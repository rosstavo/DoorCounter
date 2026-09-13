#!/usr/bin/env bash
#
# CLI flashing for the door counter — the README's Arduino-IDE GUI steps are not
# required. NOTE: this board only flashes reliably at 115200 baud (the default
# 921600 drops the chip mid-write), so that's pinned below.
#
# Usage:
#   scripts/flash.sh firmware   # compile + upload the sketch
#   scripts/flash.sh fs         # build LittleFS from data/ and flash it
#   scripts/flash.sh all        # fs, then firmware
#   scripts/flash.sh tuner      # compile + upload the live web tuner (web/README.md)
#   scripts/flash.sh test       # compile + upload the PIR wiring test
#
# Override the port:  PORT=/dev/cu.usbserial-XXXX scripts/flash.sh firmware
#
# WARNING: `fs` reflashes the filesystem from door_counter/data/, which contains
# only service_account.json — so it WIPES the device's stored counters and logs
# (today.json, events.csv, daily.csv, gsheet_queue.csv). Use it for a clean
# deploy or to reset test data; don't run it on a live device mid-day.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH="$ROOT/door_counter"
TUNER="$ROOT/tuner"
HWTEST="$ROOT/hardware_test"
DATA="$SKETCH/data"
FQBN="esp32:esp32:esp32"
BAUD=115200

# Default "4MB with spiffs" partition layout (door_counter uses the default).
# If you change the partition scheme, update these from the partition CSV.
FS_OFFSET=0x290000
FS_SIZE=$((0x160000))   # 1441792 bytes

command -v arduino-cli >/dev/null \
  || { echo "arduino-cli not on PATH (brew install arduino-cli)"; exit 1; }

PORT="${PORT:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1 || true)}"
[ -n "$PORT" ] \
  || { echo "No /dev/cu.usbserial-* port found. Is the board plugged in?"; exit 1; }

PKG="$HOME/Library/Arduino15/packages/esp32"
MKLFS="$(ls -d "$PKG"/tools/mklittlefs/*/mklittlefs 2>/dev/null | sort -V | tail -1 || true)"
ESPTOOL="$(ls -d "$PKG"/tools/esptool_py/*/esptool  2>/dev/null | sort -V | tail -1 || true)"

flash_firmware() {
  echo ">> compiling firmware"
  arduino-cli compile --fqbn "$FQBN" "$SKETCH"
  echo ">> uploading firmware to $PORT @ ${BAUD}"
  arduino-cli upload -p "$PORT" --fqbn "${FQBN}:UploadSpeed=${BAUD}" "$SKETCH"
}

# The tuner and hardware-test sketches don't touch the filesystem, so they only
# ever need the firmware step — and neither disturbs stored counters or logs.
flash_sketch() {
  local dir="$1" name="$2"
  echo ">> compiling $name"
  arduino-cli compile --fqbn "$FQBN" "$dir"
  echo ">> uploading $name to $PORT @ ${BAUD}"
  arduino-cli upload -p "$PORT" --fqbn "${FQBN}:UploadSpeed=${BAUD}" "$dir"
}

flash_fs() {
  [ -n "$MKLFS" ]   || { echo "mklittlefs not found under $PKG/tools"; exit 1; }
  [ -n "$ESPTOOL" ] || { echo "esptool not found under $PKG/tools"; exit 1; }
  local tmpd img
  tmpd="$(mktemp -d)"
  img="$tmpd/littlefs.img"
  echo ">> building LittleFS image from $DATA"
  "$MKLFS" -c "$DATA" -p 256 -b 4096 -s "$FS_SIZE" "$img"
  echo ">> flashing filesystem to $FS_OFFSET on $PORT @ ${BAUD}"
  "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" \
    --before default_reset --after hard-reset write-flash "$FS_OFFSET" "$img"
  rm -rf "$tmpd"
}

case "${1:-}" in
  firmware) flash_firmware ;;
  fs)       flash_fs ;;
  all)      flash_fs; flash_firmware ;;
  tuner)    flash_sketch "$TUNER" "tuner" ;;
  test)     flash_sketch "$HWTEST" "hardware test" ;;
  *) echo "usage: $0 {firmware|fs|all|tuner|test}"; exit 1 ;;
esac
echo ">> done"
