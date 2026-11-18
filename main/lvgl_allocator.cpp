// LVGL heap hooks (CONFIG_LV_USE_CUSTOM_MALLOC).
//
// With CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, plain malloc() places every block
// under 4 KiB in internal RAM. LVGL objects, styles, and label text are all
// small, so building one 100-row queue page consumed the last ~35 KiB of
// internal RAM. The microSD SPI driver then could not allocate its 512-byte
// DMA bounce buffer, and ESP-IDF's error path panicked mid-read, which also
// left the card unusable until the next mount recovered it (issue #48).
//
// LVGL never hands these blocks to DMA (the display's draw buffers are
// allocated separately with MALLOC_CAP_DMA), so prefer PSRAM and fall back to
// internal RAM only when PSRAM is exhausted.

#include "esp_heap_caps.h"
#include "lvgl.h"

namespace {

constexpr uint32_t kPreferred = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t kFallback = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

} // namespace

extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return nullptr;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) {
  return heap_caps_malloc_prefer(size, 2, kPreferred, kFallback);
}

void *lv_realloc_core(void *p, size_t new_size) {
  return heap_caps_realloc_prefer(p, new_size, 2, kPreferred, kFallback);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

} // extern "C"
