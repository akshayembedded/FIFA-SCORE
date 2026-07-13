#pragma once

// =============================================================================
//  USER CONFIGURATION  -  edit the values below, then upload.
// =============================================================================

// --- WiFi ---------------------------------------------------------------------
#define WIFI_SSID      "SargasanHome"
#define WIFI_PASSWORD  "Sargasan@123#"

// --- Local FIFA World Cup server ------------------------------------------------
// This firmware polls the local Python scraper/API server (see README.md), not
// football-data.org. The ESP32 and the PC running `python main.py` must be on
// the same WiFi network. SERVER_HOST is that PC's LAN IP (find it with
// `ipconfig`, NOT "localhost"/"127.0.0.1" - that would point at the ESP32
// itself). SERVER_PORT matches the server's PORT env var (8000 by default).
#define SERVER_HOST    "192.168.29.196"
#define SERVER_PORT    8000

// No endpoint to configure here: the firmware asks GET /rounds for the
// tournament's current round, then pulls GET /fixtures?round=<that> - it
// covers live + finished + upcoming matches for the whole round regardless
// of date, so Results/All still have data on rest days between rounds
// (unlike /today, which is empty whenever nothing's scheduled today).

// How often to refresh, in seconds. The server itself caches responses
// (20s for /live, 5min for others), so there's no need to go much below that.
#define REFRESH_SECONDS  30

// --- Local time ---------------------------------------------------------------
// Offset from UTC for displaying kickoff times & the clock, in minutes.
// India (IST) = +5:30 = 330.  UK = 0.  Central Europe (CET) = 60.
#define TZ_OFFSET_MINUTES  330

// --- Display ------------------------------------------------------------------
// 1 = USB port on the left (landscape), 3 = USB on the right (landscape flipped).
#define SCREEN_ROTATION  1

// --- Touch calibration ---------------------------------------------------------
// Leave TOUCH_CALIBRATED at 0 for the first flash: the screen will show
// "Touch corners as indicated" and print 5 values to the Serial Monitor
// (115200 baud). Copy those into TOUCH_CAL_DATA below, set TOUCH_CALIBRATED
// to 1, and reflash - the calibration step won't show again.
#define TOUCH_CALIBRATED  1
 #define TOUCH_CAL_DATA { 358, 3309, 357, 3007, 7 }
