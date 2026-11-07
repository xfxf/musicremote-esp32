#pragma once

#include "music_model.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace musicremote {

class PlaylistIndex {
public:
  enum class SearchMode { contains, prefix, non_alpha };

  struct BuildResult {
    bool success{false};
    bool stale{false};
    int32_t playlist_id{-1};
    uint32_t count{0};
    std::string name;
    std::string error;
  };

  using Progress = std::function<void(uint32_t completed, uint32_t total)>;

  explicit PlaylistIndex(std::string directory, bool internal_flash = false);

  bool available() const;
  const std::string &incoming_path() const;
  BuildResult build_from_compressed(const Progress &progress = {});
  bool load_page(uint32_t start, uint32_t limit, std::vector<Track> &tracks,
                 uint32_t &total, int32_t &playlist_id, std::string &name) const;
  bool search_page(std::string_view query, SearchMode mode, uint32_t start,
                   uint32_t limit, std::vector<Track> &tracks, uint32_t &total,
                   int32_t &playlist_id, std::string &name) const;

private:
  std::string path(const char *name) const;
  bool recover_index() const;

  std::string directory_;
  std::string incoming_path_;
  bool internal_flash_{false};
  int64_t last_internal_promotion_us_{0};
};

} // namespace musicremote
