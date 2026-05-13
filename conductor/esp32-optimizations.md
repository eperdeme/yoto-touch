# ESP32 UI/UX and Advanced Stability Optimizations

## Background & Motivation
Following the initial round of optimizations (display bounce buffer, widget reuse, async SD reads, and precise PNG sizing), a further review identified critical areas for stability and efficiency improvements. Specifically, a potential memory race condition during fast scrolling, redundant JSON processing, and inefficient API response handling. 

## Scope & Impact
These advanced optimizations will focus on:
1.  Eliminating a use-after-free race condition in the image cache.
2.  Reducing heap fragmentation and CPU usage by removing internal JSON round-trips.
3.  Minimizing peak memory usage during large library fetches via streaming JSON parsing.
4.  Tuning the display refresh rate to reduce memory bandwidth requirements.

## Proposed Solution

### 1. Fix Cover Cache Race Condition
**File:** `firmware/arduino/src/main.cpp`
- **Issue:** `netFetchPageIntoCache()` takes a raw destination pointer, releases the mutex, and downloads directly into it. If the UI thread calls `evictDistantCovers()` and frees that memory while the download is running, it results in a use-after-free crash.
- **Change:** Allocate a temporary 41KB PSRAM buffer for the download/decode process. Once decoding is complete, re-acquire the `coverCacheMtx`, check if the card is still in the cache, and if so, `memcpy` from the temp buffer to the cache buffer. Free the temp buffer.

### 2. Remove Internal JSON Round-trips
**File:** `firmware/arduino/src/main.cpp`
- **Issue:** The network task fetches data, builds a JSON string, passes it via a shared variable, and the UI task parses it back into objects.
- **Change:** Replace `netManifestBody` and `netFiltersBody` strings with `std::vector<CardMeta>` and native structs (e.g., `std::vector<String>` for authors/series). The net task populates these structs directly and passes them under a mutex, eliminating the intermediate JSON serialization step.

### 3. Stream API JSON
**File:** `firmware/arduino/src/yoto_api.cpp`
- **Issue:** `apiGet()` reads the entire Yoto API response into a single `String`, which requires a contiguous block of heap memory and can fail for large libraries.
- **Change:** Refactor `yotoFetchLibrary()` to use `deserializeJson(doc, http.getStream())` along with a JSON filter to discard unnecessary fields immediately during parsing, drastically reducing peak memory usage.

### 4. UX & Refresh Tuning
**Files:** `firmware/arduino/include/lv_conf.h`, `firmware/arduino/src/main.cpp`
- **Change:** Change `LV_DEF_REFR_PERIOD` from `16` to `33` (approx 30 FPS). This halves the SPI/DMA memory bandwidth requirements for screen updates.
- **Change:** In `processDetailResult()`, pre-fill the author and series information in the Card Detail view immediately from the `allCards` cache, rather than waiting for the `/content/{id}` API call to complete.

## Verification & Testing
- Rapidly page through the library while on a slow network connection to verify the cache race condition is resolved (no crashes).
- Monitor heap usage in the developer modal before and after library syncs to verify streaming JSON improvements.
- Verify the Card Detail view feels instantly responsive.