#pragma once

#include "artwork_decoder.hpp"
#include "music_model.hpp"
#include "playlist_index.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <mutex>
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
};

struct Command {
  CommandType type;
  int32_t first{0};
  int32_t second{0};
  std::string text;
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
  bool process_frame(uint32_t type, const std::vector<uint8_t> &payload);
  bool send_packet(uint32_t type, const void *data = nullptr, size_t size = 0);
  void send_command(const Command &command);
  void request_initial_state();

  void parse_track_info(const std::vector<uint8_t> &payload);
  void parse_playback(const std::vector<uint8_t> &payload);
  void parse_playlists(const std::vector<uint8_t> &payload);
  void parse_playlist(const std::vector<uint8_t> &payload);
  void parse_compressed(const std::vector<uint8_t> &payload);
  std::string take_cover_request();
  void reject_cover_reply();
  bool start_streamed_frame(uint32_t type, uint32_t length);
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
  int64_t last_receive_us_{0};
  int64_t last_ping_us_{0};
  std::mutex commands_mutex_;
  std::deque<Command> commands_;
};

} // namespace musicremote
