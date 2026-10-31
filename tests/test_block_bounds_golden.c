/* Beta 1.7.3 bounds, hand-derived from the persistent mcb173-retromcp client
 * decompile. Each table group cites its class/method and source lines. Coordinates
 * are block-local, as in Beta. Doors belong to test_door_bounds_golden.
 * Stairs use a different cuboid partition here: compare the exact occupied union.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "block/blocks.h"

struct shape {
	size_t count;
	struct AABB boxes[2];
};
struct golden {
	uint8_t type, first_meta, last_meta, neighbours;
	struct shape selection, collision;
};
#define BOX(a,b,c,d,e,f) {a,b,c,d,e,f}
#define FULL BOX(0,0,0,1,1,1)
#define ONE(...) {1,{__VA_ARGS__}}
#define TWO(...) {2,{__VA_ARGS__}}
#define NONE {0,{{0,0,0,0,0,0}}}
#define ROW(...) {__VA_ARGS__}
/* Neighbour bits: -X,+X,-Z,+Z. Portal orientation ignores metadata. */
static const struct golden goldens[] = {
	/* BlockTrapDoor.setBlockBoundsForBlockRender:47-65, collision:33-35. */
	ROW(BLOCK_TRAP_DOOR,0,3,0,ONE(BOX(0,0,0,1,.1875F,1)),ONE(BOX(0,0,0,1,.1875F,1))),
	ROW(BLOCK_TRAP_DOOR,4,4,0,ONE(BOX(0,0,.8125F,1,1,1)),ONE(BOX(0,0,.8125F,1,1,1))),
	ROW(BLOCK_TRAP_DOOR,5,5,0,ONE(BOX(0,0,0,1,1,.1875F)),ONE(BOX(0,0,0,1,1,.1875F))),
	ROW(BLOCK_TRAP_DOOR,6,6,0,ONE(BOX(.8125F,0,0,1,1,1)),ONE(BOX(.8125F,0,0,1,1,1))),
	ROW(BLOCK_TRAP_DOOR,7,7,0,ONE(BOX(0,0,0,.1875F,1,1)),ONE(BOX(0,0,0,.1875F,1,1))),
	/* BlockBed.setBounds:131-132; inherited Block.getCollisionBoundingBoxFromPool:267-268. */
	ROW(BLOCK_BED,0,15,0,ONE(BOX(0,0,0,1,.5625F,1)),ONE(BOX(0,0,0,1,.5625F,1))),
	/* BlockStairs.setBlockBoundsBasedOnState:18-19; getCollidingBoundingBoxes:42-66.
	 * Both materials share these four exact unions. */
#define STAIRS(id) \
	ROW(id,0,0,0,ONE(FULL),TWO(BOX(0,0,0,.5F,.5F,1),BOX(.5F,0,0,1,1,1))), \
	ROW(id,1,1,0,ONE(FULL),TWO(BOX(0,0,0,.5F,1,1),BOX(.5F,0,0,1,.5F,1))), \
	ROW(id,2,2,0,ONE(FULL),TWO(BOX(0,0,0,1,.5F,.5F),BOX(0,0,.5F,1,1,1))), \
	ROW(id,3,3,0,ONE(FULL),TWO(BOX(0,0,0,1,1,.5F),BOX(0,0,.5F,1,.5F,1)))
	STAIRS(BLOCK_WOODEN_STAIRS),
	STAIRS(BLOCK_STONE_STAIRS),
	/* BlockStep.<init>:6-14 (double slab retains Block's full bounds). */
	ROW(BLOCK_SLAB,0,3,0,ONE(BOX(0,0,0,1,.5F,1)),ONE(BOX(0,0,0,1,.5F,1))),
	ROW(BLOCK_DOUBLE_SLAB,0,3,0,ONE(FULL),ONE(FULL)),
	/* BlockFence.getCollisionBoundingBoxFromPool:12-13; inherited full selection. */
	ROW(BLOCK_FENCE,0,15,0,ONE(FULL),ONE(BOX(0,0,0,1,1.5F,1))),
	/* BlockSign.<init>:14-16; setBlockBoundsBasedOnState:28-50; collision:19-20. */
	ROW(BLOCK_SIGN_POST,0,15,0,ONE(BOX(.25F,0,.25F,.75F,1,.75F)),NONE),
	ROW(BLOCK_SIGN,2,2,0,ONE(BOX(0,.28125F,.875F,1,.78125F,1)),NONE),
	ROW(BLOCK_SIGN,3,3,0,ONE(BOX(0,.28125F,0,1,.78125F,.125F)),NONE),
	ROW(BLOCK_SIGN,4,4,0,ONE(BOX(.875F,.28125F,0,1,.78125F,1)),NONE),
	ROW(BLOCK_SIGN,5,5,0,ONE(BOX(0,.28125F,0,.125F,.78125F,1)),NONE),
	/* BlockLadder.getCollisionBoundingBoxFromPool:10-29; selection:32-51. */
	ROW(BLOCK_LADDER,2,2,0,ONE(BOX(0,0,.875F,1,1,1)),ONE(BOX(0,0,.875F,1,1,1))),
	ROW(BLOCK_LADDER,3,3,0,ONE(BOX(0,0,0,1,1,.125F)),ONE(BOX(0,0,0,1,1,.125F))),
	ROW(BLOCK_LADDER,4,4,0,ONE(BOX(.875F,0,0,1,1,1)),ONE(BOX(.875F,0,0,1,1,1))),
	ROW(BLOCK_LADDER,5,5,0,ONE(BOX(0,0,0,.125F,1,1)),ONE(BOX(0,0,0,.125F,1,1))),
	/* BlockButton.setBlockBoundsBasedOnState:96-115, pressed bit changes depth only. */
#define BUTTON(m,d) \
	ROW(BLOCK_STONE_BUTTON,1+m,1+m,0,ONE(BOX(0,.375F,.3125F,d,.625F,.6875F)),NONE), \
	ROW(BLOCK_STONE_BUTTON,2+m,2+m,0,ONE(BOX(1-d,.375F,.3125F,1,.625F,.6875F)),NONE), \
	ROW(BLOCK_STONE_BUTTON,3+m,3+m,0,ONE(BOX(.3125F,.375F,0,.6875F,.625F,d)),NONE), \
	ROW(BLOCK_STONE_BUTTON,4+m,4+m,0,ONE(BOX(.3125F,.375F,1-d,.6875F,.625F,1)),NONE)
	BUTTON(0,.125F), BUTTON(8,.0625F),
	/* BlockLever.setBlockBoundsBasedOnState:111-124; collision:8-9. */
#define LEVER(m) \
	ROW(BLOCK_LEVER,1+m,1+m,0,ONE(BOX(0,.2F,.3125F,.375F,.8F,.6875F)),NONE), \
	ROW(BLOCK_LEVER,2+m,2+m,0,ONE(BOX(.625F,.2F,.3125F,1,.8F,.6875F)),NONE), \
	ROW(BLOCK_LEVER,3+m,3+m,0,ONE(BOX(.3125F,.2F,0,.6875F,.8F,.375F)),NONE), \
	ROW(BLOCK_LEVER,4+m,4+m,0,ONE(BOX(.3125F,.2F,.625F,.6875F,.8F,1)),NONE), \
	ROW(BLOCK_LEVER,5+m,7+m,0,ONE(BOX(.25F,0,.25F,.75F,.6F,.75F)),NONE), \
	ROW(BLOCK_LEVER,0+m,0+m,0,ONE(BOX(.25F,0,.25F,.75F,.6F,.75F)),NONE)
	LEVER(0), LEVER(8),
	/* BlockTorch.collisionRayTrace:126-139 (meta & 7); collision:11-12.
	 * BlockRedstoneTorch inherits these methods for both lit states. */
#define TORCH(id,m) \
	ROW(id,1+m,1+m,0,ONE(BOX(0,.2F,.35F,.3F,.8F,.65F)),NONE), \
	ROW(id,2+m,2+m,0,ONE(BOX(.7F,.2F,.35F,1,.8F,.65F)),NONE), \
	ROW(id,3+m,3+m,0,ONE(BOX(.35F,.2F,0,.65F,.8F,.3F)),NONE), \
	ROW(id,4+m,4+m,0,ONE(BOX(.35F,.2F,.7F,.65F,.8F,1)),NONE), \
	ROW(id,5+m,7+m,0,ONE(BOX(.4F,0,.4F,.6F,.6F,.6F)),NONE), \
	ROW(id,0+m,0+m,0,ONE(BOX(.4F,0,.4F,.6F,.6F,.6F)),NONE)
	TORCH(BLOCK_TORCH,0), TORCH(BLOCK_TORCH,8),
	TORCH(BLOCK_REDSTONE_TORCH,0), TORCH(BLOCK_REDSTONE_TORCH,8),
	TORCH(BLOCK_REDSTONE_TORCH_LIT,0), TORCH(BLOCK_REDSTONE_TORCH_LIT,8),
	/* BlockRail.setBlockBoundsBasedOnState:40-45: RAW metadata, even powered rails.
	 * Plain rail includes all curves 6..9; powered/detector includes bit 8. */
#define RAIL(id) \
	ROW(id,0,1,0,ONE(BOX(0,0,0,1,.125F,1)),NONE), \
	ROW(id,2,5,0,ONE(BOX(0,0,0,1,.625F,1)),NONE), \
	ROW(id,6,15,0,ONE(BOX(0,0,0,1,.125F,1)),NONE)
	RAIL(BLOCK_RAIL), RAIL(BLOCK_POWERED_RAIL), RAIL(BLOCK_DETECTOR_RAIL),
	/* BlockPressurePlate.setBlockBoundsBasedOnState:122-128: meta == 1 ONLY. */
#define PLATE(id) \
	ROW(id,0,0,0,ONE(BOX(.0625F,0,.0625F,.9375F,.0625F,.9375F)),NONE), \
	ROW(id,1,1,0,ONE(BOX(.0625F,0,.0625F,.9375F,.03125F,.9375F)),NONE), \
	ROW(id,2,15,0,ONE(BOX(.0625F,0,.0625F,.9375F,.0625F,.9375F)),NONE)
	PLATE(BLOCK_STONE_PRESSURE_PLATE), PLATE(BLOCK_WOOD_PRESSURE_PLATE),
	/* BlockCake.setBlockBoundsBasedOnState:11-16; getCollisionBoundingBoxFromPool:25-30. */
#define CAKE(m) ROW(BLOCK_CAKE,m,m,0, \
	ONE(BOX((1+2*m)/16.0F,0,.0625F,.9375F,.5F,.9375F)), \
	ONE(BOX((1+2*m)/16.0F,0,.0625F,.9375F,.4375F,.9375F)))
	CAKE(0), CAKE(1), CAKE(2), CAKE(3), CAKE(4), CAKE(5), CAKE(6),
	/* BlockCactus.getCollisionBoundingBoxFromPool:30-32, selected bounds:35-37. */
	ROW(BLOCK_CACTUS,0,15,0,ONE(BOX(.0625F,0,.0625F,.9375F,1,.9375F)),ONE(BOX(.0625F,0,.0625F,.9375F,.9375F,.9375F))),
	/* BlockSnow.setBlockBoundsBasedOnState:25-28; collision:12-14 (meta & 7). */
#define SNOW(m,b) ROW(BLOCK_SNOW,m,m,0,ONE(BOX(0,0,0,1,((m&7)+1)/8.0F,1)),b)
	SNOW(0,NONE), SNOW(1,NONE), SNOW(2,NONE),
	SNOW(3,ONE(BOX(0,0,0,1,.5F,1))), SNOW(4,ONE(BOX(0,0,0,1,.5F,1))),
	SNOW(5,ONE(BOX(0,0,0,1,.5F,1))), SNOW(6,ONE(BOX(0,0,0,1,.5F,1))),
	SNOW(7,ONE(BOX(0,0,0,1,.5F,1))),
	/* BlockFarmland.<init>:10 and getCollisionBoundingBoxFromPool:14-15: FULL cube. */
	ROW(BLOCK_FARMLAND,0,7,0,ONE(BOX(0,0,0,1,.9375F,1)),ONE(FULL)),
	/* BlockSoulSand.getCollisionBoundingBoxFromPool:8-10; inherited full selection. */
	ROW(BLOCK_SOULSAND,0,15,0,ONE(FULL),ONE(BOX(0,0,0,1,.875F,1))),
	/* BlockPistonBase.setBlockBoundsBasedOnState:145-168;
	 * getCollidingBoundingBoxes:177-179 always restores full collision. */
#define PISTON(m,b) \
	ROW(BLOCK_PISTON_BASE,m,m,0,ONE(b),ONE(FULL)), \
	ROW(BLOCK_PISTON_STICKY,m,m,0,ONE(b),ONE(FULL))
	ROW(BLOCK_PISTON_BASE,0,5,0,ONE(FULL),ONE(FULL)),
	ROW(BLOCK_PISTON_STICKY,0,5,0,ONE(FULL),ONE(FULL)),
	PISTON(8,BOX(0,.25F,0,1,1,1)), PISTON(9,BOX(0,0,0,1,.75F,1)),
	PISTON(10,BOX(0,0,.25F,1,1,1)), PISTON(11,BOX(0,0,0,1,1,.75F)),
	PISTON(12,BOX(.25F,0,0,1,1,1)), PISTON(13,BOX(0,0,0,.75F,1,1)),
	/* BlockPistonExtension.setBlockBoundsBasedOnState:113-132;
	 * getCollidingBoundingBoxes:69-106. Pin the asymmetric Beta rods verbatim. */
#define HEAD(m,p,r) \
	ROW(BLOCK_PISTON_HEAD,m,m,0,ONE(p),TWO(p,r)), \
	ROW(BLOCK_PISTON_HEAD,m+8,m+8,0,ONE(p),TWO(p,r))
	HEAD(0,BOX(0,0,0,1,.25F,1),BOX(.375F,.25F,.375F,.625F,1,.625F)),
	HEAD(1,BOX(0,.75F,0,1,1,1),BOX(.375F,0,.375F,.625F,.75F,.625F)),
	HEAD(2,BOX(0,0,0,1,1,.25F),BOX(.25F,.375F,.25F,.75F,.625F,1)),
	HEAD(3,BOX(0,0,.75F,1,1,1),BOX(.25F,.375F,0,.75F,.625F,.75F)),
	HEAD(4,BOX(0,0,0,.25F,1,1),BOX(.375F,.25F,.25F,.625F,.75F,1)),
	HEAD(5,BOX(.75F,0,0,1,1,1),BOX(0,.375F,.25F,.75F,.625F,.75F)),
	/* BlockRedstoneRepeater.<init>:13; inherited Block.getCollisionBoundingBoxFromPool:267-268.
	 * Material.circuits DOES NOT imply a null collision box. */
	ROW(BLOCK_REDSTONE_REPEATER,0,15,0,ONE(BOX(0,0,0,1,.125F,1)),ONE(BOX(0,0,0,1,.125F,1))),
	ROW(BLOCK_REDSTONE_REPEATER_LIT,0,15,0,ONE(BOX(0,0,0,1,.125F,1)),ONE(BOX(0,0,0,1,.125F,1))),
	/* BlockRedstoneWire.<init>:14; collision:21-22. */
	ROW(BLOCK_REDSTONE_WIRE,0,15,0,ONE(BOX(0,0,0,1,.0625F,1)),NONE),
	/* BlockFlower.<init>:10-11: half .2F, height .2F*3; collision:43-44. */
	ROW(BLOCK_FLOWER,0,15,0,ONE(BOX(.3F,0,.3F,.7F,.2F*3,.7F)),NONE),
	ROW(BLOCK_ROSE,0,15,0,ONE(BOX(.3F,0,.3F,.7F,.2F*3,.7F)),NONE),
	/* BlockMushroom.<init>:8-9: half .2F, height .4F; inherits null collision. */
	ROW(BLOCK_BROWM_MUSHROOM,0,15,0,ONE(BOX(.3F,0,.3F,.7F,.4F,.7F)),NONE),
	ROW(BLOCK_RED_MUSHROOM,0,15,0,ONE(BOX(.3F,0,.3F,.7F,.4F,.7F)),NONE),
	/* BlockSapling.<init>:8-9; BlockTallGrass.<init>:8-9: half .4F, height .8F. */
	ROW(BLOCK_SAPLING,0,15,0,ONE(BOX(.5F-.4F,0,.5F-.4F,.5F+.4F,.8F,.5F+.4F)),NONE),
	ROW(BLOCK_TALL_GRASS,0,2,0,ONE(BOX(.5F-.4F,0,.5F-.4F,.5F+.4F,.8F,.5F+.4F)),NONE),
	/* BlockCrops.<init>:10-11; inherited BlockFlower collision. */
	ROW(BLOCK_CROPS,0,7,0,ONE(BOX(0,0,0,1,.25F,1)),NONE),
	/* BlockReed.<init>:9-10; collision:54-55. */
	ROW(BLOCK_REED,0,15,0,ONE(BOX(.125F,0,.125F,.875F,1,.875F)),NONE),
	/* BlockWeb.getCollisionBoundingBoxFromPool:18-19; inherited full selection. */
	ROW(BLOCK_COBWEB,0,15,0,ONE(FULL),NONE),
	/* BlockPortal.setBlockBoundsBasedOnState:14-24; collision:10-11.
	 * Default Z plane; either X neighbour wins even with Z neighbours present. */
#define PORTAL(n,b) ROW(BLOCK_PORTAL,0,15,n,ONE(b),NONE)
	PORTAL(0,BOX(.375F,0,0,.625F,1,1)),
	PORTAL(1,BOX(0,0,.375F,1,1,.625F)), PORTAL(2,BOX(0,0,.375F,1,1,.625F)),
	PORTAL(4,BOX(.375F,0,0,.625F,1,1)), PORTAL(8,BOX(.375F,0,0,.625F,1,1)),
	PORTAL(15,BOX(0,0,.375F,1,1,.625F)),
	/* BlockFire.getCollisionBoundingBoxFromPool:31-32; no bounds override:
	 * Block.<init>/getSelectedBoundingBoxFromPool retain full cube. */
	ROW(BLOCK_FIRE,0,15,0,ONE(FULL),NONE),
	/* BlockChest, BlockLockedChest, BlockDispenser, BlockFurnace inherit
	 * Block.<init>/getSelectedBoundingBoxFromPool/getCollisionBoundingBoxFromPool. */
	ROW(BLOCK_CHEST,0,15,0,ONE(FULL),ONE(FULL)),
	ROW(BLOCK_LOCKED_CHEST,0,15,0,ONE(FULL),ONE(FULL)),
	ROW(BLOCK_DISPENSER,0,15,0,ONE(FULL),ONE(FULL)),
	ROW(BLOCK_FURNACE,0,15,0,ONE(FULL),ONE(FULL)),
	ROW(BLOCK_FURNACE_LIT,0,15,0,ONE(FULL),ONE(FULL)),
};

static bool contains(const struct AABB* boxes, size_t n, float x, float y, float z) {
	for(size_t i = 0; i < n; i++)
		if(x > boxes[i].x1 && x < boxes[i].x2 && y > boxes[i].y1
		   && y < boxes[i].y2 && z > boxes[i].z1 && z < boxes[i].z2)
			return true;
	return false;
}

static int float_cmp(const void* a, const void* b) {
	float x = *(const float*)a, y = *(const float*)b;
	return (x > y) - (x < y);
}

/* All cuboid faces partition space into cells of constant occupancy. Comparing
 * each cell proves union equality without prescribing the stairs decomposition. */
static bool union_equal(const struct AABB* got, const struct shape* want) {
	float faces[3][8];
	size_t n = 0;
	for(size_t i = 0; i < want->count; i++) {
		const struct AABB pair[] = {got[i], want->boxes[i]};
		for(size_t j = 0; j < 2; j++) {
			faces[0][n] = pair[j].x1; faces[0][n+1] = pair[j].x2;
			faces[1][n] = pair[j].y1; faces[1][n+1] = pair[j].y2;
			faces[2][n] = pair[j].z1; faces[2][n+1] = pair[j].z2;
			n += 2;
		}
	}
	for(size_t axis = 0; axis < 3; axis++)
		qsort(faces[axis], n, sizeof(float), float_cmp);
	for(size_t x = 1; x < n; x++)
		for(size_t y = 1; y < n; y++)
			for(size_t z = 1; z < n; z++) {
				float px = (faces[0][x-1] + faces[0][x]) / 2;
				float py = (faces[1][y-1] + faces[1][y]) / 2;
				float pz = (faces[2][z-1] + faces[2][z]) / 2;
				if(faces[0][x-1] == faces[0][x] || faces[1][y-1] == faces[1][y]
				   || faces[2][z-1] == faces[2][z])
					continue;
				if(contains(got, want->count, px, py, pz)
				   != contains(want->boxes, want->count, px, py, pz))
					return false;
			}
	return true;
}

static bool box_equal(struct AABB a, struct AABB b) {
	return a.x1 == b.x1 && a.y1 == b.y1 && a.z1 == b.z1
		&& a.x2 == b.x2 && a.y2 == b.y2 && a.z2 == b.z2;
}

static bool neighbour_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
						  struct block_data* out) {
	const struct block_info* info = ctx;
	const enum side sides[] = {SIDE_LEFT,SIDE_RIGHT,SIDE_FRONT,SIDE_BACK};
	const int dx[] = {-1,1,0,0}, dz[] = {0,0,-1,1};
	*out = (struct block_data) {0};
	for(int i = 0; i < 4; i++)
		if(x == info->x + dx[i] && y == info->y && z == info->z + dz[i]) {
			*out = info->neighbours[sides[i]];
			return true;
		}
	return false;
}

int main(void) {
	blocks_init();
	int failures = 0, checks = 0;
	for(size_t i = 0; i < sizeof(goldens)/sizeof(goldens[0]); i++) {
		const struct golden* g = &goldens[i];
		for(int meta = g->first_meta; meta <= g->last_meta; meta++) {
			struct block_data b = {.type = g->type, .metadata = meta};
			struct block_data neighbours[SIDE_MAX] = {{0}};
			const enum side sides[] = {SIDE_LEFT,SIDE_RIGHT,SIDE_FRONT,SIDE_BACK};
			for(int s = 0; s < 4; s++)
				if(g->neighbours & (1 << s))
					neighbours[sides[s]].type = BLOCK_PORTAL;
			struct block_info info = {.block = &b, .neighbours = neighbours,
				.x = 17, .y = 64, .z = -33};
			assert(blocks[g->type]);
			for(int entity = 0; entity < 2; entity++) {
				const struct shape* want = entity ? &g->collision : &g->selection;
				struct AABB got[8] = {{0}};
				size_t count = blocks[g->type]->getBoundingBox(&info, entity, NULL);
				size_t filled = blocks[g->type]->getBoundingBox(&info, entity, got);
				bool ok = count == want->count && filled == want->count;
				if(ok && count == 1)
					ok = box_equal(got[0], want->boxes[0]);
				if(ok && count == 2)
					ok = union_equal(got, want);
				/* Both supported neighbour providers must select the same portal
				 * plane. The accessor receives WORLD coordinates, not local offsets. */
				if(g->type == BLOCK_PORTAL) {
					struct block_info accessor = info;
					accessor.neighbours = NULL;
					accessor.get_block = neighbour_get;
					accessor.get_block_ctx = &info;
					size_t n = blocks[g->type]->getBoundingBox(&accessor, entity, got);
					ok = ok && n == want->count;
					if(n == 1)
						ok = ok && box_equal(got[0], want->boxes[0]);
				}
				checks++;
				if(!ok) {
					printf("FAIL id=%u meta=%d neighbours=%u %s: count=%zu/%zu want=%zu\n",
						g->type, meta, g->neighbours, entity ? "collision" : "selection",
						count, filled, want->count);
					for(size_t k = 0; k < want->count; k++)
						printf("  got (%g,%g,%g)-(%g,%g,%g); want (%g,%g,%g)-(%g,%g,%g)\n",
							got[k].x1,got[k].y1,got[k].z1,got[k].x2,got[k].y2,got[k].z2,
							want->boxes[k].x1,want->boxes[k].y1,want->boxes[k].z1,
							want->boxes[k].x2,want->boxes[k].y2,want->boxes[k].z2);
					failures++;
				}
			}
		}
	}
	printf("block_bounds_golden: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
