/* Experimental local split-screen: the client half of gameplay admission.
 * local_gameplay_ready is the ONE gate main.c's two-view loop reads, so this
 * pins exactly what may set it -- and proves nothing else can.
 * GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "entity/entity.h"
#include "game/client_session.h"
#include "game/respawn_clearance.h"
#include "game/gui/screen_death.h"
#include "platform/input_owner.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local_state.h"
#include "block/blocks.h"
#include "world.h"

static void feed(struct client_session* s, uint64_t ticket, uint64_t generation,
				 int result) {
	struct client_rpc call = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest
		= {.ticket = ticket, .generation = generation, .result = result}};
	client_session_local_status(s, &call);
}

static struct client_rpc respawn_reply(uint64_t ticket, uint64_t generation,
	int result, uint8_t owner, uint64_t epoch, double x, double y, double z) {
	struct client_rpc call = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket=ticket, .generation=generation,
			.result=result, .owner=owner, .respawn_epoch=epoch,
			.x=x, .y=y, .z=z,
			.correction_flags=PLAYER_CORRECTION_POSITION_ONLY
				| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
				| PLAYER_CORRECTION_ZERO_VEL_Z | PLAYER_CORRECTION_SET_ON_GROUND}};
	return call;
}

static void feed_respawn_owner(struct client_session* s, uint64_t ticket,
	uint64_t generation, int result, uint8_t owner, uint64_t epoch,
	double x, double y, double z) {
	struct client_rpc call = respawn_reply(ticket, generation, result, owner,
		epoch, x, y, z);
	client_session_local_status(s, &call);
}

static void feed_respawn(struct client_session* s, uint64_t ticket,
	uint64_t generation, int result, uint64_t epoch, double x, double y, double z) {
	feed_respawn_owner(s, ticket, generation, result, 1, epoch, x, y, z);
}

/* Walk the existing window handshake to ATTACHED. That much is settled work
 * (test_local_guest drives it against the real server); this test only needs
 * a session standing exactly where a Join lands. */
static uint64_t attach(struct client_session* s, uint64_t generation) {
	assert(client_session_request_local_join(s));
	uint64_t ticket = s->local_ticket;
	assert(s->local_phase == CLIENT_GUEST_REQUESTED);
	feed(s, ticket, generation, LOCAL_GUEST_OFFER);
	assert(s->local_phase == CLIENT_GUEST_INSTALLING && s->local_bound);
	feed(s, ticket, generation, LOCAL_GUEST_INSTALLED);
	assert(s->local_phase == CLIENT_GUEST_CONFIRMING);
	feed(s, ticket, generation, LOCAL_GUEST_ATTACHED);
	assert(s->local_phase == CLIENT_GUEST_ATTACHED);
	/* ATTACHED is window ownership only: no body, no gameplay. */
	assert(!s->local_gameplay_ready && !s->local_player);
	s->local_pending = false;
	return ticket;
}

static void test_request_shape(void) {
	struct client_session s = {0};
	/* Every phase before ATTACHED refuses the body request outright. */
	assert(!client_session_request_local_gameplay(NULL));
	assert(!client_session_request_local_gameplay(&s));
	uint64_t ticket = attach(&s, 41);
	assert(client_session_request_local_gameplay(&s));
	assert(s.local_pending && s.local_pending_action == LOCAL_GUEST_GAMEPLAY);
	/* One request at a time: a second press cannot queue a duplicate. */
	assert(!client_session_request_local_gameplay(&s));
	s.local_pending = false;
	assert(ticket == s.local_ticket);
	client_session_clear_windows(&s);
	puts("PASS join request shape: ATTACHED-only, one in flight, ticket kept");
}

static void test_ready_sets_the_gate(void) {
	struct client_session s = {0};
	struct entity body = {.type = ENTITY_LOCAL_PLAYER, .id = 7};
	uint64_t ticket = attach(&s, 55);
	assert(client_session_request_local_gameplay(&s));
	s.local_pending = false;

	/* A READY reply WITHOUT an installed body never opens the gate: the
	 * caller installs the entity first, and a failed install must leave the
	 * session retryable rather than half-admitted. */
	feed(&s, ticket, 55, LOCAL_GUEST_GAMEPLAY_READY);
	assert(!s.local_gameplay_ready);

	/* Wrong generation and wrong ticket are both inert even with a body. */
	s.local_player = &body;
	feed(&s, ticket, 54, LOCAL_GUEST_GAMEPLAY_READY);
	assert(!s.local_gameplay_ready);
	feed(&s, ticket + 1, 55, LOCAL_GUEST_GAMEPLAY_READY);
	assert(!s.local_gameplay_ready);

	/* A session whose replies have failed is not admitted either. */
	s.local_reply_failed = true;
	feed(&s, ticket, 55, LOCAL_GUEST_GAMEPLAY_READY);
	assert(!s.local_gameplay_ready);
	s.local_reply_failed = false;

	/* Matching ticket + generation + ATTACHED + body: granted. */
	feed(&s, ticket, 55, LOCAL_GUEST_GAMEPLAY_READY);
	assert(s.local_gameplay_ready && s.local_player == &body);
	/* Already granted: no further request is issued. */
	assert(!client_session_request_local_gameplay(&s));

	/* Losing window ownership retires the gate with it. The BODY belongs to
	 * the caller (client_interface), so this must not touch it. */
	client_session_clear_windows(&s);
	assert(!s.local_gameplay_ready && s.local_phase == CLIENT_GUEST_IDLE);
	assert(s.local_player == &body && body.id == 7);
	puts("PASS gameplay ready: body+generation+ticket+phase all required; "
		 "cleared with window ownership; body untouched");
}

static void test_rebind_clears_admission(void) {
	struct client_session s = {0};
	struct entity body = {.type = ENTITY_LOCAL_PLAYER, .id = 9};
	uint64_t ticket = attach(&s, 61);
	assert(client_session_request_local_gameplay(&s));
	s.local_pending = false;
	s.local_player = &body;
	feed(&s, ticket, 61, LOCAL_GUEST_GAMEPLAY_READY);
	assert(s.local_gameplay_ready);
	/* A new provider generation is a new client. bind_local refuses while a
	 * body is still installed, so the caller must retire it first -- exactly
	 * the ordering client_interface enforces -- and a retired generation can
	 * never be handed out again. */
	assert(!client_session_bind_local(&s, 62));
	s.local_player = NULL;
	assert(!client_session_bind_local(&s, 61));
	assert(client_session_bind_local(&s, 62));
	assert(!s.local_gameplay_ready && !s.local_player);
	assert(s.local_generation == 62 && s.local_bound);
	client_session_clear_windows(&s);
	puts("PASS rebind: a replacement generation starts with no admission");
}

/* The EXACT expression main.c uses to choose one viewport or two:
 *   view_count = players[1].local_bound && players[1].local_gameplay_ready
 *                && players[1].local_player ? 2 : 1;
 * Flag OFF means the server never sends GAMEPLAY_READY, so this stays 1 for
 * every state the handshake can reach -- which is what makes single-player
 * rendering byte-identical (world_render, not world_render_view). */
static unsigned view_count_of(const struct client_session* s) {
	return s->local_bound && s->local_gameplay_ready && s->local_player ? 2 : 1;
}

static void test_two_view_gate(void) {
	struct client_session s = {0};
	struct entity body = {.type = ENTITY_LOCAL_PLAYER, .id = 11};
	assert(view_count_of(&s) == 1);
	uint64_t ticket = attach(&s, 91);
	assert(view_count_of(&s) == 1); /* ATTACHED alone never splits the screen */
	s.local_player = &body;
	assert(view_count_of(&s) == 1); /* nor does a body without admission */
	assert(client_session_request_local_gameplay(&s));
	s.local_pending = false;
	assert(view_count_of(&s) == 1); /* nor a request still in flight */
	/* Every OTHER reply the provider can send leaves it at one view. */
	int inert[] = {LOCAL_GUEST_OFFER, LOCAL_GUEST_INSTALLED, LOCAL_GUEST_ATTACHED,
				   LOCAL_GUEST_REJECTED, LOCAL_GUEST_LEAVE_FAILED,
				   LOCAL_GUEST_GAMEPLAY_DENIED};
	for(size_t k = 0; k < sizeof(inert) / sizeof(*inert); k++) {
		feed(&s, ticket, 91, inert[k]);
		assert(view_count_of(&s) == 1);
	}
	/* Exactly one reply opens the second viewport. */
	s.local_phase = CLIENT_GUEST_ATTACHED;
	s.local_pending = false;
	feed(&s, ticket, 91, LOCAL_GUEST_GAMEPLAY_READY);
	assert(view_count_of(&s) == 2);
	client_session_clear_windows(&s);
	assert(view_count_of(&s) == 1);
	puts("PASS two-view gate: only GAMEPLAY_READY can ever make view_count 2");
}

static void test_respawn_order_and_retention(void) {
	struct client_session s = {0};
	struct entity body = {.type=ENTITY_LOCAL_PLAYER, .id=19,
		.pos={1,70,2}, .pos_old={1,70,2}, .network_pos={1,70,2},
		.vel={3,4,5}, .on_ground=true};
	uint64_t ticket = attach(&s, 101);
	s.local_player = &body;
	feed(&s, ticket, 101, LOCAL_GUEST_GAMEPLAY_READY);
	assert(s.local_gameplay_ready);
	s.camera.x=1;s.camera.y=70;s.camera.z=2;
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_RESPAWN_LOADING, 4, 20.5, 72, -3.5);
	assert(s.local_respawn_loading && !s.local_gameplay_ready
		&&s.local_player==&body&&s.local_respawn_epoch==4
		&&s.local_respawn_render_camera_frozen
		&&client_session_local_render_camera(&s)==&s.local_respawn_render_camera
		&&s.local_respawn_render_camera.x==1
		&&s.local_respawn_render_camera.y==70
		&&s.local_respawn_render_camera.z==2);
	assert(!client_session_request_local_gameplay(&s));
	struct camera hidden;
	assert(client_session_local_respawn_hidden_camera(&s, &hidden));
	assert(hidden.x == 20.5F && hidden.y == 72.0F && hidden.z == -3.5F);
	assert(s.camera.x == 1 && s.camera.y == 70 && s.camera.z == 2
		&& "hidden LOD targeting must not replace the retained render camera");
	/* READY before the matching correction and every wrong tuple stay inert. */
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_GAMEPLAY_READY, 4, 0,0,0);
	feed_respawn(&s, ticket, 100, LOCAL_GUEST_CORRECTION, 4, 20.5,72,-3.5);
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_CORRECTION, 3, 20.5,72,-3.5);
	assert(!s.local_gameplay_ready && !s.local_respawn_correction_installed);
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_CORRECTION, 4, 20.5,72,-3.5);
	assert(s.local_respawn_correction_installed && body.pos[0] == 20.5F
		&& body.pos[1] == 72.0F && body.pos[2] == -3.5F
		&& body.vel[0] == 0 && body.vel[1] == 0 && body.vel[2] == 0
		&& !body.on_ground&&s.camera.x==1&&s.camera.y==70&&s.camera.z==2
		&&client_session_local_render_camera(&s)->x==1);
	/* Duplicate LOADING cannot erase the installed correction. */
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_RESPAWN_LOADING, 4, 99,99,99);
	assert(s.local_respawn_correction_installed && s.local_respawn_x == 20.5);
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_GAMEPLAY_READY, 4, 0,0,0);
	assert(s.local_gameplay_ready && !s.local_respawn_loading
		&&s.local_respawn_completed_epoch==4&&s.local_player==&body
		&&!s.local_respawn_render_camera_frozen
		&&client_session_local_render_camera(&s)==&s.camera);
	/* Completed and older epochs cannot reopen the transaction. */
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_RESPAWN_LOADING, 4, 8,8,8);
	feed_respawn(&s, ticket, 101, LOCAL_GUEST_RESPAWN_LOADING, 3, 8,8,8);
	assert(s.local_gameplay_ready && !s.local_respawn_loading);
	client_session_clear_windows(&s);
	assert(!s.local_respawn_completed_epoch);
	puts("PASS respawn client order: body retained, hidden target, correction before READY, epoch floor");
}

static void assert_wrong_owner_inert(const struct client_session* s,
	const struct entity* body) {
	struct camera hidden;
	assert(s->local_player == body && s->local_respawn_loading
		&& !s->local_gameplay_ready && s->local_respawn_epoch == 6
		&& s->local_respawn_completed_epoch == 4
		&& !s->local_respawn_correction_installed
		&& s->local_respawn_ack_send_pending
		&& !s->local_respawn_awaiting_commit
		&& s->local_respawn_x == 30.5 && s->local_respawn_y == 74.0
		&& s->local_respawn_z == -12.5
		&& body->pos[0] == 3 && body->pos[1] == 70 && body->pos[2] == 4
		&& body->vel[0] == 5 && body->vel[1] == 6 && body->vel[2] == 7);
	assert(client_session_local_respawn_hidden_camera(s, &hidden));
	assert(hidden.x == 30.5F && hidden.y == 74.0F && hidden.z == -12.5F);
}

static void test_respawn_wrong_owner(void) {
	struct client_session s = {0};
	struct entity body = {.type=ENTITY_LOCAL_PLAYER, .id=23,
		.pos={3,70,4}, .pos_old={3,70,4}, .network_pos={3,70,4},
		.vel={5,6,7}, .on_ground=true};
	uint64_t ticket = attach(&s, 111);
	s.local_player = &body;
	feed(&s, ticket, 111, LOCAL_GUEST_GAMEPLAY_READY);
	feed_respawn(&s, ticket, 111, LOCAL_GUEST_RESPAWN_LOADING,
		6, 30.5, 74, -12.5);
	s.local_respawn_completed_epoch = 4;
	s.local_respawn_ack_send_pending = true;

	/* Each non-guest owner would mutate a different part of the transaction if
	 * the epoch-owner guard were absent: newer LOADING retargets the camera,
	 * CORRECTION moves the retained body, and READY can reopen gameplay once a
	 * correction is installed. Exercise both invalid envelope owners. */
	for(uint8_t owner = 0; owner <= 2; owner += 2) {
		feed_respawn_owner(&s, ticket, 111, LOCAL_GUEST_RESPAWN_LOADING,
			owner, 7, 90, 91, 92);
		assert_wrong_owner_inert(&s, &body);
		feed_respawn_owner(&s, ticket, 111, LOCAL_GUEST_CORRECTION,
			owner, 6, 90, 91, 92);
		assert_wrong_owner_inert(&s, &body);
		s.local_respawn_correction_installed = true;
		feed_respawn_owner(&s, ticket, 111, LOCAL_GUEST_GAMEPLAY_READY,
			owner, 6, 0, 0, 0);
		s.local_respawn_correction_installed = false;
		assert_wrong_owner_inert(&s, &body);
	}

	/* Owner is an epoch discriminator only. Legacy epoch-zero correction keeps
	 * its established compatibility even if the otherwise-unused owner byte is
	 * non-guest. */
	s.local_respawn_loading = false;
	s.local_respawn_ack_send_pending = false;
	s.local_gameplay_ready = true;
	feed_respawn_owner(&s, ticket, 111, LOCAL_GUEST_CORRECTION,
		2, 0, 8, 75, 9);
	assert(body.pos[0] == 8 && body.pos[1] == 75 && body.pos[2] == 9);
	client_session_clear_windows(&s);
	puts("PASS respawn owner: owner 0/2 inert for epoch LOADING/CORRECTION/READY; epoch zero compatible");
}

static void test_real_world_readiness(void) {
	int saved_radius = world_server_view_radius();
	struct client_session s = {.local_bound=true, .local_ticket=9,
		.local_generation=8, .local_phase=CLIENT_GUEST_ATTACHED,
		.local_respawn_loading=true, .local_respawn_epoch=2,
		.local_respawn_x=.5,
		.local_respawn_y=(double)((float)64+PLAYER_EYE_HEIGHT),
		.local_respawn_z=.5};
	struct world w;
	world_create(&w);
	world_set_server_view_radius(1);
	assert(!client_session_local_respawn_readiness(&s, &w, true, false, false));
	for(int x=-1; x<=1; x++) for(int z=-1; z<=1; z++)
		memset(dict_wsection_safe_get(w.sections, SECTION_TO_ID(x,z)), 0,
			sizeof(struct world_section));
	struct world_section* center = dict_wsection_get(w.sections, SECTION_TO_ID(0,0));
	struct chunk* cbelow=chunk_alloc();assert(cbelow);
	chunk_init(cbelow,&w,0,48,0);center->column[3]=cbelow;
	struct chunk* c = chunk_alloc(); assert(c);
	chunk_init(c, &w, 0, 64, 0); center->column[4] = c;
	for(int x=0;x<CHUNK_SIZE;x++)for(int y=0;y<CHUNK_SIZE;y++)
		for(int z=0;z<CHUNK_SIZE;z++)
			chunk_set_block_raw(cbelow,x,y,z,(struct block_data){.type=BLOCK_AIR});
	cbelow->rebuild_displist=cbelow->mesh_in_flight=false;
	for(int x=0;x<CHUNK_SIZE;x++) for(int y=0;y<CHUNK_SIZE;y++)
		for(int z=0;z<CHUNK_SIZE;z++)
			chunk_set_block_raw(c,x,y,z,(struct block_data){.type=BLOCK_AIR});
	c->rebuild_displist = true;
	assert(!client_session_local_respawn_readiness(&s, &w, true, false, false));
	c->rebuild_displist = false;
	chunk_set_block_raw(cbelow,0,15,0,(struct block_data){.type=BLOCK_STONE});
	assert(client_session_local_respawn_readiness(&s,&w,true,false,false));
	assert(s.local_respawn_x==.5
		&&s.local_respawn_y==(double)((float)64+PLAYER_EYE_HEIGHT)
		&&s.local_respawn_z==.5);
	s.local_respawn_ack_send_pending=false;
	struct chunk* c16=chunk_alloc();struct chunk* c32=chunk_alloc();
	assert(c16&&c32);chunk_init(c16,&w,0,16,0);chunk_init(c32,&w,0,32,0);
	center->column[1]=c16;center->column[2]=c32;
	for(int x=0;x<CHUNK_SIZE;x++)for(int y=0;y<CHUNK_SIZE;y++)
		for(int z=0;z<CHUNK_SIZE;z++) {
			chunk_set_block_raw(c16,x,y,z,(struct block_data){.type=BLOCK_AIR});
			chunk_set_block_raw(c32,x,y,z,(struct block_data){.type=BLOCK_AIR});
		}
	c16->rebuild_displist=c16->mesh_in_flight=false;
	c32->rebuild_displist=c32->mesh_in_flight=false;
	chunk_set_block_raw(c16,0,15,0,(struct block_data){.type=BLOCK_STONE});
	s.local_respawn_y=(double)((float)32+PLAYER_EYE_HEIGHT);
	assert(client_session_local_respawn_readiness(&s,&w,true,false,false));
	s.local_respawn_ack_send_pending=false;
	/* Positive-volume feet/head overlap blocks while floor face contact clears. */
	s.local_respawn_y=(double)((float)64+PLAYER_EYE_HEIGHT);
	chunk_set_block_raw(c,0,0,0,(struct block_data){.type=BLOCK_STONE});
	assert(!client_session_local_respawn_readiness(&s,&w,true,false,false));
	chunk_set_block_raw(c,0,0,0,(struct block_data){.type=BLOCK_AIR});
	/* Authored bed +0.1 is not snapped back to the integral floor. */
	s.local_respawn_y=(double)((float)64+PLAYER_EYE_HEIGHT)+0.1;
	assert(client_session_local_respawn_readiness(&s,&w,true,false,false));
	s.local_respawn_ack_send_pending=false;
	/* A real id-36 lookup contributes its shifted stored-stone box. At x=1,
	 * eastbound shift .5 overlaps the body's east side; shift .2 ends exactly
	 * at x=.8 and is clear under Beta's strict face-contact rule. */
	s.local_respawn_y=(double)((float)64+PLAYER_EYE_HEIGHT);
	chunk_set_block_raw(c,1,0,0,(struct block_data){.type=BLOCK_PISTON_MOVING});
	struct piston_moving_state moving={.stored_id=BLOCK_STONE,.facing=5,
		.flags=PISTON_MOVING_FLAG_EXTENDING,.current_progress=.5F,
		.last_progress=.5F};
	client_piston_moving_clear();
	assert(client_piston_moving_upsert(1,64,0,&moving));
	assert(!client_session_local_respawn_readiness(&s,&w,true,false,false));
	moving.current_progress=moving.last_progress=.8F;
	assert(client_piston_moving_upsert(1,64,0,&moving));
	assert(client_session_local_respawn_readiness(&s,&w,true,false,false));
	s.local_respawn_ack_send_pending=false;client_piston_moving_clear();
	chunk_set_block_raw(c,1,0,0,(struct block_data){.type=BLOCK_AIR});
	chunk_set_block_raw(c, 0, 0, 0, (struct block_data){.type=BLOCK_AIR});
	/* Move the success probe into an absent vertical slice after the collision
	 * discriminator; horizontal residency remains the same full ring. */
	s.local_respawn_y = 81.62;
	struct chunk* c2 = chunk_alloc(); assert(c2);
	chunk_init(c2, &w, 0, 80, 0); center->column[5] = c2;
	assert(!client_session_local_respawn_readiness(&s, &w, false, false, false));
	assert(!client_session_local_respawn_readiness(&s, &w, true, true, false));
	assert(!client_session_local_respawn_readiness(&s, &w, true, false, true));
	assert(client_session_local_respawn_readiness(&s, &w, true, false, false));
	assert(s.local_respawn_ack_send_pending);
	assert(!client_session_local_respawn_readiness(&s, &w, true, false, false));
	world_destroy(&w);

	/* A zero mirror means the production MAX_VIEW_DISTANCE, not an empty ring.
	 * Populate every real ring cell but one outer-boundary discriminator: that
	 * omission must block, and adding exactly that cell must admit. */
	struct client_session z = {.local_bound=true, .local_ticket=10,
		.local_generation=9, .local_phase=CLIENT_GUEST_ATTACHED,
		.local_respawn_loading=true, .local_respawn_epoch=3,
		.local_respawn_x=.5, .local_respawn_y=81.62, .local_respawn_z=.5};
	struct world wz;
	world_create(&wz);
	world_set_server_view_radius(0);
	for(int x=-MAX_VIEW_DISTANCE; x<=MAX_VIEW_DISTANCE; x++) {
		for(int zc=-MAX_VIEW_DISTANCE; zc<=MAX_VIEW_DISTANCE; zc++) {
			if(x == MAX_VIEW_DISTANCE && zc == 0) continue;
			memset(dict_wsection_safe_get(wz.sections, SECTION_TO_ID(x,zc)),
				0, sizeof(struct world_section));
		}
	}
	struct world_section* zcenter = dict_wsection_get(
		wz.sections, SECTION_TO_ID(0,0));
	struct chunk* zlower = chunk_alloc(); assert(zlower);
	chunk_init(zlower, &wz, 0, 64, 0); zcenter->column[4] = zlower;
	struct chunk* zc = chunk_alloc(); assert(zc);
	chunk_init(zc, &wz, 0, 80, 0); zcenter->column[5] = zc;
	for(int x=0; x<CHUNK_SIZE; x++) for(int y=0; y<CHUNK_SIZE; y++)
		for(int zc_i=0; zc_i<CHUNK_SIZE; zc_i++) {
			chunk_set_block_raw(zlower, x, y, zc_i,
				(struct block_data){.type=BLOCK_AIR});
			chunk_set_block_raw(zc, x, y, zc_i,
				(struct block_data){.type=BLOCK_AIR});
		}
	zlower->rebuild_displist = zlower->mesh_in_flight = false;
	zc->rebuild_displist = false;
	zc->mesh_in_flight = false;
	assert(!client_session_local_respawn_readiness(&z, &wz, true, false, false));
	memset(dict_wsection_safe_get(wz.sections,
		SECTION_TO_ID(MAX_VIEW_DISTANCE,0)), 0, sizeof(struct world_section));
	assert(client_session_local_respawn_readiness(&z, &wz, true, false, false));
	world_destroy(&wz);
	world_set_server_view_radius(saved_radius);
	puts("PASS respawn readiness: real missing ring, mesh, AABB, world/failure/hold gates; radius zero uses full MAX ring");
}

static void test_respawn_clearance_numeric_edges(void) {
	struct AABB body;
	double y=(double)((float)64+PLAYER_EYE_HEIGHT);
	assert(respawn_clearance_body(0.5,y,0.5,&body));
	assert(!respawn_clearance_body(NAN,y,0.5,&body));
	assert(!respawn_clearance_body(0.5,INFINITY,0.5,&body));
	assert(!respawn_clearance_body(0.5,y,-INFINITY,&body));
	/* Largest positive float whose derived +margin scan remains int32. */
	float pos=nextafterf((float)INT32_MAX,0.0F);
	assert(respawn_clearance_body((double)pos,y,0.5,&body));
	assert(!respawn_clearance_body((double)(float)INT32_MAX,y,0.5,&body));
	assert(respawn_clearance_body((double)(float)INT32_MIN,y,0.5,&body));
	assert(!respawn_clearance_body((double)nextafterf((float)INT32_MIN,-INFINITY),
		y,0.5,&body));
	puts("PASS respawn clearance numeric edges: self-contained bounds reject nonfinite/unrepresentable derived scans");
}

static unsigned recorded_request_count;
static enum local_guest_action recorded_request_action[4];
static struct server_rpc_source recorded_request_source[4];

static void record_local_request(struct server_rpc_source source,
	struct server_rpc* call, void* unused) {
	(void)unused;
	assert(call->type == SRPC_LOCAL_GUEST && recorded_request_count < 4);
	recorded_request_source[recorded_request_count] = source;
	recorded_request_action[recorded_request_count++]
		= (enum local_guest_action)call->payload.local_guest.action;
}

static unsigned saturate_svin_ordinary(void) {
	struct server_rpc pressure = {.type=SRPC_USE_ITEM};
	unsigned accepted = 0;
	while(svin_rpc_send(&pressure)) {
		accepted++;
		assert(accepted < 256);
	}
	assert(accepted > 0 && svin_pending_count() > 0);
	return accepted;
}

static void test_observed_leave_receipt(void) {
	assert(client_session_local_leave_observe(NULL, NULL)
		== CLIENT_LOCAL_LEAVE_STALE);

	/* FAILED is a local observation, not a revocation of committed intent. A
	 * later exact authoritative LEFT still retires this lifetime once. */
	assert(svin_init());
	struct client_session failed = {0};
	uint64_t failed_ticket = attach(&failed, 171);
	struct client_local_leave_token token, repeated;
	assert(client_session_local_leave_begin(&failed, &token));
	assert(token.ticket == failed_ticket && token.generation == 171);
	assert(client_session_local_leave_begin(&failed, &repeated));
	assert(repeated.ticket == token.ticket
		&& repeated.generation == token.generation);
	client_session_flush_local(&failed);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1
		&& recorded_request_action[0] == LOCAL_GUEST_LEAVE);
	feed(&failed, failed_ticket, 170, LOCAL_GUEST_LEFT);
	assert(failed.local_phase == CLIENT_GUEST_LEAVING
		&& client_session_local_leave_observe(&failed, &token)
			== CLIENT_LOCAL_LEAVE_PENDING);
	feed(&failed, failed_ticket, 171, LOCAL_GUEST_LEAVE_FAILED);
	assert(failed.local_phase == CLIENT_GUEST_FAILED && !failed.local_pending
		&& client_session_local_leave_observe(&failed, &token)
			== CLIENT_LOCAL_LEAVE_FAILED);
	assert(client_session_local_leave_begin(&failed, &repeated));
	assert(!failed.local_pending
		&& client_session_local_leave_observe(&failed, &token)
			== CLIENT_LOCAL_LEAVE_FAILED);
	feed(&failed, failed_ticket, 171, LOCAL_GUEST_LEFT);
	assert(failed.local_phase == CLIENT_GUEST_IDLE
		&& client_session_local_leave_observe(&failed, &token)
			== CLIENT_LOCAL_LEAVE_LEFT);
	assert(client_session_request_local_join(&failed));
	assert(client_session_local_leave_observe(&failed, &token)
		== CLIENT_LOCAL_LEAVE_STALE);
	client_session_clear_windows(&failed);

	/* An unbound Cancel logically owns generation zero. The server may process
	 * Join first and return LEFT with the transient offered generation; retain
	 * zero so the token captured before that race observes its own success. */
	assert(svin_init());
	struct client_session cancel = {0};
	assert(client_session_request_local_join(&cancel));
	uint64_t cancel_ticket = cancel.local_ticket;
	client_session_flush_local(&cancel);
	assert(client_session_local_leave_begin(&cancel, &token));
	assert(token.ticket == cancel_ticket && token.generation == 0);
	client_session_flush_local(&cancel);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 2
		&& recorded_request_action[0] == LOCAL_GUEST_JOIN
		&& recorded_request_action[1] == LOCAL_GUEST_CANCEL);
	feed(&cancel, cancel_ticket, 172, LOCAL_GUEST_OFFER);
	assert(cancel.local_phase == CLIENT_GUEST_LEAVING && !cancel.local_bound);
	feed(&cancel, cancel_ticket, 172, LOCAL_GUEST_LEFT);
	assert(client_session_local_leave_observe(&cancel, &token)
		== CLIENT_LOCAL_LEAVE_LEFT
		&& cancel.local_leave_receipt.generation == 0);
	assert(client_session_bind_local(&cancel, 172));
	assert(client_session_local_leave_observe(&cancel, &token)
		== CLIENT_LOCAL_LEAVE_STALE);
	client_session_clear_windows(&cancel);

	/* Observer ownership turns the 20 s edge into one TIMED_OUT result. No
	 * background request train is permitted; either owner may explicitly rearm
	 * the same token, and a second press while PENDING remains deduplicated. */
	assert(svin_init());
	struct client_session timed = {0};
	uint64_t timed_ticket = attach(&timed, 173);
	assert(client_session_local_leave_begin(&timed, &token));
	client_session_flush_local(&timed);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1);
	ptime_t expired = time_add_ms(timed.local_leave_deadline, 1);
	client_session_local_leave_tick(&timed, expired);
	assert(!timed.local_pending
		&& client_session_local_leave_observe(&timed, &token)
			== CLIENT_LOCAL_LEAVE_TIMED_OUT);
	for(unsigned k = 0; k < 3; k++) client_session_flush_local(&timed);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 0);
	struct client_local_leave_token wrong = token;
	wrong.ticket++;
	assert(!client_session_local_leave_release(&timed, &wrong)
		&& timed.local_leave_receipt_manual_retry);
	assert(client_session_local_leave_release(&timed, &token)
		&& !timed.local_leave_receipt_manual_retry);
	client_session_local_leave_tick(&timed, time_add_ms(expired, 1));
	assert(timed.local_pending && timed.local_leave_deadline_armed
		&& client_session_local_leave_observe(&timed, &token)
			== CLIENT_LOCAL_LEAVE_PENDING);
	assert(client_session_local_leave_begin(&timed, &repeated)
		&& timed.local_leave_receipt_manual_retry);
	assert(client_session_local_leave_release(&timed, &token));
	assert(client_session_request_local_leave(&timed)); /* guest Pause Retry */
	assert(client_session_local_leave_observe(&timed, &token)
		== CLIENT_LOCAL_LEAVE_PENDING);
	assert(client_session_request_local_leave(&timed)); /* deduplicated */
	assert(!client_session_local_leave_retry(&timed, &token));
	client_session_flush_local(&timed);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1
		&& recorded_request_action[0] == LOCAL_GUEST_LEAVE);
	feed(&timed, timed_ticket, 173, LOCAL_GUEST_LEFT);
	assert(client_session_local_leave_observe(&timed, &token)
		== CLIENT_LOCAL_LEAVE_LEFT);

	/* If guest Pause exists when the coordinator retires, the same timeout is
	 * handed to its explicit Retry state instead of starting background sends. */
	struct client_session pause_handoff = {0};
	(void)attach(&pause_handoff, 174);
	assert(client_session_local_leave_begin(&pause_handoff, &token));
	client_session_local_leave_tick(&pause_handoff,
		time_add_ms(pause_handoff.local_leave_deadline, 1));
	pause_handoff.pause.open = true;
	pause_handoff.pause.exit_pending = true;
	assert(client_session_local_leave_release(&pause_handoff, &token));
	assert(pause_handoff.pause.exit_failed
		&& !pause_handoff.pause.exit_pending && pause_handoff.pause.selected == 3
		&& !pause_handoff.local_pending);
	/* Retiring the coordinator must hand the renamed cell to the real Retry
	 * activation, preserving the same leave receipt and owned generation. */
	struct input_player_frame retry = {.connected=true,
		.category=INPUT_CAT_GAMECUBE};
	retry.pressed[IB_GUI_CLICK] = true;
	assert(local_pause_update(&pause_handoff.pause, &pause_handoff, &retry,
		0, 0, 640, 240) == LOCAL_PAUSE_NONE);
	assert(pause_handoff.pause.page == LOCAL_PAUSE_MENU
		&& pause_handoff.pause.exit_pending && !pause_handoff.pause.exit_failed
		&& pause_handoff.local_pending && pause_handoff.local_leave_deadline_armed
		&& pause_handoff.local_ticket == token.ticket
		&& pause_handoff.local_generation == token.generation
		&& client_session_local_leave_observe(&pause_handoff, &token)
			== CLIENT_LOCAL_LEAVE_PENDING);
	client_session_clear_windows(&pause_handoff);

	/* The death UI's direct-send retry shares the same receipt instead of
	 * leaving the host coordinator on a stale FAILED result. */
	assert(svin_init());
	struct client_session direct = {0};
	uint64_t direct_ticket = attach(&direct, 175);
	assert(client_session_local_leave_begin(&direct, &token));
	client_session_flush_local(&direct);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1);
	feed(&direct, direct_ticket, 175, LOCAL_GUEST_LEAVE_FAILED);
	assert(client_session_try_local_leave(&direct)
		&& client_session_local_leave_observe(&direct, &token)
			== CLIENT_LOCAL_LEAVE_PENDING
		&& direct.local_leave_deadline_armed);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1
		&& recorded_request_action[0] == LOCAL_GUEST_LEAVE);
	feed(&direct, direct_ticket, 175, LOCAL_GUEST_LEFT);
	assert(client_session_local_leave_observe(&direct, &token)
		== CLIENT_LOCAL_LEAVE_LEFT);

	assert(svin_init());
	struct client_session explicit_retry = {0};
	uint64_t retry_ticket = attach(&explicit_retry, 176);
	assert(client_session_local_leave_begin(&explicit_retry, &token));
	client_session_flush_local(&explicit_retry);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1);
	feed(&explicit_retry, retry_ticket, 176, LOCAL_GUEST_LEAVE_FAILED);
	assert(client_session_local_leave_release(&explicit_retry, &token));
	assert(client_session_local_leave_observe(&explicit_retry, &token)
		== CLIENT_LOCAL_LEAVE_FAILED && !explicit_retry.local_pending);
	assert(client_session_local_leave_begin(&explicit_retry, &repeated));
	assert(client_session_local_leave_retry(&explicit_retry, &token));
	assert(!client_session_local_leave_retry(&explicit_retry, &token));
	client_session_flush_local(&explicit_retry);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1);
	feed(&explicit_retry, retry_ticket, 176, LOCAL_GUEST_LEFT);
	assert(client_session_local_leave_observe(&explicit_retry, &token)
		== CLIENT_LOCAL_LEAVE_LEFT);

	/* Ordinary clear is a hard invalidation; the same late reply cannot restore
	 * terminal evidence after world/session replacement. */
	struct client_session cleared = {0};
	uint64_t cleared_ticket = attach(&cleared, 177);
	assert(client_session_local_leave_begin(&cleared, &token));
	feed(&cleared, cleared_ticket, 177, LOCAL_GUEST_LEAVE_FAILED);
	client_session_clear_windows(&cleared);
	feed(&cleared, cleared_ticket, 177, LOCAL_GUEST_LEFT);
	assert(client_session_local_leave_observe(&cleared, &token)
		== CLIENT_LOCAL_LEAVE_STALE);
	puts("PASS observed Leave: failed late LEFT, unbound generation normalization, reset/bind invalidation, one retry owner");
}

static unsigned death_request_count;
static struct server_rpc_source death_request_source;
static struct server_rpc death_request;
static void record_death_request(struct server_rpc_source source,
	struct server_rpc* call,void* unused) {
	(void)unused;death_request_count++;death_request_source=source;
	death_request=*call;
}

static void test_owned_death_mailbox(void) {
	assert(svin_init());
	struct client_session s={0};
	struct entity body={.type=ENTITY_LOCAL_PLAYER,.id=35,.health=0};
	uint64_t ticket=attach(&s,141);
	s.local_player=&body;s.local_gameplay_ready=true;
	struct input_player_frame frame={.connected=true};
	gstate.config_death_hold_respawn_ms=0;
	screen_death_owner_enter(&s);
	assert(screen_death_owner_update(&s,1,&frame,.5F,0,240,640,240));
	frame.down[IB_GUI_CLICK]=frame.pressed[IB_GUI_CLICK]=true;
	screen_death_owner_update(&s,1,&frame,.016F,0,240,640,240);
	assert(s.death.commit_pending==DEATH_COMMIT_RESPAWN
		&&s.death.request_generation==141&&s.death.completed_floor==0);
	death_request_count=0;
	svin_process_owned_messages(record_death_request,NULL,false);
	assert(death_request_count==1&&death_request_source.slot==1
		&&death_request_source.generation==141
		&&death_request.type==SRPC_RESPAWN
		&&death_request.payload.respawn.owner==1
		&&death_request.payload.respawn.generation==141
		&&!death_request.payload.respawn.respawn_epoch);
	feed_respawn(&s,ticket,141,LOCAL_GUEST_RESPAWN_LOADING,6,12.5,70,4.5);
	assert(s.death.active&&s.local_player==&body&&s.local_respawn_loading);
	feed_respawn(&s,ticket,141,LOCAL_GUEST_CORRECTION,6,12.5,70,4.5);
	assert(s.death.active);
	feed_respawn(&s,ticket,141,LOCAL_GUEST_GAMEPLAY_READY,6,0,0,0);
	frame=(struct input_player_frame){.connected=true};
	screen_death_owner_update(&s,1,&frame,.016F,0,240,640,240);
	assert(!s.death.active&&s.local_respawn_completed_epoch==6);
	client_session_clear_windows(&s);
	assert(!s.death.active&&!s.death.commit_pending);

	/* The same real FIFO at capacity rejects activation without fabricating a
	 * pending commit; a later fresh press remains retryable. */
	assert(svin_init());
	(void)saturate_svin_ordinary();
	struct client_session blocked={0};
	struct entity blocked_body={.type=ENTITY_LOCAL_PLAYER,.id=36,.health=0};
	(void)attach(&blocked,142);blocked.local_player=&blocked_body;
	blocked.local_gameplay_ready=true;
	frame=(struct input_player_frame){.connected=true};
	screen_death_owner_enter(&blocked);
	screen_death_owner_update(&blocked,1,&frame,.5F,0,240,640,240);
	frame.down[IB_GUI_CLICK]=frame.pressed[IB_GUI_CLICK]=true;
	screen_death_owner_update(&blocked,1,&frame,.016F,0,240,640,240);
	assert(blocked.death.commit_pending==DEATH_COMMIT_NONE);
	assert(svin_init());
	frame=(struct input_player_frame){.connected=true};
	screen_death_owner_update(&blocked,1,&frame,.016F,0,240,640,240);
	frame.down[IB_GUI_CLICK]=frame.pressed[IB_GUI_CLICK]=true;
	screen_death_owner_update(&blocked,1,&frame,.016F,0,240,640,240);
	assert(blocked.death.commit_pending==DEATH_COMMIT_RESPAWN);
	client_session_clear_windows(&blocked);
	puts("PASS owner death UI: real tagged respawn mailbox and READY completion");
}

static void test_respawn_ack_leave_queue(void) {
	/* With both real server-input tiers saturated, flush must preserve an ACK
	 * that was not accepted. Leave then owns the retry state and removes only
	 * that unsent ACK; it must not pretend either request entered the FIFO. */
	assert(svin_init());
	unsigned saturated_at = saturate_svin_ordinary();
	struct client_session blocked = {0};
	struct entity blocked_body = {.type=ENTITY_LOCAL_PLAYER, .id=27};
	uint64_t blocked_ticket = attach(&blocked, 121);
	blocked.local_player = &blocked_body;
	feed(&blocked, blocked_ticket, 121, LOCAL_GUEST_GAMEPLAY_READY);
	feed_respawn(&blocked, blocked_ticket, 121, LOCAL_GUEST_RESPAWN_LOADING,
		8, 40.5, 76, 2.5);
	blocked.local_respawn_ack_send_pending = true;
	client_session_flush_local(&blocked);
	assert(blocked.local_respawn_ack_send_pending
		&& !blocked.local_respawn_awaiting_commit);
	assert(client_session_request_local_leave(&blocked));
	assert(!blocked.local_respawn_ack_send_pending
		&& blocked.local_pending
		&& blocked.local_pending_action == LOCAL_GUEST_LEAVE);
	client_session_flush_local(&blocked);
	assert(blocked.local_pending && !blocked.local_respawn_awaiting_commit);
	client_session_clear_windows(&blocked);

	/* On an empty real queue, ACK and then Leave are accepted in FIFO order.
	 * Commit replies queued afterward through the tagged client inbox are inert
	 * in LEAVING. A failed Leave rearms the same owned request; LEFT clears the
	 * generation-scoped tuple and lets client_interface retire the body. */
	assert(svin_init());
	struct client_session* s = &gstate.players[1];
	memset(s, 0, sizeof(*s));
	uint64_t ticket = attach(s, 131);
	struct entity* body = calloc(1, sizeof(*body));
	assert(body);
	body->type = ENTITY_LOCAL_PLAYER;
	body->id = 31;
	body->pos[0] = body->pos_old[0] = body->network_pos[0] = 5;
	body->pos[1] = body->pos_old[1] = body->network_pos[1] = 70;
	body->pos[2] = body->pos_old[2] = body->network_pos[2] = 6;
	*dict_entity_safe_get(gstate.entities, body->id) = body;
	s->local_player = body;
	feed(s, ticket, 131, LOCAL_GUEST_GAMEPLAY_READY);
	feed_respawn(s, ticket, 131, LOCAL_GUEST_RESPAWN_LOADING,
		9, 50.5, 78, -6.5);
	s->local_respawn_completed_epoch = 5;
	s->local_respawn_ack_send_pending = true;
	client_session_flush_local(s);
	assert(!s->local_respawn_ack_send_pending
		&& s->local_respawn_awaiting_commit);
	assert(client_session_request_local_leave(s));
	client_session_flush_local(s);
	assert(!s->local_pending && s->local_phase == CLIENT_GUEST_LEAVING);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 2
		&& recorded_request_action[0] == LOCAL_GUEST_RESPAWN_READY
		&& recorded_request_action[1] == LOCAL_GUEST_LEAVE);
	for(unsigned k = 0; k < recorded_request_count; k++)
		assert(recorded_request_source[k].slot == 1
			&& recorded_request_source[k].generation == 131);

	struct client_rpc correction = respawn_reply(ticket, 131,
		LOCAL_GUEST_CORRECTION, 1, 9, 99, 99, 99);
	struct client_rpc ready = respawn_reply(ticket, 131,
		LOCAL_GUEST_GAMEPLAY_READY, 1, 9, 0, 0, 0);
	assert(clin_rpc_send_local(&correction, 1, 131));
	assert(clin_rpc_send_local(&ready, 1, 131));
	clin_update_drain();
	assert(s->local_phase == CLIENT_GUEST_LEAVING && s->local_player == body
		&& s->local_respawn_loading && !s->local_gameplay_ready
		&& !s->local_respawn_correction_installed
		&& s->local_respawn_completed_epoch == 5
		&& body->pos[0] == 5 && body->pos[1] == 70 && body->pos[2] == 6);

	struct client_rpc failed = respawn_reply(ticket, 131,
		LOCAL_GUEST_LEAVE_FAILED, 1, 0, 0, 0, 0);
	assert(clin_rpc_send_local(&failed, 1, 131));
	clin_update_drain();
	assert(s->local_phase == CLIENT_GUEST_LEAVING && s->local_pending
		&& s->local_pending_action == LOCAL_GUEST_LEAVE
		&& s->local_player == body && s->local_respawn_loading);
	client_session_flush_local(s);
	assert(!s->local_pending);
	recorded_request_count = 0;
	svin_process_owned_messages(record_local_request, NULL, false);
	assert(recorded_request_count == 1
		&& recorded_request_action[0] == LOCAL_GUEST_LEAVE);

	struct client_rpc left = respawn_reply(ticket, 131,
		LOCAL_GUEST_LEFT, 1, 0, 0, 0, 0);
	assert(clin_rpc_send_local(&left, 1, 131));
	clin_update_drain();
	assert(s->local_phase == CLIENT_GUEST_IDLE && !s->local_bound
		&& !s->local_player && !s->local_respawn_loading
		&& !s->local_respawn_ack_send_pending
		&& !s->local_respawn_awaiting_commit
		&& !s->local_respawn_completed_epoch
		&& !dict_entity_get(gstate.entities, 31));
	printf("PASS respawn ACK/Leave queue: saturation at %u preserves unsent ACK; FIFO ACK->Leave, inert commits, failed retry, LEFT cleanup\n",
		saturated_at);
}

static void test_host_respawn_fifo_lifetime(void) {
	struct client_session* s=&gstate.players[0];
	struct entity body={.type=ENTITY_LOCAL_PLAYER,.id=0,.health=0,
		.pos={2,70,3},.pos_old={2,70,3},.network_pos={2,70,3},
		.vel={4,5,6},.on_ground=true};
	memset(s,0,sizeof(*s));s->local_player=&body;
	s->camera.x=2;s->camera.y=70;s->camera.z=3;
	s->death.active=true;s->death.completed_floor=0;
	s->respawn_request_observation_pending=true;
	struct client_rpc call={.type=CRPC_HOST_RESPAWN,
		.payload.host_respawn={.result=HOST_RESPAWN_LOADING,.owner=1,
			.generation=0,.respawn_epoch=7,.x=40.5,.y=75,.z=-4.5}};
	clin_process(&call);
	assert(!s->host_respawn_loading);
	call.payload.host_respawn.owner=0;clin_process(&call);
	assert(s->host_respawn_loading&&s->host_respawn_epoch==7
		&&s->host_respawn_render_camera_frozen
		&&client_session_local_render_camera(s)==&s->host_respawn_render_camera
		&&!s->respawn_request_observation_pending);
	call.payload.host_respawn.x=99;clin_process(&call);
	assert(s->host_respawn_x==40.5); /* equal LOADING preserves progress */
	call.payload.host_respawn.result=HOST_RESPAWN_GAMEPLAY_READY;
	clin_process(&call);assert(!s->host_respawn_completed_epoch);
	call.payload.host_respawn.result=HOST_RESPAWN_CORRECTION;
	call.payload.host_respawn.x=40.5;call.payload.host_respawn.y=75;
	call.payload.host_respawn.z=-4.5;call.payload.host_respawn.correction_seq=11;
	call.payload.host_respawn.correction_flags=PLAYER_CORRECTION_POSITION_ONLY
		|PLAYER_CORRECTION_ZERO_VEL_X|PLAYER_CORRECTION_ZERO_VEL_Y
		|PLAYER_CORRECTION_ZERO_VEL_Z|PLAYER_CORRECTION_SET_ON_GROUND;
	clin_process(&call);
	assert(s->host_respawn_correction_installed&&body.pos[0]==40.5F
		&&body.vel[0]==0&&body.vel[1]==0&&body.vel[2]==0&&!body.on_ground
		&&s->camera.x==2);
	call.payload.host_respawn.result=HOST_RESPAWN_GAMEPLAY_READY;
	clin_process(&call);
	assert(s->host_respawn_completed_epoch==7&&!s->host_respawn_loading
		&&!s->host_respawn_render_camera_frozen);
	/* Reset retires an unfinished epoch and blocks all of its delayed traffic. */
	s->host_respawn_loading=true;s->host_respawn_epoch=9;
	client_session_host_respawn_world_reset(s);
	assert(s->host_respawn_completed_epoch==9&&!s->host_respawn_loading);
	call.payload.host_respawn.result=HOST_RESPAWN_LOADING;
	call.payload.host_respawn.respawn_epoch=9;clin_process(&call);
	assert(!s->host_respawn_loading&&s->host_respawn_completed_epoch==9);
	puts("PASS host respawn FIFO: route/epoch fences, frozen camera, corrected READY, unfinished reset retirement");
}

static void test_guest_failure_dispatch_lifetime(void) {
	struct client_session* s=&gstate.players[1];memset(s,0,sizeof(*s));
	uint64_t ticket=attach(s,151);
	struct entity* body=calloc(1,sizeof(*body));assert(body);
	body->type=ENTITY_LOCAL_PLAYER;body->id=50;body->health=0;
	*dict_entity_safe_get(gstate.entities,body->id)=body;
	s->local_player=body;s->local_gameplay_ready=true;s->death.active=true;
	s->death.request_generation=151;s->death.completed_floor=0;
	s->respawn_request_observation_pending=true;
	struct client_rpc failure=respawn_reply(ticket,151,
		LOCAL_GUEST_RESPAWN_RETRY,1,0,0,0,0);
	/* The shipping tagged route refuses a wrong owner before enqueue. */
	failure.payload.local_guest.owner=0;
	assert(!clin_rpc_send_local(&failure,1,151));
	failure.payload.local_guest.owner=1;
	/* A delivered stale generation remains inert in the real drain. */
	failure.payload.local_guest.generation=150;
	assert(clin_rpc_send_local(&failure,1,150));clin_update_drain();
	assert(s->respawn_request_observation_pending&&!s->respawn_failure_serial);
	failure.payload.local_guest.generation=151;
	assert(clin_rpc_send_local(&failure,1,151));clin_update_drain();
	assert(!s->respawn_request_observation_pending
		&&s->respawn_failure==RESPAWN_FAILURE_RETRY
		&&s->respawn_failure_serial==1);
	/* Consumed observation makes an identical FIFO reply inert. */
	assert(clin_rpc_send_local(&failure,1,151));clin_update_drain();
	assert(s->respawn_failure_serial==1);

	s->respawn_request_observation_pending=true;
	struct client_rpc loading=respawn_reply(ticket,151,
		LOCAL_GUEST_RESPAWN_LOADING,1,4,20.5,72,-2.5);
	assert(clin_rpc_send_local(&loading,1,151));clin_update_drain();
	assert(s->local_respawn_loading&&!s->respawn_request_observation_pending
		&&s->respawn_failure==RESPAWN_FAILURE_NONE);
	failure.payload.local_guest.respawn_epoch=4;
	assert(clin_rpc_send_local(&failure,1,151));clin_update_drain();
	assert(s->respawn_failure_serial==1); /* first LOADING consumed the press */
	/* residency-1005/5: the server's bounded LOADING termination is its own
	 * result. A foreign epoch is inert; the matching epoch ends the loading
	 * screen, keeps the dead body installed and re-arms the death card. */
	struct client_rpc lfail=respawn_reply(ticket,151,
		LOCAL_GUEST_RESPAWN_LOADING_FAILED,1,3,0,0,0);
	assert(clin_rpc_send_local(&lfail,1,151));clin_update_drain();
	assert(s->local_respawn_loading&&s->respawn_failure_serial==1);
	lfail.payload.local_guest.respawn_epoch=4;
	assert(clin_rpc_send_local(&lfail,1,151));clin_update_drain();
	assert(!s->local_respawn_loading&&!s->local_respawn_epoch
		&&s->local_gameplay_ready&&s->local_player==body
		&&s->respawn_failure==RESPAWN_FAILURE_RETRY
		&&s->respawn_failure_epoch==4&&s->respawn_failure_serial==2);
	assert(clin_rpc_send_local(&lfail,1,151));clin_update_drain();
	assert(s->respawn_failure_serial==2); /* duplicate inert */
	s->local_respawn_loading=true;s->local_respawn_epoch=4;
	s->local_gameplay_ready=false;s->respawn_failure_serial=1;

	/* A later death gets a fresh epoch-zero observation after floor E. */
	s->local_respawn_loading=false;s->local_respawn_epoch=0;
	s->local_respawn_completed_epoch=4;s->death.completed_floor=4;
	s->respawn_request_observation_pending=true;
	failure.payload.local_guest.respawn_epoch=0;
	failure.payload.local_guest.result=LOCAL_GUEST_RESPAWN_TERMINAL;
	assert(clin_rpc_send_local(&failure,1,151));clin_update_drain();
	assert(s->respawn_failure==RESPAWN_FAILURE_TERMINAL
		&&s->respawn_failure_serial==2&&!s->respawn_request_observation_pending);

	if(s->local_player) {
		s->local_player=NULL;dict_entity_erase(gstate.entities,body->id);free(body);
	}
	client_session_clear_windows(s);
	puts("PASS guest failure FIFO: wrong route/generation inert, once-only observation, LOADING consumption, later-death epoch0 terminal");
}

static void test_manual_leave_failed_dispatch(void) {
	assert(svin_init());
	struct client_session* s=&gstate.players[1];memset(s,0,sizeof(*s));
	uint64_t ticket=attach(s,161);
	struct entity* body=calloc(1,sizeof(*body));assert(body);
	body->type=ENTITY_LOCAL_PLAYER;body->id=51;body->health=0;
	*dict_entity_safe_get(gstate.entities,body->id)=body;
	s->local_player=body;s->local_gameplay_ready=true;s->local_respawn_loading=true;
	s->local_respawn_epoch=12;s->death.active=true;
	assert(client_session_try_local_leave(s));
	s->death.exit_pending=true;s->death.commit_pending=DEATH_COMMIT_EXIT;
	assert(s->local_phase==CLIENT_GUEST_LEAVING&&s->local_death_leave_manual);
	/* A commit reply is discarded in LEAVING; even a later health update must
	 * not make this retained, uncorrected body gameplay-active. */
	struct client_rpc ready=respawn_reply(ticket,161,
		LOCAL_GUEST_GAMEPLAY_READY,1,12,0,0,0);
	assert(clin_rpc_send_local(&ready,1,161));clin_update_drain();
	body->health=MAX_PLAYER_HEALTH;
	struct client_rpc failed=respawn_reply(ticket,161,
		LOCAL_GUEST_LEAVE_FAILED,1,0,0,0,0);
	assert(clin_rpc_send_local(&failed,1,161));clin_update_drain();
	assert(s->local_phase==CLIENT_GUEST_FAILED&&s->local_death_leave_manual
		&&s->death.active&&s->death.exit_failed
		&&s->local_respawn_loading&&!client_session_local_gameplay_active(s));
	assert(client_session_try_local_leave(s));
	struct client_rpc left=respawn_reply(ticket,161,LOCAL_GUEST_LEFT,1,0,0,0,0);
	assert(clin_rpc_send_local(&left,1,161));clin_update_drain();
	assert(s->local_phase==CLIENT_GUEST_IDLE&&!s->local_player
		&&!s->local_death_leave_manual&&!dict_entity_get(gstate.entities,51));
	puts("PASS manual death Leave FIFO: discarded READY + full health stays exit-only after FAILED; explicit retry + LEFT releases");
}

int main(void) {
	blocks_init();
	assert(clin_init() && svin_init());
	dict_entity_init(gstate.entities);
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	test_request_shape();
	test_ready_sets_the_gate();
	test_rebind_clears_admission();
	test_two_view_gate();
	test_respawn_order_and_retention();
	test_respawn_wrong_owner();
	test_real_world_readiness();
	test_respawn_clearance_numeric_edges();
	test_observed_leave_receipt();
	test_owned_death_mailbox();
	test_respawn_ack_leave_queue();
	test_host_respawn_fifo_lifetime();
	test_guest_failure_dispatch_lifetime();
	test_manual_leave_failed_dispatch();
	puts("test_local_join_ready: PASS");
	return 0;
}
