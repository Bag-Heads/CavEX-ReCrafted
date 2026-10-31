/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef CHUNK_MESHER_H
#define CHUNK_MESHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHUNK_MESHER_QLENGTH 8

// T1-d (2026-09-03 far-ring frame-drop root cause, mechanism A): pure PACING
// cap on how many mesh jobs chunk_mesher_send() may have in flight at once
// (chunk_mesher_pending()) -- separate from CHUNK_MESHER_QLENGTH (the token
// POOL size, unchanged: still 8). The root-cause telemetry table
// (docs/perf_frame_drops_rootcause_2026-09-03.md) found mesh_q in {7,8} to be
// only 15% of sampled frames but 6/6 of the gpu_ms > 16.6ms GP-class events --
// the saturated mesher's MEM2 grow/flush churn (chunk.c's GX_CallDispList
// reads from the same MEM2 the mesher's displaylist writes/flushes into)
// contends with the GP's own MEM2 streaming reads. Capping the backlog to 4
// halves that worst-case overlap without dropping any work: chunk_mesher_send
// returning false leaves rebuild_displist set (chunk.c:432-437), so a capped
// chunk simply retries next frame -- same final mesh bytes, submitted later.
// Set to CHUNK_MESHER_QLENGTH (8) to reach today's uncapped behaviour exactly
// (the cap becomes a no-op: chunk_mesher_send's own empty-token gate is then
// the only limit, as before this change).
//
// H-1 HOTFIX (2026-09-03 responsiveness lane): restored to 8 (uncapped).
// Capping to 4 was sized against a worker that was ALSO paying up to ~25ms of
// dead post-commit sleep per build (see CHUNK_MESHER_SLICE_MS's H-1 note) --
// with that sleep gone, a 4-deep backlog cap throttles throughput to roughly
// one chunk per (build + accumulated slice-overshoot) for no remaining
// benefit: the MEM2-churn-vs-GP-streaming contention this cap was fighting
// was actually dominated by the emit-loop building AND then holding the CPU
// asleep past the finalize, not by queue depth. Re-tune this independently
// (with a fresh measurement) if the H-1 emit-loop fix alone doesn't resolve
// the mesher-can't-keep-up symptom; don't reintroduce a queue cap to
// compensate for a solved sleep-placement bug.
#define CHUNK_MESHER_ENQUEUE_CAP CHUNK_MESHER_QLENGTH

// P2-b / T1-e (2026-09-03 round-2 root-cause doc, "#2 Post-load spikes"), H-1
// HOTFIX (2026-09-03 responsiveness lane): cooperative slice budget for
// chunk_mesher_build's emit phase (chunk_mesher_rebuild's per-y-layer
// checkpoints, via mesher_emit_slice_tick). At the mesher worker's priority
// (32, well below main/server's 64), it only ever RUNS while main is blocked
// -- so its whole per-chunk duty cycle lands inside whatever window main
// happened to be waiting on, and a build that runs uninterrupted for the GP's
// whole vsync/flip window makes its MEM2 churn (grow/flush) contend with the
// GP's own MEM2 display-list streaming for that entire window. Once emit work
// since the last yield (or build start) has passed this threshold, the worker
// gives up the CPU with a REAL sleep (thread_msleep) rather than
// LWP_YieldThread -- yield only rotates EQUAL-or-higher-priority READY
// threads (server_tick_pacing.h's SERVER_TICK_OVERRUN_SLEEP_MS comment
// documents the same libogc rule from the other side), and at priority 32
// there is normally nothing else at 32 waiting to rotate to, so a yield here
// would be a no-op that lets main/server run only if THEY are already the one
// currently scheduled -- exactly the "coincident, not preempted" problem this
// fix targets. The H-1 hotfix moved this from a POST-finalize check (which
// could compound into ~25ms of dead sleep after every byte was already
// committed, because "elapsed since build start" only ever grows once it
// crosses the threshold) to a budget that resets on every sleep, firing at
// most once per >= this many ms of emit work. Pure pacing either way: nothing
// but this worker thread ever reads a chunk mid-build, so pausing and
// resuming changes only the wall-clock schedule, never a single emitted byte
// (tests/test_chunk_mesher_slice_env.c).
#define CHUNK_MESHER_SLICE_MS 2
// MERGE NOTE (candidate 9, 2026-09-04): the 2026-09-03 restore ruling
// disabled the slice entirely (CHUNK_MESHER_SLICE_MS 1000000) because the
// 2 ms value shipped in the same hardware-unconfirmed batch as the enqueue
// cap that starved the near ring. This merge keeps H-1's MECHANISM (the
// per-y-layer emit checkpoint, which resets its budget on every sleep and
// so cannot compound the ~25ms post-commit dead sleep the restore was
// reacting to) and therefore keeps H-1's 2 ms budget. Still
// hardware-unconfirmed: re-measure on real Wii (far_walk_6m) before trusting.

// A chunk mesh is split into CHUNK_MESH_PARTS draw buckets: 6 opaque side faces
// (indexed by enum side 0..5), 6 transparent side faces (+6), and 1 double-sided
// list (12). For each bucket there is an UNTINTED (9-byte/vertex GX_VTXFMT0)
// variant at index [0..PARTS-1] and a biome-TINTED (12-byte GX_VTXFMT4, carries
// the per-vertex grass/leaf tint) variant at [PARTS..2*PARTS-1]. ~99% of faces
// have no tint and land in the cheaper 9-byte lists -- that's the whole point:
// they skip 3 tint bytes/vertex (emit store + GX FIFO) and a TEV stage at draw.
// The few tinted faces (grass tops, leaves, tall grass) use the +PARTS lists.
#define CHUNK_MESH_PARTS 13
#define CHUNK_MESH_LISTS (CHUNK_MESH_PARTS * 2)

struct chunk;
struct block_data;

// Throughput dial (wasted mesher work), NOT a fault dial -- see the note at its definition
// in chunk_mesher.c. Non-zero is normal; judge it as a ratio, not an absolute.
// CR-08 (2026-08-20): kept as plain uint32_t externs (NOT getter functions) --
// tests/test_piston_chunk_mesh_env.c reads these directly by value in several
// places, and matching that call shape (rather than forcing every reader to a
// function call) keeps this fix from spreading into test-only churn. The
// genuinely cross-thread accesses (mesh_stale_discards: incremented on the main
// thread in chunk_mesher_receive; g_mesh_truncations: incremented on the mesher
// WORKER thread in chunk_mesher_build; both read from the main thread's
// telemetry AND the Wii watchdog thread's hang-report dump) each go through
// __atomic at their own call site instead of relying on a plain read/write.
extern uint32_t mesh_stale_discards;
extern uint32_t g_mesh_truncations;

/* colcap-1006 (ASTRA-CC-R4 point 7): display-list owner accounting, read-only.
 * COMPLETED results: finalized lists the worker has posted that main has not
 * yet received (bytes = summed l->length, as displaylist_storage_bytes).
 * Added by the worker immediately before it posts a result, subtracted by
 * main when it takes the result -- the same lists, unchanged in between. */
void chunk_mesher_completed_stats(uint32_t* bytes, uint32_t* lists);
/* Monotonic milliseconds since the first call; the mesh-install timestamp
 * clock for chunk.mesh_installed_ms. */
uint32_t chunk_mesher_clock_ms(void);

// Mesh jobs submitted but not yet consumed back (request queue + in-build +
// results queue). Main-thread-only counter; for the debug overlay.
int chunk_mesher_pending(void);

// H-1: cumulative chunk_mesher_build completions (successful or not) since
// process start -- the mesher worker's throughput counter. Divide a delta of
// this by the wall-clock window it spans for "chunks meshed/s". For the debug
// overlay and the H-3 heartbeat's chunks_meshed field.
uint32_t chunk_mesher_meshed_count(void);

// Last + running-max chunk_mesher_build wall-clock in ms (worker thread).
// For the debug-overlay CPU profiler. Either pointer may be NULL.
void chunk_mesher_build_ms(float* last, float* max);

// M1 LOD instrument: cumulative emitted vertices + chunk count per render tier
// (index 0 = full mesh, 1 = far-ring surface skin). Either pointer may be NULL.
// verts[t]/chunks[t] = average vertices per chunk at tier t -> the skin's drop.
void chunk_mesher_lod_stats(uint64_t verts[2], uint32_t chunks[2]);

// T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md):
// cumulative 2x2 geomip patches considered vs merged, across every lod_skin build
// since startup. Both 0 whenever "lod.geomip" is off (the precompute pass never
// runs -- see lod_geomip_compute_plan). Either pointer may be NULL.
void chunk_mesher_geomip_stats(uint32_t* considered, uint32_t* merged);

// Sub-phase split (ms) of the worst (max-total) build: vertex_light
// precompute, vertex emit, reachability BFS, displaylist finalize. Shows
// what dominates the cold-build stall. Any pointer may be NULL.
void chunk_mesher_build_breakdown(float* vl, float* em, float* bfs,
								  float* fin);

bool chunk_mesher_init(void);
void chunk_mesher_receive(void);
/* R19 MAIN service: complete results only; never cut installation in half. */
bool chunk_mesher_receive_budget(uint32_t budget_us);
/* One fixed-size result; caller has completed the GPU/render-token handoff.
 * False retains further results in the existing queue for the next seam. */
bool chunk_mesher_receive_step(uint32_t budget_us);
bool chunk_mesher_send(struct chunk* c);
/* Caller supplies (CHUNK_SIZE + 2)^3 block_data cells. */
void chunk_mesher_gather(struct chunk* c, struct block_data* blocks);
void chunk_mesher_gather_generic(struct chunk* c, struct block_data* blocks);
/* Main only, after the GPU safe point: join, retire every outstanding job and
 * close the channels. Idempotent; receive afterwards is harmless; init may
 * start a fresh worker for a later session. */
void chunk_mesher_shutdown(void);

#ifdef CAVEWII_DISPLAYLIST_TEST
/* Deterministic queue/teardown tests: stop always releases this worker gate. */
void chunk_mesher_test_hold_worker(bool hold);
bool chunk_mesher_test_worker_held(void);
// One-shot seam for the real asynchronous mesher test: the occurrence'th
// mesher scratch allocation (light_data / visited / stk, in per-build order)
// from now reports TOTAL OOM -- arena exhausted AND the malloc fallback dead.
// occurrence==0 disables. Same arming rules as displaylist_test_fail_*: set
// only while the worker is idle.
void chunk_mesher_test_fail_scratch_after(unsigned occurrence);
// P2-b (T1-e) test-only seam: override the CHUNK_MESHER_SLICE_MS threshold
// chunk_mesher_build checks after finalizing each list. -1 (the default set
// at process start) restores production behaviour (use the real compile
// constant); a real chunk builds in well under the 2 ms default, so a test
// forces this to 0 to make the slice actually fire on every list, or a very
// large value to guarantee it never does, then compares the two builds'
// finalized mesh bytes byte-for-byte (test_chunk_mesher_slice_env.c).
void chunk_mesher_test_set_slice_ms(int ms);
// H-1 test-only seam: override CHUNK_MESHER_ENQUEUE_CAP's effective value in
// chunk_mesher_send. -1 (the default) restores production behaviour (the real
// compile constant, CHUNK_MESHER_QLENGTH -- a no-op cap since the H-1
// hotfix); a smaller value lets test_chunk_mesher_enqueue_cap_env.c force a
// real deferral and prove it stays pure pacing, independent of what value
// production ships.
void chunk_mesher_test_set_enqueue_cap(int cap);
#endif

/* Passive census: these buffers are BSS, not carve allocations. */
void chunk_mesher_census_static(size_t* scratch, size_t* transport);

#endif
