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
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include "esp_task_wdt.h"

#include "config.h"
#include "secrets.h"
#include "Types.h"
#include "Detector.h"
#include "Storage.h"
#include "GoogleSheets.h"

// ---------------------------------------------------------------------------
// Detection state machine
// ---------------------------------------------------------------------------
// The logic itself lives in Detector.h, shared verbatim with tuner/tuner.ino
// so the web tuner dials in the same state machine that ships here. Its
// timings are loaded from config.h in setup().
static Detect::Detector s_det;

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

// Sensor health (whether each PIR has ever fired is tracked by s_det)
static uint32_t s_lastHealthWarnMs = 0;

// Button
static bool     s_btnPressed = false;
static uint32_t s_btnPressStartMs = 0;
static bool     s_btnHandled = false;
// GPIO34 is input-only (no internal pull-up); if it floats / reads "pressed" at
// boot we must not fire a push until the button has been seen released once.
static bool     s_btnReleasedOnce = false;

// Status web page
static WebServer s_http(STATUS_HTTP_PORT);
static bool     s_mdnsStarted = false;
static char     s_lastEvent[40] = "";  // e.g. "Entry at 2026-09-13 10:32:05"
static char     s_lastPush[80] = "";   // outcome of the most recent Sheets push

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

// True if the clock says we are in the afternoon half of the day. With no
// synced clock we cannot place an event, so it falls into the AM half.
static bool nowIsPm() {
  struct tm tmv;
  if (!getLocalTimeStruct(tmv)) return false;
  return tmv.tm_hour >= PM_START_HOUR;
}

// Three-letter day name for an ISO "YYYY-MM-DD" date. Derived from the date
// itself, not from "now", so a push that lands after midnight still labels the
// day it is reporting on. Empty string if the date is not a real one.
static void dayOfWeek(const char* isoDate, char* out, size_t len) {
  int y = 0, m = 0, d = 0;
  if (sscanf(isoDate, "%d-%d-%d", &y, &m, &d) != 3 || y < 1970) {
    if (len) out[0] = '\0';
    return;
  }
  // days_from_civil: days since 1970-01-01, which was a Thursday.
  y -= (m <= 2);
  int era = y / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  long days = (long)era * 146097L + (long)doe - 719468L;
  int wd = (int)((days % 7 + 10) % 7);  // 0 = Monday
  static const char* kNames[7] = {"Mon", "Tue", "Wed", "Thu",
                                  "Fri", "Sat", "Sun"};
  snprintf(out, len, "%s", kNames[wd]);
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

  char dow[4];
  dayOfWeek(date, dow, sizeof(dow));

  slog("DAILY PUSH (%s): %s %s entries=%u exits=%u net=%ld open=%s close=%s",
       reason, date, dow, g_day.entries, g_day.exits, (long)g_day.net(),
       g_day.openingTime, g_day.closingTime);
  slog("  AM entries=%u exits=%u | PM entries=%u exits=%u", g_day.amEntries,
       g_day.amExits, g_day.pmEntries, g_day.pmExits);

  // 1) Permanent local record (always).
  Storage::logDaily(date, dow, g_day, notes);

  // 2) Google Sheets (or queue for retry).
  bool pushed = false;
  char ts[24];
  nowTimestamp(ts, sizeof(ts));
  if (GoogleSheets::isConfigured() && WiFi.status() == WL_CONNECTED) {
    int code = 0;
    pushed = GoogleSheets::appendDailyRow(date, dow, g_day, notes, &code);
    if (pushed) {
      snprintf(s_lastPush, sizeof(s_lastPush), "Sent OK at %s", ts);
    } else {
      slog("Google Sheets push failed (HTTP %d) — queuing", code);
      snprintf(s_lastPush, sizeof(s_lastPush),
               "Failed (HTTP %d) at %s — will retry", code, ts);
    }
  } else {
    slog("Wi-Fi/Sheets unavailable — queuing daily push");
    snprintf(s_lastPush, sizeof(s_lastPush),
             "No Wi-Fi/Sheets at %s — will retry", ts);
  }
  if (!pushed) {
    // Queue a CSV row matching the sheet column order (Storage::DAILY_HEADER).
    char row[200];
    snprintf(row, sizeof(row), "%s,%u,%u,%ld,%s,%s,%s,%s,%u,%u,%u,%u", date,
             g_day.entries, g_day.exits, (long)g_day.net(), g_day.openingTime,
             g_day.closingTime, notes, dow, g_day.amEntries, g_day.amExits,
             g_day.pmEntries, g_day.pmExits);
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

  bool pm = nowIsPm();

  if (type == EVENT_ENTRY) {
    g_day.entries++;
    if (pm) g_day.pmEntries++; else g_day.amEntries++;
    if (g_day.openingTime[0] == '\0' && clk[0] != '\0')
      strncpy(g_day.openingTime, clk, sizeof(g_day.openingTime) - 1);
  } else {
    g_day.exits++;
    if (pm) g_day.pmExits++; else g_day.amExits++;
    if (clk[0] != '\0')
      strncpy(g_day.closingTime, clk, sizeof(g_day.closingTime) - 1);
  }

  snprintf(s_lastEvent, sizeof(s_lastEvent), "%s at %s",
           type == EVENT_ENTRY ? "Entry" : "Exit", ts);

  Storage::logEvent(ts, type, g_day);
  Storage::saveDay(g_day);

  slog("%s registered (%s). entries=%u exits=%u net=%ld",
       type == EVENT_ENTRY ? "ENTRY" : "EXIT", pm ? "PM" : "AM", g_day.entries,
       g_day.exits, (long)g_day.net());
}

// ---------------------------------------------------------------------------
// Sensor state machine
// ---------------------------------------------------------------------------

static void serviceSensors() {
  Detect::Outcome o =
      s_det.update(millis(), digitalRead(PIR_OUTER_PIN), digitalRead(PIR_INNER_PIN));

  switch (o.result) {
    case Detect::ENTRY:
      slog("Valid sequence OUTER->INNER (delta %lums)", (unsigned long)o.deltaMs);
      registerEvent(EVENT_ENTRY);
      break;
    case Detect::EXIT:
      slog("Valid sequence INNER->OUTER (delta %lums)", (unsigned long)o.deltaMs);
      registerEvent(EVENT_EXIT);
      break;
    case Detect::DISCARDED_NOISE:
      slog("DISCARDED_NOISE (single %s trigger, no follow-up)",
           o.firstWasOuter ? "OUTER" : "INNER");
      break;
    case Detect::DISCARDED_SIMULTANEOUS:
      if (o.deltaMs == 0) {
        slog("DISCARDED_SIMULTANEOUS (both sensors fired together)");
      } else {
        slog("DISCARDED_SIMULTANEOUS (delta %lums < %lums)",
             (unsigned long)o.deltaMs, (unsigned long)s_det.cfg.simultaneousMs);
      }
      break;
    case Detect::NONE:
      break;
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
  if (!s_mdnsStarted && MDNS.begin(STATUS_HOSTNAME)) {
    MDNS.addService("http", "tcp", STATUS_HTTP_PORT);
    s_mdnsStarted = true;
  }
  slog("Status page: http://%s.local/ or http://%s/", STATUS_HOSTNAME,
       WiFi.localIP().toString().c_str());
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
        slog("Restored today's counters: entries=%u exits=%u (AM %u/%u, PM %u/%u)",
             g_day.entries, g_day.exits, g_day.amEntries, g_day.amExits,
             g_day.pmEntries, g_day.pmExits);
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
        char ts[24];
        nowTimestamp(ts, sizeof(ts));
        snprintf(s_lastPush, sizeof(s_lastPush),
                 "Retry failed (HTTP %d) at %s — will retry", code, ts);
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
    char ts[24];
    nowTimestamp(ts, sizeof(ts));
    snprintf(s_lastPush, sizeof(s_lastPush), "Queued row(s) sent OK at %s", ts);
  }
}

// ---------------------------------------------------------------------------
// Status web page
// ---------------------------------------------------------------------------
// Read-only: it shows state but has no controls, so anyone on the LAN can look
// without being able to reset or push the day.

static void htmlRow(String& h, const char* label, const String& value) {
  h += "<tr><th>";
  h += label;
  h += "</th><td>";
  h += value;
  h += "</td></tr>";
}

static void handleStatusPage() {
  uint32_t now = millis();
  char buf[64];

  String h;
  h.reserve(2600);
  h += "<!doctype html><html><head><meta charset=utf-8>"
       "<meta name=viewport content='width=device-width,initial-scale=1'>";
  snprintf(buf, sizeof(buf), "<meta http-equiv=refresh content=%d>", STATUS_REFRESH_S);
  h += buf;
  h += "<title>Door counter</title><style>"
       "body{font:16px system-ui,sans-serif;margin:0 auto;padding:16px;max-width:480px;"
       "background:#faf9f6;color:#222}"
       "h1{font-size:20px;margin:0 0 4px}"
       ".ok{color:#1a7f37}.warn{color:#b35900}"
       ".big{display:flex;gap:8px;margin:16px 0}"
       ".big div{flex:1;background:#fff;border:1px solid #ddd;border-radius:8px;"
       "padding:10px;text-align:center}"
       ".big b{display:block;font-size:32px}"
       "table{width:100%;border-collapse:collapse}"
       "th,td{text-align:left;padding:6px 4px;border-top:1px solid #e5e5e5;"
       "vertical-align:top}th{font-weight:500;color:#666;width:42%}"
       "</style></head><body><h1>Station Books door counter</h1>";

  if (s_counting) {
    h += "<div class=ok>&#9679; Counting</div>";
  } else {
    snprintf(buf, sizeof(buf),
             "<div class=warn>&#9679; Warming up — %lus left</div>",
             (unsigned long)(now < s_warmupEndMs ? (s_warmupEndMs - now) / 1000 : 0));
    h += buf;
  }

  h += "<div class=big>";
  snprintf(buf, sizeof(buf), "<div><b>%u</b>In</div>", g_day.entries);
  h += buf;
  snprintf(buf, sizeof(buf), "<div><b>%u</b>Out</div>", g_day.exits);
  h += buf;
  snprintf(buf, sizeof(buf), "<div><b>%ld</b>Inside</div>", (long)g_day.net());
  h += buf;
  h += "</div><table>";

  htmlRow(h, "Day", g_day.date[0] ? String(g_day.date) : String("not set yet"));
  snprintf(buf, sizeof(buf), "%u in / %u out", g_day.amEntries, g_day.amExits);
  htmlRow(h, "Morning", buf);
  snprintf(buf, sizeof(buf), "%u in / %u out", g_day.pmEntries, g_day.pmExits);
  htmlRow(h, "Afternoon", buf);
  htmlRow(h, "Last person", s_lastEvent[0] ? String(s_lastEvent) : String("none yet"));
  htmlRow(h, "First entry", g_day.openingTime[0] ? String(g_day.openingTime) : String("—"));
  htmlRow(h, "Last exit", g_day.closingTime[0] ? String(g_day.closingTime) : String("—"));

  htmlRow(h, "Outer sensor", s_det.outerEverFired() ? "has fired" : "<span class=warn>not fired yet</span>");
  htmlRow(h, "Inner sensor", s_det.innerEverFired() ? "has fired" : "<span class=warn>not fired yet</span>");

  snprintf(buf, sizeof(buf), "%s (signal %d)", WiFi.SSID().c_str(), WiFi.RSSI());
  htmlRow(h, "Wi-Fi", buf);
  nowTimestamp(buf, sizeof(buf));
  htmlRow(h, "Clock", s_timeValid ? String(buf) : String("<span class=warn>not synced</span>"));

  snprintf(buf, sizeof(buf), "%02d:%02d daily", DAILY_RESET_HOUR, DAILY_RESET_MINUTE);
  htmlRow(h, "Sheets push", buf);
  htmlRow(h, "Last push", s_lastPush[0] ? String(s_lastPush) : String("none since boot"));
  htmlRow(h, "Waiting to send",
          Storage::hasQueuedPushes() ? "<span class=warn>yes</span>" : "nothing");

  uint32_t up = now / 1000;
  snprintf(buf, sizeof(buf), "%lud %luh %lum", (unsigned long)(up / 86400),
           (unsigned long)(up / 3600 % 24), (unsigned long)(up / 60 % 60));
  htmlRow(h, "Up for", buf);

  h += "</table></body></html>";
  s_http.send(200, "text/html; charset=utf-8", h);
}

static void statusServerSetup() {
  s_http.on("/", HTTP_GET, handleStatusPage);
  s_http.onNotFound([] { s_http.send(404, "text/plain", "Not found"); });
  s_http.begin();
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
  if (s_det.outerEverFired() && s_det.innerEverFired()) return;
  if (now - s_lastHealthWarnMs < 60000) return;
  s_lastHealthWarnMs = now;
  if (!s_det.outerEverFired())
    slog("WARNING: PIR OUTER (GPIO %d) has not triggered — check wiring/sensor",
         PIR_OUTER_PIN);
  if (!s_det.innerEverFired())
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

  // Detection timings come from config.h — tune them with the web tuner
  // (web/README.md), which drives this same state machine.
  s_det.cfg.detectionWindowMs = DETECTION_WINDOW_MS;
  s_det.cfg.debounceMs        = DEBOUNCE_MS;
  s_det.cfg.simultaneousMs    = SIMULTANEOUS_MS;

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
  WiFi.setHostname(STATUS_HOSTNAME);
  WiFi.mode(WIFI_STA);
  statusServerSetup();
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
    s_det.begin(digitalRead(PIR_OUTER_PIN), digitalRead(PIR_INNER_PIN));
    slog("Warm-up complete — counting active.");
  }

  if (s_counting) serviceSensors();

  serviceButton();
  maintainDailyReset();
  maintainQueue();
  maintainSensorHealth();
  s_http.handleClient();

  delay(2);  // ~500Hz poll: fast enough to resolve 15-25cm sensor separation
}
