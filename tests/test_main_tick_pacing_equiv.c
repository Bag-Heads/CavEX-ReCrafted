/* Copyright (c) 2026 CaveWii_v2 contributors.
 * REL-10-tests (R10 / BUG-TICK 1017 PLAN): MAIN pacing OFF vs ON leaves the
 * simulation byte-identical, single-world AND split (two live worlds).
 *
 * Every ARM runs in its own child process (process-global RNG streams and
 * entity ids must start equal). Two production workloads, nothing mocked:
 *
 *  STRESS  the perf_stress_5m light-queue stress driver on a real level
 *          (roof/torch edits, furnace, piston, blast, coherent pinned save),
 *          the real autosave cadence, a mid-run save-and-quit checkpoint and
 *          the exit checkpoint. Fixture = test_g9_save_io_equiv's session
 *          (pre-populated near ring, gen-elapsed IGNORE, pinned LastPlayed and
 *          publication stamp). One harness item entity is dropped at open so
 *          an entity-owned gameplay_rng is always resident.
 *  SPLIT   the REAL split admission (owner_swap_harness: host + admitted guest
 *          body, every update in lockstep with the client half so the
 *          harness adds no thread race), then the host's real A2-T transfer
 *          to the Nether: two LIVE
 *          worlds (slot 0 Overworld with the guest, slot 1 Nether with the
 *          host), with fixed glowstone toggles in both worlds driving real
 *          light drains.
 *
 * Threads mirror server_local_thread: a SERVER thread registers through
 * thread_g9_lease_local / thread_g9_tick_boost_local(true, 63) and brackets
 * each production server_local_test_update_once (update + epilogue) with
 * thread_g9_tick_boost_begin/end -- the boundaries REL-10 publishes -- while
 * MAIN, the pacing owner, runs frames of real thread_main_pacing_checkpoint_at
 * calls across every phase for the whole run. The only difference between a
 * reference arm and its paced arm is thread_main_pacing_configure.
 *
 * Deterministic donation engagement (today's window = a light drain): a
 * read-only G9B history hook sees each in-drain flood yield on the SERVER
 * thread. In a paced arm the first ENGAGE yields inside an armed drain window
 * hold the server (it only waits; no engine call) until MAIN records a real
 * pacing sleep, so every paced arm provably DONATED while the compared
 * workload ran -- not just "pacing was configured". The reference arm runs
 * the same hook and must see the same drain windows with zero sleeps.
 * Non-drain engagement is REL-10's change and is asserted by the RED
 * eligibility test (test_main_tick_pacing), not here.
 *
 * Arms:  off / on (preset 1, shipped) / on2 (preset 2)            STRESS
 *        split_off / split_on (preset 1)                          SPLIT
 * Observed after EVERY executed tick (identical tick count; no wall time):
 *   [0] world time and every session RNG (rand_src, container/dispenser/
 *       entity streams, Math.random), light-queue tick
 *   [1] per live world: World.rand (full state), hell populate RNG, tick
 *   [2] per live world: light seeds (ordered), undo columns, pins, faults
 *   [3] per live world: every resident column's ids/metadata/sky/torch/
 *       heightmap bytes, populated/sent, backing revisions
 *   [4] every entity: production archive record + its own gameplay_rng
 *   [5] per live world: the scheduled-tick queue in execution order + drops
 *   [6] host and guest bodies: pose, health, Entity.rand (full state),
 *       every inventory slot and the picked item
 *   [7] tile entities: every live furnace row (slot owner, position,
 *       burn/cook/current-item-burn timers, input/fuel/output stacks) and the
 *       whole moving-piston store (count/reserved/next_order, every used
 *       entry's position, stored block, facing, flags, both progress floats,
 *       loaded-list order, owner slot). Beta TileEntityFurnace.updateEntity
 *       advances AND resets furnaceCookTime every tick, so a cook-progress
 *       difference can vanish before any save; only a per-tick capture sees it.
 * plus the full save directory (every file's bytes) at the checkpoints.
 *
 * Detector self-check (must MISMATCH, first difference at the mutation tick):
 *   mutant_draw    one extra session-RNG draw            (STRESS)
 *   mutant_order   the harness's two same-cell edits swapped (STRESS)
 *   mutant_entity  one extra draw on an entity's gameplay_rng (STRESS)
 *   mutant_player  one extra draw on the host's Entity.rand  (STRESS)
 *   mutant_furnace the stress furnace's furnaceCookTime +1   (STRESS)
 *   mutant_piston  a moving piston's last_progress nudged one ulp, at the
 *                  first tick >= MUTATE_TICK with an entry in flight (STRESS)
 *   split_mutant_guest   one extra draw on the guest's Entity.rand (SPLIT)
 *   split_mutant_nether  one extra draw on the Nether World.rand   (SPLIT)
 * A paced arm that differs reports the first tick and field; that is a FAIL. */
#include "support/owner_swap_harness.h" /* check()/failures, split admission */
#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"

#include "block/blocks.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "item/inventory.h"
#include "item/items.h"
#include "lighting.h"
#include "game/difficulty.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_transfer.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/time.h"

#include <m-lib/m-string.h>
#include <math.h>
#include <zlib.h>

#define CLOCK_STAMP 0x5F5E1234u
#define TICKS 640
#define MID_TICK 320
#define MUTATE_TICK 200
#define SPLIT_TICKS 240
#define SPLIT_MUTATE_TICK 120
#define FIELDS 8
#define BARRIERS 2
#define ENGAGE 3 /* drain-window donations each paced arm must record */
#define ENGAGE_WAIT_S 20

enum arm {
	ARM_OFF, ARM_ON, ARM_ON2, ARM_ON1MS, ARM_MUTANT_DRAW, ARM_MUTANT_ORDER,
	ARM_MUTANT_ENTITY, ARM_MUTANT_PLAYER, ARM_MUTANT_FURNACE, ARM_MUTANT_PISTON,
	ARM_SPLIT_OFF, ARM_SPLIT_ON, ARM_SPLIT_ON1MS, ARM_SPLIT_MUTANT_GUEST, ARM_SPLIT_MUTANT_NETHER,
	ARM_COUNT
};
static const char* const ARM_NAME[ARM_COUNT] = {"off", "on", "on2", "on1ms", "mutant_draw",
	"mutant_order", "mutant_entity", "mutant_player", "mutant_furnace", "mutant_piston",
	"split_off", "split_on", "split_on1ms",
	"split_mutant_guest", "split_mutant_nether"};
static bool arm_split(enum arm a) { return a >= ARM_SPLIT_OFF; }
static bool arm_paced(enum arm a) {
	return a == ARM_ON || a == ARM_ON2 || a == ARM_ON1MS || a == ARM_SPLIT_ON
		|| a == ARM_SPLIT_ON1MS;
}

struct arm_record {
	uint64_t obs[TICKS][FIELDS];
	unsigned ticks;
	struct g9io_snap snap[BARRIERS];
	uint32_t pacing[THREAD_PACING_WORDS];
	uint32_t main_checks;
	uint32_t main_donations;  /* checkpoints that really slept */
	uint32_t drain_yields;    /* in-window flood yields the hook saw */
	uint32_t engaged;         /* yields held until MAIN donated */
	uint32_t engage_timeouts; /* yields MAIN never donated during */
	uint32_t entities_at_mutate;
	uint32_t furnace_rows_max;    /* most live furnace rows seen in one tick */
	uint32_t cooking_ticks;       /* ticks with a furnace furnaceCookTime > 0 */
	uint32_t cook_resets;         /* cookTime >0 -> 0 with no new output */
	uint32_t piston_moving_ticks; /* ticks with a used moving-piston entry */
	int32_t mutated_at;           /* tick a tile mutant planted its change; -1 = none */
};
static struct arm_record* s_rec;
static const char* s_root;
static enum arm s_arm;

/* ---- deterministic donation engagement (read-only history hook) --------- */
static uint32_t s_donations; /* MAIN writes, SERVER reads */
static bool s_measuring;     /* SERVER: inside the compared tick loop */
static bool engage_hook(const struct server_world_history_event* e) {
	if(strcmp(e->operation, "drain") || strcmp(e->edge, "yield")) return true;
	if(!s_measuring || !thread_drain_wait_active())
		return true; /* setup, or not inside an armed window */
	s_rec->drain_yields++;
	if(!arm_paced(s_arm) || s_rec->engaged + s_rec->engage_timeouts >= ENGAGE)
		return true;
	/* Hold the SERVER inside the drain window (a pure wait: no engine call,
	 * no simulation state touched) until MAIN's checkpoint sleeps. */
	const uint32_t before = __atomic_load_n(&s_donations, __ATOMIC_ACQUIRE);
	const ptime_t start = time_get();
	while(__atomic_load_n(&s_donations, __ATOMIC_ACQUIRE) == before
		  && time_diff_s(start, time_get()) < ENGAGE_WAIT_S)
		thread_yield();
	if(__atomic_load_n(&s_donations, __ATOMIC_ACQUIRE) != before) s_rec->engaged++;
	else s_rec->engage_timeouts++;
	return true;
}

/* ---- STRESS: the deterministic production session (test_g9_save_io_equiv) */
static struct server_local SS;
static void session_discard(const struct client_rpc* call, void* arg) {
	(void)call; (void)arg;
	clin_drain_discard();
}
static void session_open(struct server_local* s) {
	clin_test_set_block_pack(false);
	memset(s, 0, sizeof *s);
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true, false);
	string_t level;
	string_init_set_str(level, s_root);
	assert(level_archive_create_new(&s->level, level, 4242, (vec3) {8, 4, 8}));
	string_set(s->level_name, level);
	assert(server_world_create_with_populate_scratch(
		&s->worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true,
		s->world_populate_scratch));
	string_clear(level);
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	s->scheduled_tick_budget = 128;
	s->rand_src.seed = 0x9E11u;
	s->world_time = 6000;
	server_local_skylight_recompute(s);
	const int pr = MAX_VIEW_DISTANCE;
	for(int cx = -pr; cx <= pr; cx++)
		for(int cz = -pr; cz <= pr; cz++) {
			struct server_chunk* c = NULL;
			bool done = false;
			for(unsigned n = 0; n < 1000 && !done; n++) {
				server_world_begin_update(&s->worlds[0]);
				done = server_world_ensure_populated(&s->worlds[0], s, cx, cz, &c);
			}
			assert(done);
		}
	s->player.inventory.items[INVENTORY_SLOT_MAIN]
		= (struct item_data) {.id = ITEM_DIAMOND, .count = 3};
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 8, 8, &hh));
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 8.5;
	s->player.y = hh + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 8.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.oxygen = MAX_OXYGEN;
	server_local_test_seed_body_rand(s, &s->player, INT64_C(0x5EED10));
	s->render_demo_mob = -1;
	s->light_queue_stress = true;
	s->light_queue_defer = 1;
	s->autosave_ticks = 200;
	s->autosave_timer = 0;
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(server_local_test_chunk_send_batch());
	server_local_test_set_last_played(INT64_C(1791532400000));
	server_local_request_difficulty(0);
	/* A resident entity with its own gameplay_rng, far from the player so it
	 * is never collected during the run. */
	uint8_t ih = 0;
	assert(server_world_get_height(&s->worlds[0], 40, 8, &ih));
	struct item_data drop = {.id = BLOCK_COBBLESTONE, .count = 1};
	assert(server_local_spawn_item((vec3) {40.5F, ih + 1.0F, 8.5F}, &drop, false, s));
}
static void session_close(struct server_local* s) {
	server_world_destroy(&s->worlds[0]);
	level_archive_destroy(&s->level);
	server_world_populate_scratch_destroy(s->world_populate_scratch);
	inventory_destroy(&s->player.inventory, &s->player);
	dict_entity_clear(s->entities);
	dict_autosave_saved_clear(s->autosave_saved_this_epoch);
	for(unsigned k = 0; k < SERVER_LOCAL_WORLD_SLOTS; k++)
		dict_autosave_saved_clear(s->autosave_saved_this_epoch_w[k]);
	string_clear(s->level_name);
	if(s->residency)
		mem2_free(s->residency);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	server_local_test_set_near_send_minimum(0);
	server_local_test_set_last_played(0);
}

/* ---- observation -------------------------------------------------------- */
#define OBS(k, v) h[k] = g9io_fnv(h[k], &(v), sizeof(v))
static void obs_jrand(uint64_t* h, unsigned k, const struct java_random* r) {
	OBS(k, r->seed); OBS(k, r->have_next_gaussian); OBS(k, r->next_gaussian_cached);
}
static void obs_body(uint64_t* h, const struct server_player* p) {
	const unsigned k = 6;
	OBS(k, p->x); OBS(k, p->y); OBS(k, p->z); OBS(k, p->health); OBS(k, p->oxygen);
	obs_jrand(h, k, &p->entity_rand.r); OBS(k, p->entity_rand.seeded);
	const struct inventory* inv = &p->inventory;
	OBS(k, inv->capacity); OBS(k, inv->hotbar_slot);
	for(size_t i = 0; i < inv->capacity; i++) {
		const struct item_data* it = &inv->items[i];
		OBS(k, it->id); OBS(k, it->durability); OBS(k, it->count); OBS(k, it->nbt_record);
	}
	OBS(k, inv->picked_item.id); OBS(k, inv->picked_item.durability);
	OBS(k, inv->picked_item.count); OBS(k, inv->picked_item.nbt_record);
}
static void obs_world(uint64_t* h, const struct server_world* w) {
	OBS(1, w->world_index); obs_jrand(h, 1, &w->rand);
	obs_jrand(h, 1, &w->hell_populate_rng); OBS(1, w->current_tick);
	OBS(2, w->light_seed_n); OBS(2, w->light_undo_n); OBS(2, w->light_pin_n);
	OBS(2, w->light_frontier_live); OBS(2, w->light_fault); OBS(2, w->light_fault_reason);
	for(size_t k = 0; k < w->light_seed_n; k++) {
		OBS(2, w->light_seeds[k].x); OBS(2, w->light_seeds[k].y); OBS(2, w->light_seeds[k].z);
	}
	for(size_t k = 0; k < w->light_undo_n; k++) {
		OBS(2, w->light_undo[k].x); OBS(2, w->light_undo[k].z);
		h[2] = g9io_fnv(h[2], w->light_undo[k].sky, WORLD_HEIGHT / 2);
	}
	h[2] = g9io_fnv(h[2], w->light_pins, w->light_pin_n * sizeof *w->light_pins);
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		struct server_chunk* c = &dict_server_chunks_ref(it)->value;
		OBS(3, dict_server_chunks_ref(it)->key);
		uint32_t crc = crc32(0, c->ids, 32768);
		crc = crc32(crc, c->metadata, 16384);
		crc = crc32(crc, c->lighting_sky, 16384);
		crc = crc32(crc, c->lighting_torch, 16384);
		crc = crc32(crc, c->heightmap, 256);
		OBS(3, crc); OBS(3, c->populated); OBS(3, c->sent);
		if(c->backing) {
			OBS(3, c->backing->revision.content); OBS(3, c->backing->revision.light);
			OBS(3, c->backing->revision.events); OBS(3, c->backing->settled_light_revision);
		}
	}
	OBS(5, w->world_index);
	OBS(5, w->scheduled_tick_count); OBS(5, w->scheduled_tick_drops);
	for(size_t k = 0; k < w->scheduled_tick_count; k++) {
		const struct scheduled_tick* t
			= &w->scheduled_ticks[(w->scheduled_tick_head + k) % w->scheduled_tick_capacity];
		OBS(5, t->x); OBS(5, t->y); OBS(5, t->z);
		OBS(5, t->fire_at_tick); OBS(5, t->block_type); OBS(5, t->queue_flags);
	}
}
static void obs_item(uint64_t* h, unsigned k, const struct item_data* it) {
	OBS(k, it->id); OBS(k, it->durability); OBS(k, it->count); OBS(k, it->nbt_record);
}
/* Previous tick's per-row cook state, for the reset counter (one process =
 * one arm, so file statics are per arm). */
static int s_prev_cook[MAX_FURNACES];
static uint8_t s_prev_out[MAX_FURNACES];
static void obs_tiles(uint64_t* h, const struct server_local* s) {
	const unsigned k = 7;
	uint32_t rows = 0;
	bool cooking = false;
	for(int i = 0; i < MAX_FURNACES; i++) {
		if(s->furnace_pos[i].y < 0) { s_prev_cook[i] = 0; s_prev_out[i] = 0; continue; }
		const struct furnace_te* f = &s->furnaces[i];
		rows++;
		OBS(k, i); OBS(k, s->te_owners.furnace_slot[i]);
		OBS(k, s->furnace_pos[i].x); OBS(k, s->furnace_pos[i].y); OBS(k, s->furnace_pos[i].z);
		OBS(k, f->burn_time); OBS(k, f->cook_time); OBS(k, f->current_item_burn_time);
		for(unsigned j = 0; j < 3; j++) obs_item(h, k, &f->items[j]);
		if(f->cook_time > 0) cooking = true;
		if(s_prev_cook[i] > 0 && f->cook_time == 0 && f->items[2].count == s_prev_out[i])
			s_rec->cook_resets++;
		s_prev_cook[i] = f->cook_time;
		s_prev_out[i] = f->items[2].count;
	}
	OBS(k, rows);
	if(rows > s_rec->furnace_rows_max) s_rec->furnace_rows_max = rows;
	if(cooking) s_rec->cooking_ticks++;
	const struct piston_moving_store* ps = &s->piston_moving;
	OBS(k, ps->count); OBS(k, ps->reserved); OBS(k, ps->next_order);
	bool moving = false;
	for(size_t i = 0; ps->entries && i < ps->capacity; i++) {
		const struct piston_moving_store_entry* e = &ps->entries[i];
		if(!e->used) continue;
		moving = true;
		OBS(k, i); OBS(k, e->x); OBS(k, e->y); OBS(k, e->z);
		OBS(k, e->state.stored_id); OBS(k, e->state.stored_meta);
		OBS(k, e->state.facing); OBS(k, e->state.flags);
		OBS(k, e->state.current_progress); OBS(k, e->state.last_progress);
		OBS(k, e->order); OBS(k, e->owner_slot);
	}
	if(moving) s_rec->piston_moving_ticks++;
}
/* The lowest-index live furnace row, NULL when none. */
static struct furnace_te* first_furnace(struct server_local* s) {
	for(int i = 0; i < MAX_FURNACES; i++)
		if(s->furnace_pos[i].y >= 0) return &s->furnaces[i];
	return NULL;
}
/* The lowest-index used moving-piston entry, NULL when none is in flight. */
static struct piston_moving_store_entry* first_piston(struct server_local* s) {
	struct piston_moving_store* ps = &s->piston_moving;
	for(size_t i = 0; ps->entries && i < ps->capacity; i++)
		if(ps->entries[i].used) return &ps->entries[i];
	return NULL;
}
static void observe(struct server_local* s, unsigned tick) {
	uint64_t* h = s_rec->obs[tick];
	for(unsigned k = 0; k < FIELDS; k++) h[k] = G9IO_FNV_BASIS;
	OBS(0, s->world_time); OBS(0, s->rand_src.seed);
	OBS(0, s->container_drop_rng.seed); OBS(0, s->dispenser_break_rng.seed);
	OBS(0, s->entity_rng.seed); OBS(0, s->light_queue_tick);
	struct server_local_math_random_state mr;
	memset(&mr, 0, sizeof mr);
	server_local_math_random_capture(&mr);
	OBS(0, mr.rng.seed); OBS(0, mr.initialized);
	/* Live worlds only (generation 0 = the unused slot of the single-world
	 * session); the generation number itself is process-global, not hashed. */
	for(unsigned k = 0; k < SERVER_LOCAL_WORLD_SLOTS; k++)
		if(s->worlds[k].generation) obs_world(h, &s->worlds[k]);
	dict_entity_it_t ei;
	for(dict_entity_it(ei, s->entities); !dict_entity_end_p(ei); dict_entity_next(ei)) {
		struct entity* e = dict_entity_ref(ei)->value;
		OBS(4, dict_entity_ref(ei)->key); OBS(4, e->type); OBS(4, e->pos);
		OBS(4, e->on_ground); OBS(4, e->delay_destroy);
		obs_jrand(h, 4, &e->gameplay_rng); /* not in the archive record */
		struct archived_entity a;
		memset(&a, 0, sizeof a);
		server_local_test_archive_entity(e, &a);
		/* Strings are archived through their first NUL; the unused tail is
		 * allocator residue (test_g9_save_io_equiv). */
		char* text = a.kind == ARCHIVED_ENTITY_MONSTER ? a.data.monster.wolf_owner
			: a.kind == ARCHIVED_ENTITY_PAINTING ? a.data.painting.art_name : NULL;
		if(text) {
			size_t n = a.kind == ARCHIVED_ENTITY_MONSTER
				? sizeof a.data.monster.wolf_owner : sizeof a.data.painting.art_name;
			for(size_t k = 0; k < n; k++)
				if(!text[k]) { memset(text + k, 0, n - k); break; }
		}
		h[4] = g9io_fnv(h[4], &a, sizeof a);
	}
	obs_body(h, &s->player);
	if(s->guest_actor) obs_body(h, s->guest_actor);
	obs_tiles(h, s);
}
#undef OBS

/* The lowest-id resident entity (dict order is not a stable choice). */
static struct entity* first_entity(struct server_local* s, uint32_t* count) {
	struct entity* best = NULL;
	uint32_t best_id = 0, n = 0;
	dict_entity_it_t ei;
	for(dict_entity_it(ei, s->entities); !dict_entity_end_p(ei); dict_entity_next(ei)) {
		n++;
		if(!best || dict_entity_ref(ei)->key < best_id) {
			best = dict_entity_ref(ei)->value;
			best_id = dict_entity_ref(ei)->key;
		}
	}
	*count = n;
	return best;
}

/* Two harness edits to ONE cell, in a fixed order; mutant_order swaps them. */
static void harness_edits(struct server_local* s, bool swapped) {
	struct server_world* w = &s->worlds[0];
	uint8_t hh = 0;
	assert(server_world_get_height(w, 12, 12, &hh));
	const struct block_data a = {.type = BLOCK_TORCH};
	const struct block_data b = {.type = BLOCK_GLOWSTONE};
	assert(server_world_set_block(w, 12, hh + 2, 12, swapped ? b : a));
	assert(server_world_set_block(w, 12, hh + 2, 12, swapped ? a : b));
}

static void run_stress(void) {
	struct server_local* s = &SS;
	session_open(s);
	thread_g9_lease_local(true, 63);      /* server_local_thread start */
	thread_g9_tick_boost_local(true, 63);
	s_measuring = true;
	for(unsigned t = 0; t < TICKS; t++) {
		s->player.health = MAX_PLAYER_HEALTH;
		thread_g9_tick_boost_begin();     /* gameplay tick: update + epilogue */
		server_local_test_update_once(s);
		thread_g9_tick_boost_end();
		clin_drain_discard();
		if(t == MUTATE_TICK) {
			harness_edits(s, s_arm == ARM_MUTANT_ORDER);
			struct entity* e = first_entity(s, &s_rec->entities_at_mutate);
			if(s_arm == ARM_MUTANT_DRAW) (void)rand_gen(&s->rand_src);
			if(s_arm == ARM_MUTANT_ENTITY && e) (void)jrand_next_float(&e->gameplay_rng);
			if(s_arm == ARM_MUTANT_PLAYER)
				(void)jrand_next_float(server_local_player_rand(s, &s->player));
			if(s_arm == ARM_MUTANT_FURNACE) {
				struct furnace_te* f = first_furnace(s);
				if(f) { f->cook_time++; s_rec->mutated_at = (int32_t)t; }
			}
		}
		if(s_arm == ARM_MUTANT_PISTON && t >= MUTATE_TICK && s_rec->mutated_at < 0) {
			struct piston_moving_store_entry* e = first_piston(s);
			if(e) {
				e->state.last_progress = nextafterf(e->state.last_progress, -1.0f);
				s_rec->mutated_at = (int32_t)t;
			}
		}
		if(t == MID_TICK) {
			check(server_local_test_checkpoint_live_world(s, "rel10_mid"),
				  "mid-run save-and-quit checkpoint");
			g9io_snap_take(&s_rec->snap[0], s_root);
		}
		observe(s, t);
		s_rec->ticks = t + 1;
	}
	s_measuring = false;
	check(server_local_test_checkpoint_live_world(s, "rel10_exit"), "exit checkpoint");
	g9io_snap_take(&s_rec->snap[1], s_root);
	thread_g9_tick_boost_local(false, 63); /* server_local_thread exit */
	thread_g9_lease_local(false, 63);
	session_close(s);
}

/* ---- SPLIT: real admission + real A2-T transfer, two LIVE worlds -------- */
/* One production update with the client half run strictly BETWEEN updates
 * (no concurrent consumer). The harness's tick() drains the client inbox on a
 * second thread DURING the update, so whether the client's capacity reply /
 * landing READY lands inside that same update (the server reads its inbox
 * mid-update) is a thread race: measured, the transfer COMMIT tick moved by
 * one between two identical unpaced runs. Lockstep removes that harness
 * race; the server's sends that do not fit a token wait in the client's
 * deferred ring (CLIN_RPC_PENDING_CAPACITY) until the drain below. */
static void lockstep_tick(struct server_local* s) {
	server_local_test_update_once(s);
	clin_update_drain();
}
/* owner_swap_harness's admitted_fixture with every update in lockstep (its
 * step() drains the client on a second thread mid-update; measured, that
 * race left two different Overworld region-journal (.idx) residues between
 * two identical unpaced runs). Same real admission sequence otherwise. */
static struct server_local* lockstep_admitted_fixture(void) {
	struct server_local* s = split_fixture();
	s->paused = true;
	s->split_view_radius = 4;
	unsigned before = markers;
	for(unsigned i = 0; i < 2000 && markers == before; i++)
		lockstep_tick(s);
	assert(markers == before + 1 && s->input_ack_pending);
	screen_load_world.update(&screen_load_world, 30.0f); /* acknowledge() */
	clin_update_input_readiness();
	lockstep_tick(s);
	assert(!s->input_ack_pending && !s->stream_recovery_epoch);
	assert(s->input_epoch_counter != 0);
	s->thread_started = true;
	enable_split(s);
	guest_attach(s);
	guest_body_home(s);
	struct client_session* guest_client = &gstate.players[1];
	assert(client_session_request_local_gameplay(guest_client));
	pump_handshake(s);
	assert(s->guest_gameplay && guest_client->local_gameplay_ready);
	assert(guest_client->local_player && gstate.players[0].local_player);
	assert(s->guest_actor && s->guest_actor->finished_loading);
	assert(s->player.has_pos && s->player.finished_loading);
	return s;
}
/* A lockstep run must never have hit the client token pool's exhaustion
 * wait (which, with no concurrent consumer, would end in a publication
 * fault instead of a delivery). */
static void check_no_exhaustion(const char* when) {
	uint32_t pending = 0, peak = 0, deferred = 0, exhausted = 0;
	clin_rpc_pending_stats(&pending, &peak, &deferred, &exhausted);
	char label[96];
	snprintf(label, sizeof label, "split %s: no client exhaustion wait (peak %u)",
			 when, (unsigned)peak);
	check(exhausted == 0 && !clin_rpc_publication_faulted(), label);
}
static enum server_transfer_state drive(struct server_local* s, uint32_t id) {
	bool was_paused = s->paused;
	s->paused = false;
	enum server_transfer_state st = server_transfer_state_of(s, id);
	for(unsigned i = 0; i < 4000; i++) {
		st = server_transfer_state_of(s, id);
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK) break;
		lockstep_tick(s);
	}
	s->paused = was_paused;
	return st;
}
/* Fixed light work in both worlds: a glowstone at a set cell toggles every
 * 16 ticks (Overworld beside the guest; Nether beside the host, only where
 * that cell is resident air or the harness's own glowstone). */
static void split_edits(struct server_local* s, unsigned t, const int nc[3]) {
	if(t % 16) return;
	const bool on = (t / 16) % 2 == 0;
	assert(server_world_set_block(&s->worlds[0], 4, 6, 4,
		(struct block_data) {.type = on ? BLOCK_GLOWSTONE : BLOCK_AIR}));
	struct block_data b;
	if(server_world_get_block(&s->worlds[1], nc[0], nc[1], nc[2], &b)
	   && (b.type == BLOCK_AIR || b.type == BLOCK_GLOWSTONE))
		(void)server_world_set_block(&s->worlds[1], nc[0], nc[1], nc[2],
			(struct block_data) {.type = on ? BLOCK_GLOWSTONE : BLOCK_AIR});
}
static void run_split(void) {
	/* Pinned like the STRESS fixture: LastPlayed, and generation's wall-clock
	 * budget ignored (measured: with it NORMAL, which columns were journaled
	 * during the initial stream varied run to run in the region .idx). */
	server_local_test_set_last_played(INT64_C(1791532400000));
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(server_local_test_chunk_send_batch());
	struct server_local* s = lockstep_admitted_fixture();
	string_set_str(s->level_name, split_path);
	s_root = split_path;
	uint32_t id = 0;
	assert(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
		   == SERVER_TRANSFER_OK);
	assert(drive(s, id) == SERVER_TRANSFER_COMMITTED);
	assert(server_local_world_live(&s->worlds[0]) && server_local_world_live(&s->worlds[1]));
	assert(s->worlds[1].dimension == WORLD_DIM_NETHER && s->guest_actor);
	check_no_exhaustion("setup");
	s->paused = false;
	server_local_test_seed_body_rand(s, &s->player, INT64_C(0x5EED11));
	server_local_test_seed_body_rand(s, s->guest_actor, INT64_C(0x5EED22));
	const int nc[3] = {(int)floor(s->player.x) + 1, (int)floor(s->player.y),
		(int)floor(s->player.z)};
	thread_g9_lease_local(true, 63);
	thread_g9_tick_boost_local(true, 63);
	s_measuring = true;
	for(unsigned t = 0; t < SPLIT_TICKS; t++) {
		s->player.health = MAX_PLAYER_HEALTH;
		s->guest_actor->health = MAX_PLAYER_HEALTH;
		split_edits(s, t, nc);
		thread_g9_tick_boost_begin();
		lockstep_tick(s); /* production update + epilogue, then the client */
		thread_g9_tick_boost_end();
		if(t == SPLIT_MUTATE_TICK) {
			if(s_arm == ARM_SPLIT_MUTANT_GUEST)
				(void)jrand_next_float(&s->guest_actor->entity_rand.r);
			if(s_arm == ARM_SPLIT_MUTANT_NETHER)
				(void)jrand_next_float(&s->worlds[1].rand);
		}
		observe(s, t);
		s_rec->ticks = t + 1;
	}
	s_measuring = false;
	check_no_exhaustion("run");
	check(server_local_test_checkpoint_live_world(s, "rel10_split_exit"),
		  "split exit checkpoint");
	g9io_snap_take(&s_rec->snap[1], split_path);
	thread_g9_tick_boost_local(false, 63);
	thread_g9_lease_local(false, 63);
	teardown(s);
	server_local_test_set_last_played(0);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	server_local_test_set_near_send_minimum(0);
}

/* ---- child: SERVER thread runs the session, MAIN runs paced frames ------- */
static int s_server_done;
static void* server_entry(void* unused) {
	(void)unused;
	server_world_test_history_reset();
	server_world_test_history_hook = engage_hook;
	if(arm_split(s_arm)) run_split();
	else run_stress();
	server_world_test_history_hook = NULL;
	__atomic_store_n(&s_server_done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void main_frames(void) {
	static const enum thread_pacing_phase phases[] = {THREAD_PACING_RPC,
		THREAD_PACING_LIGHTING, THREAD_PACING_BUILD, THREAD_PACING_SCREEN,
		THREAD_PACING_RENDER, THREAD_PACING_OTHER};
	unsigned k = 0;
	while(!__atomic_load_n(&s_server_done, __ATOMIC_ACQUIRE)) {
		thread_main_pacing_frame();
		/* ~16 ms of MAIN "frame work" between checkpoints, real clock. */
		for(unsigned c = 0; c < 24 && !__atomic_load_n(&s_server_done, __ATOMIC_ACQUIRE); c++) {
			ptime_t start = time_get();
			while(time_diff_us(start, time_get()) < 650) {}
			if(thread_main_pacing_checkpoint_at(phases[k++ % 6])) {
				s_rec->main_donations++;
				__atomic_add_fetch(&s_donations, 1, __ATOMIC_RELEASE);
			}
			s_rec->main_checks++;
		}
	}
}
static int run_child(enum arm arm, const char* out) {
	static struct arm_record rec;
	static char root[200];
	char id[96];
	snprintf(id, sizeof id, "test_main_tick_pacing_equiv_%s", ARM_NAME[arm]);
	test_fixture_dir(id, root, sizeof root);
	G9IO_MKDIR(root);
	s_root = root;
	s_rec = &rec;
	s_arm = arm;
	rec.mutated_at = -1;
	g9io_arm(1); /* shipping save helper */
	region_archive_test_clock(CLOCK_STAMP);
	thread_g9_tick_boost_configure(false, 50); /* shipped: boost OFF */
	if(arm == ARM_ON || arm == ARM_SPLIT_ON) thread_main_pacing_configure(true, 2000, 1, 8);
	else if(arm == ARM_ON1MS || arm == ARM_SPLIT_ON1MS) thread_main_pacing_configure(true, 1000, 1, 8);
	else if(arm == ARM_ON2) thread_main_pacing_configure(true, 500, 1, 16);
	else thread_main_pacing_configure(false, 2000, 1, 8);
	struct thread server;
	assert(thread_create(&server, server_entry, NULL, 63));
	main_frames();
	thread_join(&server);
	if(!thread_main_pacing_snapshot(rec.pacing)) memset(rec.pacing, 0, sizeof rec.pacing);
	region_archive_test_clock(0);
	test_fixture_dir_cleanup(root);
	FILE* f = fopen(out, "wb");
	bool ok = f && fwrite(&rec, sizeof rec, 1, f) == 1;
	if(f) ok = !fclose(f) && ok;
	printf("  [child %s] ticks %u main checkpoints %u donations %u | pacing sleeps %u "
		   "requested_ms %u drain-window checks %u no-drain %u | drain yields %u "
		   "engaged %u timeouts %u | entities@mutate %u | furnace rows %u cooking ticks %u "
		   "cook resets %u piston-moving ticks %u mutated@ %d\n",
		   ARM_NAME[arm], rec.ticks, rec.main_checks, rec.main_donations, rec.pacing[11],
		   rec.pacing[12], rec.pacing[5], rec.pacing[6], rec.drain_yields, rec.engaged,
		   rec.engage_timeouts, rec.entities_at_mutate, rec.furnace_rows_max,
		   rec.cooking_ticks, rec.cook_resets, rec.piston_moving_ticks, (int)rec.mutated_at);
	return ok && !failures ? 0 : 1;
}

/* ---- parent ------------------------------------------------------------- */
static const char* const FIELD_NAME[FIELDS] = {"session time+RNG", "world clocks+RNG",
	"light queues", "column bytes", "entities+gameplay_rng", "scheduled ticks",
	"bodies+Entity.rand+inventories", "furnaces+moving pistons"};
/* First (tick, field) difference; returns false when identical. */
static bool first_mismatch(const struct arm_record* a, const struct arm_record* b,
						   unsigned* tick, unsigned* field) {
	for(unsigned t = 0; t < a->ticks && t < TICKS; t++)
		for(unsigned k = 0; k < FIELDS; k++)
			if(a->obs[t][k] != b->obs[t][k]) { *tick = t; *field = k; return true; }
	return false;
}
static bool run_arm(const char* self, enum arm arm, struct arm_record* rec) {
	char path[128], cmd[512];
	snprintf(path, sizeof path, "rel10_equiv_%s_%ld.bin", ARM_NAME[arm], TEST_FIXTURE_GETPID());
#ifdef _WIN32
	snprintf(cmd, sizeof cmd, "\"\"%s\" arm %d %s\"", self, (int)arm, path);
#else
	snprintf(cmd, sizeof cmd, "\"%s\" arm %d %s", self, (int)arm, path);
#endif
	int rc = system(cmd);
	FILE* f = fopen(path, "rb");
	bool ok = rc == 0 && f && fread(rec, sizeof *rec, 1, f) == 1;
	if(f) fclose(f);
	remove(path);
	if(!ok) printf("FAIL: arm %s child rc %d / record unreadable\n", ARM_NAME[arm], rc);
	return ok && rec->ticks == (arm_split(arm) ? SPLIT_TICKS : TICKS);
}

static void compare_paced(const struct arm_record* ref, const struct arm_record* r,
						  const char* name, bool split) {
	unsigned t, k;
	if(first_mismatch(ref, r, &t, &k)) {
		printf("FAIL: pacing %s vs reference: first mismatch at tick %u field %u (%s): "
			   "%016" PRIx64 " / %016" PRIx64 "\n", name, t, k, FIELD_NAME[k],
			   ref->obs[t][k], r->obs[t][k]);
		failures++;
	} else
		printf("  pacing %s vs reference: %u ticks x %u fields identical\n",
			   name, r->ticks, FIELDS);
	for(unsigned b = split ? 1 : 0; b < BARRIERS; b++) {
		char label[80];
		snprintf(label, sizeof label, "%s save directory @ %s", name,
				 b ? "exit checkpoint" : "mid checkpoint");
		bool same = g9io_snap_equal(&ref->snap[b], &r->snap[b], label);
		printf("  %s: %zu files digest %016" PRIx64 " %s\n", label, r->snap[b].n,
			   g9io_snap_digest(&r->snap[b]), same ? "EQUAL" : "DIFFERENT");
		check(same && r->snap[b].n > 0, "save directory bytes identical");
	}
	check(r->pacing[0] == 1, "paced arm really ran with pacing enabled");
	/* Deterministic donation engagement during the compared workload. */
	check(r->engaged == ENGAGE && r->engage_timeouts == 0,
		  "paced arm: MAIN donated inside each held light-drain window");
	check(r->pacing[11] >= ENGAGE && r->pacing[5] >= ENGAGE
			  && r->main_donations == r->pacing[11],
		  "paced arm: recorded drain-window checks and sleeps match the donations");
	check(r->drain_yields == ref->drain_yields,
		  "paced arm saw exactly the reference arm's drain-window yields");
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc == 4 && !strcmp(argv[1], "arm")) {
		enum arm arm = (enum arm)atoi(argv[2]);
		if(arm < 0 || arm >= ARM_COUNT) return 2;
		if(arm_split(arm)) {
			harness_init(); /* real client half for the split admission */
		} else {
			assert(clin_init());
			assert(svin_init());
			clin_test_set_rpc_observer(session_discard, NULL);
			blocks_init();
			items_init();
		}
		return run_child(arm, argv[3]);
	}
	if(argc != 1) {
		printf("usage: %s\n", argv[0]);
		return 2;
	}
	static struct arm_record rec[ARM_COUNT];
	for(int arm = 0; arm < ARM_COUNT; arm++)
		if(!run_arm(argv[0], (enum arm)arm, &rec[arm])) {
			printf("test_main_tick_pacing_equiv: FAILED (arm %s did not complete)\n",
				   ARM_NAME[arm]);
			return 1;
		}
	compare_paced(&rec[ARM_OFF], &rec[ARM_ON], "on", false);
	compare_paced(&rec[ARM_OFF], &rec[ARM_ON2], "on2", false);
	compare_paced(&rec[ARM_OFF], &rec[ARM_ON1MS], "on1ms", false);
	compare_paced(&rec[ARM_SPLIT_OFF], &rec[ARM_SPLIT_ON1MS], "split_on1ms", true);
	compare_paced(&rec[ARM_SPLIT_OFF], &rec[ARM_SPLIT_ON], "split_on", true);
	check(rec[ARM_OFF].pacing[11] == 0 && rec[ARM_SPLIT_OFF].pacing[11] == 0
			  && rec[ARM_OFF].main_donations == 0 && rec[ARM_SPLIT_OFF].main_donations == 0,
		  "reference arms never slept");
	check(rec[ARM_OFF].drain_yields >= ENGAGE && rec[ARM_SPLIT_OFF].drain_yields >= ENGAGE,
		  "reference arms ran the same light-drain windows the paced arms donated in");
	check(rec[ARM_OFF].entities_at_mutate > 0, "an entity was resident at the mutation tick");
	/* The tile-entity capture really saw live furnace and piston state. */
	check(rec[ARM_OFF].furnace_rows_max > 0 && rec[ARM_OFF].cooking_ticks > 0,
		  "reference arm observed a live furnace with cook progress");
	check(rec[ARM_OFF].cook_resets > 0,
		  "reference arm observed cook progress reset to 0 without smelting");
	check(rec[ARM_OFF].piston_moving_ticks > 0,
		  "reference arm observed moving-piston entries in flight");
	check(rec[ARM_MUTANT_FURNACE].mutated_at == MUTATE_TICK,
		  "mutant_furnace planted its change at the mutation tick");
	check(rec[ARM_MUTANT_PISTON].mutated_at >= MUTATE_TICK,
		  "mutant_piston found a moving piston to perturb");
	static const struct { enum arm arm, ref; unsigned tick; } det[] = {
		{ARM_MUTANT_DRAW, ARM_OFF, MUTATE_TICK}, {ARM_MUTANT_ORDER, ARM_OFF, MUTATE_TICK},
		{ARM_MUTANT_ENTITY, ARM_OFF, MUTATE_TICK}, {ARM_MUTANT_PLAYER, ARM_OFF, MUTATE_TICK},
		{ARM_MUTANT_FURNACE, ARM_OFF, MUTATE_TICK}, {ARM_MUTANT_PISTON, ARM_OFF, 0},
		{ARM_SPLIT_MUTANT_GUEST, ARM_SPLIT_OFF, SPLIT_MUTATE_TICK},
		{ARM_SPLIT_MUTANT_NETHER, ARM_SPLIT_OFF, SPLIT_MUTATE_TICK}};
	for(size_t i = 0; i < sizeof det / sizeof det[0]; i++) {
		unsigned t = 0, k = 0;
		bool found = first_mismatch(&rec[det[i].ref], &rec[det[i].arm], &t, &k);
		/* mutant_piston plants at a workload-chosen tick (first in-flight entry). */
		const unsigned want = det[i].arm == ARM_MUTANT_PISTON
			? (unsigned)rec[ARM_MUTANT_PISTON].mutated_at : det[i].tick;
		if(found)
			printf("  detector %s: first mismatch at tick %u field %u (%s)\n",
				   ARM_NAME[det[i].arm], t, k, FIELD_NAME[k]);
		check(found && t == want, "detector reports the planted change at its tick");
		if(det[i].arm == ARM_MUTANT_FURNACE || det[i].arm == ARM_MUTANT_PISTON)
			check(found && k == 7, "tile-entity mutant is caught by the tile-entity field");
	}
	if(failures) {
		printf("test_main_tick_pacing_equiv: %u FAILED\n", failures);
		return 1;
	}
	printf("test_main_tick_pacing_equiv: PASS\n");
	return 0;
}
