/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h> /* nextafterf: local_eye_on_feet */

#include "../block/block_slipperiness.h"
#include "../block/blocks.h" /* block_is_normal_cube */
#include "../block/blocks_data.h"
#include "../platform/audio.h" // MP client-side footstep (remote_session)
#include "../platform/input_owner.h"
#include "../worldgen/beta_math.h" /* mh_sin/mh_cos: MathHelper.SIN_TABLE */
#include "../game/game_state.h"
#include "../network/client_world_payload.h"
#include "../network/client_interface.h" /* D1: clin_input_epoch, clin_transfer_actor_held */
#include "../network/server_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "entity.h"
#include "entity_boat.h"
#include "entity_local_player.h" /* local_player_column_present, exported */
#include "entity_minecart.h"
#include "combat.h" // PIG_MOUNTED_Y_OFFSET, MONSTER_PIG (saddle-slice review fix)
#include "../network/vehicle_smoke_probe.h" /* host_vehicle_smoke_8m follow witness */

#define EYE_HEIGHT 1.62F

bool local_player_online_dismount(struct entity* cart) {
	/* Beta's dismount is the same interact that mounted us: Packet7
	 * use-entity on the ridden vehicle [740c583 EntityMinecart.interact ->
	 * Entity.mountEntity on the vehicle already ridden -> release]. The
	 * server answers with Packet39(-1); until then nothing changes locally. */
	if(!cart)
		return false;
	return svin_rpc_send(&(struct server_rpc) {
		.type = SRPC_ENTITY_USE,
		.payload.entity_use.entity_id = cart->id,
		.payload.entity_use.desired_occupied = false,
	});
}

static void liquid_aabb(struct AABB* out, struct block_info* blk_info) {
	int block_height = (blk_info->block->metadata & 0x8) ?
		16 :
		(8 - blk_info->block->metadata) * 2 * 7 / 8;
	aabb_setsize(out, 1.0F, (float)block_height / 16.0F, 1.0F);
	aabb_translate(out, blk_info->x, blk_info->y, blk_info->z);
}

static bool test_in_lava(struct AABB* entity, struct block_info* blk_info) {
	if(blk_info->block->type != BLOCK_LAVA_FLOW
	   && blk_info->block->type != BLOCK_LAVA_STILL)
		return false;

	struct AABB bbox;
	liquid_aabb(&bbox, blk_info);
	return aabb_intersection(entity, &bbox);
}

static bool test_in_water(struct AABB* entity, struct block_info* blk_info) {
	if(blk_info->block->type != BLOCK_WATER_FLOW
	   && blk_info->block->type != BLOCK_WATER_STILL)
		return false;

	struct AABB bbox;
	liquid_aabb(&bbox, blk_info);
	return aabb_intersection(entity, &bbox);
}

static bool test_in_liquid(struct AABB* entity, struct block_info* blk_info) {
	return test_in_water(entity, blk_info) || test_in_lava(entity, blk_info);
}

/* A BETA chunk IS a full 16x128x16 COLUMN; this port's chunk is one 16x16x16
 * SECTION of a column. So Beta's
 *     blockExists at the floored X, y 64, the floored Z
 * [740c583 EntityClientPlayerMP.onUpdate -> World.blockExists ->
 * chunkExists on the chunk coordinates] asks "is MY CHUNK loaded", and its literal 64
 * is only "some y inside the column" -- it makes no claim about the y=[64,79]
 * band, because in Beta there is no such band to claim. Translating that one
 * call as world_find_chunk(x, 64, z) therefore asks a DIFFERENT, wrong
 * question here (F8's known regression: three fixtures stand a local player at
 * y~103 with no y=[64,79] section and froze forever).
 *
 * The faithful translation (F8b ruling) is "at least one section of the column
 * (cx, cz) is present in the world": the remote column install creates a
 * column's sections together, and no Beta column is entirely empty, so
 * any-section is exactly Beta's chunkExists. The player's-own-y variant is
 * REJECTED -- it is strictly weaker than Beta, since a section can exist above
 * a still-missing ground section, which would permit precisely the join-time
 * fall this gate exists to stop.
 *
 * Beta's own literal 64 is probed FIRST, so the common (loaded) case still
 * costs exactly one world_find_chunk.
 *
 * EXPORTED (push-0921/B4c), declared in entity_local_player.h: scripted
 * observers need THE gate, not a second copy of it -- see that header. The
 * body below is byte-for-byte what it was as a file static; only `static` is
 * gone. */
bool local_player_column_present(struct world* w, w_coord_t x, w_coord_t z) {
	assert(w);

	const w_coord_t beta_cy = 64 / CHUNK_SIZE;

	if(world_find_chunk(w, x, beta_cy * CHUNK_SIZE, z))
		return true;

	for(w_coord_t cy = 0; cy < WORLD_HEIGHT / CHUNK_SIZE; cy++) {
		if(cy == beta_cy)
			continue;

		if(world_find_chunk(w, x, cy * CHUNK_SIZE, z))
			return true;
	}

	return false;
}

/* host_vehicle_smoke_8m (CONTRACT-v1 ride_sample client): the seat error of
 * an ACTUAL follow application, measured against the same-tick client
 * vehicle and the same formula, after the write. Called only when armed. */
static float vsp_seat_error(const float got[3], const float want[3]) {
	float worst = 0.0F;
	for(int k = 0; k < 3; k++) {
		float d = fabsf(got[k] - want[k]);
		if(d > worst)
			worst = d;
	}
	return worst;
}

/* D1 (ASTRA-DEFECTS-RULING items 2-6): the HOST ride-action latch.
 *
 * The F3-1 JUMP dismount is a one-frame input edge (input_pressed), but its
 * consumer below runs only in the 20 Hz client tick. On a 60 Hz frame loop
 * two frames in three run NO tick (more on a faster PC), and a press whose
 * edge frame ran no tick was lost (measured on the PC smoke: the JUMP frame
 * had ticks_this_frame=0, cart dismount_timeout_st3). The edge is now
 * captured EVERY frame (entity_local_player_ride_action_frame, called by
 * main.c right before the tick catch-up) and consumed ONCE by the next host
 * tick that reaches the JUMP site. Held state (frame.down) and every
 * frame-based UI edge are untouched; the guest (slot 1) keeps its own owned
 * frame edge exactly as before.
 *
 * Scope. A latch is only taken while the host RIDES a cart, and records that
 * cart, the input epoch and the ride transaction (next_request_seq). It is
 * dropped by any ineligible frame (menu/pause/focus or controller loss,
 * death, world switch, transfer hold), by any capture that finds the host no
 * longer on that cart, and -- living in struct minecart_ride_client_state --
 * by every ride reset (world reset, cart destroy, death/body replacement,
 * load failure). The take re-validates all three keys, so a press can never
 * escape into a later ride.
 *
 * Not Beta: Beta has no JUMP dismount (EntityMinecart.interact toggles the
 * rider, EntityMinecart.java:763-770) and reads jump as HELD key state
 * (MovementInputFromOptions.java:38,69), so a press+release drained between
 * two ticks leaves jump false there. Reliable tick consumption of the short
 * tap is the documented controller adaptation in
 * docs/wii_controller_playability.md F3-1, not a byte-exact Beta behaviour.
 *
 * The per-frame hook is the ONLY capture point: it runs exactly once per
 * input frame, so one edge is latched at most once however many catch-up
 * ticks the frame then runs (each tick sees the same input_pressed edge;
 * before D1 every one of them re-fired it -- online, a second Packet7 would
 * have toggled the rider straight back on). */

/* The cart the host session rides, chosen exactly as the follow scan below
 * chooses it: a pending MOUNT target first, else any cart naming the body as
 * its occupant. NULL when not riding a cart. */
static struct entity* local_ride_action_cart(struct entity* e,
	struct client_session* session, struct dict_entity_s* entities) {
	if(!entities)
		return NULL;
	if(session->ride.pending && session->ride.desired_occupied
	   && session->ride.target_id != 0) {
		struct entity** pp = dict_entity_get(entities, session->ride.target_id);
		if(pp && *pp && (*pp)->type == ENTITY_MINECART
		   && (*pp)->data.minecart.occupied
		   && (*pp)->data.minecart.occupant_id == e->id)
			return *pp;
	}
	dict_entity_it_t it;
	dict_entity_it(it, entities);
	while(!dict_entity_end_p(it)) {
		struct entity* cart = dict_entity_ref(it)->value;
		if(cart && cart->type == ENTITY_MINECART
		   && cart->data.minecart.occupied
		   && cart->data.minecart.occupant_id == e->id)
			return cart;
		dict_entity_next(it);
	}
	return NULL;
}

/* O07 / R04 D1-2: the host's input is LIVE -- its owned controller frame
 * (when one is bound) is connected, or the harness/driver input that is
 * explicitly authorized to drive a controllerless host (the autopilot,
 * which also carries the vehicle-smoke script) owns it. The input layer
 * publishes a disconnect at input_poll, BEFORE the next frame's recovery /
 * ownership gate refreshes the body's cached capture_input; reading the
 * owner frame here closes that window for both capture and consumption. */
static bool local_ride_action_input_live(void) {
	if (gstate.autopilot.active)
		return true;
	const struct input_player_frame* owner = input_player_frame(0);
	return !owner || owner->connected;
}

static void local_ride_action_capture(struct entity* e,
	struct client_session* session, struct dict_entity_s* entities,
	bool eligible) {
	struct minecart_ride_client_state* r = &session->ride;
	bool fresh = input_pressed(IB_JUMP);
	if(!eligible) {
		r->action_latched = false;
		return;
	}
	/* Idle frames (no edge, nothing pending) cost no entity scan. */
	if(!fresh && !r->action_latched)
		return;
	struct entity* cart = local_ride_action_cart(e, session, entities);
	if(r->action_latched && (!cart || cart->id != r->action_cart_id))
		r->action_latched = false; /* that ride ended */
	if(!fresh || !cart)
		return;
	/* A leave for this cart is already in flight: nothing to latch. */
	if(r->pending && !r->desired_occupied && r->target_id == cart->id)
		return;
	r->action_latched = true;
	r->action_cart_id = cart->id;
	r->action_ride_epoch = cart->data.minecart.ride_epoch;
	r->action_input_epoch = clin_input_epoch();
	r->action_request_seq = r->next_request_seq;
}

/* Consume the latch at the JUMP site for `cart`: true at most once. */
static bool local_ride_action_take(struct client_session* session,
								   const struct entity* cart) {
	struct minecart_ride_client_state* r = &session->ride;
	if(!r->action_latched)
		return false;
	/* O07 / R04 D1-1: the same RIDE, not just the same cart: an authoritative
	 * end and restart of this cart's ride carries a new ride epoch. A tap
	 * taken under epoch 0 (the optimistic mount, before its confirmation)
	 * stays valid when that mount is confirmed. O07 D1-2: never consumed
	 * while the owner's controller is gone. */
	bool ok = r->action_cart_id == cart->id
		&& r->action_input_epoch == clin_input_epoch()
		&& r->action_request_seq == r->next_request_seq
		&& (r->action_ride_epoch == 0
			|| r->action_ride_epoch == cart->data.minecart.ride_epoch)
		&& local_ride_action_input_live();
	r->action_latched = false;
	return ok;
}

/* The legacy mounted path, shared by both local sessions. True means the
 * seat owns this tick and ordinary walking/footsteps must be skipped. */
static bool local_player_follow_mounted(struct entity* e,
	struct client_session* session, const struct input_player_frame* frame) {
	unsigned slot = session == &gstate.players[1] ? 1 : 0;
	struct server_rpc_source source = {
		.slot = slot,
		.generation = slot ? session->local_generation : 0,
	};
	struct dict_entity_s* entities = world_view_entities();
	if(slot && clin_world_payloads_enabled()) {
		struct clin_world_payload payload;
		if(!clin_world_payload_of_view(slot, &payload))
			return false;
		entities = payload.entities;
	}

	// PIG MOUNT FOLLOW (saddle-slice review fix, finding 1) [740c583: Entity.
	// updateRidden -> updateRiderPosition]. A saddled pig has no steering
	// (EntityPig has no moveEntityWithHeading override in b1.7.3 -- see the
	// negative-control tests in tests/test_pig_saddle_env.c), so unlike the
	// minecart scan below there is no throttle/control-epoch protocol to run --
	// this mirrors ONLY the minecart's "follow position + skip walking physics"
	// halves. The server is authoritative (it forces the identical formula every
	// tick in entity_monster.c and can be reached via a plain SRPC_PLAYER_POS/
	// PLAYER_CORRECTION_POSITION_ONLY correction regardless), so this client-side
	// copy exists to keep local prediction from visibly fighting that authority
	// and to let the EXISTING generic e->pos-driven camera_attach (game/camera.c)
	// follow the mount -- no source/graphics/ edit needed for that. Mutually
	// exclusive with the minecart scan below by construction (finding 2:
	// mounting either kind of vehicle releases any other the player already
	// rides), so at most one of the two blocks ever matches for a real player.
	{
		dict_entity_it_t pig_it;
		dict_entity_it(pig_it, entities);
		while(!dict_entity_end_p(pig_it)) {
			struct entity* pig = dict_entity_ref(pig_it)->value;
			if(pig && pig->type == ENTITY_MONSTER
			   && pig->data.monster.id == MONSTER_PIG
			   && pig->data.monster.pig_ridden
			   && pig->data.monster.pig_rider_id == e->id) {
				e->pos[0] = pig->pos[0];
				e->pos[1] = pig->pos[1] + PIG_MOUNTED_Y_OFFSET
					+ PLAYER_EYE_HEIGHT;
				e->pos[2] = pig->pos[2];
				glm_vec3_copy(e->pos, e->pos_old);
				glm_vec3_zero(e->vel);
				/* P2's pose producer carries boat input even on another mount.
				 * Retire the previous boat's steering; preserve the host path. */
				if(slot) {
					e->data.local_player.boat_rider_vel_x = 0.0F;
					e->data.local_player.boat_rider_vel_z = 0.0F;
				}
				/* b1-chat-opener-0919 slice 2 (Astra ruling 3 D3): a successful
				 * vehicle ENTRY clears the Wii Remote sneak toggle. This scan is
				 * already the one place that knows the local player is aboard, so
				 * it publishes the level and input.c takes the entry edge. No-op
				 * off the Wii. */
				input_sneak_vehicle_mark(slot, true);
				/* No sneak dip in a vehicle. This branch skips the walking
				 * tick, so nothing would decay ySize and a dip entered
				 * before mounting would freeze; the toggle is cleared on
				 * entry by the line above, so the dip must go too. */
				player_ysize_reset(&session->camera.ysize);
				if(vsp_armed()) {
					float want[3] = {pig->pos[0],
						pig->pos[1] + PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT,
						pig->pos[2]};
					vsp_client_follow(session, e, pig,
									  vsp_seat_error(e->pos, want));
				}
				return true; // skip walking physics when riding (minecart precedent)
			}
			dict_entity_next(pig_it);
		}
	}

	// MINECART CONTROL: detect cart by scanning entities for occupant_id == player id
	/* Pre-selection: if a pending mount intent targets a specific cart,
	 * resolve it by ID before the scan so the correct candidate is
	 * processed regardless of hash iteration order. Without this, if
	 * the player is still locally occupied on cart A while a pending
	 * mount targets cart B, a hash order that encounters A first would
	 * cause JUMP to author a dismount for A instead of the pending
	 * target B. */
	struct entity* preferred_cart = NULL;
	if(session->ride.pending
	   && session->ride.desired_occupied
	   && session->ride.target_id != 0) {
		struct entity** pp = dict_entity_get(entities,
		                                     session->ride.target_id);
		if(pp && *pp && (*pp)->type == ENTITY_MINECART
		   && (*pp)->data.minecart.occupied
		   && (*pp)->data.minecart.occupant_id == e->id) {
			preferred_cart = *pp;
		}
	}

	dict_entity_it_t it;
	dict_entity_it(it, entities);
	while(!dict_entity_end_p(it)) {
		dict_entity_itref_t *ref = dict_entity_ref(it);      // ref is a pointer to pair { key, value* }
		struct entity* cart = ref->value;                   // value is struct entity*, so deref once
		/* The cumulative tuple remains dirty until an authoritative snapshot
		 * acknowledges it. Retry it even after optimistic dismount cleared the
		 * occupant fields; otherwise a temporarily full transport can lose the
		 * final release-frame input forever. The dedicated transport slot coalesces
		 * repeats at their original FIFO position. */
		/* vehicle-smoke-1004 O03: a cart's cumulative control tuple belongs to
		 * the session that AUTHORED it. The retry admits carts this body no
		 * longer occupies, which for the host is exactly its own optimistic
		 * dismount; for the GUEST it also admitted every cart some OTHER
		 * body rides (occupant_id 0 = the host != guest id 1), so the guest
		 * resent the host's unacknowledged tuple on the slot-1 route every
		 * tick of the host's ride (measured on the PC run: ~110 slot-1
		 * MINECART_CONTROL refusals). The guest retries only a cart its OWN
		 * ride transaction names (it can author a tuple only for that cart);
		 * the host condition is unchanged byte for byte. */
		if(cart && cart->type == ENTITY_MINECART
		   && cart->data.minecart.control_ready
		   && cart->data.minecart.control_dirty
		   && (!cart->data.minecart.occupied
			   || cart->data.minecart.occupant_id != e->id)
		   && (slot == 0 || session->ride.target_id == cart->id)) {
			(void)svin_rpc_send_from(source, &(struct server_rpc) {
				.type = SRPC_MINECART_CONTROL,
				.payload.minecart_control = {
					.entity_id = cart->id,
					.ride_epoch = cart->data.minecart.ride_epoch,
					.input_seq = cart->data.minecart.control_seq,
					.positive_steps
						= cart->data.minecart.control_positive_steps,
					.negative_steps
						= cart->data.minecart.control_negative_steps,
				},
			});
		}
		if(cart && cart->type == ENTITY_MINECART &&
		   cart->data.minecart.occupied &&
		   cart->data.minecart.occupant_id == e->id
		   && (!preferred_cart || cart == preferred_cart)) {

			/* F3: if a pending leave intent targets this cart, suppress
			 * the follow-pin for the entire pending lifetime. A chunk
			 * respawn can re-initialize the cart occupied while the
			 * false intent is still pending; without this guard the
			 * next tick would follow/pin the player back to the cart. */
			if(session->ride.pending
			   && !session->ride.desired_occupied
			   && session->ride.target_id == cart->id) {
				/* Keep the control tuple's existing final send/retry
				 * eligible, but do not follow or return. */
				if(cart->data.minecart.control_ready
				   && cart->data.minecart.control_dirty) {
					(void)svin_rpc_send_from(source, &(struct server_rpc) {
						.type = SRPC_MINECART_CONTROL,
						.payload.minecart_control = {
							.entity_id = cart->id,
							.ride_epoch = cart->data.minecart.ride_epoch,
							.input_seq = cart->data.minecart.control_seq,
							.positive_steps
								= cart->data.minecart.control_positive_steps,
							.negative_steps
								= cart->data.minecart.control_negative_steps,
						},
					});
				}
				/* Fall through to normal player physics. */
			} else {
			/* Beta Entity.updateRidden:879-881 zeroes the rider's motion at
			 * the START of every ridden tick, before onUpdate (the pig branch
			 * above already does). Without it a velocity carried from before
			 * the mount survived the whole ride and, on a JUMP escape tick,
			 * the ordinary physics below dropped the body under the clearance
			 * floor (the removed pre-W1A "unstuck" snap had been hiding it). */
			glm_vec3_zero(e->vel);
			/* F3: JUMP is an escape input, not a control-session input.
			 * Detect JUMP after capture_input but BEFORE the control_ready /
			 * nonzero-epoch gate, so an epoch-zero optimistic mount or a
			 * confirmed ride can both escape. On JUMP: author
			 * desired_occupied=false, predict local detachment, set the
			 * clearance floor, and skip the follow-copy this tick. */
			bool jump_escape = false;
			/* D1: the host consumes its tick-latched ride action (once); the
			 * guest keeps its own owned frame edge, unchanged. */
			bool jump_action = e->data.local_player.capture_input
				&& frame && frame->connected
				&& (slot == 0 ? local_ride_action_take(session, cart)
							  : frame->pressed[IB_JUMP]);
			if(gstate.remote_session) {
				/* Online: the F3-1 JUMP dismount keeps working, but as
				 * Beta's own dismount -- ONE Packet7 use-entity on the cart we
				 * ride (EntityMinecart.interact toggles it off server-side)
				 * and NO local prediction: the ride-intent RPC is
				 * server_local-only (the remote provider drops it), and a
				 * local occupancy clear would desync the riding wire form
				 * (Packet11/13 with -999) until the server's Packet39(-1)
				 * snapped us back onto the cart. The follow-copy below stays
				 * live until CRPC_ATTACH_ENTITY releases the cart.
				 * docs/mp_mounts_design_2026-09-11.md "Online JUMP dismount". */
				if(jump_action)
					(void)local_player_online_dismount(cart);
			} else
			if(jump_action) {
				/* Author a dismount ride-intent through the centralized
				 * state. The clin_update tick sends the actual RPC. */
				session->ride.target_id = cart->id;
				session->ride.next_request_seq++;
				if(session->ride.next_request_seq == 0)
					session->ride.next_request_seq = 1;
				session->ride.pending_request_seq
					= session->ride.next_request_seq;
				session->ride.desired_occupied = false;
				session->ride.send_dirty = true;
				session->ride.pending = true;
				/* Predict local detachment immediately. */
				cart->data.minecart.occupied = false;
				cart->data.minecart.occupant_id = 0;
				cart->data.minecart.pending_throttle = 0;
				cart->data.minecart.replay_throttle = 0;
				cart->rightClickText = "Ride";
				/* Clearance lift: both interpolation samples to at least
				 * cart_y + 1.0 + 1.2, recording that absolute floor. Clamp
				 * each vertical sample independently so an already-higher body
				 * is not lowered, and leave X/Z interpolation untouched. */
				float floor_y = cart->pos[1] + 1.0f + 1.2f;
				if(e->pos[1] < floor_y)
					e->pos[1] = floor_y;
				if(e->pos_old[1] < floor_y)
					e->pos_old[1] = floor_y;
				session->ride.exit_floor_y = floor_y;
				session->ride.exit_lift_pending = true;
				session->ride.exit_repin_eligible = false;
				jump_escape = true;
			}

			if(!jump_escape) {
			// Only READ rider input when the control session is live. Online
			// there is no control session: Beta SMP carts take no rider
			// throttle (the server never sees moveForward), so nothing is
			// authored -- the remote provider would drop it anyway.
			if(!gstate.remote_session
			   && e->data.local_player.capture_input
			   && cart->data.minecart.control_ready
			   && cart->data.minecart.ride_epoch != 0) {
				// Rider throttle. SEND the signed input to the authority.
				int8_t intent = 0;
				if((frame && frame->connected && frame->down[IB_FORWARD]))  intent += 1;
				if((frame && frame->connected && frame->down[IB_BACKWARD])) intent -= 1;
				int8_t throttle = entity_minecart_resolve_throttle(
					intent, cart->data.minecart.speed, cart->data.minecart.hx,
					cart->data.minecart.hz, e->orient[0]);
				if(throttle != 0) {
					cart->data.minecart.control_seq++;
					if(throttle > 0)
						cart->data.minecart.control_positive_steps++;
					else
						cart->data.minecart.control_negative_steps++;
					cart->data.minecart.pending_throttle += throttle;
					cart->data.minecart.control_dirty = true;
				}
			}

			// Follow cart position (simple, cheap copy)
			e->pos[0] = cart->pos[0];
			e->pos[1] = cart->pos[1] + 1.0f;
			e->pos[2] = cart->pos[2];
			glm_vec3_copy(e->pos, e->pos_old);
			} /* end !jump_escape */

			/* Send once after this tick's authoring and possible optimistic
			 * dismount. Failed sends remain dirty and are retried by the scan
			 * above. On a JUMP escape tick, suppress new throttle but keep
			 * an already-dirty tuple's existing final send/retry eligible. */
			if(cart->data.minecart.control_ready
			   && cart->data.minecart.control_dirty) {
				(void)svin_rpc_send_from(source, &(struct server_rpc) {
					.type = SRPC_MINECART_CONTROL,
					.payload.minecart_control = {
						.entity_id = cart->id,
						.ride_epoch = cart->data.minecart.ride_epoch,
						.input_seq = cart->data.minecart.control_seq,
						.positive_steps
							= cart->data.minecart.control_positive_steps,
						.negative_steps
							= cart->data.minecart.control_negative_steps,
					},
				});
			}

			if(!jump_escape) {
				/* b1-chat-opener-0919 slice 2 (Astra ruling 3 D3): a successful
				 * vehicle ENTRY clears the Wii Remote sneak toggle. This scan is
				 * already the one place that knows the local player is aboard, so
				 * it publishes the level and input.c takes the entry edge. No-op
				 * off the Wii. */
				input_sneak_vehicle_mark(slot, true);
				/* No sneak dip in a vehicle. This branch skips the walking
				 * tick, so nothing would decay ySize and a dip entered
				 * before mounting would freeze; the toggle is cleared on
				 * entry by the line above, so the dip must go too. */
				player_ysize_reset(&session->camera.ysize);
				if(slot) {
					e->data.local_player.boat_rider_vel_x = 0.0F;
					e->data.local_player.boat_rider_vel_z = 0.0F;
				}
				if(vsp_armed()) {
					float want[3] = {cart->pos[0], cart->pos[1] + 1.0f,
									 cart->pos[2]};
					vsp_client_follow(session, e, cart,
									  vsp_seat_error(e->pos, want));
				}
				return true; // skip walking physics when riding
			}
			/* On JUMP escape: leave the riding branch and continue through
			 * ordinary player physics that tick (design section 4).
			 * Break out of the cart scan so the frame-latched
			 * JUMP cannot author a second false intent against another
			 * local claim during the A->B prediction gap. Exactly one ride
			 * candidate is processed per tick. */
			} /* end else (not pending leave) */
			/* ride_candidate fully processed (riding return, or JUMP escape
			 * fall-through). No more rider input this tick. */
			break;
		}
		/* Normal iteration: dirty-control retries continue for other
		 * carts. The break above only fires when we hit ride_candidate. */
		dict_entity_next(it);
	}

	// BOAT MOUNT (adversarial-review findings #2/#3/#5): tether the local
	// player's OWN entity to the boat it occupies. Before this branch existed
	// at all, mounting a boat only flipped server-side flags -- the player's
	// avatar kept running ordinary walk/swim/gravity physics every tick,
	// completely disconnected from the boat, and past the 6-block reach gate
	// (server_local.c SRPC_ENTITY_USE/SRPC_ENTITY_ATTACK) dismount became
	// permanently unreachable. Mirrors Beta's EntityLiving.updateRidden,
	// which has the ridden entity run updateRiderPosition every tick while mounted
	// and runs no ordinary AI/movement for the rider at all [740c583:
	// Entity.updateRidden]. Unlike the minecart scan above, this is pure
	// position-follow + physics-skip -- no throttle, no JUMP-escape --
	// PLUS (closing docs/deviations.md "Boat rider steering") the rider
	// input->velocity report below, which does not move this entity itself
	// (the tether above already fully owns e->pos while mounted); it only
	// feeds the BOAT's own coupling term via boat->data.boat.rider_vel_x/z.
	dict_entity_it_t boat_it;
	dict_entity_it(boat_it, entities);
	while(!dict_entity_end_p(boat_it)) {
		struct entity* boat = dict_entity_ref(boat_it)->value;
		if(boat && boat->type == ENTITY_BOAT && boat->data.boat.occupied
		   && boat->data.boat.occupant_id == e->id) {
			// [740c583: EntityBoat.updateRiderPosition] rider.pos = boat.pos +
			// (cos(yaw)*0.4, mountedYOffset + rider.yOffset, sin(yaw)*0.4).
			// boat->orient[0] is ALREADY radians in the same atan2(dz,dx)
			// convention Beta's rotationYaw*pi/180 produces (verified against
			// boat_advance's own turn-toward-motion code), so double cos/sin
			// need no extra conversion; X/Z narrow only at float storage.
			// CaveWii's LOCAL PLAYER e->pos[1] is
			// EYE-basis (this file's EYE_HEIGHT convention, e.g. the ladder
			// probe below) while Beta's formula yields the rider's FEET-basis
			// posY -- add EYE_HEIGHT once at this boundary, matching every
			// other feet->eye conversion already in this file, rather than
			// changing what pos[1] means anywhere else.
			e->pos[0] = (float)((double)boat->pos[0]
				+ cos((double)boat->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET);
			e->pos[1] = boat->pos[1] + BOAT_MOUNTED_Y_OFFSET + e->y_offset
				+ EYE_HEIGHT;
			e->pos[2] = (float)((double)boat->pos[2]
				+ sin((double)boat->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET);
			// Snap pos_old to the new pos every tick, matching the minecart
			// follow's own precedent above (a mounted avatar does not
			// interpolate render/camera between ticks).
			glm_vec3_copy(e->pos, e->pos_old);

			// RIDER STEERING INPUT (closes docs/deviations.md "Boat rider
			// steering"): Beta's riddenByEntity.motionX/Z is re-derived
			// FRESH every tick from live movement input [740c583:
			// Entity.updateRidden zeroes motion then calls onUpdate ->
			// EntityLiving.onLivingUpdate -> moveEntityWithHeading(
			// moveStrafing, moveForward) -> moveFlying], independent of the
			// position tether above (Beta's own EntityBoat.
			// updateRiderPosition -- ported above -- only ever writes the
			// rider's POSITION, never its motion). Deliberately does NOT
			// touch e->vel[0]/[2] (kept in the dedicated boat_rider_vel_x/z
			// field instead): test_boat_env.c section (9) pins e->vel
			// byte-unchanged while mounted (no ordinary walking-physics
			// leakage), and Beta's riddenByEntity.motionX/Z is genuinely a
			// different quantity from this engine's persistent e->vel -- it
			// never accumulates tick-to-tick and never drives this entity's
			// own position.
			float forward = 0.0F, strafe = 0.0F;
			if(e->data.local_player.capture_input) {
				if((frame && frame->connected && frame->down[IB_FORWARD]))  forward += 1.0F;
				if((frame && frame->connected && frame->down[IB_BACKWARD])) forward -= 1.0F;
				if((frame && frame->connected && frame->down[IB_RIGHT]))    strafe  += 1.0F;
				if((frame && frame->connected && frame->down[IB_LEFT]))     strafe  -= 1.0F;
				// Beta sneak speed scale, matching the on-foot precedent
				// below [740c583: MovementInputFromOptions].
				if((frame && frame->connected && frame->down[IB_SNEAK])) {
					forward *= 0.3F;
					strafe  *= 0.3F;
				}
			}
			boat_rider_input_velocity(forward, strafe, e->orient[0],
									  &e->data.local_player.boat_rider_vel_x,
									  &e->data.local_player.boat_rider_vel_z);
			boat->data.boat.rider_vel_x
				= e->data.local_player.boat_rider_vel_x;
			boat->data.boat.rider_vel_z
				= e->data.local_player.boat_rider_vel_z;

			/* Same vehicle-entry clear as the cart branch above. */
			input_sneak_vehicle_mark(slot, true);
			/* No sneak dip in a vehicle. This branch skips the walking
			 * tick, so nothing would decay ySize and a dip entered
			 * before mounting would freeze; the toggle is cleared on
			 * entry by the line above, so the dip must go too. */
			player_ysize_reset(&session->camera.ysize);
			if(vsp_armed()) {
				float want[3] = {
					(float)((double)boat->pos[0]
						+ cos((double)boat->orient[0])
						* BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET),
					boat->pos[1] + BOAT_MOUNTED_Y_OFFSET + e->y_offset + EYE_HEIGHT,
					(float)((double)boat->pos[2]
						+ sin((double)boat->orient[0])
						* BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET)};
				vsp_client_follow(session, e, boat, vsp_seat_error(e->pos, want));
			}
			return true; // skip walking physics when riding, matching the cart above
		}
		dict_entity_next(boat_it);
	}

	/* The FALLING edge of the same level (review 09 finding 2). Reaching this
	 * line means this tick matched no pig, no cart and no boat, i.e. the
	 * player is NOT riding -- and the mark is a LEVEL, not a pulse, so it has
	 * to be published both ways or a dismount could never re-arm the entry
	 * clear. Placed here rather than at the top of the tick so the three
	 * riding branches above, which all return early, keep the level they set.
	 * No-op off the Wii. */
	input_sneak_vehicle_mark(slot, false);

	return false;
}

/* D1 (see entity_local_player.h). */
bool entity_local_player_ride_action_eligible(void) {
	struct client_session* s = &gstate.players[0];
	return s->local_player && s->local_player->data.local_player.capture_input
		&& !gstate.paused && game_state_client_entities_active(&gstate)
		&& client_session_host_gameplay_active(s)
		&& !clin_transfer_actor_held(0)
		&& local_ride_action_input_live(); /* O07 D1-2 */
}

/* True once this frame's begin capture saw a JUMP edge (see _frame_end). */
static bool s_ride_action_edge_this_frame;

static void local_ride_action_frame_capture(void) {
	struct client_session* s = &gstate.players[0];
	if(!s->local_player) {
		s->ride.action_latched = false;
		return;
	}
	/* The host body's own world view (player 0's slot; flag OFF: gstate). */
	struct clin_view_world v;
	bool entered = clin_world_view_enter(0, &v);
	local_ride_action_capture(s->local_player, s,
		entered ? v.entities : NULL,
		entered && entity_local_player_ride_action_eligible());
	if(entered)
		clin_world_view_leave(&v);
}

void entity_local_player_ride_action_frame(void) {
	s_ride_action_edge_this_frame = input_pressed(IB_JUMP);
	local_ride_action_frame_capture();
}

void entity_local_player_ride_action_frame_end(void) {
	if(s_ride_action_edge_this_frame) {
		s_ride_action_edge_this_frame = false;
		/* O07 / R04 D1-3: the begin capture owned this frame's edge, so no
		 * second capture -- but a menu/recovery/input loss that arose later
		 * in this frame (the screen section) still invalidates the latch. */
		if (!entity_local_player_ride_action_eligible())
			gstate.players[0].ride.action_latched = false;
		return;
	}
	local_ride_action_frame_capture();
}

static bool entity_tick(struct entity* e) {
	assert(e);

	/* BETA blockExists GATE -- literally the first thing onUpdate does
	 * [740c583 EntityClientPlayerMP.onUpdate]:
	 *   if the block at (floor x, 64, floor z) does not exist yet, return at once;
	 *   only then run the parent onUpdate and func_4056_N (the motion send).
	 * The client player does NOTHING -- no physics, no damage, and no motion
	 * send (func_4056_N is skipped too) -- until the column it stands in
	 * exists. WHY it is load-bearing: a JOINING player must not fall through
	 * terrain that has not arrived yet. Without this gate the body sits at the
	 * server spawn while the join burst is still installing columns, gravity
	 * runs, and the fall the server reads out of the reported positions kills
	 * the player behind the loading screen (real-Wii dense_walk_0913c/d:
	 * y=76 health 160 at tick 1 -> 0 by tick 12).
	 * The predicate is local_player_column_present above -- "any section of my
	 * column is resident", which is what Beta's chunkExists actually asks (see
	 * that helper's comment and the F8b ruling); a literal
	 * world_find_chunk(x, 64, z) would ask only about the y=[64,79] section.
	 * Placed ahead of the sleeping branch to match
	 * Beta's ordering, where the gate precedes everything in onUpdate.
	 * Preserve the interpolation samples and zero the velocity exactly as the
	 * sleeping branch below does, so render/camera attachment sees a
	 * stationary body and the 50 ms position echo keeps reporting an
	 * UNCHANGED position. Returning false means "do not remove this entity"
	 * (entity.c entities_client_tick). */
	if(!local_player_column_present(e->world, (w_coord_t)floorf(e->pos[0]),
									(w_coord_t)floorf(e->pos[2]))) {
		glm_vec3_copy(e->pos, e->pos_old);
		glm_vec2_copy(e->orient, e->orient_old);
		glm_vec3_zero(e->vel);
		return false;
	}

	/* Beta EntityPlayer.isMovementBlocked returns true while sleeping, and
	 * sleepInBedAt zeros all three motion components [740c583 EntityPlayer].
	 * Do this before minecart controls and ordinary physics: a server-only RPC
	 * gate otherwise lets the client accumulate displacement and submit it on
	 * the first wake frame. Preserve the interpolation samples so render/camera
	 * attachment also sees a stationary body. */
	if(gstate.players[0].sleeping) {
		/* EntityClientPlayerMP.onUpdate:31-35 still sends after a sleeping
		 * update; only the blockExists hold above suppresses that update. */
		clin_local_tick_physics_executed(0);
		glm_vec3_copy(e->pos, e->pos_old);
		glm_vec2_copy(e->orient, e->orient_old);
		glm_vec3_zero(e->vel);
		return false;
	}

	struct input_player_frame frame = {.connected = true};
	const enum input_button buttons[] = {
		IB_FORWARD, IB_BACKWARD, IB_RIGHT, IB_LEFT, IB_JUMP, IB_SNEAK
	};
	for(size_t k = 0; k < sizeof(buttons) / sizeof(*buttons); k++)
		frame.down[buttons[k]] = input_held(buttons[k]);
	frame.pressed[IB_JUMP] = input_pressed(IB_JUMP);
	return entity_local_player_walk_owned(e, &gstate.players[0], &frame);
}

/* EntityPlayerSP.isBlockTranslucent:194-196 == World.isBlockNormalCube; an
 * unreadable cell is getBlockId 0, i.e. false. */
static bool local_player_normal_cube(struct entity* e, w_coord_t x,
									 w_coord_t y, w_coord_t z) {
	struct block_data b;
	return entity_get_block(e, x, y, z, &b) && block_is_normal_cube(b.type);
}

/* EntityPlayerSP.pushOutOfBlocks:198-250, transcribed. The sample point is
 * (x + sx x width x 0.35, box minimum Y + 0.5, z + sz x width x
 * 0.35) (onLivingUpdate:83-86); double arithmetic as in Beta. A hit sets
 * motion X or Z to +-0.1 toward the nearest open side; ties keep the
 * first of -X, +X, -Z, +Z. */
static void local_player_push_out_of_blocks(struct entity* e, int sx,
											int sz) {
	const double width = (double)0.6F;
	double px = (double)e->pos[0] + (double)sx * width * 0.35;
	double py = (double)(e->pos[1] - EYE_HEIGHT) + 0.5;
	double pz = (double)e->pos[2] + (double)sz * width * 0.35;
	w_coord_t x = (w_coord_t)floor(px);
	w_coord_t y = (w_coord_t)floor(py);
	w_coord_t z = (w_coord_t)floor(pz);
	double fx = px - (double)x;
	double fz = pz - (double)z;
	if(!local_player_normal_cube(e, x, y, z)
	   && !local_player_normal_cube(e, x, y + 1, z))
		return;
	bool open_nx = !local_player_normal_cube(e, x - 1, y, z)
		&& !local_player_normal_cube(e, x - 1, y + 1, z);
	bool open_px = !local_player_normal_cube(e, x + 1, y, z)
		&& !local_player_normal_cube(e, x + 1, y + 1, z);
	bool open_nz = !local_player_normal_cube(e, x, y, z - 1)
		&& !local_player_normal_cube(e, x, y + 1, z - 1);
	bool open_pz = !local_player_normal_cube(e, x, y, z + 1)
		&& !local_player_normal_cube(e, x, y + 1, z + 1);
	int dir = -1;
	double best = 9999.0;
	if(open_nx && fx < best) {
		best = fx;
		dir = 0;
	}
	if(open_px && 1.0 - fx < best) {
		best = 1.0 - fx;
		dir = 1;
	}
	if(open_nz && fz < best) {
		best = fz;
		dir = 4;
	}
	if(open_pz && 1.0 - fz < best) {
		best = 1.0 - fz;
		dir = 5;
	}
	const float push = 0.1F;
	if(dir == 0)
		e->vel[0] = -push;
	if(dir == 1)
		e->vel[0] = push;
	if(dir == 4)
		e->vel[2] = -push;
	if(dir == 5)
		e->vel[2] = push;
}

/* EntityLiving.stepHeight [EntityLiving.java:67]. */
#define LOCAL_PLAYER_STEP_HEIGHT 0.5F

/* The eye whose derived box minimum (eye + box_lo, the value every
 * collision test of the flat pass uses) sits ON `feet`: the same "face - lo,
 * nudged one ulp to the safe side" rule entity_try_move uses when it places a
 * body on a face (entity.c). Used to hand a step-route result, computed on
 * the box minimum, back to the eye-based body. */
static float local_eye_on_feet(float feet, float box_lo) {
	float eye = feet - box_lo;
	if(eye + box_lo < feet)
		eye = nextafterf(eye, INFINITY);
	return eye;
}

bool entity_local_player_walk_owned(struct entity* e,
	struct client_session* session, const struct input_player_frame* frame) {
	assert(e && session);
	/* EntityClientPlayerMP.onUpdate:31-35 sends after every eligible update,
	 * including sleep (func_4056_N:80-81, stationary Packet10). Main's guest
	 * dispatch / the host entity tick already checked the terrain boundary. */
	if(e == session->local_player) {
		if(session == &gstate.players[0]) clin_local_tick_physics_executed(0);
		else if(session == &gstate.players[1]) clin_local_tick_physics_executed(1);
	}
	/* Beta EntityPlayer.isMovementBlocked returns true while sleeping, and
	 * sleepInBedAt zeros all three motion components [740c583 EntityPlayer].
	 * Do this before minecart controls and ordinary physics: a server-only RPC
	 * gate otherwise lets the client accumulate displacement and submit it on
	 * the first wake frame. Preserve the interpolation samples so render/camera
	 * attachment also sees a stationary body. */
	if(session->sleeping) {
		glm_vec3_copy(e->pos, e->pos_old);
		glm_vec2_copy(e->orient, e->orient_old);
		glm_vec3_zero(e->vel);
		/* No walking tick ran, so nothing would decay the sneak dip and a
		 * crouch entered before sleeping would freeze into the bed camera.
		 * Beta cannot reach this state (the sleep GUI owns the input and
		 * ySize decays to nothing within the sleep), so zeroing is the
		 * faithful resting value, and it matches the branch's own intent of
		 * presenting a completely stationary body. */
		player_ysize_reset(&session->camera.ysize);
		return false;
	}

	if(local_player_follow_mounted(e, session, frame))
		return false;

	// Audit 2026-09-01 #1 (REGRESSION-TODAY, CONFIRMED): reaching this point
	// means the boat search above did NOT find an occupied boat for this
	// entity this tick -- i.e. this player is not boat-mounted right now,
	// whether that is "never mounted" or "just dismounted" (DISMOUNT site:
	// the boat loop stops matching the same tick server_local_set_boat_
	// occupancy(s, boat, false) flips boat->data.boat.occupied). Before this
	// fix, boat_rider_vel_x/z were left holding whatever the LAST mounted
	// tick wrote (or, pre-ctor-fix above, raw malloc garbage), breaking the
	// field's own "0.0 whenever not boat-mounted" contract (entity.h) and
	// transmitting a stale nonzero vel_x/vel_z over every SRPC_PLAYER_POS
	// (client_interface.c ~:3376) for a player who is provably not riding.
	e->data.local_player.boat_rider_vel_x = 0.0F;
	e->data.local_player.boat_rider_vel_z = 0.0F;

	// ---------- normal player physics ----------
	glm_vec3_copy(e->pos, e->pos_old);
	glm_vec2_copy(e->orient, e->orient_old);

	/* No |motion| < 0.005 cutoff: Beta 1.7.3 has none (EntityLiving.
	 * onLivingUpdate:583-640, EntityPlayer, EntityPlayerSP -- W1A-b). */

	struct AABB bbox;
	aabb_setsize_centered(&bbox, 0.6F, 1.0F, 0.6F);
	aabb_translate(&bbox, e->pos[0], e->pos[1] + 1.8F / 2.0F - EYE_HEIGHT, e->pos[2]);

	/* Beta Entity.onEntityUpdate:168 -> handleWaterMovement [Entity.java:
	 * 543-545]: World.handleMaterialAcceleration over the 0.6x1.8 body box
	 * (minY = feet = eye - yOffset 1.62; this physics box carries no ySize)
	 * shrunk 0.4 in Y and 0.001 on every axis -- Beta's inWater verdict AND
	 * the 0.014 flow impulse on the motion, applied HERE, before jump /
	 * moveFlying / the move, as onUpdate orders it (next-gaps-0924/W2 P3).
	 * The walk owner is the single authority for the player's push: host and
	 * split guest run this same walk, and the MP client predicts it exactly as
	 * Beta's client does; the server's body probe (server_local_player_body_
	 * in_water_for) only reads the same verdict for fire quench / fall reset.
	 * Replaces the liquid-height AABB test (a second inWater definition,
	 * docs/intent/p3-water-flow-push.md). */
	bool in_water = entity_handle_water_movement(
		e, (double)e->pos[0], (double)e->pos[1] - (double)EYE_HEIGHT,
		(double)e->pos[2], 0.6F, 1.8F);
	bool in_lava  = entity_intersection(e, &bbox, test_in_lava);

	/* Beta ground friction, computed BEFORE the move from the pre-move
	 * onGround and the block under the pre-move feet [retromcp b1.7.3
	 * EntityLiving.moveEntityWithHeading:478-485, recomputed identically at
	 * :490-497 and applied after the move]:
	 *   the friction is 0.91; on the ground it becomes the 546 x 0.1 x 0.1 x 0.1
	 *   float product, and if the block id under the feet (floor x, floor of the
	 *   box minimum Y minus one, floor z) is nonzero, that block's slipperiness
	 *   times 0.91.
	 * The 546 chain is float left-to-right (== 0.6 x 0.91 in float, bit for bit). An
	 * unreadable cell is World.getBlockId's 0. Feet = eye - EYE_HEIGHT, the same
	 * basis the ladder probe below uses; this port's physics box carries no
	 * ySize, so it IS boundingBox.minY. Unused in water/lava (their own
	 * branches, :455-477). */
	float ground_friction = 0.91F;
	if(e->on_ground) {
		ground_friction = 546.0F * 0.1F * 0.1F * 0.1F;
		struct block_data under;
		if(entity_get_block(e, (w_coord_t)floorf(e->pos[0]),
							(w_coord_t)floorf(e->pos[1] - EYE_HEIGHT) - 1,
							(w_coord_t)floorf(e->pos[2]), &under)
		   && under.type > 0)
			ground_friction = block_slipperiness(under.type) * 0.91F;
	}
	/* moveFlying's third argument: 0.02 in water (:457) and lava (:469);
	 * on land 0.1 x (0.16277136 / friction cubed) when grounded, else 0.02 (:488-489). */
	float move_accel;
	if(in_water || in_lava)
		move_accel = 0.02F;
	else if(e->on_ground)
		move_accel = 0.1F
			* (0.16277136F
			   / (ground_friction * ground_friction * ground_friction));
	else
		move_accel = 0.02F;

	float forward = 0.0F;
	float strafe  = 0.0F;
	bool jumping  = false;
	bool sneaking = false;

	if(e->data.local_player.capture_input) {
		if((frame && frame->connected && frame->down[IB_FORWARD]))  forward += 1.0F;
		if((frame && frame->connected && frame->down[IB_BACKWARD])) forward -= 1.0F;
		if((frame && frame->connected && frame->down[IB_RIGHT]))    strafe  += 1.0F;
		if((frame && frame->connected && frame->down[IB_LEFT]))     strafe  -= 1.0F;
		jumping  = (frame && frame->connected && frame->down[IB_JUMP]);
		sneaking = (frame && frame->connected && frame->down[IB_SNEAK]);
	}

	/* Beta SNEAK CAMERA DIP, step 1 of 2 [740c583 EntityPlayerSP.onLivingUpdate:79-80
	 * -- a sneaking player's ySize is raised to at least 0.2], fused with the
	 * tick-top render sample of Entity.onEntityUpdate:164. This is the last
	 * point before the movement step where this tick's sneak level is known,
	 * which is exactly where Beta puts the floor: onLivingUpdate runs it right
	 * after updatePlayerMoveState and before moveEntityWithHeading.
	 *
	 * CAMERA ONLY. The state lives on this owner's camera (one per
	 * client_session, so the host and the split guest each have their own);
	 * nothing here touches e->pos, the bounding box, or anything the server
	 * sees. player_ysize.h has the full contract and the deviation note. */
	player_ysize_pre_move(&session->camera.ysize, sneaking);

	/* EntityPlayerSP.pushOutOfBlocks, four corners, right after the ySize
	 * floor and before super.onLivingUpdate [EntityPlayerSP.onLivingUpdate:
	 * 83-87]. Client-side in Beta (EntityClientPlayerMP inherits it too), so it
	 * lives here, in the one walk both owners run: host and split guest alike.
	 * minY is the feet (this port's physics box carries no ySize). */
	local_player_push_out_of_blocks(e, -1, 1);
	local_player_push_out_of_blocks(e, -1, -1);
	local_player_push_out_of_blocks(e, 1, -1);
	local_player_push_out_of_blocks(e, 1, 1);

	// Beta 1.7.3 sneak speed (MovementInputFromOptions): moveStrafe/moveForward are scaled
	// by 0.3 BEFORE moveEntityWithHeading's max(sqrt(s^2+f^2),1) normalization. Our distf
	// normalization below matches Beta's clamp, so applying 0.3 to forward/strafe here is
	// byte-faithful -- and identical to the old int code for the non-sneak case (1.0 == 1).
	if(sneaking) {
		forward *= 0.3F;
		strafe  *= 0.3F;
	}

	/* EntityLiving.onLivingUpdate:638-640 decays the move input by 0.98 right
	 * before moveEntityWithHeading, every tick (moveForward/moveStrafing were
	 * just reloaded from movementInput by updatePlayerActionState). */
	forward *= 0.98F;
	strafe  *= 0.98F;

	/* Entity.moveFlying:570-585, float throughout: mag is the float root of the inputs' squared length
	 * (sqrtf is the same correctly-rounded value); dead zone 0.01; clamp the
	 * divisor to 1; scale BOTH inputs by accel / mag, then rotate with
	 * MathHelper.sin/cos (the 65536-entry SIN_TABLE, MathHelper.java:6-12,
	 * 56-61 == mh_sin/mh_cos) of rotationYaw times float pi over 180.
	 * Beta's frame: rotationYaw is minus orient[0] in degrees (the SAME mapping every
	 * SRPC_PLAYER_POS sends, client_interface.c) and moveStrafe is +1 for LEFT
	 * (MovementInputFromOptions:61-67; this port's strafe is +1 for right). */
	float mag = sqrtf(strafe * strafe + forward * forward);
	if(mag >= 0.01F) {
		if(mag < 1.0F)
			mag = 1.0F;
		mag = move_accel / mag;
		float s = -strafe * mag;
		float f = forward * mag;
		float yaw = -glm_deg(e->orient[0]);
		float angle = yaw * (float)M_PI / 180.0F;
		float sn = mh_sin(angle);
		float cs = mh_cos(angle);
		e->vel[0] += s * cs - f * sn;
		e->vel[2] += f * cs + s * sn;
	}

	/* EntityLiving.onLivingUpdate:625-634: while jump is held, +0.04 in
	 * water, else +0.04 in lava, else jump() (vertical motion 0.42, :659-661) on
	 * EVERY tick onGround -- Beta 1.7.3 has no jump cooldown (jumpTicks is a
	 * later version's). data.local_player.jump_ticks is no longer read. */
	if(jumping) {
		if(in_water || in_lava)
			e->vel[1] += 0.04F;
		else if(e->on_ground)
			e->vel[1] = 0.42F;
	}

	aabb_setsize_centered(&bbox, 0.6F, 1.8F, 0.6F);
	aabb_translate(&bbox, 0.0F, 1.8F / 2.0F - EYE_HEIGHT, 0.0F);

	/* No "unstuck" lift (next-gaps-0924/W1A). The upward snap added in
	 * 85feb7ba answered a spawn-inside-terrain report from a time when (a) the
	 * spawn Y was not probed and (b) the collision test froze any entity that
	 * already overlapped a box. Both are fixed at their source: the world-spawn
	 * fallback and every no-bed respawn run Beta's push-up-while-colliding
	 * loop (server_local_anti_suffocate_column, eafbcedd), and
	 * entity_intersection_threshold never clips against a box the mover is
	 * already inside (3e3e3c18) -- Beta's AxisAlignedBB.calculate*Offset rule --
	 * so an embedded body walks out. Beta has no such lift; a buried head now
	 * suffocates server-side (Entity.isEntityInsideOpaqueBlock). */

	/* Beta ladder, BEFORE moveEntity [EntityLiving.moveEntityWithHeading:
	 * 500-527], land branch only. isOnLadder (:550-555) reads the ladder at the
	 * pre-move feet cell:
	 *   motion X/Z clamped to +-0.15 and the fall distance reset to 0; downward
	 *   motion Y floored at -0.15 (a double compare); and while sneaking, any
	 *   downward motion Y is zeroed -- the hold.
	 * isSneaking is the input sneak flag while not sleeping (EntityPlayerSP:144-146);
	 * this function returned above while sleeping. fallDistance is server
	 * state here (server_local resets it on a ladder). */
	if(!in_water && !in_lava) {
		struct block_data rung;
		if(entity_get_block(e, (w_coord_t)floorf(e->pos[0]),
							(w_coord_t)floorf(e->pos[1] - EYE_HEIGHT),
							(w_coord_t)floorf(e->pos[2]), &rung)
		   && rung.type == BLOCK_LADDER) {
			const float rail = 0.15F;
			if(e->vel[0] < -rail) e->vel[0] = -rail;
			if(e->vel[0] > rail)  e->vel[0] = rail;
			if(e->vel[2] < -rail) e->vel[2] = -rail;
			if(e->vel[2] > rail)  e->vel[2] = rail;
			if((double)e->vel[1] < -0.15)
				e->vel[1] = (float)-0.15;
			if(sneaking && e->vel[1] < 0.0F)
				e->vel[1] = 0.0F;
		}
	}

	// Beta 1.7.3 sneak ledge-stop (Entity.moveEntity, on ground while sneaking): clamp the
	// horizontal move so the player can't step off an edge. Done after the unstuck nudge
	// and BEFORE the new_pos/new_vel copies + the collision move, so the step-up path below
	// sees the clamped velocity too.
	// The same pre-move snapshot is Beta's sneaking-on-ground footstep suppression, read
	// by the MP footstep block after the move [740c583: Entity.moveEntity:478].
	/* next-gaps-0924/W2 P12: Entity.moveEntity:262-268 -- the isInWeb branch
	 * runs before the sneak ledge loop (:271-296) scales the same deltas. */
	const bool web_move = entity_web_consume(e);

	bool step_suppress = sneaking && e->on_ground;
	if(step_suppress)
		entity_sneak_ledge_clamp(e, &bbox);

	/* Beta SNEAK CAMERA DIP, step 2 of 2 [740c583 Entity.moveEntity:259 --
	 * ySize is multiplied by 0.4, the FIRST statement of the move]. Placed on the
	 * movement step, after the floor above, so the per-tick result is Beta's:
	 * a held sneak sits at 0.2 x 0.4 forever and a release decays by 0.4 per
	 * tick. Camera only; the move itself is untouched. */
	player_ysize_move(&session->camera.ysize);

	vec3 new_pos, new_vel;
	glm_vec3_copy(e->pos, new_pos);
	glm_vec3_copy(e->vel, new_vel);

	/* Beta's pre-move onGround, for the step gate's grounded term below. */
	const bool was_on_ground = e->on_ground;

	bool collision_xz = false;
	for(int k = 0; k < 3; k++)
		entity_try_move(e, e->pos, e->vel, &bbox, (size_t[]){1, 0, 2}[k],
		                &collision_xz, &e->on_ground);

	/* The step-route gate, Entity.moveEntity:341 [mcb173-retromcp]:
	 *   step only when stepHeight is positive, the body counts as grounded, it is
	 *   sneak-grounded or its ySize is under 0.05, and the flat pass clipped X or Z.
	 * Grounded (:311) means onGround, or the requested dy clipped while moving down: the
	 * pre-move flag, or the flat Y pass clipped moving down -- which is
	 * exactly what entity_try_move left in e->on_ground after the Y pass (the
	 * X/Z passes never write it). Sneak-grounded (:276) is onGround while sneaking, the
	 * pre-move snapshot step_suppress already holds. ySize is this tick's value
	 * after the :259 decay (player_ysize_move above). The last term is the flat
	 * pass's horizontal clip, collision_xz. The ySize term is what makes a
	 * player who just climbed wait ~2 ticks before the next rise (0.51 ->
	 * 0.204 -> 0.0816 -> 0.0326); the sneak-grounded term lets a sneaking player -- whose ySize
	 * sits at 0.2 x 0.4 -- step at all. */
	const bool step_grounded = was_on_ground || e->on_ground;
	if(step_grounded
	   && (step_suppress || session->camera.ysize.y_size < 0.05F)
	   && collision_xz) {
		/* THE STEP ROUTE, ON THE BOX MINIMUM [Entity.moveEntity:342-396].
		 * Beta restores its saved box, as it was BEFORE the flat pass
		 * (:274, :350) -- and offsets it by exactly +stepHeight (:346), the
		 * requested x and z, then exactly -stepHeight (:390), in double.
		 * EntityLiving's stepHeight is 0.5 [EntityLiving.java:67].
		 *
		 * This port's body is a float EYE and its box is derived as
		 * eye + lo, so "+0.5F on the eye" is NOT "+0.5 on the box": the
		 * derived minimum can land an ulp short (feet 62.0 -> 62.4999962 at
		 * the binade splits), and a box an ulp inside a slab's top is stopped
		 * by it where Beta's clears it. So the route runs where Beta runs it:
		 * on a box whose minimum IS the coordinate. feet0 starts at the
		 * pre-move eye's own derived minimum (fl(eye + lo), exactly what the
		 * flat pass collided with) and the +/-0.5F offsets are then exact
		 * float sums (feet + 0.5 is representable for every feet on the
		 * 1/16 grid, and for any feet whose last bit survives the sum). The
		 * box top is minimum + 1.8F, Beta's own construction (setPosition
		 * :132, height 1.8F), for the ceiling test.
		 *
		 * Only the hand-back to the eye rounds: an accepted route that ends
		 * on a face converts with the same placement rule every landing uses
		 * (local_eye_on_feet); a route that returns to the original minimum
		 * keeps the original eye bit for bit. docs/deviations.md "Local step
		 * route on the box minimum". tests/test_local_step_height_golden.c. */
		struct AABB step_box = bbox;
		step_box.y1 = 0.0F;
		step_box.y2 = 1.8F;
		const float feet0 = new_pos[1] + bbox.y1; /* pre-move box minimum */
		vec3 step_pos = {new_pos[0], feet0, new_pos[2]};
		const float requested_dy = new_vel[1]; /* Beta's requested dy */
		bool collision = false;
		bool ground    = e->on_ground;

		new_vel[1] = LOCAL_PLAYER_STEP_HEIGHT;
		entity_try_move(e, step_pos, new_vel, &step_box, 1, &collision,
						&ground);

		new_vel[1] = 0.0F;
		entity_try_move(e, step_pos, new_vel, &step_box, 0, &collision,
						&ground);
		entity_try_move(e, step_pos, new_vel, &step_box, 2, &collision,
						&ground);

		const float feet_before_down = step_pos[1];
		new_vel[1] = -LOCAL_PLAYER_STEP_HEIGHT;
		entity_try_move(e, step_pos, new_vel, &step_box, 1, &collision,
						&ground);
		/* Beta's resolved dy after the route: the down pass's component. */
		const float step_dy = step_pos[1] - feet_before_down;

		/* Keep the step route only when its HORIZONTAL travel is strictly
		 * longer [Entity.moveEntity:398-402]:
		 *   the flat pass is restored when its squared horizontal travel is >= the route's
		 * (flat travel = the flat pass's resolved x/z, route travel the step
		 * route's). No Y term and no "ended higher" requirement. The resolved
		 * offsets are the float displacements from the pre-move position
		 * (pos_old), squared and summed in double as the decompile does; a
		 * tie keeps the flat pass. */
		const double flat_dx = (double)(e->pos[0] - e->pos_old[0]);
		const double flat_dz = (double)(e->pos[2] - e->pos_old[2]);
		const double step_dx = (double)(step_pos[0] - e->pos_old[0]);
		const double step_dz = (double)(step_pos[2] - e->pos_old[2]);
		if(!(flat_dx * flat_dx + flat_dz * flat_dz
			 >= step_dx * step_dx + step_dz * step_dz)) {
			collision_xz = collision;
			/* :415-424 -- vertically collided means the requested dy differs from
			 *   the resolved dy; onGround means that collision while moving down; and a
			 *   vertical collision zeroes motion Y.
			 * The dy compared is the REQUESTED one, not the route's down pass: a jump
			 * that steps onto a slab is NOT on the ground afterwards. X/Z
			 * motion: entity_try_move already zeroed each clipped axis
			 * (:420, :428) and left the unclipped ones as requested. */
			const bool collided_y = requested_dy != step_dy;
			e->on_ground = collided_y && requested_dy < 0.0F;
			new_vel[1] = collided_y ? 0.0F : requested_dy;
			/* Beta Entity.moveEntity:403-409 banks the accepted route's
			 * fractional box minimum into ySize. step_pos[1] IS Beta's
			 * boundingBox.minY here (the route ran on the minimum), so no
			 * recovery from the float eye is needed. Camera only; see
			 * player_ysize.h. */
			player_ysize_step(&session->camera.ysize, step_pos[1]);
			e->pos[0] = step_pos[0];
			e->pos[1] = step_pos[1] == feet0
				? new_pos[1]
				: local_eye_on_feet(step_pos[1], bbox.y1);
			e->pos[2] = step_pos[2];
			glm_vec3_copy(new_vel, e->vel);
		}
	}

	/* P12: Beta zeroed the motion before the move (:266-268); the same bytes
	 * result from zeroing here, after the moves and before the dispatch and
	 * friction that multiply it. */
	if(web_move)
		glm_vec3_zero(e->vel);

	// Beta Entity.moveEntity block-collide dispatch (soul sand scales motionX/Z by 0.4 per
	// intersected cell, cobweb isInWeb) runs at the END of the move, i.e. after the
	// collision moves above and BEFORE the friction below -- Beta's exact order. The
	// 7/8 soul-sand collision box is what puts the standing player's feet inside the
	// cell. [decompile 740c583 Entity.moveEntity + BlockSoulSand + BlockWeb]
	{
		struct AABB sbox = bbox; // relative box (already carries the eye-height offset)
		aabb_translate(&sbox, e->pos[0], e->pos[1], e->pos[2]);
		entity_soulsand_slowdown(e, &sbox, &e->vel[0], &e->vel[2]);
	}

	// MP client-side footstep (remote_session only -- in SP server_local plays
	// the identical step from the SRPC_PLAYER_POS delta, server_local.c, and
	// this gate keeps the two paths exclusive). Beta's step block runs right
	// here inside moveEntity, after the collision move: gate
	// canTriggerWalking, not sneak-grounded, and not riding (sneak-grounded = the
	// pre-move on-ground-while-sneaking snapshot above; the riding early-returns
	// never reach this point), accumulate sqrt(dx^2+dz^2)*0.6 into the
	// odometer, and on the nextStepDistance crossing with a non-air block
	// under the feet play stepSoundDir2() at 0.15 x its volume and its pitch
	// with the snow-layer-above override, at the FEET. The 2-block teleport
	// cap mirrors the SP handler's documented deviation (corrections/respawn
	// teleports must not accumulate). [740c583: Entity.moveEntity:478-499]
	if(gstate.remote_session && !step_suppress) {
		double ddx = (double)e->pos[0] - (double)e->pos_old[0];
		double ddz = (double)e->pos[2] - (double)e->pos_old[2];
		if(ddx * ddx + ddz * ddz < 4.0) {
			int bx = (int)floorf(e->pos[0]);
			int by = (int)floorf(e->pos[1] - 0.2F - EYE_HEIGHT);
			int bz = (int)floorf(e->pos[2]);
			struct block_data foot, above;
			bool has_foot = entity_get_block(e, bx, by, bz, &foot);
			bool solid = has_foot && foot.type != BLOCK_AIR;
			if(server_player_walk_step(&e->data.local_player.walk_dist,
									   &e->data.local_player.walk_next_step,
									   ddx, ddz, solid)
			   && has_foot) {
				uint8_t above_id
					= entity_get_block(e, bx, by + 1, bz, &above)
					? (uint8_t)above.type
					: (uint8_t)BLOCK_AIR;
				float vol, pitch;
				uint16_t sid = audio_footstep_params((uint8_t)foot.type,
													 above_id, &vol, &pitch);
				if(sid != AUDIO_SOUND_NONE)
					audio_play3d(sid,
								 (vec3) {e->pos[0], e->pos[1] - EYE_HEIGHT,
										 e->pos[2]},
								 vol, pitch);
			}
		}
	}

	/* Post-move drag [EntityLiving.moveEntityWithHeading]. Beta tests
	 * isInWater() FIRST (:455), then handleLavaMovement() (:466). */
	if(in_water) {
		e->vel[0] *= 0.8F;
		e->vel[2] *= 0.8F;
		e->vel[1]  = e->vel[1] * 0.8F - 0.02F;
	} else if(in_lava) {
		e->vel[0] *= 0.5F;
		e->vel[2] *= 0.5F;
		e->vel[1]  = e->vel[1] * 0.5F - 0.02F;
	} else {
		/* :528-535 -- isOnLadder() is re-read at the POST-move feet cell:
		 *   a horizontally collided body on a ladder gets motion Y 0.2; then gravity
		 *   subtracts 0.08 and motion Y is scaled by 0.98; horizontal motion is
		 *   scaled by the friction computed above. */
		struct block_data blk;
		if(collision_xz
		   && entity_get_block(e, (w_coord_t)floorf(e->pos[0]),
							   (w_coord_t)floorf(e->pos[1] - EYE_HEIGHT),
							   (w_coord_t)floorf(e->pos[2]), &blk)
		   && blk.type == BLOCK_LADDER)
			e->vel[1] = (float)0.2;
		e->vel[1] -= 0.08F;
		e->vel[1] *= 0.98F;
		e->vel[0] = (float)((double)e->vel[0] * (double)ground_friction);
		e->vel[2] = (float)((double)e->vel[2] * (double)ground_friction);
	}

	if(collision_xz && (in_lava || in_water)) {
		struct AABB tmp;
		aabb_setsize_centered(&tmp, 0.6F, 1.8F, 0.6F);
		aabb_translate(&tmp, e->pos[0] + e->vel[0],
		               e->pos[1] + e->vel[1] + 1.8F / 2.0F - 1.62F + 0.6F,
		               e->pos[2] + e->vel[2]);
		if(!entity_intersection(e, &tmp, test_in_liquid))
			e->vel[1] = 0.3F;
	}

	/* next-gaps-0924/W2 P4: the player's OWN entity-push loop, Beta
	 * EntityLiving.onLivingUpdate:641-651 right after moveEntityWithHeading
	 * (whose tail is the climb-out above), run by the walk owner against the
	 * client's mob copies -- exactly what Beta's client player does
	 * (EntityClientPlayerMP / EntityPlayerSP inherit it): for every pushable
	 * mob whose box strictly intersects the player box grown 0.2 in X and Z,
	 * the mob's applyEntityCollision with the player as argument gives the PLAYER its
	 * +(dx,dz) half [Entity.java:686, addVelocity on the argument]. P4b (Opus HIGH-1):
	 * Beta's client ALSO runs every mob copy's onLivingUpdate (only
	 * updatePlayerActionState is gated by isMultiplayerEntity, :623), whose
	 * loop has the player apply collision with the mob [:643-648] -- the player
	 * takes a SECOND push, adding the negated (mob - player) offset to itself
	 * [:685], i.e. the same vector. Its gate, the mob box grown by 0.2 strictly
	 * overlapping the player's box, is the same inequality per axis
	 * (a.max + 0.2 > b.min <=> a.max > b.min - 0.2), so both halves apply to
	 * exactly the same mobs. So did SP: EntityPlayerSP + every mob in one
	 * world. The mob's halves are the server's (entity_player_collide_with_
	 * nearby, and each mob's own loop) -- a client copy's motion is never
	 * authoritative, so it is left untouched. Mount gate Entity.java:669: the
	 * pig this player rides pushes neither way (pig_ridden + pig_rider_id ==
	 * this entity, the mount-follow scan's own test). Boxes are Beta doubles
	 * from the copies' float pos/size; the candidate order is the client
	 * dict's (Beta's client chunk lists are not networked), a last-bit
	 * summation-order difference on an already float-predicted motion
	 * (docs/intent/p4-entity-push.md). */
	{
		const double ex = (double)0.2F;
		const double px = (double)e->pos[0], pz = (double)e->pos[2];
		const double feet = (double)e->pos[1] - (double)EYE_HEIGHT;
		const double half = (double)(0.6F / 2.0F);
		const double min_x = px - half - ex, max_x = px + half + ex;
		const double min_z = pz - half - ex, max_z = pz + half + ex;
		const double min_y = feet, max_y = feet + (double)1.8F;
		dict_entity_it_t it;
		dict_entity_it(it, world_view_entities());
		while(!dict_entity_end_p(it)) {
			struct entity* m = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(!m || m == e || m->type != ENTITY_MONSTER
			   || m->delay_destroy == 0)
				continue;
			if(m->data.monster.id == MONSTER_PIG && m->data.monster.pig_ridden
			   && m->data.monster.pig_rider_id == e->id)
				continue; /* Entity.java:669, both directions */
			const double mh = (double)(m->width / 2.0F);
			const double mx = (double)m->pos[0], mz = (double)m->pos[2];
			const double my0 = (double)m->pos[1] - (double)m->y_offset
				+ (double)m->y_size;
			const double my1 = my0 + (double)m->height;
			if(!(mx + mh > min_x && mx - mh < max_x && my1 > min_y
				 && my0 < max_y && mz + mh > min_z && mz - mh < max_z))
				continue;
			double dx, dz;
			if(!entity_collision_impulse(mx, mz, px, pz, &dx, &dz))
				continue;
			/* own loop: the argument (the player) adds (+dx, +dz) -- dx = player -
			 * mob points away from the mob [Entity.java:686]. */
			e->vel[0] = (float)((double)e->vel[0] + dx);
			e->vel[2] = (float)((double)e->vel[2] + dz);
			/* the mob copy's loop: the player itself adds the negated offset, where
			 * offset = mob - player = -dx [Entity.java:685] -- the same vector,
			 * as a second float add (two addVelocity calls in Beta). */
			e->vel[0] = (float)((double)e->vel[0] + dx);
			e->vel[2] = (float)((double)e->vel[2] + dz);
		}
	}

	// update client-side oxygen bar
	if(session->in_water) {
		session->oxygen--;
		// water_demo showcase: hold the air bar near full so the Beta bubble HUD
		// stays on-screen for screenshots (normal gameplay drains it fully).
		if(gstate.water_demo && session->oxygen < MAX_OXYGEN - 32 * 3)
			session->oxygen = MAX_OXYGEN - 32 * 3;
	} else session->oxygen = MAX_OXYGEN;

	return false;
}
bool entity_local_player_block_collide(vec3 pos, struct block_info* blk_info) {
	assert(pos && blk_info);

	struct AABB bbox;
	aabb_setsize_centered(&bbox, 0.6F, 1.8F, 0.6F);
	aabb_translate(&bbox, pos[0], 1.8F / 2.0F - EYE_HEIGHT + pos[1], pos[2]);

	return entity_block_aabb_test(&bbox, blk_info);
}

static size_t getBoundingBox_local_player(
	const struct entity* e, struct AABB* out) {
	assert(e && out);
	aabb_setsize_centered(out, 0.6F, 1.8F, 0.6F);
	aabb_translate(
		out, e->pos[0], e->pos[1] + 1.8F / 2.0F - EYE_HEIGHT, e->pos[2]);
	return 1;
}

void entity_local_player(uint32_t id, struct entity* e, struct world* w) {
	assert(e && w);

	e->name = "Local Player";
	e->id = id;
	e->tick_server = NULL;
	e->tick_client = entity_tick;
	e->render = NULL;
	e->teleport = entity_default_teleport;
	e->type = ENTITY_LOCAL_PLAYER;
	e->getBoundingBox = getBoundingBox_local_player;
	e->data.local_player.capture_input = false;
    e->leftClickText = NULL;
	e->onLeftClick = NULL;
    e->rightClickText = NULL;
	e->onRightClick = NULL;

	entity_default_init(e, false, w);
	e->data.local_player.jump_ticks = 0;
	// Beta Entity ctor: distanceWalkedModified starts at 0, nextStepDistance at 1
	// (the MP footstep odometer; SP never reads these).
	e->data.local_player.walk_dist = 0.0F;
	e->data.local_player.walk_next_step = 1;
	// Audit 2026-09-01 #1 (REGRESSION-TODAY, CONFIRMED): the caller
	// (client_interface.c CRPC_SPAWN_WORLD_RESET) allocates this entity with
	// plain malloc, not calloc -- entity.h's `union entity_data` is never
	// zeroed, so an unset field here is genuinely indeterminate stack/heap
	// garbage, not a benign 0. Every OTHER local_player field above this
	// line is explicitly set; boat_rider_vel_x/z were the two left out.
	// This field's own doc comment (entity.h) already promises "0.0 (no
	// input) whenever not boat-mounted" -- honor that from construction so
	// a player who never once mounts a boat still transmits a deterministic
	// 0 over SRPC_PLAYER_POS (client_interface.c ~:3376), not whatever the
	// allocator handed back.
	e->data.local_player.boat_rider_vel_x = 0.0F;
	e->data.local_player.boat_rider_vel_z = 0.0F;
}
