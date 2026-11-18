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

std::string absolute(const std::string &relpath) {
  return std::string(kMountPoint) + "/" + relpath;
}

enum class JournalRead { absent, read, error };

// Reads a journal (a phase byte and an 8.3 name).
JournalRead read_journal(const std::string &path, std::string &content) {
  const int journal = open(path.c_str(), O_RDONLY);
  if (journal < 0) {
    return errno == ENOENT ? JournalRead::absent : JournalRead::error;
  }
  char buffer[24] = {};
  const ssize_t length = read(journal, buffer, sizeof(buffer) - 1);
  close(journal);
  if (length < 0) {
    return JournalRead::error;
  }
  content.assign(buffer, static_cast<size_t>(length));
  return JournalRead::read;
}

class Loader {
 public:
  explicit Loader(bool sd_mounted) : sd_mounted_(sd_mounted) {}

  [[noreturn]] void run() {
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
    if (!promotion_settled(*path)) {
      reply_nak("BEGIN", "recovery");
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

  // True unless this stem has a journal that boot recovery left in place
  // (ambiguous) or could not read. Uploading then could truncate or delete a
  // name that shares clusters with the target.
  bool promotion_settled(const Relpath &path) {
    std::string content;
    switch (read_journal(absolute(path.journal()), content)) {
      case JournalRead::absent:
        return true;
      case JournalRead::error:
        return false;
      case JournalRead::read: {
        const auto journal = sd_loader::parse_journal(path.directory, path.stem + ".jnl", content);
        return journal && journal->phase == sd_loader::kJournalCommitted;
      }
    }
    return false;
  }

  // Replaces the target with the verified staging file, following the
  // journalled sequence in sd_loader_policy.hpp so a reset at any point is
  // recoverable at boot. The previous target becomes <stem>.bak rather than
  // being deleted: a reader that opened it before the upload (the run-time
  // CJK font holds its file open) keeps valid clusters until reboot, when
  // recovery deletes it.
  bool promote() {
    const std::string target = absolute(path_->target());
    const std::string staging = absolute(path_->staging());
    const std::string backup = absolute(path_->backup());
    const std::string journal = absolute(path_->journal());
    struct stat status {};
    const bool had_target = stat(target.c_str(), &status) == 0;
    if (had_target && !S_ISREG(status.st_mode)) {
      return false;
    }
    if (unlink(backup.c_str()) != 0 && errno != ENOENT) {
      return false;
    }
    const char phase = had_target ? sd_loader::kJournalReplacing : sd_loader::kJournalCreating;
    if (!write_journal(journal, sd_loader::journal_content(phase, path_->name()))) {
      return false;
    }
    if (had_target && rename(target.c_str(), backup.c_str()) != 0) {
      unlink(journal.c_str());
      return false;
    }
    if (rename(staging.c_str(), target.c_str()) != 0) {
      if (!had_target || rename(backup.c_str(), target.c_str()) == 0) {
        unlink(journal.c_str());
      }
      return false;
    }
    if (!commit_journal(journal)) {
      // The file is in place; boot recovery finishes the cleanup.
      ESP_LOGW(kTag, "Could not commit %s", journal.c_str());
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

  static bool write_journal(const std::string &path, const std::string &content) {
    const int journal = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (journal < 0) {
      return false;
    }
    const bool written = write(journal, content.data(), content.size()) ==
                             static_cast<ssize_t>(content.size()) &&
                         fsync(journal) == 0;
    close(journal);
    return written;
  }

  // Overwrites only the phase byte, so a reset leaves either phase intact.
  static bool commit_journal(const std::string &path) {
    const int journal = open(path.c_str(), O_WRONLY);
    if (journal < 0) {
      return false;
    }
    const char committed = sd_loader::kJournalCommitted;
    const bool written = write(journal, &committed, 1) == 1 && fsync(journal) == 0;
    close(journal);
    return written;
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

enum class Lookup { absent, present, error };

Lookup lookup(const std::string &path) {
  struct stat status {};
  if (stat(path.c_str(), &status) == 0) {
    return Lookup::present;
  }
  return errno == ENOENT ? Lookup::absent : Lookup::error;
}

// Recovers one stem from its journal (see sd_loader::recovery_actions). Any
// lookup or read error other than "not found" leaves everything for the next
// boot.
void recover_journal(const std::string &directory, std::string_view directory_name,
                     const std::string &name) {
  const std::string journal_path = directory + "/" + name;
  const std::string stem = name.substr(0, name.rfind('.'));
  std::string content;
  if (read_journal(journal_path, content) != JournalRead::read) {
    return;
  }
  const auto journal = sd_loader::parse_journal(directory_name, name, content);
  const std::string staging = directory + "/" + stem + ".tmp";
  const std::string backup = directory + "/" + stem + ".bak";
  const std::string target = journal ? directory + "/" + journal->target : std::string();
  const Lookup staging_found = lookup(staging);
  const Lookup backup_found = lookup(backup);
  const Lookup target_found = journal ? lookup(target) : Lookup::absent;
  if (staging_found == Lookup::error || backup_found == Lookup::error ||
      target_found == Lookup::error) {
    ESP_LOGW(kTag, "Could not inspect %s; recovery deferred to the next boot",
             journal_path.c_str());
    return;
  }
  const auto actions =
      sd_loader::recovery_actions(journal, staging_found == Lookup::present,
                                  backup_found == Lookup::present,
                                  target_found == Lookup::present);
  if (actions.ambiguous) {
    ESP_LOGW(kTag,
             "Interrupted replacement of %s left entries that may share data; leaving them "
             "untouched and refusing uploads of it (copy the card's fonts/ off and back to "
             "repair)",
             target.c_str());
    return;
  }
  if (!journal) {
    ESP_LOGW(kTag, "Ignoring unreadable %s; any backup is kept", journal_path.c_str());
  }
  if (actions.restore_backup) {
    if (rename(backup.c_str(), target.c_str()) != 0) {
      ESP_LOGE(kTag, "Could not restore %s from %s", target.c_str(), backup.c_str());
      return;
    }
    ESP_LOGW(kTag, "Restored %s from an interrupted replacement", target.c_str());
  }
  bool handled = true;
  if (actions.remove_staging) {
    handled = unlink(staging.c_str()) == 0 && handled;
  }
  if (actions.remove_backup) {
    const bool removed = unlink(backup.c_str()) == 0;
    if (removed) {
      ESP_LOGI(kTag, "Removed stale %s", backup.c_str());
    }
    handled = removed && handled;
  }
  if (actions.remove_journal && handled) {
    unlink(journal_path.c_str());
  }
}

// Finishes or undoes interrupted uploads and promotions before any upload or
// file reader starts. One streaming pass handles each reserved entry as it is
// listed, by direct lookups, so memory does not grow with the directory. An
// orphan *.TMP (no journal for its stem) is an interrupted upload.
void recover_directory(std::string_view directory_name) {
  const std::string directory = absolute(std::string(directory_name));
  DIR *listing = opendir(directory.c_str());
  if (listing == nullptr) {
    return;
  }
  while (const dirent *entry = readdir(listing)) {
    if (entry->d_type != DT_REG) {
      continue;
    }
    const std::string name(entry->d_name);
    switch (sd_loader::reserved_kind(name)) {
      case sd_loader::ReservedKind::journal:
        recover_journal(directory, directory_name, name);
        break;
      case sd_loader::ReservedKind::staging: {
        const std::string stem = name.substr(0, name.rfind('.'));
        const std::string path = directory + "/" + name;
        if (lookup(directory + "/" + stem + ".jnl") == Lookup::absent &&
            unlink(path.c_str()) == 0) {
          ESP_LOGI(kTag, "Removed stale %s", path.c_str());
        }
        break;
      }
      case sd_loader::ReservedKind::none:
        break;
    }
  }
  closedir(listing);
}

}  // namespace

bool start_sd_loader(bool sd_mounted) {
  if (sd_mounted) {
    for (const auto directory : sd_loader::kAllowedDirectories) {
      recover_directory(directory);
    }
  }
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
