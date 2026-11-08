# Product specification

## Goal

Provide the SC01 Plus with the day-to-day remote-control surface expected from
a paid mobile AIMP remote, without requiring a phone: reliable transport
controls, state feedback, queue manipulation, and playlist management.

## Acceptance criteria

### Connectivity

- Connect to Wi-Fi using gitignored first-boot defaults, then persist editable
  Wi-Fi and AIMP host, port, and password settings in NVS without displaying or
  logging password values.
- Resolve an AIMP `.local` hostname, authenticate to the configured TCP port,
  and reconnect after interruption without blocking touch input.
- Show offline, connecting, authenticating, rejected, and online states.

### Playback

- Reflect play/pause/stop, position, duration, volume, shuffle, repeat, and
  current-track metadata.
- Retrieve, bound, decode, cache, and display current-track cover art without
  blocking LVGL or socket processing; retain a clean fallback for missing art.
- Control play, pause, stop, previous, next, seek, volume, shuffle, repeat, and
  volume from the touchscreen.

### Playlists and queue

- List playlists with active state and track count.
- Activate a playlist and render bounded 100-row pages. Stream queues too large
  for RAM into a persistent microSD-preferred index without retaining the full
  decoded collection in PSRAM.
- Browse indexed queues by A-Z title prefix and case-insensitive title/artist
  substring search while preserving the original AIMP row for playback.
- Play or remove an item; clear the active playlist; create a named playlist;
  and delete an inactive playlist.
- Require a long press for destructive operations.

### Interface

- Render at 480x320 landscape with LVGL and working rotated touch mapping.
- Use a near-black, high-contrast, type-led Zune-inspired visual system and
  touch targets near or above 44 pixels.
- Remain responsive while socket I/O, decompression, or reconnects occur.
- Provide persistent brightness and screen-off settings; a touch on a blanked
  display wakes it without activating the control underneath.

### Quality gates

- Build using the pinned ESP-IDF version and fit the configured app partition.
- Boot, connect, authenticate, and soak without panic on the physical board.
- Capture and inspect all four pages from the actual LVGL framebuffer.
- Exercise live state and controls against the configured AIMP machine while
  restoring deliberately changed state when practical.
- Open a pull request and request built-in Codex review with `@codex review`.

## Explicit follow-up scope

Cover art, lyrics, and media-library search are implemented. The server
capability mask is surfaced for diagnostics. Equalizer, sleep-timer, and
computer-power actions remain hidden until their command payloads and dedicated
confirmation flows are physically qualified; automated validation must never
invoke a destructive computer-power action.

