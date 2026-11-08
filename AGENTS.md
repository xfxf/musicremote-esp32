# Agent guidance

This repository contains ESP-IDF firmware for the Smart Panlee / Wireless-Tag
WT32-SC01 Plus (ESP32-S3, 480x320 ST7796, FT5x06 touch). The product is an
independent AIMP remote that interoperates with the MonkeyMote AIMP plugin.

## Non-negotiable rules

- Never commit `.env`, Wi-Fi credentials, AIMP passwords, captured traffic that
  contains credentials, or generated `sdkconfig` files.
- Build with the ESP-IDF version documented in `README.md`; do not silently
  change the target board or pin mapping.
- Keep all LVGL calls on the GUI task or under the GUI mutex.
- Keep socket I/O off the GUI task. UI callbacks enqueue commands; the network
  task owns connection, authentication, parsing, retries, and keepalive.
- Protocol integers are little-endian. Every packet is `magic`, `type`, and
  `payload_length` (three `uint32_t` fields), followed by the payload.
- Treat all lengths received from the network as untrusted and bound them before
  allocating or copying.
- Preserve user changes. Check `git status` before editing and do not overwrite
  unrelated work.

## Required validation before a push

1. `tools/configure.sh`
2. `idf.py build`
3. `tools/protocol_probe.py --env .env state` against the live AIMP host when
   it is available.
4. Flash and monitor the physical board when `/dev/ttyACM0` is available.
5. Exercise playback, volume, seek, shuffle/repeat, playlist selection, and a
   playlist edit against AIMP. Restore any deliberately changed volume or
   playback state when practical.
6. Capture and visually inspect the UI diagnostic screenshot. Store reviewed
   captures under `artifacts/`. Refresh affected README screenshots whenever a
   shipped UI change makes them stale, then run `make check-visual`.

## Product conventions

- Landscape UI is 480x320. Use large touch targets (minimum 44 px), high contrast,
  restrained motion, and the Zune-inspired typographic hierarchy in `docs/UI.md`.
- Use LVGL's bundled Montserrat fonts (SIL OFL 1.1) unless a replacement is
  explicitly reviewed with its license.
- Connection loss must never freeze the UI. Show stale/offline state and retry
  with bounded exponential backoff.
- Protocol research notes and packet IDs live in `docs/PROTOCOL.md`; update that
  document whenever behavior is learned from the live server.
- Specifications are tracked as GitHub issues. Reference the issue in each PR
  and keep commits narrowly aligned with an acceptance criterion.
- Every PR body must complete the Before and After sections in the repository
  template. Visual changes require device-rendered images in both; non-visual
  changes require an explicit `N/A:` explanation in both. UI-source changes
  must commit refreshed PNG evidence under `artifacts/`.

## Code Review Rules

### Review workflow

- For each implementation pull request, run one adversarial review in a
  dedicated subagent after the build, tests, and hardware evidence are ready.
- The reviewer must inspect the diff and validation evidence for correctness,
  protocol safety, bounded memory use, responsive touch behavior, secrets, and
  regressions on the physical device.
- Resolve all actionable findings before merge. A single focused follow-up
  review may verify fixes for blocking findings; do not run repeated broad or
  token-heavy review loops.
- Record the reviewer outcome on the pull request. Built-in pull-request review
  may supplement this workflow, but it is not a prerequisite for local work.
- Do not run `ccr-local` as a routine review step. It is permitted only when an
  actual merge gate requires it, for any pull request; use the Codex engine,
  disable engine fallback, and run the minimum rounds needed to satisfy that
  gate.

### Protocol compatibility

- Flag any packet parser that trusts remote counts, lengths, or decompressed
  sizes without proving they fit the received buffer and the documented limit.
  Safe path: validate before allocation, indexing, or copying and reject the
  entire malformed frame without mutating the model.
- Flag commands whose wire type, subcommand, integer width, or byte order
  diverges from `docs/PROTOCOL.md`. Safe path: update both implementation and
  protocol evidence together, then validate against the live AIMP plug-in.

### Secrets and diagnostics

- Flag credentials, authentication digests, private LAN captures, or generated
  credential-bearing build configuration entering git or logs. Safe path: keep
  secrets only in ignored `.env`/build outputs and redact diagnostic output.

### Hardware evidence

- Flag UI, display orientation, touch mapping, or memory-allocation changes that
  claim device readiness without a physical-board build/flash/soak and reviewed
  device-rendered screenshots. Safe path: record the evidence in `docs/TESTING.md`.
- Flag visual PRs whose before/after evidence is missing, stale, simulated, or
  inconsistent with the README gallery. CI validates evidence structure and
  image integrity; the reviewer remains responsible for visual inspection.

