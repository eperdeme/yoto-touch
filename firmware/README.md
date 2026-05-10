# Firmware — CrowPanel 7" (ESP32-S3 / 800x480 / GT911)

MicroPython + LVGL UI for the kid-facing card picker.

## Files

| File                  | Role                                              |
| --------------------- | ------------------------------------------------- |
| `boot.py`             | WiFi connect on boot                              |
| `main.py`             | Entry point — init display, then run UI           |
| `display.py`          | RGB panel + GT911 touch + LVGL bootstrap (board-specific, see below) |
| `ui.py`               | LVGL screen: top bar / grid / bottom bar          |
| `api.py`              | Tiny HTTP client → companion server               |
| `config.py.example`   | WiFi creds + server URL — copy to `config.py`     |

## You need a custom MicroPython build

Stock MicroPython for ESP32-S3 does **not** include LVGL bindings, RGB panel
drivers, or the GT911 touch driver. You need a build that has all three.

The cleanest path right now is **lv_micropython** with the CrowPanel /
ESP32-S3 RGB panel work merged in. Two options:

### Option A — flash a community pre-built image

Search for **"CrowPanel 7 LVGL MicroPython"** on the Elecrow / LilyGO / lv_micropython
issue trackers and Discord. Several community members publish ready-to-flash
binaries with the panel + GT911 already wired into LVGL. If you use one of
those, you can leave [display.py](display.py) effectively empty.

### Option B — build it yourself

```bash
git clone --recurse-submodules https://github.com/lvgl/lv_micropython.git
cd lv_micropython
git submodule update --init --recursive
make -C mpy-cross
cd ports/esp32
# Use a board variant with PSRAM and an RGB panel example, e.g. ESP32_GENERIC_S3.
make BOARD=ESP32_GENERIC_S3 BOARD_VARIANT=SPIRAM_OCT submodules
make BOARD=ESP32_GENERIC_S3 BOARD_VARIANT=SPIRAM_OCT
```

Then in [display.py](display.py) wire LVGL to:

- the **RGB parallel panel** at the CrowPanel pin map (HSYNC, VSYNC, DE, PCLK
  + 16 RGB data pins — see CrowPanel 7" schematic),
- the **GT911** capacitive touch on I2C (address `0x5D`, INT + RST pins per schematic),
- a periodic timer driving `lv.tick_inc()` and `lv.task_handler()`.

There's a worked example in `lv_micropython/examples/` for the ILI9488 panel
that's a good template — copy it and swap in RGB-panel init.

## Flash & deploy

```bash
# one-off: flash the firmware you built (or downloaded)
esptool.py --chip esp32s3 --port /dev/cu.usbmodem* erase_flash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem* write_flash 0 firmware.bin

# copy the UI code
pip install mpremote
cp config.py.example config.py   # then edit WiFi + SERVER_URL
mpremote cp boot.py main.py display.py ui.py api.py config.py :/
mpremote reset
```

## Memory

The thumbnails coming back from the server are 200x200 JPEGs (~10–15 KB).
LVGL needs an SJPG decoder enabled in your build to render them directly,
otherwise change [ui.py](ui.py) to fetch pre-decoded RGB565 from the server
(easy follow-up: add `/thumb565/{cardId}` on the server and have it return
raw 200×200×2 = 80 KB blobs).

## What it looks like

- Top bar: All / Favourites / Folders + volume −/+
- Middle: 5×3 grid of card tiles (200×200 cover + title)
- Bottom: page nav + now-playing label + pause/resume

Tap a tile → server tells the player to start that card. That's it.
