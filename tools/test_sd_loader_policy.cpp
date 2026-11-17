#include "sd_loader_policy.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace loader = musicremote::sd_loader;
using loader::ChunkVerdict;
using loader::CommandType;

namespace {

uint32_t crc_of(std::string_view text) {
  return loader::crc32(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

std::vector<std::string> lines_from(std::string_view stream) {
  loader::LineAssembler assembler;
  std::vector<std::string> lines;
  for (const char character : stream) {
    if (assembler.push(static_cast<uint8_t>(character))) {
      lines.emplace_back(assembler.line());
    }
  }
  return lines;
}

constexpr char kSha[] = "2392015530438bafc48edfc4aee6d9de2387f627a6134d8ab3dfcc99d21c8240";

void test_crc32() {
  // zlib.crc32 reference values.
  assert(crc_of("") == 0);
  assert(crc_of("123456789") == 0xCBF43926u);
  assert(crc_of("The quick brown fox jumps over the lazy dog") == 0x414FA339u);
  const std::string text = "The quick brown fox jumps over the lazy dog";
  const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
  assert(loader::crc32(bytes + 10, text.size() - 10, loader::crc32(bytes, 10)) == 0x414FA339u);
}

void test_numbers() {
  assert(loader::parse_decimal("0") == 0u);
  assert(loader::parse_decimal("4033576") == 4033576u);
  assert(loader::parse_decimal("4294967295") == 4294967295u);
  assert(!loader::parse_decimal("4294967296"));
  assert(!loader::parse_decimal("99999999999"));
  assert(!loader::parse_decimal(""));
  assert(!loader::parse_decimal("012"));
  assert(!loader::parse_decimal("-1"));
  assert(!loader::parse_decimal("+1"));
  assert(!loader::parse_decimal("1x"));

  assert(loader::parse_crc32_hex("cbf43926") == 0xCBF43926u);
  assert(loader::parse_crc32_hex("CBF43926") == 0xCBF43926u);
  assert(!loader::parse_crc32_hex("cbf4392"));
  assert(!loader::parse_crc32_hex("cbf439260"));
  assert(!loader::parse_crc32_hex("cbf4392g"));

  const auto digest = loader::parse_sha256_hex(kSha);
  assert(digest && (*digest)[0] == 0x23 && (*digest)[31] == 0x40);
  assert(loader::sha256_hex(*digest) == kSha);
  assert(!loader::parse_sha256_hex(std::string_view(kSha, 63)));
  std::string bad = kSha;
  bad[5] = 'z';
  assert(!loader::parse_sha256_hex(bad));
}

void test_paths() {
  const auto font = loader::validate_relpath("fonts/cjk.ttf");
  assert(font);
  assert(font->target() == "fonts/cjk.ttf");
  assert(font->staging() == "fonts/cjk.tmp");
  assert(font->backup() == "fonts/cjk.bak");

  const auto bare = loader::validate_relpath("fonts/README");
  assert(bare && bare->target() == "fonts/README" && bare->staging() == "fonts/README.tmp");
  assert(loader::validate_relpath("fonts/a_b-12.otf"));
  assert(loader::validate_relpath("fonts/ABCDEFGH.TTF"));

  for (const char *rejected : {
           "",                          // empty
           "cjk.ttf",                   // no directory
           "/fonts/cjk.ttf",            // absolute
           "/sdcard/fonts/cjk.ttf",     // absolute
           "fonts/../cjk.ttf",          // traversal / nested
           "../fonts/cjk.ttf",          // traversal
           "fonts/..",                  // dot-dot name
           "fonts/.",                   // dot name
           "fonts/",                    // empty name
           "fonts//cjk.ttf",            // empty component
           "fonts/sub/cjk.ttf",         // nested
           "fonts\\cjk.ttf",            // backslash
           "mremote/INDEX.DAT",         // not allow-listed
           "FONTS/cjk.ttf",             // allow-list is exact
           "fonts2/cjk.ttf",            // allow-list is exact
           "font/cjk.ttf",              // allow-list is exact
           "fonts/toolongname.ttf",     // stem over 8
           "fonts/cjk.ttff",            // extension over 3
           "fonts/cjk.",                // empty extension
           "fonts/.ttf",                // empty stem
           "fonts/cjk.tar.gz",          // two dots
           "fonts/cjk.tmp",             // reserved staging name
           "fonts/cjk.TMP",             // reserved staging name
           "fonts/cjk.bak",             // reserved backup name
           "fonts/cj k.ttf",            // space
           "fonts/cjk*.ttf",            // wildcard
           "fonts/cjk.ttf\n",           // control character
           "fonts/abcdefgh.ttfxxxxxxxxxxxxxxx",  // too long
       }) {
    assert(!loader::validate_relpath(rejected));
  }
  assert(!loader::validate_relpath(std::string_view("fonts/cjk\0.ttf", 14)));
}

void test_stale_entries() {
  const auto stale = loader::stale_entries({"CJK.TTF", "CJK.BAK", "CJK.TMP", "LATIN.BAK",
                                            "OTHER.TTF", "NOTES.TXT"});
  // CJK.TMP: interrupted upload. CJK.BAK: superseded by CJK.TTF. LATIN.BAK is
  // the only copy left by a reset between the promotion renames, so it stays.
  assert(stale.size() == 2);
  assert(std::find(stale.begin(), stale.end(), "CJK.BAK") != stale.end());
  assert(std::find(stale.begin(), stale.end(), "CJK.TMP") != stale.end());
  assert(loader::stale_entries({"cjk.bak", "cjk.tmp"}) == std::vector<std::string>{"cjk.tmp"});
  assert(loader::stale_entries({}).empty());
}

void test_line_assembler() {
  const auto lines = lines_from("I (123) boot: log line\r\n@@LOAD END\n\n@@LOAD ABORT\r\n");
  assert(lines.size() == 4);
  assert(lines[0] == "I (123) boot: log line");
  assert(lines[1] == "@@LOAD END");
  assert(lines[2].empty());
  assert(lines[3] == "@@LOAD ABORT");

  // An overlong line is discarded through its newline; the next line survives.
  std::string stream(loader::kMaxLineBytes + 20, 'x');
  stream += "\n@@LOAD END\n";
  const auto recovered = lines_from(stream);
  assert(recovered.size() == 1 && recovered[0] == "@@LOAD END");

  // Exactly kMaxLineBytes fits.
  std::string exact(loader::kMaxLineBytes, 'y');
  const auto fitted = lines_from(exact + "\n");
  assert(fitted.size() == 1 && fitted[0].size() == loader::kMaxLineBytes);

  loader::LineAssembler assembler;
  for (const char character : std::string_view("partial")) {
    assembler.push(static_cast<uint8_t>(character));
  }
  assembler.reset();
  bool done = false;
  for (const char character : std::string_view("@@LOAD END\n")) {
    done = assembler.push(static_cast<uint8_t>(character));
  }
  assert(done && assembler.line() == "@@LOAD END");
}

void test_commands() {
  assert(loader::parse_command("").type == CommandType::none);
  assert(loader::parse_command("I (5) sd_loader: hello").type == CommandType::none);
  assert(loader::parse_command("@@LOADX END").type == CommandType::none);
  assert(loader::parse_command("@@LOAD").type == CommandType::none);
  assert(loader::parse_command("x@@LOAD END").type == CommandType::none);

  const auto query = loader::parse_command("@@LOAD QUERY fonts/cjk.ttf");
  assert(query.type == CommandType::query && query.relpath == "fonts/cjk.ttf");

  // Command::relpath views the parsed line, so the line must outlive it.
  const std::string begin_line = std::string("@@LOAD BEGIN fonts/cjk.ttf 4033576 ") + kSha;
  const auto begin = loader::parse_command(begin_line);
  assert(begin.type == CommandType::begin);
  assert(begin.relpath == "fonts/cjk.ttf" && begin.size == 4033576u);
  assert(loader::sha256_hex(begin.sha256) == kSha);
  // BEGIN parses any size; the firmware applies kMaxFileBytes so it can
  // reply "size" rather than "syntax".
  assert(loader::parse_command(std::string("@@LOAD BEGIN fonts/x.ttf 0 ") + kSha).type ==
         CommandType::begin);

  const auto data = loader::parse_command("@@LOAD DATA 7 4096 cbf43926");
  assert(data.type == CommandType::data);
  assert(data.sequence == 7 && data.length == 4096 && data.crc32 == 0xCBF43926u);

  assert(loader::parse_command("@@LOAD END").type == CommandType::end);
  assert(loader::parse_command("@@LOAD ABORT").type == CommandType::abort);

  for (const std::string &invalid : {
           std::string("@@LOAD "),
           std::string("@@LOAD  END"),
           std::string("@@LOAD END "),
           std::string("@@LOAD END now"),
           std::string("@@LOAD QUERY"),
           std::string("@@LOAD QUERY a b"),
           std::string("@@LOAD BEGIN fonts/cjk.ttf 12"),
           std::string("@@LOAD BEGIN fonts/cjk.ttf -1 ") + kSha,
           std::string("@@LOAD BEGIN fonts/cjk.ttf 99999999999 ") + kSha,
           std::string("@@LOAD BEGIN fonts/cjk.ttf 12 abc"),
           std::string("@@LOAD DATA 0 0 00000000"),
           std::string("@@LOAD DATA 0 4097 00000000"),
           std::string("@@LOAD DATA 0 10 0000000"),
           std::string("@@LOAD DATA x 10 00000000"),
           std::string("@@LOAD DATA 0 10"),
           std::string("@@LOAD FORMAT"),
           std::string("@@LOAD a b c d e f"),
       }) {
    assert(loader::parse_command(invalid).type == CommandType::invalid);
  }
}

void test_progress() {
  loader::TransferProgress progress(10);
  const uint32_t a = crc_of("abcdef");
  const uint32_t b = crc_of("ghij");
  assert(progress.check(0, 6, a ^ 1, a) == ChunkVerdict::bad_crc);
  assert(progress.check(1, 6, a, a) == ChunkVerdict::out_of_order);
  assert(progress.check(0, 6, a, a) == ChunkVerdict::accept);
  progress.commit(6, a);
  assert(!progress.complete() && progress.received() == 6 && progress.next_sequence() == 1);

  // A retransmission of chunk 0 (its ACK was lost) is re-ACKed, not written.
  assert(progress.check(0, 6, a, a) == ChunkVerdict::duplicate);
  // A different chunk claiming sequence 0 is not a duplicate.
  assert(progress.check(0, 4, b, b) == ChunkVerdict::out_of_order);
  assert(progress.check(2, 4, b, b) == ChunkVerdict::out_of_order);

  // More bytes than BEGIN declared.
  assert(progress.check(1, 5, crc_of("ghijk"), crc_of("ghijk")) == ChunkVerdict::overrun);

  assert(progress.check(1, 4, b, b) == ChunkVerdict::accept);
  progress.commit(4, b);
  assert(progress.complete() && progress.received() == 10);
  assert(progress.check(2, 1, crc_of("z"), crc_of("z")) == ChunkVerdict::overrun);

  loader::TransferProgress empty(0);
  assert(empty.complete());
}

void test_replies() {
  loader::ReplyBuffer out;
  const auto path = *loader::validate_relpath("fonts/cjk.ttf");
  auto text = [&](size_t length) { return std::string(out.data(), length); };

  assert(text(loader::reply_ack_begin(out, path)) == "\n@@LOADACK BEGIN fonts/cjk.ttf\n");
  assert(text(loader::reply_ack(out, 42)) == "\n@@LOADACK 42\n");
  assert(text(loader::reply_nak(out, 3, "crc")) == "\n@@LOADNAK 3 crc\n");
  assert(text(loader::reply_nak(out, "BEGIN", "path")) == "\n@@LOADNAK BEGIN path\n");
  const auto digest = *loader::parse_sha256_hex(kSha);
  assert(text(loader::reply_file(out, loader::FileReply::ok, path, 4033576, digest)) ==
         std::string("\n@@LOADOK fonts/cjk.ttf 4033576 ") + kSha + "\n");
  assert(text(loader::reply_file(out, loader::FileReply::info, path, 1, digest)) ==
         std::string("\n@@LOADINFO fonts/cjk.ttf 1 ") + kSha + "\n");
  // The longest permitted path still fits.
  const auto longest = *loader::validate_relpath("fonts/abcdefgh.ttf");
  assert(loader::reply_file(out, loader::FileReply::info, longest, 4294967295u, digest) > 0);
}

void test_ring_bound() {
  // Stop-and-wait plus ACK-before-write means at most the frame being
  // processed and one more are buffered.
  static_assert(loader::kRxRingBytes >= 2 * (loader::kMaxChunkBytes + loader::kMaxLineBytes));
  static_assert(loader::kMaxFileBytes == 16u * 1024u * 1024u);
}

}  // namespace

int main() {
  test_crc32();
  test_numbers();
  test_paths();
  test_stale_entries();
  test_line_assembler();
  test_commands();
  test_progress();
  test_replies();
  test_ring_bound();
  return 0;
}
