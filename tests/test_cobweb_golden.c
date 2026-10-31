/*
	test_cobweb_golden -- next-gaps-0924/W2 MOVE card 3 (P12). FAITHFULNESS
	golden (Beta 1.7.3, mcb173-retromcp src_original), authored BEFORE the
	port. Intent: docs/intent/p12-cobweb.md.

	Spec:
	  BlockWeb.onEntityCollidedWithBlock:10-12   entity.isInWeb = true, reached
	      through Entity.moveEntity's post-move per-cell dispatch :460-476
	      (floor(min+0.001) .. floor(max-0.001) inclusive, every block id > 0).
	  Entity.moveEntity:262-268   at the top of the NEXT move:
	      isInWeb = false; dx *= 0.25D; dy *= (double)0.05F; dz *= 0.25D;
	      motionX = motionY = motionZ = 0.0D.
	  EntityLiving.moveEntityWithHeading:478-497,532-535   the walk: moveFlying
	      (stone accel 0.1F*(0.16277136F/0.546F^3) = 0.09999999F, input 0.98F
	      -> motionZ 0.09799999F) BEFORE the move, friction after; the web move
	      is therefore 0.09799999F*0.25 = 0.0244999975 each tick, with the
	      motion zero after it (no accumulation: the web crawl).
	  EntityLiving.jump:659-661   motionY = 0.42F -> web 0.42F*0.05F = 0.021.
	  EntityItem.onUpdate:45-66   motionY -= 0.04F before the move.

	Values are hand-derived from those lines with Java float/double semantics
	(the quarter is exact in binary; 0.05F = 0.05000000074505806). Nothing here
	ran CaveWii. RED on the pre-fix base: the player crosses at 0.098/tick, mob
	and item at their full motion.
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
#include "support/mob_motion_fixture.h"

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/input_owner.h"
#include "platform/thread.h"
#include "world.h"

#define VEL_TOL 1e-6
#define POS_TOL 2e-5

static int s_checks, s_failures, s_section_start;
static const char* s_last_failed;

static void fail(const char* what, const char* msg) {
	s_failures++;
	if(s_last_failed != what)
		fprintf(stderr, "FAIL %s: %s\n", what, msg);
	s_last_failed = what;
}

static void near_or_die(const char* what, int tick, double got, double want,
						double tol) {
	s_checks++;
	if(fabs(got - want) > tol) {
		char msg[160];
		snprintf(msg, sizeof(msg),
				 "tick %d got %.10f want %.10f (|d|=%.3g > %.1g)", tick, got,
				 want, fabs(got - want), tol);
		fail(what, msg);
	}
}

static void section_done(const char* msg) {
	printf("%s %s\n", s_failures == s_section_start ? "PASS" : "RED ", msg);
	fflush(stdout);
	s_section_start = s_failures;
}

/* ----------------------------------------------------------- client half */

static struct client_session s_owner;

static void cfill(int x0, int x1, int y0, int y1, int z0, int z1, uint8_t type) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				world_set_block(&gstate.world, x, y, z,
								(struct block_data) {.type = type}, false);
}

static void walker(struct entity* e, float x, float z) {
	memset(e, 0, sizeof *e);
	entity_local_player(91, e, &gstate.world);
	e->pos[0] = x;
	e->pos[1] = 101.0F + 1.62F;
	e->pos[2] = z;
	e->orient[1] = GLM_PI_2;
	e->on_ground = true;
	e->data.local_player.capture_input = true;
	memset(&s_owner, 0, sizeof s_owner);
	s_owner.local_player = e;
	s_owner.oxygen = MAX_OXYGEN;
	/* W1A settle: Beta's standing state (motionY -0.0784, onGround) */
	struct input_player_frame settle = {.connected = true};
	for(int t = 0; t < 4; t++)
		assert(!entity_local_player_walk_owned(e, &s_owner, &settle));
	assert(e->on_ground);
	e->pos[0] = x;
	e->pos[2] = z;
	e->vel[0] = e->vel[2] = 0.0F;
}

static void step(struct entity* e, bool forward, bool jump) {
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_FORWARD] = forward;
	frame.down[IB_JUMP] = jump;
	assert(!entity_local_player_walk_owned(e, &s_owner, &frame));
}

static void test_player(void) {
	cfill(2, 8, 100, 100, 2, 14, BLOCK_STONE);
	/* The walker settles INSIDE the web cell (5,101,5): the settle's last
	 * sweep already raised isInWeb, as Beta's would for a player standing
	 * in a web. */
	cfill(5, 5, 101, 101, 5, 5, BLOCK_COBWEB);
	struct entity e;
	walker(&e, 5.5F, 5.5F);
	const double crawl = 0.0244999975; /* 0.09799999F * 0.25 */
	for(int t = 0; t < 4; t++) {
		step(&e, true, false);
		near_or_die("P forward z", t, e.pos[2], 5.5 + (t + 1) * crawl, POS_TOL);
		near_or_die("P forward vz zeroed", t, e.vel[2], 0.0, 0.0);
		near_or_die("P forward vy (0 - 0.08D) * 0.98F", t, e.vel[1],
					-0.0784000009, VEL_TOL);
	}
	/* jump in the web: 0.42F -> * 0.05F lift, then motion zero, gravity */
	walker(&e, 5.5F, 5.5F);
	step(&e, false, true);
	near_or_die("P jump lift", 0, (double)e.pos[1] - 1.62, 101.0 + 0.021000000312924385, 2e-4);
	near_or_die("P jump vy after", 0, e.vel[1], -0.0784000009, VEL_TOL);
	/* neighbouring cell: the player's box (z 5.2..5.8) never touches (5,101,4) */
	cfill(5, 5, 101, 101, 5, 5, BLOCK_AIR);
	cfill(5, 5, 101, 101, 4, 4, BLOCK_COBWEB);
	walker(&e, 5.5F, 5.5F);
	step(&e, true, false);
	near_or_die("P beside the web: full walk", 0, e.pos[2], 5.597999990, POS_TOL);
	cfill(2, 8, 100, 101, 2, 14, BLOCK_AIR);
	section_done("P player: web crawl 0.0245/tick, motion zeroed, jump lift 0.021, neighbour cell inert");
}

/* ----------------------------------------------------------- server half */

static char s_fixture[128];
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

static void sset(int x, int y, int z, int type) {
	struct block_data cur;
	if(server_world_get_block(&s.worlds[0], x, y, z, &cur) && cur.type == type
	   && cur.metadata == 0)
		return;
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {.type = (uint8_t)type}));
}

static struct entity* spawn_settled(float x, float z) {
	vec3 pos = {x, 102.0F, z};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE, &s);
	assert(m && m->tick_server);
	for(int i = 0; i < 120; i++) {
		m->vel[0] = m->vel[2] = 0.0f;
		m->pos[0] = x;
		m->pos[2] = z;
		test_mob_sync(m); /* motion units S2 */
		m->data.monster.path.length = 0;
		m->data.monster.knockback_time = 100;
		assert(!m->tick_server(m, &s));
	}
	assert(floorf(m->pos[1]) == 101.0F && m->pos[1] < 101.05F);
	m->data.monster.path.length = 0;
	m->data.monster.direction[0] = m->data.monster.direction[1] = 0.0f;
	m->vel[0] = m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	return m;
}

static void mob_tick(struct entity* m) {
	m->data.monster.knockback_time = 100;
	m->data.monster.path.length = 0;
	assert(!m->tick_server(m, &s));
}

static void test_mob(void) {
	struct entity* m = spawn_settled(5.5F, 8.5F);
	sset(5, 101, 8, BLOCK_COBWEB); /* placed after the settle */
	mob_tick(m);                    /* the sweep raises isInWeb */
	m->vel[0] = 0.1F;
	test_mob_sync(m); /* motion units S2 */
	mob_tick(m); /* 0.1 * 0.25 = 0.025, then motion zero */
	near_or_die("M x", 0, m->pos[0], 5.525, POS_TOL);
	near_or_die("M vx zeroed", 0, m->vel[0], 0.0, 0.0);
	mob_tick(m); /* still in the web, nothing carried */
	near_or_die("M x holds", 1, m->pos[0], 5.525, POS_TOL);
	section_done("M mob: quarter move then dead stop in a web");
}

static struct entity* make_item(bool server, void* world, float x, float y, float z) {
	struct entity* e = malloc(sizeof *e);
	assert(e);
	memset(e, 0, sizeof *e);
	entity_item(900, e, server, world,
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	e->teleport(e, (vec3) {x, y, z});
	glm_vec3_zero(e->vel);
	e->data.item.was_in_water = true;
	return e;
}

static void test_item(void) {
	sset(5, 101, 12, BLOCK_COBWEB);
	struct entity* it = make_item(true, &s.worlds[0], 5.5F, 101.125F, 12.5F);
	assert(!it->tick_server(it, &s)); /* sweep raises isInWeb */
	it->vel[0] = 0.1F;
	assert(!it->tick_server(it, &s));
	near_or_die("I server x", 0, it->pos[0], 5.525, POS_TOL);
	near_or_die("I server vx zeroed", 0, it->vel[0], 0.0, 0.0);
	near_or_die("I server vy zeroed", 0, it->vel[1], 0.0, 0.0);
	free(it);
	/* the client prediction, same numbers */
	cfill(2, 8, 100, 100, 10, 14, BLOCK_STONE);
	cfill(5, 5, 101, 101, 12, 12, BLOCK_COBWEB);
	struct entity* ci = make_item(false, &gstate.world, 5.5F, 101.125F, 12.5F);
	assert(!ci->tick_client(ci));
	ci->vel[0] = 0.1F;
	assert(!ci->tick_client(ci));
	near_or_die("I client x", 0, ci->pos[0], 5.525, POS_TOL);
	near_or_die("I client vx zeroed", 0, ci->vel[0], 0.0, 0.0);
	free(ci);
	section_done("I item: quarter move then dead stop (server tick + client prediction)");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_cobweb_golden_tmp", s_fixture, sizeof s_fixture);
	assert(clin_init());
	blocks_init();
	items_init();

	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	test_player();

	MKDIR(s_fixture);
	MKDIR(test_fixture_join(s_fixture, "/region"));
	remove(test_fixture_join(s_fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_fixture);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = 0x51DEB002u;
	s.entity_rng.seed = 0x13579BDFu;
	dict_entity_init(s.entities);
	s.player.x = 8.5;
	s.player.y = 140.0;
	s.player.z = 10.0;

	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -3; cx <= 3; cx++)
		for(w_coord_t cz = -3; cz <= 3; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	for(int x = 0; x <= 12; x++)
		for(int z = 4; z <= 16; z++)
			sset(x, 100, z, BLOCK_STONE);

	test_mob();
	test_item();

	g_drain_run = 0;
	thread_join(&drain);
	if(s_failures) {
		fprintf(stderr, "test_cobweb_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_cobweb_golden (%d checks)\n", s_checks);
	return 0;
}
