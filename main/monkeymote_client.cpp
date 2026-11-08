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

uint64_t read_u64(const uint8_t *p) {
  return static_cast<uint64_t>(read_u32(p)) |
         (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

int32_t read_i32(const uint8_t *p) { return static_cast<int32_t>(read_u32(p)); }

int32_t resolve_playlist_id(const Snapshot &state, int32_t playlist_id,
                            const std::string &name, uint32_t count) {
  if (playlist_id >= 0) {
    return playlist_id;
  }
  int32_t match = -1;
  for (const auto &playlist : state.playlists) {
    if (!playlist.folder && playlist.name == name && playlist.count >= 0 &&
        static_cast<uint32_t>(playlist.count) == count) {
      if (match >= 0) {
        return -1;
      }
      match = playlist.id;
    }
  }
  return match;
}

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

bool read_string_field(const std::vector<uint8_t> &data, size_t &offset, size_t end,
                       uint16_t length, std::string &out, size_t max_length = 1024) {
  const size_t field_size = static_cast<size_t>(length) + 1;
  if (length > max_length || offset > end || field_size > end - offset || end > data.size() ||
      data[offset + length] != 0) {
    return false;
  }
  out.assign(reinterpret_cast<const char *>(data.data() + offset), length);
  offset += field_size;
  return true;
}

bool read_bounded_cstring(const std::vector<uint8_t> &data, size_t &offset, std::string &out,
                          size_t max_length = 1024) {
  if (offset >= data.size()) {
    return false;
  }
  const auto begin = data.begin() + static_cast<ptrdiff_t>(offset);
  const auto end = std::find(begin, data.end(), 0);
  if (end == data.end() || static_cast<size_t>(end - begin) > max_length) {
    return false;
  }
  out.assign(reinterpret_cast<const char *>(data.data() + offset),
             static_cast<size_t>(end - begin));
  offset = static_cast<size_t>(end - data.begin()) + 1;
  return true;
}

void append_optional_string(std::vector<uint8_t> &payload, const std::string &value) {
  if (!value.empty()) {
    payload.insert(payload.end(), value.begin(), value.end());
    payload.push_back(0);
  }
}

uint32_t library_path_flags(const LibraryPath &path) {
  return (path.genre.empty() ? 0u : 1u) | (path.artist.empty() ? 0u : 2u) |
         (path.album.empty() ? 0u : 4u) | (path.composer.empty() ? 0u : 8u);
}

std::string artwork_track_key(const Track &track) {
  if (track.title.empty()) {
    return {};
  }
  mbedtls_md5_context md5;
  mbedtls_md5_init(&md5);
  mbedtls_md5_starts(&md5);
  const auto add = [&md5](const void *data, size_t size) {
    mbedtls_md5_update(&md5, static_cast<const unsigned char *>(data), size);
  };
  const auto add_string = [&add](const std::string &value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    add(&size, sizeof(size));
    add(value.data(), value.size());
  };
  add(&track.duration_ms, sizeof(track.duration_ms));
  add_string(track.title);
  add_string(track.artist);
  add_string(track.album);
  std::array<unsigned char, 16> digest{};
  mbedtls_md5_finish(&md5, digest.data());
  mbedtls_md5_free(&md5);
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(32, '0');
  for (size_t index = 0; index < digest.size(); ++index) {
    result[index * 2] = kHex[digest[index] >> 4];
    result[index * 2 + 1] = kHex[digest[index] & 0x0f];
  }
  return result;
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
      playlist_index_(config_.storage_directory, config_.storage_internal_flash),
      artwork_decoder_(model_, {.storage_directory = config_.storage_directory,
                                .storage_internal_flash = config_.storage_internal_flash}) {}

MonkeyMoteClient::~MonkeyMoteClient() { stop(); }

void MonkeyMoteClient::start() {
  if (running_.exchange(true)) {
    return;
  }
  if (!artwork_decoder_.start()) {
    running_ = false;
    model_.update([](Snapshot &state) {
      state.connection = ConnectionState::offline;
      state.status = "COVER TASK FAILED";
    });
    return;
  }
  TaskHandle_t handle = nullptr;
  if (xTaskCreatePinnedToCore(task_entry, "monkeymote", 12 * 1024, this, 5, &handle, 0) != pdPASS) {
    running_ = false;
    artwork_decoder_.stop();
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
  artwork_decoder_.stop();
  close_socket();
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

void MonkeyMoteClient::index_task_entry(void *context) {
  static_cast<MonkeyMoteClient *>(context)->build_streamed_index();
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
      expire_requests(now);
      if (socket_ < 0) {
        break;
      }
      if (pending_library_play_row_ >= 0 &&
          now >= pending_library_play_deadline_us_.load()) {
        fail_pending_library_play("LIBRARY PLAY TIMED OUT");
      }
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
  cover_requests_.clear();
  cover_track_key_.clear();
  lyrics_requests_.clear();
  library_request_deadline_us_ = 0;
  pending_library_play_row_ = -1;
  pending_library_play_destination_ = -1;
  pending_library_play_deadline_us_ = 0;
  pending_library_play_token_ = 0;
  {
    std::lock_guard lock(playlist_requests_mutex_);
    playlist_requests_.clear();
    streamed_playlist_request_.reset();
    streamed_playlist_generation_ = 0;
  }
  artwork_decoder_.select_track({});
  last_receive_us_ = esp_timer_get_time();
  last_ping_us_ = last_receive_us_;
  model_.update([](Snapshot &state) {
    state.connection = ConnectionState::online;
    ++state.connection_generation;
    state.active_playlist_count_known = false;
    state.library_play_pending = false;
    state.status = "AIMP ONLINE";
    if (state.library_loading) {
      state.library_loading = false;
      state.library_message = "LIBRARY REQUEST INTERRUPTED";
      ++state.library_revision;
    }
    if (state.lyrics_state == LyricsState::loading) {
      state.lyrics_state = LyricsState::failed;
      state.lyrics = std::make_shared<const std::string>("Lyrics request interrupted.");
      ++state.lyrics_revision;
    }
  });
  ESP_LOGI(kTag, "Authenticated to %s:%u", config_.host.c_str(), config_.port);
  return true;
}

void MonkeyMoteClient::close_socket() {
  abort_streamed_frame("LIBRARY TRANSFER INTERRUPTED");
  const int old_socket = socket_;
  socket_ = -1;
  if (old_socket >= 0) {
    shutdown(old_socket, SHUT_RDWR);
    close(old_socket);
  }
  lyrics_requests_.clear();
  library_request_deadline_us_ = 0;
  pending_library_play_row_ = -1;
  pending_library_play_destination_ = -1;
  pending_library_play_deadline_us_ = 0;
  pending_library_play_token_ = 0;
  {
    std::lock_guard lock(playlist_requests_mutex_);
    playlist_requests_.clear();
    streamed_playlist_request_.reset();
    streamed_playlist_generation_ = 0;
  }
  model_.update([](Snapshot &state) {
    state.active_playlist_count_known = false;
    state.library_play_pending = false;
    state.capabilities_known = false;
    state.server_capabilities = 0;
    bool library_changed = false;
    bool lyrics_changed = false;
    if (state.library_loading) {
      state.library_loading = false;
      state.library_message = "LIBRARY REQUEST INTERRUPTED";
      library_changed = true;
    }
    if (state.lyrics_state == LyricsState::loading) {
      // Returning to none permits the open overlay to request the current
      // track again after the connection is re-established.
      state.lyrics_state = LyricsState::none;
      state.lyrics = std::make_shared<const std::string>();
      lyrics_changed = true;
    }
    if (library_changed) {
      ++state.library_revision;
    }
    if (lyrics_changed) {
      ++state.lyrics_revision;
    }
  });
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
    if (type == 45 && length > kMaxPayload && receive_buffer_.size() < 22) {
      break;
    }
    const bool compressed_envelope_ready =
        type == 45 && length >= 10 && receive_buffer_.size() >= 22;
    const uint32_t advertised_size =
        compressed_envelope_ready ? read_u32(receive_buffer_.data() + 12) : 0;
    const uint32_t inner_type =
        compressed_envelope_ready ? read_u32(receive_buffer_.data() + 16) : 0;
    const bool stream_playlist =
        compressed_envelope_ready && advertised_size <= kMaxStreamedDecoded &&
        (inner_type == 9 || inner_type == 31 || inner_type == 50) &&
        (length > kMaxPayload || advertised_size > kMaxPayload);
    const uint64_t stream_generation =
        stream_playlist ? playlist_stream_generation_.fetch_add(1) + 1 : 0;
    if (stream_playlist || length > kMaxPayload) {
      receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + 12);
      const bool oversized_library =
          type == 19 || (compressed_envelope_ready && inner_type == 19);
      if (oversized_library) {
        ESP_LOGW(kTag, "Draining oversized library reply: %" PRIu32, length);
        discard_remaining_ = length;
        library_request_deadline_us_ = 0;
        model_.update([](Snapshot &state) {
          state.library_loading = false;
          state.library_items = std::make_shared<const LibraryItems>();
          state.library_total_count = 0;
          state.library_truncated = true;
          state.library_message = "LIBRARY RESULT TOO LARGE";
          state.status = "LIBRARY RESULT TOO LARGE";
          ++state.library_revision;
        });
        continue;
      }
      const bool oversized_lyrics =
          type == 36 || (compressed_envelope_ready && inner_type == 36);
      if (oversized_lyrics) {
        ESP_LOGW(kTag, "Draining oversized lyrics reply: %" PRIu32, length);
        reject_lyrics_reply("Lyrics exceed the 24 KiB display limit.");
        discard_remaining_ = length;
        continue;
      }
      if (type == 8 || type == 13 ||
          (compressed_envelope_ready &&
           (inner_type == 8 || inner_type == 13))) {
        reject_cover_reply();
        ESP_LOGW(kTag, "Draining oversized cover reply: %" PRIu32, length);
        discard_remaining_ = length;
        continue;
      }
      if (stream_playlist && index_building_) {
        index_superseded_ = true;
        if (index_building_) {
          ESP_LOGI(kTag, "Deferring large playlist received while index build is active");
          discard_remaining_ = length;
          consume_playlist_response(false);
          // AIMP has already switched to the newer playlist. Hide rows from
          // any intervening queue until the worker can request this frame
          // again; otherwise a tap would play the same row in the new queue.
          model_.update([](Snapshot &state) {
            state.queue.clear();
            state.queue_total_count = 0;
            state.active_playlist_count_known = false;
            state.queue_page_start = 0;
            state.queue_index_ready = false;
            state.queue_indexing = true;
            ++state.queue_revision;
            state.active_playlist_name = "LOADING LIBRARY";
            state.status = "REFRESHING LIBRARY";
          });
          continue;
        }
      }
      if (stream_playlist && length <= kMaxStreamedPayload &&
          start_streamed_frame(type, length, stream_generation)) {
        continue;
      }
      const bool storage_failed = stream_playlist && length <= kMaxStreamedPayload &&
                                  playlist_index_.available();
      ESP_LOGW(kTag, "Draining oversized payload: %" PRIu32, length);
      discard_remaining_ = length;
      if (stream_playlist || type == 9 || type == 31 || type == 50) {
        consume_playlist_response(false);
      }
      model_.update([storage_failed](Snapshot &state) {
        state.queue.clear();
        state.queue_total_count = 0;
        state.active_playlist_count_known = false;
        state.queue_page_start = 0;
        state.queue_index_ready = false;
        state.queue_indexing = false;
        ++state.queue_revision;
        state.active_playlist_name =
            storage_failed ? "PLAYLIST UNAVAILABLE" : "PLAYLIST TOO LARGE";
        state.status = storage_failed ? "SD CARD UNAVAILABLE" : "PLAYLIST TOO LARGE";
      });
      continue;
    }
    if (receive_buffer_.size() < 12 + length) {
      break;
    }
    std::vector<uint8_t> payload;
    if (receive_buffer_.size() == 12 + length) {
      receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + 12);
      payload.swap(receive_buffer_);
    } else {
      payload.assign(receive_buffer_.begin() + 12,
                     receive_buffer_.begin() + 12 + length);
      receive_buffer_.erase(receive_buffer_.begin(),
                            receive_buffer_.begin() + 12 + length);
      if (receive_buffer_.capacity() > 64 * 1024 && receive_buffer_.size() < 8 * 1024) {
        std::vector<uint8_t> compact(receive_buffer_);
        receive_buffer_.swap(compact);
      }
    }
    if (!process_frame(type, payload)) {
      ESP_LOGW(kTag, "Malformed packet type=%" PRIu32 " bytes=%" PRIu32, type, length);
    }
  }
  return true;
}

bool MonkeyMoteClient::start_streamed_frame(uint32_t type, uint32_t length,
                                            uint64_t generation) {
  if (type != 45 || !playlist_index_.available()) {
    return false;
  }
  index_superseded_ = false;
  streamed_frame_ = std::fopen(playlist_index_.incoming_path().c_str(), "wb");
  if (streamed_frame_ == nullptr) {
    ESP_LOGW(kTag, "Could not open playlist stream file %s: errno=%d",
             playlist_index_.incoming_path().c_str(), errno);
    return false;
  }
  streamed_remaining_ = length;
  receiving_stream_generation_ = generation;
  {
    std::lock_guard lock(playlist_requests_mutex_);
    if (!playlist_requests_.empty()) {
      // Large frames must be written to storage before their token is popped;
      // allow that bounded transfer for every playlist request.
      const int64_t stream_deadline_us = esp_timer_get_time() + 120'000'000;
      playlist_requests_.front().deadline_us = stream_deadline_us;
      if (playlist_requests_.front().confirms_library_play &&
          pending_library_play_token_ == playlist_requests_.front().id) {
        pending_library_play_deadline_us_ = stream_deadline_us;
      }
    }
  }
  queue_filter_active_ = false;
  queue_filter_.clear();
  model_.update([](Snapshot &state) {
    state.queue.clear();
    state.queue_total_count = 0;
    state.active_playlist_count_known = false;
    state.queue_page_start = 0;
    state.queue_index_ready = false;
    state.queue_indexing = true;
    state.queue_filter_active = false;
    state.queue_filter_label.clear();
    ++state.queue_revision;
    state.active_playlist_name = "LOADING LIBRARY";
    state.status = "RECEIVING LIBRARY";
  });
  ESP_LOGI(kTag, "Streaming %" PRIu32 "-byte playlist frame to index storage", length);
  return true;
}

bool MonkeyMoteClient::consume_streamed_frame() {
  const size_t count = std::min(streamed_remaining_, receive_buffer_.size());
  if (count != 0 && std::fwrite(receive_buffer_.data(), 1, count, streamed_frame_) != count) {
    receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + count);
    std::fclose(streamed_frame_);
    streamed_frame_ = nullptr;
    unlink(playlist_index_.incoming_path().c_str());
    streamed_remaining_ -= count;
    discard_remaining_ = streamed_remaining_;
    streamed_remaining_ = 0;
    consume_playlist_response(false);
    model_.update([](Snapshot &state) {
      state.queue_indexing = false;
      state.active_playlist_name = "PLAYLIST UNAVAILABLE";
      state.status = "STORAGE WRITE FAILED";
    });
    return true;
  }
  receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + count);
  streamed_remaining_ -= count;
  if (streamed_remaining_ == 0) {
    const bool flushed = std::fflush(streamed_frame_) == 0 &&
                         fsync(fileno(streamed_frame_)) == 0;
    const bool closed = std::fclose(streamed_frame_) == 0;
    streamed_frame_ = nullptr;
    if (!flushed || !closed) {
      unlink(playlist_index_.incoming_path().c_str());
      consume_playlist_response(false);
      model_.update([](Snapshot &state) {
        state.queue_indexing = false;
        state.active_playlist_name = "PLAYLIST UNAVAILABLE";
        state.status = "STORAGE WRITE FAILED";
      });
      return true;
    }
    finish_streamed_frame();
  }
  return true;
}

void MonkeyMoteClient::finish_streamed_frame() {
  if (index_building_.exchange(true)) {
    unlink(playlist_index_.incoming_path().c_str());
    consume_playlist_response(false);
    return;
  }
  {
    std::lock_guard lock(playlist_requests_mutex_);
    if (!playlist_requests_.empty()) {
      streamed_playlist_request_ = playlist_requests_.front();
      playlist_requests_.pop_front();
      streamed_playlist_generation_ = receiving_stream_generation_;
      if (streamed_playlist_request_->confirms_library_play &&
          pending_library_play_token_ == streamed_playlist_request_->id) {
        // A changed source hash can require a full 48k-row rebuild. This
        // bounds stalls while leaving margin over the measured ~110 s build.
        pending_library_play_deadline_us_ = esp_timer_get_time() + 180'000'000;
      }
    } else {
      streamed_playlist_request_.reset();
      streamed_playlist_generation_ = 0;
    }
  }
  model_.update([](Snapshot &state) { state.status = "INDEXING LIBRARY"; });
  TaskHandle_t handle = nullptr;
  if (xTaskCreatePinnedToCore(index_task_entry, "playlist_index", 12 * 1024, this, 4, &handle,
                              1) != pdPASS) {
    index_building_ = false;
    unlink(playlist_index_.incoming_path().c_str());
    std::optional<PlaylistRequestToken> token;
    {
      std::lock_guard lock(playlist_requests_mutex_);
      token = streamed_playlist_request_;
      streamed_playlist_request_.reset();
      streamed_playlist_generation_ = 0;
    }
    finish_playlist_response(token, false);
    model_.update([](Snapshot &state) {
      state.queue_indexing = false;
      state.active_playlist_name = "PLAYLIST UNAVAILABLE";
      state.status = "INDEX TASK FAILED";
    });
  }
}

void MonkeyMoteClient::build_streamed_index() {
  std::optional<PlaylistRequestToken> response_token;
  uint64_t response_generation = 0;
  {
    std::lock_guard lock(playlist_requests_mutex_);
    response_token = streamed_playlist_request_;
    response_generation = streamed_playlist_generation_;
    streamed_playlist_request_.reset();
    streamed_playlist_generation_ = 0;
  }
  const bool refresh_confirmation_deadline =
      response_token && response_token->confirms_library_play;
  const auto result = playlist_index_.build_from_compressed(
      [this, refresh_confirmation_deadline](uint32_t completed, uint32_t total) {
        if (refresh_confirmation_deadline) {
          // Measure a stalled build rather than total playlist size.
          pending_library_play_deadline_us_ = esp_timer_get_time() + 60'000'000;
        }
        if (index_superseded_) {
          return;
        }
        if (completed == total || completed % 4096 == 1) {
          model_.update([completed, total](Snapshot &state) {
            if (!state.queue_indexing) {
              return;
            }
            state.status = "INDEXING " +
                           std::to_string(completed * 100 /
                                          std::max<uint32_t>(1, total)) +
                           "%";
          });
        }
      });
  unlink(playlist_index_.incoming_path().c_str());
  bool superseded = index_superseded_.exchange(false);
  if (response_token && response_token->confirms_library_play) {
    Command completion{CommandType::streamed_playlist_result};
    completion.first = result.playlist_id >= 0 ? result.playlist_id
                                               : response_token->playlist_id;
    completion.second = result.count <= static_cast<uint32_t>(INT32_MAX)
                            ? static_cast<int32_t>(result.count)
                            : -1;
    completion.text = result.success && !superseded ? "valid" : "invalid";
    completion.request_token = response_token->id;
    completion.stream_generation = response_generation;
    enqueue(std::move(completion));
  }
  if (!result.success) {
    ESP_LOGW(kTag, "Large playlist index failed: %s", result.error.c_str());
    model_.update([this, message = result.error, superseded](Snapshot &state) {
      if (!state.queue_indexing || superseded || index_superseded_) {
        return;
      }
      state.queue.clear();
      state.queue_total_count = 0;
      state.active_playlist_count_known = false;
      state.queue_page_start = 0;
      state.queue_index_ready = false;
      state.queue_indexing = false;
      ++state.queue_revision;
      state.active_playlist_name = "PLAYLIST TOO LARGE";
      state.status = message;
    });
    index_building_ = false;
  } else {
    ESP_LOGI(kTag, "Indexed %" PRIu32 " tracks for playlist %" PRId32, result.count,
             result.playlist_id);
    if (!superseded) {
      load_index_page(0, true);
      if (result.stale) {
        model_.update([](Snapshot &state) { state.status = "INDEX STALE - RETRY LATER"; });
      }
    }
    index_building_ = false;
  }
  superseded = index_superseded_.exchange(false) || superseded;
  if (superseded) {
    model_.update([](Snapshot &state) {
      if (state.queue_indexing) {
        state.queue.clear();
        state.queue_total_count = 0;
        state.active_playlist_count_known = false;
        state.queue_page_start = 0;
        state.queue_index_ready = false;
        ++state.queue_revision;
        state.status = "REFRESHING LIBRARY";
      }
    });
    enqueue({CommandType::request_playlist, -1});
  }
}

void MonkeyMoteClient::abort_streamed_frame(const char *status) {
  if (streamed_frame_ == nullptr) {
    return;
  }
  std::fclose(streamed_frame_);
  streamed_frame_ = nullptr;
  streamed_remaining_ = 0;
  unlink(playlist_index_.incoming_path().c_str());
  consume_playlist_response(false);
  model_.update([status](Snapshot &state) {
    state.queue.clear();
    state.queue_total_count = 0;
    state.active_playlist_count_known = false;
    state.queue_page_start = 0;
    state.queue_index_ready = false;
    state.queue_indexing = false;
    ++state.queue_revision;
    state.active_playlist_name = "PLAYLIST UNAVAILABLE";
    state.status = status;
  });
}

void MonkeyMoteClient::load_index_page(uint32_t start, bool require_indexing) {
  std::vector<Track> tracks;
  uint32_t total = 0;
  int32_t playlist_id = -1;
  std::string name;
  if (!playlist_index_.load_page(start, 100, tracks, total, playlist_id, name)) {
    model_.update([this, require_indexing](Snapshot &state) {
      if (require_indexing && (!state.queue_indexing || index_superseded_)) {
        return;
      }
      state.queue.clear();
      state.queue_total_count = 0;
      state.active_playlist_count_known = false;
      state.queue_page_start = 0;
      state.queue_indexing = false;
      state.queue_index_ready = false;
      ++state.queue_revision;
      state.active_playlist_name = "PLAYLIST UNAVAILABLE";
      state.status = "INDEX READ FAILED";
    });
    return;
  }
  model_.update([this, start, total, playlist_id, require_indexing, name = std::move(name),
                 tracks = std::move(tracks)](Snapshot &state) mutable {
    // The network task sets index_superseded_ before its deferral update. Check
    // it while publishing under the model lock so an older page cannot replace
    // the refresh state with tappable rows.
    if (require_indexing && (!state.queue_indexing || index_superseded_)) {
      return;
    }
    int32_t resolved_playlist_id = playlist_id;
    if (resolved_playlist_id < 0 && state.queue_index_ready) {
      resolved_playlist_id = state.active_playlist_id;
    }
    if (resolved_playlist_id < 0) {
      resolved_playlist_id = resolve_playlist_id(state, playlist_id, name, total);
    }
    if (!state.queue_indexing && state.active_playlist_id >= 0 && resolved_playlist_id >= 0 &&
        state.active_playlist_id != resolved_playlist_id) {
      state.queue.clear();
      state.queue_total_count = 0;
      state.active_playlist_count_known = false;
      state.queue_page_start = 0;
      state.queue_index_ready = false;
      ++state.queue_revision;
      state.status = "STALE PLAYLIST INDEX";
      return;
    }
    state.active_playlist_id = resolved_playlist_id;
    state.active_playlist_name = std::move(name);
    bool playlist_state_changed = false;
    for (auto &playlist : state.playlists) {
      const bool active = playlist.id == resolved_playlist_id;
      playlist_state_changed = playlist_state_changed || playlist.active != active;
      playlist.active = active;
    }
    if (playlist_state_changed) {
      ++state.playlists_revision;
    }
    state.queue = std::move(tracks);
    state.queue_total_count = total;
    state.active_playlist_track_count = total;
    state.active_playlist_count_known = true;
    state.queue_page_start = start;
    state.queue_indexing = false;
    state.queue_index_ready = true;
    state.queue_filter_active = false;
    state.queue_filter_label.clear();
    state.status = "LIBRARY READY";
    ++state.queue_revision;
  });
}

void MonkeyMoteClient::search_index_page(uint32_t start, bool require_indexing) {
  std::vector<Track> tracks;
  uint32_t total = 0;
  int32_t playlist_id = -1;
  std::string name;
  if (!playlist_index_.search_page(queue_filter_, queue_filter_mode_, start, 100, tracks,
                                   total, playlist_id, name)) {
    model_.update([this, require_indexing](Snapshot &state) {
      if (require_indexing && (!state.queue_indexing || index_superseded_)) {
        return;
      }
      state.queue.clear();
      state.queue_total_count = 0;
      state.queue_page_start = 0;
      state.queue_indexing = false;
      state.queue_filter_active = false;
      state.queue_filter_label.clear();
      ++state.queue_revision;
      state.status = "SEARCH INDEX FAILED";
    });
    return;
  }
  const std::string label = queue_filter_mode_ == PlaylistIndex::SearchMode::contains
                                ? "FIND: " + queue_filter_
                            : queue_filter_mode_ == PlaylistIndex::SearchMode::non_alpha
                                ? "A-Z: #"
                                : "A-Z: " + queue_filter_;
  model_.update([this, start, total, playlist_id, require_indexing, label,
                 name = std::move(name), tracks = std::move(tracks)](Snapshot &state) mutable {
    if (require_indexing && (!state.queue_indexing || index_superseded_)) {
      return;
    }
    int32_t resolved_playlist_id = playlist_id;
    if (resolved_playlist_id < 0) {
      resolved_playlist_id = state.active_playlist_id;
    }
    if (state.active_playlist_id >= 0 && resolved_playlist_id >= 0 &&
        state.active_playlist_id != resolved_playlist_id) {
      state.status = "STALE PLAYLIST INDEX";
      return;
    }
    state.active_playlist_id = resolved_playlist_id;
    state.active_playlist_name = std::move(name);
    state.queue = std::move(tracks);
    state.queue_total_count = total;
    state.queue_page_start = start;
    state.queue_indexing = false;
    state.queue_index_ready = true;
    state.queue_filter_active = true;
    state.queue_filter_label = label;
    state.status = total == 0 ? "NO MATCHES" : "FILTER READY";
    ++state.queue_revision;
  });
}

bool MonkeyMoteClient::process_frame(uint32_t type, std::vector<uint8_t> &payload) {
  switch (type) {
  case 5:
    parse_track_info(payload);
    return true;
  case 6:
    parse_playback(payload);
    return true;
  case 8: {
    // Cover reply: uint32 JPEG size, 16-byte server identity, then JPEG bytes.
    std::string response_track_key = take_cover_request();
    if (response_track_key.empty()) {
      ESP_LOGW(kTag, "Ignoring cover reply without a matching request");
      return true;
    }
    if (payload.size() < 24) {
      if (response_track_key == cover_track_key_) {
        artwork_decoder_.mark_unavailable(response_track_key);
      }
      return false;
    }
    const uint32_t jpeg_size = read_u32(payload.data());
    if (jpeg_size != payload.size() - 20 || jpeg_size > 256 * 1024 ||
        payload[20] != 0xff || payload[21] != 0xd8 ||
        payload[payload.size() - 2] != 0xff || payload.back() != 0xd9) {
      if (response_track_key == cover_track_key_) {
        artwork_decoder_.mark_unavailable(response_track_key);
      }
      return false;
    }
    if (response_track_key != cover_track_key_) {
      ESP_LOGI(kTag, "Discarded stale cover reply");
      return true;
    }
    std::vector<uint8_t> jpeg(payload.begin() + 20, payload.end());
    artwork_decoder_.submit_jpeg(response_track_key, std::move(jpeg));
    return true;
  }
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
    playlist_stream_generation_.fetch_add(1);
    if (index_building_) {
      index_superseded_ = true;
    }
    model_.update([](Snapshot &state) { state.active_playlist_count_known = false; });
    enqueue({CommandType::request_playlist, -1});
    return true;
  case 13: {
    // Type 13 is the empty response to a cover query when the current item has
    // no embedded or folder artwork. A malformed reply still consumes its
    // request so later replies stay matched.
    std::string response_track_key = take_cover_request();
    if (!response_track_key.empty() && response_track_key == cover_track_key_) {
      artwork_decoder_.mark_unavailable(response_track_key);
    }
    return payload.empty();
  }
  case 19:
    if (!parse_library(payload)) {
      model_.update([](Snapshot &state) {
        state.library_loading = false;
        state.library_message = "LIBRARY RESPONSE INVALID";
        state.status = "LIBRARY RESPONSE INVALID";
        ++state.library_revision;
      });
      return false;
    }
    return true;
  case 23:
    send_packet(23);
    return true;
  case 28:
    if (payload.size() != 8) {
      return false;
    }
    model_.update([capabilities = read_u64(payload.data())](Snapshot &state) {
      state.capabilities_known = true;
      state.server_capabilities = capabilities;
    });
    return true;
  case 36:
    return parse_lyrics(payload);
  case 41:
    parse_playlists(payload);
    return true;
  case 45:
    parse_compressed(payload);
    return true;
  case 9:
  case 31:
  case 50: {
    const bool valid = parse_playlist(payload);
    if (!valid) {
      consume_playlist_response(false);
    }
    return valid;
  }
  case 43:
    if (!payload.empty() && payload[0] != 0) {
      const bool library_play_pending = pending_library_play_row_ >= 0;
      fail_pending_library_play("LIBRARY SELECTION FAILED");
      model_.update([](Snapshot &state) { state.status = "AIMP COMMAND FAILED"; });
      // A rejection does not identify which later queue response, if any,
      // belongs to the already-sent confirmation request. Reset the stream.
      if (library_play_pending) {
        close_socket();
      }
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

bool MonkeyMoteClient::send_playlist_request(int32_t playlist_id,
                                             bool confirms_library_play) {
  {
    std::lock_guard lock(playlist_requests_mutex_);
    if (!confirms_library_play && playlist_id == -1 &&
        std::any_of(playlist_requests_.begin(), playlist_requests_.end(),
                    [playlist_id](const PlaylistRequestToken &token) {
                      return !token.confirms_library_play &&
                             token.playlist_id == playlist_id;
                    })) {
      return true;
    }
    if (playlist_requests_.size() >= 8) {
      ESP_LOGW(kTag, "Playlist response queue is full");
      return false;
    }
  }
  std::vector<uint8_t> payload{10};
  append_i32(payload, playlist_id);
  append_i32(payload, 1);
  if (!send_packet(4, payload.data(), payload.size())) {
    return false;
  }
  const uint64_t token_id = next_playlist_request_token_++;
  {
    std::lock_guard lock(playlist_requests_mutex_);
    const bool becomes_head = playlist_requests_.empty();
    playlist_requests_.push_back({.id = token_id,
                                  .playlist_id = playlist_id,
                                  .confirms_library_play = confirms_library_play,
                                  .deadline_us = becomes_head
                                                     ? esp_timer_get_time() + 15'000'000
                                                     : 0});
  }
  if (confirms_library_play) {
    pending_library_play_token_ = token_id;
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
  auto invalidate_streamed_playlist = [this]() {
    playlist_stream_generation_.fetch_add(1);
    if (index_building_) {
      index_superseded_ = true;
    }
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
    send_playlist_request(command.first);
    break;
  case CommandType::activate_playlist:
    if (pending_library_play_row_ >= 0) {
      fail_pending_library_play("LIBRARY PLAY CANCELLED");
    }
    queue_filter_active_ = false;
    queue_filter_.clear();
    invalidate_streamed_playlist();
    model_.update([](Snapshot &state) { state.active_playlist_count_known = false; });
    simple(12, command.first);
    enqueue({CommandType::request_playlist, command.first});
    break;
  case CommandType::play_index:
    if (pending_library_play_row_ >= 0) {
      fail_pending_library_play("LIBRARY PLAY CANCELLED");
    }
    simple(10, command.first);
    simple(1, 0);
    break;
  case CommandType::delete_index:
    if (pending_library_play_row_ >= 0) {
      fail_pending_library_play("LIBRARY PLAY CANCELLED");
    }
    invalidate_streamed_playlist();
    model_.update([](Snapshot &state) { state.active_playlist_count_known = false; });
    append_i32(payload, command.first);
    append_i32(payload, command.second);
    send_packet(32, payload.data(), payload.size());
    enqueue({CommandType::request_playlist, command.first});
    break;
  case CommandType::clear_playlist:
    if (pending_library_play_row_ >= 0) {
      fail_pending_library_play("LIBRARY PLAY CANCELLED");
    }
    invalidate_streamed_playlist();
    model_.update([](Snapshot &state) { state.active_playlist_count_known = false; });
    append_i32(payload, command.first);
    send_packet(20, payload.data(), payload.size());
    enqueue({CommandType::request_playlist, command.first});
    break;
  case CommandType::delete_playlist:
    if (pending_library_play_row_ >= 0) {
      fail_pending_library_play("LIBRARY PLAY CANCELLED");
    }
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
    if (model_.copy().queue_index_ready) {
      const uint32_t start = static_cast<uint32_t>(std::max<int32_t>(command.first, 0));
      if (queue_filter_active_) {
        search_index_page(start);
      } else {
        load_index_page(start);
      }
    }
    break;
  case CommandType::filter_queue:
    if (model_.copy().queue_index_ready &&
        command.first >= static_cast<int32_t>(PlaylistIndex::SearchMode::contains) &&
        command.first <= static_cast<int32_t>(PlaylistIndex::SearchMode::non_alpha)) {
      queue_filter_active_ = true;
      queue_filter_mode_ = static_cast<PlaylistIndex::SearchMode>(command.first);
      queue_filter_ = command.text;
      search_index_page(static_cast<uint32_t>(std::max<int32_t>(command.second, 0)));
    }
    break;
  case CommandType::clear_queue_filter:
    queue_filter_active_ = false;
    queue_filter_.clear();
    if (model_.copy().queue_index_ready) {
      load_index_page(0);
    }
    break;
  case CommandType::request_library_root: {
    static constexpr std::array<uint8_t, 4> kSubcommands{7, 16, 9, 12};
    static constexpr std::array<const char *, 4> kTitles{"ARTISTS", "ALBUMS", "GENRES",
                                                        "COMPOSERS"};
    const size_t category = static_cast<size_t>(std::clamp<int32_t>(command.first, 0, 3));
    library_request_id_ = library_request_id_ == UINT32_MAX ? 1 : library_request_id_ + 1;
    pending_library_path_ = {};
    pending_library_title_ = kTitles[category];
    pending_library_search_ = false;
    payload.push_back(kSubcommands[category]);
    append_u32(payload, library_request_id_);
    model_.update([title = pending_library_title_](Snapshot &state) {
      state.library_loading = true;
      state.library_items = std::make_shared<const LibraryItems>();
      state.library_total_count = 0;
      state.library_title = title;
      state.library_message.clear();
      state.status = "LOADING LIBRARY";
      ++state.library_revision;
    });
    if (!send_packet(4, payload.data(), payload.size())) {
      library_request_deadline_us_ = 0;
      model_.update([](Snapshot &state) {
        state.library_loading = false;
        state.library_message = "LIBRARY REQUEST FAILED";
        state.status = "LIBRARY REQUEST FAILED";
        ++state.library_revision;
      });
    } else {
      library_request_deadline_us_ = esp_timer_get_time() + 15'000'000;
    }
    break;
  }
  case CommandType::browse_library:
    request_library(command.library_path, command.text);
    break;
  case CommandType::search_library: {
    const size_t query_size = std::min<size_t>(command.text.size(), 128);
    if (query_size == 0) {
      break;
    }
    library_request_id_ = library_request_id_ == UINT32_MAX ? 1 : library_request_id_ + 1;
    pending_library_path_ = {};
    pending_library_title_ = "SEARCH: " + command.text.substr(0, query_size);
    pending_library_search_ = true;
    append_u32(payload, library_request_id_);
    payload.insert(payload.end(), command.text.begin(), command.text.begin() + query_size);
    payload.push_back(0);
    model_.update([title = pending_library_title_](Snapshot &state) {
      state.library_loading = true;
      state.library_items = std::make_shared<const LibraryItems>();
      state.library_total_count = 0;
      state.library_title = title;
      state.library_message.clear();
      state.status = "SEARCHING LIBRARY";
      ++state.library_revision;
    });
    if (!send_packet(24, payload.data(), payload.size())) {
      library_request_deadline_us_ = 0;
      model_.update([](Snapshot &state) {
        state.library_loading = false;
        state.library_message = "LIBRARY SEARCH FAILED";
        state.status = "LIBRARY SEARCH FAILED";
        ++state.library_revision;
      });
    } else {
      library_request_deadline_us_ = esp_timer_get_time() + 15'000'000;
    }
    break;
  }
  case CommandType::play_library_item:
    select_library_item(command, true);
    break;
  case CommandType::enqueue_library_item:
    select_library_item(command, false);
    break;
  case CommandType::request_lyrics:
    if (cover_track_key_.empty()) {
      model_.update([](Snapshot &state) {
        state.lyrics_state = LyricsState::unavailable;
        state.lyrics = std::make_shared<const std::string>("No track is playing.");
        ++state.lyrics_revision;
      });
      break;
    }
    payload.push_back(11);
    append_i32(payload, 0);
    model_.update([key = cover_track_key_](Snapshot &state) {
      state.lyrics_state = LyricsState::loading;
      state.lyrics = std::make_shared<const std::string>();
      state.lyrics_track_key = key;
      ++state.lyrics_revision;
    });
    if (lyrics_requests_.size() >= 32 ||
        !send_packet(4, payload.data(), payload.size())) {
      model_.update([](Snapshot &state) {
        state.lyrics_state = LyricsState::failed;
        state.lyrics = std::make_shared<const std::string>("Lyrics request failed.");
        ++state.lyrics_revision;
      });
    } else {
      lyrics_requests_.push_back(
          {.track_key = cover_track_key_,
           .deadline_us = esp_timer_get_time() + 15'000'000});
    }
    break;
  case CommandType::streamed_playlist_result: {
    PlaylistRequestToken token{.id = command.request_token,
                               .playlist_id = command.first,
                               .confirms_library_play = true};
    const bool valid = command.text == "valid" && command.second >= 0 &&
                       command.stream_generation == playlist_stream_generation_.load();
    finish_playlist_response(token, valid, command.first,
                             valid ? static_cast<uint32_t>(command.second) : 0);
    break;
  }
  }
}

void MonkeyMoteClient::request_library(const LibraryPath &path, std::string title) {
  const uint32_t flags = library_path_flags(path);
  if (flags == 0) {
    return;
  }
  library_request_id_ = library_request_id_ == UINT32_MAX ? 1 : library_request_id_ + 1;
  pending_library_path_ = path;
  pending_library_title_ = title.empty() ? "BROWSING" : std::move(title);
  pending_library_search_ = false;
  std::vector<uint8_t> payload;
  append_u32(payload, library_request_id_);
  append_u32(payload, flags);
  append_optional_string(payload, path.genre);
  append_optional_string(payload, path.artist);
  append_optional_string(payload, path.album);
  append_optional_string(payload, path.composer);
  model_.update([path, title = pending_library_title_](Snapshot &state) {
    state.library_loading = true;
    state.library_items = std::make_shared<const LibraryItems>();
    state.library_total_count = 0;
    state.library_path = path;
    state.library_title = title;
    state.library_message.clear();
    state.status = "BROWSING LIBRARY";
    ++state.library_revision;
  });
  if (!send_packet(39, payload.data(), payload.size())) {
    library_request_deadline_us_ = 0;
    model_.update([](Snapshot &state) {
      state.library_loading = false;
      state.library_message = "LIBRARY BROWSE FAILED";
      state.status = "LIBRARY BROWSE FAILED";
      ++state.library_revision;
    });
  } else {
    library_request_deadline_us_ = esp_timer_get_time() + 15'000'000;
  }
}

void MonkeyMoteClient::select_library_item(const Command &command, bool play) {
  const Snapshot current = model_.copy();
  const int32_t destination = current.active_playlist_id;
  if (pending_library_play_row_ >= 0) {
    model_.update([](Snapshot &state) { state.status = "WAIT FOR LIBRARY ADD"; });
    return;
  }
  if (play && (!current.active_playlist_count_known ||
               current.active_playlist_track_count > static_cast<uint32_t>(INT32_MAX))) {
    model_.update([](Snapshot &state) { state.status = "REFRESH PLAYLIST FIRST"; });
    return;
  }
  const int32_t appended_row =
      static_cast<int32_t>(current.active_playlist_track_count);
  const auto invalidate_streamed_playlist = [this]() {
    playlist_stream_generation_.fetch_add(1);
    if (index_building_) {
      index_superseded_ = true;
    }
  };
  const auto confirm_before_play = [this, destination, appended_row]() {
    pending_library_play_row_ = appended_row;
    pending_library_play_destination_ = destination;
    pending_library_play_deadline_us_ = esp_timer_get_time() + 15'000'000;
    if (!send_playlist_request(destination, true)) {
      fail_pending_library_play("LIBRARY PLAY CONFIRM FAILED");
      close_socket();
      return false;
    }
    return true;
  };
  if (destination < 0) {
    model_.update([](Snapshot &state) { state.status = "NO ACTIVE PLAYLIST"; });
    return;
  }
  if (command.library_path.empty() && command.second > 0 && command.first >= 0) {
    std::vector<uint8_t> payload;
    append_i32(payload, command.second);
    append_i32(payload, destination);
    append_i32(payload, command.first);
    append_i32(payload, -2);
    append_u16(payload, 0);
    const bool sent = send_packet(44, payload.data(), payload.size());
    if (sent) {
      invalidate_streamed_playlist();
    }
    const bool confirmation_started = !play || !sent || confirm_before_play();
    if (sent && !play) {
      send_playlist_request(destination);
    }
    model_.update([play, sent, confirmation_started](Snapshot &state) {
      if (sent) {
        state.active_playlist_count_known = false;
      }
      state.library_play_pending = sent && play && confirmation_started;
      state.status = !sent                       ? "LIBRARY SELECTION FAILED"
                     : !confirmation_started     ? "LIBRARY PLAY CONFIRM FAILED"
                     : play                      ? "ADDING SEARCH RESULT"
                                                 : "ENQUEUED SEARCH RESULT";
    });
    return;
  }
  const uint32_t path_flags = library_path_flags(command.library_path);
  if (path_flags == 0) {
    return;
  }
  std::vector<uint8_t> payload;
  append_i32(payload, destination);
  append_u32(payload, path_flags);
  append_i32(payload, command.first);
  append_optional_string(payload, command.library_path.genre);
  append_optional_string(payload, command.library_path.artist);
  append_optional_string(payload, command.library_path.album);
  append_optional_string(payload, command.library_path.composer);
  const bool sent = send_packet(40, payload.data(), payload.size());
  if (sent) {
    invalidate_streamed_playlist();
  }
  const bool confirmation_started = !play || !sent || confirm_before_play();
  if (sent && !play) {
    send_playlist_request(destination);
  }
  model_.update([play, sent, confirmation_started](Snapshot &state) {
    if (sent) {
      state.active_playlist_count_known = false;
    }
    state.library_play_pending = sent && play && confirmation_started;
    state.status = !sent                   ? "LIBRARY SELECTION FAILED"
                   : !confirmation_started ? "LIBRARY PLAY CONFIRM FAILED"
                   : play                  ? "ADDING LIBRARY ITEM"
                                           : "ENQUEUED LIBRARY ITEM";
  });
}

void MonkeyMoteClient::complete_pending_library_play() {
  if (pending_library_play_row_ < 0) {
    return;
  }
  const int32_t row = pending_library_play_row_;
  pending_library_play_row_ = -1;
  pending_library_play_destination_ = -1;
  pending_library_play_deadline_us_ = 0;
  pending_library_play_token_ = 0;
  std::vector<uint8_t> body{10};
  append_i32(body, row);
  const bool selected = send_packet(7, body.data(), body.size());
  body.assign(1, 1);
  append_i32(body, 0);
  const bool started = selected && send_packet(7, body.data(), body.size());
  model_.update([started](Snapshot &state) {
    state.library_play_pending = false;
    state.status = started ? "PLAYING LIBRARY ITEM" : "LIBRARY PLAY FAILED";
  });
}

void MonkeyMoteClient::fail_pending_library_play(const char *message) {
  if (pending_library_play_row_ < 0) {
    return;
  }
  pending_library_play_row_ = -1;
  pending_library_play_destination_ = -1;
  pending_library_play_deadline_us_ = 0;
  pending_library_play_token_ = 0;
  model_.update([message = std::string(message)](Snapshot &state) {
    state.active_playlist_count_known = false;
    state.library_play_pending = false;
    state.status = message;
  });
}

std::optional<MonkeyMoteClient::PlaylistRequestToken>
MonkeyMoteClient::take_playlist_response_token() {
  std::lock_guard lock(playlist_requests_mutex_);
  if (playlist_requests_.empty()) {
    return std::nullopt;
  }
  PlaylistRequestToken token = playlist_requests_.front();
  playlist_requests_.pop_front();
  if (!playlist_requests_.empty()) {
    playlist_requests_.front().deadline_us = esp_timer_get_time() + 15'000'000;
  }
  return token;
}

void MonkeyMoteClient::finish_playlist_response(
    const std::optional<PlaylistRequestToken> &token, bool valid,
    int32_t resolved_playlist_id, uint32_t count) {
  if (!token || !token->confirms_library_play || pending_library_play_row_ < 0 ||
      pending_library_play_token_ != token->id) {
    return;
  }
  const bool appended_row_confirmed =
      valid && resolved_playlist_id == pending_library_play_destination_ &&
      count > static_cast<uint32_t>(pending_library_play_row_);
  if (!appended_row_confirmed) {
    const int32_t destination = pending_library_play_destination_;
    fail_pending_library_play("LIBRARY APPEND NOT CONFIRMED");
    // The failed response token has already been consumed, so a fresh
    // authoritative request cannot be mis-correlated with it.
    if (destination >= 0 && socket_ >= 0) {
      enqueue({CommandType::request_playlist, destination});
    }
    return;
  }
  complete_pending_library_play();
}

void MonkeyMoteClient::consume_playlist_response(bool valid,
                                                 int32_t resolved_playlist_id,
                                                 uint32_t count) {
  playlist_stream_generation_.fetch_add(1);
  const auto token = take_playlist_response_token();
  finish_playlist_response(token, valid, resolved_playlist_id, count);
}

void MonkeyMoteClient::request_initial_state() {
  enqueue({CommandType::request_playlists, -1});
  enqueue({CommandType::request_playlist, -1});
}

bool MonkeyMoteClient::parse_library(const std::vector<uint8_t> &payload) {
  constexpr uint32_t kMaxRecords = 100'000;
  constexpr size_t kMaxRetainedRecords = 1'000;
  constexpr size_t kMaxRetainedText = 128 * 1024;
  if (payload.size() < 9) {
    return false;
  }
  const uint32_t request_id = read_u32(payload.data());
  if (request_id != library_request_id_) {
    ESP_LOGI(kTag, "Ignoring stale library response id=%" PRIu32, request_id);
    return true;
  }
  const uint32_t count = read_u32(payload.data() + 4);
  const uint8_t wire_kind = payload[8];
  if (count > kMaxRecords || (wire_kind != 0 && wire_kind != 2 && wire_kind != 4 &&
                             wire_kind != 6 && wire_kind != 7 && wire_kind != 8)) {
    return false;
  }

  size_t offset = 9;
  size_t retained_text = 0;
  bool truncated = false;
  LibraryItems items;
  // Bound the initial allocation by both the retention cap and the minimum
  // number of bytes required to encode each declared record. The count is
  // untrusted and must not request a large contiguous allocation by itself.
  const size_t min_record_size = wire_kind == 6                ? 14
                                 : wire_kind == 7 || wire_kind == 8 ? 23
                                                                    : 1;
  items.reserve(std::min<size_t>(
      {count, kMaxRetainedRecords, (payload.size() - 9) / min_record_size}));
  auto retain = [&](LibraryItem item) {
    const size_t text_size = item.title.size() + item.subtitle.size() +
                             item.path.genre.size() + item.path.artist.size() +
                             item.path.album.size() + item.path.composer.size();
    if (truncated || items.size() >= kMaxRetainedRecords ||
        text_size > kMaxRetainedText - retained_text) {
      truncated = true;
      return;
    }
    retained_text += text_size;
    items.push_back(std::move(item));
  };

  for (uint32_t index = 0; index < count; ++index) {
    LibraryItem item;
    item.selection_index = wire_kind == 7 || wire_kind == 8
                               ? static_cast<int32_t>(index)
                               : -1;
    item.selection_request_id = pending_library_search_ ? request_id : 0;
    item.path = pending_library_path_;
    if (wire_kind == 0 || wire_kind == 2 || wire_kind == 4) {
      if (!read_bounded_cstring(payload, offset, item.title)) {
        return false;
      }
      if (wire_kind == 0) {
        item.path.artist = item.title;
      } else if (wire_kind == 2) {
        item.path.genre = item.title;
      } else {
        item.path.composer = item.title;
      }
      retain(std::move(item));
      continue;
    }

    if (wire_kind == 6) {
      if (offset > payload.size() || payload.size() - offset < 12) {
        return false;
      }
      const uint16_t album_length = read_u16(payload.data() + offset);
      const uint16_t artist_length = read_u16(payload.data() + offset + 2);
      item.year = read_u16(payload.data() + offset + 4);
      const uint32_t record_size = read_u32(payload.data() + offset + 8);
      offset += 12;
      if (record_size > payload.size() - offset) {
        return false;
      }
      const size_t record_end = offset + record_size;
      if (!read_string_field(payload, offset, record_end, album_length, item.title) ||
          !read_string_field(payload, offset, record_end, artist_length, item.subtitle)) {
        return false;
      }
      offset = record_end;
      item.kind = LibraryItemKind::album;
      item.path.album = item.title;
      if (!item.subtitle.empty()) {
        item.path.artist = item.subtitle;
      }
      retain(std::move(item));
      continue;
    }

    if (offset > payload.size() || payload.size() - offset < 20) {
      return false;
    }
    const uint16_t artist_length = read_u16(payload.data() + offset);
    const uint16_t title_length = read_u16(payload.data() + offset + 2);
    const uint16_t album_length = read_u16(payload.data() + offset + 4);
    item.year = read_u16(payload.data() + offset + 6);
    const uint32_t record_size = read_u32(payload.data() + offset + 8);
    item.duration_ms = read_i32(payload.data() + offset + 12);
    offset += 20;
    if (record_size > payload.size() - offset) {
      return false;
    }
    const size_t record_end = offset + record_size;
    std::string artist;
    std::string album;
    if (!read_string_field(payload, offset, record_end, artist_length, artist) ||
        !read_string_field(payload, offset, record_end, title_length, item.title) ||
        !read_string_field(payload, offset, record_end, album_length, album)) {
      return false;
    }
    offset = record_end;
    item.kind = LibraryItemKind::track;
    item.subtitle = artist.empty() ? album : artist;
    // Browsed rows are selected by their parent path plus result index (type
    // 40). Search rows retain no path and use their request ID plus result
    // index (type 44). The display metadata must not overwrite either key.
    retain(std::move(item));
  }
  if (offset != payload.size()) {
    return false;
  }

  auto published_items = std::make_shared<const LibraryItems>(std::move(items));
  library_request_deadline_us_ = 0;
  model_.update([items = std::move(published_items), path = pending_library_path_,
                 title = pending_library_title_, count, truncated](Snapshot &state) mutable {
    state.library_items = std::move(items);
    state.library_path = std::move(path);
    state.library_title = std::move(title);
    state.library_total_count = count;
    state.library_loading = false;
    state.library_truncated = truncated || state.library_items->size() != count;
    state.library_message.clear();
    state.status = count == 0 ? "NO LIBRARY MATCHES" : "LIBRARY READY";
    ++state.library_revision;
  });
  return true;
}

bool MonkeyMoteClient::parse_lyrics(const std::vector<uint8_t> &payload) {
  constexpr uint32_t kMaxLyricsBytes = 24 * 1024;
  const std::string request_track_key = take_lyrics_request();
  if (request_track_key.empty()) {
    ESP_LOGW(kTag, "Ignoring lyrics reply without a matching request");
    return true;
  }
  if (payload.size() < 20) {
    if (request_track_key == cover_track_key_) {
      model_.update([key = request_track_key](Snapshot &state) {
        state.lyrics_state = LyricsState::failed;
        state.lyrics = std::make_shared<const std::string>("Lyrics response was invalid.");
        state.lyrics_track_key = key;
        ++state.lyrics_revision;
      });
    }
    return false;
  }
  const uint32_t length = read_u32(payload.data());
  if (length != payload.size() - 20) {
    if (request_track_key == cover_track_key_) {
      model_.update([key = request_track_key](Snapshot &state) {
        state.lyrics_state = LyricsState::failed;
        state.lyrics = std::make_shared<const std::string>("Lyrics response was invalid.");
        state.lyrics_track_key = key;
        ++state.lyrics_revision;
      });
    }
    return false;
  }
  if (request_track_key != cover_track_key_) {
    return true;
  }
  if (length > kMaxLyricsBytes) {
    model_.update([key = request_track_key](Snapshot &state) {
      state.lyrics_state = LyricsState::failed;
      state.lyrics = std::make_shared<const std::string>(
          "Lyrics exceed the 24 KiB display limit.");
      state.lyrics_track_key = key;
      ++state.lyrics_revision;
    });
    return true;
  }
  std::string lyrics(reinterpret_cast<const char *>(payload.data() + 20), length);
  std::replace(lyrics.begin(), lyrics.end(), '\0', ' ');
  const bool empty = lyrics.empty();
  auto published_lyrics = std::make_shared<const std::string>(
      empty ? "No lyrics are available for this track." : std::move(lyrics));
  model_.update([key = request_track_key, empty,
                 lyrics = std::move(published_lyrics)](
                    Snapshot &state) mutable {
    state.lyrics_state = empty ? LyricsState::unavailable : LyricsState::ready;
    state.lyrics = std::move(lyrics);
    state.lyrics_track_key = key;
    ++state.lyrics_revision;
  });
  return true;
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
  const std::string track_key = artwork_track_key(track);
  const bool is_now_playing = (flags & 2) != 0 || track.index >= 0;
  model_.update(
      [track = std::move(track), is_now_playing, track_key](Snapshot &state) mutable {
        if (is_now_playing) {
          state.now_playing = std::move(track);
          if (state.lyrics_track_key != track_key) {
            state.lyrics_state = LyricsState::none;
            state.lyrics = std::make_shared<const std::string>();
            state.lyrics_track_key = track_key;
            ++state.lyrics_revision;
          }
        }
      });
  if (is_now_playing && track_key != cover_track_key_) {
    cover_track_key_ = track_key;
    artwork_decoder_.select_track(track_key);
    if (!track_key.empty()) {
      std::vector<uint8_t> request{3};
      append_i32(request, 144);
      append_i32(request, 144);
      if (cover_requests_.size() >= 32 ||
          !send_packet(4, request.data(), request.size())) {
        artwork_decoder_.mark_unavailable(track_key);
      } else {
        cover_requests_.push_back(track_key);
      }
    }
  }
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

bool MonkeyMoteClient::parse_playlist(const std::vector<uint8_t> &payload) {
  if (payload.size() < 11) {
    return false;
  }
  const uint32_t count = read_u32(payload.data());
  const int32_t playlist_id = read_i32(payload.data() + 4);
  const uint16_t name_length = read_u16(payload.data() + 8);
  const uint8_t flags = payload[10];
  size_t offset = 11;
  if (offset + name_length + 1 > payload.size()) {
    return false;
  }
  std::string name(reinterpret_cast<const char *>(payload.data() + offset), name_length);
  offset += name_length + 1;
  int32_t resolved_response_id = model_.read([&](const Snapshot &state) {
    return resolve_playlist_id(state, playlist_id, name, count);
  });
  // AIMP can encode an explicitly requested active playlist as -1. Only use
  // the destination from the exact FIFO request token paired with this reply.
  int32_t requested_playlist_id = -1;
  {
    std::lock_guard lock(playlist_requests_mutex_);
    if (!playlist_requests_.empty()) {
      requested_playlist_id = playlist_requests_.front().playlist_id;
    }
  }
  if (resolved_response_id < 0 && requested_playlist_id >= 0) {
    resolved_response_id = requested_playlist_id;
  }
  if (count > 10000) {
    // Oversized queues are not retained, but still validate the complete
    // record structure before allowing an untrusted frame to alter the model.
    size_t validation_offset = offset;
    for (uint32_t i = 0; i < count; ++i) {
      if (validation_offset > payload.size() || payload.size() - validation_offset < 32) {
        return false;
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
          return false;
        }
        validation_offset += field_size;
      }
    }
    queue_filter_active_ = false;
    queue_filter_.clear();
    model_.update([resolved_playlist_id = resolved_response_id, count,
                   name = std::move(name)](Snapshot &state) mutable {
      state.active_playlist_id = resolved_playlist_id;
      state.active_playlist_name = name.empty() ? "PLAYLIST TOO LARGE" : std::move(name);
      state.queue.clear();
      state.queue_total_count = 0;
      state.active_playlist_track_count = count;
      state.active_playlist_count_known = true;
      state.queue_page_start = 0;
      state.queue_index_ready = false;
      state.queue_indexing = false;
      state.queue_filter_active = false;
      state.queue_filter_label.clear();
      ++state.queue_revision;
      bool playlist_state_changed = false;
      for (auto &playlist : state.playlists) {
        const bool active = playlist.id == resolved_playlist_id;
        playlist_state_changed = playlist_state_changed || playlist.active != active;
        playlist.active = active;
      }
      if (playlist_state_changed) {
        ++state.playlists_revision;
      }
      state.status = "PLAYLIST TOO LARGE";
    });
    consume_playlist_response(true, resolved_response_id, count);
    return true;
  }
  queue_filter_active_ = false;
  queue_filter_.clear();
  std::vector<Track> tracks;
  tracks.reserve(std::min<uint32_t>(count, 1000));
  for (uint32_t i = 0; i < count; ++i) {
    if (offset + 32 > payload.size()) {
      return false;
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
        return false;
      }
      fields[field]->assign(reinterpret_cast<const char *>(payload.data() + offset), lengths[field]);
      offset += lengths[field] + 1;
    }
    if (tracks.size() < 1000) {
      tracks.push_back(std::move(track));
    }
  }
  model_.update([resolved_playlist_id = resolved_response_id, name = std::move(name), flags, count,
                 tracks = std::move(tracks)](Snapshot &state) mutable {
    const bool replaced_index_build = state.queue_indexing;
    state.active_playlist_id = resolved_playlist_id;
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
      ++state.queue_revision;
    }
    state.queue_total_count = count;
    state.active_playlist_track_count = count;
    state.active_playlist_count_known = true;
    state.queue_page_start = 0;
    state.queue_index_ready = false;
    state.queue_indexing = false;
    state.queue_filter_active = false;
    state.queue_filter_label.clear();
    if (replaced_index_build) {
      state.status = "AIMP ONLINE";
    }
    bool playlist_state_changed = false;
    for (auto &playlist : state.playlists) {
      const bool active = playlist.id == resolved_playlist_id;
      playlist_state_changed = playlist_state_changed || playlist.active != active;
      playlist.active = active;
    }
    if (playlist_state_changed) {
      ++state.playlists_revision;
    }
    (void)flags;
  });
  consume_playlist_response(true, resolved_response_id, count);
  return true;
}

void MonkeyMoteClient::parse_compressed(std::vector<uint8_t> &payload) {
  auto reset_unclassifiable_playlist_correlation = [this]() {
    bool pending = false;
    {
      std::lock_guard lock(playlist_requests_mutex_);
      pending = !playlist_requests_.empty();
    }
    if (pending) {
      model_.update([](Snapshot &state) {
        state.active_playlist_count_known = false;
        state.status = "PLAYLIST RESPONSE INVALID";
      });
      close_socket();
    }
  };
  if (payload.size() < 10) {
    if (payload.size() >= 8) {
      const uint32_t malformed_inner_type = read_u32(payload.data() + 4);
      if (malformed_inner_type == 36) {
        reject_lyrics_reply("Lyrics response was invalid.");
      } else if (malformed_inner_type == 8 || malformed_inner_type == 13) {
        reject_cover_reply();
      } else if (malformed_inner_type == 19) {
        library_request_deadline_us_ = 0;
        model_.update([](Snapshot &state) {
          state.library_loading = false;
          state.library_message = "LIBRARY RESPONSE INVALID";
          state.status = "LIBRARY RESPONSE INVALID";
          ++state.library_revision;
        });
      } else if (malformed_inner_type == 9 || malformed_inner_type == 31 ||
                 malformed_inner_type == 50) {
        consume_playlist_response(false);
      } else {
        reset_unclassifiable_playlist_correlation();
      }
    } else {
      reset_unclassifiable_playlist_correlation();
    }
    return;
  }
  const uint32_t uncompressed_size = read_u32(payload.data());
  const uint32_t inner_type = read_u32(payload.data() + 4);
  if (inner_type == 45) {
    ESP_LOGW(kTag, "Nested compressed packet rejected");
    reset_unclassifiable_playlist_correlation();
    return;
  }
  if (uncompressed_size > kMaxPayload ||
      (inner_type == 36 && uncompressed_size > 24 * 1024 + 20)) {
    if (inner_type == 8 || inner_type == 13) {
      reject_cover_reply();
      return;
    }
    if (inner_type == 19) {
      library_request_deadline_us_ = 0;
      model_.update([](Snapshot &state) {
        state.library_loading = false;
        state.library_items = std::make_shared<const LibraryItems>();
        state.library_total_count = 0;
        state.library_truncated = true;
        state.library_message = "LIBRARY RESULT TOO LARGE";
        state.status = "LIBRARY RESULT TOO LARGE";
        ++state.library_revision;
      });
      return;
    }
    if (inner_type == 36) {
      reject_lyrics_reply("Lyrics exceed the 24 KiB display limit.");
      return;
    }
    queue_filter_active_ = false;
    queue_filter_.clear();
    if (inner_type == 9 || inner_type == 31 || inner_type == 50) {
      consume_playlist_response(false);
    }
    model_.update([](Snapshot &state) {
      state.queue.clear();
      state.queue_total_count = 0;
      state.active_playlist_count_known = false;
      state.queue_page_start = 0;
      state.queue_index_ready = false;
      state.queue_indexing = false;
      state.queue_filter_active = false;
      state.queue_filter_label.clear();
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
    if (inner_type == 8 || inner_type == 13) {
      reject_cover_reply();
    } else if (inner_type == 19) {
      library_request_deadline_us_ = 0;
      model_.update([](Snapshot &state) {
        state.library_loading = false;
        state.library_message = "LIBRARY RESPONSE INVALID";
        state.status = "LIBRARY RESPONSE INVALID";
        ++state.library_revision;
      });
    } else if (inner_type == 36) {
      reject_lyrics_reply("Lyrics response was invalid.");
    } else if (inner_type == 9 || inner_type == 31 || inner_type == 50) {
      consume_playlist_response(false);
    }
    return;
  }
  std::vector<uint8_t>().swap(payload);
  process_frame(inner_type, uncompressed);
}

std::string MonkeyMoteClient::take_cover_request() {
  if (cover_requests_.empty()) {
    return {};
  }
  std::string track_key = std::move(cover_requests_.front());
  cover_requests_.pop_front();
  return track_key;
}

void MonkeyMoteClient::reject_cover_reply() {
  std::string track_key = take_cover_request();
  if (!track_key.empty() && track_key == cover_track_key_) {
    artwork_decoder_.mark_unavailable(track_key);
  }
}

std::string MonkeyMoteClient::take_lyrics_request() {
  if (lyrics_requests_.empty()) {
    return {};
  }
  std::string track_key = std::move(lyrics_requests_.front().track_key);
  lyrics_requests_.pop_front();
  return track_key;
}

void MonkeyMoteClient::expire_requests(int64_t now_us) {
  bool playlist_response_timed_out = false;
  {
    std::lock_guard lock(playlist_requests_mutex_);
    playlist_response_timed_out =
        !playlist_requests_.empty() &&
        now_us >= playlist_requests_.front().deadline_us;
  }
  if (playlist_response_timed_out) {
    model_.update([](Snapshot &state) {
      state.active_playlist_count_known = false;
      state.status = "PLAYLIST RESPONSE TIMED OUT";
    });
    // Never drop only the head token: without response IDs that would shift
    // every subsequent response. Reconnect to reset the complete correlation.
    close_socket();
    return;
  }
  if (library_request_deadline_us_ != 0 && now_us >= library_request_deadline_us_) {
    library_request_deadline_us_ = 0;
    model_.update([](Snapshot &state) {
      if (!state.library_loading) {
        return;
      }
      state.library_loading = false;
      state.library_message = "LIBRARY REQUEST TIMED OUT";
      state.status = "LIBRARY REQUEST TIMED OUT";
      ++state.library_revision;
    });
  }
  if (!lyrics_requests_.empty() &&
      now_us >= lyrics_requests_.front().deadline_us) {
    // Type 36 has no request identifier. Clear the FIFO as a unit so one
    // missing response cannot permanently shift every later correlation.
    lyrics_requests_.clear();
    model_.update([](Snapshot &state) {
      if (state.lyrics_state == LyricsState::loading) {
        state.lyrics_state = LyricsState::failed;
        state.lyrics =
            std::make_shared<const std::string>("Lyrics request timed out.");
        ++state.lyrics_revision;
      }
    });
  }
}

void MonkeyMoteClient::reject_lyrics_reply(const char *message) {
  const std::string track_key = take_lyrics_request();
  if (track_key.empty() || track_key != cover_track_key_) {
    return;
  }
  model_.update([track_key, message = std::string(message)](Snapshot &state) {
    state.lyrics_state = LyricsState::failed;
    state.lyrics = std::make_shared<const std::string>(message);
    state.lyrics_track_key = track_key;
    ++state.lyrics_revision;
  });
}

} // namespace musicremote
