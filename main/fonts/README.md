# UI fonts

The `ui_font_*.c` files are LVGL 9 bitmap fonts derived from
Microsoft's Selawik typeface, licensed under the SIL Open Font License 1.1
(`Selawik-OFL.txt`). They are compiled into flash rodata; nothing renders a TTF
at run time.

## Provenance

- Repository: <https://github.com/microsoft/Selawik>
- Release: tag `1.01`, commit `b65ff9ce7c4673e75ec5c622924a6b6f61f2f876`
- Asset: <https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip>
  (SHA-256 `3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423`)
- License text: `LICENSE.txt` at the same commit, copied verbatim to
  `Selawik-OFL.txt`

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

Each font covers U+0020-U+007E and U+00A0-U+00FF (ASCII and Latin-1) at 4 bpp
without compression, which favors draw speed over flash. The font's
`.fallback` points to the LVGL Montserrat font of the same size. LVGL symbol
glyphs (`LV_SYMBOL_*`, from FontAwesome) and characters outside Latin-1 come
from that fallback, so those Montserrat sizes must stay enabled in
`sdkconfig.defaults`. The 48 px audio glyph uses `lv_font_montserrat_48`
directly.

Selawik's license declares "Selawik" a Reserved Font Name. These subsetted,
format-converted fonts are Modified Versions under the OFL, so they and their
C symbols (`ui_font_14` and the other sizes) deliberately do not use that
name. The source is credited here, as the license permits.

## Regenerating

Requires `curl`, `unzip`, `sha256sum`, and Node.js with `npx`. From the
repository root:

```sh
tools/generate_fonts.sh
```

The script downloads the release zip into `build/selawik-src/`, which git
ignores, and verifies the zip and TTF hashes above. It then runs these
commands with `lv_font_conv@1.5.3`:

```sh
npx --yes lv_font_conv@1.5.3 --font build/selawik-src/selawk.ttf -r 0x20-0x7E -r 0xA0-0xFF --size 14 --format lvgl --bpp 4 --no-compress --lv-fallback lv_font_montserrat_14 --lv-font-name ui_font_14 -o main/fonts/ui_font_14.c
npx --yes lv_font_conv@1.5.3 --font build/selawik-src/selawksb.ttf -r 0x20-0x7E -r 0xA0-0xFF --size 14 --format lvgl --bpp 4 --no-compress --lv-fallback lv_font_montserrat_14 --lv-font-name ui_font_14_semibold -o main/fonts/ui_font_14_semibold.c
npx --yes lv_font_conv@1.5.3 --font build/selawik-src/selawk.ttf -r 0x20-0x7E -r 0xA0-0xFF --size 18 --format lvgl --bpp 4 --no-compress --lv-fallback lv_font_montserrat_18 --lv-font-name ui_font_18 -o main/fonts/ui_font_18.c
npx --yes lv_font_conv@1.5.3 --font build/selawik-src/selawksb.ttf -r 0x20-0x7E -r 0xA0-0xFF --size 22 --format lvgl --bpp 4 --no-compress --lv-fallback lv_font_montserrat_22 --lv-font-name ui_font_22 -o main/fonts/ui_font_22.c
npx --yes lv_font_conv@1.5.3 --font build/selawik-src/selawksb.ttf -r 0x20-0x7E -r 0xA0-0xFF --size 28 --format lvgl --bpp 4 --no-compress --lv-fallback lv_font_montserrat_28 --lv-font-name ui_font_28 -o main/fonts/ui_font_28.c
```

The generated files include `lvgl/lvgl.h` unless `LV_LVGL_H_INCLUDE_SIMPLE` is
defined. `main/CMakeLists.txt` defines it, because the managed LVGL component
is not installed under an `lvgl/` directory.
