// yoto_api.cpp — Direct Yoto cloud API client for ESP32.
// All functions are blocking; call from the net task only.

#include "yoto_api.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <pngle.h>
#include "sd_cache.h"

// Auth endpoints
static const char *AUTH_BASE = "https://login.yotoplay.com";
static const char *API_BASE  = "https://api.yotoplay.com";
static const char *YOTO_AUDIENCE = "https://api.yotoplay.com";

// From secrets.h — user must add YOTO_CLIENT_ID
#include "secrets.h"
#ifndef YOTO_CLIENT_ID
#error "Define YOTO_CLIENT_ID in secrets.h (get one at https://dashboard.yoto.dev/)"
#endif

static const char *SCOPES = "offline_access family:library:view family:devices:view "
                            "family:devices:control user:content:view";

// Forward declaration
static String urlEncode(const String &s);

// Token storage in NVS
static String s_accessToken;
static String s_refreshToken;
static unsigned long s_tokenExpiresAt = 0;  // millis
static String s_deviceId;
static SemaphoreHandle_t s_authMtx = nullptr;

static void lockAuth() {
    if (s_authMtx) xSemaphoreTake(s_authMtx, portMAX_DELAY);
}

static void unlockAuth() {
    if (s_authMtx) xSemaphoreGive(s_authMtx);
}

static void saveTokens() {
    Preferences p;
    p.begin("yoto_auth", false);
    p.putString("access", s_accessToken);
    p.putString("refresh", s_refreshToken);
    p.putULong("expires", s_tokenExpiresAt);
    p.putString("device", s_deviceId);
    p.end();
}

static String accessTokenSnapshot() {
    lockAuth();
    String token = s_accessToken;
    unlockAuth();
    return token;
}

static void loadTokens() {
    Preferences p;
    p.begin("yoto_auth", true);
    s_accessToken = p.getString("access", "");
    s_refreshToken = p.getString("refresh", "");
    // Stored expiry was based on millis(), which resets on every reboot.
    // Keep the refresh token, but force a fresh access token after boot.
    s_tokenExpiresAt = 0;
    s_deviceId = p.getString("device", "");
    p.end();
}

void yotoApiInit() {
    if (!s_authMtx) s_authMtx = xSemaphoreCreateMutex();
    loadTokens();
    Serial.printf("[yoto] tokens loaded, have_access=%d have_refresh=%d device=%s\n",
                  s_accessToken.length() > 0, s_refreshToken.length() > 0,
                  s_deviceId.length() ? s_deviceId.c_str() : "(none)");
}

static void clearTokensUnlocked() {
    s_accessToken = "";
    s_refreshToken = "";
    s_tokenExpiresAt = 0;
    s_deviceId = "";
    Preferences p;
    p.begin("yoto_auth", false);
    p.clear();
    p.end();
}

bool yotoAuthReady() {
    lockAuth();
    bool ready = s_refreshToken.length() > 0;
    unlockAuth();
    return ready;
}

void yotoClearTokens() {
    lockAuth();
    clearTokensUnlocked();
    unlockAuth();
}

// ---------- Device Code Flow ----------

DeviceCodeResult yotoStartDeviceFlow() {
    DeviceCodeResult res = {};
    res.ok = false;

    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.begin(sec, String(AUTH_BASE) + "/oauth/device/code");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "client_id=" + String(YOTO_CLIENT_ID) +
                  "&scope=" + urlEncode(String(SCOPES)) +
                  "&audience=" + urlEncode(String(YOTO_AUDIENCE));

    int code = http.POST(body);
    if (code != 200) {
        Serial.printf("[yoto] device code request failed: %d\n", code);
        http.end();
        return res;
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return res;

    res.deviceCode = doc["device_code"].as<String>();
    res.userCode = doc["user_code"].as<String>();
    res.verificationUri = doc["verification_uri"].as<String>();
    res.verificationUriComplete = doc["verification_uri_complete"].as<String>();
    res.interval = doc["interval"] | 5;
    res.expiresIn = doc["expires_in"] | 300;
    res.ok = true;

    Serial.printf("[yoto] device code: user_code=%s uri=%s\n",
                  res.userCode.c_str(), res.verificationUri.c_str());
    return res;
}

// URL-encode helper (local to this file)
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

String yotoPollDeviceFlow(const String &deviceCode) {
    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.begin(sec, String(AUTH_BASE) + "/oauth/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
                  "&device_code=" + deviceCode +
                  "&client_id=" + String(YOTO_CLIENT_ID);

    int code = http.POST(body);
    String resp = http.getString();
    http.end();

    if (code == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, resp) != DeserializationError::Ok) return "error";
        lockAuth();
        s_accessToken = doc["access_token"].as<String>();
        s_refreshToken = doc["refresh_token"] | s_refreshToken;
        int expiresIn = doc["expires_in"] | 3600;
        s_tokenExpiresAt = millis() + (unsigned long)expiresIn * 1000UL - 30000UL;
        saveTokens();
        unlockAuth();
        Serial.println("[yoto] authorized! tokens saved.");
        return "authorized";
    }

    if (code == 403) {
        JsonDocument doc;
        if (deserializeJson(doc, resp) == DeserializationError::Ok) {
            String err = doc["error"].as<String>();
            if (err == "authorization_pending" || err == "slow_down") {
                return "pending";
            }
            if (err == "expired_token") return "expired";
        }
    }

    Serial.printf("[yoto] poll failed: %d %s\n", code, resp.c_str());
    return "error";
}

// ---------- Token refresh ----------

static bool refreshTokenUnlocked() {
    if (s_refreshToken.length() == 0) return false;

    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.begin(sec, String(AUTH_BASE) + "/oauth/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "grant_type=refresh_token"
                  "&client_id=" + String(YOTO_CLIENT_ID) +
                  "&refresh_token=" + urlEncode(s_refreshToken);

    int code = http.POST(body);
    if (code != 200) {
        String resp = (code > 0) ? http.getString() : "";
        Serial.printf("[yoto] refresh failed: %d %s\n", code, resp.c_str());
        http.end();
        bool explicitlyInvalid = resp.indexOf("invalid_grant") >= 0 ||
                                 resp.indexOf("invalid_token") >= 0 ||
                                 resp.indexOf("expired") >= 0;
        if (explicitlyInvalid) {
            Serial.println("[yoto] refresh token rejected; clearing stored auth");
            clearTokensUnlocked();
        }
        return false;
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

    s_accessToken = doc["access_token"].as<String>();
    // Yoto rotates refresh tokens
    String newRefresh = doc["refresh_token"] | "";
    if (newRefresh.length()) s_refreshToken = newRefresh;
    int expiresIn = doc["expires_in"] | 3600;
    s_tokenExpiresAt = millis() + (unsigned long)expiresIn * 1000UL - 30000UL;
    saveTokens();
    Serial.println("[yoto] token refreshed");
    return true;
}

static bool forceRefreshToken() {
    lockAuth();
    bool ok = refreshTokenUnlocked();
    unlockAuth();
    return ok;
}

bool yotoEnsureToken() {
    lockAuth();
    bool ok = true;
    if (s_accessToken.length() == 0 || s_tokenExpiresAt == 0 || millis() >= s_tokenExpiresAt) {
        ok = refreshTokenUnlocked();
    }
    unlockAuth();
    return ok;
}

String yotoGetAccessToken() {
    return accessTokenSnapshot();
}

// ---------- API helpers ----------

static String apiGet(const String &path) {
    if (!yotoEnsureToken()) return "";

    Serial.printf("[yoto] apiGet %s heap=%u internal=%u\n", path.c_str(),
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.begin(sec, String(API_BASE) + path);
    http.addHeader("Authorization", "Bearer " + accessTokenSnapshot());
    http.setTimeout(15000);

    int code = http.GET();
    String body = (code > 0) ? http.getString() : "";
    http.end();

    if (code == 401 || code == 403) {
        // Token might be stale, try refresh once. Yoto sometimes reports an
        // expired/invalid bearer as 403 instead of 401.
        if (forceRefreshToken()) {
            http.begin(sec, String(API_BASE) + path);
            http.addHeader("Authorization", "Bearer " + accessTokenSnapshot());
            http.setTimeout(15000);
            code = http.GET();
            body = (code > 0) ? http.getString() : "";
            http.end();
        }
    }
    if (code != 200) {
        Serial.printf("[yoto] GET %s -> %d\n", path.c_str(), code);
        return "";
    }
    return body;
}

static bool apiPost(const String &path, const String &jsonBody = "{}") {
    if (!yotoEnsureToken()) return false;

    Serial.printf("[yoto] apiPost %s heap=%u\n", path.c_str(), (unsigned)ESP.getFreeHeap());
    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.begin(sec, String(API_BASE) + path);
    http.addHeader("Authorization", "Bearer " + accessTokenSnapshot());
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    int code = http.POST(jsonBody);
    http.end();

    if (code == 401 || code == 403) {
        if (forceRefreshToken()) {
            http.begin(sec, String(API_BASE) + path);
            http.addHeader("Authorization", "Bearer " + accessTokenSnapshot());
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(10000);
            code = http.POST(jsonBody);
            http.end();
        }
    }

    if (code < 200 || code >= 300) {
        Serial.printf("[yoto] POST %s -> %d\n", path.c_str(), code);
        return false;
    }
    return true;
}

// ---------- Devices ----------

bool yotoListDevices(std::vector<YotoDevice> &out) {
    String body = apiGet("/device-v2/devices/mine");
    if (!body.length()) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

    JsonArray devices = doc["devices"].as<JsonArray>();
    out.clear();
    for (JsonObject d : devices) {
        YotoDevice dev;
        dev.deviceId = d["deviceId"].as<String>();
        dev.name = d["name"] | "Player";
        dev.online = d["online"] | false;
        out.push_back(dev);
    }
    return true;
}

String yotoGetDeviceId() {
    lockAuth();
    String cachedDeviceId = s_deviceId;
    unlockAuth();
    if (cachedDeviceId.length()) return cachedDeviceId;
    std::vector<YotoDevice> devs;
    if (!yotoListDevices(devs) || devs.empty()) return "";
    String selectedDeviceId = devs[0].deviceId;
    lockAuth();
    s_deviceId = selectedDeviceId;
    saveTokens();
    unlockAuth();
    Serial.printf("[yoto] auto-selected device: %s (%s)\n",
                  selectedDeviceId.c_str(), devs[0].name.c_str());
    return selectedDeviceId;
}

// ---------- Library ----------

class PsramWriteStream : public Stream {
  public:
    ~PsramWriteStream() override {
        if (_buf) heap_caps_free(_buf);
    }

    bool reserve(size_t wanted) {
        if (wanted <= _cap) return true;
        size_t newCap = _cap ? _cap : 4096;
        while (newCap < wanted) newCap *= 2;
        uint8_t *next = (uint8_t *)heap_caps_realloc(
            _buf, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) {
            setWriteError();
            return false;
        }
        _buf = next;
        _cap = newCap;
        return true;
    }

    size_t write(uint8_t b) override {
        return write(&b, 1);
    }

    size_t write(const uint8_t *data, size_t len) override {
        if (!len) return 0;
        if (!reserve(_len + len + 1)) return 0;
        memcpy(_buf + _len, data, len);
        _len += len;
        _buf[_len] = 0;
        return len;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    void reset() {
        _len = 0;
        if (_buf) _buf[0] = 0;
        clearWriteError();
    }

    const char *data() const { return (const char *)_buf; }
    size_t size() const { return _len; }

  private:
    uint8_t *_buf = nullptr;
    size_t _len = 0;
    size_t _cap = 0;
};

static bool fetchLibraryPath(const char *path, PsramWriteStream &body) {
    body.reset();
    if (!yotoEnsureToken()) return false;

    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.begin(sec, String(API_BASE) + path);
    http.addHeader("Authorization", "Bearer " + accessTokenSnapshot());
    http.setTimeout(30000);

    int code = http.GET();
    if (code == 401 || code == 403) {
        Serial.printf("[yoto] library GET %s -> %d, refreshing token\n", path, code);
        http.end();
        if (!forceRefreshToken()) return false;
        http.begin(sec, String(API_BASE) + path);
        http.addHeader("Authorization", "Bearer " + accessTokenSnapshot());
        http.setTimeout(30000);
        code = http.GET();
    }
    if (code != 200) {
        Serial.printf("[yoto] library GET %s -> %d\n", path, code);
        http.end();
        return false;
    }

    int expected = http.getSize();
    if (expected > 0) body.reserve((size_t)expected + 1);
    int written = http.writeToStream(&body);
    http.end();
    if (written < 0 || body.size() == 0) {
        Serial.printf("[yoto] library read failed: %d size=%u\n",
                      written, (unsigned)body.size());
        return false;
    }
    Serial.printf("[yoto] library %s body: %u bytes\n", path, (unsigned)body.size());
    return true;
}

static String jsonString(JsonVariant v) {
    if (v.isNull()) return "";
    if (v.is<const char *>()) return String(v.as<const char *>());
    if (v.is<String>()) return v.as<String>();
    if (v.is<int>()) return String(v.as<int>());
    return "";
}

static String jsonStringConst(JsonVariantConst v) {
    if (v.isNull()) return "";
    if (v.is<const char *>()) return String(v.as<const char *>());
    if (v.is<String>()) return v.as<String>();
    if (v.is<int>()) return String(v.as<int>());
    return "";
}

static String fnv1aHex(const char *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
    return String(buf);
}

static String firstString(JsonObject a, JsonObject b, const char *k1, const char *k2 = nullptr, const char *k3 = nullptr) {
    const char *keys[3] = { k1, k2, k3 };
    for (int i = 0; i < 3; i++) {
        if (!keys[i]) continue;
        String v = jsonString(a[keys[i]]);
        if (v.length()) return v;
        v = jsonString(b[keys[i]]);
        if (v.length()) return v;
    }
    return "";
}

static int firstInt(JsonObject a, JsonObject b, const char *k1, const char *k2 = nullptr) {
    if (!a[k1].isNull()) return a[k1] | 0;
    if (!b[k1].isNull()) return b[k1] | 0;
    if (k2) {
        if (!a[k2].isNull()) return a[k2] | 0;
        if (!b[k2].isNull()) return b[k2] | 0;
    }
    return 0;
}

static bool keyIs(const char *key, const char *k1, const char *k2 = nullptr, const char *k3 = nullptr) {
    if (!key) return false;
    return (k1 && strcmp(key, k1) == 0) ||
           (k2 && strcmp(key, k2) == 0) ||
           (k3 && strcmp(key, k3) == 0);
}

static String findStringRecursive(JsonVariantConst v, const char *k1, const char *k2 = nullptr, const char *k3 = nullptr) {
    if (v.is<JsonObjectConst>()) {
        JsonObjectConst obj = v.as<JsonObjectConst>();
        for (JsonPairConst kv : obj) {
            if (keyIs(kv.key().c_str(), k1, k2, k3)) {
                String val = jsonStringConst(kv.value());
                if (val.length()) return val;
            }
        }
        for (JsonPairConst kv : obj) {
            String val = findStringRecursive(kv.value(), k1, k2, k3);
            if (val.length()) return val;
        }
    } else if (v.is<JsonArrayConst>()) {
        for (JsonVariantConst item : v.as<JsonArrayConst>()) {
            String val = findStringRecursive(item, k1, k2, k3);
            if (val.length()) return val;
        }
    }
    return "";
}

static int findIntRecursive(JsonVariantConst v, const char *k1, const char *k2 = nullptr, const char *k3 = nullptr) {
    if (v.is<JsonObjectConst>()) {
        JsonObjectConst obj = v.as<JsonObjectConst>();
        for (JsonPairConst kv : obj) {
            if (keyIs(kv.key().c_str(), k1, k2, k3) && kv.value().is<int>()) return kv.value().as<int>();
        }
        for (JsonPairConst kv : obj) {
            int val = findIntRecursive(kv.value(), k1, k2, k3);
            if (val > 0) return val;
        }
    } else if (v.is<JsonArrayConst>()) {
        for (JsonVariantConst item : v.as<JsonArrayConst>()) {
            int val = findIntRecursive(item, k1, k2, k3);
            if (val > 0) return val;
        }
    }
    return 0;
}

static JsonObject nestedObject(JsonObject outer, const char *name) {
    JsonObject obj = outer[name].as<JsonObject>();
    return obj;
}

static String coverUrlFrom(JsonObject card, JsonObject meta) {
    JsonObject cover = nestedObject(meta, "cover");
    String url = firstString(cover, card, "imageS", "imageL", "image");
    if (url.length()) return url;
    url = firstString(cover, card, "imageUrl", "coverUrl", "coverImage");
    if (url.length()) return url;
    JsonObject cardCover = nestedObject(card, "cover");
    url = firstString(cardCover, meta, "imageS", "imageL", "image");
    if (url.length()) return url;
    JsonObject content = nestedObject(card, "content");
    JsonObject contentCover = nestedObject(content, "cover");
    return firstString(contentCover, content, "imageS", "imageL", "image");
}

static JsonObject cardContentObject(JsonObject outer) {
    JsonObject card = outer["card"].as<JsonObject>();
    if (!card.isNull()) return card;
    JsonObject content = outer["content"].as<JsonObject>();
    if (!content.isNull()) return content;
    return outer;
}

static String languageFrom(JsonObject meta, JsonObject card) {
    String lang = firstString(meta, card, "language", "locale");
    if (lang.length()) return lang;
    JsonArray langs = meta["languages"].as<JsonArray>();
    if (!langs.isNull() && langs.size() > 0) return jsonString(langs[0]);
    return "";
}

static void parseSeries(JsonObject meta, YotoCard &yc) {
    const char *note = meta["note"] | "";
    String noteStr(note);
    int hashPos = noteStr.lastIndexOf('#');
    if (hashPos > 0) {
        yc.series = noteStr.substring(0, hashPos);
        yc.series.trim();
        yc.sequenceNumber = noteStr.substring(hashPos + 1).toInt();
    } else {
        yc.series = meta["series"] | "";
        yc.sequenceNumber = meta["sequence_number"] | -1;
    }
}

static bool spanContains(const char *start, size_t len, const String &needle) {
    if (!start || !needle.length() || len < needle.length()) return false;
    const char first = needle[0];
    const size_t n = needle.length();
    for (size_t i = 0; i + n <= len; i++) {
        if (start[i] == first && memcmp(start + i, needle.c_str(), n) == 0) return true;
    }
    return false;
}

static bool extractLibraryCardObject(const char *json, size_t len, const String &cardId, String &out) {
    if (!json || !len || !cardId.length()) return false;

    const char *cardsKey = strstr(json, "\"cards\"");
    if (!cardsKey) return false;
    const char *array = strchr(cardsKey, '[');
    if (!array) return false;

    bool inString = false;
    bool escape = false;
    int depth = 0;
    const char *objStart = nullptr;

    for (const char *p = array + 1; p < json + len; p++) {
        char c = *p;
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }

        if (c == '"') { inString = true; continue; }
        if (c == ']' && depth == 0) break;

        if (c == '{') {
            if (depth == 0) objStart = p;
            depth++;
        } else if (c == '}' && depth > 0) {
            depth--;
            if (depth == 0 && objStart) {
                size_t objLen = (size_t)(p - objStart + 1);
                if (spanContains(objStart, objLen, cardId)) {
                    out = "";
                    out.reserve(objLen + 1);
                    out.concat(objStart, objLen);
                    return true;
                }
                objStart = nullptr;
            }
        }
    }
    return false;
}

static String cardIdFromDetailJson(const String &cardJson) {
    JsonDocument filter;
    filter["cardId"] = true;
    filter["contentId"] = true;
    filter["id"] = true;
    filter["content"]["cardId"] = true;
    filter["content"]["contentId"] = true;
    filter["content"]["id"] = true;
    filter["card"]["cardId"] = true;
    filter["card"]["contentId"] = true;
    filter["card"]["id"] = true;
    filter["card"]["content"]["cardId"] = true;
    filter["card"]["content"]["contentId"] = true;
    filter["card"]["content"]["id"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, cardJson, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return "";
    JsonObject outer = doc.as<JsonObject>();
    JsonObject card = cardContentObject(outer);
    String id = firstString(outer, card, "cardId", "contentId", "id");
    if (id.length()) return id;
    JsonObject nestedCard = outer["card"].as<JsonObject>();
    if (!nestedCard.isNull()) {
        JsonObject nestedContent = nestedCard["content"].as<JsonObject>();
        id = firstString(nestedCard, nestedContent, "cardId", "contentId", "id");
    }
    return id;
}

static void cacheLibraryDetailSidecars(const char *json, size_t len) {
    if (!sdcache::ready() || !json || !len) return;

    const char *cardsKey = strstr(json, "\"cards\"");
    if (!cardsKey) return;
    const char *array = strchr(cardsKey, '[');
    if (!array) return;

    bool inString = false;
    bool escape = false;
    int depth = 0;
    const char *objStart = nullptr;
    unsigned saved = 0;

    for (const char *p = array + 1; p < json + len; p++) {
        char c = *p;
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }

        if (c == '"') { inString = true; continue; }
        if (c == ']' && depth == 0) break;

        if (c == '{') {
            if (depth == 0) objStart = p;
            depth++;
        } else if (c == '}' && depth > 0) {
            depth--;
            if (depth == 0 && objStart) {
                size_t objLen = (size_t)(p - objStart + 1);
                String cardJson;
                cardJson.reserve(objLen + 1);
                cardJson.concat(objStart, objLen);
                String id = cardIdFromDetailJson(cardJson);
                if (id.length() && sdcache::saveDetailJson(id, cardJson.c_str(), cardJson.length())) saved++;
                objStart = nullptr;
                vTaskDelay(1);
            }
        }
    }
    if (saved) Serial.printf("[yoto] cached %u card detail sidecars\n", saved);
}

static void fillDetailFromObjects(JsonObject card, JsonObject meta, JsonObject content, YotoCardDetail &out) {
    if (!meta.isNull()) {
        if (!out.description.length()) out.description = firstString(meta, card, "description", "shortDescription", "summary");
        if (!out.author.length()) out.author = firstString(meta, card, "author", "artist");
        if (!out.narrator.length()) out.narrator = firstString(meta, card, "narrator", "readBy");
        if (!out.genre.length()) out.genre = firstString(meta, card, "genre", "category");
        if (!out.language.length()) out.language = languageFrom(meta, card);
        if (out.duration <= 0) {
            JsonObject media = meta["media"].as<JsonObject>();
            out.duration = firstInt(meta, media, "duration", "durationSeconds");
        }
        if (!out.series.length()) {
            YotoCard tmp;
            tmp.sequenceNumber = -1;
            parseSeries(meta, tmp);
            out.series = tmp.series;
            out.sequenceNumber = tmp.sequenceNumber;
        }
    }

    if (!content.isNull()) {
        JsonObject contentMeta = content["metadata"].as<JsonObject>();
        fillDetailFromObjects(content, contentMeta, JsonObject(), out);
    }
}

static bool parseCardDetailJson(const String &cardJson, YotoCardDetail &out);

static bool yotoFetchFamilyLibraryCardDetail(const String &cardId, YotoCardDetail &out) {
    String cachedJson;
    if (sdcache::loadDetailJson(cardId, cachedJson)) {
        Serial.printf("[yoto] detail %s from SD sidecar (%u bytes)\n", cardId.c_str(), (unsigned)cachedJson.length());
        return parseCardDetailJson(cachedJson, out);
    }

    PsramWriteStream body;
    if (!fetchLibraryPath("/card/family/library", body)) return false;

    String cardJson;
    if (!extractLibraryCardObject(body.data(), body.size(), cardId, cardJson)) {
        Serial.printf("[yoto] family detail card not found: %s\n", cardId.c_str());
        return false;
    }

    sdcache::saveDetailJson(cardId, cardJson.c_str(), cardJson.length());

    return parseCardDetailJson(cardJson, out);
}

static bool parseCardDetailJson(const String &cardJson, YotoCardDetail &out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, cardJson);
    if (err != DeserializationError::Ok) {
        Serial.printf("[yoto] detail parse error: %s\n", err.c_str());
        return false;
    }

    JsonObject outer = doc.as<JsonObject>();
    JsonObject card = cardContentObject(outer);
    fillDetailFromObjects(card, card["metadata"].as<JsonObject>(), card["content"].as<JsonObject>(), out);
    fillDetailFromObjects(outer, outer["metadata"].as<JsonObject>(), outer["content"].as<JsonObject>(), out);

    JsonObject nestedCard = outer["card"].as<JsonObject>();
    if (!nestedCard.isNull()) {
        fillDetailFromObjects(nestedCard, nestedCard["metadata"].as<JsonObject>(), nestedCard["content"].as<JsonObject>(), out);
    }

    JsonVariantConst root = doc.as<JsonVariantConst>();
    if (!out.description.length()) out.description = findStringRecursive(root, "description", "shortDescription", "summary");
    if (!out.author.length()) out.author = findStringRecursive(root, "author", "artist");
    if (!out.narrator.length()) out.narrator = findStringRecursive(root, "narrator", "readBy");
    if (!out.genre.length()) out.genre = findStringRecursive(root, "genre", "category");
    if (!out.language.length()) out.language = findStringRecursive(root, "language", "locale");
    if (!out.series.length()) out.series = findStringRecursive(root, "series");
    if (out.sequenceNumber <= 0) out.sequenceNumber = findIntRecursive(root, "sequence_number", "sequenceNumber");
    if (out.duration <= 0) out.duration = findIntRecursive(root, "duration", "durationSeconds");

    Serial.printf("[yoto] detail parsed desc=%u author=%u series=%u duration=%d\n",
        (unsigned)out.description.length(), (unsigned)out.author.length(),
        (unsigned)out.series.length(), out.duration);
    return out.description.length() || out.author.length() || out.genre.length() || out.series.length() || out.duration > 0;
}

static void resetCardDetail(YotoCardDetail &out) {
    out.description = "";
    out.chapterCount = 0;
    out.trackCount = 0;
    out.narrator = "";
    out.genre = "";
    out.author = "";
    out.series = "";
    out.language = "";
    out.sequenceNumber = -1;
    out.duration = 0;
}

static bool parseLibraryCard(JsonObject outer, YotoCard &yc) {
    JsonObject card = cardContentObject(outer);
    JsonObject meta = card["metadata"].as<JsonObject>();
    JsonObject media = meta["media"].as<JsonObject>();

    yc.shareType = outer["shareType"] | "";
    yc.cardId = firstString(outer, card, "cardId", "contentId", "id");
    if (!yc.cardId.length()) return false;

    yc.title = firstString(outer, card, "title", "name");
    if (!yc.title.length()) yc.title = meta["title"] | "Untitled";
    yc.author = firstString(meta, card, "author", "artist");
    yc.coverUrl = coverUrlFrom(card, meta);
    yc.description = "";
    yc.category = "";
    yc.language = "";
    yc.duration = firstInt(meta, media, "duration", "durationSeconds");
    yc.sequenceNumber = -1;
    parseSeries(meta, yc);
    return true;
}

bool yotoFetchLibrary(std::vector<YotoCard> &out) {
    if (!yotoEnsureToken()) return false;

    Serial.printf("[yoto] fetch library, heap=%u internal=%u psram=%u\n",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)ESP.getFreePsram());

    PsramWriteStream body;
    const char *source = "/card/family/library";
    if (!fetchLibraryPath(source, body)) {
        source = "/content/mine?showdeleted=false";
        if (!fetchLibraryPath(source, body)) return false;
    }

    if (strcmp(source, "/card/family/library") == 0) {
        String nextSig = fnv1aHex(body.data(), body.size());
        String prevSig;
        if (sdcache::loadDetailsSignature(prevSig) && prevSig == nextSig) {
            Serial.printf("[yoto] detail sidecars unchanged (%s)\n", nextSig.c_str());
        } else {
            cacheLibraryDetailSidecars(body.data(), body.size());
            sdcache::saveDetailsSignature(nextSig);
        }
    }

    // Filter to keep only necessary fields
    JsonDocument filter;
    filter["cards"][0]["cardId"] = true;
    filter["cards"][0]["contentId"] = true;
    filter["cards"][0]["id"] = true;
    filter["cards"][0]["shareType"] = true;
    filter["cards"][0]["title"] = true;
    filter["cards"][0]["name"] = true;
    filter["cards"][0]["metadata"]["author"] = true;
    filter["cards"][0]["metadata"]["cover"]["imageS"] = true;
    filter["cards"][0]["metadata"]["cover"]["imageL"] = true;
    filter["cards"][0]["metadata"]["genre"] = true;
    filter["cards"][0]["metadata"]["media"]["duration"] = true;
    filter["cards"][0]["metadata"]["note"] = true;
    filter["cards"][0]["metadata"]["series"] = true;
    filter["cards"][0]["metadata"]["sequence_number"] = true;
    filter["cards"][0]["metadata"]["duration"] = true;
    filter["cards"][0]["content"]["cardId"] = true;
    filter["cards"][0]["content"]["contentId"] = true;
    filter["cards"][0]["content"]["id"] = true;
    filter["cards"][0]["content"]["title"] = true;
    filter["cards"][0]["content"]["name"] = true;
    filter["cards"][0]["content"]["author"] = true;
    filter["cards"][0]["content"]["cover"]["imageS"] = true;
    filter["cards"][0]["content"]["cover"]["imageL"] = true;
    filter["cards"][0]["content"]["playbackType"] = true;
    filter["cards"][0]["content"]["metadata"]["author"] = true;
    filter["cards"][0]["content"]["metadata"]["cover"]["imageS"] = true;
    filter["cards"][0]["content"]["metadata"]["cover"]["imageL"] = true;
    filter["cards"][0]["content"]["metadata"]["genre"] = true;
    filter["cards"][0]["content"]["metadata"]["media"]["duration"] = true;
    filter["cards"][0]["content"]["metadata"]["note"] = true;
    filter["cards"][0]["content"]["metadata"]["series"] = true;
    filter["cards"][0]["content"]["metadata"]["sequence_number"] = true;
    filter["cards"][0]["content"]["metadata"]["duration"] = true;
    filter["cards"][0]["card"]["cardId"] = true;
    filter["cards"][0]["card"]["contentId"] = true;
    filter["cards"][0]["card"]["id"] = true;
    filter["cards"][0]["card"]["title"] = true;
    filter["cards"][0]["card"]["name"] = true;
    filter["cards"][0]["card"]["author"] = true;
    filter["cards"][0]["card"]["cover"]["imageS"] = true;
    filter["cards"][0]["card"]["cover"]["imageL"] = true;
    filter["cards"][0]["card"]["content"]["cover"]["imageS"] = true;
    filter["cards"][0]["card"]["content"]["cover"]["imageL"] = true;
    filter["cards"][0]["card"]["content"]["playbackType"] = true;
    filter["cards"][0]["card"]["metadata"]["author"] = true;
    filter["cards"][0]["card"]["metadata"]["cover"]["imageS"] = true;
    filter["cards"][0]["card"]["metadata"]["cover"]["imageL"] = true;
    filter["cards"][0]["card"]["metadata"]["genre"] = true;
    filter["cards"][0]["card"]["metadata"]["media"]["duration"] = true;
    filter["cards"][0]["card"]["metadata"]["note"] = true;
    filter["cards"][0]["card"]["metadata"]["series"] = true;
    filter["cards"][0]["card"]["metadata"]["sequence_number"] = true;
    filter["cards"][0]["card"]["metadata"]["duration"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, body.data(), body.size(), DeserializationOption::Filter(filter));

    if (err != DeserializationError::Ok) {
        Serial.printf("[yoto] library parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray cards = doc["cards"].as<JsonArray>();
    out.clear();
    for (JsonObject c : cards) {
        YotoCard yc;
        if (!parseLibraryCard(c, yc)) continue;
        out.push_back(yc);
    }

    std::sort(out.begin(), out.end(), [](const YotoCard &a, const YotoCard &b) {
        return a.title < b.title;
    });

    Serial.printf("[yoto] library %s: %u cards\n", source, (unsigned)out.size());
    return true;
}

// ---------- Card detail ----------

bool yotoFetchCardDetail(const String &cardId, YotoCardDetail &out) {
    resetCardDetail(out);

    String cachedJson;
    if (sdcache::loadDetailJson(cardId, cachedJson)) {
        Serial.printf("[yoto] detail %s from SD sidecar (%u bytes)\n", cardId.c_str(), (unsigned)cachedJson.length());
        return parseCardDetailJson(cachedJson, out);
    }

    String body = apiGet("/content/" + cardId);
    if (!body.length()) return yotoFetchFamilyLibraryCardDetail(cardId, out);

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        return yotoFetchFamilyLibraryCardDetail(cardId, out);
    }

    // API sometimes wraps as {"card": {...}}
    JsonObject card = doc["card"].as<JsonObject>();
    if (card.isNull()) card = doc.as<JsonObject>();

    JsonObject meta = card["metadata"].as<JsonObject>();
    JsonObject content = card["content"].as<JsonObject>();

    if (!meta.isNull()) {
        out.description = meta["description"] | "";
        out.narrator = meta["narrator"] | (meta["readBy"] | "");
        out.genre = meta["genre"] | "";
        out.author = meta["author"] | "";
        out.series = meta["series"] | "";
        out.language = meta["language"] | "";
        out.sequenceNumber = meta["sequence_number"] | -1;
        out.duration = meta["duration"] | 0;

        // Series from metadata.note fallback (same as library fetch)
        if (out.series.length() == 0) {
            const char *note = meta["note"] | "";
            String noteStr(note);
            int hashPos = noteStr.lastIndexOf('#');
            if (hashPos > 0) {
                out.series = noteStr.substring(0, hashPos);
                out.series.trim();
                out.sequenceNumber = noteStr.substring(hashPos + 1).toInt();
            }
        }
    }

    if (!content.isNull()) {
        JsonArray chapters = content["chapters"].as<JsonArray>();
        out.chapterCount = chapters.size();
        for (JsonObject ch : chapters) {
            JsonArray tracks = ch["tracks"].as<JsonArray>();
            out.trackCount += tracks.size();
        }
    }

    if (!out.description.length()) {
        yotoFetchFamilyLibraryCardDetail(cardId, out);
    }
    return true;
}

// ---------- Player control ----------

bool yotoPlayCard(const String &cardId) {
    String did = yotoGetDeviceId();
    if (!did.length()) return false;
    String payload = "{\"uri\":\"https://yoto.io/" + cardId + "\"}";
    return apiPost("/device-v2/" + did + "/command/card/start", payload);
}

bool yotoPause() {
    String did = yotoGetDeviceId();
    if (!did.length()) return false;
    return apiPost("/device-v2/" + did + "/command/card/pause");
}

bool yotoResume() {
    String did = yotoGetDeviceId();
    if (!did.length()) return false;
    return apiPost("/device-v2/" + did + "/command/card/resume");
}

bool yotoStop() {
    String did = yotoGetDeviceId();
    if (!did.length()) return false;
    return apiPost("/device-v2/" + did + "/command/card/stop");
}

bool yotoSetVolume(int vol) {
    String did = yotoGetDeviceId();
    if (!did.length()) return false;
    vol = max(0, min(100, vol));
    String payload = "{\"volume\":" + String(vol) + "}";
    return apiPost("/device-v2/" + did + "/command/volume/set", payload);
}

// ---------- Cover download + PNG decode ----------

// Dedicated buffers for net task to avoid heap thrashing/overlap with cover cache.
static uint8_t *s_rawBuf = nullptr;
static const size_t kRawBufSize = 256 * 1024; // exact 144px CDN thumbnails are typically < 25 KB

static void ensureNetBuffers() {
    if (!s_rawBuf) s_rawBuf = (uint8_t *)heap_caps_malloc(kRawBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

struct PngleCoverDecode {
    uint8_t *rgb565 = nullptr;
    int thumbSize = 144;
    uint32_t pngW = 0;
    uint32_t pngH = 0;
    int outW = 0;
    int outH = 0;
    int offX = 0;
    int offY = 0;
    uint8_t alphaMin = 255;
    uint8_t alphaMax = 0;
    uint32_t transparentPixels = 0;
    bool initialized = false;
    bool done = false;
};

static void pngleCoverInit(pngle_t *pngle, uint32_t w, uint32_t h) {
    PngleCoverDecode *ctx = (PngleCoverDecode *)pngle_get_user_data(pngle);
    if (!ctx) return;

    ctx->pngW = w;
    ctx->pngH = h;
    const int sz = ctx->thumbSize;
    ctx->outW = sz;
    ctx->outH = sz;
    if (w == 0 || h == 0) {
        ctx->outW = 0;
        ctx->outH = 0;
        return;
    }

    if ((uint64_t)w * sz > (uint64_t)h * sz) {
        ctx->outH = max(1, (int)((uint64_t)h * sz / w));
    } else {
        ctx->outW = max(1, (int)((uint64_t)w * sz / h));
    }
    ctx->offX = (sz - ctx->outW) / 2;
    ctx->offY = (sz - ctx->outH) / 2;
    memset(ctx->rgb565, 0xFF, (size_t)sz * sz * 2);
    ctx->initialized = true;
    if (ctx->outW != sz || ctx->outH != sz) {
        Serial.printf("[yoto] cover letterbox %ux%u -> %dx%d at %d,%d\n",
            (unsigned)w, (unsigned)h, ctx->outW, ctx->outH, ctx->offX, ctx->offY);
    }
}

static void pngleCoverDraw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    PngleCoverDecode *ctx = (PngleCoverDecode *)pngle_get_user_data(pngle);
    if (!ctx || !ctx->initialized || !ctx->rgb565 || ctx->pngW == 0 || ctx->pngH == 0) return;

    uint8_t r = rgba[0];
    uint8_t g = rgba[1];
    uint8_t b = rgba[2];
    uint8_t a = rgba[3];
    if (a < ctx->alphaMin) ctx->alphaMin = a;
    if (a > ctx->alphaMax) ctx->alphaMax = a;
    if (a < 255) ctx->transparentPixels += w * h;
    if (a < 255) {
        r = (uint8_t)((r * a + 255 * (255 - a)) / 255);
        g = (uint8_t)((g * a + 255 * (255 - a)) / 255);
        b = (uint8_t)((b * a + 255 * (255 - a)) / 255);
    }
    const uint16_t rgb565Pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    int dx0 = ctx->offX + (int)((uint64_t)x * ctx->outW / ctx->pngW);
    int dy0 = ctx->offY + (int)((uint64_t)y * ctx->outH / ctx->pngH);
    int dx1 = ctx->offX + (int)(((uint64_t)(x + w) * ctx->outW + ctx->pngW - 1) / ctx->pngW);
    int dy1 = ctx->offY + (int)(((uint64_t)(y + h) * ctx->outH + ctx->pngH - 1) / ctx->pngH);

    const int sz = ctx->thumbSize;
    dx0 = max(0, min(sz, dx0));
    dy0 = max(0, min(sz, dy0));
    dx1 = max(0, min(sz, dx1));
    dy1 = max(0, min(sz, dy1));
    if (dx1 <= dx0 || dy1 <= dy0) return;

    for (int yy = dy0; yy < dy1; yy++) {
        uint8_t *row = ctx->rgb565 + ((size_t)yy * sz + dx0) * 2;
        for (int xx = dx0; xx < dx1; xx++) {
            row[0] = rgb565Pixel & 0xFF;
            row[1] = (rgb565Pixel >> 8) & 0xFF;
            row += 2;
        }
    }
}

static void pngleCoverDone(pngle_t *pngle) {
    PngleCoverDecode *ctx = (PngleCoverDecode *)pngle_get_user_data(pngle);
    if (ctx) ctx->done = true;
}

static String appendImageResizeParams(const String &url, int width, int quality) {
    String base = url;
    String kept;
    int q = base.indexOf('?');
    if (q >= 0) {
        kept = base.substring(q + 1);
        base = base.substring(0, q);
    }

    String out = base;
    bool hasParams = false;
    int start = 0;
    while (start < kept.length()) {
        int amp = kept.indexOf('&', start);
        if (amp < 0) amp = kept.length();
        String param = kept.substring(start, amp);
        int eq = param.indexOf('=');
        String key = eq >= 0 ? param.substring(0, eq) : param;
        if (key != "width" && key != "height" && key != "quality") {
            out += hasParams ? '&' : '?';
            out += param;
            hasParams = true;
        }
        start = amp + 1;
    }

    out += hasParams ? '&' : '?';
    out += "width=" + String(width) + "&quality=" + String(quality);
    return out;
}

bool yotoDownloadCover(const String &url, uint8_t **outRgb565, int thumbSize) {
    if (outRgb565) *outRgb565 = nullptr;
    if (!url.length() || !outRgb565) return false;
    ensureNetBuffers();
    if (!s_rawBuf) return false;

    // Ask the CDN to scale by width only so tall book covers aren't cropped.
    const int sz = thumbSize > 0 ? thumbSize : 144;
    const int quality = sz > 144 ? 85 : 70;
    String resizedUrl = appendImageResizeParams(url, sz, quality);

    Serial.printf("[yoto] fetch cover: %s\n", resizedUrl.c_str());

    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(sec, resizedUrl);
    http.addHeader("Accept", "image/png,image/jpeg,*/*");
    http.setTimeout(15000);
    const char *hdrs[] = {"Content-Type", "Content-Length", "Server"};
    http.collectHeaders(hdrs, 3);
    int code = http.GET();
    if (code != 200) { Serial.printf("[yoto] cover HTTP %d\n", code); http.end(); return false; }

    int len = http.getSize();
    String ct = http.header("Content-Type");
    Serial.printf("[yoto] cover %d bytes ct=%s\n", len, ct.c_str());
    if (len <= 0 || len > (int)kRawBufSize) { http.end(); return false; }

    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    unsigned long t0 = millis();
    while (got < (size_t)len && (millis() - t0) < 20000) {
        int avail = stream->available();
        if (avail <= 0) { if (!stream->connected()) break; delay(10); continue; }
        int toRead = min(avail, (int)(len - got));
        int n = stream->read(s_rawBuf + got, toRead);
        if (n <= 0) break;
        got += n;
        delay(1);
    }
    http.end();

    if ((int)got != len) {
        Serial.printf("[yoto] cover short read: %u/%d\n", (unsigned)got, len);
        return false;
    }

    if (got >= 4 && s_rawBuf[0] == 0x89 && s_rawBuf[1] == 'P' && s_rawBuf[2] == 'N' && s_rawBuf[3] == 'G') {
        uint8_t *rgb565 = (uint8_t *)heap_caps_malloc((size_t)sz * sz * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb565) {
            Serial.println("[yoto] RGB565 alloc failed");
            return false;
        }

        pngle_t *pngle = pngle_new();
        if (!pngle) {
            Serial.println("[yoto] pngle alloc failed");
            heap_caps_free(rgb565);
            return false;
        }

        PngleCoverDecode ctx;
        ctx.rgb565 = rgb565;
        ctx.thumbSize = sz;
        pngle_set_user_data(pngle, &ctx);
        pngle_set_init_callback(pngle, pngleCoverInit);
        pngle_set_draw_callback(pngle, pngleCoverDraw);
        pngle_set_done_callback(pngle, pngleCoverDone);

        uint8_t feedBuf[1024];
        size_t remain = 0;
        size_t consumed = 0;
        while (consumed < got) {
            if (remain >= sizeof(feedBuf)) {
                Serial.println("[yoto] pngle feed buffer exceeded");
                pngle_destroy(pngle);
                heap_caps_free(rgb565);
                return false;
            }
            size_t toCopy = min(sizeof(feedBuf) - remain, got - consumed);
            memcpy(feedBuf + remain, s_rawBuf + consumed, toCopy);
            consumed += toCopy;

            int fed = pngle_feed(pngle, feedBuf, remain + toCopy);
            if (fed < 0) {
                Serial.printf("[yoto] pngle decode failed: %s\n", pngle_error(pngle));
                pngle_destroy(pngle);
                heap_caps_free(rgb565);
                return false;
            }
            remain = remain + toCopy - (size_t)fed;
            if (remain > 0) {
                if (remain >= sizeof(feedBuf)) {
                    Serial.println("[yoto] pngle feed buffer exceeded");
                    pngle_destroy(pngle);
                    heap_caps_free(rgb565);
                    return false;
                }
                memmove(feedBuf, feedBuf + fed, remain);
            }
            delay(1);
        }

        if (!ctx.initialized || !ctx.done) {
            Serial.printf("[yoto] pngle incomplete init=%d done=%d remain=%u\n",
                ctx.initialized, ctx.done, (unsigned)remain);
            pngle_destroy(pngle);
            heap_caps_free(rgb565);
            return false;
        }

        Serial.printf("[yoto] PNG %ux%u decoded OK with pngle to RGB565 %p\n",
            (unsigned)ctx.pngW, (unsigned)ctx.pngH, rgb565);
        if (ctx.transparentPixels) {
            Serial.printf("[yoto] PNG alpha min=%u max=%u transparent=%u/%u\n",
                ctx.alphaMin, ctx.alphaMax, (unsigned)ctx.transparentPixels,
                (unsigned)(ctx.pngW * ctx.pngH));
        }
        pngle_destroy(pngle);
        *outRgb565 = rgb565;
        return true;
    }

    Serial.printf("[yoto] cover: unknown format (0x%02X 0x%02X)\n", s_rawBuf[0], s_rawBuf[1]);
    return false;
}
