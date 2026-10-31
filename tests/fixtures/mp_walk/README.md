# mp_walk fixture: real-Beta session drop while walking (IN-007)

`stream.bin` is the inbound (server to client) byte stream of a real Beta 1.7.3
server session, from the first byte after TCP connect up to and including the
Packet51 that the chain replay identifies as the one CaveWii refuses. The
diagnosis is in `tmp/jobs/loop/IN-007-cause.md`.

| file | bytes | sha256 |
|---|---|---|
| `stream.bin` | 336,615 (4,504 packets) | `488ceb635d39467e7da38644c6906c23d4be356fb0ea494b96da355d74ff461b` |
| `c2s.bin` | 13,764 | `18f0912ee23d849e07e92edebe8973acf2f1aee2d17b7542f51d79db9affc78e` |
| raw tee `s2c.bin` (not kept) | 337,282 | `d7bf27d00a72ba03a47541c1ebda9ad8d926c689c9f2fb73310374ef6c0792eb` |

- The last packet of `stream.bin` starts at offset 336,517: Packet51
  `x=200 y=10 z=-144 size=3x4x6`, column (12,-9). It is the fifth body that
  column received after its Packet50 load: a full 16x128x16 head at 277,603,
  then partial bodies at 307,437, 312,435, 313,273 and 336,517. The head covers
  every partial, but **no later body covers an earlier one**, so the directional
  coalescing in `remote_chunk_append_body_locked`
  (`source/network/server_remote_chunks.c:290-316`, which only drops a queued
  body that the NEW body covers) removes nothing. Reproduce with
  `python -I -B tests/fixtures/mp_walk/replay_chain.py tests/fixtures/mp_walk/stream.bin`
  (output kept in `receipts/replay_chain.txt`).
- Received bytes vs processed packets: the tee received 337,282 server bytes
  and forwarded all of them to the client. The final read (tee
  `events.log`, `19735.3 s2c 334371 2911`) delivered bytes 334,371-337,281 in
  one chunk, 0.1 ms before the tee saw the client close (`c2s eof at 13764`).
  The capture cannot say how many of those bytes the client's socket read. What
  is established: the client stopped with
  `reason="remote Packet51 body chain exhausted"` (`receipts/run3_client_stderr.txt`),
  and the replay finds no overflow before offset 336,517 and an overflow there.
  The last 667 received bytes (after the end of that packet) are cut off because they
  play no part in the failure.
- `c2s.bin` is the complete, untrimmed outbound stream of the same session
  (Packet2/1 login, then Packet13/12 movement). It shows the walk going from
  (37.60, -70.87) to (80.27, -35.75).

## How it was captured (2026-10-12, 13:21)

- Server: Project Poseidon `poseidon-craftbukkit-1.1.12.jar` (Beta 1.7.3, protocol 14)
  in `tmp/poseidon`, already running from `tmp/poseidon/poseidon_host.ps1`.
  Settings: port 25565, view-distance 8, online-mode false, level-seed 8675309.
  Login and disconnect lines: `receipts/server_log_excerpt.txt` (lines 197-211
  of `tmp/poseidon/server.log`).
- Client: PC `build-verify/cavewii_v2.exe` built at HEAD `5496635e2` (heartbeat
  `build_id` `5496635_10.12`, `receipts/run3_build_receipt.txt`), headless,
  running the real `--cw-profile mp_lan_walk_10m` (autopilot walk) with
  `--cw-mp-endpoint 127.0.0.1:25570 --cw-mp-username WiiTuck` and
  `mp.install_radius = 4` (`receipts/run3_config.json`, line 74). 4 is the Wii
  default (`SERVER_REMOTE_ADMIT_RADIUS_DEFAULT`, `source/network/server_remote.h:88`);
  the PC default is 10.
- Recorder: `capture_tee.py` listens on 25570, forwards every byte unchanged to
  25565 and writes `s2c.bin`, `c2s.bin` and `events.log` (each received chunk is
  written to file before it is forwarded). `run_capture.ps1` runs the whole thing:
  `powershell -File tests/fixtures/mp_walk/run_capture.ps1 -RunDir <dir> -User WiiTuck -Seconds 90`.
- Original run directory: `run3` under this session's scratchpad
  (`<TEMP>\claude\<project-slug>\601ab4ec-1d04-4119-a33b-267f8aa3e9ca\scratchpad\run3`).
  That directory is temporary; the small text receipts from it are kept in
  `receipts/` (client stderr, pids/exit, config, tee events log, build id;
  the `.log` files were renamed to `.txt` so `.gitignore`'s `*.log` does not drop them).
  The 371 KB client stdout and the raw binaries are not kept; their hashes are
  in `receipts/SHA256SUMS.txt`.
- Trimming (the exact commands; their output hashes are in `receipts/SHA256SUMS.txt`):
  `head -c 336615 run3/tee/s2c.bin > stream.bin` and `cp run3/tee/c2s.bin c2s.bin`.
- What happened: the player logged in at (37.6, 65.0, -70.9), the saved position
  of the user `WiiTuck` from an earlier run. After 17 s of walking
  (server.log 13:21:20 login to 13:21:37 `disconnect.endOfStream`) the client logged
  `[mp] state=0 reason="remote Packet51 body chain exhausted"` and closed the
  socket. It did not reconnect (no second login in the server log).
- To split the stream into packets: `python -I -B tests/fixtures/mp_walk/parse_stream.py tests/fixtures/mp_walk/stream.bin [--all]`.
  The packet layouts follow the CFR `Packet*.readPacketData` bodies.
- What each body of column (12,-9) changes:
  `python -I -B tests/fixtures/mp_walk/body_changes.py tests/fixtures/mp_walk/stream.bin tests/fixtures/mp_walk/c2s.bin`
  (output kept in `receipts/body_changes_12_-9.txt`).

## Control runs (same rig, same build; streams not kept)

Only what the receipts show is stated here.
- run1: user WalkCap, no `mp.install_radius` key (PC default 10), ended by the run
  timeout (exit -1, killed; about 120 s). Server log 13:12:52 login to 13:14:51 `endOfStream`; client
  stderr (`receipts/control_run1_client_stderr.txt`) has no `state=0` line.
  Raw s2c 530,228 bytes.
- run2: user WiiTuck, no `mp.install_radius` key (PC default 10), ended by the run
  timeout (exit -1, killed; about 90 s). Server log 13:16:53 login to 13:18:23 `endOfStream`; client
  stderr (`receipts/control_run2_client_stderr.txt`) has no `state=0` line.
  Raw s2c 1,012,959 bytes.
- These show only that radius-10 sessions on this rig did not hit the chain
  error within their timeouts. They are not a controlled A/B (different start
  positions and walk paths), and their chain depths are not established here.
