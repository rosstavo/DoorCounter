/*
 * Types.h — Shared data structures used across firmware modules.
 */

#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

// A single counted event.
enum EventType {
  EVENT_ENTRY,
  EVENT_EXIT
};

// The running tally for the current trading day.
//
// `entries`/`exits` are the authoritative totals. The AM/PM pairs are a
// best-effort split of those totals at PM_START_HOUR: an event counted while
// the clock is still unsynced lands in the AM half, because there is no
// time-of-day to place it by. They always sum back to the totals.
struct DayCounters {
  char    date[11];        // "YYYY-MM-DD" of the day these counters belong to
  uint32_t entries;        // total entries today
  uint32_t exits;          // total exits today
  uint32_t amEntries;      // entries before PM_START_HOUR
  uint32_t amExits;        // exits before PM_START_HOUR
  uint32_t pmEntries;      // entries at/after PM_START_HOUR
  uint32_t pmExits;        // exits at/after PM_START_HOUR
  char    openingTime[9];  // "HH:MM:SS" of first entry, or "" if none yet
  char    closingTime[9];  // "HH:MM:SS" of last exit, or "" if none yet
  bool    valid;           // false until we have a confirmed date from NTP

  int32_t net()   const { return (int32_t)entries - (int32_t)exits; }
  int32_t amNet() const { return (int32_t)amEntries - (int32_t)amExits; }
  int32_t pmNet() const { return (int32_t)pmEntries - (int32_t)pmExits; }
};

#endif  // TYPES_H
