/* ci-1101/43 item 9 (HUNT-14 #9): the tamed-wolf owner teleport gate.
 *
 * Beta EntityWolf.setPathEntity [740c583: EntityWolf.java:192-193]:
 *
 *     a path to the owner is requested from the world with range 16.0; if no
 *     path is returned and the owner distance exceeds 12.0, a 5x5 ring scan
 *     picks a spot and the wolf is relocated there; otherwise the returned
 *     path is adopted.
 *
 * so the teleport fires iff NO walkable path to the owner exists AND the
 * owner is farther than 12 blocks. CaveWii's pre-fix gate used the LOS eye
 * ray as a stand-in "no path" test (docs/deviations.md "Mob-AI pathing",
 * revert condition: "replace the eye-ray no-path stand-in with the real
 * getPathToEntity once A* lands"). FH-13's A* port has landed; this test pins
 * the real path gate.
 *
 * The distinguishing case is an owner the wolf can WALK to but cannot SEE:
 * eye ray blocked, a clear 16-block path exists. Pre-fix the blocked eye ray
 * teleported the wolf; post-fix the real getPathToEntity(16) != null
 * suppresses the teleport. The walled-off control proves the path search
 * returning null still teleports, so the negative result above is the path
 * gate rather than a mis-tuned fixture.
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "wolf_test_common.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct inventory_logic inventory_logic_player;

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

static void set_block(struct server_local* s, int x, int y, int z, int type) {
	assert(server_world_set_block(
		&s->worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = 0}));
}

/* A tamed wolf, position pinned, ticked repeatedly. Returns the number of
 * ticks whose END pose sits in the owner's 5x5 teleport ring ([owner_floor-2,
 * owner_floor+2] on x and z) -- i.e. the number of owner teleports that
 * fired. The wolf's own per-tick follow steer moves it at ~0.1 block/tick
 * (< 1 block), so a pose 12+ blocks from its pinned spot can only be the
 * teleport. */
static int wolf_teleport_ticks(struct server_local* s, struct entity* wolf,
							   vec3 pinned, float owner_x, float owner_z,
							   int ticks) {
	int hits = 0;
	int ox = (int)floorf(owner_x);
	int oz = (int)floorf(owner_z);
	for(int i = 0; i < ticks; i++) {
		wolf->pos[0] = pinned[0];
		wolf->pos[1] = pinned[1];
		wolf->pos[2] = pinned[2];
		wolf->vel[0] = wolf->vel[1] = wolf->vel[2] = 0.0f;
		wolf->on_ground = true;
		test_mob_sync(wolf);
		bool reaped = wolf->tick_server(wolf, s);
		assert(!reaped);
		int px = (int)floorf(wolf->pos[0]);
		int pz = (int)floorf(wolf->pos[2]);
		if(px >= ox - 2 && px <= ox + 2 && pz >= oz - 2 && pz <= oz + 2)
			hits++;
	}
	return hits;
}

int main(void) {
	test_fixture_dir("test_wolf_teleport_path_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	// Overworld-only fixture: every world access names &s.worlds[0] (the Overworld slot) explicitly.
	struct server_local s = {0};
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 5150,
						true /*superflat*/);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	/* Host is present in the routed world [EntityWolf.getOwner ->
	 * worldObj.getPlayerEntityByName]; entity_wolf_owner_actor resolves the
	 * local player when the wolf's owner name is the local-player name. */
	s.player.has_pos = true;
	s.player.finished_loading = true;

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	/* Superflat floor: an explicit stone slab well clear of the default
	 * terrain. Corridor z in [2,10], x in [8,28]. The wolf and owner share
	 * z = 5.5 so the pre-fix eye ray travels straight down z-cell 5. */
	const int YF = 40;
	for(int x = 8; x <= 28; x++)
		for(int z = 2; z <= 10; z++)
			set_block(&s, x, YF, z, BLOCK_STONE);

	const float wolf_x = 10.5f, wolf_z = 5.5f;
	const float owner_x = 24.5f, owner_z = 5.5f;
	/* owner_dist_sq must clear both follow (>25) and teleport (>144): the
	 * horizontal separation is 14 blocks -> 196. */
	const float owner_feet_y = (float)YF + 1.0f;
	s.player.x = (double)owner_x;
	s.player.y = (double)owner_feet_y + (double)PLAYER_EYE_HEIGHT;
	s.player.z = (double)owner_z;

	struct entity* wolf = wolf_test_make(&s, 1, MONSTER_WOLF);
	wolf->data.monster.wolf_flags = WOLF_FLAG_TAMED;
	strcpy(wolf->data.monster.wolf_owner, WOLF_LOCAL_PLAYER_NAME);
	wolf->data.monster.wolf_target_id = UINT32_MAX;
	wolf->data.monster.wolf_shake_active = false;
	vec3 pinned = {wolf_x, (float)YF + 1.0f, wolf_z};
	wolf->data.monster.wolf_flags &= (uint8_t)~WOLF_FLAG_SITTING;

	/* --- 1. PATH EXISTS, LOS BLOCKED -------------------------------------
	 * One 3-high stone column at (17, YF+1..YF+3, 5) blocks the straight eye
	 * ray down z-cell 5, but the wolf walks around it via z = 6 (or 4): a
	 * real getPathToEntity(this, owner, 16) finds a path. Beta then takes the
	 * setPathToEntity branch and NEVER teleports; the pre-fix eye-ray stand-in
	 * saw a blocked ray and teleported. */
	for(int y = YF + 1; y <= YF + 3; y++)
		set_block(&s, 17, y, 5, BLOCK_STONE);
	int hits = wolf_teleport_ticks(&s, wolf, pinned, owner_x, owner_z, 4);
	printf("path exists / LOS blocked: teleport ticks=%d (expect 0)\n", hits);
	assert(hits == 0
		   && "a walkable path to the owner must suppress the teleport even "
			  "with the eye ray blocked (Beta getPathToEntity(16) != null)");

	/* --- 2. WALLED-OFF WOLF (no path) ------------------------------------
	 * Fully enclose the wolf so getPathToEntity(this, owner, 16) returns
	 * null: the pathfinder's addToPath only reports "no path" when the search
	 * never improves on the start node, so the start cell's four cardinal
	 * neighbours -- AND their can_step_up cells -- must all be unreachable.
	 * Making x in [9,11], z in [4,6] solid at the wolf's feet (YF+1) and head
	 * (YF+2) does that: every neighbour's getVerticalOffset hits solid, so
	 * neither the direct open nor the step-up probe yields a point. Best stays
	 * the start -> null path -> Beta's ring teleport fires. (A wall across the
	 * corridor alone is NOT enough: the wolf still steps east toward the owner,
	 * which is a real partial path, and Beta suppresses the teleport.) This is
	 * the positive control that a null path still teleports, proving case 1's
	 * zero came from the path search, not a dead fixture. */
	for(int x = 9; x <= 11; x++)
		for(int z = 4; z <= 6; z++)
			for(int y = YF + 1; y <= YF + 2; y++)
				set_block(&s, x, y, z, BLOCK_STONE);
	hits = wolf_teleport_ticks(&s, wolf, pinned, owner_x, owner_z, 3);
	printf("walled-off owner (no path): teleport ticks=%d (expect >0)\n", hits);
	assert(hits > 0
		   && "a walled-off owner with no 16-block path must teleport the wolf "
			  "into the 5x5 ring (Beta setPathEntity)");

	wolf_test_clear(&s);
	g_drain_run = 0;
	thread_join(&drain);
	puts("test_wolf_teleport_path: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
