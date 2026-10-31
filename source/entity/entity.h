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

#ifndef ENTITY_H
#define ENTITY_H

#include <m-lib/m-dict.h>
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../cglm/cglm.h"
#include "../item/item_types.h"
#include "../platform/thread.h"
#include "../util.h"
#include "../worldgen/java_random.h" // mob-owned java.util.Random (Entity.rand)
#include "actor_ident.h" // struct actor_ident (S1-a2 lifetime-safe actor roles)
#include "monsters_object.h"
#include "mob_windup_cue.h"
#include "pathfinding.h" // FH-13: struct path_entity (waypoint-follow chase/wander)
#include "mob_motion.h" // mob motion units S1: struct mob_motion

struct camera;

// EntityPickupFX.maxAge [740c583] -- the exact 3-tick Beta pickup-flight
// duration; see entity_item's pickup_flight_* fields and entity_item.c.
#define ENTITY_ITEM_PICKUP_FLIGHT_TICKS 3


/* Local arrow presentation, in Beta degrees. Optional in the existing
 * SPAWN/MOVE envelope; remote producers leave valid false. */
struct arrow_visual_state {
	vec2 rotation, previous; /* yaw, pitch */
	uint8_t shake_ticks;
	bool valid;
};

enum entity_type {
	ENTITY_LOCAL_PLAYER,
	ENTITY_ITEM,
	ENTITY_MONSTER,
	ENTITY_MINECART,
	ENTITY_ARROW,
	// Beta EntityTNTPrimed: a real moving entity whose fuse is independent
	// of World's scheduled-block-tick queue. Appended to preserve every
	// existing entity_type numeric value used by diagnostics.
	ENTITY_PRIMED_TNT,
	// Beta EntityEgg (M4 S5): server-side thrown projectile. Appended per the
	// same numeric-stability rule. The CLIENT never constructs this type -- the
	// egg's client counterpart is the CRPC_SPAWN_ARROW display entity carrying
	// item_id ITEM_EGG. RenderSnowball's half-block camera-facing billboard
	// draws it, while gstate.entities still holds an ENTITY_ARROW.
	ENTITY_EGG,
	// Beta EntitySnowball (2026-08-07 caveat batch 1a): the egg's twin minus
	// the chick roll -- shares entity_egg.c's tick and the entity_egg data
	// union member. Appended per the numeric-stability rule; client display is
	// the same CRPC_SPAWN_ARROW display carrying ITEM_SNOW_BALL and using
	// RenderSnowball's half-block camera-facing billboard.
	ENTITY_SNOWBALL,
	// Beta EntityFireball (batch 1b, the ghast's shot): 1x1 accel-driven
	// projectile, no gravity, drag 0.95/0.8, zero direct hit damage -- the
	// strength-1.0 FLAMING explosion is the payload. Punch-deflectable
	// (entity_fireball_deflect). Client display is the CRPC_SPAWN_ARROW
	// billboard with ITEM_SNOW_BALL -- Beta's OWN RenderFireball draws the
	// snowball item sprite (2x scale).
	ENTITY_FIREBALL,
	// Beta EntityFish (fishing-slice): the SERVER-side fishing-rod bobber.
	// Appended per the numeric-stability rule. Server-only, like the egg --
	// the client display is the SAME CRPC_SPAWN_ARROW billboard mechanism,
	// carrying ITEM_FISHING_ROD (no dedicated bobber sprite/model exists;
	// documented cosmetic substitution, entity_fish.c file header). NOT
	// archived (server_entity_archive_kind has no case for it, matching the
	// local player's own exclusion) -- see entity_fish.c for the citation:
	// Beta's own EntityFish.readEntityFromNBT never relinks `angler`, so a
	// hook reconstructed from a chunk reload is a latent null-angler bug in
	// REAL Beta, not a behavior worth porting.
	ENTITY_FISH,
	// Beta EntityBoat (boat-slice, 2026-08-13): server-authoritative floating
	// vehicle. Shares EntityMinecart's exact damage/shake/rock-direction shape
	// (attackEntityFrom, onUpdate decay, the >40 destroy-and-drop threshold) but
	// is NOT rail-bound: buoyancy sampling + drag + a speed-gated land-crash
	// destroy replace the rail physics. Appended per the numeric-stability rule.
	ENTITY_BOAT,
	// MP S3 (plan section 6.3): a remote (other-player) render entity, spawned
	// only by server_remote's CRPC_SPAWN_PLAYER. Deliberately NOT a
	// pluralized local player -- "a remote player is a render entity, never
	// players[N]". Appended per the numeric-stability rule. Render-follow-up:
	// e->render is now render_entity_player (entity_remote_player.c) -- the
	// LOCAL player still has e->render == NULL (entity_local_player.c; no
	// 1st-person body needed), but a REMOTE player needs a visible 3rd-person
	// biped since another client is watching it from outside. Position/look
	// flow through the existing generic CRPC_ENTITY_MOVE/CRPC_ENTITY_DESTROY
	// boundary plus the new CRPC_ENTITY_LOOK (client_interface.h).
	ENTITY_REMOTE_PLAYER,
	// Beta EntityPainting (painting-slice, gap audit #9): a wall-mounted
	// static "hanging" entity, never affected by gravity/physics -- Beta's
	// own moveEntity/addVelocity overrides pop it into a dropped item on ANY
	// attempted forced movement. NOT wired, but not because no mechanism
	// exists: block/redstone_piston_server.c's pmove_push_entity_candidate IS
	// this engine's real generic entity-pushing path (a first draft of this
	// comment claimed otherwise from a grep of the wrong file,
	// block/block_piston.c -- corrected by post-review finding #2). What
	// actually keeps a painting out of that sweep today is
	// e->getBoundingBox staying NULL (entity_painting.c's
	// entity_painting(), matching Beta -- only EntityBoat/EntityMinecart
	// override that accessor): pmove_push_entity_candidate's own first guard
	// clause is `if(!e || e->delay_destroy == 0 || !e->getBoundingBox)
	// return;`, so a painting is invisible to the sweep rather than being
	// pushed by it. That is correct exclusion, not the Beta pop-into-drop
	// REACTION -- a piston extending into a painting's space today silently
	// does nothing to it (no push, no pop), which is the honestly-open gap;
	// see docs/deviations.md's painting entry. Appended AFTER
	// ENTITY_REMOTE_PLAYER (not directly after ENTITY_BOAT, where this slice
	// originally sat in its source worktree at base 558a7f4/2026-08-13) --
	// MP S3's remote-player kind landed on tip in the interim and already
	// claims the next numeric slot, so true append-only numeric stability
	// means going after IT, not resurrecting a now-taken value.
	ENTITY_PAINTING,
	// Beta EntityFallingSand (Batch I, audit register worldgen-saves #1: the
	// largest confirmed parity gap -- sand/gravel never fell). A real moving
	// entity, like EntityTNTPrimed, spawned by BlockSand.tryToFall's scheduled
	// tick when the block below is passable. Carries which block id it is
	// (BLOCK_SAND or BLOCK_GRAVEL); on landing it re-places that block (or, if
	// the landing cell can't accept it, drops it as an item). Appended after
	// ENTITY_PAINTING per the numeric-stability rule.
	ENTITY_FALLING_SAND
};

enum ai_state {
    AI_IDLE,
    AI_CHASE,
    AI_FUSE,
    AI_ATTACK,
    // todo: add more
};


struct server_local;

struct AABB;


struct entity {
	uint32_t id;
	bool on_server;
	void* world;
	/* A2-E (split-dimensions-plan-0924, design M6): the GENERATION-SAFE
	 * binding of a server entity to the world instance it was constructed
	 * into -- the (slot, generation) of `world` at bind time
	 * (entity_bind_world). `world` is only a synchronous resolved borrow: a
	 * world's storage slot is reused by its successor (same address, new
	 * generation), so server_local_entity_world resolves the STAMP, never the
	 * pointer, and yields NULL for an entity of a retired generation. Both
	 * zero = unbound (a hand-built fixture entity, or a client entity): the
	 * legacy meaning "the routed world" applies. */
	uint8_t world_slot;
	uint32_t world_generation;
	/* Runtime transport claim: set only after an A2-W spawn enters the client
	 * FIFO. Zero means the entity has not been published in this generation;
	 * scoped stream reset/rebind clears it. Never persisted or simulated. */
	uint8_t client_pub_slot;
	uint32_t client_pub_generation;
	int delay_destroy;
	short health;
	struct item_data drop_item;

	vec3 pos;
	vec3 pos_old;
	vec3 vel;
	vec2 orient;
	vec2 orient_old;
	/* Beta Entity's persistent physical state. `pos[1]` is the logical Y;
	 * physical minY is posY-y_offset+y_size. Keeping these fields on the
	 * common entity is required because every subclass inherits moveEntity. */
	float width;
	float height;
	float y_offset;
	float y_size;
	float step_height;
	float fall_distance;
	bool on_ground;
	bool collided_horizontally;
	bool collided_vertically;
	bool collided;
	bool in_web;
	/* Beta Chunk.addEntity membership. Entity queries walk chunk X, chunk Z,
	 * vertical section, then each section's insertion-ordered list. Moving to a
	 * different tuple removes/re-appends the entity and assigns a fresh order. */
	int32_t chunk_coord_x;
	int32_t chunk_coord_y;
	int32_t chunk_coord_z;
	uint64_t chunk_list_order;
	bool added_to_chunk;

	vec3 network_pos;
	/* Beta client-side motion smoothing (EntityLiving.newPosRotationIncrements):
	 * every remote-server move/teleport/look targets network_pos/network_orient
	 * and arms network_increments=3; the client tick then steps
	 * pos += (target-pos)/increments (yaw via the shortest wrapped delta) and
	 * decrements, landing EXACTLY on the target when it reaches 0. increments==0
	 * is the local-provider path: same-tick snap to network_pos, which
	 * reproduces Beta singleplayer's direct per-tick movement (the render layer
	 * still lerps pos_old->pos). [740c583 EntityLiving.setPositionAndRotation2]
	 * [740c583 EntityLiving.onLivingUpdate] [740c583
	 * NetClientHandler.handleEntity] (increments always 3 on the wire).
	 * network_orient is consumed only where a networked look exists (remote
	 * players); mob head yaw stays movement-derived (entity_monster.c). */
	vec2 network_orient;
	uint8_t network_increments;

	bool on_fire; // client-visible burning flag (networked via CRPC_ENTITY_MOVE)
	bool swelling; // client-visible creeper-swelling flag (networked)
	bool hurt; // client-visible mob-hurt flag (hurt_time>0): drives the audit #17f
	           // red hurt-flash overlay. Networked via CRPC_ENTITY_MOVE (hurt_time
	           // itself is server-side only, like on_fire's fire_time).

    float         detection_range;
    float         ai_timer;
    enum ai_state ai_state;

	bool (*tick_client)(struct entity*);
	bool (*tick_server)(struct entity*, struct server_local*);
	void (*render)(struct entity*, mat4, float);
	void (*teleport)(struct entity*, vec3);

	enum entity_type type;
    const char *name;
	size_t (*getBoundingBox)(const struct entity *e, struct AABB *out);
    const char *leftClickText;
    const char *rightClickText;
    bool (*onRightClick)(struct entity *e);
    bool (*onLeftClick)(struct entity *e);
	union entity_data {
		struct entity_local_player {
			int jump_ticks;
			bool capture_input;
			// --- MP client-side footstep odometer (remote_session only; in SP
			// the server plays footsteps from SRPC_PLAYER_POS deltas instead --
			// server_local.c). Beta Entity.distanceWalkedModified /
			// nextStepDistance, sharing server_player_walk_step's exact
			// cadence like the mob odometer. [740c583: Entity.moveEntity:478]
			float walk_dist;    // distanceWalkedModified (starts 0)
			int walk_next_step; // nextStepDistance (starts 1)
			// Boat rider steering SOURCE (docs/deviations.md "Boat rider
			// steering"): the rider's own horizontal input velocity,
			// recomputed FRESH every tick while boat-mounted -- mirrors
			// Beta's riddenByEntity.motionX/Z, which Entity.updateRidden
			// zeroes then re-derives from moveFlying every tick [740c583:
			// Entity.updateRidden, EntityLiving.moveEntityWithHeading].
			// Deliberately SEPARATE from e->vel[0]/[2] (which stays
			// byte-unchanged while mounted -- see the BOAT MOUNT branch in
			// entity_local_player.c and test_boat_env.c section (9)'s own
			// "no ordinary walking-physics leakage" invariant): Beta's
			// riddenByEntity.motionX/Z is a genuinely different quantity --
			// it never accumulates tick-to-tick and never drives this
			// entity's own position (the tether above already owns that).
			// 0.0 (no input) whenever not boat-mounted or capture_input is
			// off.
			float boat_rider_vel_x, boat_rider_vel_z;
		} local_player;
		struct entity_item {
			struct item_data item;
			int age;
			/* Archive-retained common Entity scalars [740c583 Entity.writeToNBT].
			 * persisted_fire IS the live Beta Entity.fire field for items since
			 * next-gaps-0924/W1C (fire DoT, lava 600, burn-tail 300/-1 in
			 * entity_item.c); air is still archive-only (items never drown). */
			int16_t persisted_fire;
			int16_t persisted_air;
			// FH-12: Beta EntityItem.delayBeforeCanPickup [740c583] -- a REAL
			// per-instance field, distinct from `age` above (age drives the
			// 6000-tick despawn only). Default 0 (the Java field default; left
			// untouched by e.g. BlockChest/BlockDispenser/BlockFurnace
			// onBlockRemoval spill and BlockDispenser.dispense -- none of
			// those Beta methods ever touch the field). Set to 10 by every
			// drop that funnels through Block.dropBlockAsItem_do or
			// Entity.entityDropItem (ordinary block breaks, mob death drops,
			// jukebox eject, boat destruction, sheep shearing, leaf-shears
			// harvest) and to 40 ONLY by
			// EntityPlayer.dropPlayerItemWithRandomChoice (Q-drop, the GUI
			// cursor drop, and player death drops -- InventoryPlayer
			// .dropAllItems calls it too, so death drops ARE 40, not 0).
			// Decremented once per tick, unconditionally, in
			// entity_server_tick (mirrors EntityItem.onUpdate's
			// `if(delayBeforeCanPickup>0) --delayBeforeCanPickup;`, which
			// runs BEFORE the age/pickup logic). Gates onCollideWithPlayer's
			// pickup check (`delayBeforeCanPickup == 0`) -- replaces the old
			// flat `age >= 2*4` universal gate. NOT persisted: Beta's own
			// writeEntityToNBT/readEntityFromNBT touch only Health/Age/Item,
			// so entity_archive_copy/restore deliberately never copy this
			// field -- a reloaded item always comes back at entity_item()'s
			// default (0) regardless of its value before the save.
			int pickup_delay;
			/* Nonzero only while/after a crash-safe Nether-death transfer.
			 * sequence identifies this exact stack within death_drops.dat. */
			uint32_t death_transaction;
			uint16_t death_sequence;
			bool was_in_water; // S2 splash latch (EntityItem calls
			                   // super.onUpdate() -> Entity splash edge).
			                   // Starts TRUE: an underwater mining drop or
			                   // death drop spawning INTO water never splashes
			                   // (the isFirstUpdate guard). Runtime-only.
			// MP visual-parity pass (2026-08-31) item 4: cosmetic pickup
			// flight toward the LOCAL collector, deviating from Beta's own
			// architecture (a SEPARATE EntityPickupFX particle entity) by
			// reusing this same item entity and delaying its destroy instead
			// -- see client_interface.c's CRPC_PICKUP_ITEM/CRPC_ENTITY_DESTROY
			// consumers and entity_item.c's entity_client_tick/entity_render
			// for the full citation [740c583 NetClientHandler.handleCollect +
			// EntityPickupFX ctor/onUpdate/renderParticle]. Armed only for the
			// local-collector case (the pre-existing CRPC_PICKUP_ITEM scope);
			// a bystander watching someone else's pickup still sees the old
			// instant destroy -- documented residual, not a silent gap.
			bool pickup_flight_active;
			int pickup_flight_age; // 0..ENTITY_ITEM_PICKUP_FLIGHT_TICKS
			vec3 pickup_flight_start; // item pos frozen at collection time
		} item;
		struct entity_monster {
			int id;
			// WOLF dataWatcher state [740c583: EntityWolf.entityInit]. Flags are
			// watcher 16 (sit/angry/tamed); owner is watcher 17; watched_health is
			// watcher 18. UINT32_MAX means no wolf target, 0 is the local player.
			uint8_t wolf_flags;
			char wolf_owner[WOLF_OWNER_MAX];
			int wolf_watched_health;
			uint32_t wolf_target_id;
			bool wolf_shake_armed;
			bool wolf_shake_active;
			float wolf_shake_time;
			float wolf_prev_shake_time;
			bool wolf_interested;
			float wolf_head_rotation;
			float wolf_head_rotation_old;
			// Ambient look-at-nearby-player latch [740c583:
			// EntityLiving.hasCurrentTarget/getCurrentTarget/currentTarget].
			// UNRELATED to wolf_target_id above (EntityCreature.playerToAttack,
			// the retaliation/sheep-chase/owner-follow target) -- Beta keeps the
			// two fields entirely separate, and only looksWithInterest gating
			// (entity_monster.c) consumes this one. wolf_current_target_hold
			// mirrors numTicksToChaseTarget's countdown while latched.
			bool wolf_has_current_target;
			int wolf_current_target_hold;
			uint8_t fleece_color; // sheep wool metadata; explicit so fresh runtime
			                       // ids on reload do not change appearance/drops
			bool sheared; // sheep: Beta EntitySheep "Sheared" NBT / dataWatcher
			              // bit 0x10. Set by the shears interact, never cleared --
			              // b1.7.3 has NO wool regrowth (no grass-eating; sheared
			              // is permanent). Gates the wool overlay render pass, the
			              // 1-wool death drop, and re-shearing. Networked via
			              // CRPC_SPAWN_MONSTER + CRPC_ENTITY_MOVE, archived as
			              // CaveWiiSheared. [740c583: EntitySheep]
			// PIG saddle [740c583: EntityPig]. dataWatcher byte 16 bit 0
			// (getSaddled/setSaddled); set by ItemSaddle.hitEntity (attack, NOT
			// right-click -- b1.7.3 has no use-on-entity path for the saddle).
			// Networked via CRPC_SPAWN_MONSTER + CRPC_ENTITY_MOVE (the
			// sheared/fleece_color precedent) and archived as the exact Beta tag
			// "Saddle" -- unlike Sheared/EggTimer this field has no CaveWii-only
			// sibling data crowding its compound, so the archive uses Beta's own
			// name directly.
			bool saddled;
			// Mount relationship [740c583: Entity.riddenByEntity / mountEntity;
			// EntityPig.interact]. pig_rider_id names the rider ONLY while
			// pig_ridden is true: entity id 0 is SERVER_LOCAL_PLAYER_ENTITY_ID
			// (a legitimate rider), so it cannot double as its own "no rider"
			// sentinel -- a separate bool disambiguates it exactly like
			// entity_minecart's occupied/occupant_id pair. Server-authoritative,
			// RUNTIME-ONLY: never archived (Beta writes no riding relationship
			// for ANY entity -- EntityPig.writeEntityToNBT writes only "Saddle" --
			// confirmed by the minecart slice's identical finding: "Beta persists
			// NO rider" -- so a save/evict/reload always comes back unridden).
			// Networked via CRPC_ENTITY_MOVE (review-fix finding 1, same
			// live-update rule as saddled). Two claims of DIFFERENT status live
			// near this field; do not conflate them (review-fix finding 6):
			//  - Rider position-follow IS implemented and is ordinary Beta parity,
			//    not a gap: [740c583 Entity.updateRidden/updateRiderPosition]
			//    forces the rider onto pig.pos + PIG_MOUNTED_Y_OFFSET +
			//    PLAYER_EYE_HEIGHT every server tick (entity_monster.c's
			//    server_tick_monster, tail of the pig's own tick so the mount's
			//    position is already final -- matches World.updateEntityWithOptional
			//    Force's recursive rider-update-from-the-mount's-own-slot order),
			//    zeroing the rider's velocity and rejecting free movement by simply
			//    overwriting whatever SRPC_PLAYER_POS supplied that same tick
			//    (server_local_update runs svin_process_messages BEFORE the entity
			//    pass, so this follow always has final say). The client mirrors the
			//    identical formula in entity_local_player.c so the camera attaches
			//    via the existing generic e->pos-driven camera_attach -- no
			//    source/graphics/ edit needed. This is a HARD SNAP each tick, not
			//    the minecart's smoothed gstate.players[0].ride.camera_correction -- Beta's own
			//    non-networked integrated-server model has no inter-frame gap to
			//    smooth over either, so skipping that smoothing is not tracked as a
			//    docs/deviations.md deviation (see SADDLE_SLICE_NOTES.md); it is a
			//    documented, deferred POLISH cut, not a correctness one.
			//  - Steering is PERMANENTLY absent, by design, matching Beta exactly:
			//    EntityPig has no moveEntityWithHeading override, so no rider input
			//    is ever read (proved by the S4 negative-control test in
			//    tests/test_pig_saddle_env.c: a ridden pig's motion is
			//    bit-identical to an unridden pig's under the same pinned RNG).
			//    Do NOT add it.
			bool pig_ridden;
			uint32_t pig_rider_id;
			/* S1-a2: WHICH actor pig_rider_id names. Beta's riddenByEntity is
			 * a live EntityPlayer reference [740c583 Entity.mountEntity];
			 * pig_rider_id alone cannot distinguish two local actors because
			 * both would have to be the one client-side local-player id
			 * SERVER_LOCAL_PLAYER_ENTITY_ID. Zero == the host, so the existing
			 * single-rider behaviour is byte-identical. Meaningful ONLY while
			 * pig_ridden; a rider whose identity stops resolving (guest leave,
			 * death, rejoin) is DISMOUNTED by the entity tick, exactly as Beta
			 * forces a dismount when the rider is gone. Runtime-only, never
			 * archived -- same rule as pig_rider_id itself. */
			struct actor_ident pig_rider_actor;
			/* G8-VEHICLE-SEAMS-01: a guest seat correction the guest's private
			 * route refused (token pool full); retried on the next ridden tick
			 * even when the seat did not move. Runtime-only. */
			bool pig_seat_correction_pending;
			// Spider jockey mount linkage [740c583: SpawnerAnimals.
			// creatureSpecificInit + Entity.mountEntity/ridingEntity/
			// riddenByEntity]. Generic single-rider MOB-ON-MOB mount pair
			// between two independent ENTITY_MONSTER entities -- distinct
			// from pig_rider_id above (a PLAYER-on-mob saddle mount, whose
			// "rider" lives in server_local's player state, not another
			// entity struct). The only Beta 1.7.3 case that mounts one
			// mob onto another is the 1% skeleton-on-spider "spider
			// jockey" rolled once per successfully spawned EntitySpider in
			// SpawnerAnimals.creatureSpecificInit (immediately after
			// entityJoinedWorld, before the pack's getMaxSpawnedInChunk
			// check) -- so this pair is scoped to that one use, not a
			// general N-mob riding graph.
			//   - jockey_mount_id: the entity id THIS mob is RIDING
			//     (Entity.ridingEntity); UINT32_MAX = not riding anything.
			//     Meaningful on the skeleton half of a jockey pair.
			//   - jockey_rider_id: the entity id RIDING this mob
			//     (Entity.riddenByEntity); UINT32_MAX = no rider.
			//     Meaningful on the spider half of a jockey pair.
			// RUNTIME-ONLY, never archived: Beta writes NO riding
			// relationship to NBT for any entity (EntitySpider/
			// EntitySkeleton.writeEntityToNBT call only super, and
			// Entity's own writeToNBTOptional carries no ridingEntity/
			// riddenByEntity field at all) -- the SAME "Beta persists NO
			// rider" finding already recorded for pig_rider_id above and
			// for entity_minecart's occupant, so a save/evict/reload
			// always comes back unmounted, faithfully.
			uint32_t jockey_mount_id;
			uint32_t jockey_rider_id;
			int egg_timer; // chicken: Beta EntityChicken.timeUntilNextEgg. 0 =
			               // "unrolled" sentinel (retained deferral -- the slime
			               // jump_delay precedent): the first server tick
			               // rolls Beta's rand.nextInt(6000)+6000 (6000..11999)
			               // without laying, then each tick decrements; at <= 0
			               // lay 1 egg + re-roll. Archived as CaveWiiEggTimer
			               // (absent -> 0 -> re-roll on load).
			               // [740c583: EntityChicken ctor + onLivingUpdate]
			int frame;
			int frame_time_left;
			vec2 direction;
			int direction_time;
			float body_yaw;
		    float head_yaw;
		    /* MP wire-driven head look (2026-08-26 residual item 2): a SEPARATE
		     * pair from network_orient/network_increments above -- those stay
		     * POSITION-only for a mob (client_tick_monster calls
		     * entity_network_approach_tick(e, false)), and their unit/sign
		     * convention (player-frame yaw, mirrored + combined with
		     * network_increments shared against position) is incompatible with
		     * this file's own head_yaw (radians, atan2(dx,dz) movement-derived
		     * convention, e->orient[1] holding body+head combined for render).
		     * Reusing network_orient/network_increments for a mob would collide
		     * with CRPC_ENTITY_MOVE's own unconditional rearm-to-current-orient
		     * rule and require sign/unit translation on every read; a dedicated
		     * pair avoids both traps. Armed by CRPC_ENTITY_LOOK (client_interface.c)
		     * when a real Packet32/33/34 look arrives; client_tick_monster
		     * approaches head_yaw toward network_head_yaw over
		     * network_head_increments ticks (the same Beta 3-increment
		     * mechanism, scoped to this pair) and falls back to the
		     * movement-derived atan2 once the counter reaches 0 -- Beta's own
		     * "non-rotating" no-op-target case. [740c583 NetClientHandler.
		     * handleEntity rotating flag] Zero for every non-networked (SP)
		     * mob, so client_tick_monster's behaviour there is unchanged. */
		    float network_head_yaw;
		    uint8_t network_head_increments;
		    /* Beta EntityLiving idle head-turn state (updatePlayerActionState's
		     * FALLBACK, reached whenever EntityCreature has no active path this
		     * tick) [740c583: EntityLiving.currentTarget /
		     * numTicksToChaseTarget / randomYawVelocity]. Purely cosmetic --
		     * moveForward/moveStrafing stay 0 (no walking) and no attack or
		     * acquisition decision reads it; those stay server-authoritative
		     * via aggro_time. Like head_yaw's own movement-derived fallback
		     * immediately above, this is CLIENT-derived (non-networked, SP and
		     * host-local by design -- see entity_monster.c's
		     * mob_idle_look_tick), so it draws from the client's own
		     * non-deterministic cosmetic RNG rather than the server's seeded
		     * s->rand_src: no simulation RNG draw-count change. Reset at spawn
		     * (entity_monster factory) like every other non-memset field here. */
		    bool idle_has_target;      // Beta currentTarget != null
		    int idle_target_ticks;     // Beta numTicksToChaseTarget
		    float idle_random_yaw_vel; // Beta randomYawVelocity, RADIANS/tick
		    int fuse;
		    // Beta EntityCreeper's SECOND, INDEPENDENT state variable: the DataWatcher
		    // creeperState byte (id 16), -1 (idle/deflating) or 1 (actively priming this
		    // tick). This is the hysteresis SELECTOR read by attackEntity's
		    // `int var3 = this.getCreeperState()` -- it is set to -1 by the else/deflate
		    // branch on EVERY failing tick (attackEntity's else, attackBlockedEntity, and
		    // the onUpdate null-target tail all do the identical
		    // `setCreeperState(-1); --timeSinceIgnited;`) and LATCHES there, forcing the
		    // tight 3-block re-ignite test, until a tick actually satisfies dist<3.0 --
		    // regardless of how much `fuse` (Beta's timeSinceIgnited ramp) remains banked.
		    // Deliberately kept SEPARATE from `fuse` (parity fix 2026-08-31): deriving the
		    // hysteresis selector from `fuse >= 0` instead of this persisted flag let a
		    // creeper that had only partially deflated re-arm on the loose 7-block test
		    // instead of Beta's tight 3-block one. [740c583: EntityCreeper.attackEntity /
		    // .attackBlockedEntity / .onUpdate] Not persisted (Beta's writeEntityToNBT
		    // never saves creeperState either) -- reset to -1 alongside `fuse` on spawn
		    // and on load.
		    int8_t creeper_state;
		    // Wii cap ownership: once the fuse reaches its terminal edge but
		    // admission is full, this creeper is an immutable pending blast. It is
		    // neither targetable nor simulated until the pre-RPC retry commits.
		    bool explosion_pending;
		    // Beta EntityCreeper's "powered" DataWatcher byte 17, set permanently
		    // (never cleared) by EntityCreeper.onStruckByLightning [740c583]. Doubles
		    // createExplosion's power (CREEPER_EXPLOSION_POWER_CHARGED); persisted
		    // (entity_archive.c "powered", Beta's own tag name) and synced to the
		    // client (CRPC_SPAWN_MONSTER) for the render tint. Unused by any other
		    // monster kind.
		    bool charged;
		    int attack_time; // Beta attackTime: swing cooldown, 0 = can swing
		    int attack_windup; // Wii-playability: telegraph ticks left before a swing
			struct mob_windup_cue windup_cue; /* presentation only, not archived */
		    int fire_time; // Beta fire field: ticks of burning left (sun-burn DoT)
			/* CLIENT-ONLY edge detector for the hurt-flail below. Beta slams
			 * legYaw when the hurt PACKET arrives (handleHealthUpdate case 2),
			 * a one-shot; our `hurt` flag is a level, so the rising edge is the
			 * faithful analogue. Never simulated, never archived. */
			bool hurt_prev;
		    int hurt_time; // Beta hurtTime: persisted red-flash / hurt animation timer
		    int hurt_resist_time; // effective half of Beta heartsLife: incoming damage
		                          // arbitration is active while this is > 0
		    int last_damage; // Beta field_9346_af: strongest incoming hit in the
		                     // current non-persisted resistance window
		    int knockback_time; // ticks of player-knockback slide left: suppresses the
		                        // chase steer + speed cap so the shove carries
		    /* (knockback_extra_vy, the EXPLOSION-KB bridge for the deferred
		     * knockBack pop, is retired by mob motion units S2: knockBack is
		     * now composed AT THE HIT on the authoritative doubles, before a
		     * blast adds its impulse -- Explosion.doExplosionA:100-103 order
		     * by construction.) */
		    // PIG ZOMBIE anger [740c583: EntityPigZombie]. angerLevel is set to
		    // 400+rand(400) by becomeAngryAt and NEVER decremented anywhere in
		    // b1.7.3 -- once angry, angry until death. It gates target
		    // acquisition (findPlayerToAttack returns null at 0) and persists
		    // as the "Anger" short. anger_sound_delay = randomSoundDelay
		    // (rand(40) at becomeAngryAt; zpigangry fires when it hits 0;
		    // runtime-only, not persisted -- Beta doesn't either).
		    int anger_level;
		    int anger_sound_delay;
		    // CLIENT mirror of `anger_level > 0` (Pig Zombie only), carried by
		    // CRPC_ENTITY_MOVE. Presentation/assist data only: Beta's client
		    // never learns pigman anger, but the opt-in soft-snap attack assist
		    // (docs/deviations.md "Soft-snap melee assist") must include an
		    // angry pigman and exclude a calm one. Never read by simulation.
		    bool pig_zombie_angry;
		    int aggro_time; // TARGET LATCH (Beta entityToAttack persistence, audit #14):
		                    // ticks of retained aggro on the player. 0 = no target (wander).
		                    // (Re)armed to MOB_TARGET_RETENTION_TICKS on a fresh acquire OR
		                    // on retaliation; refreshed each tick the mob still sees the
		                    // player; decremented otherwise. While > 0 (and in leash) the
		                    // mob CHASES through brief LOS flicker -- swings only with can_see.
		                    // WHICH player is `target_actor` immediately below --
		                    // no longer implicitly s->player (S1-a2).
		    /* S1-a2: WHICH actor aggro_time is retained ON. Beta keeps a live
		     * entityToAttack / playerToAttack reference [740c583 EntityCreature,
		     * EntityMob.attackEntityFrom]; this is its lifetime-safe value form
		     * (see struct actor_ident). Zero == the host, so every pre-existing
		     * spawn/load path keeps the single-player behaviour byte-for-byte.
		     * Written ONLY by the retaliation/aggro sites (the acting actor) and
		     * re-resolved on every AI tick; a failed resolve (guest left, died,
		     * rejoined under a new generation) resets it to the host AND drops
		     * aggro_time to 0, i.e. back to "no target / wander". NOT archived:
		     * a saved world has no live guest session, so the identity would be
		     * meaningless on load -- entity_archive keeps aggro_time only, which
		     * loads against the host default exactly as before. */
		    struct actor_ident target_actor;
		    float limb_swing; // walk-anim amplitude (Beta limbSwingAmount): lerps to
		                      // 0 when stopped so legs settle instead of freezing
		    // --- S2 sound state (runtime-only, none of it archived -- Beta
		    // persists none of these either) ---
		    float walk_dist;    // Beta Entity.distanceWalkedModified for the MOB
		                        // footstep odometer (limb_swing is client-only);
		                        // shares server_player_walk_step's exact cadence
		    int walk_next_step; // Beta Entity.nextStepDistance (starts at 1)
		    int living_sound_time; // Beta EntityLiving.livingSoundTime: the idle-
		                        // voice ramp counter (rand(1000) < t++ -> play,
		                        // reset to -getTalkInterval()). Starts at 0.
		    bool was_in_water;  // Beta Entity.inWater: previous-tick wet state for
		                        // the random.splash entry edge. Starts TRUE (the
		                        // isFirstUpdate guard: spawning/restoring INTO
		                        // water never splashes).
		    float fall_distance; // Beta Entity.fallDistance: accumulated downward
		                         // distance (blocks) while airborne; on landing,
		                         // ceil(fall_distance-3) mob-points of fall damage,
		                         // then reset. Zeroed in water / on a climbing spider.
		    // next-gaps-0924 W1B-b: Beta's Entity.onGround / isCollidedHorizontally as
		    // the PREVIOUS moveEntity left them. The tick's on_ground is cleared every
		    // tick by the movement tail (landing-edge invariant), so moveEntityWithHeading's
		    // pre-move reads (ground friction var8, the 0.1F*var9 vs 0.02F push select,
		    // EntitySpider.isOnLadder == isCollidedHorizontally) need their own copy:
		    // written right after the axis-1 / XZ moves, read before the next tick's move.
		    bool prev_on_ground;
		    bool prev_collided_xz;
		    int air;             // Beta Entity.air: drown timer (MOB_AIR_MAX=300 full
		                         // breath); drains while the head cell is water, deals
		                         // MOB_DROWN_DAMAGE when it hits the threshold (audit #15)
		    int entity_age;      // Beta EntityLiving.entityAge: ticks alive, +1/tick.
		                         // Drives natural despawn (audit #16): once > 600 a
		                         // 1/800-per-tick roll despawns the mob unless within 32
		                         // blocks (then it resets to 0). Reset on taking damage.
		    int death_time;      // Beta EntityLiving.deathTime (audit #18 item 3): 0 while
		                         // alive; once health<=0 it counts 0,1,2,... and the mob is
		                         // removed when it exceeds MOB_DEATH_TIME_MAX (20).
		                         // renderer flops the model over the 20-tick window.
		    bool death_loot_dropped; // onDeath ran. Separate from death_time because a
		                         // fall/cactus kill occurs after that tick's deathTime
		                         // increment phase while still dropping loot immediately.
		    bool hit_by_skeleton_arrow; // KILLING-BLOW attribution (M2): true only
		                         // while the most recent damage came from a
		                         // skeleton-owned arrow; every other damage site
		                         // clears it. Read on the death edge: a creeper whose
		                         // killing blow was a skeleton arrow drops a music
		                         // record (Beta EntityCreeper.onDeath).
		    // --- M2 final units: per-id AI state (slime hop / squid swim) ---
		    int jump_delay;      // SLIME Beta slimeJumpDelay: grounded ticks until
		                         // the next hop (rand(20)+10, /3 with a player near).
		                         // 0 on spawn -> first grounded tick hops (Beta rolls
		                         // in the constructor; the roll is deferred here,
		                         // a documented one-hop-early difference).
		    float swim_vec[3];   // SQUID randomMotionVec: the unit-ish swim heading
		                         // re-rolled by the 1-in-50 gate (Beta). Zero on
		                         // spawn -> the vec-zero clause forces a first roll.
		    float swim_speed;    // SQUID randomMotionSpeed: pulse-driven (1.0 on the
		                         // contraction kick, *0.9 decay on the glide half)
		    float squid_phase;   // SQUID squidRotation: tentacle/propulsion phase,
		                         // advances by squid_phase_vel per tick, wraps at 2PI
		    // GHAST flight/attack state [740c583: EntityGhast]. waypoint =
		    // waypointX/Y/Z (re-rolled +-16 when dist <1 or >60);
		    // ghast_course_cooldown = courseChangeCooldown (post-decrement,
		    // re-armed rand(5)+2); ghast_attack_counter = attackCounter
		    // (-40..20: charge sound at 10, fireball at 20 then reset -40);
		    // ghast_attacking mirrors the dataWatcher bit (counter > 10) for
		    // the client texture swap (server sets, client reads).
		    float ghast_waypoint[3];
		    int ghast_course_cooldown;
		    int ghast_attack_counter;
		    bool ghast_attacking;
		    float squid_phase_vel; // SQUID rotationVelocity: Beta 1/(rand+1)*0.2 in
		                         // (0.1, 0.2]; factory seeds the 0.15 midpoint (a
		                         // retained deferral) -- re-rolled 1-in-10 per wrap
			// FH-13: Beta 1.7.3 A* path state [740c583: EntityCreature.pathToEntity].
			// `path.length == 0` is this port's "pathToEntity == null" sentinel
			// (path_entity_clear sets it; see pathfinding.h). Populated by
			// pathfinder_create_path_to_entity/xyz (chase acquisition / wander
			// roll) in entity_monster.c's server_tick_monster, consumed waypoint-
			// by-waypoint by the same function every tick. NOT persisted/networked
			// (Beta doesn't either -- pathToEntity is transient, re-derived from
			// scratch on the next acquisition after a save/load or a despawn/
			// respawn, exactly like the wolf/pig-zombie runtime-only fields above).
			struct path_entity path;

			// T1 PERF (server_tick_monster's trailing CRPC_ENTITY_MOVE
			// change-gate, entity_monster.c): a cached copy of the payload
			// last actually SENT to the client for THIS mob, plus how many
			// ticks it has been since that last send. Pure engine bookkeeping
			// -- NOT Beta state, NOT itself networked, and does not
			// participate in any simulated value -- so a mob whose networked
			// state is bit-for-bit unchanged from last tick can skip the
			// send without the client ever seeing a difference. Mirrors
			// every field of client_rpc's entity_move payload that this
			// mob's trailing send site actually varies (entity_id never
			// changes for a live entity; death_time is always 0 at that
			// call site -- the death-flop send is a separate, untouched
			// site). last_sent_move_valid is set false by the monster constructor (the allocation is NOT zeroed), so a
			// freshly spawned mob's first tick always sends -- see the .c
			// site for the full T1 citation, the bounded 20-tick fallback
			// resend, and why: an unreachable-without-source/network/ event
			// (a client reconnect or a world-load resync, which resends a
			// fresh CRPC_SPAWN_MONSTER over in server_local.c) could carry
			// state this cache doesn't know changed underneath it.
			struct monster_move_snapshot {
				struct mob_windup_cue windup_cue;
				vec3 pos;
				bool on_fire;
				bool swelling;
				bool hurt;
				int8_t fuse;
				bool sheared;
				bool saddled;
				uint8_t fleece_color;
				bool ghast_attacking;
				uint8_t wolf_flags;
				int32_t wolf_health;
				bool wolf_shake_armed;
				bool wolf_shake_active;
				float wolf_shake_time;
				float wolf_prev_shake_time;
				float wolf_head_rotation;
				float wolf_head_rotation_old;
				bool charged;
				bool pig_ridden;
				uint32_t pig_rider_id;
				bool pig_zombie_angry;
			} last_sent_move;
			bool last_sent_move_valid;
			uint8_t ticks_since_move_send;
			/* Mob motion units S1 (docs/intent/mob-motion-units.md): the
			 * authoritative double position/motion/boundingBox state (Astra
			 * ruling Q1 (a)); e->pos/e->vel are its float mirrors. Inactive
			 * until S2 (MOB_MOTION_ACTIVE). Written only through
			 * mob_motion.h's mutation API. */
			struct mob_motion motion_d;
		} monster;
        /* Beta EntityMinecart.getSizeInventory() / TileEntityChest.getSizeInventory()
         * both return 27 [740c583]. Chest-cart-only; type 0/2 carts carry a
         * permanently-empty array (Beta allocates cargoItems for every type too --
         * only interact()'s displayGUIChest ever populates it). */
        #define MINECART_CHEST_SLOTS 27
        struct entity_minecart {
			float speed;
			uint8_t rail_meta; // metadata of rail underneath
			bool occupied;
			uint32_t occupant_id;
			/* G8-VEHICLE-SEAMS-01 (S1): WHICH actor lifetime rides, beside the
			 * wire id, as boat rider_actor / pig pig_rider_actor. ACTOR_IDENT_HOST
			 * (zero) when empty or host-ridden. Runtime-only, never persisted. */
			struct actor_ident rider_actor;
			/* Beta minecartType: 0 rideable / 1 chest / 2 furnace
			 * [740c583: EntityMinecart.minecartType, ctor(World,d,d,d,int)].
			 * Immutable after spawn; installed only by CRPC_SPAWN_MINECART. */
			uint8_t minecart_type;
			/* Beta pushX/pushZ: furnace-cart (type 2) self-propulsion direction,
			 * unridden only [740c583: EntityMinecart.pushX/pushZ]. World-axis unit-
			 * ish vector, NOT the rail-axis (hx,hz). SERVER-ONLY: never networked
			 * (no client-visible effect beyond the position/speed it produces,
			 * which already rides the ordinary minecart snapshot -- see
			 * MINECART_SLICE_NOTES.md). Beta uses double; float per the entity
			 * motion convention (CODESTYLE.md S2.4). */
			float push_x, push_z;
			/* Beta fuel: furnace-cart burn ticks remaining [740c583:
			 * EntityMinecart.fuel]. +=1200 per coal fed (interact()), uncapped
			 * upward in Beta's own runtime int; -1 with a 1-in-4 chance per tick
			 * while actively pushing (onUpdate). SERVER-ONLY, never networked. */
			int32_t fuel;
			/* Beta cargoItems: chest-cart (type 1) storage, MINECART_CHEST_SLOTS
			 * wide [740c583: EntityMinecart.cargoItems]. Populated only via the
			 * entity-backed chest GUI (inventory_minecart_chest.c); every other
			 * type's array stays all-zero. */
			struct item_data inventory[MINECART_CHEST_SLOTS];
			/* Cumulative rider-input protocol. On a CLIENT cart, control_* is
			 * the locally authored tuple and control_acked_* is the latest tuple
			 * echoed by the authority. On a SERVER cart, control_* is the latest
			 * accepted tuple and the ack mirror is unused. Every nonzero step
			 * increments control_seq and exactly one directional counter. */
			uint32_t ride_epoch;
			uint32_t control_seq;
			uint32_t control_positive_steps;
			uint32_t control_negative_steps;
			uint32_t control_acked_seq;
			uint32_t control_acked_positive_steps;
			uint32_t control_acked_negative_steps;
			bool control_ready; /* client has authoritative epoch ownership */
			bool control_dirty; /* authored tuple is not fully acknowledged */
			/* Signed step count consumed (applied then zeroed) at the one
			 * deterministic throttle point in minecart_server_tick. int32 keeps
			 * a recovered cumulative gap representable without signed overflow. */
			int32_t pending_throttle;
			/* Client-only correction suffix. Kept separate from current-tick
			 * prediction so dictionary iteration order cannot decide whether a
			 * correction and newly sampled input collapse into one physics tick. */
			int32_t replay_throttle;
		    int8_t hx, hz;         // cart-heading in XZ (unit grid: -1,0,+1)
		    uint8_t last_meta;
			/* Server-only bounded correction scheduler state. Regular physics
			 * and external impulses mark dirty; mount/reject/epoch transitions
			 * additionally mark urgent. Stationary carts leave the background
			 * rotation only after a successful final snapshot. */
			uint64_t snapshot_last_tick;
			bool snapshot_dirty;
			bool snapshot_urgent;
			/* VEHICLE BREAKING [740c583 EntityMinecart.attackEntityFrom:70-85,
			 * onUpdate:155-161]: shake=10 per hit (render rock), damage +=
			 * strength*10, BOTH decay 1/tick, damage>40 -> dismount+die+drop.
			 * Beta persists NONE of these (no NBT tags) -- never archived. */
			int16_t damage_taken;
			int8_t shake_ticks;
			int8_t shake_dir;
			/* Explosion entity pass latch (server_world.c): the blast's
			 * attackEntityFrom pushed damage_taken past 40 while the entity
			 * dict was being iterated; the cart's own next server tick runs
			 * the Beta dismount+drop break and returns "remove". Never
			 * archived (a saved cart has no pending blast). */
			bool blast_break_pending;
			/* CLIENT-ONLY rolling-sound odometer (non-Beta addition; see
			 * audio_sound_table.h). Never archived, never simulated: it only
			 * paces a presentational sound. */
			float roll_dist;
			/* CLIENT-ONLY "inside" seat-loop retrigger countdown (same
			 * non-Beta add-on family as roll_dist above; see
			 * audio_sound_table.h / minecart_inside_sound). Ticks down to 0,
			 * then fires and resets -- never archived, never simulated. */
			float inside_timer;

			struct item_data item;   // houdt id, count, durability

        } minecart;
		struct entity_arrow {
			uint32_t owner_id; // entity that fired it; excluded from the sweep only
			                   // during the owner-grace window (ARROW_OWNER_GRACE_TICKS)
			/* S1-a2 PROJECTILE ATTRIBUTION. Beta keeps a live
			 * EntityArrow.shootingEntity reference [740c583 EntityArrow.<init>;
			 * doesArrowBelongToPlayer; onUpdate's `var9 != shootingEntity ||
			 * ticksInAir >= 5` self-exclusion]. owner_id ALONE cannot say WHICH
			 * local actor fired: both actors' bodies are the one client-side
			 * local-player id, so a guest shot used to read as the host's (and
			 * would have been absorbed by the HOST's owner grace while being
			 * able to damage the host). This is the lifetime-safe value form of
			 * that reference, stamped at FIRE time from the acting actor.
			 * Zero == the host, so a host-fired arrow is byte-identical.
			 * Runtime-only (arrows are not archived with an owner identity). */
			struct actor_ident shooter_actor;
			int age;           // Beta ticksInAir; no age-based arrow despawn
			uint16_t item_id;  // render model item (ITEM_ARROW)
			bool fireball;     // client-only spawn kind; snowballs share ITEM_SNOW_BALL
			int16_t damage;    // player-scale damage on a PLAYER hit (ARROW_DAMAGE); a MOB
			                   // hit uses the mob-scale ARROW_DAMAGE_BOW_BASE instead
			bool from_player;  // doesArrowBelongToPlayer: true = player-fired (hits MOBS,
			                   // pickable when stuck); false = skeleton (hits the player +
			                   // other mobs via infighting, NOT pickable)
			bool from_skeleton; // shooter resolved AT FIRE TIME (Beta keeps a live
			                   // shooter reference; a dead-by-impact skeleton must
			                   // still earn the creeper record drop). Stamped by
			                   // server_local_flush_arrows from the pending queue.
			// --- audit #17d: stick / pickup / in-ground despawn ---
			bool in_ground;    // Beta inGround: stuck in a block -> frozen + pickable
			int stuck_ticks;   // Beta ticksInGround: in-ground age (despawn at 1200)
			w_coord_t stuck_block_x, stuck_block_y, stuck_block_z;
			uint8_t stuck_block_type, stuck_block_meta;
			int shake_ticks;   // Beta arrowShake: counts down from 7 on stick; pickup
			                   // is blocked until it reaches 0 (~7 ticks)
			bool was_in_water; // S2 splash latch (EntityArrow calls
			                   // super.onUpdate() -> Entity splash edge; a
			                   // shot into water audibly splashes at flight
			                   // speed). Starts TRUE. The in_ground early-out
			                   // skips the probe: an embedded arrow's motion
			                   // is zero, so Beta's velocity-derived volume
			                   // would be inaudible anyway. Runtime-only.
			// --- MP client-side embed observation (remote_session only). Beta
			// MP clients run a full self-simulating EntityArrow whose own
			// onUpdate plays random.drr on hits [740c583 NetClientHandler.
			// handleVehicleSpawn type 60 -> new EntityArrow; EntityArrow.
			// onUpdate]; this engine's remote arrows are wire-position-driven,
			// so the embed is OBSERVED instead: flight = per-tick deltas above
			// epsilon, embed = deltas going still after having moved
			// (arrow_client_settled, combat.h). On client copies in_ground
			// doubles as the played-once latch (unused there otherwise).
			bool client_moved;          // saw at least one real network delta
			uint8_t client_still_ticks; // consecutive still ticks since then
			struct arrow_visual_state visual;
			bool visual_pending; /* client: apply a new sample before countdown */
		} arrow;
		struct entity_egg {
			uint32_t owner_id; // thrower (0 = the local player); excluded from the
			                   // hit sweep only while age < ARROW_OWNER_GRACE_TICKS
			                   // (Beta `var9 != thrower || ticksinAir >= 5`)
			/* S1-a2: WHICH actor threw it -- the same lifetime-safe form and
			 * the same reason as entity_arrow.shooter_actor above. Beta:
			 * EntityThrowable's `owner` live reference [740c583 EntityEgg /
			 * EntitySnowball extend EntityThrowable]. Zero == the host. */
			struct actor_ident thrower_actor;
			int age;           // Beta ticksinAir; also drives the same in-air
			                   // miss-despawn stand-in cap the arrow documents
		} egg; // shared by ENTITY_EGG and ENTITY_SNOWBALL (identical field pair)
		struct entity_fireball {
			uint32_t owner_id; // the shooting ghast (thrower-immunity while
			                   // age < 25 [740c583: EntityFireball ticksInAir])
			int age;           // ticksInAir + the arrow-style despawn cap
			float accel[3];    // accelerationX/Y/Z: dir/|dir| * 0.1, added to
			                   // the motion every tick before the 0.95 drag.
			                   // NOT persisted (Beta loses it on reload too).
		} fireball;
		struct entity_primed_tnt {
			// Beta starts at 80. The post-decrement update leaves a LIVE fuse
			// value 0 for one tick, then detonates on the following update.
			int fuse;
			/* Distinguishes that ordinary live fuse-zero tick from a final step
			 * already attempted and rejected by the Wii explosion cap. Only the
			 * latter belongs in the immutable pre-RPC retry pass. */
			bool explosion_pending;
			uint8_t correction_ticks; // server tracker cadence; client leaves at 0
			bool awaiting_destroy; // client expired locally; server DESTROY is authoritative
		} primed_tnt;
		/* Beta EntityFish [740c583]. The "angler" is the actor named by
		 * angler_actor below (S1-a2; it used to be assumed to be the single
		 * local player, id 0 by this engine's entity-id convention, which a
		 * local split guest falsified). See entity_fish.h for the pure helpers
		 * (fish_catch_cost, fish_pull_toward_angler, fish_bite_chance, ...)
		 * that mirror EntityFish.catchFish()/onUpdate's bite branch. */
		struct entity_fish {
			/* S1-a2 ANGLER IDENTITY. Beta's EntityFishHook.angler is a live
			 * EntityPlayer reference [740c583 EntityFishHook.<init>(World,
			 * EntityPlayer); onUpdate's `angler.fishEntity != this` self-removal;
			 * catchFish's pull toward `angler`]. The comment this replaces said
			 * "the angler is always the single local player" -- with a local
			 * split guest that is no longer true, and the REEL writes the
			 * angler's own vel_x/vel_z, so an unattributed hook would yank the
			 * wrong player. Zero == the host (byte-identical single-player
			 * behaviour). A hook whose angler stops resolving (leave, death,
			 * rejoin) is REMOVED, which is exactly Beta's `angler.isDead ->
			 * setEntityDead()` leg. */
			struct actor_ident angler_actor;
			bool bobber_hooked; // Beta `this.bobber != null`
			uint32_t bobber_id; // valid iff bobber_hooked; 0 = a local player
			/* S1-a3: WHICH local player body bobber_id == 0 names. Both local
			 * bodies share SERVER_LOCAL_PLAYER_ENTITY_ID, so once the hook
			 * sweep became two-bodied the id alone could no longer say whether
			 * the snagged player is the angler (Beta self-hook) or the OTHER
			 * player -- and the reel pulls the HOOKED entity toward the angler
			 * [740c583 EntityFishHook.catchFish]. Zero value = host, so every
			 * pre-existing initializer keeps the single-player meaning. */
			struct actor_ident bobber_actor;
			                    // (self-hook, Beta allows it -- the angler is a
			                    // valid sweep candidate once past the grace
			                    // window), else a dict_entity id (a hooked mob)
			bool in_ground;     // Beta inGround
			int32_t ticks_in_ground; // Beta ticksInGround (despawns at 1200).
			                    // Beta's xTile/yTile/zTile/inTile "already
			                    // unstuck?" recheck is UNREACHABLE for a
			                    // freshly-cast (non-NBT-loaded) hook --
			                    // getBlockId(-1,-1,-1) (the never-written
			                    // constructor defaults) is 0 by World's own Y<0
			                    // bounds check [740c583: World.getBlockId],
			                    // which always equals inTile's own 0 default,
			                    // so the "still the same block?" compare is
			                    // provably always true and the unstick branch
			                    // never runs. This engine does not persist the
			                    // hook (see ENTITY_FISH's enum comment), so
			                    // that reachability can never change here --
			                    // the reduced form (ticksInGround simply counts
			                    // to 1200) is the faithful behavior for every
			                    // reachable case, not a shortcut.
			int32_t ticks_in_air; // Beta ticksInAir: owner-grace age for the
			                    // entity/player sweep (arrow_should_hit reuse)
			int32_t ticks_catchable; // Beta ticksCatchable: >0 while a fish is
			                    // biting; counts down to 0 once hooked
			/* W2-B1 (BLOCKS lane hunk): Beta Entity.distanceWalkedModified /
			 * nextStepDistance (init 1) -- EntityFish keeps canTriggerWalking()
			 * true, so its moveEntity runs the footstep odometer. Runtime-only
			 * (Beta persists neither). [740c583: Entity.java:40-42,437-458] */
			float walk_dist;
			int32_t walk_next_step;
		} fish; // Beta's `shake` field decrements every tick but nothing in
		        // EntityFish.java EVER sets it above 0 (verified: the only
		        // writes are the two constructors' `= 0` and the NBT
		        // reader/writer) -- it is genuinely inert for a live session
		        // in this engine (persistence is deliberately not ported; see
		        // ENTITY_FISH), so it is NOT carried here. Documented, not
		        // silently dropped: a field with zero possible observable
		        // effect is not "faithfulness", it is dead weight.
		struct entity_boat {
			// Beta boatCurrentDamage/boatTimeSinceHit/boatRockDirection
			// [740c583: EntityBoat]. IDENTICAL shape to entity_minecart's
			// damage_taken/shake_ticks/shake_dir -- both vehicle classes share
			// this exact Beta pattern (attackEntityFrom, the 1/tick onUpdate
			// decay, RenderBoat's hit-shake formula matches RenderMinecart's).
			int16_t damage_taken;
			int8_t shake_ticks;
			int8_t shake_dir; // starts at 1, flips sign on every hit
			/* Same explosion-pass latch as entity_minecart.blast_break_pending. */
			bool blast_break_pending;
			bool occupied;
			// Runtime id of the current rider. CaveWii models exactly one
			// possible occupant (the local player, fixed sentinel id 0 --
			// SERVER_LOCAL_PLAYER_ENTITY_ID), so this is never remapped on
			// chunk-entity restore and no separate persisted "source id" is
			// needed: `occupied` alone determines it on load.
			uint32_t occupant_id;
			/* S1-a2: WHICH actor occupant_id names. Beta's riddenByEntity is a
			 * live reference [740c583 EntityBoat.interact / attackEntityFrom's
			 * forced dismount]; the fixed SERVER_LOCAL_PLAYER_ENTITY_ID sentinel
			 * cannot tell two local actors apart. Zero == the host, so the
			 * existing single-rider boat is byte-identical. Meaningful only
			 * while `occupied`; a rider whose identity stops resolving is
			 * DISMOUNTED by the boat's own server tick. Runtime-only: the
			 * archive restores `occupied` alone, which loads as the host. */
			struct actor_ident rider_actor;
			// Server tracker cadence (mirrors entity_primed_tnt's
			// correction_ticks / Beta's EntityTrackerEntry 10-tick interval):
			// the boat's physics are shared+deterministic between client and
			// server, so only periodic drift correction is needed, not a
			// full authoritative snapshot every tick.
			uint8_t correction_ticks;
			// Client predicted a land-crash destroy locally; waits for the
			// server's authoritative CRPC_ENTITY_DESTROY before actually
			// removing itself (entity_primed_tnt.c's awaiting_destroy
			// precedent -- the client must never unilaterally free a
			// server-owned entity).
			bool awaiting_destroy;
			// Beta Entity.beenAttacked, set by attackEntityFrom's
			// setBeenAttacked() step [740c583: EntityBoat.attackEntityFrom,
			// Entity.setBeenAttacked]. Write-only in Beta itself (zero readers
			// anywhere in the 1.7.3-LTS client tree) -- kept only for
			// verbatim-order transcription fidelity, server-side only.
			bool been_attacked;
			// Rider steering coupling SOURCE (docs/deviations.md "Boat rider
			// steering"): the current rider's own horizontal input velocity
			// -- Beta's riddenByEntity.motionX/Z [740c583: EntityBoat.
			// onUpdate]. Populated per-tick by the CALLER, never by
			// boat_advance itself: the CLIENT's entity_local_player.c BOAT
			// MOUNT branch writes it directly (local prediction) and the
			// SERVER's boat_tick_server copies it from struct server_player.
			// rider_vel_x/z (itself fed by the client-reported SRPC_
			// PLAYER_POS vel_x/z). 0.0 whenever unoccupied -- boat_advance's
			// coupling is gated on `occupied` too, so a stale value here is
			// inert, never read.
			float rider_vel_x, rider_vel_z;
		} boat;
		// MP S3->player-render follow-up: ENTITY_REMOTE_PLAYER's data. render
		// is now render_entity_player (entity_remote_player.c), the shared
		// ModelBiped geometry from render_entity.c textured with
		// texture_mob_char; held_item mirrors the LAST CRPC_SPAWN_PLAYER/
		// CRPC_ENTITY_EQUIPMENT slot-0 (held) update. [740c583
		// Packet20NamedEntitySpawn] [740c583 Packet5PlayerInventory]
		struct entity_remote_player {
			// Packet1Login's 16-code-unit login-name maximum + NUL (matches
			// mp_codec.h's MP_NAMED_ENTITY_NAME_MAX -- not included here to
			// avoid a network-layer dependency in this low-level header).
			char username[17];
			struct item_data held_item;
			bool has_held_item;
			// Render-follow-up animation/orientation state, the
			// entity_monster.c body_yaw/head_yaw/frame precedent applied to a
			// remote player. UNLIKE a monster, head yaw/pitch is REAL
			// networked look (e->orient, ENGINE-frame degrees -- Beta wire
			// angles are converted at the CRPC boundary, yaw mirrored /
			// pitch +90, the exact inverse of the local-player send path;
			// see server_remote.c's CRPC_SPAWN_PLAYER assembly note), not a
			// movement-direction approximation, so
			// only body_yaw (radians, lags e->orient[0] via
			// entity_blend_body_to_head) is stored here. frame/frame_time_left
			// mirror entity_tick_animation's fields exactly (that helper
			// itself asserts ENTITY_MONSTER and cannot be reused directly on
			// this union member -- entity_remote_player.c's own client tick
			// inlines the identical logic). limb_swing mirrors
			// client_tick_monster's walk-amplitude ease. All zero-started by
			// the memset below (this struct, unlike entity_monster's, IS
			// memset-zeroed at construction).
			float body_yaw;
			int frame;
			int frame_time_left;
			float limb_swing;
			// MP visual-parity pass (2026-08-31), item 1/2/3 -- sneak tracking +
			// arm swing. `sneaking` is set by CRPC_ENTITY_FLAGS, which decodes
			// the REAL Beta mechanism: the generic Entity DataWatcher flag byte
			// (index 0, bit 1 == 0x02) [740c583 Entity.isSneaking ->
			// getEntityFlag(1) -> dataWatcher.getWatchableObjectByte(0) &
			// (1<<1)], synced via Packet40 EntityMetadata whenever a real
			// server applies a client's Packet19EntityAction. Packet18Animation
			// animate==4 (crouch) is NOT this signal -- EntityPlayer.
			// func_6420_o(), the handler for that code, is a literally EMPTY
			// method on both EntityOtherPlayerMP and EntityPlayerSP [740c583]:
			// a genuine Beta no-op, confirmed by decompile before assuming it
			// needed wiring.
			bool sneaking;
			// EntityLiving.swingProgress/swingProgressInt/isSwinging state
			// machine [740c583 EntityPlayer.updatePlayerActionState /
			// .swingItem]: swingItem() (driven by CRPC_ENTITY_ANIMATION
			// animate==1) sets swing_progress_int=-1, is_swinging=true; every
			// tick thereafter (entity_remote_player_client_tick) increments
			// swing_progress_int while is_swinging, resetting both at 8, and
			// rederives swing_progress = swing_progress_int/8.0F -- an exact
			// mirror of EntityPlayer.updatePlayerActionState's own body.
			// prev_swing_progress is the previous tick's value, feeding
			// EntityLiving.getSwingProgress's own render-time lerp
			// (prevSwingProgress + (swingProgress-prevSwingProgress)*partial).
			int swing_progress_int;
			bool is_swinging;
			float swing_progress;
			float prev_swing_progress;
			// Animation-parity pass (2026-09-03) item 2: Beta deathTime, mirrored
			// from the SAME server_remote flush_entity_status wall-clock pump
			// that already drives a dying MOB's flop (server_remote_protocol.c's
			// MP_EVENT_ENTITY_STATUS status==3 case, now armed for
			// REMOTE_ENTITY_PLAYER too). 0 while alive; 1..20 while falling over
			// [740c583 EntityLiving.onEntityUpdate deathTime>=20 cap,
			// RenderLiving.rotateCorpse]. Rendered via the SAME
			// mob_death_roll_deg helper entity_monster.c's dying mobs use
			// (combat.h) -- one shared, decompile-cited formula, two callers.
			int death_time;
			// Animation-parity pass item 4: Beta EntityLiving/Entity.isRiding()
			// -- for any entity OTHER than the receiving client's own player,
			// the ONLY wire signal is the generic DataWatcher flag byte bit 2
			// (0x04) [740c583 Entity.setEntityFlag(2, ridingEntity != null) in
			// onEntityUpdate; Entity.isRiding() -> getEntityFlag(2)].
			// Packet39AttachEntity is NOT this signal for an observer: the only
			// server call site (EntityPlayerMP.onUpdate) sends it exclusively
			// down the MOUNTING player's own connection
			// (`this.playerNetServerHandler.sendPacket(...)`), never broadcast
			// to trackers (confirmed by decompile grep: zero other
			// construction sites in the server tree). So a remote OTHER
			// player's "is riding something" pose is real Beta behaviour
			// driven by this SAME byte0 metadata sync as sneaking (bit 1) and
			// on_fire (bit 0), decoded together in server_remote_protocol.c's
			// MP_EVENT_ENTITY_METADATA player branch. There is deliberately NO
			// vehicle-follow here: Beta itself gives an observer no wire
			// reference to WHAT the remote player is riding, only that they
			// are -- ModelBiped's isRiding pose (bent legs/arms) is applied
			// with the player's own already-networked position, exactly
			// matching what a real Beta 1.7.3 client would show.
			bool is_riding;
			// Animation-parity pass item 5: Packet5PlayerInventory slots 1..4
			// (armor), previously discarded client-side ("armor slots 1..4:
			// tracked nowhere client-side yet" -- client_interface.c). Indexed
			// by enum armor_type (item_types.h): HELMET=0, CHESTPLATE=1,
			// LEGGINGS=2, BOOTS=3. 0 == empty slot (ITEM id 0 is Beta's own
			// "no item" sentinel throughout this codebase, the held_item
			// precedent above). [740c583 EntityPlayerMP.getEquipmentInSlot:
			// wire slot N (1..4) -> inventory.armorInventory[N-1], and
			// RenderPlayer.setArmorModel's armorItemInSlot(3-var2) confirms
			// armorInventory index 0=boots,1=leggings,2=chestplate,3=helmet --
			// i.e. wire slot 1=boots, 2=leggings, 3=chestplate, 4=helmet].
			uint16_t armor_id[4];
			/* MP-BED: EntityPlayer.sleepInBedAt state for a remote player
			 * (Packet17, NetClientHandler.java:370-378), cleared by its
			 * Packet18 animate 3 wakeUpPlayer (NetClientHandler.java:359-361).
			 * bed_orient_deg = getBedOrientationInDegrees (EntityPlayer.java:
			 * 663-680); sleep_off_x/z = func_22052_e's field_22063_x/
			 * field_22061_z render offsets (EntityPlayer.java:594-611). */
			bool sleeping;
			float bed_orient_deg;
			float sleep_off_x, sleep_off_z;
			/* MP-RIDE: Entity.ridingEntity for a remote player (Packet39,
			 * NetClientHandler.java:439-449): the client entity id of the
			 * vehicle while riding_vehicle is true. */
			bool riding_vehicle;
			uint32_t riding_vehicle_id;
		} remote_player;
		/* Beta EntityPainting fields [740c583]: xPosition/yPosition/zPosition
		 * (the wall block the frame hangs on, NOT e->pos -- e->pos is the
		 * derived logical center painting_compute_geometry computes),
		 * direction (0..3, see entity_painting.h painting_direction), and
		 * which EnumArt entry (by table index rather than a Java enum ref).
		 * tick_counter mirrors Beta's private tickCounter1 (onUpdate's
		 * every-100th-tick onValidSurface recheck). */
		struct entity_painting {
			int art_index;
			int direction;
			int tile_x, tile_y, tile_z;
			int tick_counter;
		} painting;
		/* Beta EntityFallingSand [740c583]. blockID is the carried block
		 * (BLOCK_SAND or BLOCK_GRAVEL); fallTime counts ticks airborne and
		 * gates the 100-tick in-air give-up (drop as an item, self-destroy --
		 * the "fell into the void forever" case). awaiting_destroy mirrors
		 * entity_primed_tnt.c's field of the same name: the CLIENT'S locally
		 * predicted copy sets it once its own physics reaches on_ground, then
		 * waits for the server's authoritative CRPC_ENTITY_DESTROY (plus the
		 * CRPC_SET_BLOCK for the settled block) instead of unilaterally
		 * removing a server-owned entity. */
		struct entity_falling_sand {
			uint8_t block_id;
			int fall_time;
			bool awaiting_destroy;
		} falling_sand;
	} data;
	/* Entity.java:50 owns a java.util.Random per entity. Mob simulation uses
	 * this inline stream with Java's own nextInt(n)/nextFloat() algorithms;
	 * other entity consumers are a separate migration.
	 * Runtime-only: construction/reload starts a new lifetime, as Beta
	 * Entity.writeToNBT does not persist Random state. Client copies stay
	 * zeroed (entity_default_init). */
	struct java_random gameplay_rng;
};



void make_creeper_bbox(struct AABB* out);

// CROSS-SLICE harden audit (#1): a single "is this mob a live, hittable target?"
// predicate, shared by every system that picks a mob to damage / lock onto. A mob is
// targetable ONLY while genuinely alive: a real monster (id stays through the
// deathTime flop, so type alone isn't enough), with health > 0, and NOT yet in its
// audit-#18 death animation (death_time == 0). During the ~20-tick deathTime flop the
// corpse keeps its collision/raycast box, so an arrow sweep or aim-assist that didn't
// check this would let a CORPSE eat an arrow or win an aim-lock. Mirrors the melee
// SRPC_ENTITY_ATTACK guard (which already early-outs on death_time > 0). Pure inline,
// no RNG -- safe to call inside a live dict walk (it only reads, never removes).
static inline bool mob_is_targetable(const struct entity* e) {
	return e && e->type == ENTITY_MONSTER && e->health > 0
		&& e->data.monster.death_time == 0
		&& !e->data.monster.explosion_pending;
}

// next-gaps-0924/G8b: the red hurt overlay gate for a rendered living body.
// Beta RenderLiving.doRenderLiving (RenderLiving.java:67-75) washes the model
// red while `hurtTime > 0 || deathTime > 0` -- the whole deathTime flop, not
// just the 10-tick hurt window. e->hurt is the networked hurtTime stand-in
// (SP hurt_flash_active, MP Packet38 status 2 + 500 ms); deathTime is the
// networked death_time each entity kind already carries (the SP dying move
// sends hurt=false, so the death half is derived here, no wire change).
static inline bool entity_living_red_tint(const struct entity* e) {
	if(!e)
		return false;
	if(e->hurt)
		return true;
	switch(e->type) {
	case ENTITY_MONSTER: return e->data.monster.death_time > 0;
	case ENTITY_REMOTE_PLAYER: return e->data.remote_player.death_time > 0;
	default: return false;
	}
}

/*
	Entity insertion is a mutation preflight for TNT/explosions. M-lib's default
	dictionary reserve aborts on allocation failure and can resize data and index,
	then allocate collision scratch. This allocator makes those calls transactional
	while a reserve is armed: old buffers stay owned until every allocation succeeds; on
	failure longjmp returns to dict_entity_try_reserve, which restores the untouched
	dictionary and reports false to the caller. Outside that narrow preflight it is
	the ordinary realloc/free contract.
*/
struct entity_dict_realloc_record {
	void* old_ptr;
	void* new_ptr;
};

enum entity_dict_transaction_state {
	ENTITY_DICT_TX_IDLE,
	ENTITY_DICT_TX_PUBLISHING,
	ENTITY_DICT_TX_ARMED,
};

/* This state is header-local with the generated dictionary implementation.
 * Exact-owner admission is still required: tests and future callers may use two
 * dictionaries from different threads in the same translation unit. */
static unsigned entity_dict_realloc_transaction_state;
static uintptr_t entity_dict_realloc_transaction_owner;
static jmp_buf entity_dict_realloc_escape;
static struct entity_dict_realloc_record entity_dict_realloc_records[4];
static size_t entity_dict_realloc_record_count;
static unsigned entity_dict_realloc_fail_countdown;

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static void (*entity_dict_transaction_test_hook)(void*);
static void* entity_dict_transaction_test_hook_arg;
#endif

static inline uintptr_t entity_dict_current_thread(void) {
	uintptr_t result = 0;
#ifdef PLATFORM_PC
	pthread_t native = pthread_self();
#elif defined(PLATFORM_WII)
	lwp_t native = LWP_GetSelf();
#else
	/* CaveWii has only PC and Wii threaded builds. Keep single-threaded tools
	 * buildable without pretending to provide cross-thread ownership there. */
	uintptr_t native = 1;
#endif
	typedef char entity_dict_thread_id_must_fit_uintptr[
		(sizeof(native) <= sizeof(result)) ? 1 : -1];
	(void)sizeof(entity_dict_thread_id_must_fit_uintptr);
	memcpy(&result, &native, sizeof(native));
	return result;
}

static inline bool entity_dict_transaction_owned(void) {
	if(__atomic_load_n(&entity_dict_realloc_transaction_state, __ATOMIC_ACQUIRE)
	   != ENTITY_DICT_TX_ARMED)
		return false;
	return __atomic_load_n(
			   &entity_dict_realloc_transaction_owner, __ATOMIC_RELAXED)
		== entity_dict_current_thread();
}

static inline bool entity_dict_transaction_try_begin(void) {
	unsigned expected = ENTITY_DICT_TX_IDLE;
	if(!__atomic_compare_exchange_n(
		   &entity_dict_realloc_transaction_state, &expected,
		   ENTITY_DICT_TX_PUBLISHING, false, __ATOMIC_ACQ_REL,
		   __ATOMIC_ACQUIRE))
		return false;
	__atomic_store_n(&entity_dict_realloc_transaction_owner,
					 entity_dict_current_thread(), __ATOMIC_RELAXED);
	__atomic_store_n(&entity_dict_realloc_transaction_state,
					 ENTITY_DICT_TX_ARMED, __ATOMIC_RELEASE);
	return true;
}

static inline void entity_dict_transaction_end(void) {
	assert(entity_dict_transaction_owned());
	__atomic_store_n(&entity_dict_realloc_transaction_state,
					 ENTITY_DICT_TX_IDLE, __ATOMIC_RELEASE);
}

static inline void* entity_dict_realloc_impl(void* ptr, size_t old_size,
									 size_t new_size) {
	if(entity_dict_realloc_fail_countdown > 0
	   && --entity_dict_realloc_fail_countdown == 0)
		return NULL;
	if(!entity_dict_transaction_owned())
		return realloc(ptr, new_size);
	if(entity_dict_realloc_record_count
	   >= sizeof(entity_dict_realloc_records)
		  / sizeof(entity_dict_realloc_records[0]))
		return NULL;
	void* fresh = malloc(new_size);
	if(!fresh)
		return NULL;
	if(ptr)
		memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
	entity_dict_realloc_records[entity_dict_realloc_record_count++]
		= (struct entity_dict_realloc_record) {
			.old_ptr = ptr,
			.new_ptr = fresh,
		};
	return fresh;
}

static inline void entity_dict_free_impl(void* ptr) {
	if(entity_dict_transaction_owned()) {
		/* The resize collision scratch is allocated through this oplist so its
		 * OOM participates in the transaction. It has no borrowed old buffer;
		 * remove its record when M-lib releases it to avoid a later double free. */
		for(size_t i = 0; i < entity_dict_realloc_record_count; i++) {
			struct entity_dict_realloc_record* record
				= &entity_dict_realloc_records[i];
			if(record->old_ptr == NULL && record->new_ptr == ptr) {
				free(ptr);
				entity_dict_realloc_records[i]
					= entity_dict_realloc_records[
						--entity_dict_realloc_record_count];
				return;
			}
		}
		assert(!"dictionary transaction attempted to free a borrowed buffer");
		return;
	}
	free(ptr);
}

static inline void entity_dict_memory_full(void) {
	if(entity_dict_transaction_owned())
		longjmp(entity_dict_realloc_escape, 1);
	abort();
}

#define ENTITY_DICT_REALLOC(ctx, type, ptr, old_count, new_count)              \
	((new_count) > SIZE_MAX / sizeof(type)                                      \
		 ? NULL                                                                  \
		 : (type*)entity_dict_realloc_impl(                                      \
			   (ptr), (old_count) * sizeof(type), (new_count) * sizeof(type)))
#define ENTITY_DICT_FREE(ctx, type, ptr, count)                               \
	((void)(count), entity_dict_free_impl(ptr))
#define ENTITY_DICT_KEY_OPLIST                                                \
	M_OPEXTEND(M_BASIC_OPLIST, REALLOC(ENTITY_DICT_REALLOC),                    \
			   FREE(ENTITY_DICT_FREE))

/* M_MEMORY_FULL must not continue, so the reserve-only handler escapes to the
 * transaction boundary. Restore M-lib's default fatal policy immediately after
 * generating this one dictionary. */
#undef M_MEMORY_FULL
#define M_MEMORY_FULL(type, number)                                           \
	do {                                                                        \
		(void)sizeof(type);                                                       \
		(void)(number);                                                           \
		entity_dict_memory_full();                                                \
	} while(0)
DICT_DEF2(dict_entity, uint32_t, ENTITY_DICT_KEY_OPLIST, struct entity*,
		  M_POD_OPLIST)
#undef M_MEMORY_FULL
#define M_MEMORY_FULL(type, number)                                           \
	M_RAISE_FATAL(                                                             \
		"Cannot allocate %zu objects of type '%s' (size '%zu') at (%s:%s:%d).\n", \
		(size_t)(number), M_AS_STR(type), sizeof(type), __FILE__, __func__,       \
		__LINE__)

static inline void dict_entity_test_fail_realloc_after(unsigned occurrence) {
	entity_dict_realloc_fail_countdown = occurrence;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static inline void dict_entity_test_set_transaction_hook(
	void (*hook)(void*), void* arg) {
	entity_dict_transaction_test_hook = hook;
	entity_dict_transaction_test_hook_arg = arg;
}
#endif

static inline bool dict_entity_try_reserve(dict_entity_t dict, size_t capacity) {
	assert(dict);
	/* m-lib reserve(0) means shrink-to-fit, which frees live buffers -- outside
	 * this journal's grow-only contract (bug-hunt D10 latent reserve-0 guard).
	 * Nothing to reserve is trivially satisfied; return before arming. */
	if(capacity == 0)
		return true;
	if(!entity_dict_transaction_try_begin())
		return false;
	struct dict_entity_s saved = *dict;
	entity_dict_realloc_record_count = 0;
	if(setjmp(entity_dict_realloc_escape) == 0) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
		if(entity_dict_transaction_test_hook)
			entity_dict_transaction_test_hook(
				entity_dict_transaction_test_hook_arg);
#endif
		dict_entity_reserve(dict, capacity);
		for(size_t i = 0; i < entity_dict_realloc_record_count; i++)
			free(entity_dict_realloc_records[i].old_ptr);
		entity_dict_realloc_record_count = 0;
		entity_dict_transaction_end();
		return true;
	}
	for(size_t i = 0; i < entity_dict_realloc_record_count; i++)
		free(entity_dict_realloc_records[i].new_ptr);
	entity_dict_realloc_record_count = 0;
	*dict = saved;
	entity_dict_transaction_end();
	return false;
}

/* Reserve for future insertions, including M-lib's tombstones. `size+count`
 * alone is insufficient after erases: safe_get rehashes when count_delete
 * reaches upper_limit even when the live count is small. Force a checked growth
 * before that boundary so no later safe_get can enter the fatal OOM policy. */
static inline bool dict_entity_try_reserve_insertions(
	dict_entity_t dict, size_t count) {
	assert(dict);
	/* Zero future insertions need no guarantee; passing size+0 down would reach
	 * m-lib's shrink path (D10 reserve-0 guard -- see dict_entity_try_reserve). */
	if(count == 0)
		return true;
	size_t size = dict_entity_size(dict);
	if(count > SIZE_MAX - size)
		return false;
	size_t capacity = size + count;
	if(count > SIZE_MAX - (size_t)dict->count_delete)
		return false;
	if((size_t)dict->count_delete + count
	   >= (size_t)dict->upper_limit) {
		/* upper_limit+1 can truncate back to the current power-of-two table
		 * inside M-lib's floating reserve calculation (e.g. 11 -> 16). Asking
		 * for the current bucket count itself necessarily rounds to 2x. */
		size_t growth_capacity = (size_t)dict->mask + 1;
		if(growth_capacity == 0)
			return false;
		if(capacity < growth_capacity)
			capacity = growth_capacity;
	}
	return dict_entity_try_reserve(dict, capacity);
}

#undef ENTITY_DICT_KEY_OPLIST
#undef ENTITY_DICT_FREE
#undef ENTITY_DICT_REALLOC

#include "../world.h"

void entity_local_player(uint32_t id, struct entity* e, struct world* w);
/* Online (remote session) F3-1: JUMP while riding a cart asks the SERVER to
 * dismount us through Beta's use-entity toggle (Packet7 via SRPC_ENTITY_USE);
 * no local occupancy edit, no ride-intent RPC. Returns whether the RPC was
 * accepted into the client->server queue. Exported for the wire test. */
bool local_player_online_dismount(struct entity* cart);
bool entity_local_player_block_collide(vec3 pos, struct block_info* blk_info);
/* Unmounted prediction with explicit player state and immutable input.
 * The caller resolves rides before entering this walking step. NULL or
 * disconnected input contributes no actions; it never falls back to player 0. */
struct client_session;
struct input_player_frame;
bool entity_local_player_walk_owned(struct entity* e,
	struct client_session* session, const struct input_player_frame* frame);


void entity_item(uint32_t id, struct entity* e, bool server, void* world,
				 struct item_data it);

void entity_monster(uint32_t id, struct entity* e, bool server, void* world,
				 int monster_id);

void entity_minecart(uint32_t id, struct entity* e, bool server, void* world);

void entity_arrow_visual_init(struct entity* e);
void entity_arrow_visual_restore(struct entity* e);

void entity_arrow(uint32_t id, struct entity* e, bool server, void* world,
				  uint32_t owner_id, uint16_t item_id, int damage,
				  bool from_player);
void entity_fireball_render(struct entity* e, struct camera* c,
						   float tick_delta);
void entity_thrown_render(struct entity* e, struct camera* c,
						  float tick_delta);

// Beta EntityEgg (M4 S5): SERVER-side thrown egg. The client renders the
// CRPC_SPAWN_ARROW display entity instead (see ENTITY_EGG in the type enum).
void entity_egg(uint32_t id, struct entity* e, bool server, void* world,
				uint32_t owner_id);

// Beta EntitySnowball (batch 1a): the egg minus the chick roll; same tick,
// same data union member, same CRPC_SPAWN_ARROW client display.
void entity_snowball(uint32_t id, struct entity* e, bool server, void* world,
					 uint32_t owner_id);

// Beta EntityFireball (batch 1b): the ghast's shot. accel = the launch
// direction * 0.1 (normalized inside).
void entity_fireball(uint32_t id, struct entity* e, bool server, void* world,
					 uint32_t owner_id, const float dir[3]);
// Punch-deflection [740c583: EntityFireball.attackEntityFrom]: motion = the
// attacker's look vector, accel = look * 0.1.
void entity_fireball_deflect(struct entity* e, const float look[3]);

// Beta EntityBoat (boat-slice): setSize(1.5F, 0.6F), yOffset = height/2.
void entity_boat(uint32_t id, struct entity* e, bool server, void* world);

// MP S3: a remote (other-player) render entity -- CLIENT-ONLY, constructed by
// client_interface.c's CRPC_SPAWN_PLAYER consumer. Player-sized bbox
// (0.6x1.8, like the local player); e->render draws the shared ModelBiped
// (render_entity_player, see the ENTITY_REMOTE_PLAYER enum comment).
// `username` is copied and NUL-clamped into e->data.remote_player.username.
struct client_session;
void entity_local_player_render_other(const struct client_session* owner,
	mat4 view, float tick_delta);
void entity_remote_player(uint32_t id, struct entity* e, void* world,
						  const char* username);
/* MP-BED: EntityPlayer.sleepInBedAt for a remote player on the multiplayer
 * client (EntityPlayer.java:537-588; the multiplayerWorld branch skips every
 * check). block_exists/bed_meta = World.blockExists + getBlockMetadata at the
 * bed cell, read by the caller. */
void entity_remote_player_sleep_at(struct entity* e, int32_t bed_x,
								   int32_t bed_y, int32_t bed_z,
								   bool block_exists, uint8_t bed_meta);
/* MP-BED: EntityPlayer.wakeUpPlayer(false, false, false) as Packet18 animate 3
 * drives it (NetClientHandler.java:359-361; EntityPlayer.java:613-640). */
void entity_remote_player_wake(struct entity* e);
/* MP-RIDE: the rider position Entity.updateRiderPosition gives a remote
 * player (yOffset 0, EntityPlayer.getYOffset = yOffset - 0.5) on `vehicle`
 * (Entity.java:929-931, 937-939; EntityBoat/EntityMinecart.java
 * getMountedYOffset = -0.3; EntitySpider.java:11-13). False if no seat. */
bool entity_remote_player_rider_pos(const struct entity* vehicle, vec3 out);
/* MP-RIDE: Entity.mountEntity(vehicle or NULL) for a remote player
 * (Entity.java:941-967); `entities` resolves the vehicle being left. */
void entity_remote_player_mount(struct entity* rider, struct entity* vehicle,
								struct dict_entity_s* entities);

uint32_t entity_gen_id(dict_entity_t dict);
void entities_client_tick(dict_entity_t dict);
void entities_client_render(dict_entity_t dict, struct camera* c,
							float tick_delta);

void entity_default_init(struct entity* e, bool server, void* world);
/* Seed a mob-owned gameplay_rng for a new server lifetime. Pure: mixes only the
 * declared world seed, dimension and assigned entity ID (no world-stream draw,
 * no clock). The 32-bit mix is ZERO-extended into Java's 64-bit setSeed input;
 * jrand_set_seed also clears the nextGaussian cache. A reproducible lifetime
 * adaptation: equal declared seeds give Java-identical primitive draws, not
 * Java's wall-clock default-constructor seed. */
void entity_lifetime_rng_seed(struct java_random* r, int64_t world_seed,
                              int dimension, uint32_t id);
/* A2-E: bind `e` to `world` -- sets e->world and, for a server entity bound
 * to a struct server_world*, stamps that instance's (slot, generation).
 * Every server constructor binds through entity_default_init; queued joins
 * and restore paths that rebind call this directly. NULL world = unbound. */
#define ENTITY_A2E_GENERATION_BIND 1
void entity_bind_world(struct entity* e, void* world);
void entity_default_teleport(struct entity* e, vec3 pos);
bool entity_default_client_tick(struct entity* e);
/* One Beta setPositionAndRotation2 approach step (see the network_increments
 * field comment). apply_look=false for ticks whose orientation is derived
 * elsewhere (mob head yaw). */
void entity_network_approach_tick(struct entity* e, bool apply_look);
void entity_set_size(struct entity* e, float width, float height,
					 float y_offset);
void entity_make_local_bbox(const struct entity* e, struct AABB* out);
void entity_make_world_bbox(const struct entity* e, struct AABB* out);
void entity_set_position_from_bbox(struct entity* e, const struct AABB* box);

void entity_shadow(struct entity* e, struct AABB* a, mat4 view);

bool entity_get_block(struct entity* e, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk);
void entity_soulsand_slowdown(struct entity* e, const struct AABB* box,
							  float* vx, float* vz);
/* next-gaps-0924/W2 P12: BlockWeb.onEntityCollidedWithBlock [BlockWeb.java:
 * 10-12 `isInWeb = true`] reached through Entity.moveEntity's post-move cell
 * sweep [Entity.java:460-476, floor(min+0.001) .. floor(max-0.001)
 * inclusive]: sets e->in_web when any cell of the ABSOLUTE post-move box
 * `box` is a cobweb. For owners whose sweep is not a shared function (the
 * client item prediction); the player/mob/item server sweeps carry the arm
 * inline. docs/intent/p12-cobweb.md. */
void entity_web_contact(struct entity* e, const struct AABB* box);
/* Entity.moveEntity:262-268, the first thing a move does:
 *     if(isInWeb) { isInWeb = false; var1 *= 0.25D; var3 *= (double)0.05F;
 *                   var5 *= 0.25D; motionX = motionY = motionZ = 0.0D; }
 * This port moves by e->vel, so the caller scales e->vel here, moves, then
 * zeroes e->vel (entity_web_release) before its friction -- the same bytes
 * Beta's zero-before-move + collision-zeroing leave. Returns true when the
 * web was consumed (the caller's release flag). */
bool entity_web_consume(struct entity* e);
/* Beta World.handleMaterialAcceleration(box, Material.water, this)
 * [World.java:1418-1450]: scans floor(min) .. floor(max + 1.0) (half-open) of
 * the given box in Beta's doubles, fails closed when any chunk of that span
 * (checkChunksExist, World.java:327-348, inclusive of floor(max+1)'s chunk) is
 * absent, sums BlockFluid.getFlowVector [BlockFluid.java:86-166] over every
 * water cell, normalises (Vec3D.normalize, float-rounded sqrt) and ADDS the
 * 0.014D impulse to e->vel. Returns Beta's inWater verdict. One definition for
 * every entity that reaches Entity.onEntityUpdate (next-gaps-0924/W2 P3,
 * docs/intent/p3-water-flow-push.md). Client entities read the client world
 * (chunkExists = the column-present gate); server entities the server world. */
bool entity_water_acceleration(struct entity* e, double min_x, double min_y,
							   double min_z, double max_x, double max_y,
							   double max_z);
/* G8-BLAST-VELOCITY v3: the same World.handleMaterialAcceleration on a
 * SERVER world for a body that is not a struct entity (the server player):
 * returns inWater and writes the exact double impulse (normalised flow *
 * 0.014D, zero when none) instead of applying it. */
struct server_world;
bool entity_server_water_flow_impulse(struct server_world* w, double min_x,
	double min_y, double min_z, double max_x, double max_y, double max_z,
	double out[3]);
/* Entity.handleWaterMovement [Entity.java:543-545]: the Entity.setPosition box
 * (pos_x +- width/2F, box_min_y = posY - yOffset + ySize, + height) expanded
 * (0, -0.4F, 0) and contracted 0.001 on every axis, through
 * entity_water_acceleration. Players and every EntityLiving; EntityItem
 * overrides it with the plain box (entity_item.c). */
bool entity_handle_water_movement(struct entity* e, double pos_x,
								  double box_min_y, double pos_z, float width,
								  float height);
/* ---- next-gaps-0924/W2 P4: Beta entity-entity push (docs/intent/p4-entity-push.md)
 * Entity.applyEntityCollision [Entity.java:668-691] on Beta's doubles: the
 * pushed entity (`this`) at this_x/z, the mover (`var1`) at mover_x/z. False
 * below the 0.01F gate; otherwise dx and dz hold Beta's scaled values -- the
 * pushed entity adds (-dx, 0, -dz), the mover adds (dx, 0, dz). */
bool entity_collision_impulse(double this_x, double this_z, double mover_x,
							  double mover_z, double* dx, double* dz);
/* EntityLiving.onLivingUpdate:641-651 for a SERVER mob `mover`, run right
 * after its moveEntityWithHeading: every candidate of
 * getEntitiesWithinAABBExcludingEntity(this, bb.expand(0.2F, 0, 0.2F))
 * [World.java:2007-2023, Chunk.java:484-506 -- the Beta-ordered
 * server_local entity query snapshot] that canBePushed() [EntityLiving.java:
 * 85-87] gets applyEntityCollision(mover); impulses land on e->vel at once
 * (Beta's repeated-call semantics: later movers this tick see them). The
 * host player is a candidate; its half lands on its server motion. */
void entity_living_collide_with_nearby(struct entity* mover,
									   struct server_local* s);
/* G8-BLAST-VELOCITY v4: the same loop with a PLAYER body (host or guest) as
 * the mover, at an explicit box -- the box moveEntityWithHeading left inside
 * ONE accepted movement packet's onUpdateEntity (NetServerHandler.handleFlying
 * -> EntityLiving.onLivingUpdate:623-629). Candidates take their -dx half;
 * the mover's +dx half is added to *mx / *mz (that packet's motion). */
struct server_player;
void entity_actor_collide_with_nearby(struct server_local* s,
	struct server_world* w, const struct server_player* mover, double min_x,
	double min_y, double min_z, double max_x, double max_y, double max_z,
	double* mx, double* mz);
/* v5: there is no per-tick player loop. Beta runs a player's deathTime
 * step and own push loop only inside its packets' onUpdateEntity; the
 * server_local replay runs them per packet, plus one stationary corpse
 * packet per update for a dead body (server_local_replay_actor_motion). */
bool entity_intersection(struct entity* e, struct AABB* a,
						 bool (*test)(struct AABB* entity,
									  struct block_info* blk_info));
bool entity_block_aabb_test(struct AABB* entity, struct block_info* blk_info);
bool entity_aabb_intersection(struct entity* e, struct AABB* a);
void entity_try_move(struct entity* e, vec3 pos, vec3 vel, struct AABB* bbox,
					 size_t coord, bool* collision_xz, bool* on_ground);
// Beta 1.7.3 sneak ledge-stop (Entity.moveEntity, onGround && isSneaking branch): shrink
// e->vel x/z toward 0 in 0.05 steps while stepping there would leave the entity over a gap
// (its LOCAL bbox, offset to (pos + delta, -1), hits no block). Call only when the entity
// is on the ground AND sneaking. Exposed for the sneak golden test.
void entity_sneak_ledge_clamp(struct entity* e, struct AABB* bbox);
bool entity_aabb_intersect_ray(const vec3 origin,
                               const vec3 dir,
                               const struct entity *e,
                               float *out_t);

struct entity *raycast_entity(dict_entity_t *entities,
                              const vec3 origin,
                              const vec3 dir,
                              float maxDist,
                              float *out_tNear);
void entity_damp_velocity(struct entity* e, float threshold);
void entity_move_in_direction(struct entity* e, float accel, vec2 dir);
void entity_clamp_speed(struct entity* e, float max_speed);
void entity_apply_gravity(struct entity* e, float gravity);
void entity_apply_friction(struct entity* e, float slip);
// Gravity + friction in ONE call, reading on_ground for the slip selection BEFORE
// clearing it. The split pair above clears on_ground inside entity_apply_gravity, so
// a following entity_apply_friction(e->on_ground?...) always sees the air slip -- use
// this combined form when both run on the same entity in the same tick.
void entity_apply_gravity_and_friction(struct entity* e, float gravity,
                                       float slip_ground, float slip_air);
bool entity_try_auto_jump(struct entity* e, float jump_force, float threshold);
void entity_try_unstuck(struct entity* e, void (*make_bbox)(struct AABB*));
void entity_try_move_axis(struct entity* e, int axis, struct AABB (*make_bbox)(void), bool* collision_flag, bool* on_ground_flag);
void entity_get_delta(struct entity* e, vec3 out_delta);
void entity_blend_body_to_head(float* body_yaw, float head_yaw, float factor);
void entity_tick_animation(struct entity* e, float walk_speed, int max_frame);

#endif
