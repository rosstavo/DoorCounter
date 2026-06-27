# PRD: Station Books Footfall Counter
*Claude Code handoff document*

---

## Overview

A dual-PIR doorway entry/exit counter for a small independent bookshop. The device counts people entering and exiting through a single doorway, logs daily totals, and makes those totals available alongside an existing end-of-day Z-read process. The goal is to distinguish between footfall (people entering the shop) and buyers (receipt count from the till), which is currently the single unresolved data gap in the shop's trading analysis.

This is a standalone embedded device. There is no existing codebase to integrate with. The operator is non-technical; the device must run without any manual software intervention after initial setup.

---

## Hardware

**Microcontroller:** Olimex ESP32-DevKit-LiPo
- ESP32-WROOM-32 module (dual-core Xtensa LX6, 240MHz, 4MB flash)
- Pin-to-pin compatible with Espressif ESP32-DevKitC
- Built-in LiPo charger circuit with JST-PH connector
- Micro-USB for programming and charging
- Wi-Fi (802.11 b/g/n) and Bluetooth 5 onboard
- Single 3.3V output pin, multiple GND pins on header

**Battery:** 2000mAh 3.7V LiPo, JST-PH connector, connected to board's onboard LiPo charger

**Sensors:** 2× Pi Hut PIR Motion Sensor Module (HC-SR501-compatible)
- 3-pin interface: VCC (5V tolerant, but 3.3V works), GND, OUT
- OUT is 3.3V TTL — safe to connect directly to ESP32 GPIO with no level shifting required
- Two onboard potentiometers: sensitivity (detection range) and delay (how long OUT stays HIGH after trigger)
- Delay potentiometer should be set to minimum (fully anticlockwise) before installation

**Wiring (confirmed, no breadboard in final build):**
- PIR #1 VCC → ESP32 3.3V
- PIR #1 GND → ESP32 GND
- PIR #1 OUT → ESP32 GPIO 16
- PIR #2 VCC → ESP32 3.3V (shared pin, piggybacked)
- PIR #2 GND → ESP32 GND (second GND pin if available, otherwise piggybacked)
- PIR #2 OUT → ESP32 GPIO 17
- All connections via female-to-female Dupont jumper wires (PIR male pins to ESP32 male header pins, direct point-to-point)

**Physical mounting:**
- Both PIR sensors mounted at chest height on a fixed bracket, aimed across the doorway threshold
- Sensors separated by 15–25cm along the axis of travel (one slightly further inside the shop than the other)
- Each sensor's 120° detection cone narrowed to approximately 20–30° using a short length of black tube/conduit (~3–4cm) fitted over the dome
- PIR #1 (GPIO 16) is the outer sensor (faces the street side of the threshold)
- PIR #2 (GPIO 17) is the inner sensor (faces the shop interior side of the threshold)

---

## Behaviour Specification

### Boot sequence

1. On power-up, wait **60 seconds** before beginning to process sensor input — PIR sensors produce unreliable output during warm-up and will otherwise generate phantom counts
2. Log boot time to the daily log file
3. Connect to Wi-Fi (see Data Output section)
4. Begin normal counting loop

### Entry/exit detection logic

The device determines direction of travel by the order in which the two sensors trigger:

- **Entry:** GPIO 16 (outer) triggers first, then GPIO 17 (inner) triggers within the detection window → increment ENTRY counter by 1
- **Exit:** GPIO 17 (inner) triggers first, then GPIO 16 (outer) triggers within the detection window → increment EXIT counter by 1
- **Discard:** Only one sensor fires with no follow-up from the other within the detection window → treat as noise, do not increment either counter. Causes include: someone reaching toward the door without entering, a passing vehicle's headlights at night, a bird or animal triggering one sensor only

**Detection window:** The time within which the second sensor must fire after the first for a valid entry/exit to be registered. Default value: **400ms**. This should be a configurable constant at the top of the code (not hardcoded inline), since the optimal value depends on the physical sensor separation and typical walking pace at the specific doorway, and will need empirical tuning during installation.

**Debounce:** After a valid entry or exit is registered, ignore both GPIO pins for **1 second** before resuming detection. This prevents a single person walking slowly through the threshold from registering as multiple events.

**State machine approach:** Use a simple state machine rather than polling in a tight loop:
- IDLE: waiting for first trigger
- ARMED (sensor, timestamp): first sensor has fired, waiting for second within the window
- If second fires within window: register event, reset to IDLE, apply debounce pause
- If window expires without second firing: discard, reset to IDLE

### Data the device tracks

Per session (from boot to next boot, or midnight rollover):
- Entry count
- Exit count
- Net count (entries minus exits) — a sanity check; at end of day this should be approximately zero or slightly negative (more people left than came in, since some may have left before the counter was running)
- Timestamp of each individual event (for debugging and post-hoc analysis if needed)

Per day:
- Date
- Total entries
- Total exits
- Opening time (first entry of the day)
- Closing time (last exit of the day)
- Any boot/restart events with timestamps

---

## Data Output

Two output methods, both implemented. Wi-Fi connectivity is required for Method 1; Method 2 works offline.

### Method 1 — Google Sheets (primary)

Append a row to a specified Google Sheet at the end of each trading day (triggered at a configurable time, default 22:00, or manually via the reset button).

Row format:
```
Date | Total Entries | Total Exits | Net | Opening Time | Closing Time | Notes
```

Use the Google Sheets API via HTTP POST (no third-party library — raw HTTPS requests only, using the ESP32's built-in WiFiClientSecure). Authentication via a Google Service Account JSON key, with the private key and sheet ID stored in a config file (see Configuration section).

If Wi-Fi is unavailable at the time of the daily push, queue the data locally and retry on next successful connection.

### Method 2 — Local CSV log (fallback and permanent record)

Append to a CSV file stored on the ESP32's internal flash (SPIFFS or LittleFS filesystem) simultaneously with every event and every daily summary. This is the permanent local record and the fallback if Google Sheets is unavailable.

File structure:
- `/data/events.csv` — one row per individual entry/exit event, with timestamp, type (ENTRY/EXIT), and running daily totals at time of event
- `/data/daily.csv` — one row per day, matching the Google Sheets row format above

The CSV files should survive a reboot (written to flash, not RAM). Implement log rotation: keep a maximum of 90 days of event data to avoid filling flash storage.

### Method 3 — Serial output (development and debugging)

Print all events and state changes to Serial at 115200 baud. This should remain in the production build (not stripped out) so that if something is behaving unexpectedly, plugging in a USB cable and opening a serial monitor immediately shows what the device is seeing. Prefix all serial output with a timestamp.

---

## Configuration

All tuneable values in a single config block at the top of the main file, clearly labelled. No magic numbers inline. The operator will not edit these after installation, but they need to be findable and changeable by someone following brief written instructions.

```cpp
// --- CONFIGURATION ---

// GPIO pins
#define PIR_OUTER_PIN 16      // Sensor facing the street
#define PIR_INNER_PIN 17      // Sensor facing the shop interior

// Timing (milliseconds)
#define WARMUP_MS         60000   // Boot warm-up period before counting starts
#define DETECTION_WINDOW_MS 400   // Max time between sensor triggers for a valid event
#define DEBOUNCE_MS        1000   // Ignore period after a valid event is registered

// Daily reset time (24hr)
#define DAILY_RESET_HOUR   22     // Hour at which daily totals are pushed and reset
#define DAILY_RESET_MINUTE  0

// Wi-Fi credentials
#define WIFI_SSID     "your_network_name"
#define WIFI_PASSWORD "your_network_password"

// Google Sheets
#define GSHEET_SHEET_ID   "your_sheet_id_here"
// Service account credentials loaded from /data/service_account.json on SPIFFS

// Serial
#define SERIAL_BAUD 115200
```

Sensitive credentials (Wi-Fi password, Google service account key) should be stored in a separate file that is clearly marked and not included in any code repository.

---

## Device Management

### Automatic operation

The device should require zero daily interaction once installed. Specifically:
- Auto-connect to Wi-Fi on boot with retry logic (retry every 30 seconds indefinitely, continue counting locally if Wi-Fi never connects)
- Auto-push daily summary at the configured time
- Auto-restart cleanly from a power cut — counts from before the power cut are already written to flash and are not lost; the device simply starts a new session

### Manual controls

The Olimex board has one user-programmable button. Assign it a single function: **manual daily reset and push**. Pressing and holding for 3 seconds triggers an immediate daily summary push (same as the midnight automatic push) and resets the day's counters. This allows the operator to manually mark end-of-day if closing early, or to test that the Google Sheets push is working.

### OTA updates

Not required for initial build. Do not add OTA infrastructure speculatively.

### Watchdog

Enable the ESP32's hardware watchdog timer. If the main loop stops executing (firmware hang), the device should reboot automatically within 30 seconds rather than sitting silently dead.

---

## Error Handling

**Wi-Fi connection failure:** Log to serial and local CSV. Continue counting. Retry connection in background. Do not block the counting loop waiting for Wi-Fi.

**Google Sheets push failure:** Log error to serial and local CSV with HTTP response code if available. Queue the failed push and retry next time Wi-Fi is available and connected. Do not lose data.

**Flash storage full:** Log a warning to serial. Stop writing to event log (which is verbose) but continue writing daily summaries (which are small). Do not crash.

**Both sensors triggering simultaneously (within <10ms of each other):** Discard the event — this is most likely a large group walking through together rather than a single countable entry. Log to serial as DISCARDED_SIMULTANEOUS for debugging. This is a known limitation of PIR-based directional counting.

**Sensor not responding after warm-up:** If either GPIO pin has not triggered at all within the first 10 minutes after warm-up (suggesting a possible wiring or sensor failure), log a WARNING to serial every 60 seconds. Do not alert the operator via any other channel in the initial build.

---

## Development and Testing

### Recommended IDE

Arduino IDE 2.x with the Espressif ESP32 Arduino core installed (Board: "ESP32 Dev Module" or the Olimex-specific entry if available in the board manager — either works since this board is pin-compatible with DevKitC).

### Initial hardware test (before full firmware)

Before building the full firmware, write and flash a minimal test sketch first:
- Print to serial whenever GPIO 16 goes HIGH: `"PIR OUTER triggered"`
- Print to serial whenever GPIO 17 goes HIGH: `"PIR INNER triggered"`
- No counting logic, no timing, no Wi-Fi

This confirms both sensors are wired correctly and both GPIOs are reading before the full state machine is layered on top. Do not skip this step.

### Tuning the detection window

After the full firmware is flashed, the detection window (default 400ms) will likely need adjustment for the specific doorway. To tune it:
1. Enable verbose serial output (already on by default per Method 3 above)
2. Walk through the doorway at normal pace several times
3. Note the logged time delta between the two sensor triggers
4. Set DETECTION_WINDOW_MS to approximately 1.5× the largest observed delta to give margin without being so wide it catches false sequences

---

## Acceptance Criteria

The build is complete when:

1. Walking through the doorway in either direction registers correctly as ENTRY or EXIT in the serial output
2. Walking through 10 times in a row in the same direction registers exactly 10 events (no double-counts, no misses at normal walking pace)
3. Standing still in the doorway for 5 seconds does not register any events
4. A power cycle does not lose the current day's count (data survives reboot from flash)
5. At the configured daily reset time, a row is appended to the Google Sheet with the correct date and counts
6. The device boots, connects to Wi-Fi, and begins counting without any human interaction after being plugged in

---

## Out of Scope (do not build)

- Web dashboard or local HTTP server
- Multiple doorway support
- Camera or computer vision
- Heatmap or dwell time tracking
- Any form of personal data capture
- OTA firmware updates
- SMS or email alerts
- Integration with the shop's till or HubSpot systems
