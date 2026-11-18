#pragma once

#include "music_model.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace musicremote {

class PlaylistIndex {
public:
  enum class SearchMode { contains, prefix, non_alpha };

  // How a successful result reused the promoted index instead of rebuilding.
  enum class Reuse { none, source_hash, content_hash };

  struct BuildResult {
    bool success{false};
    bool stale{false};
    Reuse reused{Reuse::none};
    int32_t playlist_id{-1};
    uint32_t count{0};
    std::string name;
    std::string error;
    // Phase timings for the serial log, in milliseconds.
    uint32_t source_hash_ms{0};
    uint32_t content_check_ms{0};
    uint32_t rebuild_ms{0};
    uint32_t decode_ms{0};
    uint32_t rows_ms{0};
    uint32_t search_ms{0};
    uint32_t finalize_ms{0};
  };

  // Rebuild progress across decode, rows, search area, and finalize, in units
  // of `total`; reaching `total` means the index is promoted.
  using Progress = std::function<void(uint32_t completed, uint32_t total)>;

  struct ActiveIdentity {
    int32_t playlist_id{-1};
    uint32_t count{0};
    std::string name;
  };

  explicit PlaylistIndex(std::string directory, bool internal_flash = false);

  bool available() const;
  const std::string &incoming_path() const;
  // FNV-1a over WIRE.TMP, which the network task computes while it streams
  // the frame so the build need not read the file again.
  static constexpr uint64_t kSourceHashSeed = 14695981039346656037ULL;
  static uint64_t extend_source_hash(uint64_t hash, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      hash ^= data[i];
      hash *= 1099511628211ULL;
    }
    return hash;
  }
  // `source_hash`, when known, must be extend_source_hash() over all of
  // WIRE.TMP; otherwise the file is hashed here.
  BuildResult build_from_compressed(const Progress &progress = {},
                                    std::optional<uint64_t> source_hash = std::nullopt);
  bool load_page(uint32_t start, uint32_t limit, std::vector<Track> &tracks,
                 uint32_t &total, int32_t &playlist_id, std::string &name) const;
  bool search_page(std::string_view query, SearchMode mode, uint32_t start,
                   uint32_t limit, std::vector<Track> &tracks, uint32_t &total,
                   int32_t &playlist_id, std::string &name,
                   const Progress &progress = {}) const;
  bool load_active_identity(ActiveIdentity &identity) const;
  bool mark_active(int32_t resolved_playlist_id) const;
  void clear_active() const;
  // A failed page read means INDEX.DAT is damaged even when its header and
  // extent validate. Rebuild it from the next response instead of reusing it.
  void mark_current_unusable() const;

private:
  std::string path(const char *name) const;
  bool recover_index() const;

  std::string directory_;
  std::string incoming_path_;
  bool internal_flash_{false};
  int64_t last_internal_promotion_us_{0};
  mutable std::atomic<bool> current_unusable_{false};
};

} // namespace musicremote
