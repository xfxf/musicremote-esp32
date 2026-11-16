#!/usr/bin/env python3
"""Validate committed device screenshots and pull-request visual evidence."""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
import zlib
from collections.abc import Collection
from pathlib import Path
from urllib.parse import unquote, urlsplit


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
DEVICE_SIZE = (480, 320)
MAX_PNG_BYTES = 2 * 1024 * 1024
MAX_DECODED_BYTES = 2 * 1024 * 1024
README_IMAGE = re.compile(r"!\[[^\]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
MARKDOWN_IMAGE = re.compile(r"!\[[^\]]*\]\([^)]+\)")
MARKDOWN_IMAGE_TARGET = re.compile(
    r"!\[[^\]]*\]\((?:<)?([^)>\s]+)(?:>)?(?:\s+[^)]*)?\)"
)
VISUAL_SOURCES = {"main/music_ui.cpp", "main/music_ui.hpp"}
GITHUB_REPOSITORY = "/xfxf/musicremote-esp32/"


class ValidationError(ValueError):
    pass


def read_png_size(path: Path) -> tuple[int, int]:
    """Structurally decode a non-interlaced PNG and return its dimensions."""
    if path.stat().st_size > MAX_PNG_BYTES:
        raise ValidationError("PNG exceeds the 2 MiB evidence limit")
    payload = path.read_bytes()
    if not payload.startswith(PNG_SIGNATURE):
        raise ValidationError("not a PNG")
    offset = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    idat = bytearray()
    saw_iend = False
    saw_palette = False
    idat_started = False
    idat_finished = False
    first = True
    while offset < len(payload):
        if offset + 12 > len(payload):
            raise ValidationError("truncated chunk")
        length = struct.unpack_from(">I", payload, offset)[0]
        kind = payload[offset + 4 : offset + 8]
        end = offset + 12 + length
        if end > len(payload):
            raise ValidationError("chunk exceeds file")
        data = payload[offset + 8 : offset + 8 + length]
        expected_crc = struct.unpack_from(">I", payload, offset + 8 + length)[0]
        if zlib.crc32(kind + data) & 0xFFFFFFFF != expected_crc:
            raise ValidationError(f"bad {kind.decode('ascii', 'replace')} CRC")
        if first and kind != b"IHDR":
            raise ValidationError("IHDR is not first")
        if len(kind) != 4 or not all(
            ord("A") <= byte <= ord("Z") or ord("a") <= byte <= ord("z")
            for byte in kind
        ):
            raise ValidationError("invalid PNG chunk type")
        if kind[2] & 0x20:
            raise ValidationError("PNG chunk has a non-zero reserved bit")
        first = False
        if kind == b"IHDR":
            if length != 13 or width is not None:
                raise ValidationError("invalid IHDR")
            width, height, bit_depth, color_type, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", data)
            )
            if width == 0 or height == 0 or compression != 0 or filtering != 0:
                raise ValidationError("invalid PNG geometry or method")
            if interlace != 0:
                raise ValidationError("interlaced PNG is not supported")
            valid_depths = {
                0: {1, 2, 4, 8, 16},
                2: {8, 16},
                3: {1, 2, 4, 8},
                4: {8, 16},
                6: {8, 16},
            }
            if color_type not in valid_depths or bit_depth not in valid_depths[color_type]:
                raise ValidationError("invalid color type / bit depth")
        elif kind == b"PLTE":
            if saw_palette or idat_started or color_type in {0, 4}:
                raise ValidationError("invalid PLTE placement")
            if length == 0 or length > 768 or length % 3 != 0:
                raise ValidationError("invalid PLTE size")
            if color_type == 3 and length // 3 > 1 << bit_depth:
                raise ValidationError("palette exceeds indexed bit depth")
            saw_palette = True
        elif kind == b"IDAT":
            if idat_finished:
                raise ValidationError("IDAT chunks are not consecutive")
            if color_type == 3 and not saw_palette:
                raise ValidationError("indexed PNG PLTE must precede IDAT")
            idat_started = True
            idat.extend(data)
        elif kind == b"IEND":
            if length != 0:
                raise ValidationError("invalid IEND")
            saw_iend = True
            offset = end
            break
        elif kind[0] & 0x20 == 0:
            raise ValidationError(
                f"unknown critical chunk {kind.decode('ascii', 'replace')}"
            )
        if idat_started and kind != b"IDAT":
            idat_finished = True
        offset = end
    if width is None or not idat or not saw_iend or offset != len(payload):
        raise ValidationError("incomplete PNG")
    if color_type == 3 and not saw_palette:
        raise ValidationError("indexed PNG has no palette")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    row_bytes = (width * channels * bit_depth + 7) // 8
    expected_size = height * (row_bytes + 1)
    if expected_size > MAX_DECODED_BYTES:
        raise ValidationError("decoded PNG exceeds the 2 MiB evidence limit")
    try:
        inflater = zlib.decompressobj()
        decoded = inflater.decompress(bytes(idat), expected_size + 1)
        if len(decoded) > expected_size or inflater.unconsumed_tail or not inflater.eof:
            raise ValidationError("image stream exceeds its declared geometry")
        decoded += inflater.flush()
    except zlib.error as error:
        raise ValidationError(f"invalid image stream: {error}") from error
    if inflater.unused_data or len(decoded) != expected_size:
        raise ValidationError("decoded image stream has the wrong size")
    for row in range(height):
        if decoded[row * (row_bytes + 1)] > 4:
            raise ValidationError("invalid PNG row filter")
    return width, height


def validate_readme(root: Path, readme: Path) -> list[str]:
    errors: list[str] = []
    text = readme.read_text(encoding="utf-8")
    references = README_IMAGE.findall(text)
    if len(references) < 4:
        errors.append("README must contain at least four local device screenshots")
    seen: set[str] = set()
    for reference in references:
        if reference.startswith(("http://", "https://", "data:")):
            errors.append(f"README image must be repository-local: {reference}")
            continue
        path = (readme.parent / reference).resolve()
        try:
            relative = path.relative_to(root.resolve()).as_posix()
        except ValueError:
            errors.append(f"README image escapes the repository: {reference}")
            continue
        if relative in seen:
            errors.append(f"README image is duplicated: {relative}")
            continue
        seen.add(relative)
        if not relative.startswith("artifacts/"):
            errors.append(f"README image is not curated under artifacts/: {relative}")
            continue
        if path.suffix.lower() != ".png" or not path.is_file():
            errors.append(f"README screenshot is missing or not PNG: {relative}")
            continue
        try:
            size = read_png_size(path)
        except (OSError, ValidationError) as error:
            errors.append(f"README screenshot does not decode: {relative}: {error}")
            continue
        if size != DEVICE_SIZE:
            errors.append(
                f"README screenshot has {size[0]}x{size[1]}, expected "
                f"{DEVICE_SIZE[0]}x{DEVICE_SIZE[1]}: {relative}"
            )
    return errors


def _section(body: str, name: str) -> str | None:
    match = re.search(
        rf"(?ims)^###\s+{re.escape(name)}\s*$\n(.*?)(?=^#{{2,3}}\s|\Z)", body
    )
    return match.group(1).strip() if match else None


def validate_pr_body(body: str, visual_sources_changed: bool = False,
                     screenshots_changed: bool = False) -> list[str]:
    errors: list[str] = []
    decision = re.search(r"(?im)^Visual change:\s*(yes|no)\s*$", body)
    if decision is None:
        errors.append("PR must set `Visual change: yes` or `Visual change: no`")
        return errors
    visual = decision.group(1).lower() == "yes"
    before = _section(body, "Before")
    after = _section(body, "After")
    if before is None or after is None:
        errors.append("PR must retain both `### Before` and `### After` sections")
    elif visual:
        before = re.sub(r"(?s)<!--.*?-->", "", before)
        after = re.sub(r"(?s)<!--.*?-->", "", after)
        if MARKDOWN_IMAGE.search(before) is None:
            errors.append("visual PR must include a Markdown image in Before")
        if MARKDOWN_IMAGE.search(after) is None:
            errors.append("visual PR must include a Markdown image in After")
    else:
        for name, section in (("Before", before), ("After", after)):
            section = re.sub(r"(?s)<!--.*?-->", "", section)
            if re.search(r"(?i)\bN/A\s*:\s*\S", section) is None:
                errors.append(f"non-visual PR must explain `N/A:` in {name}")
    if visual_sources_changed and not visual:
        errors.append("music_ui source changed, so `Visual change` must be `yes`")
    if visual_sources_changed and not screenshots_changed:
        errors.append("music_ui source changed without a refreshed artifacts/*.png screenshot")
    return errors


def changed_paths(root: Path, base_sha: str) -> set[str]:
    result = subprocess.run(
        ["git", "diff", "--diff-filter=AM", "--name-only", f"{base_sha}...HEAD"],
        cwd=root,
        check=True,
        text=True,
        capture_output=True,
    )
    return {line for line in result.stdout.splitlines() if line}


def validate_changed_screenshots(
    root: Path, paths: set[str], body: str, revisions: Collection[str] = ()
) -> tuple[bool, list[str]]:
    """Validate changed screenshots and require After to reference one.

    A github.com blob or raw.githubusercontent.com URL counts only when its
    revision is one of ``revisions`` (the PR head branch name or head SHA).
    The remote blob itself is not fetched, so its contents are not verified.
    """
    errors: list[str] = []
    candidates = sorted(
        path
        for path in paths
        if path.startswith("artifacts/") and path.lower().endswith(".png")
    )
    valid: list[str] = []
    for candidate in candidates:
        path = (root / candidate).resolve()
        try:
            path.relative_to(root.resolve())
        except ValueError:
            errors.append(f"changed screenshot escapes the repository: {candidate}")
            continue
        if not path.is_file():
            errors.append(f"changed screenshot is not an existing file: {candidate}")
            continue
        try:
            size = read_png_size(path)
        except (OSError, ValidationError) as error:
            errors.append(f"changed screenshot does not decode: {candidate}: {error}")
            continue
        if size != DEVICE_SIZE:
            errors.append(
                f"changed screenshot has {size[0]}x{size[1]}, expected "
                f"{DEVICE_SIZE[0]}x{DEVICE_SIZE[1]}: {candidate}"
            )
            continue
        valid.append(candidate)
    after = _section(body, "After") or ""
    after = re.sub(r"(?s)<!--.*?-->", "", after)
    targets = MARKDOWN_IMAGE_TARGET.findall(after)

    def references_candidate(target: str, candidate: str) -> bool:
        if target.removeprefix("./") == candidate:
            return True
        parsed = urlsplit(target)
        if parsed.scheme != "https":
            return False
        path = unquote(parsed.path)
        if parsed.netloc == "github.com":
            prefix = GITHUB_REPOSITORY + "blob/"
        elif parsed.netloc == "raw.githubusercontent.com":
            prefix = GITHUB_REPOSITORY
        else:
            return False
        return any(
            path == f"{prefix}{revision}/{candidate}"
            for revision in revisions
            if revision
        )

    if valid and not any(
        references_candidate(target, candidate)
        for target in targets
        for candidate in valid
    ):
        errors.append("After evidence does not reference a refreshed artifacts/*.png file")
    return bool(valid), errors


def validate_event(root: Path, event_path: Path) -> list[str]:
    event = json.loads(event_path.read_text(encoding="utf-8"))
    pull_request = event.get("pull_request")
    if not isinstance(pull_request, dict):
        return []
    base = pull_request.get("base", {}).get("sha")
    if not isinstance(base, str) or not base:
        return ["pull-request event has no base SHA"]
    head = pull_request.get("head") or {}
    revisions = [
        value
        for value in (head.get("ref"), head.get("sha"))
        if isinstance(value, str) and value
    ]
    paths = changed_paths(root, base)
    visual_sources_changed = bool(paths & VISUAL_SOURCES)
    body = pull_request.get("body") or ""
    screenshots_changed = False
    errors: list[str] = []
    if visual_sources_changed:
        screenshots_changed, screenshot_errors = validate_changed_screenshots(
            root, paths, body, revisions
        )
        errors.extend(screenshot_errors)
    errors.extend(
        validate_pr_body(body, visual_sources_changed, screenshots_changed)
    )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--event", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    errors = validate_readme(root, root / "README.md")
    if args.event is not None:
        errors.extend(validate_event(root, args.event))
    if errors:
        for error in errors:
            print(f"visual-evidence: {error}", file=sys.stderr)
        return 1
    print("visual-evidence: README screenshots and PR evidence are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
