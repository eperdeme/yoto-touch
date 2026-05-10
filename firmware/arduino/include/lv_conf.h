// Modern LVGL v9 config for yoto-touch on the CrowPanel 7".
#pragma once

/* COLOR SETTINGS */
#define LV_COLOR_DEPTH 16

/* MEMORY SETTINGS */
// In v9, memory configuration has changed. We'll use the new standard keys.
#define LV_USE_BUILTIN_MALLOC 0
#define LV_USE_BUILTIN_FREE   0
#define LV_USE_BUILTIN_REALLOC 0

#if LV_USE_BUILTIN_MALLOC == 0
    #define LV_MALLOC(size)      ps_malloc(size)
    #define LV_FREE(ptr)         free(ptr)
    #define LV_REALLOC(ptr, size) ps_realloc(ptr, size)
#endif

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
