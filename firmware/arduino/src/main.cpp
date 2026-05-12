// yoto-touch — CrowPanel 7" LVGL v9 UI for Sophie's card picker.
//
// V2: Modern UI with Card Covers (160x160 RGB565).
//   - Uses 4x2 grid for better visibility.
//   - Fetches raw images from server to avoid JPEG overhead.
//   - Preserves ESP32-S3 RGB "direct mode" performance.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <unordered_map>
#include <set>
#include <esp_timer.h>

#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <lvgl.h>

#include "pins.h"
#include "secrets.h"

// ---------- constants ----------
static const int THUMB_SIZE = 144;
static const int GRID_COLS = 4;
static const int GRID_ROWS = 2;
static const int PAGE_SIZE = GRID_COLS * GRID_ROWS;

// ---------- display ----------
static Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    PIN_DE, PIN_VSYNC, PIN_HSYNC, PIN_PCLK,
    PIN_R0, PIN_R1, PIN_R2, PIN_R3, PIN_R4,
    PIN_G0, PIN_G1, PIN_G2, PIN_G3, PIN_G4, PIN_G5,
    PIN_B0, PIN_B1, PIN_B2, PIN_B3, PIN_B4,
    HSYNC_POL, HSYNC_FP, HSYNC_PW, HSYNC_BP,
    VSYNC_POL, VSYNC_FP, VSYNC_PW, VSYNC_BP,
    PCLK_ACTIVE_NEG, PCLK_HZ,
    /*useBigEndian*/ false,
    /*de_idle_high*/ 0, /*pclk_idle_high*/ 0,
    /*bounce_buffer_size_px*/ LCD_W * 10);

static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(LCD_W, LCD_H, bus);

// ---------- touch ----------
static TAMC_GT911 ts(PIN_TOUCH_SDA, PIN_TOUCH_SCL, (uint8_t)PIN_TOUCH_INT, (uint8_t)PIN_TOUCH_RST, LCD_W, LCD_H);

// ---------- LVGL v9 plumbing ----------
// DIRECT render into the panel framebuffer. The RGB peripheral runs a bounce-
// buffer DMA from PSRAM (configured on the bus); we just have to writeback
// the dirty rect from CPU cache to PSRAM after LVGL renders, so DMA reads
// the latest pixels. This is the canonical fast path for ESP32-S3 RGB +
// Arduino_GFX + LVGL 9 — no extra copy, no tearing.
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    gfx->flush();             // Cache_WriteBack_Addr on the framebuffer area
    lv_display_flush_ready(disp);
}

// ---------- backlight + idle dim ----------
// PWM the backlight so we can dim it (less washed-out colour at lower
// duty, easier on the eyes at night) and fade to off when idle.
static const int BL_FREQ_HZ = 25000;   // above audible — avoids LED-driver coil whine
static const int BL_RES_BITS = 8;
static uint8_t BL_DUTY_ACTIVE = 180;         // ~70% — tunable via dev panel
static const uint8_t BL_DUTY_DIM    = 40;     // ~16% — night-light level
static const uint32_t IDLE_DIM_MS   = 60 * 1000;       // 1 min  -> dim
static const uint32_t IDLE_OFF_MS   = 5 * 60 * 1000;   // 5 min  -> off

static uint32_t lastInteractionMs = 0;
static uint8_t  currentBlDuty = BL_DUTY_ACTIVE;
static bool     screenOff = false;

static void blSetDuty(uint8_t d) {
    if (d == currentBlDuty) return;
    currentBlDuty = d;
    ledcWrite(PIN_BL, d);
}

static void wakeScreen() {
    lastInteractionMs = millis();
    if (screenOff || currentBlDuty != BL_DUTY_ACTIVE) {
        blSetDuty(BL_DUTY_ACTIVE);
        screenOff = false;
    }
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    ts.read();
    if (ts.isTouched && ts.touches > 0) {
        // Any touch wakes the screen but is consumed (not passed to LVGL) if
        // the display was off, so kids can't accidentally trigger a button
        // when waking it up from a dark idle screen.
        if (screenOff) {
            wakeScreen();
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        wakeScreen();
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = ts.points[0].x;
        data->point.y = ts.points[0].y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------- HTTP helpers ----------

// Filter / sort state — declared early so the net task and pageQuerySuffix can read them.
String currentAuthor = "";       // empty = all authors
String currentSeries = "";       // empty = no series filter
String currentSort = "title";    // title | title_desc | author

static String urlEncode(const String &s) {
    String out;
    out.reserve(s.length() * 3);
    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
        uint8_t c = (uint8_t)s[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static String pageQuerySuffix() {
    String s = String("&sort=") + currentSort;
    if (currentAuthor.length()) s += String("&author=") + urlEncode(currentAuthor);
    if (currentSeries.length()) s += String("&series=") + urlEncode(currentSeries);
    return s;
}

static HTTPClient netHttp;
static bool netHttpInit = false;

static String httpGet(const String &path) {
    if (!netHttpInit) {
        netHttp.setReuse(true);
        netHttp.setTimeout(10000);
        netHttpInit = true;
    }
    String url = String(SERVER_BASE) + path;
    netHttp.begin(url);
    int code = netHttp.GET();
    String body = (code > 0) ? netHttp.getString() : String("");
    if (code != 200) {
        Serial.printf("[http] GET %s -> %d\n", url.c_str(), code);
    }
    netHttp.end();
    return body;
}

static int httpPost(const String &path) {
    HTTPClient http;
    String url = String(SERVER_BASE) + path;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST("");
    Serial.printf("[http] POST %s -> %d\n", url.c_str(), code);
    http.end();
    return code;
}

// ---------- Background network task ----------
// All HTTP I/O runs here on core 0 so the LVGL UI loop on core 1 never blocks.
// Communication is via a FreeRTOS queue (requests) and atomic flags (results).

enum NetCmd : uint8_t {
    NET_POST,              // fire-and-forget POST
    NET_FETCH_MANIFEST,    // reload manifest with current filters
    NET_FETCH_PAGE,        // fetch /page565 covers for a page
    NET_FETCH_DETAIL,      // fetch /cards/{id} extras
    NET_FETCH_FILTERS,     // fetch /authors + /series for filter modal
    NET_FETCH_SERIES,      // fetch /series?expand=true for series browser
};

struct NetRequest {
    NetCmd cmd;
    int    intArg;         // page number for NET_FETCH_PAGE
    char   strArg[128];   // URL path for POST, cardId for detail
};

// Result slots — written by net task, consumed by UI loop.
static volatile bool netManifestReady = false;
static String        netManifestBody;
static SemaphoreHandle_t netManifestMtx;

static volatile bool netDetailReady = false;
static String        netDetailBody;
static String        netDetailId;
static SemaphoreHandle_t netDetailMtx;

static volatile bool netFiltersReady = false;
static String        netAuthorsBody;
static String        netSeriesBody;
static SemaphoreHandle_t netFiltersMtx;

static volatile bool netSeriesBrowseReady = false;
static String        netSeriesBrowseBody;
static SemaphoreHandle_t netSeriesBrowseMtx;

// Page fetch results — covers go directly into coverCache (PSRAM),
// then we set a flag so UI knows to refresh tiles.
static volatile bool netPageReady = false;
static volatile int  netPageNum = -1;

static QueueHandle_t netQueue = nullptr;
static TaskHandle_t  netTaskHandle = nullptr;
static SemaphoreHandle_t coverCacheMtx = nullptr;  // protects coverCache across tasks

// Forward declaration — defined after CachedCover/coverCache declarations.
static bool netFetchPageIntoCache(int page);

static void netTaskFn(void *) {
    NetRequest req;
    for (;;) {
        if (xQueueReceive(netQueue, &req, portMAX_DELAY) != pdTRUE) continue;
        if (WiFi.status() != WL_CONNECTED) continue;

        switch (req.cmd) {
        case NET_POST: {
            httpPost(String(req.strArg));
            break;
        }
        case NET_FETCH_MANIFEST: {
            String url = String("/cards?page=0&size=200&sort=") + currentSort;
            if (currentAuthor.length()) url += String("&author=") + urlEncode(currentAuthor);
            if (currentSeries.length()) url += String("&series=") + urlEncode(currentSeries);
            String body = httpGet(url);
            xSemaphoreTake(netManifestMtx, portMAX_DELAY);
            netManifestBody = body;
            netManifestReady = true;
            xSemaphoreGive(netManifestMtx);
            break;
        }
        case NET_FETCH_PAGE: {
            netFetchPageIntoCache(req.intArg);
            netPageNum = req.intArg;
            netPageReady = true;
            break;
        }
        case NET_FETCH_DETAIL: {
            String body = httpGet(String("/cards/") + req.strArg);
            xSemaphoreTake(netDetailMtx, portMAX_DELAY);
            netDetailBody = body;
            netDetailId = String(req.strArg);
            netDetailReady = true;
            xSemaphoreGive(netDetailMtx);
            break;
        }
        case NET_FETCH_FILTERS: {
            String a = httpGet("/authors");
            String s = httpGet("/series");
            xSemaphoreTake(netFiltersMtx, portMAX_DELAY);
            netAuthorsBody = a;
            netSeriesBody = s;
            netFiltersReady = true;
            xSemaphoreGive(netFiltersMtx);
            break;
        }
        case NET_FETCH_SERIES: {
            String body = httpGet("/series?expand=true");
            xSemaphoreTake(netSeriesBrowseMtx, portMAX_DELAY);
            netSeriesBrowseBody = body;
            netSeriesBrowseReady = true;
            xSemaphoreGive(netSeriesBrowseMtx);
            break;
        }
        }
    }
}

static void netInit() {
    netManifestMtx    = xSemaphoreCreateMutex();
    netDetailMtx      = xSemaphoreCreateMutex();
    netFiltersMtx     = xSemaphoreCreateMutex();
    netSeriesBrowseMtx = xSemaphoreCreateMutex();
    netQueue = xQueueCreate(8, sizeof(NetRequest));
    coverCacheMtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(netTaskFn, "net", 12288, nullptr, 2, &netTaskHandle, 0);
}

// Helpers to enqueue requests from UI thread (non-blocking).
static void netPost(const char *path) {
    NetRequest r; r.cmd = NET_POST; r.intArg = 0;
    strlcpy(r.strArg, path, sizeof(r.strArg));
    xQueueSend(netQueue, &r, 0);
}

static void netFetchManifest() {
    NetRequest r; r.cmd = NET_FETCH_MANIFEST; r.intArg = 0; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static void netFetchPage(int page) {
    NetRequest r; r.cmd = NET_FETCH_PAGE; r.intArg = page; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static void netFetchDetail(const char *cardId) {
    NetRequest r; r.cmd = NET_FETCH_DETAIL; r.intArg = 0;
    strlcpy(r.strArg, cardId, sizeof(r.strArg));
    xQueueSend(netQueue, &r, 0);
}

static void netFetchFilters() {
    NetRequest r; r.cmd = NET_FETCH_FILTERS; r.intArg = 0; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static void netFetchSeries() {
    NetRequest r; r.cmd = NET_FETCH_SERIES; r.intArg = 0; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static HTTPClient imgHttp;
static bool imgHttpInit = false;

static bool fetchImage(const String &cardId, uint8_t *dest) {
    if (!imgHttpInit) {
        imgHttp.setReuse(true);
        imgHttp.setTimeout(8000);
        imgHttpInit = true;
    }
    String url = String(SERVER_BASE) + "/thumb565/" + cardId;
    imgHttp.begin(url);
    int code = imgHttp.GET();
    if (code != 200) {
        Serial.printf("[img] %s -> %d\n", cardId.c_str(), code);
        imgHttp.end();
        return false;
    }
    int len = imgHttp.getSize();
    const int expected = THUMB_SIZE * THUMB_SIZE * 2;
    if (len != expected) {
        Serial.printf("[img] %s wrong len %d (expected %d)\n", cardId.c_str(), len, expected);
        imgHttp.end();
        return false;
    }
    WiFiClient *stream = imgHttp.getStreamPtr();
    size_t got = stream->readBytes(dest, expected);
    imgHttp.end();
    if ((int)got != expected) {
        Serial.printf("[img] %s short read %u\n", cardId.c_str(), (unsigned)got);
        return false;
    }
    return true;
}

// ---------- UI State ----------
#include "sd_cache.h"

struct CachedCover {
    uint8_t *data;
    lv_image_dsc_t dsc;
};

static std::vector<CardMeta> allCards;             // full manifest, fetched once at boot

struct StringHash {
    size_t operator()(const String &s) const {
        size_t h = 2166136261u;
        for (unsigned i = 0; i < s.length(); i++) {
            h ^= (uint8_t)s[i];
            h *= 16777619u;
        }
        return h;
    }
};
static std::unordered_map<String, CachedCover *, StringHash> coverCache;

static int currentPage = 0;
static int pendingPage = -1;
static bool loading = false;

// Visible-page state: one tile per slot.
static lv_obj_t *slotImg[PAGE_SIZE] = {nullptr};
static String slotId[PAGE_SIZE];

static lv_obj_t *grid = nullptr;
static lv_obj_t *pageLabel = nullptr;
static lv_obj_t *statusLabel = nullptr;
static lv_obj_t *filterBtnLabel = nullptr;
static lv_obj_t *prevBtn = nullptr;
static lv_obj_t *nextBtn = nullptr;
static lv_obj_t *nowPlayingBar = nullptr;
static lv_obj_t *nowPlayingLabel = nullptr;

static WebServer otaServer(80);

static const int THUMB_BYTES = THUMB_SIZE * THUMB_SIZE * 2;
static int maxPageCount() { return max(1, (int)((allCards.size() + PAGE_SIZE - 1) / PAGE_SIZE)); }

static CachedCover *getOrAllocCover(const String &id) {
    auto it = coverCache.find(id);
    if (it != coverCache.end()) return it->second;
    CachedCover *cv = new CachedCover();
    cv->data = (uint8_t *)ps_malloc(THUMB_BYTES);
    if (!cv->data) { delete cv; return nullptr; }
    memset(&cv->dsc, 0, sizeof(cv->dsc));
    cv->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cv->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    cv->dsc.header.w = THUMB_SIZE;
    cv->dsc.header.h = THUMB_SIZE;
    cv->dsc.header.stride = THUMB_SIZE * 2;
    cv->dsc.data_size = THUMB_BYTES;
    cv->dsc.data = cv->data;
    // header.flags defaulted to 0 by memset above
    coverCache[id] = cv;
    return cv;
}

// Try to populate the buffer for `id` from SD. Returns true on hit.
// Only inserts into coverCache if SD actually has the cover, so isCovered()
// stays honest (otherwise we'd skip the HTTP fallback and show empty buffers).
static bool tryLoadCoverFromSd(const String &id) {
    if (!sdcache::ready()) return false;
    if (!sdcache::hasCover(id)) return false;
    CachedCover *cv = getOrAllocCover(id);
    if (!cv || !cv->data) return false;
    if (!sdcache::loadCover(id, cv->data, THUMB_BYTES)) {
        // The file exists but was unreadable / wrong size. Drop the empty
        // allocation so isCovered() returns false and HTTP will refill it.
        coverCache.erase(id);
        free(cv->data);
        delete cv;
        return false;
    }
    return true;
}

static bool isCovered(const String &id) {
    auto it = coverCache.find(id);
    return it != coverCache.end() && it->second && it->second->data;
}

// Background page fetch — runs on net task, writes into coverCache directly.
// Must NOT touch LVGL objects. Returns true if all covers landed.
static bool netFetchPageIntoCache(int page) {
    int start = page * PAGE_SIZE;
    int count = min((int)allCards.size() - start, PAGE_SIZE);
    if (count <= 0) return true;

    CachedCover *targets[PAGE_SIZE] = {nullptr};
    xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
    for (int i = 0; i < count; i++) targets[i] = getOrAllocCover(allCards[start + i].id);
    xSemaphoreGive(coverCacheMtx);

    HTTPClient http;
    String url = String(SERVER_BASE) + "/page565?page=" + page + "&size=" + PAGE_SIZE + pageQuerySuffix();
    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[page565] page=%d HTTP %d\n", page, code);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (!targets[i] || !targets[i]->data) {
            int skipped = 0;
            while (skipped < THUMB_BYTES && http.connected()) {
                uint8_t junk[1024];
                int n = stream->readBytes(junk, min((int)sizeof(junk), THUMB_BYTES - skipped));
                if (n <= 0) break;
                skipped += n;
            }
            continue;
        }
        size_t got = stream->readBytes(targets[i]->data, THUMB_BYTES);
        if ((int)got != THUMB_BYTES) {
            Serial.printf("[page565] page=%d slot=%d short read %u\n", page, i, (unsigned)got);
            free(targets[i]->data);
            delete targets[i];
            xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
            coverCache.erase(allCards[start + i].id);
            xSemaphoreGive(coverCacheMtx);
            ok = false;
            break;
        }
        sdcache::saveCover(allCards[start + i].id, targets[i]->data, THUMB_BYTES);
    }
    http.end();
    return ok;
}

static void freeUserStr(lv_event_t *e) {
    char *s = (char *)lv_event_get_user_data(e);
    if (s) free(s);
}

// ---------- Card detail view ----------
// Fullscreen "book page" shown when a card is tapped. Designed for a 6-yr-old:
//   - per-card colour theme (hue derived from cardId hash)
//   - big cover with gentle bob animation + glow
//   - chips: series + book #, duration, track count
//   - description text from /cards/{id}
//   - HUGE pulsing green PLAY button
static lv_obj_t *cardDetail = nullptr;
static char *cardDetailId = nullptr;   // strdup'd, freed on dismiss
static lv_timer_t *detailFetchTimer = nullptr;
static lv_obj_t *detailDescLbl = nullptr;
static lv_obj_t *detailChipsRow = nullptr;
static lv_obj_t *detailAuthorLbl = nullptr;
static lv_obj_t *detailSeriesBtn = nullptr;
static char *detailSeriesName = nullptr;

static void reloadManifest();

static void dismissCardDetail() {
    if (detailFetchTimer) { lv_timer_delete(detailFetchTimer); detailFetchTimer = nullptr; }
    if (cardDetail) {
        lv_obj_delete_async(cardDetail);
        cardDetail = nullptr;
    }
    detailDescLbl = nullptr;
    detailChipsRow = nullptr;
    detailAuthorLbl = nullptr;
    detailSeriesBtn = nullptr;
    if (cardDetailId) { free(cardDetailId); cardDetailId = nullptr; }
    if (detailSeriesName) { free(detailSeriesName); detailSeriesName = nullptr; }
}

static void onDetailBack(lv_event_t *) {
    dismissCardDetail();
}

static void openSeriesModal(lv_event_t *);  // fwd

static void onDetailSeries(lv_event_t *) {
    // If this card belongs to a named series, jump straight to that filter.
    // Otherwise open the series browser modal so the user can pick one.
    if (detailSeriesName) {
        currentSeries = String(detailSeriesName);
        currentAuthor = "";
        dismissCardDetail();
        reloadManifest();
        return;
    }
    dismissCardDetail();
    openSeriesModal(nullptr);
}

static void onDetailPlay(lv_event_t *) {
    if (!cardDetailId) return;
    Serial.printf("[ui] play %s\n", cardDetailId);
    String id = cardDetailId;
    String title;
    for (auto &c : allCards) { if (c.id == id) { title = c.title; break; } }
    if (nowPlayingLabel) lv_label_set_text_fmt(nowPlayingLabel, LV_SYMBOL_PLAY "  %s", title.c_str());
    netPost((String("/play/") + id).c_str());
    dismissCardDetail();
}

// Cheap deterministic hue 0..359 from a card id string.
static uint16_t hueForId(const char *id) {
    uint32_t h = 2166136261u;
    for (const char *p = id; p && *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    return (uint16_t)(h % 360);
}

// Add a coloured "chip" with an icon + text to the chips row.
static void addChip(lv_obj_t *row, const char *icon, const char *text, uint32_t bg) {
    lv_obj_t *chip = lv_obj_create(row);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 18, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_hor(chip, 14, 0);
    lv_obj_set_style_pad_ver(chip, 4, 0);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip, 6, 0);

    lv_obj_t *l = lv_label_create(chip);
    String t = String(icon) + " " + String(text);
    lv_label_set_text(l, t.c_str());
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
}

// Kick a background fetch for card detail extras. Result processed in loop() polling.
static void fetchDetailExtras(lv_timer_t *t) {
    detailFetchTimer = nullptr;
    lv_timer_delete(t);
    if (!cardDetail || !cardDetailId) return;
    netFetchDetail(cardDetailId);
}

// Process card detail result from net task.
static void processDetailResult(const String &body) {
    if (!cardDetail) return;
    if (!body.length()) return;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;

    const char *author = doc["author"] | "";
    const char *series = doc["series"] | "";
    int seq = doc["sequence_number"] | -1;
    int duration = doc["duration"] | 0;          // seconds
    int trackCount = doc["track_count"] | 0;
    int chapterCount = doc["chapter_count"] | 0;
    const char *desc = doc["description"] | "";

    if (detailAuthorLbl && author && *author) {
        lv_label_set_text_fmt(detailAuthorLbl, LV_SYMBOL_EDIT "  by %s", author);
    }

    if (detailChipsRow) {
        lv_obj_clean(detailChipsRow);
        if (series && *series) {
            char buf[96];
            if (seq > 0) snprintf(buf, sizeof(buf), "%s  #%d", series, seq);
            else         snprintf(buf, sizeof(buf), "%s", series);
            addChip(detailChipsRow, LV_SYMBOL_LIST, buf, 0x6f42c1);
        }
        if (duration > 0) {
            char buf[32];
            int h = duration / 3600, m = (duration % 3600) / 60;
            if (h > 0) snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
            else       snprintf(buf, sizeof(buf), "%d min", m ? m : 1);
            addChip(detailChipsRow, LV_SYMBOL_AUDIO, buf, 0x1f6feb);
        }
        if (trackCount > 0 || chapterCount > 0) {
            (void)trackCount; (void)chapterCount;
        }
    }

    // Capture series name for the Series button.
    if (detailSeriesName) { free(detailSeriesName); detailSeriesName = nullptr; }
    if (series && *series) {
        detailSeriesName = strdup(series);
        if (detailSeriesBtn) lv_obj_remove_flag(detailSeriesBtn, LV_OBJ_FLAG_HIDDEN);
    }

    if (detailDescLbl && desc && *desc) {
        lv_label_set_text(detailDescLbl, desc);
    } else if (detailDescLbl) {
        lv_label_set_text(detailDescLbl, "Tap PLAY to begin the story.");
    }
}

static void openCardDetail(const char *id, const char *title) {
    if (cardDetail) return;
    cardDetailId = strdup(id ? id : "");

    // Per-card colour theme.
    uint16_t hue = hueForId(cardDetailId);
    lv_color_t accent = lv_color_hsv_to_rgb(hue, 70, 95);
    lv_color_t accentDark = lv_color_hsv_to_rgb(hue, 80, 30);
    lv_color_t bgTop = lv_color_hsv_to_rgb(hue, 35, 22);
    lv_color_t bgBot = lv_color_hsv_to_rgb((hue + 30) % 360, 50, 12);

    cardDetail = lv_obj_create(lv_screen_active());
    lv_obj_set_size(cardDetail, LCD_W, LCD_H);
    lv_obj_align(cardDetail, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cardDetail, bgTop, 0);
    lv_obj_set_style_bg_opa(cardDetail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cardDetail, 0, 0);
    lv_obj_set_style_radius(cardDetail, 0, 0);
    lv_obj_set_style_pad_all(cardDetail, 0, 0);
    lv_obj_remove_flag(cardDetail, LV_OBJ_FLAG_SCROLLABLE);

    // Back button — top-left.
    lv_obj_t *backBtn = lv_btn_create(cardDetail);
    lv_obj_set_size(backBtn, 110, 50);
    lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 18, 18);
    lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x1a2230), 0);
    lv_obj_set_style_bg_opa(backBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(backBtn, 25, 0);
    lv_obj_set_style_border_width(backBtn, 1, 0);
    lv_obj_set_style_border_color(backBtn, lv_color_hex(0x3a4450), 0);
    lv_obj_set_style_border_color(backBtn, lv_color_hex(0x58a6ff), LV_STATE_PRESSED);
    lv_obj_add_event_cb(backBtn, onDetailBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(backLbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(backLbl);

    // Cover — floating glow, gentle bob.
    lv_obj_t *coverWrap = lv_obj_create(cardDetail);
    lv_obj_set_size(coverWrap, THUMB_SIZE + 16, THUMB_SIZE + 16);
    lv_obj_align(coverWrap, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_bg_color(coverWrap, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_bg_opa(coverWrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(coverWrap, 3, 0);
    lv_obj_set_style_border_color(coverWrap, accent, 0);
    lv_obj_set_style_radius(coverWrap, 12, 0);
    lv_obj_set_style_pad_all(coverWrap, 8, 0);
    lv_obj_remove_flag(coverWrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(coverWrap);
    auto cv = coverCache.find(String(id));
    if (cv != coverCache.end() && cv->second && cv->second->data) {
        lv_image_set_src(img, &cv->second->dsc);
    }
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

    // Right column container.
    lv_obj_t *right = lv_obj_create(cardDetail);
    lv_obj_set_size(right, 410, 380);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    // Title.
    lv_obj_t *titleLbl = lv_label_create(right);
    lv_label_set_text(titleLbl, title ? title : "");
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(titleLbl, 410);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(titleLbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Author (filled in by fetch).
    detailAuthorLbl = lv_label_create(right);
    lv_label_set_text(detailAuthorLbl, LV_SYMBOL_EDIT "  ...");
    lv_obj_set_style_text_color(detailAuthorLbl, lv_color_hex(0xc9d1d9), 0);
    lv_obj_set_style_text_font(detailAuthorLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(detailAuthorLbl, LV_ALIGN_TOP_LEFT, 2, 64);

    // Chips row (filled in by fetch). 36 px tall, sits right under the author.
    detailChipsRow = lv_obj_create(right);
    lv_obj_set_size(detailChipsRow, 410, 36);
    lv_obj_align(detailChipsRow, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_obj_set_style_bg_opa(detailChipsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detailChipsRow, 0, 0);
    lv_obj_set_style_pad_all(detailChipsRow, 0, 0);
    lv_obj_set_style_pad_column(detailChipsRow, 8, 0);
    lv_obj_set_flex_flow(detailChipsRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(detailChipsRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(detailChipsRow, LV_OBJ_FLAG_SCROLLABLE);

    // Description container — fixed size, clips overflow so a long blurb
    // can never paint over the PLAY / Series buttons.
    lv_obj_t *descBox = lv_obj_create(right);
    lv_obj_set_size(descBox, 410, 160);
    lv_obj_align(descBox, LV_ALIGN_TOP_LEFT, 0, 136);
    lv_obj_set_style_bg_opa(descBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(descBox, 0, 0);
    lv_obj_set_style_pad_all(descBox, 0, 0);
    lv_obj_set_scrollbar_mode(descBox, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(descBox, LV_OBJ_FLAG_SCROLLABLE);

    detailDescLbl = lv_label_create(descBox);
    lv_label_set_text(detailDescLbl, "Loading...");
    lv_label_set_long_mode(detailDescLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detailDescLbl, 410);
    lv_obj_set_style_text_color(detailDescLbl, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_text_font(detailDescLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(detailDescLbl, 4, 0);
    lv_obj_align(detailDescLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // PLAY button — modestly sized, bottom-right of the right column.
    lv_obj_t *playBtn = lv_btn_create(right);
    lv_obj_set_size(playBtn, 180, 70);
    lv_obj_align(playBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(playBtn, lv_color_hex(0x2ea043), 0);
    lv_obj_set_style_bg_grad_color(playBtn, lv_color_hex(0x238636), 0);
    lv_obj_set_style_bg_grad_dir(playBtn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_color(playBtn, lv_color_hex(0x238636), LV_STATE_PRESSED);
    lv_obj_set_style_radius(playBtn, 35, 0);
    lv_obj_set_style_border_width(playBtn, 2, 0);
    lv_obj_set_style_border_color(playBtn, lv_color_hex(0x4a6a3a), 0);
    lv_obj_set_style_border_color(playBtn, lv_color_hex(0x58a6ff), LV_STATE_PRESSED);
    lv_obj_add_event_cb(playBtn, onDetailPlay, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *playLbl = lv_label_create(playBtn);
    lv_label_set_text(playLbl, LV_SYMBOL_PLAY "  PLAY");
    lv_obj_set_style_text_font(playLbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(playLbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(playLbl);

    // Series button — left of PLAY. Hidden until fetchDetailExtras
    // confirms the card belongs to a series.
    detailSeriesBtn = lv_btn_create(right);
    lv_obj_set_size(detailSeriesBtn, 180, 70);
    lv_obj_align(detailSeriesBtn, LV_ALIGN_BOTTOM_RIGHT, -200, 0);
    lv_obj_set_style_bg_color(detailSeriesBtn, lv_color_hex(0x6f42c1), 0);
    lv_obj_set_style_bg_color(detailSeriesBtn, lv_color_hex(0x553098), LV_STATE_PRESSED);
    lv_obj_set_style_radius(detailSeriesBtn, 35, 0);
    lv_obj_set_style_border_width(detailSeriesBtn, 2, 0);
    lv_obj_set_style_border_color(detailSeriesBtn, lv_color_hex(0x4a3570), 0);
    lv_obj_set_style_border_color(detailSeriesBtn, lv_color_hex(0x58a6ff), LV_STATE_PRESSED);
    lv_obj_add_event_cb(detailSeriesBtn, onDetailSeries, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sLbl = lv_label_create(detailSeriesBtn);
    lv_label_set_text(sLbl, LV_SYMBOL_LIST "  Series");
    lv_obj_set_style_text_font(sLbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sLbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(sLbl);
    lv_obj_add_flag(detailSeriesBtn, LV_OBJ_FLAG_HIDDEN);

    // Kick the metadata fetch immediately (one HTTP req on the LVGL task).
    detailFetchTimer = lv_timer_create(fetchDetailExtras, 20, nullptr);
    lv_timer_set_repeat_count(detailFetchTimer, 1);

    (void)accentDark; // reserved for future use
}

static void onCardClick(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    const char *title = (const char *)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    openCardDetail(id, title);
}

// Evict cover-cache entries that are far from the current page to bound
// PSRAM usage.  Keep current page ± 1 page worth of cards.
static void evictDistantCovers() {
    int keepStart = max(0, (currentPage - 1) * PAGE_SIZE);
    int keepEnd   = min((int)allCards.size(), (currentPage + 2) * PAGE_SIZE);
    // Build a set of IDs to keep.
    std::set<String> keep;
    for (int i = keepStart; i < keepEnd; i++) keep.insert(allCards[i].id);
    // Walk the cache and free anything outside the keep set.
    for (auto it = coverCache.begin(); it != coverCache.end(); ) {
        if (keep.count(it->first) == 0) {
            if (it->second) { free(it->second->data); delete it->second; }
            it = coverCache.erase(it);
        } else {
            ++it;
        }
    }
}

static void renderGrid() {
    if (grid) lv_obj_clean(grid);
    evictDistantCovers();
    for (int i = 0; i < PAGE_SIZE; i++) slotImg[i] = nullptr;

    int cellW = LCD_W / GRID_COLS;
    int cellH = (LCD_H - 60 - 40 - 60) / GRID_ROWS;

    int start = currentPage * PAGE_SIZE;
    int end = min((int)allCards.size(), start + PAGE_SIZE);

    for (int i = 0; i < end - start; i++) {
        const CardMeta &cm = allCards[start + i];
        slotId[i] = cm.id;

        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        lv_obj_t *cont = lv_obj_create(grid);
        lv_obj_set_size(cont, cellW - 16, cellH - 8);
        lv_obj_set_pos(cont, col * cellW + 8, row * cellH + 4);
        lv_obj_set_style_bg_color(cont, lv_color_hex(0x1c2128), 0);
        lv_obj_set_style_border_width(cont, 1, 0);
        lv_obj_set_style_border_color(cont, lv_color_hex(0x2d333b), 0);
        lv_obj_set_style_radius(cont, 12, 0);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cont, 2, 0);
        // Tactile press feedback: brighter border (no transform_scale —
        // each scaled card needs a ~126 KB layer buffer and we have 8 visible).
        lv_obj_set_style_border_color(cont, lv_color_hex(0x58a6ff), LV_STATE_PRESSED);

        lv_obj_t *img = lv_image_create(cont);
        lv_obj_set_size(img, THUMB_SIZE, THUMB_SIZE);
        lv_obj_set_style_bg_color(img, lv_color_hex(0x2a313a), 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(img, 8, 0);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);
        auto cv = coverCache.find(cm.id);
        if (cv != coverCache.end() && cv->second && cv->second->data) {
            lv_image_set_src(img, &cv->second->dsc);
        }
        slotImg[i] = img;

        // Title overlay across the bottom of the cover: solid dark band so
        // white text reads cleanly on any cover artwork. Wraps to 2 lines
        // when the title is long, so series like "Amelia Fang and the ..."
        // remain distinguishable.
        lv_obj_t *l = lv_label_create(cont);
        lv_label_set_text(l, cm.title.c_str());
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, THUMB_SIZE);
        lv_obj_set_style_max_height(l, 40, 0);
        lv_obj_set_style_bg_color(l, lv_color_hex(0x0a0a0a), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_line_space(l, 1, 0);
        lv_obj_set_style_pad_hor(l, 4, 0);
        lv_obj_set_style_pad_ver(l, 2, 0);
        lv_obj_set_style_radius(l, 0, 0);
        // Align to the bottom of the image, not the container, so the band
        // sits cleanly on the cover.
        lv_obj_align_to(l, img, LV_ALIGN_BOTTOM_MID, 0, 0);

        char *idStr = strdup(cm.id.c_str());
        char *titleStr = strdup(cm.title.c_str());
        lv_obj_add_event_cb(cont, onCardClick, LV_EVENT_CLICKED, idStr);
        lv_obj_add_event_cb(cont, freeUserStr, LV_EVENT_DELETE, idStr);
        lv_obj_set_user_data(cont, titleStr);
        lv_obj_add_event_cb(cont, [](lv_event_t *e) {
            char *t = (char *)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
            if (t) free(t);
        }, LV_EVENT_DELETE, nullptr);
        lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_label_set_text_fmt(pageLabel, "Page %d / %d", currentPage + 1, maxPageCount());
}

static void loadPage(int page);
static void flashBtn(lv_obj_t *b);

// ---------- manifest persistence (NVS) ----------
// Stored as a compact length-prefixed binary blob keyed by current filter
// signature. Hand-rolled to avoid ArduinoJson allocations on a tight heap.
//
// Layout: [u8 sig_len][sig bytes][u16 card_count]
//   repeated card_count times:
//     [u8 id_len][id bytes][u8 title_len][title bytes]

static String manifestSignature() {
    return currentSort + "|" + currentAuthor + "|" + currentSeries;
}

static void saveManifestNvs() {
    if (allCards.empty()) return;
    String sig = manifestSignature();
    // Pre-compute size to allocate once.
    size_t total = 1 + sig.length() + 2;
    for (const auto &c : allCards) {
        if (c.id.length() > 255 || c.title.length() > 255) return;  // bail on weird data
        total += 1 + c.id.length() + 1 + c.title.length();
    }
    if (total > 12 * 1024) {
        Serial.printf("[nvs] manifest too big (%u bytes), skip\n", (unsigned)total);
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { Serial.println("[nvs] malloc fail"); return; }

    size_t o = 0;
    buf[o++] = (uint8_t)sig.length();
    memcpy(buf + o, sig.c_str(), sig.length()); o += sig.length();
    uint16_t n = (uint16_t)allCards.size();
    buf[o++] = (uint8_t)(n & 0xFF);
    buf[o++] = (uint8_t)(n >> 8);
    for (const auto &c : allCards) {
        buf[o++] = (uint8_t)c.id.length();
        memcpy(buf + o, c.id.c_str(), c.id.length()); o += c.id.length();
        buf[o++] = (uint8_t)c.title.length();
        memcpy(buf + o, c.title.c_str(), c.title.length()); o += c.title.length();
    }

    Preferences p;
    if (p.begin("yoto", false)) {
        size_t wrote = p.putBytes("manifest", buf, total);
        p.end();
        Serial.printf("[nvs] saved manifest %u bytes (%u cards)\n", (unsigned)wrote, n);
    } else {
        Serial.println("[nvs] begin(rw) failed");
    }
    free(buf);
}

static bool loadManifestNvs() {
    Preferences p;
    if (!p.begin("yoto", true)) return false;
    size_t len = p.getBytesLength("manifest");
    if (len < 4 || len > 16 * 1024) { p.end(); return false; }
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { p.end(); return false; }
    p.getBytes("manifest", buf, len);
    p.end();

    size_t o = 0;
    uint8_t slen = buf[o++];
    if (slen + 3 > len) { free(buf); return false; }
    String sig;
    sig.reserve(slen);
    for (uint8_t i = 0; i < slen; i++) sig += (char)buf[o++];
    if (sig != manifestSignature()) { free(buf); return false; }
    uint16_t n = buf[o] | (buf[o + 1] << 8); o += 2;

    allCards.clear();
    allCards.reserve(n);
    for (uint16_t i = 0; i < n && o < len; i++) {
        uint8_t idl = buf[o++]; if (o + idl > len) break;
        CardMeta m;
        m.id.reserve(idl);
        for (uint8_t k = 0; k < idl; k++) m.id += (char)buf[o++];
        if (o >= len) break;
        uint8_t tl = buf[o++]; if (o + tl > len) break;
        m.title.reserve(tl);
        for (uint8_t k = 0; k < tl; k++) m.title += (char)buf[o++];
        if (m.id.length()) allCards.push_back(m);
    }
    free(buf);
    Serial.printf("[nvs] loaded manifest %u cards\n", (unsigned)allCards.size());
    return !allCards.empty();
}

// Pull a fresh manifest from the server using current filter+sort and reset the grid to page 0.
// Now non-blocking: enqueues the request to the net task. Result is polled in loop().
static void reloadManifest() {
    if (filterBtnLabel) {
        String fb;
        if (currentSeries.length())      fb = String(LV_SYMBOL_LIST "  ") + currentSeries;
        else if (currentAuthor.length()) fb = String(LV_SYMBOL_EDIT "  ") + currentAuthor;
        else                              fb = String(LV_SYMBOL_LIST "  Series");
        lv_label_set_text(filterBtnLabel, fb.c_str());
    }
    lv_label_set_text(statusLabel, "Loading...");
    netFetchManifest();
}

// Process manifest result from net task (called from loop polling).
static void processManifestResult(const String &body) {
    allCards.clear();
    if (body.length()) {
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            JsonArray cards = doc["cards"].as<JsonArray>();
            for (JsonObject c : cards) {
                CardMeta m;
                m.id = String((const char *)(c["cardId"] | ""));
                m.title = String((const char *)(c["title"] | "?"));
                if (m.id.length()) allCards.push_back(m);
            }
        }
    }
    Serial.printf("[manifest] %u cards (author=%s, series=%s, sort=%s)\n",
                  (unsigned)allCards.size(),
                  currentAuthor.length() ? currentAuthor.c_str() : "*",
                  currentSeries.length() ? currentSeries.c_str() : "*",
                  currentSort.c_str());
    if (allCards.size()) sdcache::saveManifest(manifestSignature(), allCards);
    pendingPage = -1;
    loadPage(0);
}

static void loadPage(int page) {
    loading = true;
    currentPage = page;
    pendingPage = -1;

    // Render skeleton immediately (uses cache for covers already present).
    renderGrid();

    // SD pass first — fills the cache from disk for anything not already in PSRAM.
    int start = page * PAGE_SIZE;
    int count = min((int)allCards.size() - start, PAGE_SIZE);
    for (int i = 0; i < count; i++) {
        const String &id = allCards[start + i].id;
        if (!isCovered(id)) {
            if (tryLoadCoverFromSd(id) && slotImg[i] && slotId[i] == id) {
                lv_image_set_src(slotImg[i], &coverCache[id]->dsc);
                lv_obj_invalidate(slotImg[i]);
            }
        }
    }

    // Check which covers on this page are still missing.
    bool allCached = true;
    for (int i = 0; i < count; i++) {
        if (!isCovered(allCards[start + i].id)) { allCached = false; break; }
    }

    if (allCached) {
        lv_label_set_text(statusLabel, "Ready");
        loading = false;
    } else if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text(statusLabel, "Loading covers...");
        netFetchPage(page);
        // loading stays true until netPageReady is polled in loop()
    } else {
        lv_label_set_text(statusLabel, "Offline");
        loading = false;
    }
}

// Background: pick the next uncached page and warm it. Called from loop().
// Walks pages outward from currentPage (1 ahead, 1 behind, 2 ahead, 2 behind, ...).
static int nextPrefetchPage() {
    // Only prefetch pages within the eviction window (current ± 1),
    // otherwise evictDistantCovers() frees them and we loop forever.
    int total = maxPageCount();
    for (int d = 0; d <= 1; d++) {
        int a = currentPage + d;
        if (a >= 0 && a < total) {
            for (int i = a * PAGE_SIZE; i < min((int)allCards.size(), (a + 1) * PAGE_SIZE); i++) {
                if (!isCovered(allCards[i].id)) return a;
            }
        }
        if (d == 0) continue; // don't check currentPage - 0 twice
        int b = currentPage - d;
        if (b >= 0 && b < total) {
            for (int i = b * PAGE_SIZE; i < min((int)allCards.size(), (b + 1) * PAGE_SIZE); i++) {
                if (!isCovered(allCards[i].id)) return b;
            }
        }
    }
    return -1;
}

static void backgroundPrefetch() {
    if (loading || pendingPage >= 0) return;

    // Cheap pass: pull from SD first (no WiFi needed, ~1 ms per cover).
    // We do at most one cover per tick to avoid stalling LVGL.
    for (size_t i = 0; i < allCards.size(); i++) {
        const String &id = allCards[i].id;
        if (isCovered(id)) continue;
        if (!sdcache::hasCover(id)) continue;
        if (tryLoadCoverFromSd(id)) {
            // If it landed on the current page, refresh that tile.
            int p = i / PAGE_SIZE;
            int s = i % PAGE_SIZE;
            if (p == currentPage && slotImg[s] && slotId[s] == id) {
                lv_image_set_src(slotImg[s], &coverCache[id]->dsc);
                lv_obj_invalidate(slotImg[s]);
            }
            return;  // one per tick
        }
    }

    if (WiFi.status() != WL_CONNECTED) return;
    int p = nextPrefetchPage();
    if (p < 0) return;
    loading = true;
    netFetchPage(p);
    // loading cleared when netPageReady is consumed in loop()
}

static void onPrev(lv_event_t *) {
    flashBtn(prevBtn);
    int p = (pendingPage >= 0 ? pendingPage : currentPage);
    if (p > 0) {
        if (loading) lv_label_set_text(statusLabel, "Loading...");
        loadPage(p - 1);
    }
}
static void onNext(lv_event_t *) {
    flashBtn(nextBtn);
    int p = (pendingPage >= 0 ? pendingPage : currentPage);
    if (p + 1 < maxPageCount()) {
        if (loading) lv_label_set_text(statusLabel, "Loading...");
        loadPage(p + 1);
    }
}
static void onPause(lv_event_t *)  { netPost("/pause"); }
static void onResume(lv_event_t *) { netPost("/resume"); }

static void onHomeClick(lv_event_t *) {
    if (currentSeries.length() || currentAuthor.length()) {
        currentSeries = "";
        currentAuthor = "";
        reloadManifest();
    }
}

// ---------- Filter modal ----------
static lv_obj_t *filterModal = nullptr;

// Closing the modal must be deferred: we're inside an event dispatched
// against a child of the modal, and lv_obj_del would yank state out from
// under the dispatcher (assert in lv_label_set_text / use-after-free).
static void dismissModal() {
    if (filterModal) {
        lv_obj_delete_async(filterModal);
        filterModal = nullptr;
    }
}

static void closeFilterModal(lv_event_t *) {
    dismissModal();
}

static void onAuthorPick(lv_event_t *e) {
    const char *name = (const char *)lv_event_get_user_data(e);
    currentAuthor = (name && name[0]) ? String(name) : String("");
    currentSeries = "";
    dismissModal();
    reloadManifest();
}

static void onSeriesPick(lv_event_t *e) {
    const char *name = (const char *)lv_event_get_user_data(e);
    currentSeries = (name && name[0]) ? String(name) : String("");
    currentAuthor = "";
    dismissModal();
    reloadManifest();
}

static void freePickStr(lv_event_t *e) {
    char *s = (char *)lv_event_get_user_data(e);
    if (s) free(s);
}

static void openFilterModal(lv_event_t *) {
    if (filterModal) return;
    lv_label_set_text(statusLabel, "Loading filters...");
    netFetchFilters();
}

// Build filter modal from fetched data (called from loop polling).
static void buildFilterModal(const String &authorsBody, const String &seriesBody) {
    lv_label_set_text(statusLabel, "Ready");
    if (!authorsBody.length() && !seriesBody.length()) return;

    JsonDocument aDoc, sDoc;
    bool aOk = authorsBody.length() && deserializeJson(aDoc, authorsBody) == DeserializationError::Ok;
    bool sOk = seriesBody.length()  && deserializeJson(sDoc, seriesBody)  == DeserializationError::Ok;

    filterModal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(filterModal, LCD_W, LCD_H);
    lv_obj_align(filterModal, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(filterModal, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_bg_opa(filterModal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(filterModal, 0, 0);
    lv_obj_set_style_radius(filterModal, 0, 0);
    lv_obj_set_style_pad_all(filterModal, 0, 0);
    lv_obj_remove_flag(filterModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(filterModal);
    lv_obj_set_size(hdr, LCD_W, 56);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, "Filter library");
    lv_obj_set_style_text_color(t, lv_color_hex(0xc9d1d9), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *closeBtn = lv_button_create(hdr);
    lv_obj_set_size(closeBtn, 80, 40);
    lv_obj_align(closeBtn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x30363d), 0);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(closeBtn, closeFilterModal, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *list = lv_list_create(filterModal);
    lv_obj_set_size(list, LCD_W, LCD_H - 56);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);

    int total = (aOk ? (int)(aDoc["total"] | 0) : 0);
    char allLabel[48];
    snprintf(allLabel, sizeof(allLabel), "All cards  (%d)", total);
    lv_obj_t *allBtn = lv_list_add_button(list, NULL, allLabel);
    char *emptyStr = strdup("");
    lv_obj_add_event_cb(allBtn, onAuthorPick, LV_EVENT_CLICKED, emptyStr);
    lv_obj_add_event_cb(allBtn, freePickStr, LV_EVENT_DELETE, emptyStr);

    if (sOk) {
        JsonArray series = sDoc["series"].as<JsonArray>();
        if (series.size()) lv_list_add_text(list, "Series");
        for (JsonObject s : series) {
            const char *name = s["name"] | "?";
            int count = s["count"] | 0;
            char row[112];
            snprintf(row, sizeof(row), "%s  (%d)", name, count);
            lv_obj_t *b = lv_list_add_button(list, NULL, row);
            char *ns = strdup(name);
            lv_obj_add_event_cb(b, onSeriesPick, LV_EVENT_CLICKED, ns);
            lv_obj_add_event_cb(b, freePickStr, LV_EVENT_DELETE, ns);
        }
    }

    if (aOk) {
        JsonArray authors = aDoc["authors"].as<JsonArray>();
        if (authors.size()) lv_list_add_text(list, "Authors");
        for (JsonObject a : authors) {
            const char *name = a["name"] | "?";
            int count = a["count"] | 0;
            char row[96];
            snprintf(row, sizeof(row), "%s  (%d)", name, count);
            lv_obj_t *b = lv_list_add_button(list, NULL, row);
            char *ns = strdup(name);
            lv_obj_add_event_cb(b, onAuthorPick, LV_EVENT_CLICKED, ns);
            lv_obj_add_event_cb(b, freePickStr, LV_EVENT_DELETE, ns);
        }
    }
}

static void onSortCycle(lv_event_t *) {
    // Sort cycle removed; kept as no-op stub to avoid stale references.
}

// ---------- Series browser modal ----------
//
// Shows every auto-detected series as a section header followed by its card
// titles. Tap a card title to play it directly, or tap a series name to filter
// the main grid down to just that series.

static lv_obj_t *seriesModal = nullptr;

static void dismissSeriesModal() {
    if (seriesModal) {
        lv_obj_delete_async(seriesModal);
        seriesModal = nullptr;
    }
}

static void closeSeriesModal(lv_event_t *) {
    dismissSeriesModal();
}

static void onSeriesCardPick(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    if (!id || !id[0]) return;
    Serial.printf("[ui] play (series) %s\n", id);
    netPost((String("/play/") + id).c_str());
    dismissSeriesModal();
}

static void onSeriesHeaderPick(lv_event_t *e) {
    const char *name = (const char *)lv_event_get_user_data(e);
    currentSeries = (name && name[0]) ? String(name) : String("");
    currentAuthor = "";
    dismissSeriesModal();
    reloadManifest();
}

// ---------- Developer sheet ----------
// Hidden by design — opened via 5 quick taps on the status-bar area.
static lv_obj_t *devModal = nullptr;
static lv_obj_t *devLog = nullptr;
static lv_obj_t *devBlLbl = nullptr;
static uint32_t  devTapTimes[5] = {0};
static uint8_t   devTapIdx = 0;

static void dismissDevModal(lv_event_t *) {
    if (!devModal) return;
    lv_obj_del(devModal);
    devModal = nullptr;
    devLog = nullptr;
    devBlLbl = nullptr;
}

static void devLogf(const char *fmt, ...) {
    if (!devLog) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    String cur = lv_label_get_text(devLog);
    cur += buf;
    cur += "\n";
    lv_label_set_text(devLog, cur.c_str());
}

static void devWipeCache(lv_event_t *) {
    devLogf("wiping /yoto...");
    lv_timer_handler();
    bool ok = sdcache::purge();
    devLogf(ok ? "OK — rebooting in 1s" : "FAILED");
    lv_timer_handler();
    delay(1000);
    ESP.restart();
}

static void devShowInfo(lv_event_t *) {
    sdcache::Stats st;
    if (!sdcache::stats(st)) { devLogf("SD not ready"); return; }
    devLogf("card: %u MB  fat%u", (unsigned)st.cardSizeMB, st.fatType);
    devLogf("covers: %u files, %llu B",
            (unsigned)st.coverCount, (unsigned long long)st.coverBytes);
    devLogf("manifest: %s", st.manifestPresent ? "present" : "missing");
    devLogf("heap: %u  psram: %u",
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

// Brightness: persist to NVS so it survives reboot.
static void saveBrightness() {
    Preferences p;
    if (p.begin("yoto", false)) {
        p.putUChar("bl_duty", BL_DUTY_ACTIVE);
        p.end();
    }
}

static void loadBrightness() {
    Preferences p;
    if (p.begin("yoto", true)) {
        BL_DUTY_ACTIVE = p.getUChar("bl_duty", 180);
        p.end();
    }
}

static void updateBlLabel() {
    if (devBlLbl) lv_label_set_text_fmt(devBlLbl, "%d", (int)BL_DUTY_ACTIVE);
}

static void onBlDown(lv_event_t *) {
    if (BL_DUTY_ACTIVE > 20) BL_DUTY_ACTIVE -= 10;
    blSetDuty(BL_DUTY_ACTIVE);
    saveBrightness();
    updateBlLabel();
}

static void onBlUp(lv_event_t *) {
    if (BL_DUTY_ACTIVE < 250) BL_DUTY_ACTIVE += 10;
    blSetDuty(BL_DUTY_ACTIVE);
    saveBrightness();
    updateBlLabel();
}

static void openDevModal() {
    if (devModal) return;
    devModal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(devModal, 620, 400);
    lv_obj_center(devModal);
    lv_obj_set_style_bg_color(devModal, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_border_color(devModal, lv_color_hex(0xf85149), 0);
    lv_obj_set_style_border_width(devModal, 2, 0);
    lv_obj_set_style_radius(devModal, 12, 0);
    lv_obj_set_style_pad_all(devModal, 16, 0);
    lv_obj_remove_flag(devModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(devModal);
    lv_label_set_text(t, "Developer");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xf85149), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    // --- Left column: action buttons ---
    auto mkBtn = [&](const char *txt, int y, lv_event_cb_t cb, uint32_t color) {
        lv_obj_t *b = lv_button_create(devModal);
        lv_obj_set_size(b, 220, 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    };
    mkBtn("Wipe SD cache & reboot", 44, devWipeCache, 0xda3633);
    mkBtn("Show SD info",           94, devShowInfo, 0x1f6feb);
    mkBtn("Close",                 144, dismissDevModal, 0x30363d);

    // --- Brightness controls ---
    lv_obj_t *blTitle = lv_label_create(devModal);
    lv_label_set_text(blTitle, "Brightness");
    lv_obj_set_style_text_color(blTitle, lv_color_hex(0xc9d1d9), 0);
    lv_obj_set_style_text_font(blTitle, &lv_font_montserrat_16, 0);
    lv_obj_align(blTitle, LV_ALIGN_TOP_LEFT, 0, 210);

    lv_obj_t *blDown = lv_button_create(devModal);
    lv_obj_set_size(blDown, 60, 44);
    lv_obj_set_style_bg_color(blDown, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(blDown, 8, 0);
    lv_obj_align(blDown, LV_ALIGN_TOP_LEFT, 110, 200);
    lv_obj_t *bdl = lv_label_create(blDown);
    lv_label_set_text(bdl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(bdl, &lv_font_montserrat_20, 0);
    lv_obj_center(bdl);
    lv_obj_add_event_cb(blDown, onBlDown, LV_EVENT_CLICKED, nullptr);

    devBlLbl = lv_label_create(devModal);
    lv_label_set_text_fmt(devBlLbl, "%d", (int)BL_DUTY_ACTIVE);
    lv_obj_set_style_text_color(devBlLbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(devBlLbl, &lv_font_montserrat_24, 0);
    lv_obj_set_width(devBlLbl, 60);
    lv_obj_set_style_text_align(devBlLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(devBlLbl, LV_ALIGN_TOP_LEFT, 180, 207);

    lv_obj_t *blUp = lv_button_create(devModal);
    lv_obj_set_size(blUp, 60, 44);
    lv_obj_set_style_bg_color(blUp, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(blUp, 8, 0);
    lv_obj_align(blUp, LV_ALIGN_TOP_LEFT, 250, 200);
    lv_obj_t *bul = lv_label_create(blUp);
    lv_label_set_text(bul, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(bul, &lv_font_montserrat_20, 0);
    lv_obj_center(bul);
    lv_obj_add_event_cb(blUp, onBlUp, LV_EVENT_CLICKED, nullptr);

    // Hint: 0-255 range
    lv_obj_t *blHint = lv_label_create(devModal);
    lv_label_set_text(blHint, "0=off  255=max  (saved)");
    lv_obj_set_style_text_color(blHint, lv_color_hex(0x484f58), 0);
    lv_obj_set_style_text_font(blHint, &lv_font_montserrat_12, 0);
    lv_obj_align(blHint, LV_ALIGN_TOP_LEFT, 0, 250);

    // --- Right column: log ---
    devLog = lv_label_create(devModal);
    lv_obj_set_width(devLog, 260);
    lv_obj_align(devLog, LV_ALIGN_TOP_RIGHT, 0, 44);
    lv_obj_set_style_text_font(devLog, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(devLog, lv_color_hex(0x8b949e), 0);
    lv_label_set_text(devLog, "");
    devShowInfo(nullptr);
}

static void devTapZoneClicked(lv_event_t *) {
    uint32_t now = millis();
    // Rolling window: shift, then check oldest within 2s.
    for (int i = 0; i < 4; i++) devTapTimes[i] = devTapTimes[i + 1];
    devTapTimes[4] = now;
    if (devTapTimes[0] && (now - devTapTimes[0] < 2000)) {
        memset(devTapTimes, 0, sizeof(devTapTimes));
        openDevModal();
    }
}

// ---------- Now-playing status modal ----------
static lv_obj_t *npModal = nullptr;
static lv_obj_t *npVolLbl = nullptr;
static lv_obj_t *npPlayPauseBtn = nullptr;
static lv_obj_t *npPlayPauseLbl = nullptr;
static bool npIsPlaying = false;
static int npVolume = 50;

static void dismissNpModal() {
    if (npModal) { lv_obj_delete_async(npModal); npModal = nullptr; }
    npVolLbl = nullptr;
    npPlayPauseBtn = nullptr;
    npPlayPauseLbl = nullptr;
}

static void onNpClose(lv_event_t *) { dismissNpModal(); }

static void onVolDown(lv_event_t *) {
    npVolume = max(0, npVolume - 10);
    netPost((String("/volume/") + npVolume).c_str());
    if (npVolLbl) lv_label_set_text_fmt(npVolLbl, "%d%%", npVolume);
}

static void onVolUp(lv_event_t *) {
    npVolume = min(100, npVolume + 10);
    netPost((String("/volume/") + npVolume).c_str());
    if (npVolLbl) lv_label_set_text_fmt(npVolLbl, "%d%%", npVolume);
}

static void updatePlayPauseBtn() {
    if (!npPlayPauseBtn || !npPlayPauseLbl) return;
    if (npIsPlaying) {
        lv_label_set_text(npPlayPauseLbl, LV_SYMBOL_PAUSE "  Pause");
        lv_obj_set_style_bg_color(npPlayPauseBtn, lv_color_hex(0x30363d), 0);
    } else {
        lv_label_set_text(npPlayPauseLbl, LV_SYMBOL_PLAY "  Resume");
        lv_obj_set_style_bg_color(npPlayPauseBtn, lv_color_hex(0x238636), 0);
    }
}

static void onNpPlayPause(lv_event_t *) {
    if (npIsPlaying) {
        netPost("/pause");
        npIsPlaying = false;
    } else {
        netPost("/resume");
        npIsPlaying = true;
    }
    updatePlayPauseBtn();
}

static void onNpStop(lv_event_t *) {
    netPost("/stop");
    if (nowPlayingLabel) lv_label_set_text(nowPlayingLabel, LV_SYMBOL_AUDIO "  Tap for player status");
    dismissNpModal();
}

static void openNpModal() {
    if (npModal) return;

    npModal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(npModal, 560, 280);
    lv_obj_center(npModal);
    lv_obj_set_style_bg_color(npModal, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_border_color(npModal, lv_color_hex(0x238636), 0);
    lv_obj_set_style_border_width(npModal, 2, 0);
    lv_obj_set_style_radius(npModal, 16, 0);
    lv_obj_set_style_pad_all(npModal, 20, 0);
    lv_obj_remove_flag(npModal, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *hdr = lv_label_create(npModal);
    lv_label_set_text(hdr, LV_SYMBOL_AUDIO "  Player Controls");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x58a6ff), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *closeBtn = lv_button_create(npModal);
    lv_obj_set_size(closeBtn, 80, 40);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(closeBtn, 8, 0);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(closeBtn, onNpClose, LV_EVENT_CLICKED, nullptr);

    // Volume: label [ – ] 50% [ + ]
    lv_obj_t *vLbl = lv_label_create(npModal);
    lv_label_set_text(vLbl, "Volume");
    lv_obj_set_style_text_color(vLbl, lv_color_hex(0xc9d1d9), 0);
    lv_obj_set_style_text_font(vLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(vLbl, LV_ALIGN_TOP_LEFT, 0, 60);

    lv_obj_t *minBtn = lv_button_create(npModal);
    lv_obj_set_size(minBtn, 60, 50);
    lv_obj_align(minBtn, LV_ALIGN_TOP_LEFT, 100, 50);
    lv_obj_set_style_bg_color(minBtn, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(minBtn, 10, 0);
    lv_obj_t *ml = lv_label_create(minBtn);
    lv_label_set_text(ml, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_20, 0);
    lv_obj_center(ml);
    lv_obj_add_event_cb(minBtn, onVolDown, LV_EVENT_CLICKED, nullptr);

    npVolLbl = lv_label_create(npModal);
    lv_label_set_text_fmt(npVolLbl, "%d%%", npVolume);
    lv_obj_set_style_text_color(npVolLbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(npVolLbl, &lv_font_montserrat_24, 0);
    lv_obj_set_width(npVolLbl, 80);
    lv_obj_set_style_text_align(npVolLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(npVolLbl, LV_ALIGN_TOP_LEFT, 180, 57);

    lv_obj_t *plsBtn = lv_button_create(npModal);
    lv_obj_set_size(plsBtn, 60, 50);
    lv_obj_align(plsBtn, LV_ALIGN_TOP_LEFT, 280, 50);
    lv_obj_set_style_bg_color(plsBtn, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(plsBtn, 10, 0);
    lv_obj_t *pl = lv_label_create(plsBtn);
    lv_label_set_text(pl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, 0);
    lv_obj_center(pl);
    lv_obj_add_event_cb(plsBtn, onVolUp, LV_EVENT_CLICKED, nullptr);

    // Playback controls
    npPlayPauseBtn = lv_button_create(npModal);
    lv_obj_set_size(npPlayPauseBtn, 220, 56);
    lv_obj_set_style_radius(npPlayPauseBtn, 12, 0);
    lv_obj_align(npPlayPauseBtn, LV_ALIGN_TOP_LEFT, 0, 130);
    npPlayPauseLbl = lv_label_create(npPlayPauseBtn);
    lv_obj_set_style_text_font(npPlayPauseLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(npPlayPauseLbl);
    lv_obj_add_event_cb(npPlayPauseBtn, onNpPlayPause, LV_EVENT_CLICKED, nullptr);
    updatePlayPauseBtn();

    lv_obj_t *stopBtn = lv_button_create(npModal);
    lv_obj_set_size(stopBtn, 220, 56);
    lv_obj_set_style_bg_color(stopBtn, lv_color_hex(0xda3633), 0);
    lv_obj_set_style_radius(stopBtn, 12, 0);
    lv_obj_align(stopBtn, LV_ALIGN_TOP_LEFT, 240, 130);
    lv_obj_t *sl = lv_label_create(stopBtn);
    lv_label_set_text(sl, LV_SYMBOL_STOP "  Stop");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(stopBtn, onNpStop, LV_EVENT_CLICKED, nullptr);
}

static void onNowPlayingTap(lv_event_t *) {
    openNpModal();
}

static void openSeriesModal(lv_event_t *) {
    if (seriesModal) return;
    lv_label_set_text(statusLabel, "Loading series...");
    netFetchSeries();
}

// Build series modal from fetched data (called from loop polling).
static void buildSeriesModal(const String &body) {
    lv_label_set_text(statusLabel, "Ready");
    if (!body.length()) return;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;

    seriesModal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(seriesModal, LCD_W, LCD_H);
    lv_obj_align(seriesModal, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(seriesModal, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_bg_opa(seriesModal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(seriesModal, 0, 0);
    lv_obj_set_style_radius(seriesModal, 0, 0);
    lv_obj_set_style_pad_all(seriesModal, 0, 0);
    lv_obj_remove_flag(seriesModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(seriesModal);
    lv_obj_set_size(hdr, LCD_W, 56);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, "Browse by series");
    lv_obj_set_style_text_color(t, lv_color_hex(0xc9d1d9), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *closeBtn = lv_button_create(hdr);
    lv_obj_set_size(closeBtn, 80, 40);
    lv_obj_align(closeBtn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x30363d), 0);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(closeBtn, closeSeriesModal, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *list = lv_list_create(seriesModal);
    lv_obj_set_size(list, LCD_W, LCD_H - 56);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);

    JsonArray series = doc["series"].as<JsonArray>();
    if (!series.size()) {
        lv_list_add_text(list, "No series detected");
        return;
    }
    for (JsonObject s : series) {
        const char *name = s["name"] | "?";
        int count = s["count"] | 0;
        char hdrTxt[112];
        snprintf(hdrTxt, sizeof(hdrTxt), "%s  (%d)", name, count);
        lv_obj_t *h = lv_list_add_button(list, NULL, hdrTxt);
        lv_obj_set_style_bg_color(h, lv_color_hex(0x21262d), 0);
        lv_obj_set_style_text_color(h, lv_color_hex(0x58a6ff), 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
        char *ns = strdup(name);
        lv_obj_add_event_cb(h, onSeriesHeaderPick, LV_EVENT_CLICKED, ns);
        lv_obj_add_event_cb(h, freePickStr, LV_EVENT_DELETE, ns);

        JsonArray cardsArr = s["cards"].as<JsonArray>();
        for (JsonObject c : cardsArr) {
            const char *cid = c["cardId"] | "";
            const char *title = c["title"] | "?";
            char row[128];
            snprintf(row, sizeof(row), "  %s", title);
            lv_obj_t *b = lv_list_add_button(list, LV_SYMBOL_PLAY, row);
            char *cidStr = strdup(cid);
            lv_obj_add_event_cb(b, onSeriesCardPick, LV_EVENT_CLICKED, cidStr);
            lv_obj_add_event_cb(b, freePickStr, LV_EVENT_DELETE, cidStr);
        }
    }
}

// ---------- Prev/Next tap feedback ----------
static void resetBtnColor(lv_timer_t *t) {
    lv_obj_t *b = (lv_obj_t *)lv_timer_get_user_data(t);
    if (b) lv_obj_set_style_bg_color(b, lv_color_hex(0x21262d), 0);
    lv_timer_delete(t);
}

static void flashBtn(lv_obj_t *b) {
    if (!b) return;
    lv_obj_set_style_bg_color(b, lv_color_hex(0x58a6ff), 0);
    lv_timer_t *t = lv_timer_create(resetBtnColor, 180, b);
    lv_timer_set_repeat_count(t, 1);
}

static void buildScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // Top bar
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, LCD_W, 60);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "Sophie's Yoto");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58a6ff), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(title, lv_color_hex(0x2ea043), LV_STATE_PRESSED);
    lv_obj_add_event_cb(title, onHomeClick, LV_EVENT_CLICKED, nullptr);

    // Series browser button — single primary nav action.
    lv_obj_t *sb = lv_button_create(top);
    lv_obj_set_size(sb, 160, 44);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(sb, 8, 0);
    lv_obj_align(sb, LV_ALIGN_CENTER, 0, 0);
    filterBtnLabel = lv_label_create(sb);  // re-used as the current-filter readout
    lv_label_set_text(filterBtnLabel, LV_SYMBOL_LIST "  Series");
    lv_label_set_long_mode(filterBtnLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(filterBtnLabel, 140);
    lv_obj_set_style_text_align(filterBtnLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(filterBtnLabel);
    lv_obj_add_event_cb(sb, openSeriesModal, LV_EVENT_CLICKED, nullptr);

    statusLabel = lv_label_create(top);
    lv_label_set_text(statusLabel, "Connecting...");
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x8b949e), 0);
    lv_obj_align(statusLabel, LV_ALIGN_RIGHT_MID, -20, 0);

    // Invisible dev tap-zone over the right end of the title bar (5 fast taps -> dev sheet).
    lv_obj_t *devZone = lv_obj_create(top);
    lv_obj_set_size(devZone, 120, 56);
    lv_obj_align(devZone, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(devZone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(devZone, 0, 0);
    lv_obj_set_style_pad_all(devZone, 0, 0);
    lv_obj_remove_flag(devZone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(devZone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(devZone, devTapZoneClicked, LV_EVENT_CLICKED, nullptr);

    // Bottom bar
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_set_size(bot, LCD_W, 60);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_radius(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_remove_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);

    auto mkBtn = [&](const char *txt, int x, lv_event_cb_t cb, uint32_t color = 0x21262d) {
        lv_obj_t *b = lv_button_create(bot);
        lv_obj_set_size(b, 100, 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, x, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        return b;
    };
    
    prevBtn = mkBtn("< Prev", 20, onPrev);
    nextBtn = mkBtn("Next >", 140, onNext);
    pageLabel = lv_label_create(bot);
    lv_label_set_text(pageLabel, "Page 1 / 1");
    lv_obj_set_style_text_color(pageLabel, lv_color_hex(0xc9d1d9), 0);
    lv_obj_align(pageLabel, LV_ALIGN_CENTER, 0, 0);

    // Now Playing Bar sits in a reserved 40px strip ABOVE the bottom bar so
    // it never overlaps the card grid. Hidden until the user taps a tile.
    nowPlayingBar = lv_obj_create(scr);
    lv_obj_set_size(nowPlayingBar, LCD_W, 40);
    lv_obj_align(nowPlayingBar, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_bg_color(nowPlayingBar, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(nowPlayingBar, 0, 0);
    lv_obj_set_style_radius(nowPlayingBar, 0, 0);
    lv_obj_set_style_pad_all(nowPlayingBar, 0, 0);
    lv_obj_remove_flag(nowPlayingBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nowPlayingBar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(nowPlayingBar, onNowPlayingTap, LV_EVENT_CLICKED, nullptr);

    nowPlayingLabel = lv_label_create(nowPlayingBar);
    lv_label_set_text(nowPlayingLabel, LV_SYMBOL_AUDIO "  Tap for player status");
    lv_label_set_long_mode(nowPlayingLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nowPlayingLabel, LCD_W - 20);
    lv_obj_set_style_text_align(nowPlayingLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(nowPlayingLabel);

    // Grid: top bar (60) + grid + reserved now-playing strip (40) + bottom bar (60) = LCD_H.
    grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LCD_W, LCD_H - 60 - 40 - 60);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------- WiFi ----------
static void startWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[wifi] connecting to %s...\n", WIFI_SSID);
}

// Boot stages keep the UI loop running while we do (relatively) slow work.
enum BootStage : uint8_t {
    BOOT_SD,           // mount SD + show cached manifest immediately (offline)
    BOOT_WIFI,         // waiting for WiFi (UI already interactive)
    BOOT_FETCH,        // refresh manifest from server
    BOOT_DONE,
};
static BootStage bootStage = BOOT_SD;
static uint32_t bootStageEntered = 0;

static void enterStage(BootStage s) {
    bootStage = s;
    bootStageEntered = millis();
}

static void tickBoot() {
    switch (bootStage) {
    case BOOT_SD: {
        // Try the persisted manifest first so the user sees the library before
        // WiFi even comes up. If SD has it, this is "instant boot" territory.
        String sig;
        std::vector<CardMeta> cards;
        if (sdcache::loadManifest(sig, cards) && !cards.empty()) {
            allCards = std::move(cards);
            // Restore filter signature so /cards refresh matches and the label is correct.
            int p1 = sig.indexOf('|');
            int p2 = sig.indexOf('|', p1 + 1);
            if (p1 > 0 && p2 > p1) {
                currentSort   = sig.substring(0, p1);
                currentAuthor = sig.substring(p1 + 1, p2);
                currentSeries = sig.substring(p2 + 1);
            }
            if (filterBtnLabel) {
                String fb;
                if (currentSeries.length())      fb = String(LV_SYMBOL_LIST "  ") + currentSeries;
                else if (currentAuthor.length()) fb = String(LV_SYMBOL_EDIT "  ") + currentAuthor;
                else                              fb = String(LV_SYMBOL_LIST "  Series");
                lv_label_set_text(filterBtnLabel, fb.c_str());
            }
            loadPage(0);                           // pulls covers from SD too
            lv_label_set_text(statusLabel, "Offline cache");
        }
        startWifi();
        enterStage(BOOT_WIFI);
        break;
    }
    case BOOT_WIFI: {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
            // Start ElegantOTA web server — browse to http://<IP>/update
            ElegantOTA.begin(&otaServer);
            otaServer.begin();
            Serial.println("[ota] ElegantOTA ready at /update");
            lv_label_set_text(statusLabel, allCards.empty() ? "Loading..." : "Refreshing...");
            enterStage(BOOT_FETCH);
            return;
        }
        if (millis() - bootStageEntered > 30000) {
            lv_label_set_text(statusLabel, allCards.empty() ? "WiFi failed" : "Offline");
            enterStage(BOOT_DONE);
        }
        break;
    }
    case BOOT_FETCH: {
        reloadManifest();  // non-blocking: enqueues to net task
        enterStage(BOOT_DONE);
        break;
    }
    case BOOT_DONE:
        break;
    }
}

// ---------- setup / loop ----------
void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n=== yoto-touch boot (V2) ===");

    pinMode(PIN_BL, OUTPUT);
    ledcAttach(PIN_BL, BL_FREQ_HZ, BL_RES_BITS);
    ledcWrite(PIN_BL, BL_DUTY_ACTIVE);
    lastInteractionMs = millis();

    if (!gfx->begin()) Serial.println("[gfx] begin FAILED");
    gfx->fillScreen(0x0000);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    ts.begin();
    ts.setRotation(ROTATION_INVERTED);

    lv_init();
    void *fb = gfx->getFramebuffer();
    if (!fb) { Serial.println("[lvgl] panel framebuffer NULL"); while (1) delay(1000); }

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(disp, disp_flush);
    uint32_t fb_size_bytes = LCD_W * LCD_H * sizeof(lv_color_t);
    lv_display_set_buffers(disp, fb, nullptr, fb_size_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);

    buildScreen();
    lv_label_set_text(statusLabel, "Mounting SD...");
    lv_timer_handler();

    sdcache::begin();
    loadBrightness();
    blSetDuty(BL_DUTY_ACTIVE);

    lv_label_set_text(statusLabel, "Connecting...");
    lv_timer_handler();

    static const esp_timer_create_args_t targs = {
        .callback = [](void *) { lv_tick_inc(2); },
        .name = "lv_tick"
    };
    esp_timer_handle_t th;
    esp_timer_create(&targs, &th);
    esp_timer_start_periodic(th, 2000);

    enterStage(BOOT_SD);
    netInit();
    Serial.println("[boot] setup done; UI live");
}

void loop() {
    lv_timer_handler();
    delay(2);

    // Idle dim/off. Tick once per loop; cheap.
    if (!screenOff) {
        uint32_t idle = millis() - lastInteractionMs;
        if (idle > IDLE_OFF_MS) {
            blSetDuty(0);
            screenOff = true;
        } else if (idle > IDLE_DIM_MS) {
            blSetDuty(BL_DUTY_DIM);
        }
    }

    if (bootStage != BOOT_DONE) {
        tickBoot();
        return;  // skip polling until boot finishes
    }

    // --- Poll net task results (non-blocking) ---

    // Manifest result
    if (netManifestReady) {
        xSemaphoreTake(netManifestMtx, portMAX_DELAY);
        String body = std::move(netManifestBody);
        netManifestReady = false;
        xSemaphoreGive(netManifestMtx);
        processManifestResult(body);
    }

    // Page covers result — refresh visible tiles
    if (netPageReady) {
        int page = netPageNum;
        netPageReady = false;
        loading = false;
        if (page == currentPage) {
            // Refresh tiles that now have covers
            int start = page * PAGE_SIZE;
            int count = min((int)allCards.size() - start, PAGE_SIZE);
            for (int i = 0; i < count; i++) {
                const String &id = allCards[start + i].id;
                if (slotImg[i] && slotId[i] == id && isCovered(id)) {
                    lv_image_set_src(slotImg[i], &coverCache[id]->dsc);
                    lv_obj_invalidate(slotImg[i]);
                }
            }
            lv_label_set_text(statusLabel, "Ready");
        }
        // Handle queued page change
        if (pendingPage >= 0) {
            int next = pendingPage;
            pendingPage = -1;
            loadPage(next);
        }
    }

    // Card detail extras result
    if (netDetailReady) {
        xSemaphoreTake(netDetailMtx, portMAX_DELAY);
        String body = std::move(netDetailBody);
        String id = std::move(netDetailId);
        netDetailReady = false;
        xSemaphoreGive(netDetailMtx);
        // Only process if the detail view is still showing the same card
        if (cardDetail && cardDetailId && id == String(cardDetailId)) {
            processDetailResult(body);
        }
    }

    // Filter modal data ready
    if (netFiltersReady) {
        xSemaphoreTake(netFiltersMtx, portMAX_DELAY);
        String a = std::move(netAuthorsBody);
        String s = std::move(netSeriesBody);
        netFiltersReady = false;
        xSemaphoreGive(netFiltersMtx);
        if (!filterModal) buildFilterModal(a, s);
    }

    // Series browser data ready
    if (netSeriesBrowseReady) {
        xSemaphoreTake(netSeriesBrowseMtx, portMAX_DELAY);
        String body = std::move(netSeriesBrowseBody);
        netSeriesBrowseReady = false;
        xSemaphoreGive(netSeriesBrowseMtx);
        if (!seriesModal) buildSeriesModal(body);
    }

    // Background prefetch (SD only — net prefetch uses the queue)
    static uint32_t lastPrefetch = 0;
    if (!loading && millis() - lastPrefetch > 300) {
        lastPrefetch = millis();
        backgroundPrefetch();
    }

    otaServer.handleClient();
    ElegantOTA.loop();
}
