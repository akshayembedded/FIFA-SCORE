// =============================================================================
//  ESP32 Live Football Scoreboard  ->  ILI9488 480x320 TFT (TFT_eSPI)
//  Data: football-data.org v4 API  (free tier)
//
//  Edit src/config.h with your WiFi + API token before uploading.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#include "config.h"

// ----------------------------------------------------------------------------
//  Colours / layout
// ----------------------------------------------------------------------------
#define COL_BG        0x0000  // black
#define COL_HEADER    0x18E3  // dark slate
#define COL_TEXT      0xFFFF  // white
#define COL_DIM       0x8410  // grey
#define COL_LIVE      0xF800  // red
#define COL_HT        0xFC00  // orange
#define COL_FT        0x52AA  // muted blue-grey
#define COL_SCHED     0x07E0  // green
#define COL_ROW_ALT   0x10A2  // very dark blue (alternate rows)
#define COL_ACCENT    0x07FF  // cyan

static const int SCREEN_W   = 480;
static const int SCREEN_H   = 320;
static const int HEADER_H   = 42;
static const int ROW_H      = 38;
static const int ROW_TOP    = HEADER_H + 4;
static const int MAX_ROWS   = (SCREEN_H - ROW_TOP) / ROW_H;   // ~7 rows
static const int MAX_MATCH  = 12;

TFT_eSPI tft = TFT_eSPI();

// ----------------------------------------------------------------------------
//  Match model
// ----------------------------------------------------------------------------
struct Match {
  char home[5];     // TLA, e.g. "ARS"
  char away[5];
  char comp[6];     // competition code
  int  homeGoals;
  int  awayGoals;
  char status[12];  // raw status from API
  char kickoff[6];  // "HH:MM" local time
};

static Match matches[MAX_MATCH];
static int   matchCount = 0;
static char  statusLine[48] = "Starting...";

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------

// Copy a short team identifier into dst (max 4 chars). Prefers TLA, then a
// trimmed shortName/name fallback.
static void copyTeamId(JsonObjectConst team, char *dst) {
  const char *tla       = team["tla"]       | "";
  const char *shortName = team["shortName"] | "";
  const char *name      = team["name"]      | "";
  const char *src = tla[0] ? tla : (shortName[0] ? shortName : name);
  int i = 0;
  for (; src[i] && i < 4; i++) dst[i] = (char)toupper((unsigned char)src[i]);
  dst[i] = '\0';
  if (!dst[0]) strcpy(dst, "???");
}

// Convert a UTC ISO timestamp ("2026-06-29T18:00:00Z") to local "HH:MM".
static void utcToLocal(const char *utc, char *out) {
  out[0] = '\0';
  if (!utc || strlen(utc) < 16) { strcpy(out, "--:--"); return; }
  int h = (utc[11] - '0') * 10 + (utc[12] - '0');
  int m = (utc[14] - '0') * 10 + (utc[15] - '0');
  int total = h * 60 + m + TZ_OFFSET_MINUTES;
  total = ((total % 1440) + 1440) % 1440;
  snprintf(out, 6, "%02d:%02d", total / 60, total % 60);
}

// Returns a short status badge ("LIVE", "HT", "FT", or kickoff time) + colour.
static const char *badgeFor(const Match &m, uint16_t &colour) {
  if (!strcmp(m.status, "IN_PLAY"))   { colour = COL_LIVE;  return "LIVE"; }
  if (!strcmp(m.status, "PAUSED"))    { colour = COL_HT;    return "HT";   }
  if (!strcmp(m.status, "FINISHED"))  { colour = COL_FT;    return "FT";   }
  if (!strcmp(m.status, "POSTPONED")) { colour = COL_DIM;   return "PP";   }
  if (!strcmp(m.status, "SUSPENDED")) { colour = COL_DIM;   return "SUSP"; }
  if (!strcmp(m.status, "CANCELLED")) { colour = COL_DIM;   return "CANC"; }
  colour = COL_SCHED;                   // TIMED / SCHEDULED
  return m.kickoff;
}

static bool matchHasScore(const Match &m) {
  return strcmp(m.status, "TIMED") && strcmp(m.status, "SCHEDULED") &&
         strcmp(m.status, "POSTPONED") && strcmp(m.status, "CANCELLED");
}

// ----------------------------------------------------------------------------
//  Drawing
// ----------------------------------------------------------------------------
static void drawHeader(int secsToRefresh) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HEADER);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_ACCENT, COL_HEADER);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString("LIVE SCORES", 10, HEADER_H / 2);

  // Clock (from NTP) on the right
  char clk[6] = "--:--";
  struct tm t;
  if (getLocalTime(&t, 5)) snprintf(clk, sizeof(clk), "%02d:%02d", t.tm_hour, t.tm_min);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(COL_TEXT, COL_HEADER);
  tft.drawString(clk, SCREEN_W - 12, HEADER_H / 2);

  // Refresh countdown bar
  int barW = (int)((long)SCREEN_W * (REFRESH_SECONDS - secsToRefresh) / REFRESH_SECONDS);
  tft.fillRect(0, HEADER_H - 3, SCREEN_W, 3, COL_BG);
  tft.fillRect(0, HEADER_H - 3, barW, 3, COL_ACCENT);
}

static void drawMessage(const char *msg, uint16_t colour) {
  tft.fillRect(0, ROW_TOP, SCREEN_W, SCREEN_H - ROW_TOP, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextColor(colour, COL_BG);
  tft.drawString(msg, SCREEN_W / 2, (ROW_TOP + SCREEN_H) / 2);
}

static void drawRow(int idx, const Match &m) {
  int y  = ROW_TOP + idx * ROW_H;
  uint16_t rowBg = (idx & 1) ? COL_ROW_ALT : COL_BG;
  tft.fillRect(0, y, SCREEN_W, ROW_H, rowBg);

  int midY = y + ROW_H / 2;

  // Status badge (left)
  uint16_t badgeCol;
  const char *badge = badgeFor(m, badgeCol);
  tft.fillRoundRect(8, y + 6, 70, ROW_H - 12, 4, badgeCol);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_BG, badgeCol);
  tft.drawString(badge, 8 + 35, midY);

  // Competition code (small, under nothing - left of names)
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_DIM, rowBg);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString(m.comp, 90, midY);

  // Home team (right aligned toward centre)
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(COL_TEXT, rowBg);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(m.home, 230, midY);

  // Score or "vs" (centre)
  char score[12];
  if (matchHasScore(m)) snprintf(score, sizeof(score), "%d - %d", m.homeGoals, m.awayGoals);
  else                  strcpy(score, "vs");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(strcmp(m.status, "IN_PLAY") ? COL_TEXT : COL_LIVE, rowBg);
  tft.drawString(score, 270, midY);

  // Away team (left aligned from centre)
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(COL_TEXT, rowBg);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(m.away, 310, midY);
}

static void render(int secsToRefresh) {
  drawHeader(secsToRefresh);
  if (matchCount == 0) {
    drawMessage(statusLine, COL_DIM);
    return;
  }
  tft.fillRect(0, ROW_TOP, SCREEN_W, SCREEN_H - ROW_TOP, COL_BG);
  int rows = min(matchCount, MAX_ROWS);
  for (int i = 0; i < rows; i++) drawRow(i, matches[i]);
}

// ----------------------------------------------------------------------------
//  Networking
// ----------------------------------------------------------------------------
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  strcpy(statusLine, "Connecting to WiFi...");
  render(REFRESH_SECONDS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    // Kick off NTP for the on-screen clock / local kickoff math.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // we apply TZ offset ourselves
    setenv("TZ", "UTC0", 1);
    tzset();
    // Shift system clock to local by configuring the offset directly:
    configTime(TZ_OFFSET_MINUTES * 60, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    strcpy(statusLine, "WiFi failed - check config.h");
    render(REFRESH_SECONDS);
  }
}

// Builds the request URL from config.
static String buildUrl() {
  String url = "https://api.football-data.org/v4/matches";
  if (strlen(COMPETITIONS) > 0) url += String("?competitions=") + COMPETITIONS;
  return url;
}

// Fetch + parse today's matches. Returns true on success.
static bool fetchMatches() {
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); if (WiFi.status() != WL_CONNECTED) return false; }

  WiFiClientSecure client;
  client.setInsecure();                 // football-data.org cert not pinned
  HTTPClient https;
  https.setTimeout(12000);

  if (!https.begin(client, buildUrl())) {
    strcpy(statusLine, "HTTPS begin failed");
    return false;
  }
  https.addHeader("X-Auth-Token", API_TOKEN);

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    if (code == 429)      strcpy(statusLine, "Rate limited (10/min) - slow down");
    else if (code == 403) strcpy(statusLine, "403 - competition not in free tier");
    else if (code == 400) strcpy(statusLine, "400 - check API token / params");
    else                  snprintf(statusLine, sizeof(statusLine), "HTTP error %d", code);
    https.end();
    return false;
  }

  // Only pull the fields we actually render -> keeps RAM use small.
  JsonDocument filter;
  JsonObject fm = filter["matches"].add<JsonObject>();
  fm["status"] = true;
  fm["utcDate"] = true;
  fm["competition"]["code"] = true;
  fm["homeTeam"]["tla"] = true;
  fm["homeTeam"]["shortName"] = true;
  fm["homeTeam"]["name"] = true;
  fm["awayTeam"]["tla"] = true;
  fm["awayTeam"]["shortName"] = true;
  fm["awayTeam"]["name"] = true;
  fm["score"]["fullTime"]["home"] = true;
  fm["score"]["fullTime"]["away"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();

  if (err) {
    snprintf(statusLine, sizeof(statusLine), "JSON error: %s", err.c_str());
    return false;
  }

  JsonArrayConst arr = doc["matches"].as<JsonArrayConst>();
  matchCount = 0;
  for (JsonObjectConst mo : arr) {
    if (matchCount >= MAX_MATCH) break;
    Match &m = matches[matchCount];

    copyTeamId(mo["homeTeam"], m.home);
    copyTeamId(mo["awayTeam"], m.away);

    const char *st = mo["status"] | "SCHEDULED";
    strncpy(m.status, st, sizeof(m.status) - 1);
    m.status[sizeof(m.status) - 1] = '\0';

    const char *cc = mo["competition"]["code"] | "";
    strncpy(m.comp, cc, sizeof(m.comp) - 1);
    m.comp[sizeof(m.comp) - 1] = '\0';

    m.homeGoals = mo["score"]["fullTime"]["home"] | 0;
    m.awayGoals = mo["score"]["fullTime"]["away"] | 0;

    utcToLocal(mo["utcDate"] | "", m.kickoff);
    matchCount++;
  }

  if (matchCount == 0) strcpy(statusLine, "No matches today");
  return true;
}

// ----------------------------------------------------------------------------
//  Arduino entry points
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(COL_BG);

  connectWiFi();
  fetchMatches();
  render(REFRESH_SECONDS);
}

void loop() {
  static uint32_t lastFetch = 0;
  static uint32_t lastTick  = 0;
  uint32_t now = millis();

  // First fetch happens in setup(); thereafter every REFRESH_SECONDS.
  if (now - lastFetch >= (uint32_t)REFRESH_SECONDS * 1000UL) {
    lastFetch = now;
    fetchMatches();
    render(REFRESH_SECONDS);
  }

  // Update header (clock + countdown bar) once per second.
  if (now - lastTick >= 1000) {
    lastTick = now;
    int elapsed = (now - lastFetch) / 1000;
    drawHeader(REFRESH_SECONDS - elapsed);
  }
}
