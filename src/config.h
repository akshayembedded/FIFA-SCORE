#pragma once

// =============================================================================
//  USER CONFIGURATION  -  edit the values below, then upload.
// =============================================================================

// --- WiFi ---------------------------------------------------------------------
#define WIFI_SSID      "Hello"
#define WIFI_PASSWORD  "12345678901"

// --- football-data.org --------------------------------------------------------
// Get a free token at https://www.football-data.org/client/register
// It is sent in the "X-Auth-Token" header on every request.
#define API_TOKEN      "cc66f87687814dfea6415d758c32715d"

// Which competitions to show today's matches for (comma separated, NO spaces).
// Free "Tier One" codes: PL (Premier League), PD (La Liga), SA (Serie A),
// BL1 (Bundesliga), FL1 (Ligue 1), DED (Eredivisie), PPL (Primeira Liga),
// ELC (Championship), CL (Champions League), EC, WC, BSA.
// Leave as "" to show every available competition for today.
#define COMPETITIONS   "WC"

// How often to refresh, in seconds. Free tier allows 10 requests/minute, so
// keep this at 30 or higher to stay well under the limit.
#define REFRESH_SECONDS  60

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
