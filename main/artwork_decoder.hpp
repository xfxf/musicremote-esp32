#pragma once

#include "music_model.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace musicremote {

// Decodes and caches cover art on its own task. No LVGL or socket operation is
// performed here, and stale results are discarded by track key before publish.
class ArtworkDecoder {
public:
  struct Config {
    std::string storage_directory;
    bool storage_internal_flash{false};
  };

  ArtworkDecoder(Model &model, Config config);
  ~ArtworkDecoder();

  bool start();
  void stop();
  void select_track(std::string track_key);
  void submit_jpeg(std::string track_key, std::vector<uint8_t> jpeg);
  void mark_unavailable(const std::string &track_key);

private:
  enum class JobType { load_cache, decode };
  struct Job {
    JobType type;
    std::string track_key;
    std::vector<uint8_t> jpeg;
  };

  static void task_entry(void *context);
  void run();
  void process(Job job);
  void publish(const std::string &track_key,
               std::shared_ptr<ArtworkBitmap> bitmap);
  std::string cache_path(const std::string &track_key) const;
  bool load_cache(const std::string &path, const std::string &track_key,
                  std::vector<uint8_t> &jpeg) const;
  void store_cache(const std::string &path, const std::string &track_key,
                   const std::vector<uint8_t> &jpeg);
  void prune_cache();

  Model &model_;
  Config config_;
  std::string cache_directory_;
  std::atomic<bool> running_{false};
  void *task_{nullptr};
  std::mutex jobs_mutex_;
  std::deque<Job> jobs_;
};

} // namespace musicremote
