#!/usr/bin/env bash
# Regenerate the LVGL bitmap fonts in main/fonts/:
#   ui_font_<size>      every glyph of the official microsoft/Selawik 1.01
#                       release (Reserved Font Name, so not named Selawik);
#   ui_font_ext_<size>  European, punctuation and symbol fallback from Noto
#                       Sans and Noto Sans Math, minus Selawik's glyphs.
# See main/fonts/README.md for provenance and the fallback chain.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

lv_font_conv="lv_font_conv@1.5.3"

# Fetch url to path unless present, then verify its SHA-256.
fetch() {
    local url="$1" path="$2" sha256="$3"
    if [ ! -f "${path}" ]; then
        curl -fsSL -o "${path}.part" "${url}"
        mv "${path}.part" "${path}"
    fi
    echo "${sha256}  ${path}" | sha256sum -c -
}

selawik_dir="build/selawik-src"
mkdir -p "${selawik_dir}"
fetch "https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip" \
    "${selawik_dir}/Selawik_Release.zip" \
    3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423
unzip -o -q -j "${selawik_dir}/Selawik_Release.zip" selawk.ttf selawksb.ttf -d "${selawik_dir}"
sha256sum -c - <<EOF
e9d98518d8ac2817782a9a382430463a2e0793ea68350b695bb727d9a830ee1c  ${selawik_dir}/selawk.ttf
0a9e9d0549a10f24bef9b3a29e06fe6e0b5c21e7a784c503b048a307841a7783  ${selawik_dir}/selawksb.ttf
EOF

# Noto Sans 2.015 and Noto Sans Math 3.000, unhinted, from the official Noto
# distribution repository. NotoSans-Regular.ttf is byte-identical to the one
# in the NotoSans-v2.015 release of notofonts/latin-greek-cyrillic.
noto_dir="build/noto-src"
noto_commit="28b15b4b43b7bed62b5cf6e6b0b5ff5846270535"
noto_base="https://raw.githubusercontent.com/notofonts/notofonts.github.io/${noto_commit}/fonts"
mkdir -p "${noto_dir}"
fetch "${noto_base}/NotoSans/unhinted/ttf/NotoSans-Regular.ttf" \
    "${noto_dir}/NotoSans-Regular.ttf" \
    f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8
fetch "${noto_base}/NotoSansMath/unhinted/ttf/NotoSansMath-Regular.ttf" \
    "${noto_dir}/NotoSansMath-Regular.ttf" \
    b127e84699212b6b2ef50aff58e0ebebeec04ffe6db1b9eb9e209c8c3d97b4aa

# Selawik's complete cmap (U+000D aside), as fontTools reports it for both
# selawk.ttf and selawksb.ttf.
selawik_cmap=(
    0x20-0x7E 0xA1-0xA9 0xAB-0xB9 0xBB-0x107 0x10A-0x113 0x116-0x11B
    0x11E-0x123 0x126-0x127 0x12A-0x12B 0x12E-0x133 0x136-0x137 0x139-0x148
    0x14A-0x14D 0x150-0x15B 0x15E-0x167 0x16A-0x16B 0x16E-0x17E 0x192
    0x218-0x21B 0x237 0x2C6-0x2C7 0x2D8-0x2DD 0x300-0x304 0x306-0x308
    0x30A-0x30C 0x312 0x326-0x328 0x335-0x336 0x1E80-0x1E85 0x1EF2-0x1EF3
    0x2013-0x2014 0x2018-0x201A 0x201C-0x201E 0x2020-0x2022 0x2026 0x2030
    0x2039-0x203A 0x20AC 0x2122
)
# Also request U+00A0, which Selawik lacks: lv_font_conv skips a missing
# codepoint inside a range (it rejects a range with none), so ui_font_ext
# supplies it.
selawik_ranges=("${selawik_cmap[@]/#0xA1-0xA9/0xA0-0xA9}")

# Noto Sans blocks: Latin-1 letters Selawik lacks, Latin Extended-A/B, IPA,
# spacing modifiers, combining diacritics (for decomposed text), Greek and
# Coptic, Cyrillic and Cyrillic Supplement, Latin Extended Additional
# (Vietnamese), General Punctuation, superscripts and subscripts, currency,
# letterlike symbols, number forms, BOM and the replacement character.
noto_sans_wanted=(
    0xA0-0xFF 0x100-0x24F 0x250-0x2AF 0x2B0-0x2FF 0x300-0x36F 0x370-0x3FF
    0x400-0x52F 0x1E00-0x1EFF 0x2000-0x206F 0x2070-0x209F 0x20A0-0x20CF
    0x2100-0x214F 0x2150-0x218F 0xFEFF 0xFFFC-0xFFFD
)

# Noto Sans Math, which matches Noto Sans: arrows, common mathematical
# operators, geometric shapes, and the stars, card suits, music notes and
# check mark that turn up in track titles. Noto Sans has none of these.
noto_math_wanted=(
    0x2190-0x21FF
    0x2200-0x220B 0x220F-0x221E 0x2227-0x222B 0x2234-0x2235 0x223C 0x2245
    0x2248 0x2260-0x2265 0x226A-0x226B 0x2282-0x2287 0x2295 0x2297 0x22A5
    0x22C5
    0x25A0-0x25A1 0x25AA-0x25AB 0x25B2-0x25B3 0x25B6-0x25B7 0x25BA-0x25BD
    0x25C0-0x25C1 0x25C4-0x25C7 0x25CA-0x25CB 0x25CF 0x25E6 0x25EF
    0x2605-0x2606 0x2640 0x2642 0x2660-0x2667 0x2669-0x266F 0x2713
)

# Print each range of $2.. as "-r <range>", minus the ranges in $1 (a
# space-separated list), so ui_font_ext never duplicates a Selawik glyph.
ranges_minus() {
    local exclude="$1"
    shift
    python3 -I - "${exclude}" "$@" <<'PY'
import sys

def parse(text):
    first, _, last = text.partition("-")
    return int(first, 16), int(last or first, 16)

excluded = set()
for text in sys.argv[1].split():
    first, last = parse(text)
    excluded.update(range(first, last + 1))
wanted = sorted({c for text in sys.argv[2:]
                 for c in range(parse(text)[0], parse(text)[1] + 1)} - excluded)
runs = []
for c in wanted:
    if runs and runs[-1][1] == c - 1:
        runs[-1][1] = c
    else:
        runs.append([c, c])
for first, last in runs:
    print("-r", f"0x{first:X}" if first == last else f"0x{first:X}-0x{last:X}")
PY
}

selawik_args=()
for range in "${selawik_ranges[@]}"; do
    selawik_args+=(-r "${range}")
done
read -r -a noto_sans_args <<<"$(ranges_minus "${selawik_cmap[*]}" "${noto_sans_wanted[@]}" | tr '\n' ' ')"
read -r -a noto_math_args <<<"$(ranges_minus "${selawik_cmap[*]}" "${noto_math_wanted[@]}" | tr '\n' ' ')"

# size  ttf            name                  (weight: use)
selawik_fonts=(
    "14 selawk.ttf   ui_font_14"           # Regular: body text, row details
    "14 selawksb.ttf ui_font_14_semibold"  # Semibold: navigation, album line
    "18 selawk.ttf   ui_font_18"           # Regular: buttons, labels, list titles
    "22 selawksb.ttf ui_font_22"           # Semibold: overlay and page titles
    "28 selawksb.ttf ui_font_28"           # Semibold: now-playing title, SETTINGS
)

for entry in "${selawik_fonts[@]}"; do
    read -r size ttf name <<<"${entry}"
    npx --yes "${lv_font_conv}" \
        --font "${selawik_dir}/${ttf}" "${selawik_args[@]}" \
        --size "${size}" --format lvgl --bpp 4 --no-compress \
        --lv-fallback "ui_font_ext_${size}" \
        --lv-font-name "${name}" \
        -o "main/fonts/${name}.c"
done

# ui_font_cjk_<size> is defined in main/cjk_font.cpp: Montserrat (for the
# LV_SYMBOL_* glyphs) until /sdcard/fonts/cjk.ttf loads.
for size in 14 18 22 28; do
    npx --yes "${lv_font_conv}" \
        --font "${noto_dir}/NotoSans-Regular.ttf" "${noto_sans_args[@]}" \
        --font "${noto_dir}/NotoSansMath-Regular.ttf" "${noto_math_args[@]}" \
        --size "${size}" --format lvgl --bpp 4 --no-compress \
        --lv-fallback "ui_font_cjk_${size}" \
        --lv-font-name "ui_font_ext_${size}" \
        -o "main/fonts/ui_font_ext_${size}.c"
done

# lv_font_conv quotes each character in a comment. Drop the quoted character
# for format and control characters (bidirectional marks and the like), which
# GCC rightly flags (-Wbidi-chars) and which are invisible in review anyway.
python3 -I - main/fonts/ui_font_*.c <<'PY'
import re
import sys
import unicodedata

comment = re.compile(r'/\* U\+([0-9A-F]+) ".*" \*/')

def plain(match):
    char = chr(int(match.group(1), 16))
    if unicodedata.category(char) in ("Cc", "Cf", "Zl", "Zp"):
        return f"/* U+{match.group(1)} */"
    return match.group(0)

for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as source:
        text = source.read()
    with open(path, "w", encoding="utf-8") as output:
        output.write(comment.sub(plain, text))
PY
