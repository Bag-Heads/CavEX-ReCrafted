/*
	Lane C painting-bbox-0927: a painting's Beta Entity.boundingBox reaches it
	for explosions and lightning bolts.

	Ground truth (decompile 740c583, ../mcb173-retromcp src_original):
	  - EntityPainting.setDirection :123 sets this.boundingBox to the inset
	    frame box (the same box painting_compute_geometry builds). Beta's
	    getBoundingBox() (the COLLISION accessor) stays null for paintings --
	    only EntityBoat/EntityMinecart override it -- so placement/pistons
	    still ignore a painting (test_painting_env pins that half).
	  - World.getEntitiesWithinAABBExcludingEntity selects by boundingBox, so
	    Explosion.doExplosionA :77-105 (entity pass, :100-103
	    attackEntityFrom(exploder, damage)) and EntityLightningBolt.onUpdate
	    :62-69 (box posX+-3, posY-3..posY+9, posZ+-3 -> Entity.
	    onStruckByLightning :1044-1051 -> dealFireDamage(5) :521-525 ->
	    attackEntityFrom) both reach a painting.
	  - EntityPainting.attackEntityFrom :200-208: if(!isDead) {
	    setEntityDead(); spawn EntityItem(Item.painting) } -- any damage from
	    any source breaks it with exactly one drop.

	Engine latch (documented deviation of ORDER only): the drop cannot be
	spawned inside the blast/bolt entity-dict walk, so the hit latches
	delay_destroy = 1 and the painting's own next server tick drops + removes
	it (the vehicles' blast_break_pending pattern). The tests pin both halves:
	still resident right after the hit, gone with exactly one Item.painting
	after one entity tick.

	Cases (argv[1]): box | lightning | explosion. `explosion` is RED
	(WILL_FAIL) until the lane A handoff (PAINTING-EXPL-handoff.patch,
	server_world.c expl_hit_entity) lands.
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
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/entity_painting.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"
#include "support/drain_quiesce.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define WY 100 /* wall plane -- well above the superflat terrain */

static struct server_local s;

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type) {
	server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = 0});
}

static struct block_data g_bd;
static struct block_info info_at(int x, int y, int z) {
	assert(server_world_get_block(&s.worlds[0], x, y, z, &g_bd));
	return (struct block_info) {
		.block = &g_bd, .neighbours = NULL, .x = x, .y = y, .z = z};
}

static struct entity* find_painting_at(int tx, int ty, int tz) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_PAINTING && e->data.painting.tile_x == tx
		   && e->data.painting.tile_y == ty && e->data.painting.tile_z == tz) {
			assert(!found);
			found = e;
		}
		dict_entity_next(it);
	}
	return found;
}

static size_t count_painting_drops(void) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_PAINTING)
			n++;
		dict_entity_next(it);
	}
	return n;
}

/* A stone wall slab at z = tz and a painting hung on its +Z (SOUTH) face
 * through the real ItemPainting.onItemUse hook. */
static struct entity* place_painting(int tx, int tz) {
	for(int x = tx - 3; x <= tx + 3; x++)
		for(int y = WY - 3; y <= WY + 3; y++)
			set(x, y, tz, BLOCK_STONE);
	struct item_data held = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	struct block_info on = info_at(tx, WY, tz);
	assert(items[ITEM_PAINTING]->onUseOnBlock(&s, &s.player, &held, &on,
											  SIDE_BACK));
	struct entity* p = find_painting_at(tx, WY, tz);
	assert(p && p->data.painting.direction == PAINTING_DIR_SOUTH);
	assert(p->delay_destroy < 0);
	return p;
}

/* box: the hit box IS setDirection's boundingBox; the collision accessor
 * stays null. */
static void case_box(void) {
	struct entity* p = place_painting(40, 10);
	assert(p->getBoundingBox == NULL);
	struct AABB box, ref;
	assert(entity_painting_hit_box(p, &box));
	vec3 pos;
	painting_compute_geometry(p->data.painting.art_index,
							  p->data.painting.direction, 40, WY, 10, pos,
							  &ref);
	assert(memcmp(&box, &ref, sizeof box) == 0);
	/* the box contains the entity position (setDirection centres it) */
	assert(box.x1 <= p->pos[0] && p->pos[0] <= box.x2);
	assert(box.y1 <= p->pos[1] && p->pos[1] <= box.y2);
	assert(box.z1 <= p->pos[2] && p->pos[2] <= box.z2);
	/* a non-painting never has one */
	struct entity other;
	memset(&other, 0, sizeof other);
	other.type = ENTITY_ITEM;
	assert(!entity_painting_hit_box(&other, &box));
	puts("  box: hit box == setDirection boundingBox; getBoundingBox NULL OK");
	/* L214: attackEntityFrom creates EntityItem in the painting's WorldObj,
	 * including allocation refusal (reserved id only, no item/RNG consumed). */
	string_t path;
	string_init_set_str(path, TMP_WORLD);
	server_world_create(&s.worlds[1], path, WORLD_DIM_NETHER, 4242, true);
	string_clear(path);
	s.worlds[1].local = &s;
	s.worlds[1].session = &s;
	s.worlds[1].world_index = 1;
	struct server_chunk* sc;
	assert(server_world_load_chunk(&s.worlds[1], 2, 0, &sc));
	struct entity own = {0};
	entity_painting(0, &own, true, &s.worlds[1], 0, PAINTING_DIR_SOUTH, 40, WY, 10);
	struct random_gen before = s.rand_src;
	uint32_t next_before = s.next_entity_id;
	size_t drops = count_painting_drops();
	server_local_test_fail_next_entity_alloc();
	entity_painting_drop_self(&own, &s);
	assert(count_painting_drops() == drops && s.next_entity_id == next_before + 1
		&& !memcmp(&before, &s.rand_src, sizeof(before)));
	entity_painting_drop_self(&own, &s);
	assert(count_painting_drops() == drops + 1);
	struct entity** pp = dict_entity_get(s.entities, s.next_entity_id);
	assert(pp && *pp && server_local_entity_world(&s, *pp) == &s.worlds[1]);
	assert((*pp)->data.item.item.id == ITEM_PAINTING && (*pp)->data.item.item.count == 1);
	puts("  L214: secondary painting drop belongs to its world; allocation refusal preserves RNG OK");
	server_world_discard(&s.worlds[1]);
}

/* lightning: a bolt whose +-3 / -3..+9 box overlaps the frame box breaks the
 * painting (one drop); a painting outside every bolt box is untouched. */
static void case_lightning(void) {
	const int TX = 40, TZ = 10, FX = 70, FZ = 30;
	struct entity* hit = place_painting(TX, TZ);
	struct entity* far = place_painting(FX, FZ);
	uint32_t hit_id = hit->id;
	size_t drops0 = count_painting_drops();

	s.weather.active_bolts_count = 0;
	world_lightning_bolt_spawn(&s, TX, WY, TZ + 2);
	server_weather_tick_lightning_bolts(&s);
	s.weather.active_bolts_count = 0;

	/* latched, not yet removed (engine ordering) */
	hit = find_painting_at(TX, WY, TZ);
	assert(hit && hit->id == hit_id);
	assert(hit->delay_destroy == 1
		   && "EntityLightningBolt box overlap -> attackEntityFrom: the "
			  "painting must be marked dead");
	assert(far->delay_destroy < 0);
	assert(count_painting_drops() == drops0);

	assert(server_local_test_tick_entities(&s));
	assert(find_painting_at(TX, WY, TZ) == NULL);
	assert(count_painting_drops() == drops0 + 1);
	assert(find_painting_at(FX, WY, FZ) == far);
	puts("  lightning: struck painting breaks with exactly one "
		 "Item.painting; unstruck one survives OK");
}

/* explosion: a blast whose (2*size+1) box overlaps the frame box breaks the
 * painting (one drop); a painting outside the blast is untouched. */
static int case_explosion(void) {
	const int TX = 40, TZ = 10, FX = 70, FZ = 30;
	struct entity* hit = place_painting(TX, TZ);
	struct entity* far = place_painting(FX, FZ);
	uint32_t hit_id = hit->id;
	size_t drops0 = count_painting_drops();

	/* power 1: explosionSize*2 = 2, centre ~1.5 blocks in front of the
	 * frame, in open air (exposure > 0, distance/size2 < 1). */
	server_world_explode(&s.worlds[0],
						 (vec3) {(float)TX + 0.5f, (float)WY + 0.5f,
								 (float)TZ + 2.5f},
						 1.0f);

	hit = find_painting_at(TX, WY, TZ);
	assert(hit && hit->id == hit_id);
	/* The RED clause exits 1 (not an assert abort): ctest's WILL_FAIL only
	 * inverts a clean non-zero exit, never a crash/abort code. */
	if(hit->delay_destroy != 1) {
		fprintf(stderr, "RED: Explosion.doExplosionA boundingBox overlap -> "
						"attackEntityFrom did not mark the painting dead "
						"(lane A handoff PAINTING-EXPL not applied)\n");
		return 1;
	}
	assert(far->delay_destroy < 0);

	assert(server_local_test_tick_entities(&s));
	assert(find_painting_at(TX, WY, TZ) == NULL);
	assert(count_painting_drops() == drops0 + 1);
	assert(find_painting_at(FX, WY, FZ) == far);
	puts("  explosion: blasted painting breaks with exactly one "
		 "Item.painting; unblasted one survives OK");
	return 0;
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "";
	char tag[64];
	snprintf(tag, sizeof tag, "test_painting_hit_%s_tmp", which);
	test_fixture_dir(tag, s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	/* Overworld-only fixture: worlds[0] named explicitly (world alias gate). */
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x9A1D71E5u; /* xorshift: never 0 */
	dict_entity_init(s.entities);
	for(int cz = -2; cz <= 5; cz++)
		for(int cx = -2; cx <= 9; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	s.player.x = 5000.0;
	s.player.y = 5000.0;
	s.player.z = 5000.0;

	if(strcmp(which, "box") == 0)
		case_box();
	else if(strcmp(which, "lightning") == 0)
		case_lightning();
	else if(strcmp(which, "explosion") == 0) {
		if(case_explosion() != 0) {
			g_drain_run = 0;
			thread_join(&drain);
			test_fixture_dir_cleanup(TMP_WORLD);
			return 1;
		}
	}
	else {
		fprintf(stderr, "usage: test_painting_hit box|lightning|explosion\n");
		return 2;
	}

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_painting_hit %s: PASS\n", which);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
