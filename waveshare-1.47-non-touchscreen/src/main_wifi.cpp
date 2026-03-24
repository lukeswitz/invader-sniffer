#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <USB.h>
#include <USBMSC.h>
#include <sdmmc_cmd.h>
#include <tusb.h>
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_timer.h"

#define GFX_BL 48
#define LED_PIN 38

#define CRAB_COLS 11
#define CRAB_ROWS 8
#define CRAB_SCALE 4
#define CRAB_CX (SCREEN_W / 2)
#define CRAB_CY 115


#define USB_SKIP_MAGIC 0xDEAD5541u
RTC_NOINIT_ATTR static uint32_t g_skipUsbDetect;

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    41, 42, 40, 45
);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, 39, 0, false, 172, 320, 34, 0, 34, 0
);
static Arduino_Canvas *g_canvas = nullptr;

static constexpr int SCREEN_W = 172;
static constexpr int SCREEN_H = 320;
static constexpr int TITLE_H = 26;
static constexpr int STATUS_H = 24;
static constexpr int ANIM_Y0 = TITLE_H;
static constexpr int ANIM_Y1 = SCREEN_H - STATUS_H;

static constexpr uint32_t LINKTYPE_IEEE802_11_RADIOTAP = 127u;
static constexpr uint32_t LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR = 256u;

static volatile uint32_t g_crabShakeTime = 0;
static volatile int g_crabShakeAmount = 0;

static constexpr uint16_t COL_IDLE = 0x07E0;
static constexpr uint16_t COL_CAP = 0xF81F;
static constexpr uint16_t COL_DIM = 0x2945;
static constexpr uint16_t COL_BAR_BG = 0x0841;
static constexpr uint16_t COL_WIFI = 0xF800;
static constexpr uint16_t COL_BLE = 0x001F;

static inline uint16_t modeColor(bool isBLE) {
    return isBLE ? COL_BLE : COL_WIFI;
}

enum class CaptureMode { WIFI, BLE };
enum class DeviceMode { STOPPED, CAPTURING };

volatile CaptureMode g_captureMode = CaptureMode::WIFI;
volatile DeviceMode g_mode = DeviceMode::STOPPED;

static constexpr int BOOT_BTN = 0;

volatile uint32_t g_rawPresses = 0;
static uint32_t g_debounceMs = 0;

// --------------------- PCAP Creation

File g_pcap;
String g_capturePath = "";
volatile uint32_t g_packetCount = 0;
volatile uint32_t g_dropCount = 0;

struct __attribute__((packed)) PcapGlobalHeader {
    uint32_t magic_number = 0xa1b2c3d4;
    uint16_t version_major = 2;
    uint16_t version_minor = 4;
    int32_t thiszone = 0;
    uint32_t sigfigs = 0;
    uint32_t snaplen = 2500;
    uint32_t network = LINKTYPE_IEEE802_11_RADIOTAP;
};

struct __attribute__((packed)) PcapRecordHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};

struct __attribute__((packed)) WifiRadiotapHeader {
    uint8_t  it_version;
    uint8_t  it_pad;
    uint16_t it_len;
    uint32_t it_present;
    uint8_t  flags;
    uint8_t  pad1;
    uint16_t chan_freq;
    uint16_t chan_flags;
    int8_t   dbm_antsignal;
    uint8_t  antenna;
};

struct __attribute__((packed)) BleLlWithPhdrHeader {
    uint8_t rf_channel;
    int8_t signal_power;
    int8_t noise_power;
    uint8_t access_address_offenses;
    uint32_t ref_access_address;
    uint16_t flags;
};

struct BleSeenItem {
    uint8_t bda[6];
    uint8_t evt;
    uint8_t len;
    uint32_t hash;
    uint32_t ts_ms;
};

static BleSeenItem g_bleSeen[64] = {};
static uint8_t g_bleSeenPos = 0;

struct PacketItem {
    uint32_t ts_us;
    uint32_t incl_len;
    uint32_t orig_len;
    int8_t rssi;
    uint8_t channel;
    uint8_t *data;
};

QueueHandle_t g_pktQueue = nullptr;


// --------------------- HW Init

void toggleCapture();
void switchCaptureMode();

void ARDUINO_ISR_ATTR bootButtonISR() {
    uint32_t now = millis();
    if (now - g_debounceMs > 50) {
        g_rawPresses++;
        g_debounceMs = now;
    }
}

static uint32_t g_seenPresses = 0;
static bool g_tapPending = false;
static uint32_t g_tapTime = 0;

static void handleButton() {
    uint32_t raw = g_rawPresses;
    while (raw > g_seenPresses) {
        g_seenPresses++;
        if (!g_tapPending) {
            g_tapPending = true;
            g_tapTime = millis();
        } else {
            if (millis() - g_tapTime < 500) {
                Serial.println("[btn] double-tap -> switch mode");
                switchCaptureMode();
                g_tapPending = false;
            } else {
                toggleCapture();
                g_tapPending = true;
                g_tapTime = millis();
            }
            return;
        }
    }
    if (g_tapPending && millis() - g_tapTime > 500) {
        Serial.println("[btn] single-tap -> toggle");
        toggleCapture();
        g_tapPending = false;
    }
}

static constexpr int SD_CLK = 14;
static constexpr int SD_CMD = 15;
static constexpr int SD_D0 = 16;
static constexpr int SD_D1 = 18;
static constexpr int SD_D2 = 17;
static constexpr int SD_D3 = 21;

bool initSD() {
    if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)) {
        Serial.println("[sd] setPins failed");
        return false;
    }
    if (!SD_MMC.begin()) {
        Serial.println("[sd] begin failed");
        return false;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("[sd] no card");
        return false;
    }
    Serial.printf(
        "[sd] card: %llu MB\n",
        SD_MMC.cardSize() / (1024ULL * 1024ULL)
    );
    return true;
}

String nextCaptureName(const char *prefix) {
    for (int i = 0; i < 10000; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "/%s_%04d.pcap", prefix, i);
        if (!SD_MMC.exists(buf)) return String(buf);
    }
    return String("/") + prefix + ".pcap";
}

// --------------------- Wireless Scans

static uint32_t fnv1a32(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static uint16_t wifiChannelToFreqMHz(uint8_t ch) {
    if (ch >= 1 && ch <= 13) return (uint16_t)(2407 + ch * 5);
    if (ch == 14) return 2484u;
    return 0u;
}

static uint16_t wifiChannelFlags(uint8_t ch) {
    uint16_t flags = 0x0080u;
    uint16_t freq = wifiChannelToFreqMHz(ch);
    if (freq >= 2412u && freq <= 2484u) flags |= 0x0040u;
    return flags;
}

static uint16_t blePhdrFlags(
    bool signalValid,
    bool noiseValid,
    bool refAaValid
) {
    uint16_t flags = 0u;
    if (signalValid) flags |= 0x0001u;
    if (noiseValid) flags |= 0x0002u;
    if (refAaValid) flags |= 0x0008u;
    return flags;
}

static uint8_t bleAdvChannelForEvent(uint8_t ble_evt_type) {
    switch (ble_evt_type) {
        case ESP_BLE_EVT_CONN_ADV:
        case ESP_BLE_EVT_CONN_DIR_ADV:
        case ESP_BLE_EVT_NON_CONN_ADV:
        case ESP_BLE_EVT_DISC_ADV:
        case ESP_BLE_EVT_SCAN_RSP:
        default:
            return 37u;
    }
}

static bool bleShouldDropDuplicate(
    const uint8_t *bda,
    uint8_t evt,
    const uint8_t *payload,
    uint8_t len,
    uint32_t now_ms
) {
    if (!bda || (!payload && len != 0u)) return true;

    uint32_t h = fnv1a32(payload, len);
    for (size_t i = 0; i < (sizeof(g_bleSeen) / sizeof(g_bleSeen[0])); i++) {
        BleSeenItem &s = g_bleSeen[i];
        if (s.len != len) continue;
        if (s.evt != evt) continue;
        if (s.hash != h) continue;
        if (memcmp(s.bda, bda, 6) != 0) continue;
        if ((uint32_t)(now_ms - s.ts_ms) < 250u) return true;
    }

    BleSeenItem &slot = g_bleSeen[g_bleSeenPos++ % 64u];
    memcpy(slot.bda, bda, 6);
    slot.evt = evt;
    slot.len = len;
    slot.hash = h;
    slot.ts_ms = now_ms;
    return false;
}

bool openPcap() {
    const char *prefix = (g_captureMode == CaptureMode::BLE) ? "ble" : "wifi";
    g_capturePath = nextCaptureName(prefix);
    g_pcap = SD_MMC.open(g_capturePath, FILE_WRITE);
    if (!g_pcap) {
        Serial.println("[pcap] open failed");
        return false;
    }

    PcapGlobalHeader gh;
    gh.network = (g_captureMode == CaptureMode::BLE)
        ? LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR
        : LINKTYPE_IEEE802_11_RADIOTAP;

    size_t w = g_pcap.write((const uint8_t *)&gh, sizeof(gh));
    g_pcap.flush();
    if (w != sizeof(gh)) {
        Serial.println("[pcap] header write failed");
        g_pcap.close();
        return false;
    }

    Serial.printf("[pcap] opened %s\n", g_capturePath.c_str());
    return true;
}

void closePcap() {
    if (g_pcap) {
        g_pcap.flush();
        g_pcap.close();
        Serial.println("[pcap] closed");
    }
}

static constexpr uint8_t NUM_CHANNELS = 14;
const uint8_t g_channels[NUM_CHANNELS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
};
volatile uint8_t g_hopIndex = 0;
volatile bool g_hopRequested = false;
hw_timer_t *g_hopTimer = nullptr;

static volatile uint32_t g_chanPkts[NUM_CHANNELS] = {};
static uint8_t g_chanDwell[NUM_CHANNELS] = {
    2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1
};
static uint8_t g_dwellLeft = 2;

void lcd_reg_init() {
    bus->sendCommand(0x11);
    delay(120);
    bus->sendCommand(0x3A);
    bus->sendData(0x05);
    bus->sendCommand(0x36);
    bus->sendData(0x00);
    bus->sendCommand(0x21);
    bus->sendCommand(0x13);
    bus->sendCommand(0x29);
    delay(120);
}

// --------------------- Graphics

static const uint16_t CRAB_F0[CRAB_ROWS] = {
    0x104, 0x28A, 0x3FE, 0x6DB, 0x7FF, 0x1DC, 0x104, 0x28A,
};
static const uint16_t CRAB_F1[CRAB_ROWS] = {
    0x104, 0x088, 0x3FE, 0x6DB, 0x7FF, 0x1DC, 0x202, 0x401,
};

static USBMSC g_msc;

namespace {
class SDMMCAccessor : public fs::SDMMCFS {
  public:
    sdmmc_card_t *card() { return _card; }
};

static sdmmc_card_t *sdRawCard() {
    return reinterpret_cast<SDMMCAccessor *>(&SD_MMC)->card();
}
}

static int32_t onMscRead(
    uint32_t lba,
    uint32_t offset,
    void *buffer,
    uint32_t bufsize
) {
    (void)offset;
    sdmmc_card_t *card = sdRawCard();
    if (!card) return -1;
    return (sdmmc_read_sectors(card, buffer, lba, bufsize / 512) == ESP_OK)
        ? (int32_t)bufsize
        : -1;
}

static int32_t onMscWrite(
    uint32_t lba,
    uint32_t offset,
    uint8_t *buffer,
    uint32_t bufsize
) {
    (void)offset;
    sdmmc_card_t *card = sdRawCard();
    if (!card) return -1;
    return (sdmmc_write_sectors(card, buffer, lba, bufsize / 512) == ESP_OK)
        ? (int32_t)bufsize
        : -1;
}

static void drawMscScreen(
    const char *line1,
    const char *line2,
    uint16_t statusColor,
    const char *footer = nullptr
) {
    gfx->fillScreen(RGB565_BLACK);

    int16_t x0 = CRAB_CX - (CRAB_COLS * CRAB_SCALE) / 2;
    int16_t y0 = 55 - (CRAB_ROWS * CRAB_SCALE) / 2;
    for (int row = 0; row < CRAB_ROWS; row++) {
        uint16_t bits = CRAB_F0[row];
        for (int col = 0; col < CRAB_COLS; col++) {
            if (bits & (1u << (CRAB_COLS - 1 - col))) {
                gfx->fillRect(
                    x0 + col * CRAB_SCALE,
                    y0 + row * CRAB_SCALE,
                    CRAB_SCALE,
                    CRAB_SCALE,
                    0x07FF
                );
            }
        }
    }

    gfx->setTextSize(3);
    gfx->setTextColor(0x07FF);
    int16_t tw = (int16_t)(8 * 18);
    gfx->setCursor((SCREEN_W - tw) / 2, 90);
    gfx->print("USB MODE");

    gfx->drawFastHLine(10, 120, SCREEN_W - 20, 0x07FF);

    gfx->setTextSize(1);
    gfx->setTextColor(statusColor);
    tw = (int16_t)(strlen(line1) * 6);
    gfx->setCursor((SCREEN_W - tw) / 2, 132);
    gfx->print(line1);

    if (line2 && line2[0]) {
        gfx->setTextColor(0x8410);
        tw = (int16_t)(strlen(line2) * 6);
        gfx->setCursor((SCREEN_W - tw) / 2, 144);
        gfx->print(line2);
    }

    const char *hint = footer ? footer : "Press BOOT button to exit";
    gfx->setTextColor(COL_DIM);
    tw = (int16_t)(strlen(hint) * 6);
    gfx->setCursor((SCREEN_W - tw) / 2, SCREEN_H - 16);
    gfx->print(hint);
}

static void updateMscFooter(const char *hint, uint16_t color) {
    gfx->fillRect(0, SCREEN_H - 20, SCREEN_W, 20, RGB565_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(color);
    int16_t tw = (int16_t)(strlen(hint) * 6);
    gfx->setCursor((SCREEN_W - tw) / 2, SCREEN_H - 16);
    gfx->print(hint);
}

static void enterMscMode() {
    drawMscScreen("Initialising SD...", "", RGB565_WHITE);

    if (!initSD()) {
        drawMscScreen("SD card failed!", "Check card and reboot", RGB565_RED);
        while (true) delay(1000);
    }

    uint32_t numSectors = (uint32_t)(SD_MMC.cardSize() / 512);
    uint64_t cardMB = SD_MMC.cardSize() / (1024ULL * 1024ULL);

    g_msc.vendorID("ESP32-S3");
    g_msc.productID("SNIFF-SD");
    g_msc.productRevision("1.0");
    g_msc.mediaPresent(true);
    g_msc.onRead(onMscRead);
    g_msc.onWrite(onMscWrite);
    g_msc.begin(numSectors, 512);

    Serial.printf("[msc] %u sectors (%llu MB) ready\n", numSectors, cardMB);

    char line1[32];
    char line2[32];
    snprintf(line1, sizeof(line1), "Drive ready  (%llu MB)", cardMB);
    snprintf(line2, sizeof(line2), "Waiting for host mount...");
    drawMscScreen(line1, line2, COL_IDLE, "Press BOOT to exit to capture");

    uint32_t t0 = millis();
    while (!tud_mounted() && (millis() - t0 < 5000)) delay(50);

    if (tud_mounted()) {
        snprintf(line2, sizeof(line2), "Disk mounted by host");
        drawMscScreen(line1, line2, COL_IDLE, "Press BOOT to exit to capture");
        Serial.println("[msc] mounted by host");
    } else {
        drawMscScreen(
            line1,
            "Mount timeout - still accessible",
            0xFFE0,
            "Press BOOT to exit to capture"
        );
        Serial.println("[msc] mount timeout (drive still usable)");
    }

    uint32_t lastBootCheck = millis();
    for (;;) {
        uint32_t now = millis();
        if (now - lastBootCheck > 100) {
            lastBootCheck = now;
            if (digitalRead(BOOT_BTN) == LOW) {
                updateMscFooter("Restarting...", RGB565_WHITE);
                Serial.println("[msc] boot pressed - restarting into capture mode");
                delay(300);
                g_skipUsbDetect = USB_SKIP_MAGIC;
                ESP.restart();
            }
        }
        delay(20);
    }
}

static void drawCrab(bool frame1, bool capturing, bool isBLE) {
    const uint16_t *rows = frame1 ? CRAB_F1 : CRAB_F0;
    const uint16_t color = capturing ? COL_CAP : modeColor(isBLE);

    int16_t shake = 0;
    if (millis() < g_crabShakeTime && g_crabShakeAmount > 0) {
        shake =
            (g_crabShakeAmount * (int)(esp_random() % 3)) - g_crabShakeAmount;
    } else {
        g_crabShakeAmount = 0;
    }

    const int16_t x0 = CRAB_CX - (CRAB_COLS * CRAB_SCALE) / 2 + shake;
    const int16_t y0 = CRAB_CY - (CRAB_ROWS * CRAB_SCALE) / 2;

    for (int row = 0; row < CRAB_ROWS; row++) {
        uint16_t bits = rows[row];
        for (int col = 0; col < CRAB_COLS; col++) {
            if (bits & (1u << (CRAB_COLS - 1 - col))) {
                g_canvas->fillRect(
                    x0 + col * CRAB_SCALE,
                    y0 + row * CRAB_SCALE,
                    CRAB_SCALE,
                    CRAB_SCALE,
                    color
                );
            }
        }
    }
}

#define STAR_COUNT 80
struct Star {
    float x;
    float y;
    float speed;
    uint8_t bright;
};
static Star stars[STAR_COUNT];

static void initStars() {
    for (int i = 0; i < STAR_COUNT; i++) {
        stars[i].x = (float)(esp_random() % SCREEN_W);
        stars[i].y = (float)(esp_random() % SCREEN_H);
        int tier = (int)(esp_random() % 3);
        stars[i].speed =
            0.4f + tier * 0.8f + (float)(esp_random() % 10) * 0.05f;
        stars[i].bright = (uint8_t)(80 + tier * 55 + (esp_random() % 40));
    }
}

static void updateStars(float mult) {
    for (int i = 0; i < STAR_COUNT; i++) {
        stars[i].y += stars[i].speed * mult;
        if (stars[i].y >= (float)SCREEN_H) {
            stars[i].y = (float)ANIM_Y0;
            stars[i].x = (float)(esp_random() % SCREEN_W);
        }
    }
}

static void drawStars(bool capturing) {
    for (int i = 0; i < STAR_COUNT; i++) {
        uint8_t b = stars[i].bright;
        uint16_t col = capturing
            ? g_canvas->color565(b, b >> 2, b)
            : g_canvas->color565(b, b, b);
        g_canvas->drawPixel((int16_t)stars[i].x, (int16_t)stars[i].y, col);
    }
}

static void drawTitleBar(bool capturing, bool isBLE) {
    g_canvas->fillRect(0, 0, SCREEN_W, TITLE_H, RGB565_BLACK);

    uint16_t tcolor = capturing ? COL_CAP : modeColor(isBLE);
    if (capturing && (millis() / 400) % 2) tcolor = 0xFC1F;

    const char *title;
    if (capturing) {
        title = isBLE ? "BLE CAPTURE" : "WIFI CAPTURE";
    } else {
        title = "INSERT TOKEN";
    }

    static constexpr uint8_t textSize = 2;
    int16_t charW = textSize * 6;
    int16_t charH = textSize * 8;
    int16_t tw = (int16_t)(strlen(title) * charW);
    int16_t tx = (SCREEN_W - tw) / 2;
    int16_t ty = (TITLE_H - charH) / 2;

    g_canvas->setTextSize(textSize);
    g_canvas->setTextColor(tcolor);
    g_canvas->setCursor(tx, ty);
    g_canvas->print(title);
    g_canvas->drawFastHLine(0, TITLE_H - 1, SCREEN_W, tcolor);
}

static void drawStatusBar(bool capturing, bool isBLE) {
    g_canvas->fillRect(0, ANIM_Y1, SCREEN_W, STATUS_H, COL_BAR_BG);
    g_canvas->drawFastHLine(
        0,
        ANIM_Y1,
        SCREEN_W,
        capturing ? COL_CAP : modeColor(isBLE)
    );

    g_canvas->setTextSize(1);

    if (capturing) {
        g_canvas->setTextColor(RGB565_WHITE);
        g_canvas->setCursor(2, ANIM_Y1 + 4);
        char buf[32];
        const char *p = g_capturePath.c_str();
        const char *fname = strrchr(p, '/');
        fname = fname ? fname + 1 : p;
        snprintf(
            buf,
            sizeof(buf),
            "%-10s P:%-6lu",
            fname,
            (unsigned long)g_packetCount
        );
        g_canvas->print(buf);

        g_canvas->setTextColor(0xC618);
        g_canvas->setCursor(2, ANIM_Y1 + 13);
        if (isBLE) {
            snprintf(buf, sizeof(buf), "DRP:%-5lu", (unsigned long)g_dropCount);
        } else {
            snprintf(
                buf,
                sizeof(buf),
                "CH:%-2u  DRP:%-5lu",
                g_channels[g_hopIndex],
                (unsigned long)g_dropCount
            );
        }
        g_canvas->print(buf);
    } else {
        g_canvas->setTextColor(modeColor(isBLE));
        g_canvas->setCursor(2, ANIM_Y1 + 4);
        g_canvas->print(isBLE ? "BLE READY" : "WIFI READY");

        if (g_capturePath.length()) {
            g_canvas->setTextColor(0x8410);
            g_canvas->setCursor(2, ANIM_Y1 + 13);
            char buf[24];
            const char *p = g_capturePath.c_str();
            const char *fname = strrchr(p, '/');
            fname = fname ? fname + 1 : p;
            snprintf(buf, sizeof(buf), "last: %s", fname);
            g_canvas->print(buf);
        }
    }
}

static void drawIdleHint(bool isBLE) {
    if ((millis() / 900) % 2 == 0) {
        const char *hint = isBLE ? "START BLE PCAP" : "START WIFI PCAP";
        int16_t tw = (int16_t)(strlen(hint) * 6);
        g_canvas->setTextSize(1);
        g_canvas->setTextColor(modeColor(isBLE));
        g_canvas->setCursor((SCREEN_W - tw) / 2, CRAB_CY + 28);
        g_canvas->print(hint);
    }
}

static void drawChannelBadge() {
    char buf[8];
    snprintf(buf, sizeof(buf), "CH:%u", g_channels[g_hopIndex]);
    int16_t tw = (int16_t)(strlen(buf) * 6);
    g_canvas->setTextSize(1);
    g_canvas->setTextColor(COL_CAP);
    g_canvas->setCursor(SCREEN_W - tw - 2, ANIM_Y0 + 3);
    g_canvas->print(buf);
}

static void drawChannelMap() {
    static constexpr int BAR_W = 10;
    static constexpr int BAR_GAP = 2;
    static constexpr int MAX_BAR_H = 18;
    static constexpr int MAP_Y = ANIM_Y1 - MAX_BAR_H - 4;

    int totalW = NUM_CHANNELS * (BAR_W + BAR_GAP) - BAR_GAP;
    int startX = (SCREEN_W - totalW) / 2;

    g_canvas->fillRect(startX, MAP_Y, totalW, MAX_BAR_H + 1, RGB565_BLACK);

    for (int i = 0; i < NUM_CHANNELS; i++) {
        uint8_t dwell = g_chanDwell[i];
        bool isCurrent = ((uint8_t)i == g_hopIndex);
        bool isPrimary = (
            g_channels[i] == 1 || g_channels[i] == 6 || g_channels[i] == 11
        );

        int barH = max(2, (int)dwell * MAX_BAR_H / 8);
        int x = startX + i * (BAR_W + BAR_GAP);
        int y = MAP_Y + (MAX_BAR_H - barH);

        uint16_t col;
        if (isCurrent) {
            col = RGB565_WHITE;
        } else if (dwell > 2) {
            col = COL_CAP;
        } else if (isPrimary) {
            col = COL_IDLE;
        } else {
            col = COL_DIM;
        }

        g_canvas->fillRect(x, y, BAR_W, barH, col);
    }
}

static volatile uint32_t g_lastPktFlash = 0;

struct Projectile {
    float x;
    float y;
    float vx;
    float vy;
    uint8_t life;
    uint16_t color;
};

static constexpr int MAX_PROJECTILES = 64;
static Projectile g_projectiles[MAX_PROJECTILES];
static int g_projectileCount = 0;

static uint16_t channelColor(uint8_t ch) {
    if (ch >= 1 && ch <= 6) return 0x07E0;
    if (ch >= 7 && ch <= 11) return 0xF800;
    return 0x001F;
}

static int channelToX(uint8_t ch) {
    if (ch == 37) return SCREEN_W / 2;
    int idx = 0;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (g_channels[i] == ch) {
            idx = i;
            break;
        }
    }
    int totalW = NUM_CHANNELS * 12 - 2;
    int startX = (SCREEN_W - totalW) / 2;
    return startX + idx * 12 + 6;
}

static void spawnProjectile(uint8_t ch) {
    if (g_projectileCount >= MAX_PROJECTILES) return;
    Projectile &p = g_projectiles[g_projectileCount++];
    p.x = (float)channelToX(ch);
    p.y = (float)(SCREEN_H - STATUS_H - 4);
    p.vx = 0.0f;
    p.vy = -3.5f;
    p.life = 255;
    p.color = channelColor(ch);
}

static void updateProjectiles() {
    for (int i = 0; i < g_projectileCount;) {
        Projectile &p = g_projectiles[i];
        p.y += p.vy;
        p.life -= 8;

        bool hitCrab = false;
        if (
            p.y >= CRAB_CY - 20 && p.y <= CRAB_CY + 20 &&
            p.x >= CRAB_CX - 20 && p.x <= CRAB_CX + 20
        ) {
            hitCrab = true;
            g_crabShakeTime = millis() + 200;
            g_crabShakeAmount = 4;
        }

        if (p.y < ANIM_Y0 || p.life == 0 || hitCrab) {
            g_projectiles[i] = g_projectiles[--g_projectileCount];
        } else {
            i++;
        }
    }
}

static void drawProjectiles() {
   for (int i = 0; i < g_projectileCount; i++) {
       const Projectile &p = g_projectiles[i];
       
       // Fade out
       float fadeZone = (SCREEN_H - STATUS_H - 4 - ANIM_Y0) * 0.4f;
       float progress = (p.y - ANIM_Y0) / fadeZone;
       if (progress < 0) progress = 0;
       if (progress > 1) progress = 1;
       
       progress = progress * progress;
       
       uint8_t alpha = (uint8_t)(255 * progress);
       
       uint16_t col = p.color;
       uint8_t r = ((col >> 11) & 0x1F) * alpha >> 8;
       uint8_t g = ((col >> 5) & 0x3F) * alpha >> 8;
       uint8_t b = (col & 0x1F) * alpha >> 8;
       uint16_t faded = (r << 11) | (g << 5) | b;
       g_canvas->fillRect((int16_t)p.x - 1, (int16_t)p.y - 2, 3, 4, faded);
   }
}

static void drawPacketFlash() {
    if (millis() - g_lastPktFlash > 80) return;
    static const int8_t dx[] = {-8, 8, 0, 0};
    static const int8_t dy[] = {0, 0, -8, 8};
    for (int i = 0; i < 4; i++) {
        g_canvas->fillRect(
            CRAB_CX + dx[i] - 1,
            CRAB_CY + dy[i] - 1,
            3,
            3,
            0xFFE0
        );
    }
}

// --------------------- NeoPixel 

static void ledUpdate() {
    bool capturing = (g_mode == DeviceMode::CAPTURING);
    bool isBLE = (g_captureMode == CaptureMode::BLE);
    uint32_t now = millis();

    if (!capturing) {
        float phase = (float)(now % 2000) / 2000.0f;
        uint8_t b = (uint8_t)(sinf(phase * 6.2832f) * 40.0f + 50.0f);
        if (isBLE) {
            neopixelWrite(LED_PIN, 0, 0, b);
        } else {
            neopixelWrite(LED_PIN, 0, b, 0);
        }
    } else if (isBLE) {
        if (now - g_lastPktFlash < 120) {
            uint8_t r = (uint8_t)(esp_random() % 256);
            uint8_t g = (uint8_t)(esp_random() % 256);
            uint8_t b = (uint8_t)(esp_random() % 256);
            neopixelWrite(LED_PIN, r, g, b);
        } else {
            neopixelWrite(LED_PIN, 0, 0, 0);
        }
    } else {
        if (now - g_lastPktFlash < 80) {
            neopixelWrite(LED_PIN, 255, 220, 120);
            return;
        }
        static uint32_t lastFlicker = 0;
        static uint8_t flickR = 220;
        static uint8_t flickG = 80;
        if (now - lastFlicker > 60) {
            lastFlicker = now;
            flickR = (uint8_t)(180 + (esp_random() % 75));
            flickG = (uint8_t)(40 + (esp_random() % 90));
        }
        neopixelWrite(LED_PIN, flickR, flickG, 0);
    }
}

static int g_crabFrame = 0;
static uint32_t g_lastFlip = 0;

static void renderFrame() {
    bool capturing = (g_mode == DeviceMode::CAPTURING);
    bool isBLE = (g_captureMode == CaptureMode::BLE);
    uint32_t now = millis();

    if (now - g_lastFlip >= 400) {
        g_crabFrame = 1 - g_crabFrame;
        g_lastFlip = now;
    }

    updateStars(capturing ? 2.5f : 1.0f);

    g_canvas->fillScreen(RGB565_BLACK);
    drawStars(capturing);
    drawCrab(g_crabFrame, capturing, isBLE);

    if (!capturing) {
        drawIdleHint(isBLE);
    } else if (!isBLE) {
        drawChannelBadge();
        drawChannelMap();
    }

    drawPacketFlash();
    updateProjectiles();
    drawProjectiles();
    drawTitleBar(capturing, isBLE);
    drawStatusBar(capturing, isBLE);
    g_canvas->flush();
    ledUpdate();
}

// --------------------- Packet Sniffing

void ARDUINO_ISR_ATTR hopISR() {
    g_hopRequested = true;
}

void packetWriterTask(void *param) {
    (void)param;
    PacketItem item;

    while (true) {
        if (xQueueReceive(g_pktQueue, &item, portMAX_DELAY) == pdTRUE) {
            if (g_mode == DeviceMode::CAPTURING && g_pcap) {
                PcapRecordHeader rh{};
                rh.ts_sec = item.ts_us / 1000000UL;
                rh.ts_usec = item.ts_us % 1000000UL;
                rh.incl_len = item.incl_len;
                rh.orig_len = item.orig_len;
                g_pcap.write((const uint8_t *)&rh, sizeof(rh));
                g_pcap.write(item.data, item.incl_len);
                g_packetCount = g_packetCount + 1;
                g_lastPktFlash = millis();
                if ((g_packetCount % 32u) == 0u) g_pcap.flush();
            }

            free(item.data);
            item.data = nullptr;
        }
    }
}

void wifiSniffer(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (g_mode != DeviceMode::CAPTURING || g_captureMode != CaptureMode::WIFI) {
        return;
    }
    if (type == WIFI_PKT_MISC) return;

    const wifi_promiscuous_pkt_t *ppkt =
        static_cast<const wifi_promiscuous_pkt_t *>(buf);
    if (!ppkt) return;

    const wifi_pkt_rx_ctrl_t &rx = ppkt->rx_ctrl;
    uint32_t payload_len         = rx.sig_len;
    if (payload_len == 0u) return;

    g_chanPkts[g_hopIndex]++;
    spawnProjectile(rx.channel);

    WifiRadiotapHeader rt{};
    rt.it_version    = 0u;
    rt.it_pad        = 0u;
    rt.it_len        = sizeof(WifiRadiotapHeader);
    rt.it_present    = 0x0000082Au;
    rt.flags         = 0x00u;
    rt.pad1          = 0u;
    rt.chan_freq     = wifiChannelToFreqMHz(rx.channel);
    rt.chan_flags    = wifiChannelFlags(rx.channel);
    rt.dbm_antsignal = rx.rssi;
    rt.antenna       = 0u;

    uint32_t full_orig_len = (uint32_t)sizeof(WifiRadiotapHeader) + payload_len;
    uint32_t frame_len     = min<uint32_t>(full_orig_len, 2500u);
    uint32_t body_len      = frame_len - sizeof(WifiRadiotapHeader);

    uint8_t *pkt_data = (uint8_t *)heap_caps_malloc(
        frame_len,
        MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM
    );
    if (!pkt_data) pkt_data = (uint8_t *)malloc(frame_len);
    if (!pkt_data) {
        g_dropCount = g_dropCount + 1;
        return;
    }

    memcpy(pkt_data, &rt, sizeof(rt));
    memcpy(pkt_data + sizeof(rt), ppkt->payload, body_len);

    PacketItem item{};
    item.ts_us    = (uint32_t)esp_timer_get_time();
    item.rssi     = rx.rssi;
    item.channel  = rx.channel;
    item.orig_len = full_orig_len;
    item.incl_len = frame_len;
    item.data     = pkt_data;

    if (xQueueSend(g_pktQueue, &item, 0) != pdTRUE) {
        free(pkt_data);
        g_dropCount = g_dropCount + 1;
    }
}

static bool g_bleReady = false;
static uint8_t g_devTable[128][6];
static uint8_t g_devTableLen = 0;
static SemaphoreHandle_t g_devMutex = nullptr;

static bool trackDevice(const uint8_t *bda) {
    if (!g_devMutex) return false;
    if (xSemaphoreTake(g_devMutex, 0) != pdTRUE) return false;

    bool isnew = true;
    for (uint8_t i = 0; i < g_devTableLen; i++) {
        if (memcmp(g_devTable[i], bda, 6) == 0) {
            isnew = false;
            break;
        }
    }

    if (isnew && g_devTableLen < 128) {
        memcpy(g_devTable[g_devTableLen++], bda, 6);
    }

    xSemaphoreGive(g_devMutex);
    return isnew;
}

static void bleScanResultHandler(
    esp_ble_gap_cb_param_t::ble_scan_result_evt_param *rst
);
static void bleGapCallback(
    esp_gap_ble_cb_event_t event,
    esp_ble_gap_cb_param_t *param
);

bool initBLE() {
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[ble] mem_release warn: %d\n", err);
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        Serial.printf("[ble] controller_init failed: %d\n", err);
        return false;
    }
    if ((err = esp_bt_controller_enable(ESP_BT_MODE_BLE)) != ESP_OK) {
        Serial.printf("[ble] controller_enable failed: %d\n", err);
        return false;
    }
    if ((err = esp_bluedroid_init()) != ESP_OK) {
        Serial.printf("[ble] bluedroid_init failed: %d\n", err);
        return false;
    }
    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        Serial.printf("[ble] bluedroid_enable failed: %d\n", err);
        return false;
    }
    if ((err = esp_ble_gap_register_callback(bleGapCallback)) != ESP_OK) {
        Serial.printf("[ble] register_callback failed: %d\n", err);
        return false;
    }

    Serial.println("[ble] Bluedroid initialised");
    g_bleReady = true;
    return true;
}

static uint32_t ble_crc24(
    const uint8_t *data,
    size_t len,
    uint32_t init = 0x555555u
) {
    uint32_t crc = init;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t feedback = ((b >> j) & 1u) ^ (crc & 1u);
            crc >>= 1;
            if (feedback) crc ^= 0xDA6000u;
        }
    }
    return crc & 0xFFFFFFu;
}

static void enqueueBleAdvFrame(
    uint8_t rf_channel,
    int8_t rssi,
    uint8_t ble_addr_type,
    const uint8_t *bda,
    uint8_t pdu_type,
    const uint8_t *payload,
    uint8_t payload_len
) {
    if (!bda) return;
    if (payload_len > 31u) payload_len = 31u;
    if (payload_len > 0u && !payload) return;

    uint8_t tx_add = (ble_addr_type != BLE_ADDR_TYPE_PUBLIC) ? 1u : 0u;
    uint8_t pdu_hdr0 = (uint8_t)((pdu_type & 0x0Fu) | (tx_add << 6u));
    uint8_t pdu_hdr1 = (uint8_t)(6u + payload_len);

    BleLlWithPhdrHeader phdr{};
    phdr.rf_channel = rf_channel;
    phdr.signal_power = rssi;
    phdr.noise_power = -128;
    phdr.access_address_offenses = 0u;
    phdr.ref_access_address = 0x8E89BED6u;
    phdr.flags = blePhdrFlags(true, false, true);

    uint16_t ll_len = (uint16_t)(4u + 2u + 6u + payload_len + 3u);
    uint16_t full_len = (uint16_t)(sizeof(BleLlWithPhdrHeader) + ll_len);

    uint8_t *buf = (uint8_t *)heap_caps_malloc(
        full_len,
        MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM
    );
    if (!buf) buf = (uint8_t *)malloc(full_len);
    if (!buf) {
        g_dropCount = g_dropCount + 1;
        return;
    }

    uint8_t *p = buf;
    memcpy(p, &phdr, sizeof(phdr));
    p += sizeof(phdr);

    *p++ = 0xD6u;
    *p++ = 0xBEu;
    *p++ = 0x89u;
    *p++ = 0x8Eu;
    *p++ = pdu_hdr0;
    *p++ = pdu_hdr1;
    memcpy(p, bda, 6u);
    p += 6u;

    if (payload_len > 0u) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }

    uint32_t crc = ble_crc24(
        buf + sizeof(BleLlWithPhdrHeader) + 4u,
        (size_t)(2u + 6u + payload_len)
    );
    *p++ = (uint8_t)(crc);
    *p++ = (uint8_t)(crc >> 8u);
    *p++ = (uint8_t)(crc >> 16u);

    PacketItem item{};
    item.ts_us = (uint32_t)esp_timer_get_time();
    item.rssi = rssi;
    item.channel = rf_channel;
    item.orig_len = full_len;
    item.incl_len = full_len;
    item.data = buf;

    if (xQueueSend(g_pktQueue, &item, 0) != pdTRUE) {
        free(buf);
        g_dropCount = g_dropCount + 1;
    } else {
        g_lastPktFlash = millis();
    }
}

static void bleScanResultHandler(
    esp_ble_gap_cb_param_t::ble_scan_result_evt_param *rst
) {
    if (!rst) return;
    if (g_mode != DeviceMode::CAPTURING || g_captureMode != CaptureMode::BLE) {
        return;
    }
    if (rst->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

    uint8_t rf_channel = bleAdvChannelForEvent(rst->ble_evt_type);
    uint32_t now_ms = millis();

    if (rst->ble_evt_type == ESP_BLE_EVT_SCAN_RSP) {
        const uint8_t *scan_rsp = rst->ble_adv;
        uint8_t scan_rsp_len = rst->adv_data_len;
        if (scan_rsp_len > 31u) scan_rsp_len = 31u;

        if (scan_rsp && scan_rsp_len > 0u) {
            if (!bleShouldDropDuplicate(
                    rst->bda,
                    rst->ble_evt_type,
                    scan_rsp,
                    scan_rsp_len,
                    now_ms
                )) {
                enqueueBleAdvFrame(
                    rf_channel,
                    (int8_t)rst->rssi,
                    rst->ble_addr_type,
                    rst->bda,
                    0x04u,
                    scan_rsp,
                    scan_rsp_len
                );
            }
        }
        return;
    }

    trackDevice(rst->bda);
    spawnProjectile(rf_channel);

    uint8_t pdu_type;
    switch (rst->ble_evt_type) {
        case ESP_BLE_EVT_CONN_ADV:
            pdu_type = 0x00u;
            break;
        case ESP_BLE_EVT_CONN_DIR_ADV:
            pdu_type = 0x01u;
            break;
        case ESP_BLE_EVT_NON_CONN_ADV:
            pdu_type = 0x02u;
            break;
        case ESP_BLE_EVT_DISC_ADV:
            pdu_type = 0x06u;
            break;
        default:
            pdu_type = 0x00u;
            break;
    }

    uint8_t adv_len = rst->adv_data_len;
    if (adv_len > 31u) adv_len = 31u;

    if (rst->ble_adv && adv_len > 0u) {
        if (!bleShouldDropDuplicate(
                rst->bda,
                rst->ble_evt_type,
                rst->ble_adv,
                adv_len,
                now_ms
            )) {
            enqueueBleAdvFrame(
                rf_channel,
                (int8_t)rst->rssi,
                rst->ble_addr_type,
                rst->bda,
                pdu_type,
                rst->ble_adv,
                adv_len
            );
        }
    }

    uint8_t scan_rsp_len = rst->scan_rsp_len;
    if (scan_rsp_len > 31u) scan_rsp_len = 31u;

    if (rst->ble_adv && scan_rsp_len > 0u) {
        const uint8_t *scan_rsp = rst->ble_adv + rst->adv_data_len;
        if (!bleShouldDropDuplicate(
                rst->bda,
                ESP_BLE_EVT_SCAN_RSP,
                scan_rsp,
                scan_rsp_len,
                now_ms
            )) {
            enqueueBleAdvFrame(
                rf_channel,
                (int8_t)rst->rssi,
                rst->ble_addr_type,
                rst->bda,
                0x04u,
                scan_rsp,
                scan_rsp_len
            );
        }
    }
}

static void bleGapCallback(
    esp_gap_ble_cb_event_t event,
    esp_ble_gap_cb_param_t *param
) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (
                g_mode == DeviceMode::CAPTURING &&
                g_captureMode == CaptureMode::BLE
            ) {
                esp_err_t e = esp_ble_gap_start_scanning(0);
                Serial.printf("[ble] start_scanning -> %d\n", e);
            }
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            Serial.printf(
                "[ble] scan started, status=%d\n",
                param->scan_start_cmpl.status
            );
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            Serial.printf(
                "[ble] scan stopped, status=%d\n",
                param->scan_stop_cmpl.status
            );
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            bleScanResultHandler(&param->scan_rst);
            break;

        default:
            break;
    }
}

bool startCapture() {
    if (!g_pcap && !openPcap()) return false;

    g_packetCount = 0;
    g_dropCount = 0;
    g_hopIndex = 0;
    memset(g_bleSeen, 0, sizeof(g_bleSeen));
    g_bleSeenPos = 0;

    if (g_captureMode == CaptureMode::BLE) {
        if (!g_bleReady) {
            Serial.println("[ble] not ready");
            closePcap();
            return false;
        }

        if (g_devMutex &&
            xSemaphoreTake(g_devMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_devTableLen = 0;
            xSemaphoreGive(g_devMutex);
        }

        g_mode = DeviceMode::CAPTURING;

        static const esp_ble_scan_params_t scan_params = {
            .scan_type = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval = 0x00A0,
            .scan_window = 0x00A0,
            .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
        };

        esp_err_t e = esp_ble_gap_set_scan_params(
            (esp_ble_scan_params_t *)&scan_params
        );
        if (e != ESP_OK) {
            Serial.printf("[ble] set_scan_params failed: %d\n", e);
            closePcap();
            g_mode = DeviceMode::STOPPED;
            return false;
        }
    } else {
        g_dwellLeft = g_chanDwell[0];
        memset((void *)g_chanPkts, 0, sizeof(g_chanPkts));

        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_MODE_STA);
        delay(100);

        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        delay(20);

        esp_wifi_set_channel(g_channels[g_hopIndex], WIFI_SECOND_CHAN_NONE);

        wifi_promiscuous_filter_t filt{};
        filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                           WIFI_PROMIS_FILTER_MASK_CTRL |
                           WIFI_PROMIS_FILTER_MASK_DATA;

        esp_wifi_set_promiscuous_filter(&filt);
        esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
        esp_wifi_set_promiscuous(true);
        timerStart(g_hopTimer);

        g_mode = DeviceMode::CAPTURING;
    }

    Serial.printf("[capture] started: %s\n", g_capturePath.c_str());
    return true;
}

void stopCapture() {
    if (g_captureMode == CaptureMode::WIFI) {
        timerStop(g_hopTimer);
        esp_wifi_set_promiscuous(false);
    } else {
        esp_ble_gap_stop_scanning();
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    closePcap();
    g_mode = DeviceMode::STOPPED;
    Serial.println("[capture] stopped");
}

void toggleCapture() {
    if (g_mode == DeviceMode::CAPTURING) {
        stopCapture();
    } else {
        startCapture();
    }
}

void switchCaptureMode() {
    if (g_mode == DeviceMode::CAPTURING) stopCapture();
    g_captureMode = (g_captureMode == CaptureMode::WIFI)
        ? CaptureMode::BLE
        : CaptureMode::WIFI;
    Serial.printf(
        "[mode] switched to %s\n",
        g_captureMode == CaptureMode::BLE ? "BLE" : "WIFI"
    );
}

// --------------------- Main Functions

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n[boot] HYBRID SNIFFER starting");

    pinMode(LED_PIN, OUTPUT);
    neopixelWrite(LED_PIN, 0, 0, 0);

    pinMode(BOOT_BTN, INPUT_PULLUP);
    attachInterrupt(BOOT_BTN, bootButtonISR, FALLING);

    if (g_skipUsbDetect == USB_SKIP_MAGIC) {
        g_skipUsbDetect = 0;
        Serial.println("[boot] USB detection skipped (boot exit)");
    } else {
        bool hostFound = false;
        uint32_t t0 = millis();
        while (millis() - t0 < 500) {
            if (tud_connected()) {
                hostFound = true;
                break;
            }
            delay(10);
        }

        if (hostFound) {
            Serial.println("[usb] host detected - entering MSC mode");
            if (!gfx->begin()) Serial.println("[gfx] begin failed");
            lcd_reg_init();
            gfx->setRotation(0);
            gfx->fillScreen(RGB565_BLACK);
            pinMode(GFX_BL, OUTPUT);
            digitalWrite(GFX_BL, HIGH);
            enterMscMode();
        }
    }

    Serial.println("[boot] no USB host - capture mode");

    if (!gfx->begin()) Serial.println("[gfx] begin failed");
    lcd_reg_init();
    gfx->setRotation(0);
    gfx->fillScreen(RGB565_BLACK);
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);

    gfx->setTextSize(2);
    gfx->setTextColor(COL_IDLE);
    gfx->setCursor(24, 148);
    gfx->print("BOOTING...");

    g_canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx, 0, 0, 0);
    if (!g_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
        Serial.println("[canvas] begin failed - halting");
        while (true) delay(1000);
    }
    Serial.printf("[canvas] %dx%d buffer ready\n", SCREEN_W, SCREEN_H);

    if (!initSD()) {
        gfx->fillScreen(RGB565_BLACK);
        gfx->setTextSize(2);
        gfx->setTextColor(RGB565_RED);
        gfx->setCursor(10, 148);
        gfx->print("SD FAILED");
        while (true) delay(1000);
    }

    g_pktQueue = xQueueCreate(128, sizeof(PacketItem));
    if (!g_pktQueue) {
        gfx->fillScreen(RGB565_BLACK);
        gfx->setTextSize(2);
        gfx->setTextColor(RGB565_RED);
        gfx->setCursor(10, 148);
        gfx->print("MEM FAILED");
        while (true) delay(1000);
    }

    g_devMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(
        packetWriterTask,
        "pktWriter",
        8192,
        nullptr,
        1,
        nullptr,
        1
    );

    g_hopTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(g_hopTimer, &hopISR, true);
    timerAlarmWrite(g_hopTimer, 250000, true);
    timerAlarmEnable(g_hopTimer);

    WiFi.mode(WIFI_MODE_NULL);

    if (!initBLE()) {
        gfx->fillScreen(RGB565_BLACK);
        gfx->setTextSize(2);
        gfx->setTextColor(RGB565_RED);
        gfx->setCursor(10, 148);
        gfx->print("BLE FAILED");
        while (true) delay(1000);
    }

    initStars();

    Serial.println(
        "[boot] ready - tap=toggle capture, double-tap=switch WiFi/BLE"
    );
}

void loop() {
    static uint32_t lastAlive = 0;
    static uint32_t lastFrame = 0;

    while (Serial.available()) {
        char c = Serial.read();
        if (c == 's' || c == 'S') toggleCapture();
        if (c == 'm' || c == 'M') switchCaptureMode();
    }

    handleButton();

    if (
        g_mode == DeviceMode::CAPTURING &&
        g_captureMode == CaptureMode::WIFI &&
        g_hopRequested
    ) {
        g_hopRequested = false;

        if (g_dwellLeft > 1) {
            g_dwellLeft--;
        } else {
            uint8_t idx = g_hopIndex;
            uint32_t pkts = g_chanPkts[idx];
            bool isPrimary = (
                g_channels[idx] == 1 ||
                g_channels[idx] == 6 ||
                g_channels[idx] == 11
            );
            uint8_t base = isPrimary ? 2 : 1;
            uint8_t bonus =
                (uint8_t)min((pkts + 2) / 3, (uint32_t)6);
            g_chanDwell[idx] = base + bonus;
            g_chanPkts[idx] = pkts >> 1;

            g_hopIndex = (g_hopIndex + 1) % NUM_CHANNELS;
            g_dwellLeft = g_chanDwell[g_hopIndex];
            esp_wifi_set_channel(
                g_channels[g_hopIndex],
                WIFI_SECOND_CHAN_NONE
            );
        }
    }

    uint32_t now = millis();
    if (now - lastFrame >= 33) {
        lastFrame = now;
        renderFrame();
    } else {
        delay(5);
    }

    if (now - lastAlive >= 2000) {
        lastAlive = now;
        Serial.printf(
            "[alive] %s %s pkts=%lu drops=%lu ch=%u\n",
            g_captureMode == CaptureMode::BLE ? "BLE" : "WIFI",
            g_mode == DeviceMode::CAPTURING ? "CAP" : "STOP",
            (unsigned long)g_packetCount,
            (unsigned long)g_dropCount,
            g_channels[g_hopIndex]
        );
    }
}