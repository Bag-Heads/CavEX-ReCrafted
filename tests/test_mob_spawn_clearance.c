/*
	2026-08-08 hardware-video caveat: the spider frozen in place on snow
	(VID_20260808_085407) -- a hostile spawned beside a tree trunk never moved
	again.

	Root cause pair, both fixed and both pinned here:

	1. SPAWN: mob_spawn_spot_ok tests one cell COLUMN, but a 1.4-wide spider's
	   physics box pokes 0.2 into all four neighbour columns; spawned at the
	   cell-centre snap beside a trunk it began already overlapping the
	   trunk's collision box. Beta never creates that state:
	   EntityLiving.getCanSpawnHere probes the WHOLE entity box --
	   checkIfAABBIsClear (no live preventEntitySpawning entity) &&
	   getCollidingBoundingBoxes(this, bb).size() == 0 && !getIsAnyLiquid(bb)
	   [740c583: EntityLiving.java:781; World.java:959/1357/1370] -- and every
	   natural spawn (EntityMob / EntityAnimal / EntitySlime supers,
	   EntityPigZombie verbatim) runs it. EntityWaterMob keeps ONLY the entity
	   clause. Intersections are STRICT (AxisAlignedBB.intersectsWith:
	   `maxX > minX && minX < maxX` per axis [AxisAlignedBB.java:185]):
	   face-touching -- every mob box vs its own floor -- must not refuse.

	2. PHYSICS: entity_intersection_threshold returned threshold 0 for a mover
	   ALREADY intersecting at old_pos, zeroing velocity on all three axes,
	   every tick, forever. Beta clips per-box, and calculateX/Y/ZOffset clip
	   ONLY an approach from OUTSIDE [AxisAlignedBB.java:104-195]: a box the
	   mover is already inside never clips, so an embedded entity slides out
	   while everything else (the floor!) still clips it. The non-embedded
	   paths are byte-identical before/after -- S1 pins the goldens.
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
#include "block/aabb.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "world.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_spawn_clearance_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70

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

static void set(int x, int y, int z, uint8_t type, uint8_t meta) {
	bool changed = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = (uint8_t)(meta & 0x0F)});
	if(!changed) {
		struct block_data live;
		assert(server_world_get_block(&s.worlds[0], x, y, z, &live));
		assert(live.type == type);
	}
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e)
			free(e);
	}
	dict_entity_reset(s.entities);
}

static struct entity* spawn_mob(int mob, float x, float y, float z) {
	uint32_t id = server_local_gen_id(&s);
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(**pp));
	assert(*pp);
	entity_monster(id, *pp, true, &s.worlds[0], mob);
	(*pp)->teleport(*pp, (vec3) {x, y, z});
	assert(server_local_test_entity_join(&s, id));
	return *pp;
}

static bool clear_ok(int mob, int x, int y, int z) {
	return server_local_test_mob_spawn_bbox_clear(&s, mob, (w_coord_t)x,
												  (w_coord_t)y, (w_coord_t)z);
}

/* Beta newborns have Java-default motionX/Y/Z = 0 (Entity.java:20-22;
 * constructors Entity:75-80 and EntityLiving:60-68). SpawnerAnimals:123-133
 * constructs, sets location/angles and joins without a generic impulse.
 * Slime constructors DO have their own RNG (EntitySlime:11-13); the C factory
 * currently defers those species rolls. Compare against that same factory so
 * this check does not forbid legitimate initialization draws. No tick runs. */
static void test_newborn_motion(void) {
	const int mobs[] = {MONSTER_CREEPER, MONSTER_PIG, MONSTER_CHICKEN,
		MONSTER_SLIME_MEDIUM};
	for(size_t i = 0; i < sizeof mobs / sizeof mobs[0]; i++) {
		clear_entities();
		uint32_t seed = 0x51A7C001u + (uint32_t)i;
		s.rand_src.seed = seed;
		struct entity factory;
		entity_monster(999, &factory, true, &s.worlds[0], mobs[i]);
		uint32_t after_factory = s.rand_src.seed;
		s.rand_src.seed = seed;
		/* Fractional coordinates also pin the exact-position helper used for
		 * slime children (EntitySlime.setEntityDead:102-108), not cell snapping. */
		vec3 pos = {8.25F, (float)Y + 0.5F, 8.75F};
		struct entity* m = server_local_spawn_monster_at(pos, mobs[i], &s);
		assert(m && m->data.monster.motion_d.valid);
		fprintf(stderr, "newborn id=%d motion=(%.17g,%.17g,%.17g) "
			"rng=%08x factory=%08x\n", mobs[i],
			m->data.monster.motion_d.motion[0],
			m->data.monster.motion_d.motion[1],
			m->data.monster.motion_d.motion[2], s.rand_src.seed, after_factory);
		for(int axis = 0; axis < 3; axis++) {
			assert(m->data.monster.motion_d.motion[axis] == 0.0);
			assert(m->vel[axis] == 0.0F);
			assert(m->pos[axis] == pos[axis]);
		}
		assert(s.rand_src.seed == after_factory);
		assert(m->data.monster.jump_delay == factory.data.monster.jump_delay);
		assert(m->data.monster.egg_timer == factory.data.monster.egg_timer);
	}
	clear_entities();
	fprintf(stderr, "  S0: newborn canonical/mirror motion zero; factory RNG preserved\n");
}

int main(void) {
	test_fixture_dir("test_spawn_clearance_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0xBEEFCAFEu;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	/* The entity scenarios occupy chunk (0,0). Beta's entity activation gate
	 * additionally requires the complete position +/-32 block neighborhood. */
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	/* Player parked in-chunk but far from every probe cell (its own box is a
	 * Beta preventEntitySpawning collider -- S3 tests it explicitly). */
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = -12.5;
	s.player.y = Y + PLAYER_EYE_HEIGHT;
	s.player.z = -12.5;
	s.player.health = MAX_PLAYER_HEALTH;

	/* A flat 16x16 stone platform at Y-1, air above, centred on (8, Y, 8),
	 * with a 2-tall rim so the S2 wander integration cannot stroll off the
	 * edge into the (empty superflat) void. */
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			set(x, Y - 1, z, BLOCK_STONE, 0);
			set(x, Y, z, BLOCK_AIR, 0);
			set(x, Y + 1, z, BLOCK_AIR, 0);
			set(x, Y + 2, z, BLOCK_AIR, 0);
		}
	for(int i = -1; i <= 16; i++)
		for(int lvl = 0; lvl <= 1; lvl++) {
			set(i, Y + lvl, -1, BLOCK_STONE, 0);
			set(i, Y + lvl, 16, BLOCK_STONE, 0);
			set(-1, Y + lvl, i, BLOCK_STONE, 0);
			set(16, Y + lvl, i, BLOCK_STONE, 0);
		}

	test_newborn_motion();

	/* ====== S1: outside-approach clipping is UNCHANGED (equivalence) ====== */
	{
		/* Wall two cells east of the zombie's cell. */
		set(10, Y, 8, BLOCK_STONE, 0);
		set(10, Y + 1, 8, BLOCK_STONE, 0);
		/* +0.01: the resting float entity_try_unstuck maintains in production
		 * ticks -- an EXACTLY-planted box face-touches its floor and clips at
		 * threshold 0 on every axis (pre-fix and post-fix alike; the strict
		 * exclusion deliberately does NOT except touching). The primitive
		 * sections drive entity_try_move without the unstuck step, so start
		 * from the floated state the physics actually runs in. */
		struct entity* z = spawn_mob(MONSTER_ZOMBIE, 8.5f, (float)Y + 0.01f, 8.5f);
		struct AABB zb;
		entity_make_local_bbox(z, &zb);

		/* Free move first: full advance, no flags. */
		bool cxz = false, og = false;
		z->vel[0] = 0.4f;
		entity_try_move(z, z->pos, z->vel, &zb, 0, &cxz, &og);
		assert(!cxz && fabsf(z->pos[0] - 8.9f) < 1e-4f
			   && "open-floor move must advance in full");

		/* Now sweep into the wall: face at x=10, zombie half-width 0.3 ->
		 * clipped resting x in (10-0.3-0.011, 10-0.3+1e-4]. The binary search
		 * stops on a <0.01 segment, so allow its granularity below the exact
		 * face contact but NEVER penetration past it. */
		for(int i = 0; i < 8; i++) {
			cxz = false;
			z->vel[0] = 0.4f;
			entity_try_move(z, z->pos, z->vel, &zb, 0, &cxz, &og);
		}
		assert(cxz && "wall must flag the X collision");
		assert(z->vel[0] == 0.0f && "wall hit must zero the axis velocity");
		assert(z->pos[0] <= 9.7f + 1e-4f && z->pos[0] > 9.7f - 0.011f
			   && "wall clip must rest at the face minus half-width");

		/* Landing: lift, then fall -- must ground exactly on the floor top
		 * (same search granularity), zero vel[1], set on_ground. */
		z->pos[1] = (float)Y + 0.75f;
		og = false;
		for(int i = 0; i < 12 && !og; i++) {
			cxz = false;
			z->vel[1] = -0.12f;
			entity_try_move(z, z->pos, z->vel, &zb, 1, &cxz, &og);
		}
		assert(og && "falling onto the floor must set on_ground");
		assert(z->vel[1] == 0.0f);
		assert(z->pos[1] >= (float)Y - 1e-4f && z->pos[1] < (float)Y + 0.011f
			   && "landing must rest on the floor top");
		clear_entities();
		set(10, Y, 8, BLOCK_AIR, 0);
		set(10, Y + 1, 8, BLOCK_AIR, 0);
		fprintf(stderr, "  S1: outside-approach clip goldens unchanged\n");
	}

	/* ====== S2: an EMBEDDED mob escapes (the freeze fix) ====== */
	{
		/* Tree trunk directly east: spider at (4.5, Y, 4.5) spans
		 * x 3.8..5.2 -- 0.2 inside the log's cell [5,6]. This is the video's
		 * frozen-spider geometry. */
		set(5, Y, 4, BLOCK_LOG, 0);
		set(5, Y + 1, 4, BLOCK_LOG, 0);
		/* Same +0.01 resting float as S1; the spider's 1.4-wide box still
		 * overlaps the trunk cell -- the embedded state under test. */
		struct entity* sp
			= spawn_mob(MONSTER_SPIDER, 4.5f, (float)Y + 0.01f, 4.5f);
		struct AABB spb;
		entity_make_local_bbox(sp, &spb);

		/* While EMBEDDED the overlapped log box clips NEITHER direction
		 * (Beta calculateOffset takes neither approach branch). Deeper in
		 * first: 4.5 -> 4.6 (box 3.9..5.3, still overlapping). */
		bool cxz = false, og = false;
		sp->vel[0] = 0.1f;
		entity_try_move(sp, sp->pos, sp->vel, &spb, 0, &cxz, &og);
		assert(fabsf(sp->pos[0] - 4.6f) < 1e-4f
			   && "an overlapped box clips neither direction (Beta)");

		/* Away: full advance, twice -- 4.6 -> 4.4 (still 0.1 embedded) ->
		 * 4.2 (fully out; box maxX 4.9). Pre-fix: threshold 0 froze both. */
		for(int i = 0; i < 2; i++) {
			cxz = false;
			sp->vel[0] = -0.2f;
			entity_try_move(sp, sp->pos, sp->vel, &spb, 0, &cxz, &og);
		}
		assert(fabsf(sp->pos[0] - 4.2f) < 1e-4f
			   && "embedded mob must slide OUT (away moves unclipped)");

		/* ESCAPED: the trunk is an ordinary wall again -- an approach from
		 * outside clips at the face (5.0 - half-width 0.7 = 4.3, search
		 * granularity below, never past). */
		cxz = false;
		sp->vel[0] = 0.2f;
		entity_try_move(sp, sp->pos, sp->vel, &spb, 0, &cxz, &og);
		assert(cxz && sp->vel[0] == 0.0f);
		assert(sp->pos[0] <= 4.3f + 1e-4f && sp->pos[0] > 4.3f - 0.011f
			   && "after escape, normal outside-approach clipping resumes");

		/* But the FLOOR -- not overlapped -- still grounds it. */
		sp->pos[1] = (float)Y + 0.4f;
		og = false;
		for(int i = 0; i < 10 && !og; i++) {
			cxz = false;
			sp->vel[1] = -0.1f;
			entity_try_move(sp, sp->pos, sp->vel, &spb, 1, &cxz, &og);
		}
		assert(og && sp->vel[1] == 0.0f
			   && "the floor under an embedded mob still clips/grounds");
		clear_entities();

		/* Full server-tick integration: the video repro. A spider spawned
		 * embedded in the trunk must WALK AWAY within 80 ticks (pre-fix it
		 * stayed at the spawn point forever, limbs frozen). */
		struct entity* sp2 = spawn_mob(MONSTER_SPIDER, 4.5f, (float)Y, 4.5f);
		float sx = sp2->pos[0], sz = sp2->pos[2];
		for(int t = 0; t < 60; t++) {
			server_world_begin_update(&s.worlds[0]);
			assert(server_local_test_tick_entities(&s));
		}
		float dx = sp2->pos[0] - sx, dz = sp2->pos[2] - sz;
		assert(sqrtf(dx * dx + dz * dz) > 0.05f
			   && "an embedded spider must move again (the frozen-mob fix)");
		clear_entities();
		set(5, Y, 4, BLOCK_AIR, 0);
		set(5, Y + 1, 4, BLOCK_AIR, 0);
		fprintf(stderr, "  S2: embedded spider slides out; floor still grounds\n");
	}

	/* ====== S3: the Beta getCanSpawnHere box gate ====== */
	{
		/* Open flat floor: everything passes (face-touching the floor must
		 * NOT refuse -- strict intersectsWith). */
		assert(clear_ok(MONSTER_ZOMBIE, 8, Y, 8));
		assert(clear_ok(MONSTER_SPIDER, 8, Y, 8));

		/* Tree trunk in the +X neighbour: the 0.6 biped still fits, the
		 * 1.4-wide spider does not -- the exact frozen-spider spawn. */
		set(9, Y, 8, BLOCK_LOG, 0);
		set(9, Y + 1, 8, BLOCK_LOG, 0);
		assert(clear_ok(MONSTER_ZOMBIE, 8, Y, 8)
			   && "a 0.6 biped beside a trunk is a legal Beta spawn");
		assert(!clear_ok(MONSTER_SPIDER, 8, Y, 8)
			   && "the 1.4 spider beside a trunk must be REFUSED");
		set(9, Y, 8, BLOCK_AIR, 0);
		set(9, Y + 1, 8, BLOCK_AIR, 0);

		/* Snow-covered ground (thin layers, meta 0 -> NO collision box, Beta
		 * BlockSnow.getCollisionBoundingBoxFromPool null): the spider's cell
		 * AND all four neighbours hold layers -- must remain spawnable. THE
		 * user regression: hostiles must spawn move-capable on snow. */
		for(int dx2 = -1; dx2 <= 1; dx2++)
			for(int dz2 = -1; dz2 <= 1; dz2++)
				set(8 + dx2, Y, 8 + dz2, BLOCK_SNOW, 0);
		assert(clear_ok(MONSTER_SPIDER, 8, Y, 8)
			   && "thin snow layers must not refuse a spider spawn");
		/* Thickened layer (meta 3 -> Beta HALF-SLAB collision box) in a
		 * neighbour: now the spider's overhang hits it, the biped does not. */
		set(9, Y, 8, BLOCK_SNOW, 3);
		assert(!clear_ok(MONSTER_SPIDER, 8, Y, 8)
			   && "a meta>=3 snow layer collides like Beta's half-box");
		assert(clear_ok(MONSTER_ZOMBIE, 8, Y, 8));
		for(int dx2 = -1; dx2 <= 1; dx2++)
			for(int dz2 = -1; dz2 <= 1; dz2++)
				set(8 + dx2, Y, 8 + dz2, BLOCK_AIR, 0);

		/* Liquid clause: water in the body volume refuses. */
		set(8, Y, 8, BLOCK_WATER_STILL, 0);
		assert(!clear_ok(MONSTER_ZOMBIE, 8, Y, 8)
			   && "getIsAnyLiquid must refuse an in-water land spawn");
		set(8, Y, 8, BLOCK_AIR, 0);

		/* Entity clause: a live mob in the box refuses; gone -> clear. */
		spawn_mob(MONSTER_ZOMBIE, 8.5f, (float)Y, 8.5f);
		assert(!clear_ok(MONSTER_ZOMBIE, 8, Y, 8)
			   && "checkIfAABBIsClear: a live mob blocks the spawn box");
		assert(clear_ok(MONSTER_ZOMBIE, 12, Y, 12));
		clear_entities();
		assert(clear_ok(MONSTER_ZOMBIE, 8, Y, 8));

		/* Player clause: the player is an EntityLiving too. */
		double opx = s.player.x, opy = s.player.y, opz = s.player.z;
		s.player.x = 8.5;
		s.player.y = Y + PLAYER_EYE_HEIGHT;
		s.player.z = 8.5;
		assert(!clear_ok(MONSTER_ZOMBIE, 8, Y, 8)
			   && "the player's own box blocks a spawn");
		s.player.x = opx;
		s.player.y = opy;
		s.player.z = opz;

		/* Ghast 4x4x4: open floor passes (face-touching floor + strict);
		 * a FENCE two cells out -- a non-cube collider the old normal-cube
		 * clearance could not see -- refuses. */
		assert(clear_ok(MONSTER_GHAST, 8, Y, 8));
		set(10, Y + 1, 8, BLOCK_FENCE, 0);
		assert(!clear_ok(MONSTER_GHAST, 8, Y, 8)
			   && "a fence inside the 4x4x4 body volume must refuse a ghast");
		set(10, Y + 1, 8, BLOCK_AIR, 0);

		/* S1-e: Beta checkIfAABBIsClear walks every non-dead
		 * preventEntitySpawning entity, including every EntityPlayer. The guest
		 * is intentionally bound but not gameplay-admitted: its body already
		 * exists geometrically. Exact face contact remains clear because
		 * AxisAlignedBB.intersectsWith is strict. [b1.7.3 server decompile:
		 * EntityLiving.java:762-764; World.java:1094-1100] */
		static struct server_player guest;
		memset(&guest, 0, sizeof guest);
		guest.has_pos = true;
		guest.health = 0;
		guest.y = Y + PLAYER_EYE_HEIGHT;
		guest.z = 8.5;
		s.guest_actor = &guest;
		s.guest_generation = 91;
		s.guest_gameplay = false;
		guest.x = 9.1; /* guest minX == candidate maxX == 8.8 */
		assert(clear_ok(MONSTER_ZOMBIE, 8, Y, 8)
			   && "guest face-touch must not count as an AABB intersection");
		guest.x = 8.5;
		assert(!clear_ok(MONSTER_ZOMBIE, 8, Y, 8)
			   && "a bound guest alone must suppress a physically overlapping spawn");
		s.guest_actor = NULL;
		s.guest_generation = 0;
		assert(clear_ok(MONSTER_ZOMBIE, 8, Y, 8)
			   && "departed guest coordinates must not suppress later spawns");

		/* The real anchor/member proximity predicate consumes no RNG. Preserve
		 * Cave's existing horizontal 16-block strict boundary for the host, then
		 * require the same decision for a guest while the host is far away.
		 * Beta's all-player authority is World.getClosestPlayer's playerEntities
		 * loop [SpawnerAnimals.java:101-134; World.java:1924-1937]. */
		double saved_x = s.player.x, saved_z = s.player.z;
		s.player.x = 8.5;
		s.player.z = 8.5;
		assert(server_local_test_mob_spawn_far_from_players(&s, 24.5F, 8.5F));
		assert(!server_local_test_mob_spawn_far_from_players(&s, 24.49F, 8.5F));
		/* Anchor generation already used the host-centred ring. Its flooring
		 * can put the cell centre at 15.6 from a fractional host coordinate;
		 * host-only must retain the old accepted path. A bound guest activates
		 * the explicit all-present guard for all three category anchors. */
		s.player.x = 0.9;
		s.player.z = 0.5;
		assert(server_local_test_mob_spawn_anchor_far_from_players(
			&s, 16.5F, 0.5F)
			&& "host-only fractional anchor must preserve the old ring decision");
		s.player.x = -12.5;
		s.player.z = -12.5;
		guest.x = 8.5;
		guest.z = 8.5;
		s.guest_actor = &guest;
		s.guest_generation = 92;
		assert(!server_local_test_mob_spawn_far_from_players(&s, 8.5F, 8.5F)
			   && "guest alone must exclude a point-blank natural-spawn candidate");
		assert(!server_local_test_mob_spawn_anchor_far_from_players(
			&s, 8.5F, 8.5F)
			&& "bound guest must activate the all-present anchor guard");
		s.guest_actor = NULL;
		s.guest_generation = 0;
		s.player.x = saved_x;
		s.player.z = saved_z;
		fprintf(stderr, "  S3: getCanSpawnHere gate -- blocks/liquid/entities/player\n");
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	fprintf(stderr, "test_mob_spawn_clearance: all sections OK\n");
	puts("test_mob_spawn_clearance: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
