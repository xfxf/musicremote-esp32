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


if __name__ == "__main__":
    unittest.main()
