/*
	Golden test for the Beta 1.7.3 in-world item drop ('Q').

	Drives server_local_drop_hotbar_item over a real server_local + superflat world and
	pins the faithful semantics of EntityPlayer.dropCurrentItem ->
	dropPlayerItemWithRandomChoice(decrStackSize(currentItem, 1), false):

	  - drops exactly ONE item from the CURRENT hotbar slot per call (all=false);
	  - the slot empties when it hits 0; an empty hand is a no-op (returns NULL);
	  - all=true drops the whole stack at once;
	  - the EntityItem spawns at the player eye - 0.18 (Beta posY-0.30+getEyeHeight());
	  - the throw velocity equals the exact b1.7.3 false-branch base
	    (-sin(yaw)cos(pitch)*0.3, -sin(pitch)*0.3+0.1, cos(yaw)cos(pitch)*0.3) within
	    Beta's own random scatter bounds (|dx|,|dz| <= 0.02, |dy| <= 0.1), with
	    player.rx/ry == Beta rotationYaw/rotationPitch.

	Decompile: 740c583:1.7.3-LTS EntityPlayer.dropPlayerItemWithRandomChoice.
*/

#include <assert.h>
#include <math.h>
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
#include "entity/entity.h"
#include "item/inventory.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_player_item_drop_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define ITEM_STONE 1

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

static void reset_player(double x, double y, double z, float rx, float ry) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = x;
	s.player.y = y;
	s.player.z = z;
	s.player.rx = rx;
	s.player.ry = ry;
	s.player.active_inventory = &s.player.inventory;
}

// Put `count` of stone in hotbar slot `slot` and select it.
static void give_hotbar(size_t slot, uint8_t count) {
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + slot,
					   (struct item_data) {.id = ITEM_STONE, .count = count});
	inventory_set_hotbar(&s.player.inventory, slot);
}

static uint8_t slot_count(size_t slot) {
	struct item_data it;
	if(!inventory_get_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + slot, &it))
		return 0;
	return it.id == 0 ? 0 : it.count;
}

// Assert the thrown velocity matches Beta's base throw, derived INDEPENDENTLY from the
// player look RAY -- NOT by re-running the implementation's own -sin(yaw)/cos(yaw) formula
// (which would only prove the code equals itself and could never catch a wrong
// rx/ry->yaw/pitch mapping). The client sends rx=-deg(camera.rx), ry=deg(camera.ry)-90, and
// the camera look ray (camera_get_ray) is dir = {sin(crx)sin(cry), cos(cry), cos(crx)sin(cry)}.
// Beta throws at 0.3*dir + {0,0.1,0}, then adds bounded random scatter (xz +-0.02, y +-0.1).
// A different route reaching the same numbers proves the direction is right.
static void assert_throw_velocity(struct entity* e, float rx, float ry) {
	float crx = -rx * (float)M_PI / 180.0f;          // camera.rx (radians)
	float cry = (ry + 90.0f) * (float)M_PI / 180.0f; // camera.ry (radians)
	float ex = (sinf(crx) * sinf(cry)) * 0.3f;
	float ey = (cosf(cry)) * 0.3f + 0.1f;
	float ez = (cosf(crx) * sinf(cry)) * 0.3f;
	float eps = 1e-4f;
	assert(fabsf(e->vel[0] - ex) <= 0.02f + eps);
	assert(fabsf(e->vel[2] - ez) <= 0.02f + eps);
	assert(fabsf(e->vel[1] - ey) <= 0.1f + eps);
}

static void test_drop_one_decrements_current_slot(void) {
	reset_player(8.0, 70.0, 8.0, 0.0f, 0.0f); // look +Z, level
	give_hotbar(0, 5);

	struct entity* e = server_local_drop_hotbar_item(&s, false);
	assert(e); // a thrown item entity was spawned

	// one item left the slot
	assert(slot_count(0) == 4);
	// the dropped entity carries exactly one stone
	assert(e->data.item.item.id == ITEM_STONE);
	assert(e->data.item.item.count == 1);
	// spawn position == player eye - 0.18 (exact)
	assert(fabsf(e->pos[0] - 8.0f) < 1e-3f);
	assert(fabsf(e->pos[1] - (70.0f - 0.18f)) < 1e-3f);
	assert(fabsf(e->pos[2] - 8.0f) < 1e-3f);
	// thrown forward (+Z) at the Beta base velocity, within scatter bounds
	assert_throw_velocity(e, 0.0f, 0.0f);
	assert(e->vel[2] > 0.2f); // base 0.3 +- 0.02 -> clearly +Z
	printf("  drop-one: 5->4, one stone thrown +Z at eye-0.18 OK\n");
}

static void test_drop_to_empty_then_noop(void) {
	reset_player(8.0, 70.0, 8.0, 0.0f, 0.0f);
	give_hotbar(0, 2);

	assert(server_local_drop_hotbar_item(&s, false)); // 2 -> 1
	assert(slot_count(0) == 1);
	assert(server_local_drop_hotbar_item(&s, false)); // 1 -> 0 (slot clears)
	assert(slot_count(0) == 0);
	// empty hand -> no-op, no entity
	assert(server_local_drop_hotbar_item(&s, false) == NULL);
	printf("  drop to empty then empty-hand no-op OK\n");
}

static void test_drop_all_drops_stack(void) {
	reset_player(8.0, 70.0, 8.0, 0.0f, 0.0f);
	give_hotbar(0, 7);

	struct entity* e = server_local_drop_hotbar_item(&s, true);
	assert(e);
	assert(slot_count(0) == 0);              // whole stack left the slot
	assert(e->data.item.item.count == 7);    // dropped as one stack of 7
	printf("  drop-all: 7 -> 0, one stack of 7 thrown OK\n");
}

static void test_drops_from_selected_slot(void) {
	reset_player(8.0, 70.0, 8.0, 0.0f, 0.0f);
	// fill slot 0 and slot 3; select slot 3 -> the drop must come from 3, not 0.
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0,
					   (struct item_data) {.id = ITEM_STONE, .count = 9});
	give_hotbar(3, 4);

	struct entity* e = server_local_drop_hotbar_item(&s, false);
	assert(e);
	assert(slot_count(3) == 3); // selected slot decremented
	assert(slot_count(0) == 9); // other slot untouched
	printf("  drops from the SELECTED hotbar slot OK\n");
}

static void test_velocity_tracks_look(void) {
	// yaw 90 (face -X): base vx ~ -0.3, vz ~ 0
	reset_player(8.0, 70.0, 8.0, 90.0f, 0.0f);
	give_hotbar(0, 3);
	struct entity* e = server_local_drop_hotbar_item(&s, false);
	assert(e);
	assert_throw_velocity(e, 90.0f, 0.0f);
	assert(e->vel[0] < -0.2f); // clearly -X

	// pitch 45 (look down): base vy ~ -0.112, vz ~ 0.212
	reset_player(8.0, 70.0, 8.0, 0.0f, 45.0f);
	give_hotbar(0, 3);
	e = server_local_drop_hotbar_item(&s, false);
	assert(e);
	assert_throw_velocity(e, 0.0f, 45.0f);
	assert(e->vel[1] < 0.0f); // looking down -> net downward throw
	printf("  throw velocity tracks look (yaw + pitch) at Beta base OK\n");
}

int main(void) {
	test_fixture_dir("test_player_item_drop_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;

	printf("test_player_item_drop:\n");
	test_drop_one_decrements_current_slot();
	test_drop_to_empty_then_noop();
	test_drop_all_drops_stack();
	test_drops_from_selected_slot();
	test_velocity_tracks_look();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_player_item_drop\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
