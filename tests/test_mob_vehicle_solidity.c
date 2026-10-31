/* G3 (vehicle-smoke-1004): which vehicles are solid to a mob's move.
 *
 * Beta: World.getCollidingBoundingBoxes adds, for every other entity near the
 * mover, that entity's getBoundingBox(). Only two classes override it:
 *   EntityBoat.getBoundingBox()     -> this.boundingBox   [CFR :59-61]
 *   EntityMinecart.getBoundingBox() -> null               [CFR :81-83]
 * so a boat is solid to a walking mob and a minecart is NOT (a cart blocks
 * only through its own func_89_d / applyEntityCollision push, which is G2).
 *
 * Production seam: entity_monster_collide_env(s).entity_boxes, the exact
 * callback the mob mover queries. A boat, a minecart and a second boat (the
 * querying "self") overlap one query box; the returned list must hold the
 * other boat's box and nothing for the cart.
 */
#include <assert.h>
#include <stdbool.h>
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
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/mob_motion.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
static volatile int g_drain_run = 1;
static unsigned failures;

static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static bool list_has(const struct mob_box_list* l, const struct AABB* b) {
	for(size_t i = 0; i < l->count; i++)
		if(l->boxes[i].min[0] == (double)b->x1 && l->boxes[i].min[1] == (double)b->y1
		   && l->boxes[i].min[2] == (double)b->z1
		   && l->boxes[i].max[0] == (double)b->x2
		   && l->boxes[i].max[1] == (double)b->y2
		   && l->boxes[i].max[2] == (double)b->z2)
			return true;
	return false;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_mob_vehicle_solidity_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	struct server_chunk* sc = NULL;
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	struct entity* boat = server_local_spawn_boat((vec3) {8.0F, 101.0F, 8.0F}, &s);
	struct entity* cart = server_local_spawn_minecart((vec3) {9.0F, 101.0F, 8.0F}, &s);
	struct entity* self = server_local_spawn_boat((vec3) {8.5F, 101.0F, 9.5F}, &s);
	assert(boat && cart && self);

	struct AABB boat_box, cart_box;
	check(boat->getBoundingBox && boat->getBoundingBox(boat, &boat_box) != 0,
		  "boat exposes a box");
	bool cart_has_box = cart->getBoundingBox
		&& cart->getBoundingBox(cart, &cart_box) != 0;

	struct mob_collide_env env = entity_monster_collide_env(&s);
	assert(env.entity_boxes);
	struct mob_aabb_d q = {{4.0, 98.0, 4.0}, {13.0, 105.0, 13.0}};
	struct mob_box_list list = {0};
	env.entity_boxes(env.entity_ctx, self, &q, &list);
	check(!list.oom, "entity box query complete");
	check(list_has(&list, &boat_box),
		  "boat is solid to a mob mover (EntityBoat.getBoundingBox :59-61)");
	check(!cart_has_box || !list_has(&list, &cart_box),
		  "minecart is NOT solid to a mob mover "
		  "(EntityMinecart.getBoundingBox returns null, :81-83)");
	check(list.count == 1, "exactly one vehicle box (the other boat)");
	mob_box_list_free(&list);

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	printf("%s test_mob_vehicle_solidity (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
