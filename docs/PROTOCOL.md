# AIMP interoperability notes

These notes document observed behavior needed by this independent client. All
integer fields are little-endian. Packet bodies are treated as untrusted input.

## Framing and authentication

Each frame starts with three `uint32_t` fields: magic `0x71D9873B`, message type,
and payload length. The server sends a type 1 hello with an eight-byte nonce.
For password-protected sessions, type 2 contains `MD5(password || nonce)` plus
client capability bytes `0x81, 0x05`. Type 3 with payload `01` accepts the
session. Passwords and digests must never be logged.

## Keepalive and client liveness

Live probing on 2026-11-17 (issue #48) established the session timing. AIMP
sends an empty type 23 keepalive about every 6 seconds; the client echoes it.
A client-initiated type 23 receives no reply. AIMP closes a client that has
sent it nothing for about 10 seconds: a probe that stopped answering
closed 10.5 seconds after its last packet.

AIMP's keepalives are queued on the same TCP stream as its replies, so a
client that is still receiving a large frame cannot see, or echo, them. A
probe reading the 1.2 MB `NAS:Music` response at 20 KB/s was disconnected
after 13 seconds and 268 KB; at 120 KB/s it received the whole frame and was
still disconnected at 10.2 seconds. The same 20 KB/s read with a client
keepalive every 3 seconds stayed connected for the full 90-second test.
Earlier firmware sent a keepalive only after 10 seconds without receiving
data, which never happens during a transfer, so a frame written to slow
storage (about 20 KB/s on the internal SPIFFS fallback) was cut off every
time. The firmware now sends a type 23 whenever it has sent nothing for
4 seconds, whether or not it is receiving.

Its request deadlines likewise pause while a frame is partially received,
including a large playlist frame streamed to storage or an oversized frame
being drained, because every later reply is queued behind it. A stalled
transfer still ends the session after 30 seconds without data. Every
deliberate disconnect logs `Closing AIMP connection: <reason>`.

## Received messages

| Type | Meaning |
|---:|---|
| 5 | Current track metadata |
| 6 | Position, playback, shuffle, repeat, and volume state |
| 8 | Current-track cover-art response |
| 11 | Current queue index |
| 12 / 17 / 30 / 35 | Queue/playlist changed notification; request the current queue again (see below for the activation echo) |
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
history, and only a 100-row index page is loaded into the model. The decoded
row count is bounded by the bytes left after the playlist header (each row
needs at least 36: a 32-byte base and four terminators) before the record table
is reserved, and a stored title or artist longer than the 2048-byte field cap
marks the index unreadable rather than sizing an allocation. Larger and
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

Live probing on 2026-11-17 (issue #48) showed that play-row, play, next,
previous, pause, and stop produce only type 5, 6, and 11 frames, never a
type 12/17/30/35 notification. Activating a playlist (subcommand 12), even
the one already active, is answered with one empty type 12 before AIMP
answers any request sent after the activation. Treating that echo as a change
had made the reply to the following request stale, so a tap on a cached row
never played and the 1.2 MB playlist was transferred two more times. The
firmware now expects one echo per activation it sends, in order, each for
5 seconds (AIMP once took 2.3 seconds), paused while a large frame is being
received; several activations can be outstanding at once. A type 12 matching
an expectation is absorbed; any other is handled as a change. The trade-off is
that a genuine change notified inside that window is missed until the next
notification, refresh, or tap verification.
A playlist switch asks for the current playlist (`-1`) right after the
activation instead of the explicit ID: that response is byte-identical to the
one the index was last built or verified from, so an unchanged large playlist
is reused by source hash (5.2 s end to end for `NAS:Music`) instead of being
decoded for a content check (11.2 s).

## Other implemented operations

- Type 4 subcommand 13 requests playlists. Its `int32_t` argument is `-1` for
  the root or a playlist-folder ID for that folder's children. In each type 41
  entry, flag bit 0 means folder and bit 1 means protected/read-only; neither
  bit indicates the active playlist.
- Type 4 subcommand 10 requests a playlist and queue. Its payload is the
  subcommand byte, playlist ID (`int32_t`, with `-1` meaning current), and an
  observed constant `int32_t` value of 1.

  The type 9/31/50 response starts with `uint32_t count`, `int32_t
  playlist_id`, `uint16_t name_length`, one flags byte, and the name plus a
  NUL. Requested as `-1`, `NAS:Music` came back with ID `-1` and flags `1`;
  requested as ID 2, with ID 2 and flags `0`. Those five bytes were the only
  differences in 3,933,117 decoded bytes, yet the compressed frames were
  1,223,087 and 1,223,090 bytes. Each of the `count` rows is a 32-byte base followed by four strings,
  each of its declared length plus a NUL:

  | Offset | Field |
  |---:|---|
  | 0 | `int32_t` item ID (`-1` on every `NAS:Music` row) |
  | 4 | `int32_t` duration in milliseconds |
  | 8 | `uint16_t` bitrate in kbit/s |
  | 10 | `uint8_t` sample rate in kHz (44, 22, ...), inferred |
  | 11 | `uint8_t` rating, 0 through 5 |
  | 12 | `uint16_t` year, 0 when unknown |
  | 14 | `uint16_t`, non-zero on 62% of rows; probably track number (unverified) |
  | 16 | eight bytes, unidentified, almost always zero |
  | 24 | `uint16_t` title, artist, album, and genre byte lengths |

  The string order is title, artist, album, genre. Earlier notes and the
  in-memory model called the fourth string `codec`; the live 48,321-row
  `NAS:Music` response (3,933,117 bytes decoded, no trailing bytes) shows
  genre names such as `Electronic`, `Rock`, and `Drum & Bass`. Non-empty
  counts there were 48,056 titles, 39,549 artists, 37,777 albums, and 29,622
  genres; the longest were 150, 96, 127, and 90 bytes. There is no file path,
  composer, or per-row revision. The large-playlist index stores title,
  artist, and album, and searches all three (`docs/LARGE_PLAYLISTS.md`).
- Type 4 subcommand 3 requests artwork. The first `int32_t` is a queue row in
  the current playlist, with `-1` meaning the current track. The second is
  the maximum edge in pixels: `0` gives a default size, and `-1` gives the
  original. The production UI requests `-1, 144` and receives a 144x144 type 8
  reply, or the empty type 13 result when that item has no artwork.

  Live probing on 2026-11-17 established this. Requests `(0,144)`, `(64,64)`,
  and `(100,100)` returned the covers of queue rows 0, 64, and 100 at that
  size; rows without artwork (128, 143, 144) returned type 13. Earlier
  firmware sent `144, 144`, misread as width and height. It therefore showed
  queue row 144's artwork, or none, rather than the current track's.
  The on-disk artwork cache format moved from `MRA1` to `MRA2` so those
  wrongly keyed entries are refetched rather than reused.
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
  response kind has a five-second deadline, paused like every other request
  deadline while a large frame is received. A missing response marks only that
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
invalidates an older completion before it can authorize playback. A playlist
count is authoritative only when it comes from a current response to a request
sent after the latest queue mutation or type 12/17/30/35 notification. Pages
read from the local index, throttled (stale) index builds, and responses to
earlier requests never restore it, and duplicate refreshes are coalesced only
with a request of the same mutation generation. A response to an earlier
request, small or streamed, also publishes no rows, playlist ID, or active
marker and keeps the mutation guard: after a switch its rows would play in the
newly active playlist, and the mutation has already queued a newer request.
Stop and pause cancel a
deferred library play. The firmware never clears the user's current playlist as a side effect of tapping a library
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
An already validated band layout stays editable while that readback is
outstanding, so successive slider edits are sent without waiting for it; the
sliders are withdrawn only when no layout is known or the readback times out.

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
local secondary index provides A-Z title prefixes and title/artist/album substring
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
  the same. A failed page or search read also marks the stored index unusable,
  so the following response rebuilds it instead of reusing it by source hash;
- because same-count edits and active-playlist changes made while the client is
  offline cannot be proven from this protocol, reused rows are visibly labelled
  `CACHED` rather than claimed fresh. Destructive queue, playlist, and library
  mutations remain disabled in that state, including while a replacement is
  transferred and indexed and after a failed or throttled (stale) build, until
  an authoritative index or small playlist response replaces it. Paging, A-Z,
  and search of the local index are served while AIMP is offline. A throttled
  build publishes no rows, playlist ID, or count from the kept index.
  The manual refresh is the authoritative escape hatch;
- tapping a row plays it optimistically (issue #48). On a cached queue the
  firmware first activates the recorded playlist ID (its type 12 echo is
  absorbed), then sends play-row (type 7 subcommand 10) with the global row
  and play, and switches to Now Playing at once; no transfer or confirmation
  request sits on the tap path. It then compares AIMP's next type 11 current
  row and type 5 title and artist with the index row, within 6 seconds
  (paused while a large frame is received). A match confirms playback only:
  one row says nothing about the others (cached A, B, C against live A, C, B
  matches a tap on A), and rows are addressed by number, so `CACHED` and the
  edit guard stay until a full refresh matches or rebuilds the index; a
  long-press edit or CLEAR on cached rows shows "Refresh to edit". A
  mismatch or timeout marks it unverified again, shows "Playlist changed",
  and starts the background refresh with the rows still on screen; playback
  continues. The trade-off: a stale cache can briefly play the wrong row,
  which then corrects itself through that refresh. On the board, play-row
  left 20 to 95 ms after the tap, and the match arrived 0.2 to 2.3 seconds
  after it.

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
