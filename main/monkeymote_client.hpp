#pragma once

#include "artwork_decoder.hpp"
#include "connection_policy.hpp"
#include "music_model.hpp"
#include "playlist_index.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
  request_advanced_controls,
  equalizer_gain,
  sleep_timer,
  computer_power,
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
  int64_t enqueued_us{0};
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

  bool prepare_index_worker();
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
  // Every deliberate disconnect names its reason so a reconnect loop is
  // attributable from the serial log.
  void close_socket(const char *reason);
  bool process_socket(int timeout_ms, const char *&failure);
  bool process_frame(uint32_t type, std::vector<uint8_t> &payload);
  bool send_packet(uint32_t type, const void *data = nullptr, size_t size = 0);
  // `request_current` asks AIMP for the current playlist (-1) on the wire
  // while the token still expects `playlist_id`, e.g. right after activating
  // it: AIMP's -1 response is byte-identical to the one the index was built or
  // last verified from, so it is reused by source hash instead of decoded.
  bool send_playlist_request(int32_t playlist_id, bool confirms_library_play = false,
                             bool cacheable_active = false, bool request_current = false);
  void send_command(const Command &command);
  void request_initial_state();
  bool prepare_persisted_playlist_cache();
  void validate_persisted_playlist_cache(const std::vector<Playlist> &playlists,
                                         int32_t folder_id);
  void begin_playlist_refresh(const char *status);
  void process_offline_commands();

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
    bool cacheable_active{false};
    int64_t deadline_us{0};
    uint64_t mutation_generation{0};
    int64_t sent_us{0};
  };
  std::optional<PlaylistRequestToken> take_playlist_response_token();
  void finish_playlist_response(const std::optional<PlaylistRequestToken> &token,
                                bool valid, int32_t resolved_playlist_id = -1,
                                uint32_t count = 0);
  void consume_playlist_response(bool valid, int32_t resolved_playlist_id = -1,
                                 uint32_t count = 0);
  void expire_requests(int64_t now_us);
  void expect_activation_echo(int64_t now_us);
  void check_play_verification(int64_t now_us);
  bool take_activation_echo(int64_t now_us);
  std::string take_cover_request();
  void reject_cover_reply();
  std::string take_lyrics_request();
  void reject_lyrics_reply(const char *message);
  bool start_streamed_frame(uint32_t type, uint32_t length, uint64_t generation);
  bool consume_streamed_frame();
  void finish_streamed_frame();
  bool build_streamed_index();
  void abort_streamed_frame(const char *status);
  // `response_playlist_id` is the ID the producing request expected (-1 when
  // it asked for the current playlist); used only when publishing a build.
  void load_index_page(uint32_t start, bool require_indexing = false,
                       bool count_current = false,
                       int32_t response_playlist_id = -1);
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
  int64_t streamed_started_us_{0};
  // Hash of the frame streamed so far, handed to the index worker with it.
  uint64_t streamed_source_hash_{PlaylistIndex::kSourceHashSeed};
  uint64_t completed_source_hash_{0};
  // The page shown while a large frame re-validates it; the build republishes
  // that page rather than jumping to the first.
  std::atomic<uint32_t> revalidation_page_start_{0};
  std::atomic<bool> index_building_{false};
  std::atomic<bool> index_superseded_{false};
  TaskHandle_t index_task_{nullptr};
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
  std::vector<uint8_t> equalizer_payload_;
  advanced::EqualizerEditOverlay equalizer_edits_;
  advanced::RequestTracker advanced_requests_;
  uint32_t library_request_id_{0};
  int64_t library_request_deadline_us_{0};
  LibraryPath pending_library_path_;
  std::string pending_library_title_;
  bool pending_library_search_{false};
  int32_t pending_library_play_row_{-1};
  int32_t pending_library_play_destination_{-1};
  std::atomic<int64_t> pending_library_play_deadline_us_{0};
  uint64_t pending_library_play_token_{0};
  uint64_t next_playlist_request_token_{1};
  // One deadline per playlist activation whose type 12 echo has not arrived.
  // Activations can outrun their echoes (e.g. behind a large frame), so each
  // is expected separately; deadlines pause while a large frame is received.
  std::deque<int64_t> activation_echo_deadlines_;
  // The row a tap played, checked against AIMP's next now-playing report.
  struct PlayVerification {
    bool active{false};
    int32_t row{-1};
    std::string title;
    std::string artist;
    int64_t deadline_us{0};
    bool row_reported{false};
    bool row_matched{false};
    bool track_reported{false};
    bool track_matched{false};
  } play_verification_;
  std::deque<PlaylistRequestToken> playlist_requests_;
  std::optional<PlaylistRequestToken> streamed_playlist_request_;
  std::mutex playlist_requests_mutex_;
  std::atomic<uint64_t> playlist_stream_generation_{0};
  // Incremented by every queue mutation and change notification. Only a
  // response to a request sent after the latest one carries a current count.
  std::atomic<uint64_t> playlist_mutation_generation_{0};
  uint64_t receiving_stream_generation_{0};
  uint64_t streamed_playlist_generation_{0};
  struct PersistedPlaylistCache {
    bool available{false};
    bool awaiting_root_validation{false};
    int32_t playlist_id{-1};
    uint32_t count{0};
    std::string name;
  } persisted_playlist_cache_;
  std::atomic<bool> using_persisted_playlist_cache_{false};
  int64_t persisted_cache_validation_deadline_us_{0};
  int64_t last_receive_us_{0};
  int64_t last_send_us_{0};
  connection_policy::StreamPauseClock stream_pause_clock_;
  std::mutex commands_mutex_;
  std::deque<Command> commands_;
};

} // namespace musicremote
