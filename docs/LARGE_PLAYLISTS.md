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
roughly 80-second full-library build. If another large response arrives while
that task is active, the in-flight index is marked superseded and is not
published; the current playlist is requested again after the worker exits.
Likewise, a late success or failure cannot replace a newer small-playlist queue.

`INDEX.DAT` version 2 contains a versioned header, one fixed-size record per
queue row, a variable string area, and a sequential secondary search area.
Records retain the AIMP item ID, duration, and offsets and lengths for title,
artist, and the normalized search key. Search entries map that key back to the
original zero-based queue row. Normalization performs ASCII case-folding over
`title<TAB>artist`; UTF-8 bytes outside ASCII are preserved. Only one page of
100 display records is read into the model.

Index construction reserves the fixed-record table in `INDEX.TMP`, appends
strings in wire order, fills records in 128-row batches, then appends the search
area in a second bounded pass. This avoids duplicate record/string staging
files and allows a fitting index to be built in the small internal partition.
Before a build, free space is checked conservatively against six decoded sizes
plus 1 MiB on microSD, or three decoded sizes plus 256 KiB on SPIFFS. The
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
rebuilding or rewriting it. MonkeyMote does not expose a playlist revision,
delta, pagination, or changed-row notification, so AIMP must still send the
complete compressed response before the device can verify that hash. A changed
hash invalidates the complete local index; safe partial invalidation is not
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

`FIND` opens a full-screen A-Z surface. `#` matches titles whose first byte is
not A-Z, letters perform a case-insensitive title-prefix scan, and `ALL` clears
the filter. `SEARCH` opens the LVGL keyboard and performs a case-insensitive
substring scan over both title and artist. The complete secondary area is
scanned with a small buffer, but only the requested 100 matching rows are
materialized. Result rows retain their global AIMP queue row, so selecting one
plays the correct item and returns to Now Playing.

AIMP may label a request for the current queue with playlist ID `-1`. On the
first indexed page the firmware resolves that sentinel to a unique playlist
with the same name and track count, then retains the resolved ID across later
pages even while the user browses playlist folders.

An absent, full, interrupted or malformed card never causes the firmware to
allocate the complete response. It drains the frame, preserves any previously
promoted index, and reports the storage/index error. An internal index that
cannot fit reports `FLASH INDEX TOO LARGE`; normal small playlists continue to
use the bounded in-memory path.
