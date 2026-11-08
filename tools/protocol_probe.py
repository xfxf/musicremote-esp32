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
            if state.state == 1 and abs(state.position_ms - target_ms) < 2500
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
                    and state.state == 1
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

        client.send(21, add_payload)
        client.collect(1.0)
        wait_for(
            client,
            lambda frames: True if playlist_count(frames, scratch_id) == 1 else None,
            timeout=10.0,
            request=lambda: client.send(4, struct.pack("<Bii", 10, scratch_id, 1)),
        )
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
            "stop",
        ),
    )
    parser.add_argument("--playlist-id", type=int, default=-1)
    parser.add_argument("--queue-row", type=int, default=0)
    parser.add_argument(
        "--confirm-live-mutations",
        action="store_true",
        help="allow the disposable-playlist mutation/seek/rating matrix",
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
