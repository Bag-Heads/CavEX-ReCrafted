/*
	Beta 1.7.3 painting slice (painting-slice, gap audit #9,
	PAINTING_DISPATCH.md): EntityPainting, EnumArt, ItemPainting.

	Every expected value below is HAND-DERIVED from the cited decompile
	arithmetic (740c583), never by running CaveWii's own implementation and
	recording its output (CODESTYLE.md S4.2, the poisoned-golden rule). The
	xorshift32 draws in the RNG-selection tests are computed by hand-running
	util.c's OWN already-shipped, pre-existing algorithm (x^=x<<13; x^=x>>17;
	x^=x<<5) against a literal chosen seed -- an independent trusted
	primitive, not the painting code under test.

	Ground truth (decompile 740c583):

	EnumArt: 25 entries, (title, sizeX, sizeY, offsetX, offsetY) all in
	PIXELS; declaration order is EnumArt.values() order.

	EntityPainting.setDirection: direction 0/1/2/3 face -Z/-X/+Z/+X. The box
	half-extent on the axis running ALONG the wall is art.sizeX (art.sizeY
	for the vertical axis); the PERPENDICULAR (thin) axis is a fixed 0.5. An
	even block-count footprint (32px=2, 64px=4) gets a further +0.5 center
	shift (getArtSize); odd counts (16px=1, 48px=3) get none.

	EntityPainting.onValidSurface: (1) getCollidingBoundingBoxes(this,
	boundingBox) -- solid block geometry OR another entity's own
	getBoundingBox()-exposed box (ONLY EntityBoat/EntityMinecart override
	that accessor in 740c583 -- grep-verified, cited in
	entity_painting.c) intersecting the thin frame box blocks placement; (2)
	the backing wall must be solid across every art.sizeX/16 x art.sizeY/16
	cell; (3) no other EntityPainting may already occupy the footprint.

	EntityPainting(World,x,y,z,dir): iterates EnumArt.values() in order,
	testing each candidate's own onValidSurface, then draws ONE
	rand.nextInt(fitting.size()) pick among the arts that fit.

	EntityPainting.onUpdate: `if (tickCounter1++ == 100 && !multiplayerWorld)`
	-- fires on the 101st call (post-increment compares the OLD value), then
	resets to 0; on failure, setEntityDead() + drop one ItemStack(Item.painting).

	EntityPainting.attackEntityFrom: ANY hit (no health/damage accumulation)
	-> setEntityDead() + drop one ItemStack(Item.painting).

	ItemPainting.onItemUse: side 0/1 (top/bottom) -> false, no attempt. Other
	sides map FRONT(-Z)->dir0, LEFT(-X)->dir1, BACK(+Z)->dir2, RIGHT(+X)->dir3
	(engine `enum side` naming; piston_texture.c's FRONT=-Z/BACK=+Z/LEFT=-X/
	RIGHT=+X convention cross-checked against Beta's raw side ints 2/4/3/5).

	Batch-f landing note (2026-08-25): ported onto tip from the 2026-08-13
	painting-slice worktree (base 558a7f4). One test added beyond the
	original 19: test_valid_surface_top_boat_in_frame, proving the
	entity-occupancy scan's boat-awareness (the source worktree predates
	CaveWii's own boat entity, so its scan hardcoded ENTITY_MINECART only --
	landing onto tip required reconciling the scan against Beta's real
	{EntityBoat, EntityMinecart} getBoundingBox() override set; see
	entity_painting.c's entity_painting_validate_surface comment).
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_painting.h"
#include "game/game_state.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "particle.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"
#include "worldgen/java_random.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_painting_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define WY 100 /* wall plane -- well above any superflat default terrain */

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

/* [740c583: World.setBlockAndMetadataWithNotify] a write that does not
   change id OR metadata is a faithful no-op and returns false -- several
   sub-tests below deliberately "set" a cell to the value it already has
   (e.g. clearing an untouched, default-air cell to air), so the return
   value here is informational only, never a failure signal. */
static void set(int x, int y, int z, int type, int meta) {
	server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
}

static struct block_data g_bd;
static struct block_info info_at(int x, int y, int z) {
	assert(server_world_get_block(&s.worlds[0], x, y, z, &g_bd));
	return (struct block_info) {
		.block = &g_bd, .neighbours = NULL, .x = x, .y = y, .z = z};
}

/* A generous solid slab, well beyond every EnumArt entry's widest reach
   (block-verified: 64px/shift-0.5 sizes reach tile-1..tile+2 on both axes,
   the widest in the table), centered on (cx,WY,z). Used by the "accept"/
   "everything fits" sub-tests so their own arithmetic is not what is under
   test. */
static void build_slab(int cx, int z) {
	for(int x = cx - 3; x <= cx + 3; x++)
		for(int y = WY - 3; y <= WY + 3; y++)
			set(x, y, z, BLOCK_STONE, 0);
}

static void clear_slab(int cx, int z) {
	for(int x = cx - 3; x <= cx + 3; x++)
		for(int y = WY - 3; y <= WY + 3; y++)
			set(x, y, z, BLOCK_AIR, 0);
}

/* Post-review findings #1/#2/#7: a raw, non-painting entity manually joined
   into s.entities, matching test_entity_persistence_env.c's own alloc_entity
   idiom -- heap-allocated (the dict elsewhere frees its values with free(),
   and other code paths may iterate/erase this id) and poisoned before the
   real constructor runs, so a constructor that forgets to set a field this
   test reads is caught rather than accidentally passing on zeroed memory. */
static struct entity* alloc_entity(uint32_t id) {
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(**pp));
	assert(*pp);
	memset(*pp, 0xBE, sizeof(**pp));
	return *pp;
}

/* --- RPC observer: captures CRPC_SPAWN_PAINTING so the client wiring can be
 * proven without racing the drain thread (deterministic; clin_rpc_send
 * publishes synchronously from the calling thread -- test_boat_env.c's own
 * g_obs idiom, reused verbatim). --- */
static struct {
	unsigned spawn_painting_count;
	struct client_rpc last_spawn_painting;
} g_obs;

static void reset_observer(void) {
	memset(&g_obs, 0, sizeof(g_obs));
}

static void observe_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SPAWN_PAINTING) {
		g_obs.spawn_painting_count++;
		g_obs.last_spawn_painting = *call;
	}
}

/* ===================================================================== *
 * Test 1: the complete 25-entry EnumArt table (transposed-row / swapped-
 * dimension catcher). Pure data, no world needed.
 * ===================================================================== */

static void expect_art(int idx, const char* name, int w, int h, int u, int v) {
	assert(strcmp(g_painting_art[idx].name, name) == 0);
	assert(g_painting_art[idx].size_x_px == w);
	assert(g_painting_art[idx].size_y_px == h);
	assert(g_painting_art[idx].atlas_u_px == u);
	assert(g_painting_art[idx].atlas_v_px == v);
}

static void test_enum_art_table(void) {
	expect_art(0, "Kebab", 16, 16, 0, 0);
	expect_art(1, "Aztec", 16, 16, 16, 0);
	expect_art(2, "Alban", 16, 16, 32, 0);
	expect_art(3, "Aztec2", 16, 16, 48, 0);
	expect_art(4, "Bomb", 16, 16, 64, 0);
	expect_art(5, "Plant", 16, 16, 80, 0);
	expect_art(6, "Wasteland", 16, 16, 96, 0);
	expect_art(7, "Pool", 32, 16, 0, 32);
	expect_art(8, "Courbet", 32, 16, 32, 32);
	expect_art(9, "Sea", 32, 16, 64, 32);
	expect_art(10, "Sunset", 32, 16, 96, 32);
	expect_art(11, "Creebet", 32, 16, 128, 32);
	expect_art(12, "Wanderer", 16, 32, 0, 64);
	expect_art(13, "Graham", 16, 32, 16, 64);
	expect_art(14, "Match", 32, 32, 0, 128);
	expect_art(15, "Bust", 32, 32, 32, 128);
	expect_art(16, "Stage", 32, 32, 64, 128);
	expect_art(17, "Void", 32, 32, 96, 128);
	expect_art(18, "SkullAndRoses", 32, 32, 128, 128);
	expect_art(19, "Fighters", 64, 32, 0, 96);
	expect_art(20, "Pointer", 64, 64, 0, 192);
	expect_art(21, "Pigscene", 64, 64, 64, 192);
	expect_art(22, "BurningSkull", 64, 64, 128, 192);
	expect_art(23, "Skeleton", 64, 48, 192, 64);
	expect_art(24, "DonkeyKong", 64, 48, 192, 112);

	/* painting_art_by_title: exact match + the Kebab(0) fallback for an
	   absent/unmatched Motive. */
	assert(painting_art_by_title("SkullAndRoses") == 18);
	assert(painting_art_by_title("DonkeyKong") == 24);
	assert(painting_art_by_title("NoSuchArt") == 0);
	assert(painting_art_by_title(NULL) == 0);

	printf("  EnumArt table (25/25) + painting_art_by_title OK\n");
}

/* ===================================================================== *
 * Test 2: direction/facing mapping (ItemPainting.onItemUse's side switch).
 * ===================================================================== */

static void test_direction_mapping(void) {
	int dir = -1;
	assert(painting_direction_from_side(SIDE_FRONT, &dir)
		  && dir == PAINTING_DIR_NORTH);
	assert(painting_direction_from_side(SIDE_LEFT, &dir)
		  && dir == PAINTING_DIR_WEST);
	assert(painting_direction_from_side(SIDE_BACK, &dir)
		  && dir == PAINTING_DIR_SOUTH);
	assert(painting_direction_from_side(SIDE_RIGHT, &dir)
		  && dir == PAINTING_DIR_EAST);
	/* Negative control: a painting cannot hang on a floor/ceiling face. */
	assert(!painting_direction_from_side(SIDE_TOP, &dir));
	assert(!painting_direction_from_side(SIDE_BOTTOM, &dir));

	printf("  direction/facing mapping (FRONT/LEFT/BACK/RIGHT, TOP/BOTTOM"
		  " refuse) OK\n");
}

/* ===================================================================== *
 * onValidSurface backing-wall tests: 1x1 (Kebab), 2x1 (Pool), 4x4 (Pointer).
 * Each accepts a full wall, rejects a single missing cell anywhere in the
 * footprint (not just the first), and accepts again once restored.
 * ===================================================================== */

static void test_valid_surface_1x1_kebab(void) {
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	const int TX = 2, TY = WY, TZ = 10;

	set(TX, TY, TZ, BLOCK_AIR, 0);
	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											 PAINTING_DIR_SOUTH, TX, TY, TZ,
											 UINT32_MAX));

	set(TX, TY, TZ, BLOCK_STONE, 0);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	set(TX, TY, TZ, BLOCK_AIR, 0);
	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											 PAINTING_DIR_SOUTH, TX, TY, TZ,
											 UINT32_MAX));

	printf("  onValidSurface 1x1 (Kebab): full wall accepts, hole rejects,"
		  " restore accepts again OK\n");
}

static void test_valid_surface_2x1_pool(void) {
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	const int TX = 8, TY = WY, TZ = 10;
	/* [740c583] Pool is 32x16 -> wall_x=floor(posX-1.0)=TX, blocks_w=2:
	   footprint is exactly (TX,TY,TZ) and (TX+1,TY,TZ). */
	set(TX, TY, TZ, BLOCK_STONE, 0);
	set(TX + 1, TY, TZ, BLOCK_STONE, 0);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Pool*/ 7,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	/* Hole in the SECOND cell only -- catches an "only checks the first
	   cell" bug that a naive port could ship. */
	set(TX + 1, TY, TZ, BLOCK_AIR, 0);
	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Pool*/ 7,
											 PAINTING_DIR_SOUTH, TX, TY, TZ,
											 UINT32_MAX));

	set(TX + 1, TY, TZ, BLOCK_STONE, 0);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Pool*/ 7,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	printf("  onValidSurface 2x1 (Pool): full wall accepts, hole in the 2nd"
		  " cell rejects, restore accepts again OK\n");
}

static void test_valid_surface_4x4_pointer(void) {
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	const int TX = 16, TY = WY, TZ = 10;
	/* [740c583] Pointer is 64x64 -> wall_x=floor(posX-2.0)=TX-1, wall_y=TY-1,
	   blocks_w=blocks_h=4: footprint spans X=[TX-1,TX+2], Y=[TY-1,TY+2]. */
	for(int x = TX - 1; x <= TX + 2; x++)
		for(int y = TY - 1; y <= TY + 2; y++)
			set(x, y, TZ, BLOCK_STONE, 0);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Pointer*/ 20,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	/* Hole at an interior cell (not a corner/edge -- rules out an
	   off-by-one that only checks the footprint's boundary). */
	set(TX + 1, TY + 1, TZ, BLOCK_AIR, 0);
	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Pointer*/ 20,
											 PAINTING_DIR_SOUTH, TX, TY, TZ,
											 UINT32_MAX));

	set(TX + 1, TY + 1, TZ, BLOCK_STONE, 0);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Pointer*/ 20,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	printf("  onValidSurface 4x4 (Pointer): full wall accepts, an interior"
		  " hole rejects, restore accepts again OK\n");
}

/* ===================================================================== *
 * Post-review findings #5/#7 (adversarial review, 2026-08-13): onValidSurface
 * has three parts per this file's own header comment above -- TOP (block
 * half, entity half) and BOTTOM (other-painting) -- but only the MIDDLE
 * backing-wall check had any coverage before this pass. These two exercise
 * the TOP third; test_valid_surface_bottom_painting_overlap and
 * test_getboundingbox_null_and_placement_unobstructed below (after
 * place_painting exists) cover the rest.
 * ===================================================================== */

static void test_valid_surface_top_block_in_frame(void) {
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	const int TX = 4, TY = WY, TZ = 20;
	build_slab(TX, TZ);

	/* [740c583: World.getCollidingBoundingBoxes, STRICT intersectsWith --
	   post-review finding #5] a solid block occupying the frame's own thin
	   box -- the cell directly in front of the wall -- blocks placement even
	   though the backing wall itself is fully intact. Kebab/SOUTH's frame
	   floors entirely into the single cell (TX,TY,TZ+1): pz=TZ+0.5+0.5625,
	   half_z=0.5/32=0.015625 (post-review-round-2 finding #6: the THIN
	   perpendicular axis is a FIXED 0.5 -- painting_compute_geometry's
	   `half_z=0.5f` branch for a N/S wall -- independent of Kebab's own art
	   size; an earlier version of this comment showed "16/32/2" as the
	   derivation, which is both the wrong formula for this axis AND does not
	   even evaluate to 0.015625 under left-to-right division (16/32/2 =
	   0.25)), inset=-0.00625 keeps both faces inside that one cell on every
	   axis. */
	set(TX, TY, TZ + 1, BLOCK_STONE, 0);
	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											 PAINTING_DIR_SOUTH, TX, TY, TZ,
											 UINT32_MAX));

	set(TX, TY, TZ + 1, BLOCK_AIR, 0);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	printf("  onValidSurface TOP (block occupying the frame's own cell):"
		  " blocks placement, removing it restores OK\n");
}

static void test_valid_surface_top_minecart_in_frame(void) {
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	const int TX = 14, TY = WY, TZ = 20;
	build_slab(TX, TZ);

	vec3 frame_pos;
	painting_compute_geometry(/*Kebab*/ 0, PAINTING_DIR_SOUTH, TX, TY, TZ,
							  frame_pos, NULL);
	struct entity* cart = server_local_spawn_minecart_type(frame_pos, &s, 0);
	assert(cart);

	/* [740c583: only EntityBoat/EntityMinecart override
	   Entity.getBoundingBox()] a minecart parked exactly where the frame
	   sits blocks placement -- one of the two entity kinds that can occupy
	   this half of the TOP check per this file's own header comment (the
	   other is a boat -- see test_valid_surface_top_boat_in_frame below). */
	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											 PAINTING_DIR_SOUTH, TX, TY, TZ,
											 UINT32_MAX));

	uint32_t cart_id = cart->id;
	dict_entity_erase(s.entities, cart_id);
	free(cart);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	printf("  onValidSurface TOP (minecart parked in the frame): blocks"
		  " placement, removing it restores OK\n");
}

/* ===================================================================== *
 * Batch-f landing addition (2026-08-25): the source worktree's own base
 * (558a7f4, 2026-08-13) predates CaveWii's boat entity, so its
 * entity-occupancy scan hardcoded ENTITY_MINECART only. Landing this slice
 * onto a tip that HAS boats required reconciling that scan against Beta's
 * real getBoundingBox()-override set ({EntityBoat, EntityMinecart}, per
 * this file's own header comment) rather than blindly generalizing to "any
 * entity with a non-null e->getBoundingBox" (which would have been WRONG --
 * see entity_painting.c's entity_painting_validate_surface comment for why).
 * This test proves the boat half of that set actually works, mirroring
 * test_valid_surface_top_minecart_in_frame above one-for-one.
 * ===================================================================== */

static void test_valid_surface_top_boat_in_frame(void) {
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	const int TX = 48, TY = WY, TZ = 25; /* fresh row (TZ=25), clear of every
		other sub-test's own slab regardless of X overlap. */
	build_slab(TX, TZ);

	vec3 frame_pos;
	painting_compute_geometry(/*Kebab*/ 0, PAINTING_DIR_SOUTH, TX, TY, TZ,
							  frame_pos, NULL);
	struct entity* boat = server_local_spawn_boat(frame_pos, &s);
	assert(boat);

	/* [740c583: only EntityBoat/EntityMinecart override
	   Entity.getBoundingBox()] a boat parked exactly where the frame sits
	   blocks placement -- the SAME reason a parked minecart does above. */
	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											 PAINTING_DIR_SOUTH, TX, TY, TZ,
											 UINT32_MAX));

	uint32_t boat_id = boat->id;
	dict_entity_erase(s.entities, boat_id);
	free(boat);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											PAINTING_DIR_SOUTH, TX, TY, TZ,
											UINT32_MAX));

	printf("  onValidSurface TOP (boat parked in the frame): blocks"
		  " placement, removing it restores OK\n");
}

/* ===================================================================== *
 * Placement-time art SELECTION under a pinned RNG.
 *
 * util.c's xorshift32, hand-run for seed=12345 (independent of the painting
 * code under test): x^=x<<13; x^=x>>17; x^=x<<5 -> raw=3336926330
 * (0xC6E5747A). rand_u32_to_flt = (raw>>8) / 16777216 = 0.7769386768...
 *   rand_gen_int(seed=12345, 25) = (int)(0.7769386768 * 25) = (int)19.42... = 19
 *   rand_gen_int(seed=12345, 12) = (int)(0.7769386768 * 12) = (int)9.32...  = 9
 * ===================================================================== */

static void test_art_selection_all_fit(void) {
	const int TX = 26, TZ = 10;
	build_slab(TX, TZ);

	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	/* I02: the pick is java.util.Random.nextInt(25) on the painting's own
	   stream. Oracle: an independent Python port of java.util.Random
	   (setSeed scramble ^0x5DEECE66D, 48-bit LCG, nextInt(n) rejection
	   loop): new Random(12345).nextInt(25) == 1. */
	struct java_random rng;
	jrand_set_seed(&rng, 12345);
	int art_index = -1;
	assert(entity_painting_select_art(&scratch, &s, PAINTING_DIR_SOUTH, TX,
									  WY, TZ, &rng, &art_index));
	/* Every one of the 25 fits (the slab covers the widest reach on every
	   axis), so the fitting array is the identity [0..24] and the single
	   nextInt(25) draw picks its own value directly: index 1 = "Aztec". */
	assert(art_index == 1);
	assert(strcmp(g_painting_art[art_index].name, "Aztec") == 0);

	printf("  art selection (all 25 fit): pinned seed picks index 1"
		  " (Aztec) OK\n");
}

/* Post-review-round-2 finding #3 (adversarial review, 2026-08-13): the
   ORIGINAL version of this test (a 2-wide, 1-tall wall, fitting set
   [0..11]) and test_art_selection_all_fit (fitting set [0..24]) are BOTH
   dense, zero-based prefixes -- fitting[pick]==pick for every reachable
   draw -- so a regression that drops the fitting[] indirection entirely
   (`*out_art_index = pick;` instead of `*out_art_index = fitting[pick];`)
   would pass every art-selection test in this file despite this test's own
   former comment claiming it "exercises the collected-fitting-set
   indirection." Exhaustively re-derived by hand (see the achievable-count
   analysis in this session's report): under this engine's SOUTH-anchored,
   single-clicked-tile geometry, EVERY art's footprint nests inside every
   larger one anchored at the same tile, so the ONLY way to get a fitting
   set with a gap BEFORE its tail (not just after, where the seed=12345 the
   rest of this file uses can never reach) is to satisfy the seven 1x1 arts
   (0-6) and the two 1x2 arts (12,13) -- both width-1 -- while EXCLUDING the
   five 2x1 arts (7-11), which need a second column. That geometry is
   unchanged from the original test's "1-wide, 2-tall" scenario (still
   width-1, still height-2); what changes is the PINNED SEED: seed=12345's
   own single draw lands its pick inside this scenario's dense [0..6] head
   (verified: floor(0.7769386768...*9)=6, and fitting[6]==6), which is
   EXACTLY why the file's original two art-selection tests don't
   discriminate -- not a fixable property of their geometry, a property of
   that one seed for every count this geometry family can produce. */
static void test_art_selection_subset(void) {
	const int TX = 36, TZ = 10;
	/* ONLY a 1-BLOCK-WIDE, 2-tall backing wall -- column TX solid at rows WY
	   and WY+1; column TX+1 stays default air. Every width-1 entry fits
	   regardless of the extra height (the seven 16x16 arts, 0-6); the two
	   16x32 arts (12,13 -- the only width-1 entries taller than one block)
	   additionally need row WY+1, which is present. Every width>=2 entry
	   (7-11, 14-24) needs column TX+1 too, which stays air, so NONE of them
	   fit. Fitting set (ascending index order, the loop's order):
	   [0,1,2,3,4,5,6,12,13] -- a REAL gap (7-11 skipped). */
	set(TX, WY, TZ, BLOCK_STONE, 0);
	set(TX, WY + 1, TZ, BLOCK_STONE, 0);

	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	/* A DIFFERENT pinned seed than the rest of this file (12345): hand-run
	   the SAME already-shipped, pre-existing xorshift32 algorithm this
	   file's header documents (x^=x<<13; x^=x>>17; x^=x<<5) against
	   seed=12868 -> raw=0xCF5E9D60. rand_u32_to_flt = (raw>>8)/16777216 =
	   0.8100374341... rand_gen_int(seed=12868, 9) = (int)(0.8100374341*9) =
	   (int)7.29... = 7. Independently cross-checked two ways: (1) a Python
	   reimplementation of the same three-line xorshift32 step, searched for
	   a seed landing comfortably inside [7,8) for n=9 (margin ~0.29, far
	   above any float32-vs-double rounding slop); (2) a standalone C build
	   of util.c's OWN rand_gen/rand_gen_int (not the painting code under
	   test), which also reproduces this file's EXISTING seed=12345
	   citations (n=25->19, n=12->9) before trusting it for this new seed. */
	/* I02: java.util.Random now (oracle as above): new Random(12002)
	   .nextInt(9) == 7, the first seed >= 12000 landing on position 7. */
	struct java_random rng;
	jrand_set_seed(&rng, 12002);
	int art_index = -1;
	assert(entity_painting_select_art(&scratch, &s, PAINTING_DIR_SOUTH, TX,
									  WY, TZ, &rng, &art_index));
	/* fitting = [0,1,2,3,4,5,6,12,13]; pick=7 lands on the array's 8th
	   ENTRY (0-indexed position 7), which is VALUE 12 ("Wanderer") -- NOT
	   value 7 ("Pool", which doesn't even fit this wall). A regression that
	   returns `pick` directly instead of `fitting[pick]` would produce 7
	   here and fail this assertion. */
	assert(art_index == 12);
	assert(strcmp(g_painting_art[art_index].name, "Wanderer") == 0);

	printf("  art selection (gapped subset [0-6,12,13], seed=12002): pinned"
		  " seed picks fitting[7]=12 (Wanderer), NOT raw pick=7 (Pool) --"
		  " discriminates the fitting[] indirection OK\n");
}

static void test_art_selection_none_fit(void) {
	const int TX = 44, TZ = 10; /* left untouched: pure default air */
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	struct java_random rng;
	jrand_set_seed(&rng, 12345);
	int art_index = -1;
	assert(!entity_painting_select_art(&scratch, &s, PAINTING_DIR_SOUTH, TX,
									   WY, TZ, &rng, &art_index));

	printf("  art selection (nothing fits): returns false OK\n");
}

/* ===================================================================== *
 * Real placement path (items[ITEM_PAINTING]->onUseOnBlock) + the periodic
 * (100-tick) invalid-surface drop [740c583: onUpdate].
 * ===================================================================== */

static struct entity* find_painting_at(int tx, int ty, int tz) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_PAINTING && e->data.painting.tile_x == tx
		   && e->data.painting.tile_y == ty && e->data.painting.tile_z == tz) {
			assert(!found); /* the other-painting overlap check must forbid a dup */
			found = e;
		}
		dict_entity_next(it);
	}
	return found;
}

static size_t count_painting_drops(void) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_PAINTING)
			n++;
		dict_entity_next(it);
	}
	return n;
}

static struct entity* place_painting(int tx, int tz, struct item_data* held) {
	build_slab(tx, tz);
	struct block_info on = info_at(tx, WY, tz);
	/* SIDE_BACK == direction SOUTH (+Z), matching every geometry helper
	   above -- drives the REAL production hook, not raw entity_painting(). */
	assert(items[ITEM_PAINTING]->onUseOnBlock(&s, &s.player, held, &on, SIDE_BACK));
	struct entity* pnt = find_painting_at(tx, WY, tz);
	assert(pnt);
	assert(pnt->data.painting.direction == PAINTING_DIR_SOUTH);
	assert(pnt->data.painting.art_index >= 0
		  && pnt->data.painting.art_index < PAINTING_ART_COUNT);
	/* [740c583: EntityPainting(World)] tickCounter1 starts at 0 -- a fresh
	   spawn's counter is exactly known, which is what makes the 100-vs-101
	   boundary test below an EXACT count instead of a window. */
	assert(pnt->data.painting.tick_counter == 0);
	return pnt;
}

/* I02 (board #8): the art pick is the NEW painting's own Random, not the
 * world's. CFR EntityPainting.java:61 `this.art = (EnumArt)var6.get(this.rand
 * .nextInt(var6.size()))` inside the constructor -- `this.rand` is the
 * painting's Entity.rand (Entity.java:96), and Entity's constructor has
 * already taken the entity id (nextEntityID++). Here: the lifetime stream
 * seeded from (world seed, dimension, the placed painting's id). Every art
 * fits this slab, so the fitting set is the identity [0..24]. */
static void test_placement_rng_owner(void) {
	/* Four placements (separate walls): a stream-owner mutant that still
	 * picks the right art by 1-in-25 coincidence cannot survive all four. */
	for(int k = 0; k < 4; k++) {
	const int TX = 100 + 8 * k, TZ = 40;
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	const struct random_gen world_before = s.rand_src;
	struct entity* pnt = place_painting(TX, TZ, &held);
	const bool world_kept
		= memcmp(&world_before, &s.rand_src, sizeof world_before) == 0;
	struct java_random jr;
	entity_lifetime_rng_seed(&jr, s.worlds[0].seed, s.worlds[0].dimension,
							 pnt->id);
	const int want = jrand_next_int_bound(&jr, PAINTING_ART_COUNT);
	printf("  placement art pick: id %u art %d, own-stream Beta pick %d, world"
		   " stream %s\n", (unsigned)pnt->id, pnt->data.painting.art_index,
		   want, world_kept ? "untouched" : "MOVED");
	assert(world_kept
		   && "the art pick draws nothing from the world stream");
	assert(pnt->data.painting.art_index == want
		   && "the art pick is nextInt(25) on the painting's own stream");
	}
	printf("  placement art pick on the painting's own stream "
		   "(EntityPainting.java:61) OK\n");
}

static void test_placement_and_invalid_surface_drop(void) {
	assert(ITEM_PAINTING == 321);
	assert(item_get(&(struct item_data) {.id = ITEM_PAINTING})
		  == items[ITEM_PAINTING]);

	/* Entity A: a VALID wall must survive ticking well past the 100-tick
	   boundary (twice over) -- a loose negative control, no exact-count
	   claim (each firing just re-validates and harmlessly resets the
	   counter, since the wall never becomes invalid here). */
	const int AX = 52, AZ = 10;
	struct item_data held_a = {.id = ITEM_PAINTING, .durability = 0, .count = 5};
	place_painting(AX, AZ, &held_a);
	assert(held_a.count == 4); /* [740c583] `--var1.stackSize;` -- exactly one */
	for(int i = 0; i < 205; i++)
		assert(server_local_test_tick_entities(&s));
	assert(find_painting_at(AX, WY, AZ) != NULL);

	/* Entity B: a FRESH painting (tick_counter guaranteed 0), spaced well
	   clear of A's slab, isolates the exact 100-vs-101 boundary from any
	   residual counter phase A's loop above may have left behind. */
	const int BX = 64, BZ = 10;
	struct item_data held_b = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* pnt = place_painting(BX, BZ, &held_b);

	/* Knock the backing wall out from under it. */
	clear_slab(BX, BZ);
	size_t drops_before = count_painting_drops();

	/* [740c583] `tickCounter1++ == 100` fires on the 101st call (post-
	   increment compares the OLD value) -- exactly 100 ticks from the known
	   zero counter must still not have removed it (CODESTYLE S4.3: exact
	   tick counts, not windows). */
	for(int i = 0; i < 100; i++)
		assert(server_local_test_tick_entities(&s));
	assert(find_painting_at(BX, WY, BZ) == pnt);
	assert(count_painting_drops() == drops_before);

	/* The 101st tick crosses the boundary: removed, exactly one drop. */
	assert(server_local_test_tick_entities(&s));
	assert(find_painting_at(BX, WY, BZ) == NULL);
	assert(count_painting_drops() == drops_before + 1);

	printf("  real placement (onUseOnBlock, -1 stack) + invalid-surface"
		  " drop at EXACTLY tick 101, not before OK\n");
}

/* ===================================================================== *
 * Post-review finding #7 continued (BOTTOM check) and findings #1/#2 (a live
 * painting must never expose a non-null getBoundingBox to generic external
 * consumers). Placed after place_painting/find_painting_at exist.
 * ===================================================================== */

static void test_valid_surface_bottom_painting_overlap(void) {
	const int TX = 24, TZ = 20;
	struct item_data held_a = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* a = place_painting(TX, TZ, &held_a);

	/* [740c583: onValidSurface's trailing `instanceof EntityPainting` scan] a
	   second painting attempting the SAME footprint through the REAL
	   placement path must be refused -- driven through items[ITEM_PAINTING]
	   ->onUseOnBlock, not entity_painting_on_valid_surface directly, so a
	   regression in the self_id exclusion or a dropped ENTITY_PAINTING branch
	   is caught the same way a player would trigger it. Beta's onItemUse
	   returns true unconditionally on a non-top/bottom face even when
	   nothing spawns (PAINTING_SLICE_NOTES.md conflict #2) -- the real
	   assertion is that nothing NEW gets created and the stack is untouched. */
	struct item_data held_b = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct block_info on = info_at(TX, WY, TZ);
	assert(items[ITEM_PAINTING]->onUseOnBlock(&s, &s.player, &held_b, &on, SIDE_BACK));
	assert(held_b.count == 1); /* refused placement never consumes the item */
	assert(find_painting_at(TX, WY, TZ) == a); /* still exactly the first one;
		find_painting_at's own internal assert(!found) would also catch a
		silent duplicate */

	printf("  onValidSurface BOTTOM (painting overlap): a second painting"
		  " cannot occupy an occupied footprint via the real placement path"
		  " OK\n");
}

/* L214: Beta's collision and painting scans belong to WorldObj. Identical
 * frame coordinates in another live dimension never obstruct this wall. */
static void test_foreign_world_overlaps(void) {
	const int TX = 24, TZ = 35;
	build_slab(TX, TZ);
	struct entity scratch = {0};
	entity_painting(0, &scratch, true, &s.worlds[0], 0,
		PAINTING_DIR_SOUTH, TX, WY, TZ);
	string_t path;
	string_init_set_str(path, TMP_WORLD);
	server_world_create(&s.worlds[1], path, WORLD_DIM_NETHER, 4242, true);
	string_clear(path);
	s.worlds[1].session = &s;
	s.worlds[1].world_index = 1;
	for(unsigned kind = 0; kind < 3; kind++) {
		uint32_t id = server_local_gen_id(&s);
		struct entity* foreign = alloc_entity(id);
		if(kind == 0)
			entity_painting(id, foreign, true, &s.worlds[1], 0,
				PAINTING_DIR_SOUTH, TX, WY, TZ);
		else {
			if(kind == 1) entity_boat(id, foreign, true, &s.worlds[1]);
			else entity_minecart(id, foreign, true, &s.worlds[1]);
			foreign->teleport(foreign, scratch.pos);
		}
		assert(entity_painting_on_valid_surface(&scratch, &s, 0,
			PAINTING_DIR_SOUTH, TX, WY, TZ, UINT32_MAX));
		free(foreign); dict_entity_erase(s.entities, id);
	}
	server_world_discard(&s.worlds[1]);
	puts("  L214: foreign painting/boat/cart frames do not obstruct own wall OK");
}

static void test_getboundingbox_null_and_placement_unobstructed(void) {
	const int TX = 34, TZ = 20;
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* pnt = place_painting(TX, TZ, &held);

	/* Root cause, directly [740c583: only EntityBoat/EntityMinecart override
	   Entity.getBoundingBox()] -- post-review findings #1/#2. */
	assert(pnt->getBoundingBox == NULL);

	/* block_place_obstructed_for (block/blocks.c) has no per-entity-kind
	   allowlist: it treats ANY entity with a non-null getBoundingBox as
	   placement-blocking. Attempt to place an ordinary block into the SAME
	   cell the frame occupies (the same geometry
	   test_valid_surface_top_block_in_frame proves the frame covers) --
	   Beta allows this unconditionally. */
	struct block_data new_block = {.type = BLOCK_STONE, .metadata = 0,
								   .sky_light = 0, .torch_light = 0};
	struct block_info where
		= {.block = &new_block, .x = TX, .y = WY, .z = TZ + 1};
	assert(!block_place_obstructed_for(&s, &s.player, &where)
		  && "a live painting must never block ordinary placement through its"
			" own frame -- Beta's Entity.getBoundingBox() is null for"
			" EntityPainting");

	/* Negative control: the SAME function correctly refuses placement into a
	   live mob's box, proving the assertion above is discriminating -- the
	   sweep is actually live, not vacuously passing because nothing in it
	   ever blocks. */
	const uint32_t MOB_ID = 990001;
	struct entity* mob = alloc_entity(MOB_ID);
	entity_monster(MOB_ID, mob, true, &s.worlds[0], MONSTER_PIG);
	mob->teleport(mob, (vec3) {(float)TX + 3.5f, (float)WY, (float)TZ + 0.5f});
	struct block_data mob_block = {.type = BLOCK_STONE, .metadata = 0,
								   .sky_light = 0, .torch_light = 0};
	struct block_info mob_where
		= {.block = &mob_block, .x = TX + 3, .y = WY, .z = TZ};
	assert(block_place_obstructed_for(&s, &s.player, &mob_where)
		  && "negative control: a live mob's own box must still obstruct"
			" placement (proves the sweep itself is live)");
	dict_entity_erase(s.entities, MOB_ID);
	free(mob);

	printf("  getBoundingBox stays NULL: a painting never obstructs ordinary"
		  " block placement (negative control: a mob still does) OK\n");
}

static void test_periodic_recheck_survives_unloaded_wall_chunk(void) {
	/* Post-review finding #4: a painting's OWN chunk membership is keyed by
	   e->pos (the wall-offset geometry center), which can land in a
	   DIFFERENT chunk than its backing wall's own tile_x/tile_z when the
	   wall sits at a chunk seam. If ONLY the wall's chunk is evicted (this
	   engine's near/far streaming has no atomicity between neighbouring
	   chunks) while the painting's own chunk -- and therefore the entity
	   itself -- stays resident and ticking, the periodic revalidation must
	   not destroy an otherwise-intact painting just because its dependency
	   is temporarily not resident in server memory. */
	const int TX = 84, TZ = 15; /* wall chunk (5,0); SOUTH's 0.5625 wall
		offset pushes the entity's own pos into chunk (5,1) -- confirmed
		below. Kebab/SOUTH's along-axis (X) has no center shift, so X never
		diverges; TX=84 sits mid-chunk (80..95) with margin either way. */
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* pnt = place_painting(TX, TZ, &held);
	uint32_t id = pnt->id;

	/* Sanity: confirm the geometric premise (both operands non-negative, so
	   plain integer division IS floor division here) before trusting the
	   rest of this test. */
	assert(TZ / 16 == 0);               /* the wall's own chunk row */
	assert((int)pnt->pos[2] / 16 == 1); /* the entity's own chunk row */
	assert((int)pnt->pos[0] / 16 == TX / 16); /* X never diverges for
		Kebab/SOUTH (no along-axis center shift) -- same chunk column as the
		wall either way, isolating the scenario to Z alone. */

	/* Evict ONLY the wall's chunk -- the same save+free primitive the real
	   near-eviction loop uses (server_local.c / redstone_piston_server.c),
	   called directly to isolate the scenario. server_world_save_chunk only
	   knows about struct server_world, never s.entities, so the entity
	   itself is untouched. */
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true, 5, 0));
	assert(dict_entity_get(s.entities, id) != NULL); /* still live/ticking */

	/* 101 ticks would destroy a GENUINELY invalid painting (proven by Entity
	   B's case in test_placement_and_invalid_surface_drop above, whose wall
	   stays LOADED throughout). Here the wall is fully intact on disk --
	   just not resident -- so the fix must skip this cycle, not destroy. */
	size_t drops_before = count_painting_drops();
	for(int i = 0; i < 101; i++)
		assert(server_local_test_tick_entities(&s));
	assert(find_painting_at(TX, WY, TZ) == pnt);
	assert(count_painting_drops() == drops_before);

	/* Restore the wall's chunk (round-trips the exact stone block saved
	   before eviction) and cross the NEXT 100-tick boundary: the skip must
	   be a deferral, not a permanent grandfather-in -- the real, now-
	   resident, genuinely solid wall gets an actual re-check and still
	   survives. */
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 5, 0, &sc) && sc);
	for(int i = 0; i < 101; i++)
		assert(server_local_test_tick_entities(&s));
	assert(find_painting_at(TX, WY, TZ) == pnt);
	assert(count_painting_drops() == drops_before);

	printf("  periodic recheck: an unloaded WALL chunk (entity's own chunk"
		  " still resident) is skipped, not destroyed -- reload then a real"
		  " re-check both confirm survival OK\n");
}

/* ===================================================================== *
 * attackEntityFrom: ANY hit pops it -- confirms the dispatch note's open
 * question ("does a player-broken painting also drop?" -- yes [740c583]).
 * ===================================================================== */

static void attack(uint32_t id) {
	struct server_rpc rpc
		= {.type = SRPC_ENTITY_ATTACK, .payload.entity_attack.entity_id = id};
	server_local_test_process_rpc(&s, &rpc);
}

static void test_attack_pops_and_drops(void) {
	const int TX = 76, TZ = 10; /* clear of entity B's 61..67 slab above */
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* pnt = place_painting(TX, TZ, &held);
	uint32_t id = pnt->id;

	/* Reach gate setup (same 36.0 feet-basis convention as the minecart/
	   fireball attack branches this dispatch shares). */
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	/* Batch-f landing note: server_rpc_blocked_while_dead (server_local.c)
	   drops SRPC_ENTITY_ATTACK outright while player.health<=0 -- a gate
	   added after this test's original 2026-08-13 base, matching
	   test_minecart_authority.c's own precedent for the identical
	   attack-dispatch test shape. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = pnt->pos[0];
	s.player.y = pnt->pos[1] + PLAYER_EYE_HEIGHT;
	/* Batch-f landing note: +1.0, not -1.0 -- SOUTH's wall_offset already
	   pushes the frame ~1 block out from the wall into open (+Z) air, so
	   standing at pos[2]-1.0 (this test's original 2026-08-13 value, written
	   before server_local_entity_interaction_allowed's LOS leg existed for
	   this branch) puts the player's EYE literally inside build_slab's own
	   oversized solid wall a single cell over -- los_segment_blocked then
	   (correctly) reports no line of sight from inside solid geometry, and
	   the attack is silently dropped by the gate, exactly like a real player
	   embedded in a wall would be. +1.0 stands in genuinely open air on the
	   frame's visible (open) side, matching how an actual player would have
	   to approach to see/attack a wall-mounted painting. */
	s.player.z = pnt->pos[2] + 1.0;

	size_t drops_before = count_painting_drops();
	attack(id);

	assert(find_painting_at(TX, WY, TZ) == NULL);
	assert(count_painting_drops() == drops_before + 1);

	printf("  attackEntityFrom: a single hit pops the painting and drops"
		  " exactly one Item.painting OK\n");
}

/* ===================================================================== *
 * Post-review findings #3/#8: onLeftClick must QUEUE SRPC_ENTITY_ATTACK
 * through the real svin transport -- screen_ingame.c only sends that RPC
 * when e->onLeftClick is non-NULL (game/gui/screen_ingame.c's left-click
 * block calls e->onLeftClick(e) and returns), so leaving it NULL made the
 * already-tested server-side dispatch above unreachable from real input --
 * test_attack_pops_and_drops is green only because attack() injects
 * SRPC_ENTITY_ATTACK directly, bypassing the client dispatch entirely.
 * Mirrors test_minecart_authority.c's proven fix for the identical bug class
 * on the minecart's onLeftClick.
 * ===================================================================== */

static void test_left_click_queues_attack_rpc(void) {
	const int TX = 44, TZ = 20;
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* authoritative = place_painting(TX, TZ, &held);
	uint32_t id = authoritative->id;

	/* Reach gate setup; guarded against test_attack_pops_and_drops above
	   already having created the player inventory -- inventory_create
	   mallocs a fresh items[] array unconditionally, so a second call here
	   would leak the first (CODESTYLE 4.3-class fixture hazard). */
	if(!s.player.active_inventory) {
		assert(inventory_create(&s.player.inventory, &inventory_logic_player,
								&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
		s.player.active_inventory = &s.player.inventory;
	}
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = authoritative->pos[0];
	s.player.y = authoritative->pos[1] + PLAYER_EYE_HEIGHT;
	/* +1.0 not -1.0 -- see test_attack_pops_and_drops's comment: this stands
	   in genuinely open air on the frame's visible side, clear of
	   build_slab's own oversized wall, so server_local_entity_interaction_
	   allowed's LOS leg (server_local.c) does not (correctly) refuse an
	   eye position embedded in solid geometry. */
	s.player.z = authoritative->pos[2] + 1.0;

	/* The client-side twin: the SAME factory function, server=false --
	   exactly what a future CRPC_SPAWN_PAINTING handler would construct
	   (entity.h's ENTITY_PAINTING comment; PAINTING_SLICE_NOTES.md's
	   deferred client-networking item). */
	struct entity proxy;
	memset(&proxy, 0xBE, sizeof(proxy));
	entity_painting(id, &proxy, false, &s.worlds[0],
					authoritative->data.painting.art_index,
					authoritative->data.painting.direction,
					authoritative->data.painting.tile_x,
					authoritative->data.painting.tile_y,
					authoritative->data.painting.tile_z);
	assert(proxy.onLeftClick && "a client painting must have a click handler");

	size_t drops_before = count_painting_drops();
	assert(proxy.onLeftClick(&proxy));
	/* Unlike the minecart, painting has no client-predicted shake/damage
	   state -- the ONLY effect of a click is the queued RPC, so nothing
	   authoritative has changed yet. */
	assert(find_painting_at(TX, WY, TZ) == authoritative);

	server_local_test_process_messages(&s);

	assert(find_painting_at(TX, WY, TZ) == NULL
		  && "a left click must reach the server as SRPC_ENTITY_ATTACK"
			" through the real transport, not just via direct injection");
	assert(count_painting_drops() == drops_before + 1);

	printf("  onLeftClick: queues SRPC_ENTITY_ATTACK through the real svin"
		  " transport (screen_ingame's actual dispatch path), not just"
		  " direct RPC injection OK\n");
}

/* ===================================================================== *
 * Post-review-round-2 finding #1 (adversarial review, 2026-08-13):
 * entity_painting() correctly nulls e->getBoundingBox (findings #1/#2 of
 * the first review round -- Beta's Entity.getBoundingBox() is null for
 * EntityPainting), but this engine's raycast_entity/entity_aabb_
 * intersect_ray (entity.c) reused that SAME accessor as its click-target
 * gate, so the fix for #1/#2 silently broke the fix for #3/#8 (onLeftClick
 * reachability): an entity with a null getBoundingBox was invisible to
 * raycast_entity, so screen_ingame.c's gstate.players[0].camera_hit.entity_hit could
 * never become true for a painting and onLeftClick was never actually
 * called from real input -- test_left_click_queues_attack_rpc above only
 * proves the RPC plumbing works ONCE onLeftClick fires; it calls
 * proxy.onLeftClick(&proxy) directly, never raycast_entity/main.c's real
 * hit-test path. This test drives the REAL raycast_entity/entity_aabb_
 * intersect_ray path end to end (the exact functions main.c calls to
 * populate gstate.players[0].camera_hit) against a live, dict-resident painting.
 * ===================================================================== */

static void test_raycast_hits_live_painting(void) {
	const int TX = 92, TZ = 10;
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* pnt = place_painting(TX, TZ, &held);
	assert(pnt->getBoundingBox == NULL); /* the findings #1/#2 precondition */

	/* A ray aimed straight at the frame's own logical center (pnt->pos --
	   painting_compute_geometry's out_pos, exactly what the frame AABB is
	   centered on) from 3 blocks out on the open (+Z) side, looking back
	   along -Z -- guaranteed to cross the thin frame box regardless of its
	   exact half-extent. */
	vec3 origin = {pnt->pos[0], pnt->pos[1], pnt->pos[2] + 3.0f};
	vec3 dir = {0.0f, 0.0f, -1.0f};
	float t_near = -1.0f;
	struct entity* hit = raycast_entity(&s.entities, origin, dir, 10.0f, &t_near);

	assert(hit == pnt
		  && "a live painting with getBoundingBox==NULL must still be a"
			" valid raycast_entity target -- the real click-target gate"
			" main.c/screen_ingame.c use, not just direct onLeftClick"
			" injection");
	assert(t_near > 0.0f && t_near <= 10.0f);

	/* Negative control: aim the SAME ray direction from an origin well clear
	   of the frame on the along-wall axis, proving the hit above is real
	   geometric intersection, not raycast_entity vacuously returning
	   whatever entity happens to be resident regardless of direction. */
	vec3 origin_miss = {pnt->pos[0] + 20.0f, pnt->pos[1], pnt->pos[2] + 3.0f};
	float t_near_miss = -1.0f;
	struct entity* miss = raycast_entity(&s.entities, origin_miss, dir, 10.0f,
										 &t_near_miss);
	assert(miss != pnt);

	printf("  raycast_entity/entity_aabb_intersect_ray: a live painting"
		  " (getBoundingBox==NULL) IS a valid click-target ray hit; a ray"
		  " aimed away from it is not OK\n");
}

/* ===================================================================== *
 * Post-review-round-2 finding #2: give the TOP check's block-half
 * (painting_top_block_probe) the SAME SOLID/BLOCKED/INDETERMINATE
 * tri-state test_periodic_recheck_survives_unloaded_wall_chunk above
 * already proved for the MIDDLE (backing-wall) check -- and discriminate it
 * the way that test's own tick-level observation CANNOT:
 * entity_painting_server_tick's dispatch only branches on
 * `== PAINTING_SURFACE_BLOCKED` (its switch treats VALID and INDETERMINATE
 * identically -- survive either way), so the TOP check's tri-state fix has
 * NO tick-time-observable consumer today. The PLACEMENT-time bool wrapper
 * (entity_painting_on_valid_surface) DOES discriminate: it collapses
 * BLOCKED **and** INDETERMINATE to `false`, so an unresolved TOP-check cell
 * now correctly REFUSES to certify placement, whereas the pre-fix generic
 * entity_intersection walk silently treated "chunk not resident" identically
 * to "resolved, genuinely clear" and returned VALID.
 * ===================================================================== */

static void test_top_check_unresolved_frame_chunk_refuses_placement(void) {
	struct entity scratch;
	entity_default_init(&scratch, true, &s.worlds[0]);
	const int TX = 56, TZ = 15; /* wall chunk (3,0); SOUTH's 0.5625 wall
		offset pushes the FRAME (not just e->pos, per
		test_periodic_recheck_survives_unloaded_wall_chunk's own premise)
		into chunk (3,1) -- an otherwise-untouched chunk pair, isolated from
		every other test in this file. */
	set(TX, WY, TZ, BLOCK_STONE, 0); /* the backing wall: solid, resident. */

	assert(TZ / 16 == 0); /* the wall's own chunk row */

	/* Evict ONLY the frame's chunk -- the wall's own chunk (3,0) stays fully
	   resident, so the MIDDLE check always has a definitive answer here;
	   only the TOP check's block-half probe (which scans cells around the
	   frame's thin box, i.e. around the painting's logical center) can see
	   an unresolved cell in this scenario. */
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true, 3, 1));

	assert(!entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											 PAINTING_DIR_SOUTH, TX, WY, TZ,
											 UINT32_MAX)
		  && "an unresolved cell in the TOP check's own probe box must"
			" refuse to certify placement, not silently pass it as clear");

	/* Reload the frame's chunk: the SAME wall, now fully resolvable again,
	   must be accepted -- proving the refusal above was really about
	   residency, not a genuine geometry problem. */
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 3, 1, &sc) && sc);
	assert(entity_painting_on_valid_surface(&scratch, &s, /*Kebab*/ 0,
											PAINTING_DIR_SOUTH, TX, WY, TZ,
											UINT32_MAX));

	printf("  onValidSurface TOP (unresolved frame chunk, wall fully"
		  " resident): refuses to certify placement on unverifiable data,"
		  " reload restores acceptance OK\n");
}

/* ===================================================================== *
 * Post-review-round-2 finding #4: server_local_gen_id's wraparound guard
 * excluded id 0 but not UINT32_MAX, which entity_painting_select_art uses
 * as the "no self" sentinel for a not-yet-placed scratch candidate -- a
 * live entity actually assigned that id would be silently excluded from
 * every future painting/minecart overlap dict-scan. Force the counter to
 * the boundary directly (server_local.h's next_entity_id is an ordinary
 * struct field, no encapsulation to fight) rather than looping 2^32 times.
 * ===================================================================== */

static void test_gen_id_never_returns_uint32_max(void) {
	uint32_t saved = s.next_entity_id;
	s.next_entity_id = UINT32_MAX - 1;

	uint32_t first = server_local_gen_id(&s);
	assert(first != 0 && first != UINT32_MAX);

	uint32_t second = server_local_gen_id(&s);
	assert(second != 0 && second != UINT32_MAX);
	assert(second != first); /* still monotonically distinct */

	s.next_entity_id = saved; /* restore -- later tests rely on ordinary ids */

	printf("  server_local_gen_id: the wraparound guard also excludes"
		  " UINT32_MAX (painting_select_art's self-exclusion sentinel), not"
		  " just 0 OK\n");
}

/* ===================================================================== *
 * Client spawn wiring (this commit's own gap-closer): a placed painting is
 * fully live server-side already (proven above), but until now NO CRPC ever
 * told a client one exists. Three things need proving, matching the task's
 * own "assert the client entity dict + fields" shape:
 *   (a) fresh placement (server_local_spawn_painting) publishes
 *       CRPC_SPAWN_PAINTING with the exact server-assigned fields;
 *   (b) chunk-restore-on-load (server_local_restore_chunk_entities)
 *       ALSO republishes it -- the second, previously-silent call site;
 *   (c) client_interface.c's own CRPC_SPAWN_PAINTING handler builds a real
 *       client-side ENTITY_PAINTING dict entry with correct motive/size/
 *       facing (art_index/direction/tile_x/y/z) and the right render/tick
 *       wiring (render != NULL, tick_client == NULL, getBoundingBox ==
 *       NULL, matching entity_painting()'s own SP construction).
 * ===================================================================== */

static void test_client_spawn_wiring_fresh_placement(void) {
	const int TX = 90, TZ = 5; /* fresh spot, clear of every other test's own
		zone; X=90 stays inside the loaded cx<=5 chunk range and TZ=5 stays
		well clear of the cz<=1 loaded-chunk boundary (a SOUTH-facing frame's
		occupancy scan extrudes a bit past TZ in +Z -- TZ=30 originally
		reached into the unloaded cz=2 chunk, INDETERMINATE-refusing every
		art; see this test's own debug history). */
	reset_observer();
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct entity* pnt = place_painting(TX, TZ, &held);

	assert(g_obs.spawn_painting_count == 1
		  && "a fresh placement must publish EXACTLY one CRPC_SPAWN_PAINTING");
	const struct client_rpc* rpc = &g_obs.last_spawn_painting;
	assert(rpc->payload.spawn_painting.entity_id == pnt->id);
	assert(rpc->payload.spawn_painting.art_index
		  == pnt->data.painting.art_index);
	assert(rpc->payload.spawn_painting.direction
		  == pnt->data.painting.direction);
	assert(rpc->payload.spawn_painting.tile_x == pnt->data.painting.tile_x);
	assert(rpc->payload.spawn_painting.tile_y == pnt->data.painting.tile_y);
	assert(rpc->payload.spawn_painting.tile_z == pnt->data.painting.tile_z);

	printf("  client spawn wiring: fresh placement (server_local_spawn_"
		  "painting) publishes CRPC_SPAWN_PAINTING with the exact"
		  " server-assigned entity_id/art_index/direction/tile_x/y/z OK\n");
}

static void test_client_spawn_wiring_restore_on_load(void) {
	/* [740c583: EntityPainting.readEntityFromNBT] direct art lookup by
	   title, no RNG -- entity_archive.h's archived_painting_state carries
	   art_name (a title string), not an index; painting_art_by_title
	   resolves it inside server_local's own restore switch. "Sunset" (index
	   10, a 32x16 art distinct from the fresh-placement test's Kebab) so a
	   regression that silently hardcodes index 0 cannot pass both tests. */
	struct archived_entity restored;
	memset(&restored, 0, sizeof(restored));
	restored.kind = ARCHIVED_ENTITY_PAINTING;
	restored.source_id = 777001;
	restored.pos[0] = 40.5f;
	restored.pos[1] = (float)WY + 0.5f;
	restored.pos[2] = 40.9375f; /* SOUTH wall_offset-consistent, matches the
		real derivation -- irrelevant to this test's own assertions below,
		which only check the republished CRPC's tile/art/direction fields. */
	restored.data.painting.tile_x = 40;
	restored.data.painting.tile_y = WY;
	restored.data.painting.tile_z = 40;
	restored.data.painting.direction = PAINTING_DIR_SOUTH;
	strncpy(restored.data.painting.art_name, "Sunset",
		   sizeof(restored.data.painting.art_name) - 1);

	reset_observer();
	assert(server_local_restore_chunk_entities(
		&s, &(struct chunk_entities) {.entities = &restored, .count = 1}));

	assert(g_obs.spawn_painting_count == 1
		  && "chunk-restore-on-load must ALSO publish CRPC_SPAWN_PAINTING --"
			 " the second, previously-silent call site (server_local.c's"
			 " restore switch)");
	const struct client_rpc* rpc = &g_obs.last_spawn_painting;
	assert(rpc->payload.spawn_painting.art_index
		  == painting_art_by_title("Sunset"));
	assert(rpc->payload.spawn_painting.art_index == 10);
	assert(rpc->payload.spawn_painting.direction == PAINTING_DIR_SOUTH);
	assert(rpc->payload.spawn_painting.tile_x == 40);
	assert(rpc->payload.spawn_painting.tile_y == WY);
	assert(rpc->payload.spawn_painting.tile_z == 40);

	/* The restored entity keeps its OWN new runtime id (server_local_gen_id
	   remaps archived source_id -> a fresh live id -- entity_archive's own
	   documented contract); confirm the republished CRPC targets a REAL
	   live dict entry, not a stale/wrong id. */
	struct entity** live = dict_entity_get(s.entities,
		rpc->payload.spawn_painting.entity_id);
	assert(live && *live && (*live)->type == ENTITY_PAINTING);

	printf("  client spawn wiring: chunk-restore-on-load (server_local_"
		  "restore_chunk_entities) ALSO publishes CRPC_SPAWN_PAINTING,"
		  " independently of fresh placement OK\n");
}

static void test_client_entity_construction_from_spawn_rpc(void) {
	/* Bootstrap the CLIENT-side gstate exactly like test_boat_env.c's own
	   rider-tethering section does (the established precedent for this same
	   pooled-test-linking-client_interface.c shape): a fresh CRPC_WORLD_RESET
	   flips client_interface.c's internal s_world_reset_ready gate true and
	   constructs gstate.players[0].local_player, both preconditions CRPC_SPAWN_PAINTING's
	   own handler checks. */
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct client_rpc client_reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = SERVER_LOCAL_PLAYER_ENTITY_ID,
		},
	};
	clin_process(&client_reset);
	assert(gstate.players[0].local_player);

	/* A fresh server painting, art/direction/tile chosen to discriminate a
	   swapped-field regression (Wanderer is 16x32 -- NOT square, so a
	   direction<->art_index mixup or a tile_x<->tile_z swap changes the
	   derived pos in a way this test's epsilon check below would catch). */
	const int TX = 5, TZ = 3;
	struct entity* server_pnt = server_local_spawn_painting(
		&s, /*Wanderer*/ 12, PAINTING_DIR_WEST, TX, WY, TZ);
	assert(server_pnt);
	reset_observer();
	/* Republish deterministically (server_publish_painting_spawn is
	   idempotent -- no snapshot/dirty state to disturb, unlike the boat/
	   minecart) rather than relying on the ONE spawn-time send racing this
	   test's own reset_observer() call above. */
	struct client_rpc republish = {
		.type = CRPC_SPAWN_PAINTING,
		.payload.spawn_painting = {
			.entity_id = server_pnt->id,
			.art_index = server_pnt->data.painting.art_index,
			.direction = server_pnt->data.painting.direction,
			.tile_x = server_pnt->data.painting.tile_x,
			.tile_y = server_pnt->data.painting.tile_y,
			.tile_z = server_pnt->data.painting.tile_z,
		},
	};
	clin_process(&republish);

	struct entity** client_pp
		= dict_entity_get(gstate.entities, server_pnt->id);
	assert(client_pp && *client_pp
		  && "CRPC_SPAWN_PAINTING must build a real client-side dict entry");
	struct entity* client_pnt = *client_pp;

	assert(client_pnt->type == ENTITY_PAINTING);
	assert(client_pnt->data.painting.art_index == 12);
	assert(client_pnt->data.painting.direction == PAINTING_DIR_WEST);
	assert(client_pnt->data.painting.tile_x == TX);
	assert(client_pnt->data.painting.tile_y == WY);
	assert(client_pnt->data.painting.tile_z == TZ);

	/* Render/tick/collision wiring: matches entity_painting()'s own SP
	   construction exactly (server=false changes nothing about these three
	   fields -- see entity_painting.c). */
	assert(client_pnt->render != NULL
		  && "a client-visible painting must have a real render function --"
			 " this is the whole point of this slice");
	assert(client_pnt->tick_client == NULL
		  && "[740c583: EntityPainting.onUpdate is server-only] no client"
			 " tick exists, on purpose");
	assert(client_pnt->getBoundingBox == NULL
		  && "post-review findings #1/#2: a painting's own getBoundingBox"
			 " stays NULL even on the client copy");

	/* Motive/size/facing "settle by capture" per the task brief -- pixel
	   output cannot be asserted headlessly even now that a bundled CC0
	   assets/art/kz.png exists (this env test has no gfx/texture_init, so
	   texture_painting.data is never loaded here either way; see texture.c's
	   texture_painting note), but the DERIVED geometry that
	   feeds the renderer can: client_pnt->pos must match the same
	   painting_compute_geometry the SERVER side already computed (proven
	   correct by test_valid_surface_* above), confirming the client copy's
	   world-space anchor is byte-identical, not just its raw tile fields. */
	vec3 expected_pos;
	painting_compute_geometry(12, PAINTING_DIR_WEST, TX, WY, TZ, expected_pos,
							  NULL);
	assert(fabsf(client_pnt->pos[0] - expected_pos[0]) < 1.0e-5f);
	assert(fabsf(client_pnt->pos[1] - expected_pos[1]) < 1.0e-5f);
	assert(fabsf(client_pnt->pos[2] - expected_pos[2]) < 1.0e-5f);
	assert(fabsf(client_pnt->pos[0] - server_pnt->pos[0]) < 1.0e-5f
		  && fabsf(client_pnt->pos[1] - server_pnt->pos[1]) < 1.0e-5f
		  && fabsf(client_pnt->pos[2] - server_pnt->pos[2]) < 1.0e-5f
		  && "client and server copies must land on the EXACT same frame"
			 " center");

	printf("  client entity construction: CRPC_SPAWN_PAINTING builds a real"
		  " gstate.entities ENTITY_PAINTING with correct motive/direction/"
		  "tile fields, render wired, tick_client/getBoundingBox correctly"
		  " NULL, and a pos matching the server's own painting_compute_"
		  "geometry OK (render pixels themselves settle by capture --"
		  " hardware-unconfirmed by this headless env test)\n");
}

int main(void) {
	test_fixture_dir("test_painting_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	// Post-review findings #3/#8's real-transport test (test_left_click_
	// queues_attack_rpc) drives svin_rpc_send -> server_local_test_process_
	// messages end to end, which needs the svin_inbox channel actually
	// constructed (test_minecart_authority.c's harness already does this for
	// the identical minecart-onLeftClick regression test) -- without it the
	// send silently falls into the local retry queue and never reaches the
	// server, which is exactly the failure mode that test exists to catch.
	assert(svin_init());
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	clin_test_set_rpc_observer(observe_rpc, NULL);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.1.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0; // unlimited
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	/* Cover every painting scene's Beta position +/-32 entity-activation
	 * neighborhood.  Tests for unresolved frame/wall chunks explicitly evict
	 * their target chunks below before checking the tri-state behavior. */
	for(int cz = -2; cz <= 3; cz++)
		for(int cx = -2; cx <= 7; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	s.player.x = 5000.0;
	s.player.y = 5000.0;
	s.player.z = 5000.0;

	test_enum_art_table();
	test_direction_mapping();
	test_valid_surface_1x1_kebab();
	test_valid_surface_2x1_pool();
	test_valid_surface_4x4_pointer();
	test_valid_surface_top_block_in_frame();
	test_valid_surface_top_minecart_in_frame();
	test_valid_surface_top_boat_in_frame();
	test_art_selection_all_fit();
	test_art_selection_subset();
	test_art_selection_none_fit();
	test_placement_and_invalid_surface_drop();
	test_placement_rng_owner();
	test_valid_surface_bottom_painting_overlap();
	test_foreign_world_overlaps();
	test_getboundingbox_null_and_placement_unobstructed();
	test_periodic_recheck_survives_unloaded_wall_chunk();
	test_attack_pops_and_drops();
	test_left_click_queues_attack_rpc();
	test_raycast_hits_live_painting();
	test_top_check_unresolved_frame_chunk_refuses_placement();
	test_gen_id_never_returns_uint32_max();
	test_client_spawn_wiring_fresh_placement();
	test_client_spawn_wiring_restore_on_load();
	test_client_entity_construction_from_spawn_rpc();

	g_drain_run = 0;
	thread_join(&drain);

	printf("test_painting_env: ALL OK\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
