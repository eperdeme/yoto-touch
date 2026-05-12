// Persistent cover + manifest cache backed by the on-board microSD.
//
// All paths live under /yoto/ on the SD root:
//   /yoto/covers/<cardId>.565   raw 120x120 RGB565 (28,800 bytes)
//   /yoto/manifest.bin          [u8 sig_len][sig][u16 n]{[u8 idlen][id][u8 titlelen][title]}*
//
// Read paths are stateless and synchronous. Writes are best-effort and never
// block longer than ~100 ms (SPI SD on this board).
#pragma once

#include <Arduino.h>
#include <vector>

struct CardMeta {
    String id;
    String title;
};

namespace sdcache {

bool begin();                                              // mount; safe to call repeatedly
bool ready();                                              // returns last mount result

// Covers (RGB565, fixed-size).
bool loadCover(const String &id, uint8_t *buf, size_t buf_bytes);
bool saveCover(const String &id, const uint8_t *buf, size_t buf_bytes);
bool hasCover(const String &id);

// Manifest (full card list for the current filter signature).
bool saveManifest(const String &signature, const std::vector<CardMeta> &cards);
bool loadManifest(String &outSignature, std::vector<CardMeta> &outCards);

// Dev helpers.
struct Stats { uint32_t coverCount; uint64_t coverBytes; bool manifestPresent; uint32_t cardSizeMB; uint8_t fatType; };
bool stats(Stats &out);
bool purge();                                              // deletes /yoto recursively

}  // namespace sdcache
