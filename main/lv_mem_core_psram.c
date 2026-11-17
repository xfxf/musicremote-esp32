// LVGL memory core for CONFIG_LV_USE_CUSTOM_MALLOC; see lv_mem_psram.h.

#include "lv_mem_psram.h"

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "sdkconfig predates CONFIG_LV_USE_CUSTOM_MALLOC; run tools/configure.sh to regenerate it from sdkconfig.defaults"
#endif

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static bool prefer_psram;

bool ui_lv_mem_prefer_psram(bool prefer) {
  const bool previous = prefer_psram;
  prefer_psram = prefer;
  return previous;
}

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

// Falls back to internal RAM only when PSRAM is exhausted: LVGL treats a
// failed allocation as fatal in several places.
void *lv_malloc_core(size_t size) {
  if (prefer_psram) {
    return heap_caps_malloc_prefer(size, 2, PSRAM_CAPS, MALLOC_CAP_DEFAULT);
  }
  return malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size) {
  if (prefer_psram) {
    return heap_caps_realloc_prefer(p, new_size, 2, PSRAM_CAPS, MALLOC_CAP_DEFAULT);
  }
  return realloc(p, new_size);
}

void lv_free_core(void *p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
