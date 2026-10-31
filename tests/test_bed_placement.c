/*
	Bed placement faithfulness (item_bed.c), decompile 740c583,
	ItemBed.onItemUse:

	    face != 1 -> refused;                              // TOP face only
	    y goes up by one;
	    d = floor(rotationYaw*4.0F/360.0F + 0.5D) & 3;
	    // x/z step per d (BlockBed.headBlockToFootBlockMap):
	    //   0->(0,+1) +Z, 1->(-1,0) -X, 2->(0,-1) -Z, 3->(+1,0) +X.
	    if (isAirBlock(foot) && isAirBlock(foot+dir) &&
	        isBlockNormalCube(foot.y-1) && isBlockNormalCube((foot+dir).y-1)) {
	        set(foot, d); set(foot+dir, d+8);
	        return true;
	    }
	    return false;

	The pre-fix code ignored rotationYaw entirely: it tried a FIXED search
	order (+Z, -Z, +X, -X) and placed the head in whichever of those four
	cells happened to be free first -- so the bed's facing never followed
	the player, and a placement that Beta would refuse outright (the
	yaw-derived direction is blocked) could silently succeed facing a
	different way instead.

	This file pins: (1) all four cardinal yaws pick the Beta-derived
	direction (translated to this project's own historic metadata table --
	see below); (2) NO fallback search -- if the yaw-derived direction is
	obstructed, placement fails outright, even though another direction
	would have been free; (3) a missing solid floor under either half
	refuses placement; (4) a non-TOP click is refused.

	Metadata (PAR-BED-META, 2026-10-21): beds store Beta's OWN layout --
	direction = meta&3 = d itself (0 +Z, 1 -X, 2 -Z, 3 +X), 0x4 occupied,
	0x8 the half placed at foot+dir (source/block/bed_meta.h). The pre-fix
	code wrote a port-private index<<1 over {+Z,-Z,+X,-X}, so a real Beta
	save or server chunk decoded wrongly; expected metadata here is Beta's raw
	d / d+8.

	Also pinned here (same fixture): neighbour pop-off for every direction
	and the occupied variants (Beta BlockBed.onNeighborBlockChange:111-125 --
	the 0x8 half pops silently, the other half pops and drops the bed item),
	getDroppedItem's half split (idDropped:127-129), and the client render
	orientation helpers against RenderBlocks.renderBlockBed's top-face UVs.
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
#include "block/bed_meta.h"
#include "item/items.h"
#include "item/items_object.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_bed_placement_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y 101  // bed plane

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

/* Place a bed with the foot on top of the floor block at (x,YF,z), the
 * player facing `yaw` degrees (player.rx maps 1:1 onto rotationYaw). */
static bool place_bed(int x, int z, float yaw, enum side side) {
	s.player.rx = yaw;
	struct block_info on = info_at(x, YF, z);
	struct block_info where = {
		.block = NULL, .neighbours = NULL, .x = x, .y = Y, .z = z};
	struct item_data it = {.id = ITEM_BED, .durability = 0, .count = 1};
	return item_bed.onItemPlace(&s, &s.player, &it, &where, &on, side);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* progress lines survive an assert */
	test_fixture_dir("test_bed_placement_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_bed_placement: ItemBed.onItemUse yaw-facing, no fallback "
		   "search (fixes the fixed +Z,-Z,+X,-X search-order bug)\n");

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
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc); // x,z <= 15
	s.player.x = 1000.0;
	s.player.y = 1000.0;
	s.player.z = 1000.0;

	// Isolated stone floor covering every column used below.
	for(int x = 0; x <= 15; x++)
		for(int z = 0; z <= 15; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	// (1) Four cardinal yaws -> Beta's floor(yaw*4/360+0.5)&3, stored raw.
	struct {
		float yaw;
		int fx, fz;
		int hx, hz; // expected head cell
		int want_meta; // Beta var9 (ItemBed.onItemUse:14,34-35)
	} cases[] = {
		{0.0F, 2, 2, 2, 3, 0},   // +Z -> d 0
		{90.0F, 6, 2, 5, 2, 1},  // -X -> d 1
		{180.0F, 10, 4, 10, 3, 2}, // -Z -> d 2
		{270.0F, 14, 2, 15, 2, 3}, // +X -> d 3
		{-90.0F, 2, 6, 3, 6, 3},  // floor(-1+0.5)&3 == 3 -> +X
		{44.0F, 6, 6, 6, 7, 0},   // floor(0.489+0.5) == 0 -> +Z
		{46.0F, 12, 12, 11, 12, 1}, // floor(0.511+0.5) == 1 -> -X
	};
	for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		assert(place_bed(cases[i].fx, cases[i].fz, cases[i].yaw, SIDE_TOP));
		int foot_meta = getmeta(cases[i].fx, Y, cases[i].fz);
		int head_meta = getmeta(cases[i].hx, Y, cases[i].hz);
		if(foot_meta != cases[i].want_meta
		   || head_meta != (cases[i].want_meta | 8)) {
			fprintf(stderr,
					"test_bed_placement: yaw=%.0f foot_meta=%d head_meta=%d, "
					"want foot=%d head=%d\n",
					(double)cases[i].yaw, foot_meta, head_meta,
					cases[i].want_meta, cases[i].want_meta | 8);
			assert(0 && "bed yaw-facing diverges from Beta ItemBed.onItemUse");
		}
		assert(get(cases[i].fx, Y, cases[i].fz) == BLOCK_BED);
		assert(get(cases[i].hx, Y, cases[i].hz) == BLOCK_BED);
		printf("  yaw=%.0f -> foot meta=%d head meta=%d OK\n", (double)cases[i].yaw,
			   foot_meta, head_meta);
	}

	// (2) NO fallback search: the yaw-derived direction (+Z) is obstructed
	// by a pre-existing block, even though the other three directions
	// (-Z,+X,-X) around the same foot cell are wide open -- the old
	// fixed-search code would have placed the head there instead; Beta (and
	// this fix) must refuse outright.
	set(3, Y, 4, BLOCK_STONE, 0); // occupies the +Z head cell in advance
	assert(!place_bed(3, 3, 0.0F, SIDE_TOP));
	assert(get(3, Y, 3) != BLOCK_BED); // foot must NOT have been placed either
	printf("  obstructed yaw-direction with 3 free alternatives: refused, no "
		   "fallback OK\n");

	// (3) Missing solid support under the FOOT half refuses placement.
	// [decompile 740c583] isBlockNormalCube(foot.y-1, ...)
	set(8, YF, 8, BLOCK_AIR, 0); // remove support under the foot itself
	assert(!place_bed(8, 8, 0.0F, SIDE_TOP));
	assert(get(8, Y, 8) != BLOCK_BED);
	printf("  no floor under foot: refused OK\n");

	// (4) Missing solid support under the HEAD half refuses placement, even
	// though the foot's own floor is solid.
	set(9, YF, 9, BLOCK_AIR, 0); // remove support under where the +Z head lands
	assert(!place_bed(9, 8, 0.0F, SIDE_TOP));
	assert(get(9, Y, 8) != BLOCK_BED);
	assert(get(9, Y, 9) != BLOCK_BED);
	printf("  no floor under head: refused OK\n");

	// (5) Refusal: a non-TOP click never places a bed [decompile 740c583:
	// `if (var7 != 1) return false;`], independently confirmed by mc173's
	// `if face != Face::PosY { return false; }`.
	assert(!place_bed(11, 11, 0.0F, SIDE_FRONT));
	assert(get(11, Y, 11) != BLOCK_BED);
	printf("  non-TOP click refused OK\n");

	// (6) Neighbour pop-off, every Beta direction, plain and occupied.
	// [BlockBed.onNeighborBlockChange:111-125] dir = meta&3; the 0x8 half
	// checks (x,z) - map[dir] and pops SILENTLY; the other half checks
	// (x,z) + map[dir], pops, and drops the bed (dropBlockAsItem with its own
	// meta, which dropBlockAsItemWithChance:184-188 lets through because 0x8
	// is clear). Decoding with the old (meta>>1)&3 layout looks for the
	// partner in the wrong cell for directions 1/3 and the occupied variants,
	// popping a perfectly good bed the moment any neighbour changes.
	{
		static const int map[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
		const int PY = 120; // fresh plane (WORLD_HEIGHT 128), nothing else here
		for(int occ = 0; occ <= 1; occ++)
			for(int d = 0; d < 4; d++) {
				int fx = 2 + d * 3, fz = 4 + occ * 6;
				int foot_meta = d | (occ ? 4 : 0);
				int hx = fx + map[d][0], hz = fz + map[d][1];
				// (a) an intact pair survives an unrelated neighbour change
				set(fx, PY, fz, BLOCK_BED, foot_meta);
				set(hx, PY, hz, BLOCK_BED, foot_meta | 8);
				set(fx, PY + 1, fz, BLOCK_STONE, 0); // notifies the foot
				set(hx, PY + 1, hz, BLOCK_STONE, 0); // notifies the head
				assert(get(fx, PY, fz) == BLOCK_BED && getmeta(fx, PY, fz) == foot_meta
					   && "intact Beta-meta foot must survive a neighbour update");
				assert(get(hx, PY, hz) == BLOCK_BED
					   && getmeta(hx, PY, hz) == (foot_meta | 8)
					   && "intact Beta-meta head must survive a neighbour update");
				set(fx, PY + 1, fz, BLOCK_AIR, 0);
				set(hx, PY + 1, hz, BLOCK_AIR, 0);
				// (b) remove the 0x8 half -> the other half pops AND drops one bed
				size_t before = dict_entity_size(s.entities);
				set(hx, PY, hz, BLOCK_AIR, 0);
				assert(get(fx, PY, fz) == BLOCK_AIR
					   && "foot must pop when its 0x8 half disappears");
				assert(dict_entity_size(s.entities) == before + 1
					   && "the half WITHOUT 0x8 drops exactly one bed (Beta)");
				// (c) remove the non-0x8 half -> the 0x8 half pops silently
				set(fx, PY, fz, BLOCK_BED, foot_meta);
				set(hx, PY, hz, BLOCK_BED, foot_meta | 8);
				before = dict_entity_size(s.entities);
				set(fx, PY, fz, BLOCK_AIR, 0);
				assert(get(hx, PY, hz) == BLOCK_AIR
					   && "0x8 half must pop when its foot disappears");
				assert(dict_entity_size(s.entities) == before
					   && "the 0x8 half pops SILENTLY (Beta idDropped == 0)");
			}
		printf("  neighbour pop-off: 4 directions x {free, occupied} OK\n");
	}

	// (7) getDroppedItem's half split [BlockBed.idDropped:127-129]:
	// isBlockFootOfBed(meta) = meta&8 -> 0, otherwise Item.bed.
	for(int m = 0; m < 16; m++) {
		struct block_data b = {.type = BLOCK_BED, .metadata = (uint8_t)m};
		struct block_info bi = {.block = &b, .x = 0, .y = 0, .z = 0};
		struct item_data it = {0};
		size_t n = blocks[BLOCK_BED]->getDroppedItem(&bi, &it, NULL, &s);
		assert(n == ((m & 8) ? 0u : 1u) && "only the non-0x8 half drops");
		if(n)
			assert(it.id == ITEM_BED && it.count == 1);
	}
	printf("  getDroppedItem: non-0x8 half drops, 0x8 half never OK\n");

	// (8) Render orientation vs RenderBlocks.renderBlockBed (RetroMCP
	// RenderBlocks.java:142-171). Beta's top quad, per var7 = meta&3, gives
	// each corner (minX/maxX, minZ/maxZ) a texture u; the bed-top tiles run
	// foot -> head along +u in terrain.png, so the world direction in which u
	// grows is where Beta draws the pillow. Rebuild it from Beta's literal UV
	// table, then from CaveWii's SIDE_TOP corner order (render_block.c
	// render_block_side_adv_v2: (0,0)->c[r], (1,0)->c[r+1], (1,1)->c[r+2],
	// (0,1)->c[r+3], c = {u0v0, u1v0, u1v1, u0v1}) with
	// r = bed_meta_top_rotation, and require both to equal map[meta&3].
	{
		static const int map[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
		for(int m = 0; m < 16; m++) {
			int d = m & 3;
			// Beta var38/var40/var46/var48 (u per corner): 0 = u0, 1 = u1.
			int v38 = 0, v40 = 1, v46 = 0, v48 = 1;
			if(d == 0) {
				v40 = 0;
				v46 = 1;
			} else if(d == 2) {
				v38 = 1;
				v48 = 0;
			} else if(d == 3) {
				v38 = 1;
				v48 = 0;
				v40 = 0;
				v46 = 1;
			}
			// corners: (maxX,maxZ)=v46 (maxX,minZ)=v38 (minX,minZ)=v40 (minX,maxZ)=v48
			int bdx = (v46 + v38) - (v40 + v48);
			int bdz = (v46 + v48) - (v38 + v40);
			int bx = bdx > 0 ? 1 : bdx < 0 ? -1 : 0;
			int bz = bdz > 0 ? 1 : bdz < 0 ? -1 : 0;
			assert(bx == map[d][0] && bz == map[d][1]
				   && "Beta's own top UVs put +u along headBlockToFootBlockMap");
			static const int cu[4] = {0, 1, 1, 0}; // u of c[0..3]
			int r = bed_meta_top_rotation((uint8_t)m);
			int u00 = cu[(r + 0) % 4], u10 = cu[(r + 1) % 4];
			int u11 = cu[(r + 2) % 4], u01 = cu[(r + 3) % 4];
			int cdx = (u10 + u11) - (u00 + u01);
			int cdz = (u11 + u01) - (u00 + u10);
			int cx = cdx > 0 ? 1 : cdx < 0 ? -1 : 0;
			int cz = cdz > 0 ? 1 : cdz < 0 ? -1 : 0;
			assert(cx == map[d][0] && cz == map[d][1]
				   && "CaveWii's bed top must point the pillow where Beta does");
			assert(bed_meta_dx((uint8_t)m) == map[d][0]
				   && bed_meta_dz((uint8_t)m) == map[d][1]);
			// The end textures (headboard / footboard) sit on the faces
			// across the bed's axis: X faces for directions 1/3.
			assert(bed_meta_ends_on_x_axis((uint8_t)m) == (map[d][0] != 0));
		}
		printf("  render: top pillow + end-face axis match renderBlockBed for all 16 metas OK\n");
	}

	// (9) Save version 5 legacy conversion table (bed_meta_from_legacy):
	// old index<<1 over {+Z,-Z,+X,-X} -> Beta's direction, head bit kept.
	{
		static const struct {
			uint8_t legacy, beta;
		} conv[] = {
			{0, 0}, {2, 2}, {4, 3}, {6, 1}, {8, 8}, {10, 10}, {12, 11}, {14, 9},
		};
		for(size_t i = 0; i < sizeof conv / sizeof conv[0]; i++)
			assert(bed_meta_from_legacy(conv[i].legacy) == conv[i].beta);
		printf("  legacy 0/2/4/6 (+8) -> Beta 0/2/3/1 (+8) OK\n");
	}

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_bed_placement: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
