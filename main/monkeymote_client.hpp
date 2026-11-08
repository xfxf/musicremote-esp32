#pragma once

#include "artwork_decoder.hpp"
#include "music_model.hpp"
#include "playlist_index.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace musicremote {

enum class CommandType {
  play,
  pause,
  stop,
  previous,
  next,
  seek,
  volume,
  shuffle,
  repeat,
  rating,
  request_state,
  request_playlists,
  request_playlist,
  activate_playlist,
  play_index,
  delete_index,
  clear_playlist,
  delete_playlist,
  create_playlist,
  request_queue_page,
  filter_queue,
  clear_queue_filter,
  request_library_root,
  browse_library,
  search_library,
  play_library_item,
  enqueue_library_item,
  request_lyrics,
  streamed_playlist_result,
};

struct Command {
  CommandType type;
  int32_t first{0};
  int32_t second{0};
  std::string text;
  LibraryPath library_path;
  uint64_t request_token{0};
  uint64_t stream_generation{0};
};

class MonkeyMoteClient {
public:
  struct Config {
    std::string host;
    uint16_t port{19371};
    std::string password;
    std::string storage_directory;
    bool storage_internal_flash{false};
  };

  MonkeyMoteClient(Model &model, Config config);
  ~MonkeyMoteClient();

  void start();
  void stop();
  void enqueue(Command command);

private:
  static constexpr uint32_t kMagic = 0x71D9873B;
  // Leave headroom for LVGL, Wi-Fi and the decoded model on a board with 2 MB
  // PSRAM. Oversized playlist frames are drained without allocating them.
  static constexpr size_t kMaxPayload = 512 * 1024;
  static constexpr size_t kMaxStreamedPayload = 4 * 1024 * 1024;
  static constexpr uint32_t kMaxStreamedDecoded = 64 * 1024 * 1024;

  static void task_entry(void *context);
  static void index_task_entry(void *context);
  void run();
  bool connect_and_authenticate();
  void close_socket();
  bool process_socket(int timeout_ms);
  bool process_frame(uint32_t type, std::vector<uint8_t> &payload);
  bool send_packet(uint32_t type, const void *data = nullptr, size_t size = 0);
  bool send_playlist_request(int32_t playlist_id, bool confirms_library_play = false);
  void send_command(const Command &command);
  void request_initial_state();

  void parse_track_info(const std::vector<uint8_t> &payload);
  void parse_playback(const std::vector<uint8_t> &payload);
  void parse_playlists(const std::vector<uint8_t> &payload);
  bool parse_playlist(const std::vector<uint8_t> &payload);
  void parse_compressed(std::vector<uint8_t> &payload);
  bool parse_library(const std::vector<uint8_t> &payload);
  bool parse_lyrics(const std::vector<uint8_t> &payload);
  void request_library(const LibraryPath &path, std::string title);
  void select_library_item(const Command &command, bool play);
  void complete_pending_library_play();
  void fail_pending_library_play(const char *message);
  struct PlaylistRequestToken {
    uint64_t id{0};
    int32_t playlist_id{-1};
    bool confirms_library_play{false};
    int64_t deadline_us{0};
  };
  std::optional<PlaylistRequestToken> take_playlist_response_token();
  void finish_playlist_response(const std::optional<PlaylistRequestToken> &token,
                                bool valid, int32_t resolved_playlist_id = -1,
                                uint32_t count = 0);
  void consume_playlist_response(bool valid, int32_t resolved_playlist_id = -1,
                                 uint32_t count = 0);
  void expire_requests(int64_t now_us);
  std::string take_cover_request();
  void reject_cover_reply();
  std::string take_lyrics_request();
  void reject_lyrics_reply(const char *message);
  bool start_streamed_frame(uint32_t type, uint32_t length, uint64_t generation);
  bool consume_streamed_frame();
  void finish_streamed_frame();
  void build_streamed_index();
  void abort_streamed_frame(const char *status);
  void load_index_page(uint32_t start, bool require_indexing = false);
  void search_index_page(uint32_t start, bool require_indexing = false);

  Model &model_;
  Config config_;
  std::atomic<bool> running_{false};
  int socket_{-1};
  void *task_{nullptr};
  std::vector<uint8_t> receive_buffer_;
  size_t discard_remaining_{0};
  PlaylistIndex playlist_index_;
  ArtworkDecoder artwork_decoder_;
  FILE *streamed_frame_{nullptr};
  size_t streamed_remaining_{0};
  std::atomic<bool> index_building_{false};
  std::atomic<bool> index_superseded_{false};
  bool queue_filter_active_{false};
  PlaylistIndex::SearchMode queue_filter_mode_{PlaylistIndex::SearchMode::contains};
  std::string queue_filter_;
  std::string cover_track_key_;
  std::deque<std::string> cover_requests_;
  struct PendingLyricsRequest {
    std::string track_key;
    int64_t deadline_us{0};
  };
  std::deque<PendingLyricsRequest> lyrics_requests_;
  uint32_t library_request_id_{0};
  int64_t library_request_deadline_us_{0};
  LibraryPath pending_library_path_;
  std::string pending_library_title_;
  bool pending_library_search_{false};
  int32_t pending_library_play_row_{-1};
  int32_t pending_library_play_destination_{-1};
  int64_t pending_library_play_deadline_us_{0};
  uint64_t pending_library_play_token_{0};
  uint64_t next_playlist_request_token_{1};
  std::deque<PlaylistRequestToken> playlist_requests_;
  std::optional<PlaylistRequestToken> streamed_playlist_request_;
  std::mutex playlist_requests_mutex_;
  std::atomic<uint64_t> playlist_stream_generation_{0};
  uint64_t receiving_stream_generation_{0};
  uint64_t streamed_playlist_generation_{0};
  int64_t last_receive_us_{0};
  int64_t last_ping_us_{0};
  std::mutex commands_mutex_;
  std::deque<Command> commands_;
};

} // namespace musicremote
