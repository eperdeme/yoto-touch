# Project Guidelines

## Architecture

Standalone ESP32-S3 firmware (no companion server). The device talks directly
to the Yoto cloud API over TLS from the ESP32.

- **Dual-core split**: LVGL UI on core 1 (Arduino `loop()`), all network I/O
  on core 0 via a FreeRTOS task (`netTaskFn`) with a command queue.
- **Cover pipeline**: Yoto CDN URL → append `?width=144&quality=70` → HTTP GET
  with redirect following → PNG decode via pngle → contain/letterbox RGB565
  conversion → SD cache save → LVGL `lv_image_dsc_t` display.
- **SD cache**: covers as raw RGB565 files, manifest as binary. All SD access
  protected by `SdLock` RAII mutex (SPI bus shared between cores).

## Hardware

- **Board**: Elecrow CrowPanel 7" HMI — ESP32-S3-N4R8 (4 MB flash, 8 MB octal PSRAM)
- **Display**: 800×480 RGB parallel panel, LVGL 9 direct-render mode, bounce buffer DMA
- **Touch**: GT911 capacitive on I2C (SDA=19, SCL=20)
- **SD card**: HSPI (CS=10, MOSI=11, SCK=12, MISO=13)
- **Backlight**: GPIO 2, PWM dimming
- **Serial**: CH343 USB-UART on `/dev/cu.wchusbserial110`

## Build and Test

```bash
cd firmware/arduino

# Build and flash
pio run -t upload

# Monitor serial output
pio device monitor -p /dev/cu.wchusbserial110 -b 115200
```

- PlatformIO with pioarduino fork (Arduino-ESP32 v3 / IDF v5)
- Partition: `min_spiffs.csv` — two 1.875 MB OTA app slots, firmware ~90% flash
- C++17 (`-std=gnu++17`)
- No unit tests — verify by flashing and monitoring serial output

## Conventions

### Memory

- All LVGL allocations go through the custom PSRAM allocator (`lv_mem_psram.cpp`).
  Never use `lv_malloc` for non-LVGL buffers; use `ps_malloc` for large PSRAM
  allocations and stack/heap for small internal-DRAM buffers.
- ESP32 SPI DMA cannot read from PSRAM. SD card reads/writes use a 4 KB
  internal-DRAM bounce buffer (`readBounced`/`writeBounced` in `sd_cache.cpp`).
- Cover buffers are 144×144×2 = 41,472 bytes each in PSRAM. Only current
  page ± 1 page kept resident; distant pages evicted and reloaded from SD.

### Thread safety

- `coverCacheMtx` protects the `coverCache` map (read/write from both cores).
- `s_sdMtx` protects all SD card SPI access (RAII `SdLock` in `sd_cache.cpp`).
- Net task must never touch LVGL objects. It writes to shared buffers under
  mutex, sets volatile flags, and the UI loop picks up results.

### Yoto API

- **Before making any changes to API calls, always check the official docs at
  <https://yoto.dev/api/> first.** The API has undocumented quirks (e.g. CDN
  query params, content-type behaviour, scope requirements) that aren't
  obvious from reading the code alone. Fetch the relevant endpoint docs to
  confirm request/response shape, required scopes, and error codes.
- Auth: `login.yotoplay.com`. API: `api.yotoplay.com`.
- Scopes: `offline_access family:library:view family:devices:view family:devices:control user:content:view`
- Tokens stored in NVS namespace `"yoto_auth"`.
- Per-request `WiFiClientSecure` (no persistent TLS session).
- WiFi power-save disabled (`esp_wifi_set_ps(WIFI_PS_NONE)`) to prevent
  PHY timer crashes during TLS handshakes.
- CDN supports `?width=N&height=N&quality=N` (not shorthand `?w=` or `?q=`).
  CDN returns PNG regardless of source format.

### LVGL

- LVGL 9.5.0 with `LV_DISPLAY_RENDER_MODE_DIRECT`.
- Available fonts: Montserrat 12, 14, 16, 20, 24 only.
- Image descriptors (`lv_image_dsc_t`) use `LV_COLOR_FORMAT_RGB565`,
  stride = width × 2 bytes, data in little-endian byte order.
- `LV_USE_LODEPNG 1` enables bundled lodepng for PNG decode.
- `LV_USE_QRCODE 1` for auth QR code display.

### Code style

- Single-file UI in `main.cpp` (large but keeps all LVGL widget code together).
- API client in `yoto_api.cpp` — all functions are blocking, call from net task only.
- Prefer `Serial.printf` for debug logging with `[tag]` prefixes
  (e.g. `[yoto]`, `[cover]`, `[sd]`, `[net]`).
- Use `ps_malloc` / `free` for PSRAM, `new` / `delete` for small structs.
