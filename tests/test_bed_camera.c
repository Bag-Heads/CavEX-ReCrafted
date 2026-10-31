/*
	2026-08-08 hardware-video caveat: sleeping moved the EYE to the pillow
	(batch 2a body snap) but the VIEW kept following the pointer -- only half
	of Beta's in-bed camera.

	Ground truth [740c583 EntityRenderer.orientCamera:192-205]: the sleep
	branch pre-applies the exact INVERSE of the standard look tail (yaw+180
	about -Y, pitch about -X against the tail's +X/+Y), so the player's look
	input cancels COMPLETELY, leaving a single rotate of (bedMeta&3)*90 about
	Y read from the block at the sleeper's own cell: a pitch-0 view from the
	pillow toward the FOOT of the bed, pointer-immune until wake. The sleep
	GUI took mouse focus, so rotationYaw froze and the wake-up view is the
	pre-sleep view.

	This pins camera_attach's mirror of that: the per-direction yaw lock over
	our (meta>>1)&3 {+Z,-Z,+X,-X} convention, the pitch lock at ry = pi/2,
	pointer-immunity while asleep, the pre-sleep look restore on wake, and
	the documented micro-delta (bed destroyed mid-sleep keeps current yaw).
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
#include "game/camera.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/window_container.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "particle.h"
#include "platform/thread.h"
#include "platform/input_owner.h"
#include "platform/audio.h"
#include "platform/gfx.h"
#include "world.h"
#include "support/camera_update_reference.h"
#include "support/local_walk_reference.h"


static void init_walker(struct entity* body) {
	memset(body, 0, sizeof(*body));
	entity_local_player(91, body, &gstate.world);
	body->pos[0] = body->pos[2] = 4.5F;
	body->pos[1] = 104.62F;
	body->orient[1] = GLM_PI_2;
	body->data.local_player.capture_input = true;
	body->on_ground = true;
}

static void test_owned_walking(void) {
	const enum input_button buttons[] = {
		IB_FORWARD, IB_BACKWARD, IB_RIGHT, IB_LEFT, IB_JUMP, IB_SNEAK
	};
	const uint8_t materials[] = {
		BLOCK_AIR, BLOCK_WATER_STILL, BLOCK_LAVA_STILL, BLOCK_LADDER,
		BLOCK_STONE, BLOCK_SOULSAND
	};
	unsigned ticks = 0;
	for(size_t m = 0; m < sizeof(materials); m++) {
		for(int x = 2; x <= 6; x++) for(int z = 2; z <= 6; z++) {
			world_set_block(&gstate.world, x, 102, z,
				(struct block_data){.type=BLOCK_STONE}, false);
			world_set_block(&gstate.world, x, 103, z,
				(struct block_data){.type=materials[m]}, false);
			world_set_block(&gstate.world, x, 104, z,
				(struct block_data){.type=materials[m] == BLOCK_WATER_STILL
					|| materials[m] == BLOCK_LAVA_STILL ? materials[m] : BLOCK_AIR}, false);
		}
		for(unsigned mask = 0; mask < 64; mask++)
		for(unsigned angle = 0; angle < 4; angle++) {
			struct entity actual, expected;
			init_walker(&actual);
			actual.orient[0] = angle * .731F;
			actual.vel[0] = mask & 1 ? .004F : -.14F;
			actual.vel[1] = mask & 2 ? .19F : -.12F;
			actual.vel[2] = mask & 4 ? -.004F : .07F;
			actual.on_ground = (mask & 8) != 0;
			memcpy(&expected, &actual, sizeof(expected));
			struct client_session owner = {0}, reference;
			owner.local_player = &actual;
			owner.in_water = materials[m] == BLOCK_WATER_STILL;
			owner.oxygen = 130;
			memcpy(&reference, &owner, sizeof(reference));
			struct input_player_frame frame = {.connected=true};
			for(unsigned b = 0; b < 6; b++) frame.down[buttons[b]] = (mask & (1u<<b)) != 0;
			struct input_player_frame frame_before;
			memcpy(&frame_before, &frame, sizeof(frame));
			for(int step = 0; step < 4; step++) {
				assert(!local_walk_reference(&expected, &reference, &frame));
				assert(!entity_local_player_walk_owned(&actual, &owner, &frame));
				assert(!memcmp(&expected, &actual, sizeof(actual)));
				assert(!memcmp(&reference, &owner, sizeof(owner)));
				assert(!memcmp(&frame_before, &frame, sizeof(frame)));
				ticks++;
			}
		}
	}
	/* Owner isolation: the host is asleep, submerged, and has different air.
	 * Guest motion must still use its own buttons, and preserve all host state. */
	for(int x = 2; x <= 6; x++) for(int z = 2; z <= 6; z++)
		for(int y = 103; y <= 104; y++)
			world_set_block(&gstate.world,x,y,z,(struct block_data){.type=BLOCK_AIR},false);
	struct client_session saved, host_before;
	memcpy(&saved, &gstate.players[0], sizeof(saved));
	gstate.players[0].sleeping = true;
	gstate.players[0].in_water = true;
	gstate.players[0].oxygen = 27;
	memcpy(&host_before, &gstate.players[0], sizeof(host_before));
	struct entity guest_body, disconnected, idle;
	init_walker(&guest_body);
	struct client_session guest = {.local_player=&guest_body, .oxygen=17};
	struct input_player_frame forward = {.connected=true};
	forward.down[IB_FORWARD] = true;
	assert(!entity_local_player_walk_owned(&guest_body, &guest, &forward));
	assert(guest_body.pos[2] > 4.5F && guest.oxygen == MAX_OXYGEN);
	assert(!memcmp(&host_before, &gstate.players[0], sizeof(host_before)));
	init_walker(&disconnected);
	memcpy(&idle, &disconnected, sizeof(idle));
	struct client_session a = {0}, b = {0};
	forward.connected = false; /* Held bit is deliberately stale. */
	entity_local_player_walk_owned(&disconnected, &a, &forward);
	entity_local_player_walk_owned(&idle, &b, NULL);
	assert(!memcmp(&disconnected, &idle, sizeof(idle)));
	guest.sleeping = true;
	vec3 sleep_pos; glm_vec3_copy(guest_body.pos, sleep_pos);
	forward.connected = true;
	entity_local_player_walk_owned(&guest_body, &guest, &forward);
	assert(!memcmp(sleep_pos, guest_body.pos, sizeof(sleep_pos)));
	assert(guest_body.vel[0] == 0 && guest_body.vel[1] == 0 && guest_body.vel[2] == 0);
	memcpy(&gstate.players[0], &saved, sizeof(saved));
	printf("  owned walking: %u byte-exact steps; input, air, sleep and disconnect isolated\n",ticks);
}

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_bed_camera_tmp' (a literal) */
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

/* One camera_attach frame at a fixed small dt (joystick idle in the headless
 * harness; autopilot/agent inactive). */
static void attach_frame(void) {
	camera_attach(&gstate.players[0].camera, gstate.players[0].local_player, 1.0F, 0.016F);
}

static void set_player_eye(float x, float y, float z) {
	struct client_rpc pos = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {x, y, z},
			.input_epoch = 1,
			.oxygen = MAX_OXYGEN,
			.selected_hotbar = 0,
		},
	};
	clin_process(&pos);
}

static void set_sleeping(bool sleeping) {
	struct client_rpc sl = {
		.type = CRPC_SLEEP_STATE,
		.payload.sleep_state = {.timer = sleeping ? 0 : 110,
								.sleeping = sleeping},
	};
	clin_process(&sl);
}

static void test_viewport_camera_equiv(void) {
	float saved_fov = gstate.config.fov, saved_distance = gstate.config.render_distance;
	bool saved_bobbing = gstate.config.view_bobbing;
	gstate.config.render_distance = 128;
	for(int k = 0; k < 160; k++) {
		gstate.config.fov = 55 + k % 45;
		gstate.config.view_bobbing = (k & 1) != 0;
		struct camera original = {.x = k * .25F, .y = 64 + k * .125F, .z = -k,
			.rx = k * .13F, .ry = .3F + (k % 20) * .1F,
			.bob_active = (k & 2) != 0, .bob_partial = (k % 8) * .125F,
			.bob = {.dwm = 2.1F, .dwm_prev = 1.9F, .cam_yaw = .04F,
				.cam_yaw_prev = .02F, .cam_pitch = .3F, .cam_pitch_prev = .2F},
		};
		hud_hurt_tick(&original.hurt, 160, true);
		hud_hurt_tick(&original.hurt, k % 3 ? 100 : 0, true);
		struct camera current = original;
		camera_update_reference(&original, (k & 4) != 0);
		assert(camera_update_viewport(&current, (k & 4) != 0, gfx_width(), gfx_height()));
		assert(!memcmp(original.view, current.view, sizeof(original.view)));
		assert(!memcmp(original.projection, current.projection, sizeof(original.projection)));
		assert(!memcmp(original.frustum_planes, current.frustum_planes, sizeof(original.frustum_planes)));
		assert(original.bob_active == current.bob_active);
	}
	struct camera full = {.rx = .4F, .ry = 1.2F}, half = full;
	assert(camera_update_viewport(&full, false, 640, 480));
	assert(camera_update_viewport(&half, false, 640, 240));
	assert(!memcmp(full.view, half.view, sizeof(full.view)));
	assert(full.projection[1][1] == half.projection[1][1]);
	assert(fabsf(half.projection[0][0] * 2 - full.projection[0][0]) < 1e-6F);
	struct camera before = half;
	assert(!camera_update_viewport(&half, false, 640, 0));
	assert(!memcmp(&before, &half, sizeof(half)));
	gstate.config.fov = saved_fov; gstate.config.render_distance = saved_distance;
	gstate.config.view_bobbing = saved_bobbing;
	puts("PASS camera equivalence: original full-view matrices/frustum byte-identical; compact aspect ratio correct");
}

static void test_owned_cameras(void) {
	struct client_session players[2] = {0};
	struct entity bodies[2] = {0};
	for(unsigned p = 0; p < 2; p++) {
		players[p].local_player = &bodies[p];
		bodies[p].pos[0] = bodies[p].pos_old[0] = 12 + p;
		bodies[p].pos[1] = bodies[p].pos_old[1] = 104;
		bodies[p].pos[2] = bodies[p].pos_old[2] = 12;
	}
	struct camera_player_view a = {.world = &gstate.world, .ride = &players[0].ride,
		.sleeping = true, .yaw = .4F, .pitch = .9F};
	struct camera_player_view b = {.world = &gstate.world, .ride = &players[1].ride,
		.sleeping = true, .yaw = 1.3F, .pitch = 1.7F};
	camera_attach_owned(&players[0].camera, &bodies[0], 1, 0, &a);
	camera_attach_owned(&players[1].camera, &bodies[1], 1, 0, &b);
	assert(players[0].camera.ry == (float)GLM_PI_2 && players[1].camera.ry == (float)GLM_PI_2);
	a.sleeping = false; a.yaw = 2; a.pitch = 2;
	camera_attach_owned(&players[0].camera, &bodies[0], 1, 0, &a);
	assert(players[0].camera.rx == .4F && players[0].camera.ry == .9F);
	assert(players[1].camera.was_sleeping);
	b.sleeping = false; b.yaw = 2; b.pitch = 2;
	camera_attach_owned(&players[1].camera, &bodies[1], 1, 0, &b);
	assert(players[1].camera.rx == 1.3F && players[1].camera.ry == 1.7F);
	players[0].ride.camera_correction_active = true;
	players[0].ride.camera_correction_start[0] = 2;
	struct camera b_before = players[1].camera;
	camera_attach_owned(&players[0].camera, &bodies[0], 1, .1F, &a);
	assert(players[0].camera.x == 14 && players[0].ride.camera_correction_elapsed == .1F);
	assert(!players[1].ride.camera_correction_active);
	assert(!memcmp(&b_before, &players[1].camera, sizeof(b_before)));
	camera_attach_owned(&players[1].camera, &bodies[1], 1, .1F, &b);
	assert(players[1].camera.x == 13);
	puts("PASS owned cameras: independent wake angles, look and ride correction in one shared world");
}

int main(void) {
	test_viewport_camera_equiv();
	test_fixture_dir("test_bed_camera_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	assert(gstate.players[0].local_player);

	/* Floor + a lit column so the camera cell reads are deterministic. */
	world_set_block(&gstate.world, 4, 102, 4,
					(struct block_data) {.type = BLOCK_STONE}, false);

	/* ====== S1: yaw/pitch lock per bed direction ====== */
	test_owned_cameras();
	test_owned_walking();
	{
		/* Beta's layout (PAR-BED-META, source/block/bed_meta.h): direction
		 * d = meta&3 over headBlockToFootBlockMap {+Z,-X,-Z,+X} (foot->head),
		 * 0x4 occupied, 0x8 the pillow half. EntityRenderer.orientCamera rotates
		 * by (meta&3)*90 [RetroMCP EntityRenderer.java:197-200], so the occupied
		 * bit must not move the view. The sleeper lies at the pillow looking
		 * toward the FOOT = -map[d]: on the camera's (sin rx, 0, cos rx) heading
		 * that is rx = {pi, pi/2, 0, -pi/2}. The eye cell is the HEAD cell. */
		static const float want_rx[4]
			= {GLM_PI, GLM_PI_2, 0.0F, -GLM_PI_2};
		for(int m = 0; m < 8; m++) {
			int d = m & 3;
			set_sleeping(false);
			attach_frame(); /* settle the wake edge from any prior loop */
			world_set_block(
				&gstate.world, 4, 103, 4,
				(struct block_data) {.type = BLOCK_BED,
									 .metadata = (uint8_t)(0x8 | m)},
				false);
			set_player_eye(4.5F, 103.9375F, 4.5F);
			gstate.players[0].camera.rx = 1.234F; /* junk look to be overridden */
			gstate.players[0].camera.ry = 2.345F;
			set_sleeping(true);
			attach_frame();
			assert(fabsf(gstate.players[0].camera.rx - want_rx[d]) < 1e-5f
				   && "sleep must yaw the view pillow->foot per Beta bed dir (meta&3)");
			assert(fabsf(gstate.players[0].camera.ry - GLM_PI_2) < 1e-5f
				   && "sleep must level the view (pitch 0)");
			/* Pointer-immunity: junk again, attach again -- still locked. */
			gstate.players[0].camera.rx = -2.5F;
			gstate.players[0].camera.ry = 0.7F;
			attach_frame();
			assert(fabsf(gstate.players[0].camera.rx - want_rx[d]) < 1e-5f
				   && "look input must be ignored while sleeping");
			assert(fabsf(gstate.players[0].camera.ry - GLM_PI_2) < 1e-5f);
		}
		fprintf(stderr, "  S1: four-direction pillow->foot lock + immunity\n");
	}

	/* ====== S2: pre-sleep look survives the night ====== */
	{
		set_sleeping(false);
		attach_frame();
		world_set_block(&gstate.world, 4, 103, 4,
						(struct block_data) {.type = BLOCK_BED,
											 .metadata = 0x8},
						false);
		set_player_eye(4.5F, 103.9375F, 4.5F);
		gstate.players[0].camera.rx = 0.777F; /* the look the player went to bed with */
		gstate.players[0].camera.ry = 1.111F;
		set_sleeping(true);
		attach_frame();
		assert(fabsf(gstate.players[0].camera.rx - GLM_PI) < 1e-5f); /* locked */
		set_sleeping(false);
		attach_frame();
		assert(fabsf(gstate.players[0].camera.rx - 0.777F) < 1e-5f
			   && "wake must restore the pre-sleep yaw");
		assert(fabsf(gstate.players[0].camera.ry - 1.111F) < 1e-5f
			   && "wake must restore the pre-sleep pitch");
		fprintf(stderr, "  S2: pre-sleep look restored on wake\n");
	}

	/* ====== S3: bed destroyed mid-sleep -- documented micro-delta ====== */
	{
		set_sleeping(false);
		attach_frame();
		world_set_block(&gstate.world, 4, 103, 4,
						(struct block_data) {.type = BLOCK_BED,
											 .metadata = 0x8},
						false);
		set_player_eye(4.5F, 103.9375F, 4.5F);
		set_sleeping(true);
		attach_frame();
		assert(fabsf(gstate.players[0].camera.rx - GLM_PI) < 1e-5f);
		world_set_block(&gstate.world, 4, 103, 4,
						(struct block_data) {.type = BLOCK_AIR}, false);
		attach_frame();
		assert(fabsf(gstate.players[0].camera.rx - GLM_PI) < 1e-5f
			   && "no bed under the sleeper: keep current yaw (recorded)");
		assert(fabsf(gstate.players[0].camera.ry - GLM_PI_2) < 1e-5f
			   && "pitch stays levelled while the flag is still set");
		set_sleeping(false);
		attach_frame();
		fprintf(stderr, "  S3: bed-gone micro-delta pinned\n");
	}

	g_drain_run = 0;
	{
		/* Review 19 M-2: clin_guest_body_install is the third ABSOLUTE camera
		 * writer, and gstate.players[1] is REUSED across joins -- a guest who
		 * left while sneaking leaves a fold-out record behind, and the next
		 * guest would send that dip at the server from its first echo. Driven
		 * through the real dispatch (CRPC_LOCAL_GUEST -> clin_local_guest_status
		 * -> install); camera.y == the admitted Y proves the install ran. */
		struct client_session* guest = &gstate.players[1];
		assert(!guest->local_player);
		guest->local_bound = true;
		guest->local_ticket = 77;
		guest->local_generation = 5;
		guest->local_phase = CLIENT_GUEST_ATTACHED;
		guest->camera.y = 50.0F - 0.2F * 0.4F;
		guest->camera.ysize.y_size = guest->camera.ysize.y_size_prev
			= 0.2F * 0.4F;
		guest->camera.ysize.applied = 0.2F * 0.4F;
		guest->camera.ysize.pre_dip_y = 50.0F;
		guest->camera.ysize.post_dip_y = 50.0F - 0.2F * 0.4F;
		struct client_rpc ready = {
			.type = CRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = 77, .generation = 5,
									.result = LOCAL_GUEST_GAMEPLAY_READY,
									.entity_id = 4242,
									.health = MAX_PLAYER_HEALTH,
									.x = 4.5, .y = 104.0, .z = 4.5},
		};
		clin_process(&ready);
		assert(guest->camera.y == 104.0F
			   && "the guest body install did not run");
		assert(guest->camera.ysize.applied == 0.0F
			   && guest->camera.ysize.pre_dip_y == 0.0F
			   && guest->camera.ysize.post_dip_y == 0.0F
			   && guest->camera.ysize.y_size == 0.0F
			   && guest->camera.ysize.y_size_prev == 0.0F
			   && "a joining guest inherited the last guest's sneak dip");
		assert(player_ysize_unapply_y(guest->camera.y, &guest->camera.ysize)
				   == 104.0F
			   && "the joining guest's first echo carries a stale dip");
		guest->local_player = NULL;
		guest->local_bound = false;
		guest->local_ticket = 0;
		guest->local_generation = 0;
		fputs("test_bed_camera: guest install resets the sneak dip OK\n", stderr);
	}
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
	fprintf(stderr, "test_bed_camera: all sections OK\n");
	puts("test_bed_camera: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
