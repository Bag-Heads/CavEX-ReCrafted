/*
	Group D integration goldens for the 2026-06-19 mob-AI parity audit, driving the REAL
	arrow tick (entity_arrow.c) over a superflat server_world -- pins what the pure-math
	copies in test_skeleton_arrow.c / test_bow_arrow.c cannot reach:

	1. DRAG-THEN-GRAVITY. Beta EntityArrow.onUpdate does motionY *= 0.99 then motionY -=
	   0.03, so motionY_next = motionY*0.99 - 0.03 (tick 1 from rest = -0.03). The old
	   gravity-then-drag order gave -0.0297 (1% under-gravity).
	2. 0.3 HIT-BOX EXPANSION. Beta widens each candidate box by 0.3 per axis
	   (AxisAlignedBB.expand) before the swept intercept, so a shot grazing 0.2 outside the
	   raw mob box still HITS, while one 0.4 outside (beyond the 0.3 grow) MISSES.

	Decompile: 740c583:1.7.3-LTS EntityArrow.onUpdate.
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
#include "block/piston_moving.h"
#include "block/dispenser_logic.h" // DISPENSER_PROJECTILE_OWNER_SENTINEL
#include "entity/block_ray.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/client_admission.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/audio.h" /* F4-RNG: SOUND_RANDOM_DRR */
#include "platform/thread.h"
#include "game/game_state.h"
#include "game/client_world_context.h"
#include "particle.h"
#include "network/entity_archive.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_arrow_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
static int feq(float a, float b) { return fabsf(a - b) < 1.0e-5f; }
static struct entity* pinned_zombie(float x, float z);

// Materialize an arrow entity directly (server_local_spawn_arrow only QUEUES into
// pending_arrows, flushed by the static server tick loop). Mirrors
// server_local_flush_arrows: fresh id -> dict slot -> entity_arrow ctor -> teleport.
static uint32_t g_next_id = 5000;
static struct entity* spawn_arrow_now_kind(
	vec3 pos, vec3 vel, uint32_t owner_id, bool from_player) {
	uint32_t id = g_next_id++;
	struct entity** e_ptr = dict_entity_safe_get(s.entities, id);
	*e_ptr = calloc(1,sizeof(struct entity));
	struct entity* e = *e_ptr;
	assert(e);
	entity_arrow(id, e, true, &s.worlds[0], owner_id, ITEM_ARROW, ARROW_DAMAGE,
		from_player);
	e->teleport(e, pos);
	glm_vec3_copy(vel, e->vel);
	assert(e->tick_server);
	return e;
}

static struct entity* spawn_arrow_now(vec3 pos, vec3 vel, uint32_t owner_id) {
	return spawn_arrow_now_kind(pos, vel, owner_id, false);
}

// 1. The real arrow tick integrates drag then gravity.
static void test_gravity_drag_order(void) {
	s.player.has_pos = false;          // no player target to intercept
	s.player.finished_loading = false;
	vec3 pos = {4.0f, 75.0f, 4.0f};    // above the superflat surface, away from the mobs
	vec3 vel = {0.0f, 0.0f, 0.0f};     // at rest -> tick 1 vel[1] = 0*0.99 - 0.03
	struct entity* a = spawn_arrow_now(pos, vel, 999u);

	a->tick_server(a, &s);
	assert(feq(a->vel[1], -ARROW_GRAVITY)); // -0.03 (NOT the gravity-then-drag -0.0297)
	a->tick_server(a, &s);
	assert(fabsf(a->vel[1] - (-0.0597f)) < 1.0e-5f); // -0.03*0.99 - 0.03
	printf("  arrow drag-then-gravity: vel[1] = -0.03 then -0.0597 (real tick) OK\n");
}

// 1b. WATER DRAG [decompile 740c583: EntityArrow.onUpdate:200-218]:
//     the drag factor starts at 0.99 and gravity at 0.03; in water the drag
//     factor becomes 0.8 (bubbles are spawned); all three velocity components
//     are scaled by the drag factor, then gravity is subtracted from the
//     vertical component.
// Gravity stays 0.03 underwater; only the per-tick velocity multiplier swaps
// 0.99 -> 0.8. Beta's isInWater is the bbox handleWaterMovement verdict; this
// engine uses the arrow's own foot-cell probe (the egg precedent,
// entity_egg.c EntityEgg.onUpdate tail).
static void test_water_drag(void) {
	s.player.has_pos = false;
	s.player.finished_loading = false;
	// A still-water cell well above the superflat surface, away from mobs.
	assert(server_world_set_block(&s.worlds[0], 6, 80, 6,
		(struct block_data) {.type = BLOCK_WATER_STILL}));
	vec3 pos = {6.5f, 80.5f, 6.5f};
	// At rest: vel[1] = 0*0.8 - 0.03 = -0.03 (gravity UNCHANGED in water).
	struct entity* wet = spawn_arrow_now(pos, (vec3) {0.0f, 0.0f, 0.0f}, 998u);
	wet->tick_server(wet, &s);
	assert(feq(wet->vel[1], -ARROW_GRAVITY));
	// Nonzero sink: drag must be 0.8, NOT 0.99. -0.5*0.8 - 0.03 = -0.43.
	struct entity* sink
		= spawn_arrow_now(pos, (vec3) {0.4f, -0.5f, 0.0f}, 997u);
	sink->tick_server(sink, &s);
	if(!feq(sink->vel[0], 0.4f * ARROW_WATER_DRAG)
	   || !feq(sink->vel[1], -0.5f * ARROW_WATER_DRAG - ARROW_GRAVITY)) {
		fprintf(stderr,
			"RED water_drag: vel=(%.6f,%.6f,%.6f), want (%.6f,%.6f,%.6f) "
			"[EntityArrow.java:200-218]\n",
			(double)sink->vel[0], (double)sink->vel[1], (double)sink->vel[2],
			(double)(0.4f * ARROW_WATER_DRAG),
			(double)(-0.5f * ARROW_WATER_DRAG - ARROW_GRAVITY),
			(double)sink->vel[2]);
		assert(0 && "arrow water drag is not Beta's 0.8");
	}
	// Air control: the same velocity in a dry cell keeps the 0.99 drag.
	// (the cell at y=90 is already air; server_world_set_block refuses id 0.)
	vec3 dry = {6.5f, 90.5f, 6.5f};
	struct entity* air = spawn_arrow_now(dry, (vec3) {0.0f, -0.5f, 0.0f}, 996u);
	air->tick_server(air, &s);
	assert(feq(air->vel[1], -0.5f * ARROW_AIR_DRAG - ARROW_GRAVITY));
	printf("  arrow water drag 0.8 / air drag 0.99 OK\n");
}

// EntityArrow.java:107-125 and Entity.java:222-224: no air age limit; the
// void reaps on the tick after crossing posY < -64, and a stuck arrow dies at 1200.
static void test_arrow_reapers(void) {
	s.player.has_pos = false;
	s.player.finished_loading = false;
	vec3 pos = {8.0F, 100.0F, 8.0F};
	vec3 vel = {0.0F, 0.0F, 0.0F};
	struct entity* air = spawn_arrow_now(pos, vel, 999u);
	air->data.arrow.age = 100;
	assert(!air->tick_server(air, &s));
	assert(air->data.arrow.age == 101);
	air->data.arrow.age = 1999;
	assert(!air->tick_server(air, &s));
	assert(air->data.arrow.age == 2000);

	air->pos[1] = -63.95F;
	air->vel[1] = -0.1F;
	assert(!air->tick_server(air, &s));
	assert(air->pos[1] < -64.0F);
	assert(air->tick_server(air, &s));

	struct entity* stuck = spawn_arrow_now(pos, vel, 999u);
	assert(server_world_set_block(&s.worlds[0], 8, 100, 8,
		(struct block_data) {.type = BLOCK_STONE}));
	stuck->data.arrow.in_ground = true;
	stuck->data.arrow.stuck_block_x = 8;
	stuck->data.arrow.stuck_block_y = 100;
	stuck->data.arrow.stuck_block_z = 8;
	stuck->data.arrow.stuck_block_type = BLOCK_STONE;
	stuck->data.arrow.stuck_ticks = 1198;
	assert(!stuck->tick_server(stuck, &s));
	assert(stuck->data.arrow.stuck_ticks == 1199);
	assert(stuck->tick_server(stuck, &s));
	assert(stuck->data.arrow.stuck_ticks == 1200);
	puts("  arrow reapers: air 101/2000 alive; next-tick void and ground 1200 removed OK");
}

static void test_removed_stuck_block(void) {
	vec3 pos = {8.0F, 75.0F, 8.0F};
	vec3 vel = {1.0F, 0.5F, -1.0F};
	struct entity* a = spawn_arrow_now(pos, vel, 999u);
	struct java_random born = {0};
	entity_lifetime_rng_seed(&born, s.worlds[0].seed, s.worlds[0].dimension, a->id);
	assert(a->gameplay_rng.seed == born.seed);
	a->data.arrow.in_ground = true;
	a->data.arrow.stuck_ticks = 17;
	a->data.arrow.age = 9;
	a->data.arrow.stuck_block_x = 8;
	a->data.arrow.stuck_block_y = 75;
	a->data.arrow.stuck_block_z = 8;
	a->data.arrow.stuck_block_type = BLOCK_STONE;
	assert(server_world_set_block(&s.worlds[0], 8, 75, 8,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(&s.worlds[0], 8, 75, 8,
		(struct block_data) {.type = BLOCK_AIR}));
	/* CFR EntityArrow.java:128-130: X/Y/Z, own Random. */
	jrand_set_seed(&a->gameplay_rng, 0x13579BDFu);
	struct java_random expected = a->gameplay_rng;
	struct random_gen world_before = s.rand_src;
	struct random_gen shared_before = s.entity_rng;
	float rx = jrand_next_float(&expected) * 0.2F;
	float ry = jrand_next_float(&expected) * 0.2F;
	float rz = jrand_next_float(&expected) * 0.2F;
	assert(!a->tick_server(a, &s));
	assert(!a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_ticks == 0 && a->data.arrow.age == 0);
	assert(feq(a->vel[0], vel[0] * rx));
	assert(feq(a->vel[1], vel[1] * ry));
	assert(feq(a->vel[2], vel[2] * rz));
	assert(a->gameplay_rng.seed == expected.seed);
	assert(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0);
	assert(memcmp(&shared_before, &s.entity_rng, sizeof shared_before) == 0);

	// Drive the actual collision path so its saved block identity is exercised.
	assert(server_world_set_block(&s.worlds[0], 12, 75, 8,
		(struct block_data) {.type = BLOCK_STONE}));
	vec3 approach = {10.0F, 75.5F, 8.5F};
	/* Point ray: the endpoint must continue inside x=12; Beta World:813-840
	 * has zero remainder when it ends exactly at the entry face. */
	vec3 flight = {2.25F, 0.0F, 0.0F};
	struct entity* embedded = spawn_arrow_now(approach, flight, 999u);
	assert(!embedded->tick_server(embedded, &s));
	assert(embedded->data.arrow.in_ground);
	assert(embedded->data.arrow.stuck_block_x == 12);
	assert(embedded->data.arrow.stuck_block_y == 75);
	assert(embedded->data.arrow.stuck_block_z == 8);
	assert(!embedded->tick_server(embedded, &s));
	assert(embedded->data.arrow.in_ground);
	assert(server_world_set_block(&s.worlds[0], 12, 75, 8,
		(struct block_data) {.type = BLOCK_AIR}));
	assert(!embedded->tick_server(embedded, &s));
	assert(!embedded->data.arrow.in_ground);
}

// BlockTrapDoor.java: closed thickness is 3/16; Beta's block ray hits its top.
// EntityArrow.onUpdate:175-179 stores the hit block's coordinates, not the
// cell below the arrow center. Exercise the real collision and next stuck tick.
static void test_thin_trapdoor_stuck_identity(void) {
	// Facing 0 hangs from +Z; keep the support present during both ticks.
	assert(server_world_set_block(&s.worlds[0], 14, 75, 9,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(&s.worlds[0], 14, 75, 8,
		(struct block_data) {.type = BLOCK_TRAP_DOOR, .metadata = 0}));
	vec3 pos = {14.5F, 76.0F, 8.5F};
	vec3 vel = {0.0F, -1.0F, 0.0F};
	struct entity* a = spawn_arrow_now(pos, vel, 999u);
	assert(!a->tick_server(a, &s));
	assert(a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_block_x == 14);
	assert(a->data.arrow.stuck_block_y == 75);
	assert(a->data.arrow.stuck_block_z == 8);
	assert(a->data.arrow.stuck_block_type == BLOCK_TRAP_DOOR);
	assert(!a->tick_server(a, &s));
	assert(a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_ticks == 1);
}

// EntityArrow.onUpdate:175-182,193-195,216-230: an X=17 to X=18 ray hit
// replaces the launch speed 2 with impact displacement 1, then applies 0.99
// drag and 0.03 gravity before the block can be removed on the next tick.
static void test_collision_derived_release_motion(void) {
	assert(server_world_set_block(&s.worlds[0], 18, 75, 8,
		(struct block_data) {.type = BLOCK_STONE}));
	vec3 pos = {17.0F, 75.5F, 8.5F};
	vec3 vel = {2.0F, 0.0F, 0.0F};
	struct entity* a = spawn_arrow_now(pos, vel, 999u);
	assert(!a->tick_server(a, &s));
	assert(a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_block_x == 18);
	assert(a->data.arrow.stuck_block_y == 75);
	assert(a->data.arrow.stuck_block_z == 8);
	assert(feq(a->vel[0], 0.99F));
	assert(feq(a->vel[1], -0.03F));
	assert(feq(a->vel[2], 0.0F));
	assert(server_world_set_block(&s.worlds[0], 18, 75, 8,
		(struct block_data) {.type = BLOCK_AIR}));
	/* CFR EntityArrow.java:128-130: X/Y/Z, own Random. */
	jrand_set_seed(&a->gameplay_rng, 0x13579BDFu);
	struct java_random expected = a->gameplay_rng;
	struct random_gen world_before = s.rand_src;
	struct random_gen shared_before = s.entity_rng;
	float rx = jrand_next_float(&expected) * 0.2F;
	float ry = jrand_next_float(&expected) * 0.2F;
	float rz = jrand_next_float(&expected) * 0.2F;
	assert(!a->tick_server(a, &s));
	assert(!a->data.arrow.in_ground);
	assert(feq(a->vel[0], 0.99F * rx));
	assert(feq(a->vel[1], -0.03F * ry));
	assert(feq(a->vel[2], 0.0F * rz));
	assert(a->gameplay_rng.seed == expected.seed);
	assert(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0);
	assert(memcmp(&shared_before, &s.entity_rng, sizeof shared_before) == 0);
}

/* [740c583 BlockFence:12-14, Block.collisionRayTrace:357]: the fence's
 * collision box reaches 1.5, but its ray bounds stop at 1.0. */
static void test_fence_point_ray(void) {
	assert(server_world_set_block(&s.worlds[0], 23, 75, 8,
		(struct block_data){.type=BLOCK_FENCE}));
	struct entity* high=spawn_arrow_now((vec3){22.0F,76.2F,8.5F},
		(vec3){2.0F,0,0},999u);
	assert(!high->tick_server(high,&s) && !high->data.arrow.in_ground);
	struct entity* low=spawn_arrow_now((vec3){22.0F,75.5F,8.5F},
		(vec3){2.0F,0,0},999u);
	assert(!low->tick_server(low,&s) && low->data.arrow.in_ground);
	assert(low->data.arrow.stuck_block_x==23 && low->data.arrow.stuck_block_y==75);
	assert(!low->tick_server(low,&s) && low->data.arrow.in_ground);
}

/* [740c583 BlockCactus:27-38, Block.java:357]: inherited ray is the
 * full unit cube, despite the selected/collision inset. Ladder gets its
 * 2/16 side strip from collision-presence lookup before the ray. */
static void test_cactus_ladder_and_stairs_ray(void) {
	assert(server_world_set_block(&s.worlds[0],26,75,8,
		(struct block_data){.type=BLOCK_CACTUS}));
	struct entity* cactus=spawn_arrow_now((vec3){25.5F,75.5F,8.5F},
		(vec3){1.5F,0,0},999u);
	assert(!cactus->tick_server(cactus,&s) && cactus->data.arrow.in_ground);
	assert(cactus->data.arrow.stuck_block_x==26);
	assert(feq(cactus->vel[0],.5F*.99F));
	assert(server_world_set_block(&s.worlds[0],28,75,8,
		(struct block_data){.type=BLOCK_LADDER,.metadata=4}));
	struct entity* ladder=spawn_arrow_now((vec3){29.5F,75.5F,8.5F},
		(vec3){-1.5F,0,0},999u);
	assert(!ladder->tick_server(ladder,&s) && ladder->data.arrow.in_ground);
	assert(ladder->data.arrow.stuck_block_x==28);
	assert(server_world_set_block(&s.worlds[0],31,75,8,
		(struct block_data){.type=BLOCK_WOODEN_STAIRS,.metadata=0}));
	struct entity* stairs=spawn_arrow_now((vec3){30.5F,75.75F,8.5F},
		(vec3){1.5F,0,0},999u);
	assert(!stairs->tick_server(stairs,&s) && stairs->data.arrow.in_ground);
	assert(stairs->data.arrow.stuck_block_x==31);
}

/* [740c583 BlockSnow:8-29]: level 2 has a ray box but NULL collision;
 * level 3 has collision presence and a 4/8-high ray. */
static void test_snow_presence_then_ray(void) {
	assert(server_world_set_block(&s.worlds[0],32,75,10,
		(struct block_data){.type=BLOCK_SNOW,.metadata=2}));
	struct entity* low=spawn_arrow_now((vec3){32.5F,75.8F,9.5F},
		(vec3){0,0,1.5F},999u);
	assert(!low->tick_server(low,&s) && !low->data.arrow.in_ground);
	assert(server_world_set_block(&s.worlds[0],32,75,10,
		(struct block_data){.type=BLOCK_SNOW,.metadata=3}));
	struct entity* high=spawn_arrow_now((vec3){32.5F,76.0F,10.5F},
		(vec3){0,-1.0F,0},999u);
	assert(!high->tick_server(high,&s) && high->data.arrow.in_ground);
	assert(high->data.arrow.stuck_block_y==75);
}

/* SYNTHETIC adapter fixture, not a claim of naturally pushable snow:
 * BlockPistonMoving:80-118 queries stored bounds at moving-cell metadata,
 * and TileEntityPiston.func_31008_a(0) uses last, not current progress.
 * The real world may normally copy metadata on placement; that reachability
 * is separate from these literal API semantics. */
static void test_moving_cell_ray_synthetic(void) {
	s.worlds[0].local=&s;
	assert(server_world_set_block(&s.worlds[0],35,75,12,
		(struct block_data){.type=BLOCK_PISTON_MOVING,.metadata=3}));
	struct entity* missing=spawn_arrow_now((vec3){35.9F,75.25F,12.5F},
		(vec3){-1.0F,0,0},999u);
	assert(!missing->tick_server(missing,&s) && !missing->data.arrow.in_ground);
	struct piston_moving_state st={.stored_id=BLOCK_SNOW,.stored_meta=2,
		.facing=5,.flags=PISTON_MOVING_FLAG_EXTENDING,
		.last_progress=.25F,.current_progress=.75F};
	assert(piston_moving_store_upsert_owned(&s.piston_moving,server_world_piston_owner(&s.worlds[0]),35,75,12,&st,NULL) /* A1b: world-tagged */);
	struct entity* extend=spawn_arrow_now((vec3){35.9F,75.25F,12.5F},
		(vec3){-1.0F,0,0},999u);
	assert(!extend->tick_server(extend,&s) && extend->data.arrow.in_ground);
	assert(extend->data.arrow.stuck_block_x==35);
	assert(feq(extend->vel[0],-.65F*.99F)); /* right face at 35.25 */
	st.flags=0; /* retracting uses last=.25 -> right face at 35.75 */
	assert(piston_moving_store_upsert_owned(&s.piston_moving,server_world_piston_owner(&s.worlds[0]),35,75,12,&st,NULL) /* A1b: world-tagged */);
	struct entity* retract=spawn_arrow_now((vec3){35.9F,75.25F,12.5F},
		(vec3){-1.0F,0,0},999u);
	assert(!retract->tick_server(retract,&s) && retract->data.arrow.in_ground);
	assert(feq(retract->vel[0],-.15F*.99F));
	assert(piston_moving_store_remove(&s.piston_moving,35,75,12));
	/* Stored cactus has the same full-cube ray exception when moving. */
	st.stored_id=BLOCK_CACTUS; st.stored_meta=0; st.flags=0;
	st.last_progress=0; st.current_progress=.75F;
	assert(piston_moving_store_upsert_owned(&s.piston_moving,server_world_piston_owner(&s.worlds[0]),35,75,12,&st,NULL) /* A1b: world-tagged */);
	struct entity* cactus=spawn_arrow_now((vec3){35.5F,75.5F,12.5F},
		(vec3){1.0F,0,0},999u);
	assert(!cactus->tick_server(cactus,&s) && cactus->data.arrow.in_ground);
	assert(cactus->data.arrow.stuck_block_x==35);
	assert(piston_moving_store_remove(&s.piston_moving,35,75,12));
}

/* [740c583 World.func_28105_a:716,838; BlockFluid:56,70]: arrow passes
 * source water because stopOnLiquid=false; plants and torches have a null
 * collision getter when skipNoCollisionBox=true. */
static void test_noncolliding_cells_skipped(void) {
	const uint8_t ids[]={BLOCK_WATER_STILL,BLOCK_WATER_FLOW,
		BLOCK_TALL_GRASS,BLOCK_TORCH};
	for(int i=0;i<4;i++) {
		int x=37+i;
		assert(server_world_set_block(&s.worlds[0],x,75,15,
			(struct block_data){.type=ids[i]}));
		struct entity* a=spawn_arrow_now((vec3){(float)x-.5F,75.5F,15.5F},
			(vec3){1.5F,0,0},999u);
		assert(!a->tick_server(a,&s) && !a->data.arrow.in_ground);
		assert(server_world_set_block(&s.worlds[0],x,75,15,
			(struct block_data){.type=BLOCK_AIR}));
	}
}

/* [740c583 EntityArrow.onUpdate:128-160]: the entity sweep ends at the
 * block hitVec. A body before the block wins; one behind is untouched. */
static void test_entity_order_against_block_ray(void) {
	assert(server_world_set_block(&s.worlds[0],40,70,36,
		(struct block_data){.type=BLOCK_STONE}));
	struct entity* front=pinned_zombie(40.5F,35.0F);
	short hp=front->health;
	struct entity* a=spawn_arrow_now((vec3){40.5F,70.85F,33.5F},
		(vec3){0,0,4.0F},999u);
	assert(a->tick_server(a,&s));
	assert(front->health<hp);
	assert(server_world_set_block(&s.worlds[0],42,70,36,
		(struct block_data){.type=BLOCK_STONE}));
	struct entity* behind=pinned_zombie(42.5F,39.0F);
	hp=behind->health;
	a=spawn_arrow_now((vec3){42.5F,70.85F,34.0F},
		(vec3){0,0,6.0F},999u);
	assert(!a->tick_server(a,&s) && a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_block_z==36 && behind->health==hp);
}

/* [740c583 EntityArrow:131-158; AxisAlignedBB:224-253; Vec3D:101-133]
 * After the block clips the segment to 0.0002, every per-axis Beta face
 * candidate is null (dx*dx < (double)1e-7F). A float slab would hit the
 * expanded near face at 0.9999. The longer 0.001 control must still hit. */
static void test_short_clipped_entity_intercept(void) {
	s.player.has_pos=false;
	assert(server_world_set_block(&s.worlds[0],1,70,46,
		(struct block_data){.type=BLOCK_STONE}));
	struct entity* mob=pinned_zombie(1.5999F,46.5F);
	short hp=mob->health;
	struct entity* a=spawn_arrow_now((vec3){.9998F,70.85F,46.5F},
		(vec3){1.0F,0,0},999u);
	assert(!a->tick_server(a,&s) && a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_block_x==1 && mob->health==hp);

	assert(server_world_set_block(&s.worlds[0],1,70,47,
		(struct block_data){.type=BLOCK_STONE}));
	s.player.has_pos=true;
	s.player.finished_loading=true;
	s.player.x=1.5999;
	s.player.y=70.0+PLAYER_EYE_HEIGHT;
	s.player.z=47.5;
	s.player.health=MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time=0;
	hp=s.player.health;
	a=spawn_arrow_now((vec3){.9998F,70.85F,47.5F},
		(vec3){1.0F,0,0},999u);
	assert(!a->tick_server(a,&s) && a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_block_x==1 && s.player.health==hp);
	s.player.has_pos=false;

	assert(server_world_set_block(&s.worlds[0],1,70,45,
		(struct block_data){.type=BLOCK_STONE}));
	mob=pinned_zombie(1.5995F,45.5F);
	hp=mob->health;
	a=spawn_arrow_now((vec3){.999F,70.85F,45.5F},
		(vec3){1.0F,0,0},999u);
	assert(a->tick_server(a,&s));
	assert(mob->health<hp);
}

/* [740c583 Vec3D:101-133; AxisAlignedBB:224-253] t=1 is inclusive.
 * The first mob at the clipped endpoint must beat the stone contact. */
static void test_entity_at_clipped_endpoint(void) {
	s.player.has_pos=false;
	assert(server_world_set_block(&s.worlds[0],1,70,44,
		(struct block_data){.type=BLOCK_STONE}));
	struct entity* mob=pinned_zombie(1.6F,44.5F);
	short hp=mob->health;
	struct entity* a=spawn_arrow_now((vec3){.5F,70.85F,44.5F},
		(vec3){1.5F,0,0},999u);
	assert(a->tick_server(a,&s));
	assert(mob->health<hp);
}

/* The start and direction are representable floats, but converting the
 * double segment endpoint back to float at x=8192 erases .0004 travel. */
static void test_shifted_short_entity_intercept(void) {
	s.player.has_pos=true;
	s.player.finished_loading=true;
	s.player.x=8192.6002; /* expanded minX = 8192.0002, inside .0004 */
	s.player.y=70.0+PLAYER_EYE_HEIGHT;
	s.player.z=52.5;
	s.player.health=MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time=0;
	struct entity* a=spawn_arrow_now((vec3){8192.0F,70.85F,52.5F},
		(vec3){.0004F,0,0},999u);
	assert(a->tick_server(a,&s));
	assert(s.player.health<MAX_PLAYER_HEALTH);
	s.player.has_pos=false;
}

/* Ruling 13: Beta's exact-face t=0 makes backoff 0/0. CaveWii preserves a
 * finite position to avoid undefined NaN-to-world-coordinate conversion. A
 * pre-fix executable RED would reach that conversion; this is post-fix only. */
static void test_exact_face_zero_impact_safety(void) {
	assert(server_world_set_block(&s.worlds[0],18,75,8,
		(struct block_data){.type=BLOCK_STONE}));
	struct entity* a=spawn_arrow_now((vec3){18.0F,75.5F,8.5F},
		(vec3){1.0F,0,0},999u);
	assert(!a->tick_server(a,&s) && a->data.arrow.in_ground);
	assert(a->data.arrow.stuck_block_x==18
		&& a->data.arrow.stuck_block_y==75
		&& a->data.arrow.stuck_block_z==8
		&& a->data.arrow.stuck_block_type==BLOCK_STONE);
	for(int i=0;i<3;i++) assert(isfinite(a->pos[i]) && isfinite(a->vel[i]));
	assert(feq(a->pos[0],18.0F) && feq(a->pos[1],75.5F)
		&& feq(a->pos[2],8.5F));
}

/* Operation count for the world-free primitive: 16 arrow-like tracks over
 * 20 ticks, 1.5 blocks/tick, through sparse low boxes that miss at y=.5.
 * This is a synthetic ray workload, not instrumentation of real arrow ticks
 * and not a speed or hardware measurement. */
static bool sparse_ray_cell(int x, int y, int z, bool stop_on_liquid,
							struct block_ray_cell* out, void* ctx) {
	(void)y; (void)z; (void)stop_on_liquid; (void)ctx;
	*out=(struct block_ray_cell){0};
	if(x%4==0) {
		*out=(struct block_ray_cell){.present=true,.collidable=true,
			.has_collision_box=true,.has_ray_box=true,
			.x1=0,.y1=0,.z1=0,.x2=1,.y2=.125,.z2=1};
	}
	return true;
}

static void test_block_ray_workload(void) {
	unsigned long cells=0, box_tests=0;
	for(int arrow=0;arrow<16;arrow++) for(int tick=0;tick<20;tick++) {
		double x=.25+(double)tick*1.5;
		const double from[3]={x,100.5,(double)arrow+.5};
		const double to[3]={x+1.5,100.5,(double)arrow+.5};
		struct block_ray_stats stats;
		assert(!block_ray_trace(from,to,false,true,sparse_ray_cell,NULL,
			NULL,&stats));
		cells+=stats.cells;
		box_tests+=stats.box_tests;
	}
	assert(cells>=16u*20u && box_tests>0);
	printf("  synthetic point-ray workload: 16 arrows x 20 ticks, "
		"cells=%lu box_tests=%lu (generic primitive only)\n",cells,box_tests);
}

static void test_other_projectile_reapers(void) {
	vec3 pos = {8.0F, 100.0F, 8.0F};
	struct entity egg, snow, fire;
	const float dir[3] = {0.0F, 0.0F, 0.0F};
	entity_egg(7001, &egg, true, &s.worlds[0], 999u);
	entity_snowball(7002, &snow, true, &s.worlds[0], 999u);
	entity_fireball(7003, &fire, true, &s.worlds[0], 999u, dir);
	struct entity* shots[] = {&egg, &snow, &fire};
	for(size_t i = 0; i < 3; i++) {
		struct entity* e = shots[i];
		e->teleport(e, pos);
		if(e->type == ENTITY_FIREBALL) e->data.fireball.age = 99;
		else e->data.egg.age = 99;
		assert(!e->tick_server(e, &s));
		if(e->type == ENTITY_FIREBALL) e->data.fireball.age = 1999;
		else e->data.egg.age = 1999;
		assert(!e->tick_server(e, &s));
		e->pos[1] = -63.95F;
		e->vel[1] = -0.1F;
		assert(!e->tick_server(e, &s));
		assert(e->pos[1] < -64.0F);
		assert(e->tick_server(e, &s));
	}
}

// 2. The 0.3 box expansion: a graze 0.2 outside the raw box hits; 0.4 outside misses.
//    A zombie box is 0.6 wide (center +/- 0.3); an arrow holding x = center + 0.5 is 0.2
//    outside the raw +x face but inside the 0.3-grown [center-0.6, center+0.6].
static bool fire_past(float mob_x, float mob_z, float arrow_x_off) {
	vec3 mpos = {mob_x, 70.0f, mob_z};
	struct entity* mob = server_local_spawn_monster(mpos, MONSTER_ZOMBIE, &s);
	assert(mob);
	mob->pos[0] = mob_x; mob->pos[1] = 70.0f; mob->pos[2] = mob_z; // pin (not ticked)
	mob->data.monster.hurt_time = 0;
	short hp0 = mob->health;

	// Arrow sweeps +z at a fixed x offset, mid-body height, from 1.5 blocks before the
	// mob, 2 blocks/tick (clears the mob's z-range in one tick).
	vec3 apos = {mob_x + arrow_x_off, 70.85f, mob_z - 1.5f};
	vec3 avel = {0.0f, 0.0f, 2.0f};
	struct entity* a = spawn_arrow_now(apos, avel, 999u);
	a->tick_server(a, &s);

	return mob->health < hp0 || mob->data.monster.hurt_time > 0; // was the mob hit?
}

static void test_hitbox_expand(void) {
	// 0.2 outside the raw +x face (x = center + 0.5): inside the 0.3-grown box -> HIT.
	assert(fire_past(10.0f, 10.0f, 0.5f));
	printf("  arrow 0.3 expand: graze 0.2 outside raw box HITS OK\n");
	// 0.4 outside (x = center + 0.7): beyond the 0.3 grow [.., center+0.6] -> MISS.
	assert(!fire_past(30.0f, 30.0f, 0.7f));
	printf("  arrow 0.3 expand: graze 0.4 outside (beyond 0.3) MISSES OK\n");
}

static struct entity* pinned_zombie(float x, float z) {
	vec3 pos = {x, 70.0F, z};
	struct entity* mob = server_local_spawn_monster(pos, MONSTER_ZOMBIE, &s);
	assert(mob);
	mob->pos[0] = x;
	mob->pos[1] = 70.0F;
	mob->pos[2] = z;
	mob->health = 20;
	return mob;
}

static struct entity* pinned_mob(int monster_id, float x, float z) {
	vec3 pos = {x, 70.0F, z};
	struct entity* mob = server_local_spawn_monster(pos, monster_id, &s);
	assert(mob);
	mob->pos[0] = x;
	mob->pos[1] = 70.0F;
	mob->pos[2] = z;
	return mob;
}

static void test_absorbed_arrow_bounce(void) {
	struct entity* mob = pinned_zombie(20.0F, 20.0F);
	mob->data.monster.hurt_time = 5;
	mob->data.monster.hurt_resist_time = 5;
	mob->data.monster.last_damage = arrow_hit_damage(false);
	vec3 pos = {20.0F, 70.85F, 18.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* arrow = spawn_arrow_now(pos, vel, 999u);
	assert(!arrow->tick_server(arrow, &s));
	assert(mob->health == 20);
	assert(arrow->data.arrow.age == 0 && !arrow->data.arrow.in_ground);
	assert(feq(arrow->pos[2], 18.3F));
	assert(feq(arrow->vel[2], -0.2F * ARROW_AIR_DRAG));
	assert(feq(arrow->vel[1], -ARROW_GRAVITY));
	puts("  absorbed arrow: reverses -0.1, resets flight age, and survives OK");
}

static void test_excess_arrow_is_accepted(void) {
	struct entity* mob = pinned_zombie(24.0F, 20.0F);
	mob->data.monster.hurt_time = 5;
	mob->data.monster.hurt_resist_time = 5;
	mob->data.monster.last_damage = 1;
	vec3 pos = {24.0F, 70.85F, 18.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* arrow = spawn_arrow_now_kind(pos, vel, 0u, true);
	assert(arrow->tick_server(arrow, &s));
	assert(mob->health == 17); /* incoming 4 - prior strongest 1 */
	assert(mob->data.monster.last_damage == arrow_hit_damage(false));
	assert(mob->data.monster.hurt_time == 5
		   && mob->data.monster.hurt_resist_time == 5);
	assert(mob->data.monster.aggro_time == MOB_TARGET_RETENTION_TICKS);
	puts("  stronger in-window arrow: excess damage accepted; arrow consumed OK");
}

static void test_player_owner_grace_and_absorbed_bounce(void) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 28.0;
	s.player.y = 70.0 + PLAYER_EYE_HEIGHT;
	s.player.z = 20.0;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 5;
	s.player.last_damage = ARROW_DAMAGE;

	vec3 pos = {28.0F, 70.85F, 18.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* grace = spawn_arrow_now_kind(pos, vel, 0u, true);
	grace->data.arrow.age = ARROW_OWNER_GRACE_TICKS - 2; /* increments to 4 */
	assert(!grace->tick_server(grace, &s));
	assert(grace->data.arrow.age == ARROW_OWNER_GRACE_TICKS - 1);
	assert(feq(grace->pos[2], 20.5F));
	assert(s.player.health == MAX_PLAYER_HEALTH);

	struct entity* returned = spawn_arrow_now_kind(pos, vel, 0u, true);
	returned->data.arrow.age = ARROW_OWNER_GRACE_TICKS - 1; /* increments to 5 */
	assert(!returned->tick_server(returned, &s));
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(returned->data.arrow.age == 0);
	assert(feq(returned->pos[2], 18.3F));
	assert(feq(returned->vel[2], -0.2F * ARROW_AIR_DRAG));
	assert(feq(returned->vel[1], -ARROW_GRAVITY));
	s.player.has_pos = false;
	s.player.finished_loading = false;
	puts("  player owner grace: immune through tick 4; absorbed return at tick 5 bounces OK");
}

/* R-S1a2 F1 REGRESSION: a MOB- or DISPENSER-fired arrow must be able to hit
 * the player at tick 0. Beta's owner grace excludes the SHOOTING ENTITY only
 * -- EntityArrow.onUpdate skips a candidate iff `var9 == this.shootingEntity
 * && this.ticksInAir < 5` (cross-checked in ../McBetaCpp
 * src/net/minecraft/src/EntityArrow.cpp:161, `!canBeCollidedWith() ||
 * (candidate == owner && ticksInAir < 5)`), and a skeleton/dispenser arrow's
 * shooter is never the player. S1-a2 replaced the id compare with a pure
 * ACTOR-IDENTITY compare (`actor_ident_is_host(shooter_actor)`), and every
 * arrow -- skeleton (entity_monster.c's server_local_spawn_arrow) and
 * dispenser (the DISPENSER_PROJECTILE_OWNER_SENTINEL path) alike -- carries
 * the HOST default identity, so the host was graced against arrows it never
 * fired and a point-blank skeleton became harmless. dispenser_logic.h's own
 * sentinel comment names exactly this hazard ("passing 0 here would wrongly
 * grant the PLAYER a 5-tick immunity window against a dispensed
 * projectile"). Both legs are needed: shooter is a local player body
 * (owner_id == 0) AND it is this body. */
static void test_mob_and_dispenser_arrows_hit_player_at_tick_zero(void) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Chunks 0..2 are the loaded fixture (x,z < 48), and every pinned mob in
	 * this file sits at z == 20 or z == 24 -- so this lane is empty world. */
	s.player.x = 44.0;
	s.player.y = 70.0 + PLAYER_EYE_HEIGHT;
	s.player.z = 30.0;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;

	vec3 pos = {44.0F, 70.85F, 28.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	/* A skeleton arrow: owner_id is the skeleton entity id. */
	struct entity* skel = spawn_arrow_now(pos, vel, 777u);
	assert(skel->data.arrow.age == 0);
	assert(actor_ident_is_host(skel->data.arrow.shooter_actor));
	assert(skel->tick_server(skel, &s)); /* consumed == it hit */
	assert(s.player.health == MAX_PLAYER_HEALTH - ARROW_DAMAGE);

	/* A DISPENSED arrow: the no-owner sentinel, same rule. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;
	struct entity* disp
		= spawn_arrow_now_kind(pos, vel, DISPENSER_PROJECTILE_OWNER_SENTINEL,
							   true /* doesArrowBelongToPlayer, Beta */);
	assert(disp->tick_server(disp, &s));
	assert(s.player.health == MAX_PLAYER_HEALTH - ARROW_DAMAGE);

	/* ...while the HOST's own arrow keeps its five-tick self-immunity. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;
	struct entity* own = spawn_arrow_now_kind(pos, vel, 0u, true);
	assert(!own->tick_server(own, &s));
	assert(s.player.health == MAX_PLAYER_HEALTH);

	s.player.has_pos = false;
	s.player.finished_loading = false;
	puts("  mob/dispenser arrows: hit the player at tick 0 (owner grace is the "
		 "SHOOTER only); the player's own arrow still graced OK");
}

static void test_death_flop_is_collidable_and_bounces_arrow(void) {
	struct entity* corpse = pinned_zombie(32.0F, 20.0F);
	corpse->health = 0;
	corpse->data.monster.death_time = 1;
	vec3 pos = {32.0F, 70.85F, 18.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* arrow = spawn_arrow_now(pos, vel, 999u);
	assert(!arrow->tick_server(arrow, &s));
	assert(arrow->data.arrow.age == 0);
	assert(feq(arrow->pos[2], 18.3F));
	puts("  death-flop collision: attack rejects, arrow bounces instead of ghosting OK");
}

static void test_equal_distance_keeps_first_chunk_list_candidate(void) {
	struct entity* first = pinned_zombie(36.0F, 20.0F);
	struct entity* second = pinned_zombie(36.0F, 20.0F);
	assert(first->chunk_list_order < second->chunk_list_order);
	vec3 pos = {36.0F, 70.85F, 18.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* arrow = spawn_arrow_now(pos, vel, 999u);
	assert(arrow->tick_server(arrow, &s));
	assert(first->health == 16 && second->health == 20);
	puts("  equal-distance tie: first Beta chunk-list candidate wins OK");
}

static void test_arrow_retaliation_ownership(void) {
	vec3 pos = {40.0F, 70.85F, 18.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* pig = pinned_zombie(40.0F, 20.0F);
	pig->data.monster.id = MONSTER_PIG;
	struct entity* player_arrow = spawn_arrow_now_kind(pos, vel, 0u, true);
	assert(player_arrow->tick_server(player_arrow, &s));
	assert(pig->data.monster.aggro_time == 0);

	struct entity* zombie = pinned_zombie(44.0F, 20.0F);
	pos[0] = 44.0F;
	struct entity* skeleton_arrow = spawn_arrow_now(pos, vel, 777u);
	skeleton_arrow->data.arrow.from_skeleton = true;
	assert(skeleton_arrow->tick_server(skeleton_arrow, &s));
	assert(zombie->data.monster.aggro_time == 0);
	puts("  arrow retaliation: passive and skeleton-arrow victims do not target player OK");
}

// PIG ZOMBIE SELF-ANGER (parity fix 2026-09-02, decompile-verified,
// 740c583:EntityPigZombie.attackEntityFrom + becomeAngryAt;
// EntityArrow.java:163, the hit entity takes attackEntityFrom(owner, 4)):
//     EntityPigZombie.attackEntityFrom(attacker, damage):
//         when the attacker is an EntityPlayer,
//             every pig zombie in the 32-block box becomes angry at it,
//             and so does the struck one;
//         (end of the player branch)
//         then the base attackEntityFrom result is returned.
//     becomeAngryAt: angerLevel = 400 + nextInt(400); randomSoundDelay = nextInt(40);
// A PLAYER-fired arrow's owner satisfies `instanceof EntityPlayer`, so it must
// arm the struck pig zombie's own anger_level -- even on an i-frame-ABSORBED
// hit, since becomeAngryAt precedes the base attackEntityFrom's health/i-frame
// gate. A SKELETON-fired arrow (infighting) must NOT anger it (the shooter is
// not a player), and an already-flopping (death_time>0) pig zombie IS
// re-armed (PIGZ-PACK: the override precedes super's health gate). The pack
// half is pinned by test_pig_zombie_pack_anger.
static void test_arrow_pig_zombie_self_anger(void) {
	// Fresh hit: angers.
	struct entity* pz = pinned_mob(MONSTER_PIG_ZOMBIE, 10.0F, 24.0F);
	assert(pz->data.monster.anger_level == 0);
	vec3 pos = {10.0F, 70.85F, 22.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* arrow = spawn_arrow_now_kind(pos, vel, 0u, true);
	assert(arrow->tick_server(arrow, &s));
	assert(pz->data.monster.anger_level >= 400
		   && pz->data.monster.anger_level < 800);
	printf("  pig zombie arrow self-anger: fresh player hit -> anger_level=%d OK\n",
		   pz->data.monster.anger_level);

	// ABSORBED hit (active i-frame window, an equal-or-stronger prior hit):
	// still angers -- becomeAngryAt precedes the health/i-frame gate in Beta.
	struct entity* pz2 = pinned_mob(MONSTER_PIG_ZOMBIE, 14.0F, 24.0F);
	pz2->data.monster.hurt_time = 5;
	pz2->data.monster.hurt_resist_time = 5;
	pz2->data.monster.last_damage = arrow_hit_damage(false);
	pos[0] = 14.0F;
	struct entity* arrow2 = spawn_arrow_now_kind(pos, vel, 0u, true);
	assert(!arrow2->tick_server(arrow2, &s)); // absorbed -> bounces, not consumed
	assert(pz2->data.monster.anger_level >= 400
		   && pz2->data.monster.anger_level < 800);
	printf("  pig zombie arrow self-anger: i-frame ABSORBED hit still angers "
		   "(anger_level=%d) OK\n",
		   pz2->data.monster.anger_level);

	// Skeleton-fired arrow (infighting): damage lands, but the shooter is not
	// a player, so anger must NOT arm.
	struct entity* pz3 = pinned_mob(MONSTER_PIG_ZOMBIE, 18.0F, 24.0F);
	pos[0] = 18.0F;
	struct entity* skel_arrow = spawn_arrow_now(pos, vel, 777u);
	skel_arrow->data.arrow.from_skeleton = true;
	assert(skel_arrow->tick_server(skel_arrow, &s));
	assert(pz3->data.monster.anger_level == 0);
	printf("  pig zombie arrow self-anger: skeleton-fired arrow does NOT anger "
		   "(not EntityPlayer) OK\n");

	// Already death-flopping: PIGZ-PACK -- Beta DOES re-arm it. The corpse is
	// still collidable (EntityLiving.canBeCollidedWith = !isDead), the arrow
	// calls attackEntityFrom(owner, 4), and EntityPigZombie's override runs
	// becomeAngryAt BEFORE super's `health <= 0` return [server
	// EntityPigZombie.java:49-64, EntityLiving.java:293-299]; that false
	// return is what bounces the arrow. Exactly the two anger draws, no pitch.
	struct entity* pz4 = pinned_mob(MONSTER_PIG_ZOMBIE, 22.0F, 24.0F);
	pz4->health = 0;
	pz4->data.monster.death_time = 1;
	struct java_random r4 = pz4->gameplay_rng;
	const int want_anger4 = 400 + jrand_next_int_bound(&r4, 400);
	const int want_delay4 = jrand_next_int_bound(&r4, 40);
	pos[0] = 22.0F;
	struct entity* arrow4 = spawn_arrow_now_kind(pos, vel, 0u, true);
	assert(!arrow4->tick_server(arrow4, &s)); // death-flop rejects, arrow bounces
	assert(pz4->data.monster.anger_level == want_anger4
		   && pz4->data.monster.anger_sound_delay == want_delay4
		   && pz4->gameplay_rng.seed == r4.seed);
	printf("  pig zombie arrow anger: death-flop corpse is re-armed (Beta) OK\n");
}

/* S1-a3: the arrow sweep enumerates LOCAL PLAYER BODIES through
 * server_local_damage_candidate instead of reading s->player. With no split
 * guest admitted that enumeration must yield the host and NOTHING else --
 * which is why every case above (and the whole OFF path) is unchanged by the
 * two-body sweep. It also pins the corpse rule the sweep must NOT invent: a
 * player at health 0 is not isDead, so it stays a swept candidate and the HIT
 * is refused downstream (accepted=false, the arrow bounces) -- the same shape
 * test_death_flop_is_collidable_and_bounces_arrow pins for a mob. */
/* EntityArrow.java:120 queries the arrow's worldObj before the six-face
 * intercept. A nearer Nether pig zombie must not steal an Overworld hit. */
static void test_arrow_world_overlap(void) {
	s.player.has_pos = false;
	s.player.finished_loading = false;
	struct server_world* nether = &s.worlds[1];
	string_t path;
	string_init_set_str(path, test_fixture_join(TMP_WORLD, "/nether"));
	server_world_create(nether, path, WORLD_DIM_NETHER, 4242, true);
	string_clear(path);
	nether->session = &s;
	nether->world_index = 1;
	struct entity* foreign = pinned_mob(MONSTER_PIG_ZOMBIE, 44, 38);
	foreign->world = nether;
	foreign->world_slot = 1;
	foreign->world_generation = nether->generation;
	struct entity* local = pinned_mob(MONSTER_PIG_ZOMBIE, 44, 39);
	struct java_random foreign_rng = foreign->gameplay_rng;
	struct entity* a = spawn_arrow_now_kind((vec3){44,70.85F,36.5F},
		(vec3){0,0,3}, 0, true);
	a->data.arrow.age = 10;
	assert(a->tick_server(a, &s));
	assert(foreign->health == 20 && foreign->data.monster.anger_level == 0
		&& foreign->gameplay_rng.seed == foreign_rng.seed);
	assert(local->health == 16 && local->data.monster.anger_level >= 400);
	/* Retire this fixture world before the visual test admits its own slot1. */
	foreign->delay_destroy = 0;
	server_world_destroy(nether);
	memset(nether, 0, sizeof *nether);
	puts("  nearer overlapping Nether pig zombie cannot steal Overworld arrow hit OK");
}

static void test_damage_candidates_are_host_only_without_a_guest(void) {
	assert(!s.guest_actor && !s.guest_gameplay);
	assert(server_local_damage_candidate(&s, 0) == &s.player);
	assert(server_local_damage_candidate(&s, 1) == NULL);
	assert(server_local_damage_candidate(&s, 2) == NULL);

	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 60.0;
	s.player.y = 71.0;
	s.player.z = 60.0;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	vec3 pos = {(float)s.player.x, (float)s.player.y - 0.85f,
				(float)s.player.z - 1.5f};
	vec3 vel = {0.0f, 0.0f, 2.0f};
	struct entity* live_hit = spawn_arrow_now(pos, vel, 999u);
	live_hit->tick_server(live_hit, &s);
	assert(s.player.health < MAX_PLAYER_HEALTH);

	/* Now kill the body and fire the identical shot: still a candidate, but
	 * the hit is refused and the arrow BOUNCES instead of passing through. */
	s.player.health = 0;
	s.player.hurt_resist_time = 0;
	struct entity* corpse_shot = spawn_arrow_now(pos, vel, 999u);
	float vz_before = corpse_shot->vel[2];
	corpse_shot->tick_server(corpse_shot, &s);
	/* Rejected-hit visual bounce adds180 to BOTH yaw buffers before unwrap. */
	assert(feq(corpse_shot->data.arrow.visual.previous[0],-180.0F));
	assert(feq(corpse_shot->data.arrow.visual.rotation[0],-180.0F));
	assert(s.player.health == 0);          /* no damage to a corpse */
	assert(s.player.hurt_resist_time == 0); /* and no i-frame churn */
	assert(corpse_shot->vel[2] < vz_before); /* the rejected-hit bounce */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	printf("  damage candidates: host only with no guest; a corpse is still a "
		   "candidate and bounces the arrow OK\n");
}


/* Lifecycle presentation: REAL server tick -> existing SPAWN/MOVE producer ->
 * actual client RPC consumer/tick. No movement, pickup or RNG state is driven
 * by this observation. Reference order: EntityArrow.java:65-66,87-104,170-172,
 * 189,197-215 and RenderArrow.java:8-28 (Beta 1.7.3). */
static struct client_rpc visual_call, visual_spawn_call;
static unsigned visual_moves, visual_spawns;
static uint32_t visual_id;
static void visual_observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_ENTITY_MOVE
	   && (!visual_id || call->payload.entity_move.entity_id == visual_id)) {
		visual_call = *call;
		visual_moves++;
	} else if(call->type == CRPC_SPAWN_ARROW
		&& (!visual_id || call->payload.spawn_arrow.entity_id == visual_id)) {
		visual_call = *call;
		visual_spawns++;
		visual_spawn_call = *call;
	}
}

static void visual_beta_direction(const vec3 velocity, vec2 direction) {
	float horizontal = (float)sqrt((double)velocity[0]*velocity[0]
		+ (double)velocity[2]*velocity[2]);
	direction[0] = (float)(atan2((double)velocity[0],(double)velocity[2])
		*180.0/3.1415927410125732421875);
	direction[1] = (float)(atan2((double)velocity[1],(double)horizontal)
		*180.0/3.1415927410125732421875);
}

/* Headless stand-in for a consuming client PLUS the server's tick-start
 * clin_rpc_flush_pending (test_rpc_pending_output.c:538). This case runs after
 * the background drain thread is joined, and that thread only recycles inbox
 * tokens: every send the earlier cases made while the 16-token pool was empty
 * is still in the server-owned pending ring (79 entries measured here).
 * clin_try_rpc_send refuses to overtake pending output (SOL-REVIEW-d42f
 * Major 1), so the SPAWN re-publish below was refused by design. Discard
 * the inbox and flush the ring until both are empty, as production does
 * between updates. */
static void visual_drain_server_output(void) {
	for(int k = 0; k < 256; k++) {
		clin_drain_discard();
		if(clin_rpc_flush_pending()) {
			clin_drain_discard();
			uint32_t pending = 1;
			clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
			assert(pending == 0);
			return;
		}
	}
	assert(!"server pending output never drained");
}

static void test_arrow_visual_lifecycle(void) {
	visual_drain_server_output(); /* earlier cases left deferred output */
	s.player.has_pos = false;
	s.player.finished_loading = false;
	/* Beta's both-zero reinitialization runs again on tick2 after gravity. */
	struct entity* initial=spawn_arrow_now((vec3){42,100,42},(vec3){0,0,1},999u);
	assert(!initial->tick_server(initial,&s));
	visual_drain_server_output();
	assert(initial->data.arrow.visual.previous[0]==0.0F
		&& initial->data.arrow.visual.previous[1]==0.0F);
	assert(!initial->tick_server(initial,&s));
	visual_drain_server_output();
	assert(initial->data.arrow.visual.previous[1]<0.0F);
	assert(initial->data.arrow.visual.previous[1]==initial->data.arrow.visual.rotation[1]);
	struct entity* a = spawn_arrow_now((vec3){35.5F,76.5F,35.5F},
		(vec3){0.2F,0.4F,-0.8F},999u);
	visual_id = a->id;
	visual_moves = visual_spawns = 0;
	clin_test_set_rpc_observer(visual_observe, NULL);
	vec2 expected;
	visual_beta_direction(a->vel, expected);
	struct random_gen rng = s.entity_rng;
	assert(!a->tick_server(a,&s));
	assert(visual_moves == 1 && visual_call.payload.entity_move.arrow_visual.valid);
	assert(memcmp(&rng,&s.entity_rng,sizeof rng) == 0);
	assert(memcmp(a->data.arrow.visual.rotation,expected,sizeof expected) == 0);
	assert(memcmp(a->data.arrow.visual.previous,expected,sizeof expected) == 0);
	visual_drain_server_output();
	/* Existing SPAWN publication carries the same authoritative observation. */
	assert(server_local_try_publish_entity_w(&s,&s.worlds[0],a));
	assert(visual_spawns == 1 && visual_call.payload.spawn_arrow.visual.valid);
	assert(memcmp(&visual_call.payload.spawn_arrow.visual,&a->data.arrow.visual,
		sizeof a->data.arrow.visual) == 0);
	visual_drain_server_output();
	items_init();
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct client_rpc reset = {.type=CRPC_WORLD_RESET,
		.payload.world_reset={.dimension=s.worlds[0].dimension,
			.world_index=s.worlds[0].world_index,.world_generation=s.worlds[0].generation}};
	clin_process(&reset);
	struct client_world_context* ctx=client_ctx_registry_current(clin_world_contexts(),0);
	assert(ctx && !ctx->unstamped && ctx->server_generation==s.worlds[0].generation);
	clin_process(&visual_call);
	struct entity** slot = dict_entity_get(gstate.entities,a->id);
	assert(slot && *slot && (*slot)->data.arrow.visual.valid);
	struct entity* client = *slot;
	assert(client->data.arrow.visual_pending);
	assert(!client->tick_client(client));
	assert(!client->data.arrow.visual_pending);
	/* Curve update: previous yaw is preserved, pitch samples pre-drag motion. */
	vec2 prior;
	glm_vec2_copy(a->data.arrow.visual.rotation,prior);
	visual_beta_direction(a->vel,expected);
	for(int axis=0;axis<2;axis++) {
		while(expected[axis]-prior[axis]<-180.0F)prior[axis]-=360.0F;
		while(expected[axis]-prior[axis]>=180.0F)prior[axis]+=360.0F;
		expected[axis]=prior[axis]+(expected[axis]-prior[axis])*0.2F;
	}
	assert(!a->tick_server(a,&s));
	assert(visual_moves == 2);
	assert(memcmp(a->data.arrow.visual.rotation,expected,sizeof expected)==0);
	visual_drain_server_output();
	clin_process(&visual_call);
	assert(memcmp(&client->data.arrow.visual,&a->data.arrow.visual,
		sizeof a->data.arrow.visual)==0);
	/* Actual point-ray impact on a placed block sends shake7 in its existing MOVE. */
	struct block_data stone={.type=BLOCK_STONE};
	assert(server_world_set_block(&s.worlds[0],37,76,35,stone));
	a->teleport(a,(vec3){36.5F,76.5F,35.5F});
	glm_vec3_copy((vec3){1.0F,0.0F,0.0F},a->vel);
	assert(!a->tick_server(a,&s));
	assert(a->data.arrow.in_ground && a->data.arrow.shake_ticks==7);
	assert(a->data.arrow.visual.shake_ticks==7 && visual_moves==3);
	visual_drain_server_output();
	clin_process(&visual_call);
	assert(!client->tick_client(client));
	assert(client->data.arrow.visual.shake_ticks==7); /* no receipt double-decrement */
	unsigned impact_moves=visual_moves;
	for(unsigned tick=0;tick<2;tick++) {
		assert(!a->tick_server(a,&s));
		assert(!client->tick_client(client));
		assert(a->data.arrow.shake_ticks==(int)(6-tick));
		assert(client->data.arrow.visual.shake_ticks==6-tick);
		assert(visual_moves==impact_moves); /* no new stationary RPC */
	}
	/* Remove the REAL embedded block. Simulation's pickup counter stays5;
	 * independent cosmetic timer advances through the early-return release. */
	struct block_data air={.type=BLOCK_AIR};
	assert(server_world_set_block(&s.worlds[0],37,76,35,air));
	assert(!a->tick_server(a,&s));
	assert(!a->data.arrow.in_ground && a->data.arrow.shake_ticks==5);
	assert(a->data.arrow.visual.shake_ticks==4 && visual_moves==impact_moves);
	assert(!client->tick_client(client));
	for(unsigned tick=0;tick<5;tick++) {
		assert(!a->tick_server(a,&s));
		visual_drain_server_output();
		clin_process(&visual_call);
		assert(!client->tick_client(client));
		assert(client->data.arrow.visual.shake_ticks==a->data.arrow.visual.shake_ticks);
	}
	assert(a->data.arrow.shake_ticks==5 && a->data.arrow.visual.shake_ticks==0);
	/* Actual consumer drops cosmetic writes from a wrong world or unknown generation. */
	struct arrow_visual_state before=client->data.arrow.visual;
	struct client_rpc foreign=visual_call;
	foreign.world_index=1;foreign.world_generation=12345;
	foreign.payload.entity_move.arrow_visual.shake_ticks=7;
	clin_process(&foreign);
	assert(memcmp(&before,&client->data.arrow.visual,sizeof before)==0);
	foreign.world_index=0;foreign.world_generation=12345;
	clin_process(&foreign);
	assert(memcmp(&before,&client->data.arrow.visual,sizeof before)==0);
	/* Existing archive serialization + restoration + actual republish: current
	 * radians persist, previous becomes current at the load boundary. */
	struct archived_entity archive;
	server_local_test_archive_entity(a,&archive);
	visual_id=0;
	struct chunk_entities archived={.entities=&archive,.count=1};
	assert(server_local_restore_chunk_entities(&s,&archived));
	assert(visual_call.type==CRPC_SPAWN_ARROW && visual_call.payload.spawn_arrow.visual.valid);
	struct entity** restored=dict_entity_get(s.entities,visual_call.payload.spawn_arrow.entity_id);
	assert(restored && *restored);
	assert(memcmp((*restored)->data.arrow.visual.previous,
		(*restored)->data.arrow.visual.rotation,sizeof(vec2))==0);
	assert(feq((*restored)->data.arrow.visual.rotation[0],glm_deg(archive.orient[0])));
	assert(feq((*restored)->data.arrow.visual.rotation[1],glm_deg(archive.orient[1])));
	assert((*restored)->data.arrow.shake_ticks==5 && (*restored)->data.arrow.visual.shake_ticks==0);
	visual_drain_server_output();
	/* Historic zero and unsafe finite/infinite saved Rotation fall back; the
	 * actual next airborne tick seeds bounded observed velocity and terminates. */
	float saved_rotations[]={0.0F,1e20F,INFINITY};
	for(unsigned row=0;row<3;row++) {
		archive.orient[0]=archive.orient[1]=saved_rotations[row];
		assert(server_local_restore_chunk_entities(&s,&archived));
		assert(!visual_call.payload.spawn_arrow.visual.valid);
		restored=dict_entity_get(s.entities,visual_call.payload.spawn_arrow.entity_id);
		assert(restored && *restored);
		assert(!(*restored)->tick_server(*restored,&s));
		assert((*restored)->data.arrow.visual.valid);
		visual_drain_server_output();
	}
	/* Exercise the ORIGINAL queued spawn path, not only republication. */
	server_local_refresh_entity_membership(&s,a);
	server_local_spawn_arrow((vec3){35,90,35},(vec3){.5F,.2F,.1F},999,
		ITEM_ARROW,ARROW_DAMAGE,false,&s);
	unsigned queued_before=visual_spawns;
	assert(server_local_test_tick_entities(&s));
	assert(visual_spawns==queued_before+1 && visual_spawn_call.type==CRPC_SPAWN_ARROW);
	assert(visual_spawn_call.payload.spawn_arrow.visual.valid);
	visual_drain_server_output();
	/* Remote/default envelope has no visual sample and retains its fallback. */
	struct client_rpc remote={.type=CRPC_SPAWN_ARROW,
		.world_index=s.worlds[0].world_index,.world_generation=s.worlds[0].generation,
		.payload.spawn_arrow={.entity_id=65001,.item_id=ITEM_ARROW,
			.pos={4,76,4},.vel={1,0,1}}};
	clin_process(&remote);
	struct entity** remote_slot=dict_entity_get(gstate.entities,65001);
	assert(remote_slot && !(*remote_slot)->data.arrow.visual.valid);
	/* Cosmetic state from an unrelated projectile kind cannot enter the union. */
	remote.payload.spawn_arrow.entity_id=65002;
	remote.payload.spawn_arrow.item_id=ITEM_EGG;
	remote.payload.spawn_arrow.visual=a->data.arrow.visual;
	clin_process(&remote);
	struct entity** egg_slot=dict_entity_get(gstate.entities,65002);
	assert(egg_slot && !(*egg_slot)->data.arrow.visual.valid);
	/* A second REAL server world is capacity-admitted and installed through the
	 * public guest RESET. Its production SPAWN/MOVE must reach only ctx1. */
	struct entity primary_before=*client;
	struct server_world* secondary=&s.worlds[1];
	string_t secondary_path;
	string_init_set_str(secondary_path,test_fixture_join(TMP_WORLD,"/secondary"));
	server_world_create(secondary,secondary_path,WORLD_DIM_OVERWORLD,4243,true);
	string_clear(secondary_path);
	secondary->local=&s;secondary->session=&s;secondary->world_index=1;
	struct clin_capacity_need need={0};
	clin_capacity_ticket ticket=0;
	assert(clin_capacity_reserve_world(1,secondary->generation,&need,&ticket)==CLIN_CAP_OK);
	assert(ticket && clin_capacity_commit(ticket));
	reset.payload.world_reset=(typeof(reset.payload.world_reset)) {
		.dimension=secondary->dimension,.world_index=1,
		.world_generation=secondary->generation,.guest_world=true,.guest_view=1};
	clin_process(&reset);
	struct client_world_context* secondary_ctx=client_ctx_registry_current(clin_world_contexts(),1);
	assert(secondary_ctx && !secondary_ctx->unstamped
		&& secondary_ctx->server_generation==secondary->generation
		&& secondary_ctx->bound_views==2 && clin_world_payload_ready(1));
	struct clin_world_payload secondary_payload;
	assert(clin_world_payload_of_slot(1,&secondary_payload));
	assert(secondary_payload.terrain!=&gstate.world && secondary_payload.entities!=gstate.entities);
	uint32_t secondary_id=g_next_id++;
	struct entity** secondary_arrow=dict_entity_safe_get(s.entities,secondary_id);
	*secondary_arrow=calloc(1,sizeof(struct entity));
	assert(*secondary_arrow);
	entity_arrow(secondary_id,*secondary_arrow,true,secondary,999u,ITEM_ARROW,ARROW_DAMAGE,false);
	(*secondary_arrow)->teleport(*secondary_arrow,(vec3){8,100,8});
	glm_vec3_copy((vec3){.3F,.2F,.7F},(*secondary_arrow)->vel);
	visual_id=secondary_id;
	assert(!(*secondary_arrow)->tick_server(*secondary_arrow,&s));
	visual_drain_server_output();
	assert(server_local_try_publish_entity_w(&s,secondary,*secondary_arrow));
	assert(visual_call.type==CRPC_SPAWN_ARROW && visual_call.world_index==1
		&& visual_call.world_generation==secondary->generation
		&& visual_call.payload.spawn_arrow.visual.valid);
	visual_drain_server_output();
	clin_process(&visual_call);
	struct entity** secondary_client=dict_entity_get(secondary_payload.entities,secondary_id);
	assert(secondary_client && *secondary_client
		&& (*secondary_client)->world==secondary_payload.terrain);
	assert(memcmp(&(*secondary_client)->data.arrow.visual,&(*secondary_arrow)->data.arrow.visual,
		sizeof(struct arrow_visual_state))==0);
	assert(!(*secondary_arrow)->tick_server(*secondary_arrow,&s));
	assert(visual_call.type==CRPC_ENTITY_MOVE && visual_call.world_index==1
		&& visual_call.world_generation==secondary->generation);
	visual_drain_server_output();
	clin_process(&visual_call);
	assert(memcmp(&(*secondary_client)->data.arrow.visual,&(*secondary_arrow)->data.arrow.visual,
		sizeof(struct arrow_visual_state))==0);
	assert(memcmp(&primary_before,client,sizeof primary_before)==0);
	assert(clin_capacity_release(ticket));
	puts("  actual admitted ctx1 SPAWN/MOVE visual; ctx0 arrow byte-identical OK");
	clin_test_set_rpc_observer(NULL,NULL);
	clin_force_world_reset();
	client_ctx_registry_retire(clin_world_contexts(),0);
	client_ctx_registry_poll(clin_world_contexts());
	assert(clin_world_payloads_set_enabled(false));
	puts("  actual arrow SPAWN/MOVE/client: rotation, impact7, stationary0RPC, unembed decay, fallback OK");
}

/* F4-RNG (brief 18, RNG ruling 9): the arrow impact "random.drr" pitch is
 * 1.2F / (this.rand.nextFloat() * 0.2F + 0.9F) on the ARROW's own Entity.rand
 * (EntityArrow.onUpdate:148 accepted entity hit, :175 block impact) -- the
 * same gameplay_rng as the heading Gaussians, with the Gaussian cache intact;
 * a rejected (absorbed) hit draws nothing. JVM witness (F4RngWitness.java,
 * OpenJDK 21.0.7, output sha256 2736a03f...): new Random(0x6A55CA5EL):
 * nextGaussian() -> bits 0xbff0094a01b1648d, then nextFloat() -> 0x3f1fdbb5
 * (served from the LCG, the cached Gaussian untouched), final state
 * 0x9fdbb58daae6. */
#define F4A_GAUSS_SEED INT64_C(0x6A55CA5E)
#define F4A_G0_BITS UINT64_C(0xbff0094a01b1648d)
#define F4A_F1_BITS 0x3f1fdbb5u
#define F4A_FINAL UINT64_C(0x9fdbb58daae6)
static float f4a_pitch[16];
static size_t f4a_pitch_n;
static void f4a_observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SOUND && call->payload.sound.sound_id == SOUND_RANDOM_DRR
	   && f4a_pitch_n < 16)
		f4a_pitch[f4a_pitch_n++] = call->payload.sound.pitch;
}
static uint32_t f4a_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void f4a_prime(struct entity* a) {
	jrand_set_seed(&a->gameplay_rng, F4A_GAUSS_SEED);
	double g0 = jrand_next_gaussian(&a->gameplay_rng);
	uint64_t gb;
	memcpy(&gb, &g0, 8);
	assert(gb == F4A_G0_BITS && a->gameplay_rng.have_next_gaussian);
}
static void test_f4rng_arrow_impact_stream(void) {
	int fails = 0;
	float f1;
	uint32_t f1b = F4A_F1_BITS;
	memcpy(&f1, &f1b, 4);
	const float want = 1.2F / (f1 * 0.2F + 0.9F);
	clin_test_set_rpc_observer(f4a_observe, NULL);

	/* (1) accepted entity hit: one draw AFTER the victim, cache intact */
	struct entity* mob = pinned_zombie(28.0F, 40.0F);
	vec3 pos = {28.0F, 70.85F, 38.5F};
	vec3 vel = {0.0F, 0.0F, 2.0F};
	struct entity* arrow = spawn_arrow_now(pos, vel, 999u);
	f4a_prime(arrow);
	const double cached = arrow->gameplay_rng.next_gaussian_cached;
	f4a_pitch_n = 0;
	assert(arrow->tick_server(arrow, &s));
	bool ok1 = f4a_pitch_n == 1 && f4a_bits(f4a_pitch[0]) == f4a_bits(want)
		&& arrow->gameplay_rng.seed == F4A_FINAL
		&& arrow->gameplay_rng.have_next_gaussian
		&& arrow->gameplay_rng.next_gaussian_cached == cached;
	printf("  (F4.A1) accepted hit: %zu drr, pitch %.9g (want %.9g), state %#llx "
		   "(want %#llx), cache %s\n", f4a_pitch_n, f4a_pitch_n ? (double)f4a_pitch[0] : 0.0,
		   (double)want, (unsigned long long)arrow->gameplay_rng.seed,
		   (unsigned long long)F4A_FINAL,
		   arrow->gameplay_rng.have_next_gaussian ? "kept" : "LOST");
	if(!ok1) { puts("  F4 FAIL: accepted arrow hit pitch not the arrow's gameplay_rng"); fails++; }
	(void)mob;

	/* (2) rejected (absorbed) hit: NO sound draw */
	struct entity* mob2 = pinned_zombie(32.0F, 40.0F);
	mob2->data.monster.hurt_time = 5;
	mob2->data.monster.hurt_resist_time = 5;
	mob2->data.monster.last_damage = arrow_hit_damage(false);
	vec3 pos2 = {32.0F, 70.85F, 38.5F};
	struct entity* bounced = spawn_arrow_now(pos2, vel, 999u);
	f4a_prime(bounced);
	const struct java_random before = bounced->gameplay_rng;
	f4a_pitch_n = 0;
	assert(!bounced->tick_server(bounced, &s));
	bool ok2 = f4a_pitch_n == 0 && bounced->gameplay_rng.seed == before.seed
		&& bounced->gameplay_rng.have_next_gaussian;
	printf("  (F4.A2) rejected hit: %zu drr, state %s\n", f4a_pitch_n,
		   bounced->gameplay_rng.seed == before.seed ? "unchanged" : "ADVANCED");
	if(!ok2) { puts("  F4 FAIL: rejected arrow hit consumed/sounded"); fails++; }

	/* (3) block impact: one draw on the same stream */
	assert(server_world_set_block(&s.worlds[0], 40, 75, 8,
		(struct block_data) {.type = BLOCK_STONE}));
	vec3 approach = {38.0F, 75.5F, 8.5F};
	vec3 flight = {2.25F, 0.0F, 0.0F};
	struct entity* stuck = spawn_arrow_now(approach, flight, 999u);
	f4a_prime(stuck);
	f4a_pitch_n = 0;
	assert(!stuck->tick_server(stuck, &s));
	bool ok3 = stuck->data.arrow.in_ground && f4a_pitch_n == 1
		&& f4a_bits(f4a_pitch[0]) == f4a_bits(want)
		&& stuck->gameplay_rng.seed == F4A_FINAL;
	printf("  (F4.A3) block impact: %zu drr, pitch %.9g, state %#llx\n", f4a_pitch_n,
		   f4a_pitch_n ? (double)f4a_pitch[0] : 0.0,
		   (unsigned long long)stuck->gameplay_rng.seed);
	if(!ok3) { puts("  F4 FAIL: block-impact pitch not the arrow's gameplay_rng"); fails++; }
	assert(server_world_set_block(&s.worlds[0], 40, 75, 8,
		(struct block_data) {.type = BLOCK_AIR}));

	clin_test_set_rpc_observer(NULL, NULL);
	mob->pos[1] = mob2->pos[1] = -200.0F; /* out of every later case */
	assert(fails == 0);
	puts("  F4-RNG arrow: accepted/rejected/block impact on the arrow's own Entity.rand OK");
}

int main(void) {
	test_fixture_dir("test_arrow_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 2; cx++)
		for(int cz = 0; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	printf("test_arrow_env:\n");
	test_gravity_drag_order();
	test_water_drag();
	test_arrow_reapers();
	test_removed_stuck_block();
	test_collision_derived_release_motion();
	test_thin_trapdoor_stuck_identity();
	test_fence_point_ray();
	test_cactus_ladder_and_stairs_ray();
	test_snow_presence_then_ray();
	test_moving_cell_ray_synthetic();
	test_noncolliding_cells_skipped();
	test_entity_order_against_block_ray();
	test_short_clipped_entity_intercept();
	test_entity_at_clipped_endpoint();
	test_shifted_short_entity_intercept();
	test_exact_face_zero_impact_safety();
	test_block_ray_workload();
	test_other_projectile_reapers();
	test_hitbox_expand();
	test_absorbed_arrow_bounce();
	test_excess_arrow_is_accepted();
	test_f4rng_arrow_impact_stream();
	test_player_owner_grace_and_absorbed_bounce();
	test_mob_and_dispenser_arrows_hit_player_at_tick_zero();
	test_death_flop_is_collidable_and_bounces_arrow();
	test_equal_distance_keeps_first_chunk_list_candidate();
	test_arrow_retaliation_ownership();
	test_arrow_pig_zombie_self_anger();
	test_arrow_world_overlap();

	test_damage_candidates_are_host_only_without_a_guest();

	g_drain_run = 0;
	thread_join(&drain);
	test_arrow_visual_lifecycle();
	printf("PASS test_arrow_env\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
