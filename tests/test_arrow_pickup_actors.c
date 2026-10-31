/* lane C #14 (2026-09-26): stuck-arrow pickup for BOTH actors.
 *
 * Beta 1.7.3 (decompile ../mcb173-retromcp/.../src_original):
 *   EntityPlayer.onLivingUpdate [EntityPlayer.java:196-205]: only while
 *     `health > 0`, the player queries getEntitiesWithinAABBExcludingEntity(
 *     this, boundingBox.expand(1.0D, 0.0D, 1.0D)) and calls
 *     onCollideWithPlayer on every non-dead hit. Every living player runs
 *     this -- nothing is host-only -- and only over entities of ITS OWN
 *     world (worldObj), so an arrow in another dimension is never visited.
 *   EntityArrow.onCollideWithPlayer [EntityArrow.java:257-266]:
 *     inGround && doesArrowBelongToPlayer && arrowShake <= 0 &&
 *     inventory.addItemStackToInventory(new ItemStack(Item.arrow, 1)) ->
 *     random.pop, onItemPickup, setEntityDead. doesArrowBelongToPlayer is
 *     "shot by A player" (any player) -- the collector need not be the
 *     shooter. A full inventory refuses and the arrow stays.
 *   Arrow box: setSize(0.5F, 0.5F) [EntityArrow.java:20-34], yOffset 0.
 *   Player box: 0.6 x 1.8, minY = posY - 1.62 [Entity.setPosition].
 *   Order: the host is scanned first (the item pickup precedent,
 *   entity_item.c) so a shared spot goes to the host.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "entity/entity.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static struct server_local s;
static struct server_player guest;
static volatile int draining = 1;
static char world_dir[128], world_dir2[128];
static uint8_t stuck_type, stuck_meta;

static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}

/* A stuck, player-fired arrow at (x,y,z) embedded in the flat grass cell
 * (8,3,8) of `world` (the release check compares that cell each tick). */
static struct entity* stuck_arrow(struct server_world* world, float x, float y,
								  float z, bool from_player, int shake) {
	struct entity* e = calloc(1, sizeof *e);
	assert(e);
	entity_arrow(700, e, true, world, 0, ITEM_ARROW, 2, from_player);
	e->teleport(e, (vec3) {x, y, z});
	glm_vec3_zero(e->vel);
	e->data.arrow.in_ground = true;
	e->data.arrow.shake_ticks = shake;
	e->data.arrow.stuck_block_x = 8;
	e->data.arrow.stuck_block_y = 3;
	e->data.arrow.stuck_block_z = 8;
	e->data.arrow.stuck_block_type = stuck_type;
	e->data.arrow.stuck_block_meta = stuck_meta;
	return e;
}

static void clear_inventory(struct server_player* p) {
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		inventory_clear_slot(&p->inventory, i);
}

static int arrows_held(struct server_player* p) {
	int n = 0;
	for(size_t i = 0; i < INVENTORY_SIZE; i++) {
		struct item_data it = {0};
		if(inventory_get_slot(&p->inventory, i, &it) && it.id == ITEM_ARROW)
			n += it.count;
	}
	return n;
}

static void place(struct server_player* p, double x, double y, double z) {
	p->x = x; p->y = y; p->z = z;
}

static void run_actor(bool use_guest) {
	struct server_player* me = use_guest ? &guest : &s.player;
	struct server_player* other = use_guest ? &s.player : &guest;
	const char* who = use_guest ? "guest" : "host";
	place(me, 8.5, 102.62, 8.5);
	place(other, 40.5, 102.62, 40.5);
	clear_inventory(me);
	clear_inventory(other);
	struct server_world* w = &s.worlds[0];

	/* 1. Plain pickup at the feet: +1 arrow to THIS actor, arrow removed. */
	struct entity* e = stuck_arrow(w, 8.5F, 101.2F, 8.5F, true, 0);
	assert(e->tick_server(e, &s));
	assert(arrows_held(me) == 1 && arrows_held(other) == 0);
	free(e);

	/* 2. arrowShake gate: the countdown runs before the player scan; 2 ->
	 * 1 (refused) -> 0 (taken). */
	e = stuck_arrow(w, 8.5F, 101.2F, 8.5F, true, 2);
	assert(!e->tick_server(e, &s) && arrows_held(me) == 1);
	assert(e->tick_server(e, &s) && arrows_held(me) == 2);
	free(e);

	/* 3. Skeleton / dispenser arrow (doesArrowBelongToPlayer false). */
	e = stuck_arrow(w, 8.5F, 101.2F, 8.5F, false, 0);
	assert(!e->tick_server(e, &s) && arrows_held(me) == 2);
	free(e);

	/* 4. Full inventory refuses; one free slot then accepts. */
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		inventory_set_slot(&me->inventory, i,
			(struct item_data) {.id = BLOCK_STONE, .count = 64});
	e = stuck_arrow(w, 8.5F, 101.2F, 8.5F, true, 0);
	assert(!e->tick_server(e, &s) && e->data.arrow.in_ground);
	inventory_clear_slot(&me->inventory, INVENTORY_SLOT_HOTBAR);
	assert(e->tick_server(e, &s) && arrows_held(me) == 1);
	free(e);
	clear_inventory(me);

	/* 5. Box edges (expand(1,0,1) vs the 0.5 arrow box, strict):
	 * +1.6 on X: arrow minX 8.5+1.35 > player maxX 8.5+1.3 -> no hit, though
	 * a 2-block sphere would take it. */
	e = stuck_arrow(w, 10.1F, 101.2F, 8.5F, true, 0);
	assert(!e->tick_server(e, &s) && arrows_held(me) == 0);
	free(e);
	/* Top corner: (+1.5, eye+0.1, +1.5) intersects the expanded box (minX
	 * +1.25 < +1.3; minY eye+0.1 < maxY eye+0.18), sphere distance 2.23. */
	e = stuck_arrow(w, 10.0F, 102.72F, 10.0F, true, 0);
	assert(e->tick_server(e, &s) && arrows_held(me) == 1);
	free(e);
	clear_inventory(me);

	/* 6. Dead actor runs no scan [EntityPlayer.java:196]. */
	me->health = 0;
	e = stuck_arrow(w, 8.5F, 101.2F, 8.5F, true, 0);
	assert(!e->tick_server(e, &s) && arrows_held(me) == 0);
	me->health = MAX_PLAYER_HEALTH;
	assert(e->tick_server(e, &s) && arrows_held(me) == 1);
	free(e);
	clear_inventory(me);

	/* 7. Cross-world: the same spot in ANOTHER live world is never visited
	 * (the actor's world is server_local_world_of == worlds[0]). */
	e = stuck_arrow(&s.worlds[1], 8.5F, 101.2F, 8.5F, true, 0);
	assert(!e->tick_server(e, &s) && e->data.arrow.in_ground);
	assert(arrows_held(me) == 0 && arrows_held(other) == 0);
	free(e);
	printf("%s: pickup, shake, non-player, full inventory, box edges, "
		   "dead gate, other-world rejection PASS\n", who);
}

static void run_shared_spot(void) {
	/* Both actors over one arrow: host first, the guest gets nothing. */
	place(&s.player, 8.5, 102.62, 8.5);
	place(&guest, 8.6, 102.62, 8.4);
	clear_inventory(&s.player);
	clear_inventory(&guest);
	struct entity* e = stuck_arrow(&s.worlds[0], 8.5F, 101.2F, 8.5F, true, 0);
	assert(e->tick_server(e, &s));
	assert(arrows_held(&s.player) == 1 && arrows_held(&guest) == 0);
	free(e);
	/* Host full -> the guest takes it in the same tick. */
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		inventory_set_slot(&s.player.inventory, i,
			(struct item_data) {.id = BLOCK_STONE, .count = 64});
	e = stuck_arrow(&s.worlds[0], 8.5F, 101.2F, 8.5F, true, 0);
	assert(e->tick_server(e, &s));
	assert(arrows_held(&guest) == 1);
	free(e);
	/* Guest not admitted to gameplay -> host-only behaviour. */
	clear_inventory(&guest);
	s.guest_gameplay = false;
	e = stuck_arrow(&s.worlds[0], 8.5F, 101.2F, 8.5F, true, 0);
	assert(!e->tick_server(e, &s) && arrows_held(&guest) == 0);
	s.guest_gameplay = true;
	free(e);
	puts("shared spot: host first, full host -> guest, unadmitted guest PASS");
}

static void make_world(struct server_world* w, char* dir, size_t n,
					   const char* name) {
	test_fixture_dir(name, dir, n);
	MKDIR(dir);
	MKDIR(test_fixture_join(dir, "/region"));
	string_t level;
	string_init_set_str(level, dir);
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true);
	w->session = &s;
	w->local = &s;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 2; cx++)
		for(int cz = 0; cz <= 2; cz++)
			assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
}

int main(void) {
	assert(clin_init()); blocks_init(); items_init();
	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	make_world(&s.worlds[0], world_dir, sizeof world_dir,
			   "test_arrow_pickup_actors_tmp");
	make_world(&s.worlds[1], world_dir2, sizeof world_dir2,
			   "test_arrow_pickup_actors_tmp2");
	s.worlds[1].world_index = 1;
	s.scheduled_tick_budget = 128;
	s.entity_rng.seed = 0x13579BDFu;
	dict_entity_init(s.entities);
	assert(server_local_world_of(&s, &s.player) == &s.worlds[0]);
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], 8, 3, 8, &b));
	stuck_type = b.type;
	stuck_meta = b.metadata;
	assert(server_world_get_block(&s.worlds[1], 8, 3, 8, &b)
		   && b.type == stuck_type && b.metadata == stuck_meta);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	assert(inventory_create(&guest.inventory, &inventory_logic_player, &s,
		&guest, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	guest.active_inventory = &guest.inventory;
	s.player.has_pos = guest.has_pos = true;
	s.player.finished_loading = guest.finished_loading = true;
	s.player.health = guest.health = MAX_PLAYER_HEALTH;
	s.guest_actor = &guest; s.guest_gameplay = true;
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	run_actor(false);
	run_actor(true);
	run_shared_spot();
	draining = 0; thread_join(&drain);
	puts("test_arrow_pickup_actors: PASS");
	return 0;
}
