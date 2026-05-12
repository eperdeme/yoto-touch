// Custom LVGL allocator that routes everything to PSRAM.
//
// LVGL v9 calls these `_core` symbols when LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM.
// We back them with heap_caps_*(MALLOC_CAP_SPIRAM) so widgets, styles, list
// rows, animations etc all live in the 8MB external PSRAM and never compete
// with WiFi/HTTP/task stacks for the ~190KB internal DRAM heap.
//
// PSRAM access is ~4× slower than DRAM, but for widget metadata that's
// invisible. Hot pixel paths (framebuffer, draw buffers) are managed by the
// GFX library and aren't routed through here.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "lvgl.h"

extern "C" {

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}

void *lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size) {
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) {
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    if (!mon_p) return;
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    mon_p->total_size       = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_size        = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->free_cnt         = info.free_blocks;
    mon_p->used_cnt         = info.allocated_blocks;
    mon_p->max_used         = info.total_allocated_bytes;
    mon_p->used_pct         = mon_p->total_size
        ? (uint8_t)((100ULL * info.total_allocated_bytes) / mon_p->total_size)
        : 0;
    mon_p->frag_pct         = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}
