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

#ifndef DISPLAYLIST_H
#define DISPLAYLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct displaylist_backing;

struct displaylist {
	void* data;
	size_t length; // Wii: emit capacity; once finished, allocation incl. GX padding
	size_t index;
	/* Keep the seven ownership/emit flags in one byte: devkitPPC sizeof
	 * stays 24 B despite the last-use stamp and backing lease. No stream bits. */
	bool finished : 1;
	// Set true if an OOM (a mem2_alloc/mem2_realloc NULL during vertex emit) made an
	// emit call early-return WITHOUT advancing index. The mesher still accumulated the
	// FULL intended vertex count, so finalizing this list would stamp a GX_QUADS header
	// claiming more quads than were actually written -- the GP would then parse the
	// GX_NOP tail / stale bytes as POS/CLR/TEX (garbled geometry) and a desynced
	// primitive stream can WEDGE the GP command processor (GX_WaitDrawDone never
	// returns) = the real-Wii frozen-no-dump hang. chunk_mesher_build DROPS a truncated
	// replacement instead of finalizing it; bounded per-chunk retries resume after
	// allocator pressure eases.
	bool truncated : 1;
	// When set, every vertex carries a second, direct-RGB colour channel (the
	// per-column Beta biome tint) in addition to the usual indexed light. Only
	// chunk meshes opt in; item/entity/GUI lists leave it false and are byte-for-
	// byte unchanged. `tint` is the current tint emitted by displaylist_color
	// until the next displaylist_set_tint; it defaults to white (no tint).
	bool tinted : 1;
	bool accounted : 1; // finalized allocation counted, independent of PC VBO upload
	/* M2 mesh blocks (Wii): this list's bytes live inside a chunk-mesh
	 * generation BLOCK owned by the chunk (chunk.mesh_block); destroy frees
	 * nothing, the block is released when the generation retires. */
	bool block_owned : 1;
	/* M2: only lists the mesher WORKER flags (displaylist_allow_scratch) may
	 * allocate from the per-build scratch; main-thread lists (HUD, held item,
	 * entities, piston) keep the carve path even while a window is open. */
	bool scratch_ok : 1;
	/* Ownership moves with the mesher's by-value publication. Only MAIN
	 * submits; detached storage keeps this stamp until GPU retirement. */
	bool submitted : 1;
	uint8_t tint[3];
	uint32_t last_submit;
	struct displaylist_backing* backing;
#ifdef PLATFORM_PC
	int vbo;
#endif
};

void displaylist_init(struct displaylist* l, size_t vertices,
					  size_t vertex_size);
// T1-c (2026-09-03 far-ring frame-drop root cause): identical to
// displaylist_init, except `length` is additionally raised to cover
// `prior_index` -- the FINAL l->index a previous build of the same chunk/side
// reached (0 = no history) -- so the first lazy allocation in
// displaylist_pos/displaylist_vertex is already big enough for a same-shape
// rebuild and never needs to grow. `tinted` mirrors the flag the caller will
// set via displaylist_set_tinted right after this call (displaylist_init
// always resets it to false, so it isn't yet on `l` when this runs); each
// backend interprets `prior_index` in ITS OWN post-finalize units (Wii: a
// byte offset, already directly comparable to `length`; PC: a vertex count,
// which needs `tinted` to convert to bytes) -- see the two definitions.
// prior_index == 0 reproduces plain displaylist_init exactly (today's initial
// size stays reachable). Changes ONLY buffer capacity, never emitted bytes
// (governed solely by `index`); see tests/test_displaylist_vertex_equiv.c.
void displaylist_init_presized(struct displaylist* l, size_t vertices,
							   size_t vertex_size, bool tinted,
							   size_t prior_index);
void displaylist_destroy(struct displaylist* l);
void displaylist_reset(struct displaylist* l);
void displaylist_finalize(struct displaylist* l, uint16_t vtxcnt);
void displaylist_render(struct displaylist* l);
void displaylist_render_immediate(struct displaylist* l, uint16_t vtxcnt);

void displaylist_pos(struct displaylist* l, int16_t x, int16_t y, int16_t z);
void displaylist_color(struct displaylist* l, uint8_t index);
void displaylist_texcoord(struct displaylist* l, uint8_t s, uint8_t t);

// Emit one full vertex in a single call. Byte-for-byte identical to the
// sequence  displaylist_pos(l,x,y,z); displaylist_color(l,light);
// displaylist_texcoord(l,s,t);  but with the capacity/grow check, the
// data/index/tinted loads and the index advance all hoisted out of the three
// separate calls -- the mesher emit hot path. `light` is the indexed-light
// value; the biome tint is read from l->tint exactly as displaylist_color does.
void displaylist_vertex(struct displaylist* l, int16_t x, int16_t y, int16_t z,
						uint8_t light, uint8_t s, uint8_t t);

// Mark this list as carrying the per-vertex biome tint channel (see `tinted`).
// Call right after displaylist_init, before any vertices are emitted.
void displaylist_set_tinted(struct displaylist* l, bool tinted);
// Set the tint colour emitted with subsequent vertices (0..255 per channel).
// No-op on the byte stream of a non-tinted list. White = no tint.
void displaylist_set_tint(struct displaylist* l, uint8_t r, uint8_t g,
						  uint8_t b);

// T1-c grow-churn gauge: cumulative mem2_realloc/realloc grow cycles across
// every list since boot. Wii-meaningful (the mem2_realloc allocate+copy+free
// churn T1-c targets); the PC backend increments it too for build/API
// symmetry, though PC's plain-malloc grow is comparatively free.
uint32_t displaylist_grow_count(void);
// Experimental finalization compaction. Default OFF; configure before workers
// start. Changes allocation capacity only, never emitted bytes or draw ranges.
void displaylist_set_compact(bool enabled);
bool displaylist_compact_enabled(void);
// All resident finalized allocations, including unpublished/GPU-held lists.
// Slack includes the separately reported trailing allocation pad (Wii: 32/list).
// Used bytes include GX cache-line rounding/leading NOPs; PC uses vertex bytes.
// Independent relaxed snapshots: transitions may straddle a heartbeat.
void displaylist_storage_stats(uint32_t* lists, uint32_t* slack, uint32_t* pad);
// colcap-1006 (ASTRA-CC-R3 item 3): resident finalized lists and their summed
// capacity bytes (l->length). Telemetry only; same relaxed-snapshot caveat.
void displaylist_storage_bytes(uint32_t* lists, uint32_t* bytes);

/* M2 MESH BLOCKS (Wii; docs/p2_storage_ruling_fable_2026-09-09.md "Memory plan
 * v2"). Measured on real Wii: 21 % of the display-list allocation at split
 * readiness is ladder slack and ~15,000 relocating grow-reallocs shredded the
 * carve to a 928 B largest block. In block mode the mesher worker emits a
 * chunk's lists into a private per-build SCRATCH arena (bump allocation, no
 * carve traffic), then packs the finished streams into ONE exact-size carve
 * block per chunk-mesh generation. GX bytes are identical; only where they
 * live changes (T1). Opt-in via "memory.mesh_blocks" until hardware-proven. */
void displaylist_set_block_mode(bool enabled);
bool displaylist_block_mode(void);
/* Worker thread only: begin/end one build's scratch window. Lists emitted
 * between them allocate from the scratch arena (falling back to the carve on
 * overflow -- counted, never fatal). */
void displaylist_scratch_begin(void);
void displaylist_scratch_end(void);
/* Worker thread: mark a freshly initialised list as eligible for the scratch. */
void displaylist_allow_scratch(struct displaylist* l);
/* Pack every finished scratch-resident list of `lists[0..n)` (has[k]) into one
 * exact-size carve block; on success each list points into the block and is
 * block_owned. `*out_block`/`*out_bytes` receive the block and STREAM bytes
 * (NULL/0 when no
 * list needed packing). Returns false when the carve cannot supply the block:
 * the lists are left untouched (still scratch-resident; the caller must
 * destroy them before the next scratch_begin). A private ownership record
 * follows the stream bytes in the same allocation; block_free recovers it
 * directly from this pair and releases the full allocation after its fence. */
bool displaylist_pack_block(struct displaylist* lists, const bool* has,
							size_t n, void** out_block, size_t* out_bytes);
void displaylist_block_free(void* block, size_t bytes);
#ifdef PLATFORM_WII
#define displaylist_block_allocation_bytes wii_displaylist_block_allocation_bytes
#define displaylist_block_bad_free_count wii_displaylist_block_bad_free_count
#endif
size_t displaylist_block_allocation_bytes(size_t stream_bytes);
uint32_t displaylist_block_bad_free_count(void);

/* MAIN configures once before workers start. NULL hooks give the PC's
 * immediate-completion GPU; tests install a delayed fake. The seal callback
 * appends a draw-sync command, wait flushes/polls it (or completes an abort),
 * and is_main prevents a worker from calling either callback. seal emits the
 * low 16 bits to hardware; wait and completion queries retain the full stamp. */
struct displaylist_gpu_hooks {
	uint16_t (*read_completed)(void);
	void (*seal)(uint16_t token);
	void (*wait)(uint32_t token);
	bool (*is_main)(void);
};
/* Backend aliases also allow the existing host equivalence fixture to link
 * both real emitters in one executable. */
#ifdef PLATFORM_WII
#define displaylist_gpu_configure wii_displaylist_gpu_configure
#define displaylist_gpu_seal wii_displaylist_gpu_seal
#define displaylist_gpu_reap wii_displaylist_gpu_reap
#define displaylist_gpu_abort_begin wii_displaylist_gpu_abort_begin
#define displaylist_gpu_abort_complete wii_displaylist_gpu_abort_complete
#define displaylist_gpu_pending wii_displaylist_gpu_pending
#define displaylist_gpu_token_complete wii_displaylist_gpu_token_complete
#endif
void displaylist_gpu_configure(const struct displaylist_gpu_hooks* hooks,
							  uint16_t initial_token);
uint32_t displaylist_gpu_seal(void);
void displaylist_gpu_reap(void);
/* No retirement is allowed between begin and the synchronous abort witness.
 * complete is called ONLY after GX_AbortFrame has reset the entire pipeline. */
void displaylist_gpu_abort_begin(void);
void displaylist_gpu_abort_complete(void);
bool displaylist_gpu_token_complete(uint32_t token);
uint32_t displaylist_gpu_pending(void);
void displaylist_block_counters(uint32_t* packed, uint32_t* overflow,
								uint32_t* pack_fail);
/* Astra consult 2026-09-10 (M2 hardening): the EFFECTIVE mode and the byte
 * accounting the A/B needs. mode_requested is the config key; mode_effective
 * drops to false when the scratch arena could not be allocated
 * (scratch_fail counts those). copy_bytes: scratch -> block memcpy at pack;
 * spill_bytes: bytes of lists that overflowed the scratch into the carve;
 * grow_copies: scratch grows that could not extend in place (copied). All
 * 32-bit so the Wii reads them without a libatomic helper. */
struct displaylist_block_stats {
	bool mode_requested, mode_effective;
	uint32_t scratch_fail, grow_copies, copy_bytes, spill_bytes;
};
void displaylist_block_stats(struct displaylist_block_stats* out);

// Per-frame draw-call + triangle counters (2026-09-03 far-ring frame-drop
// root-cause doc): the GP-load gauge that was missing from the heartbeat (the
// existing `dlcall` telemetry field is a corruption counter, not a call
// count). _reset() is called once per frame before the world render pass
// (main.c); _get() reads the totals for that frame's heartbeat. Every actual
// GX_CallDispList / glDrawArrays(GL_QUADS) chunk-mesh draw counts as one
// dlcall; tris is derived from each list's own emitted vertex count (2
// triangles/quad).
void displaylist_frame_counters_reset(void);
void displaylist_frame_counters_get(uint32_t* dlcalls, uint32_t* tris);

#ifdef CAVEWII_DISPLAYLIST_TEST
// One-shot PC allocation-failure seams for the real asynchronous mesher test.
// occurrence==0 disables a seam; 1 fails the next matching allocation/grow.
// Arm only while the worker is idle; the deliberately tiny countdown is not
// atomic because the test sets it before dispatch and never mutates it in flight.
void displaylist_test_fail_malloc_after(unsigned occurrence);
void displaylist_test_fail_realloc_after(unsigned occurrence);
#endif

#endif
