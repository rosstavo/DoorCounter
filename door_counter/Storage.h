/*
 * Storage.h — LittleFS-backed persistence: event log, daily log, the
 * current-day counter state, and a retry queue for failed Google Sheets pushes.
 *
 * Responsibilities (PRD "Method 2 — Local CSV log" + persistence):
 *   - Append every event to /events.csv (survives reboot)
 *   - Append every daily summary to /daily.csv
 *   - Persist current-day counters to /today.json after every change so a
 *     power cut does not lose the day's count
 *   - Rotate the event log to EVENT_RETENTION_DAYS
 *   - Degrade gracefully when flash is nearly full
 *   - Hold rows that failed to reach Google Sheets and replay them later
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include "Types.h"

namespace Storage {

// Mount LittleFS (formats on first run) and ensure header rows exist.
// Returns false if the filesystem could not be mounted.
bool begin();

// True if free space is above FLASH_MIN_FREE_BYTES.
bool hasRoomForEvents();

// Append one event row to /events.csv. Skipped silently (with a serial
// warning) if flash is nearly full. `nowTs` is "YYYY-MM-DD HH:MM:SS" or a
// millis-based fallback string.
void logEvent(const char* nowTs, EventType type, const DayCounters& day);

// Append one fully-formed daily summary row to /daily.csv.
void logDaily(const char* dateStr, uint32_t entries, uint32_t exits,
              const char* openingTime, const char* closingTime,
              const char* notes);

// Append a free-form line to the event log (boot events, warnings). Best-effort.
void logNote(const char* nowTs, const char* note);

// --- Current-day counter persistence ---
bool loadDay(DayCounters& out);            // false if no saved state
void saveDay(const DayCounters& day);

// --- Google Sheets retry queue ---
// Each queued entry is one CSV-encoded daily row awaiting a successful push.
void queuePush(const char* csvRow);
bool hasQueuedPushes();
// Reads all queued rows into `out` (newline-separated). Returns count.
int  readQueue(String& out);
void clearQueue();

// Maintenance — call once per day after rollover.
void rotateEventLog();

}  // namespace Storage

#endif  // STORAGE_H
