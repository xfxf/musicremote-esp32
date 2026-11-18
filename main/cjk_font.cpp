// Run-time CJK glyphs: LVGL tiny_ttf streaming a TrueType font from microSD.
//
// The fallback chain at each size is ui_font_<size> (Selawik-derived) ->
// ui_font_ext_<size> (Noto, compiled in) -> ui_font_cjk_<size> -> Montserrat.
// The generated fonts are const, so ui_font_ext_<size> names the mutable
// ui_font_cjk_<size> objects below as its fallback. Each starts as a copy of
// Montserrat without a fallback, so LV_SYMBOL_* icons work with no card. When
// the font loads, it becomes a copy of the tiny_ttf font (same dsc and
// callbacks) that falls back to Montserrat. lv_font_get_glyph_dsc() records
// the font that supplied each glyph as resolved_font, and LVGL fetches and
// releases the bitmap through that same object, so the copy is used
// consistently.

#include "cjk_font.hpp"

#include <array>
#include <atomic>
#include <cinttypes>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "cjk_font_policy.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/sha256.h"

#if !LV_USE_TINY_TTF || !LV_TINY_TTF_FILE_SUPPORT
#error "sdkconfig predates the CJK font; run tools/configure.sh to regenerate it from sdkconfig.defaults"
#endif

namespace {

lv_font_t symbols_only(const lv_font_t &montserrat) {
  lv_font_t copy = montserrat;
  copy.fallback = nullptr;
  return copy;
}

} // namespace

// Dynamic initialisation runs before app_main, and the Montserrat fonts are
// constant data, so these are valid before LVGL draws anything. They are
// deliberately not const: activate() overwrites them once the font is verified.
extern "C" {
lv_font_t ui_font_cjk_14 = symbols_only(lv_font_montserrat_14);
lv_font_t ui_font_cjk_18 = symbols_only(lv_font_montserrat_18);
lv_font_t ui_font_cjk_22 = symbols_only(lv_font_montserrat_22);
lv_font_t ui_font_cjk_28 = symbols_only(lv_font_montserrat_28);
}

namespace musicremote {
namespace {

namespace policy = cjk_font;

constexpr const char *kTag = "cjk-font";
// The lv_fs drive letter of the font driver below; it serves one file.
constexpr const char *kLvglPath = "F:cjk.ttf";
constexpr uint32_t kMaxOpenHandles = 8;

struct Proxy {
  lv_font_t *font;
  const lv_font_t *symbols;
};

static_assert(policy::kSizes.size() == 4 && policy::kSizes[0].pixel_size == 14 &&
                  policy::kSizes[1].pixel_size == 18 && policy::kSizes[2].pixel_size == 22 &&
                  policy::kSizes[3].pixel_size == 28,
              "kProxies pairs each size with its ui_font_cjk_<size> and Montserrat font");

const std::array<Proxy, policy::kSizes.size()> kProxies{{
    {&ui_font_cjk_14, &lv_font_montserrat_14},
    {&ui_font_cjk_18, &lv_font_montserrat_18},
    {&ui_font_cjk_22, &lv_font_montserrat_22},
    {&ui_font_cjk_28, &lv_font_montserrat_28},
}};

// tiny_ttf's own callbacks, identical for every size.
decltype(lv_font_t::get_glyph_dsc) ttf_get_glyph_dsc = nullptr;
decltype(lv_font_t::get_glyph_bitmap) ttf_get_glyph_bitmap = nullptr;
decltype(lv_font_t::release_glyph) ttf_release_glyph = nullptr;

// tiny_ttf allocates its glyph and bitmap caches and stb_truetype's rasteriser
// buffers through LVGL, which prefers PSRAM (main/lvgl_allocator.cpp).

bool cjk_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out, uint32_t letter,
                       uint32_t letter_next) {
  if (policy::is_private_use(letter)) {
    return false;
  }
  return ttf_get_glyph_dsc(font, dsc_out, letter, letter_next);
}

const void *cjk_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc, lv_draw_buf_t *draw_buf) {
  return ttf_get_glyph_bitmap(g_dsc, draw_buf);
}

void cjk_release_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *g_dsc) {
  ttf_release_glyph(font, g_dsc);
}

// A read-only lv_fs driver over one shared descriptor and a PSRAM block
// cache. Each tiny_ttf font opens the file, so four sizes would otherwise
// hold four of the card's eight file slots and four separate read buffers.
int font_fd = -1;
policy::FontBlockCache *block_cache = nullptr;

struct Handle {
  bool open = false;
  uint32_t position = 0;
};
std::array<Handle, kMaxOpenHandles> handles;
lv_fs_drv_t driver;

std::size_t read_at(void *context, uint32_t offset, uint8_t *destination, std::size_t length) {
  const ssize_t got = pread(*static_cast<int *>(context), destination, length, offset);
  return got > 0 ? static_cast<std::size_t>(got) : 0;
}

void *fs_open(lv_fs_drv_t *, const char *, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD || block_cache == nullptr) {
    return nullptr;
  }
  for (Handle &handle : handles) {
    if (!handle.open) {
      handle = Handle{.open = true, .position = 0};
      return &handle;
    }
  }
  return nullptr;
}

lv_fs_res_t fs_close(lv_fs_drv_t *, void *file) {
  static_cast<Handle *>(file)->open = false;
  return LV_FS_RES_OK;
}

// stb_truetype ignores short reads; BlockCache zero-fills them.
lv_fs_res_t fs_read(lv_fs_drv_t *, void *file, void *buffer, uint32_t length, uint32_t *read) {
  auto *handle = static_cast<Handle *>(file);
  const std::size_t got = block_cache->read(handle->position, buffer, length);
  handle->position += static_cast<uint32_t>(got);
  if (read != nullptr) {
    *read = static_cast<uint32_t>(got);
  }
  return LV_FS_RES_OK;
}

lv_fs_res_t fs_seek(lv_fs_drv_t *, void *file, uint32_t position, lv_fs_whence_t whence) {
  auto *handle = static_cast<Handle *>(file);
  switch (whence) {
  case LV_FS_SEEK_SET:
    handle->position = position;
    break;
  case LV_FS_SEEK_CUR:
    handle->position += position;
    break;
  case LV_FS_SEEK_END:
    handle->position = block_cache->file_size() + position;
    break;
  default:
    return LV_FS_RES_INV_PARAM;
  }
  return LV_FS_RES_OK;
}

lv_fs_res_t fs_tell(lv_fs_drv_t *, void *file, uint32_t *position) {
  *position = static_cast<Handle *>(file)->position;
  return LV_FS_RES_OK;
}

void register_driver() {
  static bool registered = false;
  if (registered) {
    return;
  }
  lv_fs_drv_init(&driver);
  driver.letter = kLvglPath[0];
  driver.cache_size = 0; // BlockCache already caches.
  driver.open_cb = fs_open;
  driver.close_cb = fs_close;
  driver.read_cb = fs_read;
  driver.seek_cb = fs_seek;
  driver.tell_cb = fs_tell;
  lv_fs_drv_register(&driver);
  registered = true;
}

void release_file() {
  if (block_cache != nullptr) {
    heap_caps_free(block_cache->storage());
    block_cache->~BlockCache();
    heap_caps_free(block_cache);
    block_cache = nullptr;
  }
  if (font_fd >= 0) {
    close(font_fd);
    font_fd = -1;
  }
  // tiny_ttf does not close the file when stb_truetype rejects it.
  handles.fill(Handle{});
}

unsigned kib(std::size_t bytes) { return static_cast<unsigned>((bytes + 1023) / 1024); }

} // namespace

namespace {

// Verification state, handed from the verify task to the GUI task.
enum class Stage : uint8_t { idle, verifying, verified, rejected, active };
std::atomic<Stage> stage{Stage::idle};
uint32_t verified_size = 0;
int64_t verify_ms = 0;
const char *verified_name = nullptr;

constexpr std::size_t kHashChunkBytes = 16384;
constexpr uint32_t kVerifyStackBytes = 4096;
constexpr UBaseType_t kVerifyPriority = 2;

// Hashes the already-open font_fd. The descriptor that was hashed is the one
// tiny_ttf later reads, so a file replaced on the card meanwhile is never
// used unverified.
void verify_task(void *) {
  const int64_t started_us = esp_timer_get_time();
  auto *buffer = static_cast<uint8_t *>(
      heap_caps_malloc(kHashChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool ok = buffer != nullptr && mbedtls_sha256_starts(&sha, 0) == 0;
  uint32_t total = 0;
  while (ok) {
    const ssize_t got = pread(font_fd, buffer, kHashChunkBytes, total);
    if (got < 0) {
      ok = false;
    } else if (got == 0) {
      break;
    } else {
      ok = mbedtls_sha256_update(&sha, buffer, static_cast<std::size_t>(got)) == 0;
      total += static_cast<uint32_t>(got);
    }
  }
  std::array<uint8_t, 32> digest{};
  ok = ok && mbedtls_sha256_finish(&sha, digest.data()) == 0;
  mbedtls_sha256_free(&sha);
  heap_caps_free(buffer);
  const policy::ReviewedFont *font = ok ? policy::reviewed_font(total, digest) : nullptr;
  verify_ms = (esp_timer_get_time() - started_us) / 1000;
  if (font == nullptr) {
    ESP_LOGW(kTag, "%s is not a reviewed font (%s); CJK text will show boxes",
             policy::kFontPath, ok ? "SHA-256 mismatch" : "read error");
    close(font_fd);
    font_fd = -1;
    stage.store(Stage::rejected);
  } else {
    verified_size = total;
    verified_name = font->name;
    stage.store(Stage::verified);
  }
  vTaskDelete(nullptr);
}

// Creates the tiny_ttf fonts over the verified descriptor and swaps them into
// the ui_font_cjk_<size> proxies. Runs on the GUI task with LVGL locked.
bool activate() {
  void *cache_memory =
      heap_caps_malloc(sizeof(policy::FontBlockCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  auto *storage = static_cast<uint8_t *>(heap_caps_malloc(policy::FontBlockCache::kStorageBytes,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (cache_memory == nullptr || storage == nullptr) {
    heap_caps_free(cache_memory);
    heap_caps_free(storage);
    close(font_fd);
    font_fd = -1;
    ESP_LOGW(kTag, "Could not reserve the CJK font's PSRAM cache; CJK text will show boxes");
    return false;
  }
  block_cache = new (cache_memory) policy::FontBlockCache();
  block_cache->attach(storage, verified_size, read_at, &font_fd);
  register_driver();

  std::array<lv_font_t *, policy::kSizes.size()> loaded{};
  bool complete = true;
  for (std::size_t i = 0; i < loaded.size(); ++i) {
    loaded[i] = lv_tiny_ttf_create_file_ex(kLvglPath, policy::kSizes[i].pixel_size,
                                           LV_FONT_KERNING_NONE, policy::kSizes[i].cache_glyphs);
    if (loaded[i] == nullptr) {
      complete = false;
      break;
    }
  }
  if (!complete) {
    for (lv_font_t *font : loaded) {
      if (font != nullptr) {
        lv_tiny_ttf_destroy(font);
      }
    }
    release_file();
    ESP_LOGW(kTag, "%s is not a TrueType font tiny_ttf can read; CJK text will show boxes",
             policy::kFontPath);
    return false;
  }

  ttf_get_glyph_dsc = loaded[0]->get_glyph_dsc;
  ttf_get_glyph_bitmap = loaded[0]->get_glyph_bitmap;
  ttf_release_glyph = loaded[0]->release_glyph;
  for (std::size_t i = 0; i < loaded.size(); ++i) {
    lv_font_t &proxy = *kProxies[i].font;
    proxy = *loaded[i];
    proxy.get_glyph_dsc = cjk_get_glyph_dsc;
    proxy.get_glyph_bitmap = cjk_get_glyph_bitmap;
    proxy.release_glyph = cjk_release_glyph;
    proxy.fallback = kProxies[i].symbols;
    // The tiny_ttf font object itself is no longer referenced: its dsc,
    // which owns the caches and the open file, now belongs to the proxy.
    lv_free(loaded[i]);
  }
  // Labels measured their text with the placeholder glyphs; re-measure.
  lv_obj_report_style_change(nullptr);

  ESP_LOGI(kTag,
           "Loaded %s (%s, %" PRIu32 " KB, SHA-256 verified in %lld ms) at 14/18/22/28 px; "
           "PSRAM cache up to %u KB; internal RAM free %u KB, PSRAM free %u KB",
           policy::kFontPath, verified_name, static_cast<uint32_t>(kib(verified_size)),
           static_cast<long long>(verify_ms),
           kib(policy::FontBlockCache::kStorageBytes + policy::glyph_cache_bytes_upper_bound()),
           kib(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           kib(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  return true;
}

} // namespace

bool start_cjk_font(bool sd_mounted) {
  if (!sd_mounted) {
    ESP_LOGI(kTag, "microSD not mounted; CJK text will show placeholder boxes");
    return false;
  }
  struct stat info {};
  if (stat(policy::kFontPath, &info) != 0) {
    ESP_LOGI(kTag, "%s not found; CJK text will show placeholder boxes", policy::kFontPath);
    return false;
  }
  if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
      !policy::reviewed_size(static_cast<uint32_t>(info.st_size))) {
    ESP_LOGW(kTag, "%s is not a reviewed font (%ld bytes); CJK text will show boxes",
             policy::kFontPath, static_cast<long>(info.st_size));
    return false;
  }
  font_fd = open(policy::kFontPath, O_RDONLY);
  if (font_fd < 0) {
    ESP_LOGW(kTag, "Could not open %s; CJK text will show boxes", policy::kFontPath);
    return false;
  }
  stage.store(Stage::verifying);
  if (xTaskCreatePinnedToCoreWithCaps(verify_task, "cjk_verify", kVerifyStackBytes, nullptr,
                                      kVerifyPriority, nullptr, tskNO_AFFINITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS &&
      xTaskCreatePinnedToCore(verify_task, "cjk_verify", kVerifyStackBytes, nullptr,
                              kVerifyPriority, nullptr, tskNO_AFFINITY) != pdPASS) {
    close(font_fd);
    font_fd = -1;
    stage.store(Stage::rejected);
    ESP_LOGW(kTag, "Could not start CJK font verification; CJK text will show boxes");
    return false;
  }
  return true;
}

void poll_cjk_font() {
  Stage expected = Stage::verified;
  if (stage.compare_exchange_strong(expected, Stage::active)) {
    activate();
  }
}

} // namespace musicremote
