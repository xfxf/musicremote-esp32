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
- UI text uses the reviewed bitmap fonts in `main/fonts/`, derived from
  Selawik (SIL OFL 1.1; provenance and regeneration in `main/fonts/README.md`).
  "Selawik" is a Reserved Font Name, so the converted fonts and their symbols
  must never be named Selawik. They fall back to the reviewed Noto Sans and
  Noto Sans Math bitmap fonts (`ui_font_ext_*`, SIL OFL 1.1), then to the
  optional microSD CJK font (Droid Sans Fallback Full, Apache 2.0, never
  committed), then to LVGL's bundled Montserrat (SIL OFL 1.1), which stays
  enabled for `LV_SYMBOL_*` glyphs. Any other typeface must be explicitly
  reviewed with its license.
- Connection loss must never freeze the UI. Show stale/offline state and retry
  with bounded exponential backoff.
- Protocol research notes and packet IDs live in `docs/PROTOCOL.md`; update that
  document whenever behavior is learned from the live server.
- Specifications are tracked as GitHub issues. Reference the issue in each PR
  and keep commits narrowly aligned with an acceptance criterion. See the ship
  workflow below.
- Every PR body must complete the Before and After sections in the repository
  template. Visual changes require device-rendered images in both; non-visual
  changes require an explicit `N/A:` explanation in both. UI-source changes
  must commit refreshed PNG evidence under `artifacts/`.

## Ship workflow

Every change, including one requested in conversation, follows these steps.
"Ship" means all of them.

1. **Issue.** Find or file a GitHub issue using
   `.github/ISSUE_TEMPLATE/feature.yml`, with outcome, acceptance criteria,
   and state restoration/safety. A request that arrives in conversation
   becomes an issue before any code is written.
2. **Branch** from up-to-date `main`, named for the issue (for example
   `claude/31-flip-screen`).
3. **Implement and validate** using the required validation steps above.
   Ask the owner to perform physical touch interactions you cannot do
   yourself, and record what they confirmed.
4. **Evidence.** Commit reviewed device captures under `artifacts/`, and
   record hardware qualification in `docs/TESTING.md`.
5. **Pull request** from the template, with `Closes #<issue>` (or
   `Refs #<issue>` when the issue has remaining work) and completed Before
   and After sections.
6. **Review** with ccr-local per the review workflow below. Answer and resolve
   every thread.
7. **Merge** once CI passes and review is settled. Delete the branch, update
   local `main`, and flash the merged build when the device is attached.
   Confirm that completed issues are closed; leave issues with remaining work
   open.

## Code Review Rules

### Review workflow

Pull requests are reviewed by ccr-local (a local, Copilot-like review). The
built-in `@codex review` integration is not connected to this repository and
never responds; do not request it or count it as a review.

The `main` ruleset enforces the gate. Direct pushes, force-pushes, and branch
deletion are blocked, and merging requires a pull request, the `validate` CI
check, a `ccr-local/review` status on the exact PR head, and every review
thread resolved. Only merge commits are allowed, and merged branches are
deleted automatically.

- Once the build, tests, and hardware evidence are ready and the PR head is
  pushed, check out exactly that head with nothing uncommitted and review it:
  `"${CCR_HOME:-$HOME/tools/ccr-local}/ccr" review --pr <N> --post --engine codex --no-engine-fallback`
  Run it in the background; it takes several minutes.
- **One review round is the default.** If it finds nothing, merge. If you push
  fixes for its findings, run one follow-up review of those fixes so the new
  head carries the required status. The follow-up is incremental and cheap,
  and it is round 2.
- **Further rounds** are owed only while the latest round reports a
  `critical` finding in a file matching `final_review.paths` in `.ccr.json`.
  Fix it, push, and review again. When ccr-local exits 6 (a final full
  review is owed), rerun with `--final`; that counts as a round.
  `.ccr.json` sets `max_rounds` to 3. If a critical finding in a critical
  file survives the third round, stop and ask the owner. Do not raise the
  cap yourself on an open PR.
- **Retroactive reviews** of already-merged PRs, which the owner approved
  for PRs that merged without a ccr-local review, may raise the cap for one
  run with `AGENT_REVIEW_MAX_ROUNDS=20`. Post the reason on that PR first.
  Pass the PR's pre-merge base with `--base <sha>`; the default
  `main...head` diff is empty once the PR is merged. Fix the resulting
  findings on `main` through a new PR. Critical findings must be fixed;
  moderate and nit findings may be fixed in the same PR or deferred.
- Answer every review thread with the action taken, then resolve it:
  `Fixed in <commit>`, `Not actioned: <reason>`, `Won't fix: <reason>`,
  `Deferred to #<issue>`, or `Resolving after: <what and when>`. ccr-local
  reads these replies, and a settled finding is not raised again.
- **Deferral.** Every finding you do not fix needs a GitHub issue labelled
  `deferred`. The issue links the review thread and states the finding, its
  severity, and why it was deferred. Reply `Deferred to #<issue>` on the
  thread. Never create an issue for a finding you fixed, and never defer a
  critical finding in a critical file.
- Never set `SKIP_REVIEW`, `SKIP_CCR`, or `SKIP_COPILOT` yourself. Use the
  Codex engine without fallback; if it is unavailable (exit 4), retry later
  rather than switching engines.
- Reviewers apply the rules below, and should also check bounded memory use,
  responsive touch behavior, secrets, and regressions on the physical device.

### Critical files (`final_review.paths`)

`.ccr.json` lists, as `final_review.paths` globs, the code where a defect
corrupts persisted state, mishandles untrusted input, leaks credentials, or
acts on the host computer:

- the AIMP wire protocol, authentication, and parsers (`monkeymote_client`);
- the on-disk playlist index and cache-trust policy;
- NVS settings, schema migration, and credential storage;
- the untrusted cover-art decoder and its disk cache;
- the microSD CJK font loader and its bounds-checked block cache
  (`cjk_font.*`, `cjk_font_policy.hpp`);
- host power actions (`advanced_controls.hpp`);
- the boot path, Wi-Fi credentials, and the network diagnostics endpoint
  (`app_main.cpp`);
- the flash partition table.

A PR that adds such a path adds its glob in the same PR, and a PR that
renames or moves one updates the glob in the same PR. Reviewers flag a PR
that does not. UI-only code is deliberately excluded.

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

