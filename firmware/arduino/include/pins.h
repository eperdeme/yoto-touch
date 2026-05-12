// Pin map for Elecrow CrowPanel 7.0" HMI ESP32-S3-N4R8 (800x480 RGB + GT911).
// Source: Elecrow CrowPanel-ESP32-HMI-7.0-inch tutorial repo.
#pragma once

#include <Arduino.h>

// ----- RGB panel (16-bit, 5R/6G/5B) -----
// Values from Elecrow's CrowPanel_ESP32_LVGL_Demo gfx_conf.h (7.0" v3.0 board).
#define PIN_DE       41
#define PIN_VSYNC    40
#define PIN_HSYNC    39
#define PIN_PCLK      0

// Red bits (R3..R7 -> 5 lines)
#define PIN_R0       14
#define PIN_R1       21
#define PIN_R2       47
#define PIN_R3       48
#define PIN_R4       45
// Green bits (G2..G7 -> 6 lines)
#define PIN_G0        9
#define PIN_G1       46
#define PIN_G2        3
#define PIN_G3        8
#define PIN_G4       16
#define PIN_G5        1
// Blue bits (B3..B7 -> 5 lines)
#define PIN_B0       15
#define PIN_B1        7
#define PIN_B2        6
#define PIN_B3        5
#define PIN_B4        4

#define PIN_BL       2     // backlight enable

// Panel timings — values from Elecrow CrowPanel_ESP32_LVGL_Demo gfx_conf.h.
#define LCD_W        800
#define LCD_H        480
#define HSYNC_POL    0
#define HSYNC_FP     40
#define HSYNC_PW     48
#define HSYNC_BP     40
#define VSYNC_POL    0
#define VSYNC_FP     1
#define VSYNC_PW     31
#define VSYNC_BP     13
#define PCLK_HZ      15000000
#define PCLK_ACTIVE_NEG 1

// ----- GT911 touch (I2C) -----
#define PIN_TOUCH_SDA  19
#define PIN_TOUCH_SCL  20
#define PIN_TOUCH_INT  -1
#define PIN_TOUCH_RST  -1
#define GT911_ADDR1    0x5D
#define GT911_ADDR2    0x14

// ----- SD card (SPI, shared bus on the Elecrow CrowPanel 7" HMI) -----
#define PIN_SD_CS     10
#define PIN_SD_MOSI   11
#define PIN_SD_SCK    12
#define PIN_SD_MISO   13
