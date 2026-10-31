/* Copyright (c) 2026 CaveWii_v2 contributors
 * R17 S3 (terrain FIFO head) baseline RED + structural blocker witness.
 *
 * Real local fixture (server_world backings, real chunk/far slots, real
 * clin_rpc_send envelopes and tokens). Each scenario enqueues ONE real
 * terrain head (CRPC_CHUNK / CRPC_FAR_CHUNK / CRPC_SET_BLOCK /
 * CRPC_UNLOAD_CHUNK) followed by TIME_SET or WEATHER_SET, then:
 *  1. drives MAIN's production bounded handoff service
 *     (clin_g9_handoff_service(G9_HANDOFF_RPC, 500)) 64 times. The S3 gate
 *     requires the head to finish and its token (observed at the consume
 *     observer, immediately before tchannel_send(&clin_empty_msg)) to return
 *     exactly once before any successor. Baseline: every call is UNSAFE, the
 *     head stays held and no successor runs -> admission failure (RED).
 *  2. runs the unchanged ordinary drain (clin_update_drain) and requires the
 *     same head and successor to be published IN ORDER by the real handlers,
 *     with their real world effects (nonempty ordered publication; no
 *     synchronous discard observer).
 *  3. counts, inside that ordinary handler window only, blocking stripe
 *     acquisitions (PC thread_stripe test hook, ACQUIRED) and allocator calls
 *     (malloc/calloc/realloc/free and mem2_alloc/mem2_alloc_pooled wraps).
 *     These are the work units a bounded continuation would have to perform.
 *     thread.h offers only the BLOCKING thread_stripe_lock; R18 says blocking
 *     lock acquisition and allocator work never qualify as a bounded unit.
 *
 * Default mode asserts the retained baseline invariants (head preserved, no
 * successor overtakes, lossless ordered ordinary resumption) and exits 0.
 * --admission applies the S3 progress predicate and exits 1 while any real
 * terrain head cannot progress under bounded admission (the RED receipt).
 * Host-only: no Wii timing is measured or implied. */
#include "support/split_residency_fixture.h"
#include "game/game_state.h"
#include "column_view.h"
#include "config.h"
#include "world.h"
#include "item/items.h"
#include <stdio.h>

static bool counting;
static unsigned stripe_acquired, allocs, frees, mem2_allocs;
void* __real_malloc(size_t bytes);
void* __real_calloc(size_t count, size_t bytes);
void* __real_realloc(void* p, size_t bytes);
void __real_free(void* p);
void* __real_mem2_alloc(size_t bytes);
void* __real_mem2_alloc_pooled(size_t bytes, enum mem2_pool_id pool);
void* __wrap_malloc(size_t bytes) {
	if(counting) allocs++;
	return __real_malloc(bytes);
}
void* __wrap_calloc(size_t count, size_t bytes) {
	if(counting) allocs++;
	return __real_calloc(count, bytes);
}
void* __wrap_realloc(void* p, size_t bytes) {
	if(counting) allocs++;
	return __real_realloc(p, bytes);
}
void __wrap_free(void* p) {
	if(counting && p) frees++;
	__real_free(p);
}
void* __wrap_mem2_alloc(size_t bytes) {
	if(counting) mem2_allocs++;
	return __real_mem2_alloc(bytes);
}
void* __wrap_mem2_alloc_pooled(size_t bytes, enum mem2_pool_id pool) {
	if(counting) mem2_allocs++;
	return __real_mem2_alloc_pooled(bytes, pool);
}
static void stripe_hook(unsigned event, void* user) {
	(void)user;
	if(counting && event == THREAD_STRIPE_TEST_ACQUIRED) stripe_acquired++;
}

struct order { unsigned n; enum client_rpc_type type[8]; };
static void consume(const struct client_rpc* call, void* user) {
	struct order* o = user;
	if(o->n < 8) o->type[o->n] = call->type;
	o->n++;
}

static void configure_off(void) {
	struct config c = {.root = json_value_init_object()};
	assert(c.root);
	assert(config_set_int(&c, "experimental.g9_priority_lease_int", 0));
	assert(config_set_int(&c, "experimental.g9_main_handoff_int", 0));
	struct config_g9_lease lease = config_read_g9_lease(&c);
	assert(!lease.enabled && !lease.main_handoff);
	thread_g9_lease_configure(false, lease.max_us, lease.service_us);
	thread_g9_handoff_configure(false);
	column_light_configure(false);
	config_destroy(&c);
}

static struct server_local* s_local;
static void stamp(struct client_rpc* call) {
	clin_rpc_stamp(call, s_local->worlds[0].world_index, s_local->worlds[0].generation);
}
static struct server_local* setup(void) {
	configure_off();
	gstate.remote_session = false;
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD}});
	struct server_local* s = split_fixture();
	struct server_chunk* sc;
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
	clin_drain_discard();
	return s;
}
static void teardown(struct server_local* s) {
	clin_test_set_consume_observer(NULL, NULL);
	clin_drain_discard();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player); gstate.players[0].local_player = NULL;
	split_destroy(s);
	uint32_t made, freed, refs;
	column_backing_test_counts(&made, &freed, &refs);
	assert(made == freed && refs == 0);
}

static struct client_rpc local_chunk_head(void) {
	struct server_chunk* sc = dict_server_chunks_get(s_local->worlds[0].chunks,
		S_CHUNK_ID(0, 0));
	assert(sc && sc->backing);
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	clin_chunk_slot_copy_local(slot, sc);
	memset(slot->grass_i, 0, sizeof(slot->grass_i));
	memset(slot->grass_j, 0, sizeof(slot->grass_j));
	slot->piston_moving.count = 0;
	struct client_rpc call = {.type = CRPC_CHUNK,
		.payload.chunk = {.x = 0, .z = 0, .sx = 16, .sy = WORLD_HEIGHT,
			.sz = 16, .slot = slot}};
	stamp(&call);
	return call;
}
static struct client_rpc far_head(void) {
	struct far_rpc_slot* slot = clin_far_slot_try_acquire();
	assert(slot);
	memset(slot->skin_y, 60, sizeof(slot->skin_y));
	memset(slot->top_id, BLOCK_STONE, sizeof(slot->top_id));
	memset(slot->grass_i, 0, sizeof(slot->grass_i));
	memset(slot->grass_j, 0, sizeof(slot->grass_j));
	struct client_rpc call = {.type = CRPC_FAR_CHUNK,
		.payload.far_chunk = {.x = 4 * CHUNK_SIZE, .z = 4 * CHUNK_SIZE,
			.slot = slot}};
	stamp(&call);
	return call;
}
static struct client_rpc set_block_head(void) {
	struct client_rpc call = {.type = CRPC_SET_BLOCK,
		.payload.set_block = {.x = 8, .y = 100, .z = 8,
			.block = {.type = BLOCK_STONE}}};
	stamp(&call);
	return call;
}
static struct client_rpc unload_head(void) {
	struct client_rpc call = {.type = CRPC_UNLOAD_CHUNK,
		.payload.unload_chunk = {.x = 0, .z = 0}};
	stamp(&call);
	return call;
}

struct witness {
	const char* name;
	unsigned unsafe, more, done, bounded_consumed;
	unsigned stripes, allocs, frees, mem2;
	bool admission_progress;
};
static struct witness scenario(const char* name, struct client_rpc head,
							   bool weather, uint64_t time) {
	struct witness w = {.name = name};
	struct client_rpc succ = weather
		? (struct client_rpc){.type = CRPC_WEATHER_SET,
			.payload.weather_set = {.raining = true, .rain_strength = 0.5f}}
		: (struct client_rpc){.type = CRPC_TIME_SET, .payload.time_set = time};
	stamp(&succ);
	uint64_t time_before = gstate.world_time;
	bool rain_before = gstate.weather_raining;
	clin_rpc_send(&head);
	clin_rpc_send(&succ);

	/* 1. MAIN's bounded handoff service, production entry point. */
	struct order bounded = {0};
	clin_test_set_consume_observer(consume, &bounded);
	for(unsigned k = 0; k < 64; k++) {
		enum g9_handoff_result r = clin_g9_handoff_service(G9_HANDOFF_RPC, 500);
		if(r == G9_HANDOFF_UNSAFE) w.unsafe++;
		else if(r == G9_HANDOFF_MORE) w.more++;
		else w.done++;
	}
	w.bounded_consumed = bounded.n;
	/* S3 predicate: the head finished and returned its token exactly once,
	 * before any successor; at most head then successor were published. */
	w.admission_progress = bounded.n >= 1 && bounded.n <= 2
		&& bounded.type[0] == head.type
		&& (bounded.n == 1 || bounded.type[1] == succ.type);
	/* Retained baseline invariants: no successor overtook the held head. */
	if(bounded.n == 0) {
		assert(gstate.world_time == time_before);
		assert(gstate.weather_raining == rain_before);
	} else {
		assert(bounded.type[0] == head.type);
	}

	/* 2. Unchanged ordinary drain: real handlers, nonempty ordered publication. */
	struct order ordinary = {0};
	clin_test_set_consume_observer(consume, &ordinary);
	stripe_acquired = allocs = frees = mem2_allocs = 0;
	thread_stripe_test_hook_set(stripe_hook, NULL);
	counting = true;
	clin_update_drain();
	counting = false;
	thread_stripe_test_hook_set(NULL, NULL);
	clin_test_set_consume_observer(NULL, NULL);
	w.stripes = stripe_acquired; w.allocs = allocs; w.frees = frees;
	w.mem2 = mem2_allocs;
	unsigned expected = 2 - (bounded.n > 2 ? 2 : bounded.n);
	assert(ordinary.n == expected);
	if(bounded.n == 0) {
		assert(ordinary.type[0] == head.type && ordinary.type[1] == succ.type);
	}
	if(weather) assert(gstate.weather_raining);
	else assert(gstate.world_time == time);
	printf("%-28s bounded: unsafe=%u more=%u done=%u consumed=%u progress=%u | "
		   "ordinary: published=%u order=[%d,%d] stripe_acquired=%u allocs=%u "
		   "frees=%u mem2_allocs=%u\n",
		name, w.unsafe, w.more, w.done, w.bounded_consumed,
		(unsigned)w.admission_progress, ordinary.n,
		ordinary.n > 0 ? (int)ordinary.type[0] : -1,
		ordinary.n > 1 ? (int)ordinary.type[1] : -1,
		w.stripes, w.allocs, w.frees, w.mem2);
	return w;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	bool admission = argc > 1 && !strcmp(argv[1], "--admission");
	blocks_init(); items_init();
	assert(svin_init() && clin_init());
	s_local = setup();
	unsigned failures = 0;
	struct witness w[4];

	w[0] = scenario("CRPC_CHUNK(local backed)", local_chunk_head(), false, 1001);
	struct chunk* c = world_find_chunk(&gstate.world, 8, 8, 8);
	assert(c && c->ownership == LOCAL_SHARED_VIEW); /* real install */

	w[1] = scenario("CRPC_SET_BLOCK(shared view)", set_block_head(), true, 0);
	assert(world_get_block(&gstate.world, 8, 100, 8).type == BLOCK_STONE);

	w[2] = scenario("CRPC_FAR_CHUNK", far_head(), false, 1003);
	struct world_section* far = dict_wsection_get(gstate.world.sections,
		SECTION_TO_ID(4, 4));
	assert(far && far->is_far_lod);
	assert(world_get_block(&gstate.world, 4 * 16 + 3, 60, 4 * 16 + 3).type
		== BLOCK_STONE);

	w[3] = scenario("CRPC_UNLOAD_CHUNK(shared)", unload_head(), false, 1004);
	assert(!world_find_chunk(&gstate.world, 8, 8, 8));

	for(unsigned k = 0; k < 4; k++) {
		if(!w[k].admission_progress) failures++;
		/* The baseline service never blocks, allocates or partially applies:
		 * it rejects at the head. Both properties are retained for S3. */
		if(!admission) assert(!w[k].admission_progress && w[k].unsafe == 64);
	}
	teardown(s_local);
	printf("S3 terrain admission_failures=%u/4 (baseline UNSAFE at held head); "
		   "ordinary handlers: chunk stripes=%u allocs=%u mem2=%u, "
		   "set_block stripes=%u allocs=%u, far stripes=%u allocs=%u mem2=%u, "
		   "unload stripes=%u frees=%u\n",
		failures, w[0].stripes, w[0].allocs, w[0].mem2, w[1].stripes,
		w[1].allocs, w[2].stripes, w[2].allocs, w[2].mem2, w[3].stripes,
		w[3].frees);
	if(admission) {
		printf("test_g9_fifo_bound --admission: %s\n",
			failures ? "RED (real terrain heads cannot progress under bounded admission)"
					 : "GREEN");
		return failures ? 1 : 0;
	}
	puts("test_g9_fifo_bound: PASS (baseline witness; S3 admission RED retained via --admission; host only, Wii timing unverified)");
	return 0;
}
