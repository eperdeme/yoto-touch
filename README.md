# yoto-touch

A fully standalone kid-friendly touchscreen remote for the
[Yoto Player](https://yotoplay.com). Built for a 7-year-old with ~200 cards
who needs a tidy way to pick what to play without leaving cards strewn across
her bedroom floor.

## Architecture

```
 ┌────────────────────────┐         HTTPS           ┌──────────────┐
 │ CrowPanel 7" ESP32-S3  │  ◀─────────────────────▶│  Yoto cloud  │
 │ Arduino + LVGL 9       │   OAuth2 device-flow    │ login.yoto.. │
 │ Touch UI for the kid   │   REST API              │ api.yoto..   │
 └────────────────────────┘                         └──────────────┘
```

No companion server needed — the ESP32 talks directly to the Yoto API over
TLS, handles OAuth2 device-code auth with on-screen QR code, caches the
library manifest and cover thumbnails to microSD, and sends play/pause/volume
commands straight to the player.

Uses the official Yoto API documented at <https://yoto.dev/api/>:

- Device-flow auth: `POST https://login.yotoplay.com/oauth/device/code` + `/oauth/token`
- Library: `GET /card/family/library` for purchased/shared cards, fallback
  `GET /content/mine` for MYO cards, best-effort `GET /content/{cardId}` for
  MYO detail enrichment
- Devices: `GET /device-v2/devices/mine`
- Commands: `POST /device-v2/{id}/command/card/start`, `card/pause`, `card/resume`,
  `card/stop`, `volume/set`

## Hardware

| Component | Details |
| --------- | ------- |
| Board | [Elecrow CrowPanel 7.0" HMI](https://www.elecrow.com/crowpanel-esp32-display.html) — ESP32-S3-N4R8 (4 MB flash, 8 MB octal PSRAM) |
| Display | 800×480 RGB parallel panel, LVGL 9 direct-render mode |
| Touch | GT911 capacitive, I2C |
| Storage | microSD on HSPI (SPI mutex for thread safety) |

## Features

- **4×2 cover grid** — paginated library with 144×144 cover thumbnails
- **On-device OAuth** — QR code displayed on screen, scan with phone to sign in
- **Persistent auth** — tokens stored in NVS, auto-refresh
- **SD cover cache** — covers saved as RGB565 to microSD, instant reload on boot
- **SD manifest cache** — library metadata persisted to microSD, offline browsing
- **PNG cover decode** — CDN covers resized via `?width=144&quality=70`,
  decoded from PNG using pngle
- **Player controls** — play, pause, resume, stop, volume
- **Card detail view** — tap a card for description, chapter count, series info
- **Filter & sort** — by author, series, title A-Z / Z-A
- **Idle dimming** — backlight dims after 1 min, off after 5 min, wake on touch
- **OTA updates** — ElegantOTA web server for firmware updates over WiFi
- **Background prefetch** — adjacent pages pre-cached while idle

## Official, Shared, And MYO Cards

`GET /card/family/library` returns the family library catalogue, including
purchased Yoto store cards (`shareType: "yoto"`), physically/digitally shared
cards, and MYO cards. The firmware uses this as the primary catalogue source and
parses only the fields needed for display and playback.

`GET /content/{cardId}` is still used only as best-effort enrichment for MYO
cards. It is not reliable for purchased store cards because those content
objects are owned by Yoto, so store-card detail comes from the family-library
response itself.

## Repo layout

| Path | What |
| ---- | ---- |
| `firmware/arduino/` | PlatformIO project — Arduino + LVGL 9 firmware |
| `firmware/arduino/src/main.cpp` | UI, net task, boot flow, grid rendering |
| `firmware/arduino/src/yoto_api.cpp` | Yoto REST client — auth, library, covers, commands |
| `firmware/arduino/src/sd_cache.cpp` | SD card cache — covers (RGB565) + manifest |
| `firmware/arduino/include/pins.h` | CrowPanel 7" pin map |
| `firmware/arduino/include/lv_conf.h` | LVGL 9 configuration |

## Quick start

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- An Elecrow CrowPanel 7.0" HMI (ESP32-S3-N4R8)
- A Yoto API client ID from <https://dashboard.yoto.dev/> (create a "Public Client")
- A microSD card (any size, FAT32 or exFAT)

### Build & flash

```bash
cd firmware/arduino

# Create secrets file
cp src/secrets.h.example src/secrets.h
# Edit src/secrets.h — fill in WIFI_SSID, WIFI_PASSWORD, YOTO_CLIENT_ID

# Build and upload
pio run -t upload

# Monitor serial
pio device monitor -b 115200
```

### First run — sign in

On first boot (or after clearing tokens), the screen shows a QR code and a
user code. Scan the QR code with your phone, sign in with your Yoto account,
and the device picks up the tokens automatically. Credentials are stored in
NVS and refreshed forever after.

## How it works

1. **Boot** — mounts SD, connects WiFi, disables WiFi power-save (prevents
   PHY timer crashes during TLS)
2. **Auth** — checks NVS for saved tokens, refreshes if expired, starts
   device-code flow if none
3. **Library fetch** — pulls card list from Yoto API, saves manifest to SD
4. **Cover fetch** — downloads PNG covers from Yoto CDN (resized server-side),
   decodes to RGB565, caches to SD. Net task runs on core 0 to avoid blocking
   LVGL on core 1
5. **UI** — LVGL 9 grid with touch navigation. Tap a card to see details or
   play it

## Key technical details

- **Dual-core split**: LVGL UI runs on core 1 (Arduino loop), all network I/O
  on core 0 via a FreeRTOS task with a command queue
- **SPI mutex**: SD card on HSPI is protected by a mutex since both cores access it
- **Cover cache race safety**: `netFetchPageIntoCache` snapshots job pointers under
  lock, re-validates before each write to handle concurrent eviction
- **PSRAM routing**: all LVGL allocations go through a custom allocator into 8 MB
  octal PSRAM (`lv_mem_psram.cpp`)
- **WiFi PS disabled**: `esp_wifi_set_ps(WIFI_PS_NONE)` prevents PHY timer
  allocation failures during TLS handshakes

## Security notes

- OAuth tokens are stored in ESP32 NVS (on-chip flash). No credentials are
  transmitted in plaintext or stored on the SD card.
- TLS is used for all API calls. Certificate validation is currently disabled
  (`setInsecure()`) — a future improvement.
- OTA update server is unauthenticated on the local network. Don't expose it
  to the internet.
- Don't commit `src/secrets.h`.
