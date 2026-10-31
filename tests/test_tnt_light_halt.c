/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_tnt_light_halt -- REL-TNT-tests (SPEC R5a): powered TNT must never
 * terminate the game when lighting refuses its AIR write; it halts through
 * the committed HALT-STOP P1 machinery instead.
 *
 * Beta order kept (BlockTNT.java:41-53): a powered onBlockAdded /
 * onNeighborBlockChange first calls onBlockDestroyedByPlayer(meta 1) --
 * construct EntityTNTPrimed, entityJoinedWorld, "random.fuse" (68-79) -- and
 * only THEN setBlockWithNotify(AIR). So at a refused AIR write the primed
 * entity is already committed and the TNT block is still there. That
 * partial operation is the expected stopped state: this test never requires
 * AIR or zero entities after the refusal.
 *
 *   added     : server_world_set_block places TNT beside an ON lever; the
 *               real TNT onBlockAdded primes it and its nested AIR write is
 *               the refused light reservation (the outer placement passes).
 *   neighbour : the real lever onRightClick turns an OFF lever on; its
 *               neighbour fan reaches the TNT (-X) first, the real TNT
 *               onNeighbourBlockChange primes it and the AIR write is the
 *               first, refused reservation. The +X trap door must never be
 *               dispatched.
 *
 * The refusal is server_world_test_light_fail_after (the production
 * reserve), never a pre-set halt. abort() is a target-local link wrapper
 * (-Wl,--wrap=abort): ONLY while a forced trigger is armed AND the fault
 * matches the verified shape (one RESERVE fault, raised inside the AIR
 * write at the TNT cell, exactly one new primed entity committed, TNT
 * retained, nothing changed since) does it print "REL-TNT RED:" and _Exit(1).
 * Any other abort prints "REL-TNT SETUP:" (unarmed) or "REL-TNT UNEXPECTED:"
 * and calls the real abort, so an unrelated failure can never pass as RED.
 * server_world_set_block is wrapped only to record which call was refused.
 *
 * GREEN (after the REL-TNT fix) must return from the real callback and pass
 * every containment, save-refusal, persisting-teardown and reopen check.
 * Each witness also runs an uninjected healthy control first: AIR, one
 * primed entity, save + reopen round trip of planes and archived entities.
 * A failed healthy control exits 2 (REL-TNT HEALTHY-FAIL) before the
 * forced witness is armed, and the wrapper refuses RED after any FAIL.
 *
 * Usage: test_tnt_light_halt [--witness added|neighbour]; no argument runs
 * both witnesses. --probe-unarmed-abort / --probe-armed-wrong-shape are
 * wrapper self-probes that must NOT exit 1; --probe-healthy-fail
 * added|neighbour injects one healthy-control FAIL and must exit 2 with
 * REL-TNT HEALTHY-FAIL, never reaching the forced witness. */

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
#include "entity/entity.h"
#include "entity/entity_primed_tnt.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

extern int blocks_test_notify_depth(void);
extern int server_world_test_notify_scope_depth(void);

enum { W_ADDED, W_NEIGHBOUR, W_N };
static const char* const NAMES[W_N] = {"added", "neighbour"};

/* The TNT cell (chunk 0,0) and its ON/OFF lever at +X on a stone floor. */
#define TX 7
#define TY 101
#define TZ 8

static struct server_local S;
static char s_dir[220];
static int s_fail;
static const char* s_name = "setup";

static void need(bool ok, const char* what) {
	if(ok) { printf("  PASS %s: %s\n", s_name, what); return; }
	printf("  FAIL %s: %s\n", s_name, what);
	s_fail++;
}
/* Fixture construction failures are not a RED. */
static void setup(bool ok, const char* what) {
	if(ok) return;
	printf("REL-TNT SETUP: %s: %s\n", s_name, what);
	fflush(stdout);
	exit(4);
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
static void putf(struct buf* b, float v) { put(b, &v, sizeof v); }
static bool same(const struct buf* a, const struct buf* b) {
	return a->n == b->n && !memcmp(a->p, b->p, a->n);
}
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

/* Live entities, field by field, sorted by id: id/type/world/pos/vel and the
 * whole primed-TNT state (fuse, explosion latch, tracker cadence). */
struct esnap {
	uint32_t id; int type; uint8_t world_slot; float pos[3], vel[3];
	int fuse; bool pending; uint8_t corr; bool awaiting;
};
static int cmp_esnap(const void* a, const void* b) {
	uint32_t x = ((const struct esnap*)a)->id, y = ((const struct esnap*)b)->id;
	return x < y ? -1 : x > y;
}
static size_t esnaps(struct esnap* out, size_t cap) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, S.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		assert(n < cap);
		struct esnap* r = &out[n++];
		memset(r, 0, sizeof *r);
		r->id = e->id; r->type = e->type; r->world_slot = e->world_slot;
		memcpy(r->pos, e->pos, sizeof r->pos); memcpy(r->vel, e->vel, sizeof r->vel);
		if(e->type == ENTITY_PRIMED_TNT) {
			r->fuse = e->data.primed_tnt.fuse;
			r->pending = e->data.primed_tnt.explosion_pending;
			r->corr = e->data.primed_tnt.correction_ticks;
			r->awaiting = e->data.primed_tnt.awaiting_destroy;
		}
	}
	qsort(out, n, sizeof *out, cmp_esnap);
	return n;
}
static void entities(struct buf* b) {
	struct esnap e[32];
	size_t n = esnaps(e, 32);
	put64(b, n);
	for(size_t i = 0; i < n; i++) {
		put64(b, e[i].id); put64(b, (uint64_t)e[i].type); put64(b, e[i].world_slot);
		for(int k = 0; k < 3; k++) { putf(b, e[i].pos[k]); putf(b, e[i].vel[k]); }
		put64(b, (uint64_t)e[i].fuse); put64(b, e[i].pending); put64(b, e[i].corr);
		put64(b, e[i].awaiting);
	}
}
static size_t primed_count(void) {
	struct esnap e[32];
	size_t n = esnaps(e, 32), c = 0;
	for(size_t i = 0; i < n; i++) c += e[i].type == ENTITY_PRIMED_TNT;
	return c;
}
/* Planes, entities, pending light and the scheduled ring. */
static void state(struct server_world* w, struct buf* b) {
	planes(w, b);
	entities(b);
	put64(b, w->light_seed_n);
	put64(b, w->scheduled_tick_count);
	for(size_t i = 0; i < w->scheduled_tick_count; i++) {
		const struct scheduled_tick* t = &w->scheduled_ticks[(w->scheduled_tick_head + i) % w->scheduled_tick_capacity];
		put64(b, (uint64_t)t->x); put64(b, (uint64_t)t->y); put64(b, (uint64_t)t->z);
		put64(b, t->block_type); put64(b, t->fire_at_tick); put64(b, t->queue_flags);
	}
}

/* The persisted entity image, through the production chunk archive gather:
 * every archived record of the 9 chunks. source_id is a relation key that
 * a reload never reuses, so it is zeroed; records are sorted bytewise. */
static int cmp_arch(const void* a, const void* b) { return memcmp(a, b, sizeof(struct archived_entity)); }
static void archived(struct server_world* w, struct buf* b) {
	b->n = 0;
	struct archived_entity all[32]; size_t n = 0;
	for(int cx = -1; cx <= 1; cx++) for(int cz = -1; cz <= 1; cz++) {
		struct chunk_entities ce;
		setup(server_local_gather_chunk_entities_w(&S, w, cx, cz, &ce), "gather chunk entities");
		for(size_t i = 0; i < ce.count; i++) {
			assert(n < 32);
			all[n] = ce.entities[i];
			all[n].source_id = 0;
			n++;
		}
		chunk_entities_free(&ce);
	}
	qsort(all, n, sizeof all[0], cmp_arch);
	put64(b, n);
	put(b, all, n * sizeof all[0]);
}
static size_t archived_kind(const struct buf* b, enum archived_entity_kind kind, int* fuse) {
	uint64_t n; memcpy(&n, b->p, sizeof n);
	size_t c = 0;
	for(uint64_t i = 0; i < n; i++) {
		struct archived_entity a;
		memcpy(&a, b->p + sizeof n + i * sizeof a, sizeof a);
		if(a.kind == kind) { c++; if(fuse) *fuse = a.data.primed_tnt.fuse; }
	}
	return c;
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
		setup(server_world_load_chunk(w, cx, cz, &sc) && sc, "load 3x3 chunks");
	}
	setup(server_world_light_drain(w), "initial light drain");
	return w;
}
static void sess_close(bool persist) {
	redstone_server_reset();
	if(persist) server_world_destroy(&S.worlds[0]);
	else server_world_discard(&S.worlds[0]);
	dict_entity_clear(S.entities);
}

static void raw(struct server_world* w, int x, int y, int z, int id, int meta) {
	setup(server_world_set_block_silent(w, x, y, z, (struct block_data){.type = id, .metadata = meta}),
		"raw fixture write");
}
static struct block_data at(struct server_world* w, int x, int y, int z) {
	struct block_data b = {0}; setup(server_world_get_block(w, x, y, z, &b), "read block"); return b;
}

/* added: ON lever (+X) on stone, TNT cell empty, trap door at -X (support
 * stone at x=5) as the later neighbour of the TNT cell.
 * neighbour: committed TNT, OFF lever (+X) on stone, trap door at the
 * lever's +X (support stone at x=10), dispatched after the TNT. Both: a
 * committed anchor minecart, so reopen proves entity restore. */
static void build(int k, struct server_world* w) {
	raw(w, 8, 100, 8, BLOCK_STONE, 0);
	if(k == W_ADDED) {
		raw(w, 8, 101, 8, BLOCK_LEVER, 5 | 8);
		raw(w, 5, 101, 8, BLOCK_STONE, 0); raw(w, 6, 101, 8, BLOCK_TRAP_DOOR, 3);
	} else {
		raw(w, TX, TY, TZ, BLOCK_TNT, 0);
		raw(w, 8, 101, 8, BLOCK_LEVER, 5);
		raw(w, 10, 101, 8, BLOCK_STONE, 0); raw(w, 9, 101, 8, BLOCK_TRAP_DOOR, 2);
	}
	struct server_world* prev = block_world_enter(&S, w);
	setup(server_local_spawn_minecart((vec3){3.5f, 64.5f, 3.5f}, &S) != NULL, "anchor minecart");
	block_world_leave(&S, prev);
}

/* --- counting wrappers: always delegate to production --------------------- */
static struct block* s_orig[256];
static struct block s_copy[256];
static unsigned s_added, s_nb[256], s_nb_total;
static void wrap_nb(struct server_local* s, struct block_info* i) {
	s_nb[i->block->type]++; s_nb_total++;
	s_orig[i->block->type]->onNeighbourBlockChange(s, i);
}
static void wrap_added(struct server_local* s, struct block_info* i) {
	s_added++; s_orig[i->block->type]->onBlockAdded(s, i);
}
static void wrap(uint8_t id) {
	s_orig[id] = blocks[id]; s_copy[id] = *blocks[id];
	if(s_copy[id].onNeighbourBlockChange) s_copy[id].onNeighbourBlockChange = wrap_nb;
	if(id == BLOCK_TNT) s_copy[id].onBlockAdded = wrap_added;
	blocks[id] = &s_copy[id];
}
static void unwrap(uint8_t id) { blocks[id] = s_orig[id]; }

/* server_world_set_block link wrapper: a call stack of requested writes. */
struct frame { int x, y, z, type; };
static struct frame s_stack[64];
static int s_depth;
static unsigned s_set_calls;
bool __real_server_world_set_block(struct server_world* w, w_coord_t x, w_coord_t y,
	w_coord_t z, struct block_data blk);
bool __wrap_server_world_set_block(struct server_world* w, w_coord_t x, w_coord_t y,
	w_coord_t z, struct block_data blk) {
	s_set_calls++;
	if(s_depth < 64) s_stack[s_depth] = (struct frame){(int)x, (int)y, (int)z, blk.type};
	s_depth++;
	bool ok = __real_server_world_set_block(w, x, y, z, blk);
	s_depth--;
	return ok;
}

/* --- the fault snapshot ---------------------------------------------------- */
static int s_witness;
static bool s_armed;
static int s_faults;
static struct frame s_fault_frame;
static int s_fault_depth, s_fault_d1, s_fault_d2;
static uint8_t s_fault_reason;
static bool s_fault_bytes;
static struct block_data s_fault_block;
static size_t s_pre_entities, s_fault_entities, s_fault_primed;
static struct esnap s_fault_prime;
static unsigned s_fault_added, s_fault_nb_total, s_fault_set_calls;
static unsigned s_fault_nb_tnt, s_fault_nb_lever, s_fault_nb_trap;
static struct buf s_at_fault, s_ent_fault;

static bool all_stop_bytes(void) {
	for(int i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++)
		if(!S.worlds[i].simulation_stopped) return false;
	return true;
}
static bool find_prime(struct esnap* out) {
	struct esnap e[32]; size_t n = esnaps(e, 32); bool found = false;
	for(size_t i = 0; i < n; i++)
		if(e[i].type == ENTITY_PRIMED_TNT) { *out = e[i]; found = true; }
	return found;
}
static void on_fault(struct server_world* w) {
	if(s_faults++) return;
	s_fault_depth = s_depth;
	if(s_depth > 0 && s_depth <= 64) s_fault_frame = s_stack[s_depth - 1];
	s_fault_d1 = blocks_test_notify_depth(); s_fault_d2 = server_world_test_notify_scope_depth();
	s_fault_reason = w->light_fault_reason;
	s_fault_bytes = all_stop_bytes();
	(void)server_world_get_block(w, TX, TY, TZ, &s_fault_block);
	struct esnap e[32];
	s_fault_entities = esnaps(e, 32);
	s_fault_primed = primed_count();
	memset(&s_fault_prime, 0, sizeof s_fault_prime);
	(void)find_prime(&s_fault_prime);
	s_fault_added = s_added; s_fault_nb_total = s_nb_total; s_fault_set_calls = s_set_calls;
	s_fault_nb_tnt = s_nb[BLOCK_TNT]; s_fault_nb_lever = s_nb[BLOCK_LEVER];
	s_fault_nb_trap = s_nb[BLOCK_TRAP_DOOR];
	s_at_fault.n = 0; state(w, &s_at_fault);
	s_ent_fault.n = 0; entities(&s_ent_fault);
}

static bool fault_frame_is_tnt_air(void) {
	return s_fault_frame.x == TX && s_fault_frame.y == TY && s_fault_frame.z == TZ
		&& s_fault_frame.type == BLOCK_AIR
		&& s_fault_depth == (s_witness == W_ADDED ? 2 : 1);
}
static bool prime_ok(const struct esnap* p) {
	return p->type == ENTITY_PRIMED_TNT && p->id != 0 && p->fuse == TNT_FUSE_DEFAULT
		&& !p->pending && p->world_slot == 0
		&& p->pos[0] == TX + 0.5f && p->pos[1] == TY + 0.5f && p->pos[2] == TZ + 0.5f;
}
/* The verified fault shape at the refused AIR write. */
static bool fault_shape(void) {
	return s_faults == 1 && s_fault_reason == SERVER_LIGHT_FAULT_RESERVE && s_fault_bytes
		&& fault_frame_is_tnt_air() && s_fault_block.type == BLOCK_TNT
		&& s_fault_primed == 1 && s_fault_entities == s_pre_entities + 1 && prime_ok(&s_fault_prime);
}

void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void) {
	fflush(stderr);
	if(!s_armed) {
		printf("REL-TNT SETUP: abort outside an armed forced trigger (witness %s)\n", s_name);
		fflush(stdout);
		__real_abort();
	}
	s_armed = false;
	/* Only a recorded fault implies a live fixture worth reading; any
	 * preceding FAIL means the run is not a clean RED. */
	struct block_data now = {0};
	bool shape = s_fail == 0 && fault_shape();
	if(shape) {
		struct buf ent_now = {0};
		(void)server_world_get_block(&S.worlds[0], TX, TY, TZ, &now);
		entities(&ent_now);
		shape = now.type == BLOCK_TNT && same(&ent_now, &s_ent_fault);
	}
	if(shape) {
		printf("REL-TNT RED: powered TNT AIR refusal reached reservation violation "
			"(witness %s: RESERVE fault inside the AIR write at (%d,%d,%d), set_block depth %d; "
			"primed entity id=%u type=%d fuse=%d committed, TNT retained, process aborted)\n",
			s_name, TX, TY, TZ, s_fault_depth, s_fault_prime.id, s_fault_prime.type, s_fault_prime.fuse);
		fflush(stdout);
		_Exit(1);
	}
	printf("REL-TNT UNEXPECTED: abort while armed is not the verified TNT AIR refusal "
		"(witness %s: prior FAILs=%d faults=%d reason=%u frame=(%d,%d,%d type %d) depth=%d block=%d primed=%zu "
		"entities=%zu/%zu now=%d)\n",
		s_name, s_fail, s_faults, s_fault_reason, s_fault_frame.x, s_fault_frame.y, s_fault_frame.z,
		s_fault_frame.type, s_fault_depth, s_fault_block.type, s_fault_primed, s_fault_entities,
		s_pre_entities, now.type);
	fflush(stdout);
	__real_abort();
}

static void trigger(int k, struct server_world* w) {
	struct server_world* prev = block_world_enter(&S, w);
	if(k == W_ADDED) {
		(void)server_world_set_block(w, TX, TY, TZ, (struct block_data){.type = BLOCK_TNT});
	} else {
		struct block_data lb = at(w, 8, 101, 8);
		struct block_info on = {.block = &lb, .x = 8, .y = 101, .z = 8};
		blocks[BLOCK_LEVER]->onRightClick(&S, NULL, NULL, &on, &on, SIDE_TOP);
	}
	block_world_leave(&S, prev);
}

static void run(int k, bool healthy) {
	char id[64]; snprintf(id, sizeof id, "test_tnt_light_halt_%d_%d", k, healthy);
	test_fixture_dir(id, s_dir, sizeof s_dir);
	MKDIR(s_dir);
	char region[300]; snprintf(region, sizeof region, "%s/region", s_dir);
	MKDIR(region);
	struct buf committed = {0}, committed_ent = {0}, disk0 = {0}, cur = {0}, tmp = {0};
	/* 1. build and COMMIT the fixture (persisting teardown, handles closed). */
	struct server_world* w = sess_open();
	build(k, w);
	setup(server_world_light_drain(w), "fixture light drain");
	sess_close(true);
	disk(s_dir, &disk0);
	/* 2. reopen: the committed planes and entities as loaded. */
	w = sess_open();
	planes(w, &committed);
	archived(w, &committed_ent);
	setup(archived_kind(&committed_ent, ARCHIVED_ENTITY_MINECART, NULL) == 1
		&& archived_kind(&committed_ent, ARCHIVED_ENTITY_PRIMED_TNT, NULL) == 0,
		"committed fixture restores exactly the anchor minecart");
	setup(at(w, TX, TY, TZ).type == (k == W_ADDED ? BLOCK_AIR : BLOCK_TNT), "committed TNT cell");
	{ struct esnap e[32]; s_pre_entities = esnaps(e, 32); }
	wrap(BLOCK_TNT); wrap(BLOCK_LEVER); wrap(BLOCK_TRAP_DOOR);
	s_added = s_nb_total = s_set_calls = 0; memset(s_nb, 0, sizeof s_nb);
	s_faults = 0; s_depth = 0; s_at_fault.n = s_ent_fault.n = 0;
	memset(&s_fault_frame, 0, sizeof s_fault_frame);
	server_world_test_light_fault_hook = on_fault;
	/* added: the outer TNT placement reserves first, the nested AIR second. */
	server_world_test_light_fail_after(healthy ? -1 : (k == W_ADDED ? 1 : 0));
	s_armed = !healthy;
	trigger(k, w);
	s_armed = false;
	server_world_test_light_fail_after(-1);
	server_world_test_light_fault_hook = NULL;
	printf("%s %s: faults=%d added=%u nb(tnt/lever/trap)=%u/%u/%u set_calls=%u | at fault: depth=%d "
		"frame=(%d,%d,%d type %d) notify D1/D2=%d/%d primed=%zu entities=%zu (pre %zu)\n",
		s_name, healthy ? "healthy" : "forced", s_faults, s_added, s_nb[BLOCK_TNT], s_nb[BLOCK_LEVER],
		s_nb[BLOCK_TRAP_DOOR], s_set_calls, s_fault_depth, s_fault_frame.x, s_fault_frame.y,
		s_fault_frame.z, s_fault_frame.type, s_fault_d1, s_fault_d2, s_fault_primed,
		s_fault_entities, s_pre_entities);
	const bool fired = k == W_ADDED ? s_added == 1 : s_nb[BLOCK_TNT] == 1;
	if(healthy) {
		struct esnap p = {0};
		need(!s_faults && !server_world_light_halted(w) && fired, "healthy control runs the real powered callback uninjected");
		need(at(w, TX, TY, TZ).type == BLOCK_AIR, "healthy control writes AIR at the TNT cell");
		need(primed_count() == 1 && find_prime(&p) && prime_ok(&p),
			"healthy control commits exactly one primed entity (fuse 80, at the cell centre)");
		if(k == W_NEIGHBOUR)
			need(s_nb[BLOCK_TRAP_DOOR] > 0 && (at(w, 9, 101, 8).metadata & 4),
				"healthy lever fan continues to the +X trap door");
		setup(server_world_light_drain(w), "healthy light drain");
		planes(w, &cur);
		struct buf ent = {0};
		archived(w, &ent);
		int fuse = -1;
		need(archived_kind(&ent, ARCHIVED_ENTITY_PRIMED_TNT, &fuse) == 1 && fuse == TNT_FUSE_DEFAULT
			&& archived_kind(&ent, ARCHIVED_ENTITY_MINECART, NULL) == 1,
			"healthy archive image holds the primed TNT (fuse 80) and the anchor");
		unwrap(BLOCK_TNT); unwrap(BLOCK_LEVER); unwrap(BLOCK_TRAP_DOOR);
		sess_close(true);
		tmp.n = 0; disk(s_dir, &tmp);
		need(!same(&tmp, &disk0), "healthy persisting save writes the new world");
		w = sess_open(); planes(w, &tmp);
		need(same(&tmp, &cur), "healthy reopen reloads the saved planes byte-for-byte");
		archived(w, &tmp);
		first_diff("entities", &ent, &tmp);
		need(same(&tmp, &ent), "healthy reopen restores the archived entities (primed TNT + anchor) exactly");
		free(ent.p);
	} else {
		/* Reaching here means the real callback RETURNED (no abort). */
		need(true, "the real powered callback returned without terminating");
		need(fired, "the real powered callback was dispatched exactly once");
		need(fault_shape(), "one RESERVE fault raised inside the AIR write at the TNT cell, "
			"entity committed first (Beta order), TNT retained");
		printf("  %s prime at fault id=%u type=%d fuse=%d pos=(%.2f,%.2f,%.2f)\n", s_name,
			s_fault_prime.id, s_fault_prime.type, s_fault_prime.fuse,
			s_fault_prime.pos[0], s_fault_prime.pos[1], s_fault_prime.pos[2]);
		need(server_world_light_halted(w) && w->light_fault
			&& w->light_fault_reason == SERVER_LIGHT_FAULT_RESERVE && all_stop_bytes(),
			"halt latched; every session stop byte still set after return");
		need(at(w, TX, TY, TZ).type == BLOCK_TNT, "TNT block retained (no raw AIR write)");
		cur.n = 0; entities(&cur);
		need(same(&cur, &s_ent_fault) && primed_count() == 1,
			"exactly the one committed primed entity, identical to the fault snapshot");
		need(s_set_calls == s_fault_set_calls && s_added == s_fault_added,
			"no retry: no later set_block call and no second prime");
		need(s_nb_total == s_fault_nb_total, "no later P1 neighbour dispatch");
		if(k == W_NEIGHBOUR)
			need(s_fault_d1 > 0 && s_fault_nb_tnt == 1 && s_nb[BLOCK_TRAP_DOOR] == 0
				&& !(at(w, 9, 101, 8).metadata & 4),
				"fault inside the lever fan's first child; the +X trap door is never dispatched");
		else
			need(s_nb_total == 0, "the placement's own neighbour fan never runs");
		state(w, &tmp);
		printf("  %s state at fault %016llx after %016llx (%zu bytes)\n", s_name,
			(unsigned long long)fnv(&s_at_fault), (unsigned long long)fnv(&tmp), tmp.n);
		need(same(&tmp, &s_at_fault), "no world/entity/queue mutation after the fault");
		need(!server_world_set_block(w, 0, 100, 0, (struct block_data){.type = BLOCK_STONE}),
			"a later lit write is refused");
		struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
		c->modified = true;
		need(!server_world_save_chunk(w, false, 0, 0), "no successful chunk save of the halted world");
		tmp.n = 0; state(w, &tmp);
		need(same(&tmp, &s_at_fault), "refused write/save leave the world unchanged");
		unwrap(BLOCK_TNT); unwrap(BLOCK_LEVER); unwrap(BLOCK_TRAP_DOOR);
		sess_close(true);
		tmp.n = 0; disk(s_dir, &tmp);
		printf("  %s committed disk %016llx after halt %016llx (%zu bytes)\n", s_name,
			(unsigned long long)fnv(&disk0), (unsigned long long)fnv(&tmp), tmp.n);
		first_diff("disk", &disk0, &tmp);
		need(same(&tmp, &disk0), "persisting teardown of the halted world leaves the committed save byte-identical");
		w = sess_open(); planes(w, &tmp);
		need(same(&tmp, &committed), "reopen reloads the last committed planes");
		archived(w, &tmp);
		first_diff("entities", &committed_ent, &tmp);
		need(same(&tmp, &committed_ent) && primed_count() == 0,
			"reopen restores the committed entities (anchor only), no leaked primed TNT");
	}
	sess_close(false);
	test_fixture_dir_cleanup(s_dir);
	free(committed.p); free(committed_ent.p); free(disk0.p); free(cur.p); free(tmp.p);
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	int only = -1;
	bool probe_healthy_fail = false;
	/* Wrapper self-probes (receipts that the abort wrapper cannot bless an
	 * unrelated failure): an unarmed abort must report SETUP, an armed abort
	 * without the verified fault shape must report UNEXPECTED; both then take
	 * the real abort, so neither can exit 1. */
	if(argc == 2 && !strcmp(argv[1], "--probe-unarmed-abort")) {
		s_name = "probe-unarmed";
		abort();
	}
	if(argc == 2 && !strcmp(argv[1], "--probe-armed-wrong-shape")) {
		s_name = "probe-armed";
		s_armed = true;
		abort();
	}
	if(argc == 3 && !strcmp(argv[1], "--probe-healthy-fail")) {
		probe_healthy_fail = true;
		for(int k = 0; k < W_N; k++)
			if(!strcmp(argv[2], NAMES[k])) only = k;
	}
	if(argc == 3 && !strcmp(argv[1], "--witness")) {
		for(int k = 0; k < W_N; k++)
			if(!strcmp(argv[2], NAMES[k])) only = k;
	}
	if(argc != 1 && only < 0) {
		printf("REL-TNT SETUP: usage: test_tnt_light_halt [--witness added|neighbour]\n");
		return 4;
	}
	setup(clin_init() && svin_init(), "client/server inbox init");
	struct thread drain;
	setup(thread_create(&drain, drain_entry, NULL, 64), "drain thread");
	blocks_init();
	items_init();
	int witnesses = 0;
	for(int k = 0; k < W_N; k++) {
		if(only >= 0 && k != only) continue;
		s_witness = k; s_name = NAMES[k];
		witnesses++;
		run(k, true);
		if(probe_healthy_fail) need(false, "probe: injected healthy-control failure");
		/* A failed healthy control invalidates the fixture: stop with a
		 * non-RED exit BEFORE the forced witness can be armed. */
		if(s_fail) {
			printf("REL-TNT HEALTHY-FAIL: %s healthy control failed %d check(s); forced witness not armed\n",
				s_name, s_fail);
			printf("test_tnt_light_halt: %d FAIL\n", s_fail);
			return 2;
		}
		run(k, false);
	}
	free(s_at_fault.p); free(s_ent_fault.p);
	if(witnesses == 0) { printf("REL-TNT SETUP: no witness executed\n"); return 4; }
	if(s_fail) { printf("test_tnt_light_halt: %d FAIL\n", s_fail); return 2; }
	printf("test_tnt_light_halt: PASS\n");
	return 0;
}
