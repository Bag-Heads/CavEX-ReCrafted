/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_rpc_pool_burst -- BUG-TICK (SPEC R10), real-Wii hw1030a signature:
 * the server tick parked in clin_rpc_send (WDS_RPC_SEND 78 ms = 3 token
 * waits, rpc_pend 128) because the client did not drain the 16-token inbox
 * while the pending-output ring was full. Same signature as hw1018b.
 *
 * Who fills the ring: the perf_stress burst tick (three power-4 blasts) is
 * ~390 SET_BLOCK + ~120 SPAWN_ITEM (packed, ~28 entries) PLUS ~70 single
 * CRPC_ENTITY_DESTROY envelopes from the perf_stress item reaper (plus 3
 * ENTITY_EXPLODE + 3 SOUND), each a whole 792-B ring entry -- 101 entries on
 * this flat fixture, 128 on real terrain. Fix: ENTITY_DESTROY / ENTITY_EXPLODE
 * / SOUND (by-value, pointer-free) join the pack as `other` records; one
 * token now carries the reaper's destroys 19 at a time. Transport only.
 *
 * The host reproduction runs the REAL producer against the REAL consumer: a
 * real server_local streaming a disk island to the real client world with
 * debug.perf_stress armed (the production server_local_perf_stress_tick:
 * bursts every 30 world ticks, the item reaper), the consumer WITHHELD for
 * WITHHOLD_TICKS production ticks across one burst (the Wii main thread was
 * inside 20-40 ms frames of client light + mesh and drained nothing), and the
 * ring admission scaled to CAP so the fixture's burst meets the hardware
 * signature (101 entries here vs 128 there). Two arms, each its own process:
 *   arm U  packing OFF  -- the pre-fix transport (a rescue consumer, the
 *          hardware's eventual drain, keeps it off the 1-s deadline)
 *   arm P  packing ON   -- shipping
 * Acceptance: arm P never waits for a token and never exhausts during the
 * withhold; arm U does (the reproduction is real); the consumer-effect
 * trace (every envelope, in order, with its payload fields), the final
 * client island bytes, the client entity table and the server simulation
 * bytes are identical between the arms. */

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/split_residency_fixture.h"

#include "entity/entity.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/recipe.h"
#include "world.h"

#define ISLAND 3 /* chunks -ISLAND..ISLAND on both axes */
#define WARMUP_TICKS 140
#define TOTAL_TICKS (WARMUP_TICKS + 20)
#define WITHHOLD_TICKS 3
/* Ring admission for the reproduction: the fixture's 3-blast burst is 101
 * entries unpacked (flat disk island); the Wii's real terrain reached the
 * accepted 128. 64 puts the fixture's burst where the hardware's was. */
#define CAP 64u
#define TAIL_N 6

static FILE* s_out;
static uint64_t s_trace_n, s_trace_hash = 1469598103934665603ull;
static void out_bytes(const void* p, size_t n) {
	const uint8_t* b = p;
	for(size_t i = 0; i < n; i++)
		s_trace_hash = (s_trace_hash ^ b[i]) * 1099511628211ull;
	assert(fwrite(p, 1, n, s_out) == n);
}
static void fold_f(uint32_t* dst, float f) {
	memcpy(dst, &f, sizeof f);
}
/* The real consumer: one call per processed envelope (packed records
 * included, one each), after clin_process_queued. */
static void consumed(const struct client_rpc* c, void* unused) {
	(void)unused;
	assert(c->type != CRPC_SET_BLOCK_PACK && "a pack reached a consumer");
	uint32_t rec[16];
	memset(rec, 0, sizeof rec);
	rec[0] = (uint32_t)c->type;
	rec[1] = c->world_index;
	rec[2] = c->world_generation;
	switch(c->type) {
	case CRPC_SET_BLOCK: {
		const struct crpc_set_block* b = &c->payload.set_block;
		rec[3] = (uint32_t)b->x;
		rec[4] = (uint32_t)b->y;
		rec[5] = (uint32_t)b->z;
		rec[6] = b->block.type | (uint32_t)b->block.metadata << 8
			| (uint32_t)b->block.sky_light << 12 | (uint32_t)b->block.torch_light << 16;
		rec[7] = b->local_revision.owner;
		rec[8] = b->local_revision.incarnation;
		rec[9] = b->local_revision.content;
		rec[10] = b->local_revision.light;
		rec[11] = b->local_revision.events;
		rec[12] = b->local_previous_content;
		break;
	}
	case CRPC_SPAWN_ITEM: {
		const struct crpc_spawn_item* s = &c->payload.spawn_item;
		rec[3] = s->entity_id;
		rec[4] = s->item.id | (uint32_t)s->item.durability << 16;
		rec[5] = s->item.count;
		for(int k = 0; k < 3; k++) fold_f(&rec[6 + k], s->pos[k]);
		for(int k = 0; k < 3; k++) fold_f(&rec[9 + k], s->vel[k]);
		break;
	}
	case CRPC_ENTITY_DESTROY: rec[3] = c->payload.entity_destroy.entity_id; break;
	case CRPC_ENTITY_EXPLODE:
		for(int k = 0; k < 3; k++) fold_f(&rec[3 + k], c->payload.entity_explode.pos[k]);
		fold_f(&rec[6], c->payload.entity_explode.power);
		break;
	case CRPC_SOUND:
		rec[3] = c->payload.sound.sound_id | (uint32_t)c->payload.sound.ui << 16;
		for(int k = 0; k < 3; k++) fold_f(&rec[4 + k], c->payload.sound.pos[k]);
		fold_f(&rec[7], c->payload.sound.vol);
		fold_f(&rec[8], c->payload.sound.pitch);
		break;
	default: break;
	}
	out_bytes(rec, sizeof rec);
	s_trace_n++;
}

static unsigned s_tick_hist[256], s_tick_sent;
static void sent(const struct client_rpc* c, void* unused) {
	(void)unused;
	if(c->type < 256) s_tick_hist[c->type]++;
	s_tick_sent++;
}

/* The consumer is serialised by this lock: the test thread drains after
 * every non-withheld tick; the RESCUE thread drains only while the server
 * (this same test thread) is parked in the exhaustion wait -- the
 * hardware's eventual drain, so a waiting arm cannot hit the 1-s deadline. */
static struct thread_mutex s_consumer;
static volatile int s_rescue_stop;
static unsigned s_rescues;
static uint32_t s_rescue_seen0;
static void* rescue_entry(void* unused) {
	(void)unused;
	uint32_t seen = s_rescue_seen0;
	while(!s_rescue_stop) {
		uint32_t ex = 0;
		clin_rpc_pending_stats(NULL, NULL, NULL, &ex);
		if(ex != seen) {
			seen = ex;
			thread_mutex_lock(&s_consumer);
			clin_update_drain();
			s_rescues++;
			thread_mutex_unlock(&s_consumer);
		}
		thread_msleep(1);
	}
	return NULL;
}
static void drain_to_empty(void) {
	thread_mutex_lock(&s_consumer);
	for(;;) {
		clin_update_drain();
		const bool flushed = clin_rpc_flush_pending();
		uint32_t pending = 0;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(flushed && pending == 0) {
			clin_update_drain();
			break;
		}
	}
	assert(!clin_world_message_held());
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	thread_mutex_unlock(&s_consumer);
}

static const char* kind_name(unsigned t) {
	switch(t) {
	case CRPC_SET_BLOCK: return "SET_BLOCK";
	case CRPC_SPAWN_ITEM: return "SPAWN_ITEM";
	case CRPC_ENTITY_DESTROY: return "ENTITY_DESTROY";
	case CRPC_ENTITY_EXPLODE: return "ENTITY_EXPLODE";
	case CRPC_SOUND: return "SOUND";
	case CRPC_ENTITY_MOVE: return "ENTITY_MOVE";
	case CRPC_CHUNK: return "CHUNK";
	case CRPC_TIME_SET: return "TIME_SET";
	default: return NULL;
	}
}
static void print_hist(const char* label, const unsigned* h, unsigned total) {
	printf("  %s: %u envelopes:", label, total);
	for(unsigned t = 0; t < 256; t++)
		if(h[t]) {
			const char* n = kind_name(t);
			if(n) printf(" %s=%u", n, h[t]);
			else printf(" type%u=%u", t, h[t]);
		}
	printf("\n");
}

/* Order-independent digest of the client entity table. */
static uint64_t client_entities_digest(unsigned* count) {
	uint64_t sum = 0;
	unsigned n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, gstate.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		const struct entity* e = dict_entity_cref(it)->value;
		uint64_t h = 1469598103934665603ull;
		uint32_t f[8] = {e->id, (uint32_t)e->type};
		memcpy(&f[2], e->pos, sizeof(float) * 3);
		if(e->type == ENTITY_ITEM) {
			f[5] = e->data.item.item.id;
			f[6] = e->data.item.item.durability;
			f[7] = e->data.item.item.count;
		}
		const uint8_t* b = (const uint8_t*)f;
		for(size_t k = 0; k < sizeof f; k++) h = (h ^ b[k]) * 1099511628211ull;
		sum += h;
		n++;
	}
	*count = n;
	return sum;
}

static int arm(char mode, const char* path) {
	const bool pack = mode == 'P';
	s_out = fopen(path, "wb");
	assert(s_out);
	assert(thread_mutex_init(&s_consumer));
	assert(clin_init() && svin_init());
	blocks_init();
	items_init();
	recipe_init();
	clin_test_set_block_pack(pack);
	clin_test_set_pack_spawn_items(pack);
	clin_test_set_pending_capacity(CAP);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD, .local_entity = 0},
	});
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_seed_math_random(0x73C0DE);
	s->rand_src.seed = 0x9E11u;
	for(int z = -ISLAND; z <= ISLAND; z++)
		for(int x = -ISLAND; x <= ISLAND; x++)
			split_populate(s, x, z);
	while(dict_server_chunks_size(s->worlds[0].chunks)) {
		dict_server_chunks_it_t it;
		dict_server_chunks_it(it, s->worlds[0].chunks);
		int64_t id = dict_server_chunks_ref(it)->key;
		assert(server_world_save_chunk(&s->worlds[0], true, S_CHUNK_X(id), S_CHUNK_Z(id)));
	}
	s->worlds[0].generator = NULL; /* disk-only streaming: deterministic */
	s->player.x = 8.5;
	s->player.z = 8.5;
	s->player.y = 4.0 + PLAYER_EYE_HEIGHT;
	s->input_ack_pending = 1;
	s->input_epoch_counter = 1;
	s->player.finished_loading = true;
	s->perf_stress = true; /* the production stress driver */
	for(unsigned n = 0; n < 120; n++) {
		server_local_test_update_once(s);
		drain_to_empty();
	}
	s->input_ack_pending = 0;
	for(unsigned n = 0; n < WARMUP_TICKS - 120; n++) {
		server_local_test_update_once(s);
		drain_to_empty();
	}
	unsigned resident = 0;
	for(int z = -1; z <= 1; z++)
		for(int x = -1; x <= 1; x++)
			resident += world_find_chunk(&gstate.world, x * 16, 4, z * 16) != NULL;
	assert(resident == 9 && "fixture: the near ring streamed to the real client");

	/* the traced phase: the consumer withheld from one tick before the next
	 * burst (world_time advances inside the update: the burst fires in the
	 * update that moves world_time onto a multiple of 30) */
	clin_test_set_consume_observer(consumed, NULL);
	clin_test_set_rpc_observer(sent, NULL);
	const uint64_t burst_at = (s->world_time / 30 + 1) * 30;
	const uint64_t hold_from = burst_at - 2;
	uint32_t wait_sum, wait_max, wait_n;
	clin_rpc_wait_take(&wait_sum, &wait_max, &wait_n); /* clear */
	uint32_t ex0 = 0, packs0, rec0;
	clin_rpc_pending_stats(NULL, NULL, NULL, &ex0);
	clin_rpc_pack_stats(&packs0, &rec0);
	s_rescue_seen0 = ex0;
	struct thread rescue;
	assert(thread_create(&rescue, rescue_entry, NULL, 0));
	unsigned held = 0, ring_peak = 0, hold_sent = 0, hold_hist[256];
	uint32_t hold_waits = 0, hold_wait_us = 0, hold_exhausted = 0;
	bool reported = false;
	memset(hold_hist, 0, sizeof hold_hist);
	for(unsigned t = WARMUP_TICKS; t < TOTAL_TICKS; t++) {
		const uint64_t wt = s->world_time;
		const bool hold = wt >= hold_from && wt < hold_from + WITHHOLD_TICKS;
		memset(s_tick_hist, 0, sizeof s_tick_hist);
		s_tick_sent = 0;
		server_local_test_update_once(s);
		uint32_t pending = 0;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(hold) {
			char label[80];
			snprintf(label, sizeof label, "arm %c withheld tick -> wt=%" PRIu64 "%s, ring %u",
					 mode, wt + 1, wt + 1 == burst_at ? " (BURST)" : "", pending);
			print_hist(label, s_tick_hist, s_tick_sent);
			for(unsigned k = 0; k < 256; k++) hold_hist[k] += s_tick_hist[k];
			hold_sent += s_tick_sent;
			held++;
			if(pending > ring_peak) ring_peak = pending;
			continue; /* the consumer does not run this tick */
		}
		if(held == WITHHOLD_TICKS && !reported) {
			reported = true;
			clin_rpc_wait_take(&hold_wait_us, &wait_max, &hold_waits);
			uint32_t ex1 = 0;
			clin_rpc_pending_stats(NULL, NULL, NULL, &ex1);
			hold_exhausted = ex1 - ex0;
			char label[64];
			snprintf(label, sizeof label, "arm %c withheld window total", mode);
			print_hist(label, hold_hist, hold_sent);
			printf("  arm %c: withheld %u ticks at ring admission %u: ring peak %u entries, "
				   "token waits %u (sum %u us, max %u us), exhaustion %u, rescues %u, "
				   "publication %s\n",
				   mode, WITHHOLD_TICKS, CAP, ring_peak, hold_waits, hold_wait_us, wait_max,
				   hold_exhausted, s_rescues,
				   clin_rpc_publication_faulted() ? "FAULTED" : "ok");
		}
		drain_to_empty();
	}
	s_rescue_stop = 1;
	thread_join(&rescue);
	clin_test_set_rpc_observer(NULL, NULL);
	clin_test_set_consume_observer(NULL, NULL);
	assert(reported);
	assert(!clin_rpc_publication_faulted() && !server_local_light_stopped(s)
		   && "fixture validity: the session never stopped publishing");
	uint32_t packs1, rec1;
	clin_rpc_pack_stats(&packs1, &rec1);
	printf("  arm %c: %" PRIu64 " consumed envelopes, trace %016" PRIx64 ", packs %u carrying "
		   "%u appended records\n", mode, s_trace_n, s_trace_hash, packs1 - packs0, rec1 - rec0);
	/* section markers keep the comparison self-describing */
	const uint32_t marker = 0xC0FFEE01u;
	out_bytes(&marker, sizeof marker);
	for(int x = -ISLAND * 16; x < (ISLAND + 1) * 16; x++)
		for(int z = -ISLAND * 16; z < (ISLAND + 1) * 16; z++)
			for(int yy = 0; yy < WORLD_HEIGHT; yy++) {
				struct block_data b = world_get_block(&gstate.world, x, yy, z);
				const uint8_t v[4] = {b.type, b.metadata, b.sky_light, b.torch_light};
				out_bytes(v, sizeof v);
			}
	const uint32_t marker2 = 0xC0FFEE02u;
	out_bytes(&marker2, sizeof marker2);
	unsigned client_entities = 0;
	const uint64_t edigest = client_entities_digest(&client_entities);
	out_bytes(&client_entities, sizeof client_entities);
	out_bytes(&edigest, sizeof edigest);
	/* the client applied every ENTITY_DESTROY: its table matches the server's */
	const unsigned server_entities = (unsigned)dict_entity_size(s->entities);
	printf("  arm %c: %u client entities, %u server entities\n", mode, client_entities,
		   server_entities);
	out_bytes(&server_entities, sizeof server_entities);
	/* the server simulation */
	for(int cx = -ISLAND; cx <= ISLAND; cx++)
		for(int cz = -ISLAND; cz <= ISLAND; cz++) {
			struct server_chunk* sc
				= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(cx, cz));
			const uint8_t present = sc != NULL;
			out_bytes(&present, 1);
			if(!sc) continue;
			const size_t cells = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
			out_bytes(sc->ids, cells);
			out_bytes(sc->metadata, cells / 2);
			out_bytes(sc->lighting_sky, cells / 2);
			out_bytes(sc->lighting_torch, cells / 2);
			out_bytes(sc->heightmap, CHUNK_SIZE * CHUNK_SIZE);
		}
	struct server_local_math_random_state math;
	server_local_math_random_capture(&math);
	const uint64_t rngs[] = {(uint64_t)s->rand_src.seed, (uint64_t)s->container_drop_rng.seed,
		(uint64_t)s->dispenser_break_rng.seed, (uint64_t)s->entity_rng.seed, math.rng.seed,
		s->world_time};
	out_bytes(rngs, sizeof rngs);
	const struct server_world* w0 = &s->worlds[0];
	const uint32_t q[] = {w0->current_tick, (uint32_t)w0->scheduled_tick_count};
	out_bytes(q, sizeof q);
	for(size_t i = 0; i < w0->scheduled_tick_count; i++) {
		const struct scheduled_tick* tick = &w0->scheduled_ticks[
			(w0->scheduled_tick_head + i) % w0->scheduled_tick_capacity];
		const uint32_t f[] = {(uint32_t)tick->x, (uint32_t)tick->y, (uint32_t)tick->z,
			tick->fire_at_tick, tick->block_type, tick->queue_flags};
		out_bytes(f, sizeof f);
	}
	/* summary tail the parent reads back */
	const uint32_t tail[TAIL_N] = {packs1 - packs0, (uint32_t)s_trace_n, hold_waits,
		hold_exhausted, ring_peak, hold_hist[CRPC_ENTITY_DESTROY]};
	assert(fwrite(tail, 1, sizeof tail, s_out) == sizeof tail);
	fclose(s_out);
	clin_update_drain();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	dict_entity_clear(gstate.entities);
	split_destroy(s);
	return 0;
}

static unsigned char* slurp(const char* path, size_t* n) {
	FILE* f = fopen(path, "rb");
	if(!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* b = malloc((size_t)len + 1);
	assert(b);
	*n = fread(b, 1, (size_t)len, f);
	fclose(f);
	return b;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc == 4 && strcmp(argv[1], "arm") == 0)
		return arm(argv[2][0], argv[3]);
	char dir[220], outs[2][260], cmd[1200];
	test_fixture_dir("test_rpc_pool_burst", dir, sizeof dir);
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0777);
#endif
	const char* arms[2] = {"U", "P"};
	for(int i = 0; i < 2; i++) {
		snprintf(outs[i], sizeof outs[i], "%s/%s.bin", dir, arms[i]);
		snprintf(cmd, sizeof cmd, "\"\"%s\" arm %s \"%s\"\"", argv[0], arms[i], outs[i]);
#ifndef _WIN32
		snprintf(cmd, sizeof cmd, "\"%s\" arm %s \"%s\"", argv[0], arms[i], outs[i]);
#endif
		printf("arm %s (%s)\n", arms[i], i ? "shipping: packing ON" : "packing OFF");
		const int rc = system(cmd);
		if(rc != 0) {
			printf("FAIL: arm %s exited %d\n", arms[i], rc);
			return 1;
		}
	}
	size_t n[2];
	unsigned char* b[2];
	uint32_t tl[2][TAIL_N];
	enum { TAIL = TAIL_N * sizeof(uint32_t) };
	for(int i = 0; i < 2; i++) {
		b[i] = slurp(outs[i], &n[i]);
		assert(b[i] && n[i] >= TAIL);
		memcpy(tl[i], b[i] + n[i] - TAIL, TAIL);
	}
	int fail = 0;
	printf("  info: withheld window -- arm U %u token waits / %u exhaustions / ring peak %u; "
		   "arm P %u / %u / %u; %u ENTITY_DESTROY in the window; arm P %u packs\n",
		   tl[0][2], tl[0][3], tl[0][4], tl[1][2], tl[1][3], tl[1][4], tl[0][5], tl[1][0]);
	const bool reproduced = tl[0][2] > 0 && tl[0][3] > 0 && tl[0][5] > 0;
	printf("  %s: the unpacked transport reproduces hw1030a (the withheld burst parks "
		   "the server in a token wait; ENTITY_DESTROY singles in the burst)\n",
		   reproduced ? "PASS" : "FAIL");
	fail += !reproduced;
	const bool fixed = tl[1][2] == 0 && tl[1][3] == 0 && tl[1][0] > 0 && tl[1][4] < tl[0][4];
	printf("  %s: shipping transport: the server tick never waits on the full client RPC "
		   "pool across the withheld burst (0 waits, 0 exhaustions, ring peak %u < %u)\n",
		   fixed ? "PASS" : "FAIL", tl[1][4], tl[0][4]);
	fail += !fixed;
	const bool same = n[0] == n[1] && memcmp(b[0], b[1], n[0] - TAIL) == 0 && tl[0][1] == tl[1][1];
	size_t first = 0;
	if(!same)
		while(first < n[0] && first < n[1] && b[0][first] == b[1][first]) first++;
	printf("  %s: arm P == arm U: consumer-effect trace (%u envelopes), final client bytes, "
		   "client + server entity counts, server simulation bytes (%zu bytes)%s\n",
		   same ? "PASS" : "FAIL", tl[0][1], n[0] - TAIL,
		   same ? "" : " -- first difference below");
	if(!same) printf("    first differing byte offset %zu (sizes %zu / %zu)\n", first, n[0], n[1]);
	fail += !same;
	for(int i = 0; i < 2; i++) {
		free(b[i]);
		remove(outs[i]);
	}
	test_fixture_dir_cleanup(dir);
	printf(fail ? "test_rpc_pool_burst FAIL (%d)\n" : "test_rpc_pool_burst PASS\n", fail);
	return fail ? 1 : 0;
}
