# yoto-touch

A kid-friendly touchscreen remote for the [Yoto Player](https://yotoplay.com).
Built for a 7-year-old with ~200 cards who needs a tidy way to pick what to play
without leaving cards strewn across her bedroom floor.

## Architecture

```
 ┌────────────────────────┐        HTTP          ┌──────────────────────────┐         HTTPS              ┌──────────────┐
 │ CrowPanel 7" ESP32-S3  │  ◀──────────────▶   │  Companion server        │  ◀────────────────────────▶ │  Yoto cloud  │
 │ MicroPython + LVGL     │   /cards /play ...  │  FastAPI (Python)        │   OAuth2 + REST            │ login.yoto.. │
 │ Touch UI for the kid   │                     │  caches library + art    │                            │ api.yoto..   │
 └────────────────────────┘                     └──────────────────────────┘                            └──────────────┘
```

The companion server does the gnarly bits (OAuth2 device flow, token refresh,
library caching, thumbnail resizing, REST commands to the player) so the
firmware stays small: fetch JSON, draw a grid, send a play command.

Run the server on anything that's always on at home — Raspberry Pi, NAS,
old laptop, or a Docker container on your desktop.

Uses the official Yoto API documented at <https://yoto.dev/api/>, specifically:

- Device-flow auth: `POST https://login.yotoplay.com/oauth/device/code` + `/oauth/token`
- Library: `GET /content/mine`, `GET /content/{cardId}`
- Devices: `GET /device-v2/devices/mine`, `GET /device-v2/{id}/status`
- Commands: `POST /device-v2/{id}/command/card/start`, `card/pause`, `card/resume`,
  `card/stop`, `volume/set`, `sleep-timer/set` (bodies mirror the MQTT payloads).

## ⚠ Important caveat: official cards vs MYO

`GET /content/mine` only returns **MYO (Make Your Own)** cards — the ones you
created in the Yoto app. **Commercial Yoto cards bought from the shop are not
listed by any current public endpoint**.

The good news: once you know a commercial card's `cardId`, you can play it via
`/device-v2/{id}/command/card/start` with `uri: https://yoto.io/<cardId>`.

Workarounds the server already supports:

- `server/data/extra_cards.json` — a local catalogue you can hand-curate. Each
  entry needs at minimum `cardId` and `title`; optionally `metadata.cover.imageL`
  for artwork. The library cache merges these in alongside MYO cards.
- A simple "scan all my cards" workflow: hold each card to the player, watch
  `GET /now-playing` for the resulting `activeCard` value, save it to
  `extra_cards.json` with the title. Slow but one-off.

(Long-term: pre-built community catalogues of UK/US card IDs exist on GitHub —
import one and you're done in seconds.)

## Repo layout

| Path        | What                                                                  |
| ----------- | --------------------------------------------------------------------- |
| `server/`   | FastAPI companion server (Python 3.11+)                               |
| `firmware/` | MicroPython + LVGL firmware for the CrowPanel 7" (ESP32-S3 800×480)   |

## Quick start (server)

1. Create a **public** client at <https://dashboard.yoto.dev/> and copy the
   client ID.
2. ```bash
   cd server
   python -m venv .venv && source .venv/bin/activate
   pip install -e .
   cp .env.example .env       # paste your YOTO_CLIENT_ID
   uvicorn app.main:app --host 0.0.0.0 --port 8000
   ```
3. First run only — sign in:
   ```bash
   curl http://localhost:8000/auth/start
   # → { "user_code": "ZDTT-NSTF",
   #     "verification_uri_complete": "https://login.yotoplay.com/activate?user_code=ZDTT-NSTF", ... }
   # open that URL on your phone and confirm.
   curl http://localhost:8000/auth/poll      # repeat until {"status":"authorized"}
   ```
   Tokens are saved to `server/tokens/tokens.json` and refreshed automatically
   forever after.

### Endpoints

| Method | Path                          | Notes                                          |
| ------ | ----------------------------- | ---------------------------------------------- |
| GET    | `/auth/start`                 | begin device flow (returns user_code + URL)    |
| GET    | `/auth/poll`                  | poll until authorised                          |
| GET    | `/auth/status`                | `{ signed_in: bool }`                          |
| GET    | `/cards?page=&size=&favourites_only=&folder=` | paged library                  |
| GET    | `/cards/{id}`                 | full card detail incl. chapters                |
| GET    | `/thumb/{id}`                 | 200×200 JPEG, cached on disk                   |
| GET    | `/favourites`                 |                                                |
| POST   | `/favourites/{id}`            | toggle                                         |
| GET    | `/folders`                    | `{name: [cardId, ...]}`                        |
| POST   | `/folders/{name}/{id}`        | add card to folder                             |
| DELETE | `/folders/{name}/{id}`        |                                                |
| GET    | `/devices`                    | players on the account                         |
| GET    | `/now-playing`                | device status incl. active card                |
| POST   | `/play/{id}`                  | play card on default (or `?device_id=`) player |
| POST   | `/pause` `/resume` `/stop`    |                                                |
| POST   | `/volume/{0..100}`            |                                                |
| POST   | `/sleep-timer/{seconds}`      | 0 = cancel                                     |

## Quick start (firmware)

The CrowPanel 7" needs a MicroPython build that has LVGL + the RGB panel +
GT911 touch driver compiled in. See [firmware/README.md](firmware/README.md)
for the build / pre-built-image options and the `mpremote` flash recipe.

Once the board boots into MicroPython:

```bash
cd firmware
cp config.py.example config.py    # edit WiFi creds + SERVER_URL
mpremote cp -r . :/
mpremote reset
```

## Folders & favourites for kids

Two simple knobs to tame 200 cards:

- **Favourites** — single tap on a heart in the future UI extension; today
  via `POST /favourites/{cardId}`. Shows on the "Favourites" page in the UI.
- **Folders** — bedtime / music / stories / car / school. Edit
  `server/data/folders.json` directly:
  ```json
  {
    "Bedtime":  ["abc123", "def456"],
    "Adventures": ["..."],
    "Songs":    ["..."]
  }
  ```

## Status

v1 scope: browse grid, tap to play, now-playing controls, favourites, folders.
Not yet: sleep-timer UI, multi-player switching from the UI, MYO content upload,
commercial-card auto-discovery.

## Security notes

- Tokens live on the companion server only. The ESP32 never sees Yoto creds.
- The companion server has **no authentication** in front of it. Don't expose
  port 8000 to the public internet — keep it on your home LAN. If you must
  expose it, put it behind a reverse proxy with basic auth or a Tailscale /
  WireGuard tunnel.
- Don't commit `.env`, `server/tokens/`, or `firmware/config.py`.
