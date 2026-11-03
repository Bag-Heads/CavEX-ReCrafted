/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include "../../block/blocks.h"
#include "../../chunk.h" // chunk_guard_hits/_first: the MEM1 red-zone dials on the F3 row
#include "../../chunk_mesher.h"
#include "../../daytime.h"
#include "../../entity/combat.h" // PLAYER_EYE_HEIGHT for client fallback feet
#include "../../entity/pathfinding.h" // FH-13: pathfinding_perf_counters (F3 row)
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gfx_util.h"
#include "../../graphics/gui_util.h"
#include "chat_hud.h"
#include "achievement_notice.h"
#include "../../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../../graphics/render_model.h"
#include "../../graphics/render_weather.h"
#include "../../item/item_nbt.h"
#include "../../item/items.h" // direct include (was transitive via blocks.h pre-split)
#include "../../lighting.h"
#include "../../network/client_world_payload.h"
#include "../../network/server_interface.h"
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/server_world.h"
#include "../../particle.h"
#include "../../platform/audio.h" // S0: mining hit ticks + the F3 audio row
#include "../../platform/audio_music.h" // S3: the F3 music row
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "../../platform/mem2.h"
#include "../../stack.h"
#include "../../world.h"
#include "../attack_assist.h" // soft-snap melee assist: crosshair tint
#include "../autopilot.h"
#include "../game_state.h"
#include "../mp_arm_swing.h" // card m1: Packet18 arm-swing call sites
#include "hud_survival_row.h" // Batch J HUD hearts/armor/air row, survival-only (2026-09-03)
#include "local_pause.h" // review 28 M1: the single-view pause is drawn OVER this HUD
#include "../dig_start.h" // R1d: Beta clickBlock dig-start dedup + start_seq
#include "../../platform/hw_telemetry.h" // split PvP: peer-attack observer line

/* R1d: process-monotonic dig-start id shared by every local slot (host, split
 * guest): each logical Packet14-status-0 carries a fresh id, so the local
 * server can drop a duplicate without ever dropping a real re-press. */
static uint32_t s_dig_start_seq;

#include <malloc.h>

#ifdef PLATFORM_WII
#include <ogc/system.h>
#endif

static void screen_ingame_reset(struct screen* s, int width, int height) {
	input_pointer_enable(false);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = true;
}

static bool screen_ingame_try_pause(void) {
	if(gstate.remote_session) {
		screen_set(&screen_pause);
		return true;
	}
	struct server_rpc toggle = {
		.type = SRPC_TOGGLE_PAUSE,
	};
	/* TOGGLE is reliable only while the bounded ordinary-action queue admits it.
	 * Publish the optimistic client state after admission, so a saturated queue
	 * cannot invert client/server pause ownership on the next HOME press. */
	if(!svin_rpc_send(&toggle))
		return false;
	gstate.paused = true;
	screen_set(&screen_pause);
	return true;
}

static bool screen_ingame_gate_sleep_actions_owned(struct client_session* p) {
	if(!p->sleeping)
		return false;
	/* A dig can be active on the frame the sleep-state RPC arrives. Drop the
	 * optimistic client half; the authority rejects every queued world action
	 * while asleep. Camera look and HOME/pause remain controller-usable. */
	p->digging.active = false;
	return true;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Host-shaped wrapper kept for screen_ingame_test_gate_sleep_actions below --
 * the action path itself now calls the _owned form with its own session, so
 * this has no production caller. */
static bool screen_ingame_gate_sleep_actions(void) {
	return screen_ingame_gate_sleep_actions_owned(&gstate.players[0]);
}
#endif

/* Beta Minecraft.runTick:972-975 + displayGuiScreen:421-425 [740c583]: each
 * tick with NO screen open and thePlayer.health <= 0, the null screen
 * substitutes to GuiGameOver. screen_ingame active IS "no screen open" here;
 * a container screen stays up over a dead player until the player closes it,
 * exactly like Beta (the trigger requires currentScreen == null). Gated on a
 * settled world so a mid-switch frame can never flash the death screen. */
static bool screen_ingame_death_trigger(void) {
	if(gstate.players[0].local_player && gstate.players[0].local_player->health <= 0
	   && gstate.world_loaded && !gstate.world_switch_pending) {
		screen_set(&screen_death);
		return true;
	}
	return false;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool screen_ingame_test_try_pause(void) {
	return screen_ingame_try_pause();
}

bool screen_ingame_test_gate_sleep_actions(void) {
	return screen_ingame_gate_sleep_actions();
}
#endif

void screen_ingame_render3D(struct screen* s, mat4 view) {
	screen_ingame_render3D_owned(&gstate.players[0], view);
}

/* Split PvP proof (split_play_2m / split_actions_6m): one observer line per
 * local-split melee swing at the OTHER pane's body, with a per-slot running
 * count. This is the CLIENT half ("the pick selected the peer and the attack
 * RPC left this slot"); the landed hit is the peer's act_health drop in the
 * same run's heartbeats. Clicks are discrete presses, so the rate is bounded
 * by the player's own attack cadence. */
static uint32_t s_peer_attacks[2];
static void screen_ingame_note_peer_attack(unsigned slot, uint32_t target) {
	if(slot >= 2) return;
	if(s_peer_attacks[slot] < UINT32_MAX) s_peer_attacks[slot]++;
	char line[96];
	snprintf(line, sizeof(line),
		"split_peer_attack slot=%u target=%u count=%u\n", slot,
		(unsigned)target, (unsigned)s_peer_attacks[slot]);
	fputs(line, stderr);
	(void)hw_telemetry_emit_observer(line);
}

void screen_ingame_render3D_owned(struct client_session* owner, mat4 view) {
	
	if (gstate.world_loaded && owner->camera_hit.entity_hit) {
    struct entity *e = *dict_entity_get(world_view_entities(),
                                        owner->camera_hit.entity_id);
		if (e) {
			if (e->type != 0 && e->type != 1){
			gfx_blending(MODE_BLEND);
			gfx_alpha_test(false);

			gutil_entity_selection(view, e);

			gfx_blending(MODE_OFF);
			gfx_alpha_test(true);
			}
		}
	}

	if(gstate.world_loaded && owner->camera_hit.hit) {
		struct block_data blk
			= world_get_block(world_view_current(), owner->camera_hit.x,
							  owner->camera_hit.y, owner->camera_hit.z);

		if(owner->digging.active)
			render_block_cracks_owned(owner, &blk, view, owner->camera_hit.x,
								owner->camera_hit.y, owner->camera_hit.z);

		gfx_blending(MODE_BLEND);
		gfx_alpha_test(false);

		gutil_block_selection(view,
							  &(struct block_info) {
								  .block = &blk,
								  .x = owner->camera_hit.x,
								  .y = owner->camera_hit.y,
								  .z = owner->camera_hit.z,
							  });

		gfx_blending(MODE_OFF);
		gfx_alpha_test(true);
	}

	

	float place_lerp = 0.0F;
	size_t slot = inventory_get_hotbar(
		windowc_get_latest(owner->windows[WINDOWC_INVENTORY]));

	float dig_lerp
		= time_diff_s(owner->held_item_animation.punch.start, time_get())
		/ 0.4F;

	if(owner->held_item_animation.punch.place)
		place_lerp = 1.0F - glm_clamp(dig_lerp * 4.0F, 0, 1);

	if(dig_lerp >= 1.0F)
		dig_lerp = 0.0F;
	/* An equip-only dip (a true sendUseItem) re-raises the item but does not
	 * swing the arm [Minecraft.java:834-838]. */
	if(owner->held_item_animation.punch.equip_only)
		dig_lerp = 0.0F;

	float swing_lerp
		= time_diff_s(owner->held_item_animation.switch_item.start, time_get())
		/ 0.3F;

	if(swing_lerp < 0.5F)
		slot = owner->held_item_animation.switch_item.old_slot;

	if(swing_lerp >= 1.0F)
		swing_lerp = 0.0F;

	float sinHalfCircle = sinf(dig_lerp * GLM_PI);
	float sqrtLerpPI = sqrtf(dig_lerp) * GLM_PI;
	float sinHalfCircleWeird = sinf(glm_pow2(dig_lerp) * GLM_PI);

	struct block_data in_block
		= world_get_block(world_view_current(), floorf(owner->camera.x),
						  floorf(owner->camera.y), floorf(owner->camera.z));
	uint8_t light = (in_block.torch_light << 4) | in_block.sky_light;

	gfx_depth_range(0.0F, 0.1F);

	mat4 model;
	struct item_data item;

	if(inventory_get_slot(windowc_get_latest(owner->windows[WINDOWC_INVENTORY]),
						  slot + INVENTORY_SLOT_HOTBAR, &item)
	   && item_get(&item)) {
		if(item.id == ITEM_MAP) {
			/* ItemRenderer.java:154-220 replaces the generic held-item pose.
			 * Camera pitch is polar radians here; Beta uses degrees down from
			 * horizontal. Resolve swing/equip from THIS owner's timing layer. */
			glm_mat4_identity(model);
			render_item_update_light(light);
			float equip = glm_clamp(1.0F - place_lerp
				- (2.0F / 3.0F) * sinf(swing_lerp * GLM_PI), 0, 1);
			render_item_map(item_get(&item), &item, model, false,
				R_ITEM_ENV_FIRSTPERSON, owner, glm_deg(owner->camera.ry) - 90.0F,
				dig_lerp, equip);
		} else {
			glm_translate_make(model,
							   (vec3) {0.56F - sinf(sqrtLerpPI) * 0.4F,
									   -0.52F + sinf(sqrtLerpPI * 2.0F) * 0.2F
										   - 0.6F * place_lerp
										   - 0.4F * sinf(swing_lerp * GLM_PI),
									   -0.72F - sinHalfCircle * 0.2F});
			glm_rotate_y(model, glm_rad(45.0F), model);
			glm_rotate_y(model, glm_rad(-sinHalfCircleWeird * 20.0F), model);
			#ifdef GFX_3D_ELEMENTS
			glm_rotate_z(model, glm_rad(-sinf(sqrtLerpPI) * 20.0F), model);
			#endif
			glm_rotate_x(model, glm_rad(-sinf(sqrtLerpPI) * 80.0F), model);

			glm_scale_uni(model, 0.4F);
			glm_translate(model, (vec3) {-0.5F, -0.5F, -0.5F});
			render_item_update_light(light);
			items[item.id]->renderItem(item_get(&item), &item, model, false,
									   R_ITEM_ENV_FIRSTPERSON);
		}
	} else {
		glm_translate_make(model,
						   (vec3) {0.64F - sinf(sqrtLerpPI) * 0.3F,
								   -0.6F + sinf(sqrtLerpPI * 2.0F) * 0.4F
									   - 0.4F * sinf(swing_lerp * GLM_PI),
								   -0.72F - sinHalfCircle * 0.4F});
		glm_rotate_y(model, glm_rad(45.0F), model);
		glm_rotate_y(model, glm_rad(sinf(sqrtLerpPI) * 70.0F), model);
		glm_rotate_z(model, glm_rad(-sinHalfCircleWeird * 20.0F), model);

		gfx_lighting(false);
		gfx_bind_texture(&texture_mob_char);

		glm_translate(model, (vec3) {-1.0F, 3.6F, 3.5F});
		glm_rotate_z(model, glm_rad(120.0F), model);
		glm_rotate_x(model, glm_rad(200.0F), model);
		glm_rotate_y(model, glm_rad(-135.0F), model);
		glm_translate(model, (vec3) {5.6F, 0.0F, 0.0F});

		glm_translate(model,
					  (vec3) {-5.0F / 16.0F, 2.0F / 16.0F, 0.0F / 16.0F});
		glm_scale_uni(model, 1.0F / 16.0F);
		glm_translate(model, (vec3) {-3.0F, -2.0F, -2.0F});

		// TODO: depth fix in inventory
		render_model_box(model, (vec3) {2, 12, 2}, (vec3) {2, 0, 2},
						 (vec3) {180.0F, 0, 0}, (ivec2) {44, 20},
						 (ivec3) {4, 4, 12}, 0.0F, false,
						 gfx_lookup_light(light));
	}

	gfx_depth_range(0.0F, 1.0F);
}

/* S1-b: the in-game ACTION path, owner-routed (Astra call #6 ruling item 7).
 *
 * `p` is the acting session and `slot` its transport identity. Slot 0 is the
 * HOST: every statement below is the pre-S1-b one, in the same order -- the
 * merged-mask edges (input_pressed/_held/_released, autopilot and agent hooks
 * intact), the autopilot place pulse, and svin_rpc_send, which is exactly
 * source {slot 0, generation 0}. Slot 1 is the local split-screen GUEST: it
 * reads its OWN latched controller frame and sources every RPC
 * {slot 1, players[1].local_generation}, the same envelope the owner-local
 * menu path already uses (main.c local_container_send).
 *
 * Host-only on purpose: the death screen, the global pause, the inventory
 * screen and the chat composer all mutate GLOBAL client state (screen_set /
 * gstate.paused). The guest reaches its own equivalents through the owner-local
 * menu host and pause in main.c; a guest button must never retarget the host's
 * screen. Riding stays fail-closed for the guest (see the right-click branch).
 */
static bool act_pressed(unsigned slot, const struct input_player_frame* frame,
						enum input_button b) {
	return slot == 0 ? input_pressed(b) : input_frame_pressed(frame, b);
}

static bool act_held(unsigned slot, const struct input_player_frame* frame,
					 enum input_button b) {
	return slot == 0 ? input_held(b) : input_frame_held(frame, b);
}

static bool act_released(unsigned slot, const struct input_player_frame* frame,
						 enum input_button b) {
	return slot == 0 ? input_released(b) : input_frame_released(frame, b);
}

static bool act_send(unsigned slot, const struct client_session* p,
					 struct server_rpc* call) {
	if(slot == 0)
		return svin_rpc_send(call);
	return svin_rpc_send_from(
		(struct server_rpc_source) {.slot = (uint8_t)slot,
									.generation = p->local_generation},
		call);
}

/* Card m1 (docs/intent/m1-mp-arm-swing.md): the multiplayer arm swing.
 * Beta sends Packet18Animation(self, 1) only from EntityClientPlayerMP
 * [EntityClientPlayerMP.java:118-121] -- one per client, the HOST, and only in
 * a remote session (EntityPlayerSP.swingItem sends nothing, so a local world
 * and the local split guest never emit it). The call sites and the Beta
 * cadence live in game/mp_arm_swing.h; the local first-person punch animation
 * (held_item_animation) is untouched. */
static struct mp_swing_clock s_mp_swing;
static ptime_t s_mp_swing_epoch;
static bool s_mp_swing_epoch_set;

static int64_t mp_swing_now_ms(void) {
	ptime_t now = time_get();
	if(!s_mp_swing_epoch_set) {
		s_mp_swing_epoch = now;
		s_mp_swing_epoch_set = true;
	}
	return (int64_t)time_diff_ms(s_mp_swing_epoch, now);
}

static bool mp_swing_active(unsigned slot) {
	return slot == 0 && gstate.remote_session;
}

static void mp_swing_send(void) {
	(void)svin_rpc_send(&(struct server_rpc) {.type = SRPC_ARM_SWING});
}

static uint8_t mp_swing_world_block(void* ctx, int x, int y, int z) {
	(void)ctx;
	if(y < 0 || y >= WORLD_HEIGHT)
		return 0; /* World.getBlockId outside the column */
	return world_get_block(world_view_current(), x, y, z).type;
}

static uint8_t mp_swing_world_meta(void* ctx, int x, int y, int z) {
	(void)ctx;
	if(y < 0 || y >= WORLD_HEIGHT)
		return 0;
	return world_get_block(world_view_current(), x, y, z).metadata;
}

/* The left-click action this engine emits (Packet7 attack or a fresh
 * Packet14 status 0) always rides a Beta clickMouse(0), which swings first.
 * If the cadence did not already swing this frame, swing now. */
static void mp_swing_before_left_action(unsigned slot, bool* swung) {
	if(!mp_swing_active(slot) || *swung)
		return;
	mp_swing_left_click_forced(&s_mp_swing, mp_swing_now_ms());
	mp_swing_send();
	*swung = true;
}

static void screen_ingame_update_slot(struct client_session* p, unsigned slot,
									  const struct input_player_frame* frame,
									  float dt) {
	/* b1-chat-opener-0919 slice 2 (Astra ruling 3 D3): claim THIS frame for
	 * the Wii Remote sneak toggle, BEFORE the two early returns below, so the
	 * death and bed clears are reported on the very frame they happen. A frame
	 * this call does not reach is a frame another screen owned: the toggle is
	 * preserved across it, its input is suppressed, and the way back costs a
	 * neutral frame (Remote 1 is both the menu Close and the sneak button).
	 * On every platform but the Wii this is a no-op and sneak stays Beta's
	 * hold. */
	input_sneak_gameplay_tick(slot,
							  p->local_player && p->local_player->health <= 0,
							  p->sleeping);

	/* Death outranks everything, including the sleep gate: a lethal hit
	 * always woke the server side first, and Beta's runTick check sits at
	 * the same top-of-tick position. */
	if(slot == 0 && screen_ingame_death_trigger())
		return;

	if(screen_ingame_gate_sleep_actions_owned(p)) {
		if(slot == 0 && input_pressed(IB_HOME)) {
			/* MP-BED: online, the in-bed screen is Beta's GuiSleepMP
			 * (Minecraft.java:963-964), whose Esc key and "Leave Bed" button
			 * both send Packet19EntityAction state 3 (GuiSleepMP.java:16-18,
			 * 36-38, 45-49). HOME is this port's Esc. The request rides the
			 * next SRPC_PLAYER_STATE (client_interface.c); the server's wake
			 * (Packet18 animate 3) clears the bed state. Local play keeps
			 * the pause, as single-player Beta has no leave-bed control. */
			if(gstate.remote_session && slot == 0)
				p->player_sync.leave_bed_seq++;
			else
				(void)screen_ingame_try_pause();
		}
		return;
	}

	/* Card m1: Beta's left-click driver (Minecraft.runTick -> clickMouse(0)),
	 * run once per frame BEFORE any left-click action so its swing precedes
	 * that action's Packet7/Packet14 on the wire, exactly as clickMouse(0)
	 * calls swingItem first [Minecraft.java:789-791]. */
	bool mp_swung_left = false;
	if(mp_swing_active(slot)
	   && mp_swing_left_click(&s_mp_swing, mp_swing_now_ms(),
							  act_pressed(slot, frame, IB_ACTION1),
							  act_held(slot, frame, IB_ACTION1),
							  !p->camera_hit.hit && !p->camera_hit.entity_hit)) {
		mp_swing_send();
		mp_swung_left = true;
	}

	// left click interaction
	/* Soft-snap melee assist (docs/deviations.md): attack_pick sets
	 * assist_hit only on a complete miss, so the assisted id is the attack
	 * target exactly when there is no direct entity and no block. Only this
	 * ATTACK path reads it -- use/place/mine/bow keep the direct pick. */
	bool assisted_attack = !p->camera_hit.entity_hit && !p->camera_hit.hit
	    && p->camera_hit.assist_hit;
	if ((p->camera_hit.entity_hit || assisted_attack)
	    && act_pressed(slot, frame, IB_ACTION1)
	    && !p->digging.active)
	{
	    struct entity **ptr = dict_entity_get(
	        world_view_entities(),
	        assisted_attack ? p->camera_hit.assist_entity_id
	                        : p->camera_hit.entity_id
	    );
	    if (ptr) {
	        struct entity *e = *ptr;
	        /* Both local bodies have no legacy click callback. A peer hit
	         * still uses the acting slot RPC and server validation. */
	        if (e && (e->onLeftClick
	            || (slot < 2 && e == gstate.players[1 - slot].local_player))) {
	            /* S1-b: every onLeftClick implementation -- mob/remote player
	             * (entity_monster.c), painting, boat, minecart -- puts the SAME
	             * thing on the wire: svin_rpc_send{SRPC_ENTITY_ATTACK, e->id},
	             * i.e. hard-coded source slot 0. Calling it for the guest would
	             * attribute the hit to the HOST's actor (wrong damage owner,
	             * wrong aggro identity), so the guest emits that one RPC here
	             * with its own source instead. What the guest gives up is the
	             * boat/minecart CLIENT-SIDE shake prediction those two
	             * callbacks also do: presentation only, and the server's
	             * authoritative reply still drives the real state. Documented
	             * deviation -- parametrizing the entity callbacks is a server-
	             * side edit this lane does not own. */
	            mp_swing_before_left_action(slot, &mp_swung_left);
	            if(slot == 0 && e->onLeftClick) {
	                e->onLeftClick(e);
	            } else {
	                act_send(slot, p, &(struct server_rpc) {
	                    .type = SRPC_ENTITY_ATTACK,
	                    .payload.entity_attack.entity_id = e->id,
	                });
	            }
	            if(slot < 2 && e == gstate.players[1 - slot].local_player)
	                screen_ingame_note_peer_attack(slot, e->id);
	            /* Minecraft.clickMouse(0) swings first, empty hand or not
	             * [Minecraft.java:789-791]. */
	            held_anim_swing(&p->held_item_animation, time_get(), false);
	            return;
	        }
	    }
	}

	// right click interaction met entity via dict_entity_get
	if (p->camera_hit.entity_hit
	    && act_pressed(slot, frame, IB_ACTION2)
	    && !p->digging.active)
	{
	    struct entity **ptr = dict_entity_get(
	        world_view_entities(),
	        p->camera_hit.entity_id
	    );
	    if (ptr) {
	        struct entity *e = *ptr;
	        if (e) {
	            // M4 S1: server-authoritative USE-on-entity (Beta Minecraft.clickMouse
	            // ENTITY branch -> PlayerControllerMP.interactWithEntity ->
	            // Packet7UseEntity flag 0). Sent for EVERY targeted entity, mirroring
	            // SRPC_ENTITY_ATTACK; the server validates + dispatches (cow milking,
	            // sheep shearing, dye). The client-local e->onRightClick authors the
	            // minecart ride intent. F3: minecarts now use SRPC_MINECART_RIDE_INTENT,
	            // not SRPC_ENTITY_USE. Beta's trailing sendUseItem after the entity
	            // click IS ported below (2026-10-30, Bucket C parity: rod, food, egg,
	            // snowball, boat, bow), EXCEPT for the empty bucket: re-emptying a
	            // fresh milk bucket (or filling from water behind the mob) stays
	            // the hostile misfeature playability carve-out #2 removes (M4
	            // notes). [Minecraft.java:788-838]
	            //
	            // minecart-slice: the type-0 (rideable) exclusion above is now
	            // type-SPECIFIC. Type 1 (chest) / 2 (furnace) carts never mount,
	            // so their interact() is entirely server-authoritative like
	            // every other entity here -- entity_minecart.c's onRightClick
	            // early-returns for them before touching gstate.players[0].ride, so the
	            // two paths stay mutually exclusive per cart, never both-fired.
	            // boat-slice: a boat has no gstate.players[0].ride-style client-predicted
	            // mount path either (entity_boat.c leaves onRightClick NULL) --
	            // its mount/dismount is entirely carried BY this SRPC_ENTITY_USE
	            // send (server_local.c's SRPC_ENTITY_USE ENTITY_BOAT branch is
	            // the whole mechanism), so ENTITY_BOAT must stay on the
	            // send-it side of this gate; only the rideable (type-0) cart is
	            // excluded, since IT alone has the dedicated
	            // SRPC_MINECART_RIDE_INTENT path instead.
	            /* Online (remote session) a rideable cart is used exactly like
	             * a boat: ONE Packet7 use-entity and nothing predicted locally
	             * -- Beta's EntityMinecart.interact toggles mount/dismount on
	             * the server, which answers with Packet39 (CRPC_ATTACH_ENTITY).
	             * The F3 ride-intent protocol (the cart's onRightClick below)
	             * is server_local-only; the remote provider drops it, so it
	             * must not run online. docs/mp_mounts_design_2026-09-11.md */
	            bool cart_online = gstate.remote_session
	                && e->type == ENTITY_MINECART
	                && e->data.minecart.minecart_type == 0;
	            if(cart_online || e->type != ENTITY_MINECART
	               || e->data.minecart.minecart_type != 0) {
	                // Adversarial-review finding #6: desired_occupied is the
	                // state THIS press wants, computed once from the boat's own
	                // (not-yet-corrected) local occupied flag -- not a toggle
	                // read at send time twice for two rapid presses. See the
	                // field's doc comment (server_interface.h) and the server
	                // handler (server_local.c SRPC_ENTITY_USE ENTITY_BOAT) for
	                // why this makes a double-press idempotent instead of a
	                // flip-back. Meaningless (and ignored) for any other target.
	                bool desired_occupied = e->type == ENTITY_BOAT
	                    ? !e->data.boat.occupied
	                    : cart_online ? !e->data.minecart.occupied : false;
	                act_send(slot, p, &(struct server_rpc) {
	                    .type = SRPC_ENTITY_USE,
	                    .payload.entity_use.entity_id = e->id,
	                    .payload.entity_use.desired_occupied = desired_occupied,
	                });
	            }
	            /* S1-b: the ride-intent callback is HOST-only. The one
	             * implementation (entity_minecart.c onRightClick) writes
	             * gstate.players[0].ride outright, so running it for the guest
	             * would author a mount on the HOST's session; and guest riding
	             * is fail-closed until S1-a2's mount work is proven (Astra call
	             * #6 item 4). A guest right-click on a rideable type-0 cart is
	             * therefore a no-op on both halves -- it is excluded from the
	             * SRPC_ENTITY_USE send above too, exactly as offline host play
	             * excludes it. */
	            if (slot == 0 && e->onRightClick && !cart_online)
	                e->onRightClick(e);
	            /* interactWithEntity itself never swings (saddle, shears,
	             * dye) [Minecraft.java:804-806; PlayerControllerMP.java:
	             * 165-169; EntityPlayer.java:457-469]. But var2 stays true in
	             * the ENTITY branch, so clickMouse(1)'s trailing sendUseItem
	             * still runs with the (post-interact) held stack
	             * [Minecraft.java:834-838]: Packet15(-1,-1,-1,255) after the
	             * Packet7, the rod's swing + Packet18 after that, and a true
	             * use (food, egg, snowball, boat) re-equips without a swing.
	             * The client cannot see what interact did to the stack; it
	             * uses the stack as clicked. A consumed saddle/dye makes the
	             * server's Packet15(-1) a no-op (NetServerHandler: null item
	             * returns). KEPT CARVE-OUT (M4 S1, playability #2): the EMPTY
	             * BUCKET gets no follow-up -- in Beta the cow's interact turns
	             * it into milk and the trailing use instantly pours it out. */
	            struct item_data ent_held;
	            if(inventory_get_hotbar_item(
	                   windowc_get_latest(p->windows[WINDOWC_INVENTORY]),
	                   &ent_held)
	               && ent_held.count > 0 && ent_held.id != ITEM_BUCKET) {
	                act_send(slot, p, &(struct server_rpc) {
	                    .type = SRPC_USE_ITEM,
	                    .payload.use_item.charge = 1.0F,
	                    .payload.use_item.held_item = ent_held,
	                    .payload.use_item.has_held_item = true,
	                });
	                if(mp_swing_after_use_item(true, ent_held.id)) {
	                    if(mp_swing_active(slot))
	                        mp_swing_send();
	                    held_anim_swing(&p->held_item_animation, time_get(),
	                                    false);
	                } else if(ent_held.id != ITEM_BOW
	                          && (item_has_air_use(ent_held.id)
	                              || item_is_food(ent_held.id))) {
	                    held_anim_equip_dip(&p->held_item_animation,
	                                        time_get());
	                }
	            }
	            return;
	        }
	    }
	}


// block place (real USE press, or a deterministic aim-autopilot pulse -- consumed only when a
// block is under the crosshair so it is never lost to a no-target frame). Bitwise '|' (not '||')
// so a real press coinciding with a pending pulse still CONSUMES the pulse (one place, not two);
// autopilot_take_place_pulse() only decrements when a pulse is actually pending.
// S1-b: the pulse is the HOST's headless-capture rig (one global autopilot,
// player 0); the guest never consumes it, so its place is the plain edge.
	bool place_pulse_now = false;
	if(slot == 0 && p->camera_hit.hit && !p->digging.active)
		place_pulse_now = autopilot_take_place_pulse(); /* same rule as before: consumed only with a target */
	if(p->camera_hit.hit && !p->digging.active
	   && (act_pressed(slot, frame, IB_ACTION2) | place_pulse_now)) {
		if(place_pulse_now) {
			gstate.autopilot.pulse_count++;
			gstate.autopilot.pulse_x = p->camera_hit.x;
			gstate.autopilot.pulse_y = p->camera_hit.y;
			gstate.autopilot.pulse_z = p->camera_hit.z;
			gstate.autopilot.pulse_block
				= world_get_block(world_view_current(), p->camera_hit.x,
								  p->camera_hit.y,
								  p->camera_hit.z).type;
		}
		struct item_data held_for_place = {0};
		bool has_held_for_place = inventory_get_hotbar_item(
			windowc_get_latest(p->windows[WINDOWC_INVENTORY]), &held_for_place);
		/* Minecraft.clickMouse(1) swings iff sendPlaceBlock returned true
		 * [Minecraft.java:817-820; PlayerController.java:75-78]: the target's
		 * blockActivated (lever, button, door, trapdoor, chest, furnace,
		 * workbench, bed, note block, repeater, cake, dispenser, a full
		 * jukebox) or the held item's onItemUse. Beta calls swingItem in
		 * single player too (EntityPlayer.swingItem); only the Packet18 is
		 * multiplayer-only. Read BEFORE the RPC so the predicate sees the
		 * world the click saw. */
		struct mp_swing_world place_w = {.ctx = NULL,
			.block = mp_swing_world_block,
			.meta = mp_swing_world_meta};
		uint8_t place_target = mp_swing_world_block(NULL, p->camera_hit.x,
			p->camera_hit.y, p->camera_hit.z);
		bool place_activated = place_target > 0
			&& mp_swing_block_activated(place_target,
				mp_swing_world_meta(NULL, p->camera_hit.x, p->camera_hit.y,
					p->camera_hit.z));
		/* Beta rotationYaw, the SRPC_PLAYER_POS convention
		 * (client_interface.c: rx = -glm_deg(camera.rx)). */
		bool place_swings = mp_swing_after_place(&place_w, p->camera_hit.x,
			p->camera_hit.y, p->camera_hit.z,
			mp_swing_beta_face(p->camera_hit.side), has_held_for_place,
			held_for_place.id, held_for_place.durability,
			held_for_place.count, -glm_deg(p->camera.rx));
		/* sendPlaceBlock false -> the trailing sendUseItem runs at the block
		 * too [Minecraft.java:834-838]; of all onItemRightClick only the
		 * fishing rod swings [ItemFishingRod.java:22,29]. */
		bool place_rod_swings = !place_swings
			&& mp_swing_after_use_item(has_held_for_place, held_for_place.id);
		act_send(slot, p, &(struct server_rpc) {
			.type = SRPC_BLOCK_PLACE,
			.payload.block_place.x = p->camera_hit.x,
			.payload.block_place.y = p->camera_hit.y,
			.payload.block_place.z = p->camera_hit.z,
			.payload.block_place.side = p->camera_hit.side,
			.payload.block_place.held_item = held_for_place,
			.payload.block_place.has_held_item = has_held_for_place,
		});
		/* Online, sendPlaceBlock false -> Beta's trailing sendUseItem queues
		 * Packet15(-1,-1,-1,255) for ANY held stack, right after the block's
		 * Packet15 and before the rod's Packet18 [Minecraft.java:834-838;
		 * PlayerControllerMP.java:148-153]; a vanilla server only runs
		 * useItemRightClick (eat, bow, bucket, egg, snowball, rod) from that
		 * packet. Local play needs no second RPC: server_local's
		 * SRPC_BLOCK_PLACE already falls through to the held item's use, so
		 * sending one would use the item twice. */
		if(!place_swings && has_held_for_place && held_for_place.count > 0
		   && mp_swing_active(slot))
			act_send(slot, p, &(struct server_rpc) {
				.type = SRPC_USE_ITEM,
				.payload.use_item.charge = 1.0F,
				.payload.use_item.held_item = held_for_place,
				.payload.use_item.has_held_item = true,
			});
		/* Card m1: the Packet18 goes AFTER sendPlaceBlock's Packet15. */
		if((place_swings || place_rod_swings) && mp_swing_active(slot))
			mp_swing_send();
		if(place_swings || place_rod_swings) {
			/* The re-equip dip rides a stack-size change
			 * (ItemRenderer.func_9449_b, Minecraft.java:827-829): a placed
			 * block/item, not an activation, nor the flint/hoe/rod (which
			 * only take damage). */
			bool dip = place_swings && !place_activated && has_held_for_place
				&& held_for_place.id != ITEM_FLINT_STEEL
				&& held_for_place.id != ITEM_WOOD_HOE
				&& held_for_place.id != ITEM_STONE_HOE
				&& held_for_place.id != ITEM_IRON_HOE
				&& held_for_place.id != ITEM_DIAMOND_HOE
				&& held_for_place.id != ITEM_GOLD_HOE;
			held_anim_swing(&p->held_item_animation, time_get(), dip);
		} else if(has_held_for_place && held_for_place.id != ITEM_BOW
				  && (item_has_air_use(held_for_place.id)
					  || item_is_food(held_for_place.id))) {
			/* The trailing sendUseItem at a block (food, egg, snowball,
			 * boat): re-equip, no swing -- as the open-air path below. */
			held_anim_equip_dip(&p->held_item_animation, time_get());
		}
	}

	// PLAYER BOW (audit #17c) -- Wii control scheme (documented playability layer,
	// see combat.h): the IR pointer aims (it drives the camera rx/ry the server
	// reconstructs), and pressing the USE button (IB_ACTION2) into OPEN AIR -- no
	// block AND no entity under the crosshair -- fires the bow. Beta 1.7.3's bow is
	// INSTANT-FIRE (no draw/charge), so a single press = one shot; we map it to
	// charge=1.0 (the server ignores the value, it is carried for API symmetry). The
	// open-air gate keeps IB_ACTION2 unambiguous: a targeted block still places, a
	// targeted entity still right-clicks; only an empty-air press shoots, exactly
	// like Beta using the bow into the sky. The server consumes one arrow + enforces
	// "must hold a bow", so a harmless press with the wrong item is a no-op there.
	if(!p->camera_hit.hit && !p->camera_hit.entity_hit
	   && act_pressed(slot, frame, IB_ACTION2) && !p->digging.active) {
		struct item_data held;
		// M4 S5: generalized from the bow-only check to item_has_air_use() so
		// the thrown egg (Beta ItemEgg fires on a right-click ANYWHERE; the
		// at-a-block case rides the server's SRPC_BLOCK_PLACE fall-through)
		// shares this one open-air path. The server dispatches per held id.
		// critique-1028 eat: Beta sends this for ANY held stack, not a list:
		// Minecraft.clickMouse :834-838 `if(var9 != null &&
		// playerController.sendUseItem(...))`, and PlayerControllerMP
		// .sendUseItem :148-153 queues Packet15Place(-1,-1,-1,255,item)
		// before the local useItemRightClick. Gating on item_has_air_use()
		// left FOOD (ItemFood.onItemRightClick, instant) unusable with no
		// block under the crosshair, and online it withheld the packet a
		// vanilla server needs for every other onItemRightClick (e.g.
		// ItemBucket's own liquid raytrace). The server ignores items with no
		// air use (Beta's base Item.onItemRightClick returns the stack).
		if(inventory_get_hotbar_item(
			   windowc_get_latest(p->windows[WINDOWC_INVENTORY]), &held)
		   && held.count > 0) {
			act_send(slot, p, &(struct server_rpc) {
				.type = SRPC_USE_ITEM,
				.payload.use_item.charge = 1.0F, // instant fire (no draw in b1.7.3)
				.payload.use_item.held_item = held,
				.payload.use_item.has_held_item = true,
			});
			/* Card m1: ItemFishingRod.onItemRightClick swings (cast and
			 * reel) after sendUseItem's Packet15(-1) [ItemFishingRod.java:
			 * 22,29]; the bow, egg, snowball and boat never swing. */
			if(mp_swing_after_use_item(true, held.id)) {
				if(mp_swing_active(slot))
					mp_swing_send();
				held_anim_swing(&p->held_item_animation, time_get(), false);
			} else if(held.id != ITEM_BOW
					  && (item_has_air_use(held.id) || item_is_food(held.id))) {
				/* No swing: ItemFood/ItemEgg/ItemSnowball/ItemBoat never
				 * call swingItem; their --stackSize makes sendUseItem true,
				 * which only re-equips (ItemRenderer.func_9450_c,
				 * Minecraft.java:836-838). ItemBow returns the same,
				 * unchanged stack -> no dip either. (A boat aimed at no
				 * water and a bucket's fill/empty are not predicted here:
				 * cosmetic, and the dip-or-not needs the item's own
				 * raytrace.) */
				held_anim_equip_dip(&p->held_item_animation, time_get());
			}
		}
	}

	// block dig
	if(p->digging.active) {
		struct block_data blk
			= world_get_block(world_view_current(), p->digging.x, p->digging.y,
							  p->digging.z);
		struct item_data it;
		inventory_get_hotbar_item(
			windowc_get_latest(p->windows[WINDOWC_INVENTORY]), &it);
		/* Beta EntityPlayer.getCurrentPlayerStrVsBlock (740c583,
		 * EntityPlayer.java:291-302) divides dig strength by 5.0F while the eye
		 * is inside water material and by 5.0F again while !onGround, so the
		 * dig time is read from the OBSERVED body, never assumed dry-and-
		 * standing. `on_ground` is spelled exactly as the SRPC_PLAYER_STATE
		 * send reads it (client_interface.c), so we time against the value the
		 * server was told -- a mismatch there is what makes a Beta server log
		 * "incoherent break time" and refuse the break. `in_water` is this
		 * session's eye-cell water flag (main.c's camera probe), the engine's
		 * closest predicate to isInsideOfMaterial(Material.water). */
		bool dig_on_ground
			= p->local_player ? p->local_player->on_ground : false;
		int delay = blocks[blk.type] ?
			tool_dig_delay_env_ms(blocks[blk.type], item_get(&it), p->in_water,
								  dig_on_ground) :
			0;
		/* CREATIVE instant break (net-new, maintainer exception 2026-08-25 --
		 * docs/creative_mode.md): one-frame dig on ANY real block, including
		 * the hardness<=0 "never digs" sentinels (bedrock/portal). Must stay
		 * strictly > 0: the finished-send gate below requires delay > 0, so
		 * 0 would make everything UNbreakable -- the bedrock trap. */
		if(gstate.config_creative && blocks[blk.type])
			delay = 1;

		/* R1d (Fable R1 review): Beta PlayerControllerMP.clickBlock sends a
		 * dig-start only on `!isHittingBlock || cell differs`, and a crosshair
		 * off every block runs resetBlockRemoving -- NO packet. The old
		 * `!camera_hit.hit ||` clause re-sent a start at the STALE cell every
		 * frame, which the server's onBlockClicked dispatch turned into a
		 * lever/door toggle, cake bite or note per frame. On a miss, stop the
		 * dig without sending; returning to a block while held re-presses via
		 * the idle branch below (Beta's re-click on return). Shared by the host
		 * slot, the split guest slot and the MP remote provider (act_send).
		 * [740c583: PlayerControllerMP.java:41-66] */
		enum dig_start_action dig_act = dig_start_decide(
			p->camera_hit.hit,
			p->digging.x == p->camera_hit.x && p->digging.y == p->camera_hit.y
				&& p->digging.z == p->camera_hit.z);
		if(dig_act == DIG_START_RESET) {
			p->digging.start = time_get();
			p->digging.active = false;
		} else {
			if(dig_act == DIG_START_SEND) {
				p->digging.start = time_get();
				p->digging.x = p->camera_hit.x;
				p->digging.y = p->camera_hit.y;
				p->digging.z = p->camera_hit.z;

				act_send(slot, p, &(struct server_rpc) {
					.type = SRPC_BLOCK_DIG,
					.payload.block_dig.x = p->digging.x,
					.payload.block_dig.y = p->digging.y,
					.payload.block_dig.z = p->digging.z,
					.payload.block_dig.side = p->camera_hit.side,
					.payload.block_dig.finished = false,
					.payload.block_dig.start_seq
					= dig_start_next_seq(&s_dig_start_seq),
				});
			}

			if(delay > 0
			   && time_diff_ms(p->digging.start, time_get()) >= delay) {
				act_send(slot, p, &(struct server_rpc) {
					.type = SRPC_BLOCK_DIG,
					.payload.block_dig.x = p->digging.x,
					.payload.block_dig.y = p->digging.y,
					.payload.block_dig.z = p->digging.z,
					.payload.block_dig.side = p->camera_hit.side,
					.payload.block_dig.finished = true,
				});

				p->digging.cooldown = time_get();
				p->digging.active = false;
			}
		}

		if(act_released(slot, frame, IB_ACTION1))
			p->digging.active = false;
	} else {
		if(p->camera_hit.hit && act_held(slot, frame, IB_ACTION1)
		   && time_reached(time_get(), time_add_ms(p->digging.cooldown, 250))) {
			/* Card m1: a fresh dig start is clickBlock's Packet14 status 0,
			 * sent from inside clickMouse(0) right after its swing
			 * [PlayerControllerMP.java:41-43]. (The retarget above is
			 * sendBlockRemoving -> clickBlock, which does NOT swing.) */
			mp_swing_before_left_action(slot, &mp_swung_left);
			p->digging.active = true;
			p->digging.start = time_get();
			p->digging.x = p->camera_hit.x;
			p->digging.y = p->camera_hit.y;
			p->digging.z = p->camera_hit.z;

			act_send(slot, p, &(struct server_rpc) {
				.type = SRPC_BLOCK_DIG,
				.payload.block_dig.x = p->digging.x,
				.payload.block_dig.y = p->digging.y,
				.payload.block_dig.z = p->digging.z,
				.payload.block_dig.side = p->camera_hit.side,
				.payload.block_dig.finished = false,
				.payload.block_dig.start_seq
				= dig_start_next_seq(&s_dig_start_seq),
			});
		}
	}

	if(act_held(slot, frame, IB_ACTION1)
	   && time_diff_s(p->held_item_animation.punch.start, time_get())
		   >= 0.2F) {
		held_anim_swing(&p->held_item_animation, time_get(), false);

		if(p->camera_hit.hit) {
			struct block_data blk
				= world_get_block(world_view_current(), p->camera_hit.x,
								  p->camera_hit.y, p->camera_hit.z);

			struct block_data neighbours[6];

			for(int k = 0; k < SIDE_MAX; k++) {
				int ox, oy, oz;
				blocks_side_offset((enum side)k, &ox, &oy, &oz);

				neighbours[k] = world_get_block(
					world_view_current(), p->camera_hit.x + ox,
					p->camera_hit.y + oy, p->camera_hit.z + oz);
			}

			particle_generate_side(
				&(struct block_info) {.block = &blk,
									  .neighbours = neighbours,
									  .x = p->camera_hit.x,
									  .y = p->camera_hit.y,
									  .z = p->camera_hit.z},
				p->camera_hit.side);

			// S0 mining hit tick, CLIENT-LOCAL (Beta calls mc.sndManager
			// directly -- this never touches World even in SP): the
			// STEP-name sound at (vol+1)/8 = 0.25F, pitch*0.5F, at the
			// block center, every 4th damage tick -- 4 ticks @ 20Hz =
			// 200ms, exactly this punch loop's cadence, so the sound rides
			// the same beat as the hit-crack particles above. Gated on an
			// ACTIVE dig (the cooldown/idle swing stays silent, like
			// Beta's blockHitDelay gate). [740c583: PlayerControllerSP
			// .java:73-75]
			if(p->digging.active && blk.type != BLOCK_AIR)
				audio_play3d(audio_block_step_sound_id(blk.type),
							 (vec3) {p->camera_hit.x + 0.5F,
									 p->camera_hit.y + 0.5F,
									 p->camera_hit.z + 0.5F},
							 (audio_block_sound_volume(blk.type) + 1.0F)
								 / 8.0F,
							 audio_block_sound_pitch(blk.type) * 0.5F);
		}
	}

	size_t slot_index = inventory_get_hotbar(
		windowc_get_latest(p->windows[WINDOWC_INVENTORY]));
	bool old_item_exists = inventory_get_hotbar_item(
		windowc_get_latest(p->windows[WINDOWC_INVENTORY]), NULL);

	// Only the FORCED overlay (capture/tuning rig, "debug.overlay":"force")
	// repurposes +/- as the LOD reach dial (handled in main.c). With the plain
	// F3 toggle / overlay "on", +/- stays the HOTBAR scroll -- Wiimote + / -
	// and Classic ZL/ZR / + / - cycle the held slot like the console editions.
	/* b1-chat-opener-0919 slice 1 (maintainer's CavEX layout, Astra ruling 3
	 * D2): on the Wii Remote, Minus and Plus carry BOTH the hotbar and the F3
	 * chord, so the step comes from input_hotbar_step_edge -- the RELEASE of a
	 * clean tap, cancelled by any overlap of the two. Both gates are read
	 * UNCONDITIONALLY, once per frame, because a frame they are not called on
	 * is exactly what cancels a gesture in progress; the forced overlay then
	 * discards the answer instead of hiding the call.
	 *
	 * PER PLAYER (review 09 finding 1): the gate takes the acting slot and
	 * resolves THAT player's owner frame, device and family, so the split
	 * guest's own Remote gets the ruled rule instead of the press edge it kept
	 * while this line gated on slot 0. For a player whose family is not the
	 * double-duty pair the gate degrades to exactly the edge act_pressed()
	 * gave it -- the merged mask for slot 0, the owner frame otherwise. */
	bool step_left = input_hotbar_step_edge(slot, frame, IB_SCROLL_LEFT);
	bool step_right = input_hotbar_step_edge(slot, frame, IB_SCROLL_RIGHT);

	if(!gstate.force_debug_overlay && step_left) {
		size_t next_slot = (slot_index == 0) ? INVENTORY_SIZE_HOTBAR - 1 : slot_index - 1;
		inventory_set_hotbar(
			windowc_get_latest(p->windows[WINDOWC_INVENTORY]), next_slot);
		bool new_item_exists = inventory_get_hotbar_item(
			windowc_get_latest(p->windows[WINDOWC_INVENTORY]), NULL);

		if(time_diff_s(p->held_item_animation.switch_item.start, time_get())
			   >= 0.15F
		   && (old_item_exists || new_item_exists)) {
			p->held_item_animation.switch_item.start = time_get();
			p->held_item_animation.switch_item.old_slot = slot_index;
		}

		if(p->digging.active)
			p->digging.start = time_get();

		act_send(slot, p, &(struct server_rpc) {
			.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot.slot = next_slot,
		});
	}

	if(!gstate.force_debug_overlay && step_right) {
		size_t next_slot = (slot_index == INVENTORY_SIZE_HOTBAR - 1) ? 0 : slot_index + 1;
		inventory_set_hotbar(
			windowc_get_latest(p->windows[WINDOWC_INVENTORY]), next_slot);
		bool new_item_exists = inventory_get_hotbar_item(
			windowc_get_latest(p->windows[WINDOWC_INVENTORY]), NULL);

		if(time_diff_s(p->held_item_animation.switch_item.start, time_get())
			   >= 0.15F
		   && (old_item_exists || new_item_exists)) {
			p->held_item_animation.switch_item.start = time_get();
			p->held_item_animation.switch_item.old_slot = slot_index;
		}

		if(p->digging.active)
			p->digging.start = time_get();

		act_send(slot, p, &(struct server_rpc) {
			.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot.slot = next_slot,
		});
	}

	/* Global-screen edges: HOST only (see the function comment). The guest
	 * opens its Pause and its containers through the owner-local menu host in
	 * main.c, which consumes those edges before this path ever runs. */
	if(slot == 0) {
		if(input_pressed(screen_ingame_action_role[SCREEN_INGAME_ACT_PAUSE])
		   && screen_ingame_try_pause())
			return; /* the pause screen owns every later same-frame edge */

		/* brief 07 C: the GAMEPLAY OPEN role, not the menu Close role. A
		 * screen that is up reads IB_INVENTORY for its Back/Cancel; this
		 * reads the button the CavEX layout puts the open on (Remote A). */
		if(input_pressed(
			   screen_ingame_action_role[SCREEN_INGAME_ACT_OPEN_INVENTORY])) {
			/* Creative phase 2: the picker fronts the inventory button; the
			 * survival inventory stays reachable via the picker's own button
			 * (docs/creative_mode.md). */
			screen_set(gstate.config_creative ? &screen_creative_picker
											  : &screen_inventory);
			/* Review finding 7: the arriving screen owns every later edge of
			 * this frame, exactly as the pause branch above already returns.
			 * Without this a chat RELEASE edge below could screen_set over
			 * the inventory that was just opened. */
			return;
		}

		/* Multiplayer chat / command composer: remote sessions only, since
		 * Beta single-player chat send is a no-op. input_chat_open_edge owns
		 * the resolved binding (defaults: Remote 2 tap, Classic and GameCube
		 * D-pad Up) and MUST be called once per frame from here: frames on
		 * which it is not called cancel a gesture in progress. */
		if(gstate.remote_session && input_chat_open_edge()) {
			screen_set(&screen_chat);
			return;
		}
	}

	// Beta 'Q': drop one of the held hotbar item into the world. The server pulls the
	// current slot, decrements it, and spawns the thrown EntityItem (SRPC_DROP_ITEM).
	// Gate the send on a non-empty hand so an empty-handed press isn't a wasted RPC.
	if(act_pressed(slot, frame, IB_DROP)) {
		struct item_data held;
		if(inventory_get_hotbar_item(
			   windowc_get_latest(p->windows[WINDOWC_INVENTORY]), &held)
		   && item_get(&held)) {
			act_send(slot, p, &(struct server_rpc) {
				.type = SRPC_DROP_ITEM,
				.payload.drop_item.all = false,
			});
		}
	}
}

static void screen_ingame_update(struct screen* s, float dt) {
	screen_ingame_update_slot(&gstate.players[0], 0, NULL, dt);
}

/* S1-b entry for a NON-host local owner (the split-screen guest, owner 1).
 * main.c calls this once per frame while that owner is admitted, with the
 * owner's own latched controller frame. Refuses slot 0 (the host path stays
 * the screen vtable's update) and refuses an owner with no inventory window --
 * every branch below dereferences windowc_get_latest(windows[WINDOWC_INVENTORY]),
 * which asserts on NULL. */
void screen_ingame_update_owned(struct client_session* owner, unsigned slot,
								const struct input_player_frame* frame,
								float dt) {
	if(!owner || slot == 0 || slot >= CLIENT_LOCAL_PLAYERS
	   || !owner->windows[WINDOWC_INVENTORY])
		return;
	struct clin_view_world action_world;
	if(!clin_world_view_enter(slot, &action_world))
		return;
	screen_ingame_update_slot(owner, slot, frame, dt);
	clin_world_view_leave(&action_world);
}

/* S1-b owner-1 admission + NEUTRAL-REARM latch (Astra call #6 ruling item 7,
 * call #8 (iv)).
 *
 * The guest's action pass may run only while its claim is continuously valid:
 * the caller's `admitted` (view, world, menus, pause, body, health > 0 --
 * main.c owns those), plus a connected owner frame, plus the SAME provider
 * generation and the SAME physical device as the last armed frame. The moment
 * any of that breaks -- controller unplugged, guest leave, a rejoin's new
 * generation, a different pad claiming owner 1 -- the pending optimistic action
 * state is dropped and the latch disarms.
 *
 * Re-arming then requires ONE neutral frame: every action button seen released.
 * input_owner_latch already blocks a reconnecting device's buttons until they
 * are released, but it cannot see a generation or ownership change on a pad
 * that never disconnected -- which is exactly the rejoin case -- so the latch
 * enforces the same rule at this layer. Without it a finger still on the dig
 * button at leave time would resume digging into the NEW body the instant it
 * was admitted: a phantom action nobody pressed.
 *
 * One guest exists, so the latch is one file-static; screen_ingame_guest_action_reset
 * clears it for a test fixture or an explicit teardown. */
static const enum input_button s_guest_action_buttons[]
	= {IB_ACTION1, IB_ACTION2, IB_DROP, IB_SCROLL_LEFT, IB_SCROLL_RIGHT};

static struct {
	uint64_t generation;
	struct input_device device;
	bool tracking; /* the latch currently follows a live claim */
	bool armed;    /* a neutral frame has been seen since the last drop */
} s_guest_action;

/* Drop the guest's PENDING/HELD optimistic action state. Presentation and
 * prediction only -- the server is authoritative for everything that already
 * left through the queue; this is what stops the client half from carrying a
 * half-finished dig or a swing animation across a drop/rejoin. */
static void screen_ingame_guest_action_clear(struct client_session* owner) {
	if(!owner)
		return;
	owner->digging = (struct digging) {0};
	owner->held_item_animation = (struct held_anim) {0};
}

void screen_ingame_guest_action_reset(struct client_session* owner) {
	screen_ingame_guest_action_clear(owner);
	s_guest_action.generation = 0;
	s_guest_action.device = (struct input_device) {0};
	s_guest_action.tracking = false;
	s_guest_action.armed = false;
}

bool screen_ingame_guest_action_frame(struct client_session* owner,
									  const struct input_player_frame* frame,
									  const struct input_device* device,
									  bool admitted) {
	bool live = admitted && owner && owner->local_bound
		&& owner->local_generation && !owner->local_reply_failed && frame
		&& frame->connected && device && device->kind != INPUT_DEVICE_NONE;
	/* A generation or ownership change is a DROP first, never a handover: the
	 * next frame re-latches on the new claim and re-arms from neutral. */
	if(live && s_guest_action.tracking
	   && (s_guest_action.generation != owner->local_generation
		   || !input_device_equal(s_guest_action.device, *device)))
		live = false;

	if(!live) {
		if(s_guest_action.tracking || s_guest_action.armed)
			screen_ingame_guest_action_clear(owner);
		s_guest_action.tracking = false;
		s_guest_action.armed = false;
		return false;
	}

	if(!s_guest_action.tracking) {
		s_guest_action.tracking = true;
		s_guest_action.armed = false;
		s_guest_action.generation = owner->local_generation;
		s_guest_action.device = *device;
	}

	if(!s_guest_action.armed) {
		for(size_t k = 0; k < sizeof(s_guest_action_buttons)
							  / sizeof(s_guest_action_buttons[0]);
			k++)
			if(frame->down[s_guest_action_buttons[k]])
				return false;
		s_guest_action.armed = true;
	}
	return true;
}

static void screen_ingame_render2D(struct screen* s, int width, int height) {
	struct input_player_frame input;
	input_prompt_frame(0, &input);
	screen_ingame_render2D_owned(&gstate.players[0], &input, width, height);
}

/* ---- H-C2 THE CHAT SOCKET (Astra build spec 15 A.1/A.3/A.4; maintainer
 * pick 2026-09-21 "I like Hc 2") ------------------------------------------
 *
 * The bottom-right corner block that tells a remote player how to open the
 * composer: the hotbar's own gui.png panel texels as a 44x44 BODY, the
 * selection frame around it as a 48x48 FRAME, and ONE complete official
 * 32x32 glyph cell inside it -- unscaled, untrimmed, untinted, nearest, the
 * approved artwork exactly as the sheet draws it. "Tap" sits above the socket
 * when the resolved opener runs through the gesture rule; "Chat" sits to its
 * right.
 *
 * WHAT IT REPLACES (A.5). Ruling 8's single-line hint drew a HALF-size 16 px
 * cell plus a direction WORD, because input_glyph_key answers cell 24 -- the
 * whole D-pad -- for all four directions of every family, so the glyph alone
 * could not say which way to push. That made a remap the widest case
 * ("[D-pad] Right Chat", 126 px) and forced the whole prompt to shrink.
 * The socket draws the DIRECTION'S OWN cell instead (25 Up, 26 Down, 27 Left,
 * 28 Right -- input_glyph_dpad_direction_cell, a read-only second view of the
 * same four codes that leaves every other prompt on the shared cell 24), so
 * the word is gone and every single-button binding is the same size. A CHORD
 * is not one glyph wide, so it keeps the existing modifier + "+" + button
 * composition at size 16 -- 40 px -- and the modifier is never omitted.
 *
 * Both facts about the binding -- the code and whether it opens on a TAP --
 * come from input.c through input_player_chat_opener; NEITHER the binding
 * codes nor the config are read here (lead ruling 1).
 *
 * PLACEMENT: the maintainer's 2026-09-21 call ("B for the (2) and Tap chat")
 * supersedes spec 15 A.1's positions with Astra placement PL-B -- the socket
 * TUCKS AGAINST the hotbar's right end at hotbar height, 8 px clear of the
 * slot-9 selection frame, and the label sits BESIDE it, left-aligned, as
 * "Tap" over "Chat" when the opener is a gesture and as "Chat" alone,
 * vertically centred on the socket, when it is not. Nothing else about the
 * socket changed.
 *
 * ALL NUMBERS BELOW ARE DESTINATION PIXELS AT GUI SCALE 2 (the spec's own
 * unit): nothing is multiplied by GFX_GUI_SCALE a second time, and the split
 * rule IS the single rule -- the same numbers against that VIEWPORT's own
 * width and height, which is what "subtract 240 from every y" means.
 *
 * PURE, so tests assert the rectangles instead of eyeballing them. */
#define CHAT_SOCKET_NONE 0   /* unbound: no socket */
#define CHAT_SOCKET_SINGLE 1 /* one complete official 32x32 cell */
#define CHAT_SOCKET_CHORD 2  /* the modifier + button composition, 40 px */
#define CHAT_SOCKET_TEXT 16

/* Resolve the socket for one owner's frame and fill every rectangle A.1 and
 * A.3 name. Returns the KIND (CHAT_SOCKET_*); `cells` receives {modifier,
 * button}, -1/-1 when nothing is bound. A rectangle that is NOT drawn comes
 * back all-zero, so "zero width" is the layout's own way of saying "absent"
 * -- that is what an unbound action does to the socket, and what a plain
 * press edge does to "Tap".
 *
 * Every out-parameter may be NULL, and the same pointer may be passed twice
 * by a caller that only wants one of them. */
int screen_ingame_chat_socket_layout(const struct input_player_frame* input,
	int width, int height, int code, bool is_tap,
	struct gutil_button_bounds* body, struct gutil_button_bounds* frame,
	struct gutil_button_bounds* glyph, struct gutil_button_bounds* tap,
	struct gutil_button_bounds* label, int cells[2], const char** label_text);
int screen_ingame_chat_socket_layout(const struct input_player_frame* input,
	int width, int height, int code, bool is_tap,
	struct gutil_button_bounds* body, struct gutil_button_bounds* frame,
	struct gutil_button_bounds* glyph, struct gutil_button_bounds* tap,
	struct gutil_button_bounds* label, int cells[2], const char** label_text) {
	int modifier = -1, button = -1, kind = CHAT_SOCKET_NONE;
	if(input && input_glyph_cells(input->glyphs[IB_CHAT], &modifier, &button)) {
		kind = modifier >= 0 ? CHAT_SOCKET_CHORD : CHAT_SOCKET_SINGLE;
		if(kind == CHAT_SOCKET_SINGLE) {
			/* A.1: the DIRECTION's own cell replaces the shared whole-D-pad
			 * cell the resolver chose. Scoped to this prompt -- every other
			 * caller of input_glyph_key still gets cell 24 -- and never
			 * applied to a chord, which directional cells cannot make one
			 * glyph wide. A code that is not a D-pad direction OF THIS
			 * FAMILY (a PC key, a stick, an ordinary button) answers -1 and
			 * keeps the resolver's own cell. */
			int direction
				= input_glyph_dpad_direction_cell(input->category, code);
			if(direction >= 0)
				button = direction;
		}
	}
	if(kind == CHAT_SOCKET_NONE)
		modifier = button = -1;
	if(cells) {
		cells[0] = modifier;
		cells[1] = button;
	}
	bool drawn = kind != CHAT_SOCKET_NONE;
	const struct gutil_button_bounds absent = {0};
	/* PLACEMENT PL-B, the maintainer's 2026-09-21 call ("B for the (2) and
	 * Tap chat"): the socket tucks against the hotbar's right end at hotbar
	 * height and the two-line label sits BESIDE it, left-aligned. Every
	 * offset is stated against the FRAME, which is the one anchor, so the
	 * parts cannot drift apart. */
	bool wide = false;
	if(drawn && kind == CHAT_SOCKET_SINGLE)
		(void)input_glyph_label_for_code(input->glyphs[IB_CHAT], &wide);
	int extra = wide ? 32 : 0;
	int fx = width - 128, fy = height - 72;
	if(frame)
		*frame = drawn
			? (struct gutil_button_bounds) {fx, fy, 48 + extra, 48} : absent;
	if(body)
		*body = drawn
			? (struct gutil_button_bounds) {fx + 2, fy + 2, 44 + extra, 44} : absent;
	if(glyph)
		*glyph = !drawn ? absent
			: kind == CHAT_SOCKET_CHORD
				/* "its two complete cells remain proportional; the helper's
				 * + is 8 px" -- gutil_prompt_glyph measures exactly
				 * size + size/2 + size = 40 at 16. */
				? (struct gutil_button_bounds) {fx + 4, fy + 16, 40, 16}
				: (struct gutil_button_bounds) {fx + 8, fy + 8, 32 + extra, 32};
	/* A wide PC cap extends rightward, clear of the hotbar; put its label
	 * above the frame so the 64 px cap and Chat both stay TV-safe. */
	if(tap)
		*tap = drawn && is_tap
			? (struct gutil_button_bounds) {wide ? fx : width - 72, wide ? fy - 40 : height - 68,
				gutil_font_width("Tap", CHAT_SOCKET_TEXT), CHAT_SOCKET_TEXT}
			: absent;
	/* An explicitly unbound chat action advertises no opener. */
	const char* lab = drawn ? "Chat" : "";
	if(label_text)
		*label_text = lab;
	if(label)
		*label = !drawn ? absent : (struct gutil_button_bounds) {
			/* With "Tap" above it the pair straddles the socket; alone it is
			 * vertically centred on the socket instead. */
			wide ? fx : width - 72,
			wide ? fy - 20 : is_tap ? height - 46 : height - 56,
			gutil_font_width(lab, CHAT_SOCKET_TEXT), CHAT_SOCKET_TEXT};
	return kind;
}

/* ---- WHICH MODALS OWN THIS VIEWPORT (A.3/A.4; review 28 H2 + M1) --------
 * A.3: "Suppression rules apply before drawing; a modal must not merely cover
 * a still-visible hint." Three modals are drawn OVER this HUD rather than
 * instead of it, and each one has to suppress BOTH the chat socket and the
 * sneak rail -- so the fact is computed once, here, and handed to both gates.
 *
 *  `owner_recovery`   -- that owner's menu-host recovery modal
 *                        (local_menu_host_recovery_render, the split path).
 *  `primary_recovery` -- the PRIMARY controller-recovery modal,
 *                        screen_controller_recovery_render, which this very
 *                        function calls at its end for players[0]: a
 *                        153-alpha viewport wash plus a 460 px panel. In
 *                        SINGLE view -- the ordinary one-console remote
 *                        session -- this is THE recovery modal, and it is a
 *                        DIFFERENT machine from the per-owner menu_host one
 *                        (main.c runs that only for a pause / menu-host /
 *                        short-viewport frame), which is why reading only the
 *                        per-owner phase left the socket visible at ~40 %
 *                        through the dim, advertising a control the owner
 *                        physically could not press.
 *  `local_pause`      -- the LOCAL pause. In single view main.c draws
 *                        local_pause_render AFTER this function: a
 *                        viewport-wide 180-alpha wash plus a panel, i.e. the
 *                        HUD underneath is COVERED, not suppressed. (In split
 *                        the local pause replaces the HUD instead, so the
 *                        term simply never comes up there.)
 *
 * What is genuinely hidden BY CONSTRUCTION, and therefore has no term here:
 * the GLOBAL screens -- pause, inventory, crafting, the composer, the title /
 * world-select / connect screens -- because they are current_screen and this
 * function is not called at all; and the death screen, which is the global
 * screen_death in single view and main.c's own branch in split. There is no
 * loading / terrain-hold overlay to take: the world load happens under those
 * global screens. The connection notices this function draws are a top-of-
 * viewport card, not a modal -- they own no input and dim nothing. */
bool screen_ingame_hud_modal_active(bool owner_recovery, bool primary_recovery,
									bool local_pause);
bool screen_ingame_hud_modal_active(bool owner_recovery, bool primary_recovery,
									bool local_pause) {
	return owner_recovery || primary_recovery || local_pause;
}

/* ---- the socket's VISIBILITY (A.4) --------------------------------------
 * `owner_can_chat` is the CHAT ROUTE's own owner (below), not an identity
 * test: A.5 retires the `owner == players[0]` restriction "where the owner
 * can actually use chat", and the composer is a global screen, so that is
 * what the restriction was standing in for.
 *
 * `modal` is screen_ingame_hud_modal_active above -- every modal that owns
 * this viewport, not just the per-owner recovery phase.
 *
 * `capture` is the rig lever; like the sneak lever it forces the DRAW, and
 * like it, it is exempt from NO suppression -- a rig frame can never
 * photograph the socket under a modal or a join prompt.
 *
 * Menus, the pause SCREEN and the composer hide it by CONSTRUCTION: those
 * GLOBAL screens own the frame instead of screen_ingame_render2D_owned,
 * exactly as they already do for the hotbar and the hearts. (The owner-LOCAL
 * pause does not -- it is drawn over this HUD, and it is one of `modal`'s
 * three terms.) This engine has no separate hidden-HUD toggle, so there is no
 * further term to take. And the retired `chat_hint_done` gate is simply gone:
 * A.4 makes the socket persistent throughout eligible gameplay, even after
 * chat is first opened. */
bool screen_ingame_chat_socket_visible(bool remote_session, bool owner_can_chat,
									   bool modal, bool join_prompt,
									   bool capture);
bool screen_ingame_chat_socket_visible(bool remote_session, bool owner_can_chat,
									   bool modal, bool join_prompt,
									   bool capture) {
	return owner_can_chat && (remote_session || capture) && !modal
		&& !join_prompt;
}

/* Which LOCAL owner can actually open the chat composer. The in-game opener
 * calls screen_set, which owns the GLOBAL screen, so it is host-only; the
 * split guest reaches its own menus through the owner-local menu host, which
 * has no composer row. A socket in the guest's viewport would therefore
 * advertise a control that owner cannot press. Stated once, parameterized by
 * owner, so the socket follows the ROUTE rather than an identity test. */
bool screen_ingame_chat_socket_owner(unsigned slot);
bool screen_ingame_chat_socket_owner(unsigned slot) {
	return slot == 0;
}

/* How many times the socket-capture lever's config key has actually been
 * consulted this process. The cache below exists so that number is ONE
 * however many frames and viewports are drawn; without an instrument the
 * suite could not tell a working cache from one that re-parses the config on
 * every frame (review 28 M2). Test-only: no production caller. */
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static unsigned s_socket_capture_reads;
unsigned screen_ingame_test_socket_capture_reads(void);
unsigned screen_ingame_test_socket_capture_reads(void) {
	return s_socket_capture_reads;
}
#define screen_ingame_socket_capture_read_counted() (s_socket_capture_reads++)
#else
#define screen_ingame_socket_capture_read_counted() ((void)0)
#endif

/* ---- the socket's CAPTURE LEVER (debug.chat_hint_capture) ---------------
 * The socket draws only in a remote session, so neither the PC rig nor a
 * single-player console run could photograph it -- pc-captures-22's stated
 * limit 1, and exactly what a layout review needs. The lever forces the DRAW
 * with a chosen binding:
 *
 *   1 = the shipped Remote default (code 7, which opens through the gesture
 *       rule, so "Tap"; official cell 3, the Wii "2" glyph);
 *   2 = a D-pad RIGHT remap, which the socket draws as cell 28 with no
 *       direction word;
 *   3 = a CHORD (Classic Minus + Plus), the 40 px composition.
 *
 * Absent or any other value is off. CHANGED from the ruling-8 lever, which
 * substituted the wording only and deliberately left a PC build drawing PC
 * KEY/MSE artwork: brief 26 requires a PC frame to show the REMOTE cells it
 * is asked for, so the lever now supplies the encoded GLYPH too -- resolved
 * through the production resolver, never a hand-written atlas cell. It is
 * still a DRAW: it opens nothing, latches nothing and writes no game state.
 * PURE, so the test drives the lever itself rather than the frame it
 * produces, and every out-parameter is left untouched when it is off. */
bool screen_ingame_chat_socket_capture(int capture,
									   enum input_category* category, int* code,
									   bool* is_tap, int* glyph);
bool screen_ingame_chat_socket_capture(int capture,
									   enum input_category* category, int* code,
									   bool* is_tap, int* glyph) {
	/* Classic Minus (210) + Plus (211), in the binding layer's own chord
	 * encoding -- input_glyph_binding turns it into the packed cell pair. */
	static const int classic_chord = 210 * 1000 + 211;
	enum input_category family;
	int c, g;
	bool tap;
	switch(capture) {
		case 1:
			family = INPUT_CAT_NUNCHUK;
			c = 7;
			tap = true;
			g = input_glyph_key(family, c);
			break;
		case 2:
			family = INPUT_CAT_GAMECUBE;
			c = 403;
			tap = false;
			g = input_glyph_key(family, c);
			break;
		case 3:
			family = INPUT_CAT_CLASSIC_CONTROLLER;
			c = 211;
			tap = false;
			g = input_glyph_binding(family, &classic_chord, 1);
			break;
		default:
			return false;
	}
	if(category)
		*category = family;
	if(code)
		*code = c;
	if(is_tap)
		*is_tap = tap;
	if(glyph)
		*glyph = g;
	return true;
}

/* Draw the socket. NOT static and NOT #ifdef'd: tests/test_ui_geometry.c
 * traces this exact helper, so what is asserted is what is drawn. */
/* Keep named PC codes intact; only an unlabelled single D-pad glyph needs
 * the socket-specific direction cell. Shared by drawing and headless tests. */
int screen_ingame_chat_socket_prompt(const struct input_player_frame* input, int code);
int screen_ingame_chat_socket_prompt(const struct input_player_frame* input, int code) {
	int glyph = input->glyphs[IB_CHAT], modifier = -1, button = -1;
	if(input_glyph_label_for_code(glyph, NULL)
	   || !input_glyph_cells(glyph, &modifier, &button) || modifier >= 0)
		return glyph;
	int direction = input_glyph_dpad_direction_cell(input->category, code);
	return direction >= 0 ? direction : glyph;
}

void screen_ingame_chat_socket_draw(const struct input_player_frame* input,
									int width, int height, int code,
									bool is_tap);
void screen_ingame_chat_socket_draw(const struct input_player_frame* input,
									int width, int height, int code,
									bool is_tap) {
	struct gutil_button_bounds body, frame, glyph, tap, label;
	int cells[2];
	const char* lab;
	int kind = screen_ingame_chat_socket_layout(input, width, height, code,
												is_tap, &body, &frame, &glyph,
												&tap, &label, cells, &lab);
	if(kind != CHAT_SOCKET_NONE) {
		/* A.1's paint order: body, frame, glyph, text.
		 *
		 * MODE_BLEND for the two gui.png quads, the same bracket the hotbar's
		 * own selection-frame draw further down uses -- and for the same
		 * reason A.1 states: "preserve source alpha". The body's interior is
		 * semi-transparent in a genuine Beta pack (alpha 186) and the frame's
		 * is fully transparent, so the alpha-test-only path would have
		 * clipped both to hard edges instead of compositing them.
		 *
		 * THE BRACKET RESTORES THE PASS'S ENTRY MODE, WHICH IS MODE_BLEND --
		 * not MODE_OFF (review 28 H1). The 2D pass is entered by
		 * gfx_mode_gui_viewport, which sets MODE_BLEND + alpha_test(true) on
		 * both backends (source/platform/pc/gfx.c:412-413,
		 * source/platform/wii/gfx.c:871-872) once per viewport. Restoring
		 * MODE_OFF here was not a restore at all: it disabled blending for
		 * every later draw in the pass -- the hotbar BODY quad (the same
		 * alpha-186 gui.png region, so a genuine Beta pack lost its
		 * translucent bar for as long as the socket was up), Beta's sleep
		 * fade (a pure alpha ramp) and the sneak rail. tests/
		 * test_ui_geometry.c's check_hud_blend_state asserts the bracket
		 * directly and requires those three draws to see the same mode with
		 * the socket on screen as without it. */
		gfx_blending(MODE_BLEND);
		gfx_bind_texture(&texture_gui_widgets);
		gutil_texquad(body.x, body.y, 0, 0, 22, 22, body.width, body.height);
		gutil_texquad(frame.x, frame.y, 0, 22, 24, 24, frame.width,
					  frame.height);
		/* One COMPLETE cell at its own size: gutil_prompt_glyph blits the
		 * whole 32x32 source into `size`, so 32 is 1:1 -- no scaling, no
		 * trimming -- and its (255,255,255,255) vertex colour is no tint.
		 * For a chord the same helper draws the two cells and the 8 px "+"
		 * at 16, which is the 40 px A.1 asks for. Named caps keep their
		 * encoded code so the helper can draw the active-font key label;
		 * only unlabelled single glyphs use the direction-cell override. */
		gutil_prompt_glyph(glyph.x, glyph.y, glyph.height,
						   screen_ingame_chat_socket_prompt(input, code));
		gfx_blending(MODE_BLEND);
		/* A.1: both labels are white with gutil_text's native hard shadow --
		 * offset (2,2), RGB (63,63,63) -- which is what `true` selects. */
		if(tap.width)
			gutil_text(tap.x, tap.y, "Tap", CHAT_SOCKET_TEXT, true);
	}
	if(kind != CHAT_SOCKET_NONE)
		gutil_text(label.x, label.y, lab, CHAT_SOCKET_TEXT, true);
}

/* ---- H-S1 THE SNEAK TOP RAIL (Astra build spec 15 A.2/A.3/A.4; maintainer
 * pick 2026-09-21 "and Hs 1") ---------------------------------------------
 * The sneak toggle is carve-out #2: on all three Wii pads there is no finger
 * free to HOLD sneak while the thumb walks, so it latches. Latched, the only
 * feedback was Beta's 0.08-block camera dip and the movement slowdown --
 * neither of which reads while standing still, so a player could be crouched
 * without knowing it.
 *
 * WHAT IT REPLACES (A.5). Ruling 6/7's marker was a plain black box at alpha
 * 192 with TWO placements -- centred 36 units above the hotbar in single
 * view, and a split special case at viewport-local x = 8 beside the bar,
 * chosen by the HUD's own split predicate. A.2 replaces the whole thing with
 * one viewport-local rule and no backing at all: a 4 px rail along the
 * hotbar's own top border, in the PACK's selection colour, and the word
 * SNEAK beside the bar. There is no `split` argument any more.
 *
 * The rail is A.2's (B+4, T, 356, 4) with B = floor((W-364)/2) and T = H-62;
 * it is expressed here in the hotbar's OWN units (2 in, 178 wide, 2 tall of
 * the 182x22 bar -- "nominal source-border pixels x = 2..179, y = 0..1"), so
 * it cannot drift from the bar it rides, and at the shipped GFX_GUI_SCALE of
 * 2 those are exactly the spec's 4 / 356 / 4. The LABEL's offsets are
 * PIXELS, the unit the spec states for its tables ("destination pixels at
 * GUI scale 2"), like ruling 7 V4's two 8s.
 *
 * Pure, so tests/test_ui_geometry.c asserts the rectangles instead of
 * eyeballing them; the hotbar anchors are recomputed from the draw site's own
 * expressions, which test_hud_source_pin pins. */
/* Maintainer ruling 2026-10-29 (critique-1028): the CavEX hotbar position
 * is back. b70f38003 had moved the hotbar to height-62 when it removed the
 * bottom button-hint row; the row is restored (always on), so the bar sits
 * where CavEX had it again: height - (16*scale)*8/5 - 22*scale = 385 of 480
 * (145 of 240 in a split pane), i.e. the 51 px the hint row reserves under
 * it. ONE expression for the draw and every layout that anchors to it. */
/* Controller UI redesign A2 (maintainer picks 10-31, WIRING-PLAN Q1): the
 * CavEX row becomes the shared hint row (gutil_hint_row, 28 px strip at
 * 428-456 of 480, 196-224 of 240), and the hotbar sits on it: hotbar top =
 * row top - SCREEN_INGAME_ROW_HOTBAR_GAP - 22*scale = 383 of 480 (2 px up
 * from 385), 151 of 240. ONE expression for the draw and every layout that
 * anchors to it (Leave Bed, the sneak rail, hud_menu_draw_owned, the menus). */
bool screen_ingame_split_pane(int height) {
	return height <= SCREEN_INGAME_SPLIT_PANE_H;
}

struct gutil_button_bounds screen_ingame_row_band(int width, int height) {
	int margin = gutil_hint_row_margin(width);
	int bottom = height - (screen_ingame_split_pane(height) ?
		GUTIL_HINT_BOTTOM_SPLIT : GUTIL_HINT_BOTTOM_FULL);
	return (struct gutil_button_bounds) {margin, bottom - GUTIL_HINT_STRIP_H,
		width - 2 * margin, GUTIL_HINT_STRIP_H};
}

int screen_ingame_hotbar_top(int height, int scale) {
	return screen_ingame_row_band(0, height).y - SCREEN_INGAME_ROW_HOTBAR_GAP
		- 22 * scale;
}

void screen_ingame_hud_layout(int width, int height,
	struct screen_ingame_hud_layout* out) {
	const int s = GFX_GUI_SCALE;
	int bar_y = screen_ingame_hotbar_top(height, s);
	int left = (width - 182 * s) / 2;
	out->row = screen_ingame_row_band(width, height);
	/* the bar body plus the selection frame's 1-unit overhang around it */
	out->hotbar = (struct gutil_button_bounds) {left - s, bar_y - s,
		184 * s, 24 * s};
	/* hearts row 10 units above the bar, armour/air above it: 19 units */
	out->hearts = (struct gutil_button_bounds) {left, bar_y - 19 * s,
		182 * s, 18 * s};
}

/* hint-audit-1030: see gui_util.h. Each entry is the role the gameplay code
 * below polls (the open/pause edges read it from here; break/use are the
 * act_pressed(IB_ACTION1/2) sites; jump is the player physics' IB_JUMP). */
const enum input_button screen_ingame_action_role[SCREEN_INGAME_ACT_COUNT] = {
	[SCREEN_INGAME_ACT_OPEN_INVENTORY] = IB_OPEN_INVENTORY, /* Remote A, Classic X, GC Y */
	[SCREEN_INGAME_ACT_JUMP] = IB_JUMP, /* Remote B, Classic A, GC A */
	[SCREEN_INGAME_ACT_BREAK] = IB_ACTION1, /* Nunchuk Z, ZR, R */
	[SCREEN_INGAME_ACT_USE] = IB_ACTION2, /* Nunchuk C, ZL, L */
	[SCREEN_INGAME_ACT_PAUSE] = IB_HOME, /* Home, Home, Start */
};

/* The restored CavEX hint row's CONTENT (b70f38003's removed block, same
 * order): Inventory, Jump, then the target's actions, then Pause.
 *  - entity: its leftClickText (break) and rightClickText (use);
 *  - block: "Use" on a block with onRightClick, else the held item's
 *    "Place" (a block) / "Use" (any other item); then "Mine";
 *  - nothing: "Punch", plus -- new, the eat/air-use fix -- "Eat" for held
 *    food and "Use" for a held item with an open-air use.
 * Buttons come ONLY from screen_ingame_action_role (hint-audit-1030).
 * No sneak state in the row (hint-audit-1030 choice): the SNEAK marker left
 * of the hotbar (ui.sneak_toggle_indicator, default on) is the sneak
 * indicator, and a sixth/seventh cell would crowd the row in a split pane.
 * Pure so tests/test_ui_geometry.c pins the rules; glyphs come from the
 * caller's frame (each pane's own device, input_prompt_frame per player). */
int screen_ingame_hint_row(const struct screen_ingame_hint_ctx* c,
	struct screen_ingame_hint out[SCREEN_INGAME_HINT_MAX]);
int screen_ingame_hint_row(const struct screen_ingame_hint_ctx* c,
	struct screen_ingame_hint out[SCREEN_INGAME_HINT_MAX]) {
	int n = 0;
#define HINT(a, t) do { if(n < SCREEN_INGAME_HINT_MAX) 	out[n++] = (struct screen_ingame_hint) { 		screen_ingame_action_role[SCREEN_INGAME_ACT_##a], (t)}; } while(0)
	HINT(OPEN_INVENTORY, "Inventory");
	HINT(JUMP, "Jump");
	if(c->target == SCREEN_INGAME_HINT_ENTITY) {
		if(c->entity_left) HINT(BREAK, c->entity_left);
		if(c->entity_right) HINT(USE, c->entity_right);
	} else if(c->target == SCREEN_INGAME_HINT_BLOCK) {
		if(c->block_usable)
			HINT(USE, "Use");
		else if(c->held != SCREEN_INGAME_HELD_NONE)
			HINT(USE, c->held == SCREEN_INGAME_HELD_BLOCK ? "Place" : "Use");
		HINT(BREAK, "Mine");
	} else {
		HINT(BREAK, "Punch");
		if(c->held == SCREEN_INGAME_HELD_FOOD)
			HINT(USE, "Eat");
		else if(c->held == SCREEN_INGAME_HELD_AIR_USE)
			HINT(USE, "Use");
	}
	HINT(PAUSE, "Pause");
#undef HINT
	return n;
}

/* The gameplay row as shared hint-row items: Pause is the row's Back (never
 * dropped); on overflow the target's actions stay longest, then Inventory,
 * then Jump. */
void screen_ingame_hint_items(const struct screen_ingame_hint* hints, int n,
	struct gutil_hint_item* items) {
	for(int k = 0; k < n; k++) {
		enum input_button b = (enum input_button)hints[k].button;
		items[k] = (struct gutil_hint_item) {{b, b}, 1, hints[k].label,
			b == screen_ingame_action_role[SCREEN_INGAME_ACT_OPEN_INVENTORY] ? 3
			: b == screen_ingame_action_role[SCREEN_INGAME_ACT_JUMP] ? 2 : 4,
			b == screen_ingame_action_role[SCREEN_INGAME_ACT_PAUSE]};
	}
}

/* Reserve the online socket before drawing the gameplay strip. Keep Pause,
 * dropping the lowest-priority action only when the two collide. */
bool screen_ingame_gameplay_hint_layout(const struct input_player_frame* input,
	int width, int height, bool chat_socket,
	const struct screen_ingame_hint* hints, int count, struct gutil_hint_row* row);
bool screen_ingame_gameplay_hint_layout(const struct input_player_frame* input,
	int width, int height, bool chat_socket,
	const struct screen_ingame_hint* hints, int count, struct gutil_hint_row* row) {
	struct gutil_hint_item items[SCREEN_INGAME_HINT_MAX];
	screen_ingame_hint_items(hints, count, items);
	struct gutil_button_bounds socket = {0};
	if(chat_socket)
		(void)screen_ingame_chat_socket_layout(input, width, height, 0, true,
			NULL, &socket, NULL, NULL, NULL, NULL, NULL);
	while(gutil_hint_row_layout(input, 0, 0, width, height,
			screen_ingame_split_pane(height), GUTIL_HINT_ALIGN_LEFT, items, count, row)) {
		if(!socket.width || row->strip.x + row->strip.width <= socket.x
		   || row->strip.y >= socket.y + socket.height
		   || row->strip.y + row->strip.height <= socket.y)
			return true;
		int drop = -1;
		/* Include items A1 already hid for overflow, so a later layout
		 * cannot resurrect Jump after we remove a higher-priority item. */
		for(int k = 0; k < count; k++)
			if(!items[k].keep && (drop < 0 || items[k].priority <= items[drop].priority))
				drop = k;
		if(drop < 0) return false;
		for(int k = drop; k + 1 < count; k++) items[k] = items[k + 1];
		count--;
	}
	return false;
}

static enum screen_ingame_held screen_ingame_held_kind(
	struct client_session* owner) {
	struct item_data item;
	if(!owner->windows[WINDOWC_INVENTORY]
	   || !inventory_get_hotbar_item(
			  windowc_get_latest(owner->windows[WINDOWC_INVENTORY]), &item)
	   || !item_get(&item))
		return SCREEN_INGAME_HELD_NONE;
	if(item_is_block(&item)) return SCREEN_INGAME_HELD_BLOCK;
	if(item_is_food(item.id)) return SCREEN_INGAME_HELD_FOOD;
	if(item_has_air_use(item.id)) return SCREEN_INGAME_HELD_AIR_USE;
	return SCREEN_INGAME_HELD_OTHER;
}

/* Lead ruling 2026-10-29: the online "Leave Bed" prompt (HOME cell + label)
 * sits with its bottom edge 40 px above the restored CavEX hotbar top,
 * centred, so it clears the hotbar, the hearts/armour/air block (which reaches
 * 19 GUI units = 38 px above the bar) and the bottom hint row -- in a split
 * pane too, since everything derives from the pane's own height. Pure:
 * tests/test_ui_geometry.c checks the clearances. */
void screen_ingame_leave_bed_layout(int width, int height, int glyph_w,
	int label_w, struct gutil_button_bounds* cell,
	struct gutil_button_bounds* label) {
	/* Q5 (10-31): the glyph is 24 px now, on one centre line with the label
	 * (16 px text, 7-row letters: top = centre - 7). */
	int gap = glyph_w ? 6 : 0;
	int x = (width - (glyph_w + gap + label_w)) / 2;
	int y = screen_ingame_hotbar_top(height, GFX_GUI_SCALE) - 40
		- GUTIL_HINT_GLYPH;
	*cell = (struct gutil_button_bounds) {x, y, glyph_w, GUTIL_HINT_GLYPH};
	*label = (struct gutil_button_bounds) {x + glyph_w + gap,
		y + GUTIL_HINT_GLYPH / 2 - 7, label_w, 16};
}

#define SNEAK_LABEL_GAP 8
void screen_ingame_sneak_rail_layout(int width, int height, int scale,
	int text_w, struct gutil_button_bounds* rail,
	struct gutil_button_bounds* text);
void screen_ingame_sneak_rail_layout(int width, int height, int scale,
	int text_w, struct gutil_button_bounds* rail,
	struct gutil_button_bounds* text) {
	int hotbar_left = (width - 182 * scale) / 2;
	int hotbar_top = screen_ingame_hotbar_top(height, scale);
	rail->x = hotbar_left + 2 * scale;
	rail->y = hotbar_top;
	rail->width = 178 * scale;
	rail->height = 2 * scale;
	/* Maintainer 2026-10-30 (review hub "sneak-bar": "I like the sneak to
	 * the left of the hotbar like we had it"): the label sits IMMEDIATELY
	 * left of the bar -- right edge SNEAK_LABEL_GAP px before the bar's left
	 * edge, whatever the font measures -- and is centred on the bar's own
	 * height, so it stays inside the bar's row: below the hearts/armour/air
	 * block, above the hint row, never under the centred Leave Bed prompt. */
	text->x = hotbar_left - SNEAK_LABEL_GAP - text_w;
	text->y = hotbar_top + (22 * scale - 16) / 2;
	text->width = text_w;
	text->height = 16;
}

/* Ruling item 5 + item 8 + the capture lever. `config_on` is
 * ui.sneak_toggle_indicator, which hides ONLY the marker; `capture` is
 * debug.sneak_indicator_capture, which forces the DRAW so a PC or console
 * capture rig can photograph it -- draw only, it latches/slows/sneaks
 * nothing. The config is the outer gate on purpose: `false` means the marker
 * is off, including for a rig. */
bool screen_ingame_sneak_marker_visible(bool latched, bool config_on,
	bool capture, bool toast_visible, bool modal);
bool screen_ingame_sneak_marker_visible(bool latched, bool config_on,
	bool capture, bool toast_visible, bool modal) {
	/* Ruling 7 V5: the transient bed toast suppresses the marker outright --
	 * 0 px overlap, not a moved or dimmed marker -- and nothing here
	 * remembers that it did, so the marker returns on the first frame the
	 * toast is gone if the toggle is still latched. The term sits OUTSIDE the
	 * (latched || capture) pair on purpose: the ruling gives the capture
	 * lever no exemption, so a rig frame can never photograph the two
	 * overlapping either.
	 *
	 * `modal` is the same screen_ingame_hud_modal_active answer the socket
	 * takes (review 28 M1): the rail and its label are covered, not
	 * suppressed, by the recovery modals and the single-view local pause, and
	 * A.3 forbids exactly that. It sits outside the capture pair for the same
	 * reason the toast does. */
	return config_on && (latched || capture) && !toast_visible && !modal;
}

/* ui.sneak_toggle_indicator: JSON true/false, also 1/0, ABSENT = TRUE.
 *
 * Maintainer 2026-10-30 (review hub "sneak-bar"): the marker is an option that
 * is ON by default -- ruling 7 (D)'s off-until-approved default is lifted with
 * the left-of-hotbar placement (maintainer yes 2026-10-31). A config that does not mention the key, or that
 * cannot be read at all, shows the marker; `false` hides it. It is stated
 * explicitly as `true` in config_wii.json and both presets so nobody has to
 * know what an absent key means.
 *
 * config_read_int understands only JSONNumber, so the boolean form is read
 * through parson here and the numeric form falls through to it. config.c has
 * no config_read_bool and growing one is not this change's to make. */
/* A5: a ui.* switch read like ui.sneak_toggle_indicator -- JSON true/false
 * or 1/0, absent = true. */
bool screen_ingame_ui_flag(struct config* c, const char* key) {
	if(!c || !c->root)
		return true;
	JSON_Object* root = json_object(c->root);
	if(root && json_object_dothas_value_of_type(root, key, JSONBoolean))
		return json_object_dotget_boolean(root, key) == 1;
	return config_read_int(c, key, 1) != 0;
}

int screen_ingame_coords_layout(bool coords, int coords_w, bool fps, int fps_w,
	struct gutil_button_bounds out[2]) {
	int n = 0, y = SCREEN_INGAME_COORDS_Y;
	if(coords) {
		out[n++] = (struct gutil_button_bounds) {SCREEN_INGAME_COORDS_X, y,
			coords_w + 2, 18};
		y += SCREEN_INGAME_COORDS_PITCH;
	}
	if(fps)
		out[n++] = (struct gutil_button_bounds) {SCREEN_INGAME_COORDS_X, y,
			fps_w + 2, 18};
	return n;
}

void screen_ingame_block_pos(const float pos[3], int out[3]) {
	out[0] = (int)floorf(pos[0]);
	out[1] = (int)floorf(pos[1] - PLAYER_EYE_HEIGHT);
	out[2] = (int)floorf(pos[2]);
}

bool screen_ingame_card_hides_hud(bool pane_recovery, bool primary_recovery) {
	return pane_recovery || primary_recovery;
}

bool screen_ingame_sneak_marker_config(struct config* c);
bool screen_ingame_sneak_marker_config(struct config* c) {
	if(!c || !c->root)
		return true;
	JSON_Object* root = json_object(c->root);
	if(root
	   && json_object_dothas_value_of_type(root, "ui.sneak_toggle_indicator",
										   JSONBoolean))
		return json_object_dotget_boolean(root, "ui.sneak_toggle_indicator")
			== 1;
	return config_read_int(c, "ui.sneak_toggle_indicator", 1) != 0;
}

/* Drawn INSIDE the hotbar pass (A.3), which is why it takes no texture or
 * blend state of its own: texture_gui_widgets is already bound by the bar
 * body quad immediately above it, and the blend state is the 2D pass's own
 * MODE_BLEND -- what gfx_mode_gui_viewport enters every viewport with
 * (source/platform/pc/gfx.c:412-413, source/platform/wii/gfx.c:871-872).
 * The rail's texel is alpha 255 in every pack, so MODE_BLEND composites it
 * to exactly the same pixels an unblended draw would produce; it wants no
 * bracket of its own either way.
 *
 * (Review 28 M3: this comment used to claim the state here was MODE_OFF.
 * That was false in single player -- nothing had turned blending off -- and
 * true in a remote session only because the chat socket was leaking
 * MODE_OFF, which is the defect H1 fixed. The state is now MODE_BLEND on
 * every path, and check_hud_blend_state pins it instead of prose.) */
static void sneak_rail_draw(int width, int height, int scale) {
	struct gutil_button_bounds rail, text;
	screen_ingame_sneak_rail_layout(width, height, scale,
									gutil_font_width("SNEAK", 16), &rail,
									&text);
	/* THE RAIL IS ONE gui.png TEXEL: (1,23), the top-left BRIGHT pixel of the
	 * selection frame, stretched over the whole 356x4 destination (lead
	 * implementation ruling). That makes the colour the pack's own
	 * @selection BY CONSTRUCTION, with no CPU pixel inspection, no per-frame
	 * token lookup and no change to the texture loader.
	 *
	 * MEASURED at (1,23), not assumed (review 28 L3 corrects the earlier
	 * wording): the BUNDLED assets/gui/gui.png is already (222,204,143,255)
	 * -- byte-identical to artwork/cc0-pack/earth-and-ash/gui/gui.png -- so
	 * that cream IS what a DEFAULT install shows, not an Earth-and-Ash-only
	 * look; a genuine Beta 1.7.3 gui.png reads (255,255,255,255) there and
	 * gives the rail Beta's selection white. (Spec A.2's own Beta @selection
	 * design token is (247,249,247); that is the token, not this texel, and
	 * the by-construction mechanism deliberately follows the texel.) The
	 * GLYPH sheet is a different question and has a different answer: the
	 * bundled controller artwork is forced under every pack (texture.c), per
	 * the maintainer's "keep the official Wii glyph for 2 on the earth and
	 * ash pack".
	 *
	 * FLATNESS: the GUI atlas is loaded with linear = false (texture.c
	 * TEX_LOAD_FILE "gui/gui.png" ... false), so both backends MAGNIFY it
	 * with a point sampler -- GL_NEAREST on PC, GX_NEAR on the Wii -- and a
	 * 1x1 source rectangle has exactly one texel to sample. Every
	 * destination pixel centre interpolates strictly inside [1,2)x[23,24) in
	 * texel space, so there is no second texel for a filter to bleed in from
	 * and the result cannot be anything but flat. Both packs' texel is opaque
	 * (alpha 255), which is A.2's "opaque". */
	gutil_texquad(rail.x, rail.y, 1, 23, 1, 1, rail.width, rail.height);
	/* A.2: white 16 px with gutil_text's native hard shadow -- offset (2,2),
	 * RGB (63,63,63) -- and NO backing box. "The native dark shadow supplies
	 * separation on bright terrain; this is the intended design." */
	gutil_text(text.x, text.y, "SNEAK", text.height, true);
}

void screen_ingame_render2D_owned(struct client_session* owner,
	const struct input_player_frame* input, int width, int height) {
	char str[192];
	/* This viewport's owner, once: both the H-C2 socket and the H-S1 rail are
	 * per-owner rules (A.4), and main.c calls this function once per viewport
	 * with that viewport's own owner and prompt frame. */
	unsigned owner_slot = owner == &gstate.players[1] ? 1 : 0;
	/* Which modal OWNS this viewport (A.3; review 28 H2 + M1). Taken once,
	 * before either gate, so the socket and the sneak rail can never disagree
	 * about it inside one frame. The PRIMARY recovery term is read under the
	 * same owner condition its draw at the end of this function is gated on,
	 * so a guest viewport is not suppressed by a modal that is not painted
	 * into it. */
	bool hud_modal = screen_ingame_hud_modal_active(
		gstate.players[owner_slot].menu_host.recovery.phase
			!= CONTROLLER_RECOVERY_NONE,
		owner == &gstate.players[0]
			&& screen_controller_recovery_state()->phase
				!= CONTROLLER_RECOVERY_NONE,
		local_pause_active(&owner->pause));
	/* Wii feedback 10-31 (split_play_2m on 018b4ba25): while THIS pane's
	 * disconnect card is up, the pane shows only the card over the world --
	 * no hint row, hotbar, crosshair or hearts (no stray half-HUD). */
	bool card_up = screen_ingame_card_hides_hud(
		gstate.players[owner_slot].menu_host.recovery.phase
			!= CONTROLLER_RECOVERY_NONE,
		owner == &gstate.players[0]
			&& screen_controller_recovery_state()->phase
				!= CONTROLLER_RECOVERY_NONE);

	// P4(a) HUD toast (CRPC_HUD_MESSAGE): the Beta bed strings, ~3s, centered above the
	// hotbar. Beta prints these to chat; this engine has none, so a transient toast is
	// the Wii-playable stand-in. Drawn first so crosshair/overlay stay on top.
	if(owner->hud_message[0]) {
		// time_reached, never subtraction: Wii ptime_t is unsigned ticks, so
		// time_diff_ms(now, until) after expiry computes (until - now) on u64, wraps to
		// a huge positive int and the toast NEVER went away (2026-07-30 hardware find --
		// the exact in-flight-unsigned-difference trap the wd_uptime_ticks notes name).
		// (A bare `>=` was right on Wii but doesn't compile on PC's struct-timespec
		// ptime_t -- time_reached is the portable deadline primitive.)
		// Clear the message so the check costs nothing once expired.
		if(time_reached(time_get(), owner->hud_message_until)) {
			owner->hud_message[0] = 0;
		} else {
			// Above BOTH the hotbar and the hearts row: 48*scale sat behind them on
			// hardware (half-hidden "You..." in the 2026-07-30 photos).
			int tw = (int)strlen(owner->hud_message) * 8 * GFX_GUI_SCALE;
			gutil_text((width - tw) / 2, height - 72 * GFX_GUI_SCALE,
					   owner->hud_message, 8 * GFX_GUI_SCALE, true);
		}
	}

	// admit-retry-0925 (Astra review 3 item 2): a Player 2 admission failure
	// keeps its OWN line on the host HUD, one row above the toast, until the
	// next Join clears it. Ordinary toasts never overwrite or shorten it.
	if(owner == &gstate.players[0]) {
		const char* reason
			= client_session_local_admit_failure_reason(&gstate.players[1]);
		if(reason) {
			int tw = (int)strlen(reason) * 8 * GFX_GUI_SCALE;
			gutil_text((width - tw) / 2, height - 84 * GFX_GUI_SCALE, reason,
					   8 * GFX_GUI_SCALE, true);
		}
	}

	// Beta's chat block [740c583 GuiIngame.renderGameOverlay]: bottom-left,
	// the newest line at scaledHeight - 48, older lines stacked upward, at
	// most 10 while the composer is closed, each hidden at 200 ticks with the
	// quadratic fade (chat_hud.c). This HUD sits one control-hint row
	// (16 * scale * 8 / 5, see the hotbar quad below) higher than Beta's, so
	// the block keeps Beta's 48 relative to the raised hotbar: 48 + 26.
	// The composer screen draws the open branch.
	/* Build spec 15 A.3: during an ELIGIBLE MULTIPLAYER SESSION the bottom of
	 * the rendered history bounds -- shadow and backing included -- is
	 * clamped to H-88, the ceiling that keeps the block off the socket's
	 * "Tap" row. Beta's closed strip is a fixed 320 LOGICAL units wide, i.e.
	 * the whole 640 px viewport at GUI scale 2, so the block does reach the
	 * socket's column and a vertical ceiling is the right instrument.
	 *
	 * "Keep any existing higher position": the shipped H-148 anchor puts the
	 * bottom at H-130, which already clears the ceiling by 42 px in BOTH
	 * viewport heights, so this is a guard rather than a move -- and single
	 * player never reaches it at all, which is what keeps that path
	 * byte-identical. The composer's own band (chat_hud_render_open, from
	 * screen_chat.c) is a different call and is untouched. */
	int chat_base = height - 74 * GFX_GUI_SCALE;
	if(gstate.remote_session && screen_ingame_chat_socket_owner(owner_slot))
		chat_base = chat_hud_clamp_base_y(chat_base, height - 88);
	chat_hud_render(&gstate.chat_ring, 2 * GFX_GUI_SCALE, chat_base, false, 10);
	/* H-C2 chat socket (docs/mp_chat_auth_research_2026-09-11.md decision
	 * item 4, rebuilt to build spec 15 A.1/A.3/A.4). A remote session shows
	 * how to open the composer for as long as the session is eligible -- A.4
	 * makes it PERSISTENT, so the retired first-use gate is gone -- with the
	 * RESOLVED binding cell and the pause-menu route when unbound. It sits in
	 * the bottom-right corner of THAT player's viewport, right of the hotbar
	 * and clear of everything on the HUD (the clearances are asserted, not
	 * assumed); it is its own draw, never the hud_message toast slot.
	 *
	 * The capture lever (brief 25 item 4, extended by brief 26 to substitute
	 * the cell as well) is read ONCE for the same reason the sneak keys are:
	 * no parson dot-path lookup per frame per viewport on the Wii, and
	 * neither key is rewritten while the game runs.
	 *
	 * "NOT READ YET" IS A SEPARATE FLAG, never a value of the lever's own
	 * domain (review 28 M2). The lever's off-table explicitly includes
	 * negatives -- check_chat_socket_capture sweeps {0, -1, 4, 99, 2026} --
	 * so the old `static int = -1; if(< 0)` sentinel meant a config carrying
	 * `debug.chat_hint_capture: -1` (or `+set:debug.chat_hint_capture=-1`)
	 * stored -1, kept failing the test, and re-ran the parson dot-path lookup
	 * EVERY frame, per viewport, on the Wii main thread -- precisely the cost
	 * this cache exists to avoid. The sneak block below carries the same flag
	 * for the same reason. */
	static int cfg_socket_capture;
	static bool cfg_socket_capture_loaded;
	if(!cfg_socket_capture_loaded) {
		cfg_socket_capture = config_read_int(&gstate.config_user,
											 "debug.chat_hint_capture", 0);
		cfg_socket_capture_loaded = true;
		screen_ingame_socket_capture_read_counted();
	}
	enum input_category cap_category = INPUT_CAT_NONE;
	int cap_code = 0, cap_glyph = -1;
	bool cap_is_tap = false;
	bool socket_capture = screen_ingame_chat_socket_capture(
		cfg_socket_capture, &cap_category, &cap_code, &cap_is_tap, &cap_glyph);
	/* A.4's two runtime suppressions: every modal that owns this viewport
	 * (hud_modal above -- both recovery machines and the local pause, not
	 * just the per-owner menu_host phase the F3 gate below happens to read)
	 * and this owner's own join prompt. */
	if(screen_ingame_chat_socket_visible(
		   gstate.remote_session, screen_ingame_chat_socket_owner(owner_slot),
		   hud_modal, input && input->join_prompt, socket_capture)) {
		/* The accessor is the ONLY source of the binding. It answers false
		 * when nobody owns this player (or the family binds no chat code),
		 * and leaves both out-parameters untouched -- the -1 then names no
		 * family direction and no gesture, so the socket falls back to the
		 * resolver's own cell with no "Tap". */
		int code = -1;
		bool is_tap = false;
		(void)input_player_chat_opener(owner_slot, &code, &is_tap);
		const struct input_player_frame* socket_input = input;
		struct input_player_frame captured;
		if(socket_capture && input) {
			/* DRAW ONLY: the lever substitutes the family, the code, the tap
			 * semantics and -- brief 26, unlike the ruling-8 lever -- the
			 * encoded GLYPH, so a PC capture shows the REMOTE cell it was
			 * asked for instead of PC KEY/MSE artwork. It opens nothing,
			 * latches nothing and writes no game state. */
			captured = *input;
			captured.category = cap_category;
			captured.glyphs[IB_CHAT] = cap_glyph;
			socket_input = &captured;
			code = cap_code;
			is_tap = cap_is_tap;
		}
		/* The socket places ITSELF against this viewport's right and bottom
		 * edges, so the draw takes the viewport, not a slot. */
		screen_ingame_chat_socket_draw(socket_input, width, height, code,
									   is_tap);
	}
	// Beta's sleep fade [740c583 GuiIngame.java:150-163]: a 0x101020 wash whose alpha
	// ramps 0->220 across the 100-tick wind-up, then back out over the 10-tick wake
	// tail. Without it a bed click produced NO feedback for five seconds and then day
	// teleported in -- the "bed mechanics are delayed" report from the 2026-07-30
	// console run. Values (tint, 220 peak, both ramp legs) are Beta's exactly.
	//
	// DEVIATION (docs/deviations.md): Beta draws this after the hotbar and before the
	// debug lines, dimming the HUD but not F3. This engine draws the HUD *last*, so
	// the same slot dims the world and leaves the hotbar lit. Keeping Beta's position
	// relative to the DEBUG OVERLAY is the deliberate half: every hardware capture
	// must show a readable build stamp, and a 220-alpha wash over F3 would blind the
	// proof workflow. The player still gets the feedback the report asked for.
	if(owner->sleep_timer > 0) {
		float ramp = (float)owner->sleep_timer / 100.0F;
		if(ramp > 1.0F)
			ramp = 1.0F - (float)(owner->sleep_timer - 100) / 10.0F;
		if(ramp > 0.0F) {
			// gfx_texture(false) FIRST -- this is a flat colour wash, not a
			// textured quad, and TEV stage 0 stands at GX_MODULATE. Without the
			// bracket the wash is multiplied by whatever texture happens to be
			// bound at this point in the frame, so its colour is undefined and
			// it can be alpha-tested away entirely. Every other fullscreen dim
			// in the GUI brackets this way (screen_chest.c:254-256 et al);
			// omitting it was an adversarial-review finding, 2026-07-31.
			gfx_texture(false);
			gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0x10, 0x10, 0x20,
							  (uint8_t)(220.0F * ramp));
			gfx_texture(true);
		}
	}
	/* MP-BED: GuiSleepMP's one control, "Leave Bed"
	 * (multiplayer.stopSleeping), centred at height - 40 GUI units
	 * (GuiSleepMP.java:9). Shown while the online player is in bed; HOME
	 * activates it (screen_ingame_update_slot). Placement in pixels at this
	 * HUD's 2x GUI scale. */
	if(gstate.remote_session && owner == &gstate.players[0] && owner->sleeping) {
		/* critique-1028: the owner's own HOME cell (the key that sends the
		 * leave, see screen_ingame_update_slot) in front of the label, laid
		 * out like every footer prompt: 32 px cell, label 6 px after, label
		 * y = cell y + 8, the pair centred. Existing glyph cells only. */
		const char* leave = "Leave Bed";
		int lw = gutil_font_width(leave, 16);
		struct input_player_frame bed_prompt;
		input_prompt_frame(0, &bed_prompt);
		int glyph = bed_prompt.glyphs[IB_HOME];
		int gw = gutil_prompt_glyph_width(glyph, GUTIL_HINT_GLYPH);
		struct gutil_button_bounds cell, label;
		screen_ingame_leave_bed_layout(width, height, gw, lw, &cell, &label);
		if(gw)
			(void)gutil_prompt_glyph(cell.x, cell.y, GUTIL_HINT_GLYPH, glyph);
		gutil_text(label.x, label.y, leave, 16, true);
	}

	// Runtime-gated F3-style overlay (was compile-time #ifndef NDEBUG, which
	// meant it never showed in the release Wii build). Toggled by
	// IB_DEBUG_OVERLAY / the "debug.overlay" config flag. Lines 0-4 are the
	// original debug readout; line 5 adds the platform-perf-milestone gauges
	// (mesher backlog + heap) so the Task 4 priority and Task 5 pooling
	// behaviour can be judged live; line 6 is the looked-at block/entity.
	bool recovering = gstate.players[0].menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE
		|| gstate.players[1].menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE;
	bool compact_debug = gstate.players[1].local_gameplay_ready || height < gfx_height();
	if(gstate.debug_overlay && !recovering && !compact_debug) {

	// CW_BUILD_ID = short git hash + optional dirty-tree digest + build date, baked in by the
	// build system (CMake + Makefile). Every screenshot/video of the overlay
	// now self-identifies its build -- no more md5 bookkeeping to figure out
	// which .dol a piece of field footage came from.
#ifndef CW_BUILD_ID
#define CW_BUILD_ID "unstamped"
#endif
	// Header format (2026-08-04): "CaveWii (impl. B1.7.3) <build id>". The old
	// "Alpha %i.%i.%i_f%i" version block was dropped on purpose -- the _f suffix
	// was the fCavEX FORK revision, which reads as nonsense next to the CaveWii
	// name and told you nothing this line does not. The build id is the only
	// token that actually distinguishes one binary from another, and it is what
	// every screenshot/field-capture is matched against.
	// Passed through %s rather than used AS the format string: CW_BUILD_ID is
	// generated by the build system, and a stray '%' in it would otherwise be a
	// format-string bug reading garbage off the stack.
	snprintf(str, sizeof(str), "%s", GAME_NAME " (impl. B1.7.3) " CW_BUILD_ID);
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 0, str, GFX_GUI_SCALE * 8, true);


	snprintf(str, sizeof(str), "%0.1f fps, wait: gpu %0.1fms, vsync %0.1fms",
			gstate.stats.fps, gstate.stats.dt_gpu * 1000.0F,
			gstate.stats.dt_vsync * 1000.0F);
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 1, str, GFX_GUI_SCALE * 8, true);

	snprintf(str, sizeof(str), "%zu chunks, mesh q:%d stale:%u",
			 gstate.stats.chunks_rendered,
			chunk_mesher_pending(),
			// CR-08: cross-thread read of a plain uint32_t (main-thread
			// writer; also read by the Wii watchdog thread's hang-report
			// dump) -- __atomic_load_n instead of a bare load.
			(unsigned)__atomic_load_n(&mesh_stale_discards, __ATOMIC_RELAXED));
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 2, str, GFX_GUI_SCALE * 8, true);

	snprintf(str, sizeof(str), "(%0.1f, %0.1f, %0.1f) (%0.1f, %0.1f)",
			 owner->camera.x,
			owner->camera.y, owner->camera.z, glm_deg(owner->camera.rx),
			glm_deg(owner->camera.ry));
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 3, str, GFX_GUI_SCALE * 8, true);

float time = gstate.world_time + time_diff_s(gstate.world_time_start, time_get()) * 1000.0f / 50.0f;
float day_ticks = fmodf(time, 24000.0f);
float angle = daytime_celestial_angle(day_ticks / 24000.0f);
	snprintf(str, sizeof(str), "time: %.0f (%.0f)  angle: %.3f", time,
			 day_ticks, angle);
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 4, str, GFX_GUI_SCALE * 8, true);

#ifdef PLATFORM_WII
	// Heap usage from newlib's allocator. uordblks trending up over a session
	// is the leak/fragmentation signal; this is the dial to watch while the
	// Task 2 arena and Task 5 pool changes soak. (mallinfo is a newlib API;
	// the PC/UCRT toolchain doesn't expose it, hence the Wii guard.)
	struct mallinfo mi = mallinfo();
	snprintf(str, sizeof(str), "heap: used %dKB free %dKB", mi.uordblks / 1024,
			mi.fordblks / 1024);
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 5, str, GFX_GUI_SCALE * 8, true);

	// MEM1 vs MEM2 free arena. The malloc heap (and today, textures) lives in
	// MEM1; MEM2 should read ~near its full 64MB if it is unused. This is the
	// dial for the MEM2 texture-allocation work (HD packs).
	// MEM2 free here is SYS_GetArena2Size = the malloc-spill region ABOVE the 16MB MEM2
	// carve, NOT the carve where display lists live -- so it can read tens of MB while the
	// carve is fragmented/full. "spill"/"oom" expose that blind spot: spill = mem2_allocs
	// the carve couldn't satisfy (fell back to MEM1), oom = true NULL allocs. "trunc" =
	// chunk meshes DROPPED because an OOM truncated them mid-emit. If trunc (or oom)
	// climbs in the seconds before a freeze, the partial-mesh -> GP-wedge hang is live.
	snprintf(str, sizeof(str),
			 "MEM1 free %dKB  MEM2 free %dKB | spill %zu oom %zu trunc %u estk %u",
			(int)(SYS_GetArena1Size() / 1024), (int)(SYS_GetArena2Size() / 1024),
			mem2_stat_carve_spill(), mem2_stat_oom(),
			// CR-08: cross-thread reads (mesher-worker and server-thread
			// writers respectively; both also read by the Wii watchdog
			// thread's hang-report dump) -- __atomic_load_n, not bare loads.
			(unsigned)__atomic_load_n(&g_mesh_truncations, __ATOMIC_RELAXED),
			(unsigned)__atomic_load_n(&g_evict_stuck, __ATOMIC_RELAXED));
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 9, str, GFX_GUI_SCALE * 8, true);

	// Display-list health (2026-06-21 GARBLE probe). dlbad = finalized lists whose declared
	// vtxcnt != bytes written (finalize floored the tail); dlcall = GX_CallDispList refused
	// because the list struct was corrupt. These were previously surfaced ONLY in the
	// freeze hang-report -- but the redstone GARBLE never freezes, so it was unreadable. Now
	// LIVE on F3: photograph this row WHILE the cyan far-region garble is on screen. A nonzero
	// dlbad/dlcall NAMES a malformed/corrupt-CALL list (the far-ring LOD producer is the prime
	// suspect); BOTH 0 during a visible garble => the corruption is GP-side coherency, not a
	// CPU-emitted bad list (escalate to a GX FIFO/state trace). MUST normally be 0.
	// ilist = bounded chunk-list walks ABANDONED (world.c's corrupt-chain guard -- the fix
	// for the 2026-07-27 world_build_chunks spin). It belongs on this row because it is the
	// same failure family: a corrupt structure the shipping build's compiled-out asserts
	// cannot catch. It is NOT a warning -- each bail deliberately leaks that frame's chunk
	// references rather than double-free them, so a climbing ilist IS the leak, and until
	// this row existed nothing in the running game ever read the counter. MUST stay 0.
	// cguard = MEM1 red-zone violations on struct chunk -- the allocation holding the very
	// intrusive-list nodes `ilist` reports a corrupt walk of, and the one the MEM2 canary
	// ("cnry") is structurally blind to (it guards mem2_alloc blocks only, which is why it
	// read 0 through the whole 2026-07-27 investigation). Printed as hits@offset: the offset
	// is the first corrupt byte INSIDE the guard, and a value that repeats across runs is the
	// stray writer's stride -- i.e. its fingerprint. MUST stay 0.
	{
		extern uint32_t g_displaylist_malformed, g_displaylist_badcall;
		size_t cguard_off;
		chunk_guard_first(NULL, &cguard_off); // base pointer only matters in the hang report
		snprintf(str, sizeof(str), "dlbad %u dlcall %u ilist %u cguard %u@%u",
				(unsigned)g_displaylist_malformed,
				(unsigned)g_displaylist_badcall,
				(unsigned)world_ilist_overrun_count(),
				(unsigned)chunk_guard_hits(), (unsigned)cguard_off);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 18, str, GFX_GUI_SCALE * 8, true);
	}
#endif

	// LEVER 1 (render-distance measurement enabler, 2026-06-22): the SERVER tick CPU
	// split, surfaced in NORMAL play (was perf_stress-SD-dump-only). rt = random-tick
	// floor, wt = scheduled-tick/fluid-cascade cost (server_local.c g_srv_tick_*).
	// Read together with the "wait: gpu/vsync" row above to NAME the limiter on real
	// hardware: high rt/wt => CPU/tick-bound (the MEM2-dict storm); high gpu wait =>
	// GPU draw-ceiling-bound. This is the Step-0 instrument gating any render-distance
	// increase. Memory-only -- no SD I/O on the tick thread (that would itself spike).
	{
		extern int g_srv_tick_rt_us, g_srv_tick_wt_us;
		snprintf(str, sizeof(str), "srv tick: rt %dus wt %dus", g_srv_tick_rt_us,
				g_srv_tick_wt_us);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 19, str, GFX_GUI_SCALE * 8, true);
	}

	// Row 20: S0 audio dials -- fixed PCM-arena fill (the MEM2-dial
	// precedent: decode-at-load claims it once, so this is a load-time
	// constant per pack; a full arena logs + skips samples at load) and
	// the live one-shot voice count out of the fixed pool.
	snprintf(str, sizeof(str), "snd: arena %u/%uKB voices %d",
			(unsigned)(audio_arena_used() / 1024),
			(unsigned)(audio_arena_total() / 1024), audio_voices_active());
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 20, str, GFX_GUI_SCALE * 8,
			   true);

	// Row 21: ON-SCREEN WPAD DISCRIMINATOR (Batch E item 2, 2026-08-23
	// HBC-direct RCA -- wiimote-dead-direct-launch investigation). Mirrors
	// the wpad_probe0 heartbeat telemetry field (hw_telemetry.h) so a human
	// at the console can read the raw WPAD_Probe(chan 0) verdict with ZERO
	// network: -2 NOT_READY = the BT stack is stuck mid-async-enable
	// (libogc-side, the 08-18 "LED solid, data starved" symptom class);
	// -1 NO_CONTROLLER = the stack is healthy/idle, nothing paged in yet;
	// 0 = a controller answered. PC builds have no WPAD and print the same
	// -99 sentinel the telemetry heartbeat uses. Debug-overlay-gated only
	// (this whole block is `if(gstate.debug_overlay)`), never the normal HUD.
	{
		int wpad_probe0 =
#ifdef PLATFORM_WII
			input_wpad_probe0();
#else
			-99;
#endif
		unsigned int wpad_held0 =
#ifdef PLATFORM_WII
			input_wpad_raw_held0();
#else
			0;
#endif
		// held: the RAW WPAD_ButtonsHeld mask. probe0==0 + held stuck at 0x0
		// while a human mashes buttons = the data pipe below WPAD is silent
		// (the 2026-08-23 attached-but-starved verdict); held changing =
		// WPAD data flows and any remaining deadness is app-side mapping.
		int wderr = -99; unsigned int wdp = 0, wbat = 0;
#ifdef PLATFORM_WII
		input_wpad_raw_diag0(&wderr, &wdp, &wbat);
#endif
		// HELD FIRST (left, never covered by the HUD hearts -- 2026-08-23
		// field lesson); then the WPADData internals: e=err, dp=data_present
		// bitmask, b=battery (0 with e=0 -> NO status report has EVER
		// arrived: silence sits below wiiuse's event layer). p=probe0 last.
		unsigned int wksp =
#ifdef PLATFORM_WII
			input_lwp_wkspace_free();
#else
			0;
#endif
		// w = lwp kernel workspace FREE BYTES -- the attach-time wiiuse queue
		// pool (the cornered suspect). ~0 here + stage-server-plenty =
		// exhaustion confirmed.
		snprintf(str, sizeof(str), "wpad h%08lx e%d dp%x b%u p%d w%u",
				 (unsigned long)wpad_held0, wderr, wdp, wbat, wpad_probe0,
				 wksp);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 21, str, GFX_GUI_SCALE * 8,
				   true);
	}

	// Row 22: region-archive I/O counters (T1 measurement instrumentation,
	// 2026-08-24 Wii-only world-load-stall investigation --
	// docs/resource_lessons_review_2026-08-24.md section (b)). Process-
	// lifetime totals, so reading this row once a long world load finishes
	// answers "how much region I/O did that load actually do": fopen should
	// track the number of DISTINCT regions touched (the LRU caps it at
	// MAX_REGIONS resident, region_archive.h), never the chunk count: a
	// climbing fopen alongside a climbing chunk count on the loading screen
	// would mean the per-region handle cache regressed. fread/bytes are the
	// coalesced-record-read proof: compare against a same-fixture run from
	// before this commit to see the syscall-count drop the commit claims.
	{
		uint32_t region_fopen = 0, region_fread = 0;
		uint64_t region_bytes = 0;
		region_archive_io_stats(&region_fopen, &region_fread, &region_bytes);
		snprintf(str, sizeof(str), "region io: fopen %u fread %u bytes %uKB nbt-full %u",
				 (unsigned)region_fopen, (unsigned)region_fread,
				 (unsigned)(region_bytes / 1024), item_nbt_exhaustions());
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 22, str, GFX_GUI_SCALE * 8,
				   true);
	}

	// Per-phase main-thread CPU cost (ms). bf=world_pre_render reach/BFS,
	// li=lighting, lo=LOD tier assignment, bd=chunk_mesher_send dispatch/copy.
	snprintf(str, sizeof(str),
			 "cpu: bf%.1f li%.1f lo%.1f bd%.1f rv%.1f rn%.1f tk%.1f",
			gstate.stats.ms_bfs, gstate.stats.ms_light, gstate.stats.ms_lod,
			gstate.stats.ms_build, gstate.stats.ms_recv, gstate.stats.ms_render,
			gstate.stats.ms_tick);
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 6, str, GFX_GUI_SCALE * 8, true);

	// Mesher worker build cost — the heaviest single-core CPU consumer
	// when chunks stream in. Last + session max.
	float mb_last = 0.0F, mb_max = 0.0F;
	chunk_mesher_build_ms(&mb_last, &mb_max);
	snprintf(str, sizeof(str),
			 "cmax bf%.0f li%.0f lo%.0f bd%.0f gap%u mesh %.1f/%.1f",
			gstate.stats.ms_bfs_max, gstate.stats.ms_light_max,
			gstate.stats.ms_lod_max, gstate.stats.ms_build_max,
			(unsigned)gstate.stats.wd_hb_gap_max_ms, mb_last, mb_max);
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 7, str, GFX_GUI_SCALE * 8, true);

	// Sub-phase split of the worst build — vertex light / emit / BFS /
	// finalize — to see what dominates the cold-chunk stall.
	float mb_vl = 0.0F, mb_em = 0.0F, mb_bfs = 0.0F, mb_fin = 0.0F;
	chunk_mesher_build_breakdown(&mb_vl, &mb_em, &mb_bfs, &mb_fin);
	snprintf(str, sizeof(str), "max split: vl %.0f em %.0f bfs %.0f fin %.0f",
			 mb_vl, mb_em,
			mb_bfs, mb_fin);
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 8, str, GFX_GUI_SCALE * 8, true);

	// Worldgen generation throttle: wall time + chunks generated / deferred in
	// the last frontier cascade (server_world_ensure_populated). deferred > 0 is
	// the throttle capping the cascade (working as intended). On real Wii
	// hardware, widen SERVER_GEN_BUDGET_MS if the frontier lags, tighten it if a
	// hitch persists. (Row 10, below the mesher split.)
	int gen_ms = 0, gen_n = 0, gen_def = 0;
	server_world_gen_stats(&gen_ms, &gen_n, &gen_def);
	snprintf(str, sizeof(str),
			 "gen: %dms %dc def %d  lightflood %dus  cflood %u  ncas %uus",
			 gen_ms,
			gen_n, gen_def, server_world_light_flood_us(),
			(unsigned)lighting_client_flood_max_pops(),
			(unsigned)server_world_notify_cascade_max_us());
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 10, str, GFX_GUI_SCALE * 8, true);

	// Dungeon-spawner pass cost (the +-16 block scan) + spawners tracked, so the
	// brute-force scan is measurable on Dolphin/Wii before more ticking systems.
	int spwn_us = 0, spwn_n = 0;
	server_local_spawner_stats(&spwn_us, &spwn_n);
	int biome_rain = 0, sky = 0, fire_ticks = 0, wet = 0;
	if(owner->local_player && gstate.world_loaded) {
		int bp[3];
		screen_ingame_block_pos(owner->local_player->pos, bp);
		int x = bp[0], y = bp[1], z = bp[2];
		uint8_t gi, gj;
		if(world_get_section_tint_at(world_view_current(), x, z, &gi, &gj))
			biome_rain = precip_biome_enable_rain(
				precip_biome_from_tint(gi, gj));
		sky = world_get_height(world_view_current(), x, z) <= y;
		fire_ticks = owner->local_player->on_fire ? 1 : 0;
		wet = ((owner == &gstate.players[0]) && gstate.players[0].in_water)
			|| (gstate.weather_rain_strength > 0.2f && biome_rain && sky);
	}
	struct server_local_host_weather_diagnostic diag;
	if(!gstate.remote_session)
		server_local_host_weather_diagnostic_request();
	bool server_truth = !gstate.remote_session
		&& server_local_host_weather_diagnostic_read(&diag);
	if(server_truth) {
		biome_rain = diag.biome_rains;
		sky = diag.sky;
		fire_ticks = diag.fire_ticks;
		wet = diag.wet;
		snprintf(str, sizeof(str),
			"spawner: %dus %dn  wx r%.2f b%d s%d f%d w%d",
			spwn_us, spwn_n, (double)gstate.weather_rain_strength,
			biome_rain, sky, fire_ticks, wet);
	} else {
		snprintf(str, sizeof(str),
			"spawner: %dus %dn  wx r%.2f b%d s%d f%d w%dc",
			spwn_us, spwn_n, (double)gstate.weather_rain_strength,
			biome_rain, sky, fire_ticks, wet);
	}
	if(4 + gutil_font_width(str, GFX_GUI_SCALE * 8) > width - 4) {
		if(server_truth)
			snprintf(str, sizeof(str), "wx r%.2f b%d s%d f%d w%d",
				(double)gstate.weather_rain_strength, biome_rain, sky,
				fire_ticks, wet);
		else
			snprintf(str, sizeof(str), "wx r%.2f b%d s%d f%d w%dc",
				(double)gstate.weather_rain_strength, biome_rain, sky,
				fire_ticks, wet);
	}
	while(str[0] && 4 + gutil_font_width(str, GFX_GUI_SCALE * 8) > width - 4)
		str[strlen(str) - 1] = '\0';
	gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 11, str, GFX_GUI_SCALE * 8, true);

	// Row 12: M1 far-ring LOD instrument. Near radius (the reach dial) plus the
	// cumulative average vertices/chunk per tier -- the surface skin (far) vs the
	// full mesh (near). The ratio is the GPU-vertex win at distance.
	{
		uint64_t lv[2];
		uint32_t lc[2];
		chunk_mesher_lod_stats(lv, lc);
		unsigned full_avg = lc[0] ? (unsigned)(lv[0] / lc[0]) : 0;
		unsigned skin_avg = lc[1] ? (unsigned)(lv[1] / lc[1]) : 0;
		snprintf(str, sizeof(str), "lod near%d: full %uv/c(%uc) skin %uv/c(%uc)",
				world_lod_near_radius(), full_avg, (unsigned)lc[0], skin_avg,
				(unsigned)lc[1]);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 12, str, GFX_GUI_SCALE * 8,
				   true);
	}

	// P0 perf instruments. Row 13: the SERVER tick's true wall-clock (distinct from
	// the 20Hz client "tck" on the cpu-ms line) -- last + session max; on the single
	// core this is stolen straight from the render budget, the headline tick number.
	{
		float srv_last = 0.0f, srv_max = 0.0f;
		server_local_tick_stats(&srv_last, &srv_max);
		snprintf(str, sizeof(str), "srv tick: %.1fms (max %.1f)", srv_last,
				 srv_max);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 13, str, GFX_GUI_SCALE * 8,
				   true);
	}

	// Rows 14-15: the per-edit block path. Row 14 = us in (and count of)
	// server_world_set_block this tick + the session-peak us -- its lighting flood
	// + 6-neighbour notify, the #1 unbudgeted spike under explosions/fluids (the
	// peak catches a one-tick explosion a periodic screenshot would miss). Row 15 =
	// get_block dict fetches this tick + peak -- the random-tick floor + per-mob
	// collision sweep, the everyday dict-hash storm.
	{
		int sb_us = 0, sb_n = 0, sb_pk = 0, gb_n = 0, gb_pk = 0;
		server_world_edit_stats(&sb_us, &sb_n, &sb_pk, &gb_n, &gb_pk);
		snprintf(str, sizeof(str), "setblk: %dus %dn (pk %dus)", sb_us, sb_n,
				 sb_pk);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 14, str, GFX_GUI_SCALE * 8,
				   true);
		// cache% = Tier-0 chunk-resolution cache hit rate -- the dict-hash
		// storm killer; should sit high (90s) during sweeps/floods.
		snprintf(str, sizeof(str), "getblk: %d/tk (pk %d) cache%d%%", gb_n,
				 gb_pk,
				server_world_chunkres_hit_pct());
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 15, str, GFX_GUI_SCALE * 8,
				   true);
	}

	// Row 16: M1 deep reach far-ring stream. total = cumulative far chunks streamed
	// (climbs as the far ring fills -> the server stream is working); live = current
	// far_sent count (the bounded far ring; should sit near the annulus size, not
	// grow without bound -> the far evictor is keeping it bounded). rpcdrop = client->
	// server messages dropped on a full inbox (svin_rpc_send under extreme chunk-stream
	// backpressure) -- surfaced here so the drop counter is never silent; anything
	// persistently non-zero in steady play flags a real problem.
	{
		unsigned far_total = 0;
		int far_live = 0;
		server_local_far_stats(&far_total, &far_live);
		// cnry = mem2 red-zone canary violations (a stray writer smashed a
		// block's trailing guard; mem2_canary_first names the first offender
		// in the watchdog hang report). MUST stay 0; non-zero = the heap
		// corruption class is ACTIVE this session.
		// soom = stack grow-pushes refused on realloc OOM (the far-ring lighting
		// flood's old NULL-data DSI crash, now a counted bounded relight gap).
		// lfc = relight floods aborted at the work ceiling. BOTH must stay 0 once the
		// synthetic far sections store self-consistent light (no seam cascade).
		// bail = the far-slot pool's CURRENT run of consecutive batch bails. The
		// non-blocking acquire that unwedged the server thread made an exhausted pool
		// silent, and its only alarm is an fprintf(stderr) the shipping -DNDEBUG .dol
		// compiles away -- so this row is where a leak becomes visible on hardware. A few
		// bails that come back to 0 are ordinary backpressure at a high far_view_distance;
		// a run that keeps climbing while "sent" stops moving is a far slot the client
		// consumed and never released, i.e. far streaming has stopped for good.
		snprintf(str, sizeof(str),
				 "far: %u sent, %d live, cnry %u, soom %u, lfc %u, bail %u",
				far_total, far_live, (unsigned)mem2_canary_hits(),
				(unsigned)stack_oom_drops(),
				(unsigned)lighting_flood_ceiling_hits(),
				(unsigned)server_local_far_slot_starve());
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 16, str, GFX_GUI_SCALE * 8,
				   true);
	}

	// Row 23: FH-13 A* pathfinding cost instrument. srch/tk = mob path searches
	// resolved last tick (chase acquire/re-acquire + wander rolls, combined --
	// this is the number the hardware round watches for a stampede of concurrent
	// chasers going pathological). nodes = the LAST search's opened-node count this
	// reset window (0 if none ran that tick); peak = the session-high single-search
	// node count, sized against PATHFINDER_POOL_CAPACITY (1024) -- a peak that keeps
	// climbing toward the cap flags a search that is chewing through open terrain,
	// the T2-deviation trigger this lane's brief asks the hardware round to watch.
	{
		int32_t searches = 0, nodes_last = 0, nodes_max = 0;
		pathfinding_perf_counters(&searches, &nodes_last, &nodes_max);
		snprintf(str, sizeof(str), "path: %d srch/tk, %d nodes (peak %d)",
				 searches, nodes_last, nodes_max);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 23, str, GFX_GUI_SCALE * 8,
				   true);
	}

	// S3 background-music + jukebox-record dials (the "snd: arena ..." row 20
	// precedent). tracks = the flat catalog audio_music_tick picks from
	// (0 with no imported music/ pack -- expected, not a bug); key = which
	// of the 4 Beta pool keys is currently sounding (audio_music_table.h),
	// "record" while a jukebox disc is playing instead (audio_music_
	// is_playing_record -- current_key has no table entry for a record, so
	// leaving this at the raw audio_music_current_key() read would have
	// misreported an actively-sounding record as silent), "-" when neither;
	// underruns = audio_music_ring.h's decode-fell-behind counter,
	// s_arena_rejects's precedent -- should read 0 on real hardware, a live
	// count is a genuine report, never hidden.
	//
	// Field report (2026-08-31): this row's old left-column slot (row 24,
	// stacked under rows 0-23 like every other F3 line) sat under the HUD
	// hearts on the Wii's 480-line screen and was unreadable during a
	// capture. Relocated to the top-right corner (right-aligned, y=4) --
	// unused by every other overlay/HUD element in this file (the only
	// other "width - ..." placements here are horizontally CENTERED
	// hotbar/inventory/sleep-fade rows anchored near the bottom of the
	// screen, not this y=4 top strip).
	{
		const char* key = audio_music_current_key();
		const char* what
			= audio_music_is_playing_record() ? "record" : (key ? key : "-");
		snprintf(str, sizeof(str), "music: tracks %d key %s underruns %u",
				audio_music_track_count(), what,
				audio_music_underrun_count());
		int tw = gutil_font_width(str, GFX_GUI_SCALE * 8);
		gutil_text(width - tw - 4, 4, str, GFX_GUI_SCALE * 8, true);
	}

	// Nearest hostile mob: horizontal distance + east-offset (dx) to the player
	// eye. Lets a chase/speed play-test read the EXACT closing distance per frame
	// off the overlay (the right instrument for tuning MOB_MAX_SPEED_WII) instead
	// of guessing from on-screen scale. Only printed when a monster is loaded.
	{
		float best = -1.0f, best_dx = 0.0f;
		dict_entity_it_t it;
		dict_entity_it(it, world_view_entities());
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_MONSTER) {
				float dx = e->pos[0] - owner->camera.x;
				float dz = e->pos[2] - owner->camera.z;
				float d = sqrtf(dx * dx + dz * dz);
				if(best < 0.0f || d < best) { best = d; best_dx = dx; }
			}
			dict_entity_next(it);
		}
		if(best >= 0.0f) {
			// Row 17: row 12 is the LOD line above -- printing here at 12 overwrote
			// it whenever a monster was loaded.
			snprintf(str, sizeof(str), "mob: nearest %.2f (dx %.2f) blocks",
					 best, best_dx);
			gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 17, str,
					   GFX_GUI_SCALE * 8, true);
		}
	}

	if (owner->camera_hit.entity_hit) {
		struct entity **ptr = dict_entity_get(
		    world_view_entities(),
		    owner->camera_hit.entity_id
		);
		if (ptr && *ptr) {
		    struct entity *e = *ptr;
		    const char *ename = e->name ? e->name : "unknown";
		    snprintf(str, sizeof(str), "(%i, %i, %i), %s (%u)",
		            owner->camera_hit.x,
		            owner->camera_hit.y,
		            owner->camera_hit.z,
		            ename, e->id);
		} else {
		    snprintf(str, sizeof(str), "(%i, %i, %i)",
		            owner->camera_hit.x,
		            owner->camera_hit.y,
		            owner->camera_hit.z);
		}
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 9, str, GFX_GUI_SCALE * 8, true);
	} else if(gstate.debug_overlay && compact_debug && !recovering) {
		snprintf(str, sizeof(str), "P%u  %s  %.0f FPS",
			(unsigned)(owner == &gstate.players[1] ? 2 : 1), CW_BUILD_ID,
			(double)gstate.stats.fps);
		gutil_text(8, 8, str, 12, true);
	} else if(!recovering && !compact_debug && owner->camera_hit.hit) {
		struct block_data bd
			= world_get_block(world_view_current(), owner->camera_hit.x,
							  owner->camera_hit.y, owner->camera_hit.z);
		struct block* b = blocks[bd.type];
		snprintf(str, sizeof(str), "side: %s, (%i, %i, %i), %s, (%i:%i)",
				block_side_name(owner->camera_hit.side), owner->camera_hit.x,
				owner->camera_hit.y, owner->camera_hit.z,
				b ? b->name : "unknown",
				bd.type, bd.metadata);
		gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * 9, str, GFX_GUI_SCALE * 8, true);
	}
	}

	/* The CavEX bottom button-hint row, restored ALWAYS ON (maintainer
	 * ruling 2026-10-29, reverting b70f38003's removal): the same content and
	 * order, drawn by the same gutil_control_icon_owned, but from THIS pane's
	 * own prompt frame (`input` = input_prompt_frame of this owner), so each
	 * split pane shows its own device's existing glyph cells. */
	if(!card_up) {
		struct screen_ingame_hint_ctx hc = {
			.target = SCREEN_INGAME_HINT_NONE,
			.held = screen_ingame_held_kind(owner)};
		if(owner->camera_hit.entity_hit) {
			struct entity** ptr = dict_entity_get(world_view_entities(),
												  owner->camera_hit.entity_id);
			hc.target = SCREEN_INGAME_HINT_ENTITY;
			if(ptr && *ptr) {
				hc.entity_left = (*ptr)->leftClickText;
				hc.entity_right = (*ptr)->rightClickText;
			}
		} else if(owner->camera_hit.hit) {
			struct block_data bd = world_get_block(world_view_current(),
				owner->camera_hit.x, owner->camera_hit.y, owner->camera_hit.z);
			hc.target = SCREEN_INGAME_HINT_BLOCK;
			hc.block_usable = blocks[bd.type] && blocks[bd.type]->onRightClick;
		}
		struct screen_ingame_hint hints[SCREEN_INGAME_HINT_MAX];
		int hint_count = screen_ingame_hint_row(&hc, hints);
		struct gutil_hint_row row;
		if(screen_ingame_gameplay_hint_layout(input, width, height,
			gstate.remote_session && screen_ingame_chat_socket_owner(owner_slot),
			hints, hint_count, &row))
			gutil_hint_row_draw(&row);
	}

	// draw hotbar -- from the real Beta gui/gui.png (texture_gui_widgets), not the
	// fCavEX gui_2 atlas, so a genuine Beta pack shows its authentic hotbar.
	/* Approved rows (menus.html:210/399/400, Astra ui1): hotbar top at
	 * height-62 (an 18 px bottom margin under the 44 px bar -- the old
	 * (16*scale)*8/5 = 51 px reservation belonged to the removed prompt
	 * strip), items 6 px below the bar top, the selection frame 2 px above
	 * it, the health/armor row 22 px above it (4 px clear of the bar). */
	int bar_y = screen_ingame_hotbar_top(height, GFX_GUI_SCALE);
	int hud_left = (width - 182 * GFX_GUI_SCALE) / 2;
	if(!card_up) {
	gfx_bind_texture(&texture_gui_widgets);
	gutil_texquad((width - 182 * GFX_GUI_SCALE) / 2, bar_y, 0, 0,
				  182, 22, 182 * GFX_GUI_SCALE, 22 * GFX_GUI_SCALE);

	/* H-S1 SNEAK RAIL (build spec 15 A.2/A.3/A.4). INSIDE the hotbar pass, on
	 * purpose and by ruling: after the bar BODY above and before the item
	 * icons, the counts and the selected-slot frame below, so the selection
	 * frame paints OVER the rail at slots 1 and 9 -- the rail is never
	 * shortened around it -- and no later hotbar draw can erase it.
	 *
	 * The GLOBAL screens that must hide it -- the pause SCREEN, inventory,
	 * crafting, every menu and the composer -- hide it by construction: they
	 * own the frame instead of this function, exactly as they already do for
	 * the hotbar and the hearts. The modals that are drawn OVER this function
	 * instead (both recovery machines and the owner-LOCAL pause) do NOT, and
	 * that is what `hud_modal` is for -- review 28 M1 found this comment
	 * asserting the local pause away when main.c paints it at alpha 180 right
	 * on top of the rail.
	 *
	 * The config and the capture lever are read ONCE: neither key is
	 * rewritten while the game runs (input_init reads controls.* the same
	 * way), and a parson dot-path lookup per frame per viewport is not
	 * something to spend on the Wii. The "not read yet" answer is a SEPARATE
	 * flag, never a value of the keys' own domain (review 28 M2 -- see the
	 * socket lever's cache above for the defect that pattern caused there). */
	{
		static int cfg_on, cfg_capture;
		static bool cfg_sneak_loaded;
		if(!cfg_sneak_loaded) {
			cfg_on = screen_ingame_sneak_marker_config(&gstate.config_user);
			cfg_capture = config_read_int(&gstate.config_user,
										  "debug.sneak_indicator_capture", 0)
				== 1;
			cfg_sneak_loaded = true;
		}
		/* Ruling 7 V5, unchanged by A.4: THIS player's transient bed toast
		 * suppresses the indicator while it is up, and it returns by itself
		 * when the toast ends if the toggle is still latched. The toast is
		 * not touched: the same two facts its own draw reads -- its text and
		 * its deadline -- are read here, so the two can never disagree about
		 * one frame, and the short-circuit means an ordinary frame with no
		 * toast pays nothing. (The toast block above has already cleared an
		 * EXPIRED message, which answers the same way.) */
		bool toast_visible = owner->hud_message[0]
			&& !time_reached(time_get(), owner->hud_message_until);
		/* Rail and label appear and disappear TOGETHER: one gate, one draw. */
		if(screen_ingame_sneak_marker_visible(
			   input_sneak_toggle_latched(owner_slot), cfg_on != 0,
			   cfg_capture != 0, toast_visible, hud_modal))
			sneak_rail_draw(width, height, GFX_GUI_SCALE);
	}

	// Crosshair from the real Beta gui/icons.png at (0,0) (16x16), so a Beta pack
	// shows its authentic crosshair; the bundled fCavEX icons.png carries a plain
	// '+' there. Drawn with MODE_INVERT so it reads on any background.
	gfx_bind_texture(&texture_icons);
	if(attack_assist_crosshair_tinted(&owner->camera_hit)) {
		/* Soft-snap melee assist feedback (committee ruling 2026-10-05):
		 * the snap picked the target, so the crosshair turns a solid warm
		 * red instead of inverting -- visible on any background, and only
		 * while an attack would land on the assisted mob. */
		gfx_blending(MODE_BLEND);
		gutil_texquad_col((width - 16 * GFX_GUI_SCALE) / 2,
						  (height - 16 * GFX_GUI_SCALE) / 2, 0, 0, 16, 16,
						  16 * GFX_GUI_SCALE, 16 * GFX_GUI_SCALE,
						  ATTACK_ASSIST_TINT_R, ATTACK_ASSIST_TINT_G,
						  ATTACK_ASSIST_TINT_B, 0xFF);
	} else {
		gfx_blending(MODE_INVERT);
		gutil_texquad((width - 16 * GFX_GUI_SCALE) / 2, (height - 16 * GFX_GUI_SCALE) / 2, 0, 0, 16, 16,
					  16 * GFX_GUI_SCALE, 16 * GFX_GUI_SCALE);
	}

	gfx_blending(MODE_OFF);

	for(int k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		struct item_data item;
		if(inventory_get_slot(
			   windowc_get_latest(owner->windows[WINDOWC_INVENTORY]),
			   k + INVENTORY_SLOT_HOTBAR, &item))
			gutil_draw_item(&item, (width - 182 * GFX_GUI_SCALE) / 2 + 3 * GFX_GUI_SCALE + 20 * GFX_GUI_SCALE * k,
							bar_y + 3 * GFX_GUI_SCALE, 0);
	}

	gfx_blending(MODE_BLEND);
	gfx_bind_texture(&texture_gui_widgets);

	// draw hotbar selection (Beta gui.png: selected-slot highlight is at 0,22)
	gutil_texquad((width - 182 * GFX_GUI_SCALE) / 2 - 2
					  + 20 * GFX_GUI_SCALE
						  * inventory_get_hotbar(windowc_get_latest(
							  owner->windows[WINDOWC_INVENTORY])),
				  bar_y - GFX_GUI_SCALE, 0, 22, 24, 24, 24 * GFX_GUI_SCALE, 24 * GFX_GUI_SCALE);

	// Health + armor + air HUD, drawn from the real Beta gui/icons.png --
	// see hud_survival_row.c for the UV/state commentary. SURVIVAL ONLY
	// (maintainer ruling 2026-09-03, docs/creative_mode.md "Creative HUD --
	// no hearts/armor/air"): a creative world's mode is known client-side via
	// gstate.config_creative, which is armed per-world at SRPC_LOAD_WORLD
	// time from the world's own LEVEL_GAME_MODE tag (screen_select_world.c),
	// so this follows the WORLD's mode, not a stale session-global flag --
	// the retired phase-1 `gameplay.creative` config no longer exists. The
	// hotbar/crosshair/held-item above are NOT gated: only the health/armor/
	// air rows disappear in creative.
	int heart_row_y = bar_y - 10 * GFX_GUI_SCALE; /* CavEX: 365 of 480 */
	if(!gstate.config_creative)
		hud_survival_row_draw_owned(owner, hud_left, heart_row_y);
	} /* !card_up */
	gutil_connection_notices(owner == &gstate.players[1] ? 1 : 0,
							 gstate.players[1].local_player != NULL, 16, 8,
							 width - 32, height - 8); /* card at the one anchor, 24,16 (maintainer design A, 10-31) */
	if(owner == &gstate.players[0])
		screen_controller_recovery_render(width, height);
	/* Approved owner-local toast/hint. Menus and death freeze presentation;
	 * the global update still observes binding changes on hidden frames. */
	struct menu_rect occupied[6] = {
		{hud_left - 2, bar_y - 24, 368, 70},
		{(width - 48) / 2, (height - 48) / 2, 48, 48},
		{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
	/* A5 (10-31): coordinates + FPS under the notice card area, every pane,
	 * hidden under F3 (it shows both) and whenever the HUD is not this
	 * pane's (menus, modals, death, loading). Config read once, like the
	 * sneak lever above. */
	{
		static bool cfg_coords_loaded, cfg_coords, cfg_fps;
		if(!cfg_coords_loaded) {
			cfg_coords = screen_ingame_ui_flag(&gstate.config_user,
											   "ui.show_coordinates");
			cfg_fps = screen_ingame_ui_flag(&gstate.config_user, "ui.show_fps");
			cfg_coords_loaded = true;
		}
		bool hud_shown = !gstate.debug_overlay && !hud_modal
			&& !owner->death.active && !owner->local_respawn_loading
			&& owner->current_screen == &screen_ingame && gstate.world_loaded
			&& owner->local_player;
		if(hud_shown && (cfg_coords || cfg_fps)) {
			char coords[48], fps[24];
			int bp[3];
			screen_ingame_block_pos(owner->local_player->pos, bp);
			snprintf(coords, sizeof coords, "X: %d  Y: %d  Z: %d", bp[0], bp[1],
					 bp[2]);
			snprintf(fps, sizeof fps, "%.0f FPS", (double)gstate.stats.fps);
			struct gutil_button_bounds lines[2];
			int n = screen_ingame_coords_layout(cfg_coords,
				gutil_font_width(coords, 16), cfg_fps, gutil_font_width(fps, 16),
				lines);
			for(int k = 0; k < n; k++)
				gutil_text(lines[k].x, lines[k].y,
						   cfg_coords && k == 0 ? coords : fps, 16, true);
			if(n)
				occupied[5] = (struct menu_rect) {lines[0].x, lines[0].y,
					lines[0].width > lines[n - 1].width ? lines[0].width
						: lines[n - 1].width,
					lines[n - 1].y + lines[n - 1].height - lines[0].y};
		}
	}
	int chat_lines = chat_hud_visible_lines(&gstate.chat_ring, false, 10);
	if(chat_lines)
		occupied[2] = (struct menu_rect) {2 * GFX_GUI_SCALE,
			chat_base - (chat_lines - 1) * 9 * GFX_GUI_SCALE - GFX_GUI_SCALE,
			320 * GFX_GUI_SCALE, (chat_lines - 1) * 9 * GFX_GUI_SCALE
				+ 10 * GFX_GUI_SCALE};
	if(gstate.debug_overlay && compact_debug) {
		snprintf(str, sizeof str, "P%u  %s  %.0f FPS", owner_slot + 1,
			CW_BUILD_ID, (double)gstate.stats.fps);
		occupied[3] = (struct menu_rect) {8, 8,
			gutil_font_width(str, 12) + 2, 14};
	}
	int controller_notice_width = gutil_connection_notice_width(owner_slot,
		gstate.players[1].local_player != NULL, false);
	if(controller_notice_width > 0)
		/* card A (10-31): the card at the (24,16) anchor, 32 tall, plus
		 * its 2 px dark edge on every side */
		occupied[4] = (struct menu_rect) {GUTIL_NOTICE_ANCHOR_X - 2,
			GUTIL_NOTICE_ANCHOR_Y - 2,
			(controller_notice_width < width - 48 ? controller_notice_width
				: width - 48) + 4, GUTIL_NOTICE_CARD_H + 4};
	achievement_notice_render(owner_slot, input, 0, 0, width, height,
		hud_modal || owner->death.active || owner->local_respawn_loading
			|| owner->current_screen != &screen_ingame || !gstate.world_loaded
			|| (gstate.debug_overlay && !compact_debug), occupied, 6, NULL);
}

/* Controller UI redesign A2 (maintainer picks 10-31, placement 1): a menu
 * keeps the hearts and the hotbar EXACTLY where gameplay draws them -- the
 * same screen_ingame_hotbar_top anchor, the same gui.png body/selection UVs,
 * items 3 units in, hearts 10 units above the bar (screen_ingame.c's own
 * expressions, which test_hud_source_pin pins) -- so opening a menu never
 * moves them. Hearts follow the same single survival gate as gameplay. The
 * sneak rail is gameplay-only and is not drawn here. */
void hud_menu_draw_owned(const struct client_session* owner, int width,
	int height) {
	if(!owner || !owner->local_player || !owner->windows[WINDOWC_INVENTORY])
		return;
	int bar_y = screen_ingame_hotbar_top(height, GFX_GUI_SCALE);
	int hud_left = (width - 182 * GFX_GUI_SCALE) / 2;
	struct inventory* inv = windowc_get_latest(owner->windows[WINDOWC_INVENTORY]);
	gfx_blending(MODE_BLEND);
	gfx_bind_texture(&texture_gui_widgets);
	gutil_texquad(hud_left, bar_y, 0, 0, 182, 22, 182 * GFX_GUI_SCALE,
				  22 * GFX_GUI_SCALE);
	gfx_blending(MODE_OFF);
	for(int k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		struct item_data item;
		if(inventory_get_slot(inv, k + INVENTORY_SLOT_HOTBAR, &item))
			gutil_draw_item(&item, hud_left + 3 * GFX_GUI_SCALE
								+ 20 * GFX_GUI_SCALE * k,
							bar_y + 3 * GFX_GUI_SCALE, 0);
	}
	gfx_blending(MODE_BLEND);
	gfx_bind_texture(&texture_gui_widgets);
	gutil_texquad(hud_left - 2 + 20 * GFX_GUI_SCALE * inventory_get_hotbar(inv),
				  bar_y - GFX_GUI_SCALE, 0, 22, 24, 24, 24 * GFX_GUI_SCALE,
				  24 * GFX_GUI_SCALE);
	/* SOL-REVIEW item 4 (10-31): no oxygen bubbles while a menu is up. */
	if(!gstate.config_creative)
		hud_survival_row_draw_owned_ex(owner, hud_left,
									   bar_y - 10 * GFX_GUI_SCALE, false);
}

struct screen screen_ingame = {
	.reset = screen_ingame_reset,
	.update = screen_ingame_update,
	.render2D = screen_ingame_render2D,
	.render3D = screen_ingame_render3D,
	.render_world = true,
};
