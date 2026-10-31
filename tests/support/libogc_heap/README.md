# Shared-carve allocator proof

`displaylist_finalize_wii.c` includes this harness in the existing
`test_displaylist_vertex_equiv` target; no new engine build profile is used.

`lwp_heap.c.inc` contains the actual init/allocate/free/getinfo bodies from
the project's Wii sidecar at
`<repo-analysis>/logc256kr`, commit
`c70bdf2fc36bf6c6c12d946ae3ad70d604366d1b` (libogc v3.1.0).
The sole change to that file is disabling its platform includes; original
SHA256 is `bb2dd70f3702051055dba663331495ae320728dda92dbfce8717e92aa4211be0`.
See the accompanying upstream LICENSE. This is test-only vendoring.

`host.h` adapts libogc's helpers to native host pointers and explicit
8-byte used-block tags. Sentinel addresses are expressed relative to the
first/perm_null fields so their next/prev aliases work on a 64-bit host.
User-pointer subtraction uses host-width arithmetic rather than PPC32 wrap.
Free blocks are 24 bytes on this host versus Wii's 16; the allocator splits
only remainders greater than 40 bytes and production shrink returns whole
32-byte pages, so that difference changes neither admissible split here.
The production `mem2_shrink.inc` is included directly. ISR masking is a no-op:
the deterministic replay is single-threaded, as the Wii carve is serialized
by its mutex/interrupt critical section. This proves allocator semantics,
not PPC instruction timing or general-heap behavior.

`series.h` preserves every one of the 411 `dln`/`dls` heartbeat samples in
`split_anchor_6m_1f7e3d51_s42_d256_fv6_voxel_0906_221025`; its source hash is
in the header. **These are aggregates, not a captured per-list histogram.**
The explicit synthetic mix is 4, 8, 16, 32, 64, 128, 256, 512, 768, 1024
vertices, alternating actual Wii 9/12-byte emission. Resident counts follow
the series exactly (peak 6451); remeshed lists take a vertex-weighted share
of the sample's mean slack, rounded to 32 bytes and including the old pad.
One sixteenth of resident lists remesh each checkpoint; new lists enter and
departing lists free immediately. Consequently aggregate resident slack need
not equal that heartbeat's slack exactly: retained lists keep their previous
requests. This is a stated workload model, not a fabricated allocation trace.

Eight full series replays give 3288 checkpoints per mode in a shared 36 MiB
carve. The baseline retains finalized capacity and an 8192-page overlay;
the lever run calls the real finalizer/tail release and uses 256 pages.
Both run the identical column (81920 B), far-slice (10240 B), and page
(256 B) allocation/free script. A 4 MiB live-byte fallback budget models
the reserve; host malloc backs spills. General-heap shrink conservatively
retains the old allocation, since its Wii malloc placement is not emulated.

Spills count actual failures of `__lwp_heap_allocate`; mesh truncations count
the real emitter's `truncated` flags, following the mesher's per-list rule.
They are harness counters, not invented readings of PC's always-zero MEM2
platform counters. Canaries, tags, links, no-adjacent-free-block invariants,
total free and largest block are checked at every checkpoint. Both complete
teardowns must coalesce to the original single 37,748,728-byte free block.
A direct tail-range assertion proves a far slice can occupy released list
space; the replay additionally counts page/tenant allocations wholly inside
recent released tails. That count excludes merged-hole reuse it cannot prove.

The assertions require spill and mesh-trunc counts no worse than baseline at
**every** checkpoint, total-free no worse, and largest-block regression no
more than **1 MiB**. First-fit placement is not monotonic: do not describe this
test as proving largest-free dominance. Exact measured regression and minima
are printed by the executable and recorded with the fix validation evidence.


## Round 2: low-headroom diagnostic and evidence limits

The default 36 MiB test remains the original **synthetic** regression; it is
not a hardware safety certificate. The test now probes real late carve
admissions at every checkpoint (12,352; 32,768; 65,536; 131,072; 262,144;
524,288; 1,048,576; 2,097,152 bytes, plus the production 256-byte guard).
These are stress requests, **not claimed observed list maxima**. Probe metadata
is restored after each admission, so probes cannot reorder the free list or
alter the subsequent replay. The shared 4 MiB fallback is the production
MEM2_RESERVE_MARGIN budget model; it does not reproduce malloc fragmentation
or other allocations consuming that reserve. Probes specifically test carve
admission, not the success of the subsequent general-heap fallback.

Run the low-headroom diagnostic with `CW_CHURN_LOW_HEADROOM=1` in the process
environment. It uses 33,905 KiB, the recorded boot free-carve reading (KiB
telemetry itself loses sub-KiB precision). **The current lever implementation
fails its no-new-admission-failure assertion.** Do not count that diagnostic
as passing because the default suite is green. Optional
`CW_CHURN_MIN_TAIL=<bytes>` skips smaller shrinks in the **harness only**, for
candidate-bound experiments; it does not change production shrink policy.
The direct coalescence fixture requires a threshold no greater than 31,744.

With the production shrink policy, 3,288 checkpoints per mode yield **329
baseline-successful carve probes that fail after the levers**. The first is
zero-based step **461**, pass 2, sample **50**, request **12,352 B** (+256 B
guard): largest-free **15,256 -> 11,312 B**, lever total-free **4,756,968 B**.
This is a fragmentation finding in the modeled order, not an observed Wii OOM.
A general-heap spill may rescue the request; reserve fragmentation is unmodeled.
Candidate minimum-tail bounds of **4 KiB and 16 KiB both fail** (first failure
step 458, sample 47, request 12,352 B; 73,184 -> 7,704 B largest-free).
Neither is a justified production fix. A minimum-tail bound alone has not
been proven sufficient: it changes first-fit placement, and even skipping all
small list tails leaves the overlay reserve-size change. Keep this finding
open until an actual allocation trace permits a validated bound; do not
silently ship one of the rejected thresholds.

All three requested captures were inspected (411 + 441 + 446 = **1,298**
heartbeats). They contain aggregate heartbeat events, not alloc/free/resize
records. Specifically:

- `dln` counts lists; `dls` sums capacity slack including `dlp`. They contain
  neither list capacities/used sizes nor identities. Both 65af7cf6 runs have
  `dls=dlp=0`; their pre-lever capacities cannot be reconstructed.
- `oph` is a boot high-water mark, not a page allocation/free sequence.
- `db`/`ds` are sampled counts, so net deltas cannot reveal intervening
  destruction/replacement or which tenant survives a checkpoint.
- `chunks` is `chunks_rendered`, not resident allocation count.
- `far_storage` is the storage enum; `far_missing`/anchor far fields describe
  coverage, not the identities, byte sizes, or lifetimes of far allocations.

Consequently **captured tenant lifetimes/order and the largest observed list
are unavailable**. The batch/interleave model has deliberately not been
relabeled as captured order. This diagnostic closes neither that modeling
gap nor the requested three-run captured-order proof. Required extra evidence:
ordered alloc/free/resize/finalize events with stable allocation identity,
requested/retained size, tenant tag, carve/spill outcome, and initial live
allocations/reserve state. Existing `carve_note` producers identify the event
seams, but these runs did not record those events. No capture is regenerated.
Source SHA256s, selected aggregates, diagnostic logs and reproduction scripts
are saved under `tmp/levers_fix2/`.

## Round 3: compaction is opt-in

The production default is now **OFF** on both backends. Set
`"memory": { "displaylist_compact": 1 }` in config.json before launch to opt in
(merge into an existing memory object if present). Use integer 0 to disable;
an absent setting defaults to 0. No vertex bytes, GX call ranges, simulation
rules, column sharing, or overlay recovery transitions change. Runtime toggling
through a UI is not provided; configure before workers start.

`dlc=1` is appended only when compaction is explicitly enabled; flag-off
storage heartbeats stay byte-identical to the pre-lever stream. The collector
reports its observed range; flag-off and old captures say unavailable. There is no arena,
so arena high-water/overflow fields are not applicable and are not fabricated.
Continue comparing dln/dls/dlp, MEM2 largest-free/spill, oom and mesh_trunc.

With `CW_CHURN_LOW_HEADROOM=1`, the regression harness now isolates lever 2:
both sides use the same 256-page overlay reserve and compaction defaults OFF.
It requires zero new probe failures at all 3,288 checkpoints. Add
`CW_CHURN_COMPACT=1 CW_CHURN_COUNT_ONLY=1` for the deliberately failing opt-in
diagnostic (645 new failures; first at zero-based checkpoint 462, sample 51,
request 32,768 B). Count-only suppresses only the regression assertions so the
report reaches the end; all other safety assertions remain active. Add
`CW_CHURN_LEGACY_BASELINE=1` to compare against the historical
8192-page overlay baseline: OFF has 469 failures, ON reproduces the original
329. The baseline change is explicit: disabling lever 2 removes its own tail
fragmentation, but does not establish safety for lever 1's changed placement.
Do not describe the isolated pass as clearing the combined-lever finding.

The normal 36 MiB suite still explicitly exercises compaction and the original
combined baseline. Finalizer equivalence covers enabled, disabled, and failed
shrink paths. Rejected arena/tail-pool/hole-threshold experiments and detailed
carve arithmetic are in `tmp/levers_fix3/VALIDATION.md`. Default compaction
returns 0 KiB; the overlay change still returns 1,984 KiB, instead of the prior
1,984 plus approximately 1,700 KiB median list slack. This remains a console
candidate, hardware-unconfirmed.
