/* Copyright (c) 2026 CaveWii_v2 contributors
 * A1a (split-dimensions-plan-0924): world identity, explicit-world mutation
 * APIs and the actor->world accessor must leave single-player simulation
 * BYTE-IDENTICAL, and a mutation of one server_world must never touch another
 * world at the same coordinates.
 *
 * PART 1 -- SINGLE-WORLD EQUIVALENCE (golden). A 600-tick host session on the
 * split-residency superflat fixture, driven only through genuine slot-0 SRPCs
 * (bucket water + lava that flow and meet, a sand block that falls as an
 * entity, TNT primed by a redstone torch that blows a crater and scatters
 * drops, a lit torch feeding a redstone wire line, digs). Every tick folds a
 * trace of the tick counter, the RNG streams, a probe set of blocks around
 * each mechanism, the scheduled-tick queue depth and the full entity table;
 * the end state folds every resident chunk (ids, metadata, both light
 * arrays, heightmap, populated), the host actor and the entity table.
 *
 * The GOLDEN below was recorded by building THIS FILE, with
 * CW_WORLD_CONTEXT_BASE_CAPTURE defined, in the worktree split-dims-a1a-0925
 * at main 7f1bd8bf + the A0 carry d82a1684 (which only adds
 * server_local_world_of and guest-only refusal paths), before any A1a source
 * edit (tmp/jobs/split-dimensions-plan-0924/a1a-takeover/golden-base.txt).
 * PART 1 touches the world only through server_local_world_of(s, &s->player),
 * the A0 accessor that exists on both trees with the same signature, plus the
 * shared split fixture -- so the same source text is measured on both sides.
 *
 * PART 2 -- SAME-COORDINATE ISOLATION (A1a API, not on the base). Two
 * server_world instances in one server_local (worlds[0] and worlds[1]) are
 * populated at the SAME chunk; set_block, the silent/metadata writes and an
 * explosion in one leave the other's chunk bytes, scheduled-tick queue,
 * explosion counters and client stream untouched, in both directions.
 *
 * PART 3 -- STALE GENERATION (A1a API). A world handle taken from a live world
 * resolves only while that exact world instance is live: after discard, and
 * after a re-create in the same slot, the old handle resolves to NULL.
 */
#include "support/split_residency_fixture.h"
#include <math.h> /* bucket_use_top aim */

#include "item/items.h"
#include "network/server_furnace.h"
#include "platform/audio.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* Recorded from the unmodified base 7f1bd8bf + A0 (d82a1684); see the header comment. */
/* INTEG3 re-pin (2026-09-25, after the A1a review M1+M2 extension: client
 * sends folded in; piston, furnace, sapling+bone meal, creeper try_explode_by
 * with a pig victim, chain TNT, fireball explode_flaming with a pig AND the
 * host in range). Base = the waves tree 88f8fb40 (7f1bd8bf + waves 2/3, no
 * A0/A1a), built with CW_WORLD_CONTEXT_BASE_CAPTURE + WCE_PRE_A1A_TREE and the
 * harness shim. Portable and host-speed-independent since the fixed-width
 * sends fold and the lifted wall-clock far budget: waves-only Windows (two
 * processes), integ3 Windows (60c57a41), integ3 Linux (WSL gcc/glibc) and
 * integ3 Linux under ASan+UBSan all give total=518a97fc7f32ad65 chunks=8efba3bbf1d23d11
 * actor=4b2117456b5fb601 entities=fb995271e7f66fc6 per_tick=5048ec4ec79407bc
 * sends_hash=8a1e1fcead7629cb (sends=4411) -- A1a byte-neutral on the waves
 * tree, same value on both platforms. History: 0x91ffe57a50ef1bdd (7f1bd8bf+A0,
 * old scenario), 0x37ab7f223f32c5f7 (waves tree, old scenario),
 * 0x720fa5abaceb6110 (M2 scenario, Windows only: the old fold hashed an
 * `unsigned long`, 4 bytes on LLP64 Windows vs 8 on LP64 Linux). */
/* Re-baselined 2026-09-27 (lane C mob motion units S2, docs/intent/mob-motion-units.md;
 * Astra rulings 2026-09-27). Attributed by component against the S1 tree 325a10e9, which
 * prints exactly the old pin (total=518a97fc7f32ad65 chunks=8efba3bbf1d23d11
 * actor=4b2117456b5fb601 entities=fb995271e7f66fc6 per_tick=5048ec4ec79407bc
 * sends_hash=8a1e1fcead7629cb sends=4411). S2 prints total=376c9ce9a1974846
 * chunks=92cbee63cea4e002 actor=4b2117456b5fb601 (UNCHANGED -- the host player path is
 * untouched) entities=2833f831fd433785 per_tick=0c11831e077c6a27 sends_hash=e6ae394f31a98248
 * sends=4222. The moved components are the mob-driven ones: mob positions and motion (Beta
 * move-then-gravity, jump() (double)0.42F, knockBack at the hit), and through them the
 * blocks the scenario's creeper try_explode_by / chain-TNT / fireball blasts carve (the
 * blast centres and the pig victims sit on mob positions), plus the per-tick mob move sends. */
/* Re-baselined 2026-09-27 (lane C death-motion-0927: a dying mob keeps Beta's
 * moveEntityWithHeading(0, 0) through its 20-tick deathTime flop). Attributed by
 * component against the S2 pin 376c9ce9a1974846: chunks=92cbee63cea4e002,
 * actor=4b2117456b5fb601, entities=2833f831fd433785 (the FINAL entity set),
 * sends_hash=e6ae394f31a98248 and sends=4222 are all UNCHANGED; ONLY per_tick moves
 * (0c11831e077c6a27 -> b60341369bd4661b). per_tick folds the per-tick entity hash,
 * so it sees the scenario's blast-killed mobs falling/sliding during their flop
 * instead of hanging where they died; they are reaped before the final hash, and
 * neither the RNG streams nor any block/send changed (disabling the corpse's splash
 * edge or landing thud leaves per_tick at b60341369bd4661b). Previous:
 * 0x376c9ce9a1974846. */
/* pre-V6 pin (P=3cf20cf01): 0xbc8816830bb241bc.
 * FAST re-pin (maintainer 2026-10-11): first divergence script tick35 session RNG58335854->1ec354a2 in source/network/server_weather.c:812 (f83b1074e/f049fa3d8). */
/* L3 FAST re-pin: P=11d73857e and R8=c2f7c0ce1 reproduce
 * 0x149cb1826d6b378c. WIP 356da47fe's fluid_block.c alone reproduces
 * HEAD's complete golden; first divergence is script tick34's session RNG
 * (516117666 -> 1119331928). CFR BlockFluid.java:217-222 assigns fizz's
 * two float draws to World.rand. HEAD8/8 complete captures are identical;
 * D:/cavewii-evidence/l3/world-{P-0,R8,fizz-only,HEAD-0}.{bin,map,log}. */
/* G9X-6 re-record (REGRESS-1015, maintainer re-record ruling 2026-10-14): old pin 0x88ad13b935a6bbf6 (chunks/actor/entities/per_tick unchanged; sends 4540 -> 4601). With the G9X-6 post-drain refresh disabled the old pin reproduces exactly; with it the fold stream differs ONLY by 61 inserted CRPC_CHUNK republishes of already-sent columns, each carrying a stored Sky/Block light change (Beta publishes every light store, CFR World.java:512-517). tmp/jobs/loop/REGRESS-1015/RERECORD.md. */
#define GOLDEN UINT64_C(0x1c9801d25d730485)

#define FNV_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

/* Superflat: bedrock 0, dirt 1-2, grass 3, air above. */
#define GROUND_Y 3

/* The stone pillar + sand sit 14 blocks (Manhattan) from the water source:
 * Beta water spreads 7 cells over flat ground, and sand falling into a cell
 * the flow reached first never re-forms a block (observed on 7f1bd8bf), so
 * the two mechanisms are kept apart to stay independently observable. */
#define PILLAR_X (-4)
#define SAND_X (PILLAR_X + 1)
#define LOAD_TICKS 400u
#define MEASURE_TICKS 600u

/* Attribution dump (CW_WCE_DUMP=<prefix>): every byte folded into the golden
 * during a run_session is appended, in fold order, to <prefix>.<session>, so
 * two builds can be compared on the complete canonical stream, not just the
 * terminal hash. Write-only: never feeds back into any folded value. */
static FILE* wce_dump;
static unsigned wce_dump_session;
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
	if(wce_dump)
		fwrite(data, 1, n, wce_dump);
	while(n--) {
		hash ^= *p++;
		hash *= FNV_PRIME;
	}
	return hash;
}
#define hash_bytes(h, data, n) \
	hash_bytes_at((h), (data), (n), #data, __func__)


/* M1 (A1a review): every client publication is folded into the golden -- the
 * count AND the ordered sequence of RPC types (payload unions carry padding,
 * so only the type tag is hashed), because every clin_rpc_send now routes
 * through the world publication gate. */
static unsigned long sends;
static bool send_trace;
static int send_trace_tick = -1; /* -1 = load phase */
static uint64_t send_hash;
static void observe_send(const struct client_rpc* call, void* user) {
	(void)user;
	sends++;
	uint32_t type = (uint32_t)call->type;
	send_hash = hash_bytes(send_hash, &type, sizeof(type));
	/* Divergence triage (CW_WCE_TRACE=1): one line per publication. */
	if(send_trace)
		printf("S %d %lu %u\n", send_trace_tick, sends, (unsigned)type);
	clin_drain_discard();
}

/* WCE_PRE_A1A_TREE: define it (with CW_WORLD_CONTEXT_BASE_CAPTURE) to build
 * PART 1 on a tree WITHOUT A0/A1a -- e.g. the waves tree 88f8fb40 -- for the
 * byte-neutrality capture. Only these two shims differ; the scenario text is
 * identical on both sides. */
#ifdef WCE_PRE_A1A_TREE
/* The capture harness supplies tests/wce_pre_a1a_shim.h (NOT in this tree, so
 * the alias gate never sees it): wce_world_of(s, actor) = the A0
 * server_local_world_of host branch -- the single pre-A1a world once its
 * chunks are ready, NULL for any other actor. See INTEG3-result.md. */
#include "wce_pre_a1a_shim.h"
#define WCE_SET_BLOCK(s, x, y, z, blk) server_world_set_block((s), (x), (y), (z), (blk))
#else
#define wce_world_of server_local_world_of
#define WCE_SET_BLOCK(s, x, y, z, blk) \
	server_world_set_block(host_world(s), (x), (y), (z), (blk))
#endif

static struct server_world* host_world(struct server_local* s) {
	struct server_world* w = wce_world_of(s, &s->player);
	assert(w);
	return w;
}

static int compare_keys(const void* a, const void* b) {
	int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t chunk_hash_world(struct server_world* w, size_t* out_count) {
	size_t count = dict_server_chunks_size(w->chunks);
	int64_t* keys = malloc((count ? count : 1) * sizeof(*keys));
	assert(keys);
	size_t n = 0;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		assert(n < count);
		keys[n++] = dict_server_chunks_ref(it)->key;
	}
	assert(n == count);
	qsort(keys, count, sizeof(*keys), compare_keys);
	uint64_t hash = hash_bytes(FNV_BASIS, &count, sizeof(count));
	for(size_t i = 0; i < count; i++) {
		assert(i == 0 || keys[i] > keys[i - 1]);
		const struct server_chunk* c = dict_server_chunks_get(w->chunks, keys[i]);
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
	if(out_count)
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
		bool present = inventory_get_slot(&s->player.inventory, slot, &item);
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
		struct entity* const* slot = dict_entity_get(s->entities, ids[i]);
		assert(slot && *slot);
		const struct entity* e = *slot;
		hash = hash_bytes(hash, &ids[i], sizeof(ids[i]));
		hash = hash_bytes(hash, &e->type, sizeof(e->type));
		for(unsigned k = 0; k < 3; k++) {
			hash = hash_bytes(hash, &e->pos[k], sizeof(e->pos[k]));
			hash = hash_bytes(hash, &e->vel[k], sizeof(e->vel[k]));
		}
		hash = hash_bytes(hash, &e->on_ground, sizeof(e->on_ground));
		/* Only the kinds that INITIALISE health (EntityItem.health = 5, the
		 * mob tables) fold it: entities come from malloc and
		 * entity_default_init never touches `health`, so a falling-sand or
		 * primed-TNT entity carries heap garbage there (observed 3 vs 5
		 * across two sessions of one process on 7f1bd8bf) -- never read by
		 * the simulation for those kinds, but not a value to pin. */
		if(e->type == ENTITY_ITEM || e->type == ENTITY_MONSTER)
			hash = hash_bytes(hash, &e->health, sizeof(e->health));
	}
	free(ids);
	if(out_count)
		*out_count = count;
	return hash;
}

static bool get_at(struct server_local* s, int x, int y, int z,
				   struct block_data* out) {
	return server_world_get_block(host_world(s), x, y, z, out);
}

static uint8_t type_at(struct server_local* s, int x, int y, int z) {
	struct block_data blk;
	assert(get_at(s, x, y, z, &blk));
	return blk.type;
}

/* Put `id` in hotbar slot 0 and select it: the held item for the next SRPC. */
static void hold(struct server_local* s, int id, int count) {
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = id, .count = count});
	inventory_set_hotbar(&s->player.inventory, 0);
}

static void use_on(struct server_local* s, int x, int y, int z, enum side side) {
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side}};
	server_local_test_process_rpc(s, &call);
}

/* A bucket click the way a Beta player makes it: ItemBucket.onItemRightClick
 * ignores the clicked block/face and runs its OWN 5.0-block ray from the eye
 * [ItemBucket.java:12-28], so the click must come from a pose that can reach
 * and sees the target face. The script's host stands near (0, 0), out of
 * reach of these cells, so for the one click the body stands on the ground one
 * block west of the cell and looks at the top-face centre; the pose is
 * restored at once, so nothing else in the golden sees it. */
static void bucket_use_top(struct server_local* s, int x, int y, int z) {
	double px = s->player.x, py = s->player.y, pz = s->player.z;
	float prx = s->player.rx, pry = s->player.ry;
	s->player.x = x - 0.5;
	s->player.y = y + 1 + 1.62;
	s->player.z = z + 0.5;
	double dx = (x + 0.5) - s->player.x, dy = (y + 1.0) - s->player.y,
		   dz = (z + 0.5) - s->player.z;
	/* Beta look vector: (-sin yaw cos pitch, -sin pitch, cos yaw cos pitch). */
	s->player.rx = (float)(atan2(-dx, dz) * 180.0 / 3.141592653589793);
	s->player.ry = (float)(atan2(-dy, sqrt(dx * dx + dz * dz)) * 180.0
		/ 3.141592653589793);
	use_on(s, x, y, z, SIDE_TOP);
	s->player.x = px; s->player.y = py; s->player.z = pz;
	s->player.rx = prx; s->player.ry = pry;
}

static void dig(struct server_local* s, int x, int y, int z) {
	assert(type_at(s, x, y, z) != BLOCK_AIR);
	struct server_rpc call = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig
		= {.finished = true, .x = x, .y = y, .z = z, .side = SIDE_TOP}};
	server_local_test_process_rpc(s, &call);
	assert(type_at(s, x, y, z) == BLOCK_AIR);
}

static void hold_meta(struct server_local* s, int id, int count, int durability) {
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = id, .count = count, .durability = durability});
	inventory_set_hotbar(&s->player.inventory, 0);
}

static struct entity* find_entity(struct server_local* s, uint32_t id) {
	struct entity* const* slot = dict_entity_get(s->entities, id);
	return slot ? *slot : NULL;
}

/* M2 scenario anchors (A1a review): piston, furnace, tree, creeper + victim,
 * chain TNT, fireball + victims. Each sits clear of the reach of the others. */
#define PISTON_X 12
#define PISTON_Z 12
#define FURNACE_X 10
#define FURNACE_Z (-8)
#define TREE_X (-6)
#define TREE_Z 12
#define CREEPER_X 23
#define CREEPER_Z (-10)
#define CHAIN_TNT_Z 2
static uint32_t creeper_victim_id, fireball_victim_id;
static int creeper_victim_hp;

static bool is_water(uint8_t t) {
	return t == BLOCK_WATER_FLOW || t == BLOCK_WATER_STILL;
}
static bool is_lava(uint8_t t) {
	return t == BLOCK_LAVA_FLOW || t == BLOCK_LAVA_STILL;
}

/* Fixed tick -> action; each action asserts it really changed the world so the
 * golden can never silently pin an idle simulation. */
static void scripted_action(struct server_local* s, unsigned tick) {
	switch(tick) {
	case 10: /* water source on the ground, 6 east */
		hold(s, ITEM_BUCKET_WATER, 1);
		bucket_use_top(s, 6, GROUND_Y, 0);
		assert(is_water(type_at(s, 6, GROUND_Y + 1, 0)));
		break;
	case 20: /* lava source 6 blocks south of it: the flows meet */
		hold(s, ITEM_BUCKET_LAVA, 1);
		bucket_use_top(s, 6, GROUND_Y, 6);
		assert(is_lava(type_at(s, 6, GROUND_Y + 1, 6)));
		break;
	case 30: /* a two-high stone pillar ... */
		hold(s, BLOCK_STONE, 64);
		use_on(s, PILLAR_X, GROUND_Y, -4, SIDE_TOP);
		assert(type_at(s, PILLAR_X, GROUND_Y + 1, -4) == BLOCK_STONE);
		break;
	case 35:
		use_on(s, PILLAR_X, GROUND_Y + 1, -4, SIDE_TOP);
		assert(type_at(s, PILLAR_X, GROUND_Y + 2, -4) == BLOCK_STONE);
		break;
	case 40: /* ... with sand stuck to its side over air: it falls */
		hold(s, BLOCK_SAND, 8);
		use_on(s, PILLAR_X, GROUND_Y + 2, -4, SIDE_RIGHT);
		assert(type_at(s, SAND_X, GROUND_Y + 2, -4) == BLOCK_SAND);
		break;
	case 50: /* TNT far enough away that the blast cannot reach the host */
		hold(s, BLOCK_TNT, 4);
		use_on(s, -10, GROUND_Y, 0, SIDE_TOP);
		assert(type_at(s, -10, GROUND_Y + 1, 0) == BLOCK_TNT);
		break;
	case 52: /* M2 chain TNT: unpowered, 2 from the first; only its blast primes
			  * it (commit_reserved_chain_tnt) */
		hold(s, BLOCK_TNT, 4);
		use_on(s, -10, GROUND_Y, CHAIN_TNT_Z, SIDE_TOP);
		assert(type_at(s, -10, GROUND_Y + 1, CHAIN_TNT_Z) == BLOCK_TNT);
		break;
	case 55: /* a lit redstone torch beside it primes it */
		hold(s, BLOCK_REDSTONE_TORCH_LIT, 8);
		use_on(s, -11, GROUND_Y, 0, SIDE_TOP);
		assert(type_at(s, -11, GROUND_Y + 1, 0) == BLOCK_REDSTONE_TORCH_LIT);
		assert(type_at(s, -10, GROUND_Y + 1, 0) == BLOCK_AIR); /* primed */
		break;
	case 60: /* torch + a three-wire line it powers */
		hold(s, BLOCK_REDSTONE_TORCH_LIT, 8);
		use_on(s, 0, GROUND_Y, 8, SIDE_TOP);
		assert(type_at(s, 0, GROUND_Y + 1, 8) == BLOCK_REDSTONE_TORCH_LIT);
		break;
	case 65:
	case 70:
	case 75: {
		int x = 1 + (int)(tick - 65) / 5;
		hold(s, ITEM_REDSTONE, 16);
		use_on(s, x, GROUND_Y, 8, SIDE_TOP);
		assert(type_at(s, x, GROUND_Y + 1, 8) == BLOCK_REDSTONE_WIRE);
		break;
	}
	case 80: /* M2 piston: stone in front of an east-facing piston ... */
		hold(s, BLOCK_STONE, 64);
		use_on(s, PISTON_X + 1, GROUND_Y, PISTON_Z, SIDE_TOP);
		assert(type_at(s, PISTON_X + 1, GROUND_Y + 1, PISTON_Z) == BLOCK_STONE);
		break;
	case 82: /* (metadata 5 = +x; placed directly: player-facing placement is not
			  * what this golden measures) */
		WCE_SET_BLOCK(s, PISTON_X, GROUND_Y + 1, PISTON_Z,
			((struct block_data) {.type = BLOCK_PISTON_BASE, .metadata = 5}));
		assert(type_at(s, PISTON_X, GROUND_Y + 1, PISTON_Z) == BLOCK_PISTON_BASE);
		break;
	case 85: /* ... powered by a lit torch behind it: set/send_piston_moving */
		hold(s, BLOCK_REDSTONE_TORCH_LIT, 8);
		use_on(s, PISTON_X - 1, GROUND_Y, PISTON_Z, SIDE_TOP);
		assert(type_at(s, PISTON_X - 1, GROUND_Y + 1, PISTON_Z)
			   == BLOCK_REDSTONE_TORCH_LIT);
		break;
	case 100: { /* M2 furnace: iron ore + one coal -> lit swap + two smelts */
		assert(type_at(s, PISTON_X + 2, GROUND_Y + 1, PISTON_Z) == BLOCK_STONE);
		assert(type_at(s, PISTON_X + 1, GROUND_Y + 1, PISTON_Z) == BLOCK_PISTON_HEAD);
		hold(s, BLOCK_FURNACE, 1);
		use_on(s, FURNACE_X, GROUND_Y, FURNACE_Z, SIDE_TOP);
		assert(type_at(s, FURNACE_X, GROUND_Y + 1, FURNACE_Z) == BLOCK_FURNACE);
		struct furnace_te* f
			= furnace_te_lookup(s, FURNACE_X, GROUND_Y + 1, FURNACE_Z);
		assert(f);
		f->items[FURNACE_SLOT_INPUT_TE]
			= (struct item_data) {.id = BLOCK_IRON_ORE, .count = 2};
		f->items[FURNACE_SLOT_FUEL_TE]
			= (struct item_data) {.id = ITEM_COAL, .count = 1};
		break;
	}
	case 105:
		assert(type_at(s, FURNACE_X, GROUND_Y + 1, FURNACE_Z) == BLOCK_FURNACE_LIT);
		break;
	case 120: /* M2 tree: sapling + bone meal -> growTree through tree_runtime */
		hold(s, BLOCK_SAPLING, 4);
		use_on(s, TREE_X, GROUND_Y, TREE_Z, SIDE_TOP);
		assert(type_at(s, TREE_X, GROUND_Y + 1, TREE_Z) == BLOCK_SAPLING);
		break;
	case 122:
		hold_meta(s, ITEM_DYE, 4, 15);
		use_on(s, TREE_X, GROUND_Y + 1, TREE_Z, SIDE_TOP);
		assert(type_at(s, TREE_X, GROUND_Y + 1, TREE_Z) == BLOCK_LOG);
		break;
	case 150: { /* M2 creeper try_explode_by with a pig victim in the blast */
		struct entity* pig = server_local_spawn_monster(
			(vec3) {CREEPER_X - 1.5F, GROUND_Y + 1.0F, CREEPER_Z + 0.5F}, MONSTER_PIG, s);
		assert(pig);
		creeper_victim_id = pig->id;
		creeper_victim_hp = pig->health;
		struct entity* creeper = server_local_spawn_monster(
			(vec3) {CREEPER_X + 0.5F, GROUND_Y + 1.0F, CREEPER_Z + 0.5F},
			MONSTER_CREEPER, s);
		assert(creeper);
		creeper->data.monster.fuse = CREEPER_FUSE_TICKS; /* detonates next tick */
		break;
	}
	case 152: {
		struct entity* pig = find_entity(s, creeper_victim_id);
		assert(!pig || pig->health < creeper_victim_hp); /* the blast met it */
		assert(type_at(s, CREEPER_X, GROUND_Y, CREEPER_Z) == BLOCK_AIR);
		break;
	}
	case 300: dig(s, PILLAR_X, GROUND_Y + 2, -4); break;
	case 320: dig(s, PILLAR_X, GROUND_Y + 1, -4); break;
	case 340: dig(s, 2, GROUND_Y + 1, 8); break; /* cut the wire line */
	case 360: dig(s, PISTON_X - 1, GROUND_Y + 1, PISTON_Z); break; /* unpower */
	case 380: /* retracted (non-sticky: the stone stays pushed) */
		assert(type_at(s, PISTON_X + 1, GROUND_Y + 1, PISTON_Z) == BLOCK_AIR);
		assert(type_at(s, PISTON_X + 2, GROUND_Y + 1, PISTON_Z) == BLOCK_STONE);
		break;
	case 418: { /* M2 fireball explode_flaming: a pig AND the host in range */
		struct entity* pig = server_local_spawn_monster(
			(vec3) {2.5F, GROUND_Y + 1.0F, 1.5F}, MONSTER_PIG, s);
		assert(pig);
		fireball_victim_id = pig->id;
		break;
	}
	case 420:
		/* 0db9e79ea removes the non-Beta newborn launch impulse and its
		 * four world-RNG draws. The pig no longer guarantees an early impact:
		 * the old ground impact at (1.5,4,1.5) is 2.15 blocks from the host's
		 * eye, outside strength 1's doubled radius. Keep even the ground
		 * impact in range (1.97 blocks), independently of the pig's motion.
		 * [Beta Explosion.doExplosionA:77-100; EntityFireball.onUpdate:127,
		 * persistent RetroMCP b1.7.3 client decompile] */
		assert(s->player.has_pos && s->player.finished_loading);
		assert(!s->player.rz.recovering);
		assert(s->player.health == MAX_PLAYER_HEALTH);
		assert(host_world(s) == &s->worlds[0]);
		server_local_spawn_fireball((vec3) {1.0F, GROUND_Y + 3.5F, 1.5F},
			(const float[3]) {0.0F, -1.0F, 0.0F}, 0, s);
		break;
	case 430:
		assert(s->player.health > 0 && s->player.health < MAX_PLAYER_HEALTH);
		printf("PASS fireball host damage: health=%d/%d, alive in slot 0\n",
			(int)s->player.health, MAX_PLAYER_HEALTH);
		break;
	default: break;
	}
}

/* Probe cells around every mechanism: the per-tick BLOCK trace. */
static const int probes[][3] = {
	{6, GROUND_Y + 1, 0}, {6, GROUND_Y + 1, 3}, {6, GROUND_Y + 1, 5},
	{8, GROUND_Y + 1, 0}, {6, GROUND_Y + 1, 6}, {7, GROUND_Y + 1, 6},
	{SAND_X, GROUND_Y + 2, -4}, {SAND_X, GROUND_Y + 1, -4}, {-10, GROUND_Y + 1, 0},
	{-10, GROUND_Y, 0}, {-9, GROUND_Y - 1, 0}, {-11, GROUND_Y + 1, 0},
	{1, GROUND_Y + 1, 8}, {2, GROUND_Y + 1, 8}, {3, GROUND_Y + 1, 8},
	{-10, GROUND_Y + 1, CHAIN_TNT_Z}, {-10, GROUND_Y, CHAIN_TNT_Z},
	{PISTON_X, GROUND_Y + 1, PISTON_Z}, {PISTON_X + 1, GROUND_Y + 1, PISTON_Z},
	{PISTON_X + 2, GROUND_Y + 1, PISTON_Z}, {FURNACE_X, GROUND_Y + 1, FURNACE_Z},
	{TREE_X, GROUND_Y + 1, TREE_Z}, {TREE_X, GROUND_Y + 4, TREE_Z},
	{CREEPER_X, GROUND_Y, CREEPER_Z}, {1, GROUND_Y, 1}, {2, GROUND_Y + 1, 1},
};

struct result {
	uint64_t total, chunks, actor, entities, per_tick, sends;
	uint32_t tick;
	size_t chunk_count, entity_count;
};

static struct result run_session(void) {
	sends = 0;
	send_hash = FNV_BASIS;
	if(getenv("CW_WCE_DUMP")) {
		char dump_path[512];
		snprintf(dump_path, sizeof(dump_path), "%s.%u", getenv("CW_WCE_DUMP"),
			wce_dump_session++);
		wce_dump = fopen(dump_path, "wb");
		assert(wce_dump);
	}
	creeper_victim_id = fireball_victim_id = 0;
	struct server_local* s = split_fixture();
	s->rand_src = (struct random_gen) {.seed = 24680};
	/* F4-RNG: World.rand drives rays/drops; body Entity.rand drives hurt
	 * pitch and death scatter. Both are runtime-seeded in normal play. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	server_local_test_seed_body_rand(s, &s->player, 1357);
	*server_local_sound_rng() = (struct random_gen) {.seed = 1357};
	*audio_cosmetic_rng() = (struct random_gen) {.seed = 97531};
	server_local_test_seed_math_random(8642);
	/* F4-RNG: ExplosionRNG is fresh per blast. Pin the next flaming blast
	 * (the scripted fireball), so its fire cells replay. */
	server_world_test_seed_fire_rng(4242);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	/* The far ring's per-tick generation cap is WALL-CLOCK
	 * (far_gen_budget_ms, FAR_GEN_BUDGET_MS by default): on a slow host
	 * (ASan, a loaded rig) fewer CRPC_FAR_CHUNK sends fit in a tick, which
	 * reorders the folded send stream (observed: ASan Linux sends 4413 vs
	 * 4411, only sends_hash moved). Lift it so the far batch is bounded by
	 * its send COUNT alone -- wall time is never an input to this golden. */
	s->far_gen_budget_ms = INT_MAX;
	assert(s->autosave_ticks == 0);
	s->player.active_inventory = &s->player.inventory;

	send_trace = getenv("CW_WCE_TRACE") != NULL;
	send_trace_tick = -1;
	clin_test_set_rpc_observer(observe_send, NULL);
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	for(unsigned load = 0; load < LOAD_TICKS; load++) {
		server_local_test_update_once(s);
		clin_drain_discard();
	}
	struct server_world* w = host_world(s);
	for(int cz = -2; cz <= 2; cz++)
		for(int cx = -2; cx <= 2; cx++) {
			const struct server_chunk* c
				= dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
			assert(c && c->sent);
		}
	assert(w->current_tick == 0);
	s->input_ack_pending = 0;

	uint64_t per_tick = FNV_BASIS;
	bool saw_primed = false, saw_many = false;
	for(unsigned tick = 0; tick < MEASURE_TICKS; tick++) {
		send_trace_tick = (int)tick;
		scripted_action(s, tick);
		server_local_test_update_once(s);
		clin_drain_discard();
		w = host_world(s);
		per_tick = hash_bytes(per_tick, &w->current_tick, sizeof(w->current_tick));
		per_tick = hash_bytes(per_tick, &s->rand_src.seed, sizeof(s->rand_src.seed));
		per_tick = hash_bytes(per_tick, &server_local_sound_rng()->seed,
			sizeof(server_local_sound_rng()->seed));
		per_tick = hash_bytes(per_tick, &w->scheduled_tick_count,
			sizeof(w->scheduled_tick_count));
		per_tick = hash_bytes(per_tick, &w->explosions_this_tick,
			sizeof(w->explosions_this_tick));
		for(size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
			struct block_data b = {0};
			bool ok = get_at(s, probes[i][0], probes[i][1], probes[i][2], &b);
			/* metadata/sky/torch are 4-bit fields: widen before hashing. */
			uint8_t cell[4] = {b.type, b.metadata, b.sky_light, b.torch_light};
			per_tick = hash_bytes(per_tick, &ok, sizeof(ok));
			per_tick = hash_bytes(per_tick, cell, sizeof(cell));
		}
		size_t entities = 0;
		uint64_t eh = entity_hash(s, &entities);
		per_tick = hash_bytes(per_tick, &eh, sizeof(eh));
		per_tick = hash_bytes(per_tick, &s->player.health, sizeof(s->player.health));
		/* Divergence triage: CW_WCE_TRACE=1 prints every folded term per tick
		 * so two builds can be diffed to the first differing tick and term. */
		if(getenv("CW_WCE_TRACE")) {
			printf("T %u ct=%u seed=%u snd=%u sched=%zu expl=%u eh=%016" PRIx64
				   " n=%zu hp=%d probes=", tick, w->current_tick, s->rand_src.seed,
				server_local_sound_rng()->seed, (size_t)w->scheduled_tick_count,
				(unsigned)w->explosions_this_tick, eh, entities, (int)s->player.health);
			for(size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
				struct block_data b = {0};
				bool ok = get_at(s, probes[i][0], probes[i][1], probes[i][2], &b);
				printf("%d:%u/%u/%u/%u ", ok, b.type, b.metadata, b.sky_light, b.torch_light);
			}
			putchar('\n');
			dict_entity_it_t it;
			for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
				dict_entity_next(it)) {
				const struct entity* e = dict_entity_ref(it)->value;
				printf("E %u id=%u type=%d pos=%a,%a,%a vel=%a,%a,%a og=%d hp=%d w=%d\n",
					tick, e->id, (int)e->type, e->pos[0], e->pos[1], e->pos[2],
					e->vel[0], e->vel[1], e->vel[2], (int)e->on_ground, (int)e->health,
					(int)(e->world == wce_world_of(s, &s->player)));
			}
		}
		if(tick > 55 && tick < 100 && entities > 0)
			saw_primed = true;
		if(entities >= 4)
			saw_many = true;
	}
	clin_test_set_rpc_observer(NULL, NULL);

	struct result r = {0};
	w = host_world(s);
	r.per_tick = per_tick;
	r.chunks = chunk_hash_world(w, &r.chunk_count);
	r.actor = actor_hash(s);
	r.entities = entity_hash(s, &r.entity_count);
	r.tick = w->current_tick;
	assert(r.tick == MEASURE_TICKS);
	/* The mechanisms really ran: the sand fell, the TNT blew a crater through
	 * the grass, the wire was lit before the cut. */
	assert(saw_primed && saw_many);
	assert(type_at(s, SAND_X, GROUND_Y + 1, -4) == BLOCK_SAND);
	assert(type_at(s, SAND_X, GROUND_Y + 2, -4) == BLOCK_AIR);
	assert(type_at(s, -10, GROUND_Y, 0) == BLOCK_AIR);
	/* M2: the chain TNT blew, the furnace smelted both ores on one coal and is
	 * still lit, and the fireball flaming blast met the host (hurt, alive). */
	assert(type_at(s, -10, GROUND_Y, CHAIN_TNT_Z) == BLOCK_AIR);
	{
		struct furnace_te* f
			= furnace_te_lookup(s, FURNACE_X, GROUND_Y + 1, FURNACE_Z);
		assert(f && f->items[FURNACE_SLOT_OUTPUT_TE].id == ITEM_IRON
			   && f->items[FURNACE_SLOT_OUTPUT_TE].count == 2);
		assert(type_at(s, FURNACE_X, GROUND_Y + 1, FURNACE_Z) == BLOCK_FURNACE_LIT);
	}
	assert(s->player.health > 0 && s->player.health < MAX_PLAYER_HEALTH);
	/* Fixed-width fold: `unsigned long` is 4 bytes on Windows (LLP64) and 8 on
	 * Linux (LP64), so hashing `sends` itself made the golden platform-
	 * dependent (identical send sequences, different sends_hash). */
	uint64_t sends64 = (uint64_t)sends;
	r.sends = hash_bytes(send_hash, &sends64, sizeof(sends64));

	r.total = FNV_BASIS;
	r.total = hash_bytes(r.total, &r.chunks, sizeof(r.chunks));
	r.total = hash_bytes(r.total, &r.actor, sizeof(r.actor));
	r.total = hash_bytes(r.total, &r.entities, sizeof(r.entities));
	r.total = hash_bytes(r.total, &r.tick, sizeof(r.tick));
	r.total = hash_bytes(r.total, &r.per_tick, sizeof(r.per_tick));
	r.total = hash_bytes(r.total, &r.sends, sizeof(r.sends));
	printf("world_context total=%016" PRIx64 " chunks=%016" PRIx64
		   " actor=%016" PRIx64 " entities=%016" PRIx64 " per_tick=%016" PRIx64
		   " sends_hash=%016" PRIx64 " tick=%u columns=%zu entity_rows=%zu sends=%lu\n",
		r.total, r.chunks, r.actor, r.entities, r.per_tick, r.sends, r.tick,
		r.chunk_count, r.entity_count, sends);
	fflush(stdout);
	if(wce_dump) {
		fclose(wce_dump);
		wce_dump = NULL;
	}
	split_destroy(s);
	if(attr_bytes) { assert(fflush(attr_bytes) == 0); assert(fflush(attr_map) == 0); }
	return r;
}

#ifndef CW_WORLD_CONTEXT_BASE_CAPTURE
/* ---- PART 2: same-coordinate isolation between two world instances ---- */

static unsigned long set_block_rpcs;
static void count_set_block(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SET_BLOCK || call->type == CRPC_PISTON_MOVING)
		set_block_rpcs++;
	clin_drain_discard();
}

static uint64_t chunk_bytes(struct server_world* w, int cx, int cz) {
	const struct server_chunk* c
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	assert(c);
	uint64_t hash = FNV_BASIS;
	hash = hash_bytes(hash, c->ids, 32768);
	hash = hash_bytes(hash, c->metadata, 16384);
	hash = hash_bytes(hash, c->lighting_sky, 16384);
	hash = hash_bytes(hash, c->lighting_torch, 16384);
	hash = hash_bytes(hash, c->heightmap, 256);
	return hash;
}

struct world_snapshot {
	uint64_t chunk;
	size_t scheduled;
	uint32_t explosions, deferrals;
	size_t resident;
};

static struct world_snapshot snap(struct server_world* w) {
	return (struct world_snapshot) {
		.chunk = chunk_bytes(w, 0, 0),
		.scheduled = w->scheduled_tick_count,
		.explosions = w->explosions_this_tick,
		.deferrals = w->explosion_deferrals,
		.resident = dict_server_chunks_size(w->chunks),
	};
}

static void assert_same(struct world_snapshot a, struct world_snapshot b) {
	assert(a.chunk == b.chunk);
	assert(a.scheduled == b.scheduled);
	assert(a.explosions == b.explosions);
	assert(a.deferrals == b.deferrals);
	assert(a.resident == b.resident);
}

static void populate_origin(struct server_local* s, struct server_world* w) {
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 1000; n++) {
		server_world_begin_update(w);
		if(server_world_ensure_populated(w, s, 0, 0, &c))
			return;
	}
	assert(!"populate failed");
}

static void isolation(void) {
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	/* Layout: the deprecated alias IS slot 0, never a copy. */
	assert(&s->world == &s->worlds[0]);
	assert(s->worlds[0].world_index == 0);
	struct server_world* a = &s->worlds[0];
	struct server_world* b = &s->worlds[1];
	assert(!server_world_chunks_ready(b));

	/* A second, independent instance in slot 1, same seed + dimension so the
	 * two chunks at (0,0) start byte-identical. */
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_OVERWORLD, 12345, true);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	populate_origin(s, a);
	populate_origin(s, b);
	assert(chunk_bytes(a, 0, 0) == chunk_bytes(b, 0, 0));
	assert(a->generation && b->generation && a->generation != b->generation);

	clin_test_set_rpc_observer(count_set_block, NULL);
	struct server_world* order[2][2] = {{b, a}, {a, b}};
	for(unsigned dir = 0; dir < 2; dir++) {
		struct server_world* hit = order[dir][0];
		struct server_world* other = order[dir][1];
		struct world_snapshot before = snap(other);
		unsigned long rpcs_before = set_block_rpcs;
		int y = GROUND_Y + 3 + (int)dir;

		assert(server_world_set_block(hit, 5, y, 5,
			(struct block_data) {.type = BLOCK_STONE}));
		assert(server_world_set_block(hit, 6, y, 5,
			(struct block_data) {.type = BLOCK_WATER_FLOW}));
		assert(server_world_set_block_silent(hit, 7, y, 5,
			(struct block_data) {.type = BLOCK_COBBLESTONE}));
		assert(server_world_set_block_metadata_quiet(hit, 5, y, 5, 3));
		assert(server_world_set_block_metadata_with_notify(hit, 5, y, 5, 4));
		struct block_data got;
		assert(server_world_get_block(hit, 5, y, 5, &got)
			   && got.type == BLOCK_STONE && got.metadata == 4);
		assert(server_world_get_block(other, 5, y, 5, &got)
			   && got.type == BLOCK_AIR);
		/* The water's tick is scheduled by BlockFlowing.onBlockAdded, a block
		 * CALLBACK. D4 landed (block_world dispatch scope) and the A2
		 * activation blocker is lifted ON PURPOSE (server_world_callback_host
		 * serves every slot of its session): each world gets its own water
		 * tick, in itself. */
		assert(hit->scheduled_tick_count > 0); /* the water tick is in hit */
		server_world_begin_update(hit);
		server_world_explode(hit, (vec3) {5.5F, GROUND_Y + 0.5F, 12.5F}, 3.0F);
		assert(server_world_get_block(hit, 5, GROUND_Y, 12, &got)
			   && got.type == BLOCK_AIR); /* the blast landed in `hit` */

		assert_same(before, snap(other));
		/* Only the world the client stream views (slot 0) publishes. */
		if(hit == a)
			assert(set_block_rpcs > rpcs_before);
		else
			assert(set_block_rpcs == rpcs_before);
		printf("PASS isolation %s: other world byte-identical at the same "
			   "coordinates (chunk=%016" PRIx64 ")\n",
			hit == a ? "slot0->slot1" : "slot1->slot0", before.chunk);
	}
	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();

	/* ---- PART 3: stale generation ---- */
	struct server_world_handle hb = server_world_handle_of(b);
	struct server_world_handle ha = server_world_handle_of(a);
	assert(hb.world_index == 1 && hb.generation == b->generation);
	assert(server_local_world_from_handle(s, hb) == b);
	assert(server_local_world_from_handle(s, ha) == a);
	assert(server_local_world_of(s, &s->player) == a);
	/* Out-of-range and zero-generation handles never resolve. */
	assert(!server_local_world_from_handle(s,
		(struct server_world_handle) {.world_index = 2, .generation = hb.generation}));
	assert(!server_local_world_from_handle(s,
		(struct server_world_handle) {.world_index = 1, .generation = 0}));
	/* A handle naming slot 1 with slot 0's generation is a different world. */
	assert(!server_local_world_from_handle(s,
		(struct server_world_handle) {.world_index = 1, .generation = ha.generation}));
	server_world_discard(b);
	assert(!server_local_world_from_handle(s, hb)); /* torn down */
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	assert(b->generation != hb.generation);
	assert(!server_local_world_from_handle(s, hb)); /* re-created: still stale */
	assert(server_local_world_from_handle(s, server_world_handle_of(b)) == b);
	server_world_discard(b);
	puts("PASS stale generation: discarded and re-created slot rejects the old handle");

	/* The accessor refuses an actor that is neither the host nor the bound guest. */
	struct server_player stranger = {0};
	assert(!server_local_world_of(s, &stranger));
	assert(!server_local_world_of(s, NULL));
	split_destroy(s);
}
#endif

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();

#ifndef CW_WORLD_CONTEXT_BASE_CAPTURE
	/* Isolation must run even when the independent historical golden fails. */
	isolation();
#endif
	struct result r = run_session();
#ifdef CW_WORLD_CONTEXT_BASE_CAPTURE
	printf("BASE CAPTURE: record GOLDEN = 0x%016" PRIx64 "\n", r.total);
	return 0;
#else
	if(r.total != GOLDEN) {
		fprintf(stderr, "test_world_context_equiv: total=%016" PRIx64
						" != GOLDEN %016" PRIx64 " (base: waves tree 88f8fb40)\n",
			r.total, GOLDEN);
		assert(false && "single-world simulation diverged from the base golden");
	}
	/* Twice in one process: no hidden state carried across sessions. */
	struct result again = run_session();
	assert(again.total == r.total);
	printf("PASS single-world equivalence: byte-identical to the waves tree 88f8fb40 "
		   "(golden %016" PRIx64 ")\n", GOLDEN);
	puts("PASS test_world_context_equiv");
	return 0;
#endif
}
