# AIMP interoperability notes

These notes document observed behavior needed by this independent client. All
integer fields are little-endian. Packet bodies are treated as untrusted input.

## Framing and authentication

Each frame starts with three `uint32_t` fields: magic `0x71D9873B`, message type,
and payload length. The server sends a type 1 hello with an eight-byte nonce.
For password-protected sessions, type 2 contains `MD5(password || nonce)` plus
client capability bytes `0x81, 0x05`. Type 3 with payload `01` accepts the
session. Passwords and digests must never be logged.

## Received messages

| Type | Meaning |
|---:|---|
| 5 | Current track metadata |
| 6 | Position, playback, shuffle, repeat, and volume state |
| 11 | Current queue index |
| 12 / 17 / 30 / 35 | Queue/playlist changed notification; request the current queue again |
| 23 | Keepalive (echoed) |
| 28 | Eight-byte capability mask |
| 41 | Playlist collection |
| 45 | LZ4-compressed inner message |
| 9 / 31 / 50 | Playlist name and queue contents |
| 43 | Command result; a non-zero first byte means failure |

Type 45 begins with the uncompressed size (`uint32_t`), inner type (`uint32_t`),
two reserved bytes, then a raw LZ4 block. Normal messages reject output beyond
512 KiB and require an exact advertised-size match. Oversized type-45 playlist
responses of at most 4 MiB on the wire and 64 MiB decoded take a separate
microSD path: the wire body is streamed to disk, the raw block is decoded with a
bounded 64 KiB history, and only a 100-row index page is loaded into the model.
Larger and other oversized frames are drained without allocation.

## Commands

Type 7 carries playback commands. The first byte is the subcommand and most
values are a following `int32_t`:

| Subcommand | Action |
|---:|---|
| 1 / 2 / 3 | Play / pause / stop |
| 4 / 5 | Shuffle / repeat mode |
| 6 | Volume (`uint16_t` coarse plus exact value) |
| 7 | Seek in milliseconds |
| 8 / 9 | Next / previous |
| 10 | Play queue index |
| 12 | Activate playlist ID |
| 16 | Delete playlist ID |

The play value for subcommand 10 is the zero-based row in the current queue,
not the per-item ID in playlist metadata (which AIMP may report as `-1`). Type
11 publishes the current zero-based queue row.

## Other implemented operations

- Type 4 subcommand 13 requests playlists. Its `int32_t` argument is `-1` for
  the root or a playlist-folder ID for that folder's children. In each type 41
  entry, flag bit 0 means folder and bit 1 means protected/read-only; neither
  bit indicates the active playlist.
- Type 4 subcommand 10 requests a playlist and queue. Its payload is the
  subcommand byte, playlist ID (`int32_t`, with `-1` meaning current), and an
  observed constant `int32_t` value of 1.
- Type 10 sets a one-byte rating from 0 through 5.
- Type 20 clears a playlist.
- Type 32 removes a queue item (playlist ID and item index).
- Type 42 creates a playlist from a parent/folder ID, reserved `uint16_t`, name
  byte length, and NUL-terminated name.

## Large playlists

Live testing of the 48,321-entry `NAS:Music` playlist produced one 1,223,088
byte frame before decoding. No playlist offset/count parameters or server-side
search operation were found in the observed protocol, and the reference client
filters its received collection locally. Consequently, paging the UI does not
reduce the network or decoding cost: full-library search requires streaming the
single response into external storage and building a local index. The issue #7
implementation prefers the SC01 Plus microSD slot, uses atomic promotion, and
reads 100-row pages. If storage is unavailable or indexing fails, the frame is
drained and the UI reports the failure without allocating the complete response
or dropping the AIMP connection.

The host probe in `tools/protocol_probe.py` implements the same framing,
authentication, and LZ4 decoding without third-party Python dependencies.
