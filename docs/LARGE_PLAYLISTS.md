# Large-playlist storage

MonkeyMote returns the complete active playlist in one response. The live
`NAS:Music` response contains 48,321 tracks and its compressed type-45 frame is
1,223,088 bytes, so it cannot be retained alongside LVGL and networking in the
SC01 Plus's 2 MB PSRAM.

The firmware therefore mounts the SC01 Plus microSD slot without automatic
formatting and uses `/sdcard/mremote`. Oversized compressed responses are
written directly from the socket in bounded chunks to `WIRE.TMP`. A raw-LZ4
decoder then uses one 64 KiB history window in PSRAM and writes `RAW.TMP`.
Neither the compressed nor decoded response is held in memory.

`INDEX.DAT` contains a versioned header, one fixed-size record per queue row,
and a variable string area. Records retain the AIMP item ID, duration, and the
offsets and lengths of the title, artist and normalized lowercase search key.
The zero-based queue row is implied by the record's position, so records remain
in wire order. Only one page of 100 display records is read into the model.

Index construction uses separate record and string temporary files. Before a
build, free space is checked against five times the advertised decoded size
plus 1 MiB. The completed file is flushed and synced, the prior index is moved
to `INDEX.BAK`, and the new `INDEX.TMP` is promoted. A failed promotion restores
the backup; boot recovery also restores a backup when the final file is absent.
All names use FAT 8.3 form because this firmware intentionally disables FAT
long-file-name allocation.

An absent, full, interrupted or malformed card never causes the firmware to
allocate the complete response. It drains the frame, preserves any previously
promoted index, and reports the storage/index error. Internal-flash fallback,
the on-screen text-search workflow, and removal/interruption qualification are
still tracked in issue #7; internal flash must not be rebuilt frequently because
its erase endurance is finite.
