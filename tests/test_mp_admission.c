/* MP admission radius + demote-on-exit / promote-on-entry with a LOSSLESS
 * re-encode (source/network/server_remote_chunks.c). W1 of mp-split-0912;
 * design authority docs/mp_admission_budget_design_2026-09-13.md sections
 * 2.1, 2.2, 2.3, 2.5, ratified by Astra's gate-2 ruling (items 3, 5, 6, 7).
 *
 * What the real Wii run betamc_0913 proved: BetaMC streams 441 columns and
 * this client installs every one of them (81,920 B of MEM2 voxels each), so
 * the carve ran dry at tick 16 and the read thread died. The fix is a radius
 * gate on installs, plus a demotion that gives a column's voxels back WITHOUT
 * losing its state -- and "without losing its state" is what this file
 * proves, byte-for-byte.
 *
 * The harness is the existing S0 suite with its main renamed, exactly like
 * test_mp_chunk_order.c. Like that file, the cases drive the PRODUCTION
 * functions directly (remote_retain_map_locked / remote_promotion_step_locked
 * / remote_residency_step_locked, in the order server_remote_pump calls them)
 * instead of standing up a socket peer: the code under test is identical, the
 * 441-column burst is deterministic and fast, and nothing here depends on
 * thread timing. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

/* W2: the MP backing reservation the cases size, fill and read back
 * (platform/mem2.h). */
#include "../source/platform/mem2.h"
/* F5a case (r): the CARVE_MP_BACKING acquire/release notes are the only
 * instrument that reads the same on PC and on the Wii (mem2_carve_free_bytes
 * is SIZE_MAX on PC). */
#include "../source/carve_observer.h"
/* F5b case (w): the H4 proof needs a TRYLOCK on impl->lock, which
 * platform/thread.h does not expose (no production caller wants one). This
 * suite is PC-only, where struct thread_mutex IS a pthread_mutex_t, so the
 * probe reads the real lock rather than a stand-in. */
#include <pthread.h>

/* One full column in Packet51 wire layout -- the same four planes
 * remote_finish_promotion_locked consumes and the demotion re-encodes. */
#define ADM_CELLS (16u * 128u * 16u)
#define ADM_NIB (ADM_CELLS / 2u)
#define ADM_RAW (ADM_CELLS + 3u * ADM_NIB)

static struct server_remote adm_owner;
static struct server_remote_impl* s_r;
static uint8_t adm_raw[ADM_RAW];
static uint8_t adm_ref[ADM_RAW];
static uint8_t adm_out[ADM_RAW];

/* Deterministic pseudo-random column content, keyed by the column's own
 * coordinate so every reference comparison can be regenerated instead of
 * stored. Values come in short runs: the content still varies cell by cell
 * across the column (so a wrong plane offset or a swapped sky/torch pair
 * shows up immediately in the byte compare), but it deflates to a few KB
 * instead of ~45 KB, which keeps a 441-column burst inside the retained-byte
 * cap and the test inside a second. */
static uint32_t adm_rng(uint32_t* state) {
	*state = *state * 1664525u + 1013904223u;
	return *state >> 8;
}

static void adm_fill_raw(int cx, int cz, uint8_t* raw) {
	/* Only ids with a blocks[] entry: remote_finish_promotion_locked kills
	 * the connection on an unknown id, which is its own (already tested)
	 * behaviour and not what this file is about. */
	static const uint8_t palette[]
		= {BLOCK_AIR,	BLOCK_STONE, BLOCK_DIRT,
		   BLOCK_SAND,	BLOCK_GRAVEL, BLOCK_COBBLESTONE};
	uint32_t state = (uint32_t)(cx * 73856093) ^ (uint32_t)(cz * 19349663)
		^ 0x9E3779B9u;
	uint8_t* ids = raw;
	uint8_t* metadata = raw + ADM_CELLS;
	uint8_t* torch = metadata + ADM_NIB;
	uint8_t* sky = torch + ADM_NIB;
	size_t i = 0;
	while(i < ADM_CELLS) {
		uint32_t r = adm_rng(&state);
		uint8_t id = palette[r % (sizeof(palette) / sizeof(palette[0]))];
		size_t run = 1 + (r >> 8) % 13u;
		for(size_t k = 0; k < run && i < ADM_CELLS; k++, i++)
			ids[i] = id;
	}
	for(unsigned plane = 0; plane < 3; plane++) {
		uint8_t* dst = plane == 0 ? metadata : (plane == 1 ? torch : sky);
		size_t j = 0;
		while(j < ADM_NIB) {
			uint32_t r = adm_rng(&state);
			uint8_t byte = (uint8_t)(r & 0xFFu);
			size_t run = 1 + (r >> 8) % 7u;
			for(size_t k = 0; k < run && j < ADM_NIB; k++, j++)
				dst[j] = byte;
		}
	}
}

/* W2: a body handed to the retention MUST come out of the MP backing
 * reservation, exactly as the decoder's own allocation site does
 * (mp_codec.c's Packet51 header case) -- remote_body_free asserts that
 * ownership rather than guessing, so a malloc'd body here would be a heap
 * mismatch the moment the retention dropped it. Compress into a scratch
 * buffer of compressBound size, then take the EXACT length from the
 * reservation and copy, which is what production does. Returns NULL when the
 * reservation refuses, so case (i) can drive a real capacity failure. */
static void* adm_body_from(const void* raw, size_t raw_length, uLongf* out_n) {
	uLongf n = compressBound((uLong)raw_length);
	unsigned char* scratch = malloc(n);
	assert(scratch);
	assert(compress(scratch, &n, raw, (uLong)raw_length) == Z_OK);
	void* body = mp_backing_alloc(n);
	if(body)
		memcpy(body, scratch, n);
	free(scratch);
	*out_n = n;
	return body;
}

/* Retain one deterministic FULL column body through the production path. */
static void adm_retain_full(int cx, int cz) {
	adm_fill_raw(cx, cz, adm_raw);
	uLongf n = 0;
	unsigned char* compressed = adm_body_from(adm_raw, ADM_RAW, &n);
	assert(compressed);
	struct mp_event e = {.type = MP_EVENT_MAP_CHUNK};
	e.payload.map_chunk = (struct mp_map_chunk) {
		.x = cx * 16, .y = 0, .z = cz * 16, .sx = 16, .sy = 128, .sz = 16,
		.compressed = compressed, .compressed_length = n,
		.inflated_length = ADM_RAW};
	assert(remote_retain_map_locked(s_r, &e));
}

/* Retain a SUB-VOLUME body (y 0..sy-1) of uniform `id` for one column. */
static void adm_retain_partial(int cx, int cz, int sy, int id) {
	size_t cells = 16u * (size_t)sy * 16u, bytes = cells * 5u / 2u;
	unsigned char* rawp = calloc(1, bytes);
	assert(rawp);
	memset(rawp, id, cells);
	uLongf n = 0;
	unsigned char* compressed = adm_body_from(rawp, bytes, &n);
	assert(compressed);
	free(rawp);
	struct mp_event e = {.type = MP_EVENT_MAP_CHUNK};
	e.payload.map_chunk = (struct mp_map_chunk) {
		.x = cx * 16, .y = 0, .z = cz * 16, .sx = 16, .sy = (uint16_t)sy,
		.sz = 16, .compressed = compressed, .compressed_length = n,
		.inflated_length = bytes};
	assert(remote_retain_map_locked(s_r, &e));
}

/* Where the server says the player is. Column (cx, cz), mid-column.
 * F5b: a server position packet also refreshes the ADMISSION CENTRE (the one
 * accessor the gate, the hysteresis and the census now share), so this
 * stand-in for Packet13 feeds it through the same production setter
 * remote_handle_correction uses. */
static void adm_anchor(int cx, int cz) {
	s_r->have_authoritative_position = true;
	s_r->authoritative_x = cx * 16.0 + 8.0;
	s_r->authoritative_y = 64.0;
	s_r->authoritative_z = cz * 16.0 + 8.0;
	remote_admission_centre_set(s_r, s_r->authoritative_x,
								s_r->authoritative_z, true);
}
/* The LOGIN SPAWN hint (Packet6) on its own -- no position packet, which is
 * the window review finding H5 is about. */
static void adm_spawn_hint(int cx, int cz) {
	remote_admission_centre_set(s_r, cx * 16.0 + 8.0, cz * 16.0 + 8.0, false);
}

/* THE RESIDENCY STEP, with impl->lock held the way server_remote_pump holds
 * it (server_remote.c). F5b (review finding H4): the demotion's re-encode now
 * RELEASES that lock across its gather+deflate and re-acquires it to
 * validate and install, so a caller that does not hold it on entry would be
 * unlocking a mutex it never took. The promotion step below needs the same
 * wrapper (REL-MP-JOINSPEED); no other *_locked function this file drives
 * directly ever drops the lock. */
static void adm_residency_step(void) {
	thread_mutex_lock(&s_r->lock);
	remote_residency_step_locked(s_r);
	thread_mutex_unlock(&s_r->lock);
}

/* THE PROMOTION STEP, under impl->lock like server_remote_pump calls it.
 * REL-MP-JOINSPEED: the pump's compaction now releases the lock across its
 * deflate (the demotion's protocol), so this step needs the same wrapper as
 * the residency step above. */
static bool adm_promotion_step(void) {
	thread_mutex_lock(&s_r->lock);
	bool ok = remote_promotion_step_locked(s_r);
	thread_mutex_unlock(&s_r->lock);
	return ok;
}

/* One pump's worth of residency work, in server_remote_pump's own order. */
static void adm_pump(void) {
	assert(adm_promotion_step());
	adm_residency_step();
	remote_emit_pending_unloads_locked(s_r);
}
static void adm_pump_n(int n) {
	for(int i = 0; i < n; i++)
		adm_pump();
}

/* Emulate the pump's edit path: each edit is the next received packet. */
static void adm_edit(int x, int y, int z, int id) {
	s_r->pump_packet_seq = ++s_r->arrival_sequence;
	s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, x, (uint8_t)y, z, (uint8_t)id, 0);
}

static size_t adm_entry(int cx, int cz) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		if(s_r->chunks[i].state != REMOTE_CHUNK_UNUSED
		   && s_r->chunks[i].chunk_x == cx && s_r->chunks[i].chunk_z == cz)
			return i;
	return REMOTE_CHUNK_ENTRY_CAPACITY;
}
static enum remote_chunk_state adm_state(int cx, int cz) {
	size_t i = adm_entry(cx, cz);
	assert(i != REMOTE_CHUNK_ENTRY_CAPACITY);
	return s_r->chunks[i].state;
}
static size_t adm_count(enum remote_chunk_state state) {
	size_t n = 0;
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		if(s_r->chunks[i].state == state)
			n++;
	return n;
}
static bool adm_installed(int cx, int cz) {
	return world_find_chunk(&gstate.world, cx * 16, 0, cz * 16) != NULL;
}
/* Export into adm_out's four planes (wire order: ids, metadata, torch, sky). */
static bool adm_export(int cx, int cz) {
	memset(adm_out, 0xCD, sizeof(adm_out));
	return server_remote_test_export_column(&adm_owner, cx, cz, adm_out,
		adm_out + ADM_CELLS, adm_out + ADM_CELLS + 2 * ADM_NIB,
		adm_out + ADM_CELLS + ADM_NIB);
}

#define ADM_BURST 10 /* 21x21 = 441 columns, Beta's default server view */
#define ADM_RADIUS 2

/* ---- (a) the admission gate ------------------------------------------- */
static void test_admission_gate(void) {
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_RADIUS);
	for(int cx = -ADM_BURST; cx <= ADM_BURST; cx++)
		for(int cz = -ADM_BURST; cz <= ADM_BURST; cz++)
			adm_retain_full(cx, cz);
	assert(s_r->retained_entries == 441);
	/* Four pumps per column is generous: a full column needs three inflate
	 * steps plus the publish. If the gate leaked, this is more than enough
	 * pumps to install all 441. */
	adm_pump_n(441 * 4);
	size_t resident = adm_count(REMOTE_CHUNK_RESIDENT);
	assert(resident == (2 * ADM_RADIUS + 1) * (2 * ADM_RADIUS + 1)
		   && "exactly the (2R+1)^2 in-radius columns install");
	assert(adm_count(REMOTE_CHUNK_TOMBSTONE) == 0
		   && "nothing is evicted: a refused column keeps its body");
	assert(s_r->retention_evictions == 0 && s_r->forced_reconnects == 0
		   && !s_r->stop_now);
	for(int cx = -ADM_BURST; cx <= ADM_BURST; cx++)
		for(int cz = -ADM_BURST; cz <= ADM_BURST; cz++) {
			int d = (cx < 0 ? -cx : cx) > (cz < 0 ? -cz : cz)
				? (cx < 0 ? -cx : cx) : (cz < 0 ? -cz : cz);
			size_t i = adm_entry(cx, cz);
			assert(i != REMOTE_CHUNK_ENTRY_CAPACITY);
			if(d <= ADM_RADIUS) {
				assert(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT);
				assert(adm_installed(cx, cz));
			} else {
				assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
					   && "out of radius stays BACKED");
				assert(s_r->chunks[i].wire.compressed
					   && s_r->chunks[i].wire.compressed_length
					   && "with its complete compressed body intact");
				assert(!adm_installed(cx, cz)
					   && "and no voxels of its own");
			}
		}
	printf("PASS (a) admission gate: 441 announced, %zu resident (R=%d), "
		   "416 BACKED with bodies intact, 0 tombstones\n",
		   resident, ADM_RADIUS);
}

/* ---- (b) demote on exit, promote on entry ---------------------------- */
static void test_move_demotes_and_promotes(void) {
	uint32_t demotions_before = s_r->demotions;
	adm_anchor(4, 0); /* four columns east */
	/* One demotion per pump is the design's bound, so settling 15 columns
	 * takes at least 15 pumps; give the promotions room too. */
	uint32_t before = s_r->demotions;
	adm_pump();
	assert(s_r->demotions - before <= 1 && "at most ONE demotion per pump");
	adm_pump_n(200 * 4);
	/* The expected steady state, derived rather than asserted blind.
	 * admit_radius = 2 around column 4: cx 2..6, cz -2..2 install (25).
	 * Demotion needs distance > admit_radius + 1 = 3 (the design's one
	 * column of hysteresis), so the cx = 1 row of the OLD ring -- now at
	 * distance exactly 3 -- keeps its voxels: 5 more, 30 resident. The 15
	 * columns at cx -2..0 are past the hysteresis and demote. */
	for(int cx = -ADM_BURST; cx <= ADM_BURST; cx++)
		for(int cz = -ADM_BURST; cz <= ADM_BURST; cz++) {
			int dx = cx - 4 < 0 ? 4 - cx : cx - 4;
			int dz = cz < 0 ? -cz : cz;
			int d = dx > dz ? dx : dz;
			size_t i = adm_entry(cx, cz);
			if(d <= ADM_RADIUS)
				assert(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT
					   && adm_installed(cx, cz)
					   && "every in-radius column promoted on entry");
			if(d > ADM_RADIUS + 1) {
				assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
					   && "past the hysteresis: demoted back to BACKED");
				assert(s_r->chunks[i].wire.compressed
					   && "with a re-encoded body of its own");
				assert(!adm_installed(cx, cz) && "and its voxels released");
			}
		}
	assert(adm_count(REMOTE_CHUNK_RESIDENT) == 30);
	assert(s_r->demotions - demotions_before == 15
		   && "the 15 columns of the old ring past radius+1, and only those");
	assert(s_r->demotion_failures == 0 && !s_r->stop_now
		   && s_r->forced_reconnects == 0);
	printf("PASS (b) move by 4: 15 demoted (one per pump), 20 promoted on "
		   "entry, 30 resident (25 in radius + the radius+1 hysteresis row)\n");
}

/* ---- (c) byte-for-byte reconstruction -------------------------------- */
static void test_reconstruction_byte_exact(void) {
	/* Column (0,0) was RESIDENT in (a) and demoted in (b). Its backing must
	 * reconstruct to exactly the bytes the server originally sent. */
	adm_fill_raw(0, 0, adm_ref);
	assert(adm_state(0, 0) == REMOTE_CHUNK_RETAINED && !adm_installed(0, 0));
	assert(adm_export(0, 0) && "a demoted column exports from its backing");
	assert(memcmp(adm_out, adm_ref, ADM_RAW) == 0
		   && "BACKED re-encode == the original body, byte for byte");
	/* Walk back and let it re-promote from that re-encoded body. */
	adm_anchor(0, 0);
	adm_pump_n(200 * 4);
	assert(adm_state(0, 0) == REMOTE_CHUNK_RESIDENT && adm_installed(0, 0));
	assert(adm_export(0, 0) && "and now from the installed voxels");
	assert(memcmp(adm_out, adm_ref, ADM_RAW) == 0
		   && "RESIDENT -> demoted -> RESIDENT is byte-identical");
	assert(s_r->demotion_failures == 0 && !s_r->stop_now);
	printf("PASS (c) reconstruction: RESIDENT -> demote -> RESIDENT round "
		   "trip is byte-identical over all %u plane bytes\n", ADM_RAW);
}

/* ---- (d) edits survive a demotion ------------------------------------ */
static void test_edits_survive(void) {
	/* (d1) a LIVE edit applied to a RESIDENT column. (0,0) is resident and
	 * in radius after (c). */
	int ex = 3, ey = 40, ez = 5;
	adm_edit(ex, ey, ez, BLOCK_PLANKS);
	assert(world_get_block(&gstate.world, ex, ey, ez).type == BLOCK_PLANKS);
	uint32_t applied_before = s_r->deferred_edits_applied;
	adm_anchor(8, 0); /* out of radius + 1 -> demote */
	adm_pump_n(200 * 4);
	assert(adm_state(0, 0) == REMOTE_CHUNK_RETAINED && !adm_installed(0, 0));
	adm_anchor(0, 0); /* back in -> re-promote from the re-encode */
	adm_pump_n(200 * 4);
	assert(adm_state(0, 0) == REMOTE_CHUNK_RESIDENT);
	assert(world_get_block(&gstate.world, ex, ey, ez).type == BLOCK_PLANKS
		   && "the live edit is inside the demotion re-encode");
	assert(s_r->deferred_edits_applied == applied_before
		   && "it was never parked: it was already in the voxels");
	/* And the rest of the column still matches the original body -- the
	 * re-encode captured the edit WITHOUT disturbing anything else. */
	adm_fill_raw(0, 0, adm_ref);
	assert(adm_export(0, 0));
	size_t edited = ((size_t)(ex & 15) * 16u + (size_t)(ez & 15)) * 128u
		+ (size_t)ey;
	adm_ref[edited] = BLOCK_PLANKS;
	assert(memcmp(adm_out, adm_ref, ADM_CELLS) == 0
		   && "exactly one id changed; every other cell is the original");

	/* (d2) an edit parked against a BACKED column, then promoted. (7,0) is
	 * out of radius from column 0, so it has never been installed. */
	adm_fill_raw(7, 0, adm_ref);
	assert(adm_state(7, 0) == REMOTE_CHUNK_RETAINED && !adm_installed(7, 0));
	int bx = 7 * 16 + 2, by = 70, bz = 9;
	adm_edit(bx, by, bz, BLOCK_PLANKS);
	assert(s_r->deferred_edits == 1 && "parked in the deferred log");
	adm_anchor(7, 0);
	adm_pump_n(200 * 4);
	assert(adm_state(7, 0) == REMOTE_CHUNK_RESIDENT);
	assert(world_get_block(&gstate.world, bx, by, bz).type == BLOCK_PLANKS
		   && "the parked edit replayed on promotion");
	assert(adm_export(7, 0));
	adm_ref[((size_t)(bx & 15) * 16u + (size_t)(bz & 15)) * 128u + (size_t)by]
		= BLOCK_PLANKS;
	assert(memcmp(adm_out, adm_ref, ADM_CELLS) == 0
		   && "BACKED + parked edit reconstructs identically to (c) + edit");
	assert(!s_r->stop_now && s_r->deferred_edits_dropped == 0);
	printf("PASS (d) edits: a live edit rides inside the demotion re-encode; "
		   "a parked edit replays on promotion; both byte-exact elsewhere\n");
}

/* ---- (e) a queued newer sub-volume survives the demotion -------------- */
static void test_queued_body_across_demotion(void) {
	/* (7,0) is RESIDENT after (d2). Demote it, then let a NEWER partial
	 * arrive: the re-encoded full head does not cover it, so it queues
	 * behind that head and must apply AFTER it on re-promotion -- the
	 * ordering contract of design section 2.5, across a demotion. */
	adm_fill_raw(7, 0, adm_ref);
	adm_ref[((size_t)((7 * 16 + 2) & 15) * 16u + 9u) * 128u + 70u]
		= BLOCK_PLANKS; /* (d2)'s edit is part of the backing now */
	adm_anchor(0, 0);
	adm_pump_n(200 * 4);
	size_t i = adm_entry(7, 0);
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
		   && !adm_installed(7, 0));
	uint64_t head_stamp = s_r->chunks[i].arrival;
	adm_retain_partial(7, 0, 8, BLOCK_BRICKS); /* y 0..7, newer than the head */
	assert(s_r->chunks[i].queued_count == 1
		   && "the partial queued behind the re-encoded head");
	assert(s_r->chunks[i].queued_arrival[0] > head_stamp
		   && "and it is stamped newer than the demotion body");
	adm_anchor(7, 0);
	adm_pump_n(200 * 4);
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT
		   && s_r->chunks[i].queued_count == 0 && "both bodies installed");
	for(int y = 0; y < 8; y++)
		assert(world_get_block(&gstate.world, 7 * 16 + 2, y, 9).type
				   == BLOCK_BRICKS
			   && "the later partial wins inside its own volume");
	assert(world_get_block(&gstate.world, 7 * 16 + 2, 70, 9).type
			   == BLOCK_PLANKS
		   && "and the re-encoded head still owns everything outside it");
	assert(adm_export(7, 0));
	assert(memcmp(adm_out + 8, adm_ref + 8, 120) == 0
		   && "the head's cells above the partial are untouched");
	assert(!s_r->stop_now && s_r->forced_reconnects == 0
		   && s_r->chain_rejections == 0);
	printf("PASS (e) chain: a newer sub-volume queued behind a demotion body "
		   "applies after it, in order, on re-promotion\n");
}

/* ---- (f) the demotion vetoes ----------------------------------------- */
/* Drain: run ONLY the residency step, enough times that every eligible
 * resident column is demoted (one per step). Returns how many demotions
 * happened. No promotion step -- (f) arms promotion.active by hand, which the
 * promotion pipeline itself must not be run against. */
static uint32_t adm_drain_residency(int steps) {
	uint32_t before = s_r->demotions;
	for(int k = 0; k < steps; k++)
		adm_residency_step();
	return s_r->demotions - before;
}

static void test_demotion_vetoes(void) {
	size_t i = adm_entry(7, 0);
	/* Park the anchor far away: (7,0) is RESIDENT at distance 7, well past
	 * admit_radius + 1, so the ONLY thing that can keep it installed is a
	 * veto. The drains below run 600 residency steps -- far more than the
	 * ~30 eligible columns left over from (a)-(e) -- so a veto that did not
	 * hold would let this column be picked many times over. */
	adm_anchor(0, 0);
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT && adm_installed(7, 0)
		   && !s_r->chunks[i].unload_pending);
	assert(7 > s_r->admit_radius + 1
		   && "the column really is past the hysteresis (Chebyshev 7 from "
			  "column 0, admit_radius + 1 = 3)");
	uint32_t failures_before = s_r->demotion_failures;
	/* (f1) an active promotion naming this very entry. Armed directly: the
	 * production path cannot reach RESIDENT + PROMOTING at once (a promoting
	 * entry's state IS PROMOTING), so the state test is what catches this in
	 * the field and the per-entry test is the belt that has to keep holding
	 * if anyone ever relaxes that. */
	s_r->promotion.active = true;
	s_r->promotion.entry = i;
	uint32_t drained = adm_drain_residency(600);
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT && adm_installed(7, 0)
		   && "no demotion while a body for this column is mid-decode");
	assert(drained > 0
		   && "and the step was live: the other eligible columns demoted");
	memset(&s_r->promotion, 0, sizeof(s_r->promotion));
	/* (f2) an unload the pump has not emitted yet owns the entry. */
	s_r->chunks[i].unload_pending = true;
	(void)adm_drain_residency(600);
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT && adm_installed(7, 0)
		   && "no demotion while an unload is pending");
	s_r->chunks[i].unload_pending = false;
	/* Neither veto is a failure: a failure means "tried and could not". */
	assert(s_r->demotion_failures == failures_before);
	/* With both vetoes gone the very same column demotes, which is what
	 * proves the two assertions above were about the vetoes and not about
	 * some unrelated reason this column could not be demoted. */
	uint32_t demotions_before = s_r->demotions;
	adm_residency_step();
	assert(s_r->demotions == demotions_before + 1
		   && s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
		   && !adm_installed(7, 0) && s_r->chunks[i].wire.compressed
		   && "and demotes the moment the vetoes are gone");
	assert(s_r->demotion_failures == failures_before && !s_r->stop_now);
	printf("PASS (f) vetoes: demotion refuses during an active promotion of "
		   "the same column and while an unload is pending, then succeeds\n");
}

/* ======================================================================
 * W2 (mp-split-0912): the MEM2 backing reservation's budget cases
 * ======================================================================
 * Design authority docs/mp_admission_budget_design_2026-09-13.md section 2.4
 * (reservation + capacity failure) and 2.5 (deferred-log compaction), Astra
 * gate-2 items 4, 5 and 7. Cases (a)-(f) above prove the admission gate and
 * the lossless demotion; these prove the BYTES -- that the 441-column join
 * burst fits a Wii-sized reservation, that a poor-compression burst still
 * fits, that a burst that genuinely does not fit ends the session cleanly
 * instead of crashing, that the log is bounded without losing edits, and
 * that an unload returns its bytes. */

static uint8_t adm_out2[ADM_RAW];

/* REALISTIC SYNTHETIC TERRAIN, keyed by an explicit (key_x, key_z) so two
 * different columns can carry IDENTICAL content -- which is what case (j)'s
 * compacted-vs-resident equivalence needs. Layered bedrock / stone / dirt /
 * grass / air with a per-column surface height and ~2% iron-ore noise in the
 * stone, plus a sky-light plane that is 15 above the surface and 0 below,
 * i.e. the shape a Beta server actually sends. `noise` bytes of incompressible
 * random data are written over the light planes to model a POORLY compressing
 * column (cases (h)/(i)); 0 is the ordinary case.
 *
 * MEASURED compressed sizes (zlib default level, the same level a Beta server
 * uses for Packet51, over all 441 columns of the 21x21 burst):
 *   noise 0      -> avg 1,674 B  (min 1,586 / max 1,753), 441 = 0.70 MiB
 *   noise 13,400 -> avg 15,575 B (min 15,468 / max 15,669), 441 = 6.55 MiB
 *   noise 18,600 -> avg 20,812 B (min 20,708 / max 20,909), 441 = 8.75 MiB
 * against an 8 MiB (8.389 MB) Wii reservation: the first two fit, the third
 * does not -- which is exactly the three cases below. */
static void adm_fill_terrain(int key_x, int key_z, uint8_t* raw, size_t noise) {
	uint32_t state = ((uint32_t)key_x * 374761393u)
		^ ((uint32_t)key_z * 668265263u) ^ 0x85EBCA6Bu;
	uint8_t* ids = raw;
	uint8_t* metadata = raw + ADM_CELLS;
	uint8_t* torch = metadata + ADM_NIB;
	uint8_t* sky = torch + ADM_NIB;
	memset(metadata, 0, ADM_NIB);
	memset(torch, 0, ADM_NIB);
	memset(sky, 0, ADM_NIB);
	for(unsigned x = 0; x < 16; x++)
		for(unsigned z = 0; z < 16; z++) {
			int h = 62 + (int)(adm_rng(&state) % 7u);
			for(unsigned y = 0; y < 128; y++) {
				size_t idx = ((size_t)x * 16u + z) * 128u + y;
				uint8_t id;
				if(y == 0)
					id = BLOCK_BEDROCK;
				else if((int)y < h - 4)
					id = BLOCK_STONE;
				else if((int)y < h)
					id = BLOCK_DIRT;
				else if((int)y == h)
					id = BLOCK_GRASS;
				else
					id = BLOCK_AIR;
				if(id == BLOCK_STONE && adm_rng(&state) % 50u == 0)
					id = BLOCK_IRON_ORE;
				ids[idx] = id;
				unsigned lit = (int)y > h ? 15u : 0u;
				sky[idx >> 1] |= (uint8_t)(lit << ((y & 1u) * 4u));
			}
		}
	/* Incompressible filler. It lands in the LIGHT planes on purpose: ids and
	 * metadata stay a valid, comparable column, so a case that also checks
	 * content still can. Padding the compressed body instead would not work --
	 * the promotion's inflate refuses a stream with bytes left over
	 * ("remote Packet51 inflated length mismatch"), so a poorly compressing
	 * column has to be poorly compressing for real. */
	for(size_t i = 0; i < noise; i++) {
		uint8_t b = (uint8_t)(adm_rng(&state) & 0xFFu);
		if(i < ADM_NIB)
			torch[i] = b;
		else if(i - ADM_NIB < ADM_NIB)
			sky[i - ADM_NIB] = b;
	}
}

/* Retain one terrain column at (cx, cz) whose CONTENT comes from
 * (key_x, key_z). Returns the compressed length, or 0 when the reservation
 * refused the body (case (i)'s capacity path). */
static size_t adm_retain_terrain(int cx, int cz, int key_x, int key_z,
								 size_t noise) {
	adm_fill_terrain(key_x, key_z, adm_raw, noise);
	uLongf n = 0;
	unsigned char* compressed = adm_body_from(adm_raw, ADM_RAW, &n);
	if(!compressed)
		return 0;
	struct mp_event e = {.type = MP_EVENT_MAP_CHUNK};
	e.payload.map_chunk = (struct mp_map_chunk) {
		.x = cx * 16, .y = 0, .z = cz * 16, .sx = 16, .sy = 128, .sz = 16,
		.compressed = compressed, .compressed_length = n,
		.inflated_length = ADM_RAW};
	assert(remote_retain_map_locked(s_r, &e));
	return (size_t)n;
}

/* Tear the session down, give the world back, and start a fresh one over a
 * reservation of exactly `backing_bytes`. server_remote_destroy routes
 * through server_remote_cancel -> remote_chunks_clear_locked, which frees
 * every retained body, so the in_use assertion below is also a LEAK CHECK on
 * everything the previous case did. */
static void adm_fresh_session(size_t backing_bytes) {
	server_remote_destroy(&adm_owner);
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	assert(in_use == 0
		   && "the previous session returned every body to the reservation");
	mp_backing_reset();
	assert(server_remote_init(&adm_owner));
	s_r = adm_owner.impl;
	/* AFTER the init, not before: server_remote_init sizes the reservation
	 * from `mp.backing_mib` / the platform default (12 MiB on PC), so a
	 * configure ahead of it would simply be overwritten. This case wants the
	 * emulated 8 MiB Wii budget, and the MP layer's own "bytes available"
	 * view has to be told the same number. */
	assert(mp_backing_configure(backing_bytes) == backing_bytes);
	server_remote_test_set_retained_byte_cap(&adm_owner, backing_bytes);
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);
}

#define ADM_WII_BACKING (8u * 1024u * 1024u)
#define ADM_BUDGET_RADIUS 5

/* ---- (g) the 441-column burst fits a Wii-sized reservation ------------ */
static void test_burst_fits_wii_budget(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_BUDGET_RADIUS);
	size_t total = 0, smallest = (size_t)-1, largest = 0;
	for(int cx = -ADM_BURST; cx <= ADM_BURST; cx++)
		for(int cz = -ADM_BURST; cz <= ADM_BURST; cz++) {
			size_t n = adm_retain_terrain(cx, cz, cx, cz, 0);
			assert(n && "no refusal on the acceptance workload");
			total += n;
			if(n < smallest)
				smallest = n;
			if(n > largest)
				largest = n;
		}
	assert(s_r->retained_entries == 441);
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	assert(capacity == ADM_WII_BACKING);
	assert(peak < capacity
		   && "441 columns of ordinary terrain fit the Wii reservation");
	assert(in_use == total && "every body's bytes are accounted, exactly");
	adm_pump_n(441 * 4);
	size_t resident = adm_count(REMOTE_CHUNK_RESIDENT);
	assert(resident
		   == (2 * ADM_BUDGET_RADIUS + 1) * (2 * ADM_BUDGET_RADIUS + 1));
	assert(resident <= 121 && "the Wii's (2R+1)^2 ceiling at R=5");
	/* Every announced column is accounted for: installed, or backed with its
	 * complete body. Astra item 4's "account for every tenant". */
	for(int cx = -ADM_BURST; cx <= ADM_BURST; cx++)
		for(int cz = -ADM_BURST; cz <= ADM_BURST; cz++) {
			size_t i = adm_entry(cx, cz);
			assert(i != REMOTE_CHUNK_ENTRY_CAPACITY);
			if(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT)
				assert(adm_installed(cx, cz));
			else
				assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
					   && s_r->chunks[i].wire.compressed
					   && mp_backing_owns(s_r->chunks[i].wire.compressed));
		}
	assert(s_r->retention_evictions == 0 && s_r->backing_refusals == 0
		   && s_r->forced_reconnects == 0 && adm_count(REMOTE_CHUNK_TOMBSTONE) == 0);
	assert(s_r->mp_end == 0 && !s_r->stop_now);
	mp_backing_stats(&in_use, &peak, &capacity);
	printf("PASS (g) 441-column burst: %zu B/column measured (min %zu, max "
		   "%zu), backing peak %zu B of %zu B capacity, %zu resident (R=%d), "
		   "%zu backed, 0 refusals, mp_end=%u\n",
		   total / 441u, smallest, largest, peak, capacity, resident,
		   ADM_BUDGET_RADIUS, adm_count(REMOTE_CHUNK_RETAINED),
		   (unsigned)s_r->mp_end);
}

/* ---- (h) a poor-compression burst still fits -------------------------- */
#define ADM_NOISE_15K 13400u
static void test_poor_compression_burst_fits(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_BUDGET_RADIUS);
	size_t total = 0;
	for(int cx = -ADM_BURST; cx <= ADM_BURST; cx++)
		for(int cz = -ADM_BURST; cz <= ADM_BURST; cz++) {
			size_t n = adm_retain_terrain(cx, cz, cx, cz, ADM_NOISE_15K);
			assert(n && "a 15 KB-per-column server must still be playable");
			total += n;
		}
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	assert(peak < capacity && in_use == total);
	adm_pump_n(441 * 4);
	assert(adm_count(REMOTE_CHUNK_RESIDENT)
		   == (2 * ADM_BUDGET_RADIUS + 1) * (2 * ADM_BUDGET_RADIUS + 1));
	assert(s_r->backing_refusals == 0 && s_r->mp_end == 0 && !s_r->stop_now
		   && s_r->retention_evictions == 0);
	printf("PASS (h) poor compression: %zu B/column measured, 441 columns = "
		   "%zu B, backing peak %zu B of %zu B, no refusal, mp_end=%u\n",
		   total / 441u, total, peak, capacity, (unsigned)s_r->mp_end);
}

/* ---- (i) an over-budget burst fails CLEANLY --------------------------- */
#define ADM_NOISE_20K 18600u
static void test_over_budget_fails_cleanly(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_BUDGET_RADIUS);
	size_t admitted = 0, total = 0, refused_at = 0;
	for(int cx = -ADM_BURST; cx <= ADM_BURST && !refused_at; cx++)
		for(int cz = -ADM_BURST; cz <= ADM_BURST && !refused_at; cz++) {
			size_t n = adm_retain_terrain(cx, cz, cx, cz, ADM_NOISE_20K);
			if(n) {
				admitted++;
				total += n;
				continue;
			}
			/* THE PRODUCTION PATH. mp_backing_alloc answered "no room", which
			 * is exactly what the decoder's Packet51 site sees; the read
			 * thread then calls this, and a false return means the session has
			 * already ended cleanly. */
			refused_at = admitted + 1;
			assert(!remote_backing_reclaim_or_fail_locked(s_r, 20u * 1024u)
				   && "nothing is reclaimable, so this is a capacity failure");
		}
	assert(refused_at && "441 x ~20 KB must not fit an 8 MiB reservation");
	assert(s_r->mp_end == SERVER_REMOTE_END_CAPACITY && s_r->mp_end == 5u);
	assert(s_r->backing_refusals == 1);
	assert(s_r->stop_now && "the read loop is told to stop");
	assert(strstr(s_r->reason, "does not fit this console")
		   && "the player is told WHY, not shown a decode error");
	assert(adm_count(REMOTE_CHUNK_TOMBSTONE) == 0
		   && s_r->retention_evictions == 0
		   && "a capacity failure never destroys a column");
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	char reason[sizeof(s_r->reason)];
	snprintf(reason, sizeof(reason), "%s", s_r->reason);
	printf("PASS (i) over budget: %zu B/column, refused at column %zu of 441 "
		   "(%zu B held of %zu B), mp_end=%u, reason \"%s\"\n",
		   total / (admitted ? admitted : 1u), refused_at, in_use, capacity,
		   (unsigned)s_r->mp_end, reason);
	/* No crash, and no leak: the cancel inside adm_fresh_session's destroy
	 * returns every body, which its own in_use == 0 assertion proves. */
	adm_fresh_session(ADM_WII_BACKING);
	mp_backing_stats(&in_use, &peak, &capacity);
	assert(in_use == 0 && peak == 0 && "cancelled session leaked nothing");
	printf("PASS (i) cancel: backing in_use back to 0 after the capacity "
		   "failure, no leak\n");
}

/* ---- (j) compaction bounds the log without losing an edit ------------- */
/* REL-MP-FIX3a: A + 2 x filler > HIGH_WATER and 2 x filler <= LOW_WATER after
 * A, so the latch releases after exactly one fold (latched drain: case s8).
 * REL-MP-JOINSPEED: derived from the marks (4,196 + 2 x 4,096 at 12,288 /
 * 8,192) instead of the 400 + 2 x 100 the 1,024-entry log allowed. */
#define ADM_COMPACT_EDITS \
	(REMOTE_DEFERRED_EDIT_HIGH_WATER - REMOTE_DEFERRED_EDIT_LOW_WATER + 100)
#define ADM_FILLER_EDITS (REMOTE_DEFERRED_EDIT_LOW_WATER / 2)
_Static_assert(ADM_COMPACT_EDITS > ADM_FILLER_EDITS
				   && ADM_COMPACT_EDITS + 2 * ADM_FILLER_EDITS
					   > REMOTE_DEFERRED_EDIT_HIGH_WATER
				   && 2 * ADM_FILLER_EDITS <= REMOTE_DEFERRED_EDIT_LOW_WATER
				   && ADM_COMPACT_EDITS + 2 * ADM_FILLER_EDITS
					   < REMOTE_DEFERRED_EDIT_CAPACITY
				   && ADM_COMPACT_EDITS < 16 * 16 * 100,
			   "(j): one fold of A crosses both marks; A's cells fit y 20..119");
static void adm_compact_edit(int cx, int cz, int n, int* out_x, int* out_y,
							 int* out_z, int* out_id) {
	static const int palette[]
		= {BLOCK_PLANKS, BLOCK_BRICKS, BLOCK_STONE, BLOCK_DIRT};
	/* DISTINCT cells: the log holds one entry per coordinate, so a repeat
	 * would collapse two edits into one and the counts below would lie. */
	*out_x = cx * 16 + (n % 16);
	*out_z = cz * 16 + ((n / 16) % 16);
	*out_y = 20 + n / 256;
	*out_id = palette[n % 4];
}

static void test_compaction(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_RADIUS);
	/* A: out of radius, so it stays BACKED and its edits park. B: in radius,
	 * so it installs and its edits apply live. IDENTICAL content (both keyed
	 * (0,0)), which is what makes the byte compare at the end a statement
	 * about compaction and nothing else. */
	adm_retain_terrain(7, 0, 0, 0, 0);
	adm_retain_terrain(2, 0, 0, 0, 0);
	adm_retain_terrain(0, 7, 0, 0, 0);  /* filler, BACKED */
	adm_retain_terrain(0, -7, 0, 0, 0); /* filler, BACKED */
	adm_pump_n(40);
	assert(adm_state(2, 0) == REMOTE_CHUNK_RESIDENT && adm_installed(2, 0));
	assert(adm_state(7, 0) == REMOTE_CHUNK_RETAINED && !adm_installed(7, 0));
	/* The same edits to both, at the same in-column offsets. */
	for(int n = 0; n < ADM_COMPACT_EDITS; n++) {
		int x, y, z, id;
		adm_compact_edit(7, 0, n, &x, &y, &z, &id);
		adm_edit(x, y, z, id);
		adm_compact_edit(2, 0, n, &x, &y, &z, &id);
		adm_edit(x, y, z, id);
		assert(world_get_block(&gstate.world, x, y, z).type == (uint8_t)id
			   && "B is resident: its edit applied live");
	}
	assert(s_r->deferred_edits == ADM_COMPACT_EDITS
		   && "only A's edits parked");
	for(int n = 0; n < ADM_FILLER_EDITS; n++) {
		int x, y, z, id;
		adm_compact_edit(0, 7, n, &x, &y, &z, &id);
		adm_edit(x, y, z, id);
		adm_compact_edit(0, -7, n, &x, &y, &z, &id);
		adm_edit(x, y, z, id);
	}
	uint32_t parked = s_r->deferred_edits;
	assert(parked == ADM_COMPACT_EDITS + 2 * ADM_FILLER_EDITS);
	assert(parked > REMOTE_DEFERRED_EDIT_HIGH_WATER
		   && parked < REMOTE_DEFERRED_EDIT_CAPACITY
		   && "past the compaction trigger, short of the kill threshold");
	assert(s_r->compactions == 0 && !s_r->stop_now);
	uint64_t head_before = s_r->chunks[adm_entry(7, 0)].arrival;
	adm_pump();
	assert(s_r->compactions == 1 && "one compaction per pump, and it ran");
	assert(s_r->compacted_edits == ADM_COMPACT_EDITS
		   && "the heaviest backed column's whole set was folded in");
	assert(s_r->deferred_edits == parked - ADM_COMPACT_EDITS
		   && "and only those slots were released");
	assert(s_r->deferred_edits_dropped == 0
		   && "nothing was lost: the log shrank because the BODY grew");
	assert(s_r->chunks[adm_entry(7, 0)].arrival > head_before
		   && "the new head is stamped with the newest edit it contains");
	assert(s_r->compaction_failures == 0 && !s_r->stop_now);
	/* B's state as a resident column that took the same edits live. */
	assert(adm_export(2, 0));
	memcpy(adm_out2, adm_out, ADM_RAW);
	/* Re-promote A from its compacted body and compare. */
	adm_anchor(7, 0);
	adm_pump_n(200);
	assert(adm_state(7, 0) == REMOTE_CHUNK_RESIDENT && adm_installed(7, 0));
	assert(adm_export(7, 0));
	assert(memcmp(adm_out, adm_out2, ADM_CELLS) == 0
		   && "compacted + re-promoted == resident + live edits, every id");
	assert(memcmp(adm_out + ADM_CELLS, adm_out2 + ADM_CELLS, ADM_NIB) == 0
		   && "and every metadata nibble");
	printf("PASS (j) compaction: %u edits parked, 1 compaction folded %u of "
		   "them into the head, log %u -> %u, 0 dropped; re-promotion is "
		   "byte-identical to a resident column over all %u id + %u metadata "
		   "bytes (light planes stay as the server sent them -- a compacted "
		   "column relights like a fresh install, documented deviation)\n",
		   parked, s_r->compacted_edits, parked, s_r->deferred_edits,
		   ADM_CELLS, ADM_NIB);
}

/* ---- (k) an unload returns the column's bytes ------------------------- */
static void test_unload_returns_bytes(void) {
	/* (j) left (0,7) BACKED with its body and a partly drained log. */
	size_t i = adm_entry(0, 7);
	assert(i != REMOTE_CHUNK_ENTRY_CAPACITY);
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
		   && s_r->chunks[i].wire.compressed);
	size_t body_bytes = s_r->chunks[i].wire.compressed_length;
	size_t in_use_before = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use_before, &peak, &capacity);
	size_t retained_before = s_r->retained_bytes;
	/* Packet50 load=false, then the pump's emit -- the real 0x32 path. */
	assert(remote_pre_chunk_locked(s_r, 0, 7, false));
	remote_emit_pending_unloads_locked(s_r);
	size_t in_use_after = 0;
	mp_backing_stats(&in_use_after, &peak, &capacity);
	assert(in_use_after == in_use_before - body_bytes
		   && "the unloaded column's bytes are back in the reservation");
	assert(s_r->retained_bytes == retained_before - body_bytes
		   && "and the MP layer's own accounting agrees with it");
	assert(adm_entry(0, 7) == REMOTE_CHUNK_ENTRY_CAPACITY
		   && "the entry itself is released");
	/* And the reservation can hand those exact bytes straight back out. */
	void* again = mp_backing_alloc(body_bytes);
	assert(again && "a freed body's bytes are immediately reusable");
	mp_backing_free(again);
	printf("PASS (k) unload: 0x32 returned %zu B to the reservation (%zu -> "
		   "%zu in use) and the entry was released\n",
		   body_bytes, in_use_before, in_use_after);
}

/* ---- (j2)/(j3) REL-MP: compaction reaches CHAINED backed columns ------- */
/* tmp/jobs/loop/REL-MP-DIAG/DIAG.md: fresh Beta terrain parks >1024 edits on
 * backed columns that each hold a full head plus queued partial Packet51
 * bodies (PlayerInstance.java:133-160); compaction skipped every chained
 * column and the log ended the session. Run after (k), which needs (j)'s
 * live session. A case returns 1 on ANY terminal, but prints the
 * "REL-MP RED:" marker only for mp_end 6 with the exact exhaustion reason. */
#define ADM_REL_MP_REASON "remote deferred edit log exhausted"
static int adm_rel_mp_terminal(const char* name) {
	if(!s_r->stop_now && s_r->mp_end == 0)
		return 0;
	/* One production pump publishes the terminal and latches mp_end
	 * (server_remote_pump's side-slot latch), as it would in a session. */
	server_remote_pump(&adm_owner);
	struct server_remote_status status;
	server_remote_get_status(&adm_owner, &status);
	char reason[sizeof(status.reason)];
	snprintf(reason, sizeof reason, "%s",
			 status.reason[0] ? status.reason : s_r->reason);
	bool red = status.mp_end == SERVER_REMOTE_END_PROTOCOL
		&& strcmp(reason, ADM_REL_MP_REASON) == 0;
	printf("%s %s: mp_end=%u reason=%s state=%d stop_now=%d "
		   "deferred_edits=%u\n",
		   red ? "REL-MP RED:" : "FAIL (unexpected terminal)", name,
		   (unsigned)status.mp_end, reason, (int)status.state,
		   (int)s_r->stop_now, s_r->deferred_edits);
	return 1;
}
/* adm_edit always writes metadata 0; these cases need nonzero metadata. */
static void adm_edit_meta(int x, int y, int z, int id, int meta) {
	s_r->pump_packet_seq = ++s_r->arrival_sequence;
	s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, x, (uint8_t)y, z, (uint8_t)id, (uint8_t)meta);
}
/* Partial body y0..y0+sy-1, uniform id + metadata nibble, light 0. */
static void adm_retain_partial_meta(int cx, int cz, int y0, int sy, int id,
									int meta) {
	size_t cells = 16u * (size_t)sy * 16u, bytes = cells * 5u / 2u;
	unsigned char* rawp = calloc(1, bytes);
	assert(rawp);
	memset(rawp, id, cells);
	memset(rawp + cells, meta | (meta << 4), cells / 2u);
	uLongf n = 0;
	unsigned char* compressed = adm_body_from(rawp, bytes, &n);
	assert(compressed);
	free(rawp);
	struct mp_event e = {.type = MP_EVENT_MAP_CHUNK};
	e.payload.map_chunk = (struct mp_map_chunk) {
		.x = cx * 16, .y = (int16_t)y0, .z = cz * 16, .sx = 16,
		.sy = (uint16_t)sy, .sz = 16, .compressed = compressed,
		.compressed_length = n, .inflated_length = bytes};
	assert(remote_retain_map_locked(s_r, &e));
}
/* Independent reference: one cell of a wire-layout column. */
static void adm_ref_set(int x, int y, int z, int id, int meta) {
	size_t idx = ((size_t)(x & 15) * 16u + (size_t)(z & 15)) * 128u + (size_t)y;
	unsigned shift = ((unsigned)y & 1u) * 4u;
	uint8_t* m = adm_ref + ADM_CELLS + (idx >> 1);
	adm_ref[idx] = (uint8_t)id;
	*m = (uint8_t)((*m & ~(0x0Fu << shift)) | ((unsigned)(meta & 15) << shift));
}
static void adm_ref_partial(int y0, int sy, int id, int meta) {
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = y0; y < y0 + sy; y++)
				adm_ref_set(x, y, z, id, meta);
}
/* Edit n of a column: distinct cells from y=base up, wool, meta 1..15. */
static void adm_j_cell(int cx, int cz, int n, int base, int* x, int* y, int* z) {
	*x = cx * 16 + n % 16;
	*z = cz * 16 + (n / 16) % 16;
	*y = base + n / 256;
}
/* Promote (cx,cz), then compare installed ids + metadata with adm_ref. */
static void adm_promote_matches_ref(int cx, int cz) {
	adm_anchor(cx, cz);
	adm_pump_n(200);
	assert(adm_state(cx, cz) == REMOTE_CHUNK_RESIDENT && adm_installed(cx, cz));
	assert(adm_export(cx, cz));
	assert(memcmp(adm_out, adm_ref, ADM_CELLS) == 0 && "every id, wire order");
	assert(memcmp(adm_out + ADM_CELLS, adm_ref + ADM_CELLS, ADM_NIB) == 0
		   && "every metadata nibble, wire order");
}

/* per column: 2 x (CAP/2 + 8) > the whole log (REL-MP-JOINSPEED: was 520
 * for the 1,024-entry log; cells y 20..84). */
#define ADM_J2_OUTSIDE (REMOTE_DEFERRED_EDIT_CAPACITY / 2 + 8)
static int test_chained_compaction(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_RADIUS);
	static const int col[2][2] = {{7, 0}, {0, 7}};
	static const int body_id[2] = {BLOCK_LOG, BLOCK_WOOL}, body_meta[2] = {2, 7};
	for(int c = 0; c < 2; c++) {
		adm_retain_terrain(col[c][0], col[c][1], col[c][0], col[c][1], 0);
		adm_retain_partial_meta(col[c][0], col[c][1], 0, 8, body_id[c],
								body_meta[c]);
	}
	adm_pump_n(40);
	for(int c = 0; c < 2; c++) {
		size_t i = adm_entry(col[c][0], col[c][1]);
		assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
			   && !s_r->chunks[i].installed && s_r->chunks[i].queued_count == 1
			   && "a backed full head with one queued partial");
	}
	/* Distinct edits outside the partials (y >= 20), then newer edits INSIDE
	 * them (y 3), interleaved across both columns, no pump in between. */
	for(int n = 0; n < ADM_J2_OUTSIDE + 4; n++)
		for(int c = 0; c < 2; c++) {
			int x, y, z;
			adm_j_cell(col[c][0], col[c][1], n, 20, &x, &y, &z);
			if(n >= ADM_J2_OUTSIDE)
				y = 3;
			adm_edit_meta(x, y, z, BLOCK_WOOL, 1 + n % 15);
			if(adm_rel_mp_terminal("(j2) chained backed columns"))
				return 1;
		}
	assert(s_r->deferred_edit_log_exhausted == 0
		   && s_r->deferred_edits_dropped == 0 && "no stop and no drop");
	assert(s_r->compactions > 0 && s_r->deferred_log_rescues > 0
		   && "the exhaustion path compacted a chained column");
	assert((s_r->chunks[adm_entry(7, 0)].queued_count == 0
			|| s_r->chunks[adm_entry(0, 7)].queued_count == 0)
		   && "a compacted victim's chain was collapsed into its head");
	for(int c = 0; c < 2; c++) {
		adm_fill_terrain(col[c][0], col[c][1], adm_ref, 0);
		adm_ref_partial(0, 8, body_id[c], body_meta[c]);
		for(int n = 0; n < ADM_J2_OUTSIDE + 4; n++) {
			int x, y, z;
			adm_j_cell(col[c][0], col[c][1], n, 20, &x, &y, &z);
			adm_ref_set(x, n >= ADM_J2_OUTSIDE ? 3 : y, z, BLOCK_WOOL, 1 + n % 15);
		}
		adm_promote_matches_ref(col[c][0], col[c][1]);
	}
	assert(!s_r->stop_now && s_r->mp_end == 0);
	printf("PASS (j2) chained compaction: %d distinct edits on 2 chained "
		   "backed columns, %u compactions, 0 dropped, both promote byte-exact "
		   "(ids + metadata) to head, body, edits in wire order\n",
		   2 * (ADM_J2_OUTSIDE + 4), s_r->compactions);
	return 0;
}

/* The pump's order (server_remote_pump): an edit is received into the ring
 * by the read thread, then applied stamped with its receive sequence while it
 * is still at the ring head, then popped. */
static void adm_ring_push(int x, int y, int z, int id, int meta) {
	assert(s_r->event_count < REMOTE_QUEUE_CAPACITY);
	struct remote_event* e = &s_r->events[(s_r->event_head + s_r->event_count)
										  % REMOTE_QUEUE_CAPACITY];
	memset(e, 0, sizeof(*e));
	e->kind = REMOTE_EVENT_PACKET;
	e->generation = s_r->generation;
	e->world_epoch = s_r->world_epoch;
	e->seq = ++s_r->arrival_sequence;
	e->packet.type = MP_EVENT_BLOCK_CHANGE;
	e->packet.payload.block_change.x = x;
	e->packet.payload.block_change.y = (uint8_t)y;
	e->packet.payload.block_change.z = z;
	e->packet.payload.block_change.block_type = (uint8_t)id;
	e->packet.payload.block_change.metadata = (uint8_t)meta;
	s_r->event_count++;
}
static void adm_ring_pump_one(void) {
	assert(s_r->event_count);
	const struct remote_event* e = &s_r->events[s_r->event_head];
	s_r->pump_packet_seq = e->seq;
	s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, e->packet.payload.block_change.x,
							  e->packet.payload.block_change.y,
							  e->packet.payload.block_change.z,
							  e->packet.payload.block_change.block_type,
							  e->packet.payload.block_change.metadata);
	s_r->event_head = (s_r->event_head + 1) % REMOTE_QUEUE_CAPACITY;
	s_r->event_count--;
}

#define ADM_J3_FILLER (REMOTE_DEFERRED_EDIT_HIGH_WATER + 8)
#define ADM_J3_DRIVE (REMOTE_DEFERRED_EDIT_CAPACITY + 8)
static int test_ring_order_supersede(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_RADIUS);
	const int cx = 7, cz = 0, bx = cx * 16;
	assert(remote_pre_chunk_locked(s_r, cx, cz, true)); /* Packet50 load */
	/* E0: older than the head. y 120: above every filler/drive cell (y <= 104
	 * since REL-MP-JOINSPEED sized the drive past the 16,384-entry log). */
	adm_ring_push(bx + 4, 120, 4, BLOCK_WOOL, 3);
	adm_retain_terrain(cx, cz, cx, cz, 0);
	adm_ring_pump_one(); /* E0 reaches the pump after its covering head */
	for(int n = 0; n < ADM_J3_FILLER; n++) { /* newer than the head, y 20+ */
		int x, y, z;
		adm_j_cell(cx, cz, n, 20, &x, &y, &z);
		adm_edit_meta(x, y, z, BLOCK_WOOL, 1 + n % 15);
	}
	adm_ring_push(bx + 1, 2, 1, BLOCK_WOOL, 5);	 /* E1: inside P1, older */
	adm_ring_push(bx + 2, 10, 2, BLOCK_WOOL, 6); /* E2: inside P2, older */
	adm_retain_partial_meta(cx, cz, 0, 8, BLOCK_LOG, 1);   /* P1 */
	adm_retain_partial_meta(cx, cz, 8, 8, BLOCK_WOOL, 9);  /* P2 */
	size_t i = adm_entry(cx, cz);
	assert(s_r->chunks[i].queued_count == 2 && s_r->event_count == 2);
	uint64_t head_stamp = s_r->chunks[i].arrival;
	uint32_t compactions = s_r->compactions, parked = s_r->deferred_edits;
	assert(parked > REMOTE_DEFERRED_EDIT_HIGH_WATER);
	adm_pump(); /* high-water compaction while E1/E2 are still in the ring */
	assert(s_r->compactions == compactions && s_r->chunks[i].queued_count == 2
		   && s_r->chunks[i].arrival == head_stamp
		   && s_r->deferred_edits == parked
		   && "ring edits for the column make the fold WAIT");
	adm_ring_pump_one();
	adm_ring_pump_one(); /* E1, E2 reach the pump after their bodies */
	adm_pump();			 /* ring clear: the high-water compaction may fold */
	bool high_water_folded = s_r->compactions > compactions;
	for(int n = 0; n < ADM_J3_DRIVE; n++) { /* then drive past the log cap */
		int x, y, z;
		adm_j_cell(cx, cz, n, 40, &x, &y, &z);
		adm_edit_meta(x, y, z, BLOCK_WOOL, 1 + (n + 7) % 15);
		if(adm_rel_mp_terminal("(j3) ring-order chained column"))
			return 1;
	}
	assert(high_water_folded && s_r->chunks[i].queued_count == 0
		   && "the high-water fold collapsed the chain once the ring cleared");
	assert(s_r->deferred_edits_dropped == 0 && s_r->deferred_edit_log_exhausted == 0);
	adm_fill_terrain(cx, cz, adm_ref, 0);
	adm_ref_partial(0, 8, BLOCK_LOG, 1);
	adm_ref_partial(8, 8, BLOCK_WOOL, 9);
	for(int n = 0; n < ADM_J3_FILLER; n++) {
		int x, y, z;
		adm_j_cell(cx, cz, n, 20, &x, &y, &z);
		adm_ref_set(x, y, z, BLOCK_WOOL, 1 + n % 15);
	}
	for(int n = 0; n < ADM_J3_DRIVE; n++) {
		int x, y, z;
		adm_j_cell(cx, cz, n, 40, &x, &y, &z);
		adm_ref_set(x, y, z, BLOCK_WOOL, 1 + (n + 7) % 15);
	}
	adm_promote_matches_ref(cx, cz);
	struct block_data b0 = world_get_block(&gstate.world, bx + 4, 120, 4),
					  b1 = world_get_block(&gstate.world, bx + 1, 2, 1),
					  b2 = world_get_block(&gstate.world, bx + 2, 10, 2);
	assert(b0.type == BLOCK_AIR && "E0 older than the head: the head wins");
	assert(b1.type == BLOCK_LOG && b1.metadata == 1 && "P1 beats older E1");
	assert(b2.type == BLOCK_WOOL && b2.metadata == 9 && "P2 beats older E2");
	assert(!s_r->stop_now && s_r->mp_end == 0);
	puts("PASS (j3) ring order: the fold WAITs while column edits are in the "
		 "ring; late-parked older edits lose to the head and to each queued "
		 "body; promotion is byte-exact");
	return 0;
}

/* --rel-mp-chained: only (j2)/(j3), for the RED receipts. */
static int adm_rel_mp_cases(void) {
	int rc = test_chained_compaction();
	rc |= test_ring_order_supersede();
	return rc;
}

/* ---- (l) the W1 residual: a partial body arriving AFTER residency ----- */
/* W1's demotion precondition was `state == REMOTE_CHUNK_RESIDENT`. A Packet51
 * for an already installed column moves the entry to RETAINED (the new body
 * is pending) while the voxels stay installed, so such a column could never
 * be demoted -- it held its 81,920 B of carve for the whole session even far
 * outside the admission radius, and outside the radius the promotion gate
 * will not consume the pending body either, so nothing ever cleared it. W1
 * recorded this as the residual for W2; struct remote_chunk::installed is the
 * fix, and the re-encode PREPENDS itself ahead of the pending body so the
 * ordering survives. */
static void test_partial_after_residency_demotes(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_RADIUS);
	adm_retain_terrain(1, 0, 1, 0, 0);
	adm_pump_n(40);
	assert(adm_state(1, 0) == REMOTE_CHUNK_RESIDENT && adm_installed(1, 0));
	size_t i = adm_entry(1, 0);
	assert(s_r->chunks[i].installed && !s_r->chunks[i].wire.compressed
		   && "a published column holds voxels and no pending body");
	/* THE RESIDUAL STATE: a newer partial for the installed column. */
	adm_retain_partial(1, 0, 8, BLOCK_BRICKS);
	assert(adm_state(1, 0) == REMOTE_CHUNK_RETAINED
		   && "the entry is BACKED again, with the partial pending");
	assert(adm_installed(1, 0) && s_r->chunks[i].installed
		   && "but the voxels are still installed -- W1 could not demote this");
	uint64_t partial_stamp = s_r->chunks[i].arrival;
	uint32_t demotions_before = s_r->demotions;
	/* Out of radius + 1, so it must now demote. */
	adm_anchor(9, 0);
	adm_pump_n(40);
	assert(s_r->demotions == demotions_before + 1 && "W2 demotes it");
	assert(adm_state(1, 0) == REMOTE_CHUNK_RETAINED);
	assert(!adm_installed(1, 0) && !s_r->chunks[i].installed
		   && "its 81,920 B of voxels are released");
	assert(s_r->chunks[i].wire.compressed
		   && "the re-encode is the new HEAD (what was installed)");
	assert(s_r->chunks[i].queued_count == 1
		   && s_r->chunks[i].queued_arrival[0] == partial_stamp
		   && "and the pending partial is preserved behind it, same stamp");
	assert(s_r->chunks[i].arrival < partial_stamp
		   && "stamped with the installed content, so it cannot shadow it");
	assert(s_r->demotion_failures == 0 && !s_r->stop_now);
	/* Walk back: head installs, then the partial applies on top -- exactly
	 * the order the original stream had. */
	adm_anchor(1, 0);
	adm_pump_n(200);
	assert(adm_state(1, 0) == REMOTE_CHUNK_RESIDENT
		   && s_r->chunks[i].queued_count == 0);
	adm_fill_terrain(1, 0, adm_ref, 0);
	for(unsigned x = 0; x < 16; x++)
		for(unsigned z = 0; z < 16; z++)
			for(unsigned y = 0; y < 8; y++)
				adm_ref[((size_t)x * 16u + z) * 128u + y] = BLOCK_BRICKS;
	assert(adm_export(1, 0));
	assert(memcmp(adm_out, adm_ref, ADM_CELLS) == 0
		   && "installed -> partial -> demote -> re-promote is byte-exact: "
			  "the partial's volume and the re-encode's remainder");
	printf("PASS (l) W1 residual: a partial arriving after residency leaves "
		   "RETAINED-with-voxels; W2 demotes it (re-encode prepended ahead of "
		   "the pending partial, stamps intact) and the re-promotion is "
		   "byte-exact over all %u ids\n", ADM_CELLS);
}

/* ======================================================================
 * F4 (mp-split-0912): the MEMORY-AWARE promotion gate
 * ======================================================================
 * Design 2.3/2.4, Astra gate-2 items 3-4. Real run betamc_adm_0913 ran W1's
 * radius gate at R=5 on BetaMC and still died of memory -- not with a crash
 * (oom=0, MEM1 floor 5888 KB) but with mem2_carve_free_kb pinned at 39 KB
 * from tick 11 to tick 60 and **ndisp = 0 for the whole run**: ~157 resident
 * columns x 81,920 B had taken the carve, so no display list could be
 * allocated and a fully resident world drew nothing. A radius bounds the
 * column COUNT; only a byte test bounds the carve.
 *
 * Emulating that on PC: the carve reading is a READING, so a fixed override
 * would never fall and the gate would never fire. adm_pump_emulated_carve
 * therefore re-reads the override from the columns actually installed, i.e.
 * it models chunk_alloc taking REMOTE_VOXEL_COLUMN_BYTES out of the carve on
 * every install and giving them back on every demotion -- which is exactly
 * what the Wii heap does. Every threshold below is built out of the
 * PRODUCTION constants (REMOTE_VOXEL_COLUMN_BYTES +
 * REMOTE_MESH_RESERVE_BYTES, server_remote_internal.h), so the cases follow
 * a later re-tuning of the reserve instead of freezing today's literals. */
static size_t adm_voxel_columns(void) {
	size_t n = 0;
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		if(s_r->chunks[i].installed)
			n++;
	return n;
}
/* The gate's own threshold: what the carve must hold to start ONE column. */
#define ADM_GATE_THRESHOLD \
	((size_t)REMOTE_VOXEL_COLUMN_BYTES + (size_t)REMOTE_MESH_RESERVE_BYTES)

/* Pump with the emulated carve draining/refilling as columns install and
 * demote. Returns the highest voxel-holding column count seen after any
 * pump -- the moving-window measurement case (n) is about. */
static size_t adm_pump_emulated_carve(size_t budget, int pumps) {
	size_t high_water = 0;
	for(int i = 0; i < pumps; i++) {
		size_t held = adm_voxel_columns();
		size_t spent = held * (size_t)REMOTE_VOXEL_COLUMN_BYTES;
		server_remote_test_set_carve_free_bytes(budget > spent ? budget - spent
															  : 0);
		adm_pump();
		held = adm_voxel_columns();
		if(held > high_water)
			high_water = held;
	}
	return high_water;
}

/* ---- (m) the carve gate defers instead of starving the mesher --------- */
#define ADM_GATE_COLUMNS 3u
static void test_memory_gate_defers(void) {
	size_t oom_before = mem2_stat_oom();
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_RADIUS); /* 25 in radius */
	for(int cx = -ADM_RADIUS; cx <= ADM_RADIUS; cx++)
		for(int cz = -ADM_RADIUS; cz <= ADM_RADIUS; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	size_t announced = (2 * ADM_RADIUS + 1) * (2 * ADM_RADIUS + 1);
	assert(s_r->retained_entries == announced);
	/* A carve that can afford exactly ADM_GATE_COLUMNS columns on top of the
	 * mesh reserve. All 25 are in radius, so the RADIUS gate admits every one
	 * of them: whatever stops at 3 is the memory gate and nothing else. */
	size_t budget = ADM_GATE_COLUMNS * (size_t)REMOTE_VOXEL_COLUMN_BYTES
		+ (size_t)REMOTE_MESH_RESERVE_BYTES;
	adm_pump_emulated_carve(budget, (int)announced * 8);
	assert(adm_count(REMOTE_CHUNK_RESIDENT) == ADM_GATE_COLUMNS
		   && "the carve afforded 3 columns, so 3 installed -- not 25");
	assert(adm_voxel_columns() == ADM_GATE_COLUMNS);
	uint32_t deferred = s_r->promote_deferred_memory;
	assert(deferred > 0 && "and the refusals were COUNTED, not silent");
	/* A deferral is not a loss and not a failure: every other column still
	 * holds its complete compressed body in the reservation, and nothing in
	 * the allocator or the backing had to fail to get here. */
	size_t backlog = 0;
	for(int cx = -ADM_RADIUS; cx <= ADM_RADIUS; cx++)
		for(int cz = -ADM_RADIUS; cz <= ADM_RADIUS; cz++) {
			size_t i = adm_entry(cx, cz);
			assert(i != REMOTE_CHUNK_ENTRY_CAPACITY);
			if(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT)
				continue;
			assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
				   && s_r->chunks[i].wire.compressed
				   && mp_backing_owns(s_r->chunks[i].wire.compressed)
				   && "a deferred column keeps its whole body, BACKED");
			backlog++;
		}
	assert(backlog == announced - ADM_GATE_COLUMNS);
	assert(s_r->retention_allocation_failures == 0
		   && s_r->backing_refusals == 0 && s_r->retention_evictions == 0
		   && adm_count(REMOTE_CHUNK_TOMBSTONE) == 0);
	assert(s_r->mp_end == 0 && !s_r->stop_now && s_r->forced_reconnects == 0);
	assert(mem2_stat_oom() == oom_before && "and no allocation ever OOM'd");
	printf("PASS (m) carve gate: %zu in-radius columns announced, carve sized "
		   "for %u (%zu B = %u x %u voxel + %u reserve), %zu resident, %zu "
		   "BACKED with bodies intact, promote_deferred_memory=%u, 0 alloc "
		   "failures, 0 refusals, 0 OOM, mp_end=%u\n",
		   announced, (unsigned)ADM_GATE_COLUMNS, budget,
		   (unsigned)ADM_GATE_COLUMNS, (unsigned)REMOTE_VOXEL_COLUMN_BYTES,
		   (unsigned)REMOTE_MESH_RESERVE_BYTES,
		   adm_count(REMOTE_CHUNK_RESIDENT), backlog, deferred,
		   (unsigned)s_r->mp_end);

	/* THE THRESHOLD ITSELF, either side of it. This is what pins the gate to
	 * "column + mesh reserve" rather than to "column": the two values are one
	 * byte apart and expressed only through the production constants, so a
	 * reserve that stopped being subtracted would fail the first half. */
	server_remote_test_set_carve_free_bytes(ADM_GATE_THRESHOLD - 1u);
	uint32_t deferred_at_boundary = s_r->promote_deferred_memory;
	adm_pump_n(12);
	assert(adm_count(REMOTE_CHUNK_RESIDENT) == ADM_GATE_COLUMNS
		   && "one byte short of column + reserve: nothing starts");
	assert(s_r->promote_deferred_memory > deferred_at_boundary);
	server_remote_test_set_carve_free_bytes(ADM_GATE_THRESHOLD);
	adm_pump_n(12);
	assert(adm_count(REMOTE_CHUNK_RESIDENT) > ADM_GATE_COLUMNS
		   && "exactly at column + reserve: promotion starts");
	printf("PASS (m) threshold: refused at %zu B free, admitted at %zu B "
		   "(= %u voxel + %u mesh reserve)\n",
		   ADM_GATE_THRESHOLD - 1u, ADM_GATE_THRESHOLD,
		   (unsigned)REMOTE_VOXEL_COLUMN_BYTES,
		   (unsigned)REMOTE_MESH_RESERVE_BYTES);

	/* Raise the carve: the backlog was never lost, so promotion resumes and
	 * finishes the ring. (On the console this is what a demotion or a 0x32
	 * unload does for free.) */
	adm_pump_emulated_carve(64u * (size_t)REMOTE_VOXEL_COLUMN_BYTES
								+ (size_t)REMOTE_MESH_RESERVE_BYTES,
							(int)announced * 8);
	assert(adm_count(REMOTE_CHUNK_RESIDENT) == announced
		   && "every deferred column installed once the carve allowed it");
	assert(s_r->retention_allocation_failures == 0 && s_r->mp_end == 0
		   && !s_r->stop_now);
	printf("PASS (m) resume: carve raised, all %zu in-radius columns resident, "
		   "nothing re-requested (protocol 14 cannot), 0 losses\n", announced);
	server_remote_test_set_carve_free_bytes(SIZE_MAX);
}

/* ---- (n) the moving-window bound at the shipped Wii default ----------- */
/* The real numbers the Wii default is sized from (betamc_adm_0913, tick 1). */
#define ADM_WII_CARVE_AT_JOIN (25570u * 1024u) /* mem2_carve_free_kb 25570 */
#define ADM_WII_BACKING_DEFAULT (6u * 1024u * 1024u) /* MEM2_MP_BACKING_BYTES */
#define ADM_WII_MESH_RESERVE (6u * 1024u * 1024u)	 /* the Wii reserve */
#define ADM_WALK_RADIUS 4							 /* the Wii default */
#define ADM_WALK_COLUMNS 20
static void test_moving_window_bound(void) {
	/* Static and moving maxima at R=4 (design 2.3). Promotion admits distance
	 * <= R, demotion only fires past R + 1, so a column can be resident out
	 * to distance R + 1: (2R+1)^2 = 81 standing still, (2R+3)^2 = 121 while
	 * moving. */
	const size_t statics = (2 * ADM_WALK_RADIUS + 1) * (2 * ADM_WALK_RADIUS + 1);
	const size_t moving = (2 * ADM_WALK_RADIUS + 3) * (2 * ADM_WALK_RADIUS + 3);
	assert(statics == 81 && moving == 121);
	/* THE BUDGET, asserted instead of asserted-in-prose: the moving maximum
	 * of voxels AND the mesh reserve both fit the carve the real console had
	 * free when its MP session started, after the backing reservation. This
	 * is why the Wii default is 4 -- at R=5 the same line is 169 columns =
	 * 13.8 MB of voxels and the reserve no longer fits, which is the
	 * ndisp=0 run. */
	const size_t carve_for_terrain
		= (size_t)ADM_WII_CARVE_AT_JOIN - (size_t)ADM_WII_BACKING_DEFAULT;
	assert(moving * (size_t)REMOTE_VOXEL_COLUMN_BYTES
			   + (size_t)ADM_WII_MESH_RESERVE
		   <= carve_for_terrain);
	assert((size_t)(2 * 5 + 3) * (2 * 5 + 3)
				   * (size_t)REMOTE_VOXEL_COLUMN_BYTES
			   + (size_t)ADM_WII_MESH_RESERVE
		   > carve_for_terrain);

	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_WALK_RADIUS);
	/* Announce every column the walk can reach: the path plus R + 1 on each
	 * side, 31 x 11 = 341 entries of REMOTE_CHUNK_ENTRY_CAPACITY (576). */
	size_t announced = 0;
	for(int cx = -ADM_WALK_RADIUS - 1; cx <= ADM_WALK_COLUMNS + ADM_WALK_RADIUS + 1;
		cx++)
		for(int cz = -ADM_WALK_RADIUS - 1; cz <= ADM_WALK_RADIUS + 1; cz++) {
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
			announced++;
		}
	assert(announced <= REMOTE_CHUNK_ENTRY_CAPACITY);
	/* The emulated carve is the REAL post-backing budget, so this walk is the
	 * console's own arithmetic and a spurious refusal would show up as a
	 * nonzero promote_deferred_memory below. */
	size_t high_water = adm_pump_emulated_carve(carve_for_terrain,
												(int)statics * 8);
	assert(adm_voxel_columns() == statics
		   && "standing still at R=4: exactly (2R+1)^2 resident");
	for(int step = 1; step <= ADM_WALK_COLUMNS; step++) {
		adm_anchor(step, 0);
		size_t peak = adm_pump_emulated_carve(carve_for_terrain, 90);
		if(peak > high_water)
			high_water = peak;
		assert(peak <= moving
			   && "the moving window never exceeds (2R+3)^2 columns");
		assert(s_r->demotion_failures == 0 && !s_r->stop_now);
	}
	assert(high_water <= moving);
	assert(s_r->promote_deferred_memory == 0
		   && "at R=4 the measured carve never has to defer a column");
	assert(s_r->retention_allocation_failures == 0 && s_r->backing_refusals == 0
		   && s_r->retention_evictions == 0
		   && adm_count(REMOTE_CHUNK_TOMBSTONE) == 0);
	assert(s_r->mp_end == 0 && s_r->forced_reconnects == 0);
	assert(s_r->demotions >= ADM_WALK_COLUMNS
		   && "and the columns left behind gave their voxels back");
	printf("PASS (n) moving window: R=%d, walked %d columns over %zu announced,"
		   " resident high-water %zu of the (2R+3)^2 = %zu bound (%zu static),"
		   " %u demotions, promote_deferred_memory=%u against the measured "
		   "%zu B post-backing carve\n",
		   ADM_WALK_RADIUS, ADM_WALK_COLUMNS, announced, high_water, moving,
		   statics, s_r->demotions, s_r->promote_deferred_memory,
		   carve_for_terrain);
	server_remote_test_set_carve_free_bytes(SIZE_MAX);
}

/* ====================================================================== */
/* F5a (mp-split-0912/26) -- the capacity latch, the reservation release,  */
/* the scratch accounting and the deferred-log cliff. Review findings H2,  */
/* H3, M8, M9, L10 of tmp/jobs/mp-split-0912/v1-hostile-review.md.        */
/* ====================================================================== */

/* The smallest reservation the clamp allows: these cases want to FILL one,
 * and 2 MiB fills ~10x faster than the Wii default. */
#define ADM_TIGHT_BACKING (2u * 1024u * 1024u)

/* Build a Packet51 HEADER on the wire, exactly as a server sends it: id,
 * x/y/z, (sx-1)/(sy-1)/(sz-1), then the compressed length. 18 bytes, and the
 * last of them is the one that makes the decoder allocate the body -- which
 * is the whole point of these two cases: the capacity answer has to come out
 * of the REAL decoder, not out of a fixture that allocates the body itself
 * (review H2: "the tests do not cover this path"). */
static size_t adm_map_header(uint8_t* out, int cx, int cz, size_t compressed) {
	size_t n = 0;
	int32_t x = cx * 16, z = cz * 16;
	out[n++] = MP_PACKET_MAP_CHUNK;
	for(int s = 3; s >= 0; s--)
		out[n++] = (uint8_t)(((uint32_t)x >> (s * 8)) & 0xFFu);
	out[n++] = 0; /* y hi */
	out[n++] = 0; /* y lo */
	for(int s = 3; s >= 0; s--)
		out[n++] = (uint8_t)(((uint32_t)z >> (s * 8)) & 0xFFu);
	out[n++] = 15;	/* sx - 1 */
	out[n++] = 127; /* sy - 1 */
	out[n++] = 15;	/* sz - 1 */
	for(int s = 3; s >= 0; s--)
		out[n++] = (uint8_t)(((uint32_t)compressed >> (s * 8)) & 0xFFu);
	return n;
}

/* Feed a header through the production decoder with the production reclaim
 * seam wired (remote_attach_decoder_reclaim -- the same call the read thread
 * makes). Returns the decoder's verdict on the last byte. */
static enum mp_decode_result adm_feed_header(struct mp_decoder* decoder,
											 struct mp_event* event, int cx,
											 int cz, size_t compressed) {
	uint8_t header[24];
	size_t n = adm_map_header(header, cx, cz, compressed);
	enum mp_decode_result result = MP_DECODE_MORE;
	for(size_t i = 0; i < n; i++)
		result = mp_decoder_feed(decoder, header[i], event);
	return result;
}

/* Fill the reservation with poorly-compressing columns until the ALLOCATOR
 * itself refuses (adm_retain_terrain returns 0 before it ever reaches the
 * retention, so no reclaim ladder runs on the way in). Returns how many
 * columns landed. The byte cap is lifted first: these cases want the
 * allocator to be the only gate, which is what production has once the cap
 * is the reservation capacity. */
static size_t adm_fill_until_allocator_refuses(void) {
	server_remote_test_set_retained_byte_cap(&adm_owner, SIZE_MAX);
	size_t admitted = 0;
	for(int cx = 0; cx < 21; cx++)
		for(int cz = 0; cz < 21; cz++) {
			if(!adm_retain_terrain(cx + 40, cz + 40, cx, cz, ADM_NOISE_20K))
				return admitted;
			admitted++;
		}
	assert(false && "441 x ~20 KB must not fit a 2 MiB reservation");
	return admitted;
}

/* ---- (p) the capacity failure comes out of the REAL decoder ----------- */
static void test_decoder_capacity_latch(void) {
	adm_fresh_session(ADM_TIGHT_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 0);
	size_t admitted = adm_fill_until_allocator_refuses();
	assert(admitted && s_r->mp_end == 0 && !s_r->stop_now);
	/* Nothing is reclaimable: no column has a queued body (rung 2) and no
	 * edit is parked (rung 1), so this is a true capacity wall. */
	assert(s_r->deferred_edits == 0);

	struct mp_decoder decoder;
	struct mp_event event;
	mp_decoder_init(&decoder);
	remote_attach_decoder_reclaim(s_r, &decoder);
	enum mp_decode_result result
		= adm_feed_header(&decoder, &event, 99, 99, ADM_NOISE_20K + 2000u);
	assert(result == MP_DECODE_ERROR && decoder.allocation_failed
		   && decoder.error_packet_id == MP_PACKET_MAP_CHUNK
		   && "the body allocation, not the header, is what failed");
	assert(decoder.alloc_wanted == ADM_NOISE_20K + 2000u);
	/* The hook already ran the ladder and, finding nothing to reclaim, ended
	 * the session -- which is what H2 says must happen BEFORE the decode
	 * error is diagnosed. */
	assert(s_r->mp_end == SERVER_REMOTE_END_CAPACITY && s_r->mp_end == 5u);
	assert(s_r->stop_now && s_r->backing_refusals == 1);

	/* And the read loop's own terminal agrees: capacity, never a protocol
	 * error -- and it does not double-count the refusal. */
	assert(remote_read_decode_error(s_r, &decoder, s_r->generation)
		   && "a refused Packet51 body is a CAPACITY end, not a bad packet");
	assert(s_r->backing_refusals == 1 && s_r->mp_end == 5u);
	assert(s_r->backing_latch_bugs == 0);
	assert(strstr(s_r->reason, "does not fit this console"));
	assert(!strstr(s_r->reason, "bad or unsupported"));
	assert(s_r->error_pending
		   && strstr(s_r->error_pending_msg, "does not fit this console")
		   && !strstr(s_r->error_pending_msg, "bad or unsupported")
		   && "the PUBLISHED text is the capacity reason (H2: the player used "
			  "to be told their server was broken)");
	assert(s_r->retention_evictions == 0
		   && adm_count(REMOTE_CHUNK_TOMBSTONE) == 0
		   && "a capacity failure still never destroys a column");
	char reason[sizeof(s_r->reason)];
	snprintf(reason, sizeof(reason), "%s", s_r->reason);
	mp_decoder_destroy(&decoder);
	printf("PASS (p) decoder capacity latch: %zu columns held a 2 MiB "
		   "reservation, the decoder's own %u B body was refused, reclaim "
		   "found nothing, mp_end=%u (5=capacity), refusals=%u, read loop "
		   "stopped, published \"%s\"\n",
		   admitted, (unsigned)decoder.alloc_wanted, (unsigned)s_r->mp_end,
		   s_r->backing_refusals, reason);
}

/* ---- (p2) a failed fold keeps its reason through decoder alloc failure */
static void test_fold_failed_decoder_alloc_preserves_reason(void) {
	adm_fresh_session(ADM_TIGHT_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 0);
	adm_retain_full(7, 0);
	adm_retain_partial(7, 0, 8, BLOCK_LOG);
	size_t i = adm_entry(7, 0);
	assert(i != REMOTE_CHUNK_ENTRY_CAPACITY);
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
		   && !s_r->chunks[i].installed && s_r->chunks[i].queued_count == 1);
	/* Invalid zlib header: the retained queued body must yield FOLD_FAILED
	 * when the decoder's reclaim hook tries to compact this column. */
	s_r->chunks[i].queued[0].compressed[0] = 0;
	for(int n = 0; n <= REMOTE_DEFERRED_EDIT_HIGH_WATER; n++)
		adm_edit(7 * 16 + n % 16, 20 + n / 256, (n / 16) % 16,
				 BLOCK_WOOL);
	assert(s_r->deferred_edits == REMOTE_DEFERRED_EDIT_HIGH_WATER + 1);
	assert(!s_r->stop_now && s_r->mp_end == 0);

	struct mp_decoder decoder;
	struct mp_event event;
	mp_decoder_init(&decoder);
	remote_attach_decoder_reclaim(s_r, &decoder);
	mp_codec_set_alloc_hook(fail_alloc);
	enum mp_decode_result result
		= adm_feed_header(&decoder, &event, 99, 99, 1024);
	mp_codec_set_alloc_hook(NULL);
	assert(result == MP_DECODE_ERROR && decoder.allocation_failed
		   && decoder.error_packet_id == MP_PACKET_MAP_CHUNK);
	assert(decoder.alloc_wanted == 1024);
	const char* reason = "remote Packet51 body does not decode (column 7,0)";
	assert(s_r->chain_rejections == 1 && s_r->stop_now);
	assert(s_r->mp_end == SERVER_REMOTE_END_PROTOCOL && s_r->mp_end == 6u);
	assert(strcmp(s_r->reason, reason) == 0
		   && "the reclaim ladder must preserve the fold's decode reason");
	assert(s_r->error_pending && strcmp(s_r->error_pending_msg, reason) == 0);

	/* The read loop unconditionally diagnoses the refused body as capacity;
	 * its terminal must keep the earlier fold failure and published text. */
	assert(remote_read_decode_error(s_r, &decoder, s_r->generation));
	assert(s_r->mp_end == SERVER_REMOTE_END_PROTOCOL && s_r->mp_end == 6u);
	assert(strcmp(s_r->reason, reason) == 0);
	assert(s_r->error_pending && strcmp(s_r->error_pending_msg, reason) == 0);
	assert(s_r->backing_refusals == 0 && s_r->backing_latch_bugs == 0);
	mp_decoder_destroy(&decoder);
	puts("PASS (p2) FOLD_FAILED then decoder allocation failure: mp_end=6 "
		 "and the fold's decode reason is preserved");
}

/* ---- (q) reclaim, THEN retry -- the decode continues ------------------ */
/* A partial body at an arbitrary y offset, filled with incompressible bytes
 * so it is the LARGEST block in the reservation. adm_retain_partial's
 * constant fill deflates to a few hundred bytes, which cannot be the
 * reclaim's subject in an arithmetic that has to be deterministic. */
static size_t adm_retain_noise_partial(int cx, int cz, int y, int sy) {
	size_t cells = 16u * (size_t)sy * 16u, bytes = cells * 5u / 2u;
	uint8_t* rawp = malloc(bytes);
	assert(rawp);
	uint32_t state = (uint32_t)(y * 2654435761u) ^ 0xC2B2AE35u;
	for(size_t i = 0; i < bytes; i++)
		rawp[i] = (uint8_t)(adm_rng(&state) & 0xFFu);
	uLongf n = 0;
	unsigned char* compressed = adm_body_from(rawp, bytes, &n);
	assert(compressed);
	free(rawp);
	struct mp_event e = {.type = MP_EVENT_MAP_CHUNK};
	e.payload.map_chunk = (struct mp_map_chunk) {
		.x = cx * 16, .y = (int16_t)y, .z = cz * 16, .sx = 16,
		.sy = (uint16_t)sy, .sz = 16, .compressed = compressed,
		.compressed_length = n, .inflated_length = bytes};
	assert(remote_retain_map_locked(s_r, &e));
	return (size_t)n;
}

static void test_reclaim_then_retry(void) {
	adm_fresh_session(ADM_TIGHT_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 0);
	server_remote_test_set_retained_byte_cap(&adm_owner, SIZE_MAX);
	/* Rung 2 of the ladder needs a head a QUEUED body already covers in
	 * full. Volumes are chosen so no arrival can coalesce the chain away:
	 * head A = y0..95, then B = y64..127 (covers nothing) is queued, then
	 * C = y0..95 covers A but not B, so it too is queued -- leaving A
	 * superseded and reclaimable. */
	size_t head = adm_retain_noise_partial(0, 0, 0, 96);
	(void)adm_retain_noise_partial(0, 0, 64, 64);
	(void)adm_retain_noise_partial(0, 0, 0, 96);
	size_t idx = adm_entry(0, 0);
	assert(idx != REMOTE_CHUNK_ENTRY_CAPACITY);
	assert(s_r->chunks[idx].queued_count == 2
		   && s_r->chunks[idx].wire.compressed_length == head
		   && "the superseded head is still the head");
	size_t admitted = adm_fill_until_allocator_refuses();
	assert(admitted);
	/* The request: bigger than any free run the fill can have left behind
	 * (every filler body is ~20 KB, so the tail remainder is smaller than
	 * that), and smaller than the head the reclaim is about to free. Only
	 * the reclaim can satisfy it. */
	size_t want = 30u * 1024u;
	assert(head > want && want > ADM_NOISE_20K + 2048u);
	void* direct = mp_backing_alloc(want);
	assert(!direct && "the reservation really cannot serve this yet");

	struct mp_decoder decoder;
	struct mp_event event;
	mp_decoder_init(&decoder);
	remote_attach_decoder_reclaim(s_r, &decoder);
	enum mp_decode_result result
		= adm_feed_header(&decoder, &event, 99, 99, want);
	assert(result == MP_DECODE_MORE && !decoder.allocation_failed
		   && "one lossless reclaim, then the RETRY -- not a discarded packet");
	assert(decoder.chunk_body && mp_backing_owns(decoder.chunk_body)
		   && decoder.chunk_need == want);
	/* The session is untouched: this is the path H2 says used to die with
	 * mp_end=0 and a "bad packet" diagnosis even when the reclaim WORKED. */
	assert(s_r->mp_end == 0 && !s_r->stop_now && s_r->backing_refusals == 0
		   && s_r->backing_latch_bugs == 0);
	assert(s_r->retention_evictions == 0
		   && adm_count(REMOTE_CHUNK_TOMBSTONE) == 0);
	/* Rung 2 did it: the head is gone, the chain shifted up, both queued
	 * bodies are still here (nothing was lost to make room). */
	assert(s_r->chunks[idx].queued_count == 1
		   && s_r->chunks[idx].wire.compressed
		   && s_r->chunks[idx].wire.compressed_length != head);
	mp_decoder_destroy(&decoder);
	printf("PASS (q) reclaim then retry: %zu columns + a %zu B superseded "
		   "head filled 2 MiB, the decoder's %zu B body was refused, the "
		   "ladder freed the head losslessly and the RETRY succeeded -- "
		   "mp_end=%u, refusals=%u, chain intact\n",
		   admitted, head, want, (unsigned)s_r->mp_end,
		   s_r->backing_refusals);
}

/* ---- (r) the reservation goes back to the carve at session end -------- */
/* The instrument is the carve observer itself: mp_backing_region_acquire and
 * _release note CARVE_ALLOC/CARVE_TAG(CARVE_MP_BACKING) and CARVE_FREE on
 * both platforms, so what a PC test tallies here is exactly what the Wii
 * carve sees. mem2_carve_free_bytes() cannot serve: on PC it is SIZE_MAX. */
static struct {
	uintptr_t block[8];
	size_t bytes[8];
	size_t live;
	size_t outstanding;
	size_t peak;
	unsigned releases;
	uintptr_t pending;
	size_t pending_bytes;
} adm_carve;

static void adm_carve_cb(enum carve_action action, uintptr_t p, uintptr_t q,
						 size_t n, size_t aux) {
	(void)q;
	if(action == CARVE_ALLOC) {
		adm_carve.pending = p;
		adm_carve.pending_bytes = n;
	} else if(action == CARVE_TAG && aux == CARVE_MP_BACKING
			  && p == adm_carve.pending) {
		if(adm_carve.live < 8) {
			adm_carve.block[adm_carve.live] = p;
			adm_carve.bytes[adm_carve.live] = adm_carve.pending_bytes;
			adm_carve.live++;
		}
		adm_carve.outstanding += adm_carve.pending_bytes;
		if(adm_carve.outstanding > adm_carve.peak)
			adm_carve.peak = adm_carve.outstanding;
	} else if(action == CARVE_FREE) {
		for(size_t i = 0; i < adm_carve.live; i++)
			if(adm_carve.block[i] == p) {
				adm_carve.outstanding -= adm_carve.bytes[i];
				adm_carve.block[i] = adm_carve.block[adm_carve.live - 1];
				adm_carve.bytes[i] = adm_carve.bytes[adm_carve.live - 1];
				adm_carve.live--;
				adm_carve.releases++;
				break;
			}
	}
}

static void test_reservation_returns_to_carve(void) {
	adm_fresh_session(ADM_TIGHT_BACKING);
	carve_callback saved = mem2_carve_observe;
	memset(&adm_carve, 0, sizeof(adm_carve));
	mem2_carve_observe = adm_carve_cb;
	/* BEFORE: adm_fresh_session's mp_backing_reset left no region carved. */
	size_t before = adm_carve.outstanding;
	assert(before == 0);

	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 1);
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	adm_pump_n(40); /* promotions claim the inflate scratch slot */
	adm_edit(8, 70, 8, BLOCK_PLANKS);
	adm_pump_n(40);
	size_t during = adm_carve.outstanding;
	assert(during >= ADM_TIGHT_BACKING
		   && "the body reservation is carved once the burst starts");
	size_t scratch_in_use = 0, scratch_peak = 0, scratch_cap = 0;
	mp_backing_scratch_stats(&scratch_in_use, &scratch_peak, &scratch_cap);
	assert(scratch_peak > 0 && "and the scratch side budget with it");
	assert(during == ADM_TIGHT_BACKING + MEM2_MP_SCRATCH_BYTES
		   && "exactly the two CARVE_MP_BACKING regions, nothing else");

	/* THE CLAIM (M9): the session end hands both regions back. */
	server_remote_destroy(&adm_owner);
	size_t after = adm_carve.outstanding;
	unsigned releases = adm_carve.releases;
	mem2_carve_observe = saved;
	assert(after == before && after == 0
		   && "MP -> title must leave the carve exactly as it found it");
	assert(releases >= 2 && "both the reservation and its scratch came back");
	mp_backing_scratch_stats(&scratch_in_use, &scratch_peak, &scratch_cap);
	assert(scratch_in_use == 0);
	printf("PASS (r) carve returned: CARVE_MP_BACKING outstanding %zu B "
		   "before, %zu B during the burst (%u B reservation + %u B scratch), "
		   "%zu B after connect/burst/cancel -- %u regions released\n",
		   before, during, ADM_TIGHT_BACKING, MEM2_MP_SCRATCH_BYTES, after,
		   releases);
	adm_fresh_session(ADM_WII_BACKING);
}

/* ---- (s) a BROAD edit burst no longer hits the 1024 cliff ------------- */
#define ADM_M8_COLUMNS 300
/* REL-MP-JOINSPEED: enough per column that the burst still overruns the
 * (now 16,384-entry) log: 300 x 56 = 16,800. */
#define ADM_M8_PER_COLUMN (REMOTE_DEFERRED_EDIT_CAPACITY / ADM_M8_COLUMNS + 2)
#define ADM_M8_EDITS (ADM_M8_COLUMNS * ADM_M8_PER_COLUMN)
_Static_assert(ADM_M8_EDITS > REMOTE_DEFERRED_EDIT_CAPACITY,
			   "(s) must overrun the log with no pump in between");
/* Column n of the burst, laid out as a 20x15 grid anchored at the origin so
 * a 5x5 window around (2, 2) is inside it and can be promoted and
 * byte-verified. */
static void adm_m8_column(int n, int* cx, int* cz) {
	*cx = n % 20;
	*cz = n / 20;
}
static void adm_m8_edit_cell(int n, int k, int* x, int* y, int* z) {
	int cx, cz;
	adm_m8_column(n, &cx, &cz);
	*x = cx * 16 + k % 16;
	*z = cz * 16 + (k / 16) % 16;
	*y = 70 + k / 256;
}

static void test_broad_edit_burst_no_kill(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(2, 2);
	/* Radius 0: every column stays BACKED, which is the state that parks
	 * edits in the log instead of applying them live. */
	server_remote_set_admit_radius(&adm_owner, 0);
	for(int n = 0; n < ADM_M8_COLUMNS; n++) {
		int cx, cz;
		adm_m8_column(n, &cx, &cz);
		/* adm_retain_terrain, not adm_retain_full: the pseudo-random fixture
		 * is ~30 KB/column (measured, see main's comment) and 300 of those
		 * would not fit any reservation the clamp allows. Ordinary terrain is
		 * ~1.7 KB and is what a real server streams. */
		assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	}
	assert(s_r->retained_entries == ADM_M8_COLUMNS);
	/* ADM_M8_EDITS spread over all 300, with NO pump in between -- the case
	 * the pump's own high-water compaction cannot answer (review M8). */
	for(int k = 0; k < ADM_M8_PER_COLUMN; k++)
		for(int n = 0; n < ADM_M8_COLUMNS; n++) {
			int x, y, z;
			adm_m8_edit_cell(n, k, &x, &y, &z);
			adm_edit(x, y, z, BLOCK_PLANKS);
			assert(!s_r->stop_now
				   && "the log-capacity cliff is gone: no kill mid-burst");
		}
	assert(s_r->deferred_edit_log_exhausted == 0 && s_r->mp_end == 0
		   && !s_r->stop_now);
	assert(s_r->compactions > 0 && s_r->deferred_log_rescues > 0
		   && "the exhaustion path compacted instead of killing");
	assert(s_r->deferred_edits <= REMOTE_DEFERRED_EDIT_CAPACITY);
	/* NOTHING WAS LOST: every one of them is either still parked or inside
	 * a compacted body, and none was dropped. */
	assert(s_r->deferred_edits + s_r->deferred_edits_applied == ADM_M8_EDITS);
	assert(s_r->deferred_edits_dropped == 0);
	uint32_t rescues = s_r->deferred_log_rescues,
			 compactions = s_r->compactions, parked = s_r->deferred_edits,
			 folded = s_r->deferred_edits_applied;

	/* And they RECONSTRUCT. Promote the 5x5 window around the anchor and
	 * byte-verify its edits through the client world: a compacted
	 * column's edits have to come back out of the body they were folded
	 * into, and a parked one's out of the drain. */
	server_remote_set_admit_radius(&adm_owner, 2);
	adm_pump_n(ADM_M8_COLUMNS * 6);
	unsigned checked = 0;
	for(int n = 0; n < ADM_M8_COLUMNS; n++) {
		int cx, cz;
		adm_m8_column(n, &cx, &cz);
		if(cx < 0 || cx > 4 || cz < 0 || cz > 4)
			continue;
		if(!adm_installed(cx, cz))
			continue;
		for(int k = 0; k < ADM_M8_PER_COLUMN; k++) {
			int x, y, z;
			adm_m8_edit_cell(n, k, &x, &y, &z);
			struct block_data b = world_get_block(&gstate.world, x, y, z);
			assert(b.type == BLOCK_PLANKS
				   && "a folded/parked edit must come back byte-exact");
			checked++;
		}
	}
	assert(checked >= 25 * ADM_M8_PER_COLUMN
		   && "the whole 5x5 window promoted and verified");
	assert(s_r->mp_end == 0 && !s_r->stop_now
		   && s_r->deferred_edit_log_exhausted == 0);
	printf("PASS (s) broad edit burst: %d edits over %d backed columns, log "
		   "cap %d, high-water %d -- 0 kills, %u compactions (%u of them "
		   "exhaustion rescues), %u edits folded into bodies + %u still "
		   "parked = %d, 0 dropped, %u edits re-read byte-exact from the "
		   "promoted 5x5 window\n",
		   ADM_M8_EDITS, ADM_M8_COLUMNS, REMOTE_DEFERRED_EDIT_CAPACITY,
		   REMOTE_DEFERRED_EDIT_HIGH_WATER, compactions, rescues, folded,
		   parked, ADM_M8_EDITS, checked);
}

/* ---- (s2) exhaustion finds an eligible victim after 32 ineligible cols -- */
#define ADM_S2_INELIGIBLE_COLUMNS 32
static void test_compaction_scans_past_ineligible_prefix(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 0);

	/* Fill the log in insertion order with CAPACITY - 1 edits belonging to
	 * 32 announced columns that have no body yet. They are valid deferred
	 * edits; the rescue's base rule ranks every full head before them. */
	enum { S2_PER = REMOTE_DEFERRED_EDIT_CAPACITY / ADM_S2_INELIGIBLE_COLUMNS };
	unsigned inserted = 0;
	for(int n = 0; n < ADM_S2_INELIGIBLE_COLUMNS; n++) {
		int cx = 100 + n;
		assert(remote_pre_chunk_locked(s_r, cx, 0, true));
		assert(adm_state(cx, 0) == REMOTE_CHUNK_ANNOUNCED);
		unsigned count = n == ADM_S2_INELIGIBLE_COLUMNS - 1 ? S2_PER - 1u : S2_PER;
		for(unsigned k = 0; k < count; k++) {
			adm_edit(cx * 16 + (int)(k & 15u), 70 + (int)(k >> 8),
					 (int)((k >> 4) & 15u), BLOCK_PLANKS);
			inserted++;
		}
	}
	assert(inserted == REMOTE_DEFERRED_EDIT_CAPACITY - 1u);

	/* The last slot belongs to a later, fully backed column. On the next
	 * distinct edit the exhaustion rescue must scan past the ineligible prefix,
	 * fold this column's edits into its body, and keep the session alive. */
	const int eligible_cx = 200;
	assert(adm_retain_terrain(eligible_cx, 0, eligible_cx, 0, 0));
	assert(adm_state(eligible_cx, 0) == REMOTE_CHUNK_RETAINED
		   && !adm_installed(eligible_cx, 0));
	adm_edit(eligible_cx * 16 + 1, 70, 1, BLOCK_PLANKS);
	assert(s_r->deferred_edits == REMOTE_DEFERRED_EDIT_CAPACITY);
	adm_edit(eligible_cx * 16 + 2, 70, 1, BLOCK_BRICKS);

	assert(!s_r->stop_now && s_r->mp_end == 0
		   && s_r->deferred_edit_log_exhausted == 0
		   && "an eligible victim after 32 ineligible columns must prevent a false terminal");
	assert(s_r->compactions == 1 && s_r->deferred_log_rescues == 1);
	assert(s_r->deferred_edits == REMOTE_DEFERRED_EDIT_CAPACITY
		   && "the existing eligible edit folded, then the triggering edit parked");
	assert(s_r->deferred_edits_dropped == 0);
	adm_anchor(eligible_cx, 0);
	adm_pump_n(200);
	assert(adm_state(eligible_cx, 0) == REMOTE_CHUNK_RESIDENT);
	assert(world_get_block(&gstate.world, eligible_cx * 16 + 1, 70, 1).type
		   == BLOCK_PLANKS);
	assert(world_get_block(&gstate.world, eligible_cx * 16 + 2, 70, 1).type
		   == BLOCK_BRICKS
		   && "folded and newly parked edits both reconstruct after promotion");
	puts("PASS (s2) full deferred log scans past 32 ineligible columns and "
		 "losslessly compacts and reconstructs a later eligible victim");
}

/* ---- (s3) REL-MP-FIX2: vanilla floods announced columns pre-body ------- */
/* tmp/jobs/triage-1019/kick-diag.md: vanilla sends 0x32 and subscribes the
 * player at once (PlayerInstance.java:62-69), forwards 0x35/0x34/partial 0x33
 * before the full snapshot (:125-159) and sends one full column per update
 * (EntityPlayerMP.java:214-222). No column has a full head, so nothing can
 * compact and the 1,025th distinct edit ends the session. Beta's client
 * applies them to an empty pre-chunk (ChunkProviderClient.java:40). */
#define ADM_V_COLS 40		  /* announced, never full-bodied during the flood */
/* REL-MP-JOINSPEED: 20 odd columns x 600 + 20 even x (16 + 256) = 17,440
 * distinct cells > the 16,384-entry log (was 27 for the 1,024 one). */
#define ADM_V_PER_COL 600
#define ADM_V_PARTIAL_AT 8	  /* even columns get a partial 0x33 at this round */
#define ADM_V_INSIDE_FROM 16  /* later edits land INSIDE that partial (y 3) */
#define ADM_V_MULTI 3		  /* every third round is one 0x34 for 2 columns */
static void adm_v_cell(int c, int n, int* x, int* y, int* z) {
	adm_j_cell(100 + c, 0, n, 20, x, y, z);
	if(c % 2 == 0 && n >= ADM_V_INSIDE_FROM)
		*y = 3;
}
static void adm_v_ref_flood(int c) {
	memset(adm_ref, 0, ADM_CELLS + ADM_NIB); /* pre-chunk: air, meta 0 */
	if(c % 2 == 0)
		adm_ref_partial(0, 8, BLOCK_LOG, 2);
	for(int n = 0; n < ADM_V_PER_COL; n++) {
		int x, y, z;
		adm_v_cell(c, n, &x, &y, &z);
		adm_ref_set(x, y, z, BLOCK_WOOL, 1 + n % 15);
	}
}
static int test_vanilla_prebody_flood(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 0);
	for(int c = 0; c < ADM_V_COLS; c++) {
		assert(remote_pre_chunk_locked(s_r, 100 + c, 0, true));
		assert(adm_state(100 + c, 0) == REMOTE_CHUNK_ANNOUNCED);
	}
	for(int n = 0; n < ADM_V_PER_COL; n++)
		for(int c = 0; c < ADM_V_COLS; c++) {
			if(n == ADM_V_PARTIAL_AT && c % 2 == 0)
				adm_retain_partial_meta(100 + c, 0, 0, 8, BLOCK_LOG, 2);
			/* 0x35 = one packet per edit; 0x34 = one packet stamp shared by
			 * the edits of an odd column and its even neighbour. */
			if(n % ADM_V_MULTI != 0 || c % 2 == 0) {
				s_r->pump_packet_seq = ++s_r->arrival_sequence;
				s_r->pump_packet_sub = 0;
			}
			int x, y, z;
			adm_v_cell(c, n, &x, &y, &z);
			remote_apply_block_change(s_r, x, (uint8_t)y, z, BLOCK_WOOL,
									  (uint8_t)(1 + n % 15));
			if(adm_rel_mp_terminal("(s3) vanilla pre-body flood"))
				return 1;
		}
	assert(s_r->deferred_edits_dropped == 0
		   && s_r->deferred_edit_log_exhausted == 0 && "no drop, no stop");
	/* BEFORE any full snapshot: a partial-only column inside the radius
	 * installs today; it must read pre-chunk base + partial + every edit in
	 * wire order. Sections 0-1 (y 0..31) hold the partial and every edit;
	 * higher ones are never allocated here (world.c:800 reads stone). */
	adm_v_ref_flood(0);
	adm_anchor(100, 0);
	adm_pump_n(200);
	assert(adm_state(100, 0) == REMOTE_CHUNK_RESIDENT && adm_installed(100, 0));
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y < 32; y++) {
				size_t idx = ((size_t)x * 16u + (size_t)z) * 128u + (size_t)y;
				struct block_data b
					= world_get_block(&gstate.world, 1600 + x, y, z);
				assert(b.type == adm_ref[idx]
					   && b.metadata == ((adm_ref[ADM_CELLS + (idx >> 1)]
										  >> ((y & 1) * 4)) & 15)
					   && "pre-full column: id + metadata, every cell");
			}
	/* The full snapshots, one per update, then newer edits on top. */
	adm_anchor(0, 0);
	for(int c = 0; c < ADM_V_COLS; c++) {
		assert(adm_retain_terrain(100 + c, 0, 100 + c, 0, 0));
		adm_edit_meta((100 + c) * 16 + 5, 100, 7, BLOCK_WOOL, 1 + c % 15);
		if(adm_rel_mp_terminal("(s3) after full snapshots"))
			return 1;
	}
	/* AFTER: the snapshot overwrites every earlier cell; later edits stand. */
	for(int c = 0; c < 4; c++) {
		adm_fill_terrain(100 + c, 0, adm_ref, 0);
		adm_ref_set((100 + c) * 16 + 5, 100, 7, BLOCK_WOOL, 1 + c % 15);
		adm_promote_matches_ref(100 + c, 0);
	}
	assert(!s_r->stop_now && s_r->mp_end == 0 && s_r->deferred_edits_dropped == 0);
	printf("PASS (s3) vanilla pre-body flood: %d distinct edits + %d partials "
		   "on %d announced columns survive; byte-exact before and after the "
		   "full snapshot\n", ADM_V_COLS * ADM_V_PER_COL, ADM_V_COLS / 2,
		   ADM_V_COLS);
	return 0;
}

/* ---- (s4) REL-MP-FIX2a: pre-chunk base reconstruction + provenance ----- */
/* A never-installed column with partial bodies only folds onto Beta's empty
 * pre-chunk (air, meta/block light 0, sky light 15; ChunkProviderClient.java
 * :40), stamped at the load so older edits outside every region survive. */
static bool adm_parked(int x, int y, int z) {
	for(size_t i = 0; s_r->deferred_edit_log && i < REMOTE_DEFERRED_EDIT_CAPACITY;
		i++) {
		const struct remote_deferred_edit* e = &s_r->deferred_edit_log[i];
		if(remote_deferred_edit_used(e) && remote_deferred_edit_x(s_r, e) == x
		   && remote_deferred_edit_y(e) == y && remote_deferred_edit_z(s_r, e) == z)
			return true;
	}
	return false;
}
static void adm_ref_region(int y0, int sy, int id, int meta) {
	adm_ref_partial(y0, sy, id, meta);
	for(size_t c = 0; c < 256u; c++) /* the partial bodies carry light 0 */
		memset(adm_ref + ADM_CELLS + 2u * ADM_NIB + c * 64u + y0 / 2, 0, sy / 2);
}
static void test_fix2a_synthetic_base(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	const int cx = 200, bx = 3200;
	assert(remote_pre_chunk_locked(s_r, cx, 0, true));
	adm_edit_meta(bx + 1, 50, 1, BLOCK_WOOL, 5); /* older, outside: survives */
	adm_edit_meta(bx + 2, 3, 2, BLOCK_WOOL, 6);	 /* older, inside P1: gone */
	adm_retain_partial_meta(cx, 0, 0, 8, BLOCK_LOG, 2);	/* P1 = head */
	adm_edit_meta(bx + 3, 4, 3, BLOCK_WOOL, 7);	 /* newer, inside P1: stays */
	adm_retain_partial_meta(cx, 0, 16, 8, BLOCK_STONE, 0); /* P2 queued */
	struct remote_chunk* ch = &s_r->chunks[adm_entry(cx, 0)];
	assert(ch->queued_count == 1 && !ch->synthetic_base);
	assert(server_remote_test_fold_column(&adm_owner, cx, 0));
	assert(ch->synthetic_base && ch->queued_count == 0
		   && ch->state == REMOTE_CHUNK_RETAINED
		   && ch->arrival == ch->residency_seq && "stamped at the load");
	assert(adm_parked(bx + 1, 50, 1) && adm_parked(bx + 3, 4, 3)
		   && !adm_parked(bx + 2, 3, 2) && s_r->deferred_edits == 2);
	memset(adm_ref, 0, ADM_CELLS + 2u * ADM_NIB);
	memset(adm_ref + ADM_CELLS + 2u * ADM_NIB, 0xFF, ADM_NIB);
	adm_ref_region(0, 8, BLOCK_LOG, 2);
	adm_ref_region(16, 8, BLOCK_STONE, 0);
	assert(adm_export(cx, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0
		   && "air base + regions, all four planes");
	/* Provenance survives a second fold through install_head. */
	adm_retain_partial_meta(cx, 0, 32, 8, BLOCK_DIRT, 1);
	assert(ch->synthetic_base && ch->queued_count == 1 && "append keeps it");
	assert(server_remote_test_fold_column(&adm_owner, cx, 0));
	assert(ch->synthetic_base && ch->arrival == ch->residency_seq);
	adm_ref_region(32, 8, BLOCK_DIRT, 1);
	assert(adm_export(cx, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0);
	/* REL-MP-FIX2b: it installs like Beta's prepared chunk -- untouched cells
	 * air, all four planes, parked edits drained on top -- but finishes no
	 * loading; a demotion hands the voxels' provenance back to the head. */
	adm_anchor(cx, 0);
	adm_pump_n(50);
	assert(adm_installed(cx, 0) && ch->voxels_synthetic
		   && !s_r->load_completion_pending && s_r->deferred_edits == 0);
	adm_ref_set(bx + 1, 50, 1, BLOCK_WOOL, 5);
	adm_ref_set(bx + 3, 4, 3, BLOCK_WOOL, 7);
	/* The drained edit is a live CRPC_SET_BLOCK: its cell is written with
	 * light 0 and relit later (the unchanged drain path). */
	adm_ref[ADM_CELLS + 2u * ADM_NIB + ((17u * 128u + 50u) >> 1)] &= 0xF0;
	assert(adm_export(cx, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0);
	assert(world_get_block(&gstate.world, bx + 5, 100, 5).type == BLOCK_AIR);
	adm_anchor(0, 0);
	adm_pump_n(20);
	assert(!adm_installed(cx, 0) && ch->synthetic_base && !ch->voxels_synthetic);
	/* The real snapshot replaces it and overwrites every earlier cell; a
	 * newer edit stands on top, and only now does loading complete. */
	assert(adm_retain_terrain(cx, 0, cx, 0, 0));
	assert(!ch->synthetic_base && s_r->deferred_edits == 0);
	adm_edit_meta(bx + 9, 100, 9, BLOCK_WOOL, 4);
	adm_fill_terrain(cx, 0, adm_ref, 0);
	adm_ref_set(bx + 9, 100, 9, BLOCK_WOOL, 4);
	adm_promote_matches_ref(cx, 0);
	assert(!ch->voxels_synthetic && s_r->load_completion_pending);
	/* drop_body: an unload takes the synthetic head and its provenance. */
	adm_anchor(0, 0);
	assert(remote_pre_chunk_locked(s_r, cx + 1, 0, true));
	adm_retain_partial_meta(cx + 1, 0, 0, 8, BLOCK_LOG, 2);
	assert(server_remote_test_fold_column(&adm_owner, cx + 1, 0));
	ch = &s_r->chunks[adm_entry(cx + 1, 0)];
	assert(ch->synthetic_base);
	assert(remote_pre_chunk_locked(s_r, cx + 1, 0, false));
	assert(!ch->synthetic_base && !ch->wire.compressed);
	assert(!s_r->stop_now && s_r->mp_end == 0 && s_r->deferred_edits_dropped == 0);
	printf("PASS (s4) FIX2a pre-chunk base: partial-only column folds onto air "
		   "(sky 15) once, stamped at the load; older outside edit kept, older "
		   "inside superseded; installs as air without completing loading, "
		   "demotion keeps provenance; real snapshot overwrites + completes\n");
}

/* ---- (s5) REL-MP-FIX2b: uncompacted partial-only install + lifecycle --- */
static void adm_ref_pre_chunk(void) {
	memset(adm_ref, 0, ADM_CELLS + 2u * ADM_NIB);
	memset(adm_ref + ADM_CELLS + 2u * ADM_NIB, 0xFF, ADM_NIB);
}
/* A live-drained edit (CRPC_SET_BLOCK) writes its own cell with light 0. */
static void adm_ref_sky0(int x, int y, int z) {
	size_t idx = ((size_t)(x & 15) * 16u + (size_t)(z & 15)) * 128u + (size_t)y;
	adm_ref[ADM_CELLS + 2u * ADM_NIB + (idx >> 1)]
		&= (uint8_t)((y & 1) ? 0x0Fu : 0xF0u);
}
static void adm_edits_at(int cx, int count) {
	for(int n = 0; n < count; n++) {
		int x, y, z;
		adm_j_cell(cx, 0, n, 20, &x, &y, &z);
		adm_edit_meta(x, y, z, BLOCK_WOOL, 1 + n % 15);
	}
}
static void test_fix2b_partial_only_install(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	const int cx = 300, bx = 4800;
	assert(remote_pre_chunk_locked(s_r, cx, 0, true));
	adm_edit_meta(bx + 1, 60, 1, BLOCK_WOOL, 3);		/* older, outside */
	adm_retain_partial_meta(cx, 0, 8, 8, BLOCK_LOG, 2); /* the only body */
	adm_edit_meta(bx + 2, 10, 2, BLOCK_WOOL, 4);		/* newer, inside */
	struct remote_chunk* ch = &s_r->chunks[adm_entry(cx, 0)];
	adm_anchor(cx, 0);
	adm_pump_n(50);
	assert(s_r->compactions == 0 && s_r->fold.folds == 0
		   && ch->state == REMOTE_CHUNK_RESIDENT && ch->voxels_synthetic
		   && !s_r->load_completion_pending && "the partial itself, expanded");
	adm_ref_pre_chunk();
	adm_ref_region(8, 8, BLOCK_LOG, 2);
	adm_ref_set(bx + 1, 60, 1, BLOCK_WOOL, 3);
	adm_ref_sky0(bx + 1, 60, 1);
	adm_ref_set(bx + 2, 10, 2, BLOCK_WOOL, 4);
	assert(adm_export(cx, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0
		   && "all four planes: pre-chunk + region + edits in wire order");
	struct block_data low = world_get_block(&gstate.world, bx + 5, 0, 5);
	struct block_data high = world_get_block(&gstate.world, bx + 5, 100, 5);
	assert(low.type == BLOCK_AIR && high.type == BLOCK_AIR
		   && high.sky_light == 15 && !blocks[high.type]
		   && "untouched cells: air, no collision, not the stone sentinel");
	assert(world_get_block(&gstate.world, bx + 16, 100, 0).type == BLOCK_STONE
		   && "an absent column still reads the not-loaded sentinel");
	/* The genuine snapshot overwrites every cell; a newer edit stands. */
	assert(adm_retain_terrain(cx, 0, cx, 0, 0));
	adm_edit_meta(bx + 9, 100, 9, BLOCK_WOOL, 6);
	adm_fill_terrain(cx, 0, adm_ref, 0);
	adm_ref_set(bx + 9, 100, 9, BLOCK_WOOL, 6);
	adm_promote_matches_ref(cx, 0);
	assert(!ch->voxels_synthetic && s_r->load_completion_pending);
	/* Unload/reload: 0x32 ends the residency (voxels, provenance, entry);
	 * an edit between unload and reload is dropped; the reload starts from
	 * a fresh pre-chunk with nothing of the old residency. */
	const int ux = cx + 2;
	assert(remote_pre_chunk_locked(s_r, ux, 0, true));
	adm_retain_partial_meta(ux, 0, 0, 8, BLOCK_LOG, 2);
	adm_anchor(ux, 0);
	adm_pump_n(50);
	assert(adm_installed(ux, 0) && s_r->chunks[adm_entry(ux, 0)].voxels_synthetic);
	assert(remote_pre_chunk_locked(s_r, ux, 0, false));
	adm_edit_meta(ux * 16 + 4, 70, 4, BLOCK_WOOL, 1);
	adm_pump_n(2);
	assert(!adm_installed(ux, 0) && adm_entry(ux, 0) == REMOTE_CHUNK_ENTRY_CAPACITY
		   && s_r->deferred_edits_dropped == 1);
	assert(remote_pre_chunk_locked(s_r, ux, 0, true));
	adm_retain_partial_meta(ux, 0, 16, 8, BLOCK_STONE, 0);
	adm_pump_n(50);
	assert(adm_installed(ux, 0) && s_r->chunks[adm_entry(ux, 0)].voxels_synthetic);
	adm_ref_pre_chunk();
	adm_ref_region(16, 8, BLOCK_STONE, 0);
	assert(adm_export(ux, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0);
	/* Dimension change with a synthetic head and synthetic voxels live: the
	 * table clear takes both flags, every body and the log. */
	assert(remote_pre_chunk_locked(s_r, ux + 1, 0, true));
	adm_retain_partial_meta(ux + 1, 0, 0, 8, BLOCK_LOG, 2);
	assert(server_remote_test_fold_column(&adm_owner, ux + 1, 0));
	adm_edit_meta((ux + 1) * 16, 70, 0, BLOCK_WOOL, 1);
	assert(s_r->chunks[adm_entry(ux + 1, 0)].synthetic_base && s_r->deferred_edits);
	s_r->world_epoch++;
	remote_chunks_clear_locked(s_r);
	s_r->applied_epoch = s_r->world_epoch;
	remote_admission_centre_clear(s_r);
	clin_force_world_reset_login(WORLD_DIM_NETHER, 42);
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		assert(s_r->chunks[i].state == REMOTE_CHUNK_UNUSED
			   && !s_r->chunks[i].synthetic_base
			   && !s_r->chunks[i].voxels_synthetic);
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	assert(in_use == 0 && s_r->deferred_edits == 0 && s_r->resident_cols == 0);
	assert(!s_r->stop_now && s_r->mp_end == 0);
	puts("PASS (s5) FIX2b partial-only install: uncompacted partial installs "
		 "whole on the pre-chunk (4 planes exact, untouched air + sky 15, no "
		 "load completion); real snapshot overwrites + completes; unload/"
		 "reload and dimension reset clear provenance and bodies");
}

/* ---- (s6) REL-MP-FIX2b: bodyless compaction under refusal, ring order -- */
static void test_fix2b_bodyless_refusal_and_ring(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	const int cx = 400, n_edits = REMOTE_DEFERRED_EDIT_HIGH_WATER + 1;
	assert(remote_pre_chunk_locked(s_r, cx, 0, true));
	adm_edits_at(cx, n_edits);
	struct remote_chunk* ch = &s_r->chunks[adm_entry(cx, 0)];
	/* The backing refuses the compacted body: nothing changes, nothing is
	 * lost, the session lives; once room returns it compacts. */
	void* hog[64];
	size_t hogs = 0;
	for(size_t len = ADM_WII_BACKING; len && hogs < 64;) {
		void* p = mp_backing_alloc(len);
		if(p)
			hog[hogs++] = p;
		else
			len /= 2;
	}
	adm_pump();
	assert(s_r->compactions == 0 && s_r->compaction_failures > 0
		   && s_r->deferred_edits == (uint32_t)n_edits
		   && ch->state == REMOTE_CHUNK_ANNOUNCED && !ch->wire.compressed
		   && !ch->synthetic_base && !s_r->stop_now && s_r->mp_end == 0);
	while(hogs)
		mp_backing_free(hog[--hogs]);
	adm_pump();
	assert(s_r->compactions == 1 && s_r->deferred_edits == 0
		   && ch->state == REMOTE_CHUNK_RETAINED && ch->synthetic_base
		   && ch->arrival > ch->residency_seq);
	adm_ref_pre_chunk(); /* folded edits keep the planes' light */
	for(int n = 0; n < n_edits; n++) {
		int x, y, z;
		adm_j_cell(cx, 0, n, 20, &x, &y, &z);
		adm_ref_set(x, y, z, BLOCK_WOOL, 1 + n % 15);
	}
	assert(adm_export(cx, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0);
	adm_anchor(cx, 0);
	adm_pump_n(50);
	assert(adm_installed(cx, 0) && ch->voxels_synthetic
		   && !s_r->load_completion_pending);
	assert(adm_export(cx, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0);
	/* Ring order: a partial head folds only once no ring edit OLDER than it
	 * can still park; a NEWER one (the rescue's own event) is no barrier. */
	adm_anchor(0, 0);
	const int px = 401;
	assert(remote_pre_chunk_locked(s_r, px, 0, true));
	adm_edits_at(px, n_edits);
	adm_retain_partial_meta(px, 0, 0, 8, BLOCK_LOG, 2);
	uint64_t p_seq = s_r->arrival_sequence;
	struct remote_event* e = &s_r->events[s_r->event_head];
	memset(e, 0, sizeof(*e));
	e->kind = REMOTE_EVENT_PACKET;
	e->packet.type = MP_EVENT_BLOCK_CHANGE;
	e->packet.payload.block_change.x = px * 16;
	e->seq = p_seq - 1;
	s_r->event_count = 1;
	adm_pump();
	assert(s_r->compactions == 1 && "an older ring edit holds the fold");
	e->seq = p_seq + 1;
	adm_pump();
	s_r->event_count = 0;
	assert(s_r->compactions == 2 && s_r->deferred_edits == 0
		   && s_r->chunks[adm_entry(px, 0)].synthetic_base);
	assert(!s_r->stop_now && s_r->mp_end == 0 && s_r->deferred_edits_dropped == 0);
	puts("PASS (s6) FIX2b bodyless compaction: refused body leaves column and "
		 "log untouched, then compacts onto the pre-chunk byte-exact and "
		 "installs without completing loading; ring barrier holds only for "
		 "edits older than the partial");
}

/* ---- (s7) REL-MP-FIX3a: compact the most-parked column first ----------- */
/* tmp/jobs/triage-1020/walk-diag.md: "full head first" folded 1-edit heads
 * ~2 per frame (1,263 in 15 s on vanilla) while bodyless columns held the
 * log. The victim is the greatest eligible tally, whatever backs it. */
static int test_fix3a_largest_victim(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 0);
	for(int k = 0; k < 10; k++) { /* ten full heads, one parked edit each */
		adm_retain_full(500 + k, 0);
		adm_edit_meta((500 + k) * 16 + 3, 90, 3, BLOCK_WOOL, 5);
	}
	assert(remote_pre_chunk_locked(s_r, 600, 0, true));
	assert(remote_pre_chunk_locked(s_r, 601, 0, true));
	/* REL-MP-JOINSPEED: scaled to the 12,288 / 8,192 marks (was 300 + 220 +
	 * 10 = 530 over 512, 230 left <= 256). */
	enum { S7_BIG = 6300, S7_SMALL = 6000 };
	_Static_assert(S7_BIG + S7_SMALL + 10 > REMOTE_DEFERRED_EDIT_HIGH_WATER
					   && S7_SMALL + 10 <= REMOTE_DEFERRED_EDIT_LOW_WATER,
				   "(s7): one fold crosses both marks");
	adm_edits_at(600, S7_BIG);
	adm_edits_at(601, S7_SMALL);
	assert(s_r->deferred_edits == S7_BIG + S7_SMALL + 10 && s_r->compactions == 0);
	adm_pump();
	struct remote_chunk* ch = &s_r->chunks[adm_entry(600, 0)];
	printf("(s7) after one pump: compactions=%u deferred=%u\n",
		   s_r->compactions, s_r->deferred_edits);
	if(ch->state != REMOTE_CHUNK_RETAINED || !ch->synthetic_base
	   || s_r->compactions != 1 || s_r->deferred_edits != S7_SMALL + 10)
		return 1; /* biggest column first; at <= low water, no 2nd fold */
	adm_ref_pre_chunk(); /* four planes: pre-chunk light, folded ids/meta */
	for(int n = 0; n < S7_BIG; n++) {
		int x, y, z;
		adm_j_cell(600, 0, n, 20, &x, &y, &z);
		adm_ref_set(x, y, z, BLOCK_WOOL, 1 + n % 15);
	}
	assert(adm_export(600, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0);
	assert(!s_r->stop_now && s_r->mp_end == 0 && s_r->deferred_edits_dropped == 0);
	puts("PASS (s7) FIX3a victim: the biggest bodyless column compacts before "
		 "ten 1-edit full heads, byte-exact four planes");
	return 0;
}

/* ---- (s8) REL-MP-FIX3a: latched pressure drains to the low-water mark -- */
#define ADM_D_SIDE 17	/* 17x17 announced columns, full bodies held back */
/* distinct cells, > HIGH_WATER (REL-MP-JOINSPEED: was 1,100 > 1,024) */
#define ADM_D_EDITS (REMOTE_DEFERRED_EDIT_HIGH_WATER + 100)
static void adm_d_cell(int n, int* x, int* y, int* z) {
	int c = n % (ADM_D_SIDE * ADM_D_SIDE);
	adm_j_cell(200 + c % ADM_D_SIDE, c / ADM_D_SIDE, n / (ADM_D_SIDE * ADM_D_SIDE),
			   20, x, y, z);
}
static int test_fix3a_drain_to_low_water(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 0);
	for(int c = 0; c < ADM_D_SIDE * ADM_D_SIDE; c++)
		assert(remote_pre_chunk_locked(s_r, 200 + c % ADM_D_SIDE,
									   c / ADM_D_SIDE, true));
	for(int n = 0; n < ADM_D_EDITS; n++) { /* no pump: the reader floods */
		if(n % 3 != 1) { /* 0x35, or the first edit of a two-edit 0x34 */
			s_r->pump_packet_seq = ++s_r->arrival_sequence;
			s_r->pump_packet_sub = 0;
		}
		int x, y, z;
		adm_d_cell(n, &x, &y, &z);
		remote_apply_block_change(s_r, x, (uint8_t)y, z, BLOCK_WOOL,
								  (uint8_t)(1 + n % 15));
		if(adm_rel_mp_terminal("(s8) flood"))
			return 1;
	}
	/* Every still-parked column holds >= ADM_D_EDITS/289 rounded down >= 3
	 * edits, so each compaction frees >= 3: every pump >= 3, two per pump
	 * >= 6. */
	uint32_t start = s_r->deferred_edits;
	assert(start > REMOTE_DEFERRED_EDIT_HIGH_WATER);
	uint32_t bound = (start - REMOTE_DEFERRED_EDIT_LOW_WATER) / 6u + 1u;
	uint32_t pumps = 0;
	while(s_r->deferred_edits > REMOTE_DEFERRED_EDIT_LOW_WATER && pumps < bound) {
		uint32_t before = s_r->deferred_edits, done = s_r->compactions;
		adm_pump();
		pumps++;
		if(s_r->deferred_edits + 3u > before) {
			printf("(s8) zero-progress pump %u at %u parked\n", pumps, before);
			return 1;
		}
		assert(s_r->compactions - done <= REMOTE_COMPACTIONS_PER_PUMP);
	}
	printf("(s8) %u parked -> %u after %u pumps (bound %u)\n", start,
		   s_r->deferred_edits, pumps, bound);
	if(s_r->deferred_edits > REMOTE_DEFERRED_EDIT_LOW_WATER)
		return 1;
	uint32_t settled = s_r->compactions;
	adm_pump(); /* latch released: no more compactions at low water */
	assert(s_r->compactions == settled);
	assert(s_r->deferred_edits + s_r->deferred_edits_applied == ADM_D_EDITS
		   && s_r->deferred_edits_dropped == 0 && !s_r->stop_now);
	adm_ref_pre_chunk(); /* column (200,0): every edit, ids + metadata */
	for(int n = 0; n < ADM_D_EDITS; n += ADM_D_SIDE * ADM_D_SIDE) {
		int x, y, z;
		adm_d_cell(n, &x, &y, &z);
		adm_ref_set(x, y, z, BLOCK_WOOL, 1 + n % 15);
	}
	adm_promote_matches_ref(200, 0);
	printf("PASS (s8) FIX3a drain: %d distinct edits on %d bodyless columns, "
		   "pressure latched at %u, drained to <= %d in %u pumps\n",
		   ADM_D_EDITS, ADM_D_SIDE * ADM_D_SIDE, start,
		   REMOTE_DEFERRED_EDIT_LOW_WATER, pumps);
	return 0;
}

/* (s9) FIX3a-r: the FORCED reader rescue keeps the base 450fbf31f rule (full
 * heads first). Saturated log: bodyless 701 holds 1,023 edits but its
 * replacement cannot fit the 256 B hole; full head 700 (1 edit) can. */
static int test_fix3a_forced_rescue_base_victim(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_retain_partial(700, 0, 128, BLOCK_STONE); /* full stone head */
	adm_edit_meta(700 * 16 + 3, 90, 3, BLOCK_WOOL, 5);
	assert(remote_pre_chunk_locked(s_r, 701, 0, true));
	adm_edits_at(701, REMOTE_DEFERRED_EDIT_CAPACITY - 1);
	void *gap = mp_backing_alloc(256), *hog[128];
	size_t hogs = 0;
	for(size_t len = ADM_WII_BACKING; len && hogs < 128;)
		if((hog[hogs] = mp_backing_alloc(len)) != NULL) hogs++; else len /= 2;
	assert(gap && hogs < 128 && s_r->deferred_edits == REMOTE_DEFERRED_EDIT_CAPACITY);
	mp_backing_free(gap);
	adm_edit_meta(701 * 16, 100, 0, BLOCK_WOOL, 7); /* full log: reader rescue */
	while(hogs) mp_backing_free(hog[--hogs]);
	printf("(s9) compactions=%u failures=%u stop=%d\n", s_r->compactions,
		   s_r->compaction_failures, (int)s_r->stop_now);
	if(adm_rel_mp_terminal("(s9) forced rescue")) return 1;
	assert(s_r->compactions == 1 && s_r->compaction_failures == 0
		   && s_r->deferred_edits == REMOTE_DEFERRED_EDIT_CAPACITY
		   && s_r->deferred_edits_applied == 1 && s_r->deferred_edits_dropped == 0);
	memset(adm_ref, 0, ADM_RAW);
	adm_ref_partial(0, 128, BLOCK_STONE, 0);
	adm_ref_set(700 * 16 + 3, 90, 3, BLOCK_WOOL, 5);
	assert(adm_export(700, 0) && memcmp(adm_out, adm_ref, ADM_RAW) == 0);
	puts("PASS (s9) FIX3a-r forced rescue: base victim (fitting full head) "
		 "folds first, session lives");
	return 0;
}

/* ---- (t) the body-path scratch is inside the accounting --------------- */
/* Mirrors server_remote_chunks.c's private REMOTE_DEMOTE_SCRATCH_BYTES
 * (REMOTE_COLUMN_RAW_BYTES + REMOTE_DEMOTE_STAGE_BYTES). Asserted by EQUALITY
 * below, so if that file's staging size moves this case fails loudly instead
 * of quietly asserting something weaker. */
#define ADM_RECODE_SCRATCH (ADM_RAW + (ADM_RAW + 1024u))

static void test_scratch_is_accounted(void) {
	adm_fresh_session(ADM_WII_BACKING);
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_scratch_stats(&in_use, &peak, &capacity);
	assert(in_use == 0 && peak == 0
		   && capacity == (size_t)MEM2_MP_SCRATCH_BYTES
		   && "a fresh session starts with an empty, readable scratch budget");
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, 1);
	for(int cx = -3; cx <= 3; cx++)
		for(int cz = -3; cz <= 3; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	adm_pump_n(60); /* promotions: the INFLATE slot */
	/* The inflate scratch is accounted while a promotion holds it. This used
	 * to be a bare MEM1 malloc(inflated_length) that no budget saw. */
	mp_backing_scratch_stats(&in_use, &peak, &capacity);
	assert(peak >= (size_t)MP_CHUNK_INFLATED_CAP
		   && "the inflate slot showed up in the accounting");
	size_t inflate_peak = peak;
	adm_anchor(6, 6);
	adm_pump_n(60); /* demotions: the RECODE slot */
	assert(s_r->demotions > 0 && "the demotion re-encode really ran");
	mp_backing_scratch_stats(&in_use, &peak, &capacity);
	/* Both slots are real bytes in the reservation's side region now, not
	 * MEM1 mallocs outside every budget (review L10). The re-encode slot is
	 * held for the rest of the session (one per connection), so it is
	 * readable as in_use right now, by EQUALITY -- the constant mirrors
	 * chunks.c's private size, and a drift there must fail here. */
	assert(in_use == (size_t)ADM_RECODE_SCRATCH
		   || in_use == (size_t)ADM_RECODE_SCRATCH + MP_CHUNK_INFLATED_CAP);
	assert(in_use >= (size_t)ADM_RECODE_SCRATCH
		   && "the re-encode scratch is held, and it is ACCOUNTED");
	assert(peak >= (size_t)ADM_RECODE_SCRATCH && peak >= inflate_peak);
	assert(peak <= capacity);
	size_t held = in_use, high = peak;
	/* And they are RELEASED with the session, like the bodies. */
	server_remote_destroy(&adm_owner);
	mp_backing_scratch_stats(&in_use, &peak, &capacity);
	assert(in_use == 0 && "no scratch slot outlives the session");
	printf("PASS (t) scratch accounted: recode %u B + inflate %u B live in "
		   "the reservation's %u B side region (held %zu B, peak %zu B of "
		   "%zu B) and are 0 B after the session ends\n",
		   (unsigned)ADM_RECODE_SCRATCH,
		   (unsigned)MP_CHUNK_INFLATED_CAP, MEM2_MP_SCRATCH_BYTES, held, high,
		   capacity);
	adm_fresh_session(ADM_WII_BACKING);
}

/* ======================================================================
 * F5b (v1 hostile review H1, H4, H5, M7 + section 4's uncovered items)
 * ====================================================================== */

#define ADM_F5B_RADIUS 2
#define ADM_F5B_CAP REMOTE_RESIDENT_COLUMN_CAP(ADM_F5B_RADIUS) /* (2R+3)^2 */

/* A pump that runs ONLY the promotion step -- i.e. a pump on which the
 * residency step did not demote, which is what H1's event-ring veto produced
 * on essentially every frame of its burst. */
static void adm_pump_promote_only(int n) {
	for(int i = 0; i < n; i++) {
		assert(adm_promotion_step());
		remote_emit_pending_unloads_locked(s_r);
	}
}

static size_t adm_installed_count(void) {
	size_t n = 0;
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		if(s_r->chunks[i].installed)
			n++;
	return n;
}

/* ---- (u) H1: the resident set is hard-capped across a centre move ------ */
static void test_hard_cap_survives_respawn(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	for(int cx = -ADM_F5B_RADIUS; cx <= ADM_F5B_RADIUS; cx++)
		for(int cz = -ADM_F5B_RADIUS; cz <= ADM_F5B_RADIUS; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	adm_pump_n(25 * 4);
	size_t first_disk = (2 * ADM_F5B_RADIUS + 1) * (2 * ADM_F5B_RADIUS + 1);
	assert(s_r->resident_cols == first_disk);
	assert(adm_installed_count() == first_disk
		   && "the maintained count agrees with the table");
	/* THE SAME-DIMENSION RESPAWN (H1's scenario). Packet9 with an unchanged
	 * dimension bumps no epoch and clears NO entries, then Packet13 jumps
	 * the position: all 25 columns above are suddenly 100 columns away and
	 * still installed. Before F5b nothing counted them, so a second full
	 * disk installed on top -- 2x81,920 B per column pair, the measured
	 * carve exhaustion. */
	adm_anchor(100, 100);
	for(int cx = 100 - ADM_F5B_RADIUS; cx <= 100 + ADM_F5B_RADIUS; cx++)
		for(int cz = 100 - ADM_F5B_RADIUS; cz <= 100 + ADM_F5B_RADIUS; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	/* Promotion-only pumps: the demotion never gets a turn, exactly as the
	 * unbounded event-ring veto arranged. The cap is the ONLY thing standing
	 * between this and 50 resident columns. */
	size_t high_water = 0;
	for(int i = 0; i < 25 * 8; i++) {
		adm_pump_promote_only(1);
		if(s_r->resident_cols > high_water)
			high_water = s_r->resident_cols;
		assert(s_r->resident_cols <= ADM_F5B_CAP
			   && "the resident set never exceeds (2R+3)^2");
	}
	assert(s_r->promote_deferred_cap > 0
		   && "and a promotion was actually deferred by the cap, not by luck");
	assert(s_r->resident_cols == ADM_F5B_CAP);
	assert(s_r->retention_evictions == 0 && s_r->forced_reconnects == 0
		   && !s_r->stop_now && "nothing was dropped to stay under the cap");
	/* Now let the residency step run: the old disk is beyond R+1, so it
	 * demotes one per pump and the new disk finishes installing. */
	adm_pump_n(200);
	assert(s_r->resident_cols == first_disk
		   && "the set settles back to the new centre's own disk");
	for(int cx = 100 - ADM_F5B_RADIUS; cx <= 100 + ADM_F5B_RADIUS; cx++)
		for(int cz = 100 - ADM_F5B_RADIUS; cz <= 100 + ADM_F5B_RADIUS; cz++)
			assert(adm_installed(cx, cz));
	for(int cx = -ADM_F5B_RADIUS; cx <= ADM_F5B_RADIUS; cx++)
		for(int cz = -ADM_F5B_RADIUS; cz <= ADM_F5B_RADIUS; cz++) {
			assert(!adm_installed(cx, cz));
			size_t i = adm_entry(cx, cz);
			assert(i != REMOTE_CHUNK_ENTRY_CAPACITY
				   && s_r->chunks[i].wire.compressed
				   && "demoted, not dropped: the body is still there");
		}
	printf("PASS (u) hard cap: same-dimension respawn moved the centre 100 "
		   "columns with 25 still installed; resident high-water %zu <= cap "
		   "%zu (was 50 unbounded), %u promotions deferred by the cap, then "
		   "%u demotions settled it back to %zu\n",
		   high_water, (size_t)ADM_F5B_CAP, s_r->promote_deferred_cap,
		   s_r->demotions, s_r->resident_cols);
}

/* ---- (u2) H1: the ordering veto is bounded ---------------------------- */
static void test_veto_is_bounded(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	assert(adm_retain_terrain(0, 0, 0, 0, 0));
	adm_pump_n(8);
	assert(adm_installed(0, 0) && s_r->resident_cols == 1);
	/* Move the centre so (0,0) is past the hysteresis, then hold a decoded
	 * event in the ring -- the state that used to veto the residency step
	 * for as long as the server kept editing. The event's own seq is NEWER
	 * than anything this column contains, so the per-column publication test
	 * permits the demotion once the pump budget is spent. */
	adm_anchor(50, 50);
	s_r->event_count = 1;
	s_r->event_head = 0;
	s_r->events[0].seq = s_r->arrival_sequence + 1000;
	uint32_t skips_before = s_r->residency_veto_skips;
	for(unsigned i = 0; i < REMOTE_RESIDENCY_VETO_PUMP_LIMIT; i++) {
		adm_pump();
		assert(s_r->resident_cols == 1 && "the veto still holds inside bound");
	}
	assert(s_r->residency_veto_skips
		   == skips_before + REMOTE_RESIDENCY_VETO_PUMP_LIMIT);
	assert(s_r->demotions == 0);
	adm_pump();
	assert(s_r->residency_veto_overrides == 1
		   && "past the bound the step runs anyway");
	assert(s_r->demotions == 1 && s_r->resident_cols == 0
		   && !adm_installed(0, 0));
	s_r->event_count = 0;
	printf("PASS (u2) bounded veto: %u vetoed pumps held the demotion, the "
		   "%uth demoted anyway under the publication-gate rule (overrides "
		   "%u, demotions %u)\n",
		   (unsigned)REMOTE_RESIDENCY_VETO_PUMP_LIMIT,
		   (unsigned)REMOTE_RESIDENCY_VETO_PUMP_LIMIT + 1u,
		   s_r->residency_veto_overrides, s_r->demotions);
}

/* ---- (v) H5: the gate is live from the LOGIN spawn position ------------ */
static void test_centre_from_login_spawn(void) {
	adm_fresh_session(ADM_WII_BACKING);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	/* NO position packet -- only Packet6's world spawn, which is what a
	 * server that streams before it teleports gives us. H5: this window used
	 * to admit EVERY announced column. */
	assert(!s_r->have_authoritative_position);
	adm_spawn_hint(0, 0);
	for(int cx = -6; cx <= 6; cx++)
		for(int cz = -6; cz <= 6; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	adm_pump_n(169 * 4);
	size_t disk = (2 * ADM_F5B_RADIUS + 1) * (2 * ADM_F5B_RADIUS + 1);
	assert(s_r->resident_cols == disk
		   && "the spawn hint alone bounds the install to the radius disk");
	assert(!s_r->have_authoritative_position
		   && "and it did so without inventing a position");
	/* A real position outranks the hint and moves the disk; a LATER spawn
	 * hint (a respawn's Packet6) must not drag the centre back. */
	adm_anchor(20, 0);
	adm_spawn_hint(0, 0);
	int32_t cx_now = 0, cz_now = 0;
	assert(remote_admission_centre_locked(s_r, &cx_now, &cz_now));
	assert(cx_now == 20 && cz_now == 0
		   && "a spawn hint never overwrites a live position");
	printf("PASS (v) login centre: spawn hint only (no Packet13) -> %zu of "
		   "169 announced columns installed (radius %d disk), and the hint "
		   "cannot overwrite a later authoritative position\n",
		   s_r->resident_cols, ADM_F5B_RADIUS);
}

/* ---- (w) H4: the re-encode's deflate runs with impl->lock RELEASED ---- */
static unsigned s_probe_calls, s_probe_unlocked, s_probe_blocked;
static void adm_deflate_probe(struct server_remote_impl* impl) {
	s_probe_calls++;
	/* The brief's instrument: trylock the very mutex the read thread needs
	 * for remote_retain_map_locked. platform/thread.h exposes no trylock
	 * (nothing in production wants one), and this test is PC-only, so it
	 * goes at the pthread underneath -- reading the same lock, not a copy. */
	if(pthread_mutex_trylock(&impl->lock.native) == 0) {
		s_probe_unlocked++;
		pthread_mutex_unlock(&impl->lock.native);
	} else {
		s_probe_blocked++;
	}
}

static void test_deflate_runs_unlocked(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	assert(adm_retain_terrain(0, 0, 0, 0, 0));
	adm_pump_n(8);
	assert(adm_installed(0, 0));
	s_probe_calls = s_probe_unlocked = s_probe_blocked = 0;
	server_remote_test_set_deflate_probe(adm_deflate_probe);
	adm_anchor(50, 50); /* past the hysteresis -> the next pump demotes */
	adm_pump_n(4);
	server_remote_test_set_deflate_probe(NULL);
	assert(s_r->demotions >= 1 && "a demotion actually ran");
	assert(s_probe_calls >= 1 && "the probe was reached inside the re-encode");
	assert(s_probe_blocked == 0
		   && "impl->lock is NOT held across the gather/deflate phase");
	assert(s_probe_unlocked == s_probe_calls);
	assert(s_r->demote_deflate_us_last > 0
		   && s_r->demote_deflate_us_max >= s_r->demote_deflate_us_last
		   && "and the phase is measured, not assumed");
	assert(s_r->recode_aborts == 0
		   && "a quiet session revalidates cleanly across the unlocked gap");
	printf("PASS (w) lock split: %u deflate-phase probes, all acquired "
		   "impl->lock (0 blocked); unlocked gather+deflate measured at %u us "
		   "(session max %u us) for %u demotions\n",
		   s_probe_calls, s_r->demote_deflate_us_last,
		   s_r->demote_deflate_us_max, s_r->demotions);
}

/* ---- (x) a DELAYED announcement installs and is accounted ------------- */
static void test_delayed_announcement(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	for(int cx = -ADM_F5B_RADIUS; cx <= ADM_F5B_RADIUS; cx++)
		for(int cz = -ADM_F5B_RADIUS; cz <= ADM_F5B_RADIUS; cz++)
			if(cx || cz) /* everything but the centre column */
				assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	adm_pump_n(24 * 4);
	size_t resident_before = s_r->resident_cols;
	assert(resident_before == 24);
	size_t in_use_before = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use_before, &peak, &capacity);
	/* The burst is long over and the gate is quiet. NOW the server announces
	 * the column it held back -- in radius, this epoch. */
	size_t body = adm_retain_terrain(0, 0, 0, 0, 0);
	assert(body);
	size_t in_use_announced = 0;
	mp_backing_stats(&in_use_announced, &peak, &capacity);
	assert(in_use_announced == in_use_before + body
		   && "the late body is charged to the reservation like any other");
	adm_pump_n(8);
	assert(adm_installed(0, 0) && "and it installs on the next pumps");
	assert(s_r->resident_cols == resident_before + 1
		   && "the maintained resident count follows it");
	assert(adm_state(0, 0) == REMOTE_CHUNK_RESIDENT);
	printf("PASS (x) delayed announcement: a %zu B body announced %d pumps "
		   "after the burst installed and raised the resident count %zu -> "
		   "%zu\n",
		   body, 24 * 4, resident_before, s_r->resident_cols);
}

/* ---- (y) 0x32 unload of a RESIDENT column ----------------------------- */
static void test_unload_of_resident_column(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	assert(adm_retain_terrain(1, 1, 1, 1, 0));
	adm_pump_n(8);
	assert(adm_installed(1, 1) && adm_state(1, 1) == REMOTE_CHUNK_RESIDENT);
	size_t i = adm_entry(1, 1);
	assert(s_r->chunks[i].installed && s_r->resident_cols == 1);
	/* A RESIDENT column's head body was freed at publication, so the
	 * reservation is not what this case measures -- the VOXELS are. */
	assert(remote_pre_chunk_locked(s_r, 1, 1, false));
	remote_emit_pending_unloads_locked(s_r);
	assert(!adm_installed(1, 1) && "the client's voxels are gone");
	assert(s_r->resident_cols == 0
		   && "and the resident count dropped with them");
	assert(adm_entry(1, 1) == REMOTE_CHUNK_ENTRY_CAPACITY
		   && "the entry is released");
	assert(s_r->unload_refused_retries == 0);
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	assert(in_use == 0 && "nothing of that column is left in the reservation");
	printf("PASS (y) resident unload: 0x32 against an INSTALLED column freed "
		   "its voxels, cleared the installed bit, dropped the resident count "
		   "to %zu and released the entry\n",
		   s_r->resident_cols);
}

/* ---- (y2) M7: an unload the client REFUSES is kept and retried -------- */
static void test_unload_refusal_is_retried(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	assert(adm_retain_terrain(1, 0, 1, 0, 0));
	adm_pump_n(8);
	assert(adm_installed(1, 0) && s_r->resident_cols == 1);
	size_t i = adm_entry(1, 0);
	assert(remote_pre_chunk_locked(s_r, 1, 0, false));
	assert(s_r->chunks[i].unload_pending);
	/* The client's gated handler refuses (world not reset-ready / stream
	 * recovery). M7: this used to destroy the entry and its body while the
	 * 81,920 B of voxels stayed installed with nothing pointing at them. */
	server_remote_test_refuse_unloads(1);
	remote_emit_pending_unloads_locked(s_r);
	assert(adm_installed(1, 0) && "the client kept the column");
	assert(adm_entry(1, 0) == i && "so the ENTRY is kept too");
	assert(s_r->chunks[i].installed
		   && "the installed bit still describes reality");
	assert(s_r->chunks[i].unload_pending && "and the unload is still owed");
	assert(s_r->resident_cols == 1 && s_r->unload_refused_retries == 1);
	/* Next pump, the client accepts: the unload completes normally. */
	remote_emit_pending_unloads_locked(s_r);
	assert(!adm_installed(1, 0) && s_r->resident_cols == 0);
	assert(adm_entry(1, 0) == REMOTE_CHUNK_ENTRY_CAPACITY);
	assert(s_r->unload_refused_retries == 1);
	printf("PASS (y2) M7: a refused 0x32 kept the entry, the installed bit "
		   "and unload_pending (retries %u), and the retry completed the "
		   "unload -- no orphaned voxels\n",
		   s_r->unload_refused_retries);
}

/* ---- (y3) the demotion REVERT branch (client refuses the unload) ------ */
static void test_demotion_revert_on_refusal(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	assert(adm_retain_terrain(0, 0, 0, 0, 0));
	adm_pump_n(8);
	assert(adm_installed(0, 0) && s_r->resident_cols == 1);
	size_t i = adm_entry(0, 0);
	uint64_t arrival_before = s_r->chunks[i].arrival;
	size_t retained_before = s_r->retained_bytes;
	size_t in_use_before = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use_before, &peak, &capacity);
	adm_anchor(50, 50); /* past the hysteresis */
	server_remote_test_refuse_unloads(1);
	uint32_t failures_before = s_r->demotion_failures;
	adm_pump();
	assert(s_r->demotion_failures == failures_before + 1
		   && "a refused unload is a demotion FAILURE, retried next pump");
	assert(adm_installed(0, 0) && "the voxels are still the client's");
	assert(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT
		   && s_r->chunks[i].installed && s_r->resident_cols == 1
		   && "and the entry is exactly as it was");
	assert(s_r->chunks[i].arrival == arrival_before);
	assert(s_r->chunks[i].queued_count == 0);
	assert(s_r->retained_bytes == retained_before
		   && "the re-encode's own body was freed on the revert");
	size_t in_use_after = 0;
	mp_backing_stats(&in_use_after, &peak, &capacity);
	assert(in_use_after == in_use_before
		   && "so the reservation is back where it started");
	/* Unarmed, the very next pump demotes for real. */
	adm_pump();
	assert(s_r->demotions == 1 && !adm_installed(0, 0)
		   && s_r->resident_cols == 0);
	assert(s_r->chunks[i].wire.compressed && "with its state re-encoded");
	printf("PASS (y3) demotion revert: the client refused the unload -> "
		   "column stayed RESIDENT with its chain and byte accounting intact "
		   "(failures %u), and the next pump demoted it\n",
		   s_r->demotion_failures);
}

/* ---- (z) a DIMENSION SWAP clears everything --------------------------- */
static void test_dimension_swap_clears(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	for(int cx = -3; cx <= 3; cx++)
		for(int cz = -3; cz <= 3; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	adm_pump_n(49 * 4);
	assert(s_r->resident_cols
		   == (size_t)(2 * ADM_F5B_RADIUS + 1) * (2 * ADM_F5B_RADIUS + 1));
	size_t in_use_before = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use_before, &peak, &capacity);
	assert(in_use_before > 0 && "backed columns are holding reservation bytes");
	/* The read thread's half of a dimension-changed Respawn: bump the epoch
	 * and clear the table AT that point in the stream (transport.c), then
	 * the consumer applies the epoch and drops the centre with the world it
	 * belonged to (protocol.c). Runs here with live bodies, an active
	 * promotion and a live demote scratch -- the combination review section
	 * 4 found untested. */
	assert(s_r->promotion.active || s_r->demote_scratch
		   || s_r->retained_entries);
	s_r->world_epoch++;
	remote_chunks_clear_locked(s_r);
	s_r->applied_epoch = s_r->world_epoch;
	remote_admission_centre_clear(s_r);
	clin_force_world_reset_login(WORLD_DIM_NETHER, 42);
	size_t in_use_cleared = 0;
	mp_backing_stats(&in_use_cleared, &peak, &capacity);
	assert(in_use_cleared == 0
		   && "every body went back to the reservation with the table");
	assert(s_r->resident_cols == 0 && s_r->retained_entries == 0
		   && s_r->retained_bytes == 0);
	assert(s_r->resident_cols_peak > 0
		   && "the session high-water survives the swap (it is evidence)");
	assert(!s_r->promotion.active && !s_r->demote_scratch);
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		assert(s_r->chunks[i].state == REMOTE_CHUNK_UNUSED
			   && !s_r->chunks[i].installed);
	/* The NEW world's bodies install against the new centre. */
	adm_anchor(0, 0);
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++)
			assert(adm_retain_terrain(cx, cz, cx + 7, cz + 7, 0));
	adm_pump_n(9 * 4);
	assert(s_r->resident_cols == 9 && "new-epoch bodies promote normally");
	assert(!s_r->stop_now && s_r->forced_reconnects == 0);
	printf("PASS (z) dimension swap: cleared 49 entries mid-session with "
		   "live bodies/promotion/scratch -- reservation %zu B -> 0 B, "
		   "resident %zu (peak kept at %zu), then 9 new-epoch columns "
		   "installed\n",
		   in_use_before, s_r->resident_cols, s_r->resident_cols_peak);
}

/* ---- (aa) a session with no remote chunk traffic touches nothing ------ */
/* The SP end-to-end smoke this suite can actually make honest: single player
 * runs no server_remote session at all, so the acceptance claim "SP goldens
 * unchanged" reduces to "the admission machinery does nothing without remote
 * chunk traffic". Pumped 200 times with an anchor, a radius and a world, and
 * every admission counter must still read zero -- no promotions, no
 * demotions, no re-encode time, no reservation bytes. A regression that made
 * the pump touch the world or the carve on its own would show up here. */
static void test_no_traffic_no_admission(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, ADM_F5B_RADIUS);
	adm_pump_n(200);
	assert(s_r->resident_cols == 0 && s_r->resident_cols_peak == 0);
	assert(s_r->demotions == 0 && s_r->demotion_failures == 0);
	assert(s_r->compactions == 0 && s_r->compaction_failures == 0);
	assert(s_r->promote_deferred_cap == 0
		   && s_r->promote_deferred_memory == 0);
	assert(s_r->demote_deflate_us_last == 0 && s_r->demote_deflate_us_max == 0);
	assert(s_r->recode_aborts == 0 && s_r->unload_refused_retries == 0);
	assert(s_r->residency_veto_skips == 0
		   && s_r->residency_veto_overrides == 0);
	assert(s_r->backing_refusals == 0 && s_r->backing_latch_bugs == 0);
	assert(s_r->retained_entries == 0 && s_r->retained_bytes == 0);
	assert(!s_r->demote_scratch && !s_r->recode_busy);
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	assert(in_use == 0 && peak == 0
		   && "not one reservation byte is spent without a remote column");
	/* And the public status mirror says the same thing. */
	struct server_remote_status status;
	server_remote_get_status(&adm_owner, &status);
	assert(status.resident_columns == 0 && status.resident_columns_peak == 0);
	assert(status.promote_deferred_cap == 0 && status.recode_aborts == 0);
	assert(status.demote_deflate_us_max == 0);
	assert(status.unload_refused_retries == 0);
	printf("PASS (aa) no-traffic smoke: 200 pumps with a radius and a centre "
		   "but no Packet51 -- every admission counter and the whole "
		   "reservation still read zero\n");
}

/* ======================================================================
 * F7 (mp-split-0912/38) -- the centre follows the position the client SENDS
 * ====================================================================== */

#define ADM_F7_RADIUS 2

/* THE REAL OUTBOUND MOVEMENT PATH, driven exactly the way server_remote_pump
 * drives it: move the body to where the player now is, then hand
 * remote_process_srpc an SRPC_PLAYER_POS through the SAME svin mailbox the
 * pump drains (server_remote.c's `svin_process_messages(remote_process_srpc,
 * remote, false)`). The production movement block decides which
 * Packet10/11/12/13 variant to emit and whether the centre moves -- nothing
 * here reaches past that boundary. */
static void adm_send_position(double x, double y, double z) {
	gstate.players[0].camera.x = x;
	gstate.players[0].camera.y = y;
	gstate.players[0].camera.z = z;
	if(gstate.players[0].local_player) {
		gstate.players[0].local_player->pos[0] = x;
		gstate.players[0].local_player->pos[1] = y;
		gstate.players[0].local_player->pos[2] = z;
	}
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x, .y = y, .z = z, .rx = 0.0f, .ry = 0.0f,
			.on_ground = true, .correction_seq = s_r->correction_seq},
	};
	assert(svin_rpc_send(&pos));
	svin_process_messages(remote_process_srpc, &adm_owner, false);
}

/* A real INCOMING server position (a moving Packet13) through production
 * process_event -> remote_handle_correction -- the OTHER feeder of the one
 * admission centre, so this case proves the two coexist rather than trusting
 * the adm_anchor stand-in for both. */
static void adm_incoming_correction(double x, double y, double z) {
	struct remote_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = REMOTE_EVENT_PACKET;
	ev.generation = s_r->generation;
	ev.world_epoch = s_r->world_epoch;
	ev.packet.type = MP_EVENT_PLAYER_LOOK_MOVE;
	ev.packet.payload.movement = (struct mp_movement) {.x = x, .y = y,
		.stance = y, .z = z, .yaw = 0.0f, .pitch = 0.0f,
		.moving = true, .rotating = true, .on_ground = true};
	assert(process_event(&adm_owner, &ev));
}

static int32_t adm_centre_x(void) {
	int32_t cx = 0, cz = 0;
	thread_mutex_lock(&s_r->lock);
	bool have = remote_admission_centre_locked(s_r, &cx, &cz);
	thread_mutex_unlock(&s_r->lock);
	assert(have);
	return cx;
}
static int32_t adm_centre_z(void) {
	int32_t cx = 0, cz = 0;
	thread_mutex_lock(&s_r->lock);
	bool have = remote_admission_centre_locked(s_r, &cx, &cz);
	thread_mutex_unlock(&s_r->lock);
	assert(have);
	return cz;
}
static int adm_cheb(int ax, int az, int bx, int bz) {
	int dx = ax - bx, dz = az - bz;
	if(dx < 0)
		dx = -dx;
	if(dz < 0)
		dz = -dz;
	return dx > dz ? dx : dz;
}

/* ---- (ab) F7: walking moves the centre, so columns actually stream ---- */
static void test_centre_follows_sent_position(void) {
	adm_fresh_session(ADM_WII_BACKING);
	/* The movement block runs ONLY after login: its own fast-path gate reads
	 * `impl->state == REMOTE_PLAY && !impl->stop_now`
	 * (server_remote_protocol.c remote_process_srpc) and
	 * queue_movement_playing re-validates REMOTE_PLAY under the same lock
	 * acquisition as the frame append (server_remote_transport.c), so the
	 * centre set that follows that append can never run before login. This is
	 * the ordinary post-install walking state: load complete, so the
	 * `preworld` force is off and the Beta sendMotionUpdates dedupe is live. */
	s_r->state = REMOTE_PLAY;
	s_r->remote_load_complete_sent = true;
	server_remote_set_admit_radius(&adm_owner, ADM_F7_RADIUS);
	/* LOGIN: the server's 0x0D teleport put us in column (0,0). */
	adm_anchor(0, 0);
	const size_t disk = (2 * ADM_F7_RADIUS + 1) * (2 * ADM_F7_RADIUS + 1);
	for(int cx = -ADM_F7_RADIUS; cx <= ADM_F7_RADIUS; cx++)
		for(int cz = -ADM_F7_RADIUS; cz <= ADM_F7_RADIUS; cz++)
			assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	/* The strip the walk crosses, ahead of the login disk: cx 3..8 at cz 0
	 * (cx 0..2 are already in the disk above, and re-retaining a live entry
	 * is the superseded-body path, which is case (e)'s subject, not this
	 * one). This is the dense_walk_0913c shape -- the server streams the
	 * columns ahead of the player and they all arrive BACKED. */
	for(int cx = ADM_F7_RADIUS + 1; cx <= 8; cx++)
		assert(adm_retain_terrain(cx, 0, cx, 0, 0));
	assert(s_r->retained_entries == disk + 6);
	adm_pump_n((int)(disk + 6) * 4);
	assert(s_r->resident_cols == disk
		   && "standing at the login centre: exactly the (2R+1)^2 disk");
	for(int cx = ADM_F7_RADIUS + 1; cx <= 8; cx++)
		assert(adm_state(cx, 0) == REMOTE_CHUNK_RETAINED
			   && "the strip ahead of the walk is BACKED, waiting for a centre");
	assert(s_r->demotions == 0);
	assert(adm_centre_x() == 0 && adm_centre_z() == 0);

	/* THE WALK. One sent position at column 6 -- the SAME variant the game
	 * emits (a moved+looked sample is Packet13; a moved-only sample is
	 * Packet11), through the real mailbox and the real movement block. Before
	 * F7 this changed nothing at all: dense_walk_0913c walked 53 blocks and
	 * mp_resident_cols never left the login disk. */
	adm_send_position(16.0 * 6.0 + 0.5, 64.0, 0.5);
	assert(adm_centre_x() == 6 && adm_centre_z() == 0
		   && "the centre followed the position we SENT");
	assert(s_r->frame_count > 0 && "and it did so off an actually-queued frame");
	adm_pump_n((int)(disk + 6) * 8);

	/* Promote-on-entry: the strip columns within R of (6,0) are resident. */
	for(int cx = 6 - ADM_F7_RADIUS; cx <= 6 + ADM_F7_RADIUS; cx++) {
		assert(adm_state(cx, 0) == REMOTE_CHUNK_RESIDENT
			   && "columns entering admit_radius promoted");
		assert(adm_installed(cx, 0));
	}
	/* Demote-on-exit: nothing past the R + 1 hysteresis ring is resident any
	 * more, and the demotions are real (the counter moved), not an artefact
	 * of the columns never having installed. */
	assert(s_r->demotions > 0);
	size_t still_resident = 0;
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		if(s_r->chunks[i].state == REMOTE_CHUNK_UNUSED)
			continue;
		int d = adm_cheb(s_r->chunks[i].chunk_x, s_r->chunks[i].chunk_z, 6, 0);
		if(d > ADM_F7_RADIUS + 1) {
			assert(s_r->chunks[i].state != REMOTE_CHUNK_RESIDENT
				   && "past R + 1 of the NEW centre: demoted");
			assert(!s_r->chunks[i].installed);
			assert(s_r->chunks[i].wire.compressed
				   && s_r->chunks[i].wire.compressed_length
				   && "demoted losslessly: the body is still there");
		} else if(s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT)
			still_resident++;
	}
	assert(still_resident == s_r->resident_cols);
	assert(s_r->demotion_failures == 0 && s_r->retention_evictions == 0
		   && s_r->forced_reconnects == 0 && !s_r->stop_now);

	/* THE CENSUS follows the same one centre (L12), not the caller's camera:
	 * ask it about column (0,0) and it still answers from the server's (6,0).
	 * That is the whole point of the single accessor -- the loading screen can
	 * never demand a disk the gate is not filling. */
	struct server_remote_ready_census census;
	assert(server_remote_ready_census(&adm_owner, 0, 0, ADM_F7_RADIUS,
									  &census));
	assert(census.centre_x == 6 && census.centre_z == 0
		   && census.centre_authoritative);

	/* AND THE INCOMING CORRECTION STILL WINS. A rubber-band back to
	 * (0.5, y, 0.5) goes through the very same setter from
	 * remote_handle_correction, so the centre reads (0,0) again -- F5b's
	 * behaviour is preserved, not replaced. */
	uint32_t corrections_before = s_r->corrections_received;
	adm_incoming_correction(0.5, 64.0, 0.5);
	assert(s_r->corrections_received == corrections_before + 1);
	assert(adm_centre_x() == 0 && adm_centre_z() == 0
		   && "an incoming server position overrides the sent one");
	/* NOT ON PACKET10. Re-sending the IDENTICAL sample is exactly Beta's
	 * dedupe case: flag1/flag2 are both false, so sendMotionUpdates emits
	 * Packet10Flying, which carries no position at all [740c583
	 * EntityClientPlayerMP.sendMotionUpdates]. The centre must therefore
	 * STAND where the correction put it -- a Packet10 that moved it would be
	 * this fix overreaching past the position-bearing variants. */
	uint32_t frames_before = (uint32_t)s_r->frame_count;
	adm_send_position(16.0 * 6.0 + 0.5, 64.0, 0.5);
	assert((uint32_t)s_r->frame_count == frames_before + 1
		   && "the deduped sample still put a Packet10 on the wire");
	assert(adm_centre_x() == 0 && adm_centre_z() == 0
		   && "a Packet10 carries no position: the centre does not move");
	/* And a genuinely NEW sample resumes tracking from wherever the
	 * correction left us -- the two feeders are last-writer-wins, in order.
	 * This one is a moved-but-not-looked sample, i.e. the Packet11 variant
	 * (the walk above was the first sample of the session, so its look was
	 * also new: Packet13). Both position-bearing variants feed the centre. */
	adm_send_position(16.0 + 0.5, 64.0, 0.5);
	assert(adm_centre_x() == 1 && adm_centre_z() == 0);

	printf("PASS (ab) sent position is the centre: R=%d, login disk %zu "
		   "resident with cx 3..8 BACKED, one sent Packet13 at column 6 moved "
		   "the centre there, %u demotions settled the resident set to %zu "
		   "with cx 4..8 installed, census centre_x=%d from a (0,0) query; an "
		   "incoming correction took it back to (0,0), a deduped Packet10 "
		   "moved it nowhere, and the next Packet11 resumed at (%d,%d)\n",
		   ADM_F7_RADIUS, disk, s_r->demotions, s_r->resident_cols,
		   census.centre_x, adm_centre_x(), adm_centre_z());
}

/* ---- (LH) REL-MP-LOADHANG: a re-sent INSTALLED column needs no new reserve
 * Real-Wii run hw1021f mp_pause_menu_3m (tmp/jobs/hw1021f/LOADHANG-RCA.md):
 * the whole 9x9 admit disk (R=4) installed, carve free settled at ~4.1 MB,
 * then a vanilla server re-sent a region Packet51 for a column inside the
 * 3x3 safe core. The entry went RESIDENT -> RETAINED (voxels still
 * installed), and its re-promotion went through the carve gate, which wants
 * one column + the 6 MiB mesh reserve free -- never true again, so the
 * column stayed RETAINED for the rest of the session: the safe core was
 * never "resident", the loading screen never ended, and every server block
 * update to an installed column parked forever. PC's reserve is 0, so the
 * case injects the Wii figure. */
#define ADM_LH_RADIUS 4
#define ADM_LH_CARVE_FREE (4157u * 1024u) /* the run's flat mem2_carve_free_kb */
#define ADM_LH_WII_RESERVE (6u * 1024u * 1024u)
static void test_resent_installed_column_needs_no_reserve(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, -1);
	server_remote_set_admit_radius(&adm_owner, ADM_LH_RADIUS);
	/* The disk, except one COLD column at distance 3 that will arrive after
	 * the carve is tight (nearer than (4,-1) below, so a refused cold pick
	 * would sit in front of it in the nearest-first order). */
	size_t disk = (2 * ADM_LH_RADIUS + 1) * (2 * ADM_LH_RADIUS + 1);
	for(int cx = -ADM_LH_RADIUS; cx <= ADM_LH_RADIUS; cx++)
		for(int cz = -1 - ADM_LH_RADIUS; cz <= -1 + ADM_LH_RADIUS; cz++)
			if(!(cx == 3 && cz == -1))
				assert(adm_retain_terrain(cx, cz, cx, cz, 0));
	adm_pump_n((int)disk * 5);
	assert(s_r->resident_cols == disk - 1);
	assert(adm_count(REMOTE_CHUNK_RESIDENT) == disk - 1);

	/* The console's numbers: a 6 MiB reserve and a carve that holds one
	 * column comfortably but never column + reserve again. */
	server_remote_test_set_mesh_reserve_bytes(ADM_LH_WII_RESERVE);
	server_remote_test_set_carve_free_bytes(ADM_LH_CARVE_FREE);
	uint32_t deferred_before = s_r->promote_deferred_memory;

	assert(adm_retain_terrain(3, -1, 3, -1, 0)); /* cold, d=3 */
	adm_retain_partial(0, -1, 8, BLOCK_COBBLESTONE); /* core centre, d=0 */
	adm_retain_partial(4, -1, 8, BLOCK_COBBLESTONE); /* disk edge, d=4 */
	assert(adm_state(0, -1) == REMOTE_CHUNK_RETAINED && adm_installed(0, -1));
	/* A later single-block update to the re-bodied core column: it parks
	 * behind the partial and must apply once the partial does. */
	adm_edit(3, 100, -13, BLOCK_PLANKS);
	adm_pump_n(60);

	assert(adm_state(0, -1) == REMOTE_CHUNK_RESIDENT
		   && "a partial on an installed core column re-installs under carve "
			  "pressure: it allocates no new voxels");
	assert(adm_state(4, -1) == REMOTE_CHUNK_RESIDENT
		   && "and a refused COLD column nearer in the pick order does not "
			  "block it");
	assert(world_get_block(&gstate.world, 3, 2, -13).type == BLOCK_COBBLESTONE
		   && "the partial's blocks are in the world");
	assert(world_get_block(&gstate.world, 3, 100, -13).type == BLOCK_PLANKS
		   && "and the block update parked behind it applied");
	assert(adm_state(3, -1) == REMOTE_CHUNK_RETAINED && !adm_installed(3, -1)
		   && "the cold column still waits: the gate keeps its purpose");
	assert(s_r->promote_deferred_memory > deferred_before
		   && "and that refusal was counted");
	assert(s_r->resident_cols == disk - 1 && "no new voxel column was taken");
	struct server_remote_ready_census census;
	assert(server_remote_ready_census(&adm_owner, 0, -1, 2, &census));
	assert(census.core_resident && "the safe core is resident again");

	/* A FULL re-send of an installed column replaces it: the new column is
	 * allocated before the old one is released (world.c), so it needs ONE
	 * column of carve -- not column + mesh reserve, because the meshes it
	 * replaces are already accounted. */
	assert(adm_retain_terrain(1, -1, 2, -2, 0));
	adm_pump_n(20);
	assert(adm_state(1, -1) == REMOTE_CHUNK_RESIDENT
		   && "full re-send with one column of carve free installs");
	adm_fill_terrain(2, -2, adm_ref, 0);
	assert(adm_export(1, -1));
	assert(memcmp(adm_out, adm_ref, ADM_CELLS) == 0
		   && "and replaced the column's content");
	/* ...but a carve that cannot hold even the replacement keeps it waiting
	 * (the old voxels stay installed and drawn), until it can. */
	server_remote_test_set_carve_free_bytes(REMOTE_VOXEL_COLUMN_BYTES - 1u);
	deferred_before = s_r->promote_deferred_memory;
	assert(adm_retain_terrain(-1, -1, 2, -2, 0));
	adm_pump_n(20);
	assert(adm_state(-1, -1) == REMOTE_CHUNK_RETAINED && adm_installed(-1, -1));
	assert(s_r->promote_deferred_memory > deferred_before);
	server_remote_test_set_carve_free_bytes(REMOTE_VOXEL_COLUMN_BYTES);
	adm_pump_n(20);
	assert(adm_state(-1, -1) == REMOTE_CHUNK_RESIDENT);
	assert(s_r->resident_cols == disk - 1);
	assert(s_r->mp_end == 0 && !s_r->stop_now
		   && s_r->retention_allocation_failures == 0);

	server_remote_test_set_carve_free_bytes(SIZE_MAX);
	server_remote_test_set_mesh_reserve_bytes(SIZE_MAX);
	printf("PASS (LH) REL-MP-LOADHANG: %zu-column disk at %u B carve free "
		   "with a %u B mesh reserve; a partial re-send of core column "
		   "(0,-1) and of (4,-1) re-installed past a refused cold (3,-1), "
		   "the parked block update applied, core census resident; a full "
		   "re-send installs on one column of carve and waits below it\n",
		   disk - 1, (unsigned)ADM_LH_CARVE_FREE,
		   (unsigned)ADM_LH_WII_RESERVE);
}

int main(int argc, char** argv) {
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	assert(server_remote_init(&adm_owner));
	s_r = adm_owner.impl;
	/* W2: cases (a)-(f) are ADMISSION tests, not budget tests, so give them
	 * the largest reservation the clamp allows and let the budget cases below
	 * own the byte arithmetic. They need it: the pseudo-random fixture
	 * adm_fill_raw produces the worst-case body this suite has -- MEASURED
	 * avg 29,985 B (min 29,677 / max 30,287) per column, so the 441-column
	 * burst alone is 12.61 MiB and would not fit the 12 MiB PC default. (W1's
	 * own comment here said "a few KB each"; that was an estimate, and this
	 * is the measurement.) */
	server_remote_test_set_retained_byte_cap(&adm_owner, 16u * 1024u * 1024u);
	assert(mp_backing_configure(16u * 1024u * 1024u) == 16u * 1024u * 1024u);
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);
	if(argc > 1 && strcmp(argv[1], "--rel-mp-chained") == 0) {
		int rc = adm_rel_mp_cases();
		server_remote_destroy(&adm_owner);
		return rc;
	}
	if(argc > 1 && strcmp(argv[1], "--rel-mp-fix2") == 0) {
		int rc = test_vanilla_prebody_flood();
		server_remote_destroy(&adm_owner);
		return rc;
	}

	test_admission_gate();
	test_move_demotes_and_promotes();
	test_reconstruction_byte_exact();
	test_edits_survive();
	test_queued_body_across_demotion();
	test_demotion_vetoes();

	/* W2 budget cases; each starts from a fresh session over a reservation of
	 * its own size (adm_fresh_session), whose in_use == 0 assertion is also
	 * the leak check on the case before it. */
	test_burst_fits_wii_budget();
	test_poor_compression_burst_fits();
	test_over_budget_fails_cleanly();
	test_compaction();
	test_unload_returns_bytes();
	/* REL-MP (j2)/(j3): after (k), each on its own fresh session. */
	int rel_mp = adm_rel_mp_cases();
	test_partial_after_residency_demotes();

	/* F4 memory-aware promotion. Last, because they are the only cases that
	 * override the carve reading (a process-wide platform stand-in); each one
	 * puts it back to SIZE_MAX on the way out so nothing here is order-fragile
	 * if a case is added after them. */
	test_memory_gate_defers();
	test_moving_window_bound();

	/* F5a: the capacity latch through the real decoder, the reclaim-retry,
	 * the carve release, the deferred-log cliff and the scratch accounting.
	 * After the carve-reading cases on purpose -- (r) arms the carve
	 * OBSERVER hook and (t) reads the scratch budget, and both want the
	 * platform stand-ins back at their defaults first. */
	test_decoder_capacity_latch();
	test_fold_failed_decoder_alloc_preserves_reason();
	test_reclaim_then_retry();
	test_reservation_returns_to_carve();
	test_broad_edit_burst_no_kill();
	test_compaction_scans_past_ineligible_prefix();
	rel_mp |= test_vanilla_prebody_flood();
	test_fix2a_synthetic_base();
	test_fix2b_partial_only_install();
	test_fix2b_bodyless_refusal_and_ring();
	rel_mp |= test_fix3a_largest_victim();
	rel_mp |= test_fix3a_drain_to_low_water();
	rel_mp |= test_fix3a_forced_rescue_base_victim();
	test_scratch_is_accounted();

	/* F5b: the hard cap, the bounded veto, the login centre, the unlocked
	 * re-encode, and the five acceptance items review section 4 found
	 * uncovered. Each starts from its own adm_fresh_session, so they are
	 * order-independent and each one's in_use == 0 assertion is the leak
	 * check on the case before it. */
	test_hard_cap_survives_respawn();
	test_veto_is_bounded();
	test_centre_from_login_spawn();
	test_deflate_runs_unlocked();
	test_delayed_announcement();
	test_unload_of_resident_column();
	test_unload_refusal_is_retried();
	test_demotion_revert_on_refusal();
	test_dimension_swap_clears();
	test_no_traffic_no_admission();

	/* F7: the outbound half of the same one centre -- a sent Packet11/13
	 * moves it, an incoming correction still overrides it. Own fresh
	 * session, so it stays order-independent like the F5b cases above. */
	test_centre_follows_sent_position();

	/* REL-MP-LOADHANG: own fresh session; puts both platform overrides back. */
	test_resent_installed_column_needs_no_reserve();

	server_remote_destroy(&adm_owner);
	if(rel_mp) {
		printf("FAIL test_mp_admission: a REL-MP chained case did not pass\n");
		return 1;
	}
	printf("PASS test_mp_admission\n");
	return 0;
}
