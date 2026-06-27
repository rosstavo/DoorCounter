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
struct DayCounters {
  char    date[11];        // "YYYY-MM-DD" of the day these counters belong to
  uint32_t entries;        // total entries today
  uint32_t exits;          // total exits today
  char    openingTime[9];  // "HH:MM:SS" of first entry, or "" if none yet
  char    closingTime[9];  // "HH:MM:SS" of last exit, or "" if none yet
  bool    valid;           // false until we have a confirmed date from NTP

  int32_t net() const { return (int32_t)entries - (int32_t)exits; }
};

#endif  // TYPES_H
