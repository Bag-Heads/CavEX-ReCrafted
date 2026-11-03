/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include "../platform/watchdog.h"
#include "../platform/hw_telemetry.h"
#include "../platform/g9_worst_tick.h"
#include "../platform/service_observer.h"
#include "../tps_observer.h"
tps_callback world_tps_observe;
tps_scan_callback world_tps_scan;
tps_mutation_callback world_tps_mutation;
tps_light_callback world_tps_light;

#include <assert.h>
#include <errno.h>
#include <math.h> // floor() -- audit #24 player pressure-plate feet-cell probe
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef PLATFORM_WII
#include <ogc/irq.h> // IRQ_Disable: 64-bit RMW without __atomic_*_8
#include <ogc/system.h> // G9X-5b preflight: SYS_GetArena1Size / SYS_GetArena2Size
#endif

#include "../block/aabb.h"
#include "../entity/actor_ident.h"
#include "../entity/entity_boat.h" // BOAT_DESTROY_DAMAGE_THRESHOLD
#include "../entity/combat.h"
#include "../game/difficulty.h" // monster_is_beta_entity_mob
#include "../entity/entity_minecart.h"
#include "../entity/entity_monster.h"
#include "../entity/entity_painting.h" // entity_painting_hit_box / mark_broken
#include "../entity/entity_wolf.h"
#include "../lighting.h"
#include "../platform/audio.h" // S0: SOUND_RANDOM_EXPLODE
#include "../platform/mem2.h"
#include "../platform/thread.h"
#include "../platform/g9_probe.h"
#include "../platform/g9_capture.h"
#include "../util.h"
#include "../worldgen/decorate.h"
#include "../worldgen/java_random.h"
#include "../worldgen/jmath.h"
#include "../worldgen/populate.h"
#include "../worldgen/worldgen.h"
#include "../block/block_pressure_plate.h" // audit #24: block_pressure_plate_on_entity_tick
#include "../block/fluid_block.h"
#include "../block/fluid_spread.h"
#include "../block/redstone_server.h" // G5 F1: redstone_server_service_pending
#include "client_interface.h"
#include "entity_archive.h"
#include "explosion_model.h"
#include "server_dispenser.h"
#include "server_furnace.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)
#include "server_spawner.h"
#include "server_world.h"
#ifdef PLATFORM_WII
#include "save_path.h"
#endif
#include "../column_observer.h"
#include "../column_view.h"

// Kept out of server_local.h to avoid making every block translation unit
// depend on this explosion-only chance seam.
extern void server_local_spawn_block_drops_with_chance(
	struct server_world* w, struct block_info* blk_info, float chance);
#include "tile_entities_store.h"
#include "../daytime.h"
#include "../profile.h"

/* G9B: state belongs solely to an armed server-thread capture. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
bool (*server_world_test_history_hook)(const struct server_world_history_event*);
static uint64_t s_history_next, s_history_stack[128];
static struct server_world* s_history_worlds[128];
static const char* s_history_operations[128];
static unsigned s_history_depth;
static bool s_history_failed;
void server_world_test_history_reset(void) {
	s_history_next = s_history_depth = 0;
	s_history_failed = false;
}
bool server_world_test_history_failed(void) { return s_history_failed; }
static void server_world_history_emit(struct server_world* w,
	const char* op, const char* edge, const char* outcome,
	uint64_t request, uint64_t parent, w_coord_t x, w_coord_t z,
	const struct server_chunk* chunk) {
	if(!server_world_test_history_hook) return;
	struct server_world_history_event e = {
		.world = w, .session = w ? w->session : NULL, .chunk = chunk,
		.operation = op, .edge = edge, .outcome = outcome,
		.request = request, .parent = parent, .x = x, .z = z,
		.generation = w ? w->generation : 0,
		.slot = w ? w->world_index : 0,
		.dimension = w ? (int)w->dimension : 0,
		.hell_rng = w ? w->hell_populate_rng.seed : 0,
	};
	if(e.session) {
		e.update = e.session->near_send_seq;
		e.session_rng = e.session->rand_src.seed;
	}
	if(!server_world_test_history_hook(&e)) s_history_failed = true;
}
uint64_t server_world_test_history_begin(struct server_world* w,
	const char* op, w_coord_t x, w_coord_t z) {
	if(!server_world_test_history_hook) return 0;
	if(s_history_depth == 128 || s_history_next == UINT64_MAX) {
		s_history_failed = true;
		return 0;
	}
	uint64_t parent = s_history_depth ? s_history_stack[s_history_depth - 1] : 0;
	uint64_t request = ++s_history_next;
	s_history_stack[s_history_depth++] = request;
	s_history_worlds[s_history_depth - 1] = w;
	s_history_operations[s_history_depth - 1] = op;
	server_world_history_emit(w, op, "begin", "", request, parent, x, z, NULL);
	return request;
}
void server_world_test_history_end(struct server_world* w,
	const char* op, uint64_t request, const char* outcome, w_coord_t x, w_coord_t z) {
	if(!request) return;
	if(!s_history_depth || s_history_stack[s_history_depth - 1] != request) {
		s_history_failed = true;
		return;
	}
	s_history_depth--;
	uint64_t parent = s_history_depth ? s_history_stack[s_history_depth - 1] : 0;
	server_world_history_emit(w, op, "end", outcome, request, parent, x, z, NULL);
}
void server_world_test_history_note(struct server_world* w,
	const char* op, const char* edge, const char* outcome,
	w_coord_t x, w_coord_t z, const struct server_chunk* chunk) {
	if(!server_world_test_history_hook) return;
	uint64_t request = s_history_depth ? s_history_stack[s_history_depth - 1] : 0;
	server_world_history_emit(w, op, edge, outcome, request, request, x, z, chunk);
}
static void server_world_history_attempt(struct server_world* w,
	w_coord_t x, w_coord_t y, w_coord_t z, int type, int metadata) {
	if(!server_world_test_history_hook) return;
	const struct server_chunk* sc = w && y >= 0 && y < WORLD_HEIGHT
		? dict_server_chunks_get(w->chunks, S_CHUNK_ID(
			WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z))) : NULL;
	const unsigned index = sc ? (((unsigned)x & 15u) << 11)
		| (((unsigned)z & 15u) << 7) | (unsigned)y : 0;
	struct server_world_history_event e = {
		.world = w, .session = w ? w->session : NULL, .chunk = sc,
		.operation = "setter-attempt", .edge = "attempt", .outcome = "",
		.request = s_history_depth ? s_history_stack[s_history_depth - 1] : 0,
		.x = x, .y = y, .z = z,
		.before_id = sc ? sc->ids[index] : -1,
		.before_meta = sc ? (sc->metadata[index / 2]
			>> ((index & 1u) * 4)) & 15 : -1,
		.after_id = type, .after_meta = metadata,
		.generation = w ? w->generation : 0,
		.slot = w ? w->world_index : 0,
		.dimension = w ? (int)w->dimension : 0,
		.hell_rng = w ? w->hell_populate_rng.seed : 0,
	};
	if(e.session) {
		e.update = e.session->near_send_seq;
		e.session_rng = e.session->rand_src.seed;
	}
	if(!server_world_test_history_hook(&e)) s_history_failed = true;
}
#define G9B_ATTEMPT(w, x, y, z, type, meta) \
	server_world_history_attempt(w, x, y, z, type, meta)
#else
#define G9B_ATTEMPT(w, x, y, z, type, meta) ((void)0)
#endif

/* Opt-in hardware attribution. Server-thread writers, atomic snapshots.
 * LOAD includes TERRAIN and REARM; these nested elapsed intervals are not
 * additive CPU times. Compare uint32 deltas modulo 2^32 within 71 minutes. */
static bool s_deep_profile_enabled;
static uint32_t s_deep_us[8];
void server_world_work_profile_enable(bool enabled) {
	s_deep_profile_enabled = enabled;
	region_archive_work_profile_enable(enabled);
}
void server_world_work_stats(uint32_t out[14]) {
	for(int i = 0; i < 8; i++)
		out[i] = __atomic_load_n(&s_deep_us[i], __ATOMIC_RELAXED);
	region_archive_work_stats(out + 8);
}
static ptime_t deep_start(void) { return s_deep_profile_enabled ? time_get() : (ptime_t){0}; }
static void deep_end(int part, ptime_t start) {
	if(s_deep_profile_enabled)
		__atomic_fetch_add(&s_deep_us[part], (uint32_t)(time_diff_s(start, time_get()) * 1e6f), __ATOMIC_RELAXED);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define CHUNK_DIST2(x1, x2, z1, z2)                                            \
	(((x1) - (x2)) * ((x1) - (x2)) + ((z1) - (z2)) * ((z1) - (z2)))

#define S_CHUNK_IDX(x, y, z)                                                   \
	((y) + (W2C_COORD(z) + W2C_COORD(x) * CHUNK_SIZE) * WORLD_HEIGHT)

/* One populate() transaction can touch any cell in its four-chunk quadrant.
 * Keep a byte-exact block/metadata image of all four chunks in a session-owned
 * rollback base. The fixed 192 KiB reservation is made before world streaming;
 * begin/rollback are memcpy-only and never enter the fragmented allocator. */
#define QUAD_CHUNK_CELL_COUNT                                                \
	((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define QUAD_POPULATE_CELL_COUNT (4u * QUAD_CHUNK_CELL_COUNT)
#define QUAD_CHUNK_METADATA_BYTES (QUAD_CHUNK_CELL_COUNT / 2u)

struct server_world_populate_scratch {
	uint8_t ids[4][QUAD_CHUNK_CELL_COUNT];
	uint8_t metadata[4][QUAD_CHUNK_METADATA_BYTES];
	/* R8: plain Nether population stores height/Sky inside the transaction. */
	uint8_t sky[4][QUAD_CHUNK_METADATA_BYTES];
	uint8_t height[4][CHUNK_SIZE * CHUNK_SIZE];
	bool attached;
	bool in_use;
};

#define QUAD_POPULATE_SCRATCH_BYTES                                        \
	sizeof(struct server_world_populate_scratch)

#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
static size_t quad_populate_test_scratch_claims;
static size_t quad_populate_test_committed_writes;
static size_t quad_populate_test_max_writes;
#endif

// View server_local's chest/sign tables (Structure-of-Arrays) as a te_store for
// the per-chunk tile-entity lifecycle. complex_block_pos is {int x,y,z}, layout-
// identical to te_pos, so the position arrays alias directly; the static assert
// guards that invariant. This is the only coupling point between the world's
// chunk lifecycle and the chest store.
// Compile-time guard (C99-clean): te_pos must match complex_block_pos's layout
// (both {int x,y,z}). The cast in server_world_te_store reinterprets the
// position arrays, so a size change here must fail the build, not corrupt data.
typedef char te_pos_layout_guard[
	(sizeof(struct complex_block_pos) == sizeof(struct te_pos)) ? 1 : -1];

/* A1b: the chunk lifecycle reaches the session's shared tile tables only
 * through server_world_tile_store(w) (server_local_state.h), the world-keyed
 * production view: restore/gather/release see and stamp only `w`'s rows. */

// Single canonical zero-init for a to-be-constructed struct server_chunk.
// Boot-hang audit (2026-09-02, agreed with peer session): every current
// construction site already reaches zero for near_send_fail_streak/
// near_send_fail_seq (the disk-load site's `(struct server_chunk){.modified =
// false}` compound literal zero-initializes every unlisted member per
// C11 6.7.9p19-21; the generation site's local `tmp` is fully memset(0) as
// worldgen_generate_chunk's very first statement, before any field is set;
// the far-skin-extract site already memset(0)s explicitly) -- so no live
// uninitialized-read bug was found. But three of those sites relied on that
// guarantee IMPLICITLY (a designated initializer's "rest is zero" rule, or
// an unrelated callee's internal memset) rather than stating it at the
// construction site itself -- fragile should either ever change. This gives
// every site (including this one, which now calls it instead of its own
// inline memset) one explicit, self-documenting zeroing call instead of
// three different implicit routes to the same guarantee.
void server_chunk_init(struct server_chunk* sc) {
	assert(sc);
	memset(sc, 0, sizeof(*sc));
}

void (*server_column_observe_chunk)(struct server_world*, int32_t, int32_t,
	const struct server_chunk*, bool);

/* Only 32-bit atomics: Broadway has no 64-bit libatomic runtime. Refuse a
 * wrapped identity rather than allowing a stale lease to alias a new world. */
static uint32_t s_backing_world_instance;
/* A1a: process-unique world-instance generations (struct server_world_handle).
 * 32-bit atomics only (Broadway); a wrap would alias a stale handle, refuse. */
static uint32_t s_world_generation;
static struct thread_mutex s_backing_locks[COLUMN_BACKING_LOCK_STRIPES];
static struct thread_mutex s_light_drain_hb_lock;
static uint32_t s_backing_locks_state; // 0 uninitialized, 1 initializing, 2 ready

bool column_backing_init(void) {
	for(;;) {
		uint32_t state = __atomic_load_n(&s_backing_locks_state, __ATOMIC_ACQUIRE);
		if(state == 2) return true;
		if(state == 0 && __atomic_compare_exchange_n(&s_backing_locks_state,
			&state, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
		thread_msleep(1); // boot only; no backing/allocator lock is held
	}
	unsigned n = 0;
	while(n < COLUMN_BACKING_LOCK_STRIPES && thread_mutex_init(&s_backing_locks[n])) {
		thread_stripe_register(&s_backing_locks[n], n);
		thread_stripe_set_name(&s_backing_locks[n], "column_backing");
		n++;
	}
	bool ok = n == COLUMN_BACKING_LOCK_STRIPES
		&& thread_mutex_init(&s_light_drain_hb_lock);
	if(!ok)
		while(n) thread_mutex_destroy(&s_backing_locks[--n]);
	__atomic_store_n(&s_backing_locks_state, ok ? 2 : 0, __ATOMIC_RELEASE);
	return ok;
}

/* Transition counters include unpublished and worker/GPU-held backings.
 * Existing stripes serialize each refcount; atomics combine different stripes. */
static uint32_t s_backing_live, s_backing_shared;
void column_backing_get_stats(uint32_t* live, uint32_t* shared) {
	*live = __atomic_load_n(&s_backing_live, __ATOMIC_RELAXED);
	*shared = __atomic_load_n(&s_backing_shared, __ATOMIC_RELAXED);
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static uint32_t s_backing_created, s_backing_destroyed, s_backing_references;
static unsigned s_backing_fail_next;
void column_backing_test_fail_next(void) {
	s_backing_fail_next = 1;
}
void column_backing_test_counts(uint32_t* created, uint32_t* destroyed,
	uint32_t* references) {
	*created = __atomic_load_n(&s_backing_created, __ATOMIC_RELAXED);
	*destroyed = __atomic_load_n(&s_backing_destroyed, __ATOMIC_RELAXED);
	*references = __atomic_load_n(&s_backing_references, __ATOMIC_RELAXED);
}
#endif

void column_backing_ref(struct column_backing* b) {
	if(!b) return;
	thread_stripe_lock(b->mutex);
	if(!b->references || b->references == UINT32_MAX) abort();
	if(b->references++ == 1)
		__atomic_fetch_add(&s_backing_shared, 1, __ATOMIC_RELAXED);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	__atomic_fetch_add(&s_backing_references, 1, __ATOMIC_RELAXED);
#endif
	thread_stripe_unlock(b->mutex);
}

void column_backing_unref(struct column_backing* b) {
	if(!b) return;
	thread_stripe_lock(b->mutex);
	if(!b->references) abort();
	bool destroy = --b->references == 0;
	if(b->references == 1)
		__atomic_fetch_sub(&s_backing_shared, 1, __ATOMIC_RELAXED);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	__atomic_fetch_sub(&s_backing_references, 1, __ATOMIC_RELAXED);
#endif
	thread_stripe_unlock(b->mutex);
	if(!destroy) return;
	/* No owner can acquire from zero. Destruction and allocator locks occur
	 * AFTER unlocking, never inside the backing critical section. */
	/* The backing's arrays are the chunk's arrays (adopted): one pooled slot
	 * when arrays_slot was set at adoption, five arrays otherwise. */
	if(b->arrays_slot) {
		mem2_free_sized(b->ids, MEM2_POOL_COLUMN_OBJECT);
	} else {
		mem2_free_sized(b->ids, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
		mem2_free_sized(b->metadata, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
		mem2_free_sized(b->lighting_sky, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
		mem2_free_sized(b->lighting_torch, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
		mem2_free_sized(b->heightmap, CHUNK_SIZE * CHUNK_SIZE);
	}
#ifdef CAVEWII_SERVER_LOCAL_TEST
	__atomic_fetch_add(&s_backing_destroyed, 1, __ATOMIC_RELAXED);
#endif
	/* Keep this allocation counted until its arrays are actually freed. A
	 * server admission may race a client/worker's final reference release. */
	__atomic_fetch_sub(&s_backing_live, 1, __ATOMIC_RELEASE);
	free(b);
}

uint32_t column_backing_references(struct column_backing* b) {
	assert(b);
	thread_stripe_lock(b->mutex);
	uint32_t count = b->references;
	thread_stripe_unlock(b->mutex);
	return count;
}

bool column_backing_matches(const struct column_backing* b,
	uint32_t world_instance, enum world_dim dimension, uint32_t generation) {
	return b && b->world_instance == world_instance && b->dimension == dimension
		&& b->generation == generation;
}

/* Called before dictionary publication, after disk/generation succeeds.
 * Allocation failure leaves the unpublished arrays owned by sc for unwind. */
static bool server_world_adopt_backing(struct server_world* w,
	struct server_chunk* sc, int32_t x, int32_t z) {
	assert(!sc->backing);
	/* The legacy void world constructor may leave a failed boot world
	 * available to a disk-load caller. Never publish an uninitialized stripe. */
	if(__atomic_load_n(&s_backing_locks_state, __ATOMIC_ACQUIRE) != 2)
		return false;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	unsigned fail = s_backing_fail_next;
	s_backing_fail_next = 0;
	if(fail == 1) return false;
#endif
	struct column_backing* b = calloc(1, sizeof(*b));
	if(!b) return false;
	/* Unsigned column-key hash; dimension/generation deliberately share the
	 * same stripe. No per-backing kernel object or fixed-slot consumption. */
	uint32_t hash = (uint32_t)x * UINT32_C(0x9e3779b1) ^ (uint32_t)z;
	hash ^= hash >> 16;
	b->mutex = &s_backing_locks[hash & (COLUMN_BACKING_LOCK_STRIPES - 1)];
	if(++w->backing_generation == 0) abort();
	b->ids = sc->ids;
	b->metadata = sc->metadata;
	b->lighting_sky = sc->lighting_sky;
	b->lighting_torch = sc->lighting_torch;
	b->heightmap = sc->heightmap;
	b->arrays_slot = sc->arrays_slot; /* ownership shape travels with the arrays */
	b->world_instance = w->backing_world_instance;
	b->dimension = w->dimension;
	b->generation = w->backing_generation;
	b->references = 1; // transferred, without increment, to the dictionary
	b->revision = (struct column_revision){w->backing_world_instance,
		b->generation, 1, 1, 1};
	b->resident = true;
	column_light_mutated(NULL, false);
	__atomic_fetch_add(&s_backing_live, 1, __ATOMIC_RELAXED);
	sc->backing = b;
	/* The backing now owns the budget slot previously held by its keyed
	 * reservation. Consume the reservation at adoption, before either count
	 * can be observed together as two columns. */
	server_transfer_column_inserted(w, x, z);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	__atomic_fetch_add(&s_backing_created, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_backing_references, 1, __ATOMIC_RELAXED);
#endif
	return true;
}

void server_world_chunk_destroy(struct server_chunk* sc) {
	tps_mutated(3);
	assert(sc);
	if(server_column_observe_chunk) server_column_observe_chunk(NULL, 0, 0, sc, false);
	if(sc->backing) {
		thread_stripe_lock(sc->backing->mutex);
		sc->backing->resident = false;
		column_light_mutated(sc->backing, false);
		thread_stripe_unlock(sc->backing->mutex);
		column_backing_unref(sc->backing);
		server_chunk_init(sc);
		return;
	}

	// The 5 arrays are alloc'd via mem2_alloc by BOTH creation paths --
	// worldgen_alloc_chunk (P0 MEM1-relief fix) AND region_archive_get_blocks
	// (disk reload; copies out of the cNBT parse tree since the MEM1-pinning
	// borrow fix) -> they MUST be released through the range-aware mem2_free (a
	// plain free() on a MEM2-region pointer corrupts the heap). On PC mem2_free
	// == free, so PC is unchanged.
	// Size-aware frees verify the trailing red-zone canary on these high-churn
	// blocks (sizes are invariant for both creation paths).
	server_chunk_arrays_free(sc);
	server_chunk_init(sc);
}

struct server_world_populate_scratch*
server_world_populate_scratch_create(void) {
	struct server_world_populate_scratch* storage
		= mem2_tag(mem2_alloc(QUAD_POPULATE_SCRATCH_BYTES), QUAD_POPULATE_SCRATCH_BYTES, CARVE_POPULATE);
	if(storage) {
		storage->attached = false;
		storage->in_use = false;
	}
	return storage;
}

void server_world_populate_scratch_destroy(
	struct server_world_populate_scratch* storage) {
	if(!storage)
		return;
	assert(!storage->attached && !storage->in_use);
	if(storage->attached || storage->in_use) {
		fprintf(stderr,
				"[world] refusing to free attached populate rollback scratch\n");
		return;
	}
	mem2_free_sized(storage, QUAD_POPULATE_SCRATCH_BYTES);
}

void (*server_column_observe_lifetime)(struct server_world*, bool);

/* I07: the drain-armed yield (defined with the drain telemetry below). */
static void server_world_light_yield(void);
static void server_world_light_allocation(bool before) {
	if(before) thread_g9_lease_dependency();
	else thread_g9_lease_dependency_done();
}

static void server_world_import_simulation_stop(struct server_world* w);

#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*server_world_test_create_hook)(struct server_world* w);
void (*server_world_test_fluid_tail_hook)(struct server_world* w, bool reactivation);
#endif
static void server_world_create_impl(
	struct server_world* w, string_t level_name, enum world_dim dimension,
	int64_t seed, bool superflat,
	struct server_world_populate_scratch* borrowed_populate_scratch,
	struct server_local* session, uint8_t slot) {
	assert(w && dimension >= -1 && dimension <= 0);

	/* Never read prior standalone storage. Owner slots are initialized by
	 * server_local before any constructor or observer runs. */
	w->session = session;
	w->world_index = slot;
	const bool inherited_stop = session && server_local_light_stopped(session);
	/* Import existing legacy/publication state before exposing any new slot.
	 * An inherited byte must already belong to the terminal authority. */
	if(inherited_stop) server_world_import_simulation_stop(w);
	w->simulation_stopped = inherited_stop;
	/* Owned storage is initialized, so its origin cause survives recreation. */
	w->light_fault_accounted = session && w->light_fault_accounted;
	const bool inherited_fault = session && w->light_fault;
	const uint8_t inherited_reason = inherited_fault ? w->light_fault_reason : SERVER_LIGHT_FAULT_NONE;
	w->light_fault = inherited_fault;
	w->light_fault_reason = inherited_reason;
	// A2: pace the relight BFS so a lava-heavy quadrant relight (hundreds of ms of
	// flood) yields the single core back to render mid-flood instead of freezing it.
	// thread_yield is a no-op on PC, so this is Wii-only behaviour; the flood result is
	// byte-identical either way (order-independent fixed point).
	lighting_set_yield(server_world_light_yield);
	lighting_set_drain_allocation(server_world_light_allocation);

	dict_server_chunks_init(w->chunks);
	/* Publish AFTER the dict is constructed: from here the per-tick observer
	 * hooks may walk it (see server_world_chunks_ready). */
	w->chunks_ready = true;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(server_world_test_create_hook) server_world_test_create_hook(w);
#endif
	w->backing_world_instance = __atomic_add_fetch(
		&s_backing_world_instance, 1, __ATOMIC_RELAXED);
	if(!w->backing_world_instance) abort();
	w->checkpoint_stream_active = w->checkpoint_failed = false;
	memset(w->checkpoints, 0, sizeof w->checkpoints);
	w->checkpoint_head = w->checkpoint_count = 0;
	w->checkpoint_sequence = w->checkpoint_durable = 0;
	w->backing_generation = 0;
	w->generation = __atomic_add_fetch(&s_world_generation, 1, __ATOMIC_RELAXED);
	w->client_retired = false; /* WORLD-RETIRE-SRV: a new instance is shown */
	/* Local identities stay in the LOCAL half of the namespace
	 * (WORLD_GENERATION_REMOTE_BASE, client_interface.h): crossing it would
	 * alias a remote session's identity, so refuse exactly like a wrap. */
	if(!world_generation_is_local(w->generation)) abort();
	server_world_chunk_cache_invalidate(w); // Tier-0 resolution cache starts empty
	dict_far_sent_init(w->far_sent);
	/* A standalone world has no entity/tile-entity owner. server_local binds
	 * this immediately after create; initializing it here is required because
	 * callers may pass fresh stack storage rather than a zeroed struct. */
	w->local = NULL;
	/* Session identity and inherited stop were initialized before exposure. */
	/* A2-S: no handles yet; the pool binding is resolved at first use (the
	 * session re-binds `session` right after this call). The embedded pool
	 * is (re)initialised so non-zeroed caller storage never reads as live. */
	w->region_pool = NULL;
	region_pool_init(&w->own_region_pool);
	w->region_slot_count = 0;
	string_init_set(w->level_name, level_name);
	w->dimension = dimension;
	w->seed = seed;
	w->superflat = superflat;
	w->editing_blocks = false; // Beta World.editingBlocks default
	/* G9: an empty pending-light queue (server_world_light_free on destroy). */
	w->light_seeds = NULL;
	w->light_undo = NULL;
	w->light_pins = NULL;
	w->light_seed_n = w->light_seed_cap = 0;
	w->generation_light_pages = NULL;
	w->generation_light_backing = NULL;
	w->generation_light_n = w->generation_light_base = 0;
	w->generation_light_slots = 0;
	memset(w->generation_light_stat, 0, sizeof w->generation_light_stat);
	w->checkpoint_encode_cache = NULL;
	w->checkpoint_encode_active = false;
	w->light_undo_n = w->light_undo_cap = 0;
	w->light_pin_n = w->light_pin_cap = 0;
	w->light_resv_seeds = w->light_resv_cols = 0;
	lighting_drain_queue_init(&w->light_frontier);
	w->light_frontier_live = false;
	w->light_gen_hold = false;
	w->light_gen_hold_age = 0;
	w->light_gen_hold_x = w->light_gen_hold_z = 0;
	w->generation_light_io_fails = 0;
	w->generation_light_io_cause = w->generation_light_io_site = 0;
	w->gen_storage_failed = false;
	w->light_journal = NULL;
	w->light_journal_failed = false;
	{
		/* G9X-5b: an orphan checkpoint journal of this slot/dimension (a
		 * crash mid-checkpoint) is never replayed: remove it at level open */
		char jp[300];
		const int jn = snprintf(jp, sizeof jp, "%s/cw_light_journal_%u_%d.tmp",
			string_get_cstr(w->level_name), (unsigned)w->world_index, (int)w->dimension);
		if(jn > 0 && (size_t)jn < sizeof jp) (void)remove(jp);
	}
	w->gen_storage_cause = w->gen_storage_site = 0;
	w->gen_storage_x = w->gen_storage_z = 0;
	// Build the Beta terrain generator once per world. Both the overworld
	// (ChunkProviderGenerate) and the Nether (ChunkProviderHell) are handled by
	// worldgen_create + worldgen_generate_chunk; the assert above bounds
	// `dimension` to exactly those two. (Was overworld-only before the M6 Nether
	// port; a NULL generator now only happens on allocation failure.)
	w->generator = column_backing_init() ? worldgen_create(seed, dimension) : NULL;
	if(w->generator && superflat)
		worldgen_set_superflat(w->generator, true);
	// CFR ChunkProviderHell.java:61-71,202-209,300-347: constructor noise,
	// terrain surface and population share one Random. Disk loads preserve it.
	jrand_set_seed(&w->hell_populate_rng, seed);
	if(w->generator && dimension == WORLD_DIM_NETHER)
		worldgen_hell_initial_rng(w->generator, &w->hell_populate_rng);
	/* F4-RNG: World.rand = new Random() (World.java:37) -- an independent
	 * runtime seed for THIS instance (a reload/regeneration is a new World
	 * and therefore a new stream), then the initializer's
	 * `ambientTickCountdown = this.rand.nextInt(12000)` (World.java:54).
	 * Observation boundary: the stream is matched from here on. The
	 * spawn-point search (World.java:104-105) runs only for a brand-new
	 * level and consumes from an already-random state, so it is not
	 * replayed; nothing restores or saves this state (WorldInfo.saveNBTTag
	 * persists the terrain seed only). */
	server_world_runtime_jrand_seed(&w->rand);
	(void)jrand_next_int_bound(&w->rand, 12000);
	w->region_evict_failed = false;
	w->region_close_failed = false;
	w->save_batch_open = false; /* explicit: callers may pass non-zeroed storage (see :437) */
	for(size_t k = 0; k < SERVER_WORLD_RETIRING_MAX; k++) {
		w->retiring[k].active = false;
		w->retiring[k].record = BUFFER_INIT;
	}
	w->retiring_active = 0;
	w->retiring_bytes = 0;
	w->retiring_retries = 0;
	w->retiring_refused = 0;
	w->retiring_dropped = 0;
	w->current_tick = 0;
	w->explosions_this_tick = 0;
	w->explosion_deferrals = 0;
	w->day_phase_initialized = false;
	w->is_day_phase = false;
	/* Bind the server's pre-stream reservation, or allocate one for a standalone
	 * world. A missing base fails populate closed; peak-time journal growth was
	 * precisely the half-loaded portal wedge this fixed snapshot removes. */
	w->populate_scratch_storage = borrowed_populate_scratch
		? borrowed_populate_scratch
		: server_world_populate_scratch_create();
	w->populate_scratch_owned = !borrowed_populate_scratch;
	if(w->populate_scratch_storage) {
		assert(!w->populate_scratch_storage->attached
			   && !w->populate_scratch_storage->in_use);
		w->populate_scratch_storage->attached = true;
	} else
		fprintf(stderr,
				"[world] populate rollback scratch unavailable (%zu bytes)\n",
				(size_t)QUAD_POPULATE_SCRATCH_BYTES);
	memset(w->gen_pending, 0, sizeof(w->gen_pending));
	memset(w->gen_pending_x, 0, sizeof(w->gen_pending_x));
	memset(w->gen_pending_z, 0, sizeof(w->gen_pending_z));
	w->gen_throttle_armed = false; // armed only inside ensure_populated
	w->gen_attempts_window = 0;
	w->gen_this_window = 0;
	w->gen_deferred_window = 0;
	server_world_scheduled_ticks_create(
		w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	if(server_column_observe_lifetime) server_column_observe_lifetime(w, true);
}

static void server_world_destroy_impl(struct server_world* w, bool persist);
static void server_world_light_free(struct server_world* w); /* G9 */
static void server_world_light_journal_free(struct server_world* w); /* G9X-5b */
/* S2 retiring ring (defined beside server_world_save_chunk_obj_impl). */
static void server_world_retiring_release(struct server_world* w,
										  struct server_world_retiring* r);
enum server_world_retiring_load {
	SERVER_WORLD_RETIRING_LOAD_NONE = 0, /* no retained record for this id */
	SERVER_WORLD_RETIRING_LOAD_CONSUMED, /* record now readable; mark dirty */
	SERVER_WORLD_RETIRING_LOAD_BLOCKED,  /* cannot make it readable: refuse */
};
static enum server_world_retiring_load server_world_retiring_consume_for_load(
	struct server_world* w, struct region_archive* ra, w_coord_t x,
	w_coord_t z, struct server_world_retiring** out_entry);
static void server_world_reconcile_retiring(struct server_world* w);

void server_world_create(struct server_world* w, string_t level_name,
						 enum world_dim dimension, int64_t seed, bool superflat) {
	server_world_create_impl(
		w, level_name, dimension, seed, superflat, NULL, NULL, 0);
}

void server_world_create_owned(struct server_world* w, string_t level_name,
	enum world_dim dimension, int64_t seed, bool superflat,
	struct server_local* session, uint8_t slot) {
	assert(session && slot < SERVER_LOCAL_WORLD_SLOTS);
	server_world_create_impl(w, level_name, dimension, seed, superflat,
		NULL, session, slot);
}

/* F4-RNG: java.util.Random() seeds itself from seedUniquifier() ^
 * System.nanoTime(); the uniquifier is the JDK's multiplicative sequence.
 * The clock half is RAND_GEN_CLOCK_SEED (pinned under tests, like every other
 * clock-seeded stream), so two streams created in one tick still differ. */
static uint64_t s_jrand_seed_uniquifier = UINT64_C(8682522807148012);
#ifdef CAVEWII_SERVER_LOCAL_TEST
static struct java_random* s_test_runtime_seed_target;
static int64_t s_test_runtime_seed;
void server_world_test_seed_runtime_jrand(struct java_random* target, int64_t seed) {
	s_test_runtime_seed_target = target;
	s_test_runtime_seed = seed;
}
#endif
void server_world_runtime_jrand_seed(struct java_random* r) {
	assert(r);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(r == s_test_runtime_seed_target) {
		s_test_runtime_seed_target = NULL;
		jrand_set_seed(r, s_test_runtime_seed);
		return;
	}
#endif
	uint64_t next;
#ifdef PLATFORM_WII
	/* PPC32 has no 64-bit atomics (__atomic_*_8 undefined at link); the
	 * single-core Broadway makes an IRQ-off read-modify-write atomic
	 * against every other thread (same pattern as mem2.c). */
	u32 level = IRQ_Disable();
	next = s_jrand_seed_uniquifier * UINT64_C(181783497276652981);
	s_jrand_seed_uniquifier = next;
	IRQ_Restore(level);
#else
	uint64_t prev = __atomic_load_n(&s_jrand_seed_uniquifier, __ATOMIC_RELAXED);
	do {
		next = prev * UINT64_C(181783497276652981);
	} while(!__atomic_compare_exchange_n(&s_jrand_seed_uniquifier, &prev, next,
										 false, __ATOMIC_RELAXED,
										 __ATOMIC_RELAXED));
#endif
	uint64_t clock = (uint64_t)RAND_GEN_CLOCK_SEED();
	clock = (clock << 32) ^ (clock * UINT64_C(0x9E3779B97F4A7C15));
	jrand_set_seed(r, (int64_t)(next ^ clock));
}

bool server_world_create_with_populate_scratch(
	struct server_world* w, string_t level_name, enum world_dim dimension,
	int64_t seed, bool superflat,
	struct server_world_populate_scratch* populate_scratch) {
	if(!populate_scratch || populate_scratch->attached
	   || populate_scratch->in_use)
		return false;
	server_world_create_impl(
		w, level_name, dimension, seed, superflat, populate_scratch, NULL, 0);
	if(!w->generator) {
		fprintf(stderr,
				"[world] terrain generator allocation failed; load aborted\n");
		server_world_destroy_impl(w, false);
		return false;
	}
	return true;
}

bool server_world_create_owned_with_populate_scratch(
	struct server_world* w, string_t level_name, enum world_dim dimension,
	int64_t seed, bool superflat, struct server_world_populate_scratch* scratch,
	struct server_local* session, uint8_t slot) {
	assert(session && slot < SERVER_LOCAL_WORLD_SLOTS);
	if(!scratch || scratch->attached || scratch->in_use) return false;
	server_world_create_impl(w, level_name, dimension, seed, superflat,
		scratch, session, slot);
	if(!w->generator) {
		server_world_destroy_impl(w, false);
		return false;
	}
	return true;
}

static void server_world_destroy_impl(struct server_world* w, bool persist) {
	assert(w);
	// next-gaps-0924/G5c: a suspended redstone cascade belongs to THIS world; never replay it into
	// the next one (dimension travel recreates s->world).
	redstone_server_reset();
	assert(!w->populate_scratch_storage
		   || (w->populate_scratch_storage->attached
			   && !w->populate_scratch_storage->in_use));

	/* Final drain precedes freeing any world/handle, including discard. */
	if(!server_world_checkpoint_drain(w)) w->region_evict_failed = true;
	if(server_column_observe_lifetime) server_column_observe_lifetime(w, false);

	/* G9: a persisting teardown (shutdown, world switch) is this world last
	 * prescribed drain boundary: nothing of it runs afterwards, so settling
	 * its pending light here is unobservable to simulation, and every save
	 * below writes settled columns (no lost work). A discard drops it with
	 * the chunks. */
	if(persist)
		(void)server_world_light_drain(w); /* a stopped world keeps its pins */

	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);

	while(!dict_server_chunks_end_p(it)) {
		struct server_chunk* sc = &dict_server_chunks_ref(it)->value;
		int64_t id = dict_server_chunks_ref(it)->key;
		/* G9: a column still pinned (stopped drain) is never written: its
		 * disk image stays the last coherent one. */
		if(persist)
			server_world_save_chunk_obj(w, false, S_CHUNK_X(id), S_CHUNK_Z(id), sc);
		server_world_chunk_destroy(sc);

		dict_server_chunks_next(it);
	}
	server_world_light_free(w); /* after the saves, which read the pins */

	server_world_chunk_cache_invalidate(w); // every chunk is gone
	dict_server_chunks_clear(w->chunks);
	dict_far_sent_clear(w->far_sent);
	/* m*lib's clear frees the index array and leaves index == NULL. Retract the
	 * permission BEFORE anything else can observe the world again: from here to
	 * the next create, reading `chunks` is a NULL dereference. */
	w->chunks_ready = false;

	// Tear down the cached region archives (~9KB MEM1 + a FILE handle each, up
	// to MAX_REGIONS). This ran AFTER the chunk save loop above on purpose --
	// the saves still need (and may repopulate) the cache. Without this, every
	// world unload orphaned the whole cache: server_world_create re-inits the
	// LRU head and zeroes loaded_regions_length, so each menu->world cycle
	// leaked another batch (the already-fixed save-path tmp leak's sibling).
	// Popping through the LRU list destroys exactly the live entries.
	/* S2: drain the retiring ring while the region handles are still open.
	 * region_archive_sync waits for the helper and reports a failed
	 * publication; the reconcile then re-appends any FAILED retained record
	 * into the still-open handle, and the sync+close loop below publishes
	 * it. Only what is STILL unpublished after that is counted dropped. */
	if(persist && w->retiring_active) {
		for(size_t k = 0; k < server_world_region_count(w); k++)
			(void)region_archive_sync(server_world_region_at(w, k));
		/* Ring only: the chunk dict was cleared above. */
		server_world_reconcile_retiring(w);
	}
	/* A2-S: sync-then-close every handle this world owns in the shared pool
	 * (region_pool_release_world reports into the two flags and names the
	 * region on stderr exactly as the old per-world loop did). */
	if(w->region_pool)
		(void)region_pool_release_world(w->region_pool, w);
	assert(w->region_slot_count == 0);

	/* S2: every handle above synced+closed, which publishes (and REPORTS a
	 * failure into the two flags). A retained record is therefore durable
	 * unless a flag is set; count anything else as DROPPED -- loudly. */
	for(size_t k = 0; k < SERVER_WORLD_RETIRING_MAX; k++) {
		struct server_world_retiring* r = w->retiring + k;
		if(!r->active)
			continue;
		if(persist && (w->region_evict_failed || w->region_close_failed)) {
			w->retiring_dropped++;
			fprintf(stderr,
					"[world] evicted chunk (%d,%d): retained record never "
					"published before teardown -- newest state LOST\n",
					(int)S_CHUNK_X(r->id), (int)S_CHUNK_Z(r->id));
		}
		server_world_retiring_release(w, r);
	}

	string_clear(w->level_name);
	region_archive_encode_cache_free(&w->checkpoint_encode_cache);
	server_world_scheduled_ticks_destroy(w);
	worldgen_destroy(w->generator);
	w->generator = NULL;
	if(w->populate_scratch_storage)
		w->populate_scratch_storage->attached = false;
	if(w->populate_scratch_owned)
		server_world_populate_scratch_destroy(w->populate_scratch_storage);
	w->populate_scratch_storage = NULL;
	w->populate_scratch_owned = false;
	w->generation = 0; /* A1a: every handle to this instance is now stale */
}

void server_world_destroy(struct server_world* w) {
	server_world_destroy_impl(w, true);
}

void server_world_discard(struct server_world* w) {
	server_world_destroy_impl(w, false);
}

bool server_world_chunks_ready(const struct server_world* w) {
	return w && w->chunks_ready;
}

struct server_world_handle server_world_handle_of(const struct server_world* w) {
	if(!w || !w->chunks_ready)
		return (struct server_world_handle) {0};
	return (struct server_world_handle) {
		.world_index = w->world_index, .generation = w->generation};
}

static bool server_chunk_get_block(void* user, c_coord_t x, w_coord_t y,
								   c_coord_t z, struct block_data* blk) {
	assert(user && blk);
	struct server_chunk* sc = user;

	if(y < 0 || y >= WORLD_HEIGHT)
		return false;

	size_t idx = S_CHUNK_IDX(x, y, z);

	*blk = (struct block_data) {
		.type = sc->ids[idx],
		.metadata = nibble_read(sc->metadata, idx),
		.sky_light = nibble_read(sc->lighting_sky, idx),
		.torch_light = nibble_read(sc->lighting_torch, idx),
	};

	return true;
}

static void server_world_fill_neighbours(struct server_world* w,
										 struct block_data* neighbour_data,
										 w_coord_t x, w_coord_t y,
										 w_coord_t z) {
	assert(w && neighbour_data);

	for(int side = 0; side < SIDE_MAX; side++) {
		int ox, oy, oz;
		blocks_side_offset((enum side)side, &ox, &oy, &oz);

		if(!server_world_get_block(w, x + ox, y + oy, z + oz,
								   &neighbour_data[side])) {
			neighbour_data[side] = (struct block_data) {
				.type = BLOCK_AIR,
				.metadata = 0,
				.sky_light = 0,
				.torch_light = 0,
			};
		}
	}
}

static bool server_world_schedule_chunk_ticks(struct server_world* w,
											  w_coord_t chunk_x,
											  w_coord_t chunk_z,
											  struct server_chunk* sc) {
	assert(w && sc);
	const uint32_t cell_count
		= (uint32_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	assert(sc->load_tick_rearm_cursor < cell_count);
	if(sc->load_tick_rearm_cursor >= cell_count)
		sc->load_tick_rearm_cursor = 0;

	for(; sc->load_tick_rearm_cursor < cell_count;
		sc->load_tick_rearm_cursor++) {
		const uint32_t cursor = sc->load_tick_rearm_cursor;
		const int y = (int)(cursor % WORLD_HEIGHT);
		const uint32_t column = cursor / WORLD_HEIGHT;
		const int cx = (int)(column / CHUNK_SIZE);
		const int cz = (int)(column % CHUNK_SIZE);
		struct block_data blk;
		if(!server_chunk_get_block(sc, cx, y, cz, &blk))
			continue;
		// CaveWii does not persist Beta's pending-tick set, so this load scan
		// re-arms scheduled-only idempotent tickers. A block with a neighbour
		// hook owns its lifecycle and is skipped: repeaters self-arm their
		// metadata delay; still fluids quiet-wake there; moving fluids keep
		// BlockFluid's harden hook and are random-tick-enabled exactly like
		// Beta's setTickOnLoad(true), so an existing disk cell must not gain a
		// synthetic schedule merely from loading.
		const struct block* cb = blocks[blk.type];
		// EXEMPT from the skip: the pressure plates and the button. Their blanket
		// load-tick IS idempotent -- the plate poll corrects/holds state, and a
		// button saved mid-press releases (the tick queue is not persisted, so this
		// is the only way a loaded pressed button ever lets go). Without the
		// exemption a reloaded plate never polls (inert until a neighbour edit).
		if(!cb || !cb->onWorldTick)
			continue;
		if(cb->onNeighbourBlockChange
		   && blk.type != BLOCK_STONE_PRESSURE_PLATE
		   && blk.type != BLOCK_WOOD_PRESSURE_PLATE
		   && blk.type != BLOCK_STONE_BUTTON)
			continue;
		if(!server_world_schedule_block_tick_rearm(
			   w, chunk_x * CHUNK_SIZE + cx, y,
			   chunk_z * CHUNK_SIZE + cz, blk.type))
			return false; /* resume at this unadmitted cell */
	}
	sc->load_tick_rearm_cursor = 0;
	return true;
}

/* Scanning an entire chunk requires the union of every cell's Beta +/-8
 * checkChunksExist box: the resident 3x3 centred on that chunk. A lone chunk
 * can never pass that test, so an eager one-shot scan at insertion silently
 * lost every load-rearm callback. Defer until the union is complete. */
static bool server_world_chunk_tick_neighbourhood_loaded(
	struct server_world* w, w_coord_t chunk_x, w_coord_t chunk_z) {
	assert(w);
	for(int dz = -1; dz <= 1; dz++)
		for(int dx = -1; dx <= 1; dx++)
			if(!dict_server_chunks_get(
				   w->chunks, S_CHUNK_ID(chunk_x + dx, chunk_z + dz)))
				return false;
	return true;
}

static void server_world_try_rearm_chunk_ticks_impl(struct server_world* w,
											w_coord_t chunk_x,
											w_coord_t chunk_z) {
	struct server_chunk* sc
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(chunk_x, chunk_z));
	if(!sc || sc->load_ticks_rearmed
	   || !server_world_chunk_tick_neighbourhood_loaded(w, chunk_x, chunk_z))
		return;
	if(!w->scheduled_ticks)
		return;

	/* Mark complete only after every eligible callback was admitted. A full
	 * queue leaves this epoch pending so a later neighbourhood refresh can retry;
	 * the reconstruction insert always keep-first deduplicates admitted entries. */
	if(server_world_schedule_chunk_ticks(w, chunk_x, chunk_z, sc))
		sc->load_ticks_rearmed = true;
}
static void server_world_try_rearm_chunk_ticks(struct server_world* w,
	w_coord_t x, w_coord_t z) {
	tps_note(world_tps_observe, TPS_ENTER, TPS_REARM);
	ptime_t start = deep_start();
	server_world_try_rearm_chunk_ticks_impl(w, x, z);
	deep_end(7, start);
	tps_note(world_tps_observe, TPS_LEAVE, TPS_REARM);
}


static bool server_world_find_pending_chunk_tick_rearm(
	struct server_world* w, w_coord_t* out_x, w_coord_t* out_z) {
	assert(w && out_x && out_z);
	bool found = false;
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);
	while(!dict_server_chunks_end_p(it)) {
		const int64_t id = dict_server_chunks_ref(it)->key;
		struct server_chunk* sc = &dict_server_chunks_ref(it)->value;
		const w_coord_t x = S_CHUNK_X(id);
		const w_coord_t z = S_CHUNK_Z(id);
		if(!sc->load_ticks_rearmed
		   && server_world_chunk_tick_neighbourhood_loaded(w, x, z)
		   && (!found || z < *out_z || (z == *out_z && x < *out_x))) {
			*out_x = x;
			*out_z = z;
			found = true;
		}
		dict_server_chunks_next(it);
	}
	return found;
}

static void server_world_retry_pending_chunk_tick_rearms(
	struct server_world* w) {
	assert(w);
	/* A transiently full ring must not turn a failed scan into permanent loss.
	 * Retry after each scheduled drain has had a chance to free entries. Select
	 * the lowest (z,x) ready centre explicitly instead of relying on hash-table
	 * iteration order, preserving deterministic queue insertion. */
	while(w->scheduled_ticks) {
		w_coord_t x = 0, z = 0;
		if(!server_world_find_pending_chunk_tick_rearm(w, &x, &z))
			return;
		server_world_try_rearm_chunk_ticks(w, x, z);
		struct server_chunk* sc
			= dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
		if(!sc || !sc->load_ticks_rearmed)
			return; /* queue filled part-way; resume after the next drain */
	}
}

static void server_world_refresh_chunk_tick_rearms(struct server_world* w,
											 w_coord_t loaded_x,
											 w_coord_t loaded_z) {
	assert(w);
	/* Only centres within one chunk of the newly resident member can have
	 * changed from incomplete to complete. Keep the z-major order stable so
	 * simultaneous completions enter the scheduled queue deterministically. */
	for(int dz = -1; dz <= 1; dz++)
		for(int dx = -1; dx <= 1; dx++)
			server_world_try_rearm_chunk_ticks(
				w, loaded_x + dx, loaded_z + dz);
}

static void server_world_invalidate_chunk_tick_rearms(
	struct server_world* w, w_coord_t unloaded_x, w_coord_t unloaded_z) {
	assert(w);
	/* Pending entries whose +/-8 box crosses this chunk will be consumed by
	 * TickUpdates while it is absent. Start a new readiness epoch for every
	 * affected resident centre so reload can restore those one-shot callbacks. */
	for(int dz = -1; dz <= 1; dz++) {
		for(int dx = -1; dx <= 1; dx++) {
			struct server_chunk* sc = dict_server_chunks_get(
				w->chunks, S_CHUNK_ID(unloaded_x + dx, unloaded_z + dz));
			if(sc) {
				sc->load_tick_rearm_cursor = 0;
				sc->load_ticks_rearmed = false;
			}
		}
	}
}

// Tier-0 chunk-resolution cache (see the server_world.h field comment for the
// threading + invalidation contract). Hit/miss tallies feed the F3 getblk row
// so the win is MEASURED, not assumed (doctrine).
static int s_chunkres_hits = 0;
static int s_chunkres_lookups = 0;
static int s_chunkres_last_hits = 0;
static int s_chunkres_last_lookups = 0;
/* Server-owned per-kernel census, copied into the locked drain record.
 * Probe OFF leaves these unsigned diagnostic counters at zero. */
static uint64_t s_kernel_resolve_hits, s_kernel_resolve_misses;
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* G9X-7 work census (test builds only; server_world_test_beta_light_work):
 * every residency dict probe and every resolve-cache chunk lookup. */
static uint64_t s_sbl_dict_probes, s_sbl_resolves;
#endif

static struct server_chunk* server_world_resolve_chunk_counted(
	struct server_world* w, w_coord_t cx, w_coord_t cz, bool census) {
	int64_t id = S_CHUNK_ID(cx, cz);
	s_chunkres_lookups++;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	s_sbl_resolves++;
#endif
	for(unsigned k = 0; k < SERVER_WORLD_CHUNK_CACHE_WAYS; k++)
		if(w->chunk_cache[k] && w->chunk_cache_id[k] == id) {
			s_chunkres_hits++;
			if(census)
				s_kernel_resolve_hits++;
			return w->chunk_cache[k];
		}
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, id);
	if(census)
		s_kernel_resolve_misses++;
	if(sc) {
		const unsigned k = w->chunk_cache_next;
		w->chunk_cache[k] = sc;
		w->chunk_cache_id[k] = id;
		w->chunk_cache_next
			= (uint8_t)((k + 1) % SERVER_WORLD_CHUNK_CACHE_WAYS);
	}
	return sc;
}

static struct server_chunk* server_world_resolve_chunk(struct server_world* w,
	w_coord_t cx, w_coord_t cz) {
	return server_world_resolve_chunk_counted(w, cx, cz, false);
}

/* next-gaps-0924/W1C-b: the same residency answer as
 * server_world_is_chunk_loaded, through the Tier-0 resolve cache get_block
 * already uses (a hit costs no dict lookup). */
bool server_world_chunk_resident(struct server_world* w, w_coord_t cx,
								 w_coord_t cz) {
	assert(w);
	return server_world_resolve_chunk(w, cx, cz) != NULL;
}

void server_world_chunk_cache_invalidate(struct server_world* w) {
	assert(w);
	for(unsigned k = 0; k < SERVER_WORLD_CHUNK_CACHE_WAYS; k++) {
		w->chunk_cache[k] = NULL;
		w->chunk_cache_id[k] = 0;
	}
	w->chunk_cache_next = 0;
}

static bool server_world_light_get_block(void* user, w_coord_t x, w_coord_t y,
										 w_coord_t z, struct block_data* blk,
										 uint8_t* height) {
	assert(user);
	struct server_world* w = user;

	if(y < 0 || y >= WORLD_HEIGHT)
		return false;

	struct server_chunk* sc = server_world_resolve_chunk(
		w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));

	if(!sc)
		return false;

	if(blk)
		server_chunk_get_block(sc, W2C_COORD(x), y, W2C_COORD(z), blk);

	if(height)
		*height = sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE];

	return true;
}

/* I07 lane 3 (T1): the drain's light-only neighbour read -- the same chunk
 * resolve and the same two nibble planes server_world_light_get_block
 * reads, without the id byte and metadata nibble the flood never uses for
 * a neighbour. Existence (y range + resident chunk) is the same test. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
static bool server_world_light_get_light(void* user, w_coord_t x, w_coord_t y,
										 w_coord_t z, uint8_t* sky,
										 uint8_t* torch) {
	struct server_world* w = user;
	if(y < 0 || y >= WORLD_HEIGHT)
		return false;
	struct server_chunk* sc = server_world_resolve_chunk(
		w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
	if(!sc)
		return false;
	const size_t idx = S_CHUNK_IDX(W2C_COORD(x), y, W2C_COORD(z));
	*sky = nibble_read(sc->lighting_sky, idx);
	*torch = nibble_read(sc->lighting_torch, idx);
	return true;
}

#endif

/* Some isolated worlds own unpublished arrays (no transport/backing). Live
 * dictionary columns always use the synchronized published writer. */
#define LIGHT_TRIAL_CELLS ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define LIGHT_TRIAL_PLANE (LIGHT_TRIAL_CELLS / 2)
#define LIGHT_TRIAL_PAGE_CELLS 64u
struct server_light_trial_capture {
	uint8_t* light; /* two private packed planes, valid only in captured pages */
	uint32_t pages[LIGHT_TRIAL_CELLS / LIGHT_TRIAL_PAGE_CELLS / 32];
};

/* Only the server writes the light aliases. Capture BOTH pre-trial planes
 * before the FIRST changing store in this page (private undo included).
 * Untouched pages never need a snapshot or a rollback scan. */
static inline void server_chunk_trial_capture(struct server_chunk* sc,
	unsigned index) {
	struct server_light_trial_capture* c = sc->light_trial_capture;
	if(!c) return;
	const unsigned page = index / LIGHT_TRIAL_PAGE_CELLS;
	const uint32_t bit = UINT32_C(1) << (page % 32);
	if(c->pages[page / 32] & bit) return;
	const size_t offset = page * (LIGHT_TRIAL_PAGE_CELLS / 2);
	memcpy(c->light + offset, sc->lighting_sky + offset,
		LIGHT_TRIAL_PAGE_CELLS / 2);
	memcpy(c->light + LIGHT_TRIAL_PLANE + offset,
		sc->lighting_torch + offset, LIGHT_TRIAL_PAGE_CELLS / 2);
	c->pages[page / 32] |= bit;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* G9X-1: semantic store site. The macros below record the caller's
 * __FILE__/__LINE__ before calling the unchanged function (the parenthesised
 * definitions are not macro-expanded); production has no macro. */
static const char* s_history_site_file;
static int s_history_site_line;
#define server_chunk_store(sc, plane, index, value) \
	(s_history_site_file = __FILE__, s_history_site_line = __LINE__, \
	 (server_chunk_store)(sc, plane, index, value))
#define server_chunk_store_light_known(sc, index, sky, torch, ws, wt, held) \
	(s_history_site_file = __FILE__, s_history_site_line = __LINE__, \
	 (server_chunk_store_light_known)(sc, index, sky, torch, ws, wt, held))
static void server_world_history_store(struct server_chunk* sc, unsigned plane,
	unsigned index, uint8_t value) {
	if(server_world_test_history_hook) {
		struct server_world* w = s_history_depth
			? s_history_worlds[s_history_depth - 1] : NULL;
		struct server_world_history_event e = {
			.world = w, .session = w ? w->session : NULL, .chunk = sc,
			.operation = "store", .edge = "attempt", .outcome = "",
			.request = s_history_depth ? s_history_stack[s_history_depth - 1] : 0,
			.parent = s_history_depth ? s_history_stack[s_history_depth - 1] : 0,
			.generation = w ? w->generation : 0,
			.hell_rng = w ? w->hell_populate_rng.seed : 0,
			.x = (w_coord_t)index, .y = (w_coord_t)plane,
			.before_id = plane == COLUMN_IDS ? sc->ids[index]
				: nibble_read(plane == COLUMN_META ? sc->metadata
					: plane == COLUMN_SKY ? sc->lighting_sky : sc->lighting_torch, index),
			.after_id = value,
			.slot = w ? w->world_index : 0,
			.dimension = w ? (int)w->dimension : 0,
			.file = s_history_site_file, .line = s_history_site_line,
		};
		if(e.session) {
			e.update = e.session->near_send_seq;
			e.session_rng = e.session->rand_src.seed;
		}
		if(!server_world_test_history_hook(&e)) s_history_failed = true;
	}
}
#define G9B_STORE(sc, plane, index, value) \
	server_world_history_store(sc, plane, index, value)
#else
#define G9B_STORE(sc, plane, index, value) ((void)0)
#endif

/* G9X-6: publication debt follows what Beta publishes. Every stored LIGHT
 * change is published [CFR World.java:512-519 setLightValue ->
 * IWorldAccess.markBlockNeedsUpdate]. A block (id/metadata) change is
 * published only through Beta's notify setters [World.java:332-346
 * setBlockWithNotify / setBlockAndMetadataWithNotify -> notifyBlockChange ->
 * markBlockNeedsUpdate; World.java:306-315 setBlockMetadataWithNotify]; CaveWii
 * publishes exactly those already, at the write (server_world_notify_block ->
 * CRPC_SET_BLOCK). The no-notify setters [World.java:259-271
 * setBlockAndMetadata, :273-285 setBlock, :317-330 setBlockMetadata; e.g.
 * BlockFlowing.java:29-31 flowing -> still] publish nothing. So only light
 * stores create debt here; height is derived by the client from its blocks.
 * s_pub_trial: true while a checkpoint trial drain runs or its exact restore
 * rewrites the pre-trial bytes: those speculative stores neither create nor
 * clear debt. */
static bool s_pub_trial;
static inline void server_chunk_pub_touch(struct server_chunk* sc) {
	if(!s_pub_trial)
		sc->pub_debt = true;
}

static void (server_chunk_store)(struct server_chunk* sc, unsigned plane,
	unsigned index, uint8_t value) {
	G9B_STORE(sc, plane, index, value);
	/* Only the server writes these aliases. An identical light nibble cannot
	 * diverge a presentation page or emit a mutation; keep changing writes
	 * on the original separately locked SKY-before-TORCH path. */
	if(plane == COLUMN_SKY || plane == COLUMN_TORCH) {
		/* proto-light-1008 P2: nibble_read's expression, inlined */
		const uint8_t* lp = plane == COLUMN_SKY ? sc->lighting_sky
												: sc->lighting_torch;
		if(((lp[index / 2] >> ((index % 2) * 4)) & 0xF) == value)
			return;
		server_chunk_trial_capture(sc, index);
		server_chunk_pub_touch(sc);
	}
	tps_mutation_callback census = __atomic_load_n(&world_tps_mutation, __ATOMIC_ACQUIRE);
	if(census) {
		uint8_t old = plane == COLUMN_IDS ? sc->ids[index]
			: nibble_read(plane == COLUMN_META ? sc->metadata
				: plane == COLUMN_SKY ? sc->lighting_sky : sc->lighting_torch, index);
		if(old != value) census(plane == COLUMN_IDS ? 1 : 2);
	}
	if(sc->backing) column_backing_set_cell(sc->backing, plane, index, value);
	else if(plane == COLUMN_IDS) sc->ids[index] = value;
	else {
		uint8_t* data = plane == COLUMN_META ? sc->metadata
			: plane == COLUMN_SKY ? sc->lighting_sky : sc->lighting_torch;
		nibble_write(data, index, value);
	}
}

/* g9-compute-1009 (T1): the drain writer's SKY + TORCH pair, equal to
 * server_chunk_store(SKY) then server_chunk_store(TORCH): the same equality
 * skips, the same census calls in the same order (a counter on this thread),
 * the same per-plane backing writes in the same order -- under one stripe
 * hold instead of two (column_backing_set_light). */
/* g9-trial-1009 (T1): the same body with the two equality tests supplied
 * by a caller that has just read this cell's planes (the drain kernel reads
 * both nibbles at its pop and nothing writes the cell before its store). */
static inline void (server_chunk_store_light_known)(struct server_chunk* sc,
	unsigned index, uint8_t sky, uint8_t torch, bool ws, bool wt,
	struct thread_mutex** held) {
	G9B_STORE(sc, COLUMN_SKY, index, sky);
	G9B_STORE(sc, COLUMN_TORCH, index, torch);
	if(!ws && !wt)
		return;
	server_chunk_trial_capture(sc, index);
	server_chunk_pub_touch(sc);
	tps_mutation_callback census = __atomic_load_n(&world_tps_mutation, __ATOMIC_ACQUIRE);
	if(census) {
		if(ws) census(2);
		if(wt) census(2);
	}
	const unsigned planes = (ws ? 1u : 0u) | (wt ? 2u : 0u);
	if(sc->backing && held) {
		/* g9-compute-1009 (T1): the drain keeps the last written column's
		 * stripe across consecutive writes (server_light_flood releases it
		 * at every yield, queue growth and exit). Same writes, same order. */
		struct thread_mutex* m = sc->backing->mutex;
		if(*held != m) {
			if(*held)
				thread_stripe_unlock(*held);
			thread_stripe_lock(m);
			*held = m;
		}
		column_backing_set_light_locked(sc->backing, index, planes, sky, torch);
	} else if(sc->backing)
		column_backing_set_light(sc->backing, index, planes, sky, torch);
	else {
		if(ws) nibble_write(sc->lighting_sky, index, sky);
		if(wt) nibble_write(sc->lighting_torch, index, torch);
	}
}
static inline void server_chunk_store_light(struct server_chunk* sc,
	unsigned index, uint8_t sky, uint8_t torch, struct thread_mutex** held) {
	const unsigned sh = (index % 2) * 4;
	server_chunk_store_light_known(sc, index, sky, torch,
		((sc->lighting_sky[index / 2] >> sh) & 0xF) != sky,
		((sc->lighting_torch[index / 2] >> sh) & 0xF) != torch, held);
}

static void server_world_light_set_light(void* user, w_coord_t x, w_coord_t y,
										 w_coord_t z, uint8_t light) {
	assert(user);
	struct server_world* w = user;
	struct server_chunk* sc = server_world_resolve_chunk(
		w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
	assert(sc);

	size_t idx = S_CHUNK_IDX(x, y, z);
	server_chunk_store(sc, COLUMN_SKY, idx, light & 0xF);
	server_chunk_store(sc, COLUMN_TORCH, idx, light >> 4);
	server_world_chunk_touch(sc);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(server_world_test_light_write_hook)
		server_world_test_light_write_hook(x, y, z, light);
#endif
}

#include "server_light_kernel.inc"
#ifdef CAVEWII_SERVER_LOCAL_TEST
lighting_drain_kernel_fn server_world_test_light_flood_override;
#endif

/* ---- G9: Beta deferred light (ASTRA-1005B B2-B4, ASTRA-G9-RULING 1-9) ----
 *
 * Beta Chunk.setBlockIDWithMetadata [CFR server Chunk.java:208-239] writes the
 * id, runs onBlockRemoval, then -- when the column height moves -- calls
 * func_339_g [Chunk.java:141-200], which rewrites the height and THAT
 * column's skylight at once, and only SCHEDULES every Sky/Block propagation
 * (World.scheduleLightingUpdate -> func_484_a queues a MetadataChunkBlock,
 * World.java:1293-1330). MinecraftServer.doTick:346-348 runs World.tick()
 * (spawning, TickUpdates, random ticks), then `while (func_6156_d())`, which
 * empties the queue (World.java:1269-1291), then updateEntities.
 *
 * Here: every gameplay write site (set_block, piston moving, explosion, the
 * explosion fire) does the exact func_339_g column write immediately and
 * appends its cell to w->light_seeds; server_world_light_drain settles them.
 * The settled field is the engine's existing fixed-point flood (values
 * byte-identical to the old synchronous path, ruling B5). The drain first
 * restores each touched column's pre-batch skylight (light_undo -- private
 * to the drain, no consumer or publication runs between restore and flood),
 * so the flood starts from the exact state the old path started from.
 *
 * Order inside the drain: Beta pops its queue LIFO with recent-region
 * merging. Nothing observes the light field while the queue drains (the
 * while loop runs it empty before updateEntities), and the engine's flood is
 * an order-independent fixed point (tests/test_lighting_multi.c), so one
 * batched flood == LIFO single-seed floods; test_light_queue_equiv's lifo
 * mode proves it on this queue. Beta's own MetadataChunkBlock relaxation is
 * NOT reproduced (settled-value parity with it is out of scope and not
 * claimed). Generation passes (populate relight, skylight bleed) stay
 * synchronous in provision order (ASTRA-G9-RULING 1-4) and never run on a
 * chunk the pending work pins (server_world_ensure_populated defers). */
static bool s_light_sync_test = false;
static bool s_light_lifo_test = false;
static long s_light_fail_after = -1;
static struct server_world_light_stats s_light_stats;
static struct server_world_light_hb s_light_hb; /* G9 R6 heartbeat view */
/* I07 drain telemetry: the per-drain record (server_world.h). The flood's
 * yield hook (every 256 pops) stamps the slice clock while a drain is
 * armed; outside a drain it is plain thread_yield. */
static struct server_world_light_drain_hb s_light_drain_hb;
static struct server_world_light_drain_rec s_light_drain_last;
static bool s_light_slice_armed;
static ptime_t s_light_slice_t0, s_light_drain_t0;
static uint32_t s_light_resolve_t0, s_light_hit_t0;
static uint64_t s_light_yield_us;
static uintptr_t s_light_frontier_address, s_light_seeds_address;
static uint32_t s_light_slices, s_light_slice_min_us, s_light_slice_max_us;
static void server_world_light_yield(void) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(server_world_test_history_hook && s_history_depth
	   && strcmp(s_history_operations[s_history_depth - 1], "drain") == 0)
		G9B_NOTE(s_history_worlds[s_history_depth - 1], "drain", "yield", "",
			0, 0, NULL);
#endif
	if(thread_drain_wait_active() && s_light_slice_armed) {
		const ptime_t now = time_get();
		const uint32_t us
			= (uint32_t)(time_diff_s(s_light_slice_t0, now) * 1.0e6f);
		s_light_slice_t0 = now;
		s_light_slices++;
		if(s_light_slices == 1 || us < s_light_slice_min_us)
			s_light_slice_min_us = us;
		if(us > s_light_slice_max_us)
			s_light_slice_max_us = us;
	}
	const ptime_t start = time_get();
	if(thread_g9_lease_active_owner()) {
		thread_g9_lease_checkpoint();
	}
	else thread_yield();
	if(thread_drain_wait_active() && s_light_slice_armed) s_light_yield_us += time_diff_us(start, time_get());
}
static void server_world_light_slice_arm(ptime_t t0) {
	s_light_slice_t0 = s_light_drain_t0 = t0;
	s_light_slices = 0;
	s_light_yield_us = 0;
	s_light_resolve_t0 = (uint32_t)s_chunkres_lookups;
	s_light_hit_t0 = (uint32_t)s_chunkres_hits;
	thread_drain_wait_begin();
	s_light_slice_min_us = s_light_slice_max_us = 0;
	s_light_slice_armed = true;
}
static void server_world_light_drain_record(
	const struct server_world* w, size_t seeds, size_t pops,
	const struct lighting_drain_stats* st, bool window) {
	s_light_slice_armed = false;
	g9_capture_record_enter(); /* before restore/telemetry mutex */
	thread_g9_lease_end(); /* before the telemetry mutex, including faults/trials */
	g9_capture_restore_return();
	thread_mutex_lock(&s_light_drain_hb_lock);
	struct thread_drain_waits waits;
	thread_drain_wait_end(&waits);
	uint32_t us = (uint32_t)time_diff_us(s_light_drain_t0, time_get());
	g9_capture_record_end(us); /* preserve the historical post-mutex end */
	g9_probe_end((uint32_t)pops, us);
	g9_worst_tick_drain(!window, (uint32_t)pops, us); /* nested: R20 Q3 */
	const unsigned k = w->world_index >= 1 ? 1u : 0u;
	struct server_world_light_drain_rec r = {
		.seeds = (uint32_t)seeds,
		.pops = (uint32_t)pops,
		.pushes = st->pushes,
		.writes = st->writes,
		.depth_peak = st->depth_peak,
		.us = us,
		.slices = s_light_slices,
		.resolves = (uint32_t)s_chunkres_lookups - s_light_resolve_t0,
		.resolve_misses = ((uint32_t)s_chunkres_lookups - s_light_resolve_t0)
			- ((uint32_t)s_chunkres_hits - s_light_hit_t0),
		.kernel_resolve_hits = s_kernel_resolve_hits,
		.kernel_resolve_misses = s_kernel_resolve_misses,
		.yield_us = (uint32_t)s_light_yield_us,
		.lock_us = (uint32_t)waits.lock_us,
		.recovery_us = (uint32_t)waits.recovery_us,
		.frontier_address = s_light_frontier_address,
		.seeds_address = s_light_seeds_address,
		.slice_min_us = s_light_slice_min_us,
		.slice_max_us = s_light_slice_max_us,
		.wait_us = waits.wait_us,
		.wait_mutex = waits.wait_mutex,
		.wait_holder_id = waits.wait_holder_id,
		.wait_holder_tag = waits.wait_holder_tag,
		.wait_stripe = waits.wait_stripe,
	};
	memcpy(r.wait_mutex_name, waits.wait_mutex_name, sizeof r.wait_mutex_name);
	s_light_drain_last = r;
	if(!window) {
		/* the checkpoint's trial drain (R5 point 3: real work, attributed) */
		if(us > s_light_drain_hb.ckpt.drain_us)
			s_light_drain_hb.ckpt.drain_us = us;
		if((uint32_t)pops > s_light_drain_hb.ckpt.drain_pops)
			s_light_drain_hb.ckpt.drain_pops = (uint32_t)pops;
		thread_mutex_unlock(&s_light_drain_hb_lock);
		g9_capture_record_return();
		return;
	}
	s_light_drain_hb.drains[k]++;
	s_light_drain_hb.pops_total[k] += (uint32_t)pops;
	if(us >= s_light_drain_hb.win[k].us)
		s_light_drain_hb.win[k] = r;
	/* ci-1101/37: keep the window's longest >= 10 ms drain mutex wait and its
	 * lock, regardless of which drain carried it. */
	if(r.wait_us > s_light_drain_hb.wait_attrib.wait_us) {
		s_light_drain_hb.wait_attrib.wait_us = r.wait_us;
		s_light_drain_hb.wait_attrib.mutex = r.wait_mutex;
		s_light_drain_hb.wait_attrib.holder_id = r.wait_holder_id;
		s_light_drain_hb.wait_attrib.holder_tag = r.wait_holder_tag;
		s_light_drain_hb.wait_attrib.stripe = r.wait_stripe;
		memcpy(s_light_drain_hb.wait_attrib.name, r.wait_mutex_name,
			   sizeof s_light_drain_hb.wait_attrib.name);
	}
	thread_mutex_unlock(&s_light_drain_hb_lock);
	g9_capture_record_return();
}
void server_world_light_drain_hb_take(
	struct server_world_light_drain_hb* out) {
	if(!out)
		return;
	/* One critical section for producer records and take/reset: preserves
	 * the whole winning drain record and never loses a checkpoint maximum. */
	if(!column_backing_init()) { memset(out, 0, sizeof *out); return; }
	thread_mutex_lock(&s_light_drain_hb_lock);
	*out = s_light_drain_hb;
	memset(s_light_drain_hb.win, 0, sizeof s_light_drain_hb.win);
	memset(&s_light_drain_hb.ckpt, 0, sizeof s_light_drain_hb.ckpt);
	memset(&s_light_drain_hb.wait_attrib, 0, sizeof s_light_drain_hb.wait_attrib);
	thread_mutex_unlock(&s_light_drain_hb_lock);
}
static void server_world_light_ckpt_max(uint32_t* slot, uint32_t us) {
	thread_mutex_lock(&s_light_drain_hb_lock);
	if(us > *slot)
		*slot = us;
	thread_mutex_unlock(&s_light_drain_hb_lock);
}
static void server_world_light_ckpt_record(uint32_t us, uint32_t restore_us) {
	thread_mutex_lock(&s_light_drain_hb_lock);
	if(us > s_light_drain_hb.ckpt.us) s_light_drain_hb.ckpt.us = us;
	if(restore_us > s_light_drain_hb.ckpt.restore_us)
		s_light_drain_hb.ckpt.restore_us = restore_us;
	s_light_drain_hb.ckpt.n++;
	thread_mutex_unlock(&s_light_drain_hb_lock);
}
#ifdef CAVEWII_SERVER_LOCAL_TEST
void server_world_test_light_ckpt_record(uint32_t us) {
	server_world_light_ckpt_record(us, us);
}
#endif

void server_world_light_drain_last(struct server_world_light_drain_rec* out) {
	if(out)
		*out = s_light_drain_last;
}
/* Queue / frontier bytes kept across ticks (more is freed after a drain). */
#define SERVER_WORLD_LIGHT_SCRATCH_KEEP ((size_t)16384)

void server_world_light_stats(struct server_world_light_stats* out) {
	if(out)
		*out = s_light_stats;
}

void server_world_light_hb(struct server_world_light_hb* out) {
	if(!out)
		return;
	*out = s_light_hb;
	out->faults = s_light_stats.faults;
	out->ceil = s_light_stats.drain_ceiling;
	out->oom = s_light_stats.drain_oom;
	out->ckpt_fail = s_light_stats.trial_alloc_fail;
	out->trials = s_light_stats.checkpoint_trials;
}

/* G9 R6: light time spent in the current server update (drains + trials). */
static uint32_t s_light_tick_accum_us;
static bool s_light_trial_running; /* the trial counts its drain itself */
static void server_world_light_tick_add(uint32_t us) {
	s_light_tick_accum_us = UINT32_MAX - s_light_tick_accum_us < us ?
		UINT32_MAX :
		s_light_tick_accum_us + us;
	if(s_light_tick_accum_us > s_light_hb.tick_us)
		s_light_hb.tick_us = s_light_tick_accum_us;
}
void server_world_light_tick_begin(void) {
	s_light_tick_accum_us = 0;
}

static uint32_t server_world_light_u32(size_t v) {
	return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static unsigned server_world_light_slot(const struct server_world* w) {
	return w->world_index >= 1 ? 1u : 0u;
}

/* Re-account w's allocated queue storage (after a grow, every flood --
 * completed or stopped -- and a drain's trim): each array's capacity peak
 * separately, the frontier (flood scratch) peak, and the totals. */
static void server_world_light_account(struct server_world* w) {
	const unsigned k = server_world_light_slot(w);
	const size_t sb = w->light_seed_cap * sizeof *w->light_seeds;
	const size_t ub = w->light_undo_cap * sizeof *w->light_undo;
	const size_t pb = w->light_pin_cap * sizeof *w->light_pins;
	const size_t fb = lighting_drain_queue_bytes(&w->light_frontier);
	const uint32_t b32 = server_world_light_u32(sb + ub + pb + fb);
	s_light_hb.bytes[k] = b32;
	if(b32 > s_light_hb.peak[k])
		s_light_hb.peak[k] = b32;
	if(server_world_light_u32(sb) > s_light_hb.seed_b[k])
		s_light_hb.seed_b[k] = server_world_light_u32(sb);
	if(server_world_light_u32(ub) > s_light_hb.undo_b[k])
		s_light_hb.undo_b[k] = server_world_light_u32(ub);
	if(server_world_light_u32(pb) > s_light_hb.pin_b[k])
		s_light_hb.pin_b[k] = server_world_light_u32(pb);
	if(server_world_light_u32(fb) > s_light_hb.front_b[k])
		s_light_hb.front_b[k] = server_world_light_u32(fb);
	if(w->light_seed_n > s_light_hb.seeds_pk[k])
		s_light_hb.seeds_pk[k] = server_world_light_u32(w->light_seed_n);
}

void server_world_light_note_boundary(const struct server_world* w) {
	if(!w)
		return;
	const unsigned k = server_world_light_slot(w);
	if(server_world_light_u32(w->light_seed_n) > s_light_hb.debt[k])
		s_light_hb.debt[k] = server_world_light_u32(w->light_seed_n);
}

void server_world_light_note_send_deferred(void) {
	s_light_stats.send_deferred++;
}

void server_world_light_note_evict_deferred(void) {
	s_light_stats.evict_deferred++;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
void server_world_test_set_light_sync(bool sync) {
	s_light_sync_test = sync;
}
void server_world_test_set_light_lifo(bool lifo) {
	s_light_lifo_test = lifo;
}
void server_world_test_light_fail_after(long n) {
	s_light_fail_after = n;
}
void server_world_test_light_stats_reset(void) {
	assert(column_backing_init());
	memset(&s_light_stats, 0, sizeof s_light_stats);
	memset(&s_light_hb, 0, sizeof s_light_hb);
	thread_mutex_lock(&s_light_drain_hb_lock);
	memset(&s_light_drain_hb, 0, sizeof s_light_drain_hb);
	thread_mutex_unlock(&s_light_drain_hb_lock);
	memset(&s_light_drain_last, 0, sizeof s_light_drain_last);
}
void server_world_test_light_clear_fault(struct server_world* w) {
	/* Explicit isolated-fixture reset only; sticky storage/journal stay stopped. */
	if(!w->gen_storage_failed && !w->light_journal_failed) {
		w->simulation_stopped = false;
		w->light_fault_accounted = false;
		if(w->session) for(uint8_t i=0; i<SERVER_LOCAL_WORLD_SLOTS; i++)
			if(!w->session->worlds[i].light_fault)
				w->session->worlds[i].simulation_stopped = false;
	}
	w->light_fault = false;
	w->light_fault_reason = SERVER_LIGHT_FAULT_NONE;
	/* G9X-5a: a generic light-fault reset never clears the sticky
	 * population backing-storage stop. */
	if(w->gen_storage_failed) {
		w->light_fault = true;
		w->light_fault_reason = SERVER_LIGHT_FAULT_POPULATION_STORAGE;
	}
	/* G9X-5b: nor the sticky failed checkpoint-journal restore (the live
	 * light is partially restored; only teardown ends it). */
	if(w->light_journal_failed) {
		w->light_fault = true;
		w->light_fault_reason = SERVER_LIGHT_FAULT_CHECKPOINT_JOURNAL;
	}
}
static size_t s_light_ceiling_test = 0;
void server_world_test_light_drain_ceiling(size_t pops) {
	s_light_ceiling_test = pops;
}
#endif

/* G9 R3 (ASTRA-G9-REVIEW-2 item 1): a light stop of a session world raises
 * the publication barrier at once (clin_server_publication_stop: no window,
 * gameplay or terrain RPC from any producer follows it) and every simulation
 * loop of the session ends at its next iteration (server_world_light_halted).
 * The terminal itself is published by server_local at its next containment
 * check. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*server_world_test_light_fault_hook)(struct server_world* w) = NULL;
void (*server_world_test_light_drain_hook)(struct server_world* w) = NULL;
void (*server_world_test_light_write_hook)(w_coord_t x, w_coord_t y,
										   w_coord_t z, uint8_t light) = NULL;
static unsigned s_light_trial_fail_test;
void server_world_test_light_trial_fail(unsigned n) {
	s_light_trial_fail_test = n;
}
static bool s_light_trial_reference_test;
static bool s_light_trial_eager_test;
void server_world_test_light_trial_reference(bool on) {
	s_light_trial_reference_test = on;
}
void server_world_test_light_trial_eager(bool on) {
	s_light_trial_eager_test = on;
}
#endif
void server_world_stop_simulation(struct server_world* w, uint8_t reason) {
	if(!w || w->light_fault_accounted) return;
	const bool already_stopped = w->simulation_stopped;
	/* Import a legacy first cause; a peer stop is not a new fault origin. */
	if(!w->light_fault || w->light_fault_reason == SERVER_LIGHT_FAULT_NONE)
		w->light_fault_reason = reason;
	w->light_fault = true;
	w->light_fault_accounted = true;
	w->simulation_stopped = true;
	if(w->session && !already_stopped) {
		w->session->light_stop_slot = w->world_index;
		for(uint8_t i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++)
			w->session->worlds[i].simulation_stopped = true;
	}
	s_light_stats.faults++;
	if(w->light_fault_reason == SERVER_LIGHT_FAULT_DRAIN_OOM) s_light_stats.drain_oom++;
	if(w->light_fault_reason == SERVER_LIGHT_FAULT_DRAIN_CEILING) s_light_stats.drain_ceiling++;
	if(w->session)
		clin_server_publication_stop(true);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(server_world_test_light_fault_hook)
		server_world_test_light_fault_hook(w);
#endif
}

/* Slow server-thread import; never used in a hot neighbour loop. */
static void server_world_import_simulation_stop(struct server_world* w) {
	if(!w || w->light_fault_accounted) return;
	if(w->light_fault || w->light_fault_reason == SERVER_LIGHT_FAULT_POPULATION_STORAGE) {
		server_world_stop_simulation(w, w->light_fault_reason);
		return;
	}
	if(w->session) for(uint8_t i=0; i<SERVER_LOCAL_WORLD_SLOTS; i++) {
		struct server_world* origin=&w->session->worlds[i];
		if(origin->light_fault) {
			server_world_stop_simulation(origin, origin->light_fault_reason);
			return;
		}
	}
	if(clin_rpc_publication_faulted())
		server_world_stop_simulation(w->session ? &w->session->worlds[0] : w,
			SERVER_LIGHT_FAULT_PUBLICATION);
}

bool server_world_light_halted(const struct server_world* w) {
	if(!w) return false;
	if(w->light_fault) return true;
	if(w->light_fault_reason == SERVER_LIGHT_FAULT_POPULATION_STORAGE) return true;
	/* Retain the acquire-based containment query at its existing boundaries. */
	const bool session_stopped = w->session && server_local_light_stopped(w->session);
	return w->simulation_stopped || session_stopped;
}

static bool server_world_light_grow(void** buf, size_t* cap, size_t need,
									size_t elem) {
	if(need <= *cap)
		return true;
	size_t nc = *cap ? *cap : 16;
	while(nc < need)
		nc *= 2;
	void* nb = realloc(*buf, nc * elem);
	if(!nb)
		return false;
	*buf = nb;
	*cap = nc;
	return true;
}

/* Reserve room for `seeds` more seeds and `cols` more undo columns BEFORE any
 * mutation. On failure nothing was written; the world is stopped
 * (light_fault) so no partial state is ever published (ruling 7). */
static bool server_world_light_reserve(struct server_world* w, size_t seeds,
									   size_t cols) {
	if(server_world_light_halted(w)) {
		server_world_import_simulation_stop(w);
		return false;
	}
	bool ok = true;
	if(s_light_fail_after == 0)
		ok = false;
	else if(s_light_fail_after > 0)
		s_light_fail_after--;
	const size_t rs = w->light_resv_seeds + seeds;
	const size_t rc = w->light_resv_cols + cols;
	ok = ok
		&& server_world_light_grow((void**)&w->light_seeds, &w->light_seed_cap,
								   w->light_seed_n + rs,
								   sizeof *w->light_seeds)
		&& server_world_light_grow((void**)&w->light_undo, &w->light_undo_cap,
								   w->light_undo_n + rc,
								   sizeof *w->light_undo)
		&& server_world_light_grow((void**)&w->light_pins, &w->light_pin_cap,
								   w->light_pin_n + rs,
								   sizeof *w->light_pins);
	if(ok) {
		w->light_resv_seeds = rs;
		w->light_resv_cols = rc;
		server_world_light_account(w);
	}
	if(!ok) {
		server_world_stop_simulation(w, SERVER_LIGHT_FAULT_RESERVE);
		fprintf(stderr,
				"[light] world %u: pending-light reservation failed (%u "
				"seeds pending); world stopped before the write, nothing "
				"published\n",
				(unsigned)w->world_index, (unsigned)w->light_seed_n);

	}
	return ok;
}

/* A write site gives back its reservation when it returns. Every seed it
 * pushed in between used capacity counted by it (nested sites by theirs). */
static void server_world_light_release(struct server_world* w, size_t seeds,
									   size_t cols) {
	assert(w->light_resv_seeds >= seeds && w->light_resv_cols >= cols);
	w->light_resv_seeds -= seeds;
	w->light_resv_cols -= cols;
}

bool server_world_light_pending(const struct server_world* w) {
	return w && (w->light_seed_n > 0 || w->generation_light_n > 0);
}

bool server_world_light_pinned(const struct server_world* w, w_coord_t cx,
							   w_coord_t cz) {
	if(!w)
		return false;
	for(size_t i = 0; i < w->light_pin_n; i++) {
		w_coord_t px = S_CHUNK_X(w->light_pins[i]);
		w_coord_t pz = S_CHUNK_Z(w->light_pins[i]);
		if(px - cx <= 1 && cx - px <= 1 && pz - cz <= 1 && cz - pz <= 1)
			return true;
	}
	return false;
}

/* Capacity was reserved by the caller. */
static void server_world_light_seed(struct server_world* w, w_coord_t x,
									w_coord_t y, w_coord_t z) {
	assert(w->light_seed_n < w->light_seed_cap);
	w->light_seeds[w->light_seed_n++] = (struct world_modification_entry) {
		.x = x, .y = y, .z = z};
	if(w->light_seed_n > s_light_stats.seeds_peak)
		s_light_stats.seeds_peak = (uint32_t)w->light_seed_n;
	if(w->light_seed_n > s_light_hb.seeds_pk[server_world_light_slot(w)])
		s_light_hb.seeds_pk[server_world_light_slot(w)]
			= (uint32_t)w->light_seed_n;
	s_light_stats.seeds_total++;
	int64_t id = S_CHUNK_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
	for(size_t i = 0; i < w->light_pin_n; i++)
		if(w->light_pins[i] == id)
			return;
	assert(w->light_pin_n < w->light_pin_cap);
	w->light_pins[w->light_pin_n++] = id;
}

/* Block.lightOpacity as the engine stores it: an opaque block is 255, a
 * see-through block its `opacity` (ice 3, water 3, leaves 1, glass 0). */
static int server_world_beta_light_opacity(uint8_t type) {
	const struct block* b = blocks[type];
	if(!b)
		return 0;
	return b->can_see_through ? b->opacity : 255;
}

/* Chunk.func_339_g's immediate writes for one column whose height moved from
 * old_h to new_h [CFR Chunk.java:141-200]: a lowering sets [new, old) to 15
 * (:174-176), a raise sets [old, new) to 0 (:178-180), then the attenuation
 * walk from the new top down (:182-193; opacity 0 counts as 1, stops once the
 * value reaches 0). WithMetadata guards this in Nether; plain ID does not
 * (Chunk.java:218-231,242-276). Capture pre-batch Sky only for Sky drains. */
static bool server_beta_light_schedule(struct server_world* w, int channel,
	int x1, int y1, int z1, int x2, int y2, int z2);
static void server_world_light_column_write_impl(struct server_world* w,
											struct server_chunk* sc,
											w_coord_t x, w_coord_t z,
											int old_h, int new_h, bool remember) {
	if(old_h == new_h || !sc)
		return;
	bool have = false;
	for(size_t i = w->light_undo_n; i-- > 0;)
		if(w->light_undo[i].x == x && w->light_undo[i].z == z) {
			have = true;
			break;
		}
	const c_coord_t lx = W2C_COORD(x), lz = W2C_COORD(z);
	/* Nether has no Sky queue; its direct setter stores survive Block drains.
	 * CFR World.java:1300-1301. */
	if(remember && !have && w->dimension != WORLD_DIM_NETHER) {
		assert(w->light_undo_n < w->light_undo_cap);
		struct server_light_undo* u = &w->light_undo[w->light_undo_n++];
		u->x = x;
		u->z = z;
		/* Y is the contiguous, even-aligned 128-cell axis: identical packed
		 * bytes to the per-cell nibble read/write, without 256 helper calls. */
		memcpy(u->sky, sc->lighting_sky + S_CHUNK_IDX(lx, 0, lz) / 2,
			sizeof u->sky);
		if(w->light_undo_n > s_light_stats.undo_peak)
			s_light_stats.undo_peak = (uint32_t)w->light_undo_n;
	}
	if(new_h < old_h) {
		for(int y = new_h; y < old_h; y++)
			server_chunk_store(sc, COLUMN_SKY, S_CHUNK_IDX(lx, y, lz), 15);
	} else {
		if(!remember && !server_beta_light_schedule(w, COLUMN_SKY, x, old_h, z, x, new_h, z)) {
			w->light_fault = true;
			w->light_fault_reason = SERVER_LIGHT_FAULT_RESERVE;
		}
		for(int y = old_h; y < new_h; y++)
			server_chunk_store(sc, COLUMN_SKY, S_CHUNK_IDX(lx, y, lz), 0);
	}
	int v = 15;
	int y = new_h;
	while(y > 0 && v > 0) {
		y--;
		int op = server_world_beta_light_opacity(
			sc->ids[S_CHUNK_IDX(lx, y, lz)]);
		if(op == 0)
			op = 1;
		v -= op;
		if(v < 0)
			v = 0;
		server_chunk_store(sc, COLUMN_SKY, S_CHUNK_IDX(lx, y, lz), (uint8_t)v);
	}
	if(!remember) {
		while(y > 0 && !server_world_beta_light_opacity(sc->ids[S_CHUNK_IDX(lx, y - 1, lz)])) y--;
		if(y != new_h && !server_beta_light_schedule(w, COLUMN_SKY, x-1, y, z-1, x+1, new_h, z+1)) {
			w->light_fault = true;
			w->light_fault_reason = SERVER_LIGHT_FAULT_RESERVE;
		}
	}
	server_world_chunk_touch(sc);
}

static void server_world_light_column_write(struct server_world* w,
	struct server_chunk* sc, w_coord_t x, w_coord_t z, int old_h, int new_h) {
	server_world_light_column_write_impl(w, sc, x, z, old_h, new_h, true);
}

/* Plain Chunk.setBlockID has no hasNoSky guard. Scan from max(old,y+1)
 * for an opaque replacement, or from old for removal at old-1, and store
 * the height ONCE before func_339_g's Sky writes. CFR Chunk.java:142-202,
 * 242-276; independent original-JAR witness at (8,75,8), seed4242. */
static void server_world_plain_column_write_impl(struct server_world* w,
	struct server_chunk* sc, int x, int y, int z, uint8_t type, bool remember) {
	const unsigned hi = W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE;
	const int old_h = sc->heightmap[hi];
	int new_h = old_h;
	if(server_world_beta_light_opacity(type)) {
		if(y < old_h) return;
		new_h = y + 1;
	} else if(y != old_h - 1) return;
	while(new_h > 0 && !server_world_beta_light_opacity(
		sc->ids[S_CHUNK_IDX(x, new_h - 1, z)])) new_h--;
	if(new_h == old_h) return;
	column_backing_write_begin(sc->backing, 0, 0, 1);
	G9B_BYTE(sc->heightmap, hi, new_h);
	column_backing_write_end(sc->backing, 0, 0, 1);
	server_world_light_column_write_impl(w, sc, x, z, old_h, new_h, remember);
}
static void server_world_plain_column_write(struct server_world* w,
	struct server_chunk* sc, int x, int y, int z, uint8_t type) {
	server_world_plain_column_write_impl(w, sc, x, y, z, type, true);
}

/* ASTRA-G9-REVIEW Required 2: the drain is lossless. It returns true only
 * when every pending seed has settled. Order of a drain:
 *   1. seed the world-owned frontier, all or nothing (a failure mutates
 *      nothing: the field still holds the pre-drain Beta values);
 *   2. restore each touched column's pre-batch skylight (private undo);
 *   3. flood, reserving each pop's pushes before the pop.
 * A stop (frontier OOM, or the pop ceiling, which is the old per-edit budget
 * times the seed count, so batching never tightens it) keeps the frontier,
 * seeds, pins and undo, records the cause and stops the world (light_fault):
 * the caller must not let an entity, tile entity, save or publication read
 * it. Nothing is dropped and nothing pinned is ever written or sent. */
static void server_world_light_stop(struct server_world* w, uint8_t reason,
									size_t pops) {
	if(w->light_fault_accounted) return;
	server_world_stop_simulation(w, reason);
	fprintf(stderr,
			"[light] world %u: drain stopped (%s) after %zu pops; %zu seeds, "
			"%zu frontier cells retained; world stopped, nothing published\n",
			(unsigned)w->world_index,
			reason == SERVER_LIGHT_FAULT_DRAIN_OOM ? "frontier alloc"
			: reason == SERVER_LIGHT_FAULT_POPULATION_STORAGE ? "population backing storage"
			: reason == SERVER_LIGHT_FAULT_CHECKPOINT_JOURNAL ? "checkpoint journal restore"
			: "ceiling",
			pops, w->light_seed_n, stack_size(&w->light_frontier));

}

#include "server_beta_light.inc"

#ifdef CAVEWII_SERVER_LOCAL_TEST
size_t server_world_test_generation_queue_copy(const struct server_world* w,
	void* out,size_t capacity) {
	/* the LOGICAL list, spilled pages included (G9X-5) */
	const size_t bytes=w->generation_light_n * sizeof(struct server_beta_light_region);
	if(out && capacity>=bytes)
		for(size_t i=0;i<w->generation_light_n;i++) {
			const bool read=sbl_peek(w,i,(struct server_beta_light_region*)out+i);
			assert(read);
			(void)read;
		}
	return bytes;
}
#endif

/* CFR Chunk.java:122-140: func_333_c compares the column against each
 * neighboring saved height and schedules the intervening Sky interval.
 * func_355_f marks the setter's chunk modified right after each such request
 * (Chunk.java:134/137); `touch` is that chunk for a resident setter target
 * (G9X-3), NULL for a chunk not yet installed (generation). */
static bool server_beta_light_height_edges(struct server_world* w,
	const struct server_chunk* sc, int x, int z, struct server_chunk* touch) {
	const int h = sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE];
	const int xyz[4][2] = {{x-1,z},{x+1,z},{x,z-1},{x,z+1}};
	for(unsigned k = 0; k < 4; k++) {
		uint8_t nh = 0;
		(void)server_world_get_height(w, xyz[k][0], xyz[k][1], &nh);
		if(nh == h) continue;
		const bool queued = server_beta_light_schedule(w, COLUMN_SKY,
			xyz[k][0], nh < h ? nh : h, xyz[k][1],
			xyz[k][0], nh > h ? nh : h, xyz[k][1]);
		if(touch) server_world_chunk_touch(touch);
		if(!queued) return false;
	}
	return true;
}

static bool server_world_light_drain_ex(struct server_world* w, bool trim);

struct server_light_certificate {
	struct server_world* world;
	struct column_light_publication* publication;
	bool valid;
	unsigned writes;
};

static bool server_light_certificate_get(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z, struct block_data* block, uint8_t* height) {
	struct server_light_certificate* c = user;
	struct column_light_publication* p = c->publication;
	struct column_light_cell cell = {.x = x, .y = y, .z = z};
	cell.exists = server_world_light_get_block(c->world, x, y, z,
		&cell.block, &cell.height);
	bool known = false;
	for(unsigned n = 0; n < p->cells; n++)
		if(p->read[n].x == x && p->read[n].y == y && p->read[n].z == z)
			known = true;
	if(!known) {
		if(p->cells == COLUMN_LIGHT_READS) c->valid = false;
		else p->read[p->cells++] = cell;
	}
	if(y >= 0 && y < WORLD_HEIGHT) {
		int32_t cx = WCOORD_CHUNK_OFFSET(x), cz = WCOORD_CHUNK_OFFSET(z);
		known = false;
		for(unsigned n = 0; n < p->dependencies; n++)
			if(p->dependency[n].x == cx && p->dependency[n].z == cz) known = true;
		if(!known) {
			if(p->dependencies == COLUMN_LIGHT_DEPENDENCIES) c->valid = false;
			else {
				struct column_light_dependency* d = &p->dependency[p->dependencies++];
				d->x = cx; d->z = cz;
				struct server_chunk* sc = server_world_resolve_chunk(c->world, cx, cz);
				d->backing = sc ? sc->backing : NULL;
				if(sc && !d->backing) c->valid = false;
				if(d->backing) {
					column_backing_ref(d->backing);
					thread_stripe_lock(d->backing->mutex);
					d->revision = d->backing->revision;
					/* The wrapper has completed a committed whole-world drain. */
					d->backing->settled_light_revision = d->revision.light;
					thread_stripe_unlock(d->backing->mutex);
				}
			}
		}
	}
	if(block) *block = cell.block;
	if(height) *height = cell.height;
	return cell.exists;
}
static bool server_light_certificate_light(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z, uint8_t* sky, uint8_t* torch) {
	struct block_data b;
	bool exists = server_light_certificate_get(user, x, y, z, &b, NULL);
	*sky = b.sky_light; *torch = b.torch_light;
	return exists;
}
static void server_light_certificate_write(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z, uint8_t light) {
	struct server_light_certificate* c = user;
	struct column_light_publication* p = c->publication;
	struct block_data b;
	bool exists = server_light_certificate_get(user, x, y, z, &b, NULL);
	/* No live backing mutation. A changing or propagating flood is rejected. */
	if(!exists || x != p->x || y != p->y || z != p->z
	   || light != (uint8_t)(b.sky_light | b.torch_light << 4) || ++c->writes != 1)
		c->valid = false;
}
static void server_world_publish_settled(struct server_world* w,
	struct world_modification_entry source) {
	/* The exact probe may read two cells beyond the source. Reject edge
	 * coordinates before any signed addition in this optional path. */
	if(source.x < INT32_MIN + 2 || source.x > INT32_MAX - 2
	   || source.z < INT32_MIN + 2 || source.z > INT32_MAX - 2) {
		column_light_note_fallback(COLUMN_LIGHT_CHANGED);
		return;
	}
	if(!w->session || w->world_index >= SERVER_LOCAL_WORLD_SLOTS
	   || w != &w->session->worlds[w->world_index]
	   || !world_generation_is_local(w->generation) || !server_local_world_live(w)) {
		column_light_note_fallback(COLUMN_LIGHT_OWNER);
		return;
	}
	if(column_view_recovery_pending()) {
		column_light_note_fallback(COLUMN_LIGHT_VERSION);
		return;
	}
	ptime_t start = time_get();
	struct column_light_publication* p = column_light_publication_alloc();
	if(!p) return;
	p->owner = w->backing_world_instance;
	p->world_generation = w->generation; p->world_index = w->world_index;
	p->dimension = w->dimension;
	p->mutation_epoch = column_light_mutation_epoch();
	p->presentation_epoch = column_view_presentation_epoch();
	p->x = source.x; p->y = source.y; p->z = source.z;
	struct server_light_certificate certificate = {w, p, true, 0};
	/* Sufficient fixed-point proof, without duplicating light arithmetic:
	 * every existing source/direct neighbour is opaque, dark and below height.
	 * The exact kernel can then only write the forced source, unchanged. */
	static const int offsets[7][3] = {
		{0, 0, 0}, {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0},
		{0, 0, 1}, {0, 0, -1}};
	for(unsigned n = 0; n < 7; n++) {
		struct block_data b;
		uint8_t height;
		int32_t y = p->y + offsets[n][1];
		bool exists = server_light_certificate_get(&certificate,
			p->x + offsets[n][0], y, p->z + offsets[n][2], &b, &height);
		if(exists && (!blocks[b.type] || blocks[b.type]->can_see_through
		   || blocks[b.type]->luminance || b.sky_light || b.torch_light
		   || (w->dimension != WORLD_DIM_NETHER && y >= height)))
			certificate.valid = false;
	}
	if(!certificate.valid) {
		column_light_note_fallback(COLUMN_LIGHT_CHANGED);
		column_light_note_probe(0, (uint32_t)time_diff_us(start, time_get()), false);
		column_light_publication_release(p);
		return;
	}
	struct stack queue;
	/* Seven pops can push at most 42 entries. This scratch never allocates,
	 * grows, changes the client's flood diagnostics, or touches server light. */
	union { long double align; uint8_t bytes[1024]; } frontier;
	lighting_drain_queue_init(&queue);
	size_t element_size = queue.element_size;
	if(element_size > sizeof(frontier.bytes) / 43) {
		column_light_note_fallback(COLUMN_LIGHT_CAP);
		column_light_publication_release(p);
		return;
	}
	stack_create_fixed(&queue, frontier.bytes, sizeof(frontier.bytes) / element_size, element_size);
	bool seeded = lighting_drain_seed(&queue, &source, 1);
	size_t pops = 0;
	enum lighting_drain_result result = seeded ? lighting_drain_flood(&queue,
		w->dimension == WORLD_DIM_NETHER, 7, &pops, NULL,
		server_light_certificate_get, server_light_certificate_light,
		server_light_certificate_write, &certificate) : LIGHTING_DRAIN_OOM;
	p->pops = (uint32_t)pops;
	bool valid = certificate.valid && certificate.writes == 1
		&& result == LIGHTING_DRAIN_DONE && stack_empty(&queue) && p->mutation_epoch
		&& p->mutation_epoch == column_light_mutation_epoch()
		&& p->presentation_epoch == column_view_presentation_epoch()
		&& !column_view_recovery_pending();
	stack_destroy(&queue);
	bool published = false;
	uint32_t probe_pops = p->pops; /* enqueue transfers ownership immediately */
	if(valid) {
		struct client_rpc call = {.type = CRPC_LOCAL_SETTLED_LIGHT,
			.payload.settled_light = p};
		clin_rpc_stamp(&call, w->world_index, w->generation);
		published = clin_try_rpc_send(&call); /* never wait for MAIN */
		if(!published) column_light_note_fallback(COLUMN_LIGHT_INGRESS);
	} else column_light_note_fallback(COLUMN_LIGHT_CHANGED);
	column_light_note_probe(probe_pops, (uint32_t)time_diff_us(start, time_get()), published);
	if(!published) column_light_publication_release(p);
}

static bool server_world_light_drain_history_impl(struct server_world* w) {
	bool one = column_light_enabled() && w && w->light_seed_n == 1;
	struct world_modification_entry source = one ? w->light_seeds[0]
		: (struct world_modification_entry){0};
	if(column_light_enabled() && w && w->light_seed_n > 1)
		column_light_note_fallback(COLUMN_LIGHT_SEEDS);
	const bool ok = server_world_light_drain_ex(w, true);
	if(ok && one && !s_light_trial_running && !server_world_light_halted(w))
		server_world_publish_settled(w, source);
	/* G9 item 4: every prescribed boundary ages the generation hold; the
	 * held column's stream retries right after its drain, so a hold still
	 * set after two more boundaries belongs to a stream that moved on. */
	if(ok && w && w->light_gen_hold && ++w->light_gen_hold_age > 2)
		w->light_gen_hold = false;
	return ok;
}

bool server_world_light_drain(struct server_world* w) {
	uint64_t request = G9B_BEGIN(w, "drain", 0, 0);
	bool ok = server_world_light_drain_history_impl(w);
	G9B_END(w, "drain", request, ok ? "ok" : "failed", 0, 0);
	return ok;
}


/* trim = false only for the R1 checkpoint trial, whose rollback re-uses the
 * seed / undo / pin entries the drain merely un-counted. */
/* The same ascending SKY stores as the per-cell private undo. A matching
 * packed byte contains two equality no-ops; a differing byte still visits
 * both cells through the original synchronized writer. */
static void server_world_light_undo_column(struct server_chunk* sc,
	const struct server_light_undo* u) {
	const unsigned first = S_CHUNK_IDX(W2C_COORD(u->x), 0, W2C_COORD(u->z));
	for(unsigned b = 0; b < WORLD_HEIGHT / 2; b++) {
		const uint8_t value = u->sky[b];
		if(sc->lighting_sky[first / 2 + b] == value) continue;
		for(unsigned h = 0; h < 2; h++)
			server_chunk_store(sc, COLUMN_SKY, first + b * 2 + h,
				(uint8_t)((value >> (h * 4)) & 15));
	}
}

static bool server_world_light_drain_ex(struct server_world* w, bool trim) {
	if(!w)
		return true;
	if(w->light_fault
	   || w->light_fault_reason == SERVER_LIGHT_FAULT_POPULATION_STORAGE)
		return false; /* stopped: the retained work is never half-applied */
	if(!server_beta_light_drain(w)) return false;
	if(w->light_seed_n == 0) {
		w->light_undo_n = 0;
		w->light_pin_n = 0;
		return true;
	}
	ptime_t t0 = time_get();
	const bool nether = w->dimension == WORLD_DIM_NETHER;
	s_kernel_resolve_hits = s_kernel_resolve_misses = 0;
	size_t pops = 0;
	struct lighting_drain_stats st = {0};
	const size_t seeds = w->light_seed_n;
	if(s_light_lifo_test) {
		/* test-only Beta LIFO single-seed order (equivalence proof) */
		for(size_t i = w->light_undo_n; i-- > 0;) {
			const struct server_light_undo* u = &w->light_undo[i];
			struct server_chunk* sc = server_world_resolve_chunk(
				w, WCOORD_CHUNK_OFFSET(u->x), WCOORD_CHUNK_OFFSET(u->z));
			assert(sc);
			server_world_light_undo_column(sc, u);
		}
		for(size_t i = w->light_seed_n; i-- > 0;)
			lighting_update_at_block(w->light_seeds[i], nether,
									 server_world_light_get_block,
									 server_world_light_set_light, w);
	} else {
		g9_capture_drain_begin(w->world_index, w->generation, w->current_tick,
			s_light_trial_running ? 1u : 0u, (uint32_t)seeds);
		g9_probe_begin((uint32_t)w->world_index, s_light_trial_running ? 1u : 0u);
		server_world_light_slice_arm(t0);
		thread_g9_lease_begin(); /* logical drain, before its first mutation */
		if(!w->light_frontier_live) {
			/* 1. seed first: a failure here has changed nothing */
			if(!lighting_drain_seed(&w->light_frontier, w->light_seeds,
									w->light_seed_n)) {
				s_light_frontier_address = (uintptr_t)w->light_frontier.data;
				s_light_seeds_address = (uintptr_t)w->light_seeds;
				g9_capture_drain_final(0, LIGHTING_DRAIN_OOM);
				server_world_light_drain_record(w, seeds, 0, &st, !s_light_trial_running);
				server_world_light_stop(w, SERVER_LIGHT_FAULT_DRAIN_OOM, 0);
				return false;
			}
			w->light_frontier_live = true;
			/* 2. private restore: the flood starts from the pre-batch field */
			for(size_t i = w->light_undo_n; i-- > 0;) {
				const struct server_light_undo* u = &w->light_undo[i];
				struct server_chunk* sc = server_world_resolve_chunk(
					w, WCOORD_CHUNK_OFFSET(u->x), WCOORD_CHUNK_OFFSET(u->z));
				assert(sc && "pending light pins its chunks through retirement");
				if(!sc)
					continue;
				server_world_light_undo_column(sc, u);
			}
		}
		/* 3. the flood: same pops, order and values as
		 * lighting_update_at_blocks, or an explicit stop */
		size_t ceiling = (size_t)LIGHTING_DRAIN_CEILING_PER_SEED;
		ceiling = w->light_seed_n > SIZE_MAX / ceiling ?
			SIZE_MAX :
			ceiling * w->light_seed_n;
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(s_light_ceiling_test)
			ceiling = s_light_ceiling_test;
#endif
		s_light_frontier_address = (uintptr_t)w->light_frontier.data;
		s_light_seeds_address = (uintptr_t)w->light_seeds;

		enum lighting_drain_result r;
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(server_world_test_light_flood_override)
			r = server_world_test_light_flood_override(
				&w->light_frontier, nether, ceiling, &pops, &st,
				server_world_light_get_block, server_world_light_get_light,
				server_world_light_set_light, w);
		else
#endif
			r = server_light_kernel(w, nether, ceiling, &pops, &st);
		g9_capture_drain_final((uint32_t)pops, (uint32_t)r);
		/* G9 R6: the flood scratch peak counts stopped floods too */
		thread_g9_lease_end(); /* before fault handling and scratch/free paths */
		g9_capture_restore_return();
		server_world_light_account(w);
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(server_world_test_light_drain_hook)
			server_world_test_light_drain_hook(w);
#endif
		{
			const unsigned k = server_world_light_slot(w);
			const uint32_t us = (uint32_t)(time_diff_s(t0, time_get()) * 1.0e6f);
			if(pops > s_light_hb.drain_pops[k])
				s_light_hb.drain_pops[k] = server_world_light_u32(pops);
			if(us > s_light_hb.drain_us[k])
				s_light_hb.drain_us[k] = us;
			if(r != LIGHTING_DRAIN_DONE && !s_light_trial_running)
				server_world_light_tick_add(us);
		}
		if(r != LIGHTING_DRAIN_DONE) {
			server_world_light_drain_record(w, seeds, pops, &st, !s_light_trial_running);
			server_world_light_stop(w,
									r == LIGHTING_DRAIN_OOM ?
										SERVER_LIGHT_FAULT_DRAIN_OOM :
										SERVER_LIGHT_FAULT_DRAIN_CEILING,
									pops);
			return false;
		}
		w->light_frontier_live = false;
		if(pops > s_light_stats.drain_pops_max)
			s_light_stats.drain_pops_max = (uint32_t)pops;
	}
	w->light_seed_n = 0;
	w->light_undo_n = 0;
	w->light_pin_n = 0;
	uint32_t us = (uint32_t)(time_diff_s(t0, time_get()) * 1.0e6f);
	s_light_stats.drains++;
	s_light_stats.drain_us_last = us;
	if(us > s_light_stats.drain_us_max)
		s_light_stats.drain_us_max = us;
	if(us > s_light_hb.drain_us[server_world_light_slot(w)])
		s_light_hb.drain_us[server_world_light_slot(w)] = us;
	if(!s_light_trial_running)
		server_world_light_tick_add(us);
	server_world_light_account(w); /* the peak sees the grown buffers */
	/* All queue storage is per-drain scratch once the queue is empty: like
	 * the old per-flood stack, a large allocation is returned when the drain
	 * completes (a TNT blast reserves ~2x its cells of seeds, undo columns
	 * and pins, and its flood grows the frontier: ~320 KiB on PC), so a world
	 * never holds a blast-sized buffer across ticks. Small buffers are kept
	 * to avoid per-tick churn. Values are unaffected (storage only). */
	if(trim) {
		assert(w->light_resv_seeds == 0 && w->light_resv_cols == 0);
		if(lighting_drain_queue_bytes(&w->light_frontier)
		   > SERVER_WORLD_LIGHT_SCRATCH_KEEP) {
			stack_destroy(&w->light_frontier);
			lighting_drain_queue_init(&w->light_frontier);
		}
		if(w->light_seed_cap * sizeof *w->light_seeds
			   + w->light_undo_cap * sizeof *w->light_undo
			   + w->light_pin_cap * sizeof *w->light_pins
		   > SERVER_WORLD_LIGHT_SCRATCH_KEEP) {
			free(w->light_seeds);
			free(w->light_undo);
			free(w->light_pins);
			w->light_seeds = NULL;
			w->light_undo = NULL;
			w->light_pins = NULL;
			w->light_seed_cap = w->light_undo_cap = w->light_pin_cap = 0;
		}
		server_world_light_account(w);
	}
	if(!s_light_lifo_test)
		server_world_light_drain_record(w, seeds, pops, &st, !s_light_trial_running);
	return true;
}

static void server_world_light_free(struct server_world* w) {
	/* Free light storage, retaining terminal authority until session teardown. */
	if(!w->light_fault_accounted) {
		w->light_fault = false;
		w->light_fault_reason = SERVER_LIGHT_FAULT_NONE;
	}
	free(w->light_seeds);
	free(w->light_undo);
	free(w->light_pins);
	sbl_free(w); /* resident pages, the backing file (removed) */
	server_world_light_journal_free(w); /* G9X-5b: the checkpoint journal */
	stack_destroy(&w->light_frontier);
	lighting_drain_queue_init(&w->light_frontier);
	w->light_frontier_live = false;
	w->light_seeds = NULL;
	w->light_undo = NULL;
	w->light_pins = NULL;
	w->light_seed_n = w->light_seed_cap = 0;
	w->light_undo_n = w->light_undo_cap = 0;
	w->light_pin_n = w->light_pin_cap = 0;
}

// --- P0 perf instruments (debug overlay) ------------------------------------
// Per-tick cost of the block-edit path + the raw count of block dict fetches,
// surfaced to the F3 overlay the same single-process, no-sync way as
// s_spawner_us / s_light_accum_us. server_local_update latches the previous
// tick's totals and zeroes these at the tick boundary (perf_tick_reset below).
//   setblk us/n : wall time in (and count of) server_world_set_block this tick --
//                 its lighting flood + 6-neighbour notify, the #1 unbudgeted spike
//                 during explosions/fluids. Timed at the OUTERMOST call only so a
//                 recursive cascade (fluid/redstone/falling sand via
//                 onNeighbourBlockChange) is wall-timed once, not double-counted;
//                 the count still tallies every write, nested ones included.
//   getblk n    : server_world_get_block calls this tick -- the random-tick floor
//                 + per-mob collision-sweep dict-hash storm.
static float s_setblock_accum_us = 0;
static int   s_setblock_count = 0;
static int   s_setblock_depth = 0;
static int   s_getblock_count = 0;
static float s_setblock_last_us = 0;
static int   s_setblock_last_n = 0;
static int   s_getblock_last_n = 0;
// Session peak-hold of the per-tick totals: an explosion's flood is a single-tick
// transient that a periodic screenshot would miss, so latch the worst tick seen.
static float s_setblock_max_us = 0;
static int   s_getblock_max_n = 0;

bool server_world_get_block(struct server_world* w, w_coord_t x, w_coord_t y,
							w_coord_t z, struct block_data* blk) {
	assert(w && blk);

	s_getblock_count++; // P0 instrument: dict-fetch storm magnitude (per tick)

	if(y < 0 || y >= WORLD_HEIGHT)
		return false;

	struct server_chunk* sc = server_world_resolve_chunk(
		w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));

	if(!sc)
		return false;

	size_t idx = S_CHUNK_IDX(W2C_COORD(x), y, W2C_COORD(z));

	*blk = (struct block_data) {
		.type = sc->ids[idx],
		.metadata = nibble_read(sc->metadata, idx),
		.sky_light = nibble_read(sc->lighting_sky, idx),
		.torch_light = nibble_read(sc->lighting_torch, idx),
	};

	return true;
}

enum server_world_scan server_world_aabb_lava_scan(struct server_world* w,
	double min_x, double min_y, double min_z,
	double max_x, double max_y, double max_z) {
	assert(w);
	/* [740c583: World.isMaterialInBB] uses floor(min) and
	 * floor(max + 1.0D), then scans each half-open range. The walk order and
	 * the first-hit return are unchanged; a cell whose read fails is recorded
	 * as UNKNOWN only when its Y is in range (World.getBlockId answers 0 for
	 * y < 0 / y >= 128 before touching a chunk -- CFR World.java:206-214). */
	const w_coord_t x0 = (w_coord_t)floor(min_x);
	const w_coord_t x1 = (w_coord_t)floor(max_x + 1.0);
	const w_coord_t y0 = (w_coord_t)floor(min_y);
	const w_coord_t y1 = (w_coord_t)floor(max_y + 1.0);
	const w_coord_t z0 = (w_coord_t)floor(min_z);
	const w_coord_t z1 = (w_coord_t)floor(max_z + 1.0);
	bool unknown = false;

	for(w_coord_t x = x0; x < x1; x++)
		for(w_coord_t y = y0; y < y1; y++)
			for(w_coord_t z = z0; z < z1; z++) {
				struct block_data block;
				if(server_world_get_block(w, x, y, z, &block)) {
					if(block.type == BLOCK_LAVA_FLOW
					   || block.type == BLOCK_LAVA_STILL)
						return SERVER_WORLD_SCAN_PRESENT;
				} else if(y >= 0 && y < WORLD_HEIGHT)
					unknown = true;
			}
	return unknown ? SERVER_WORLD_SCAN_UNKNOWN : SERVER_WORLD_SCAN_ABSENT;
}

bool server_world_aabb_has_lava(struct server_world* w,
	double min_x, double min_y, double min_z,
	double max_x, double max_y, double max_z) {
	return server_world_aabb_lava_scan(w, min_x, min_y, min_z, max_x, max_y,
			   max_z)
		== SERVER_WORLD_SCAN_PRESENT;
}

bool server_world_columns_resident(struct server_world* w, double min_x,
	double min_z, double max_x, double max_z) {
	assert(w);
	if(!isfinite(min_x) || !isfinite(min_z) || !isfinite(max_x)
	   || !isfinite(max_z) || min_x > max_x || min_z > max_z
	   || fabs(min_x) > 3.2e7 || fabs(max_x) > 3.2e7
	   || fabs(min_z) > 3.2e7 || fabs(max_z) > 3.2e7)
		return false;
	const w_coord_t cx0 = WCOORD_CHUNK_OFFSET((w_coord_t)floor(min_x));
	const w_coord_t cx1 = WCOORD_CHUNK_OFFSET((w_coord_t)floor(max_x));
	const w_coord_t cz0 = WCOORD_CHUNK_OFFSET((w_coord_t)floor(min_z));
	const w_coord_t cz1 = WCOORD_CHUNK_OFFSET((w_coord_t)floor(max_z));
	if(cx1 - cx0 > 4 || cz1 - cz0 > 4)
		return false;
	for(w_coord_t cx = cx0; cx <= cx1; cx++)
		for(w_coord_t cz = cz0; cz <= cz1; cz++)
			if(!server_world_chunk_resident(w, cx, cz))
				return false;
	return true;
}

bool server_world_get_height(struct server_world* w, w_coord_t x, w_coord_t z,
							 uint8_t* out_height) {
	assert(w && out_height);
	// Reuse the lighting accessor's heightmap read (y is only used for its bounds
	// check there, so 0 is fine; blk=NULL skips the block fetch).
	return server_world_light_get_block(w, x, 0, z, NULL, out_height);
}

/* Resolve one edit, capture the old cell, and publish only the replacement ID.
 * The split is observable: Beta Chunk.setBlockIDWithMetadata calls the old
 * block's onBlockRemoval here, while getBlockId sees NEW and getBlockMetadata
 * still sees OLD. Lighting and neighbour notification happen later. */
static struct server_chunk* server_world_begin_block_write(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	struct block_data blk, struct block_data* old,
	bool* id_or_metadata_changed) {
	if(server_world_light_halted(w))
		return NULL;
	if(y < 0 || y >= WORLD_HEIGHT)
		return NULL;

	struct server_chunk* sc = server_world_resolve_chunk(
		w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
	if(!sc)
		return NULL;

	size_t idx = S_CHUNK_IDX(x, y, z);
	uint8_t old_type = sc->ids[idx];
	uint8_t old_metadata = nibble_read(sc->metadata, idx);
	const bool changed = old_type != blk.type || old_metadata != blk.metadata;
	if(old)
		*old = (struct block_data) {
			.type = old_type,
			.metadata = old_metadata,
			.sky_light = nibble_read(sc->lighting_sky, idx),
			.torch_light = nibble_read(sc->lighting_torch, idx),
		};
	if(id_or_metadata_changed)
		*id_or_metadata_changed = changed;
	/* Chunk.setBlockIDWithMetadata returns before setting isModified or writing
	 * anything when both fields match. World.*WithNotify consequently emits no
	 * lighting/client/neighbour work for an identical edit. */
	if(!changed)
		return sc;
	server_world_chunk_touch(sc);
	server_chunk_store(sc, COLUMN_IDS, idx, blk.type);
	return sc;
}

static void server_world_finish_block_metadata(
	struct server_chunk* sc, w_coord_t x, w_coord_t y, w_coord_t z,
	struct block_data blk) {
	assert(sc);
	size_t idx = S_CHUNK_IDX(x, y, z);
	server_chunk_store(sc, COLUMN_META, idx, blk.metadata);
	/* Chunk.setBlockIDWithMetadata writes this nibble twice. The second write is
	 * repeated by finish_block_write after height/lighting bookkeeping. */
}

/* `light`: a gameplay write site (set_block, piston moving, explosion fire)
 * that reserved one seed + one column beforehand. Outside the test-only
 * synchronous mode it does Chunk.func_339_g's immediate column write here --
 * after onBlockRemoval and the first metadata write, before the second
 * [CFR Chunk.java:216-235] -- and queues the cell's Sky/Block propagation
 * (:230/:232). Raw/silent writes never lit and still do not. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*server_world_test_block_write_hook)(struct server_world* w, w_coord_t x,
										   w_coord_t y, w_coord_t z,
										   uint8_t type, bool light) = NULL;
#endif
static void server_world_finish_block_write(
	struct server_world* w, struct server_chunk* sc,
	w_coord_t x, w_coord_t y, w_coord_t z, struct block_data blk, bool light,
	bool plain) {
	assert(w && sc);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(server_world_test_block_write_hook)
		server_world_test_block_write_hook(w, x, y, z, blk.type, light);
#endif
	server_world_finish_block_metadata(sc, x, y, z, blk);
	if(plain) {
		server_world_plain_column_write(w, sc, x, y, z, blk.type);
	} else if(w->dimension != WORLD_DIM_NETHER) {
		/* Height is retained privately by the client; synchronize only the
		 * backing snapshot. The callback is a read-only, allocation-free scan. */
		tps_mutated(2);
		uint8_t* hp = &sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE];
		const int old_h = *hp;
		column_backing_write_begin(sc->backing, 0, 0, 1);
		lighting_heightmap_update(sc->heightmap, W2C_COORD(x), y, W2C_COORD(z),
								  blk.type, server_chunk_get_block, sc);
		column_backing_write_end(sc->backing, 0, 0, 1);
		if(light && !s_light_sync_test)
			server_world_light_column_write(w, sc, x, z, old_h, *hp);
	}
	if(light && !s_light_sync_test)
		server_world_light_seed(w, x, y, z);
	/* Chunk.setBlockIDWithMetadata writes the metadata nibble a second time
	 * after relight/occlusion. Preserve that final-authority write. */
	if(!plain) server_world_finish_block_metadata(sc, x, y, z, blk);
}

// Callback-free primitive used by metadata-only/quiet paths. It still writes
// id+metadata and heightmap, but never performs lifecycle callbacks.
static struct server_chunk* server_world_write_block_raw(struct server_world* w,
		w_coord_t x, w_coord_t y, w_coord_t z, struct block_data blk,
		bool* id_or_metadata_changed) {
	bool changed = false;
	struct server_chunk* sc = server_world_begin_block_write(
		w, x, y, z, blk, NULL, &changed);
	if(id_or_metadata_changed)
		*id_or_metadata_changed = changed;
	if(!sc || !changed)
		return NULL;
	server_world_finish_block_write(w, sc, x, y, z, blk, false, false);
	return sc;
}

/* A1a bridge (split-dimensions-plan-0924; Astra D1 + ruling astra-A1a-a
 * item 9). Every mutation API names its world explicitly and derives session
 * services through w->session. Block/tile CALLBACKS (onBlockAdded,
 * onBlockRemoval, onNeighbourBlockChange, onBreak, the fire/portal hook) still
 * take `struct server_local*` and reach their world through the legacy slot-0
 * path until D4 hands them `w`; dispatching one for any other world would
 * apply its destination effects to worlds[0]. So a callback runs only for the
 * world it can see (slot 0). A mutation of any other world performs its own
 * write, lighting, scheduled ticks, counters and drops, but NO callback.
 * D4 landed: every dispatch below runs its callback inside
 * block_world_enter(s, w), and every block file reaches its world through
 * block_world(s), so a callback now runs for ANY world slot of its session
 * and acts on that world (onBlockAdded/Removal, neighbour change, scheduled /
 * random ticks, explosion onBreak -- the TNT chain, drops, fire). A world with
 * no session, or a world that is not one of its session's slots, still gets
 * none. In single-world play every mutation targets slot 0: byte-identical
 * (test_world_context_equiv). */
static struct server_local* server_world_callback_host(struct server_world* w) {
	struct server_local* s = w ? w->session : NULL;
	return s && w->world_index < SERVER_LOCAL_WORLD_SLOTS
			&& w == &s->worlds[w->world_index]
		? s : NULL;
}

/* M4 (A1a review): a world with NO owning session drops every block callback
 * above. That is the contract for generator/standalone worlds, but a test
 * fixture that merely forgot `w->session = s` would silently run a weaker
 * simulation. In test builds, fail loudly the moment a callback that WOULD
 * have been dispatched is dropped for want of a session. Production builds
 * compile this out and keep the silent skip (release behaviour unchanged; the
 * live load path binds the session before any mutation). Worlds other than
 * slot 0 of a real session are the intended D4 bridge and are not flagged. */
static inline void server_world_callback_guard(const struct server_world* w,
											   bool would_dispatch) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	assert(!(would_dispatch && w && !w->session)
		   && "block callback dropped: this world has no session -- bind "
			  "w->session to the owning server_local in the fixture");
#else
	(void)w;
	(void)would_dispatch;
#endif
}

/* Block edits and moving-block sidecars name the world they changed, just
 * like chunk snapshots. The old slot-0-only gate silently discarded every
 * live secondary edit; unstamped slot-0 edits are ambiguous with two worlds.
 * Preserve standalone fixtures and the flag-off legacy stream exactly. */
static void server_world_publish(struct server_world* w,
								 struct client_rpc* rpc) {
	if(!w) return;
	if(server_world_light_halted(w)) {
		server_world_import_simulation_stop(w);
		return;
	}
	if(rpc->type == CRPC_SET_BLOCK) {
		/* G9X-6: the client's copy of this column is no longer the last
		 * accepted CHUNK snapshot, so that snapshot's digest can no longer
		 * prove a later refresh redundant. */
		struct server_chunk* pc = server_world_resolve_chunk(w,
			WCOORD_CHUNK_OFFSET(rpc->payload.set_block.x),
			WCOORD_CHUNK_OFFSET(rpc->payload.set_block.z));
		if(pc)
			pc->pub_digest_valid = false;
	}
	if(column_light_enabled() && rpc->type == CRPC_SET_BLOCK) {
		struct server_chunk* sc = server_world_resolve_chunk(w,
			WCOORD_CHUNK_OFFSET(rpc->payload.set_block.x),
			WCOORD_CHUNK_OFFSET(rpc->payload.set_block.z));
		if(sc && sc->backing) {
			struct column_backing* b = sc->backing;
			thread_stripe_lock(b->mutex);
			if(b->revision.events && b->revision.events < UINT32_MAX)
				++b->revision.events;
			else b->revision.events = 0;
			rpc->payload.set_block.local_revision = b->revision;
			rpc->payload.set_block.local_previous_content = b->event_previous_content;
			b->event_previous_content = 0;
			column_light_mutated(NULL, false);
			thread_stripe_unlock(b->mutex);
		}
	}
	if(w->session && clin_world_payloads_enabled()) {
		if(!server_local_world_live(w)
		   || (w->world_index != 0 && !w->session->a2w_stream_on[w->world_index]))
			return;
		clin_rpc_stamp(rpc, w->world_index, w->generation);
		clin_rpc_send(rpc);
	} else if(!w->session || w == &w->session->worlds[0]) {
		clin_rpc_send(rpc);
	}
	if(clin_rpc_publication_faulted()) {
		server_world_stop_simulation(w, SERVER_LIGHT_FAULT_PUBLICATION);
	}
}

static void server_world_dispatch_block_removal(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	struct block_data* old) {
	struct server_local* s = server_world_callback_host(w);
	server_world_callback_guard(w, old && old->type != BLOCK_AIR && blocks[old->type]
									  && blocks[old->type]->onBlockRemoval);
	if(!s || !old || old->type == BLOCK_AIR || !blocks[old->type]
	   || !blocks[old->type]->onBlockRemoval)
		return;
	struct block_info info = {
		.block = old,
		.neighbours = NULL,
		.x = x,
		.y = y,
		.z = z,
	};
	{ struct server_world* bw_prev = block_world_enter(s, w);
	blocks[old->type]->onBlockRemoval(s, &info);
	block_world_leave(s, bw_prev); }
}

static struct server_chunk* server_world_write_block_lifecycle(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	struct block_data blk, bool* id_or_metadata_changed, bool light, bool plain) {
	assert(w);
	struct block_data old = {0};
	bool changed = false;
	if(plain) {
		struct block_data current;
		if(!server_world_get_block(w, x, y, z, &current) || current.type == blk.type)
			return NULL; /* Plain setter compares ID only, preserving metadata. */
		blk.metadata = 0;
	}
	struct server_chunk* sc = server_world_begin_block_write(
		w, x, y, z, blk, &old, &changed);
	if(id_or_metadata_changed)
		*id_or_metadata_changed = changed;
	if(!sc || !changed)
		return NULL;
	server_world_dispatch_block_removal(w, x, y, z, &old);
	server_world_finish_block_write(w, sc, x, y, z, blk, light, plain);
	return sc;
}

// Phase 3 of a block edit: the observable side effects, run AFTER the world ids AND
// the light field are final. The CRPC_SET_BLOCK + the block's own scheduled tick fire
// only when the block was actually written (sc != NULL). An identical id+metadata
// or an unavailable target returns before this phase, matching Beta's
// setBlockAndMetadataWithNotify=false/no-notify contract. No reachable handler reads
// the light field, so running this after the (now batched) flood is observably
// identical -- see the P1 equivalence analysis.
// WII single-core PACING + recursion bound for the synchronous neighbour-notify cascade
// (2026-06-27, the redstone place/break + clock-loop freeze). This dispatches each neighbour's
// onNeighbourBlockChange INLINE, and a handler may server_world_set_block -> notify again
// (recursion); a dense redstone edit (a clock's torch toggles / completing a feedback loop) ran
// it long enough to STARVE the main render thread on the 729 MHz Broadway core -> watchdog
// freeze. Never reproduced on PC (far faster + thread_yield is a PC no-op, so this is byte-exact
// there -- the redstone golden tests prove it). The periodic thread_yield hands the core back to
// render mid-cascade exactly like the wire flood does; it changes only WHEN handlers interleave
// with render, never which run or what they emit. NOTE: the SECOND notify path notifyNeighbours()
// (block/blocks.c) is yield-paced the same way (the torch support-drop / lever / plate / trapdoor
// route through it). SERVER_WORLD_NOTIFY_MAX_DEPTH mirrors the wire/blocks notify caps (real
// circuits nest 1-2, so 16 is never reached -> byte-exact; it only caps a pathological deep chain
// below the 64 KB stack).
static int s_notify_depth = 0;
static unsigned s_notify_yield_ctr = 0;
static uint32_t s_notify_cascade_max_us = 0;
static int s_notify_depth_max = 0;
static int s_notify_depth_cap_hits = 0;
static int s_notify_cascade_dispatches = 0;
static int s_notify_cascade_max_dispatches = 0;
static uint32_t s_notify_dispatch_total = 0; // monotonic; survives diag_reset
uint32_t server_world_notify_cascade_max_us(void) { return s_notify_cascade_max_us; }
uint32_t server_world_notify_dispatch_total(void) { return s_notify_dispatch_total; }
int server_world_notify_depth_max(void) { return s_notify_depth_max; }
int server_world_notify_depth_cap_hits(void) { return s_notify_depth_cap_hits; }
int server_world_notify_cascade_max_dispatches(void) { return s_notify_cascade_max_dispatches; }
void server_world_notify_diag_reset(void) {
	s_notify_depth_max = 0;
	s_notify_depth_cap_hits = 0;
	s_notify_cascade_dispatches = 0;
	s_notify_cascade_max_dispatches = 0;
	s_notify_cascade_max_us = 0;
}

// Beta onNeighborBlockChange's var5: the id of the block that CHANGED (the notify origin).
// CaveWii's handler signature carries no source param, so BOTH dispatch paths (this file and
// blocks.c notifyNeighbours) publish it here immediately before EACH handler call. A handler
// that needs it must read it AT ENTRY -- a nested set_block inside the handler re-publishes.
// Server thread only (both dispatchers run there). 0 = air/unknown.
static uint8_t s_notify_origin = 0;
uint8_t server_world_notify_origin(void) { return s_notify_origin; }
void server_world_notify_origin_set(uint8_t type) { s_notify_origin = type; }

static void server_world_notify_block(struct server_world* w, w_coord_t x,
		w_coord_t y, w_coord_t z, struct block_data blk,
		uint8_t notify_origin, struct server_chunk* sc) {
	if(server_world_simulation_stopped(w)) return;
	struct server_local* s = server_world_callback_host(w);
	const bool top = (s_notify_depth == 0); // time the WHOLE top-level cascade (F3 "ncas")
	const ptime_t t0 = time_get(); // cheap; only USED when top (ptime_t is a struct on PC)
	if(top)
		s_notify_cascade_dispatches = 0;

	if(sc) {
		server_world_publish(w, &(struct client_rpc) {
			.type = CRPC_SET_BLOCK,
			.payload.set_block.x = x,
			.payload.set_block.y = y,
			.payload.set_block.z = z,
			.payload.set_block.block = blk,
		});
		if(server_world_simulation_stopped(w)) return;
		// GAP #1: skip the CONSTANT-tick_delay auto-schedule for a block that defines
		// onNeighbourBlockChange -- it self-schedules its own tick (the redstone repeater,
		// whose delay is metadata-varying {2,4,6,8}, NOT a fixed tick_delay; a delay-1
		// auto-tick here would collapse the metadata delay -- the keep-first (pos,type) dedup
		// lets that earlier-inserted delay-1 entry win over the repeater's own metadata-delay
		// schedule). Blocks that set BOTH hooks (repeater, pressure plate, button) self-schedule
		// their own ticks (metadata delay / press-poll re-arm / press timer) -- this skip is what
		// makes that self-scheduling authoritative.
		const struct block* cb = blocks[blk.type];
		if(!cb || !cb->onNeighbourBlockChange)
			server_world_schedule_block_tick(w, x, y, z, blk.type);
	}

	/* Recovered Beta World.notifyBlocksOfNeighborChange order:
	   -X,+X,-Y,+Y,-Z,+Z. Keep this identical to blocks.c's explicit-origin path. */
	static const int dx[6] = {-1, 1, 0, 0, 0, 0};
	static const int dy[6] = {0, 0, -1, 1, 0, 0};
	static const int dz[6] = {0, 0, 0, 0, -1, 1};

	// Beta World.editingBlocks: the whole six-neighbour pass (auto-schedule AND
	// onNeighbourBlockChange dispatch) is suppressed while a multi-block structure
	// is being placed -- Beta gates World.notifyBlockOfNeighborChange on the flag,
	// and its notify path schedules nothing, so skipping the loop wholesale is the
	// faithful mapping. The CRPC + this cell's own GAP#1 schedule above still ran
	// (Beta's markBlockNeedsUpdate render update still happens under the flag).
	// [decompile 740c583 World.notifyBlockOfNeighborChange:516]
	if(w->editing_blocks)
		return;

	// Bound the synchronous recursion (handler -> set_block -> notify). Real circuits never nest
	// this deep; the cap only prevents a pathological chain from overflowing the 64 KB stack.
	if(s_notify_depth < SERVER_WORLD_NOTIFY_MAX_DEPTH) {
		s_notify_depth++;
		if(s_notify_depth > s_notify_depth_max)
			s_notify_depth_max = s_notify_depth;
		for(int i = 0; i < 6; i++) {
			if(server_world_simulation_stopped(w)) break;
			w_coord_t nx = x + dx[i];
			w_coord_t ny = y + dy[i];
			w_coord_t nz = z + dz[i];

			struct block_data nb;
			if(!server_world_get_block(w, nx, ny, nz, &nb))
				continue;

			const struct block* b = blocks[nb.type];
			// GAP #1 (neighbour-notify half): a self-scheduling block (defines
			// onNeighbourBlockChange, dispatched just below) must NOT also be auto-scheduled at
			// its constant tick_delay -- the repeater arms its own metadata-varying delay there.
			// Constant-delay components (torch tickRate=2, the fluids' flowing struct) have no
			// onNeighbourBlockChange and keep this auto-schedule unchanged.
			if(b && b->onWorldTick && !b->onNeighbourBlockChange)
				server_world_schedule_block_tick(w, nx, ny, nz, nb.type);

			server_world_callback_guard(w, b && b->onNeighbourBlockChange);
			if(s && b && b->onNeighbourBlockChange) {
				// Pace the cascade: every 32nd handler dispatch hand the single Broadway core
				// back to render so the main heartbeat advances. No-op on PC -> byte-exact.
				if((++s_notify_yield_ctr & 31u) == 0)
					thread_yield();
				s_notify_cascade_dispatches++;
				s_notify_dispatch_total++;
				struct block_info info = {
					.block      = &nb,
					.neighbours = NULL,
					.x          = nx,
					.y          = ny,
					.z          = nz
				};
				// Beta var5 = the block just SET at the notify origin (this cell).
				server_world_notify_origin_set(notify_origin);
				{ struct server_world* bw_prev = block_world_enter(s, w);
				b->onNeighbourBlockChange(s, &info);
				block_world_leave(s, bw_prev); }
			}
		}
		s_notify_depth--;
	} else {
		s_notify_depth_cap_hits++;
	}

	if(top) {
		uint32_t us = (uint32_t)(time_diff_s(t0, time_get()) * 1.0e6f);
		if(us > s_notify_cascade_max_us)
			s_notify_cascade_max_us = us;
		if(s_notify_cascade_dispatches > s_notify_cascade_max_dispatches)
			s_notify_cascade_max_dispatches = s_notify_cascade_dispatches;
	}
}

static bool server_world_set_block_history_impl(struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z, struct block_data blk, bool plain) {
	assert(w);
	struct server_local* s = server_world_callback_host(w);
	if(y < 0 || y >= WORLD_HEIGHT)
		return false;
	uint32_t previous_content = 0;
	if(column_light_enabled()) {
		struct server_chunk* before = server_world_resolve_chunk(w,
			WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
		if(before && before->backing) {
			thread_stripe_lock(before->backing->mutex);
			uint8_t old_type = before->ids[S_CHUNK_IDX(x, y, z)];
			/* Lifecycle callbacks can quietly mutate other cells without an
			 * ordered RPC. Only a callback-free edit certifies this chain. */
			if(blocks[old_type] && blocks[blk.type]
			   && !blocks[old_type]->onBlockRemoval && !blocks[blk.type]->onBlockAdded)
				previous_content = before->backing->revision.content;
			thread_stripe_unlock(before->backing->mutex);
		}
	}

	const uint8_t requested_type = blk.type;
	// P0 instrument: count every write; wall-time only the outermost edit so a
	// recursive cascade (fluid/redstone/falling sand via onNeighbourBlockChange)
	// is timed once, not N-deep. (See the s_setblock_* statics above.)
	s_setblock_count++;
	bool sb_top = (s_setblock_depth++ == 0);
	ptime_t sb_t0 = time_get();

	// Chunk.setBlockIDWithMetadata is a no-op for an identical id+metadata;
	// World.setBlockAndMetadataWithNotify returns false without lighting,
	// onBlockAdded, a client update, or neighbour notification.
	// Identity composition of the three phases: write -> single flood -> notify.
	bool id_or_metadata_changed = false;
	/* G9: reserve the pending-light bookkeeping BEFORE any mutation; a
	 * failed reservation stops this world with nothing written. */
	const bool light_resv = !s_light_sync_test;
	if(light_resv && !server_world_light_reserve(w, 1, 1)) {
		if(sb_top)
			s_setblock_accum_us += time_diff_s(sb_t0, time_get()) * 1.0e6f;
		s_setblock_depth--;
		return false;
	}
	struct server_chunk* sc = server_world_write_block_lifecycle(
		w, x, y, z, blk, &id_or_metadata_changed, true, plain);
	if(light_resv)
		server_world_light_release(w, 1, 1);
	if(!sc) {
		if(sb_top)
			s_setblock_accum_us += time_diff_s(sb_t0, time_get()) * 1.0e6f;
		s_setblock_depth--;
		return false;
	}

	/* Old synchronous flood: only the test equivalence baseline now. The
	 * live path queued this cell in finish_block_write (Beta schedules Sky
	 * and Block lighting, Chunk.java:230-232) for the tick drain. */
	if(s_light_sync_test)
		lighting_update_at_blocks_server(
			&(struct world_modification_entry) {
				.x = x,
				.y = y,
				.z = z,
				.blk = blk,
			}, 1,
			w->dimension == WORLD_DIM_NETHER, server_world_light_get_block,
			server_world_light_set_light, w);

	// Beta Chunk.setBlockID[WithMetadata] dispatches Block.onBlockAdded AFTER
	// the id/metadata write + lighting and BEFORE World.notifyBlockChange's
	// neighbour dispatch. Invoke the generic hook first; a hook may replace its
	// own cell synchronously (lava hardening), so re-read the live block before
	// the client/neighbor phase below.
	// [decompile 740c583 Chunk.setBlockID:275-302,
	//  Chunk.setBlockIDWithMetadata:217-258]
	server_world_callback_guard(w, id_or_metadata_changed && blocks[blk.type]
									  && blocks[blk.type]->onBlockAdded);
	server_world_callback_guard(w, blk.type == BLOCK_FIRE && !w->editing_blocks);
	if(s && id_or_metadata_changed && blocks[blk.type]
	   && blocks[blk.type]->onBlockAdded) {
		struct block_info info = {
			.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
		{ struct server_world* bw_prev = block_world_enter(s, w);
		blocks[blk.type]->onBlockAdded(s, &info);
		block_world_leave(s, bw_prev); }
		server_world_get_block(w, x, y, z, &blk);
	}

	// Preserve the existing fire portal hook and its editing_blocks guard. Fire
	// AGE bumps use the silent metadata path, so they never re-run onBlockAdded.
	// [decompile 740c583 BlockFire.onBlockAdded:192, BlockFire.updateTick:66]
	if(s && blk.type == BLOCK_FIRE && !w->editing_blocks) {
		block_fire_on_added(s, x, y, z);
		// The portal fill may have overwritten this very fire cell (fire is an
		// interior cell of the frame). Re-read so the notify below carries the
		// LIVE block -- Beta's notifyBlockChange reads current world state, and
		// a stale fire CRPC here would repaint the client cell back to fire.
		server_world_get_block(w, x, y, z, &blk);
	}

	if(column_light_enabled() && sc->backing)
		sc->backing->event_previous_content = previous_content;
	server_world_notify_block(w, x, y, z, blk, requested_type, sc);

	if(sb_top)
		s_setblock_accum_us += time_diff_s(sb_t0, time_get()) * 1.0e6f;
	s_setblock_depth--;
	return sc;
}

bool server_world_set_block(struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z, struct block_data blk) {
	uint64_t request = G9B_BEGIN(w, "setter-id-meta", x, z);
	G9B_ATTEMPT(w, x, y, z, blk.type, blk.metadata);
	bool ok = server_world_set_block_history_impl(w, x, y, z, blk, false);
	G9B_END(w, "setter-id-meta", request, ok ? "ok" : "rejected", x, z);
	return ok;
}


/* World.setBlockWithNotify -> Chunk.setBlockID (World.java:273-282,332-337).
 * Keep this API distinct from the id+metadata route above. */
bool server_world_set_block_plain(struct server_world* w, w_coord_t x,
	w_coord_t y, w_coord_t z, uint8_t type) {
	uint64_t request = G9B_BEGIN(w, "setter-id", x, z);
	G9B_ATTEMPT(w, x, y, z, type, 0);
	bool ok = server_world_set_block_history_impl(w, x, y, z,
		(struct block_data){.type = type}, true);
	G9B_END(w, "setter-id", request, ok ? "ok" : "rejected", x, z);
	return ok;
}

/* Quiet World.setBlockAndMetadata, plus the adapters' explicit client mark.
 * RetroMCP server World.java:192-205; Chunk.java:215-250. Unlike the raw
 * primitive below, EVERY non-identical ID+metadata call runs Chunk lighting
 * and removal/addition, including same-ID metadata changes. No World notify. */
static bool server_world_set_block_quiet_history_impl(struct server_world* w,
	w_coord_t x, w_coord_t y, w_coord_t z, struct block_data blk) {
	if(!w || y < 0 || y >= WORLD_HEIGHT || server_world_light_halted(w))
		return false;
	struct block_data old;
	if(!server_world_get_block(w, x, y, z, &old)
	   || (old.type == blk.type && old.metadata == blk.metadata))
		return false;
	/* At most eight requests of our own, reserved BEFORE the ID store.
	 * Callbacks reserve their own work. A failed reserve does not latch a
	 * stop by itself, so enter the existing lossless containment path. */
	if(!server_beta_light_reserve(w, 32)) {
		server_world_light_stop(w, SERVER_LIGHT_FAULT_RESERVE, 0);
		return false;
	}
	bool changed = false;
	struct server_chunk* sc = server_world_begin_block_write(
		w, x, y, z, blk, &old, &changed);
	if(!sc || !changed)
		return false;
	server_world_dispatch_block_removal(w, x, y, z, &old);
	if(server_world_light_halted(w))
		return false;
	server_world_finish_block_metadata(sc, x, y, z, blk);
	/* Chunk.java:230-244: immediate column work, Sky (outside Nether),
	 * Block, -X/+X/-Z/+Z height edges, then the second metadata store.
	 * Deliberately no gameplay BFS seed in addition to these ordered regions. */
	if(w->dimension != WORLD_DIM_NETHER)
		server_world_plain_column_write_impl(w, sc, x, y, z, blk.type, false);
	if(w->light_fault
	   || (w->dimension != WORLD_DIM_NETHER
		   && !server_beta_light_schedule(w, COLUMN_SKY, x,y,z,x,y,z))
	   || !server_beta_light_schedule(w, COLUMN_TORCH, x,y,z,x,y,z)
	   || !server_beta_light_height_edges(w, sc, x, z, sc)) {
		server_world_light_stop(w, SERVER_LIGHT_FAULT_RESERVE, 0);
		return false;
	}
	server_world_finish_block_metadata(sc, x, y, z, blk);
	struct server_local* s = server_world_callback_host(w);
	server_world_callback_guard(w, blocks[blk.type] && blocks[blk.type]->onBlockAdded);
	server_world_callback_guard(w, blk.type == BLOCK_FIRE && !w->editing_blocks);
	if(s && blocks[blk.type] && blocks[blk.type]->onBlockAdded) {
		struct block_info info = {
			.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
		struct server_world* previous = block_world_enter(s, w);
		blocks[blk.type]->onBlockAdded(s, &info);
		block_world_leave(s, previous);
		if(server_world_light_halted(w))
			return false;
		server_world_get_block(w, x, y, z, &blk);
	}
	if(s && blk.type == BLOCK_FIRE && !w->editing_blocks) {
		struct server_world* previous = block_world_enter(s, w);
		block_fire_on_added(s, x, y, z);
		block_world_leave(s, previous);
		if(server_world_light_halted(w))
			return false;
	}
	if(!server_world_get_block(w, x, y, z, &blk))
		return false;
	server_world_chunk_touch(sc);
	server_world_publish(w, &(struct client_rpc) {
		.type = CRPC_SET_BLOCK,
		.payload.set_block.x = x,
		.payload.set_block.y = y,
		.payload.set_block.z = z,
		.payload.set_block.block = blk,
	});
	return !server_world_light_halted(w);
}

bool server_world_set_block_quiet(struct server_world* w, w_coord_t x,
	w_coord_t y, w_coord_t z, struct block_data blk) {
	uint64_t request = G9B_BEGIN(w, "setter-id-meta", x, z);
	G9B_ATTEMPT(w, x, y, z, blk.type, blk.metadata);
	bool ok = server_world_set_block_quiet_history_impl(w, x, y, z, blk);
	G9B_END(w, "setter-id-meta", request, ok ? "ok" : "rejected", x, z);
	return ok;
}

// Raw write: set a block and tell the client, but WITHOUT the neighbour-notify cascade
// (no scheduled neighbour ticks, no neighbour onNeighbourBlockChange). The redstone wire flood
// uses this: it settles the whole connected wire network itself, so re-notifying wire
// neighbours per cell would re-flood redundantly, and it explicitly notifies the NON-wire
// neighbours (components) afterward. No lighting pass -- callers use it for metadata-only
// changes (wire strength) that do not alter block opacity.
static bool server_world_set_block_silent_history_impl(struct server_world* w, w_coord_t x, w_coord_t y,
								   w_coord_t z, struct block_data blk) {
	if(!w || y < 0 || y >= WORLD_HEIGHT)
		return false;
	struct server_chunk* sc
		= server_world_write_block_raw(w, x, y, z, blk, NULL);
	if(sc)
		server_world_publish(w, &(struct client_rpc) {
			.type = CRPC_SET_BLOCK,
			.payload.set_block.x = x,
			.payload.set_block.y = y,
			.payload.set_block.z = z,
			.payload.set_block.block = blk,
		});
	return sc != NULL;
}

bool server_world_set_block_silent(struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z, struct block_data blk) {
	uint64_t request = G9B_BEGIN(w, "raw-id-meta", x, z);
	G9B_ATTEMPT(w, x, y, z, blk.type, blk.metadata);
	bool ok = server_world_set_block_silent_history_impl(w, x, y, z, blk);
	G9B_END(w, "raw-id-meta", request, ok ? "ok" : "rejected", x, z);
	return ok;
}


static bool server_world_set_block_metadata_quiet_history_impl(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	uint8_t metadata) {
	if(!w || y < 0 || y >= WORLD_HEIGHT)
		return false;
	if(server_world_light_halted(w))
		return false;
	struct server_chunk* sc = server_world_resolve_chunk(
		w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
	if(!sc)
		return false;
	/* Chunk.setBlockMetadata is deliberately unconditional: it marks the chunk
	 * modified and returns through World.setBlockMetadata even when the nibble is
	 * already equal. That distinction is load-bearing for a piston's quiet latch
	 * followed by setBlockMetadataWithNotify of the same powered value. */
	server_world_chunk_touch(sc);
	server_chunk_store(sc, COLUMN_META, S_CHUNK_IDX(x, y, z), metadata & 0x0F);
	if(column_light_enabled() && sc->backing) {
		thread_stripe_lock(sc->backing->mutex);
		column_light_mutated(sc->backing, false); /* unconditional quiet ABA */
		thread_stripe_unlock(sc->backing->mutex);
	}
	return true;
}

bool server_world_set_block_metadata_quiet(struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t metadata) {
	uint64_t request = G9B_BEGIN(w, "setter-metadata", x, z);
	G9B_ATTEMPT(w, x, y, z, -1, metadata);
	bool ok = server_world_set_block_metadata_quiet_history_impl(w, x, y, z, metadata);
	G9B_END(w, "setter-metadata", request, ok ? "ok" : "rejected", x, z);
	return ok;
}


static bool server_world_set_block_metadata_with_notify_history_impl(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	uint8_t metadata) {
	if(!server_world_set_block_metadata_quiet(w, x, y, z, metadata))
		return false;
	struct server_chunk* sc = server_world_resolve_chunk(
		w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
	if(!sc)
		return false;
	struct block_data live;
	if(!server_world_get_block(w, x, y, z, &live))
		return false;
	server_world_notify_block(w, x, y, z, live, live.type, sc);
	return true;
}

bool server_world_set_block_metadata_with_notify(struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t metadata) {
	uint64_t request = G9B_BEGIN(w, "setter-metadata-notify", x, z);
	G9B_ATTEMPT(w, x, y, z, -1, metadata);
	bool ok = server_world_set_block_metadata_with_notify_history_impl(w, x, y, z, metadata);
	G9B_END(w, "setter-metadata-notify", request, ok ? "ok" : "rejected", x, z);
	return ok;
}


static bool server_world_prepare_piston_moving_rpc(
	struct client_rpc* rpc, w_coord_t x, w_coord_t y, w_coord_t z,
	const struct piston_moving_state* state) {
	if(!rpc)
		return false;
	*rpc = (struct client_rpc) {
		.type = CRPC_PISTON_MOVING,
		.payload.piston_moving.x = x,
		.payload.piston_moving.y = y,
		.payload.piston_moving.z = z,
	};
	return piston_moving_wire_encode(&rpc->payload.piston_moving.state, state);
}

bool server_world_send_piston_moving(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	const struct piston_moving_state* state) {
	if(!w)
		return false;
	struct client_rpc rpc;
	if(!server_world_prepare_piston_moving_rpc(&rpc, x, y, z, state))
		return false;
	server_world_publish(w, &rpc);
	return true;
}

bool server_world_set_piston_moving(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	const struct piston_moving_state* state) {
	if(!w || y < 0 || y >= WORLD_HEIGHT)
		return false;
	/* Validate before mutating the authoritative cell. */
	struct client_rpc rpc;
	if(!server_world_prepare_piston_moving_rpc(&rpc, x, y, z, state))
		return false;
	struct block_data block = {
		.type = BLOCK_PISTON_MOVING,
		.metadata = state->stored_meta,
	};
	const bool light_resv = !s_light_sync_test;
	if(light_resv && !server_world_light_reserve(w, 1, 1))
		return false;
	struct server_chunk* chunk = server_world_write_block_lifecycle(
		w, x, y, z, block, NULL, true, false);
	if(light_resv)
		server_world_light_release(w, 1, 1);
	if(!chunk)
		return false;
	/* Chunk.setBlockIDWithMetadata updates light even though piston placement
	 * deliberately suppresses scheduled ticks and neighbour callbacks: queued
	 * in finish_block_write (G9); synchronous only in the test baseline. */
	if(s_light_sync_test)
	lighting_update_at_block(
		(struct world_modification_entry) {
			.x = x,
			.y = y,
			.z = z,
			.blk = block,
		},
		w->dimension == WORLD_DIM_NETHER, server_world_light_get_block,
		server_world_light_set_light, w);
	server_world_publish(w, &rpc);
	return true;
}

bool server_world_furthest_chunk_anchors(struct server_world* w, w_coord_t dist,
										 w_coord_t pin_x, w_coord_t pin_z,
										 w_coord_t pin_r,
										 const struct residency_anchor* anchors,
										 size_t count, w_coord_t* x,
										 w_coord_t* z) {
	assert(w && x && z && anchors && count);
	bool scoped = clin_world_payloads_enabled();
	if(count == 1 && (!scoped || anchors[0].dimension == w->dimension))
		return server_world_furthest_chunk(w, dist, pin_x, pin_z, pin_r,
			anchors[0].x, anchors[0].z, x, z);
	w_coord_t px = anchors[0].x, pz = anchors[0].z;
	if(scoped)
		for(size_t i = 0; i < count; i++)
			if(anchors[i].dimension == w->dimension) {
				px = anchors[i].x;
				pz = anchors[i].z;
				break;
			}

	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);

	w_coord_t furthest_dist2 = -1;
	bool found = false;
	while(!dict_server_chunks_end_p(it)) {
		int64_t id = dict_server_chunks_ref(it)->key;
		w_coord_t d = CHUNK_DIST2(px, S_CHUNK_X(id), pz, S_CHUNK_Z(id));
		dict_server_chunks_next(it);

		/* Cheap rejections FIRST (Astra round 5, D; T1 -- the winner is
		 * identical): a column inside either keep radius, inside the pinned
		 * zone, or not farther than the current best can never be selected,
		 * so it must not pay the nine residency_find lookups of the
		 * dependency check below. Two anchors at 42/256 spent 130-140 ms/s
		 * in EVICT_NEAR with this scan running the lookups for every
		 * resident column. Same iteration order, same tie-break (strict >). */
		if(d <= furthest_dist2)
			continue;
		if(scoped ? residency_contains_in(anchors, count, dist, w->dimension,
					S_CHUNK_X(id), S_CHUNK_Z(id))
				  : residency_contains(anchors, count, dist, S_CHUNK_X(id),
					S_CHUNK_Z(id)))
			continue;
		// Pinned exclusion zone: an ensure_populated cascade far from the
		// player (save_migrate fixture/verify) makes incremental cross-tick
		// progress on Broadway; evicting its partial neighborhood each tick
		// livelocks the cascade (observed staged_save_migrate_20260824_193055:
		// 300s at FIXTURE, zero regions). Same doctrine as the loading
		// suspension above this caller -- but scoped to the cascade's
		// neighborhood so the rest of the world still drains.
		if(pin_r >= 0 && abs(pin_x - S_CHUNK_X(id)) <= pin_r
		   && abs(pin_z - S_CHUNK_Z(id)) <= pin_r)
			continue;

		/* Budgeted population must retain the same terrain/decoration state
		 * until its next attempt. Otherwise the near evictor saves/frees the
		 * radius-6 support every tick, then the far sender loads it again and
		 * spends its entire budget repeating the same unfinished step. */
		bool pending_support = false;
		for(unsigned stream = 0; stream < 2; stream++)
			if(w->gen_pending[stream]
			   && llabs((int64_t)w->gen_pending_x[stream] - S_CHUNK_X(id)) <= 1
			   && llabs((int64_t)w->gen_pending_z[stream] - S_CHUNK_Z(id)) <= 1)
				pending_support = true;
		if(pending_support
		   || (scoped ? server_local_residency_dependency_w(w->local, w,
							S_CHUNK_X(id), S_CHUNK_Z(id))
					  : server_local_residency_dependency(w->local,
							S_CHUNK_X(id), S_CHUNK_Z(id))))
			continue;

		*x = S_CHUNK_X(id);
		*z = S_CHUNK_Z(id);
		furthest_dist2 = d;
		found = true;
	}

	return found;
}

bool server_world_furthest_chunk(struct server_world* w, w_coord_t dist,
								 w_coord_t pin_x, w_coord_t pin_z,
								 w_coord_t pin_r, w_coord_t px, w_coord_t pz,
								 w_coord_t* x, w_coord_t* z) {
	assert(w && x && z);

	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);

	w_coord_t furthest_dist2 = -1;
	bool found = false;
	while(!dict_server_chunks_end_p(it)) {
		int64_t id = dict_server_chunks_ref(it)->key;
		w_coord_t d = CHUNK_DIST2(px, S_CHUNK_X(id), pz, S_CHUNK_Z(id));
		dict_server_chunks_next(it);

		/* Budgeted population must retain the same terrain/decoration state
		 * until its next attempt. Otherwise the near evictor saves/frees the
		 * radius-6 support every tick, then the far sender loads it again and
		 * spends its entire budget repeating the same unfinished step. */
		bool pending_support = false;
		for(unsigned stream = 0; stream < 2; stream++)
			if(w->gen_pending[stream]
			   && llabs((int64_t)w->gen_pending_x[stream] - S_CHUNK_X(id)) <= 1
			   && llabs((int64_t)w->gen_pending_z[stream] - S_CHUNK_Z(id)) <= 1)
				pending_support = true;
		if(pending_support)
			continue;

		// Pinned exclusion zone: an ensure_populated cascade far from the
		// player (save_migrate fixture/verify) makes incremental cross-tick
		// progress on Broadway; evicting its partial neighborhood each tick
		// livelocks the cascade (observed staged_save_migrate_20260824_193055:
		// 300s at FIXTURE, zero regions). Same doctrine as the loading
		// suspension above this caller -- but scoped to the cascade's
		// neighborhood so the rest of the world still drains.
		if(pin_r >= 0 && abs(pin_x - S_CHUNK_X(id)) <= pin_r
		   && abs(pin_z - S_CHUNK_Z(id)) <= pin_r)
			continue;

		if((abs(px - S_CHUNK_X(id)) > dist || abs(pz - S_CHUNK_Z(id)) > dist)
		   && d > furthest_dist2) {
			*x = S_CHUNK_X(id);
			*z = S_CHUNK_Z(id);
			furthest_dist2 = d;
			found = true;
		}
	}

	return found;
}

bool server_world_is_chunk_loaded(struct server_world* w, w_coord_t x,
								  w_coord_t z) {
	assert(w);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	s_sbl_dict_probes++;
#endif
	return dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z)) != NULL;
}

// --- Generation throttle knobs (see struct server_world). An ensure_populated
// cascade completes at least one terrain/populate unit, then SERVER_GEN_BUDGET_MS
// defers further work once that much wall time is spent. SERVER_GEN_MAX_COUNT is
// an unconditional hard backstop if the clock misbehaves. On real Wii hardware,
// watch the F3 "gen" line and widen the budget until the frontier keeps up without
// a render hitch. ---
#define SERVER_GEN_BUDGET_MS 14
#define SERVER_GEN_MAX_COUNT 6
#define SERVER_GEN_MAX_ATTEMPTS 6

#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
static enum server_world_test_gen_elapsed_mode s_test_gen_elapsed_mode
	= SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL;
static unsigned s_test_gen_failures_remaining = 0;
static unsigned s_test_fail_after_install = 0;

void server_world_test_fail_loads_after_install(unsigned count) {
	s_test_fail_after_install = count;
}

unsigned server_world_test_fail_loads_after_install_remaining(void) {
	return s_test_fail_after_install;
}

void server_world_test_set_gen_elapsed_mode(
	enum server_world_test_gen_elapsed_mode mode) {
	assert(mode >= SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL
		   && mode <= SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	s_test_gen_elapsed_mode = mode;
}

void server_world_test_fail_gen_attempts(unsigned count) {
	s_test_gen_failures_remaining = count;
}

unsigned server_world_test_gen_failures_remaining(void) {
	return s_test_gen_failures_remaining;
}
#endif

/* SD metadata reads can take longer than the whole nominal generation budget
 * on a real Wii. A wall-clock-only gate could therefore reject terrain after
 * the first slow stat/region probe forever. Let one indivisible disk/generation/
 * populate attempt finish, successful or not, before elapsed time may stop the
 * window. The separate attempt ceiling bounds repeated failure even if the
 * clock misbehaves; gen_this_window remains completed monotonic work only. */
static bool server_world_gen_budget_spent(struct server_world* w) {
	assert(w);
	if(w->gen_this_window >= SERVER_GEN_MAX_COUNT)
		return true;
	if(w->gen_attempts_window >= SERVER_GEN_MAX_ATTEMPTS)
		return true;
	if(w->gen_attempts_window == 0)
		return false;
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	if(s_test_gen_elapsed_mode == SERVER_WORLD_TEST_GEN_ELAPSED_FORCE)
		return true;
	if(s_test_gen_elapsed_mode == SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE)
		return false;
#endif
	return time_diff_ms(w->gen_window_start, time_get())
		>= (w->gen_budget_ms > 0 ? w->gen_budget_ms : SERVER_GEN_BUDGET_MS);
}

static bool server_world_gen_attempt_failed(struct server_world* w) {
	assert(w);
	if(w->gen_throttle_armed) {
		w->gen_attempts_window++;
		w->gen_deferred_window++;
	}
	return false;
}

// Last-window generation stats, surfaced to the F3 overlay (same-process server).
static int s_gen_last_ms = 0;
static int s_gen_last_generated = 0;
static int s_gen_last_deferred = 0;
// Relight flood time (us) accumulated across the quadrants of the last
// ensure_populated window -- the block-light flood (server_world_relight_quadrant_
// blocklight) PLUS the D1 horizontal skylight bleed (server_world_skylight_bleed) --
// surfaced to F3 so its Wii cost is visible, not hidden inside the gen ms. This is
// the dial to watch when reading the D1 skylight-occlusion cost on hardware.
static float s_light_accum_us = 0;

void server_world_gen_stats(int* ms, int* generated, int* deferred) {
	if(ms)
		*ms = s_gen_last_ms;
	if(generated)
		*generated = s_gen_last_generated;
	if(deferred)
		*deferred = s_gen_last_deferred;
}

// Relight flood cost (us) of the last cascade (block light + D1 skylight bleed),
// for the F3 overlay.
int server_world_light_flood_us(void) {
	return (int)s_light_accum_us;
}

// P0/stress perf counters surfaced to the overlay/log: cumulative explosion
// deferrals (the per-tick explosion cap pushing blasts to a later tick -- non-zero
// means the cap is saturated, the intended state under the chained-explosion
// stress), the live scheduled-tick queue depth (climbing toward its ring cap is a
// CORRECTNESS risk -- ticks would be dropped), and the resident server-chunk count
// (must stay bounded by the view window; growth under the load-center hop = the
// evictor falling behind, the historical far-travel crash path).
void server_world_perf_counters(struct server_world* w, int* explosion_deferrals,
								int* sched_ticks, int* resident_chunks) {
	if(explosion_deferrals)
		*explosion_deferrals = (int)w->explosion_deferrals;
	if(sched_ticks)
		*sched_ticks = (int)w->scheduled_tick_count;
	if(resident_chunks)
		*resident_chunks = (int)dict_server_chunks_size(w->chunks);
}

// P0 perf: latch the just-finished tick's edit/fetch totals into the "last"
// values the overlay reads, then zero the accumulators for the next tick. Called
// once at the top of server_local_update (so it captures the whole prior tick --
// RPC handling, entity tick, random tick, world tick -- regardless of where that
// tick returned). s_setblock_depth is intentionally NOT reset: it must return to
// 0 within each tick on its own, and forcing it would mask an unbalanced inc/dec.
void server_world_perf_tick_reset(void) {
	s_setblock_last_us = s_setblock_accum_us;
	s_setblock_last_n  = s_setblock_count;
	s_getblock_last_n  = s_getblock_count;
	if(s_setblock_accum_us > s_setblock_max_us)
		s_setblock_max_us = s_setblock_accum_us;
	if(s_getblock_count > s_getblock_max_n)
		s_getblock_max_n = s_getblock_count;
	s_setblock_accum_us = 0;
	s_setblock_count = 0;
	s_getblock_count = 0;
	s_chunkres_last_hits = s_chunkres_hits;
	s_chunkres_last_lookups = s_chunkres_lookups;
	s_chunkres_hits = 0;
	s_chunkres_lookups = 0;
}

// Tier-0 chunk-resolution cache hit rate this tick, in percent (0 when idle).
// MEASURES the cache (doctrine: no claimed speedup without an instrument) --
// surfaced on the F3 getblk row.
int server_world_chunkres_hit_pct(void) {
	return s_chunkres_last_lookups
		? (int)(100 * s_chunkres_last_hits / s_chunkres_last_lookups)
		: 0;
}

// P0 perf: last tick's set_block wall time (us) + write count and session-peak us,
// plus get_block fetch count + session peak, for the F3 overlay. Any out may be NULL.
void server_world_edit_stats(int* setblock_us, int* setblock_n, int* setblock_max_us,
							 int* getblock_n, int* getblock_max_n) {
	if(setblock_us)
		*setblock_us = (int)s_setblock_last_us;
	if(setblock_n)
		*setblock_n = s_setblock_last_n;
	if(setblock_max_us)
		*setblock_max_us = (int)s_setblock_max_us;
	if(getblock_n)
		*getblock_n = s_getblock_last_n;
	if(getblock_max_n)
		*getblock_max_n = s_getblock_max_n;
}

// Process-lifetime count of the stat(2) calls this function actually issues.
// T1 measurement instrument (2026-09-01 region-probe-order fix, see
// server_world_load_chunk): mirrors region_archive.c's s_region_fopen_count/
// region_archive_io_stats idiom -- a plain always-compiled counter (no test
// gate) read via server_world_region_stat_count(), proving the LRU-first
// probe actually skips the libfat directory walk on a resident region
// instead of merely asserting it by inspection.
static uint32_t s_region_stat_calls = 0;

uint32_t server_world_region_stat_count(void) {
	return s_region_stat_calls;
}

static bool server_world_region_file_status(const struct server_world* w,
										 w_coord_t x, w_coord_t z,
										 bool* exists) {
	assert(w && exists);
#ifdef PLATFORM_WII
	save_path_set_world(string_get_cstr(w->level_name));
#endif
	string_t path;
	if(w->dimension == WORLD_DIM_OVERWORLD)
		string_init_printf(path, "%s/region/r.%i.%i.mcr",
						   string_get_cstr(w->level_name), CHUNK_REGION_COORD(x),
						   CHUNK_REGION_COORD(z));
	else
		string_init_printf(path, "%s/DIM-1/region/r.%i.%i.mcr",
						   string_get_cstr(w->level_name), CHUNK_REGION_COORD(x),
						   CHUNK_REGION_COORD(z));

	struct stat info;
#ifdef PLATFORM_WII
	bool profile = region_archive_work_profile_enabled();
	ptime_t start = profile ? time_get() : 0;
#endif
	errno = 0;
	s_region_stat_calls++;
	int result = stat(string_get_cstr(path), &info);
	int error = errno;
#ifdef PLATFORM_WII
	if(profile) {
		uint32_t elapsed_us = (uint32_t)(time_diff_s(start, time_get()) * 1e6f);
		region_archive_path_profile_note(elapsed_us);
		region_archive_path_profile_emit("region_stat", elapsed_us);
	}
	errno = error;
#endif
	string_clear(path);
	if(result == 0) {
		*exists = true;
		return true;
	}
	*exists = false;
	return error == ENOENT;
}

// True if region (x,z)'s owning region is already resident in the LRU --
// a pure in-memory offset-table check (region_archive_contains does no I/O),
// so callers can skip server_world_region_file_status's stat(2) entirely
// when the handle is already open. See server_world_load_chunk.
static bool server_world_region_resident(struct server_world* w, w_coord_t x,
										 w_coord_t z) {
	for(size_t k = 0; k < server_world_region_count(w); k++) {
		bool chunk_exists;
		if(region_archive_contains(server_world_region_at(w, k), x, z,
								   &chunk_exists))
			return true;
	}
	return false;
}

/* File status was checked immediately before admission. Keep the pool's
 * absent-before-eviction contract without repeating its full-path stat. */
static struct region_archive* server_world_region_probed(struct server_world* w,
	w_coord_t x, w_coord_t z, bool exists) {
	struct region_archive* ra = NULL;
	enum storage_rc rc = region_pool_acquire_probed(server_world_region_pool(w),
		w, CHUNK_REGION_COORD(x), CHUNK_REGION_COORD(z), exists, &ra);
	return rc == STORAGE_OK ? ra : NULL;
}

static struct {
	unsigned n;
	uint8_t world_index;
	uint32_t generation;
	int32_t cx[4], cz[4];
	unsigned blocked;
} s_rz_fault;
void server_world_rz_fault_set(uint8_t world_index, uint32_t generation,
	const int32_t* cx, const int32_t* cz, unsigned n) {
	if(n > 4)
		n = 4;
	s_rz_fault.world_index = world_index;
	s_rz_fault.generation = generation;
	for(unsigned i = 0; i < n; i++) {
		s_rz_fault.cx[i] = cx[i];
		s_rz_fault.cz[i] = cz[i];
	}
	s_rz_fault.blocked = 0;
	s_rz_fault.n = n;
}
void server_world_rz_fault_clear(void) { s_rz_fault.n = 0; }
unsigned server_world_rz_fault_blocked(void) { return s_rz_fault.blocked; }
static bool server_world_rz_fault_blocks(const struct server_world* w,
	w_coord_t x, w_coord_t z) {
	if(!s_rz_fault.n || w->world_index != s_rz_fault.world_index
	   || w->generation != s_rz_fault.generation)
		return false;
	for(unsigned i = 0; i < s_rz_fault.n; i++)
		if(s_rz_fault.cx[i] == x && s_rz_fault.cz[i] == z)
			return true;
	return false;
}

static void server_world_population_storage_stop(struct server_world* w,
	w_coord_t x, w_coord_t z, uint8_t site);
static bool server_world_load_chunk_impl(struct server_world* w, w_coord_t x, w_coord_t z,
	struct server_chunk** sc, bool structural_fixture) {
	assert(w && sc);
	*sc = NULL;

	if(server_world_is_chunk_loaded(w, x, z))
		return false;
	/* A prior successful OR failed attempt owns the elapsed-time waiver. Check
	 * before paying another SD probe; the first attempt still runs through its
	 * disk miss and terrain generation as one indivisible unit. */
	if(w->gen_throttle_armed && server_world_gen_budget_spent(w)) {
		w->gen_deferred_window++;
		return false;
	}
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	if(w->gen_throttle_armed && s_test_gen_failures_remaining > 0) {
		s_test_gen_failures_remaining--;
		return server_world_gen_attempt_failed(w);
	}
#endif
	if(server_world_rz_fault_blocks(w, x, z)) {
		s_rz_fault.blocked++;
		return server_world_gen_attempt_failed(w);
	}

	// First try to load the chunk from disk -- through server_world_chunk_region,
	// which opens the OWNING region file on demand (Beta's RegionFileCache
	// behavior). A path check below distinguishes a missing file from a corrupt
	// or temporarily unreadable existing file before interpreting NULL. The old
	// code here scanned only the ALREADY-OPEN LRU entries: a saved chunk whose
	// region had not been opened by some earlier disk_has_chunk probe silently
	// fell through to regeneration, and the evictor then OVERWROTE the real
	// .mcr with the regenerated terrain (frontier/region-boundary save
	// corruption, 2026-06-12 persistence audit -- regression-pinned by
	// test_region_boundary).
	//
	// T1 (2026-09-01 region-probe-order fix): the path check above is a
	// stat(2) -- a libfat directory walk on the Wii -- and used to run
	// UNCONDITIONALLY, even when the owning region is already open in the
	// 4-entry LRU (server_world_chunk_region's own first move is to scan
	// that LRU with region_archive_contains, a pure in-memory check). Every
	// disk load of a chunk whose neighbour in the same region was loaded
	// moments earlier paid a redundant stat it didn't need. Probe the LRU
	// FIRST (server_world_region_resident, same in-memory check, zero I/O)
	// and only fall through to the stat on an actual miss. Identical load
	// results either way: a resident region is, by construction, an
	// existing region file that opened cleanly, so region_file_exists=true
	// and server_world_chunk_region below just returns the cached handle
	// without reopening it -- see test_region_load_stat_lru.c (byte-equal
	// output plus a server_world_region_stat_count() pin proving the second
	// load of a same-region neighbour issues zero additional stats).
	{
		// WDS_REGION_IO (2026-09-08 wedge instrumentation lane): brackets the
		// disk-touching span (stat/open + region_archive_get_chunk fread) --
		// the one real blocking I/O reachable from either WDS_ENSURE_POPULATED
		// (near) or WDS_FAR_POPULATE (far, server_local.c). Restored to the
		// CALLER's phase at the two points that matter (falling through to
		// generation below, and a successful disk hit's `return true`).
		// KNOWN GAP, documented not silent: the handful of rare mid-block
		// failure returns (corrupt region / OOM / stale piston sidecar) leave
		// the tag at WDS_REGION_IO instead of restoring immediately -- each
		// returns promptly right after, so this can only mislabel an
		// already-fast error exit for at most one loop iteration, never hide
		// an actual stall.
		uint32_t wd_prev_phase = watchdog_phase_srv_current();
		watchdog_phase_srv(WDS_REGION_IO);
		bool resident = server_world_region_resident(w, x, z);
		bool region_file_exists = resident;
		if(!resident && !server_world_region_file_status(w, x, z, &region_file_exists)) {
			fprintf(stderr, "[load %d,%d] region_file_status FAILED\n", (int)x,
					(int)z);
			return server_world_gen_attempt_failed(w);
		}
		struct region_archive* ra = server_world_region_probed(w, x, z, region_file_exists);
		if(!ra && region_file_exists) {
			fprintf(stderr, "[load %d,%d] region OPEN failed on an EXISTING file\n",
					(int)x, (int)z);
			return server_world_gen_attempt_failed(w); /* never regenerate corrupt data */
		}

		struct server_chunk tmp;
		server_chunk_init(&tmp); // .modified stays false, like every other field
		struct chunk_tile_entities te = {0};
		struct chunk_entities entities = {0};
		struct chunk_tile_entities* te_sink = w->local ? &te : NULL;
		struct chunk_entities* entity_sink = w->local ? &entities : NULL;

		/* S2: BEFORE asking the index -- a FAILED first-ever record rolled
		 * the index entry back to "absent", and the retained bytes must win
		 * over a fresh regeneration. */
		struct server_world_retiring* retiring_entry = NULL;
		enum server_world_retiring_load retiring
			= server_world_retiring_consume_for_load(w, ra, x, z,
													 &retiring_entry);
		if(retiring == SERVER_WORLD_RETIRING_LOAD_BLOCKED)
			return server_world_gen_attempt_failed(w);
		bool from_retiring = retiring == SERVER_WORLD_RETIRING_LOAD_CONSUMED;
		bool chunk_exists = false;
		if(ra && !region_archive_contains(ra, x, z, &chunk_exists))
			return server_world_gen_attempt_failed(w);
		assert(!(from_retiring && !chunk_exists)
			   && "a consumed retained record is readable through the index");
		if(ra && chunk_exists) {
			if(!region_archive_get_chunk(ra, x, z, &tmp, te_sink, entity_sink))
				return server_world_gen_attempt_failed(w); /* disk remains authoritative */
			/* Transfer the pending job without rewriting its bytes. Beta
			 * ChunkLoader.storeChunkInCompound stamps LastUpdate from world
			 * time; re-dirtying this unchanged reload would restamp it. The
			 * retiring entry survives until the complete restore succeeds. */
			if(from_retiring) {
				tmp.save_job = retiring_entry->save_job;
				tmp.save_pending = true;
			}
			/* Stored light is kept as saved, Nether included: Beta writes
			 * Nether skylight (Chunk.java:242-276 -> func_339_g has no Nether
			 * guard) and reloads it unchanged (ChunkLoader.java:163-185).
			 * Astra ruling 2026-10-12 (option A) retired the 07-09 blanket
			 * wipe; it destroyed imported Beta Nether light. */
			if(w->local) {
				size_t stale_pistons = 0;
				if(!te_store_filter_piston_hosts(
					   &te, x, z, tmp.ids, &stale_pistons)) {
					tile_entities_parse_free(&te);
					chunk_entities_free(&entities);
					server_world_chunk_destroy(&tmp);
					return server_world_gen_attempt_failed(w);
				}
				if(stale_pistons != 0)
					fprintf(stderr,
							"[chunk] discarded %zu stale Piston sidecar(s) in (%d,%d)\n",
							stale_pistons, (int)x, (int)z);
			}

			struct te_store st;
			if(w->local) {
				st = server_world_tile_store(w);
				bool te_ready = tmp.persisted_tile_entities_complete
					? te_store_can_replace_legacy_chunk(&st, &te, x, z)
					: te_store_can_insert(&st, &te);
				if(!te_ready) {
					tile_entities_parse_free(&te);
					chunk_entities_free(&entities);
					server_world_chunk_destroy(&tmp);
					return server_world_gen_attempt_failed(w);
				}
			}

			if(!server_world_adopt_backing(w, &tmp, x, z)) {
				tile_entities_parse_free(&te);
				chunk_entities_free(&entities);
				server_world_chunk_destroy(&tmp);
				return server_world_gen_attempt_failed(w);
			}

			// region_archive_get_blocks restored tmp.populated from the
			// saved TerrainPopulated flag (false for a chunk that has only
			// received neighbour decoration spill -- it still owes its own
			// populate(), and re-running it on reload is correct).
			tmp.sent = false;
			tmp.load_ticks_rearmed = false;
			tmp.load_tick_rearm_cursor = 0;
			// Insert can rehash the dict and move EVERY resident chunk ->
			// the resolution cache's pointer dangles. Drop it first.
			server_world_chunk_cache_invalidate(w);
			dict_server_chunks_set_at(w->chunks, S_CHUNK_ID(x, z), tmp);
			tps_mutated(3);
			*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
			G9B_NOTE(w, "residency", "install", "ok", x, z, *sc);
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
			/* Test seam (Astra consult 2026-09-10, contract §4): fail the load
			 * AFTER the chunk is installed but before it is complete -- the
			 * shape of a failed server_local_restore_chunk_entities below. */
			if(s_test_fail_after_install) {
				s_test_fail_after_install--;
				tile_entities_parse_free(&te);
				chunk_entities_free(&entities);
				server_world_chunk_destroy(*sc);
				server_world_chunk_cache_invalidate(w);
				dict_server_chunks_erase(w->chunks, S_CHUNK_ID(x, z));
				G9B_NOTE(w, "residency", "remove", "ok", x, z, NULL);
				*sc = NULL;
				return server_world_gen_attempt_failed(w);
			}
#endif
			// Move this chunk's saved chests/signs into the resident tables. A
			// CaveWii-marked full image replaces every flat row in this chunk;
			// old/unmarked chunks retain overwrite-by-position migration semantics.
			if(w->local) {
				if(!server_local_restore_chunk_entities_w(w->local, w, &entities)) {
					tile_entities_parse_free(&te);
					chunk_entities_free(&entities);
					server_world_chunk_destroy(*sc);
					server_world_chunk_cache_invalidate(w);
					dict_server_chunks_erase(w->chunks, S_CHUNK_ID(x, z));
				G9B_NOTE(w, "residency", "remove", "ok", x, z, NULL);
					*sc = NULL;
					return server_world_gen_attempt_failed(w);
				}
				/* IMP-TE-CAPS: the preflight may have grown the shared tile
				 * slabs (and the entity restore above may build its own
				 * views); rebuild this one so it points at the live slabs. */
				st = server_world_tile_store(w);
				if((*sc)->persisted_tile_entities_complete)
					te_store_release_legacy_chunk(&st, x, z);
				/* The single-threaded preflight above guarantees this commit. */
				bool inserted = te_store_insert(&st, &te);
				assert(inserted);
				if(!inserted)
					abort(); /* internal preflight/commit contract violation */
				// Beta readFromNBT recomputes currentItemBurnTime from the fuel
				// slot, and the block id must agree with the restored burn state;
				// do both for this chunk's just-inserted furnaces.
				furnace_server_load_fixup_w(w, x, z);
				// dispenserRandom is never persisted (Beta `new Random()` on every
				// TileEntityDispenser construction) -- reseed every dispenser this
				// chunk just inserted so it isn't left zero-locked at {0}.
				dispenser_server_load_fixup_w(w, x, z);
				tile_entities_parse_free(&te);
				chunk_entities_free(&entities);
			}
			/* S2 (Astra consult 2026-09-10, contract §4): the retained record
			 * is released only now, on the SUCCESS exit -- a failed entity
			 * restore above destroys the installed chunk, and the retained
			 * record must outlive that failure as the newest state's only
			 * retry source. */
			if(retiring_entry)
				server_world_retiring_release(w, retiring_entry);
			server_world_refresh_chunk_tick_rearms(w, x, z);
			if(w->gen_throttle_armed)
				w->gen_attempts_window++;
			watchdog_phase_srv((enum wd_phase_srv)wd_prev_phase);
			return true;
		}
		// A missing region file or a valid region with no indexed chunk is fresh
		// terrain and may fall through to generation. Restore the caller's phase:
		// the I/O span (stat + open + the contains() probe) is over either way.
		watchdog_phase_srv((enum wd_phase_srv)wd_prev_phase);
	}

	// Not on disk -> generate it with the Beta terrain generator. Freshly
	// generated chunks are marked modified so they persist on unload, after
	// which subsequent visits load the identical terrain straight from disk.
	if(w->generator) {
		/* G9X-5a: a world stopped by a population backing-storage failure
		 * generates no further terrain (no RNG draw, no new request). */
		if(w->gen_storage_failed)
			return false;
		// Generation throttle: once this cascade's time budget (or the count
		// backstop) is spent, defer NEW terrain generation to a later cycle --
		// the populate quadrant then aborts and retries. The target chunk and a
		// few neighbours still generate each cycle, so a frontier crossing loads
		// smoothly instead of as one multi-chunk render hitch. Only armed during
		// server_world_ensure_populated; every other caller generates normally.
		if(w->gen_throttle_armed && server_world_gen_budget_spent(w)) {
			w->gen_deferred_window++;
			return false;
		}

		if(w->gen_throttle_armed)
			w->gen_attempts_window++;
		struct server_chunk tmp;
		// Explicit at THIS call site rather than relying solely on
		// worldgen_generate_chunk's own internal memset(0) -- see
		// server_chunk_init's doc comment. Cheap (one ~40-byte memset) and
		// makes this site's zero-init guarantee self-contained even if the
		// callee's implementation ever changes.
		server_chunk_init(&tmp);
		tps_note(world_tps_observe, TPS_ENTER, TPS_TERRAIN);
		ptime_t terrain_start = deep_start();
		uint64_t history = G9B_BEGIN(w, "generate", x, z);
		struct java_random rng_before = w->hell_populate_rng;
		bool generated = worldgen_generate_chunk_provider(w->generator, &tmp,
			x, z, structural_fixture ? NULL : &w->hell_populate_rng);
		G9B_NOTE(w, "generated-arrays", "snapshot", generated ? "ok" : "failed", x, z, generated ? &tmp : NULL);
		G9B_END(w, "generate", history, generated ? "ok" : "failed", x, z);
		deep_end(1, terrain_start);
		tps_note(world_tps_observe, TPS_LEAVE, TPS_TERRAIN);
		if(generated) {
			/* ChunkProviderGenerate.java:234 / Chunk.java:90-116: height
			 * edges schedule BEFORE the new chunk is installed in the provider. */
			if(w->dimension == WORLD_DIM_OVERWORLD && !structural_fixture) {
				const size_t io_before = w->generation_light_io_fails;
				bool queued = server_beta_light_reserve(w, 1024);
				for(int lx = 0; lx < 16 && queued; lx++)
					for(int lz = 0; lz < 16 && queued; lz++)
						queued = server_beta_light_height_edges(w, &tmp, x*16+lx, z*16+lz, NULL);
				if(!queued) {
					server_world_chunk_destroy(&tmp);
					/* G9X-5a: a backing I/O failure here is the sticky
					 * population storage stop, latched after the discard
					 * (the chunk was never installed), whoever loads it;
					 * any other refusal keeps the reserve stop. */
					if(w->generation_light_io_fails != io_before) {
						w->hell_populate_rng = rng_before;
						server_world_population_storage_stop(w, x, z,
							SERVER_POPULATION_SITE_TERRAIN);
						if(w->gen_throttle_armed) w->gen_deferred_window++;
						return false;
					}
					server_world_stop_simulation(w, SERVER_LIGHT_FAULT_RESERVE);
					return false;
				}
			}
			if(!server_world_adopt_backing(w, &tmp, x, z)) {
				w->hell_populate_rng = rng_before;
				server_world_chunk_destroy(&tmp);
				if(w->gen_throttle_armed) w->gen_deferred_window++;
				return false;
			}
			// Terrain is generated but NOT yet decorated; populate() runs once
			// this chunk's 2x2 neighbour quadrant exists (server_world_ensure_
			// populated). Not yet sent to the client either.
			tmp.populated = false;
			tmp.sent = false;
			tmp.load_ticks_rearmed = false;
			tmp.load_tick_rearm_cursor = 0;
			// Insert can rehash -> invalidate the resolution cache (see above).
			server_world_chunk_cache_invalidate(w);
			dict_server_chunks_set_at(w->chunks, S_CHUNK_ID(x, z), tmp);
			tps_mutated(3);
			*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
			G9B_NOTE(w, "residency", "install", "ok", x, z, *sc);
			server_world_refresh_chunk_tick_rearms(w, x, z);
			w->gen_this_window++;
			return true;
		}
		if(w->gen_throttle_armed)
			w->gen_deferred_window++;
		return false;
	}

	return server_world_gen_attempt_failed(w);
}

static bool server_world_load_chunk_history_impl(struct server_world* w, w_coord_t x, w_coord_t z,
	struct server_chunk** sc, bool structural_fixture) {
	bool ephemeral = false;
	if(!server_transfer_column_load_begin(w, x, z, &ephemeral)) {
		*sc = NULL;
		return false;
	}
	tps_note(world_tps_observe, TPS_ENTER, TPS_LOAD);
	ptime_t start = deep_start();
	bool result = server_world_load_chunk_impl(w, x, z, sc, structural_fixture);
	server_transfer_column_load_end(w, x, z, ephemeral);
	if(result && server_column_observe_chunk)
		server_column_observe_chunk(w, x, z, *sc, true);
	deep_end(0, start);
	tps_note(world_tps_observe, TPS_LEAVE, TPS_LOAD);
	return result;
}

static bool server_world_populate_quadrant(struct server_world* w,
	struct server_local* s, w_coord_t x, w_coord_t z);

#ifdef CAVEWII_SERVER_LOCAL_TEST
bool server_world_test_load_structural_chunk(struct server_world* w,
	w_coord_t x, w_coord_t z, struct server_chunk** sc) {
	/* R5/R6 declared historical reconstruction fixture: preserve its old
	 * pre-population RNG as well as terrain arrays. Normal loads never enter. */
	if(dict_server_chunks_size(w->chunks) == 0)
		jrand_set_seed(&w->hell_populate_rng, w->seed);
	uint64_t request = G9B_BEGIN(w, "structural-fixture-load", x, z);
	bool ok = server_world_load_chunk_history_impl(w, x, z, sc, true);
	G9B_END(w, "structural-fixture-load", request, ok ? "ok" : "deferred", x, z);
	return ok;
}
#endif

/* CFR server ChunkProviderServer.java:67-95: install first, then populate
 * eligible resident quadrants in current, west, north, northwest order.
 * No neighbour is generated merely to satisfy an automatic population. */
static void server_world_populate_loaded_neighbours(struct server_world* w,
	w_coord_t x, w_coord_t z) {
	static const int offsets[4][2] = {{0, 0}, {-1, 0}, {0, -1}, {-1, -1}};
	if(!w->generator
	   || (w->dimension == WORLD_DIM_NETHER && w->light_gen_hold))
		return;
	/* G9X-5a: once the sticky population storage stop latches (in an
	 * earlier neighbour's population), no further neighbour is started. */
	for(unsigned i = 0; i < 4 && !w->gen_storage_failed; i++) {
		w_coord_t px = x + offsets[i][0], pz = z + offsets[i][1];
		struct server_chunk* origin = dict_server_chunks_get(w->chunks, S_CHUNK_ID(px, pz));
		if(!origin || origin->populated)
			continue;
		bool ready = true;
		for(int dz = 0; dz < 2; dz++)
			for(int dx = 0; dx < 2; dx++)
				ready &= server_world_is_chunk_loaded(w, px + dx, pz + dz)
					&& !server_world_light_pinned(w, px + dx, pz + dz);
		/* Retain lossless deferred-light/fault handling: ensure_populated's
		 * retry owns a quadrant whose pending edit prevents generation. */
		if(ready)
			server_world_populate_quadrant(w, w->local, px, pz);
	}
}

bool server_world_load_chunk(struct server_world* w, w_coord_t x, w_coord_t z, struct server_chunk** sc) {
	uint64_t request = G9B_BEGIN(w, "load", x, z);
	bool ok = server_world_load_chunk_history_impl(w, x, z, sc, false);
	if(ok) {
		server_world_populate_loaded_neighbours(w, x, z);
		/* Population's loader may rehash the resident map. */
		*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
	}
	G9B_END(w, "load", request, ok ? "ok" : "deferred", x, z);
	return ok;
}


// ---------------------------------------------------------------------------
// Beta 1.7.3 populate (decorate) phase: the cross-chunk lifecycle.
//
// populate(P) writes across P's 2x2 chunk quadrant -- the +8 placement offset
// deliberately spills ore veins / trees into the +X/+Z neighbours -- so it runs
// against a wgen_world backed by the four loaded chunks of the quadrant. These
// are RAW array writes; routing them through server_world_set_block would emit
// per-block CRPC_SET_BLOCK, run lighting + scheduled ticks, and corrupt timing
// for chunks the client has not even received yet.
// ---------------------------------------------------------------------------

struct quad_fluid_driver;
struct quad_populate_tx;

struct quad_ctx {
	w_coord_t px, pz;             // quadrant origin (chunk coords)
	struct server_chunk* c[2][2]; // c[dx][dz] = chunk (px+dx, pz+dz)
	struct server_local* sl;      // owning local server (chest store)
	struct server_world* pop_world; // A1b: the world this quadrant populates
	struct quad_populate_tx* tx;  // all-or-nothing raw decorate transaction
	struct quad_fluid_driver* fluid_driver; // heap-backed immediate trampoline
	struct java_random* fluid_active_rng;   // root update only; children use World.rand
	int fluid_editing_depth;                // Beta World.editingBlocks nesting
	struct wgen_world* view; // this quad's own wgen_world (wgen_leaves_on_removal)
};

struct quad_populate_chest {
	int x, y, z;
	struct item_data items[DUNGEON_CHEST_SLOTS];
};

// FH-17: a dungeon spawner staged during populate, mirroring quad_populate_
// chest above. mob_id is the small fixed Beta EntityId string (never a
// DUNGEON_CHEST_SLOTS-sized payload), so staging cost is negligible.
struct quad_populate_spawner {
	int x, y, z;
	char mob_id[SPAWNER_MOB_ID_MAX];
};

/* A populate retry must start from the exact four-chunk/RNG/tick state that
 * preceded the decorate call. Blocks/metadata use the fixed world snapshot;
 * side effects which need not be visible to later features are staged. */
struct quad_populate_extra {
	struct quad_populate_extra* next;
	struct server_chunk* chunk;
	uint8_t ids[32768], metadata[16384], sky[16384], height[256];
	/* G9X-3: dirty/durability flags before the first write, restored by a
	 * rollback together with the bytes. */
	bool modified, save_pending;
};

struct quad_populate_tx {
	struct quad_ctx* q;
	struct server_world* world;
	struct server_local* sl;
	bool active;
	bool failed;
	struct quad_populate_chest* chests;
	size_t chest_count;
	size_t chest_capacity;
	struct quad_populate_spawner* spawners;
	size_t spawner_count;
	size_t spawner_capacity;
	struct server_local_planned_drop* drops;
	size_t drop_count;
	size_t drop_capacity;
	struct random_gen world_rng_before;
	struct java_random hell_rng_before;
	/* F4-RNG: the populating world's World.rand (planned fluid drops). */
	struct java_random world_jrand_before;
	struct server_local_math_random_state math_rng_before;
	size_t scheduled_head_before;
	size_t scheduled_count_before;
	uint32_t scheduled_drops_before;
	uint32_t lava_mix_effects;
	size_t write_count;
	struct quad_populate_extra* extra;
	/* G9X-3: the quadrant's dirty/durability flags at begin; every write
	 * marks its chunk modified, so a rollback must restore these too. */
	bool quad_modified_before[2][2], quad_save_pending_before[2][2];
	size_t light_regions_before;
	struct server_beta_light_region light_tail_before[5];
	size_t light_tail_count;
	/* G9X-4: a population region reservation that fails inside func_339_g
	 * (server_world_light_column_write_impl) latches light_fault; the
	 * rollback undoes that request with the queue, so it restores these. */
	bool light_fault_before;
	uint8_t light_fault_reason_before;
};

static struct server_chunk* quad_chunk(struct quad_ctx* q, int wx, int wz) {
	int dx = WCOORD_CHUNK_OFFSET(wx) - q->px;
	int dz = WCOORD_CHUNK_OFFSET(wz) - q->pz;
	/* CFR World.java:getBlockId / ChunkProviderServer.java:108-114:
	 * the guaranteed quadrant does not exclude other already resident chunks. */
	if(dx < 0 || dx > 1 || dz < 0 || dz > 1)
		return server_world_resolve_chunk(q->pop_world, WCOORD_CHUNK_OFFSET(wx), WCOORD_CHUNK_OFFSET(wz));
	return q->c[dx][dz];
}

static uint8_t quad_get(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= WORLD_HEIGHT)
		return 0; // BLOCK_AIR out of world
	struct server_chunk* sc = quad_chunk((struct quad_ctx*)ctx, x, z);
	return sc ? sc->ids[S_CHUNK_IDX(x, y, z)] : 0;
}

static void quad_populate_tx_release(struct quad_populate_tx* tx) {
	if(!tx)
		return;
	assert(tx->world && tx->world->populate_scratch_storage
		   && tx->world->populate_scratch_storage->in_use);
	tx->world->populate_scratch_storage->in_use = false;
	while(tx->extra) {
		struct quad_populate_extra* next = tx->extra->next;
		mem2_free_sized(tx->extra, sizeof(*tx->extra));
		tx->extra = next;
	}
	mem2_free_sized(
		tx->chests, tx->chest_capacity * sizeof(*tx->chests));
	mem2_free_sized(
		tx->spawners, tx->spawner_capacity * sizeof(*tx->spawners));
	mem2_free_sized(
		tx->drops, tx->drop_capacity * sizeof(*tx->drops));
	tx->chests = NULL;
	tx->spawners = NULL;
	tx->drops = NULL;
	tx->chest_count = tx->chest_capacity = 0;
	tx->spawner_count = tx->spawner_capacity = 0;
	tx->drop_count = tx->drop_capacity = 0;
}

static void quad_populate_tx_fail(struct quad_populate_tx* tx) {
	if(tx)
		tx->failed = true;
}

static bool quad_populate_tx_begin(
	struct quad_populate_tx* tx, struct quad_ctx* q, struct server_world* world) {
	assert(tx && q && world);
	/* G9X-5: the mergeable tail must be resident to be captured. A failed
	 * reload returns before anything is touched; the caller classifies it
	 * (backing I/O = the sticky population stop, G9X-5a). */
	if(!sbl_ensure_tail(world))
		return false;
	memset(tx, 0, sizeof(*tx));
	tx->q = q;
	tx->world = world;
	tx->sl = q->sl;
	tx->light_regions_before = world->generation_light_n;
	tx->light_fault_before = world->light_fault;
	tx->light_fault_reason_before = world->light_fault_reason;
	tx->light_tail_count = world->generation_light_n < 5 ? world->generation_light_n : 5;
	for(size_t k = 0; k < tx->light_tail_count; k++)
		tx->light_tail_before[k]
			= *sbl_at(world, world->generation_light_n - tx->light_tail_count + k);
	/* A populate transaction is synchronous and server-thread-owned, so one
	 * world reservation is sufficient. Do not fall back to a peak-time allocate:
	 * that allocation was the portal-return stall at the first fresh frontier. */
	struct server_world_populate_scratch* scratch
		= world->populate_scratch_storage;
	assert(!scratch || (scratch->attached && !scratch->in_use));
	if(!scratch || !scratch->attached || scratch->in_use)
		return false;
	scratch->in_use = true;
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	quad_populate_test_scratch_claims++;
#endif
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++) {
			size_t chunk_index = (size_t)(dz * 2 + dx);
			assert(q->c[dx][dz] && q->c[dx][dz]->ids
				   && q->c[dx][dz]->metadata);
			tx->quad_modified_before[dx][dz] = q->c[dx][dz]->modified;
			tx->quad_save_pending_before[dx][dz] = q->c[dx][dz]->save_pending;
			memcpy(
				scratch->ids[chunk_index], q->c[dx][dz]->ids,
				QUAD_CHUNK_CELL_COUNT);
			memcpy(
				scratch->metadata[chunk_index], q->c[dx][dz]->metadata,
				QUAD_CHUNK_METADATA_BYTES);
			{
				memcpy(scratch->sky[chunk_index], q->c[dx][dz]->lighting_sky,
					QUAD_CHUNK_METADATA_BYTES);
				memcpy(scratch->height[chunk_index], q->c[dx][dz]->heightmap,
					CHUNK_SIZE * CHUNK_SIZE);
			}
		}
	tx->hell_rng_before = world->hell_populate_rng;
	tx->world_jrand_before = world->rand;
	if(tx->sl) {
		tx->world_rng_before = tx->sl->rand_src;
		server_local_math_random_capture(&tx->math_rng_before);
		tx->scheduled_head_before = world->scheduled_tick_head;
		tx->scheduled_count_before = world->scheduled_tick_count;
		tx->scheduled_drops_before = world->scheduled_tick_drops;
	}
	tx->active = true;
	q->tx = tx;
	return true;
}

static bool quad_populate_tx_record(struct quad_ctx* q, struct server_chunk* sc) {
	struct quad_populate_tx* tx = q->tx;
	if(!tx || !tx->active)
		return true;
	if(tx->failed)
		return false;
	bool captured = false;
	for(int dx = 0; dx < 2; dx++) for(int dz = 0; dz < 2; dz++)
		captured |= sc == q->c[dx][dz];
	for(struct quad_populate_extra* e = tx->extra; e && !captured; e = e->next)
		captured = sc == e->chunk;
	if(!captured) {
		struct quad_populate_extra* e
			= G9X_POP_FAULT() ? NULL : mem2_alloc(sizeof(*e));
		if(!e) { tx->failed = true; return false; }
		e->chunk = sc; e->next = tx->extra; tx->extra = e;
		memcpy(e->ids, sc->ids, sizeof e->ids);
		memcpy(e->metadata, sc->metadata, sizeof e->metadata);
		memcpy(e->sky, sc->lighting_sky, sizeof e->sky);
		memcpy(e->height, sc->heightmap, sizeof e->height);
		e->modified = sc->modified;
		e->save_pending = sc->save_pending;
	}
	tx->write_count++;
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	if(tx->write_count > quad_populate_test_max_writes)
		quad_populate_test_max_writes = tx->write_count;
#endif
	return true;
}

// World.getBlockMetadata over the quad: the populate-time leaf-removal flag
// pass (populate.h, next-gaps-0924/W1E-c) reads the species bits it must keep.
static uint8_t quad_get_meta(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= WORLD_HEIGHT)
		return 0;
	struct server_chunk* sc = quad_chunk((struct quad_ctx*)ctx, x, z);
	return sc ? nibble_read(sc->metadata, S_CHUNK_IDX(x, y, z)) : 0;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* G9X-1 observer (test pool only): World.setBlock / setBlockAndMetadata /
 * setBlockMetadata attempts with their callback nesting depth (a setter
 * reached from another setter's removal callback) and the Beta-visible
 * boolean result, computed READ-ONLY before the unchanged body runs:
 * CFR World.java:259-283 (y range -> false), Chunk.java:208-213 / 242-247
 * (unchanged ID [+metadata] -> false), World.java:317-331 (metadata setter
 * true in range). A quadrant-absent chunk reports -1: Beta would have
 * provided one, so the difference must surface, not be guessed. */
static unsigned quad_api_depth;

static void quad_api_emit(const char* op, const struct quad_ctx* q, int x, int y,
	int z, uint8_t id, uint8_t meta, unsigned depth, int result, int line,
	const void* caller) {
	lighting_test_history_emit((struct lighting_history_event){
		.operation = op, .object = q->pop_world,
		.x = x, .y = y, .z = z, .after = id, .before = meta,
		.depth = depth, .result = result,
		.file = __FILE__, .line = line, .caller = caller});
}

static int quad_api_result(struct quad_ctx* q, int x, int y, int z, uint8_t id,
	uint8_t meta, int kind) {
	if(y < 0 || y >= WORLD_HEIGHT)
		return 0;
	const struct server_chunk* sc = quad_chunk(q, x, z);
	if(!sc)
		return -1;
	if(kind == 2)
		return 1;
	const size_t idx = S_CHUNK_IDX(x, y, z);
	return !(sc->ids[idx] == id
		&& (kind == 0 || nibble_read(sc->metadata, idx) == meta));
}
#endif

static void quad_set_kind_body(struct quad_ctx* q, int x, int y, int z,
	uint8_t id, uint8_t meta, bool plain) {
	if(y < 0 || y >= WORLD_HEIGHT)
		return;
	struct server_chunk* sc = quad_chunk(q, x, z);
	if(sc) {
		size_t idx = S_CHUNK_IDX(x, y, z);
		uint8_t old = sc->ids[idx];
		uint8_t old_meta = nibble_read(sc->metadata, idx);
		if(old == id && (plain || old_meta == meta)) return;
		if(!server_beta_light_reserve(q->pop_world, 32)) {
			quad_populate_tx_fail(q->tx);
			return;
		}
		if(!quad_populate_tx_record(q, sc))
			return;
		server_chunk_store(sc, COLUMN_IDS, idx, id);
		/* Chunk.setBlockID[WithMetadata] -> the OLD block's onBlockRemoval
		 * after EVERY non-no-op write, same ID included (G9X-2): CFR
		 * Chunk.java:212-219 returns early only when ID AND metadata match;
		 * plain setBlockID's unchanged-ID no-op (:246) returned above. The
		 * decorate-time removals with an effect are BlockLeaves.java:40-50 and
		 * BlockLog.java:38-48. */
		if(old == BLOCK_LEAVES && q->view)
			wgen_leaves_on_removal(q->view, q->px, q->pz, x, y, z);
		if(old == BLOCK_LOG && q->view)
			wgen_log_on_removal(q->view, q->px, q->pz, x, y, z);
		/* CFR Chunk.java:219/256: reset/store metadata AFTER removal. */
		server_chunk_store(sc, COLUMN_META, idx, plain ? 0 : meta);
		/* Chunk.java:218-239 versus :242-276: immediate height/Sky,
		 * then deferred propagation; no post-population bulk reconstruction. */
		if(plain || q->pop_world->dimension != WORLD_DIM_NETHER)
			server_world_plain_column_write_impl(q->pop_world, sc, x, y, z, id, false);
		/* CFR Chunk.java:218-231: metadata API suppresses the Sky REQUEST
		 * in Nether. Plain setBlockID:267 requests Sky even when World rejects
		 * it. Preserve that distinction in the actual ordered queue history. */
		bool scheduled = (!(plain || q->pop_world->dimension != WORLD_DIM_NETHER)
			|| server_beta_light_schedule(q->pop_world, COLUMN_SKY, x,y,z,x,y,z))
			&& server_beta_light_schedule(q->pop_world, COLUMN_TORCH, x,y,z,x,y,z)
			&& server_beta_light_height_edges(q->pop_world, sc, x, z, sc);
		if(!scheduled || q->pop_world->light_fault) quad_populate_tx_fail(q->tx);
		if(!plain) server_chunk_store(sc, COLUMN_META, idx, meta);
		/* G9X-3: CFR Chunk.java:238/269 -- a non-no-op setter marks the chunk
		 * it ACTUALLY mutated modified as its last step, whichever resident
		 * chunk that is (ChunkProviderServer.java:98-104); func_339_g
		 * (Chunk.java:200, server_world_light_column_write_impl) and
		 * func_355_f (Chunk.java:134/137, height edges above) may have marked
		 * it earlier, at the same points as Beta. The success-path touch of
		 * the 2x2 never reached a chunk outside the quadrant. Rollback
		 * restores the flag recorded by quad_populate_tx_record/_begin. */
		server_world_chunk_touch(sc);
	}
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* G9X-4 observer (test pool only; read-only: no allocation, no fault point,
 * no state change). The fluid/sand driver is an explicit-stack emulation of
 * Beta's recursion: a setter whose new block's onBlockAdded Beta calls INSIDE
 * the setter (CFR Chunk.java:236 / 267, before World.setBlock*
 * returns) is modelled as a later ON_ADDED action. Its "setter-end" (Beta's
 * IRETURN) is therefore held until that ON_ADDED action and every action it
 * pushed have completed, and the api depth stays inside the setter meanwhile,
 * so nested setters/ticks of the callback carry Beta's position and depth.
 * QUAD_API_DEFER() before the setter holds its end; quad_fluid_push_on_added_
 * of_setter() binds it under the ON_ADDED it pushes; quad_api_hold_flush()
 * emits it at once on a path that pushes no ON_ADDED (failure). */
struct quad_api_end {
	const struct quad_ctx* q;
	const void* driver;
	size_t end_at;
	int x, y, z;
	uint8_t id, meta;
	unsigned depth;
	int result, line;
	const void* caller;
};
static bool quad_api_defer_next;
static bool quad_api_held_valid;
static struct quad_api_end quad_api_held;
static struct quad_api_end quad_api_ends[256];
static size_t quad_api_end_n;

static void quad_api_end_emit(const struct quad_api_end* e) {
	quad_api_depth--;
	assert(quad_api_depth == e->depth);
	quad_api_emit("setter-end", e->q, e->x, e->y, e->z, e->id, e->meta,
		e->depth, e->result, e->line, e->caller);
}

static void quad_api_hold_flush(void) {
	if(!quad_api_held_valid)
		return;
	quad_api_held_valid = false;
	quad_api_end_emit(&quad_api_held);
}
#define QUAD_API_DEFER() (quad_api_defer_next = true)
#else
#define QUAD_API_DEFER() ((void)0)
static inline void quad_api_hold_flush(void) {}
#endif

static void quad_set_kind(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta, bool plain) {
	struct quad_ctx* q = ctx;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	const bool defer = quad_api_defer_next; /* this call only, not nested ones */
	quad_api_defer_next = false;
	const void* caller = __builtin_return_address(0);
	const int result = quad_api_result(q, x, y, z, id, meta, plain ? 0 : 1);
	const unsigned depth = quad_api_depth++;
	quad_api_emit(plain ? "api-plain" : "api-metadata", q, x, y, z, id, meta,
		depth, result, __LINE__, caller);
	quad_set_kind_body(q, x, y, z, id, meta, plain);
	if(defer) {
		assert(!quad_api_held_valid);
		quad_api_held = (struct quad_api_end){.q = q, .x = x, .y = y, .z = z,
			.id = id, .meta = meta, .depth = depth, .result = result,
			.line = __LINE__, .caller = caller};
		quad_api_held_valid = true;
		return;
	}
	quad_api_depth--;
	quad_api_emit("setter-end", q, x, y, z, id, meta, depth, result, __LINE__,
		caller);
#else
	quad_set_kind_body(q, x, y, z, id, meta, plain);
#endif
}

/* CFR World.java:294-303 / Chunk.java:285-287: no ID or light queue. */
static void quad_set_meta_body(struct quad_ctx* q, int x, int y, int z,
	uint8_t meta) {
	if(y < 0 || y >= WORLD_HEIGHT) return;
	struct server_chunk* sc = quad_chunk(q, x, z);
	/* An absent chunk is Beta's EmptyChunk (EmptyChunk.java:70-71): the write
	 * is swallowed and no residency is invented. */
	if(sc && quad_populate_tx_record(q, sc)) {
		/* G9X-3: CFR Chunk.java:277-279 marks the chunk modified
		 * unconditionally, also for a same-value nibble and for a chunk
		 * outside the population quadrant (a leaf-removal flag pass). */
		server_world_chunk_touch(sc);
		server_chunk_store(sc, COLUMN_META, S_CHUNK_IDX(x,y,z), meta);
	}
}

static void quad_set_meta(void* ctx, int x, int y, int z, uint8_t meta) {
	struct quad_ctx* q = ctx;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	const void* caller = __builtin_return_address(0);
	const int result = quad_api_result(q, x, y, z, 0, meta, 2);
	const unsigned depth = quad_api_depth++;
	quad_api_emit("api-meta", q, x, y, z, 0, meta, depth, result, __LINE__,
		caller);
	quad_set_meta_body(q, x, y, z, meta);
	quad_api_depth--;
	quad_api_emit("setter-end", q, x, y, z, 0, meta, depth, result, __LINE__,
		caller);
#else
	quad_set_meta_body(q, x, y, z, meta);
#endif
}

static void quad_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	quad_set_kind(ctx, x, y, z, id, meta, false);
}

/* CFR Chunk.java:242-276: plain ID no-op and metadata reset, followed by
 * unguarded func_339_g height/Sky stores. Population keeps its existing
 * deferred Block relight; this path introduces no whole-quadrant Sky scan. */
static void quad_set_plain(void* ctx, int x, int y, int z, uint8_t id) {
	quad_set_kind(ctx, x, y, z, id, 0, true);
}

static bool quad_populate_tx_reserve_chests(
	struct quad_populate_tx* tx, size_t needed) {
	if(needed <= tx->chest_capacity)
		return true;
	size_t capacity = tx->chest_capacity ? tx->chest_capacity : 4;
	while(capacity < needed) {
		if(capacity > SIZE_MAX / 2) {
			quad_populate_tx_fail(tx);
			return false;
		}
		capacity *= 2;
	}
	if(capacity > SIZE_MAX / sizeof(*tx->chests)) {
		quad_populate_tx_fail(tx);
		return false;
	}
	size_t old_size = tx->chest_capacity * sizeof(*tx->chests);
	size_t new_size = capacity * sizeof(*tx->chests);
	void* grown = tx->chests
		? mem2_realloc(tx->chests, old_size, new_size)
		: mem2_alloc(new_size);
	if(!grown) {
		quad_populate_tx_fail(tx);
		return false;
	}
	tx->chests = grown;
	tx->chest_capacity = capacity;
	return true;
}

static bool quad_populate_tx_stage_chest(
	struct quad_populate_tx* tx, int x, int y, int z,
	const struct item_data* items) {
	if(!tx || tx->failed || !items)
		return false;
	if(!quad_populate_tx_reserve_chests(tx, tx->chest_count + 1))
		return false;
	struct quad_populate_chest* stage = &tx->chests[tx->chest_count++];
	stage->x = x;
	stage->y = y;
	stage->z = z;
	memcpy(stage->items, items, sizeof(stage->items));
	return true;
}

// FH-17: chest staging's exact shape, sized for quad_populate_spawner instead.
static bool quad_populate_tx_reserve_spawners(
	struct quad_populate_tx* tx, size_t needed) {
	if(needed <= tx->spawner_capacity)
		return true;
	size_t capacity = tx->spawner_capacity ? tx->spawner_capacity : 4;
	while(capacity < needed) {
		if(capacity > SIZE_MAX / 2) {
			quad_populate_tx_fail(tx);
			return false;
		}
		capacity *= 2;
	}
	if(capacity > SIZE_MAX / sizeof(*tx->spawners)) {
		quad_populate_tx_fail(tx);
		return false;
	}
	size_t old_size = tx->spawner_capacity * sizeof(*tx->spawners);
	size_t new_size = capacity * sizeof(*tx->spawners);
	void* grown = tx->spawners
		? mem2_realloc(tx->spawners, old_size, new_size)
		: mem2_alloc(new_size);
	if(!grown) {
		quad_populate_tx_fail(tx);
		return false;
	}
	tx->spawners = grown;
	tx->spawner_capacity = capacity;
	return true;
}

static bool quad_populate_tx_stage_spawner(
	struct quad_populate_tx* tx, int x, int y, int z, const char* mob_id) {
	if(!tx || tx->failed || !mob_id)
		return false;
	if(!quad_populate_tx_reserve_spawners(tx, tx->spawner_count + 1))
		return false;
	struct quad_populate_spawner* stage = &tx->spawners[tx->spawner_count++];
	stage->x = x;
	stage->y = y;
	stage->z = z;
	size_t n = strlen(mob_id);
	if(n >= sizeof(stage->mob_id))
		n = sizeof(stage->mob_id) - 1;
	memcpy(stage->mob_id, mob_id, n);
	stage->mob_id[n] = '\0';
	return true;
}

static bool quad_populate_tx_reserve_drops(
	struct quad_populate_tx* tx, size_t needed) {
	if(G9X_POP_FAULT()) {
		quad_populate_tx_fail(tx);
		return false;
	}
	if(needed <= tx->drop_capacity)
		return true;
	size_t capacity = tx->drop_capacity ? tx->drop_capacity : 8;
	while(capacity < needed) {
		if(capacity > SIZE_MAX / 2) {
			quad_populate_tx_fail(tx);
			return false;
		}
		capacity *= 2;
	}
	if(capacity > SIZE_MAX / sizeof(*tx->drops)) {
		quad_populate_tx_fail(tx);
		return false;
	}
	size_t old_size = tx->drop_capacity * sizeof(*tx->drops);
	size_t new_size = capacity * sizeof(*tx->drops);
	void* grown = tx->drops
		? mem2_realloc(tx->drops, old_size, new_size)
		: mem2_alloc(new_size);
	if(!grown) {
		quad_populate_tx_fail(tx);
		return false;
	}
	tx->drops = grown;
	tx->drop_capacity = capacity;
	return true;
}

static bool quad_populate_tx_stage_drop(
	void* ctx, const struct server_local_planned_drop* drop) {
	struct quad_populate_tx* tx = ctx;
	if(!tx || tx->failed || !drop
	   || !quad_populate_tx_reserve_drops(tx, tx->drop_count + 1))
		return false;
	tx->drops[tx->drop_count++] = *drop;
	return true;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*server_world_test_populate_rollback_hook)(struct server_world* w);
#endif

static void quad_populate_tx_rollback(struct quad_populate_tx* tx) {
	assert(tx && tx->q && tx->world);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	/* G9X-4: the live, failed state BEFORE anything is restored. */
	if(server_world_test_populate_rollback_hook)
		server_world_test_populate_rollback_hook(tx->world);
#endif
	tx->active = false;
	struct server_world_populate_scratch* scratch
		= tx->world->populate_scratch_storage;
	assert(scratch && scratch->in_use);
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++) {
			size_t chunk_index = (size_t)(dz * 2 + dx);
			struct server_chunk* sc = tx->q->c[dx][dz];
			tps_mutated(3);
			column_backing_write_begin(sc->backing, 3, 0, QUAD_CHUNK_CELL_COUNT);
			memcpy(
				sc->ids, scratch->ids[chunk_index],
				QUAD_CHUNK_CELL_COUNT);
			memcpy(
				sc->metadata, scratch->metadata[chunk_index],
				QUAD_CHUNK_METADATA_BYTES);
			column_backing_write_end(sc->backing, 3, 0, QUAD_CHUNK_CELL_COUNT);
			{
				column_backing_write_begin(sc->backing, 4, 0, QUAD_CHUNK_CELL_COUNT);
				memcpy(sc->lighting_sky, scratch->sky[chunk_index], QUAD_CHUNK_METADATA_BYTES);
				memcpy(sc->heightmap, scratch->height[chunk_index], CHUNK_SIZE * CHUNK_SIZE);
				column_backing_write_end(sc->backing, 4, 0, QUAD_CHUNK_CELL_COUNT);
			}
			sc->modified = tx->quad_modified_before[dx][dz];
			sc->save_pending = tx->quad_save_pending_before[dx][dz];
		}
	for(struct quad_populate_extra* e = tx->extra; e; e = e->next) {
		struct server_chunk* sc = e->chunk;
		column_backing_write_begin(sc->backing, 3, 0, QUAD_CHUNK_CELL_COUNT);
		memcpy(sc->ids, e->ids, sizeof e->ids);
		memcpy(sc->metadata, e->metadata, sizeof e->metadata);
		column_backing_write_end(sc->backing, 3, 0, QUAD_CHUNK_CELL_COUNT);
		column_backing_write_begin(sc->backing, 4, 0, QUAD_CHUNK_CELL_COUNT);
		memcpy(sc->lighting_sky, e->sky, sizeof e->sky);
		memcpy(sc->heightmap, e->height, sizeof e->height);
		column_backing_write_end(sc->backing, 4, 0, QUAD_CHUNK_CELL_COUNT);
		sc->modified = e->modified;
		sc->save_pending = e->save_pending;
	}
	sbl_rollback(tx->world, tx->light_regions_before,
		tx->light_tail_before, tx->light_tail_count);
	if(!tx->world->light_fault_accounted) {
		tx->world->light_fault = tx->light_fault_before;
		tx->world->light_fault_reason = tx->light_fault_reason_before;
	}
	if(tx->world->gen_storage_failed && !tx->world->light_fault_accounted) { /* tentative rollback never clears storage failure */
		tx->world->light_fault = true;
		tx->world->light_fault_reason = SERVER_LIGHT_FAULT_POPULATION_STORAGE;
	}
	tx->world->hell_populate_rng = tx->hell_rng_before;
	tx->world->rand = tx->world_jrand_before;
	if(tx->sl) {
		tx->sl->rand_src = tx->world_rng_before;
		server_local_math_random_restore(&tx->math_rng_before);
		tx->world->scheduled_tick_head = tx->scheduled_head_before;
		tx->world->scheduled_tick_count = tx->scheduled_count_before;
		/* ci-1101/39: the containment set must forget the rolled-back entries. */
		server_world_scheduled_ticks_resync(tx->world);
		tx->world->scheduled_tick_drops = tx->scheduled_drops_before;
	}
	for(size_t i = 0; i < tx->drop_count; i++)
		server_local_cancel_planned_drop(&tx->drops[i]);
	tx->q->tx = NULL;
	quad_populate_tx_release(tx);
}

static bool quad_populate_tx_commit(struct quad_populate_tx* tx);

/* Published population neighbours may already have presentation views. The
 * generation-only entry point is also used on unpublished scratch columns;
 * these two runtime calls must preserve old sky pages before the bulk scan.
 * worldgen_lighting performs no allocation, yield, or mutating callback. */
static void server_world_rebuild_lighting(struct server_chunk* sc,
	enum world_dim dimension) {
	/* CFR Chunk.java:218-231,240-276 / ChunkProviderHell.java:300-347:
	 * neither setter performs a whole-quadrant rebuild. WithMetadata has
	 * the hasNoSky guard; plain setBlockID can update individual Nether
	 * heights. That API distinction is not represented by quad_set yet. */
	if(dimension == WORLD_DIM_NETHER)
		return;
	tps_light_callback census = __atomic_load_n(&world_tps_light, __ATOMIC_ACQUIRE);
	if(census) {
		struct tps_light_input in = {.world = NULL,
			.columns = {sc->ids, sc->lighting_sky, sc->heightmap},
			.dimension = dimension, .count = 1, .kind = 0};
		census(&in);
	}
	column_backing_write_begin(sc->backing, 4, 0, QUAD_CHUNK_CELL_COUNT);
	worldgen_lighting(sc, false, dimension);
	column_backing_write_end(sc->backing, 4, 0, QUAD_CHUNK_CELL_COUNT);
	tps_mutated(4); /* exact comparison of the bulk height/sky output */
}

/*
	Beta WorldGenLiquids / WorldGenHellLava place the moving source (whose
	onBlockAdded arms its normal delayed tick), then temporarily set
	scheduledUpdatesAreImmediate and call updateTick BEFORE the next decoration.
	A normal server_world_set_block cascade here
	would send CRPCs for chunks the client has not received and run a lighting
	flood per fluid cell. This adapter executes the same fluid_spread logic over
	the raw 2x2 populate view; every write of a moving id recursively ticks before
	returning to its parent, matching scheduleBlockUpdate's immediate call order.
	The later one-shot quadrant relight observes the final fluid field.
	[decompile 740c583 WorldGenLiquids.generate, WorldGenHellLava.generate,
	 World.scheduleBlockUpdate(scheduledUpdatesAreImmediate branch)]
*/
static uint8_t quad_fluid_get(void* ctx, int x, int y, int z,
							  uint8_t* meta_out) {
	struct quad_ctx* q = ctx;
	if(y < 0 || y >= WORLD_HEIGHT) {
		if(meta_out)
			*meta_out = 0;
		return BLOCK_AIR;
	}
	struct server_chunk* sc = quad_chunk(q, x, z);
	if(!sc) {
		if(meta_out)
			*meta_out = 0;
		return BLOCK_AIR; /* CFR EmptyChunk.getBlockID: absent provider is air. */
	}
	size_t idx = S_CHUNK_IDX(x, y, z);
	if(meta_out)
		*meta_out = nibble_read(sc->metadata, idx);
	return sc->ids[idx];
}

static bool quad_fluid_blocks_flow(void* ctx, uint8_t type) {
	(void)ctx;
	return fluid_block_blocks_flow_id(type);
}

static bool quad_fluid_material_solid(void* ctx, uint8_t type) {
	(void)ctx;
	return fluid_block_material_is_solid_id(type);
}

enum quad_fluid_action_kind {
	QUAD_FLUID_ACTION_NOOP,
	QUAD_FLUID_ACTION_TICK,
	QUAD_FLUID_ACTION_ON_ADDED,
	QUAD_FLUID_ACTION_NOTIFY,
	QUAD_FLUID_ACTION_ON_NEIGHBOUR,
	QUAD_FLUID_ACTION_EDIT_END,
	QUAD_FLUID_ACTION_SCHEDULE,
	QUAD_FLUID_ACTION_FALLING,
	QUAD_FLUID_ACTION_FALLING_FINISH,
	QUAD_FLUID_ACTION_LAVA_MIX,
};

struct quad_fluid_pos_action {
	int x, y, z;
	uint8_t type;
};

struct quad_fluid_action {
	enum quad_fluid_action_kind kind;
	union {
		struct {
			struct fluid_tick_state state;
			struct java_random* feature_rng;
			uint8_t type;
			bool started;
		} tick;
		struct quad_fluid_pos_action pos;
		struct {
			int x, y, z;
			uint8_t next_neighbour;
		} notify;
	};
};

struct quad_fluid_driver {
	struct quad_ctx* q;
	struct quad_fluid_action* actions;
	size_t count;
	size_t capacity;
	bool immediate;
	bool failed;
	/* Two BlockFlowing instances, water and lava, shared by nested ticks. */
	bool directions[2][4];
};

#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
static size_t quad_fluid_test_peak_actions;
static size_t quad_fluid_test_fail_reserve_above;
static size_t quad_fluid_test_failed_after_writes;
static size_t quad_fluid_test_failed_chests;
static size_t quad_fluid_test_failed_drops;
static uint32_t quad_fluid_test_failed_lava_mix;
#endif

static bool quad_fluid_is_flow(uint8_t type) {
	return type == BLOCK_WATER_FLOW || type == BLOCK_LAVA_FLOW;
}

static bool quad_fluid_is_still(uint8_t type) {
	return type == BLOCK_WATER_STILL || type == BLOCK_LAVA_STILL;
}

static bool quad_fluid_is_lava(uint8_t type) {
	return type == BLOCK_LAVA_FLOW || type == BLOCK_LAVA_STILL;
}

static bool quad_fluid_is_water(uint8_t type) {
	return type == BLOCK_WATER_FLOW || type == BLOCK_WATER_STILL;
}

static bool quad_fluid_driver_push(struct quad_fluid_driver* driver,
								  struct quad_fluid_action action);

static int quad_fluid_next_int(void* ctx, int bound) {
	struct quad_ctx* q = ctx;
	if(q->fluid_active_rng)
		return jrand_next_int_bound(q->fluid_active_rng, bound);
	/* CFR World.java:scheduledUpdatesAreImmediate uses World.rand. */
	return q->pop_world ? jrand_next_int_bound(&q->pop_world->rand, bound) : 0;
}

static void quad_fluid_drop(void* ctx, int x, int y, int z, uint8_t type,
							uint8_t meta) {
	struct quad_ctx* q = ctx;
	if(!q->sl)
		return;
	struct block_data old = {.type = type, .metadata = meta};
	struct block_info info
		= {.block = &old, .neighbours = NULL, .x = x, .y = y, .z = z};
	if(q->tx && q->tx->active) {
		if(q->tx->failed)
			return;
		/* F4-RNG: plan on the POPULATING world's World.rand. */
		if(!(q->pop_world
			 ? server_local_plan_block_drops_w(
				   q->pop_world, &info, quad_populate_tx_stage_drop, q->tx)
			 : server_local_plan_block_drops(
				   q->sl, &info, quad_populate_tx_stage_drop, q->tx)))
			quad_populate_tx_fail(q->tx);
		return;
	}
	/* F4-RNG: the drop belongs to the populating world (its World.rand). */
	struct server_world* bw_prev = block_world_enter(q->sl, q->pop_world);
	server_local_spawn_block_drops(q->sl, &info);
	block_world_leave(q->sl, bw_prev);
}

static void quad_fluid_lava_mix(void* ctx, int x, int y, int z) {
	struct quad_ctx* q = ctx;
	if(!q->sl)
		return;
	if(q->tx && q->tx->active) {
		if(q->tx->failed)
			return;
		G9B_NOTE(q->pop_world, "population-fizz", "before", "", x, z, NULL);
		/* Same BlockFluid.java:217-222 helper as runtime callbacks, under
		 * the populating world's dispatch; only the effect count is staged. */
		struct server_world* prev = block_world_enter(q->sl, q->pop_world);
		(void)fluid_block_lava_mix_effect_draws(q->sl);
		block_world_leave(q->sl, prev);
		G9B_NOTE(q->pop_world, "population-fizz", "after", "", x, z, NULL);
		q->tx->lava_mix_effects++;
		return;
	}
	struct server_world* prev = block_world_enter(q->sl, q->pop_world);
	fluid_block_lava_mix_effect(q->sl, x, y, z, false);
	block_world_leave(q->sl, prev);
}

static bool quad_fluid_immediate_area_loaded(const struct quad_ctx* q, int x,
										 int z) {
	/* World.scheduleBlockUpdate's immediate branch first calls
	   checkChunksExist(x-8,z-8 .. x+8,z+8). */
	for(int cx = WCOORD_CHUNK_OFFSET(x-8); cx <= WCOORD_CHUNK_OFFSET(x+8); cx++)
		for(int cz = WCOORD_CHUNK_OFFSET(z-8); cz <= WCOORD_CHUNK_OFFSET(z+8); cz++)
			if(!server_world_chunk_resident(q->pop_world, cx, cz)) return false;
	return true;
}

/* The recovered World.scheduledUpdatesAreImmediate path is recursive in Java:
   each set finishes onBlockAdded + neighbour notification (and any child tick)
   before its parent updateTick resumes.  Wii's server thread has a fixed 64 KiB
   stack, so the adapter represents those continuations as heap actions instead
   of C frames.  fluid_tick_state_step yields after each set; the actions pushed
   by that set then run above the suspended tick, preserving exact DFS order. */
static bool quad_fluid_driver_reserve(struct quad_fluid_driver* driver,
									 size_t needed) {
	if(G9X_POP_FAULT()) {
		driver->failed = true;
		quad_populate_tx_fail(driver->q->tx);
		return false;
	}
	if(needed <= driver->capacity)
		return true;
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	if(quad_fluid_test_fail_reserve_above > 0
	   && needed > quad_fluid_test_fail_reserve_above) {
		quad_fluid_test_fail_reserve_above = 0; /* one-shot injection */
		driver->failed = true;
		quad_populate_tx_fail(driver->q->tx);
		return false;
	}
#endif
	size_t capacity = driver->capacity ? driver->capacity : 32;
	while(capacity < needed) {
		if(capacity > SIZE_MAX / 2) {
			driver->failed = true;
			quad_populate_tx_fail(driver->q->tx);
			return false;
		}
		capacity *= 2;
	}
	if(capacity > SIZE_MAX / sizeof(*driver->actions)) {
		driver->failed = true;
		quad_populate_tx_fail(driver->q->tx);
		return false;
	}
	size_t old_size = driver->capacity * sizeof(*driver->actions);
	size_t new_size = capacity * sizeof(*driver->actions);
	void* grown = driver->actions
		? mem2_realloc(driver->actions, old_size, new_size)
		: mem2_alloc(new_size);
	if(!grown) {
		driver->failed = true;
		quad_populate_tx_fail(driver->q->tx);
		return false;
	}
	driver->actions = grown;
	driver->capacity = capacity;
	return true;
}

static bool quad_fluid_driver_push(struct quad_fluid_driver* driver,
								  struct quad_fluid_action action) {
	if(driver->failed || (driver->q->tx && driver->q->tx->failed)
	   || !quad_fluid_driver_reserve(driver, driver->count + 1))
		return false;
	driver->actions[driver->count++] = action;
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	if(driver->count > quad_fluid_test_peak_actions)
		quad_fluid_test_peak_actions = driver->count;
#endif
	return true;
}

static void quad_fluid_push_pos(struct quad_fluid_driver* driver,
								const enum quad_fluid_action_kind kind, int x,
								int y, int z) {
	quad_fluid_driver_push(driver,
		(struct quad_fluid_action) {
			.kind = kind, .pos = {.x = x, .y = y, .z = z}});
}

static void quad_fluid_push_notify(struct quad_fluid_driver* driver, int x,
								  int y, int z) {
	/* G9X-6: this is a population-time notifyBlockChange, i.e. Beta's
	 * markBlockNeedsUpdate for the written cell [CFR World.java:332-346 -> notifyBlockChange -> markBlockNeedsUpdate :348-357:
	 * setBlockWithNotify / setBlockAndMetadataWithNotify; :306-315
	 * setBlockMetadataWithNotify for fluids, which are requiresSelfNotify,
	 * Block.java:225-228; spring source WorldGenLiquids.java:62-64; dungeon /
	 * glowstone / snow via quad_set_plain_notify]. Beta publishes that cell
	 * to every watcher of an already-sent chunk, so the column owes it. */
	if(y >= 0 && y < WORLD_HEIGHT) {
		struct server_chunk* sc = quad_chunk(driver->q, x, z);
		if(sc)
			server_chunk_pub_touch(sc);
	}
	quad_fluid_driver_push(driver,
		(struct quad_fluid_action) {
			.kind = QUAD_FLUID_ACTION_NOTIFY,
			.notify = {.x = x, .y = y, .z = z, .next_neighbour = 0}});
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Emit every held setter end of `driver` whose ON_ADDED cascade completed
 * (the stack is back at the depth it had below that ON_ADDED); all = the
 * driver stopped (done or failed). */
static void quad_api_ends_settle(const struct quad_fluid_driver* driver,
	bool all) {
	while(quad_api_end_n && quad_api_ends[quad_api_end_n - 1].driver == driver
		  && (all || driver->count <= quad_api_ends[quad_api_end_n - 1].end_at))
		quad_api_end_emit(&quad_api_ends[--quad_api_end_n]);
}
#endif

/* The ON_ADDED that models the just-written setter's own onBlockAdded (see
 * quad_api_end): binds that setter's held end under it, then pushes it. */
static void quad_fluid_push_on_added_of_setter(
	struct quad_fluid_driver* driver, int x, int y, int z) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(quad_api_held_valid) {
		quad_api_held_valid = false;
		assert(quad_api_end_n < sizeof quad_api_ends / sizeof quad_api_ends[0]);
		quad_api_held.driver = driver;
		quad_api_held.end_at = driver->count;
		quad_api_ends[quad_api_end_n++] = quad_api_held;
	}
#endif
	quad_fluid_push_pos(driver, QUAD_FLUID_ACTION_ON_ADDED, x, y, z);
}

static void quad_fluid_push_tick(struct quad_fluid_driver* driver, int x, int y,
								int z, uint8_t type,
								struct java_random* feature_rng) {
	assert(type == BLOCK_WATER_FLOW || type == BLOCK_LAVA_FLOW);
	struct quad_fluid_action action = {
		.kind = QUAD_FLUID_ACTION_TICK,
		.tick = {.feature_rng = feature_rng, .type = type},
	};
	bool nether = driver->q->pop_world
		&& driver->q->pop_world->dimension == WORLD_DIM_NETHER;
	fluid_tick_state_init(&action.tick.state, x, y, z,
						  type == BLOCK_LAVA_FLOW, nether);
	quad_fluid_driver_push(driver, action);
}

static void quad_fluid_schedule(struct quad_fluid_driver* driver, int x, int y,
								int z, uint8_t type) {
	if(!driver->immediate) {
		if(driver->q->sl)
			server_world_schedule_block_tick(
				driver->q->pop_world, x, y, z, type);
		return;
	}
	/* Beta's immediate branch does nothing when the +/-8 area is not loaded. */
	if(quad_fluid_immediate_area_loaded(driver->q, x, z)) {
		if(type == BLOCK_SAND || type == BLOCK_GRAVEL)
			quad_fluid_driver_push(driver, (struct quad_fluid_action){
				.kind=QUAD_FLUID_ACTION_FALLING, .pos={.x=x,.y=y,.z=z,.type=type}});
		else quad_fluid_push_tick(driver, x, y, z, type, NULL);
	}
}

/* Pure checkForHarden predicate. The action interpreter owns replacement,
   notification and effect ordering so none of them can recurse in C. */
static bool quad_fluid_harden_product_at(struct quad_ctx* q, int x, int y,
									int z, uint8_t* product_out) {
	uint8_t meta;
	uint8_t type = quad_fluid_get(q, x, y, z, &meta);
	if(!quad_fluid_is_lava(type))
		return false;
	static const int dx[5] = {0, 0, -1, 1, 0};
	static const int dy[5] = {0, 0, 0, 0, 1};
	static const int dz[5] = {-1, 1, 0, 0, 0};
	for(int i = 0; i < 5; i++) {
		uint8_t ignored;
		if(quad_fluid_is_water(
			   quad_fluid_get(q, x + dx[i], y + dy[i], z + dz[i], &ignored))) {
			*product_out = fluid_block_harden_product(meta);
			return true;
		}
	}
	return false;
}

static void quad_fluid_harden_action(struct quad_fluid_driver* driver, int x,
									int y, int z, uint8_t product) {
	if(product == BLOCK_AIR) {
		/* Raw metadata 5..15 still consumes the two World.rand fizz draws. */
		quad_fluid_lava_mix(driver->q, x, y, z);
		return;
	}
	/* CFR BlockFluid.java:207-210: nested hardening is plain ID. */
	quad_set_plain(driver->q, x, y, z, product);
	/* setBlockWithNotify: neighbour callbacks complete before the mix effect. */
	quad_fluid_push_pos(
		driver, QUAD_FLUID_ACTION_LAVA_MIX, x, y, z);
	quad_fluid_push_notify(driver, x, y, z);
}

static void quad_fluid_process_on_added(struct quad_fluid_driver* driver,
									   int x, int y, int z) {
	uint8_t product;
	if(quad_fluid_harden_product_at(driver->q, x, y, z, &product)) {
		quad_fluid_harden_action(driver, x, y, z, product);
		if(product != BLOCK_AIR)
			return;
	}
	uint8_t meta;
	uint8_t type = quad_fluid_get(driver->q, x, y, z, &meta);
	(void)meta;
	if(quad_fluid_is_flow(type) || type == BLOCK_SAND || type == BLOCK_GRAVEL)
		quad_fluid_schedule(driver, x, y, z, type);
}

static void quad_fluid_process_on_neighbour(struct quad_fluid_driver* driver,
										int x, int y, int z) {
	uint8_t product;
	if(quad_fluid_harden_product_at(driver->q, x, y, z, &product)) {
		quad_fluid_harden_action(driver, x, y, z, product);
		if(product != BLOCK_AIR)
			return;
	}
	uint8_t meta;
	uint8_t type = quad_fluid_get(driver->q, x, y, z, &meta);
	if(type == BLOCK_SAND || type == BLOCK_GRAVEL) {
		quad_fluid_schedule(driver, x, y, z, type);
		return;
	}
	if(!quad_fluid_is_still(type))
		return;
	/* BlockStationary.setNotStationary: editingBlocks remains true through the
	   child onBlockAdded/immediate cascade, then is restored by EDIT_END. */
	uint8_t flow = type == BLOCK_WATER_STILL ? BLOCK_WATER_FLOW : BLOCK_LAVA_FLOW;
	/* CFR BlockStationary.java:37,41 uses one World boolean, not a
	 * nesting depth: a nested wake clears its caller's flag too. */
	driver->q->fluid_editing_depth = 1;
	QUAD_API_DEFER();
	quad_set(driver->q, x, y, z, flow, meta);
	quad_fluid_push_pos(driver, QUAD_FLUID_ACTION_EDIT_END, x, y, z);
	/* CFR BlockStationary.java:36-41: a SECOND schedule follows the
	 * complete child onBlockAdded; its immediate branch rechecks exact ID. */
	quad_fluid_driver_push(driver, (struct quad_fluid_action){
		.kind = QUAD_FLUID_ACTION_SCHEDULE, .pos = {.x=x,.y=y,.z=z,.type=flow}});
	quad_fluid_push_on_added_of_setter(driver, x, y, z);
}

static void quad_fluid_set(void* ctx, int x, int y, int z, uint8_t type,
						   uint8_t meta) {
	struct quad_ctx* q = ctx;
	if(y < 0 || y >= WORLD_HEIGHT || !quad_chunk(q, x, z))
		return;
	uint8_t old_meta;
	uint8_t old_type = quad_fluid_get(q, x, y, z, &old_meta);
	if(old_type == type && old_meta == meta)
		return;
	struct quad_fluid_driver* driver = q->fluid_driver;
	assert(driver);
	bool old_flow = quad_fluid_is_flow(old_type);
	bool old_still = quad_fluid_is_still(old_type);
	bool new_flow = quad_fluid_is_flow(type);
	bool new_still = quad_fluid_is_still(type);
	bool same_fluid_family
		= (quad_fluid_is_water(old_type) && quad_fluid_is_water(type))
		|| (quad_fluid_is_lava(old_type) && quad_fluid_is_lava(type));
	/* CFR BlockFlowing.java:72-75: removal is plain ID; same-ID
	 * flow changes are metadata-only; all other transitions carry metadata. */
	if(type == BLOCK_AIR) quad_set_plain(q, x, y, z, type);
	else if(old_type == type && new_flow) quad_set_meta(q, x, y, z, meta);
	else {
		QUAD_API_DEFER(); /* its onBlockAdded is the ON_ADDED pushed below */
		quad_set(q, x, y, z, type, meta);
	}
	if(q->tx && q->tx->failed) {
		quad_api_hold_flush();
		return;
	}

	/* updateFlow / setNotStationary: onBlockAdded, no outer notification. */
	if(same_fluid_family
	   && ((old_flow && new_still) || (old_still && new_flow))) {
		quad_fluid_push_on_added_of_setter(driver, x, y, z);
		return;
	}

	/* Metadata update: notify, immediate self schedule, notify. Push in reverse. */
	if(old_type == type && new_flow) {
		quad_fluid_push_notify(driver, x, y, z);
		if(driver->immediate && quad_fluid_immediate_area_loaded(q, x, z))
			quad_fluid_push_tick(driver, x, y, z, type, NULL);
		else if(!driver->immediate && q->sl)
			server_world_schedule_block_tick(&q->sl->world, x, y, z, type);
		quad_fluid_push_notify(driver, x, y, z);
		return;
	}

	/* General setBlockWithNotify: new block onAdded, then outer notification. */
	quad_fluid_push_notify(driver, x, y, z);
	if(type != BLOCK_AIR)
		quad_fluid_push_on_added_of_setter(driver, x, y, z);
}

static void quad_fluid_driver_run(struct quad_fluid_driver* driver) {
	static const int dx[6] = {-1, 1, 0, 0, 0, 0};
	static const int dy[6] = {0, 0, -1, 1, 0, 0};
	static const int dz[6] = {0, 0, 0, 0, -1, 1};
	struct quad_ctx* q = driver->q;
	assert(!q->fluid_driver);
	q->fluid_driver = driver;
	while(driver->count > 0 && !driver->failed
		  && !(q->tx && q->tx->failed)) {
		size_t top = driver->count - 1;
		struct quad_fluid_action* action = &driver->actions[top];
		switch(action->kind) {
		case QUAD_FLUID_ACTION_NOOP:
			driver->count--;
			break;
		case QUAD_FLUID_ACTION_TICK: {
			if(!action->tick.started && !action->tick.feature_rng
			   && quad_get(q, action->tick.state.x, action->tick.state.y, action->tick.state.z)
				!= action->tick.type) {
				driver->count--;
				break;
			}
			action->tick.started = true;
			/* quad_fluid_set may append at most three lifecycle actions. Reserve
			   first so its callback cannot invalidate the in-array state pointer. */
			if(!quad_fluid_driver_reserve(driver, driver->count + 3))
				break;
			action = &driver->actions[top];
			q->fluid_active_rng = action->tick.feature_rng;
			struct fluid_world fluid = {
				.ctx = q,
				.get = quad_fluid_get,
				.set = quad_fluid_set,
				.blocks_flow = quad_fluid_blocks_flow,
				.material_is_solid = quad_fluid_material_solid,
				.next_int = quad_fluid_next_int,
				.drop_block = quad_fluid_drop,
				.lava_mix = quad_fluid_lava_mix,
				.shared_directions = driver->directions[action->tick.type == BLOCK_LAVA_FLOW],
			};
			bool done = fluid_tick_state_step(&fluid, &action->tick.state);
			q->fluid_active_rng = NULL;
			if(done)
				driver->actions[top].kind = QUAD_FLUID_ACTION_NOOP;
			break;
		}
		case QUAD_FLUID_ACTION_ON_ADDED: {
			struct quad_fluid_pos_action pos = action->pos;
			driver->count--;
			quad_fluid_process_on_added(driver, pos.x, pos.y, pos.z);
			break;
		}
		case QUAD_FLUID_ACTION_NOTIFY: {
			int x = action->notify.x, y = action->notify.y;
			int z = action->notify.z;
			uint8_t next = action->notify.next_neighbour;
			driver->count--;
			if(q->fluid_editing_depth > 0 || next >= 6)
				break;
			/* Keep the loop continuation below this neighbour's complete cascade. */
			if(next + 1 < 6)
				quad_fluid_driver_push(driver,
					(struct quad_fluid_action) {
						.kind = QUAD_FLUID_ACTION_NOTIFY,
						.notify = {.x = x,
								   .y = y,
								   .z = z,
								   .next_neighbour = (uint8_t)(next + 1)}});
			quad_fluid_push_pos(driver, QUAD_FLUID_ACTION_ON_NEIGHBOUR,
								x + dx[next], y + dy[next], z + dz[next]);
			break;
		}
		case QUAD_FLUID_ACTION_ON_NEIGHBOUR: {
			struct quad_fluid_pos_action pos = action->pos;
			driver->count--;
			quad_fluid_process_on_neighbour(driver, pos.x, pos.y, pos.z);
			break;
		}
		/* CFR BlockSand.java:tryToFall: worldgen sets fallInstantly=true.
		 * AIR+notify completes BEFORE the downward search and final placement. */
		case QUAD_FLUID_ACTION_FALLING: {
			struct quad_fluid_pos_action pos = action->pos;
			driver->count--;
			uint8_t below = quad_get(q, pos.x, pos.y-1, pos.z);
			if(pos.y < 0 || quad_get(q,pos.x,pos.y,pos.z) != pos.type
			   || !(below == BLOCK_AIR || below == BLOCK_FIRE || quad_fluid_is_water(below) || quad_fluid_is_lava(below))) break;
			quad_set_plain(q, pos.x, pos.y, pos.z, BLOCK_AIR);
			quad_fluid_driver_push(driver, (struct quad_fluid_action){
				.kind=QUAD_FLUID_ACTION_FALLING_FINISH, .pos=pos});
			quad_fluid_push_notify(driver, pos.x, pos.y, pos.z);
			break;
		}
		case QUAD_FLUID_ACTION_FALLING_FINISH: {
			struct quad_fluid_pos_action pos = action->pos;
			driver->count--;
			while(pos.y > 0) {
				uint8_t below = quad_get(q, pos.x, pos.y-1, pos.z);
				if(!(below == BLOCK_AIR || below == BLOCK_FIRE || quad_fluid_is_water(below) || quad_fluid_is_lava(below))) break;
				pos.y--;
			}
			if(pos.y > 0) {
				QUAD_API_DEFER();
				quad_set_plain(q, pos.x, pos.y, pos.z, pos.type);
				quad_fluid_push_notify(driver, pos.x, pos.y, pos.z);
				quad_fluid_push_on_added_of_setter(driver, pos.x, pos.y, pos.z);
			}
			break;
		}
		case QUAD_FLUID_ACTION_SCHEDULE: {
			struct quad_fluid_pos_action pos = action->pos;
			driver->count--;
			if(!driver->immediate || quad_get(q, pos.x, pos.y, pos.z) == pos.type)
				quad_fluid_schedule(driver, pos.x, pos.y, pos.z, pos.type);
			break;
		}
		case QUAD_FLUID_ACTION_EDIT_END:
			driver->count--;
			q->fluid_editing_depth = 0;
			break;
		case QUAD_FLUID_ACTION_LAVA_MIX: {
			struct quad_fluid_pos_action pos = action->pos;
			driver->count--;
			quad_fluid_lava_mix(q, pos.x, pos.y, pos.z);
			break;
		}
		}
#ifdef CAVEWII_SERVER_LOCAL_TEST
		quad_api_ends_settle(driver, false);
#endif
	}
#ifdef CAVEWII_SERVER_LOCAL_TEST
	quad_api_hold_flush(); /* a held end its caller never bound (stopped) */
	quad_api_ends_settle(driver, true);
#endif
	q->fluid_active_rng = NULL;
	q->fluid_driver = NULL;
}

static void quad_fluid_tick_immediate(void* ctx,
									  struct java_random* feature_rng, int x, int y,
									  int z, uint8_t type) {
	struct quad_ctx* q = ctx;
	assert(!q->fluid_driver && q->fluid_editing_depth == 0);
	struct quad_fluid_driver driver = {.q = q};
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	quad_fluid_test_peak_actions = 0;
#endif

	/* Source setBlockWithNotify occurred before World enabled its immediate flag:
	   onAdded leaves a normal delayed tick, then the outer notify completes. */
	driver.immediate = false;
	quad_fluid_push_notify(&driver, x, y, z);
	quad_fluid_push_pos(&driver, QUAD_FLUID_ACTION_ON_ADDED, x, y, z);
	quad_fluid_driver_run(&driver);

	if(!driver.failed && !(q->tx && q->tx->failed)) {
		driver.immediate = true;
		quad_fluid_push_tick(&driver, x, y, z, type, feature_rng);
		quad_fluid_driver_run(&driver);
	}
	if(driver.failed || (q->tx && q->tx->failed))
		q->fluid_editing_depth = 0; /* adapter-only state; world cells roll back */
	else
		assert(q->fluid_editing_depth == 0 && !q->fluid_active_rng);
	mem2_free_sized(driver.actions,
					driver.capacity * sizeof(*driver.actions));
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*server_world_test_notify_boundary_hook)(struct server_world* w,
	int x, int y, int z);
#endif

/* CFR World.setBlockWithNotify: plain Chunk API, then six neighbors.
 * Non-fluid feature writes must also wake neighboring stationary fluids. */
static void quad_set_plain_notify_body(void* ctx, int x, int y, int z, uint8_t id) {
	struct quad_ctx* q = ctx;
	/* CFR World.java:setBlockWithNotify always attempts setBlock; only its
	 * successful return triggers notification. Dungeon AIR rewrites are real
	 * no-op attempts in the original-JAR history, not skipped caller calls. */
	const bool changed = y >= 0 && y < WORLD_HEIGHT && quad_chunk(q,x,z)
		&& quad_get(ctx,x,y,z) != id;
	const bool on_added = quad_fluid_is_flow(id) || id == BLOCK_SAND || id == BLOCK_GRAVEL;
	if(changed && on_added)
		QUAD_API_DEFER(); /* its onBlockAdded is the ON_ADDED pushed below */
	quad_set_plain(q, x, y, z, id);
	if(!changed) return;
	if(q->tx && q->tx->failed) {
		quad_api_hold_flush();
		return;
	}
	struct quad_fluid_driver driver = {.q = q};
	quad_fluid_push_notify(&driver, x, y, z);
	if(on_added)
		quad_fluid_push_on_added_of_setter(&driver, x, y, z);
	quad_fluid_driver_run(&driver);
	if(driver.failed || (q->tx && q->tx->failed)) q->fluid_editing_depth = 0;
	else assert(q->fluid_editing_depth == 0);
	mem2_free_sized(driver.actions, driver.capacity * sizeof(*driver.actions));
}

static void quad_set_plain_notify(void* ctx, int x, int y, int z, uint8_t id) {
	quad_set_plain_notify_body(ctx, x, y, z, id);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	/* G9X-4 read-only observer: the external call (setter + its complete
	 * notification cascade) has returned. */
	struct quad_ctx* q = ctx;
	if(server_world_test_notify_boundary_hook && q->pop_world)
		server_world_test_notify_boundary_hook(q->pop_world, x, y, z);
#endif
}

static void quad_set_chest(void* ctx, int x, int y, int z,
						   const struct item_data* items);

#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
void server_world_test_populate_scratch_stats_reset(void) {
	quad_populate_test_scratch_claims = 0;
	quad_populate_test_committed_writes = 0;
	quad_populate_test_max_writes = 0;
}

void server_world_test_populate_scratch_stats(
	size_t* scratch_claims, size_t* committed_writes,
	size_t* max_writes) {
	if(scratch_claims)
		*scratch_claims = quad_populate_test_scratch_claims;
	if(committed_writes)
		*committed_writes = quad_populate_test_committed_writes;
	if(max_writes)
		*max_writes = quad_populate_test_max_writes;
}

bool server_world_test_populate_scratch_in_use(
	const struct server_world* world) {
	return world && world->populate_scratch_storage
		&& world->populate_scratch_storage->in_use;
}

size_t server_world_test_populate_fluid_peak_actions(void) {
	return quad_fluid_test_peak_actions;
}

void server_world_test_fail_populate_fluid_action_reserve_above(
	size_t action_count) {
	quad_fluid_test_fail_reserve_above = action_count;
}

size_t server_world_test_populate_fluid_failed_after_writes(void) {
	return quad_fluid_test_failed_after_writes;
}

void server_world_test_populate_fluid_failed_staged(
	size_t* chests, size_t* drops, uint32_t* lava_mix_effects) {
	if(chests)
		*chests = quad_fluid_test_failed_chests;
	if(drops)
		*drops = quad_fluid_test_failed_drops;
	if(lava_mix_effects)
		*lava_mix_effects = quad_fluid_test_failed_lava_mix;
}

static bool quad_fluid_test_ctx(
	struct server_world* w, w_coord_t px, w_coord_t pz, struct quad_ctx* q) {
	*q = (struct quad_ctx) {.px = px, .pz = pz, .sl = w->session, .pop_world = w};
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++) {
			q->c[dx][dz] = dict_server_chunks_get(
				w->chunks, S_CHUNK_ID(px + dx, pz + dz));
			if(!q->c[dx][dz])
				return false;
		}
	return true;
}

bool server_world_test_populate_snapshot_rollback(
	struct server_world* w, w_coord_t px, w_coord_t pz,
	size_t unique_writes) {
	assert(w && unique_writes <= QUAD_POPULATE_CELL_COUNT);
	struct quad_ctx q;
	if(!quad_fluid_test_ctx(w, px, pz, &q))
		return false;
	struct quad_populate_tx tx;
	if(!quad_populate_tx_begin(&tx, &q, w))
		return false;
	for(size_t cell = 0; cell < unique_writes && !tx.failed; cell++) {
		size_t chunk_index = cell / QUAD_CHUNK_CELL_COUNT;
		size_t idx = cell % QUAD_CHUNK_CELL_COUNT;
		int dx = (int)(chunk_index & 1u);
		int dz = (int)(chunk_index >> 1u);
		int y = (int)(idx % WORLD_HEIGHT);
		size_t column = idx / WORLD_HEIGHT;
		int z = (int)(column % CHUNK_SIZE);
		int x = (int)(column / CHUNK_SIZE);
		struct server_chunk* sc = q.c[dx][dz];
		uint8_t replacement
			= sc->ids[idx] == BLOCK_BEDROCK ? BLOCK_STONE : BLOCK_BEDROCK;
		if(w->dimension == WORLD_DIM_NETHER)
			quad_set_plain(&q, (px + dx) * CHUNK_SIZE + x, y,
				(pz + dz) * CHUNK_SIZE + z, replacement);
		else
			quad_set(&q, (px + dx) * CHUNK_SIZE + x, y,
				(pz + dz) * CHUNK_SIZE + z, replacement, 0);
	}
	bool complete = !tx.failed && tx.write_count == unique_writes;
	quad_populate_tx_rollback(&tx);
	return complete;
}

bool server_world_test_populate_fluid_immediate(
	struct server_world* w, w_coord_t px, w_coord_t pz, int x, int y, int z,
	uint8_t type, struct java_random* feature_rng) {
	assert(w && feature_rng);
	struct quad_ctx q;
	if(!quad_fluid_test_ctx(w, px, pz, &q))
		return false;
	struct quad_populate_tx tx;
	if(!quad_populate_tx_begin(&tx, &q, w))
		return false;
	struct java_random feature_before = *feature_rng;
	quad_set(&q, x, y, z, type, 0);
	quad_fluid_tick_immediate(&q, feature_rng, x, y, z, type);
	if(tx.failed) {
		quad_fluid_test_failed_after_writes = tx.write_count;
		quad_fluid_test_failed_chests = tx.chest_count;
		quad_fluid_test_failed_drops = tx.drop_count;
		quad_fluid_test_failed_lava_mix = tx.lava_mix_effects;
		quad_populate_tx_rollback(&tx);
		if(feature_rng != &w->hell_populate_rng)
			*feature_rng = feature_before;
		return false;
	}
	if(!quad_populate_tx_commit(&tx)) {
		quad_fluid_test_failed_after_writes = tx.write_count;
		quad_fluid_test_failed_chests = tx.chest_count;
		quad_fluid_test_failed_drops = tx.drop_count;
		quad_fluid_test_failed_lava_mix = tx.lava_mix_effects;
		quad_populate_tx_rollback(&tx);
		if(feature_rng != &w->hell_populate_rng)
			*feature_rng = feature_before;
		return false;
	}
	return true;
}

bool server_world_test_populate_fluid_transaction(
	struct server_world* w, w_coord_t px, w_coord_t pz,
	int marker_x, int marker_y, int marker_z, uint8_t marker_type,
	uint8_t marker_meta, const struct server_world_test_fluid_source* sources,
	size_t source_count, struct java_random* feature_rng, bool stage_chest) {
	assert(w && feature_rng && (source_count == 0 || sources));
	struct quad_ctx q;
	if(!quad_fluid_test_ctx(w, px, pz, &q))
		return false;
	struct quad_populate_tx tx;
	if(!quad_populate_tx_begin(&tx, &q, w))
		return false;
	struct java_random feature_before = *feature_rng;
	quad_fluid_test_failed_after_writes = 0;
	quad_fluid_test_failed_chests = 0;
	quad_fluid_test_failed_drops = 0;
	quad_fluid_test_failed_lava_mix = 0;
	quad_set(&q, marker_x, marker_y, marker_z, marker_type, marker_meta);
	if(stage_chest) {
		/* Simulate the shared decorate stream draw which rolled this dungeon's
		 * loot/position before its set_chest callback. The production transaction
		 * begins before those draws, so the failure regression must restore it. */
		(void)jrand_next_int_bound(feature_rng, 31);
		struct item_data items[DUNGEON_CHEST_SLOTS] = {0};
		items[0] = (struct item_data) {
			.id = BLOCK_COBBLESTONE, .durability = 0, .count = 1};
		quad_set_chest(&q, marker_x, marker_y, marker_z, items);
	}
	for(size_t i = 0; i < source_count && !tx.failed; i++) {
		quad_set(&q, sources[i].x, sources[i].y, sources[i].z,
				 sources[i].type, 0);
		if(!tx.failed)
			quad_fluid_tick_immediate(
				&q, feature_rng, sources[i].x, sources[i].y, sources[i].z,
				sources[i].type);
	}
	if(tx.failed) {
		quad_fluid_test_failed_after_writes = tx.write_count;
		quad_fluid_test_failed_chests = tx.chest_count;
		quad_fluid_test_failed_drops = tx.drop_count;
		quad_fluid_test_failed_lava_mix = tx.lava_mix_effects;
		quad_populate_tx_rollback(&tx);
		if(feature_rng != &w->hell_populate_rng)
			*feature_rng = feature_before;
		return false;
	}
	if(!quad_populate_tx_commit(&tx)) {
		quad_fluid_test_failed_after_writes = tx.write_count;
		quad_fluid_test_failed_chests = tx.chest_count;
		quad_fluid_test_failed_drops = tx.drop_count;
		quad_fluid_test_failed_lava_mix = tx.lava_mix_effects;
		quad_populate_tx_rollback(&tx);
		if(feature_rng != &w->hell_populate_rng)
			*feature_rng = feature_before;
		return false;
	}
	return true;
}
#endif

static int quad_height(void* ctx, int x, int z) {
	struct server_chunk* sc = quad_chunk((struct quad_ctx*)ctx, x, z);
	return sc ? sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE] : 0;
}

static int quad_skylight(void* ctx, int x, int y, int z) {
	if(y >= WORLD_HEIGHT)
		return 15; // full daylight above the world
	if(y < 0)
		return 0;
	struct server_chunk* sc = quad_chunk((struct quad_ctx*)ctx, x, z);
	return sc ? nibble_read(sc->lighting_sky, S_CHUNK_IDX(x, y, z)) : 0;
}

static bool quad_chunks_exist(void* ctx, int x1, int y1, int z1, int x2, int y2, int z2) {
	struct quad_ctx* q = ctx;
	if(y2 < 0 || y1 >= WORLD_HEIGHT) return false;
	for(int cx = WCOORD_CHUNK_OFFSET(x1); cx <= WCOORD_CHUNK_OFFSET(x2); cx++)
		for(int cz = WCOORD_CHUNK_OFFSET(z1); cz <= WCOORD_CHUNK_OFFSET(z2); cz++)
			if(!server_world_chunk_resident(q->pop_world, cx, cz)) return false;
	return true;
}

static int quad_full_light(void* ctx, int x, int y, int z) {
	struct quad_ctx* q = ctx;
	struct block_data b;
	if(!server_world_get_block(q->pop_world, x, y, z, &b)) return 0;
	return b.sky_light > b.torch_light ? b.sky_light : b.torch_light;
}

// Register a freshly generated dungeon chest's loot into the local server's
// container store (the chest_pos/chest_items arrays the place handler fills and
// complex_block_archive persists). The raw quad_set above already wrote the
// BLOCK_CHEST id; this stocks its inventory WITHOUT server_world_set_block (no
// CRPC, no lighting -- the chunk has not been sent). Idempotent by coordinate.
static void quad_apply_chest(struct server_local* s, struct server_world* w,
							 int x, int y, int z,
							 const struct item_data* items) {
	if(!s || !w)
		return; // a world-view without a container store

	/* A1b: register in the POPULATED world's rows only -- a same-coordinate
	 * chest the other world owns is never overwritten. Same first-match /
	 * first-free order as before, so single-world slot assignment is
	 * unchanged. */
	struct te_store st = server_world_tile_store(w);
	int slot = te_store_claim(&st, TE_KIND_CHEST, x, y, z, NULL);
	if(slot < 0) {
		puts("Too many chests (dungeon loot dropped)");
		return;
	}

	memset(&s->chest_items[slot], 0, sizeof(s->chest_items[slot]));
	for(int j = 0; j < DUNGEON_CHEST_SLOTS; j++)
		s->chest_items[slot][j] = items[j];
}

static void quad_set_chest(void* ctx, int x, int y, int z,
						   const struct item_data* items) {
	struct quad_ctx* q = ctx;
	if(q->tx && q->tx->active) {
		if(!quad_populate_tx_stage_chest(q->tx, x, y, z, items))
			quad_populate_tx_fail(q->tx);
		return;
	}
	quad_apply_chest(q->sl, q->pop_world, x, y, z, items);
}

// FH-17: register a freshly generated dungeon spawner's MobSpawner tile
// entity into the local server's spawner store (server_spawner.c) --
// quad_apply_chest's counterpart. The raw quad_set above already wrote the
// BLOCK_MOB_SPAWNER id (metadata 0, faithful); this materialises its
// EntityId/Delay WITHOUT server_world_set_block (no CRPC, no lighting -- the
// chunk has not been sent). Idempotent by coordinate.
static void quad_apply_spawner(struct server_local* s, struct server_world* w,
							   int x, int y, int z, const char* mob_id) {
	if(!s || !w)
		return; // a world-view without a spawner store (unit tests/tools)
	if(!spawner_te_upsert_w(w, x, y, z, mob_id))
		fprintf(stderr, "Too many mob spawners (dungeon spawner TE dropped)\n");
}

static void quad_set_spawner(void* ctx, int x, int y, int z,
							 const char* mob_id) {
	struct quad_ctx* q = ctx;
	if(q->tx && q->tx->active) {
		if(!quad_populate_tx_stage_spawner(q->tx, x, y, z, mob_id))
			quad_populate_tx_fail(q->tx);
		return;
	}
	quad_apply_spawner(q->sl, q->pop_world, x, y, z, mob_id);
}

static bool quad_populate_tx_commit(struct quad_populate_tx* tx) {
	assert(tx && tx->active && !tx->failed && tx->q);
	/* Detached EntityItems and their three World.rand-derived offsets already
	 * exist, but dictionary/live-tick insertion capacity does not. Preflight both
	 * while every block/World.rand side effect remains rollbackable. A successful
	 * reservation makes the constructor-Math draws and publication loop below
	 * allocation-free and infallible. */
	if(!server_local_reserve_planned_drop_batch(tx->sl, tx->drop_count)) {
		quad_populate_tx_fail(tx);
		return false;
	}
	/* No later generation step observes container inventories or item entities.
	 * Publish only after every action and journal allocation has succeeded. */
	tx->active = false;
	for(size_t i = 0; i < tx->chest_count; i++) {
		const struct quad_populate_chest* chest = &tx->chests[i];
		quad_apply_chest(tx->sl, tx->world, chest->x, chest->y, chest->z,
						 chest->items);
	}
	for(size_t i = 0; i < tx->spawner_count; i++) {
		const struct quad_populate_spawner* sp = &tx->spawners[i];
		quad_apply_spawner(tx->sl, tx->world, sp->x, sp->y, sp->z,
						   sp->mob_id);
	}
	fluid_block_lava_mix_effect_commit_staged(tx->lava_mix_effects);
	tx->q->tx = NULL;
	/* The raw drop entities are already owned. Detach their compact records, free
	 * the now-unneeded quadrant buffers, then consume the independent EntityItem
	 * constructor Math stream and publish in original order without allocation. */
	struct server_local_planned_drop* drops = tx->drops;
	size_t drop_count = tx->drop_count;
	size_t drop_capacity = tx->drop_capacity;
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
	quad_populate_test_committed_writes += tx->write_count;
#endif
	tx->drops = NULL;
	tx->drop_count = tx->drop_capacity = 0;
	struct server_world* drop_world = tx->world;
	quad_populate_tx_release(tx);
	server_local_commit_planned_drop_batch(drop_world, drops, drop_count);
	mem2_free_sized(drops, drop_capacity * sizeof(*drops));
	return true;
}

// Flood block light from every light source the populate pass placed across the
// quadrant (lava springs glow at luminance 15) via the runtime lighting BFS. The
// world-coord callbacks cross the quadrant's chunk seams natively and stop at
// unloaded neighbours -- Beta's lazy edge convergence. Skylight is already set
// per column by worldgen_lighting; this only adds block light.
/* Literal-NULL specialization removes all per-cell observer work when
 * dormant; always_inline makes that a code-generation contract on PC/Wii GCC. */
/* Call-owned scratch: four 32768-cell columns = 16384 bytes maximum.
 * malloc is MEM2 (platform/mem2.c MALLOC_MEM2), outside the carve. No new
 * lock. Count overwrites every bit without a clearing scan. This preserves
 * Beta 740c583 Chunk.generateSkylightMap/propagateSkylightOcclusion values. */
enum {
	LIGHT_MEMO_COLUMN_CELLS = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT,
	LIGHT_MEMO_COLUMN_BYTES = LIGHT_MEMO_COLUMN_CELLS / 8,
	LIGHT_MEMO_COLUMNS = 4
};
_Static_assert(CHUNK_SIZE == 16 && WORLD_HEIGHT == 128,
	"Relight memo indices require 16x16x128 columns");
_Static_assert(LIGHT_MEMO_COLUMN_CELLS % 8 == 0
	&& LIGHT_MEMO_COLUMNS * LIGHT_MEMO_COLUMN_BYTES == 16384,
	"Relight memo must fit the 16 KiB bound");
static unsigned light_memo_test_mode;
void (*server_world_test_light_memo_output)(struct server_world*,
	const struct world_modification_entry*, size_t, unsigned);
void server_world_test_light_memo_mode(unsigned mode) {
	light_memo_test_mode = mode; /* quiescent: 0 on, 1 off, 2 OOM, 4 source OOM */
}
static uint8_t* light_memo_alloc(unsigned columns, struct tps_scan* scan) {
	if(light_memo_test_mode == 1) return NULL;
	if(columns > LIGHT_MEMO_COLUMNS || light_memo_test_mode == 2) {
		scan->memo_fallback = true;
		return NULL;
	}
	uint8_t* bits = malloc(columns * LIGHT_MEMO_COLUMN_BYTES);
	scan->memo_fallback = !bits;
	return bits;
}
static inline void light_memo_record(uint8_t* bits, unsigned i, bool source) {
	uint8_t mask = (uint8_t)(1u << (i & 7));
	/* Both count traversals visit physical indices monotonically. */
	if(!(i & 7)) bits[i >> 3] = 0;
	if(source) bits[i >> 3] |= mask;
}
static inline bool light_memo_read(const uint8_t* bits, unsigned i) {
	return (bits[i >> 3] & (1u << (i & 7))) != 0;
}

/* Quiescent boundary probe uses the production allocator and bit operations.
 * Recompute reads the input again, just like the fill fallback. */
bool server_world_test_light_memo_cells(const uint8_t* ids, unsigned columns,
	uint8_t* consumed, bool* fallback) {
	struct tps_scan scan = {0};
	uint8_t* memo = light_memo_alloc(columns, &scan);
	unsigned cells = columns * LIGHT_MEMO_COLUMN_CELLS;
	if(memo) for(unsigned i = 0; i < cells; i++)
		light_memo_record(memo, i, blocks[ids[i]] && blocks[ids[i]]->luminance > 0);
	for(unsigned i = 0; i < cells; i++)
		consumed[i] = memo ? light_memo_read(memo, i)
			: blocks[ids[i]] && blocks[ids[i]]->luminance > 0;
	bool used = memo != NULL;
	free(memo);
	*fallback = scan.memo_fallback;
	return used;
}

/* The server exclusively owns these base bytes. Neighbour installation and
 * edits execute on this same server thread; lighting lookups only resolve/read.
 * Client materialisation/writes use differential/private views. Thus no base
 * mutation intervenes between count and fill; stripes do not make them atomic. */
static inline __attribute__((always_inline)) size_t census_block_count_kernel(
	struct quad_ctx* q, struct tps_scan* scan, uint8_t* memo) {
	size_t n = 0;
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++) {
			struct server_chunk* sc = q->c[dx][dz];
			for(int i = 0; i < CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT; i++) {
				bool source = blocks[sc->ids[i]] && blocks[sc->ids[i]]->luminance > 0;
				if(memo) light_memo_record(memo, (dz * 2 + dx) * LIGHT_MEMO_COLUMN_CELLS + i, source);
				if(scan) tps_scan_count(scan, (dz * 2 + dx) * LIGHT_MEMO_COLUMN_CELLS + i, source);
				if(source) n++;
			}
		}
	return n;
}

static inline __attribute__((always_inline)) size_t census_block_fill_kernel(
	struct quad_ctx* q, struct world_modification_entry* sources, size_t n,
	struct tps_scan* scan, const uint8_t* memo) {
	size_t k = 0;
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++) {
			struct server_chunk* sc = q->c[dx][dz];
			w_coord_t bx = (q->px + dx) * CHUNK_SIZE;
			w_coord_t bz = (q->pz + dz) * CHUNK_SIZE;
			for(int lx = 0; lx < CHUNK_SIZE; lx++)
				for(int lz = 0; lz < CHUNK_SIZE; lz++)
					for(int y = 0; y < WORLD_HEIGHT; y++) {
						w_coord_t x = bx + lx, z = bz + lz;
						uint8_t id = sc->ids[S_CHUNK_IDX(x, y, z)];
						bool source = memo ? light_memo_read(memo,
							(dz * 2 + dx) * LIGHT_MEMO_COLUMN_CELLS + S_CHUNK_IDX(x, y, z))
							: blocks[id] && blocks[id]->luminance != 0;
						if(scan) {
							scan->consumed_cells++;
							scan->consumed_sources += source;
						}
						if(scan && !memo) tps_scan_fill(scan,
							(dz * 2 + dx) * LIGHT_MEMO_COLUMN_CELLS + S_CHUNK_IDX(x, y, z), source);
						if(!source) continue;
						if(k < n)
							sources[k++] = (struct world_modification_entry) {
								.x = x,
								.y = y,
								.z = z,
								.blk = (struct block_data) {.type = id}};
					}
		}

	return k;
}

static void server_world_relight_quadrant_blocklight(struct server_world* w,
													  struct quad_ctx* q) {
	// Collect every luminous block (lava, glowstone, ...) in the 4 quadrant
	// chunks, then flood their light in ONE multi-source pass. The old code
	// flooded once PER luminous block -- O(luminous x area) -- which froze gen on
	// lava-heavy chunks (a worldgen lava lake is hundreds of overlapping sources).
	// A single multi-source flood reaches the identical light field at O(area).
	tps_scan_callback census = __atomic_load_n(&world_tps_scan, __ATOMIC_ACQUIRE);
	tps_light_callback light_census = __atomic_load_n(&world_tps_light, __ATOMIC_ACQUIRE);
	if(light_census) {
		struct tps_light_input in = {.world = w, .count = 4, .kind = 1,
			.x = q->px, .z = q->pz, .dimension = w->dimension};
		for(unsigned i = 0; i < 4; i++) in.columns[i] = q->c[i % 2][i / 2]->ids;
		light_census(&in);
	}
	struct tps_scan scan = {.kind = 0};
	if(census) census(TPS_SCAN_BEGIN, &scan);
	uint8_t* memo = light_memo_alloc(4, &scan);
	if(census && scan.memo_fallback) census(TPS_SCAN_MEMO_FALLBACK, &scan);
	size_t n = scan.bits ? census_block_count_kernel(q, &scan, memo)
		: census_block_count_kernel(q, NULL, memo);
	if(census) census(TPS_SCAN_COUNT_END, &scan);
	if(n == 0) {
		free(memo);
		return;
	}

	struct world_modification_entry* sources
		= light_memo_test_mode == 4 && memo ? NULL
		: malloc(n * sizeof(struct world_modification_entry));
	if(!sources && memo) {
		free(memo);
		memo = NULL;
		scan.memo_fallback = true;
		if(census) census(TPS_SCAN_MEMO_FALLBACK, &scan);
		sources = malloc(n * sizeof(*sources));
	}
	if(!sources)
		return; /* Known P2: allocation failure leaves block light incomplete.
				  Atomic shadow-light retry is tracked separately from populate. */

	if(census) census(TPS_SCAN_FILL_BEGIN, &scan);
	size_t k = scan.bits ? census_block_fill_kernel(q, sources, n, &scan, memo)
		: census_block_fill_kernel(q, sources, n, NULL, memo);

	scan.memoised = memo != NULL;
	free(memo);
	if(census) census(TPS_SCAN_END, &scan);
	lighting_update_at_blocks_server(sources, k, w->dimension == WORLD_DIM_NETHER,
							  server_world_light_get_block,
							  server_world_light_set_light, w);
	if(server_world_test_light_memo_output)
		server_world_test_light_memo_output(w, sources, k, 0);
	free(sources);
}

// D1 (skylight occlusion). worldgen_lighting sets each column's skylight TOP-DOWN
// with NO horizontal bleed -- so a cell tucked under an overhang or just inside a
// cave mouth reads sky 0 even when open sky is one block to the side, and hostiles
// spawn there at noon (eff sky 0). Beta fixes this with propagateSkylightOcclusion
// + the general light BFS; here we seed the SAME runtime flood from the "under-lit"
// frontier so it converges to Beta's fixed point.
//
// A cell is a frontier seed iff it is transparent, below full sky (sky < 15), and a
// neighbour could raise it: neighbour.sky - max(opacity,1) > this.sky -- the exact
// condition lighting_flood would use. Seeding these and running ONE multi-source
// flood bleeds skylight horizontally inward (the flood pushes the next-inner dark
// cell, which recomputes higher, and so on). The current cell is read straight from
// the owning chunk; only the 6 neighbours go through the world-coord accessor so
// seam neighbours resolve and the bleed crosses loaded chunk borders (stopping at
// unloaded ones -- Beta's lazy edge convergence, same as the block-light flood).
//
// SAFETY (this is the far-ring-crash class -- a relight cascade): horizontal skylight
// attenuates >= 1 per block, so a bleed dies within 14 cells of any sky-15 donor. It
// physically CANNOT run away across a region the way the old synthetic-far-section
// inconsistency did; the LIGHTING_FLOOD_MAX_POPS ceiling + lfc counter remain as the
// backstop. EQUIVALENCE: an open column has no transparent cell below its heightmap,
// so it is never seeded and its skylight is byte-unchanged (the flood's fixed point
// equals the vertical pass there) -- proven in tests/test_skylight_occlusion.c.
static bool server_world_cell_skylight_underlit(struct server_world* w,
												struct server_chunk* sc,
												w_coord_t bx, w_coord_t bz, int lx,
												int y, int lz) {
	size_t idx = S_CHUNK_IDX(bx + lx, y, bz + lz);
	uint8_t id = sc->ids[idx];
	int sky = nibble_read(sc->lighting_sky, idx);

	// Cheap reject BEFORE the 6 neighbour fetches: a strict superset of
	// skylight_cell_underlit's accept set (opaque or full-sky cells can never be
	// seeds), so skipping these never drops a real frontier cell. The authoritative
	// decision is the shared skylight_cell_underlit below (no drift vs the test).
	if((blocks[id] && !blocks[id]->can_see_through) || sky >= 15)
		return false;

	int neighbour_sky[SIDE_MAX];
	int nc = 0;
	w_coord_t x = bx + lx, z = bz + lz;
	for(enum side s = 0; s < SIDE_MAX; s++) {
		int dx, dy, dz;
		blocks_side_offset(s, &dx, &dy, &dz);

		struct block_data nb;
		if(server_world_light_get_block(w, x + dx, y + dy, z + dz, &nb, NULL))
			neighbour_sky[nc++] = nb.sky_light;
	}
	return skylight_cell_underlit(id, sky, neighbour_sky, nc);
}

// Run the horizontal skylight bleed over `nc` freshly-(re)lit chunks. Two passes
// (count, then fill an exact-sized array -- exclusive server base-byte ownership
// keeps the field stable, as documented at the block count/fill site above)
// so there is no realloc churn, then ONE multi-source flood. NOTE: this must run
// AFTER every worldgen_lighting (vertical) pass that touches a chunk, because that
// pass overwrites per-cell skylight with vertical-only values -- it wipes any prior
// bleed, exactly as the block-light flood re-adds block light after it.
//
// SEAM CAVEAT (known, cosmetic): the flood's set_light writes through the world-coord
// accessor, so a bleed seeded here can propagate <=14 cells across a loaded chunk
// border and brighten cells in a neighbour that was ALREADY SENT to the client (e.g. a
// later-decorated chunk carves a lake that opens a roofed seam column). There is no
// resend path, so the client keeps the darker seam until that chunk reloads (its
// ->modified is set, so a reload heals it). This is render-only: server-side mob-spawn
// light reads the corrected (brighter) values, so the D1 anti-noon-spawn guarantee
// holds. It matches Beta's own lazy edge convergence and the pre-existing block-light
// flood's identical cross-seam behaviour; a resend-dirty-neighbour pass is the clean
// future fix if the seam ever proves visible.
static inline __attribute__((always_inline)) size_t census_sky_count_kernel(
	struct server_world* w, struct server_chunk* const* chunks,
	const w_coord_t* bx, const w_coord_t* bz, int nc, struct tps_scan* scan, uint8_t* memo) {
	size_t n = 0;
	for(int ci = 0; ci < nc; ci++)
		for(int lx = 0; lx < CHUNK_SIZE; lx++)
			for(int lz = 0; lz < CHUNK_SIZE; lz++)
				for(int y = 0; y < WORLD_HEIGHT; y++) {
					bool source = server_world_cell_skylight_underlit(w,
						chunks[ci], bx[ci], bz[ci], lx, y, lz);
					if(memo) light_memo_record(memo,
						ci * LIGHT_MEMO_COLUMN_CELLS + S_CHUNK_IDX(lx, y, lz), source);
					if(scan) tps_scan_count(scan,
						ci * LIGHT_MEMO_COLUMN_CELLS + S_CHUNK_IDX(lx, y, lz), source);
					if(source) n++;
				}
	return n;
}

static inline __attribute__((always_inline)) size_t census_sky_fill_kernel(
	struct server_world* w, struct server_chunk* const* chunks,
	const w_coord_t* bx, const w_coord_t* bz, int nc, struct world_modification_entry* sources,
	size_t n, struct tps_scan* scan, const uint8_t* memo) {
	size_t k = 0;
	for(int ci = 0; ci < nc && k < n; ci++)
		for(int lx = 0; lx < CHUNK_SIZE && k < n; lx++)
			for(int lz = 0; lz < CHUNK_SIZE && k < n; lz++)
				for(int y = 0; y < WORLD_HEIGHT && k < n; y++) {
					bool source = memo ? light_memo_read(memo,
						ci * LIGHT_MEMO_COLUMN_CELLS + S_CHUNK_IDX(lx, y, lz))
						: server_world_cell_skylight_underlit(w,
						chunks[ci], bx[ci], bz[ci], lx, y, lz);
					if(scan) {
						scan->consumed_cells++;
						scan->consumed_sources += source;
					}
					if(scan && !memo) tps_scan_fill(scan,
						ci * LIGHT_MEMO_COLUMN_CELLS + S_CHUNK_IDX(lx, y, lz), source);
					if(source) sources[k++] = (struct world_modification_entry) {
						.x = bx[ci] + lx, .y = y, .z = bz[ci] + lz};
				}

	return k;
}

static void server_world_skylight_bleed(struct server_world* w,
										struct server_chunk* const* chunks,
										const w_coord_t* bx, const w_coord_t* bz,
										int nc) {
	if(w->dimension == WORLD_DIM_NETHER)
		return; // the nether has no skylight to bleed

	tps_scan_callback census = __atomic_load_n(&world_tps_scan, __ATOMIC_ACQUIRE);
	tps_light_callback light_census = __atomic_load_n(&world_tps_light, __ATOMIC_ACQUIRE);
	if(light_census) {
		struct tps_light_input in = {.world = w, .count = nc, .kind = 2,
			.x = bx[0], .z = bz[0], .dimension = w->dimension};
		for(int i = 0; i < nc; i++) in.columns[i] = chunks[i]->ids;
		light_census(&in);
	}
	struct tps_scan scan = {.kind = 1};
	if(census) census(TPS_SCAN_BEGIN, &scan);
	uint8_t* memo = light_memo_alloc(nc, &scan);
	if(census && scan.memo_fallback) census(TPS_SCAN_MEMO_FALLBACK, &scan);
	size_t n = scan.bits ? census_sky_count_kernel(w, chunks, bx, bz, nc, &scan, memo)
		: census_sky_count_kernel(w, chunks, bx, bz, nc, NULL, memo);
	if(census) census(TPS_SCAN_COUNT_END, &scan);
	if(n == 0) {
		free(memo);
		return;
	} // no overhang/cave-mouth frontier -> nothing to bleed

	struct world_modification_entry* sources
		= light_memo_test_mode == 4 && memo ? NULL
		: malloc(n * sizeof(struct world_modification_entry));
	if(!sources && memo) {
		free(memo);
		memo = NULL;
		scan.memo_fallback = true;
		if(census) census(TPS_SCAN_MEMO_FALLBACK, &scan);
		sources = malloc(n * sizeof(*sources));
	}
	if(!sources)
		return; /* Known P2: this can affect spawn light, not merely rendering.
				  A checked shadow-light transaction is the required full fix. */

	if(census) census(TPS_SCAN_FILL_BEGIN, &scan);
	size_t k = scan.bits ? census_sky_fill_kernel(w, chunks, bx, bz, nc, sources, n, &scan, memo)
		: census_sky_fill_kernel(w, chunks, bx, bz, nc, sources, n, NULL, memo);

	scan.memoised = memo != NULL;
	free(memo);
	if(census) census(TPS_SCAN_END, &scan);
	lighting_update_at_blocks_server(sources, k, false /* overworld: skylight matters */,
							  server_world_light_get_block,
							  server_world_light_set_light, w);
	if(server_world_test_light_memo_output)
		server_world_test_light_memo_output(w, sources, k, 1);
	free(sources);
}

bool server_world_test_relight_columns(struct server_world* w, w_coord_t x, w_coord_t z) {
	struct quad_ctx q = {.px = x, .pz = z};
	struct server_chunk* columns[4];
	w_coord_t bx[4], bz[4];
	for(unsigned i = 0; i < 4; i++) {
		w_coord_t cx = x + (int)(i % 2), cz = z + (int)(i / 2);
		columns[i] = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		if(!columns[i]) return false;
		q.c[i % 2][i / 2] = columns[i];
		bx[i] = cx * CHUNK_SIZE;
		bz[i] = cz * CHUNK_SIZE;
	}
	for(unsigned i = 0; i < 4; i++) server_world_rebuild_lighting(columns[i], w->dimension);
	server_world_relight_quadrant_blocklight(w, &q);
	server_world_skylight_bleed(w, columns, bx, bz, 4);
	return true;
}

// Run populate() for the quadrant whose origin chunk is (px,pz), once. Ensures
// the whole 2x2's terrain exists first, then decorates across all four chunks.
// Returns true iff it actually ran the full populate+relight this call (false for
// every early-return: already-populated/disk, budget-deferred, load-fail, superflat).
// ensure_populated uses this to skip its final cross-chunk relight when NOTHING was
// populated -- e.g. an all-disk near region, whose chunks already carry saved lighting,
// so re-running worldgen_lighting on each would be pure wasted work (the disk-load spike).
/* The production populate view over one quadrant context: every decorate
 * writer and callback (BlockLeaves/BlockLog.onBlockRemoval through q->view)
 * sees exactly these hooks. Shared with the G9X-2 setter-script seam. */
static void quad_view_init(struct wgen_world* view, struct quad_ctx* q) {
	*view = (struct wgen_world) {
		.ctx = q,
		.get = quad_get,
		.set = quad_set,
		.set_plain = quad_set_plain,
		.height = quad_height,
		.skylight = quad_skylight,
		.set_chest = quad_set_chest,
		.tick_fluid_immediate = quad_fluid_tick_immediate,
		.set_spawner = quad_set_spawner,
		.get_meta = quad_get_meta,
		.full_light = quad_full_light,
		.set_meta = quad_set_meta,
		.chunks_exist = quad_chunks_exist,
		.set_plain_notify = quad_set_plain_notify,
	};
	q->view = view;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
#define G9X_POP_PHASE(bit) (s_pop_fault_phase = (bit))
#else
#define G9X_POP_PHASE(bit) ((void)0)
#endif

/* G9X-4: the snow loop is the END of ChunkProviderGenerate.populate (CFR
 * ChunkProviderGenerate.java:553-563, before BlockSand.fallInstantly=false
 * at :564), so each World.setBlockWithNotify (World.java:332-337) and its
 * neighbour notifications -- BlockStationary/BlockFlowing wake-ups
 * (BlockFlowing.java:157-187), BlockSand rescheduling (BlockSand.java:44-57)
 * through World.scheduleUpdateTick (World.java:827-843) -- happen inside
 * population. CaveWii's population transaction (rollback is CaveWii
 * infrastructure) therefore stays alive until the last snow write and its
 * last notification: a region, fluid-driver or outside-chunk record
 * allocation failure anywhere in them fails the transaction, the whole
 * population rolls back and is deferred (never a quadrant marked populated
 * with snow or notifications missing). Only then are the deferred
 * containers/items/effect counters committed. temp_grid NULL = no snow pass
 * (Nether / no generator). Relight's separate source/flood allocations are
 * still best-effort (documented P2 above), outside this transaction. */
static bool quad_populate_snow_and_commit(struct quad_populate_tx* tx,
	struct wgen_world* view, const double* temp_grid) {
	struct quad_ctx* q = tx->q;
	if(temp_grid && !tx->failed) {
		G9X_POP_PHASE(SERVER_WORLD_TEST_POP_PHASE_SNOW);
		worldgen_snow(view, temp_grid, q->px * 16, q->pz * 16);
		G9X_POP_PHASE(0);
	}
	if(tx->failed || !quad_populate_tx_commit(tx)) {
		quad_populate_tx_rollback(tx);
		return false;
	}
	return true;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static void quad_test_setter_op(struct quad_ctx* q,
	const struct server_world_test_setter_op* op) {
	switch(op->api) {
		case SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA:
			quad_set(q, op->x, op->y, op->z, op->id, op->meta);
			break;
		case SERVER_WORLD_TEST_SET_BLOCK:
			quad_set_plain(q, op->x, op->y, op->z, op->id);
			break;
		case SERVER_WORLD_TEST_SET_BLOCK_METADATA:
			quad_set_meta(q, op->x, op->y, op->z, op->meta);
			break;
		default: assert(false && "unknown setter api");
	}
}

/* G9X-2 seam: run scripted World.setBlockAndMetadata / setBlock /
 * setBlockMetadata calls through the PRODUCTION quadrant writer, view and
 * removal callbacks inside one real populate transaction, then commit. */
bool server_world_test_populate_setter_script(struct server_world* w,
	w_coord_t px, w_coord_t pz, const struct server_world_test_setter_op* ops,
	size_t count) {
	assert(w && (count == 0 || ops));
	struct quad_ctx q;
	if(!quad_fluid_test_ctx(w, px, pz, &q))
		return false;
	struct quad_populate_tx tx;
	if(!quad_populate_tx_begin(&tx, &q, w))
		return false;
	struct wgen_world view;
	quad_view_init(&view, &q);
	for(size_t i = 0; i < count && !tx.failed; i++)
		quad_test_setter_op(&q, &ops[i]);
	if(tx.failed || !quad_populate_tx_commit(&tx)) {
		quad_populate_tx_rollback(&tx);
		return false;
	}
	return true;
}

/* G9X-4 seams. The fault counter is armed for the given phases only. */
void server_world_test_populate_fault(unsigned phase_mask, long fail_at) {
	/* Arming (a nonzero mask) restarts the count; disarming keeps it. */
	if(phase_mask)
		s_pop_fault_points = 0;
	s_pop_fault_mask = phase_mask;
	s_pop_fault_at = fail_at;
}

size_t server_world_test_populate_fault_points(void) {
	return s_pop_fault_points;
}

/* Scripted decorate-stage World.setBlockWithNotify writes through the
 * PRODUCTION notifying view hook, then the PRODUCTION snow-and-commit tail,
 * in one real population transaction. */
bool server_world_test_populate_snow_script(struct server_world* w,
	w_coord_t px, w_coord_t pz, const struct server_world_test_notify_op* ops,
	size_t count, const double* temp_grid, bool* committed) {
	assert(w && (count == 0 || ops) && committed);
	*committed = false;
	struct quad_ctx q;
	if(!quad_fluid_test_ctx(w, px, pz, &q))
		return false;
	struct quad_populate_tx tx;
	if(!quad_populate_tx_begin(&tx, &q, w))
		return false;
	struct wgen_world view;
	quad_view_init(&view, &q);
	G9X_POP_PHASE(SERVER_WORLD_TEST_POP_PHASE_DECORATE);
	for(size_t i = 0; i < count && !tx.failed; i++)
		view.set_plain_notify(&q, ops[i].x, ops[i].y, ops[i].z, ops[i].id);
	G9X_POP_PHASE(0);
	if(tx.failed) {
		quad_populate_tx_rollback(&tx);
		return true;
	}
	*committed = quad_populate_snow_and_commit(&tx, &view,
		w->dimension == WORLD_DIM_OVERWORLD ? temp_grid : NULL);
	return true;
}

/* The production population entry point for one quadrant. */
bool server_world_test_populate_quadrant(struct server_world* w,
	w_coord_t px, w_coord_t pz) {
	return server_world_populate_quadrant(w, w->local, px, pz);
}

bool server_world_test_populate_setter_script_fail(struct server_world* w,
	w_coord_t px, w_coord_t pz, const struct server_world_test_setter_op* ops,
	size_t count, size_t fail_before, size_t* writes_at_fail,
	size_t* writes_after, server_world_test_setter_observe_fn observe,
	void* observe_ctx) {
	assert(w && (count == 0 || ops) && writes_at_fail && writes_after);
	struct quad_ctx q;
	if(!quad_fluid_test_ctx(w, px, pz, &q))
		return false;
	struct quad_populate_tx tx;
	if(!quad_populate_tx_begin(&tx, &q, w))
		return false;
	struct wgen_world view;
	quad_view_init(&view, &q);
	*writes_at_fail = 0;
	for(size_t i = 0; i < count; i++) {
		if(i == fail_before) {
			quad_populate_tx_fail(&tx);
			*writes_at_fail = tx.write_count;
		}
		quad_test_setter_op(&q, &ops[i]);
		/* The live world after op i, BEFORE any rollback: a failed write
		 * that mutated and was later undone by the rollback is visible here. */
		if(observe)
			observe(w, i, tx.write_count, observe_ctx);
	}
	if(fail_before >= count) {
		quad_populate_tx_fail(&tx);
		*writes_at_fail = tx.write_count;
	}
	*writes_after = tx.write_count;
	quad_populate_tx_rollback(&tx);
	return true;
}
#endif

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* G9X-2 read-only view of the ordered generation light queue (channel is
 * COLUMN_SKY/COLUMN_TORCH, then the six bounds). */
/* G9X-5 bounded-queue seams. */
void server_world_test_light_queue_stats(const struct server_world* w,
	struct server_world_light_queue_stats* out) {
	assert(out);
	memset(out, 0, sizeof *out);
	if(w) {
		out->resident_bytes = sbl_resident_bytes(w);
		out->meta_bytes = (w->generation_light_pages
			? w->generation_light_slots * sizeof *w->generation_light_pages : 0)
			+ (w->generation_light_backing ? sizeof *w->generation_light_backing : 0);
		out->spills = w->generation_light_stat[0];
		out->loads = w->generation_light_stat[1];
		out->peak_logical = w->generation_light_stat[2];
		out->spilled_pages = w->generation_light_base;
		out->file_pages = w->generation_light_backing
			? w->generation_light_backing->file_pages : 0;
	}
	out->subsystem_bytes = s_sbl_bytes;
	out->subsystem_peak = s_sbl_peak;
	out->world_resident_peak = s_sbl_world_peak;
	out->ckpt_peak = s_sbl_ckpt_peak;
	out->wii_bytes = s_sbl_wii_bytes;
	out->wii_peak = s_sbl_wii_peak;
	out->storage_failures = s_sbl_storage_fail;
	out->alloc_failures = s_sbl_alloc_fail;
	out->last_fail_site = s_sbl_fail_site;
	out->last_fail_cause = s_sbl_fail_cause;
}
void server_world_test_light_queue_stats_reset(void) {
	s_sbl_peak = s_sbl_bytes;
	s_sbl_wii_peak = s_sbl_wii_bytes;
	s_sbl_world_peak = s_sbl_ckpt_peak = 0;
	s_sbl_fail_site = s_sbl_fail_cause = 0;
}
void server_world_test_light_queue_force_spill(bool on) {
	s_sbl_force_spill = on;
}
void server_world_test_gen_storage(const struct server_world* w,
	struct server_world_test_gen_storage* out) {
	out->failed = w->gen_storage_failed;
	out->cause = w->gen_storage_cause;
	out->site = w->gen_storage_site;
	out->x = (int)w->gen_storage_x;
	out->z = (int)w->gen_storage_z;
	out->io_fails = w->generation_light_io_fails;
}
bool server_world_test_light_queue_backing_page(const struct server_world* w,
	size_t page, unsigned phys_slot, void* out, int* loc, bool* flipped, bool* trial) {
	return sbl_test_page(w, page, phys_slot, out, loc, flipped, trial);
}
bool server_world_test_light_queue_slots(struct server_world* w, uint32_t slots) {
	assert(w && slots >= 2); /* a spill keeps the newest five: two pages minimum */
	if(w->generation_light_n || w->generation_light_base) return false;
	const size_t stat[3] = {w->generation_light_stat[0], w->generation_light_stat[1],
		w->generation_light_stat[2]};
	sbl_free(w);
	memcpy(w->generation_light_stat, stat, sizeof stat);
	w->generation_light_slots = slots;
	return true;
}
void server_world_test_light_queue_fault(enum server_world_test_lq_fault kind, long at) {
	assert(kind < SERVER_WORLD_TEST_LQ_FAULTS);
	s_sbl_fault_at[kind] = at;
	s_sbl_fault_n[kind] = 0;
}
size_t server_world_test_light_queue_fault_count(enum server_world_test_lq_fault kind) {
	assert(kind < SERVER_WORLD_TEST_LQ_FAULTS);
	return s_sbl_fault_n[kind];
}
bool server_world_test_generation_light_schedule(struct server_world* w,
	int channel, int x1, int y1, int z1, int x2, int y2, int z2) {
	return server_beta_light_schedule(w, channel, x1, y1, z1, x2, y2, z2);
}
const char* server_world_test_light_queue_backing_path(const struct server_world* w) {
	return w->generation_light_backing ? w->generation_light_backing->path : "";
}

bool server_world_test_generation_light_region(const struct server_world* w,
	size_t i, int out[7]) {
	assert(w && out);
	struct server_beta_light_region r;
	if(!sbl_peek(w, i, &r))
		return false;
	const struct server_beta_light_region* m = &r;
	out[0] = m->channel;
	out[1] = m->x1; out[2] = m->y1; out[3] = m->z1;
	out[4] = m->x2; out[5] = m->y2; out[6] = m->z2;
	return true;
}
#endif

#ifdef CAVEWII_SERVER_LOCAL_TEST
unsigned server_world_test_population_calls;
#endif
/* G9X-5a POPULATION_STORAGE_FAILED (server_world.h gen_storage_failed). The
 * caller has already rolled back any transaction it began (arrays, flags,
 * named RNGs, the logical queue and its tail patches); only then is the
 * sticky stop latched, so no rollback can clear it. The first failure is
 * kept; the world's pending work and backing file stay owned until its
 * teardown; nothing retries it. The light stop raises the existing session
 * publication barrier (server_world_stop_simulation), which also pauses
 * the session's other world: the failure belongs to this world. */
static void server_world_population_storage_stop(struct server_world* w,
	w_coord_t x, w_coord_t z, uint8_t site) {
	if(!w->gen_storage_failed) {
		w->gen_storage_failed = true;
		w->gen_storage_cause = w->generation_light_io_cause;
		w->gen_storage_site = site;
		w->gen_storage_x = x;
		w->gen_storage_z = z;
		fprintf(stderr, "[light] world %u: population backing storage failed (cause %u, "
			"site %u) at (%d,%d); world stopped for good, pending work retained, "
			"nothing published\n", (unsigned)w->world_index, (unsigned)w->gen_storage_cause,
			(unsigned)site, (int)x, (int)z);
	} else if(w->light_fault && w->light_fault_reason == SERVER_LIGHT_FAULT_POPULATION_STORAGE)
		return; /* already stopped (a caller re-checking after a nested latch) */
	server_world_light_stop(w, SERVER_LIGHT_FAULT_POPULATION_STORAGE, 0);
}

static bool server_world_populate_quadrant_history_impl(struct server_world* w,
										   struct server_local* s, w_coord_t px,
										   w_coord_t pz) {
	struct server_chunk* origin
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(px, pz));
	if(origin && origin->populated)
		return false; // already decorated (incl. disk chunks) -- never re-run
	/* G9X-5a: a world stopped by a population backing-storage failure
	 * populates nothing, ever (sticky; no retry). */
	if(w->gen_storage_failed) {
		w->gen_deferred_window++;
		return false;
	}
	/* ONE backing-I/O baseline for this whole populate call: every check
	 * below compares against it (G9X-5a). */
	const size_t io_before = w->generation_light_io_fails;

	// Populate is now heavy (trees, big trees, springs, snow, AND the post-
	// populate relight below). Budget it on the same per-cycle clock as terrain
	// gen: if this cascade's time/count is already spent, defer the whole
	// quadrant -- it stays unpopulated, ensure_populated reports not-ready, and
	// the streaming loop retries it next cycle, converging with no render hitch.
	if(w->gen_throttle_armed && server_world_gen_budget_spent(w)) {
		w->gen_deferred_window++;
		return false;
	}

	// Make sure all four quadrant chunks have terrain (no-op if resident).
	/* G9X-5a: after EACH load, before the next load, pointer validation, an
	 * early return or any quadrant work: a backing I/O failure met by that
	 * load (its terrain generation -- latched there as SITE_TERRAIN -- or a
	 * nested population) is the sticky stop; this quadrant touched nothing. */
	struct server_chunk* dummy;
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++) {
			server_world_load_chunk(w, px + dx, pz + dz, &dummy);
			if(w->gen_storage_failed || w->generation_light_io_fails != io_before) {
				server_world_population_storage_stop(w, px, pz, SERVER_POPULATION_SITE_LOAD);
				w->gen_deferred_window++;
				return false;
			}
		}

	// Fetch the four pointers AFTER loading (load_chunk may rehash the dict).
	struct quad_ctx q = {.px = px, .pz = pz, .sl = s, .pop_world = w};
	for(int dz = 0; dz < 2; dz++) {
		for(int dx = 0; dx < 2; dx++) {
			q.c[dx][dz] = dict_server_chunks_get(
				w->chunks, S_CHUNK_ID(px + dx, pz + dz));
			if(!q.c[dx][dz])
				return false; // a quadrant chunk failed to load; retry next time
		}
	}

	// Re-check populated AFTER the loads: the early-out at the top only sees
	// chunks that were RESIDENT before this call, so a non-resident origin
	// that just disk-loaded with TerrainPopulated=1 would re-run its whole
	// populate -- re-stamping deterministic decorations over previously-sent
	// terrain (regrown player-cleared trees, reset dungeon-chest loot; the
	// 2026-06-12 persistence audit's side-find, routine on the first picks of
	// a reloaded session).
	if(q.c[0][0]->populated)
		return false;

	// Superflat debug stage: nothing to decorate (no trees/ores/snow). The flat
	// terrain + skylight are already produced by generate_chunk, so just mark the
	// quadrant populated -- mirroring the normal path below, which marks only the
	// origin chunk.
	if(w->superflat) {
		q.c[0][0]->populated = true;
		return false; // flat terrain: no decoration/spill, so no final relight needed
	}

	// Re-check the budget AFTER the terrain load: generating up to four missing
	// terrain chunks above can itself spend the frame budget, and the full
	// populate + snow + relight below would then overshoot the cascade. Defer if
	// spent (the quadrant stays unpopulated and is retried next cycle).
	if(w->gen_throttle_armed && server_world_gen_budget_spent(w)) {
		w->gen_deferred_window++;
		return false;
	}

	/* Immediate fluid updates can grow an explicit continuation stack after
	 * earlier decorations already changed all four chunks. Begin one logical
	 * transaction before the first decorate write so allocation failure can
	 * retry from the exact pre-populate world, never from a half-decorated one. */
	if(w->gen_throttle_armed)
		w->gen_attempts_window++;
	struct quad_populate_tx populate_tx;
	if(!quad_populate_tx_begin(&populate_tx, &q, w)) {
		/* G9X-5a: a backing I/O failure reloading the mergeable tail stops
		 * the world (nothing began); any other cause defers as before. */
		if(w->generation_light_io_fails != io_before)
			server_world_population_storage_stop(w, px, pz, SERVER_POPULATION_SITE_TX_BEGIN);
		w->gen_deferred_window++;
		return false;
	}

	struct wgen_world view;
	quad_view_init(&view, &q);

	// Tree count + type need the biome at the chunk's +16,+16 corner and the
	// treeDensity noise at its origin (Beta samples both from the generator).
	// Overworld-only -- the Nether decorate uses neither (ChunkProviderHell.populate
	// draws lava/fire/glowstone/mushrooms with no biome/tree input).
	enum biome corner_biome = BIOME_NONE;
	double tree_density = 0.0;
	if(w->generator && w->dimension != WORLD_DIM_NETHER) {
		corner_biome
			= worldgen_biome_at(w->generator, px * 16 + 16, pz * 16 + 16);
		tree_density = worldgen_tree_density(w->generator, px * 16, pz * 16);
	}

	tps_note(world_tps_observe, TPS_ENTER, TPS_DECORATE);
	ptime_t decorate_start = deep_start();
	if(w->dimension == WORLD_DIM_NETHER)
		// Beta ChunkProviderHell.populate: lava springs / fire / glowstone / brown
		// + red mushrooms, drawn from the shared world-global hellRNG (NO per-chunk
		// reseed -- so non-deterministic per chunk, a faithful Beta property).
		worldgen_hell_populate(&w->hell_populate_rng, px, pz, &view);
	else {
#ifdef CAVEWII_SERVER_LOCAL_TEST
		server_world_test_population_calls++;
#endif
		worldgen_populate(w->seed, px, pz, &view, corner_biome, tree_density);
	}
	deep_end(2, decorate_start);
	tps_note(world_tps_observe, TPS_LEAVE, TPS_DECORATE);
	if(populate_tx.failed) {
		quad_populate_tx_rollback(&populate_tx);
		/* G9X-5a: rolled back completely FIRST, then the sticky stop. */
		if(w->generation_light_io_fails != io_before)
			server_world_population_storage_stop(w, px, pz, SERVER_POPULATION_SITE_TX);
		w->gen_deferred_window++;
		return false;
	}
	// PACING (single-core Wii): this quadrant's populate + relight is the multi-hundred-ms
	// work that, at server==render priority, freezes the render thread until it completes.
	// Yield at each heavy sub-step so render can take a frame mid-cascade. PURE pacing -- it
	// changes only the wall-clock interleave, never a generated block/light value (the steps
	// run in the identical order and the chunk is still only sent once all 4 quadrants finish).
	thread_yield();

	// Snow pass: RNG-free, so running it after populate can't shift the stream.
	// Sample the +8 temperature field from the generator and lay snow layers on
	// cold exposed columns, then commit (quad_populate_snow_and_commit).
	double temp_grid[256];
	const bool snow = w->generator && w->dimension == WORLD_DIM_OVERWORLD;
	if(snow)
		worldgen_temperature_grid(w->generator, px * 16 + 8, pz * 16 + 8,
								  temp_grid);
	if(!quad_populate_snow_and_commit(&populate_tx, &view,
		   snow ? temp_grid : NULL)) {
		/* snow_and_commit rolled the transaction back (G9X-4) */
		if(w->generation_light_io_fails != io_before)
			server_world_population_storage_stop(w, px, pz, SERVER_POPULATION_SITE_SNOW);
		w->gen_deferred_window++;
		return false;
	}
	thread_yield();

	/* CFR ChunkProviderServer.java:157-160 and Chunk.java:208-276: populate
	 * retains each actual setter's immediate stores and deferred regions.
	 * MinecraftServer.java:346-348 drains at the next world boundary; there
	 * is no bulk height/Sky rebuild, emitter scan or synchronous flood here. */
	thread_yield();

	// Re-arm populate-placed scheduled-only tickers (principally fire). Fluid
	// springs already ran their exact immediate cascade inline through the view
	// hook above; moving fluid has an onNeighbour hook + random-tick callback, so
	// this load-style scan deliberately skips it rather than planting a second,
	// delayed update. Pace it like the other quadrant sub-passes.
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++) {
			/* Populate changed the scan contents, so begin a fresh cursor epoch.
			 * Only run it once the full union is resident; otherwise a checked
			 * per-cell rejection is indistinguishable from capacity pressure and
			 * would stop at an arbitrary boundary cell. Later chunk arrival resumes
			 * this epoch through the normal neighbourhood refresh. */
			q.c[dx][dz]->load_ticks_rearmed = false;
			q.c[dx][dz]->load_tick_rearm_cursor = 0;
			server_world_try_rearm_chunk_ticks(
				w, q.px + dx, q.pz + dz);
			thread_yield();
		}

	// Count this populate (with its relight) against the cascade budget; the
	// time check at the top is the real gate, this feeds the count backstop.
	w->gen_this_window++;

	// All four chunks' blocks changed -> persist them; only (px,pz)'s own
	// quadrant has run, so only it is marked populated.
	for(int dz = 0; dz < 2; dz++)
		for(int dx = 0; dx < 2; dx++)
			server_world_chunk_touch(q.c[dx][dz]);
	q.c[0][0]->populated = true;
	return true; // a full populate ran -> ensure_populated should do its final relight
}
static bool quad_populated(struct server_world* w, w_coord_t px, w_coord_t pz);
static bool server_world_populate_quadrant(struct server_world* w,
	struct server_local* s, w_coord_t x, w_coord_t z) {
	uint64_t request = G9B_BEGIN(w, "populate", x, z);
	bool changed = server_world_populate_quadrant_history_impl(w, s, x, z);
	G9B_END(w, "populate", request, quad_populated(w, x, z) ? "ok" : "deferred", x, z);
	return changed;
}


static bool quad_populated(struct server_world* w, w_coord_t px, w_coord_t pz) {
	struct server_chunk* c
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(px, pz));
	return c && c->populated;
}

static bool server_world_ensure_populated_impl(struct server_world* w,
								   struct server_local* s, w_coord_t cx,
								   w_coord_t cz, struct server_chunk** sc, unsigned stream) {
	assert(w && sc);
	/* An already-decorated resident target's four covering quadrants return
	 * without loading a neighbour or drawing RNG. Streaming it at a saturated
	 * cap must not reserve missing peripheral columns it will never touch. */
	/* G9X-5a: a world stopped by a population backing-storage failure
	 * provides nothing -- checked before requested-column admission, any
	 * load, population or RNG draw. The request stays pending; no retry. */
	if(w->gen_storage_failed) {
		*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		w->gen_pending[stream] = true;
		w->gen_pending_x[stream] = cx;
		w->gen_pending_z[stream] = cz;
		return false;
	}
	/* ONE backing-I/O baseline for this whole ensure call (G9X-5a). */
	const size_t io_before = w->generation_light_io_fails;
	bool ready_target = server_world_chunk_resident(w, cx, cz)
		&& quad_populated(w, cx, cz) && quad_populated(w, cx - 1, cz)
		&& quad_populated(w, cx, cz - 1) && quad_populated(w, cx - 1, cz - 1);
	/* G9 (ASTRA-G9-RULING 4): generation passes stay synchronous, but none may
	 * read or rewrite light that gameplay work still holds pending (the
	 * populate relight/bleed and WorldGenLakes read skylight; the drain undo
	 * must never overwrite a generation write). Any populate this call could
	 * run touches chunks cx-1..cx+1; while one is pinned, defer the whole
	 * provision to after the drain exactly like a budget deferral -- never an
	 * early drain. */
	bool light_conflict = false;
	if(!ready_target && w->light_pin_n)
		for(w_coord_t dz = -1; dz <= 1 && !light_conflict; dz++)
			for(w_coord_t dx = -1; dx <= 1 && !light_conflict; dx++)
				light_conflict = server_world_light_pinned(w, cx + dx, cz + dz);
	/* G9 item 4: the order guard -- while a light-deferred column holds the
	 * world's generation, no other column populates before it. Only where
	 * population order is observable: the Nether's ChunkProviderHell
	 * populate draws one shared, never reseeded hellRNG; the Overworld's
	 * ChunkProviderGenerate.populate reseeds its Random per chunk (order-
	 * independent), so it is never held there. */
	const bool order_matters = w->dimension == WORLD_DIM_NETHER;
	if(!ready_target && order_matters && w->light_gen_hold && !light_conflict
	   && (cx != w->light_gen_hold_x || cz != w->light_gen_hold_z)) {
		s_light_stats.gen_order_held++;
		*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		w->gen_pending[stream] = true;
		w->gen_pending_x[stream] = cx;
		w->gen_pending_z[stream] = cz;
		return false;
	}
	if(light_conflict) {
		if(order_matters && !w->light_gen_hold) {
			w->light_gen_hold = true;
			w->light_gen_hold_x = cx;
			w->light_gen_hold_z = cz;
		}
		w->light_gen_hold_age = 0;
		s_light_stats.provision_deferred++;
		*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		w->gen_pending[stream] = true;
		w->gen_pending_x[stream] = cx;
		w->gen_pending_z[stream] = cz;
		return false;
	}
	if(!ready_target && !server_transfer_column_preflight(w, cx, cz)) {
		*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		w->gen_pending[stream] = true;
		w->gen_pending_x[stream] = cx;
		w->gen_pending_z[stream] = cz;
		return false;
	}

	// Arm the per-cycle generation budget for this cascade (the only place the
	// throttle is active) and reset the window stats.
	w->gen_window_start = time_get();
	w->gen_throttle_armed = true;
	w->gen_attempts_window = 0;
	w->gen_this_window = 0;
	w->gen_deferred_window = 0;
	s_light_accum_us = 0; // reset the block-light flood timer for this window

	// Ensure chunk C itself is resident (disk or generated). C generates first,
	// while the budget is still fresh.
	struct server_chunk* dummy;
	server_world_load_chunk(w, cx, cz, &dummy);
	/* G9X-5a: C's own load met a backing I/O failure (its terrain latched
	 * SITE_TERRAIN, a nested population its own): stop. */
	if(w->generation_light_io_fails != io_before)
		server_world_population_storage_stop(w, cx, cz, SERVER_POPULATION_SITE_PROVISION);

	bool ready = !w->gen_storage_failed;
	if(w->generator && !w->gen_storage_failed) {
		// C's final blocks come from the four populate() quadrants that cover
		// it: origins (cx,cz), (cx-1,cz), (cx,cz-1), (cx-1,cz-1). Run any not
		// yet done; each writes its +8 spill into C. Under the budget some of
		// the underlying terrain generations may defer, leaving a quadrant
		// un-run.
		// Call all four (no short-circuit): each covering quadrant must run. OR their
		// "did I actually populate?" results -> any_populated gates the final relight.
		/* G9X-5a: after each, against the call's one baseline: a backing
		 * I/O failure anywhere in it is the sticky stop and no further
		 * quadrant is called. */
		static const w_coord_t quad_dx[4] = {0, -1, 0, -1}, quad_dz[4] = {0, 0, -1, -1};
		for(unsigned qi = 0; qi < 4 && !w->gen_storage_failed; qi++) {
			server_world_populate_quadrant(w, s, cx + quad_dx[qi], cz + quad_dz[qi]);
			if(w->generation_light_io_fails != io_before)
				server_world_population_storage_stop(w, cx + quad_dx[qi], cz + quad_dz[qi],
					SERVER_POPULATION_SITE_LOAD);
		}

		// C is only fully decorated -- and so only safe to SEND -- once ALL FOUR
		// covering quadrants have run. If any deferred, report not-ready; the
		// streaming loop keeps C as the nearest unsent chunk and retries next
		// cycle (the already-generated terrain persists, so it converges in a
		// few cycles with no single hitch).
		ready = !w->gen_storage_failed
			&& quad_populated(w, cx, cz) && quad_populated(w, cx - 1, cz)
			&& quad_populated(w, cx, cz - 1) && quad_populated(w, cx - 1, cz - 1);
	}

	w->gen_throttle_armed = false;
	s_gen_last_ms = time_diff_ms(w->gen_window_start, time_get());
	s_gen_last_generated = w->gen_this_window;
	s_gen_last_deferred = w->gen_deferred_window;

	*sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));

	/* Setter stores already follow every spill. Propagation stays queued for
	 * the prescribed drain; re-provisioning never rebuilds saved arrays. */

	w->gen_pending[stream] = !(ready && *sc != NULL);
	w->gen_pending_x[stream] = cx;
	w->gen_pending_z[stream] = cz;
	if(!ready_target)
		server_transfer_column_preflight_finish(w);
	return !w->gen_pending[stream];
}

/* G9 item 4: a populate of the held column (or a held column found already
 * decorated) releases the generation order guard. */
static void server_world_light_gen_hold_done(struct server_world* w,
											 w_coord_t cx, w_coord_t cz,
											 bool ok) {
	if(ok && w->light_gen_hold && cx == w->light_gen_hold_x
	   && cz == w->light_gen_hold_z)
		w->light_gen_hold = false;
}

static bool server_world_ensure_populated_history_impl(struct server_world* w,
								   struct server_local* s, w_coord_t cx,
								   w_coord_t cz, struct server_chunk** sc) {
	const bool ok = server_world_ensure_populated_impl(w, s, cx, cz, sc, 0);
	server_world_light_gen_hold_done(w, cx, cz, ok);
	return ok;
}

bool server_world_ensure_populated(struct server_world* w, struct server_local* s, w_coord_t cx, w_coord_t cz, struct server_chunk** sc) {
	uint64_t request = G9B_BEGIN(w, "provide", cx, cz);
	bool ok = server_world_ensure_populated_history_impl(w, s, cx, cz, sc);
	G9B_END(w, "provide", request, ok ? "ok" : "deferred", cx, cz);
	return ok;
}


static bool server_world_ensure_populated_far_history_impl(struct server_world* w,
								   struct server_local* s, w_coord_t cx,
								   w_coord_t cz, struct server_chunk** sc) {
	const bool ok = server_world_ensure_populated_impl(w, s, cx, cz, sc, 1);
	server_world_light_gen_hold_done(w, cx, cz, ok);
	return ok;
}

bool server_world_ensure_populated_far(struct server_world* w, struct server_local* s, w_coord_t cx, w_coord_t cz, struct server_chunk** sc) {
	uint64_t request = G9B_BEGIN(w, "provide-far", cx, cz);
	bool ok = server_world_ensure_populated_far_history_impl(w, s, cx, cz, sc);
	G9B_END(w, "provide-far", request, ok ? "ok" : "deferred", cx, cz);
	return ok;
}


bool server_world_is_chunk_sent(struct server_world* w, w_coord_t x,
								w_coord_t z) {
	assert(w);
	struct server_chunk* c
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
	return c && c->sent;
}

void server_world_mark_far_sent(struct server_world* w, w_coord_t x,
								w_coord_t z) {
	assert(w);
	dict_far_sent_set_at(w->far_sent, S_CHUNK_ID(x, z), 1);
}

void server_world_clear_far_sent(struct server_world* w, w_coord_t x,
								 w_coord_t z) {
	assert(w);
	dict_far_sent_erase(w->far_sent, S_CHUNK_ID(x, z));
}

bool server_world_is_far_sent(struct server_world* w, w_coord_t x, w_coord_t z) {
	assert(w);
	return dict_far_sent_get(w->far_sent, S_CHUNK_ID(x, z)) != NULL;
}

void server_world_extract_far_column(struct server_world* w,
									 struct server_chunk* sc, w_coord_t cx,
									 w_coord_t cz, struct far_rpc_slot* out) {
	assert(w && sc && out);

	// Per column scan ids top-down for the top NON-see-through block -- the EXACT
	// lod_surface_height predicate (chunk_mesher.c), so the far ring's surface
	// matches the abutting near skin ring with no seam (NOT the heightmap, which
	// sits at the water plane and would gap the skin ring ~9 blocks over ocean).
	// NOTE: the predicate captures only the top NON-see-through block. Tree CANOPIES
	// (leaves) + snow layers are see-through, so they NEVER enter the skin -- only a
	// LOG trunk raises skin_y. Under the cheap terrain-only far path (sc is NOT
	// populated) that trunk is intentionally absent: a render-only LOD tradeoff
	// (sim byte-identical on promotion), see server_world_extract_far_skin.
	// The Nether has a solid ROOF: Beta func_4058_b replaces randomized cells near
	// y=127 with bedrock but leaves netherrack directly below and between them. A naive
	// top-down scan would skin that opaque shell, and world_build_far_section would then
	// raise a solid plane near world height across the whole annulus -- occluding the far
	// ring instead of extending the view. So in the Nether we peel the entire contiguous
	// opaque ceiling shell down to the first transparent cavern cell, then skin the
	// terrain UNDER it (what the player actually sees from below). This
	// is the FAR-LOD skin ONLY; the near ring meshes full chunks so the real ceiling
	// stays visible up close. Beta renders the Nether at the same distance as the
	// overworld (only fog differs) -- EntityRenderer.farPlaneDistance has no dim term.
	const bool nether = (w->dimension == WORLD_DIM_NETHER);
	for(int x = 0; x < CHUNK_SIZE; x++) {
		for(int z = 0; z < CHUNK_SIZE; z++) {
			// ids layout = WG_IDX(x,y,z) = y + (z + x*CHUNK_SIZE)*WORLD_HEIGHT.
			size_t col = ((size_t)z + (size_t)x * CHUNK_SIZE) * WORLD_HEIGHT;
			int skin = -1;
			int top_fluid = -1; // highest visible water/lava block
			uint8_t top_fluid_id = BLOCK_AIR;
			int y_start = WORLD_HEIGHT - 1;
			if(nether) { // skip the mixed netherrack/bedrock ceiling shell
				while(y_start >= 0) {
					uint8_t roof_id = sc->ids[col + (size_t)y_start];
					if(!blocks[roof_id] || blocks[roof_id]->can_see_through)
						break;
					y_start--;
				}
			}
			for(int y = y_start; y >= 0; y--) {
				uint8_t id = sc->ids[col + (size_t)y];
				// First liquid seen from the top is its visible surface. Both fluids
				// are transparent to this opaque-skin predicate, so remember them
				// before continuing down to the supporting terrain.
				if(top_fluid < 0
				   && (id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW
					   || id == BLOCK_LAVA_STILL || id == BLOCK_LAVA_FLOW)) {
					top_fluid = y;
					top_fluid_id
						= (id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW)
							? BLOCK_WATER_STILL
							: BLOCK_LAVA_STILL;
				}
				if(blocks[id] && !blocks[id]->can_see_through) {
					skin = y;
					break;
				}
			}
			int i = x * CHUNK_SIZE + z;
			// A liquid above its opaque bed/floor is the visible surface, not the
			// hidden terrain. Beta draws both water and Nether lava to render distance;
			// omitting transparent lava here made the far Nether a bare netherrack floor.
			// top_id=water has
			// biome_tint==NONE, so the mesher renders it untinted white -- byte-faithful
			// to BlockFluid.colorMultiplier()==0xFFFFFF (no biome water tint in b1.7.3).
			// Capture the top liquid BLOCK (y, not the y+1 heightmap plane) so the far
			// surface sits at the SAME height the near skin draws it -> no seam. Normalize
			// flowing water/lava to still: the far plane is static and flow level is
			// sub-pixel at far distance.
			if(top_fluid >= 0 && top_fluid > skin) {
				out->skin_y[i] = (uint8_t)top_fluid;
				out->top_id[i] = top_fluid_id;
			} else if(skin >= 0) {
				out->skin_y[i] = (uint8_t)skin;
				out->top_id[i] = sc->ids[col + (size_t)skin];
			} else {
				out->skin_y[i] = FAR_NO_SURFACE;
				out->top_id[i] = 0;
			}
		}
	}

	// Biome grass/foliage tint indices: same call + [localX*16 + localZ] layout as
	// the full-chunk send (server_local.c) so the client tints far grass identically.
	if(w->generator) {
		worldgen_climate(w->generator, cx, cz, out->grass_i, out->grass_j);
	} else {
		memset(out->grass_i, 0, CHUNK_SIZE * CHUNK_SIZE);
		memset(out->grass_j, 0, CHUNK_SIZE * CHUNK_SIZE);
	}
}

// M1 deep-reach CHEAP far-skin: generate the target chunk ALONE (single chunk, no
// populate cascade, no resident-dict insert) and extract its surface skin into out.
// This is the render-only LOD path for the far ring -- the sanctioned LOD render-
// reach carve-out (CLAUDE.md): it drops DECORATION (tree trunks, snow, surface
// lakes) from the distant silhouette, but the SIMULATION is untouched -- the chunk
// is fully transient (freed here) and never marked populated, so when the player
// promotes it into full-detail range the real ensure_populated cascade regenerates
// a byte-identical Beta world. ~10x cheaper than ensure_populated, which forces a
// 9-chunk populate cascade resident just to read one chunk's ~1KB skin (the prior
// FAR_SEND_BUDGET=3 cap existed only because of that spike). Terrain/material/tint
// are byte-identical to the populated skin at every NON-tree/NON-lake column
// (caves/ravine mouths included -- cave_carve clamps to local coords); proven by
// tests/test_far_skin_equiv. KNOWN bounded LOD effect: an EDITED chunk re-entering
// the far ring regenerates its ORIGINAL terrain here (no disk-edit load) -- the edit
// reappears on promotion; sub-pixel at far distance. The caller bounds the per-tick
// count + time budget (this path runs OUTSIDE ensure_populated's gen throttle).
// Returns false if there is no generator or generation failed.
// Approximate tree-trunk silhouette for the cheap terrain-only far skin: scatter LOG
// trunk spikes at the right per-chunk DENSITY so a distant forest reads as forested
// (no bald tree-line at the near<->far boundary) without paying the populate cascade.
// This is a render-only LOD APPROXIMATION (CLAUDE.md's sanctioned LOD carve-out), NOT
// faithful tree placement: Beta roots trees via a 4-origin +8-offset roll whose RNG
// consumption depends on the surrounding terrain (worldgen_trees / tree_oak), so the
// real trunk positions can't be reproduced without the ~9-chunk terrain the cheap path
// drops. So these trunks are at PLAUSIBLE-but-not-real spots and REPOSITION into the
// real trees when the chunk promotes to full detail. A private deterministic Random
// (the world seed + coords, a distinct salt so it never aliases the real tree stream)
// keeps it stable + world-varied; the count uses Beta's tree-count formula so density
// tracks biome + treeDensity (forests dense, plains/desert bare).
static void far_skin_trunk_stub(struct server_world* w, w_coord_t cx, w_coord_t cz,
								struct far_rpc_slot* out) {
	enum biome b = worldgen_biome_at(w->generator, cx * CHUNK_SIZE + 16,
									 cz * CHUNK_SIZE + 16);
	double td
		= worldgen_tree_density(w->generator, cx * CHUNK_SIZE, cz * CHUNK_SIZE);

	struct java_random rand;
	// Mix in UNSIGNED (wrapping) arithmetic: `(int64_t)cx * <bigprime>` is signed-overflow
	// UB for negative cx/cz (UBSan halts the CI soak). The 64-bit seed bit-pattern is
	// byte-identical, so the far-skin RNG stream is unchanged.
	jrand_set_seed(&rand,
				   w->seed ^ (int64_t)((uint64_t)cx * 0x2545F4914F6CDD1DULL)
					   ^ (int64_t)((uint64_t)cz * 0x9E3779B97F4A7C15ULL)
					   ^ 0xFA5C0DEDLL);

	// Beta tree-count formula (worldgen_trees), with the private stream + biome bump.
	int sample = java_double_to_int(
		(td / 8.0 + jrand_next_double(&rand) * 4.0 + 4.0) / 3.0);
	int count = (jrand_next_int_bound(&rand, 10) == 0) ? 1 : 0;
	switch(b) {
		case BIOME_FOREST:
		case BIOME_RAINFOREST:
		case BIOME_TAIGA: count += sample + 5; break;
		case BIOME_SEASONAL_FOREST: count += sample + 2; break;
		case BIOME_DESERT:
		case BIOME_TUNDRA:
		case BIOME_PLAINS: count -= 20; break;
		default: break;
	}
	if(count <= 0)
		return; // bare biome (faithful: Beta places ~no trees here)

	for(int i = 0; i < count; i++) {
		int lx = jrand_next_int_bound(&rand, CHUNK_SIZE);
		int lz = jrand_next_int_bound(&rand, CHUNK_SIZE);
		int h = jrand_next_int_bound(&rand, 3) + 4; // oak-ish trunk height
		int col = lx * CHUNK_SIZE + lz;
		if(out->skin_y[col] == FAR_NO_SURFACE)
			continue; // ocean / no surface -> no tree
		// Root only on grass/dirt (Beta's ground check); skip stone slopes + sand.
		if(out->top_id[col] != BLOCK_GRASS && out->top_id[col] != BLOCK_DIRT)
			continue;
		int ty = (int)out->skin_y[col] + h;
		if(ty >= WORLD_HEIGHT)
			ty = WORLD_HEIGHT - 1;
		out->skin_y[col] = (uint8_t)ty;
		out->top_id[col] = BLOCK_LOG; // a LOG spike -> the client renders the trunk
	}
}

bool server_world_extract_far_skin(struct server_world* w, w_coord_t cx,
								   w_coord_t cz, struct far_rpc_slot* out,
								   bool trunk_stub) {
	assert(w && out);
	if(!w->generator)
		return false;

	struct server_chunk tmp;
	server_chunk_init(&tmp);
	// W1-d (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md
	// "worldgen fast path (a)"): the skin extract below reads ONLY tmp.ids
	// (the tint comes from worldgen_climate), and every array phase 5 (the
	// only phase this skips) would have written -- lighting_sky/torch,
	// heightmap -- is freed immediately below UNREAD regardless. So skip
	// computing them in the first place (worldgen_generate_chunk_far_skin)
	// instead of computing-then-discarding: ~15-20% of the ~1ms/column
	// far-gen cost, T1 dead work (tests/test_far_skin_equiv.c's lighting-
	// skip CASE proves byte-identical sc->ids either way).
	if(!worldgen_generate_chunk_far_skin(w->generator, &tmp, cx, cz))
		return false;

	// The skin extract reads ONLY tmp.ids (the tint comes from worldgen_climate),
	// so free the metadata/lighting/heightmap arrays up front -- the far gen then
	// holds just ~32KB (ids) during the scan, not the full ~80KB server_chunk.
	// (mem2_free: these came from worldgen_alloc_chunk via mem2_alloc -> MEM2;
	// still allocated even though lighting/heightmap were never WRITTEN --
	// worldgen_alloc_chunk's shape stays uniform across every construction site.)
	/* 2026-09-11: the five arrays are one pooled slot, so the early partial
	 * free is gone -- the far gen holds the whole slot for the scan (a pool
	 * slot, not shared-heap bytes) and releases it with the chunk below. */

	server_world_extract_far_column(w, &tmp, cx, cz, out);

	/* Whole shape (slot on the Wii, five arrays on PC -- Astra P1: freeing
	 * only ids leaked the other four on PC). */
	server_chunk_arrays_free(&tmp);

	// Optional: overlay an approximate tree-trunk silhouette so far forests aren't
	// bald (render-only LOD; the trunks reposition to real trees on promotion).
	if(trunk_stub)
		far_skin_trunk_stub(w, cx, cz, out);

	return true;
}

bool server_world_save_chunk(struct server_world* w, bool erase, w_coord_t x,
							 w_coord_t z) {
	assert(w);
	struct server_chunk* c
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
	return c ? server_world_save_chunk_obj(w, erase, x, z, c) : true;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static size_t s_chunk_write_attempts;

void server_world_test_chunk_write_attempts_reset(void) {
	s_chunk_write_attempts = 0;
}

size_t server_world_test_chunk_write_attempts(void) {
	return s_chunk_write_attempts;
}
#endif

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* F2 test hook: when > 0, the next that many malloc calls from the blast
 * staging allocation will return NULL, simulating OOM. Set via
 * server_world_test_fail_blast_mallocs(). */
static size_t s_blast_malloc_fail_countdown = 0;

void server_world_test_fail_blast_mallocs(size_t count) {
	s_blast_malloc_fail_countdown = count;
}

static void* blast_staging_malloc(size_t sz) {
	if(s_blast_malloc_fail_countdown > 0
	   && s_blast_malloc_fail_countdown-- > 0)
		return NULL;
	return malloc(sz);
}
#else
static void* blast_staging_malloc(size_t sz) {
	return malloc(sz);
}
#endif

/* ---- S2 two-phase retirement: the retiring ring (see server_world.h) ---- */

size_t server_world_retiring_active(const struct server_world* w) {
	assert(w);
	return w->retiring_active;
}

/* A1b (D10): the retained-record budget is a SESSION aggregate. Every world
 * of the owning session (constructed instances only: a slot that was never
 * created may hold stack garbage) is charged until its records are actually
 * freed, so two worlds share the one existing 512 KiB threshold (plus the
 * documented one-record overshoot) instead of doubling it. A world without a
 * session is charged alone, as before. Single-world: identical to the old
 * per-world test. */
size_t server_world_session_retiring_bytes(const struct server_world* w) {
	assert(w);
	struct server_local* s = w->session;
	if(!s || &s->worlds[w->world_index] != w)
		return w->retiring_bytes;
	size_t total = 0;
	for(int i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++)
		if(server_world_chunks_ready(&s->worlds[i]))
			total += s->worlds[i].retiring_bytes;
	return total;
}

static struct server_world_retiring* server_world_retiring_free_slot(
	struct server_world* w) {
	if(server_world_session_retiring_bytes(w)
	   >= SERVER_WORLD_RETIRING_MAX_BYTES)
		return NULL;
	for(size_t k = 0; k < SERVER_WORLD_RETIRING_MAX; k++)
		if(!w->retiring[k].active)
			return w->retiring + k;
	return NULL;
}

static struct server_world_retiring* server_world_retiring_find(
	struct server_world* w, int64_t id) {
	for(size_t k = 0; k < SERVER_WORLD_RETIRING_MAX; k++)
		if(w->retiring[k].active && w->retiring[k].id == id)
			return w->retiring + k;
	return NULL;
}

static void server_world_retiring_release(struct server_world* w,
										  struct server_world_retiring* r) {
	assert(w && r && r->active);
	assert(w->retiring_bytes >= r->record.len);
	w->retiring_bytes -= r->record.len;
	buffer_free(&r->record);
	r->record = BUFFER_INIT;
	r->active = false;
	assert(w->retiring_active > 0);
	w->retiring_active--;
}

/* The archive answer for one retained record: the same lookup
 * server_world_reconcile_pending_saves makes for a resident chunk. */
static enum region_save_state server_world_retiring_state(
	struct server_world* w, const struct server_world_retiring* r) {
	w_coord_t x = S_CHUNK_X(r->id), z = S_CHUNK_Z(r->id);
	for(size_t k = 0; k < server_world_region_count(w); k++) {
		struct region_archive* ra = server_world_region_at(w, k);
		if(CHUNK_REGION_COORD(x) != ra->x || CHUNK_REGION_COORD(z) != ra->z)
			continue;
		return region_archive_save_state(ra, r->save_job);
	}
	/* A handle that is no longer resident was closed by
	 * region_archive_destroy, which publishes and REPORTS. */
	return (w->region_evict_failed || w->region_close_failed)
		? REGION_SAVE_FAILED
		: REGION_SAVE_DURABLE;
}

/* Re-append one FAILED record from its retained copy. Joins the open batch
 * like any write (the seam callers run with the batch closed, so the write
 * publishes itself the ordinary way). Returns false when the region cannot
 * be opened or the append fails: the entry is KEPT for the next seam. */
static bool server_world_retiring_retry(struct server_world* w,
										struct server_world_retiring* r,
										struct region_archive* ra) {
	w_coord_t x = S_CHUNK_X(r->id), z = S_CHUNK_Z(r->id);
	/* NOTE: opening the region can LRU-replace another resident handle;
	 * every caller re-derives handles by index after this returns. */
	if(!ra)
		ra = server_world_chunk_region(w, x, z);
	if(!ra)
		return false;
	if(w->save_batch_open)
		region_archive_batch_begin(ra);
	/* The job this re-append joins is known only AFTER the append (the
	 * append itself may publish on overflow first); a pre-write sample
	 * would then name the job that just completed. */
	uint64_t before = region_archive_last_join_job(ra);
	if(!region_archive_recommit_record(ra, x, z, &r->record))
		return false;
	uint64_t after = region_archive_last_join_job(ra);
	/* `after == before` is legitimate: the re-append joined a job that
	 * already held a delta of this batch. Either way `after` names the job
	 * the retained bytes now wait on. */
	(void)before;
	r->save_job = after;
	w->retiring_retries++;
	return true;
}

/* Walk the ring at a batch seam: DURABLE entries are released, FAILED
 * entries are re-appended from their retained record, PENDING entries wait. */
/* `allow_open`: a retry may open (and LRU-replace) a region handle. The
 * seam callers pass true; the save path -- which holds a live handle with an
 * armed batch -- passes false and only RELEASES durable entries. */
static void server_world_reconcile_retiring_ex(struct server_world* w,
											   bool allow_open) {
	for(size_t k = 0; k < SERVER_WORLD_RETIRING_MAX; k++) {
		struct server_world_retiring* r = w->retiring + k;
		if(!r->active)
			continue;
		enum region_save_state state = server_world_retiring_state(w, r);
		if(state == REGION_SAVE_PENDING)
			continue;
		if(state == REGION_SAVE_DURABLE) {
			server_world_retiring_release(w, r);
			continue;
		}
		if(allow_open) {
			region_save_failure_note("archive_publish");
			fprintf(stderr,
				"[chunk_save] result=fail chunk=(%d,%d) stage=archive_publish "
				"job=%llu retry_owner=retiring\n",
				(int)S_CHUNK_X(r->id), (int)S_CHUNK_Z(r->id),
				(unsigned long long)r->save_job);
			(void)server_world_retiring_retry(w, r, NULL);
		}
	}
}

static void server_world_reconcile_retiring(struct server_world* w) {
	server_world_reconcile_retiring_ex(w, true);
}

/* Invariant keeper: a chunk about to be RELOADED from disk must not have a
 * retained record in flight. If its record FAILED, re-append it first so the
 * in-memory offsets (which region_archive_get_chunk reads through) name the
 * NEWEST bytes; then hand retry responsibility to the resident chunk by
 * transferring its pending job to the resident chunk and drop the entry
 * only after the complete load succeeds. No rewrite or LastUpdate change. */
static enum server_world_retiring_load server_world_retiring_consume_for_load(
	struct server_world* w, struct region_archive* ra, w_coord_t x,
	w_coord_t z, struct server_world_retiring** out_entry) {
	struct server_world_retiring* r
		= server_world_retiring_find(w, S_CHUNK_ID(x, z));
	*out_entry = NULL;
	if(!r)
		return SERVER_WORLD_RETIRING_LOAD_NONE;
	/* An id is never resident AND retained across a tick: either the retained
	 * bytes become the bytes the loader reads (PENDING: the in-memory offsets
	 * already name them; FAILED: re-append them now), or the load is refused
	 * so the next seam can retry first. Never fabricate/regenerate a chunk
	 * over a retained newer record. The two ownerships overlap only for the
	 * duration of one load (install .. success exit, same thread, no seam in
	 * between): the loader releases the entry at its success exit, so a load
	 * that fails after install keeps the retained record as the retry source. */
	if(!ra)
		return SERVER_WORLD_RETIRING_LOAD_BLOCKED;
	enum region_save_state state = server_world_retiring_state(w, r);
	if(state == REGION_SAVE_FAILED && !server_world_retiring_retry(w, r, ra))
		return SERVER_WORLD_RETIRING_LOAD_BLOCKED;
	/* NOT released here: the load can still fail after this point, and the
	 * retained record must outlive a failed load. The loader releases the
	 * entry once the chunk is in the dict. */
	*out_entry = r;
	return SERVER_WORLD_RETIRING_LOAD_CONSUMED;
}

static bool server_world_save_chunk_obj_impl(struct server_world* w,
											 bool erase, w_coord_t x,
											 w_coord_t z,
											 struct server_chunk* c) {
	assert(w && c);
	const char* failure_stage = "none";
	w->evict_retire_deferred = false;

	// Gather this chunk's tile/dynamic entities -- they live outside the block
	// arrays. A nonempty live set forces an NBT rewrite because contents, text,
	// fuse, position, and velocity can change without touching c->modified.
	// Conversely, an untouched chunk with no live state and no previously
	// persisted entity list is a true no-op: portal/death checkpoints must not
	// synchronously recompress the whole resident ring to Wii FAT storage.
	struct chunk_tile_entities te = {0};
	struct chunk_entities entities = {0};
	bool have_local = w->local != NULL;
	bool tile_entities_ready = true;
	bool entities_ready = true;
	if(have_local) {
		struct te_store st = server_world_tile_store(w);
		tile_entities_ready = te_store_gather(&st, x, z, &te);
		entities_ready
			= server_local_gather_chunk_entities_w(w->local, w, x, z, &entities);
	}

	bool gathered = tile_entities_ready && entities_ready;
	if(!tile_entities_ready)
		failure_stage = "tile_entity_gather";
	else if(!entities_ready)
		failure_stage = "entity_gather";
	// tile_entities_any is the shared every-kind predicate (tile_entities_nbt.h).
	// A hand-rolled enumeration here omitted dispensers (D12): contents-only
	// dispenser edits never set c->modified, so the omission skipped the write
	// entirely and the edit was silently lost on eviction.
	bool tile_entities_nonempty = have_local && gathered
		&& tile_entities_any(&te);
	bool live_entities_nonempty
		= have_local && gathered && entities.count != 0;
	/* persisted_entities_nonempty is the one bit needed to distinguish an
	 * untouched empty chunk from a chunk whose formerly nonempty disk list must
	 * be explicitly cleared after all entities moved/died. */
	bool write_required = c->modified
		|| (have_local && gathered
			&& (tile_entities_nonempty || live_entities_nonempty
				|| c->persisted_entities_nonempty));
	/* A failed gather is never an empty snapshot. Even when no block write was
	 * otherwise required, eviction/checkpoint must retain every live owner. */
	bool save_succeeded = gathered && !write_required;
	/* S5: set once the destroy batch has been admitted AND this chunk's
	 * server entities freed, so the erase tail below never double-frees. */
	bool entities_retired = false;
	/* stage22-0927: an EVICTING save of a chunk with live entities must be
	 * able to admit its entity-destroy batch (+ unload) BEFORE the record is
	 * written. Without headroom, write nothing and defer: the old order
	 * committed the record, met a full client inbox at the batch, re-dirtied
	 * the chunk and threw the write away (real-Wii split_play_8m, save_fail
	 * stage 22). The server thread is the only producer into the client
	 * inbox, so the answer holds until this save's own sends. */
	if(erase && have_local && gathered && entities.count != 0
	   && !server_local_evict_headroom_w(w->local, w)) {
		w->evict_retire_deferred = true;
		failure_stage = "evict_retire_deferred";
		write_required = false; /* skip the write; save_succeeded stays false */
		save_succeeded = false;
	}
	/* A clean chunk can still own an unpublished non-evicting save. Move
	 * the EXACT pending payload into the bounded retry ring before freeing
	 * the resident arrays. Refuse eviction on pressure/read/rollback: the
	 * resident owner survives and the next seam resolves its pending job. */
	if(erase && save_succeeded && c->save_pending) {
		struct region_archive* ra = server_world_chunk_region(w, x, z);
		enum region_save_state state = ra
			? region_archive_save_state(ra, c->save_job) : REGION_SAVE_FAILED;
		if(state == REGION_SAVE_DURABLE) {
			c->save_pending = false;
		} else {
			save_succeeded = false;
			struct server_world_retiring* slot = server_world_retiring_free_slot(w);
			if(!slot) {
				server_world_reconcile_retiring_ex(w, false);
				slot = server_world_retiring_free_slot(w);
			}
			struct buffer record = BUFFER_INIT;
			if(slot && ra && region_archive_copy_pending_record(
				   ra, x, z, c->save_job, &record)) {
				assert(!server_world_retiring_find(w, S_CHUNK_ID(x, z)));
				if(record.cap > record.len) {
					unsigned char* tight = realloc(record.data, record.len);
					if(tight) { record.data = tight; record.cap = record.len; }
				}
				*slot = (struct server_world_retiring){
					.id = S_CHUNK_ID(x, z), .save_job = c->save_job,
					.record = record, .active = true};
				w->retiring_active++;
				w->retiring_bytes += record.len;
				save_succeeded = true;
			} else if(slot && ra
				&& region_archive_save_state(ra, c->save_job)
					== REGION_SAVE_DURABLE) {
				/* The copy waits for async publication before reading. If the
				 * exact job completed there, no pending record remains to copy.
				 * Only this clean/no-write path may use that durable result;
				 * FAILED or still PENDING must retain the resident owner. */
				c->save_pending = false;
				save_succeeded = true;
			} else {
				if(!slot) w->retiring_refused++;
				failure_stage = slot ? "pending_record" : "retiring_reserve";
			}
		}
	}
	if(write_required && gathered) {
		//  load region archive into cache
		/* A2-S: the write goes through the shared pool's admission, creating
		 * an absent region file INSIDE the cap (the old code opened an
		 * uncached temporary here -- a fifth handle, synced and closed per
		 * chunk). The handle stays cached and batched like any other. */
		struct region_archive* ra = server_world_chunk_region_create(w, x, z);
		const struct region_archive* tmp = NULL; /* A2-S: no temporary */
		bool have_archive = (ra != NULL);
		if(!have_archive)
			failure_stage = "archive_open";

		if(have_archive) {
			if(w->checkpoint_stream_active && !region_archive_checkpoint_ordered(ra)) {
				/* A newly touched legacy handle failed before the stream
				 * handoff. Keep the existing explicit ownership barrier. */
				if(!region_archive_sync(ra)) w->region_evict_failed = true;
				(void)region_archive_checkpoint_ordered(ra);
			}
			/* SAVE BATCH: defer this write's header publication to
			 * server_world_save_batch_end. Re-armed per save because an LRU
			 * eviction can replace a resident slot mid-batch, and because a
			 * region first touched inside the batch is opened here. The
			 * throwaway `tmp` handle is deliberately excluded: it syncs and
			 * closes on the next line, so batching it would buy nothing and
			 * only widen its failure window. */
			if(w->save_batch_open && ra != tmp)
				region_archive_batch_begin(ra);
			region_pool_pin(server_world_region_pool(w), ra);
#ifdef CAVEWII_SERVER_LOCAL_TEST
			s_chunk_write_attempts++;
#endif
			/* M3: sample the epoch BEFORE the write. With no batch open the
			 * write publishes itself, so by the time set_chunk returns the
			 * epoch has already advanced past the one its own bytes joined. */
			uint64_t save_job = region_archive_save_job(ra);
			/* S2: an EVICTING write inside an open batch keeps its record
			 * in the retiring ring until the archive answers DURABLE.
			 * Reserve the slot BEFORE writing so a full ring refuses the
			 * write (nothing appended, nothing freed) instead of losing
			 * the record: that is the retain-everything failure path the
			 * callers already back off on. */
			bool retain = erase && w->save_batch_open && ra != tmp;
			struct server_world_retiring* slot = NULL;
			struct buffer record = BUFFER_INIT;
			bool committed = false;
			uint64_t join_before = region_archive_last_join_job(ra);
			if(retain) {
				slot = server_world_retiring_free_slot(w);
				if(!slot) {
					/* Entries published by an EARLIER batch stay in the ring
					 * until the next seam; free them now rather than refuse
					 * on records that are already durable. */
					server_world_reconcile_retiring_ex(w, false);
					slot = server_world_retiring_free_slot(w);
				}
				if(slot)
					committed = region_archive_set_chunk_retain(
						ra, x, z, c, have_local ? &te : NULL,
						have_local ? &entities : NULL, &record);
				else
					w->retiring_refused++;
			} else {
				committed = w->checkpoint_encode_active
					? region_archive_set_chunk_cached(ra, x, z, c,
						have_local ? &te : NULL, have_local ? &entities : NULL,
						&w->checkpoint_encode_cache)
					: w->checkpoint_encode_cache
					? region_archive_set_chunk_watched(ra, x, z, c,
						have_local ? &te : NULL, have_local ? &entities : NULL,
						&w->checkpoint_encode_cache)
					: region_archive_set_chunk(ra, x, z, c, have_local ? &te : NULL,
						have_local ? &entities : NULL);
			}
			if(!committed)
				failure_stage
					= retain && !slot ? "retiring_reserve" : "archive_write";
			/* S2: if this write joined a job, that job is the one to wait
			 * for -- not the pre-write guess, which is wrong exactly when the
			 * append published the previous job on overflow first. */
			if(committed && ra != tmp
			   && region_archive_last_join_job(ra) != join_before)
				save_job = region_archive_last_join_job(ra);
			else if(committed && region_archive_deferred_io_enabled())
				/* G9 R19: the write added no delta (the record is already
				 * byte-identical), but the record it matched may itself still
				 * be waiting on a queued publication of an EARLIER job: wait
				 * on THAT job (0 = durable), never on whichever id the
				 * pre-write sample happened to name. */
				save_job = region_archive_chunk_job(ra, x, z);
			region_pool_unpin(server_world_region_pool(w), ra);
			if(committed) {
				/* M3. The record is IN THE FILE, so these two flags describe
				 * it truthfully either way; what is not yet settled is whether
				 * the on-disk INDEX names it. */
				c->persisted_tile_entities_complete
					= have_local && te.legacy_flat_complete;
				/* region_archive_set_chunk serializes an empty list when there is
				 * no local owner, so this mirrors the just-committed raw structure. */
				c->persisted_entities_nonempty
					= have_local && live_entities_nonempty;
				enum region_save_state state
					= region_archive_save_state(ra, save_job);
				if(state == REGION_SAVE_FAILED)
					failure_stage = "archive_publish";
				/* A write whose record was already byte-identical on disk
				 * (round 4) contributes NO delta, so the epoch never moves for
				 * it and it is durable as it stands. Ask the archive whether
				 * this chunk is actually waiting for anything. */
				if(state == REGION_SAVE_PENDING
				   && !region_archive_chunk_pending(ra, x, z))
					state = REGION_SAVE_DURABLE;
				/* THE EVICTION PATH IS NOT DEFERRED -- MEASURED, NOT
				 * ASSUMED. Holding an evicted chunk resident until its batch
				 * is durable (the shape the fix-round brief asks for, at
				 * REGION_BATCH_MAX x 82,176 B) was implemented and REJECTED by
				 * the suite: with a cap of 8 -- and again with a cap of 2 --
				 * test_respawn_residency, test_tnt_primed_env,
				 * test_column_dedup_lifetime/_state_equiv/_render_equiv,
				 * test_split_residency_single_equiv and
				 * test_startup_retirement all fail, because a saved-and-erased
				 * chunk leaving the dict IMMEDIATELY is a contract those tests
				 * pin (load completion and the residency/dedup accounting are
				 * driven off it). Forcing durability instead -- one
				 * region_archive_sync per evicting write -- is worse still: an
				 * eviction runs inside the open save batch, so that reinstates
				 * the per-chunk publication cost the whole pace program
				 * removed.
				 *
				 * So an evicting save keeps the pre-existing behaviour, and
				 * this half of M3 is NOT closed: see the fix-round section of
				 * tmp/round5_design.md. The archive-side half IS closed -- a
				 * failed publication is now latched, refuses the next batch
				 * and is REPORTED at the next barrier (region_evict_failed /
				 * region_close_failed), which is what turns a silent revert
				 * into a visible failure. */
				c->modified = false;
				save_succeeded = true;
				/* S5: THE EVICTION NOTIFICATION IS ADMITTED HERE -- after the
				 * record is in the file, before ANY owner of this chunk is
				 * retired and before the retiring ring takes the record. One
				 * nonblocking batch carries every dynamic entity of this
				 * chunk; a full client inbox is a RETRY, never a blocking
				 * send (that block is the near/far-evict deadlock shape).
				 * Refused => keep the chunk, its tile entities and its live
				 * entities, and hand the caller the existing retain-
				 * everything failure result. Nothing enters the ring: the
				 * chunk is still RESIDENT, so "ring INTERSECT dict = {}"
				 * holds and its own dirty bit is the retry source.
				 *
				 * THE RE-DIRTY IS LOAD-BEARING, and its scope is exact: a
				 * write that COMMITTED but whose chunk stays resident must
				 * mark that chunk dirty even if it began clean. This record
				 * NAMES the live entities; if one of them moves to another
				 * chunk before the retry, an unmodified entity-free chunk
				 * would look like an untouched no-op, the record would never
				 * be rewritten, and the moved entity would exist twice on
				 * disk. Dirty forces a complete fresh record (empty entity
				 * list included) on the retry.
				 *
				 * DELIBERATELY NOT WIDENED to every attempted write. The
				 * candidate's `write_attempted && !retired` form was
				 * implemented here first and MEASURED wrong: a FAILED write
				 * rolls its region back, so disk still matches the flags and
				 * nothing needs clearing, while the extra dirty bit feeds
				 * the 1-chunk-per-tick autosave sweep -- exactly the trap the
				 * RE-DIRTY-ON-FAILURE note below records. It diverged
				 * test_column_dedup_state_equiv's frozen trace (PASS on the
				 * base, FAIL with the blanket rule). Commit-then-retain is
				 * the only shape that needs it. */
				if(erase && have_local) {
					if(server_local_try_release_chunk_entities_w(
						   w->local, w, &entities))
						entities_retired = true;
					else {
						/* Backstop (the headroom check above makes this the
						 * notify-array OOM case): the record committed, so
						 * re-dirty (load-bearing, see above) and defer. */
						c->modified = true;
						save_succeeded = false;
						w->evict_retire_deferred = true;
						failure_stage = "evict_retire_deferred";
					}
				}
				/* RE-DIRTY ON FAILURE, rather than hold the dirty bit.
				 * HOLDING it was implemented first and MEASURED wrong: a
				 * chunk that stays modified is re-saved by the 1-chunk-per-
				 * tick autosave sweep on every tick of the ~40-tick batch
				 * window, which monopolises the sweep --
				 * test_column_dedup_lifetime (a 600-tick progress bound),
				 * test_respawn_residency and test_tnt_primed_env all fail.
				 * Clearing optimistically and re-dirtying at
				 * server_world_reconcile_pending_saves when the archive says
				 * the batch FAILED is behaviour-identical on the success path
				 * and still closes the hole: the chunk is resident (erase is
				 * false here), so its blocks are still in memory to write
				 * again. */
				if(state == REGION_SAVE_PENDING && !erase) {
					c->save_job = save_job;
					c->save_pending = true;
				} else {
					c->save_pending = false;
				}
				/* S2: the evicted chunk is freed below; its record is NOT
				 * durable yet (PENDING), or joined an already-latched epoch
				 * (FAILED) -- either way the retained copy is the retry
				 * source. An unchanged record (empty buffer) never enters
				 * the ring: nothing was appended, it is durable as it
				 * stands. */
				if(retain && slot && save_succeeded && record.data
				   && record.len && state != REGION_SAVE_DURABLE) {
					assert(!server_world_retiring_find(w, S_CHUNK_ID(x, z))
						   && "a resident id has no retained record");
					/* Shrink the grow-doubled dump buffer to its length. */
					if(record.cap > record.len) {
						unsigned char* tight = realloc(record.data, record.len);
						if(tight) {
							record.data = tight;
							record.cap = record.len;
						}
					}
					slot->id = S_CHUNK_ID(x, z);
					slot->save_job = save_job;
					slot->record = record;
					slot->active = true;
					w->retiring_active++;
					w->retiring_bytes += record.len;
					record = BUFFER_INIT;
				}
				/* An EVICTED chunk is freed below, so there is nothing left to
				 * re-dirty; holding it resident until the batch lands was also
				 * implemented and rejected (see server_world.h). Its safety net
				 * is the archive-side half of M3: a failed publication is now
				 * latched, refuses the next batch, and is REPORTED at the next
				 * barrier into region_evict_failed / region_close_failed
				 * instead of passing silently. */
				/* REGION_SAVE_FAILED: save_succeeded stays false, which is the
				 * existing retain-everything failure path. */
			}
			buffer_free(&record); /* S2: no-op when the ring took it */
			// A write/sync/close failure retains both the block dirty bit and the
			// prior disk-list marker. Dynamic-only conditions are re-gathered on
			// the retry. Cached archives remain open for the shared barrier above;
			// only the throwaway archive is closed here.
		}
		// create/open/gather failures take the same non-destructive path. The near
		// and far eviction callers break their current batch and back off before
		// retrying, avoiding both data loss and the historical tight retry wedge.
	}

	tile_entities_parse_free(&te);

	/* Eviction transfers ownership to the archive only after the complete chunk
	 * record committed. Failed saves leave the entities live for a later retry.
	 * S5: the successful path already retired them behind the admitted destroy
	 * batch; entities_retired keeps that the single retirement point. */
	bool erase_now = erase && save_succeeded;
	assert(!(erase_now && have_local && entities.count != 0
			 && !entities_retired)
		   && "an evicted chunk's entities retire only behind an admitted "
			  "destroy batch");
	chunk_entities_free(&entities);

	if(erase_now) {
		// The chunk's tile entities are now persisted (above), so free their
		// table slots: the resident chest/sign set shrinks with the loaded
		// chunks, which is the whole point of the residency redesign.
		if(have_local) {
			struct te_store st = server_world_tile_store(w);
			te_store_release(&st, x, z);
		}
		/* This chunk's absence breaks the complete +/-8 union for itself and
		 * every adjacent resident chunk. Clear those readiness epochs before
		 * erasing the member so its later reload can rescan them. */
		server_world_invalidate_chunk_tick_rearms(w, x, z);
		server_world_chunk_destroy(c);
		// Erase moves/frees in-table values -> the resolution cache's pointer
		// (possibly THIS chunk) dangles. Drop it.
		server_world_chunk_cache_invalidate(w);
		dict_server_chunks_erase(w->chunks, S_CHUNK_ID(x, z));
		G9B_NOTE(w, "residency", "remove", "ok", x, z, NULL);
	}
	if(!save_succeeded) {
		/* stage22-0927 follow-up: a full-inbox eviction DEFERRAL is not a
		 * failed save (nothing lost; save_fail_n never counts it), so its
		 * stderr line says so rather than "result=fail". */
		fprintf(stderr,
				"[chunk_save] result=%s chunk=(%d,%d) stage=%s erase=%d modified=%d\n",
				w->evict_retire_deferred ? "deferred" : "fail", (int)x, (int)z,
				failure_stage, erase ? 1 : 0, c->modified ? 1 : 0);
		/* lava-aabb-0924/H1: heartbeat-visible failure stage. archive_write
		 * was already noted -- with its finer region stage -- inside
		 * region_archive_set_chunk; every other stage is this function's own. */
		/* stage22-0927: an eviction DEFERRAL (full client inbox) is not a
		 * save failure -- nothing was lost -- so it never reaches
		 * save_fail_n; it is counted as near_evict_defer by the caller. */
		if(strcmp(failure_stage, "archive_write") != 0
		   && !w->evict_retire_deferred)
			region_save_failure_note(failure_stage);
	}
	return save_succeeded;
}

static bool server_world_save_chunk_obj_checked(struct server_world* w,
	bool erase, w_coord_t x, w_coord_t z, struct server_chunk* c) {
	/* G9 (ASTRA-G9-RULING 9, ASTRA-G9-REVIEW Required 1-2): pending light
	 * pins this column, so its light is not coherent. It is NEVER written:
	 * the disk keeps the last coherent image (pending seeds and undo are not
	 * persisted, so writing it would let a crash lose the settle). An
	 * unload waits for the next drain (the existing retry-next-tick
	 * deferral); a non-evicting save leaves the chunk dirty and is retried
	 * after the drain. */
	/* G9 R3: a stopped world (or session) writes nothing: a partially
	 * applied tick is not a coherent image. */
	if(w->light_fault || (w->session && server_local_light_stopped(w->session))) {
		c->modified = true;
		return false;
	}
	if(server_world_light_pinned(w, x, z)) {
		s_light_stats.save_deferred++;
		if(erase)
			w->evict_retire_deferred = true;
		c->modified = true;
		return false;
	}
	tps_note(world_tps_observe, TPS_ENTER, TPS_SAVE);
	ptime_t start = deep_start();
	bool result = server_world_save_chunk_obj_impl(w, erase, x, z, c);
	deep_end(6, start);
	tps_note(world_tps_observe, TPS_LEAVE, TPS_SAVE);
	return result;
}

static bool server_world_save_chunk_obj_history_impl(struct server_world* w, bool erase,
								 w_coord_t x, w_coord_t z,
								 struct server_chunk* c) {
	return server_world_save_chunk_obj_checked(w, erase, x, z, c);
}

bool server_world_save_chunk_obj(struct server_world* w, bool erase, w_coord_t x, w_coord_t z, struct server_chunk* c) {
	const char* operation = erase ? "unload" : "save";
	uint64_t request = G9B_BEGIN(w, operation, x, z);
	bool ok = server_world_save_chunk_obj_history_impl(w, erase, x, z, c);
	G9B_END(w, operation, request, ok ? "ok" : "deferred", x, z);
	(void)operation;
	return ok;
}


/* M3: take the archives-side answer for every chunk that is waiting on a
 * publication. Runs on the SERVER thread at the batch seams (begin/end) and
 * before the checkpoint barrier -- never inside the batch, so a chunk is
 * reconciled at most once per batch window.
 *
 * A handle that is no longer resident was closed by region_archive_destroy,
 * which publishes and REPORTS; the world-level region_evict_failed /
 * region_close_failed flags are that report, so an absent handle means durable
 * unless one of them is set. */
static void server_world_reconcile_pending_saves_ex(struct server_world* w, bool retry) {
	assert(w);
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);
	for(; !dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
		struct server_chunk* sc = &dict_server_chunks_ref(it)->value;
		if(!sc->save_pending)
			continue;
		int64_t id = dict_server_chunks_ref(it)->key;
		w_coord_t x = S_CHUNK_X(id), z = S_CHUNK_Z(id);
		enum region_save_state state = REGION_SAVE_DURABLE;
		bool found = false;
		for(size_t k = 0; k < server_world_region_count(w); k++) {
			struct region_archive* ra = server_world_region_at(w, k);
			if(CHUNK_REGION_COORD(x) != ra->x || CHUNK_REGION_COORD(z) != ra->z)
				continue;
			state = region_archive_save_state(ra, sc->save_job);
			found = true;
			break;
		}
		if(!found && (w->region_evict_failed || w->region_close_failed))
			state = REGION_SAVE_FAILED;
		if(state == REGION_SAVE_PENDING)
			continue;
		sc->save_pending = false;
		/* The only action is on FAILURE: the batch was rolled back, so this
		 * chunk is still un-named on disk and must be written again. */
		if(state == REGION_SAVE_FAILED)
			sc->modified = true;
	}
	server_world_reconcile_retiring_ex(w, retry);
}
void server_world_reconcile_pending_saves(struct server_world* w) {
	server_world_reconcile_pending_saves_ex(w, true);
}

/* ---- G9 R1: coherent checkpoints without an early live drain ----
 *
 * ASTRA-G9-REVIEW Required 1. A checkpoint (actor transfer, guest leave,
 * unload / death / portal teardown, demos) must write every column coherent,
 * yet the live world may resume simulation after it (a non-terminal
 * checkpoint, or a teardown whose checkpoint failed), so it may not settle
 * the pending light early. The checkpoint therefore settles a TRIAL:
 *   1. reserve a Sky/Block snapshot for every resident chunk the pending
 *      drain can write (the 3x3 around each pin); capture a page before its
 *      first changing trial store, or copy all planes on metadata OOM;
 *   2. run the ordinary drain (so the image is exactly the settled field the
 *      next prescribed drain will produce, byte for byte);
 *   3. write the chunks (now unpinned, coherent);
 *   4. roll the live world back: restore the snapshot nibbles and the
 *      seeds / undo / pins (the drain only reset their counts).
 * Simulation never observes the trial: nothing runs between 1 and 4 but the
 * saves. A crash after 3 reloads the settled image of the same edits (the
 * pending seeds/undo are not persisted, and need not be). A failed save
 * leaves the chunks dirty and the queue pending; the caller retains its
 * transaction and retries. A snapshot allocation failure fails the
 * checkpoint (retryable) without touching the world. */
struct server_light_trial {
	bool active;
	size_t n;
	int64_t* ids;
	uint8_t* light; /* n private packed plane pairs (reference: one byte/cell) */
	struct server_light_trial_capture* captures;
	size_t seed_n, undo_n, pin_n;
	struct sbl_cursor generation; /* G9X-5: cursor + resident window only */
	ptime_t t0; /* G9 R6: the trial's start (ckpt_us) */
	bool journal; /* G9X-5b: the before-image is in w->light_journal */
};
_Static_assert(LIGHT_TRIAL_PLANE == SERVER_CHUNK_NIBBLE_BYTES,
			   "the trial snapshot copies whole server nibble planes");

/* g9-ckpt-1009 (T1): roll one chunk back to its snapshot planes. Equal to
 * the reference loop "for idx ascending: server_chunk_store(SKY, snap) then
 * server_chunk_store(TORCH, snap)": a byte where both planes already equal
 * the snapshot holds two cells whose stores are both equality no-ops, so it
 * is skipped (8 bytes per compare); any other byte runs its two cells in
 * ascending order through server_chunk_store_light, which is
 * server_chunk_store(SKY) then server_chunk_store(TORCH) (same equality
 * skips, census calls and per-plane backing writes in the same order). The
 * live planes are re-read after every store, as the reference did. */
/* G9X-5b: the same loop over live bytes [b0, b0 + len) against snapshot
 * segments ss/st that start at b0 (the journal restores a column through a
 * bounded buffer, segment by segment in ascending b: the store sequence is
 * the whole-plane span's, cut at multiples of 8). */
static inline void server_world_light_trial_restore_seg(struct server_chunk* sc,
	const uint8_t* ss, const uint8_t* st, size_t b0, size_t len,
	struct thread_mutex** hold) {
	assert(b0 % 8 == 0 && len % 8 == 0);
	size_t o = 0;
	while(o < len) {
		{
			uint64_t ls, lt, vs, vt;
			memcpy(&ls, sc->lighting_sky + b0 + o, 8);
			memcpy(&lt, sc->lighting_torch + b0 + o, 8);
			memcpy(&vs, ss + o, 8);
			memcpy(&vt, st + o, 8);
			if(((ls ^ vs) | (lt ^ vt)) == 0) {
				o += 8;
				continue;
			}
		}
		const size_t end = o + 8;
		for(; o < end; o++) {
			const size_t b = b0 + o;
			if(sc->lighting_sky[b] == ss[o] && sc->lighting_torch[b] == st[o])
				continue;
			for(unsigned h = 0; h < 2; h++) {
				const unsigned idx = (unsigned)(b * 2 + h);
				server_chunk_store_light(sc, idx, (uint8_t)((ss[o] >> (h * 4)) & 0xF),
										 (uint8_t)((st[o] >> (h * 4)) & 0xF), hold);
			}
		}
	}
}
static inline void server_world_light_trial_restore_span(struct server_chunk* sc,
	const uint8_t* snap, size_t b, size_t limit, struct thread_mutex** hold) {
	assert(b % 8 == 0 && limit % 8 == 0 && b <= limit);
	server_world_light_trial_restore_seg(sc, snap + b, snap + LIGHT_TRIAL_PLANE + b,
		b, limit - b, hold);
}

static void server_world_light_trial_restore_chunk(struct server_chunk* sc,
	const uint8_t* snap, const uint32_t* pages) {
	/* The stripe policy and SKY-before-TORCH store order stay unchanged.
	 * Enumerate captured pages in ascending order; a zero bitmap word skips
	 * 2048 cells at once, rather than probing it again for every 8 bytes. */
	struct thread_mutex* held = NULL;
	struct thread_mutex** const hold = thread_g9_lease_enabled() ? NULL : &held;
	if(pages) {
		for(unsigned word = 0; word < LIGHT_TRIAL_CELLS / LIGHT_TRIAL_PAGE_CELLS / 32; word++) {
			uint32_t bits = pages[word];
			while(bits) {
				const size_t first = (word * 32u + (unsigned)__builtin_ctz(bits))
					* (LIGHT_TRIAL_PAGE_CELLS / 2);
				server_world_light_trial_restore_span(sc, snap, first,
					first + LIGHT_TRIAL_PAGE_CELLS / 2, hold);
				bits &= bits - 1;
			}
		}
	} else server_world_light_trial_restore_span(sc, snap, 0, LIGHT_TRIAL_PLANE, hold);
	if(held)
		thread_stripe_unlock(held);
}

/* ---- G9X-5b: per-increment memory preflight ---------------------------
 * The model server_world_mem_plan_run replays (PPC32 Wii values, read from
 * the INSTALLED devkitPPC libc.a, objdump -dr of libc_a-mallocr.o and
 * libc_a-malignr.o; offsets are in .text._malloc_r / .text._memalign_r,
 * tmp/jobs/loop/G9X-5b/NOTES.txt):
 *  - request padding, _malloc_r+0x0..0x34,0xa8: nb = (bytes + 11) & ~7,
 *    16 when bytes + 11 <= 22 (SIZE_SZ 4, 8 B alignment, MINSIZE 16);
 *  - a top miss, _malloc_r+0x2e0..0x31c: sbrk(nb + __malloc_top_pad + 16
 *    rounded up to 4,096) -- the WHOLE padded request, never nb minus the
 *    old top; __malloc_top_pad is .sbss (0; CaveWii never calls mallopt);
 *    the first sbrk (+0x758) is the same size unrounded (smaller);
 *  - a non-contiguous break (+0x344..0x350 -> +0x5c0..0x608, aligned:
 *    +0x778..0x7a8) is followed by ONE correction sbrk masked to the page
 *    (clrlwi 20): at most 4,095 B;
 *  - _memalign_r+0x50..0x64: malloc(nb + alignment + 16).
 *  - libogc _sbrk_r with MALLOC_MEM2 = 1 (logc256kr/libogc/sbrk.c:16-68):
 *    an increment goes to Arena1 iff lo + incr <= Arena1 hi (equality
 *    fits), else WHOLE to Arena2 iff it fits there; once in Arena2 every
 *    later increment stays there, however small (sticky; only a shrink back
 *    to exactly mem2_start returns to Arena1, and no plan below frees).
 *  - the carve (logc256kr/libogc/lwp_heap.c:54-76, pg 32, used-overhead
 *    8): mem2_alloc(p) asks the heap for p + 256 (MEM2_GUARD), which a
 *    hole takes iff p + 256 <= the largest-allocatable reading
 *    (mem2_carve_largest_allocatable_bytes, request units); it consumes
 *    round32(p + 256) + 40 B, plus an unsplit remainder <= 40 B
 *    (SBL_WII_MEM2_COST). mem2_alloc falls back to memalign(32, p + 256)
 *    on a carve miss; mem2_alloc_strict never does.
 * Conservative where the state is unknown:
 *  - every trace that is still unswitched has an Arena1 balance somewhere
 *    in [mem1_lo, mem1_hi] (lo: every earlier increment grew, hi: every
 *    earlier allocation that could reuse a chunk did). An increment may
 *    land in Arena1 iff it fits the HIGHEST balance; it is admitted only if
 *    the lowest balance it can land on still leaves >= the floor (so an
 *    increment that fits some balances exactly is refused). A switch is
 *    certain only when no free chunk or top could have served the request;
 *    a switch charges the correction to Arena2 as well;
 *  - after a carve the largest hole is lowered to the lower bound of what
 *    is left of the hole it came from (round32(request) + 64 smaller).
 *    That is only a LOWER bound: another hole may still take a later
 *    request above it, so a soft mem2_alloc above the estimate is charged
 *    BOTH ways (the carve debit against the carve floor, and its memalign
 *    growth) -- whichever the heap does. A strict one above it is refused;
 *  - a carve must leave >= the carve floor + the admission reserve. */
#define SWM_PAGE 4096u
#define SWM_MINSIZE 16u
#define SWM_CORRECTION (SWM_PAGE - 1u)
static bool swm_add(size_t a, size_t b, size_t* out) {
	if(a > SIZE_MAX - b) return false;
	*out = a + b;
	return true;
}
static bool swm_mul(size_t a, size_t b, size_t* out) {
	if(b && a > SIZE_MAX / b) return false;
	*out = a * b;
	return true;
}
/* _malloc_r's request padding: max(16, (b + 11) & ~7) */
static bool swm_pad(size_t bytes, size_t* nb) {
	if(!swm_add(bytes, 4u + 7u, nb)) return false;
	*nb &= ~(size_t)7u;
	if(*nb < SWM_MINSIZE) *nb = SWM_MINSIZE;
	return true;
}

bool server_world_mem_plan_run(const struct server_world_mem_state* st,
	const struct server_world_mem_event* events, size_t n,
	struct server_world_mem_plan* out) {
	memset(out, 0, sizeof *out);
	struct server_world_mem_state s = *st;
	bool may_u = s.arena != SERVER_WORLD_MEM_SWITCHED;
	bool may_s = s.arena != SERVER_WORLD_MEM_UNSWITCHED;
	size_t lo = s.mem1_free, hi = s.mem1_free; /* unswitched balances */
	out->mem1_min = s.mem1_free;
	out->carve_min = s.carve_free;
	size_t carve_need = 0;
	const bool need_ok = swm_add(SERVER_WORLD_CARVE_FLOOR, s.carve_reserve, &carve_need);
#define SWM_FAIL(r) do { out->ok = false; out->reason = (r); out->fail_event = i; \
		return false; } while(0)
	for(size_t i = 0; i < n; i++) {
		const size_t reps = events[i].count ? events[i].count : 1;
		for(size_t r = 0; r < reps; r++) {
			struct server_world_mem_step step = {
				.event = (uint32_t)i, .kind = (uint8_t)events[i].kind,
				.requested = events[i].bytes};
			size_t bytes = events[i].bytes; /* what malloc is asked for */
			bool via_malloc = events[i].kind == SERVER_WORLD_MEM_EV_MALLOC;
			bool carved = false;
			if(!via_malloc) {
				size_t req = 0, nb = 0;
				if(!swm_add(bytes, 256u, &req) || req > SIZE_MAX / 2) SWM_FAIL(SERVER_WORLD_MEM_OVERFLOW);
				if(!s.carve_valid) SWM_FAIL(SERVER_WORLD_MEM_CARVE_UNKNOWN);
				const bool sure = req <= s.carve_largest; /* a known hole takes it */
				if(!sure && events[i].kind == SERVER_WORLD_MEM_EV_MEM2_STRICT)
					SWM_FAIL(SERVER_WORLD_MEM_CARVE_LARGEST);
				/* charged to the carve whenever a hole MAY take it */
				const size_t debit = SBL_WII_MEM2_COST(bytes);
				if(!need_ok || s.carve_free < debit || s.carve_free - debit < carve_need)
					SWM_FAIL(SERVER_WORLD_MEM_CARVE_FLOOR);
				s.carve_free -= debit;
				step.carve_debit = debit;
				carved = true;
				if(sure) {
					const size_t r32 = (req + 31u) / 32u * 32u;
					s.carve_largest = s.carve_largest >= r32 + 64u ? s.carve_largest - r32 - 64u : 0;
				} else {
					/* ...and may miss: memalign(32, p + 256) = malloc(nb + 32 + 16) */
					via_malloc = true;
					if(!swm_pad(req, &nb) || !swm_add(nb, 32u + SWM_MINSIZE, &bytes))
						SWM_FAIL(SERVER_WORLD_MEM_OVERFLOW);
				}
			}
			if(via_malloc) {
				size_t nb = 0, g = 0;
				if(!swm_pad(bytes, &nb)) SWM_FAIL(SERVER_WORLD_MEM_OVERFLOW);
				if(!swm_add(nb, SWM_MINSIZE + SWM_PAGE - 1u, &g)
				   || g > SIZE_MAX - SWM_CORRECTION)
					SWM_FAIL(SERVER_WORLD_MEM_OVERFLOW);
				g = g / SWM_PAGE * SWM_PAGE;
				const bool hit = s.bins_free >= nb
					|| (s.top_free >= SWM_MINSIZE && s.top_free - SWM_MINSIZE >= nb);
				bool next_u = false, at1 = false;
				size_t a2 = 0, nlo = lo, nhi = hi;
				if(may_u) {
					if(g <= hi) { /* lands in Arena1 on some balance in [max(lo, g), hi] */
						const size_t worst = (lo > g ? lo : g) - g;
						if(worst < SERVER_WORLD_MEM1_FLOOR)
							SWM_FAIL(SERVER_WORLD_MEM_MEM1_FLOOR);
						nlo = worst;
						nhi = hit ? hi : hi - g;
						next_u = at1 = true;
					}
					if(g > lo) { /* switches on some balance */
						a2 = g + SWM_CORRECTION; /* non-contiguous: one correction */
						may_s = true;
						if(!at1) { next_u = hit; nlo = lo; nhi = hi; }
					}
					if(at1 && hit && lo < nlo) nlo = lo; /* the reuse trace keeps lo */
				}
				if(may_s && a2 < g)
					a2 = g; /* already in Arena2: contiguous growth */
				if(a2 > s.arena2_free) SWM_FAIL(SERVER_WORLD_MEM_ARENA2_FULL);
				s.arena2_free -= a2;
				may_u = next_u;
				lo = nlo;
				hi = nhi;
				step.growth = at1 && a2 ? (g > a2 ? g : a2) : at1 ? g : a2;
				step.where = at1 && a2 ? SERVER_WORLD_MEM_AT_EITHER
					: at1 ? SERVER_WORLD_MEM_AT_MEM1 : SERVER_WORLD_MEM_AT_ARENA2;
				if(carved) step.where = SERVER_WORLD_MEM_AT_EITHER;
				if(s.top_free != SIZE_MAX)
					s.top_free = s.top_free > SIZE_MAX - (g - nb) ? SIZE_MAX : s.top_free + (g - nb);
			} else
				step.where = SERVER_WORLD_MEM_AT_CARVE;
			s.mem1_free = may_u ? lo : s.mem1_free;
			if(s.mem1_free < out->mem1_min) out->mem1_min = s.mem1_free;
			if(s.carve_free < out->carve_min) out->carve_min = s.carve_free;
			step.mem1_after = s.mem1_free;
			step.arena2_after = s.arena2_free;
			step.carve_after = s.carve_free;
			step.largest_after = s.carve_largest;
			if(out->steps < SERVER_WORLD_MEM_TRACE_MAX) out->step[out->steps] = step;
			out->steps++;
		}
	}
#undef SWM_FAIL
	out->ok = true;
	out->reason = SERVER_WORLD_MEM_OK;
	return true;
}

/* THE switch. G9X-5b part 2: ON. A trial the planner refuses (or one that
 * runs the gameplay flood frontier, or fails its pre-drain revalidation)
 * takes the pre-reserved checkpoint journal instead (bounded progress, no
 * new allocation). It only gates where the sampler can read fixed heaps
 * (the Wii, or a test model): PC production is unchanged. */
static bool s_mem_preflight_enabled = true;
static struct server_world_test_mem_report s_mem_report;
#ifdef CAVEWII_SERVER_LOCAL_TEST
void server_world_test_mem_preflight(bool on) {
	s_mem_preflight_enabled = on;
}
static struct server_world_mem_state s_mem_model[4];
static unsigned s_mem_model_n, s_mem_model_k;
void server_world_test_mem_model(const struct server_world_mem_state* states,
	unsigned n) {
	assert(n <= 4);
	if(n) memcpy(s_mem_model, states, n * sizeof *states);
	s_mem_model_n = n;
	s_mem_model_k = 0;
	memset(&s_mem_report, 0, sizeof s_mem_report);
}
void server_world_test_mem_last(struct server_world_test_mem_report* out) {
	*out = s_mem_report;
}
#endif
#define SWM_REPORT(field) (s_mem_report.field)

/* A reserved, not-yet-resident column (server_transfer.c column_reserve)
 * takes a carved, free MEM2_POOL_COLUMN slot when the pool has one (no new
 * carve: the slab is already debited). Otherwise mem2_allocate_pooled
 * (mem2.c ~976-994) first calls mem2_pool_grow while the pool is below its
 * cap: a WHOLE slab (lead + MEM2_POOL_SLAB_SLOTS strides, ~2.64 MB) carved
 * from any one hole that fits it, regardless of the floor; only when that
 * fails does the column fall back to ONE unpooled MEM2_POOL_COLUMN_OBJECT
 * (server_chunk_arrays_alloc_slot; the Wii has no five-array shape). Both
 * paths are charged and the larger kept: a slab that fits is not a slab
 * that leaves the floor. */
size_t server_world_mem_column_reserve(size_t reservations, size_t pool_free_slots,
	size_t slab_room, size_t slab_request) {
	const size_t u = reservations > pool_free_slots ? reservations - pool_free_slots : 0;
	size_t unpooled = 0, slab = 0, rest = 0, t = 0;
	if(!swm_mul(u, SBL_WII_MEM2_COST(MEM2_POOL_COLUMN_OBJECT), &unpooled)) return SIZE_MAX;
	if(!u || !slab_room || !slab_request) return unpooled;
	size_t slabs = (u + MEM2_POOL_SLAB_SLOTS - 1u) / MEM2_POOL_SLAB_SLOTS;
	if(slabs > slab_room) slabs = slab_room;
	const size_t covered = slabs * MEM2_POOL_SLAB_SLOTS;
	if(slab_request > SIZE_MAX - 256u - 80u
	   || !swm_mul(slabs, SBL_WII_MEM2_COST(slab_request), &slab)
	   || !swm_mul(u > covered ? u - covered : 0, SBL_WII_MEM2_COST(MEM2_POOL_COLUMN_OBJECT), &rest)
	   || !swm_add(slab, rest, &t))
		return SIZE_MAX;
	return t > unpooled ? t : unpooled;
}

/* false = no fixed heaps to gate on (PC production): behave as before. */
static bool server_world_mem_sample(const struct server_world* w,
	struct server_world_mem_state* out) {
	memset(out, 0, sizeof *out);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_mem_model_n) {
		const unsigned k = s_mem_model_k < s_mem_model_n ? s_mem_model_k : s_mem_model_n - 1;
		s_mem_model_k++;
		*out = s_mem_model[k];
		return true;
	}
#endif
#ifdef PLATFORM_WII
	out->mem1_free = SYS_GetArena1Size();
	out->arena2_free = SYS_GetArena2Size();
	out->bins_free = out->top_free = SIZE_MAX; /* newlib bins: not sampled */
	out->arena = SERVER_WORLD_MEM_UNKNOWN;     /* _sbrk_r's mem2_start is private */
	size_t largest = 0;
	const size_t free_b = mem2_carve_free_bytes();
	out->carve_valid = mem2_carve_largest_allocatable_bytes(&largest) && free_b != SIZE_MAX;
	out->carve_free = out->carve_valid ? free_b : 0;
	out->carve_largest = out->carve_valid ? largest : 0;
	struct mem2_pool_stat ps;
	mem2_pool_column_stats(&ps);
	const size_t carved = (size_t)ps.slabs * MEM2_POOL_SLAB_SLOTS;
	/* one slab request: the carved slab size when known, else a slab of
	 * 32-byte-rounded objects; + 32 for the slab's alignment lead */
	const size_t slab_req = (ps.slabs ? (size_t)(ps.bytes / ps.slabs)
		: (size_t)MEM2_POOL_SLAB_SLOTS * ((MEM2_POOL_COLUMN_OBJECT + 31u) / 32u * 32u)) + 32u;
	out->carve_reserve = server_world_mem_column_reserve(
		w->session ? w->session->transfer_column_reservation_n : 0,
		carved > ps.used ? carved - ps.used : 0,
		ps.slabs_max > ps.slabs ? ps.slabs_max - ps.slabs : 0, slab_req);
	return true;
#else
	(void)w;
	return false;
#endif
}

/* The trial's resident columns without allocating: generation work walks
 * every resident column; otherwise the deduplicated resident 3x3 around
 * each pin, exactly the set server_world_light_trial_begin collects. */
static size_t server_world_light_trial_columns(struct server_world* w) {
	if(w->generation_light_n)
		return dict_server_chunks_size(w->chunks);
	size_t n = 0;
	const size_t k_n = w->light_pin_n * 9;
	for(size_t k = 0; k < k_n; k++) {
		const int64_t id = S_CHUNK_ID(S_CHUNK_X(w->light_pins[k / 9]) + (int)(k % 9 / 3) - 1,
			S_CHUNK_Z(w->light_pins[k / 9]) + (int)(k % 3) - 1);
		bool first = true;
		for(size_t j = 0; j < k && first; j++)
			first = S_CHUNK_ID(S_CHUNK_X(w->light_pins[j / 9]) + (int)(j % 9 / 3) - 1,
				S_CHUNK_Z(w->light_pins[j / 9]) + (int)(j % 3) - 1) != id;
		if(first && server_world_resolve_chunk(w, S_CHUNK_X(id), S_CHUNK_Z(id)))
			n++;
	}
	return n;
}

/* PPC32 sizeof(struct server_light_trial_capture): a 4 B pointer + bitmap. */
#define SWM_CAPTURE_PPC (SBL_WII_PTR + sizeof(((struct server_light_trial_capture*)0)->pages))

/* The trial's allocations in production order (brief items 7-12): IDs,
 * the tail pages sbl_ensure_tail loads, the cursor window, the slot bitmap
 * (iff backing), the planes, the optional captures; then (from *lazy) what
 * the trial drain may still allocate: every empty ring slot and, without
 * backing, the backing struct and its FILE/devoptab state (5a ledger
 * bounds). Returns the count, 0 on size overflow. */
static size_t server_world_light_trial_events(struct server_world* w, size_t cap,
	size_t n, bool captures, struct server_world_mem_event* ev, size_t* lazy) {
	size_t m = 0, b = 0;
	if(!swm_mul(cap, sizeof(int64_t), &b)) return 0;
	ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC, b, 1};
	if(w->generation_light_n) {
		const size_t tail = sbl_plan_tail_pages(w);
		if(tail)
			ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MEM2, SBL_PAGE_BYTES, tail};
		size_t base = w->generation_light_base;
		while(base > 0 && w->generation_light_n < base * SBL_PAGE + SBL_TAIL + 1) base--;
		const size_t count = w->generation_light_n - base * SBL_PAGE;
		ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC,
			count * sizeof(struct server_beta_light_region) + 1, 1};
		if(w->generation_light_backing)
			ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC, SBL_BITMAP_BYTES, 1};
	}
	if(!swm_mul(n, LIGHT_TRIAL_CELLS, &b)) return 0;
	ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC, b, 1};
	if(captures) {
		if(!swm_mul(n, SWM_CAPTURE_PPC, &b)) return 0;
		ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC, b, 1};
	}
	*lazy = m;
	/* The gameplay flood frontier (lighting_drain_seed -> lighting_drain_
	 * reserve, lighting.c:163 -> stack.c:102) grows by realloc doubling, old
	 * and new coexisting. Allowance: the growth to hold the seeds (+5, the
	 * first pop's reserve), then ONE further doubling. A flood that needs
	 * more fails its reserve losslessly (DRAIN_OOM keeps the frontier), as
	 * in HEAD; the allowance is a planning bound, not a cap. */
	if(w->light_seed_n || w->light_frontier_live) {
		const struct stack* q = &w->light_frontier;
		const size_t el = q->element_size ? q->element_size : sizeof(w_coord_t) * 3u;
		const size_t need = (w->light_frontier_live ? q->index : w->light_seed_n) + 5u;
		size_t len = q->length && q->data ? q->length : 16u;
		while(len < need) {
			if(len > SIZE_MAX / 2u) return 0;
			len *= 2u;
		}
		if(len > (q->data ? q->length : 0u)) {
			if(!swm_mul(len, el, &b)) return 0;
			ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC, b, 1};
		}
		if(len > SIZE_MAX / 2u || !swm_mul(len * 2u, el, &b)) return 0;
		ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC, b, 1};
	}
	if(w->generation_light_n) {
		const size_t free_slots = sbl_plan_free_slots(w);
		if(free_slots)
			ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MEM2, SBL_PAGE_BYTES, free_slots};
		if(!w->generation_light_backing) {
			ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC, SBL_WII_BACKING_BYTES, 1};
			ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC,
				SBL_WII_GLUE_HDR + 4u * SBL_WII_SFILE_MAX, 1};
			ev[m++] = (struct server_world_mem_event) {SERVER_WORLD_MEM_EV_MALLOC,
				SBL_WII_HANDLE_HDR_MAX + SBL_WII_FAT_STRUCT, 1};
		}
	}
	return m;
}
#define SWM_TRIAL_EVENTS_MAX 12

/* Pseudocode E: after the admitted allocations and before the drain (the
 * first simulation write), re-sample: whatever consumed memory since the
 * plan (another thread's carve, a stale sample) must leave the floors held
 * (or not lowered below where the plan started) and still admit what the
 * drain may allocate lazily. */
static bool swm_revalidate(const struct server_world_mem_state* was,
	const struct server_world_mem_state* now,
	const struct server_world_mem_event* lazy, size_t n,
	struct server_world_mem_plan* out) {
	if(!server_world_mem_plan_run(now, lazy, n, out)) {
		out->reason = SERVER_WORLD_MEM_STALE;
		return false;
	}
	/* a floor counts only when it is NEWLY breached: one already below it
	 * before (`was`) is where HEAD runs anyway, so lowering it further is
	 * not a reason to free HEAD's allocations and churn malloc/free */
	size_t need = 0, was_need = 0;
	const bool mem1 = now->mem1_free >= SERVER_WORLD_MEM1_FLOOR
		|| now->mem1_free >= was->mem1_free
		|| was->mem1_free < SERVER_WORLD_MEM1_FLOOR;
	const bool carve = !now->carve_valid || !was->carve_valid
		|| now->carve_free >= was->carve_free
		|| (swm_add(SERVER_WORLD_CARVE_FLOOR, now->carve_reserve, &need)
			&& now->carve_free >= need)
		|| (swm_add(SERVER_WORLD_CARVE_FLOOR, was->carve_reserve, &was_need)
			&& was->carve_free < was_need);
	if(mem1 && carve) return true;
	out->ok = false;
	out->reason = SERVER_WORLD_MEM_STALE;
	out->fail_event = n;
	return false;
}

/* Sampler with its cost recorded (per checkpoint trial, never per
 * allocation: one sample to plan, at most one to revalidate the full copy,
 * and -- once per world -- one after acquiring the journal). */
static bool swm_sample_timed(const struct server_world* w, struct server_world_mem_state* out) {
	const ptime_t t0 = time_get();
	const bool r = server_world_mem_sample(w, out);
	SWM_REPORT(samples)++;
	SWM_REPORT(sample_us) += (size_t)time_diff_us(t0, time_get());
	return r;
}

/* ---- G9X-5b part 2: the pressured checkpoint journal -------------------
 * GOVERNING RULE: never worse than HEAD. A checkpoint whose in-RAM full copy
 * the planner admits runs HEAD's path unchanged (same allocations, same
 * cost). Only a planner REFUSAL (HEAD's mallocs would fail or breach a
 * floor) or a failed pre-drain revalidation takes the journal; when the
 * journal cannot be used -- reservation refused/failed, prepare or read-back
 * verify failed, all before any world mutation -- the trial runs HEAD's RAM
 * path exactly as HEAD would (ungated).
 *   - RAM: ONE block (struct server_light_journal, mem2_alloc_strict: never
 *     a silent MEM1 spill) + the FILE opened with it (unbuffered,
 *     SBL_WII_FILE_BYTES of SDK state; libfat read/write/seek import no
 *     allocator, 5a evidence), reserved at the world's first refused
 *     checkpoint through the SAME planner (strict carve + the two FILE
 *     mallocs) and revalidated by a fresh sample after acquiring; kept until
 *     teardown. A journal checkpoint allocates nothing else for its
 *     before-image (no IDs, cursor window, bitmap, planes or captures: the
 *     8,867,794 B full-copy payload at 256 columns);
 *   - disk, "<level>/cw_light_journal_<slot>_<dim>.tmp" (one name per world
 *     slot and dimension, like 5a's cw_light_regions_%u_%d.tmp: the next
 *     open truncates an orphan, and server_world_create removes it):
 *       [0, 64)  header, fixed-width little-endian: "CWLJ", version 1, slot,
 *                dimension, world generation, trial sequence, column count,
 *                queue n, base, window count (u64 each), slot-bits flag,
 *                FNV-1a of the queue section, FNV-1a of bytes [0, 60);
 *       [64, R)  the queue cursor's resident window, 28 B per entry, written
 *                straight from the resident pages (R = 64 + round32);
 *       R + k * 32,800: record k = {column id u64, sequence, length 32,768,
 *                Sky FNV, Block FNV (both seeded with the id), index k,
 *                FNV-1a of the 28 header bytes} + Sky plane + Block plane,
 *                in the exact column order the full copy uses.
 *   The record header is the index (fixed stride); the file header is
 *   written LAST, so a partial record is never valid. Every record carries
 *   the trial sequence (no version reuse) and its index, both inside its
 *   checksum, as is its column id.
 * PREPARE writes the before-image, flushes, then READS IT ALL BACK and
 * verifies every checksum -- still before the drain, with the world
 * untouched, so a failure falls back to HEAD's path. RESTORE first
 * re-verifies the queue section, then per record checks the header
 * checksum and that the column is resident BEFORE applying it, and applies
 * the unchanged equality-skipping span (server_world_light_trial_restore_seg,
 * 1,792 B Sky + 1,792 B Block segments): HEAD's eager-restore stores in
 * HEAD's order. A restore that still fails (an SD read fault after the
 * verified read-back, or tampering) has changed live light: the world takes
 * the sticky SERVER_LIGHT_FAULT_CHECKPOINT_JOURNAL stop and the checkpoint
 * reports failure (the residual window: the save between verify and restore
 * may already have published the settled image, which is a valid settled
 * state). The drain's own lazy queue pages / backing open and the flood
 * frontier beyond its allowance are the live drain's growth, gated as in
 * HEAD (part 3 owns the queue-growth planner). */
#define SWJ_VERSION 1u
#define SWJ_HEADER 64u
#define SWJ_RECORD_HEADER 32u
#define SWJ_RECORD (SWJ_RECORD_HEADER + 2u * LIGHT_TRIAL_PLANE)
#define SWJ_BUF 3584u
#define SWJ_SEG (SWJ_BUF / 2u)
_Static_assert(SWJ_SEG % 8u == 0 && LIGHT_TRIAL_PLANE % 8u == 0, "8-byte restore spans");
struct server_light_journal {
	FILE* file;
	uint32_t sequence; /* the current trial's version (every record has it) */
	bool has_loc, verifying;
	long records;      /* offset of record 0 */
	uint32_t prepare_us, verify_us; /* this trial's SD phases */
	uint32_t queue_fnv; /* the verified queue section's checksum (restore) */
	uint8_t loc[SBL_BITMAP_BYTES];
	uint8_t buf[SWJ_BUF];
	char path[256];
};
/* PPC32: 4 + 4 + 2 (+2) + 4 + 8 + 4 + 977 + 3,584 + 256 (+ 3 tail pad) = 4,848 B */
#define SWJ_WII_BYTES 4848u
#ifdef PLATFORM_WII
_Static_assert(sizeof(struct server_light_journal) <= SWJ_WII_BYTES,
	"the PPC journal block within its ledger bound");
#endif
#define SWJ_WII_COST (SBL_WII_MEM2_COST(SWJ_WII_BYTES) + SBL_WII_FILE_BYTES)
_Static_assert(SBL_WII_MEM2_COST(SWJ_WII_BYTES) == 5200u,
	"one strict carve block: 4,848 B payload, 256 B guard, <= 5,200 B physical");

#ifdef CAVEWII_SERVER_LOCAL_TEST
static long s_swj_fault_at[SERVER_WORLD_TEST_JOURNAL_FAULTS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
_Static_assert(SERVER_WORLD_TEST_JOURNAL_FAULTS == 10, "one initializer per journal fault");
static size_t s_swj_fault_n[SERVER_WORLD_TEST_JOURNAL_FAULTS];
static int s_swj_tamper;
static bool swj_fault(enum server_world_test_journal_fault k) {
	const size_t at = s_swj_fault_n[k]++;
	return s_swj_fault_at[k] >= 0 && at == (size_t)s_swj_fault_at[k];
}
void server_world_test_journal_fault(enum server_world_test_journal_fault kind, long at) {
	s_swj_fault_at[kind] = at;
	s_swj_fault_n[kind] = 0;
}
void server_world_test_journal_tamper(int kind) {
	s_swj_tamper = kind;
}
const char* server_world_test_journal_path(const struct server_world* w) {
	return w->light_journal ? w->light_journal->path : "";
}
#define SWJ_FAULT(k) swj_fault(SERVER_WORLD_TEST_JOURNAL_##k)
#else
#define SWJ_FAULT(k) false
#endif

static void swj_put32(uint8_t* p, uint32_t v) {
	for(unsigned k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (8 * k));
}
static void swj_put64(uint8_t* p, uint64_t v) {
	for(unsigned k = 0; k < 8; k++) p[k] = (uint8_t)(v >> (8 * k));
}
static uint32_t swj_get32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t swj_get64(const uint8_t* p) {
	return (uint64_t)swj_get32(p) | (uint64_t)swj_get32(p + 4) << 32;
}
static uint32_t swj_fnv(uint32_t h, const uint8_t* p, size_t n) {
	for(size_t k = 0; k < n; k++) h = (h ^ p[k]) * 16777619u;
	return h;
}
#define SWJ_FNV0 2166136261u
/* a plane checksum is seeded with its column id */
static uint32_t swj_seed(int64_t id) {
	uint8_t b[8];
	swj_put64(b, (uint64_t)id);
	return swj_fnv(SWJ_FNV0, b, 8);
}

static bool swj_write(struct server_light_journal* j, long off, const void* data, size_t len) {
	if(SWJ_FAULT(SEEK) || fseek(j->file, off, SEEK_SET) != 0) return false;
	if(SWJ_FAULT(SHORT_WRITE)) {
		(void)fwrite(data, 1, len / 2, j->file); /* a real short write */
		return false;
	}
	if(SWJ_FAULT(WRITE) || fwrite(data, 1, len, j->file) != len) return false;
	SWM_REPORT(journal_written) += len;
	return true;
}
static bool swj_read(struct server_light_journal* j, long off, void* data, size_t len) {
	if(fseek(j->file, off, SEEK_SET) != 0) return false;
	if(j->verifying ? SWJ_FAULT(VERIFY) : SWJ_FAULT(READ)) return false;
	/* a real short read */
	const size_t want = !j->verifying && SWJ_FAULT(SHORT_READ) ? len / 2 : len;
	if(fread(data, 1, want, j->file) != len) return false;
	SWM_REPORT(journal_read) += len;
	return true;
}

static void server_world_light_journal_free(struct server_world* w) {
	struct server_light_journal* j = w->light_journal;
	if(j) {
		fclose(j->file);
		remove(j->path);
		sbl_account(-(ptrdiff_t)sizeof *j, -(ptrdiff_t)SWJ_WII_COST);
		mem2_free(j);
	}
	w->light_journal = NULL;
	w->light_journal_failed = false;
}

/* The journal's own admission: the strict carve block and the FILE's two
 * SDK mallocs through the same per-increment planner, against the trial's
 * sample `ms`; after acquiring, a fresh sample must still hold the floors
 * (else the block is given back). false: no journal (HEAD's path runs). */
static bool server_world_light_journal_reserve(struct server_world* w,
	const struct server_world_mem_state* ms) {
	if(w->light_journal) return true;
	const struct server_world_mem_event ev[3] = {
		{SERVER_WORLD_MEM_EV_MEM2_STRICT, SWJ_WII_BYTES, 1},
		{SERVER_WORLD_MEM_EV_MALLOC, SBL_WII_GLUE_HDR + 4u * SBL_WII_SFILE_MAX, 1},
		{SERVER_WORLD_MEM_EV_MALLOC, SBL_WII_HANDLE_HDR_MAX + SBL_WII_FAT_STRUCT, 1}};
	struct server_world_mem_plan p;
	if(!server_world_mem_plan_run(ms, ev, 3, &p)) {
		SWM_REPORT(journal_reserve_refused)++;
		return false;
	}
	struct server_light_journal* j = SWJ_FAULT(RESERVE) ? NULL : mem2_alloc_strict(sizeof *j);
	if(!j) {
		SWM_REPORT(journal_reserve_fail)++;
		return false;
	}
	memset(j, 0, sizeof *j);
	const int len = snprintf(j->path, sizeof j->path, "%s/cw_light_journal_%u_%d.tmp",
		string_get_cstr(w->level_name), (unsigned)w->world_index, (int)w->dimension);
	j->file = len > 0 && (size_t)len < sizeof j->path && !SWJ_FAULT(OPEN)
		? fopen(j->path, "w+b") : NULL;
	if(!j->file) {
		mem2_free(j);
		SWM_REPORT(journal_reserve_fail)++;
		return false;
	}
	setvbuf(j->file, NULL, _IONBF, 0);
	w->light_journal = j;
	sbl_account((ptrdiff_t)sizeof *j, (ptrdiff_t)SWJ_WII_COST);
	struct server_world_mem_state now;
	struct server_world_mem_plan p2;
	if(swm_sample_timed(w, &now) && !swm_revalidate(ms, &now, NULL, 0, &p2)) {
		server_world_light_journal_free(w);
		SWM_REPORT(journal_reserve_refused)++;
		return false;
	}
	return true;
}

/* The queue cursor: sbl_trial_begin without its window/bitmap mallocs. */
static bool swj_queue_begin(struct server_world* w, struct server_light_journal* j,
	struct sbl_cursor* c, uint32_t* qfnv) {
	memset(c, 0, sizeof *c);
	j->has_loc = false;
	*qfnv = SWJ_FNV0;
	if(!w->generation_light_n) return true;
	if(!sbl_ensure_tail(w)) return false;
	c->n = w->generation_light_n;
	c->base = w->generation_light_base;
	c->count = c->n - c->base * SBL_PAGE;
	for(size_t k = 0; k < c->count;) {
		const size_t m = c->count - k < SBL_PAGE ? c->count - k : SBL_PAGE;
		const void* src = sbl_at(w, c->base * SBL_PAGE + k);
		*qfnv = swj_fnv(*qfnv, src, m * sizeof(struct server_beta_light_region));
		if(!swj_write(j, (long)(SWJ_HEADER + k * sizeof(struct server_beta_light_region)),
			   src, m * sizeof(struct server_beta_light_region)))
			return false;
		k += m;
	}
	if(w->generation_light_backing) {
		memcpy(j->loc, w->generation_light_backing->loc, SBL_BITMAP_BYTES);
		j->has_loc = true;
		memset(w->generation_light_backing->flipped, 0, SBL_BITMAP_BYTES);
		w->generation_light_backing->trial = true;
	}
	return true;
}
/* The queue section's checksum, read through the buffer (no live write). */
static bool swj_queue_verify(struct server_light_journal* j, size_t count, uint32_t want) {
	uint32_t h = SWJ_FNV0;
	const size_t total = count * sizeof(struct server_beta_light_region);
	for(size_t o = 0; o < total;) {
		const size_t m = total - o < SWJ_BUF ? total - o : SWJ_BUF;
		if(!swj_read(j, (long)(SWJ_HEADER + o), j->buf, m)) return false;
		h = swj_fnv(h, j->buf, m);
		o += m;
	}
	return h == want;
}
#ifdef CAVEWII_SERVER_LOCAL_TEST
static void swj_tamper_queue(struct server_light_journal* j);
#endif
/* The live queue back from the journal: each page is read into the bounded
 * transfer buffer and checksummed THERE, then copied; the running checksum
 * of exactly the copied bytes must equal the verified section checksum, or
 * the restore fails (the caller's sticky journal stop: never a silent
 * success with unauthenticated queue bytes). */
static bool swj_queue_restore(struct server_world* w, struct server_light_journal* j,
	const struct sbl_cursor* c) {
	if(!c->n) return true;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	swj_tamper_queue(j);
#endif
	w->generation_light_n = c->n;
	w->generation_light_base = c->base;
	uint32_t h = SWJ_FNV0;
	for(size_t k = 0; k < c->count;) {
		const size_t m = c->count - k < SBL_PAGE ? c->count - k : SBL_PAGE;
		const size_t bytes = m * sizeof(struct server_beta_light_region);
		_Static_assert(SBL_PAGE * sizeof(struct server_beta_light_region) <= SWJ_BUF,
			"one queue page fits the transfer buffer");
		if(!swj_read(j, (long)(SWJ_HEADER + k * sizeof(struct server_beta_light_region)),
			   j->buf, bytes))
			return false;
		h = swj_fnv(h, j->buf, bytes);
		memcpy(sbl_at(w, c->base * SBL_PAGE + k), j->buf, bytes);
		k += m;
	}
	if(h != j->queue_fnv) return false;
	if(j->has_loc) memcpy(w->generation_light_backing->loc, j->loc, SBL_BITMAP_BYTES);
	return true;
}

static void swj_record_header(struct server_light_journal* j, uint8_t* h, size_t k,
	int64_t id, uint32_t cs, uint32_t ct) {
	memset(h, 0, SWJ_RECORD_HEADER);
	swj_put64(h, (uint64_t)id);
	swj_put32(h + 8, j->sequence);
	swj_put32(h + 12, (uint32_t)(2u * LIGHT_TRIAL_PLANE));
	swj_put32(h + 16, cs);
	swj_put32(h + 20, ct);
	swj_put32(h + 24, (uint32_t)k);
	swj_put32(h + 28, swj_fnv(SWJ_FNV0, h, 28));
}
/* a record header that is this trial's record k, intact */
static bool swj_record_ok(const struct server_light_journal* j, const uint8_t* h, size_t k) {
	return swj_get32(h + 28) == swj_fnv(SWJ_FNV0, h, 28) && swj_get32(h + 8) == j->sequence
		&& swj_get32(h + 12) == 2u * LIGHT_TRIAL_PLANE && swj_get32(h + 24) == (uint32_t)k;
}
static bool swj_put_record(struct server_light_journal* j, size_t k, int64_t id,
	const struct server_chunk* sc) {
	if(k >= (size_t)((LONG_MAX - j->records) / (long)SWJ_RECORD)) return false;
	const long off = j->records + (long)(k * SWJ_RECORD);
	const uint32_t seed = swj_seed(id);
	swj_record_header(j, j->buf, k, id, swj_fnv(seed, sc->lighting_sky, LIGHT_TRIAL_PLANE),
		swj_fnv(seed, sc->lighting_torch, LIGHT_TRIAL_PLANE));
	return swj_write(j, off + (long)SWJ_RECORD_HEADER, sc->lighting_sky, LIGHT_TRIAL_PLANE)
		&& swj_write(j, off + (long)(SWJ_RECORD_HEADER + LIGHT_TRIAL_PLANE),
			sc->lighting_torch, LIGHT_TRIAL_PLANE)
		&& swj_write(j, off, j->buf, SWJ_RECORD_HEADER);
}
static bool swj_header_ok(const struct server_light_journal* j, const uint8_t* h, size_t n) {
	return memcmp(h, "CWLJ", 4) == 0 && swj_get32(h + 4) == SWJ_VERSION
		&& swj_get32(h + 20) == j->sequence && swj_get32(h + 24) == (uint32_t)n
		&& swj_get32(h + 60) == swj_fnv(SWJ_FNV0, h, 60);
}

/* VERIFY: read the whole journal back (world untouched): header, queue
 * section, every record header and both planes against their checksums. */
static bool swj_verify(struct server_light_journal* j, size_t n, size_t qcount) {
	uint8_t* h = j->buf;
	if(!swj_read(j, 0, h, SWJ_HEADER) || !swj_header_ok(j, h, n)) return false;
	const uint32_t qfnv = swj_get32(h + 56);
	if(!swj_queue_verify(j, qcount, qfnv)) return false;
	for(size_t k = 0; k < n; k++) {
		const long off = j->records + (long)(k * SWJ_RECORD);
		if(!swj_read(j, off, h, SWJ_RECORD_HEADER) || !swj_record_ok(j, h, k)) return false;
		const int64_t id = (int64_t)swj_get64(h);
		const uint32_t ws = swj_get32(h + 16), wt = swj_get32(h + 20);
		for(unsigned plane = 0; plane < 2; plane++) {
			uint32_t c = swj_seed(id);
			for(size_t o = 0; o < LIGHT_TRIAL_PLANE;) {
				const size_t m = LIGHT_TRIAL_PLANE - o < SWJ_BUF ? LIGHT_TRIAL_PLANE - o : SWJ_BUF;
				if(!swj_read(j, off + (long)(SWJ_RECORD_HEADER + plane * LIGHT_TRIAL_PLANE + o),
					   h, m))
					return false;
				c = swj_fnv(c, h, m);
				o += m;
			}
			if(c != (plane ? wt : ws)) return false;
		}
	}
	return true;
}

/* PREPARE: the queue cursor, every column's planes (the full copy's column
 * order), the header (last), a flush, then the read-back verify. false:
 * nothing of the world changed (the caller releases the cursor's trial
 * ownership and runs HEAD's path). */
static bool server_world_light_journal_prepare(struct server_world* w,
	struct server_light_trial* t) {
	struct server_light_journal* j = w->light_journal;
	const ptime_t t0 = time_get();
	if(++j->sequence == 0) j->sequence = 1;
	uint32_t qfnv = SWJ_FNV0;
	if(!swj_queue_begin(w, j, &t->generation, &qfnv)) return false;
	const size_t qbytes = t->generation.count * sizeof(struct server_beta_light_region);
	j->records = (long)(SWJ_HEADER + (qbytes + 31u) / 32u * 32u);
	size_t n = 0;
	if(w->generation_light_n) {
		dict_server_chunks_it_t it;
		for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
			dict_server_chunks_next(it)) {
			const int64_t id = dict_server_chunks_cref(it)->key;
			if(!swj_put_record(j, n, id, &dict_server_chunks_cref(it)->value)) return false;
			n++;
		}
	} else {
		/* the deduplicated resident 3x3 around each pin, in the order
		 * server_world_light_trial_begin collects it */
		const size_t k_n = w->light_pin_n * 9;
		for(size_t k = 0; k < k_n; k++) {
			const int64_t id = S_CHUNK_ID(S_CHUNK_X(w->light_pins[k / 9]) + (int)(k % 9 / 3) - 1,
				S_CHUNK_Z(w->light_pins[k / 9]) + (int)(k % 3) - 1);
			bool first = true;
			for(size_t i = 0; i < k && first; i++)
				first = S_CHUNK_ID(S_CHUNK_X(w->light_pins[i / 9]) + (int)(i % 9 / 3) - 1,
					S_CHUNK_Z(w->light_pins[i / 9]) + (int)(i % 3) - 1) != id;
			if(!first) continue;
			struct server_chunk* sc = server_world_resolve_chunk(w, S_CHUNK_X(id), S_CHUNK_Z(id));
			if(!sc) continue;
			if(!swj_put_record(j, n, id, sc)) return false;
			n++;
		}
	}
	uint8_t* h = j->buf;
	memset(h, 0, SWJ_HEADER);
	memcpy(h, "CWLJ", 4);
	swj_put32(h + 4, SWJ_VERSION);
	swj_put32(h + 8, (uint32_t)w->world_index);
	swj_put32(h + 12, (uint32_t)(int32_t)w->dimension);
	swj_put32(h + 16, (uint32_t)w->generation);
	swj_put32(h + 20, j->sequence);
	swj_put32(h + 24, (uint32_t)n);
	swj_put64(h + 28, (uint64_t)t->generation.n);
	swj_put64(h + 36, (uint64_t)t->generation.base);
	swj_put64(h + 44, (uint64_t)t->generation.count);
	swj_put32(h + 52, j->has_loc ? 1u : 0u);
	swj_put32(h + 56, qfnv);
	swj_put32(h + 60, swj_fnv(SWJ_FNV0, h, 60));
	if(n > UINT32_MAX || !swj_write(j, 0, h, SWJ_HEADER)) return false;
	if(SWJ_FAULT(FLUSH) || fflush(j->file) != 0) return false;
	const ptime_t t1 = time_get();
	j->prepare_us = (uint32_t)time_diff_us(t0, t1);
	j->verifying = true;
	const bool verified = swj_verify(j, n, t->generation.count);
	j->verifying = false;
	j->verify_us = (uint32_t)time_diff_us(t1, time_get());
	if(!verified) {
		SWM_REPORT(journal_verify_fail)++;
		return false;
	}
	t->n = n;
	t->journal = true;
	SWM_REPORT(journal_trials)++;
	SWM_REPORT(journal_columns) = n;
	SWM_REPORT(journal_sequence) = j->sequence;
	return true;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static void swj_tamper(struct server_world* w, struct server_light_journal* j, size_t n) {
	const int kind = s_swj_tamper;
	s_swj_tamper = 0;
	uint8_t h[SWJ_RECORD_HEADER], h1[SWJ_RECORD_HEADER];
	if(kind == 1 && n >= 2 && swj_read(j, j->records, h, sizeof h)
	   && swj_read(j, j->records + (long)SWJ_RECORD, h1, sizeof h1)) {
		memcpy(h, h1, 8); /* record 0 now names record 1's column */
		(void)swj_write(j, j->records, h, sizeof h);
	} else if(kind == 2) {
		uint8_t b = 0;
		if(swj_read(j, (long)SWJ_HEADER + 3, &b, 1)) {
			b ^= 0x40u;
			(void)swj_write(j, (long)SWJ_HEADER + 3, &b, 1);
		}
	} else if(kind == 3 && n >= 1 && swj_read(j, j->records, h, sizeof h)) {
		swj_record_header(j, h, 0, S_CHUNK_ID(100000, 100000), swj_get32(h + 16),
			swj_get32(h + 20));
		(void)swj_write(j, j->records, h, sizeof h);
	}
	(void)w;
	if(kind == 4) s_swj_tamper = 4; /* applied between verify and the queue copy */
	(void)fflush(j->file);
}
/* kind 4: flip a queue byte AFTER the section was verified, BEFORE the copy */
static void swj_tamper_queue(struct server_light_journal* j) {
	if(s_swj_tamper != 4) return;
	s_swj_tamper = 0;
	uint8_t b = 0;
	if(swj_read(j, (long)SWJ_HEADER + 5, &b, 1)) {
		b ^= 0x20u;
		(void)swj_write(j, (long)SWJ_HEADER + 5, &b, 1);
	}
	(void)fflush(j->file);
}
#endif

/* RESTORE: the queue section re-verified, then every record back in order,
 * its header and column checked before any of it is applied (the caller
 * then restores the counts and the queue cursor). */
static bool server_world_light_journal_restore(struct server_world* w,
	const struct server_light_trial* t) {
	struct server_light_journal* j = w->light_journal;
	uint8_t* h = j->buf;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_swj_tamper) swj_tamper(w, j, t->n);
#endif
	if(!swj_read(j, 0, h, SWJ_HEADER) || !swj_header_ok(j, h, t->n)) return false;
	j->queue_fnv = swj_get32(h + 56);
	if(!swj_queue_verify(j, t->generation.count, j->queue_fnv)) return false;
	for(size_t k = 0; k < t->n; k++) {
		const long off = j->records + (long)(k * SWJ_RECORD);
		if(!swj_read(j, off, h, SWJ_RECORD_HEADER) || !swj_record_ok(j, h, k))
			return false;
		const int64_t id = (int64_t)swj_get64(h);
		const uint32_t want_s = swj_get32(h + 16), want_t = swj_get32(h + 20);
		struct server_chunk* sc = server_world_resolve_chunk(w, S_CHUNK_X(id), S_CHUNK_Z(id));
		if(!sc) return false; /* a missing column: nothing of it applied */
		const uint32_t seed = swj_seed(id);
		uint32_t hs = seed, ht = seed;
		for(size_t o = 0; o < LIGHT_TRIAL_PLANE;) {
			const size_t m = LIGHT_TRIAL_PLANE - o < SWJ_SEG ? LIGHT_TRIAL_PLANE - o : SWJ_SEG;
			if(!swj_read(j, off + (long)(SWJ_RECORD_HEADER + o), h, m)
			   || !swj_read(j, off + (long)(SWJ_RECORD_HEADER + LIGHT_TRIAL_PLANE + o),
				   h + SWJ_SEG, m))
				return false;
			if(SWJ_FAULT(CORRUPT)) h[0] ^= 1u; /* a flipped bit on the card */
			hs = swj_fnv(hs, h, m);
			ht = swj_fnv(ht, h + SWJ_SEG, m);
			/* no stripe is held across the next read */
			struct thread_mutex* held = NULL;
			struct thread_mutex** const hold = thread_g9_lease_enabled() ? NULL : &held;
			server_world_light_trial_restore_seg(sc, h, h + SWJ_SEG, o, m, hold);
			if(held) thread_stripe_unlock(held);
			o += m;
		}
		server_world_chunk_touch(sc);
		sc->modified = true; /* live (pending) != the settled image on disk */
		if(hs != want_s || ht != want_t) return false;
	}
	return true;
}


static bool server_world_light_trial_begin(struct server_world* w,
										   struct server_light_trial* t) {
	memset(t, 0, sizeof *t);
	if(server_world_light_halted(w))
		return false;
	if(w->light_seed_n == 0 && w->generation_light_n == 0)
		return true;
	/* Even a byte-equal trial/rollback cannot leave an older identity valid. */
	column_light_mutated(NULL, false);
	t->t0 = time_get();
	g9_probe_begin((uint32_t)w->world_index, 2);
	/* The original-JAR generation regions can propagate through every
	 * resident column (CFR MetadataChunkBlock.java:58-120). A coherent save
	 * trials that exact drain, then restores BOTH live bytes and queue order;
	 * the old gameplay pin union alone does not bound generation work. */
	size_t cap = w->generation_light_n ? dict_server_chunks_size(w->chunks)
		: w->light_pin_n * 9;
	/* Pending regions may outlive every resident column (Beta unload before
	 * drain). Avoid making an empty, valid snapshot depend on malloc(0). */
	if(cap==0) cap=1;
	bool inject = false;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_light_trial_fail_test) {
		s_light_trial_fail_test--;
		inject = true;
	}
#endif
	/* G9X-5b: replay every allocation below, in order, before the first. */
	struct server_world_mem_state ms;
	struct server_world_mem_event ev[SWM_TRIAL_EVENTS_MAX];
	size_t ev_n = 0, lazy = 0;
	bool want_captures = true;
	SWM_REPORT(samples) = SWM_REPORT(sample_us) = 0;
	SWM_REPORT(mode) = SERVER_WORLD_CKPT_NONE;
	bool gated = s_mem_preflight_enabled && swm_sample_timed(w, &ms);
	bool journal = false;
	/* the planner refused: its model is conservative (the Wii sampler cannot
	 * see newlib's reusable bins/top or the arena switch), so a refusal is
	 * NOT proof HEAD cannot complete in RAM. HEAD's own allocations run
	 * first, in HEAD's order; only a real allocation failure, or a fresh
	 * sample right after them showing a floor breached, takes the journal. */
	bool attempt = false;
	if(gated) {
		/* pending gameplay light is planned with its frontier allowance
		 * (server_world_light_trial_events); it does not force the journal */
		const size_t cols = server_world_light_trial_columns(w);
		ev_n = server_world_light_trial_events(w, cap, cols, true, ev, &lazy);
		bool ok = ev_n && server_world_mem_plan_run(&ms, ev, ev_n, &SWM_REPORT(plan));
		if(!ok && ev_n) { /* the captures are optional: plan without them */
			struct server_world_mem_plan p2;
			ev_n = server_world_light_trial_events(w, cap, cols, false, ev, &lazy);
			if(ev_n && server_world_mem_plan_run(&ms, ev, ev_n, &p2)) {
				ok = true;
				want_captures = false;
				SWM_REPORT(plan) = p2;
				SWM_REPORT(captures_skipped)++;
			}
		}
		if(!ev_n) {
			memset(&SWM_REPORT(plan), 0, sizeof SWM_REPORT(plan));
			SWM_REPORT(plan).reason = SERVER_WORLD_MEM_OVERFLOW;
		}
		if(!ok) {
			SWM_REPORT(refusals)++;
			attempt = true;
			want_captures = true;
		}
	}
	if(journal) {
journal_route:
		if(!server_world_light_journal_prepare(w, t)) {
			/* nothing of the world touched: run HEAD's path instead, with
			 * the journal's block and FILE given back first (re-reserved
			 * lazily by a later refused checkpoint) */
			sbl_trial_release(w, &t->generation);
			server_world_light_journal_free(w);
			SWM_REPORT(journal_prepare_fail)++;
			SWM_REPORT(head_fallbacks)++;
			const ptime_t t0 = t->t0;
			memset(t, 0, sizeof *t);
			t->t0 = t0;
			gated = false;
			attempt = false;
			want_captures = true;
			goto head_path;
		}
		SWM_REPORT(mode) = SERVER_WORLD_CKPT_JOURNAL;
		{
			const unsigned k = server_world_light_slot(w);
			const uint32_t tb = server_world_light_u32(sizeof *w->light_journal);
			if(tb > s_light_hb.ckpt_b[k])
				s_light_hb.ckpt_b[k] = tb;
		}
		goto snapshot_done;
	}
head_path:
	t->ids = inject ? NULL : malloc(cap * sizeof *t->ids);
	if(!t->ids && attempt) goto ram_failed;
	if(!t->ids) {
		g9_capture_stage(false, w->world_index, 2);
		g9_probe_end(0, (uint32_t)time_diff_us(t->t0, time_get()));
		s_light_stats.trial_alloc_fail++;
		memset(t, 0, sizeof *t);
		return false; /* nothing touched: the checkpoint fails, retryable */
	}
	if(w->generation_light_n) {
		if(!sbl_trial_begin(w,&t->generation)) {
			if(attempt) { /* a failed sbl_trial_begin accounted nothing */
				memset(&t->generation, 0, sizeof t->generation);
				goto ram_failed;
			}
			free(t->ids); memset(t,0,sizeof *t);
			s_light_stats.trial_alloc_fail++;
			g9_capture_stage(false,w->world_index,2); g9_probe_end(0,0);
			return false;
		}
		dict_server_chunks_it_t it; dict_server_chunks_it(it,w->chunks);
		while(!dict_server_chunks_end_p(it)) {
			t->ids[t->n++]=dict_server_chunks_cref(it)->key;
			dict_server_chunks_next(it);
		}
	} else for(size_t i = 0; i < w->light_pin_n; i++)
		for(int dx = -1; dx <= 1; dx++)
			for(int dz = -1; dz <= 1; dz++) {
				w_coord_t cx = S_CHUNK_X(w->light_pins[i]) + dx;
				w_coord_t cz = S_CHUNK_Z(w->light_pins[i]) + dz;
				int64_t id = S_CHUNK_ID(cx, cz);
				bool dup = false;
				for(size_t k = 0; k < t->n && !dup; k++)
					dup = t->ids[k] == id;
				if(!dup && server_world_resolve_chunk(w, cx, cz))
					t->ids[t->n++] = id;
			}
	t->light = malloc(t->n * LIGHT_TRIAL_CELLS);
	if(!t->light && t->n && attempt) goto ram_failed;
	if(!t->light && t->n) {
		g9_capture_stage(false, w->world_index, 2);
		g9_probe_end(0, (uint32_t)time_diff_us(t->t0, time_get()));
		free(t->ids);
		sbl_trial_release(w, &t->generation);
		memset(t, 0, sizeof *t);
		s_light_stats.trial_alloc_fail++;
		return false;
	}
	/* Optional metadata: OOM falls back to the existing eager plane copy.
	 * It may never turn an otherwise successful checkpoint into a failure. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(!s_light_trial_reference_test && !s_light_trial_eager_test)
#endif
		if(want_captures)
			t->captures = calloc(t->n, sizeof *t->captures);
	if(attempt) {
		/* HEAD's allocations succeeded: keep them unless a fresh sample
		 * shows them breaching a floor they did not already breach */
		struct server_world_mem_state now, after;
		struct server_world_mem_plan p3;
		if(!swm_sample_timed(w, &now) || swm_revalidate(&ms, &now, NULL, 0, &p3)) {
			SWM_REPORT(ram_after_refusal)++;
			goto ram_kept;
		}
		SWM_REPORT(ram_floor_breach)++;
ram_failed:
		free(t->captures);
		free(t->ids);
		free(t->light);
		sbl_trial_release(w, &t->generation);
		{
			const ptime_t t0 = t->t0;
			memset(t, 0, sizeof *t);
			t->t0 = t0;
		}
		attempt = false;
		gated = false;
		want_captures = true;
		if(swm_sample_timed(w, &after) && server_world_light_journal_reserve(w, &after))
			goto journal_route;
		/* no journal: exactly HEAD (its allocations again, ungated) */
		SWM_REPORT(head_fallbacks)++;
		goto head_path;
	}
	if(gated) {
		struct server_world_mem_state now;
		struct server_world_mem_plan p3;
		/* the same rule as a refused plan: HEAD's allocations all
		 * succeeded, so only a fresh sample showing a floor NEWLY
		 * breached releases them. The lazy forecast (what the drain may
		 * still allocate) is recorded, never treated as proof that HEAD
		 * cannot complete in RAM. */
		struct server_world_mem_plan fc;
		bool sampled = swm_sample_timed(w, &now);
		if(sampled && !server_world_mem_plan_run(&now, ev + lazy, ev_n - lazy, &fc))
			SWM_REPORT(revalidation_forecast_kept)++;
		if(!sampled || !swm_revalidate(&ms, &now, NULL, 0, &p3)) {
			p3.ok = false;
			p3.reason = SERVER_WORLD_MEM_STALE;
			SWM_REPORT(plan) = p3;
			SWM_REPORT(refusals)++;
			SWM_REPORT(revalidation_refusals)++;
			free(t->captures);
			free(t->ids);
			free(t->light);
			sbl_trial_release(w, &t->generation);
			/* (the probe is ended by whichever path completes the trial) */
			const ptime_t t0 = t->t0;
			memset(t, 0, sizeof *t);
			t->t0 = t0;
			if(server_world_light_journal_reserve(w, &now)) {
				/* unwound before any simulation write: take the journal,
				 * which allocates nothing more */
				goto journal_route;
			}
			/* no journal: HEAD's path, exactly as HEAD (ungated) */
			SWM_REPORT(head_fallbacks)++;
			gated = false;
			want_captures = true;
			goto head_path;
		}
	}
ram_kept:
	SWM_REPORT(mode) = SERVER_WORLD_CKPT_FULL;
	{
		const unsigned k = server_world_light_slot(w);
		const uint32_t tb = server_world_light_u32(
			t->n * LIGHT_TRIAL_CELLS + cap * sizeof *t->ids
			+ (t->captures ? t->n * sizeof *t->captures : 0)
			+ t->generation.bytes);
		if(tb > s_light_hb.ckpt_b[k])
			s_light_hb.ckpt_b[k] = tb;
	}
	for(size_t k = 0; k < t->n; k++) {
		struct server_chunk* sc = server_world_resolve_chunk(
			w, S_CHUNK_X(t->ids[k]), S_CHUNK_Z(t->ids[k]));
		uint8_t* out = t->light + k * LIGHT_TRIAL_CELLS;
		assert(!sc->light_trial_capture);
		if(t->captures) {
			t->captures[k].light = out;
			sc->light_trial_capture = t->captures + k;
			continue;
		}
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(s_light_trial_reference_test) {
			for(size_t idx = 0; idx < LIGHT_TRIAL_CELLS; idx++)
				out[idx] = (uint8_t)((nibble_read(sc->lighting_torch, idx) << 4)
									 | nibble_read(sc->lighting_sky, idx));
			continue;
		}
#endif
		/* g9-ckpt-1009 (T1): the two nibble planes verbatim, Sky then Block
		 * (the same 32 KiB per chunk the per-cell (torch<<4)|sky pack used;
		 * the snapshot is private, only the restore below reads it). */
		memcpy(out, sc->lighting_sky, LIGHT_TRIAL_PLANE);
		memcpy(out + LIGHT_TRIAL_PLANE, sc->lighting_torch, LIGHT_TRIAL_PLANE);
	}
snapshot_done:
	server_world_light_ckpt_max(
		&s_light_drain_hb.ckpt.snap_us,
		(uint32_t)(time_diff_s(t->t0, time_get()) * 1.0e6f));
	g9_capture_stage(false, w->world_index, 2);
	g9_probe_end(0, (uint32_t)time_diff_us(t->t0, time_get()));
	t->seed_n = w->light_seed_n;
	t->undo_n = w->light_undo_n;
	t->pin_n = w->light_pin_n;
	t->active = true;
	s_light_trial_running = true;
	s_pub_trial = true; /* G9X-6: speculative stores, restored below */
	const bool drained = server_world_light_drain_ex(w, false);
	s_pub_trial = false;
	s_light_trial_running = false;
	if(!drained) {
		/* the drain stopped (world stopped, its work retained): the trial
		 * is abandoned and the checkpoint fails */
		/* the journal trial installed no captures and holds no ids */
		for(size_t k = 0; !t->journal && k < t->n; k++) {
			struct server_chunk* sc = server_world_resolve_chunk(
				w, S_CHUNK_X(t->ids[k]), S_CHUNK_Z(t->ids[k]));
			if(sc) sc->light_trial_capture = NULL;
		}
		free(t->captures);
		free(t->ids);
		free(t->light);
		/* the stopped world keeps its progressed list (no restore) */
		sbl_trial_release(w, &t->generation);
		memset(t, 0, sizeof *t);
		return false;
	}
	s_light_stats.checkpoint_trials++;
	return true;
}

/* false only when a journal restore failed: the world is then stopped
 * (sticky), the checkpoint must not report success. */
static bool server_world_light_trial_end(struct server_world* w,
										 struct server_light_trial* t) {
	if(!t->active)
		return true;
	const ptime_t restore_t0 = time_get();
	g9_probe_begin((uint32_t)w->world_index, 4);
	bool restored = true;
	/* G9X-6: the exact pre-trial bytes return; publication debt stays as it
	 * was before the trial (neither created nor cleared). */
	s_pub_trial = true;
	if(t->journal)
		restored = server_world_light_journal_restore(w, t);
	const uint32_t journal_restore_us = t->journal
		? (uint32_t)time_diff_us(restore_t0, time_get()) : 0u;
	for(size_t k = 0; !t->journal && k < t->n; k++) {
		struct server_chunk* sc = server_world_resolve_chunk(
			w, S_CHUNK_X(t->ids[k]), S_CHUNK_Z(t->ids[k]));
		assert(sc && "pending light pins its chunks through the checkpoint");
		if(!sc)
			continue;
		const uint8_t* in = t->light + k * LIGHT_TRIAL_CELLS;
		sc->light_trial_capture = NULL; /* rollback must not recapture the trial */
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(s_light_trial_reference_test) {
			for(size_t idx = 0; idx < LIGHT_TRIAL_CELLS; idx++) {
				/* only the cells the trial drain changed are written back */
				if(nibble_read(sc->lighting_sky, idx) != (in[idx] & 0xF))
					server_chunk_store(sc, COLUMN_SKY, (unsigned)idx, in[idx] & 0xF);
				if(nibble_read(sc->lighting_torch, idx) != (in[idx] >> 4))
					server_chunk_store(sc, COLUMN_TORCH, (unsigned)idx, in[idx] >> 4);
			}
		} else
#endif
			/* only the cells the trial drain changed are written back */
			server_world_light_trial_restore_chunk(sc, in,
				t->captures ? t->captures[k].pages : NULL);
		server_world_chunk_touch(sc);
		sc->modified = true; /* live (pending) != the settled image on disk */
	}
	s_pub_trial = false; /* G9X-6: the pre-trial bytes are back */
	/* the drain only reset the counts; the entries are still in place */
	w->light_seed_n = t->seed_n;
	w->light_undo_n = t->undo_n;
	w->light_pin_n = t->pin_n;
	if(!t->journal)
		sbl_trial_restore(w, &t->generation);
	else if(restored)
		restored = swj_queue_restore(w, w->light_journal, &t->generation);
	if(!restored) {
		/* live light / queue partially restored: stop the world, keep the
		 * journal (owned until teardown), report the checkpoint failed */
		SWM_REPORT(journal_restore_fail)++;
		w->light_journal_failed = true;
		server_world_light_stop(w, SERVER_LIGHT_FAULT_CHECKPOINT_JOURNAL, 0);
	}
	{
		const ptime_t now = time_get();
		const uint32_t us = (uint32_t)(time_diff_s(t->t0, now) * 1.0e6f);
		if(us > s_light_hb.ckpt_us)
			s_light_hb.ckpt_us = us;
		server_world_light_tick_add(us);
		/* R5: unsaturated, per window */
		server_world_light_ckpt_record(us,
			(uint32_t)(time_diff_s(restore_t0, now) * 1.0e6f));
		g9_worst_tick_ckpt(us, w->save_batch_open); /* nested: R20 Q3 */
		if(t->journal) {
			/* G9X-5b: the journal's synchronous SD phases, per window
			 * (the hardware 50 ms gate is judged on these: perf_stress_5m,
			 * split_play_8m) and one observer line per journal checkpoint */
			const struct server_light_journal* j = w->light_journal;
			server_world_light_ckpt_max(&s_light_drain_hb.ckpt.journal_prepare_us, j->prepare_us);
			server_world_light_ckpt_max(&s_light_drain_hb.ckpt.journal_verify_us, j->verify_us);
			server_world_light_ckpt_max(&s_light_drain_hb.ckpt.journal_restore_us, journal_restore_us);
			thread_mutex_lock(&s_light_drain_hb_lock);
			s_light_drain_hb.ckpt.journal_n++;
			thread_mutex_unlock(&s_light_drain_hb_lock);
			char line[240];
			snprintf(line, sizeof line,
				"{\"event\":\"light_journal\",\"world\":%u,\"columns\":%u,\"prepare_us\":%u,"
				"\"verify_us\":%u,\"restore_us\":%u,\"total_us\":%u,\"restored\":%d}",
				(unsigned)w->world_index, (unsigned)t->n, j->prepare_us, j->verify_us,
				journal_restore_us, us, restored ? 1 : 0);
			(void)hw_telemetry_emit_observer(line);
		}
	}
	free(t->captures);
	free(t->ids);
	free(t->light);
	sbl_trial_release(w, &t->generation);
	g9_capture_stage(false, w->world_index, 4);
	g9_probe_end(0, (uint32_t)time_diff_us(restore_t0, time_get()));
	memset(t, 0, sizeof *t);
	return restored;
}

/* One durable non-evicting save that must not wait for the drain (the death
 * journal base chunks): a pinned column is written from a settled trial and
 * the live world rolled back, exactly like a checkpoint. */
bool server_world_save_chunk_coherent(struct server_world* w, w_coord_t x,
									  w_coord_t z) {
	assert(w);
	if(!server_world_light_pinned(w, x, z) && w->generation_light_n == 0)
		return server_world_save_chunk(w, false, x, z);
	struct server_light_trial trial;
	if(!server_world_light_trial_begin(w, &trial))
		return false;
	const ptime_t save_t0 = time_get();
	g9_probe_begin((uint32_t)w->world_index, 3);
	assert(!w->checkpoint_encode_active);
	w->checkpoint_encode_active = true;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_light_trial_reference_test) w->checkpoint_encode_active = false;
#endif
	bool ok = server_world_save_chunk(w, false, x, z);
	w->checkpoint_encode_active = false;
	g9_capture_stage(false, w->world_index, 3);
	g9_probe_end(0, (uint32_t)time_diff_us(save_t0, time_get()));
	server_world_light_ckpt_max(
		&s_light_drain_hb.ckpt.save_us,
		(uint32_t)(time_diff_s(save_t0, time_get()) * 1.0e6f));
	if(!server_world_light_trial_end(w, &trial)) ok = false;
	return ok;
}

/* R24 existing region_journal observer + transition rows on the same queue.
 * [submitted, durable, backlog, peak backlog, oldest age us, pending bytes,
 *  last completion us, max completion us, rejected, failed, retries, drain us]. */
static uint32_t s_checkpoint_stats[SERVER_CHECKPOINT_STATS];
void server_world_checkpoint_stats(uint32_t out[SERVER_CHECKPOINT_STATS]) {
	for(unsigned k = 0; k < SERVER_CHECKPOINT_STATS; k++)
		out[k] = __atomic_load_n(&s_checkpoint_stats[k], __ATOMIC_RELAXED);
}
static void checkpoint_stat(unsigned k, uint32_t v) {
	__atomic_store_n(&s_checkpoint_stats[k], v, __ATOMIC_RELAXED);
}
static void checkpoint_add(unsigned k) {
	__atomic_fetch_add(&s_checkpoint_stats[k], 1u, __ATOMIC_RELAXED);
}
static uint32_t checkpoint_us(ptime_t start) {
	uint64_t us = time_diff_us(start, time_get());
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}
static void checkpoint_note(struct server_world* w,
	struct server_checkpoint_token* t) {
	char line[360];
	snprintf(line, sizeof line,
		"{\"event\":\"region_journal\",\"checkpoint\":%u,\"generation\":%u,"
		"\"logical_tick\":%u,\"state\":%u,\"pending_age_us\":%llu,"
		"\"backlog\":%u,\"sequence_lag\":%u,\"pending_bytes\":%u,\"completion_us\":%llu}",
		t->sequence, t->generation, t->logical_tick, (unsigned)t->state,
		(unsigned long long)time_diff_us(t->submitted, time_get()), w->checkpoint_count,
		w->checkpoint_sequence - w->checkpoint_durable, region_archive_checkpoint_pending_bytes(),
		t->state == REGION_CHECKPOINT_DURABLE ? (unsigned long long)time_diff_us(t->submitted, time_get()) : 0ull);
	(void)hw_telemetry_emit_observer(line);
	t->noted = time_get();
}
static enum region_checkpoint_state checkpoint_reject(struct server_world* w,
	struct server_checkpoint_token* t) {
	checkpoint_add(8);
	if(!t->rejection_reported) {
		t->rejection_reported = true;
		checkpoint_note(w, t);
	}
	return REGION_CHECKPOINT_REJECTED;
}
void server_world_checkpoint_poll(struct server_world* w) {
	while(w->checkpoint_count) {
		struct server_checkpoint_token* t = w->checkpoints + w->checkpoint_head;
		assert(t->generation == w->generation);
		if(t->state == REGION_CHECKPOINT_REJECTED) break; /* caller retries capture */
		bool durable = true, failed = false;
		bool retry = t->state == REGION_CHECKPOINT_FAILED;
		for(unsigned k = 0; k < t->handles; k++) {
			enum region_checkpoint_state st = region_archive_checkpoint_poll(t->fence[k], retry);
			failed |= st == REGION_CHECKPOINT_FAILED;
			durable &= st == REGION_CHECKPOINT_DURABLE;
		}
		if(failed) {
			if(t->state != REGION_CHECKPOINT_FAILED) {
				SERVICE_OBSERVER(service_observer_checkpoint(t->generation, t->sequence, t->logical_tick, REGION_CHECKPOINT_FAILED, t->state));
				t->state = REGION_CHECKPOINT_FAILED;
				w->checkpoint_failed = true;
				checkpoint_add(9);
				checkpoint_note(w, t);
				fprintf(stderr, "[light_queue_stress] checkpoint seq=%u generation=%u FAILED; retained for retry\n",
					t->sequence, t->generation);
			}
			if(retry) checkpoint_add(10);
			break;
		}
		if(!durable) {
			SERVICE_OBSERVER(service_observer_checkpoint(t->generation, t->sequence, t->logical_tick, REGION_CHECKPOINT_PENDING, t->state));
			t->state = REGION_CHECKPOINT_PENDING;
			break;
		}
		/* No worker reads a world or clears a dirty revision. Reconciliation
		 * only resolves each chunk's recorded job; newer edits stay dirty. */
		server_world_reconcile_pending_saves_ex(w, false);
		SERVICE_OBSERVER(service_observer_checkpoint(t->generation, t->sequence, t->logical_tick, REGION_CHECKPOINT_DURABLE, t->state));
		t->state = REGION_CHECKPOINT_DURABLE;
		t->latency_us = checkpoint_us(t->submitted);
		w->checkpoint_durable = t->sequence;
		checkpoint_add(1);
		checkpoint_stat(6, t->latency_us);
		if(t->latency_us > __atomic_load_n(&s_checkpoint_stats[7], __ATOMIC_RELAXED))
			checkpoint_stat(7, t->latency_us);
		checkpoint_note(w, t);
		for(unsigned k = 0; k < t->handles; k++) {
			region_archive_checkpoint_release(t->fence[k]);
			region_pool_unpin(server_world_region_pool(w), t->archive[k]);
			t->fence[k] = NULL;
		}
		t->handles = 0;
		w->checkpoint_head = (w->checkpoint_head + 1) % SERVER_CHECKPOINT_SLOTS;
		w->checkpoint_count--;
	}
	/* Existing observer queue: refresh each pending token's age/backlog,
	 * including N+1 parked behind N, at the existing two-second cadence. */
	for(unsigned k = 0; k < w->checkpoint_count; k++) {
		struct server_checkpoint_token* t = &w->checkpoints[(w->checkpoint_head + k) % SERVER_CHECKPOINT_SLOTS];
		if(time_diff_ms(t->noted, time_get()) >= 2000) checkpoint_note(w, t);
	}
	checkpoint_stat(2, w->checkpoint_count);
	checkpoint_stat(4, w->checkpoint_count
		? checkpoint_us(w->checkpoints[w->checkpoint_head].submitted) : 0);
	checkpoint_stat(5, region_archive_checkpoint_pending_bytes());
}
enum region_checkpoint_state server_world_checkpoint_submit(
	struct server_world* w, w_coord_t x, w_coord_t z, uint32_t tick,
	struct server_checkpoint_token** out) {
	server_world_checkpoint_poll(w);
	struct server_checkpoint_token* t = *out;
	if(!t) {
		if(w->checkpoint_count == SERVER_CHECKPOINT_SLOTS) {
			checkpoint_add(8);
			return REGION_CHECKPOINT_REJECTED;
		}
		t = &w->checkpoints[(w->checkpoint_head + w->checkpoint_count++) % SERVER_CHECKPOINT_SLOTS];
		memset(t, 0, sizeof *t);
		if(++w->checkpoint_sequence == 0) abort();
		t->sequence = w->checkpoint_sequence;
		t->generation = w->generation;
		t->logical_tick = tick;
		t->submitted = t->noted = time_get();
		SERVICE_OBSERVER(service_observer_checkpoint(t->generation, t->sequence, t->logical_tick, REGION_CHECKPOINT_REJECTED, UINT32_MAX));
		t->state = REGION_CHECKPOINT_REJECTED;
		*out = t;
		checkpoint_add(0);
		if(w->checkpoint_count > __atomic_load_n(&s_checkpoint_stats[3], __ATOMIC_RELAXED))
			checkpoint_stat(3, w->checkpoint_count);
	}
	assert(t->state == REGION_CHECKPOINT_REJECTED && t->generation == w->generation);
	if(w->checkpoint_count > SERVER_CHECKPOINT_MAX) return checkpoint_reject(w, t);
	/* Opening the first archive also applies the test/environment IO mode.
	 * Resolve it before choosing the inline path: that path's barrier must
	 * never see its own still-unadmitted REJECTED token as pending work. */
	struct region_archive* target = server_world_chunk_region_create(w, x, z);
	if(!target) return checkpoint_reject(w, t);
	if(!region_archive_deferred_io_enabled()) {
		bool saved = server_world_save_chunk_coherent(w, x, z);
		bool synced = server_world_sync_regions(w);
		if(!saved || !synced) return checkpoint_reject(w, t);
		SERVICE_OBSERVER(service_observer_checkpoint(t->generation, t->sequence, t->logical_tick, REGION_CHECKPOINT_DURABLE, t->state));
		t->state = REGION_CHECKPOINT_DURABLE;
		server_world_checkpoint_poll(w);
		return t->state;
	}
	bool capacity = true;
	for(size_t k = 0; capacity && k < server_world_region_count(w); k++) {
		struct region_archive* ra = server_world_region_at(w, k);
		if(ra->publish_blocked) {
			/* A pre-R24 batch error must be surfaced/reconciled before
			 * admitting a new snapshot. This explicit pre-capture barrier
			 * counts in the admission tick and never certifies this token. */
			(void)region_archive_sync(ra);
			server_world_reconcile_pending_saves(w);
			return checkpoint_reject(w, t);
		}
		capacity = region_archive_checkpoint_capacity(ra);
	}
	if(!capacity || !region_archive_checkpoint_reserve((unsigned)server_world_region_count(w))) {
		return checkpoint_reject(w, t);
	}
	for(size_t k = 0; k < server_world_region_count(w); k++) {
		struct region_archive* ra = server_world_region_at(w, k);
		if(!region_archive_checkpoint_ordered(ra)) {
			region_archive_checkpoint_unreserve();
			(void)region_archive_sync(ra);
			server_world_reconcile_pending_saves(w);
			return checkpoint_reject(w, t); /* no trial/snapshot admitted */
		}
	}
	w->checkpoint_stream_active = true;
	bool saved = server_world_save_chunk_coherent(w, x, z);
	if(saved) {
		for(size_t k = 0; k < server_world_region_count(w); k++) {
			struct region_archive* ra = server_world_region_at(w, k);
			struct region_io_fence* f = region_archive_checkpoint_fence(ra);
			assert(f && "reserved immutable publication/sync must be admitted");
			if(!f) abort(); /* never turn partial submission into success */
			region_pool_pin(server_world_region_pool(w), ra);
			t->archive[t->handles] = ra;
			t->fence[t->handles++] = f;
		}
		SERVICE_OBSERVER(service_observer_checkpoint(t->generation, t->sequence, t->logical_tick, REGION_CHECKPOINT_PENDING, t->state));
		t->state = REGION_CHECKPOINT_PENDING;
		checkpoint_note(w, t);
	} else (void)checkpoint_reject(w, t);
	region_archive_checkpoint_unreserve();
	return t->state;
}
bool server_world_checkpoint_drain(struct server_world* w) {
	if(!w->checkpoint_sequence) return true;
	ptime_t start = time_get();
	uint32_t errors = __atomic_load_n(&s_checkpoint_stats[9], __ATOMIC_RELAXED);
	while(w->checkpoint_count) {
		server_world_checkpoint_poll(w);
		if(w->checkpoint_count) {
			assert(w->checkpoints[w->checkpoint_head].state != REGION_CHECKPOINT_REJECTED);
			thread_msleep(1);
		}
	}
	uint64_t elapsed = time_diff_us(start, time_get());
	checkpoint_stat(11, checkpoint_us(start));
	char line[200];
	snprintf(line, sizeof line,
		"{\"event\":\"region_journal\",\"checkpoint_drain_us\":%llu,"
		"\"generation\":%u,\"requested\":%u,\"durable\":%u,\"backlog\":%u}",
		(unsigned long long)elapsed, w->generation, w->checkpoint_sequence,
		w->checkpoint_durable, w->checkpoint_count);
	(void)hw_telemetry_emit_observer(line);
	return errors == __atomic_load_n(&s_checkpoint_stats[9], __ATOMIC_RELAXED);
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static bool s_test_unsavable_on;
static w_coord_t s_test_unsavable_x, s_test_unsavable_z;
void server_world_test_unsavable_chunk(bool on, w_coord_t x, w_coord_t z) {
	s_test_unsavable_on = on;
	s_test_unsavable_x = x;
	s_test_unsavable_z = z;
}
#endif

bool server_world_checkpoint_chunks(struct server_world* w) {
	assert(w);
	w->ckpt_unsaved_count = 0; /* IMP-QUIT-UNSAVABLE: this pass's record */
	w->ckpt_light_failed = false;
	struct server_light_trial trial;
	if(!server_world_light_trial_begin(w, &trial)) {
		w->ckpt_light_failed = true;
		return false; /* nothing written, nothing settled: retry later */
	}
	bool succeeded = true;
	const ptime_t save_t0 = time_get();
	g9_probe_begin((uint32_t)w->world_index, 3);
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);
	while(!dict_server_chunks_end_p(it)) {
		struct server_chunk* sc = &dict_server_chunks_ref(it)->value;
		int64_t id = dict_server_chunks_ref(it)->key;
		/* One chunk's failure never stops the pass: every other chunk is still
		 * written, and the failed one keeps its last saved record on disk. */
		bool chunk_ok;
		const uint32_t noted_before = region_save_failure_count();
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(s_test_unsavable_on && S_CHUNK_X(id) == s_test_unsavable_x
		   && S_CHUNK_Z(id) == s_test_unsavable_z) {
			region_save_failure_note("entity_preserve");
			sc->modified = true;
			chunk_ok = false;
		} else
#endif
		chunk_ok = server_world_save_chunk_obj_checked(
			w, false, S_CHUNK_X(id), S_CHUNK_Z(id), sc);
		if(!chunk_ok) {
			succeeded = false;
			/* A refusal that noted no region stage (light fault / pinned
			 * column) reports RSF_NONE, never a stale earlier stage. */
			enum region_save_fail_stage stage
				= region_save_failure_count() != noted_before
				? region_save_failure_stage() : RSF_NONE;
			if(w->ckpt_unsaved_count++ == 0) {
				w->ckpt_unsaved_x = S_CHUNK_X(id);
				w->ckpt_unsaved_z = S_CHUNK_Z(id);
				w->ckpt_unsaved_stage = (uint8_t)stage;
			}
			fprintf(stderr,
					"[checkpoint] unsaved chunk=(%d,%d) stage=%s dim=%d\n",
					(int)S_CHUNK_X(id), (int)S_CHUNK_Z(id),
					region_save_failure_stage_name(stage), (int)w->dimension);
		}
		dict_server_chunks_next(it);
	}
	server_world_light_ckpt_max(
		&s_light_drain_hb.ckpt.save_us,
		(uint32_t)(time_diff_s(save_t0, time_get()) * 1.0e6f));
	g9_capture_stage(false, w->world_index, 3);
	g9_probe_end(0, (uint32_t)time_diff_us(save_t0, time_get()));
	if(!server_world_light_trial_end(w, &trial)) {
		w->ckpt_light_failed = true;
		succeeded = false;
	}
	return succeeded;
}

// Sync every open region handle to the card (region_archive_sync): the
// checkpoint's final durability barrier. The LRU keeps region FILE*s open
// across checkpoints for write batching, which on the Wii leaves everything
// since open in libfat's cache -- a power cut then tears the region files
// (2026-07-27 cold-boot proof finding). Every dirty checkpoint path must end
// with this before its result may be called durable.
bool server_world_sync_regions(struct server_world* w) {
	assert(w);
	/* A failed eviction leaves its old handle resident so this loop can retry
	 * the exact durability operation. Conservatively report the original
	 * failure once as well; clear it only after every retained handle actually
	 * participated in a successful barrier. */
	bool carried_failure = w->region_evict_failed;
	if(region_archive_deferred_io_enabled() && !server_world_checkpoint_drain(w))
		carried_failure = true;
	bool residents_ok = true;
	/* M3: region_archive_sync below drains and publishes every resident
	 * handle, so reconcile straight after it settles every pending chunk. */
	for(size_t k = 0; k < server_world_region_count(w); k++)
		if(!region_archive_sync(server_world_region_at(w, k)))
			residents_ok = false;
	server_world_reconcile_pending_saves(w);
	/* S2: the reconcile above re-appended any FAILED retained records with
	 * the batch closed, so they published themselves; one more barrier
	 * settles them. Anything still retained after that is NOT durable and
	 * this barrier must say so. */
	if(w->retiring_active) {
		for(size_t k = 0; k < server_world_region_count(w); k++)
			if(!region_archive_sync(server_world_region_at(w, k)))
				residents_ok = false;
		server_world_reconcile_pending_saves(w);
	}
	if(residents_ok)
		w->region_evict_failed = false;
	/* A close-failed handle has already been consumed by fclose and cannot be
	 * revisited. Never turn that lost evidence into success during this world
	 * instance; recovery requires a clean reopen, not observation of a bit. */
	return residents_ok && !carried_failure && !w->region_close_failed
		&& w->retiring_active == 0;
}

void server_world_save_batch_begin(struct server_world* w) {
	assert(w);
	w->save_batch_open = true;
	/* M3: the previous batch-s publications have had a whole batch window to
	 * land; take their answers before this batch adds more. */
	server_world_reconcile_pending_saves(w);
	/* Arm the archives that are already resident; server_world_save_chunk_obj_
	 * impl arms any archive the batch opens later. */
	for(size_t k = 0; k < server_world_region_count(w); k++)
		region_archive_batch_begin(server_world_region_at(w, k));
}

bool server_world_save_batch_end(struct server_world* w) {
	assert(w);
	if(!w->save_batch_open)
		return true;
	w->save_batch_open = false;
	bool ok = true;
	for(size_t k = 0; k < server_world_region_count(w); k++)
		if(!region_archive_batch_end(server_world_region_at(w, k)))
			ok = false;
	/* Anything published INLINE here (async declined, or the batch overflowed)
	 * is durable already: settle it now rather than a window later. */
	server_world_reconcile_pending_saves(w);
	return ok;
}

bool server_world_disk_has_chunk(struct server_world* w, w_coord_t x,
								 w_coord_t z) {
	struct region_archive* ra = server_world_chunk_region(w, x, z);
	bool chunk_exists;
	return ra ?
		(region_archive_contains(ra, x, z, &chunk_exists) && chunk_exists) :
		false;
}

struct region_pool* server_world_region_pool(struct server_world* w) {
	assert(w);
	if(!w->region_pool) {
		/* Bound once, at first use: the session's shared pool when this world
		 * is a session slot, else the embedded one. A world that already holds
		 * handles never re-binds (its vector indexes ONE pool). */
		w->region_pool = w->session ? &w->session->region_pool
									: &w->own_region_pool;
		if(!w->region_pool->ready)
			region_pool_init(w->region_pool);
	}
	assert(w->region_slot_count == 0
		   || w->region_pool
			   == (w->session ? &w->session->region_pool : &w->own_region_pool));
	return w->region_pool;
}

size_t server_world_region_count(const struct server_world* w) {
	assert(w);
	return w->region_slot_count;
}

struct region_archive* server_world_region_at(struct server_world* w, size_t k) {
	assert(w && k < w->region_slot_count && w->region_pool);
	return &w->region_pool->slot[w->region_slots[k]].archive;
}

/* A2-S: every region open of this world goes through the session pool's
 * admission (region_pool_acquire): hit -> touched handle; miss -> a FREE slot
 * or the oldest unpinned donor across BOTH worlds is settled and closed
 * BEFORE the candidate opens (never a fifth handle). A failed donor sync
 * latches the donor owner's region_evict_failed and refuses this request with
 * the exact donor still resident -- the shape test_region_evict_failure pins.
 * The EVICTION-PATH RULE (batch arming at installation) lives in the pool. */
static struct region_archive* server_world_chunk_region_impl(
	struct server_world* w, w_coord_t x, w_coord_t z, bool create) {
	assert(w);
	struct region_archive* ra = NULL;
	enum storage_rc rc = region_pool_acquire(server_world_region_pool(w), w,
											 CHUNK_REGION_COORD(x),
											 CHUNK_REGION_COORD(z), create, &ra);
	return rc == STORAGE_OK ? ra : NULL;
}

struct region_archive* server_world_chunk_region(struct server_world* w,
												 w_coord_t x, w_coord_t z) {
	return server_world_chunk_region_impl(w, x, z, false);
}

struct region_archive* server_world_chunk_region_create(struct server_world* w,
														w_coord_t x,
														w_coord_t z) {
	return server_world_chunk_region_impl(w, x, z, true);
}

enum server_world_region_result server_world_chunk_region_checked(
	struct server_world* w, w_coord_t x, w_coord_t z,
	struct region_archive** out) {
	assert(w && out);
	*out = NULL;
	for(size_t k = 0; k < server_world_region_count(w); k++) {
		bool belongs = false;
		if(region_archive_contains(server_world_region_at(w, k), x, z,
								   &belongs)) {
			*out = server_world_chunk_region(w, x, z);
			return *out ? SERVER_WORLD_REGION_OK : SERVER_WORLD_REGION_ERROR;
		}
	}

	bool exists = false;
	if(!server_world_region_file_status(w, x, z, &exists))
		return SERVER_WORLD_REGION_ERROR;
	if(!exists)
		return SERVER_WORLD_REGION_ABSENT;
	*out = server_world_region_probed(w, x, z, true);
	return *out ? SERVER_WORLD_REGION_OK : SERVER_WORLD_REGION_ERROR;
}


static void server_world_fire_scheduled_tick(struct server_world* w,
										 struct server_local* s,
										 struct scheduled_tick tick) {
	/* Beta removes the entry from both pending collections before this second
	 * radius-8 check. If any required chunk unloaded while the tick waited, the
	 * entry is consumed without querying the target or calling updateTick.
	 * [Beta 1.7.3 decompile 740c583: World.TickUpdates] */
	if(!server_world_scheduled_tick_area_loaded(
		   w, tick.x, tick.y, tick.z))
		return;

	struct block_data blk;
	if(!server_world_get_block(w, tick.x, tick.y, tick.z, &blk)
	   || blk.type != tick.block_type)
		return;

	const struct block* b = blocks[blk.type];
	if(!b || !b->onWorldTick)
		return;

	struct block_data neighbour_data[SIDE_MAX];
	struct block_data* neighbours = NULL;
	if(server_world_block_needs_neighbours(blk.type)) {
		server_world_fill_neighbours(w, neighbour_data, tick.x, tick.y,
									 tick.z);
		neighbours = neighbour_data;
	}

	struct block_info info = {
		.block = &blk,
		.neighbours = neighbours,
		.x = tick.x,
		.y = tick.y,
		.z = tick.z,
	};
	{ struct server_world* bw_prev = block_world_enter(s, w);
	b->onWorldTick(s, &info);
	block_world_leave(s, bw_prev); }
	/* Scheduled updates are one-shot. A callback that needs a later update must
	 * explicitly call scheduleBlockUpdate itself (flow metadata changes, fire,
	 * pressure plates, etc.). In particular, BlockFlowing's hesitant-lava branch
	 * leaves the cell unchanged with var8=false and does not schedule a fixed
	 * +30 retry; it waits for a later random tick. [740c583
	 * BlockFlowing.updateTick:59-75] */
}

// Fire one ready scheduled block tick. Explosion cap deferrals remain owned by
// their source entities and can never compete with or be dropped by this ring.
static void server_world_fire_one(struct server_world* w, struct server_local* s,
								  struct scheduled_tick tick) {
	server_world_fire_scheduled_tick(w, s, tick);
	/* The standalone queue keeps its field-only fault contract. A callback
	 * whose publication failed must stop it before another tick is popped. */
	if(clin_rpc_publication_faulted()) {
		server_world_stop_simulation(w, SERVER_LIGHT_FAULT_PUBLICATION);
	}
}

static void server_world_process_scheduled_ticks(struct server_world* w,
											 struct server_local* s) {
	// Drain this game-tick's ready scheduled ticks under the flowing-fluid budget
	// (s->scheduled_tick_budget; 0 = unlimited). Conservation, stable old-entry
	// precedence, and the intentional mixed-class ordering policy are pinned by
	// test_scheduled_tick_budget. profile.h / wii_controller_playability.md document
	// why Wii caps below Beta and why prompt non-fluid callbacks may overtake fluid.
	// thread_yield paces the render thread through a long cascade.
	if(server_world_simulation_stopped(w)) return;
	server_world_drain_scheduled(w, s, s->scheduled_tick_budget,
								 server_world_fire_one, thread_yield);
	if(!server_world_simulation_stopped(w))
		server_world_retry_pending_chunk_tick_rearms(w);
}

// Day/night from the authoritative SERVER clock (s->world_time, in ticks). The old
// daytime_get_time() reads the render-thread gstate.world_time -- a cross-thread read AND
// a second clock driving the simulation. Same Beta threshold (time-of-day < 13000 = day).
static bool server_world_day_phase(uint64_t world_time) {
	return (world_time % 24000) < 13000;
}

static void server_world_run_phase_transition(struct server_world* w,
											  struct server_local* s,
											  bool is_day) {
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);

	while(!dict_server_chunks_end_p(it)) {
		struct server_chunk* sc = &dict_server_chunks_ref(it)->value;
		w_coord_t base_x = S_CHUNK_X(dict_server_chunks_ref(it)->key)
			* CHUNK_SIZE;
		w_coord_t base_z = S_CHUNK_Z(dict_server_chunks_ref(it)->key)
			* CHUNK_SIZE;

		for(int cx = 0; cx < CHUNK_SIZE; cx++) {
			for(int cz = 0; cz < CHUNK_SIZE; cz++) {
				for(int y = 0; y < WORLD_HEIGHT && !w->light_fault; y++) {
					struct block_data blk;
					if(!server_chunk_get_block(sc, cx, y, cz, &blk)
					   || !block_has_day_night_callback(blk.type))
						continue;

					const struct block* b = blocks[blk.type];
					struct block_info info = {
						.block = &blk,
						.neighbours = NULL,
						.x = base_x + cx,
						.y = y,
						.z = base_z + cz,
					};

					if(is_day && b->onDay)
						{ struct server_world* bw_prev = block_world_enter(s, w);
						b->onDay(s, &info);
						block_world_leave(s, bw_prev); }
					if(!is_day && b->onNight)
						{ struct server_world* bw_prev = block_world_enter(s, w);
						b->onNight(s, &info);
						block_world_leave(s, bw_prev); }
				}
			}
		}

		// A1: the day<->night transition rescans EVERY block of EVERY loaded chunk
		// (16x16x128 x N) in one shot, unbudgeted -- on the single core that froze
		// render for the whole scan at each dawn/dusk. Yield after each chunk so render
		// takes a frame mid-scan. Pure pacing: the same callbacks fire on the same
		// blocks in the same order; only the wall-clock interleave changes.
		thread_yield();
		if(w->light_fault)
			break; /* G9 R3: a light stop ends the loop at once */

		dict_server_chunks_next(it);
	}
}

void (*server_column_observe_tick)(struct server_world*);

void server_world_tick(struct server_world* w, struct server_local* s) {
	assert(w && s);
	// next-gaps-0924/G5 F1: finish any redstone cascade an allocation failure suspended (a cheap
	// bool when idle) BEFORE this tick advances -- a retained torch-clock notify must not stall.
	redstone_server_service_pending(s);

	w->current_tick++;

	bool is_day = server_world_day_phase(s->world_time);
	if(!w->day_phase_initialized) {
		w->day_phase_initialized = true;
		w->is_day_phase = is_day;
	} else if(w->is_day_phase != is_day) {
		w->is_day_phase = is_day;
		server_world_run_phase_transition(w, s, is_day);
	}

	// Audit #24: Beta BlockPressurePlate.onEntityCollidedWithBlock (740c583),
	// player half -- Entity.moveEntity's per-tick block-collision scan reduced
	// to a center-column feet-cell check, the SAME simplification
	// server_local.c already uses for player fire/cactus/ladder contact
	// (s->player.x/y/z is not in s->entities, so it needs its own probe here).
	// Mobs and dropped items get the identical entry point wired from their
	// own post-move scans instead (entity_monster.c, entity_item.c) since
	// Beta's dispatch is per-ENTITY, not world-tick-global. See
	// block_pressure_plate.h for the full citation, including why arrows are
	// correctly NOT wired anywhere (EntityArrow.onUpdate never calls
	// Entity.moveEntity in Beta 1.7.3).
	{
		/* Beta's collision/query belongs to the player's own World
		 * (RetroMCP b1.7.3 BlockPressurePlate.java:61-83). Both local bodies
		 * need the arrival probe, only when committed to this world. */
		struct server_player* players[SERVER_LOCAL_MAX_PRESENT_ACTORS];
		size_t player_count = server_local_present_actors(
			s, players, SERVER_LOCAL_MAX_PRESENT_ACTORS);
		for(size_t i = 0; i < player_count; i++) {
			struct server_player* player = players[i];
			if(server_local_world_of(s, player) != w)
				continue;
			const w_coord_t pp_x = (w_coord_t)floor(player->x);
			const w_coord_t pp_z = (w_coord_t)floor(player->z);
			const w_coord_t pp_y
				= (w_coord_t)floor(player->y - PLAYER_EYE_HEIGHT);
			struct block_data pp_feet;
			if(server_world_get_block(w, pp_x, pp_y, pp_z, &pp_feet)
			   && (pp_feet.type == BLOCK_STONE_PRESSURE_PLATE
				   || pp_feet.type == BLOCK_WOOD_PRESSURE_PLATE)) {
				struct server_world* bw_prev = block_world_enter(s, w);
				block_pressure_plate_on_entity_tick(s, pp_x, pp_y, pp_z);
				block_world_leave(s, bw_prev);
			}
		}
	}

	server_world_process_scheduled_ticks(w, s);
	/* The column-observer state sample is NOT here: since G10 the random pass
	 * runs after this function, so a hook here would sample mid-tick. It fires
	 * at the end of server_local_world_block_ticks_w (CD1, Astra 2026-10-08). */
}

void server_world_random_tick(struct server_world* w, struct random_gen* g,
							  struct server_local* s, w_coord_t px,
							  w_coord_t pz, w_coord_t dist) {
	assert(w && g && s);
	if(s->pool_selftest_freeze)
		return; /* R6: no world edits or RNG draws in the pool comparison. */

	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);

	while(!dict_server_chunks_end_p(it)) {
		struct server_chunk* sc = &dict_server_chunks_ref(it)->value;
		int64_t id = dict_server_chunks_ref(it)->key;

		bool in_host = abs(S_CHUNK_X(id) - px) <= dist
			&& abs(S_CHUNK_Z(id) - pz) <= dist;
		/* Split-screen guest random-tick coverage (Astra debate 2026-09-11,
		 * D): Beta ticks the loaded chunks around EVERY player; the square
		 * around the admitted guest body joins the host's (a chunk in both
		 * squares ticks once). Single player and the synthetic anchor sweep
		 * are unchanged (no guest body -> host square only). */
		bool in_guest = false;
		/* Beta World.java:1862-1888 builds positionsToUpdate only from
		 * this.playerEntities; another dimension's actor owns no draws. */
		if(!in_host && s->guest_gameplay && s->guest_actor
		   && (!clin_world_payloads_enabled()
			   || s->residency_anchors[1].dimension == w->dimension))
			in_guest = abs(S_CHUNK_X(id) - s->residency_anchors[1].x) <= dist
				&& abs(S_CHUNK_Z(id) - s->residency_anchors[1].z) <= dist;
		if(server_world_light_halted(w))
			break; /* G9 R3: a light stop ends the loop at once */
		if(in_host || in_guest) {
			for(int k = 0; k < FCAVEX_RANDOM_TICKS_PER_CHUNK
				&& !server_world_light_halted(w); k++) {
				c_coord_t cx = rand_gen_range(g, 0, CHUNK_SIZE);
				c_coord_t cz = rand_gen_range(g, 0, CHUNK_SIZE);

				w_coord_t x = S_CHUNK_X(id) * CHUNK_SIZE + cx;
				w_coord_t y = rand_gen_range(g, 0, WORLD_HEIGHT);
				w_coord_t z = S_CHUNK_Z(id) * CHUNK_SIZE + cz;

				struct block_data blk;
				if(server_chunk_get_block(sc, cx, y, cz, &blk)
				   && blocks[blk.type] && blocks[blk.type]->onRandomTick) {
					{ struct server_world* bw_prev = block_world_enter(s, w);
					blocks[blk.type]->onRandomTick(
						s,
						&(struct block_info) {.block = &blk,
											  .neighbours = NULL,
											  .x = x,
											  .y = y,
											  .z = z});
					block_world_leave(s, bw_prev); }
				}
			}
		}

		dict_server_chunks_next(it);
	}
}


struct broken_coord { int x,y,z; };

/* B7/P1 (bug hunt): one explosion's destroyed-cell dedupe set. Beta uses a
 * HashSet (Explosion.destroyedBlockPositions) and honours every cell; the old
 * fixed broken[512] + O(N^2) linear scan had two defects: blasts breaking
 * MORE than 512 unique cells silently abandoned everything past the cap
 * (blocks left floating in the crater -- no AIR write, no drop, no light
 * flood), and the scan itself is O(bc^2) per blast. This open-addressed
 * linear-probe set gives O(1) amortised lookup/insert. Membership is never
 * iterated: the collect loop still appends to broken[] in FIRST-SEEN order,
 * so every blast that fit under the old cap produces a byte-identical
 * broken[] (and therefore byte-identical RNG/drop/notify sequencing) -- the
 * fix only changes what happens past 512, where the old code was wrong.
 * All memory is transient (freed before the blast returns), so the Wii pays
 * for a worst-case blast only while it runs, never in permanent .bss. */
/* F4-RNG (ASTRA-RNG2-RULING 10-14): Explosion.destroyedBlockPositions is a
 * java.util.HashSet<ChunkPosition>, and Explosion copies it into an ArrayList
 * (iteration order) and walks that list in REVERSE for both the flaming pass
 * and doEffects -- so the set's iteration order decides which cell draws
 * which World.rand value. The declared V6 baseline is the OpenJDK 6 HashMap
 * (openjdk/jdk6 jdk/src/share/classes/java/util/HashMap.java): table length
 * 16 doubling, threshold (int)(len * 0.75f), supplemental hash
 * h ^= (h>>>20)^(h>>>12); h ^ (h>>>7) ^ (h>>>4), bucket h & (len-1), new
 * entries at the bucket HEAD, `if (size++ >= threshold) resize(2*len)`,
 * transfer walking old buckets ascending and head-inserting into the new
 * table (which reverses colliding chains), and HashIterator walking buckets
 * ascending, each chain head-first. ChunkPosition.hashCode is
 * x*8976890 + y*981131 + z with Java int wrap (ChunkPosition.java:24) and
 * equals compares x/y/z. No Java 8+ tree bins. Reproduced from the REAL
 * insertion sequence (every add() of the ray march, air included); the
 * independent legacy-collection oracle qualification is pending (Sol, F4a).
 * Entries live in flat arrays (index links), all transient per blast. */
struct expl_j6_set {
	struct broken_coord* keys;
	int32_t* hash;
	int32_t* next;
	int32_t* table;
	uint32_t len;
	uint32_t threshold;
	uint32_t size;
	uint32_t cap;
};

static void expl_j6_init(struct expl_j6_set* m) {
	memset(m, 0, sizeof(*m));
}

static void expl_j6_destroy(struct expl_j6_set* m) {
	free(m->keys);
	free(m->hash);
	free(m->next);
	free(m->table);
	expl_j6_init(m);
}

static int32_t expl_j6_supplemental_hash(int32_t h) {
	uint32_t u = (uint32_t)h;
	u ^= (u >> 20) ^ (u >> 12);
	return (int32_t)(u ^ (u >> 7) ^ (u >> 4));
}

/* HashMap.resize -> transfer: ascending old buckets, head insertion. */
static bool expl_j6_resize(struct expl_j6_set* m, uint32_t new_len) {
	int32_t* nt = malloc((size_t)new_len * sizeof(*nt));
	if(!nt)
		return false;
	for(uint32_t i = 0; i < new_len; i++)
		nt[i] = -1;
	for(uint32_t j = 0; j < m->len; j++) {
		int32_t e = m->table[j];
		while(e >= 0) {
			int32_t nx = m->next[e];
			uint32_t i = (uint32_t)m->hash[e] & (new_len - 1);
			m->next[e] = nt[i];
			nt[i] = e;
			e = nx;
		}
	}
	free(m->table);
	m->table = nt;
	m->len = new_len;
	m->threshold = (uint32_t)((float)new_len * 0.75f);
	return true;
}

/* HashSet.add(new ChunkPosition(x, y, z)). Returns false only on allocation
 * failure (the caller aborts the whole blast before any mutation). */
static bool expl_j6_add(struct expl_j6_set* m, int x, int y, int z) {
	if(!m->table) {
		m->table = malloc(16 * sizeof(*m->table));
		if(!m->table)
			return false;
		for(uint32_t i = 0; i < 16; i++)
			m->table[i] = -1;
		m->len = 16;
		m->threshold = (uint32_t)(16 * 0.75f);
	}
	const int32_t hc = (int32_t)((uint32_t)x * 8976890u
								 + (uint32_t)y * 981131u + (uint32_t)z);
	const int32_t h = expl_j6_supplemental_hash(hc);
	const uint32_t bi = (uint32_t)h & (m->len - 1);
	for(int32_t e = m->table[bi]; e >= 0; e = m->next[e])
		if(m->hash[e] == h && m->keys[e].x == x && m->keys[e].y == y
		   && m->keys[e].z == z)
			return true; /* already present: order unchanged */
	if(m->size == m->cap) {
		uint32_t ncap = m->cap ? m->cap * 2 : 256;
		struct broken_coord* nk = realloc(m->keys, ncap * sizeof(*nk));
		if(!nk)
			return false;
		m->keys = nk;
		int32_t* nh = realloc(m->hash, ncap * sizeof(*nh));
		if(!nh)
			return false;
		m->hash = nh;
		int32_t* nn = realloc(m->next, ncap * sizeof(*nn));
		if(!nn)
			return false;
		m->next = nn;
		m->cap = ncap;
	}
	const int32_t n = (int32_t)m->size;
	m->keys[n].x = x;
	m->keys[n].y = y;
	m->keys[n].z = z;
	m->hash[n] = h;
	m->next[n] = m->table[bi];
	m->table[bi] = n;
	if(m->size++ >= m->threshold && !expl_j6_resize(m, m->len * 2))
		return false;
	return true;
}

/* ArrayList.addAll(set): HashIterator order into out[0..size). */
static void expl_j6_iterate(const struct expl_j6_set* m,
							struct broken_coord* out) {
	size_t n = 0;
	for(uint32_t j = 0; j < m->len; j++)
		for(int32_t e = m->table[j]; e >= 0; e = m->next[e])
			out[n++] = m->keys[e];
	assert(n == m->size);
}

/* ===== Beta Explosion.doExplosionA / World.func_675_a support ============ */

/* MathHelper.floor_double: a TRUE floor (not the Java (int) truncation). */
static int expl_floor_double(double v) {
	int i = (int)v;
	return v < (double)i ? i - 1 : i;
}

/* Per-blast cost record for the Wii budget question (docs/deviations.md #4 asks
 * for a re-measure against the old 300-ray model). Read back through
 * server_world_explosion_last_cost after any blast. */
static struct server_world_explosion_cost s_expl_last_cost;

void server_world_explosion_last_cost(struct server_world_explosion_cost* out) {
	assert(out);
	*out = s_expl_last_cost;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* F4-RNG test seam: the last blast's doEffects visit sequence (air included). */
static int (*s_test_blast_order)[3] = NULL;
static size_t s_test_blast_order_n = 0, s_test_blast_order_cap = 0;
static void server_world_test_record_blast_order(
	const struct broken_coord* cells, size_t n, bool reverse) {
	if(n > s_test_blast_order_cap) {
		int (*nb)[3] = realloc(s_test_blast_order, n * sizeof(*nb));
		if(!nb) {
			s_test_blast_order_n = 0;
			return;
		}
		s_test_blast_order = nb;
		s_test_blast_order_cap = n;
	}
	for(size_t i = 0; i < n; i++) {
		const struct broken_coord* c = &cells[reverse ? n - 1 - i : i];
		s_test_blast_order[i][0] = c->x;
		s_test_blast_order[i][1] = c->y;
		s_test_blast_order[i][2] = c->z;
	}
	s_test_blast_order_n = n;
}
size_t server_world_test_last_blast_order(int (*xyz)[3], size_t cap) {
	size_t n = s_test_blast_order_n < cap ? s_test_blast_order_n : cap;
	if(xyz && n)
		memcpy(xyz, s_test_blast_order, n * sizeof(*xyz));
	return s_test_blast_order_n;
}
#endif

/* Beta Block.collisionRayTrace's segment clip against ONE box: each of the six
 * face planes is intersected by the segment a->b (getIntermediateWith?Value:
 * only a parameter in [0,1] counts, and a segment shorter than 1e-7^0.5 along
 * that axis never reaches the plane), and a face point counts when it lies
 * inside the face's other two extents (isVecInside??Bounds, INCLUSIVE). A
 * start point already INSIDE the box therefore sees every face behind it and
 * misses -- a Beta property this keeps on purpose, not a bug to fix.
 * [740c583 Block.collisionRayTrace:357-419, Vec3D.getIntermediateWithXValue] */
static bool expl_segment_hits_box(const double a[3], const double b[3],
								  const struct AABB* box) {
	const double lo[3] = {box->x1, box->y1, box->z1};
	const double hi[3] = {box->x2, box->y2, box->z2};
	for(int axis = 0; axis < 3; axis++) {
		double d = b[axis] - a[axis];
		if(d * d < (double)1.0E-7F) /* Vec3D.getIntermediateWith?Value:105 */
			continue;
		for(int face = 0; face < 2; face++) {
			double plane = face ? hi[axis] : lo[axis];
			double t = (plane - a[axis]) / d;
			if(t < 0.0 || t > 1.0)
				continue;
			int u = (axis + 1) % 3, v = (axis + 2) % 3;
			double pu = a[u] + (b[u] - a[u]) * t;
			double pv = a[v] + (b[v] - a[v]) * t;
			if(pu >= lo[u] && pu <= hi[u] && pv >= lo[v] && pv <= hi[v])
				return true;
		}
	}
	return false;
}

/* World.rayTraceBlocks' per-cell blocker test: `id > 0 &&
 * canCollideCheck(meta, false) && collisionRayTrace(...) != null`.
 * canCollideCheck(meta, false) is Block.isCollidable() for every block except
 * the fluids (BlockFluid: `var2 && var1 == 0` -> false here), fire
 * (BlockFire.isCollidable false) and stairs (delegates to the model block,
 * collidable). collisionRayTrace is setBlockBoundsBasedOnState + the clip
 * above, i.e. this engine's getBoundingBox(this, entity=false) selection
 * boxes. [740c583 World.func_28105_a:717-719, BlockFluid.canCollideCheck:56,
 * BlockFire.isCollidable:156] */
static bool expl_cell_blocks_ray(struct server_world* w, int x, int y, int z,
								 const double from[3], const double to[3]) {
	struct block_data blk;
	if(!server_world_get_block(w, x, y, z, &blk))
		return false; /* getBlockId == 0 outside the loaded world */
	if(blk.type == 0 || blk.type == BLOCK_FIRE || !blocks[blk.type]
	   || !blocks[blk.type]->getBoundingBox)
		return false;
	if(blk.type == BLOCK_WATER_STILL || blk.type == BLOCK_WATER_FLOW
	   || blk.type == BLOCK_LAVA_STILL || blk.type == BLOCK_LAVA_FLOW)
		return false;
	struct block_info info = {.block = &blk, .neighbours = NULL,
							  .x = x, .y = y, .z = z};
	/* BlockCactus overrides only the POOL getters (selection/collision
	 * insets of 1/16); it never calls setBlockBounds, so collisionRayTrace
	 * clips the default full cube. The engine's getBoundingBox(entity=false)
	 * is the selection inset, so the density ray takes the full cube here.
	 * [740c583 BlockCactus.getSelectedBoundingBoxFromPool:35 vs
	 *  Block.collisionRayTrace:357 (this.minX..maxX untouched)] */
	if(blk.type == BLOCK_CACTUS) {
		struct AABB cube = {0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};
		aabb_translate(&cube, (float)x, (float)y, (float)z);
		return expl_segment_hits_box(from, to, &cube);
	}
	size_t count = blocks[blk.type]->getBoundingBox(&info, false, NULL);
	if(count == 0)
		return false;
	struct AABB boxes[count];
	blocks[blk.type]->getBoundingBox(&info, false, boxes);
	for(size_t k = 0; k < count; k++) {
		aabb_translate(boxes + k, (float)x, (float)y, (float)z);
		if(expl_segment_hits_box(from, to, boxes + k))
			return true;
	}
	return false;
}

/* World.rayTraceBlocks(from, to) == func_28105_a(from, to, false, false): the
 * start cell is tested, then the exact Beta plane-crossing walk (at most 200
 * steps) tests each entered cell until the END cell is reached -- the end cell
 * itself is never tested. Returns true iff some tested cell blocks the ray.
 * `from` is copied because Beta advances var1 in place.
 * [740c583 World.func_28105_a:704-851] */
static bool expl_ray_blocked(struct server_world* w, const double from_in[3],
							 const double to[3]) {
	double var1[3] = {from_in[0], from_in[1], from_in[2]};
	int var5 = expl_floor_double(to[0]);
	int var6 = expl_floor_double(to[1]);
	int var7 = expl_floor_double(to[2]);
	int var8 = expl_floor_double(var1[0]);
	int var9 = expl_floor_double(var1[1]);
	int var10 = expl_floor_double(var1[2]);
	s_expl_last_cost.density_cells++;
	if(expl_cell_blocks_ray(w, var8, var9, var10, var1, to))
		return true;
	int var11 = 200;
	while(var11-- >= 0) {
		if(var8 == var5 && var9 == var6 && var10 == var7)
			return false;
		bool var39 = true, var40 = true, var41 = true;
		double var15 = 999.0, var17 = 999.0, var19 = 999.0;
		if(var5 > var8)
			var15 = (double)var8 + 1.0;
		else if(var5 < var8)
			var15 = (double)var8 + 0.0;
		else
			var39 = false;
		if(var6 > var9)
			var17 = (double)var9 + 1.0;
		else if(var6 < var9)
			var17 = (double)var9 + 0.0;
		else
			var40 = false;
		if(var7 > var10)
			var19 = (double)var10 + 1.0;
		else if(var7 < var10)
			var19 = (double)var10 + 0.0;
		else
			var41 = false;
		double var21 = 999.0, var23 = 999.0, var25 = 999.0;
		double var27 = to[0] - var1[0];
		double var29 = to[1] - var1[1];
		double var31 = to[2] - var1[2];
		if(var39)
			var21 = (var15 - var1[0]) / var27;
		if(var40)
			var23 = (var17 - var1[1]) / var29;
		if(var41)
			var25 = (var19 - var1[2]) / var31;
		int var42;
		if(var21 < var23 && var21 < var25) {
			var42 = var5 > var8 ? 4 : 5;
			var1[0] = var15;
			var1[1] += var29 * var21;
			var1[2] += var31 * var21;
		} else if(var23 < var25) {
			var42 = var6 > var9 ? 0 : 1;
			var1[0] += var27 * var23;
			var1[1] = var17;
			var1[2] += var31 * var23;
		} else {
			var42 = var7 > var10 ? 2 : 3;
			var1[0] += var27 * var25;
			var1[1] += var29 * var25;
			var1[2] = var19;
		}
		var8 = expl_floor_double(var1[0]);
		if(var42 == 5)
			--var8;
		var9 = expl_floor_double(var1[1]);
		if(var42 == 1)
			--var9;
		var10 = expl_floor_double(var1[2]);
		if(var42 == 3)
			--var10;
		s_expl_last_cost.density_cells++;
		if(expl_cell_blocks_ray(w, var8, var9, var10, var1, to))
			return true;
	}
	return false;
}

/* World.func_675_a (the explosion's block-density / exposure factor): sample
 * the entity's bounding box on a lattice of (2*extent+1) points per axis --
 * float loop counter, double increment, exactly as Beta -- and count the
 * samples whose ray to the blast centre is unobstructed.
 * [740c583 World.func_675_a:1522-1545] */
/* Explosion.getBlockDensity over a DOUBLE box (the entity's boundingBox --
 * a mob's is the authoritative double box, mob motion units S2). */
static float expl_block_density(struct server_world* w, const double c[3],
								const double bb6[6]) {
	double minX = bb6[0], maxX = bb6[3], minY = bb6[1], maxY = bb6[4],
		   minZ = bb6[2], maxZ = bb6[5];
	double var3 = 1.0 / ((maxX - minX) * 2.0 + 1.0);
	double var5 = 1.0 / ((maxY - minY) * 2.0 + 1.0);
	double var7 = 1.0 / ((maxZ - minZ) * 2.0 + 1.0);
	/* Beta would spin forever on a zero/negative-extent box; every real
	 * entity box has positive extents, so a degenerate one is a corrupt
	 * entity, treated as fully exposed rather than a hang. */
	if(!(var3 > 0.0 && var3 <= 1.0 && var5 > 0.0 && var5 <= 1.0 && var7 > 0.0
		 && var7 <= 1.0))
		return 1.0F;
	int var9 = 0, var10 = 0;
	for(float var11 = 0.0F; var11 <= 1.0F;
		var11 = (float)((double)var11 + var3)) {
		for(float var12 = 0.0F; var12 <= 1.0F;
			var12 = (float)((double)var12 + var5)) {
			for(float var13 = 0.0F; var13 <= 1.0F;
				var13 = (float)((double)var13 + var7)) {
				double p[3] = {minX + (maxX - minX) * (double)var11,
							   minY + (maxY - minY) * (double)var12,
							   minZ + (maxZ - minZ) * (double)var13};
				s_expl_last_cost.density_rays++;
				if(!expl_ray_blocked(w, p, c))
					++var9;
				++var10;
			}
		}
	}
	return (float)var9 / (float)var10;
}

/* Beta EntityPlayer boundingBox: setSize(0.6F, 1.8F) with yOffset 1.62F; the
 * server player's y IS Beta's posY (eye level), so the box spans feet..feet+1.8.
 * [740c583 EntityPlayer.<init>: yOffset = 1.62F; Entity.setSize] */
static void expl_player_box(const struct server_player* p, struct AABB* out) {
	float feet = (float)(p->y - PLAYER_EYE_HEIGHT);
	out->x1 = (float)p->x - 0.3F;
	out->x2 = (float)p->x + 0.3F;
	out->y1 = feet;
	out->y2 = feet + 1.8F;
	out->z1 = (float)p->z - 0.3F;
	out->z2 = (float)p->z + 0.3F;
}

/* One entity-pass victim: Beta doExplosionA:87-105 for a single candidate whose
 * box already intersects the blast box. `pos` is the Beta posX/Y/Z, `box` the
 * boundingBox. Returns false if the candidate was out of range (var13 > 1). */
static bool expl_victim_terms(struct server_world* w, const double c[3],
							  float size2, const double pos[3],
							  const double box[6], double dir[3],
							  int* damage, double* impulse) {
	double var15 = pos[0] - c[0];
	double var17 = pos[1] - c[1];
	double var19 = pos[2] - c[2];
	/* Entity.getDistance: (double)MathHelper.sqrt_double(...) -- a FLOAT sqrt. */
	double var13
		= (double)(float)sqrt(var15 * var15 + var17 * var17 + var19 * var19)
		/ (double)size2;
	if(var13 > 1.0)
		return false;
	double var39 = (double)(float)sqrt(var15 * var15 + var17 * var17
									   + var19 * var19);
	if(var39 > 0.0) {
		var15 /= var39;
		var17 /= var39;
		var19 /= var39;
	} else {
		/* Beta divides by zero here (NaN motion) for an entity exactly at the
		 * centre -- a corruption, not a value to reproduce; zero direction. */
		var15 = var17 = var19 = 0.0;
	}
	float var40 = expl_block_density(w, c, box);
	*damage = explosion_entity_damage(var13, var40, size2, impulse);
	dir[0] = var15;
	dir[1] = var17;
	dir[2] = var19;
	return true;
}

/* The player leg of the entity pass (host or admitted guest). Damage crosses
 * the entity boundary here ONCE: Beta half-hearts x8 = engine units
 * (CODESTYLE 2.4). Beta EntityLiving.attackEntityFrom applies knockBack away
 * from a non-null exploder (a creeper) before the explosion adds its own
 * motion impulse; the null-exploder (TNT/bed/fireball) hit has no knockBack.
 * G8-BLAST-VELOCITY (ASTRA-RB06-RESIDUALS section 2): all three impulse
 * components go to the server's MOTION copy (never the position), absorbed
 * hits included; a fresh hit raises beenAttacked and the body's own client
 * gets one absolute Packet28 at the tracker phase
 * (server_local_flush_actor_velocity) -- no immediate send here.
 * [740c583 Explosion.doExplosionA:100-103, EntityLiving.attackEntityFrom:293-344,
 *  EntityTrackerEntry:125-127] */
/* Explosion.doExplosion:78-85 constructs floor_double bounds from the
 * double center. Keep those integers in double: float loses odd bounds at
 * the historical HashMap overflow witness's +/-25,000,000 coordinates. */
static bool expl_blast_intersection(const struct AABB* box, const double b[6]) {
	return (double)box->x2 > b[0] && (double)box->x1 < b[3]
		&& (double)box->y2 > b[1] && (double)box->y1 < b[4]
		&& (double)box->z2 > b[2] && (double)box->z1 < b[5];
}

static void expl_hit_player(struct server_world* w, unsigned ci,
							const struct entity* exploder, const double c[3],
							float size2, const double blast_box[6]) {
	struct server_local* s = w->session;
	struct server_player* victim = server_local_damage_candidate(s, ci);
	if(!victim || !victim->has_pos)
		return;
	/* A held transfer owner has no controllable body until its own handoff
	 * finishes; exclude it before both damage and the blast impulse. */
	if(server_transfer_actor_inert(s, victim))
		return;
	/* A1a (D1): a blast damages only actors committed to its own world. */
	if(server_local_world_of(s, victim) != w)
		return;
	struct AABB box;
	expl_player_box(victim, &box);
	if(!expl_blast_intersection(&box, blast_box))
		return;
	double pos[3] = {victim->x, victim->y, victim->z};
	double dir[3], impulse;
	int damage;
	const double box6[6] = {box.x1, box.y1, box.z1, box.x2, box.y2, box.z2};
	if(!expl_victim_terms(w, c, size2, pos, box6, dir, &damage, &impulse))
		return;
	s_expl_last_cost.entities++;
	float kx = 0.0F, kz = 0.0F;
	if(exploder) {
		kx = (float)(victim->x - (double)exploder->pos[0]);
		kz = (float)(victim->z - (double)exploder->pos[2]);
	}
	/* M2 integration (W1F x G2): Explosion.doExplosionA:100 passes the
	 * EXPLODER as the attacker, so a creeper's blast (EntityCreeper extends
	 * EntityMob) goes through EntityPlayer.attackEntityFrom:366-377's
	 * difficulty scale -- Peaceful 0 (return false: no i-frames, no
	 * knockBack), Easy /3+1, Hard *3/2 -- exactly like its melee. TNT, beds
	 * and fireballs pass null and stay unscaled. The impulse below is
	 * Explosion:101-103, unconditional either way. */
	const bool from_entity_mob = exploder && exploder->type == ENTITY_MONSTER
		&& monster_is_beta_entity_mob(exploder->data.monster.id);
	const short old_health = victim->health;
	/* v5: the exploder as attackEntityFrom's attacker (its absence = TNT /
	 * bed / fireball = Beta's null branch); the delta keeps the float
	 * narrowing kx/kz always had (exploder pos is float). */
	bool alive = server_local_blast_damage_actor(s, victim, damage * 8,
		exploder != NULL, -(double)kx, -(double)kz, from_entity_mob);
	if(ci == 0 && s->portal_demo_split && victim->health != old_health) {
		static unsigned blast_events;
		if(blast_events < 12) {
			blast_events++;
			const struct server_world* exploder_world = exploder
				? server_local_entity_world(s, exploder) : NULL;
			const struct server_world* victim_world
				= server_local_world_of(s, victim);
			fprintf(stderr,
				"[split_blast] world=%u/%u/dim%d center=(%.3f,%.3f,%.3f) power=%.2f exploder=%d/%u/mob%d exploder_world=%d/%u/dim%d victim_world=%d/%u/dim%d victim=(%.3f,%.3f,%.3f) damage=%d old=%d new=%d\n",
				w->world_index, w->generation, w->dimension,
				c[0], c[1], c[2], size2 * 0.5f,
				exploder ? (int)exploder->type : -1,
				exploder ? exploder->id : 0,
				exploder && exploder->type == ENTITY_MONSTER
					? exploder->data.monster.id : -1,
				exploder_world ? exploder_world->world_index : -1,
				exploder_world ? exploder_world->generation : 0,
				exploder_world ? exploder_world->dimension : 0,
				victim_world ? victim_world->world_index : -1,
				victim_world ? victim_world->generation : 0,
				victim_world ? victim_world->dimension : 0,
				pos[0], pos[1], pos[2], damage * 8, old_health,
				victim->health);
		}
	}
	/* Beta adds the blast impulse even when the damage was absorbed by
	 * i-frames. A lethal hit has already respawned/repositioned the player,
	 * so never shove that new body. The candidate is re-resolved: the
	 * respawn transaction invalidates player pointers. */
	victim = server_local_damage_candidate(s, ci);
	if(!alive || !victim || s->zombie_demo)
		return;
	server_local_add_actor_motion(s, victim, dir[0] * impulse,
								  dir[1] * impulse, dir[2] * impulse);
}

/* The non-player leg: every entity kind's attackEntityFrom(exploder, dmg)
 * followed by the unconditional motion impulse [740c583 Explosion.doExplosionA
 * :100-103]. Per kind:
 *  - EntityLiving (mobs): the shared i-frame arbitration
 *    (entity_monster_apply_damage / entity_wolf_attack_from), then knockBack
 *    away from a non-null exploder on a FRESH hit [EntityLiving.attackEntityFrom
 *    :336-344, knockBack:392-405]. A mob left with health <= 0 dies on its own
 *    tick's death edge (onDeath + drops), exactly as Beta's next onUpdate.
 *  - EntityItem: health -= dmg; <= 0 is setEntityDead (the item tick removes
 *    it) [EntityItem.attackEntityFrom:87-93].
 *  - EntityMinecart / EntityBoat: rock direction flips, 10 shake ticks,
 *    currentDamage += dmg*10 [EntityMinecart.attackEntityFrom:73-79,
 *    EntityBoat.attackEntityFrom:63-68]. The > 40 break-and-drop frees the
 *    entity, which cannot happen inside the entity tick's live dict
 *    iteration this pass runs in, so it is latched (blast_break_pending)
 *    and consumed at the top of the vehicle's own next tick (Beta's
 *    setEntityDead is likewise only honoured by the world's list sweep).
 *  - Everything else (primed TNT, projectiles, falling sand, fish): Entity's
 *    default setBeenAttacked no-op [Entity.attackEntityFrom:705-708].
 *  - Remote players are a client-side mirror of another server's body: no
 *    authority here, skipped.
 *  - Paintings: selected by their Entity.boundingBox (the frame box; their
 *    getBoundingBox accessor stays NULL) and broken with the item dropped
 *    [EntityPainting.attackEntityFrom:200-208] -- see the painting branch in
 *    expl_hit_entity. */
/* Beta `minecartCurrentDamage += var2 * 10` on an int; this engine stores an
 * int16 that decays 1/tick, so a chain of blasts must saturate rather than
 * wrap (a wrapped negative would silently heal the vehicle). */
static int16_t expl_vehicle_damage(int16_t current, int damage) {
	int32_t next = (int32_t)current + damage * 10;
	if(next > INT16_MAX)
		next = INT16_MAX;
	return (int16_t)next;
}

static void expl_hit_entity(struct server_world* w, struct entity* e,
							const struct entity* exploder, const double c[3],
							float size2, const double blast_box[6]) {
	struct server_local* s = w->session;
	/* A1a (D1): an entity is hit only by a blast in the world it lives in. */
	if(e == exploder || e->type == ENTITY_REMOTE_PLAYER || e->world != w)
		return;
	struct AABB box;
	/* Paintings keep e->getBoundingBox NULL (Beta's collision accessor is
	 * null for them, so placement and pistons ignore the frame) but DO have
	 * Entity.boundingBox (EntityPainting.setDirection :123), the box
	 * getEntitiesWithinAABBExcludingEntity selects by. A selected painting
	 * takes attackEntityFrom(exploder, damage) [Explosion.doExplosionA
	 * :100-103] -> EntityPainting.attackEntityFrom :200-208: dies and drops
	 * Item.painting, whatever the damage. The drop cannot spawn inside this
	 * entity walk, so the break is latched and consumed on the painting's
	 * own next tick (the vehicles' blast_break_pending pattern). Its motion
	 * impulse is moot (it is dead). */
	if(e->type == ENTITY_PAINTING) {
		if(!entity_painting_hit_box(e, &box)
		   || !expl_blast_intersection(&box, blast_box))
			return;
		double ppos[3] = {e->pos[0], e->pos[1], e->pos[2]};
		const double pbox6[6] = {box.x1, box.y1, box.z1,
								 box.x2, box.y2, box.z2};
		double pdir[3], pimpulse;
		int pdamage;
		if(!expl_victim_terms(w, c, size2, ppos, pbox6, pdir, &pdamage,
							  &pimpulse))
			return;
		s_expl_last_cost.entities++;
		entity_painting_mark_broken(e);
		return;
	}
	if(!e->getBoundingBox || e->getBoundingBox(e, &box) == 0)
		return;
	double pos[3] = {e->pos[0], e->pos[1], e->pos[2]};
	double box6[6] = {box.x1, box.y1, box.z1, box.x2, box.y2, box.z2};
	if(e->type == ENTITY_MONSTER) {
		/* Mob motion units S2: a mob's posX/Y/Z and boundingBox are the
		 * authoritative doubles; the blast box's floor()ed bounds widen
		 * exactly, so the strict test runs in doubles as Beta's does. */
		const struct mob_motion* emd = &e->data.monster.motion_d;
		for(int k = 0; k < 3; k++) {
			pos[k] = emd->pos[k];
			box6[k] = emd->bb.min[k];
			box6[k + 3] = emd->bb.max[k];
		}
		const double* bx = blast_box;
		if(!(box6[3] > bx[0] && box6[0] < bx[3] && box6[4] > bx[1]
			 && box6[1] < bx[4] && box6[5] > bx[2] && box6[2] < bx[5]))
			return;
	} else if(!expl_blast_intersection(&box, blast_box)) {
		return;
	}
	double dir[3], impulse;
	int damage;
	if(!expl_victim_terms(w, c, size2, pos, box6, dir, &damage, &impulse))
		return;
	s_expl_last_cost.entities++;
	switch(e->type) {
		case ENTITY_MONSTER: {
			enum monster_damage_result r;
			if(entity_is_wolf(e))
				r = entity_wolf_attack_from(e, s, damage,
											exploder ? exploder->id : UINT32_MAX,
											false, false, false,
											ACTOR_IDENT_HOST);
			else
				r = entity_monster_apply_damage(e, damage, false);
			struct mob_motion* emd = &e->data.monster.motion_d;
			if(exploder && r == MONSTER_DAMAGE_FRESH) {
				/* Explosion.doExplosionA:100-103 attackEntityFrom(exploder,
				 * dmg) -> EntityLiving.attackEntityFrom:336-346 knockBack
				 * (exploder.posX - posX, exploder.posZ - posZ) AT THE HIT,
				 * then the blast adds its impulse on top (mob motion units
				 * S2, Q3): the old deferred compose clamped the lift away. */
				double ex = exploder->type == ENTITY_MONSTER
					? exploder->data.monster.motion_d.pos[0]
					: (double)exploder->pos[0];
				double ez = exploder->type == ENTITY_MONSTER
					? exploder->data.monster.motion_d.pos[2]
					: (double)exploder->pos[2];
				mob_motion_knockback(e, emd, ex - emd->pos[0],
									 ez - emd->pos[2],
									 server_local_sound_rng());
				e->data.monster.knockback_time = MOB_KNOCKBACK_TICKS;
			}
			/* `entity.motionX += var15 * var34` (doubles). */
			mob_motion_add_velocity(e, emd, dir[0] * impulse,
									dir[1] * impulse, dir[2] * impulse);
			/* Beta's mob AI never clamps motion, so the impulse simply
			 * carries and decays. This engine's Wii-playability speed cap
			 * (carve-out #2, entity_clamp_speed in server_tick_monster)
			 * WOULD clamp it on the very next tick; knockback_time is the
			 * engine's existing "let an impulse carry" gate (carve-out #2).
			 * A blast without a knockBack (null exploder) keeps the pre-card
			 * window length, one tick short of MOB_KNOCKBACK_TICKS. */
			if(impulse > 0.0 && e->data.monster.knockback_time == 0)
				e->data.monster.knockback_time = MOB_KNOCKBACK_TICKS - 1;
			break;
		}
		case ENTITY_ITEM:
			e->health -= (short)damage;
			e->vel[0] += (float)(dir[0] * impulse);
			e->vel[1] += (float)(dir[1] * impulse);
			e->vel[2] += (float)(dir[2] * impulse);
			break;
		case ENTITY_MINECART:
			e->data.minecart.shake_dir = (int8_t)-e->data.minecart.shake_dir;
			e->data.minecart.shake_ticks = 10;
			e->data.minecart.damage_taken = expl_vehicle_damage(
				e->data.minecart.damage_taken, damage);
			if(e->data.minecart.damage_taken > 40)
				e->data.minecart.blast_break_pending = true;
			e->vel[0] += (float)(dir[0] * impulse);
			e->vel[1] += (float)(dir[1] * impulse);
			e->vel[2] += (float)(dir[2] * impulse);
			entity_minecart_mark_snapshot_dirty(e, true);
			break;
		case ENTITY_BOAT:
			e->data.boat.shake_dir = (int8_t)-e->data.boat.shake_dir;
			e->data.boat.shake_ticks = 10;
			e->data.boat.damage_taken = expl_vehicle_damage(
				e->data.boat.damage_taken, damage);
			if(e->data.boat.damage_taken > BOAT_DESTROY_DAMAGE_THRESHOLD)
				e->data.boat.blast_break_pending = true;
			e->vel[0] += (float)(dir[0] * impulse);
			e->vel[1] += (float)(dir[1] * impulse);
			e->vel[2] += (float)(dir[2] * impulse);
			break;
		default:
			e->vel[0] += (float)(dir[0] * impulse);
			e->vel[1] += (float)(dir[1] * impulse);
			e->vel[2] += (float)(dir[2] * impulse);
			break;
	}
}

/* Explosion.ExplosionRNG: `new Random()` per Explosion (Explosion.java:11). */
/* F4-RNG: no process-lifetime stream any more -- every flaming Explosion
 * builds its own fresh java.util.Random. The test seam only pre-seeds the
 * NEXT explosion's fresh stream (one-shot), so it cannot carry state. */
static bool s_fire_rng_pending = false;
static int64_t s_fire_rng_seed = 0;
#ifdef CAVEWII_SERVER_LOCAL_TEST
static bool s_test_fire_flood_path = false;
void server_world_test_seed_fire_rng(uint32_t seed) {
	s_fire_rng_seed = (int64_t)seed;
	s_fire_rng_pending = true;
}
void server_world_test_set_fire_flood_path(bool enable) {
	s_test_fire_flood_path = enable;
}
#endif

/* The isFlaming `setBlockWithNotify(fire)` minus its per-cell light flood.
 * The cell stays in the destroyed set, so doExplosionB's pass 1 rewrites it
 * to AIR raw and pass 2's ONE batched flood settles it with every other
 * cell -- the same T1 the block destruction already relies on (a lit-then-
 * removed cell's light is a fixed point of the FINAL blocks, whatever the
 * transient held; test_explosion_beta_golden (o) proves blocks, metadata,
 * light and the neighbour-notify order identical to the flooding path).
 * Everything else setBlockWithNotify does is kept in order: id/metadata
 * write + old-block removal lifecycle, heightmap, onBlockAdded (fire's
 * tick schedule), then the six-neighbour notify.
 * [740c583 Explosion.doExplosionA:119, World.setBlockWithNotify] */
static void expl_place_fire_raw(struct server_world* w, int x, int y, int z) {
	/* Block callbacks (onBlockAdded, the fire portal hook) run only for the
	 * world they can reach until D4 -- see server_world_callback_host. */
	struct server_local* s = server_world_callback_host(w);
	struct block_data blk = {.type = BLOCK_FIRE, .metadata = 0,
							 .sky_light = 0, .torch_light = 15};
	bool changed = false;
	/* G9: the blast reserved this cell seed + column with its staging. */
	struct server_chunk* sc
		= server_world_write_block_lifecycle(w, x, y, z, blk, &changed, true, false);
	if(!sc)
		return;
	s_setblock_count++;
	if(w->dimension != WORLD_DIM_NETHER) {
		column_backing_write_begin(sc->backing, 0, 0, 1);
		lighting_heightmap_update(sc->heightmap, W2C_COORD(x), y, W2C_COORD(z),
								  blk.type, server_chunk_get_block, sc);
		column_backing_write_end(sc->backing, 0, 0, 1);
	}
	server_world_callback_guard(w, changed && blocks[blk.type]
									  && blocks[blk.type]->onBlockAdded);
	server_world_callback_guard(w, !w->editing_blocks); /* the fire portal hook */
	if(s && changed && blocks[blk.type] && blocks[blk.type]->onBlockAdded) {
		struct block_info info = {
			.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
		{ struct server_world* bw_prev = block_world_enter(s, w);
		blocks[blk.type]->onBlockAdded(s, &info);
		block_world_leave(s, bw_prev); }
		server_world_get_block(w, x, y, z, &blk);
	}
	if(s && blk.type == BLOCK_FIRE && !w->editing_blocks) {
		block_fire_on_added(s, x, y, z);
		server_world_get_block(w, x, y, z, &blk);
	}
	server_world_notify_block(w, x, y, z, blk, BLOCK_FIRE, sc);
}

static void server_world_explode_now(struct server_world* w,
									 const struct entity* exploder,
									 const double center[3], float power, bool flaming) {
	/* A1a: the blast belongs to `w`. Session services (entity table, sound
	 * sink) come from w->session; block writes, lighting, counters, drops,
	 * chain TNT and publications follow `w`; only actors and entities
	 * committed to `w` are damaged; block callbacks run only where
	 * server_world_callback_host allows (see the bridge comment above).
	 * F4-RNG: every World.rand draw (rays, sound, drops, chain fuse) is
	 * `w`'s OWN java.util.Random (w->rand) -- never the session xorshift. */
	struct server_local* s = w->session;
	assert(s);
	struct server_local* callbacks = server_world_callback_host(w);
	/* Snapshot the constructor coordinates before any publication/callback.
	 * Explosion.java:20-26 copies them into three owned double fields. */
	const double c[3] = {center[0], center[1], center[2]};
	/* Emit the cosmetic terminal event only when the explosion actually begins.
	 * A retained entity retry enters here only after admission and emits exactly
	 * once. Centralizing also covers synchronous bed/flaming explosions. */
	server_world_publish(w, &(struct client_rpc) {
		.type = CRPC_ENTITY_EXPLODE,
		.payload.entity_explode.pos = {(float)c[0], (float)c[1], (float)c[2]},
		.payload.entity_explode.power = power,
	});

    /* ---- Beta Explosion.doExplosion, block collection --------------------
     * 1352 deterministic rays (the surface cells of a 16^3 lattice, x/y/z
     * loop order), each with intensity size*(0.7F + World.rand.nextFloat()*
     * 0.6F) -- exactly ONE w->rand draw per ray, in lattice order -- marched
     * 0.3 blocks per step from the centre; each step first pays the block's
     * (getExplosionResistance + 0.3F) * 0.3F, then, if any intensity remains,
     * adds the cell (AIR INCLUDED) to destroyedBlockPositions, then decays
     * 0.3F * 12/16. Intensity/resistance are float, position/direction
     * double, as Beta. An unloaded chunk or out-of-world y reads as id 0,
     * Beta's getBlockId contract. destroyedBlockPositions is the OpenJDK 6
     * HashSet model (expl_j6_*, ASTRA-RNG2-RULING 10-14) fed the real
     * insertion sequence; `cells` is its iteration order, which every later
     * pass walks in REVERSE (Explosion.java:111,131).
     * [740c583 Explosion.doExplosion:29-75] */
    struct broken_coord* cells = NULL;
    size_t cc = 0;
    struct broken_coord* broken = NULL;
    size_t bc = 0;
    struct expl_j6_set seen;
    expl_j6_init(&seen);
    bool collect_ok = true;
    memset(&s_expl_last_cost, 0, sizeof(s_expl_last_cost));

    for(int gx = 0; gx < EXPLOSION_GRID && collect_ok; gx++) {
        for(int gy = 0; gy < EXPLOSION_GRID && collect_ok; gy++) {
            for(int gz = 0; gz < EXPLOSION_GRID && collect_ok; gz++) {
                if(!explosion_ray_cell(gx, gy, gz))
                    continue;
                double dir[3];
                explosion_ray_dir(gx, gy, gz, dir);
                float next_float = jrand_next_float(&w->rand);
                float var14 = explosion_ray_intensity(power, next_float);
                double var15 = c[0], var17 = c[1], var19 = c[2];
                s_expl_last_cost.rays++;

                for(const float var21 = EXPLOSION_RAY_STEP; var14 > 0.0F;
                    var14 -= var21 * (12.0F / 16.0F)) {
                    int var22 = expl_floor_double(var15);
                    int var23 = expl_floor_double(var17);
                    int var24 = expl_floor_double(var19);
                    struct block_data blk;
                    uint8_t var25 = 0;
                    s_expl_last_cost.ray_steps++;
                    s_expl_last_cost.block_lookups++;
                    if(server_world_get_block(w, var22, var23, var24,
                                              &blk))
                        var25 = blk.type;
                    if(var25 > 0)
                        var14 -= (explosion_block_resistance(var25) + 0.3F)
                            * var21;

                    if(var14 > 0.0F
                       && !expl_j6_add(&seen, var22, var23, var24)) {
                        collect_ok = false;
                        break;
                    }

                    var15 += dir[0] * (double)var21;
                    var17 += dir[1] * (double)var21;
                    var19 += dir[2] * (double)var21;
                }
            }
        }
    }
    if(collect_ok) {
        cc = seen.size;
        cells = cc ? malloc(cc * sizeof(*cells)) : NULL;
        if(cc && !cells)
            collect_ok = false;
        else
            expl_j6_iterate(&seen, cells);
    }
    expl_j6_destroy(&seen);
    s_expl_last_cost.cells = (uint32_t)cc;
#ifdef CAVEWII_SERVER_LOCAL_TEST
    if(collect_ok)
        server_world_test_record_blast_order(cells, cc, true);
#endif

    if(!collect_ok) {
        /* Working-set exhaustion mid-collect. NO block has been mutated yet
         * (damage and all writes happen below), so the honest outcome is a
         * fully aborted blast, never a silently truncated one -- truncation
         * is exactly the B7 failure class this rewrite removes. Only the
         * cosmetic ENTITY_EXPLODE event has fired; doEffects (sound) never
         * ran, so its two World.rand draws are not consumed. */
        fprintf(stderr,
                "[explosion] staging allocation failed; blast aborted before "
                "any block mutation\n");
        free(cells);
        return;
    }

    // Staging is heap-sized to THIS blast and allocated BEFORE the entity pass
    // (F2 rule): an allocation failure aborts before ANY mutation -- block or
    // player. The six arrays keep the established staging fault contract
    // (server_world_test_fail_blast_mallocs). They are indexed per mutated
    // cell (broken/expl_old/expl_sc/expl_lit) or per visited cell
    // (expl_tnt_reserved) and freed before the blast returns.
    struct world_modification_entry* expl_lit = NULL;
    struct block_data* expl_nw = NULL;
    struct block_data* expl_old = NULL;
    struct server_chunk** expl_sc = NULL;
    struct entity** expl_tnt_reserved = NULL;
    int nlit = 0;

    if(cc > 0) {
        broken = blast_staging_malloc(cc * sizeof(*broken));
        expl_lit = blast_staging_malloc(cc * sizeof(*expl_lit));
        expl_nw = blast_staging_malloc(cc * sizeof(*expl_nw));
        expl_old = blast_staging_malloc(cc * sizeof(*expl_old));
        expl_sc = blast_staging_malloc(cc * sizeof(*expl_sc));
        expl_tnt_reserved = blast_staging_malloc(cc * sizeof(*expl_tnt_reserved));
        if(!broken || !expl_lit || !expl_nw || !expl_old || !expl_sc
           || !expl_tnt_reserved) {
            /* Allocation failure: no block has been mutated and no entity
             * has been damaged. Abort the whole blast cleanly. */
            fprintf(stderr,
                    "[explosion] staging allocation failed; blast aborted "
                    "before any block or entity mutation\n");
            free(expl_lit);
            free(expl_nw);
            free(expl_old);
            free(expl_sc);
            free(expl_tnt_reserved);
            free(cells);
            free(broken);
            return;
        }
    }
    /* G9: reserve the blast pending light (one seed + one column per cell
     * for the isFlaming fire and again for the AIR write) with the staging,
     * before any mutation. A failure stops this world (light_fault) with
     * nothing damaged, lit or written. */
    const size_t expl_light_resv = s_light_sync_test ? 0 : 2 * cc;
    if(expl_light_resv
       && !server_world_light_reserve(w, expl_light_resv, expl_light_resv)) {
        free(expl_lit);
        free(expl_nw);
        free(expl_old);
        free(expl_sc);
        free(expl_tnt_reserved);
        free(cells);
        free(broken);
        return;
    }

    /* ---- Beta Explosion.doExplosionA, entity pass ------------------------
     * After the block set is collected and BEFORE doExplosionB mutates any
     * block: explosionSize is doubled, every entity whose boundingBox
     * intersects the (2*size + 1)-expanded floor()ed box -- except the
     * exploder -- takes exposure-scaled damage and a motion impulse. The
     * candidate order is World.getEntitiesWithinAABBExcludingEntity's chunk
     * walk (chunk x, chunk z, section, insertion order), which the entity
     * query snapshot reproduces (the host player is a member of that list in
     * Beta singleplayer). Entity pointers are re-resolved through the dict
     * before each hit: a player death inside the loop runs the respawn
     * transaction and no pointer may outlive that.
     * recovery-0916/01: the admitted split guest is not in Beta's list (it is
     * not a Beta entity); it is processed after the list, in the fixed
     * server_local_damage_candidate order every other world-hit sweep walks,
     * so with the split switch off the host executes exactly Beta's pass.
     * [740c583 Explosion.doExplosionA:77-105] */
    const float size2 = power * 2.0F; /* this.explosionSize *= 2.0F */
    const double blast_box[6] = {
        (double)expl_floor_double(c[0] - (double)size2 - 1.0),
        (double)expl_floor_double(c[1] - (double)size2 - 1.0),
        (double)expl_floor_double(c[2] - (double)size2 - 1.0),
        (double)expl_floor_double(c[0] + (double)size2 + 1.0),
        (double)expl_floor_double(c[1] + (double)size2 + 1.0),
        (double)expl_floor_double(c[2] + (double)size2 + 1.0),
    };
    /* World.getEntitiesWithinAABBExcludingEntity walks only the chunks
     * floor((min-2)/16)..floor((max+2)/16); the snapshot is bounded the same
     * way so a blast never sorts every entity in the world. */
    /* ... and, per chunk, only the vertical sections
     * floor((minY-2)/16)..floor((maxY+2)/16) clamped to 0..7
     * [740c583 Chunk.getEntitiesWithinAABBForEntity:485-493]. */
    int cy0 = expl_floor_double((blast_box[1] - 2.0) / 16.0);
    int cy1 = expl_floor_double((blast_box[4] + 2.0) / 16.0);
    if(cy0 < 0)
        cy0 = 0;
    if(cy1 >= WORLD_HEIGHT / 16)
        cy1 = WORLD_HEIGHT / 16 - 1;
    /* A2-E review F2: the blast is `w`'s -- only entities resolving to `w`
     * (the per-world exhaustive reference), never another world's or a
     * retired generation's bodies at the same coordinates. */
    if(server_local_prepare_entity_query_snapshot_bounded_w(
           s, w, expl_floor_double((blast_box[0] - 2.0) / 16.0),
           expl_floor_double((blast_box[2] - 2.0) / 16.0),
           expl_floor_double((blast_box[3] + 2.0) / 16.0),
           expl_floor_double((blast_box[5] + 2.0) / 16.0), cy0, cy1)) {
        size_t nrefs = 0;
        const struct server_entity_query_ref* refs
            = server_local_entity_query_snapshot(s, &nrefs);
        /* v5: each player body is hit at its OWN section-list position
         * (the guest is a section member too). */
        bool listed[2] = {false, false};
        for(size_t i = 0; i < nrefs; i++) {
            if(refs[i].player) {
                const unsigned ci = refs[i].actor ? 1u : 0u;
                listed[ci] = true;
                expl_hit_player(w, ci, exploder, c, size2, blast_box);
                continue;
            }
            struct entity** ptr = dict_entity_get(s->entities, refs[i].id);
            if(!ptr || !*ptr || *ptr != refs[i].entity)
                continue;
            expl_hit_entity(w, *ptr, exploder, c, size2, blast_box);
        }
        /* The snapshot lists the host only once its chunk membership is
         * registered (player.added_to_chunk); a body with a position but no
         * membership yet is still Beta's EntityPlayer in the world and is
         * hit after the list (its slot in Beta's chunk order is unknown). */
        if(!listed[0])
            expl_hit_player(w, 0, exploder, c, size2, blast_box);
        if(!listed[1])
            expl_hit_player(w, 1, exploder, c, size2, blast_box);
    } else {
        /* Snapshot storage exhausted: the pass still runs, in dict order
         * (only the ORDER of hits, hence of any death-drop RNG, deviates). */
        fprintf(stderr,
                "[explosion] entity query snapshot unavailable; entity pass "
                "in dict order\n");
        expl_hit_player(w, 0, exploder, c, size2, blast_box);
        dict_entity_it_t it;
        dict_entity_it(it, s->entities);
        while(!dict_entity_end_p(it)) {
            struct entity* e = dict_entity_ref(it)->value;
            dict_entity_next(it);
            /* A2-E review F2: the fallback walk is world-filtered too. */
            if(e && e->added_to_chunk && server_local_entity_world(s, e) == w)
                expl_hit_entity(w, e, exploder, c, size2, blast_box);
        }
        expl_hit_player(w, 1, exploder, c, size2, blast_box);
    }

    /* ---- Beta Explosion.doExplosion tail: isFlaming ----------------------
     * STILL inside doExplosion, i.e. BEFORE doEffects destroys anything:
     * walk the list copy of destroyedBlockPositions in REVERSE
     * (Explosion.java:108-111) and set fire on every cell that reads (LIVE)
     * air over an opaque cube with ExplosionRNG.nextInt(3) == 0. Those cells
     * stay in the set, so doEffects then finds id 51 > 0 there and removes
     * the fire again along with every other non-air cell -- the decompile's
     * literal order. ExplosionRNG is `private Random ExplosionRNG = new
     * Random()` (Explosion.java:11): a FRESH java.util.Random per Explosion,
     * never World.rand and never a process-lifetime stream (RNG ruling 7).
     * mc173 keeps the pre-fire air flag and so preserves its fire --
     * decompile > mc173 (CLAUDE.md).
     * [decompile 740c583 Explosion.doExplosion:107-122, doEffects:157-160] */
    if (flaming) {
        struct java_random explosion_rng;
        if(s_fire_rng_pending) {
            jrand_set_seed(&explosion_rng, s_fire_rng_seed);
            s_fire_rng_pending = false;
        } else {
            server_world_runtime_jrand_seed(&explosion_rng);
        }
        for (size_t i = cc; i-- > 0;) {
            int bx = cells[i].x, by = cells[i].y, bz = cells[i].z;
            struct block_data cur, below;
            if (!server_world_get_block(w, bx, by, bz, &cur)
                || cur.type != BLOCK_AIR)
                continue;
            if (!server_world_get_block(w, bx, by - 1, bz, &below)
                || !block_is_normal_cube(below.type)) // Beta opaqueCubeLookup
                continue;
            if (jrand_next_int_bound(&explosion_rng, 3) == 0) {
                s_expl_last_cost.fire_lit++;
#ifdef CAVEWII_SERVER_LOCAL_TEST
                if(s_test_fire_flood_path) {
                    server_world_set_block(
                        w, bx, by, bz,
                        (struct block_data) {.type = BLOCK_FIRE, .metadata = 0,
                                             .sky_light = 0, .torch_light = 15});
                    continue;
                }
#endif
                expl_place_fire_raw(w, bx, by, bz);
            }
        }
    }

    ptime_t expl_t0 = time_get();
    bool expl_top = (s_setblock_depth++ == 0);

    /* Reserve every chain TNT's raw entity memory + live-list capacity before
     * mutating any block (indexed by visited cell). The reservation is
     * deliberately detached: Beta does not construct the entity (and therefore
     * consumes no id/Math.random/World.rand draw) until AFTER
     * setBlockWithNotify(AIR). A failed reservation leaves that TNT cell
     * untouched and produces no transient AIR/restore neighbour cascade.
     * [decompile 740c583 Explosion.doEffects; BlockTNT.onBlockDestroyedByExplosion] */
    for (size_t i = 0; i < cc; i++) {
        struct block_data cur;
        expl_tnt_reserved[i] = NULL;
        if(server_world_get_block(w, cells[i].x, cells[i].y, cells[i].z, &cur)
           && cur.type == BLOCK_TNT) {
            expl_tnt_reserved[i] = server_local_reserve_primed_tnt(s);
            if(!expl_tnt_reserved[i])
                fprintf(stderr,
                        "[tnt] chain reservation failed at (%d,%d,%d); "
                        "block retained\n", cells[i].x, cells[i].y, cells[i].z);
        }
    }

    /* ---- Beta Explosion.doEffects(false) -- the SERVER entry -------------
     * WorldServer.newExplosion:91-97 calls doEffects(FALSE): no particle
     * draws exist on this path (RNG ruling 6). First the sound: TWO
     * sequential World.rand floats, (1.0F + (r1 - r2) * 0.2F) * 0.7F, at a
     * CONSTANT volume 4.0F -- drawn even when no listener hears it. Then,
     * for every cell of the list copy in REVERSE, read the LIVE id; if > 0:
     * dropBlockAsItemWithChance(0.3F) on World.rand, setBlockWithNotify(AIR)
     * (removal hook + neighbour notification, whose synchronous cascades --
     * support-loss drops included -- run BEFORE the next cell), then
     * onBlockDestroyedByExplosion (chain TNT: nextInt(fuse/4)+fuse/8 on
     * World.rand). Per cell, in order, against live reads: nothing is batched
     * ahead of a later cell (ASTRA-RNG2-RULING 7).
     * [740c583 Explosion.doEffects:126-163, WorldServer.newExplosion:91-97] */
    {
        const float r1 = jrand_next_float(&w->rand);
        const float r2 = jrand_next_float(&w->rand);
        /* split dims: stamped with the exploding world (heard only there) */
        server_local_emit_sound_w(w, SOUND_RANDOM_EXPLODE,
            (vec3){(float)c[0], (float)c[1], (float)c[2]}, 4.0F,
                                  (1.0F + (r1 - r2) * 0.2F) * 0.7F);
    }

    for (size_t vi = cc; vi-- > 0;) {
        int bx = cells[vi].x, by = cells[vi].y, bz = cells[vi].z;
        struct block_data cur;
        if(!server_world_get_block(w, bx, by, bz, &cur) || cur.type == 0) {
            if(expl_tnt_reserved[vi]) {
                server_local_cancel_primed_tnt_reservation(
                    s, expl_tnt_reserved[vi]);
                expl_tnt_reserved[vi] = NULL;
            }
            continue;
        }
        const bool is_tnt = cur.type == BLOCK_TNT;
        if(is_tnt && !expl_tnt_reserved[vi])
            continue; /* reservation failed: the block is retained */
        if(!is_tnt && expl_tnt_reserved[vi]) {
            server_local_cancel_primed_tnt_reservation(
                s, expl_tnt_reserved[vi]);
            expl_tnt_reserved[vi] = NULL;
        }
        const size_t i = bc++;
        broken[i] = cells[vi];
        expl_old[i] = cur;
        expl_nw[i] = (struct block_data){0}; // air

        /* Publish AIR now but retain the old metadata nibble until the
         * removal step, so the drop roll and onBlockRemoval observe Beta's
         * exact new-ID/old-metadata transition. */
        expl_sc[i] = server_world_begin_block_write(
			w, bx, by, bz, expl_nw[i], NULL, NULL);
        if (!expl_sc[i]) {
            if(expl_tnt_reserved[vi]) {
                server_local_cancel_primed_tnt_reservation(
                    s, expl_tnt_reserved[vi]);
                expl_tnt_reserved[vi] = NULL;
                fprintf(stderr,
                        "[tnt] chain AIR write failed at (%d,%d,%d); "
                        "reservation cancelled\n", bx, by, bz);
            }
            continue;
        }
#ifdef CAVEWII_SERVER_LOCAL_TEST
        if(server_world_test_block_write_hook) /* G9: the AIR write */
            server_world_test_block_write_hook(w, bx, by, bz,
                                               expl_nw[i].type, true);
#endif
        if(w->dimension != WORLD_DIM_NETHER) {
            tps_mutated(2);
            uint8_t* hp = &expl_sc[i]->heightmap[
                W2C_COORD(bx) + W2C_COORD(bz) * CHUNK_SIZE];
            const int old_h = *hp;
            column_backing_write_begin(expl_sc[i]->backing, 0, 0, 1);
            lighting_heightmap_update(
                expl_sc[i]->heightmap, W2C_COORD(bx), by,
                W2C_COORD(bz), expl_nw[i].type,
                server_chunk_get_block, expl_sc[i]);
            column_backing_write_end(expl_sc[i]->backing, 0, 0, 1);
            /* each setBlockWithNotify(AIR) runs Chunk.func_339_g at once */
            if(!s_light_sync_test)
                server_world_light_column_write(w, expl_sc[i], bx, bz,
                                                old_h, *hp);
        }
        if(!s_light_sync_test)
            server_world_light_seed(w, bx, by, bz);
        expl_lit[nlit++] = (struct world_modification_entry){
            .x = bx, .y = by, .z = bz, .blk = expl_nw[i] };

        const struct block* broken_block = blocks[expl_old[i].type];
        struct block_info broken_info
            = { .x=bx, .y=by, .z=bz, .block=&expl_old[i] };
        if(is_tnt) {
            /* BlockTNT.quantityDropped is 0: no drop draws. Then
             * setBlockWithNotify(air), construct EntityTNTPrimed (Math.random
             * launch), World.rand.nextInt(20)+10, then join. */
			server_world_dispatch_block_removal(
				w, bx, by, bz, &expl_old[i]);
			server_world_finish_block_metadata(
				expl_sc[i], bx, by, bz, expl_nw[i]);
			server_world_finish_block_metadata(
				expl_sc[i], bx, by, bz, expl_nw[i]);
            server_world_notify_block(
				w, bx, by, bz, expl_nw[i], expl_nw[i].type, expl_sc[i]);
            server_local_commit_reserved_chain_tnt(
                w, expl_tnt_reserved[vi],
                (vec3) {bx + 0.5F, by + 0.5F, bz + 0.5F});
            expl_tnt_reserved[vi] = NULL;
            continue;
        }
        // Beta BlockPistonMoving.dropBlockAsItemWithChance IGNORES the explosion's 0.3F chance:
        // it delegates straight to the STORED block's dropBlockAsItem, i.e. chance 1.0 (bug-hunt
        // D3). The per-item chance draws still happen (rand <= 1.0F always passes). Container
        // contents use their own RNG; Math.random stays distinct. [740c583:
        // BlockPistonMoving.dropBlockAsItemWithChance -> Block.dropBlockAsItem]
        server_local_spawn_block_drops_with_chance(
            w, &broken_info,
            expl_old[i].type == BLOCK_PISTON_MOVING ? 1.0F : 0.3F);
        server_world_callback_guard(w, broken_block && broken_block->onBreak);
        if(callbacks && broken_block && broken_block->onBreak)
            { struct server_world* bw_prev = block_world_enter(callbacks, w);
            broken_block->onBreak(callbacks, &broken_info);
            block_world_leave(callbacks, bw_prev); }
		server_world_dispatch_block_removal(w, bx, by, bz, &expl_old[i]);
		server_world_finish_block_metadata(
			expl_sc[i], bx, by, bz, expl_nw[i]);
		server_world_finish_block_metadata(
			expl_sc[i], bx, by, bz, expl_nw[i]);
        server_world_notify_block(
			w, bx, by, bz, expl_nw[i], expl_nw[i].type, expl_sc[i]);
    }
    s_expl_last_cost.broken = (uint32_t)bc;

    // G9: live play queued every cell above (Beta schedules each, the world
    // drain settles them); this flood is the test-only sync baseline. No
    // explosion-reachable neighbour handler reads the light field.
    if(s_light_sync_test)
    lighting_update_at_blocks_server(expl_lit, nlit,
                              w->dimension == WORLD_DIM_NETHER,
                              server_world_light_get_block,
                              server_world_light_set_light, w);

    // Instrument: count every write and wall-time the per-cell pass once
    // (depth-guarded like set_block, so a recursive set_block from a
    // neighbour handler isn't double-timed).
    s_setblock_count += nlit;
    if (expl_top)
        s_setblock_accum_us += time_diff_s(expl_t0, time_get()) * 1.0e6f;
    s_setblock_depth--;
    if(expl_light_resv)
        server_world_light_release(w, expl_light_resv, expl_light_resv);

    free(cells);
    free(broken);
    free(expl_lit);
    free(expl_nw);
    free(expl_old);
    free(expl_sc);
    free(expl_tnt_reserved);
}

bool server_world_try_explode(struct server_world* w, vec3 center, float power) {
	assert(w && w->session && center);
	/* A primed TNT or fully-fused creeper is already the durable owner of this
	 * blast. The caller keeps its source alive when this returns false and retries
	 * after begin_update resets the cap.
	 * [decompile 740c583 EntityTNTPrimed.onUpdate: explode before setDead] */
	if(!server_world_try_begin_explosion(w)) {
		w->explosion_deferrals++;
		return false;
	}
	server_world_explode_now(w, NULL,
        (double[3]){center[0], center[1], center[2]}, power, false);
	return true;
}

bool server_world_try_explode_by(struct server_world* w,
								 const struct entity* exploder, vec3 center,
								 float power) {
	assert(w && w->session && center);
	if(!server_world_try_begin_explosion(w)) {
		w->explosion_deferrals++;
		return false;
	}
	server_world_explode_now(w, exploder,
        (double[3]){center[0], center[1], center[2]}, power, false);
	return true;
}

void server_world_explode_precise(struct server_world* w,
								const double center[3], float power) {
	assert(w && w->session && center);
	w->explosions_this_tick++;
	server_world_explode_now(w, NULL, center, power, false);
}

void server_world_explode_flaming_precise(struct server_world* w,
										const double center[3], float power) {
	assert(w && w->session && center);
	w->explosions_this_tick++;
	server_world_explode_now(w, NULL, center, power, true);
}

void server_world_explode(struct server_world* w, vec3 center, float power) {
	assert(w && w->session && center);
	/* Direct callers have no source to retain. Beta runs them synchronously; the
	 * small production set (Nether bed plus debug fixtures) therefore bypasses the
	 * entity pacing cap instead of entering a volatile world-teardown queue. */
	w->explosions_this_tick++;
	server_world_explode_now(w, NULL,
        (double[3]){center[0], center[1], center[2]}, power, false);
}

void server_world_explode_flaming(struct server_world* w, vec3 center,
								  float power) {
	assert(w && w->session && center);
	w->explosions_this_tick++;
	server_world_explode_now(w, NULL,
        (double[3]){center[0], center[1], center[2]}, power, true);
}


// NOTE: no callers yet (parked for the mob-unstuck work). Non-const world:
// server_world_get_block updates the Tier-0 resolution cache.
bool server_world_find_empty_spot_nearby(const float pos[3], struct server_world *world, float out_pos[3]){
	const float offs[][3] = {
        { 1.0f, 0.0f,  0.0f }, { -1.0f, 0.0f,  0.0f },
        { 0.0f, 0.0f,  1.0f }, {  0.0f, 0.0f, -1.0f },
        { 1.0f, 0.0f,  1.0f }, { -1.0f, 0.0f, -1.0f },
        { 0.0f, 1.0f,  0.0f }, // bovenop als laatste
    };
    int num_offsets = sizeof(offs) / sizeof(offs[0]);
    for (int i = 0; i < num_offsets; ++i) {
        float nx = pos[0] + offs[i][0];
        float ny = pos[1] + offs[i][1];
        float nz = pos[2] + offs[i][2];
        int tx = (int)floorf(nx);
        int ty = (int)floorf(ny);
        int tz = (int)floorf(nz);
        struct block_data bd;
        // Unresolved chunk (or y out of range) is NOT a verified-empty spot --
        // and bd would be UNINITIALIZED (the old code indexed blocks[] with
        // garbage). Skip the candidate.
        if (!server_world_get_block(world, tx, ty, tz, &bd))
            continue;
        if (!blocks[bd.type] || !blocks[bd.type]->getBoundingBox) {
            out_pos[0] = nx; out_pos[1] = ny; out_pos[2] = nz;
            return true;
        }
    }
    return false;
}

/* Isolated production-pool workload: no installed client columns, renderer,
 * mesher or server worker. Channels must have been initialized by the caller.
 * Own temporary world; never writes an existing save. */
#ifdef _WIN32
#include <direct.h>
#endif
#include "g9_drain_workload.h"
bool server_world_g9_diagnostic(bool (*emit)(const char*)) {
	g9_drain_discard_publications = true;
	struct server_local* s = calloc(1, sizeof *s);
	if(!s) return false;
	/* IMP-TE-CAPS: growable tile slabs (every row free) */
	if(!server_local_te_tables_init(s)) {
		free(s);
		return false;
	}
	dict_entity_init(s->entities);
	string_t level;
	char dirname[96], region[112];
	bool made = false;
	for(unsigned n = 0; n < 1024 && !made; n++) {
		snprintf(dirname, sizeof dirname, "g9_drain_isolated_%llu_%u",
			(unsigned long long)time_diff_us((ptime_t){0}, time_get()), n);
#ifdef _WIN32
		made = _mkdir(dirname) == 0;
#else
		made = mkdir(dirname, 0777) == 0;
#endif
		if(!made && errno != EEXIST) break;
	}
	if(!made) { dict_entity_clear(s->entities); free(s); return false; }
	snprintf(region, sizeof region, "%s/region", dirname);
#ifdef _WIN32
	if(_mkdir(region) != 0) { dict_entity_clear(s->entities); free(s); return false; }
#else
	if(mkdir(region, 0777) != 0) { dict_entity_clear(s->entities); free(s); return false; }
#endif
	string_init_set_str(level, dirname);
	server_world_create(&s->worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	struct server_world* w = &s->worlds[0];
	w->session = w->local = s;
	s->scheduled_tick_budget = 128;
	s->rand_src.seed = 0x9E11u;
	s->world_time = 6000;
	server_local_skylight_recompute(s);
	/* The same edits/notifications, without a live client or a consumer
	 * thread. Discard each publication immediately outside the drain. */
	bool ok = true;
	for(int cx = -G9_DRAIN_RADIUS; cx <= G9_DRAIN_RADIUS && ok; cx++)
		for(int cz = -G9_DRAIN_RADIUS; cz <= G9_DRAIN_RADIUS && ok; cz++) {
			struct server_chunk* sc = NULL;
			ok = server_world_load_chunk(w, cx, cz, &sc) && sc;
		}
	uint8_t height = 0;
	ok = ok && server_world_get_height(w, 8, 8, &height);
	uint64_t pops = 0, pushes = 0, writes = 0;
	char row[640];
	if(ok) {
		g9_drain_floor(w, height + 12);
		ok = server_world_light_drain(w);
	}
	for(int t = 1; t <= G9_DRAIN_TICKS && ok; t++) {
		g9_drain_edits(w, height + 12, t);
		ok = server_world_light_drain(w);
		clin_drain_discard();
		if(t % 20) continue;
		struct server_world_light_drain_rec d;
		server_world_light_drain_last(&d);
		pops += d.pops; pushes += d.pushes; writes += d.writes;
		struct server_chunk* sc = server_world_resolve_chunk(w, -1, 1);
		snprintf(row, sizeof row,
			"\"tick\":%d,\"pops\":%u,\"us\":%u,\"yield_us\":%u,"
			"\"lock_us\":%u,\"recovery_us\":%u,\"yields\":%u,\"resolves\":%u,\"resolve_misses\":%u,"
			"\"frontier_addr\":%llu,\"seeds_addr\":%llu,\"chunk_addr\":%llu,"
			"\"ids_addr\":%llu,\"sky_addr\":%llu,\"torch_addr\":%llu}",
			t, d.pops, d.us, d.yield_us, d.lock_us, d.recovery_us, d.slices, d.resolves, d.resolve_misses,
			(unsigned long long)d.frontier_address,
			(unsigned long long)d.seeds_address, (unsigned long long)(uintptr_t)sc,
			(unsigned long long)(uintptr_t)sc->ids,
			(unsigned long long)(uintptr_t)sc->lighting_sky,
			(unsigned long long)(uintptr_t)sc->lighting_torch);
		if(!emit(row)) ok = false;
	}
	ok = ok && pops == 2209840 && pushes == 2206968 && writes == 367828;
	snprintf(row, sizeof row,
		"\"done\":true,\"ok\":%s,\"pops\":%llu,\"pushes\":%llu,\"writes\":%llu}",
		ok ? "true" : "false", (unsigned long long)pops,
		(unsigned long long)pushes, (unsigned long long)writes);
	ok = emit(row) && ok;
	dict_entity_clear(s->entities);
	server_world_discard(w);
	free(s);
	g9_drain_discard_publications = false;
	return ok;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
int server_world_test_notify_scope_depth(void) { return s_notify_depth; }
#endif
