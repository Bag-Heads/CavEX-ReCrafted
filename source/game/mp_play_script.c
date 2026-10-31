/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2. See mp_play_script.h.
*/

#include "mp_play_script.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../block/blocks.h"

#include "../entity/entity.h"
#include "../item/inventory.h"
#include "../item/window_container.h"
#include "../network/server_interface.h" /* HEALTH_PER_HEART / MAX_PLAYER_HEALTH */
#include "../network/server_remote.h"
#include "../platform/input.h"
#include "../world.h"
#include "autopilot.h"
#include "camera.h"
#include "chat_login_script.h"
#include "game_state.h"
#include "gui/screen.h"
#include "gui/screen_load_world.h"
#include "mp_play_map.h"

/* The one constant mp_play_map.h restates instead of including
 * chat_login_script.h for (see that header's note). Pinned HERE, the one
 * translation unit that legitimately sees both -- and pinned at COMPILE time,
 * not with an assert, because an assert compiles out of the shipping .dol
 * exactly where a silent mismatch would truncate a request. */
typedef char mpp_actions_cap_matches_login
	[(MPP_MAP_MAX_ACTIONS == CHAT_LOGIN_PLAY_ACTIONS) ? 1 : -1];

/* The same treatment for the health scale (push-0921/B11). mp_play_map.h is a
 * PURE header and may not include server_interface.h -- that one drags in
 * world.h and m-lib -- so it restates the 16 and this translation unit, which
 * legitimately sees both, pins them together at COMPILE time. MAX_PLAYER_HEALTH
 * is checked too: it is what the wire converter clamps to, so a ten-heart
 * player must be exactly the Beta maximum on the other side of the mapping. */
typedef char mpp_health_scale_matches_wire
	[(MPP_MAP_HEALTH_PER_HEART == HEALTH_PER_HEART) ? 1 : -1];
typedef char mpp_health_max_matches_wire
	[(MAX_PLAYER_HEALTH == MPP_MAP_BETA_MAX_HEALTH * (HEALTH_PER_HEART / 2))
		 ? 1
		 : -1];

/* Beta's player eye height. struct entity::pos[1] is Beta's posY (the EYE
 * reference) -- source/network/server_remote_protocol.c's movement encode
 * spells this out: it sends `y = pos - 1.62` as the bounding-box minY and the
 * raw pos as `stance`. The core, and the fixture receipts it is matched
 * against, both work in FEET: mpp_floor(o->y) - 1 is the cell under the feet,
 * o->ty == mpp_floor(o->y) is the neighbour at feet level, and mc173's own
 * `pos` receipt is the wire y. So the observation subtracts it here, once.
 * The same literal is already spelled out in entity_local_player.c's
 * EYE_HEIGHT, autopilot.c's `feet` and the encode above. */
#define MPP_EYE_HEIGHT 1.62F

/* Once the run is done, the marker queue gets this long to drain before
 * profile_done goes out regardless. The `ok` / `fail` marker is the run's own
 * last word and should reach the wire first -- but "a stuck run always
 * reports" outranks that, and chat_login's own cl_fail_at sets the precedent
 * (marker best-effort, profile_done unconditional). */
#define MPP_DONE_DRAIN_MS 5000u

static struct {
	bool armed;
	struct mp_play_hooks hooks;
	struct mpp_state core;
	char username[32]; /* the run nonce; escaped worst case fits the builder */
	/* A login has been started at least once. Before that the login machine is
	 * unarmed and its phase would read IDLE for a reason the core must not
	 * confuse with "running". */
	bool login_started;
	/* A restart that REFUSED. Without this the machine would still read PARKED
	 * from the FIRST login and the core would credit generation 2 with an
	 * instant login it never made. */
	bool login_restart_failed;
	bool done_sent;
	uint32_t done_at_ms;
	bool done_seen;
	unsigned refused_actions;
	/* The Join screen needs one DOWN edge to enter the selected saved row,
	 * followed by RIGHT while that same abstract press remains held. Keeping
	 * this in the driver avoids linking its engine-pool TU to the MP-only
	 * screen implementation. A release or any other route resets it. */
	bool mps_route_advanced;
} s;

/* Two scratch buffers kept out of the frame: struct server_remote_play_obs is
 * ~560 bytes (the three observation rings) and the marker line is another, and
 * this runs on the main loop every frame on a console whose stacks are
 * fixed-size (CODESTYLE 1.2). Neither is state -- both are fully written
 * before they are read, every frame.
 *
 * The rings live in their OWN struct, not in server_remote_status
 * (push-0921/B18, review B17 H2): that struct is a STACK LOCAL in main.c's
 * heartbeat, in main()'s own frame, in mp_session and on every frame of the
 * connect screen -- six readers that never look at the rings, each memcpy'ing
 * 560 B of them under the remote lock. A third buffer, s_status, sat here
 * until push-0921/B19 item 0: once the rings moved out, this file read nothing
 * from it, so it and its hook are gone. */
static struct server_remote_play_obs s_obs;
static char s_line[MPP_MARKER_BODY + 640];

/* ---- lifecycle ----------------------------------------------------------- */

/* The unattended pad owns menu navigation from pause opening through the
 * second Join. A live mouse/IR may still hover a different row, where the
 * approved activation rule correctly consumes a directional click as
 * focus-only. Answer "no pointer" for this profile's scripted menu leg,
 * without changing the native cursor mode or other profiles. */
static bool mpp_pointer_script(float* x, float* y, float* angle,
							 bool* available) {
	(void)x; (void)y; (void)angle;
	if(!mpp_pointer_script_owns(s.armed, &s.core))
		return false;
	*available = false;
	return true;
}

void mp_play_set_short_play(uint32_t ms) {
	s.core.short_play_ms = ms;
}

void mp_play_reset(void) {
	input_pointer_set_script(NULL);
	memset(&s, 0, sizeof(s));
	memset(&s_obs, 0, sizeof(s_obs));
	s_line[0] = 0;
}

void mp_play_arm(const struct mp_play_hooks* hooks, const char* username,
				 const char* run_id) {
	mp_play_reset();
	if(!hooks)
		return;
	s.hooks = *hooks;
	snprintf(s.username, sizeof(s.username), "%s", username ? username : "");
	/* NOT stored, and that is the point: the run identity belongs to the
	 * TRANSPORT, which stamps every emitted line with it
	 * (hw_telemetry_emit_observer, source/platform/hw_telemetry.c:2743-2747).
	 * A copy here could only ever be a second, divergible source of truth --
	 * and until 2026-09-21 it was literally written a second time into every
	 * marker. The parameter stays so main.c's call site keeps saying which run
	 * it armed, and so a future reader looks here rather than re-adding it. */
	(void)run_id;
	s.armed = true;
	input_pointer_set_script(mpp_pointer_script);
}

bool mp_play_armed(void) {
	return s.armed;
}

enum mpp_phase mp_play_phase_now(void) {
	return s.core.phase;
}

uint32_t mp_play_seq_now(void) {
	return s.core.seq;
}

unsigned mp_play_markers_pending(void) {
	return s.core.q_count;
}

bool mp_play_done_now(void) {
	return mpp_done(&s.core);
}

unsigned mp_play_refused_actions(void) {
	return s.refused_actions;
}

/* ---- pure mappings ------------------------------------------------------- */

enum mpp_screen mp_play_screen_of(const struct screen* sc) {
	if(!sc)
		return MPP_SCREEN_OTHER;
	if(sc == &screen_ingame)
		return MPP_SCREEN_INGAME;
	if(sc == &screen_inventory)
		return MPP_SCREEN_INVENTORY;
	if(sc == &screen_pause)
		return MPP_SCREEN_PAUSE;
	if(sc == &screen_title)
		return MPP_SCREEN_TITLE;
	/* Through the POINTERS, never the _impl objects main.c compares against:
	 * screen_multiplayer_setup.c / _connect.c are CAVEX_MP_SOURCES, a pool
	 * this engine-pool file does not link, so naming those symbols is an
	 * undefined reference in every non-MP target. The pointers themselves live
	 * in screen.c (engine core) and are NULL until the MP pool registers them,
	 * which is exactly the indirection screen.h provides for this -- and a
	 * build with no MP screens then simply never reports them. */
	if(screen_multiplayer_setup && sc == screen_multiplayer_setup)
		return MPP_SCREEN_MP_SETUP;
	if(screen_multiplayer_connect && sc == screen_multiplayer_connect)
		return MPP_SCREEN_MP_CONNECT;
	if(sc == &screen_load_world)
		return MPP_SCREEN_LOAD_WORLD;
	if(sc == &screen_chat)
		return MPP_SCREEN_CHAT;
	if(sc == &screen_death)
		return MPP_SCREEN_DEATH;
	return MPP_SCREEN_OTHER;
}

void mp_play_login_ctx(const struct mp_play_ctx* in, struct chat_login_ctx* out) {
	const struct entity* body;
	if(!in || !out)
		return;
	memset(out, 0, sizeof(*out));
	body = in->local_player ? gstate.players[0].local_player : NULL;
	out->remote_session = in->remote_session;
	out->world_loaded = in->world_loaded;
	out->world_switch_pending = in->world_switch_pending;
	out->local_player = in->local_player;
	out->screen_is_ingame = in->current_screen == &screen_ingame;
	out->screen_is_chat = in->current_screen == &screen_chat;
	switch(mp_play_screen_of(in->current_screen)) {
		case MPP_SCREEN_INGAME: out->screen_id = CHAT_LOGIN_SCREEN_INGAME; break;
		case MPP_SCREEN_CHAT: out->screen_id = CHAT_LOGIN_SCREEN_CHAT; break;
		case MPP_SCREEN_LOAD_WORLD:
			out->screen_id = CHAT_LOGIN_SCREEN_LOAD_WORLD;
			break;
		case MPP_SCREEN_TITLE: out->screen_id = CHAT_LOGIN_SCREEN_TITLE; break;
		case MPP_SCREEN_MP_CONNECT:
			out->screen_id = CHAT_LOGIN_SCREEN_MP_CONNECT;
			break;
		case MPP_SCREEN_PAUSE: out->screen_id = CHAT_LOGIN_SCREEN_PAUSE; break;
		case MPP_SCREEN_DEATH: out->screen_id = CHAT_LOGIN_SCREEN_DEATH; break;
		default: out->screen_id = CHAT_LOGIN_SCREEN_OTHER; break;
	}
	out->recovery_modal = in->recovery_modal;
	/* p0.session's position, in the login driver's own terms -- it publishes
	 * the raw body position, so this does NOT fold out the eye height. */
	out->x = body ? body->pos[0] : 0.0F;
	out->y = body ? body->pos[1] : 0.0F;
	out->z = body ? body->pos[2] : 0.0F;
	out->epoch = in->epoch;
	out->now_ms = in->now_ms;
	out->ring = in->ring;
}

/* ---- observation --------------------------------------------------------- */

static struct mpp_item mpp_item_of(const struct item_data* it) {
	struct mpp_item out;
	memset(&out, 0, sizeof(out));
	/* The client spells "empty" id == 0 and the core spells it item == 0, so
	 * this is the one place the two conventions meet (the set-slot ring's own
	 * -1 convention is mp_play_map.h's mpp_map_item_from_set_slot). */
	if(!it || it->id == 0)
		return out;
	out.item = (int16_t)it->id;
	out.count = it->count;
	out.damage = (int16_t)it->durability;
	return out;
}

static void mpp_fill_session(struct mpp_obs* o, const struct mp_play_ctx* ctx) {
	o->now_ms = ctx->now_ms;
	o->remote_session = ctx->remote_session;
	/* A world with a switch PENDING is not the world the driver is playing
	 * in -- it is the one being torn down for the next one -- so it does not
	 * count as loaded for the readiness gate. */
	o->world_loaded = ctx->world_loaded && !ctx->world_switch_pending;
	o->screen = mp_play_screen_of(ctx->current_screen);
	/* Cumulative for the life of the process and never reset, which is what
	 * makes "zero throughout, including the rejoin" a provable claim rather
	 * than a sample (Astra gate 1 Lane B item 2). */
	o->timeout_admits = lworld_ready_timeout_admits();
	/* Not observed -> false. See mp_play_hooks::body_ticking: the production
	 * predicate is a file static this file may not reach and may not copy. */
	o->body_ticking = ctx->local_player && s.hooks.body_ticking
		&& s.hooks.body_ticking(s.hooks.user);
	/* The only fault source the driver has: the login machine's two run-ending
	 * rules (a real pad on its channel, the controller-recovery modal) latch
	 * there while it is parked, and this is where they are read. */
	o->fault = chat_login_play_fault(NULL);
}

static void mpp_fill_body(struct mpp_obs* o, const struct mp_play_ctx* ctx) {
	const struct entity* p
		= ctx->local_player ? gstate.players[0].local_player : NULL;
	o->dimension = (int)gstate.world.dimension;
	if(!p)
		return;
	o->x = p->pos[0];
	o->y = p->pos[1] - MPP_EYE_HEIGHT; /* eye -> feet; see the #define */
	o->z = p->pos[2];
	/* BETA HIT POINTS (0..20), not CaveWii's internal 0..160 -- see
	 * mpp_map_health_to_beta for the two scales and why this one wins: the
	 * fixture's `ev=health h=<n>` receipt, the contract's "no health h<=0
	 * receipt" rule and Beta itself all speak half-hearts, and until
	 * push-0921/B11 the `ready` marker published the internal number beside
	 * them (run pc_mp_play_20260921_143758: marker 152, receipt h=19, same body,
	 * same instant). The core's death latch is unaffected -- the conversion
	 * reports 0 for exactly the inputs that were already <= 0. */
	o->health = mpp_map_health_to_beta(p->health);
	/* The predicate itself is PURE and lives in mp_play_map.h, where
	 * tests/test_mp_play_map.c can drive it with the velocities production
	 * really produces. It is the single observation the whole build leg turns
	 * on, and it is either right for every reading or wrong for all of them. */
	o->at_rest = mpp_map_body_at_rest(p->on_ground, p->vel[0], p->vel[1],
									  p->vel[2]);
	/* WHERE FORWARD REALLY GOES this frame, read from the body's own yaw --
	 * NOT from the yaw the driver asked for. e->orient[0] is the exact angle
	 * the movement code uses (entity_local_player.c:644-645), and it is what
	 * the aim lock's request has ACTUALLY become by now: autopilot_aim_set_deg
	 * -> gstate.autopilot.yaw -> camera_attach's `yaw` -> c->rx ->
	 * e->orient[0] (camera.c:418). Reporting the request instead would defeat
	 * the whole point of the field -- the core uses it to refuse to nudge when
	 * the aim has not actually arrived. */
	mpp_map_forward_of(p->orient[0], &o->fwd_x, &o->fwd_z);
	/* IN A FLUID: the flag the client already keeps for player 0
	 * (client_session.h:135), recomputed every frame in main.c:4084-4092 from
	 * the block at the CAMERA cell. CALLED, never re-derived -- the physics'
	 * own body test (entity_local_player.c:67-75 test_in_water, through
	 * entity_intersection) is a file static with no header, so re-deriving it
	 * here would be the second copy the F8b ruling closed.
	 *
	 * STATED LIMIT, because it is not the same predicate: this is the HEAD
	 * cell, so a body standing in one block of water reads false. That case
	 * still rests and still digs the bottom block, which is harmless; the case
	 * the core added `in_water` for -- floating/swimming, where the body never
	 * settles and the cell under the feet is not what gets dug -- is submerged
	 * and does read true. Exporting the physics predicate would make this
	 * exact; that is a production edit this lane does not own. */
	o->in_water = gstate.players[0].in_water;
}

static void mpp_fill_target(struct mpp_obs* o, const struct mp_play_ctx* ctx) {
	const struct camera_ray_result* hit = &gstate.players[0].camera_hit;
	int face;
	if(ctx->local_player
	   && (s.core.phase == MPP_PH_BUILD_SURVEY
		   || s.core.phase == MPP_PH_BUILD_DIG)) {
		/* BUILD_DIG must keep checking the ORIGINAL landing cell. Once the
		 * block above it breaks, the body's feet can descend and a fresh
		 * floor(feet)-2 would name the wrong cell. Other phases survey the
		 * current column. Missing chunks fail closed: world_get_block alone
		 * supplies a collision fallback for unknown terrain. */
		o->support_x = s.core.phase == MPP_PH_BUILD_DIG
			? s.core.dig_x : mpp_floor(o->x);
		o->support_y = s.core.phase == MPP_PH_BUILD_DIG
			? s.core.dig_y - 1 : mpp_floor(o->y) - 2;
		o->support_z = s.core.phase == MPP_PH_BUILD_DIG
			? s.core.dig_z : mpp_floor(o->z);
		o->support_known = world_find_chunk(&gstate.world, o->support_x,
			o->support_y, o->support_z) != NULL;
		if(o->support_known) {
			struct block_data block = world_get_block(&gstate.world,
				o->support_x, o->support_y, o->support_z);
			struct block* kind = blocks[block.type];
			if(kind && kind->getBoundingBox) {
				struct block_info info = {.block = &block,
					.x = o->support_x, .y = o->support_y,
					.z = o->support_z};
				size_t count = kind->getBoundingBox(&info, true, NULL);
				struct AABB box;
				if(count == 1u)
					kind->getBoundingBox(&info, true, &box);
				o->support_safe = mpp_map_support_full_cube(true, block.type,
					count, count == 1u ? &box : NULL);
			}
		}
	}
	if(ctx->local_player)
		o->under_feet_type = world_get_block(&gstate.world, mpp_floor(o->x),
											 mpp_floor(o->y) - 1,
											 mpp_floor(o->z))
								 .type;
	/* The crosshair the client ALREADY computed this frame for the block
	 * highlight and the digging path (main.c's camera_ray_pick into
	 * players[0].camera_hit). No second raycast: a driver that cast its own
	 * ray could aim at a cell the game is not highlighting, and then count a
	 * dig the player never made. An ENTITY pick is not a block target. */
	if(!hit->hit || hit->entity_hit)
		return;
	face = mpp_map_face_of_side(hit->side);
	if(face < 0)
		return; /* unmapped side: report NO target rather than a wrong face */
	o->target_valid = true;
	o->tx = hit->x;
	o->ty = hit->y;
	o->tz = hit->z;
	o->tface = face;
	/* WHAT the crosshair is resting on (push-0921/B12), for the core's plant
	 * clearing. Same source and same field as under_feet_type above -- the
	 * client's own world, world_get_block(...).type -- read at the cell
	 * camera_ray_pick already resolved this frame, so the id and the target
	 * coordinates can never describe two different cells.
	 *
	 * No mp_play_map.h helper and no test there, deliberately: there is nothing
	 * to translate. Every other id in this struct (under_feet_type, the echo
	 * types) is the same raw client block id on the same scale, and the core
	 * compares it against raw ids (mpp_is_plant, mpp_hand_diggable). A helper
	 * would only be a second name for `.type`. Zero when there is no target,
	 * which is the memset at the top of mp_play_frame, and which mpp_is_plant
	 * answers false for. */
	o->target_type
		= world_get_block(&gstate.world, hit->x, hit->y, hit->z).type;
	o->target_top_clear
		= world_get_block(&gstate.world, hit->x, hit->y + 1, hit->z).type
		== BLOCK_AIR;
}

static void mpp_fill_inventory(struct mpp_obs* o) {
	struct window_container* wc
		= gstate.players[0].windows[WINDOWC_INVENTORY];
	struct inventory* inv = wc ? windowc_get_latest(wc) : NULL;
	int focus, k;
	o->inv_focus_slot = -1;
	if(!inv || !inv->items)
		return;
	for(k = 0; k < MPP_INV_SLOTS; k++) {
		int client = mpp_map_core_to_client(k);
		if(client < 0 || (size_t)client >= inv->capacity)
			continue;
		o->inv[k] = mpp_item_of(&inv->items[client]);
	}
	o->cursor = mpp_item_of(&inv->picked_item);
	o->held_hotbar = (int)inventory_get_hotbar(inv);
	/* The id of the click the client last SENT -- which is what the core
	 * matches the server's Transaction against, and which is NOT the client's
	 * counter: that names the NEXT click (mpp_map_last_click_id has the
	 * derivation and the run that proved it). */
	o->last_click_action_id = mpp_map_last_click_id(wc->next_action_id);
	focus = screen_inventory_focus_player_slot();
	if(focus >= 0)
		o->inv_focus_slot = mpp_map_proto_to_core(focus);
}

static void mpp_fill_remote(struct mpp_obs* o) {
	/* ONE gate, one hook (push-0921/B19 item 0). This used to call
	 * remote_status first and then remote_play_obs; after B18 moved the rings
	 * out of struct server_remote_status this file read nothing out of that
	 * struct, so the first call survived only as an "is there a remote
	 * provider" test -- which THIS hook answers under exactly the same
	 * condition (both hook bodies in main.c guard on `!remote ||
	 * !remote->impl`). Collapsing them gives the frame one mutex acquisition
	 * instead of two and stops copying 696 B nobody read.
	 *
	 * No remote provider: NOTHING is confirmed. `obs` is memset at the top of
	 * every frame (mp_play_frame), so returning here leaves the rings empty,
	 * watch_echo.serial 0 and install_gen 0 -- the core then waits in
	 * MPP_PH_READY and fails with "world never became ready" on its own
	 * budget. It never counts a dig or a place it could not see the server
	 * confirm. */
	if(!s.hooks.remote_play_obs
	   || !s.hooks.remote_play_obs(s.hooks.user, &s_obs))
		return;
	mpp_map_copy_echoes(&s_obs, o);
	mpp_map_copy_txns(&s_obs, o);
	/* The core judges an overflowed echo ring differently when the watched-cell
	 * seam exists behind its out->watch (mp_play_core.h, BUILD_DIG). */
	o->watch_supported = s.hooks.remote_watch != NULL;
	o->install_gen = s_obs.world_install_generation;
}

static enum mpp_login mpp_login_state(void) {
	if(!s.login_started)
		return MPP_LOGIN_IDLE;
	if(s.login_restart_failed)
		return MPP_LOGIN_FAILED;
	switch(chat_login_phase_now()) {
		/* PARKED is play mode's "the login succeeded and the pad is being held
		 * open for me" -- cl_ok_tail parks instead of reporting, because this
		 * driver owns the run's verdict. */
		case CHAT_LOGIN_PARKED: return MPP_LOGIN_OK;
		case CHAT_LOGIN_FAILED: return MPP_LOGIN_FAILED;
		default: return MPP_LOGIN_RUNNING;
	}
}

static enum mpp_focus mpp_focus_now(const struct mpp_obs* o) {
	if(o->screen == MPP_SCREEN_PAUSE)
		return screen_pause_focus_is_leave() ? MPP_FOCUS_PAUSE_DISCONNECT
											 : MPP_FOCUS_OTHER;
	/* The proven core calls the whole front-end rejoin destination "setup".
	 * The approved frontend inserts the Load tab before Join; while that bridge
	 * is live, only the production observer may authorize the click that opens
	 * Join. */
	if(gstate.players[0].current_screen == &screen_select_world)
		return screen_load_focus_is_join_tab() ? MPP_FOCUS_MPS_JOIN
											 : MPP_FOCUS_OTHER;
	if(!s.hooks.menu_focus)
		return MPP_FOCUS_UNKNOWN; /* not observed; see the hook's comment */
	return s.hooks.menu_focus(s.hooks.user);
}

static enum mpp_load_focus mpp_load_focus_from_screen(int focus) {
	switch(focus) {
		case SCREEN_SWORLD_FOCUS_LIST: return MPP_LOAD_FOCUS_LIST;
		case SCREEN_SWORLD_FOCUS_TAB_LOAD: return MPP_LOAD_FOCUS_TAB_LOAD;
		case SCREEN_SWORLD_FOCUS_TAB_CREATE: return MPP_LOAD_FOCUS_TAB_CREATE;
		case SCREEN_SWORLD_FOCUS_TAB_JOIN: return MPP_LOAD_FOCUS_TAB_JOIN;
		case SCREEN_SWORLD_FOCUS_PREV: return MPP_LOAD_FOCUS_PREV;
		case SCREEN_SWORLD_FOCUS_NEXT: return MPP_LOAD_FOCUS_NEXT;
		case SCREEN_SWORLD_FOCUS_PLAY: return MPP_LOAD_FOCUS_PLAY;
		case SCREEN_SWORLD_FOCUS_RETRY: return MPP_LOAD_FOCUS_RETRY;
		case SCREEN_SWORLD_FOCUS_OPTIONS: return MPP_LOAD_FOCUS_OPTIONS;
		default: return MPP_LOAD_FOCUS_OTHER;
	}
}

/* ---- acting -------------------------------------------------------------- */

/* push-0921/B19: publish the core's watched cell to the network layer, every
 * frame, from the core's own out. The core sets mpp_out.watch from the FIRST
 * frame of a dig / place / plant-clear wait, so calling this BEFORE the
 * frame's buttons (see mp_play_frame) means the watch is armed before the
 * press that will produce the echo can possibly be answered.
 *
 * Unconditional -- `out->watch == false` turns it OFF rather than leaving the
 * previous cell armed. A watch left behind would keep storing echoes for a
 * cell nobody is waiting on, and the next wait's first frame would find a
 * stale record in the slot. */
static void mpp_apply_watch(const struct mpp_out* out) {
	if(!s.hooks.remote_watch)
		return; /* inert: obs.watch_echo stays 0 and the core uses the ring */
	s.hooks.remote_watch(s.hooks.user, out->watch, out->watch_x, out->watch_y,
						 out->watch_z);
}

static void mpp_apply_login(const struct mpp_out* out) {
	struct chat_login_hooks login;
	if(!out->login_start)
		return;
	if(s.login_started) {
		/* Generation 2 on the SAME still-bound pad. A refusal must not leave
		 * the machine reading PARKED from generation 1's login -- the core
		 * would credit an instant second login. */
		if(!chat_login_play_restart())
			s.login_restart_failed = true;
		return;
	}
	memset(&login, 0, sizeof(login));
	login.emit = s.hooks.emit;
	login.profile_done = s.hooks.profile_done;
	login.chat_lines_sent = s.hooks.chat_lines_sent;
	login.user = s.hooks.user;
	/* Play mode: no hw_run config and no profile gate -- this driver decides
	 * when a login is due, twice per run. chat_login_play_end() below is what
	 * keeps its profile_done from ever firing; completion is ours.
	 * REL-14b-0: ACTION-ONLY. Vanilla Beta never answers `/login` (CFR
	 * NetServerHandler.java:471-474), so readiness is the observed session
	 * and bind, not a typed command's fixture ACK. */
	chat_login_arm_play_actions(&login, s.username);
	s.login_started = true;
}

static void mpp_apply_actions(const struct mpp_obs* o, const struct mpp_out* out,
							  const struct mp_play_ctx* ctx) {
	enum input_button list[MPP_MAP_MAX_ACTIONS];
	int n;
	unsigned actions = out->actions;
	/* The LOGIN owns the pad while it runs (REL-14b: action-only, it acquires
	 * a fresh ready generation; the pad is refused until PARKED), and the
	 * recovery modal owns the whole update when it
	 * is up -- main.c skips the screen's own update then. A press posted into
	 * either would land where nobody is looking. Release rather than hold: a
	 * button still down when the composer opens would reach it. */
	if(o->login == MPP_LOGIN_RUNNING || ctx->recovery_modal) {
		mpp_map_mps_route_reset(&s.mps_route_advanced);
		(void)chat_login_play_request(NULL, 0);
		return;
	}
	if(ctx->current_screen == &screen_select_world
	   && actions == MPP_ACT_GUI_DOWN) {
		enum input_button route
			= mpp_map_load_route_button(
				mpp_load_focus_from_screen(screen_sworld_focus_target()));
		list[0] = route;
		n = 1;
	} else if(screen_multiplayer_setup
			  && ctx->current_screen == screen_multiplayer_setup
			  && actions == MPP_ACT_GUI_DOWN) {
		/* The first frame enters the already-selected row. On the next frame the
		 * screen has consumed that DOWN edge, so switch this continuous abstract
		 * press to RIGHT and hold it through the Join-focus frame. Returning to
		 * DOWN before the neutral gap would create a fresh edge and select the
		 * next saved endpoint. */
		if(o->focus == MPP_FOCUS_MPS_JOIN)
			s.mps_route_advanced = true;
		list[0] = mpp_map_mps_route_step(&s.mps_route_advanced);
		n = 1;
	} else {
		mpp_map_mps_route_reset(&s.mps_route_advanced);
		n = mpp_map_actions(actions, list, MPP_MAP_MAX_ACTIONS);
	}
	if(n < 0) {
		/* The core asked for something outside the vocabulary the map knows.
		 * Counted and released -- never partially pressed, which would be a
		 * press the core believes it made in full. The step that needed it
		 * then fails on its own budget, by name. */
		s.refused_actions++;
		(void)chat_login_play_request(NULL, 0);
		return;
	}
	/* Every frame, including count 0: the request PERSISTS until replaced, so
	 * a release is a call, not the absence of one. */
	(void)chat_login_play_request(list, (unsigned)n);
}

static void mpp_apply_motion(const struct mpp_out* out) {
	/* Walk first: autopilot's seam resolves walk-over-aim, and asserting the
	 * aim before the walk would set the angles and have the walker take them
	 * back on the same frame.
	 *
	 * BOTH angles cross the convention boundary here, and nowhere else: the
	 * core speaks Beta's yaw, the aim lock and the walker speak this engine's
	 * (mpp_map_engine_yaw_deg has the derivation). */
	autopilot_script_walk(out->walk,
						  mpp_map_engine_yaw_deg(out->walk_heading_deg));
	autopilot_script_aim(out->aim, mpp_map_engine_yaw_deg(out->yaw_deg),
						 out->pitch_deg);
}

static void mpp_drain_markers(void) {
	const struct mpp_marker* m;
	while((m = mpp_marker_peek(&s.core)) != NULL) {
		if(!mpp_map_marker_line(s_line, sizeof(s_line), m, s.username,
								CHAT_LOGIN_PLATFORM))
			break; /* would not fit: never emit a partial JSON object */
		if(!s.hooks.emit || !s.hooks.emit(s.hooks.user, s_line))
			break; /* refused: the SAME line, the same seq, next frame */
		/* Popped ONLY once the sender took it, which is what lets the
		 * collector assume a gapless seq. */
		mpp_marker_pop(&s.core);
	}
}

static void mpp_finish(void) {
	char status[48];
	if(!mpp_done(&s.core) || s.done_sent)
		return;
	if(!s.done_seen) {
		s.done_seen = true;
		s.done_at_ms = s.core.now_ms;
		input_pointer_set_script(NULL);
		/* Withdraw the pad and release the body the moment the verdict is in,
		 * not when the queue happens to drain: chat_login_play_end() does NOT
		 * report (this driver owns completion), and the autopilot seam hands
		 * back whatever state it found. */
		chat_login_play_end();
		autopilot_script_walk(false, 0);
		autopilot_script_aim(false, 0, 0);
	}
	/* Let the run's own last word (`ok` / `fail`) reach the wire first, but
	 * never at the cost of the verdict: past the drain window profile_done
	 * goes out regardless, so a stuck queue reports a failure instead of
	 * looking like a silent pass. */
	if(s.core.q_count && s.core.now_ms - s.done_at_ms < MPP_DONE_DRAIN_MS)
		return;
	if(s.hooks.profile_done)
		s.hooks.profile_done(s.hooks.user,
							 mpp_status(&s.core, status, sizeof(status)));
	s.done_sent = true;
}

/* ---- the frame ----------------------------------------------------------- */

void mp_play_frame(const struct mp_play_ctx* ctx) {
	struct mpp_obs obs;
	struct mpp_out out;
	if(!s.armed || !ctx)
		return; /* inert: see mp_play_script.h */

	memset(&obs, 0, sizeof(obs));
	mpp_fill_session(&obs, ctx);
	if(ctx->current_screen == &screen_select_world
	   && (s.core.phase == MPP_PH_REJOIN_TITLE
		   || s.core.phase == MPP_PH_REJOIN_SETUP))
		obs.screen = MPP_SCREEN_MP_SETUP;
	mpp_fill_body(&obs, ctx);
	mpp_fill_target(&obs, ctx); /* after the body: it uses obs.x/y/z */
	mpp_fill_inventory(&obs);
	mpp_fill_remote(&obs);
	obs.login = mpp_login_state();
	obs.focus = mpp_focus_now(&obs); /* after the screen, which it branches on */

	mpp_tick(&s.core, &obs, &out);

	/* Login control first: a restart must be in flight before the next frame's
	 * observation is taken, or that frame would still read the PREVIOUS
	 * login's PARKED and credit generation 2 with a login it never made. */
	mpp_apply_login(&out);
	/* The watched cell BEFORE the buttons (push-0921/B19): the press applied
	 * below is what makes the server send the echo this frame named, so the
	 * network layer must already be watching that cell when it arrives. */
	mpp_apply_watch(&out);
	mpp_apply_actions(&obs, &out, ctx);
	mpp_apply_motion(&out);
	mpp_drain_markers();
	mpp_finish();
}
