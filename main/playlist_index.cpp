#include "playlist_index.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace musicremote {
namespace {

constexpr uint32_t kIndexMagic = 0x5844494d; // MIDX
constexpr uint16_t kIndexVersion = 3;
constexpr uint32_t kActiveMagic = 0x5443414d; // MACT
constexpr uint16_t kActiveVersion = 1;
constexpr uint32_t kMaxDecodedBytes = 64 * 1024 * 1024;
constexpr size_t kHistoryBytes = 64 * 1024;
constexpr size_t kCopyBufferBytes = 4096;
// stdio buffer for the build's sequential files: fewer, larger FAT transfers.
constexpr size_t kBuildFileBuffer = 16 * 1024;
// Rebuild progress, in thousandths, weighted by measured phase durations.
constexpr uint32_t kProgressTotal = 1000;
constexpr uint32_t kDecodeWeight = 171;
constexpr uint32_t kRowsWeight = 678;
constexpr uint32_t kSearchWeight = 148;
constexpr uint16_t kMaxStoredField = 2048;
constexpr int64_t kInternalFlashRebuildIntervalUs = 15LL * 60 * 1000 * 1000;
constexpr uint64_t kFnvOffset = PlaylistIndex::kSourceHashSeed;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

#pragma pack(push, 1)
struct IndexHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  int32_t playlist_id;
  uint32_t count;
  uint32_t records_offset;
  uint32_t strings_offset;
  uint32_t strings_size;
  uint32_t search_offset;
  uint32_t search_size;
  uint32_t source_size;
  uint64_t source_hash;
  // Version 3: hash of the decoded playlist without its ID and flags fields.
  // A request for the current playlist (-1) and one for its explicit ID
  // compress differently but describe the same rows.
  uint64_t content_hash;
  char name[96];
};

// Version 3 adds the album. Its record is 32 bytes; version 2 (no album, 28
// bytes) fails the header version check and is rebuilt, never misread.
struct IndexRecord {
  int32_t item_id;
  int32_t duration_ms;
  uint32_t title_offset;
  uint32_t artist_offset;
  uint32_t album_offset;
  uint32_t key_offset;
  uint16_t title_length;
  uint16_t artist_length;
  uint16_t album_length;
  uint16_t key_length;
};
static_assert(sizeof(IndexRecord) == 32);

struct SearchEntry {
  uint32_t queue_row;
  uint16_t key_length;
};

struct ActiveMarker {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  int32_t playlist_id;
  uint32_t count;
  char name[96];
};
#pragma pack(pop)

uint16_t read_u16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

uint32_t read_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t read_i32(const uint8_t *p) { return static_cast<int32_t>(read_u32(p)); }

bool read_exact(FILE *file, void *data, size_t size) {
  return size == 0 || std::fread(data, 1, size, file) == size;
}

bool write_exact(FILE *file, const void *data, size_t size) {
  return size == 0 || std::fwrite(data, 1, size, file) == size;
}

bool hash_file(FILE *file, uint64_t &hash) {
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    return false;
  }
  hash = kFnvOffset;
  std::vector<uint8_t> buffer(kCopyBufferBytes);
  while (true) {
    const size_t count = std::fread(buffer.data(), 1, buffer.size(), file);
    for (size_t i = 0; i < count; ++i) {
      hash ^= buffer[i];
      hash *= kFnvPrime;
    }
    if (count < buffer.size()) {
      return std::feof(file) != 0;
    }
    vTaskDelay(1);
  }
}

class ByteReader {
public:
  ByteReader(FILE *file, size_t remaining)
      : file_(file), remaining_(remaining), buffer_(kCopyBufferBytes) {}

  bool byte(uint8_t &value) {
    if (cursor_ == available_) {
      if (remaining_ == 0) {
        return false;
      }
      available_ = std::fread(buffer_.data(), 1, std::min(buffer_.size(), remaining_), file_);
      cursor_ = 0;
      if (available_ == 0) {
        return false;
      }
      remaining_ -= available_;
    }
    value = buffer_[cursor_++];
    return true;
  }

  bool done() const { return remaining_ == 0 && cursor_ == available_; }

private:
  FILE *file_;
  size_t remaining_;
  // A 4 KiB member would otherwise be embedded in the ByteReader stack
  // object, on top of the LZ4/std::function/FAT call chain.
  std::vector<uint8_t> buffer_;
  size_t cursor_{0};
  size_t available_{0};
};

// A stdio buffer for one of the build's sequential files. Internal,
// DMA-capable memory lets the SD driver move whole multi-sector runs; a PSRAM
// buffer is bounced through 512 bytes at a time, which made the rows pass
// write at about 80 KB/s. Falls back to stdio's own buffer when internal
// memory is short.
class BuildFileBuffer {
public:
  BuildFileBuffer() {
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= kInternalReserve + kBuildFileBuffer) {
      data_ = static_cast<char *>(
          heap_caps_malloc(kBuildFileBuffer, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
  }
  ~BuildFileBuffer() { heap_caps_free(data_); }
  BuildFileBuffer(const BuildFileBuffer &) = delete;
  BuildFileBuffer &operator=(const BuildFileBuffer &) = delete;

  // The file must be closed before this buffer is destroyed.
  void attach(FILE *file) const {
    std::setvbuf(file, data_, _IOFBF, kBuildFileBuffer);
  }
  char *data() const { return data_; }

private:
  static constexpr size_t kInternalReserve = 64 * 1024;
  char *data_{nullptr};
};

// Receives each decoded chunk in order; returning false aborts the decode.
using DecodeSink = std::function<bool(const uint8_t *data, size_t size)>;

// Identity of a decoded playlist response without writing it anywhere:
// FNV-1a over every byte except the playlist ID (offsets 4-7) and flags byte
// (offset 10), plus the header fields needed to compare it with INDEX.DAT. A
// request for the current playlist (-1) and one for its explicit ID differ
// only in those bytes, yet compress to different frames.
class ContentDigest {
public:
  bool add(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i, ++position_) {
      const uint8_t value = data[i];
      if (position_ < prefix_.size()) {
        prefix_[position_] = value;
      }
      if (position_ >= 4 && position_ < 11) {
        continue;
      }
      hash_ ^= value;
      hash_ *= kFnvPrime;
    }
    return true;
  }

  uint64_t hash() const { return hash_; }
  // Whether the count, ID, and name (or as much of it as is kept) are known.
  bool header_known() const {
    if (position_ < 11) {
      return false;
    }
    const uint64_t name_end = 11U + read_u16(prefix_.data() + 8);
    return position_ >= std::min<uint64_t>(name_end, prefix_.size());
  }
  // Valid once header_known().
  uint32_t count() const { return read_u32(prefix_.data()); }
  int32_t playlist_id() const { return read_i32(prefix_.data() + 4); }
  // The name when it fits the INDEX.DAT header field, otherwise empty with
  // `fits` false: a longer name can never be compared with a stored one.
  std::string name(bool &fits) const {
    const uint16_t length = read_u16(prefix_.data() + 8);
    fits = position_ >= 11 && length < sizeof(IndexHeader::name) &&
           11U + length <= std::min<uint64_t>(position_, prefix_.size());
    return fits ? std::string(reinterpret_cast<const char *>(prefix_.data() + 11), length)
                : std::string{};
  }

private:
  uint64_t hash_{kFnvOffset};
  uint64_t position_{0};
  std::array<uint8_t, 11 + sizeof(IndexHeader::name)> prefix_{};
};

bool decompress_lz4(FILE *source, size_t compressed_size, uint32_t expected_size,
                    const DecodeSink &sink) {
  ByteReader input(source, compressed_size);
  // This must live in the external-capable heap, not on the 12 KiB index-task
  // stack. It is the only full LZ4 history window retained in RAM.
  std::vector<uint8_t> history(kHistoryBytes);
  std::vector<uint8_t> output_buffer(kCopyBufferBytes);
  size_t buffered = 0;
  size_t output = 0;

  auto emit = [&](uint8_t value) {
    if (output >= expected_size) {
      return false;
    }
    history[output % history.size()] = value;
    output_buffer[buffered++] = value;
    ++output;
    if (buffered == output_buffer.size()) {
      if (!sink(output_buffer.data(), buffered)) {
        return false;
      }
      buffered = 0;
      vTaskDelay(1);
    }
    return true;
  };

  while (!input.done()) {
    uint8_t token = 0;
    if (!input.byte(token)) {
      return false;
    }
    size_t literal_length = token >> 4;
    if (literal_length == 15) {
      uint8_t extension = 255;
      while (extension == 255) {
        if (!input.byte(extension)) {
          return false;
        }
        literal_length += extension;
      }
    }
    for (size_t i = 0; i < literal_length; ++i) {
      uint8_t value = 0;
      if (!input.byte(value) || !emit(value)) {
        return false;
      }
    }
    if (input.done()) {
      break;
    }
    uint8_t lo = 0;
    uint8_t hi = 0;
    if (!input.byte(lo) || !input.byte(hi)) {
      return false;
    }
    const size_t distance = static_cast<size_t>(lo) | (static_cast<size_t>(hi) << 8);
    if (distance == 0 || distance > output || distance > history.size()) {
      return false;
    }
    size_t match_length = token & 0x0f;
    if (match_length == 15) {
      uint8_t extension = 255;
      while (extension == 255) {
        if (!input.byte(extension)) {
          return false;
        }
        match_length += extension;
      }
    }
    match_length += 4;
    for (size_t i = 0; i < match_length; ++i) {
      const uint8_t value = history[(output - distance) % history.size()];
      if (!emit(value)) {
        return false;
      }
    }
  }
  return output == expected_size && (buffered == 0 || sink(output_buffer.data(), buffered));
}

// The search key is `title<TAB>artist<TAB>album`, ASCII case-folded and
// capped at kMaxStoredField. Title comes first so A-Z prefix filters still
// match on the title alone.
std::string normalize(std::string_view title, std::string_view artist,
                      std::string_view album) {
  std::string result;
  result.reserve(std::min<size_t>(kMaxStoredField,
                                  title.size() + artist.size() + album.size() + 2));
  auto append = [&](std::string_view text) {
    for (const unsigned char value : text) {
      if (result.size() >= kMaxStoredField) {
        break;
      }
      result.push_back(value >= 'A' && value <= 'Z' ? static_cast<char>(value + 32)
                                                   : static_cast<char>(value));
    }
  };
  append(title);
  for (const std::string_view field : {artist, album}) {
    if (result.size() < kMaxStoredField) {
      result.push_back('\t');
    }
    append(field);
  }
  return result;
}

std::string normalize_query(std::string_view query) {
  std::string result;
  result.reserve(std::min<size_t>(kMaxStoredField, query.size()));
  for (const unsigned char value : query) {
    if (result.size() >= kMaxStoredField) {
      break;
    }
    result.push_back(value >= 'A' && value <= 'Z' ? static_cast<char>(value + 32)
                                                  : static_cast<char>(value));
  }
  return result;
}

bool read_field(FILE *file, uint16_t wire_length, std::string &value) {
  const size_t stored = std::min<size_t>(wire_length, kMaxStoredField);
  value.resize(stored);
  if (!read_exact(file, value.data(), stored)) {
    return false;
  }
  if (wire_length > stored && std::fseek(file, wire_length - stored, SEEK_CUR) != 0) {
    return false;
  }
  uint8_t terminator = 0;
  return read_exact(file, &terminator, 1) && terminator == 0;
}

bool skip_field(FILE *file, uint16_t wire_length) {
  std::array<uint8_t, 256> buffer{};
  size_t remaining = wire_length;
  while (remaining != 0) {
    const size_t chunk = std::min(remaining, buffer.size());
    if (!read_exact(file, buffer.data(), chunk)) {
      return false;
    }
    remaining -= chunk;
  }
  uint8_t terminator = 0;
  return read_exact(file, &terminator, 1) && terminator == 0;
}

bool append_string(FILE *file, std::string_view value, uint32_t &offset, uint16_t &length) {
  const long position = std::ftell(file);
  if (position < 0 || static_cast<unsigned long>(position) > UINT32_MAX) {
    return false;
  }
  offset = static_cast<uint32_t>(position);
  length = static_cast<uint16_t>(value.size());
  return write_exact(file, value.data(), value.size());
}

bool file_exists(const std::string &path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool valid_header(const IndexHeader &header) {
  return header.magic == kIndexMagic && header.version == kIndexVersion &&
         header.header_size == sizeof(IndexHeader) &&
         header.records_offset == sizeof(IndexHeader) &&
         static_cast<uint64_t>(header.strings_offset) ==
             sizeof(IndexHeader) +
                 static_cast<uint64_t>(header.count) * sizeof(IndexRecord) &&
         static_cast<uint64_t>(header.search_offset) ==
             static_cast<uint64_t>(header.strings_offset) + header.strings_size;
}

bool valid_index_file(const std::string &path) {
  FILE *file = std::fopen(path.c_str(), "rb");
  IndexHeader header{};
  struct stat file_stat {};
  const bool valid =
      file != nullptr && read_exact(file, &header, sizeof(header)) && valid_header(header) &&
      fstat(fileno(file), &file_stat) == 0 && file_stat.st_size >= 0 &&
      static_cast<uint64_t>(header.search_offset) + header.search_size ==
          static_cast<uint64_t>(file_stat.st_size);
  if (file != nullptr) {
    std::fclose(file);
  }
  return valid;
}

bool load_track(FILE *file, const IndexHeader &header, uint32_t row, Track &track) {
  if (row >= header.count) {
    return false;
  }
  IndexRecord record{};
  const uint64_t record_position =
      static_cast<uint64_t>(header.records_offset) +
      static_cast<uint64_t>(row) * sizeof(IndexRecord);
  if (record_position > LONG_MAX ||
      std::fseek(file, static_cast<long>(record_position), SEEK_SET) != 0 ||
      !read_exact(file, &record, sizeof(record))) {
    return false;
  }
  auto load_string = [&](uint32_t offset, uint16_t length, std::string &value) {
    const uint64_t position = static_cast<uint64_t>(header.strings_offset) + offset;
    // The builder never stores a field longer than kMaxStoredField. Reject a
    // corrupt record before it can size a 64 KiB allocation per field.
    if (length > kMaxStoredField ||
        static_cast<uint64_t>(offset) + length > header.strings_size || position > LONG_MAX ||
        std::fseek(file, static_cast<long>(position), SEEK_SET) != 0) {
      return false;
    }
    value.resize(length);
    return read_exact(file, value.data(), length);
  };
  track = {};
  track.index = record.item_id;
  track.queue_row = static_cast<int32_t>(row);
  track.duration_ms = record.duration_ms;
  return load_string(record.title_offset, record.title_length, track.title) &&
         load_string(record.artist_offset, record.artist_length, track.artist) &&
         load_string(record.album_offset, record.album_length, track.album);
}

// Loads consecutive rows with two reads: the fixed records, then the string
// span they reference (title, artist, and album of neighbouring rows are
// adjacent). Each seek in a large FAT file walks its cluster chain, so the
// per-row path took about 2 s for a 100-row page on microSD. Falls back to
// per-row reads when the span is unexpectedly large.
bool load_tracks(FILE *file, const IndexHeader &header, uint32_t start, uint32_t count,
                 std::vector<Track> &tracks) {
  constexpr uint64_t kMaxBulkSpan = 128 * 1024;
  std::vector<IndexRecord> records(count);
  const uint64_t record_position = static_cast<uint64_t>(header.records_offset) +
                                   static_cast<uint64_t>(start) * sizeof(IndexRecord);
  if (count == 0) {
    return true;
  }
  if (start >= header.count || count > header.count - start || record_position > LONG_MAX ||
      std::fseek(file, static_cast<long>(record_position), SEEK_SET) != 0 ||
      !read_exact(file, records.data(), records.size() * sizeof(IndexRecord))) {
    return false;
  }
  uint64_t span_begin = UINT64_MAX;
  uint64_t span_end = 0;
  for (const IndexRecord &record : records) {
    for (const auto &[offset, length] : {std::pair{record.title_offset, record.title_length},
                                        std::pair{record.artist_offset, record.artist_length},
                                        std::pair{record.album_offset, record.album_length}}) {
      // The builder never stores a field longer than kMaxStoredField. Reject
      // a corrupt record before it can size an allocation.
      if (length > kMaxStoredField ||
          static_cast<uint64_t>(offset) + length > header.strings_size) {
        return false;
      }
      span_begin = std::min<uint64_t>(span_begin, offset);
      span_end = std::max<uint64_t>(span_end, static_cast<uint64_t>(offset) + length);
    }
  }
  if (span_end - span_begin > kMaxBulkSpan) {
    for (uint32_t i = 0; i < count; ++i) {
      Track track;
      if (!load_track(file, header, start + i, track)) {
        return false;
      }
      tracks.push_back(std::move(track));
    }
    return true;
  }
  std::vector<char> span(static_cast<size_t>(span_end - span_begin));
  const uint64_t span_position = static_cast<uint64_t>(header.strings_offset) + span_begin;
  if (span_position > LONG_MAX ||
      std::fseek(file, static_cast<long>(span_position), SEEK_SET) != 0 ||
      !read_exact(file, span.data(), span.size())) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const IndexRecord &record = records[i];
    Track track;
    track.index = record.item_id;
    track.queue_row = static_cast<int32_t>(start + i);
    track.duration_ms = record.duration_ms;
    track.title.assign(span.data() + (record.title_offset - span_begin), record.title_length);
    track.artist.assign(span.data() + (record.artist_offset - span_begin), record.artist_length);
    track.album.assign(span.data() + (record.album_offset - span_begin), record.album_length);
    tracks.push_back(std::move(track));
  }
  return true;
}

} // namespace

PlaylistIndex::PlaylistIndex(std::string directory, bool internal_flash)
    : directory_(std::move(directory)), internal_flash_(internal_flash) {
  if (!directory_.empty()) {
    mkdir(directory_.c_str(), 0755);
  }
  incoming_path_ = path("WIRE.TMP");
  const bool recovered = recover_index();
  if (internal_flash_ && recovered) {
    // Treat an index recovered at boot as recently written. This prevents a
    // reconnect loop from repeatedly erasing the internal partition before
    // the user has had a chance to select microSD storage instead.
    last_internal_promotion_us_ = esp_timer_get_time();
  }
}

bool PlaylistIndex::available() const { return !directory_.empty(); }

const std::string &PlaylistIndex::incoming_path() const { return incoming_path_; }

std::string PlaylistIndex::path(const char *name) const {
  return directory_.empty() ? std::string{} : directory_ + "/" + name;
}

bool PlaylistIndex::recover_index() const {
  if (directory_.empty()) {
    return false;
  }
  // Construction calls recovery before the network/index tasks start. Remove
  // staging files left by a reset so they cannot consume the free space needed
  // by the next build.
  for (const char *name : {"WIRE.TMP", "RAW.TMP", "REC.TMP", "STR.TMP", "SEA.TMP",
                           "INDEX.TMP", "ACTIVE.TMP"}) {
    unlink(path(name).c_str());
  }
  const std::string final = path("INDEX.DAT");
  const std::string backup = path("INDEX.BAK");
  const bool final_valid = valid_index_file(final);
  const bool backup_valid = valid_index_file(backup);
  if (!final_valid && backup_valid) {
    unlink(final.c_str());
    return rename(backup.c_str(), final.c_str()) == 0;
  }
  if (final_valid) {
    // A reset after the new index was promoted can leave the old backup in
    // place. Once the promoted header and extent are valid, it is safe to
    // reclaim that backup.
    unlink(backup.c_str());
  } else {
    unlink(path("ACTIVE.DAT").c_str());
  }
  return final_valid;
}

bool PlaylistIndex::load_active_identity(ActiveIdentity &identity) const {
  identity = {};
  std::vector<Track> unused;
  uint32_t index_count = 0;
  int32_t index_playlist_id = -1;
  std::string index_name;
  if (!load_page(0, 0, unused, index_count, index_playlist_id, index_name)) {
    return false;
  }
  FILE *file = std::fopen(path("ACTIVE.DAT").c_str(), "rb");
  ActiveMarker marker{};
  struct stat file_stat {};
  const bool valid = file != nullptr && read_exact(file, &marker, sizeof(marker)) &&
                     fstat(fileno(file), &file_stat) == 0 &&
                     file_stat.st_size == static_cast<off_t>(sizeof(marker)) &&
                     marker.magic == kActiveMagic && marker.version == kActiveVersion &&
                     marker.size == sizeof(marker) && marker.playlist_id >= 0 &&
                     marker.count == index_count &&
                     (index_playlist_id < 0 || marker.playlist_id == index_playlist_id) &&
                     strnlen(marker.name, sizeof(marker.name)) < sizeof(marker.name) &&
                     index_name == marker.name;
  if (file != nullptr) {
    std::fclose(file);
  }
  if (!valid) {
    unlink(path("ACTIVE.DAT").c_str());
    return false;
  }
  identity.playlist_id = marker.playlist_id;
  identity.count = marker.count;
  identity.name = marker.name;
  return true;
}

bool PlaylistIndex::mark_active(int32_t resolved_playlist_id) const {
  if (resolved_playlist_id < 0) {
    return false;
  }
  std::vector<Track> unused;
  uint32_t count = 0;
  int32_t index_playlist_id = -1;
  std::string name;
  if (!load_page(0, 0, unused, count, index_playlist_id, name) ||
      (index_playlist_id >= 0 && index_playlist_id != resolved_playlist_id)) {
    return false;
  }
  ActiveMarker marker{.magic = kActiveMagic,
                      .version = kActiveVersion,
                      .size = sizeof(ActiveMarker),
                      .playlist_id = resolved_playlist_id,
                      .count = count,
                      .name = {}};
  std::snprintf(marker.name, sizeof(marker.name), "%s", name.c_str());
  const std::string temporary = path("ACTIVE.TMP");
  FILE *file = std::fopen(temporary.c_str(), "wb");
  const bool written = file != nullptr && write_exact(file, &marker, sizeof(marker)) &&
                       std::fflush(file) == 0 && fsync(fileno(file)) == 0;
  const bool closed = file != nullptr && std::fclose(file) == 0;
  if (!written || !closed || rename(temporary.c_str(), path("ACTIVE.DAT").c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  return true;
}

void PlaylistIndex::clear_active() const {
  unlink(path("ACTIVE.TMP").c_str());
  unlink(path("ACTIVE.DAT").c_str());
}

void PlaylistIndex::mark_current_unusable() const { current_unusable_ = true; }

PlaylistIndex::BuildResult PlaylistIndex::build_from_compressed(
    const Progress &progress, std::optional<uint64_t> known_source_hash) {
  BuildResult result;
  // Declared first: every file using them is closed before they are freed.
  const BuildFileBuffer raw_buffer;
  const BuildFileBuffer index_buffer;
  const BuildFileBuffer search_buffer;
  if (!available()) {
    result.error = "NO STORAGE";
    return result;
  }
  const std::string raw_path = path("RAW.TMP");
  const std::string index_path = path("INDEX.TMP");
  FILE *wire = std::fopen(incoming_path_.c_str(), "rb");
  if (wire == nullptr) {
    result.error = "WIRE FILE MISSING";
    return result;
  }
  // INDEX.TMP is opened only after WIRE.TMP is closed; they share a buffer.
  index_buffer.attach(wire);
  std::array<uint8_t, 10> envelope{};
  struct stat wire_stat {};
  if (!read_exact(wire, envelope.data(), envelope.size()) ||
      fstat(fileno(wire), &wire_stat) != 0 || wire_stat.st_size < 10) {
    std::fclose(wire);
    result.error = "BAD COMPRESSED HEADER";
    return result;
  }
  const uint32_t decoded_size = read_u32(envelope.data());
  const uint32_t inner_type = read_u32(envelope.data() + 4);
  if (decoded_size == 0 || decoded_size > kMaxDecodedBytes ||
      (inner_type != 9 && inner_type != 31 && inner_type != 50)) {
    std::fclose(wire);
    result.error = "UNSUPPORTED LARGE FRAME";
    return result;
  }
  const auto elapsed_ms = [](int64_t since_us) {
    return static_cast<uint32_t>((esp_timer_get_time() - since_us) / 1000);
  };
  const int64_t hash_started_us = esp_timer_get_time();
  uint64_t source_hash = known_source_hash.value_or(0);
  const bool hashed = known_source_hash.has_value() || hash_file(wire, source_hash);
  result.source_hash_ms = elapsed_ms(hash_started_us);
  if (!hashed || std::fseek(wire, envelope.size(), SEEK_SET) != 0) {
    std::fclose(wire);
    result.error = "STORAGE READ FAILED";
    return result;
  }
  const uint32_t source_size = static_cast<uint32_t>(wire_stat.st_size);
  IndexHeader current_header{};
  bool valid_current = false;
  {
    FILE *current = std::fopen(path("INDEX.DAT").c_str(), "rb");
    IndexHeader &header = current_header;
    struct stat current_stat {};
    valid_current =
        !current_unusable_.load() && current != nullptr &&
        read_exact(current, &header, sizeof(header)) &&
        valid_header(header) && fstat(fileno(current), &current_stat) == 0 &&
        static_cast<uint64_t>(header.search_offset) + header.search_size ==
            static_cast<uint64_t>(current_stat.st_size);
    if (current != nullptr) {
      std::fclose(current);
    }
    if (valid_current && header.source_size == source_size &&
        header.source_hash == source_hash) {
      std::fclose(wire);
      result.success = true;
      result.reused = Reuse::source_hash;
      result.playlist_id = header.playlist_id;
      result.count = header.count;
      result.name.assign(header.name, strnlen(header.name, sizeof(header.name)));
      return result;
    }
  }
  // The compressed frame differs, but a response for the current playlist
  // (-1) and one for its explicit ID describe the same rows. Before any
  // storage write, space reservation, or wear throttle, decode the frame into
  // a digest only and reuse the index when its content, name, and count match.
  // A `-1` response never reuses an index stored under an explicit ID: it does
  // not show that this ID is still the active playlist, and the stored ID is
  // published as the active one. A stored `-1` makes no ID claim; the client
  // resolves it from its active playlist or a unique name and count.
  const std::string stored_name(current_header.name,
                                strnlen(current_header.name, sizeof(current_header.name)));
  if (valid_current) {
    const int64_t check_started_us = esp_timer_get_time();
    ContentDigest digest;
    bool incompatible = false;
    const auto compare = [&](const uint8_t *data, size_t size) {
      const bool header_was_known = digest.header_known();
      digest.add(data, size);
      if (!header_was_known && digest.header_known()) {
        bool name_fits = false;
        const std::string name = digest.name(name_fits);
        const bool compatible_id = current_header.playlist_id < 0 ||
                                   current_header.playlist_id == digest.playlist_id();
        incompatible = !compatible_id || !name_fits || name != stored_name ||
                       digest.count() != current_header.count;
      }
      return !incompatible;
    };
    const bool decoded =
        decompress_lz4(wire, static_cast<size_t>(wire_stat.st_size) - 10, decoded_size, compare);
    result.content_check_ms = elapsed_ms(check_started_us);
    if (decoded && !incompatible && digest.header_known() &&
        digest.hash() == current_header.content_hash) {
      std::fclose(wire);
      result.success = true;
      result.reused = Reuse::content_hash;
      result.playlist_id = digest.playlist_id();
      result.count = current_header.count;
      result.name = stored_name;
      return result;
    }
    if (std::ferror(wire) != 0 || std::fseek(wire, envelope.size(), SEEK_SET) != 0) {
      std::fclose(wire);
      result.error = "STORAGE READ FAILED";
      return result;
    }
  }
  {
    // A confirmed rebuild on internal flash is throttled to bound erase wear.
    if (internal_flash_ && valid_current && last_internal_promotion_us_ != 0 &&
        esp_timer_get_time() - last_internal_promotion_us_ <
            kInternalFlashRebuildIntervalUs) {
      std::fclose(wire);
      result.success = true;
      result.stale = true;
      result.playlist_id = current_header.playlist_id;
      result.count = current_header.count;
      result.name = stored_name;
      return result;
    }
  }
  // WIRE.TMP already counts as used space. The single-file builder peaks at
  // RAW.TMP plus INDEX.TMP while preserving the previously promoted index.
  // The index stores title, artist, and album text three times (fields, key,
  // and search area) plus 42 bytes per row, which stays below three decoded
  // sizes because each decoded row also carries a 36-byte base and its genre.
  // Four decoded sizes plus 256 KiB covers RAW.TMP, INDEX.TMP, filesystem
  // metadata, and unusually short source fields.
  const uint64_t needed = internal_flash_
                              ? static_cast<uint64_t>(decoded_size) * 4 + 256 * 1024
                              : static_cast<uint64_t>(decoded_size) * 6 + 1024 * 1024;
  uint64_t free_bytes = 0;
  esp_err_t capacity_status = ESP_FAIL;
  if (internal_flash_) {
    size_t total = 0;
    size_t used = 0;
    capacity_status = esp_spiffs_info("storage", &total, &used);
    free_bytes = total >= used ? total - used : 0;
  } else {
    const size_t slash = directory_.find('/', 1);
    const std::string mount_point = directory_.substr(0, slash);
    uint64_t total_bytes = 0;
    capacity_status = esp_vfs_fat_info(mount_point.c_str(), &total_bytes, &free_bytes);
  }
  if (capacity_status != ESP_OK) {
    std::fclose(wire);
    result.error = "STORAGE UNAVAILABLE";
    return result;
  }
  if (free_bytes < needed) {
    std::fclose(wire);
    result.error = internal_flash_ ? "FLASH INDEX TOO LARGE" : "SD CARD FULL";
    return result;
  }
  const int64_t rebuild_started_us = esp_timer_get_time();
  FILE *raw = std::fopen(raw_path.c_str(), "wb");
  if (raw == nullptr) {
    std::fclose(wire);
    result.error = "INDEX FILE OPEN FAILED";
    return result;
  }
  raw_buffer.attach(raw);
  ContentDigest digest;
  uint64_t decoded_bytes = 0;
  const bool decoded = decompress_lz4(
      wire, static_cast<size_t>(wire_stat.st_size) - 10, decoded_size,
      [&](const uint8_t *data, size_t size) {
        decoded_bytes += size;
        if (progress && (decoded_bytes % (64 * 1024)) < size) {
          progress(static_cast<uint32_t>(decoded_bytes * kDecodeWeight / decoded_size),
                   kProgressTotal);
        }
        return digest.add(data, size) && write_exact(raw, data, size);
      });
  result.decode_ms = elapsed_ms(rebuild_started_us);
  const uint64_t content_hash = digest.hash();
  bool read_failed = std::ferror(wire) != 0;
  bool write_failed = std::ferror(raw) != 0 || std::fflush(raw) != 0;
  read_failed = std::fclose(wire) != 0 || read_failed;
  write_failed = std::fclose(raw) != 0 || write_failed;
  if (!decoded || read_failed || write_failed) {
    unlink(raw_path.c_str());
    result.error = write_failed ? "STORAGE WRITE FAILED"
                   : read_failed ? "STORAGE READ FAILED"
                                 : "LZ4 DECODE FAILED";
    return result;
  }

  // The compressed staging file is no longer needed once RAW.TMP is durable.
  // Reclaim it before constructing the index, which is essential on the 4 MB
  // internal partition.
  unlink(incoming_path_.c_str());
  raw = std::fopen(raw_path.c_str(), "rb");
  FILE *index = nullptr;
  FILE *search = nullptr;
  const std::string search_path = path("SEA.TMP");
  int64_t phase_started_us = esp_timer_get_time();
  std::array<uint8_t, 11> playlist_header{};
  if (raw == nullptr || !read_exact(raw, playlist_header.data(), playlist_header.size())) {
    result.error = "INDEX FILE OPEN FAILED";
    goto cleanup;
  }
  raw_buffer.attach(raw);
  result.count = read_u32(playlist_header.data());
  result.playlist_id = read_i32(playlist_header.data() + 4);
  {
    const uint16_t name_length = read_u16(playlist_header.data() + 8);
    if (name_length > kMaxStoredField || !read_field(raw, name_length, result.name)) {
      result.error = "BAD PLAYLIST NAME";
      goto cleanup;
    }
  }
  index = std::fopen(index_path.c_str(), "w+b");
  // Search entries are written sequentially to their own file during the row
  // pass, then appended once. Gathering them from INDEX.TMP afterwards took
  // 48,321 seeks in a large FAT file (about 58 s for NAS:Music).
  search = std::fopen(search_path.c_str(), "w+b");
  if (index == nullptr || search == nullptr) {
    result.error = "INDEX FILE OPEN FAILED";
    goto cleanup;
  }
  index_buffer.attach(index);
  search_buffer.attach(search);
  {
    // Every wire row needs a 32-byte base and four field terminators. Bound the
    // untrusted count by the decoded bytes left before reserving its table.
    constexpr uint64_t kMinWireRowBytes = 32 + 4;
    const long records_start = std::ftell(raw);
    const uint64_t strings_offset =
        sizeof(IndexHeader) + static_cast<uint64_t>(result.count) * sizeof(IndexRecord);
    if (records_start < 0 || static_cast<uint64_t>(records_start) > decoded_size ||
        result.count > (decoded_size - static_cast<uint64_t>(records_start)) / kMinWireRowBytes ||
        strings_offset > UINT32_MAX || strings_offset > LONG_MAX) {
      result.error = "INDEX SIZE FAILED";
      goto cleanup;
    }
    IndexHeader header{.magic = kIndexMagic,
                       .version = kIndexVersion,
                       .header_size = sizeof(IndexHeader),
                       .playlist_id = result.playlist_id,
                       .count = result.count,
                       .records_offset = sizeof(IndexHeader),
                       .strings_offset = static_cast<uint32_t>(strings_offset),
                       .strings_size = 0,
                       .search_offset = 0,
                       .search_size = 0,
                       .source_size = source_size,
                       .source_hash = source_hash,
                       .content_hash = content_hash,
                       .name = {}};
    std::snprintf(header.name, sizeof(header.name), "%s", result.name.c_str());
    if (!write_exact(index, &header, sizeof(header))) {
      result.error = "STORAGE WRITE FAILED";
      goto cleanup;
    }
    std::vector<uint8_t> zeroes(kCopyBufferBytes, 0);
    uint64_t reserved = strings_offset - sizeof(IndexHeader);
    while (reserved != 0) {
      const size_t chunk = std::min<uint64_t>(reserved, zeroes.size());
      if (!write_exact(index, zeroes.data(), chunk)) {
        result.error = "STORAGE WRITE FAILED";
        goto cleanup;
      }
      reserved -= chunk;
      vTaskDelay(1);
    }

    // Each flush seeks back to the record table and forward to the end.
    constexpr uint32_t kRecordBatch = 1024;
    std::vector<IndexRecord> record_batch;
    record_batch.reserve(kRecordBatch);
    uint32_t batch_start = 0;
    uint64_t search_bytes = 0;
    auto flush_records = [&]() {
      if (record_batch.empty()) {
        return true;
      }
      const long string_end = std::ftell(index);
      const uint64_t position = static_cast<uint64_t>(header.records_offset) +
                                static_cast<uint64_t>(batch_start) * sizeof(IndexRecord);
      const bool ok = string_end >= 0 && position <= LONG_MAX &&
                      std::fseek(index, static_cast<long>(position), SEEK_SET) == 0 &&
                      write_exact(index, record_batch.data(),
                                  record_batch.size() * sizeof(IndexRecord)) &&
                      std::fseek(index, string_end, SEEK_SET) == 0;
      batch_start += record_batch.size();
      record_batch.clear();
      return ok;
    };

  for (uint32_t row = 0; row < result.count; ++row) {
    std::array<uint8_t, 32> base{};
    if (!read_exact(raw, base.data(), base.size())) {
      result.error = "TRUNCATED PLAYLIST";
      goto cleanup;
    }
    const std::array<uint16_t, 4> lengths{read_u16(base.data() + 24),
                                          read_u16(base.data() + 26),
                                          read_u16(base.data() + 28),
                                          read_u16(base.data() + 30)};
    // Wire strings: title, artist, album, genre (docs/PROTOCOL.md). Genre is
    // not indexed: a genre word matches thousands of rows and would bury
    // title, artist, and album matches in an unranked result list.
    std::string title;
    std::string artist;
    std::string album;
    if (!read_field(raw, lengths[0], title) || !read_field(raw, lengths[1], artist) ||
        !read_field(raw, lengths[2], album) || !skip_field(raw, lengths[3])) {
      result.error = "TRUNCATED TRACK";
      goto cleanup;
    }
    IndexRecord record{};
    record.item_id = read_i32(base.data());
    record.duration_ms = read_i32(base.data() + 4);
    const std::string key = normalize(title, artist, album);
    if (!append_string(index, title, record.title_offset, record.title_length) ||
        !append_string(index, artist, record.artist_offset, record.artist_length) ||
        !append_string(index, album, record.album_offset, record.album_length) ||
        !append_string(index, key, record.key_offset, record.key_length) ||
        record.title_offset < header.strings_offset ||
        record.artist_offset < header.strings_offset ||
        record.album_offset < header.strings_offset ||
        record.key_offset < header.strings_offset) {
      result.error = "STORAGE WRITE FAILED";
      goto cleanup;
    }
    record.title_offset -= header.strings_offset;
    record.artist_offset -= header.strings_offset;
    record.album_offset -= header.strings_offset;
    record.key_offset -= header.strings_offset;
    record_batch.push_back(record);
    {
      const SearchEntry entry{.queue_row = row, .key_length = record.key_length};
      if (!write_exact(search, &entry, sizeof(entry)) ||
          !write_exact(search, key.data(), key.size())) {
        result.error = "STORAGE WRITE FAILED";
        goto cleanup;
      }
      search_bytes += sizeof(entry) + key.size();
    }
    if (record_batch.size() == kRecordBatch && !flush_records()) {
      result.error = "STORAGE WRITE FAILED";
      goto cleanup;
    }
    if (progress && (row % 512 == 0 || row + 1 == result.count)) {
      progress(kDecodeWeight + static_cast<uint32_t>(static_cast<uint64_t>(row + 1) *
                                                     kRowsWeight / result.count),
               kProgressTotal);
    }
    if ((row & 0xff) == 0) {
      vTaskDelay(1);
    }
  }
    if (!flush_records()) {
      result.error = "STORAGE WRITE FAILED";
      goto cleanup;
    }
  if (std::fgetc(raw) != EOF || std::ferror(raw) != 0) {
    result.error = "PLAYLIST TRAILING DATA";
    goto cleanup;
  }
    std::fclose(raw);
    raw = nullptr;
    unlink(raw_path.c_str());

    const long strings_end = std::ftell(index);
    if (strings_end < 0 || static_cast<uint64_t>(strings_end) < header.strings_offset ||
        static_cast<uint64_t>(strings_end) - header.strings_offset > UINT32_MAX) {
      result.error = "INDEX SIZE FAILED";
      goto cleanup;
    }
    header.strings_size = static_cast<uint32_t>(strings_end - header.strings_offset);
    header.search_offset = static_cast<uint32_t>(strings_end);

    result.rows_ms = elapsed_ms(phase_started_us);
    phase_started_us = esp_timer_get_time();
    uint64_t search_size = 0;
    {
      // RAW.TMP is closed by now, so its internal buffer carries the copy.
      std::vector<uint8_t> fallback;
      if (raw_buffer.data() == nullptr) {
        fallback.resize(kBuildFileBuffer);
      }
      uint8_t *const copy = raw_buffer.data() != nullptr
                                ? reinterpret_cast<uint8_t *>(raw_buffer.data())
                                : fallback.data();
      if (std::fflush(search) != 0 || std::fseek(search, 0, SEEK_SET) != 0 ||
          std::fseek(index, 0, SEEK_END) != 0) {
        result.error = "STORAGE WRITE FAILED";
        goto cleanup;
      }
      while (true) {
        const size_t count = std::fread(copy, 1, kBuildFileBuffer, search);
        if (count != 0 && !write_exact(index, copy, count)) {
          result.error = "STORAGE WRITE FAILED";
          goto cleanup;
        }
        search_size += count;
        if (progress && search_bytes != 0) {
          progress(kDecodeWeight + kRowsWeight +
                       static_cast<uint32_t>(std::min(search_size, search_bytes) *
                                             kSearchWeight / search_bytes),
                   kProgressTotal);
        }
        if (count < kBuildFileBuffer) {
          break;
        }
        vTaskDelay(1);
      }
      if (std::ferror(search) != 0 || search_size != search_bytes) {
        result.error = "STORAGE READ FAILED";
        goto cleanup;
      }
      std::fclose(search);
      search = nullptr;
      unlink(search_path.c_str());
    }
    result.search_ms = elapsed_ms(phase_started_us);
    phase_started_us = esp_timer_get_time();
    if (search_size > UINT32_MAX) {
      result.error = "INDEX SIZE FAILED";
      goto cleanup;
    }
    header.search_size = static_cast<uint32_t>(search_size);
    // Close the index even when finalization fails, so repeated storage
    // errors cannot exhaust the mount's open-file slots.
    const bool finalized = std::fseek(index, 0, SEEK_SET) == 0 &&
                           write_exact(index, &header, sizeof(header)) &&
                           std::fflush(index) == 0 && fsync(fileno(index)) == 0;
    const bool closed = std::fclose(index) == 0;
    index = nullptr;
    if (!finalized || !closed) {
      result.error = "STORAGE WRITE FAILED";
      goto cleanup;
    }
  }
  if (progress) {
    progress(kDecodeWeight + kRowsWeight + kSearchWeight, kProgressTotal);
  }
  {
    const std::string final = path("INDEX.DAT");
    const std::string backup = path("INDEX.BAK");
    unlink(backup.c_str());
    const bool had_final = file_exists(final);
    if ((had_final && rename(final.c_str(), backup.c_str()) != 0) ||
        rename(index_path.c_str(), final.c_str()) != 0) {
      if (had_final) {
        rename(backup.c_str(), final.c_str());
      }
      result.error = "INDEX PROMOTION FAILED";
      goto cleanup;
    }
    unlink(backup.c_str());
  }
  result.success = true;
  current_unusable_ = false;
  if (internal_flash_) {
    last_internal_promotion_us_ = esp_timer_get_time();
  }
  result.finalize_ms = elapsed_ms(phase_started_us);
  if (progress) {
    progress(kProgressTotal, kProgressTotal);
  }

cleanup:
  result.rebuild_ms = elapsed_ms(rebuild_started_us);
  if (raw != nullptr) std::fclose(raw);
  if (index != nullptr) std::fclose(index);
  if (search != nullptr) std::fclose(search);
  unlink(raw_path.c_str());
  unlink(path("SEA.TMP").c_str());
  if (!result.success) unlink(index_path.c_str());
  return result;
}

bool PlaylistIndex::load_page(uint32_t start, uint32_t limit, std::vector<Track> &tracks,
                              uint32_t &total, int32_t &playlist_id, std::string &name) const {
  tracks.clear();
  FILE *file = std::fopen(path("INDEX.DAT").c_str(), "rb");
  IndexHeader header{};
  struct stat file_stat {};
  if (file == nullptr || !read_exact(file, &header, sizeof(header)) || !valid_header(header) ||
      fstat(fileno(file), &file_stat) != 0 ||
      static_cast<uint64_t>(header.strings_offset) + header.strings_size >
          static_cast<uint64_t>(file_stat.st_size) ||
      static_cast<uint64_t>(header.search_offset) + header.search_size !=
          static_cast<uint64_t>(file_stat.st_size)) {
    if (file != nullptr) std::fclose(file);
    return false;
  }
  total = header.count;
  playlist_id = header.playlist_id;
  name.assign(header.name, strnlen(header.name, sizeof(header.name)));
  if (start >= total) {
    std::fclose(file);
    return true;
  }
  const uint32_t count = std::min(limit, total - start);
  tracks.reserve(count);
  if (!load_tracks(file, header, start, count, tracks)) {
    std::fclose(file);
    tracks.clear();
    return false;
  }
  std::fclose(file);
  return true;
}

bool PlaylistIndex::search_page(std::string_view query, SearchMode mode, uint32_t start,
                                uint32_t limit, std::vector<Track> &tracks,
                                uint32_t &total, int32_t &playlist_id,
                                std::string &name, const Progress &progress) const {
  tracks.clear();
  total = 0;
  // Declared before the file, which is closed before this buffer is freed.
  const BuildFileBuffer stream_buffer;
  FILE *file = std::fopen(path("INDEX.DAT").c_str(), "rb");
  if (file != nullptr) {
    stream_buffer.attach(file);
  }
  IndexHeader header{};
  struct stat file_stat {};
  if (file == nullptr || !read_exact(file, &header, sizeof(header)) || !valid_header(header) ||
      fstat(fileno(file), &file_stat) != 0 ||
      static_cast<uint64_t>(header.search_offset) + header.search_size !=
          static_cast<uint64_t>(file_stat.st_size) ||
      std::fseek(file, static_cast<long>(header.search_offset), SEEK_SET) != 0) {
    if (file != nullptr) std::fclose(file);
    return false;
  }
  playlist_id = header.playlist_id;
  name.assign(header.name, strnlen(header.name, sizeof(header.name)));
  const std::string needle = normalize_query(query);
  if (mode != SearchMode::non_alpha && needle.empty()) {
    std::fclose(file);
    return false;
  }
  // The search area is scanned sequentially through one large buffer, with
  // no allocation per entry.
  // The stream's 16 KiB internal DMA buffer takes multi-sector SD reads; the
  // scan copies out of it in small chunks. Measured on the board: reading
  // 16 KiB at a time straight into PSRAM took about 5 s for the 48k-row area,
  // and an unbuffered stream 81 s.
  constexpr size_t kScanBuffer = 4 * 1024;
  std::vector<uint8_t> scan_buffer(kScanBuffer);
  uint8_t *const buffer = scan_buffer.data();
  size_t buffered = 0;
  size_t cursor = 0;
  uint32_t remaining = header.search_size;
  const auto take = [&](void *out, size_t size) {
    auto *target = static_cast<uint8_t *>(out);
    while (size != 0) {
      if (cursor == buffered) {
        const size_t want = std::min<size_t>(kScanBuffer, remaining);
        if (want == 0) {
          return false;
        }
        buffered = std::fread(buffer, 1, want, file);
        cursor = 0;
        if (buffered == 0) {
          return false;
        }
        remaining -= static_cast<uint32_t>(buffered);
      }
      const size_t chunk = std::min(size, buffered - cursor);
      std::memcpy(target, buffer + cursor, chunk);
      cursor += chunk;
      target += chunk;
      size -= chunk;
    }
    return true;
  };
  std::vector<uint32_t> rows;
  rows.reserve(limit);
  std::string key;
  key.reserve(kMaxStoredField);
  uint32_t consumed = 0;
  uint32_t entries = 0;
  while (consumed < header.search_size) {
    SearchEntry entry{};
    if (header.search_size - consumed < sizeof(entry) || !take(&entry, sizeof(entry))) {
      std::fclose(file);
      return false;
    }
    consumed += sizeof(entry);
    if (entry.queue_row >= header.count || entry.key_length > kMaxStoredField ||
        entry.key_length > header.search_size - consumed) {
      std::fclose(file);
      return false;
    }
    key.resize(entry.key_length);
    if (!take(key.data(), key.size())) {
      std::fclose(file);
      return false;
    }
    consumed += entry.key_length;
    ++entries;
    bool matched = false;
    if (mode == SearchMode::contains) {
      matched = key.find(needle) != std::string::npos;
    } else if (mode == SearchMode::prefix) {
      matched = key.starts_with(needle);
    } else {
      matched = key.empty() || key[0] < 'a' || key[0] > 'z';
    }
    if (matched) {
      if (total >= start && rows.size() < limit) {
        rows.push_back(entry.queue_row);
      }
      ++total;
    }
    if ((entries & 0xfff) == 0) {
      if (progress) {
        progress(entries, header.count);
      }
      vTaskDelay(1);
    }
  }
  if (consumed != header.search_size || entries != header.count) {
    std::fclose(file);
    return false;
  }
  if (progress) {
    progress(entries, header.count);
  }
  // Matching rows ascend, and so do their records and strings: read every
  // record first, then every string, so each seek moves forward. A backward
  // seek in a large FAT file walks its cluster chain from the start.
  std::vector<IndexRecord> records(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const uint64_t position = static_cast<uint64_t>(header.records_offset) +
                              static_cast<uint64_t>(rows[i]) * sizeof(IndexRecord);
    if (position > LONG_MAX || std::fseek(file, static_cast<long>(position), SEEK_SET) != 0 ||
        !read_exact(file, &records[i], sizeof(IndexRecord))) {
      std::fclose(file);
      return false;
    }
  }
  tracks.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const IndexRecord &record = records[i];
    Track track;
    track.index = record.item_id;
    track.queue_row = static_cast<int32_t>(rows[i]);
    track.duration_ms = record.duration_ms;
    for (const auto &[offset, length, value] :
         {std::tuple{record.title_offset, record.title_length, &track.title},
          std::tuple{record.artist_offset, record.artist_length, &track.artist},
          std::tuple{record.album_offset, record.album_length, &track.album}}) {
      const uint64_t position = static_cast<uint64_t>(header.strings_offset) + offset;
      if (length > kMaxStoredField ||
          static_cast<uint64_t>(offset) + length > header.strings_size || position > LONG_MAX ||
          std::fseek(file, static_cast<long>(position), SEEK_SET) != 0) {
        std::fclose(file);
        tracks.clear();
        return false;
      }
      value->resize(length);
      if (!read_exact(file, value->data(), length)) {
        std::fclose(file);
        tracks.clear();
        return false;
      }
    }
    tracks.push_back(std::move(track));
  }
  std::fclose(file);
  return true;
}

} // namespace musicremote
