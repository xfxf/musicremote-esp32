import struct
import unittest

import protocol_probe as probe


class CoverFrameTest(unittest.TestCase):
    def test_parses_bounded_jpeg(self) -> None:
        jpeg = b"\xff\xd8data\xff\xd9"
        identity = bytes(range(16))
        cover = probe.parse_cover_frame(
            probe.Frame(8, struct.pack("<I", len(jpeg)) + identity + jpeg)
        )
        self.assertIsNotNone(cover)
        self.assertEqual(cover.identity, identity)
        self.assertEqual(cover.jpeg, jpeg)

    def test_rejects_mismatched_length(self) -> None:
        payload = struct.pack("<I", 99) + bytes(16) + b"\xff\xd8\xff\xd9"
        with self.assertRaisesRegex(ValueError, "invalid cover-art response"):
            probe.parse_cover_frame(probe.Frame(8, payload))

    def test_rejects_non_jpeg(self) -> None:
        payload = struct.pack("<I", 4) + bytes(16) + b"nope"
        with self.assertRaisesRegex(ValueError, "invalid cover-art response"):
            probe.parse_cover_frame(probe.Frame(8, payload))

    def test_ignores_unrelated_frame(self) -> None:
        self.assertIsNone(probe.parse_cover_frame(probe.Frame(13, b"")))

    def test_describe_reports_malformed_cover(self) -> None:
        payload = struct.pack("<I", 99) + bytes(16) + b"\xff\xd8\xff\xd9"
        self.assertEqual(
            probe.describe(probe.Frame(8, payload)),
            f"invalid cover response bytes={len(payload)}",
        )


class LibraryFrameTest(unittest.TestCase):
    def test_parses_nodes(self) -> None:
        payload = struct.pack("<IIB", 42, 2, 0) + b"Artist A\0Artist B\0"
        result = probe.parse_library_frame(probe.Frame(19, payload))
        self.assertIsNotNone(result)
        self.assertEqual(result.request_id, 42)
        self.assertEqual(result.total_count, 2)
        self.assertEqual([record.title for record in result.records], ["Artist A", "Artist B"])

    def test_parses_album_record(self) -> None:
        album = b"Abbey Road"
        artist = b"The Beatles"
        body = album + b"\0" + artist + b"\0"
        record = struct.pack("<HHHHI", len(album), len(artist), 1969, 0, len(body)) + body
        result = probe.parse_library_frame(
            probe.Frame(19, struct.pack("<IIB", 7, 1, 6) + record)
        )
        self.assertEqual(result.records[0].title, "Abbey Road")
        self.assertEqual(result.records[0].subtitle, "The Beatles")
        self.assertEqual(result.records[0].year, 1969)

    def test_parses_track_record(self) -> None:
        artist = b"The Beatles"
        title = b"Something"
        album = b"Abbey Road"
        body = artist + b"\0" + title + b"\0" + album + b"\0"
        record = (
            struct.pack(
                "<HHHHIiHBB",
                len(artist),
                len(title),
                len(album),
                1969,
                len(body),
                182_000,
                320,
                0,
                0,
            )
            + body
        )
        result = probe.parse_library_frame(
            probe.Frame(19, struct.pack("<IIB", 8, 1, 7) + record)
        )
        self.assertEqual(result.records[0].title, "Something")
        self.assertEqual(result.records[0].subtitle, "The Beatles")
        self.assertEqual(result.records[0].duration_ms, 182_000)

    def test_rejects_trailing_data(self) -> None:
        payload = struct.pack("<IIB", 1, 1, 0) + b"Artist\0extra"
        with self.assertRaisesRegex(ValueError, "trailing data"):
            probe.parse_library_frame(probe.Frame(19, payload))

    def test_rejects_record_overrun(self) -> None:
        payload = struct.pack("<IIBHHHHI", 1, 1, 6, 2, 2, 0, 0, 999) + b"a\0b\0"
        with self.assertRaisesRegex(ValueError, "album size"):
            probe.parse_library_frame(probe.Frame(19, payload))


class LyricsFrameTest(unittest.TestCase):
    def test_parses_bounded_lyrics(self) -> None:
        text = "Line one\nLine two".encode()
        identity = bytes(range(16))
        result = probe.parse_lyrics_frame(
            probe.Frame(36, struct.pack("<I", len(text)) + identity + text)
        )
        self.assertEqual(result.identity, identity)
        self.assertEqual(result.text, "Line one\nLine two")

    def test_rejects_mismatched_length(self) -> None:
        with self.assertRaisesRegex(ValueError, "lyrics response length"):
            probe.parse_lyrics_frame(probe.Frame(36, struct.pack("<I", 4) + bytes(16)))

    def test_rejects_oversized_lyrics(self) -> None:
        text = b"x" * 32
        frame = probe.Frame(36, struct.pack("<I", len(text)) + bytes(16) + text)
        with self.assertRaisesRegex(ValueError, "display limit"):
            probe.parse_lyrics_frame(frame, max_text=16)


class AdvancedControlsFrameTest(unittest.TestCase):
    def test_parses_equalizer_and_presets(self) -> None:
        equalizer = bytes((2, 15, 0, 1)) + struct.pack(
            "<HHHHHHH", 300, 150, 0, 70, 160, 1000, 140
        )
        state = probe.parse_equalizer_frame(probe.Frame(48, equalizer))
        self.assertTrue(state.enabled)
        self.assertEqual(state.preamp_tenths_offset, 150)
        self.assertEqual(state.bands[1], probe.EqualizerBand(1000, 140))

        presets = probe.parse_equalizer_presets_frame(
            probe.Frame(49, struct.pack("<HH", 2, 0) + b"Flat\0Rock\0")
        )
        self.assertEqual(presets, ("Flat", "Rock"))

    def test_rejects_malformed_equalizer(self) -> None:
        payload = bytes((1, 12, 0, 1)) + struct.pack(
            "<HHHHH", 300, 150, 0, 70, 150
        )
        with self.assertRaisesRegex(ValueError, "invalid equalizer response"):
            probe.parse_equalizer_frame(probe.Frame(48, payload))

        payload = bytes((1, 15, 0, 1)) + struct.pack(
            "<HHHHH", 300, 150, 0, 70, 301
        )
        with self.assertRaisesRegex(ValueError, "invalid equalizer band"):
            probe.parse_equalizer_frame(probe.Frame(48, payload))

    def test_rejects_malformed_presets(self) -> None:
        with self.assertRaisesRegex(ValueError, "unterminated equalizer preset"):
            probe.parse_equalizer_presets_frame(
                probe.Frame(49, struct.pack("<HH", 1, 0) + b"Flat")
            )
        with self.assertRaisesRegex(ValueError, "trailing data"):
            probe.parse_equalizer_presets_frame(
                probe.Frame(49, struct.pack("<HH", 0, 0) + b"extra")
            )

    def test_parses_sleep_timer(self) -> None:
        payload = bytes((1, 1, 1, 0)) + struct.pack("<I", 900)
        state = probe.parse_sleep_timer_frame(probe.Frame(51, payload))
        self.assertTrue(state.enabled)
        self.assertEqual(state.seconds_remaining, 900)
        with self.assertRaisesRegex(ValueError, "invalid sleep timer response"):
            probe.parse_sleep_timer_frame(probe.Frame(51, bytes(8)))

    def test_reads_latest_capability_mask(self) -> None:
        frames = [
            probe.Frame(28, struct.pack("<Q", probe.EQUALIZER_CAPABILITY)),
            probe.Frame(23, b""),
        ]
        self.assertEqual(
            probe.latest_capabilities(frames), probe.EQUALIZER_CAPABILITY
        )

    def test_latest_advanced_state_ignores_unrelated_frames(self) -> None:
        equalizer = bytes((1, 15, 0, 0)) + struct.pack(
            "<HHHHH", 300, 150, 0, 70, 150
        )
        timer = bytes((0, 1, 1, 0)) + struct.pack("<I", 0)
        frames = [
            probe.Frame(48, equalizer),
            probe.Frame(23, b""),
            probe.Frame(51, timer),
        ]
        self.assertEqual(probe.latest_equalizer(frames).payload, equalizer)
        self.assertEqual(probe.latest_sleep_timer(frames).payload, timer)

    def test_guarded_advanced_validation_restores_baselines(self) -> None:
        class FakeClient:
            def __init__(self, equalizer: bytes, timer: bytes) -> None:
                self.equalizer = equalizer
                self.timer = timer
                self.pending: list[probe.Frame] = []
                self.sent: list[tuple[int, bytes]] = []
                self.timer_restore_queries = 0
                self.restoring_timer = False

            def close(self) -> None:
                pass

            def connect(self) -> None:
                pass

            def send(self, message_type: int, payload: bytes = b"") -> None:
                self.sent.append((message_type, payload))
                if message_type == 48:
                    self.equalizer = payload
                    self.pending.append(probe.Frame(48, payload))
                elif message_type == 51:
                    self.timer = payload
                    if payload == baseline_timer:
                        # A disabled-but-different response must not satisfy exact
                        # restoration; the subsequent state request returns truth.
                        wrong = bytes((0, 2, 2, 0)) + struct.pack("<I", 99)
                        self.pending.append(probe.Frame(51, wrong))
                        self.restoring_timer = True
                    else:
                        self.pending.append(probe.Frame(51, payload))
                elif message_type == 4 and payload:
                    if payload[0] == 17:
                        self.pending.append(probe.Frame(48, self.equalizer))
                    elif payload[0] == 18:
                        self.pending.append(
                            probe.Frame(49, struct.pack("<HH", 1, 0) + b"Flat\0")
                        )
                    elif payload[0] == 19:
                        if self.restoring_timer:
                            self.timer_restore_queries += 1
                            if self.timer_restore_queries == 1:
                                wrong = bytes((0, 2, 2, 0)) + struct.pack("<I", 99)
                                self.pending.append(probe.Frame(51, wrong))
                            else:
                                self.pending.append(probe.Frame(51, self.timer))
                        else:
                            self.pending.append(probe.Frame(51, self.timer))

            def collect(self, _seconds: float = 0.8) -> list[probe.Frame]:
                frames = self.pending
                self.pending = []
                return frames

        baseline_equalizer = bytes((1, 15, 0, 1)) + struct.pack(
            "<HHHHH", 300, 150, 0, 70, 150
        )
        baseline_timer = bytes((0, 1, 1, 0)) + struct.pack("<I", 0)
        client = FakeClient(baseline_equalizer, baseline_timer)
        initial = [
            probe.Frame(
                28,
                struct.pack(
                    "<Q", probe.EQUALIZER_CAPABILITY | probe.SLEEP_TIMER_CAPABILITY
                ),
            )
        ]

        notes = probe.validate_advanced_and_restore(client, initial)

        self.assertEqual(client.equalizer, baseline_equalizer)
        self.assertEqual(client.timer, baseline_timer)
        self.assertGreaterEqual(client.timer_restore_queries, 2)
        self.assertTrue(any("equalizer baseline restored" in note for note in notes))
        self.assertTrue(any("sleep timer baseline restored" in note for note in notes))
        self.assertFalse(
            any(
                message_type == 4 and payload and payload[0] == 14
                for message_type, payload in client.sent
            )
        )

    def test_ambiguous_write_reconnects_and_restores_equalizer(self) -> None:
        baseline_equalizer = bytes((1, 15, 0, 1)) + struct.pack(
            "<HHHHH", 300, 150, 0, 70, 150
        )

        class FaultClient:
            def __init__(self) -> None:
                self.equalizer = baseline_equalizer
                self.pending: list[probe.Frame] = []
                self.connected = True
                self.failed = False
                self.reconnects = 0

            def send(self, message_type: int, payload: bytes = b"") -> None:
                if not self.connected:
                    raise OSError("socket unusable")
                if message_type == 48:
                    self.equalizer = payload
                    self.pending.append(probe.Frame(48, payload))
                    if not self.failed and payload != baseline_equalizer:
                        self.failed = True
                        self.connected = False
                        raise OSError("ambiguous send")
                elif message_type == 4 and payload and payload[0] == 17:
                    self.pending.append(probe.Frame(48, self.equalizer))

            def collect(self, _seconds: float = 0.8) -> list[probe.Frame]:
                if not self.connected:
                    raise OSError("socket unusable")
                frames = self.pending
                self.pending = []
                return frames

            def close(self) -> None:
                self.connected = False

            def connect(self) -> None:
                self.connected = True
                self.reconnects += 1

        client = FaultClient()
        initial = [
            probe.Frame(28, struct.pack("<Q", probe.EQUALIZER_CAPABILITY)),
            probe.Frame(48, baseline_equalizer),
        ]

        with self.assertRaisesRegex(OSError, "ambiguous send"):
            probe.validate_advanced_and_restore(client, initial)

        self.assertEqual(client.equalizer, baseline_equalizer)
        self.assertEqual(client.reconnects, 1)


if __name__ == "__main__":
    unittest.main()
