# Firmware — CrowPanel 7" (ESP32-S3 / 800×480 / GT911)

Arduino + LVGL 9 standalone firmware for the kid-facing card picker.
Talks directly to the Yoto API — no companion server required.

## Source files

| File | Role |
| ---- | ---- |
| `arduino/src/main.cpp` | Boot flow, LVGL UI, net task, grid rendering, card detail |
| `arduino/src/yoto_api.cpp` | Yoto REST client — OAuth, library, covers, player commands |
| `arduino/src/yoto_api.h` | API function and struct declarations |
| `arduino/src/sd_cache.cpp` | SD card cache — RGB565 covers + binary manifest |
| `arduino/src/sd_cache.h` | Cache interface and `CardMeta` struct |
| `arduino/src/lv_mem_psram.cpp` | Custom LVGL allocator routing to PSRAM |
| `arduino/src/secrets.h.example` | Template — WiFi creds + Yoto client ID |
| `arduino/include/pins.h` | CrowPanel 7" pin map (RGB panel, GT911, SD, backlight) |
| `arduino/include/lv_conf.h` | LVGL 9 configuration (RGB565, PSRAM allocator, fonts) |
| `arduino/platformio.ini` | PlatformIO build config |

## Toolchain

- **PlatformIO** with [pioarduino](https://github.com/pioarduino/platform-espressif32)
  fork (Arduino-ESP32 v3 / IDF v5)
- **LVGL 9.5.0** — direct render mode into the RGB panel framebuffer
- **Arduino_GFX 1.6.5** — ESP32-S3 RGB parallel panel with bounce buffer DMA
- **SdFat 2.2.3** — FAT32/exFAT on HSPI with RAII mutex for thread safety
- **pngle 1.1.0** — streaming PNG decode for cover thumbnails
- **TJpg_Decoder 1.1.0** — JPEG decode fallback
- **ElegantOTA 3.1.7** — web-based firmware updates

## Build & flash

```bash
cd firmware/arduino

# Create your secrets file
cp src/secrets.h.example src/secrets.h
# Edit: set WIFI_SSID, WIFI_PASSWORD, YOTO_CLIENT_ID

# Build and upload (serial via CH343 USB-UART)
pio run -t upload

# Monitor
pio device monitor -b 115200
```

## Memory budget

| Resource | Budget |
| -------- | ------ |
| Flash | 1.875 MB per OTA slot (`min_spiffs.csv`), firmware ~90% |
| PSRAM | 8 MB octal — framebuffer (~750 KB), LVGL heap, cover cache |
| Internal DRAM | ~190 KB free — WiFi/BT stack, FreeRTOS, SPI DMA |
| SD card | Covers: 41 KB each (144×144 RGB565). Manifest: ~10–50 KB |

## Architecture notes

- **Dual-core**: LVGL UI on core 1 (Arduino loop), all HTTP/TLS on core 0
  (FreeRTOS task, 16 KB stack, priority 2)
- **Cover pipeline**: CDN URL + `?width=144&quality=70` → PNG download →
  pngle streaming decode → contain/letterbox RGB565 conversion → SD cache
- **SD SPI mutex**: RAII `SdLock` struct prevents SPI bus contention between cores
- **WiFi power-save disabled**: `esp_wifi_set_ps(WIFI_PS_NONE)` prevents PHY
  timer allocation failures during TLS handshakes
- **Cover eviction**: only current page ± 1 kept in PSRAM; distant covers
  freed and reloaded from SD on demand
