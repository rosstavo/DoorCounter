# CLI scripts

Command-line workflow for flashing and monitoring the counter, as an alternative
to the Arduino IDE GUI described in the top-level README. Handy for quick
reflashes and for watching serial while tuning at the doorway.

## One-time setup

```sh
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

# pyserial for the monitor (system Python is often PEP 668 / externally managed,
# so use a venv):
python3 -m venv .venv && .venv/bin/pip install pyserial
```

## Flashing — `flash.sh`

```sh
scripts/flash.sh firmware   # compile + upload the sketch
scripts/flash.sh fs         # build LittleFS from data/ and flash it
scripts/flash.sh all        # fs, then firmware
```

- Pinned to **115200 baud** — this board drops the chip mid-write at the default
  921600.
- Auto-detects the port (first `/dev/cu.usbserial-*`); override with
  `PORT=/dev/cu.usbserial-XXXX scripts/flash.sh firmware`.
- **`fs` wipes stored counters/logs** (it reflashes the filesystem from
  `door_counter/data/`, which holds only `service_account.json`). Use it for a
  clean deploy or to reset test data — never on a live device mid-day.

## Monitoring — `monitor.py`

```sh
.venv/bin/python scripts/monitor.py            # stream until Ctrl-C
.venv/bin/python scripts/monitor.py --reset    # reboot board first (see boot banner)
.venv/bin/python scripts/monitor.py --seconds 30
```

Auto-detects the port; `--port` to override, `--baud` defaults to 115200.

## Gotchas learned the hard way

- **115200 baud only** for uploads (see above).
- The USB port can **re-enumerate** if the board is unplugged/replugged
  (e.g. `-2110` → `-1110`); the scripts auto-detect, or run
  `ls /dev/cu.usbserial-*`.
- `arduino-cli monitor` tends not to hold the port here — use `monitor.py`.
- PIR HC-SR501 **time-delay pot to minimum**, and tune `DETECTION_WINDOW_MS`
  ([door_counter/config.h](../door_counter/config.h)) from real walk-through
  deltas shown in the serial log.
