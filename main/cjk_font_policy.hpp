#pragma once

// Policy for the optional CJK font that LVGL's tiny_ttf renders at run time
// from the microSD card. Free of ESP-IDF and LVGL so tools/ can test it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace musicremote::cjk_font {

inline constexpr const char *kFontPath = "/sdcard/fonts/cjk.ttf";

// Fonts reviewed for use. stb_truetype trusts font structure (compound
// glyphs recurse without a depth limit), so tiny_ttf only ever sees a file
// whose size and SHA-256 match an entry here. Add an entry only after
// reviewing the font; tools/fetch_cjk_font.sh pins the same file.
struct ReviewedFont {
  const char *name;
  uint32_t size;
  std::array<uint8_t, 32> sha256;
};

inline constexpr std::array<ReviewedFont, 1> kReviewedFonts{{
    {"Droid Sans Fallback Full (AOSP)", 4033576,
     {0x23, 0x92, 0x01, 0x55, 0x30, 0x43, 0x8b, 0xaf, 0xc4, 0x8e, 0xdf, 0xc4, 0xae, 0xe6, 0xd9, 0xde, 0x23, 0x87, 0xf6, 0x27, 0xa6, 0x13, 0x4d, 0x8a, 0xb3, 0xdf, 0xcc, 0x99, 0xd2, 0x1c, 0x82, 0x40}},
}};

constexpr bool reviewed_size(uint32_t size) {
  for (const ReviewedFont &font : kReviewedFonts) {
    if (font.size == size) return true;
  }
  return false;
}

inline const ReviewedFont *reviewed_font(uint32_t size, const std::array<uint8_t, 32> &sha256) {
  for (const ReviewedFont &font : kReviewedFonts) {
    if (font.size == size && font.sha256 == sha256) return &font;
  }
  return nullptr;
}

struct SizeSpec {
  int32_t pixel_size;
  // Glyphs tiny_ttf keeps per size: both the metrics cache and the rendered
  // bitmap cache hold this many entries. Body sizes must exceed the distinct
  // CJK glyphs on one screen of rows, or LRU thrashes and every redraw
  // re-renders from the card.
  uint16_t cache_glyphs;
};

// The order matches the ui_font_cjk_<size> proxies in cjk_font.cpp.
inline constexpr std::array<SizeSpec, 4> kSizes{{{14, 96}, {18, 96}, {22, 32}, {28, 32}}};

// Upper bound of one cached glyph, in bytes: an A8 bitmap with one pixel of
// margin each way (tiny_ttf adds one to both dimensions; four-byte stride
// alignment adds at most three more per row), plus the draw buffer header,
// the two cache entries and allocator overhead.
constexpr std::size_t glyph_bytes_upper_bound(int32_t pixel_size) {
  const std::size_t side = static_cast<std::size_t>(pixel_size) + 2;
  return (side + 3) * side + 512;
}

constexpr std::size_t glyph_cache_bytes_upper_bound() {
  std::size_t total = 0;
  for (const SizeSpec &size : kSizes) {
    total += size.cache_glyphs * glyph_bytes_upper_bound(size.pixel_size);
  }
  return total;
}

// LV_SYMBOL_* glyphs are FontAwesome codepoints in the Private Use Area and
// come from the Montserrat font behind the CJK font. Skipping the TTF for
// them avoids a cmap search and a negative cache entry on every icon draw.
constexpr bool is_private_use(uint32_t codepoint) {
  return (codepoint >= 0xE000 && codepoint <= 0xF8FF) || codepoint >= 0xF0000;
}

// Sizes of the file block cache that sits between stb_truetype's many small
// reads and the card.
inline constexpr std::size_t kBlockBytes = 1024;
inline constexpr std::size_t kBlockCount = 64;

// A fixed-size, least-recently-used cache of file blocks. stb_truetype reads
// a font one to four bytes at a time; this turns those reads into memory
// copies and touches the card once per missing block. The caller supplies
// the block storage (PSRAM on the device) and a positional read function.
// Bytes past the end of the file, or lost to a read error, read as zero, so
// a truncated or corrupt font cannot make stb_truetype read stale data.
template <std::size_t BlockBytes, std::size_t BlockCount>
class BlockCache {
 public:
  // Reads up to length bytes at offset; returns the number read.
  using ReadFn = std::size_t (*)(void *context, uint32_t offset, uint8_t *destination,
                                 std::size_t length);

  static constexpr std::size_t kStorageBytes = BlockBytes * BlockCount;

  void attach(uint8_t *storage, uint32_t file_size, ReadFn read, void *context) {
    storage_ = storage;
    file_size_ = file_size;
    read_ = read;
    context_ = context;
    for (Slot &slot : slots_) {
      slot = Slot{};
    }
    clock_ = 0;
    most_recent_ = 0;
    misses_ = 0;
  }

  uint8_t *storage() const { return storage_; }
  uint32_t file_size() const { return file_size_; }
  uint32_t misses() const { return misses_; }

  // Copies length bytes at offset into destination and returns how many of
  // them came from the file; the rest are zero.
  std::size_t read(uint32_t offset, void *destination, std::size_t length) {
    auto *out = static_cast<uint8_t *>(destination);
    std::size_t copied = 0;
    while (copied < length) {
      const uint64_t position = static_cast<uint64_t>(offset) + copied;
      if (position >= file_size_) {
        break;
      }
      const uint32_t block = static_cast<uint32_t>(position / BlockBytes);
      const std::size_t within = static_cast<std::size_t>(position % BlockBytes);
      const Slot *slot = find_or_load(block);
      if (slot == nullptr || within >= slot->valid_bytes) {
        break;
      }
      std::size_t run = slot->valid_bytes - within;
      if (run > length - copied) {
        run = length - copied;
      }
      std::memcpy(out + copied, data(*slot) + within, run);
      copied += run;
    }
    if (copied < length) {
      std::memset(out + copied, 0, length - copied);
    }
    return copied;
  }

 private:
  static constexpr uint32_t kNoBlock = UINT32_MAX;

  struct Slot {
    uint32_t block = kNoBlock;
    uint32_t valid_bytes = 0;
    uint64_t last_use = 0;
  };

  const uint8_t *data(const Slot &slot) const {
    return storage_ + static_cast<std::size_t>(&slot - slots_.data()) * BlockBytes;
  }

  const Slot *find_or_load(uint32_t block) {
    ++clock_;
    // Consecutive reads nearly always hit the block used last.
    Slot &recent = slots_[most_recent_];
    if (recent.block == block) {
      recent.last_use = clock_;
      return &recent;
    }
    std::size_t victim = 0;
    for (std::size_t i = 0; i < BlockCount; ++i) {
      if (slots_[i].block == block) {
        slots_[i].last_use = clock_;
        most_recent_ = i;
        return &slots_[i];
      }
      if (slots_[i].last_use < slots_[victim].last_use) {
        victim = i;
      }
    }
    if (storage_ == nullptr || read_ == nullptr) {
      return nullptr;
    }
    ++misses_;
    Slot &slot = slots_[victim];
    const uint64_t start = static_cast<uint64_t>(block) * BlockBytes;
    std::size_t wanted = BlockBytes;
    if (file_size_ - start < wanted) {
      wanted = static_cast<std::size_t>(file_size_ - start);
    }
    uint8_t *buffer = storage_ + victim * BlockBytes;
    const std::size_t got = read_(context_, static_cast<uint32_t>(start), buffer, wanted);
    if (got == 0 || got > wanted) {
      // Leave the slot empty so a transient error is retried next time.
      slot = Slot{};
      return nullptr;
    }
    slot.block = block;
    slot.valid_bytes = static_cast<uint32_t>(got);
    slot.last_use = clock_;
    most_recent_ = victim;
    return &slot;
  }

  std::array<Slot, BlockCount> slots_{};
  uint8_t *storage_ = nullptr;
  uint32_t file_size_ = 0;
  ReadFn read_ = nullptr;
  void *context_ = nullptr;
  uint64_t clock_ = 0;
  std::size_t most_recent_ = 0;
  uint32_t misses_ = 0;
};

using FontBlockCache = BlockCache<kBlockBytes, kBlockCount>;

} // namespace musicremote::cjk_font
