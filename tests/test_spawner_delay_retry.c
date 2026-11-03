/* ci-1101/43 item 2 (HUNT-14 #2a): the dungeon mob-spawner delay cadence.
 *
 * Beta TileEntityMobSpawner.updateEntity [740c583, TileEntityMobSpawner.java:
 * 48-91] re-rolls its refire delay (updateDelay: `delay = 200 + rand.nextInt
 * (600)`) ONLY on a member SUCCESS (:86) or the `var10 >= 6` crowd exit (:68).
 * A burst whose 4 members all fail getCanSpawnHere falls out of the for(var8<4)
 * loop with `delay` still spent (`<= 0`), so the spawner RETRIES on the very
 * next tick -- it is NOT benched for a fresh 200..800 ticks.
 *
 * CaveWii re-rolled the delay after EVERY burst, so a spawner sitting in a
 * spot where its members are momentarily refused (player too close, walls /
 * no air gap, cap) idles 200..780 ticks between tries. This test pins the Beta
 * decision through the real burst seam (server_local_test_spawner_fire now
 * returns updateDelay's yes/no) and through the real tick loop
 * (server_local_test_tick_spawners): an all-refused burst must NOT re-roll.
 */
#include <assert.h>
#include <stdbool.h>
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
#include "entity/combat.h" // PLAYER_EYE_HEIGHT (player y is the eye)
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_spawner.h"
#include "network/server_world.h"
#include "platform/thread.h"

extern struct inventory_logic inventory_logic_player;

static struct server_local s;
static char s_dir[128];
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

static void fill(struct server_world* w, int x0, int x1, int y0, int y1,
				 int z0, int z1, int type) {
	for(int x = x0; x <= x1; x++)
		for(int z = z0; z <= z1; z++)
			for(int y = y0; y <= y1; y++)
				(void)server_world_set_block_silent(w, x, y, z,
					(struct block_data) {.type = (uint8_t)type});
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_spawner_delay_retry_tmp", s_dir, sizeof s_dir);
	MKDIR(s_dir);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	char region[160];
	snprintf(region, sizeof region, "%s/region", s_dir);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, s_dir);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, false);
	string_clear(level);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	s.render_demo_mob = -1;
	s.rand_src.seed = 0x51A7E5EDu;
	dict_entity_init(s.entities);
	/* The tick-loop step (3) drives the real near-player tracker, which needs
	 * the spawner tile-entity table (IMP-TE-CAPS growable slabs). server_local
	 * normally allocates it in server_local_init; this hand-built fixture must
	 * do the same or spawner_te_upsert_w cannot claim a row. server_world_create
	 * already stamped worlds[0].generation, so the owner-keyed te_store accepts
	 * rows for it. */
	assert(server_local_te_tables_init(&s));
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	/* Player far away so the spawner is not player-excluded and no natural
	 * pass competes; the burst seam is driven directly below. */
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 500.5;
	s.player.y = 64.0 + PLAYER_EYE_HEIGHT;
	s.player.z = 500.5;

	const int sx = 8, sy = 20, sz = 8;

	/* (1) ALL-REFUSED burst: solid stone everywhere around the spawner cell,
	 * so every member's spot_ok fails (no air gap / no floor above). Beta
	 * falls out of the loop with delay spent -> NO re-roll. */
	fill(&s.worlds[0], sx - 6, sx + 6, sy - 2, sy + 3, sz - 6, sz + 6,
		 BLOCK_STONE);
	bool reroll = server_local_test_spawner_fire(&s, sx, sy, sz, MONSTER_ZOMBIE);
	printf("all-refused burst: reroll=%d (expect 0)\n", reroll);
	assert(!reroll && "an all-refused burst must NOT re-roll the delay "
					  "(Beta retries next tick: TileEntityMobSpawner.java:48-91)");

	/* (2) A burst that CAN emit (real air gap over a stone floor) DOES re-roll
	 * (updateDelay on the member success path). Proves the seam distinguishes
	 * the two cases rather than always returning false. */
	fill(&s.worlds[0], sx - 6, sx + 6, sy - 1, sy + 3, sz - 6, sz + 6,
		 BLOCK_AIR);
	fill(&s.worlds[0], sx - 6, sx + 6, sy - 1, sy - 1, sz - 6, sz + 6,
		 BLOCK_STONE);
	/* open sky at midnight: force a dark effective light so the members pass
	 * the two-roll light gate (the spawner cell itself also needs a solid
	 * floor: put one under the spawner). */
	fill(&s.worlds[0], sx, sx, sy + 2, sy + 3, sz, sz, BLOCK_STONE);
	s.world_time = 18000;
	bool any = false;
	for(int t = 0; t < 400 && !any; t++) {
		reroll = server_local_test_spawner_fire(&s, sx, sy, sz, MONSTER_ZOMBIE);
		if(reroll)
			any = true;
		/* clear so the >=6 crowd exit never trips */
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			free(e);
		}
		dict_entity_reset(s.entities);
	}
	printf("open-floor burst: emitted-reroll seen=%d (expect 1)\n", any);
	assert(any && "a burst with a valid spot must re-roll the delay on the "
				  "member success path (updateDelay)");

	/* (3) The real tick loop honors the decision: seed a tracked spawner,
	 * force its delay to 0, and drive one tick with the all-refused geometry.
	 * Before the fix the loop re-rolled unconditionally (delay jumps to
	 * 10..39); after it, the all-refused burst leaves delay == 0 so the very
	 * next tick retries. */
	fill(&s.worlds[0], sx - 6, sx + 6, sy - 2, sy + 3, sz - 6, sz + 6,
		 BLOCK_STONE);
	/* A real dungeon spawner: the tracker's block scan needs a
	 * BLOCK_MOB_SPAWNER *and* its MobSpawner tile entity (FH-17: the mob type
	 * comes from the TE, not block metadata), or the row is evicted. */
	assert(server_world_set_block_silent(&s.worlds[0], sx, sy, sz,
		(struct block_data) {.type = BLOCK_MOB_SPAWNER}));
	assert(spawner_te_upsert_w(&s.worlds[0], sx, sy, sz, "Zombie"));
	s.player.x = (double)sx + 4.5; /* within SPAWNER_RANGE so it tracks */
	s.player.y = (double)sy + PLAYER_EYE_HEIGHT;
	s.player.z = (double)sz + 0.5;
	(void)server_local_test_tick_spawners(&s); /* first sight: allocates the row */
	int row = -1;
	for(int i = 0; i < MAX_ACTIVE_SPAWNERS; i++)
		if(s.active_spawners[i].used
		   && s.active_spawners[i].x == sx && s.active_spawners[i].y == sy
		   && s.active_spawners[i].z == sz)
			row = i;
	assert(row >= 0 && "fixture: the spawner was tracked");
	s.active_spawners[row].delay = 0; /* due now */
	assert(server_local_test_tick_spawners(&s) >= 1);
	/* After the fire: an all-refused burst must NOT have re-rolled. Beta
	 * leaves delay spent so the next tick retries; CaveWii pre-fix set it to
	 * SPAWNER_MIN_UNITS + rand(SPAWNER_RAND_UNITS) (>= 10). */
	printf("tick-loop after all-refused burst: delay=%d (expect 0)\n",
		   s.active_spawners[row].delay);
	assert(s.active_spawners[row].delay == 0
		   && "the tick loop must not benched a spawner whose burst was "
			  "entirely refused; Beta retries next tick");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(s_dir);
	puts("PASS spawner delay retry cadence (Beta updateDelay on success/crowd only)");
	return 0;
}