#include "sd_cache.h"

#include <SPI.h>
#include <SdFat.h>
#include "pins.h"

namespace sdcache {

// SdFat with ExFAT + FAT32 support. Uses HSPI; DMA is fine but the default
// non-DMA mode is plenty for our 28.8 KB cover reads.
static SPIClass sdSpi(HSPI);
static SdFs sd;
static bool s_ready = false;

static const char *kRoot         = "/yoto";
static const char *kCoversDir    = "/yoto/covers";
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
    s_ready = true;
    return true;
}

bool ready() { return s_ready; }

static String coverPath(const String &id) {
    return String(kCoversDir) + "/" + id + ".565";
}

bool hasCover(const String &id) {
    if (!s_ready) return false;
    return sd.exists(coverPath(id).c_str());
}

bool loadCover(const String &id, uint8_t *buf, size_t buf_bytes) {
    if (!s_ready) return false;
    FsFile f = sd.open(coverPath(id).c_str(), O_RDONLY);
    if (!f) return false;
    if ((size_t)f.fileSize() != buf_bytes) { f.close(); return false; }
    int got = readBounced(f, buf, buf_bytes);
    f.close();
    return got == (int)buf_bytes;
}

bool saveCover(const String &id, const uint8_t *buf, size_t buf_bytes) {
    if (!s_ready) return false;
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

// ----- manifest -----

bool saveManifest(const String &sig, const std::vector<CardMeta> &cards) {
    if (!s_ready) return false;
    if (sig.length() > 255) return false;
    FsFile f = sd.open(kManifestPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    uint8_t sl = (uint8_t)sig.length();
    f.write(&sl, 1);
    f.write((const uint8_t *)sig.c_str(), sl);
    uint16_t n = (uint16_t)min((size_t)0xFFFF, cards.size());
    uint8_t nb[2] = { (uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
    f.write(nb, 2);
    for (uint16_t i = 0; i < n; i++) {
        const CardMeta &c = cards[i];
        if (c.id.length() > 255 || c.title.length() > 255) { f.close(); sd.remove(kManifestPath); return false; }
        uint8_t il = (uint8_t)c.id.length();
        uint8_t tl = (uint8_t)c.title.length();
        f.write(&il, 1);
        f.write((const uint8_t *)c.id.c_str(), il);
        f.write(&tl, 1);
        f.write((const uint8_t *)c.title.c_str(), tl);
    }
    f.close();
    return true;
}

bool loadManifest(String &outSig, std::vector<CardMeta> &out) {
    if (!s_ready) return false;
    FsFile f = sd.open(kManifestPath, O_RDONLY);
    if (!f) return false;
    uint8_t sl = 0;
    if (f.read(&sl, 1) != 1) { f.close(); return false; }
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
        uint8_t il = 0, tl = 0;
        if (f.read(&il, 1) != 1) { f.close(); return false; }
        char idBuf[256];
        if (f.read((uint8_t *)idBuf, il) != (int)il) { f.close(); return false; }
        idBuf[il] = 0;
        if (f.read(&tl, 1) != 1) { f.close(); return false; }
        char tBuf[256];
        if (f.read((uint8_t *)tBuf, tl) != (int)tl) { f.close(); return false; }
        tBuf[tl] = 0;
        out.push_back(CardMeta{ String(idBuf), String(tBuf) });
    }
    f.close();
    return true;
}

bool stats(Stats &out) {
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
    if (!s_ready) return false;
    // Manual depth-1 sweep: covers + manifest live in /yoto, no nested dirs.
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
    sd.remove(kManifestPath);
    sd.rmdir(kCoversDir);
    sd.rmdir(kRoot);
    ensureDir(kRoot);
    ensureDir(kCoversDir);
    return true;
}

}  // namespace sdcache
