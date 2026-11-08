#pragma once

#include "music_model.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace musicremote::playlist_cache_policy {

struct Identity {
  int32_t playlist_id{-1};
  uint32_t count{0};
  std::string_view name;
};

// AIMP's lightweight playlist collection has no revision or active-playlist
// bit. An explicit ID is authoritative only when its name and count still
// match. A cached current-playlist sentinel (-1) can be resolved only when the
// name/count pair identifies exactly one non-folder playlist.
inline std::optional<int32_t> resolve(const Identity &identity,
                                      const std::vector<Playlist> &playlists) {
  std::optional<int32_t> resolved;
  for (const auto &playlist : playlists) {
    if (playlist.folder || playlist.count < 0 ||
        static_cast<uint32_t>(playlist.count) != identity.count ||
        playlist.name != identity.name ||
        (identity.playlist_id >= 0 && playlist.id != identity.playlist_id)) {
      continue;
    }
    if (resolved.has_value()) {
      return std::nullopt;
    }
    resolved = playlist.id;
  }
  return resolved;
}

} // namespace musicremote::playlist_cache_policy
