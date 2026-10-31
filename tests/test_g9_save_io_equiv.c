/* Copyright (c) 2026 CaveWii_v2 contributors
 * G9 R19 step 1 (ASTRA-G9-R19-IO): SAVE-IO EQUIVALENCE, synchronous vs worker.
 *
 * The same deterministic save workload runs in three ARMS (support/
 * g9_save_io_rig.h): 0 = every publication inline (the synchronous reference),
 * 1 = the shipping default (round-5 helper publications), 2 = the opt-in
 * deferred IO worker (experimental.g9_save_io_int). The publication stamp is
 * pinned with region_archive_test_clock, the RNG/seed/clock inputs of the
 * world are fixed, and at EVERY COMPLETED BARRIER each arm records
 *   - the full save directory: every file, relative path + size + FNV-1a-64
 *     over ALL bytes -- .mcr headers, payloads, padding, timestamp table,
 *     .idx journals, level.dat and its _old/_new rotation, the death journal;
 *     nothing masked, no decoded-NBT substitute;
 *   - the ordered per-file mutation trace (payload write, flush, fsync,
 *     journal record, sector flip, retire, rollback, journal open/remove,
 *     close, level write/sync/rotate) with the root stripped.
 * Arms 1 and 2 must equal arm 0 at every barrier. A per-FILE trace is the
 * deterministic projection: the cross-file interleaving of arm 1's helper is
 * scheduler-dependent by design, while each file's own order is fixed.
 *
 * Cases (argv[1]):
 *   world    -- server_world level: repeated same-chunk saves inside and
 *               across batch windows, an evicting save inside a batch,
 *               multi-region eviction through the shared pool (10 regions,
 *               8 slots), a checkpoint + sync barrier, both dimensions, and
 *               world destruction (the exit barrier).
 *   session  -- server_local level: the perf_stress_5m light-queue stress
 *               driver (roof/torch edits, coherent pinned save every 600
 *               ticks -- the death-journal path), the real autosave cadence
 *               (level.dat rotation + the 1-chunk-per-tick sweep), the
 *               40-tick batch windows, the eviction loop, a death journal
 *               store/remove cycle and the exit checkpoint. */
#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"

#include "block/blocks.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "network/entity_archive.h"
#include <zlib.h>
#include "game/difficulty.h"
#include "game/game_state.h"
#include "item/inventory.h"
#include "item/items.h"
#include "lighting.h"
#include "network/client_interface.h"
#include "network/death_drop_journal.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/time.h"

#include <m-lib/m-string.h>

#define CLOCK_STAMP 0x5F5E1234u

static unsigned failures;
static void check(bool cond, const char* label) {
	if(!cond) {
		printf("FAIL: %s\n", label);
		failures++;
	}
}

/* One arm's record: a snapshot + trace per barrier. */
#define MAX_BARRIERS 16
#define SESSION_TICKS 1300
struct arm_record {
	size_t barriers;
	uint64_t observations[SESSION_TICKS][4];
	struct g9io_snap snap[MAX_BARRIERS];
	struct g9io_trace* trace[MAX_BARRIERS];
	char label[MAX_BARRIERS][48];
};
static struct arm_record* s_rec;
static const char* s_root;
static struct g9io_trace* s_trace;

static void barrier(const char* label) {
	struct arm_record* r = s_rec;
	assert(r->barriers < MAX_BARRIERS);
	size_t k = r->barriers++;
	snprintf(r->label[k], sizeof r->label[k], "%s", label);
	g9io_snap_take(&r->snap[k], s_root);
	r->trace[k] = malloc(sizeof(struct g9io_trace));
	assert(r->trace[k]);
	memcpy(r->trace[k], s_trace, sizeof(struct g9io_trace));
}

static void edit(struct server_chunk* sc, int k, uint8_t id) {
	sc->ids[(k * 37) % (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)] = id;
	sc->modified = true;
}
static struct server_chunk* load(struct server_world* w, w_coord_t x,
								 w_coord_t z) {
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
	if(sc)
		return sc;
	assert(server_world_load_chunk(w, x, z, &sc) && sc);
	return sc;
}

/* ---- case: world --------------------------------------------------------- */
static void run_world(void) {
	string_t level;
	string_init_set_str(level, s_root);
	struct server_world ow = {0};
	server_world_create(&ow, level, WORLD_DIM_OVERWORLD, 4242, true);

	/* Window 1: 10 regions (the pool holds 8 -> handles evict), one chunk
	 * saved THREE times inside the window, everything non-evicting. */
	server_world_save_batch_begin(&ow);
	for(int r = 0; r < 10; r++)
		for(int k = 0; k < 2; k++) {
			struct server_chunk* sc = load(&ow, (w_coord_t)(r * 32 + k), 0);
			edit(sc, r * 2 + k, BLOCK_BRICKS);
			check(server_world_save_chunk(&ow, false, r * 32 + k, 0),
				  "window-1 save");
		}
	for(int rep = 0; rep < 3; rep++) {
		struct server_chunk* sc = load(&ow, 1, 0);
		edit(sc, 100 + rep, BLOCK_STONE);
		check(server_world_save_chunk(&ow, false, 1, 0), "repeated save");
	}
	check(server_world_save_batch_end(&ow), "window-1 end");

	/* Window 2: the same chunk again (its previous record may still be in
	 * flight), an EVICTING save inside the batch, a chunk of a region whose
	 * handle was evicted. */
	server_world_save_batch_begin(&ow);
	{
		struct server_chunk* sc = load(&ow, 1, 0);
		edit(sc, 200, BLOCK_GLASS);
		check(server_world_save_chunk(&ow, false, 1, 0), "cross-window save");
		sc = load(&ow, 32, 0);
		edit(sc, 201, BLOCK_GLASS);
		check(server_world_save_chunk(&ow, true, 32, 0), "evicting save");
		sc = load(&ow, 0, 0);
		edit(sc, 202, BLOCK_GLASS);
		check(server_world_save_chunk(&ow, false, 0, 0), "reopened region");
	}
	check(server_world_save_batch_end(&ow), "window-2 end");
	check(server_world_checkpoint_chunks(&ow), "checkpoint");
	check(server_world_sync_regions(&ow), "sync barrier");
	barrier("overworld checkpoint+sync");

	/* Window 3 without a closing batch_end: the sync barrier must publish. */
	server_world_save_batch_begin(&ow);
	{
		struct server_chunk* sc = load(&ow, 64, 0);
		edit(sc, 300, BLOCK_BRICKS);
		check(server_world_save_chunk(&ow, false, 64, 0), "open-window save");
		/* the reload-after-evict read path: (32,0) was erased above */
		sc = load(&ow, 32, 0);
		check(sc && sc->ids[(201 * 37) % (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)]
				  == BLOCK_GLASS,
			  "an evicted chunk reloads its newest record");
	}
	check(server_world_save_batch_end(&ow), "window-3 end");
	check(server_world_sync_regions(&ow), "sync barrier 2");
	barrier("overworld second sync");

	/* The Nether: its own regions under DIM-1. */
	struct server_world nw = {0};
	server_world_create(&nw, level, WORLD_DIM_NETHER, 4242, true);
	server_world_save_batch_begin(&nw);
	for(int k = 0; k < 3; k++) {
		struct server_chunk* sc = load(&nw, k * 17, 0);
		edit(sc, 400 + k, BLOCK_NETHERRACK);
		check(server_world_save_chunk(&nw, false, k * 17, 0), "nether save");
	}
	check(server_world_save_batch_end(&nw), "nether end");
	check(server_world_checkpoint_chunks(&nw) && server_world_sync_regions(&nw),
		  "nether checkpoint+sync");
	barrier("nether checkpoint+sync");

	server_world_destroy(&nw);
	server_world_destroy(&ow);
	barrier("worlds destroyed (exit)");
	string_clear(level);
}

/* ---- case: session ------------------------------------------------------- */
/* The perf_stress_5m workload (production light-queue stress driver: roof +
 * torch edits, a furnace, a piston, a blast, and the coherent pinned save of
 * the death-journal path every 600 ticks) on a world with a real level.dat,
 * the real autosave cadence (shortened to 200 ticks: level.dat rotation + the
 * 1-chunk-per-tick sweep), the 40-tick batch windows and the eviction loop.
 * Barriers: a mid-run save-and-quit checkpoint, a death-journal PREPARING
 * store and its removal around a second checkpoint, and the exit checkpoint
 * + world destruction. */
static struct server_local SS;
static void session_open(struct server_local* s) {
	/* Use the existing RPC observer to discard the previous envelope before
	 * every send. With one consumer on this thread, headroom/residency no
	 * longer depends on wall-clock IO-worker timing. Production update,
	 * fixture, save cadence and normal simulation are unchanged. */
	clin_test_set_block_pack(false);
	memset(s, 0, sizeof *s);
	/* the production server_local_init (inventory, dicts, timers, ...) */
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
	/* Determinism: the whole near ring (MAX_VIEW_DISTANCE around the player
	 * column) is generated AND populated here, before the clock starts, so
	 * the streamer never has to generate on a tick whose budget depends on
	 * the discard consumer's timing (and so which column populates when). */
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
	s->render_demo_mob = -1;
	s->light_queue_stress = true;
	s->light_queue_defer = 1;
	s->autosave_ticks = 200;
	s->autosave_timer = 0;
	/* Determinism: no wall-clock budget may decide WHAT the world is (which
	 * columns generate or stream on a given tick), and LastPlayed is pinned. */
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(server_local_test_chunk_send_batch());
	server_local_test_set_last_played(INT64_C(1791532400000));
	/* Peaceful: hostile spawning is the one producer whose outcome depends on
	 * the discard consumer's timing (inbox headroom); the save workload --
	 * light stress, coherent pinned saves, autosave, batches -- is unchanged. */
	server_local_request_difficulty(0);
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
/* All resident columns, every plane byte, ordered pending work, live
 * entities and RNG at EACH corresponding logical update. Runtime storage
 * completion identities/save_pending are intentionally separate from world
 * observations; revisions and published flags are included. The isolated
 * fixture also compares modified; full updates can legitimately clear that
 * storage-only dirty flag at different durable-completion times. */
static void session_observe(struct server_local* s, unsigned tick, bool dirty) {
	uint64_t* h = s_rec->observations[tick];
	for(unsigned k = 0; k < 4; k++) h[k] = G9IO_FNV_BASIS;
#define OBS(k, v) h[k] = g9io_fnv(h[k], &(v), sizeof(v))
	OBS(0, s->world_time); OBS(0, s->rand_src.seed);
	OBS(0, s->light_queue_tick); OBS(0, s->player.health);
	OBS(0, s->player.x); OBS(0, s->player.y); OBS(0, s->player.z);
	struct server_world* w = &s->worlds[0];
	OBS(1, w->light_seed_n); OBS(1, w->light_undo_n); OBS(1, w->light_pin_n);
	OBS(1, w->light_frontier_live); OBS(1, w->light_fault); OBS(1, w->light_fault_reason);
	for(size_t k = 0; k < w->light_seed_n; k++) {
		OBS(1, w->light_seeds[k].x); OBS(1, w->light_seeds[k].y); OBS(1, w->light_seeds[k].z);
	}
	for(size_t k = 0; k < w->light_undo_n; k++) {
		OBS(1, w->light_undo[k].x); OBS(1, w->light_undo[k].z);
		h[1] = g9io_fnv(h[1], w->light_undo[k].sky, WORLD_HEIGHT / 2);
	}
	h[1] = g9io_fnv(h[1], w->light_pins, w->light_pin_n * sizeof *w->light_pins);
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
		struct server_chunk* c = &dict_server_chunks_ref(it)->value;
		OBS(2, dict_server_chunks_ref(it)->key);
		uint32_t crc = crc32(0, c->ids, 32768);
		crc = crc32(crc, c->metadata, 16384);
		crc = crc32(crc, c->lighting_sky, 16384);
		crc = crc32(crc, c->lighting_torch, 16384);
		crc = crc32(crc, c->heightmap, 256);
		OBS(2, crc); if(dirty) { OBS(2, c->modified); }
		OBS(2, c->populated); OBS(2, c->sent);
		if(c->backing) {
			OBS(2, c->backing->revision.content); OBS(2, c->backing->revision.light);
			OBS(2, c->backing->revision.events); OBS(2, c->backing->settled_light_revision);
		}
	}
	dict_entity_it_t ei;
	for(dict_entity_it(ei, s->entities); !dict_entity_end_p(ei); dict_entity_next(ei)) {
		struct entity* e = dict_entity_ref(ei)->value;
		OBS(3, dict_entity_ref(ei)->key); OBS(3, e->type); OBS(3, e->pos);
		OBS(3, e->on_ground); OBS(3, e->delay_destroy);
		struct archived_entity a;
		memset(&a, 0, sizeof a);
		server_local_test_archive_entity(e, &a);
		/* The archive writer encodes strings through their first NUL. Mob
		 * factories deliberately initialize owner[0], not its unused tail;
		 * hashing that malloc residue would compare allocator history. */
		char* text = a.kind == ARCHIVED_ENTITY_MONSTER ? a.data.monster.wolf_owner
			: a.kind == ARCHIVED_ENTITY_PAINTING ? a.data.painting.art_name : NULL;
		if(text) {
			size_t n = a.kind == ARCHIVED_ENTITY_MONSTER
				? sizeof a.data.monster.wolf_owner : sizeof a.data.painting.art_name;
			for(size_t k = 0; k < n; k++)
				if(!text[k]) { memset(text + k, 0, n - k); break; }
		}
		h[3] = g9io_fnv(h[3], &a, sizeof a);
	}
#undef OBS
}
static void run_session(void) {
	struct server_local* s = &SS;
	session_open(s);
	for(int t = 0; t < SESSION_TICKS; t++) {
		s->player.health = MAX_PLAYER_HEALTH;
		server_local_test_update_once(s);
		clin_drain_discard();

		if(t == 650) {
			check(server_local_test_checkpoint_live_world(s, "g9io_mid"),
				  "mid-run save-and-quit checkpoint");
			barrier("mid-run live checkpoint");
		}
		if(t == 900) {
			/* the death journal's own file, around a checkpoint */
			struct death_drop_journal* j = death_drop_journal_alloc();
			assert(j);
			memset(j, 0, sizeof *j);
			j->phase = DEATH_DROP_JOURNAL_PREPARING;
			j->transaction_id = 7;
			j->source_dimension = WORLD_DIM_NETHER;
			j->final_dimension = WORLD_DIM_OVERWORLD;
			j->final_health = MAX_PLAYER_HEALTH;
			check(death_drop_journal_store(s->level_name, j)
					  == DEATH_DROP_JOURNAL_STORE_DURABLE,
				  "death journal PREPARING store");
			check(server_local_test_checkpoint_live_world(s, "g9io_death"),
				  "checkpoint with a journal present");
			barrier("death journal + checkpoint");
			check(death_drop_journal_remove(s->level_name), "journal removal");
			free(j);
		}
		session_observe(s, (unsigned)t, false);
	}
	check(server_local_test_checkpoint_live_world(s, "g9io_exit"),
		  "exit checkpoint");
	barrier("exit checkpoint");
	session_close(s);
	barrier("world + level destroyed");
}

/* In addition to every full production update, hold the worker across N and
 * N+1 and compare every logical fixture step with the normal prescribed
 * light drain, subsequent edits/reloads and independent immutable tokens. */
static void run_stress_steps(void) {
	struct server_local* s = &SS;
	memset(s, 0, sizeof *s);
	server_local_test_create_without_thread(s, false, false, false, false, false, true, false);
	string_t level;
	string_init_set_str(level, s_root);
	server_world_create(&s->worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	s->worlds[0].local = s; s->worlds[0].session = s;
	s->scheduled_tick_budget = 128;
	s->rand_src.seed = 0x9E11u; s->world_time = 6000;
	s->player.has_pos = true; s->player.x = 8; s->player.y = 84; s->player.z = 8;
	for(int x = -2; x <= 2; x++)
		for(int z = -2; z <= 2; z++) (void)load(&s->worlds[0], x, z);
	server_local_test_light_queue_stress_demo(s); /* unchanged production fixture */
	check(server_world_light_drain(&s->worlds[0]), "fixture setup prescribed drain");
	check(server_world_checkpoint_chunks(&s->worlds[0]) && server_world_sync_regions(&s->worlds[0]), "steps durable base");
	bool deferred = region_archive_deferred_io_enabled();
	for(unsigned t = 0; t < SESSION_TICKS; t++) {
		if(t == 598) {
			server_world_save_batch_begin(&s->worlds[0]); /* checkpoint 600 open */
			if(deferred) region_archive_test_io_hold(true);
		}
		s->world_time++;
		server_local_test_light_queue_stress_drive(s);
		if(t == 599 && deferred) {
			check(s->worlds[0].checkpoint_count == 1, "first real caller token is pending");
			check(s->worlds[0].checkpoints[s->worlds[0].checkpoint_head].state == REGION_CHECKPOINT_PENDING,
				"snapshot acceptance is not DURABLE");
		}
		check(server_world_light_drain(&s->worlds[0]), "ordinary simulation drain at matching logical step");
		if(t == 700) {
			struct server_chunk* sc = load(&s->worlds[0], 2, -2);
			edit(sc, 55, BLOCK_GLOWSTONE);
			check(server_world_save_chunk(&s->worlds[0], true, 2, -2), "edit/evict while checkpoint pending");
			check(load(&s->worlds[0], 2, -2)->ids[(55 * 37) % 32768] == BLOCK_GLOWSTONE,
				"reload newest pending bytes");
			check(server_world_save_batch_end(&s->worlds[0]), "checkpoint 1200 closed batch");
		}
		if(t == 1199 && deferred) {
			check(s->worlds[0].checkpoint_count == 2, "worker stalled across next real checkpoint; both immutable snapshots owned");
			region_archive_test_io_hold(false);
		}
		session_observe(s, t, true);
	}
	check(server_local_checkpoint_drain(s), "production shutdown drain reconciles all steps tokens/errors");
	check(s->worlds[0].checkpoint_sequence == 2 && s->worlds[0].checkpoint_durable == 2,
		"both requested snapshots durably acknowledged");
	check(server_world_checkpoint_chunks(&s->worlds[0]) && server_world_sync_regions(&s->worlds[0]), "steps final save barrier");
	barrier("real caller steps final barrier");
	server_world_destroy(&s->worlds[0]);
	dict_entity_clear(s->entities);
	barrier("real caller steps destroyed");
}

/* ---- driver -------------------------------------------------------------- */
static void run_arm(int arm, void (*body)(void), struct arm_record* rec,
					const char* base) {
	static char root[200];
	char id[96];
	snprintf(id, sizeof id, "%s_%s", base, G9IO_ARM_NAME[arm]);
	test_fixture_dir(id, root, sizeof root);
	G9IO_MKDIR(root);
	s_root = root;
	s_rec = rec;
	memset(rec, 0, sizeof(*rec));
	g9io_arm(arm);
	region_archive_test_clock(CLOCK_STAMP);
	s_trace = calloc(1, sizeof(struct g9io_trace));
	assert(s_trace);
	g9io_trace_begin(s_trace, root);
	body();
	g9io_trace_end();
	region_archive_test_clock(0);
	free(s_trace);
	s_trace = NULL;
	g9io_arm(1);
	if(!getenv("G9IO_KEEP"))
		test_fixture_dir_cleanup(root);
}

static void compare(const struct arm_record* ref, const struct arm_record* x,
					int arm) {
	char label[96];
	check(ref->barriers == x->barriers, "same barrier count");
	for(unsigned t = 0; t < SESSION_TICKS; t++)
		for(unsigned k = 0; k < 4; k++)
			if(ref->observations[t][k] != x->observations[t][k]) {
				printf("  logical step %u observation %u: %016" PRIx64 " / %016" PRIx64 "\n",
					t, k, ref->observations[t][k], x->observations[t][k]);
				check(false, "unchanged simulation observations at matching logical steps");
				return;
			}
	for(size_t k = 0; k < ref->barriers && k < x->barriers; k++) {
		snprintf(label, sizeof label, "%s @ %s", G9IO_ARM_NAME[arm],
				 ref->label[k]);
		bool bytes = g9io_snap_equal(&ref->snap[k], &x->snap[k], label);
		bool trace = g9io_trace_equal(ref->trace[k], x->trace[k], label);
		printf("  barrier %zu %-28s %s: %zu files digest %016" PRIx64
			   " %s | trace %u ops %s\n",
			   k, ref->label[k], G9IO_ARM_NAME[arm], x->snap[k].n,
			   g9io_snap_digest(&x->snap[k]), bytes ? "EQUAL" : "DIFFERENT",
			   x->trace[k]->total, trace ? "EQUAL" : "DIFFERENT");
		check(bytes, "save directory bytes identical at the barrier");
		check(trace, "per-file ordered mutation trace identical");
	}
}

/* The session case runs every arm in its OWN child process: the simulation
 * keeps process-global state (RNG streams, entity runtime ids, ...) that one
 * arm would otherwise hand to the next, and two arms must start equal. The
 * child writes its arm_record to a file; the parent compares. */
static const char* s_self;
static bool record_write(const struct arm_record* r, const char* path) {
	FILE* f = fopen(path, "wb");
	if(!f)
		return false;
	bool ok = fwrite(&r->barriers, sizeof r->barriers, 1, f) == 1
		&& fwrite(r->snap, sizeof r->snap, 1, f) == 1
		&& fwrite(r->label, sizeof r->label, 1, f) == 1
		&& fwrite(r->observations, sizeof r->observations, 1, f) == 1;
	for(size_t k = 0; ok && k < r->barriers; k++)
		ok = fwrite(r->trace[k], sizeof(struct g9io_trace), 1, f) == 1;
	return !fclose(f) && ok;
}
static bool record_read(struct arm_record* r, const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	memset(r, 0, sizeof *r);
	bool ok = fread(&r->barriers, sizeof r->barriers, 1, f) == 1
		&& r->barriers <= MAX_BARRIERS
		&& fread(r->snap, sizeof r->snap, 1, f) == 1
		&& fread(r->label, sizeof r->label, 1, f) == 1
		&& fread(r->observations, sizeof r->observations, 1, f) == 1;
	for(size_t k = 0; ok && k < r->barriers; k++) {
		r->trace[k] = malloc(sizeof(struct g9io_trace));
		ok = r->trace[k]
			&& fread(r->trace[k], sizeof(struct g9io_trace), 1, f) == 1;
	}
	fclose(f);
	return ok;
}
static void record_path(char* out, size_t n, int arm) {
	snprintf(out, n, "g9io_session_arm%d_%ld.bin", arm, (long)TEST_FIXTURE_GETPID());
}
static int run_case(const char* name, void (*body)(void)) {
	struct arm_record* rec[3];
	for(int arm = 0; arm < 3; arm++) {
		rec[arm] = calloc(1, sizeof(struct arm_record));
		assert(rec[arm]);
		if(body == run_session || body == run_stress_steps) {
			char path[96], cmd[512];
			record_path(path, sizeof path, arm);
			snprintf(cmd, sizeof cmd, "\"\"%s\" %s %d %s\"", s_self,
					 body == run_session ? "session_arm" : "steps_arm", arm, path);
#ifndef _WIN32
			snprintf(cmd, sizeof cmd, "\"%s\" %s %d %s", s_self,
					 body == run_session ? "session_arm" : "steps_arm", arm, path);
#endif
			int rc = system(cmd);
			check(rc == 0, "session arm child succeeded");
			bool read = record_read(rec[arm], path);
			check(read, "session arm record read back");
			remove(path);
			if(!read)
				return 1;
		} else {
			run_arm(arm, body, rec[arm], name);
		}
	}
	uint32_t st[REGION_DEFERRED_STATS];
	region_archive_deferred_stats(st);
	printf("  deferred census: ops %u writes %u publishes %u syncs %u "
		   "backpressure %u barrier waits %u failed %u peak bytes %u peak ops "
		   "%u raw hits %u locked reads %u\n",
		   st[0], st[1], st[2], st[3], st[4], st[5], st[7], st[8], st[9],
		   st[10], st[11]);
	if(body == run_world)
		check(st[1] > 0 && st[2] > 0, "the deferred arm really deferred writes");
	compare(rec[0], rec[1], 1);
	compare(rec[0], rec[2], 2);
	for(int arm = 0; arm < 3; arm++) {
		for(size_t k = 0; k < rec[arm]->barriers; k++)
			free(rec[arm]->trace[k]);
		free(rec[arm]);
	}
	(void)name;
	return 0;
}

static void session_discard(const struct client_rpc* call, void* arg) {
	(void)call; (void)arg;
	clin_drain_discard();
}

static void* drain_entry(void* arg) {
	(void)arg;
	for(;;) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	assert(svin_init());
	const char* which = argc > 1 ? argv[1] : "world";
	if(!strcmp(which, "session_arm"))
		clin_test_set_rpc_observer(session_discard, NULL);
	else {
		struct thread drain;
		assert(thread_create(&drain, drain_entry, NULL, 64));
	}
	blocks_init();
	items_init();
	s_self = argv[0];
	if((!strcmp(which, "session_arm") || !strcmp(which, "steps_arm")) && argc > 3) {
		/* child: one arm, its record to argv[3] */
		struct arm_record* rec = calloc(1, sizeof(struct arm_record));
		assert(rec);
		int arm = atoi(argv[2]);
		run_arm(arm, !strcmp(which, "session_arm") ? run_session : run_stress_steps, rec, "test_g9_save_io_session");
		uint32_t st[REGION_DEFERRED_STATS];
		region_archive_deferred_stats(st);
		printf("  [child %s] deferred census: ops %u writes %u publishes %u "
			   "syncs %u barrier waits %u locked reads %u\n",
			   G9IO_ARM_NAME[arm], st[0], st[1], st[2], st[3], st[5], st[11]);
		bool ok = record_write(rec, argv[3]);
		return ok && !failures ? 0 : 1;
	}
	if(!strcmp(which, "world"))
		run_case("test_g9_save_io_world", run_world);
	else if(!strcmp(which, "session")) {
		run_case("test_g9_save_io_session", run_session);
		run_case("test_g9_save_io_steps", run_stress_steps);
	}
	else {
		printf("unknown case %s\n", which);
		return 2;
	}
	if(failures) {
		printf("test_g9_save_io_equiv[%s]: %u FAILED\n", which, failures);
		return 1;
	}
	printf("test_g9_save_io_equiv[%s]: PASS\n", which);
	return 0;
}
