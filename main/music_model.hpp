#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace musicremote {

enum class ConnectionState { offline, wifi, connecting, authenticating, online, auth_failed };
enum class PlaybackState : uint8_t { playing = 1, paused = 2, stopped = 3, unknown = 4 };

struct Track {
  int32_t index{-1};
  int32_t queue_row{-1};
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

enum class ArtworkState : uint8_t { none, loading, ready, unavailable, failed };

enum class LyricsState : uint8_t { none, loading, ready, unavailable, failed };

using SharedText = std::shared_ptr<const std::string>;

struct LibraryPath {
  std::string genre;
  std::string artist;
  std::string album;
  std::string composer;

  bool empty() const {
    return genre.empty() && artist.empty() && album.empty() && composer.empty();
  }
};

enum class LibraryItemKind : uint8_t { node, album, track };

struct LibraryItem {
  LibraryItemKind kind{LibraryItemKind::node};
  int32_t selection_index{-1};
  uint32_t selection_request_id{0};
  std::string title;
  std::string subtitle;
  LibraryPath path;
  int32_t duration_ms{0};
  uint16_t year{0};
};

using LibraryItems = std::vector<LibraryItem>;
using SharedLibraryItems = std::shared_ptr<const LibraryItems>;

struct ArtworkBitmap {
  uint16_t width{0};
  uint16_t height{0};
  uint32_t stride{0};
  // LVGL's RGB888 storage order on this target is B, G, R.
  std::vector<uint8_t> pixels;
};

struct Snapshot {
  ConnectionState connection{ConnectionState::offline};
  uint64_t connection_generation{0};
  bool capabilities_known{false};
  uint64_t server_capabilities{0};
  PlaybackState playback{PlaybackState::unknown};
  bool shuffle{false};
  uint8_t repeat{0};
  uint16_t volume{0};
  int32_t position_ms{0};
  int64_t position_updated_us{0};
  int64_t seek_sent_us{0};
  Track now_playing;
  ArtworkState artwork_state{ArtworkState::none};
  std::shared_ptr<const ArtworkBitmap> artwork;
  std::string artwork_track_key;
  uint64_t artwork_revision{0};
  LyricsState lyrics_state{LyricsState::none};
  SharedText lyrics{std::make_shared<const std::string>()};
  std::string lyrics_track_key;
  uint64_t lyrics_revision{0};
  int32_t active_queue_index{-1};
  int32_t active_playlist_id{-1};
  std::string active_playlist_name{"CURRENT PLAYLIST"};
  std::vector<Track> queue;
  uint32_t queue_total_count{0};
  uint32_t active_playlist_track_count{0};
  bool active_playlist_count_known{false};
  bool library_play_pending{false};
  uint32_t queue_page_start{0};
  bool queue_index_ready{false};
  bool queue_indexing{false};
  bool queue_filter_active{false};
  std::string queue_filter_label;
  std::vector<Playlist> playlists;
  SharedLibraryItems library_items{std::make_shared<const LibraryItems>()};
  LibraryPath library_path;
  std::string library_title{"MEDIA LIBRARY"};
  uint32_t library_total_count{0};
  bool library_loading{false};
  bool library_truncated{false};
  std::string library_message;
  uint64_t library_revision{0};
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

  bool copy_if_changed(uint64_t known_revision, Snapshot &out) const {
    std::lock_guard lock(mutex_);
    if (state_.revision == known_revision) {
      return false;
    }
    out = state_;
    return true;
  }

  template <typename Fn> auto read(Fn &&fn) const {
    std::lock_guard lock(mutex_);
    return fn(state_);
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
