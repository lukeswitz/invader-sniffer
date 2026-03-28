#!/usr/bin/env bash
# Invader Sniffer — one-shot flash script
# Usage: bash <(curl -fsSL https://raw.githubusercontent.com/lukeswitz/invader-sniffer/main/flash.sh)
# Override port: PORT=/dev/ttyUSB0 bash <(curl ...)
set -euo pipefail

PAGES_BASE="https://lukeswitz.github.io/invader-sniffer/firmware"

echo ""
echo "  INVADER SNIFFER — Flasher"
echo "  =========================="
echo ""

# Python check
if ! command -v python3 &>/dev/null; then
  echo "ERROR: python3 not found. Install from https://python.org and retry."
  exit 1
fi

# esptool check / install
if ! python3 -m esptool version &>/dev/null 2>&1; then
  echo "Installing esptool..."
  pip3 install --quiet esptool
fi

# Board selection
echo "Board variant:"
echo "  1) Standard            (wave_sniff_s3)   — USB-A port"
echo "  2) USB-C 1.47B variant (wave_sniff_s3_b) — USB-C port"
echo ""
read -rp "Select [1/2, default 1]: " CHOICE
CHOICE="${CHOICE:-1}"

case "$CHOICE" in
  2) ENV="wave_sniff_s3_b" ;;
  *) ENV="wave_sniff_s3" ;;
esac

BIN="${ENV}.bin"
TMP="/tmp/${BIN}"

echo ""
echo "Downloading ${BIN} from GitHub Pages..."
curl -fSL --progress-bar "${PAGES_BASE}/${BIN}" -o "$TMP"
echo "Downloaded: $(du -h "$TMP" | cut -f1)"

# Port detection
if [[ -z "${PORT:-}" ]]; then
  for candidate in \
      /dev/tty.usbmodem* \
      /dev/tty.SLAB_USBtoUART* \
      /dev/ttyUSB* \
      /dev/ttyACM*; do
    if [[ -e "$candidate" ]]; then
      PORT="$candidate"
      break
    fi
  done
fi

if [[ -z "${PORT:-}" ]]; then
  echo ""
  echo "Could not auto-detect a serial port."
  echo "Hold BOOT on the device, connect USB, then set PORT and re-run:"
  echo ""
  echo "  PORT=/dev/ttyXXX bash <(curl -fsSL https://raw.githubusercontent.com/lukeswitz/invader-sniffer/main/flash.sh)"
  exit 1
fi

echo ""
echo "Flashing to ${PORT} at 921600 baud..."
python3 -m esptool \
  --chip esp32s3 \
  --port "$PORT" \
  --baud 921600 \
  write_flash \
  --flash-mode dio \
  --flash-freq 80m \
  --flash-size 16MB \
  0x0 "$TMP"

echo ""
echo "Done. Power-cycle or press RESET on the device."
echo ""
