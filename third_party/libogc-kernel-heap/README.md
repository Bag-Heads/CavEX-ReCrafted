# libogc lwp KERNEL_HEAP: 64KB -> 256KB (Wiimote attach fix)

This directory holds a patch set against upstream **libogc v3.1.0**
(`devkitPro/libogc`, tag `v3.1.0`, commit `c70bdf2fc36bf6c6c12d946ae3ad70d604366d1b`)
plus the sidecar build recipe that turns it into a drop-in `stage-lib/`
directory for the Wii `.dol` link. libogc itself is **not** vendored into
this repo (it is a whole SDK, not a header-only library) -- only the patches
and the reproduction recipe live here. The devkitPro toolchain install is
**never modified in place**; every build described here is a sidecar.

Two patches (0001 KERNEL_HEAP + 0002 the catnip build-environment necessity)
are the hardware-**certified** Wiimote-attach fix this file is named for; a
third (0003, `libogc/wiisd.c`) is a separate, **experimental,
hardware-unconfirmed** SD/FAT cold-boot-retry patch riding the same
sidecar pipeline -- see its own section below. The directory name stays
`libogc-kernel-heap` for the same reason the decision record below already
notes for the `logc128k`-named stage-lib dir: renaming a shipped path is
more churn than it's worth.

## Why: the Wiimote was dead, and it was never app code

Session 2026-08-23 (see `PROJECT_MEMORY.md` and the
`wiimote-dead-direct-launch-2026-08-18` / `wiimote-CONVICTED-2026-08-23`
session memory) traced a real-hardware bug where the Wiimote attached (LED
solid) but **never delivered a single input report**, on every direct-launch
boot path, for the game's entire life:

1. **The gauge.** An in-game overlay + telemetry field reading
   `__lwp_heap_getinfo` on libogc's lwp kernel workspace showed **4,932 bytes
   free out of the stock 64KB `KERNEL_HEAP`** at the point Wiimote attach is
   attempted -- both at the menu and in-world.
2. **The conviction.** wiiuse allocates its per-remote command queue *at
   attach time* (late, by design): `MAX_COMMANDS` (0x100) x `sizeof(cmd_blk_t)`
   (72B) = **18,432 contiguous bytes**, requested from that same lwp
   workspace. The allocation silently fails at `wiiuse/io_wii.c:275` (no
   error surfaces to the caller) whenever fewer than 18,432 contiguous bytes
   are free. 4,932 < 18,432, every time, on every boot path this game
   actually exercises -- so the remote pages, links, and then can never be
   *commanded* (no data-format request ever reaches it), which reads
   externally as "dead Wiimote." A control run of the stage-server binary
   (same toolchain, same libogc, far smaller app) accepted the same remote
   **instantly**, which is what pointed at a starved shared pool rather than
   a Bluetooth/pairing problem.
3. **The fix in principle.** `KERNEL_HEAP` is a hardcoded `#define` inside
   libogc itself (`libogc/system.c`, originally line 71) -- not a project
   config knob, not something `ogcsys.h` exposes, not something
   `LWP_InitQueue`-adjacent code can override at runtime. The only fix is to
   rebuild libogc with a bigger workspace.

## The 128KB round: proved insufficient, not just "smaller"

The first attempt doubled `KERNEL_HEAP` to 128KB, expecting roughly
64KB+64KB headroom. Measured on real hardware, free grew by only **+10,192
bytes** (to 15,124 total -- still under the 18,432-byte requirement) because
the moment more room existed, **roughly 55KB of *other* boot allocations
that had been silently failing against the 64KB pool started succeeding
too**, and ate most of the new headroom. This is the "fixed boot backlog,
not a scaler" finding: the workspace pool has a roughly constant ~115KB
worth of consumers once *any* pool large enough to satisfy them exists (see
below -- confirmed identical at 256KB), and 128KB simply wasn't past that
threshold.

## The 256KB round: hardware-certified

`KERNEL_HEAP` raised to `256*1024` (0x40000). Measured via the same
telemetry (`wkspT`/`wkspU`/`wkspB` fields, both PASS profiles
`heartbeat_selftest_200311` and `far_idle_2m_200720`):

| Field | Value |
|---|---|
| total (`wkspT`) | 262,100 B |
| used (`wkspU`) | 115,912 B (identical menu vs. world, identical to the 128KB round's used figure -- confirms a fixed backlog, not a scaling consumer) |
| free (`wkspB`) | **146,196 B in ONE contiguous block** (~8x the 18,432B wiiuse needs) |
| OOM events | 0 |
| fps | ~60 (unaffected) |
| MEM1 cost of the extra 192KB pool | in the noise |

Decision: **256KB is the shipped value.** It was chosen empirically (128KB
measured insufficient, 256KB measured with ~8x headroom and no measurable
MEM1/perf cost), not by doubling on a guess.

## What's in this directory

- `0001-lwp-kernel-heap-64k-to-256k.patch` -- the actual fix. One line in
  `libogc/system.c`: `#define KERNEL_HEAP (64*1024)` -> `(256*1024)`.
- `0002-catnip-drop-cube-preset-host-has-no-gamecube-toolset.patch` -- a
  **local build-environment necessity, not a libogc behavior change**. This
  development machine has no GameCube (`cube`) toolset installed, and
  catnip validates every preset in `DEFAULT` before building any of them, so
  leaving `cube` in `Catnip.cmake`'s `DEFAULT` list aborts the *entire*
  build (including the `wii` preset we actually want) before it starts.
  Drops `cube` from `catnip_package(libogc DEFAULT cube wii)` and comments
  out its `catnip_add_preset(cube ...)` line. If your machine has a working
  GameCube toolset, this patch is unnecessary (but harmless to keep -- it
  only removes an unrelated preset, not any libogc source behavior).
- `0003-wiisd-sd0-initio-cold-boot-retry.patch` -- **EXPERIMENTAL, HARDWARE-
  UNCONFIRMED** SD/FAT cold-boot-retry patch to `libogc/wiisd.c`. See the
  dedicated section below.
- `0004-tuxedo-clear-lazy-fpu-owner-on-thread-exit.patch` -- see the patch's
  own header.
- `0005-wiisd-sd0-functions-align64.patch` -- **the pre-FAT boot-death root
  cause (K12)**: one line in `libogc/wiisd.c`, `__sd0_functions[64]`
  `ATTRIBUTE_ALIGN(32)` -> `ATTRIBUTE_ALIGN(64)`. See the dedicated section
  below.
- `README.md` -- this file.

All patches (0001-0005) apply cleanly with `git apply` (or `patch -p1`) against a
pristine `v3.1.0` checkout; `tools/build_libogc_sidecar.sh` applies them
automatically.

## 0003: sd0_initio cold-boot retry (EXPERIMENTAL, hardware-unconfirmed)

**Status: fix candidate, hardware-unconfirmed -- requires a cold-boot PASS on
the physical rig.** Do not report this as fixed/done until that PASS exists
(see "What still needs to happen" below).

**Symptom.** This project's real-Wii test rig has hit **8/8 pre-FAT cold-boot
deaths** (PROJECT_MEMORY 2026-08-25, the `mp-s4-polish`/marathon-batch
sessions): every cold power-up attempt on the rig has failed before the
filesystem (`libfat`) ever mounted, distinct from the already-solved
KERNEL_HEAP Wiimote-attach starvation above (that bug let the Wiimote pair
but never deliver input; this one is earlier still -- the SD card itself
never finishes initializing). The rig's own "cold-reinit degradation"
hypothesis (repeated power-cycles on the same card behaving worse than a
fresh boot) matches a card-settle-timing failure mode, not an application bug.

**Evidence.** `docs/fork_and_prior_art_sweep_2026-08-26.md` (research pass B,
"SD/FAT cold-boot deaths") traced the prime suspect to libogc
`libogc/wiisd.c`'s `__sd0_initio()`:

1. It calls `__sdio_resetcard()` **unconditionally**, right at function
   entry, before any of the rest of the init sequence -- and the stock code
   never even looks at the return value.
2. **devkitPro sourceforge bug #94** documents real SD cards that fail
   exactly at this reset call immediately after Wii power-up (a cold-card
   settle-timing issue, not a card-compatibility one) -- and that removing or
   tolerating that failure let those cards initialize correctly afterward.
3. The stock main init retry loop (the ACMD41/`SEND_OP_COND` poll further
   down `__sd0_initio()`) only tries 10 times at a 10ms interval -- a 100ms
   total budget that leaves no meaningful settle time for a cold card.

Both observations point the same direction: a card that hasn't finished its
own internal power-up sequencing yet is being asked to respond long before
it's ready, and the stock code's response to any hiccup along that path was
to give up quickly rather than tolerate/retry it.

**What changes** (`0003-wiisd-sd0-initio-cold-boot-retry.patch`, against
`libogc/wiisd.c`'s `__sd0_initio()`):

- (a) The initial `__sdio_resetcard()` call is retried up to **3x with a
  100ms sleep** between attempts. If it still reports failure after all 3
  tries, that is treated as **non-fatal** -- the function falls through into
  the normal init sequence anyway, per bug #94's own evidence that some cards
  only succeed once the reset step is tolerated, not hammered on.
- (b) The main ACMD41 retry loop is widened from **10 tries @10ms** to **20
  tries @200ms** (a 100ms worst-case budget becomes a 4s one), giving a cold
  card real settle time before init is declared failed.
- (c) A new static counter, `sd0_init_attempts`, is bumped once per
  `__sd0_initio()` entry, readable via a small non-static accessor,
  `u32 sd0_get_init_attempts(void)`. **This patch does NOT wire that accessor
  into any game-side boot-phase band/breadcrumb/telemetry code** -- it is an
  accessor only, added so a future rig session *could* report how hard SD
  init had to try (a caller would declare
  `extern u32 sd0_get_init_attempts(void);` and read it once this experiment
  is confirmed).

**What stays untouched:** every other code path in `__sd0_initio()`
(host-controller reset, clock/timeout setup, CSD/CID/bus-width negotiation,
the `fail:` cleanup label) is byte-identical to stock v3.1.0. This is a
libogc SDK/driver-layer change, not a CaveWii simulation change, so the
worldgen/redstone/fluid/recipe/lighting byte-exactness rules in the top-level
`CLAUDE.md` do not apply to it -- but the same "don't claim a fix without
hardware evidence" discipline does.

**What still needs to happen:** a cold-boot proof round on the physical rig
(power the Wii fully off, then on, into this sidecar-linked `.dol`, repeated
across multiple cold cycles) producing a `tmp/wii_runs/**/PASS.json` for a
cold-boot profile. Until that PASS exists, this is a **fix candidate**, not a
confirmed fix -- the rig's next session should build with this sidecar
(`tools/build_libogc_sidecar.sh`, which now applies and gates this patch too)
and run the cold-boot repro before anything gets reported as resolved.

## 0005: `__sd0_functions` must never lie across a 4 KB line (pre-FAT boot death, K12)

**Status: root cause isolated on real hardware 2026-09-17; the patch boots;
the N=10 cold-boot acceptance round on a frozen artifact is still owed.** Until
that round exists the honest label is "fix candidate, hardware-unconfirmed".

**Symptom.** A freshly linked `.dol` hangs before FAT mounts: zero boot-trace
lines on the card, no network, the telemetry collector "could not connect",
only a hard power cycle recovers. It came and went with innocent commits and
tracked the image END (`__bss_end` page offset 0x3dc), which is why it was
first read as an arena/argv/layout problem.

**Cause.** libogc v3.1.0 `wiisd.c` declares
`static u8 __sd0_functions[64] ATTRIBUTE_ALIGN(32);` and hands it RAW to the
SD host controller as the DMA data buffer of CMD6 SWITCH_FUNC
(`__sdio_sendcommand(..., 1, sizeof(__sd0_functions), __sd0_functions, ...)`
-> `request->dma_addr = buffer; isdma = 1` -> `IOS_Ioctlv`) during SD init.
When that 64-byte buffer lies across a 4 KB address line the call never
returns. A 64-byte object on 32-byte alignment straddles in exactly ONE
32-byte slot per 4 KB, and libogc's `.bss` tail is rigid relative to
`__bss_end` -- so the death looked like "one bad `__bss_end` slot".
*Why* IOS never completes that transfer is a HYPOTHESIS (SD host SDMA 4 KB
buffer-boundary handling for a sub-block transfer); multi-block reads into
caller buffers cross 4 KB lines routinely and work.

**Evidence (real Wii, `heartbeat_selftest`, same program content, predictions
written before each run; full record `tmp/jobs/bootdeath-0917/EXPERIMENT-3.md`):**

| layout | `__sd0_functions` | `__bss_end` | result |
|---|---|---|---|
| dying base, +16 variant, +4096 copy | 0x806dffe0 / 0x806e0fe0 (straddles) | slot ...3dc | DIES 5/5 |
| -32, +32, +256 variants, three other builds | no straddle | various | boots, every run |
| **4c** experiment-only shim sidecar: straddle WITHOUT the slot | 0x806dffe0 (straddles) | 0x8070b3fc (a slot that boots) | **DIES 2/2** |
| **4b** same sidecar: the slot WITHOUT the straddle (every object above the array at the dying build's address) | 0x806dffc0 | 0x8070b3dc (the dying slot) | **BOOTS 2/2** |
| **4a** this patch | 0x806e0000 | 0x8070b3fc | boots |

Of 575 `.bss` symbols above the game's layout pad, `__sd0_functions` is the
only one whose size and alignment reproduce the one-slot-per-4 KB pattern.

**The fix.** A 64-byte object aligned to its own size cannot cross any
power-of-two line >= 64. One line; no behaviour change.

**The gate.** `tools/check_sd0_dma_buffer.sh` (called from the `.dol` rule and
`build-verify.sh`) refuses a linked image unless exactly one `__sd0_functions`
of size 0x40 exists, is 64-byte aligned, and does not cross a 4 KB line. A PASS
proves the layout invariant, not the provenance of the library.

**Patch 0003 (cold-boot retry)** was written against this same symptom before
the cause was known. Status per Astra 2026-09-17: *not demonstrated necessary
for K12; independent retry benefit unproven.* It stays unchanged through the
0.1.0 release; a controlled removal comparison is scheduled afterwards.

**Anything else linked against libogc** (for example
`tools/wii_stage_server`) can hit the same hang when it is relinked; build it
against the patched sidecar too.

## Reproducing the sidecar build

Use `tools/build_libogc_sidecar.sh` (repo root). It clones libogc v3.1.0
fresh, applies both patches, builds it inside the exact MSYS2 environment
this fix was proven in (see that script's header comment for the four hard
traps: MSYS-not-Windows cmake, env not crossing shells, the cube-preset
abort, and the short-path requirement), objdump-gates the resulting
`libogc.a` for the `lis r3,4` (0x40000) literal feeding
`__lwp_wkspace_init`, verifies the artifact's mtime actually advanced, and
assembles a `stage-lib/` directory ready for `LIBOGC_LIB=`. See
`docs/wii_libogc_kernel_heap_sidecar.md` for the full standing procedure and
the exact `make ... LIBOGC_LIB=...` command line.

```bash
tools/build_libogc_sidecar.sh
# defaults: OUT_DIR=<repo-analysis>\logc256kr
#           LIBOGC_SRC=<repo-analysis>\_libogc_audit
#           LIBOGC_REF=v3.1.0 (falls back to cloning
#           https://github.com/devkitPro/libogc directly if LIBOGC_SRC
#           doesn't exist locally)
```

## Decision record

- **2026-08-23 (night):** root cause proven on hardware (4,932B free vs.
  18,432B needed); fix identified as a libogc rebuild, not app code.
- **2026-08-23 (night, same session):** 128KB tried, measured insufficient
  (15,124B free, still short) -- the "fixed ~55KB backlog" finding.
- **2026-08-23 (night, same session):** 256KB built and hardware-certified
  (146,196B free / 1 block, both proof profiles PASS). Shipped as a sidecar
  (`../logc128k` stage-lib, despite the directory's name) because the
  session's classifier forbade writing to `/opt` (the toolchain install)
  even under maintainer go-ahead.
- **2026-08-24:** made repo-reproducible -- this directory + the sidecar
  build script + `docs/wii_libogc_kernel_heap_sidecar.md`. The toolchain
  install is still never modified; every real Wii build that needs the fix
  passes `LIBOGC_LIB=<stage-lib>` to `make`/`build-verify.sh`.

- **2026-09-17:** pre-FAT boot death (K12) root-caused on hardware to
  `__sd0_functions` lying across a 4 KB line (double dissociation, section
  0005 above); patch 0005 added. Astra ruling 2 of `tmp/jobs/bootdeath-0917/`.

## Upstream disposition

Patch 0005 looks like a genuine upstream libogc defect (any homebrew whose
`.bss` happens to put `__sd0_functions` across a 4 KB line would hang in SD
init). Reporting it upstream is an outward-facing action and needs the
maintainer's explicit go-ahead; nothing has been sent.


Not proposed upstream (yet). `KERNEL_HEAP` genuinely needs to stay
conservative for GameCube (24MB total RAM, no MEM2) -- a blanket 256KB bump
in stock libogc would be wrong for that platform. The right upstream fix is
making `KERNEL_HEAP` configurable (an env/CMake cache var defaulted per
platform, `HW_RVL` vs GameCube) rather than a hardcoded `#define`; until
that lands, or until this repo's own boot-allocation footprint against the
lwp workspace shrinks well under the 64KB stock pool, this sidecar is the
standing build requirement for any real-Wii build.
