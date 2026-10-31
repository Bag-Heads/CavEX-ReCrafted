/* Copyright (c) 2026 CaveWii_v2 contributors
 * SNOW-001: reed / cake / repeater used ON a snow layer (BLOCK_SNOW, id 78)
 * place INSIDE the snow cell, exactly like Beta ItemReed.onItemUse.
 *
 * Beta spec (Item.reed, Item.cake and Item.redstoneRepeater are all
 * `new ItemReed(...)`):
 *   ItemReed.java:12-13 [740c583] / server CFR ItemReed.java:32-33:
 *     if(world.getBlockId(x, y, z) == Block.snow.blockID) side = 0;
 *     -- the clicked SNOW cell itself is the target and the side becomes 0
 *     (bottom), whatever face was clicked; the offset cell is NEVER read.
 *     Otherwise the target is the clicked cell offset by the side
 *     (740c583 ItemReed.java:11-53; server CFR :34-53).
 *   ItemReed.java:43-48 [740c583] / server CFR :57-63:
 *     canBlockBePlacedAt(id, x, y, z, false, side) -> setBlockWithNotify ->
 *     onBlockPlaced(side) -> onBlockPlacedBy(player) -> playSoundEffect at
 *     (x + .5, y + .5, z + .5), step name, (vol + 1) / 2, pitch * .8 ->
 *     --stackSize.
 *   World.java:2090-2106 [740c583] (server CFR World.java:1605-1620)
 *     canBlockBePlacedAt: the placed block's collision box must be clear of
 *     entities, and snow (like water/lava/fire) counts as replaceable.
 *   BlockReed.java:33-35 [740c583]: dirt/grass below with water beside it
 *     (or reed below).
 *   BlockCake.java:80-93 [740c583]: canBlockStay = solid material below.
 *   BlockRedstoneRepeater.java:20-21,117-122 [740c583]: normal cube below;
 *     onBlockPlacedBy facing ((floor(yaw * 4 / 360 + .5) & 3) + 2) % 4.
 *
 * Every placement goes through the REAL SRPC_BLOCK_PLACE dispatch: the host
 * through its own inbox, the guest through its tagged slot-1 route, each into
 * its own committed world (host slot 0 / guest slot 1, then swapped). The
 * other world holds DIFFERENT blocks at the same coordinates (wool, not snow),
 * so a wrong-world read or write is visible; it and the other player's
 * inventory must stay byte-identical.
 *
 * The placement SIDE the dispatch hands to the item's onItemPlace is observed
 * by a pass-through spy on the three item definitions (the original hook is
 * always called, so behaviour is unchanged).
 *
 * RED until SNOW-002 (server_local.c place dispatch snow retarget). Setup
 * failures abort; behaviour checks print ok/FAIL and the process exits 1. */
#include "support/owner_swap_harness.h"

#include "block/blocks.h"
#include "platform/audio.h"

#define TEST_NAME "test_itemreed_snow_place"
#define Y 101
#define CX 8
#define CZ 8
/* chunk-edge case: chunk (20, 0) is loaded on purpose, (21, 0) never is */
#define EDGE_CHUNK 20
#define EDGE_X (EDGE_CHUNK * 16 + 15)

/* ---- the three ItemReed-class items ------------------------------------ */
enum kind { K_REED, K_CAKE, K_REPEATER, K_COUNT };
static const struct {
	const char* name;
	uint16_t item;
	uint8_t block;
	uint16_t step; /* hard-coded Beta step sounds, not the production table:
					* Block.java:300 reed=soundGrassFootstep, :309 cake=
					* soundClothFootstep, :310 repeater=soundWoodFootstep;
					* all StepSound(name, 1.0f, 1.0f) */
	uint8_t count; /* cake is maxStackSize 1 */
	uint8_t meta;  /* reed 0, fresh cake 0, repeater yaw 0 -> facing 2 */
} kinds[K_COUNT] = {
	{"reed", ITEM_REED, BLOCK_REED, SOUND_STEP_GRASS, 2, 0},
	{"cake", ITEM_CAKE, BLOCK_CAKE, SOUND_STEP_CLOTH, 1, 0},
	{"repeater", ITEM_REDSTONE_REPEATER, BLOCK_REDSTONE_REPEATER,
	 SOUND_STEP_WOOD, 2, 2},
};

/* ---- pass-through spy on onItemPlace ----------------------------------- */
typedef bool (*place_fn)(struct server_local*, struct server_player*,
	struct item_data*, struct block_info*, struct block_info*, enum side);
static place_fn spy_orig[K_COUNT];
static unsigned spy_calls;
static enum side spy_side;
static int spy_x, spy_y, spy_z;

static bool spy_place(struct server_local* s, struct server_player* actor,
	struct item_data* it, struct block_info* where, struct block_info* on,
	enum side side) {
	spy_calls++;
	spy_side = side;
	spy_x = where->x;
	spy_y = where->y;
	spy_z = where->z;
	for(int k = 0; k < K_COUNT; k++)
		if(it->id == kinds[k].item)
			return spy_orig[k](s, actor, it, where, on, side);
	assert(false);
	return false;
}

/* ---- sound capture (the harness tally plus vol/pitch) ------------------- */
static unsigned snd_n;
static struct client_rpc snd_last;
static void observer(const struct client_rpc* rpc, void* user) {
	consumed(rpc, user);
	if(rpc->type == CRPC_SOUND && !rpc->payload.sound.ui) {
		snd_n++;
		snd_last = *rpc;
	}
}

/* ---- world helpers ------------------------------------------------------ */
static bool get(struct server_world* w, int x, int y, int z,
	struct block_data* b) {
	return server_world_get_block(w, x, y, z, b);
}
static struct block_data cell(struct server_world* w, int x, int y, int z) {
	struct block_data b = {0};
	assert(get(w, x, y, z, &b));
	return b;
}
static void set_cell(struct server_world* w, int x, int y, int z,
	uint8_t type, uint8_t meta) {
	struct block_data b = cell(w, x, y, z);
	if(b.type != type || b.metadata != meta)
		assert(server_world_set_block_silent(w, x, y, z,
			(struct block_data) {.type = type, .metadata = meta}));
}
static void load_chunk(struct server_world* w, int cx, int cz) {
	struct server_chunk* c
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	for(unsigned n = 0; n < 200 && !c; n++) {
		server_world_begin_update(w);
		(void)server_world_load_chunk(w, cx, cz, &c);
	}
	assert(c);
}

/* Silent setup still publishes to the bounded client FIFO: a consumer
 * thread drains it while the box is rebuilt (test_piston_world_isolation). */
static struct thread setup_consumer;
static void setup_begin(void) {
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&setup_consumer, consumer_entry, NULL, 64));
}
static void setup_end(void) {
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&setup_consumer);
	clin_update_drain();
}

/* Clear the 5x5x5 box around (x, y, z) (cells inside the world only). */
static void clear_box(struct server_world* w, int x, int y, int z) {
	for(int dx = -2; dx <= 2; dx++)
		for(int dz = -2; dz <= 2; dz++)
			for(int dy = -2; dy <= 2; dy++)
				if(y + dy >= 0 && y + dy < WORLD_HEIGHT)
					set_cell(w, x + dx, y + dy, z + dz, BLOCK_AIR, 0);
}

/* The prop for `kind` with its top cell at (x, y, z): Beta-valid support
 * (dirt + water beside it for reed, stone otherwise) one below. */
static void prop(struct server_world* w, enum kind k, int x, int y, int z,
	uint8_t top) {
	clear_box(w, x, y, z);
	set_cell(w, x, y - 1, z, k == K_REED ? BLOCK_DIRT : BLOCK_STONE, 0);
	if(k == K_REED)
		set_cell(w, x - 1, y - 1, z, BLOCK_WATER_STILL, 0);
	if(top != BLOCK_AIR)
		set_cell(w, x, y, z, top, 0);
}

/* The OTHER world at the same coordinates: different blocks (no snow --
 * wool on stone), so a wrong-world read takes the offset path. */
static void decoy(struct server_world* w, int x, int y, int z) {
	clear_box(w, x, y, z);
	set_cell(w, x, y - 1, z, BLOCK_STONE, 0);
	set_cell(w, x, y, z, BLOCK_WOOL, 5);
}

/* FNV-1a over type+metadata of the 5x5x5 box (unreadable cells hash a
 * marker). Lighting is excluded: the isolation contract is block ids. */
static uint64_t box_hash(struct server_world* w, int x, int y, int z) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(int dx = -2; dx <= 2; dx++)
		for(int dz = -2; dz <= 2; dz++)
			for(int dy = -2; dy <= 2; dy++) {
				struct block_data b;
				uint32_t v = get(w, x + dx, y + dy, z + dz, &b)
					? ((uint32_t)b.type << 8 | b.metadata) : 0xFFFFFFFFu;
				for(int i = 0; i < 4; i++) {
					h ^= (v >> (8 * i)) & 0xFF;
					h *= UINT64_C(1099511628211);
				}
			}
	return h;
}

/* How many cells of the box hold `type`. */
static unsigned box_count(struct server_world* w, int x, int y, int z,
	uint8_t type) {
	unsigned n = 0;
	for(int dx = -2; dx <= 2; dx++)
		for(int dz = -2; dz <= 2; dz++)
			for(int dy = -2; dy <= 2; dy++) {
				struct block_data b;
				if(get(w, x + dx, y + dy, z + dz, &b) && b.type == type)
					n++;
			}
	return n;
}

/* ---- actors ------------------------------------------------------------- */
static void stand(struct server_player* p, double x, int feet_y, double z) {
	p->x = x;
	p->y = feet_y + (double)PLAYER_EYE_HEIGHT;
	p->z = z;
	p->rx = 0.0F; /* yaw 0 -> repeater facing 2 */
	p->ry = 0.0F;
	p->vel_x = p->vel_y = p->vel_z = 0.0F;
	p->health = MAX_PLAYER_HEALTH;
	p->on_ground = p->has_pos = p->finished_loading = true;
	p->active_inventory = &p->inventory;
}
/* Bodies park well away from every fixture cell (the obstruction sweep sees
 * every present body). */
static void park(struct server_local* s) {
	stand(&s->player, 2.5, Y, 2.5);
	stand(s->guest_actor, 2.5, Y, 3.5);
	server_local_refresh_player_membership(s);
}

static void hold(struct server_player* p, enum kind k) {
	inventory_set_hotbar(&p->inventory, 0);
	inventory_set_slot(&p->inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = kinds[k].item, .count = kinds[k].count});
}

static void place(struct server_local* s, struct server_player* actor,
	int x, int y, int z, enum side side) {
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side}};
	snd_n = 0;
	spy_calls = 0;
	spy_side = SIDE_MAX;
	if(actor == &s->player)
		host_send(s, &call);
	else
		assert(guest_send(s, &call));
}

static const char* side_name(enum side side) {
	switch(side) {
		case SIDE_TOP: return "top";
		case SIDE_BOTTOM: return "bottom";
		case SIDE_LEFT: return "-x";
		case SIDE_RIGHT: return "+x";
		case SIDE_FRONT: return "front";
		case SIDE_BACK: return "back";
		default: return "?";
	}
}

/* ---- one placement through the real dispatch ---------------------------- */
struct ctx {
	struct server_local* s;
	struct server_player* actor;
	struct server_player* other;
	struct server_world* w;  /* the actor's committed world */
	struct server_world* ow; /* the other player's committed world */
	const char* who;
};

static char label[160];
static void say(const struct ctx* c, enum kind k, const char* what,
	const char* claim) {
	snprintf(label, sizeof label, "%s world %u %s %s: %s", c->who,
		c->w->world_index, kinds[k].name, what, claim);
}

/* Clicks (x, y, z) on `side`; expects the block at (px, py, pz) with the
 * dispatch handing `eside` to onItemPlace. The box is centred on the
 * clicked cell; `offset_x/y/z` is the clicked side's neighbour, which must
 * be untouched when the click was on snow. */
static void expect_place(const struct ctx* c, enum kind k, const char* what,
	int x, int y, int z, enum side side, int px, int py, int pz,
	enum side eside, bool on_snow) {
	struct server_local* s = c->s;
	int ox, oy, oz;
	blocks_side_offset(side, &ox, &oy, &oz);
	ox += x;
	oy += y;
	oz += z;
	struct block_data off_before;
	bool off_readable = get(c->w, ox, oy, oz, &off_before);
	hold(c->actor, k);
	const uint64_t other_world = box_hash(c->ow, x, y, z);
	struct item_data other_inv[INVENTORY_SIZE];
	memcpy(other_inv, c->other->inventory.items, sizeof other_inv);
	const struct random_gen rng = s->rand_src;
	place(s, c->actor, x, y, z, side);

	struct block_data got = {0};
	bool readable = get(c->w, px, py, pz, &got);
	say(c, k, what, "block placed at the expected cell");
	check(readable && got.type == kinds[k].block, label);
	if(readable && got.type == kinds[k].block) {
		say(c, k, what, "placed metadata is Beta's");
		check(got.metadata == kinds[k].meta, label);
	}
	say(c, k, what, "exactly one such block in the box (no y change)");
	check(box_count(c->w, x, y, z, kinds[k].block) == 1, label);
	if(on_snow) {
		say(c, k, what, "snow gone");
		check(box_count(c->w, x, y, z, BLOCK_SNOW) == 0, label);
		struct block_data off_after;
		say(c, k, what, "clicked side's offset cell untouched");
		check(off_readable == get(c->w, ox, oy, oz, &off_after)
				&& (!off_readable
					|| (off_after.type == off_before.type
						&& off_after.metadata == off_before.metadata)),
			label);
	}
	say(c, k, what, "onItemPlace got the expected side and cell");
	check(spy_calls == 1 && spy_side == eside && spy_x == px && spy_y == py
			&& spy_z == pz,
		label);
	if(spy_calls)
		printf("     (onItemPlace side=%s at %d,%d,%d; clicked %s at %d,%d,%d)\n",
			side_name(spy_side), spy_x, spy_y, spy_z, side_name(side), x, y, z);
	say(c, k, what, "exactly one Beta STEP sound at the placed cell + .5");
	check(snd_n == 1 && snd_last.payload.sound.sound_id == kinds[k].step
			/* ItemReed:62 (v + 1) / 2 and p * .8, StepSound(name, 1, 1) */
			&& snd_last.payload.sound.vol == (1.0F + 1.0F) / 2.0F
			&& snd_last.payload.sound.pitch == 1.0F * 0.8F
			&& snd_last.payload.sound.pos[0] == px + 0.5F
			&& snd_last.payload.sound.pos[1] == py + 0.5F
			&& snd_last.payload.sound.pos[2] == pz + 0.5F,
		label);
	const struct item_data held
		= c->actor->inventory.items[INVENTORY_SLOT_HOTBAR];
	say(c, k, what, "exactly one survival item consumed");
	check(held.count == kinds[k].count - 1
			&& (held.count == 0 || held.id == kinds[k].item),
		label);
	say(c, k, what, "s->rand_src unchanged");
	check(memcmp(&rng, &s->rand_src, sizeof rng) == 0, label);
	say(c, k, what, "other world byte-identical");
	check(box_hash(c->ow, x, y, z) == other_world, label);
	say(c, k, what, "other player's inventory byte-identical");
	check(memcmp(other_inv, c->other->inventory.items, sizeof other_inv) == 0,
		label);
	check(block_world(s) == &s->worlds[0], "dispatch world restored");
}

/* A refused click on snow: snow unchanged, inventory unchanged, no sound. */
static void expect_refusal(const struct ctx* c, enum kind k, const char* what,
	int x, int y, int z) {
	struct server_local* s = c->s;
	hold(c->actor, k);
	const struct block_data snow_before = cell(c->w, x, y, z);
	const uint64_t world_before = box_hash(c->w, x, y, z);
	const uint64_t other_world = box_hash(c->ow, x, y, z);
	struct item_data inv[INVENTORY_SIZE], other_inv[INVENTORY_SIZE];
	memcpy(inv, c->actor->inventory.items, sizeof inv);
	memcpy(other_inv, c->other->inventory.items, sizeof other_inv);
	const struct random_gen rng = s->rand_src;
	place(s, c->actor, x, y, z, SIDE_TOP);
	const struct block_data snow_after = cell(c->w, x, y, z);
	say(c, k, what, "refused: snow unchanged");
	check(snow_before.type == BLOCK_SNOW && snow_after.type == BLOCK_SNOW
			&& snow_after.metadata == snow_before.metadata,
		label);
	say(c, k, what, "refused: actor world box unchanged");
	check(box_hash(c->w, x, y, z) == world_before, label);
	say(c, k, what, "refused: inventory unchanged");
	check(memcmp(inv, c->actor->inventory.items, sizeof inv) == 0, label);
	say(c, k, what, "refused: ZERO sounds");
	check(snd_n == 0, label);
	say(c, k, what, "refused: s->rand_src unchanged");
	check(memcmp(&rng, &s->rand_src, sizeof rng) == 0, label);
	say(c, k, what, "refused: other world + other inventory untouched");
	check(box_hash(c->ow, x, y, z) == other_world
			&& memcmp(other_inv, c->other->inventory.items, sizeof other_inv)
				== 0,
		label);
}

/* ---- the case matrix for one actor -------------------------------------- */
static void run_actor(struct ctx* c) {
	struct server_world* w = c->w;
	for(int k = 0; k < K_COUNT; k++) {
		/* Snow on each face: TOP, a HORIZONTAL (+x) and the BOTTOM. The
		 * horizontal neighbour sits on stone so the old offset path could
		 * place there -- the wrong cell. */
		static const enum side faces[] = {SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM};
		for(unsigned f = 0; f < sizeof faces / sizeof faces[0]; f++) {
			setup_begin();
			prop(w, k, CX, Y, CZ, BLOCK_SNOW);
			set_cell(w, CX + 1, Y - 1, CZ, BLOCK_STONE, 0);
			decoy(c->ow, CX, Y, CZ);
			setup_end();
			char what[48];
			snprintf(what, sizeof what, "snow %s face", side_name(faces[f]));
			expect_place(c, k, what, CX, Y, CZ, faces[f], CX, Y, CZ,
				SIDE_BOTTOM, true);
		}

		/* Offset cell OCCUPIED (stone above the snow): Beta never reads it. */
		setup_begin();
		prop(w, k, CX, Y, CZ, BLOCK_SNOW);
		set_cell(w, CX, Y + 1, CZ, BLOCK_STONE, 0);
		decoy(c->ow, CX, Y, CZ);
		setup_end();
		expect_place(c, k, "snow top, offset occupied", CX, Y, CZ, SIDE_TOP,
			CX, Y, CZ, SIDE_BOTTOM, true);

		/* Offset cell UNREADABLE, world height: snow at y 127, top face. */
		setup_begin();
		prop(w, k, CX, WORLD_HEIGHT - 1, CZ, BLOCK_SNOW);
		decoy(c->ow, CX, WORLD_HEIGHT - 1, CZ);
		setup_end();
		{
			struct block_data probe;
			assert(!get(w, CX, WORLD_HEIGHT, CZ, &probe));
		}
		expect_place(c, k, "snow top at y 127, offset unreadable", CX,
			WORLD_HEIGHT - 1, CZ, SIDE_TOP, CX, WORLD_HEIGHT - 1, CZ,
			SIDE_BOTTOM, true);

		/* Offset cell UNREADABLE, chunk edge: +x face of the last column of
		 * a loaded chunk whose +x neighbour chunk is not loaded. */
		setup_begin();
		prop(w, k, EDGE_X - 2, Y, CZ, BLOCK_AIR); /* clear inside the chunk */
		set_cell(w, EDGE_X, Y - 1, CZ, k == K_REED ? BLOCK_DIRT : BLOCK_STONE,
			0);
		if(k == K_REED)
			set_cell(w, EDGE_X - 1, Y - 1, CZ, BLOCK_WATER_STILL, 0);
		set_cell(w, EDGE_X, Y, CZ, BLOCK_SNOW, 0);
		set_cell(w, EDGE_X, Y + 1, CZ, BLOCK_AIR, 0);
		setup_end();
		{
			struct block_data probe;
			assert(!get(w, EDGE_X + 1, Y, CZ, &probe));
		}
		{
			const uint64_t other_edge = box_hash(c->ow, EDGE_X - 2, Y, CZ);
			hold(c->actor, k);
			place(c->s, c->actor, EDGE_X, Y, CZ, SIDE_RIGHT);
			struct block_data got = cell(w, EDGE_X, Y, CZ);
			say(c, k, "snow +x face at chunk edge, offset unreadable",
				"block placed IN the snow cell");
			check(got.type == kinds[k].block && got.metadata == kinds[k].meta,
				label);
			say(c, k, "snow +x face at chunk edge, offset unreadable",
				"onItemPlace got the bottom side, one STEP sound, one item");
			check(spy_calls == 1 && spy_side == SIDE_BOTTOM && snd_n == 1
					&& snd_last.payload.sound.sound_id == kinds[k].step
					&& snd_last.payload.sound.pos[0] == EDGE_X + 0.5F
					&& snd_last.payload.sound.pos[1] == Y + 0.5F
					&& snd_last.payload.sound.pos[2] == CZ + 0.5F
					&& c->actor->inventory.items[INVENTORY_SLOT_HOTBAR].count
						== kinds[k].count - 1,
				label);
			say(c, k, "snow +x face at chunk edge, offset unreadable",
				"other world untouched");
			check(box_hash(c->ow, EDGE_X - 2, Y, CZ) == other_edge, label);
		}

		/* Non-snow control: the top of the support -> the offset cell above,
		 * with the clicked side unchanged. */
		setup_begin();
		prop(w, k, CX, Y, CZ, BLOCK_AIR);
		decoy(c->ow, CX, Y, CZ);
		setup_end();
		expect_place(c, k, "non-snow control", CX, Y - 1, CZ, SIDE_TOP,
			CX, Y, CZ, SIDE_TOP, false);
	}

	/* Refusals. */
	setup_begin();
	prop(w, K_REED, CX, Y, CZ, BLOCK_SNOW);
	set_cell(w, CX, Y - 1, CZ, BLOCK_STONE, 0);       /* no dirt ... */
	set_cell(w, CX - 1, Y - 1, CZ, BLOCK_STONE, 0);   /* ... no water */
	decoy(c->ow, CX, Y, CZ);
	setup_end();
	expect_refusal(c, K_REED, "reed on snow over stone", CX, Y, CZ);

	setup_begin();
	prop(w, K_CAKE, CX, Y, CZ, BLOCK_SNOW);
	set_cell(w, CX, Y - 1, CZ, BLOCK_AIR, 0); /* no solid support */
	decoy(c->ow, CX, Y, CZ);
	setup_end();
	expect_refusal(c, K_CAKE, "cake on unsupported snow", CX, Y, CZ);

	/* Obstructed: the actor's own body stands in the snow cell
	 * (canBlockBePlacedAt -> checkIfAABBIsClear, nobody excluded). */
	setup_begin();
	prop(w, K_CAKE, CX, Y, CZ, BLOCK_SNOW);
	decoy(c->ow, CX, Y, CZ);
	setup_end();
	stand(c->actor, CX + 0.5, Y, CZ + 0.5);
	server_local_refresh_player_membership(c->s);
	expect_refusal(c, K_CAKE, "cake on snow, body in the cell", CX, Y, CZ);
	park(c->s);

	/* Creative: the held stack is restored after a snow placement. */
	c->s->creative_mode = true;
	for(int k = 0; k < K_COUNT; k++) {
		setup_begin();
		prop(w, k, CX, Y, CZ, BLOCK_SNOW);
		decoy(c->ow, CX, Y, CZ);
		setup_end();
		hold(c->actor, k);
		place(c->s, c->actor, CX, Y, CZ, SIDE_TOP);
		const struct item_data held
			= c->actor->inventory.items[INVENTORY_SLOT_HOTBAR];
		say(c, k, "creative snow top", "placed in the snow cell");
		check(cell(w, CX, Y, CZ).type == kinds[k].block, label);
		say(c, k, "creative snow top", "held stack restored");
		check(held.id == kinds[k].item && held.count == kinds[k].count
				&& held.durability == 0,
			label);
	}
	c->s->creative_mode = false;
}

int main(void) {
	harness_init();
	clin_test_set_consume_observer(observer, NULL);
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	s->render_demo_mob = -1;

	/* Second committed world (test_piston_world_isolation's setup). */
	struct server_world* nether = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(nether, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	nether->local = nether->session = s;
	nether->world_index = 1;
	s->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;

	setup_begin();
	for(unsigned wi = 0; wi < 2; wi++) {
		load_chunk(&s->worlds[wi], 0, 0);
		load_chunk(&s->worlds[wi], EDGE_CHUNK, 0);
	}
	setup_end();

	for(int k = 0; k < K_COUNT; k++) {
		struct item* def = items[kinds[k].item];
		assert(def && def->onItemPlace);
		spy_orig[k] = def->onItemPlace;
		def->onItemPlace = spy_place;
	}

	struct server_player* guest = s->guest_actor;
	for(unsigned swap = 0; swap < 2; swap++) {
		s->player.world_slot = swap;
		s->player.dimension = swap ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
		guest->world_slot = !swap;
		guest->dimension = swap ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
		park(s);
		struct ctx host = {s, &s->player, guest,
			server_local_world_of(s, &s->player),
			server_local_world_of(s, guest), "host"};
		struct ctx gst = {s, guest, &s->player, host.ow, host.w, "guest"};
		assert(host.w && host.ow && host.w != host.ow);
		run_actor(&host);
		run_actor(&gst);
	}

	for(int k = 0; k < K_COUNT; k++)
		items[kinds[k].item]->onItemPlace = spy_orig[k];
	s->player.world_slot = guest->world_slot = 0;
	s->player.dimension = guest->dimension = WORLD_DIM_OVERWORLD;
	s->world_slot_state[1] = SERVER_WORLD_SLOT_EMPTY;
	server_world_discard(nether);
	teardown(s);
	return harness_result(TEST_NAME, "reed/cake/repeater on snow");
}
