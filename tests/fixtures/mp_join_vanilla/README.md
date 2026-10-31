# mp_join_vanilla fixture: a join against a FRESH vanilla Beta 1.7.3 world (REL-MP-JOINSPEED)

`stream.bin` is the complete inbound (server to client) byte stream of one
protocol-14 join against the pinned vanilla Beta 1.7.3 server
(`tools/mp_fixture/vanilla-b173.json`) on a freshly generated world, from the
first byte after TCP connect. Replayed byte-exact by `test_mp_walk_session`
(ctest `test_mp_walk_session_vanilla_join`) through the production transport,
decoder, retain/chain path and pump at the Wii admission radius 4.

| file | bytes | packets | sha256 |
|---|---|---|---|
| `stream.bin` | 1,425,380 | 68,724 | `b4bba88e6f5dd0a8f86d51028e3fc78e30a8757e062c4f640807bd3b7fcd3932` |

Census (`trim_stream.py`): 289 Packet50 loads, 0 unloads, 139 full and 609
partial Packet51, 19,366 Packet53, 6,824 Packet52 (38,735 cell writes in all),
2,666 Packet23. Only 139 of the 289 announced columns receive their full body
inside the 55 s: vanilla subscribes the player to every column at once and
forwards each block AND light change of the columns it is still populating
(`PlayerInstance.java:62-69, 122-166`, `World.setLightValue` ->
`markBlockNeedsUpdate`), but streams at most one full column per tick and only
while fewer than 4 chunk packets are queued (`EntityPlayerMP.java:131-142`).
That pre-body flood is what parked ~15,000 distinct cells and, with the old
1,024-entry log, ran one column compaction per incoming edit
(tmp/jobs/join-regress-1022).

## How it was captured (2026-10-22, 09:57)

- Server: the owned `minecraft_server.jar` (sha256 `033a127e...`, referenced by
  hash only, never copied) under Java 1.8.0_491, `tools/mp_fixture/server.properties`
  with two changes made by the recorder: port 25593 (a private port; the rig's
  servers were not touched) and `allow-flight=true`, so the motionless recorder
  is not kicked for floating (`NetServerHandler` floating check). Fresh world
  directory, seed 9999, view-distance 8, offline mode.
- Recorder: `record_vanilla_join.py <run_dir> 25593 30 55 16` (run with
  `python -I`). It starts the server in `<run_dir>`, waits for "Done", logs in
  as `WiiTuck`, echoes the server's Packet13 and then sends one Packet13 per
  50 ms tick, and writes every received byte to `s2c.bin`. For the first 30 s it
  reads at 16 KB/s through a 16 KB receive buffer -- the Wii's measured 8-32
  KB/s arrival during the slow joins -- so the server's chunk queue backs up the
  same way and the edit/body interleaving is the one the console sees; then full
  speed until 55 s. Stopped with `stop` on the server's stdin (own PID only).
- The stream ends on a packet boundary (`trim_stream.py` confirms; nothing was
  cut). Server log and recorder log stayed in the session scratchpad.

## What the test checks on it (and on the other three recorded streams)

- An independent model of the Beta client (pre-chunk on load, each Packet51
  region overwrites ids + metadata in arrival order, each 52/53/60 cell write
  applies) must equal CaveWii's final id + metadata of every loaded column:
  installed voxels, or head + chain + parked edits in stamp order for a column
  that is not installed. Light is client-derived (relight) and not compared.
- Bounds: no exhaustion rescue, compactions <= announced columns, and the time
  compaction holds `impl->lock` (generous PC ceilings).
