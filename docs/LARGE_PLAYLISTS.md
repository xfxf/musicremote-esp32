# Large-playlist storage

MonkeyMote returns the complete active playlist in one response. The live
`NAS:Music` response contains 48,321 tracks and its compressed type-45 frame is
1,223,088 bytes, so it cannot be retained alongside LVGL and networking in the
SC01 Plus's 2 MB PSRAM.

The default `PLAYLIST_STORAGE=auto` policy mounts the SC01 Plus microSD slot
without automatic formatting and uses `/sdcard/mremote`. If no card is
available, it mounts the dedicated 4 MB SPIFFS `storage` partition at
`/flash/mremote`. `sd` requires the card and disables fallback; `flash`
explicitly selects internal storage. Oversized compressed responses are
written directly from the socket in bounded chunks to `WIRE.TMP`. A raw-LZ4
decoder then uses one 64 KiB history window in PSRAM and writes `RAW.TMP`.
Neither the compressed nor decoded response is held in memory.

Decompression and index construction run on a dedicated FreeRTOS task so the
network task continues answering AIMP keepalives and UI requests during the
roughly 145-second full-library build (48,321 rows on microSD). If another
large response arrives while that task is active, the in-flight index is
marked superseded and is not published; the current playlist is requested
again after the worker exits.
Likewise, a late success or failure cannot replace a newer small-playlist queue.

`INDEX.DAT` version 3 contains a versioned header, one fixed-size 32-byte
record per queue row, a variable string area, and a sequential secondary search
area. Records retain the AIMP item ID, duration, and offsets and lengths for
title, artist, album, and the normalized search key. Search entries map that
key back to the original zero-based queue row. Normalization performs ASCII
case-folding over `title<TAB>artist<TAB>album`, capped at 2,048 bytes; UTF-8
bytes outside ASCII are preserved. Only one page of 100 display records is
read into the model. AIMP also sends a genre string per row
(`docs/PROTOCOL.md`); it is not indexed, because an unranked substring match
on a genre word returns thousands of rows and buries title, artist, and album
matches. Version 2 files (title and artist only, 28-byte records) fail the
header version check: at boot they are not offered as a cached queue, and the
next response rebuilds the index rather than reusing it by source hash.

Index construction reserves the fixed-record table in `INDEX.TMP`, appends
strings in wire order, fills records in 128-row batches, then appends the search
area in a second bounded pass. This avoids duplicate record/string staging
files and allows a fitting index to be built in the small internal partition.
Before a build, free space is checked conservatively against six decoded sizes
plus 1 MiB on microSD, or four decoded sizes plus 256 KiB on SPIFFS. The
completed file is flushed and synced, the prior index is moved to `INDEX.BAK`,
and `INDEX.TMP` is promoted. A failed promotion restores the backup; boot
recovery also restores a structurally valid backup when the final file is
absent or has an invalid header/extent. All names use FAT 8.3 form because this
firmware intentionally disables FAT long-file-name allocation.

The index header records a 64-bit hash and byte length of the compressed source,
and `INDEX.DAT` remains on the selected storage across resets and power loss.
The current format retains one promoted oversized-playlist index—the most
recent one—not a separate cache for every AIMP playlist.
After reboot, an identical response reuses the promoted file instead of
rebuilding or rewriting it. The header also records a 64-bit hash of the
decoded playlist that leaves out its playlist-ID and flags fields. A response
to a request for the current playlist (`-1`) and one to a request for its
explicit ID describe the same rows but compress differently (1,223,087 and
1,223,090 bytes for `NAS:Music`), so the compressed hash alone rebuilt the
index after every switch away and back. When the compressed hash differs and
a valid index exists, the firmware first decodes the response into a digest
only, with no storage write, and compares the content hash, name, and count.
A match reuses the index after that decode alone. Only a confirmed rebuild
then faces the internal-flash wear throttle and the free-space reserve, so an
unchanged playlist is never reported stale or `SD CARD FULL`. The stored
playlist ID is published as the active playlist, so reuse also requires that
ID to be `-1` or the response's own ID: a current-playlist (`-1`) response
does not prove that an explicitly stored playlist is still the active one,
and it rebuilds instead. MonkeyMote does not expose a playlist revision, delta,
pagination, or changed-row notification, so AIMP must still send the complete
compressed response before the device can verify that hash. A changed hash
invalidates the complete local index; safe partial invalidation is not
possible without row identities or a server revision. On
internal flash, changed content received less than 15 minutes after boot or the
last promotion keeps the previous index file and visibly reports
`INDEX STALE - RETRY LATER`. The kept file is reused only if a later response
hashes identically; its rows no longer match AIMP's queue, so the queue shows
no rows, and nothing can be played, edited, or paged, until a matching index
is built or a manual refresh after the interval rebuilds it. This bounds erase
frequency during reconnect or
notification storms. SPIFFS still has finite endurance; microSD is recommended
for frequently edited or large libraries.

Because AIMP offers no cheap change check, the queue header shows a refresh
button (the LVGL refresh symbol, 44x42 px between the title and `FIND`)
whenever the queue is served from the index or from the persisted cache. It
sends the same authoritative request as Settings > Playback > `REFRESH FROM
AIMP`: AIMP resends the complete response (about 4 seconds for `NAS:Music` on
microSD), and an unchanged response is matched by source hash and reused in
about 3 seconds instead of rebuilding. The button is hidden for small
playlists, which AIMP's change notifications keep current, and while a
transfer or build is reporting progress in the title.

Switching to another playlist keeps `INDEX.DAT` unless that playlist is also
large: a small playlist uses the in-memory path and never touches the file.
The switch clears the `ACTIVE.DAT` marker, so the next boot requests the
active queue instead of offering the cached rows. Switching back transfers the
large response again and reuses the kept index by content hash, decoding it
into a digest in 8 seconds after a 3.7-second transfer for `NAS:Music`,
instead of rebuilding it (about 145 seconds). AIMP answers every activation
with a type 12 notification before it answers the request sent with it; the
firmware recognizes that echo and does not transfer the playlist again. A
second large playlist replaces the single index, and switching back to the
first then rebuilds it.

Re-validating the playlist already on screen (the refresh button, a genuine
change notification, or a tap on a cached row) keeps the current page visible
while the response is transferred and checked; the title reports progress.
Those rows are treated as unverified until the build finishes: a tap goes
through the cached-play confirmation, destructive edits stay disabled, and
paging and `FIND` wait for the build, which then republishes the same page.
Moving between the NOW and QUEUE tabs never requests anything from AIMP.

`FIND` opens a full-screen A-Z surface. `#` matches titles whose first byte is
not A-Z, letters perform a case-insensitive title-prefix scan, and `ALL` clears
the filter. `SEARCH` opens the LVGL keyboard and performs a case-insensitive
substring scan over title, artist, and album. The complete secondary area is
scanned with a small buffer, but only the requested 100 matching rows are
materialized. Result rows retain their global AIMP queue row. Selecting one
plays that item (through the cached-play confirmation when the queue is
cached), clears the filter, and stays on the queue at the playlist page that
holds it, scrolled into view and highlighted once AIMP reports it as current,
so the rest of its album can be browsed in playlist order.

AIMP may label a request for the current queue with playlist ID `-1`. On the
first indexed page the firmware resolves that sentinel to a unique playlist
with the same name and track count, then retains the resolved ID across later
pages even while the user browses playlist folders.

An absent, full, interrupted or malformed card never causes the firmware to
allocate the complete response. It drains the frame, preserves any previously
promoted index, and reports the storage/index error. An internal index that
cannot fit reports `FLASH INDEX TOO LARGE`; normal small playlists continue to
use the bounded in-memory path.
