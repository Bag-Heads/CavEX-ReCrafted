/*
	PAR-WIRE-REMOVAL-ORDER golden. Beta BlockRedstoneWire.onBlockRemoval (src_original :222-257)
	notifies above (:225), below (:226), then RECALCULATES AT THE REMOVED CELL (:227,
	updateAndPropagateCurrentStrength) before the horizontal notifyWireNeighborsOfNeighborChange
	(:228-254). Chunk.setBlockID (:278-289) calls onBlockRemoval with the new id but the OLD
	metadata nibble, so func_21030_a (:57-171) reads the old strength; a new strength
	(:61-106) that differs sets the nibble (editingBlocks: notifies suppressed, World.java:508)
	and, when old == 0 || new <= 1 (:160), adds the cell + six neighbours (:161-167), drained
	through notifyBlocksOfNeighborChange (:50-53, World.java:498-505).

	A: lone wire on a support cube strongly powered by a lit torch below it (torch isIndirectly-
	   PoweringTo side 0, BlockRedstoneTorch.java:119-121) = 15. Mine the support: the wire pops,
	   :227 gives old 15 -> new 0 (no power, no wire neighbours) -> 7 positions = 42 drain
	   visits (checked as a MULTISET: Beta drains a HashSet copy, :47-53, so its iteration
	   order is not insertion order; the port's first-insertion drain is a documented
	   deviation, redstone_server.c blocksNeedingUpdate note). An idle torch at wire+2x (reached ONLY by that drain) is notified ->
	   scheduleBlockUpdate(2) (:114-117) -> updateTick lights it (:105-109).
	B: same support under the head of a 15/14/13 line. :227 gives 15 -> 13 (W1 14 - 1): no cell
	   mark (gate false), line settles to air/0/0; an idle torch reached only by a cell mark
	   stays idle (negative control). ORDER: every drain visit of the :227 flood (the line's
	   own marks) precedes the first horizontal notify reaching the remaining wires (:228-254);
	   observed by wrapping the wire's onNeighbourBlockChange (drain visits skip wire cells).
*/
#include <assert.h>
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
#include "block/redstone_server.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_dir[128];
static struct server_local s;
static volatile int g_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
static void put(int x, int y, int z, int type, int meta, bool silent) {
	struct block_data b = {.type = (uint8_t)type, .metadata = (uint8_t)meta};
	assert(silent ? server_world_set_block_silent(&s.worlds[0], x, y, z, b)
				  : server_world_set_block(&s.worlds[0], x, y, z, b));
}
static int get(int x, int y, int z, bool meta) {
	struct block_data b;
	if(!server_world_get_block(&s.worlds[0], x, y, z, &b))
		return -1;
	return meta ? (b.metadata & 0x0F) : b.type;
}
// One event log: 'D' = drain visit (redstone_server visit hook), 'W' = a wire at z == 9, x >= 5
// (case B's remaining line) receiving onNeighbourBlockChange.
static struct {
	char k;
	int x, y, z;
} g_ev[256];
static int g_nev;
static void ev(char k, int x, int y, int z) {
	if(g_nev < 256)
		g_ev[g_nev].k = k, g_ev[g_nev].x = x, g_ev[g_nev].y = y, g_ev[g_nev].z = z;
	g_nev++;
}
static void on_visit(int x, int y, int z) { ev('D', x, y, z); }
static struct block s_wire_logged;
static void (*s_wire_nbc)(struct server_local*, struct block_info*);
static void wire_nbc_logged(struct server_local* sl, struct block_info* info) {
	if(info->z == 9 && info->x >= 5)
		ev('W', info->x, info->y, info->z);
	s_wire_nbc(sl, info);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); // keep the diagnostic lines on an assert abort
	test_fixture_dir("test_rs_wire_rmorder_tmp", s_dir, sizeof s_dir);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(s_dir);
	MKDIR(test_fixture_join(s_dir, "/region"));
	remove(test_fixture_join(s_dir, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_dir);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	for(int cx = -1; cx <= 1; cx++) // World.TickUpdates runs a tick only if its +/-8 box exists
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}

	for(int z = 5; z <= 9; z += 4) { // torch(100) under support(101) under wire(102)
		put(4, 99, z, BLOCK_STONE, 0, false);
		put(4, 100, z, BLOCK_REDSTONE_TORCH_LIT, 5, false);
		put(4, 101, z, BLOCK_STONE, 0, false);
		put(4, 102, z, BLOCK_REDSTONE_WIRE, 0, false);
	}
	for(int x = 5; x <= 6; x++) { // B: the 14/13 tail
		put(x, 101, 9, BLOCK_STONE, 0, false);
		put(x, 102, 9, BLOCK_REDSTONE_WIRE, 0, false);
	}
	put(6, 101, 5, BLOCK_STONE, 0, false);	// A: BUD torch mount (wire + 2x)
	put(4, 101, 11, BLOCK_STONE, 0, false); // B: BUD torch mount (wire + 2z)
	for(int k = 0; k < 30; k++)
		server_world_tick(&s.worlds[0], &s);
	assert(get(4, 102, 5, true) == 15);
	assert(get(4, 102, 9, true) == 15 && get(5, 102, 9, true) == 14 && get(6, 102, 9, true) == 13);
	// Stale idle torches on unpowered mounts: they light only if something notifies them.
	put(6, 102, 5, BLOCK_REDSTONE_TORCH, 5, true);
	put(4, 102, 11, BLOCK_REDSTONE_TORCH, 5, true);

	redstone_server_set_visit_hook(on_visit);
	put(4, 101, 5, BLOCK_AIR, 0, false); // A: mine the powered support
	redstone_server_set_visit_hook(NULL);
	printf("A: wire=%d drain visits=%d\n", get(4, 102, 5, false), g_nev);
	assert(get(4, 102, 5, false) == BLOCK_AIR);
	assert(g_nev == 42); // :161-167 seven positions x World.java:498-505 six faces
	static const int P[7][3] = {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
								{0, 1, 0}, {0, 0, -1}, {0, 0, 1}}; // :161-167
	bool used[42] = {false};
	for(int p = 0; p < 7; p++) // multiset equality: each expected visit consumed exactly once
		for(int d = 1; d < 7; d++) {
			const int ex = 4 + P[p][0] + P[d][0], ey = 102 + P[p][1] + P[d][1],
					  ez = 5 + P[p][2] + P[d][2];
			int i = 0;
			while(i < 42 && (used[i] || g_ev[i].x != ex || g_ev[i].y != ey || g_ev[i].z != ez))
				i++;
			assert(i < 42);
			used[i] = true;
		}

	g_nev = 0; // B: mine the powered support under the line head, logging wire notifies too
	s_wire_logged = *blocks[BLOCK_REDSTONE_WIRE];
	s_wire_nbc = s_wire_logged.onNeighbourBlockChange;
	s_wire_logged.onNeighbourBlockChange = wire_nbc_logged;
	struct block* wire_orig = blocks[BLOCK_REDSTONE_WIRE];
	blocks[BLOCK_REDSTONE_WIRE] = &s_wire_logged;
	redstone_server_set_visit_hook(on_visit);
	put(4, 101, 9, BLOCK_AIR, 0, false);
	redstone_server_set_visit_hook(NULL);
	blocks[BLOCK_REDSTONE_WIRE] = wire_orig;
	assert(get(4, 102, 9, false) == BLOCK_AIR);
	assert(get(5, 102, 9, true) == 0 && get(6, 102, 9, true) == 0);
	int last_d = -1, first_w = -1;
	for(int i = 0; i < g_nev && i < 256; i++) {
		if(g_ev[i].k == 'D')
			last_d = i;
		else if(first_w < 0)
			first_w = i;
	}
	printf("B: events=%d last drain=%d first wire notify=%d\n", g_nev, last_d, first_w);
	assert(g_nev <= 256 && last_d >= 0 && first_w >= 0);
	assert(last_d < first_w); // :227 flood drained before the horizontal notifies (:228-254)

	for(int k = 0; k < 6; k++)
		server_world_tick(&s.worlds[0], &s);
	printf("A BUD torch=%d B BUD torch=%d (lit=%d idle=%d)\n", get(6, 102, 5, false),
		   get(4, 102, 11, false), BLOCK_REDSTONE_TORCH_LIT, BLOCK_REDSTONE_TORCH);
	assert(get(6, 102, 5, false) == BLOCK_REDSTONE_TORCH_LIT); // A: notified by :227 -> lit
	assert(get(4, 102, 11, false) == BLOCK_REDSTONE_TORCH);	   // B: no cell mark -> untouched
	assert(get(5, 102, 9, true) == 0 && get(6, 102, 9, true) == 0);

	g_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(s_dir);
	printf("test_redstone_wire_removal_order_golden: OK\n");
	return 0;
}
