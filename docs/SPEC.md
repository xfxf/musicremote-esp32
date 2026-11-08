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

### Advanced AIMP controls

- Gate equalizer, sleep timer, and computer actions on the server capability
  mask and clear their state on disconnect.
- Display and edit the complete reported manual equalizer, including preamp;
  reject malformed or out-of-range bodies before retaining or echoing them.
- Offer non-destructive sleep-timer choices for 15, 30, and 60 minutes or the
  end of the current track.
- Put computer sleep, restart, and shutdown behind a separate full-screen
  confirmation. Never send those actions from automated validation.

### Quality gates

- Build using the pinned ESP-IDF version and fit the configured app partition.
- Boot, connect, authenticate, and soak without panic on the physical board.
- Capture and inspect all four pages from the actual LVGL framebuffer.
- Exercise live state and controls against the configured AIMP machine while
  restoring deliberately changed state when practical.
- Open a pull request and request built-in Codex review with `@codex review`.

## Qualification boundary

Cover art, lyrics, media-library search, persistent settings, and the advanced
control surfaces are implemented. Equalizer, sleep-timer, and computer-power
wire layouts have static reference-client and official plug-in evidence plus
strict host tests and physical UI evidence. Live advanced-control confirmation
must remain explicitly pending whenever the configured AIMP host is offline;
computer-power actions are never part of automated validation.

