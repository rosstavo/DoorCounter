#include "GoogleSheets.h"
#include "config.h"
#include "secrets.h"  // GSHEET_SHEET_ID

#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/version.h"  // MBEDTLS_VERSION_MAJOR — API differs 2.x vs 3.x

namespace GoogleSheets {

// ---------------------------------------------------------------------------
// Root CA for *.googleapis.com / oauth2.googleapis.com.
//
// Leave empty to fall back to an unverified TLS connection (a serial warning is
// printed). For full verification, paste the "GTS Root R1" PEM here and set
// GSHEET_INSECURE 0. See README "TLS certificate".
// ---------------------------------------------------------------------------
static const char* GOOGLE_ROOT_CA = "";

static String s_clientEmail;
static String s_privateKeyPem;
static String s_tokenUri = "https://oauth2.googleapis.com/token";
static bool   s_configured = false;

// Cached OAuth token.
static String  s_accessToken;
static time_t  s_tokenExpiry = 0;

// --- small JSON string extractor (flat object, no nested quotes in values) ---
static bool jsonField(const String& src, const char* key, String& out) {
  // Match "key" then ':' with optional surrounding whitespace, then the opening
  // quote. Google key files are pretty-printed as "key": "value" (note space).
  String needle = String("\"") + key + "\"";
  int k = src.indexOf(needle);
  if (k < 0) return false;
  int i = k + needle.length();
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '\t')) i++;
  if (i >= (int)src.length() || src[i] != ':') return false;
  i++;  // past ':'
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '\t' ||
                                   src[i] == '\n' || src[i] == '\r')) i++;
  if (i >= (int)src.length() || src[i] != '"') return false;
  i++;  // past opening quote
  int j = i;
  // find the next unescaped quote
  while (j < (int)src.length()) {
    if (src[j] == '"' && src[j - 1] != '\\') break;
    j++;
  }
  if (j >= (int)src.length()) return false;
  out = src.substring(i, j);
  return true;
}

static void unescapeNewlines(String& s) {
  s.replace("\\n", "\n");
}

bool begin() {
  s_configured = false;
  if (!LittleFS.exists(SERVICE_ACCOUNT_FILE)) {
    Serial.printf("[GSHEET] %s not found — Google Sheets push disabled\n",
                  SERVICE_ACCOUNT_FILE);
    return false;
  }
  File f = LittleFS.open(SERVICE_ACCOUNT_FILE, FILE_READ);
  if (!f) return false;
  String json = f.readString();
  f.close();

  if (!jsonField(json, "client_email", s_clientEmail) ||
      !jsonField(json, "private_key", s_privateKeyPem)) {
    Serial.println("[GSHEET] service_account.json missing required fields");
    return false;
  }
  String tu;
  if (jsonField(json, "token_uri", tu) && tu.length() > 0) s_tokenUri = tu;
  unescapeNewlines(s_privateKeyPem);

  s_configured = true;
  Serial.printf("[GSHEET] Loaded service account: %s\n", s_clientEmail.c_str());
  return true;
}

bool isConfigured() { return s_configured; }

// --- base64url helpers ---
static String base64url(const uint8_t* data, size_t len) {
  size_t olen = 0;
  // mbedtls needs a buffer; 4/3 expansion + padding + nul.
  size_t bufLen = ((len + 2) / 3) * 4 + 1;
  uint8_t* buf = (uint8_t*)malloc(bufLen);
  if (!buf) return String();
  mbedtls_base64_encode(buf, bufLen, &olen, data, len);
  String s((char*)buf);
  free(buf);
  // URL-safe, strip padding
  s.replace("+", "-");
  s.replace("/", "_");
  s.replace("=", "");
  return s;
}

static String base64url(const String& in) {
  return base64url((const uint8_t*)in.c_str(), in.length());
}

// Sign `input` with the loaded RSA private key (RS256). Returns base64url sig,
// or empty on failure.
static String signRS256(const String& input) {
  mbedtls_pk_context pk;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_pk_init(&pk);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  String result;
  const char* pers = "footfall_jwt";

  int rc = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const uint8_t*)pers, strlen(pers));
  if (rc != 0) { Serial.println("[GSHEET] RNG seed failed"); goto done; }

  // Parse PEM private key. mbedTLS 3.x added an RNG parameter to this call.
#if MBEDTLS_VERSION_MAJOR >= 3
  rc = mbedtls_pk_parse_key(
      &pk, (const uint8_t*)s_privateKeyPem.c_str(),
      s_privateKeyPem.length() + 1, nullptr, 0,
      mbedtls_ctr_drbg_random, &ctr_drbg);
#else
  rc = mbedtls_pk_parse_key(
      &pk, (const uint8_t*)s_privateKeyPem.c_str(),
      s_privateKeyPem.length() + 1, nullptr, 0);
#endif
  if (rc != 0) {
    Serial.printf("[GSHEET] pk_parse_key failed: -0x%04x\n", -rc);
    goto done;
  }

  {
    uint8_t hash[32];
    rc = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    (const uint8_t*)input.c_str(), input.length(), hash);
    if (rc != 0) { Serial.println("[GSHEET] sha256 failed"); goto done; }

    uint8_t sig[512];  // enough for RSA-2048/4096
    size_t sigLen = 0;
    // mbedTLS 3.x added a sig buffer-size parameter to mbedtls_pk_sign.
#if MBEDTLS_VERSION_MAJOR >= 3
    rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig,
                         sizeof(sig), &sigLen, mbedtls_ctr_drbg_random,
                         &ctr_drbg);
#else
    rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig,
                         &sigLen, mbedtls_ctr_drbg_random, &ctr_drbg);
#endif
    if (rc != 0) {
      Serial.printf("[GSHEET] pk_sign failed: -0x%04x\n", -rc);
      goto done;
    }
    result = base64url(sig, sigLen);
  }

done:
  mbedtls_pk_free(&pk);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return result;
}

// Configure TLS trust on a client.
static void applyTls(WiFiClientSecure& client) {
#if GSHEET_INSECURE
  client.setInsecure();
#else
  if (GOOGLE_ROOT_CA && strlen(GOOGLE_ROOT_CA) > 0) {
    client.setCACert(GOOGLE_ROOT_CA);
  } else {
    Serial.println(
        "[GSHEET] WARNING: no root CA configured — using INSECURE TLS. "
        "Paste GTS Root R1 into GoogleSheets.cpp for verification.");
    client.setInsecure();
  }
#endif
}

// Mint (or reuse) an OAuth2 access token. Returns true if s_accessToken valid.
static bool ensureAccessToken() {
  time_t now = time(nullptr);
  if (now < 1700000000) {  // clock not yet NTP-synced (~2023+)
    Serial.println("[GSHEET] Clock not synced — cannot mint token yet");
    return false;
  }
  // Reuse with 60s safety margin.
  if (s_accessToken.length() > 0 && now < s_tokenExpiry - 60) return true;

  // Build JWT.
  String header = base64url(String("{\"alg\":\"RS256\",\"typ\":\"JWT\"}"));
  String claim = "{\"iss\":\"" + s_clientEmail +
                 "\",\"scope\":\"https://www.googleapis.com/auth/spreadsheets\","
                 "\"aud\":\"" + s_tokenUri + "\",\"iat\":" + String((long)now) +
                 ",\"exp\":" + String((long)(now + 3600)) + "}";
  String payload = header + "." + base64url(claim);
  String sig = signRS256(payload);
  if (sig.length() == 0) return false;
  String assertion = payload + "." + sig;

  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, s_tokenUri)) {
    Serial.println("[GSHEET] token request begin() failed");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body =
      "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer"
      "&assertion=" + assertion;
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code != 200) {
    Serial.printf("[GSHEET] token endpoint HTTP %d: %s\n", code, resp.c_str());
    return false;
  }
  String token;
  if (!jsonField(resp, "access_token", token)) {
    Serial.println("[GSHEET] no access_token in response");
    return false;
  }
  s_accessToken = token;
  s_tokenExpiry = now + 3600;
  Serial.println("[GSHEET] Access token obtained");
  return true;
}

// POST a values:append with the given JSON "values" array body fragment.
static bool appendValues(const String& valuesJsonArray, int* httpCodeOut) {
  if (!s_configured) {
    if (httpCodeOut) *httpCodeOut = -100;
    return false;
  }
  if (!ensureAccessToken()) {
    if (httpCodeOut) *httpCodeOut = -101;
    return false;
  }

  String url = "https://sheets.googleapis.com/v4/spreadsheets/" +
               String(GSHEET_SHEET_ID) + "/values/" + String(GSHEET_RANGE) +
               ":append?valueInputOption=USER_ENTERED";

  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, url)) {
    if (httpCodeOut) *httpCodeOut = -102;
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + s_accessToken);

  String body = "{\"values\":[" + valuesJsonArray + "]}";
  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  if (httpCodeOut) *httpCodeOut = code;

  if (code == 200) {
    Serial.println("[GSHEET] Row appended OK");
    return true;
  }
  // 401 → token may have been revoked; drop it so we re-mint next time.
  if (code == 401) s_accessToken = "";
  Serial.printf("[GSHEET] append HTTP %d: %s\n", code, resp.c_str());
  return false;
}

// JSON-escape a cell value.
static String jcell(const char* s) {
  String v(s);
  v.replace("\\", "\\\\");
  v.replace("\"", "\\\"");
  return "\"" + v + "\"";
}

bool appendDailyRow(const char* date, uint32_t entries, uint32_t exits,
                    const char* opening, const char* closing, const char* notes,
                    int* httpCodeOut) {
  long net = (long)((int32_t)entries - (int32_t)exits);
  String row = "[" + jcell(date) + "," + String(entries) + "," +
               String(exits) + "," + String(net) + "," + jcell(opening) + "," +
               jcell(closing) + "," + jcell(notes) + "]";
  return appendValues(row, httpCodeOut);
}

bool appendCsvRow(const String& csvLine, int* httpCodeOut) {
  // Split on commas (cell values here never contain commas — they are dates,
  // numbers, HH:MM:SS times and a controlled notes string).
  String row = "[";
  int start = 0;
  bool first = true;
  while (start <= csvLine.length()) {
    int comma = csvLine.indexOf(',', start);
    String cell =
        (comma < 0) ? csvLine.substring(start) : csvLine.substring(start, comma);
    if (!first) row += ",";
    row += jcell(cell.c_str());
    first = false;
    if (comma < 0) break;
    start = comma + 1;
  }
  row += "]";
  return appendValues(row, httpCodeOut);
}

}  // namespace GoogleSheets
