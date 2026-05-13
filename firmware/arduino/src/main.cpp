// yoto-touch — CrowPanel 7" LVGL v9 UI for Sophie's card picker.
//
// V2: Modern UI with Card Covers (160x160 RGB565).
//   - Uses 4x2 grid for better visibility.
//   - Fetches Yoto CDN covers and decodes PNG thumbnails to RGB565.
//   - Preserves ESP32-S3 RGB "direct mode" performance.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <set>
#include <esp_timer.h>
#include <esp_cache.h>

#include <Arduino_GFX_Library.h>
#include <lvgl.h>

extern "C" void lv_image_cache_drop(const void *src);

#include "pins.h"
#include "secrets.h"
#include "yoto_api.h"
#include "sd_cache.h"

// ---------- constants ----------
static const int THUMB_SIZE = 144;
static const int DETAIL_COVER_SIZE = 260;
static const uint32_t DETAIL_COVER_TLS_MIN_INTERNAL = 90000;
static const int GRID_COLS = 4;
static const int GRID_ROWS = 2;
static const int PAGE_SIZE = GRID_COLS * GRID_ROWS;

SET_LOOP_TASK_STACK_SIZE(12288);

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
static const uint16_t GT911_PRODUCT_ID_REG = 0x8140;
static const uint16_t GT911_POINT_INFO_REG = 0x814E;
static const uint16_t GT911_POINT_1_REG = 0x814F;

static uint8_t touchAddr = GT911_ADDR1;
static bool touchReady = false;

struct TouchPoint {
    uint16_t x;
    uint16_t y;
};

static bool touchWriteReg(uint16_t reg, const uint8_t *data, size_t len) {
    Wire.beginTransmission(touchAddr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    for (size_t i = 0; i < len; i++) Wire.write(data[i]);
    return Wire.endTransmission() == 0;
}

static bool touchReadReg(uint16_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(touchAddr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    size_t got = Wire.requestFrom((int)touchAddr, (int)len);
    if (got != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static bool touchProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static void touchClearPointInfo() {
    uint8_t zero = 0;
    touchWriteReg(GT911_POINT_INFO_REG, &zero, 1);
}

static bool touchInit() {
    if (touchProbe(GT911_ADDR1)) {
        touchAddr = GT911_ADDR1;
    } else if (touchProbe(GT911_ADDR2)) {
        touchAddr = GT911_ADDR2;
    } else {
        Serial.println("[touch] GT911 not found");
        return false;
    }

    char product[5] = {0};
    if (touchReadReg(GT911_PRODUCT_ID_REG, (uint8_t *)product, 4)) {
        Serial.printf("[touch] GT911 addr=0x%02X product=%s\n", touchAddr, product);
    } else {
        Serial.printf("[touch] GT911 addr=0x%02X\n", touchAddr);
    }
    touchClearPointInfo();
    touchReady = true;
    return true;
}

static bool touchReadPoint(TouchPoint &pt) {
    if (!touchReady) return false;

    uint8_t info = 0;
    if (!touchReadReg(GT911_POINT_INFO_REG, &info, 1)) return false;

    bool bufferReady = (info & 0x80) != 0;
    uint8_t touches = info & 0x0F;
    if (!bufferReady || touches == 0) {
        if (bufferReady) touchClearPointInfo();
        return false;
    }

    uint8_t data[7] = {0};
    bool ok = touchReadReg(GT911_POINT_1_REG, data, sizeof(data));
    touchClearPointInfo();
    if (!ok) return false;

    pt.x = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    pt.y = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
    if (pt.x >= LCD_W) pt.x = LCD_W - 1;
    if (pt.y >= LCD_H) pt.y = LCD_H - 1;
    return true;
}

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
    TouchPoint pt = {};
    if (touchReadPoint(pt)) {
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
        data->point.x = pt.x;
        data->point.y = pt.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------- HTTP helpers ----------

// Filter / sort state — declared early so the net task and UI can read them.
String currentAuthor = "";       // empty = all authors
String currentSeries = "";       // empty = no series filter
String currentSort = "title";    // title | title_desc | author
String currentSource = "";      // empty | creative | purchased

// ---------- Background network task ----------
// All HTTP I/O runs here on core 0 so the LVGL UI loop on core 1 never blocks.
// Communication is via a FreeRTOS queue (requests) and atomic flags (results).

enum NetCmd : uint8_t {
    NET_PLAY,              // play a card by ID
    NET_PAUSE,
    NET_RESUME,
    NET_STOP,
    NET_VOLUME,            // intArg = volume 0-100
    NET_FETCH_MANIFEST,    // reload library from Yoto API
    NET_FETCH_PAGE,        // fetch covers for a page (download PNG + decode)
    NET_FETCH_DETAIL,      // fetch card detail
    NET_FETCH_DETAIL_COVER,// fetch one larger cover for the open detail card
    NET_FETCH_FILTERS,     // compute authors + series from libraryCards (local)
    NET_FETCH_SERIES,      // same as filters but expanded for series browser
    NET_FILL_COVER,        // slowly fill SD cover cache; intArg = libraryCards index
    NET_AUTH_POLL,          // poll device code flow
};

struct NetRequest {
    NetCmd cmd;
    int    intArg;         // page number for NET_FETCH_PAGE
    char   strArg[128];   // URL path for POST, cardId for detail
};

struct AuthorInfo { String name; int count; };
struct SeriesInfo { String name; int count; };
struct SeriesDetail { 
    String name; 
    int count; 
    struct Card { String id; String title; int sequenceNumber; };
    std::vector<Card> cards;
};

// Result slots — written by net task, consumed by UI loop.
static volatile bool netManifestReady = false;
static volatile bool netManifestOk = false;
static std::vector<CardMeta> netManifestCards;
static SemaphoreHandle_t netManifestMtx;

static volatile bool netDetailReady = false;
static YotoCardDetail netDetailResult;
static String        netDetailId;
static SemaphoreHandle_t netDetailMtx;

static volatile bool netDetailCoverReady = false;
static volatile bool netDetailCoverOk = false;
static String        netDetailCoverId;
static uint8_t      *netDetailCoverPixels = nullptr;
static SemaphoreHandle_t netDetailCoverMtx;

static volatile bool netFiltersReady = false;
static std::vector<AuthorInfo> netAuthors;
static std::vector<SeriesInfo> netSeries;
static int netTotalCards = 0;
static SemaphoreHandle_t netFiltersMtx;

static volatile bool netSeriesBrowseReady = false;
static std::vector<SeriesDetail> netSeriesBrowse;
static SemaphoreHandle_t netSeriesBrowseMtx;

static volatile bool netCacheFillReady = false;
static volatile int netCacheFillIndex = -1;
static volatile bool netCacheFillSaved = false;

// Auth result
static volatile bool netAuthReady = false;
static String        netAuthResult;     // "authorized", "pending", "expired", "error"
static SemaphoreHandle_t netAuthMtx;

// Page fetch results — covers go directly into coverCache (PSRAM),
// then we set a flag so UI knows to refresh tiles.
static volatile bool netPageReady = false;
static volatile int  netPageNum = -1;

static QueueHandle_t netQueue = nullptr;
static TaskHandle_t  netTaskHandle = nullptr;
static SemaphoreHandle_t coverCacheMtx = nullptr;  // protects coverCache across tasks

// Forward declaration — defined after CachedCover/coverCache declarations.
static bool netFetchPageIntoCache(int page);
static bool netFillOneCover(int index);
static void setNowPlaying(const String &id, const String &title);
static void playCardNow(const String &id);

// Forward-declared; defined in UI state section.
static std::vector<CardMeta> libraryCards;         // full synced library
static std::vector<uint16_t> visibleCardIdx;        // current filtered/sorted view into libraryCards

static const CardMeta &visibleCardAt(int pos) {
    return libraryCards[visibleCardIdx[pos]];
}

static bool cardComesBeforeInSeries(const CardMeta &a, const CardMeta &b) {
    const bool aNumbered = a.sequenceNumber > 0;
    const bool bNumbered = b.sequenceNumber > 0;
    if (aNumbered && bNumbered && a.sequenceNumber != b.sequenceNumber) return a.sequenceNumber < b.sequenceNumber;
    if (aNumbered != bNumbered) return aNumbered;
    if (a.title != b.title) return a.title < b.title;
    return a.id < b.id;
}

static bool seriesCardComesBefore(const SeriesDetail::Card &a, const SeriesDetail::Card &b) {
    const bool aNumbered = a.sequenceNumber > 0;
    const bool bNumbered = b.sequenceNumber > 0;
    if (aNumbered && bNumbered && a.sequenceNumber != b.sequenceNumber) return a.sequenceNumber < b.sequenceNumber;
    if (aNumbered != bNumbered) return aNumbered;
    if (a.title != b.title) return a.title < b.title;
    return a.id < b.id;
}

// Build authors from the full library (runs on net task, no server needed).
static std::vector<AuthorInfo> buildAuthorsList() {
    std::map<String, int> counts;
    for (const auto &c : libraryCards) {
        String a = c.author.length() ? c.author : "(Unknown)";
        counts[a]++;
    }
    std::vector<AuthorInfo> out;
    for (const auto &p : counts) out.push_back({p.first, p.second});
    return out;
}

// Build series from the full library (runs on net task, no server needed).
static std::vector<SeriesDetail> buildSeriesList(bool expand) {
    std::map<String, std::vector<int>> seriesMap;
    for (int i = 0; i < (int)libraryCards.size(); i++) {
        if (libraryCards[i].series.length()) {
            seriesMap[libraryCards[i].series].push_back(i);
        }
    }
    std::vector<SeriesDetail> out;
    for (const auto &p : seriesMap) {
        if (p.second.size() < 2) continue;
        SeriesDetail s;
        s.name = p.first;
        s.count = (int)p.second.size();
        if (expand) {
            for (int idx : p.second) {
                s.cards.push_back({libraryCards[idx].id, libraryCards[idx].title, libraryCards[idx].sequenceNumber});
            }
            std::sort(s.cards.begin(), s.cards.end(), seriesCardComesBefore);
        }
        out.push_back(std::move(s));
    }
    return out;
}

static void netTaskFn(void *) {
    NetRequest req;
    for (;;) {
        if (xQueueReceive(netQueue, &req, portMAX_DELAY) != pdTRUE) continue;

        // Auth commands work even without WiFi check (for polling)
        if (req.cmd == NET_AUTH_POLL) {
            String result = yotoPollDeviceFlow(String(req.strArg));
            xSemaphoreTake(netAuthMtx, portMAX_DELAY);
            netAuthResult = result;
            netAuthReady = true;
            xSemaphoreGive(netAuthMtx);
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) continue;

        switch (req.cmd) {
        case NET_PLAY: {
            yotoPlayCard(String(req.strArg));
            break;
        }
        case NET_PAUSE: { yotoPause(); break; }
        case NET_RESUME: { yotoResume(); break; }
        case NET_STOP: { yotoStop(); break; }
        case NET_VOLUME: { yotoSetVolume(req.intArg); break; }
        case NET_FETCH_MANIFEST: {
            // Fetch full library from Yoto API
            std::vector<YotoCard> yCards;
            std::vector<CardMeta> results;
            bool ok = yotoFetchLibrary(yCards);
            if (ok) {
                for (const auto &yc : yCards) {
                    results.push_back({yc.cardId, yc.title, yc.author, yc.coverUrl,
                                       "", "", "", yc.shareType, yc.series,
                                       yc.sequenceNumber, yc.duration});
                }
            }
            xSemaphoreTake(netManifestMtx, portMAX_DELAY);
            netManifestCards = std::move(results);
            netManifestOk = ok;
            netManifestReady = true;
            xSemaphoreGive(netManifestMtx);
            Serial.printf("[net] manifest done, stack HWM=%u heap=%u internal=%u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            break;
        }
        case NET_FETCH_PAGE: {
            Serial.printf("[net] fetch page %d, heap=%u internal=%u\n",
                req.intArg, (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            netFetchPageIntoCache(req.intArg);
            netPageNum = req.intArg;
            netPageReady = true;
            break;
        }
        case NET_FETCH_DETAIL: {
            YotoCardDetail detail;
            if (yotoFetchCardDetail(String(req.strArg), detail)) {
                xSemaphoreTake(netDetailMtx, portMAX_DELAY);
                netDetailResult = std::move(detail);
                netDetailId = String(req.strArg);
                netDetailReady = true;
                xSemaphoreGive(netDetailMtx);
            }
            break;
        }
        case NET_FETCH_DETAIL_COVER: {
            const String cardId(req.strArg);
            String coverUrl;
            for (const auto &card : libraryCards) {
                if (card.id == cardId) {
                    coverUrl = card.coverUrl;
                    break;
                }
            }

            uint8_t *pixels = nullptr;
            uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            bool ok = false;
            if (coverUrl.length() && internalFree >= DETAIL_COVER_TLS_MIN_INTERNAL) {
                ok = yotoDownloadCover(coverUrl, &pixels, DETAIL_COVER_SIZE);
            } else {
                Serial.printf("[cover] skip detail cover %s internal=%u\n",
                    cardId.c_str(), (unsigned)internalFree);
            }

            xSemaphoreTake(netDetailCoverMtx, portMAX_DELAY);
            if (netDetailCoverPixels) {
                heap_caps_free(netDetailCoverPixels);
                netDetailCoverPixels = nullptr;
            }
            netDetailCoverId = cardId;
            netDetailCoverPixels = ok ? pixels : nullptr;
            netDetailCoverOk = ok;
            netDetailCoverReady = true;
            xSemaphoreGive(netDetailCoverMtx);

            if (!ok && pixels) heap_caps_free(pixels);
            break;
        }
        case NET_FETCH_FILTERS: {
            auto a = buildAuthorsList();
            auto s_full = buildSeriesList(false);
            std::vector<SeriesInfo> s;
            for (const auto &sf : s_full) s.push_back({sf.name, sf.count});

            xSemaphoreTake(netFiltersMtx, portMAX_DELAY);
            netAuthors = std::move(a);
            netSeries = std::move(s);
            netTotalCards = (int)libraryCards.size();
            netFiltersReady = true;
            xSemaphoreGive(netFiltersMtx);
            break;
        }
        case NET_FETCH_SERIES: {
            auto res = buildSeriesList(true);
            xSemaphoreTake(netSeriesBrowseMtx, portMAX_DELAY);
            netSeriesBrowse = std::move(res);
            netSeriesBrowseReady = true;
            xSemaphoreGive(netSeriesBrowseMtx);
            break;
        }
        case NET_FILL_COVER: {
            bool saved = netFillOneCover(req.intArg);
            netCacheFillIndex = req.intArg;
            netCacheFillSaved = saved;
            netCacheFillReady = true;
            break;
        }
        default: break;
        }
    }
}

static void netInit() {
    netManifestMtx    = xSemaphoreCreateMutex();
    netDetailMtx      = xSemaphoreCreateMutex();
    netDetailCoverMtx = xSemaphoreCreateMutex();
    netFiltersMtx     = xSemaphoreCreateMutex();
    netSeriesBrowseMtx = xSemaphoreCreateMutex();
    netAuthMtx        = xSemaphoreCreateMutex();
    netQueue = xQueueCreate(8, sizeof(NetRequest));
    coverCacheMtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(netTaskFn, "net", 24576, nullptr, 2, &netTaskHandle, 0);
}

// Helpers to enqueue requests from UI thread (non-blocking).
static void netPlayCard(const char *cardId) {
    NetRequest r; r.cmd = NET_PLAY; r.intArg = 0;
    strlcpy(r.strArg, cardId, sizeof(r.strArg));
    xQueueSend(netQueue, &r, 0);
}

static void netPause() {
    NetRequest r; r.cmd = NET_PAUSE; r.intArg = 0; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static void netResume() {
    NetRequest r; r.cmd = NET_RESUME; r.intArg = 0; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static void netStop() {
    NetRequest r; r.cmd = NET_STOP; r.intArg = 0; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static void netSetVolume(int vol) {
    NetRequest r; r.cmd = NET_VOLUME; r.intArg = vol; r.strArg[0] = 0;
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

static void netFillCover(int index) {
    NetRequest r; r.cmd = NET_FILL_COVER; r.intArg = index; r.strArg[0] = 0;
    xQueueSend(netQueue, &r, 0);
}

static void netFetchDetail(const char *cardId) {
    NetRequest r; r.cmd = NET_FETCH_DETAIL; r.intArg = 0;
    strlcpy(r.strArg, cardId, sizeof(r.strArg));
    xQueueSend(netQueue, &r, 0);
}

static void netFetchDetailCover(const char *cardId) {
    NetRequest r; r.cmd = NET_FETCH_DETAIL_COVER; r.intArg = 0;
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

static void netAuthPoll(const char *deviceCode) {
    NetRequest r; r.cmd = NET_AUTH_POLL; r.intArg = 0;
    strlcpy(r.strArg, deviceCode, sizeof(r.strArg));
    xQueueSend(netQueue, &r, 0);
}

// ---------- UI State ----------

struct CachedCover {
    uint8_t *alloc;
    uint8_t *data;
    lv_image_dsc_t dsc;
};

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
static int restorePage = 0;
static int pendingPage = -1;
static bool loading = false;
static bool cacheFillInFlight = false;
static int cacheFillIndex = 0;

// Visible-page state: one tile per slot.
static lv_obj_t *slotImg[PAGE_SIZE] = {nullptr};
static String slotId[PAGE_SIZE];

static lv_obj_t *grid = nullptr;
static lv_obj_t *pageLabel = nullptr;
static lv_obj_t *statusLabel = nullptr;
static lv_obj_t *filterBtnLabel = nullptr;
static lv_obj_t *creativeTopBtn = nullptr;
static lv_obj_t *purchasedTopBtn = nullptr;
static lv_obj_t *bootSplash = nullptr;
static lv_obj_t *bootSplashLabel = nullptr;
static lv_obj_t *prevBtn = nullptr;
static lv_obj_t *nextBtn = nullptr;
static lv_obj_t *nowPlayingBar = nullptr;
static lv_obj_t *nowPlayingImg = nullptr;
static lv_obj_t *nowPlayingLabel = nullptr;
static String nowPlayingId = "";
static String nowPlayingTitle = "";

static WebServer otaServer(80);

static const int THUMB_BYTES = THUMB_SIZE * THUMB_SIZE * 2;
static const int DETAIL_COVER_BYTES = DETAIL_COVER_SIZE * DETAIL_COVER_SIZE * 2;
static int maxPageCount() { return max(1, (int)((visibleCardIdx.size() + PAGE_SIZE - 1) / PAGE_SIZE)); }

static uint8_t *allocAlignedPsram(size_t bytes, uint8_t **rawOut) {
    uint8_t *raw = (uint8_t *)heap_caps_malloc(bytes + 63, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        if (rawOut) *rawOut = nullptr;
        return nullptr;
    }
    uintptr_t aligned = ((uintptr_t)raw + 63) & ~(uintptr_t)63;
    if (rawOut) *rawOut = raw;
    return (uint8_t *)aligned;
}

static void freeAlignedPsram(uint8_t *raw) {
    if (raw) heap_caps_free(raw);
}

static bool isCreativeCard(const CardMeta &card) {
    return card.shareType == "myo" || card.shareType.length() == 0;
}

static bool cardMatchesSource(const CardMeta &card) {
    if (currentSource == "creative") return isCreativeCard(card);
    if (currentSource == "purchased") return !isCreativeCard(card);
    return true;
}

static void setBootSplashText(const char *text) {
    if (bootSplashLabel) lv_label_set_text(bootSplashLabel, text ? text : "Waking up...");
}

static void hideBootSplash() {
    if (!bootSplash) return;
    lv_obj_delete_async(bootSplash);
    bootSplash = nullptr;
    bootSplashLabel = nullptr;
}

static void syncPixels(uint8_t *data, size_t bytes) {
    if (!data) return;
    esp_err_t err = esp_cache_msync(
        data,
        bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
            ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK) {
        Serial.printf("[cover] cache sync failed: %d\n", (int)err);
    }
}

static void syncCoverPixels(uint8_t *data) {
    syncPixels(data, THUMB_BYTES);
}

static CachedCover *getOrAllocCover(const String &id) {
    auto it = coverCache.find(id);
    if (it != coverCache.end()) return it->second;
    CachedCover *cv = new CachedCover();
    cv->alloc = nullptr;
    cv->data = allocAlignedPsram(THUMB_BYTES, &cv->alloc);
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
    xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
    CachedCover *cv = getOrAllocCover(id);
    if (!cv || !cv->data) {
        xSemaphoreGive(coverCacheMtx);
        return false;
    }
    if (!sdcache::loadCover(id, cv->data, THUMB_BYTES)) {
        coverCache.erase(id);
        freeAlignedPsram(cv->alloc);
        delete cv;
        xSemaphoreGive(coverCacheMtx);
        return false;
    }
    syncCoverPixels(cv->data);
    lv_image_cache_drop(&cv->dsc);
    xSemaphoreGive(coverCacheMtx);
    return true;
}

static bool isCovered(const String &id) {
    xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
    auto it = coverCache.find(id);
    bool has = it != coverCache.end() && it->second && it->second->data;
    xSemaphoreGive(coverCacheMtx);
    return has;
}

// Background page fetch — runs on net task, writes into coverCache directly.
// Must NOT touch LVGL objects. Returns true if all covers landed.
static bool netFetchPageIntoCache(int page) {
    int start = page * PAGE_SIZE;
    int count = min((int)visibleCardIdx.size() - start, PAGE_SIZE);
    if (count <= 0) return true;

    // Snapshot card info while holding mutex, then release for slow I/O
    struct FetchJob { String cardId; String coverUrl; String title; };
    FetchJob jobs[PAGE_SIZE];
    int jobCount = 0;

    xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        const CardMeta &card = visibleCardAt(start + i);
        jobs[jobCount].cardId = card.id;
        jobs[jobCount].coverUrl = card.coverUrl;
        jobs[jobCount].title = card.title;
        jobCount++;
    }
    xSemaphoreGive(coverCacheMtx);

    for (int i = 0; i < jobCount; i++) {
        const String &cardId = jobs[i].cardId;
        const String &coverUrl = jobs[i].coverUrl;
        if (!cardId.length()) continue;
        if (isCovered(cardId)) {
            vTaskDelay(1);
            continue;
        }

        bool fetched = false;
        uint8_t *coverPixels = nullptr;

        // Try SD cache first
        if (sdcache::hasCover(cardId)) {
            coverPixels = (uint8_t *)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (coverPixels && sdcache::loadCover(cardId, coverPixels, THUMB_BYTES)) {
                Serial.printf("[cover] %s from SD cache\n", cardId.c_str());
                fetched = true;
            } else if (coverPixels) {
                heap_caps_free(coverPixels);
                coverPixels = nullptr;
            }
        }

        // Download + decode from Yoto CDN
        if (!fetched && coverUrl.length() && yotoDownloadCover(coverUrl, &coverPixels, THUMB_SIZE)) {
            sdcache::saveCover(cardId, coverPixels, THUMB_BYTES);
            Serial.printf("[cover] %s (%s) downloaded OK\n", cardId.c_str(), jobs[i].title.c_str());
            fetched = true;
        }

        if (!fetched) {
            coverPixels = (uint8_t *)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (coverPixels) {
                memset(coverPixels, 0xEF, THUMB_BYTES);
                Serial.printf("[cover] %s no cover\n", cardId.c_str());
                fetched = true;
            } else {
                Serial.printf("[cover] %s no cover buffer\n", cardId.c_str());
            }
        }

        if (fetched && coverPixels) {
            // Commit under lock after slow I/O so LVGL never sees a partial cover.
            xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
            CachedCover *cv = getOrAllocCover(cardId);
            if (cv && cv->data) {
                memcpy(cv->data, coverPixels, THUMB_BYTES);
                syncCoverPixels(cv->data);
                // Note: lv_image_cache_drop removed from net task to avoid race with UI task.
                // UI task will handle refresh in renderGrid().
                Serial.printf("[cover] committed %s to buf %p\n", cardId.c_str(), cv->data);
            }
            xSemaphoreGive(coverCacheMtx);
        }
        if (coverPixels) heap_caps_free(coverPixels);
        vTaskDelay(1);
    }
    return true;
}

static bool netFillOneCover(int index) {
    if (index < 0 || index >= (int)libraryCards.size()) return false;
    CardMeta card = libraryCards[index];
    if (!card.id.length() || !card.coverUrl.length()) return false;
    if (sdcache::hasCover(card.id)) return false;

    uint8_t *coverPixels = nullptr;
    if (!yotoDownloadCover(card.coverUrl, &coverPixels, THUMB_SIZE)) return false;
    bool saved = sdcache::saveCover(card.id, coverPixels, THUMB_BYTES);
    if (coverPixels) heap_caps_free(coverPixels);
    if (saved) {
        Serial.printf("[cover] background cached %s (%d/%u)\n",
            card.id.c_str(), index + 1, (unsigned)libraryCards.size());
    }
    vTaskDelay(1);
    return saved;
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
static lv_obj_t *detailCoverImg = nullptr;
static uint8_t *detailCoverPixels = nullptr;
static lv_image_dsc_t detailCoverDsc;
static lv_obj_t *detailDescLbl = nullptr;
static lv_obj_t *detailChipsRow = nullptr;
static lv_obj_t *detailAuthorLbl = nullptr;
static lv_obj_t *detailSeriesBtn = nullptr;
static char *detailSeriesName = nullptr;

static void reloadManifest();
static void applyLocalLibraryView(int targetPage);

static void clearDetailCoverPixels() {
    if (detailCoverImg) lv_image_set_src(detailCoverImg, NULL);
    if (detailCoverPixels) {
        lv_image_cache_drop(&detailCoverDsc);
        heap_caps_free(detailCoverPixels);
        detailCoverPixels = nullptr;
    }
    memset(&detailCoverDsc, 0, sizeof(detailCoverDsc));
}

static void dismissCardDetail() {
    if (detailFetchTimer) { lv_timer_delete(detailFetchTimer); detailFetchTimer = nullptr; }
    clearDetailCoverPixels();
    if (cardDetail) {
        lv_obj_delete_async(cardDetail);
        cardDetail = nullptr;
    }
    detailCoverImg = nullptr;
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
        currentSource = "";
        dismissCardDetail();
        applyLocalLibraryView(0);
        return;
    }
    dismissCardDetail();
    openSeriesModal(nullptr);
}

static void onDetailPlay(lv_event_t *) {
    if (!cardDetailId) return;
    Serial.printf("[ui] play %s\n", cardDetailId);
    playCardNow(String(cardDetailId));
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
    netFetchDetailCover(cardDetailId);
}

static void processDetailCoverResult(const String &id, uint8_t *pixels) {
    if (!cardDetail || !cardDetailId || id != String(cardDetailId) || !pixels) {
        if (pixels) heap_caps_free(pixels);
        return;
    }
    clearDetailCoverPixels();
    detailCoverPixels = pixels;
    memset(&detailCoverDsc, 0, sizeof(detailCoverDsc));
    detailCoverDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    detailCoverDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    detailCoverDsc.header.w = DETAIL_COVER_SIZE;
    detailCoverDsc.header.h = DETAIL_COVER_SIZE;
    detailCoverDsc.header.stride = DETAIL_COVER_SIZE * 2;
    detailCoverDsc.data_size = DETAIL_COVER_BYTES;
    detailCoverDsc.data = detailCoverPixels;
    syncPixels(detailCoverPixels, DETAIL_COVER_BYTES);

    if (detailCoverImg) {
        lv_image_cache_drop(&detailCoverDsc);
        lv_image_set_src(detailCoverImg, &detailCoverDsc);
        lv_image_set_scale(detailCoverImg, 256);
        lv_obj_center(detailCoverImg);
        lv_obj_invalidate(detailCoverImg);
    }
}

// Process card detail result from net task.
static void processDetailResult(const YotoCardDetail &detail) {
    if (!cardDetail) return;

    if (detailAuthorLbl && detail.author.length()) {
        lv_label_set_text_fmt(detailAuthorLbl, LV_SYMBOL_EDIT "  by %s", detail.author.c_str());
    }

    if (detailChipsRow) {
        lv_obj_clean(detailChipsRow);
        if (detail.series.length()) {
            char buf[96];
            if (detail.sequenceNumber > 0) snprintf(buf, sizeof(buf), "%s  #%d", detail.series.c_str(), detail.sequenceNumber);
            else                           snprintf(buf, sizeof(buf), "%s", detail.series.c_str());
            addChip(detailChipsRow, LV_SYMBOL_LIST, buf, 0x6f42c1);
        }
        if (detail.duration > 0) {
            char buf[32];
            int h = detail.duration / 3600, m = (detail.duration % 3600) / 60;
            if (h > 0) snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
            else       snprintf(buf, sizeof(buf), "%d min", m ? m : 1);
            addChip(detailChipsRow, LV_SYMBOL_AUDIO, buf, 0x1f6feb);
        }
    }

    // Capture series name for the Series button.
    if (detailSeriesName) { free(detailSeriesName); detailSeriesName = nullptr; }
    if (detail.series.length()) {
        detailSeriesName = strdup(detail.series.c_str());
        if (detailSeriesBtn) lv_obj_remove_flag(detailSeriesBtn, LV_OBJ_FLAG_HIDDEN);
    }

    if (detailDescLbl && detail.description.length()) {
        lv_label_set_text(detailDescLbl, detail.description.c_str());
    } else if (detailDescLbl) {
        lv_label_set_text(detailDescLbl, "Tap PLAY to begin the story.");
    }
}

static void openCardDetail(const char *id, const char *title) {
    if (cardDetail) return;
    cardDetailId = strdup(id ? id : "");

    // Find known metadata from the full library immediately.
    const CardMeta *known = nullptr;
    for (const auto &c : libraryCards) { if (c.id == String(cardDetailId)) { known = &c; break; } }

    // Per-card colour theme.
    uint16_t hue = hueForId(cardDetailId);
    lv_color_t accent = lv_color_hsv_to_rgb(hue, 70, 95);
    lv_color_t bgTop = lv_color_hsv_to_rgb(hue, 35, 22);

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

    // Cover — floating glow
    lv_obj_t *coverWrap = lv_obj_create(cardDetail);
    lv_obj_set_size(coverWrap, DETAIL_COVER_SIZE + 20, DETAIL_COVER_SIZE + 20);
    lv_obj_align(coverWrap, LV_ALIGN_LEFT_MID, 42, 0);
    lv_obj_set_style_bg_color(coverWrap, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_bg_opa(coverWrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(coverWrap, 3, 0);
    lv_obj_set_style_border_color(coverWrap, accent, 0);
    lv_obj_set_style_radius(coverWrap, 12, 0);
    lv_obj_set_style_pad_all(coverWrap, 8, 0);
    lv_obj_remove_flag(coverWrap, LV_OBJ_FLAG_SCROLLABLE);

    detailCoverImg = lv_image_create(coverWrap);
    auto cv = coverCache.find(String(cardDetailId));
    if (cv != coverCache.end() && cv->second && cv->second->data) {
        lv_image_set_src(detailCoverImg, &cv->second->dsc);
        lv_image_set_scale(detailCoverImg, (uint32_t)(256 * DETAIL_COVER_SIZE / THUMB_SIZE));
    }
    lv_obj_align(detailCoverImg, LV_ALIGN_CENTER, 0, 0);

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

    // Author (filled immediately if known).
    detailAuthorLbl = lv_label_create(right);
    if (known && known->author.length()) {
        lv_label_set_text_fmt(detailAuthorLbl, LV_SYMBOL_EDIT "  by %s", known->author.c_str());
    } else {
        lv_label_set_text(detailAuthorLbl, LV_SYMBOL_EDIT "  ...");
    }
    lv_obj_set_style_text_color(detailAuthorLbl, lv_color_hex(0xc9d1d9), 0);
    lv_obj_set_style_text_font(detailAuthorLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(detailAuthorLbl, LV_ALIGN_TOP_LEFT, 2, 64);

    // Chips row
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

    if (known && known->series.length()) {
        char buf[96];
        if (known->sequenceNumber > 0) snprintf(buf, sizeof(buf), "%s  #%d", known->series.c_str(), known->sequenceNumber);
        else                           snprintf(buf, sizeof(buf), "%s", known->series.c_str());
        addChip(detailChipsRow, LV_SYMBOL_LIST, buf, 0x6f42c1);
    }
    if (known && known->duration > 0) {
        char buf[32];
        int h = known->duration / 3600, m = (known->duration % 3600) / 60;
        if (h > 0) snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
        else       snprintf(buf, sizeof(buf), "%d min", m ? m : 1);
        addChip(detailChipsRow, LV_SYMBOL_AUDIO, buf, 0x1f6feb);
    }
    if (known && known->category.length()) {
        addChip(detailChipsRow, LV_SYMBOL_DIRECTORY, known->category.c_str(), 0x8b949e);
    }

    // Description container
    lv_obj_t *descBox = lv_obj_create(right);
    lv_obj_set_size(descBox, 410, 160);
    lv_obj_align(descBox, LV_ALIGN_TOP_LEFT, 0, 136);
    lv_obj_set_style_bg_opa(descBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(descBox, 0, 0);
    lv_obj_set_style_pad_all(descBox, 0, 0);
    lv_obj_set_scrollbar_mode(descBox, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(descBox, LV_OBJ_FLAG_SCROLLABLE);

    detailDescLbl = lv_label_create(descBox);
    if (known && known->description.length()) lv_label_set_text(detailDescLbl, known->description.c_str());
    else                                     lv_label_set_text(detailDescLbl, "Loading...");
    lv_label_set_long_mode(detailDescLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detailDescLbl, 410);
    lv_obj_set_style_text_color(detailDescLbl, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_text_font(detailDescLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(detailDescLbl, 4, 0);
    lv_obj_align(detailDescLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // PLAY button
    lv_obj_t *playBtn = lv_btn_create(right);
    lv_obj_set_size(playBtn, 180, 70);
    lv_obj_align(playBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(playBtn, lv_color_hex(0x2ea043), 0);
    lv_obj_set_style_radius(playBtn, 35, 0);
    lv_obj_add_event_cb(playBtn, onDetailPlay, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *playLbl = lv_label_create(playBtn);
    lv_label_set_text(playLbl, LV_SYMBOL_PLAY "  PLAY");
    lv_obj_set_style_text_font(playLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(playLbl);

    // Series button
    detailSeriesBtn = lv_btn_create(right);
    lv_obj_set_size(detailSeriesBtn, 180, 70);
    lv_obj_align(detailSeriesBtn, LV_ALIGN_BOTTOM_RIGHT, -200, 0);
    lv_obj_set_style_bg_color(detailSeriesBtn, lv_color_hex(0x6f42c1), 0);
    lv_obj_set_style_radius(detailSeriesBtn, 35, 0);
    lv_obj_add_event_cb(detailSeriesBtn, onDetailSeries, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sLbl = lv_label_create(detailSeriesBtn);
    lv_label_set_text(sLbl, LV_SYMBOL_LIST "  Series");
    lv_obj_set_style_text_font(sLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(sLbl);
    
    if (known && known->series.length()) {
        detailSeriesName = strdup(known->series.c_str());
        lv_obj_remove_flag(detailSeriesBtn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(detailSeriesBtn, LV_OBJ_FLAG_HIDDEN);
    }

    detailFetchTimer = lv_timer_create(fetchDetailExtras, 20, nullptr);
    lv_timer_set_repeat_count(detailFetchTimer, 1);
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
    int keepEnd   = min((int)visibleCardIdx.size(), (currentPage + 2) * PAGE_SIZE);
    // Build a set of IDs to keep.
    std::set<String> keep;
    for (int i = keepStart; i < keepEnd; i++) keep.insert(visibleCardAt(i).id);
    // Walk the cache and free anything outside the keep set.
    xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
    for (auto it = coverCache.begin(); it != coverCache.end(); ) {
        if (keep.count(it->first) == 0) {
            if (it->second) {
                const lv_image_dsc_t *src = &it->second->dsc;
                for (int i = 0; i < PAGE_SIZE; i++) {
                    if (slotImg[i] && lv_image_get_src(slotImg[i]) == src) {
                        lv_image_set_src(slotImg[i], NULL);
                    }
                }
                lv_image_cache_drop(src);
                freeAlignedPsram(it->second->alloc);
                delete it->second;
            }
            it = coverCache.erase(it);
        } else {
            ++it;
        }
    }
    xSemaphoreGive(coverCacheMtx);
}

static void renderGrid() {
    evictDistantCovers();

    int start = currentPage * PAGE_SIZE;
    int end = min((int)visibleCardIdx.size(), start + PAGE_SIZE);

    xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
    for (int i = 0; i < PAGE_SIZE; i++) {
        lv_obj_t *cont = lv_obj_get_child(grid, i);
        if (!cont) continue;

        if (i < end - start) {
            const CardMeta &cm = visibleCardAt(start + i);
            slotId[i] = cm.id;

            lv_obj_t *img = lv_obj_get_child(cont, 0);
            lv_obj_t *l = lv_obj_get_child(cont, 1);
            lv_obj_t *badge = lv_obj_get_child(cont, 2);

            lv_label_set_text(l, cm.title.c_str());
            if (badge) {
                if (cm.sequenceNumber > 0) {
                    lv_label_set_text_fmt(badge, "#%d", cm.sequenceNumber);
                    lv_obj_remove_flag(badge, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
                }
            }
            
            auto it = coverCache.find(cm.id);
            if (it != coverCache.end() && it->second && it->second->data) {
                lv_image_cache_drop(&it->second->dsc);
                lv_image_set_src(img, NULL);
                lv_image_set_src(img, &it->second->dsc);
                lv_obj_invalidate(img); // Force redraw
            } else {
                lv_image_set_src(img, NULL);
            }
            slotImg[i] = img;

            // Update user data (title string) for the detail view
            char *oldTitle = (char *)lv_obj_get_user_data(cont);
            if (oldTitle) free(oldTitle);
            lv_obj_set_user_data(cont, strdup(cm.title.c_str()));
            
            lv_obj_remove_flag(cont, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);
            slotImg[i] = nullptr;
            slotId[i] = "";
        }
    }
    xSemaphoreGive(coverCacheMtx);

    lv_label_set_text_fmt(pageLabel, "Page %d / %d", currentPage + 1, maxPageCount());
}

static void loadPage(int page);
static void flashBtn(lv_obj_t *b);

// ---------- manifest persistence (NVS) ----------
// Legacy compact NVS manifest helpers. The active library manifest lives on SD.
//
// Layout: [u8 sig_len][sig bytes][u16 card_count]
//   repeated card_count times:
//     [u8 id_len][id bytes][u8 title_len][title bytes]

static String manifestSignature() {
    return "library-v4";
}

static void updateFilterButtonLabel() {
    if (filterBtnLabel) {
        String fb;
        if (currentSeries.length())      fb = String(LV_SYMBOL_LIST "  ") + currentSeries;
        else if (currentAuthor.length()) fb = String(LV_SYMBOL_EDIT "  ") + currentAuthor;
        else                              fb = String(LV_SYMBOL_LIST "  Series");
        lv_label_set_text(filterBtnLabel, fb.c_str());
    }
    if (creativeTopBtn) {
        lv_obj_set_style_bg_color(creativeTopBtn, lv_color_hex(currentSource == "creative" ? 0x238636 : 0x30363d), 0);
    }
    if (purchasedTopBtn) {
        lv_obj_set_style_bg_color(purchasedTopBtn, lv_color_hex(currentSource == "purchased" ? 0x1f6feb : 0x30363d), 0);
    }
}

static void saveUiState() {
    Preferences p;
    if (!p.begin("yoto_ui", false)) return;
    p.putString("sort", currentSort);
    p.putString("author", currentAuthor);
    p.putString("series", currentSeries);
    p.putString("source", currentSource);
    p.putInt("page", currentPage);
    p.end();
}

static void loadUiState() {
    Preferences p;
    if (!p.begin("yoto_ui", true)) return;
    currentSort = p.getString("sort", currentSort);
    currentAuthor = p.getString("author", currentAuthor);
    currentSeries = p.getString("series", currentSeries);
    currentSource = p.getString("source", currentSource);
    restorePage = max(0, (int)p.getInt("page", 0));
    p.end();
}

static void saveManifestNvs() {
    if (libraryCards.empty()) return;
    String sig = manifestSignature();
    // Pre-compute size to allocate once.
    size_t total = 1 + sig.length() + 2;
    for (const auto &c : libraryCards) {
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
    uint16_t n = (uint16_t)libraryCards.size();
    buf[o++] = (uint8_t)(n & 0xFF);
    buf[o++] = (uint8_t)(n >> 8);
    for (const auto &c : libraryCards) {
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

    libraryCards.clear();
    libraryCards.reserve(n);
    for (uint16_t i = 0; i < n && o < len; i++) {
        uint8_t idl = buf[o++]; if (o + idl > len) break;
        CardMeta m;
        m.id.reserve(idl);
        for (uint8_t k = 0; k < idl; k++) m.id += (char)buf[o++];
        if (o >= len) break;
        uint8_t tl = buf[o++]; if (o + tl > len) break;
        m.title.reserve(tl);
        for (uint8_t k = 0; k < tl; k++) m.title += (char)buf[o++];
        if (m.id.length()) libraryCards.push_back(m);
    }
    free(buf);
    Serial.printf("[nvs] loaded manifest %u cards\n", (unsigned)libraryCards.size());
    return !libraryCards.empty();
}

static void applyLibraryView(int targetPage) {
    visibleCardIdx.clear();
    visibleCardIdx.reserve(libraryCards.size());

    for (int i = 0; i < (int)libraryCards.size(); i++) {
        const CardMeta &card = libraryCards[i];
        if (!cardMatchesSource(card)) continue;
        if (currentAuthor.length() && card.author != currentAuthor) continue;
        if (currentSeries.length() && card.series != currentSeries) continue;
        visibleCardIdx.push_back((uint16_t)i);
    }

    if (currentSeries.length()) {
        std::sort(visibleCardIdx.begin(), visibleCardIdx.end(),
            [](uint16_t a, uint16_t b) { return cardComesBeforeInSeries(libraryCards[a], libraryCards[b]); });
    } else if (currentSort == "title_desc") {
        std::sort(visibleCardIdx.begin(), visibleCardIdx.end(),
            [](uint16_t a, uint16_t b) { return libraryCards[a].title > libraryCards[b].title; });
    } else if (currentSort == "author") {
        std::sort(visibleCardIdx.begin(), visibleCardIdx.end(),
            [](uint16_t a, uint16_t b) {
                const CardMeta &ca = libraryCards[a];
                const CardMeta &cb = libraryCards[b];
                if (ca.author != cb.author) return ca.author < cb.author;
                return ca.title < cb.title;
            });
    } else {
        std::sort(visibleCardIdx.begin(), visibleCardIdx.end(),
            [](uint16_t a, uint16_t b) { return libraryCards[a].title < libraryCards[b].title; });
    }

    Serial.printf("[manifest] view %u/%u cards (source=%s, author=%s, series=%s, sort=%s)\n",
                  (unsigned)visibleCardIdx.size(), (unsigned)libraryCards.size(),
                  currentSource.length() ? currentSource.c_str() : "*",
                  currentAuthor.length() ? currentAuthor.c_str() : "*",
                  currentSeries.length() ? currentSeries.c_str() : "*",
                  currentSort.c_str());

    updateFilterButtonLabel();
    cacheFillIndex = 0;
    cacheFillInFlight = false;
    pendingPage = -1;
    loadPage(min(max(0, targetPage), maxPageCount() - 1));
}

static void applyLocalLibraryView(int targetPage) {
    if (libraryCards.empty()) {
        lv_label_set_text(statusLabel, "No library cache");
        return;
    }
    applyLibraryView(targetPage);
}

// Pull a fresh full manifest from Yoto. Filtering/sorting is applied locally after it arrives.
// Now non-blocking: enqueues the request to the net task. Result is polled in loop().
static void reloadManifest() {
    updateFilterButtonLabel();
    lv_label_set_text(statusLabel, "Refreshing...");
    netFetchManifest();
}

// Process manifest result from net task (called from loop polling).
static void processManifestResult(std::vector<CardMeta> cards, bool ok) {
    if (!ok) {
        Serial.println("[manifest] refresh failed; keeping existing cards");
        if (statusLabel) lv_label_set_text(statusLabel, visibleCardIdx.empty() ? "Refresh failed" : "Offline cache");
        hideBootSplash();
        loading = false;
        return;
    }

    libraryCards = std::move(cards);
    if (libraryCards.size()) sdcache::saveManifest(manifestSignature(), libraryCards);
    int targetPage = restorePage;
    restorePage = 0;
    applyLibraryView(targetPage);
    hideBootSplash();
}

static void loadPage(int page) {
    loading = true;
    currentPage = page;
    pendingPage = -1;
    saveUiState();

    // Render skeleton immediately (uses cache for covers already present).
    renderGrid();

    // Check which covers on this page are still missing.
    int start = page * PAGE_SIZE;
    int count = min((int)visibleCardIdx.size() - start, PAGE_SIZE);
    bool allCached = true;
    for (int i = 0; i < count; i++) {
        if (!isCovered(visibleCardAt(start + i).id)) { allCached = false; break; }
    }

    if (allCached) {
        lv_label_set_text(statusLabel, "Ready");
        loading = false;
    } else {
        lv_label_set_text(statusLabel, "Loading covers...");
        netFetchPage(page);
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
            for (int i = a * PAGE_SIZE; i < min((int)visibleCardIdx.size(), (a + 1) * PAGE_SIZE); i++) {
                if (!isCovered(visibleCardAt(i).id)) return a;
            }
        }
        if (d == 0) continue; // don't check currentPage - 0 twice
        int b = currentPage - d;
        if (b >= 0 && b < total) {
            for (int i = b * PAGE_SIZE; i < min((int)visibleCardIdx.size(), (b + 1) * PAGE_SIZE); i++) {
                if (!isCovered(visibleCardAt(i).id)) return b;
            }
        }
    }
    return -1;
}

static void backgroundPrefetch() {
    if (loading || pendingPage >= 0) return;

    int p = nextPrefetchPage();
    if (p < 0) return;
    loading = true;
    netFetchPage(p);
    // loading cleared when netPageReady is consumed in loop()
}

static void backgroundCacheFill() {
    if (loading || pendingPage >= 0 || cacheFillInFlight || libraryCards.empty()) return;
    if (cacheFillIndex >= (int)libraryCards.size()) return;

    int start = cacheFillIndex;
    for (int attempts = 0; attempts < (int)libraryCards.size(); attempts++) {
        int idx = (start + attempts) % (int)libraryCards.size();
        if (libraryCards[idx].coverUrl.length() && !sdcache::hasCover(libraryCards[idx].id)) {
            cacheFillIndex = idx;
            cacheFillInFlight = true;
            netFillCover(idx);
            return;
        }
    }
    cacheFillIndex = (int)libraryCards.size();
}

static void onPrev(lv_event_t *) {
    flashBtn(prevBtn);
    int p = (pendingPage >= 0 ? pendingPage : currentPage);
    if (p > 0) {
        int target = p - 1;
        if (loading) {
            pendingPage = target;
            currentPage = target;
            renderGrid();
            lv_label_set_text(statusLabel, "Loading...");
        } else {
            loadPage(target);
        }
    }
}
static void onNext(lv_event_t *) {
    flashBtn(nextBtn);
    int p = (pendingPage >= 0 ? pendingPage : currentPage);
    if (p + 1 < maxPageCount()) {
        int target = p + 1;
        if (loading) {
            pendingPage = target;
            currentPage = target;
            renderGrid();
            lv_label_set_text(statusLabel, "Loading...");
        } else {
            loadPage(target);
        }
    }
}
static void onPause(lv_event_t *)  { netPause(); }
static void onResume(lv_event_t *) { netResume(); }

static void onHomeClick(lv_event_t *) {
    if (currentSeries.length() || currentAuthor.length() || currentSource.length()) {
        currentSeries = "";
        currentAuthor = "";
        currentSource = "";
        applyLocalLibraryView(0);
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
    currentSource = "";
    dismissModal();
    applyLocalLibraryView(0);
}

static void onSourcePick(lv_event_t *e) {
    const char *source = (const char *)lv_event_get_user_data(e);
    currentSource = (source && source[0]) ? String(source) : String("");
    currentAuthor = "";
    currentSeries = "";
    dismissModal();
    applyLocalLibraryView(0);
}

static void setSourceFilter(const char *source) {
    String next = (source && source[0]) ? String(source) : String("");
    currentSource = (currentSource == next) ? String("") : next;
    currentAuthor = "";
    currentSeries = "";
    applyLocalLibraryView(0);
}

static void onCreativeTop(lv_event_t *) {
    setSourceFilter("creative");
}

static void onPurchasedTop(lv_event_t *) {
    setSourceFilter("purchased");
}

static void onSeriesPick(lv_event_t *e) {
    const char *name = (const char *)lv_event_get_user_data(e);
    currentSeries = (name && name[0]) ? String(name) : String("");
    currentAuthor = "";
    currentSource = "";
    dismissModal();
    applyLocalLibraryView(0);
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
static void buildFilterModal(const std::vector<AuthorInfo> &authors, const std::vector<SeriesInfo> &series, int total) {
    lv_label_set_text(statusLabel, "Ready");

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

    char allLabel[48];
    snprintf(allLabel, sizeof(allLabel), "All cards  (%d)", total);
    lv_obj_t *allBtn = lv_list_add_button(list, NULL, allLabel);
    char *emptyStr = strdup("");
    lv_obj_add_event_cb(allBtn, onSourcePick, LV_EVENT_CLICKED, emptyStr);
    lv_obj_add_event_cb(allBtn, freePickStr, LV_EVENT_DELETE, emptyStr);

    int creativeCount = 0;
    int purchasedCount = 0;
    for (const auto &c : libraryCards) {
        if (isCreativeCard(c)) creativeCount++;
        else purchasedCount++;
    }

    lv_list_add_text(list, "Type");
    char creativeLabel[64];
    snprintf(creativeLabel, sizeof(creativeLabel), "Creative  (%d)", creativeCount);
    lv_obj_t *creativeBtn = lv_list_add_button(list, LV_SYMBOL_EDIT, creativeLabel);
    char *creativeStr = strdup("creative");
    lv_obj_add_event_cb(creativeBtn, onSourcePick, LV_EVENT_CLICKED, creativeStr);
    lv_obj_add_event_cb(creativeBtn, freePickStr, LV_EVENT_DELETE, creativeStr);

    char purchasedLabel[64];
    snprintf(purchasedLabel, sizeof(purchasedLabel), "Purchased  (%d)", purchasedCount);
    lv_obj_t *purchasedBtn = lv_list_add_button(list, LV_SYMBOL_AUDIO, purchasedLabel);
    char *purchasedStr = strdup("purchased");
    lv_obj_add_event_cb(purchasedBtn, onSourcePick, LV_EVENT_CLICKED, purchasedStr);
    lv_obj_add_event_cb(purchasedBtn, freePickStr, LV_EVENT_DELETE, purchasedStr);

    if (series.size()) {
        lv_list_add_text(list, "Series");
        for (const auto &s : series) {
            char row[112];
            snprintf(row, sizeof(row), "%s  (%d)", s.name.c_str(), s.count);
            lv_obj_t *b = lv_list_add_button(list, NULL, row);
            char *ns = strdup(s.name.c_str());
            lv_obj_add_event_cb(b, onSeriesPick, LV_EVENT_CLICKED, ns);
            lv_obj_add_event_cb(b, freePickStr, LV_EVENT_DELETE, ns);
        }
    }

    if (authors.size()) {
        lv_list_add_text(list, "Authors");
        for (const auto &a : authors) {
            char row[96];
            snprintf(row, sizeof(row), "%s  (%d)", a.name.c_str(), a.count);
            lv_obj_t *b = lv_list_add_button(list, NULL, row);
            char *ns = strdup(a.name.c_str());
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
    playCardNow(String(id));
    dismissSeriesModal();
}

static void onSeriesHeaderPick(lv_event_t *e) {
    const char *name = (const char *)lv_event_get_user_data(e);
    currentSeries = (name && name[0]) ? String(name) : String("");
    currentAuthor = "";
    currentSource = "";
    dismissSeriesModal();
    applyLocalLibraryView(0);
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

static String titleForCardId(const String &id) {
    for (const auto &c : libraryCards) {
        if (c.id == id) return c.title;
    }
    for (uint16_t idx : visibleCardIdx) {
        const CardMeta &c = libraryCards[idx];
        if (c.id == id) return c.title;
    }
    return "";
}

static void updateNowPlayingBar() {
    if (nowPlayingTitle.length()) {
        if (nowPlayingLabel) lv_label_set_text_fmt(nowPlayingLabel, LV_SYMBOL_PLAY "  %s", nowPlayingTitle.c_str());
    } else {
        if (nowPlayingLabel) lv_label_set_text(nowPlayingLabel, LV_SYMBOL_AUDIO "  Tap for player status");
    }

    if (!nowPlayingImg) return;
    lv_image_set_src(nowPlayingImg, NULL);
    if (!nowPlayingId.length()) return;
    xSemaphoreTake(coverCacheMtx, portMAX_DELAY);
    auto it = coverCache.find(nowPlayingId);
    if (it != coverCache.end() && it->second && it->second->data) {
        lv_image_cache_drop(&it->second->dsc);
        lv_image_set_src(nowPlayingImg, &it->second->dsc);
    }
    xSemaphoreGive(coverCacheMtx);
}

static void setNowPlaying(const String &id, const String &title) {
    nowPlayingId = id;
    nowPlayingTitle = title.length() ? title : titleForCardId(id);
    npIsPlaying = true;
    updateNowPlayingBar();
}

static void playCardNow(const String &id) {
    if (!id.length()) return;
    setNowPlaying(id, titleForCardId(id));
    netPlayCard(id.c_str());
}

static void dismissNpModal() {
    if (npModal) { lv_obj_delete_async(npModal); npModal = nullptr; }
    npVolLbl = nullptr;
    npPlayPauseBtn = nullptr;
    npPlayPauseLbl = nullptr;
}

static void onNpClose(lv_event_t *) { dismissNpModal(); }

static void onVolDown(lv_event_t *) {
    npVolume = max(0, npVolume - 10);
    netSetVolume(npVolume);
    if (npVolLbl) lv_label_set_text_fmt(npVolLbl, "%d%%", npVolume);
}

static void onVolUp(lv_event_t *) {
    npVolume = min(100, npVolume + 10);
    netSetVolume(npVolume);
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
        netPause();
        npIsPlaying = false;
    } else {
        netResume();
        npIsPlaying = true;
    }
    updatePlayPauseBtn();
}

static void onNpStop(lv_event_t *) {
    netStop();
    nowPlayingId = "";
    nowPlayingTitle = "";
    npIsPlaying = false;
    updateNowPlayingBar();
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
static void buildSeriesModal(const std::vector<SeriesDetail> &series) {
    lv_label_set_text(statusLabel, "Ready");

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

    if (series.empty()) {
        lv_list_add_text(list, "No series detected");
        return;
    }
    for (const auto &s : series) {
        char hdrTxt[112];
        snprintf(hdrTxt, sizeof(hdrTxt), "%s  (%d)", s.name.c_str(), s.count);
        lv_obj_t *h = lv_list_add_button(list, NULL, hdrTxt);
        lv_obj_set_style_bg_color(h, lv_color_hex(0x21262d), 0);
        lv_obj_set_style_text_color(h, lv_color_hex(0x58a6ff), 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
        char *ns = strdup(s.name.c_str());
        lv_obj_add_event_cb(h, onSeriesHeaderPick, LV_EVENT_CLICKED, ns);
        lv_obj_add_event_cb(h, freePickStr, LV_EVENT_DELETE, ns);

        for (const auto &c : s.cards) {
            char row[128];
            if (c.sequenceNumber > 0) snprintf(row, sizeof(row), "  #%d  %s", c.sequenceNumber, c.title.c_str());
            else snprintf(row, sizeof(row), "  %s", c.title.c_str());
            lv_obj_t *b = lv_list_add_button(list, LV_SYMBOL_PLAY, row);
            char *cidStr = strdup(c.id.c_str());
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

    lv_obj_t *homeBtn = lv_button_create(top);
    lv_obj_set_size(homeBtn, 112, 44);
    lv_obj_set_style_bg_color(homeBtn, lv_color_hex(0x21262d), 0);
    lv_obj_set_style_radius(homeBtn, 8, 0);
    lv_obj_align(homeBtn, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_event_cb(homeBtn, onHomeClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME "  Home");
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(homeLbl);

    creativeTopBtn = lv_button_create(top);
    lv_obj_set_size(creativeTopBtn, 132, 44);
    lv_obj_set_style_bg_color(creativeTopBtn, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(creativeTopBtn, 8, 0);
    lv_obj_align(creativeTopBtn, LV_ALIGN_CENTER, -74, 0);
    lv_obj_add_event_cb(creativeTopBtn, onCreativeTop, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *creativeLbl = lv_label_create(creativeTopBtn);
    lv_label_set_text(creativeLbl, LV_SYMBOL_EDIT "  Creative");
    lv_obj_set_style_text_font(creativeLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(creativeLbl);

    purchasedTopBtn = lv_button_create(top);
    lv_obj_set_size(purchasedTopBtn, 142, 44);
    lv_obj_set_style_bg_color(purchasedTopBtn, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_radius(purchasedTopBtn, 8, 0);
    lv_obj_align(purchasedTopBtn, LV_ALIGN_CENTER, 72, 0);
    lv_obj_add_event_cb(purchasedTopBtn, onPurchasedTop, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *purchasedLbl = lv_label_create(purchasedTopBtn);
    lv_label_set_text(purchasedLbl, LV_SYMBOL_AUDIO "  Purchased");
    lv_obj_set_style_text_font(purchasedLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(purchasedLbl);

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

    nowPlayingImg = lv_image_create(nowPlayingBar);
    lv_obj_set_size(nowPlayingImg, 32, 32);
    lv_image_set_scale(nowPlayingImg, 57);
    lv_obj_set_style_bg_color(nowPlayingImg, lv_color_hex(0x2a313a), 0);
    lv_obj_set_style_bg_opa(nowPlayingImg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nowPlayingImg, 5, 0);
    lv_obj_align(nowPlayingImg, LV_ALIGN_LEFT_MID, 18, 0);

    nowPlayingLabel = lv_label_create(nowPlayingBar);
    lv_label_set_text(nowPlayingLabel, LV_SYMBOL_AUDIO "  Tap for player status");
    lv_label_set_long_mode(nowPlayingLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nowPlayingLabel, LCD_W - 80);
    lv_obj_set_style_text_align(nowPlayingLabel, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(nowPlayingLabel, LV_ALIGN_LEFT_MID, 60, 0);

    // Grid: pre-allocate slots
    grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LCD_W, LCD_H - 60 - 40 - 60);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int cellW = LCD_W / GRID_COLS;
    int cellH = (LCD_H - 60 - 40 - 60) / GRID_ROWS;

    for (int i = 0; i < PAGE_SIZE; i++) {
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
        lv_obj_set_style_border_color(cont, lv_color_hex(0x58a6ff), LV_STATE_PRESSED);

        lv_obj_t *img = lv_image_create(cont);
        lv_obj_set_size(img, THUMB_SIZE, THUMB_SIZE);
        lv_obj_set_style_bg_color(img, lv_color_hex(0x2a313a), 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(img, 8, 0);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *l = lv_label_create(cont);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(l, THUMB_SIZE, 48);
        lv_obj_set_style_bg_color(l, lv_color_hex(0x0a0a0a), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_line_space(l, 1, 0);
        lv_obj_set_style_pad_hor(l, 4, 0);
        lv_obj_set_style_pad_ver(l, 2, 0);
        lv_obj_set_style_radius(l, 0, 0);
        lv_obj_align_to(l, img, LV_ALIGN_BOTTOM_MID, 0, 0);

        lv_obj_t *badge = lv_label_create(cont);
        lv_label_set_text(badge, "#1");
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x238636), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(badge, lv_color_white(), 0);
        lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_hor(badge, 6, 0);
        lv_obj_set_style_pad_ver(badge, 3, 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_align_to(badge, img, LV_ALIGN_TOP_RIGHT, -4, 4);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);

        // Click handler needs to know which slot was clicked
        lv_obj_add_event_cb(cont, [](lv_event_t *e) {
            lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
            int idx = -1;
            for(int k=0; k<PAGE_SIZE; k++) if(lv_obj_get_child(grid, k) == obj) { idx = k; break; }
            if(idx >= 0 && slotId[idx].length() > 0) {
                const char* title = (const char*)lv_obj_get_user_data(obj);
                openCardDetail(slotId[idx].c_str(), title);
            }
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(cont, [](lv_event_t *e) {
            lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
            int idx = -1;
            for(int k=0; k<PAGE_SIZE; k++) if(lv_obj_get_child(grid, k) == obj) { idx = k; break; }
            if(idx >= 0 && slotId[idx].length() > 0) {
                playCardNow(slotId[idx]);
                lv_label_set_text(statusLabel, "Playing");
            }
        }, LV_EVENT_LONG_PRESSED, nullptr);

        // Cleanup user data on delete
        lv_obj_add_event_cb(cont, [](lv_event_t *e) {
            char *t = (char *)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
            if (t) free(t);
        }, LV_EVENT_DELETE, nullptr);
    }

    bootSplash = lv_obj_create(scr);
    lv_obj_set_size(bootSplash, LCD_W, LCD_H);
    lv_obj_align(bootSplash, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bootSplash, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_bg_opa(bootSplash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bootSplash, 0, 0);
    lv_obj_set_style_radius(bootSplash, 0, 0);
    lv_obj_remove_flag(bootSplash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(bootSplash);
    lv_label_set_text(brand, "Sophie's Yoto");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(0x58a6ff), 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -58);

    lv_obj_t *spinner = lv_spinner_create(bootSplash);
    lv_obj_set_size(spinner, 48, 48);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x58a6ff), LV_PART_INDICATOR);

    bootSplashLabel = lv_label_create(bootSplash);
    lv_label_set_text(bootSplashLabel, "Waking up...");
    lv_obj_set_style_text_font(bootSplashLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bootSplashLabel, lv_color_hex(0xc9d1d9), 0);
    lv_obj_align(bootSplashLabel, LV_ALIGN_CENTER, 0, 58);
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
    BOOT_AUTH,         // Yoto auth (device code flow if needed)
    BOOT_FETCH,        // refresh manifest from Yoto API
    BOOT_DONE,
};
static BootStage bootStage = BOOT_SD;
static uint32_t bootStageEntered = 0;

// Auth flow state
static String authDeviceCode;
static uint32_t authNextPoll = 0;
static int authPollInterval = 5;
static lv_obj_t *authOverlay = nullptr;   // full-screen auth panel
static lv_obj_t *authCodeLabel = nullptr; // the big user_code text
static lv_obj_t *authUrlLabel = nullptr;  // the URL text

static void dismissAuthOverlay() {
    if (authOverlay) { lv_obj_del(authOverlay); authOverlay = nullptr; authCodeLabel = nullptr; authUrlLabel = nullptr; }
}

static void showAuthOverlay(const String &userCode, const String &url) {
    dismissAuthOverlay();
    // Full-screen opaque dark panel
    authOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(authOverlay, LCD_W, LCD_H);
    lv_obj_align(authOverlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(authOverlay, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_bg_opa(authOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(authOverlay, 0, 0);
    lv_obj_set_style_radius(authOverlay, 0, 0);
    lv_obj_set_style_pad_all(authOverlay, 20, 0);
    lv_obj_set_flex_flow(authOverlay, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(authOverlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(authOverlay, 30, 0);

    // --- Left side: QR code ---
    // Build the full URL with user_code embedded so scanning goes straight to activation
    String qrUrl = url;
    if (qrUrl.indexOf('?') < 0) {
        qrUrl += "?user_code=" + userCode;
    }
    lv_obj_t *qr = lv_qrcode_create(authOverlay);
    lv_qrcode_set_size(qr, 200);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x161b22));
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_data(qr, qrUrl.c_str());

    // --- Right side: text column ---
    lv_obj_t *col = lv_obj_create(authOverlay);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);

    // Title
    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Yoto Login");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    // Instructions
    lv_obj_t *inst = lv_label_create(col);
    lv_label_set_text(inst, "Scan the QR code, or visit:");
    lv_obj_set_style_text_color(inst, lv_color_hex(0x8b949e), 0);

    // URL
    authUrlLabel = lv_label_create(col);
    lv_label_set_text(authUrlLabel, url.c_str());
    lv_obj_set_style_text_font(authUrlLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(authUrlLabel, lv_color_hex(0x58a6ff), 0);

    // "and enter code:"
    lv_obj_t *inst2 = lv_label_create(col);
    lv_label_set_text(inst2, "and enter this code:");
    lv_obj_set_style_text_color(inst2, lv_color_hex(0x8b949e), 0);

    // Big code
    authCodeLabel = lv_label_create(col);
    lv_label_set_text(authCodeLabel, userCode.c_str());
    lv_obj_set_style_text_font(authCodeLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(authCodeLabel, lv_color_hex(0x3fb950), 0);
    lv_obj_set_style_text_letter_space(authCodeLabel, 10, 0);

    // Waiting message
    lv_obj_t *wait = lv_label_create(col);
    lv_label_set_text(wait, "Waiting for authorization...");
    lv_obj_set_style_text_color(wait, lv_color_hex(0x8b949e), 0);
}

static void enterStage(BootStage s) {
    bootStage = s;
    bootStageEntered = millis();
}

static bool loadCachedLibraryView(const char *statusText) {
    String sig;
    std::vector<CardMeta> cards;
    if (!sdcache::loadManifest(sig, cards) || cards.empty()) return false;
    libraryCards = std::move(cards);
    int targetPage = restorePage;
    restorePage = 0;
    applyLibraryView(targetPage);
    if (statusLabel) lv_label_set_text(statusLabel, statusText);
    hideBootSplash();
    Serial.printf("[boot] cached view heap=%u internal=%u psram=%u\n",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)ESP.getFreePsram());
    return true;
}

static void tickBoot() {
    switch (bootStage) {
    case BOOT_SD: {
        setBootSplashText("Finding WiFi...");
        startWifi();
        enterStage(BOOT_WIFI);
        break;
    }
    case BOOT_WIFI: {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
            setBootSplashText("Signing in...");
            // Disable WiFi power save — keeps PHY active so it doesn't
            // need to re-init (and allocate timers) while TLS uses internal SRAM
            esp_wifi_set_ps(WIFI_PS_NONE);
            // Start ElegantOTA web server — browse to http://<IP>/update
            ElegantOTA.begin(&otaServer);
            otaServer.begin();
            Serial.println("[ota] ElegantOTA ready at /update");
            lv_label_set_text(statusLabel, "Authenticating...");
            enterStage(BOOT_AUTH);
            return;
        }
        if (millis() - bootStageEntered > 30000) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            setBootSplashText("Opening offline library...");
            if (!loadCachedLibraryView("Offline cache")) {
                lv_label_set_text(statusLabel, "WiFi failed");
                setBootSplashText("WiFi failed");
            }
            enterStage(BOOT_DONE);
        }
        break;
    }
    case BOOT_AUTH: {
        // If we already have a refresh token, skip auth
        if (yotoAuthReady()) {
            // Do an eager token refresh NOW (on the main core) while heap is
            // still plentiful. The first TLS handshake allocates PHY timers
            // and needs ~40KB free internal SRAM; doing it here avoids the
            // OOM that would happen if the net task tried it after LVGL has
            // loaded the full grid + cover cache.
            Serial.printf("[auth] eager token refresh, heap=%u internal=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            bool tokenOk = yotoEnsureToken();
            Serial.printf("[auth] after refresh, heap=%u internal=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

            if (!tokenOk) {
                Serial.println("[auth] stored token refresh failed");
                if (!yotoAuthReady()) {
                    Serial.println("[auth] stored credentials rejected; starting login flow");
                    authDeviceCode = "";
                    dismissAuthOverlay();
                } else {
                    setBootSplashText("Opening offline library...");
                    if (!loadCachedLibraryView("Offline cache")) {
                        lv_label_set_text(statusLabel, "Auth refresh failed");
                        setBootSplashText("Login unavailable");
                    }
                    enterStage(BOOT_DONE);
                    return;
                }
            } else {
                lv_label_set_text(statusLabel, visibleCardIdx.empty() ? "Loading..." : "Refreshing...");
                setBootSplashText("Checking cards...");
                dismissAuthOverlay();
                enterStage(BOOT_FETCH);
                return;
            }
        }

        // Check for completed auth poll
        if (netAuthReady) {
            xSemaphoreTake(netAuthMtx, portMAX_DELAY);
            String result = netAuthResult;
            netAuthReady = false;
            xSemaphoreGive(netAuthMtx);

            if (result == "authorized") {
                Serial.println("[auth] authorized!");
                lv_label_set_text(statusLabel, visibleCardIdx.empty() ? "Loading..." : "Refreshing...");
                setBootSplashText("Checking cards...");
                dismissAuthOverlay();
                enterStage(BOOT_FETCH);
                return;
            } else if (result == "expired" || result == "error") {
                Serial.println("[auth] flow failed, restarting...");
                authDeviceCode = "";
                // Will restart flow below
            }
            // "pending" — keep polling
        }

        // Start device code flow if not started
        if (!authDeviceCode.length()) {
            DeviceCodeResult dcr = yotoStartDeviceFlow();
            if (dcr.ok) {
                authDeviceCode = dcr.deviceCode;
                authPollInterval = dcr.interval;
                authNextPoll = millis() + (uint32_t)authPollInterval * 1000;

                // Show full-screen auth overlay with QR code
                // Use verificationUriComplete if available (has user_code in URL)
                String qrUrl = dcr.verificationUriComplete.length()
                    ? dcr.verificationUriComplete
                    : dcr.verificationUri;
                showAuthOverlay(dcr.userCode, qrUrl);
                lv_label_set_text(statusLabel, "Awaiting Yoto login...");
                setBootSplashText("Waiting for login...");
            } else {
                lv_label_set_text(statusLabel, "Auth failed — retrying...");
                setBootSplashText("Retrying login...");
                enterStage(BOOT_AUTH);  // retry in next tick
            }
            return;
        }

        // Poll at interval
        if (millis() >= authNextPoll) {
            authNextPoll = millis() + (uint32_t)authPollInterval * 1000;
            netAuthPoll(authDeviceCode.c_str());
        }
        break;
    }
    case BOOT_FETCH: {
        setBootSplashText("Loading library...");
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
    Wire.setClock(400000);
    touchInit();

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

    loadUiState();
    buildScreen();
    updateFilterButtonLabel();
    lv_label_set_text(statusLabel, "Mounting SD...");
    lv_timer_handler();

    sdcache::begin();
    loadBrightness();
    blSetDuty(BL_DUTY_ACTIVE);
    yotoApiInit();

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
        auto cards = std::move(netManifestCards);
        bool ok = netManifestOk;
        netManifestReady = false;
        xSemaphoreGive(netManifestMtx);
        processManifestResult(std::move(cards), ok);
    }

    // Page covers result — refresh visible tiles
    if (netPageReady) {
        int page = netPageNum;
        netPageReady = false;
        loading = false;
        if (page == currentPage) {
            // Refresh tiles that now have covers
            int start = page * PAGE_SIZE;
            int count = min((int)visibleCardIdx.size() - start, PAGE_SIZE);
            for (int i = 0; i < count; i++) {
                const String &id = visibleCardAt(start + i).id;
                if (slotImg[i] && slotId[i] == id && isCovered(id)) {
                    const lv_image_dsc_t *src = &coverCache[id]->dsc;
                    lv_image_cache_drop(src);
                    lv_image_set_src(slotImg[i], NULL);
                    lv_image_set_src(slotImg[i], src);
                    lv_obj_invalidate(slotImg[i]);
                }
            }
            lv_label_set_text(statusLabel, "Ready");
            updateNowPlayingBar();
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
        YotoCardDetail detail = std::move(netDetailResult);
        String id = std::move(netDetailId);
        netDetailReady = false;
        xSemaphoreGive(netDetailMtx);
        // Only process if the detail view is still showing the same card
        if (cardDetail && cardDetailId && id == String(cardDetailId)) {
            processDetailResult(detail);
        }
    }

    // Larger cover for the currently open card detail view
    if (netDetailCoverReady) {
        xSemaphoreTake(netDetailCoverMtx, portMAX_DELAY);
        String id = std::move(netDetailCoverId);
        uint8_t *pixels = netDetailCoverPixels;
        bool ok = netDetailCoverOk;
        netDetailCoverPixels = nullptr;
        netDetailCoverReady = false;
        netDetailCoverOk = false;
        xSemaphoreGive(netDetailCoverMtx);

        if (ok) processDetailCoverResult(id, pixels);
        else if (pixels) heap_caps_free(pixels);
    }

    // Filter modal data ready
    if (netFiltersReady) {
        xSemaphoreTake(netFiltersMtx, portMAX_DELAY);
        auto a = std::move(netAuthors);
        auto s = std::move(netSeries);
        int total = netTotalCards;
        netFiltersReady = false;
        xSemaphoreGive(netFiltersMtx);
        if (!filterModal) buildFilterModal(a, s, total);
    }

    // Series browser data ready
    if (netSeriesBrowseReady) {
        xSemaphoreTake(netSeriesBrowseMtx, portMAX_DELAY);
        auto series = std::move(netSeriesBrowse);
        netSeriesBrowseReady = false;
        xSemaphoreGive(netSeriesBrowseMtx);
        if (!seriesModal) buildSeriesModal(series);
    }

    if (netCacheFillReady) {
        int idx = netCacheFillIndex;
        bool saved = netCacheFillSaved;
        netCacheFillReady = false;
        cacheFillInFlight = false;
        cacheFillIndex = idx + 1;
        if (saved && statusLabel && !loading) {
            lv_label_set_text(statusLabel, "Caching covers...");
        }
    }

    // Background prefetch (SD only — net prefetch uses the queue)
    static uint32_t lastPrefetch = 0;
    if (!loading && millis() - lastPrefetch > 300) {
        lastPrefetch = millis();
        backgroundPrefetch();
    }

    static uint32_t lastCacheFill = 0;
    if (bootStage == BOOT_DONE && !loading && millis() - lastCacheFill > 2000) {
        lastCacheFill = millis();
        backgroundCacheFill();
    }

    otaServer.handleClient();
    ElegantOTA.loop();
}
