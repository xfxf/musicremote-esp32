#include "playlist_index.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace musicremote {
namespace {

constexpr uint32_t kIndexMagic = 0x5844494d; // MIDX
constexpr uint16_t kIndexVersion = 1;
constexpr uint32_t kMaxDecodedBytes = 64 * 1024 * 1024;
constexpr size_t kHistoryBytes = 64 * 1024;
constexpr size_t kCopyBufferBytes = 4096;
constexpr uint16_t kMaxStoredField = 2048;

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
  char name[96];
};

struct IndexRecord {
  int32_t item_id;
  int32_t duration_ms;
  uint32_t title_offset;
  uint32_t artist_offset;
  uint32_t key_offset;
  uint16_t title_length;
  uint16_t artist_length;
  uint16_t key_length;
  uint16_t reserved;
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

bool copy_file(FILE *source, FILE *target) {
  // Index building runs on the dedicated 12 KiB playlist_index task. Keep
  // bulk I/O buffers off its stack: FAT and stdio have a deep call chain.
  std::vector<uint8_t> buffer(kCopyBufferBytes);
  while (true) {
    const size_t count = std::fread(buffer.data(), 1, buffer.size(), source);
    if (count != 0 && !write_exact(target, buffer.data(), count)) {
      return false;
    }
    vTaskDelay(1);
    if (count < buffer.size()) {
      return std::feof(source) != 0;
    }
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

bool decompress_lz4(FILE *source, size_t compressed_size, FILE *target,
                    uint32_t expected_size) {
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
      if (!write_exact(target, output_buffer.data(), buffered)) {
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
  return output == expected_size && write_exact(target, output_buffer.data(), buffered) &&
         std::fflush(target) == 0;
}

std::string normalize(std::string_view title, std::string_view artist) {
  std::string result;
  result.reserve(std::min<size_t>(kMaxStoredField, title.size() + artist.size() + 1));
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
  if (result.size() < kMaxStoredField) {
    result.push_back('\t');
  }
  append(artist);
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

bool valid_header(const IndexHeader &header) {
  return header.magic == kIndexMagic && header.version == kIndexVersion &&
         header.header_size == sizeof(IndexHeader) &&
         header.records_offset == sizeof(IndexHeader) &&
         header.strings_offset == sizeof(IndexHeader) + header.count * sizeof(IndexRecord);
}

} // namespace

PlaylistIndex::PlaylistIndex(std::string directory) : directory_(std::move(directory)) {
  if (!directory_.empty()) {
    mkdir(directory_.c_str(), 0755);
  }
  incoming_path_ = path("WIRE.TMP");
  recover_index();
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
  const std::string final = path("INDEX.DAT");
  const std::string backup = path("INDEX.BAK");
  if (access(final.c_str(), F_OK) != 0 && access(backup.c_str(), F_OK) == 0) {
    return rename(backup.c_str(), final.c_str()) == 0;
  }
  return access(final.c_str(), F_OK) == 0;
}

PlaylistIndex::BuildResult PlaylistIndex::build_from_compressed(const Progress &progress) {
  BuildResult result;
  if (!available()) {
    result.error = "NO STORAGE";
    return result;
  }
  const std::string raw_path = path("RAW.TMP");
  const std::string records_path = path("REC.TMP");
  const std::string strings_path = path("STR.TMP");
  const std::string index_path = path("INDEX.TMP");
  FILE *wire = std::fopen(incoming_path_.c_str(), "rb");
  if (wire == nullptr) {
    result.error = "WIRE FILE MISSING";
    return result;
  }
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
  const uint64_t needed = static_cast<uint64_t>(decoded_size) * 5 + 1024 * 1024;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  const size_t slash = directory_.find('/', 1);
  const std::string mount_point = directory_.substr(0, slash);
  if (esp_vfs_fat_info(mount_point.c_str(), &total_bytes, &free_bytes) != ESP_OK) {
    std::fclose(wire);
    result.error = "STORAGE UNAVAILABLE";
    return result;
  }
  if (free_bytes < needed) {
    std::fclose(wire);
    result.error = "SD CARD FULL";
    return result;
  }
  FILE *raw = std::fopen(raw_path.c_str(), "wb");
  if (raw == nullptr) {
    std::fclose(wire);
    result.error = "INDEX FILE OPEN FAILED";
    return result;
  }
  const bool decoded = decompress_lz4(
      wire, static_cast<size_t>(wire_stat.st_size) - 10, raw, decoded_size);
  bool read_failed = std::ferror(wire) != 0;
  bool write_failed = std::ferror(raw) != 0;
  read_failed = std::fclose(wire) != 0 || read_failed;
  write_failed = std::fclose(raw) != 0 || write_failed;
  if (!decoded || read_failed || write_failed) {
    unlink(raw_path.c_str());
    result.error = write_failed ? "SD CARD WRITE FAILED"
                   : read_failed ? "SD CARD READ FAILED"
                                 : "LZ4 DECODE FAILED";
    return result;
  }

  raw = std::fopen(raw_path.c_str(), "rb");
  // These temporary streams are rewound and read back while assembling the
  // final index, so they must be opened for both writing and reading.
  FILE *records = std::fopen(records_path.c_str(), "w+b");
  FILE *strings = std::fopen(strings_path.c_str(), "w+b");
  std::array<uint8_t, 11> playlist_header{};
  if (raw == nullptr || records == nullptr || strings == nullptr ||
      !read_exact(raw, playlist_header.data(), playlist_header.size())) {
    result.error = "INDEX FILE OPEN FAILED";
    goto cleanup;
  }
  result.count = read_u32(playlist_header.data());
  result.playlist_id = read_i32(playlist_header.data() + 4);
  {
    const uint16_t name_length = read_u16(playlist_header.data() + 8);
    if (name_length > kMaxStoredField || !read_field(raw, name_length, result.name)) {
      result.error = "BAD PLAYLIST NAME";
      goto cleanup;
    }
  }
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
    std::string title;
    std::string artist;
    if (!read_field(raw, lengths[0], title) || !read_field(raw, lengths[1], artist) ||
        !skip_field(raw, lengths[2]) || !skip_field(raw, lengths[3])) {
      result.error = "TRUNCATED TRACK";
      goto cleanup;
    }
    IndexRecord record{};
    record.item_id = read_i32(base.data());
    record.duration_ms = read_i32(base.data() + 4);
    const std::string key = normalize(title, artist);
    if (!append_string(strings, title, record.title_offset, record.title_length) ||
        !append_string(strings, artist, record.artist_offset, record.artist_length) ||
        !append_string(strings, key, record.key_offset, record.key_length) ||
        !write_exact(records, &record, sizeof(record))) {
      result.error = "SD CARD WRITE FAILED";
      goto cleanup;
    }
    if (progress && (row % 512 == 0 || row + 1 == result.count)) {
      progress(row + 1, result.count);
    }
    if ((row & 0xff) == 0) {
      vTaskDelay(1);
    }
  }
  if (std::fgetc(raw) != EOF || std::ferror(raw) != 0) {
    result.error = "PLAYLIST TRAILING DATA";
    goto cleanup;
  }
  {
    const long strings_size = std::ftell(strings);
    if (strings_size < 0 || static_cast<unsigned long>(strings_size) > UINT32_MAX ||
        std::fflush(records) != 0 || std::fflush(strings) != 0) {
      result.error = "INDEX SIZE FAILED";
      goto cleanup;
    }
    std::rewind(records);
    std::rewind(strings);
    FILE *index = std::fopen(index_path.c_str(), "wb");
    IndexHeader header{.magic = kIndexMagic,
                       .version = kIndexVersion,
                       .header_size = sizeof(IndexHeader),
                       .playlist_id = result.playlist_id,
                       .count = result.count,
                       .records_offset = sizeof(IndexHeader),
                       .strings_offset = static_cast<uint32_t>(sizeof(IndexHeader) +
                                                               result.count * sizeof(IndexRecord)),
                       .strings_size = static_cast<uint32_t>(strings_size)};
    std::snprintf(header.name, sizeof(header.name), "%s", result.name.c_str());
    const bool combined = index != nullptr && write_exact(index, &header, sizeof(header)) &&
                          copy_file(records, index) && copy_file(strings, index) &&
                          std::fflush(index) == 0 && fsync(fileno(index)) == 0;
    if (index != nullptr) {
      std::fclose(index);
    }
    if (!combined) {
      result.error = "INDEX COMBINE FAILED";
      goto cleanup;
    }
  }
  {
    const std::string final = path("INDEX.DAT");
    const std::string backup = path("INDEX.BAK");
    unlink(backup.c_str());
    const bool had_final = access(final.c_str(), F_OK) == 0;
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

cleanup:
  if (raw != nullptr) std::fclose(raw);
  if (records != nullptr) std::fclose(records);
  if (strings != nullptr) std::fclose(strings);
  unlink(raw_path.c_str());
  unlink(records_path.c_str());
  unlink(strings_path.c_str());
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
  for (uint32_t i = 0; i < count; ++i) {
    IndexRecord record{};
    const long record_position = static_cast<long>(header.records_offset +
                                                    (start + i) * sizeof(IndexRecord));
    if (std::fseek(file, record_position, SEEK_SET) != 0 ||
        !read_exact(file, &record, sizeof(record))) {
      std::fclose(file);
      tracks.clear();
      return false;
    }
    auto load_string = [&](uint32_t offset, uint16_t length, std::string &value) {
      if (static_cast<uint64_t>(offset) + length > header.strings_size ||
          std::fseek(file, static_cast<long>(header.strings_offset + offset), SEEK_SET) != 0) {
        return false;
      }
      value.resize(length);
      return read_exact(file, value.data(), length);
    };
    Track track;
    track.index = record.item_id;
    track.queue_row = static_cast<int32_t>(start + i);
    track.duration_ms = record.duration_ms;
    if (!load_string(record.title_offset, record.title_length, track.title) ||
        !load_string(record.artist_offset, record.artist_length, track.artist)) {
      std::fclose(file);
      tracks.clear();
      return false;
    }
    tracks.push_back(std::move(track));
  }
  std::fclose(file);
  return true;
}

} // namespace musicremote
