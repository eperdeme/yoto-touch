// Modern LVGL v9 config for yoto-touch on the CrowPanel 7".
#pragma once

/* COLOR SETTINGS */
#define LV_COLOR_DEPTH 16

/* MEMORY SETTINGS */
// All LVGL allocations go through ps_malloc into the 8MB octal PSRAM (see
// src/lv_mem_psram.cpp). Without this, even with LV_STDLIB_CLIB, small
// allocations stay in DRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL = 16384 by
// default), and a busy modal/list quickly OOMs the ~190KB DRAM heap.
// String/sprintf still use the standard C library — fast and small.
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* HAL SETTINGS */
#define LV_DEF_REFR_PERIOD  16      /* ms (~60 FPS) */
#define LV_DPI_DEF          130     /* Approximate for 7" 800x480 */

/* FEATURE USAGE */
#define LV_USE_ANIMATION     1
#define LV_USE_TIMER         1
#define LV_USE_SNAPSHOT      1

/* LOGGING */
#define LV_USE_LOG           1
#define LV_LOG_LEVEL         LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF        1

/* FONT USAGE */
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_DEFAULT        &lv_font_montserrat_16

/* WIDGET USAGE */
#define LV_USE_LABEL         1
#define LV_USE_BUTTON        1
#define LV_USE_BTNMATRIX     1
#define LV_USE_IMAGE         1
#define LV_USE_BAR           1
#define LV_USE_GRID          1
#define LV_USE_SPINNER       1

/* THEME USAGE */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
