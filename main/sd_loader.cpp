#include "sd_loader.hpp"

#include "sd_loader_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <new>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

namespace musicremote {
namespace {

using sd_loader::ChunkVerdict;
using sd_loader::Command;
using sd_loader::CommandType;
using sd_loader::FileReply;
using sd_loader::kMaxChunkBytes;
using sd_loader::kMaxFileBytes;
using sd_loader::Relpath;
using sd_loader::ReplyBuffer;
using sd_loader::Sha256;
using sd_loader::TransferProgress;

constexpr char kTag[] = "sd_loader";
constexpr char kMountPoint[] = "/sdcard";
constexpr uint32_t kTxRingBytes = 1024;
constexpr uint32_t kTaskStackBytes = 6 * 1024;
// Below the AIMP client (5), GUI (4), and cover decoder (3).
constexpr UBaseType_t kTaskPriority = 2;
constexpr size_t kReadSliceBytes = 512;
constexpr TickType_t kReadWait = pdMS_TO_TICKS(100);
constexpr TickType_t kReplyWait = pdMS_TO_TICKS(50);
// Free space required beyond the file itself, for FAT cluster rounding and
// directory growth.
constexpr uint64_t kFreeSpaceMarginBytes = 256 * 1024;
// Bounds the boot-time stale-file scan of one allow-listed directory.
constexpr size_t kMaxScannedEntries = 64;

std::string absolute(const std::string &relpath) {
  return std::string(kMountPoint) + "/" + relpath;
}

class Loader {
 public:
  explicit Loader(bool sd_mounted) : sd_mounted_(sd_mounted) {}

  [[noreturn]] void run() {
    if (sd_mounted_) {
      remove_stale_files();
    }
    uint8_t slice[kReadSliceBytes];
    for (;;) {
      const int received = usb_serial_jtag_read_bytes(slice, sizeof(slice), kReadWait);
      if (received > 0) {
        handle_bytes(slice, static_cast<size_t>(received));
        // Measured after handling, so a slow card write does not count as
        // host inactivity.
        last_rx_us_ = esp_timer_get_time();
      }
      check_timeouts(esp_timer_get_time());
    }
  }

 private:
  enum class Mode { line, payload };

  // ------------------------------------------------------------ framing

  void handle_bytes(const uint8_t *data, size_t length) {
    size_t index = 0;
    while (index < length) {
      if (mode_ == Mode::payload) {
        const size_t take =
            std::min<size_t>(length - index, frame_.length - payload_filled_);
        if (active_) {
          std::memcpy(buffer_ + payload_filled_, data + index, take);
        }
        payload_filled_ += take;
        index += take;
        if (payload_filled_ == frame_.length) {
          mode_ = Mode::line;
          handle_frame();
        }
        continue;
      }
      if (lines_.push(data[index++])) {
        handle_line(lines_.line());
      }
    }
  }

  void check_timeouts(int64_t now) {
    if (mode_ == Mode::payload &&
        now - payload_started_us_ > int64_t{sd_loader::kPayloadTimeoutMs} * 1000) {
      // Bytes were lost or the host stopped mid-frame. Drop the partial frame
      // and let the host resend it; its header will start a fresh line.
      mode_ = Mode::line;
      lines_.reset();
      reply_nak(frame_.sequence, "timeout");
    }
    if (active_ && now - last_rx_us_ > int64_t{sd_loader::kIdleTimeoutMs} * 1000) {
      fail("idle-timeout");
    }
  }

  void handle_line(std::string_view line) {
    const Command command = sd_loader::parse_command(line);
    switch (command.type) {
      case CommandType::none:
        return;
      case CommandType::invalid:
        reply_nak("-", "syntax");
        return;
      case CommandType::query:
        handle_query(command);
        return;
      case CommandType::begin:
        handle_begin(command);
        return;
      case CommandType::data:
        frame_ = command;
        payload_filled_ = 0;
        payload_started_us_ = esp_timer_get_time();
        mode_ = Mode::payload;
        return;
      case CommandType::end:
        handle_end();
        return;
      case CommandType::abort:
        if (active_) {
          fail("aborted");
        }
        failure_ = nullptr;
        reply_nak("ABORT", "aborted");
        return;
    }
  }

  // ------------------------------------------------------------ commands

  void handle_query(const Command &command) {
    if (active_) {
      reply_nak("QUERY", "busy");
      return;
    }
    if (!sd_mounted_) {
      reply_nak("QUERY", "nosd");
      return;
    }
    const auto path = sd_loader::validate_relpath(command.relpath);
    if (!path) {
      reply_nak("QUERY", "path");
      return;
    }
    const std::string target = absolute(path->target());
    struct stat status {};
    if (stat(target.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
      reply_nak("QUERY", "notfound");
      return;
    }
    if (status.st_size < 0 || static_cast<uint64_t>(status.st_size) > kMaxFileBytes) {
      reply_nak("QUERY", "size");
      return;
    }
    auto *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(kMaxChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
      reply_nak("QUERY", "nomem");
      return;
    }
    const int file = open(target.c_str(), O_RDONLY);
    if (file < 0) {
      heap_caps_free(buffer);
      reply_nak("QUERY", "open");
      return;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
    uint64_t total = 0;
    while (ok) {
      const ssize_t count = read(file, buffer, kMaxChunkBytes);
      if (count < 0) {
        ok = false;
        break;
      }
      if (count == 0) {
        break;
      }
      total += static_cast<uint64_t>(count);
      ok = total <= kMaxFileBytes &&
           mbedtls_sha256_update(&sha, buffer, static_cast<size_t>(count)) == 0;
    }
    Sha256 digest{};
    ok = ok && mbedtls_sha256_finish(&sha, digest.data()) == 0;
    mbedtls_sha256_free(&sha);
    close(file);
    heap_caps_free(buffer);
    if (!ok || total != static_cast<uint64_t>(status.st_size)) {
      reply_nak("QUERY", "read");
      return;
    }
    ReplyBuffer reply;
    send(reply, sd_loader::reply_file(reply, FileReply::info, *path,
                                      static_cast<uint32_t>(total), digest));
  }

  void handle_begin(const Command &command) {
    if (active_) {
      fail("superseded");
    }
    failure_ = nullptr;
    if (!sd_mounted_) {
      reply_nak("BEGIN", "nosd");
      return;
    }
    const auto path = sd_loader::validate_relpath(command.relpath);
    if (!path) {
      reply_nak("BEGIN", "path");
      return;
    }
    if (command.size > kMaxFileBytes) {
      reply_nak("BEGIN", "size");
      return;
    }
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(kMountPoint, &total_bytes, &free_bytes) != ESP_OK ||
        free_bytes < uint64_t{command.size} + kFreeSpaceMarginBytes) {
      reply_nak("BEGIN", "space");
      return;
    }
    const std::string directory = absolute(path->directory);
    if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) {
      reply_nak("BEGIN", "mkdir");
      return;
    }
    buffer_ = static_cast<uint8_t *>(
        heap_caps_malloc(kMaxChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer_ == nullptr) {
      reply_nak("BEGIN", "nomem");
      return;
    }
    // The only descriptor this loader holds open, for the transfer's
    // duration. The mount's slots are shared with the playlist index and
    // the cover-art cache.
    file_ = open(absolute(path->staging()).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file_ < 0) {
      heap_caps_free(buffer_);
      buffer_ = nullptr;
      reply_nak("BEGIN", "open");
      return;
    }
    mbedtls_sha256_init(&sha_);
    if (mbedtls_sha256_starts(&sha_, 0) != 0) {
      mbedtls_sha256_free(&sha_);
      close(file_);
      file_ = -1;
      unlink(absolute(path->staging()).c_str());
      heap_caps_free(buffer_);
      buffer_ = nullptr;
      reply_nak("BEGIN", "sha");
      return;
    }
    path_ = *path;
    expected_ = command.sha256;
    progress_ = TransferProgress(command.size);
    started_us_ = esp_timer_get_time();
    last_rx_us_ = started_us_;
    active_ = true;
    ESP_LOGI(kTag, "Receiving %s (%" PRIu32 " bytes)", path_->target().c_str(), command.size);
    ReplyBuffer reply;
    send(reply, sd_loader::reply_ack_begin(reply, *path_));
  }

  void handle_frame() {
    const uint32_t sequence = frame_.sequence;
    if (!active_) {
      reply_nak(sequence, failure_ != nullptr ? failure_ : "idle");
      return;
    }
    const uint32_t crc = sd_loader::crc32(buffer_, frame_.length);
    switch (progress_.check(sequence, frame_.length, frame_.crc32, crc)) {
      case ChunkVerdict::bad_crc:
        reply_nak(sequence, "crc");
        return;
      case ChunkVerdict::duplicate:
        reply_ack(sequence);
        return;
      case ChunkVerdict::out_of_order:
        fail("seq");
        reply_nak(sequence, "seq");
        return;
      case ChunkVerdict::overrun:
        fail("overrun");
        reply_nak(sequence, "overrun");
        return;
      case ChunkVerdict::accept:
        break;
    }
    // Acknowledge before writing so the host's next frame crosses USB while
    // the card writes this one. A failed write is reported on that frame.
    progress_.commit(frame_.length, crc);
    reply_ack(sequence);
    if (mbedtls_sha256_update(&sha_, buffer_, frame_.length) != 0) {
      fail("sha");
      return;
    }
    size_t written = 0;
    while (written < frame_.length) {
      const ssize_t count = write(file_, buffer_ + written, frame_.length - written);
      if (count <= 0) {
        fail("write");
        return;
      }
      written += static_cast<size_t>(count);
    }
  }

  void handle_end() {
    if (!active_) {
      reply_nak("END", failure_ != nullptr ? failure_ : "idle");
      return;
    }
    if (!progress_.complete()) {
      fail("short");
      reply_nak("END", "short");
      return;
    }
    Sha256 actual{};
    if (mbedtls_sha256_finish(&sha_, actual.data()) != 0 || actual != expected_) {
      fail("sha256");
      reply_nak("END", "sha256");
      return;
    }
    const bool synced = fsync(file_) == 0;
    const bool closed = close(file_) == 0;
    file_ = -1;
    if (!synced || !closed) {
      fail("write");
      reply_nak("END", "write");
      return;
    }
    if (!promote()) {
      fail("rename");
      reply_nak("END", "rename");
      return;
    }
    const Relpath path = *path_;
    const uint32_t size = progress_.declared_size();
    const int64_t elapsed_us = std::max<int64_t>(esp_timer_get_time() - started_us_, 1);
    release(false);
    ESP_LOGI(kTag, "Stored %s: %" PRIu32 " bytes in %" PRId64 " ms (%" PRId64
                   " KB/s), SHA-256 verified",
             path.target().c_str(), size, elapsed_us / 1000,
             static_cast<int64_t>(size) * 1000000 / elapsed_us / 1024);
    ReplyBuffer reply;
    send(reply, sd_loader::reply_file(reply, FileReply::ok, path, size, actual));
  }

  // Replaces the target with the verified staging file. The previous target
  // becomes <stem>.bak rather than being deleted: a reader that opened it
  // before the upload (the run-time CJK font holds its file open) keeps
  // valid clusters until reboot, when remove_stale_files() deletes it.
  bool promote() {
    const std::string target = absolute(path_->target());
    const std::string staging = absolute(path_->staging());
    const std::string backup = absolute(path_->backup());
    struct stat status {};
    const bool had_target = stat(target.c_str(), &status) == 0;
    if (had_target) {
      unlink(backup.c_str());
      if (rename(target.c_str(), backup.c_str()) != 0) {
        return false;
      }
    }
    if (rename(staging.c_str(), target.c_str()) != 0) {
      if (had_target) {
        rename(backup.c_str(), target.c_str());
      }
      return false;
    }
    return true;
  }

  // ------------------------------------------------------------ state

  // Discards the transfer: the staging file is deleted and the existing
  // target is untouched. Later frames and END are refused with reason until
  // the next BEGIN or ABORT.
  void fail(const char *reason) {
    ESP_LOGW(kTag, "Discarded upload of %s: %s", path_ ? path_->target().c_str() : "?", reason);
    release(true);
    failure_ = reason;
  }

  void release(bool remove_staging) {
    if (file_ >= 0) {
      close(file_);
      file_ = -1;
    }
    if (remove_staging && path_) {
      unlink(absolute(path_->staging()).c_str());
    }
    if (active_) {
      mbedtls_sha256_free(&sha_);
    }
    heap_caps_free(buffer_);
    buffer_ = nullptr;
    path_.reset();
    active_ = false;
  }

  // Deletes leftovers from an interrupted upload (*.tmp) and backups whose
  // replacement was promoted (*.bak). Runs once at boot, before any upload.
  void remove_stale_files() {
    for (const auto directory_name : sd_loader::kAllowedDirectories) {
      const std::string directory = absolute(std::string(directory_name));
      DIR *listing = opendir(directory.c_str());
      if (listing == nullptr) {
        continue;
      }
      std::vector<std::string> names;
      while (names.size() < kMaxScannedEntries) {
        const dirent *entry = readdir(listing);
        if (entry == nullptr) {
          break;
        }
        if (entry->d_type == DT_REG) {
          names.emplace_back(entry->d_name);
        }
      }
      closedir(listing);
      for (const auto &name : sd_loader::stale_entries(names)) {
        const std::string path = directory + "/" + name;
        if (unlink(path.c_str()) == 0) {
          ESP_LOGI(kTag, "Removed stale %s", path.c_str());
        }
      }
    }
  }

  // ------------------------------------------------------------ replies

  // One write per reply: the driver's TX ring takes it atomically, so a
  // reply never interleaves byte-wise with log output.
  void send(const ReplyBuffer &reply, size_t length) {
    if (length > 0) {
      usb_serial_jtag_write_bytes(reply.data(), length, kReplyWait);
    }
  }

  void reply_ack(uint32_t sequence) {
    ReplyBuffer reply;
    send(reply, sd_loader::reply_ack(reply, sequence));
  }

  void reply_nak(uint32_t sequence, const char *reason) {
    ReplyBuffer reply;
    send(reply, sd_loader::reply_nak(reply, sequence, reason));
  }

  void reply_nak(const char *subject, const char *reason) {
    ReplyBuffer reply;
    send(reply, sd_loader::reply_nak(reply, subject, reason));
  }

  const bool sd_mounted_;
  sd_loader::LineAssembler lines_;
  Mode mode_{Mode::line};
  Command frame_{};
  uint32_t payload_filled_{0};
  int64_t payload_started_us_{0};
  int64_t last_rx_us_{0};

  // Transfer state; valid while active_.
  bool active_{false};
  const char *failure_{nullptr};
  std::optional<Relpath> path_;
  int file_{-1};
  uint8_t *buffer_{nullptr};
  mbedtls_sha256_context sha_{};
  TransferProgress progress_;
  Sha256 expected_{};
  int64_t started_us_{0};
};

void task_entry(void *argument) { static_cast<Loader *>(argument)->run(); }

}  // namespace

bool start_sd_loader(bool sd_mounted) {
  usb_serial_jtag_driver_config_t config{.tx_buffer_size = kTxRingBytes,
                                         .rx_buffer_size = sd_loader::kRxRingBytes};
  const esp_err_t installed = usb_serial_jtag_driver_install(&config);
  if (installed != ESP_OK) {
    ESP_LOGW(kTag, "USB-Serial-JTAG driver unavailable: %s", esp_err_to_name(installed));
    return false;
  }
  // Console output now goes through the driver's TX ring, which the driver's
  // ISR alone drains, so log lines and loader replies cannot corrupt each
  // other in the FIFO. With no host attached the driver drops output after
  // a short wait, as the non-driver path does.
  usb_serial_jtag_vfs_use_driver();

  auto *loader = new (std::nothrow) Loader(sd_mounted);
  TaskHandle_t handle = nullptr;
  const bool created =
      loader != nullptr &&
      (xTaskCreatePinnedToCoreWithCaps(task_entry, "sd_loader", kTaskStackBytes, loader,
                                       kTaskPriority, &handle, tskNO_AFFINITY,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ||
       xTaskCreatePinnedToCore(task_entry, "sd_loader", kTaskStackBytes, loader, kTaskPriority,
                               &handle, tskNO_AFFINITY) == pdPASS);
  if (!created) {
    delete loader;
    usb_serial_jtag_vfs_use_nonblocking();
    usb_serial_jtag_driver_uninstall();
    ESP_LOGW(kTag, "Could not start the USB microSD loader task");
    return false;
  }
  ESP_LOGI(kTag, "USB microSD loader ready (%s/, up to %" PRIu32 " MiB)%s",
           std::string(sd_loader::kAllowedDirectories[0]).c_str(), kMaxFileBytes / (1024 * 1024),
           sd_mounted ? "" : "; microSD unavailable");
  return true;
}

}  // namespace musicremote
