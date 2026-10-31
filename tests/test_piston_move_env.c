/*
	E2E production-path test for the Beta 1.7.3 PISTON move (M4 pistons, phase 2b). Drives REAL pistons
	through the production redstone_piston_server.c -- the rs_ctx accessors over a live superflat
	server_world, the SYNCHRONOUS move dispatch (Beta playNoteAt->playBlock is inline), the in-RAM
	moving-TE store, and the T+2 settle driver -- over server_world_set_block's neighbour-notify cascade.
	Unlike the stub-grid goldens (which cover only the pure decision + materialization SEQUENCE), this
	exercises the engine wiring: the power query reads the real boolean model, the move materializes into
	the real world + store, and piston_moving_te_tick settles it.

	WHAT IT CATCHES (engine-wiring regressions invisible to the goldens):
	  - the moving block occupies the cell as id-36 for the full settle window, then materializes (the
	    settle COUNT pins PISTON_SETTLE_TICKS: a moving block must NOT teleport instantly, NOR linger);
	  - the power query's SIDE semantics (the stub golden ignored side; here a real torch must power the
	    piston through the faithful redstone_power.c probe);
	  - the tile-entity + bedrock + 12-limit REFUSALS over the real accessors (ps_has_tile_entity /
	    ps_get_hardness / ps_get_mobility);
	  - the BUD "block above powers the piston" quirk through the real wiresProvidePower=true query;
	  - the sticky PULL and the non-sticky retract through the real store + settle.

	POWER SOURCE: a standing lit redstone torch (meta 5) placed as a DIRECT neighbour of the piston both
	powers it (torch_is_powering_to(meta5, side!=1)==true) AND auto-notifies it (Beta onNeighborBlockChange
	-> updatePistonState fires synchronously inside set()). For the BUD cell (y+2, not a direct neighbour)
	the piston is poked explicitly, exactly as a real block update reaches it.

	HEADLESS HARNESS: cloned from test_redstone_repeater_circuit_env.c -- a drain thread keeps the client
	inbox empty so server-thread set_block never blocks; the test reads the SERVER world synchronously.
	CRITICAL: tick() MUST call piston_moving_te_tick (it is NOT in server_world_tick).
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "block/piston_render.h"
#include "block/redstone_piston.h"		   // RS_PISTON_* ids, piston facing
#include "block/redstone_piston_server.h"  // piston_server_neighbour_change, piston_moving_te_tick
#include "block/redstone_server.h"          // verify the live y+2 power fixture before poking
#include "entity/combat.h"                  // PLAYER_EYE_HEIGHT
#include "entity/entity_monster.h"
#include "entity/entity_primed_tnt.h"
#include "game/camera.h" // direct include (was transitive via world.h pre-split)
#include "game/client_session.h"
#include "network/client_interface.h"
#include "network/piston_moving_transport.h"
#include "network/server_interface.h" // MAX_PLAYER_HEALTH for the displacement fixture
#include "network/server_local.h"
#include "network/server_local_guest.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "graphics/render_piston_moving.h"
#include "item/items.h"
#include "platform/thread.h"
#include "graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "support/drain_quiesce.h" /* quiesce-0926 */

static void output_quiesce(void) {
	test_drain_quiesce();
	while(!clin_rpc_flush_pending())
		test_drain_quiesce();
	test_drain_quiesce();
}

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_piston_move_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // working plane
#define FACE_PX 5 // Beta facing +X
#define FACE_NX 4 // Beta facing -X
#define PC_VERTEX_BYTES 22 // source/platform/pc/displaylist.c, untinted packed layout

static struct server_local s;

struct piston_rpc_probe {
	uint16_t moving_mask;
	uint16_t set_block_mask;
	int player_pos_count;
	uint32_t player_correction_seq;
	uint8_t player_correction_flags;
	int guest_correction_count;
	uint64_t guest_correction_generation;
	uint8_t guest_correction_flags;
	struct client_rpc guest_correction;
	int global_health_count;
	int base_x;
	int y;
	int z;
	int entity_physics_count;
	struct client_rpc entity_physics;
};
static struct piston_rpc_probe g_rpc_probe;

static void observe_piston_rpc(const struct client_rpc* call, void* user) {
	struct piston_rpc_probe* probe = user;
	w_coord_t x, y, z;
	if(call->type == CRPC_PLAYER_POS) {
		probe->player_pos_count++;
		probe->player_correction_seq
			= call->payload.player_pos.correction_seq;
		probe->player_correction_flags
			= call->payload.player_pos.correction_flags;
		return;
	} else if(call->type == CRPC_LOCAL_GUEST
			  && call->payload.local_guest.result == LOCAL_GUEST_CORRECTION) {
		probe->guest_correction_count++;
		probe->guest_correction_generation
			= call->payload.local_guest.generation;
		probe->guest_correction_flags
			= call->payload.local_guest.correction_flags;
		probe->guest_correction = *call;
		return;
	} else if(call->type == CRPC_PLAYER_SET_HEALTH) {
		probe->global_health_count++;
		return;
	} else if(call->type == CRPC_ENTITY_PHYSICS_CORRECTION) {
		probe->entity_physics_count++;
		probe->entity_physics = *call;
		return;
	} else if(call->type == CRPC_PISTON_MOVING) {
		x = call->payload.piston_moving.x;
		y = call->payload.piston_moving.y;
		z = call->payload.piston_moving.z;
	} else if(call->type == CRPC_SET_BLOCK) {
		x = call->payload.set_block.x;
		y = call->payload.set_block.y;
		z = call->payload.set_block.z;
	} else {
		return;
	}
	int offset = x - probe->base_x;
	if(y != probe->y || z != probe->z || offset < 1 || offset > 13)
		return;
	uint16_t bit = (uint16_t)(1u << (offset - 1));
	if(call->type == CRPC_PISTON_MOVING)
		probe->moving_mask |= bit;
	else
		probe->set_block_mask |= bit;
}

struct guest_health_probe {
	int count;
	short health;
};

static void observe_guest_owned_reply(const struct client_rpc* call,
	void* user) {
	struct guest_health_probe* probe = user;
	if(call->type == CRPC_PLAYER_SET_HEALTH) {
		probe->count++;
		probe->health = call->payload.player_set_health.health;
	}
}

static void assert_entity_physics_model(void) {
	struct entity e;
	struct AABB box;

	memset(&e, 0xA5, sizeof e);
	entity_default_init(&e, true, &s.worlds[0]);
	assert(e.y_size == 0.0F && e.fall_distance == 0.0F);
	assert(e.step_height == 0.0F && !e.collided_horizontally
		   && !e.collided_vertically && !e.collided && !e.in_web
		   && !e.on_ground);

	/* EntityMinecart.<init>: setSize(.98,.7), yOffset=height/2. */
	entity_minecart(700, &e, true, &s.worlds[0]);
	e.teleport(&e, (vec3) {4.0F, 5.0F, 6.0F});
	assert(e.getBoundingBox(&e, &box) == 1);
	assert(box.x1 == 3.51F && box.x2 == 4.49F);
	assert(box.y1 == 4.65F && box.y2 == 5.35F);
	assert(box.z1 == 5.51F && box.z2 == 6.49F);
	struct block_info rail = {.x = 9, .y = 20, .z = 11};
	vec3 placed_cart_pos;
	item_minecart_spawn_position(&rail, placed_cart_pos);
	assert(placed_cart_pos[0] == 9.5F && placed_cart_pos[1] == 20.85F
		   && placed_cart_pos[2] == 11.5F);
	e.teleport(&e, placed_cart_pos);
	assert(e.getBoundingBox(&e, &box) == 1);
	assert(box.y1 == 20.5F && box.y2 == 21.2F);

	/* EntityArrow.<init>: .5 square, yOffset=0 -> bottom anchored. */
	entity_arrow(701, &e, true, &s.worlds[0], 0, ITEM_ARROW, 1, true);
	e.teleport(&e, (vec3) {4.0F, 5.0F, 6.0F});
	assert(e.getBoundingBox(&e, &box) == 1);
	assert(box.x1 == 3.75F && box.x2 == 4.25F);
	assert(box.y1 == 5.0F && box.y2 == 5.5F);
	assert(box.z1 == 5.75F && box.z2 == 6.25F);

	/* EntityLiving inherits Entity's .6 x 1.8 default and stepHeight=.5. */
	entity_monster(702, &e, true, &s.worlds[0], MONSTER_ZOMBIE);
	e.teleport(&e, (vec3) {4.0F, 5.0F, 6.0F});
	assert(e.getBoundingBox(&e, &box) == 1);
	assert(box.x1 == 3.7F && box.x2 == 4.3F);
	assert(box.y1 == 5.0F && box.y2 == 6.8F);
	assert(box.z1 == 5.7F && box.z2 == 6.3F);
	assert(e.step_height == 0.5F);

	/* A typed physics correction is immediate and owns every physics field.
	 * pos_old translates by the authoritative delta instead of snapping, so
	 * interpolation retains its existing phase. Minecart scalar physics cannot
	 * be left behind when the generic vec3 is corrected. */
	entity_minecart(703, &e, false, &s.worlds[0]);
	glm_vec3_copy((vec3) {1.0F, 2.0F, 3.0F}, e.pos);
	glm_vec3_copy((vec3) {0.5F, 1.5F, 2.5F}, e.pos_old);
	glm_vec3_copy((vec3) {-1.0F, -1.0F, -1.0F}, e.network_pos);
	struct client_rpc correction = {
		.type = CRPC_ENTITY_PHYSICS_CORRECTION,
		.payload.entity_physics_correction = {
			.entity_id = e.id,
			.pos = {4.0F, 6.0F, 8.0F},
			.vel = {0.25F, -0.5F, 0.75F},
			.y_size = 0.125F,
			.fall_distance = 2.5F,
			.on_ground = true,
			.collided_horizontally = true,
			.collided_vertically = false,
			.in_web = true,
			.on_fire = true,
			.minecart_speed = -0.2F,
			.minecart_hx = -1,
			.minecart_hz = 0,
			.minecart_rail_meta = 3,
			.minecart_last_meta = 4,
			.fuse = 17,
		},
	};
	clin_apply_entity_physics_correction(&e, &correction);
	assert(e.pos[0] == 4.0F && e.pos[1] == 6.0F && e.pos[2] == 8.0F);
	assert(e.pos_old[0] == 3.5F && e.pos_old[1] == 5.5F
		   && e.pos_old[2] == 7.5F);
	assert(e.network_pos[0] == 4.0F && e.network_pos[1] == 6.0F
		   && e.network_pos[2] == 8.0F);
	assert(e.vel[0] == 0.25F && e.vel[1] == -0.5F && e.vel[2] == 0.75F);
	assert(e.y_size == 0.125F && e.fall_distance == 2.5F && e.on_ground);
	assert(e.collided_horizontally && !e.collided_vertically && e.collided);
	assert(e.in_web && e.on_fire);
	assert(e.data.minecart.speed == -0.2F && e.data.minecart.hx == -1
		   && e.data.minecart.hz == 0 && e.data.minecart.rail_meta == 3
		   && e.data.minecart.last_meta == 4);

	entity_primed_tnt(704, &e, false, &s.worlds[0], 1);
	e.data.primed_tnt.awaiting_destroy = true;
	correction.payload.entity_physics_correction.entity_id = e.id;
	correction.payload.entity_physics_correction.fuse = 17;
	clin_apply_entity_physics_correction(&e, &correction);
	assert(e.data.primed_tnt.fuse == 17);
	assert(!e.data.primed_tnt.awaiting_destroy);
}

// --- RPC drain thread (keeps the client inbox empty so server-thread set_block never blocks) ----
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

static void set(int x, int y, int z, int type, int meta) {
	bool ok = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {
			.type = (uint8_t)type,
			.metadata = (uint8_t)(meta & 0x0F),
		});
	if(!ok) {
		/* Beta setBlockIDWithMetadata returns false for an identical no-op.
		 * Reused floor patches deliberately overlap, so equal is success for
		 * this fixture helper; a mismatched/absent cell remains fatal. */
		struct block_data current;
		ok = server_world_get_block(&s.worlds[0], x, y, z, &current)
			&& current.type == (uint8_t)type
			&& current.metadata == (uint8_t)(meta & 0x0F);
	}
	if(!ok)
		fprintf(stderr, "set failed at (%d,%d,%d), type=%d meta=%d\n",
			x, y, z, type, meta);
	assert(ok);
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static void tick(void) {
	server_world_tick(&s.worlds[0], &s);
	piston_moving_te_tick(&s); // NOT in server_world_tick -- the naive-clone trap
}
// Poke a piston as if a neighbour block updated (Beta onNeighborBlockChange). Used where the power
// source is not a direct neighbour (the BUD y+2 cell).
static void poke(int x, int y, int z) {
	struct block_info info = {.block = NULL, .neighbours = NULL, .x = x, .y = y, .z = z};
	piston_server_neighbour_change(&s, &info);
}
static void floor_patch(int x0, int x1, int z0, int z1) {
	for(int x = x0; x <= x1; x++)
		for(int z = z0; z <= z1; z++)
			set(x, YF, z, BLOCK_STONE, 0);
}

static void assert_box(const struct AABB* box, float x1, float y1, float z1,
					   float x2, float y2, float z2) {
	assert(box->x1 == x1 && box->y1 == y1 && box->z1 == z1);
	assert(box->x2 == x2 && box->y2 == y2 && box->z2 == z2);
}

static float packed_position(const struct displaylist* dl, size_t vertex, int axis) {
	float out;
	assert(axis >= 0 && axis < 3);
	memcpy(&out, (const uint8_t*)dl->data + vertex * PC_VERTEX_BYTES + axis * 4,
		   sizeof out);
	return out;
}

static uint8_t packed_light(const struct displaylist* dl, size_t vertex) {
	const uint8_t* p = (const uint8_t*)dl->data + vertex * PC_VERTEX_BYTES;
	return (uint8_t)(p[12] | (p[13] << 4));
}

static uint8_t packed_texel(const struct displaylist* dl, size_t vertex, int axis) {
	float value;
	assert(axis == 0 || axis == 1);
	memcpy(&value, (const uint8_t*)dl->data + vertex * PC_VERTEX_BYTES + 14 + axis * 4,
		   sizeof value);
	int texel = (int)(value * 256.0F + 0.5F);
	assert(texel >= 0 && texel <= 255);
	return (uint8_t)texel;
}

// Independent expected-value table transcribed from 740c583. Do not generate these by rotating one
// case: BlockPistonExtension.getCollidingBoundingBoxes case 4 contains a real asymmetric quirk.
static const struct AABB BETA_BASE_EXTENDED[6] = {
	{0.0F, 0.25F, 0.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F, 0.75F, 1.0F},
	{0.0F, 0.0F, 0.25F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.75F},
	{0.25F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 0.75F, 1.0F, 1.0F},
};
static const struct AABB BETA_HEAD_PLATE[6] = {
	{0.0F, 0.0F, 0.0F, 1.0F, 0.25F, 1.0F}, {0.0F, 0.75F, 0.0F, 1.0F, 1.0F, 1.0F},
	{0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.25F}, {0.0F, 0.0F, 0.75F, 1.0F, 1.0F, 1.0F},
	{0.0F, 0.0F, 0.0F, 0.25F, 1.0F, 1.0F}, {0.75F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F},
};
static const struct AABB BETA_HEAD_COLLISION_ROD[6] = {
	{0.375F, 0.25F, 0.375F, 0.625F, 1.0F, 0.625F},
	{0.375F, 0.0F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.25F, 0.375F, 0.25F, 0.75F, 0.625F, 1.0F},
	{0.25F, 0.375F, 0.0F, 0.75F, 0.625F, 0.75F},
	{0.375F, 0.25F, 0.25F, 0.625F, 0.75F, 1.0F},
	{0.0F, 0.375F, 0.25F, 0.75F, 0.625F, 0.75F},
};
static const struct AABB BETA_HEAD_RENDER_ROD[6] = {
	{0.375F, 0.25F, 0.375F, 0.625F, 1.25F, 0.625F},
	{0.375F, -0.25F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.375F, 0.375F, 0.25F, 0.625F, 0.625F, 1.25F},
	{0.375F, 0.375F, -0.25F, 0.625F, 0.625F, 0.75F},
	{0.25F, 0.375F, 0.375F, 1.25F, 0.625F, 0.625F},
	{-0.25F, 0.375F, 0.375F, 0.75F, 0.625F, 0.625F},
};
static const struct AABB BETA_HEAD_SHORT_ROD[6] = {
	{0.375F, 0.25F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.375F, 0.25F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.375F, 0.375F, 0.25F, 0.625F, 0.625F, 0.75F},
	{0.375F, 0.375F, 0.25F, 0.625F, 0.625F, 0.75F},
	{0.25F, 0.375F, 0.375F, 0.75F, 0.625F, 0.625F},
	{0.25F, 0.375F, 0.375F, 0.75F, 0.625F, 0.625F},
};

static void assert_box_same(const struct AABB* got, const struct AABB* expected) {
	assert_box(got, expected->x1, expected->y1, expected->z1,
			   expected->x2, expected->y2, expected->z2);
}

static int cave_side_to_beta(enum side side) {
	static const int map[SIDE_MAX] = {
		[SIDE_TOP] = 1, [SIDE_BOTTOM] = 0, [SIDE_LEFT] = 4,
		[SIDE_RIGHT] = 5, [SIDE_FRONT] = 2, [SIDE_BACK] = 3,
	};
	return map[side];
}

static uint8_t expected_rod_light(int facing, enum side side) {
	// RenderBlocks.func_31080_c uses 1.0 top, 0.5 bottom, 0.6 on X sides, and 0.8 on
	// Z sides except for X-axis rods, whose Z sides are also 0.6. Cave's nonlinear light tables
	// map an all-15 input to FF/CC/DD/EE respectively.
	int beta_side = cave_side_to_beta(side);
	if(beta_side == 1)
		return 0xFF;
	if(beta_side == 0)
		return 0xCC;
	if(beta_side == 4 || beta_side == 5 || facing == 4 || facing == 5)
		return 0xDD;
	return 0xEE;
}

static uint8_t expected_rod_light_from(uint8_t light, int facing, enum side side) {
	static const uint8_t bottom[16]
		= {0,0,0,0,1,2,2,3,4,5,6,7,8,9,10,12};
	static const uint8_t side_x[16]
		= {0,0,0,1,2,2,3,4,5,6,7,8,9,10,11,13};
	static const uint8_t side_z[16]
		= {0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14};
	int beta_side = cave_side_to_beta(side);
	const uint8_t* table = NULL;
	if(beta_side == 1)
		return light;
	else if(beta_side == 0)
		table = bottom;
	else if(beta_side == 4 || beta_side == 5 || facing == 4 || facing == 5)
		table = side_x;
	else if(beta_side == 2 || beta_side == 3)
		table = side_z;
	return table ? (uint8_t)((table[light >> 4] << 4) | table[light & 0x0F]) : light;
}

static void assert_head_render_rod(int facing, const struct AABB* expected,
								   bool full_rod) {
	struct block_data data = {
		.type = BLOCK_PISTON_HEAD,
		.metadata = (uint8_t)facing,
		.sky_light = 15,
		.torch_light = 15,
	};
	struct block_info info = {.block = &data, .x = 8, .y = Y, .z = 8};
	uint8_t light[24];
	memset(light, 0xFF, sizeof light);
	float minv[3] = {10000.0F, 10000.0F, 10000.0F};
	float maxv[3] = {-10000.0F, -10000.0F, -10000.0F};
	static const int opposite[6] = {1, 0, 3, 2, 5, 4};
	int rod_faces = 0;

	for(int raw_side = SIDE_TOP; raw_side < SIDE_MAX; raw_side++) {
		enum side side = (enum side)raw_side;
		int beta_side = cave_side_to_beta(side);
		bool lateral = beta_side != facing && beta_side != opposite[facing];
		assert(piston_render_head_variant(NULL, &info, side, light, full_rod, true)
			   == (size_t)(lateral ? 2 : 1));
		if(!lateral)
			continue;

		struct displaylist dl;
		displaylist_init(&dl, 8, 1);
		assert(piston_render_head_variant(
			&dl, &info, side, light, full_rod, false) == 2);
		assert(dl.index == 8 * PC_VERTEX_BYTES);
		int uv_min[2] = {255, 255};
		int uv_max[2] = {0, 0};
		for(size_t vertex = 4; vertex < 8; vertex++) {
			assert(packed_light(&dl, vertex) == expected_rod_light(facing, side));
			for(int axis = 0; axis < 2; axis++) {
				int uv = packed_texel(&dl, vertex, axis);
				if(uv < uv_min[axis]) uv_min[axis] = uv;
				if(uv > uv_max[axis]) uv_max[axis] = uv;
			}
			for(int axis = 0; axis < 3; axis++) {
				float value = packed_position(&dl, vertex, axis);
				if(value < minv[axis]) minv[axis] = value;
				if(value > maxv[axis]) maxv[axis] = value;
			}
		}
		int span_u = uv_max[0] - uv_min[0];
		int span_v = uv_max[1] - uv_min[1];
		assert((span_u > span_v ? span_u : span_v) == (full_rod ? 16 : 8));
		displaylist_destroy(&dl);

		// RenderBlocks.func_31080_c samples ONE centre brightness for every shaft vertex,
		// while the plate remains ordinary per-corner lighting. A gradient catches the
		// tempting but non-Beta reuse of the plate's 24 vertex-light samples for the rod.
		for(size_t i = 0; i < sizeof light; i++)
			light[i] = (uint8_t)((((i * 7) & 15) << 4) | ((i * 5 + 1) & 15));
		data.torch_light = 10;
		data.sky_light = 5;
		displaylist_init(&dl, 8, 1);
		assert(piston_render_head_variant(
			&dl, &info, side, light, full_rod, false) == 2);
		uint8_t rod_light = expected_rod_light_from(0xA5, facing, side);
		bool plate_differs = false;
		for(size_t vertex = 0; vertex < 4; vertex++)
			plate_differs |= packed_light(&dl, vertex) != rod_light;
		assert(plate_differs);
		for(size_t vertex = 4; vertex < 8; vertex++)
			assert(packed_light(&dl, vertex) == rod_light);
		displaylist_destroy(&dl);
		data.torch_light = data.sky_light = 15;
		memset(light, 0xFF, sizeof light);
		rod_faces++;
	}

	assert(rod_faces == 4);
	assert(minv[0] == (float)W2C_COORD(info.x) + expected->x1);
	assert(minv[1] == (float)W2C_COORD(info.y) + expected->y1);
	assert(minv[2] == (float)W2C_COORD(info.z) + expected->z1);
	assert(maxv[0] == (float)W2C_COORD(info.x) + expected->x2);
	assert(maxv[1] == (float)W2C_COORD(info.y) + expected->y2);
	assert(maxv[2] == (float)W2C_COORD(info.z) + expected->z2);
}

static uint64_t piston_uv_hash(struct block* block, struct block_data* data) {
	struct block_info info = {.block = data, .x = 8, .y = Y, .z = 8};
	uint8_t light[24];
	memset(light, 0xFF, sizeof light);
	uint64_t hash = UINT64_C(0xcbf29ce484222325);

	for(int raw_side = SIDE_TOP; raw_side < SIDE_MAX; raw_side++) {
		enum side side = (enum side)raw_side;
		size_t quads = block->renderBlock(NULL, &info, side, NULL, light, true);
		assert(quads == 1 || quads == 2);
		struct displaylist dl;
		displaylist_init(&dl, quads * 4, 1);
		assert(block->renderBlock(&dl, &info, side, NULL, light, false) == quads);
		assert(dl.index == quads * 4 * PC_VERTEX_BYTES);

		uint8_t texture = block->getTextureIndex(&info, side);
		int origin[2] = {
			TEX_OFFSET(TEXTURE_X(texture)), TEX_OFFSET(TEXTURE_Y(texture)),
		};
		for(size_t vertex = 0; vertex < quads * 4; vertex++)
			for(int axis = 0; axis < 2; axis++) {
				int texel = packed_texel(&dl, vertex, axis);
				assert(texel >= origin[axis] && texel <= origin[axis] + 16);
				hash ^= (uint8_t)(texel - origin[axis]);
				hash *= UINT64_C(0x100000001b3);
			}
		displaylist_destroy(&dl);
	}
	return hash;
}

static void assert_beta_piston_uv_all_facings(void) {
	// Literal FNV-1a goldens over relative (tile-local) U,V bytes, in Cave side/vertex order.
	// They were generated independently from the six RenderBlocks face methods plus func_31074_b's
	// field_3108x rotation assignments and func_31076_a/31081_b/31077_c's dedicated 16x4 rod UVs.
	static const uint64_t BASE_FULL[6] = {
		UINT64_C(0x212F2EE558E072A5), UINT64_C(0x10541B1E3DE072A5),
		UINT64_C(0x7AAAF84BB54D7525), UINT64_C(0xB2E30FEDB3E7E525),
		UINT64_C(0x7436A3C6511E1025), UINT64_C(0x0238942A64E11025),
	};
	static const uint64_t BASE_EXTENDED[6] = {
		UINT64_C(0x986288955FF635A5), UINT64_C(0x09C89CA468BC74E5),
		UINT64_C(0x074A6A31425013E5), UINT64_C(0xE10E589F593917A5),
		UINT64_C(0x055805FE14DA4505), UINT64_C(0x1137B72531B70C85),
	};
	static const uint64_t HEAD[6] = {
		UINT64_C(0x17457AB9FED6E9E5), UINT64_C(0x39AB98C1710C0CA5),
		UINT64_C(0xE207F3A0C20A8485), UINT64_C(0x7FAA62C16E883AC5),
		UINT64_C(0x99BE7003030CE885), UINT64_C(0x9483E9442E249905),
	};

	for(int facing = 0; facing < 6; facing++) {
		struct block_data base = {.type = BLOCK_PISTON_BASE, .metadata = (uint8_t)facing};
		assert(piston_uv_hash(&block_piston, &base) == BASE_FULL[facing]);
		base.metadata |= 8;
		assert(piston_uv_hash(&block_piston, &base) == BASE_EXTENDED[facing]);
		struct block_data head = {.type = BLOCK_PISTON_HEAD, .metadata = (uint8_t)facing};
		assert(piston_uv_hash(&block_piston_head, &head) == HEAD[facing]);
	}
}

static void assert_beta_piston_geometry_all_facings(void) {
	const struct AABB full = {0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};
	for(int facing = 0; facing < 6; facing++) {
		struct AABB boxes[2];
		struct block_data base = {
			.type = BLOCK_PISTON_BASE, .metadata = (uint8_t)(facing | 8),
		};
		struct block_info base_info = {.block = &base};
		assert(block_piston.getBoundingBox(&base_info, false, boxes) == 1);
		assert_box_same(&boxes[0], &BETA_BASE_EXTENDED[facing]);
		assert(block_piston.getBoundingBox(&base_info, true, boxes) == 1);
		assert_box_same(&boxes[0], &full);

		struct block_data head = {.type = BLOCK_PISTON_HEAD, .metadata = (uint8_t)facing};
		struct block_info head_info = {.block = &head};
		assert(block_piston_head.getBoundingBox(&head_info, false, boxes) == 1);
		assert_box_same(&boxes[0], &BETA_HEAD_PLATE[facing]);
		assert(block_piston_head.getBoundingBox(&head_info, true, boxes) == 2);
		assert_box_same(&boxes[0], &BETA_HEAD_PLATE[facing]);
		assert_box_same(&boxes[1], &BETA_HEAD_COLLISION_ROD[facing]);
		assert_head_render_rod(facing, &BETA_HEAD_RENDER_ROD[facing], true);
		assert_head_render_rod(facing, &BETA_HEAD_SHORT_ROD[facing], false);
	}
}

struct moving_render_fixture {
	w_coord_t x, y, z;
	int get_calls;
	int moving_calls;
	bool fence_neighbours;
	struct piston_moving_state state;
};

static bool moving_render_get_block(void* opaque, w_coord_t x, w_coord_t y,
									w_coord_t z, struct block_data* out) {
	struct moving_render_fixture* f = opaque;
	assert(f && out);
	f->get_calls++;
	*out = (struct block_data) {
		.type = BLOCK_AIR, .sky_light = 15, .torch_light = 15};
	if(x == f->x && y == f->y && z == f->z)
		out->type = BLOCK_PISTON_MOVING;
	else if(f->fence_neighbours && y == f->y
			&& ((x == f->x + 1 && z == f->z)
				|| (x == f->x && z == f->z + 1)))
		out->type = BLOCK_FENCE;
	return true;
}

static bool moving_render_get_state(void* opaque, w_coord_t x, w_coord_t y,
									w_coord_t z,
									struct piston_moving_state* out) {
	struct moving_render_fixture* f = opaque;
	assert(f && out);
	f->moving_calls++;
	if(x != f->x || y != f->y || z != f->z)
		return false;
	*out = f->state;
	return true;
}

static uint32_t moving_render_get_tint(void* opaque, w_coord_t x,
									   w_coord_t z, enum biome_tint tint) {
	(void)opaque;
	(void)x;
	(void)z;
	(void)tint;
	return 0xFFFFFFu;
}

static void assert_moving_renderer_golden(void) {
	// Every block emitter writes section-local W2C_COORD vertices. Pin the missing model
	// origin for a negative X section and nonzero Y/Z sections, plus fractional piston motion.
	float model_offset[3];
	render_piston_moving_model_offset(
		-17, 37, 34, (float[3]) {-0.25F, 0.5F, 0.75F}, model_offset);
	assert(model_offset[0] == -32.25F);
	assert(model_offset[1] == 32.5F);
	assert(model_offset[2] == 32.75F);

	struct moving_render_fixture fixture = {.x = 8, .y = Y, .z = 8};
	struct piston_moving_render_access access = {
		.get_block = moving_render_get_block,
		.get_moving = moving_render_get_state,
		.get_tint = moving_render_get_tint,
		.ctx = &fixture,
	};
	struct piston_moving_client_entry entry = {
		.x = fixture.x, .y = fixture.y, .z = fixture.z,
		.state = {
			.stored_id = BLOCK_PISTON_BASE,
			.stored_meta = FACE_PX,
			.facing = FACE_PX,
			.flags = PISTON_MOVING_FLAG_SOURCE,
			.current_progress = 0.25F,
			.last_progress = 0.25F,
		},
	};
	fixture.state = entry.state;

	// Retracting source = moving piston head (10 quads) plus stationary shortened base
	// (6), exactly TileEntityRendererPiston's two-render branch: 64 vertices total.
	assert(render_piston_moving_test_emit(
		NULL, &entry, 0.0F, &access, true) == 16);
	struct displaylist dl;
	displaylist_init(&dl, 64, 12);
	assert(render_piston_moving_test_emit(
		&dl, &entry, 0.0F, &access, false) == 16);
	assert(dl.index == 64 * PC_VERTEX_BYTES && !dl.truncated);
	displaylist_destroy(&dl);
	entry.state.current_progress = entry.state.last_progress = 1.0F;
	assert(render_piston_moving_test_emit(
		NULL, &entry, 0.0F, &access, true) == 0);

	// The TileEntityRendererPiston boundary is STRICT progress<0.5: just below uses
	// the half-length/8-texel head shaft; exactly 0.5 uses the ordinary full head.
	entry.state = (struct piston_moving_state) {
		.stored_id = BLOCK_PISTON_HEAD,
		.stored_meta = FACE_PX,
		.facing = FACE_PX,
		.flags = PISTON_MOVING_FLAG_EXTENDING,
		.current_progress = 0.499F,
		.last_progress = 0.499F,
	};
	fixture.state = entry.state;
	uint8_t short_bytes[40 * PC_VERTEX_BYTES];
	displaylist_init(&dl, 40, 12);
	assert(render_piston_moving_test_emit(
		&dl, &entry, 0.0F, &access, false) == 10);
	assert(dl.index == sizeof short_bytes);
	memcpy(short_bytes, dl.data, sizeof short_bytes);
	displaylist_destroy(&dl);
	entry.state.current_progress = entry.state.last_progress = 0.5F;
	fixture.state = entry.state;
	displaylist_init(&dl, 40, 12);
	assert(render_piston_moving_test_emit(
		&dl, &entry, 0.0F, &access, false) == 10);
	assert(dl.index == sizeof short_bytes);
	assert(memcmp(short_bytes, dl.data, sizeof short_bytes) != 0);
	displaylist_destroy(&dl);

	// Generic stored blocks run BOTH render callbacks with real adjacent cells. A fence
	// with +X/+Z neighbours is 6 post quads + 16 connection quads, not the 6-quads-only
	// result produced when renderBlockAlways or neighbour access is accidentally omitted.
	fixture.fence_neighbours = true;
	fixture.get_calls = 0;
	entry.state = (struct piston_moving_state) {
		.stored_id = BLOCK_FENCE,
		.stored_meta = 0,
		.facing = FACE_PX,
		.flags = PISTON_MOVING_FLAG_EXTENDING,
		.current_progress = 0.25F,
		.last_progress = 0.25F,
	};
	fixture.state = entry.state;
	assert(render_piston_moving_test_emit(
		NULL, &entry, 0.0F, &access, true) == 22);
	assert(fixture.get_calls > 6); // includes six neighbours plus the real light sampler
}

int main(void) {
	test_fixture_dir("test_piston_move_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	items_init();

	// Beta Block's constructor records lightOpacity=0 when isOpaqueCube()==false; all four piston
	// classes override isOpaqueCube false. World.isBlockNormalCube also requires
	// renderAsNormalBlock()==true, which BlockPistonBase/Extension/Moving override false. Thus pistons
	// neither black out skylight nor act as generic solid-cube redstone conductors. [740c583:
	// Block.<init>, BlockPistonBase, BlockPistonExtension, BlockPistonMoving; mc173 material.rs
	// is_cube/is_opaque_cube/get_light_opacity independently agrees for piston/head/moving.]
	assert(block_piston.opacity == 0 && block_piston.can_see_through);
	assert(block_piston_sticky.opacity == 0 && block_piston_sticky.can_see_through);
	assert(block_piston_head.opacity == 0 && block_piston_head.can_see_through);
	assert(block_piston_moving.opacity == 0 && block_piston_moving.can_see_through);
	assert(!block_is_normal_cube(BLOCK_PISTON_BASE));
	assert(!block_is_normal_cube(BLOCK_PISTON_STICKY));
	assert(!block_is_normal_cube(BLOCK_PISTON_HEAD));
	assert(!block_is_normal_cube(BLOCK_PISTON_MOVING));
	assert(block_piston.digging.hardness == 750);
	assert(block_piston_sticky.digging.hardness == 750);
	assert(block_piston_head.digging.hardness == 750);
	assert(block_piston_moving.digging.hardness == -1);
	assert_beta_piston_geometry_all_facings();
	assert_beta_piston_uv_all_facings();
	assert_moving_renderer_golden();

	// --- piston ITEM-ICON registration guard (this is the only piston test that links the full block
	// registry, so it pins the data value the pure texture golden cannot reach). Beta 1.7.3
	// RenderBlocks.renderBlockOnInventory FORCES metadata=1 for render type 16, so the piston item icon
	// faces UP (+Y): the front plate is the visible top face and the sticky vs normal stacks differ.
	// CaveWii reproduces that via the block_item default_metadata. A regression that drops these fields
	// reverts the icon to metadata 0 (plate hidden on the bottom -> identical, "headless" stacks). -----
	assert(block_piston.block_item.render_data.block.has_default);
	assert(block_piston.block_item.render_data.block.default_metadata == 1);
	assert(block_piston.block_item.render_data.block.default_rotation == 0);
	assert(block_piston_sticky.block_item.render_data.block.has_default);
	assert(block_piston_sticky.block_item.render_data.block.default_metadata == 1);
	assert(block_piston_sticky.block_item.render_data.block.default_rotation == 0);

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	piston_moving_store_init(&s.piston_moving);
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0; // unlimited: scheduled ticks fire exactly when due
	assert_entity_physics_model();

	// Boundary preflight golden: prepare the fixture in two resident chunks, evict the target, then
	// trigger from the base chunk. Beta World.getBlockId provides the target chunk; treating missing as
	// air would latch the base and partially lose the move. Production must reload it BEFORE mutation.
	for(int cx = 0; cx <= 1; cx++) {
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&s.worlds[0], cx, 0, &sc) && sc);
	}
	{
		const int PX = 15, Z = 4;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0); // target chunk (1,0)
		server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
		output_quiesce();
		assert(server_world_save_chunk(&s.worlds[0], true, 1, 0));
		struct block_data absent;
		assert(!server_world_get_block(&s.worlds[0], PX + 1, Y, Z, &absent));
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		assert(getmeta(PX, Y, Z) == (FACE_PX | 8));
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING);
		for(int k = 0; k < 3; k++)
			tick();
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_HEAD);
		assert(get(PX + 2, Y, Z) == BLOCK_STONE);
	}

	/* An idle boundary piston needs only its power-probe footprint. The old
	 * preflight loaded the full 14-step notification ring even with no edge. */
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	output_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true, 1, 0));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 1, 0));
	set(14, Y, 12, BLOCK_PISTON_BASE, FACE_PX);
	poke(14, Y, 12);
	assert(getmeta(14, Y, 12) == FACE_PX);
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 1, 0));

	/* Allocation is part of the move transaction: a failed reserve occurs
	 * before the powered bit, drops, air clears, or any id-36 write. Retry after
	 * the one-shot failure proves the unchanged fixture remains actionable. */
	piston_moving_store_destroy(&s.piston_moving);
	piston_moving_store_init(&s.piston_moving);
	{
		const int PX = 4, Z = 4;
		floor_patch(PX - 1, PX + 1, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, 1); /* +Y */
		for(int dy = 1; dy <= 12; dy++)
			set(PX, Y + dy, Z, BLOCK_STONE, 0);
		piston_moving_store_test_fail_allocation_after(0);
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		assert(getmeta(PX, Y, Z) == 1);
		assert(s.piston_moving.count == 0 && s.piston_moving.reserved == 0);
		for(int dy = 1; dy <= 12; dy++)
			assert(get(PX, Y + dy, Z) == BLOCK_STONE);
		assert(get(PX, Y + 13, Z) == BLOCK_AIR);
		poke(PX, Y, Z); /* one-shot failure is gone */
		assert(getmeta(PX, Y, Z) == (1 | 8));
		assert(s.piston_moving.count == 13 && s.piston_moving.reserved == 0);
		for(int k = 0; k < 3; k++)
			tick();
		assert(s.piston_moving.count == 0 && s.piston_moving.reserved == 0);
	}

	// Load every chunk the remaining scenarios touch (x through 2, z through 4).
	for(int cx = 0; cx <= 2; cx++)
		for(int cz = 0; cz <= 4; cz++) {
			if(server_world_is_chunk_loaded(&s.worlds[0], cx, cz))
				continue;
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}

	// =============================================================================================
	// CAPACITY GOLDEN (RED before the dynamic-store rewrite): five legal maximum extensions create
	// 5 * (12 shifted blocks + 1 head) == 65 simultaneous TileEntityPistons. Beta has no global
	// moving-TE cap: each Chunk installs every TileEntity into its map/list, World.updateEntities
	// visits the entry snapshot in loaded-list order, and TileEntityPiston advances 0 -> .5 -> 1
	// before settling on the following visit. All five vertical columns deliberately share one chunk,
	// so the same golden also rejects a 64-record chunk-sidecar transport. [740c583: BlockPistonBase
	// tryExtend; World.setBlockTileEntity/updateEntities; TileEntityPiston.updateEntity]
	// =============================================================================================
	{
		const int Z = 47;
		const int columns[5] = {1, 4, 7, 10, 13};
		floor_patch(0, 14, Z - 1, Z);
		for(size_t c = 0; c < sizeof columns / sizeof columns[0]; c++) {
			int x = columns[c];
			set(x, Y, Z, BLOCK_PISTON_BASE, 1); // +Y
			for(int dy = 1; dy <= 12; dy++)
				set(x, Y + dy, Z, BLOCK_STONE, 0);
			set(x, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		}

		for(size_t c = 0; c < sizeof columns / sizeof columns[0]; c++) {
			int x = columns[c];
			assert(getmeta(x, Y, Z) == (1 | 8));
			for(int dy = 1; dy <= 13; dy++) {
				assert(get(x, Y + dy, Z) == BLOCK_PISTON_MOVING);
				struct piston_moving_state moving;
				assert(piston_moving_state_at(&s, x, Y + dy, Z, &moving));
				assert(moving.current_progress == 0.0F
					   && moving.last_progress == 0.0F);
			}
		}

		tick();
		for(size_t c = 0; c < sizeof columns / sizeof columns[0]; c++)
			for(int dy = 1; dy <= 13; dy++) {
				struct piston_moving_state moving;
				assert(piston_moving_state_at(
					&s, columns[c], Y + dy, Z, &moving));
				assert(moving.current_progress == 0.5F
					   && moving.last_progress == 0.0F);
			}
		tick();
		for(size_t c = 0; c < sizeof columns / sizeof columns[0]; c++)
			for(int dy = 1; dy <= 13; dy++) {
				struct piston_moving_state moving;
				assert(piston_moving_state_at(
					&s, columns[c], Y + dy, Z, &moving));
				assert(moving.current_progress == 1.0F
					   && moving.last_progress == 0.5F);
			}
		tick();
		for(size_t c = 0; c < sizeof columns / sizeof columns[0]; c++) {
			int x = columns[c];
			assert(get(x, Y + 1, Z) == BLOCK_PISTON_HEAD);
			for(int dy = 2; dy <= 13; dy++)
				assert(get(x, Y + dy, Z) == BLOCK_STONE);
		}
	}

	// =============================================================================================
	// SCENARIO A: EXTEND pushes a 2-block column; the moving block occupies id-36 through the settle
	// window, then materializes (head + the two shifted stones) -- pinning PISTON_SETTLE_TICKS.
	// =============================================================================================
	{
		const int PX = 8, Z = 8;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX); // unpowered
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		set(PX + 2, Y, Z, BLOCK_STONE, 0); // air at PX+3

		// stable while unpowered -- no spurious extend
		for(int k = 0; k < 4; k++) {
			tick();
			assert(get(PX, Y, Z) == BLOCK_PISTON_BASE && getmeta(PX, Y, Z) == FACE_PX);
		}

		// power it: a standing lit torch on the -Z face (a direct neighbour) -> EXTEND fires NOW
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		// the powered bit is latched and the column is now three id-36 moving cells
		assert(getmeta(PX, Y, Z) == (FACE_PX | 8));
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 3, Y, Z) == BLOCK_PISTON_MOVING);

		// the moving blocks persist for PISTON_SETTLE_TICKS visits, THEN materialize on the 3rd
		tick(); // visit 1 (ticks 2->1)
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		tick(); // visit 2 (1->0)
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		tick(); // visit 3 (==0 -> settle)
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_HEAD); // head materialized in front of the piston
		assert(get(PX + 2, Y, Z) == BLOCK_STONE);		// the two stones shifted +1
		assert(get(PX + 3, Y, Z) == BLOCK_STONE);
		assert(getmeta(PX, Y, Z) == (FACE_PX | 8)); // base stays powered/extended

		// The settled geometry is part of the behavior witnessed on hardware: the old placeholder
		// rendered/collided as a SECOND full piston cube. Beta 1.7.3 740c583 pins these exact +X
		// shapes. BlockPistonBase.setBlockBoundsBasedOnState case 5 makes the extended base
		// x=[0,0.75] (selection/render; getCollidingBoundingBoxes deliberately remains full-cube).
		// BlockPistonExtension.setBlockBoundsBasedOnState case 5 selects only the x=[0.75,1]
		// plate; getCollidingBoundingBoxes case 5 appends the rod as the second collision box.
		struct block_data base_data
			= {.type = BLOCK_PISTON_BASE, .metadata = (uint8_t)(FACE_PX | 8)};
		struct block_info base_info = {.block = &base_data, .x = PX, .y = Y, .z = Z};
		struct AABB boxes[2];
		assert(block_piston.getBoundingBox(&base_info, false, boxes) == 1);
		assert_box(&boxes[0], 0.0F, 0.0F, 0.0F, 0.75F, 1.0F, 1.0F);
		assert(block_piston.getBoundingBox(&base_info, true, boxes) == 1);
		assert_box(&boxes[0], 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);

		struct block_data head_data = {.type = BLOCK_PISTON_HEAD, .metadata = FACE_PX};
		struct block_info head_info = {.block = &head_data, .x = PX + 1, .y = Y, .z = Z};
		assert(block_piston_head.getBoundingBox(&head_info, false, boxes) == 1);
		assert_box(&boxes[0], 0.75F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);
		assert(block_piston_head.getBoundingBox(&head_info, true, boxes) == 2);
		assert_box(&boxes[0], 0.75F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);
		assert_box(&boxes[1], 0.0F, 0.375F, 0.25F, 0.75F, 0.625F, 0.75F);

		// RenderBlocks.func_31080_c case 5 renders the plate as a six-face cuboid, then exactly
		// four rod side quads (no end caps). Thus the two axis faces emit one quad each and the
		// four perpendicular faces emit plate+rod. This count catches the visible full-cube head.
		uint8_t light[24] = {0};
		assert(block_piston_head.renderBlock(
			NULL, &head_info, SIDE_LEFT, NULL, light, true) == 1);
		assert(block_piston_head.renderBlock(
			NULL, &head_info, SIDE_RIGHT, NULL, light, true) == 1);
		assert(block_piston_head.renderBlock(
			NULL, &head_info, SIDE_TOP, NULL, light, true) == 2);
		assert(block_piston_head.renderBlock(
			NULL, &head_info, SIDE_BOTTOM, NULL, light, true) == 2);
		assert(block_piston_head.renderBlock(
			NULL, &head_info, SIDE_FRONT, NULL, light, true) == 2);
		assert(block_piston_head.renderBlock(
			NULL, &head_info, SIDE_BACK, NULL, light, true) == 2);

		// Exercise the EMIT pass too. The top face of a +X extended base ends at x=PX+0.75;
		// the head emits first the plate top at y=Y+1, then its rod top at y=Y+0.625.
		// RenderBlocks.func_31074_b case 5 + func_31080_c case 5 (renderFullRod=true).
		struct displaylist dl;
		displaylist_init(&dl, 4, 1);
		assert(block_piston.renderBlock(
			&dl, &base_info, SIDE_TOP, NULL, light, false) == 1);
		assert(dl.index == 4 * PC_VERTEX_BYTES);
		assert(packed_position(&dl, 0, 0) == (float)PX);
		assert(packed_position(&dl, 1, 0) == (float)PX + 0.75F);
		assert(packed_position(&dl, 2, 0) == (float)PX + 0.75F);
		assert(packed_position(&dl, 3, 0) == (float)PX);
		for(size_t i = 0; i < 4; i++)
			assert(packed_position(&dl, i, 1) == (float)W2C_COORD(Y) + 1.0F);
		displaylist_destroy(&dl);

		displaylist_init(&dl, 8, 1);
		assert(block_piston_head.renderBlock(
			&dl, &head_info, SIDE_TOP, NULL, light, false) == 2);
		assert(dl.index == 8 * PC_VERTEX_BYTES);
		for(size_t i = 0; i < 4; i++)
			assert(packed_position(&dl, i, 1) == (float)W2C_COORD(Y) + 1.0F); // plate top
		assert(packed_position(&dl, 4, 0) == (float)(PX + 1) - 0.25F);
		assert(packed_position(&dl, 5, 0) == (float)(PX + 1) + 0.75F);
		assert(packed_position(&dl, 6, 0) == (float)(PX + 1) + 0.75F);
		assert(packed_position(&dl, 7, 0) == (float)(PX + 1) - 0.25F);
		for(size_t i = 4; i < 8; i++)
			assert(packed_position(&dl, i, 1) == (float)W2C_COORD(Y) + 0.625F); // rod top
		assert(packed_position(&dl, 4, 2) == (float)Z + 0.375F);
		assert(packed_position(&dl, 6, 2) == (float)Z + 0.625F);
		displaylist_destroy(&dl);

		// settled state is stable
		for(int k = 0; k < 4; k++) {
			tick();
			assert(get(PX + 1, Y, Z) == BLOCK_PISTON_HEAD && get(PX + 2, Y, Z) == BLOCK_STONE);
		}

		// ----- RETRACT (non-sticky): cut the source -> base retracts, head cell clears -----
		set(PX, Y, Z - 1, BLOCK_AIR, 0); // breaking the -Z neighbour notifies the piston -> RETRACT
		assert(get(PX, Y, Z) == BLOCK_PISTON_MOVING); // the base cell is now a retracting moving block
		assert(get(PX + 1, Y, Z) == BLOCK_AIR);		  // the head cell cleared immediately
		for(int k = 0; k < 3; k++)
			tick(); // 3 visits -> the base settles back to the piston
		assert(get(PX, Y, Z) == BLOCK_PISTON_BASE);
		assert(getmeta(PX, Y, Z) == FACE_PX); // powered bit cleared
		assert(get(PX + 1, Y, Z) == BLOCK_AIR);
		assert(get(PX + 2, Y, Z) == BLOCK_STONE); // the pushed stones stay pushed (Beta: no un-push)
		assert(get(PX + 3, Y, Z) == BLOCK_STONE);
	}

	// =============================================================================================
	// SCENARIO B: the 12-block LIMIT -- a 13-movable column REFUSES (nothing moves, no powered bit).
	// =============================================================================================
	{
		const int PX = 8, Z = 12;
		floor_patch(PX - 1, PX + 14, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		for(int i = 1; i <= 13; i++)
			set(PX + i, Y, Z, BLOCK_STONE, 0); // 13 movable in a row -> over the limit
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5); // power it
		// refused: the piston stays unextended, the column is untouched, no id-36 anywhere
		assert(get(PX, Y, Z) == BLOCK_PISTON_BASE && getmeta(PX, Y, Z) == FACE_PX);
		for(int i = 1; i <= 13; i++)
			assert(get(PX + i, Y, Z) == BLOCK_STONE);

		// remove the 13th (now exactly 12) and re-poke -> the full 12-column extends
		set(PX + 13, Y, Z, BLOCK_AIR, 0);
		g_rpc_probe = (struct piston_rpc_probe) {
			.base_x = PX, .y = Y, .z = Z};
		clin_test_set_rpc_observer(observe_piston_rpc, &g_rpc_probe);
		poke(PX, Y, Z);
		clin_test_set_rpc_observer(NULL, NULL);
		assert(getmeta(PX, Y, Z) == (FACE_PX | 8)); // now legal -> extended
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 12, Y, Z) == BLOCK_PISTON_MOVING); // the 12th cell now holds the shifted stone
		/* A legal maximum push creates 12 moved blocks plus the head. Each
		 * destination is represented by ONE atomic RPC, never an earlier
		 * CRPC_SET_BLOCK followed by a sidecar message (16-slot inbox bound). */
		assert(g_rpc_probe.moving_mask == UINT16_C(0x1FFF));
		assert((g_rpc_probe.moving_mask & g_rpc_probe.set_block_mask) == 0);
	}

	// =============================================================================================
	// SCENARIO C: REFUSALS via the real accessors -- a tile-entity (chest) and an unbreakable (bedrock)
	// block in the push path both block the extend.
	// =============================================================================================
	{
		const int PX = 8, Z = 16;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_CHEST, 0); // ps_has_tile_entity -> not pushable
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		assert(getmeta(PX, Y, Z) == FACE_PX); // refused (no powered bit)
		assert(get(PX + 1, Y, Z) == BLOCK_CHEST); // the chest did not move

		const int Z2 = 19;
		floor_patch(PX - 1, PX + 3, Z2 - 1, Z2 + 1);
		set(PX, Y, Z2, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z2, BLOCK_BEDROCK, 0); // ps_get_hardness == -1 -> not pushable
		set(PX, Y, Z2 - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		assert(getmeta(PX, Y, Z2) == FACE_PX); // refused
		assert(get(PX + 1, Y, Z2) == BLOCK_BEDROCK);

		const int Z3 = 22;
		floor_patch(PX - 1, PX + 3, Z3 - 1, Z3 + 1);
		set(PX, Y, Z3, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z3, BLOCK_JUKEBOX, 0); // a Beta BlockContainer -> ps_has_tile_entity -> not pushable
		set(PX, Y, Z3 - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		assert(getmeta(PX, Y, Z3) == FACE_PX); // refused
		assert(get(PX + 1, Y, Z3) == BLOCK_JUKEBOX);
	}

	// =============================================================================================
	// SCENARIO D: the BUD "block above powers the piston" quirk -- power at (x,y+2,z) with NO direct-
	// face source extends the piston (the trailing quasi-connectivity probes through the real model).
	// =============================================================================================
	{
		const int PX = 8, Z = 24;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0); // one pushable block, air beyond
		// A piston base is non-normal in Beta, so a standing source cannot balance on it. Instead,
		// put the ON floor lever on top of the normal y+2 cube. The lever weakly powers its mount,
		// while remaining outside every direct-neighbour probe around the piston itself.
		set(PX, Y + 2, Z, BLOCK_STONE, 0); // the BUD "block above"
		set(PX, Y + 3, Z, BLOCK_LEVER, 5 | 8);
		assert(redstone_server_indirect_power(&s, PX, Y + 2, Z, 1, true));
		poke(PX, Y, Z);					   // a block update reaches the piston -> it re-reads its power
		assert(getmeta(PX, Y, Z) == (FACE_PX | 8)); // extended via the BUD quirk
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING); // head moving into the front cell
	}

	// =============================================================================================
	// SCENARIO E: STICKY piston PULLS a block back on retract.
	// =============================================================================================
	{
		const int PX = 8, Z = 28;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_STICKY, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0); // air beyond -> a clean 1-block push
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5); // EXTEND
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING); // head moving out
		assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING); // the stone moving out
		for(int k = 0; k < 3; k++)
			tick(); // settle: head at PX+1, the stone at PX+2
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_HEAD);
		assert(get(PX + 2, Y, Z) == BLOCK_STONE);

		set(PX, Y, Z - 1, BLOCK_AIR, 0); // RETRACT -> the sticky piston pulls the stone back
		for(int k = 0; k < 3; k++)
			tick();
		assert(get(PX, Y, Z) == BLOCK_PISTON_STICKY); // base settled back
		assert(getmeta(PX, Y, Z) == FACE_PX);
		assert(get(PX + 1, Y, Z) == BLOCK_STONE); // the stone was PULLED into the head cell
		assert(get(PX + 2, Y, Z) == BLOCK_AIR);	  // its old cell is now empty
	}

	// =============================================================================================
	// SCENARIO F: MOVING-TILE behavior, transcribed from 740c583 TileEntityPiston.updateEntity,
	// BlockPistonMoving.getCollisionBoundingBoxFromPool/dropBlockAsItemWithChance, and
	// BlockPistonBase.tryExtend. This is the golden that the former id36 full-cube/renderless/countdown
	// approximation must fail before implementation:
	//   * the transient cell metadata is the STORED block metadata, not the piston facing;
	//   * stored drops delegate with that metadata (colored wool stays colored);
	//   * update visits push an intersecting player by 0.5 + 0.0625, 0.5 + 0.0625, then 0.25;
	//   * an orphan id36 (no TileEntityPiston) has no collision box.
	// [decompile 740c583: BlockPistonBase.tryExtend, BlockPistonMoving,
	//  TileEntityPiston.func_31010_a/updateEntity]
	// =============================================================================================
	{
		const int PX = 8, Z = 34;
		const int WOOL_META = 13;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_WOOL, WOOL_META);

		// The first extending push AABB for the wool is destination [PX+2,PX+3]
		// shifted -0.5 on X. This player box intersects it, but after that push no
		// longer intersects the head TE behind it, so every expected delta below is
		// attributable to exactly one TileEntityPiston.
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.x = (double)PX + 1.6;
		s.player.y = (double)Y + 1.62; // player position is eye height
		s.player.z = (double)Z + 0.5;
		s.player.on_ground = true;
		s.player.sneaking = false;
		server_local_refresh_player_membership(&s);

		/* A reliable state edge owns the persistent pre-call values even when
		 * the periodic position sample was dropped. A stale correction epoch
		 * cannot resurrect old-body sneak. */
		struct server_rpc movement_state = {
			.type = SRPC_PLAYER_STATE,
			.payload.player_state = {
				.on_ground = true,
				.sneaking = false,
				.correction_seq = s.player.correction_seq,
			},
		};
		server_local_test_process_rpc(&s, &movement_state);
		assert(s.player.on_ground && !s.player.sneaking);
		movement_state.payload.player_state.on_ground = false;
		movement_state.payload.player_state.sneaking = true;
		movement_state.payload.player_state.correction_seq++;
		server_local_test_process_rpc(&s, &movement_state);
		assert(s.player.on_ground && !s.player.sneaking);

		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING);
		// Beta writes setBlockAndMetadata(... pistonMoving, storedMetadata).
		assert(getmeta(PX + 2, Y, Z) == WOOL_META);

		struct item_data stored = {0};
		assert(piston_moving_stored(&s, PX + 2, Y, Z, &stored));
		assert(stored.id == BLOCK_WOOL && stored.durability == WOOL_META);
		struct block_data moving_data = {
			.type = BLOCK_PISTON_MOVING, .metadata = (uint8_t)WOOL_META,
		};
		struct block_info moving_info = {
			.block = &moving_data, .x = PX + 2, .y = Y, .z = Z,
		};
		struct item_data drop = {0};
		assert(block_piston_moving.getDroppedItem(
			&moving_info, &drop, NULL, &s) == 1);
		assert(drop.id == BLOCK_WOOL && drop.durability == WOOL_META
			   && drop.count == 1);

		struct piston_rpc_probe push_probe = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &push_probe);
		tick(); // last=0,current=.5; extending displacement=.5+.0625
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(s.player.x - ((double)PX + 2.1625)) < 1.0e-9);
		assert(push_probe.player_pos_count == 1);
		assert(push_probe.player_correction_seq == s.player.correction_seq
			   && s.player.correction_seq != 0);
		assert(push_probe.player_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY
				   | PLAYER_CORRECTION_SET_ON_GROUND));
		/* R-S1a2 F2: the push correction must ALSO raise the per-actor input
		 * latch, which is what the SRPC_PLAYER_POS / SRPC_PLAYER_STATE handlers
		 * gate on since S1-a2 split the one flag in two. The piston writes the
		 * server-wide flag and calls server_local_raise_actor_input_barriers
		 * beside it; before that call the barrier survived only on the
		 * correction_seq bump, an undocumented coupling between two files. */
		assert(s.reject_player_pos_this_update
			   && s.player.reject_pos_this_update);

		// BlockPistonMoving.blockActivated returns FALSE for a live TE. The scoped bool
		// dispatch must therefore fall through to held-item placement instead of consuming
		// every click merely because id36 has a callback.
		struct inventory saved_inventory = s.player.inventory;
		struct inventory* saved_active = s.player.active_inventory;
		struct item_data click_items[INVENTORY_SIZE] = {0};
		click_items[INVENTORY_SLOT_HOTBAR]
			= (struct item_data) {.id = BLOCK_COBBLESTONE, .count = 2};
		s.player.inventory = (struct inventory) {
			.items = click_items, .capacity = INVENTORY_SIZE, .hotbar_slot = 0};
		s.player.active_inventory = &s.player.inventory;
		struct server_rpc place_through_live = {
			.type = SRPC_BLOCK_PLACE,
			.payload.block_place = {
				.x = PX + 2, .y = Y, .z = Z, .side = SIDE_BACK,
			},
		};
		server_local_test_process_rpc(&s, &place_through_live);
		assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 2, Y, Z + 1) == BLOCK_COBBLESTONE);
		assert(click_items[INVENTORY_SLOT_HOTBAR].count == 1);
		s.player.inventory = saved_inventory;
		s.player.active_inventory = saved_active;

		// A position sample authored before the correction may arrive on the next server tick. The
		// monotonic echo rejects it even after the one-update guard has cleared; the exact echo passes.
		double pushed_x = s.player.x;
		s.reject_player_pos_this_update = false;
		/* ...and the PER-ACTOR input latch beside it (S1-a2 split the one flag
		 * in two; a piston push correction raises both, R-S1a2 F2).
		 * server_local_update retires them together at the update boundary, and
		 * this fixture owns a fresh update. */
		s.player.reject_pos_this_update = false;
		struct server_rpc stale = {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos = {
				.x = (double)PX + 1.6, .y = s.player.y, .z = s.player.z,
				.rx = 99.0F, .ry = 0.0F, .vel_y = 0.0F,
				.correction_seq = s.player.correction_seq - 1,
			},
		};
		server_local_test_process_rpc(&s, &stale);
		assert(s.player.x == pushed_x && s.player.rx != 99.0F);
		stale.payload.player_pos.x = pushed_x;
		stale.payload.player_pos.rx = 37.0F;
		stale.payload.player_pos.on_ground = true;
		stale.payload.player_pos.sneaking = true;
		stale.payload.player_pos.correction_seq = s.player.correction_seq;
		server_local_test_process_rpc(&s, &stale);
		assert(s.player.x == pushed_x && s.player.rx == 37.0F
			   && s.player.on_ground && s.player.sneaking);
		movement_state.payload.player_state.on_ground = false;
		movement_state.payload.player_state.sneaking = false;
		movement_state.payload.player_state.correction_seq
			= s.player.correction_seq;
		server_local_test_process_rpc(&s, &movement_state);
		assert(!s.player.on_ground && !s.player.sneaking);

		// Position-only application translates both interpolation samples and preserves view/velocity.
		// The SET_ON_GROUND bit owns the result separately: Beta moveEntity assigns false even after an
		// ordinary horizontal piston call; vertical clipping may set it true.
		struct entity client_player;
		entity_default_init(&client_player, false, NULL);
		client_player.pos[0] = client_player.pos_old[0] = 1.0F;
		client_player.pos[1] = client_player.pos_old[1] = 2.0F;
		client_player.pos[2] = client_player.pos_old[2] = 3.0F;
		client_player.vel[0] = 0.25F;
		client_player.vel[1] = -0.1F;
		client_player.vel[2] = 0.5F;
		client_player.on_ground = true;
		struct camera camera = {.rx = 0.75F, .ry = 1.25F};
		clin_apply_position_only_correction(
			&client_player, &camera, (vec3) {1.5625F, 2.0F, 3.0F},
			PLAYER_CORRECTION_POSITION_ONLY);
		assert(client_player.pos[0] == 1.5625F
			   && client_player.pos_old[0] == 1.5625F);
		assert(client_player.vel[0] == 0.25F && client_player.vel[1] == -0.1F
			   && client_player.vel[2] == 0.5F && client_player.on_ground);
		assert(camera.rx == 0.75F && camera.ry == 1.25F);
		clin_apply_position_only_correction(
			&client_player, &camera, (vec3) {1.5625F, 2.0F, 3.0F},
			PLAYER_CORRECTION_POSITION_ONLY | PLAYER_CORRECTION_SET_ON_GROUND);
		assert(!client_player.on_ground);
		assert(client_player.vel[0] == 0.25F && client_player.vel[1] == -0.1F
			   && client_player.vel[2] == 0.5F);
		clin_apply_position_only_correction(
			&client_player, &camera, (vec3) {1.5625F, 2.0F, 3.0F},
			PLAYER_CORRECTION_POSITION_ONLY | PLAYER_CORRECTION_ZERO_VEL_Y
				| PLAYER_CORRECTION_SET_ON_GROUND | PLAYER_CORRECTION_ON_GROUND);
		assert(client_player.vel[1] == 0.0F && client_player.on_ground);
		assert(camera.rx == 0.75F && camera.ry == 1.25F);
		// Review 19 M-1 / M-2, at the REAL call site: this arm assigns camera.y
		// absolutely, so the sneak-dip fold-out RECORD of the last attach must go
		// (or clin_update would put y + applied on the wire) -- but it is the shared
		// correction channel (this very piston push), not Beta's teleport packet, and
		// Beta pushes the player through moveEntity, which never touches ySize. So the
		// dip STATE survives: a latched sneak does not pop the camera per correction.
		camera.ysize.y_size = camera.ysize.y_size_prev = 0.2F * 0.4F;
		camera.ysize.applied = 0.2F * 0.4F;
		camera.ysize.pre_dip_y = 9.0F;
		camera.ysize.post_dip_y = 9.0F - camera.ysize.applied;
		clin_apply_position_only_correction(
			&client_player, &camera, (vec3) {1.5625F, 2.0F, 3.0F},
			PLAYER_CORRECTION_POSITION_ONLY);
		assert(camera.ysize.applied == 0.0F && camera.ysize.pre_dip_y == 0.0F
			   && camera.ysize.post_dip_y == 0.0F
			   && "the position-only correction kept a stale sneak-dip record");
		assert(camera.ysize.y_size == 0.2F * 0.4F
			   && camera.ysize.y_size_prev == 0.2F * 0.4F
			   && "the position-only correction zeroed the sneak dip (camera pop)");
		assert(player_ysize_unapply_y(camera.y, &camera.ysize) == 2.0F
			   && "the wire no longer carries exactly the corrected Y");
		camera.ysize = (struct player_ysize_state) {0};
		tick(); // last=.5,current=1; extending displacement=.5+.0625
		assert(fabs(s.player.x - ((double)PX + 2.725)) < 1.0e-9);
		tick(); // last=1; final displacement=.25, then materialize
		assert(fabs(s.player.x - ((double)PX + 2.975)) < 1.0e-9);
		assert(get(PX + 2, Y, Z) == BLOCK_WOOL
			   && getmeta(PX + 2, Y, Z) == WOOL_META);

		// BlockPistonMoving returns null when its TileEntityPiston is missing. The
		// callback-free block_info below deliberately models an orphan.
		struct AABB orphan_box;
		assert(block_piston_moving.getBoundingBox(
			&moving_info, true, &orphan_box) == 0);
		assert(block_piston_moving.getBoundingBox(
			&moving_info, false, &orphan_box) == 0);

		// The same Beta callback returns TRUE for an orphan and clears it. This pins both
		// halves of the bool dispatch contract (live=false/fall-through, orphan=true/handled).
		set(PX + 3, Y + 1, Z, BLOCK_PISTON_MOVING, 0);
		struct block_data orphan_data = {.type = BLOCK_PISTON_MOVING};
		struct block_info orphan_info = {
			.block = &orphan_data, .x = PX + 3, .y = Y + 1, .z = Z};
		assert(block_piston_moving.onRightClickHandled);
		assert(block_piston_moving.onRightClickHandled(
			&s, &s.player, NULL, NULL, &orphan_info, SIDE_TOP));
		assert(get(PX + 3, Y + 1, Z) == BLOCK_AIR);
	}

	// =============================================================================================
	// SCENARIO G: vertical moveEntity ownership. An upward push clipped by a ceiling zeroes only Y
	// velocity and clears onGround; a downward push clipped by a floor sets onGround. The CRPC flags
	// carry exactly those vertical fields while retaining the position-only/view-preserving path.
	// =============================================================================================
	{
		const int PX = 8, Z = 38;
		floor_patch(PX - 1, PX + 1, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, 1); // +Y
		set(PX, Y + 1, Z, BLOCK_STONE, 0);
		set(PX, Y + 4, Z, BLOCK_STONE, 0); // ceiling bottom at Y+4
		s.player.x = PX + 0.5;
		s.player.y = (double)Y + 1.7 + 1.62; // feet Y+1.7; body top Y+3.5
		s.player.z = Z + 0.5;
		s.player.vel_y = -0.4F;
		s.player.vel_x = 0.0F;
		s.player.vel_z = 0.0F;
		s.player.on_ground = false;
		s.player.sneaking = false;
		/* Beta Entity.moveEntity leaves the scalar fallDistance unchanged by an
		 * upward piston displacement. CaveWii stores the same state as a peak Y,
		 * so that peak must rise with the body to preserve the accumulated 5.0. */
		s.player.fall_distance = 5.0F;
		server_local_refresh_player_membership(&s);
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe up = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &up);
		double before = s.player.y;
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(s.player.y - (before + 0.5)) < 1.0e-6);
		assert(fabsf(s.player.fall_distance - 5.0F) < 1.0e-6F);
		assert(s.player.vel_y == 0.0F && up.player_pos_count == 1);
		assert(up.player_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY | PLAYER_CORRECTION_ZERO_VEL_Y
				   | PLAYER_CORRECTION_SET_ON_GROUND));
		/* The next ordinary falling sample must add to, not restart, the scalar
		 * preserved by the lift. This catches the former fall-start-Y encoding's
		 * delayed erase after the immediate piston correction. */
		s.reject_player_pos_this_update = false;
		s.player.reject_pos_this_update = false; /* the per-actor latch too */
		struct server_rpc resumed_fall = {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos = {
				.x = s.player.x,
				.y = s.player.y - 0.1,
				.z = s.player.z,
				.vel_y = -0.1F,
				.on_ground = false,
				.correction_seq = s.player.correction_seq,
			},
		};
		server_local_test_process_rpc(&s, &resumed_fall);
		assert(fabsf(s.player.fall_distance - 5.1F) < 1.0e-5F);
	}
	{
		const int PX = 8, Z = 42;
		// Beta World.getCollidingBoundingBoxes scans from floor(minY)-1: the fence's
		// 1.5-block collision top lives above its own cell and must still clip the push.
		set(PX, Y, Z, BLOCK_FENCE, 0); // collision top at Y+1.5
		set(PX, Y + 4, Z, BLOCK_PISTON_BASE, 0); // -Y
		set(PX, Y + 3, Z, BLOCK_STONE, 0);
		s.player.x = PX + 0.5;
		s.player.y = (double)Y + 1.7 + 1.62; // feet Y+1.7: only .2 below to tall fence
		s.player.z = Z + 0.5;
		s.player.vel_y = -0.4F;
		s.player.on_ground = false;
		s.player.sneaking = false;
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.hurt_resist_time = 0;
		s.player.last_damage = 0;
		/* updateFallState calls fall(fallDistance) before clearing it on the
		 * landing branch; the final clipped -.2 displacement is not accumulated. */
		s.player.fall_distance = 5.0F;
		server_local_refresh_player_membership(&s);
		set(PX, Y + 3, Z - 1, BLOCK_STONE, 0); // standing-torch support
		set(PX, Y + 4, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe down = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &down);
		double before = s.player.y;
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(s.player.y - (before - 0.2)) < 1.0e-5);
		assert(s.player.health
			   == MAX_PLAYER_HEALTH - player_fall_damage_hp(5.0F, false));
		assert(s.player.fall_distance == 0.0F);
		assert(s.player.vel_y == 0.0F && down.player_pos_count == 1);
		assert(down.player_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY | PLAYER_CORRECTION_ZERO_VEL_Y
				   | PLAYER_CORRECTION_SET_ON_GROUND
				   | PLAYER_CORRECTION_ON_GROUND));
	}
	/* Monster physics historically carried a species mirror plus inherited
	 * Entity.fallDistance. The ordinary mob tick owns the species value, so every
	 * piston move must import it unconditionally before updateFallState. */
	{
		const int PX = 24, Z = 40;
		set(PX, Y, Z, BLOCK_FENCE, 0);
		set(PX, Y + 4, Z, BLOCK_PISTON_BASE, 0); // -Y
		set(PX, Y + 3, Z, BLOCK_STONE, 0);
		struct entity mob;
		entity_monster(790, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 1.7F;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = false;
		mob.health = 20;
		mob.fall_distance = 1.0F; /* stale inherited mirror */
		mob.data.monster.fall_distance = 5.0F; /* live ordinary-physics value */
		mob.data.monster.hurt_time = 0;
		mob.data.monster.hurt_resist_time = 0;
		mob.data.monster.last_damage = 0;
		mob.data.monster.entity_age = 123;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y + 3, Z - 1, BLOCK_STONE, 0);
		set(PX, Y + 4, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(mob.health == 18);
		assert(mob.fall_distance == 0.0F
			   && mob.data.monster.fall_distance == 0.0F);
		assert(mob.data.monster.entity_age == 0);
		assert(mob.data.monster.hurt_time == MOB_HURT_RESIST_TICKS);
		assert(mob.data.monster.hurt_resist_time == MOB_HURT_RESIST_TICKS);
		assert(mob.data.monster.last_damage == 2);
		dict_entity_erase(s.entities, mob.id);
	}
	/* EntityLiving.fall routes through attackEntityFrom. Even an absorbed fall
	 * resets entityAge, while the active hurt gate preserves health. */
	{
		const int PX = 24, Z = 44;
		set(PX, Y, Z, BLOCK_FENCE, 0);
		set(PX, Y + 4, Z, BLOCK_PISTON_BASE, 0); // -Y
		set(PX, Y + 3, Z, BLOCK_STONE, 0);
		struct entity mob;
		entity_monster(791, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 1.7F;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = false;
		mob.health = 20;
		mob.fall_distance = 5.0F;
		mob.data.monster.fall_distance = 5.0F;
		mob.data.monster.hurt_time = 5;
		mob.data.monster.hurt_resist_time = 5;
		mob.data.monster.last_damage = 5;
		mob.data.monster.entity_age = 123;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y + 3, Z - 1, BLOCK_STONE, 0);
		set(PX, Y + 4, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(mob.health == 20);
		assert(mob.data.monster.entity_age == 0);
		assert(mob.data.monster.hurt_time == 5);
		assert(mob.data.monster.hurt_resist_time == 5);
		assert(mob.data.monster.last_damage == 5);
		assert(mob.fall_distance == 0.0F
			   && mob.data.monster.fall_distance == 0.0F);
		dict_entity_erase(s.entities, mob.id);
	}
	/* Inside the resistance window, a stronger hit deals only the excess over
	 * field_9346_af and does not reopen either timer or apply a fresh-hit effect.
	 * [740c583: EntityLiving.attackEntityFrom] */
	{
		const int PX = 24, Z = 48;
		set(PX, Y, Z, BLOCK_FENCE, 0);
		set(PX, Y + 4, Z, BLOCK_PISTON_BASE, 0); // -Y
		set(PX, Y + 3, Z, BLOCK_STONE, 0);
		struct entity mob;
		entity_monster(792, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 1.7F;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = false;
		mob.health = 20;
		mob.fall_distance = 5.0F;
		mob.data.monster.fall_distance = 5.0F;
		mob.data.monster.hurt_time = 5;
		mob.data.monster.hurt_resist_time = 5;
		mob.data.monster.last_damage = 1;
		mob.data.monster.entity_age = 123;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y + 3, Z - 1, BLOCK_STONE, 0);
		set(PX, Y + 4, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(mob.health == 19); /* incoming 2 - prior strongest 1 */
		assert(mob.data.monster.entity_age == 0);
		assert(mob.data.monster.hurt_time == 5);
		assert(mob.data.monster.hurt_resist_time == 5);
		assert(mob.data.monster.last_damage == 2);
		assert(mob.fall_distance == 0.0F
			   && mob.data.monster.fall_distance == 0.0F);
		dict_entity_erase(s.entities, mob.id);
	}
	/* Ordinary EntityLiving landing must use the identical global window as a
	 * piston-forced landing. Seed the pre-decrement timer at 6: environmental
	 * processing decrements it to 5 before movement/fall. */
	{
		const int PX = 28, Z = 40;
		set(PX, Y, Z, BLOCK_STONE, 0);
		struct entity mob;
		entity_monster(793, &mob, true, &s.worlds[0], MONSTER_PIG);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 1.2F;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = false;
		mob.vel[1] = -0.5F;
		mob.health = 20;
		mob.fall_distance = 5.0F;
		mob.data.monster.fall_distance = 5.0F;
		mob.data.monster.hurt_time = 6;
		mob.data.monster.hurt_resist_time = 6;
		mob.data.monster.last_damage = 5;
		mob.data.monster.entity_age = 123;
		test_mob_sync(&mob); /* motion units S2 */
		assert(!mob.tick_server(&mob, &s));
		assert(mob.health == 20 && mob.data.monster.entity_age == 0);
		assert(mob.data.monster.hurt_time == 5
			   && mob.data.monster.hurt_resist_time == 5
			   && mob.data.monster.last_damage == 5);
		assert(mob.data.monster.fall_distance == 0.0F);
	}
	{
		const int PX = 28, Z = 44;
		set(PX, Y, Z, BLOCK_STONE, 0);
		struct entity mob;
		entity_monster(794, &mob, true, &s.worlds[0], MONSTER_PIG);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 1.2F;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = false;
		mob.vel[1] = -0.5F;
		mob.health = 20;
		mob.fall_distance = 5.0F;
		mob.data.monster.fall_distance = 5.0F;
		mob.data.monster.hurt_time = 6;
		mob.data.monster.hurt_resist_time = 6;
		mob.data.monster.last_damage = 1;
		mob.data.monster.entity_age = 123;
		test_mob_sync(&mob); /* motion units S2 */
		assert(!mob.tick_server(&mob, &s));
		assert(mob.health == 19 && mob.data.monster.entity_age == 0);
		assert(mob.data.monster.hurt_time == 5
			   && mob.data.monster.hurt_resist_time == 5
			   && mob.data.monster.last_damage == 2);
		assert(mob.data.monster.fall_distance == 0.0F);
	}
	/* Cactus collision happens from moveEntity after EntityLiving decrements
	 * heartsLife. With one tick left, the gate expires before the cactus hit. */
	{
		const int PX = 28, Z = 48;
		set(PX, Y, Z, BLOCK_SAND, 0);
		set(PX, Y + 1, Z, BLOCK_CACTUS, 0);
		struct entity mob;
		entity_monster(795, &mob, true, &s.worlds[0], MONSTER_PIG);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 1.0F;
		mob.pos[2] = Z + 0.5F;
		mob.health = 20;
		mob.data.monster.hurt_time = 1;
		mob.data.monster.hurt_resist_time = 1;
		mob.data.monster.last_damage = 1;
		test_mob_sync(&mob); /* motion units S2 */
		assert(!mob.tick_server(&mob, &s));
		assert(mob.health == 19);
		assert(mob.data.monster.hurt_time == MOB_HURT_ANIMATION_TICKS
			   && mob.data.monster.hurt_resist_time == MOB_HURT_RESIST_TICKS
			   && mob.data.monster.last_damage == MOB_CACTUS_DAMAGE);
	}
	/* A moveEntity-time lethal callback runs after onEntityUpdate's deathTime
	 * phase. Beta calls onDeath/dropFewItems immediately, but leaves deathTime at
	 * zero until the next tick; that tick must not drop the loot a second time. */
	{
		const int PX = 32, Z = 48;
		set(PX, Y, Z, BLOCK_SAND, 0);
		set(PX, Y + 1, Z, BLOCK_CACTUS, 0);
		struct entity mob;
		entity_monster(796, &mob, true, &s.worlds[0], MONSTER_SHEEP);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 1.0F;
		mob.pos[2] = Z + 0.5F;
		mob.health = 1;
		size_t entities_before = dict_entity_size(s.entities);
		test_mob_sync(&mob); /* motion units S2 */
		assert(!mob.tick_server(&mob, &s));
		assert(mob.health == 0 && mob.data.monster.death_time == 0);
		assert(dict_entity_size(s.entities) == entities_before + 1);
		test_mob_sync(&mob); /* motion units S2 */
		assert(!mob.tick_server(&mob, &s));
		assert(mob.data.monster.death_time == 1);
		assert(dict_entity_size(s.entities) == entities_before + 1);
	}
	/* TileEntityPiston invokes Entity.moveEntity too: fall arbitration precedes
	 * the same final-AABB cactus callback during the push, even when the mob's
	 * ordinary entity tick already ran earlier in the world update. */
	{
		const int PX = 32, Z = 52;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 2, Y - 1, Z, BLOCK_SAND, 0);
		set(PX + 2, Y, Z, BLOCK_CACTUS, 0);
		struct entity mob;
		entity_monster(797, &mob, true, &s.worlds[0], MONSTER_PIG);
		mob.pos[0] = PX + 1.5F;
		mob.pos[1] = (float)Y;
		mob.pos[2] = Z + 0.5F;
		mob.health = 20;
		mob.data.monster.knockback_time = 2;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(mob.pos[0] > PX + 1.5F);
		assert(mob.health == 19);
		assert(mob.data.monster.hurt_resist_time == MOB_HURT_RESIST_TICKS
			   && mob.data.monster.last_damage == MOB_CACTUS_DAMAGE);
		dict_entity_erase(s.entities, mob.id);
	}
	/* TileEntityPiston calls Entity.moveEntity for the player too. The final
	 * AABB sweep is not a center-cell sample: this player remains centered in
	 * z==Z, but the +Z flank enters two stacked cactus cells after the +X push.
	 * Entity.moveEntity scans floor(min+.001)..floor(max-.001), X/Y/Z order;
	 * BlockCactus calls attackEntityFrom(null, 1) for each cell. The shared
	 * resistance window therefore admits exactly one point. [740c583:
	 * TileEntityPiston.func_31010_a, Entity.moveEntity, BlockCactus] */
	{
		const int PX = 36, Z = 52;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 2, Y - 1, Z + 1, BLOCK_SAND, 0);
		set(PX + 2, Y, Z + 1, BLOCK_CACTUS, 0);
		set(PX + 2, Y + 1, Z + 1, BLOCK_CACTUS, 0);
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.x = PX + 1.5;
		s.player.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = Z + 0.75; /* center misses cactus; +.3 flank overlaps */
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.hurt_resist_time = 0;
		s.player.last_damage = 0;
		s.player.on_ground = true;
		s.player.sneaking = false;
		server_local_refresh_player_membership(&s);
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(s.player.x > PX + 1.5);
		assert(floor(s.player.z) == Z); /* later center-cell hazard still misses */
		assert(s.player.health
			   == MAX_PLAYER_HEALTH - COMBAT_HEALTH_PER_HEART / 2);
		assert(s.player.hurt_resist_time == PLAYER_HURT_RESIST_TICKS
			   && s.player.last_damage == COMBAT_HEALTH_PER_HEART / 2);
	}

	// =============================================================================================
	// SCENARIO H: Entity.moveEntity's grounded stepHeight=.5 branch. A flat +X push would stop at
	// the slab's vertical face after .5, but the Beta candidate steps up .5 and retains the full
	// .5625 piston displacement. The correction owns the resulting Y/onGround field without zeroing
	// unrelated velocity. [740c583: EntityLiving.<init>, Entity.moveEntity]
	// =============================================================================================
	{
		const int PX = 7, Z = 45;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		set(PX + 3, Y, Z, BLOCK_SLAB, 0); // bottom slab: top at feet Y+.5
		s.player.x = PX + 2.2;
		s.player.y = (double)Y + 1.62; // Beta EntityPlayer yOffset / Cave server eye basis
		s.player.z = Z + 0.5;
		s.player.vel_x = 0.3F;
		s.player.on_ground = true;
		s.player.sneaking = false;
		server_local_refresh_player_membership(&s);
		double before_x = s.player.x, before_y = s.player.y;
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe step = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &step);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(s.player.x - (before_x + 0.5625)) < 1.0e-6);
		assert(fabs(s.player.y - (before_y + 0.5)) < 1.0e-6);
		assert(s.player.vel_x == 0.3F && step.player_pos_count == 1);
		assert(step.player_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY
				   | PLAYER_CORRECTION_SET_ON_GROUND));
	}

	// Clipped horizontal Entity.moveEntity zeroes only that motion component. Pin both a +X and a
	// -Z wall so the position-only correction cannot accidentally erase view/unrelated velocity or
	// omit one sign/axis from the protocol flags.
	{
		const int PX = 7, Z = 39;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		set(PX + 3, Y, Z, BLOCK_STONE, 0); // full-height wall; .5 step cannot clear it
		s.player.x = PX + 2.2;
		s.player.y = (double)Y + 1.62;
		s.player.z = Z + 0.5;
		s.player.vel_x = 0.3F;
		s.player.vel_z = -0.2F;
		s.player.on_ground = true;
		s.player.sneaking = false;
		server_local_refresh_player_membership(&s);
		double before_x = s.player.x, before_y = s.player.y;
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe clip_x = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &clip_x);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(s.player.x - (before_x + 0.5)) < 1.0e-6);
		assert(s.player.y == before_y);
		assert(s.player.vel_x == 0.0F && s.player.vel_z == -0.2F);
		assert(clip_x.player_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY
				   | PLAYER_CORRECTION_ZERO_VEL_X
				   | PLAYER_CORRECTION_SET_ON_GROUND));
	}
	{
		const int PX = 14, Z = 45;
		floor_patch(PX - 1, PX + 1, Z - 4, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, 2); // -Z
		set(PX, Y, Z - 1, BLOCK_STONE, 0);
		set(PX, Y, Z - 3, BLOCK_STONE, 0); // full-height wall beyond destination air
		s.player.x = PX + 0.5;
		s.player.y = (double)Y + 1.62;
		s.player.z = Z - 1.2;
		s.player.vel_x = 0.2F;
		s.player.vel_z = -0.3F;
		s.player.on_ground = true;
		s.player.sneaking = false;
		server_local_refresh_player_membership(&s);
		double before_z = s.player.z, before_y = s.player.y;
		set(PX - 1, Y, Z, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe clip_z = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &clip_z);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(s.player.z - (before_z - 0.5)) < 1.0e-6);
		assert(s.player.y == before_y);
		assert(s.player.vel_x == 0.2F && s.player.vel_z == 0.0F);
		assert(clip_z.player_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY
				   | PLAYER_CORRECTION_ZERO_VEL_Z
				   | PLAYER_CORRECTION_SET_ON_GROUND));
	}

	// =============================================================================================
	// SCENARIO I: non-player Entity.moveEntity state is persistent and corrected authoritatively.
	// The ordinary flat push pins ySize*=.4 and the logical-pos/physical-box invariant. The slab
	// branch pins the successful-step fractional-minY formula. Finally a cart clipped along its
	// active heading must clear BOTH generic motionX and Cave's scalar rail speed, then send those
	// typed values in one correction. [740c583: Entity.moveEntity, EntityMinecart.<init>]
	// =============================================================================================
	{
		const int PX = 8, Z = 50;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		struct entity mob;
		entity_monster(800, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 2.2F;
		mob.y_size = 0.1F;
		mob.pos[1] = (float)Y - mob.y_size; /* physical bbox minY == Y */
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = true;
		mob.fall_distance = 1.25F;
		mob.data.monster.fall_distance = 1.25F;
		/* G6d: a BURNING zombie. The server-side e->on_fire of a mortal mob
		 * is never maintained (only set at load); the published flag is
		 * fire_time > 0, and the correction must carry that, or the client
		 * (client_interface.c) drops the flame until the next MOVE. */
		mob.data.monster.fire_time = 100;
		mob.on_fire = false;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe flat = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &flat);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabsf(mob.pos[0] - ((float)PX + 2.7625F)) < 1.0e-6F);
		assert(fabsf(mob.y_size - 0.04F) < 1.0e-6F);
		assert(fabsf(mob.pos[1] - ((float)Y - 0.04F)) < 1.0e-6F);
		assert(!mob.on_ground && !mob.collided_horizontally
			   && !mob.collided_vertically && !mob.collided);
		assert(mob.fall_distance == 1.25F
			   && mob.data.monster.fall_distance == 1.25F);
		assert(flat.entity_physics_count == 1);
		assert(flat.entity_physics.type == CRPC_ENTITY_PHYSICS_CORRECTION);
		assert(flat.entity_physics.payload.entity_physics_correction.entity_id
			   == mob.id);
		assert(flat.entity_physics.payload.entity_physics_correction.y_size
			   == mob.y_size);
		printf("piston-pushed burning zombie correction on_fire=%d (want 1)\n",
			(int)flat.entity_physics.payload.entity_physics_correction.on_fire);
		fflush(stdout);
		assert(flat.entity_physics.payload.entity_physics_correction.on_fire);
		dict_entity_erase(s.entities, mob.id);
	}
	{
		const int PX = 8, Z = 54;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		set(PX + 3, Y, Z, BLOCK_SLAB, 0);
		struct entity mob;
		entity_monster(801, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 2.2F;
		mob.y_size = 0.1F;
		mob.pos[1] = (float)Y - mob.y_size;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = true; /* persistent pre-call state; no ground reprobe */
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe step = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &step);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabsf(mob.pos[0] - ((float)PX + 2.7625F)) < 1.0e-6F);
		/* .1*.4 + fractionalMinY(.5) + .01 = .55; logical pos is
		 * bbox.minY(Y+.5)-ySize, while the reconstructed box stays on slab. */
		assert(fabsf(mob.y_size - 0.55F) < 1.0e-6F);
		assert(fabsf(mob.pos[1] - ((float)Y - 0.05F)) < 1.0e-6F);
		struct AABB body;
		assert(mob.getBoundingBox(&mob, &body) == 1);
		assert(fabsf(body.y1 - ((float)Y + 0.5F)) < 1.0e-6F);
		/* Mob motion units S2: a mob's piston push is its own double
		 * Entity.moveEntity, whose flags are the decompile's: after the kept
		 * step the applied dy is the step's net rise while the requested dy is
		 * 0; the two differ, so isCollidedVertically is TRUE (and isCollided),
		 * onGround (which also needs a downward request) stays false [Entity.java:
		 * 405-410]. (The float pmove path reported no vertical collision.) */
		assert(!mob.on_ground && !mob.collided_horizontally
			   && mob.collided_vertically && mob.collided);
		assert(step.entity_physics_count == 1);
		dict_entity_erase(s.entities, mob.id);
	}
	{
		const int PX = 8, Z = 58;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		set(PX + 3, Y, Z, BLOCK_SLAB, 0);
		struct entity mob;
		entity_monster(802, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 2.2F;
		mob.y_size = 0.2F;
		mob.pos[1] = (float)Y - mob.y_size;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = true;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		/* .2*.4=.08 fails Entity's strict ySize<.05 step gate. */
		assert(fabsf(mob.pos[0] - ((float)PX + 2.7F)) < 1.0e-6F);
		assert(fabsf(mob.y_size - 0.08F) < 1.0e-6F);
		struct AABB body;
		assert(mob.getBoundingBox(&mob, &body) == 1);
		/* Mob motion units S2: the authoritative box is the double one (Beta
		 * boundingBox, on the floor face to the fixture's float seed); the float display box is
		 * pos_f + y_size_f, one float rounding away (a float ulp at y ~ 70 is
		 * ~7.6e-6). */
		assert(fabs(mob.data.monster.motion_d.bb.min[1] - (double)Y) < 1.0e-5);
		assert(fabsf(body.y1 - (float)Y) < 1.6e-5F);
		assert(mob.collided_horizontally && !mob.collided_vertically
			   && mob.collided);
		dict_entity_erase(s.entities, mob.id);
	}
	{
		const int PX = 8, Z = 62;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		set(PX + 3, Y, Z, BLOCK_STONE, 0);
		struct entity cart;
		entity_minecart(803, &cart, true, &s.worlds[0]);
		cart.pos[0] = PX + 2.01F;
		cart.pos[1] = (float)Y + cart.y_offset; /* physical minY == Y */
		cart.pos[2] = Z + 0.5F;
		cart.on_ground = true;
		cart.vel[0] = 0.3F;
		cart.vel[1] = 0.1F;
		cart.vel[2] = -0.2F;
		cart.data.minecart.speed = 0.3F;
		cart.data.minecart.hx = 1;
		cart.data.minecart.hz = 0;
		*dict_entity_safe_get(s.entities, cart.id) = &cart;
		assert(server_local_test_entity_join(&s, cart.id));
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe clip = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &clip);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(cart.vel[0] == 0.0F && cart.vel[1] == 0.1F
			   && cart.vel[2] == -0.2F);
		assert(cart.data.minecart.speed == 0.0F);
		assert(clip.entity_physics_count == 1);
		assert(clip.entity_physics.payload.entity_physics_correction.minecart_speed
			   == 0.0F);
		assert(clip.entity_physics.payload.entity_physics_correction.minecart_hx
			   == 1);
		dict_entity_erase(s.entities, cart.id);
	}

	// =============================================================================================
	// SCENARIO J: persistent player sneak owns the exact .05 ledge clamp. The
	// requested first-visit push is .5625; at this floor edge .5625 and .5125
	// lose support, so Beta's loop retains .4625. Geometry re-probing onGround
	// after the call would miss this pre-call-only branch entirely.
	// =============================================================================================
	{
		const int PX = 8, Z = 66;
		floor_patch(PX - 1, PX + 2, Z - 1, Z + 1); /* no floor at PX+3 */
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		s.player.x = PX + 2.79;
		s.player.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = Z + 0.5;
		s.reject_player_pos_this_update = false;
		s.player.reject_pos_this_update = false; /* the per-actor latch too */
		struct server_rpc sneak = {
			.type = SRPC_PLAYER_STATE,
			.payload.player_state = {
				.on_ground = true,
				.sneaking = true,
				.correction_seq = s.player.correction_seq,
			},
		};
		server_local_test_process_rpc(&s, &sneak);
		server_local_refresh_player_membership(&s);
		assert(s.player.on_ground && s.player.sneaking);
		double before = s.player.x;
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(fabs(s.player.x - (before + 0.4625)) < 1.0e-6);
		assert(!s.player.on_ground);
		tick();
		tick();
	}

	// =============================================================================================
	// SCENARIO K: query scratch OOM is transactional. The moving TE cannot write
	// progress, push a prefix, or settle until the complete unbounded comparator
	// snapshot exists; the following tick retries the identical visit.
	// =============================================================================================
	{
		const int PX = 8, Z = 70;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		struct entity mob;
		entity_monster(804, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 1.2F;
		mob.pos[1] = (float)Y;
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = true;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_moving_state moving;
		assert(piston_moving_state_at(&s, PX + 1, Y, Z, &moving));
		assert(moving.current_progress == 0.0F);
		float before = mob.pos[0];
		server_local_test_fail_next_entity_query_reserve();
		tick();
		assert(piston_moving_state_at(&s, PX + 1, Y, Z, &moving));
		assert(moving.current_progress == 0.0F && mob.pos[0] == before);
		tick();
		assert(fabsf(mob.pos[0] - (before + 0.5625F)) < 1.0e-6F);
		dict_entity_erase(s.entities, mob.id);
		tick();
		tick();
	}

	// =============================================================================================
	// SCENARIO L: Chunk-list insertion order is observable for minecarts. With
	// the trailing cart appended second, the leading cart clips to the .005 gap.
	// Crossing a chunk and re-entering re-appends that same cart after its peer;
	// the peer then moves first and both receive the full .5625 displacement.
	// [740c583: World.getEntitiesWithinAABBExcludingEntity; Chunk.addEntity /
	// getEntitiesWithinAABBForEntity; EntityMinecart.getCollisionBox]
	// =============================================================================================
	{
		/* Keep the player in the same sorted snapshot but outside both queries. */
		s.player.x = 40.5;
		s.player.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = 40.5;
		server_local_refresh_player_membership(&s);

		const int PX = 8, Z = 74;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		struct entity first, second;
		entity_minecart(900, &first, true, &s.worlds[0]);
		entity_minecart(901, &second, true, &s.worlds[0]);
		first.pos[0] = PX + 2.0F;
		first.pos[1] = (float)Y + first.y_offset;
		first.pos[2] = Z + 0.5F;
		second.pos[0] = PX + 2.985F;
		second.pos[1] = (float)Y + second.y_offset;
		second.pos[2] = Z + 0.5F;
		*dict_entity_safe_get(s.entities, first.id) = &first;
		assert(server_local_test_entity_join(&s, first.id));
		*dict_entity_safe_get(s.entities, second.id) = &second;
		assert(server_local_test_entity_join(&s, second.id));
		assert(server_local_prepare_entity_query_snapshot(&s));
		size_t count = 0, first_i = SIZE_MAX, second_i = SIZE_MAX;
		size_t player_i = SIZE_MAX;
		const struct server_entity_query_ref* refs
			= server_local_entity_query_snapshot(&s, &count);
		for(size_t i = 0; i < count; i++) {
			if(refs[i].player)
				player_i = i;
			if(!refs[i].player && refs[i].id == first.id)
				first_i = i;
			if(!refs[i].player && refs[i].id == second.id)
				second_i = i;
		}
		assert(first_i < second_i && second_i < player_i);
		float first_before = first.pos[0], second_before = second.pos[0];
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(fabsf(first.pos[0] - (first_before + 0.005F)) < 2.0e-5F);
		assert(fabsf(second.pos[0] - (second_before + 0.5625F)) < 1.0e-6F);
		dict_entity_erase(s.entities, first.id);
		dict_entity_erase(s.entities, second.id);
		tick();
		tick();
	}
	{
		const int PX = 8, Z = 78;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		struct entity reentered, peer;
		entity_minecart(902, &reentered, true, &s.worlds[0]);
		entity_minecart(903, &peer, true, &s.worlds[0]);
		reentered.pos[0] = PX + 2.0F;
		reentered.pos[1] = (float)Y + reentered.y_offset;
		reentered.pos[2] = Z + 0.5F;
		peer.pos[0] = PX + 2.985F;
		peer.pos[1] = (float)Y + peer.y_offset;
		peer.pos[2] = Z + 0.5F;
		*dict_entity_safe_get(s.entities, reentered.id) = &reentered;
		assert(server_local_test_entity_join(&s, reentered.id));
		*dict_entity_safe_get(s.entities, peer.id) = &peer;
		assert(server_local_test_entity_join(&s, peer.id));
		/* removeEntityAtIndex + Chunk.addEntity appends on each tuple change */
		reentered.pos[0] = 32.5F;
		server_local_refresh_entity_membership(&s, &reentered);
		reentered.pos[0] = PX + 2.0F;
		server_local_refresh_entity_membership(&s, &reentered);
		assert(server_local_prepare_entity_query_snapshot(&s));
		size_t count = 0, reentered_i = SIZE_MAX, peer_i = SIZE_MAX;
		const struct server_entity_query_ref* refs
			= server_local_entity_query_snapshot(&s, &count);
		for(size_t i = 0; i < count; i++) {
			if(!refs[i].player && refs[i].id == reentered.id)
				reentered_i = i;
			if(!refs[i].player && refs[i].id == peer.id)
				peer_i = i;
		}
		assert(peer_i < reentered_i);
		float reentered_before = reentered.pos[0], peer_before = peer.pos[0];
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(fabsf(peer.pos[0] - (peer_before + 0.5625F)) < 1.0e-6F);
		assert(fabsf(reentered.pos[0] - (reentered_before + 0.5625F))
			   < 1.0e-6F);
		dict_entity_erase(s.entities, reentered.id);
		dict_entity_erase(s.entities, peer.id);
		tick();
		tick();
	}

	/* Bug-hunt D1 + D3: an explosion that carves a mid-move id-36 cell must (D1) run Beta
	 * BlockPistonMoving.onBlockRemoval -> clearPistonTileEntity, so the in-flight TE dies with its
	 * cell -- no ghost stored-block materialization into the crater on a later settle tick -- and
	 * (D3) drop the STORED block unconditionally: dropBlockAsItemWithChance ignores the explosion's
	 * 0.3F and delegates to the stored block's dropBlockAsItem (chance 1.0). The pushed block is
	 * PLANKS because its drop is unique in this blast (stone floor drops cobble at 30%, torch and
	 * base drop their own items), so the exactly-one assert cannot be masked by floor-drop noise.
	 * [740c583: BlockPistonMoving.onBlockRemoval/.dropBlockAsItemWithChance,
	 * TileEntityPiston.clearPistonTileEntity:93-101, Explosion.doExplosionB] */
	{
		const int PX = 24, Z = 12;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_PLANKS, 0);
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5); // power -> the extend fires synchronously
		assert(getmeta(PX, Y, Z) == (FACE_PX | 8));
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING);
		struct piston_moving_state moving;
		assert(piston_moving_state_at(&s, PX + 2, Y, Z, &moving)); // stored PLANKS, mid-move
		assert(moving.stored_id == BLOCK_PLANKS);
		size_t planks_before = 0;
		{
			dict_entity_it_t eit;
			dict_entity_it(eit, s.entities);
			while(!dict_entity_end_p(eit)) {
				struct entity* e = dict_entity_ref(eit)->value;
				dict_entity_next(eit);
				if(e && e->type == ENTITY_ITEM
				   && e->data.item.item.id == BLOCK_PLANKS)
					planks_before++;
			}
		}
		server_world_explode(
			&s.worlds[0], (vec3) {PX + 2 + 0.5F, Y + 0.5F, Z + 0.5F}, 3.0F);
		// SEED GOLDEN: with this fixture's deterministic world RNG the carve rolls include the
		// blast-center PLANKS carrier (PX+2). If a future change to upstream RNG consumption
		// shifts this, re-tune the fixture -- the D1/D3 asserts below depend on PX+2 being carved.
		assert(get(PX + 2, Y, Z) == BLOCK_AIR);
		// D1: the carve's lifecycle removal cleared the carved cell's moving TE with its cell.
		assert(!piston_moving_state_at(&s, PX + 2, Y, Z, &moving));
		// The head-carrier cell's inclusion is a per-ray roll: if it was carved its TE must be
		// gone too; if it survived, its TE legitimately lives on to settle normally (Beta leaves
		// un-destroyed TileEntityPistons alone).
		assert((get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING)
			   == piston_moving_state_at(&s, PX + 1, Y, Z, &moving));
		// D3: the stored PLANKS dropped exactly once despite the explosion's 0.3F chance.
		size_t planks_after = 0;
		{
			dict_entity_it_t eit;
			dict_entity_it(eit, s.entities);
			while(!dict_entity_end_p(eit)) {
				struct entity* e = dict_entity_ref(eit)->value;
				dict_entity_next(eit);
				if(e && e->type == ENTITY_ITEM
				   && e->data.item.item.id == BLOCK_PLANKS)
					planks_after++;
			}
		}
		assert(planks_after == planks_before + 1);
		// No ghost settle (THE D1 regression): ticking past T+2 must not re-materialize the
		// stored PLANKS into the carved crater cell. (PX+1's post-blast fate -- orphan head
		// settle, retraction after a carved torch -- is legitimate Beta dynamics, not pinned.)
		for(int k = 0; k < 3; k++)
			tick();
		assert(get(PX + 2, Y, Z) != BLOCK_PLANKS);
		assert(get(PX + 2, Y, Z) != BLOCK_PISTON_MOVING);
	}

	/* Deviation-#18: a lethal cactus callback now leaves a still-addressable
	 * CORPSE -- exactly Beta's dead-EntityPlayer model. The remaining
	 * stacked-cell callbacks continue against it and are absorbed by the
	 * dead-guard; only the explicit respawn constructs the fresh spawn, which
	 * no callback from this pass can touch by construction. */
	{
		const int PX = 12, Z = 76;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 2, Y - 1, Z + 1, BLOCK_SAND, 0);
		set(PX + 2, Y, Z + 1, BLOCK_CACTUS, 0);
		set(PX + 2, Y + 1, Z + 1, BLOCK_CACTUS, 0);
		s.player.x = PX + 1.5;
		s.player.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = Z + 0.75;
		s.player.vel_x = s.player.vel_y = s.player.vel_z = 0.0F;
		s.player.on_ground = true;
		s.player.sneaking = false;
		s.player.has_pos = true;
		s.player.finished_loading = true;
		/* An earlier scenario's respawn leaves the one-update position
		 * rejection latched; this fixture owns a fresh update. */
		s.reject_player_pos_this_update = false;
		s.player.reject_pos_this_update = false; /* the per-actor latch too */
		s.player.health = COMBAT_HEALTH_PER_HEART / 2;
		s.player.hurt_resist_time = 0;
		s.player.last_damage = 0;
		s.player.fall_distance = 0.0F;
		s.player.spawn_x = 2;
		s.player.spawn_y = Y;
		s.player.spawn_z = 2;
		s.player.dimension = WORLD_DIM_OVERWORLD;
		s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
		s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s.death_world_evict = false;
		s.paused = false;
		s.input_ack_pending = 0;
		s.portal_origin.valid = false;
		s.portal_job = NULL;
		struct item_data empty_inventory[INVENTORY_SIZE] = {0};
		s.player.inventory = (struct inventory) {
			.items = empty_inventory,
			.capacity = INVENTORY_SIZE,
		};
		s.player.active_inventory = &s.player.inventory;
		server_local_refresh_player_membership(&s);
		struct piston_rpc_probe lethal_cactus = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &lethal_cactus);
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		/* Dead commit only: the corpse absorbed the second cactus cell. */
		assert(s.player.health <= 0);
		assert(!s.reject_player_pos_this_update && s.player.finished_loading);
		assert(s.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		assert(server_local_respawn_request(&s));
		assert(s.player.health == MAX_PLAYER_HEALTH); /* no second-cell hit on spawn */
		assert(s.reject_player_pos_this_update && !s.player.finished_loading);
		assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
		assert(lethal_cactus.player_pos_count == 0);
	}

	// =============================================================================================
	// SCENARIO M: a lethal piston landing replaces the player inside the world-tick phase. The
	// replacement owns a loading barrier immediately: no stale piston correction, autosave, stream,
	// or old-world hazard work may run later in the same server update.
	// =============================================================================================
	{
		const int PX = 12, Z = 30;
		set(PX, Y, Z, BLOCK_FENCE, 0);
		set(PX, Y + 4, Z, BLOCK_PISTON_BASE, 0); // -Y
		set(PX, Y + 3, Z, BLOCK_STONE, 0);
		s.player.x = PX + 0.5;
		s.player.y = (double)Y + 1.7 + PLAYER_EYE_HEIGHT;
		s.player.z = Z + 0.5;
		s.player.vel_y = -0.4F;
		s.player.vel_x = 0.0F;
		s.player.vel_z = 0.0F;
		s.player.on_ground = false;
		s.player.sneaking = false;
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.health = 1;
		s.player.hurt_resist_time = 0;
		s.player.last_damage = 0;
		s.player.fall_distance = 5.0F;
		s.player.spawn_x = 2;
		s.player.spawn_y = Y;
		s.player.spawn_z = 2;
		s.player.dimension = WORLD_DIM_OVERWORLD;
		s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
		s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s.death_world_evict = false;
		s.paused = false;
		s.input_ack_pending = 0;
		s.portal_origin.valid = false;
		s.portal_job = NULL;
		struct item_data empty_inventory[INVENTORY_SIZE] = {0};
		s.player.inventory = (struct inventory) {
			.items = empty_inventory,
			.capacity = INVENTORY_SIZE,
		};
		s.player.active_inventory = &s.player.inventory;
		s.autosave_ticks = 10;
		s.autosave_timer = 3;
		server_local_refresh_player_membership(&s);
		set(PX, Y + 3, Z - 1, BLOCK_STONE, 0);
		set(PX, Y + 4, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe lethal = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &lethal);
		server_local_test_update_once(&s);
		clin_test_set_rpc_observer(NULL, NULL);
		/* Dead commit only: the dwell world keeps running (the autosave
		 * timer may legitimately tick -- saving a dead player is the
		 * quit-while-dead shape), the corpse stays, and the loading barrier
		 * arms only at the explicit respawn below. */
		assert(s.player.health <= 0);
		assert(!s.reject_player_pos_this_update && s.player.finished_loading);
		assert(s.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		assert(s.input_ack_pending == 0);
		assert(server_local_respawn_request(&s));
		assert(s.player.health == MAX_PLAYER_HEALTH);
		assert(s.reject_player_pos_this_update && !s.player.finished_loading);
		assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
		assert(s.input_ack_pending == 0);
		assert(!s.player.on_ground && !s.player.sneaking);
		assert(lethal.player_pos_count == 0);
	}

	/* SCENARIO N (S1-e RED): Beta TileEntityPiston gets every entity in the
	 * moving box and invokes moveEntity on each. Put only a bound guest in the
	 * first extending wool carrier; the host is far away. The guest must receive
	 * its own displacement and generation-stamped authoritative correction,
	 * while the host state/RPC route stays untouched. [b1.7.3 server decompile:
	 * TileEntityPiston.java:52-75] */
	{
		const int PX = 40, Z = 70;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_WOOL, 4);
		s.player.x = 2.5;
		s.player.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = 2.5;
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.reject_pos_this_update = false;
		server_local_refresh_player_membership(&s);
		struct server_player guest;
		memset(&guest, 0, sizeof guest);
		guest.x = (double)PX + 1.6;
		guest.y = (double)Y + PLAYER_EYE_HEIGHT;
		guest.z = (double)Z + 0.7625;
		guest.has_pos = true;
		guest.finished_loading = true;
		guest.health = MAX_PLAYER_HEALTH;
		guest.on_ground = true;
		struct item_data guest_items[INVENTORY_SIZE] = {0};
		struct guest_health_probe guest_health = {0};
		guest.inventory = (struct inventory) {
			.items = guest_items,
			.capacity = INVENTORY_SIZE,
			.reply = observe_guest_owned_reply,
			.reply_user = &guest_health,
		};
		guest.active_inventory = &guest.inventory;
		s.guest_actor = &guest;
		s.guest_generation = 123;
		s.guest_ticket = 456;
		s.guest_owned = true;
		s.guest_confirmed = true;
		s.guest_gameplay = true;
		/* v5: the admitted guest is a Chunk.entities[] member of its own
		 * (production refreshes it on every guest pose / update start). */
		server_local_refresh_guest_membership(&s);
		assert(guest.added_to_chunk);
		double host_x = s.player.x;
		uint32_t host_seq = s.player.correction_seq;
		uint32_t guest_seq = guest.correction_seq;
		struct piston_rpc_probe guest_push = {0};
		set(PX + 2, Y - 1, Z + 1, BLOCK_SAND, 0);
		set(PX + 2, Y, Z + 1, BLOCK_CACTUS, 0);
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		output_quiesce(); /* guest correction uses a nonblocking RPC token */
		clin_test_set_rpc_observer(observe_piston_rpc, &guest_push);
		tick(); /* last=0,current=.5: carrier push is .5 + .0625 */
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(guest.x - ((double)PX + 2.1625)) < 1.0e-9
			   && "moving piston must displace the guest body");
		assert(s.player.x == host_x && s.player.correction_seq == host_seq
			   && guest_push.player_pos_count == 0
			   && "guest-only piston push must not mutate/correct the host");
		assert(guest.correction_seq != 0
			   && guest_push.guest_correction_count == 1
			   && guest_push.guest_correction_generation == 123
			   && "piston writeback must use the live guest generation route");
		assert(guest_push.guest_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY
				   | PLAYER_CORRECTION_SET_ON_GROUND)
			   && "an unclipped X shove must preserve every velocity axis");
		assert(guest.correction_seq != guest_seq);
		assert(guest.health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2
			   && guest_health.count == 1
			   && "piston cactus contact bills the guest exactly once");
		short piston_cactus_health = guest.health;
		server_local_test_update_once(&s);
		assert(guest.health == piston_cactus_health && guest_health.count == 1
			   && "ordinary contact after the piston is absorbed by one i-frame");
		uint32_t corrected_seq = guest.correction_seq;
		double corrected_x = guest.x;
		struct server_rpc stale = {.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = corrected_x - 1.0, .y = guest.y,
				.z = guest.z, .correction_seq = guest_seq}};
		server_local_guest_position(&s, &stale);
		assert(guest.x == corrected_x && guest.correction_seq == corrected_seq
			   && "pre-piston guest input must remain stale");
		assert(!server_local_guest_correct_position_for(&s, 122,
			PLAYER_CORRECTION_POSITION_ONLY)
			   && guest.correction_seq == corrected_seq
			   && "a stale generation must not bump or send a correction");
		guest.reject_pos_this_update = false;
		s.player.reject_pos_this_update = false;
		s.reject_player_pos_this_update = false;
		output_quiesce();
		assert(server_local_guest_correct_position_for(&s, 123,
			PLAYER_CORRECTION_POSITION_ONLY)
			   && guest.reject_pos_this_update
			   && !s.player.reject_pos_this_update
			   && !s.reject_player_pos_this_update
			   && "guest correction must raise only the guest input barrier");

		/* The synthetic player sentinel expands in its old position, host first:
		 * overlapping bodies each move once and receive independent tokens. */
		const int BOTH_PX = 50;
		floor_patch(BOTH_PX - 1, BOTH_PX + 3, Z - 1, Z + 1);
		set(BOTH_PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(BOTH_PX + 1, Y, Z, BLOCK_WOOL, 4);
		s.player.x = guest.x = (double)BOTH_PX + 1.6;
		s.player.y = guest.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = guest.z = (double)Z + 0.5;
		s.player.reject_pos_this_update = guest.reject_pos_this_update = false;
		server_local_refresh_player_membership(&s);
		server_local_refresh_guest_membership(&s);
		uint32_t both_host_seq = s.player.correction_seq;
		uint32_t both_guest_seq = guest.correction_seq;
		struct piston_rpc_probe both = {0};
		set(BOTH_PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		output_quiesce();
		clin_test_set_rpc_observer(observe_piston_rpc, &both);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(s.player.x - ((double)BOTH_PX + 2.1625)) < 1.0e-9
			   && fabs(guest.x - ((double)BOTH_PX + 2.1625)) < 1.0e-9);
		assert(both.player_pos_count == 1 && both.guest_correction_count == 1
			   && s.player.correction_seq != both_host_seq
			   && guest.correction_seq != both_guest_seq);

		/* Physical presence starts at offer, before gameplay admission. */
		const int OFFER_PX = 60;
		floor_patch(OFFER_PX - 1, OFFER_PX + 3, Z - 1, Z + 1);
		set(OFFER_PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(OFFER_PX + 1, Y, Z, BLOCK_WOOL, 4);
		s.player.x = 2.5;
		guest.x = (double)OFFER_PX + 1.6;
		guest.y = (double)Y + PLAYER_EYE_HEIGHT;
		guest.z = (double)Z + 0.5;
		s.guest_gameplay = false;
		uint32_t offered_seq = guest.correction_seq;
		struct piston_rpc_probe offered = {0};
		set(OFFER_PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		output_quiesce();
		clin_test_set_rpc_observer(observe_piston_rpc, &offered);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(guest.x - ((double)OFFER_PX + 2.1625)) < 1.0e-9
			   && guest.correction_seq != offered_seq
			   && offered.guest_correction_count == 1
			   && offered.player_pos_count == 0);
		s.guest_generation = 124;
		assert(!server_local_guest_correct_position_for(&s, 123,
			PLAYER_CORRECTION_POSITION_ONLY)
			   && "a replacement generation must reject the old pass identity");
		s.guest_actor = NULL;
		s.guest_generation = 0;
		s.guest_owned = s.guest_confirmed = s.guest_gameplay = false;
		assert(!server_local_guest_correct_position_for(&s, 123,
			PLAYER_CORRECTION_POSITION_ONLY)
			   && "release before writeback must be a safe no-op");
	}

	/* SCENARIO O: the production guest accumulator must carry a real clipped
	 * Y/on-ground result into the client, then cancel only a lethally damaged
	 * guest while a later moving TE still pushes and corrects the host. */
	{
		const int PX = 44, Z = 40;
		set(PX, Y, Z, BLOCK_FENCE, 0);
		set(PX, Y + 4, Z, BLOCK_PISTON_BASE, 0); /* -Y */
		set(PX, Y + 3, Z, BLOCK_STONE, 0);
		s.player.x = s.player.z = 2.5;
		s.player.has_pos = s.player.finished_loading = true;
		server_local_refresh_player_membership(&s);
		struct server_player guest = {0};
		guest.x = PX + 0.5;
		guest.y = (double)Y + 1.7 + PLAYER_EYE_HEIGHT;
		guest.z = Z + 0.5;
		guest.vel_x = 0.25F; guest.vel_y = -0.4F; guest.vel_z = -0.2F;
		guest.has_pos = guest.finished_loading = true;
		guest.health = MAX_PLAYER_HEALTH;
		struct guest_health_probe owned_health = {0};
		guest.inventory.reply = observe_guest_owned_reply;
		guest.inventory.reply_user = &owned_health;
		s.guest_actor = &guest;
		s.guest_generation = 300;
		s.guest_ticket = 301;
		s.guest_owned = s.guest_confirmed = s.guest_gameplay = true;
		uint32_t host_seq = s.player.correction_seq;
		double before_y = guest.y;
		set(PX, Y + 3, Z - 1, BLOCK_STONE, 0);
		set(PX, Y + 4, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_rpc_probe clipped = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &clipped);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabs(guest.y - (before_y - 0.2)) < 1.0e-5);
		assert(guest.vel_x == 0.25F && guest.vel_y == 0.0F
			   && guest.vel_z == -0.2F && guest.on_ground);
		assert(clipped.player_pos_count == 0
			   && s.player.correction_seq == host_seq);
		assert(clipped.guest_correction_count == 1
			   && clipped.guest_correction_generation == 300
			   && clipped.guest_correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY
				   | PLAYER_CORRECTION_ZERO_VEL_Y
				   | PLAYER_CORRECTION_SET_ON_GROUND
				   | PLAYER_CORRECTION_ON_GROUND));

		struct entity client_body = {0};
		client_body.type = ENTITY_LOCAL_PLAYER;
		client_body.teleport = entity_default_teleport;
		client_body.vel[0] = 0.25F;
		client_body.vel[1] = -0.4F;
		client_body.vel[2] = -0.2F;
		struct client_session client = {0};
		client.local_ticket = 301;
		client.local_generation = 300;
		client.local_bound = true;
		client.local_phase = CLIENT_GUEST_ATTACHED;
		client.local_gameplay_ready = true;
		client.local_player = &client_body;
		client_session_local_status(&client, &clipped.guest_correction);
		assert(client_body.pos[0] == (float)guest.x
			   && client_body.pos[1] == (float)guest.y
			   && client_body.pos[2] == (float)guest.z);
		assert(client_body.vel[0] == 0.25F && client_body.vel[1] == 0.0F
			   && client_body.vel[2] == -0.2F && client_body.on_ground);
		assert(client.player_sync.correction_seq == guest.correction_seq);

		const int LPX = 40, BPX = 36, CPX = 44, LZ = 46;
		floor_patch(BPX - 1, CPX + 1, LZ - 1, LZ + 1);
		set(LPX, Y, LZ, BLOCK_PISTON_BASE, FACE_PX);
		set(LPX + 1, Y, LZ, BLOCK_WOOL, 4);
		set(BPX, Y, LZ, BLOCK_PISTON_BASE, FACE_PX);
		set(BPX + 1, Y, LZ, BLOCK_WOOL, 4);
		/* C runs after A and B. Its opposed carrier sweeps the dead guest's
		 * post-A box around x=42.2125, but is far from B's surviving host. */
		set(CPX, Y + 1, LZ, BLOCK_PISTON_BASE, FACE_NX);
		set(CPX - 1, Y + 1, LZ, BLOCK_WOOL, 4);
		set(LPX + 2, Y - 1, LZ + 1, BLOCK_SAND, 0);
		set(LPX + 2, Y, LZ + 1, BLOCK_CACTUS, 0);
		set(LPX + 2, Y + 1, LZ + 1, BLOCK_CACTUS, 0);
		/* B is four blocks beyond A. Its carrier is the only moving box that
		 * can touch this host, so its +X displacement proves the later TE ran. */
		s.player.x = BPX + 1.6;
		s.player.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = LZ + 0.1;
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.fall_distance = 0.0F;
		s.player.has_pos = s.player.finished_loading = true;
		s.player.reject_pos_this_update = false;
		server_local_refresh_player_membership(&s);
		guest.x = LPX + 1.65;
		guest.y = (double)Y + PLAYER_EYE_HEIGHT;
		guest.z = LZ + 0.75;
		guest.vel_x = guest.vel_y = guest.vel_z = 0.0F;
		guest.health = COMBAT_HEALTH_PER_HEART / 2;
		guest.hurt_resist_time = guest.last_damage = 0;
		guest.fall_distance = 0.0F;
		guest.on_ground = true;
		guest.reject_pos_this_update = false;
		s.guest_generation = 302;
		uint32_t lethal_guest_seq = guest.correction_seq;
		uint32_t lethal_host_seq = s.player.correction_seq;
		uint32_t host_deaths = __atomic_load_n(&s.player_deaths, __ATOMIC_ACQUIRE);
		int host_damage_cause = __atomic_load_n(&s.player_damage_cause, __ATOMIC_RELAXED);
		set(LPX, Y, LZ - 1, BLOCK_REDSTONE_TORCH_LIT, 5); /* A first */
		set(BPX, Y, LZ - 1, BLOCK_REDSTONE_TORCH_LIT, 5); /* B inserted later */
		set(CPX, Y, LZ - 1, BLOCK_STONE, 0); /* support C's elevated torch */
		set(CPX, Y + 1, LZ - 1, BLOCK_REDSTONE_TORCH_LIT, 5); /* C recontacts guest */
		struct piston_rpc_probe lethal = {0};
		clin_test_set_rpc_observer(observe_piston_rpc, &lethal);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(guest.health == 0 && owned_health.count == 1
			   && owned_health.health == 0 && lethal.global_health_count == 0);
		assert(s.guest_actor == &guest && s.guest_generation == 302);
		assert(guest.correction_seq == lethal_guest_seq
			   && lethal.guest_correction_count == 0);
		assert(fabs(guest.x - ((double)LPX + 2.2125)) < 1.0e-9);
		assert(s.player.health == MAX_PLAYER_HEALTH
			   && fabs(s.player.x - ((double)BPX + 2.1625)) < 1.0e-9
			   && "only later piston B can move the isolated host");
		struct piston_moving_state later;
		assert(piston_moving_state_at(&s, BPX + 1, Y, LZ, &later)
			   && later.current_progress > 0.0F
			   && "later piston B must advance in the same moving-TE pass");
		struct piston_moving_state recontact;
		assert(piston_moving_state_at(&s, CPX - 1, Y + 1, LZ, &recontact)
			   && recontact.current_progress > 0.0F
			   && "later opposed piston C must reach the cancelled guest");
		assert(lethal.player_pos_count == 1
			   && s.player.correction_seq != lethal_host_seq);
		assert(__atomic_load_n(&s.player_deaths, __ATOMIC_ACQUIRE) == host_deaths
			   && __atomic_load_n(&s.player_damage_cause, __ATOMIC_RELAXED)
			   == host_damage_cause);
		assert(guest.reject_pos_this_update
			   && "the surviving host correction retains its legacy both-body barrier");
		s.guest_actor = NULL;
		s.guest_generation = 0;
		s.guest_owned = s.guest_confirmed = s.guest_gameplay = false;
	}

	/* SCENARIO P: a pushed minecart uses the nearer guest AABB, then release
	 * restores the exact host-only gap. The players are obstacles, not pushes. */
	{
		const int PX = 40, Z = 58;
		floor_patch(PX - 1, PX + 4, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		s.player.x = PX + 3.10;
		s.player.y = (double)Y + PLAYER_EYE_HEIGHT;
		s.player.z = Z + 0.25;
		s.player.has_pos = s.player.finished_loading = true;
		server_local_refresh_player_membership(&s);
		struct server_player guest = {0};
		guest.x = PX + 2.90;
		guest.y = (double)Y + PLAYER_EYE_HEIGHT;
		guest.z = Z + 0.75;
		guest.has_pos = guest.finished_loading = true;
		guest.health = MAX_PLAYER_HEALTH;
		s.guest_actor = &guest;
		s.guest_generation = 400;
		s.guest_ticket = 401;
		s.guest_owned = s.guest_confirmed = s.guest_gameplay = true;
		uint32_t host_seq = s.player.correction_seq;
		uint32_t guest_seq = guest.correction_seq;
		struct entity cart;
		entity_minecart(980, &cart, true, &s.worlds[0]);
		cart.pos[0] = PX + 2.01F;
		cart.pos[1] = (float)Y + cart.y_offset;
		cart.pos[2] = Z + 0.5F;
		cart.on_ground = true;
		cart.vel[0] = 0.3F; cart.vel[1] = 0.1F; cart.vel[2] = -0.2F;
		cart.data.minecart.speed = 0.3F;
		cart.data.minecart.hx = 1;
		*dict_entity_safe_get(s.entities, cart.id) = &cart;
		assert(server_local_test_entity_join(&s, cart.id));
		struct piston_rpc_probe both = {0};
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		clin_test_set_rpc_observer(observe_piston_rpc, &both);
		tick();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(fabsf(cart.pos[0] - ((float)PX + 2.11F)) < 1.0e-6F);
		assert(cart.vel[0] == 0.0F && cart.vel[1] == 0.1F
			   && cart.vel[2] == -0.2F && cart.data.minecart.speed == 0.0F
			   && cart.data.minecart.hx == 1);
		assert(both.entity_physics_count == 1 && both.player_pos_count == 0
			   && both.guest_correction_count == 0);
		assert(s.player.correction_seq == host_seq
			   && guest.correction_seq == guest_seq
			   && s.player.x == PX + 3.10 && guest.x == PX + 2.90);
		dict_entity_erase(s.entities, cart.id);

		s.guest_actor = NULL;
		s.guest_generation = 0;
		s.guest_owned = s.guest_confirmed = s.guest_gameplay = false;
		const int Z2 = 62;
		floor_patch(PX - 1, PX + 4, Z2 - 1, Z2 + 1);
		set(PX, Y, Z2, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z2, BLOCK_STONE, 0);
		s.player.x = PX + 3.10;
		s.player.z = Z2 + 0.25;
		server_local_refresh_player_membership(&s);
		struct entity control;
		entity_minecart(981, &control, true, &s.worlds[0]);
		control.pos[0] = PX + 2.01F;
		control.pos[1] = (float)Y + control.y_offset;
		control.pos[2] = Z2 + 0.5F;
		control.on_ground = true;
		control.vel[0] = 0.3F; control.vel[1] = 0.1F; control.vel[2] = -0.2F;
		control.data.minecart.speed = 0.3F;
		control.data.minecart.hx = 1;
		*dict_entity_safe_get(s.entities, control.id) = &control;
		assert(server_local_test_entity_join(&s, control.id));
		set(PX, Y, Z2 - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		tick();
		assert(fabsf(control.pos[0] - ((float)PX + 2.31F)) < 1.0e-5F);
		assert(control.vel[0] == 0.0F && control.data.minecart.speed == 0.0F);
		dict_entity_erase(s.entities, control.id);
	}

	/* P1/P2: TileEntityPiston.func_31010_a calls Entity.moveEntity with the
	 * moving tile's Y delta (Beta TileEntityPiston.java:84). Entity.java:273,
	 * :417 derives onGround from the original downward request and clipping.
	 * The monster cache must carry that verdict into its next ordinary tick. */
	{
		const int PX = 40, Z = 72;
		floor_patch(PX - 1, PX + 1, Z - 1, Z + 1);
		set(PX, Y + 3, Z, BLOCK_PISTON_BASE, 0); /* downward */
		set(PX, Y + 2, Z, BLOCK_STONE, 0);
		struct entity mob;
		entity_monster(990, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y + 0.2F; /* feet above floor, top intersects first push */
		mob.pos[2] = Z + 0.5F;
		mob.on_ground = false;
		mob.data.monster.prev_on_ground = false;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, Y + 2, Z - 1, BLOCK_STONE, 0); /* standing-torch support */
		set(PX, Y + 3, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		struct piston_moving_state moving = {0};
		bool found = piston_moving_state_at(&s, PX, Y + 1, Z, &moving);
		fprintf(stderr,
			"P1 before tick torch=%d base=%d meta=%d TE=%d progress=%.3f\n",
			get(PX, Y + 3, Z - 1), get(PX, Y + 3, Z),
			getmeta(PX, Y + 3, Z), (int)found, moving.current_progress);
		assert(get(PX, Y + 3, Z - 1) == BLOCK_REDSTONE_TORCH_LIT);
		assert(get(PX, Y + 3, Z) == BLOCK_PISTON_BASE);
		assert(getmeta(PX, Y + 3, Z) == 8);
		assert(found && moving.current_progress == 0.0F);
		tick();
		found = piston_moving_state_at(&s, PX, Y + 1, Z, &moving);
		fprintf(stderr, "P1 after tick TE=%d progress=%.3f feet=%.7f on_ground=%d prev_on_ground=%d\n",
			(int)found, moving.current_progress, mob.pos[1],
			(int)mob.on_ground, (int)mob.data.monster.prev_on_ground);
		assert(found && moving.current_progress == 0.5F);
		assert(mob.on_ground && fabsf(mob.pos[1] - (float)Y) < 1.0e-5F);
		fprintf(stderr,
			"P1 tick progress=%.3f feet=%.7f on_ground=%d expected=1 prev_on_ground=%d expected=1\n",
			moving.current_progress, mob.pos[1], (int)mob.on_ground,
			(int)mob.data.monster.prev_on_ground);
		int cache_failures = mob.data.monster.prev_on_ground ? 0 : 1;
		dict_entity_erase(s.entities, mob.id);

		/* P2: the same real TE tick lifts a previously grounded monster. */
		const int Z2 = 75;
		floor_patch(PX - 1, PX + 1, Z2 - 1, Z2 + 1);
		set(PX, YF, Z2, BLOCK_PISTON_BASE, 1); /* upward, supports feet at Y */
		entity_monster(991, &mob, true, &s.worlds[0], MONSTER_ZOMBIE);
		mob.pos[0] = PX + 0.5F;
		mob.pos[1] = (float)Y;
		mob.pos[2] = Z2 + 0.5F;
		mob.on_ground = true;
		mob.data.monster.prev_on_ground = true;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(PX, YF - 1, Z2 - 1, BLOCK_STONE, 0);
		set(PX, YF, Z2 - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		memset(&moving, 0, sizeof moving);
		found = piston_moving_state_at(&s, PX, Y, Z2, &moving);
		fprintf(stderr,
			"P2 before tick torch=%d base=%d meta=%d TE=%d progress=%.3f\n",
			get(PX, YF, Z2 - 1), get(PX, YF, Z2),
			getmeta(PX, YF, Z2), (int)found, moving.current_progress);
		assert(get(PX, YF, Z2 - 1) == BLOCK_REDSTONE_TORCH_LIT);
		assert(get(PX, YF, Z2) == BLOCK_PISTON_BASE);
		assert(getmeta(PX, YF, Z2) == 9);
		assert(found && moving.current_progress == 0.0F);
		tick();
		found = piston_moving_state_at(&s, PX, Y, Z2, &moving);
		fprintf(stderr, "P2 after tick TE=%d progress=%.3f feet=%.7f on_ground=%d prev_on_ground=%d\n",
			(int)found, moving.current_progress, mob.pos[1],
			(int)mob.on_ground, (int)mob.data.monster.prev_on_ground);
		assert(found && moving.current_progress == 0.5F);
		assert(!mob.on_ground && mob.pos[1] > (float)Y);
		fprintf(stderr,
			"P2 tick progress=%.3f feet=%.7f on_ground=%d expected=0 prev_on_ground=%d expected=0\n",
			moving.current_progress, mob.pos[1], (int)mob.on_ground,
			(int)mob.data.monster.prev_on_ground);
		if(mob.data.monster.prev_on_ground)
			cache_failures++;
		dict_entity_erase(s.entities, mob.id);

		/* P3: a non-monster takes the same upward verdict through its ordinary
		 * on_ground field; its union must not be interpreted as monster state. */
		const int Z3 = 78;
		floor_patch(PX - 1, PX + 1, Z3 - 1, Z3 + 1);
		set(PX, YF, Z3, BLOCK_PISTON_BASE, 1);
		struct entity cart;
		entity_minecart(992, &cart, true, &s.worlds[0]);
		cart.pos[0] = PX + 0.5F;
		cart.pos[1] = (float)Y + cart.y_offset;
		cart.pos[2] = Z3 + 0.5F;
		cart.on_ground = true;
		*dict_entity_safe_get(s.entities, cart.id) = &cart;
		assert(server_local_test_entity_join(&s, cart.id));
		set(PX, YF - 1, Z3 - 1, BLOCK_STONE, 0);
		set(PX, YF, Z3 - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		memset(&moving, 0, sizeof moving);
		found = piston_moving_state_at(&s, PX, Y, Z3, &moving);
		fprintf(stderr,
			"P3 before tick torch=%d base=%d meta=%d TE=%d progress=%.3f\n",
			get(PX, YF, Z3 - 1), get(PX, YF, Z3),
			getmeta(PX, YF, Z3), (int)found, moving.current_progress);
		assert(get(PX, YF, Z3 - 1) == BLOCK_REDSTONE_TORCH_LIT);
		assert(get(PX, YF, Z3) == BLOCK_PISTON_BASE);
		assert(getmeta(PX, YF, Z3) == 9);
		assert(found && moving.current_progress == 0.0F);
		tick();
		found = piston_moving_state_at(&s, PX, Y, Z3, &moving);
		fprintf(stderr, "P3 after tick TE=%d progress=%.3f posY=%.7f on_ground=%d\n",
			(int)found, moving.current_progress, cart.pos[1],
			(int)cart.on_ground);
		assert(found && moving.current_progress == 0.5F);
		assert(!cart.on_ground && cart.pos[1] > (float)Y + cart.y_offset);
		fprintf(stderr,
			"P3 tick progress=%.3f posY=%.7f on_ground=%d expected=0\n",
			moving.current_progress, cart.pos[1], (int)cart.on_ground);
		dict_entity_erase(s.entities, cart.id);

		/* P4: the real moving carrier pushes a spider into a head-height wall.
		 * Feet above the floor and on_ground=false exclude the step-up branch. */
		const int HPX = 32, HZ = 60;
		for(int x = HPX - 1; x <= HPX + 4; x++)
			for(int z = HZ - 1; z <= HZ + 1; z++)
				for(int y = Y; y <= Y + 2; y++)
					set(x, y, z, BLOCK_AIR, 0);
		floor_patch(HPX - 1, HPX + 4, HZ - 1, HZ + 1);
		set(HPX, Y, HZ, BLOCK_PISTON_BASE, FACE_PX);
		set(HPX + 1, Y, HZ, BLOCK_STONE, 0);
		set(HPX + 3, Y + 1, HZ, BLOCK_STONE, 0);
		entity_monster(993, &mob, true, &s.worlds[0], MONSTER_SPIDER);
		mob.pos[0] = HPX + 2.0F;
		mob.pos[1] = (float)Y + 0.2F;
		mob.pos[2] = HZ + 0.5F;
		mob.on_ground = false;
		mob.data.monster.prev_on_ground = false;
		mob.data.monster.prev_collided_xz = false;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(HPX, Y, HZ - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		memset(&moving, 0, sizeof moving);
		found = piston_moving_state_at(&s, HPX + 1, Y, HZ, &moving);
		fprintf(stderr,
			"P4 before tick torch=%d base=%d meta=%d TE=%d progress=%.3f\n",
			get(HPX, Y, HZ - 1), get(HPX, Y, HZ),
			getmeta(HPX, Y, HZ), (int)found, moving.current_progress);
		assert(get(HPX, Y, HZ - 1) == BLOCK_REDSTONE_TORCH_LIT);
		assert(get(HPX, Y, HZ) == BLOCK_PISTON_BASE);
		assert(getmeta(HPX, Y, HZ) == (FACE_PX | 8));
		assert(found && moving.current_progress == 0.0F);
		tick();
		found = piston_moving_state_at(&s, HPX + 1, Y, HZ, &moving);
		fprintf(stderr,
			"P4 tick TE=%d progress=%.3f x=%.7f feet=%.7f collided_xz=%d prev_collided_xz=%d\n",
			(int)found, moving.current_progress, mob.pos[0], mob.pos[1],
			(int)mob.collided_horizontally, (int)mob.data.monster.prev_collided_xz);
		assert(found && moving.current_progress == 0.5F);
		assert(mob.collided_horizontally);
		if(!mob.data.monster.prev_collided_xz)
			cache_failures++;
		dict_entity_erase(s.entities, mob.id);

		/* P5: the same carrier pushes a spider through an open corridor. */
		const int HZ2 = 64;
		for(int x = HPX - 1; x <= HPX + 4; x++)
			for(int z = HZ2 - 1; z <= HZ2 + 1; z++)
				for(int y = Y; y <= Y + 2; y++)
					set(x, y, z, BLOCK_AIR, 0);
		floor_patch(HPX - 1, HPX + 4, HZ2 - 1, HZ2 + 1);
		set(HPX, Y, HZ2, BLOCK_PISTON_BASE, FACE_PX);
		set(HPX + 1, Y, HZ2, BLOCK_STONE, 0);
		entity_monster(994, &mob, true, &s.worlds[0], MONSTER_SPIDER);
		mob.pos[0] = HPX + 2.0F;
		mob.pos[1] = (float)Y + 0.2F;
		mob.pos[2] = HZ2 + 0.5F;
		mob.on_ground = false;
		mob.data.monster.prev_on_ground = false;
		mob.data.monster.prev_collided_xz = true;
		test_mob_sync(&mob); /* motion units S2: the fixture pins the doubles */
		*dict_entity_safe_get(s.entities, mob.id) = &mob;
		assert(server_local_test_entity_join(&s, mob.id));
		set(HPX, Y, HZ2 - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
		memset(&moving, 0, sizeof moving);
		found = piston_moving_state_at(&s, HPX + 1, Y, HZ2, &moving);
		fprintf(stderr,
			"P5 before tick torch=%d base=%d meta=%d TE=%d progress=%.3f\n",
			get(HPX, Y, HZ2 - 1), get(HPX, Y, HZ2),
			getmeta(HPX, Y, HZ2), (int)found, moving.current_progress);
		assert(get(HPX, Y, HZ2 - 1) == BLOCK_REDSTONE_TORCH_LIT);
		assert(get(HPX, Y, HZ2) == BLOCK_PISTON_BASE);
		assert(getmeta(HPX, Y, HZ2) == (FACE_PX | 8));
		assert(found && moving.current_progress == 0.0F);
		tick();
		found = piston_moving_state_at(&s, HPX + 1, Y, HZ2, &moving);
		fprintf(stderr,
			"P5 tick TE=%d progress=%.3f x=%.7f feet=%.7f collided_xz=%d prev_collided_xz=%d\n",
			(int)found, moving.current_progress, mob.pos[0], mob.pos[1],
			(int)mob.collided_horizontally, (int)mob.data.monster.prev_collided_xz);
		assert(found && moving.current_progress == 0.5F);
		assert(!mob.collided_horizontally);
		if(mob.data.monster.prev_collided_xz)
			cache_failures++;
		dict_entity_erase(s.entities, mob.id);
		fprintf(stderr, "piston monster contact-cache mismatches=%d\n",
			cache_failures);
		assert(cache_failures == 0);
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	piston_moving_store_destroy(&s.piston_moving);
	dict_entity_clear(s.entities);
	string_clear(level);
	printf("test_piston_move_env: OK (extend id-36 window + exact moving metadata/drop/collision/player "
		   "displacement + T+2 settle, non-sticky retract, the 12-limit refusal, tile-entity/bedrock "
		   "refusals, the BUD block-above quirk, and sticky pull through production)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
