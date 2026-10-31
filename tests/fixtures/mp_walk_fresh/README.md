# mp_walk_fresh fixture: fresh-terrain edit flood vs a control walk (REL-MP)

Two inbound (server to client) byte streams of real Beta 1.7.3 server
sessions, recorded on PC on 2026-10-18 for REL-MP-DIAG. The diagnosis is in
`tmp/jobs/loop/REL-MP-DIAG/DIAG.md` (sections 2-4). Both are replayed
byte-exact by the existing `test_mp_walk_session` binary (IN-008 harness,
production transport + pump, admit radius 4):

| ctest | file | bytes | packets | sha256 |
|---|---|---|---|---|
| `test_mp_walk_session_fresh` | `stream.bin` | 750,820 | 7,997 (427 Packet51) | `bf5d3aa0c272425b12b51756ae5dfdad221acc759d2040eed844b0ce07bacc20` |
| `test_mp_walk_session_fresh_control` | `control.bin` | 1,905,110 | 27,252 (1,212 Packet51) | `33bc1c95f189433b3b366cbca357baab930f7dc274ae59ceadd8dcef8ac10cc2` |

Both files are the complete, untrimmed `tee/s2c.bin` of their runs:

- `stream.bin` = `tmp/jobs/loop/REL-MP-DIAG/runs/pcwalk_eastfresh_1018_143702/tee/s2c.bin`
- `control.bin` = `tmp/jobs/loop/REL-MP-DIAG/runs/pcwalk_control_1018_144402/tee/s2c.bin`

## What they contain

- `stream.bin` (RED before REL-MP-FIX): the client walks into chunks the server
  generates for the first time. 18 s of session: 2,955 Packet53, 1,067 Packet52
  (2,793 cells), 53 partial Packet51 and 179 falling-gravel spawns (Packet23
  type 71); the busiest columns are almost all still lava (id 11). Beta sends
  this on purpose: caves below y 10 are filled with flowing lava
  (`MapGenCaves.java:128-129`), each cell later settles to still lava
  (`BlockFlowing.java:29-33`), and `PlayerInstance.onUpdate` sends a Packet53
  for 1 change, a Packet52 for 2-9 and a partial Packet51 for 10 or more
  (`PlayerInstance.java:133-160`, CFR b1.7.3 server decompile). Edits for
  columns outside the admit radius park in the deferred-edit log; the columns
  that keep them hold a full head plus 1-3 queued partial bodies, compaction
  skipped every chained column, and the log ended the session. On unchanged
  product code the replay prints `REL-MP RED: mp_end=6 reason=remote deferred
  edit log exhausted ...` at packet 7996 and exits 1.
- `control.bin` (PASS): the same walk with no pruned chunks. 190 s, 255 Packet53,
  9 Packet52, 5 partial Packet51, 0 falling blocks. It guards that a fix keeps
  an ordinary session identical: full consumption, PLAY, mp_end 0, no reason,
  no forced reconnect.

## How they were captured

- Rig: `tmp/jobs/loop/REL-MP-DIAG/run_pc_walk.ps1`. A private copy of
  `tmp/poseidon` (Project Poseidon, Beta 1.7.3 protocol 14, same jar,
  `poseidon.yml`, `bukkit.yml`, a copy of the live world, level-seed 8675309,
  view-distance 8, offline mode) on port 25571; the live server was not used.
- Recorder: `tests/fixtures/mp_walk/capture_tee.py` on port 25572, which writes
  every server byte to `s2c.bin` before forwarding it.
- Client: PC `build-verify/cavewii_v2.exe` at `edfade68a`, `--cw-profile
  mp_lan_walk_10m --cw-seed 12345 --cw-mp-username WiiTuck`,
  `mp.install_radius=4` (the Wii default, `SERVER_REMOTE_ADMIT_RADIUS_DEFAULT`).
- Start position: hw1018's login point (-53.79, 65.25, -102.08), set with the
  rig's `tools/world_prep.py`. For `stream.bin` the rig dropped the 355 chunks
  with cx >= 2 from the world copy (`-PruneCxGe 2`), so the server regenerated
  them from the seed while the client walked east into them. `control.bin`
  used the same command with no pruning.
- Receipts (client stderr, server log, tee events, replay and stream census)
  stay in the two run directories above.
