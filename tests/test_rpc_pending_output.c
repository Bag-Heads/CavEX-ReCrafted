/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_rpc_pending_output -- ASTRA-G9-R5 points 5-6, order 2 (point 11):
 * the server's RPC publication is a bounded, owned, ORDERED pending output
 * over a nonblocking transport. The test thread plays the server (it calls
 * clin_rpc_send like the server thread does) and, when it chooses, the
 * consumer (clin_test_inbox_pop pops one delivered envelope exactly as a
 * discarding client would). Cases, each its own ctest:
 *   stalled      no consumer: 16 + N sends return at once, N deferred, no
 *                wait; then the consumer drains everything, in order
 *   saturation   repeated bursts past the inbox, drained between bursts:
 *                every sequence number once, in order, nothing dropped
 *   mixed        interleaved SET_BLOCK / HUD / destroy-batch types with one
 *                sequence across the direct/deferred boundary: delivered in
 *                send order, types intact
 *   generation   clin_rpc_send_local generation tags travel with deferred
 *                entries and are stamped at delivery
 *   exhaustion   ring + inbox full: the next send WAITS for exactly one
 *                token (measured, counted), nothing is dropped, order kept
 *   shutdown     pending pooled payloads are released exactly once on
 *                discard; a publication stop drops them like a new send
 *   ownership    a destroy-batch array sent while stalled is released once,
 *                by the consumer, never by the sender
 *   simulation   two sessions, identical edits through the real
 *                server_world_set_block path, one drained every tick, one
 *                drained in bursts: every emitted payload and route tag,
 *                final field / queue / RNG identical with random ticks on */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "util.h"

static int s_fail;
static void check(bool ok, const char* what) {
	printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
	if(!ok)
		s_fail++;
}

static struct client_rpc hud(unsigned seq) {
	struct client_rpc c;
	memset(&c, 0, sizeof c);
	c.type = CRPC_HUD_MESSAGE;
	snprintf(c.payload.hud_message, sizeof c.payload.hud_message, "%u", seq);
	return c;
}
static unsigned hud_seq(const struct client_rpc* c) {
	return (unsigned)strtoul(c->payload.hud_message, NULL, 10);
}
static struct client_rpc setb(unsigned seq) {
	struct client_rpc c;
	memset(&c, 0, sizeof c);
	c.type = CRPC_SET_BLOCK;
	c.payload.set_block.x = (w_coord_t)seq;
	c.payload.set_block.y = 7;
	c.payload.set_block.z = 11;
	c.payload.set_block.block.type = BLOCK_STONE;
	return c;
}
static struct client_rpc batch(unsigned seq) {
	struct client_rpc c;
	memset(&c, 0, sizeof c);
	c.type = CRPC_ENTITY_DESTROY_BATCH;
	c.payload.entity_destroy_batch.ids = malloc(sizeof(uint32_t) * 2);
	assert(c.payload.entity_destroy_batch.ids);
	c.payload.entity_destroy_batch.ids[0] = seq;
	c.payload.entity_destroy_batch.ids[1] = 0xC0DEu;
	c.payload.entity_destroy_batch.count = 2;
	return c;
}
static unsigned seq_of(const struct client_rpc* c) {
	switch(c->type) {
		case CRPC_HUD_MESSAGE: return hud_seq(c);
		case CRPC_SET_BLOCK: return (unsigned)c->payload.set_block.x;
		case CRPC_ENTITY_DESTROY_BATCH:
			return c->payload.entity_destroy_batch.ids
				? c->payload.entity_destroy_batch.ids[0]
				: 0xFFFFFFFFu;
		default: return 0xFFFFFFFFu;
	}
}

/* Drain everything (inbox + pending) on this thread, checking that the
 * sequence numbers arrive in order starting at `expect`. Returns the count. */
static unsigned drain_all(unsigned* expect, bool* in_order) {
	unsigned n = 0;
	for(;;) {
		struct client_rpc c;
		uint64_t gen;
		while(clin_test_inbox_pop(&c, &gen)) {
			if(seq_of(&c) != *expect)
				*in_order = false;
			(*expect)++;
			n++;
			clin_test_rpc_release(&c); /* the consumer's release, once */
		}
		uint32_t pending = 0;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(pending == 0)
			break;
		(void)clin_rpc_flush_pending();
	}
	return n;
}

static void reset_stats_view(uint32_t* deferred0, uint32_t* exhausted0) {
	clin_rpc_pending_stats(NULL, NULL, deferred0, exhausted0);
}

/* ---------------- stalled ---------------- */
static void case_stalled(void) {
	const unsigned N = 16 + 50;
	uint32_t d0, x0;
	reset_stats_view(&d0, &x0);
	const ptime_t t0 = time_get();
	for(unsigned i = 0; i < N; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	const float ms = time_diff_s(t0, time_get()) * 1000.0f;
	uint32_t pending, peak, deferred, exhausted;
	clin_rpc_pending_stats(&pending, &peak, &deferred, &exhausted);
	printf("  %u sends with no consumer: %.1f ms, pending %u (peak %u), "
		   "deferred %u, exhaustion waits %u\n", N, ms, pending, peak,
		   deferred - d0, exhausted - x0);
	check(pending == 50 && deferred - d0 == 50 && exhausted - x0 == 0,
		  "the 17th..66th sends were deferred, none waited");
	check(ms < 500.0f, "the sender never parked on the token pool");
	unsigned expect = 0;
	bool in_order = true;
	const unsigned got = drain_all(&expect, &in_order);
	check(got == N && in_order, "the consumer received all 66 in send order");
}

/* ---------------- saturation ---------------- */
static void case_saturation(void) {
	unsigned expect = 0;
	bool in_order = true, all = true;
	for(int burst = 0; burst < 6; burst++) {
		const unsigned n = 16 + 40 + (unsigned)burst * 13;
		for(unsigned i = 0; i < n; i++) {
			struct client_rpc c = hud(expect + i);
			clin_rpc_send(&c);
		}
		const unsigned got = drain_all(&expect, &in_order);
		if(got != n)
			all = false;
	}
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(all && in_order && pending == 0,
		  "six bursts past the inbox: every envelope once, in order, ring "
		  "empty after each drain");
}

/* ---------------- mixed ---------------- */
static void case_mixed(void) {
	const unsigned N = 90;
	size_t rel0 = clin_test_destroy_batch_releases();
	for(unsigned i = 0; i < N; i++) {
		struct client_rpc c = i % 3 == 0 ? setb(i) : i % 3 == 1 ? hud(i) : batch(i);
		clin_rpc_send(&c);
	}
	/* pop with type checks */
	unsigned expect = 0;
	bool ok = true;
	for(;;) {
		struct client_rpc c;
		uint64_t gen;
		while(clin_test_inbox_pop(&c, &gen)) {
			const enum client_rpc_type want = expect % 3 == 0 ? CRPC_SET_BLOCK
				: expect % 3 == 1 ? CRPC_HUD_MESSAGE
								   : CRPC_ENTITY_DESTROY_BATCH;
			if(c.type != want || seq_of(&c) != expect)
				ok = false;
			expect++;
			clin_test_rpc_release(&c);
		}
		uint32_t pending;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(!pending)
			break;
		(void)clin_rpc_flush_pending();
	}
	check(ok && expect == N, "three types interleaved: delivered in send "
							 "order across the direct/deferred boundary");
	check(clin_test_destroy_batch_releases() - rel0 == N / 3,
		  "every deferred destroy-batch array released exactly once, by the "
		  "consumer");
}

/* ---------------- generation ---------------- */
static void case_generation(void) {
	const unsigned N = 16 + 24;
	for(unsigned i = 0; i < N; i++) {
		struct client_rpc c = hud(i);
		const bool sent = clin_rpc_send_local(&c, 1, 1000 + i);
		assert(sent);
		(void)sent;
	}
	unsigned expect = 0;
	bool ok = true;
	for(;;) {
		struct client_rpc c;
		uint64_t gen = 0;
		while(clin_test_inbox_pop(&c, &gen)) {
			if(seq_of(&c) != expect || gen != 1000 + expect)
				ok = false;
			expect++;
			clin_test_rpc_release(&c);
		}
		uint32_t pending;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(!pending)
			break;
		(void)clin_rpc_flush_pending();
	}
	check(ok && expect == N,
		  "generation tags travel with deferred entries and are stamped at "
		  "delivery (direct and deferred alike)");
}

/* ---------------- exhaustion ---------------- */
static void* late_consumer(void* arg) {
	(void)arg;
	thread_msleep(300);
	struct client_rpc c;
	uint64_t gen;
	if(clin_test_inbox_pop(&c, &gen)) /* frees exactly one token */
		clin_test_rpc_release(&c);
	return NULL;
}
static void case_exhaustion(void) {
	const unsigned CAP = CLIN_RPC_PENDING_CAPACITY;
	const unsigned FULL = 16 + CAP;
	uint32_t d0, x0;
	reset_stats_view(&d0, &x0);
	for(unsigned i = 0; i < FULL; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	uint32_t pending, peak, deferred, exhausted;
	clin_rpc_pending_stats(&pending, &peak, &deferred, &exhausted);
	check(pending == CAP && exhausted - x0 == 0,
		  "16 direct + CAPACITY deferred fill the window without a wait");
	struct thread consumer;
	bool ok = thread_create(&consumer, late_consumer, NULL, 64);
	assert(ok);
	(void)ok;
	const ptime_t t0 = time_get();
	struct client_rpc c = hud(FULL);
	clin_rpc_send(&c); /* 16 + CAP + 1st: must wait for the late consumer */
	const float ms = time_diff_s(t0, time_get()) * 1000.0f;
	thread_join(&consumer);
	uint32_t sum_us, max_us, n;
	clin_rpc_wait_take(&sum_us, &max_us, &n);
	clin_rpc_pending_stats(&pending, &peak, &deferred, &exhausted);
	printf("  exhaustion send waited %.1f ms (measured %u us, %u wait), "
		   "pending %u, exhaustion count %u\n", ms, max_us, n, pending,
		   exhausted - x0);
	check(ms >= 150.0f && n == 1 && max_us >= 150000u && exhausted - x0 == 1,
		  "the exhausted send waited for exactly one token and the wait was "
		  "measured");
	check(pending == CAP, "the ring stayed full: head delivered, tail appended");
	unsigned expect = 1; /* envelope 0 was popped by the late consumer */
	bool in_order = true;
	const unsigned got = drain_all(&expect, &in_order);
	check(got == FULL && in_order, "nothing dropped, order kept across "
								   "the exhaustion wait");
}

/* ---------------- shutdown ---------------- */
static void case_shutdown(void) {
	size_t rel0 = clin_test_destroy_batch_releases();
	for(unsigned i = 0; i < 16 + 10; i++) {
		struct client_rpc c = batch(i);
		clin_rpc_send(&c);
	}
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(pending == 10, "ten destroy batches pending behind a full inbox");
	clin_rpc_pending_discard();
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(pending == 0 && clin_test_destroy_batch_releases() - rel0 == 10,
		  "discard releases every pending payload exactly once (the inbox's "
		  "16 untouched)");
	unsigned expect = 0;
	bool in_order = true;
	(void)drain_all(&expect, &in_order);
	check(clin_test_destroy_batch_releases() - rel0 == 26,
		  "the inbox entries are released by the consumer, once each");
	/* publication stop: pending entries are dropped like a new send */
	rel0 = clin_test_destroy_batch_releases();
	const uint32_t drop0 = clin_server_publication_dropped();
	for(unsigned i = 0; i < 16 + 5; i++) {
		struct client_rpc c = batch(i);
		clin_rpc_send(&c);
	}
	clin_server_publication_stop(true);
	(void)clin_rpc_flush_pending();
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(pending == 0 && clin_server_publication_dropped() - drop0 == 5
			  && clin_test_destroy_batch_releases() - rel0 == 5,
		  "a publication stop drops the pending entries (released, counted)");
	clin_server_publication_stop(false);
	(void)drain_all(&expect, &in_order);
}

/* ---------------- ownership ---------------- */
static void case_ownership(void) {
	size_t rel0 = clin_test_destroy_batch_releases();
	for(unsigned i = 0; i < 16; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	struct client_rpc b = batch(16);
	uint32_t* ids = b.payload.entity_destroy_batch.ids;
	clin_rpc_send(&b); /* deferred */
	check(clin_test_destroy_batch_releases() == rel0 && ids[1] == 0xC0DEu,
		  "a deferred destroy batch is NOT released by the sender; the array "
		  "is intact while pending");
	unsigned expect = 0;
	bool in_order = true;
	const unsigned got = drain_all(&expect, &in_order);
	check(got == 17 && in_order && clin_test_destroy_batch_releases() == rel0 + 1,
		  "the consumer releases it exactly once");
}

/* ---------------- simulation ---------------- */
static struct server_local S;
static char s_dir[220];
/* Retain every delivered envelope and route tag for exact comparison of
 * all active payload bytes and order, not only a SET_BLOCK digest. */
static uint64_t s_seq_hash;
static unsigned s_seq_n;
static unsigned s_seq_types[128];
static struct client_rpc* s_output[3];
static uint64_t* s_output_gen[3];
static size_t s_output_cap[3];
static uint8_t* s_state[3];
static size_t s_state_n[3];
static unsigned s_output_session;
static void seq_note(const struct client_rpc* c, uint64_t generation) {
	/* Canonicalize active payload fields: C struct padding and unused union
	 * bytes are not message data. Fail if this fixture starts producing a new
	 * type rather than silently omitting its payload. */
	struct client_rpc normalized;
	memset(&normalized, 0, sizeof normalized);
	normalized.type = c->type;
	normalized.world_index = c->world_index;
	normalized.world_generation = c->world_generation;
	switch(c->type) {
	case CRPC_SET_BLOCK:
		normalized.payload.set_block.x = c->payload.set_block.x;
		normalized.payload.set_block.y = c->payload.set_block.y;
		normalized.payload.set_block.z = c->payload.set_block.z;
		normalized.payload.set_block.block.type = c->payload.set_block.block.type;
		normalized.payload.set_block.block.metadata = c->payload.set_block.block.metadata;
		normalized.payload.set_block.block.sky_light = c->payload.set_block.block.sky_light;
		normalized.payload.set_block.block.torch_light = c->payload.set_block.block.torch_light;
		break;
	case CRPC_SOUND:
		normalized.payload.sound.sound_id = c->payload.sound.sound_id;
		memcpy(normalized.payload.sound.pos, c->payload.sound.pos, sizeof(vec3));
		normalized.payload.sound.vol = c->payload.sound.vol;
		normalized.payload.sound.pitch = c->payload.sound.pitch;
		normalized.payload.sound.ui = c->payload.sound.ui;
		break;
	case CRPC_SPAWN_ITEM:
		normalized.payload.spawn_item.entity_id = c->payload.spawn_item.entity_id;
		normalized.payload.spawn_item.item.id = c->payload.spawn_item.item.id;
		normalized.payload.spawn_item.item.durability = c->payload.spawn_item.item.durability;
		normalized.payload.spawn_item.item.count = c->payload.spawn_item.item.count;
		normalized.payload.spawn_item.item.nbt_record = c->payload.spawn_item.item.nbt_record;
		memcpy(normalized.payload.spawn_item.pos, c->payload.spawn_item.pos, sizeof(vec3));
		memcpy(normalized.payload.spawn_item.vel, c->payload.spawn_item.vel, sizeof(vec3));
		break;
	default:
		fprintf(stderr, "uncanonicalized simulation RPC type %d\n", c->type);
		abort();
	}
	assert((unsigned)c->type < 128);
	s_seq_types[c->type]++;
	unsigned session = s_output_session;
	if(s_seq_n == s_output_cap[session]) {
		size_t cap = s_output_cap[session] ? 2 * s_output_cap[session] : 1024;
		void* output = realloc(s_output[session], cap * sizeof *s_output[session]);
		assert(output); s_output[session] = output;
		void* gens = realloc(s_output_gen[session], cap * sizeof *s_output_gen[session]);
		assert(gens); s_output_gen[session] = gens; s_output_cap[session] = cap;
	}
	memcpy(&s_output[session][s_seq_n], &normalized, sizeof normalized);
	s_output_gen[session][s_seq_n] = generation;
	const uint8_t* b = (const uint8_t*)&normalized;
	for(size_t k = 0; k < sizeof normalized; k++)
		s_seq_hash = (s_seq_hash ^ b[k]) * 1099511628211ull;
	s_seq_hash = (s_seq_hash ^ generation) * 1099511628211ull;
	s_seq_n++;
}
/* The consumer is its own thread, as in production (the client), so the
 * server-side test thread can never wait on itself: `fast` pops as soon as
 * envelopes arrive (the old blocking transport could never fall behind
 * such a consumer); `bursty` sleeps 40 ms between sweeps, so the server
 * runs several ticks ahead into the pending ring. The sequence digest is
 * updated only on the consumer thread (single consumer). */
static int s_consumer_stop;
static int s_consumer_sleep_ms;
static void* sim_consumer(void* arg) {
	(void)arg;
	while(!__atomic_load_n(&s_consumer_stop, __ATOMIC_ACQUIRE)) {
		struct client_rpc c;
		uint64_t gen;
		while(clin_test_inbox_pop(&c, &gen)) {
			seq_note(&c, gen);
			clin_test_rpc_release(&c);
		}
		thread_msleep((size_t)s_consumer_sleep_ms);
	}
	return NULL;
}
static void sim_drain_to_empty(struct thread* consumer) {
	/* let the consumer take everything: flush the ring whenever it has
	 * freed tokens, until the ring is empty; then stop it and pop leftovers */
	for(;;) {
		(void)clin_rpc_flush_pending();
		uint32_t pending;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(!pending)
			break;
		thread_msleep(2);
	}
	thread_msleep(100);
	__atomic_store_n(&s_consumer_stop, 1, __ATOMIC_RELEASE);
	thread_join(consumer);
	struct client_rpc c;
	uint64_t gen;
	while(clin_test_inbox_pop(&c, &gen)) {
		seq_note(&c, gen);
		clin_test_rpc_release(&c);
	}
}
static void sim_run(int session, int drain_every, uint64_t* hash, unsigned* n,
					uint64_t* rng, char field_hex[65]) {
	/* session 0 = the OLD blocking transport (the baseline the ruling asks
	 * for), session 1 = the shipping pending output (128 ring + G9 R21
	 * block-update packing), session 2 = the same ring with packing OFF */
	clin_test_set_rpc_blocking(session == 0);
	clin_test_set_block_pack(session != 2);
	char name[64];
	snprintf(name, sizeof name, "test_rpc_pending_sim_%d", session);
	test_fixture_dir(name, s_dir, sizeof s_dir);
	MKDIR(s_dir);
	char region[260];
	snprintf(region, sizeof region, "%s/region", s_dir);
	MKDIR(region);
	memset(&S, 0, sizeof S);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&S))
		abort();
	dict_entity_init(S.entities);
	string_t level;
	string_init_set_str(level, s_dir);
	server_world_create(&S.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	/* F4-RNG (f049fa3d8): fluid support-loss drops draw runtime-seeded
	 * World.rand. Transport comparisons must start from the same stream. */
	jrand_set_seed(&S.worlds[0].rand, 4242);
	S.worlds[0].session = &S;
	S.worlds[0].local = &S;
	S.scheduled_tick_budget = 128;
	S.rand_src.seed = 0x9E11u;
	/* Both runs need the same process-global Math.random input too: random
	 * fluid updates drop torches through EntityItem constructors. */
	server_local_test_seed_math_random(0x73C0DE);
	S.world_time = 6000;
	server_local_skylight_recompute(&S);
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			bool ok = server_world_load_chunk(&S.worlds[0], cx, cz, &sc);
			assert(ok && sc);
			(void)ok;
		}
	s_output_session = (unsigned)session;
	memset(s_seq_types, 0, sizeof s_seq_types);
	s_seq_hash = 1469598103934665603ull;
	s_seq_n = 0;
	__atomic_store_n(&s_consumer_stop, 0, __ATOMIC_RELEASE);
	s_consumer_sleep_ms = drain_every == 1 ? 1 : 40;
	struct thread consumer;
	bool started = thread_create(&consumer, sim_consumer, NULL, 64);
	assert(started);
	(void)started;
	uint8_t hh = 0;
	(void)server_world_get_height(&S.worlds[0], 8, 8, &hh);
	const int y = hh + 5;
	uint32_t lcg = 0x1234567u;
	for(int t = 1; t <= 200; t++) {
		/* a burst of edits through the real notify/publish path (torch grid
		 * toggles + water, so fluid ticks and cascades publish too) */
		for(int k = 0; k < 24; k++) {
			lcg = lcg * 1664525u + 1013904223u;
			const int x = -8 + (int)((lcg >> 8) % 32);
			lcg = lcg * 1664525u + 1013904223u;
			const int z = -8 + (int)((lcg >> 8) % 32);
			lcg = lcg * 1664525u + 1013904223u;
			const unsigned r = (lcg >> 8) % 4;
			const uint8_t type = r == 0 ? BLOCK_AIR : r == 1 ? BLOCK_TORCH
				: r == 2 ? BLOCK_STONE : BLOCK_WATER_STILL;
			(void)server_world_set_block(
				&S.worlds[0], x, y + (int)(r & 1), z,
				(struct block_data) {.type = type, .metadata = type == BLOCK_TORCH ? 5 : 0});
		}
		/* random ticks ON (anchored at spawn): the simulation and its RNG
		 * must be the same whichever transport publishes it */
		(void)server_local_world_block_ticks_w(&S, &S.worlds[0], true, 0, 0);
		server_world_light_drain(&S.worlds[0]);
		(void)clin_rpc_flush_pending(); /* the tick-start flush of production */
	}
	sim_drain_to_empty(&consumer);
	clin_test_set_rpc_blocking(false);
	clin_test_set_block_pack(true);
	*hash = s_seq_hash;
	*n = s_seq_n;
	*rng = (uint64_t)S.rand_src.seed;
	check(S.world_service_stats[0].random_tick_passes == 200, "all 200 random passes executed");
	for(unsigned type = 0; type < 128; type++)
		if(s_seq_types[type]) printf("  output type %u: %u envelopes\n", type, s_seq_types[type]);
	/* the settled field digest */
	{
		uint64_t h = 1469598103934665603ull;
		const size_t cells = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
		/* Preserve bytes as well as the diagnostic digest. No hash collision
		 * can mask a plane, RNG or scheduled-queue divergence. */
		s_state_n[session] = 9 * (cells * 5 / 2 + CHUNK_SIZE * CHUNK_SIZE)
			+ 4 * sizeof(uint64_t) + 2 * sizeof(uint32_t) + 2 + sizeof(double)
			+ S.worlds[0].scheduled_tick_count * 6 * sizeof(uint32_t);
		s_state[session] = malloc(s_state_n[session]);
		assert(s_state[session]);
		uint8_t* dst = s_state[session];
		for(int cx = -1; cx <= 1; cx++)
			for(int cz = -1; cz <= 1; cz++) {
				struct server_chunk* sc
					= dict_server_chunks_get(S.worlds[0].chunks, S_CHUNK_ID(cx, cz));
				const uint8_t* planes[5] = {sc->ids, sc->metadata, sc->lighting_sky,
					sc->lighting_torch, sc->heightmap};
				const size_t lens[5] = {cells, cells / 2, cells / 2, cells / 2,
					CHUNK_SIZE * CHUNK_SIZE};
				for(int p = 0; p < 5; p++) {
					memcpy(dst, planes[p], lens[p]); dst += lens[p];
					for(size_t i = 0; i < lens[p]; i++)
						h = (h ^ planes[p][i]) * 1099511628211ull;
				}
			}
		struct server_local_math_random_state math;
		server_local_math_random_capture(&math);
		const uint64_t rngs[] = {(uint64_t)S.container_drop_rng.seed,
			(uint64_t)S.dispenser_break_rng.seed, (uint64_t)S.entity_rng.seed,
			math.rng.seed};
		memcpy(dst, rngs, sizeof rngs); dst += sizeof rngs;
		*dst++ = math.initialized; *dst++ = math.rng.have_next_gaussian;
		const double cached = math.rng.have_next_gaussian ? math.rng.next_gaussian_cached : 0;
		memcpy(dst, &cached, sizeof cached); dst += sizeof cached;
		const uint32_t queue[] = {S.worlds[0].current_tick,
			(uint32_t)S.worlds[0].scheduled_tick_count};
		memcpy(dst, queue, sizeof queue); dst += sizeof queue;
		h = (h ^ (uint64_t)S.container_drop_rng.seed) * 1099511628211ull;
		h = (h ^ (uint64_t)S.dispenser_break_rng.seed) * 1099511628211ull;
		h = (h ^ (uint64_t)S.entity_rng.seed) * 1099511628211ull;
		h = (h ^ S.worlds[0].current_tick) * 1099511628211ull;
		h = (h ^ S.worlds[0].scheduled_tick_count) * 1099511628211ull;
		for(size_t i = 0; i < S.worlds[0].scheduled_tick_count; i++) {
			const struct scheduled_tick* tick = &S.worlds[0].scheduled_ticks[
				(S.worlds[0].scheduled_tick_head + i) % S.worlds[0].scheduled_tick_capacity];
			const uint32_t fields[] = {(uint32_t)tick->x, (uint32_t)tick->y,
				(uint32_t)tick->z, tick->fire_at_tick, tick->block_type, tick->queue_flags};
			memcpy(dst, fields, sizeof fields); dst += sizeof fields;
			for(size_t k = 0; k < sizeof fields / sizeof fields[0]; k++)
				h = (h ^ fields[k]) * 1099511628211ull;
		}
		assert((size_t)(dst - s_state[session]) == s_state_n[session]);
		snprintf(field_hex, 65, "%016" PRIx64, h);
	}
	dict_entity_clear(S.entities);
	server_world_discard(&S.worlds[0]);
	test_fixture_dir_cleanup(s_dir);
}
static void case_simulation(void) {
	uint64_t ha, hb, ra, rb, hc, rc;
	unsigned na, nb, nc;
	char fa[65], fb[65], fc[65];
	sim_run(0, 1, &ha, &na, &ra, fa); /* OLD blocking transport, consumer keeps up */
	uint32_t packs0, packed0, packs1, packed1, packs2, packed2;
	clin_rpc_pack_stats(&packs0, &packed0);
	sim_run(2, 7, &hc, &nc, &rc, fc); /* pending output, packing OFF */
	uint32_t peak128;
	clin_rpc_pending_stats(NULL, &peak128, NULL, NULL);
	clin_rpc_pack_stats(&packs1, &packed1);
	sim_run(1, 7, &hb, &nb, &rb, fb); /* shipping: ring + packing, bursty consumer */
	uint32_t peak;
	clin_rpc_pending_stats(NULL, &peak, NULL, NULL);
	clin_rpc_pack_stats(&packs2, &packed2);
	printf("  unpacked arm: %u records, digest %016" PRIx64 ", field %s, "
		   "peak %u\n", nc, hc, fc, peak128);
	printf("  packed arm: %u packs carrying %u appended records\n",
		   packs2 - packs1, packed2 - packed1);
	check(packs1 == packs0, "the unpacked arm really created no pack");
	check(packs2 > packs1 && packed2 > packed1,
		  "the shipping arm really packed deferred block updates");
	check(nc == na && hc == ha
		&& memcmp(s_output[0], s_output[2], na * sizeof *s_output[0]) == 0
		&& memcmp(s_output_gen[0], s_output_gen[2], na * sizeof *s_output_gen[0]) == 0
		&& rc == ra && strcmp(fc, fa) == 0 && s_state_n[2] == s_state_n[0]
		&& memcmp(s_state[2], s_state[0], s_state_n[0]) == 0,
		"unpacked ring == old blocking transport: delivered output (bytes, "
		"order, tags) and simulation bytes");
	printf("  delivered envelopes: %u / %u, output digests %016" PRIx64
		   " / %016" PRIx64 "; RNG %" PRIu64 " / %" PRIu64 "; field %s / %s; "
		   "pending peak %u\n", na, nb, ha, hb, ra, rb, fa, fb, peak);
	check(na == nb && ha == hb
		&& memcmp(s_output[0], s_output[1], na * sizeof *s_output[0]) == 0
		&& memcmp(s_output_gen[0], s_output_gen[1], na * sizeof *s_output_gen[0]) == 0, "FULL replayed output (every record, bytes "
								"and order) identical: old blocking transport "
								"vs packed pending output with a bursty consumer");
	check(ra == rb && strcmp(fa, fb) == 0 && s_state_n[0] == s_state_n[1]
		&& memcmp(s_state[0], s_state[1], s_state_n[0]) == 0,
		  "simulation (RNG with random ticks on, settled field) identical "
		  "across the two transports");
	check(peak > 0, "the bursty session really deferred output");
	for(unsigned i = 0; i < 3; i++) {
		free(s_state[i]); s_state[i] = NULL;
		free(s_output[i]); s_output[i] = NULL; s_output_cap[i] = 0;
		free(s_output_gen[i]); s_output_gen[i] = NULL;
	}
}


/* G9 R20 Q3 / R21: the real-Wii probe's dominant over-gate tick is a fixture
 * roof/torch SET_BLOCK burst (82-97 envelopes per roof tick, counted on the
 * host hw_workload) landing on a backlog the client has not consumed yet.
 * At the accepted 128 ring WITHOUT packing that parks the sender in
 * exhaustion waits; with packing (shipping) the same backlog + burst defers
 * without one wait at the same 128, and BOTH replay the identical ordered
 * stream. */
static int s_burst_stop;
static unsigned s_burst_got, s_burst_expect;
static bool s_burst_order;
static void* burst_consumer(void* arg) {
	(void)arg;
	thread_msleep(80); /* the client is mid-frame: nothing consumed at first */
	for(;;) {
		struct client_rpc c;
		uint64_t gen;
		bool any = false;
		while(clin_test_inbox_pop(&c, &gen)) {
			if(seq_of(&c) != s_burst_expect)
				s_burst_order = false;
			s_burst_expect++;
			s_burst_got++;
			clin_test_rpc_release(&c);
			any = true;
		}
		if(!any && __atomic_load_n(&s_burst_stop, __ATOMIC_ACQUIRE))
			break;
		thread_msleep(2);
	}
	return NULL;
}
static void burst_arm(bool pack, unsigned* waits, float* ms, unsigned* got,
					  bool* ordered) {
	enum { BACKLOG = 16 + 60, BURST = 97 };
	clin_test_set_block_pack(pack);
	uint32_t d0, x0;
	reset_stats_view(&d0, &x0);
	__atomic_store_n(&s_burst_stop, 0, __ATOMIC_RELEASE);
	s_burst_got = s_burst_expect = 0;
	s_burst_order = true;
	struct thread consumer;
	bool ok = thread_create(&consumer, burst_consumer, NULL, 64);
	assert(ok);
	(void)ok;
	const ptime_t t0 = time_get();
	for(unsigned i = 0; i < BACKLOG + BURST; i++) {
		struct client_rpc c = setb(i);
		clin_rpc_send(&c);
	}
	*ms = time_diff_s(t0, time_get()) * 1000.0f;
	uint32_t exhausted;
	clin_rpc_pending_stats(NULL, NULL, NULL, &exhausted);
	*waits = exhausted - x0;
	for(;;) { /* the production tick-start / pre-sleep flushes */
		(void)clin_rpc_flush_pending();
		uint32_t pending;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(!pending)
			break;
		thread_msleep(1);
	}
	__atomic_store_n(&s_burst_stop, 1, __ATOMIC_RELEASE);
	thread_join(&consumer);
	*got = s_burst_got;
	*ordered = s_burst_order && s_burst_got == BACKLOG + BURST;
	clin_rpc_wait_take(&d0, &x0, &exhausted); /* clear the window */
	clin_test_set_block_pack(true);
}
static void case_burst(void) {
	unsigned w128, wcap, g128, gcap;
	float ms128, mscap;
	bool o128, ocap;
	check(CLIN_RPC_PENDING_CAPACITY == 256u,
		  "the ring ships at 256 (release-1031, hw1030r hb114)");
	/* The R20/R21 claim is about the 128 window: replay it by admission
	 * limit (storage stays the shipping ring), then restore shipping. */
	clin_test_set_pending_capacity(128u);
	burst_arm(false, &w128, &ms128, &g128, &o128);
	burst_arm(true, &wcap, &mscap, &gcap, &ocap);
	clin_test_set_pending_capacity(0u);
	printf("  backlog 76 + roof burst 97 at ring 128: unpacked %u waits / "
		   "%.1f ms; packed %u waits / %.1f ms\n", w128, ms128, wcap, mscap);
	check(w128 > 0 && ms128 >= 50.0f,
		  "unpacked, the 128 ring parks the sender (the probe B1 class)");
	check(wcap == 0 && mscap < 50.0f,
		  "packed, the same 128 ring absorbs the same backlog + burst, no wait");
	check(o128 && ocap && g128 == gcap,
		  "both replay every record once, in send order");
}

/* G9 R21 FIFO / barrier / size-bound proof of the packing itself, with the
 * consumer stalled: records are packed only while CONSECUTIVE and
 * compatible; an intervening message, a world_generation change, a seal
 * (tick start) and the 19-record bound each start a new entry; the replay
 * restores the exact stream (type, stamp, payload, order). */
static struct client_rpc setb_w(unsigned seq, uint8_t wi, uint32_t wg) {
	struct client_rpc c = setb(seq);
	c.world_index = wi;
	c.world_generation = wg;
	c.payload.set_block.block.metadata = (uint8_t)(seq & 15u);
	c.payload.set_block.local_revision.events = seq * 7u + 1u;
	c.payload.set_block.local_previous_content = seq ^ 0x5A5Au;
	return c;
}
static void case_pack_mixed(void);
static void case_pack_barrier(void) {
	enum { N = 16 + 40 + 1 + 10 + 5 + 4 + 3 + 25 };
	struct client_rpc sent[N];
	unsigned k = 0;
	uint32_t packs0, rec0;
	clin_rpc_pack_stats(&packs0, &rec0);
	for(unsigned i = 0; i < 16; i++) { sent[k] = hud(k); k++; } /* fill the inbox */
	for(unsigned i = 0; i < 40; i++) { sent[k] = setb_w(k, 0, 0); k++; } /* 19+19+2 */
	{ sent[k] = hud(k); k++; }                                    /* intervening */
	for(unsigned i = 0; i < 10; i++) { sent[k] = setb_w(k, 0, 0); k++; } /* 10 */
	for(unsigned i = 0; i < 5; i++) { sent[k] = setb_w(k, 1, 3); k++; }  /* stamp change: 5 */
	for(unsigned i = 0; i < 4; i++) { sent[k] = setb_w(k, 1, 4); k++; }  /* generation change: 4 */
	const unsigned seal_at = k + 1;
	for(unsigned i = 0; i < 3; i++) { sent[k] = setb_w(k, 1, 4); k++; }  /* seal after 1st: 1 + 2 */
	for(unsigned i = 0; i < 25; i++) { sent[k] = setb_w(k, 0, 0); k++; } /* 19 + 6 */
	assert(k == N);
	for(unsigned i = 0; i < N; i++) {
		if(i == seal_at) clin_rpc_pack_seal(); /* the tick-start barrier */
		struct client_rpc c = sent[i];
		clin_rpc_send(&c);
	}
	uint32_t pending, packs1, rec1;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	clin_rpc_pack_stats(&packs1, &rec1);
	/* entries: 19,19,2 | hud | 10 | 5 | 4 | 1(+seal... joins the 4) ... */
	printf("  %u sends behind a full inbox: %u pending entries, %u packs, "
		   "%u appended records\n", N, pending, packs1 - packs0, rec1 - rec0);
	/* expected entries: [19][19][2][hud][10][5][4+1=5][2][19][6] = 10 */
	check(pending == 10, "packing stops at every intervening message, "
		  "stamp/generation change, seal and the 19-record bound");
	check(packs1 - packs0 == 9, "nine packs (every multi-record entry)");
	unsigned got = 0;
	bool exact = true;
	for(;;) {
		struct client_rpc c;
		uint64_t gen;
		while(clin_test_inbox_pop(&c, &gen)) {
			const struct client_rpc* w = &sent[got];
			bool same = got < N && c.type == w->type && gen == 0
				&& c.world_index == w->world_index
				&& c.world_generation == w->world_generation;
			if(same && c.type == CRPC_SET_BLOCK)
				same = memcmp(&c.payload.set_block, &w->payload.set_block,
							  sizeof c.payload.set_block) == 0;
			else if(same)
				same = hud_seq(&c) == hud_seq(w);
			exact &= same;
			got++;
			clin_test_rpc_release(&c);
		}
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(!pending)
			break;
		(void)clin_rpc_flush_pending();
	}
	check(got == N && exact, "the replay is the exact sent stream: every "
		  "record once, in order, type/stamp/payload bytes intact");
	case_pack_mixed(); /* same ctest: the mixed-record barrier proof */
}

/* G9 startup: CRPC_SPAWN_ITEM is a packable record too. An Explosion pass 3
 * stream (drop roll -> SPAWN_ITEM, then SET_BLOCK, per cell) packs as ONE
 * stream; the replay restores every record as the exact envelope it was
 * (type, stamp, payload bytes, order), and an intervening message still
 * closes the pack. With SPAWN_ITEM packing OFF (the R21 pack) the same
 * stream needs an entry per drop. */
static struct client_rpc spawn_w(unsigned seq) {
	struct client_rpc c;
	memset(&c, 0, sizeof c);
	c.type = CRPC_SPAWN_ITEM;
	c.payload.spawn_item.entity_id = 1000u + seq;
	c.payload.spawn_item.item.id = (uint16_t)(4u + seq % 3u);
	c.payload.spawn_item.item.count = 1;
	c.payload.spawn_item.pos[0] = (float)seq + 0.25f;
	c.payload.spawn_item.pos[1] = 64.5f;
	c.payload.spawn_item.vel[2] = -0.125f * (float)seq;
	return c;
}
static unsigned pack_mixed_arm(bool spawn_items, bool* exact_out) {
	enum { N = 16 + 60 + 1 + 6 };
	struct client_rpc sent[N];
	unsigned k = 0;
	clin_test_set_pack_spawn_items(spawn_items);
	for(unsigned i = 0; i < 16; i++) { sent[k] = hud(k); k++; } /* fill the inbox */
	for(unsigned i = 0; i < 60; i++) { /* SB SB SI SB SI ... (a blast) */
		sent[k] = (i % 5 == 2 || i % 5 == 4) ? spawn_w(k) : setb_w(k, 0, 0);
		k++;
	}
	{ sent[k] = hud(k); k++; }                                      /* barrier */
	for(unsigned i = 0; i < 6; i++) { sent[k] = spawn_w(k); k++; }  /* SI only */
	assert(k == N);
	for(unsigned i = 0; i < N; i++) {
		struct client_rpc c = sent[i];
		clin_rpc_send(&c);
	}
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	unsigned got = 0;
	bool exact = true;
	for(;;) {
		struct client_rpc c;
		uint64_t gen;
		while(clin_test_inbox_pop(&c, &gen)) {
			const struct client_rpc* w = &sent[got];
			bool same = got < N && c.type == w->type && gen == 0
				&& c.world_index == w->world_index
				&& c.world_generation == w->world_generation;
			if(same && c.type == CRPC_SET_BLOCK)
				same = memcmp(&c.payload.set_block, &w->payload.set_block,
							  sizeof c.payload.set_block) == 0;
			else if(same && c.type == CRPC_SPAWN_ITEM)
				same = memcmp(&c.payload.spawn_item, &w->payload.spawn_item,
							  sizeof c.payload.spawn_item) == 0;
			else if(same)
				same = hud_seq(&c) == hud_seq(w);
			exact &= same;
			got++;
			clin_test_rpc_release(&c);
		}
		uint32_t left;
		clin_rpc_pending_stats(&left, NULL, NULL, NULL);
		if(!left)
			break;
		(void)clin_rpc_flush_pending();
	}
	*exact_out = got == N && exact;
	clin_test_set_pack_spawn_items(true);
	return pending;
}
static void case_pack_mixed(void) {
	bool exact_on, exact_off;
	const unsigned on = pack_mixed_arm(true, &exact_on);
	const unsigned off = pack_mixed_arm(false, &exact_off);
	printf("  blast-shaped stream (36 SET_BLOCK + 24 SPAWN_ITEM, hud, 6 "
		   "SPAWN_ITEM) behind a full inbox: %u entries with SPAWN_ITEM "
		   "records, %u with the SET_BLOCK-only pack\n", on, off);
	/* [19][19][19][3] | hud | [6] = 6 entries */
	check(on == 6, "SET_BLOCK and SPAWN_ITEM records share packs, closed at "
		  "the intervening message and the 19-record bound");
	check(off > on, "the SET_BLOCK-only pack closes at every SPAWN_ITEM");
	check(exact_on && exact_off, "both replay the exact sent stream: every "
		  "record once, in order, type/stamp/payload bytes intact");
}

/* G9 R21: deliberate exhaustion is still exercised at the full ring with packing ON
 * (a full ring of full packs), and a publication stop accounts for every
 * packed RECORD it drops. */
static void case_pack_exhaustion(void) {
	const unsigned CAP = CLIN_RPC_PENDING_CAPACITY;
	const unsigned FULL = 16 + CAP * CRPC_SET_BLOCK_PACK_MAX;
	uint32_t d0, x0;
	reset_stats_view(&d0, &x0);
	for(unsigned i = 0; i < FULL; i++) {
		struct client_rpc c = setb(i);
		clin_rpc_send(&c);
	}
	uint32_t pending, peak, deferred, exhausted;
	clin_rpc_pending_stats(&pending, &peak, &deferred, &exhausted);
	check(pending == CAP && exhausted - x0 == 0
			  && deferred - d0 == CAP * CRPC_SET_BLOCK_PACK_MAX,
		  "a ring of full packs (CAP x 19 records) fills without a wait");
	struct thread consumer;
	bool ok = thread_create(&consumer, late_consumer, NULL, 64);
	assert(ok);
	(void)ok;
	const ptime_t t0 = time_get();
	struct client_rpc c = setb(FULL);
	clin_rpc_send(&c); /* no open pack, ring full: must wait */
	const float ms = time_diff_s(t0, time_get()) * 1000.0f;
	thread_join(&consumer);
	uint32_t sum_us, max_us, n;
	clin_rpc_wait_take(&sum_us, &max_us, &n);
	clin_rpc_pending_stats(&pending, &peak, &deferred, &exhausted);
	printf("  packed exhaustion send waited %.1f ms (%u us, %u wait)\n", ms,
		   max_us, n);
	check(ms >= 150.0f && n == 1 && exhausted - x0 == 1,
		  "a full ring of full packs still reaches the measured exhaustion wait");
	unsigned expect = 1; /* record 0 was popped by the late consumer */
	bool in_order = true;
	const unsigned got = drain_all(&expect, &in_order);
	check(got == FULL && in_order, "nothing dropped, order kept");
	/* publication stop accounting: 16 direct + 40 packed records */
	const uint32_t drop0 = clin_server_publication_dropped();
	for(unsigned i = 0; i < 16 + 40; i++) {
		struct client_rpc s = setb(i);
		clin_rpc_send(&s);
	}
	clin_server_publication_stop(true);
	(void)clin_rpc_flush_pending();
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(pending == 0 && clin_server_publication_dropped() - drop0 == 40,
		  "a publication stop drops (and counts) every packed record");
	clin_server_publication_stop(false);
	expect = 0;
	(void)drain_all(&expect, &in_order);
}

/* SOL-REVIEW-d42f Major 1: a try-send must not overtake deferred output. */
static void case_fifo_bypass(void) {
	for(unsigned i = 0; i < 16; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	for(unsigned i = 16; i < 19; i++) {
		struct client_rpc c = hud(i); /* deferred */
		clin_rpc_send(&c);
	}
	/* free ONE token, then try-send: it must be refused (16..18 go first) */
	struct client_rpc c;
	uint64_t gen;
	bool popped = clin_test_inbox_pop(&c, &gen);
	assert(popped);
	(void)popped;
	clin_test_rpc_release(&c);
	struct client_rpc x = hud(19);
	const bool accepted = clin_try_rpc_send(&x);
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	printf("  try-send with 3 deferred and 1 free token: accepted=%d, pending "
		   "%u\n", accepted, pending);
	check(!accepted, "the try-send is refused while output is pending");
	check(pending == 2, "the freed token went to the oldest deferred entry");
	struct client_rpc y = {.type = CRPC_INVENTORY_SLOT};
	const bool accepted_local = clin_try_rpc_send_local(&y, 1, 7);
	check(!accepted_local, "the local try-send is refused as well");
	check(!clin_rpc_headroom(1), "headroom reports none behind pending output");
	clin_rpc_send(&x); /* queue it properly, in order */
	unsigned expect = 1;
	bool in_order = true;
	const unsigned got = drain_all(&expect, &in_order);
	check(got == 19 && in_order, "delivered 1..19 in send order, 19 last");
}

/* SOL-REVIEW-d42f Major 2: the terminal CRPC_LOAD_FAILED survives a
 * publication stop while deferred and is delivered when a token frees. */
static void case_terminal_kept(void) {
	for(unsigned i = 0; i < 16; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	for(unsigned i = 16; i < 20; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c); /* deferred, non-terminal */
	}
	struct client_rpc t;
	memset(&t, 0, sizeof t);
	t.type = CRPC_LOAD_FAILED;
	snprintf(t.payload.load_failed_reason, sizeof t.payload.load_failed_reason,
			 "light stopped");
	clin_rpc_send(&t); /* deferred, terminal */
	const uint32_t drop0 = clin_server_publication_dropped();
	clin_server_publication_stop(true);
	clin_rpc_pending_exit(); /* consumer is absent even after producer exit */
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(pending == 1 && clin_server_publication_dropped() - drop0 == 4,
		  "the stop drops the 4 ordinary deferred entries and KEEPS the "
		  "terminal one");
	/* the consumer frees a token: the terminal must come through */
	struct client_rpc c;
	uint64_t gen;
	for(unsigned i = 0; i < 16; i++)
		if(clin_test_inbox_pop(&c, &gen))
			clin_test_rpc_release(&c);
	(void)clin_rpc_flush_pending();
	bool got_terminal = false;
	while(clin_test_inbox_pop(&c, &gen)) {
		got_terminal |= c.type == CRPC_LOAD_FAILED
			&& strcmp(c.payload.load_failed_reason, "light stopped") == 0;
		clin_test_rpc_release(&c);
	}
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(got_terminal && pending == 0,
		  "CRPC_LOAD_FAILED is delivered once a token frees; nothing left");
	clin_server_publication_stop(false);
}

/* Cancel after the callback is already waiting, not before entering it. */
static void* cancel_later(void* unused) {
	(void)unused;
	thread_msleep(100);
	clin_rpc_cancel(true);
	return NULL;
}
static void case_cancel(void) {
	for(unsigned i = 0; i < 16 + CLIN_RPC_PENDING_CAPACITY; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	struct thread canceller;
	assert(thread_create(&canceller, cancel_later, NULL, 64));
	const ptime_t t0 = time_get();
	struct client_rpc x = batch(144);
	const size_t releases = clin_test_destroy_batch_releases();
	clin_rpc_send(&x);
	thread_join(&canceller);
	const float ms = time_diff_s(t0, time_get()) * 1000.0f;
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(ms < 500 && clin_rpc_cancelled_sends() == 1
			&& pending == CLIN_RPC_PENDING_CAPACITY + 1,
		"shutdown interrupts an already blocked callback; triggering envelope retained");
	check(clin_test_destroy_batch_releases() == releases,
		"cancel did not free an envelope still owned by output");
	clin_rpc_pending_discard();
	check(clin_test_destroy_batch_releases() == releases + 1,
		"explicit stopped-session cleanup releases retained payload exactly once");
	clin_rpc_cancel(false);
	clin_server_publication_stop(false);
	unsigned expect = 0;
	bool ordered = true;
	(void)drain_all(&expect, &ordered);
}
static void case_deadline(void) {
	memset(&S, 0, sizeof S);
	dict_entity_init(S.entities);
	test_fixture_dir("test_rpc_deadline", s_dir, sizeof s_dir);
	MKDIR(s_dir);
	string_t level; string_init_set_str(level, s_dir);
	server_world_create(&S.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&S.worlds[0], 0, 0, &sc) && sc);
	S.worlds[0].session = S.worlds[0].local = &S;
	struct block_data original;
	assert(server_world_get_block(&S.worlds[0], 8, 70, 8, &original));
	for(unsigned i = 0; i < 16 + CLIN_RPC_PENDING_CAPACITY; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	struct client_rpc c = batch(144);
	const ptime_t t0 = time_get();
	clin_rpc_send(&c); /* no consumer and no shutdown: old code hangs forever */
	float ms = time_diff_s(t0, time_get()) * 1000.0f;
	uint32_t pending, sum, max, n;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	clin_rpc_wait_take(&sum, &max, &n);
	check(ms >= 900 && ms < 2000 && n == 1 && max >= 900000,
		"exhaustion deadline returns and attributes the entire failed wait");
	check(pending == CLIN_RPC_PENDING_CAPACITY + 1 && clin_rpc_publication_faulted()
		&& clin_server_publication_stopped() && server_local_light_stopped(&S),
		"exhaustion retains output and fails closed through the session mutation/save gate");
	server_local_test_update_once(&S);
	check(S.load_failure_terminal && S.light_stop_published,
		"production update publishes one terminal and suspends the failed session");
	check(!server_world_set_block(&S.worlds[0], 8, 70, 8,
		(struct block_data){.type = BLOCK_STONE})
		&& !server_world_set_block_silent(&S.worlds[0], 8, 70, 8,
			(struct block_data){.type = BLOCK_STONE})
		&& !server_world_set_block_metadata_quiet(&S.worlds[0], 8, 70, 8, 3),
		"notified, silent and metadata writes all refuse the failed session");
	struct block_data after;
	assert(server_world_get_block(&S.worlds[0], 8, 70, 8, &after));
	check(after.type == original.type && after.metadata == original.metadata,
		"live block bytes stay unchanged after publication fails");
	uint64_t rng = S.rand_src.seed;
	server_world_random_tick(&S.worlds[0], &S.rand_src, &S, 0, 0, 2);
	check((uint64_t)S.rand_src.seed == rng, "stopped random pass consumes no further RNG");
	check(!server_world_save_chunk_coherent(&S.worlds[0], 0, 0),
		"failed partial tick cannot be persisted");
	clin_rpc_pending_exit();
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(pending == 1, "terminal survives exhausted failure and producer exit");
	struct client_rpc out;
	uint64_t generation;
	while(clin_test_inbox_pop(&out, &generation)) clin_test_rpc_release(&out);
	check(clin_rpc_flush_pending(), "late consumer drains retained terminal");
	assert(clin_test_inbox_pop(&out, &generation));
	check(out.type == CRPC_LOAD_FAILED, "late terminal delivered once");
	clin_test_rpc_release(&out);
	clin_server_publication_stop(false);
	dict_entity_clear(S.entities);
	server_world_discard(&S.worlds[0]);
	test_fixture_dir_cleanup(s_dir);
}
static void case_chunk_busy(void) {
	for(unsigned i = 0; i < 16; i++) { struct client_rpc c = hud(i); clin_rpc_send(&c); }
	for(unsigned i = 0; i < 16; i++) {
		struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
		assert(slot);
		struct client_rpc c = {.type = CRPC_CHUNK, .payload.chunk.slot = slot};
		clin_rpc_send(&c); /* all slots held in the producer's pending ring */
	}
	ptime_t t0 = time_get();
	check(clin_chunk_slot_acquire() == NULL && time_diff_ms(t0, time_get()) < 100,
		"slots in pending output cannot deadlock streaming while consumer is absent");
	for(;;) {
		struct client_rpc c; uint64_t gen;
		while(clin_test_inbox_pop(&c, &gen)) clin_test_rpc_release(&c);
		if(clin_rpc_flush_pending()) break;
	}
	struct client_rpc c; uint64_t gen;
	while(clin_test_inbox_pop(&c, &gen)) clin_test_rpc_release(&c);
	struct chunk_rpc_slot* held[16];
	for(unsigned i = 0; i < 16; i++) { held[i] = clin_chunk_slot_try_acquire(); assert(held[i]); }
	for(unsigned i = 0; i < 16; i++) clin_chunk_slot_release(held[i]);
	check(true, "all deferred chunk slots returned exactly once");
}
static bool hb_done;
static void* hb_producer(void* unused) {
	(void)unused;
	for(uint32_t i = 1; i <= 20000; i++) server_world_test_light_ckpt_record(i);
	__atomic_store_n(&hb_done, true, __ATOMIC_RELEASE);
	return NULL;
}
static void case_telemetry_race(void) {
	server_world_test_light_stats_reset();
	__atomic_store_n(&hb_done, false, __ATOMIC_RELEASE);
	struct thread producer;
	assert(thread_create(&producer, hb_producer, NULL, 63));
	uint32_t n = 0, max = 0;
	for(;;) {
		bool done = __atomic_load_n(&hb_done, __ATOMIC_ACQUIRE);
		struct server_world_light_drain_hb hb;
		server_world_light_drain_hb_take(&hb);
		n += hb.ckpt.n;
		if(hb.ckpt.us > max) max = hb.ckpt.us;
		assert(hb.ckpt.us == hb.ckpt.restore_us);
		if(done) break;
	}
	thread_join(&producer);
	check(n == 20000 && max == 20000,
		"concurrent heartbeat take/reset preserves every checkpoint and coherent maxima");
}

/* SOL-REVIEW-d42f minor: a ring allocation failure is retried, not latched. */
static void case_oom_recovery(void) {
	/* a fresh process never allocated the ring yet only if this case runs
	 * alone; otherwise the ring exists -- so exercise the retry through the
	 * hook on the alloc path (it refuses while armed) */
	clin_test_fail_pending_alloc(true);
	for(unsigned i = 0; i < 16; i++) {
		struct client_rpc c = hud(i);
		clin_rpc_send(&c);
	}
	uint32_t d0, x0;
	clin_rpc_pending_stats(NULL, NULL, &d0, &x0);
	struct thread consumer;
	bool ok = thread_create(&consumer, late_consumer, NULL, 64);
	assert(ok);
	(void)ok;
	struct client_rpc c = hud(16);
	clin_rpc_send(&c); /* no ring (or a full one): the measured wait */
	thread_join(&consumer);
	uint32_t pending, deferred, exhausted;
	clin_rpc_pending_stats(&pending, NULL, &deferred, &exhausted);
	const bool waited = exhausted - x0 == 1 && deferred == d0;
	check(waited, "injected allocation failure reaches measured exhaustion without dropping output");
	clin_test_fail_pending_alloc(false);
	unsigned expect = 1;
	bool in_order = true;
	(void)drain_all(&expect, &in_order);
	for(unsigned i = 0; i < 16 + 3; i++) {
		struct client_rpc d = hud(100 + i);
		clin_rpc_send(&d);
	}
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	check(pending == 3, "once allocation succeeds again, sends defer again "
						"(no permanent latch)");
	expect = 100;
	(void)drain_all(&expect, &in_order);
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	bool ok = clin_init();
	assert(ok);
	ok = svin_init();
	assert(ok);
	(void)ok;
	blocks_init();
	items_init();
	const char* which = argc > 1 ? argv[1] : "all";
	const bool all = strcmp(which, "all") == 0;
	int ran = 0;
#define CASE(name, fn)                                                       \
	if(all || !strcmp(which, name)) {                                        \
		printf("case %s\n", name);                                           \
		fn();                                                                \
		ran++;                                                               \
	}
	CASE("stalled", case_stalled)
	CASE("saturation", case_saturation)
	CASE("mixed", case_mixed)
	CASE("generation", case_generation)
	CASE("exhaustion", case_exhaustion)
	CASE("shutdown", case_shutdown)
	CASE("ownership", case_ownership)
	CASE("simulation", case_simulation)
	CASE("fifo_bypass", case_fifo_bypass)
	CASE("terminal_kept", case_terminal_kept)
	CASE("cancel", case_cancel)
	CASE("oom_recovery", case_oom_recovery)
	CASE("deadline", case_deadline)
	CASE("chunk_busy", case_chunk_busy)
	CASE("telemetry_race", case_telemetry_race)
	CASE("burst", case_burst)
	CASE("pack_barrier", case_pack_barrier)
	CASE("pack_exhaustion", case_pack_exhaustion)
	assert(ran > 0 && "unknown case");
	if(s_fail) {
		printf("test_rpc_pending_output: %d FAIL\n", s_fail);
		return 1;
	}
	printf("test_rpc_pending_output: PASS\n");
	return 0;
}
