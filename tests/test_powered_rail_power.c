/*
	Golden test for POWERED-RAIL power state (handoff 2026-07-29 P1(a)).

	SYMPTOM on hardware: the powered-rail texture never changed when powered, and the
	maintainer's looping cart ran forever -- which he read as "it must be powered". It was
	not: the boost fired on block TYPE alone, so an UNPOWERED rail was accelerating it.

	FOUR coupled defects, all pinned here or in test_minecart_place.c:
	 (1) nothing ever SET metadata bit 0x8 -- no redstone->rail powering code existed;
	 (2) two paths DESTROYED the bit: `metadata` is a 4-bit field, so the
	     `(metadata & ~0xF) | newMeta` idiom in the shape rewrites was `0 | newMeta`;
	 (3) the minecart boost ignored the bit and Beta's BRAKE was missing entirely
	     (test_minecart_place.c pins those);
	 (4) calcRailMeta handed powered/detector rails curve metas 6-9 -- and 8/9 CARRY bit
	     0x8, so a corner powered rail rendered POWERED_ON with nothing powering it.

	Beta ground truth, all verified verbatim at 740c583 this session:
	  BlockRail.java:117-134  the power setter (direct power at cell or cell above, OR the
	                          chain walk both directions; writes only on state change)
	  BlockRail.java:148-244  isNeighborRailPowered/isRailPassingPower (depth cap 8 ->
	                          the classic 9-rail powered run per source)
	  BlockRail.java:50-60    ON texture = base 179, OFF = -16 = 163; the swap is guarded
	                          by blockID == railPowered so the DETECTOR keeps ONE sprite
	  RailLogic.java:174/248/275  curves only ever assigned if !isPoweredRail (detector
	                          included via the ctor flag)

	The fixture drives the REAL server: blocks placed via server_world_set_block, whose
	six-neighbour notify runs the production onNeighbourBlockChange cascade -- so bit 8
	propagation happens the way it does in game, not by hand-calling the handler.
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
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_powered_rail_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 64          // stone floor
#define RY (YF + 1)    // rail level

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
	struct block_data b = {.type = (uint8_t)type, .metadata = (uint8_t)meta};
	if(server_world_set_block(&s.worlds[0], x, y, z, b))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == b.type && live.metadata == b.metadata);
}

static uint8_t meta_at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b.metadata;
}

static uint8_t tex_of(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	struct block_info info = {.block = &b, .neighbours = NULL, .x = x, .y = y, .z = z};
	return blocks[b.type]->getTextureIndex(&info, SIDE_TOP);
}

static void floor_at(int x, int z) { set(x, YF, z, BLOCK_STONE, 0); }

// --- (1)+(2) direct power sets the bit; texture flips; removal clears it -------------
static void check_direct_power(void) {
	floor_at(10, 10);
	set(10, RY, 10, BLOCK_POWERED_RAIL, 0);
	assert(!(meta_at(10, RY, 10) & 0x8));
	assert(tex_of(10, RY, 10) == tex_atlas_lookup(TEXAT_RAIL_POWERED_OFF));

	// A lit redstone torch beside the rail. Placing it dispatches the six-neighbour
	// notify, which runs the rail's production onNeighbourBlockChange -> refresh.
	floor_at(11, 10);
	set(11, RY, 10, BLOCK_REDSTONE_TORCH_LIT, 5);
	assert((meta_at(10, RY, 10) & 0x8)
		   && "a lit torch beside a powered rail must set bit 8 -- the texture the "
			  "maintainer never saw change");
	assert(tex_of(10, RY, 10) == tex_atlas_lookup(TEXAT_RAIL_POWERED_ON));

	// Removing the source clears it again (BlockRail.java:125-127 writes meta & 7).
	set(11, RY, 10, BLOCK_AIR, 0);
	assert(!(meta_at(10, RY, 10) & 0x8) && "power-off must clear bit 8");
	assert(tex_of(10, RY, 10) == tex_atlas_lookup(TEXAT_RAIL_POWERED_OFF));
	printf("  [1] direct power sets/clears bit 8 and flips the ON/OFF texture\n");
}

// --- (3) a shape recompute must NOT wipe the bit (the & ~0xF defect) ------------------
static void check_shape_rewrite_preserves_bit(void) {
	// Re-power the rail, then force calcRailMeta by dropping a second rail beside it:
	// the shape recompute used to run `(metadata & ~0xF) | newMeta` = `0 | newMeta`.
	floor_at(11, 10);
	set(11, RY, 10, BLOCK_REDSTONE_TORCH_LIT, 5);
	assert(meta_at(10, RY, 10) & 0x8);
	floor_at(10, 11);
	set(10, RY, 11, BLOCK_RAIL, 0);
	assert((meta_at(10, RY, 10) & 0x8)
		   && "a neighbour-driven shape recompute wiped bit 8 -- the 4-bit-field "
			  "& ~0xF regression");
	// HONESTY NOTE (fail-before finding): this end-to-end assertion survives a revert of
	// the preservation line alone, because powered_rail_refresh runs in the same handler
	// and re-derives the bit from the still-present source -- the wipe self-heals here.
	// The preservation is kept as Beta's literal form (RailLogic `& 8 | shape`) and as
	// defense for TRANSIENT cascade states (a wiped bit mid-cascade is visible to
	// rail_passing_power and to the client via CRPC_SET_BLOCK), which no deterministic
	// single-thread fixture can pin. What IS independently failable: check [1] (the
	// refresh itself) and check [4] (the chain), both verified red with defect (1)
	// reverted.
	set(10, RY, 11, BLOCK_AIR, 0);
	set(11, RY, 10, BLOCK_AIR, 0);
	assert(!(meta_at(10, RY, 10) & 0x8));
	printf("  [2] shape recomputes preserve the powered flag\n");
}

// --- (4) powered/detector rails never take curve shapes -------------------------------
static void check_no_curves_on_powerable(void) {
	// An L of powered rails: neighbours to the south and east would give a plain rail
	// curve meta 6 (SE) -- which carries NO bit; but a corner assignment of 8/9 would
	// fake POWERED_ON. Beta never assigns curves to powerable rails at all.
	for(int x = 20; x <= 21; x++)
		for(int z = 20; z <= 21; z++)
			floor_at(x, z);
	set(20, RY, 20, BLOCK_POWERED_RAIL, 0);
	set(20, RY, 21, BLOCK_POWERED_RAIL, 0);
	set(21, RY, 20, BLOCK_POWERED_RAIL, 0);
	uint8_t m = meta_at(20, RY, 20);
	assert((m & 0x7) <= 5 && !(m & 0x8)
		   && "a cornered POWERED rail took a curve/fake-powered meta -- Beta clamps "
			  "powerable rails to shapes 0-5 (RailLogic curve gate)");
	assert(tex_of(20, RY, 20) == tex_atlas_lookup(TEXAT_RAIL_POWERED_OFF));

	set(20, RY, 20, BLOCK_DETECTOR_RAIL, 0);
	m = meta_at(20, RY, 20);
	assert((m & 0x7) <= 5 && !(m & 0x8) && "same clamp for the detector rail");
	// The detector's SINGLE texture is Beta-correct, with and without bit 8.
	assert(tex_of(20, RY, 20) == tex_atlas_lookup(TEXAT_RAIL_DETECTOR));
	set(20, RY, 20, (int)BLOCK_DETECTOR_RAIL, 0x8 | (m & 0x7));
	assert(tex_of(20, RY, 20) == tex_atlas_lookup(TEXAT_RAIL_DETECTOR)
		   && "1.7.3 ships no second detector sprite (BlockRail.java:52 guard)");
	for(int x = 20; x <= 21; x++)
		for(int z = 20; z <= 21; z++)
			set(x, RY, z, BLOCK_AIR, 0);
	printf("  [3] powerable rails clamp to shapes 0-5; detector keeps its one sprite\n");
}

// --- (5) the chain walk: 9 lit from one source, the 10th dark -------------------------
static void check_chain_walk(void) {
	// An EW line of 11 powered rails at x=30..40; the torch sits beside x=30. Beta's
	// depth-8 walk powers the directly-powered rail + 8 more (BlockRail.java:149-151),
	// so x=30..38 light and x=39..40 stay dark.
	for(int x = 29; x <= 40; x++)
		floor_at(x, 30);
	for(int x = 30; x <= 40; x++)
		set(x, RY, 30, BLOCK_POWERED_RAIL, 1);
	floor_at(29, 30);
	set(29, RY, 30, BLOCK_REDSTONE_TORCH_LIT, 5);

	for(int x = 30; x <= 38; x++)
		assert((meta_at(x, RY, 30) & 0x8)
			   && "a rail within 9 of the source must light via the chain walk");
	for(int x = 39; x <= 40; x++)
		assert(!(meta_at(x, RY, 30) & 0x8)
			   && "the 10th rail must stay dark -- the depth-8 cap");

	// Power off: the whole chain must go dark again.
	set(29, RY, 30, BLOCK_AIR, 0);
	for(int x = 30; x <= 40; x++)
		assert(!(meta_at(x, RY, 30) & 0x8) && "power-off must propagate down the chain");
	printf("  [4] chain walk: 9 rails light per source, 10th dark; off propagates\n");
}

int main(void) {
	test_fixture_dir("test_powered_rail_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_powered_rail_power: Beta 1.7.3 powered-rail power state (P1(a))\n");
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 2; cx++)
		for(int cz = 0; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	check_direct_power();
	check_shape_rewrite_preserves_bit();
	check_no_curves_on_powerable();
	check_chain_walk();

	g_drain_run = 0;
	printf("test_powered_rail_power: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
