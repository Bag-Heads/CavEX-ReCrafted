/* Split-dims B2 main.c wiring (B2-MAINC-PLAN.md), the per-frame WORLD passes
 * main.c now runs through source/network/client_frame_worlds.c:
 *  T1 flag OFF: every pass makes EXACTLY the pre-B2 main.c call on
 *     &gstate.world (same function, same world, same cameras/arguments, same
 *     count), recorded through --wrap'ed world_* entry points;
 *  T2 flag ON, two payloads: each world pre-rendered with only its own view's
 *     camera, lit / LOD'ed with its own cameras, built with the mesher token
 *     budget split (first world the full CHUNK_MESHER_QLENGTH, second the
 *     remainder), and EVERY storage handed off each frame (drawn -> true,
 *     undrawn -> false, so last frame's pin drains);
 *  T3 per-view environment: OFF returns main.c's daytime and the gstate
 *     weather; ON each view reads its own clock, a Nether view has no sky and
 *     no weather, a paused view keeps its previous daytime.
 * Synthetic stamps: mechanism only. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "daytime.h"
#include "game/camera.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/items.h"
#include "network/client_frame_worlds.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/server_interface.h"
#include "util.h"
#include "world.h"
#include "worldgen/beta_math.h"

#define CAVEWII_F1_DISPATCH_ONLY
#include "../source/main.c"
#undef CAVEWII_F1_DISPATCH_ONLY

#define LOCAL_EID 77

/* ---- recorded world_* calls ------------------------------------------- */
enum call_kind { PRE, PRE_VIEWS, PRE_CLEAR, LIGHT, LOD, LOD_VIEWS, RING, BUILD,
				 DONE, WAIT };
struct call {
	enum call_kind kind;
	struct world* w;
	struct camera* cam[2];
	size_t n;     /* camera count / tokens */
	bool flag;    /* render_completed new_render / build allow_far */
	w_coord_t cx, cz;
	int radius;
};
static struct call calls[64];
static unsigned ncalls;
static bool pre_views_result = true;
static size_t build_returns[2] = {5, 2};
static struct world* no_pending_world; /* its near ring reports 0 pending */
static unsigned build_calls;
static bool real_masked_list;
void __real_world_pre_render_clear(struct world* w);
size_t __real_world_build_chunks(struct world* w, size_t tokens, bool allow_far);
void __real_world_render_completed(struct world* w, bool new_render);

static struct call* rec(enum call_kind k, struct world* w) {
	assert(ncalls < 64);
	calls[ncalls] = (struct call) {.kind = k, .w = w};
	return &calls[ncalls++];
}

void __wrap_world_pre_render(struct world* w, struct camera* c, mat4 view) {
	(void)view;
	rec(PRE, w)->cam[0] = c;
}
bool __wrap_world_pre_render_views(struct world* w, struct camera* const* c,
								   size_t n) {
	struct call* k = rec(PRE_VIEWS, w);
	k->n = n;
	for(size_t i = 0; i < n && i < 2; i++) k->cam[i] = c[i];
	return pre_views_result;
}
void __wrap_world_pre_render_clear(struct world* w) {
	rec(PRE_CLEAR, w);
	if(real_masked_list) __real_world_pre_render_clear(w);
}
void __wrap_world_update_lighting(struct world* w) { rec(LIGHT, w); }
void __wrap_world_update_lod(struct world* w) { rec(LOD, w); }
void __wrap_world_update_lod_views(struct world* w, struct camera* const* c,
								   size_t n) {
	struct call* k = rec(LOD_VIEWS, w);
	k->n = n;
	for(size_t i = 0; i < n && i < 2; i++) k->cam[i] = c[i];
}
void __wrap_world_near_ring_status(struct world* w, w_coord_t cx, w_coord_t cz,
								   int radius, int* missing, int* pending) {
	struct call* k = rec(RING, w);
	k->cx = cx;
	k->cz = cz;
	k->radius = radius;
	assert(missing == NULL && pending);
	*pending = w == no_pending_world ? 0 : 3;
}
size_t __wrap_world_build_chunks(struct world* w, size_t tokens, bool allow_far) {
	struct call* k = rec(BUILD, w);
	k->n = tokens;
	k->flag = allow_far;
	if(real_masked_list) return __real_world_build_chunks(w, tokens, allow_far);
	size_t r = build_returns[build_calls++ % 2];
	return r < tokens ? r : tokens; /* never more than it was given */
}
void __wrap_world_render_completed(struct world* w, bool new_render) {
	rec(DONE, w)->flag = new_render;
	if(real_masked_list) __real_world_render_completed(w, new_render);
}
void __wrap_world_gpu_wait_idle(void) { rec(WAIT, NULL); }

static void reset_calls(void) {
	ncalls = 0;
	build_calls = 0;
}

static struct client_ctx_registry* reg;

struct guest_pose_probe { unsigned poses; uint64_t generation; };
static void capture_guest_pose(struct server_rpc_source source,
	struct server_rpc* call, void* user) {
	struct guest_pose_probe* p=user;
	if(source.slot==1&&call->type==SRPC_PLAYER_POS) {
		p->poses++;
		p->generation=source.generation;
	}
	svin_credit_release(call);
}

static void world_reset(uint8_t index, uint32_t generation) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID, .world_index = index,
			.world_generation = generation}});
}

static void flag(bool on) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(clin_world_payloads_set_enabled(false));
	assert(clin_world_payloads_set_enabled(on));
}

static struct camera cam0, cam1;
static struct camera* cams[2] = {&cam0, &cam1};

static void test_guest_pose_during_host_transfer(void) {
	/* test_view_env left player 1 bound to the independent Overworld store.
	 * Exercise the real client sender and owner-routed mailbox, not a copy of
	 * its Boolean gate. The host's global loading flags alone must not allow
	 * movement; an accepted transfer BEGIN+END must. */
	assert(clin_world_payloads_enabled() && clin_world_view_slot(1)==0);
	struct entity guest_body={0};
	struct clin_world_payload payload;
	assert(clin_world_payload_of_slot(0, &payload));
	entity_local_player(LOCAL_EID + 1, &guest_body, payload.terrain);
	guest_body.pos[0]=guest_body.pos[2]=8.5F;
	guest_body.pos[1]=70.0F;
	world_set_block(payload.terrain, 8, 64, 8,
		(struct block_data){.type=BLOCK_STONE}, false);
	guest_body.health=MAX_PLAYER_HEALTH;
	struct client_session* guest=&gstate.players[1];
	guest->local_bound=true;
	guest->local_generation=77;
	guest->local_phase=CLIENT_GUEST_ATTACHED;
	guest->local_gameplay_ready=true;
	guest->local_player=&guest_body;
	guest->player_sync.last_pos_update=(ptime_t){0};
	gstate.world_loaded=false;
	gstate.world_switch_pending=true;
	gstate.players[0].current_screen=&screen_load_world;
	svin_set_split_accept(true);
	svin_set_local_motion(true);
	struct guest_pose_probe p={0};
	clin_update();
	struct clin_frame_loading_state loading={
		.payloads=clin_world_payloads_enabled(), .guest_view_bound=true,
		.guest_bound=guest->local_bound, .guest_attached=true,
		.guest_active=client_session_local_gameplay_active(guest),
		.guest_established=client_session_local_gameplay_active(guest),
		.host_pending=gstate.world_switch_pending,
		.host_loading_screen=gstate.players[0].current_screen==&screen_load_world,
		.host_transfer_episode=clin_host_transfer_pane_active()};
	const struct input_player_frame frame={.connected=true};
	assert(!clin_frame_loading_policy(&loading).guest_continues);
	svin_process_owned_messages(capture_guest_pose,&p,false);
	assert(p.poses==0&&!clin_host_transfer_pane_active());
	struct client_rpc hold={.type=CRPC_TRANSFER_HOLD,
		.payload.transfer_hold={.actor_slot=0,.request_id=41,.begin=true}};
	clin_process(&hold);
	hold.payload.transfer_hold.begin=false;
	clin_process(&hold);
	clin_update();
	loading.host_transfer_episode=clin_host_transfer_pane_active();
	assert(clin_frame_loading_policy(&loading).guest_continues);
	main_client_guest_tick_dispatch(&frame);
	svin_process_owned_messages(capture_guest_pose,&p,false);
	assert(clin_host_transfer_pane_active()&&p.poses==1&&p.generation==77);
	svin_set_split_accept(false);
	guest->local_player=NULL;
	guest->local_bound=false;
	guest->local_gameplay_ready=false;
	puts("PASS owner-local guest pose reaches authority during accepted host loading");
}

static void test_owner_loading_policy(void) {
	struct clin_frame_loading_state s={
		.payloads=true,.guest_view_bound=true,
		.guest_bound=true,.guest_attached=true,
		.host_pending=true,.host_loading_screen=true,.host_loaded=false,
		.guest_active=true,.guest_established=true,
	};
	/* The real host reset values alone are not a transfer: first load and
	 * recovery retain the old global presentation and simulation gate. */
	struct clin_frame_loading_policy p=clin_frame_loading_policy(&s);
	assert(!p.keep_split&&p.draw_view[0]&&p.draw_view[1]
		&& !p.host_continues&&!p.guest_continues);
	s.host_transfer_episode=true;
	p=clin_frame_loading_policy(&s);
	assert(p.keep_split&&!p.draw_view[0]&&p.draw_view[1]
		&& !p.host_continues&&p.guest_continues
		&& !p.death_view_allowed[0]&&p.death_view_allowed[1]);
	/* A dying guest remains an established owner of the Overworld view, even
	 * though the actor's gameplay predicate correctly turns false. */
	s.guest_active=false;
	p=clin_frame_loading_policy(&s);
	assert(p.keep_split&&!p.draw_view[0]&&p.draw_view[1]
		&&p.guest_continues&&p.death_view_allowed[1]);
	s.guest_respawn_loading=true;
	s.guest_view_bound=false;
	p=clin_frame_loading_policy(&s);
	assert(p.keep_split&&p.guest_continues&&!p.draw_view[1]
		&&!p.guest_transfer_loading&&p.death_view_allowed[1]
		&&"unbound guest respawn retains its own UI, not fallback host terrain");
	s.guest_respawn_loading=false;
	s.guest_view_bound=true;
	s.guest_active=true;
	s.host_transfer_episode=false;
	s.host_pending=false;
	s.host_loading_screen=false;
	s.host_ingame_screen=true;
	s.host_loaded=true;
	s.host_active=true;
	s.host_established=true;
	s.guest_active=false;
	p=clin_frame_loading_policy(&s); /* initial guest join: no transfer */
	assert(!p.keep_split);
	s.guest_transfer_episode=true;
	s.guest_held=true;
	p=clin_frame_loading_policy(&s);
	assert(p.keep_split&&p.draw_view[0]&&!p.draw_view[1]
		&&p.host_continues&&!p.guest_continues);
	s.host_active=false;
	s.host_ingame_screen=false;
	s.host_death_screen=true;
	p=clin_frame_loading_policy(&s);
	assert(p.keep_split&&p.draw_view[0]&&!p.draw_view[1]
		&&p.host_continues&&p.death_view_allowed[0]
		&&"dead host retains its pane while guest transfer loads");
	s.host_active=true;
	s.host_death_screen=false;
	s.host_ingame_screen=true;
	/* READY can precede a death in the same drain. Once the transfer episode
	 * settles, a dead ready owner gets death UI, never a stale loading card. */
	s.guest_held=false;
	s.guest_transfer_episode=false;
	p=clin_frame_loading_policy(&s);
	assert(!p.keep_split&&p.draw_view[1]&&p.death_view_allowed[1]);
	s.guest_transfer_episode=true;
	s.guest_held=true;
	/* COMMIT's body gap keeps owner-local recovery; a preexisting global
	 * pause still belongs to the single-player recovery owner. */
	s.paused=true;
	assert(!clin_frame_loading_policy(&s).keep_split);
	s.paused=false;
	s.host_transfer_episode=true;
	s.host_pending=true;
	s.host_loading_screen=true;
	s.host_ingame_screen=false;
	s.host_loaded=false;
	p=clin_frame_loading_policy(&s);
	assert(p.keep_split&&!p.draw_view[0]&&!p.draw_view[1]
		&&!p.host_continues&&!p.guest_continues);
	s.host_transfer_episode=false;
	assert(!clin_frame_loading_policy(&s).keep_split
		&&"an unrelated host loading screen is not a transfer pane");
	s.host_transfer_episode=true;
	s.host_pending=false;
	assert(!clin_frame_loading_policy(&s).keep_split
		&&"a load screen without a pending host transfer is not playable");
	s.host_pending=true;
	s.remote=true;
	assert(!clin_frame_loading_policy(&s).keep_split);
	s.remote=false;
	s.failed=true;
	assert(!clin_frame_loading_policy(&s).keep_split);
	s.failed=false;
	s.guest_view_bound=false;
	p=clin_frame_loading_policy(&s);
	assert(p.keep_split&&!p.draw_view[0]&&!p.draw_view[1]
		&&"a hidden guest landing may temporarily have no bound view");
	s.guest_transfer_episode=false;
	s.guest_active=true;
	assert(!clin_frame_loading_policy(&s).keep_split
		&&"a drawable guest requires its bound view");
	/* An initial guest join has no accepted transfer BEGIN. A rapid BEGIN +
	 * END can drain before one frame; its identity must survive the idle gap
	 * while the old guest body has been released and READY has not arrived. */
	struct client_session* guest=&gstate.players[1];
	guest->local_bound=true;
	guest->local_phase=CLIENT_GUEST_ATTACHED;
	guest->local_generation=77;
	guest->local_gameplay_ready=false;
	guest->local_player=NULL;
	assert(clin_transfer_actor_request_id(1)==0);
	struct client_rpc hold={.type=CRPC_TRANSFER_HOLD};
	hold.payload.transfer_hold.actor_slot=1;
	hold.payload.transfer_hold.actor_generation=77;
	hold.payload.transfer_hold.request_id=42;
	hold.payload.transfer_hold.begin=true;
	clin_process(&hold);
	assert(clin_transfer_actor_held(1)&&clin_transfer_actor_request_id(1)==42);
	hold.payload.transfer_hold.begin=false;
	clin_process(&hold);
	assert(!clin_transfer_actor_held(1)&&clin_transfer_actor_request_id(1)==42);
	guest->local_generation=78;
	assert(clin_transfer_actor_request_id(1)==0);
	guest->local_bound=false;
	struct client_rpc host_hold={.type=CRPC_TRANSFER_HOLD};
	host_hold.payload.transfer_hold.actor_slot=0;
	host_hold.payload.transfer_hold.request_id=43;
	host_hold.payload.transfer_hold.begin=true;
	clin_process(&host_hold);
	host_hold.payload.transfer_hold.begin=false;
	clin_process(&host_hold);
	assert(!clin_transfer_actor_held(0)&&clin_transfer_actor_request_id(0)==43);
	puts("PASS owner-local loading: one or both panes, input continuation, same-drain transfer identity");
}

static void test_off_identity(void) {
	flag(false);
	world_reset(0, 0);
	struct clin_frame_worlds f;
	/* Two views, as main.c's split frame. */
	clin_frame_worlds_begin(&f, 2);
	assert(f.n_live == 1 && f.live[0] == &gstate.world);
	assert(f.n_storage == 1 && f.storage[0] == &gstate.world);
	assert(f.view_terrain[0] == &gstate.world && f.view_terrain[1] == &gstate.world);
	assert(f.view_entities[0] == gstate.entities && f.view_entities[1] == gstate.entities);
	assert(f.shared);

	reset_calls();
	bool render_world = true;
	clin_frame_pre_render(&f, cams, &render_world);
	assert(ncalls == 1 && calls[0].kind == PRE_VIEWS && calls[0].w == &gstate.world
		   && calls[0].n == 2 && calls[0].cam[0] == &cam0 && calls[0].cam[1] == &cam1
		   && render_world);
	/* A held host still shares the terrain with its guest until reset. Only
	 * the surviving guest camera may acquire that storage's GPU references. */
	clin_frame_worlds_begin(&f,2);
	f.draw_view[0]=false;
	reset_calls();
	clin_frame_pre_render(&f,cams,&render_world);
	assert(ncalls==1&&calls[0].kind==PRE&&calls[0].cam[0]==&cam1);
	reset_calls();
	clin_frame_render_completed(&f,true);
	assert(ncalls==1&&calls[0].kind==DONE&&calls[0].flag);
	/* A simultaneous transfer hides both views of the same storage. The
	 * previous frame's render list must not be leased for an undrawn frame. */
	clin_frame_worlds_begin(&f, 2);
	f.draw_view[0] = f.draw_view[1] = false;
	reset_calls();
	clin_frame_pre_render(&f, cams, &render_world);
	assert(ncalls == 1 && calls[0].kind == PRE_CLEAR
		   && calls[0].w == &gstate.world);
	reset_calls();
	clin_frame_render_completed(&f, true);
	assert(ncalls == 1 && calls[0].kind == DONE && !calls[0].flag);
	clin_frame_worlds_begin(&f,2);
	/* The failure branch: clear + render_world off, as main.c did. */
	reset_calls();
	pre_views_result = false;
	clin_frame_pre_render(&f, cams, &render_world);
	pre_views_result = true;
	assert(ncalls == 2 && calls[1].kind == PRE_CLEAR && calls[1].w == &gstate.world
		   && !render_world);
	/* One view: world_pre_render(&gstate.world, cam0, cam0->view). */
	clin_frame_worlds_begin(&f, 1);
	reset_calls();
	render_world = true;
	clin_frame_pre_render(&f, cams, &render_world);
	assert(ncalls == 1 && calls[0].kind == PRE && calls[0].w == &gstate.world
		   && calls[0].cam[0] == &cam0);

	reset_calls();
	clin_frame_pre_render_clear(&f);
	clin_frame_lighting(&f);
	clin_frame_lod(&f, cams, 1);
	clin_frame_lod(&f, cams, 2);
	assert(ncalls == 4);
	assert(calls[0].kind == PRE_CLEAR && calls[0].w == &gstate.world);
	assert(calls[1].kind == LIGHT && calls[1].w == &gstate.world);
	assert(calls[2].kind == LOD && calls[2].w == &gstate.world);
	assert(calls[3].kind == LOD_VIEWS && calls[3].w == &gstate.world
		   && calls[3].n == 2 && calls[3].cam[0] == &cam0 && calls[3].cam[1] == &cam1);

	reset_calls();
	w_coord_t centers[2][2] = {{7, -3}, {0, 0}};
	int pending = -1;
	gstate.world.build_frame = 41;
	size_t left = clin_frame_build(&f, CHUNK_MESHER_QLENGTH, centers, 5, NULL, &pending);
	assert(ncalls == 2 && calls[0].kind == RING && calls[0].w == &gstate.world
		   && calls[0].cx == 7 && calls[0].cz == -3 && calls[0].radius == 5);
	assert(calls[1].kind == BUILD && calls[1].w == &gstate.world
		   && calls[1].n == CHUNK_MESHER_QLENGTH
		   && calls[1].flag == world_band2_allow_far(3, 41));
	assert(left == 5 && pending == 3);

	reset_calls();
	clin_frame_render_completed(&f, true);
	clin_frame_render_completed(&f, false);
	assert(ncalls == 2 && calls[0].kind == DONE && calls[0].w == &gstate.world
		   && calls[0].flag && !calls[1].flag);
	puts("PASS T1 flag OFF: every frame pass is the pre-B2 main.c call on "
		 "gstate.world (pre-render single/union/failure, clear, lighting, LOD, "
		 "ring+build with the full budget, hand-off)");
}

static void test_two_worlds(void) {
	flag(true);
	world_reset(0, 100);           /* guest's overworld */
	world_reset(1, 200);           /* player 0 in the Nether (primary) */
	assert(clin_world_payload_bind_view(0, 1));
	struct clin_world_payload p0, p1;
	assert(clin_world_payload_of_slot(0, &p0) && clin_world_payload_of_slot(1, &p1));
	struct clin_frame_worlds f;
	clin_frame_worlds_begin(&f, 2);
	assert(f.n_live == 2 && f.live[0] == p1.terrain && f.live[1] == p0.terrain
		   && "player 0's world first");
	assert(f.n_storage == 2 && !f.shared);
	assert(f.view_terrain[0] == p1.terrain && f.view_terrain[1] == p0.terrain);

	reset_calls();
	bool render_world = true;
	clin_frame_pre_render(&f, cams, &render_world);
	assert(ncalls == 2 && render_world);
	assert(calls[0].kind == PRE && calls[0].w == p1.terrain && calls[0].cam[0] == &cam0);
	assert(calls[1].kind == PRE && calls[1].w == p0.terrain && calls[1].cam[0] == &cam1
		   && "each world sees only its own view's camera");

	reset_calls();
	clin_frame_lighting(&f);
	clin_frame_lod(&f, cams, 2);
	assert(ncalls == 4);
	assert(calls[0].kind == LIGHT && calls[0].w == p1.terrain);
	assert(calls[1].kind == LIGHT && calls[1].w == p0.terrain);
	assert(calls[2].kind == LOD_VIEWS && calls[2].w == p1.terrain && calls[2].n == 1
		   && calls[2].cam[0] == &cam0);
	assert(calls[3].kind == LOD_VIEWS && calls[3].w == p0.terrain && calls[3].n == 1
		   && calls[3].cam[0] == &cam1);

	/* MED-3 budget: the viewed guest world has near-ring meshing pending,
	 * so half the tokens are reserved for it; player 0's unused share rolls
	 * over; the second builder never gets more than tokens - consumed. */
	reset_calls();
	w_coord_t centers[2][2] = {{1, 1}, {-9, 4}};
	size_t left = clin_frame_build(&f, CHUNK_MESHER_QLENGTH, centers, 4, NULL, NULL);
	assert(ncalls == 4);
	assert(calls[0].kind == RING && calls[0].w == p1.terrain && calls[0].cx == 1);
	assert(calls[1].kind == RING && calls[1].w == p0.terrain && calls[1].cx == -9);
	assert(calls[2].kind == BUILD && calls[2].w == p1.terrain
		   && calls[2].n == CHUNK_MESHER_QLENGTH / 2
		   && "player 0's world builds with the unreserved half");
	assert(calls[3].kind == BUILD && calls[3].w == p0.terrain
		   && calls[3].n == CHUNK_MESHER_QLENGTH / 2 + left
		   && calls[3].n <= CHUNK_MESHER_QLENGTH
		   && "the guest world gets its reserve plus player 0's leftover");
	/* Nothing pending in the guest world: no reserve, the old policy. */
	no_pending_world = p0.terrain;
	reset_calls();
	left = clin_frame_build(&f, CHUNK_MESHER_QLENGTH, centers, 4, NULL, NULL);
	no_pending_world = NULL;
	assert(ncalls == 4 && calls[2].n == CHUNK_MESHER_QLENGTH && left == 5
		   && calls[3].n == 5);

	reset_calls();
	clin_frame_render_completed(&f, true);
	assert(ncalls == 2 && calls[0].kind == DONE && calls[1].kind == DONE);
	for(unsigned i = 0; i < 2; i++)
		assert(calls[i].flag && "both drawn worlds pin this frame");
	/* While the host loads a new world, the guest's independent view remains
	 * drawable and the hidden host storage still receives a false hand-off. */
	clin_frame_worlds_begin(&f, 2);
	f.draw_view[0] = false;
	reset_calls();
	render_world = true;
	clin_frame_pre_render(&f, cams, &render_world);
	assert(render_world && ncalls == 2 && calls[0].kind == PRE_CLEAR
		   && calls[0].w == p1.terrain && calls[1].kind == PRE
		   && calls[1].w == p0.terrain && calls[1].cam[0] == &cam1);
	reset_calls();
	clin_frame_render_completed(&f, true);
	assert(ncalls == 2);
	for(unsigned i = 0; i < 2; i++)
		assert(calls[i].flag == (calls[i].w == p0.terrain));
	/* Guest COMMIT: player 0 keeps drawing while view 1's storage drains.
	 * The hidden landing world still gets build tokens above. */
	clin_frame_worlds_begin(&f,2);
	f.draw_view[1]=false;
	reset_calls();
	clin_frame_pre_render(&f,cams,&render_world);
	assert(ncalls==2&&calls[0].kind==PRE_CLEAR&&calls[0].w==p0.terrain
		&&calls[1].kind==PRE&&calls[1].w==p1.terrain
		&&calls[1].cam[0]==&cam0);
	reset_calls();
	clin_frame_render_completed(&f,true);
	assert(ncalls==2);
	for(unsigned i=0;i<2;i++)
		assert(calls[i].flag==(calls[i].w==p1.terrain));
	clin_frame_worlds_begin(&f,2);
	f.draw_view[0]=f.draw_view[1]=false;
	reset_calls();
	clin_frame_pre_render(&f,cams,&render_world);
	assert(ncalls==2&&calls[0].kind==PRE_CLEAR&&calls[0].w==p0.terrain
		&&calls[1].kind==PRE_CLEAR&&calls[1].w==p1.terrain
		&&"simultaneous transfers clear both undrawn scratch lists");
	reset_calls();
	clin_frame_render_completed(&f,true);
	assert(ncalls==2&&!calls[0].flag&&!calls[1].flag);

	/* Next frame: only view 0 drawn (the guest viewport collapsed). The
	 * undrawn world still gets its hand-off, with false, so its pin drains. */
	clin_frame_worlds_begin(&f, 1);
	reset_calls();
	render_world = true;
	clin_frame_pre_render(&f, cams, &render_world);
	assert(ncalls == 2 && calls[0].kind == PRE_CLEAR
		   && calls[0].w == p0.terrain && calls[1].kind == PRE
		   && calls[1].w == p1.terrain);
	reset_calls();
	clin_frame_render_completed(&f, true);
	assert(ncalls == 2);
	for(unsigned i = 0; i < 2; i++)
		assert(calls[i].flag == (calls[i].w == p1.terrain)
			   && "drawn -> true, undrawn -> false (drains last frame's pin)");
	puts("PASS T2 flag ON: two worlds pre-rendered/lit/LOD'ed with their own "
		 "cameras, token budget split, every storage handed off each frame");
}

/* MED-3 (unviewed) + HIGH-1 (fence ordering). */
static void test_unviewed_and_fence(void) {
	flag(true);
	world_reset(0, 100);
	world_reset(1, 200); /* player 0 (view 0) in slot 1; slot 0 unviewed */
	struct clin_world_payload p0, p1;
	assert(clin_world_payload_of_slot(0, &p0) && clin_world_payload_of_slot(1, &p1));
	struct clin_frame_worlds f;
	clin_frame_worlds_begin(&f, 1);
	assert(f.n_live == 2 && f.view_slot[0] == 1);
	reset_calls();
	w_coord_t centers[2][2] = {{0, 0}, {0, 0}};
	clin_frame_build(&f, CHUNK_MESHER_QLENGTH, centers, 4, NULL, NULL);
	assert(ncalls == 2 && calls[0].kind == RING && calls[0].w == p1.terrain
		   && calls[1].kind == BUILD && calls[1].w == p1.terrain
		   && calls[1].n == CHUNK_MESHER_QLENGTH
		   && "an unviewed live world is neither centred nor meshed");

	/* HIGH-1: the teardown fence waits for the GPU BEFORE it unpins the
	 * second storage's frame references. */
	reset_calls();
	clin_frame_teardown_extra();
	assert(ncalls == 3 && calls[0].kind == WAIT
		   && calls[1].kind == PRE_CLEAR && calls[1].w == p1.terrain
		   && calls[2].kind == DONE && calls[2].w == p1.terrain && !calls[2].flag
		   && "gfx_wait_idle precedes the slot-1 hand-off");
	flag(false);
	reset_calls();
	clin_frame_teardown_extra();
	assert(ncalls == 0 && "flag OFF: no fence, no call");
	puts("PASS T4 unviewed world not meshed; teardown fence waits for the GPU "
		 "before releasing slot-1 frame references");
}

static void test_view_env(void) {
	/* OFF: main.c's daytime and the gstate weather, unchanged. */
	flag(false);
	world_reset(0, 0);
	gstate.weather_rain_strength = 0.75F;
	gstate.weather_thunder_strength = 0.25F;
	struct clin_view_world s;
	struct clin_view_env e;
	assert(clin_world_view_enter(0, &s));
	clin_frame_view_env(&s, 0.3125F, false, time_get(), 0.0F, &e);
	clin_world_view_leave(&s);
	assert(e.daytime == 0.3125F && e.sky && e.rain_strength == 0.75F
		   && e.thunder_strength == 0.25F);

	/* ON: per-view clocks, Nether without sky/weather. */
	flag(true);
	world_reset(0, 100);
	world_reset(1, 200);
	assert(clin_world_payload_bind_view(0, 1));
	ptime_t now = time_get();
	clin_process(&(struct client_rpc) {.type = CRPC_TIME_SET, .world_index = 0,
		.world_generation = 100, .payload.time_set = 6000});
	clin_process(&(struct client_rpc) {.type = CRPC_TIME_SET, .world_index = 1,
		.world_generation = 200, .payload.time_set = 18000});
	clin_process(&(struct client_rpc) {.type = CRPC_WEATHER_SET, .world_index = 1,
		.world_generation = 200,
		.payload.weather_set = {.raining = true, .rain_strength = 1.0F}});
	clin_process(&(struct client_rpc) {.type = CRPC_WEATHER_SET, .world_index = 0,
		.world_generation = 100,
		.payload.weather_set = {.raining = true, .rain_strength = 0.5F}});
	struct clin_world_payload_clock k0, k1;
	assert(clin_world_payload_clock(0, &k0) && clin_world_payload_clock(1, &k1));

	assert(clin_world_view_enter(0, &s)); /* Nether */
	clin_frame_view_env(&s, 0.9F, false, now, 0.0F, &e);
	clin_world_view_leave(&s);
	assert(!e.sky && e.rain_strength == 0.0F && e.dimension == WORLD_DIM_NETHER);
	assert(e.daytime == clin_frame_daytime(k1.world_time, k1.world_time_start, now)
		   && "the Nether view reads the Nether clock, not main.c's");

	assert(clin_world_view_enter(1, &s)); /* guest overworld */
	clin_frame_view_env(&s, 0.9F, false, now, 0.0F, &e);
	clin_world_view_leave(&s);
	assert(e.sky && e.rain_strength == 0.5F);
	assert(e.daytime == clin_frame_daytime(k0.world_time, k0.world_time_start, now));
	assert(fabsf(e.daytime - 0.25F) < 0.01F && "6000 ticks = quarter day");

	assert(clin_world_view_enter(1, &s));
	clin_frame_view_env(&s, 0.9F, true, now, 0.123F, &e);
	clin_world_view_leave(&s);
	assert(e.daytime == 0.123F && "paused: the view keeps its last daytime");
	puts("PASS T3 per-view env: OFF = main.c daytime + gstate weather; ON = own "
		 "clock per view, Nether no sky/weather, paused keeps daytime");
}

/* Frozen from saved original source__daytime.c SHA-256
 * 6C52F8A0D66BC32FA36783CE6AC595D6EAB3CBFF24B651D4760320727F541653.
 * This oracle keeps the pre-change arithmetic independent of the new helpers. */
static float original_sun(float time) {
	return (gstate.world.dimension == WORLD_DIM_OVERWORLD)
		? daytime_sun_from_angle(daytime_celestial_angle(time)) : 0.0F;
}

static float original_brightness(float time) {
	if(gstate.world.dimension != WORLD_DIM_OVERWORLD)
		return 0.0F;
	return glm_clamp(mh_cos(daytime_celestial_angle(time) * 3.1415927F * 2.0F) * 2.0F
						 + 0.5F, 0.0F, 1.0F);
}

static void original_sky_colors(float time, vec3 top_plane, vec3 bottom_plane,
								 vec3 atmosphere) {
	if(gstate.world.dimension == WORLD_DIM_OVERWORLD) {
		float brightness_mul = original_brightness(time);
		vec3 world_sky_color = {
			0.6222222F - (0.7F / 3.0F) * 0.05F,
			0.5F + (0.7F / 3.0F) * 0.1F,
			1.0F,
		};
		hsv2rgb(world_sky_color + 0, world_sky_color + 1, world_sky_color + 2);
		glm_vec3_scale(world_sky_color, brightness_mul, world_sky_color);
		vec3 fog_color = {
			0.7529412F * brightness_mul * 0.94F + 0.06F,
			0.8470588F * brightness_mul * 0.94F + 0.06F,
			1.0F * brightness_mul * 0.91F + 0.09F,
		};
		vec3 atmosphere_color;
		glm_vec3_lerp(fog_color, world_sky_color, 0.29F, atmosphere_color);
		glm_vec3_scale(atmosphere_color,
					   powf(0.8F, (1.0F - brightness_mul) * 11.0F),
					   atmosphere_color);
		vec3 bottom_plane_color = {0.04F, 0.04F, 0.1F};
		glm_vec3_muladd(world_sky_color, (vec3) {0.2F, 0.2F, 0.6F},
						bottom_plane_color);
		glm_vec3_scale(atmosphere_color, 255.0F, atmosphere);
		glm_vec3_scale(world_sky_color, 255.0F, top_plane);
		glm_vec3_scale(bottom_plane_color, 255.0F, bottom_plane);
	} else {
		vec3 const_color = {0.2F, 0.03F, 0.03F};
		glm_vec3_scale(const_color, 255.0F, atmosphere);
		glm_vec3_scale(const_color, 255.0F, top_plane);
		glm_vec3_scale(const_color, 255.0F, bottom_plane);
	}
}

static void test_view_atmosphere_owner(void) {
	vec3 top, bottom, fog, old_top, old_bottom, old_fog;
	const int dims[] = {WORLD_DIM_OVERWORLD, WORLD_DIM_NETHER};
	for(int tick = 0; tick < 24000; tick++) {
		float time = (float)tick / 24000.0F;
		for(unsigned global = 0; global < 2; global++) {
			gstate.world.dimension = dims[global];
			for(unsigned owner = 0; owner < 2; owner++) {
				/* Both pane orders and both global/view dimension pairings. */
				daytime_sky_colors_dimension(time, dims[owner], top, bottom, fog);
				gstate.world.dimension = dims[owner];
				original_sky_colors(time, old_top, old_bottom, old_fog);
				assert(memcmp(top, old_top, sizeof top) == 0
					   && memcmp(bottom, old_bottom, sizeof bottom) == 0
					   && memcmp(fog, old_fog, sizeof fog) == 0);
				assert(daytime_brightness_dimension(time, dims[owner])
					   == original_brightness(time));
				assert(daytime_light_sun_dimension(time, dims[owner])
					   == original_sun(time));
				gstate.world.dimension = dims[global];
			}
		}
	}
	gstate.world.dimension = WORLD_DIM_OVERWORLD;
}

/* A hidden guest storage still builds while its transfer pane is retained.
 * Its previous frame's intrusive render list must be discarded before that
 * builder walks it; the GPU handoff separately owns the old reference. */
static void test_masked_guest_clears_real_render_list(void) {
	struct world host, guest;
	world_create(&host);
	world_create(&guest);
	struct chunk* c = chunk_alloc();
	assert(c);
	chunk_init(c, &guest, 0, 0, 0);
	chunk_ref(c);
	ilist_chunks_push_back(guest.render, c);
	/* The prior drawn frame moves the token to gpu_busy, but intentionally
	 * leaves the render scratch chain until the next pre-render/clear. */
	__real_world_render_completed(&guest, true);
	assert(!ilist_chunks_empty_p(guest.render));
	assert(!ilist_chunks2_empty_p(guest.gpu_busy_chunks));
	struct clin_frame_worlds f = {0};
	f.view_count = 2;
	f.n_live = f.n_storage = 2;
	f.live[0] = f.storage[0] = f.view_terrain[0] = &host;
	f.live[1] = f.storage[1] = f.view_terrain[1] = &guest;
	f.draw_view[0] = true;
	f.draw_view[1] = false;
	real_masked_list = true;
	reset_calls();
	bool render_world = true;
	clin_frame_pre_render(&f, cams, &render_world);
	assert(render_world && f.drew[0] && !f.drew[1]);
	assert(ilist_chunks_empty_p(guest.render)
		   && "masked guest must not build from the previous frame's render list");
	w_coord_t centers[2][2] = {{0, 0}, {0, 0}};
	(void)clin_frame_build(&f, 2, centers, 2, NULL, NULL);
	clin_frame_render_completed(&f, true);
	assert(ilist_chunks2_empty_p(guest.gpu_busy_chunks));
	real_masked_list = false;
	world_destroy(&guest);
	world_destroy(&host);
	puts("PASS masked guest: real intrusive render list cleared before real build; prior GPU token drains");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	assert(svin_init());
	assert(clin_init());
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	reg = clin_world_contexts();
	test_off_identity();
	test_two_worlds();
	test_masked_guest_clears_real_render_list();
	test_unviewed_and_fence();
	test_view_env();
	test_view_atmosphere_owner();
	test_guest_pose_during_host_transfer();
	test_owner_loading_policy();
	flag(false);
	puts("PASS test_b2_frame_worlds");
	return 0;
}
