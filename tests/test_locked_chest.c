/*
	FAITHFULNESS test (Beta 1.7.3): BlockLockedChest at id 95 (FH-07).

	[740c583 Block.java:687]
	  lockedChest = a BlockLockedChest(95) with hardness 0.0F,
	    light value 1.0F, the wood step sound,
	    block name "lockedchest", tick on load,
	    and no neighbour notify on a metadata change.
	[740c583 BlockLockedChest.java] whole class: chest-look textures with
	  neighbour-opacity auto-facing, canPlaceBlockAt always true, updateTick
	  unconditionally sets the block to air, NO onBlockActivated (never a
	  container, never openable), NO idDropped/quantityDropped overrides.

	Proven here:
	  1) Registry: id 95 is the locked chest; the NET-NEW iron chest lives at
	     200 with its full feature set (container row intact) -- the FH-07
	     relocation, both sides.
	  2) Identity: luminance 15 (setLightValue(1.0F)), wood material, opaque
	     full cube, right-click is a no-op (no onRightClick), default drop
	     (its own id, qty 1 -- Block's defaults, no overrides).
	  3) updateTick faithfully self-despawns: onRandomTick on a live superflat
	     server world turns the placed 95 into air.
	  4) Auto-facing transcription: with an opaque cube on exactly one
	     horizontal side, the FRONT texture lands on the OPPOSITE side (Beta's
	     facing heuristic); with no opaque neighbours the front defaults to +Z
	     (facing 3). Compared via tex_atlas_lookup indices, top==bottom.
*/

#include <assert.h>
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
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_locked_chest_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 101

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

int main(void) {
	test_fixture_dir("test_locked_chest_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();

	// --- 1) Registry: the FH-07 relocation, both sides ---------------------
	assert(BLOCK_LOCKED_CHEST == 95);
	assert(BLOCK_IRON_CHEST == 200);
	assert(blocks[BLOCK_LOCKED_CHEST] == &block_locked_chest);
	assert(blocks[BLOCK_IRON_CHEST] == &block_iron_chest);

	// --- 2) Identity ------------------------------------------------------
	const struct block* lc = &block_locked_chest;
	assert(lc->luminance == 15);	 // setLightValue(1.0F)
	assert(lc->onRightClick == NULL); // never a container, never openable
	assert(lc->getDroppedItem == block_drop_default); // Block's defaults
	assert(!lc->transparent && !lc->can_see_through); // normal opaque cube
	assert(!lc->flammable); // absent from BlockFire's burn table
	struct block_info self = {0};
	assert(lc->getMaterial((struct block_info*)&self) == MATERIAL_WOOD);
	struct AABB box;
	assert(lc->getBoundingBox(&self, false, &box) == 1);
	assert(box.x2 - box.x1 == 1.0F && box.y2 - box.y1 == 1.0F
		   && box.z2 - box.z1 == 1.0F);
	// Default drop: its own id, quantity 1.
	struct block_data bd = {.type = BLOCK_LOCKED_CHEST, .metadata = 0};
	struct block_info info = {.block = &bd, .x = 2, .y = Y, .z = 2};
	struct item_data drop = {0};
	struct java_random g; /* F4-RNG: getDroppedItem takes World.rand */
	jrand_set_seed(&g, 0x10C4ED);
	assert(lc->getDroppedItem(&info, &drop, &g, NULL) == 1);
	assert(drop.id == BLOCK_LOCKED_CHEST && drop.count == 1);

	// --- 4) Auto-facing transcription (no world needed) --------------------
	uint8_t tex_top_i = tex_atlas_lookup(TEXAT_CHEST_TOP);
	uint8_t tex_side_i = tex_atlas_lookup(TEXAT_CHEST_SIDE);
	uint8_t tex_front_i = tex_atlas_lookup(TEXAT_CHEST_FRONT_SINGLE);
	struct block_data nb[SIDE_MAX];
	memset(nb, 0, sizeof(nb)); // all air
	struct block_info faced = {.block = &bd, .neighbours = nb};
	assert(lc->getTextureIndex(&faced, SIDE_TOP) == tex_top_i);
	assert(lc->getTextureIndex(&faced, SIDE_BOTTOM) == tex_top_i);
	// No opaque neighbours: var10 defaults to 3 -> front on +Z (SIDE_BACK).
	assert(lc->getTextureIndex(&faced, SIDE_BACK) == tex_front_i);
	assert(lc->getTextureIndex(&faced, SIDE_FRONT) == tex_side_i);
	// Opaque cube on -Z only (var6): front flips to +Z -- still SIDE_BACK
	// (Beta's first branch is the redundant no-op), sides stay sides.
	nb[SIDE_FRONT].type = BLOCK_STONE;
	assert(lc->getTextureIndex(&faced, SIDE_BACK) == tex_front_i);
	// Opaque cube on +Z only (var7): front flips to -Z (SIDE_FRONT).
	nb[SIDE_FRONT].type = 0;
	nb[SIDE_BACK].type = BLOCK_STONE;
	assert(lc->getTextureIndex(&faced, SIDE_FRONT) == tex_front_i);
	assert(lc->getTextureIndex(&faced, SIDE_BACK) == tex_side_i);
	// Opaque cube on -X only (var8): front on +X (SIDE_RIGHT).
	nb[SIDE_BACK].type = 0;
	nb[SIDE_LEFT].type = BLOCK_STONE;
	assert(lc->getTextureIndex(&faced, SIDE_RIGHT) == tex_front_i);
	// Opaque cube on +X only (var9): front on -X (SIDE_LEFT).
	nb[SIDE_LEFT].type = 0;
	nb[SIDE_RIGHT].type = BLOCK_STONE;
	assert(lc->getTextureIndex(&faced, SIDE_LEFT) == tex_front_i);
	printf("[registry+identity+facing] OK\n");

	// --- 3) updateTick self-despawn on a live server world -----------------
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
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
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	set(2, Y, 2, BLOCK_LOCKED_CHEST, 0);
	assert(get(2, Y, 2) == BLOCK_LOCKED_CHEST);
	struct block_data placed = {.type = BLOCK_LOCKED_CHEST, .metadata = 0};
	struct block_info tick_info = {.block = &placed, .x = 2, .y = Y, .z = 2};
	// [740c583 BlockLockedChest.updateTick] setBlockWithNotify(x,y,z,0),
	// unconditionally, on the very first random tick it receives.
	blocks[BLOCK_LOCKED_CHEST]->onRandomTick(&s, &tick_info);
	assert(get(2, Y, 2) == BLOCK_AIR);
	printf("[updateTick self-despawn] OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_locked_chest: ALL PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
