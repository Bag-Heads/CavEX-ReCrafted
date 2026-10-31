/*
	END-TO-END guard for the mob step-ascent SNAP (field bug 2026-08-31:
	zombie pigmen visually glitch -- stutter/snap -- when jumping up the side
	of a block in Nether SP).

	ROOT CAUSE. Beta 1.7.3 spreads a mob jump over ~7 ticks: jump() sets
	motionY 0.42, gravity drains 0.08/tick, apex ~1.17 blocks [740c583:
	EntityLiving.jump]. This engine's mob gravity is 2.0/tick (friction 0.9),
	so a multi-tick arc is impossible -- the whole ascent must fit in ONE
	tick. The old auto-jump impulse 4.0 produced a single-tick rise of
	(4.0 - 2.0) * 0.9 = +1.8 blocks (a 50ms teleport past the step top),
	followed by a ~0.8-block fall back onto the step: a pop + bounce, which
	is exactly the reported stutter.

	THE FIX under test, two halves:
	  (a) impulse trimmed to MOB_AUTO_JUMP_VEL_Y (rise ~1.17 = Beta's own
	      jump apex; the SLIME_HOP_VEL_Y 3.2 precedent in combat.h documents
	      this exact engine-units mapping) -- kills the overshoot bounce;
	  (b) the single-tick rise is banked into e->y_size and released 60%/tick
	      (Beta's ySize ease: `ySize *= 0.4F` at the top of every moveEntity,
	      `posY = minY + yOffset - ySize` [740c583: Entity.moveEntity]) -- the
	      collision box lands on the step immediately, while the OBSERVABLE
	      pos[1] (what CRPC_ENTITY_MOVE ships and the client renders) eases up
	      over ~4 ticks. Beta uses the identical mechanism for slab step-ups;
	      here it also stands in for the jump arc this engine cannot integrate.

	OBSERVABLES (sharp, not tuned):
	  - max single-tick rise of pos[1]: 1.8 before the fix, <= ~0.71 after
	    (first release tick of a 1.17 bank). Discriminator at 1.0.
	  - peak pos[1]: step_top + 0.8 before, step_top (+epsilon) after.
	    Discriminator at step_top + 0.35.
	  - FUNCTIONAL floor: the mob must still actually climb the step and
	    settle on it with y_size fully decayed -- guards against a future
	    over-trim of the impulse (a jump that no longer clears the step
	    passes both smoothness asserts by never ascending at all).

	Harness shape from tests/test_mob_snow_walk_env.c (settle-by-production-
	tick fixture note applies here too: never hand-place the resting Y).
	The mob is a PIG ZOMBIE because that is the fielded report, but the fix
	is species-generic (shared auto-jump + shared movement tail).

	AUDIT 2026-09-01 (A1/A2) -- the ease's first cut had two regressions and
	the stutter had a deeper root, all guarded below:
	  ROOT CAUSE of the stutter: the auto-jump gate's probe box was expanded
	  0.01 DOWN into the floor and never offset sideways, so under the
	  strict-overlap rule it was true for every grounded mob; the decision
	  fell to the one-unit-ahead cell test, which fired up to ~0.7 blocks
	  before the wall. The mob hopped early, cleared nothing, fell back and
	  hopped again AT the wall: pop + bounce. The gate now asks Beta's
	  question -- would this tick's XZ move collide? [740c583: EntityCreature.
	  updatePlayerActionState isCollidedHorizontally] -- so there is ONE hop.
	  A2: banking EVERY >0.5 rise also banked free hops (slime hop, a jump
	  that clears nothing): the box arced while pos was held, then dragged
	  BELOW the floor for ~3 ticks. The bank now applies only when the risen
	  box has ground within Beta's 0.5 stepHeight beneath it, probed AFTER
	  the XZ move (CASE B: free hop visible, never below the floor; the step
	  case gains a min-Y assert).
	  A1: the knockback compose read the POST-gravity velocity (-1.8 under
	  GRAVITY 2.0) and produced -0.5 -- no pop. It now composes from the
	  pre-force velocity (CASE C: a grounded hit lifts the mob ~0.4).

	MOB MOTION UNITS S2 (docs/intent/mob-motion-units.md; Astra rulings
	2026-09-27, Q2): the engine-units auto-jump (3.3), its collision probe and
	the ySize jump bank are RETIRED -- a mob now jumps exactly as Beta:
	EntityCreature.updatePlayerActionState sets isJumping when the current
	path waypoint is above floor(boundingBox.minY + 0.5), and onLivingUpdate
	jump()s motionY = (double)0.42F on a grounded tick; gravity (-0.08D,
	*0.98F) integrates the real ~7-tick arc (apex ~1.25). The fielded scenario
	(178859f: a pig zombie climbing the side of a block must not stutter,
	snap, overshoot or dip) is kept, with the same observables, driven the
	Beta way: a path whose waypoint sits on the shelf. The old cases' engine
	units (the injected post-gravity rise, the deferred knockback compose)
	are re-expressed on the doubles through the production entry points.
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
#include "entity/combat.h" /* KNOCKBACK_*, MOB_KNOCKBACK_TICKS */
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/mob_motion.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_mob_step_ascent_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define FLOOR_Y 69 /* platform blocks -> walk surface at 70.0 */
#define SHELF_Y 70 /* step blocks    -> step top at 71.0     */
#define STEP_X 11  /* first shelf column: wall face at x=11.0 */
#define STEP_TOP 71.0f

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

/* Settle by the REAL production tick -- see test_mob_snow_walk_env.c's fixture
 * note: a hand-placed integer Y reads as already-intersecting and freezes. */
static struct entity* spawn_settled(int monster_id) {
	vec3 pos = {8.5f, (float)SHELF_Y + 1.0f, 8.5f};
	struct entity* m = server_local_spawn_monster(pos, monster_id, &s);
	assert(m && m->tick_server);
	float prev_y = m->pos[1];
	struct mob_motion* md = &m->data.monster.motion_d;
	mob_motion_set_velocity(m, md, 0.0, 0.0, 0.0); /* no spawn kick */
	for(int i = 0; i < 120; i++) {
		m->data.monster.path.length = 0;
		m->data.monster.knockback_time = 100; /* no walk push */
		bool reaped = m->tick_server(m, &s);
		assert(!reaped && "mob must survive the settle");
		prev_y = m->pos[1];
	}
	assert(m->pos[1] == prev_y);
	assert(m->pos[1] > (float)FLOOR_Y + 0.9f && m->pos[1] < (float)SHELF_Y + 0.1f
		   && "mob must come to rest on the lower platform");
	m->data.monster.path.length = 0;
	m->data.monster.direction[0] = 0.0f;
	m->data.monster.direction[1] = 0.0f;
	return m;
}

int main(void) {
	test_fixture_dir("test_mob_step_ascent_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	s.rand_src.seed = 0x51DEB002u; /* xorshift needs nonzero; reproducible */

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	/* Lower platform + a one-block shelf east of it (wall face at x=11). */
	for(int x = 4; x <= 14; x++)
		for(int z = 5; z <= 12; z++)
			set(x, FLOOR_Y, z, BLOCK_STONE, 0);
	for(int x = STEP_X; x <= 14; x++)
		for(int z = 5; z <= 12; z++)
			set(x, SHELF_Y, z, BLOCK_STONE, 0);

	struct entity* m = spawn_settled(MONSTER_PIG_ZOMBIE);

	/* Drive east into the step through the FULL production tick. Velocity is
	 * re-pinned each tick at the production walk cap (the idle AI zeroes the
	 * steer heading via mob_path_follow_tail, so pinning `direction` produces
	 * no motion -- same drive shape as test_mob_snow_walk_env's
	 * walk_distance); everything downstream of the walk push (collision,
	 * auto-jump, gravity, the ease under test) stays production. */
	float y_prev = m->pos[1];
	float max_rise = 0.0f;
	float peak_y = m->pos[1];
	float min_y = m->pos[1]; /* observable Y must never go BELOW the floor */
	int settled_tick = -1;
	struct mob_motion* mmd = &m->data.monster.motion_d;
	for(int t = 0; t < 400; t++) {
		/* The Beta way up a block: a path whose current waypoint sits ON the
		 * shelf (y 71 > floor(minY + 0.5) = 70 while on the lower floor), so
		 * isJumping is set and every grounded tick jump()s (double)0.42F. The
		 * horizontal drive is pinned at the production walk cap through the
		 * mutation API, with the walk push suppressed (knockback window). */
		m->data.monster.path.points[0] = (struct path_waypoint) {STEP_X + 2, SHELF_Y + 1, 8};
		m->data.monster.path.length = 1;
		m->data.monster.path.index = 0;
		m->data.monster.knockback_time = 100;
		mob_motion_set_velocity(m, mmd, (double)0.18f, mmd->motion[1], 0.0);
		bool reaped = m->tick_server(m, &s);
		assert(!reaped && "mob must survive the climb");

		float y = m->pos[1]; /* the OBSERVABLE Y: shipped to the client */
		if(y != y_prev) /* per-tick ascent trace: the chartable observable */
			fprintf(stderr, "TRACE %d x=%.3f y=%.4f ysz=%.4f vx=%.3f vy=%.3f\n",
					t, (double)m->pos[0], (double)y, (double)m->y_size,
					(double)m->vel[0], (double)m->vel[1]);
		float rise = y - y_prev;
		if(rise > max_rise)
			max_rise = rise;
		if(y > peak_y)
			peak_y = y;
		if(y < min_y)
			min_y = y;
		y_prev = y;

		if(settled_tick < 0 && m->pos[0] > (float)STEP_X + 0.2f
		   && fabsf(y - STEP_TOP) < 0.05f && m->y_size < 0.05f) {
			settled_tick = t;
			break; /* stop driving: past here it just walks off the shelf */
		}
	}

	fprintf(stderr,
			"step ascent: max single-tick rise %.3f, peak y %.3f, final "
			"(%.2f, %.3f), y_size %.3f, settled@%d\n",
			(double)max_rise, (double)peak_y, (double)m->pos[0],
			(double)m->pos[1], (double)m->y_size, settled_tick);

	/* Harness sanity: the drive actually reached the wall. */
	assert(m->pos[0] > 10.0f && "harness sanity: mob must reach the step");

	/* FUNCTIONAL: the climb still succeeds (guards against over-trimming the
	 * jump impulse until the mob can no longer clear a 1-block step). */
	assert(settled_tick >= 0
		   && "mob must climb the 1-block step and settle on it");
	assert(fabsf(m->pos[1] - STEP_TOP) < 0.05f && m->y_size < 0.05f
		   && "mob must rest ON the step with the ySize ease fully decayed");

	/* SMOOTHNESS: the observable ascent must be eased, not a 1-tick pop.
	 * Before the fix: one +1.8 tick. After: <= ~0.71 (60% release of the
	 * banked 1.17 rise). Sharp midpoint discriminator. */
	assert(max_rise < 1.0f
		   && "observable Y must never rise a full block in a single tick");

	/* NO OVERSHOOT: before the fix the mob peaked 0.8 above the step top and
	 * bounced back down; the trimmed impulse + ease never overshoot. */
	assert(peak_y < STEP_TOP + 0.35f
		   && "observable Y must not overshoot the step top (bounce)");

	/* NO DIP (audit 2026-09-01 A2): the first ease banked EVERY >0.5 rise,
	 * including the early hops that cleared nothing, and each of those pulled
	 * the observable pos BELOW the floor for ~3 ticks while the box fell back
	 * faster than the release. One clean at-the-wall hop, banked only when it
	 * ends on the ledge, never takes pos under where it started. */
	assert(min_y > (float)FLOOR_Y + 0.99f
		   && "observable Y must never dip below the lower floor");

	printf("test_mob_step_ascent_env: 1-block step ascent is eased (max "
		   "rise %.2f, no overshoot) and still succeeds\n",
		   (double)max_rise);

	/* ================================================================
	 * CASE B -- FREE HOP (audit 2026-09-01 A2, regression from the ease):
	 * a jump that lands back on the SAME level (slime hop, an auto-jump that
	 * clears nothing) must NOT be banked into y_size. The first cut banked
	 * every >0.5 rise: the collision box arced up and down while the
	 * observable pos was held at 0 and then dragged BELOW the floor for ~3
	 * ticks (box falling faster than the 60%/tick release) -- the hop was
	 * invisible and the mob sank into the ground. Observables:
	 *   - min pos[1] over the hop >= rest (never below the floor);
	 *   - peak pos[1] >= rest + 1.0 (the ~1.17 apex is actually visible);
	 *   - it lands back at rest with y_size fully drained.
	 * Fresh mob on the open floor (x 8.5: its 0.6-wide box cannot reach the
	 * shelf wall at x=11), the auto-jump's post-gravity rise injected as
	 * vel[1] -- (MOB_AUTO_JUMP_VEL_Y - GRAVITY 2.0) * 0.9 -- exactly what the
	 * production auto-jump hands the next tick's axis-1 move.
	 * ================================================================ */
	struct entity* h = spawn_settled(MONSTER_PIG_ZOMBIE);
	const float rest_y = h->pos[1];
	/* EntityLiving.jump on the grounded mob (production entry point). */
	mob_motion_jump(h, &h->data.monster.motion_d);
	float hop_min = rest_y, hop_peak = rest_y;
	int landed_tick = -1;
	for(int t = 0; t < 60; t++) {
		h->data.monster.path.length = 0;
		h->data.monster.knockback_time = 100;
		bool reaped = h->tick_server(h, &s);
		assert(!reaped && "mob must survive the free hop");
		float y = h->pos[1];
		fprintf(stderr, "HOP %d %.4f %.4f\n", t, (double)y, (double)h->y_size);
		if(y < hop_min)
			hop_min = y;
		if(y > hop_peak)
			hop_peak = y;
		/* 0.01: the mover's contact interpolation leaves the box a hair
		 * (~0.001) above the floor on landing, same as any real landing. */
		/* (no on_ground test: the tick's gravity step clears the flag at the
		 * end of EVERY tick, so it is never true after tick_server returns) */
		if(landed_tick < 0 && t > 0 && fabsf(y - rest_y) < 0.01f
		   && h->y_size == 0.0f) {
			landed_tick = t;
			break;
		}
	}
	fprintf(stderr, "free hop: min %.3f peak %.3f rest %.3f landed@%d\n",
			(double)hop_min, (double)hop_peak, (double)rest_y, landed_tick);
	assert(hop_min > rest_y - 0.01f
		   && "free hop: observable Y must never dip below the floor");
	assert(hop_peak > rest_y + 1.0f
		   && "free hop: the ~1.17 apex must be visible (not banked away)");
	assert(landed_tick >= 0
		   && "free hop: mob must land back at rest with y_size drained");

	/* ================================================================
	 * CASE C -- GROUNDED KNOCKBACK POP (audit 2026-09-01 A1, regression
	 * from b897db0): Beta EntityLiving.knockBack halves the PRE-hit motionY
	 * (0 on the ground) and adds 0.4 -> a hit mob hops 0.4. The composed
	 * form read the POST-gravity velocity (-1.8 under this engine's 2.0/tick
	 * gravity) and produced -0.5: no hop at all. Same entity, now resting on
	 * the floor after CASE B; emulate the SRPC_ENTITY_ATTACK write (horizontal
	 * impulse + the knockback_time latch), then read the tick that composes
	 * the vertical impulse and the tick that moves on it.
	 * ================================================================ */
	/* Mob motion units S2: the SRPC_ENTITY_ATTACK path now composes
	 * EntityLiving.knockBack AT THE HIT (attacker 1 block west). A resting
	 * Beta mob carries motionY (0 - 0.08D) * (double)0.98F (gravity runs
	 * every tick, the floor clip zeroes it inside the next move), so the pop
	 * is pre / 2.0D + (double)0.4F = 0.3608 -- still a visible hop. */
	double pre_y = h->data.monster.motion_d.motion[1];
	assert(pre_y == (0.0 - 0.08) * (double)0.98F && "a resting Beta mob");
	mob_motion_knockback(h, &h->data.monster.motion_d, -1.0, 0.0, NULL);
	h->data.monster.knockback_time = MOB_KNOCKBACK_TICKS;
	fprintf(stderr, "knockback: composed motionY %.4f\n",
			h->data.monster.motion_d.motion[1]);
	assert(h->data.monster.motion_d.motion[1] == pre_y / 2.0 + (double)0.4F
		   && "grounded knockback must compose to Beta's pop, not sink");
	bool reaped_k = h->tick_server(h, &s);
	assert(!reaped_k);
	fprintf(stderr, "knockback: pos[1] after move %.4f (rest %.4f) y_size %.4f\n",
			(double)h->pos[1], (double)rest_y, (double)h->y_size);
	assert(h->pos[1] > rest_y + 0.3f
		   && "grounded knockback must visibly lift the mob (~0.4)");
	assert(h->y_size == 0.0f && "a 0.4 pop is below the ySize bank gate");
	float kb_min = h->pos[1];
	int kb_landed = -1;
	for(int t = 0; t < 60; t++) {
		h->data.monster.path.length = 0;
		bool reaped = h->tick_server(h, &s);
		assert(!reaped);
		if(h->pos[1] < kb_min)
			kb_min = h->pos[1];
		if(fabsf(h->pos[1] - rest_y) < 0.01f) {
			kb_landed = t;
			break;
		}
	}
	assert(kb_min > rest_y - 0.01f
		   && "knockback: observable Y must never dip below the floor");
	assert(kb_landed >= 0 && "knockback: mob must land back on the floor");

	printf("test_mob_step_ascent_env: free hop visible (peak +%.2f, no dip) "
		   "and grounded knockback pops (+%.2f)\n",
		   (double)(hop_peak - rest_y), (double)KNOCKBACK_V);

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
