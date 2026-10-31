/*
	F8 -- the local player must not tick until the column under it exists.

	Ground truth [740c583 EntityClientPlayerMP.onUpdate]: the FIRST thing the
	client player's onUpdate does is

		when the block at (floor(posX), 64,
		                         floor(posZ)) is not loaded yet,
			the update returns at once;
		otherwise the base onUpdate runs,
		then func_4056_N.

	i.e. nothing at all happens -- no physics, no damage, no motion send --
	while the column the player stands in is not loaded. That gate is what
	keeps a JOINING player from falling through terrain that has not arrived
	yet. CaveWii had no such gate, so on every MP join the body was placed at
	the server spawn (y=76/82), gravity ran while the join burst was still
	installing columns, and the server applied fall damage to the positions
	the client reported: real-Wii dense_walk_0913c/_0913d show y=76 health 160
	at tick 1 falling to 0 by tick 12, all behind the loading screen.

	This pins the port of that gate in entity_local_player.c's entity_tick
	(the local player's tick_client): a body standing over a column with no
	resident chunk stays EXACTLY put with zero velocity and coherent
	interpolation samples across many ticks, and ordinary Beta gravity resumes
	on the very next tick once the column installs.

	F8b: the predicate is local_player_column_present -- "at least one section
	of column (cx, cz) is resident". A Beta chunk IS the whole 16x128x16
	column, so blockExists's literal 64 is only "some y inside my chunk"; this
	port splits a column into 16x16x16 sections, so a literal
	world_find_chunk(w, x, 64, z) asks the narrower, wrong question (it froze a
	body standing at y=103 above an unloaded y=[64,79] section). Sections 3 and
	4 below pin both halves of that.

	Single-player is unaffected by construction: the loading screen already
	holds until the player's own column installs, so the gate is never armed
	there (section 2 below is exactly that state and runs full physics).
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
#include "entity/player_ysize.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "particle.h"
#include "platform/thread.h"
#include "world.h"

/* entity_local_player.c's own eye offset (EYE_HEIGHT there): e->pos[1] is the
 * EYE, so a body resting on top of the block at y=79 has pos[1] == 81.62. */
#define TEST_EYE_HEIGHT 1.62F

/* The spawn the real join burst hands a client, over chunk column (0,0). */
#define SPAWN_X 8.5F
#define SPAWN_Y 82.0F
#define SPAWN_Z 8.5F
/* Top surface of the ground we install with the column: block 79 occupies
 * [79,80], so the feet come to rest at 80.0. */
#define GROUND_TOP 80.0F

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

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

/* Install the spawn column the way the join burst eventually does: one write
 * inside the y=[64,79] section makes this port's blockExists true (that is the
 * section world_find_chunk(x, 64, z) resolves), plus the air the body's own
 * bounding box occupies in the section above it. A fresh chunk is all air, so
 * the y=80 write alone would install it -- the explicit air writes just make
 * the cells the collision scan reads unambiguous. */
static void install_spawn_column(void) {
	for(w_coord_t x = 6; x <= 11; x++)
		for(w_coord_t z = 6; z <= 11; z++) {
			world_set_block(&gstate.world, x, 79,
							z, (struct block_data) {.type = BLOCK_STONE},
							false);
			for(w_coord_t y = 80; y <= 83; y++)
				world_set_block(&gstate.world, x, y, z,
								(struct block_data) {.type = BLOCK_AIR},
								false);
		}
}

/* F8b: the predicate under test is "ANY section of column (cx, cz) exists",
 * because a Beta chunk is the whole 16x128x16 column. This is the test-side
 * spelling of it, used to pin each case's precondition exactly. */
static bool column_has_any_section(w_coord_t x, w_coord_t z) {
	for(w_coord_t y = 0; y < WORLD_HEIGHT; y += CHUNK_SIZE)
		if(world_find_chunk(&gstate.world, x, y, z))
			return true;

	return false;
}

/* Case (b): a column whose ONLY resident section is y=[96,111], with the body
 * standing inside it. Beta's chunkExists is TRUE here (the column is loaded),
 * so the gate must release -- while a literal world_find_chunk(x, 64, z) is
 * FALSE and froze the body forever (F8's known regression: test_minecart_client
 * at :309, test_boat_env and test_block_behavior_parity all stand a local
 * player at y~103 with no y=[64,79] section). */
#define HIGH_X 200.5F
#define HIGH_Z 200.5F
#define HIGH_EYE_Y 103.0F
#define HIGH_SECTION_Y 96

/* Case (c): a column with nothing resident at any height -- the gate must hold
 * even though ANOTHER column (the spawn one) is by now installed, so this
 * proves the any-section predicate is still a real per-column question. */
#define EMPTY_X 600.5F
#define EMPTY_Z 600.5F
#define EMPTY_EYE_Y 90.0F

static void place_body(struct entity* p, float x, float y, float z) {
	p->pos[0] = x;
	p->pos[1] = y;
	p->pos[2] = z;
	glm_vec3_copy(p->pos, p->pos_old);
	glm_vec2_copy(p->orient, p->orient_old);
	glm_vec3_zero(p->vel);
	p->on_ground = false;
}

int main(void) {
	test_fixture_dir("test_local_player_column_guard_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	string_clear(level);
	s.world.local = &s;
	s.worlds[0].session = &s; /* A1a (D1): owning session for services/callbacks */

	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();

	struct client_rpc client_reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = SERVER_LOCAL_PLAYER_ENTITY_ID,
		},
	};
	clin_process(&client_reset);
	struct entity* p = gstate.players[0].local_player;
	assert(p && "CRPC_WORLD_RESET must have built the local player body");
	assert(p->tick_client && "the local player's tick is entity_tick");

	/* The MP-join state: the body is already at the server spawn and the
	 * client is driving its own physics, but no column has installed yet. */
	p->data.local_player.capture_input = true;
	p->pos[0] = SPAWN_X;
	p->pos[1] = SPAWN_Y;
	p->pos[2] = SPAWN_Z;
	glm_vec3_copy(p->pos, p->pos_old);
	p->orient[0] = 0.375F;
	p->orient[1] = 1.25F;
	glm_vec2_copy(p->orient, p->orient_old);
	glm_vec3_zero(p->vel);
	p->on_ground = false;

	/* ====== 1: column missing -- the gate holds the body completely ====== */
	assert(!world_find_chunk(&gstate.world, 8, 64, 8)
		   && "the spawn column must not be resident for this case");
	/* F8b: and not at ANY height -- the predicate is the whole column. */
	assert(!column_has_any_section(8, 8)
		   && "no section of the spawn column may be resident for this case");
	for(unsigned t = 0; t < 40; t++) {
		bool remove = p->tick_client(p);
		assert(!remove && "the gate must not retire the local player");
		assert(p->pos[0] == SPAWN_X && p->pos[2] == SPAWN_Z
			   && "no horizontal drift while the column is missing");
		assert(p->pos[1] == SPAWN_Y
			   && "the body must not fall one bit through a missing column");
		assert(p->vel[0] == 0.0F && p->vel[1] == 0.0F && p->vel[2] == 0.0F
			   && "the gate zeroes velocity: no fall for the server to read");
		assert(p->pos_old[0] == p->pos[0] && p->pos_old[1] == p->pos[1]
			   && p->pos_old[2] == p->pos[2]
			   && "interpolation samples stay coherent (stationary render)");
		assert(p->orient_old[0] == p->orient[0]
			   && p->orient_old[1] == p->orient[1]
			   && "orientation samples stay coherent too");
		assert(!p->on_ground && "the gate reports nothing, not on_ground");
	}

	/* ====== 2: the column installs -- Beta physics resumes immediately ==== */
	install_spawn_column();
	assert(world_find_chunk(&gstate.world, 8, 64, 8)
		   && "the installed column must satisfy this port's blockExists");

	/* The very next tick re-authors Beta gravity (motionY = -0.08 * 0.98 drag
	 * = -0.0784). This port integrates the authored velocity into the position
	 * on the FOLLOWING tick -- pos_old/pos are snapshotted before the motion
	 * update -- so the observable order is "velocity at t1, displacement at
	 * t2". Both are proof the gate released; neither happened at all while it
	 * was armed. */
	assert(!p->tick_client(p));
	assert(p->vel[1] < 0.0F
		   && "gravity must resume on the first tick after the install");
	assert(fabsf(p->vel[1] - (-0.0784F)) < 1e-6F
		   && "and it must be Beta's -0.08 * 0.98, not some clamped value");
	fprintf(stderr, "  post-install t1: pos=%.6f vel=%.6f on_ground=%d\n",
			(double)p->pos[1], (double)p->vel[1], (int)p->on_ground);

	assert(!p->tick_client(p));
	assert(p->pos[1] < SPAWN_Y && "the body must actually start falling");
	fprintf(stderr, "  post-install t2: pos=%.6f vel=%.6f on_ground=%d\n",
			(double)p->pos[1], (double)p->vel[1], (int)p->on_ground);

	/* Enough ticks to fall the ~1.62 blocks to the floor and settle. */
	for(unsigned t = 0; t < 60; t++)
		assert(!p->tick_client(p));
	assert(p->on_ground && "the body must come to rest on the installed floor");
	/* The feet land on the top of the block at y=79 (GROUND_TOP) to within the
	 * mover's own collision-resolution epsilon (measured 0.002284 -- this
	 * engine's existing y-clamp behaviour, untouched by the gate). */
	float rest_feet = p->pos[1] - TEST_EYE_HEIGHT;
	fprintf(stderr, "  rest: eye=%.6f feet=%.6f (ground top %.1f)\n",
			(double)p->pos[1], (double)rest_feet, (double)GROUND_TOP);
	assert(rest_feet >= GROUND_TOP && rest_feet - GROUND_TOP < 0.01F
		   && "resting feet sit on the ground top, not below or floating");
	assert(p->pos[0] == SPAWN_X && p->pos[2] == SPAWN_Z
		   && "an idle fall introduces no horizontal motion");
	/* And it STAYS there: a resting body must not creep down through the
	 * floor tick after tick. */
	float settled = p->pos[1];
	for(unsigned t = 0; t < 20; t++) {
		assert(!p->tick_client(p));
		assert(p->pos[1] == settled && p->on_ground
			   && "the settled body must not creep");
	}

	puts("PASS local player column guard: 40 ticks over a missing column move "
		 "the body 0.0 blocks with vel == 0 and pos_old == pos "
		 "[740c583 EntityClientPlayerMP.onUpdate blockExists], then Beta "
		 "gravity (-0.0784) resumes the tick after the column installs and "
		 "the body falls once and rests on the floor");

	/* ====== 3 (b): ONLY the y=[96,111] section exists, body inside it ===== */
	place_body(p, HIGH_X, HIGH_EYE_Y, HIGH_Z);
	world_set_block(&gstate.world, (w_coord_t)HIGH_X, HIGH_SECTION_Y,
					(w_coord_t)HIGH_Z, (struct block_data) {.type = BLOCK_STONE},
					false);
	/* Exactly one section, and it is NOT the one Beta's literal 64 names. */
	for(w_coord_t y = 0; y < WORLD_HEIGHT; y += CHUNK_SIZE) {
		bool want = (y == (HIGH_SECTION_Y / CHUNK_SIZE) * CHUNK_SIZE);
		bool got = world_find_chunk(&gstate.world, (w_coord_t)HIGH_X, y,
									(w_coord_t)HIGH_Z) != NULL;
		assert(got == want
			   && "case (b) needs the y=[96,111] section and nothing else");
	}
	assert(!world_find_chunk(&gstate.world, (w_coord_t)HIGH_X, 64,
							 (w_coord_t)HIGH_Z)
		   && "case (b) is only meaningful while the literal-64 lookup FAILS");

	/* Beta's chunkExists is true (the column is loaded), so the gate releases
	 * and gravity is re-authored on the very first tick -- the same
	 * "velocity at t1, displacement at t2" order section 2 documents. */
	assert(!p->tick_client(p));
	assert(fabsf(p->vel[1] - (-0.0784F)) < 1e-6F
		   && "the gate must RELEASE over a column loaded only above y=79");
	fprintf(stderr, "  high-section t1: pos=%.6f vel=%.6f\n",
			(double)p->pos[1], (double)p->vel[1]);
	assert(!p->tick_client(p));
	assert(p->pos[1] < HIGH_EYE_Y && "and the body must actually move");
	fprintf(stderr, "  high-section t2: pos=%.6f vel=%.6f\n",
			(double)p->pos[1], (double)p->vel[1]);

	puts("PASS high-section column releases the gate: with ONLY the "
		 "y=[96,111] section of the column resident (the literal "
		 "world_find_chunk(x, 64, z) is NULL) a body at y=103 resumes Beta "
		 "gravity (-0.0784) on tick 1 and moves on tick 2 -- a Beta chunk is "
		 "the whole column, so chunkExists is TRUE here [740c583 "
		 "EntityClientPlayerMP.onUpdate -> World.blockExists -> chunkExists]");

	/* ====== 4 (c): no section anywhere in the column -- held 40 ticks ===== */
	place_body(p, EMPTY_X, EMPTY_EYE_Y, EMPTY_Z);
	assert(!column_has_any_section((w_coord_t)EMPTY_X, (w_coord_t)EMPTY_Z)
		   && "case (c) needs a column with no resident section at any height");
	/* Another column IS resident by now (the spawn one), so this also proves
	 * the predicate is per-column rather than "any chunk in the world". */
	assert(column_has_any_section(8, 8)
		   && "the spawn column from section 2 must still be resident");

	for(unsigned t = 0; t < 40; t++) {
		assert(!p->tick_client(p)
			   && "the gate must not retire the local player");
		assert(p->pos[0] == EMPTY_X && p->pos[1] == EMPTY_EYE_Y
			   && p->pos[2] == EMPTY_Z
			   && "a fully absent column must hold the body EXACTLY put");
		assert(p->vel[0] == 0.0F && p->vel[1] == 0.0F && p->vel[2] == 0.0F
			   && "the gate zeroes velocity: no fall for the server to read");
		assert(p->pos_old[0] == p->pos[0] && p->pos_old[1] == p->pos[1]
			   && p->pos_old[2] == p->pos[2]
			   && "interpolation samples stay coherent (stationary render)");
		assert(!p->on_ground && "the gate reports nothing, not on_ground");
	}
	fprintf(stderr, "  empty-column after 40 ticks: pos=%.6f vel=%.6f\n",
			(double)p->pos[1], (double)p->vel[1]);

	puts("PASS empty column still holds the gate: 40 ticks over a column with "
		 "NO section at any height move the body 0.0 blocks with vel == 0 and "
		 "pos_old == pos, while a different column is resident -- the "
		 "any-section predicate is per-column, not global");

	/* ====== 5 (F8d): the authoritative position is applied OUTSIDE the gate
	 *
	 * Beta applies a server position on PACKET RECEIPT
	 * [740c583 NetClientHandler.handleFlying -> EntityPlayerSP.setPositionAndRotation],
	 * not from inside onUpdate, and the blockExists gate only SUSPENDS the
	 * tick. So the gate must be unable to withhold the load-time /
	 * saved / anchor position: the local-server load-enter handoff
	 * (server_local.c CRPC_PLAYER_POS) lands in clin_apply_player_position,
	 * which runs on the client's RPC drain, never in tick_client.
	 *
	 * This pins both halves of that:
	 *   (a) a body sitting over a MISSING column (the gate is armed right
	 *       now, from section 4c) is nevertheless MOVED to the resolved
	 *       position the server sends, body AND camera, and then ticks
	 *       normally because that destination column is resident;
	 *   (b) the same delivery to a position over a MISSING column still
	 *       places the body -- and the gate then holds it there. */
	{
		/* Precondition: the gate is armed at the body's current column. */
		assert(!column_has_any_section((w_coord_t)EMPTY_X, (w_coord_t)EMPTY_Z)
			   && "case 5 starts with the gate armed (section 4c's column)");

		struct client_rpc resolved = {
			.type = CRPC_PLAYER_POS,
			.payload.player_pos = {
				.position = {SPAWN_X, SPAWN_Y, SPAWN_Z},
				.rotation = {0.0F, 0.0F},
				/* 0 = the full teleport/snapshot path the load-enter handoff
				 * and every world-switch position use. input_epoch stays 0
				 * here: this case is about the POSITION, not the input
				 * ownership marker (clin_process owns that ACK protocol). */
				.correction_flags = 0,
				.input_epoch = 0,
			},
		};
		assert(clin_apply_player_position(&gstate.players[0], &resolved)
			   && "the authoritative position must be accepted");

		assert(p->pos[0] == SPAWN_X && p->pos[1] == SPAWN_Y
			   && p->pos[2] == SPAWN_Z
			   && "the gate must NOT withhold the server's resolved position: "
				  "it is applied on receipt, like Beta's handleFlying");
		assert(p->pos_old[0] == p->pos[0] && p->pos_old[1] == p->pos[1]
			   && p->pos_old[2] == p->pos[2]
			   && "a teleport seeds both interpolation samples");
		assert(gstate.players[0].camera.x == SPAWN_X
			   && gstate.players[0].camera.y == SPAWN_Y
			   && gstate.players[0].camera.z == SPAWN_Z
			   && "the camera follows the same authoritative position");
		assert(p->orient_old[0] == p->orient[0]
			   && p->orient_old[1] == p->orient[1]
			   && "both orientation samples are installed too");

		/* And the body is live again: its new column IS resident, so the very
		 * next tick runs ordinary Beta gravity rather than the gate. */
		assert(column_has_any_section(8, 8)
			   && "the destination column must be resident for this half");
		assert(!p->tick_client(p));
		assert(fabsf(p->vel[1] - (-0.0784F)) < 1e-6F
			   && "a body placed by the load handoff ticks normally under the "
				  "guard once its own column is resident");

		fprintf(stderr,
				"  F8d applied position: pos=(%.6f, %.6f, %.6f) vel_y=%.6f\n",
				(double)p->pos[0], (double)p->pos[1], (double)p->pos[2],
				(double)p->vel[1]);

		/* (b) the same delivery into a MISSING column: still applied, then
		 * held. This is the join case -- the server owns where the body is,
		 * the gate owns whether it may move from there. */
		struct client_rpc into_missing = {
			.type = CRPC_PLAYER_POS,
			.payload.player_pos = {
				.position = {EMPTY_X, EMPTY_EYE_Y, EMPTY_Z},
				.rotation = {0.0F, 0.0F},
				.correction_flags = 0,
				.input_epoch = 0,
			},
		};
		assert(clin_apply_player_position(&gstate.players[0], &into_missing));
		assert(p->pos[0] == EMPTY_X && p->pos[1] == EMPTY_EYE_Y
			   && p->pos[2] == EMPTY_Z
			   && "a position into a missing column is applied all the same");
		for(unsigned t = 0; t < 20; t++) {
			assert(!p->tick_client(p));
			assert(p->pos[0] == EMPTY_X && p->pos[1] == EMPTY_EYE_Y
				   && p->pos[2] == EMPTY_Z
				   && "and the gate then holds it EXACTLY there");
			assert(p->vel[0] == 0.0F && p->vel[1] == 0.0F
				   && p->vel[2] == 0.0F);
		}

		/* (c) H-2 (hostile review 17), at the CALL SITE. The sneak camera dip
		 * is applied to the stored camera only and folded back out of
		 * clin_update's SRPC_PLAYER_POS, so the wire stays byte-identical
		 * standing and sneaking. That fold-out is
		 * `pre_dip_y + (camera_y - post_dip_y)`, which is right for a SHIFT
		 * and wrong after an ABSOLUTE set: this arm SETS camera.y, so the
		 * stored camera carries no dip while the record still describes the
		 * previous attach, and the fold-out added a stale 0.2F*0.4F back.
		 * Not one frame either -- the pre-world echo (client_interface.c:4063)
		 * sends every 50 ms for a whole cross-dimension terrain hold with no
		 * attach in between.
		 *
		 * tests/test_player_ysize.c pins the HELPER and the arithmetic; this
		 * pins that THIS production entry point really calls it. Beta does the
		 * same thing at the same place: NetClientHandler.handleFlying:253
		 * zeroes ySize itself, then setPositionAndRotation zeroes it again
		 * (Entity:614). */
		{
			struct camera* cam = &gstate.players[0].camera;
			/* Two held-sneak ticks, then the attach's own subtraction -- the
			 * state camera_attach_owned leaves behind (pinned there). */
			struct player_ysize_state held = {0};
			player_ysize_pre_move(&held, true);
			player_ysize_move(&held);
			player_ysize_pre_move(&held, true);
			player_ysize_move(&held);
			const float eye = (float)SPAWN_Y;
			cam->ysize = held;
			cam->ysize.applied = player_ysize_lerp(&cam->ysize, 1.0F);
			cam->ysize.pre_dip_y = eye;
			cam->ysize.post_dip_y = eye - cam->ysize.applied;
			cam->y = cam->ysize.post_dip_y;
			assert(cam->ysize.applied > 0.0F && cam->y < eye
				   && "the dip must really be baked in for this to prove "
					  "anything");

			const float T = (float)EMPTY_EYE_Y;
			struct client_rpc teleport = {
				.type = CRPC_PLAYER_POS,
				.payload.player_pos = {
					.position = {(float)EMPTY_X, T, (float)EMPTY_Z},
					.rotation = {0.0F, 0.0F},
					.correction_flags = 0,
					.input_epoch = 0,
				},
			};
			assert(clin_apply_player_position(&gstate.players[0], &teleport));
			assert(cam->y == T);
			/* THE CLAIM: what clin_update would put on the wire is exactly the
			 * position the server sent, with no dip folded back in. */
			assert(player_ysize_unapply_y(cam->y, &cam->ysize) == T
				   && "the snapshot arm left a stale sneak dip on the wire");
			assert(cam->ysize.applied == 0.0F
				   && "the absolute writer must clear the fold-out record");
			fprintf(stderr,
					"  H-2 call site: camera.y=%.6f wire=%.6f (sent %.6f)\n",
					(double)cam->y,
					(double)player_ysize_unapply_y(cam->y, &cam->ysize),
					(double)T);
		}
	}

	puts("PASS F8d: the server's resolved position is applied OUTSIDE the "
		 "column gate -- delivered while the gate was armed it moves body and "
		 "camera exactly, then ticks Beta gravity (-0.0784) because the "
		 "DESTINATION column is resident; a position into a missing column is "
		 "applied too and then held [740c583 NetClientHandler.handleFlying "
		 "applies position on receipt, outside onUpdate]");

	g_drain_run = 0;
	thread_join(&drain);
	clin_drain_discard();
	dict_entity_it_t client_it;
	dict_entity_it(client_it, gstate.entities);
	while(!dict_entity_end_p(client_it)) {
		free(dict_entity_ref(client_it)->value);
		dict_entity_next(client_it);
	}
	gstate.players[0].local_player = NULL;
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);
	server_world_destroy(&s.world);
	dict_entity_clear(s.entities);
	puts("test_local_player_column_guard: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
