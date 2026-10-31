/*
	FAITHFULNESS GOLDEN (Beta 1.7.3): the redstone-torch inverter across three wire topologies, locking a
	verified-against-decompile behavior so a future "fix" cannot silently break parity. All drive the REAL
	production path (redstone_server.c wire flood + scheduled queue + the faithful boolean power model in
	redstone_power.c) over a live superflat server_world -- the same harness as test_redstone_repeater_
	circuit_env.c.

	The NOT-gate topologies (a lit floor torch SOURCE -> ... -> a solid block B -> a floor torch on B):
	  A  CONTINUOUS wire:           SOURCE -> wire -> wire -> wire -> B -> torch       => torch OFF (inverts)
	  B  REPEATER through a wire:    SOURCE -> wire -> REPEATER -> wire -> B -> torch    => torch LIT (no invert)

	WHY B stays LIT is FAITHFUL Beta, not a bug (adjudicated 2026-06-19 against the 740c583 decompile +
	the mc173 Rust oracle; all three agree). The torch on B turns off iff World.isBlockGettingPowered(B) is
	true, which (B opaque) reduces to the adjacent wire's BlockRedstoneWire.isPoweringTo toward B. That query
	needs the wire's BACK link flag set. A wire's back-neighbour counts as a connection only when
	isPowerProviderOrWire matches: for a wire it is always true (topology A -> inverts); for a repeater it is
	true only when `side == ModelBed.footInvisibleFaceRemap[meta&3]` (footInvisibleFaceRemap = {2,3,0,1}). The
	downstream wire probes its back with side 1, and a +X-output repeater has meta&3==1 -> remap[1]==3 != 1,
	so the repeater is NOT the wire's back link. Meanwhile the torch sitting on B forces the wire's FORWARD
	(stair-up) flag true, so the wire "points at the torch," not "into B" -> B unpowered -> torch LIT.
	(For the power MODEL a repeater placed DIRECTLY against B strong-powers B's output face -- so a repeater
	powers an adjacent block, just not through an intervening dead-end wire cell. That direct case is not
	asserted here: in the production tick path the on-top torch is not re-evaluated by the repeater's
	activation alone, a separate notify-timing question outside this faithfulness golden's scope.)

	Citations: BlockRedstoneWire.isPoweringTo / .isPowerProviderOrWire (740c583), ModelBed.footInvisibleFace
	Remap = {2,3,0,1}; CaveWii transcription redstone_power.c:56-70 (is_power_provider_or_wire) + :95-143
	(wire_is_powering_to, incl. the stair-up flag at :119-132). DO NOT "fix" topology B -- it is correct.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_torch_inverter_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // circuit plane
#define REP_META 1 // facing 1 = input -X / output +X (matches test_redstone_repeater_circuit_env REP_META)

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
	test_fixture_dir("test_rs_torch_inverter_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	// stone floor under everything
	for(int x = 2; x <= 6; x++)
		for(int z = 4; z <= 14; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	const int ZA = 5, ZB = 8, ZC = 14;

	// A) CONTINUOUS wire -> B -> torch  (expect OFF)
	set(3, Y, ZA, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, ZA, BLOCK_REDSTONE_WIRE, 0);
	set(5, Y, ZA, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, ZA, BLOCK_STONE, 0);
	set(6, Y + 1, ZA, BLOCK_REDSTONE_TORCH_LIT, 5);
	set(2, Y, ZA, BLOCK_REDSTONE_TORCH_LIT, 5); // source

	// B) REPEATER through a wire -> B -> torch  (expect LIT -- faithful: the dead-end wire cell does not
	//    register the repeater as a back-link, so it cannot point its strong power into B)
	set(3, Y, ZB, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, ZB, BLOCK_REDSTONE_REPEATER, REP_META);
	set(5, Y, ZB, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, ZB, BLOCK_STONE, 0);
	set(6, Y + 1, ZB, BLOCK_REDSTONE_TORCH_LIT, 5);
	set(2, Y, ZB, BLOCK_REDSTONE_TORCH_LIT, 5); // source

	// CONTROL: a torch on a block with no adjacent wire  (expect LIT)
	set(6, Y, ZC, BLOCK_STONE, 0);
	set(6, Y + 1, ZC, BLOCK_REDSTONE_TORCH_LIT, 5);

	for(int k = 0; k < 30; k++)
		tick();

	// --- the inverter outcomes ---
	assert(get(6, Y + 1, ZA) == BLOCK_REDSTONE_TORCH); // A: OFF (continuous wire inverts)
	assert(get(6, Y + 1, ZB) == BLOCK_REDSTONE_TORCH_LIT); // B: LIT (faithful -- no invert through the dead-end wire)
	assert(get(6, Y + 1, ZC) == BLOCK_REDSTONE_TORCH_LIT); // control: LIT

	// --- prove B's LIT torch is NOT a dead circuit: the repeater IS active and its output wire IS at full
	//     power. The torch stays lit purely from the wire-into-block geometry, not a failed flood. ---
	assert(get(4, Y, ZB) == BLOCK_REDSTONE_REPEATER_LIT); // repeater forwarded (active 94)
	assert(getmeta(5, Y, ZB) == 15); // its output wire carries full power, yet does not power B

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_torch_inverter_topologies: OK (A continuous->OFF, B repeater+wire->LIT [faithful, "
		   "repeater active + outwire=15], control->LIT)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
