#!/usr/bin/env python3
"""Small dependency-free MonkeyMote/AIMP interoperability probe.

This utility deliberately implements only public wire behavior needed for
firmware validation. It never logs the configured password or authentication
digest.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

MAGIC = 0x71D9873B
HEADER = struct.Struct("<III")
MAX_PAYLOAD = 2 * 1024 * 1024


def load_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if path.exists():
        for raw in path.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    values.update({key: value for key, value in os.environ.items() if key.startswith("AIMP_")})
    return values


def packet(message_type: int, payload: bytes = b"") -> bytes:
    return HEADER.pack(MAGIC, message_type, len(payload)) + payload


@dataclass
class Frame:
    message_type: int
    payload: bytes


class Client:
    def __init__(self, host: str, port: int, password: str, timeout: float = 4.0):
        self.host = host
        self.port = port
        self.password = password
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self.buffer = bytearray()

    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port), self.timeout)
        self.sock.settimeout(self.timeout)
        hello = self.read_frame()
        if hello.message_type != 1 or len(hello.payload) != 11:
            raise RuntimeError(f"unexpected hello type={hello.message_type} bytes={len(hello.payload)}")
        requires_password = hello.payload[2] != 0
        challenge = hello.payload[3:11]
        digest = b"\0" * 16
        if requires_password:
            digest = hashlib.md5(self.password.encode("utf-8") + challenge).digest()
        self.send(2, digest + bytes((0x81, 0x05)))
        auth = self.read_frame()
        if auth.message_type != 3 or auth.payload != b"\x01":
            raise RuntimeError("authentication rejected")

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def send(self, message_type: int, payload: bytes = b"") -> None:
        if self.sock is None:
            raise RuntimeError("not connected")
        self.sock.sendall(packet(message_type, payload))

    def read_frame(self) -> Frame:
        if self.sock is None:
            raise RuntimeError("not connected")
        while len(self.buffer) < HEADER.size:
            self.buffer.extend(self.sock.recv(16384))
        magic, message_type, size = HEADER.unpack_from(self.buffer)
        if magic != MAGIC:
            raise RuntimeError(f"bad magic 0x{magic:08x}")
        if size > MAX_PAYLOAD:
            raise RuntimeError(f"payload too large: {size}")
        frame_size = HEADER.size + size
        while len(self.buffer) < frame_size:
            chunk = self.sock.recv(min(16384, frame_size - len(self.buffer)))
            if not chunk:
                raise RuntimeError("connection closed")
            self.buffer.extend(chunk)
        payload = bytes(self.buffer[HEADER.size:frame_size])
        del self.buffer[:frame_size]
        return Frame(message_type, payload)

    def collect(self, seconds: float = 0.8) -> list[Frame]:
        if self.sock is None:
            return []
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(0.15)
        frames: list[Frame] = []
        deadline = time.monotonic() + seconds
        try:
            while time.monotonic() < deadline:
                try:
                    frames.append(self.read_frame())
                except TimeoutError:
                    pass
        finally:
            self.sock.settimeout(old_timeout)
        return frames


def cstring(payload: bytes, offset: int) -> tuple[str, int]:
    end = payload.index(0, offset)
    return payload[offset:end].decode("utf-8", errors="replace"), end + 1


def lz4_decompress_block(source: bytes, expected_size: int) -> bytes:
    """Decode the raw LZ4 block used by compressed MonkeyMote frames."""
    output = bytearray()
    cursor = 0
    while cursor < len(source):
        token = source[cursor]
        cursor += 1
        literal_length = token >> 4
        if literal_length == 15:
            while True:
                if cursor >= len(source):
                    raise ValueError("truncated LZ4 literal length")
                extension = source[cursor]
                cursor += 1
                literal_length += extension
                if extension != 255:
                    break
        if cursor + literal_length > len(source):
            raise ValueError("truncated LZ4 literals")
        output.extend(source[cursor : cursor + literal_length])
        cursor += literal_length
        if cursor == len(source):
            break
        if cursor + 2 > len(source):
            raise ValueError("truncated LZ4 offset")
        match_offset = struct.unpack_from("<H", source, cursor)[0]
        cursor += 2
        if match_offset == 0 or match_offset > len(output):
            raise ValueError("invalid LZ4 offset")
        match_length = token & 0x0F
        if match_length == 15:
            while True:
                if cursor >= len(source):
                    raise ValueError("truncated LZ4 match length")
                extension = source[cursor]
                cursor += 1
                match_length += extension
                if extension != 255:
                    break
        match_length += 4
        for _ in range(match_length):
            output.append(output[-match_offset])
        if len(output) > expected_size:
            raise ValueError("LZ4 output exceeds advertised size")
    if len(output) != expected_size:
        raise ValueError(f"LZ4 size mismatch: {len(output)} != {expected_size}")
    return bytes(output)


def expand_frame(frame: Frame) -> Frame:
    if frame.message_type != 45 or len(frame.payload) < 10:
        return frame
    size, inner_type = struct.unpack_from("<II", frame.payload)
    return Frame(inner_type, lz4_decompress_block(frame.payload[10:], size))


def playback_state(frames: list[Frame]) -> tuple[int, bool, int, int] | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type == 6 and len(frame.payload) >= 8:
            p = frame.payload
            volume = p[7] * 257 if len(p) < 10 else struct.unpack_from("<H", p, 8)[0]
            return p[4], bool(p[5]), p[6], volume
    return None


def parse_playlists(frames: list[Frame]) -> list[tuple[int, str, bool, bool]]:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type != 41 or len(frame.payload) < 12:
            continue
        payload = frame.payload
        count = struct.unpack_from("<I", payload)[0]
        offset = 12
        result: list[tuple[int, str, bool, bool]] = []
        for _ in range(count):
            if offset + 12 > len(payload):
                raise ValueError("truncated playlist collection")
            playlist_id, _track_count = struct.unpack_from("<ii", payload, offset)
            flags = struct.unpack_from("<H", payload, offset + 8)[0]
            offset += 12
            name, offset = cstring(payload, offset)
            result.append((playlist_id, name, bool(flags & 1), bool(flags & 2)))
        return result
    return []


def current_playlist_id(frames: list[Frame]) -> int | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type in (9, 31, 50) and len(frame.payload) >= 8:
            return struct.unpack_from("<i", frame.payload, 4)[0]
    return None


def playback_command(client: Client, subcommand: int, value: int) -> None:
    client.send(7, struct.pack("<Bi", subcommand, value))


def exercise_and_restore(client: Client, initial: list[Frame]) -> list[str]:
    """Exercise reversible live controls and restore their observed values."""
    state = playback_state(initial)
    if state is None:
        raise RuntimeError("AIMP did not publish playback state")
    playback, shuffle, repeat, volume = state
    notes = [f"baseline playback={playback} shuffle={shuffle} repeat={repeat} volume={volume}"]

    changed_volume = volume - 1024 if volume > 32767 else volume + 1024
    try:
        client.send(7, struct.pack("<BHH", 6, changed_volume // 256, changed_volume))
        changed = playback_state(client.collect(0.7))
        if changed is None or abs(changed[3] - changed_volume) > 256:
            raise RuntimeError("volume change was not confirmed")
        notes.append(f"volume confirmed at {changed[3]}")

        playback_command(client, 4, int(not shuffle))
        changed = playback_state(client.collect(0.7))
        if changed is None or changed[1] == shuffle:
            raise RuntimeError("shuffle change was not confirmed")
        notes.append(f"shuffle confirmed at {changed[1]}")

        changed_repeat = (repeat + 1) % 3
        playback_command(client, 5, changed_repeat)
        changed = playback_state(client.collect(0.7))
        if changed is None or changed[2] != changed_repeat:
            raise RuntimeError("repeat change was not confirmed")
        notes.append(f"repeat confirmed at {changed[2]}")

    finally:
        client.send(7, struct.pack("<BHH", 6, volume // 256, volume))
        playback_command(client, 4, int(shuffle))
        playback_command(client, 5, repeat)
        restored = playback_state(client.collect(1.0))
        if restored is not None and restored[0] != playback and playback in (1, 2, 3):
            playback_command(client, playback, 0)
            restored = playback_state(client.collect(0.7))
        if restored is not None:
            notes.append(
                f"restored playback={restored[0]} shuffle={restored[1]} "
                f"repeat={restored[2]} volume={restored[3]}"
            )
    return notes


def describe(frame: Frame) -> str:
    frame = expand_frame(frame)
    p = frame.payload
    if frame.message_type == 5 and len(p) >= 35:
        track, duration = struct.unpack_from("<ii", p)
        bitrate = struct.unpack_from("<H", p, 8)[0]
        rating = p[11]
        year = struct.unpack_from("<H", p, 28)[0]
        lengths = p[31:35]
        offset = 35
        fields = []
        for length in lengths:
            fields.append(p[offset : offset + length].decode("utf-8", errors="replace"))
            offset += length + 1
        return (
            f"track-info index={track} duration_ms={duration} bitrate={bitrate} "
            f"rating={rating} year={year} title={fields[0]!r} artist={fields[1]!r}"
        )
    if frame.message_type == 6 and len(p) >= 8:
        position = struct.unpack_from("<i", p)[0]
        state = p[4]
        shuffle = bool(p[5])
        repeat = p[6]
        volume = p[7] if len(p) == 8 else struct.unpack_from("<H", p, 8)[0]
        return f"playback position_ms={position} state={state} shuffle={shuffle} repeat={repeat} volume={volume}"
    if frame.message_type == 28 and len(p) == 8:
        return f"capabilities=0x{struct.unpack('<Q', p)[0]:016x}"
    if frame.message_type == 41 and len(p) >= 12:
        count = struct.unpack_from("<I", p)[0]
        return f"playlists count={count} raw={p.hex()}"
    if frame.message_type in (9, 31, 50):
        count = struct.unpack_from("<I", p)[0] if len(p) >= 4 else -1
        return f"playlist type={frame.message_type} count={count} bytes={len(p)}"
    if frame.message_type == 23:
        return "keepalive"
    return f"type={frame.message_type} bytes={len(p)} payload={p.hex()}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", type=Path, default=Path(".env"))
    parser.add_argument(
        "command", choices=("state", "playlists", "playlist", "ping", "exercise")
    )
    parser.add_argument("--playlist-id", type=int, default=-1)
    args = parser.parse_args()
    env = load_env(args.env)
    client = Client(
        env.get("AIMP_HOST", "aerocore.local"),
        int(env.get("AIMP_PORT", "19371")),
        env.get("AIMP_PASSWORD", ""),
    )
    try:
        client.connect()
        initial = client.collect(1.0)
        if args.command == "exercise":
            for note in exercise_and_restore(client, initial):
                print(note)
            return 0
        if args.command == "playlists":
            client.send(4, struct.pack("<Bi", 13, -1))
        elif args.command == "playlist":
            client.send(4, struct.pack("<Bii", 10, args.playlist_id, 1))
        elif args.command == "ping":
            client.send(23)
        frames = initial + client.collect(1.2)
        for frame in frames:
            print(describe(frame))
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"probe failed: {exc}", file=sys.stderr)
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
