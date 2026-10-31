/*
	Golden test for the Beta 1.7.3 sneak LEDGE-STOP (the risky half of sneak).

	Drives entity_sneak_ledge_clamp over a real server world with a hand-built platform and
	pins the invariant of Entity.moveEntity's onGround && isSneaking branch: the intended
	horizontal move is shrunk in 0.05 steps so the player's bbox never ends up over a gap --
	you cannot walk off an edge while sneaking. Verified per axis + diagonal, plus the
	no-clamp cases (open ground ahead).

	The 0.3x sneak SPEED (MovementInputFromOptions) is input-driven (input_held), so it is
	verified by the end-to-end agent-bridge walk measurement, not here. This test owns the
	collision-path logic.

	Decompile: 740c583:1.7.3-LTS Entity.moveEntity (sneak edge clamp).
*/

#include <assert.h>
#include <math.h>
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
#include "entity/entity.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_player_sneak_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define PLAT_Y 70
#define EYE_H 1.62F

static struct server_local s;

// Drain the client RPC channel so server-side world setup (chunk loads / set_block) can
// clin_rpc_send without blocking on a full inbox -- same rig as the other env tests.
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

static void set(int x, int y, int z, int type) {
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = 0}));
}

// The player's LOCAL collision box, matching entity_local_player's move bbox.
static void make_player_bbox(struct AABB* b) {
	aabb_setsize_centered(b, 0.6F, 1.8F, 0.6F);
	aabb_translate(b, 0.0F, 1.8F / 2.0F - EYE_H, 0.0F);
}

// Same probe the clamp uses internally: would stepping (dx,dz) keep ground under the feet?
static bool dest_has_ground(struct entity* e, struct AABB* bbox, float dx, float dz) {
	struct AABB probe = *bbox;
	aabb_translate(&probe, e->pos[0] + dx, e->pos[1] - 1.0F, e->pos[2] + dz);
	return entity_aabb_intersection(e, &probe);
}

static void put_player(struct entity* e, float x, float z, float vx, float vz) {
	memset(e, 0, sizeof(*e));
	e->on_server = true;
	e->world = &s.world;
	e->pos[0] = x;
	e->pos[1] = (float)(PLAT_Y + 1) + EYE_H; // feet on the platform top, pos.y is the eye
	e->pos[2] = z;
	e->vel[0] = vx;
	e->vel[1] = 0.0F;
	e->vel[2] = vz;
	e->on_ground = true;
}

static void test_edge_plus_x(void) {
	struct entity e;
	struct AABB bbox;
	make_player_bbox(&bbox);
	// At the +X lip (platform x in [6..11]; x>=12 is open air), moving hard +X.
	put_player(&e, 11.475F, 10.0F, 1.0F, 0.0F);
	assert(dest_has_ground(&e, &bbox, 0.0F, 0.0F)); // currently grounded
	entity_sneak_ledge_clamp(&e, &bbox);
	assert(e.vel[0] < 1.0F - 1e-4F);                 // the move was shrunk
	assert(e.vel[0] >= 0.0F);                         // never reversed
	// the clamped move keeps the player over ground (the ledge-stop invariant)
	assert(dest_has_ground(&e, &bbox, e.vel[0], 0.0F) || e.vel[0] == 0.0F);
	// one more 0.05 step would have gone over the gap
	assert(!dest_has_ground(&e, &bbox, e.vel[0] + 0.05F, 0.0F));
	printf("  +X edge: vel 1.0 -> %.3f, stays over ground, +1 step is a gap OK\n",
		   e.vel[0]);
}

static void test_edge_minus_z(void) {
	struct entity e;
	struct AABB bbox;
	make_player_bbox(&bbox);
	// At the -Z lip (platform z in [8..12]; z<8 is open air), moving hard -Z.
	put_player(&e, 9.0F, 8.525F, 0.0F, -1.0F);
	entity_sneak_ledge_clamp(&e, &bbox);
	assert(e.vel[2] > -1.0F + 1e-4F); // shrunk toward 0
	assert(e.vel[2] <= 0.0F);
	assert(dest_has_ground(&e, &bbox, 0.0F, e.vel[2]) || e.vel[2] == 0.0F);
	printf("  -Z edge: vel -1.0 -> %.3f, stays over ground OK\n", e.vel[2]);
}

static void test_open_ground_no_clamp(void) {
	struct entity e;
	struct AABB bbox;
	make_player_bbox(&bbox);
	// Dead centre of the platform: a small move stays over ground -> untouched.
	put_player(&e, 8.5F, 10.0F, 0.2F, 0.0F);
	entity_sneak_ledge_clamp(&e, &bbox);
	assert(fabsf(e.vel[0] - 0.2F) < 1e-6F); // unchanged
	assert(fabsf(e.vel[2] - 0.0F) < 1e-6F);
	printf("  open ground: vel 0.2 unchanged (no false clamp) OK\n");
}

static void test_diagonal_into_corner_gap(void) {
	struct entity e;
	struct AABB bbox;
	make_player_bbox(&bbox);
	// Heading toward the +X/-Z corner; both axes border a gap. Use a move big enough to
	// carry the 0.6-wide bbox off the edge row (a small move stays on it -- correctly no
	// clamp, since each axis is checked independently like Beta).
	put_player(&e, 11.475F, 8.525F, 1.0F, -1.0F);
	entity_sneak_ledge_clamp(&e, &bbox);
	assert(e.vel[0] < 1.0F);  // X shrunk
	assert(e.vel[2] > -1.0F); // Z shrunk
	assert(dest_has_ground(&e, &bbox, e.vel[0], 0.0F) || e.vel[0] == 0.0F);
	assert(dest_has_ground(&e, &bbox, 0.0F, e.vel[2]) || e.vel[2] == 0.0F);
	printf("  diagonal into corner: both axes clamped OK\n");
}

// PARITY FIX 2026-09-02 (off-by-one at the world ceiling): entity_intersection
// and entity_intersects_new_boxes's y-loop is EXCLUSIVE (`y < max_y`), so the
// clamp on max_y must be WORLD_HEIGHT (128, one past the last valid index),
// not WORLD_HEIGHT-1 (127) -- the old clamp made a block at y=127 (the top
// cell of the world) invisible to any entity collision sweep whose natural
// (unclamped) bound already exceeded 127. Not a Beta citation (this is a C
// loop-bound bug in the engine's collision sweep, not a simulated value) --
// entity.c's `min_y = max(floorf(y1)-1, 0)` / `max_y = min(ceilf(y2)+1,
// WORLD_HEIGHT)` sites are the fix.
static void test_top_of_world_y127_collision(void) {
	set(20, 127, 20, 1 /*stone*/);

	struct entity e;
	memset(&e, 0, sizeof(e));
	e.on_server = true;
	e.world = &s.world;
	e.pos[0] = 20.5F;
	e.pos[1] = 127.0F;
	e.pos[2] = 20.5F;

	// A box squarely inside cell (20,127,20), with y2 (~127.98) high enough
	// that the OLD clamp (WORLD_HEIGHT-1 = 127) capped the sweep's max_y at
	// 127 and the exclusive `y < max_y` loop never visited y=127 at all.
	struct AABB probe;
	aabb_setsize_centered(&probe, 0.98F, 0.98F, 0.98F);
	aabb_translate(&probe, 20.5F, 127.0F + 0.49F, 20.5F);

	bool hit = entity_aabb_intersection(&e, &probe);
	printf("  y=127 top-of-world block detected: %s\n", hit ? "YES" : "NO");
	assert(hit
		   && "a block at the top world cell (y=127) must be collidable -- "
			  "the exclusive y-loop's clamp must be WORLD_HEIGHT, not "
			  "WORLD_HEIGHT-1");
}

// A ONE-BLOCK STEP-DOWN ledge with the feet EXACTLY on the platform face (b0931e6e lands
// bodies on the face: feet 71.0, so the probe's minY is exactly 70.0 = the top of the
// lower step). Beta's probe is World.getCollidingBoundingBoxes over
// boundingBox.getOffsetBoundingBox(var1, -1.0D, 0.0D) [Entity.java:279-289], each block
// admitted only on Block.getCollidingBoundingBoxes's STRICT intersectsWith
// [Block.java:259-263, AxisAlignedBB.java:185-187]: a box merely SHARING the lower step's
// top face is NOT ground, so the sneak clamp still stops the player at the platform lip.
// (Found closing the column_dedup pair: the walk owner, now exactly on its face, walked
// off such a step at walk tick 255 where the inclusive probe counted the touch.)
static void test_step_down_ledge_face_touch(void) {
	// Second platform, y=70, x in [20..23], z in [20..23]; the step below the +X gap:
	// x in [24..25] at y=69 (top face 70.0).
	for(int x = 20; x <= 23; x++)
		for(int z = 20; z <= 23; z++)
			set(x, PLAT_Y, z, 1 /*stone*/);
	for(int x = 24; x <= 25; x++)
		for(int z = 20; z <= 23; z++)
			set(x, PLAT_Y - 1, z, 1 /*stone*/);
	struct entity e;
	struct AABB bbox;
	make_player_bbox(&bbox);
	put_player(&e, 23.475F, 21.5F, 1.0F, 0.0F);
	struct AABB feet = bbox;
	aabb_translate(&feet, e.pos[0], e.pos[1], e.pos[2]);
	assert(feet.y1 == (float)(PLAT_Y + 1)); // exactly on the face -> probe minY == 70.0
	entity_sneak_ledge_clamp(&e, &bbox);
	// Beta: 1.0 -> 0.95 -> 0.90 -> 0.85 -> 0.80, where the offset box first overlaps the
	// platform column x=23 strictly (minX 23.975 < 24); every larger step only touches the
	// lower step's top face and is refused.
	const float min_x_after = e.pos[0] + e.vel[0] - 0.3F;
	assert(e.vel[0] > 0.75F && e.vel[0] < 0.85F);
	assert(min_x_after < 24.0F);
	assert(e.vel[2] == 0.0F);
	printf("  step-down ledge, feet on the face: vel 1.0 -> %.3f, box min x %.3f < 24 "
		   "(face touch with the lower step is not ground) OK\n",
		   e.vel[0], min_x_after);
}

int main(void) {
	test_fixture_dir("test_player_sneak_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	// A platform floating in clear air (y=70) so the ground probe sees ONLY it: x in
	// [6..11], z in [8..12]. Edges at x=12 (+X), x=5 (-X), z=13 (+Z), z=7 (-Z).
	for(int x = 6; x <= 11; x++)
		for(int z = 8; z <= 12; z++)
			set(x, PLAT_Y, z, 1 /*stone*/);

	printf("test_player_sneak_env:\n");
	test_edge_plus_x();
	test_edge_minus_z();
	test_open_ground_no_clamp();
	test_diagonal_into_corner_gap();
	test_top_of_world_y127_collision();
	test_step_down_ledge_face_touch();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_player_sneak_env\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
