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
| 8 | Current-track cover-art response |
| 11 | Current queue index |
| 12 / 17 / 30 / 35 | Queue/playlist changed notification; request the current queue again |
| 19 | Media-library node, album, or track result |
| 23 | Keepalive (echoed) |
| 28 | Eight-byte capability mask |
| 36 | Current-track lyrics response |
| 41 | Playlist collection |
| 45 | LZ4-compressed inner message |
| 9 / 31 / 50 | Playlist name and queue contents |
| 43 | Command result; a non-zero first byte means failure |

The type-28 capability mask is retained as raw diagnostic state and cleared on
disconnect. Reference-client behavior maps bit 17 (`0x00020000`) to computer
power availability, bit 21 (`0x00200000`) to equalizer availability, and bit 22
(`0x00400000`) to sleep-timer availability. These bits gate visibility only;
they do not establish a safe command wire format, so the production firmware
does not expose those actions yet.

Type 8 starts with the JPEG byte length (`uint32_t`), followed by a 16-byte
server artwork identity and the JPEG bytes. The length must exactly match the
remaining bytes. A type 13 empty response means no artwork was available for
the current item. The firmware rejects cover bodies above 256 KiB, non-JPEG
signatures, and decoded dimensions above 256 pixels on either axis.

Type 45 begins with the uncompressed size (`uint32_t`), inner type (`uint32_t`),
two reserved bytes, then a raw LZ4 block. Normal messages reject output beyond
512 KiB and require an exact advertised-size match. Oversized type-45 playlist
responses of at most 4 MiB on the wire and 64 MiB decoded take a separate
persistent-storage path: the wire body is streamed to microSD or the fitting
internal SPIFFS fallback, the raw block is decoded with a bounded 64 KiB
history, and only a 100-row index page is loaded into the model. Larger and
other oversized frames are drained without allocation.

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
- Type 4 subcommand 3 requests current-track artwork. Two following `int32_t`
  values are the maximum width and height. The production UI requests 144 by
  144 pixels and receives type 8 or the empty type 13 result.
- Type 4 subcommands 7, 16, 9, and 12 request the media-library artist,
  album, genre, and composer roots respectively. Each carries an `int32_t`
  request ID and receives type 19.
- Type 4 subcommand 11 plus an `int32_t` zero requests current-track lyrics.
  The type 36 non-empty layout is inferred as a `uint32_t` UTF-8 byte length,
  a 16-byte track identity, and the text; live testing has so far observed only
  the empty form, so this layout remains explicitly unverified. The firmware
  rejects inconsistent lengths and displays at most 24 KiB.
- Type 24 searches the complete media library. Its body is the `int32_t`
  request ID followed by a NUL-terminated UTF-8 query. Search result rows are
  subsequently addressed through type 44 using the same request ID.
- Type 39 browses a media-library path. Its body is an `int32_t` request ID,
  an `int32_t` selector mask, then the selected genre (bit 0), artist (bit 1),
  album (bit 2), and composer (bit 3) strings in bit order. Results are type
  19. Every type-19 response starts with `uint32_t request_id`, `uint32_t
  count`, and `uint8_t kind`: 0 is artist, 2 genre, 4 composer, 6 album, and
  7/8 track. Kinds 0/2/4 are one NUL-terminated UTF-8 string per row. An album
  row has a 12-byte header (`uint16_t album_length`, `uint16_t artist_length`,
  `uint16_t year`, reserved `uint16_t`, and `uint32_t record_size`) followed by
  album and artist strings, each with its declared bytes plus a NUL. A track
  row has a 20-byte header (`uint16_t` artist/title/album lengths, `uint16_t
  year`, `uint32_t record_size`, `int32_t duration_ms`, `uint16_t bitrate`,
  `uint8_t rating`, and `uint8_t flags`) followed by artist, title, and album
  strings in that order, again NUL-terminated. The implementation rejects
  counts above 100,000, fields above 1,024 bytes, inconsistent record sizes,
  trailing bytes, or any boundary violation before publishing results. It
  retains at most 1,000 rows and 128 KiB of row text.
- Type 40 adds a browsed library result. It carries destination playlist ID,
  selector flags, and result row as `int32_t`, `uint32_t`, and `int32_t`, then
  the selected NUL-terminated path strings. The production UI sends only a
  concrete track row; the unvalidated `-1` whole-node form is intentionally not
  exposed.
- Type 44 adds a type-24 search result. Its body is search request ID,
  destination playlist ID, selected result row, insertion position (`-2` for
  the end), and a reserved zero `uint16_t`.

For immediate playback, the firmware appends only the selected type-40 or
type-44 result to the active playlist, requests that playlist on the same TCP
stream, and pairs responses with explicit request tokens in FIFO order. It
selects the prior authoritative playlist count with type 7/subcommand 10 and
sends play only after the decoded response identifies the requested playlist
and contains more rows than that prior count. Large compressed responses are
confirmed only after their on-card decoder has produced the playlist metadata;
arrival or request ordering alone is not treated as proof of an append. Invalid,
discarded, and failed responses still consume their request token so later
responses cannot become mis-correlated. The token FIFO is capped, duplicate
refreshes are coalesced, and a missing head response or unclassifiable envelope
forces a bounded reconnect rather than shifting correlation. Streamed index
results also carry a monotonic generation, so a newer playlist response
invalidates an older completion before it can authorize playback. The firmware
never clears the user's current playlist as a side effect of tapping a library
result.
- Type 10 sets a one-byte rating from 0 through 5.
- Type 20 clears a playlist.
- Type 21 asks AIMP's media-library delegate to add a matching item to a
  playlist. The live validation probe uses five `int32_t` values: destination
  playlist ID followed by four zero values. With no optional filters enabled,
  selection zero adds one media-library item to the disposable destination.
- Type 32 removes a queue item (playlist ID and item index).
- Type 42 creates a playlist from a parent/folder ID, reserved `uint16_t`, name
  byte length, and NUL-terminated name.

Live AIMP 5 testing confirms seek subcommand 7 is expressed in milliseconds.
The server may send a short burst of type 6 states around the requested
position and then remain quiet while playback continues. The firmware therefore
uses the most recent type 6 position as an authoritative sync point and
extrapolates elapsed time locally while the published state is playing. A later
type 6 state always replaces that estimate.

## Large playlists

Live testing of the 48,321-entry `NAS:Music` playlist produced one 1,223,088
byte frame before decoding. No playlist offset/count parameters or server-side
search operation were found in the observed protocol, and the reference client
filters its received collection locally. Consequently, paging the UI does not
reduce the network or decoding cost: full-library search requires streaming the
single response into external storage and building a local index. The issue #7
implementation prefers the SC01 Plus microSD slot, supports a size-gated 4 MB
internal fallback, uses atomic promotion, and reads 100-row pages. A normalized
local secondary index provides A-Z title prefixes and title/artist substring
search because no corresponding server operation was observed. If storage is
unavailable or indexing fails, the frame is drained and the UI reports the
failure without allocating the complete response or dropping the AIMP
connection.

The host probe in `tools/protocol_probe.py` implements the same framing,
authentication, and LZ4 decoding without third-party Python dependencies. Its
`validate-mutations --confirm-live-mutations` command confines create, populate,
seek, remove, clear, and delete operations to a uniquely named scratch
playlist. The rating step changes the shared media-library item added to that
playlist and restores its original value before cleanup; an interrupted run can
therefore leave that track's rating changed. The command records only packet
types and sizes and restores the observed AIMP state in a `finally` block. The
explicit confirmation flag prevents accidental mutation during routine
read-only probing.

The probe's read-only `cover --cover-size 144` command validates the response
length/signature and prints only the JPEG size, server identity, and SHA-256;
it does not write or print the image body.

The `library`, `library-browse`, `library-search`, and `lyrics` probe commands
exercise these bounded parsers. The guarded mutation matrix validates both
type-40 browse selection and type-44 search selection against its disposable
playlist before clearing and deleting it.
