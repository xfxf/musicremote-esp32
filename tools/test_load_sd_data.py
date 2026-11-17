import hashlib
import io
import re
import unittest
import zlib
from pathlib import Path
from unittest import mock

import load_sd_data as tool

POLICY = Path(__file__).resolve().parent.parent / "main" / "sd_loader_policy.hpp"
LOG = b"I (12345) monkeymote: Connected to AIMP 192.0.2.1:19371\r\n"


class FakeDevice:
    """A serial port backed by a Python model of the firmware loader.

    Replies are interleaved with log text and delivered in small reads, so
    the tool's line splitting and log skipping are exercised.
    """

    def __init__(self, files=None, read_size=7):
        self.files = dict(files or {})
        self.read_size = read_size
        self.output = bytearray(LOG + b"I (12346) sd_loader: partial log li")
        self.input = bytearray()
        self.transfer = None
        self.payload_left = 0
        self.frame = None
        self.payload = bytearray()
        self.corrupt = {}  # sequence -> number of deliveries to corrupt
        self.silent = {}   # sequence -> number of deliveries to ignore
        self.write_fail_at = None
        self.mismatch = False
        self.frames_seen = []
        self.in_waiting = 0

    # serial.Serial surface used by the tool
    def write(self, data):
        self.input.extend(data)
        self._process()
        self.in_waiting = len(self.output)
        return len(data)

    def flush(self):
        pass

    def read(self, size=1):
        count = min(max(size, 1), self.read_size, len(self.output))
        data = bytes(self.output[:count])
        del self.output[:count]
        self.in_waiting = len(self.output)
        return data

    # device model
    def reply(self, text):
        # As the firmware does: the reply starts a fresh line, and log output
        # continues around it.
        self.output.extend(b"\n" + text.encode() + b"\n" + LOG)

    def _process(self):
        while self.input:
            if self.frame is not None:
                take = min(self.payload_left, len(self.input))
                self.payload.extend(self.input[:take])
                del self.input[:take]
                self.payload_left -= take
                if self.payload_left == 0:
                    self._frame_done()
                continue
            newline = self.input.find(b"\n")
            if newline < 0:
                return
            line = bytes(self.input[:newline]).decode()
            del self.input[: newline + 1]
            self._line(line)

    def _line(self, line):
        if not line.startswith("@@LOAD "):
            return
        fields = line.split(" ")[1:]
        if fields[0] == "QUERY":
            if fields[1] in self.files:
                data = self.files[fields[1]]
                self.reply(f"@@LOADINFO {fields[1]} {len(data)} "
                           f"{hashlib.sha256(data).hexdigest()}")
            else:
                self.reply("@@LOADNAK QUERY notfound")
        elif fields[0] == "BEGIN":
            path, size, digest = fields[1], int(fields[2]), fields[3]
            if not path.startswith("fonts/"):
                self.reply("@@LOADNAK BEGIN path")
                return
            self.transfer = {"path": path, "size": size, "sha": digest, "data": bytearray(),
                             "next": 0}
            self.reply(f"@@LOADACK BEGIN {path}")
        elif fields[0] == "DATA":
            self.frame = (int(fields[1]), int(fields[2]), int(fields[3], 16))
            self.payload_left = self.frame[1]
            self.payload = bytearray()
        elif fields[0] == "END":
            transfer, self.transfer = self.transfer, None
            data = bytes(transfer["data"])
            digest = hashlib.sha256(data).hexdigest()
            if self.mismatch or digest != transfer["sha"]:
                self.reply("@@LOADNAK END sha256")
                return
            self.files[transfer["path"]] = data
            self.reply(f"@@LOADOK {transfer['path']} {len(data)} {digest}")
        elif fields[0] == "ABORT":
            self.transfer = None
            self.reply("@@LOADNAK ABORT aborted")

    def _frame_done(self):
        sequence, length, crc = self.frame
        self.frame = None
        payload = bytes(self.payload)
        self.frames_seen.append(sequence)
        if self.silent.get(sequence, 0) > 0:
            self.silent[sequence] -= 1
            return
        if self.corrupt.get(sequence, 0) > 0:
            self.corrupt[sequence] -= 1
            payload = bytes([payload[0] ^ 0xFF]) + payload[1:]
        if zlib.crc32(payload) != crc:
            self.reply(f"@@LOADNAK {sequence} crc")
            return
        transfer = self.transfer
        if transfer["next"] == sequence + 1:
            self.reply(f"@@LOADACK {sequence}")  # duplicate
            return
        if sequence == self.write_fail_at:
            self.reply(f"@@LOADNAK {sequence} write")
            return
        transfer["data"].extend(payload)
        transfer["next"] += 1
        self.reply(f"@@LOADACK {sequence}")


def payload(size):
    return bytes((index * 7 + 3) & 0xFF for index in range(size))


class ParseReplyTest(unittest.TestCase):
    def test_finds_reply_after_log_text(self):
        reply = tool.parse_reply(b"I (5) wifi: sta conn@@LOADACK 12")
        self.assertEqual(reply, tool.Reply("ACK", ("12",)))

    def test_ignores_log_lines_and_unknown_tokens(self):
        self.assertIsNone(tool.parse_reply(b"I (5) sd_loader: Receiving fonts/cjk.ttf"))
        self.assertIsNone(tool.parse_reply(b"@@LOAD DATA 1 2 00000000"))
        self.assertIsNone(tool.parse_reply(b"@@LOADXYZ 1"))
        self.assertIsNone(tool.parse_reply(b""))

    def test_nak_fields(self):
        reply = tool.parse_reply(b"@@LOADNAK 7 crc\r")
        self.assertEqual((reply.kind, reply.subject, reply.reason), ("NAK", "7", "crc"))


class ReplyReaderTest(unittest.TestCase):
    def test_skips_logs_split_across_reads(self):
        port = FakeDevice(read_size=3)
        port.reply("@@LOADACK 0")
        logs = []
        reader = tool.ReplyReader(port, logs.append)
        self.assertEqual(reader.next_reply(1.0), tool.Reply("ACK", ("0",)))
        self.assertIn("I (12345) monkeymote: Connected to AIMP 192.0.2.1:19371", logs)
        # The partial log line was terminated by the reply's leading newline.
        self.assertIn("I (12346) sd_loader: partial log li", logs)

    def test_times_out_without_reply(self):
        port = FakeDevice()
        reader = tool.ReplyReader(port)
        self.assertIsNone(reader.next_reply(0.05))

    def test_drops_unterminated_flood(self):
        port = FakeDevice(read_size=1 << 20)
        port.output = bytearray(b"x" * (tool.MAX_PENDING_BYTES + 10))
        reader = tool.ReplyReader(port)
        self.assertIsNone(reader.next_reply(0.05))
        self.assertLessEqual(len(reader.pending), tool.MAX_PENDING_BYTES)


class FramingTest(unittest.TestCase):
    def test_data_frame(self):
        frame = tool.data_frame(3, b"123456789")
        self.assertEqual(frame, b"\n@@LOAD DATA 3 9 cbf43926\n123456789")

    def test_command_starts_a_fresh_line(self):
        self.assertEqual(tool.command("@@LOAD END"), b"\n@@LOAD END\n")

    def test_limits_match_firmware(self):
        text = POLICY.read_text()
        chunk = re.search(r"kMaxChunkBytes = (\d+);", text)
        self.assertIsNotNone(chunk)
        self.assertEqual(int(chunk.group(1)), tool.CHUNK_BYTES)
        self.assertIn("kMaxFileBytes = 16u * 1024u * 1024u;", text)
        self.assertEqual(tool.MAX_FILE_BYTES, 16 * 1024 * 1024)
        payload_timeout = re.search(r"kPayloadTimeoutMs = (\d+);", text)
        self.assertGreater(tool.ACK_TIMEOUT * 1000, int(payload_timeout.group(1)))


class UploadTest(unittest.TestCase):
    def test_uploads_and_verifies(self):
        device = FakeDevice()
        data = payload(3 * tool.CHUNK_BYTES + 123)
        progress = []
        loader = tool.Loader(device, on_progress=lambda sent, total: progress.append(sent))
        result = loader.upload(data, "fonts/cjk.ttf")
        self.assertEqual(device.files["fonts/cjk.ttf"], data)
        self.assertEqual(result.sha256, hashlib.sha256(data).hexdigest())
        self.assertEqual(device.frames_seen, [0, 1, 2, 3])
        self.assertEqual(progress[-1], len(data))
        self.assertEqual(loader.retries, 0)

    def test_empty_file(self):
        device = FakeDevice()
        tool.Loader(device).upload(b"", "fonts/empty.txt")
        self.assertEqual(device.files["fonts/empty.txt"], b"")
        self.assertEqual(device.frames_seen, [])

    def test_retries_crc_nak(self):
        device = FakeDevice()
        device.corrupt = {1: 2}
        data = payload(3 * tool.CHUNK_BYTES)
        loader = tool.Loader(device)
        loader.upload(data, "fonts/cjk.ttf")
        self.assertEqual(device.files["fonts/cjk.ttf"], data)
        self.assertEqual(device.frames_seen, [0, 1, 1, 1, 2])
        self.assertEqual(loader.retries, 2)

    def test_resends_after_missing_ack(self):
        device = FakeDevice()
        device.silent = {0: 1}
        data = payload(2 * tool.CHUNK_BYTES)
        loader = tool.Loader(device, ack_timeout=0.05)
        loader.upload(data, "fonts/cjk.ttf")
        self.assertEqual(device.files["fonts/cjk.ttf"], data)
        self.assertEqual(device.frames_seen, [0, 0, 1])

    def test_gives_up_after_repeated_crc_failures(self):
        device = FakeDevice()
        device.corrupt = {0: tool.CHUNK_ATTEMPTS}
        with self.assertRaisesRegex(tool.LoaderError, r"chunk 0 failed .*\(crc\)"):
            tool.Loader(device).upload(payload(10), "fonts/cjk.ttf")
        self.assertNotIn("fonts/cjk.ttf", device.files)
        self.assertIsNone(device.transfer)  # ABORT reached the device

    def test_fatal_nak_aborts(self):
        device = FakeDevice()
        device.write_fail_at = 1
        with self.assertRaisesRegex(tool.LoaderError, "rejected chunk 1: write"):
            tool.Loader(device).upload(payload(2 * tool.CHUNK_BYTES), "fonts/cjk.ttf")
        self.assertIsNone(device.transfer)  # ABORT reached the device

    def test_begin_refused(self):
        with self.assertRaisesRegex(tool.LoaderError, "refused BEGIN: path"):
            tool.Loader(FakeDevice()).upload(b"x", "../etc/passwd")

    def test_sha_mismatch_reported(self):
        device = FakeDevice()
        device.mismatch = True
        with self.assertRaisesRegex(tool.LoaderError, "discarded the upload: sha256"):
            tool.Loader(device).upload(payload(100), "fonts/cjk.ttf")

    def test_rejects_oversize_before_sending(self):
        device = FakeDevice()
        with mock.patch.object(tool, "MAX_FILE_BYTES", 10):
            with self.assertRaisesRegex(tool.LoaderError, "exceeds"):
                tool.Loader(device).upload(b"x" * 11, "fonts/cjk.ttf")
        self.assertEqual(device.input, bytearray())


class QueryTest(unittest.TestCase):
    def test_found(self):
        data = payload(5000)
        device = FakeDevice({"fonts/cjk.ttf": data})
        remote = tool.Loader(device).query("fonts/cjk.ttf")
        self.assertEqual(remote, tool.RemoteFile("fonts/cjk.ttf", 5000,
                                                 hashlib.sha256(data).hexdigest()))

    def test_not_found(self):
        self.assertIsNone(tool.Loader(FakeDevice()).query("fonts/cjk.ttf"))


class MainTest(unittest.TestCase):
    def run_main(self, device, argv):
        stdout = io.StringIO()
        with mock.patch.object(tool, "open_port", return_value=device), \
                mock.patch("sys.stdout", stdout), mock.patch("sys.stderr", io.StringIO()):
            device.close = lambda: None
            code = tool.main(argv)
        return code, stdout.getvalue()

    def test_skip_identical(self):
        data = payload(100)
        device = FakeDevice({"fonts/cjk.ttf": data})
        with mock.patch.object(Path, "read_bytes", return_value=data):
            code, out = self.run_main(device, ["--skip-identical", "local.ttf", "fonts/cjk.ttf"])
        self.assertEqual(code, 0)
        self.assertIn("already on the card", out)
        self.assertEqual(device.frames_seen, [])

    def test_uploads_when_different(self):
        data = payload(100)
        device = FakeDevice({"fonts/cjk.ttf": b"old"})
        with mock.patch.object(Path, "read_bytes", return_value=data):
            code, out = self.run_main(device, ["--skip-identical", "local.ttf", "fonts/cjk.ttf"])
        self.assertEqual(code, 0)
        self.assertIn("device verified SHA-256", out)
        self.assertEqual(device.files["fonts/cjk.ttf"], data)

    def test_query_not_found_exit_code(self):
        code, out = self.run_main(FakeDevice(), ["--query", "fonts/cjk.ttf"])
        self.assertEqual(code, 3)
        self.assertIn("not found", out)


if __name__ == "__main__":
    unittest.main()
