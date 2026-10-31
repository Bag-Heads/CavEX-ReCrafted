/*
	Door placement faithfulness (item_door_wood.c / item_door_iron.c),
	decompile 740c583, ItemDoor.onItemUse:

	    face != 1 -> refused;                             // TOP face only
	    y goes up by one;
	    d = floor((rotationYaw + 180.0F) * 4.0F / 360.0F - 0.5D) & 3;
	    // a unit step for d; a solid-neighbour
	    // count for each side (A, B); whether a same-type door is already on
	    // each side; flip = (door on A && none on B) || (count B > count A).
	    on flip, d = ((d - 1) & 3) + 4;
	    lower half (x, y, z): the door id with metadata d;
	    upper half (x, y + 1, z): metadata d + 8.

	The pre-fix code ignored rotationYaw entirely and derived a metadata
	from the clicked SIDE (a fixed switch over on_side), defaulting to 6 --
	which packs BOTH a facing (2) AND the OPEN bit (0x04) into every door
	placed on an unmatched side (i.e. almost all of them), regardless of
	player facing. This file pins:
	  (1) the four cardinal yaws each produce their own Beta-derived
	      direction, with NO open bit (0x04) when nothing forces a flip;
	  (2) the flip-by-adjacent-door case (mirrors a same-type neighbour) --
	      independently confirmed against mc173's use_door_stack, which ALSO
	      sets the open bit on the flip branch (block::door::set_open), so a
	      hinge-flipped door legitimately places with 0x04 set -- this is
	      Beta's real behaviour, not a stray "open on placement" bug;
	  (3) the flip-by-solid-neighbour-count case (swings into the wall);
	  (4) a non-TOP click is refused outright (face != 1).
	Iron door reuses the identical algorithm (item_door_iron.c); a single
	cardinal-yaw + flip-by-door check covers its own onItemPlace body.
*/

#include <assert.h>
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
#include "item/items.h"
#include "item/items_object.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_door_placement_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y 101  // door plane

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

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.metadata : -1;
}
static struct block_data g_bd;
static struct block_info info_at(int x, int y, int z) {
	assert(server_world_get_block(&s.world, x, y, z, &g_bd));
	return (struct block_info) {
		.block = &g_bd, .neighbours = NULL, .x = x, .y = y, .z = z};
}

/* Place a wood door on top of the floor block at (x,YF,z), with the player
 * facing `yaw` degrees (player.rx maps 1:1 onto Beta's rotationYaw). */
static bool place_wood_door(int x, int z, float yaw) {
	s.player.rx = yaw;
	struct block_info on = info_at(x, YF, z);
	struct block_info where = {
		.block = NULL, .neighbours = NULL, .x = x, .y = Y, .z = z};
	struct item_data it = {.id = ITEM_DOOR_WOOD, .durability = 0, .count = 1};
	return item_door_wood.onItemPlace(&s, &s.player, &it, &where, &on, SIDE_TOP);
}

static bool place_iron_door(int x, int z, float yaw) {
	s.player.rx = yaw;
	struct block_info on = info_at(x, YF, z);
	struct block_info where = {
		.block = NULL, .neighbours = NULL, .x = x, .y = Y, .z = z};
	struct item_data it = {.id = ITEM_DOOR_IRON, .durability = 0, .count = 1};
	return item_door_iron.onItemPlace(&s, &s.player, &it, &where, &on, SIDE_TOP);
}

int main(void) {
	test_fixture_dir("test_door_placement_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_door_placement: ItemDoor.onItemUse yaw-facing + hinge-flip "
		   "(fixes the on_side-switch bug that always packed the OPEN bit)\n");

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

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
	// Test coordinates range up to x=50 (chunk 3); CHUNK_SIZE==16, so load a
	// generous 0..3 chunk grid to cover every placement site below.
	for(int cz = 0; cz <= 3; cz++)
		for(int cx = 0; cx <= 3; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}
	// Keep the player far from the placement site -- onItemPlace only reads
	// s.player.rx here, but block_place_obstructed reads position too.
	s.player.x = 1000.0;
	s.player.y = 1000.0;
	s.player.z = 1000.0;

	// Isolated stone floor patch, wide open on all sides -- every door
	// placed here has EQUAL (zero) solid neighbours and no adjacent door,
	// so flip is always false and metadata == direction exactly, no bit
	// 0x04. This isolates the yaw->direction formula from the hinge logic.
	for(int x = 0; x <= 55; x++)
		for(int z = 0; z <= 20; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	// (1) Four cardinal yaws -> Beta's floor((yaw+180)*4/360-0.5)&3.
	struct { float yaw; int want_dir; } cases[] = {
		{0.0F, 1}, {90.0F, 2}, {180.0F, 3}, {270.0F, 0},
	};
	for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int x = 2 + (int)i * 3, z = 2;
		assert(place_wood_door(x, z, cases[i].yaw));
		int bottom = getmeta(x, Y, z);
		int top = getmeta(x, Y + 1, z);
		if(bottom != cases[i].want_dir || top != (cases[i].want_dir | 8)) {
			fprintf(stderr,
					"test_door_placement: yaw=%.0f bottom=%d top=%d, want "
					"bottom=%d top=%d\n",
					(double)cases[i].yaw, bottom, top, cases[i].want_dir,
					cases[i].want_dir | 8);
			assert(0 && "door yaw-facing diverges from Beta ItemDoor.onItemUse");
		}
		assert((bottom & 4) == 0 && "no flip forced -- OPEN bit must be clear");
		assert(get(x, Y, z) == BLOCK_DOOR_WOOD);
		assert(get(x, Y + 1, z) == BLOCK_DOOR_WOOD);
		printf("  yaw=%.0f -> bottom meta=%d top meta=%d OK\n",
			   (double)cases[i].yaw, bottom, top);
	}

	// (2) Flip by adjacent same-type door (mirrors a double-door set).
	// Door A at (30,Y,10), yaw=0 -> direction=1 (dx=-1,dz=0), occupies
	// x=30. Door B placed at (29,Y,10) with the SAME yaw: its "back"
	// neighbour (where.x - dx = 30) is door A -> must flip.
	assert(place_wood_door(30, 10, 0.0F));
	assert(getmeta(30, Y, 10) == 1); // direction 1, no flip (isolated)
	assert(place_wood_door(29, 10, 0.0F));
	int flipped = getmeta(29, Y, 10);
	// [decompile 740c583] var9 = (1-1)&3 + 4 = 4 -- direction rotates to 0
	// AND bit 0x04 (OPEN) is set. Independently confirmed by mc173's
	// use_door_stack: `if flip { set_open(&mut metadata, true); ... }` in
	// the very same branch -- a hinge-flipped door IS placed open in Beta.
	if(flipped != 4) {
		fprintf(stderr,
				"test_door_placement: door-mirror flip meta=%d, want 4 "
				"(direction 0 | OPEN)\n",
				flipped);
		assert(0 && "adjacent-door hinge mirroring diverges from Beta");
	}
	printf("  door-mirror flip: neighbour meta=%d (dir 0 | OPEN) OK\n",
		   flipped);

	// (3) Flip by solid-neighbour count (swings into the wall, no adjacent
	// door). direction=1 (yaw=0) => dx=-1,dz=0; "front" cells are at
	// (x-1,y) and (x-1,y+1); walling those off (both solid) while the
	// "back" side (x+1) stays open air must force the flip.
	set(49, Y, 15, BLOCK_STONE, 0);
	set(49, Y + 1, 15, BLOCK_STONE, 0);
	assert(place_wood_door(50, 15, 0.0F));
	int wall_flip = getmeta(50, Y, 15);
	if(wall_flip != 4) {
		fprintf(stderr,
				"test_door_placement: solid-neighbour flip meta=%d, want 4\n",
				wall_flip);
		assert(0 && "solid-neighbour hinge flip diverges from Beta");
	}
	printf("  solid-neighbour flip: meta=%d (dir 0 | OPEN) OK\n", wall_flip);

	// (4) Refusal: a non-TOP click never places a door [decompile 740c583:
	// `if (var7 != 1) return false;`], independently confirmed by mc173's
	// `if face != Face::PosY { return false; }`.
	{
		struct block_info on = info_at(2, YF, 2); // already has door A's floor
		struct block_info where = {
			.block = NULL, .neighbours = NULL, .x = 3, .y = Y, .z = 3};
		struct item_data it
			= {.id = ITEM_DOOR_WOOD, .durability = 0, .count = 1};
		assert(!item_door_wood.onItemPlace(&s, &s.player, &it, &where, &on, SIDE_FRONT));
		assert(get(3, Y, 3) != BLOCK_DOOR_WOOD);
	}
	printf("  non-TOP click refused OK\n");

	// Iron door: identical algorithm, one cardinal yaw + one mirrored flip
	// to confirm item_door_iron.c's independent port matches.
	assert(place_iron_door(2, 18, 0.0F));
	assert(getmeta(2, Y, 18) == 1);
	assert(get(2, Y, 18) == BLOCK_DOOR_IRON);
	assert(place_iron_door(1, 18, 0.0F));
	assert(getmeta(1, Y, 18) == 4);
	printf("  iron door: yaw=0 meta=1, mirrored neighbour meta=4 OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_door_placement: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
