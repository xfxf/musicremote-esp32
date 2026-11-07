#pragma once

#include "music_model.hpp"

#include <atomic>
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
  };

  MonkeyMoteClient(Model &model, Config config);
  ~MonkeyMoteClient();

  void start();
  void stop();
  void enqueue(Command command);

private:
  static constexpr uint32_t kMagic = 0x71D9873B;
  static constexpr size_t kMaxPayload = 2 * 1024 * 1024;

  static void task_entry(void *context);
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

  Model &model_;
  Config config_;
  std::atomic<bool> running_{false};
  int socket_{-1};
  void *task_{nullptr};
  std::vector<uint8_t> receive_buffer_;
  std::mutex commands_mutex_;
  std::deque<Command> commands_;
};

} // namespace musicremote
