# Invader Sniffer

![image](https://github.com/user-attachments/assets/e507fb9a-7b86-4d6a-b87a-ca775381b1d8)

![Build](https://github.com/lukeswitz/invader-sniffer/actions/workflows/build.yml/badge.svg)

A dual-mode WiFi and BLE packet sniffer that captures to PCAP files on an SD card. Supports OUI and name-based target detection with auto-capture, a detection sweep mode, Meta Ray-Ban and Flock (Axis) auto-alerts, and a config web UI.

Built for the Waveshare ESP32-S3 1.47" display board in two variants: standard and USB-C/1.47B.

> [!WARNING]
> Use only where you have explicit authorization. Capturing wireless traffic, monitoring devices, or spoofing identifiers may be illegal in your jurisdiction.

## Table of Contents

- [Features](#features)
- [Boot Sequence](#boot-sequence)
- [Device Modes](#device-modes)
- [Target Detection](#target-detection)
- [LED Colors](#led-colors)
- [Display](#display)
- [Controls](#controls)
- [Building](#building)
- [Flashing](#flashing)
- [Companion: BLE OUI Spoofer](#companion-ble-oui-spoofer)
- [SD Card Layout](#sd-card-layout)
- [Legal Disclaimer](#legal-disclaimer)
- [Liability](#liability)

---

## Features

- **WiFi promiscuous capture** — all 802.11 frames on channels 1–14, saved as radiotap PCAP
- **BLE passive scan capture** — advertisement frames reconstructed into BLE LL with PHDR PCAP format
- **Detection mode** — simultaneous WiFi (ch 1/6/11) and BLE sweep; auto-starts pcap on the radio that sees a target first
- **OUI target detection** — alerts on configured vendor MAC prefixes
- **Name / wildcard detection** — matches BLE device names and WiFi SSIDs with `*` wildcard patterns
- **Hardcoded special targets** — Axis IP cameras (FLOCK) and Meta Ray-Ban always active
- **Config web UI** — WiFi AP on boot for managing custom targets
- **USB MSC mode** — plug into a computer to mount the SD card as a drive

## Boot Sequence

Power on -> USB host detected? -> MSC mode (SD card as USB drive) -> No USB host -> Config AP (30s) -> Capture mode

### USB MSC Mode

If a USB host is detected within 500 ms of boot, the device enters MSC mode and presents the SD card as a USB mass storage device. Press BOOT to restart into capture mode.

### Config Mode

On every normal boot, the device starts a WiFi access point for 30 seconds:

| Item         | Value                |
| ------------ | -------------------- |
| SSID         | `SNIFF-CONFIG`       |
| Password     | `sniffconfig`        |
| URL          | `http://192.168.4.1` |
| Timeout      | 30 seconds           |

The web UI lets you add and delete custom targets before capture starts. Press BOOT during the countdown to skip config and go straight to capture.

## Device Modes

Single-tap toggles capture on or off. Double-tap cycles through three idle modes:

WiFi idle (red) -> BLE idle (blue) -> Detection (purple) -> WiFi idle

### WiFi Capture

- Hops channels 1–14 using a hardware timer
- Primary channels (1, 6, 11) get more dwell time; busy channels accumulate extra dwell based on packet rate
- All management, control, and data frames captured
- Source MAC and SSID checked against targets on every frame

### BLE Capture

- Active scan, all advertising PDU types
- 250 ms deduplication window per device, payload, and event type
- Frames reconstructed with BLE LL link layer header

### Detection Mode

Detection mode runs a passive sweep looking for configured targets on both radios simultaneously, then auto-starts a focused pcap when one is found.

- **WiFi**: hops channels 1, 6, 11 only for speed
- **BLE**: passive scan running concurrently via ESP32 coexistence (~31% BLE / ~69% WiFi radio time)
- **On target seen**: immediately starts pcap on the radio that detected it and switches to capture mode

### PCAP Format

| Mode | Link Type                 | Value |
| ---- | ------------------------- | ----- |
| WiFi | IEEE 802.11 Radiotap      | 127   |
| BLE  | Bluetooth LE LL with PHDR | 256   |

Files named sequentially: `wifi_0000.pcap`, `ble_0000.pcap`, etc. Open directly in Wireshark.

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
- `Camera*` — any device name starting with Camera
- `*Flock*` — any name containing Flock

A name-only target (no OUIs) matches purely by name pattern.

### Hardcoded Special Targets

Always active regardless of SD config.

| Target               | OUIs                                                  | LED Alert         |
| -------------------- | ----------------------------------------------------- | ----------------- |
| **Axis IP Camera**   | `D4:11:D6`, `00:17:3D`, `E0:0A:F6`, `00:25:DF`, `B4:1E:52` | Green rapid strobe |
| **Meta Ray-Ban**     | `7C:2A:9E`, `CC:66:0A`, `F4:03:43`, `5C:E9:1E`        | Cyan sine pulse   |

BLE random address type bits (top 2 bits of byte 0) are masked before OUI comparison.

### Adding Targets — Web UI

Connect to `SNIFF-CONFIG`, navigate to `http://192.168.4.1`. Enter a name (supports `*` wildcard) and up to 5 OUIs in `AA:BB:CC` format. OUIs are optional; name-only targets match by device name or SSID wildcard.

## LED Colors

| State             | Color             | Pattern                  |
| ----------------- | ----------------- | ------------------------ |
| WiFi idle         | Red               | Slow sine pulse          |
| BLE idle          | Blue              | Slow sine pulse          |
| Detection mode    | Purple            | Slow sine pulse          |
| WiFi capturing    | Green             | Random flicker           |
| WiFi packet flash | Yellow            | 80 ms flash              |
| BLE capturing     | Blue              | Slow sine pulse          |
| Custom target hit | Yellow/warm white | 150 ms alternating flash |
| FLOCK (Axis) hit  | Red               | 50 ms rapid strobe       |
| Meta Ray-Ban hit  | Cyan              | 600 ms sine pulse        |

## Display

### Title Bar

Shows current mode: `WIFI CAPTURE`, `BLE CAPTURE`, or `DETECTION MODE`.

### Status Bar

During capture: filename, packet count, channel (WiFi), drop count.
Idle: `WIFI READY`, `BLE READY`, or `DETECTION ACTIVE`.

### Channel Activity Map

**WiFi capture**: 14 bars for channels 1–14. Height indicates dwell weight. Current channel is white, high traffic is magenta, primary (1/6/11) is green, others are red.

## Controls

### Button (GPIO 0 / BOOT)

| Press                      | Action                                   |
| -------------------------- | ---------------------------------------- |
| Single tap                 | Toggle capture on/off                    |
| Double tap (within 500 ms) | Cycle mode: WiFi -> BLE -> Detection     |
| Hold during MSC mode       | Restart into capture mode                |
| Press during config        | Skip config, enter capture immediately   |

### Serial (115200 baud)

| Key       | Action      |
| --------- | ----------- |
| `s` / `S` | Toggle capture |
| `m` / `M` | Switch mode |

## Building

Requires PlatformIO.

```bash
pio run -e wave_sniff_s3
pio run -e wave_sniff_s3 -t upload

pio run -e wave_sniff_s3_b
pio run -e wave_sniff_s3_b -t upload
```

Dependencies managed by PlatformIO:

- `moononournation/GFX Library for Arduino @ 1.5.9`
- `bblanchon/ArduinoJson @ ^7.0.0`
- `ESP32Async/AsyncTCP @ ^3.3.3`
- `ESP32Async/ESPAsyncWebServer @ ^3.6.0`

## Flashing

### Web Flasher (browser, no tools required)

Chrome or Edge with WebSerial. Hold BOOT while plugging in USB:

https://lukeswitz.github.io/invader-sniffer

### CLI One-Liner (macOS / Linux)

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/lukeswitz/invader-sniffer/main/flash.sh)
```

Auto-detects the serial port and prompts for board variant. Override port with `PORT=/dev/ttyXXX`.

### CI

GitHub Actions builds both environments on every push to `main` and deploys merged binaries to GitHub Pages. Artifacts are also downloadable from the Actions tab.

## Companion: BLE OUI Spoofer

A separate PlatformIO project for an **ESP32-C3** that spoofs target OUIs over BLE to test the sniffer's detection logic without needing real target hardware.

Cycles through all hardcoded target OUIs (Axis/FLOCK and Meta Ray-Ban), advertising each as a BLE non-connectable packet with a static-random address whose top 3 bytes match the spoofed OUI.

### Controls

| Input     | Action                |
| --------- | --------------------- |
| BOOT      | Switch META/FLOCK mode |
| `n` / `N` | Skip to next OUI      |
| `m` / `M` | Switch mode           |
| `0`–`8`   | Jump to OUI index     |
| `?`       | Print status          |

```bash
cd spoofer
pio run -e spoofer_c3 -t upload
pio device monitor
```

## SD Card Layout

> [!NOTE]
> SD accessed via SD_MMC 4-bit mode. In USB MSC mode the host OS reads the card directly — do not write from the host while capture is running.

```
/
oui_targets.json
wifi_0000.pcap
wifi_0001.pcap
ble_0000.pcap
```

---

## Legal Disclaimer

This tool is provided for authorized security research, network diagnostics, and educational purposes only.

Capturing wireless packets and spoofing MAC/BLE addresses may be illegal without explicit authorization from the network owner and all parties whose data may be captured. Before using this device, you are solely responsible for:

- Obtaining written permission from the owner of any network or device you monitor
- Complying with all applicable laws in your jurisdiction, including but not limited to the Electronic Communications Privacy Act (ECPA), the Computer Fraud and Abuse Act (CFAA), the General Data Protection Regulation (GDPR), and equivalent national and regional statutes
- Ensuring that use of the BLE spoofer companion does not violate radio spectrum regulations (FCC Part 15, CE RED, or local equivalents) in your country
- Passive promiscuous WiFi capture of 802.11 management and data frames, and BLE advertisement scanning, may expose personally identifiable information (MAC addresses, device names, payload data). Handle all captured data responsibly.

The authors accept no liability for misuse. Use of this software in violation of any law is strictly prohibited and entirely your own responsibility.

## Liability

Any images, graphics, logos, screenshots, or other visual materials included in this project are provided "as is" for informational or illustrative purposes only. The project maintainers make no representations or warranties regarding ownership, licensing status, accuracy, non-infringement, or fitness for any particular purpose of such materials, except where explicitly stated.

GitHub, the GitHub logo, Meta, and the Meta logo are trademarks or registered trademarks of their respective owners. Any references, names, marks, logos, screenshots, or other brand assets are used solely for identification, commentary, compatibility, or informational purposes where applicable. Their use does not imply any affiliation with, endorsement by, sponsorship from, or approval by those trademark owners.

Users are responsible for ensuring their own use, redistribution, modification, or publication complies with applicable trademark, copyright, and other intellectual property laws.
