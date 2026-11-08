# Interface system

The interface borrows Zune's strengths—large type, flat planes, purposeful
accent colour, and information hierarchy—without copying branded assets.

- Canvas: `#090B10`; panels: `#11151E`.
- Primary accent: teal `#00D7C7`; progress accent: blue `#35A7FF`.
- Montserrat is provided by LVGL under SIL OFL 1.1 and compiled in the 14, 18,
  22, 28, 36, and 48 px sizes used by the interface.
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
  and Composers roots. Results render in 25-row UI pages; at most 2,000 results
  and 256 KiB of result text are retained even when AIMP reports a very large
  collection. The Search action reaches the complete AIMP media library when a
  desired item falls outside that bounded browse window.
- Tapping a library track appends it without clearing the playlist, starts
  playback, and returns directly to
  Now Playing. Long-pressing a track or library node enqueues it without
  changing pages. Whole-library search reuses the on-screen keyboard and keeps
  the AIMP request ID needed to select the correct result.
- Lyrics are requested on demand and shown in a scrollable overlay. Missing or
  oversized text produces a bounded status message rather than allocating an
  untrusted server length.
- Destructive operations use long press instead of confirmation dialogs to
  keep the small-screen flow predictable.
- The now-playing tile displays AIMP's current cover at its returned aspect
  ratio. A generated audio glyph remains visible while loading or when artwork
  is absent, and the rating stays usable as an overlaid touch label.

Cover JPEGs are decoded on a dedicated lower-priority task, never from the
LVGL or socket loop. The 144 px decoded RGB888 surface uses PSRAM. Successful
JPEGs are cached under the existing microSD-preferred storage directory; the
cache is capped at 128 entries/16 MiB on SD and 16 entries/512 KiB on internal
flash. Entries are evicted in approximate filesystem-timestamp order; the hard
entry and byte limits do not depend on the device having a real-time clock.

The physical panel is natively 320x480. LVGL rotates the display 90 degrees to
480x320; the board support package applies the corresponding touch transform.

