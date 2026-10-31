/*
	E2E production-path test for a wire-driven redstone REPEATER (M4 step 6), guarding the
	critical scheduling fixes the adversarial review found in the repeater production wiring.

	Drives a REAL circuit -- a lit redstone torch source -> an 8-cell redstone-wire run -> into
	the back of a repeater (delay setting 2 = 4 game-ticks) -- through the production
	redstone_server.c wire flood + the scheduled-tick queue + server_world_tick, over a real
	superflat server_world. Unlike the stub-grid goldens (which cover only the pure logic), this
	exercises redstone_wire_neighbour_change + rs_mark_visit + the GAP #1 engine guard end-to-end.

	WHAT IT CATCHES:
	  - the wire-driven delay must be the METADATA delay (rises at +4 ticks), NOT the collapsed
	    delay-1 the bug produced (rs_mark_visit armed the toggle via onWorldTick at delay 1, and
	    the GAP #1 auto-schedule would have pre-empted it the same way);
	  - a stable idle repeater must NOT spuriously toggle while its input is unchanged;
	  - both edges go through the production wire flood + the deferred blocksNeedingUpdate drain.

	HEADLESS HARNESS: server_world_set_block emits CRPC_SET_BLOCK to the client, and clin_rpc_send
	BLOCKS once the 16-token inbox fills. A dedicated drain thread spins clin_drain_discard (the
	client's consumer role -- recycle tokens, no processing) so the server-thread sends never block.
	The test reads the SERVER world synchronously (server_world_get_block); the drained RPCs are
	irrelevant.
*/

#include <assert.h>
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
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_redstone_repeater_circuit_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // circuit plane
#define Z  8
#define WX0 3   // first wire cell
#define WLEN 8  // wire x = 3..10
#define RX 11   // repeater x (reads wire x=10 on its -X/back face)
#define SX 2    // source torch x
// repeater metadata: facing 1 (input from -X, output +X) | delay index 1 (<<2) = {2,4,6,8}[1] = 4
#define REP_META (1 | (1 << 2))
#define REP_DELAY 4

static struct server_local s;

// --- RPC drain thread: keep the client inbox empty so server-thread set_block never blocks ----
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard(); // final sweep
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {.type = (uint8_t)type,
													   .metadata = (uint8_t)(meta & 0x0F),
													   .sky_light = 0,
													   .torch_light = 0}));
}
static int rep_id(void) {
	struct block_data b;
	return server_world_get_block(&s.world, RX, Y, Z, &b) ? b.type : -1;
}
static void tick(void) {
	server_world_tick(&s.world, &s);
}

int main(void) {
	test_fixture_dir("test_redstone_repeater_circuit_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	s.scheduled_tick_budget = 0; // unlimited: scheduled ticks fire exactly when due

	/* Beta scheduleBlockUpdate admits a tick only when its inclusive x/z +/-8
	 * checkChunksExist box is resident. The circuit itself fits (0,0), but its
	 * deferred repeater edge therefore requires the surrounding 3x3. */
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	// --- build (high above the superflat surface): floor, repeater, wire run -----------------
	for(int x = SX; x <= RX + 1; x++)
		set(x, YF, Z, BLOCK_STONE, 0);					   // normal-cube floor under everything
	set(RX, Y, Z, BLOCK_REDSTONE_REPEATER, REP_META);	   // idle, facing back at the wire, delay 4
	for(int i = 0; i < WLEN; i++)
		set(WX0 + i, Y, Z, BLOCK_REDSTONE_WIRE, 0);		   // wire x=3..10, settles 0 (no source yet)

	// Stable initial state: repeater idle, input off; must not spuriously toggle.
	assert(rep_id() == BLOCK_REDSTONE_REPEATER);
	for(int k = 0; k < 6; k++) {
		tick();
		assert(rep_id() == BLOCK_REDSTONE_REPEATER);
	}

	// --- RISING edge: a lit torch powers the wire -> repeater turns ON at EXACTLY +4 ----------
	set(SX, Y, Z, BLOCK_REDSTONE_TORCH_LIT, 5); // floor torch powers +X into wire x=3
	for(int k = 0; k < REP_DELAY - 1; k++) {
		tick();
		// still idle every tick before the metadata delay -- a delay-1 collapse would fire here
		assert(rep_id() == BLOCK_REDSTONE_REPEATER);
	}
	tick();
	assert(rep_id() == BLOCK_REDSTONE_REPEATER_LIT); // ON at exactly +4, not +1, not instant

	// settle: stays active, no oscillation
	for(int k = 0; k < 6; k++) {
		tick();
		assert(rep_id() == BLOCK_REDSTONE_REPEATER_LIT);
	}

	// --- FALLING edge: cut the source -> repeater turns OFF at +4, then stays off (no phantom) -
	set(SX, Y, Z, BLOCK_AIR, 0);
	for(int k = 0; k < REP_DELAY - 1; k++) {
		tick();
		assert(rep_id() == BLOCK_REDSTONE_REPEATER_LIT); // still active before the delay
	}
	tick();
	assert(rep_id() == BLOCK_REDSTONE_REPEATER); // OFF at exactly +4

	for(int k = 0; k < 8; k++) {
		tick();
		assert(rep_id() == BLOCK_REDSTONE_REPEATER); // stays idle -- no phantom re-activation
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_repeater_circuit_env: OK (wire->repeater rises at +%d not +1, falls at "
		   "+%d, no spurious/phantom toggle -- production wire flood + scheduled queue)\n",
		   REP_DELAY, REP_DELAY);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
