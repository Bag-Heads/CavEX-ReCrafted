/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-T D4 (KANBAN A1d L3; blocks.h "D4 seam"): block click / break / place
 * callbacks act on the ACTOR's committed world, not world slot 0.
 *
 * On the real admission harness (host + admitted guest, both Overworld) the
 * host transfers to the Nether (A2-T COMMIT, slot 1). Then, as the host:
 *   place  a block placed on a stone base in the Nether lands in slot 1;
 *   dig    breaking it again empties that slot-1 cell;
 * and across both, every loaded slot-0 cell in a box around the host's
 * Nether cells AND around the guest is byte-identical before/after (world 0
 * untouched). Control: the same place in the Overworld, before the
 * transfer, still lands in slot 0 (block_world outside a second world is
 * worlds[0], so single-player is unchanged). block_world() is NULL-routed
 * back to worlds[0] after every dispatch.
 *
 * Residuals (same pattern), each in the Nether world with world 0 checked
 * byte-identical:
 *   footstep  onEntityWalking through server_local_entity_walked_on_block_w
 *             lights redstone ore in the walker's world;
 *   tnt       a block-ignited primed TNT removes the Nether block and its
 *             entity belongs to the Nether world;
 *   sand      a falling-sand entity spawned from a Nether dispatch belongs to
 *             the Nether world;
 *   redstone  a retained (OOM-suspended) wire request queued from the Nether
 *             replays in the Nether even when serviced outside any dispatch.
 *
 * World effects (split dims: sounds, entity-driven block changes, callbacks):
 *   sound     every server sound carries its world's stamp: a sound made in
 *             a Nether dispatch is stamped {1, Nether generation}, one made
 *             in the Overworld (or outside any dispatch) {0, its generation};
 *             the explicit-world form stamps its world, both directions;
 *   entity    the Nether entity pass runs as a Nether dispatch: a falling-sand
 *             entity lands as sand in the Nether world, and its placement
 *             decision reads Nether cells -- the Overworld cell at the same
 *             coordinates is solid stone, which would have refused it (world
 *             0 byte-identical); a Nether explosion's chain TNT is primed as a
 *             Nether entity;
 *   callback  a block callback runs for a slot-1 world: removing the lower
 *             half of a Nether door notifies the upper half, whose
 *             onNeighbourBlockChange clears the orphan top in the Nether
 *             (world 0 untouched). */
#include "support/owner_swap_harness.h"

#include "block/blocks.h"
#include "block/redstone_server.h"
#include "entity/entity.h"
#include "network/server_transfer.h"
#include "platform/audio_sound_table.h"

void redstone_server_test_queue_request(struct server_world* w, int x, int y,
										int z);

#define TEST_NAME "test_a2_block_world_d4"

/* the stamp of the last CRPC_SOUND the client consumed */
static unsigned snd_count;
static uint8_t snd_index;
static uint32_t snd_generation;
static void snd_observer(const struct client_rpc* call, void* user) {
	consumed(call, user);
	if(call->type == CRPC_SOUND) {
		snd_count++;
		snd_index = call->world_index;
		snd_generation = call->world_generation;
	}
}
static bool sound_stamped(unsigned before, uint8_t index, uint32_t gen) {
	clin_update_drain();
	return snd_count == before + 1 && snd_index == index
		&& snd_generation == gen;
}
static size_t primed_tnt_in(struct server_local* s, struct server_world* w) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_PRIMED_TNT && server_local_entity_world(s, e) == w)
			n++;
	}
	return n;
}
#define MAX_STEPS 4000

static enum server_transfer_state drive(struct server_local* s, uint32_t id) {
	bool was_paused = s->paused;
	s->paused = false;
	enum server_transfer_state st = server_transfer_state_of(s, id);
	for(unsigned i = 0; i < MAX_STEPS; i++) {
		st = server_transfer_state_of(s, id);
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK)
			break;
		tick(s);
	}
	s->paused = was_paused;
	return st;
}

/* FNV-1a over every LOADED slot-0 cell of a box (unloaded cells hash a
 * marker, so a column loading or unloading also changes the hash). */
static uint64_t box_hash(struct server_world* w, int cx, int cz, int r) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(int x = cx - r; x <= cx + r; x++)
		for(int z = cz - r; z <= cz + r; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data b;
				uint32_t v = server_world_get_block(w, x, y, z, &b)
					? ((uint32_t)b.type << 24 | (uint32_t)b.metadata << 16
						| (uint32_t)b.sky_light << 8 | b.torch_light) : 0xFFFFFFFFu;
				for(int k = 0; k < 4; k++) {
					h ^= (v >> (8 * k)) & 0xFF;
					h *= UINT64_C(1099511628211);
				}
			}
	return h;
}

static uint8_t type_at(struct server_world* w, int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(w, x, y, z, &b) ? b.type : 0xFF;
}

static void hold(struct server_local* s, uint16_t id) {
	size_t slot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&s->player.inventory);
	s->player.inventory.items[slot]
		= (struct item_data) {.id = id, .count = 64, .durability = 0};
}

static void place_top(struct server_local* s, int x, int y, int z) {
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_TOP}};
	server_local_test_process_rpc(s, &call);
}

static void dig(struct server_local* s, int x, int y, int z) {
	struct server_rpc call = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.finished = true, .x = x, .y = y, .z = z,
							  .side = SIDE_TOP}};
	server_local_test_process_rpc(s, &call);
}

/* A stone base one block beside the body with two air cells above it
 * (server_world_set_block reports "changed", so an already-air cell is
 * verified by reading it back instead). */
static void prop(struct server_world* w, int x, int y, int z) {
	(void)server_world_set_block(w, x, y - 1, z,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block(w, x, y, z, (struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(w, x, y + 1, z,
		(struct block_data) {.type = BLOCK_AIR});
	assert(type_at(w, x, y - 1, z) == BLOCK_STONE && type_at(w, x, y, z) == BLOCK_AIR
		   && type_at(w, x, y + 1, z) == BLOCK_AIR);
}

/* Beta World.onBlockHit / ItemHoe.onItemUse use the same World for all
 * probes and mutations. Exercise both real owner inboxes with split dimensions. */
static void item_worlds(struct server_local* s) {
	struct server_player* actors[] = {&s->player, s->guest_actor};
	const struct {
		const char* name;
		uint16_t item;
		uint8_t base, target, result;
		bool base_changes;
	} cases[] = {
		{"fire punch", 0, BLOCK_STONE, BLOCK_FIRE, BLOCK_AIR, false},
		{"hoe grass", ITEM_IRON_HOE, BLOCK_GRASS, BLOCK_AIR, BLOCK_FARMLAND, true},
		{"hoe dirt", ITEM_IRON_HOE, BLOCK_DIRT, BLOCK_STONE, BLOCK_FARMLAND, true},
		{"flint steel", ITEM_FLINT_STEEL, BLOCK_STONE, BLOCK_AIR, BLOCK_FIRE, false},
		{"seeds", ITEM_SEED, BLOCK_FARMLAND, BLOCK_AIR, BLOCK_CROPS, false},
		{"scoop", ITEM_BUCKET, BLOCK_STONE, BLOCK_LAVA_STILL, BLOCK_AIR, false},
		{"lava", ITEM_BUCKET_LAVA, BLOCK_STONE, BLOCK_AIR, BLOCK_LAVA_FLOW, false},
		{"water", ITEM_BUCKET_WATER, BLOCK_STONE, BLOCK_AIR, BLOCK_WATER_FLOW, false},
		{"wood door", ITEM_DOOR_WOOD, BLOCK_STONE, BLOCK_AIR, BLOCK_DOOR_WOOD, false},
		{"iron door", ITEM_DOOR_IRON, BLOCK_STONE, BLOCK_AIR, BLOCK_DOOR_IRON, false},
		{"bed", ITEM_BED, BLOCK_STONE, BLOCK_AIR, BLOCK_BED, false},
		{"cake", ITEM_CAKE, BLOCK_STONE, BLOCK_AIR, BLOCK_CAKE, false},
		{"wire", ITEM_REDSTONE, BLOCK_STONE, BLOCK_AIR, BLOCK_REDSTONE_WIRE, false},
		{"bonemeal", ITEM_DYE, BLOCK_CROPS, BLOCK_AIR, BLOCK_CROPS, true},
		{"covered grass refuses", ITEM_IRON_HOE, BLOCK_GRASS, BLOCK_STONE, BLOCK_GRASS, true},
		{"bonemeal scatter", ITEM_DYE, BLOCK_GRASS, BLOCK_AIR, BLOCK_TALL_GRASS, false},
		{"no fire to punch", 0, BLOCK_STONE, BLOCK_AIR, BLOCK_AIR, false},
	};
	for(unsigned swap = 0; swap < 2; swap++) {
		for(unsigned a = 0; a < 2; a++) {
			actors[a]->world_slot = a ^ swap;
			actors[a]->dimension = (a ^ swap) ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
			actors[a]->x = actors[a]->z = 1.5;
			actors[a]->y = 101.62;
			actors[a]->rx = 0;
			actors[a]->health = MAX_PLAYER_HEALTH;
			actors[a]->has_pos = actors[a]->finished_loading = true;
			actors[a]->active_inventory = &actors[a]->inventory;
		}
		for(unsigned a = 0; a < 2; a++) {
			struct server_player* actor = actors[a];
			struct server_world* w = server_local_world_of(s, actor);
			struct server_world* other = server_local_world_of(s, actors[1-a]);
			assert(w && other && w != other);
			for(unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
				struct thread consumer;
				__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
				assert(thread_create(&consumer, consumer_entry, NULL, 64));
				for(unsigned wi = 0; wi < 2; wi++) {
					struct server_world* fw = &s->worlds[wi];
					struct server_chunk* chunk = NULL;
					if(!server_world_is_chunk_loaded(fw, 0, 0)) {
						server_world_begin_update(fw);
						assert(server_world_load_chunk(fw, 0, 0, &chunk) && chunk);
					}
					for(int x = 5; x <= 11; x++)
						for(int z = 5; z <= 11; z++)
							for(int y = 98; y <= 103; y++)
								(void)server_world_set_block_silent(fw, x, y, z,
									(struct block_data){.type = y <= 99 ? BLOCK_STONE : BLOCK_AIR});
					(void)server_world_set_block_silent(fw, 8, 99, 8,
						(struct block_data){.type = cases[c].base});
					(void)server_world_set_block_silent(fw, 8, 100, 8,
						(struct block_data){.type = cases[c].target});
					/* Opposite probe values expose wrong-world READS as well
					 * as writes, including the refusal paths. */
					if(fw == other && (cases[c].item == ITEM_IRON_HOE || cases[c].item == 0))
						(void)server_world_set_block_silent(fw, 8, 100, 8,
							(struct block_data){.type = cases[c].target == BLOCK_AIR
								? (cases[c].item == 0 ? BLOCK_FIRE : BLOCK_STONE) : BLOCK_AIR});
				}
				__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
				thread_join(&consumer);
				clin_update_drain();
				size_t slot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&actor->inventory);
				actor->inventory.items[slot] = (struct item_data){
					.id = cases[c].item, .count = 4, .durability = cases[c].item == ITEM_DYE ? 15 : 0};
				const uint64_t untouched = box_hash(other, 8, 8, 6);
				struct item_data other_items[INVENTORY_SIZE];
				memcpy(other_items, actors[1-a]->inventory.items, sizeof(other_items));
				struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
					.payload.block_place = {.x = 8, .y = 99, .z = 8, .side = cases[c].base == BLOCK_DIRT ? SIDE_BOTTOM : SIDE_TOP}};
				if(cases[c].item == 0)
					call = (struct server_rpc){.type = SRPC_BLOCK_DIG,
						.payload.block_dig = {.x = 8, .y = 99, .z = 8,
							.side = SIDE_TOP, .finished = false}};
				/* ItemBucket.onItemRightClick ignores the clicked cell and runs
				 * its own 5.0-block eye ray [ItemBucket.java:12-28]; the actors
				 * stand at (1.5, 1.5), out of reach, so a bucket click stands
				 * one block west of (8,99,8) and looks at its top-face centre
				 * (through the lava source for the scoop). Pose restored after. */
				const bool bucket = cases[c].item == ITEM_BUCKET
					|| cases[c].item == ITEM_BUCKET_WATER || cases[c].item == ITEM_BUCKET_LAVA;
				const double pose_x = actor->x, pose_y = actor->y, pose_z = actor->z;
				const float pose_rx = actor->rx, pose_ry = actor->ry;
				if(bucket) {
					actor->x = 7.5; actor->y = 100.0 + 1.62; actor->z = 8.5;
					double dx = 8.5 - actor->x, dy = 100.0 - actor->y, dz = 8.5 - actor->z;
					/* Beta look: (-sin yaw cos pitch, -sin pitch, cos yaw cos pitch). */
					actor->rx = (float)(atan2(-dx, dz) * 180.0 / 3.141592653589793);
					actor->ry = (float)(atan2(-dy, sqrt(dx * dx + dz * dz)) * 180.0
						/ 3.141592653589793);
				}
				if(a == 0) host_send(s, &call);
				else assert(guest_send(s, &call));
				actor->x = pose_x; actor->y = pose_y; actor->z = pose_z;
				actor->rx = pose_rx; actor->ry = pose_ry;
				uint8_t expected = cases[c].result;
				if(cases[c].item == ITEM_DYE && cases[c].base == BLOCK_GRASS) {
					uint8_t grown = type_at(w, 8, 100, 8);
					check(grown == BLOCK_TALL_GRASS || grown == BLOCK_FLOWER || grown == BLOCK_ROSE,
						"bonemeal scatters a Beta plant in actor world");
					expected = grown;
				}
				if(cases[c].item == ITEM_BUCKET_WATER && w->dimension == WORLD_DIM_NETHER)
					expected = BLOCK_AIR;
				printf("owner %u world %u %s: ", a, actor->world_slot, cases[c].name);
				check(type_at(w, 8, cases[c].base_changes ? 99 : 100, 8) == expected,
					"action affects actor world");
				check(box_hash(other, 8, 8, 6) == untouched, "other dimension untouched");
				check(memcmp(other_items, actors[1-a]->inventory.items, sizeof(other_items)) == 0,
					"other player inventory byte-identical");
				check(block_world(s) == &s->worlds[0], "dispatch world restored");
				if(cases[c].item == ITEM_IRON_HOE)
					check(actor->inventory.items[slot].durability == (expected == BLOCK_FARMLAND),
						"hoe wears only on successful till");
				if(cases[c].item == ITEM_DYE && cases[c].base == BLOCK_CROPS) {
					struct block_data b;
					assert(server_world_get_block(w, 8, 99, 8, &b));
					check(b.metadata == 7, "actor crop mature");
				}
				if(cases[c].item == ITEM_DOOR_WOOD || cases[c].item == ITEM_DOOR_IRON)
					check(type_at(w, 8, 101, 8) == expected, "door upper half in actor world");
				if(cases[c].item == ITEM_BED)
					check(type_at(w, 8, 100, 9) == BLOCK_BED, "bed head in actor world");
				if(cases[c].item == ITEM_BUCKET_WATER || cases[c].item == ITEM_BUCKET_LAVA)
					check(actor->inventory.items[slot].id == ITEM_BUCKET, "bucket emptied");
			}
		}
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	harness_init();
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_world* ow = &s->worlds[0];
	check(server_local_world_of(s, &s->player) == ow, "fixture: host in slot 0");

	/* Control: single-world place still lands in slot 0. */
	int ox = (int)floor(s->player.x) + 1, oy = (int)floor(s->player.y - 1.62 + 0.01),
		oz = (int)floor(s->player.z);
	prop(ow, ox, oy, oz);
	hold(s, BLOCK_COBBLESTONE);
	place_top(s, ox, oy - 1, oz);
	check(type_at(ow, ox, oy, oz) == BLOCK_COBBLESTONE,
		  "control: a single-world place lands in slot 0");
	check(block_world(s) == ow, "control: block_world is slot 0 after the dispatch");

	/* The host commits to the Nether. */
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK, "host -> Nether requested");
	check(drive(s, id) == SERVER_TRANSFER_COMMITTED, "host -> Nether COMMITTED");
	struct server_world* nw = server_local_world_of(s, &s->player);
	check(nw && nw != ow && nw->dimension == WORLD_DIM_NETHER,
		  "the host is committed to the Nether world (slot 1)");
	s->paused = true;

	int nx = (int)floor(s->player.x) + 1, ny = (int)floor(s->player.y - 1.62 + 0.01),
		nz = (int)floor(s->player.z);
	prop(nw, nx, ny, nz);
	const uint64_t h_here = box_hash(ow, nx, nz, 6);
	const uint64_t h_guest = box_hash(ow, (int)floor(s->guest_actor->x),
									  (int)floor(s->guest_actor->z), 6);

	hold(s, BLOCK_COBBLESTONE);
	place_top(s, nx, ny - 1, nz);
	check(type_at(nw, nx, ny, nz) == BLOCK_COBBLESTONE,
		  "place: the host's block lands in the Nether world (slot 1)");
	check(block_world(s) == ow, "place: block_world back to slot 0 afterwards");
	check(box_hash(ow, nx, nz, 6) == h_here
			  && box_hash(ow, (int)floor(s->guest_actor->x),
						  (int)floor(s->guest_actor->z), 6) == h_guest,
		  "place: world 0 untouched (same coordinates and around the guest)");

	dig(s, nx, ny, nz);
	check(type_at(nw, nx, ny, nz) == BLOCK_AIR,
		  "dig: the host breaks the block in the Nether world (slot 1)");
	check(block_world(s) == ow, "dig: block_world back to slot 0 afterwards");
	check(box_hash(ow, nx, nz, 6) == h_here
			  && box_hash(ow, (int)floor(s->guest_actor->x),
						  (int)floor(s->guest_actor->z), 6) == h_guest,
		  "dig: world 0 untouched (same coordinates and around the guest)");

	/* ---- residuals --------------------------------------------------- */
	const int rx = nx + 3, ry = ny, rz = nz + 3;

	/* footstep: redstone ore under the walker lights (Beta onEntityWalking) */
	(void)server_world_set_block_silent(nw, rx, ry - 1, rz,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block_silent(nw, rx, ry, rz,
		(struct block_data) {.type = 73}); /* redstone ore */
	const uint64_t h_res = box_hash(ow, rx, rz, 6);
	server_local_entity_walked_on_block_w(s, nw, rx, ry, rz, BLOCK_WALKER_PLAYER);
	check(type_at(nw, rx, ry, rz) == 74 /* glowing */,
		  "footstep: onEntityWalking lights the ore in the walker's world");
	check(box_hash(ow, rx, rz, 6) == h_res && block_world(s) == ow,
		  "footstep: world 0 untouched, block_world restored");

	/* tnt: ignition inside a Nether dispatch */
	(void)server_world_set_block_silent(nw, rx + 1, ry, rz,
		(struct block_data) {.type = BLOCK_TNT});
	size_t before = dict_entity_size(s->entities);
	struct server_world* prev = block_world_enter(s, nw);
	bool primed = server_local_prime_tnt_block(s, rx + 1, ry, rz, 80);
	block_world_leave(s, prev);
	struct entity* tnt = NULL;
	dict_entity_it_t eit;
	for(dict_entity_it(eit, s->entities); !dict_entity_end_p(eit);
		dict_entity_next(eit)) {
		struct entity* e = dict_entity_ref(eit)->value;
		if(e && e->type == ENTITY_PRIMED_TNT)
			tnt = e;
	}
	check(primed && type_at(nw, rx + 1, ry, rz) == BLOCK_AIR,
		  "tnt: the Nether TNT block is removed");
	check(tnt && dict_entity_size(s->entities) == before + 1
			  && server_local_entity_world(s, tnt) == nw,
		  "tnt: the primed TNT entity belongs to the Nether world");
	check(box_hash(ow, rx, rz, 6) == h_res, "tnt: world 0 untouched");
	/* the server stays paused: the fuse never runs in this fixture */

	/* sand: a falling-sand entity from a Nether dispatch */
	prev = block_world_enter(s, nw);
	struct entity* sand = server_local_spawn_falling_sand(
		s, (vec3) {rx + 0.5F, ry + 4.5F, rz + 0.5F}, BLOCK_SAND);
	block_world_leave(s, prev);
	check(sand && server_local_entity_world(s, sand) == nw,
		  "sand: the falling-sand entity belongs to the Nether world");

	/* redstone: a retained request tagged Nether replays in the Nether */
	const int wx = rx - 3, wz = rz;
	(void)server_world_set_block_silent(nw, wx, ry - 1, wz,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block_silent(nw, wx, ry, wz,
		(struct block_data) {.type = BLOCK_REDSTONE_WIRE, .metadata = 0});
	(void)server_world_set_block_silent(nw, wx + 1, ry - 1, wz,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block_silent(nw, wx + 1, ry, wz,
		(struct block_data) {.type = BLOCK_REDSTONE_TORCH_LIT, .metadata = 5});
	const uint64_t h_rs = box_hash(ow, wx, wz, 6);
	redstone_server_test_queue_request(nw, wx, ry, wz);
	check(redstone_server_pending(), "redstone: the request is retained");
	check(block_world(s) == ow, "redstone: serviced OUTSIDE any dispatch");
	redstone_server_service_pending(s);
	struct block_data wire;
	check(server_world_get_block(nw, wx, ry, wz, &wire)
			  && wire.type == BLOCK_REDSTONE_WIRE && wire.metadata == 15,
		  "redstone: the retained flood powered the Nether wire (15)");
	check(!redstone_server_pending() && box_hash(ow, wx, wz, 6) == h_rs,
		  "redstone: drained, world 0 untouched");

	/* ---- world effects: sounds ----------------------------------------- */
	clin_update_drain();
	clin_test_set_consume_observer(snd_observer, NULL);
	vec3 at = {(float)nx, (float)ny, (float)nz};
	unsigned b = snd_count;
	prev = block_world_enter(s, nw);
	server_local_emit_sound(SOUND_RANDOM_CLICK, at, 1.0F, 1.0F);
	block_world_leave(s, prev);
	check(sound_stamped(b, (uint8_t)nw->world_index, nw->generation),
		  "sound: a Nether dispatch sound is stamped with the Nether world");
	b = snd_count;
	prev = block_world_enter(s, ow);
	server_local_emit_sound(SOUND_RANDOM_CLICK, at, 1.0F, 1.0F);
	block_world_leave(s, prev);
	check(sound_stamped(b, 0, ow->generation),
		  "sound: an Overworld dispatch sound is stamped with the Overworld");
	b = snd_count;
	server_local_emit_sound(SOUND_RANDOM_CLICK, at, 1.0F, 1.0F);
	check(sound_stamped(b, 0, ow->generation),
		  "sound: outside any dispatch the sound is the Overworld (slot 0) one");
	b = snd_count;
	server_local_emit_sound_w(nw, SOUND_RANDOM_CLICK, at, 1.0F, 1.0F);
	check(sound_stamped(b, (uint8_t)nw->world_index, nw->generation),
		  "sound: emit_sound_w(Nether) is stamped with the Nether (no slot-0 gate)");
	b = snd_count;
	server_local_emit_sound_w(ow, SOUND_RANDOM_CLICK, at, 1.0F, 1.0F);
	check(sound_stamped(b, 0, ow->generation),
		  "sound: emit_sound_w(Overworld) is stamped with the Overworld");
	b = snd_count;
	server_local_emit_sound_ui_w(nw, &s->player, SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	check(sound_stamped(b, (uint8_t)nw->world_index, nw->generation),
		  "sound: a UI sound for the host in the Nether is stamped with it");
	b = snd_count;
	server_local_emit_sound_ui_w(ow, &s->player, SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	clin_update_drain();
	check(snd_count == b, "sound: a UI sound never goes to a world its actor is not in");
	clin_test_set_consume_observer(consumed, NULL);

	/* ---- world effects: entity-driven block change (falling sand) ------ */
	const int fx = nx - 3, fz = nz - 3;
	for(int y = ny - 1; y <= ny + 6; y++)
		(void)server_world_set_block_silent(nw, fx, y, fz,
			(struct block_data) {.type = y == ny - 1 ? BLOCK_STONE : BLOCK_AIR});
	/* the Overworld cell at the landing coordinates is solid: a placement
	 * decision read from world 0 (the pre-fix slot-0 default) refuses it */
	(void)server_world_set_block_silent(ow, fx, ny, fz,
		(struct block_data) {.type = BLOCK_STONE});
	const uint64_t h_sand = box_hash(ow, fx, fz, 4);
	const uint8_t ow_cell = type_at(ow, fx, ny, fz);
	prev = block_world_enter(s, nw);
	struct entity* fs = server_local_spawn_falling_sand(
		s, (vec3) {fx + 0.5F, ny + 5.5F, fz + 0.5F}, BLOCK_SAND);
	block_world_leave(s, prev);
	check(fs && server_local_entity_world(s, fs) == nw,
		  "entity: the falling sand is a Nether entity");
	bool landed = false;
	for(int i = 0; i < 200 && !landed; i++) {
		(void)server_local_test_tick_entities_w(s, nw);
		landed = type_at(nw, fx, ny, fz) == BLOCK_SAND;
	}
	check(landed, "entity: the Nether entity pass lands the sand in the Nether world");
	check(block_world(s) == ow, "entity: block_world is slot 0 after the pass");
	check(box_hash(ow, fx, fz, 4) == h_sand && type_at(ow, fx, ny, fz) == ow_cell,
		  "entity: world 0 untouched by the Nether landing");

	/* ---- world effects: a slot-1 block callback (the TNT chain) -------- */
	const int tx = nx + 6, tz = nz - 6;
	for(int dx = -1; dx <= 1; dx++)
		for(int dz = -1; dz <= 1; dz++)
			for(int y = ny - 1; y <= ny + 2; y++)
				(void)server_world_set_block_silent(nw, tx + dx, y, tz + dz,
					(struct block_data) {.type = y == ny - 1 ? BLOCK_STONE : BLOCK_AIR});
	(void)server_world_set_block_silent(nw, tx + 1, ny, tz,
		(struct block_data) {.type = BLOCK_TNT});
	const uint64_t h_tnt = box_hash(ow, tx, tz, 6);
	const size_t tnt_before = primed_tnt_in(s, nw);
	bool boomed = server_world_try_explode(nw, (vec3) {tx + 0.5F, ny + 0.5F, tz + 0.5F}, 2.0F);
	check(boomed && type_at(nw, tx + 1, ny, tz) == BLOCK_AIR,
		  "callback: the Nether explosion removed the TNT block");
	check(primed_tnt_in(s, nw) == tnt_before + 1,
		  "entity: the Nether explosion primes its chain TNT as a Nether entity");
	check(box_hash(ow, tx, tz, 6) == h_tnt, "entity: world 0 untouched by the blast");

	/* a slot-1 neighbour callback: the orphaned door top clears itself */
	const int dx0 = nx - 6, dz0 = nz + 6;
	(void)server_world_set_block_silent(nw, dx0, ny - 1, dz0,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block_silent(nw, dx0, ny, dz0,
		(struct block_data) {.type = BLOCK_DOOR_WOOD, .metadata = 0});
	(void)server_world_set_block_silent(nw, dx0, ny + 1, dz0,
		(struct block_data) {.type = BLOCK_DOOR_WOOD, .metadata = 8});
	const uint64_t h_door = box_hash(ow, dx0, dz0, 4);
	(void)server_world_set_block(nw, dx0, ny, dz0, (struct block_data) {.type = BLOCK_AIR});
	check(type_at(nw, dx0, ny + 1, dz0) == BLOCK_AIR,
		  "callback: a slot-1 set_block dispatches onNeighbourBlockChange -- the "
		  "orphan Nether door top is cleared");
	check(box_hash(ow, dx0, dz0, 4) == h_door && block_world(s) == ow,
		  "callback: world 0 untouched, block_world restored");

	item_worlds(s);
	teardown(s);
	return harness_result(TEST_NAME, "place_dig_residuals_worldfx");
}
