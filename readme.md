# Waveshare 1.47" ESP32-S3 Hybrid Sniffer

A dual-mode WiFi and BLE packet sniffer that captures to PCAP files on an SD card. Built for the Waveshare ESP32-S3 1.47" display board. Features OUI-based target detection with visual and LED alerts, a config web UI, and USB mass storage mode for easy file retrieval.

---

## Legal Disclaimer

**This tool is provided for authorized security research, network diagnostics, and educational purposes only.**

Capturing wireless packets and spoofing MAC/BLE addresses may be illegal without explicit authorization from the network owner and all parties whose data may be captured. Before using this device, you are solely responsible for:

- Obtaining written permission from the owner of any network or device you monitor.
- Complying with all applicable laws in your jurisdiction, including but not limited to the Electronic Communications Privacy Act (ECPA), the Computer Fraud and Abuse Act (CFAA), the General Data Protection Regulation (GDPR), and equivalent national and regional statutes.
- Ensuring that use of the BLE spoofer companion does not violate radio spectrum regulations (FCC Part 15, CE RED, or local equivalents) in your country.

Passive promiscuous WiFi capture of 802.11 management and data frames, and BLE advertisement scanning, may expose personally identifiable information (MAC addresses, device names, payload data). Handle all captured data responsibly.

**The authors accept no liability for misuse.** Use of this software in violation of any law is strictly prohibited and entirely your own responsibility.

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


## Review PCAPs for target OUI's using `tshark`

Source: OUI Spy by @colpanichacks

> [!NOTE]
> May produce false positives, or detect other random cameras.

```bash
OUI="b4:1e:52\|00:25:df\|e0:0a:f6\|d4:11:d6\|00:17:3d\|00:0a:b1\|00:50:c2\|00:bf:15\|58:8e:81\|ec:1b:bd\|90:35:ea\|04:0d:84\|f0:82:c0\|1c:34:f1\|38:5b:44\|94:34:69\|b4:e3:f9\|70:c9:4e\|3c:91:80\|d8:f3:bc\|80:30:49\|14:5a:fc\|74:4c:a1\|08:3a:88\|9c:2f:9d\|94:08:53\|e4:aa:ea\|f4:6a:dd\|f8:a2:d6\|00:f4:8d\|d0:39:57\|e8:d0:fc"

for f in *.pcap; do 
echo "=== $f ==="
tshark -r "$f" -T fields -e btle.advertising_address | grep -i "$OUI"
tshark -r "$f" -T fields -e wlan.sa -e wlan.da | grep -i "$OUI"
done
```
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
| **Generic ALPR & Flock**  | `D4:11:D6`, `00:17:3D`, `E0:0A:F6`, `00:25:DF`, `B4:1E:52` | Most common camera OUIs |
| **Meta Ray-Ban** | `7C:2A:9E`, `CC:66:0A`, `F4:03:43`, `5C:E9:1E` | Creepy glasses  |

BLE random address type bits (top 2 bits of byte 0) are masked before comparing, so spoofed static-random addresses still match.

---

## LED Colors

The NeoPixel LED (GPIO 38) indicates current state:

### Idle (not capturing)

| Mode | Color | Pattern |
|------|-------|---------|
| WiFi idle | Red | Slow sine pulse (~2 s cycle) |
| BLE idle | Blue | Slow sine pulse (~2 s cycle) |

### Capturing

| Mode | Color | Pattern |
|------|-------|---------|
| WiFi capturing | Orange/yellow | Random flicker (flame effect) |
| WiFi packet received | Yellow | 80 ms flash |
| BLE capturing | Blue | Slow sine pulse (~4 s cycle) |

### Target Hit (1 second, then returns to capture color)

| Target | Color | Pattern |
|--------|-------|---------|
| Custom OUI target | Yellow / warm white | Alternating 150 ms flash |
| FLOCK camera | Red | Rapid strobe (50 ms) |
| Meta Ray-Ban | Cyan | Smooth sine pulse (600 ms cycle) |

---

## Display

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

## Companion: BLE OUI Spoofer (`spoofer/`)

A separate PlatformIO project for an **ESP32-C3** that spoofs target OUIs over BLE to test the sniffer's detection logic without needing real target hardware.

### Hardware

Any ESP32-C3 dev board (e.g. ESP32-C3-DevKitM-1). Uses the onboard BOOT button (GPIO 9) and USB-CDC serial at 115200.

### What it does

Cycles through all hardcoded target OUIs — Axis camera (FLOCK) and Meta Ray-Ban (META) — advertising each as a BLE non-connectable packet with a static-random address whose top 3 bytes match the spoofed OUI. Each OUI is advertised for 5 seconds before auto-advancing to the next.

### Controls

| Input | Action |
|-------|--------|
| BOOT button | Switch between META and FLOCK mode |
| `n` / `N` | Skip to next OUI immediately |
| `m` / `M` | Switch mode (same as BOOT) |
| `0`–`8` | Jump directly to that OUI index |
| `?` | Print menu / status |

Serial output prints the active OUI, spoofed address, and countdown every 2 seconds.

### Building

```bash
cd spoofer
pio run -e spoofer_c3 -t upload
pio device monitor
```

### Notes

- The top 2 bits of the first address byte are forced to `11` (static-random BLE address type), matching the masking logic in the sniffer (`ouiByte0()`), so the sniffer will recognise the spoofed address even though those bits are set.
- The spoofer is for testing only. It transmits real BLE packets.

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
