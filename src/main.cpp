// =============================================================================
//  ESP32 Live Football Scoreboard  ->  ILI9488 480x320 TFT (TFT_eSPI)
//  Data: local Python scraper/API server (SofaScore -> JSON), see README.md
//
//  Edit src/config.h with your WiFi + SERVER_HOST (this PC's LAN IP) before
//  uploading. Both devices must be on the same local network.
//
//  Rendering is done with TFT_eSprite (off-screen buffers) for flicker-free
//  updates. A full 480x320 16bpp sprite would need ~307KB, which won't fit in
//  the ESP32's RAM, so we use two reusable region sprites instead: one for the
//  header bar and one row-height sprite that is redrawn and pushed per match.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <PNGdec.h>
#include <SPI.h>
#include <time.h>

#include "config.h"


//  TOUCH_ENABLED 0 -> all touch polling is skipped, so there are NO phantom
//                     page switches and the serial log stays clean.
//  TOUCH_DEBUG   1 -> stream the raw TFT_eSPI touch point (touched/x/y) to serial.
#define TOUCH_ENABLED 1
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
//  Touch: TFT_eSPI's built-in XPT2046 support, sharing the display's VSPI bus
//  (MISO 19, MOSI 23, SCLK 18) plus the dedicated TOUCH_CS pin (14, set in
//  lib/TFT_eSPI/User_Setup.h). No extra wiring beyond that CS line - T_CLK,
//  T_DIN and T_DO tie to the same pins as the display.
//
//  Calibration values are hardcoded in config.h (TOUCH_CAL_DATA) once known -
//  see setupTouch() below for how to obtain them.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//  Match model
// ----------------------------------------------------------------------------
struct Match {
  long id;           // server match id
  char home[5];      // short id derived from the team name, e.g. "SOUT"
  char away[5];
  char homeName[26]; // full name, e.g. "South Africa"
  char awayName[26];
  time_t koEpoch;    // kick-off as UTC epoch (server's startTimestamp, for the live clock)
  bool   secondHalf; // statusDescription says "2nd half" (or later)
  int    minute;     // server's live match minute, -1 if absent (HT/pre-KO/FT/penalties)
  char comp[8];      // short round/group label for the compact row, e.g. "GRP A" / "R27"
  int  round;
  int  homeGoals;
  int  awayGoals;
  char status[16];            // "notstarted" / "inprogress" / "finished" / ...
  char statusDescription[24]; // "2nd half", "Halftime", "Not started", ...
  char kickoff[6];   // "HH:MM" local time
  char group[16];    // e.g. "Group A" (may be empty for knockout matches)
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

// Half-time anchor (Option D): when we observe the Halftime -> "2nd half"
// transition we record the real second-half kick-off, making the 2nd-half
// minute exact instead of an estimate.
static long   gTrackedId    = -1;
static char   gPrevStatus[12] = "";
static time_t gSecondHalfKO = 0;
static time_t gLastPausedSeen = 0;  // wall-clock of the most recent Halftime poll
static bool   gAnyLive = false;     // any match in play -> poll faster (see loop)

// ----------------------------------------------------------------------------
//  Pages (multi-screen). A footer nav bar at the bottom holds < and > arrows;
//  tapping them cycles through the screens. Touch comes from the XPT2046
//  controller (TOUCH_CS in User_Setup.h), calibrated once and saved to LittleFS.
// ----------------------------------------------------------------------------
enum Page { PAGE_FEATURED = 0, PAGE_ALL, PAGE_RESULTS, PAGE_LINEUPS, PAGE_PHOTOS, PAGE_COUNT };
static int gPage = PAGE_FEATURED;
static uint32_t gLastContentSig = 0;   // last painted content hash (see loop)
static int  gRenderedPage = -1;        // page currently on screen
static long gHeroDrawnId  = -1;        // id of the match whose card is on screen
static const char *kPageTitle[PAGE_COUNT] = { "FEATURED", "ALL MATCHES", "RESULTS", "LINE-UPS", "PHOTOS" };

static const int FOOTER_H    = 40;
static const int FOOTER_Y    = SCREEN_H - FOOTER_H;    // 280
static const int CONTENT_TOP = HEADER_H + 4;           // 46
static const int CONTENT_BOT = FOOTER_Y - 2;           // 278
static const int CONTENT_H   = CONTENT_BOT - CONTENT_TOP;

// Hero card geometry (centred in the content band on the FEATURED page)
static const int HERO_X = 6;
static const int HERO_H = 196;   // tall enough to fit the goals strip below the timer
static const int HERO_Y = CONTENT_TOP + (CONTENT_H - HERO_H) / 2;
static const int HERO_W = SCREEN_W - 12;

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------

struct TeamCode { const char *name; const char *code; };

// Known team-name -> FIFA-style 3-letter code. The server sends full names
// ("South Africa") rather than codes, but the flag PNGs on LittleFS are named
// by code (data/flags/rsa.png) - so this table drives both the compact row
// label and the flag lookup key. Add an entry here whenever you add a new
// flag file; unmatched names fall back to a truncated name below, which is
// fine for the row label but won't find a flag (falls back to the lettered
// placeholder disc).
static const TeamCode kTeamCodes[] = {
  { "Brazil",        "BRA" },
  { "Canada",        "CAN" },
  { "England",       "ENG" },
  { "Japan",         "JPN" },
  { "Mexico",        "MEX" },
  { "Norway",        "NOR" },
  { "Portugal",      "POR" },
  { "South Africa",  "RSA" },
  { "Argentina",     "ARG" },
  { "France",        "FRA" },
  { "Germany",       "GER" },
  { "Spain",         "ESP" },
  { "Italy",         "ITA" },
  { "Netherlands",   "NED" },
  { "Belgium",       "BEL" },
  { "Croatia",       "CRO" },
  { "Uruguay",       "URU" },
  { "Colombia",      "COL" },
  { "Ecuador",       "ECU" },
  { "Peru",          "PER" },
  { "Chile",         "CHI" },
  { "Paraguay",      "PAR" },
  { "United States", "USA" },
  { "USA",           "USA" },
  { "Panama",        "PAN" },
  { "Costa Rica",    "CRC" },
  { "South Korea",   "KOR" },
  { "Australia",     "AUS" },
  { "Iran",          "IRN" },
  { "Saudi Arabia",  "KSA" },
  { "Qatar",         "QAT" },
  { "Morocco",       "MAR" },
  { "Algeria",       "ALG" },
  { "Tunisia",       "TUN" },
  { "Egypt",         "EGY" },
  { "Senegal",       "SEN" },
  { "Nigeria",       "NGA" },
  { "Ghana",         "GHA" },
  { "Cameroon",      "CMR" },
  { "Ivory Coast",   "CIV" },
  { "Switzerland",   "SUI" },
  { "Poland",        "POL" },
  { "Serbia",        "SRB" },
  { "Denmark",       "DEN" },
  { "Sweden",        "SWE" },
  { "Wales",         "WAL" },
  { "Scotland",      "SCO" },
};
static const int kTeamCodeCount = sizeof(kTeamCodes) / sizeof(kTeamCodes[0]);

static bool equalsCI(const char *a, const char *b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++; b++;
  }
  return *a == *b;
}

// Derive a short (max 4 char) row-display id from a full team name: a known
// FIFA-style code if we have one (also doubles as the /flags/<code>.png key),
// otherwise a truncated fallback so the row still shows something.
static void makeShortId(const char *name, char *dst) {
  for (int i = 0; i < kTeamCodeCount; i++) {
    if (equalsCI(name, kTeamCodes[i].name)) { strcpy(dst, kTeamCodes[i].code); return; }
  }
  int i = 0;
  for (; name[i] && i < 4; i++) dst[i] = (char)toupper((unsigned char)name[i]);
  dst[i] = '\0';
  if (!dst[0]) strcpy(dst, "???");
}

// Case-insensitive substring search (statusDescription text varies in case).
static bool containsCI(const char *hay, const char *needle) {
  size_t hl = strlen(hay), nl = strlen(needle);
  if (!nl || nl > hl) return false;
  for (size_t i = 0; i + nl <= hl; i++) {
    size_t j = 0;
    for (; j < nl; j++)
      if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
    if (j == nl) return true;
  }
  return false;
}

// Last space-separated token of a full name, e.g. "Cristiano Ronaldo" ->
// "Ronaldo". The hero card's goal strip only has room for a surname.
static const char *lastToken(const char *s) {
  const char *last = s;
  for (const char *p = s; *p; p++) if (*p == ' ' && p[1]) last = p + 1;
  return last;
}

// Convert a UTC epoch (server's startTimestamp) to local "HH:MM".
static void epochToLocal(time_t ts, char *out) {
  struct tm g;
  gmtime_r(&ts, &g);
  int total = g.tm_hour * 60 + g.tm_min + TZ_OFFSET_MINUTES;
  total = ((total % 1440) + 1440) % 1440;
  snprintf(out, 6, "%02d:%02d", total / 60, total % 60);
}

// Halftime is reported as status "inprogress" with a "Halftime" description,
// not a separate status value. Same idea for extra time (knockout matches
// only) and penalties - all still just "inprogress" with a description that
// says what's actually happening.
static bool isHalftime(const Match &m) {
  return containsCI(m.statusDescription, "halftime") || containsCI(m.statusDescription, "half time");
}
static bool isExtraTime(const Match &m) {
  return containsCI(m.statusDescription, "extra");
}
static bool isPenalties(const Match &m) {
  return containsCI(m.statusDescription, "penalt");
}
static bool isLive(const Match &m) {
  return !strcmp(m.status, "inprogress");
}
static bool isUpcoming(const Match &m) {
  return !strcmp(m.status, "notstarted");
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
        swap = a.koEpoch < b.koEpoch;   // finished: newest first
      } else {
        swap = a.koEpoch > b.koEpoch;   // live/upcoming: earliest first
      }
      if (swap) { Match t = a; a = b; b = t; }
    }
  }
}

// Returns a short status badge ("LIVE", "HT", "FT", or kickoff time) + colour.
static const char *badgeFor(const Match &m, uint16_t &colour) {
  if (!strcmp(m.status, "inprogress")) {
    if (isHalftime(m))  { colour = COL_HT;   return "HT";   }
    if (isPenalties(m)) { colour = COL_HT;   return "PENS"; }
    if (isExtraTime(m)) { colour = COL_LIVE; return "ET";   }
    colour = COL_LIVE; return "LIVE";
  }
  if (!strcmp(m.status, "finished"))                                        { colour = COL_FT;  return "FT";   }
  if (!strcmp(m.status, "postponed"))                                       { colour = COL_DIM; return "PP";   }
  if (!strcmp(m.status, "suspended") || !strcmp(m.status, "interrupted"))   { colour = COL_DIM; return "SUSP"; }
  if (!strcmp(m.status, "canceled")  || !strcmp(m.status, "cancelled"))    { colour = COL_DIM; return "CANC"; }
  colour = COL_SCHED;                   // notstarted
  return m.kickoff;
}

static bool matchHasScore(const Match &m) {
  return !isUpcoming(m) && strcmp(m.status, "postponed")
      && strcmp(m.status, "canceled") && strcmp(m.status, "cancelled");
}

// Knockout-round number -> name, from this competition's GET /rounds (group
// stage rounds 1-3 aren't listed there since those matches carry a `group`
// instead - see stageLabel()). The round numbers are this server's own
// numbering, not sequential by stage order, so this only covers what /rounds
// actually reported - re-check it if the server switches competitions.
struct RoundName { int round; const char *label; const char *shortLabel; };
static const RoundName kRoundNames[] = {
  { 6,  "ROUND OF 32",   "R32" },
  { 5,  "ROUND OF 16",   "R16" },
  { 27, "QUARTERFINALS", "QF"  },
  { 28, "SEMIFINALS",    "SF"  },
  { 50, "3RD PLACE",     "3RD" },
  { 29, "FINAL",         "FIN" },
};
static const RoundName *findRoundName(int round) {
  for (const RoundName &r : kRoundNames) if (r.round == round) return &r;
  return nullptr;
}

// Build a human label for the round/group, e.g. "GROUP B" or "QUARTERFINALS".
static void stageLabel(const Match &m, char *out, size_t n) {
  if (m.group[0]) {
    strncpy(out, m.group, n - 1);
    out[n - 1] = '\0';
    for (char *p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
    return;
  }
  const RoundName *r = findRoundName(m.round);
  if (r) strncpy(out, r->label, n - 1);
  else   snprintf(out, n, "ROUND %d", m.round);
  out[n - 1] = '\0';
}

// ----------------------------------------------------------------------------
//  Fonts
//  Smooth (anti-aliased) Open Sans VLW files live on LittleFS under /fonts.
//  setUiFont() selects the nearest size for direct-to-TFT text or a sprite; if
//  the VLW files aren't present it falls back to the built-in FreeFonts so the
//  UI still renders. Used everywhere text is drawn, including header and row
//  sprites, so all pages share the same font.
// ----------------------------------------------------------------------------
// Selects the nearest Open Sans size on the given target (tft or a sprite).
// Sprites keep their own font state, so the header sprite must be set too.
//
// loadFont() re-reads the whole glyph/Unicode index table from LittleFS, so
// calling it every time (a row draws badge/comp/home/away -> 3-4 calls each)
// made rows visibly paint one at a time. tft/headerSpr/rowSpr are the only
// targets ever passed in, so a tiny fixed-size cache of (target, last size)
// is enough to skip the reload whenever the size hasn't actually changed.
static const TFT_eSPI *gFontCacheTarget[3] = { nullptr, nullptr, nullptr };
static int             gFontCachePx[3]     = { -1, -1, -1 };

static void setUiFont(TFT_eSPI &g, int px) {
  if (gSmoothFonts) {
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < 3; i++) {
      if (gFontCacheTarget[i] == &g) { slot = i; break; }
      if (gFontCacheTarget[i] == nullptr && freeSlot < 0) freeSlot = i;
    }
    if (slot >= 0 && gFontCachePx[slot] == px) return;   // already loaded - skip reload
    if (slot < 0) slot = freeSlot;

    g.unloadFont();
    const char *f = (px >= 30) ? "fonts/OpenSans36"
                  : (px >= 20) ? "fonts/OpenSans20"
                               : "fonts/OpenSans16";
    g.loadFont(f, LittleFS);

    if (slot >= 0) { gFontCacheTarget[slot] = &g; gFontCachePx[slot] = px; }
  } else {
    g.setFreeFont((px >= 30) ? &FreeSansBold24pt7b
                : (px >= 20) ? &FreeSansBold12pt7b
                             : &FreeSansBold9pt7b);
  }
}
static void setUiFont(int px) { setUiFont(tft, px); }

// Switch to the tiny built-in GLCD font (8px) for compact UI chrome like the
// Photos page's jersey-number badges. Just calling setTextFont(1) isn't
// enough while a smooth (VLW) font is loaded: TFT_eSPI's drawChar() always
// prefers the loaded smooth font over `textfont` until unloadFont() is
// called - which is why those badges were rendering at the last smooth-font
// size instead of tiny digits. Invalidates this target's setUiFont() cache
// slot too, so the next setUiFont() call actually reloads instead of assuming
// nothing changed.
static void useTinyFont(TFT_eSPI &g) {
  if (gSmoothFonts) {
    g.unloadFont();
    for (int i = 0; i < 3; i++) if (gFontCacheTarget[i] == &g) gFontCachePx[i] = -1;
  }
  g.setTextFont(1);
}

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
// Background to alpha-blend transparent pixels against, packed as 0x00BBGGRR.
// 0xffffffff tells PNGdec to skip blending entirely (fine for opaque flags);
// player photos are real RGBA cutouts, so they set this to COL_BG first.
static uint32_t g_pngBkgd = 0xffffffff;

// PNGdec line callback: convert each scanline to native RGB565 and write it
// pixel-by-pixel into the target sprite. Using LITTLE_ENDIAN + drawPixel keeps
// the colour values native end-to-end (no pushImage byte-swap to get wrong).
static int pngDraw(PNGDRAW *pDraw) {
  static uint16_t lineBuf[200];
  if (!g_png || !g_pngTarget || pDraw->iWidth > 200) return 0;
  g_png->getLineAsRGB565(pDraw, lineBuf, PNG_RGB565_LITTLE_ENDIAN, g_pngBkgd);
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
        g_pngBkgd = 0xffffffff;   // flags: no alpha blending
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

// Goal-scorer strip: below the timer, two columns (home | away).
static const int HERO_GOALS_Y = HERO_Y + 134;
static const int HERO_GOALS_H = HERO_H - 134 - 6;

// Draws the red match-minute clock under the score. Prefers the server's own
// "minute" field (a real running count from SofaScore's period-start data -
// it keeps counting past 45/90, so "minute - 45" is genuine added time, e.g.
// "45+2'"), falling back to a wall-clock-since-kickoff estimate for the rare
// case the field is absent while the match is still live.
static void drawLiveTimer(bool force = false) {
  int cx = HERO_X + HERO_W / 2;

  // Build the label + colour first so we can skip repainting when it hasn't
  // changed (otherwise "HALF TIME" / the minute would flicker every second).
  char t[12];
  uint16_t col;
  if (isHalftime(heroMatch)) {
    strcpy(t, "HALF TIME");
    col = COL_HT;
  } else if (isPenalties(heroMatch)) {
    strcpy(t, "PENALTIES");
    col = COL_HT;
  } else if (heroMatch.minute >= 0) {
    int mins = heroMatch.minute;
    if (isExtraTime(heroMatch)) {
      // Extra time's own two 15-min halves run past 90 as a fresh continuous
      // count (91, 93, 106...), confirmed against the server's own event
      // timeline - it's a real minute, not stoppage tacked onto the 90.
      snprintf(t, sizeof(t), "%d'", mins);
    } else if (!heroMatch.secondHalf && mins > 45) {
      snprintf(t, sizeof(t), "45+%d'", mins - 45);
    } else if (heroMatch.secondHalf && mins > 90) {
      snprintf(t, sizeof(t), "90+%d'", mins - 90);
    } else {
      snprintf(t, sizeof(t), "%d'", mins);
    }
    col = COL_LIVE;
  } else if (heroMatch.secondHalf &&
             !(gTrackedId == heroMatch.id && gSecondHalfKO > 0)) {
    // 2nd half, but we never saw the restart (e.g. the board was switched on
    // mid-half) and the server gave no minute this time either. "elapsed - 15"
    // would over-count by first-half stoppage + the real break length - that's
    // how 57' ends up shown as 64'. Print an honest label instead of a fake number.
    strcpy(t, "2ND HALF");
    col = COL_LIVE;
  } else {
    // Wall-clock fallback:
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

// ----------------------------------------------------------------------------
//  Goal events (scorer + assist) for the hero card, from GET
//  /match/{id}/events on the local server. Unlike lineups, goals can still
//  happen after the first fetch while a match is live, so this can't be a
//  once-per-match cache - it re-fetches whenever the featured match's goal
//  tally ticks up, not just when the match itself changes.
// ----------------------------------------------------------------------------
struct GoalEvent {
  int  minute;
  bool isHome;      // true = home team scored
  char player[23];
  char assist[23];
  bool hasAssist;
  bool ownGoal;
  bool penalty;
};
static const int MAX_GOAL_EVENTS = 12;
static GoalEvent gGoalEvents[MAX_GOAL_EVENTS];
static int       gGoalEventCount  = 0;
static long      gEventsMatchId   = -1;   // match whose goals are currently loaded
static int       gEventsGoalTotal = -1;   // home+away goals as of the last successful fetch

// Fetch + parse /match/{id}/events, keeping only "goal" entries (cards/subs/
// delays are filtered out here rather than server-side - the endpoint mixes
// all event types in one chronological list).
static bool fetchGoalEvents(long matchId, int goalTotal) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(12000);

  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) +
               "/match/" + String(matchId) + "/events";
  Serial.printf("[HTTP] GET %s\n", url.c_str());

  gEventsMatchId   = matchId;
  gEventsGoalTotal = goalTotal;
  gGoalEventCount  = 0;
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  Serial.printf("[EVENTS] status=%d\n", code);
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  JsonDocument filter;
  filter["events"][0]["minute"]  = true;
  filter["events"][0]["type"]    = true;
  filter["events"][0]["team"]    = true;
  filter["events"][0]["player"]  = true;
  filter["events"][0]["assist"]  = true;
  filter["events"][0]["ownGoal"] = true;
  filter["events"][0]["penalty"] = true;

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[EVENTS] JSON error: %s\n", err.c_str());
    return false;
  }

  for (JsonObjectConst eo : doc["events"].as<JsonArrayConst>()) {
    const char *type = eo["type"] | "";
    if (strcmp(type, "goal") != 0) continue;
    if (gGoalEventCount >= MAX_GOAL_EVENTS) break;

    GoalEvent &g = gGoalEvents[gGoalEventCount];
    g.minute = eo["minute"] | 0;
    const char *team = eo["team"] | "home";
    g.isHome = !strcmp(team, "home");
    const char *pl = eo["player"] | "";
    strncpy(g.player, pl, sizeof(g.player) - 1); g.player[sizeof(g.player) - 1] = '\0';
    const char *as = eo["assist"] | "";
    g.hasAssist = as[0] != '\0';
    strncpy(g.assist, as, sizeof(g.assist) - 1); g.assist[sizeof(g.assist) - 1] = '\0';
    g.ownGoal = eo["ownGoal"] | false;
    g.penalty = eo["penalty"] | false;
    gGoalEventCount++;
  }

  Serial.printf("[EVENTS] %d goal(s) parsed\n", gGoalEventCount);
  return true;
}

// Draws the goal-scorer strip: one column per side, chronological, capped to
// whatever the band fits. If a side has more goals than fit, the oldest ones
// are dropped - the most recent goal is what matters most in a live match.
static void drawHeroGoals() {
  tft.fillRect(HERO_X + 2, HERO_GOALS_Y, HERO_W - 4, HERO_GOALS_H, COL_CARD);
  if (gGoalEventCount == 0) return;

  const int rowH    = 18;
  const int maxRows = HERO_GOALS_H / rowH;

  int homeTotal = 0, awayTotal = 0;
  for (int i = 0; i < gGoalEventCount; i++)
    if (gGoalEvents[i].isHome) homeTotal++; else awayTotal++;
  int homeSkip = (homeTotal > maxRows) ? homeTotal - maxRows : 0;
  int awaySkip = (awayTotal > maxRows) ? awayTotal - maxRows : 0;

  setUiFont(16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_CARD);

  int homeX = HERO_X + 14, awayX = HERO_X + HERO_W / 2 + 6;
  int homeSeen = 0, homeRow = 0, awaySeen = 0, awayRow = 0;

  for (int i = 0; i < gGoalEventCount; i++) {
    const GoalEvent &g = gGoalEvents[i];
    char suffix[8] = "";
    if (g.ownGoal)      strcpy(suffix, " (OG)");
    else if (g.penalty) strcpy(suffix, " (pen)");

    char line[48];
    if (g.hasAssist)
      snprintf(line, sizeof(line), "%d' %s%s (%s)", g.minute, lastToken(g.player), suffix, lastToken(g.assist));
    else
      snprintf(line, sizeof(line), "%d' %s%s", g.minute, lastToken(g.player), suffix);

    if (g.isHome) {
      if (homeSeen++ >= homeSkip) tft.drawString(line, homeX, HERO_GOALS_Y + homeRow++ * rowH);
    } else {
      if (awaySeen++ >= awaySkip) tft.drawString(line, awayX, HERO_GOALS_Y + awayRow++ * rowH);
    }
  }
}

// Keeps the goals strip in sync with the featured match: re-fetches when the
// match changes or its goal tally has moved on since the last look, then
// (re)draws. A no-score match (upcoming) just clears the strip once.
static void updateHeroGoals(const Match &m) {
  if (!matchHasScore(m)) {
    if (m.id != gEventsMatchId) { gGoalEventCount = 0; gEventsMatchId = m.id; gEventsGoalTotal = -1; }
  } else {
    int goalTotal = m.homeGoals + m.awayGoals;
    if (m.id != gEventsMatchId || goalTotal != gEventsGoalTotal) fetchGoalEvents(m.id, goalTotal);
  }
  drawHeroGoals();
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
  setUiFont(s, 16);
  s.setTextColor(COL_BG, badgeCol);
  s.drawString(badge, 8 + 35, midY);

  // Competition code
  s.setTextDatum(ML_DATUM);
  s.setTextColor(COL_DIM, rowBg);
  setUiFont(s, 16);
  s.drawString(m.comp, 90, midY);

  // Home team (right aligned toward centre)
  setUiFont(s, 20);
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
  setUiFont(s, 20);
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
// the moment we see Halftime -> "2nd half" (Option D). Resets when the match changes.
static void trackHalfTime(const Match &m) {
  if (m.id != gTrackedId) {
    gTrackedId      = m.id;
    gSecondHalfKO   = 0;
    gLastPausedSeen = 0;
    gPrevStatus[0]  = '\0';
  }
  time_t now = time(nullptr);
  bool paused = isHalftime(m);
  if (paused) gLastPausedSeen = now;   // refresh each break poll

  bool wasPaused = (strcmp(gPrevStatus, "HT") == 0);
  if (wasPaused && m.secondHalf && gSecondHalfKO == 0) {
    // The real restart happened somewhere between the last poll that still saw
    // Halftime and this one. Anchor to the midpoint of that window so the clock is
    // off by at most half a poll interval, instead of trailing the whole one.
    gSecondHalfKO = (gLastPausedSeen > 0) ? (gLastPausedSeen + now) / 2 : now;
    Serial.printf("[CLOCK] 2nd-half kickoff anchored for match %ld\n", m.id);
  }
  strcpy(gPrevStatus, paused ? "HT" : "");
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
static bool predFinished(const Match &m) { return !strcmp(m.status, "finished"); }

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
  updateHeroGoals(matches[0]);

  heroShown  = true;
  heroIsLive = isLive(matches[0]);
}

// ----------------------------------------------------------------------------
//  Line-ups page: starting XI + formation for both teams in the featured
//  match, fetched from GET /match/{id}/lineups on the local server.
// ----------------------------------------------------------------------------
struct LineupPlayer {
  char name[23];
  char pos;     // 'G' / 'D' / 'M' / 'F'
  int  jersey;
  long id;      // SofaScore player id -> GET /player/{id}/image
};
struct TeamLineup {
  char formation[8];
  char teamName[26];
  LineupPlayer starters[11];
  int  count;
};
static TeamLineup gHomeLineup, gAwayLineup;
static long gLineupMatchId = -1;   // id whose lineups are currently loaded
static bool gLineupOk      = false;

// Starting XI only (substitute == false) - subs don't fit the compact layout.
static void parseLineupTeam(JsonObjectConst teamObj, TeamLineup &out, const char *teamName) {
  const char *f = teamObj["formation"] | "";
  strncpy(out.formation, f, sizeof(out.formation) - 1); out.formation[sizeof(out.formation) - 1] = '\0';
  strncpy(out.teamName, teamName, sizeof(out.teamName) - 1); out.teamName[sizeof(out.teamName) - 1] = '\0';

  out.count = 0;
  for (JsonObjectConst p : teamObj["players"].as<JsonArrayConst>()) {
    if (out.count >= 11) break;
    if (p["substitute"] | true) continue;   // skip subs, missing flag treated as sub

    LineupPlayer &lp = out.starters[out.count];
    const char *nm = p["name"] | "";
    strncpy(lp.name, nm, sizeof(lp.name) - 1); lp.name[sizeof(lp.name) - 1] = '\0';
    const char *pos = p["position"] | "";
    lp.pos = pos[0] ? pos[0] : '?';
    lp.jersey = atoi(p["jerseyNumber"] | "0");
    lp.id = p["id"] | 0L;
    out.count++;
  }
}

// Fetch + parse the lineups for one match. Returns true if at least one side
// came back with starters (some matches have no confirmed lineup yet).
static bool fetchLineups(long matchId) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(12000);

  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) +
               "/match/" + String(matchId) + "/lineups";
  Serial.printf("[HTTP] GET %s\n", url.c_str());

  gLineupMatchId = matchId;
  gLineupOk = false;
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  Serial.printf("[LINEUP] status=%d\n", code);
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  JsonDocument filter;
  filter["home"]["formation"]                  = true;
  filter["home"]["players"][0]["name"]         = true;
  filter["home"]["players"][0]["position"]     = true;
  filter["home"]["players"][0]["jerseyNumber"] = true;
  filter["home"]["players"][0]["substitute"]   = true;
  filter["home"]["players"][0]["id"]           = true;
  filter["away"]["formation"]                  = true;
  filter["away"]["players"][0]["name"]         = true;
  filter["away"]["players"][0]["position"]     = true;
  filter["away"]["players"][0]["jerseyNumber"] = true;
  filter["away"]["players"][0]["substitute"]   = true;
  filter["away"]["players"][0]["id"]           = true;

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[LINEUP] JSON error: %s\n", err.c_str());
    return false;
  }

  parseLineupTeam(doc["home"], gHomeLineup, matches[0].homeName);
  parseLineupTeam(doc["away"], gAwayLineup, matches[0].awayName);
  gLineupOk = (gHomeLineup.count > 0 || gAwayLineup.count > 0);
  return gLineupOk;
}

// Two columns (home | away), each: team name + formation header, then the
// starting XI as jersey number + name, colour-coded by position.
static void drawLineups() {
  tft.fillRect(0, CONTENT_TOP, SCREEN_W, CONTENT_H, COL_BG);

  const Match &featured = matches[0];
  if (gLineupMatchId != featured.id) fetchLineups(featured.id);   // blocking, once per match

  if (!gLineupOk) { drawMessage("Lineups unavailable", COL_DIM); return; }

  int midX = SCREEN_W / 2;
  tft.drawFastVLine(midX, CONTENT_TOP, CONTENT_H, COL_CARD_TOP);

  const int headerH = 20;
  int rowH = (CONTENT_H - headerH) / 11;

  const TeamLineup *teams[2] = { &gHomeLineup, &gAwayLineup };
  int colX[2] = { 4, midX + 4 };

  for (int c = 0; c < 2; c++) {
    const TeamLineup &t = *teams[c];

    char hdr[36];
    snprintf(hdr, sizeof(hdr), "%s (%s)", t.teamName, t.formation);
    tft.setTextDatum(TL_DATUM);
    setUiFont(16);
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString(hdr, colX[c] + 4, CONTENT_TOP + 2);

    for (int i = 0; i < t.count; i++) {
      const LineupPlayer &p = t.starters[i];
      int y = CONTENT_TOP + headerH + i * rowH + rowH / 2;
      uint16_t posCol = (p.pos == 'G') ? COL_HT
                       : (p.pos == 'D') ? COL_ACCENT
                       : (p.pos == 'M') ? COL_SCHED
                                        : COL_LIVE;   // 'F' and anything else

      char num[4];
      snprintf(num, sizeof(num), "%d", p.jersey);
      tft.setTextDatum(ML_DATUM);
      setUiFont(16);
      tft.setTextColor(posCol, COL_BG);
      tft.drawString(num, colX[c] + 4, y);

      tft.setTextColor(COL_TEXT, COL_BG);
      tft.drawString(p.name, colX[c] + 30, y);
    }
  }
}

// ----------------------------------------------------------------------------
//  Photos page: both teams' starting XI shown at once on a single horizontal
//  pitch, players positioned by formation line (GK -> defence -> ... ->
//  attack), home attacking rightwards, away mirrored attacking leftwards -
//  the two front lines meet at the halfway circle, matching how SofaScore's
//  own lineup graphic reads. Headshots come from GET /player/{id}/image on
//  the local server (a real 64x64 PNG - the server already decoded
//  SofaScore's WebP and downscaled it, cached 24h).
// ----------------------------------------------------------------------------
static const int PHOTO_DIA = 32;   // circular headshot diameter on the pitch

static TFT_eSprite *gHomePhotoSpr[11] = { nullptr };
static TFT_eSprite *gAwayPhotoSpr[11] = { nullptr };
static int          gHomePhotoW[11] = { 0 }, gHomePhotoH[11] = { 0 };
static int          gAwayPhotoW[11] = { 0 }, gAwayPhotoH[11] = { 0 };
static long         gPhotoMatchId = -1;   // match whose photos are currently loaded

// Fetch one player's headshot into spr. Returns false (spr left empty) on any
// network/decode failure - caller draws a lettered placeholder disc instead,
// same fallback style as team flags.
static bool fetchPlayerPhoto(long playerId, TFT_eSprite &spr, int &w, int &h) {
  if (playerId <= 0 || WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) +
               "/player/" + String(playerId) + "/image";

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  int sz = http.getSize();
  if (sz <= 0 || sz > 20000) { http.end(); return false; }   // sanity cap
  uint8_t *buf = (uint8_t *)malloc(sz);
  if (!buf) { http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();
  int total = 0;
  uint32_t deadline = millis() + 8000;
  while (total < sz && millis() < deadline) {
    int n = stream->read(buf + total, sz - total);
    if (n > 0) total += n;
    else if (!http.connected() && stream->available() == 0) break;
  }
  http.end();
  if (total != sz) { free(buf); return false; }

  bool ok = false;
  g_png = new PNG();
  if (g_png) {
    if (g_png->openRAM(buf, sz, pngDraw) == PNG_SUCCESS) {
      w = g_png->getWidth();
      h = g_png->getHeight();
      if (w && h && w <= 200 && spr.createSprite(w, h)) {
        g_pngTarget = &spr;
        g_pngBkgd = 0x000000;   // photos are real RGBA cutouts - blend onto COL_BG (black)
        ok = (g_png->decode(nullptr, 0) == PNG_SUCCESS);
        g_pngTarget = nullptr;
      }
    }
    g_png->close();
    delete g_png;
    g_png = nullptr;
  }
  free(buf);
  return ok;
}

// Free any thumbnails from a previously-loaded match before fetching new ones.
static void clearPhotoSprites(TFT_eSprite **spr, int *ws, int *hs) {
  for (int i = 0; i < 11; i++) {
    if (spr[i]) { spr[i]->deleteSprite(); delete spr[i]; spr[i] = nullptr; }
    ws[i] = hs[i] = 0;
  }
}

// Blocking fetch+decode of one team's starting XI headshots - only runs once
// per match (same "blocking once" style as fetchLineups), not every redraw.
static void loadPhotosForTeam(const TeamLineup &team, TFT_eSprite **spr, int *ws, int *hs) {
  clearPhotoSprites(spr, ws, hs);
  for (int i = 0; i < team.count && i < 11; i++) {
    spr[i] = new TFT_eSprite(&tft);
    if (!fetchPlayerPhoto(team.starters[i].id, *spr[i], ws[i], hs[i])) {
      spr[i]->deleteSprite();
      delete spr[i];
      spr[i] = nullptr;
    }
  }
}

// Pitch play area (thin border matching the touchlines/halfway circle in
// SofaScore's own lineup graphic - see the reference screenshot).
static const int PITCH_TOP = CONTENT_TOP + 4;
static const int PITCH_BOT = FOOTER_Y - 4;
static const int PITCH_H   = PITCH_BOT - PITCH_TOP;

static void drawPitchLines() {
  int cx = SCREEN_W / 2, cy = PITCH_TOP + PITCH_H / 2;
  tft.drawRect(4, PITCH_TOP, SCREEN_W - 8, PITCH_H, COL_CARD_TOP);
  tft.drawFastVLine(cx, PITCH_TOP, PITCH_H, COL_CARD_TOP);
  tft.drawCircle(cx, cy, 34, COL_CARD_TOP);
}

// Draw one circular headshot (cover-fit + circular mask, same technique as
// drawFlagCircle) plus a jersey-number badge and last name. Falls back to a
// lettered disc when spr is null (fetch/decode failed for that player).
static void drawPlayerCircle(int cx, int cy, TFT_eSprite *spr, int sw, int sh, const LineupPlayer &p) {
  int r = PHOTO_DIA / 2;
  bool haveImg = spr && sw > 0 && sh > 0;

  TFT_eSprite dst(&tft);
  if (dst.createSprite(PHOTO_DIA, PHOTO_DIA)) {
    dst.fillSprite(COL_BG);
    float scale = haveImg ? max((float)PHOTO_DIA / sw, (float)PHOTO_DIA / sh) : 1.0f;
    int offX = haveImg ? (int)(sw * scale - PHOTO_DIA) / 2 : 0;
    int offY = haveImg ? (int)(sh * scale - PHOTO_DIA) / 2 : 0;

    for (int y = 0; y < PHOTO_DIA; y++) {
      int dy2 = y - r;
      for (int x = 0; x < PHOTO_DIA; x++) {
        int dx2 = x - r;
        if (dx2 * dx2 + dy2 * dy2 > r * r) continue;   // outside circle
        uint16_t c = COL_FLAG_BG;
        if (haveImg) {
          int sx = (int)((x + offX) / scale), sy = (int)((y + offY) / scale);
          if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
          if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
          c = spr->readPixel(sx, sy);
        }
        dst.drawPixel(x, y, c);
      }
    }
    dst.pushSprite(cx - r, cy - r, COL_BG);
    dst.deleteSprite();
  }
  tft.drawCircle(cx, cy, r, COL_CARD_TOP);

  char num[4];
  snprintf(num, sizeof(num), "%d", p.jersey);
  if (!haveImg) {
    tft.setTextDatum(MC_DATUM);
    useTinyFont(tft);
    tft.setTextColor(COL_TEXT, COL_FLAG_BG);
    tft.drawString(num, cx, cy);
  }

  // Jersey-number badge sitting just below the circle, tangent to its edge
  // (not overlapping the photo - the earlier corner-overlap placement let the
  // number spill outside the badge onto the face).
  int by = cy + r + 6;
  tft.fillRoundRect(cx - 8, by - 6, 16, 12, 3, COL_HT);
  tft.setTextDatum(MC_DATUM);
  useTinyFont(tft);
  tft.setTextColor(COL_BG, COL_HT);
  tft.drawString(num, cx, by);
}

// Parse "4-2-3-1" -> {4,2,3,1}. Returns the number of outfield lines (the
// keeper isn't part of the formation string - the API always gives exactly
// one, handled as an implicit extra column by the caller).
static int parseFormationLines(const char *formation, int *lines, int maxLines) {
  int n = 0, cur = 0;
  bool any = false;
  for (const char *p = formation; ; p++) {
    if (*p >= '0' && *p <= '9') { cur = cur * 10 + (*p - '0'); any = true; }
    else {
      if (any && n < maxLines) { lines[n++] = cur; cur = 0; any = false; }
      if (*p == '\0') break;
    }
  }
  return n;
}

// Lay out one team's starters by formation line: column 0 is the keeper,
// columns 1..N are the outfield lines in defence -> attack order, matching
// the order players already appear in team.starters[] (SofaScore returns
// them in that same formation-slot order). home attacks rightwards; away is
// mirrored so both front lines end up near the halfway circle.
static void drawTeamFormation(const TeamLineup &team, TFT_eSprite **spr, int *ws, int *hs, bool mirrored) {
  int outfield[6];
  int nLines = parseFormationLines(team.formation, outfield, 6);
  if (nLines == 0 || team.count == 0) return;

  int totalCols = nLines + 1;
  int edgeX = mirrored ? SCREEN_W - 30 : 30;             // own goal line
  int nearX = mirrored ? SCREEN_W / 2 + 26 : SCREEN_W / 2 - 26;   // just short of halfway

  int idx = 0;
  for (int col = 0; col < totalCols && idx < team.count; col++) {
    int lineCount = (col == 0) ? 1 : outfield[col - 1];
    float t = (totalCols == 1) ? 0.f : (float)col / (totalCols - 1);
    int x = edgeX + (int)((nearX - edgeX) * t);

    for (int j = 0; j < lineCount && idx < team.count; j++, idx++) {
      int y = PITCH_TOP + (int)(PITCH_H * (float)(j + 1) / (lineCount + 1));
      drawPlayerCircle(x, y, spr[idx], ws[idx], hs[idx], team.starters[idx]);
    }
  }
}

// Both teams' starting XI on one pitch - home left-to-right, away mirrored.
static void drawPhotos() {
  tft.fillRect(0, CONTENT_TOP, SCREEN_W, CONTENT_H, COL_BG);

  const Match &featured = matches[0];
  if (gLineupMatchId != featured.id) fetchLineups(featured.id);   // shares the text page's cache
  if (!gLineupOk) { drawMessage("Lineups unavailable", COL_DIM); return; }

  if (gPhotoMatchId != featured.id) {
    drawMessage("Loading player photos...", COL_DIM);
    loadPhotosForTeam(gHomeLineup, gHomePhotoSpr, gHomePhotoW, gHomePhotoH);
    loadPhotosForTeam(gAwayLineup, gAwayPhotoSpr, gAwayPhotoW, gAwayPhotoH);
    gPhotoMatchId = featured.id;
    tft.fillRect(0, CONTENT_TOP, SCREEN_W, CONTENT_H, COL_BG);   // clear the loading message
  }

  drawPitchLines();
  drawTeamFormation(gHomeLineup, gHomePhotoSpr, gHomePhotoW, gHomePhotoH, false);
  drawTeamFormation(gAwayLineup, gAwayPhotoSpr, gAwayPhotoW, gAwayPhotoH, true);
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
    mix((uint32_t)(m.minute + 1));   // ticks every live fetch; +1 so "-1" (absent) still mixes
    for (const char *p = m.status; *p; p++) mix((uint8_t)*p);
    // statusDescription too: "Halftime" -> "2nd half" keeps status=="inprogress",
    // so without this the HT->live transition (and the live minute label with
    // it) would never trigger a repaint - the board would just freeze on HT.
    for (const char *p = m.statusDescription; *p; p++) mix((uint8_t)*p);
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
  } else if (gPage == PAGE_LINEUPS) {
    drawLineups();          // clears the band itself
  } else if (gPage == PAGE_PHOTOS) {
    drawPhotos();           // clears the band itself
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
    // NTP for the on-screen clock and for computing the live match minute.
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

// GET /rounds -> the tournament's current round number, plus the next round
// in tournament order. /today only shows matches dated today, which is empty
// on knockout rest days - /fixtures?round=<current> instead returns that
// whole round regardless of date. But the server's "current" pointer can lag
// behind results (e.g. it stays on Quarterfinals even after all 4 QF matches
// finish, until Semifinals actually kicks off), so Results/All would show
// only finished matches with nothing upcoming. Pulling gNextRound too - the
// entry right after "current" in the "rounds" array's tournament order (NOT
// sorted by round number, since e.g. Round of 16 = round 5 comes after Round
// of 32 = round 6) - fills that gap. Best-effort: keeps the last known round
// on failure rather than blanking the display.
static int gCurrentRound = -1;
static int gNextRound    = -1;

static bool fetchCurrentRound() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);

  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/rounds";
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  JsonDocument filter;
  filter["current"]["round"]  = true;
  filter["rounds"][0]["round"] = true;

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return false;

  int cur = doc["current"]["round"] | -1;
  if (cur <= 0) return false;
  gCurrentRound = cur;

  gNextRound = -1;
  bool foundCurrent = false;
  for (JsonObjectConst ro : doc["rounds"].as<JsonArrayConst>()) {
    int rr = ro["round"] | -1;
    if (foundCurrent) { gNextRound = rr; break; }
    if (rr == gCurrentRound) foundCurrent = true;
  }
  return true;
}

// Build the request URL for the local server (plain HTTP, no auth needed -
// it's only reachable on the local network). Uses the current round's full
// fixture list once known; falls back to /today until the first successful
// /rounds lookup.
static String buildUrl() {
  String url = "http://";
  url += SERVER_HOST;
  url += ":";
  url += String(SERVER_PORT);
  if (gCurrentRound > 0) url += "/fixtures?round=" + String(gCurrentRound);
  else                   url += "/today";
  return url;
}

// GET one matches-list endpoint and parse it into matches[]. When append is
// false this resets matchCount (the primary/current-round fetch, whose
// success also drives statusLine/gOnline); when true it adds on top of
// whatever's already there (the next-round top-up) and stays silent on
// failure - that round may simply not exist yet (e.g. no Final until the
// Semifinals are done).
static bool fetchMatchesFromUrl(const String &url, bool append) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(12000);

  Serial.printf("[HTTP] GET %s\n", url.c_str());
  if (!http.begin(client, url)) {
    if (!append) { strcpy(statusLine, "HTTP begin failed"); Serial.println("[HTTP] begin() failed"); }
    return false;
  }

  int code = http.GET();
  Serial.printf("[HTTP] status=%d  contentLength=%d\n", code, http.getSize());
  if (!append) gOnline = (code == HTTP_CODE_OK);
  if (code != HTTP_CODE_OK) {
    if (!append) {
      if (code == 404)     strcpy(statusLine, "404 - check /fixtures or /today on the server");
      else if (code < 0)   snprintf(statusLine, sizeof(statusLine), "No server (%d) - check SERVER_HOST/PORT", code);
      else                 snprintf(statusLine, sizeof(statusLine), "HTTP error %d", code);
    }
    http.end();
    return false;
  }

  // Only pull the fields we render -> keeps RAM use small.
  JsonDocument filter;
  filter["matches"][0]["id"]                = true;
  filter["matches"][0]["round"]             = true;
  filter["matches"][0]["group"]             = true;
  filter["matches"][0]["home"]              = true;
  filter["matches"][0]["away"]              = true;
  filter["matches"][0]["homeScore"]         = true;
  filter["matches"][0]["awayScore"]         = true;
  filter["matches"][0]["status"]            = true;
  filter["matches"][0]["statusDescription"] = true;
  filter["matches"][0]["startTimestamp"]    = true;
  filter["matches"][0]["minute"]            = true;

  Serial.printf("[MEM] free heap before read: %u\n", ESP.getFreeHeap());
  String payload = http.getString();
  http.end();
  Serial.printf("[HTTP] body %u bytes\n", payload.length());

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (err) {
    if (!append) snprintf(statusLine, sizeof(statusLine), "JSON error: %s", err.c_str());
    Serial.printf("[JSON] %s\n", err.c_str());
    return false;
  }

  if (!append) matchCount = 0;

  JsonArrayConst arr = doc["matches"].as<JsonArrayConst>();
  for (JsonObjectConst mo : arr) {
    if (matchCount >= MAX_MATCHES) break;

    long id = mo["id"] | 0L;
    bool dup = false;
    for (int i = 0; i < matchCount; i++) if (matches[i].id == id) { dup = true; break; }
    if (dup) continue;   // current/next round fetches shouldn't overlap, but be safe

    Match &m = matches[matchCount];

    m.id = mo["id"] | 0L;

    const char *homeName = mo["home"] | "";
    const char *awayName = mo["away"] | "";
    strncpy(m.homeName, homeName, sizeof(m.homeName) - 1); m.homeName[sizeof(m.homeName) - 1] = '\0';
    strncpy(m.awayName, awayName, sizeof(m.awayName) - 1); m.awayName[sizeof(m.awayName) - 1] = '\0';
    makeShortId(homeName, m.home);
    makeShortId(awayName, m.away);

    const char *st = mo["status"] | "notstarted";
    strncpy(m.status, st, sizeof(m.status) - 1); m.status[sizeof(m.status) - 1] = '\0';
    const char *sd = mo["statusDescription"] | "";
    strncpy(m.statusDescription, sd, sizeof(m.statusDescription) - 1);
    m.statusDescription[sizeof(m.statusDescription) - 1] = '\0';

    m.round = mo["round"] | 0;
    const char *gp = mo["group"] | "";
    strncpy(m.group, gp, sizeof(m.group) - 1); m.group[sizeof(m.group) - 1] = '\0';

    m.homeGoals = mo["homeScore"] | 0;
    m.awayGoals = mo["awayScore"] | 0;
    m.koEpoch   = (time_t)(mo["startTimestamp"] | 0L);
    m.minute    = mo["minute"] | -1;   // null (HT/pre-KO/FT/penalties) -> -1
    m.secondHalf = containsCI(m.statusDescription, "2nd half");
    epochToLocal(m.koEpoch, m.kickoff);

    // Short round/group label for the compact row, e.g. "GRP A" or "R27".
    const RoundName *rn = findRoundName(m.round);
    if (m.group[0]) snprintf(m.comp, sizeof(m.comp), "GRP %c", m.group[strlen(m.group) - 1]);
    else if (rn)    strncpy(m.comp, rn->shortLabel, sizeof(m.comp) - 1);
    else             snprintf(m.comp, sizeof(m.comp), "R%d", m.round);
    m.comp[sizeof(m.comp) - 1] = '\0';

    matchCount++;
  }

  Serial.printf("[PARSE] %d matches (append=%d), now %d total\n", arr.size(), (int)append, matchCount);
  return true;
}

// Overlay fresher live-match fields from GET /live (20s server cache) onto
// whatever fetchMatchesFromUrl() already populated from /fixtures?round=N
// (5min server cache). Without this, a live match's minute/status/score only
// actually changes every 5 minutes no matter how often the firmware polls,
// since /fixtures keeps returning the same cached response until its TTL
// expires. /live is tiny (~24 bytes idle, low hundreds per live match) so
// it's cheap to call every cycle regardless of whether anything is live yet.
static void applyLiveOverlay() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/live";
  if (!http.begin(client, url)) return;

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return; }

  JsonDocument filter;
  filter["matches"][0]["id"]                = true;
  filter["matches"][0]["homeScore"]         = true;
  filter["matches"][0]["awayScore"]         = true;
  filter["matches"][0]["status"]            = true;
  filter["matches"][0]["statusDescription"] = true;
  filter["matches"][0]["minute"]            = true;

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return;

  for (JsonObjectConst mo : doc["matches"].as<JsonArrayConst>()) {
    long id = mo["id"] | 0L;
    for (int i = 0; i < matchCount; i++) {
      if (matches[i].id != id) continue;
      Match &m = matches[i];
      m.homeGoals = mo["homeScore"] | m.homeGoals;
      m.awayGoals = mo["awayScore"] | m.awayGoals;

      const char *st = mo["status"] | m.status;
      strncpy(m.status, st, sizeof(m.status) - 1); m.status[sizeof(m.status) - 1] = '\0';
      const char *sd = mo["statusDescription"] | m.statusDescription;
      strncpy(m.statusDescription, sd, sizeof(m.statusDescription) - 1);
      m.statusDescription[sizeof(m.statusDescription) - 1] = '\0';

      m.minute     = mo["minute"] | -1;
      m.secondHalf = containsCI(m.statusDescription, "2nd half");
      break;
    }
  }
}

// Fetch + parse matches: the current round (or /today until the first
// /rounds lookup succeeds), topped up with the next round in tournament
// order so Results/All still show upcoming fixtures once every match in the
// current round has finished. Returns true if the primary fetch succeeded.
static bool fetchMatches() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) { gOnline = false; return false; }
  }

  fetchCurrentRound();   // best-effort; keeps gCurrentRound/gNextRound fresh across round transitions

  bool ok = fetchMatchesFromUrl(buildUrl(), false);

  if (ok && gCurrentRound > 0 && gNextRound > 0) {
    String nextUrl = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) +
                      "/fixtures?round=" + String(gNextRound);
    fetchMatchesFromUrl(nextUrl, true);   // best-effort top-up, ignore failure
  }

  if (!ok) return false;

  applyLiveOverlay();   // best-effort; refreshes live fields ahead of the 5min /fixtures cache

  sortMatches();

  // Watch the top match's half-time transition on EVERY poll (independent of
  // which page is showing) so the 2nd-half clock is anchored reliably, and note
  // whether anything is live so loop() can poll faster while it matters.
  gAnyLive = false;
  for (int i = 0; i < matchCount; i++)
    if (isLive(matches[i])) { gAnyLive = true; break; }
  if (matchCount > 0) trackHalfTime(matches[0]);

  if (matchCount == 0) strcpy(statusLine, "No matches right now");
  return true;
}

// ----------------------------------------------------------------------------
//  Touch (TFT_eSPI built-in driver, via TOUCH_CS in User_Setup.h).
//
//  First flash: TOUCH_CALIBRATED is 0 in config.h, so this runs the on-screen
//  "touch corners" step and prints the resulting 5 values to Serial. Copy
//  those into TOUCH_CAL_DATA in config.h, set TOUCH_CALIBRATED to 1, reflash -
//  the calibration step is then skipped on every future boot.
// ----------------------------------------------------------------------------
static void setupTouch() {
#if TOUCH_CALIBRATED
  uint16_t calData[5] = TOUCH_CAL_DATA;
  tft.setTouch(calData);
  Serial.println("[TOUCH] using calibration from config.h");
#else
  uint16_t calData[5];
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextFont(2);
  tft.setCursor(20, 0);
  tft.println("Touch corners as indicated");
  tft.calibrateTouch(calData, TFT_MAGENTA, COL_BG, 15);

  Serial.print("[TOUCH] copy into config.h:  #define TOUCH_CAL_DATA { ");
  for (uint8_t i = 0; i < 5; i++) {
    Serial.print(calData[i]);
    if (i < 4) Serial.print(", ");
  }
  Serial.println(" }   then set TOUCH_CALIBRATED to 1");

  tft.setTouch(calData);
  tft.fillScreen(COL_BG);
#endif
}

// Poll the touchscreen; if an arrow button was tapped, switch pages and redraw.
static void handleTouch() {
#if !TOUCH_ENABLED
  return;                                   // touch on hold - no polling at all
#endif
  uint16_t tx = 0, ty = 0;
#if TOUCH_DEBUG
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 200) {
    lastDbg = millis();
    bool t = tft.getTouch(&tx, &ty);
    Serial.printf("[TOUCH] touched=%d  x=%4d y=%4d\n", t, tx, ty);
  }
  return;
#endif
  static bool     down = false;
  static uint32_t lastAct = 0;
  bool pressed = tft.getTouch(&tx, &ty);

  if (pressed && !down && millis() - lastAct > 250) {
    down = true;
    int sx = tx, sy = ty;
    if (sy >= FOOTER_Y) {                       // only the footer band navigates
      int prev = gPage;
      if (sx <= NAV_ZONE_W)                 gPage = (gPage + PAGE_COUNT - 1) % PAGE_COUNT;
      else if (sx >= SCREEN_W - NAV_ZONE_W) gPage = (gPage + 1) % PAGE_COUNT;
      if (gPage != prev) {
        Serial.printf("[NAV] page -> %s (screen %d,%d)\n",
                      kPageTitle[gPage], sx, sy);
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
  // live we poll every ~20s so the Halftime -> 2nd-half restart is caught
  // quickly and the 2nd-half clock anchors tightly; otherwise we use the
  // configured interval.
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
