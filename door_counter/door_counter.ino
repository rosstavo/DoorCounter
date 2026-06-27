/*
 * door_counter.ino — Station Books Footfall Counter (main firmware)
 *
 * Dual-PIR doorway entry/exit counter for a small bookshop. Counts people in
 * and out of one doorway by the ORDER the two sensors trigger, logs to local
 * flash, and pushes a daily summary to Google Sheets.
 *
 * Hardware: Olimex ESP32-DevKit-LiPo + 2x HC-SR501-compatible PIR sensors.
 *   PIR OUTER (street side)  -> GPIO 16
 *   PIR INNER (shop side)    -> GPIO 17
 *   User button (BUT1)       -> GPIO 34   (hold 3s = manual end-of-day push)
 *
 * See README.md for wiring, setup and tuning. All tuneable values live in
 * config.h; secrets in secrets.h and /data/service_account.json.
 *
 * Board: "ESP32 Dev Module". Serial: 115200 baud.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_task_wdt.h"

#include "config.h"
#include "secrets.h"
#include "Types.h"
#include "Storage.h"
#include "GoogleSheets.h"

// ---------------------------------------------------------------------------
// Detection state machine
// ---------------------------------------------------------------------------
enum DetState { IDLE, ARMED };
static DetState  s_state = IDLE;
static uint8_t   s_armedSensor = 0;   // PIR_OUTER_PIN or PIR_INNER_PIN
static uint32_t  s_armedAtMs = 0;
static uint32_t  s_debounceUntilMs = 0;

static int s_prevOuter = LOW;
static int s_prevInner = LOW;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static DayCounters g_day;             // current trading day's tally
static uint32_t s_warmupEndMs = 0;    // millis() at which warm-up completes
static bool     s_counting = false;   // true once warm-up has elapsed

static bool     s_timeValid = false;  // NTP has produced a real clock
static bool     s_ntpStarted = false;
static bool     s_wifiWasConnected = false;
static uint32_t s_lastWifiAttemptMs = 0;
static uint8_t  s_wifiNetIdx = 0;     // which saved network we're currently trying
static uint32_t s_lastQueueFlushMs = 0;

// Daily-reset bookkeeping: the date string we already pushed-and-reset for.
static char s_lastResetDate[11] = "";

// Sensor health
static bool     s_outerEverFired = false;
static bool     s_innerEverFired = false;
static uint32_t s_lastHealthWarnMs = 0;

// Button
static bool     s_btnPressed = false;
static uint32_t s_btnPressStartMs = 0;
static bool     s_btnHandled = false;
// GPIO34 is input-only (no internal pull-up); if it floats / reads "pressed" at
// boot we must not fire a push until the button has been seen released once.
static bool     s_btnReleasedOnce = false;

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

// Fill `out` with the current local time; returns false if clock not synced.
static bool getLocalTimeStruct(struct tm& out) {
  if (!s_timeValid) return false;
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &out);
  return true;
}

// "YYYY-MM-DD HH:MM:SS" when synced, else "T+<seconds>" fallback (uptime).
static void nowTimestamp(char* buf, size_t len) {
  struct tm tmv;
  if (getLocalTimeStruct(tmv)) {
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tmv);
  } else {
    snprintf(buf, len, "T+%lu", (unsigned long)(millis() / 1000));
  }
}

static void todayDate(char* buf, size_t len) {
  struct tm tmv;
  if (getLocalTimeStruct(tmv)) {
    strftime(buf, len, "%Y-%m-%d", &tmv);
  } else {
    snprintf(buf, len, "0000-00-00");
  }
}

static void nowClock(char* buf, size_t len) {
  struct tm tmv;
  if (getLocalTimeStruct(tmv)) {
    strftime(buf, len, "%H:%M:%S", &tmv);
  } else {
    buf[0] = '\0';
  }
}

// Timestamped serial line.
static void slog(const char* fmt, ...) {
  char ts[24];
  nowTimestamp(ts, sizeof(ts));
  char msg[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("[%s] %s\n", ts, msg);
}

// ---------------------------------------------------------------------------
// Day lifecycle
// ---------------------------------------------------------------------------

static void resetDay(const char* date) {
  memset(&g_day, 0, sizeof(g_day));
  strncpy(g_day.date, date, sizeof(g_day.date) - 1);
  g_day.openingTime[0] = '\0';
  g_day.closingTime[0] = '\0';
  g_day.valid = (date[0] != '0');
  Storage::saveDay(g_day);
}

// Push the current day's summary to Google Sheets (queuing on failure), write
// it to the local daily log, then reset counters for the new day.
static void pushAndResetDay(const char* reason) {
  char todayStr[11];
  todayDate(todayStr, sizeof(todayStr));
  const char* date = g_day.valid ? g_day.date : todayStr;

  char notes[64];
  snprintf(notes, sizeof(notes), "%s", reason);

  slog("DAILY PUSH (%s): %s entries=%u exits=%u net=%ld open=%s close=%s",
       reason, date, g_day.entries, g_day.exits, (long)g_day.net(),
       g_day.openingTime, g_day.closingTime);

  // 1) Permanent local record (always).
  Storage::logDaily(date, g_day.entries, g_day.exits, g_day.openingTime,
                    g_day.closingTime, notes);

  // 2) Google Sheets (or queue for retry).
  bool pushed = false;
  if (GoogleSheets::isConfigured() && WiFi.status() == WL_CONNECTED) {
    int code = 0;
    pushed = GoogleSheets::appendDailyRow(date, g_day.entries, g_day.exits,
                                          g_day.openingTime, g_day.closingTime,
                                          notes, &code);
    if (!pushed) slog("Google Sheets push failed (HTTP %d) — queuing", code);
  } else {
    slog("Wi-Fi/Sheets unavailable — queuing daily push");
  }
  if (!pushed) {
    // Queue a CSV row matching the sheet column order.
    char row[160];
    snprintf(row, sizeof(row), "%s,%u,%u,%ld,%s,%s,%s", date, g_day.entries,
             g_day.exits, (long)g_day.net(), g_day.openingTime,
             g_day.closingTime, notes);
    Storage::queuePush(row);
  }

  // 3) Roll over to a fresh day.
  Storage::rotateEventLog();
  resetDay(todayStr);
  strncpy(s_lastResetDate, todayStr, sizeof(s_lastResetDate) - 1);
}

// ---------------------------------------------------------------------------
// Event registration
// ---------------------------------------------------------------------------

static void registerEvent(EventType type) {
  char ts[24];
  nowTimestamp(ts, sizeof(ts));
  char clk[9];
  nowClock(clk, sizeof(clk));

  if (type == EVENT_ENTRY) {
    g_day.entries++;
    if (g_day.openingTime[0] == '\0' && clk[0] != '\0')
      strncpy(g_day.openingTime, clk, sizeof(g_day.openingTime) - 1);
  } else {
    g_day.exits++;
    if (clk[0] != '\0')
      strncpy(g_day.closingTime, clk, sizeof(g_day.closingTime) - 1);
  }

  Storage::logEvent(ts, type, g_day);
  Storage::saveDay(g_day);

  slog("%s registered. entries=%u exits=%u net=%ld",
       type == EVENT_ENTRY ? "ENTRY" : "EXIT", g_day.entries, g_day.exits,
       (long)g_day.net());
}

// ---------------------------------------------------------------------------
// Sensor state machine
// ---------------------------------------------------------------------------

static void serviceSensors() {
  uint32_t now = millis();
  int outer = digitalRead(PIR_OUTER_PIN);
  int inner = digitalRead(PIR_INNER_PIN);
  bool outerRise = (outer == HIGH && s_prevOuter == LOW);
  bool innerRise = (inner == HIGH && s_prevInner == LOW);
  s_prevOuter = outer;
  s_prevInner = inner;

  if (outerRise) s_outerEverFired = true;
  if (innerRise) s_innerEverFired = true;

  // Debounce window after a registered event: swallow edges.
  if (now < s_debounceUntilMs) return;

  switch (s_state) {
    case IDLE:
      if (outerRise && innerRise) {
        slog("DISCARDED_SIMULTANEOUS (both sensors fired together)");
        s_debounceUntilMs = now + DEBOUNCE_MS;
      } else if (outerRise) {
        s_state = ARMED;
        s_armedSensor = PIR_OUTER_PIN;
        s_armedAtMs = now;
      } else if (innerRise) {
        s_state = ARMED;
        s_armedSensor = PIR_INNER_PIN;
        s_armedAtMs = now;
      }
      break;

    case ARMED: {
      uint32_t elapsed = now - s_armedAtMs;
      if (elapsed > DETECTION_WINDOW_MS) {
        // No follow-up — single-sensor noise.
        slog("DISCARDED_NOISE (single %s trigger, no follow-up)",
             s_armedSensor == PIR_OUTER_PIN ? "OUTER" : "INNER");
        s_state = IDLE;
        break;
      }
      bool otherRise =
          (s_armedSensor == PIR_OUTER_PIN) ? innerRise : outerRise;
      if (otherRise) {
        if (elapsed < SIMULTANEOUS_MS) {
          slog("DISCARDED_SIMULTANEOUS (delta %lums < %dms)",
               (unsigned long)elapsed, SIMULTANEOUS_MS);
        } else if (s_armedSensor == PIR_OUTER_PIN) {
          slog("Valid sequence OUTER->INNER (delta %lums)",
               (unsigned long)elapsed);
          registerEvent(EVENT_ENTRY);
        } else {
          slog("Valid sequence INNER->OUTER (delta %lums)",
               (unsigned long)elapsed);
          registerEvent(EVENT_EXIT);
        }
        s_state = IDLE;
        s_debounceUntilMs = now + DEBOUNCE_MS;
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi / NTP / queue
// ---------------------------------------------------------------------------

static void onWifiConnected() {
  slog("Wi-Fi connected: %s (RSSI %d)", WiFi.localIP().toString().c_str(),
       WiFi.RSSI());
  if (!s_ntpStarted) {
    configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
    s_ntpStarted = true;
    slog("NTP sync requested");
  }
}

// Saved networks, tried in order (e.g. home, then shop). The primary is always
// first so it connects instantly at its own location; alternates are only
// reached if the earlier ones aren't in range. Define WIFI_SSID_2 etc. in
// secrets.h to add more — slots are optional.
struct WifiCred { const char* ssid; const char* pass; };
static const WifiCred kWifiNetworks[] = {
  { WIFI_SSID, WIFI_PASSWORD },
#ifdef WIFI_SSID_2
  { WIFI_SSID_2, WIFI_PASSWORD_2 },
#endif
#ifdef WIFI_SSID_3
  { WIFI_SSID_3, WIFI_PASSWORD_3 },
#endif
};
static const uint8_t kWifiNetworkCount =
    sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);

static void maintainWifi() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !s_wifiWasConnected) onWifiConnected();
  if (!connected && s_wifiWasConnected) slog("Wi-Fi connection lost");
  s_wifiWasConnected = connected;

  if (connected) return;
  uint32_t now = millis();
  // Each WiFi.begin() is non-blocking; if the current network hasn't connected
  // within the per-attempt timeout, rotate to the next saved network. With one
  // network this just retries it; with several it cycles home/shop/... in turn.
  bool firstAttempt = (s_lastWifiAttemptMs == 0);
  if (firstAttempt || now - s_lastWifiAttemptMs >= WIFI_CONNECT_TIMEOUT_MS) {
    if (!firstAttempt && kWifiNetworkCount > 1) {
      s_wifiNetIdx = (s_wifiNetIdx + 1) % kWifiNetworkCount;
    }
    const WifiCred& net = kWifiNetworks[s_wifiNetIdx];
    slog("Attempting Wi-Fi connection to '%s'...", net.ssid);
    WiFi.disconnect();
    WiFi.begin(net.ssid, net.pass);
    s_lastWifiAttemptMs = now;
  }
}

// Confirm NTP has delivered a real time, set the day's date the first time.
static void maintainClock() {
  if (s_timeValid) return;
  time_t now = time(nullptr);
  if (now >= 1700000000) {
    s_timeValid = true;
    char todayStr[11];
    todayDate(todayStr, sizeof(todayStr));
    slog("Clock synced. Today is %s", todayStr);

    if (!g_day.valid) {
      // First valid clock since boot.
      if (g_day.date[0] && strcmp(g_day.date, todayStr) == 0) {
        g_day.valid = true;  // restored counters belong to today
        slog("Restored today's counters: entries=%u exits=%u", g_day.entries,
             g_day.exits);
      } else if (g_day.date[0] && g_day.date[0] != '0') {
        slog("Saved counters are from %s, not today (%s) — starting fresh",
             g_day.date, todayStr);
        resetDay(todayStr);
      } else {
        resetDay(todayStr);
      }
    }
  }
}

static void maintainQueue() {
  if (WiFi.status() != WL_CONNECTED || !s_timeValid) return;
  if (!GoogleSheets::isConfigured()) return;
  if (!Storage::hasQueuedPushes()) return;
  uint32_t now = millis();
  if (s_lastQueueFlushMs != 0 && now - s_lastQueueFlushMs < 30000) return;
  s_lastQueueFlushMs = now;

  String all;
  int n = Storage::readQueue(all);
  if (n == 0) return;
  slog("Flushing %d queued Google Sheets row(s)", n);

  // Try each line; only clear the queue if ALL succeed.
  bool allOk = true;
  int start = 0;
  while (start < all.length()) {
    int nl = all.indexOf('\n', start);
    String line =
        (nl < 0) ? all.substring(start) : all.substring(start, nl);
    line.trim();
    if (line.length()) {
      int code = 0;
      if (!GoogleSheets::appendCsvRow(line, &code)) {
        slog("Queued row failed (HTTP %d) — will retry later", code);
        allOk = false;
        break;
      }
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  if (allOk) {
    Storage::clearQueue();
    slog("Queue flushed");
  }
}

// ---------------------------------------------------------------------------
// Daily reset trigger
// ---------------------------------------------------------------------------

static void maintainDailyReset() {
  struct tm tmv;
  if (!getLocalTimeStruct(tmv)) return;
  char todayStr[11];
  todayDate(todayStr, sizeof(todayStr));

  if (tmv.tm_hour == DAILY_RESET_HOUR && tmv.tm_min == DAILY_RESET_MINUTE &&
      strcmp(s_lastResetDate, todayStr) != 0) {
    pushAndResetDay("auto-daily");
  }
}

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------

static bool buttonIsPressed() {
  int raw = digitalRead(BUTTON_PIN);
#if BUTTON_ACTIVE_LOW
  return raw == LOW;
#else
  return raw == HIGH;
#endif
}

static void serviceButton() {
  uint32_t now = millis();
  bool pressed = buttonIsPressed();
  // Ignore a press that is already active at boot (e.g. a floating GPIO34):
  // only honour holds after we've observed at least one genuine release.
  if (!s_btnReleasedOnce) {
    if (!pressed) s_btnReleasedOnce = true;
    s_btnPressed = pressed;
    return;
  }
  if (pressed && !s_btnPressed) {
    s_btnPressed = true;
    s_btnPressStartMs = now;
    s_btnHandled = false;
  } else if (pressed && s_btnPressed && !s_btnHandled) {
    if (now - s_btnPressStartMs >= BUTTON_HOLD_MS) {
      s_btnHandled = true;
      slog("Button held %ds — manual end-of-day push", BUTTON_HOLD_MS / 1000);
      pushAndResetDay("manual-button");
    }
  } else if (!pressed && s_btnPressed) {
    s_btnPressed = false;
  }
}

// ---------------------------------------------------------------------------
// Sensor health watchdog
// ---------------------------------------------------------------------------

static void maintainSensorHealth() {
  if (!s_counting) return;
  uint32_t now = millis();
  if (now - s_warmupEndMs < SENSOR_SILENCE_WARN_MS) return;
  if (s_outerEverFired && s_innerEverFired) return;
  if (now - s_lastHealthWarnMs < 60000) return;
  s_lastHealthWarnMs = now;
  if (!s_outerEverFired)
    slog("WARNING: PIR OUTER (GPIO %d) has not triggered — check wiring/sensor",
         PIR_OUTER_PIN);
  if (!s_innerEverFired)
    slog("WARNING: PIR INNER (GPIO %d) has not triggered — check wiring/sensor",
         PIR_INNER_PIN);
}

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------

static void watchdogSetup() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = (uint32_t)WATCHDOG_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);  // core already inited the TWDT
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
#endif
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println("=== Station Books Footfall Counter ===");

  pinMode(PIR_OUTER_PIN, INPUT);
  pinMode(PIR_INNER_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT);

  watchdogSetup();

  if (!Storage::begin()) {
    slog("FATAL: storage init failed — counting will run but nothing persists");
  }

  // Restore any saved day (validated against the clock once NTP syncs).
  memset(&g_day, 0, sizeof(g_day));
  if (Storage::loadDay(g_day)) {
    g_day.valid = false;  // not yet confirmed to be 'today'
    slog("Loaded saved counters from %s: entries=%u exits=%u", g_day.date,
         g_day.entries, g_day.exits);
  } else {
    g_day.date[0] = '\0';
  }

  GoogleSheets::begin();

  // Kick off Wi-Fi (non-blocking; counting does not wait on it).
  WiFi.mode(WIFI_STA);
  maintainWifi();

  // Warm-up: PIRs are unreliable for the first 60s.
  s_warmupEndMs = millis() + WARMUP_MS;
  char bootTs[24];
  nowTimestamp(bootTs, sizeof(bootTs));
  Storage::logNote(bootTs, "BOOT");
  slog("Boot. Warm-up %ds before counting begins.", WARMUP_MS / 1000);
}

void loop() {
  esp_task_wdt_reset();

  maintainWifi();
  maintainClock();

  if (!s_counting && millis() >= s_warmupEndMs) {
    s_counting = true;
    // Initialise edge-detection baseline so a sensor sitting HIGH at the end of
    // warm-up does not register as a fresh rising edge.
    s_prevOuter = digitalRead(PIR_OUTER_PIN);
    s_prevInner = digitalRead(PIR_INNER_PIN);
    slog("Warm-up complete — counting active.");
  }

  if (s_counting) serviceSensors();

  serviceButton();
  maintainDailyReset();
  maintainQueue();
  maintainSensorHealth();

  delay(2);  // ~500Hz poll: fast enough to resolve 15-25cm sensor separation
}
