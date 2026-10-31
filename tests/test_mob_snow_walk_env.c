/*
	END-TO-END guard for the "frozen snow mobs" fix (5fabbb5).

	WHY THIS EXISTS. The fix made block_snow's getBoundingBox honour its `entity`
	(COLLISION) flag: Beta returns a NULL collision box for a snow layer with
	metadata < 3, so a mob standing in a thin layer is not obstructed by it.
	Before the fix the collision query got the VISUAL box (0.125 * (meta+1)
	high), so a mob whose legs were inside a snow cell was permanently wedged.

	The existing coverage (tests/test_block_behavior_parity.c:467-486) calls
	blocks[BLOCK_SNOW]->getBoundingBox directly with a synthetic struct
	block_info -- no world, no entity, no tick. It proves the BOX SHAPE. It does
	NOT prove the symptom is gone, because the freeze is produced two layers
	away, by how the collision sweep consumes that box:

	    entity_intersection_threshold (entity.c:374-376) finds the entity's box
	    ALREADY intersecting at the OLD position, returns *threshold = 0.0F, and
	    entity_try_move then does vel[coord] = 0.0F (entity.c:447) and
	    pos[coord] = pos[coord]*(1-0) + tmp[coord]*0 == pos[coord] (entity.c:451)
	    -- the position is left BIT-IDENTICAL, on every axis, every tick.

	So the observable is exact: with the bug, horizontal displacement is not
	"small", it is ZERO. A sharp discriminator, not a tuned threshold.

	THE FIXTURE DETAIL THAT MATTERS -- READ BEFORE EDITING. aabb_intersection
	(block/aabb.c:127-131) compares with `<=`, so TOUCHING counts as
	intersecting. A mob placed at exactly the integer top of its floor
	(pos[1] == 70.0f over a stone cell spanning 69..70) therefore reads as
	already-intersecting and is frozen REGARDLESS of snow. Production never
	produces that state: a falling mob is resolved by
	entity_intersection_threshold's binary search, which stops once the bracket
	is under 0.01 wide, so a settled mob rests a hair ABOVE the integer boundary.
	Hand-placing a mob at the exact boundary makes this whole test pass for
	entirely the wrong reason. Hence: every mob here is DROPPED and settled by
	the real production tick, and the bare-stone baseline asserts the harness
	itself produces motion before any snow assertion is trusted.

	That also fixes the repro shape. Dropping a mob ONTO snow does not reproduce
	the bug even when it is present -- the mob simply lands on top of the phantom
	0.125 box and walks along it. The reported symptom is a mob that is ALREADY
	standing in the layer: natural spawning places mobs in the cell directly
	above the floor, which is exactly the cell snow occupies. So this test
	settles the mob first and then lays the snow around it -- snowfall onto a
	standing mob, which is precisely the state that wedges.

	WHY entity_try_unstuck CANNOT MASK A REGRESSION. entity_try_unstuck
	(entity.c:693-703) probes exactly ONE fixed +0.01 offset. A 0.125-high layer
	is 12.5x that nudge, so the probe box still intersects and the mob is never
	rescued. (The local PLAYER never showed the symptom because it got a stepped
	scan instead -- entity_local_player.c:187-198.) A reverted block_snow.c
	therefore fails this test rather than being papered over.

	TWO ALTITUDES, deliberately:
	  (A) a pinned-velocity single step through the real collision routine
	      (entity_try_move_axis) with the mob's REAL production bounding box --
	      isolates the collision path with zero AI nondeterminism;
	  (B) the full production server tick driven for 200 ticks -- proves the
	      symptom the maintainer actually observed, through entity_monster.c.

	Harness shape from tests/test_spider_leap_env.c, with its per-tick POSITION
	re-pin dropped: that test measures per-tick velocity, this one must measure
	accumulated displacement, and re-pinning pos would destroy the quantity
	under test.
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
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_mob_snow_walk_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define FLOOR_Y 69 /* stone platform       */
#define FEET_Y 70  /* mob feet / snow cell */
#define PAD 6      /* platform half-extent */

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

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {
									  .type = (uint8_t)type,
									  .metadata = (uint8_t)(meta & 0x0F),
								  }));
}

/* Idempotent silent write: server_world_write_block_raw returns NULL when the
 * write would not CHANGE the cell (server_world.c:734), so re-laying the same
 * block reports false. Assert the POST-CONDITION, not that a write happened. */
static void put(int x, int y, int z, int type, int meta) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	if(b.type != (uint8_t)type || (b.metadata & 0x0F) != (meta & 0x0F))
		assert(server_world_set_block_silent(
			&s.worlds[0], x, y, z,
			(struct block_data) {
				.type = (uint8_t)type,
				.metadata = (uint8_t)(meta & 0x0F),
			}));
	assert(server_world_get_block(&s.world, x, y, z, &b));
	assert(b.type == (uint8_t)type && (b.metadata & 0x0F) == (meta & 0x0F));
}

/* Blanket / clear the walkable layer the mob stands in. */
static void lay_snow(int meta) {
	for(int dx = -PAD; dx <= PAD; dx++)
		for(int dz = -PAD; dz <= PAD; dz++)
			put(8 + dx, FEET_Y, 8 + dz, BLOCK_SNOW, meta);
}

static void strip_snow(void) {
	for(int dx = -PAD; dx <= PAD; dx++)
		for(int dz = -PAD; dz <= PAD; dz++)
			put(8 + dx, FEET_Y, 8 + dz, BLOCK_AIR, 0);
}

/* entity_try_move_axis takes a zero-argument bbox factory. In production those
 * are entity_monster.c's per-species make_bbox_ret_* thunks, which are static
 * to that file, so the test cannot borrow them. Reproduce the SAME construction
 * the MOB_BBOX_THUNKS macro emits (entity_monster.c:170-178):
 *
 *     aabb_setsize_centered_offset(out, box_w, box_h, box_w,
 *                                  0.0f, box_h * 0.5f, 0.0f);
 *
 * i.e. a FEET-ANCHORED box -- its bottom sits at the entity's pos, and
 * entity_try_move_axis adds e->y_size itself (entity.c:706).
 *
 * Do NOT substitute entity_make_local_bbox: that offsets by
 * `height*0.5 - y_offset + y_size`, which both drops the box by y_offset and
 * double-counts y_size once entity_try_move_axis translates again.
 *
 * Dimensions come off the live entity (entity_set_size stores the def's
 * box_w/box_h into width/height), so this stays tied to the production table
 * rather than hard-coding per-species literals. */
static struct entity* g_bbox_entity = NULL;
static struct AABB mob_bbox_ret(void) {
	assert(g_bbox_entity);
	struct AABB b;
	aabb_setsize_centered_offset(&b, g_bbox_entity->width,
								 g_bbox_entity->height, g_bbox_entity->width,
								 0.0f, g_bbox_entity->height * 0.5f, 0.0f);
	return b;
}

static void despawn_all_mobs(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_MONSTER) {
				uint32_t id = e->id;
				dict_entity_erase(s.entities, id);
				again = true;
				break;
			}
			dict_entity_next(it);
		}
	}
}

/* Spawn a mob ABOVE the floor and let the production tick settle it. Returns
 * the settled entity; asserts it actually landed. Never hand-place the Y --
 * see the fixture note in the file header. */
static struct entity* spawn_settled(int monster_id) {
	vec3 pos = {8.5f, (float)FEET_Y + 1.0f, 8.5f};
	struct entity* m = server_local_spawn_monster(pos, monster_id, &s);
	assert(m && m->tick_server);
	/* server_local_spawn_monster_at seeds a RANDOM velocity from s->rand_src
	 * (server_local.c:1865-1871), and the wander AI adds its own drift; pin
	 * the HORIZONTAL components each tick so the landing spot is deterministic.
	 * vel[1] is deliberately left alone -- gravity has to accumulate for
	 * entity_try_move to detect the landing (it sets on_ground on a downward
	 * intersection), and zeroing it every tick makes the mob hover forever. */
	float prev_y = m->pos[1];
	for(int i = 0; i < 120; i++) {
		m->vel[0] = 0.0f;
		m->vel[2] = 0.0f;
		m->pos[0] = 8.5f;
		m->pos[2] = 8.5f;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped && "mob must survive the settle");
		prev_y = m->pos[1];
	}
	/* Assert SETTLED by observing the height stopped changing, rather than
	 * trusting on_ground -- which entity_monster's tick re-derives per tick and
	 * may clear on the very last iteration. */
	assert(m->pos[1] == prev_y);
	assert(m->pos[1] > (float)FLOOR_Y && m->pos[1] < (float)FEET_Y + 0.1f
		   && "mob must come to rest just above the platform top");
	/* FH-13: the per-iteration re-pin above (comment at the top of this loop)
	 * neutralizes wander drift on every settle tick EXCEPT the last one --
	 * pos[0]/pos[2] are pinned BEFORE tick_server runs, so that final tick's own
	 * AI-driven horizontal step (if any) survives into the returned position.
	 * Before FH-13's A* wander port this residual was a small, ~constant
	 * beeline-toward-the-zeroed-player step that happened to land around x
	 * ~8.34 (the deep-snow section's comment below); FH-13's real port makes
	 * wandering mostly-idle with occasional A*-routed bursts (faithful to Beta
	 * -- see entity_monster.c's FH-13 recalc-gate comments), so the residual is
	 * no longer a small fixed offset and can land close enough to x=9.0 that
	 * the deep-snow slide test's margin checks below (tuned around the OLD
	 * ~8.34 landing spot) fail. This test's subject is snow COLLISION, not
	 * wander AI, so finish the neutralization the original comment already
	 * intended: clear any path/heading the last tick picked up and re-pin to
	 * that SAME ~8.34 landing spot explicitly -- not a new value, the exact
	 * one the margin arithmetic below (0.2 slide + 0.3 probe + the 8-push
	 * convergence-to-9.0 fence) was already hand-verified against. */
	m->data.monster.path.length = 0;
	m->data.monster.direction[0] = 0.0f;
	m->data.monster.direction[1] = 0.0f;
	m->pos[0] = 8.34f;
	m->pos[2] = 8.34f;
	return m;
}

/* (A) One pinned-velocity step through the REAL collision routine. */
static float step_dx(struct entity* m) {
	g_bbox_entity = m;
	m->vel[0] = 0.2f;
	m->vel[1] = 0.0f;
	m->vel[2] = 0.0f;
	const float x0 = m->pos[0];
	bool collision = false, on_ground = false;
	entity_try_move_axis(m, 0, mob_bbox_ret, &collision, &on_ground);
	g_bbox_entity = NULL;
	return m->pos[0] - x0;
}

/* (B) Accumulated horizontal displacement through the FULL production tick.
 *     Velocity is re-pinned each tick (deterministic forward pressure) but
 *     POSITION never is -- displacement is the quantity under test. */
static float walk_distance(struct entity* m, int ticks) {
	const float x0 = m->pos[0], z0 = m->pos[2];
	for(int i = 0; i < ticks; i++) {
		m->vel[0] = 0.2f;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped && "the mob must survive the walk");
	}
	const float dx = m->pos[0] - x0, dz = m->pos[2] - z0;
	return sqrtf(dx * dx + dz * dz);
}

int main(void) {
	test_fixture_dir("test_mob_snow_walk_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.rand_src.seed = 0x51DEB001u; /* xorshift needs nonzero; reproducible */

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	for(int dx = -PAD; dx <= PAD; dx++)
		for(int dz = -PAD; dz <= PAD; dz++)
			set(8 + dx, FLOOR_Y, 8 + dz, BLOCK_STONE, 0);

	/* Snow must not be a full cube, or the whole premise is wrong. */
	assert(!block_is_normal_cube(BLOCK_SNOW));

	/* =====================================================================
	 * BASELINE on bare stone. Establishes that the harness produces motion,
	 * so a later zero is attributable to the snow and not a broken fixture.
	 * ===================================================================== */
	strip_snow();
	struct entity* m = spawn_settled(MONSTER_ZOMBIE);
	const float bare_step = step_dx(m);
	assert(bare_step > 0.0f
		   && "harness sanity: a settled mob on bare stone must step forward");
	const float bare_walk = walk_distance(m, 200);
	assert(bare_walk > 1.0f
		   && "harness sanity: a mob on bare stone must cover ground");
	despawn_all_mobs();

	/* =====================================================================
	 * (1) THIN SNOW (metadata 0-2) HAS NO COLLISION BOX. Snow is laid AROUND
	 *     an already-settled mob -- the spawn-into-snow / snowfall state that
	 *     actually wedges. This is the assertion that fails if block_snow.c's
	 *     `entity` gate is reverted: displacement collapses to EXACTLY 0.
	 * ===================================================================== */
	for(int meta = 0; meta <= 2; meta++) {
		strip_snow();
		m = spawn_settled(MONSTER_ZOMBIE);
		lay_snow(meta); /* snowfall onto a standing mob */

		const float step = step_dx(m);
		if(!(step > 0.0f)) {
			fprintf(stderr,
					"FAIL: mob frozen by snow metadata %d -- one "
					"entity_try_move_axis produced dx = %.9f (expected > 0; "
					"Beta gives metadata < 3 a NULL collision box)\n",
					meta, (double)step);
			assert(0 && "thin snow must not block horizontal movement");
		}
		/* Thin snow must be INVISIBLE to the collision sweep, not merely
		 * "less blocking" -- the step matches bare stone exactly. */
		assert(fabsf(step - bare_step) < 1e-6f
			   && "a thin snow layer must not slow the step at all");

		const float walk = walk_distance(m, 200);
		if(!(walk > 1.0f)) {
			fprintf(stderr,
					"FAIL: mob frozen by snow metadata %d over 200 production "
					"ticks -- travelled %.9f blocks (expected > 1.0)\n",
					meta, (double)walk);
			assert(0 && "thin snow must not freeze a mob across a real tick");
		}
		despawn_all_mobs();
	}

	/* =====================================================================
	 * (2) DEEP SNOW (metadata >= 3) DOES collide -- Beta gives it a FIXED
	 *     0.5-high box. The fix must not have turned snow into a ghost block
	 *     at every depth, so a future "just always return 0" simplification
	 *     fails here.
	 *
	 *     2026-08-08 REWRITE with the Beta per-box clipping semantics
	 *     [740c583 AxisAlignedBB.calculateXOffset]: a box the mover is
	 *     already strictly INSIDE never clips it, so a mob buried by deep
	 *     snowfall slides freely within its own cell's box (the old
	 *     `== 0.0f` here was pinning the engine's threshold-0 freeze, not
	 *     Beta). The box's EXISTENCE is proven the Beta-legal way instead:
	 *     the NEIGHBOUR cell's 0.5-box is an outside approach and must fence
	 *     the walk at the cell boundary. If deep snow ever stops returning a
	 *     collision box, the fence step below advances 0.2 and this fails.
	 * ===================================================================== */
	strip_snow();
	m = spawn_settled(MONSTER_ZOMBIE);
	lay_snow(4);
	{
		/* Buried mob (feet inside its own cell's 0.5-box): the first step
		 * slides out of the overlapped box -- FULL 0.2 advance, no clip.
		 * (spawn_settled leaves the start around x ~ 8.34 after its settle
		 * drift, so the whole slide stays inside cell 8's box.) */
		const float slide = step_dx(m);
		assert(fabsf(slide - 0.2f) < 1e-4f
			   && "a mob buried in deep snow slides within its own cell "
				  "(Beta: an overlapped box never clips)");
		assert(m->pos[0] + 0.3f < 9.0f
			   && "the slide must not have crossed the neighbour face yet");
		/* The NEIGHBOUR cell's deep-snow box is an approach from OUTSIDE --
		 * Beta clips it AT face contact. Push repeatedly: the walk must
		 * CONVERGE flush against the 9.0 face (within the binary search's
		 * 0.01 granularity) and never carry the box past it. If deep snow
		 * ever stops returning a collision box these pushes advance 0.2
		 * each and sail through; and without entity.c's 5mm exclusion core,
		 * a float-epsilon overshoot of the face would disarm the neighbour
		 * box and ghost-cascade the mob cell to cell. */
		for(int push = 0; push < 8; push++)
			(void)step_dx(m);
		assert(m->pos[0] + 0.3f <= 9.0f + 1e-4f
			   && "pushes must never carry the box past the snow fence");
		assert(m->pos[0] + 0.3f > 9.0f - 0.011f
			   && "the walk must converge flush against the fence face");
	}
	despawn_all_mobs();

	/* =====================================================================
	 * (3) The fix lives in the shared collision sweep, not in anything
	 *     zombie-specific, so other species with different boxes must be
	 *     freed by it too -- including a passive mob.
	 * ===================================================================== */
	const int SPECIES[] = {MONSTER_SKELETON, MONSTER_PIG};
	for(size_t i = 0; i < sizeof(SPECIES) / sizeof(SPECIES[0]); i++) {
		strip_snow();
		m = spawn_settled(SPECIES[i]);
		lay_snow(0);
		assert(step_dx(m) > 0.0f
			   && "thin snow must not freeze this species either");
		despawn_all_mobs();
	}

	printf("test_mob_snow_walk_env: mobs traverse thin snow (metadata 0-2), "
		   "deep snow still collides OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
