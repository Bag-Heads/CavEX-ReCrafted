/*
	FAITHFULNESS env test (Beta 1.7.3): BlockTorch/BlockRedstoneTorch.onNeighborBlockChange support-drop.
	A redstone torch whose mounting block is removed must DROP (become air) -- otherwise it floats AND
	lingers as a PHANTOM POWER SOURCE, keeping adjacent wire energised forever. This drives the REAL
	production path (server_world_set_block -> server_world_notify_block -> the torch's
	onNeighbourBlockChange -> support-drop) over a live superflat server_world -- the same harness as
	test_redstone_torch_inverter_topologies.c.

	The subtlety this guards: the redstone torch defines onWorldTick (the inverter), so the engine's
	constant-tick_delay auto-schedule is SUPPRESSED for it once it ALSO defines onNeighbourBlockChange
	(server_world_notify_block: no block may set both). block_torch.c's handler therefore re-arms the
	inverter itself when the torch is still supported. The inverter staying intact is locked by
	test_redstone_torch_inverter_topologies / _2tick / _burnout; THIS test locks the new drop + the
	"phantom power gone" corollary, and (via the CONTROL row) that the drop is targeted, not a wipe.

	Citation: BlockTorch.onNeighborBlockChange -> canPlaceBlockOnSide false -> dropBlockAsItem (740c583);
	the support cell is metadata-driven (1->-X 2->+X 3->-Z 4->+Z 5->below), same map as block_lever.c.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_torch_supportdrop_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // circuit plane

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

int main(void) {
	test_fixture_dir("test_rs_torch_supportdrop_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	rand_gen_seed(&s.rand_src);	   // the support-drop spawns a dropped-item entity (needs RNG + the dict)
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc); // x<=8, z<=8 all fit chunk (0,0)

	// stone floor under everything
	for(int x = 1; x <= 8; x++)
		for(int z = 4; z <= 8; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	const int ZT = 5; // the torch-under-test row (its mount is removed)
	const int ZC = 8; // the control row (mount kept intact)

	// A LIT floor redstone torch is a power SOURCE for adjacent wire (the inverter test's source
	// pattern). meta 5 = floor torch -> mounts on the block BELOW = the stone floor (x, YF, z).
	set(2, Y, ZT, BLOCK_REDSTONE_TORCH_LIT, 5); // source torch, mount = (2, YF, ZT)
	set(3, Y, ZT, BLOCK_REDSTONE_WIRE, 0);		// wire it powers
	set(4, Y, ZT, BLOCK_REDSTONE_WIRE, 0);

	// CONTROL: an identical torch+wire-chain whose mount we will NOT remove -- proves the drop is
	// targeted. (Uses a 2-cell chain to match the under-test row: a lone wire dot adjacent to an
	// already-placed source does not latch its power in this flood model, a separate pre-existing
	// wire quirk unrelated to the torch support-drop under test here.)
	set(2, Y, ZC, BLOCK_REDSTONE_TORCH_LIT, 5);
	set(3, Y, ZC, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, ZC, BLOCK_REDSTONE_WIRE, 0);

	for(int k = 0; k < 30; k++)
		tick();

	// Baseline: both torches lit on their stone mounts, both wires energised by them.
	assert(get(2, Y, ZT) == BLOCK_REDSTONE_TORCH_LIT);
	assert(getmeta(3, Y, ZT) > 0); // wire powered by the source torch (presence of power is the point)
	assert(get(2, Y, ZC) == BLOCK_REDSTONE_TORCH_LIT);
	assert(getmeta(3, Y, ZC) > 0);

	// Remove the under-test torch's MOUNT (the floor block directly beneath it). set_block -> notify
	// -> the torch (the +Y neighbour of the removed floor) gets onNeighbourBlockChange -> its
	// mount-below is now air (not a normal cube) -> support-drop.
	set(2, YF, ZT, BLOCK_AIR, 0);
	for(int k = 0; k < 30; k++)
		tick();

	// The torch DROPPED (now air) and its phantom power is GONE (the wire fell back to 0).
	assert(get(2, Y, ZT) == BLOCK_AIR); // support-drop fired -- no floating torch
	assert(getmeta(3, Y, ZT) == 0);		// no phantom power -- wire de-energised

	// The CONTROL torch (mount intact) is untouched and still powering its wire -- the drop is targeted.
	assert(get(2, Y, ZC) == BLOCK_REDSTONE_TORCH_LIT);
	assert(getmeta(3, Y, ZC) > 0);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_torch_support_drop_env: OK (mount removed -> torch dropped + phantom power "
		   "gone; control torch on intact mount still powering)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
