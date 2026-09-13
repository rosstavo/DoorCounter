# Station Books Footfall Counter

Dual-PIR doorway entry/exit counter for a small independent bookshop, built on
an **Olimex ESP32-DevKit-LiPo**. It distinguishes **footfall** (people entering)
from **buyers** (till receipts) by counting people in and out of the single
shop doorway, logging daily totals locally and pushing them to a Google Sheet.

The device runs unattended: plug it in and it counts, logs, and pushes a daily
summary on its own. See [docs/Station_Books_Footfall_Counter_PRD.md](docs/Station_Books_Footfall_Counter_PRD.md)
for the full specification.

---

## Repository layout

```
door-counter/
├── door_counter/                 # Main firmware (Arduino sketch)
│   ├── door_counter.ino          #   setup/loop, state machine, button, watchdog
│   ├── config.h                  #   all tuneable constants (non-secret)
│   ├── secrets.h                 #   Wi-Fi + Sheet ID  (git-ignored)
│   ├── secrets.h.example         #   template for the above
│   ├── Types.h                   #   shared structs
│   ├── Storage.h / .cpp          #   LittleFS CSV logs, state, retry queue
│   ├── Detector.h                #   the detection state machine (shared, see below)
│   ├── GoogleSheets.h / .cpp     #   service-account JWT + Sheets v4 append
│   └── data/                     #   files flashed to the device filesystem
│       ├── service_account.json          (git-ignored — your real key)
│       └── service_account.json.example  (template)
├── hardware_test/
│   └── hardware_test.ino         # Minimal sensor-wiring test (flash this FIRST)
├── tuner/
│   └── tuner.ino                 # Tuning firmware: streams sensor JSON, timings
│                                 #   adjustable at runtime from the browser
├── web/                          # Live web tuner (Vite + serial bridge)
├── test/                         # Host-side tests — no board needed
└── README.md
```

`door_counter/Detector.h` holds the detection state machine on its own, and both
`door_counter.ino` and `tuner/tuner.ino` compile the same file. That is what
makes the tuner trustworthy: the constants you dial in are dialled in against
the code that ships. Change the state machine there and both stay in step;
`test/run.sh` guards its behaviour.

---

## Hardware & wiring

| Signal              | PIR pin | ESP32 pin |
|---------------------|---------|-----------|
| PIR #1 OUTER (street) VCC | VCC | 3.3V |
| PIR #1 OUTER GND    | GND     | GND |
| PIR #1 OUTER OUT    | OUT     | **GPIO 16** |
| PIR #2 INNER (shop) VCC | VCC | 3.3V (shared) |
| PIR #2 INNER GND    | GND     | GND |
| PIR #2 INNER OUT    | OUT     | **GPIO 17** |
| User button (BUT1)  | —       | GPIO 34 (on-board) |

- Mount both sensors at chest height aimed across the threshold, separated
  15–25 cm along the line of travel (OUTER nearer the street, INNER nearer the
  shop). Narrow each 120° cone to ~20–30° with a short black tube over the dome.
- Set each PIR's **delay potentiometer to minimum** (fully anticlockwise).
- OUT is 3.3 V TTL — connect directly to the GPIO, no level shifting.

---

## Toolchain

- **Arduino IDE 2.x** with the **Espressif ESP32 Arduino core** (board manager).
- Board: **ESP32 Dev Module** (the Olimex board is pin-compatible with DevKitC).
- No external libraries are required. The Google Sheets integration uses raw
  HTTPS (`WiFiClientSecure`/`HTTPClient`) and the mbedTLS bundled with the core —
  no third-party Google/OAuth library, per the PRD.
- Tested against ESP32 core 2.x and 3.x (the watchdog setup branches on
  `ESP_ARDUINO_VERSION_MAJOR`).

---

## Setup, step by step

### 1. Prove the wiring first

Open `hardware_test/hardware_test.ino`, flash it, open Serial Monitor at
**115200 baud**, and wave a hand at each sensor. You should see
`PIR OUTER triggered` / `PIR INNER triggered`. **Do not skip this** — confirm
both GPIOs read before layering on the full firmware. (Ignore phantom triggers
in the first ~60 s; PIRs are settling.)

### 2. Configure secrets

```
cp door_counter/secrets.h.example door_counter/secrets.h
```

Edit `door_counter/secrets.h` with your Wi-Fi SSID/password and the Google
Sheet ID (the long string in the sheet URL between `/d/` and `/edit`).

> A working `secrets.h` with placeholders is included so the project compiles
> out of the box, but you must fill in real values before deploying.

### 3. Google Sheets service account

1. In Google Cloud Console, create a project, enable the **Google Sheets API**,
   create a **Service Account**, and download its **JSON key**.
2. Save it as `door_counter/data/service_account.json`
   (`cp door_counter/data/service_account.json.example` … then paste your key).
3. **Share the Google Sheet** with the service account's `client_email`
   (e.g. `footfall-counter@your-project.iam.gserviceaccount.com`) as an Editor.
4. Make sure the first tab is named `Sheet1` (or change `GSHEET_RANGE` in
   `config.h`). Optionally add a header row:
   `Date | Total Entries | Total Exits | Net | Opening Time | Closing Time | Notes | Day | AM Entries | AM Exits | PM Entries | PM Exits`

### 4. Upload the filesystem (LittleFS)

The `data/` folder must be flashed to the device filesystem so the firmware can
read `service_account.json`:

- **Arduino IDE 2.x:** install the *"Arduino LittleFS Upload"* plugin, then run
  it from the command palette (`Ctrl/Cmd+Shift+P` → *Upload LittleFS to ...*).
- **PlatformIO:** `pio run --target uploadfs`.

Ensure the IDE's flash/partition scheme reserves a LittleFS partition (the
default "Default 4MB with spiffs" works).

### 5. Flash the firmware

Open `door_counter/door_counter.ino`, select the board/port, and upload.
Open Serial Monitor at 115200 to watch boot, warm-up, Wi-Fi, NTP and counting.

---

## TLS certificate

By default the Sheets/OAuth HTTPS calls fall back to an **unverified** TLS
connection and print a warning. For verified TLS, paste the **GTS Root R1** PEM
into `GOOGLE_ROOT_CA` near the top of `GoogleSheets.cpp` and keep
`GSHEET_INSECURE 0` in `config.h`. Set `GSHEET_INSECURE 1` to force the
unverified path (useful while debugging connectivity).

---

## Tuning the detection window

`DETECTION_WINDOW_MS` (default **400 ms**, in `config.h`) is the maximum gap
between the two sensor triggers for a valid event, and depends on your physical
sensor spacing and walking pace. Getting it right by editing a constant,
reflashing and squinting at a serial log is slow and miserable, so there is a
live web tuner for exactly this.

### With the web tuner (recommended)

```sh
scripts/flash.sh tuner       # tuning firmware
cd web && npm install && npm run dev
```

Open the **Network** URL on your phone and walk the doorway while watching the
sensor traces; drag the timings and the very next walk-through reflects them.
When it's right, copy the generated `config.h` block, paste it in, and
`scripts/flash.sh firmware` to put the real firmware back. Full instructions,
including the method that avoids the "narrow window hides its own evidence"
trap, are in **[web/README.md](web/README.md)**.

### By hand, from the serial log

1. Serial output is verbose by default — every valid sequence prints its
   `delta` (e.g. `Valid sequence OUTER->INNER (delta 180ms)`).
2. Walk through at normal pace several times; note the largest delta.
3. Set `DETECTION_WINDOW_MS` ≈ **1.5×** the largest observed delta.

Note that a window which is already too narrow reports those walk-throughs as
`DISCARDED_NOISE` rather than printing their delta, so widen it generously
before you start sampling.

---

## Tests

```sh
test/run.sh
```

Runs on the Mac with no board attached: the shared detection state machine is
compiled for the host and driven through entry, exit, noise, simultaneous and
debounce scenarios, and every JSON line `tuner.ino` emits is checked to parse.
Needs a C++ compiler and Node.

---

## How it works (behaviour summary)

- **Boot:** 60 s PIR warm-up (no counting), log boot, connect Wi-Fi + NTP in the
  background, then start counting. Counting never blocks on Wi-Fi.
- **Direction:** OUTER→INNER within the window = **ENTRY**; INNER→OUTER =
  **EXIT**. A lone trigger with no follow-up is discarded as noise. Both firing
  within 10 ms is discarded as `DISCARDED_SIMULTANEOUS`.
- **Debounce:** 1 s lock-out after each counted event.
- **Persistence:** every event and the running day total are written to flash
  (`/today.json`, `/events.csv`), so a power cut doesn't lose the day's count.
  Daily summaries go to `/daily.csv`.
- **AM/PM split:** each day's totals are also split into a morning and an
  afternoon half at `PM_START_HOUR` (13:00), and the row is tagged with
  its day name (`Mon`…`Sun`). See *Reading the numbers* below.
- **Daily push:** at `DAILY_RESET_HOUR:MINUTE` (default 22:00) — or on a 3 s
  button hold — the day's summary is appended to Google Sheets and counters
  reset. If Wi-Fi/Sheets is down, the row is queued to `/gsheet_queue.csv` and
  replayed automatically when connectivity returns.
- **Status page:** open `http://door-counter.local/` (or the device's IP) from
  any browser on the same network. Read-only: today's counts, sensor and Wi-Fi
  health, clock, and the last Sheets push. Reloads every 10 s.
- **Watchdog:** the hardware task watchdog reboots the device within ~30 s if
  the main loop hangs.
- **Health:** if a sensor never fires within 10 min after warm-up, a wiring
  warning is logged to serial every 60 s.

### Local log files (on device flash)

| File                 | Contents |
|----------------------|----------|
| `/events.csv`        | one row per entry/exit + boot/notes, with running totals |
| `/daily.csv`         | one row per day (same columns as the Google Sheet) |
| `/today.json`        | current day's live counters (reboot-safe) |
| `/gsheet_queue.csv`  | daily rows awaiting a successful Sheets push |

Event logs are size-capped (≈90 days) and rotated automatically. If flash runs
low, event logging stops but daily summaries keep writing.

`/daily.csv` columns (same order as the Google Sheet and the retry queue):

```
date,total_entries,total_exits,net,opening_time,closing_time,notes,
dow,am_entries,am_exits,pm_entries,pm_exits
```

The last five columns were added later, so they sit on the right — rows already
in the Sheet keep their meaning. A device upgraded from an older build rewrites
the `/daily.csv` header on first boot and keeps its old rows, which simply stop
after `notes`.

---

## Reading the numbers

**The daily total is a trend, not a headcount.** A PIR needs time to settle
after it fires, so two people crossing the threshold nose-to-tail are seen as
one. That is the sensor, not the firmware, and no tuning removes it. The count
therefore reads low, and it reads low in roughly the same way every day — which
is what makes it useful.

Use it by **comparing like with like**:

- Compare a day against the **median** of the same kind of day (weekday /
  Saturday / Sunday), not against an absolute target. The median ignores the
  odd freak day, which a mean does not.
- Compare **AM vs PM** on the same day to see which half of the day carries the
  trade.
- Collect **a few weeks** before drawing conclusions. Group the rows by the
  `dow` column, take the median of each group, then read new days against it.

---

## Acceptance checklist (from the PRD)

- [ ] Walking through either way logs ENTRY/EXIT correctly on serial.
- [ ] 10 walk-throughs in one direction, each left to complete = 10 events.
      (Back-to-back walkers undercount; see *Reading the numbers*.)
- [ ] Standing still in the doorway for 5 s logs nothing.
- [ ] Power-cycle preserves the current day's count.
- [ ] At the reset time, a correct row appears in the Google Sheet.
- [ ] Boots, connects, and counts with no human interaction.

---

## Security note

`secrets.h` and `data/service_account.json` hold credentials and are
**git-ignored**. Never commit them. Only the `.example` templates are tracked.
