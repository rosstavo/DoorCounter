/*
 * config.h — Non-sensitive tuneable configuration for the footfall counter.
 *
 * Everything an installer might reasonably need to change lives here or in
 * secrets.h. No magic numbers inline elsewhere in the firmware.
 *
 * Secrets (Wi-Fi password, Sheet ID, service-account key) live in secrets.h
 * and /data/service_account.json, which are NOT committed to the repository.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// GPIO pins
// ---------------------------------------------------------------------------
#define PIR_OUTER_PIN 16   // Sensor facing the street side of the threshold
#define PIR_INNER_PIN 17   // Sensor facing the shop interior side
#define BUTTON_PIN    34   // Olimex BUT1 user button (GPIO34, input-only)
#define BUTTON_ACTIVE_LOW 1  // BUT1 reads LOW when pressed

// ---------------------------------------------------------------------------
// Timing (milliseconds)
// ---------------------------------------------------------------------------
#define WARMUP_MS            60000  // Boot warm-up before counting starts
#define DETECTION_WINDOW_MS    400  // Max gap between sensor triggers for a valid event.
                                     // TUNE THIS during installation: ~1.5x the largest
                                     // observed walk-through delta. The live web tuner
                                     // (web/README.md) measures it for you and prints
                                     // this block ready to paste back.
#define DEBOUNCE_MS           1000  // Ignore window after a valid event is registered
#define SIMULTANEOUS_MS         10  // Triggers closer than this = discard (group/glitch)
#define BUTTON_HOLD_MS        3000  // Hold time for manual reset+push

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
// Each day's count is also split into a morning and an afternoon half. An
// event at or after this hour (local, 24h clock) counts as PM; earlier events
// count as AM. Set to the hour that best splits the shop's trading day.
#define PM_START_HOUR 13

// ---------------------------------------------------------------------------
// Daily reset / push time (24hr local time)
// ---------------------------------------------------------------------------
#define DAILY_RESET_HOUR     22
#define DAILY_RESET_MINUTE    0

// ---------------------------------------------------------------------------
// Time zone (NTP). Default: UK (GMT/BST with automatic DST).
// POSIX TZ string: https://github.com/nayarsystems/posix_tz_db
// ---------------------------------------------------------------------------
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define TIMEZONE     "GMT0BST,M3.5.0/1,M10.5.0"  // Europe/London

// ---------------------------------------------------------------------------
// Wi-Fi behaviour
// ---------------------------------------------------------------------------
#define WIFI_RETRY_INTERVAL_MS 30000  // Re-attempt connection every 30s, indefinitely
#define WIFI_CONNECT_TIMEOUT_MS 15000 // Per-attempt timeout

// ---------------------------------------------------------------------------
// Status web page (read-only; open http://door-counter.local/ on the same LAN)
// ---------------------------------------------------------------------------
#define STATUS_HOSTNAME    "door-counter"  // mDNS name and the name shown in the router
#define STATUS_HTTP_PORT   80
#define STATUS_REFRESH_S   10              // Page reloads itself this often

// ---------------------------------------------------------------------------
// Storage / logging
// ---------------------------------------------------------------------------
#define EVENTS_CSV    "/events.csv"
#define DAILY_CSV     "/daily.csv"
#define STATE_FILE    "/today.json"   // Persists current day's counters across reboot
#define QUEUE_FILE    "/gsheet_queue.csv"  // Pending Google Sheets rows awaiting retry
#define SERVICE_ACCOUNT_FILE "/service_account.json"

#define EVENT_RETENTION_DAYS  90      // Log rotation: keep at most this many days of events
#define FLASH_MIN_FREE_BYTES  20480   // Below this free space, stop writing the verbose
                                      // event log but keep writing daily summaries

// ---------------------------------------------------------------------------
// Google Sheets
// ---------------------------------------------------------------------------
// The tab/range to append to. "Sheet1" is the default first-tab name.
#define GSHEET_RANGE  "Default!A1"
// TLS verification. 0 = verify against bundled Google root CA (recommended).
//                   1 = skip verification (setInsecure) if you hit cert issues.
#define GSHEET_INSECURE 0

// ---------------------------------------------------------------------------
// Health checks
// ---------------------------------------------------------------------------
#define SENSOR_SILENCE_WARN_MS 600000  // 10 min: warn if a sensor never fired post-warmup
#define WATCHDOG_TIMEOUT_S     30       // Hardware watchdog reboot threshold

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------
#define SERIAL_BAUD 115200

#endif  // CONFIG_H
