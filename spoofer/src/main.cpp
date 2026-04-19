//
// BLE OUI Spoofer — ESP32-C3
// Tests the sniffer by cycling through target OUIs in two modes.
//
// BOOT button  — force switch to FLOCK (Evil Bird) mode immediately
// Auto-switches to FLOCK after 10 s regardless of current mode.
// Serial commands (115200):
//   n / N       — skip to next OUI immediately
//   m / M       — force switch to FLOCK (same as BOOT)
//   0 .. 9      — jump directly to that entry index (max per mode)
//

#include <Arduino.h>
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"

// ── Target OUI table ────────────────────────────────────────────────────────

struct Entry {
    const char* label;
    uint8_t     oui[3];
};

static const Entry FLOCK_ENTRIES[] = {
    // Flock Safety
    { "FLOCK  B4:1E:52", {0xB4, 0x1E, 0x52} },
    { "FLOCK  58:8E:81", {0x58, 0x8E, 0x81} },
    { "FLOCK  EC:1B:BD", {0xEC, 0x1B, 0xBD} },
    { "FLOCK  90:35:EA", {0x90, 0x35, 0xEA} },
    { "FLOCK  04:0D:84", {0x04, 0x0D, 0x84} },
    { "FLOCK  F0:82:C0", {0xF0, 0x82, 0xC0} },
    { "FLOCK  1C:34:F1", {0x1C, 0x34, 0xF1} },
    { "FLOCK  38:5B:44", {0x38, 0x5B, 0x44} },
    { "FLOCK  94:34:69", {0x94, 0x34, 0x69} },
    { "FLOCK  B4:E3:F9", {0xB4, 0xE3, 0xF9} },
    { "FLOCK  70:C9:4E", {0x70, 0xC9, 0x4E} },
    { "FLOCK  3C:91:80", {0x3C, 0x91, 0x80} },
    { "FLOCK  D8:F3:BC", {0xD8, 0xF3, 0xBC} },
    { "FLOCK  80:30:49", {0x80, 0x30, 0x49} },
    { "FLOCK  14:5A:FC", {0x14, 0x5A, 0xFC} },
    { "FLOCK  74:4C:A1", {0x74, 0x4C, 0xA1} },
    { "FLOCK  08:3A:88", {0x08, 0x3A, 0x88} },
    { "FLOCK  9C:2F:9D", {0x9C, 0x2F, 0x9D} },
    { "FLOCK  94:08:53", {0x94, 0x08, 0x53} },
    { "FLOCK  E4:AA:EA", {0xE4, 0xAA, 0xEA} },
    // Flock contract manufacturers
    { "FLOCK  F4:6A:DD", {0xF4, 0x6A, 0xDD} },
    { "FLOCK  F8:A2:D6", {0xF8, 0xA2, 0xD6} },
    { "FLOCK  E0:0A:F6", {0xE0, 0x0A, 0xF6} },
    { "FLOCK  00:F4:8D", {0x00, 0xF4, 0x8D} },
    { "FLOCK  D0:39:57", {0xD0, 0x39, 0x57} },
    { "FLOCK  E8:D0:FC", {0xE8, 0xD0, 0xFC} },
    // SoundThinking (ShotSpotter)
    { "SHOT   D4:11:D6", {0xD4, 0x11, 0xD6} },
    // Neology
    { "NEOLGY 00:17:3D", {0x00, 0x17, 0x3D} },
    // Axon
    { "AXON   00:25:DF", {0x00, 0x25, 0xDF} },
    // GENETEC
    { "GENTEC 00:0A:B1", {0x00, 0x0A, 0xB1} },
    { "GENTEC 00:50:C2", {0x00, 0x50, 0xC2} },
    { "GENTEC 00:BF:15", {0x00, 0xBF, 0x15} },
    // Leonardo UK Ltd
    { "LEO UK 00:80:E7", {0x00, 0x80, 0xE7} },
};
static constexpr int FLOCK_COUNT = (int)(sizeof(FLOCK_ENTRIES) / sizeof(FLOCK_ENTRIES[0]));

static const Entry META_ENTRIES[] = {
    { "META   7C:2A:9E", {0x7C, 0x2A, 0x9E} },
    { "META   CC:66:0A", {0xCC, 0x66, 0x0A} },
    { "META   F4:03:43", {0xF4, 0x03, 0x43} },
    { "META   5C:E9:1E", {0x5C, 0xE9, 0x1E} },
};
static constexpr int META_COUNT = (int)(sizeof(META_ENTRIES) / sizeof(META_ENTRIES[0]));

enum class SpooferMode { META, FLOCK };

// ── Config ───────────────────────────────────────────────────────────────────

// How long to advertise each OUI before moving to the next (ms)
static constexpr uint32_t DWELL_MS   = 3000;
static constexpr int      BOOT_BTN   = 9;

// ── State ────────────────────────────────────────────────────────────────────

static SpooferMode g_mode          = SpooferMode::META;
static int         g_current       = 0;
static bool        g_bleReady      = false;
static uint32_t    g_switchAt      = 0;
static bool        g_pending       = false;  // waiting for set_rand_addr callback
static bool        g_btnLastState  = HIGH;
static uint32_t    g_btnLastMs     = 0;
static uint32_t    g_lastStatusMs  = 0;
static uint32_t    g_flockAt       = 0;   // auto-switch to FLOCK after 10 s
static uint8_t     g_lastAddr[6]   = {};     // addr set by most recent beginEntry

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,   // 20 ms
    .adv_int_max       = 0x40,   // 40 ms
    .adv_type          = ADV_TYPE_NONCONN_IND,
    .own_addr_type     = BLE_ADDR_TYPE_RANDOM,
    .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// ── BLE helpers ──────────────────────────────────────────────────────────────

static const Entry* currentEntries() {
    return (g_mode == SpooferMode::META) ? META_ENTRIES : FLOCK_ENTRIES;
}
static int currentCount() {
    return (g_mode == SpooferMode::META) ? META_COUNT : FLOCK_COUNT;
}
static const char* modeName() {
    return (g_mode == SpooferMode::META) ? "META" : "FLOCK";
}

static void printMenu() {
    Serial.println("\n─────────────────────────────────────────");
    Serial.printf("  BLE OUI Spoofer  (ESP32-C3)  [%s]\n", modeName());
    Serial.println("─────────────────────────────────────────");
    const Entry* entries = currentEntries();
    int count = currentCount();
    for (int i = 0; i < count; i++) {
        Serial.printf("  [%d] %s\n", i, entries[i].label);
    }
    Serial.println("─────────────────────────────────────────");
    Serial.printf("  n = next   0-%d = jump   auto-cycle %ds\n",
                  count - 1, DWELL_MS / 1000);
    Serial.println("  BOOT / m = switch META <-> FLOCK");
    Serial.println("─────────────────────────────────────────\n");
}

static void beginEntry(int idx);  // forward decl

static void switchToFlock() {
    if (g_mode == SpooferMode::FLOCK) return;  // already there
    g_mode     = SpooferMode::FLOCK;
    g_current  = 0;
    g_switchAt = millis() + DWELL_MS;
    g_flockAt  = 0;  // disarm timer
    Serial.printf("[mode] forced to %s\n", modeName());
    printMenu();
    beginEntry(g_current);
}

static void beginEntry(int idx) {
    esp_ble_gap_stop_advertising();

    const Entry* entries = currentEntries();
    int count = currentCount();
    if (idx < 0 || idx >= count) idx = 0;

    esp_bd_addr_t addr;
    addr[0] = entries[idx].oui[0] | 0xC0;  // static random: bits 7:6 must be 11
    addr[1] = entries[idx].oui[1];
    addr[2] = entries[idx].oui[2];
    addr[3] = 0x00;
    addr[4] = 0x00;
    addr[5] = (uint8_t)(idx + 1);
    memcpy(g_lastAddr, addr, 6);

    Serial.printf("\n[%s] entry %d/%d  label: %s\n",
        modeName(), idx + 1, count, entries[idx].label);
    Serial.printf("     OUI spoof:  %02X:%02X:%02X  (raw %02X:%02X:%02X)\n",
        addr[0], addr[1], addr[2],
        entries[idx].oui[0], entries[idx].oui[1], entries[idx].oui[2]);
    Serial.printf("     Full addr:  %02X:%02X:%02X:%02X:%02X:%02X\n",
        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    Serial.printf("     Dwell: %lus\n", (unsigned long)(DWELL_MS / 1000));

    g_pending = true;
    esp_ble_gap_set_rand_addr(addr);
    // advertising starts in the gap callback once the address is set
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* p) {
    switch (event) {
        case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
            g_pending = false;
            esp_ble_gap_start_advertising(&adv_params);
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (p->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                Serial.printf("    advertising... (%ds)\n", DWELL_MS / 1000);
            } else {
                Serial.printf("    adv start failed: %d\n",
                              p->adv_start_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            break;

        default:
            break;
    }
}

// ── Arduino entry points ─────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    
    Serial.println("\n[boot] starting...");
    Serial.flush();

    pinMode(BOOT_BTN, INPUT_PULLUP);

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        Serial.println("[ERROR] controller_init failed");
        return;
    }

    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        Serial.println("[ERROR] controller_init failed");
        return;
    }
    Serial.println("[boot] controller init ok");
    Serial.flush();

    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        Serial.println("[spoofer] controller_enable failed");
        return;
    }
    if (esp_bluedroid_init() != ESP_OK) {
        Serial.println("[spoofer] bluedroid_init failed");
        return;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        Serial.println("[spoofer] bluedroid_enable failed");
        return;
    }
    if (esp_ble_gap_register_callback(gap_cb) != ESP_OK) {
        Serial.println("[spoofer] register_callback failed");
        return;
    }

    g_bleReady = true;
    printMenu();

    g_current  = 0;
    g_switchAt = millis() + DWELL_MS;
    g_flockAt  = millis() + 10000;
    beginEntry(g_current);
}

void loop() {
    if (!g_bleReady) {
        delay(500);
        return;
    }

    // Handle boot button — switches META <-> FLOCK (polled, 50 ms debounce)
    bool btnNow = digitalRead(BOOT_BTN);
    if (btnNow == LOW && g_btnLastState == HIGH && millis() - g_btnLastMs > 50) {
        g_btnLastMs = millis();
        switchToFlock();
    }
    g_btnLastState = btnNow;

    // Handle serial input
    while (Serial.available()) {
        char c = (char)Serial.read();

        if (c == 'n' || c == 'N') {
            g_current  = (g_current + 1) % currentCount();
            g_switchAt = millis() + DWELL_MS;
            beginEntry(g_current);
        } else if (c == 'm' || c == 'M') {
            switchToFlock();
        } else if (c == '?') {
            printMenu();
        } else if (c >= '0' && c < '0' + currentCount()) {
            g_current  = c - '0';
            g_switchAt = millis() + DWELL_MS;
            beginEntry(g_current);
        }
    }

    // Periodic status — show what's live every 2 s
    uint32_t now = millis();
    if (!g_pending && now - g_lastStatusMs >= 2000) {
        g_lastStatusMs = now;
        uint32_t remaining = (g_switchAt > now) ? (g_switchAt - now) / 1000 : 0;
        Serial.printf("[TX] [%s] %02X:%02X:%02X:%02X:%02X:%02X  next in %lus\n",
            modeName(),
            g_lastAddr[0], g_lastAddr[1], g_lastAddr[2],
            g_lastAddr[3], g_lastAddr[4], g_lastAddr[5],
            (unsigned long)remaining);
    }

    // Auto-switch to FLOCK after 10 s
    if (g_flockAt && millis() >= g_flockAt) {
        Serial.println("[mode] 10s elapsed — switching to FLOCK");
        switchToFlock();
    }

    // Auto-cycle within current mode
    if (!g_pending && millis() >= g_switchAt) {
        g_current  = (g_current + 1) % currentCount();
        g_switchAt = millis() + DWELL_MS;
        beginEntry(g_current);
    }

    delay(10);
}
