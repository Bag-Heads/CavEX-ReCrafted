/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

/*
	MP S3: a remote (other) player's client-side render entity (plan section
	6.3 -- "a remote-player render entity is NOT players[N]"). Constructed
	only by client_interface.c's CRPC_SPAWN_PLAYER consumer; never appears on
	server_local (singleplayer has no remote players) and never pluralizes
	server simulation state.

	Client-only, exactly like the egg/snowball/fish precedent: no
	tick_server. Position/look flow through the ordinary
	CRPC_ENTITY_MOVE/CRPC_ENTITY_LOOK/CRPC_ENTITY_DESTROY boundary (MOVE and
	DESTROY already apply generically to any dict entity, proven by
	entity_fish.c/entity_egg.c's own NULL-rendered, network-tracked shape;
	LOOK is new -- see client_interface.h), so this entity is real,
	positioned, oriented, and destroyable end-to-end.

	Render-follow-up (2026-08-19): unlike a monster, this entity's HEAD
	yaw/pitch is REAL networked look (e->orient, degrees -- set once at spawn
	by CRPC_SPAWN_PLAYER and live-updated by CRPC_ENTITY_LOOK), not a
	movement-direction approximation. Only the BODY yaw is derived
	client-side, lagging the head via entity_blend_body_to_head exactly like
	every monster's client_tick_monster (entity_monster.c) -- the same
	function, the same smoothing factor, applied to this union member instead
	of data.monster. entity_tick_animation itself asserts ENTITY_MONSTER and
	cannot be reused directly (it indexes e->data.monster), so the walk-frame
	advance below is that function's exact logic, inlined against
	data.remote_player.
*/

#include <assert.h>
#include <math.h>
#include <string.h>

#include "entity.h"
#include "combat.h" // hurt_wash_for_brightness, mob_death_roll_deg (shared mob/player formulas)
#include "../light_brightness.h" // light_brightness_current (R1b hurt/death wash)
#include "entity_monster.h" // shared onLeftClick attack handler
#include "../graphics/render_entity.h"
#include "../graphics/render_entity_anchor.h" // render_entity_death_tilt (G8)
#include "../game/client_session.h"
#include "../game/game_state.h"
#include "../particle.h" // burning cue (item 3): same particle_generate_torch/_smoke pair entity_monster.c uses
#include "../platform/gfx.h" // gfx_entity_tint (item 1: hurt red flash)

// Mirrors entity_monster.c's own YAW_SMOOTH_FACTOR (0.2f, "smoothing factor
// [0..1]") -- not shared via a header, so restated here rather than reaching
// into another translation unit's file-local macro.
#define REMOTE_PLAYER_YAW_SMOOTH_FACTOR 0.2f
// render_entity_biped's walkAngle formula is `frame % 60`; entity_monster.c's
// own entity_tick_animation call for every biped/quadruped mob passes the
// same 60. Kept in lockstep here rather than re-deriving it.
#define REMOTE_PLAYER_WALK_MAX_FRAME 60

static size_t remote_player_get_bounding_box(const struct entity* e,
											 struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

// Client tick: position/rotation now step through the Beta increments
// approach (entity_default_client_tick -> entity_network_approach_tick;
// server_remote arms 3 ticks per move, so the old hard snap and its
// gap-tick walk stutter -- mp_s1_hostile_review finding 4 -- are gone:
// approach ticks keep moving the model between packets), plus the
// animation/orientation bookkeeping the renderer needs -- body-yaw lag
// toward the networked head look, and a walk-frame advance driven by the
// horizontal distance this tick moved (the entity_tick_animation
// precedent, mirrored rather than called since that helper is asserted
// ENTITY_MONSTER-only).
static bool entity_remote_player_client_tick(struct entity* e) {
	assert(e);

	entity_default_client_tick(e); // pos_old=pos; approach pos+look; orient_old kept

	/* MP-RIDE: Entity.updateRidden (Entity.java:875-886) replaces the rider's
	 * own position with ridingEntity.updateRiderPosition() every tick, and a
	 * dead (here: no longer tracked) vehicle clears ridingEntity. */
	if(e->data.remote_player.riding_vehicle) {
		struct entity** vp = dict_entity_get(
			*(dict_entity_t*)world_view_entities(),
			e->data.remote_player.riding_vehicle_id);
		vec3 seat;
		if(vp && *vp && entity_remote_player_rider_pos(*vp, seat)) {
			glm_vec3_copy(seat, e->pos);
			glm_vec3_copy(seat, e->network_pos);
			e->network_increments = 0;
		} else if(!vp || !*vp) {
			e->data.remote_player.riding_vehicle = false;
			e->data.remote_player.riding_vehicle_id = 0;
		}
	}

	vec3 delta;
	entity_get_delta(e, delta); // pos - pos_old: this tick's movement
	float speed = sqrtf(delta[0] * delta[0] + delta[2] * delta[2]);

	// Animation-parity pass (2026-09-03) item 2: the SAME dying-freeze guard
	// entity_monster.c's client tick applies (HARDEN AUDIT #9) -- a corpse
	// mid death-roll must not gait its legs. Forcing speed to 0 here lets
	// limb_swing lerp to rest and holds the walk frame; the position lerp
	// above (entity_default_client_tick) is untouched, so the corpse still
	// slides/settles to its networked pose. [740c583 EntityLiving.
	// onEntityUpdate: no leg animation drives while deathTime > 0]
	if(e->data.remote_player.death_time > 0)
		speed = 0.0f;

	// Walk-anim amplitude: Beta's own remote-player leg math -- amplitude
	// target is the horizontal distance x4 capped at 1 (speed-proportional,
	// not a step function), blended at 0.4F. [740c583
	// EntityOtherPlayerMP.onUpdate] (legYaw += (min(1, dist*4) - legYaw)*0.4F)
	float swing_target = speed * 4.0f;
	if(swing_target > 1.0f)
		swing_target = 1.0f;
	e->data.remote_player.limb_swing
		+= (swing_target - e->data.remote_player.limb_swing) * 0.4f;

	// Walk-frame advance -- entity_tick_animation's exact logic, inlined
	// against data.remote_player.{frame,frame_time_left} since that shared
	// helper's assert(e->type == ENTITY_MONSTER) forbids calling it here.
	if(speed > 0.001f) {
		e->data.remote_player.frame_time_left--;
		if(e->data.remote_player.frame_time_left <= 0) {
			e->data.remote_player.frame
				= (e->data.remote_player.frame + 1) % REMOTE_PLAYER_WALK_MAX_FRAME;
			e->data.remote_player.frame_time_left = 1;
		}
	}

	// Body yaw lags the REAL networked head look (e->orient[0], ENGINE-frame
	// degrees -- Beta wire angles are mirrored/offset at server_remote.c's
	// CRPC assembly boundary, see the CRPC_SPAWN_PLAYER note there),
	// converted to radians once at
	// this boundary for entity_blend_body_to_head's wraparound math (which
	// operates in radians, matching every monster's body_yaw/head_yaw pair).
	entity_blend_body_to_head(&e->data.remote_player.body_yaw,
							  glm_rad(e->orient[0]),
							  REMOTE_PLAYER_YAW_SMOOTH_FACTOR);

	// MP visual-parity pass item 3: EntityPlayer.updatePlayerActionState's
	// exact swingProgressInt/isSwinging/swingProgress state machine
	// [740c583]. CRPC_ENTITY_ANIMATION's animate==1 consumer
	// (client_interface.c) is this entity's swingItem() equivalent
	// (swing_progress_int=-1, is_swinging=true); every tick thereafter
	// advances the 8-tick cycle here, exactly like Beta's own
	// `if(isSwinging) { ++swingProgressInt; if(swingProgressInt>=8) {
	// swingProgressInt=0; isSwinging=false; } } else swingProgressInt=0;
	// swingProgress = swingProgressInt/8.0F;` -- called every tick from
	// EntityOtherPlayerMP.onLivingUpdate's `super.updatePlayerActionState();`
	// regardless of whether a swing is in progress, so this runs
	// unconditionally too.
	e->data.remote_player.prev_swing_progress
		= e->data.remote_player.swing_progress;
	if(e->data.remote_player.is_swinging) {
		e->data.remote_player.swing_progress_int++;
		if(e->data.remote_player.swing_progress_int >= 8) {
			e->data.remote_player.swing_progress_int = 0;
			e->data.remote_player.is_swinging = false;
		}
	} else {
		e->data.remote_player.swing_progress_int = 0;
	}
	e->data.remote_player.swing_progress
		= (float) e->data.remote_player.swing_progress_int / 8.0f;

	return false;
}

// Render: shared ModelBiped geometry (render_entity_player, render_entity.c)
// textured with mob/char.png. Structure mirrors entity_monster.c's
// entity_render (position lerp, block-light sample, limb-swing amplitude,
// model matrix, per-mob render call). Animation-parity pass (2026-09-03)
// items 1/2/5 close the "minus the monster-only death-roll/tint passes this
// entity has no equivalent state for" gap the comment above used to note --
// hurt tint and the death roll are now shared with entity_monster.c's own
// formulas (combat.h), and armor is a new per-slot layer render_entity_player
// forwards to render_entity_biped.
static void entity_remote_player_render(struct entity* e, mat4 view, float td) {
	assert(e);

	vec3 p;
	glm_vec3_lerp(e->pos_old, e->pos, td, p);

	/* Beta Entity.getEntityBrightness samples logical posY, before the
	 * RenderPlayer-only 0.125 sneaking translation (RenderPlayer.java:43). */
	const int light_y = underwater_overlay_sample_y(
		e->pos[1], e->height, e->y_offset);

	// MP visual-parity pass item 2: RenderPlayer.renderPlayer's whole-body
	// Y offset for a sneaking OTHER player [740c583]: `var11 -= 0.125D`
	// (gated `isSneaking() && !(instanceof EntityPlayerSP)` -- the
	// EntityPlayerSP exclusion is Beta's own local-player special case,
	// which has no equivalent here since ENTITY_REMOTE_PLAYER is never the
	// local player by construction).
	if(e->data.remote_player.sneaking)
		p[1] -= 0.125F;

	/* MP-BED: RenderPlayer.renderPlayer draws at posY - yOffset
	 * (RenderPlayer.java:43), and sleepInBedAt set yOffset 0.2
	 * (EntityPlayer.java:556); func_22016_b then shifts a sleeping, living
	 * player by field_22063_x / field_22062_y / field_22061_z
	 * (RenderPlayer.java:229-236). field_22062_y is 0 after the first
	 * EntityOtherPlayerMP.onUpdate (EntityOtherPlayerMP.java:44). */
	const bool lying = e->data.remote_player.sleeping
		&& e->data.remote_player.death_time == 0;
	if(lying) {
		p[0] += e->data.remote_player.sleep_off_x;
		p[1] -= 0.2F;
		p[2] += e->data.remote_player.sleep_off_z;
	}

	struct block_data blk;
	entity_get_block(e, floorf(e->pos[0]), light_y,
		floorf(e->pos[2]), &blk);
	const uint8_t model_light = (uint8_t)((blk.torch_light << 4) | blk.sky_light);
	render_entity_update_light(model_light);
	render_entity_set_limb_swing(e->data.remote_player.limb_swing);

	mat4 model, mv;
	glm_translate_make(model, p);

	// Animation-parity pass item 2 / next-gaps-0924/G8: Beta death tilt,
	// driven by the NETWORKED deathTime. RenderPlayer.rotateCorpse's awake
	// branch is super.rotateCorpse (RenderPlayer.java:238-247), so this is
	// the RenderLiving curve with getDeathMaxRotation 90, rolled about the
	// BODY's forward axis through the feet -- the same shared composition
	// entity_monster.c's dying mobs use (combat.h mob_death_roll_deg +
	// render_entity_anchor.h render_entity_death_tilt).
	/* MP-BED: RenderPlayer.func_22017_a's sleeping branch
	 * (RenderPlayer.java:238-242) replaces rotateCorpse's
	 * glRotatef(180 - renderYawOffset, 0,1,0) with
	 * glRotatef(bedOrientation, 0,1,0); glRotatef(getDeathMaxRotation = 90,
	 * 0,0,1); glRotatef(270, 0,1,0). The first two go on the model here; the
	 * body pass's own R_y(bodyYaw + 180) supplies the 270 with bodyYaw 90
	 * (render_entity_anchor.h render_entity_body_mv, the same frame the
	 * death roll below uses). */
	float lying_yaw_deg = 0.0F;
	if(lying) {
		glm_rotate_y(model, glm_rad(e->data.remote_player.bed_orient_deg), model);
		glm_rotate_z(model, glm_rad(90.0F), model);
		lying_yaw_deg = 90.0F;
	}

	if(e->data.remote_player.death_time > 0) {
		render_entity_death_tilt(model,
			glm_deg(e->data.remote_player.body_yaw),
			mob_death_roll_deg(e->data.remote_player.death_time, td));
	}

	glm_mat4_mul(view, model, mv);

	// Head yaw is the real networked look (already degrees -- see the file
	// header note); body yaw is the client-side lag computed each tick above.
	float headYawDeg = lying ? lying_yaw_deg : e->orient[0];
	float bodyYawDeg = lying ? lying_yaw_deg
							 : glm_deg(e->data.remote_player.body_yaw);
	// Head pitch render follow-up (2026-08-26 residual item 3): e->orient[1]
	// is the real networked look pitch in this engine's OWN camera-frame
	// convention (90 = level, matching camera.c's GLM_PI_2 reset and
	// server_remote_protocol.c's CRPC_SPAWN_PLAYER boundary note "pitch +90,
	// inverse of ry - 90"). render_entity_player wants Beta's OWN
	// rotationPitch (0 = level, ABSOLUTE) -- subtract the same 90 the send
	// path adds, undoing that boundary conversion exactly once here.
	float headPitchDeg = lying ? 0.0F : e->orient[1] - 90.0F;
	int held_item = e->data.remote_player.has_held_item
		? e->data.remote_player.held_item.id
		: 0;
	// MP visual-parity pass item 3: EntityLiving.getSwingProgress's own
	// render-time lerp [740c583]: `prevSwingProgress + (swingProgress -
	// prevSwingProgress) * partialTick`.
	float swingProgress = e->data.remote_player.prev_swing_progress
		+ (e->data.remote_player.swing_progress
		   - e->data.remote_player.prev_swing_progress) * td;

	// Animation-parity pass item 1: RenderLiving.doRenderLiving's hurt-flash
	// bracket [740c583]: `if (var1.hurtTime > 0 ...) GL11.glColor4f(var25,
	// 0.0F, 0.0F, 0.4F)`, with the SAME shared combat.h hurt_wash_for_brightness
	// mobs use (R1b: var25 = model_light's lightmap entry). G8b: the gate is
	// Beta's full hurtTime > 0 || deathTime > 0 (RenderLiving.java:67-75) via
	// entity_living_red_tint, shared with entity_monster.c, so a dying body
	// stays red through the whole flop.
	bool red = entity_living_red_tint(e);
	if(red) {
		struct hurt_wash w = hurt_wash_for_brightness(light_brightness_current(model_light));
		gfx_entity_tint(w.r, w.g, w.b, w.amount);
	}

	render_entity_player(mv, headYawDeg, bodyYawDeg, headPitchDeg,
						 e->data.remote_player.frame, held_item,
						 e->data.remote_player.sneaking, swingProgress,
						 e->data.remote_player.is_riding
							 || e->data.remote_player.riding_vehicle,
						 e->data.remote_player.armor_id);

	if(red)
		gfx_entity_tint(0.0f, 0.0f, 0.0f, 0.0f); // mandatory reset -- GX FIFO safety
}

/* Local bodies use eye-based positions and owner inventories, unlike network
 * players. Draw only from the OTHER viewport; do not give the local entity a
 * generic render callback that would also draw inside its own camera. */
void entity_local_player_render_other(const struct client_session* owner,
	mat4 view, float td) {
	if(!owner || !owner->local_player || !owner->windows[WINDOWC_INVENTORY]) return;
	struct entity* e = owner->local_player;
	const struct hud_hurt_state* hurt = &owner->camera.hurt;
	/* Keep the local owner alive for its death GUI/respawn transaction, but
	 * retire its visible corpse like EntityLiving.onEntityUpdate (>20). */
	if(e->health <= 0 && hurt->death_time > 20) return;
	struct inventory* inv = windowc_get_latest(owner->windows[WINDOWC_INVENTORY]);
	vec3 p;
	glm_vec3_lerp(e->pos_old, e->pos, td, p);
	struct block_data blk;
	entity_get_block(e, floorf(e->pos[0]),
		underwater_overlay_sample_y(e->pos[1], e->height, PLAYER_EYE_HEIGHT),
		floorf(e->pos[2]), &blk);
	const uint8_t model_light = (uint8_t)((blk.torch_light << 4) | blk.sky_light);
	render_entity_update_light(model_light);
	p[1] -= PLAYER_EYE_HEIGHT;
	bool sneak = owner->player_sync.state_sneaking;
	if(sneak) p[1] -= .125F;
	mat4 model, mv;
	glm_translate_make(model, p);
	if(e->health <= 0 && hurt->death_time > 0) {
		/* Beta RenderLiving.rotateCorpse: sqrt((deathTime+partial-1)/20
		 * *1.6), clamped to one, then getDeathMaxRotation(player)=90,
		 * about the body's forward axis through the feet. G8: the shared
		 * formula + composition (was an inline copy of the same math). */
		render_entity_death_tilt(model, glm_deg(owner->camera.rx),
			mob_death_roll_deg(hurt->death_time, td));
	}
	glm_mat4_mul(view, model, mv);
	float dx = e->pos[0] - e->pos_old[0], dz = e->pos[2] - e->pos_old[2];
	render_entity_set_limb_swing(e->health > 0 ? fminf(1, sqrtf(dx*dx + dz*dz)*4) : 0);
	/* The owner already maintains a render-only walk odometer, even when
	 * first-person bobbing is disabled. Use distance so idle limbs do not cycle. */
	float walk = glm_lerp(owner->camera.bob.dwm_prev, owner->camera.bob.dwm, td);
	int frame = (int)(fmodf(walk, 4.0F) * 15);
	/* The peer pane sees this owner's swing (Beta: every client renders
	 * EntityPlayer.swingProgress); an equip-only dip is not a swing. */
	float swing = held_anim_swing_lerp(&owner->held_item_animation, time_get());
	struct item_data item;
	int held = inventory_get_hotbar_item(inv, &item) ? item.id : 0;
	uint16_t armor[4] = {0};
	for(unsigned i=0;i<4;i++)
		if(inventory_get_slot(inv, INVENTORY_SLOT_ARMOR+i, &item)) armor[i] = item.id;
	bool riding = false;
	dict_entity_it_t it;
	dict_entity_it(it, world_view_entities());
	while(!dict_entity_end_p(it)) {
		const struct entity* mount = dict_entity_ref(it)->value;
		if(mount && ((mount->type == ENTITY_MINECART
			&& mount->data.minecart.occupied && mount->data.minecart.occupant_id == e->id)
			|| (mount->type == ENTITY_BOAT && mount->data.boat.occupied
				&& mount->data.boat.occupant_id == e->id)
			|| (mount->type == ENTITY_MONSTER && mount->data.monster.id == MONSTER_PIG
				&& mount->data.monster.pig_ridden && mount->data.monster.pig_rider_id == e->id))) {
			riding = true;
			break;
		}
		dict_entity_next(it);
	}
	/* Health deltas feed the local owner's timer, not e->hurt (the
	 * network entity-status flag). Beta tints both hurt and dying bodies. */
	bool tinted = e->hurt || hurt->hurt_time > 0 || hurt->death_time > 0;
	if(tinted) {
		struct hurt_wash w = hurt_wash_for_brightness(light_brightness_current(model_light));
		gfx_entity_tint(w.r, w.g, w.b, w.amount);
	}
	render_entity_player(mv, glm_deg(owner->camera.rx), glm_deg(owner->camera.rx),
		glm_deg(owner->camera.ry)-90, frame, held, sneak, swing,
		riding, armor);
	if(tinted) gfx_entity_tint(0,0,0,0);
}

void entity_remote_player(uint32_t id, struct entity* e, void* world,
						  const char* username) {
	assert(e && world);

	entity_default_init(e, false, world);
	// Beta player bbox: 0.6 wide, 1.8 tall, no y-offset (matches
	// entity_local_player.c's own setSize call).
	entity_set_size(e, 0.6F, 1.8F, 0.0F);
	e->id = id;
	e->type = ENTITY_REMOTE_PLAYER;
	e->name = "Player";
	e->tick_server = NULL;
	e->tick_client = entity_remote_player_client_tick;
	e->render = entity_remote_player_render;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = remote_player_get_bounding_box;
	/* Field defect 2026-08-31 (maintainer, live Wii vs Ghost2): remote
	 * players were unclickable -- mobs got the SRPC_ENTITY_ATTACK
	 * left-click handler, players never did, so PvP swings sent NOTHING
	 * (the --live-pvp leg passed only because it bypasses the click UI).
	 * Beta attacks players through the very same
	 * PlayerControllerMP.attackEntity -> Packet7UseEntity path as mobs
	 * [740c583 PlayerControllerMP.attackEntity], so reuse
	 * entity_monster.c's exported handler verbatim (CODESTYLE second-
	 * transcription rule). */
	e->onLeftClick = onLeftClick;
	e->leftClickText = "Attack";
	e->onRightClick = NULL;
	e->rightClickText = NULL;
	e->delay_destroy = -1;

	// Zeroes username/held_item/has_held_item AND the render-follow-up fields
	// (body_yaw/frame/frame_time_left/limb_swing) -- see entity.h's
	// entity_remote_player struct comment for why this member, unlike
	// entity_monster's, is memset-zeroed rather than explicitly initialized
	// field-by-field.
	memset(&e->data.remote_player, 0, sizeof(e->data.remote_player));
	if(username) {
		size_t cap = sizeof(e->data.remote_player.username) - 1;
		size_t len = strlen(username);
		if(len > cap)
			len = cap;
		memcpy(e->data.remote_player.username, username, len);
		e->data.remote_player.username[len] = '\0';
	}
	glm_vec3_copy(e->pos, e->network_pos);
}

/* MP-BED: EntityPlayer.sleepInBedAt (EntityPlayer.java:537-588) on the
 * multiplayer client: the multiplayerWorld branch skips every refusal check,
 * then setSize(0.2F, 0.2F), yOffset = 0.2F (applied at render time above),
 * and -- when World.blockExists -- the bed's direction (BlockBed.
 * getDirectionFromMetadata = meta & 3) picks the in-bed spot and
 * func_22052_e's render offsets; otherwise the cell centre and no new
 * offsets. getBedOrientationInDegrees (EntityPlayer.java:663-680) reads the
 * same metadata. Motion is zeroed; sleeping = true. */
void entity_remote_player_sleep_at(struct entity* e, int32_t bed_x,
								   int32_t bed_y, int32_t bed_z,
								   bool block_exists, uint8_t bed_meta) {
	assert(e && e->type == ENTITY_REMOTE_PLAYER);
	entity_set_size(e, 0.2F, 0.2F, 0.0F);
	float fx = 0.5F, fz = 0.5F;
	int dir = bed_meta & 3;
	if(block_exists) {
		e->data.remote_player.sleep_off_x = 0.0F;
		e->data.remote_player.sleep_off_z = 0.0F;
		switch(dir) {
			case 0: fz = 0.9F; e->data.remote_player.sleep_off_z = -1.8F; break;
			case 1: fx = 0.1F; e->data.remote_player.sleep_off_x = 1.8F; break;
			case 2: fz = 0.1F; e->data.remote_player.sleep_off_z = 1.8F; break;
			default: fx = 0.9F; e->data.remote_player.sleep_off_x = -1.8F; break;
		}
	}
	static const float orient[4] = {90.0F, 0.0F, 270.0F, 180.0F};
	e->data.remote_player.bed_orient_deg = block_exists ? orient[dir] : 0.0F;
	vec3 pos = {(float)bed_x + fx, (float)bed_y + 15.0F / 16.0F,
				(float)bed_z + fz};
	e->teleport(e, pos);
	glm_vec3_zero(e->vel);
	e->data.remote_player.sleeping = true;
}

/* MP-BED: EntityPlayer.wakeUpPlayer(false, false, false) (EntityPlayer.java:
 * 613-640): setSize(0.6F, 1.8F), resetHeight (EntityOtherPlayerMP yOffset 0),
 * sleeping = false. Its two world-touching steps are left to the server,
 * which runs the same wakeUpPlayer and publishes both results: the bed's
 * occupied bit (BlockBed.setBedOccupied -> Packet53) and the stand-up spot
 * (the tracker's next move/teleport for this player). */
void entity_remote_player_wake(struct entity* e) {
	assert(e && e->type == ENTITY_REMOTE_PLAYER);
	entity_set_size(e, 0.6F, 1.8F, 0.0F);
	e->data.remote_player.sleeping = false;
}

/* MP-RIDE: Entity.updateRiderPosition (Entity.java:929-931): rider at
 * vehicle.posY + getMountedYOffset() + rider.getYOffset(). A remote player's
 * getYOffset is yOffset - 0.5F = -0.5 (EntityPlayer.java:479-481,
 * EntityOtherPlayerMP.resetHeight). getMountedYOffset: boat and minecart
 * height * 0 - 0.3F (EntityBoat.java:59-61, EntityMinecart.java:69-71),
 * spider height * 0.75 - 0.5 (EntitySpider.java:11-13), every other entity
 * height * 0.75 (Entity.java:937-939). This engine's vehicle pos is Beta's
 * posY for all of them (clin_apply_attach's box-top notes). The boat's own
 * override also shifts the rider 0.4 along the boat's yaw
 * (EntityBoat.java:318-323); remote boats carry no yaw on this client
 * (CRPC_ENTITY_LOOK feeds players and mobs only), so that horizontal 0.4 is
 * not reproduced -- a documented residual; the vertical seat is exact. */
bool entity_remote_player_rider_pos(const struct entity* vehicle, vec3 out) {
	if(!vehicle)
		return false;
	float mounted;
	switch(vehicle->type) {
		case ENTITY_BOAT:
		case ENTITY_MINECART: mounted = -0.3F; break;
		case ENTITY_MONSTER:
			mounted = vehicle->height * 0.75F;
			if(vehicle->data.monster.id == MONSTER_SPIDER)
				mounted -= 0.5F;
			break;
		default: return false;
	}
	out[0] = vehicle->pos[0];
	out[1] = vehicle->pos[1] + mounted - 0.5F;
	out[2] = vehicle->pos[2];
	return true;
}

/* MP-RIDE: Entity.mountEntity for a remote player (Entity.java:941-967), as
 * NetClientHandler.func_6497_a drives it (java:439-449). vehicle NULL =
 * mountEntity(null); the same vehicle again toggles the ride off; anything
 * else becomes ridingEntity. Leaving a vehicle puts the rider at
 * setLocationAndAngles(vehicle.posX, vehicle.boundingBox.minY +
 * vehicle.height, vehicle.posZ) -- feet, since a remote player's yOffset is
 * 0 -- when that vehicle is still known. */
void entity_remote_player_mount(struct entity* rider, struct entity* vehicle,
								struct dict_entity_s* entities) {
	assert(rider && rider->type == ENTITY_REMOTE_PLAYER);
	struct entity* current = NULL;
	if(rider->data.remote_player.riding_vehicle && entities) {
		struct entity** cp = dict_entity_get(*(dict_entity_t*)entities,
			rider->data.remote_player.riding_vehicle_id);
		current = cp ? *cp : NULL;
	}
	bool leave = !vehicle
		|| (rider->data.remote_player.riding_vehicle
			&& rider->data.remote_player.riding_vehicle_id == vehicle->id);
	if(leave) {
		if(rider->data.remote_player.riding_vehicle && current) {
			struct AABB box;
			if(current->getBoundingBox
			   && current->getBoundingBox(current, &box)) {
				vec3 to = {current->pos[0], box.y2, current->pos[2]};
				rider->teleport(rider, to);
			}
		}
		rider->data.remote_player.riding_vehicle = false;
		rider->data.remote_player.riding_vehicle_id = 0;
		return;
	}
	rider->data.remote_player.riding_vehicle = true;
	rider->data.remote_player.riding_vehicle_id = vehicle->id;
}
