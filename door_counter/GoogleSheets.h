/*
 * GoogleSheets.h — Append rows to a Google Sheet using a service-account key.
 *
 * No third-party Google/OAuth library is used (per PRD): the JWT is built and
 * RS256-signed in-firmware with mbedTLS (bundled in the ESP32 core), exchanged
 * for an OAuth2 access token, and the Sheets v4 "values:append" endpoint is
 * called over HTTPS via WiFiClientSecure.
 *
 * The service-account credentials are read from LittleFS
 * (SERVICE_ACCOUNT_FILE). Time-of-day must be NTP-synced before tokens can be
 * minted (JWT iat/exp are real epoch seconds).
 */

#ifndef GOOGLE_SHEETS_H
#define GOOGLE_SHEETS_H

#include <Arduino.h>
#include "Types.h"

namespace GoogleSheets {

// Load and parse the service-account JSON from LittleFS. Call once after
// Storage::begin(). Returns false if the file is missing/unparseable.
bool begin();

// True if a usable service account was loaded.
bool isConfigured();

// Append one daily row to the sheet. Column order matches Storage::DAILY_HEADER.
// Returns true on HTTP 200. `httpCodeOut` (optional) receives the HTTP status
// (or a negative client error).
bool appendDailyRow(const char* date, const char* dow, const DayCounters& day,
                    const char* notes, int* httpCodeOut = nullptr);

// Append a row whose cells are an already-CSV-encoded line (used to replay the
// offline retry queue). Returns true on HTTP 200.
bool appendCsvRow(const String& csvLine, int* httpCodeOut = nullptr);

}  // namespace GoogleSheets

#endif  // GOOGLE_SHEETS_H
