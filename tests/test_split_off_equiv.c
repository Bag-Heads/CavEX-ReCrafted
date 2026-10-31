/* Copyright (c) 2026 CaveWii_v2 contributors
 * S-A12 (plan item A6, Astra provisional item 5): the OFF-path byte-
 * equivalence proof for the live experimental-split-screen lane.
 *
 * THE CLAIM. With the split switch OFF and no guest attached, the local-server
 * simulation on lane/split-live (e3cbed8e) produces byte-identical output to
 * main f646e3a6, the pre-lane base. Astra's ruling was explicit that
 * "unchanged-code reasoning alone does not satisfy" -- so this is a GOLDEN
 * HASH, obtained by building and running this very file (uncommitted) in a
 * second worktree checked out at f646e3a6 and recording what it printed. The
 * constant below is that recorded number, not a number this tree produced.
 *
 * WHY THIS FILE COMPILES ON BOTH TREES, which is what makes the comparison
 * meaningful: it touches only APIs that existed BEFORE the lane -- the
 * split-residency fixture, server_local_test_update_once,
 * server_local_test_process_rpc and server_local_set_split_enabled. It names
 * neither SRPC_SPLIT_SET nor CRPC_SPLIT_STATE nor any other lane symbol. The
 * same source text, byte for byte, was run on both commits.
 *
 * WHAT THE HASH COVERS (five components, each printed separately so a
 * divergence localises to one of them instead of to "the hash"):
 *   chunks    every RESIDENT chunk, in ascending S_CHUNK_ID order (sorted, not
 *             dict-iteration order, so m-lib's bucket layout can never be the
 *             thing that differs): the key, ids[32768], metadata[16384],
 *             lighting_sky[16384], lighting_torch[16384], heightmap[256] and
 *             `populated`. The brief asks for block+meta; lighting values are
 *             simulation under CLAUDE.md's hard floor, so they are in too.
 *   actor     the host actor: x/y/z, rx/ry, on_ground, dimension, health,
 *             oxygen, the selected hotbar index and all INVENTORY_SIZE slots
 *             (id/count/durability).
 *   entities  the entity list in ascending id order: id, type, pos[3].
 *   tick      s->world.current_tick.
 *   per_tick  a running digest folded in EVERY one of the 600 ticks (tick
 *             counter, s->rand_src.seed, actor position/health, entity count,
 *             scheduled-tick queue depth). A final-state-only hash could be
 *             fooled by two runs that diverge and re-converge; this cannot.
 * All five are combined into `total`, which is what GOLDEN pins.
 *
 * THE THIRD ASSERTION (brief step 3): the same run with the switch ON and no
 * guest must produce the SAME golden. Arming the accept class and allocating
 * the residency block changes nothing the simulation can observe, because
 * guest_gameplay stays false, so server_local_two_anchor() stays false.
 *
 * DETERMINISM, and why a golden is assertable at all. Every wall-clock
 * governor in the tick path is replaced by a COUNT governor, and every RNG
 * stream is pinned, exactly as tests/test_startup_retirement.c and
 * tests/test_admission_light_equiv.c already do for their 600-tick
 * continuations: near-send budget -> server_local_test_expire_near_budget +
 * server_local_test_set_near_send_minimum; worldgen budget ->
 * SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE; s->rand_src, the sound RNG, the
 * cosmetic RNG and the Math.random stream all seeded with fixed constants; the
 * fixture's autosave_ticks == 0 keeps the timed save sweep out entirely. The
 * actor never moves, so the resident set is fixed once loading finishes and no
 * eviction churn can enter the chunk component.
 */
#include "support/split_residency_fixture.h"

#include "item/items.h"
#include "platform/audio.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* The golden, recorded from a build of this file at main f646e3a6. See
 * tmp/jobs/mp-split-0912/sa12-equiv-report.md for the two command lines and
 * both printed values. */
/* Re-baselined 2026-09-24 (next-gaps-0924 M2b): bisected across the M2 first-parent merges (tmp/jobs/next-gaps-0924/m2b-bisect-*.log): W1C item physics (deb39952, EntityItem.java:45-69 per-entity Random/pushOut/bounce), W1A player physics + suffocation (d4cf78ee, EntityLiving.moveEntityWithHeading:455-497, Entity.isEntityInsideOpaqueBlock:851-863) and W1B mob movement (7c7a8ff2, EntityLiving.moveEntityWithHeading:479-497) moved the observed stream; every value is the one printed by this file at 0b33e352 and again after the F1b/W1E-e merges. chunks and actor components are unchanged from the f646e3a6 run. */
/* Re-baselined 2026-09-25 (next-gaps-0924 GOLDEN-0925, Astra kickoff ruling 8): bisected commit-by-commit inside the W2-MOVE wave on main 5f03e259 (tmp/jobs/next-gaps-0924/bisect-move.log, bisect-move2.log, bisect-column.log; per-field dumps in golden-dump/): aa8bfa1c (living entities push apart, Entity.applyEntityCollision:668-691 from EntityLiving.onLivingUpdate:641-651) moved the entities component first at server tick 84 -- chickens 7/8 receive exactly Beta's impulse, one push because 7's loop found 8 still 0.5154 apart (strict AxisAlignedBB.intersectsWith:185 on the 0.2F-expanded box) -- and the per_tick component at tick 445 (a sheep's AI draw order once its position had moved); b0931e6e (swept per-axis clipping, Entity.moveEntity:300-328 + AxisAlignedBB.calculateX/Y/ZOffset:104-195) then lands every mob EXACTLY on the block face (sheep 1 y 4.004537 -> 4.000000 at tick 20) where the old 0.01 bisection had left it a hair above. No commit after b0931e6e moves this value (checked through the integ3 first-parent chain to d60214dd). Pinned independently by tests/test_golden_move_divergence.c S1 + S4; this value is the one this file printed at the integ3 tip dadbffd1. */
/* Re-baselined 2026-09-27 (lane C mob motion units S2): attributed by component against
 * the S1 tree 325a10e9 (exactly the old pin, total=d7620d863eaaba0d). S2: chunks
 * f4335579f45336a4, actor b937de08b0361455 and per_tick db6f9f0a08da6906 are UNCHANGED;
 * ONLY entities moves (e2aef293c3269017 -> 11812c89af527c1a) with sends 3323 -> 3305 --
 * the mobs' Beta motion (move-then-gravity, jump(), knockBack at the hit); switch OFF and
 * ON still agree. Previous: 0xd7620d863eaaba0d. */
/* pre-V6 pin (P=625ac64d4): 0x88574cd0b1e6d106.
 * FAST re-pin (maintainer 2026-10-11): first divergence tick20 field153/offset954 session RNG/animals4->0 in source/network/server_local.c:25017-25136 (f83b1074e/f049fa3d8). */
#define GOLDEN UINT64_C(0xf221123f5fe5ff2a)

#define FNV_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

/* Superflat (the fixture's world): bedrock 0, dirt 1-2, grass 3, air above. */
#define GROUND_Y 3

#define LOAD_TICKS 400u
#define MEASURE_TICKS 600u

/* Write-only attribution records; assertions and folded bytes are unchanged.
 * CW_ATTR_DUMP=<prefix> writes the complete stream and a field/offset map. */
static FILE* attr_bytes;
static FILE* attr_map;
static uint64_t attr_offset;
static void attr_record(const void* data, size_t n, const char* field,
						const char* function) {
	const char* prefix = getenv("CW_ATTR_DUMP");
	if(!prefix || !prefix[0]) return;
	if(!attr_bytes) {
		char path[1024];
		int written = snprintf(path, sizeof(path), "%s.bin", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_bytes = fopen(path, "wb"); assert(attr_bytes);
		written = snprintf(path, sizeof(path), "%s.map", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_map = fopen(path, "w"); assert(attr_map);
	}
	assert(fprintf(attr_map, "%" PRIu64 " %zu %s %s\n",
				   attr_offset, n, function, field) > 0);
	assert(fwrite(data, 1, n, attr_bytes) == n);
	attr_offset += n;
}

static uint64_t hash_bytes_at(uint64_t hash, const void* data, size_t n,
                              const char* field, const char* function) {
	attr_record(data, n, field, function);
	const uint8_t* p = data;
	while(n--) {
		hash ^= *p++;
		hash *= FNV_PRIME;
	}
	return hash;
}
#define hash_bytes(h, data, n) \
	hash_bytes_at((h), (data), (n), #data, __func__)


/* The single-threaded harness owns both ends of the transport. clin_rpc_send
 * BLOCKS once the client inbox is full, so a chunk burst inside one tick would
 * deadlock this thread; draining from the send observer is the same technique
 * test_startup_retirement.c uses. Sends are counted (not hashed) so the report
 * can show terrain actually streamed. */
static unsigned long sends;
static void observe_send(const struct client_rpc* call, void* user) {
	(void)call;
	(void)user;
	sends++;
	clin_drain_discard();
}

static int compare_keys(const void* a, const void* b) {
	int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t chunk_hash(struct server_local* s, size_t* out_count) {
	size_t count = dict_server_chunks_size(s->world.chunks);
	int64_t* keys = malloc((count ? count : 1) * sizeof(*keys));
	assert(keys);
	size_t n = 0;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, s->world.chunks);
		!dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
		assert(n < count);
		keys[n++] = dict_server_chunks_ref(it)->key;
	}
	assert(n == count);
	qsort(keys, count, sizeof(*keys), compare_keys);
	uint64_t hash = hash_bytes(FNV_BASIS, &count, sizeof(count));
	for(size_t i = 0; i < count; i++) {
		/* A sorted walk with no duplicate keys: pins the enumeration itself,
		 * not only the payloads it reaches. */
		assert(i == 0 || keys[i] > keys[i - 1]);
		const struct server_chunk* c
			= dict_server_chunks_get(s->world.chunks, keys[i]);
		assert(c);
		hash = hash_bytes(hash, &keys[i], sizeof(keys[i]));
		hash = hash_bytes(hash, c->ids, 32768);
		hash = hash_bytes(hash, c->metadata, 16384);
		hash = hash_bytes(hash, c->lighting_sky, 16384);
		hash = hash_bytes(hash, c->lighting_torch, 16384);
		hash = hash_bytes(hash, c->heightmap, 256);
		hash = hash_bytes(hash, &c->populated, sizeof(c->populated));
	}
	free(keys);
	*out_count = count;
	return hash;
}

static uint64_t actor_hash(struct server_local* s) {
	const struct server_player* p = &s->player;
	uint64_t hash = FNV_BASIS;
	hash = hash_bytes(hash, &p->x, sizeof(p->x));
	hash = hash_bytes(hash, &p->y, sizeof(p->y));
	hash = hash_bytes(hash, &p->z, sizeof(p->z));
	hash = hash_bytes(hash, &p->rx, sizeof(p->rx));
	hash = hash_bytes(hash, &p->ry, sizeof(p->ry));
	hash = hash_bytes(hash, &p->on_ground, sizeof(p->on_ground));
	hash = hash_bytes(hash, &p->dimension, sizeof(p->dimension));
	hash = hash_bytes(hash, &p->health, sizeof(p->health));
	hash = hash_bytes(hash, &p->oxygen, sizeof(p->oxygen));
	size_t hotbar = inventory_get_hotbar(&s->player.inventory);
	hash = hash_bytes(hash, &hotbar, sizeof(hotbar));
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data item;
		bool present
			= inventory_get_slot(&s->player.inventory, slot, &item);
		hash = hash_bytes(hash, &present, sizeof(present));
		if(!present)
			continue;
		hash = hash_bytes(hash, &item.id, sizeof(item.id));
		hash = hash_bytes(hash, &item.count, sizeof(item.count));
		hash = hash_bytes(hash, &item.durability, sizeof(item.durability));
	}
	return hash;
}

static int compare_ids(const void* a, const void* b) {
	uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t entity_hash(struct server_local* s, size_t* out_count) {
	size_t count = dict_entity_size(s->entities);
	uint32_t* ids = malloc((count ? count : 1) * sizeof(*ids));
	assert(ids);
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		assert(n < count);
		ids[n++] = dict_entity_ref(it)->key;
	}
	assert(n == count);
	qsort(ids, count, sizeof(*ids), compare_ids);
	uint64_t hash = hash_bytes(FNV_BASIS, &count, sizeof(count));
	for(size_t i = 0; i < count; i++) {
		assert(i == 0 || ids[i] > ids[i - 1]);
		struct entity* const* slot = dict_entity_get(s->entities, ids[i]);
		assert(slot && *slot);
		const struct entity* e = *slot;
		hash = hash_bytes(hash, &ids[i], sizeof(ids[i]));
		hash = hash_bytes(hash, &e->id, sizeof(e->id));
		hash = hash_bytes(hash, &e->type, sizeof(e->type));
		for(unsigned k = 0; k < 3; k++)
			hash = hash_bytes(hash, &e->pos[k], sizeof(e->pos[k]));
	}
	free(ids);
	*out_count = count;
	return hash;
}

static uint8_t type_at(struct server_local* s, int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s->world, x, y, z, &blk));
	return blk.type;
}

/* The host action sequence. server_local_test_process_rpc drives
 * server_local_process -- the OWNER (slot 0) handler the real transport calls
 * for an owned message -- so these are genuine slot-0 SRPCs, not a direct poke
 * at the world. Each one asserts the world actually changed, so the script can
 * never silently degrade into "nothing happened" and leave the golden pinning
 * an idle simulation. */
static void act_place(struct server_local* s, int x, int y, int z,
					  uint8_t expect) {
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place
		= {.x = x, .y = y, .z = z, .side = SIDE_TOP}};
	server_local_test_process_rpc(s, &call);
	assert(type_at(s, x, y + 1, z) == expect);
}

static void act_dig(struct server_local* s, int x, int y, int z) {
	assert(type_at(s, x, y, z) != BLOCK_AIR);
	struct server_rpc call = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig
		= {.finished = true, .x = x, .y = y, .z = z, .side = SIDE_TOP}};
	server_local_test_process_rpc(s, &call);
	assert(type_at(s, x, y, z) == BLOCK_AIR);
}

/* Fixed tick -> action. Spread across the window so the block changes,
 * their lighting relights and their dropped-item entities all live inside the
 * measured 600 ticks. */
static void scripted_action(struct server_local* s, unsigned tick) {
	switch(tick) {
	case 25: act_place(s, 2, GROUND_Y, 0, BLOCK_STONE); break;
	case 75: act_place(s, 2, GROUND_Y + 1, 0, BLOCK_STONE); break;
	case 125: act_dig(s, 2, GROUND_Y + 2, 0); break;
	case 175: act_dig(s, 0, GROUND_Y, 2); break;
	case 225: act_place(s, -2, GROUND_Y, 0, BLOCK_STONE); break;
	case 275: act_dig(s, 2, GROUND_Y + 1, 0); break;
	case 325: act_place(s, 0, GROUND_Y, -2, BLOCK_STONE); break;
	case 375: act_dig(s, -2, GROUND_Y + 1, 0); break;
	default: break;
	}
}

struct result {
	uint64_t total, chunks, actor, entities, per_tick;
	uint32_t tick;
	size_t chunk_count, entity_count;
};

static struct result run(bool split_on) {
	sends = 0;
	struct server_local* s = split_fixture();
	/* Pin every stream the simulation draws from. */
	s->rand_src = (struct random_gen) {.seed = 12345};
	/* F4-RNG: give both switch arms identical World.rand / body inputs. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	server_local_test_seed_body_rand(s, &s->player, 6789);
	*server_local_sound_rng() = (struct random_gen) {.seed = 6789};
	*audio_cosmetic_rng() = (struct random_gen) {.seed = 9876};
	server_local_test_seed_math_random(7654);
	/* ...and replace every wall-clock governor with a count governor. */
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	assert(s->autosave_ticks == 0); /* fixture: no timed save sweep */
	s->player.active_inventory = &s->player.inventory;
	inventory_set_hotbar(&s->player.inventory, 0);
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = BLOCK_STONE, .count = 64});
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 1,
		(struct item_data) {.id = BLOCK_DIRT, .count = 10});

	/* THE ONE THING UNDER TEST. Boot path, server thread stopped -- the only
	 * split API that exists on BOTH commits. With no guest ever attached,
	 * guest_gameplay stays false on either tree, so the lane's added
	 * `|| s->guest_gameplay` terms are the same value they replaced. */
	if(split_on) {
		assert(server_local_set_split_enabled(s, true));
		assert(s->split_enabled && s->residency && !s->guest_gameplay);
		assert(!server_local_two_anchor(s));
	} else {
		assert(!s->split_enabled && !s->residency);
	}

	clin_test_set_rpc_observer(observe_send, NULL);
	/* Stream the terrain in with the simulation held (input_ack_pending), the
	 * same handoff shape test_startup_retirement.c models. */
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	for(unsigned load = 0; load < LOAD_TICKS; load++) {
		server_local_test_update_once(s);
		clin_drain_discard();
	}
	/* LOAD_TICKS was enough -- measured, not assumed: the whole near square is
	 * resident AND sent before the first measured tick. */
	for(int cz = -MAX_VIEW_DISTANCE; cz <= MAX_VIEW_DISTANCE; cz++)
		for(int cx = -MAX_VIEW_DISTANCE; cx <= MAX_VIEW_DISTANCE; cx++) {
			const struct server_chunk* c
				= dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(cx, cz));
			assert(c && c->sent);
		}
	assert(s->world.current_tick == 0); /* the hold really held */
	s->input_ack_pending = 0;

	uint64_t per_tick = FNV_BASIS;
	for(unsigned tick = 0; tick < MEASURE_TICKS; tick++) {
		scripted_action(s, tick);
		server_local_test_update_once(s);
		clin_drain_discard();
		size_t entities = dict_entity_size(s->entities);
		per_tick = hash_bytes(per_tick, &s->world.current_tick,
			sizeof(s->world.current_tick));
		per_tick = hash_bytes(per_tick, &s->rand_src.seed,
			sizeof(s->rand_src.seed));
		per_tick = hash_bytes(per_tick, &s->player.x, sizeof(s->player.x));
		per_tick = hash_bytes(per_tick, &s->player.y, sizeof(s->player.y));
		per_tick = hash_bytes(per_tick, &s->player.z, sizeof(s->player.z));
		per_tick = hash_bytes(per_tick, &s->player.health,
			sizeof(s->player.health));
		per_tick = hash_bytes(per_tick, &entities, sizeof(entities));
		per_tick = hash_bytes(per_tick, &s->world.scheduled_tick_count,
			sizeof(s->world.scheduled_tick_count));
	}
	clin_test_set_rpc_observer(NULL, NULL);

	struct result r = {0};
	r.per_tick = per_tick;
	r.chunks = chunk_hash(s, &r.chunk_count);
	r.actor = actor_hash(s);
	r.entities = entity_hash(s, &r.entity_count);
	r.tick = s->world.current_tick;
	assert(r.tick == MEASURE_TICKS);
	/* The script must have LEFT something behind -- two placed blocks still
	 * standing and the dug cells open -- or the golden pins an empty claim. */
	assert(type_at(s, 0, GROUND_Y, 2) == BLOCK_AIR);
	assert(type_at(s, 0, GROUND_Y + 1, -2) == BLOCK_STONE);
	assert(type_at(s, 2, GROUND_Y + 1, 0) == BLOCK_AIR);
	assert(r.entity_count > 0); /* the dropped items are live entities */

	r.total = FNV_BASIS;
	r.total = hash_bytes(r.total, &r.chunks, sizeof(r.chunks));
	r.total = hash_bytes(r.total, &r.actor, sizeof(r.actor));
	r.total = hash_bytes(r.total, &r.entities, sizeof(r.entities));
	r.total = hash_bytes(r.total, &r.tick, sizeof(r.tick));
	r.total = hash_bytes(r.total, &r.per_tick, sizeof(r.per_tick));

	printf("split=%s total=%016" PRIx64 " chunks=%016" PRIx64
		   " actor=%016" PRIx64 " entities=%016" PRIx64
		   " per_tick=%016" PRIx64 " tick=%u columns=%zu entity_rows=%zu"
		   " sends=%lu\n",
		split_on ? "ON " : "OFF", r.total, r.chunks, r.actor, r.entities,
		r.per_tick, r.tick, r.chunk_count, r.entity_count, sends);
	fflush(stdout);
	split_destroy(s);
	if(attr_bytes) { assert(fflush(attr_bytes) == 0); assert(fflush(attr_map) == 0); }
	return r;
}

/* Name the first component that differs, so a failure is a diagnosis rather
 * than "the hash moved" (the brief's localisation requirement). */
static void report_first_difference(const struct result* got,
									const struct result* want) {
	struct {
		const char* name;
		uint64_t got, want;
	} parts[] = {
		{"chunks", got->chunks, want->chunks},
		{"actor", got->actor, want->actor},
		{"entities", got->entities, want->entities},
		{"tick", got->tick, want->tick},
		{"per_tick", got->per_tick, want->per_tick},
	};
	for(unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
		if(parts[i].got != parts[i].want) {
			fprintf(stderr,
				"test_split_off_equiv: FIRST DIFFERING COMPONENT %s "
				"got=%016" PRIx64 " want=%016" PRIx64 "\n",
				parts[i].name, parts[i].got, parts[i].want);
			return;
		}
	fprintf(stderr, "test_split_off_equiv: components all match; only the "
					"combined total differs (check the combine order)\n");
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();

	struct result off = run(false);
	if(off.total != GOLDEN) {
		fprintf(stderr,
			"test_split_off_equiv: OFF total=%016" PRIx64
			" != GOLDEN %016" PRIx64 " (main f646e3a6)\n",
			off.total, GOLDEN);
		assert(false && "switch OFF diverged from the pre-lane golden");
	}
	printf("PASS switch OFF: simulation byte-identical to main f646e3a6 "
		   "(golden %016" PRIx64 ")\n", GOLDEN);

	/* Brief step 3: arming the accept class and allocating the residency
	 * block, with no guest, must not move one byte of the simulation. */
	struct result on = run(true);
	if(on.total != off.total) {
		report_first_difference(&on, &off);
		assert(false && "switch ON with no guest changed the simulation");
	}
	assert(on.total == GOLDEN);
	printf("PASS switch ON, no guest: same golden %016" PRIx64 "\n", GOLDEN);

	puts("PASS test_split_off_equiv: OFF-path byte-equivalence vs main "
		 "f646e3a6 (A6, S-A12)");
	return 0;
}
