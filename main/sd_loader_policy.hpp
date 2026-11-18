#pragma once

// Host-testable policy for the USB microSD loader (issue #49): the framed
// protocol, its parser, the path allow-list, and the per-transfer sequence
// rules. Nothing here touches ESP-IDF, the card, or the USB peripheral.
//
// Protocol (host to device, over the USB-Serial-JTAG RX stream). Every
// command is one ASCII line; the host starts each with '\n' so a partial line
// left by an earlier interrupted run is terminated first.
//
//   @@LOAD QUERY <relpath>
//   @@LOAD BEGIN <relpath> <size> <sha256hex>
//   @@LOAD DATA <seq> <length> <crc32hex>     followed by <length> raw bytes
//   @@LOAD END
//   @@LOAD ABORT
//
// Device to host. Replies share the port with log text, so each is written as
// one contiguous line beginning "\n@@LOAD"; the host finds the token anywhere
// in a line and ignores everything else.
//
//   @@LOADINFO <relpath> <size> <sha256hex>   QUERY found the file
//   @@LOADACK BEGIN <relpath>                 ready for DATA sequence 0
//   @@LOADACK <seq>                           chunk verified (or duplicate)
//   @@LOADOK <relpath> <size> <sha256hex>     verified and renamed into place
//   @@LOADNAK <subject> <reason>              subject: QUERY, BEGIN, END,
//                                             ABORT, a sequence number, or -
//
// Flow control is stop-and-wait: the host sends one DATA frame and waits for
// its ACK or NAK. The device acknowledges a chunk after its CRC check and
// before writing it, so the next frame crosses USB while the card writes; the
// RX ring buffer therefore holds at most two frames. A write failure is
// reported as a fatal NAK on the next frame or on END.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace musicremote::sd_loader {

inline constexpr std::string_view kCommandPrefix = "@@LOAD";
inline constexpr uint32_t kMaxFileBytes = 16u * 1024u * 1024u;
inline constexpr uint32_t kMaxChunkBytes = 4096;
inline constexpr size_t kMaxLineBytes = 160;
inline constexpr size_t kMaxRelpathBytes = 32;
// An open transfer with no received bytes for this long is discarded.
inline constexpr uint32_t kIdleTimeoutMs = 5000;
// A DATA frame whose payload does not arrive within this time is NAKed with
// "timeout" and the receiver returns to line mode.
inline constexpr uint32_t kPayloadTimeoutMs = 1000;
// RX ring: one frame being processed plus one in flight, with slack for
// stray bytes. The ISR drops bytes when it is full, so this must stay above
// two maximum frames.
inline constexpr size_t kRxRingBytes = 2 * (kMaxChunkBytes + kMaxLineBytes) + 512;
static_assert(kRxRingBytes >= 2 * (kMaxChunkBytes + kMaxLineBytes));

// Directories under /sdcard that may receive files. Each upload is exactly
// "<directory>/<8.3 name>"; nested paths are refused.
inline constexpr std::array<std::string_view, 1> kAllowedDirectories{"fonts"};

using Sha256 = std::array<uint8_t, 32>;

// ---------------------------------------------------------------- CRC-32

// IEEE 802.3 CRC-32 (reflected 0xEDB88320), identical to Python's
// zlib.crc32. Pass the previous result to continue a running checksum.
inline uint32_t crc32(const uint8_t *data, size_t length, uint32_t previous = 0) {
  static constexpr auto table = [] {
    std::array<uint32_t, 256> entries{};
    for (uint32_t index = 0; index < entries.size(); ++index) {
      uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0 ? (value >> 1) ^ 0xEDB88320u : value >> 1;
      }
      entries[index] = value;
    }
    return entries;
  }();
  uint32_t crc = ~previous;
  for (size_t index = 0; index < length; ++index) {
    crc = table[(crc ^ data[index]) & 0xffu] ^ (crc >> 8);
  }
  return ~crc;
}

// ---------------------------------------------------------------- numbers

// Unsigned decimal without sign, spaces, or leading zeros (except "0").
inline std::optional<uint32_t> parse_decimal(std::string_view text) {
  if (text.empty() || text.size() > 10 || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<uint64_t>(character - '0');
  }
  if (value > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(value);
}

inline std::optional<uint8_t> hex_nibble(char character) {
  if (character >= '0' && character <= '9') return static_cast<uint8_t>(character - '0');
  if (character >= 'a' && character <= 'f') return static_cast<uint8_t>(character - 'a' + 10);
  if (character >= 'A' && character <= 'F') return static_cast<uint8_t>(character - 'A' + 10);
  return std::nullopt;
}

// Exactly eight hex digits.
inline std::optional<uint32_t> parse_crc32_hex(std::string_view text) {
  if (text.size() != 8) {
    return std::nullopt;
  }
  uint32_t value = 0;
  for (const char character : text) {
    const auto nibble = hex_nibble(character);
    if (!nibble) {
      return std::nullopt;
    }
    value = (value << 4) | *nibble;
  }
  return value;
}

// Exactly 64 hex digits.
inline std::optional<Sha256> parse_sha256_hex(std::string_view text) {
  if (text.size() != 64) {
    return std::nullopt;
  }
  Sha256 digest{};
  for (size_t index = 0; index < digest.size(); ++index) {
    const auto high = hex_nibble(text[index * 2]);
    const auto low = hex_nibble(text[index * 2 + 1]);
    if (!high || !low) {
      return std::nullopt;
    }
    digest[index] = static_cast<uint8_t>((*high << 4) | *low);
  }
  return digest;
}

inline std::string sha256_hex(const Sha256 &digest) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2);
  for (const uint8_t byte : digest) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 0x0f]);
  }
  return text;
}

// ---------------------------------------------------------------- paths

struct Relpath {
  std::string directory;  // allow-listed directory, e.g. "fonts"
  std::string stem;       // 8.3 base name, e.g. "cjk"
  std::string extension;  // 8.3 extension without the dot; may be empty

  std::string name() const { return extension.empty() ? stem : stem + "." + extension; }
  std::string target() const { return directory + "/" + name(); }
  // The card is mounted without long file names, so staging and backup names
  // replace the extension instead of appending to it: fonts/cjk.tmp.
  std::string staging() const { return directory + "/" + stem + ".tmp"; }
  std::string backup() const { return directory + "/" + stem + ".bak"; }
  // Records name() while the target is moved aside, because <stem>.bak alone
  // cannot say which extension it came from (see journal_outcome()).
  std::string journal() const { return directory + "/" + stem + ".jnl"; }
};

inline bool ascii_iequals(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t index = 0; index < left.size(); ++index) {
    char a = left[index];
    char b = right[index];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) {
      return false;
    }
  }
  return true;
}

inline bool name_character(char character) {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '_' || character == '-';
}

// Accepts only "<allow-listed directory>/<stem>[.<ext>]" with an 8.3 name of
// [A-Za-z0-9_-] characters. Absolute paths, "..", ".", backslashes, nested
// directories, and the reserved .tmp/.bak/.jnl extensions are refused.
inline std::optional<Relpath> validate_relpath(std::string_view text) {
  if (text.empty() || text.size() > kMaxRelpathBytes) {
    return std::nullopt;
  }
  const size_t slash = text.find('/');
  if (slash == std::string_view::npos || text.find('/', slash + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view directory = text.substr(0, slash);
  const std::string_view name = text.substr(slash + 1);
  bool allowed = false;
  for (const auto candidate : kAllowedDirectories) {
    allowed = allowed || directory == candidate;
  }
  if (!allowed) {
    return std::nullopt;
  }
  const size_t dot = name.find('.');
  if (dot != std::string_view::npos && name.find('.', dot + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view stem = name.substr(0, dot);
  const std::string_view extension =
      dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
  if (stem.empty() || stem.size() > 8 || extension.size() > 3 ||
      (dot != std::string_view::npos && extension.empty())) {
    return std::nullopt;
  }
  for (const char character : stem) {
    if (!name_character(character)) return std::nullopt;
  }
  for (const char character : extension) {
    if (!name_character(character)) return std::nullopt;
  }
  if (ascii_iequals(extension, "tmp") || ascii_iequals(extension, "bak") ||
      ascii_iequals(extension, "jnl")) {
    return std::nullopt;
  }
  return Relpath{std::string(directory), std::string(stem), std::string(extension)};
}

// Boot-time recovery. Promotion writes <stem>.jnl naming the target, renames
// the target to <stem>.bak, then renames <stem>.tmp to the target. Recovery
// is driven by the journals, with the backup and target looked up by path,
// so it never depends on seeing every directory entry in one scan.

enum class ReservedKind { none, staging, journal };

// Classifies a directory entry (as readdir returns it, typically upper case
// without LFN support): *.TMP is an interrupted upload, *.JNL a promotion
// record. *.BAK is not listed: backups are handled through their journals.
inline ReservedKind reserved_kind(std::string_view name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) {
    return ReservedKind::none;
  }
  const std::string_view extension = name.substr(dot + 1);
  if (ascii_iequals(extension, "tmp")) return ReservedKind::staging;
  if (ascii_iequals(extension, "jnl")) return ReservedKind::journal;
  return ReservedKind::none;
}

enum class JournalOutcome {
  remove_backup,   // the target was promoted; the backup is superseded
  restore_backup,  // a reset between the renames: rename the backup to target
  no_backup,       // nothing was moved aside (or it was already handled)
  unusable,        // the record is not a valid target for this stem
};

// Decides what one journal means. recorded is the journal's content, the
// target's name within directory; backup_exists and target_exists are
// direct lookups of <stem>.bak and that target. The journal is deleted only
// after the outcome's action succeeds. An unusable journal is deleted but its
// backup, which may be the only copy, is kept.
inline JournalOutcome journal_outcome(std::string_view directory, std::string_view journal_name,
                                      std::string_view recorded, bool backup_exists,
                                      bool target_exists) {
  const std::string_view stem = journal_name.substr(0, journal_name.rfind('.'));
  const auto target = validate_relpath(std::string(directory) + "/" + std::string(recorded));
  if (!target || !ascii_iequals(target->stem, stem)) {
    return JournalOutcome::unusable;
  }
  if (!backup_exists) {
    return JournalOutcome::no_backup;
  }
  return target_exists ? JournalOutcome::remove_backup : JournalOutcome::restore_backup;
}

// ---------------------------------------------------------------- lines

// Collects one bounded command line from the byte stream. Lines longer than
// kMaxLineBytes are discarded through their terminating newline.
class LineAssembler {
 public:
  // Returns true when byte completes a line; line() is then valid until the
  // next push.
  bool push(uint8_t byte) {
    if (complete_) {
      length_ = 0;
      complete_ = false;
    }
    if (byte == '\n') {
      const bool usable = !overflow_;
      overflow_ = false;
      if (!usable) {
        length_ = 0;
        return false;
      }
      if (length_ > 0 && buffer_[length_ - 1] == '\r') {
        --length_;
      }
      complete_ = true;
      return true;
    }
    if (overflow_) {
      return false;
    }
    if (length_ == buffer_.size()) {
      overflow_ = true;
      length_ = 0;
      return false;
    }
    buffer_[length_++] = static_cast<char>(byte);
    return false;
  }

  std::string_view line() const {
    return complete_ ? std::string_view(buffer_.data(), length_) : std::string_view{};
  }

  void reset() {
    length_ = 0;
    overflow_ = false;
    complete_ = false;
  }

 private:
  std::array<char, kMaxLineBytes> buffer_{};
  size_t length_{0};
  bool overflow_{false};
  bool complete_{false};
};

// ---------------------------------------------------------------- commands

enum class CommandType { none, invalid, query, begin, data, end, abort };

struct Command {
  CommandType type{CommandType::none};
  std::string_view relpath;  // query and begin; points into the parsed line
  uint32_t size{0};          // begin
  Sha256 sha256{};           // begin
  uint32_t sequence{0};      // data
  uint32_t length{0};        // data
  uint32_t crc32{0};         // data
};

// Lines that do not start with "@@LOAD " are log echo, keystrokes, or
// garbage and yield CommandType::none. A line that starts with the prefix but
// is malformed yields CommandType::invalid.
inline Command parse_command(std::string_view line) {
  Command command;
  if (line.size() < kCommandPrefix.size() + 1 ||
      line.substr(0, kCommandPrefix.size()) != kCommandPrefix ||
      line[kCommandPrefix.size()] != ' ') {
    return command;
  }
  command.type = CommandType::invalid;
  std::array<std::string_view, 5> fields{};
  size_t count = 0;
  std::string_view rest = line.substr(kCommandPrefix.size() + 1);
  while (!rest.empty()) {
    const size_t space = rest.find(' ');
    const std::string_view field = rest.substr(0, space);
    if (field.empty() || count == fields.size()) {
      return command;
    }
    fields[count++] = field;
    if (space == std::string_view::npos) {
      break;
    }
    rest = rest.substr(space + 1);
    if (rest.empty()) {
      return command;  // trailing space
    }
  }
  if (count == 0) {
    return command;
  }
  const std::string_view verb = fields[0];
  if (verb == "QUERY" && count == 2) {
    command.type = CommandType::query;
    command.relpath = fields[1];
  } else if (verb == "BEGIN" && count == 4) {
    const auto size = parse_decimal(fields[2]);
    const auto digest = parse_sha256_hex(fields[3]);
    if (!size || !digest) {
      return command;
    }
    command.type = CommandType::begin;
    command.relpath = fields[1];
    command.size = *size;
    command.sha256 = *digest;
  } else if (verb == "DATA" && count == 4) {
    const auto sequence = parse_decimal(fields[1]);
    const auto length = parse_decimal(fields[2]);
    const auto crc = parse_crc32_hex(fields[3]);
    if (!sequence || !length || !crc || *length == 0 || *length > kMaxChunkBytes) {
      return command;
    }
    command.type = CommandType::data;
    command.sequence = *sequence;
    command.length = *length;
    command.crc32 = *crc;
  } else if (verb == "END" && count == 1) {
    command.type = CommandType::end;
  } else if (verb == "ABORT" && count == 1) {
    command.type = CommandType::abort;
  }
  return command;
}

// ---------------------------------------------------------------- session

enum class ChunkVerdict {
  accept,        // write it and advance
  duplicate,     // the previous chunk again (its ACK was missed); re-ACK only
  bad_crc,       // retryable
  out_of_order,  // fatal: host and device disagree on the sequence
  overrun,       // fatal: more bytes than BEGIN declared
};

// Sequence and size accounting for one transfer.
class TransferProgress {
 public:
  explicit TransferProgress(uint32_t declared_size = 0) : declared_size_(declared_size) {}

  ChunkVerdict check(uint32_t sequence, uint32_t length, uint32_t declared_crc,
                     uint32_t actual_crc) const {
    if (declared_crc != actual_crc) {
      return ChunkVerdict::bad_crc;
    }
    if (next_sequence_ > 0 && sequence == next_sequence_ - 1 && length == last_length_ &&
        actual_crc == last_crc_) {
      return ChunkVerdict::duplicate;
    }
    if (sequence != next_sequence_) {
      return ChunkVerdict::out_of_order;
    }
    if (length > declared_size_ - received_) {
      return ChunkVerdict::overrun;
    }
    return ChunkVerdict::accept;
  }

  void commit(uint32_t length, uint32_t crc) {
    received_ += length;
    last_length_ = length;
    last_crc_ = crc;
    ++next_sequence_;
  }

  bool complete() const { return received_ == declared_size_; }
  uint32_t received() const { return received_; }
  uint32_t declared_size() const { return declared_size_; }
  uint32_t next_sequence() const { return next_sequence_; }

 private:
  uint32_t declared_size_{0};
  uint32_t received_{0};
  uint32_t next_sequence_{0};
  uint32_t last_length_{0};
  uint32_t last_crc_{0};
};

// ---------------------------------------------------------------- replies

// Each reply begins with '\n' so it starts a fresh line even when it lands in
// the middle of a log line, and ends with '\n'. Each returns the formatted
// length, or 0 if it did not fit.
inline constexpr size_t kMaxReplyBytes = 160;
using ReplyBuffer = std::array<char, kMaxReplyBytes>;

inline size_t reply_length(int written) {
  return written > 0 && static_cast<size_t>(written) < kMaxReplyBytes
             ? static_cast<size_t>(written)
             : 0;
}

inline size_t reply_ack_begin(ReplyBuffer &out, const Relpath &path) {
  return reply_length(
      std::snprintf(out.data(), out.size(), "\n@@LOADACK BEGIN %s\n", path.target().c_str()));
}

inline size_t reply_ack(ReplyBuffer &out, uint32_t sequence) {
  return reply_length(std::snprintf(out.data(), out.size(), "\n@@LOADACK %lu\n",
                                    static_cast<unsigned long>(sequence)));
}

inline size_t reply_nak(ReplyBuffer &out, const char *subject, const char *reason) {
  return reply_length(
      std::snprintf(out.data(), out.size(), "\n@@LOADNAK %s %s\n", subject, reason));
}

inline size_t reply_nak(ReplyBuffer &out, uint32_t sequence, const char *reason) {
  return reply_length(std::snprintf(out.data(), out.size(), "\n@@LOADNAK %lu %s\n",
                                    static_cast<unsigned long>(sequence), reason));
}

enum class FileReply { info, ok };

inline size_t reply_file(ReplyBuffer &out, FileReply kind, const Relpath &path, uint32_t size,
                         const Sha256 &digest) {
  return reply_length(std::snprintf(out.data(), out.size(), "\n@@%s %s %lu %s\n",
                                    kind == FileReply::info ? "LOADINFO" : "LOADOK",
                                    path.target().c_str(), static_cast<unsigned long>(size),
                                    sha256_hex(digest).c_str()));
}

}  // namespace musicremote::sd_loader
