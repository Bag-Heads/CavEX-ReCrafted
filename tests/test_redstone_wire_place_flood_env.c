/*
	FAITHFULNESS env test (Beta 1.7.3): a redstone DUST placed adjacent to a PRE-EXISTING power source
	must light to full strength IMMEDIATELY -- BlockRedstoneWire.onBlockAdded ->
	updateAndPropagateCurrentStrength reads the adjacent provider on placement. This drives the REAL
	player path (item_redstone.onItemPlace, the held redstone-dust placer) over a live superflat
	server_world -- the same harness as test_redstone_torch_support_drop_env.c.

	THE BUG THIS GUARDS (fixed 2026-06-25): item_redstone.onItemPlace placed the wire with a bare
	server_world_set_block, whose documented contract notifies only the 6 NEIGHBOURS -- it never runs
	the freshly placed wire's OWN flood (Beta's onBlockAdded). So a lone dust dropped beside an
	already-lit torch / lever / pressure plate stayed DARK (metadata 0) until some unrelated later
	neighbour change re-triggered a flood (e.g. placing a second dust). Two wires worked, one did not --
	the "lone wire dot does not latch a pre-existing source" quirk that test_redstone_torch_support_drop_
	env.c worked around with a 2-cell chain. The block's own .onItemPlace = redstone_wire_place DID flood,
	but it is dead for the dust path: the player holds ITEM_REDSTONE, not the wire block's block_item.

	THE FIX: item_redstone.onItemPlace now calls redstone_wire_neighbour_change(s, self) after placing --
	exactly the production equivalent of Beta updateAndPropagateCurrentStrength, identical to what
	redstone_wire_place already does. No attenuation value changes; the chain case was already correct.

	Citation (decompile 740c583): ItemRedstone.onItemUse -> World.setBlockWithNotify (which calls the
	placed block's onBlockAdded); BlockRedstoneWire.onBlockAdded -> updateAndPropagateCurrentStrength ->
	calculateCurrentChanges, where isBlockIndirectlyGettingPowered injects the flat 15 from the torch.
	Cross-checked against mc173 (block wire onBlockAdded floods on add). DO NOT regress to a bare set.

	EXTENDED (lane/items, item_redstone.c placement fix): the placement gate itself was also wrong --
	only SIDE_TOP was accepted (every other side refused outright) and the solid-below canPlaceBlockAt
	check was commented out entirely (so a wire could be placed floating over air). [decompile 740c583]
	ItemRedstone.onItemUse offsets the target by WHICHEVER side was clicked (a snow layer, Block.snow,
	is replaced in place instead of offset), then gates on
	BlockRedstoneWire.canPlaceBlockAt == isBlockNormalCube(x, y-1, z). Three more cases below: a
	SIDE_RIGHT click succeeds when the target cell has solid support; the identical click is REFUSED
	when it doesn't (the re-enabled gate); and a snow layer is replaced in place, not offset past.
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
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

// The redstone-dust item (ITEM_REDSTONE), defined in item_redstone.c and linked via _cavex_srcs.
// Forward-declared here rather than dragging in items_object.h (which would emit every static item).
extern struct item item_redstone;

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_wire_place_flood_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // circuit plane (= YF + 1, where dust lands)

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
		&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static void tick(void) { server_world_tick(&s.world, &s); }

// Place a redstone dust at (x, y, z) THROUGH THE REAL PLAYER PATH: item_redstone.onItemPlace places
// at on->(x, y+1, z) when the clicked face is SIDE_TOP, so `on` is the block directly below the target
// (the stone floor). This is what SRPC_BLOCK_PLACE dispatches for a held ITEM_REDSTONE.
static bool place_dust(int x, int y, int z) {
	struct block_data below;
	assert(server_world_get_block(&s.world, x, y - 1, z, &below));
	struct block_info on = {.block = &below, .neighbours = NULL, .x = x, .y = y - 1, .z = z};
	struct block_info where = {.block = NULL, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct item_data dust = {.id = ITEM_REDSTONE, .durability = 0, .count = 1};
	return item_redstone.onItemPlace(&s, &s.player, &dust, &where, &on, SIDE_TOP);
}

int main(void) {
	test_fixture_dir("test_rs_wire_place_flood_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);	// the wire-flood drain can notify components; keep the harness whole
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc); // x<=6, z<=8 all fit chunk (0,0)

	// stone floor under everything (widened to x<=10 for the side-placement/snow cases below;
	// still all inside chunk (0,0))
	for(int x = 1; x <= 10; x++)
		for(int z = 4; z <= 8; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	const int ZT = 5; // torch row: a lone dust placed beside an ALREADY-lit torch
	const int ZN = 8; // negative-control row: a lone dust with NO adjacent source

	// A LIT floor torch (meta 5 -> mounts on the stone below) is a power SOURCE for adjacent wire.
	set(2, Y, ZT, BLOCK_REDSTONE_TORCH_LIT, 5); // source, mount = (2, YF, ZT)

	// THE REGRESSION: a SINGLE redstone dust placed next to that torch, via the real item path.
	assert(place_dust(3, Y, ZT));
	// Beta onBlockAdded floods ON PLACEMENT -- the dust latches the torch's 15 BEFORE any tick runs.
	assert(getmeta(3, Y, ZT) == 15); // <-- pre-fix this read 0 (bare set, no self-flood)

	// NEGATIVE CONTROL: a lone dust with no power source nearby must stay 0. Proves the flood actually
	// COMPUTES strength (it is not a hack that blindly writes 15 on every placement).
	assert(place_dust(3, Y, ZN));
	assert(getmeta(3, Y, ZN) == 0);

	// SIDE PLACEMENT: a SIDE_RIGHT click on a stone pillar's east face offsets the target to
	// (9, Y, ZS) [decompile 740c583: var7's per-side var4/var5/var6 adjustment] -- the old
	// on_side != SIDE_TOP gate refused this outright.
	const int ZS = 6;
	set(8, Y, ZS, BLOCK_STONE, 0); // the clicked pillar
	{
		struct block_data pillar;
		assert(server_world_get_block(&s.world, 8, Y, ZS, &pillar));
		struct block_info on
			= {.block = &pillar, .neighbours = NULL, .x = 8, .y = Y, .z = ZS};
		struct block_info where
			= {.block = NULL, .neighbours = NULL, .x = 9, .y = Y, .z = ZS};
		struct item_data dust = {.id = ITEM_REDSTONE, .durability = 0, .count = 1};
		assert(item_redstone.onItemPlace(&s, &s.player, &dust, &where, &on, SIDE_RIGHT));
	}
	assert(get(9, Y, ZS) == BLOCK_REDSTONE_WIRE);

	// REFUSAL: the identical SIDE_RIGHT click, but the target cell has NOTHING solid below it
	// (an isolated pillar with no floor) -- must be refused, not placed as a floating wire.
	// [decompile 740c583] BlockRedstoneWire.canPlaceBlockAt: isBlockNormalCube(x, y-1, z) --
	// this gate was commented out in the pre-fix code.
	const int ZF = 12; // no floor on this row
	set(8, Y, ZF, BLOCK_STONE, 0); // isolated pillar
	{
		struct block_data pillar;
		assert(server_world_get_block(&s.world, 8, Y, ZF, &pillar));
		struct block_info on
			= {.block = &pillar, .neighbours = NULL, .x = 8, .y = Y, .z = ZF};
		struct block_info where
			= {.block = NULL, .neighbours = NULL, .x = 9, .y = Y, .z = ZF};
		struct item_data dust = {.id = ITEM_REDSTONE, .durability = 0, .count = 1};
		assert(!item_redstone.onItemPlace(&s, &s.player, &dust, &where, &on, SIDE_RIGHT));
	}
	assert(get(9, Y, ZF) != BLOCK_REDSTONE_WIRE);

	// SNOW REPLACE: clicking a snow layer replaces it IN PLACE, not offset past it.
	// [decompile 740c583] `if (world.getBlockId(x,y,z) != Block.snow.blockID) { ...offset... }`
	set(10, Y, ZS, BLOCK_SNOW, 0); // sits on the stone floor at (10, YF, ZS)
	{
		struct block_data snow;
		assert(server_world_get_block(&s.world, 10, Y, ZS, &snow));
		struct block_info on
			= {.block = &snow, .neighbours = NULL, .x = 10, .y = Y, .z = ZS};
		// `where` would be the TOP offset if this were treated like any other block --
		// the snow branch must ignore it and use `on`'s own cell instead.
		struct block_info where
			= {.block = NULL, .neighbours = NULL, .x = 10, .y = Y + 1, .z = ZS};
		struct item_data dust = {.id = ITEM_REDSTONE, .durability = 0, .count = 1};
		assert(item_redstone.onItemPlace(&s, &s.player, &dust, &where, &on, SIDE_TOP));
	}
	assert(get(10, Y, ZS) == BLOCK_REDSTONE_WIRE); // replaced the snow cell itself
	assert(get(10, Y + 1, ZS) != BLOCK_REDSTONE_WIRE); // NOT placed one cell above it

	for(int k = 0; k < 30; k++)
		tick();

	// Stable after settling: torch still lit on its intact mount, powered dust still 15, lone dust still 0.
	assert(get(2, Y, ZT) == BLOCK_REDSTONE_TORCH_LIT);
	assert(getmeta(3, Y, ZT) == 15);
	assert(getmeta(3, Y, ZN) == 0);
	assert(get(9, Y, ZS) == BLOCK_REDSTONE_WIRE);
	assert(get(9, Y, ZF) != BLOCK_REDSTONE_WIRE);
	assert(get(10, Y, ZS) == BLOCK_REDSTONE_WIRE);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_wire_place_flood_env: OK (lone dust beside a lit torch latches 15 on "
		   "placement; no-source dust stays 0; SIDE_RIGHT placement + its solid-below refusal; "
		   "snow-layer replace-in-place)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
