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
| 48 | Equalizer state and preamp/manual-band setter |
| 49 | Equalizer preset-name collection |
| 51 | Sleep-timer state and setter |
| 9 / 31 / 50 | Playlist name and queue contents |
| 43 | Command result; a non-zero first byte means failure |

The type-28 capability mask is retained as raw diagnostic state and cleared on
disconnect. Reference-client behavior maps bit 17 (`0x00020000`) to computer
power availability, bit 21 (`0x00200000`) to equalizer availability, and bit 22
(`0x00400000`) to sleep-timer availability. The production firmware requests,
parses, renders, and sends only controls whose bit is advertised. Capability
state and all advanced-control state are cleared on disconnect.

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
  rejects inconsistent lengths and displays at most 24 KiB. Type 36 carries
  no request identifier, so a lyrics request that is unanswered for 15 seconds
  (not counting time spent receiving a large frame ahead of it) forces a
  reconnect rather than letting a late reply match a newer request.
- Type 4 subcommands 17, 18, and 19 plus an `int32_t` zero request equalizer
  state, equalizer preset names, and sleep-timer state respectively. They
  produce types 48, 49, and 51. The production client sends these requests
  only after the corresponding capability bit is known. Each outstanding
  response kind has a five-second deadline. A missing response marks only that
  state unavailable without disconnecting the otherwise healthy session; a
  later valid response remains authoritative and restores the ready state.
  Sleep-timer writes additionally require a received type-51 baseline, not
  merely the advertised capability bit.
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

## Advanced controls

The following layouts were qualified against the official AIMP 5 MonkeyMote
plug-in DLL and the configured live AIMP 5 host.

Type 48 has exactly `10 + band_count * 4` bytes. Byte 0 is the band count,
byte 1 is equalizer enabled (`0` or `1`), and bytes 2 and 3 are the fixed `+15`
and `0` range markers. The little-endian words at offsets 4, 6, and 8 are the
fixed range span `300`, preamp gain, and reserved zero. Each band is a
little-endian frequency/gain word pair. Gain values encode `-15.0 .. +15.0 dB`
as `0 .. 300`, with `150` representing `0.0 dB`. AIMP accepts the complete
validated type-48 body as the preamp/manual-band setter. Its AIMP 5 command
handler does not apply byte 1, so enabled is display-only rather than a false
toggle. The firmware never constructs a setter from partial state: it mutates
only one gain in the last strictly validated response, then requests
authoritative state again. Because a type-48 reply to a readback sent before an
edit still reports the old gains, each sent gain is kept for five seconds and
applied over any reply received meanwhile, so a later setter cannot revert it.

Type 49 starts with a little-endian `uint16_t` count and a reserved zero word,
followed by that many NUL-terminated preset names. The firmware bounds this to
128 names of at most 255 bytes each. The plug-in exposes no distinct preset
selection packet, so preset names are retained as state but the UI offers only
the verified manual-band controls.

Type 51 is exactly eight bytes: enabled, trigger, action, reserved, and a
little-endian seconds value. Trigger 1 means the duration and trigger 2 means
the end of the current track. Actions 1, 2, and 3 mean stop playback, exit the
player, and shut down the computer. The device UI intentionally sends only
action 1 and cycles through off, 15, 30, and 60 minutes and end-of-track.
End-of-track is sent as enabled, trigger 2, and zero seconds; off is disabled,
trigger 1, and zero seconds.

Type 4 subcommand 14 followed by one action byte performs a computer action:
0 sleeps, 1 restarts, and 2 shuts down the AIMP computer. These controls remain
hidden without capability bit 17 and always open a separate explicit
confirmation screen. Automated validation must not send this packet.

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

The lightweight type-41 playlist collection exposes stable playlist IDs,
names, and track counts, but no content revision and no active-playlist bit.
The persisted-index policy therefore remains deliberately bounded:

- after a complete authoritative large-playlist response, an atomic marker
  records that the structurally validated index represented the active
  playlist;
- at boot, the client publishes the first cached page immediately as
  unverified, so it remains browsable while AIMP is offline. On reconnect it
  requests only the root type-41 collection and retains the index only when its
  ID, name, and count still match exactly (a legacy `-1` current-playlist ID
  must resolve to one unique name/count match). A missing or malformed root
  reply falls back to a full request after five seconds;
- type 12/17/30/35 notifications, explicit playlist activation, queue edits,
  library additions, or manual `REFRESH FROM AIMP` clear the marker and force
  the complete playlist request; cache-format or extent validation failures do
  the same;
- because same-count edits and active-playlist changes made while the client is
  offline cannot be proven from this protocol, reused rows are visibly labelled
  `CACHED` rather than claimed fresh. Destructive queue, playlist, and library
  mutations remain disabled in that state. Tapping a cached row first activates
  the recorded playlist ID, requests the authoritative current playlist, and
  sends the global queue row only after the response confirms that identity.
  The manual refresh is the authoritative escape hatch.

This avoids the large transfer on an unchanged second connection without
inventing a server revision signal that AIMP does not provide.

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

The read-only `advanced` command requests only the state advertised by the
server capability mask and summarizes equalizer, preset-count, and sleep-timer
responses without printing raw payloads. The separately guarded
`validate-advanced --confirm-live-mutations` command adjusts the equalizer
preamp by 1.0 dB and—only when no timer is already active—briefly sets
a 15-minute stop-playback timer. In `finally` it makes a best-effort exact
restore of the equalizer body and disabled timer baseline, retrying once after
reconnection. It contains no computer-power packet. Do not interrupt the
command: a process kill or persistent connection loss can prevent restoration
and leave EQ changed or a stop-playback timer active.

`--advanced-scope equalizer` and `--advanced-scope sleep-timer` qualify either
control independently when a server advertises both but does not return one
state. A missing baseline is never guessed, and that control is not mutated.
The read-only command reports such a missing state as unavailable while still
printing independently valid advanced state.
