#pragma once

// Policy for the optional CJK font that LVGL's tiny_ttf renders at run time
// from the microSD card. Free of ESP-IDF and LVGL so tools/ can test it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace musicremote::cjk_font {

inline constexpr const char *kFontPath = "/sdcard/fonts/cjk.ttf";

// A TrueType offset table is 12 bytes. The recommended font is about 4 MB;
// the upper bound only rejects files that cannot be a sensible UI font.
inline constexpr uint32_t kMinFileBytes = 12;
inline constexpr uint32_t kMaxFileBytes = 32u * 1024u * 1024u;

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
