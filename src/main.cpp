// =============================================================================
//  ESP32 Live Football Scoreboard  ->  ILI9488 480x320 TFT (TFT_eSPI)
//  Data: football-data.org v4 API  (free tier)
//
//  Edit src/config.h with your WiFi + API token before uploading.
//
//  Rendering is done with TFT_eSprite (off-screen buffers) for flicker-free
//  updates. A full 480x320 16bpp sprite would need ~307KB, which won't fit in
//  the ESP32's RAM, so we use two reusable region sprites instead: one for the
//  header bar and one row-height sprite that is redrawn and pushed per match.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <PNGdec.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "config.h"

// Touch is on hold until the hardware is sorted.
//  TOUCH_ENABLED 0 -> all touch polling is skipped, so there are NO phantom
//                     page switches and the serial log stays clean.
//  TOUCH_DEBUG   1 -> stream the raw XPT2046 point (touched/x/y/z) to serial.
// Set TOUCH_ENABLED to 1 once the touch panel works.
#define TOUCH_ENABLED 0
#define TOUCH_DEBUG   0

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
#define COL_CARD      0x0861  // live hero card background (near black)
#define COL_CARD_TOP  0x18E3  // card border
#define COL_FLAG_BG   0x4208  // flag placeholder tile
#define COL_GOLD      0xFEA0  // trophy gold accent
#define COL_BREAD     0x6C9F  // breadcrumb blue
#define COL_STAR      0x8410  // faint star outline

static const int SCREEN_W  = 480;
static const int SCREEN_H  = 320;
static const int HEADER_H  = 42;
static const int ROW_H     = 38;
static const int ROW_TOP   = HEADER_H + 4;
static const int MAX_ROWS  = (SCREEN_H - ROW_TOP) / ROW_H;   // ~7 rows
static const int MAX_MATCHES = 16;

TFT_eSPI    tft       = TFT_eSPI();
TFT_eSprite headerSpr = TFT_eSprite(&tft);   // 480 x HEADER_H
TFT_eSprite rowSpr    = TFT_eSprite(&tft);   // 480 x ROW_H, reused per row
static bool spritesOK = false;

// ----------------------------------------------------------------------------
//  Touch (XPT2046) on its OWN dedicated HSPI bus, SEPARATE from the display's
//  VSPI bus. Sharing the display bus gave frozen/garbage readings; a private
//  bus is the reliable arrangement (per the Bytes N Bits ILI9341 + XPT2046
//  tutorial / Arduino forum thread).
//
//  >>> REWIRE the 4 touch signal pins to these GPIOs (none shared with the
//      display). T_IRQ does NOT need wiring - we poll over SPI. <<<
//      T_CLK -> 25,  T_DIN(MOSI) -> 32,  T_DO(MISO) -> 39,  T_CS -> 33
// ----------------------------------------------------------------------------
#define T_CLK_PIN  25
#define T_MOSI_PIN 32
#define T_MISO_PIN 39
#define T_CS_PIN   33
SPIClass            touchSPI(HSPI);
XPT2046_Touchscreen ts(T_CS_PIN);     // poll mode (no IRQ pin needed)

// Raw-to-screen mapping (tune from TOUCH_DEBUG output). XPT2046 raw values run
// ~0..4095; these map the usable touch range onto the 480x320 panel.
static int gTouchMinX = 300, gTouchMaxX = 3800;
static int gTouchMinY = 300, gTouchMaxY = 3800;

// ----------------------------------------------------------------------------
//  Match model
// ----------------------------------------------------------------------------
struct Match {
  long id;          // football-data match id
  char home[5];     // TLA, e.g. "ARG"
  char away[5];
  char homeName[22];// full name, e.g. "South Africa"
  char awayName[22];
  time_t koEpoch;   // kick-off as UTC epoch (for the live clock)
  bool   secondHalf;// half-time score is in -> match is in/after the 2nd half
  char comp[6];     // competition code
  int  homeGoals;
  int  awayGoals;
  char status[12];  // raw status from API
  char utc[21];     // full UTC timestamp, used for sorting
  char kickoff[6];  // "HH:MM" local time
  char stage[16];   // e.g. GROUP_STAGE, LAST_16
  char group[10];   // e.g. GROUP_A (may be empty)
};

static Match matches[MAX_MATCHES];
static int   matchCount = 0;
static char  statusLine[48] = "Starting...";
static bool  gOnline = false;       // last data fetch reached the API
static bool  gSmoothFonts = false;  // Open Sans VLW fonts found on LittleFS

// Hero-card state (top featured match)
static bool  heroShown    = false;  // a hero card is currently on screen
static bool  heroIsLive   = false;  // that hero is an in-play match
static Match heroMatch;             // copy of the featured match (for the timer)

// Half-time anchor (Option D): when we observe the live PAUSED -> IN_PLAY
// transition we record the real second-half kick-off, making the 2nd-half
// minute exact instead of an estimate.
static long   gTrackedId    = -1;
static char   gPrevStatus[12] = "";
static time_t gSecondHalfKO = 0;
static time_t gLastPausedSeen = 0;  // wall-clock of the most recent PAUSED poll
static bool   gAnyLive = false;     // any match in play -> poll faster (see loop)

// ----------------------------------------------------------------------------
//  Pages (multi-screen). A footer nav bar at the bottom holds < and > arrows;
//  tapping them cycles through the screens. Touch comes from the XPT2046
//  controller (TOUCH_CS in User_Setup.h), calibrated once and saved to LittleFS.
// ----------------------------------------------------------------------------
enum Page { PAGE_FEATURED = 0, PAGE_ALL, PAGE_RESULTS, PAGE_COUNT };
static int gPage = PAGE_FEATURED;
static uint32_t gLastContentSig = 0;   // last painted content hash (see loop)
static int  gRenderedPage = -1;        // page currently on screen
static long gHeroDrawnId  = -1;        // id of the match whose card is on screen
static const char *kPageTitle[PAGE_COUNT] = { "FEATURED", "ALL MATCHES", "RESULTS" };

static const int FOOTER_H    = 40;
static const int FOOTER_Y    = SCREEN_H - FOOTER_H;    // 280
static const int CONTENT_TOP = HEADER_H + 4;           // 46
static const int CONTENT_BOT = FOOTER_Y - 2;           // 278
static const int CONTENT_H   = CONTENT_BOT - CONTENT_TOP;

// Hero card geometry (centred in the content band on the FEATURED page)
static const int HERO_X = 6;
static const int HERO_H = 158;
static const int HERO_Y = CONTENT_TOP + (CONTENT_H - HERO_H) / 2;
static const int HERO_W = SCREEN_W - 12;

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

// Copy a readable team name (prefers shortName, then full name) into dst.
static void copyTeamName(JsonObjectConst team, char *dst, size_t n) {
  const char *shortName = team["shortName"] | "";
  const char *name      = team["name"]      | "";
  const char *src = shortName[0] ? shortName : name;
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468;
}

// Parse a UTC ISO timestamp ("2026-06-29T18:00:00Z") to a UTC epoch.
static time_t isoToEpoch(const char *s) {
  if (!s || strlen(s) < 19) return 0;
  int Y = atoi(s), M = atoi(s + 5), D = atoi(s + 8);
  int h = atoi(s + 11), mi = atoi(s + 14), se = atoi(s + 17);
  return (time_t)(daysFromCivil(Y, M, D) * 86400L + h * 3600 + mi * 60 + se);
}

// Convert a UTC ISO timestamp ("2026-06-29T18:00:00Z") to local "HH:MM".
static void utcToLocal(const char *utc, char *out) {
  if (!utc || strlen(utc) < 16) { strcpy(out, "--:--"); return; }
  int h = (utc[11] - '0') * 10 + (utc[12] - '0');
  int m = (utc[14] - '0') * 10 + (utc[15] - '0');
  int total = h * 60 + m + TZ_OFFSET_MINUTES;
  total = ((total % 1440) + 1440) % 1440;
  snprintf(out, 6, "%02d:%02d", total / 60, total % 60);
}

static bool isLive(const Match &m) {
  return !strcmp(m.status, "IN_PLAY") || !strcmp(m.status, "PAUSED");
}
static bool isUpcoming(const Match &m) {
  return !strcmp(m.status, "TIMED") || !strcmp(m.status, "SCHEDULED");
}

// Sort priority: live (0) -> upcoming (1) -> finished/other (2).
static int statusRank(const Match &m) {
  if (isLive(m))     return 0;
  if (isUpcoming(m)) return 1;
  return 2;
}

// Order matches: live first, then soonest upcoming, then most-recent results.
static void sortMatches() {
  for (int i = 0; i < matchCount - 1; i++) {
    for (int j = 0; j < matchCount - 1 - i; j++) {
      Match &a = matches[j], &b = matches[j + 1];
      int ra = statusRank(a), rb = statusRank(b);
      bool swap;
      if (ra != rb) {
        swap = ra > rb;
      } else if (ra == 2) {
        swap = strcmp(a.utc, b.utc) < 0;   // finished: newest first
      } else {
        swap = strcmp(a.utc, b.utc) > 0;   // live/upcoming: earliest first
      }
      if (swap) { Match t = a; a = b; b = t; }
    }
  }
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
  return !isUpcoming(m) && strcmp(m.status, "POSTPONED") && strcmp(m.status, "CANCELLED");
}

// Build a human label for the round/group, e.g. "GROUP B" or "ROUND OF 16".
static void stageLabel(const Match &m, char *out, size_t n) {
  if (m.group[0]) {                       // "GROUP_A" -> "GROUP A"
    size_t i = 0;
    for (; m.group[i] && i < n - 1; i++) out[i] = (m.group[i] == '_') ? ' ' : m.group[i];
    out[i] = '\0';
    return;
  }
  const char *s = m.stage;
  if      (!strcmp(s, "GROUP_STAGE"))    strncpy(out, "GROUP STAGE",   n);
  else if (!strcmp(s, "LAST_32"))        strncpy(out, "ROUND OF 32",   n);
  else if (!strcmp(s, "LAST_16"))        strncpy(out, "ROUND OF 16",   n);
  else if (!strcmp(s, "QUARTER_FINALS")) strncpy(out, "QUARTER-FINAL", n);
  else if (!strcmp(s, "SEMI_FINALS"))    strncpy(out, "SEMI-FINAL",    n);
  else if (!strcmp(s, "THIRD_PLACE"))    strncpy(out, "3RD PLACE",     n);
  else if (!strcmp(s, "FINAL"))          strncpy(out, "FINAL",         n);
  else                                   strncpy(out, "WORLD CUP",     n);
  out[n - 1] = '\0';
}

// ----------------------------------------------------------------------------
//  Fonts
//  Smooth (anti-aliased) Open Sans VLW files live on LittleFS under /fonts.
//  setUiFont() selects the nearest size for direct-to-TFT text; if the VLW
//  files aren't present it falls back to the built-in FreeFonts so the UI still
//  renders. (Header/row sprites keep their own built-in fonts - independent.)
// ----------------------------------------------------------------------------
// Selects the nearest Open Sans size on the given target (tft or a sprite).
// Sprites keep their own font state, so the header sprite must be set too.
static void setUiFont(TFT_eSPI &g, int px) {
  if (gSmoothFonts) {
    g.unloadFont();
    const char *f = (px >= 30) ? "fonts/OpenSans36"
                  : (px >= 20) ? "fonts/OpenSans20"
                               : "fonts/OpenSans16";
    g.loadFont(f, LittleFS);
  } else {
    g.setFreeFont((px >= 30) ? &FreeSansBold24pt7b
                : (px >= 20) ? &FreeSansBold12pt7b
                             : &FreeSansBold9pt7b);
  }
}
static void setUiFont(int px) { setUiFont(tft, px); }

// ----------------------------------------------------------------------------
//  Flags  (PNG files on LittleFS, named /flags/<tla>.png, e.g. /flags/rsa.png)
//
//  The PNG is decoded into a native-size sprite, then cover-fit + circular-
//  masked into a round tile. Missing/undecodable files fall back to a lettered
//  disc, so unknown teams still render cleanly.
// ----------------------------------------------------------------------------
// The PNG decoder object reserves ~40KB, so we keep it on the heap and only
// allocate it while decoding a flag - otherwise it would starve the TLS buffers
// during a data fetch.
static PNG         *g_png = nullptr;
static TFT_eSprite *g_pngTarget = nullptr;   // sprite the decoder writes into

// PNGdec line callback: convert each scanline to native RGB565 and write it
// pixel-by-pixel into the target sprite. Using LITTLE_ENDIAN + drawPixel keeps
// the colour values native end-to-end (no pushImage byte-swap to get wrong).
static int pngDraw(PNGDRAW *pDraw) {
  static uint16_t lineBuf[200];
  if (!g_png || !g_pngTarget || pDraw->iWidth > 200) return 0;
  g_png->getLineAsRGB565(pDraw, lineBuf, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  for (int x = 0; x < pDraw->iWidth; x++)
    g_pngTarget->drawPixel(x, pDraw->y, lineBuf[x]);
  return 1;
}

// Decode /flags/<key>.png into spr. Returns true and sets w/h on success.
static bool decodeFlagPng(const char *key, TFT_eSprite &spr, uint16_t &w, uint16_t &h) {
  char path[40];
  snprintf(path, sizeof(path), "/flags/%s.png", key);
  fs::File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t sz = f.size();
  uint8_t *buf = (uint8_t *)malloc(sz);
  if (!buf) { f.close(); return false; }
  f.read(buf, sz);
  f.close();

  bool ok = false;
  int  rcOpen = -99, rcDec = -99;
  bool sprOk = false;
  g_png = new PNG();
  if (g_png) {
    rcOpen = g_png->openRAM(buf, sz, pngDraw);
    if (rcOpen == PNG_SUCCESS) {
      w = g_png->getWidth();
      h = g_png->getHeight();
      sprOk = (w && h && w <= 200 && spr.createSprite(w, h));
      if (sprOk) {
        g_pngTarget = &spr;
        rcDec = g_png->decode(nullptr, 0);
        ok = (rcDec == PNG_SUCCESS);
        g_pngTarget = nullptr;
      }
      g_png->close();
    }
    delete g_png;
    g_png = nullptr;
  }
  free(buf);
  Serial.printf("[FLAG] %s sz=%u open=%d w=%u h=%u spr=%d dec=%d -> %s\n",
                path, (unsigned)sz, rcOpen, w, h, sprOk, rcDec, ok ? "OK" : "FAIL");
  return ok;
}

// Circular flag: cx,cy = centre; d = diameter.
static void drawFlagCircle(int cx, int cy, int d, const char *tla) {
  int r = d / 2;
  char key[6] = {0};
  for (int i = 0; tla[i] && i < 5; i++) key[i] = (char)tolower((unsigned char)tla[i]);

  TFT_eSprite src(&tft);
  uint16_t sw = 0, sh = 0;
  bool haveImg = decodeFlagPng(key, src, sw, sh);

  TFT_eSprite dst(&tft);
  if (!dst.createSprite(d, d)) {            // last-resort direct fallback
    tft.fillCircle(cx, cy, r, COL_FLAG_BG);
    if (haveImg) src.deleteSprite();
    return;
  }
  dst.fillSprite(COL_CARD);

  // cover-fit scale so the flag fills the circle
  float scale = haveImg ? max((float)d / sw, (float)d / sh) : 1.0f;
  int offX = haveImg ? (int)(sw * scale - d) / 2 : 0;
  int offY = haveImg ? (int)(sh * scale - d) / 2 : 0;

  for (int y = 0; y < d; y++) {
    int dy2 = y - r;
    for (int x = 0; x < d; x++) {
      int dx2 = x - r;
      if (dx2 * dx2 + dy2 * dy2 > r * r) continue;   // outside circle
      uint16_t c = COL_FLAG_BG;
      if (haveImg) {
        int sx = (int)((x + offX) / scale), sy = (int)((y + offY) / scale);
        if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
        if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
        c = src.readPixel(sx, sy);
      }
      dst.drawPixel(x, y, c);
    }
  }
  dst.pushSprite(cx - r, cy - r, COL_CARD);   // COL_CARD treated as transparent
  dst.deleteSprite();
  if (haveImg) src.deleteSprite();

  if (!haveImg) {                              // letter the placeholder disc
    tft.setTextDatum(MC_DATUM);
    setUiFont(16);
    tft.setTextColor(COL_TEXT, COL_FLAG_BG);
    tft.drawString(tla, cx, cy);
  }
  tft.drawCircle(cx, cy, r, COL_CARD_TOP);     // subtle ring
}


// ----------------------------------------------------------------------------
//  Drawing (sprite based)
// ----------------------------------------------------------------------------

// Vertical centre of the score / timer block inside the hero card.
static const int HERO_SCORE_Y = HERO_Y + 78;
static const int HERO_TIMER_Y = HERO_Y + 116;

// Draws the red elapsed-time clock (mm:ss since kick-off) under the score.
// The free tier gives no match minute, so this is wall-clock since kick-off.
static void drawLiveTimer(bool force = false) {
  int cx = HERO_X + HERO_W / 2;

  // Build the label + colour first so we can skip repainting when it hasn't
  // changed (otherwise "HALF TIME" / the minute would flicker every second).
  char t[12];
  uint16_t col;
  if (!strcmp(heroMatch.status, "PAUSED")) {
    strcpy(t, "HALF TIME");
    col = COL_HT;
  } else if (heroMatch.secondHalf &&
             !(gTrackedId == heroMatch.id && gSecondHalfKO > 0)) {
    // 2nd half, but we never saw the restart (e.g. the board was switched on
    // mid-half). The free API gives no live minute, and "elapsed - 15" would
    // over-count by first-half stoppage + the real break length - that's how
    // 57' ends up shown as 64'. Print an honest label instead of a fake number.
    strcpy(t, "2ND HALF");
    col = COL_LIVE;
  } else {
    // Accurate minute:
    //  - 1st half: minutes since kick-off (no break yet)
    //  - 2nd half: 45 + time since the restart we anchored ourselves
    time_t now = time(nullptr);
    long mins = heroMatch.secondHalf
                  ? 45 + (long)((now - gSecondHalfKO) / 60)
                  : (long)((now - heroMatch.koEpoch) / 60);
    if (mins < 0) mins = 0;
    if      (!heroMatch.secondHalf && mins > 45) snprintf(t, sizeof(t), "45+'");
    else if ( heroMatch.secondHalf && mins > 90) snprintf(t, sizeof(t), "90+'");
    else                                         snprintf(t, sizeof(t), "%ld'", mins);
    col = COL_LIVE;
  }

  static char last[12] = "";
  if (!force && strcmp(t, last) == 0) return;   // unchanged -> don't repaint
  strcpy(last, t);

  tft.fillRect(cx - 70, HERO_TIMER_Y - 13, 140, 26, COL_CARD);   // clear
  tft.setTextDatum(MC_DATUM);
  setUiFont(20);
  tft.setTextColor(col, COL_CARD);
  tft.drawString(t, cx, HERO_TIMER_Y);
}

// STATIC part of the card: background, breadcrumb, circular flags, team names.
// These only change when the featured match itself changes, so we draw them
// once (the flag PNG decode is the slow bit) and leave them alone afterwards.
static void drawHeroStatic(const Match &m) {
  tft.fillRoundRect(HERO_X, HERO_Y, HERO_W, HERO_H, 8, COL_CARD);
  tft.drawRoundRect(HERO_X, HERO_Y, HERO_W, HERO_H, 8, COL_CARD_TOP);

  // Breadcrumb: "World > FIFA World Cup, <round>"
  char round[20];
  stageLabel(m, round, sizeof(round));
  char crumb[64];
  snprintf(crumb, sizeof(crumb), "World > FIFA World Cup, %s", round);
  tft.setTextDatum(TL_DATUM);
  setUiFont(16);
  tft.setTextColor(COL_BREAD, COL_CARD);
  tft.drawString(crumb, HERO_X + 14, HERO_Y + 12);

  // Circular flags
  int d = 70;
  int fcy = HERO_Y + 70;
  int hxc = HERO_X + 70;
  int axc = HERO_X + HERO_W - 70;
  drawFlagCircle(hxc, fcy, d, m.home);
  drawFlagCircle(axc, fcy, d, m.away);

  // Full team names under the flags
  tft.setTextDatum(MC_DATUM);
  setUiFont(16);
  tft.setTextColor(COL_TEXT, COL_CARD);
  tft.drawString(m.homeName, hxc, fcy + d / 2 + 18);
  tft.drawString(m.awayName, axc, fcy + d / 2 + 18);
}

// DYNAMIC part: the score / kick-off + timer. Cheap to repaint, so this runs on
// every update. It clears only its own zone over the card, never the whole card.
static void drawHeroDynamic(const Match &m) {
  int cx = HERO_X + HERO_W / 2;

  // Clear just the score zone (over the card colour), then draw the score.
  tft.fillRect(cx - 90, HERO_SCORE_Y - 22, 180, 44, COL_CARD);
  setUiFont(36);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_CARD);

  if (matchHasScore(m)) {
    char sc[12];
    snprintf(sc, sizeof(sc), "%d - %d", m.homeGoals, m.awayGoals);
    tft.drawString(sc, cx, HERO_SCORE_Y);
    if (isLive(m)) {
      drawLiveTimer(true);                                  // force a fresh paint
    } else {                                                // finished: no timer
      tft.fillRect(cx - 70, HERO_TIMER_Y - 13, 140, 26, COL_CARD);
    }
  } else {                                                  // upcoming: kick-off
    tft.drawString(m.kickoff, cx, HERO_SCORE_Y);
    setUiFont(16);
    tft.setTextColor(COL_SCHED, COL_CARD);
    tft.drawString("KICK-OFF", cx, HERO_TIMER_Y);
  }
}

// WiFi signal as 4 bars; level 0..4 derived from RSSI.
// WiFi glyph: a base dot + three concentric arcs opening upward, like a phone's
// WiFi icon. (cx,cy) is the dot centre. drawArc uses 0 deg = 6 o'clock going
// clockwise, so 180 deg is straight up and 135..225 is the top fan. Lit arcs
// use `on`; the rest are drawn dim so the full glyph shape always shows.
static void drawWifiBars(TFT_eSprite &s, int cx, int cy, int level, uint16_t on) {
  const uint16_t dim = 0x4208;                              // muted grey
  s.drawArc(cx, cy, 14, 12, 135, 225, level >= 4 ? on : dim, COL_HEADER, true);
  s.drawArc(cx, cy,  9,  7, 135, 225, level >= 3 ? on : dim, COL_HEADER, true);
  s.drawArc(cx, cy,  4,  2, 135, 225, level >= 2 ? on : dim, COL_HEADER, true);
  s.fillCircle(cx, cy, 2, level >= 1 ? on : dim);           // base dot
}

static int wifiLevel() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  long r = WiFi.RSSI();
  if (r >= -55) return 4;
  if (r >= -65) return 3;
  if (r >= -72) return 2;
  if (r >= -82) return 1;
  return 1;
}

static void drawHeader() {
  TFT_eSprite &s = headerSpr;
  s.fillSprite(COL_HEADER);

  bool connected = (WiFi.status() == WL_CONNECTED);
  bool online    = connected && gOnline;
  uint16_t stCol = online ? COL_SCHED : COL_LIVE;

  // Left: WiFi signal glyph + ONLINE/OFFLINE (same Open Sans font as the card)
  drawWifiBars(s, 18, HEADER_H / 2 + 7, wifiLevel(), stCol);
  setUiFont(s, 16);
  s.setTextDatum(ML_DATUM);
  s.setTextColor(stCol, COL_HEADER);
  s.drawString(online ? "ONLINE" : "OFFLINE", 44, HEADER_H / 2 - 1);

  // Right: "29 Jun | 00:53"
  char when[20] = "-- --- | --:--";
  struct tm t;
  if (getLocalTime(&t, 5)) strftime(when, sizeof(when), "%d %b | %H:%M", &t);
  s.setTextDatum(MR_DATUM);
  s.setTextColor(COL_TEXT, COL_HEADER);
  s.drawString(when, SCREEN_W - 12, HEADER_H / 2 - 1);

  s.pushSprite(0, 0);
}

static void drawRow(int idx, const Match &m, int screenY) {
  TFT_eSprite &s = rowSpr;
  uint16_t rowBg = (idx & 1) ? COL_ROW_ALT : COL_BG;
  s.fillSprite(rowBg);

  int midY = ROW_H / 2;

  // Status badge (left)
  uint16_t badgeCol;
  const char *badge = badgeFor(m, badgeCol);
  s.fillRoundRect(8, 6, 70, ROW_H - 12, 4, badgeCol);
  s.setTextDatum(MC_DATUM);
  s.setFreeFont(&FreeSansBold9pt7b);
  s.setTextColor(COL_BG, badgeCol);
  s.drawString(badge, 8 + 35, midY);

  // Competition code
  s.setTextDatum(ML_DATUM);
  s.setTextColor(COL_DIM, rowBg);
  s.setFreeFont(&FreeSans9pt7b);
  s.drawString(m.comp, 90, midY);

  // Home team (right aligned toward centre)
  s.setFreeFont(&FreeSansBold12pt7b);
  s.setTextColor(COL_TEXT, rowBg);
  s.setTextDatum(MR_DATUM);
  s.drawString(m.home, 230, midY);

  // Score or "vs" (centre)
  char score[12];
  if (matchHasScore(m)) snprintf(score, sizeof(score), "%d - %d", m.homeGoals, m.awayGoals);
  else                  strcpy(score, "vs");
  s.setTextDatum(MC_DATUM);
  s.setTextColor(isLive(m) ? COL_LIVE : COL_TEXT, rowBg);
  s.drawString(score, 270, midY);

  // Away team (left aligned from centre)
  s.setFreeFont(&FreeSansBold12pt7b);
  s.setTextColor(COL_TEXT, rowBg);
  s.setTextDatum(ML_DATUM);
  s.drawString(m.away, 310, midY);

  s.pushSprite(0, screenY);
}

static void drawMessage(const char *msg, uint16_t colour) {
  tft.fillRect(0, CONTENT_TOP, SCREEN_W, CONTENT_H, COL_BG);
  tft.setTextDatum(MC_DATUM);
  setUiFont(20);
  tft.setTextColor(colour, COL_BG);
  tft.drawString(msg, SCREEN_W / 2, (CONTENT_TOP + CONTENT_BOT) / 2);
}

// Watch the hero match's status across polls; capture the second-half kick-off
// the moment we see PAUSED -> IN_PLAY (Option D). Resets when the match changes.
static void trackHalfTime(const Match &m) {
  if (m.id != gTrackedId) {
    gTrackedId      = m.id;
    gSecondHalfKO   = 0;
    gLastPausedSeen = 0;
    gPrevStatus[0]  = '\0';
  }
  time_t now = time(nullptr);
  if (!strcmp(m.status, "PAUSED")) gLastPausedSeen = now;   // refresh each break poll

  bool wasPaused = (strcmp(gPrevStatus, "PAUSED") == 0);
  bool nowLive   = (strcmp(m.status, "IN_PLAY") == 0);
  if (wasPaused && nowLive && gSecondHalfKO == 0) {
    // The real restart happened somewhere between the last poll that still saw
    // PAUSED and this one. Anchor to the midpoint of that window so the clock is
    // off by at most half a poll interval, instead of trailing the whole one.
    gSecondHalfKO = (gLastPausedSeen > 0) ? (gLastPausedSeen + now) / 2 : now;
    Serial.printf("[CLOCK] 2nd-half kickoff anchored for match %ld\n", m.id);
  }
  strncpy(gPrevStatus, m.status, sizeof(gPrevStatus) - 1);
  gPrevStatus[sizeof(gPrevStatus) - 1] = '\0';
}

// ----------------------------------------------------------------------------
//  Footer navigation bar  (< page-title >  + page dots)
// ----------------------------------------------------------------------------
// Touch zones live inside the footer band; left third = previous, right third
// = next. Generous widths so the arrows are easy to hit with a fingertip.
static const int NAV_ZONE_W = 130;   // px from each edge that counts as a tap

static void drawArrow(int cx, int cy, int s, bool right, uint16_t col) {
  if (right) tft.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + s, cy, col);
  else       tft.fillTriangle(cx + s, cy - s, cx + s, cy + s, cx - s, cy, col);
}

static void drawFooter() {
  tft.fillRect(0, FOOTER_Y, SCREEN_W, FOOTER_H, COL_HEADER);
  tft.drawFastHLine(0, FOOTER_Y, SCREEN_W, COL_CARD_TOP);
  int midY = FOOTER_Y + FOOTER_H / 2;

  // Tappable arrow buttons near each edge
  tft.drawRoundRect(10, FOOTER_Y + 6, 52, FOOTER_H - 12, 6, COL_CARD_TOP);
  tft.drawRoundRect(SCREEN_W - 62, FOOTER_Y + 6, 52, FOOTER_H - 12, 6, COL_CARD_TOP);
  drawArrow(36, midY, 9, false, COL_ACCENT);             // <
  drawArrow(SCREEN_W - 36, midY, 9, true, COL_ACCENT);   // >

  // Centre: current page title
  setUiFont(16);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_HEADER);
  tft.drawString(kPageTitle[gPage], SCREEN_W / 2, midY - 4);

  // Page dots
  int gap = 14, x0 = SCREEN_W / 2 - (PAGE_COUNT - 1) * gap / 2;
  int dy = FOOTER_Y + FOOTER_H - 7;
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (i == gPage) tft.fillCircle(x0 + i * gap, dy, 3, COL_ACCENT);
    else            tft.drawCircle(x0 + i * gap, dy, 3, COL_DIM);
  }
}

// ----------------------------------------------------------------------------
//  Page bodies
// ----------------------------------------------------------------------------
static bool predAll(const Match &m)      { (void)m; return true; }
static bool predFinished(const Match &m) { return !strcmp(m.status, "FINISHED"); }

// Generic list page: draws matches passing pred() as compact rows.
static void drawListPage(bool (*pred)(const Match &), const char *emptyMsg) {
  int maxRows = CONTENT_H / ROW_H;
  int shown = 0;
  for (int i = 0; i < matchCount && shown < maxRows; i++) {
    if (pred && !pred(matches[i])) continue;
    drawRow(shown, matches[i], CONTENT_TOP + shown * ROW_H);
    shown++;
  }
  if (shown == 0) drawMessage(emptyMsg, COL_DIM);
}

// Featured page: the big hero card for the top-ranked match. The heavy static
// card is redrawn only when the match changes or we just switched to this page;
// otherwise just the score/timer zone updates in place (no flash).
static void drawFeatured() {
  heroMatch = matches[0];

  bool newCard = (matches[0].id != gHeroDrawnId) || (gPage != gRenderedPage);
  if (newCard) {
    tft.fillRect(0, CONTENT_TOP, SCREEN_W, CONTENT_H, COL_BG);  // clear band once
    drawHeroStatic(matches[0]);
    gHeroDrawnId = matches[0].id;
  }
  drawHeroDynamic(matches[0]);

  heroShown  = true;
  heroIsLive = isLive(matches[0]);
}

// A cheap hash of everything that affects the on-screen content. The loop only
// repaints the screen when this changes, so a fetch that returns identical data
// no longer triggers a jarring full-screen redraw.
static uint32_t contentSignature() {
  uint32_t h = 2166136261u;                 // FNV-1a
  auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
  mix((uint32_t)matchCount);
  mix((uint32_t)gPage);
  for (int i = 0; i < matchCount; i++) {
    const Match &m = matches[i];
    mix((uint32_t)m.id);
    mix((uint32_t)(m.homeGoals * 257 + m.awayGoals));
    for (const char *p = m.status; *p; p++) mix((uint8_t)*p);
  }
  return h;
}

static void render() {
  drawHeader();
  heroShown = heroIsLive = false;

  if (matchCount == 0) {
    drawMessage(statusLine, COL_DIM);   // clears the band itself
    gHeroDrawnId = -1;                  // force a fresh card when matches return
  } else if (gPage == PAGE_FEATURED) {
    drawFeatured();        // clears the band itself, only when the card changes
  } else {
    // List pages: clear and redraw the rows (cheap, no flag decode).
    tft.fillRect(0, CONTENT_TOP, SCREEN_W, CONTENT_H, COL_BG);
    if (gPage == PAGE_RESULTS) drawListPage(predFinished, "No results yet");
    else                       drawListPage(predAll,      "No matches");
  }

  drawFooter();
  gRenderedPage = gPage;
}

// ----------------------------------------------------------------------------
//  Networking
// ----------------------------------------------------------------------------
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  strcpy(statusLine, "Connecting to WiFi...");
  render();

  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    // NTP for the on-screen clock and for computing the UTC query date range.
    // The TZ offset is applied here so getLocalTime() returns local time.
    configTime(TZ_OFFSET_MINUTES * 60, 0, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    uint32_t s = millis();
    while (!getLocalTime(&t, 200) && millis() - s < 8000) delay(200);
    Serial.printf("[NTP] time %ssynced\n", (time(nullptr) > 1700000000) ? "" : "NOT ");
  } else {
    Serial.println("[WiFi] FAILED");
    strcpy(statusLine, "WiFi failed - check config.h");
    render();
  }
}

// Format a UTC epoch as "YYYY-MM-DD".
static void fmtDateUTC(time_t when, char *out) {
  struct tm g;
  gmtime_r(&when, &g);
  snprintf(out, 11, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
}

// Build the request URL. With NTP time we request a date range that starts one
// day back (in UTC) so live matches that kicked off "yesterday UTC" are caught,
// through +8 days of upcoming fixtures (free tier allows up to a 10-day window).
// Without a valid clock we fall back to the default "today" matches endpoint.
static String buildUrl() {
  String url = "https://api.football-data.org/v4/matches?competitions=";
  url += (strlen(COMPETITIONS) > 0) ? COMPETITIONS : "WC";

  time_t now = time(nullptr);
  if (now > 1700000000) {
    // yesterday (UTC) .. +3 days. Kept short so the decoded JSON body stays
    // small in RAM; wide enough to show live + recent + next fixtures.
    char from[11], to[11];
    fmtDateUTC(now - 86400L,     from);
    fmtDateUTC(now + 3L * 86400, to);
    url += "&dateFrom=" + String(from) + "&dateTo=" + String(to);
  }
  return url;
}

// Fetch + parse matches. Returns true on success.
static bool fetchMatches() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) { gOnline = false; return false; }
  }

  WiFiClientSecure client;
  client.setInsecure();                 // football-data.org cert not pinned
  HTTPClient https;
  https.setTimeout(12000);

  String url = buildUrl();
  Serial.printf("[HTTP] GET %s\n", url.c_str());
  if (!https.begin(client, url)) {
    strcpy(statusLine, "HTTPS begin failed");
    Serial.println("[HTTP] begin() failed");
    return false;
  }
  https.addHeader("X-Auth-Token", API_TOKEN);

  int code = https.GET();
  Serial.printf("[HTTP] status=%d  contentLength=%d\n", code, https.getSize());
  gOnline = (code == HTTP_CODE_OK);
  if (code != HTTP_CODE_OK) {
    if (code == 429)      strcpy(statusLine, "Rate limited (10/min) - slow down");
    else if (code == 403) strcpy(statusLine, "403 - competition not in free tier");
    else if (code == 400) strcpy(statusLine, "400 - check API token / params");
    else                  snprintf(statusLine, sizeof(statusLine), "HTTP error %d", code);
    https.end();
    return false;
  }

  // Only pull the fields we render -> keeps RAM use small.
  JsonDocument filter;
  filter["matches"][0]["id"]                     = true;
  filter["matches"][0]["status"]                 = true;
  filter["matches"][0]["utcDate"]                = true;
  filter["matches"][0]["stage"]                  = true;
  filter["matches"][0]["group"]                  = true;
  filter["matches"][0]["competition"]["code"]    = true;
  filter["matches"][0]["homeTeam"]["tla"]        = true;
  filter["matches"][0]["homeTeam"]["shortName"]  = true;
  filter["matches"][0]["homeTeam"]["name"]       = true;
  filter["matches"][0]["awayTeam"]["tla"]        = true;
  filter["matches"][0]["awayTeam"]["shortName"]  = true;
  filter["matches"][0]["awayTeam"]["name"]       = true;
  filter["matches"][0]["score"]["fullTime"]["home"] = true;
  filter["matches"][0]["score"]["fullTime"]["away"] = true;
  filter["matches"][0]["score"]["halfTime"]["home"] = true;

  // The API responds with Transfer-Encoding: chunked (contentLength = -1).
  // getString() decodes the chunked body; the raw getStream() would leave the
  // hex chunk-size markers in front of the JSON. Close the connection before
  // parsing so the TLS buffers are freed first.
  Serial.printf("[MEM] free heap before read: %u\n", ESP.getFreeHeap());
  String payload = https.getString();
  https.end();
  Serial.printf("[HTTP] body %u bytes\n", payload.length());

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (err) {
    snprintf(statusLine, sizeof(statusLine), "JSON error: %s", err.c_str());
    Serial.printf("[JSON] %s\n", err.c_str());
    return false;
  }

  JsonArrayConst arr = doc["matches"].as<JsonArrayConst>();
  matchCount = 0;
  for (JsonObjectConst mo : arr) {
    if (matchCount >= MAX_MATCHES) break;
    Match &m = matches[matchCount];

    m.id = mo["id"] | 0L;
    copyTeamId(mo["homeTeam"], m.home);
    copyTeamId(mo["awayTeam"], m.away);
    copyTeamName(mo["homeTeam"], m.homeName, sizeof(m.homeName));
    copyTeamName(mo["awayTeam"], m.awayName, sizeof(m.awayName));

    const char *st = mo["status"] | "SCHEDULED";
    strncpy(m.status, st, sizeof(m.status) - 1); m.status[sizeof(m.status) - 1] = '\0';

    const char *cc = mo["competition"]["code"] | "";
    strncpy(m.comp, cc, sizeof(m.comp) - 1); m.comp[sizeof(m.comp) - 1] = '\0';

    const char *ud = mo["utcDate"] | "";
    strncpy(m.utc, ud, sizeof(m.utc) - 1); m.utc[sizeof(m.utc) - 1] = '\0';

    const char *sg = mo["stage"] | "";
    strncpy(m.stage, sg, sizeof(m.stage) - 1); m.stage[sizeof(m.stage) - 1] = '\0';
    const char *gp = mo["group"] | "";
    strncpy(m.group, gp, sizeof(m.group) - 1); m.group[sizeof(m.group) - 1] = '\0';

    m.homeGoals = mo["score"]["fullTime"]["home"] | 0;
    m.awayGoals = mo["score"]["fullTime"]["away"] | 0;
    m.secondHalf = !mo["score"]["halfTime"]["home"].isNull();
    utcToLocal(m.utc, m.kickoff);
    m.koEpoch = isoToEpoch(m.utc);
    matchCount++;
  }

  Serial.printf("[PARSE] %d matches\n", matchCount);
  sortMatches();

  // Watch the top match's half-time transition on EVERY poll (independent of
  // which page is showing) so the 2nd-half clock is anchored reliably, and note
  // whether anything is live so loop() can poll faster while it matters.
  gAnyLive = false;
  for (int i = 0; i < matchCount; i++)
    if (isLive(matches[i])) { gAnyLive = true; break; }
  if (matchCount > 0) trackHalfTime(matches[0]);

  if (matchCount == 0) strcpy(statusLine, "No matches in range");
  return true;
}

// ----------------------------------------------------------------------------
//  Touch (XPT2046_Touchscreen on the dedicated HSPI bus). No on-screen
//  calibration step: raw points are mapped to screen pixels with the gTouch*
//  constants (tune them from the TOUCH_DEBUG serial output).
// ----------------------------------------------------------------------------
static void setupTouch() {
  touchSPI.begin(T_CLK_PIN, T_MISO_PIN, T_MOSI_PIN, T_CS_PIN);
  ts.begin(touchSPI);
  ts.setRotation(SCREEN_ROTATION);
  Serial.println("[TOUCH] XPT2046 started on dedicated HSPI bus "
                 "(CLK25 MISO39 MOSI32 CS33)");
}

// Poll the touchscreen; if an arrow button was tapped, switch pages and redraw.
static void handleTouch() {
#if !TOUCH_ENABLED
  return;                                   // touch on hold - no polling at all
#endif
#if TOUCH_DEBUG
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 200) {
    lastDbg = millis();
    bool t = ts.touched();
    TS_Point p = ts.getPoint();
    Serial.printf("[TOUCH] touched=%d  x=%4d y=%4d z=%4d\n", t, p.x, p.y, p.z);
  }
  return;
#endif
  static bool     down = false;
  static uint32_t lastAct = 0;
  bool pressed = ts.touched();

  if (pressed && !down && millis() - lastAct > 250) {
    down = true;
    TS_Point p = ts.getPoint();
    int sx = map(p.x, gTouchMinX, gTouchMaxX, 0, SCREEN_W);
    int sy = map(p.y, gTouchMinY, gTouchMaxY, 0, SCREEN_H);
    if (sy >= FOOTER_Y) {                       // only the footer band navigates
      int prev = gPage;
      if (sx <= NAV_ZONE_W)                 gPage = (gPage + PAGE_COUNT - 1) % PAGE_COUNT;
      else if (sx >= SCREEN_W - NAV_ZONE_W) gPage = (gPage + 1) % PAGE_COUNT;
      if (gPage != prev) {
        Serial.printf("[NAV] page -> %s (raw %d,%d -> screen %d,%d)\n",
                      kPageTitle[gPage], p.x, p.y, sx, sy);
        render();
        gLastContentSig = contentSignature();   // keep in sync after a page switch
        lastAct = millis();
      }
    }
  }
  if (!pressed) down = false;
}

// ----------------------------------------------------------------------------
//  Arduino entry points
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] ESP32 Football Scoreboard");

  Serial.printf("[MEM] PSRAM: %u free / %u total\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(COL_BG);

  // LittleFS holds the flag PNGs (uploaded with: pio run -t uploadfs)
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount FAILED");
  } else {
    Serial.println("[FS] LittleFS mounted. /flags contents:");
    fs::File dir = LittleFS.open("/flags");
    if (dir && dir.isDirectory()) {
      for (fs::File e = dir.openNextFile(); e; e = dir.openNextFile())
        Serial.printf("[FS]   %s  (%u bytes)\n", e.name(), (unsigned)e.size());
    } else {
      Serial.println("[FS]   (no /flags directory - did you run 'pio run -t uploadfs'?)");
    }
    gSmoothFonts = LittleFS.exists("/fonts/OpenSans16.vlw") &&
                   LittleFS.exists("/fonts/OpenSans20.vlw") &&
                   LittleFS.exists("/fonts/OpenSans36.vlw");
    Serial.printf("[FS] smooth fonts: %s\n", gSmoothFonts ? "found" : "MISSING (using FreeFonts)");
  }

  spritesOK  = headerSpr.createSprite(SCREEN_W, HEADER_H) != nullptr;
  spritesOK &= rowSpr.createSprite(SCREEN_W, ROW_H) != nullptr;
  Serial.printf("[SPRITE] alloc %s (free heap %u)\n",
                spritesOK ? "ok" : "FAILED", ESP.getFreeHeap());

  setupTouch();   // load or run touch calibration before we need the arrows

  connectWiFi();
  fetchMatches();
  render();
  gLastContentSig = contentSignature();
}

void loop() {
  static uint32_t lastFetch = 0;
  static uint32_t lastTick  = 0;
  static int      lastMin   = -1;
  uint32_t now = millis();

  handleTouch();   // inert while TOUCH_ENABLED is 0

  // Periodic data fetch. Repaint ONLY when the data actually changed, so an
  // unchanged refresh no longer triggers a full-screen redraw. While a match is
  // live we poll every ~20s (3 req/min, well under the free tier's 10/min) so
  // the PAUSED->IN_PLAY restart is caught quickly and the 2nd-half clock anchors
  // tightly; otherwise we use the configured interval.
  uint32_t liveSecs  = (REFRESH_SECONDS < 20) ? (uint32_t)REFRESH_SECONDS : 20;
  uint32_t fetchSecs = gAnyLive ? liveSecs : (uint32_t)REFRESH_SECONDS;
  if (now - lastFetch >= fetchSecs * 1000UL) {
    lastFetch = now;
    fetchMatches();
    uint32_t sig = contentSignature();
    if (sig != gLastContentSig) {
      gLastContentSig = sig;
      render();
    }
  }

  // Once per second: tick the live timer; refresh the header only when the
  // displayed minute changes (so the clock isn't constantly repainting).
  if (now - lastTick >= 1000) {
    lastTick = now;
    struct tm t;
    if (getLocalTime(&t, 5) && t.tm_min != lastMin) {
      lastMin = t.tm_min;
      drawHeader();
    }
    if (gPage == PAGE_FEATURED && heroShown && heroIsLive) drawLiveTimer();
  }
}
