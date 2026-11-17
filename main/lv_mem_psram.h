#pragma once

// LVGL's allocator for this firmware (CONFIG_LV_USE_CUSTOM_MALLOC). It is
// plain malloc(), which places blocks of 4 KB or less in internal RAM
// (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), except while a caller has asked for
// PSRAM. The CJK font asks for PSRAM around every tiny_ttf call, so its glyph
// caches and bitmaps never take internal RAM from Wi-Fi and the AIMP client.
// Only the GUI task, or a caller holding the GUI mutex, may change it.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sets whether LVGL allocations prefer PSRAM and returns the previous setting.
bool ui_lv_mem_prefer_psram(bool prefer);

#ifdef __cplusplus
}

namespace musicremote {

// Prefers PSRAM for LVGL allocations for the lifetime of the scope.
class LvglPsramScope {
 public:
  LvglPsramScope() : previous_(ui_lv_mem_prefer_psram(true)) {}
  ~LvglPsramScope() { ui_lv_mem_prefer_psram(previous_); }
  LvglPsramScope(const LvglPsramScope &) = delete;
  LvglPsramScope &operator=(const LvglPsramScope &) = delete;

 private:
  bool previous_;
};

} // namespace musicremote
#endif
