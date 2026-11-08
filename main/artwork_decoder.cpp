#include "artwork_decoder.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "esp32s3/rom/tjpgd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace musicremote {
namespace {

constexpr char kTag[] = "cover-art";
constexpr size_t kMaxJpegBytes = 256 * 1024;
constexpr uint16_t kMaxDimension = 256;
constexpr size_t kWorkBufferBytes = 8 * 1024;
constexpr char kCacheMagic[] = "MRA1";
constexpr size_t kCacheHeaderBytes = 4 + 32;

struct DecodeContext {
  const uint8_t *input{nullptr};
  size_t input_size{0};
  size_t offset{0};
  ArtworkBitmap *output{nullptr};
};

UINT jpeg_input(JDEC *decoder, BYTE *destination, UINT count) {
  auto *context = static_cast<DecodeContext *>(decoder->device);
  const size_t available =
      context->input_size - std::min(context->offset, context->input_size);
  const size_t copied = std::min<size_t>(available, count);
  if (destination != nullptr && copied != 0) {
    std::memcpy(destination, context->input + context->offset, copied);
  }
  context->offset += copied;
  return static_cast<UINT>(copied);
}

UINT jpeg_output(JDEC *decoder, void *source, JRECT *rectangle) {
  auto *context = static_cast<DecodeContext *>(decoder->device);
  ArtworkBitmap &output = *context->output;
  if (rectangle->right >= output.width || rectangle->bottom >= output.height ||
      rectangle->left > rectangle->right ||
      rectangle->top > rectangle->bottom) {
    return 0;
  }
  const size_t tile_width = rectangle->right - rectangle->left + 1;
  const auto *rgb = static_cast<const uint8_t *>(source);
  for (uint16_t y = rectangle->top; y <= rectangle->bottom; ++y) {
    uint8_t *destination = output.pixels.data() +
                           static_cast<size_t>(y) * output.stride +
                           static_cast<size_t>(rectangle->left) * 3;
    for (size_t x = 0; x < tile_width; ++x) {
      // TJpgDec produces R,G,B; LVGL's packed RGB888 layout is B,G,R.
      destination[x * 3] = rgb[x * 3 + 2];
      destination[x * 3 + 1] = rgb[x * 3 + 1];
      destination[x * 3 + 2] = rgb[x * 3];
    }
    rgb += tile_width * 3;
  }
  return 1;
}

std::shared_ptr<ArtworkBitmap> decode_jpeg(const std::vector<uint8_t> &jpeg) {
  if (jpeg.size() < 4 || jpeg.size() > kMaxJpegBytes || jpeg[0] != 0xff ||
      jpeg[1] != 0xd8 || jpeg[jpeg.size() - 2] != 0xff || jpeg.back() != 0xd9) {
    return {};
  }

  std::vector<uint8_t> work(kWorkBufferBytes);
  JDEC decoder{};
  DecodeContext context{.input = jpeg.data(), .input_size = jpeg.size()};
  const JRESULT prepared =
      jd_prepare(&decoder, jpeg_input, work.data(), work.size(), &context);
  if (prepared != JDR_OK || decoder.width == 0 || decoder.height == 0 ||
      decoder.width > kMaxDimension || decoder.height > kMaxDimension) {
    ESP_LOGW(kTag, "Rejected JPEG: decoder=%d dimensions=%ux%u", prepared,
             decoder.width, decoder.height);
    return {};
  }

  auto bitmap = std::make_shared<ArtworkBitmap>();
  bitmap->width = static_cast<uint16_t>(decoder.width);
  bitmap->height = static_cast<uint16_t>(decoder.height);
  bitmap->stride = static_cast<uint32_t>(decoder.width * 3);
  bitmap->pixels.resize(static_cast<size_t>(bitmap->stride) * bitmap->height);
  context.output = bitmap.get();
  if (jd_decomp(&decoder, jpeg_output, 0) != JDR_OK) {
    ESP_LOGW(kTag, "JPEG decode failed");
    return {};
  }
  return bitmap;
}

bool valid_key(const std::string &key) {
  return key.size() == 32 &&
         std::all_of(key.begin(), key.end(), [](char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f');
         });
}

} // namespace

ArtworkDecoder::ArtworkDecoder(Model &model, Config config)
    : model_(model), config_(std::move(config)) {
  if (!config_.storage_directory.empty()) {
    cache_directory_ = config_.storage_directory + "/art";
  }
}

ArtworkDecoder::~ArtworkDecoder() { stop(); }

bool ArtworkDecoder::start() {
  if (running_.exchange(true)) {
    return true;
  }
  TaskHandle_t handle = nullptr;
  if (xTaskCreatePinnedToCore(task_entry, "cover-decode", 8 * 1024, this, 3,
                              &handle, 1) != pdPASS) {
    running_ = false;
    return false;
  }
  task_ = handle;
  return true;
}

void ArtworkDecoder::stop() { running_ = false; }

void ArtworkDecoder::select_track(std::string track_key) {
  model_.update([&track_key](Snapshot &state) {
    state.artwork.reset();
    state.artwork_track_key = track_key;
    state.artwork_state =
        track_key.empty() ? ArtworkState::none : ArtworkState::loading;
    ++state.artwork_revision;
  });
  if (track_key.empty()) {
    return;
  }
  std::lock_guard lock(jobs_mutex_);
  jobs_.clear();
  jobs_.push_back({JobType::load_cache, std::move(track_key), {}});
}

void ArtworkDecoder::submit_jpeg(std::string track_key,
                                 std::vector<uint8_t> jpeg) {
  if (!valid_key(track_key) || jpeg.size() > kMaxJpegBytes) {
    return;
  }
  std::lock_guard lock(jobs_mutex_);
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                             [&](const Job &job) {
                               return job.type == JobType::decode &&
                                      job.track_key == track_key;
                             }),
              jobs_.end());
  jobs_.push_back({JobType::decode, std::move(track_key), std::move(jpeg)});
}

void ArtworkDecoder::mark_unavailable(const std::string &track_key) {
  model_.update([&track_key](Snapshot &state) {
    if (state.artwork_track_key == track_key && state.artwork == nullptr) {
      state.artwork_state = ArtworkState::unavailable;
      ++state.artwork_revision;
    }
  });
}

void ArtworkDecoder::task_entry(void *context) {
  static_cast<ArtworkDecoder *>(context)->run();
  vTaskDelete(nullptr);
}

void ArtworkDecoder::run() {
  if (!cache_directory_.empty()) {
    if (mkdir(config_.storage_directory.c_str(), 0755) != 0 &&
        errno != EEXIST && errno != ENOTSUP) {
      ESP_LOGW(kTag, "Could not create storage directory: errno=%d", errno);
    }
    if (mkdir(cache_directory_.c_str(), 0755) != 0 && errno != EEXIST &&
        errno != ENOTSUP) {
      ESP_LOGW(kTag, "Could not create cache directory: errno=%d", errno);
    }
    prune_cache();
  }
  while (running_) {
    Job job{};
    bool available = false;
    {
      std::lock_guard lock(jobs_mutex_);
      if (!jobs_.empty()) {
        job = std::move(jobs_.front());
        jobs_.pop_front();
        available = true;
      }
    }
    if (available) {
      process(std::move(job));
    } else {
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }
  task_ = nullptr;
}

void ArtworkDecoder::process(Job job) {
  const std::string path = cache_path(job.track_key);
  if (job.type == JobType::load_cache) {
    if (path.empty() || !load_cache(path, job.track_key, job.jpeg)) {
      return;
    }
    ESP_LOGI(kTag, "Cache hit for %s (%u bytes)", job.track_key.c_str(),
             static_cast<unsigned>(job.jpeg.size()));
  }
  auto bitmap = decode_jpeg(job.jpeg);
  if (bitmap == nullptr) {
    if (job.type == JobType::load_cache && !path.empty()) {
      // A torn or stale cache entry must not permanently shadow fresh art.
      unlink(path.c_str());
    }
    if (job.type == JobType::decode) {
      model_.update([&job](Snapshot &state) {
        if (state.artwork_track_key == job.track_key &&
            state.artwork == nullptr) {
          state.artwork_state = ArtworkState::failed;
          ++state.artwork_revision;
        }
      });
    }
    return;
  }
  publish(job.track_key, std::move(bitmap));
  ESP_LOGI(kTag, "Decoded %s (%u bytes)", job.track_key.c_str(),
           static_cast<unsigned>(job.jpeg.size()));
  if (job.type == JobType::decode && !path.empty()) {
    store_cache(path, job.track_key, job.jpeg);
    prune_cache();
  }
}

void ArtworkDecoder::publish(const std::string &track_key,
                             std::shared_ptr<ArtworkBitmap> bitmap) {
  model_.update(
      [&track_key, bitmap = std::move(bitmap)](Snapshot &state) mutable {
        if (state.artwork_track_key != track_key) {
          return;
        }
        state.artwork = std::move(bitmap);
        state.artwork_state = ArtworkState::ready;
        ++state.artwork_revision;
      });
}

std::string ArtworkDecoder::cache_path(const std::string &track_key) const {
  if (cache_directory_.empty() || !valid_key(track_key)) {
    return {};
  }
  // The board's FatFS mount uses 8.3 names. The complete identity is stored in
  // and verified from the cache header so a truncated-name collision can never
  // display another track's artwork.
  return cache_directory_ + "/" + track_key.substr(0, 8) + ".JPG";
}

bool ArtworkDecoder::load_cache(const std::string &path,
                                const std::string &track_key,
                                std::vector<uint8_t> &jpeg) const {
  struct stat status{};
  if (stat(path.c_str(), &status) != 0 ||
      status.st_size < static_cast<off_t>(kCacheHeaderBytes + 4) ||
      status.st_size > static_cast<off_t>(kCacheHeaderBytes + kMaxJpegBytes)) {
    return false;
  }
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::array<char, kCacheHeaderBytes> header{};
  bool ok =
      std::fread(header.data(), 1, header.size(), file) == header.size() &&
      std::memcmp(header.data(), kCacheMagic, 4) == 0 &&
      std::memcmp(header.data() + 4, track_key.data(), 32) == 0;
  jpeg.resize(static_cast<size_t>(status.st_size) - kCacheHeaderBytes);
  ok = ok && std::fread(jpeg.data(), 1, jpeg.size(), file) == jpeg.size();
  std::fclose(file);
  if (!ok) {
    jpeg.clear();
  }
  return ok;
}

void ArtworkDecoder::store_cache(const std::string &path,
                                 const std::string &track_key,
                                 const std::vector<uint8_t> &jpeg) {
  if (!valid_key(track_key) || jpeg.empty() || jpeg.size() > kMaxJpegBytes) {
    return;
  }
  std::vector<uint8_t> existing;
  if (load_cache(path, track_key, existing) && existing == jpeg) {
    return;
  }
  const std::string temporary = path.substr(0, path.size() - 3) + "TMP";
  FILE *file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) {
    ESP_LOGW(kTag, "Could not open cache staging file: errno=%d", errno);
    return;
  }
  bool ok = std::fwrite(kCacheMagic, 1, 4, file) == 4 &&
            std::fwrite(track_key.data(), 1, 32, file) == 32 &&
            std::fwrite(jpeg.data(), 1, jpeg.size(), file) == jpeg.size() &&
            std::fflush(file) == 0;
  if (ok) {
    ok = fsync(fileno(file)) == 0;
  }
  std::fclose(file);
  if (ok) {
    unlink(path.c_str());
    if (rename(temporary.c_str(), path.c_str()) != 0) {
      ESP_LOGW(kTag, "Could not promote cache entry: errno=%d", errno);
      unlink(temporary.c_str());
    } else {
      ESP_LOGI(kTag, "Cached %u-byte JPEG", static_cast<unsigned>(jpeg.size()));
    }
  } else {
    unlink(temporary.c_str());
  }
}

void ArtworkDecoder::prune_cache() {
  if (cache_directory_.empty()) {
    return;
  }
  struct Entry {
    std::string path;
    time_t modified;
    size_t size;
  };
  std::vector<Entry> entries;
  size_t total = 0;
  DIR *directory = opendir(cache_directory_.c_str());
  if (directory == nullptr) {
    return;
  }
  while (dirent *entry = readdir(directory)) {
    const std::string name(entry->d_name);
    if (name.size() == 12 && name.substr(8) == ".TMP") {
      unlink((cache_directory_ + "/" + name).c_str());
      continue;
    }
    if (name.size() != 12 || name.substr(8) != ".JPG" ||
        !std::all_of(name.begin(), name.begin() + 8, [](char value) {
          return (value >= '0' && value <= '9') ||
                 (value >= 'a' && value <= 'f') ||
                 (value >= 'A' && value <= 'F');
        })) {
      continue;
    }
    const std::string path = cache_directory_ + "/" + name;
    struct stat status{};
    if (stat(path.c_str(), &status) == 0 && status.st_size >= 0) {
      entries.push_back(
          {path, status.st_mtime, static_cast<size_t>(status.st_size)});
      total += static_cast<size_t>(status.st_size);
    }
  }
  closedir(directory);
  std::sort(entries.begin(), entries.end(),
            [](const Entry &left, const Entry &right) {
              return left.modified < right.modified;
            });
  const size_t max_entries = config_.storage_internal_flash ? 16 : 128;
  const size_t max_bytes =
      config_.storage_internal_flash ? 512 * 1024 : 16 * 1024 * 1024;
  size_t first = 0;
  while (first < entries.size() &&
         (entries.size() - first > max_entries || total > max_bytes)) {
    if (unlink(entries[first].path.c_str()) == 0) {
      total -= std::min(total, entries[first].size);
      ESP_LOGI(kTag, "Pruned one cache entry");
    }
    ++first;
  }
}

} // namespace musicremote
