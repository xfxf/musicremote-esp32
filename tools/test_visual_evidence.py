from __future__ import annotations

import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from check_visual_evidence import (
    ValidationError,
    read_png_size,
    validate_changed_screenshots,
    validate_pr_body,
    validate_readme,
)


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(
        ">I", zlib.crc32(kind + data) & 0xFFFFFFFF
    )


def make_png(width: int = 480, height: int = 320) -> bytes:
    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    rows = b"".join(b"\0" + bytes(width) for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(rows))
        + png_chunk(b"IEND", b"")
    )


def make_indexed_png_with_late_palette() -> bytes:
    header = struct.pack(">IIBBBBB", 480, 320, 8, 3, 0, 0, 0)
    rows = b"".join(b"\0" + bytes(480) for _ in range(320))
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(rows))
        + png_chunk(b"PLTE", b"\0\0\0")
        + png_chunk(b"IEND", b"")
    )


class PngValidationTests(unittest.TestCase):
    def test_valid_device_png(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "screen.png"
            path.write_bytes(make_png())
            self.assertEqual(read_png_size(path), (480, 320))

    def test_corrupt_crc_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "screen.png"
            payload = bytearray(make_png())
            payload[-1] ^= 1
            path.write_bytes(payload)
            with self.assertRaises(ValidationError):
                read_png_size(path)

    def test_declared_geometry_bomb_is_rejected_before_inflate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "screen.png"
            path.write_bytes(make_png(4000, 1000))
            with self.assertRaisesRegex(ValidationError, "decoded PNG exceeds"):
                read_png_size(path)

    def test_indexed_palette_after_idat_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "screen.png"
            path.write_bytes(make_indexed_png_with_late_palette())
            with self.assertRaisesRegex(ValidationError, "PLTE must precede IDAT"):
                read_png_size(path)

    def test_unknown_critical_chunk_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "screen.png"
            valid = make_png()
            ihdr_end = 8 + 12 + 13
            path.write_bytes(
                valid[:ihdr_end]
                + png_chunk(b"ABCD", b"")
                + valid[ihdr_end:]
            )
            with self.assertRaisesRegex(ValidationError, "unknown critical chunk"):
                read_png_size(path)

    def test_nonconsecutive_idat_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "screen.png"
            header = struct.pack(">IIBBBBB", 480, 320, 8, 0, 0, 0, 0)
            rows = b"".join(b"\0" + bytes(480) for _ in range(320))
            compressed = zlib.compress(rows)
            split = len(compressed) // 2
            path.write_bytes(
                b"\x89PNG\r\n\x1a\n"
                + png_chunk(b"IHDR", header)
                + png_chunk(b"IDAT", compressed[:split])
                + png_chunk(b"tEXt", b"note\0evidence")
                + png_chunk(b"IDAT", compressed[split:])
                + png_chunk(b"IEND", b"")
            )
            with self.assertRaisesRegex(ValidationError, "not consecutive"):
                read_png_size(path)


class ReadmeValidationTests(unittest.TestCase):
    def test_gallery_requires_local_device_screenshots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            references = []
            for index in range(4):
                name = f"screen-{index}.png"
                (artifacts / name).write_bytes(make_png())
                references.append(f"![Screen {index}](artifacts/{name})")
            (root / "README.md").write_text("\n".join(references), encoding="utf-8")
            self.assertEqual(validate_readme(root, root / "README.md"), [])

    def test_wrong_geometry_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            references = []
            for index in range(4):
                name = f"screen-{index}.png"
                size = (320, 240) if index == 3 else (480, 320)
                (artifacts / name).write_bytes(make_png(*size))
                references.append(f"![Screen {index}](artifacts/{name})")
            (root / "README.md").write_text("\n".join(references), encoding="utf-8")
            errors = validate_readme(root, root / "README.md")
            self.assertTrue(any("expected 480x320" in error for error in errors))


class PullRequestValidationTests(unittest.TestCase):
    def test_visual_change_requires_before_and_after_images(self) -> None:
        body = """Visual change: yes

### Before
![Before](https://example.invalid/before.png)

### After
![After](https://example.invalid/after.png)
"""
        self.assertEqual(validate_pr_body(body, True, True), [])

    def test_non_visual_change_requires_reasons(self) -> None:
        body = """Visual change: no

### Before
N/A: documentation policy only.

### After
N/A: no firmware pixels changed.
"""
        self.assertEqual(validate_pr_body(body), [])

    def test_ui_change_cannot_claim_non_visual(self) -> None:
        body = """Visual change: no

### Before
N/A: none.

### After
N/A: none.
"""
        errors = validate_pr_body(body, True, False)
        self.assertIn("music_ui source changed, so `Visual change` must be `yes`", errors)
        self.assertTrue(any("refreshed artifacts/*.png" in error for error in errors))

    def test_template_comment_does_not_satisfy_non_visual_reason(self) -> None:
        body = """Visual change: no

### Before
<!-- Non-visual: write `N/A: reason`. -->

### After
<!-- Non-visual: write `N/A: reason`. -->
"""
        errors = validate_pr_body(body)
        self.assertTrue(any("N/A:` in Before" in error for error in errors))
        self.assertTrue(any("N/A:` in After" in error for error in errors))

    def test_refreshed_screenshot_must_decode_and_appear_in_after(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            screenshot = artifacts / "after.png"
            screenshot.write_bytes(make_png())
            body = """### After
![After](artifacts/after.png)
"""
            changed, errors = validate_changed_screenshots(
                root, {"artifacts/after.png"}, body
            )
            self.assertTrue(changed)
            self.assertEqual(errors, [])

    def test_unrelated_same_basename_does_not_reference_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            (artifacts / "after.png").write_bytes(make_png())
            body = """### After
![Unrelated](https://example.invalid/after.png)
"""
            changed, errors = validate_changed_screenshots(
                root, {"artifacts/after.png"}, body
            )
            self.assertTrue(changed)
            self.assertTrue(any("does not reference" in error for error in errors))

    def test_exact_repository_url_references_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            (artifacts / "after.png").write_bytes(make_png())
            body = """### After
![After](https://github.com/xfxf/musicremote-esp32/blob/topic/artifacts/after.png?raw=true)
"""
            changed, errors = validate_changed_screenshots(
                root, {"artifacts/after.png"}, body
            )
            self.assertTrue(changed)
            self.assertEqual(errors, [])

    def test_deleted_or_corrupt_screenshot_does_not_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            (artifacts / "corrupt.png").write_bytes(b"not a png")
            changed, errors = validate_changed_screenshots(
                root,
                {"artifacts/deleted.png", "artifacts/corrupt.png"},
                "### After\n![After](artifacts/corrupt.png)\n",
            )
            self.assertFalse(changed)
            self.assertTrue(any("does not decode" in error for error in errors))
            self.assertTrue(any("not an existing file" in error for error in errors))

    def test_wrong_size_screenshot_does_not_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            (artifacts / "after.png").write_bytes(make_png(320, 240))
            changed, errors = validate_changed_screenshots(
                root, {"artifacts/after.png"}, "### After\nN/A: none\n"
            )
            self.assertFalse(changed)
            self.assertTrue(any("expected 480x320" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
