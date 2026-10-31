/*
	Beta 1.7.3 dispenser (dispenser-slice) goldens, driven over a live
	superflat server_world -- the door/button/TNT env-test harness pattern.

	Ground truth (decompile 740c583, cited again at each assertion below):
	  BlockDispenser.dispenseItem / .onNeighborBlockChange / .updateTick /
	  .onBlockRemoval; TileEntityDispenser.getRandomStackFromInventory;
	  EntityArrow.setArrowHeading / EntityEgg.setEggHeading (byte-identical).

	Every RNG-sensitive expected value in this file was hand-derived from an
	INDEPENDENT float32 reference model of source/util.c's xorshift32 +
	Java-polar-method nextGaussian algorithm (a small numpy script, NOT this
	project's C implementation) -- never by reading back this slice's own
	dispenser code. See DISPENSER_SLICE_NOTES.md for the derivation.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "block/dispenser_logic.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/server_dispenser.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/audio.h" // SOUND_RANDOM_CLICK/SOUND_RANDOM_BOW
#include "platform/thread.h"
#include "util.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_dispenser_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y 101  // circuit / dispenser plane

static struct server_local s;
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static bool feq(float a, float b) { return fabsf(a - b) < 1.0e-5f; }
static bool feq_tol(float a, float b, float tol) { return fabsf(a - b) < tol; }

static void set_block(int x, int y, int z, int type, int meta) {
	bool ok = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
	if(!ok) // stdout is fully buffered under a pipe; name the site on stderr
		fprintf(stderr, "set_block FAILED at (%d,%d,%d) type=%d\n", x, y, z,
				type);
	assert(ok && "server_world_set_block refused the edit");
}
static int get_meta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? (b.metadata & 0x0F)
														  : -1;
}
static void tick(void) { server_world_tick(&s.worlds[0], &s); }

// Place a dispenser at (x,y,z) with the given facing metadata, register its
// TE, and return the TE pointer (never NULL -- asserts on registration
// failure, matching every other fixture helper in this suite).
static struct dispenser_te* place_dispenser(int x, int y, int z, int meta) {
	set_block(x, y, z, BLOCK_DISPENSER, meta);
	int idx = dispenser_te_register(&s, x, y, z);
	assert(idx >= 0);
	struct dispenser_te* d = dispenser_te_lookup(&s, x, y, z);
	assert(d);
	return d;
}

// --- RPC capture -------------------------------------------------------------

static int g_spawn_item_count;
static struct client_rpc g_last_spawn_item;
static int g_spawn_arrow_count;
static struct client_rpc g_last_spawn_arrow;
static int g_smoke_puff_count;
static int g_sound_count;
static struct client_rpc g_last_sound;

static void reset_capture(void) {
	g_spawn_item_count = 0;
	g_spawn_arrow_count = 0;
	g_smoke_puff_count = 0;
	g_sound_count = 0;
	memset(&g_last_spawn_item, 0, sizeof(g_last_spawn_item));
	memset(&g_last_spawn_arrow, 0, sizeof(g_last_spawn_arrow));
	memset(&g_last_sound, 0, sizeof(g_last_sound));
}

static void capture_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	switch(call->type) {
		case CRPC_SPAWN_ITEM:
			g_spawn_item_count++;
			g_last_spawn_item = *call;
			break;
		case CRPC_SPAWN_ARROW:
			g_spawn_arrow_count++;
			g_last_spawn_arrow = *call;
			break;
		case CRPC_SMOKE_PUFF: g_smoke_puff_count++; break;
		case CRPC_SOUND:
			g_sound_count++;
			g_last_sound = *call;
			break;
		default: break;
	}
}

// ============================================================================
// 1. Direction table (pure -- BlockDispenser.dispenseItem's dx/dz switch).
// ============================================================================
static void test_direction_table(void) {
	int dx, dz;

	dispenser_direction(3, &dx, &dz); // meta==3 -> dz=+1
	assert(dx == 0 && dz == 1);
	dispenser_direction(2, &dx, &dz); // meta==2 -> dz=-1
	assert(dx == 0 && dz == -1);
	dispenser_direction(5, &dx, &dz); // meta==5 -> dx=+1
	assert(dx == 1 && dz == 0);
	dispenser_direction(0, &dx, &dz); // else (meta==0) -> dx=-1
	assert(dx == -1 && dz == 0);
	dispenser_direction(1, &dx, &dz); // else (meta==1) -- the catch-all is NOT
	assert(dx == -1 && dz == 0);	  // special-cased to meta==0 only
	dispenser_direction(7, &dx, &dz); // else (an out-of-range value too)
	assert(dx == -1 && dz == 0);

	// Spawn point: sx=x+dx*0.6+0.5, sy=y+0.5, sz=z+dz*0.6+0.5, at (10,20,30).
	float sx, sy, sz;
	dispenser_spawn_point(10, 20, 30, 0, 1, &sx, &sy, &sz); // meta 3
	assert(feq(sx, 10.5F) && feq(sy, 20.5F) && feq(sz, 31.1F));
	dispenser_spawn_point(10, 20, 30, 0, -1, &sx, &sy, &sz); // meta 2
	assert(feq(sx, 10.5F) && feq(sy, 20.5F) && feq(sz, 29.9F));
	dispenser_spawn_point(10, 20, 30, 1, 0, &sx, &sy, &sz); // meta 5
	assert(feq(sx, 11.1F) && feq(sy, 20.5F) && feq(sz, 30.5F));
	dispenser_spawn_point(10, 20, 30, -1, 0, &sx, &sy, &sz); // else
	assert(feq(sx, 9.9F) && feq(sy, 20.5F) && feq(sz, 30.5F));

	printf("  direction table: meta 2/3/5 + else-catchall, spawn points OK\n");
}

// ============================================================================
// 2. Reservoir pick (pure -- TileEntityDispenser.getRandomStackFromInventory).
//    Golden seed=1, occupied = {0,2,3,6,8} of 9 -> sel=6 (independently
//    hand-derived: draws are nextInt(1)=0@i0, nextInt(2)=0@i2, nextInt(3)=1@i3,
//    nextInt(4)=0@i6, nextInt(5)=2@i8 -- n advances ONLY on occupied slots).
// ============================================================================
static void test_reservoir_pick(void) {
	bool occupied[DISPENSER_TE_SLOTS]
		= {true, false, true, true, false, false, true, false, true};
	struct random_gen rng = {.seed = 1};
	int sel = dispenser_reservoir_select(occupied, &rng);
	assert(sel == 6);

	// Single occupied slot: nextInt(1) is ALWAYS 0, so the sole slot is
	// selected regardless of the RNG state -- proven with two very different
	// seeds.
	bool single[DISPENSER_TE_SLOTS] = {false, false, false, false, true,
									   false, false, false, false};
	struct random_gen rng2 = {.seed = 1};
	assert(dispenser_reservoir_select(single, &rng2) == 4);
	struct random_gen rng3 = {.seed = 999999};
	assert(dispenser_reservoir_select(single, &rng3) == 4);

	// Empty: no draw is EVER made (the loop body never executes), so the RNG
	// state is untouched and sel is -1.
	bool empty[DISPENSER_TE_SLOTS] = {0};
	struct random_gen rng4 = {.seed = 12345};
	assert(dispenser_reservoir_select(empty, &rng4) == -1);
	assert(rng4.seed == 12345); // zero draws

	printf("  reservoir pick: seed=1 multi-slot -> slot 6; single-item -> "
		  "always selected; empty -> -1, 0 draws OK\n");
}

// The full getRandomStackFromInventory: pick + Beta decrStackSize(sel,1).
static void test_reservoir_decrement(void) {
	struct dispenser_te d = {0};
	d.dispenser_random = (struct random_gen) {.seed = 1};
	d.items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};
	d.items[2] = (struct item_data) {.id = ITEM_ARROW, .durability = 0, .count = 3};
	d.items[3] = (struct item_data) {.id = ITEM_EGG, .durability = 0, .count = 1};
	// Slot 6 (the golden-picked slot) carries 5, so the decrement (5->4) is
	// distinguishable from "the slot went empty".
	d.items[6] = (struct item_data) {.id = BLOCK_DIRT, .durability = 0, .count = 5};
	d.items[8] = (struct item_data) {.id = ITEM_SNOW_BALL, .durability = 0, .count = 2};

	struct item_data out = {0};
	int slot = -1;
	bool got = dispenser_take_random_stack(&d, &out, &slot);
	assert(got);
	assert(slot == 6);
	assert(out.id == BLOCK_DIRT && out.durability == 0 && out.count == 1);
	assert(d.items[6].count == 4 && d.items[6].id == BLOCK_DIRT); // decremented, not emptied
	// every other slot untouched
	assert(d.items[0].count == 1 && d.items[2].count == 3
		  && d.items[3].count == 1 && d.items[8].count == 2);

	// D4 restore: undo the take exactly -- the partially-decremented slot
	// goes back to its pre-take count (4 -> 5), no id/durability change.
	dispenser_restore_stack(&d, slot, out);
	assert(d.items[6].count == 5 && d.items[6].id == BLOCK_DIRT
		  && d.items[6].durability == 0);
	assert(d.items[0].count == 1 && d.items[2].count == 3
		  && d.items[3].count == 1 && d.items[8].count == 2); // still untouched

	// Emptying case: a count==1 slot is the ONLY occupied slot -> selected,
	// decremented to the null ItemStack (id cleared too, matching Beta's
	// dispenserContents[sel]=null branch of decrStackSize).
	struct dispenser_te d2 = {0};
	d2.dispenser_random = (struct random_gen) {.seed = 42};
	d2.items[4] = (struct item_data) {.id = ITEM_EGG, .durability = 0, .count = 1};
	struct item_data out2 = {0};
	int slot2 = -1;
	assert(dispenser_take_random_stack(&d2, &out2, &slot2));
	assert(slot2 == 4);
	assert(out2.id == ITEM_EGG && out2.count == 1);
	assert(d2.items[4].id == 0 && d2.items[4].count == 0);

	// D4 restore from fully-empty: the null ItemStack comes back to life
	// verbatim (id/durability/count=1), matching Beta having never actually
	// lost it.
	dispenser_restore_stack(&d2, slot2, out2);
	assert(d2.items[4].id == ITEM_EGG && d2.items[4].durability == 0
		  && d2.items[4].count == 1);

	// Empty dispenser: no stack to take.
	struct dispenser_te d3 = {0};
	d3.dispenser_random = (struct random_gen) {.seed = 7};
	struct item_data out3 = {.id = 0xFFFF}; // sentinel: must stay untouched
	int slot3 = -1;
	assert(!dispenser_take_random_stack(&d3, &out3, &slot3));
	assert(out3.id == 0xFFFF);
	assert(slot3 == -1); // *out_slot also untouched on the empty path

	printf("  reservoir decrement: selected stack count==1, source slot -1, "
		  "empties to null when exhausted, restore undoes it exactly OK\n");
}

// ============================================================================
// 3. Empty dispenser -> SFX 1001 ("random.click" pitch 1.2) at the raw block
//    coords, NO entity, NO 2000 (CRPC_SMOKE_PUFF) event.
// ============================================================================
static void test_empty_dispenser_sfx(void) {
	struct dispenser_te* d = place_dispenser(10, Y, 10, 5); // meta 5: dx=1
	memset(d->items, 0, sizeof(d->items));
	d->dispenser_random = (struct random_gen) {.seed = 1};

	reset_capture();
	server_local_dispenser_dispense(&s, 10, Y, 10, 5);

	assert(g_spawn_item_count == 0);
	assert(g_spawn_arrow_count == 0);
	assert(g_smoke_puff_count == 0); // no 2000 event on the empty-click path
	assert(g_sound_count == 1);
	assert(g_last_sound.payload.sound.sound_id == SOUND_RANDOM_CLICK);
	assert(feq(g_last_sound.payload.sound.vol, 1.0F));
	assert(feq(g_last_sound.payload.sound.pitch, 1.2F));
	// Beta playAuxSFX(1001,x,y,z,0): the RAW block coords, no +0.5 centering.
	assert(feq(g_last_sound.payload.sound.pos[0], 10.0F));
	assert(feq(g_last_sound.payload.sound.pos[1], (float)Y));
	assert(feq(g_last_sound.payload.sound.pos[2], 10.0F));

	printf("  empty dispenser: SFX 1001 (click, pitch 1.2) at raw block "
		  "coords, no entity, no 2000 OK\n");
}

// ============================================================================
// 4. Generic-item eject: ONE nextDouble then THREE nextGaussians (X,Y,Z), on
//    s.rand_src. Golden seed=1: draw=6.294...e-5, speed=0.200006294,
//    gaussians (gx,gy,gz) = (-0.121664159, 0.029245486, -0.755511102).
// ============================================================================
static void test_generic_item_rng_order(void) {
	// dx=1,dz=0 (meta 5): motion (0.19453141, 0.20131604, -0.03399799),
	// spawned at (sx, sy-0.3, sz) = (x+1.1, y+0.2, z+0.5).
	{
		struct dispenser_te* d = place_dispenser(20, Y, 20, 5);
		memset(d->items, 0, sizeof(d->items));
		d->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 55}; // single slot: irrelevant
		s.rand_src = (struct random_gen) {.seed = 1};

		reset_capture();
		server_local_dispenser_dispense(&s, 20, Y, 20, 5);

		assert(g_spawn_item_count == 1);
		assert(g_spawn_arrow_count == 0);
		assert(g_smoke_puff_count == 1); // the 2000 event on a successful eject
		struct client_rpc* r = &g_last_spawn_item;
		assert(r->payload.spawn_item.item.id == ITEM_STICK);
		assert(r->payload.spawn_item.item.count == 1);
		assert(feq_tol(r->payload.spawn_item.pos[0], 20.0F + 1.1F, 1.0e-4f));
		assert(feq_tol(r->payload.spawn_item.pos[1], (float)Y + 0.2F, 1.0e-4f));
		assert(feq_tol(r->payload.spawn_item.pos[2], 20.5F, 1.0e-4f));
		assert(feq_tol(r->payload.spawn_item.vel[0], 0.19453141F, 1.0e-4f));
		assert(feq_tol(r->payload.spawn_item.vel[1], 0.20131604F, 1.0e-4f));
		assert(feq_tol(r->payload.spawn_item.vel[2], -0.03399799F, 1.0e-4f));
	}

	// The else-branch catch-all direction (dx=-1,dz=0), SAME seed: only the
	// X sign should flip in the pre-gaussian speed term (motion X=-0.20548117),
	// Y/Z unaffected by direction (dz=0 either way).
	{
		struct dispenser_te* d = place_dispenser(21, Y, 20, 0); // meta 0: else
		memset(d->items, 0, sizeof(d->items));
		d->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 55};
		s.rand_src = (struct random_gen) {.seed = 1};

		reset_capture();
		server_local_dispenser_dispense(&s, 21, Y, 20, 0);

		assert(g_spawn_item_count == 1);
		struct client_rpc* r = &g_last_spawn_item;
		assert(feq_tol(r->payload.spawn_item.vel[0], -0.20548117F, 1.0e-4f));
		assert(feq_tol(r->payload.spawn_item.vel[1], 0.20131604F, 1.0e-4f));
		assert(feq_tol(r->payload.spawn_item.vel[2], -0.03399799F, 1.0e-4f));
	}

	printf("  generic item eject: one-nextDouble-then-three-nextGaussian "
		  "order pinned (seed=1, both direction branches) OK\n");
}

// ============================================================================
// 5. Arrow / egg / snowball: right entity, right heading formula
//    (normalize(dx,0.1,dz) + 3 gaussians*0.0075*6.0, scaled by 1.1).
// ============================================================================
static void test_projectile_spawns(void) {
	// Arrow: server_local_spawn_arrow only QUEUES (server_local_flush_arrows
	// runs from the entity-tick pass) -- inspect the queue entry directly
	// rather than driving a full tick (keeps s.rand_src untouched by anything
	// else).
	{
		struct dispenser_te* d = place_dispenser(30, Y, 30, 5); // dx=1,dz=0
		memset(d->items, 0, sizeof(d->items));
		d->items[0] = (struct item_data) {.id = ITEM_ARROW, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 3};
		s.rand_src = (struct random_gen) {.seed = 1};
		s.pending_arrow_count = 0;

		reset_capture();
		server_local_dispenser_dispense(&s, 30, Y, 30, 5);

		assert(s.pending_arrow_count == 1);
		struct pending_arrow* pa = &s.pending_arrows[0];
		assert(pa->item_id == ITEM_ARROW);
		assert(pa->damage == ARROW_DAMAGE);
		assert(pa->from_player == true); // Beta doesArrowBelongToPlayer=true
		assert(pa->owner_id == DISPENSER_PROJECTILE_OWNER_SENTINEL);
		// I02: the spread is the NEW arrow's own Random (CFR BlockDispenser
		// .java:126-129 -> EntityArrow.setArrowHeading), drawn at the flush
		// once the arrow has its id/stream -- the queue carries the aim.
		assert(pa->own_spread && !pa->spread_advance && pa->spread_discard == 0);
		assert(feq(pa->spread_aim[0], 1.0F) && feq(pa->spread_aim[1], 0.1F)
			   && feq(pa->spread_aim[2], 0.0F));
		assert(feq(pa->spread_speed, 1.1F) && feq(pa->spread_deviation, 6.0F));
		assert(s.rand_src.seed == 1); // the world stream is not drawn
		assert(g_spawn_arrow_count == 0); // not published until flush
		server_local_test_flush_arrows(&s);
		assert(g_spawn_arrow_count == 1);
		// Heading: dx=1 dominates (X > 0), Y small positive (0.1 base +
		// jitter), Z near 0 (jitter only) -- speed magnitude close to 1.1F.
		// The exact own-stream value is pinned in test_entity_rng_streams (f).
		const float* av = g_last_spawn_arrow.payload.spawn_arrow.vel;
		float mag = sqrtf(av[0] * av[0] + av[1] * av[1] + av[2] * av[2]);
		assert(feq_tol(mag, 1.1F, 0.15F)); /* I02: ~3 sigma of 6.0-deviation spread (own-stream exact value: test_entity_rng_streams) */
		assert(av[0] > 0.9F); // dx=1 still clearly dominant post-jitter
		assert(g_smoke_puff_count == 1);
		assert(g_sound_count == 1);
		assert(g_last_sound.payload.sound.sound_id == SOUND_RANDOM_BOW);
		assert(feq(g_last_sound.payload.sound.pitch, 1.2F)); // aux-1002 fixed pitch

		printf("  arrow: queued with owner sentinel, from_player=true, "
			  "damage=ARROW_DAMAGE, heading dx-dominant |vel|~1.1 OK\n");
	}

	// Egg: direct construction (no queue) -> a real entity + CRPC_SPAWN_ARROW
	// (the engine's documented item-billboard stand-in for a thrown egg).
	{
		struct dispenser_te* d = place_dispenser(31, Y, 30, 3); // dz=1
		memset(d->items, 0, sizeof(d->items));
		d->items[0] = (struct item_data) {.id = ITEM_EGG, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 3};
		s.rand_src = (struct random_gen) {.seed = 1};

		reset_capture();
		size_t before = dict_entity_size(s.entities);
		server_local_dispenser_dispense(&s, 31, Y, 30, 3);

		assert(dict_entity_size(s.entities) == before + 1);
		assert(g_spawn_arrow_count == 1);
		assert(g_last_spawn_arrow.payload.spawn_arrow.item_id == ITEM_EGG);
		float mag = sqrtf(
			g_last_spawn_arrow.payload.spawn_arrow.vel[0]
				* g_last_spawn_arrow.payload.spawn_arrow.vel[0]
			+ g_last_spawn_arrow.payload.spawn_arrow.vel[1]
				* g_last_spawn_arrow.payload.spawn_arrow.vel[1]
			+ g_last_spawn_arrow.payload.spawn_arrow.vel[2]
				* g_last_spawn_arrow.payload.spawn_arrow.vel[2]);
		assert(feq_tol(mag, 1.1F, 0.15F)); /* I02: ~3 sigma of 6.0-deviation spread (own-stream exact value: test_entity_rng_streams) */
		assert(g_last_spawn_arrow.payload.spawn_arrow.vel[2] > 0.9F); // dz=1 dominant

		printf("  egg: real entity spawned, CRPC item_id=ITEM_EGG, heading "
			  "dz-dominant |vel|~1.1 OK\n");
	}

	// Snowball: identical shape to the egg (entity_snowball).
	{
		struct dispenser_te* d = place_dispenser(32, Y, 30, 2); // dz=-1
		memset(d->items, 0, sizeof(d->items));
		d->items[0]
			= (struct item_data) {.id = ITEM_SNOW_BALL, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 3};
		s.rand_src = (struct random_gen) {.seed = 1};

		reset_capture();
		size_t before = dict_entity_size(s.entities);
		server_local_dispenser_dispense(&s, 32, Y, 30, 2);

		assert(dict_entity_size(s.entities) == before + 1);
		assert(g_spawn_arrow_count == 1);
		assert(g_last_spawn_arrow.payload.spawn_arrow.item_id == ITEM_SNOW_BALL);
		assert(g_last_spawn_arrow.payload.spawn_arrow.vel[2] < -0.9F); // dz=-1 dominant

		printf("  snowball: real entity spawned, CRPC item_id=ITEM_SNOW_BALL, "
			  "heading dz-dominant OK\n");
	}
}

// ============================================================================
// 6. Redstone: powered at y fires; powered at y+1 ALSO fires; power removed
//    before the scheduled tick lands does NOT dispense.
// ============================================================================
static void test_redstone_trigger(void) {
	// --- powered at (x,y,z) itself: a floor lever adjacent to the dispenser.
	{
		int x = 40, y = Y, z = 40;
		set_block(x, y - 1, z, BLOCK_STONE, 0); // solid floor under everything
		struct dispenser_te* d = place_dispenser(x, y, z, 5); // dx=1,dz=0
		d->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 1};
		s.rand_src = (struct random_gen) {.seed = 1};

		set_block(x + 1, y - 1, z, BLOCK_STONE, 0); // mount for the floor lever
		set_block(x + 1, y, z, BLOCK_LEVER, 5);	 // floor lever, OFF
		struct block_data lb;
		struct block_info lever_info = {.block = &lb, .x = x + 1, .y = y, .z = z};
		assert(server_world_get_block(&s.worlds[0], x + 1, y, z, &lb));
		block_lever.onRightClick(&s, &s.player, NULL, &lever_info, &lever_info, SIDE_TOP);
		assert((get_meta(x + 1, y, z) & 8) != 0); // lever really ON

		int before = d->items[0].count;
		for(int i = 0; i < block_dispenser.tick_delay; i++)
			tick(); // let the scheduled tickRate()==4 delay elapse
		assert(d->items[0].count == before - 1); // it fired

		printf("  redstone: powered at (x,y,z) itself -> fires after "
			  "tickRate()==4 OK\n");
	}

	// --- powered at (x,y+1,z): a wall lever mounted on a pillar directly
	// above the dispenser (the pillar IS the (x,y+1,z) cell).
	{
		int x = 45, y = Y, z = 40;
		set_block(x, y - 1, z, BLOCK_STONE, 0);
		struct dispenser_te* d = place_dispenser(x, y, z, 5);
		d->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 1};
		s.rand_src = (struct random_gen) {.seed = 1};

		set_block(x, y + 1, z, BLOCK_STONE, 0); // the (x,y+1,z) cell itself
		// Lever at (x+1,y+1,z), meta=1 (mount = lever_x-1 = x = the pillar).
		set_block(x + 1, y + 1, z, BLOCK_LEVER, 1);
		struct block_data lb2;
		struct block_info lever_info2
			= {.block = &lb2, .x = x + 1, .y = y + 1, .z = z};
		assert(server_world_get_block(&s.worlds[0], x + 1, y + 1, z, &lb2));
		block_lever.onRightClick(&s, &s.player, NULL, &lever_info2, &lever_info2, SIDE_TOP);
		assert((get_meta(x + 1, y + 1, z) & 8) != 0);

		int before = d->items[0].count;
		for(int i = 0; i < block_dispenser.tick_delay; i++)
			tick();
		assert(d->items[0].count == before - 1);

		printf("  redstone: powered at (x,y+1,z) ALSO fires OK\n");
	}

	// --- power removed before the scheduled tick lands: no dispense.
	{
		int x = 50, y = Y, z = 40;
		set_block(x, y - 1, z, BLOCK_STONE, 0);
		struct dispenser_te* d = place_dispenser(x, y, z, 5);
		d->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 1};
		s.rand_src = (struct random_gen) {.seed = 1};

		set_block(x + 1, y - 1, z, BLOCK_STONE, 0);
		set_block(x + 1, y, z, BLOCK_LEVER, 5);
		struct block_data lb3;
		struct block_info lever_info3 = {.block = &lb3, .x = x + 1, .y = y, .z = z};
		assert(server_world_get_block(&s.worlds[0], x + 1, y, z, &lb3));
		block_lever.onRightClick(&s, &s.player, NULL, &lever_info3, &lever_info3, SIDE_TOP);
		assert((get_meta(x + 1, y, z) & 8) != 0); // ON -> the tick got scheduled

		// Flip it OFF again before the tickRate()==4 delay elapses.
		assert(server_world_get_block(&s.worlds[0], x + 1, y, z, &lb3));
		struct block_info lever_info3b
			= {.block = &lb3, .x = x + 1, .y = y, .z = z};
		block_lever.onRightClick(&s, &s.player, NULL, &lever_info3b, &lever_info3b, SIDE_TOP);
		assert((get_meta(x + 1, y, z) & 8) == 0); // OFF again

		int before = d->items[0].count;
		for(int i = 0; i < block_dispenser.tick_delay; i++)
			tick();
		assert(d->items[0].count == before); // updateTick's re-check: NO dispense

		printf("  redstone: power removed before the scheduled tick lands -> "
			  "NO dispense (updateTick re-check) OK\n");
	}
}

// ============================================================================
// 7. Inventory survives a save/evict/reload round-trip.
// ============================================================================
static void test_persistence_roundtrip(void) {
	int x = 60, y = Y, z = 60;
	struct dispenser_te* d = place_dispenser(x, y, z, 5);
	d->items[0] = (struct item_data) {.id = ITEM_ARROW, .durability = 0, .count = 7};
	d->items[6] = (struct item_data) {.id = BLOCK_DIRT, .durability = 0, .count = 42};

	// x,z are both small and positive in this fixture, so plain division IS
	// floor division here (avoids pulling chunk.h's WCOORD_CHUNK_OFFSET into
	// this test only for that).
	w_coord_t cx = (w_coord_t)(x / 16), cz = (w_coord_t)(z / 16);
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true /* erase */, cx, cz));

	// Evicted: no longer resident.
	assert(dispenser_te_lookup(&s, x, y, z) == NULL);

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	struct dispenser_te* reloaded = dispenser_te_lookup(&s, x, y, z);
	assert(reloaded);
	assert(reloaded->items[0].id == ITEM_ARROW && reloaded->items[0].count == 7);
	assert(reloaded->items[6].id == BLOCK_DIRT && reloaded->items[6].count == 42);
	for(int k = 1; k < DISPENSER_TE_SLOTS; k++)
		if(k != 6)
			assert(reloaded->items[k].id == 0);
	// dispenser_random must have been reseeded off zero by the load fixup
	// (Beta's dispenserRandom is a fresh `new Random()` on every
	// construction, chunk load included -- never persisted).
	assert(reloaded->dispenser_random.seed != 0);

	// The block itself survived too, facing metadata intact.
	assert(get_meta(x, y, z) == 5);

	printf("  persistence: 9-slot inventory + facing survive a real "
		  "save/evict/reload round trip; dispenser_random reseeded OK\n");
}

// ============================================================================
// 7b. D12: a CONTENTS-ONLY edit (no block change, chunk not marked modified)
//     must survive eviction. TE content mutations never set c->modified; the
//     save predicate's TE-kind enumeration is the only thing forcing the
//     write, and it omitted dispensers -- the edit below was silently dropped
//     and the STALE pre-edit contents survived on disk.
// ============================================================================
static void test_persistence_contents_only_edit(void) {
	// (12, Y, 44) = chunk (0,2): preloaded by main() and -- unlike (70,*,70),
	// which the window-sync case also builds on -- used by NO other case, so
	// this case's save/erase/reload cycling of the chunk cannot invalidate a
	// later fixture's assumptions about resident, still-pristine terrain.
	int x = 12, y = Y, z = 44;
	struct dispenser_te* d = place_dispenser(x, y, z, 5);
	d->items[0]
		= (struct item_data) {.id = ITEM_ARROW, .durability = 0, .count = 7};
	d->items[6]
		= (struct item_data) {.id = BLOCK_DIRT, .durability = 0, .count = 42};

	w_coord_t cx = (w_coord_t)(x / 16), cz = (w_coord_t)(z / 16);
	// Save #1 rides the block-place modified bit; the reload starts clean.
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true /* erase */, cx, cz));
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	assert(!sc->modified && "fixture: a freshly reloaded chunk must be clean");

	struct dispenser_te* live = dispenser_te_lookup(&s, x, y, z);
	assert(live && live->items[0].count == 7);

	// The contents-only edit: a partial dispense + one stack removed. No block
	// changes -- and the chunk must still be clean afterwards, or this test
	// would be riding the modified bit instead of the TE-kind predicate that
	// D12 is about.
	live->items[0].count = 3;
	live->items[6] = (struct item_data) {0};
	assert(!sc->modified
		   && "fixture: a contents-only TE edit must not set c->modified");

	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true /* erase */, cx, cz));
	assert(dispenser_te_lookup(&s, x, y, z) == NULL);
	assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	struct dispenser_te* re = dispenser_te_lookup(&s, x, y, z);
	assert(re);
	assert(re->items[0].id == ITEM_ARROW && re->items[0].count == 3
		   && "D12: contents-only edit lost on eviction");
	assert(re->items[6].id == 0
		   && "D12: cleared slot resurrected from stale disk contents");

	printf("  persistence: CONTENTS-ONLY edit (clean chunk) survives "
		   "evict/reload -- the TE-kind save predicate covers dispensers\n");
}

// ============================================================================
// Post-review (adversarial audit, 2026-08-13): D1, D3, D4, D6 goldens.
// ============================================================================

// D1: a dispense landing while the dispenser's own GUI is open must push
// live to the window (not just the TE), and closing an UNTOUCHED window
// afterward must not resurrect the ejected item (the old bug: inv_on_close's
// unconditional write-back stomped the TE with the window's stale
// open-time snapshot).
static void test_window_sync_no_duplication(void) {
	int x = 70, y = Y, z = 70;
	set_block(x, y - 1, z, BLOCK_STONE, 0);
	struct dispenser_te* d = place_dispenser(x, y, z, 5); // dx=1,dz=0
	memset(d->items, 0, sizeof(d->items));
	d->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 5};
	d->dispenser_random = (struct random_gen) {.seed = 1};
	s.rand_src = (struct random_gen) {.seed = 1};

	s.player.active_inventory = &s.player.inventory; // no window open yet
	assert(server_local_open_inventory(&s, &inventory_logic_dispenser,
									   DISPENSER_SIZE, x, y, z,
									   WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER));
	struct inventory* win = s.player.active_inventory;
	assert(win && win != &s.player.inventory);
	struct item_data seen;
	assert(inventory_get_slot(win, DISPENSER_SLOT_STORAGE + 0, &seen));
	assert(seen.id == ITEM_STICK && seen.count == 5); // pre-dispense snapshot

	reset_capture();
	server_local_dispenser_dispense(&s, x, y, z, 5); // a "redstone fired while GUI open" event

	// D1: the OPEN window already reflects the dispense (live push) --
	// before this fix the window kept showing count==5 until closed/reopened.
	assert(inventory_get_slot(win, DISPENSER_SLOT_STORAGE + 0, &seen));
	assert(seen.id == ITEM_STICK && seen.count == 4);
	assert(d->items[0].count == 4); // the TE agrees

	// Close the window WITHOUT the player touching any slot -- exactly the
	// repro: inv_on_close's write-back must not stomp the TE with a stale
	// pre-dispense snapshot (there isn't one anymore; D1 kept them in sync).
	assert(server_local_test_close_active_inventory(&s));
	assert(s.player.active_inventory == &s.player.inventory);
	assert(d->items[0].count == 4 && d->items[0].id == ITEM_STICK); // NOT resurrected to 5

	printf("  D1: dispense while GUI open pushes live to the window; closing "
		  "an untouched window does not resurrect the ejected item OK\n");
}

// D4: a spawn/queue capacity failure must restore the taken item (not lose
// it) and must not play the success SFX/2000 event it did not earn.
static void test_capacity_exhaustion_preserves_item(void) {
	// Arrow: fill the pending-arrows queue so the capacity check
	// deterministically fails.
	{
		int x = 71, y = Y, z = 70;
		struct dispenser_te* d = place_dispenser(x, y, z, 5);
		memset(d->items, 0, sizeof(d->items));
		d->items[0] = (struct item_data) {.id = ITEM_ARROW, .durability = 0, .count = 3};
		d->dispenser_random = (struct random_gen) {.seed = 1};
		s.rand_src = (struct random_gen) {.seed = 1};
		int saved_count = s.pending_arrow_count;
		s.pending_arrow_count = MAX_PENDING_ARROWS; // force capacity failure

		reset_capture();
		server_local_dispenser_dispense(&s, x, y, z, 5);

		assert(d->items[0].count == 3 && d->items[0].id == ITEM_ARROW); // restored, not lost
		assert(g_spawn_arrow_count == 0 && g_spawn_item_count == 0);
		assert(g_smoke_puff_count == 0); // no 2000 event -- nothing actually dispensed
		assert(g_sound_count == 1);
		assert(g_last_sound.payload.sound.sound_id == SOUND_RANDOM_CLICK);
		assert(feq(g_last_sound.payload.sound.pitch, 1.2F)); // the empty-click fallback, NOT the bow success cue
		assert(s.pending_arrow_count == MAX_PENDING_ARROWS); // untouched by the failed attempt

		s.pending_arrow_count = saved_count;
		printf("  D4 (arrow): pending-arrow queue exhaustion preserves the "
			  "item, plays SFX 1001 (not the bow cue), no 2000 event OK\n");
	}

	// Generic item: force the next entity allocation to fail.
	{
		int x = 72, y = Y, z = 70;
		struct dispenser_te* d = place_dispenser(x, y, z, 5);
		memset(d->items, 0, sizeof(d->items));
		d->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 3};
		d->dispenser_random = (struct random_gen) {.seed = 1};
		s.rand_src = (struct random_gen) {.seed = 1};

		server_local_test_fail_next_entity_alloc();
		reset_capture();
		server_local_dispenser_dispense(&s, x, y, z, 5);

		assert(d->items[0].count == 3 && d->items[0].id == ITEM_STICK); // restored, not lost
		assert(g_spawn_item_count == 0);
		assert(g_smoke_puff_count == 0);
		assert(g_sound_count == 1);
		assert(g_last_sound.payload.sound.sound_id == SOUND_RANDOM_CLICK);
		// The empty-click fallback pitch (1.2), NOT the generic-item success
		// pitch (1.0) -- proves this took the failure path, not the success one.
		assert(feq(g_last_sound.payload.sound.pitch, 1.2F));

		printf("  D4 (generic item): entity-alloc failure preserves the "
			  "item, plays SFX 1001, no 2000 event OK\n");
	}

	// Egg: same shape, force allocation failure again.
	{
		int x = 73, y = Y, z = 70;
		struct dispenser_te* d = place_dispenser(x, y, z, 5);
		memset(d->items, 0, sizeof(d->items));
		d->items[0] = (struct item_data) {.id = ITEM_EGG, .durability = 0, .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 1};
		s.rand_src = (struct random_gen) {.seed = 1};

		server_local_test_fail_next_entity_alloc();
		reset_capture();
		size_t before = dict_entity_size(s.entities);
		server_local_dispenser_dispense(&s, x, y, z, 5);

		assert(dict_entity_size(s.entities) == before); // no entity leaked in
		assert(d->items[0].count == 1 && d->items[0].id == ITEM_EGG); // restored
		assert(g_spawn_arrow_count == 0);
		assert(g_smoke_puff_count == 0);
		assert(g_sound_count == 1);
		assert(g_last_sound.payload.sound.sound_id == SOUND_RANDOM_CLICK);
		assert(feq(g_last_sound.payload.sound.pitch, 1.2F));

		printf("  D4 (egg): entity-alloc failure preserves the item, plays "
			  "SFX 1001, no 2000 event, no orphan entity OK\n");
	}
}

// D6: a break with the dispenser's GUI OPEN must scatter its contents via
// the SAME Beta-faithful algorithm (server_local_dispenser_scatter_contents)
// as a break with the GUI closed -- not the generic one-stack-per-slot model
// every other container type uses. Pin the RNG and compare entity counts +
// the first spawned item's exact position/velocity between the two cases.
static void test_window_open_break_matches_closed_break(void) {
	// Same (x,y,z) for both cases, run SEQUENTIALLY -- the scatter's spawn
	// positions are derived from the dispenser's own block position, so two
	// different positions would legitimately produce different absolute
	// coordinates even under an identical RNG stream. Reusing one position
	// (freed by the first onBreak, re-placed for the second) isolates the
	// ONLY variable under test: window state.
	int x = 74, y = Y, z = 70;

	// --- window CLOSED baseline ---
	struct dispenser_te* dc = place_dispenser(x, y, z, 5);
	memset(dc->items, 0, sizeof(dc->items));
	dc->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 15};
	s.dispenser_break_rng = (struct random_gen) {.seed = 4242};

	reset_capture();
	struct block_data bdc = {.type = BLOCK_DISPENSER, .metadata = 5};
	struct block_info infoc = {.block = &bdc, .x = x, .y = y, .z = z};
	block_dispenser.onBreak(&s, &infoc);

	int closed_spawn_count = g_spawn_item_count;
	struct client_rpc closed_first = g_last_spawn_item;
	assert(dispenser_te_lookup(&s, x, y, z) == NULL); // freed
	assert(closed_spawn_count > 0); // the scatter actually ran

	// onBreak is inventory teardown ONLY (matches furnace/chest's split --
	// see its own doc comment); it never clears the world block itself, that
	// is the caller's separate job in the real break flow. Do that step here
	// before re-placing, or the second place_dispenser's set_block sees an
	// unchanged id+metadata and faithfully no-ops (Beta's setBlockAndMetadata
	// WithNotify returns false without a real change -- server_world_set_
	// block already mirrors that).
	set_block(x, y, z, BLOCK_AIR, 0);

	// --- window OPEN case: SAME position, SAME seed, SAME contents ---
	struct dispenser_te* do_ = place_dispenser(x, y, z, 5);
	memset(do_->items, 0, sizeof(do_->items));
	do_->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 15};
	s.dispenser_break_rng = (struct random_gen) {.seed = 4242}; // identical seed

	s.player.active_inventory = &s.player.inventory;
	assert(server_local_open_inventory(&s, &inventory_logic_dispenser,
									   DISPENSER_SIZE, x, y, z,
									   WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER));
	assert(s.player.active_inventory != &s.player.inventory);

	reset_capture();
	struct block_data bdo = {.type = BLOCK_DISPENSER, .metadata = 5};
	struct block_info infoo = {.block = &bdo, .x = x, .y = y, .z = z};
	block_dispenser.onBreak(&s, &infoo);

	int open_spawn_count = g_spawn_item_count;
	struct client_rpc open_first = g_last_spawn_item;
	assert(dispenser_te_lookup(&s, x, y, z) == NULL); // freed

	// SAME distribution: same number of sub-stacks, same last-spawned
	// item's position and velocity (the scatter draws in the SAME order
	// either way, since it runs from an identical pre-teardown snapshot).
	assert(open_spawn_count == closed_spawn_count);
	assert(open_first.payload.spawn_item.pos[0] == closed_first.payload.spawn_item.pos[0]
		  && open_first.payload.spawn_item.pos[1] == closed_first.payload.spawn_item.pos[1]
		  && open_first.payload.spawn_item.pos[2] == closed_first.payload.spawn_item.pos[2]);
	assert(open_first.payload.spawn_item.vel[0] == closed_first.payload.spawn_item.vel[0]
		  && open_first.payload.spawn_item.vel[1] == closed_first.payload.spawn_item.vel[1]
		  && open_first.payload.spawn_item.vel[2] == closed_first.payload.spawn_item.vel[2]);

	// The window was invalidated and force-closes on the next update tick;
	// finish that so the player-inventory state is clean for later tests.
	assert(server_local_test_close_active_inventory(&s));
	assert(s.player.active_inventory == &s.player.inventory);

	printf("  D6: break with GUI open scatters via the SAME Beta-faithful "
		  "algorithm as GUI closed (%d sub-stacks, identical pos/vel) OK\n",
		  closed_spawn_count);
}

// D3: lethal Nether damage with the dispenser's own GUI open must still
// kill the player and journal the drops -- the dispenser window was
// missing from BOTH server_local_project_post_close_inventory and
// server_local_plan_death_drops, so the "unknown window" fallthrough made
// the nether-death journal fail EVERY time, and server_local_set_player_
// health reverts the damage on a failed journal (immortality while the GUI
// is open, standing in lava).
static void test_nether_death_with_dispenser_open(void) {
	struct server_local ds; // a separate, Nether-capable fixture (mirrors
							// test_death_drop_journal_env.c's init_server)
	memset(&ds, 0, sizeof(ds));
	rand_gen_seed(&ds.rand_src);
	ds.container_drop_rng.seed = 0x12345678u;
	ds.dispenser_break_rng.seed = 0x2545F491u;

	// A prior run of this exact test (a previous ctest invocation, or a
	// manual rerun while debugging) leaves level.dat/region files behind.
	// A REUSED region silently defeats this test's own dispenser placement
	// below: server_world_set_block's Beta-faithful "identical id+metadata
	// is a no-op, returns false" rule (World.setBlockAndMetadataWithNotify)
	// makes a second placement of the SAME block+meta onto an already-
	// persisted chunk a no-op -- the exact class of staleness D6's own
	// golden had to guard against. Force a clean slate every run.
	remove(test_fixture_join(TMP_WORLD, "_nether_death/level.dat"));
	remove(test_fixture_join(TMP_WORLD, "_nether_death/level.dat_new"));
	remove(test_fixture_join(TMP_WORLD, "_nether_death/level.dat_old"));
	remove(test_fixture_join(TMP_WORLD, "_nether_death/death_drops.dat"));
	remove(test_fixture_join(TMP_WORLD, "_nether_death/death_drops.dat_new"));
	remove(test_fixture_join(TMP_WORLD, "_nether_death/death_drops.dat_old"));
	remove(test_fixture_join(TMP_WORLD, "_nether_death/DIM-1/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "_nether_death/region/r.0.0.mcr"));

	MKDIR(test_fixture_join(TMP_WORLD, "_nether_death"));
	MKDIR(test_fixture_join(TMP_WORLD, "_nether_death/region"));
	MKDIR(test_fixture_join(TMP_WORLD, "_nether_death/DIM-1"));
	MKDIR(test_fixture_join(TMP_WORLD, "_nether_death/DIM-1/region"));
	string_init_set_str(ds.level_name, test_fixture_join(TMP_WORLD, "_nether_death"));
	{
		// Synthesize a fresh level.dat from the bundled Beta template (the
		// create_level_with_hotbar precedent in test_death_drop_journal_env.c)
		// so the level_archive_create reopen below has a real file to read.
		struct level_archive tmp_level;
		assert(level_archive_create_new(&tmp_level, ds.level_name, 424242,
										(vec3) {8.5F, 70.0F, 8.5F}));
		level_archive_destroy(&tmp_level); // flushes level.dat to disk
	}
	assert(level_archive_create(&ds.level, ds.level_name));
	if(!level_archive_read_death_tx_counter(&ds.level, &ds.death_tx_counter))
		ds.death_tx_counter = CAVEWII_DEATH_TX_LEGACY_FLOOR;

	assert(inventory_create(&ds.player.inventory, &inventory_logic_player, &ds,
							&ds.player,
							INVENTORY_SIZE, 0, 0, 0));
	assert(level_archive_read_inventory(&ds.level, &ds.player.inventory));
	ds.player.active_inventory = &ds.player.inventory;
	ds.player.x = 8.5;
	ds.player.y = 70.0;
	ds.player.z = 8.5;
	ds.player.spawn_x = 8;
	ds.player.spawn_y = 70;
	ds.player.spawn_z = 8;
	ds.player.health = MAX_PLAYER_HEALTH;
	ds.player.dimension = WORLD_DIM_NETHER;
	ds.player.has_pos = true;
	ds.player.finished_loading = true;
	ds.player.fire = -PLAYER_FIRE_RESISTANCE;
	ds.player.oxygen = MAX_OXYGEN;
	ds.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&ds))
		abort();
	piston_moving_store_init(&ds.piston_moving);
	dict_entity_init(ds.entities);

	server_world_create(&ds.world, ds.level_name, WORLD_DIM_NETHER, 424242,
					   true /* superflat */);
	ds.worlds[0].session = &ds; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	ds.world.local = &ds;
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&ds.world, 0, 0, &chunk) && chunk);

	// Place a dispenser near the player and open its GUI -- the exact
	// condition the bug needed (a dispenser window active when lethal
	// damage lands), with one item in it so the journal has something
	// concrete to carry.
	int dx = 8, dy = 70, dz = 9;
	assert(server_world_set_block(
		&ds.worlds[0], dx, dy, dz,
		(struct block_data) {.type = BLOCK_DISPENSER, .metadata = 2}));
	int idx = dispenser_te_register(&ds, dx, dy, dz);
	assert(idx >= 0);
	struct dispenser_te* dte = dispenser_te_lookup(&ds, dx, dy, dz);
	assert(dte);
	dte->items[0] = (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};

	assert(server_local_open_inventory(&ds, &inventory_logic_dispenser,
									   DISPENSER_SIZE, dx, dy, dz,
									   WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER));
	assert(ds.player.active_inventory != &ds.player.inventory);

	// Lethal Nether damage with the dispenser GUI still open.
	bool result = server_local_set_player_health(&ds, 0);

	// Deviation-#18 split: the lethal hit commits the DEAD state and the
	// close/journal/respawn transaction runs at the explicit request. The
	// D3 regression now lives in the REQUEST: before the fix,
	// server_local_plan_death_drops hit the "unknown window" fallthrough for
	// inventory_logic_dispenser, so server_local_begin_death_drop_journal
	// returned DEATH_DROP_BEGIN_FAILED and the respawn stayed retryable
	// forever -- the player could not come back. Beta's postcondition after
	// the accepted request is the immediate full respawn
	// (EntityPlayer.preparePlayerToSpawn -- health=MAX, position=spawn,
	// dimension=OVERWORLD).
	assert(result);
	assert(ds.player.health <= 0);
	assert(server_local_respawn_request(&ds));
	assert(ds.player.health == MAX_PLAYER_HEALTH);
	assert(ds.player.dimension == WORLD_DIM_OVERWORLD);
	assert(ds.death_drop_journal != NULL);
	// The dispenser BLOCK is never broken in this test -- it survives the
	// player's death exactly like an unbroken chest/furnace would, so its
	// contents are NOT dropped as loose items (server_local_plan_death_
	// drops's "valid block container" branch, mirroring furnace/chest).
	// Instead its live 9-slot window is recorded into container_slots (the
	// D9 fix's exact target: container_slot_valid used to reject every one
	// of these for window==WINDOWC_DISPENSER, which is what made journal_
	// valid -- and everything upstream of it -- fail).
	assert(ds.death_drop_journal->container_count == DISPENSER_SIZE_STORAGE);

	printf("  D3: lethal Nether damage with the dispenser GUI open now "
		  "kills+respawns the player (health=%d, dimension=overworld) and "
		  "journals its live storage (%u container slot(s)) OK\n",
		  (int)ds.player.health,
		  (unsigned)ds.death_drop_journal->container_count);

	// Local cleanup for this self-contained fixture -- mirrors
	// test_death_drop_journal_env.c's discard_server (minus the entity-tick
	// MEM2 bookkeeping this short-lived test never allocates into). Notably
	// death_drop_journal_free, NOT free(): the journal comment states it
	// "live[s] in MEM2, never in server_local's main-thread stack frame" --
	// a raw libc free() on a MEM2 allocation is the wrong deallocator.
	dict_entity_it_t it;
	dict_entity_it(it, ds.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(ds.entities);
	if(ds.player.has_pos)
		server_world_discard(&ds.world);
	piston_moving_store_destroy(&ds.piston_moving);
	if(ds.player.active_inventory
	   && ds.player.active_inventory != &ds.player.inventory)
		inventory_destroy_owned(ds.player.active_inventory, &ds.player);
	inventory_destroy(&ds.player.inventory, &ds.player);
	death_drop_journal_free(ds.death_drop_journal);
	if(ds.level.data)
		level_archive_discard(&ds.level);
	string_clear(ds.level_name);
}

int main(void) {
	test_fixture_dir("test_dispenser_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	// server_local_init's chest_pos/sign_pos/furnace_pos convention (all
	// bytes 0xFF -> every int -1, "y < 0 == free"): this test builds
	// server_local by hand (like test_arrow_env/test_tnt_primed_env) rather
	// than through server_local_init, so dispenser_pos needs the identical
	// treatment or dispenser_te_register sees every slot as "occupied at
	// (0,0,0)" and never finds a free one.
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
					   true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	// Binds the TE tables (chest/furnace/dispenser/sign) to the world's chunk
	// lifecycle (server_local_init's own setup, normally); without it,
	// server_world_save_chunk/load_chunk silently skip the te_store
	// gather/insert/release calls this test's persistence round trip needs.
	s.worlds[0].local = &s;
	s.worlds[0].session = &s; /* A1a (D1): owning session for services/callbacks */
	s.scheduled_tick_budget = 128;
	rand_gen_seed(&s.rand_src);
	rand_gen_seed(&s.dispenser_break_rng);
	dict_entity_init(s.entities);
	// D1/D6 goldens open a real dispenser window via server_local_open_
	// inventory, which requires has_pos (the "player has actually spawned"
	// gate) -- matches server_local_init's real invariant once the player
	// finishes loading; no earlier test in this file exercises the window
	// system so this was never needed before.
	s.player.has_pos = true;
	// inv_on_create (inventory_dispenser.c) copies the player's own hotbar
	// + main slots under the dispenser's 3x3 storage grid -- the same
	// "your inventory below the container" layout every other Beta
	// container GUI uses. No earlier test in this file opens a real window,
	// so s.player.inventory was never constructed; give it a real backing
	// store now (mirrors test_death_drop_journal_env.c's init_server).
	assert(inventory_create(&s.player.inventory, &inventory_logic_player,
							&s, &s.player, INVENTORY_SIZE, 0, 0, 0));

	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	clin_test_set_rpc_observer(capture_rpc, NULL);

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 4; cx++)
		for(int cz = 0; cz <= 4; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	printf("test_dispenser_env:\n");
	test_direction_table();
	test_reservoir_pick();
	test_reservoir_decrement();
	test_empty_dispenser_sfx();
	test_generic_item_rng_order();
	test_projectile_spawns();
	test_redstone_trigger();
	test_persistence_roundtrip();
	test_persistence_contents_only_edit();
	test_window_sync_no_duplication();
	test_capacity_exhaustion_preserves_item();
	test_window_open_break_matches_closed_break();
	test_nether_death_with_dispenser_open();

	clin_test_set_rpc_observer(NULL, NULL);
	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_dispenser_env\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	/* test_nether_death_with_dispenser_open's own sibling fixture (a
	 * separate "<TMP_WORLD>_nether_death" tree, not a child of TMP_WORLD) --
	 * pre-existing behavior left this one uncleaned even before the fixture
	 * moved to a per-process path; now that every run's name is unique, an
	 * uncleaned sibling would accumulate across runs instead of being
	 * overwritten in place, so it gets the same cleanup here. */
	test_fixture_dir_cleanup(test_fixture_join(TMP_WORLD, "_nether_death"));
	return 0;
}
