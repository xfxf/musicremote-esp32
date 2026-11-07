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
| 23 | Keepalive (echoed) |
| 28 | Capability mask |
| 41 | Playlist collection |
| 45 | LZ4-compressed inner message |
| 9 / 31 / 50 | Playlist name and queue contents |
| 43 | Command result |

Type 45 begins with the uncompressed size (`uint32_t`), inner type (`uint32_t`),
two reserved bytes, then a raw LZ4 block. The decoder rejects output beyond 2
MiB and requires an exact advertised-size match.

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

Other implemented operations:

- Type 4 subcommand 13 requests playlists.
- Type 4 subcommand 10 requests a playlist and queue.
- Type 10 sets a one-byte rating from 0 through 5.
- Type 20 clears a playlist.
- Type 32 removes a queue item (playlist ID and item index).
- Type 42 creates a playlist.

The host probe in `tools/protocol_probe.py` implements the same framing,
authentication, and LZ4 decoding without third-party Python dependencies.
