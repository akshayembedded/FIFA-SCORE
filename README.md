# FIFA World Cup Scraper -> API

Scrapes live FIFA World Cup scores, fixtures, standings, lineups, and match
events from SofaScore's public (unofficial) JSON API, and re-serves them as
a small, ESP32-friendly HTTP API. The ESP32 is meant to poll this server;
nothing is pushed to the device.

This uses SofaScore's undocumented API, not an official/licensed data feed.
It can change or rate-limit without notice — fine for a hobby project, not
for anything production-critical.

## Setup

```
pip install -r requirements.txt
python main.py
```

Server starts on `http://<HOST>:<PORT>` - `HOST`/`PORT` env vars, defaulting
to whatever's set in `config.py` (currently your PC's LAN IP, port 8000, so
the ESP32 can reach it directly). By default it serves the 2026 World Cup;
override with `WC_YEAR=2022` etc.

## The pattern every endpoint follows

No match ID is ever hardcoded, and none exists in advance - a match's ID
only exists once SofaScore schedules it, and a brand new tournament day
means brand new IDs. So every workflow is **two steps**:

1. **Discover**: call `/live`, `/today`, or `/fixtures?round=N`. Each match
   in the response carries an `id`.
2. **Drill in** (optional): take that `id` and call `/match/{id}/lineups`
   or `/match/{id}/events` for player-level detail on that specific match.

```
GET /live                    -> {"matches":[{"id": 12813017, "home": "Norway", ...}]}
GET /match/12813017/events   -> goal/card/sub timeline for that exact match
```

Tomorrow `/live` returns different matches with different IDs; nothing else
about the flow changes.

## Endpoints

Real response sizes below are from actual live testing, to help size
`JsonDocument` buffers / gauge what's cheap vs heavy to poll often.

### `GET /health`
Liveness check. `{"status": "ok"}`

### `GET /live` — matches currently in progress
~24 bytes when nothing's live, low hundreds of bytes per live match.
```json
{
  "count": 1,
  "matches": [
    {
      "id": 12813017,
      "round": 27,
      "group": null,
      "home": "Norway",
      "away": "England",
      "homeScore": 1,
      "awayScore": 1,
      "status": "inprogress",
      "statusDescription": "2nd half",
      "minute": 62,
      "startTimestamp": 1783803600
    }
  ]
}
```
`minute` is only set while `status` is `"inprogress"`; `null` at halftime,
before kickoff, after full time, or during penalties (no single clock to
show then). It's computed from SofaScore's own period-start data, not a
field they hand over directly - a plain running count, no "45+2" injury-time
notation. `group` is `null` for knockout-stage matches.

### `GET /today` — matches starting today, any stage
~400 bytes for a single match. Same match shape as `/live`.

### `GET /fixtures?round=1` — all matches for a round/matchday
~4.8KB for a full 24-match group-stage round; knockout rounds are smaller
(4-16 matches). Works for knockout rounds too - see `/rounds` for their
round numbers, e.g. `/fixtures?round=27` for the Quarterfinals. Response
wraps the same match shape as `/live` in `{"round": N, "count": N, "matches": [...]}`.

### `GET /rounds` — round numbers + names
~450 bytes. Group rounds are just numbered 1/2/3 (no `name`/`slug`);
knockout rounds are non-sequential and named:
```json
{
  "current": {"round": 27, "name": "Quarterfinals", "slug": "quarterfinals"},
  "rounds": [
    {"round": 1}, {"round": 2}, {"round": 3},
    {"round": 6, "name": "Round of 32", "slug": "round-of-32"},
    {"round": 5, "name": "Round of 16", "slug": "round-of-16"},
    {"round": 27, "name": "Quarterfinals", "slug": "quarterfinals"},
    {"round": 28, "name": "Semifinals", "slug": "semifinals"},
    {"round": 50, "name": "Match for 3rd place", "slug": "match-for-3rd-place"},
    {"round": 29, "name": "Final", "slug": "final"}
  ]
}
```

### `GET /standings` or `GET /standings?group=A` — group tables
~500 bytes for one group, ~7.5KB for all groups.
```json
{
  "groups": [
    {
      "group": "Group A",
      "standings": [
        {"position": 1, "team": "Mexico", "played": 3, "wins": 3, "draws": 0,
         "losses": 0, "scoresFor": 6, "scoresAgainst": 0, "points": 9}
      ]
    }
  ]
}
```

### `GET /match/{id}/lineups` — starting XI + subs, both teams
~5.5KB. Needs an `id` from `/live`, `/today`, or `/fixtures` first.
```json
{
  "home": {
    "formation": "4-2-3-1",
    "players": [
      {"name": "Unai Simón", "position": "G", "jerseyNumber": "23",
       "substitute": false, "rating": 6.2, "minutesPlayed": 90}
    ]
  },
  "away": { "formation": "...", "players": [ ... ] }
}
```

### `GET /match/{id}/events` — goal/card/substitution/stoppage timeline
~2.8KB, chronological by `minute`. Needs an `id` too.
```json
{
  "count": 3,
  "events": [
    {"minute": 30, "type": "goal", "team": "home", "player": "Fabián Ruiz",
     "assist": null, "penalty": false, "ownGoal": false},
    {"minute": 43, "type": "card", "team": "home", "player": "Pau Cubarsí",
     "cardType": "yellow"},
    {"minute": 55, "type": "substitution", "team": "home",
     "playerIn": "Pedri", "playerOut": "Fabián Ruiz"},
    {"minute": 66, "type": "delay", "phase": "start",
     "text": "Delay in match because of an injury Thibaut Courtois (Belgium)."}
  ]
}
```

Responses are cached in memory so repeated ESP32 polling doesn't hammer
SofaScore: 20s (`/live`), 60s (`/today`), 30s (`/match/{id}/...`), 5min
(`/fixtures`, `/standings`), 1hr (`/rounds`).

## Suggested ESP32 polling strategy

- Poll `/live` (or `/today` if nothing's live) on a short interval (e.g.
  15-30s) - it's tiny and cheap even every poll.
- Only fetch `/match/{id}/lineups` or `/match/{id}/events` occasionally or
  on demand (e.g. a button press, or once when a match's `id` first
  appears) - they're 10-200x bigger than `/live` and change less often
  (30s cache anyway).
- Watch for `count: 0` / an empty `matches` array - there's often no live
  World Cup match at all, which is normal, not an error.

## ESP32 firmware

Lives in `esp32/` as a PlatformIO project. It connects to WiFi, polls the
server every 15s, and prints matches to Serial - no display wired up yet.

```
cd esp32
cp include/secrets.h.example include/secrets.h
# edit secrets.h: WIFI_SSID, WIFI_PASSWORD, SERVER_HOST (this PC's LAN IP from `ipconfig`)
pio run --target upload
pio device monitor
```

Also make sure:
- The ESP32 and PC are on the same WiFi network.
- Windows Firewall allows inbound connections on port 8000 (or the `python
  main.py` server won't be reachable from the ESP32).
- `/live` will print `(none right now)` outside of an actual live match -
  to see real data while testing, change `ENDPOINT_PATH` in
  `esp32/src/main.cpp` to `/fixtures?round=1`.
