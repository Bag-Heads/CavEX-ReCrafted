/*
	Golden behavior test for the redstone-torch BURNOUT list under SATURATION (M4 redstone).

	Beta's BlockRedstoneTorch.torchUpdates is an UNBOUNDED ArrayList. On the fixed-RAM Wii the
	port caps it at RS_BURNOUT_MAX entries (a DOCUMENTED bounded-memory deviation, see
	docs/redstone_plan.md). The bug this pins: the original code SILENTLY DROPPED a new lit->idle
	record once the list was full (`if (record && n < MAX)`), so a real torch clock could fail to
	reach its >=8 burnout threshold and never burn out -- a simulation-output divergence from Beta.

	The fix evicts the OLDEST entry (FIFO) before appending when full, so a NEW record is never
	dropped and the most-recent RS_BURNOUT_MAX records (hence each live clock's per-position count)
	are preserved. This test floods the shared list to capacity with records at DISTINCT dummy
	positions, then drives ONE target torch through 8 lit->idle toggles and asserts it STILL burns
	out (relight blocked). Under the old silent-drop behaviour the 8 target records would be
	discarded, the target count would stay 0, and the torch would wrongly relight -- so this
	RED-fails the pre-fix code and GREEN-passes the eviction fix.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "block/redstone_logic.h"

#define TY 64
#define TZ 5
#define TMETA 5			// floor mount: attachment is the block at (x, y-1, z), side 0
#define NPOS 400		// > RS_BURNOUT_MAX (256); torch cells live at x in [0, NPOS)
#define TARGET_X 5		// the torch we drive to burnout
#define DUMMY_X0 100	// dummy fillers occupy x = 100 .. 100+RS_BURNOUT_MAX-1 (disjoint from target)

static int g_id[NPOS];
static bool g_input[NPOS];
static int64_t g_time;

static bool is_torch(int x, int y, int z) {
	return y == TY && z == TZ && x >= 0 && x < NPOS;
}
static int t_get_type(void* c, int x, int y, int z) {
	(void)c;
	return is_torch(x, y, z) ? g_id[x] : 0;
}
static int t_get_meta(void* c, int x, int y, int z) {
	(void)c;
	return is_torch(x, y, z) ? TMETA : 0;
}
static void t_set_type(void* c, int x, int y, int z, int type) {
	(void)c;
	if(is_torch(x, y, z))
		g_id[x] = type;
}
// attachment of a meta-5 torch is (x, y-1, z), queried with side 0 (see torch_attachment_powered)
static bool t_is_ipp(void* c, int x, int y, int z, int side) {
	(void)c;
	(void)side;
	return (y == TY - 1 && z == TZ && x >= 0 && x < NPOS) ? g_input[x] : false;
}
static int64_t t_world_time(void* c) {
	(void)c;
	return g_time;
}

static struct redstone_world RS;

// Force one lit->idle record at position x: make it LIT with its input ON, then tick. The inverter
// turns it idle and records the toggle into the burnout list.
static void record_one(int x) {
	g_id[x] = REDSTONE_TORCH_LIT_ID;
	g_input[x] = true;
	redstone_torch_update(&RS, x, TY, TZ);
	assert(g_id[x] == REDSTONE_TORCH_IDLE_ID); // it toggled off (and recorded)
}

int main(void) {
	redstone_torch_reset_burnout();
	RS = (struct redstone_world) {
		.get_type = t_get_type,
		.get_meta = t_get_meta,
		.set_type = t_set_type,
		.is_indirectly_providing_power = t_is_ipp,
		.get_world_time = t_world_time,
	};
	g_time = 0; // keep all records inside one 100-tick window (no prune) so saturation is real

	// Saturate the shared burnout list well past capacity (RS_BURNOUT_MAX is internal to
	// redstone_logic.c; 4096 records comfortably exceed the documented 256-entry cap, so the list
	// is definitely full and eviction is exercised, while staying robust if the cap is later
	// bumped -- the assertion below holds either way). Dummies cycle over 64 distinct positions.
	for(int i = 0; i < 4096; i++)
		record_one(DUMMY_X0 + (i & 63));

	// Now drive the TARGET torch through 8 lit->idle toggles. Each must still be recorded (evicting
	// an oldest dummy), NOT silently dropped. After 8 records at the target position, a relight
	// attempt must see >= 8 entries there and stay dark.
	for(int i = 0; i < 8; i++)
		record_one(TARGET_X);

	// Relight attempt: input OFF, torch currently idle -> would relight UNLESS burnt out.
	g_input[TARGET_X] = false;
	redstone_torch_update(&RS, TARGET_X, TY, TZ);
	assert(g_id[TARGET_X] == REDSTONE_TORCH_IDLE_ID); // burnt out: stays dark (records were kept)

	// Sanity: after the 100-tick window clears, the target may relight again (entries age out).
	g_time += 200;
	g_input[TARGET_X] = false;
	redstone_torch_update(&RS, TARGET_X, TY, TZ);
	assert(g_id[TARGET_X] == REDSTONE_TORCH_LIT_ID); // recovered once the window cleared

	printf("test_redstone_torch_burnout_saturation: OK (saturated list evicts oldest, never drops "
		   "a new record; target still burns out under a full-list load)\n");
	return 0;
}
