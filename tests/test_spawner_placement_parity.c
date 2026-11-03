/* ci-1101/45 part A (HUNT-14 #2b): dungeon mob-spawner member placement gate.
 *
 * Beta TileEntityMobSpawner.updateEntity places each burst member with
 * setLocationAndAngles at the rolled position and keeps it iff the member's
 * getCanSpawnHere holds [mcb173-retromcp/minecraft_server/.../
 * TileEntityMobSpawner.java:72-76]. For the dungeon mobs (EntityZombie /
 * EntitySkeleton / EntitySpider, all `extends EntityMob extends
 * EntityCreature`) that is:
 *   EntityMob.getCanSpawnHere [EntityMob.java:69-86]: the two light rolls,
 *     then the EntityCreature check must also hold;
 *   EntityCreature.getCanSpawnHere [EntityCreature.java:159-164]:
 *     the EntityLiving check must hold and getBlockPathWeight at the position
 *     must be >= 0.0;
 *   EntityLiving.getCanSpawnHere [EntityLiving.java:781-783 client /
 *     minecraft_server same body]: checkIfAABBIsClear(bb) &&
 *     getCollidingBoundingBoxes(this, bb).size() == 0 &&
 *     !getIsAnyLiquid(bb).
 * There is NO floor test and NO "cell must be air" test: the member's own
 * AABB (width x height at the exact double position) must be collision-free
 * and liquid-free. CaveWii's spawner_spot_ok instead demanded air here + air
 * above + a normal-cube floor (a CELL test), which (a) refused every member
 * over a non-floor (Beta mobs spawn mid-air and fall), (b) refused
 * non-colliding cells (tall grass, flowers, snow layer < 3, webs), and
 * (c) accepted a 1.4-wide spider whose box pokes into walls.
 *
 * Driven through the real burst seam server_local_test_spawner_fire.
 */
#include <assert.h>
#include <math.h>
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
#include "network/server_spawner.h" // spawner_te_upsert_w
#include "network/server_world.h"
#include "platform/thread.h"

extern struct inventory_logic inventory_logic_player;

static struct server_local s;
static char s_dir[128];
static volatile int g_drain_run = 1;

static const int sx = 8, sy = 20, sz = 8;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void fill(int x0, int x1, int y0, int y1, int z0, int z1, int type) {
	for(int x = x0; x <= x1; x++)
		for(int z = z0; z <= z1; z++)
			for(int y = y0; y <= y1; y++)
				(void)server_world_set_block_silent(&s.worlds[0], x, y, z,
					(struct block_data) {.type = (uint8_t)type});
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		free(e);
	}
	dict_entity_reset(s.entities);
}

/* Fire `trials` bursts of `mob`, clearing between them (so neither the >=6
 * crowd exit nor the global cap trips). Returns the total emitted; counts in
 * *floating the members whose feet cell has a non-colliding (air) cell below.
 */
static int fire_many(int mob, int trials, int* floating) {
	int total = 0;
	if(floating)
		*floating = 0;
	for(int t = 0; t < trials; t++) {
		clear_entities();
		(void)server_local_test_spawner_fire(&s, sx, sy, sz, mob);
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(!e || e->type != ENTITY_MONSTER)
				continue;
			total++;
			if(floating) {
				struct block_data below;
				w_coord_t fx = (w_coord_t)floorf(e->pos[0]);
				w_coord_t fy = (w_coord_t)floorf(e->pos[1]) - 1;
				w_coord_t fz = (w_coord_t)floorf(e->pos[2]);
				if(server_world_get_block(&s.worlds[0], fx, fy, fz, &below)
				   && below.type == BLOCK_AIR)
					(*floating)++;
			}
		}
	}
	clear_entities();
	return total;
}

/* The room every case rebuilds: a stone shell, then the case's interior.
 * Members land in cells sx-4..sx+3 / sy-1..sy+1 / sz-4..sz+3 and their boxes
 * reach at most 0.7 further (spider) and 1.8 up, so +-6 / sy-6..sy+4 covers
 * every cell the AABB, collision (-1 rim) and liquid walks read. */
static void shell(void) {
	fill(sx - 7, sx + 7, sy - 7, sy + 5, sz - 7, sz + 7, BLOCK_STONE);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_spawner_placement_parity_tmp", s_dir, sizeof s_dir);
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
	s.rand_src.seed = 0x5EA7B0A7u;
	dict_entity_init(s.entities);
	assert(server_local_te_tables_init(&s));
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	/* Player far away: never inside a member box, never player-excluded. */
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 500.5;
	s.player.y = 64.0 + PLAYER_EYE_HEIGHT;
	s.player.z = 500.5;
	s.world_time = 18000; /* midnight; the room is sealed (light 0) anyway */

	const int N = 300;
	int floating = 0;

	/* (1) NO FLOOR: a tall air shaft, the stone floor 4 cells below the
	 * lowest member row. Beta has no floor clause -> members spawn mid-air.
	 * Pre-fix: 0 (spawner_spot_ok demanded a normal-cube floor). */
	shell();
	fill(sx - 6, sx + 6, sy - 5, sy + 4, sz - 6, sz + 6, BLOCK_AIR);
	int n1 = fire_many(MONSTER_ZOMBIE, N, &floating);
	printf("(1) no-floor shaft: zombie emits=%d floating=%d (expect >0, >0)\n",
		   n1, floating);
	assert(n1 > 0 && "Beta getCanSpawnHere has no floor test");
	assert(floating > 0 && "a member with air below its feet must be legal");

	/* (2) LIQUID: the same shaft full of still water. getIsAnyLiquid refuses
	 * every member [World.java getIsAnyLiquid, EntityLiving.java:782]. */
	shell();
	fill(sx - 6, sx + 6, sy - 5, sy + 4, sz - 6, sz + 6, BLOCK_WATER_STILL);
	int n2 = fire_many(MONSTER_ZOMBIE, N, NULL);
	printf("(2) water-filled: zombie emits=%d (expect 0)\n", n2);
	assert(n2 == 0 && "a member box touching liquid must be refused");

	/* (3) NON-COLLIDING, NON-AIR cells: every member row is tall grass (no
	 * collision box), floor below. Beta keeps these members (only collision
	 * BOXES refuse). Pre-fix: 0 (the cell had to be BLOCK_AIR). */
	shell();
	fill(sx - 6, sx + 6, sy - 1, sy + 4, sz - 6, sz + 6, BLOCK_AIR);
	fill(sx - 6, sx + 6, sy - 1, sy + 1, sz - 6, sz + 6, BLOCK_TALL_GRASS);
	int n3 = fire_many(MONSTER_ZOMBIE, N, NULL);
	printf("(3) tall-grass rows: zombie emits=%d (expect >0)\n", n3);
	assert(n3 > 0 && "a non-colliding cell (tall grass) must not refuse");

	/* (4) PARTIAL COLLISION BOX: every member row is a bottom half-slab
	 * (box 0..0.5). The member box starts at the integer y of the slab cell,
	 * so it intersects the slab -> refused. */
	shell();
	fill(sx - 6, sx + 6, sy - 1, sy + 4, sz - 6, sz + 6, BLOCK_AIR);
	fill(sx - 6, sx + 6, sy - 1, sy + 1, sz - 6, sz + 6, BLOCK_SLAB);
	int n4 = fire_many(MONSTER_ZOMBIE, N, NULL);
	printf("(4) half-slab rows: zombie emits=%d (expect 0)\n", n4);
	assert(n4 == 0 && "a member box intersecting a block box must be refused");

	/* (5) WIDTH: 1-wide corridors along z (stone pillars on every odd x).
	 * A 0.6-wide zombie fits when its x lands within 0.3 of the corridor
	 * centre; a 1.4-wide spider [EntitySpider.java:7 setSize(1.4F, 0.9F)]
	 * NEVER fits. Pre-fix the cell test accepted the spider. */
	shell();
	fill(sx - 6, sx + 6, sy - 1, sy + 4, sz - 6, sz + 6, BLOCK_AIR);
	for(int x = sx - 6; x <= sx + 6; x++)
		if(x & 1)
			fill(x, x, sy - 1, sy + 4, sz - 6, sz + 6, BLOCK_STONE);
	int n5z = fire_many(MONSTER_ZOMBIE, N, NULL);
	int n5s = fire_many(MONSTER_SPIDER, N, NULL);
	printf("(5) 1-wide corridors: zombie emits=%d (expect >0), spider "
		   "emits=%d (expect 0)\n", n5z, n5s);
	assert(n5z > 0 && "a 0.6-wide box fits a 1-wide corridor");
	assert(n5s == 0 && "a 1.4-wide spider box can never fit a 1-wide corridor");

	/* (6) SOAK, no global cap (maintainer ruling 2026-11-02): four real
	 * Zombie spawners (block + MobSpawner TE), each in its own sealed 9x9x4
	 * room, the player in a sealed pocket in the middle (in range, unreachable,
	 * nothing kills the mobs). Every pass forces every tracked spawner due, so
	 * each fires as often as possible; the real tracker, burst and entity tick
	 * run for many ticks. Beta's only bound is the same-class >=6 crowd box
	 * [TileEntityMobSpawner.java:56-60]: each room (inside its spawner's
	 * expand(8,4,8) box) must hold <= 6 zombies at every step, the TOTAL must
	 * exceed the old global MOB_SPAWN_CAP 10 (proves the cap is gone from this
	 * path), and nothing asserts / overflows (entity worklist growable, RPCs
	 * drained by the discard thread through the real send path). */
	{
		clear_entities();
		const int off[4][2] = {{-10, -10}, {-10, 10}, {10, -10}, {10, 10}};
		/* one big stone block holding all rooms + the pocket */
		fill(sx - 16, sx + 16, sy - 2, sy + 5, sz - 16, sz + 16, BLOCK_STONE);
		for(int r = 0; r < 4; r++) {
			const int cx = sx + off[r][0], cz = sz + off[r][1];
			fill(cx - 4, cx + 4, sy, sy + 3, cz - 4, cz + 4, BLOCK_AIR);
			assert(server_world_set_block_silent(&s.worlds[0], cx, sy, cz,
				(struct block_data) {.type = BLOCK_MOB_SPAWNER}));
			assert(spawner_te_upsert_w(&s.worlds[0], cx, sy, cz, "Zombie"));
		}
		fill(sx, sx, sy, sy + 1, sz, sz, BLOCK_AIR); /* the player's pocket */
		s.player.x = (double)sx + 0.5;
		s.player.y = (double)sy + PLAYER_EYE_HEIGHT;
		s.player.z = (double)sz + 0.5;

		int max_room = 0, max_total = 0, passes = 0;
		for(int t = 0; t < 2000; t++) {
			s.world_time++;
			for(int i = 0; i < MAX_ACTIVE_SPAWNERS; i++)
				if(s.active_spawners[i].used)
					s.active_spawners[i].delay = 1; /* due on this pass */
			passes += server_local_test_tick_spawners(&s) > 0;
			int room_n[4] = {0, 0, 0, 0}, total = 0;
			dict_entity_it_t it;
			dict_entity_it(it, s.entities);
			while(!dict_entity_end_p(it)) {
				struct entity* e = dict_entity_ref(it)->value;
				dict_entity_next(it);
				if(!e || e->type != ENTITY_MONSTER || e->delay_destroy == 0)
					continue;
				assert(e->data.monster.id == MONSTER_ZOMBIE);
				total++;
				int in_room = -1;
				for(int r = 0; r < 4; r++) {
					const float cx = (float)(sx + off[r][0]);
					const float cz = (float)(sz + off[r][1]);
					if(e->pos[0] > cx - 4.0F && e->pos[0] < cx + 5.0F
					   && e->pos[2] > cz - 4.0F && e->pos[2] < cz + 5.0F)
						in_room = r;
				}
				assert(in_room >= 0 && "a zombie escaped its sealed room");
				room_n[in_room]++;
			}
			for(int r = 0; r < 4; r++) {
				if(room_n[r] > max_room)
					max_room = room_n[r];
				assert(room_n[r] <= 6
					   && "Beta: the same-class >=6 crowd box bounds a spawner");
			}
			if(total > max_total)
				max_total = total;
			(void)server_local_test_tick_entities(&s);
		}
		printf("(6) soak 2000 ticks, 4 spawners: active passes=%d, max per "
			   "room=%d (expect 6), max total=%d (expect > 10)\n", passes,
			   max_room, max_total);
		assert(passes > 0 && "fixture: the spawners were tracked");
		assert(max_room == 6 && "each room fills to exactly Beta's 6");
		assert(max_total > 10
			   && "no global MOB_SPAWN_CAP on the spawner path any more");
		clear_entities();
	}

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(s_dir);
	puts("PASS spawner placement parity (Beta EntityLiving.getCanSpawnHere "
		 "AABB gate: no floor, no air-cell test, box collision + liquid)");
	return 0;
}
