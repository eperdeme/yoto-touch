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
#include <ArduinoJson.h>
#include <vector>
#include <esp_timer.h>

#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <lvgl.h>

#include "pins.h"
#include "secrets.h"

// ---------- constants ----------
static const int THUMB_SIZE = 160;
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
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    lv_display_flush_ready(disp);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    ts.read();
    if (ts.isTouched && ts.touches > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = ts.points[0].x;
        data->point.y = ts.points[0].y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------- HTTP helpers ----------
static String httpGet(const String &path) {
    HTTPClient http;
    String url = String(SERVER_BASE) + path;
    http.begin(url);
    int code = http.GET();
    String body = (code > 0) ? http.getString() : String("");
    if (code != 200) {
        Serial.printf("[http] GET %s -> %d\n", url.c_str(), code);
    }
    http.end();
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

static bool fetchImage(const String &cardId, uint8_t *dest) {
    HTTPClient http;
    String url = String(SERVER_BASE) + "/thumb565/" + cardId;
    http.begin(url);
    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }
    int len = http.getSize();
    if (len <= 0) {
        http.end();
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    int read = 0;
    while (http.connected() && read < len) {
        int avail = stream->available();
        if (avail > 0) {
            int toRead = min(avail, 2048);
            stream->readBytes(dest + read, toRead);
            read += toRead;
        }
        delay(1);
    }
    http.end();
    return read == len;
}

// ---------- UI State ----------
struct Card {
    String id;
    String title;
    lv_image_dsc_t img_dsc;
    uint8_t *img_data;
};

static int currentPage = 0;
static int totalCards = 0;
static std::vector<Card> currentCards;

static lv_obj_t *grid = nullptr;
static lv_obj_t *pageLabel = nullptr;
static lv_obj_t *statusLabel = nullptr;
static lv_obj_t *nowPlayingBar = nullptr;
static lv_obj_t *nowPlayingLabel = nullptr;

static void onCardClick(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    const char *title = (const char *)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    Serial.printf("[ui] play %s\n", id);
    
    lv_label_set_text_fmt(nowPlayingLabel, "Playing: %s", title);
    lv_obj_remove_flag(nowPlayingBar, LV_OBJ_FLAG_HIDDEN);
    
    httpPost(String("/play/") + id);
}

static void renderGrid() {
    if (grid) lv_obj_clean(grid);
    
    int cellW = LCD_W / GRID_COLS;
    int cellH = (LCD_H - 120) / GRID_ROWS;

    for (size_t i = 0; i < currentCards.size(); i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        
        lv_obj_t *cont = lv_obj_create(grid);
        lv_obj_set_size(cont, cellW - 20, cellH - 20);
        lv_obj_set_pos(cont, col * cellW + 10, row * cellH + 10);
        lv_obj_set_style_bg_color(cont, lv_color_hex(0x1c2128), 0);
        lv_obj_set_style_border_width(cont, 1, 0);
        lv_obj_set_style_border_color(cont, lv_color_hex(0x2d333b), 0);
        lv_obj_set_style_radius(cont, 12, 0);
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        
        // Image
        lv_obj_t *img = lv_image_create(cont);
        if (currentCards[i].img_data) {
            lv_image_set_src(img, &currentCards[i].img_dsc);
        } else {
            // Fallback placeholder
            lv_obj_set_style_bg_color(img, lv_color_hex(0x2a313a), 0);
            lv_obj_set_size(img, THUMB_SIZE - 40, THUMB_SIZE - 40);
        }
        lv_obj_set_style_radius(img, 8, 0);
        lv_obj_set_style_clip_corner(img, true, 0);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);

        // Title
        lv_obj_t *l = lv_label_create(cont);
        lv_label_set_text(l, currentCards[i].title.c_str());
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, cellW - 40);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);

        // Click handler on the whole container
        char *id = strdup(currentCards[i].id.c_str());
        char *title = strdup(currentCards[i].title.c_str());
        lv_obj_add_event_cb(cont, onCardClick, LV_EVENT_CLICKED, id);
        lv_obj_set_user_data(cont, title);
        lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    }

    int maxPage = max(1, (totalCards + PAGE_SIZE - 1) / PAGE_SIZE);
    lv_label_set_text_fmt(pageLabel, "Page %d / %d", currentPage + 1, maxPage);
}

static void loadPage(int page) {
    lv_label_set_text(statusLabel, "Loading...");
    lv_timer_handler(); // Update UI to show loading status

    char path[128];
    snprintf(path, sizeof(path), "/cards?page=%d&size=%d", page, PAGE_SIZE);
    String body = httpGet(path);
    if (body.isEmpty()) {
        lv_label_set_text(statusLabel, "Server unreachable");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        lv_label_set_text_fmt(statusLabel, "JSON err: %s", err.c_str());
        return;
    }
    
    totalCards = doc["total"] | 0;
    currentPage = page;
    
    // Cleanup old cards
    for (auto &c : currentCards) {
        if (c.img_data) free(c.img_data);
    }
    currentCards.clear();
    
    JsonArray cards = doc["cards"].as<JsonArray>();
    for (JsonObject c : cards) {
        Card card;
        card.id = String((const char *)(c["cardId"] | ""));
        card.title = String((const char *)(c["title"] | "?"));
        
        // Allocate and fetch image
        card.img_data = (uint8_t *)ps_malloc(THUMB_SIZE * THUMB_SIZE * 2);
        if (card.img_data) {
            if (fetchImage(card.id, card.img_data)) {
                card.img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                card.img_dsc.header.w = THUMB_SIZE;
                card.img_dsc.header.h = THUMB_SIZE;
                card.img_dsc.header.stride = THUMB_SIZE * 2;
                card.img_dsc.data_size = THUMB_SIZE * THUMB_SIZE * 2;
                card.img_dsc.data = card.img_data;
            } else {
                free(card.img_data);
                card.img_data = nullptr;
            }
        }
        currentCards.push_back(card);
    }
    
    renderGrid();
    lv_label_set_text(statusLabel, "Ready");
}

static void onPrev(lv_event_t *) { if (currentPage > 0) loadPage(currentPage - 1); }
static void onNext(lv_event_t *) {
    int maxPage = (totalCards + PAGE_SIZE - 1) / PAGE_SIZE;
    if (currentPage + 1 < maxPage) loadPage(currentPage + 1);
}
static void onPause(lv_event_t *)  { httpPost("/pause"); }
static void onResume(lv_event_t *) { httpPost("/resume"); }

static void buildScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);

    // Top bar
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, LCD_W, 60);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "Sophie's Yoto");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x58a6ff), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);

    statusLabel = lv_label_create(top);
    lv_label_set_text(statusLabel, "Connecting...");
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x8b949e), 0);
    lv_obj_align(statusLabel, LV_ALIGN_RIGHT_MID, -20, 0);

    // Bottom bar
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_set_size(bot, LCD_W, 60);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_radius(bot, 0, 0);
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
    
    mkBtn("< Prev", 20, onPrev);
    pageLabel = lv_label_create(bot);
    lv_label_set_text(pageLabel, "Page 1 / 1");
    lv_obj_align(pageLabel, LV_ALIGN_CENTER, -120, 0);
    mkBtn("Next >", 240, onNext);
    
    mkBtn("Pause", LCD_W - 240, onPause, 0x30363d);
    mkBtn("Play", LCD_W - 120, onResume, 0x238636);

    // Now Playing Bar
    nowPlayingBar = lv_obj_create(scr);
    lv_obj_set_size(nowPlayingBar, LCD_W, 40);
    lv_obj_align(nowPlayingBar, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_bg_color(nowPlayingBar, lv_color_hex(0x238636), 0);
    lv_obj_set_style_border_width(nowPlayingBar, 0, 0);
    lv_obj_set_style_radius(nowPlayingBar, 0, 0);
    lv_obj_add_flag(nowPlayingBar, LV_OBJ_FLAG_HIDDEN);
    
    nowPlayingLabel = lv_label_create(nowPlayingBar);
    lv_label_set_text(nowPlayingLabel, "Nothing playing");
    lv_obj_center(nowPlayingLabel);

    // Grid container - moved AFTER bot to ensure bot isn't overlapping its bottom
    grid = lv_obj_create(scr);
    lv_obj_set_size(grid, LCD_W, LCD_H - 120);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------- WiFi ----------
static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[wifi] connecting to %s...\n", WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(250);
        lv_timer_handler();
    }
    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text(statusLabel, "Ready");
    } else {
        lv_label_set_text(statusLabel, "WiFi failed");
    }
}

// ---------- setup / loop ----------
void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n=== yoto-touch boot (V2) ===");

    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);

    if (!gfx->begin()) Serial.println("[gfx] begin FAILED");
    gfx->fillScreen(0x0000);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    ts.begin();
    ts.setRotation(ROTATION_NORMAL);

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
    lv_timer_handler();

    static const esp_timer_create_args_t targs = {
        .callback = [](void *) { lv_tick_inc(2); },
        .name = "lv_tick"
    };
    esp_timer_handle_t th;
    esp_timer_create(&targs, &th);
    esp_timer_start_periodic(th, 2000);

    connectWifi();
    if (WiFi.status() == WL_CONNECTED) {
        loadPage(0);
    }
    Serial.println("[boot] done");
}

void loop() {
    lv_timer_handler();
    delay(5);
}
