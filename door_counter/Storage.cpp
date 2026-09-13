#include "Storage.h"
#include "config.h"
#include <LittleFS.h>

namespace Storage {

static const char* EVENTS_HEADER =
    "timestamp,type,entries_running,exits_running,net_running";
// New columns are appended on the right so that rows already in the Google
// Sheet keep their meaning.
const char* DAILY_HEADER =
    "date,total_entries,total_exits,net,opening_time,closing_time,notes,"
    "dow,am_entries,am_exits,pm_entries,pm_exits";

// Ensure a CSV exists and starts with the given header row.
static void ensureHeader(const char* path, const char* header) {
  if (!LittleFS.exists(path)) {
    File f = LittleFS.open(path, FILE_WRITE);
    if (f) {
      f.println(header);
      f.close();
    }
  }
}

// Rewrite the header of an existing CSV when the column list has grown, so a
// device flashed with an older build keeps its history under the right names.
// Rows written before the upgrade simply stop after the old last column.
static void upgradeHeader(const char* path, const char* header) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;
  String first = f.readStringUntil('\n');
  first.trim();
  if (first == header) {
    f.close();
    return;
  }
  Serial.printf("[STORAGE] Upgrading %s header (new columns)\n", path);
  File tmp = LittleFS.open("/csv.tmp", FILE_WRITE);
  if (!tmp) {
    f.close();
    return;
  }
  tmp.println(header);
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) tmp.println(line);
  }
  tmp.close();
  f.close();
  LittleFS.remove(path);
  LittleFS.rename("/csv.tmp", path);
}

bool begin() {
  // format-on-fail = true: first boot on a blank device formats the partition.
  if (!LittleFS.begin(true)) {
    Serial.println("[STORAGE] LittleFS mount failed");
    return false;
  }
  ensureHeader(EVENTS_CSV, EVENTS_HEADER);
  ensureHeader(DAILY_CSV, DAILY_HEADER);
  upgradeHeader(DAILY_CSV, DAILY_HEADER);
  Serial.printf("[STORAGE] LittleFS mounted. Used %u / %u bytes\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  return true;
}

bool hasRoomForEvents() {
  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  return freeBytes > FLASH_MIN_FREE_BYTES;
}

void logEvent(const char* nowTs, EventType type, const DayCounters& day) {
  if (!hasRoomForEvents()) {
    Serial.println("[STORAGE] WARNING: flash low — skipping event log write");
    return;
  }
  File f = LittleFS.open(EVENTS_CSV, FILE_APPEND);
  if (!f) {
    Serial.println("[STORAGE] ERROR: could not open events.csv for append");
    return;
  }
  f.printf("%s,%s,%u,%u,%ld\n", nowTs, type == EVENT_ENTRY ? "ENTRY" : "EXIT",
           day.entries, day.exits, (long)day.net());
  f.close();
}

void logDaily(const char* dateStr, const char* dow, const DayCounters& day,
              const char* notes) {
  File f = LittleFS.open(DAILY_CSV, FILE_APPEND);
  if (!f) {
    Serial.println("[STORAGE] ERROR: could not open daily.csv for append");
    return;
  }
  // Column order must match DAILY_HEADER (and the queued/Sheets row).
  f.printf("%s,%u,%u,%ld,%s,%s,%s,%s,%u,%u,%u,%u\n", dateStr, day.entries,
           day.exits, (long)day.net(), day.openingTime, day.closingTime, notes,
           dow, day.amEntries, day.amExits, day.pmEntries, day.pmExits);
  f.close();
}

void logNote(const char* nowTs, const char* note) {
  if (!hasRoomForEvents()) return;
  File f = LittleFS.open(EVENTS_CSV, FILE_APPEND);
  if (!f) return;
  f.printf("%s,NOTE,,,%s\n", nowTs, note);
  f.close();
}

// --- Current-day counter persistence (small hand-rolled JSON; no library) ---

void saveDay(const DayCounters& day) {
  File f = LittleFS.open(STATE_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("[STORAGE] ERROR: could not write today.json");
    return;
  }
  f.printf(
      "{\"date\":\"%s\",\"entries\":%u,\"exits\":%u,"
      "\"am_entries\":%u,\"am_exits\":%u,"
      "\"pm_entries\":%u,\"pm_exits\":%u,"
      "\"opening\":\"%s\",\"closing\":\"%s\"}\n",
      day.date, day.entries, day.exits, day.amEntries, day.amExits,
      day.pmEntries, day.pmExits, day.openingTime, day.closingTime);
  f.close();
}

// Minimal extractor for "key":"value" or "key":number from a flat JSON object.
static bool extractStr(const String& src, const char* key, char* out,
                       size_t outLen) {
  String needle = String("\"") + key + "\":\"";
  int i = src.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  int j = src.indexOf('"', i);
  if (j < 0) return false;
  String v = src.substring(i, j);
  strncpy(out, v.c_str(), outLen - 1);
  out[outLen - 1] = '\0';
  return true;
}

static bool extractNum(const String& src, const char* key, uint32_t& out) {
  String needle = String("\"") + key + "\":";
  int i = src.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  out = (uint32_t)src.substring(i).toInt();
  return true;
}

bool loadDay(DayCounters& out) {
  if (!LittleFS.exists(STATE_FILE)) return false;
  File f = LittleFS.open(STATE_FILE, FILE_READ);
  if (!f) return false;
  String s = f.readString();
  f.close();

  if (!extractStr(s, "date", out.date, sizeof(out.date))) return false;
  // Absent keys (state written by an older build) leave the field at zero.
  out.entries = out.exits = 0;
  out.amEntries = out.amExits = out.pmEntries = out.pmExits = 0;
  extractNum(s, "entries", out.entries);
  extractNum(s, "exits", out.exits);
  extractNum(s, "am_entries", out.amEntries);
  extractNum(s, "am_exits", out.amExits);
  extractNum(s, "pm_entries", out.pmEntries);
  extractNum(s, "pm_exits", out.pmExits);
  if (!extractStr(s, "opening", out.openingTime, sizeof(out.openingTime)))
    out.openingTime[0] = '\0';
  if (!extractStr(s, "closing", out.closingTime, sizeof(out.closingTime)))
    out.closingTime[0] = '\0';
  out.valid = true;
  return true;
}

// --- Google Sheets retry queue ---

void queuePush(const char* csvRow) {
  File f = LittleFS.open(QUEUE_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[STORAGE] ERROR: could not open queue file");
    return;
  }
  f.println(csvRow);
  f.close();
  Serial.println("[STORAGE] Push queued for later retry");
}

bool hasQueuedPushes() {
  if (!LittleFS.exists(QUEUE_FILE)) return false;
  File f = LittleFS.open(QUEUE_FILE, FILE_READ);
  if (!f) return false;
  bool nonEmpty = f.size() > 0;
  f.close();
  return nonEmpty;
}

int readQueue(String& out) {
  out = "";
  if (!LittleFS.exists(QUEUE_FILE)) return 0;
  File f = LittleFS.open(QUEUE_FILE, FILE_READ);
  if (!f) return 0;
  int count = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    out += line;
    out += '\n';
    count++;
  }
  f.close();
  return count;
}

void clearQueue() {
  if (LittleFS.exists(QUEUE_FILE)) LittleFS.remove(QUEUE_FILE);
}

// --- Log rotation ---
// Keep only rows whose date is within EVENT_RETENTION_DAYS of the newest row.
// We compare the leading "YYYY-MM-DD" lexicographically (ISO dates sort
// chronologically) against a cutoff computed by the caller would be cleaner,
// but to keep this self-contained we drop the oldest rows once the file grows
// beyond a line budget that corresponds to retention. Simpler + robust on
// flash: rewrite keeping the last N lines.
void rotateEventLog() {
  // Budget: assume worst case of one event line per ~80 bytes; cap the file at
  // a size proportional to retention to avoid unbounded growth. We keep the
  // header plus the most recent lines.
  const size_t MAX_EVENT_BYTES = (size_t)EVENT_RETENTION_DAYS * 1024 * 24;
  File f = LittleFS.open(EVENTS_CSV, FILE_READ);
  if (!f) return;
  if (f.size() <= MAX_EVENT_BYTES) {
    f.close();
    return;
  }
  Serial.println("[STORAGE] Rotating event log (size cap reached)");

  // Read all lines, keep header + the most recent half.
  String header;
  if (f.available()) header = f.readStringUntil('\n');
  // Skip ahead to roughly the second half of the data.
  size_t skipTo = f.size() / 2;
  f.seek(skipTo);
  f.readStringUntil('\n');  // discard partial line

  File tmp = LittleFS.open("/events.tmp", FILE_WRITE);
  if (!tmp) {
    f.close();
    return;
  }
  tmp.println(header);
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length()) tmp.println(line);
  }
  tmp.close();
  f.close();
  LittleFS.remove(EVENTS_CSV);
  LittleFS.rename("/events.tmp", EVENTS_CSV);
}

}  // namespace Storage
