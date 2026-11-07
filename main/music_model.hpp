#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace musicremote {

enum class ConnectionState { offline, wifi, connecting, authenticating, online, auth_failed };
enum class PlaybackState : uint8_t { playing = 1, paused = 2, stopped = 3, unknown = 4 };

struct Track {
  int32_t index{-1};
  int32_t duration_ms{0};
  uint16_t bitrate{0};
  uint8_t rating{0};
  uint16_t year{0};
  std::string title;
  std::string artist;
  std::string album;
  std::string codec;
};

struct Playlist {
  int32_t id{-1};
  int32_t count{0};
  bool active{false};
  bool folder{false};
  bool read_only{false};
  std::string name;
};

struct Snapshot {
  ConnectionState connection{ConnectionState::offline};
  PlaybackState playback{PlaybackState::unknown};
  bool shuffle{false};
  uint8_t repeat{0};
  uint16_t volume{0};
  int32_t position_ms{0};
  Track now_playing;
  int32_t active_queue_index{-1};
  int32_t active_playlist_id{-1};
  std::string active_playlist_name{"CURRENT PLAYLIST"};
  std::vector<Track> queue;
  std::vector<Playlist> playlists;
  uint64_t queue_revision{0};
  uint64_t playlists_revision{0};
  uint64_t revision{0};
  std::string status{"BOOTING"};
};

class Model {
public:
  Snapshot copy() const {
    std::lock_guard lock(mutex_);
    return state_;
  }

  template <typename Fn> void update(Fn &&fn) {
    std::lock_guard lock(mutex_);
    fn(state_);
    ++state_.revision;
  }

private:
  mutable std::mutex mutex_;
  Snapshot state_;
};

} // namespace musicremote
