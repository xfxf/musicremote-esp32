# Interface system

The interface borrows Zune's strengths—large type, flat planes, purposeful
accent colour, and information hierarchy—without copying branded assets.

- Canvas: `#090B10`; panels: `#11151E`.
- Primary accent: teal `#00D7C7`; progress accent: blue `#35A7FF`.
- Text uses bitmap fonts derived from Microsoft's Selawik (SIL OFL 1.1), a
  Segoe UI metric-compatible face, pre-rendered to 4 bpp LVGL bitmap fonts
  holding every Selawik glyph (ASCII, Latin-1, most of Latin Extended-A and
  typographic punctuation such as ’ “ ” – …): Semibold for 28 px and 22 px
  titles, the now-playing album line and the bottom navigation labels, and
  Regular for other 14 and 18 px body text, buttons, and labels.
- Characters Selawik lacks fall back, at the same size, to Noto Sans and Noto
  Sans Math bitmap fonts (Latin Extended and Vietnamese, Greek, Cyrillic,
  punctuation, currency, arrows, common maths and music symbols), then to an
  optional CJK font that LVGL's tiny_ttf renders from
  `/sdcard/fonts/cjk.ttf`, then to Montserrat. Without that file, CJK text
  shows placeholder boxes. To install it, run `make load-data` with the board
  connected over USB, then reset the board.
  Its glyphs are cached in PSRAM after first use, so the first draw of a CJK
  screen reads the card and later redraws do not. See `main/fonts/README.md`
  for provenance, regeneration, the memory budget, and the Hangul gap in the
  recommended font.
- LVGL's bundled Montserrat (SIL OFL 1.1) remains compiled in at 14, 18, 22,
  28, 36, and 48 px for `LV_SYMBOL_*` glyphs, and the 48 px audio tile icon
  uses Montserrat directly.
- Pages share a 40 px status header and 46 px bottom navigation.
- Each bottom destination owns a contiguous 120x46 quarter of the navigation
  bar, including the lower display edge; no visual gutter is a dead touch zone.
- Compact buttons have four pixels of invisible click padding, and visible
  controls are at least 44 pixels high unless they are display-only.
- Lists use fixed-height rows, bounded text, ellipsis, and persistent scroll.
- Queue rows are constructed in small per-frame batches so loading a large
  playlist never starves touch handling or the ESP32 watchdog.
- Playlist folders open in place with a large Back control; protected AIMP
  playlists remain selectable but cannot be deleted from the device.
- Media Library opens as a full-screen surface with Artists, Albums, Genres,
  and Composers roots. Results render in 25-row UI pages; at most 1,000 results
  and 128 KiB of result text are retained from one response. A response whose
  wire or decoded size exceeds 512 KiB is drained and reported as `LIBRARY
  RESULT TOO LARGE` with no rows; narrow such views with Search. The Search
  action reaches the complete AIMP media library when a desired item falls
  outside the bounded browse window.
- Tapping a library track appends it without clearing the playlist, starts
  playback, and returns directly to
  Now Playing. Long-pressing a track enqueues it without
  changing pages. Whole-library search reuses the on-screen keyboard and keeps
  the AIMP request ID needed to select the correct result.
- Lyrics are requested on demand and shown in a scrollable overlay. Missing or
  oversized text produces a bounded status message rather than allocating an
  untrusted server length.
- Destructive operations use long press instead of confirmation dialogs to
  keep the small-screen flow predictable.
- The now-playing tile displays AIMP's current cover at its returned aspect
  ratio. A generated audio glyph remains visible while loading or when artwork
  is absent. Ratings are intentionally omitted from the device UI.
- Settings is a scrollable menu rather than a button grid. Connection values,
  playback actions, display controls, and storage/about details each open a
  dedicated full-screen section. Password rows are masked and password editors
  use concealed text.
- Brightness changes apply immediately and persist. The configurable blanking
  timer turns off the backlight; the first touch restores it and is consumed so
  the hidden control beneath is not triggered.

On a track change the previous cover stays on screen while the next one
loads, and it is replaced as soon as the new cover is decoded. The generic
tile appears only when AIMP reports no artwork or no cover arrives within
2 seconds, so tracks on the same album never flash a blank tile.

Cover JPEGs are decoded on a dedicated lower-priority task, never from the
LVGL or socket loop. The 144 px decoded RGB888 surface uses PSRAM. Successful
JPEGs are cached under the existing microSD-preferred storage directory; the
cache is capped at 128 entries/16 MiB on SD and 16 entries/512 KiB on internal
flash. Entries are evicted in approximate filesystem-timestamp order; the hard
entry and byte limits do not depend on the device having a real-time clock.

The physical panel is natively 320x480. LVGL rotates the display 90 degrees to
480x320 and rotates touch points to match.
The persistent FLIP SCREEN display setting selects 270 degrees instead, for
enclosures that mount the panel upside down. It applies immediately: LVGL's input
device rotates touch points from the display rotation, and the BSP reprograms
the ST7796 MADCTL from LVGL's resolution-changed event.
Diagnostic screenshots are LVGL snapshots and therefore always render upright.

