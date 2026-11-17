#!/usr/bin/env python3
"""Upload a data file to the device's microSD card over the USB console.

The firmware's USB microSD loader (main/sd_loader.cpp, issue #49) accepts the
framed protocol described in main/sd_loader_policy.hpp. This tool opens the
port without resetting the board, skips interleaved log lines, retries chunks
the device rejects, and reports the device's SHA-256 verification.

    tools/load_sd_data.py --port /dev/ttyACM0 build/data/fonts/cjk.ttf fonts/cjk.ttf
    tools/load_sd_data.py --port /dev/ttyACM0 --query fonts/cjk.ttf

Close `idf.py monitor` first; two readers would split the device's output.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

PREFIX = b"@@LOAD"
MAX_FILE_BYTES = 16 * 1024 * 1024
CHUNK_BYTES = 4096
CHUNK_ATTEMPTS = 6
BEGIN_ATTEMPTS = 3
# Longer than the device's 1 s payload timeout, so a frame the device gave up
# on has been NAKed or discarded before it is resent.
ACK_TIMEOUT = 3.0
BEGIN_TIMEOUT = 5.0
# END fsyncs and renames; QUERY hashes up to 16 MiB on the device.
END_TIMEOUT = 30.0
QUERY_TIMEOUT = 60.0
# Bytes of log text kept while waiting for a newline before it is dropped.
MAX_PENDING_BYTES = 64 * 1024
# Device NAK reasons after which resending the same chunk can succeed.
RETRYABLE = {"crc", "timeout"}


class LoaderError(RuntimeError):
    pass


@dataclass(frozen=True)
class Reply:
    kind: str  # ACK, NAK, OK, or INFO
    fields: tuple[str, ...]

    @property
    def subject(self) -> str:
        return self.fields[0] if self.fields else ""

    @property
    def reason(self) -> str:
        return self.fields[1] if len(self.fields) > 1 else ""


def parse_reply(line: bytes) -> Optional[Reply]:
    """Finds a loader reply anywhere in a line of console output.

    The device starts each reply on a fresh line, but a reply written while a
    log line is half-sent can still follow that line's text, so the token is
    searched for rather than required at column 0.
    """
    index = line.find(PREFIX)
    if index < 0:
        return None
    tokens = line[index:].decode("ascii", "replace").split()
    kinds = {"@@LOADACK": "ACK", "@@LOADNAK": "NAK", "@@LOADOK": "OK", "@@LOADINFO": "INFO"}
    if not tokens or tokens[0] not in kinds:
        return None
    return Reply(kinds[tokens[0]], tuple(tokens[1:]))


class ReplyReader:
    """Splits the port's output into lines and returns loader replies.

    Every other line is device log output, passed to `on_log`.
    """

    def __init__(self, port, on_log: Callable[[str], None] = lambda _line: None) -> None:
        self.port = port
        self.on_log = on_log
        self.pending = bytearray()

    def next_reply(self, timeout: float) -> Optional[Reply]:
        deadline = time.monotonic() + timeout
        while True:
            newline = self.pending.find(b"\n")
            while newline >= 0:
                line = bytes(self.pending[:newline]).rstrip(b"\r")
                del self.pending[: newline + 1]
                reply = parse_reply(line)
                if reply is not None:
                    return reply
                if line:
                    self.on_log(line.decode("utf-8", "replace"))
                newline = self.pending.find(b"\n")
            if len(self.pending) > MAX_PENDING_BYTES:
                self.pending.clear()
            if time.monotonic() >= deadline:
                return None
            data = self.port.read(max(1, getattr(self.port, "in_waiting", 0) or 0))
            if data:
                self.pending.extend(data)


def data_frame(sequence: int, payload: bytes) -> bytes:
    header = b"\n@@LOAD DATA %d %d %08x\n" % (sequence, len(payload), zlib.crc32(payload))
    return header + payload


def command(text: str) -> bytes:
    # The leading newline ends any partial line left on the device by an
    # interrupted earlier run.
    return b"\n" + text.encode("ascii") + b"\n"


@dataclass(frozen=True)
class RemoteFile:
    relpath: str
    size: int
    sha256: str


class Loader:
    def __init__(self, port, on_log: Callable[[str], None] = lambda _line: None,
                 on_progress: Callable[[int, int], None] = lambda _sent, _total: None,
                 ack_timeout: float = ACK_TIMEOUT) -> None:
        self.port = port
        self.reader = ReplyReader(port, on_log)
        self.on_progress = on_progress
        self.ack_timeout = ack_timeout
        self.retries = 0

    def _send(self, data: bytes) -> None:
        self.port.write(data)
        self.port.flush()

    def query(self, relpath: str) -> Optional[RemoteFile]:
        self._send(command(f"@@LOAD QUERY {relpath}"))
        deadline = time.monotonic() + QUERY_TIMEOUT
        while time.monotonic() < deadline:
            reply = self.reader.next_reply(deadline - time.monotonic())
            if reply is None:
                break
            if reply.kind == "INFO" and len(reply.fields) == 3 and reply.fields[0] == relpath:
                return RemoteFile(relpath, int(reply.fields[1]), reply.fields[2])
            if reply.kind == "NAK" and reply.subject in ("QUERY", "-"):
                if reply.reason == "notfound":
                    return None
                raise LoaderError(f"device refused query: {reply.reason}")
        raise LoaderError("no reply to QUERY; is the loader firmware running?")

    def upload(self, data: bytes, relpath: str) -> RemoteFile:
        if len(data) > MAX_FILE_BYTES:
            raise LoaderError(f"{len(data)} bytes exceeds the {MAX_FILE_BYTES}-byte limit")
        digest = hashlib.sha256(data).hexdigest()
        try:
            self._begin(relpath, len(data), digest)
            sent = 0
            self.on_progress(0, len(data))
            for sequence, offset in enumerate(range(0, len(data), CHUNK_BYTES)):
                payload = data[offset : offset + CHUNK_BYTES]
                self._send_chunk(sequence, payload)
                sent += len(payload)
                self.on_progress(sent, len(data))
            result = self._end(relpath)
        except BaseException:
            # Best effort: the device also discards an upload idle for 5 s.
            try:
                self._send(command("@@LOAD ABORT"))
            except Exception:
                pass
            raise
        if result.size != len(data) or result.sha256 != digest:
            raise LoaderError(f"device stored {result.size} bytes with SHA-256 {result.sha256}, "
                              f"expected {len(data)} bytes with {digest}")
        return result

    def _begin(self, relpath: str, size: int, digest: str) -> None:
        for _attempt in range(BEGIN_ATTEMPTS):
            self._send(command(f"@@LOAD BEGIN {relpath} {size} {digest}"))
            deadline = time.monotonic() + BEGIN_TIMEOUT
            while time.monotonic() < deadline:
                reply = self.reader.next_reply(deadline - time.monotonic())
                if reply is None:
                    break
                if reply.kind == "ACK" and reply.fields == ("BEGIN", relpath):
                    return
                if reply.kind == "NAK" and reply.subject in ("BEGIN", "-"):
                    raise LoaderError(f"device refused BEGIN: {reply.reason}")
                # Anything else is stale output from an earlier run.
        raise LoaderError("no reply to BEGIN; is the loader firmware running?")

    def _send_chunk(self, sequence: int, payload: bytes) -> None:
        frame = data_frame(sequence, payload)
        wanted = str(sequence)
        last_problem = "no reply"
        for attempt in range(CHUNK_ATTEMPTS):
            if attempt:
                self.retries += 1
            self._send(frame)
            reply = self._chunk_reply(wanted)
            if reply is None:
                last_problem = "no reply"
                continue
            if reply.kind == "ACK":
                return
            if reply.reason not in RETRYABLE:
                raise LoaderError(f"device rejected chunk {sequence}: {reply.reason}")
            last_problem = reply.reason
        raise LoaderError(f"chunk {sequence} failed after {CHUNK_ATTEMPTS} attempts "
                          f"({last_problem})")

    def _chunk_reply(self, wanted: str) -> Optional[Reply]:
        deadline = time.monotonic() + self.ack_timeout
        while time.monotonic() < deadline:
            reply = self.reader.next_reply(deadline - time.monotonic())
            if reply is None:
                return None
            if reply.kind in ("ACK", "NAK") and reply.subject == wanted:
                return reply
            # Otherwise a late reply to an earlier attempt or chunk.
        return None

    def _end(self, relpath: str) -> RemoteFile:
        self._send(command("@@LOAD END"))
        deadline = time.monotonic() + END_TIMEOUT
        while time.monotonic() < deadline:
            reply = self.reader.next_reply(deadline - time.monotonic())
            if reply is None:
                break
            if reply.kind == "OK" and len(reply.fields) == 3 and reply.fields[0] == relpath:
                return RemoteFile(relpath, int(reply.fields[1]), reply.fields[2])
            if reply.kind == "NAK" and reply.subject in ("END", "-"):
                raise LoaderError(f"device discarded the upload: {reply.reason}")
        raise LoaderError("no reply to END")


def open_port(name: str):
    import serial  # pyserial; imported here so tests run without it

    port = serial.Serial()
    port.port = name
    port.baudrate = 115200  # ignored by USB-Serial-JTAG
    port.timeout = 0.05
    port.write_timeout = 10
    # Keep DTR and RTS deasserted when the port opens; the USB-Serial-JTAG
    # reset logic would otherwise restart the board, as `idf.py monitor
    # --no-reset` also avoids.
    port.dtr = False
    port.rts = False
    port.open()
    port.reset_input_buffer()
    return port


class Progress:
    def __init__(self, stream=None) -> None:
        self.stream = stream if stream is not None else sys.stderr
        self.started = time.monotonic()
        self.last = 0.0
        self.tty = hasattr(stream, "isatty") and stream.isatty()
        self.last_decile = -1

    def __call__(self, sent: int, total: int) -> None:
        now = time.monotonic()
        elapsed = max(now - self.started, 1e-6)
        rate = sent / elapsed / 1024
        percent = 100 if total == 0 else sent * 100 // total
        if self.tty:
            if sent == total or now - self.last >= 0.2:
                self.last = now
                self.stream.write(f"\r  {sent}/{total} bytes ({percent}%) {rate:.0f} KB/s ")
                if sent == total:
                    self.stream.write("\n")
                self.stream.flush()
        elif percent // 10 != self.last_decile:
            self.last_decile = percent // 10
            self.stream.write(f"  {sent}/{total} bytes ({percent}%) {rate:.0f} KB/s\n")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--query", metavar="RELPATH",
                        help="print the size and SHA-256 of a file on the card")
    parser.add_argument("--skip-identical", action="store_true",
                        help="query first and skip the upload if the card already has the file")
    parser.add_argument("--verbose", action="store_true", help="echo device log lines")
    parser.add_argument("local_file", nargs="?", type=Path)
    parser.add_argument("relpath", nargs="?", help="destination under /sdcard, e.g. fonts/cjk.ttf")
    args = parser.parse_args(argv)

    if args.query is None and (args.local_file is None or args.relpath is None):
        parser.error("give <local-file> <device-relpath>, or --query <relpath>")
    if args.query is not None and args.local_file is not None:
        parser.error("--query takes no file arguments")

    on_log = (lambda line: print(f"  device: {line}", file=sys.stderr)) if args.verbose else (
        lambda _line: None)
    try:
        port = open_port(args.port)
    except Exception as error:  # serial.SerialException, OSError, ImportError
        print(f"error: cannot open {args.port}: {error}", file=sys.stderr)
        return 1
    try:
        if args.query is not None:
            remote = Loader(port, on_log).query(args.query)
            if remote is None:
                print(f"{args.query}: not found")
                return 3
            print(f"{remote.relpath}: {remote.size} bytes, SHA-256 {remote.sha256}")
            return 0

        data = args.local_file.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if args.skip_identical:
            remote = Loader(port, on_log).query(args.relpath)
            if remote is not None and remote.size == len(data) and remote.sha256 == digest:
                print(f"{args.relpath}: already on the card ({len(data)} bytes, SHA-256 "
                      f"{digest}); skipped")
                return 0
        print(f"Uploading {args.local_file} to /sdcard/{args.relpath} ({len(data)} bytes)",
              file=sys.stderr)
        loader = Loader(port, on_log, Progress())
        started = time.monotonic()
        result = loader.upload(data, args.relpath)
        elapsed = max(time.monotonic() - started, 1e-6)
        print(f"{result.relpath}: {result.size} bytes in {elapsed:.1f} s "
              f"({result.size / elapsed / 1024:.0f} KB/s, {loader.retries} retries); "
              f"device verified SHA-256 {result.sha256}")
        return 0
    except LoaderError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted; the device discarded the partial upload", file=sys.stderr)
        return 130
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
