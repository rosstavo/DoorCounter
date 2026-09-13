/*
 * tuner.ino — Live tuning firmware for the Station Books footfall counter.
 *
 * Flash this INSTEAD of door_counter.ino while you dial in the detection
 * constants, then copy the numbers it gives you into door_counter/config.h and
 * flash the real firmware back.
 *
 * It runs the exact same detection state machine as the production firmware —
 * Detector.h is included from the door_counter sketch folder, not copied — but
 * with the timings changeable at runtime, and with everything the state machine
 * sees streamed out of the serial port as JSON lines for the web tuner
 * (see web/README.md).
 *
 * Deliberately absent: Wi-Fi, NTP, Google Sheets, LittleFS, the watchdog.
 * Nothing here writes to flash or the network; it is a bench instrument.
 *
 * Board: "ESP32 Dev Module". Serial: 115200 baud.
 *
 * --- Wire protocol -------------------------------------------------------
 * Out (one JSON object per line):
 *   {"t":"hello","fw":...,"outerPin":16,"innerPin":17,"cfg":{...}}
 *   {"t":"cfg","cfg":{"window":400,"debounce":1000,"simultaneous":10,
 *                     "warmup":5000}}
 *   {"t":"e","ms":123456,"s":"o"|"i","v":0|1}          edge (exact timestamp)
 *   {"t":"d","ms":..,"r":"entry"|"exit"|"noise"|"simul","dt":180,"f":"o"|"i"}
 *   {"t":"s","ms":..,"o":0,"i":0,"st":"idle"|"armed","af":0,"ao":"o",
 *            "deb":0,"warm":0,"up":12,"n":{"entry":0,"exit":0,"noise":0,
 *            "simul":0}}                                status, 10 Hz
 *   {"t":"log","m":"free text"}
 *
 * In (one plain-text command per line — also typeable in a serial monitor):
 *   set window <ms> | set debounce <ms> | set simultaneous <ms>
 *   set warmup <ms>
 *   get            re-send the current config
 *   reset          zero the counters and re-baseline the detector
 *   skipwarmup     end the warm-up right now
 *   ping           replies {"t":"log","m":"pong"}
 * -------------------------------------------------------------------------
 */

#include <Arduino.h>

// Shared with the production firmware — same state machine, same behaviour.
#include "../door_counter/Detector.h"

// Pins match config.h. Kept literal here so the tuner sketch stands alone and
// never drags in Wi-Fi/Sheets settings; if you move a sensor, change both.
#define PIR_OUTER_PIN 16
#define PIR_INNER_PIN 17
#define SERIAL_BAUD   115200

#define STATUS_INTERVAL_MS 100   // 10 Hz heartbeat / level resync
#define DEFAULT_WARMUP_MS 5000   // short by design: you are standing at the door

static Detect::Detector s_det;

static uint32_t s_warmupMs    = DEFAULT_WARMUP_MS;
static uint32_t s_warmupEndMs = 0;
static bool     s_counting    = false;
static uint32_t s_lastStatusMs = 0;

static uint32_t s_nEntry = 0, s_nExit = 0, s_nNoise = 0, s_nSimul = 0;

static char s_line[96];
static uint8_t s_lineLen = 0;

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static void emitLog(const char* msg) {
  Serial.printf("{\"t\":\"log\",\"m\":\"%s\"}\n", msg);
}

static void emitCfg() {
  Serial.printf(
      "{\"t\":\"cfg\",\"cfg\":{\"window\":%lu,\"debounce\":%lu,"
      "\"simultaneous\":%lu,\"warmup\":%lu}}\n",
      (unsigned long)s_det.cfg.detectionWindowMs,
      (unsigned long)s_det.cfg.debounceMs,
      (unsigned long)s_det.cfg.simultaneousMs, (unsigned long)s_warmupMs);
}

static void emitHello() {
  Serial.printf(
      "{\"t\":\"hello\",\"fw\":\"tuner-1\",\"outerPin\":%d,\"innerPin\":%d}\n",
      PIR_OUTER_PIN, PIR_INNER_PIN);
  emitCfg();
}

static void emitEdge(uint32_t ms, char sensor, int level) {
  Serial.printf("{\"t\":\"e\",\"ms\":%lu,\"s\":\"%c\",\"v\":%d}\n",
                (unsigned long)ms, sensor, level ? 1 : 0);
}

static void emitDetection(uint32_t ms, const char* result, uint32_t dt,
                          bool firstWasOuter) {
  Serial.printf("{\"t\":\"d\",\"ms\":%lu,\"r\":\"%s\",\"dt\":%lu,\"f\":\"%c\"}\n",
                (unsigned long)ms, result, (unsigned long)dt,
                firstWasOuter ? 'o' : 'i');
}

static void emitStatus(uint32_t now, int outer, int inner) {
  uint32_t warmLeft = (!s_counting && now < s_warmupEndMs) ? s_warmupEndMs - now : 0;
  bool armed = (s_det.state() == Detect::ARMED);
  Serial.printf(
      "{\"t\":\"s\",\"ms\":%lu,\"o\":%d,\"i\":%d,\"st\":\"%s\",\"af\":%lu,"
      "\"ao\":\"%c\",\"deb\":%lu,\"warm\":%lu,\"up\":%lu,"
      "\"n\":{\"entry\":%lu,\"exit\":%lu,\"noise\":%lu,\"simul\":%lu}}\n",
      (unsigned long)now, outer == HIGH ? 1 : 0, inner == HIGH ? 1 : 0,
      armed ? "armed" : "idle", (unsigned long)s_det.armedFor(now),
      s_det.armedIsOuter() ? 'o' : 'i',
      (unsigned long)s_det.debounceRemaining(now), (unsigned long)warmLeft,
      (unsigned long)(now / 1000), (unsigned long)s_nEntry,
      (unsigned long)s_nExit, (unsigned long)s_nNoise, (unsigned long)s_nSimul);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Clamp so a stray value from the browser can't wedge the state machine.
static uint32_t clampMs(long v, long lo, long hi) {
  if (v < lo) return (uint32_t)lo;
  if (v > hi) return (uint32_t)hi;
  return (uint32_t)v;
}

static void resetCounters() {
  s_nEntry = s_nExit = s_nNoise = s_nSimul = 0;
  s_det.begin(digitalRead(PIR_OUTER_PIN), digitalRead(PIR_INNER_PIN));
  emitLog("counters reset");
}

static void handleCommand(char* line) {
  uint32_t now = millis();
  while (*line == ' ') line++;
  if (!*line) return;

  if (strcmp(line, "get") == 0) {
    emitCfg();
  } else if (strcmp(line, "reset") == 0) {
    resetCounters();
  } else if (strcmp(line, "skipwarmup") == 0) {
    s_warmupEndMs = now;
    emitLog("warm-up skipped");
  } else if (strcmp(line, "ping") == 0) {
    emitLog("pong");
  } else if (strncmp(line, "set ", 4) == 0) {
    char key[24];
    long val = 0;
    if (sscanf(line + 4, "%23s %ld", key, &val) != 2) {
      emitLog("bad set command");
      return;
    }
    if (strcmp(key, "window") == 0) {
      s_det.cfg.detectionWindowMs = clampMs(val, 10, 5000);
    } else if (strcmp(key, "debounce") == 0) {
      s_det.cfg.debounceMs = clampMs(val, 0, 20000);
    } else if (strcmp(key, "simultaneous") == 0) {
      s_det.cfg.simultaneousMs = clampMs(val, 0, 1000);
    } else if (strcmp(key, "warmup") == 0) {
      s_warmupMs = clampMs(val, 0, 120000);
      // Re-arm warm-up from boot so shortening it can end it immediately.
      s_warmupEndMs = s_warmupMs;
      if (s_counting && now < s_warmupEndMs) s_counting = false;
    } else {
      emitLog("unknown setting");
      return;
    }
    emitCfg();
  } else {
    emitLog("unknown command");
  }
}

static void serviceSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      s_line[s_lineLen] = '\0';
      handleCommand(s_line);
      s_lineLen = 0;
    } else if (s_lineLen < sizeof(s_line) - 1) {
      s_line[s_lineLen++] = c;
    } else {
      s_lineLen = 0;  // overlong line: drop it rather than half-parse
      emitLog("command too long");
    }
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();

  pinMode(PIR_OUTER_PIN, INPUT);
  pinMode(PIR_INNER_PIN, INPUT);

  s_det.cfg.detectionWindowMs = 400;   // same defaults as config.h ships with
  s_det.cfg.debounceMs        = 1000;
  s_det.cfg.simultaneousMs    = 10;

  s_warmupEndMs = millis() + s_warmupMs;
  emitHello();
  emitLog("tuner ready - PIRs need ~60s from power-on to settle");
}

void loop() {
  serviceSerial();

  uint32_t now = millis();
  int outer = digitalRead(PIR_OUTER_PIN);
  int inner = digitalRead(PIR_INNER_PIN);

  if (!s_counting && now >= s_warmupEndMs) {
    s_counting = true;
    s_det.begin(outer, inner);   // baseline, so a HIGH pin isn't a fresh edge
    emitLog("warm-up complete - counting active");
  }

  if (s_counting) {
    Detect::Outcome o = s_det.update(now, outer, inner);

    if (o.outerRise) emitEdge(now, 'o', 1);
    if (o.outerFall) emitEdge(now, 'o', 0);
    if (o.innerRise) emitEdge(now, 'i', 1);
    if (o.innerFall) emitEdge(now, 'i', 0);

    switch (o.result) {
      case Detect::ENTRY:
        s_nEntry++;
        emitDetection(now, "entry", o.deltaMs, o.firstWasOuter);
        break;
      case Detect::EXIT:
        s_nExit++;
        emitDetection(now, "exit", o.deltaMs, o.firstWasOuter);
        break;
      case Detect::DISCARDED_NOISE:
        s_nNoise++;
        emitDetection(now, "noise", o.deltaMs, o.firstWasOuter);
        break;
      case Detect::DISCARDED_SIMULTANEOUS:
        s_nSimul++;
        emitDetection(now, "simul", o.deltaMs, o.firstWasOuter);
        break;
      case Detect::NONE:
        break;
    }
  }

  if (now - s_lastStatusMs >= STATUS_INTERVAL_MS) {
    s_lastStatusMs = now;
    emitStatus(now, outer, inner);
  }

  delay(2);  // ~500 Hz, matching the production loop's poll rate
}
