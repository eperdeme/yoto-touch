# Image Rendering Fixes

## Objective
Fix the "garbled lines of colour" and "identical images" issues observed when downloading and rendering covers on the CrowPanel 7".

## Key Context
- The ESP32-S3 PSRAM cache line size is 64 bytes. Modifying unaligned memory and calling `esp_cache_msync` with the `INVALIDATE` flag causes neighboring data on the same cache line to be corrupted.
- LVGL 9 has an aggressive internal image cache. When we update the pixel data `cv->data` in the background, we must explicitly tell LVGL to invalidate the old cached texture using `lv_image_cache_drop`.

## Implementation Steps

### 1. Enforce 64-Byte Alignment for PSRAM Buffers
- In `main.cpp`, update `getOrAllocCover` to allocate `cv->data` with 64-byte alignment:
  ```cpp
  cv->data = (uint8_t *)heap_caps_aligned_alloc(64, THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ```
- Also update `netFetchPageIntoCache` to align `tempBuf`:
  ```cpp
  uint8_t *tempBuf = (uint8_t *)heap_caps_aligned_alloc(64, THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ```

### 2. Make Cache Sync Safer
- In `main.cpp`, modify `syncCoverPixels` to remove the `INVALIDATE` and `UNALIGNED` flags, as the memory is now strictly aligned and `DIR_C2M` is sufficient for the Display DMA to see the changes:
  ```cpp
  esp_err_t err = esp_cache_msync(
      data,
      THUMB_BYTES,
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA
  );
  ```

### 3. Drop LVGL Cache on Cover Update
- In `main.cpp`, whenever a cover is loaded into `cv->data` (either from SD or HTTP), immediately tell LVGL to drop it from its internal cache so it re-renders the new pixels.
- Inside `netFetchPageIntoCache`:
  ```cpp
  memcpy(cv->data, tempBuf, THUMB_BYTES);
  syncCoverPixels(cv->data);
  lv_image_cache_drop(&cv->dsc); // Force LVGL to re-read the pixels
  ```
- Do the same inside `tryLoadCoverFromSd`.

## Verification & Testing
- Wipe the SD card using the Developer Panel ("Wipe SD cache & reboot").
- Observe the image rendering as covers are downloaded from the Yoto API.
- Verify that each card renders its unique cover correctly without garbled colored lines.
