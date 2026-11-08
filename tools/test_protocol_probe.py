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


if __name__ == "__main__":
    unittest.main()
