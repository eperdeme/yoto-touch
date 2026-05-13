#include "sd_cache.h"

#include <SPI.h>
#include <SdFat.h>
#include "pins.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sdcache {

// SdFat with ExFAT + FAT32 support. Uses HSPI; DMA is fine but the default
// non-DMA mode is plenty for our 28.8 KB cover reads.
static SPIClass sdSpi(HSPI);
static SdFs sd;
static bool s_ready = false;
static SemaphoreHandle_t s_sdMtx = nullptr;

// RAII lock for SD access (thread-safe across cores)
struct SdLock {
    SdLock()  { if (s_sdMtx) xSemaphoreTake(s_sdMtx, portMAX_DELAY); }
    ~SdLock() { if (s_sdMtx) xSemaphoreGive(s_sdMtx); }
};

static const char *kRoot         = "/yoto";
static const char *kCoversDir    = "/yoto/covers";
static const char *kDetailsDir   = "/yoto/details";
static const char *kDetailsSigPath = "/yoto/details.sig";
static const char *kManifestPath = "/yoto/manifest.bin";

static bool ensureDir(const char *p) {
    if (sd.exists(p)) return true;
    return sd.mkdir(p);
}

// ESP32 SPI DMA can't reliably touch PSRAM, so we always stage through an
// internal-DRAM bounce buffer for cover reads/writes. 4 KB on the stack is fine.
static const size_t kBounceBytes = 4096;

static int readBounced(FsFile &f, uint8_t *dst, size_t n) {
    uint8_t bounce[kBounceBytes];
    size_t total = 0;
    while (total < n) {
        size_t chunk = min(kBounceBytes, n - total);
        int got = f.read(bounce, chunk);
        if (got <= 0) return total ? (int)total : got;
        memcpy(dst + total, bounce, got);
        total += got;
        if ((size_t)got < chunk) break;
    }
    return (int)total;
}

static size_t writeBounced(FsFile &f, const uint8_t *src, size_t n) {
    uint8_t bounce[kBounceBytes];
    size_t total = 0;
    while (total < n) {
        size_t chunk = min(kBounceBytes, n - total);
        memcpy(bounce, src + total, chunk);
        size_t wrote = f.write(bounce, chunk);
        total += wrote;
        if (wrote < chunk) break;
    }
    return total;
}

bool begin() {
    if (s_ready) return true;
    if (!s_sdMtx) s_sdMtx = xSemaphoreCreateMutex();
    sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    SdSpiConfig cfg(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &sdSpi);
    if (!sd.begin(cfg)) {
        Serial.println("[sd] mount FAILED");
        s_ready = false;
        return false;
    }
    uint32_t sizeMB = (uint32_t)((uint64_t)sd.card()->sectorCount() >> 11);
    uint8_t fatType = sd.fatType();   // 32 = FAT32, 64 = ExFAT
    Serial.printf("[sd] mounted fatType=%u %u MB\n", fatType, (unsigned)sizeMB);
    ensureDir(kRoot);
    ensureDir(kCoversDir);
    ensureDir(kDetailsDir);
    s_ready = true;
    return true;
}

bool ready() { return s_ready; }

static String coverPath(const String &id) {
    return String(kCoversDir) + "/" + id + ".565";
}

bool hasCover(const String &id) {
    if (!s_ready) return false;
    SdLock lk;
    return sd.exists(coverPath(id).c_str());
}

bool loadCover(const String &id, uint8_t *buf, size_t buf_bytes) {
    if (!s_ready) return false;
    SdLock lk;
    FsFile f = sd.open(coverPath(id).c_str(), O_RDONLY);
    if (!f) return false;
    if ((size_t)f.fileSize() != buf_bytes) { f.close(); return false; }
    int got = readBounced(f, buf, buf_bytes);
    f.close();
    return got == (int)buf_bytes;
}

bool saveCover(const String &id, const uint8_t *buf, size_t buf_bytes) {
    if (!s_ready) return false;
    SdLock lk;
    String tmp = coverPath(id) + ".tmp";
    sd.remove(tmp.c_str());
    FsFile f = sd.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    size_t wrote = writeBounced(f, buf, buf_bytes);
    f.close();
    if (wrote != buf_bytes) { sd.remove(tmp.c_str()); return false; }
    sd.remove(coverPath(id).c_str());
    return sd.rename(tmp.c_str(), coverPath(id).c_str());
}

static String detailPath(const String &id) {
    return String(kDetailsDir) + "/" + id + ".json";
}

bool loadDetailJson(const String &id, String &outJson) {
    outJson = "";
    if (!s_ready || !id.length()) return false;
    SdLock lk;
    FsFile f = sd.open(detailPath(id).c_str(), O_RDONLY);
    if (!f) return false;
    size_t size = (size_t)f.fileSize();
    if (size == 0 || size > 64 * 1024) { f.close(); return false; }
    outJson.reserve(size + 1);
    uint8_t bounce[kBounceBytes];
    size_t total = 0;
    while (total < size) {
        size_t chunk = min(kBounceBytes, size - total);
        int got = f.read(bounce, chunk);
        if (got <= 0) break;
        outJson.concat((const char *)bounce, got);
        total += got;
        if ((size_t)got < chunk) break;
    }
    f.close();
    return total == size;
}

bool saveDetailJson(const String &id, const char *json, size_t len) {
    if (!s_ready || !id.length() || !json || !len || len > 64 * 1024) return false;
    SdLock lk;
    ensureDir(kDetailsDir);
    String tmp = detailPath(id) + ".tmp";
    sd.remove(tmp.c_str());
    FsFile f = sd.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    size_t wrote = writeBounced(f, (const uint8_t *)json, len);
    f.close();
    if (wrote != len) { sd.remove(tmp.c_str()); return false; }
    sd.remove(detailPath(id).c_str());
    return sd.rename(tmp.c_str(), detailPath(id).c_str());
}

bool loadDetailsSignature(String &outSignature) {
    outSignature = "";
    if (!s_ready) return false;
    SdLock lk;
    FsFile f = sd.open(kDetailsSigPath, O_RDONLY);
    if (!f) return false;
    char buf[65];
    int got = f.read((uint8_t *)buf, sizeof(buf) - 1);
    f.close();
    if (got <= 0) return false;
    buf[got] = 0;
    outSignature = String(buf);
    outSignature.trim();
    return outSignature.length() > 0;
}

bool saveDetailsSignature(const String &signature) {
    if (!s_ready || !signature.length()) return false;
    SdLock lk;
    String tmp = String(kDetailsSigPath) + ".tmp";
    sd.remove(tmp.c_str());
    FsFile f = sd.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    size_t wrote = f.write((const uint8_t *)signature.c_str(), signature.length());
    f.write((const uint8_t *)"\n", 1);
    f.close();
    if (wrote != signature.length()) { sd.remove(tmp.c_str()); return false; }
    sd.remove(kDetailsSigPath);
    return sd.rename(tmp.c_str(), kDetailsSigPath);
}

// ----- manifest -----

// Helper to write a length-prefixed string (max 255 bytes)
static bool writeStr(FsFile &f, const String &s) {
    uint8_t len = (uint8_t)min((size_t)255, s.length());
    f.write(&len, 1);
    if (len) f.write((const uint8_t *)s.c_str(), len);
    return true;
}

// Helper to read a length-prefixed string
static bool readStr(FsFile &f, String &out) {
    uint8_t len = 0;
    if (f.read(&len, 1) != 1) return false;
    char buf[256];
    if (len > 0 && f.read((uint8_t *)buf, len) != (int)len) return false;
    buf[len] = 0;
    out = String(buf);
    return true;
}

static bool skipStr(FsFile &f) {
    uint8_t len = 0;
    if (f.read(&len, 1) != 1) return false;
    uint8_t buf[64];
    uint8_t left = len;
    while (left) {
        uint8_t chunk = min((uint8_t)sizeof(buf), left);
        if (f.read(buf, chunk) != chunk) return false;
        left -= chunk;
    }
    return true;
}

bool saveManifest(const String &sig, const std::vector<CardMeta> &cards) {
    if (!s_ready) return false;
    SdLock lk;
    if (sig.length() > 255) return false;
    FsFile f = sd.open(kManifestPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    // Version byte (5 = lightweight full library: no long descriptions in RAM)
    uint8_t version = 5;
    f.write(&version, 1);
    writeStr(f, sig);
    uint16_t n = (uint16_t)min((size_t)0xFFFF, cards.size());
    uint8_t nb[2] = { (uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
    f.write(nb, 2);
    for (uint16_t i = 0; i < n; i++) {
        const CardMeta &c = cards[i];
        writeStr(f, c.id);
        writeStr(f, c.title);
        writeStr(f, c.author);
        writeStr(f, c.coverUrl);
        writeStr(f, c.shareType);
        writeStr(f, c.series);
        int16_t seq = (int16_t)c.sequenceNumber;
        uint8_t sb[2] = { (uint8_t)(seq & 0xFF), (uint8_t)((uint16_t)seq >> 8) };
        f.write(sb, 2);
        uint32_t dur = (uint32_t)max(0, c.duration);
        uint8_t db[4] = { (uint8_t)(dur & 0xFF), (uint8_t)(dur >> 8), (uint8_t)(dur >> 16), (uint8_t)(dur >> 24) };
        f.write(db, 4);
    }
    f.close();
    return true;
}

bool loadManifest(String &outSig, std::vector<CardMeta> &out) {
    SdLock lk;
    if (!s_ready) return false;
    FsFile f = sd.open(kManifestPath, O_RDONLY);
    if (!f) return false;

    // Read version byte
    uint8_t version = 0;
    if (f.read(&version, 1) != 1) { f.close(); return false; }

    if (version == 2 || version == 3 || version == 4 || version == 5) {
        // New format with author/coverUrl/series
        if (!readStr(f, outSig)) { f.close(); return false; }
        uint8_t nb[2];
        if (f.read(nb, 2) != 2) { f.close(); return false; }
        uint16_t n = nb[0] | (nb[1] << 8);
        out.clear();
        out.reserve(n);
        for (uint16_t i = 0; i < n; i++) {
            CardMeta m;
            if (!readStr(f, m.id) || !readStr(f, m.title) ||
                !readStr(f, m.author) || !readStr(f, m.coverUrl)) {
                f.close(); return false;
            }
            if (version == 4) {
                if (!skipStr(f) || !skipStr(f) || !skipStr(f) || !readStr(f, m.shareType)) {
                    f.close(); return false;
                }
            } else if (version >= 5) {
                if (!readStr(f, m.shareType)) { f.close(); return false; }
            }
            if (!readStr(f, m.series)) { f.close(); return false; }
            if (version >= 3) {
                uint8_t sb[2];
                if (f.read(sb, 2) != 2) { f.close(); return false; }
                m.sequenceNumber = (int16_t)(sb[0] | (sb[1] << 8));
            }
            if (version >= 4) {
                uint8_t db[4];
                if (f.read(db, 4) != 4) { f.close(); return false; }
                m.duration = (int)((uint32_t)db[0] | ((uint32_t)db[1] << 8) |
                                   ((uint32_t)db[2] << 16) | ((uint32_t)db[3] << 24));
            }
            if (m.id.length()) out.push_back(m);
        }
    } else {
        // Legacy format (version byte is actually sig_len)
        uint8_t sl = version;  // first byte was sig length, not version
        char sigBuf[256];
        if (f.read((uint8_t *)sigBuf, sl) != (int)sl) { f.close(); return false; }
        sigBuf[sl] = 0;
        outSig = String(sigBuf);
        uint8_t nb[2];
        if (f.read(nb, 2) != 2) { f.close(); return false; }
        uint16_t n = nb[0] | (nb[1] << 8);
        out.clear();
        out.reserve(n);
        for (uint16_t i = 0; i < n; i++) {
            CardMeta m;
            if (!readStr(f, m.id) || !readStr(f, m.title)) {
                f.close(); return false;
            }
            if (m.id.length()) out.push_back(m);
        }
    }
    f.close();
    return true;
}

bool stats(Stats &out) {
    SdLock lk;
    out = {};
    if (!s_ready) return false;
    out.cardSizeMB = (uint32_t)((uint64_t)sd.card()->sectorCount() >> 11);
    out.fatType = sd.fatType();
    out.manifestPresent = sd.exists(kManifestPath);
    FsFile dir = sd.open(kCoversDir);
    if (dir) {
        FsFile e;
        while (e.openNext(&dir, O_RDONLY)) {
            if (!e.isDir()) { out.coverCount++; out.coverBytes += e.fileSize(); }
            e.close();
        }
        dir.close();
    }
    return true;
}

bool purge() {
    SdLock lk;
    if (!s_ready) return false;
    // Manual sweep for the cache dirs we own.
    FsFile dir = sd.open(kCoversDir);
    if (dir) {
        FsFile e;
        while (e.openNext(&dir, O_RDONLY)) {
            char name[128];
            e.getName(name, sizeof(name));
            e.close();
            String full = String(kCoversDir) + "/" + name;
            sd.remove(full.c_str());
        }
        dir.close();
    }
    dir = sd.open(kDetailsDir);
    if (dir) {
        FsFile e;
        while (e.openNext(&dir, O_RDONLY)) {
            char name[128];
            e.getName(name, sizeof(name));
            e.close();
            String full = String(kDetailsDir) + "/" + name;
            sd.remove(full.c_str());
        }
        dir.close();
    }
    sd.remove(kManifestPath);
    sd.remove(kDetailsSigPath);
    sd.rmdir(kDetailsDir);
    sd.rmdir(kCoversDir);
    sd.rmdir(kRoot);
    ensureDir(kRoot);
    ensureDir(kCoversDir);
    ensureDir(kDetailsDir);
    return true;
}

}  // namespace sdcache
