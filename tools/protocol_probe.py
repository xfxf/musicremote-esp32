#!/usr/bin/env python3
"""Small dependency-free MonkeyMote/AIMP interoperability probe.

This utility deliberately implements only public wire behavior needed for
firmware validation. It never logs the configured password or authentication
digest.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, TypeVar

MAGIC = 0x71D9873B
HEADER = struct.Struct("<III")
MAX_PAYLOAD = 2 * 1024 * 1024
MAX_DECODED_PAYLOAD = 32 * 1024 * 1024
COMPUTER_POWER_CAPABILITY = 1 << 17
EQUALIZER_CAPABILITY = 1 << 21
SLEEP_TIMER_CAPABILITY = 1 << 22
WaitValue = TypeVar("WaitValue")


def load_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if path.exists():
        for raw in path.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
                value = value[1:-1]
            values[key.strip()] = value
    values.update({key: value for key, value in os.environ.items() if key.startswith("AIMP_")})
    return values


def packet(message_type: int, payload: bytes = b"") -> bytes:
    return HEADER.pack(MAGIC, message_type, len(payload)) + payload


@dataclass
class Frame:
    message_type: int
    payload: bytes


@dataclass(frozen=True)
class PlaybackSnapshot:
    position_ms: int
    state: int
    shuffle: bool
    repeat: int
    volume: int


@dataclass(frozen=True)
class TrackInfo:
    index: int
    duration_ms: int
    rating: int
    title: str
    artist: str
    album: str


@dataclass(frozen=True)
class CoverArt:
    identity: bytes
    jpeg: bytes


@dataclass(frozen=True)
class LibraryRecord:
    title: str
    subtitle: str = ""
    duration_ms: int = 0
    year: int = 0
    album: str = ""


@dataclass(frozen=True)
class LibraryResult:
    request_id: int
    total_count: int
    kind: int
    records: tuple[LibraryRecord, ...]


@dataclass(frozen=True)
class LyricsResult:
    identity: bytes
    text: str


@dataclass(frozen=True)
class EqualizerBand:
    frequency_hz: int
    gain_tenths_offset: int


@dataclass(frozen=True)
class EqualizerState:
    enabled: bool
    preamp_tenths_offset: int
    bands: tuple[EqualizerBand, ...]
    payload: bytes


@dataclass(frozen=True)
class SleepTimerState:
    enabled: bool
    trigger: int
    action: int
    seconds_remaining: int
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
            chunk = self.sock.recv(16384)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buffer.extend(chunk)
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
                    frame = self.read_frame()
                    frames.append(frame)
                    if frame.message_type == 23:
                        self.send(23)
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
        if len(output) + match_length > expected_size:
            raise ValueError("LZ4 output exceeds advertised size")
        for _ in range(match_length):
            output.append(output[-match_offset])
    if len(output) != expected_size:
        raise ValueError(f"LZ4 size mismatch: {len(output)} != {expected_size}")
    return bytes(output)


def expand_frame(frame: Frame) -> Frame:
    if frame.message_type != 45 or len(frame.payload) < 10:
        return frame
    size, inner_type = struct.unpack_from("<II", frame.payload)
    if inner_type == 45:
        raise ValueError("nested compressed frame")
    if size > MAX_DECODED_PAYLOAD:
        raise ValueError(f"decoded payload too large: {size}")
    return Frame(inner_type, lz4_decompress_block(frame.payload[10:], size))


def parse_cover_frame(frame: Frame) -> CoverArt | None:
    frame = expand_frame(frame)
    if frame.message_type != 8 or len(frame.payload) < 24:
        return None
    jpeg_size = struct.unpack_from("<I", frame.payload)[0]
    jpeg = frame.payload[20:]
    if (
        jpeg_size != len(jpeg)
        or jpeg_size > 256 * 1024
        or not jpeg.startswith(b"\xff\xd8")
        or not jpeg.endswith(b"\xff\xd9")
    ):
        raise ValueError("invalid cover-art response")
    return CoverArt(frame.payload[4:20], jpeg)


def _library_field(payload: bytes, offset: int, length: int, end: int) -> tuple[str, int]:
    if length > 1024 or offset + length + 1 > end or end > len(payload):
        raise ValueError("invalid library string length")
    if payload[offset + length] != 0:
        raise ValueError("unterminated library string")
    value = payload[offset : offset + length].decode("utf-8", errors="replace")
    return value, offset + length + 1


def parse_library_frame(frame: Frame, retain: int = 10_000) -> LibraryResult | None:
    frame = expand_frame(frame)
    if frame.message_type != 19:
        return None
    payload = frame.payload
    if len(payload) < 9:
        raise ValueError("truncated library response")
    request_id, count = struct.unpack_from("<II", payload)
    kind = payload[8]
    if count > 100_000 or kind not in (0, 2, 4, 6, 7, 8):
        raise ValueError("invalid library response header")
    offset = 9
    records: list[LibraryRecord] = []
    for _ in range(count):
        if kind in (0, 2, 4):
            try:
                value, offset = cstring(payload, offset)
            except (ValueError, IndexError) as exc:
                raise ValueError("invalid library node") from exc
            if len(value.encode("utf-8")) > 1024:
                raise ValueError("library node is too long")
            if len(records) < retain:
                records.append(LibraryRecord(value))
            continue
        if kind == 6:
            if offset + 12 > len(payload):
                raise ValueError("truncated library album")
            album_length, artist_length, year, _reserved, record_size = struct.unpack_from(
                "<HHHHI", payload, offset
            )
            offset += 12
            record_end = offset + record_size
            if record_end > len(payload):
                raise ValueError("invalid library album size")
            album, offset = _library_field(payload, offset, album_length, record_end)
            artist, offset = _library_field(payload, offset, artist_length, record_end)
            offset = record_end
            if len(records) < retain:
                records.append(LibraryRecord(album, artist, year=year))
            continue
        if offset + 20 > len(payload):
            raise ValueError("truncated library track")
        (
            artist_length,
            title_length,
            album_length,
            year,
            record_size,
            duration_ms,
            _bitrate,
            _rating,
            _flags,
        ) = struct.unpack_from("<HHHHIiHBB", payload, offset)
        offset += 20
        record_end = offset + record_size
        if record_end > len(payload):
            raise ValueError("invalid library track size")
        artist, offset = _library_field(payload, offset, artist_length, record_end)
        title, offset = _library_field(payload, offset, title_length, record_end)
        album, offset = _library_field(payload, offset, album_length, record_end)
        offset = record_end
        if len(records) < retain:
            records.append(
                LibraryRecord(title, artist or album, duration_ms, year, album)
            )
    if offset != len(payload):
        raise ValueError("library response has trailing data")
    return LibraryResult(request_id, count, kind, tuple(records))


def parse_lyrics_frame(frame: Frame, max_text: int = 24 * 1024) -> LyricsResult | None:
    frame = expand_frame(frame)
    if frame.message_type != 36:
        return None
    payload = frame.payload
    if len(payload) < 20:
        raise ValueError("truncated lyrics response")
    length = struct.unpack_from("<I", payload)[0]
    if length != len(payload) - 20:
        raise ValueError("invalid lyrics response length")
    if length > max_text:
        raise ValueError("lyrics response exceeds display limit")
    return LyricsResult(payload[4:20], payload[20:].decode("utf-8", errors="replace"))


def parse_equalizer_frame(frame: Frame) -> EqualizerState | None:
    frame = expand_frame(frame)
    if frame.message_type != 48:
        return None
    payload = frame.payload
    if len(payload) < 10:
        raise ValueError("truncated equalizer response")
    count, enabled, upper, lower, span, preamp, reserved = struct.unpack_from(
        "<BBBBHHH", payload
    )
    if (
        count > 32
        or len(payload) != 10 + count * 4
        or enabled > 1
        or upper != 15
        or lower != 0
        or span != 300
        or preamp > 300
        or reserved != 0
    ):
        raise ValueError("invalid equalizer response")
    bands: list[EqualizerBand] = []
    for index in range(count):
        frequency_hz, gain = struct.unpack_from("<HH", payload, 10 + index * 4)
        if frequency_hz == 0 or gain > 300:
            raise ValueError("invalid equalizer band")
        bands.append(EqualizerBand(frequency_hz, gain))
    return EqualizerState(bool(enabled), preamp, tuple(bands), payload)


def parse_equalizer_presets_frame(frame: Frame) -> tuple[str, ...] | None:
    frame = expand_frame(frame)
    if frame.message_type != 49:
        return None
    payload = frame.payload
    if len(payload) < 4:
        raise ValueError("truncated equalizer presets response")
    count, reserved = struct.unpack_from("<HH", payload)
    if count > 128 or reserved != 0:
        raise ValueError("invalid equalizer presets response")
    offset = 4
    presets: list[str] = []
    for _ in range(count):
        try:
            end = payload.index(0, offset)
        except ValueError as exc:
            raise ValueError("unterminated equalizer preset") from exc
        if end - offset > 255:
            raise ValueError("equalizer preset is too long")
        presets.append(payload[offset:end].decode("utf-8", errors="replace"))
        offset = end + 1
    if offset != len(payload):
        raise ValueError("equalizer presets response has trailing data")
    return tuple(presets)


def parse_sleep_timer_frame(frame: Frame) -> SleepTimerState | None:
    frame = expand_frame(frame)
    if frame.message_type != 51:
        return None
    payload = frame.payload
    if (
        len(payload) != 8
        or payload[1] not in (1, 2)
        or payload[2] not in (1, 2, 3)
    ):
        raise ValueError("invalid sleep timer response")
    return SleepTimerState(
        bool(payload[0]),
        payload[1],
        payload[2],
        struct.unpack_from("<I", payload, 4)[0],
        payload,
    )


def latest_capabilities(frames: list[Frame]) -> int | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type == 28 and len(frame.payload) == 8:
            return struct.unpack("<Q", frame.payload)[0]
    return None


def send_advanced_state_requests(client: Client, capabilities: int) -> None:
    if capabilities & EQUALIZER_CAPABILITY:
        client.send(4, struct.pack("<Bi", 17, 0))
        client.send(4, struct.pack("<Bi", 18, 0))
    if capabilities & SLEEP_TIMER_CAPABILITY:
        client.send(4, struct.pack("<Bi", 19, 0))


def request_advanced_state(client: Client, capabilities: int) -> list[Frame]:
    send_advanced_state_requests(client, capabilities)
    return client.collect(1.5)


def latest_equalizer(frames: list[Frame]) -> EqualizerState | None:
    for frame in reversed(frames):
        parsed = parse_equalizer_frame(frame)
        if parsed is not None:
            return parsed
    return None


def latest_sleep_timer(frames: list[Frame]) -> SleepTimerState | None:
    for frame in reversed(frames):
        parsed = parse_sleep_timer_frame(frame)
        if parsed is not None:
            return parsed
    return None


def latest_playback(frames: list[Frame]) -> PlaybackSnapshot | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type == 6 and len(frame.payload) >= 8:
            p = frame.payload
            volume = p[7] * 257 if len(p) < 10 else struct.unpack_from("<H", p, 8)[0]
            return PlaybackSnapshot(
                struct.unpack_from("<i", p)[0], p[4], bool(p[5]), p[6], volume
            )
    return None


def playback_state(frames: list[Frame]) -> tuple[int, bool, int, int] | None:
    state = latest_playback(frames)
    if state is None:
        return None
    return state.state, state.shuffle, state.repeat, state.volume


def latest_track_info(frames: list[Frame]) -> TrackInfo | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        p = frame.payload
        if frame.message_type != 5 or len(p) < 35:
            continue
        lengths = p[31:35]
        offset = 35
        fields: list[str] = []
        valid = True
        for length in lengths:
            if offset + length >= len(p):
                valid = False
                break
            fields.append(p[offset : offset + length].decode("utf-8", errors="replace"))
            offset += length + 1
        if valid:
            return TrackInfo(
                struct.unpack_from("<i", p)[0],
                struct.unpack_from("<i", p, 4)[0],
                p[11],
                fields[0],
                fields[1],
                fields[2],
            )
    return None


def latest_queue_index(frames: list[Frame]) -> int | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type == 11 and len(frame.payload) >= 4:
            return struct.unpack_from("<i", frame.payload)[0]
    return None


def parse_playlists(frames: list[Frame]) -> list[tuple[int, str, int, bool, bool]]:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type != 41 or len(frame.payload) < 12:
            continue
        payload = frame.payload
        count = struct.unpack_from("<I", payload)[0]
        offset = 12
        result: list[tuple[int, str, int, bool, bool]] = []
        for _ in range(count):
            if offset + 12 > len(payload):
                raise ValueError("truncated playlist collection")
            playlist_id, track_count = struct.unpack_from("<ii", payload, offset)
            flags = struct.unpack_from("<H", payload, offset + 8)[0]
            offset += 12
            name, offset = cstring(payload, offset)
            result.append((playlist_id, name, track_count, bool(flags & 1), bool(flags & 2)))
        return result
    return []


def playlist_identity(frames: list[Frame]) -> tuple[int, str, int] | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type not in (9, 31, 50) or len(frame.payload) < 11:
            continue
        count = struct.unpack_from("<I", frame.payload)[0]
        playlist_id = struct.unpack_from("<i", frame.payload, 4)[0]
        name_length = struct.unpack_from("<H", frame.payload, 8)[0]
        if 11 + name_length + 1 > len(frame.payload):
            continue
        name = frame.payload[11 : 11 + name_length].decode("utf-8", errors="replace")
        return playlist_id, name, count
    return None


def resolved_current_playlist_id(frames: list[Frame]) -> int | None:
    """Resolve AIMP's -1 current-playlist sentinel against the root listing."""
    identity = playlist_identity(frames)
    if identity is None:
        return None
    playlist_id, name, count = identity
    if playlist_id != -1:
        return playlist_id
    matches = [
        candidate_id
        for candidate_id, candidate_name, candidate_count, folder, _ in parse_playlists(
            frames
        )
        if not folder and candidate_name == name and candidate_count == count
    ]
    return matches[0] if len(matches) == 1 else None


def playback_command(client: Client, subcommand: int, value: int) -> None:
    client.send(7, struct.pack("<Bi", subcommand, value))


def request_playlists(client: Client, folder_id: int = -1) -> list[Frame]:
    client.send(4, struct.pack("<Bi", 13, folder_id))
    return client.collect(1.0)


def request_playlist(client: Client, playlist_id: int = -1) -> list[Frame]:
    client.send(4, struct.pack("<Bii", 10, playlist_id, 1))
    return client.collect(1.2)


def request_current_and_root(client: Client) -> None:
    client.send(4, struct.pack("<Bii", 10, -1, 1))
    client.send(4, struct.pack("<Bi", 13, -1))


def playlist_count(frames: list[Frame], playlist_id: int) -> int | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type not in (9, 31, 50) or len(frame.payload) < 8:
            continue
        frame_id = struct.unpack_from("<i", frame.payload, 4)[0]
        if playlist_id == -1 or frame_id in (playlist_id, -1):
            return struct.unpack_from("<I", frame.payload)[0]
    return None


def playlist_first_rating(frames: list[Frame], playlist_id: int) -> int | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        p = frame.payload
        if frame.message_type not in (9, 31, 50) or len(p) < 12:
            continue
        count = struct.unpack_from("<I", p)[0]
        frame_id = struct.unpack_from("<i", p, 4)[0]
        name_length = struct.unpack_from("<H", p, 8)[0]
        record_offset = 11 + name_length + 1
        if (
            count > 0
            and frame_id in (playlist_id, -1)
            and record_offset + 12 <= len(p)
        ):
            return p[record_offset + 11]
    return None


def command_result(frames: list[Frame]) -> int | None:
    for raw in reversed(frames):
        frame = expand_frame(raw)
        if frame.message_type == 43 and frame.payload:
            return frame.payload[0]
    return None


def packet_evidence(
    action: str, message_type: int, payload_size: int, frames: list[Frame]
) -> str:
    counts = Counter(
        (frame.message_type, len(frame.payload))
        for frame in frames
        if frame.message_type != 23
    )
    responses = ",".join(
        f"{message_type}/{size}x{count}"
        for (message_type, size), count in sorted(counts.items())
    )
    result = command_result(frames)
    suffix = "" if result is None else f" result={result}"
    return (
        f"packet {action}: request={message_type}/{payload_size} "
        f"responses=[{responses}]{suffix}"
    )


def wait_for(
    client: Client,
    predicate: Callable[[list[Frame]], WaitValue | None],
    *,
    timeout: float = 6.0,
    request: Callable[[], None] | None = None,
) -> tuple[WaitValue, list[Frame]]:
    frames: list[Frame] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if request is not None:
            request()
        frames.extend(client.collect(0.6))
        value = predicate(frames)
        if value is not None and value is not False:
            return value, frames
    raise RuntimeError("timed out waiting for AIMP confirmation")


def activate_and_wait(client: Client, playlist_id: int) -> list[Frame]:
    """Activate a playlist and wait for AIMP's asynchronous state change."""
    playback_command(client, 12, playlist_id)
    _, frames = wait_for(
        client,
        lambda observed: (
            playlist_id
            if resolved_current_playlist_id(observed) == playlist_id
            else None
        ),
        timeout=8.0,
        request=lambda: request_current_and_root(client),
    )
    return frames


def validate_mutations_and_restore(
    client: Client,
    initial: list[Frame],
    emit: Callable[[str], None] | None = None,
) -> list[str]:
    """Run the destructive matrix only against a uniquely named scratch playlist."""
    class ProgressNotes(list[str]):
        def append(self, note: str) -> None:
            super().append(note)
            if emit is not None:
                emit(note)

    notes: list[str] = ProgressNotes()
    baseline_frames = initial + request_playlist(client, -1) + request_playlists(client)
    baseline_playback = latest_playback(baseline_frames)
    baseline_playlist = resolved_current_playlist_id(baseline_frames)
    baseline_queue_index = latest_queue_index(baseline_frames)
    baseline_track = latest_track_info(baseline_frames)
    if baseline_playback is None or baseline_playlist is None:
        raise RuntimeError("AIMP did not publish a restorable baseline")
    notes.append(
        "baseline "
        f"playlist={baseline_playlist} row={baseline_queue_index} "
        f"position_ms={baseline_playback.position_ms} playback={baseline_playback.state} "
        f"shuffle={baseline_playback.shuffle} repeat={baseline_playback.repeat} "
        f"volume={baseline_playback.volume}"
    )

    scratch_name = f"ESP validation {int(time.time())}"
    scratch_id: int | None = None
    test_rating: int | None = None
    rating_restored = False
    try:
        name = scratch_name.encode("utf-8")[:120]
        create_payload = struct.pack("<iHI", -1, 0, len(name)) + name + b"\0"
        client.send(42, create_payload)
        create_frames = client.collect(1.0)
        notes.append(packet_evidence("create", 42, len(create_payload), create_frames))

        def find_scratch(frames: list[Frame]) -> int | None:
            for playlist_id, playlist_name, _, folder, _ in parse_playlists(frames):
                if not folder and playlist_name == scratch_name:
                    return playlist_id
            return None

        found, playlist_frames = wait_for(
            client,
            find_scratch,
            request=lambda: client.send(4, struct.pack("<Bi", 13, -1)),
        )
        scratch_id = int(found)
        notes.append(f"create confirmed scratch_id={scratch_id}")

        # Type 21 asks AIMP's media-library delegate for one matching item. With
        # all three optional filter flags clear, selection 0 yields one item;
        # this avoids touching or copying a user playlist.
        add_payload = struct.pack("<iiiii", scratch_id, 0, 0, 0, 0)
        client.send(21, add_payload)
        add_frames = client.collect(1.2)
        notes.append(packet_evidence("add scratch item", 21, len(add_payload), add_frames))
        count, count_frames = wait_for(
            client,
            lambda frames: 1 if playlist_count(frames, scratch_id) == 1 else None,
            timeout=10.0,
            request=lambda: client.send(4, struct.pack("<Bii", 10, scratch_id, 1)),
        )
        notes.append(f"scratch population confirmed count={count}")

        activate_and_wait(client, scratch_id)
        playback_command(client, 10, 0)
        playback_command(client, 1, 0)
        play_frames = client.collect(2.0)
        track, track_frames = wait_for(
            client,
            latest_track_info,
            timeout=8.0,
        ) if latest_track_info(play_frames) is None else (latest_track_info(play_frames), play_frames)
        assert isinstance(track, TrackInfo)
        test_rating = track.rating
        if track.duration_ms < 5000:
            raise RuntimeError("scratch item is too short for a meaningful seek test")
        notes.append(
            f"scratch playback confirmed duration_ms={track.duration_ms} rating={track.rating}"
        )

        target_ms = max(2000, min(track.duration_ms // 2, track.duration_ms - 2500))
        seek_payload = struct.pack("<Bi", 7, target_ms)
        client.send(7, seek_payload)
        seek_frames = client.collect(1.5)
        notes.append(packet_evidence("seek", 7, len(seek_payload), seek_frames))
        seek_updates = [
            state
            for frame in seek_frames
            if (state := latest_playback([frame])) is not None
        ]
        if seek_updates:
            notes.append(
                "seek replies "
                f"first_ms={seek_updates[0].position_ms} "
                f"last_ms={seek_updates[-1].position_ms} "
                f"min_ms={min(state.position_ms for state in seek_updates)} "
                f"max_ms={max(state.position_ms for state in seek_updates)} "
                f"state={seek_updates[-1].state}"
            )
        matching_seek_updates = [
            state
            for state in seek_updates
            if state.state in (1, 2) and abs(state.position_ms - target_ms) < 2500
        ]
        if matching_seek_updates:
            first_position = matching_seek_updates[0].position_ms
            later_position = matching_seek_updates[-1].position_ms
        else:
            first_position, _ = wait_for(
                client,
                lambda frames: (
                    state.position_ms
                    if (state := latest_playback(frames)) is not None
                    and state.state in (1, 2)
                    and abs(state.position_ms - target_ms) < 2500
                    else None
                ),
                timeout=8.0,
                request=lambda: client.send(23),
            )
            later_position = first_position
        notes.append(
            f"seek host confirmation target_ms={target_ms} "
            f"first_ms={first_position} last_event_ms={later_position}; "
            "UI progression is locally extrapolated between host events"
        )

        changed_rating = (test_rating + 1) % 6
        client.send(10, bytes((changed_rating,)))
        rating_frames = client.collect(1.2)
        notes.append(packet_evidence("set rating", 10, 1, rating_frames))
        rating_info = latest_track_info(rating_frames)
        queue_rating = playlist_first_rating(rating_frames, scratch_id)
        if rating_info is not None and rating_info.rating == changed_rating:
            confirmed_rating = rating_info.rating
        elif queue_rating == changed_rating:
            confirmed_rating = queue_rating
        else:
            confirmed_rating, _ = wait_for(
                client,
                lambda frames: (
                    changed_rating
                    if (
                        ((info := latest_track_info(frames)) is not None
                         and info.rating == changed_rating)
                        or playlist_first_rating(frames, scratch_id) == changed_rating
                    )
                    else None
                ),
                timeout=6.0,
                request=lambda: client.send(
                    4, struct.pack("<Bii", 10, scratch_id, 1)
                ),
            )
        notes.append(f"rating confirmed at {confirmed_rating}")
        client.send(10, bytes((test_rating,)))
        restore_rating_frames = client.collect(1.2)
        notes.append(packet_evidence("restore rating", 10, 1, restore_rating_frames))
        restored_rating_info = latest_track_info(restore_rating_frames)
        restored_queue_rating = playlist_first_rating(restore_rating_frames, scratch_id)
        if (
            (restored_rating_info is None or restored_rating_info.rating != test_rating)
            and restored_queue_rating != test_rating
        ):
            wait_for(
                client,
                lambda frames: (
                    True
                    if (
                        ((info := latest_track_info(frames)) is not None
                         and info.rating == test_rating)
                        or playlist_first_rating(frames, scratch_id) == test_rating
                    )
                    else None
                ),
                timeout=6.0,
                request=lambda: client.send(
                    4, struct.pack("<Bii", 10, scratch_id, 1)
                ),
            )
        rating_restored = True
        notes.append(f"rating restored to {test_rating}")

        playback_command(client, 3, 0)
        activate_and_wait(client, baseline_playlist)

        remove_payload = struct.pack("<ii", scratch_id, 0)
        client.send(32, remove_payload)
        remove_frames = client.collect(1.0)
        notes.append(packet_evidence("remove queue item", 32, len(remove_payload), remove_frames))
        wait_for(
            client,
            lambda frames: True if playlist_count(frames, scratch_id) == 0 else None,
            request=lambda: client.send(4, struct.pack("<Bii", 10, scratch_id, 1)),
        )
        notes.append("queue-item removal confirmed count=0")

        def matching_library_row(
            frames: list[Frame], request_id: int, title: str
        ) -> int | None:
            for frame in frames:
                parsed = parse_library_frame(frame, retain=100_000)
                if parsed is None or parsed.request_id != request_id:
                    continue
                for index, record in enumerate(parsed.records):
                    if record.title == title:
                        return index
            return None

        # Browsed library rows are addressed by a parent path and result index
        # through type 40. Validate that form against the scratch playlist.
        artist = track.artist.encode("utf-8")[:1024]
        album = track.album.encode("utf-8")[:1024]
        if not artist or not album:
            raise RuntimeError("scratch item lacks artist/album for type-40 validation")
        browse_request_id = int(time.time() * 1000) & 0x7FFFFFFF
        browse_payload = (
            struct.pack("<ii", browse_request_id, 2 | 4)
            + artist
            + b"\0"
            + album
            + b"\0"
        )
        client.send(39, browse_payload)
        browse_row, browse_frames = wait_for(
            client,
            lambda frames: matching_library_row(
                frames, browse_request_id, track.title
            ),
            timeout=10.0,
        )
        browse_row = int(browse_row)
        notes.append(
            packet_evidence(
                "browse exact library parent", 39, len(browse_payload), browse_frames
            )
        )
        select_payload = (
            struct.pack("<iii", scratch_id, 2 | 4, browse_row)
            + artist
            + b"\0"
            + album
            + b"\0"
        )
        client.send(40, select_payload)
        select_frames = client.collect(1.0)
        notes.append(
            packet_evidence(
                "enqueue browsed library row", 40, len(select_payload), select_frames
            )
        )
        wait_for(
            client,
            lambda frames: True if playlist_count(frames, scratch_id) == 1 else None,
            timeout=10.0,
            request=lambda: client.send(4, struct.pack("<Bii", 10, scratch_id, 1)),
        )
        notes.append("browsed library selection confirmed count=1")
        clear_payload = struct.pack("<i", scratch_id)
        client.send(20, clear_payload)
        clear_frames = client.collect(1.0)
        notes.append(packet_evidence("clear playlist", 20, len(clear_payload), clear_frames))
        wait_for(
            client,
            lambda frames: True if playlist_count(frames, scratch_id) == 0 else None,
            request=lambda: client.send(4, struct.pack("<Bii", 10, scratch_id, 1)),
        )
        notes.append("playlist clear confirmed count=0")

        # Whole-library search rows are tied to the type-24 request ID and are
        # selected through type 44, rather than by synthesizing a browse path.
        search_request_id = (browse_request_id + 1) & 0x7FFFFFFF or 1
        search_query = track.title.encode("utf-8")[:128]
        search_payload = struct.pack("<i", search_request_id) + search_query + b"\0"
        client.send(24, search_payload)
        search_row, search_frames = wait_for(
            client,
            lambda frames: matching_library_row(
                frames, search_request_id, track.title
            ),
            timeout=10.0,
        )
        search_row = int(search_row)
        notes.append(
            packet_evidence(
                "search exact library row", 24, len(search_payload), search_frames
            )
        )
        search_select_payload = struct.pack(
            "<iiiiH", search_request_id, scratch_id, search_row, -2, 0
        )
        client.send(44, search_select_payload)
        search_select_frames = client.collect(1.0)
        notes.append(
            packet_evidence(
                "enqueue searched library row",
                44,
                len(search_select_payload),
                search_select_frames,
            )
        )
        wait_for(
            client,
            lambda frames: True if playlist_count(frames, scratch_id) == 1 else None,
            timeout=10.0,
            request=lambda: client.send(4, struct.pack("<Bii", 10, scratch_id, 1)),
        )
        notes.append("searched library selection confirmed count=1")
        activate_and_wait(client, scratch_id)
        playback_command(client, 10, 0)
        playback_command(client, 1, 0)
        selected_track, _ = wait_for(
            client,
            lambda frames: (
                info
                if (info := latest_track_info(frames)) is not None
                and info.title == track.title
                else None
            ),
            timeout=8.0,
        )
        assert isinstance(selected_track, TrackInfo)
        notes.append("searched library play confirmed at appended row=0")
        playback_command(client, 3, 0)
        activate_and_wait(client, baseline_playlist)
        client.send(20, clear_payload)
        final_clear_frames = client.collect(1.0)
        notes.append(
            packet_evidence(
                "clear searched selection", 20, len(clear_payload), final_clear_frames
            )
        )
        wait_for(
            client,
            lambda frames: True if playlist_count(frames, scratch_id) == 0 else None,
            request=lambda: client.send(4, struct.pack("<Bii", 10, scratch_id, 1)),
        )
        notes.append("searched selection clear confirmed count=0")

    finally:
        if test_rating is not None and not rating_restored:
            client.send(10, bytes((test_rating,)))
            client.collect(0.5)
        if baseline_playlist is not None:
            try:
                activate_and_wait(client, baseline_playlist)
            except RuntimeError as exc:
                notes.append(
                    f"WARNING: baseline playlist reactivation not confirmed: {exc}"
                )
        if scratch_id is not None:
            delete_payload = struct.pack("<Bi", 16, scratch_id)
            client.send(7, delete_payload)
            delete_frames = client.collect(1.0)
            notes.append(packet_evidence("delete scratch playlist", 7, len(delete_payload), delete_frames))
            try:
                wait_for(
                    client,
                    lambda frames: (
                        True
                        if any(
                            expand_frame(frame).message_type == 41
                            for frame in frames
                        )
                        and all(
                            entry[0] != scratch_id
                            for entry in parse_playlists(frames)
                        )
                        else None
                    ),
                    request=lambda: client.send(4, struct.pack("<Bi", 13, -1)),
                )
                notes.append("scratch playlist deletion confirmed")
            except RuntimeError as exc:
                notes.append(f"WARNING: scratch playlist deletion not confirmed: {exc}")

        # Reapply every observable baseline field even though the scratch
        # workflow changes only playback, active playlist, position and rating.
        client.send(
            7,
            struct.pack(
                "<BHH", 6, baseline_playback.volume // 256, baseline_playback.volume
            ),
        )
        playback_command(client, 4, int(baseline_playback.shuffle))
        playback_command(client, 5, baseline_playback.repeat)
        if baseline_queue_index is not None and baseline_queue_index >= 0:
            playback_command(client, 10, baseline_queue_index)
            playback_command(client, 1, 0)
            playback_command(client, 7, max(0, baseline_playback.position_ms))
            if baseline_playback.state == 2:
                playback_command(client, 2, 0)
            elif baseline_playback.state == 3:
                playback_command(client, 3, 0)
        elif baseline_playback.state in (1, 2, 3):
            playback_command(client, baseline_playback.state, 0)
        restored_frames = client.collect(1.5) + request_playlist(client, baseline_playlist)
        restored = latest_playback(restored_frames)
        restored_queue_index = latest_queue_index(restored_frames)
        restored_playlist = resolved_current_playlist_id(
            restored_frames + request_playlists(client)
        )
        if restored is None:
            notes.append("WARNING: final playback state was not published")
        else:
            notes.append(
                "restored "
                f"playlist={restored_playlist} position_ms={restored.position_ms} "
                f"playback={restored.state} shuffle={restored.shuffle} "
                f"repeat={restored.repeat} volume={restored.volume} "
                f"row={restored_queue_index}"
            )
            position_restored = (
                baseline_queue_index is None
                or abs(restored.position_ms - baseline_playback.position_ms) <= 5000
            )
            if (
                restored_playlist != baseline_playlist
                or restored.state != baseline_playback.state
                or restored.shuffle != baseline_playback.shuffle
                or restored.repeat != baseline_playback.repeat
                or abs(restored.volume - baseline_playback.volume) > 256
                or restored_queue_index != baseline_queue_index
                or not position_restored
            ):
                raise RuntimeError("AIMP baseline restoration did not verify")
        if baseline_track is not None:
            notes.append(
                f"baseline track rating captured before run={baseline_track.rating}; "
                "scratch-item rating restoration was verified separately"
            )
    return notes


def cleanup_validation_playlists(client: Client, restore_playlist_id: int) -> list[str]:
    """Remove only probe-owned scratch playlists left by an interrupted run."""
    notes: list[str] = []
    frames = request_playlists(client)
    scratch_ids = sorted(
        [
            playlist_id
            for playlist_id, name, _, folder, read_only in parse_playlists(frames)
            if not folder and not read_only and name.startswith("ESP validation ")
        ],
        reverse=True,
    )
    playback_command(client, 3, 0)
    activate_and_wait(client, restore_playlist_id)
    for playlist_id in scratch_ids:
        payload = struct.pack("<Bi", 16, playlist_id)
        client.send(7, payload)
        result_frames = client.collect(0.8)
        notes.append(
            packet_evidence(f"delete orphan scratch {playlist_id}", 7, len(payload), result_frames)
        )
    _, verify_frames = wait_for(
        client,
        lambda observed: (
            True
            if any(expand_frame(frame).message_type == 41 for frame in observed)
            and not any(
                name.startswith("ESP validation ")
                for _, name, _, _, _ in parse_playlists(observed)
            )
            else None
        ),
        request=lambda: client.send(4, struct.pack("<Bi", 13, -1)),
    )
    remaining = [
        name
        for _, name, _, _, _ in parse_playlists(verify_frames)
        if name.startswith("ESP validation ")
    ]
    if remaining:
        raise RuntimeError(f"probe-owned scratch playlists remain: {len(remaining)}")
    notes.append(
        f"removed {len(scratch_ids)} probe-owned scratch playlist(s); "
        f"restored playlist={restore_playlist_id} playback=stopped"
    )
    return notes


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


def validate_advanced_and_restore(
    client: Client,
    initial: list[Frame],
    *,
    validate_equalizer: bool = True,
    validate_timer: bool = True,
) -> list[str]:
    """Exercise only reversible EQ/timer state and restore exact baselines.

    Computer power is deliberately absent. A sleep timer is tested only when
    the baseline is disabled, always with action 1 (stop playback), and is
    disabled again immediately.
    """
    capabilities = latest_capabilities(initial)
    if capabilities is None:
        raise RuntimeError("AIMP did not publish a capability mask")
    requested = 0
    if validate_equalizer:
        requested |= EQUALIZER_CAPABILITY
    if validate_timer:
        requested |= SLEEP_TIMER_CAPABILITY
    supported = capabilities & requested
    if not supported:
        raise RuntimeError("AIMP advertises none of the selected advanced controls")

    baseline_frames = request_advanced_state(client, supported)
    baseline_eq = (
        latest_equalizer(baseline_frames) if validate_equalizer else None
    )
    baseline_timer = (
        latest_sleep_timer(baseline_frames) if validate_timer else None
    )
    if supported & EQUALIZER_CAPABILITY and baseline_eq is None:
        raise RuntimeError("AIMP did not return equalizer state")
    if supported & SLEEP_TIMER_CAPABILITY and baseline_timer is None:
        raise RuntimeError("AIMP did not return sleep timer state")

    notes = [
        f"advanced baseline capabilities=0x{capabilities:016x} "
        f"eq={baseline_eq is not None} sleep={baseline_timer is not None}"
    ]
    equalizer_touched = False
    timer_touched = False

    def restore_with_reconnect(
        send_restore: Callable[[], None],
        confirm_restore: Callable[[list[Frame]], WaitValue | None],
        request_state: Callable[[], None],
    ) -> WaitValue:
        """Try on the current session, then reconnect once if it is unusable."""
        failures: list[str] = []
        for attempt in range(2):
            try:
                send_restore()
                restored, _ = wait_for(
                    client,
                    confirm_restore,
                    request=request_state,
                )
                return restored
            except (OSError, RuntimeError, ValueError) as exc:
                failures.append(str(exc))
                if attempt == 0:
                    try:
                        client.close()
                        client.connect()
                        client.collect(0.5)
                    except (OSError, RuntimeError, ValueError) as reconnect_exc:
                        failures.append(f"reconnect failed: {reconnect_exc}")
                        break
        raise RuntimeError("; ".join(failures))

    try:
        if baseline_eq is not None:
            adjusted = bytearray(baseline_eq.payload)
            baseline_preamp = baseline_eq.preamp_tenths_offset
            target_preamp = baseline_preamp + 10 if baseline_preamp <= 290 else baseline_preamp - 10
            struct.pack_into("<H", adjusted, 6, target_preamp)
            expected_equalizer = bytes(adjusted)
            equalizer_touched = True
            client.send(48, expected_equalizer)
            changed, _ = wait_for(
                client,
                lambda frames: (
                    state
                    if (state := latest_equalizer(frames)) is not None
                    and state.payload == expected_equalizer
                    else None
                ),
                request=lambda: client.send(4, struct.pack("<Bi", 17, 0)),
            )
            notes.append(
                "equalizer preamp confirmed "
                f"from={(baseline_preamp - 150) / 10:+.1f}dB "
                f"to={(changed.preamp_tenths_offset - 150) / 10:+.1f}dB"
            )

        if baseline_timer is not None:
            if baseline_timer.enabled:
                notes.append("sleep timer mutation skipped: baseline timer is active")
            else:
                test_timer = bytes((1, 1, 1, 0)) + struct.pack("<I", 15 * 60)
                timer_touched = True
                client.send(51, test_timer)
                changed, _ = wait_for(
                    client,
                    lambda frames: (
                        state
                        if (state := latest_sleep_timer(frames)) is not None
                        and state.enabled
                        and state.trigger == 1
                        and state.action == 1
                        and 0 < state.seconds_remaining <= 15 * 60
                        else None
                    ),
                    request=lambda: client.send(4, struct.pack("<Bi", 19, 0)),
                )
                notes.append(
                    "sleep timer stop-playback action confirmed "
                    f"seconds={changed.seconds_remaining}"
                )
    finally:
        restore_errors: list[str] = []
        if baseline_eq is not None and equalizer_touched:
            try:
                restored = restore_with_reconnect(
                    lambda: client.send(48, baseline_eq.payload),
                    lambda frames: (
                        state
                        if (state := latest_equalizer(frames)) is not None
                        and state.payload == baseline_eq.payload
                        else None
                    ),
                    lambda: client.send(4, struct.pack("<Bi", 17, 0)),
                )
                assert isinstance(restored, EqualizerState)
                notes.append(
                    "equalizer baseline restored "
                    f"enabled={restored.enabled} "
                    f"preamp={(restored.preamp_tenths_offset - 150) / 10:+.1f}dB"
                )
            except (OSError, RuntimeError, ValueError) as exc:
                restore_errors.append(f"equalizer baseline restore failed: {exc}")
        if baseline_timer is not None and timer_touched:
            try:
                restored = restore_with_reconnect(
                    lambda: client.send(51, baseline_timer.payload),
                    lambda frames: (
                        state
                        if (state := latest_sleep_timer(frames)) is not None
                        and state.payload == baseline_timer.payload
                        else None
                    ),
                    lambda: client.send(4, struct.pack("<Bi", 19, 0)),
                )
                assert isinstance(restored, SleepTimerState)
                notes.append(f"sleep timer baseline restored enabled={restored.enabled}")
            except (OSError, RuntimeError, ValueError) as exc:
                restore_errors.append(f"sleep timer baseline restore failed: {exc}")
        if restore_errors:
            raise RuntimeError("; ".join(restore_errors))
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
    if frame.message_type == 8:
        try:
            cover = parse_cover_frame(frame)
        except ValueError:
            cover = None
        if cover is None:
            return f"invalid cover response bytes={len(p)}"
        return (
            f"cover jpeg_bytes={len(cover.jpeg)} "
            f"identity={cover.identity.hex()} "
            f"sha256={hashlib.sha256(cover.jpeg).hexdigest()}"
        )
    if frame.message_type == 28 and len(p) == 8:
        return f"capabilities=0x{struct.unpack('<Q', p)[0]:016x}"
    if frame.message_type == 19:
        try:
            result = parse_library_frame(frame, retain=0)
        except ValueError:
            return f"invalid library response bytes={len(p)}"
        assert result is not None
        return (
            f"library request={result.request_id} kind={result.kind} "
            f"count={result.total_count} bytes={len(p)}"
        )
    if frame.message_type == 36:
        try:
            lyrics = parse_lyrics_frame(frame)
        except ValueError:
            return f"invalid lyrics response bytes={len(p)}"
        assert lyrics is not None
        return f"lyrics text_bytes={len(lyrics.text.encode('utf-8'))} identity={lyrics.identity.hex()}"
    if frame.message_type == 48:
        try:
            equalizer = parse_equalizer_frame(frame)
        except ValueError:
            return f"invalid equalizer response bytes={len(p)}"
        assert equalizer is not None
        return (
            f"equalizer enabled={equalizer.enabled} "
            f"preamp={(equalizer.preamp_tenths_offset - 150) / 10:+.1f}dB "
            f"bands={len(equalizer.bands)}"
        )
    if frame.message_type == 49:
        try:
            presets = parse_equalizer_presets_frame(frame)
        except ValueError:
            return f"invalid equalizer presets response bytes={len(p)}"
        assert presets is not None
        return f"equalizer-presets count={len(presets)}"
    if frame.message_type == 51:
        try:
            timer = parse_sleep_timer_frame(frame)
        except ValueError:
            return f"invalid sleep timer response bytes={len(p)}"
        assert timer is not None
        return (
            f"sleep-timer enabled={timer.enabled} trigger={timer.trigger} "
            f"action={timer.action} seconds={timer.seconds_remaining}"
        )
    if frame.message_type == 41 and len(p) >= 12:
        count = struct.unpack_from("<I", p)[0]
        folder_id = struct.unpack_from("<i", p, 4)[0]
        entries = parse_playlists([frame])
        summary = ", ".join(
            f"{playlist_id}:{name!r} ({track_count} tracks)"
            f"{' [folder]' if folder else ''}{' [read-only]' if read_only else ''}"
            for playlist_id, name, track_count, folder, read_only in entries
        )
        return f"playlists folder_id={folder_id} count={count} entries=[{summary}]"
    if frame.message_type in (9, 31, 50):
        count = struct.unpack_from("<I", p)[0] if len(p) >= 4 else -1
        playlist_id = struct.unpack_from("<i", p, 4)[0] if len(p) >= 8 else -1
        return (
            f"playlist type={frame.message_type} id={playlist_id} "
            f"count={count} bytes={len(p)}"
        )
    if frame.message_type == 23:
        return "keepalive"
    return f"type={frame.message_type} bytes={len(p)} payload={p.hex()}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", type=Path, default=Path(".env"))
    parser.add_argument(
        "command",
        choices=(
            "state",
            "playlists",
            "playlist",
            "activate",
            "ping",
            "exercise",
            "validate-mutations",
            "cleanup-validation",
            "play-row",
            "cover",
            "library",
            "library-browse",
            "library-search",
            "lyrics",
            "advanced",
            "validate-advanced",
            "stop",
        ),
    )
    parser.add_argument("--playlist-id", type=int, default=-1)
    parser.add_argument("--queue-row", type=int, default=0)
    parser.add_argument("--cover-size", type=int, default=144)
    parser.add_argument(
        "--library-category",
        choices=("artists", "albums", "genres", "composers"),
        default="artists",
    )
    parser.add_argument("--query", default="Beatles")
    parser.add_argument("--genre", default="")
    parser.add_argument("--artist", default="")
    parser.add_argument("--album", default="")
    parser.add_argument("--composer", default="")
    parser.add_argument(
        "--confirm-live-mutations",
        action="store_true",
        help="allow guarded mutation matrices that restore captured state",
    )
    parser.add_argument(
        "--advanced-scope",
        choices=("all", "equalizer", "sleep-timer"),
        default="all",
        help="limit validate-advanced to one independently restorable control",
    )
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
        if args.command == "advanced":
            capabilities = latest_capabilities(initial)
            if capabilities is None:
                raise RuntimeError("AIMP did not publish a capability mask")
            print(
                f"capabilities=0x{capabilities:016x} "
                f"eq={bool(capabilities & EQUALIZER_CAPABILITY)} "
                f"sleep={bool(capabilities & SLEEP_TIMER_CAPABILITY)} "
                f"power={bool(capabilities & COMPUTER_POWER_CAPABILITY)}"
            )
            frames = request_advanced_state(client, capabilities)
            equalizer = next(
                (
                    parsed
                    for frame in frames
                    if (parsed := parse_equalizer_frame(frame)) is not None
                ),
                None,
            )
            presets = next(
                (
                    parsed
                    for frame in frames
                    if (parsed := parse_equalizer_presets_frame(frame)) is not None
                ),
                None,
            )
            timer = next(
                (
                    parsed
                    for frame in frames
                    if (parsed := parse_sleep_timer_frame(frame)) is not None
                ),
                None,
            )
            if capabilities & EQUALIZER_CAPABILITY:
                if equalizer is None:
                    print(
                        "equalizer state unavailable: plug-in advertised the "
                        "capability but returned no baseline"
                    )
                else:
                    print(describe(Frame(48, equalizer.payload)))
                if presets is None:
                    print(
                        "equalizer presets unavailable: plug-in advertised the "
                        "capability but returned no collection"
                    )
                else:
                    print(f"equalizer-presets count={len(presets)}")
            if capabilities & SLEEP_TIMER_CAPABILITY:
                if timer is None:
                    print(
                        "sleep-timer state unavailable: plug-in advertised the "
                        "capability but returned no baseline"
                    )
                else:
                    print(describe(Frame(51, timer.payload)))
            if not capabilities & (EQUALIZER_CAPABILITY | SLEEP_TIMER_CAPABILITY):
                print("advanced state unavailable: plug-in advertises no EQ or sleep timer")
            return 0
        if args.command == "validate-advanced":
            if not args.confirm_live_mutations:
                raise RuntimeError(
                    "validate-advanced requires --confirm-live-mutations"
                )
            for note in validate_advanced_and_restore(
                client,
                initial,
                validate_equalizer=args.advanced_scope in ("all", "equalizer"),
                validate_timer=args.advanced_scope in ("all", "sleep-timer"),
            ):
                print(note)
            return 0
        if args.command in ("library", "library-browse", "library-search"):
            request_id = int(time.time()) & 0x7FFFFFFF
            if args.command == "library":
                subcommand = {
                    "artists": 7,
                    "albums": 16,
                    "genres": 9,
                    "composers": 12,
                }[args.library_category]
                client.send(4, struct.pack("<Bi", subcommand, request_id))
            elif args.command == "library-search":
                query = args.query.encode("utf-8")[:128]
                if not query:
                    raise RuntimeError("--query must not be empty")
                client.send(24, struct.pack("<i", request_id) + query + b"\0")
            else:
                values = (args.genre, args.artist, args.album, args.composer)
                encoded = [value.encode("utf-8")[:1024] for value in values]
                flags = sum(1 << index for index, value in enumerate(encoded) if value)
                if flags == 0:
                    raise RuntimeError(
                        "library-browse needs --genre, --artist, --album, or --composer"
                    )
                payload = struct.pack("<ii", request_id, flags)
                payload += b"".join(value + b"\0" for value in encoded if value)
                client.send(39, payload)
            frames = client.collect(2.5)
            result = next(
                (
                    parsed
                    for frame in frames
                    if (parsed := parse_library_frame(frame, retain=10)) is not None
                    and parsed.request_id == request_id
                ),
                None,
            )
            if result is None:
                raise RuntimeError("AIMP did not return a matching library response")
            print(
                f"library request={result.request_id} kind={result.kind} "
                f"count={result.total_count} retained={len(result.records)}"
            )
            for record in result.records:
                print(f"item title={record.title!r} subtitle={record.subtitle!r}")
            return 0
        if args.command == "lyrics":
            client.send(4, struct.pack("<Bi", 11, 0))
            frames = client.collect(2.0)
            lyrics = next(
                (
                    parsed
                    for frame in frames
                    if (parsed := parse_lyrics_frame(frame)) is not None
                ),
                None,
            )
            if lyrics is None:
                raise RuntimeError("AIMP did not return a lyrics response")
            print(
                f"lyrics text_bytes={len(lyrics.text.encode('utf-8'))} "
                f"identity={lyrics.identity.hex()}"
            )
            return 0
        if args.command == "exercise":
            for note in exercise_and_restore(client, initial):
                print(note)
            return 0
        if args.command == "validate-mutations":
            if not args.confirm_live_mutations:
                raise RuntimeError(
                    "validate-mutations requires --confirm-live-mutations"
                )
            validate_mutations_and_restore(client, initial, emit=print)
            return 0
        if args.command == "cleanup-validation":
            if not args.confirm_live_mutations:
                raise RuntimeError(
                    "cleanup-validation requires --confirm-live-mutations"
                )
            for note in cleanup_validation_playlists(client, args.playlist_id):
                print(note)
            return 0
        if args.command == "play-row":
            if args.playlist_id >= 0:
                activate_and_wait(client, args.playlist_id)
            playback_command(client, 10, args.queue_row)
            playback_command(client, 1, 0)
            frames = client.collect(1.5)
            track = latest_track_info(frames)
            state = latest_playback(frames)
            print(
                f"play-row requested playlist={args.playlist_id} row={args.queue_row} "
                f"track={None if track is None else track.title!r} "
                f"state={None if state is None else state.state}"
            )
            return 0
        if args.command == "stop":
            playback_command(client, 3, 0)
            for frame in client.collect(1.0):
                print(describe(frame))
            return 0
        if args.command == "cover":
            if not 1 <= args.cover_size <= 256:
                raise RuntimeError("--cover-size must be between 1 and 256")
            client.send(4, struct.pack("<Bii", 3, args.cover_size, args.cover_size))
            frames = client.collect(1.5)
            covers = [
                cover
                for frame in frames
                if (cover := parse_cover_frame(frame)) is not None
            ]
            if covers:
                cover = covers[-1]
                print(
                    f"cover size={args.cover_size} jpeg_bytes={len(cover.jpeg)} "
                    f"identity={cover.identity.hex()} "
                    f"sha256={hashlib.sha256(cover.jpeg).hexdigest()}"
                )
                return 0
            if any(expand_frame(frame).message_type == 13 for frame in frames):
                print(f"cover unavailable size={args.cover_size}")
                return 0
            raise RuntimeError("AIMP did not return a cover response")
        if args.command == "playlists":
            client.send(4, struct.pack("<Bi", 13, args.playlist_id))
        elif args.command == "playlist":
            client.send(4, struct.pack("<Bii", 10, args.playlist_id, 1))
        elif args.command == "activate":
            playback_command(client, 12, args.playlist_id)
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
