# UI fonts

UI text resolves each character through a three-tier fallback chain, the same
at every UI size (14, 18, 22 and 28 px; the Semibold 14 shares the Regular 14
chain):

1. `ui_font_<size>`: every glyph of Microsoft's Selawik, as LVGL bitmap fonts
   compiled into flash.
2. `ui_font_ext_<size>`: European, punctuation and symbol glyphs that Selawik
   lacks, from Noto Sans and Noto Sans Math, also compiled into flash.
3. `ui_font_cjk_<size>`: CJK glyphs that LVGL's tiny_ttf renders at run time
   from `/sdcard/fonts/cjk.ttf`, when the card holds that file.
4. LVGL's Montserrat at the same size, for the `LV_SYMBOL_*` (FontAwesome)
   icons. Without the CJK font, `ui_font_cjk_<size>` is itself a copy of
   Montserrat.

A character that no tier has draws as a placeholder box.

The generated fonts are const, so their `.fallback` is fixed when they are
generated. `ui_font_ext_<size>` names `ui_font_cjk_<size>`, which
`main/cjk_font.cpp` defines as a mutable `lv_font_t`. It starts as a copy of
Montserrat without a fallback. When the card's font loads at boot, before the
UI exists, it becomes a copy of the tiny_ttf font that falls back to
Montserrat. LVGL fetches each glyph's bitmap through the font that resolved it
(`resolved_font` in `lv_font_get_glyph_dsc`), so the copy serves both the
metrics and the bitmap.

The 48 px audio glyph uses `lv_font_montserrat_48` directly. The Montserrat
sizes in `sdkconfig.defaults` must stay enabled.

## Tier 1: Selawik-derived bitmap fonts

- Repository: <https://github.com/microsoft/Selawik>
- Release: tag `1.01`, commit `b65ff9ce7c4673e75ec5c622924a6b6f61f2f876`
- Asset: <https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip>
  (SHA-256 `3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423`)
- License: SIL Open Font License 1.1. `LICENSE.txt` at the same commit is
  copied verbatim to `Selawik-OFL.txt`.

| Generated file          | Size  | Source TTF     | Weight    | SHA-256 of TTF |
|-------------------------|-------|----------------|-----------|----------------|
| `ui_font_14.c`  | 14 px | `selawk.ttf`   | Regular   | `e9d98518d8ac2817782a9a382430463a2e0793ea68350b695bb727d9a830ee1c` |
| `ui_font_14_semibold.c` | 14 px | `selawksb.ttf` | Semibold | `0a9e9d0549a10f24bef9b3a29e06fe6e0b5c21e7a784c503b048a307841a7783` |
| `ui_font_18.c`  | 18 px | `selawk.ttf`   | Regular   | `e9d98518d8ac2817782a9a382430463a2e0793ea68350b695bb727d9a830ee1c` |
| `ui_font_22.c`  | 22 px | `selawksb.ttf` | Semibold  | `0a9e9d0549a10f24bef9b3a29e06fe6e0b5c21e7a784c503b048a307841a7783` |
| `ui_font_28.c`  | 28 px | `selawksb.ttf` | Semibold  | `0a9e9d0549a10f24bef9b3a29e06fe6e0b5c21e7a784c503b048a307841a7783` |

The 28 px now-playing and page titles, the 22 px overlay titles, the album line
and the bottom navigation labels use Semibold. The owner reviewed Light and
Semilight titles on the physical panel and preferred bolder titles. Body text
at 14 and 18 px uses Regular for legibility.

Each font holds all 347 printable characters in Selawik's cmap (both weights
have the same cmap). These are ASCII; Latin-1 apart from U+00A0, U+00AA and
U+00BA; most of Latin Extended-A; Romanian comma-below letters; common
combining accents; the Welsh letters Ẁ ẁ Ẃ ẃ Ẅ ẅ Ỳ ỳ; and the typographic
punctuation in General Punctuation (’ ‘ ‚ “ ” „ – — † ‡ • … ‰ ‹ ›), € and ™.
They are 4 bpp without compression, which favors draw speed over flash.

Selawik's license declares "Selawik" a Reserved Font Name. These subsetted,
format-converted fonts are Modified Versions under the OFL, so they and their
C symbols (`ui_font_14` and the other sizes) deliberately do not use that
name. The source is credited here, as the license permits.

## Tier 2: Noto extended fallback

`ui_font_ext_14.c`, `ui_font_ext_18.c`, `ui_font_ext_22.c` and
`ui_font_ext_28.c` are 4 bpp bitmap fonts without compression, merged from two
Regular-weight sources. They omit every codepoint that Selawik has, so they
never duplicate a tier 1 glyph. At 22 and 28 px, these Regular glyphs sit
beside Semibold Selawik text.

| Source TTF | Version | SHA-256 |
|------------|---------|---------|
| `NotoSans-Regular.ttf` (unhinted) | 2.015 | `f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8` |
| `NotoSansMath-Regular.ttf` (unhinted) | 3.000 | `b127e84699212b6b2ef50aff58e0ebebeec04ffe6db1b9eb9e209c8c3d97b4aa` |

Both come from the official Noto distribution repository,
<https://github.com/notofonts/notofonts.github.io>, at commit
`28b15b4b43b7bed62b5cf6e6b0b5ff5846270535`, under
`fonts/NotoSans/unhinted/ttf/` and `fonts/NotoSansMath/unhinted/ttf/`.
`NotoSans-Regular.ttf` is byte-identical to the one in the `NotoSans-v2.015`
release asset of <https://github.com/notofonts/latin-greek-cyrillic>
(`NotoSans-v2.015.zip`, SHA-256
`0c34df072a3fa7efbb7cbf34950e1f971a4447cffe365d3a359e2d4089b958f5`).

Coverage, minus Selawik's glyphs:

- From Noto Sans: Latin-1 (U+00A0, ª, º); Latin Extended-A and -B; IPA
  Extensions; Spacing Modifier Letters; Combining Diacritical Marks, for
  decomposed (NFD) tags; Greek and Coptic; Cyrillic and Cyrillic Supplement;
  Latin Extended Additional, including all of Vietnamese; General Punctuation,
  including zero-width and directional marks; Superscripts and Subscripts;
  Currency Symbols; Letterlike Symbols; Number Forms; U+FEFF and U+FFFC-U+FFFD.
- From Noto Sans Math, which Noto Sans lacks: all of Arrows (U+2190-U+21FF),
  common Mathematical Operators (∀ ∂ ∃ ∅ ∆ ∇ ∈ ∉ ∏ ∑ − √ ∞ ∧ ∨ ∩ ∪ ∫ ≈ ≠ ≡ ≤ ≥
  ⊂ ⊃ and similar), common Geometric Shapes (■ □ ▲ ▶ ▼ ◀ ◆ ○ ● and
  similar), ★ ☆ ♀ ♂, card suits, music notes and signs (♩ ♪ ♫ ♬ ♭ ♮ ♯), and ✓.

The tools/generate_fonts.sh arrays `noto_sans_wanted` and `noto_math_wanted`
list the exact ranges. Greek Extended (polytonic), the Cyrillic and Latin
Extended-C/D/E blocks, and other Noto Sans blocks are left out to save flash.

### License and naming

Both fonts are licensed under the SIL Open Font License 1.1, copied verbatim
from each project's `OFL.txt` to `NotoSans-OFL.txt` (latin-greek-cyrillic at
tag `NotoSans-v2.015`) and `NotoSansMath-OFL.txt` (notofonts/math at tag
`NotoSansMath-v3.000`). Neither license's copyright line declares a Reserved
Font Name. The outputs are still named neutrally (`ui_font_ext_*`), matching
the other fonts.

## Tier 3: CJK from the microSD card

Fonts this large cannot be compiled in. Instead, LVGL's tiny_ttf
(`CONFIG_LV_USE_TINY_TTF`, `CONFIG_LV_TINY_TTF_FILE_SUPPORT`) streams a
TrueType file from the card. The firmware never ships this font; the owner
copies it to the card.

The recommended font is Droid Sans Fallback Full (`DroidSansFallbackFull.ttf`,
Copyright 2005-2008 The Android Open Source Project, Apache License 2.0). It
uses TrueType `glyf` outlines and is 4,033,576 bytes. Its 28,624 characters
cover the CJK Unified Ideographs (20,902) and Extension A (6,582), hiragana,
katakana, CJK punctuation, and full-width forms.

**It has only three precomposed Hangul syllables,** so Korean text still shows
boxes. (Checked with fontTools; AOSP shipped Hangul in separate fonts.) It also
has no Latin, Greek or Cyrillic, which the earlier tiers supply.

- Source: AOSP `platform/frameworks/base`, `data/fonts/DroidSansFallbackFull.ttf`,
  at commit `4e43ad12e1b211f15152e9c5b16b0fe88e6b93f3` (tag
  `android-15.0.0_r1`; git blob `68641aad1d1b02922fc1ca61fdcd119cb051ef06`).
- URL: <https://android.googlesource.com/platform/frameworks/base/+/4e43ad12e1b211f15152e9c5b16b0fe88e6b93f3/data/fonts/DroidSansFallbackFull.ttf?format=TEXT>
  (Gitiles serves it base64-encoded).
- SHA-256 of the decoded TTF:
  `2392015530438bafc48edfc4aee6d9de2387f627a6134d8ab3dfcc99d21c8240`.
- License: Apache 2.0, stated in `data/fonts/NOTICE` at the same commit.

### Putting the font on the card

With the board connected over USB and running (close `idf.py monitor` first),
from an ESP-IDF shell:

```sh
make load-data
```

This downloads the font from the pinned URL with `tools/fetch_cjk_font.sh`,
checks its SHA-256 and size, and uploads it over the USB console to
`/sdcard/fonts/cjk.ttf` (the loader, issue #49, verifies it on the device and
skips an identical file). Reset the board afterwards: the font is opened at
boot.

Alternatively, with the card on a computer (for example at
`/media/$USER/SDCARD`), run `tools/fetch_cjk_font.sh
/media/$USER/SDCARD/fonts/cjk.ttf`. The firmware's FAT driver uses 8.3 names
only, so keep the name `cjk.ttf`.

Any TrueType (`glyf`) font at that path is used. tiny_ttf's stb_truetype has a
CFF path, but the CFF-based Noto CJK OTFs have not been tested with streaming,
so prefer a `glyf` font.

### Boot behavior

`musicremote::load_cjk_font()` runs in `app_main` after the card mounts and
before the UI is created, so no other task is using LVGL. It logs one line:

- `cjk-font: microSD not mounted; ...` or `cjk-font: /sdcard/fonts/cjk.ttf not
  found; ...`: boot continues with CJK text as boxes.
- `cjk-font: Loaded /sdcard/fonts/cjk.ttf (3940 KB) at 14/18/22/28 px; PSRAM
  cache up to 315 KB; internal RAM free N KB, PSRAM free M KB`.
- A warning if the file is empty, oversized (over 32 MB), unreadable, or not a
  font stb_truetype accepts; boot continues without it.

The card's file is untrusted input to stb_truetype. All font reads go through
a bounds-checked block cache (`cjk_font_policy.hpp`): reads past the end of the
file, or failed reads, return zeros instead of stale or out-of-bounds memory.
stb_truetype is not hardened against hostile fonts, so use the verified file.

### Memory

| Item | Where | Size |
|------|-------|------|
| Block cache (64 x 1 KB of the file) | PSRAM | 64 KB |
| Glyph caches: 96 glyphs at 14 and 18 px, 32 at 22 and 28 px | PSRAM | up to 251 KB |
| Per-size font state (stb_truetype info, caches' bookkeeping) | PSRAM | about 2 KB |
| Rasteriser scratch while rendering one glyph | PSRAM, freed after | a few KB |
| Driver state, proxies and handles | internal `.bss` | under 0.5 KB |
| GUI task stack during rasterisation | existing 16 KB stack | 1 to 2 KB more |

The worst case is about 315 KB of PSRAM, which `make check` asserts stays below
384 KB. It adds no run-time internal RAM, unless PSRAM is exhausted, in which
case allocations fall back to internal RAM instead of failing.

LVGL allocates through `main/lvgl_allocator.cpp`
(`CONFIG_LV_USE_CUSTOM_MALLOC`), which prefers PSRAM for every LVGL block, so
everything tiny_ttf allocates (cache entries, glyph bitmaps, and stb_truetype's
rasteriser buffers) lands there too.

A custom lv_fs driver (drive `F:`) serves the file to all four sizes through
one descriptor and the shared block cache. Four `fopen()` handles would hold
half the card's eight file slots. Kerning is off
(`CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=0`); this font has no pair kerning.

### Performance

A glyph is rendered once per size and then drawn from the cache. A cache miss
reads one to nine 1 KB blocks from the card; table data the lookups share
(cmap, GPOS) stays cached. On the host, rendering one ideograph at 28 px took
63 to 613 stb_truetype stream reads but at most nine block reads. LV_SYMBOL
icons skip the TTF. If one screen shows more distinct CJK characters than a
size's cache holds, each redraw re-renders some of them; raise `cache_glyphs`
in `cjk_font_policy.hpp` if scrolling a CJK list stutters.

## Regenerating

Requires `curl`, `unzip`, `sha256sum`, `python3`, and Node.js with `npx`.
From the repository root:

```sh
tools/generate_fonts.sh
```

The script downloads the Selawik release zip into `build/selawik-src/` and the
two Noto TTFs into `build/noto-src/` (git ignores both), and verifies every
hash above. It then runs `lv_font_conv@1.5.3` once per output file. Each
generated file's header records its exact command. Every font is 4 bpp,
`--no-compress`, `--format lvgl`:

- `ui_font_<size>`: `--font selawk.ttf` or `selawksb.ttf`, with `-r` ranges for
  Selawik's cmap plus U+00A0, and `--lv-fallback ui_font_ext_<size>`.
- `ui_font_ext_<size>`: `--font NotoSans-Regular.ttf` with its ranges minus
  Selawik's cmap, then `--font NotoSansMath-Regular.ttf` with its ranges, and
  `--lv-fallback ui_font_cjk_<size>`.

lv_font_conv quotes each character in a comment. The script then strips that
character for control and format characters, such as bidirectional marks,
which GCC flags with `-Wbidi-chars`.

The generated files include `lvgl/lvgl.h` unless `LV_LVGL_H_INCLUDE_SIMPLE` is
defined. `main/CMakeLists.txt` defines it, because the managed LVGL component
is not installed under an `lvgl/` directory.

Changing `sdkconfig.defaults` does not alter an existing `sdkconfig`. Run
`tools/configure.sh` (`idf.py set-target esp32s3`) to regenerate it. A stale
`sdkconfig` stops the build with an `#error` that names this step.
