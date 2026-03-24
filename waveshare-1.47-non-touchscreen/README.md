# Waveshare 1.47" ESP32-S3 Hybrid Sniffer

A dual-mode WiFi and BLE packet sniffer that captures to PCAP files on an SD card. Built for the Waveshare ESP32-S3 1.47" display board. Features OUI-based target detection with visual and LED alerts, a config web UI, and USB mass storage mode for easy file retrieval.

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32-S3 (240 MHz, 16 MB flash, PSRAM) |
| Display | Waveshare 1.47" ST7789, 172x320, SPI |
| Storage | SD card via SD_MMC 4-bit |
| LED | NeoPixel on GPIO 38 |
| Button | BOOT button on GPIO 0 |
| USB | Native USB (CDC + MSC) |

## Features

- **WiFi promiscuous capture** — all 802.11 frames on channels 1–14, saved as radiotap PCAP
- **BLE passive scan capture** — advertisement frames reconstructed into BLE LL with PHDR PCAP format
- **OUI target detection** — alerts when a detected MAC matches a configured vendor prefix
- **Hardcoded special targets** — Axis IP cameras and Meta Ray-Ban glasses always active
- **Config web UI** — WiFi AP on boot for managing custom OUI targets
- **USB MSC mode** — plug into a computer to mount the SD card as a drive

---

## Boot Sequence

```
Power on
  -> USB host detected? -> MSC mode (SD card as USB drive)
  -> No USB host       -> Config AP (30s) -> Capture mode
```

### USB MSC Mode

If a USB host is detected within 500 ms of boot, the device enters MSC mode and presents the SD card as a USB mass storage device. The display shows "USB MODE" with mount status. Press BOOT to restart into capture mode.

### Config Mode

On every normal boot, the device starts a WiFi access point for 30 seconds:

| | |
|---|---|
| SSID | `SNIFF-CONFIG` |
| Password | `sniffconfig` |
| URL | `http://192.168.4.1` |
| Timeout | 30 seconds (resets while a client is connected) |

The web UI lets you add and delete custom OUI targets before capture starts. Press BOOT during the countdown to skip config and go straight to capture. The display shows the countdown, connected client count, and a progress bar.

---

## Capture Modes

### WiFi

- Hops channels 1–14 using a hardware timer (250 ms base interval)
- Primary channels (1, 6, 11) get more dwell time; busy channels accumulate extra dwell based on packet rate
- All management, control, and data frames are captured
- Source MAC checked against OUI targets on every frame

### BLE

- Active scan, all advertising PDU types (ADV_IND, ADV_DIRECT_IND, ADV_NONCONN_IND, SCAN_IND, SCAN_RSP)
- 250 ms deduplication window per device/payload/event type using FNV-1a hash
- Frames reconstructed with correct BLE LL link layer header and CRC24

### PCAP Format

| Mode | Link Type | Value |
|------|-----------|-------|
| WiFi | IEEE 802.11 Radiotap | 127 |
| BLE | Bluetooth LE LL with PHDR | 256 |

Files are named sequentially: `wifi_0000.pcap`, `wifi_0001.pcap`, `ble_0000.pcap`, etc. Open in Wireshark directly.

---

## OUI Target Detection

A target is a named entry with up to 5 MAC OUI prefixes (first 3 bytes). Up to 16 custom targets can be stored in `/oui_targets.json` on the SD card. Targets are checked against every captured frame's source MAC.

### Adding Targets — Web UI

Connect to `SNIFF-CONFIG` during the boot window, navigate to `http://192.168.4.1`, and use the form to add a name and up to 5 OUIs in `AA:BB:CC` format. Click **Save Configuration** to persist to SD, or **Save & Start Capture** to save and immediately exit config mode.

### OUI JSON Format

```json
{
  "targets": [
    {
      "name": "My Target Device",
      "ouis": [
        [170, 187, 204],
        [17, 34, 51]
      ]
    }
  ]
}
```

### Hardcoded Special Targets

These are always active regardless of the SD config file.

| Target | OUIs | Trigger |
|--------|------|---------|
| **Axis IP Camera** (Evil Bird) | `D4:11:D6`, `00:17:3D`, `E0:0A:F6`, `00:25:DF`, `B4:1E:52` | Any Axis Communications camera MAC |
| **Meta Ray-Ban** (Ray-Ban) | `7C:2A:9E`, `CC:66:0A`, `F4:03:43`, `5C:E9:1E` | Meta Ray-Ban glasses MAC |

BLE random address type bits (top 2 bits of byte 0) are masked before comparing, so spoofed static-random addresses still match.

---

## LED Colors

The NeoPixel LED (GPIO 38) indicates current state:

### Idle (not capturing)

| Mode | Color | Pattern |
|------|-------|---------|
| WiFi idle | Green | Slow sine pulse (~2 s cycle) |
| BLE idle | Blue | Slow sine pulse (~2 s cycle) |

### Capturing

| Mode | Color | Pattern |
|------|-------|---------|
| WiFi capturing | Orange/yellow | Random flicker (flame effect) |
| WiFi packet received | Warm white | 80 ms flash |
| BLE capturing | Blue | Fast sine pulse (~300 ms cycle) |

### Target Hit (1 second, then returns to capture color)

| Target | Color | Pattern |
|--------|-------|---------|
| Custom OUI target | Yellow / warm white | Alternating 150 ms flash |
| Axis camera (Evil Bird) | Red | Rapid strobe (50 ms, ominous alarm) |
| Meta Ray-Ban | Cyan | Smooth sine pulse (600 ms cycle) |

---

## Display

The 172x320 display uses a double-buffered canvas (30 fps cap).

### Main Screen Layout

```
+------------------+
|   TITLE BAR      |  <- Mode label, separator line
+------------------+
|                  |
|   STAR FIELD     |  <- 80 parallax stars (speed x2.5 when capturing)
|                  |
|    [MASCOT]      |  <- Animated sprite, center screen
|                  |
|  channel map     |  <- WiFi only: bar chart, 14 channels
+------------------+
|   STATUS BAR     |  <- Filename, packet count, drops, channel
+------------------+
```

### Title Bar

| State | Label | Color |
|-------|-------|-------|
| Idle | `INSERT TOKEN` | Mode color (green/blue) |
| WiFi capture | `WIFI CAPTURE` | Magenta, blinks |
| BLE capture | `BLE CAPTURE` | Magenta, blinks |

### Mascot Sprite

A pixel-art sprite animates at ~2.5 fps (2-frame cycle) in the center of the screen. Its appearance changes based on the last detected target:

| State | Sprite | Color |
|-------|--------|-------|
| Normal idle (WiFi) | Crab | Green |
| Normal idle (BLE) | Crab | Blue |
| Normal capturing | Crab | Magenta |
| Axis camera detected | Crow (Evil Bird) | Red |
| Ray-Ban detected | Sunglasses | Cyan |

When a packet hits the crab, it shakes briefly. Projectiles travel upward from the channel bar and disappear on impact.

### Status Bar

During capture:
- Filename (truncated), packet count
- Channel (WiFi) and drop count

When idle:
- Mode label (`WIFI READY` / `BLE READY`)
- Last capture filename

### Channel Activity Map (WiFi only)

14 bars representing channels 1–14. Bar height reflects accumulated dwell weight. Colors:

| | Color |
|---|---|
| Current channel | White |
| High-traffic channel | Magenta |
| Primary channel (1, 6, 11) | Green |
| Other | Dim gray |

### Config Screen

During the boot config window, the display shows the AP credentials, connected client count, countdown timer, a progress bar, and a `[ BOOT ] skip` hint.

---

## Controls

### Button (GPIO 0 / BOOT)

| Press | Action |
|-------|--------|
| Single tap | Toggle capture on/off |
| Double tap (within 500 ms) | Switch between WiFi and BLE mode |
| Hold during MSC mode | Restart into capture mode |
| Press during config countdown | Skip config, enter capture immediately |

### Serial (115200 baud)

| Key | Action |
|-----|--------|
| `s` / `S` | Toggle capture |
| `m` / `M` | Switch WiFi/BLE mode |

---

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -e wave_sniff_s3
pio run -e wave_sniff_s3 -t upload
```

Dependencies (managed by PlatformIO):

- `moononournation/GFX Library for Arduino @ 1.5.7`
- `bblanchon/ArduinoJson @ ^7.0.0`
- `me-no-dev/AsyncTCP @ ^1.1.1`
- `me-no-dev/ESPAsyncWebServer @ ^1.2.3`

---

## SD Card Layout

```
/
  oui_targets.json    <- Custom OUI target config (written by web UI)
  wifi_0000.pcap
  wifi_0001.pcap
  ble_0000.pcap
  ...
```

The SD card is accessed via SD_MMC in 4-bit mode. In USB MSC mode, the host OS can read the card directly. Do not write to the card from the host while capture is running.
