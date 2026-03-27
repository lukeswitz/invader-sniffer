# Waveshare 1.47" ESP32-S3 Hybrid Sniffer

> **Forked from [nitekry/invader-sniffer](https://github.com/nitekry/invader-sniffer)** — ported and extended for the [Waveshare ESP32-S3 1.47" display board](https://www.waveshare.com/esp32-s3-lcd-1.47.htm). Credit and thanks to the original author for the core sniffer concept and pixel-art mascot.

A dual-mode WiFi and BLE packet sniffer that captures to PCAP files on an SD card. Supports OUI and name-based target detection with auto-capture, a detection sweep mode, Meta Ray-Ban and Flock (Axis) auto-alerts, and a config web UI.

Built for the Waveshare ESP32-S3 1.47" display board (two variants: standard and USB-C/1.47B).

---

## Legal Disclaimer

**This tool is provided for authorized security research, network diagnostics, and educational purposes only.**

Capturing wireless packets may be illegal without explicit authorization from the network owner and all parties whose data may be captured. Before using this device, you are solely responsible for:

- Obtaining written permission from the owner of any network or device you monitor.
- Complying with all applicable laws in your jurisdiction, including but not limited to the Electronic Communications Privacy Act (ECPA), the Computer Fraud and Abuse Act (CFAA), the GDPR, and equivalent national and regional statutes.

**The authors accept no liability for misuse.**

---

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32-S3 (240 MHz, 16 MB flash, PSRAM) |
| Display | Waveshare 1.47" ST7789, 172x320, SPI |
| Storage | SD card via SD_MMC 4-bit |
| LED | NeoPixel on GPIO 38 |
| Button | BOOT button on GPIO 0 |
| USB | Native USB (CDC + MSC) |

Two build targets: `wave_sniff_s3` (standard) and `wave_sniff_s3_b` (USB-C 1.47B variant, `-DBOARD_1_47B`).

---

## Features

- **WiFi promiscuous capture** — all 802.11 frames on channels 1–14, saved as radiotap PCAP
- **BLE passive scan capture** — advertisement frames reconstructed into BLE LL with PHDR PCAP format
- **Detection mode** — simultaneous WiFi (ch 1/6/11) + BLE sweep; auto-starts pcap on the radio that sees a target first
- **OUI target detection** — alerts on configured vendor MAC prefixes
- **Name/wildcard detection** — matches BLE device names and WiFi SSIDs with `*` wildcard patterns
- **Hardcoded special targets** — Axis IP cameras (FLOCK) and Meta Ray-Ban always active
- **Config web UI** — WiFi AP on boot for managing custom targets
- **USB MSC mode** — plug into a computer to mount the SD card as a drive

---

## Boot Sequence

```
Power on
  -> USB host detected? -> MSC mode (SD card as USB drive)
  -> No USB host       -> Config AP (30s) -> Capture mode
```

### USB MSC Mode

If a USB host is detected within 500 ms of boot, the device enters MSC mode and presents the SD card as a USB mass storage device. Press BOOT to restart into capture mode.

### Config Mode

On every normal boot, the device starts a WiFi access point for 30 seconds:

| | |
|---|---|
| SSID | `SNIFF-CONFIG` |
| Password | `sniffconfig` |
| URL | `http://192.168.4.1` |
| Timeout | 30 seconds (resets while a client is connected) |

The web UI lets you add and delete custom targets before capture starts. Press BOOT during the countdown to skip config and go straight to capture.

---

## Device Modes

Single-tap toggles capture on/off. Double-tap cycles through three idle modes:

```
WiFi idle  ->  BLE idle  ->  Detection  ->  WiFi idle  -> ...
 (green)       (blue)        (purple)
```

### WiFi Capture

- Hops channels 1–14 using a hardware timer
- Primary channels (1, 6, 11) get more dwell time; busy channels accumulate extra dwell based on packet rate
- All management, control, and data frames captured
- Source MAC and SSID checked against targets on every frame

### BLE Capture

- Active scan, all advertising PDU types
- 250 ms deduplication window per device/payload/event type
- Frames reconstructed with BLE LL link layer header

### Detection Mode

Detection mode runs a passive sweep looking for configured targets on both radios simultaneously, then auto-starts a focused pcap when one is found.

- **WiFi**: hops channels 1, 6, 11 only
- **BLE**: passive scan running concurrently via ESP32 coexistence (~31% BLE / ~69% WiFi radio time)
- **On target seen**: immediately starts pcap on the radio that detected it and switches to capture mode
- Display shows purple theme with 3-bar channel activity map (ch 1, 6, 11) and "HUNTING ON WIFI+BLE"
- LED pulses purple

### PCAP Format

| Mode | Link Type | Value |
|------|-----------|-------|
| WiFi | IEEE 802.11 Radiotap | 127 |
| BLE | Bluetooth LE LL with PHDR | 256 |

Files named sequentially: `wifi_0000.pcap`, `ble_0000.pcap`, etc. Open directly in Wireshark.

---

## Target Detection

A target match triggers the LED alert, crab shake animation, and (in detection mode) auto-capture.

### OUI Matching

Each target has a name and up to 5 MAC OUI prefixes (first 3 bytes). Checked against:
- WiFi frame source MAC
- BLE advertisement address

### Name / Wildcard Matching

Target names support `*` as a wildcard and are matched against:
- **BLE**: device name from advertisement data (AD types `0x09` Complete Name, `0x08` Shortened Name)
- **WiFi**: SSID from beacon, probe request, and probe response frames

Matching is case-insensitive. Examples:
- `Camera*` — any device name starting with "Camera"
- `*Flock*` — any name containing "Flock"

A name-only target (no OUIs) matches purely by name pattern.

### Hardcoded Special Targets

Always active regardless of SD config.

| Target | OUIs | LED Alert |
|--------|------|-----------|
| **Axis IP Camera** (FLOCK) | `D4:11:D6`, `00:17:3D`, `E0:0A:F6`, `00:25:DF`, `B4:1E:52` | Green rapid strobe |
| **Meta Ray-Ban** | `7C:2A:9E`, `CC:66:0A`, `F4:03:43`, `5C:E9:1E` | Purple sine pulse |

BLE random address type bits (top 2 bits of byte 0) are masked before OUI comparison.

### Adding Targets — Web UI

Connect to `SNIFF-CONFIG`, navigate to `http://192.168.4.1`. Enter a name (supports `*` wildcard) and up to 5 OUIs in `AA:BB:CC` format. OUIs are optional — name-only targets match by device name/SSID wildcard.

### OUI JSON Format

```json
{
  "targets": [
    {
      "name": "Camera*",
      "ouis": []
    },
    {
      "name": "My Router",
      "ouis": [[170, 187, 204]]
    }
  ]
}
```

---

## LED Colors

| State | Color | Pattern |
|-------|-------|---------|
| WiFi idle | Red | Slow sine pulse |
| BLE idle | Blue | Slow sine pulse |
| Detection mode | Purple | Slow sine pulse |
| WiFi capturing | Orange/yellow | Random flicker |
| WiFi packet flash | Yellow | 80 ms flash |
| BLE capturing | Blue | Slow sine pulse |
| Custom target hit | Yellow/warm white | 150 ms alternating flash |
| FLOCK (Axis) hit | Green | 50 ms rapid strobe |
| Meta Ray-Ban hit | Purple | 600 ms sine pulse |

---

## Display

### Title Bar
Shows current mode: `WIFI CAPTURE`, `BLE CAPTURE`, or `DETECTION MODE` (purple).

### Status Bar
During capture: filename, packet count, channel (WiFi), drop count.
Idle: mode label (`WIFI READY` / `BLE READY` / `DETECTION ACTIVE`).

### Channel Activity Map

**WiFi capture**: 14 bars for channels 1–14. Height = dwell weight. Current channel = white, high traffic = magenta, primary (1/6/11) = green, other = dim.

**Detection mode**: 3 bars for channels 1, 6, 11 (purple theme). Height = packet rate. Current channel = white.

---

## Controls

### Button (GPIO 0 / BOOT)

| Press | Action |
|-------|--------|
| Single tap | Toggle capture on/off |
| Double tap (within 500 ms) | Cycle mode: WiFi idle -> BLE idle -> Detection -> WiFi idle |
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
# Standard board
pio run -e wave_sniff_s3
pio run -e wave_sniff_s3 -t upload

# USB-C 1.47B board
pio run -e wave_sniff_s3_b
pio run -e wave_sniff_s3_b -t upload
```

Dependencies (managed by PlatformIO):

- `moononournation/GFX Library for Arduino @ 1.5.9`
- `bblanchon/ArduinoJson @ ^7.0.0`
- `ESP32Async/AsyncTCP @ ^3.3.3`
- `ESP32Async/ESPAsyncWebServer @ ^3.6.0`

---

## Companion: BLE OUI Spoofer (`spoofer/`)

A separate PlatformIO project for an **ESP32-C3** that spoofs target OUIs over BLE to test the sniffer's detection logic without needing real target hardware.

Cycles through all hardcoded target OUIs (Axis/FLOCK and Meta Ray-Ban), advertising each as a BLE non-connectable packet with a static-random address whose top 3 bytes match the spoofed OUI.

### Controls

| Input | Action |
|-------|--------|
| BOOT button | Switch between META and FLOCK mode |
| `n` / `N` | Skip to next OUI |
| `m` / `M` | Switch mode |
| `0`–`8` | Jump to OUI index |
| `?` | Print status |

```bash
cd spoofer
pio run -e spoofer_c3 -t upload
pio device monitor
```

---

## SD Card Layout

```
/
  oui_targets.json    <- Custom target config (written by web UI)
  wifi_0000.pcap
  wifi_0001.pcap
  ble_0000.pcap
  ...
```

SD accessed via SD_MMC 4-bit mode. In USB MSC mode the host OS reads the card directly — do not write from the host while capture is running.
