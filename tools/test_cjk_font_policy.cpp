#include "cjk_font_policy.hpp"

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <vector>

namespace cjk = musicremote::cjk_font;

namespace {

// A file whose byte at offset i is (i * 7 + 3) & 0xFF, read through a fake
// card that counts reads and can fail or return short reads on demand.
struct FakeFile {
  uint32_t size = 0;
  uint32_t reads = 0;
  bool fail = false;
  std::size_t short_by = 0;

  static uint8_t byte_at(uint32_t offset) { return static_cast<uint8_t>(offset * 7 + 3); }

  static std::size_t read(void *context, uint32_t offset, uint8_t *destination,
                          std::size_t length) {
    auto *file = static_cast<FakeFile *>(context);
    ++file->reads;
    assert(offset < file->size);
    assert(length <= file->size - offset);
    if (file->fail) {
      return 0;
    }
    const std::size_t got = length > file->short_by ? length - file->short_by : 0;
    for (std::size_t i = 0; i < got; ++i) {
      destination[i] = byte_at(offset + static_cast<uint32_t>(i));
    }
    return got;
  }
};

using SmallCache = cjk::BlockCache<16, 4>;

bool matches(const std::vector<uint8_t> &data, uint32_t offset, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (data[i] != FakeFile::byte_at(offset + static_cast<uint32_t>(i))) {
      return false;
    }
  }
  return true;
}

void test_reads_within_and_across_blocks() {
  FakeFile file{.size = 100};
  std::vector<uint8_t> storage(SmallCache::kStorageBytes);
  SmallCache cache;
  cache.attach(storage.data(), file.size, FakeFile::read, &file);

  std::vector<uint8_t> out(40);
  assert(cache.read(5, out.data(), 4) == 4);
  assert(matches(out, 5, 4));
  assert(file.reads == 1);

  // Same block: served from memory.
  assert(cache.read(0, out.data(), 16) == 16);
  assert(matches(out, 0, 16));
  assert(file.reads == 1);

  // Spans blocks 0, 1 and 2; only 1 and 2 are new.
  assert(cache.read(10, out.data(), 30) == 30);
  assert(matches(out, 10, 30));
  assert(file.reads == 3);
  assert(cache.misses() == 3);
}

void test_end_of_file_reads_zero() {
  FakeFile file{.size = 37};
  std::vector<uint8_t> storage(SmallCache::kStorageBytes);
  SmallCache cache;
  cache.attach(storage.data(), file.size, FakeFile::read, &file);

  std::vector<uint8_t> out(8, 0xAA);
  // The last block holds 5 bytes; the read must not ask the card for more.
  assert(cache.read(34, out.data(), 8) == 3);
  assert(matches(out, 34, 3));
  for (std::size_t i = 3; i < out.size(); ++i) {
    assert(out[i] == 0);
  }

  std::fill(out.begin(), out.end(), 0xAA);
  assert(cache.read(1000, out.data(), 4) == 0);
  for (std::size_t i = 0; i < 4; ++i) {
    assert(out[i] == 0);
  }
  // Offsets near UINT32_MAX must not wrap into the file.
  assert(cache.read(UINT32_MAX - 1, out.data(), 4) == 0);
  assert(file.reads == 1);
}

void test_least_recently_used_eviction() {
  FakeFile file{.size = 1000};
  std::vector<uint8_t> storage(SmallCache::kStorageBytes);
  SmallCache cache;
  cache.attach(storage.data(), file.size, FakeFile::read, &file);

  uint8_t byte = 0;
  for (uint32_t block = 0; block < 4; ++block) {
    cache.read(block * 16, &byte, 1);
  }
  assert(file.reads == 4);
  // Touch block 0 so block 1 becomes least recently used.
  cache.read(0, &byte, 1);
  assert(file.reads == 4);
  cache.read(4 * 16, &byte, 1);
  assert(file.reads == 5);
  // Blocks 0, 2, 3 and 4 are resident; 1 was evicted.
  for (uint32_t block : {0u, 2u, 3u, 4u}) {
    cache.read(block * 16 + 1, &byte, 1);
    assert(byte == FakeFile::byte_at(block * 16 + 1));
  }
  assert(file.reads == 5);
  cache.read(16, &byte, 1);
  assert(byte == FakeFile::byte_at(16));
  assert(file.reads == 6);
}

void test_read_errors_zero_fill_and_retry() {
  FakeFile file{.size = 64, .fail = true};
  std::vector<uint8_t> storage(SmallCache::kStorageBytes);
  SmallCache cache;
  cache.attach(storage.data(), file.size, FakeFile::read, &file);

  std::vector<uint8_t> out(4, 0xAA);
  assert(cache.read(20, out.data(), 4) == 0);
  for (uint8_t value : out) {
    assert(value == 0);
  }
  // A failed block is not cached: the next read tries the card again.
  file.fail = false;
  assert(cache.read(20, out.data(), 4) == 4);
  assert(matches(out, 20, 4));
  assert(file.reads == 2);

  // A short read keeps what arrived and zero-fills the rest.
  file.short_by = 6;
  std::vector<uint8_t> tail(16, 0xAA);
  assert(cache.read(32, tail.data(), 16) == 10);
  assert(matches(tail, 32, 10));
  for (std::size_t i = 10; i < tail.size(); ++i) {
    assert(tail[i] == 0);
  }
}

void test_unattached_cache_reads_zero() {
  SmallCache cache;
  uint8_t out[4] = {1, 2, 3, 4};
  assert(cache.read(0, out, sizeof(out)) == 0);
  for (uint8_t value : out) {
    assert(value == 0);
  }
}

void test_policy() {
  // LV_SYMBOL_* glyphs (FontAwesome, BMP Private Use Area) skip the TTF.
  assert(cjk::is_private_use(0xF001));  // LV_SYMBOL_AUDIO
  assert(cjk::is_private_use(0xF8FF));  // LV_SYMBOL_DUMMY
  assert(cjk::is_private_use(0xE000));
  assert(cjk::is_private_use(0x10FFFD));
  assert(!cjk::is_private_use(0x5B57));  // 字
  assert(!cjk::is_private_use(0x3042));  // あ
  assert(!cjk::is_private_use(0xFF01));  // full-width !
  assert(!cjk::is_private_use(0xDFFF));
  assert(!cjk::is_private_use(0xF900));  // CJK Compatibility Ideographs

  static_assert(cjk::kSizes.size() == 4);
  for (const cjk::SizeSpec &size : cjk::kSizes) {
    assert(size.cache_glyphs >= 16);
  }
  // A 28 px Droid Sans Fallback ideograph measures 27x27 px on the host.
  static_assert(cjk::glyph_bytes_upper_bound(28) >= 27 * (27 + 3) + 256);
  // Budget documented in main/fonts/README.md: at most 384 KB of PSRAM.
  static_assert(cjk::FontBlockCache::kStorageBytes + cjk::glyph_cache_bytes_upper_bound() <=
                384 * 1024);
}

} // namespace

int main() {
  test_reads_within_and_across_blocks();
  test_end_of_file_reads_zero();
  test_least_recently_used_eviction();
  test_read_errors_zero_fill_and_retry();
  test_unattached_cache_reads_zero();
  test_policy();
}
