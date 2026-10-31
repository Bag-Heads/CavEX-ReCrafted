/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_light_halt_forced -- REL-05-tests (SPEC R5): a REAL forced lighting
 * halt through a production nested callback, against exactly the committed
 * HALT-STOP P1 dispatch boundaries (d2cc795ff):
 *
 *   D1-LEVER : lever toggle (silent write) -> notifyNeighbours_from (D1) ->
 *              -X trap door powers open -> its server_world_set_block
 *              reservation is refused. +X trap door must not be dispatched.
 *   D2-TORCH : server_world_set_block removes a stone (D2 setter fan) -> +Y
 *              torch loses support -> drop + nested air write refused. The
 *              -Z/+Z torches must not be dispatched (no drop, no write).
 *   D3-WIRE  : real wire flood (redstone drain, D3) reaches a trap door at a
 *              wire end -> refused. The other wire-end trap door is never
 *              visited and the drain makes no later visit.
 *   D4-REARM : a due redstone-torch scheduled tick (D4) refuses its toggle
 *              write. The second due torch never fires and the post-drain
 *              chunk-tick rearm tail does not touch the full/freed ring.
 *
 * The refusal is server_world_test_light_fail_after (the production reserve),
 * never a pre-set halted flag. Every witness also proves: the halt latch, no
 * later mutation, no successful chunk save, and the last committed save is
 * byte-identical on disk and reloads to the committed planes. A healthy
 * control runs the same fixture uninjected to completion.
 *
 * Mutants (runtime-armed, no compile variant): the fault observer clears the
 * P1 per-world stop byte (server_world.simulation_stopped) right after the
 * latch, i.e. exactly the guard P1 added; the legacy light_fault stays. The
 * default run proves each mutant is killed; CW_REL05_MUTANT=<name>|all runs
 * it as the primary and must print "REL-05-tests RED:" and exit 1.
 * Not covered here (cut HALT-STOP-2): leaf tails inside the already-running
 * callback (e.g. sound RNG), generic setter/entity/TE/weather tails.
 * Healthy order: Beta World.notifyBlocksOfNeighborChange -X,+X,-Y,+Y,-Z,+Z;
 * MinecraftServer.java:327-333 (world tick, light drain, entities). */

#include <assert.h>
#include <dirent.h>
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
#include "block/redstone_server.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

extern int blocks_test_notify_depth(void);
extern int server_world_test_notify_scope_depth(void);
extern int redstone_server_test_drain_depth(void);

enum { W_D1, W_D2, W_D3, W_D4, W_N };
static const char* const NAMES[W_N] = {"D1-LEVER", "D2-TORCH", "D3-WIRE", "D4-REARM"};

static struct server_local S;
static char s_dir[220];
static int s_fail, s_killed;
static bool s_expect_kill, s_blind;
static const char* s_name;

static void need(bool ok, const char* what) {
	if(ok) { printf("  PASS %s: %s\n", s_name, what); return; }
	if(s_expect_kill) { s_killed++; printf("  mutant detected %s: %s\n", s_name, what); return; }
	printf("REL-05-tests RED: %s: %s\n", s_name, what);
	s_fail++;
}

struct buf { uint8_t* p; size_t n, cap; };
static void put(struct buf* b, const void* p, size_t n) {
	if(b->n + n > b->cap) {
		b->cap = (b->n + n) * 2 + 4096;
		b->p = realloc(b->p, b->cap);
		assert(b->p);
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
}
static void put64(struct buf* b, uint64_t v) { put(b, &v, sizeof v); }
static bool same(const struct buf* a, const struct buf* b) {
	return a->n == b->n && !memcmp(a->p, b->p, a->n);
}
/* First-difference diagnostic: byte offset and the next 16 bytes (a/b). */
static void first_diff(const char* tag, const struct buf* a, const struct buf* b) {
	size_t n = a->n < b->n ? a->n : b->n, i = 0;
	while(i < n && a->p[i] == b->p[i]) i++;
	if(i == n && a->n == b->n) return;
	printf("  %s first difference at byte %zu of %zu/%zu:", tag, i, a->n, b->n);
	for(size_t j = i; j < i + 16 && j < n; j++) printf(" %02x/%02x", a->p[j], b->p[j]);
	printf("\n");
}
static uint64_t fnv(const struct buf* b) {
	uint64_t h = 1469598103934665603ull;
	for(size_t i = 0; i < b->n; i++) h = (h ^ b->p[i]) * 1099511628211ull;
	return h;
}

/* All five planes of the 9 resident chunks (the persisted world). */
static void planes(struct server_world* w, struct buf* b) {
	b->n = 0;
	for(int cx = -1; cx <= 1; cx++) for(int cz = -1; cz <= 1; cz++) {
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		assert(sc);
		put(b, sc->ids, SERVER_CHUNK_IDS_BYTES);
		put(b, sc->metadata, SERVER_CHUNK_NIBBLE_BYTES);
		put(b, sc->lighting_sky, SERVER_CHUNK_NIBBLE_BYTES);
		put(b, sc->lighting_torch, SERVER_CHUNK_NIBBLE_BYTES);
		put(b, sc->heightmap, SERVER_CHUNK_HEIGHTMAP_BYTES);
	}
}
/* Planes plus entities, pending light and the scheduled ring/rearm state. */
static void state(struct server_world* w, struct buf* b) {
	planes(w, b);
	put64(b, dict_entity_size(S.entities));
	put64(b, w->light_seed_n);
	put64(b, w->scheduled_tick_count);
	for(size_t i = 0; i < w->scheduled_tick_count; i++) {
		const struct scheduled_tick* t = &w->scheduled_ticks[(w->scheduled_tick_head + i) % w->scheduled_tick_capacity];
		put64(b, (uint64_t)t->x); put64(b, (uint64_t)t->y); put64(b, (uint64_t)t->z);
		put64(b, t->block_type); put64(b, t->fire_at_tick); put64(b, t->queue_flags);
	}
	for(int cx = -1; cx <= 1; cx++) for(int cz = -1; cz <= 1; cz++) {
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		put64(b, sc->load_tick_rearm_cursor); put64(b, sc->load_ticks_rearmed);
	}
}

/* Every byte under the save directory, names sorted (closed handles only). */
static int cmp_name(const void* a, const void* b) { return strcmp(a, b); }
static void disk(const char* dir, struct buf* b) {
	char names[64][128]; int n = 0;
	DIR* d = opendir(dir); assert(d);
	for(struct dirent* e; (e = readdir(d));)
		if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
			assert(n < 64); snprintf(names[n++], sizeof names[0], "%.120s", e->d_name);
		}
	closedir(d);
	qsort(names, n, sizeof names[0], cmp_name);
	for(int i = 0; i < n; i++) {
		char p[1024]; snprintf(p, sizeof p, "%.800s/%.120s", dir, names[i]);
		put(b, names[i], strlen(names[i]) + 1);
		if(test_fixture_is_dir(dir, names[i])) { disk(p, b); continue; }
		FILE* f = fopen(p, "rb"); assert(f);
		uint8_t chunk[4096]; size_t r;
		while((r = fread(chunk, 1, sizeof chunk, f)) > 0) put(b, chunk, r);
		fclose(f);
	}
}

static void* drain_entry(void* arg) {
	(void)arg;
	for(;;) { clin_drain_discard(); thread_msleep(1); }
	return NULL;
}

static struct server_world* sess_open(void) {
	memset(&S, 0, sizeof S);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&S))
		abort();
	dict_entity_init(S.entities);
	clin_server_publication_stop(false);
	struct server_world* w = &S.worlds[0];
	string_t level; string_init_set_str(level, s_dir);
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	w->session = &S; w->local = &S;
	jrand_set_seed(&w->rand, 4242);
	S.scheduled_tick_budget = 128; S.rand_src.seed = 0x9E11u; S.world_time = 6000;
	server_local_skylight_recompute(&S);
	for(int cx = -1; cx <= 1; cx++) for(int cz = -1; cz <= 1; cz++) {
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
	}
	assert(server_world_light_drain(w));
	return w;
}
static void sess_close(bool persist) {
	redstone_server_reset();
	if(persist) server_world_destroy(&S.worlds[0]);
	else server_world_discard(&S.worlds[0]);
	dict_entity_clear(S.entities);
}

static void raw(struct server_world* w, int x, int y, int z, int id, int meta) {
	assert(server_world_set_block_silent(w, x, y, z, (struct block_data){.type = id, .metadata = meta}));
}
static struct block_data at(struct server_world* w, int x, int y, int z) {
	struct block_data b = {0}; assert(server_world_get_block(w, x, y, z, &b)); return b;
}

static void build(int k, struct server_world* w) {
	if(k == W_D1) {
		raw(w, 8, 100, 8, BLOCK_STONE, 0); raw(w, 8, 101, 8, BLOCK_LEVER, 5);
		raw(w, 6, 101, 8, BLOCK_STONE, 0); raw(w, 7, 101, 8, BLOCK_TRAP_DOOR, 3);
		raw(w, 10, 101, 8, BLOCK_STONE, 0); raw(w, 9, 101, 8, BLOCK_TRAP_DOOR, 2);
	} else if(k == W_D2) {
		raw(w, 8, 100, 8, BLOCK_STONE, 0); raw(w, 8, 101, 8, BLOCK_TORCH, 5);
		raw(w, 8, 100, 7, BLOCK_TORCH, 4); raw(w, 8, 100, 9, BLOCK_TORCH, 3);
	} else if(k == W_D3) {
		for(int x = 7; x <= 10; x++) raw(w, x, 99, 8, BLOCK_STONE, 0);
		raw(w, 9, 99, 9, BLOCK_STONE, 0);
		raw(w, 7, 100, 8, BLOCK_REDSTONE_TORCH_LIT, 5);
		for(int x = 8; x <= 10; x++) raw(w, x, 100, 8, BLOCK_REDSTONE_WIRE, 0);
		raw(w, 9, 100, 9, BLOCK_REDSTONE_WIRE, 0);
		raw(w, 12, 100, 8, BLOCK_STONE, 0); raw(w, 11, 100, 8, BLOCK_TRAP_DOOR, 2);
		raw(w, 9, 100, 11, BLOCK_STONE, 0); raw(w, 9, 100, 10, BLOCK_TRAP_DOOR, 0);
	} else {
		raw(w, 8, 99, 8, BLOCK_STONE, 0); raw(w, 8, 100, 8, BLOCK_REDSTONE_TORCH, 5);
		raw(w, 12, 99, 8, BLOCK_STONE, 0); raw(w, 12, 100, 8, BLOCK_REDSTONE_TORCH, 5);
	}
}

/* Counting wrappers always delegate to the production handler. */
static struct block* s_orig[256];
static struct block s_copy[256];
static unsigned s_calls;
static bool s_in_tick;
static void wrap_nb(struct server_local* s, struct block_info* i) {
	s_calls++; s_orig[i->block->type]->onNeighbourBlockChange(s, i);
}
static void wrap_tick(struct server_local* s, struct block_info* i) {
	s_calls++; s_in_tick = true; s_orig[i->block->type]->onWorldTick(s, i); s_in_tick = false;
}
static void wrap(uint8_t id, bool tick) {
	s_orig[id] = blocks[id]; s_copy[id] = *blocks[id];
	if(tick) s_copy[id].onWorldTick = wrap_tick; else s_copy[id].onNeighbourBlockChange = wrap_nb;
	blocks[id] = &s_copy[id];
}

static struct buf s_at_fault;
static int s_faults, s_d1, s_d2, s_d3;
static unsigned s_calls_at_fault, s_visits_at_fault;
static bool s_tick_at_fault, s_byte_at_fault;
static void on_fault(struct server_world* w) {
	if(s_faults++) return;
	s_calls_at_fault = s_calls; s_visits_at_fault = redstone_server_drain_visits();
	s_d1 = blocks_test_notify_depth(); s_d2 = server_world_test_notify_scope_depth();
	s_d3 = redstone_server_test_drain_depth(); s_tick_at_fault = s_in_tick;
	s_byte_at_fault = w->simulation_stopped && S.worlds[0].simulation_stopped && S.worlds[1].simulation_stopped;
	state(w, &s_at_fault);
	if(s_blind) for(int i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++) S.worlds[i].simulation_stopped = false;
}

static void trigger(int k, struct server_world* w) {
	struct server_world* prev = block_world_enter(&S, w);
	if(k == W_D1) {
		struct block_data lb = at(w, 8, 101, 8);
		struct block_info on = {.block = &lb, .x = 8, .y = 101, .z = 8};
		blocks[BLOCK_LEVER]->onRightClick(&S, NULL, NULL, &on, &on, SIDE_TOP);
	} else if(k == W_D2) {
		(void)server_world_set_block(w, 8, 100, 8, (struct block_data){.type = BLOCK_AIR});
	} else if(k == W_D3) {
		struct block_data wb = at(w, 8, 100, 8);
		struct block_info info = {.block = &wb, .x = 8, .y = 100, .z = 8};
		redstone_wire_neighbour_change(&S, &info);
	} else {
		server_world_tick(w, &S);
	}
	block_world_leave(&S, prev);
}

static bool healthy_effect(int k, struct server_world* w) {
	if(k == W_D1)
		return (at(w, 7, 101, 8).metadata & 4) && (at(w, 9, 101, 8).metadata & 4);
	if(k == W_D3)
		return (at(w, 11, 100, 8).metadata & 4) && (at(w, 9, 100, 10).metadata & 4);
	if(k == W_D2)
		return at(w, 8, 101, 8).type == BLOCK_AIR && at(w, 8, 100, 7).type == BLOCK_AIR
			&& at(w, 8, 100, 9).type == BLOCK_AIR && dict_entity_size(S.entities) == 3;
	return at(w, 8, 100, 8).type == BLOCK_REDSTONE_TORCH_LIT
		&& at(w, 12, 100, 8).type == BLOCK_REDSTONE_TORCH_LIT;
}

static void run(int k, bool healthy) {
	char id[64]; snprintf(id, sizeof id, "test_light_halt_forced_%d", k);
	test_fixture_dir(id, s_dir, sizeof s_dir);
	MKDIR(s_dir);
	char region[300]; snprintf(region, sizeof region, "%s/region", s_dir);
	MKDIR(region);
	struct buf committed = {0}, disk0 = {0}, cur = {0}, tmp = {0};
	/* 1. build and COMMIT the fixture (persisting teardown, handles closed). */
	struct server_world* w = sess_open();
	build(k, w); assert(server_world_light_drain(w));
	sess_close(true);
	disk(s_dir, &disk0);
	/* 2. reopen: the committed planes as loaded. */
	w = sess_open();
	planes(w, &committed);
	if(k == W_D4) {
		server_world_scheduled_ticks_destroy(w); server_world_scheduled_ticks_create(w, 4);
		assert(server_world_schedule_tick_raw(w, 8, 100, 8, BLOCK_REDSTONE_TORCH, 0));
		assert(server_world_schedule_tick_raw(w, 12, 100, 8, BLOCK_REDSTONE_TORCH, 0));
		assert(server_world_schedule_tick_raw(w, 13, 100, 8, BLOCK_REDSTONE_TORCH, 1000));
		assert(server_world_schedule_tick_raw(w, 14, 100, 8, BLOCK_REDSTONE_TORCH, 1000));
		struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
		c->load_ticks_rearmed = false; c->load_tick_rearm_cursor = 0;
	}
	const uint8_t wrapped = k == W_D2 ? BLOCK_TORCH : k == W_D4 ? BLOCK_REDSTONE_TORCH : BLOCK_TRAP_DOOR;
	wrap(wrapped, k == W_D4);
	s_calls = 0; s_faults = 0; s_in_tick = false; s_at_fault.n = 0;
	s_calls_at_fault = s_visits_at_fault = 0; s_d1 = s_d2 = s_d3 = 0;
	s_tick_at_fault = s_byte_at_fault = false;
	server_world_test_light_fault_hook = on_fault;
	server_world_test_light_fail_after(healthy ? -1 : (k == W_D2 ? 1 : 0)); /* D2: outer ok, nested refused */
	trigger(k, w);
	server_world_test_light_fail_after(-1);
	server_world_test_light_fault_hook = NULL;
	unsigned visits = redstone_server_drain_visits();
	printf("%s %s%s: faults=%d calls=%u calls_at_fault=%u visits=%u visits_at_fault=%u depth D1/D2/D3=%d/%d/%d in_tick=%d\n",
		s_name, healthy ? "healthy" : "forced", s_blind ? " (mutant: P1 stop byte blinded)" : "",
		s_faults, s_calls, s_calls_at_fault, visits, s_visits_at_fault, s_d1, s_d2, s_d3, s_tick_at_fault);
	if(healthy) {
		need(!s_faults && !server_world_light_halted(w) && healthy_effect(k, w), "healthy control completes uninjected");
		assert(server_world_light_drain(w));
		planes(w, &cur);
		sess_close(true);
		tmp.n = 0; disk(s_dir, &tmp);
		need(!same(&tmp, &disk0), "healthy persisting save writes the new world");
		w = sess_open(); planes(w, &tmp);
		need(same(&tmp, &cur), "healthy reopen reloads the saved planes byte-for-byte");
	} else {
		need(s_faults == 1 && s_byte_at_fault && w->light_fault
			&& w->light_fault_reason == SERVER_LIGHT_FAULT_RESERVE && server_world_light_halted(w),
			"real reserve refusal latches the halt (stop byte on every slot, RESERVE cause)");
		bool boundary = k == W_D1 ? s_d1 > 0 : k == W_D2 ? s_d2 > 0 : k == W_D3 ? s_d3 > 0 : s_tick_at_fault;
		need(boundary && s_calls_at_fault == 1, "fault raised inside the first child of the named P1 boundary");
		need(s_calls == s_calls_at_fault, "no later same-tick dispatch at the boundary");
		need(visits == s_visits_at_fault, "no later redstone drain visit");
		state(w, &cur);
		printf("  %s state at fault %016llx after %016llx (%zu bytes)\n", s_name,
			(unsigned long long)fnv(&s_at_fault), (unsigned long long)fnv(&cur), cur.n);
		need(same(&cur, &s_at_fault), "no world/entity/queue/rearm mutation after the fault");
		need(!server_world_set_block(w, 0, 100, 0, (struct block_data){.type = BLOCK_STONE}),
			"a later write is refused");
		struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
		c->modified = true;
		need(!server_world_save_chunk(w, false, 0, 0), "no successful chunk save of the halted world");
		state(w, &tmp);
		need(same(&tmp, &s_at_fault), "refused write/save leave the world unchanged");
		sess_close(true);
		tmp.n = 0; disk(s_dir, &tmp);
		printf("  %s committed disk %016llx after halt %016llx (%zu bytes)\n", s_name,
			(unsigned long long)fnv(&disk0), (unsigned long long)fnv(&tmp), tmp.n);
		first_diff("disk", &disk0, &tmp);
		need(same(&tmp, &disk0), "persisting teardown of the halted world leaves the committed save byte-identical");
		w = sess_open(); planes(w, &tmp);
		printf("  %s committed planes %016llx reopened %016llx\n", s_name,
			(unsigned long long)fnv(&committed), (unsigned long long)fnv(&tmp));
		need(same(&tmp, &committed), "reopen reloads the last committed planes");
	}
	sess_close(false);
	blocks[wrapped] = s_orig[wrapped];
	test_fixture_dir_cleanup(s_dir);
	free(committed.p); free(disk0.p); free(cur.p); free(tmp.p);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	const char* mutant = getenv("CW_REL05_MUTANT");
	if(mutant) {
		bool known = !strcmp(mutant, "all");
		for(int k = 0; k < W_N; k++)
			if(!strcmp(mutant, NAMES[k])) known = true;
		if(!known) {
			printf("test_light_halt_forced: FAIL invalid CW_REL05_MUTANT \"%s\" (want \"all\" or a witness name)\n",
				mutant);
			return 1;
		}
	}
	int witnesses = 0;
	for(int k = 0; k < W_N; k++) {
		s_name = NAMES[k];
		if(mutant) {
			if(strcmp(mutant, "all") && strcmp(mutant, NAMES[k])) continue;
			s_blind = true; run(k, false); s_blind = false;
			witnesses++;
			continue;
		}
		witnesses++;
		run(k, true);
		run(k, false);
		s_blind = s_expect_kill = true; s_killed = 0;
		run(k, false);
		s_blind = s_expect_kill = false;
		need(s_killed > 0, "P1-guard mutant is killed by this witness");
	}
	free(s_at_fault.p);
	if(witnesses == 0) { printf("test_light_halt_forced: FAIL no witness executed\n"); return 1; }
	if(s_fail) { printf("test_light_halt_forced: %d FAIL\n", s_fail); return 1; }
	printf("test_light_halt_forced: PASS\n");
	return 0;
}
