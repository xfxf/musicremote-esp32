#include "monkeymote_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"
#include "mdns.h"

namespace musicremote {
namespace {

constexpr char kTag[] = "monkeymote";

uint16_t read_u16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

uint32_t read_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t read_i32(const uint8_t *p) { return static_cast<int32_t>(read_u32(p)); }

void append_u16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

void append_i32(std::vector<uint8_t> &out, int32_t value) {
  append_u32(out, static_cast<uint32_t>(value));
}

bool read_cstring(const std::vector<uint8_t> &data, size_t &offset, std::string &out) {
  if (offset >= data.size()) {
    return false;
  }
  auto end = std::find(data.begin() + static_cast<ptrdiff_t>(offset), data.end(), 0);
  if (end == data.end()) {
    return false;
  }
  out.assign(reinterpret_cast<const char *>(data.data() + offset),
             static_cast<size_t>(end - data.begin()) - offset);
  offset = static_cast<size_t>(end - data.begin()) + 1;
  return true;
}

bool lz4_decompress(const uint8_t *source, size_t source_size, uint8_t *target,
                    size_t target_size) {
  size_t input = 0;
  size_t output = 0;
  while (input < source_size) {
    const uint8_t token = source[input++];
    size_t literal_length = token >> 4;
    if (literal_length == 15) {
      uint8_t extension = 255;
      while (extension == 255) {
        if (input >= source_size) {
          return false;
        }
        extension = source[input++];
        literal_length += extension;
      }
    }
    if (input + literal_length > source_size || output + literal_length > target_size) {
      return false;
    }
    std::memcpy(target + output, source + input, literal_length);
    input += literal_length;
    output += literal_length;
    if (input == source_size) {
      return output == target_size;
    }
    if (input + 2 > source_size) {
      return false;
    }
    const size_t distance = read_u16(source + input);
    input += 2;
    if (distance == 0 || distance > output) {
      return false;
    }
    size_t match_length = token & 0x0f;
    if (match_length == 15) {
      uint8_t extension = 255;
      while (extension == 255) {
        if (input >= source_size) {
          return false;
        }
        extension = source[input++];
        match_length += extension;
      }
    }
    match_length += 4;
    if (output + match_length > target_size) {
      return false;
    }
    for (size_t i = 0; i < match_length; ++i) {
      target[output + i] = target[output - distance + i];
    }
    output += match_length;
  }
  return output == target_size;
}

bool recv_exact(int socket, void *data, size_t size) {
  auto *bytes = static_cast<uint8_t *>(data);
  size_t received = 0;
  while (received < size) {
    const ssize_t result = recv(socket, bytes + received, size - received, 0);
    if (result <= 0) {
      return false;
    }
    received += static_cast<size_t>(result);
  }
  return true;
}

} // namespace

MonkeyMoteClient::MonkeyMoteClient(Model &model, Config config)
    : model_(model), config_(std::move(config)),
      playlist_index_(config_.storage_directory) {}

MonkeyMoteClient::~MonkeyMoteClient() { stop(); }

void MonkeyMoteClient::start() {
  if (running_.exchange(true)) {
    return;
  }
  TaskHandle_t handle = nullptr;
  if (xTaskCreatePinnedToCore(task_entry, "monkeymote", 12 * 1024, this, 5, &handle, 0) != pdPASS) {
    running_ = false;
    model_.update([](Snapshot &state) {
      state.connection = ConnectionState::offline;
      state.status = "NETWORK TASK FAILED";
    });
    return;
  }
  task_ = handle;
}

void MonkeyMoteClient::stop() {
  running_ = false;
  close_socket();
  if (streamed_frame_ != nullptr) {
    std::fclose(streamed_frame_);
    streamed_frame_ = nullptr;
  }
}

void MonkeyMoteClient::enqueue(Command command) {
  std::lock_guard lock(commands_mutex_);
  if (commands_.size() >= 32) {
    commands_.pop_front();
  }
  commands_.push_back(std::move(command));
}

void MonkeyMoteClient::task_entry(void *context) {
  static_cast<MonkeyMoteClient *>(context)->run();
  vTaskDelete(nullptr);
}

void MonkeyMoteClient::run() {
  int retry_seconds = 1;
  while (running_) {
    if (!connect_and_authenticate()) {
      close_socket();
      model_.update([](Snapshot &state) {
        if (state.connection != ConnectionState::auth_failed) {
          state.connection = ConnectionState::offline;
          state.status = "RECONNECTING";
        }
      });
      for (int i = 0; i < retry_seconds * 10 && running_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      retry_seconds = std::min(retry_seconds * 2, 15);
      continue;
    }
    retry_seconds = 1;
    request_initial_state();
    while (running_ && socket_ >= 0) {
      std::deque<Command> pending;
      {
        std::lock_guard lock(commands_mutex_);
        pending.swap(commands_);
      }
      for (const auto &command : pending) {
        send_command(command);
      }
      if (!process_socket(100)) {
        break;
      }
      const int64_t now = esp_timer_get_time();
      if (now - last_receive_us_ > 30'000'000) {
        ESP_LOGW(kTag, "AIMP connection timed out");
        break;
      }
      if (now - last_receive_us_ > 10'000'000 && now - last_ping_us_ > 10'000'000) {
        if (!send_packet(23)) {
          break;
        }
        last_ping_us_ = now;
      }
    }
    close_socket();
    {
      std::lock_guard lock(commands_mutex_);
      commands_.clear();
    }
  }
  task_ = nullptr;
}

bool MonkeyMoteClient::connect_and_authenticate() {
  model_.update([](Snapshot &state) {
    state.connection = ConnectionState::connecting;
    state.status = "CONNECTING";
  });

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const std::string port = std::to_string(config_.port);
  auto connect_address = [this](int family, int type, int protocol, const sockaddr *address,
                                socklen_t address_length) {
    socket_ = socket(family, type, protocol);
    if (socket_ < 0) {
      return false;
    }
    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int yes = 1;
    setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int keep_idle = 10;
    int keep_interval = 3;
    int keep_count = 3;
    setsockopt(socket_, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    setsockopt(socket_, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval));
    setsockopt(socket_, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count));
    if (connect(socket_, address, address_length) == 0) {
      return true;
    }
    close_socket();
    return false;
  };

  if (getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &addresses) == 0 &&
      addresses != nullptr) {
    for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
      if (connect_address(address->ai_family, address->ai_socktype, address->ai_protocol,
                          address->ai_addr, address->ai_addrlen)) {
        break;
      }
    }
    freeaddrinfo(addresses);
  } else if (config_.host.ends_with(".local")) {
    const std::string hostname = config_.host.substr(0, config_.host.size() - 6);
    esp_ip4_addr_t result{};
    if (mdns_query_a(hostname.c_str(), 3000, &result) == ESP_OK) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(config_.port);
      address.sin_addr.s_addr = result.addr;
      ESP_LOGI(kTag, "mDNS resolved %s to " IPSTR, config_.host.c_str(), IP2STR(&result));
      connect_address(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                      reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    }
  }
  if (socket_ < 0) {
    ESP_LOGW(kTag, "Could not resolve/connect to %s:%u", config_.host.c_str(), config_.port);
    return false;
  }

  std::array<uint8_t, 12> header{};
  if (!recv_exact(socket_, header.data(), header.size())) {
    return false;
  }
  const uint32_t magic = read_u32(header.data());
  const uint32_t type = read_u32(header.data() + 4);
  const uint32_t length = read_u32(header.data() + 8);
  if (magic != kMagic || type != 1 || length != 11) {
    ESP_LOGW(kTag, "Unexpected server hello");
    return false;
  }
  std::array<uint8_t, 11> hello{};
  if (!recv_exact(socket_, hello.data(), hello.size())) {
    return false;
  }
  model_.update([](Snapshot &state) {
    state.connection = ConnectionState::authenticating;
    state.status = "AUTHENTICATING";
  });
  std::array<uint8_t, 18> auth{};
  if (hello[2] != 0) {
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    mbedtls_md5_update(&md5, reinterpret_cast<const unsigned char *>(config_.password.data()),
                       config_.password.size());
    mbedtls_md5_update(&md5, hello.data() + 3, 8);
    mbedtls_md5_finish(&md5, auth.data());
    mbedtls_md5_free(&md5);
  }
  auth[16] = 0x81;
  auth[17] = 0x05;
  if (!send_packet(2, auth.data(), auth.size()) ||
      !recv_exact(socket_, header.data(), header.size())) {
    return false;
  }
  const uint32_t auth_type = read_u32(header.data() + 4);
  const uint32_t auth_length = read_u32(header.data() + 8);
  if (read_u32(header.data()) != kMagic || auth_length > 16) {
    return false;
  }
  std::vector<uint8_t> auth_response(auth_length);
  if (!recv_exact(socket_, auth_response.data(), auth_response.size())) {
    return false;
  }
  if (auth_type != 3 || auth_response.size() != 1 || auth_response[0] != 1) {
    model_.update([](Snapshot &state) {
      state.connection = ConnectionState::auth_failed;
      state.status = "PASSWORD REJECTED";
    });
    return false;
  }
  timeval nonblocking_timeout{.tv_sec = 0, .tv_usec = 250000};
  setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &nonblocking_timeout,
             sizeof(nonblocking_timeout));
  receive_buffer_.clear();
  discard_remaining_ = 0;
  last_receive_us_ = esp_timer_get_time();
  last_ping_us_ = last_receive_us_;
  model_.update([](Snapshot &state) {
    state.connection = ConnectionState::online;
    state.status = "AIMP ONLINE";
  });
  ESP_LOGI(kTag, "Authenticated to %s:%u", config_.host.c_str(), config_.port);
  return true;
}

void MonkeyMoteClient::close_socket() {
  const int old_socket = socket_;
  socket_ = -1;
  if (old_socket >= 0) {
    shutdown(old_socket, SHUT_RDWR);
    close(old_socket);
  }
}

bool MonkeyMoteClient::process_socket(int timeout_ms) {
  if (socket_ < 0) {
    return false;
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket_, &read_set);
  timeval timeout{.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
  const int selected = select(socket_ + 1, &read_set, nullptr, nullptr, &timeout);
  if (selected < 0) {
    return errno == EINTR;
  }
  if (selected > 0) {
    std::array<uint8_t, 4096> chunk{};
    const ssize_t count = recv(socket_, chunk.data(), chunk.size(), 0);
    if (count <= 0) {
      return false;
    }
    last_receive_us_ = esp_timer_get_time();
    receive_buffer_.insert(receive_buffer_.end(), chunk.begin(), chunk.begin() + count);
  }
  while (true) {
    if (discard_remaining_ != 0) {
      const size_t discarded = std::min(discard_remaining_, receive_buffer_.size());
      receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + discarded);
      discard_remaining_ -= discarded;
      if (discard_remaining_ != 0) {
        break;
      }
      continue;
    }
    if (streamed_frame_ != nullptr) {
      if (!consume_streamed_frame()) {
        return false;
      }
      if (streamed_frame_ != nullptr || receive_buffer_.empty()) {
        break;
      }
      continue;
    }
    if (receive_buffer_.size() < 12) {
      break;
    }
    if (read_u32(receive_buffer_.data()) != kMagic) {
      ESP_LOGW(kTag, "Protocol desynchronised");
      return false;
    }
    const uint32_t type = read_u32(receive_buffer_.data() + 4);
    const uint32_t length = read_u32(receive_buffer_.data() + 8);
    if (length > kMaxPayload) {
      receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + 12);
      if (type == 45 && length <= 4 * 1024 * 1024 && start_streamed_frame(type, length)) {
        continue;
      }
      ESP_LOGW(kTag, "Draining oversized payload: %" PRIu32, length);
      discard_remaining_ = length;
      model_.update([](Snapshot &state) {
        state.queue.clear();
        ++state.queue_revision;
        state.active_playlist_name = "PLAYLIST TOO LARGE";
        state.status = "PLAYLIST TOO LARGE";
      });
      continue;
    }
    if (receive_buffer_.size() < 12 + length) {
      break;
    }
    std::vector<uint8_t> payload(receive_buffer_.begin() + 12,
                                 receive_buffer_.begin() + 12 + length);
    receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + 12 + length);
    if (!process_frame(type, payload)) {
      ESP_LOGW(kTag, "Malformed packet type=%" PRIu32 " bytes=%" PRIu32, type, length);
    }
  }
  return true;
}

bool MonkeyMoteClient::start_streamed_frame(uint32_t type, uint32_t length) {
  if (type != 45 || !playlist_index_.available()) {
    return false;
  }
  streamed_frame_ = std::fopen(playlist_index_.incoming_path().c_str(), "wb");
  if (streamed_frame_ == nullptr) {
    return false;
  }
  streamed_remaining_ = length;
  model_.update([](Snapshot &state) {
    state.queue.clear();
    state.queue_total_count = 0;
    state.queue_page_start = 0;
    state.queue_index_ready = false;
    state.queue_indexing = true;
    ++state.queue_revision;
    state.status = "RECEIVING LIBRARY";
  });
  ESP_LOGI(kTag, "Streaming %" PRIu32 "-byte playlist frame to microSD", length);
  return true;
}

bool MonkeyMoteClient::consume_streamed_frame() {
  const size_t count = std::min(streamed_remaining_, receive_buffer_.size());
  if (count != 0 && std::fwrite(receive_buffer_.data(), 1, count, streamed_frame_) != count) {
    std::fclose(streamed_frame_);
    streamed_frame_ = nullptr;
    streamed_remaining_ -= count;
    discard_remaining_ = streamed_remaining_;
    streamed_remaining_ = 0;
    model_.update([](Snapshot &state) {
      state.queue_indexing = false;
      state.status = "SD CARD WRITE FAILED";
    });
    return true;
  }
  receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + count);
  streamed_remaining_ -= count;
  if (streamed_remaining_ == 0) {
    std::fflush(streamed_frame_);
    fsync(fileno(streamed_frame_));
    std::fclose(streamed_frame_);
    streamed_frame_ = nullptr;
    finish_streamed_frame();
  }
  return true;
}

void MonkeyMoteClient::finish_streamed_frame() {
  model_.update([](Snapshot &state) { state.status = "INDEXING LIBRARY"; });
  const auto result = playlist_index_.build_from_compressed([this](uint32_t completed,
                                                                  uint32_t total) {
    if (completed == total || completed % 4096 == 1) {
      model_.update([completed, total](Snapshot &state) {
        state.status = "INDEXING " +
                       std::to_string(completed * 100 / std::max<uint32_t>(1, total)) + "%";
      });
    }
  });
  unlink(playlist_index_.incoming_path().c_str());
  if (!result.success) {
    ESP_LOGW(kTag, "Large playlist index failed: %s", result.error.c_str());
    model_.update([message = result.error](Snapshot &state) {
      state.queue_indexing = false;
      state.active_playlist_name = "PLAYLIST TOO LARGE";
      state.status = message;
    });
    return;
  }
  ESP_LOGI(kTag, "Indexed %" PRIu32 " tracks for playlist %" PRId32, result.count,
           result.playlist_id);
  load_index_page(0);
}

void MonkeyMoteClient::load_index_page(uint32_t start) {
  std::vector<Track> tracks;
  uint32_t total = 0;
  int32_t playlist_id = -1;
  std::string name;
  if (!playlist_index_.load_page(start, 100, tracks, total, playlist_id, name)) {
    model_.update([](Snapshot &state) {
      state.queue_indexing = false;
      state.queue_index_ready = false;
      state.status = "INDEX READ FAILED";
    });
    return;
  }
  model_.update([start, total, playlist_id, name = std::move(name),
                 tracks = std::move(tracks)](Snapshot &state) mutable {
    state.active_playlist_id = playlist_id;
    state.active_playlist_name = std::move(name);
    state.queue = std::move(tracks);
    state.queue_total_count = total;
    state.queue_page_start = start;
    state.queue_indexing = false;
    state.queue_index_ready = true;
    state.status = "LIBRARY READY";
    ++state.queue_revision;
  });
}

bool MonkeyMoteClient::process_frame(uint32_t type, const std::vector<uint8_t> &payload) {
  switch (type) {
  case 5:
    parse_track_info(payload);
    return true;
  case 6:
    parse_playback(payload);
    return true;
  case 11:
    if (payload.size() >= 4) {
      const int32_t index = read_i32(payload.data());
      model_.update([index](Snapshot &state) {
        state.active_queue_index = index;
      });
      return true;
    }
    return false;
  case 12:
  case 17:
  case 30:
  case 35:
    enqueue({CommandType::request_playlist, -1});
    return true;
  case 23:
    send_packet(23);
    return true;
  case 28:
    return payload.size() == 8;
  case 41:
    parse_playlists(payload);
    return true;
  case 45:
    parse_compressed(payload);
    return true;
  case 9:
  case 31:
  case 50:
    parse_playlist(payload);
    return true;
  case 43:
    if (!payload.empty() && payload[0] != 0) {
      model_.update([](Snapshot &state) { state.status = "AIMP COMMAND FAILED"; });
    }
    return true;
  default:
    ESP_LOGD(kTag, "Ignoring packet type=%" PRIu32 " bytes=%u", type,
             static_cast<unsigned>(payload.size()));
    return true;
  }
}

bool MonkeyMoteClient::send_packet(uint32_t type, const void *data, size_t size) {
  if (socket_ < 0 || size > kMaxPayload) {
    return false;
  }
  std::vector<uint8_t> bytes;
  bytes.reserve(12 + size);
  append_u32(bytes, kMagic);
  append_u32(bytes, type);
  append_u32(bytes, static_cast<uint32_t>(size));
  if (size != 0) {
    const auto *payload = static_cast<const uint8_t *>(data);
    bytes.insert(bytes.end(), payload, payload + size);
  }
  size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t result = send(socket_, bytes.data() + sent, bytes.size() - sent, 0);
    if (result <= 0) {
      return false;
    }
    sent += static_cast<size_t>(result);
  }
  return true;
}

void MonkeyMoteClient::send_command(const Command &command) {
  std::vector<uint8_t> payload;
  auto simple = [this](uint8_t code, int32_t value) {
    std::vector<uint8_t> bytes{code};
    append_i32(bytes, value);
    return send_packet(7, bytes.data(), bytes.size());
  };
  switch (command.type) {
  case CommandType::play:
    simple(1, 0);
    break;
  case CommandType::pause:
    simple(2, 0);
    break;
  case CommandType::stop:
    simple(3, 0);
    break;
  case CommandType::previous:
    simple(9, 0);
    break;
  case CommandType::next:
    simple(8, 0);
    break;
  case CommandType::seek:
    if (simple(7, command.first)) {
      const int64_t sent_us = esp_timer_get_time();
      model_.update([sent_us](Snapshot &state) { state.seek_sent_us = sent_us; });
    }
    break;
  case CommandType::shuffle:
    simple(4, command.first != 0);
    break;
  case CommandType::repeat:
    simple(5, std::clamp<int32_t>(command.first, 0, 2));
    break;
  case CommandType::rating: {
    const uint8_t rating = static_cast<uint8_t>(std::clamp<int32_t>(command.first, 0, 5));
    send_packet(10, &rating, sizeof(rating));
    break;
  }
  case CommandType::volume: {
    const uint16_t volume =
        static_cast<uint16_t>(std::clamp<int32_t>(command.first, 0, 65535));
    payload.push_back(6);
    append_u16(payload, static_cast<uint16_t>(volume / 256));
    append_u16(payload, volume);
    send_packet(7, payload.data(), payload.size());
    break;
  }
  case CommandType::request_state:
    request_initial_state();
    break;
  case CommandType::request_playlists:
    payload.push_back(13);
    append_i32(payload, command.first);
    send_packet(4, payload.data(), payload.size());
    break;
  case CommandType::request_playlist:
    payload.push_back(10);
    append_i32(payload, command.first);
    append_i32(payload, 1);
    send_packet(4, payload.data(), payload.size());
    break;
  case CommandType::activate_playlist:
    simple(12, command.first);
    enqueue({CommandType::request_playlist, command.first});
    break;
  case CommandType::play_index:
    simple(10, command.first);
    simple(1, 0);
    break;
  case CommandType::delete_index:
    append_i32(payload, command.first);
    append_i32(payload, command.second);
    send_packet(32, payload.data(), payload.size());
    enqueue({CommandType::request_playlist, command.first});
    break;
  case CommandType::clear_playlist:
    append_i32(payload, command.first);
    send_packet(20, payload.data(), payload.size());
    enqueue({CommandType::request_playlist, command.first});
    break;
  case CommandType::delete_playlist:
    simple(16, command.first);
    enqueue({CommandType::request_playlists, command.second});
    break;
  case CommandType::create_playlist: {
    const size_t name_size = std::min<size_t>(command.text.size(), 255);
    append_i32(payload, command.first);
    append_u16(payload, 0);
    append_u32(payload, static_cast<uint32_t>(name_size));
    payload.insert(payload.end(), command.text.begin(), command.text.begin() + name_size);
    payload.push_back(0);
    send_packet(42, payload.data(), payload.size());
    enqueue({CommandType::request_playlists, -1});
    break;
  }
  case CommandType::request_queue_page:
    load_index_page(static_cast<uint32_t>(std::max<int32_t>(command.first, 0)));
    break;
  }
}

void MonkeyMoteClient::request_initial_state() {
  enqueue({CommandType::request_playlists, -1});
  enqueue({CommandType::request_playlist, -1});
}

void MonkeyMoteClient::parse_track_info(const std::vector<uint8_t> &payload) {
  if (payload.size() < 35) {
    return;
  }
  Track track;
  track.index = read_i32(payload.data());
  track.duration_ms = read_i32(payload.data() + 4);
  track.bitrate = read_u16(payload.data() + 8);
  track.rating = payload[11];
  track.year = read_u16(payload.data() + 28);
  const uint8_t flags = payload[30];
  const std::array<uint8_t, 4> lengths{payload[31], payload[32], payload[33], payload[34]};
  size_t offset = 35;
  std::array<std::string *, 4> fields{&track.title, &track.artist, &track.album, &track.codec};
  for (size_t i = 0; i < fields.size(); ++i) {
    if (offset + lengths[i] + 1 > payload.size()) {
      return;
    }
    fields[i]->assign(reinterpret_cast<const char *>(payload.data() + offset), lengths[i]);
    offset += lengths[i] + 1;
  }
  model_.update([track = std::move(track), flags](Snapshot &state) mutable {
    if ((flags & 2) != 0 || track.index >= 0) {
      state.now_playing = std::move(track);
    }
  });
}

void MonkeyMoteClient::parse_playback(const std::vector<uint8_t> &payload) {
  if (payload.size() < 8) {
    return;
  }
  const int32_t position = read_i32(payload.data());
  const uint8_t state_value = payload[4];
  const bool shuffle = payload[5] != 0;
  const uint8_t repeat = payload[6];
  const uint16_t volume = payload.size() >= 10 ? read_u16(payload.data() + 8)
                                                : static_cast<uint16_t>(payload[7] * 257);
  const int64_t received_us = esp_timer_get_time();
  model_.update([=](Snapshot &state) {
    state.position_ms = position;
    state.position_updated_us = received_us;
    state.playback = state_value <= 3 ? static_cast<PlaybackState>(state_value)
                                      : PlaybackState::unknown;
    state.shuffle = shuffle;
    state.repeat = repeat;
    state.volume = volume;
  });
}

void MonkeyMoteClient::parse_playlists(const std::vector<uint8_t> &payload) {
  if (payload.size() < 12) {
    return;
  }
  const uint32_t count = read_u32(payload.data());
  if (count > 512) {
    return;
  }
  size_t offset = 12;
  std::vector<Playlist> playlists;
  playlists.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (offset + 12 > payload.size()) {
      return;
    }
    Playlist playlist;
    playlist.id = read_i32(payload.data() + offset);
    playlist.count = read_i32(payload.data() + offset + 4);
    const uint16_t flags = read_u16(payload.data() + offset + 8);
    playlist.folder = (flags & 1) != 0;
    playlist.read_only = (flags & 2) != 0;
    offset += 12;
    if (!read_cstring(payload, offset, playlist.name)) {
      return;
    }
    playlists.push_back(std::move(playlist));
  }
  model_.update([playlists = std::move(playlists)](Snapshot &state) mutable {
    for (auto &playlist : playlists) {
      playlist.active = playlist.id == state.active_playlist_id;
    }
    const bool changed = playlists.size() != state.playlists.size() ||
                         !std::equal(playlists.begin(), playlists.end(), state.playlists.begin(),
                                     [](const Playlist &a, const Playlist &b) {
                                       return a.id == b.id && a.count == b.count &&
                                              a.active == b.active && a.folder == b.folder &&
                                              a.read_only == b.read_only && a.name == b.name;
                                     });
    if (changed) {
      state.playlists = std::move(playlists);
      ++state.playlists_revision;
    }
  });
}

void MonkeyMoteClient::parse_playlist(const std::vector<uint8_t> &payload) {
  if (payload.size() < 11) {
    return;
  }
  const uint32_t count = read_u32(payload.data());
  const int32_t playlist_id = read_i32(payload.data() + 4);
  const uint16_t name_length = read_u16(payload.data() + 8);
  const uint8_t flags = payload[10];
  size_t offset = 11;
  if (offset + name_length + 1 > payload.size()) {
    return;
  }
  std::string name(reinterpret_cast<const char *>(payload.data() + offset), name_length);
  offset += name_length + 1;
  if (count > 10000) {
    // Oversized queues are not retained, but still validate the complete
    // record structure before allowing an untrusted frame to alter the model.
    size_t validation_offset = offset;
    for (uint32_t i = 0; i < count; ++i) {
      if (validation_offset > payload.size() || payload.size() - validation_offset < 32) {
        return;
      }
      const std::array<uint16_t, 4> lengths{
          read_u16(payload.data() + validation_offset + 24),
          read_u16(payload.data() + validation_offset + 26),
          read_u16(payload.data() + validation_offset + 28),
          read_u16(payload.data() + validation_offset + 30)};
      validation_offset += 32;
      for (const uint16_t length : lengths) {
        const size_t field_size = static_cast<size_t>(length) + 1;
        if (validation_offset > payload.size() ||
            field_size > payload.size() - validation_offset) {
          return;
        }
        validation_offset += field_size;
      }
    }
    model_.update([playlist_id, name = std::move(name)](Snapshot &state) mutable {
      state.active_playlist_id = playlist_id;
      state.active_playlist_name = name.empty() ? "PLAYLIST TOO LARGE" : std::move(name);
      state.queue.clear();
      ++state.queue_revision;
      state.status = "PLAYLIST TOO LARGE";
    });
    return;
  }
  std::vector<Track> tracks;
  tracks.reserve(std::min<uint32_t>(count, 1000));
  for (uint32_t i = 0; i < count; ++i) {
    if (offset + 32 > payload.size()) {
      return;
    }
    Track track;
    track.index = read_i32(payload.data() + offset);
    track.queue_row = static_cast<int32_t>(i);
    track.duration_ms = read_i32(payload.data() + offset + 4);
    track.bitrate = read_u16(payload.data() + offset + 8);
    track.rating = payload[offset + 11];
    track.year = read_u16(payload.data() + offset + 12);
    const std::array<uint16_t, 4> lengths{
        read_u16(payload.data() + offset + 24), read_u16(payload.data() + offset + 26),
        read_u16(payload.data() + offset + 28), read_u16(payload.data() + offset + 30)};
    offset += 32;
    std::array<std::string *, 4> fields{&track.title, &track.artist, &track.album, &track.codec};
    for (size_t field = 0; field < fields.size(); ++field) {
      if (offset + lengths[field] + 1 > payload.size()) {
        return;
      }
      fields[field]->assign(reinterpret_cast<const char *>(payload.data() + offset), lengths[field]);
      offset += lengths[field] + 1;
    }
    if (tracks.size() < 1000) {
      tracks.push_back(std::move(track));
    }
  }
  model_.update([playlist_id, name = std::move(name), flags, count,
                 tracks = std::move(tracks)](Snapshot &state) mutable {
    state.active_playlist_id = playlist_id;
    if (!name.empty()) {
      state.active_playlist_name = std::move(name);
    }
    const bool queue_changed = tracks.size() != state.queue.size() ||
                               !std::equal(tracks.begin(), tracks.end(), state.queue.begin(),
                                           [](const Track &a, const Track &b) {
                                             return a.index == b.index &&
                                                    a.duration_ms == b.duration_ms &&
                                                    a.bitrate == b.bitrate && a.rating == b.rating &&
                                                    a.year == b.year && a.title == b.title &&
                                                    a.artist == b.artist && a.album == b.album &&
                                                    a.codec == b.codec;
                                           });
    if (queue_changed) {
      state.queue = std::move(tracks);
      state.queue_total_count = count;
      state.queue_page_start = 0;
      state.queue_index_ready = false;
      state.queue_indexing = false;
      ++state.queue_revision;
    }
    bool playlist_state_changed = false;
    for (auto &playlist : state.playlists) {
      const bool active = playlist.id == playlist_id;
      playlist_state_changed = playlist_state_changed || playlist.active != active;
      playlist.active = active;
    }
    if (playlist_state_changed) {
      ++state.playlists_revision;
    }
    (void)flags;
  });
}

void MonkeyMoteClient::parse_compressed(const std::vector<uint8_t> &payload) {
  if (payload.size() < 10) {
    return;
  }
  const uint32_t uncompressed_size = read_u32(payload.data());
  const uint32_t inner_type = read_u32(payload.data() + 4);
  if (inner_type == 45) {
    ESP_LOGW(kTag, "Nested compressed packet rejected");
    return;
  }
  if (uncompressed_size > kMaxPayload) {
    model_.update([](Snapshot &state) {
      state.queue.clear();
      ++state.queue_revision;
      state.active_playlist_name = "PLAYLIST TOO LARGE";
      state.status = "PLAYLIST TOO LARGE";
    });
    return;
  }
  std::vector<uint8_t> uncompressed(uncompressed_size);
  if (!lz4_decompress(payload.data() + 10, payload.size() - 10, uncompressed.data(),
                      uncompressed.size())) {
    ESP_LOGW(kTag, "LZ4 payload failed to decompress");
    return;
  }
  process_frame(inner_type, uncompressed);
}

} // namespace musicremote
