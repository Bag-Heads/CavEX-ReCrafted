> **2026-07-30b RESPIN** — toast now expires (u64 wrap fix) and sits ABOVE the hotbar;
> the water-still bake is REVERTED (fork water art again — the bake read as pale speckle
> with a seam on hardware). Open items from the same session: the crash returning to world
> 120 (NO watchdog report on card → suspect the fail-closed world-load path), and no
> hostile/passive mob spawns.
>
> **2026-07-30 REBUILD — READ FIRST.** This card now carries build `ce3141b_07.30`
> (md5 `850b81b7d00781ef881307f103dac3f3`), a CLEAN-tree build of the 14-commit fix batch
> for the 2026-07-29 play-session findings. New things to verify on top of the runs below:
> portal/lava/fire/WATER-still as Beta filmstrips (flowing water is still fork art — known
> gap, not a regression); a LEVER beside a powered rail flips its texture and powers 9
> rails; a cart on an UNPOWERED powered rail STOPS; the chest window has no empty rows and
> a double chest opens as "Large chest" in the same GUI; a bed at night sleeps ~5 s THEN
> dawn (daytime click shows a toast); save-and-quit does not freeze or flash a banner; and
> **hold − then press + on the Wiimote to hide/show the F3 overlay** (the two stray hotbar
> scrolls cancel). STALE ITEMS CORRECTED BELOW as of this build: the cell-bar diagnostic is
> ELEVEN rows (not six); the MEM2 carve is 36MB with audio in its OWN region (row 9
> spill/oom now measure display-list pressure only); F3 row 18's `ilist`/`cguard` and row
> 16's `bail` must stay 0. Reports now write `hang_report.txt` + `hang_report_2..8.txt` —
> collect them ALL; `up=` is real uptime now.

# CaveWii_v2 hardware test card — 2026-07-28 proof package (audio build)

Boot each demo ALONE (one config per boot), pull the `*_result.txt` verdict
off the SD root afterwards. Every demo is self-driving (no input past the
Homebrew Channel launch) and self-grading: the final line of its result file
is `VERDICT PASS|FAIL|SKIP`. Screenshots (`ss_*.png`, first ~40s) land on the
SD root too.

**This build adds the whole audio stack (S0 engine + S1 pack pipeline + S2
event wiring + the 32kHz cap), so every run is ALSO a first-listen run: stay
in earshot for each run's listen window — do not walk away at boot.** Use a
stereo TV or headphones (the pan effect dims the far ear at most 50%; mono
hides it).

## Prep (before the first boot — all three matter)

1. **Provision the SD**: run `tools/provision_wii_sd.ps1` against the card.
   This stages the fresh `boot.dol`, the repo `assets/` (the 46-key
   placeholder sound tree is NEW — without it missing keys are silent), and
   `texturepacks/beta-1.7.3/` (the REAL Beta pack incl. the 32kHz-capped
   sound tree — staged in the repo, gitignored). All 6 configs already point
   `paths.texturepack` at `texturepacks/beta-1.7.3`.
2. **Verify the build**: SD `apps/CavEX/boot.dol` md5 must be
   `850b81b7d00781ef881307f103dac3f3`. In-frame F3 stamp must read
   `ce3141b_07.30` — photograph it on the FIRST frame of every run; any
   other stamp = wrong build, stop and restage.
3. **Sound sanity**: at the menu you should already hear the GUI click on
   button presses — from the REAL Beta pack, not synthetic beeps. Obvious
   sine-beep placeholder sounds = the pack didn't stage; check
   `texturepacks/beta-1.7.3/sound/` exists on the SD.

## The 5 runs (priority order) + what to LISTEN for

| # | config | verdict file | wait | proves | listen for |
|---|--------|--------------|------|--------|------------|
| 1 | `configs/nether_roundtrip.json` | `nether_roundtrip_result.txt` | ~6 min | saved-chunk reload + DIM-1 loading (3 real portal travels, byte-exact markers both dims) | `portal.trigger` at each dwell ramp start + `portal.travel` at each of the 3 switches (both centered/unattenuated), eerie portal hum near the lit pane; note the world-swap hitch length |
| 2 | `configs/world_reload_create.json` then REBOOT with `configs/world_reload_verify.json` | `world_reload_result.txt` (per boot) | ~2 min each | cold power-cycle world load (markers + item survive; the region-fsync fix on real SD) | — (quiet demo; menu click only) |
| 3 | `configs/tnt_chain.json` | `tnt_chain_result.txt` | ~3 min | TNT chains under the Wii 2-blast/tick cap, no lost deferred blasts | fuse hiss (first + 5 chained re-primes), 6 distinct booms marching across the line; booms must PAN in the direction the chain visibly advances; no clipping when close; no crackle on the high-pitched sounds |
| 4 | `configs/piston_sticky.json` | `piston_sticky_result.txt` | ~2 min | sticky retraction pull (the one piston path with NO hardware observation) | `tile.piston.out` at ~10s (extend), `tile.piston.in` ~5s later (retract), each matching the visible motion |
| 5 | `configs/soak_perf_stress.json` | `perf_log.txt` (no verdict file) | 30+ min | the 30-minute session (freeze classes only appear sustained; Dolphin cannot certify Broadway timing) | constant mob idle voices (zombie moan / skeleton rattle / spider hiss) + footstep patter from the ring; **the rotating radius-28 blasts must audibly CIRCLE you in the same direction the craters advance — this is the pan-sign oracle**; occasional splash/thud/fizz near craters is bonus; any cave-mood sound must be short and normal-pitch (a slowed drone = the 32kHz cap failed) |

Run-2 rule unchanged: do NOT delete the world between its two boots — swap
the config and reboot; the SD persists.

## Always-on watches (every run, photo where noted)

- **F3 row 9** (`MEM1 free .. MEM2 free .. | spill .. oom .. trunc ..`):
  `spill`/`oom`/`trunc` must stay **0 and flat** — photograph periodically
  during Run 5. This is THE number the audio arc put at risk (12MB audio
  arena now lives inside the 40MB MEM2 carve). Ignore "MEM2 free" looking
  huge — it reads the region ABOVE the carve, not the carve.
- **F3 row 20** (`snd: arena N/NKB voices N`): arena used should be ~10.1MB
  with the Beta pack; `voices` must fall back toward 0 in quiet moments over
  the 30 min (pinned/climbing = voice-reap failure); listen for chopped
  sound tails.
- **F3 rows 1 + 19 together** (fps/gpu wait + `srv tick rt/wt`): photograph
  in steady state AND during a blast burst in Run 5 — this is the
  instrument-grade perf capture BASELINE.md still lacks (which of rt/wt vs
  gpu-wait dominates is the deciding perf number).
- **Heap drift**: note `heap: used` at ~minute 2 and minute 30 of Run 5;
  steady climb on a stable scene = leak.
- **After EVERY run** (pass or fail): list the SD root for
  `fbguard_report.txt` — its mere presence is a finding (absorbed XFB
  overrun); bring the whole file.
- **On any freeze/panic banner**: pull `sd:/hang_report.txt`. If the banner
  painted but NO file exists, photograph the on-screen cell-bar diagnostic
  (six bars from y=84: main/server/worker phase, canary, dlbad+dlcall,
  gpreset — every 5th cell darker) — that photo is the whole diagnosis.
- **Wiimote sleep**: during Run 5 let the Wiimote sleep and resync once —
  the player must not move/turn by itself at either moment.

## Attended segment (~15 min with controller in hand — high value, optional)

Only possible with a human holding the Wiimote; these have NO other path:

1. **After Run 2's VERIFY verdict** (wait for ~t=60 so the check ticks
   first): walk the ~6 blocks to the restored planks and collect them —
   first on-device `random.pop` pickup + PLAYER footsteps.
2. **Redstone ncas read**: place/break blocks next to a wire run with F3 up;
   row 10's `ncas` (worst notify-cascade µs) must stay far below ~3,000,000.
3. **Feel-test** (playability doctrine): fight a zombie (fair reaction
   window at Wiimote speeds?), press a skeleton (reads as ranged threat?),
   judge swing cadence.
4. **LOD radius dial**: with the overlay forced, hotbar +/- raises the
   full-detail radius from 2 — note the radius where fps first leaves 60;
   that number sets the real default (chosen blind until now — Dolphin
   can't see the GPU ceiling).
5. **Pack-swap drain**: open the packs screen mid-sound and swap packs — no
   crash, no stuck voices, audio resumes from the new pack.
6. **Pan static check**: stand with a sound source ~90° to your LEFT — the
   LEFT speaker must be louder (subtle by design). Reversed = report; it's
   a one-line sign fix.

## Beyond this card — the PASS.json ledger (needs the telemetry rig, second session)

The `*_result.txt` verdicts above do NOT satisfy the repo's hardware-proof
gate (`require-hw-proof.py` wants `tmp/wii_runs/**/PASS.json` from the
telemetry flow). Until at least one profile-matched PASS.json exists, every
Wii-risk path stays "fix candidate, hardware-unconfirmed" by repo rule. In
priority order, each a dedicated boot via `run_staged_wii_hardware.ps1` +
`tools/wii_telemetry_server.py`:

1. `heartbeat_selftest` — also proves the telemetry socket is genuinely
   non-blocking on real IOS (kill the PC collector mid-run: the F3 fps row
   must NOT stall) and the CWRUN1 MEM2 chainload.
2. `watchdog_inject` — certifies the fault-capture path itself; until it
   passes, "no hang_report.txt" is not a trustworthy negative.
3. `redstone_clock_2m` — the notify-yield freeze fix has never been
   Broadway-certified (the maintainer's "redstone works" is explicitly not
   this).
4. `far_idle_2m` / `far_walk_6m` / `perf_stress_5m` — the M1 freeze fixes +
   far-ring MEM1/default gate (VD6 decision needs `fring=1` heartbeats).
5. A far/perf profile **in DIM-1** — live Nether generation on the 729MHz
   core has zero on-device evidence.
6. The 2h+ soak — the 30-min run is "the first data point, not the
   certificate" (BASELINE.md).

Never fall back to HBC wiiload upload on this rig (known black-screen
wedge); the staged SD path is the sanctioned launch.

## NOTES (final card build)

- boot.dol md5: `850b81b7d00781ef881307f103dac3f3`
- F3 stamp (verify in-frame on every capture): `ce3141b_07.30`
- Built from commit 724f71e (clean tree) — includes S0 audio engine, S1
  sound-pack pipeline, S2 full event wiring, S2b 32kHz rate cap (all 13
  cave ambience variants fit the arena; the two 192kHz cave files would
  have played SLOWED on this hardware before the cap), the region-fsync
  durability fix, and everything through M4 animals.
- Dolphin re-prove of THIS exact boot.dol (ff76d2b6), 2026-07-28: all four
  verdict demos VERDICT PASS (piston_sticky, tnt_chain, world_reload
  create+cold-verify, nether_roundtrip 5/5 markers) + a bounded soak run
  clean (no hang_report/fbguard_report, chunks flat, log alive to the end).
  One watch item for the console: the Dolphin soak's late steady `srv tick`
  read ~17ms (prior 36-min soak settled ~2ms; windows differ and Dolphin
  timing is not the instrument) — the real number is your row-19 photo.
  Evidence in tmp/reprove_* and the :8099 gallery.
