/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* colcap-1006 (ASTRA-CC-R5 item 2): the pool_selftest_3m hw_run profile --
 * the column-pool T1 proof run ON THE WII (PC: under the CW_TEST_COLUMN_POOL
 * emulation, for ctest).
 *
 * (a) pool_selftest_boot(): before any world thread exists, drives
 *     alloc/free churn through the REAL column pool (mem2_alloc_pooled /
 *     mem2_free_sized, i.e. the production take, demand carve, hand-back and
 *     canary paths): grow to 7 slabs, random churn, drain with hand-back,
 *     a FORCED heap-return refusal (mem2_pool_selftest_refuse_handbacks) whose
 *     kept slab must stay usable and later go back, then everything freed.
 *     Every step checks: full-object byte patterns (no overlap, no move, no
 *     overrun), slot ownership and address uniqueness, used == live, the
 *     lowest-first rule for the production placement, canary hits, the heap
 *     validator, and the per-op Broadway cost. It then leaves a fixed
 *     fragmented BALLAST (48 of the 128 boot-slab slots, scattered) so the
 *     game's columns land in placement-dependent holes, and re-arms the
 *     placement digest.
 * (b) pool_selftest_frame_begin/_end(): once the scene has settled, hashes
 *     three consecutive frames of chunk display-list submissions in order
 *     (displaylist_scene_hash.h); three identical frames are reported as one
 *     gx_scene_hash event. The ballast is then verified byte-for-byte (two
 *     minutes of real column traffic in neighbouring slots) and released
 *     through the canary-checking free.
 * Events (JSON, one line each): pool_selftest (phase churn | final) and
 * gx_scene_hash; the collector grades them (tools/wii_telemetry_server.py,
 * PoolSelftestEvidence) and compares two runs' hashes (--gx-compare). */

#ifndef GAME_POOL_SELFTEST_H
#define GAME_POOL_SELFTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where event lines go (default: hw_telemetry_emit_observer_retry). */
void pool_selftest_set_emit(bool (*emit)(const char* line));

/* Phase (a). False when a check failed; `reason` names the first one. The
 * churn event is emitted either way. */
bool pool_selftest_boot(bool rotating, char* reason, size_t reason_size);

/* Phase (b), main thread. begin() before the first world_render of the
 * frame, end() after the last. cam = x, y, z, yaw, pitch.
 * settled: in game, near ring streamed (camera stillness is checked here);
 * clean: no near mesh pending and the mesher idle -- only clean frames are
 * hashed. */
void pool_selftest_frame_begin(bool settled, bool clean, const float cam[5],
							   uint32_t elapsed_ms);
void pool_selftest_frame_end(uint32_t elapsed_ms);

/* The run's verdict once the driver has finished: NULL while running, else
 * "ok" or the failure class (the profile_done status). */
const char* pool_selftest_status(void);

/* ctest: reset every static (one process runs several scenarios). */
void pool_selftest_reset(void);
/* ctest: the k-th ballast object still held (NULL past the end). */
void* pool_selftest_ballast_ptr(unsigned k);

#endif
