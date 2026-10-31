/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#ifndef DISPLAYLIST_LADDER_H
#define DISPLAYLIST_LADDER_H

#include <stddef.h>

/*
	LADDER-RUNG DISPLAY-LIST SIZING (2026-09-08, carve-arena interleave lane).

	WHY. The Wii carve is ONE libogc __lwp_heap: first-fit, coalescing, 32 B
	pages, +256 B red-zone guard per block (mem2.c). Real-Wii split_anchor_6m on
	6a2c5673 (seed 42, sep 256, fv6, voxel) reached readiness with 1.42 MB free
	in the carve and a LARGEST FREE BLOCK of 936 B -- and turning
	memory.displaylist_compact on, which released every byte of list slack
	(dl_slack 4.2 MB -> 0), moved that largest free block to 928 B. The arena is
	shredded by INTERLEAVE, not held by slack: freeing a block only helps if the
	next request is exactly its size.

	Display lists already grow along a fixed chain -- initial
	DISPLAYLIST_CACHE_LINES(64, 9) = 608 B, then *3/2 + vtx_bytes rounded up to
	the 32 B cache line (displaylist.c). What broke exact reuse was every OTHER
	capacity-setting path landing BETWEEN those values:

	  * displaylist_init_presized rounds the PRIOR build's final index to 32 B,
	    so post-first-build capacities are arbitrary 32 B multiples;
	  * tinted lists start at DISPLAYLIST_CACHE_LINES(64, 12) = 800 B, a second
	    chain;
	  * the one-shot render_block/item/entity/piston lists pick their own sizes;
	  * compaction shrinks a finalized list to ceil32(index) -- again arbitrary.

	A freed 4,864 B block therefore never exactly refits the next 4,832 B
	request: first-fit splits it and leaves a 32 B crumb that nothing can use.

	WHAT. Two monotone tables and an invariant: `l->length` is ALWAYS a rung.
	The BACKBONE is not invented -- it is exactly the sequence the existing
	untinted grow recurrence already produces from 608 B, carried past the
	render cap (DISPLAYLIST_RENDER_MAX_BYTES, 2 MB) -- and a GROW snaps to it,
	so the grow chain pays ZERO extra waste. INIT and PRESIZE snap to the FULL
	table (the backbone densified with each pair's geometric midpoint, plus the
	render cap itself), which halves their snap-up waste. A freed list block is
	therefore an exact refit for the next list of that rung, and a list that is
	still growing only ever holds one of the 21 backbone sizes.

	The delta against the previous behaviour is only:
	  * tinted initial 800 -> 928 (+128 B, on lists that never grow);
	  * presize ceil32(prior_index) -> rung(prior_index) -- which, because the
	    prior build's own capacity was the smallest rung >= its final index, is
	    that SAME rung: the rebuild asks for the size the block it is reusing
	    already has;
	  * the four one-shot static lists round up once (+5,984 B for the session);
	  * the ONE grow that leaves a midpoint rung: <= +25.7% of the raw request
	    (worst case 736 -> raw 1,120 -> 1,408), after which the list is on the
	    backbone and every later grow is byte-exact again.

	WASTE BOUND, measured over every request the backend can actually make (a
	32 B-aligned capacity at or above the smallest rung) and pinned by
	tests/test_displaylist_ladder.c:
	  * init/presize snap-up <= +22.5% (worst case 1,434,688 -> 1,757,120)
	    against +50% for a backbone-only table. The table STEP tops out at 1.26
	    (736 -> 928); the realised snap-up is lower because only 32 B multiples
	    are ever asked for.
	  * grow: exactly 0 off a backbone rung (every step of the chain, both
	    vertex strides), <= +25.7% for the single grow that leaves a midpoint
	    (736 -> raw 1,120 -> 1,408).

	T1: capacities and addresses only. Not one emitted display-list byte moves --
	emitted bytes are governed solely by `index` (proof:
	tests/test_displaylist_vertex_equiv.c; rung coverage:
	tests/test_displaylist_ladder.c).

	This header is backend-independent on purpose: the Wii backend counts
	`length` in GX bytes and the PC backend in host bytes with its own doubling
	chain, so each snaps to ITS OWN ladder -- but both share the rung lookup and
	the invariant, and both are exercised by the PC suite.
*/

/* The Wii GX-byte ladder, in two layers.
 *
 * BACKBONE: DISPLAYLIST_CACHE_LINES(64, 9) = 608, then (n*3/2 + 9 + 31) & ~31
 * -- exactly the sequence the existing grow recurrence already produces --
 * carried until past DISPLAYLIST_RENDER_MAX_BYTES (2 MB). This is the table a
 * GROW snaps to (displaylist_rung_grow), so a grow off a backbone rung lands on
 * the next backbone rung EXACTLY and costs not one extra byte (pinned for every
 * step and both vertex strides by test_displaylist_ladder).
 *
 * FULL LADDER: the backbone, DENSIFIED with the geometric midpoint of each
 * adjacent backbone pair (32 B floored), plus DISPLAYLIST_RENDER_MAX_BYTES
 * itself. 42 rungs, max step 1.26 instead of 1.5. This is the table INIT and
 * PRESIZE snap to, and it is neither cosmetic nor a guess: a ladder's snap-up
 * waste is bounded by its step ratio, and the project's own libogc-heap churn
 * fixture -- tests/support/libogc_heap/churn.h, the REAL libogc heap driven by
 * the REAL emitter over 36 MB of carve -- could no longer admit its 81,920 B
 * column tenant when presize snapped to a bare 1.5x backbone. Halving the step
 * gives those bytes back while keeping the property the table exists for: one
 * fixed set of sizes, so a freed block is an exact refit for the next list of
 * its rung.
 *
 * WHY GROW USES THE BACKBONE ONLY. A grow off a MIDPOINT rung does not land on
 * the next midpoint: the +vtx_bytes and the 32 B round-up push the request a
 * hair past it (13,504 -> raw 20,288, and the midpoint is 20,256), so a
 * full-ladder snap would step to 24,832 anyway -- the waste is identical while
 * the resulting capacity chain hops between the two layers. Growing to the
 * backbone instead makes a growing list's capacity one of only 21 values rather
 * than 42, which is what makes a freed grow block an exact refit for the next
 * grow of that rung. Bound: 0 extra bytes off a backbone rung; <= +25.7% of the
 * raw request for the ONE grow that leaves a midpoint (worst case 736 -> raw
 * 1,120 -> 1,408), after which the list is on the backbone and every later grow
 * is exact.
 *
 * The lookup is a linear scan over a table that fits in a handful of cache
 * lines, off the per-vertex path (it runs once per init/presize/grow, not per
 * vertex). */
#define DISPLAYLIST_LADDER_BACKBONE_RUNGS 21u
static const size_t displaylist_ladder_backbone[DISPLAYLIST_LADDER_BACKBONE_RUNGS] = {
	608,	 928,	  1408,	   2144,	3232,	 4864,	  7328,
	11008,	 16544,	  24832,   37280,	55936,	 83936,	  125920,
	188896,	 283360,  425056,  637600,	956416,	 1434656, 2152000,
};

/* The full (densified) table. 2,097,152 is DISPLAYLIST_RENDER_MAX_BYTES itself:
 * without it, a list whose index sits between the 1,757,120 midpoint and the
 * cap would be snapped to the 2,152,000 backbone rung and then REFUSED by
 * displaylist_render's `length > DISPLAYLIST_RENDER_MAX_BYTES` corruption guard
 * -- a legal list turned illegal by its own capacity. With it, every request up
 * to the cap resolves to a rung at or below the cap, and only a request already
 * past the cap reaches 2,152,000 (where the guard rejects it, as it did before
 * the ladder: the pre-ladder grow chain produced that same 2,152,000). */
#define DISPLAYLIST_LADDER_RUNGS 42u
static const size_t displaylist_ladder[DISPLAYLIST_LADDER_RUNGS] = {
	608,	 736,	  928,	   1152,	1408,	 1728,	  2144,
	2624,	 3232,	  3968,	   4864,	5984,	 7328,	  8992,
	11008,	 13504,	  16544,   20256,	24832,	 30432,	  37280,
	45664,	 55936,	  68512,   83936,	102816,	 125920,  154240,
	188896,	 231360,  283360,  347040,	425056,	 520576,  637600,
	780896,	 956416,  1171392, 1434656, 1757120, 2097152, 2152000,
};

/* The PC host-byte ladder: the backend's own initial 4096 B doubling chain,
 * carried to 32 MB. PC has no MEM2 carve and no first-fit reuse pressure, but
 * displaylist_init_presized sized a rebuild to the exact prior byte count there
 * too, which is the same off-ladder capacity bug in the same code shape -- and
 * it is what the PC split harness measures, so the fix has to be visible on the
 * instrument that measures it. */
#define DISPLAYLIST_LADDER_PC_RUNGS 14u
static const size_t displaylist_ladder_pc[DISPLAYLIST_LADDER_PC_RUNGS] = {
	4096,	  8192,		16384,	  32768,	65536,	   131072, 262144,
	524288,	  1048576,	2097152,  4194304,	8388608,   16777216,
	33554432,
};

/* Smallest rung >= `want`, from `table` (ascending, `rungs` entries).
 * ABOVE THE LAST RUNG the request is returned rounded up to the 32 B cache
 * line instead: the Wii table now runs past DISPLAYLIST_RENDER_MAX_BYTES, so
 * such a list is already refused by displaylist_render's corruption guard, and
 * capping it to the top rung would silently truncate a (bogus) buffer rather
 * than let the existing guard name it. Unreachable in practice; correct if it
 * ever is not. */
static inline size_t displaylist_rung_of(const size_t* table, size_t rungs,
										 size_t want) {
	for(size_t i = 0; i < rungs; i++)
		if(table[i] >= want)
			return table[i];
	return (want + 31u) / 32u * 32u;
}

static inline size_t displaylist_rung(size_t want) {
	return displaylist_rung_of(displaylist_ladder, DISPLAYLIST_LADDER_RUNGS,
							   want);
}

/* GROW target: the smallest BACKBONE rung >= `want`. See the backbone comment
 * above -- init/presize may leave a capacity on a midpoint, but a list that
 * GROWS climbs the backbone from there and never returns to the midpoints. */
static inline size_t displaylist_rung_grow(size_t want) {
	return displaylist_rung_of(displaylist_ladder_backbone,
							   DISPLAYLIST_LADDER_BACKBONE_RUNGS, want);
}

static inline size_t displaylist_rung_pc(size_t want) {
	return displaylist_rung_of(displaylist_ladder_pc,
							   DISPLAYLIST_LADDER_PC_RUNGS, want);
}

#endif
