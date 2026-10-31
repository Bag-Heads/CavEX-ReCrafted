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

#ifndef SERVER_LOCAL
#define SERVER_LOCAL

#include <m-lib/m-string.h> // string_t (server_local_load_world_impl)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../cglm/cglm.h" // vec3 (spawn/emit/query API)
#include "../block/blocks_data.h" // w_coord_t, struct block_data (by value), struct block_info
#include "../block/dispenser_logic.h" // DISPENSER_TE_SLOTS (dispenser scatter/dispense array bound)
#include "../item/inventory.h" // set_inv_slot_t (DICT_SET_DEF, by value), struct inventory/inventory_logic, struct item_data
#include "../entity/actor_ident.h" // struct actor_ident (by value, S1-a2 actor roles)
#include "../worldgen/java_random.h" // struct java_random (by value, server_local_math_random_state)

// Interface split (build_architecture step 7, slice 4, 2026-08-27): this is now
// the OPAQUE API surface. The full struct server_local definition (and every
// embedded helper struct/enum it needs) lives in server_local_state.h --
// include that instead if the TU embeds the struct by value or dereferences
// its fields; every other caller (the overwhelming majority) only ever holds/
// passes a `struct server_local*` and takes this header, so a struct-field
// edit no longer forces a rebuild.
struct server_local;
#ifdef CAVEWII_SERVER_LOCAL_TEST
struct death_drop_journal;
bool server_local_test_plan_death_drops(struct server_local* s,
                                      struct death_drop_journal* journal);
#endif

// Forward decls for pointer-only parameters below (their full definitions
// live in server_local_state.h or their own owning headers -- never pulled
// in here just to name a pointer type).
struct entity;
struct server_player;
struct server_rpc;
struct server_rpc_source;
/* R4 freeze observer: fixed 32-bit words, float values as IEEE-754 bits.
 * The getter makes at most four attempts; false means no coherent sample. */
enum server_portal_diag_field {
	SPD_VALID, SPD_DEMO_SPLIT, SPD_DEMO_DEFER,
	SPD_DEMO_PENDING, SPD_DEMO_LIT,
	SPD_DEMO_X, SPD_DEMO_Y, SPD_DEMO_Z,
	SPD_HOST_X, SPD_HOST_Y, SPD_HOST_Z, SPD_HOST_WORLD, SPD_HOST_TOKEN,
	SPD_GUEST_X, SPD_GUEST_Y, SPD_GUEST_Z, SPD_GUEST_WORLD, SPD_GUEST_TOKEN,
	SPD_GUEST_PRESENT, SPD_GUEST_GAMEPLAY, SPD_GUEST_IN_PORTAL,
	SPD_GUEST_DWELL, SPD_GUEST_COOLDOWN,
	SPD_POS_ACCEPT, SPD_POS_NOT_READY, SPD_POS_FROZEN, SPD_POS_SLEEP,
	SPD_POS_RESPAWN, SPD_POS_BARRIER, SPD_POS_TOKEN, SPD_POS_INVALID,
	SPD_POS_DELTA, SPD_POS_VERTICAL, SPD_POS_LAST_RESULT,
	SPD_POS_LAST_RX_TOKEN, SPD_POS_LAST_EXPECT_TOKEN, SPD_POS_LAST_DELTA,
	SPD_HOST_TX_ID, SPD_HOST_TX_OPEN, SPD_HOST_TX_STATE,
	SPD_HOST_TX_SRC, SPD_HOST_TX_DST, SPD_HOST_TX_ACTOR,
	SPD_HOST_TX_LAST_ID, SPD_HOST_TX_LAST_STATE,
	SPD_GUEST_TX_ID, SPD_GUEST_TX_OPEN, SPD_GUEST_TX_STATE,
	SPD_GUEST_TX_SRC, SPD_GUEST_TX_DST, SPD_GUEST_TX_ACTOR,
	SPD_GUEST_TX_LAST_ID, SPD_GUEST_TX_LAST_STATE,
	SPD_FIELD_COUNT
};
enum server_portal_diag_pos_result {
	SPD_POS_RESULT_ACCEPT = 0, SPD_POS_RESULT_NOT_READY,
	SPD_POS_RESULT_FROZEN, SPD_POS_RESULT_SLEEP, SPD_POS_RESULT_RESPAWN,
	SPD_POS_RESULT_BARRIER, SPD_POS_RESULT_TOKEN, SPD_POS_RESULT_INVALID,
	SPD_POS_RESULT_DELTA, SPD_POS_RESULT_VERTICAL
};
struct server_portal_diag_snapshot { uint32_t word[SPD_FIELD_COUNT]; };
bool server_local_portal_diag_get(struct server_portal_diag_snapshot* out);
/* Server-thread-only writers. They do not affect actor or transfer state. */
void server_local_portal_diag_publish(struct server_local* s);
void server_local_portal_diag_guest_pos(struct server_local* s,
	enum server_portal_diag_pos_result result, uint32_t received_token,
	float horizontal_delta_sq);
/* Local provider/server thread only. Binding does not admit a playable guest:
 * the provider must initialize/persist the actor and install its reply queue.
 * Detach refuses an open window or carried item; settle them before retrying. */
/* O07 / R04 D2-1: the monotonic guest-binding sequence (server_local_guest.c)
 * stamped on guest gate-1 resets and GAMEPLAY_READY. */
uint32_t server_local_guest_bind_seq_bump(void);
uint32_t server_local_guest_bind_seq(void);
/* O10: test seam -- seed the counter to exercise the wrap boundary. */
void server_local_test_guest_bind_seq_set(uint32_t seq);
bool server_local_bind_guest(struct server_local* s, struct server_player* actor,
	struct server_rpc_source* source);
bool server_local_unbind_guest(struct server_local* s,
	const struct server_rpc_source* source);
/* S1-a2: the actor-aware boat occupancy writer; the historic
 * server_local_set_boat_occupancy is the host wrapper over it. */
void server_local_set_boat_occupancy_for(struct server_local* s,
	struct entity* boat, struct server_player* rider, bool occupied);
/* ===== S1-a2 actor identity (see ../entity/actor_ident.h) ================
 * resolve: the actor a stored role names, or NULL when that role is STALE --
 * the guest left, was released, or rejoined under a new generation. Every
 * consumer must treat NULL as "invalidate this role now" (drop the target,
 * remove the bobber, force the dismount), which is what makes the identity
 * lifetime-safe without any cross-module cleanup sweep.
 * ident: the identity to stamp for an acting actor. &s->player -> the host
 * zero value, so stamping the host changes nothing observable. */
struct server_player* server_local_actor_resolve(struct server_local* s,
	struct actor_ident id);
/* V6 F1 round 2/3: the ONE writer of the shared pause (server thread):
 * every already-received held request keeps its arrival pause state
 * (svin_stamp_received); except_slot = the toggling source, -1 = none. */
void server_local_set_paused(struct server_local* s, bool paused,
	int except_slot);
struct actor_ident server_local_actor_ident(const struct server_local* s,
	const struct server_player* player);
/* PIGZ-PACK: the `var1 instanceof EntityPlayer` branch of Beta's
 * EntityPigZombie.attackEntityFrom [server EntityPigZombie.java:49-64], run
 * BEFORE super.attackEntityFrom (so before any health/i-frame/death gate and
 * before the hurt pitch). Every OTHER pig zombie whose box intersects
 * struck.boundingBox.expand(32,32,32) in the STRUCK mob's own world -- dying
 * ones included (World.getEntitiesWithinAABBExcludingEntity filters only
 * the excluded entity) -- becomeAngryAt(attacker), then the struck one does.
 * becomeAngryAt = playerToAttack latch + angerLevel 400+nextInt(400) +
 * randomSoundDelay nextInt(40), on EACH pig zombie's own Entity.rand
 * (gameplay_rng). Callers pass only player attackers (melee, player-fired
 * arrow, player-thrown egg/snowball, fishing hook); no-op for a non-pig-
 * zombie `struck`. */
void server_local_pig_zombie_pack_anger(struct server_local* s,
	struct entity* struck, struct actor_ident attacker);
/* G8-VEHICLE-SEAMS-01: the wire entity id an actor is published under -- the
 * host's SERVER_LOCAL_PLAYER_ENTITY_ID, the admitted guest's entity id. */
uint32_t server_local_actor_wire_id(const struct server_local* s,
	const struct server_player* actor);
/* The rider a vehicle relation names, or NULL when that relation is no longer
 * valid: the lifetime does not resolve, the rider is not committed to the
 * vehicle's world, or the stored wire id is not that rider's. Consumers drop
 * the relation on NULL (Beta drops riddenByEntity once the rider is gone). */
struct server_player* server_local_vehicle_rider(struct server_local* s,
	const struct entity* vehicle, struct actor_ident actor, uint32_t wire_id);
/* Clear a pig's rider relation completely (ridden flag, wire id, lifetime,
 * pending seat correction). */
void server_local_pig_clear_rider(struct entity* pig);
struct server_world;
/* ===== A0 FROZEN lane-C contract: actor -> world context ==================
 * The world context `actor` currently occupies. Code that acts FOR an actor
 * must reach its world through this call instead of `&s->world`, so the
 * dual-world work (A1/A2) can give each actor its own context without another
 * sweep of the callers.
 * - `actor` must be &s->player (host) or the currently bound s->guest_actor;
 *   anything else (NULL, a released/stale guest pointer) returns NULL, which
 *   callers treat as "this actor has no world: do nothing".
 * - Today both actors share the single context, so it returns &s->worlds[0]
 *   (while live) for either actor. Once dimensions land the two may differ.
 * - Valid for the current server tick only: travel may move the actor to a
 *   different context, so never cache the pointer across ticks.
 * Signature and semantics are frozen (split-dimensions-plan-0924 slice A0);
 * change them only through an Astra contract revision. */
struct server_world* server_local_world_of(struct server_local* s,
	const struct server_player* actor);
/* ===== A1a contract (split-dimensions-plan-0924, Astra D8) ===============
 * server_local_world_of above now also requires the world to be LIVE: it
 * returns NULL while the actor's slot has no live world (release -> load
 * window), in addition to NULL for a non-admitted actor. Signature unchanged.
 *
 * server_local_world_from_handle: resolve a stored world handle (see struct
 * server_world_handle, server_world_handle_of) to its live world, or NULL if
 * the slot is out of range, not live, owned by another server, or now holds a
 * DIFFERENT instance (discarded / re-created) -- the stale-generation rule.
 * Use it for every piece of delayed work that names a world (portal jobs,
 * queued publications, window bindings); never keep a raw world pointer
 * across ticks. */
struct server_world_handle;
struct server_world* server_local_world_from_handle(struct server_local* s,
	struct server_world_handle h);
struct chunk_entities;
struct random_gen;
struct death_drop_provenance;
struct server_entity_query_ref;
// enum server_world_switch_phase (full definition in server_local_state.h) is
// used by VALUE below (server_local_test_world_switch_handoff_fn's parameter).
// A bare forward reference is a GNU C extension (accepted under this project's
// -std=gnu99 build and by check_header_hygiene.py's probe), not portable ISO C,
// but it is the only way to name the type here without including the state
// header -- verified compiling under -std=gnu99 -Wall with no diagnostic.
enum server_world_switch_phase;

#define MAX_PENDING_ARROWS \
	((int)(sizeof(((struct server_local*)0)->pending_arrows) \
		   / sizeof(struct pending_arrow)))

void server_local_create(struct server_local* s, bool give_test_kit,
						 bool chest_selftest, bool fluid_demo, bool water_demo,
						 bool zombie_demo, bool superflat, bool perf_stress);
/* Two-phase startup for callers that must publish configuration before the
 * server thread can observe the struct. Initialization is one-shot for the
 * process-lifetime server object (including its populate snapshot reservation).
 * `server_local_create` remains the initialize+start convenience used by tools. */
void server_local_init(struct server_local* s, bool give_test_kit,
					   bool chest_selftest, bool fluid_demo, bool water_demo,
					   bool zombie_demo, bool superflat, bool perf_stress);
bool server_local_start(struct server_local* s);
/* Clean quit: raise the stop flag and JOIN the server thread, so no region/
 * level write can be in flight when the process tears down (Wii: exit to
 * loader). Idempotent; a no-op if server_local_start never succeeded.
 * Bounded latency: the thread checks the flag once per tick. */
void server_local_shutdown(struct server_local* s);
/* Exclusive world owner only: server thread or shutdown after its join.
 * Covers/reconciles every R24 token and latches any failure for the profile. */
bool server_local_checkpoint_drain(struct server_local* s);
struct entity* server_local_spawn_minecart(vec3 pos, struct server_local* s);
/* Actor placement uses these live-world primitives. Legacy spawn names below
 * intentionally retain slot 0 for single-world fixtures/probes. */
struct entity* server_local_spawn_minecart_type_w(vec3 pos,
	struct server_world* w, uint8_t minecart_type);
struct entity* server_local_spawn_boat_w(vec3 pos, struct server_world* w);
// EntityMinecart.attackEntityFrom `> 40`: dismount + drops only (no removal);
// see the definition for the two callers.
void server_local_minecart_break_drops(struct server_local* s,
									   struct entity* cart);
// Beta EntityBoat, spawned from ItemBoat.onItemRightClick's own raycast (see
// server_local_place_boat). Zero initial velocity (Beta's coordinate ctor
// sets motionX/Y/Z=0.0D) -- unlike server_local_spawn_minecart, do not add a
// random launch nudge; EntityBoat's own constructor has none.
struct entity* server_local_spawn_boat(vec3 pos, struct server_local* s);
// Single writer for authoritative boat occupancy (mirrors
// server_set_minecart_occupancy's discipline): keeps s->player.riding_boat
// (footstep gate) in lockstep with the boat's own occupied/occupant_id, and
// is the only place either is written after spawn.
void server_local_set_boat_occupancy(
	struct server_local* s, struct entity* boat, bool occupied);
// ItemBoat.onItemRightClick: the item's own 5.0-block eye-ray (stopping on
// the first non-air cell, liquids included), spawn + consume one boat on a
// hit. No-op (does not consume) on a miss. [740c583: ItemBoat]
bool server_local_place_boat(struct server_local* s);
/* Beta EntityMinecart(World,d,d,d,int)'s trailing `int var8` -> minecartType
 * [740c583]. server_local_spawn_minecart(pos, s) is the type-0 (rideable) case,
 * now a thin wrapper over this so its own behaviour is provably unchanged.
 * minecart_type is 0/1/2; the caller (an item's onItemPlace) owns picking the
 * right type -- see ItemMinecart.<init>'s type argument, mirrored by
 * item_minecart_chest.c / item_minecart_furnace.c. */
struct entity* server_local_spawn_minecart_type(vec3 pos, struct server_local* s,
												 uint8_t minecart_type);
// Beta EntityPainting, fresh placement only [740c583:
// ItemPainting.onItemUse]. `art_index` must already be the RNG-selected
// fitting art (entity_painting_select_art) -- this function performs no
// selection/validity logic of its own, only alloc+init+join. Returns NULL on
// an allocation/join failure (droppable per CODESTYLE 1.7).
struct entity* server_local_spawn_painting(struct server_local* s,
										   int art_index, int direction,
										   int tile_x, int tile_y,
										   int tile_z);
/* I02: as above, with an id the caller already reserved (server_local_gen_id)
 * because the art pick drew from that id's lifetime stream. */
struct entity* server_local_spawn_painting_id(struct server_local* s,
											  uint32_t id, int art_index,
											  int direction, int tile_x,
											  int tile_y, int tile_z);
// ItemPainting's explicit acting-world join; legacy wrappers retain slot 0.
struct entity* server_local_spawn_painting_id_w(struct server_local* s,
	struct server_world* w, uint32_t id, int art_index, int direction,
	int tile_x, int tile_y, int tile_z);
// Beta EntityTNTPrimed. Fresh spawns use the supplied fuse and independent
// launch RNG; detached reservations let Explosion reserve memory, dictionary,
// and live-list capacity before any AIR write without consuming an id or RNG
// draw early.
struct entity* server_local_spawn_primed_tnt(struct server_local* s, vec3 pos,
										 int fuse);
struct entity* server_local_reserve_primed_tnt(struct server_local* s);
void server_local_cancel_primed_tnt_reservation(
	struct server_local* s, struct entity* e);
struct entity* server_local_commit_reserved_primed_tnt(
	struct server_local* s, struct entity* e, vec3 pos, int fuse);
/* A1a: the chain TNT joins the exploding world `w` (session via w->session). */
struct entity* server_local_commit_reserved_chain_tnt(
	struct server_world* w, struct entity* e, vec3 pos);
/* Reserve the entity first, replace the source block with the caller-supplied
 * Beta result, then publish the spawn. Used by fire, whose tryToCatch path may
 * leave FIRE rather than AIR before invoking BlockTNT's prime hook. */
bool server_local_prime_tnt_block_replacing(
	struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z, int fuse,
	struct block_data replacement);
bool server_local_prime_tnt_block(struct server_local* s, w_coord_t x,
								 w_coord_t y, w_coord_t z, int fuse);

// Beta EntityFallingSand, spawned from BlockSand.tryToFall (block_sand.c's
// block_sand_try_to_fall, the only caller). Pure alloc+construct+teleport+
// join+publish, mirroring server_local_spawn_boat's shape -- no Beta decision
// logic of its own (the entity-vs-direct-settle choice, and the 32-radius
// loaded-area check, belong to the caller). `block_id` is BLOCK_SAND or
// BLOCK_GRAVEL. Returns NULL on an allocation/join failure (droppable per
// CODESTYLE 1.7) -- the caller has already committed to the entity path by
// this point (chunks loaded), so a NULL here simply drops the fall, matching
// this port's other spawn-failure fallbacks.
struct entity* server_local_spawn_falling_sand(struct server_local* s,
											   vec3 pos, uint8_t block_id);

/* ===== S0 audio: server-caused sound events ============================= */
// Fire-and-forget CRPC_SOUND wrapper: EVERY sound the server causes goes
// through this RPC even in the single-process build (thin-client doctrine;
// the CRPC_SMOKE_PUFF boundary rule -- server tick code never touches the
// client voice pool). No-ops on AUDIO_SOUND_NONE. vol/pitch are the Beta
// call-site formulas, rolled server-side.
void server_local_emit_sound(uint16_t sound_id, vec3 pos, float vol,
							 float pitch);
// The server-owned RNG for those cosmetic volume/pitch rolls. Beta drew them
// from the shared world Random; CaveWii's rand_src is NOT that stream (it
// backs its own pinned sim rolls: wool counts, chick rolls, drop counts), so
// the sound rolls get a DEDICATED lazily-seeded stream -- the s_math_random
// precedent -- and every seeded sim-RNG test stays byte-stable.
struct random_gen* server_local_sound_rng(void);
// Beta's standard living-sound pitch roll:
// (rand.nextFloat() - rand.nextFloat()) * 0.2F + 1.0F
// [740c583: EntityLiving.attackEntityFrom / playLivingSound].
float server_local_sound_pitch_wobble(void);
// S2: the Beta playSoundFX path (NON-positional, quarter-volume client-side)
// as a CRPC -- portal.trigger/travel. Same fire-and-forget contract as
// server_local_emit_sound; the client routes it to audio_play_ui.
void server_local_emit_sound_ui(uint16_t sound_id, float vol, float pitch);
// S2 shared footstep tail [740c583: Entity.moveEntity:478-499]: resolve the
// foot cell (fx,fy,fz)'s STEP sound with the snow-LAYER-above override (the
// cell fy+1 == Block.snow id 78 -> cloth) and liquid silence (water/lava foot
// cell -> no sound), then emit stepSoundDir2() at getVolume()*0.15F,
// getPitch(), at `at`. Shared by the player SRPC_PLAYER_POS odometer and the
// mob odometer (entity_monster.c).
void server_local_emit_footstep(struct server_local* s, w_coord_t fx,
								w_coord_t fy, w_coord_t fz, vec3 at);
/* A1a (A1-CONTRACT row #16): the same footstep tail for a mover whose world
 * is known. `w` is the world the foot cell is read from; publication follows
 * that world (only slot 0 has a client view until B1 routes per world). The
 * server_local form above is the legacy bridge onto slot 0 for the movement
 * paths lane C converts (A1d). */
void server_local_emit_footstep_world(struct server_world* w, w_coord_t fx,
									  w_coord_t fy, w_coord_t fz, vec3 at);
// S2 water-entry splash [740c583: Entity.onEntityUpdate:205-232]: emit
// random.splash at `at` with Beta's fully velocity-derived volume
// min(1, sqrt(mx^2*0.2 + my^2 + mz^2*0.2) * 0.2) and pitch 1.0F+(r-r)*0.4F.
// m* are the entity's PER-TICK motion components (Beta motionX/Y/Z units).
// Callers own the was_in_water false->true edge detection.
void server_local_emit_splash(vec3 at, float mx, float my, float mz);
// The full per-tick splash edge (post-move wet probe + latch + earshot-culled
// emit) for any server entity that reaches Beta's Entity.onEntityUpdate --
// living entities, items, arrows (each calls super.onUpdate(); minecart/TNT/
// falling sand do not and stay splashless). Call once per tick after the move;
// `was_in_water` is the caller's latch, init TRUE at spawn (Beta's
// isFirstUpdate guard: spawning/restoring INTO water never splashes).
void server_local_splash_tick(struct server_local* s, struct entity* e,
							  bool* was_in_water);
// The same edge with the caller's Beta inWater verdict (mobs/items since
// next-gaps-0924/W2 P3: entity_handle_water_movement, at the tick top).
void server_local_splash_edge(struct server_local* s, struct entity* e,
							  bool wet_now, bool* was_in_water);

/* G6 (O13): player death drops -- InventoryPlayer.dropAllItems order (in
 * this engine's slot layout) and one Beta dropPlayerItemWithRandomChoice(
 * stack, true) item at the dying player's eye-basis position. */
extern const uint8_t server_local_death_drop_order[];
struct entity* server_local_spawn_death_item_w(struct server_world* w,
	struct server_player* dying, double x, double eye_y, double z,
	struct item_data* it);
/* O16 (R06 P1): the dying actor's own Random (Beta Entity.rand), seeded on
 * first use; never the world stream. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* F4-RNG test seam: give body `p` a deterministic Entity.rand seed
 * (java.util.Random.setSeed semantics) for the CURRENT body lifetime. */
void server_local_test_seed_body_rand(struct server_local* s,
	struct server_player* p, int64_t seed);
#endif
struct java_random* server_local_player_rand(struct server_local* s,
	struct server_player* p);
struct entity* server_local_spawn_item(vec3 pos, struct item_data* it,
									   bool throw, struct server_local* s);
// All-or-nothing close-time drop batch. A broken open double chest can orphan
// all 54 storage slots plus its cursor; 64 keeps that entire ownership transfer
// atomic with fixed rollback storage and no failure-prone scratch allocation.
#define SERVER_LOCAL_ITEM_DROP_BATCH_MAX 64
bool server_local_spawn_item_batch(vec3 pos, const struct item_data* items,
									size_t count, bool throw,
									struct server_local* s);
bool server_local_spawn_item_batch_w(struct server_world* w, vec3 pos,
	const struct item_data* items, size_t count, bool throw);
// Atomic variant for chest/furnace contents. Uses container_drop_rng rather
// than the simulation/world stream (Beta BlockChest has its own Random).
bool server_local_spawn_container_item_batch(
	vec3 pos, const struct item_data* items, size_t count, bool throw,
	struct server_local* s);
/* Block-removal variant for a complete chest/furnace backing array. During an
 * active Nether-death transaction, only an exact container_slots snapshot may
 * transition to durable tagged drops; an unexplained/mismatched owner remains
 * live and returns false. Outside that transaction this is the ordinary atomic
 * container-drop batch. */
bool server_local_spawn_destroyed_container_items(
	struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z,
	const struct item_data* slots, size_t slot_count);
// Beta BlockFurnace.onBlockRemoval scatter, atomic for the furnace TE retry
// contract. Clears an open furnace window only after all pieces are published.
bool server_local_furnace_scatter_contents(struct server_local* s,
	w_coord_t x, w_coord_t y, w_coord_t z, const struct item_data slots[3]);
/* Inventory-close variant carrying the exact old owner for each stack. In an
 * ordinary close it is behavior-identical to the two APIs above. During a
 * Nether-death journal it is the only accepted batch path. */
bool server_local_spawn_sourced_item_batch(
	vec3 pos, const struct item_data* items,
	const struct death_drop_provenance* provenance, size_t count, bool throw,
	bool container_rng, struct server_local* s);
// If the currently open authoritative window owns the block at x/y/z, spill
// only that block's storage slots at the block position and clear/sync them.
// The player's cursor is deliberately untouched. False means no matching
// owner or an allocation failure; in either case the window contents remain.
bool server_local_spill_active_container(struct server_local* s,
									  w_coord_t x, w_coord_t y, w_coord_t z);
// Dispenser-slice: BlockDispenser.dispenseItem, called from block_dispenser.c's
// onWorldTick after it re-checks the two-cell powered condition. Looks up the
// TE at (x,y,z), draws the reservoir-sampled stack (dispenser_random) and, by
// item id, spawns an EntityArrow/EntityEgg/EntitySnowball (setXHeading: the
// normalized (dx,0.1,dz) heading + 3 gaussians*0.0075*6.0F, scaled by speed
// 1.1F -- drawn from s->rand_src, matching the entity_monster.c skeleton-fire
// precedent rather than Beta's per-projectile-instance Random; see
// DISPENSER_SLICE_NOTES.md) or a generic EntityItem (one nextDouble then three
// nextGaussians on s->rand_src, in that order). No-op (does nothing, no SFX)
// if no dispenser TE is registered at (x,y,z). [decompile 740c583:
// BlockDispenser.dispenseItem]
void server_local_dispenser_dispense(struct server_local* s, w_coord_t x,
									 w_coord_t y, w_coord_t z,
									 uint8_t metadata);
// Dispenser-slice: BlockDispenser.onBlockRemoval's item scatter -- for each
// occupied slot, one fixed (x,y,z) offset (3x nextFloat*0.8+0.1), then a
// while(stackSize>0) loop splitting off nextInt(21)+10 units per spawned
// EntityItem, each with its OWN nextGaussian*0.05 velocity (Y gets +0.2). Uses
// s->dispenser_break_rng (the block-class-scoped `this.random`, distinct from
// both s->rand_src and the chest/furnace container_drop_rng). Best-effort: an
// entity-allocation failure mid-scatter drops only that one sub-stack (the
// server_local_flush_arrows precedent), not the whole scatter -- Beta itself
// has no retry concept here either. [decompile 740c583:
// BlockDispenser.onBlockRemoval]
void server_local_dispenser_scatter_contents(
	struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z,
	const struct item_data items[DISPENSER_TE_SLOTS]);
// D6: if the dispenser's own GUI is open at this position, mark it
// invalidated and clear the window's visible storage slots (a no-op
// otherwise). Does NOT spawn anything -- block_dispenser.c's onBreak calls
// server_local_dispenser_scatter_contents separately and unconditionally so
// window-open and window-closed breaks produce the identical Beta-faithful
// scatter. See the definition (server_local.c) for the full citation.
void server_local_dispenser_invalidate_open_window(struct server_local* s,
												   w_coord_t x, w_coord_t y,
												   w_coord_t z);
// Noteblock-slice: TileEntityNote.triggerNote, called from block_noteblock.c's
// onRightClick (after changePitch) and onNeighbourBlockChange (redstone rising
// edge). Gates on the cell ABOVE being air, classifies the instrument from the
// material BELOW, computes the pitch from the TE's current note value, and
// emits the sound via server_local_emit_sound. No-op if no note TE is
// registered at (x,y,z). [decompile 740c583: TileEntityNote.triggerNote /
// BlockNote.playBlock]
void server_local_noteblock_trigger(struct server_local* s, w_coord_t x,
									w_coord_t y, w_coord_t z);
// Jukebox-slice: BlockJukeBox.ejectRecord (misnamed in Beta -- it actually
// INSERTS): sets the TE's record to record_item_id, flips the block metadata
// to 1, and fires world event 1005 (data = record_item_id) via
// CRPC_RECORD_EVENT. No-op if no jukebox TE is registered at (x,y,z). Called
// from item_record.c's onUseOnBlock. [decompile 740c583:
// BlockJukeBox.ejectRecord / ItemRecord.onItemUse]
void server_local_jukebox_insert(struct server_local* s, w_coord_t x,
								 w_coord_t y, w_coord_t z,
								 uint16_t record_item_id);
// Jukebox-slice: BlockJukeBox.func_28038_b_ -- the shared EJECT (despite
// ejectRecord's name being the insert half above): a no-op if the TE holds no
// record; otherwise fires world event 1005 data 0 (stop) via
// CRPC_RECORD_EVENT, clears the TE + block metadata, then spawns the ejected
// record EntityItem at the exact offset (jukebox_eject_offset) with 3
// sequential world-rand draws. Shared by the right-click eject
// (block_jukebox.c's onRightClickHandled) and the onBreak teardown. [decompile
// 740c583: BlockJukeBox.func_28038_b_]
void server_local_jukebox_eject(struct server_local* s, w_coord_t x,
								w_coord_t y, w_coord_t z);
// In-world drop of the held HOTBAR item (Beta EntityPlayer.dropCurrentItem ->
// dropPlayerItemWithRandomChoice(decrStackSize(currentItem, n), false)): removes n from
// the current hotbar slot (n=1 when all=false, the whole stack when all=true), notifies
// the client, and spawns the thrown EntityItem at the Beta eye-0.18 offset along the
// player's look. Returns the spawned entity (NULL if the slot was empty). Factored out of
// the SRPC_DROP_ITEM handler so the golden test can drive it directly.
struct entity* server_local_drop_hotbar_item(struct server_local* s, bool all);
/* Enter Beta's sleeping state and immediately synchronize the client freeze. */
void server_local_begin_sleep(struct server_local* s, w_coord_t head_x,
							  w_coord_t head_y, w_coord_t head_z);
/* One tick of the sleeping state machine (P4(b)); called from the world tick, exposed so
 * the golden test can drive it deterministically. */
void server_local_sleep_tick(struct server_local* s);
/* Send a HUD toast (P4(a)); counts sends so tests can assert emission without a GUI. */
void server_local_send_hud_message(struct server_local* s, const char* msg);
uint32_t server_local_hud_messages_sent(void);
struct entity* server_local_spawn_monster(vec3 pos, int monster_id,
									   struct server_local* s);
// Exact-position spawn (no block-center snap) -- the slime split children's
// sub-block grid offsets must survive; natural spawn cells use the wrapper.
struct entity* server_local_spawn_monster_at(vec3 pos, int monster_id,
											 struct server_local* s);
/* ===== A1b world-explicit effect contracts (split-dimensions D4/D9) ======
 * The caller supplies the world that PRODUCES the effect (a tile's own
 * world, never an implicit current world). The legacy names above keep their
 * signatures (~145 callers in other lanes) and forward &s->worlds[0].
 * - spawn_monster_at_w: exact-position spawn bound to `w`, session-global
 *   unique id; NULL for a non-live world.
 * - emit_sound_w / emit_sound_ui_w: validated against `w` (and, for UI, the
 *   recipient actor must be committed to `w`). Until B1's world/recipient
 *   routing lands only slot 0 has a client route; any other world's effect
 *   is dropped rather than played in the wrong dimension. */
struct server_player;
struct server_world;
struct entity* server_local_spawn_monster_at_w(struct server_world* w,
											   vec3 pos, int monster_id);
// True when `w` may publish one-shot effects (sound, smoke): slot 0 or a live
// A1b world. server_local_emit_sound_w applies the same gate.
bool server_local_world_fx_ok(const struct server_world* w);
void server_local_emit_sound_w(struct server_world* w, uint16_t sound_id,
							   vec3 pos, float vol, float pitch);
void server_local_emit_sound_ui_w(struct server_world* w,
								  const struct server_player* actor,
								  uint16_t sound_id, float vol, float pitch);

/* ===== A1c world operations (split-dimensions-plan-0924, "A2: A1c") ========
 * The per-tick world services server_local_update runs -- the entity pass,
 * the item onUpdate pass, the natural spawn passes, the dungeon-spawner pass,
 * the skylight recompute, the weather tick/effects/bolt sweep
 * (server_weather.h) and the near-unload send -- are callable per EXPLICIT
 * `struct server_world* w`. The legacy `(s)` spellings that other files still
 * call are the inventoried slot-0 adapters: `_w(s, &s->worlds[0])`,
 * byte-identical to their pre-A1c bodies (the single-world goldens hold).
 *
 * ELIGIBLE-ACTOR CONTRACT (lane C consumes this for #19/#20 despawn and
 * wolf interest, #2 lightning, sleep quorum): an actor is IN world `w` iff
 * its committed world is `w` (server_local_actor_in_world). Every world
 * service considers ONLY the eligible actors of its world -- spawn centres,
 * the spawner scan, the world-spawn/player-distance exclusions, the struck-
 * entity pass and the cave-sound distance. A world with NO eligible actor
 * runs no spawn attempt and draws nothing (Beta: no player -> no eligible
 * chunk). Today both bodies are committed to slot 0 (A2 gives each actor its
 * own world handle); this predicate is the one place that changes then.
 *
 * ENTITY-WORLD CONTRACT (lane B producer identity, A2-E successor): an
 * entity belongs to the world it was constructed into (`entity->world`, the
 * A1a D1 binding); server_local_entity_world resolves it only while that
 * world instance is live (slot + generation), else NULL. The entity pass
 * for `w` ticks exactly the entities bound to `w`; a mid-pass join is ticked
 * by the pass in flight only if it is bound to `w`.
 *
 * SKYLIGHT: the cached skylightSubtracted (s->skylight_*) belongs to the
 * routed world (slot 0); `_w` on another world computes that world's value
 * (Nether 11) without touching the cache. */
#define SERVER_LOCAL_A1C_WORLD_OPS 1
struct entity;
/* True iff `w` is a live world instance of its session (slot bound, chunks
 * constructed, generation non-zero) -- the shared liveness predicate. */
/* G9 R3: true once any world of the session stopped its light (a failed
 * reservation or a stopped drain): nothing more is published or saved. */
bool server_local_light_stopped(const struct server_local* s);
bool server_local_world_live(const struct server_world* w);
/* WORLD-RETIRE-SRV (Fable B2-A2T F1, the server producer of the client's
 * CRPC_WORLD_RELEASE {retiring=false}; Astra A2 plan "LIVE with zero resident columns"): for
 * every LIVE world other than the routed slot 0 with NO eligible actor, save
 * and evict its resident columns (a bounded batch per call; a failed or
 * deferred save retries on the next call), and once it holds zero columns
 * send ONE stamped CRPC_WORLD_RELEASE {retiring=false} {world_index, generation} so the client
 * releases that slot's payload. The world stays LIVE: generation, clocks,
 * tick queue and tile metadata are untouched; nothing server-side is freed
 * beyond the ordinary eviction. Re-arms when an actor is committed to it
 * again. Called once per server update; slot 0 is never vacated before A2-T
 * gives actors their own world handles (lane A wires it then). */
#define SERVER_LOCAL_WORLD_RETIRE_SRV 1
void server_local_retire_vacant_worlds(struct server_local* s);
/* The live world whose dimension is `dimension`, or NULL. Slot 0 answers for
 * its dimension whether or not its chunks are constructed yet (the legacy
 * scheduler semantics for the routed world); other slots must be live. */
struct server_world* server_local_world_for_dimension(struct server_local* s,
													  int32_t dimension);
struct server_world* server_local_entity_world(struct server_local* s,
											   const struct entity* e);
/* A2-E (M6): resolve an entity REFERENCE held by `from` (a jockey mount/rider
 * id, a target id) to its entity only while both resolve to the SAME live
 * world -- a reference into another world, or into a retired generation, is
 * NULL: it can be released but never read or moved through. */
#define SERVER_LOCAL_A2E_ENTITY_REF 1
/* ===== A2-T / A2-W seams consumed by server_transfer.c ================== */
/* The stamped, never-blocking client send (B1): `call` carries
 * w->world_index / w->generation; false when the transport refused. */
struct client_rpc;
/* Entity-module events keep legacy bare bytes when world payloads are OFF.
 * In opt-in mode, only the entity's live owning generation can publish. */
void server_local_send_entity(struct server_local* s, const struct entity* e,
	struct client_rpc call);
/* G8-VEHICLE-SEAMS-01: the never-blocking twin of server_local_send_entity.
 * Payloads OFF: the legacy bare clin_try_rpc_send. ON: stamped with the
 * entity's live owning world; a stale entity publishes nothing and reports
 * true (dropped on purpose, exactly as server_local_send_entity drops it), so
 * a caller's retry bookkeeping never spins on a world that is gone. False
 * only when the transport refused. */
bool server_local_try_send_entity(struct server_local* s,
	const struct entity* e, struct client_rpc call);
/* The session owning the world an entity was constructed into (NULL when the
 * entity is unbound) -- for entity-module producers that hold no session. */
struct server_local* server_local_of_entity(const struct entity* e);
/* G8-VEHICLE-SEAMS-01: Entity.mountEntity's single-rider invariant as one
 * step -- release every pig/boat/cart relation naming `player`'s lifetime
 * (except the vehicle being mounted) and zero its steering input. Used by all
 * three mount paths and by the committed lifecycle boundaries (Leave/release,
 * death, transfer COMMIT) before the body or its generation is invalidated. */
void server_local_release_actor_mounts(struct server_local* s,
	struct server_player* player, const struct entity* except);
/* True when every riding flag on `player` is backed by a live vehicle whose
 * relation names this lifetime, wire id and world (the detached guest
 * snapshot's precondition; a flag without such a vehicle is corruption). */
bool server_local_actor_mounts_valid(struct server_local* s,
	const struct server_player* player);
/* The boat, cart or pig `player` validly rides (relation names its lifetime, wire
 * id and world), or NULL when it rides none. */
const struct entity* server_local_actor_vehicle(struct server_local* s,
	const struct server_player* player);
bool server_local_try_send_w(const struct server_world* w, struct client_rpc call);
/* One nonblocking, world-stamped spawn snapshot (true also for entity types
 * without a client spawn representation). */
bool server_local_try_publish_entity_w(struct server_local* s,
	const struct server_world* w, struct entity* e);
/* Drop one world slot's entity section lists (a retiring / re-created slot). */
void server_local_entity_sections_reset_slot(struct server_local* s, uint8_t slot);
/* The respawn plan (bed validated at respawn time, else the world spawn) and
 * the fresh-body reset Beta's new EntityPlayer implies. Both are the legacy
 * respawn's own steps, exported so a respawn from a non-routed world runs the
 * identical body reset. */
struct server_respawn_plan {
	double x, y, z;
	bool keep_bed_spawn;
};
bool server_local_prepare_respawn_plan(struct server_local* s,
									   struct server_respawn_plan* plan);
void server_local_respawn_fresh_body(struct server_local* s,
									 const struct server_respawn_plan* plan);
/* Item drop bound to an EXPLICIT live world (the death world of an actor
 * committed outside slot 0). NULL for a non-live world or on allocation
 * failure (nothing consumed). Publishes only where `w` has a client route. */
struct entity* server_local_spawn_item_w(struct server_world* w, vec3 pos,
										 struct item_data* it, bool throw);
struct entity* server_local_entity_ref(struct server_local* s,
									   const struct entity* from, uint32_t id);
/* A2-E review F2: an entity id named by an ACTOR's action (attack, use,
 * ride) resolves only while the entity resolves to a live world the actor is
 * committed to (server_local_actor_in_world); another world's or a retired
 * generation's body is NULL. */
struct entity* server_local_actor_entity_ref(struct server_local* s,
											 const struct server_player* actor,
											 uint32_t id);
bool server_local_actor_in_world(const struct server_local* s,
								 const struct server_player* actor,
								 const struct server_world* w);
/* Host first, then the guest body; only the actors committed to `w`. */
size_t server_local_eligible_actors_w(struct server_local* s,
									  struct server_world* w,
									  struct server_player** out, size_t cap);
/* G10: scheduled ticks then random ticks for one world, Beta World.tick
 * order; returns the random pass microseconds. */
int server_local_world_block_ticks_w(struct server_local* s,
	struct server_world* w, bool anchored, w_coord_t px, w_coord_t pz);
void server_local_skylight_recompute_w(struct server_local* s,
									   struct server_world* w);
int server_local_skylight_subtracted_w(const struct server_local* s,
									   const struct server_world* w);
int server_local_block_light_value_sub_w(struct server_local* s,
										 struct server_world* w, w_coord_t x,
										 w_coord_t y, w_coord_t z,
										 int subtract);
int server_local_block_light_value_w(struct server_local* s,
									 struct server_world* w, w_coord_t x,
									 w_coord_t y, w_coord_t z);
int server_local_hostile_spawn_light_w(struct server_local* s,
									   struct server_world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z);
/* Cell-centre monster spawn into `w` (the `_at_w` with the natural-spawn
 * snap); NULL for a non-live world. */
struct entity* server_local_spawn_monster_w(struct server_world* w, vec3 pos,
											int monster_id);
/* Residency (PLAN-v3 s.2: anchors are (dimension,x,z)): the near-owner
 * dependency query for one explicit world. */
bool server_local_residency_dependency_w(struct server_local* s,
										 struct server_world* w, int32_t x,
										 int32_t z);

/* ===== A1b tile-window binding + world-qualified tile rows ================
 * A tile window (chest, double chest, furnace, dispenser, sign) is bound to
 * the world it was opened in (server_player.window_world, stamped by the
 * open path before on_create). server_local_window_world resolves it, NULL
 * when the bound instance is gone (stale generation) or nothing is bound;
 * every backing lookup goes through it (the window/row helpers are static
 * inline in server_local_state.h). The *_in open forms name the opening
 * world; the existing *_for forms are the inventoried slot-0 adapters.
 * server_local_chest_row_w / _sign_row_w: occupied row owned by `w` at
 * (x,y,z), or -1. server_local_tile_storage_bytes: the shared tile storage
 * (fixed tables + owner keys + actual piston capacity) the session charges. */
struct inventory;
struct inventory_logic;
bool server_local_open_inventory_in(struct server_local* s,
	struct server_world* w, struct server_player* player,
	struct inventory_logic* logic, size_t capacity, w_coord_t x, w_coord_t y,
	w_coord_t z, uint8_t window, uint8_t type);
bool server_local_open_linked_inventory_in(struct server_local* s,
	struct server_world* w, struct server_player* player,
	struct inventory_logic* logic, size_t capacity, w_coord_t x, w_coord_t y,
	w_coord_t z, w_coord_t x2, w_coord_t y2, w_coord_t z2, uint8_t window,
	uint8_t type);
size_t server_local_tile_storage_bytes(const struct server_local* s);
// Deferred mid-tick monster spawn (slime death split): queued beside the
// pending arrows, flushed right after them once the entity tick loop is done.
/* A2-E: queue a split/hatch child into `w` (the spawning entity's resolved
 * world); NULL `w` queues nothing. The `(s, pos, id)` form is the slot-0
 * adapter. The flush constructs into the stamped world while it is live and
 * drops the job once that generation is retired. */
void server_local_queue_monster_w(struct server_local* s,
								  struct server_world* w, vec3 pos,
								  int monster_id);
void server_local_queue_monster(struct server_local* s, vec3 pos,
								int monster_id);
// Harden audit #3: O(1) fresh entity id from the monotonic next_entity_id counter
// (pre-increment so the first id is 1; 0 is the local player). Replaces the O(N)
// entity_gen_id full-dict scan at the server spawn sites. Unique + > 0 + monotonic
// in deterministic spawn order; consumes no RNG.
uint32_t server_local_gen_id(struct server_local* s);
bool server_local_gather_chunk_entities(struct server_local* s, w_coord_t x,
										w_coord_t z,
										struct chunk_entities* out);
/* The same for the chunk (x,z) OF `w`: only entities bound to `w`
 * (server_local_entity_world) are gathered -- a coordinate names a column in
 * every world. The legacy name gathers slot 0's. */
bool server_local_gather_chunk_entities_w(struct server_local* s,
										  struct server_world* w, w_coord_t x,
										  w_coord_t z,
										  struct chunk_entities* out);
bool server_local_restore_chunk_entities(struct server_local* s,
										 const struct chunk_entities* entities);
/* S5: admit ONE nonblocking CRPC_ENTITY_DESTROY_BATCH for this chunk's whole
 * dynamic-entity set, then retire those server entities. False means the
 * client inbox refused the notification: nothing was transferred, nothing was
 * freed, and the caller must retain every owner and retry. See the .c. */
bool server_local_try_release_chunk_entities(
	struct server_local* s, const struct chunk_entities* entities);
/* B1 local stamping: the same, naming the world the chunk belongs to -- the
 * destroy batch / restored spawns are stamped with w's identity. The legacy
 * names above use slot 0. */
bool server_local_try_release_chunk_entities_w(
	struct server_local* s, struct server_world* w,
	const struct chunk_entities* entities);
/* stage22-0927: whether an eviction of a chunk of `w` that holds live
 * entities can admit everything it will send this tick -- the entity-destroy
 * batch and (slot 0) the chunk unload. Checked BEFORE the record is written:
 * without it the record committed, the batch was then refused on a full
 * client inbox, and the committed write was thrown away (real-Wii
 * split_play_8m, save_fail stage 22). */
bool server_local_evict_headroom_w(struct server_local* s,
								   const struct server_world* w);
bool server_local_restore_chunk_entities_w(struct server_local* s,
										   struct server_world* w,
										   const struct chunk_entities* entities);
/* B2 gate 1 producer (see the .c): the in-place guest-world CRPC_WORLD_RESET
 * for world `w` (its slot + new generation); guest_view 1 = the guest's first
 * move into that slot. A2 calls it, then re-issues the guest's
 * LOCAL_GUEST_GAMEPLAY_READY. */
void server_local_send_guest_world_reset(struct server_local* s,
										 const struct server_world* w,
										 uint8_t guest_view);
/* B2 gate 1 join producer (see the .c): the guest moves back INTO player 0's
 * current world (bind only, no re-generation). A2 calls it, then re-issues
 * the guest's LOCAL_GUEST_GAMEPLAY_READY. */
void server_local_send_guest_join_host_world(struct server_local* s);
/* Java's Math.random() is one process-global java.util.Random shared by every
 * caller. EntityItem and EntityTNTPrimed constructors both draw from it; keep
 * the state independently rollbackable from World.rand for transactions which
 * prepare an EntityItem before publishing it. */
struct server_local_math_random_state {
	struct java_random rng;
	bool initialized;
};
void server_local_math_random_capture(
	struct server_local_math_random_state* out);
void server_local_math_random_restore(
	const struct server_local_math_random_state* state);
/* Advance Java's process-global Math.random stream for simulation effects
 * whose particles/audio are not rendered server-side but whose RNG ownership
 * remains observable by later EntityItem/TNT constructor physics. */
void server_local_math_random_discard(size_t double_draws);
/* A populate-time fluid may displace a droppable block before its quadrant is
 * committed. Record the exact item/three World.rand-derived offsets and own a
 * detached EntityItem allocation without inserting it or sending a client RPC.
 * The four independent Math.random constructor draws are deferred until the
 * no-fail publish after capacity preflight. */
struct server_local_planned_drop {
	struct item_data item;
	float pos[3];
	/* Detached allocation owned by the quadrant transaction until commit. */
	struct entity* reserved_entity;
};
typedef bool (*server_local_planned_drop_sink)(
	void* ctx, const struct server_local_planned_drop* drop);
bool server_local_plan_block_drops(
	struct server_local* s, struct block_info* blk_info,
	server_local_planned_drop_sink sink, void* sink_ctx);
/* F4-RNG: the same plan against an EXPLICIT world (its World.rand). */
struct server_world;
bool server_local_plan_block_drops_w(
	struct server_world* w, struct block_info* blk_info,
	server_local_planned_drop_sink sink, void* sink_ctx);
void server_local_cancel_planned_drop(
	struct server_local_planned_drop* drop);
/* Reserve every dictionary/live-tick slot while the quadrant undo journal is
 * still available. Failure changes no reservation count and is rollback-safe. */
bool server_local_reserve_planned_drop_batch(
	struct server_local* s, size_t count);
/* Requires the successful batch reservation above. Assigns IDs, inserts and
 * publishes in order with no allocation and no remaining failure point. */
/* A1a: the items join world `w` (session via w->session). */
void server_local_commit_planned_drop_batch(
	struct server_world* w, struct server_local_planned_drop* drops,
	size_t count);
void server_local_spawn_block_drops(struct server_local* s,
									struct block_info* blk_info);
/* Beta drop operation in the explicit affected world; includes special
 * overrides (moving pistons delegate with chance 1) before block removal. */
void server_local_spawn_block_drops_with_chance(struct server_world* w,
	struct block_info* blk_info, float chance);
// Drop-only half for callers that already invoked onBreak. This is required
// for Beta's explosion order (dropBlockAsItemWithChance, then block removal)
// and prevents player digging from running container teardown twice.
void server_local_spawn_block_drops_after_break(
	struct server_local* s, struct block_info* blk_info);
// Beta Entity.moveEntity footstep accumulator: distance_walked += sqrt(dx^2+dz^2)*0.6; returns
// true on the one update it crosses next_step_distance with a solid block underfoot (one footstep
// -> Block.onEntityWalking, i.e. the farmland trample). Extracted for unit testing the cadence.
bool server_player_walk_step(float* distance_walked, int* next_step_distance, double dx, double dz,
							 bool solid_below);
// R1: the block half of that footstep. Call ONLY on a server_player_walk_step true edge, after
// the step sound, with the foot cell (x, floor(feetY - 0.2), z). Beta substitutes the fence id
// when the cell BELOW is a fence (fence has no override -> no-op), else dispatches the foot
// block's onEntityWalking at the FOOT coordinates. [740c583: Entity.java:441-457]
/* A2-T D4: the same against the walker's world `w` (NULL = the dispatch
 * world). The plain form is the NULL spelling. */
struct server_world;
void server_local_entity_walked_on_block_w(struct server_local* s,
										   struct server_world* w, w_coord_t x,
										   w_coord_t y, w_coord_t z,
										   enum block_walker walker);
void server_local_entity_walked_on_block(struct server_local* s, w_coord_t x, w_coord_t y,
										 w_coord_t z, enum block_walker walker);
/* Beta Chunk membership/query ordering used by piston moveEntity. Refresh is
 * called immediately after an authoritative position mutation. Preparing a
 * snapshot captures comparator keys before the first candidate moves. */
void server_local_refresh_entity_membership(struct server_local* s,
										struct entity* e);
void server_local_refresh_player_membership(struct server_local* s);
/* The bound guest body's physical section membership (its own world). */
void server_local_refresh_guest_membership(struct server_local* s);
/* v5: the body a player query ref names (host / admitted guest), or NULL. */
struct server_entity_query_ref;
struct server_player* server_local_query_ref_actor(struct server_local* s,
	const struct server_entity_query_ref* ref);
/* Physical presence (host / bound guest), including an offer before gameplay
 * admission. Only physical collision/piston consumers use this resolver;
 * damage, targeting and ordinary impulses retain query_ref_actor(). */
struct server_player* server_local_query_ref_body(struct server_local* s,
	const struct server_entity_query_ref* ref);
/* next-gaps-0924/W2 P4b: the LOCAL Beta entity query -- Chunk.entities[]
 * section lists (server_local_state.h entity_sections) walked in Beta's
 * traversal order (chunk X asc, chunk Z asc, section asc, insertion order)
 * for the chunk/section span; fills the same entity_query_refs buffer the
 * exhaustive snapshot fills, in the same order (equivalence:
 * tests/test_entity_query_local_equiv.c). False on a buffer allocation
 * failure (count 0). */
bool server_local_entity_query_local(struct server_local* s, int32_t cx0,
									 int32_t cz0, int32_t cx1, int32_t cz1,
									 int32_t cy0, int32_t cy1);
/* A2-E: the same local query over ONE world's section index (Beta: a
 * World's chunks). The host body is a candidate only in its own world. The
 * `(s, ...)` form above is exactly `_w(s, &s->worlds[0])`. A `w` that is not
 * a world of this session answers empty. */
bool server_local_entity_query_local_w(struct server_local* s,
									   struct server_world* w, int32_t cx0,
									   int32_t cz0, int32_t cx1, int32_t cz1,
									   int32_t cy0, int32_t cy1);
/* Drops every section list (both world resets restart entity ids at 1). */
void server_local_entity_sections_reset(struct server_local* s);
bool server_local_prepare_entity_query_snapshot(struct server_local* s);
/* Same snapshot restricted to Beta's chunk walk: World.getEntitiesWithinAABB
 * visits only chunks floor((minX-2)/16)..floor((maxX+2)/16) (x and z), so a
 * box query never pays for -- or sorts -- entities elsewhere in the world.
 * Membership chunk coords are the entity's Beta Chunk.addEntity tuple.
 * [740c583 World.getEntitiesWithinAABBExcludingEntity] */
/* A2-E: the exhaustive ordered reference per world (entities resolving to
 * `w`; the host body only in its own world). The `(s, ...)` forms are the
 * slot-0 adapters. NULL `w`: an empty answer. */
bool server_local_prepare_entity_query_snapshot_bounded_w(
	struct server_local* s, struct server_world* w, int32_t cx0, int32_t cz0,
	int32_t cx1, int32_t cz1, int32_t cy0, int32_t cy1);
bool server_local_prepare_entity_query_snapshot_bounded(
	struct server_local* s, int32_t cx0, int32_t cz0, int32_t cx1, int32_t cz1,
	int32_t cy0, int32_t cy1);
const struct server_entity_query_ref* server_local_entity_query_snapshot(
	const struct server_local* s, size_t* count);
// ===== M5 portal travel (exported like walk_step for the goldens) =====
// Beta EntityPlayer.setInPortal: cooldown>0 -> refresh to 10; else arm the ramp.
void server_player_set_in_portal(struct server_player* p);
// The per-tick dwell machine (EntityPlayerSP.onLivingUpdate portal half):
// consume in_portal (ramp +0.0125F to >=1.0F -> latch portal_travel_requested
// + cooldown 10; else decay -0.05F), then decrement the cooldown.
void server_player_portal_tick(struct server_player* p);
/* Consume prior contact with Beta EntityPlayerMP's mounted dismount branch. */
void server_local_actor_portal_tick(struct server_local* s,
	struct server_player* p);
// The factored SRPC_UNLOAD_WORLD / SRPC_LOAD_WORLD bodies (see the impl
// comments for the persistence-keying rule), and the live dimension switch
// (Beta Minecraft.usePortal server-side: toggle dim, scale x/z by 8, world
// round-trip, carry fire/air, Teleporter placement). Call ONLY from the
// server tick top -- the switch destroys the world.
// A0: unload returns true iff no world remains held afterwards (the release
// committed); false means it refused (guest attached, inventory close or
// checkpoint failed) and the world is still fully live -- a caller that goes
// on to load MUST branch on it. The SRPC_UNLOAD_WORLD body itself needs no
// branch: every refusal already names itself to the host (HUD + RETURN_INGAME).
// Load returns false when no world was loaded (and never loads over a live one).
bool server_local_unload_world_impl(struct server_local* s, bool send_reset);
__attribute__((warn_unused_result))
bool server_local_load_world_impl(struct server_local* s, const string_t name);

/* R3: Beta's new-world spawn search (World.getInitialSpawnLocation) on a
 * temporary live slot-0 world of `s`, loading and populating chunks in
 * ChunkProviderServer.loadChunk order with *world_rand as World.rand (the
 * population's lava-mix fizz draws land on it). The probed chunks persist to
 * `dir` with their TerrainPopulated state. Requires no loaded world. False =
 * chunk generation failed (no spawn; the caller must not create the world). */
bool server_local_beta_spawn_search(struct server_local* s, const char* dir,
									int64_t seed,
									struct java_random* world_rand, int* out_x,
									int* out_z);
/* Route worldgen_find_initial_spawns[_rng] through the search above for the
 * world being created at `dir` (NULL, NULL uninstalls). */
void server_local_install_spawn_search(struct server_local* s,
									   const char* dir);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Test seam: called with the search world right after its World.rand was
 * set (begin = true) and right after the walk ended (begin = false). */
extern void (*server_local_test_spawn_search_phase)(struct server_world* w,
													bool begin);
#endif

enum server_local_save_migrate_state {
	SERVER_SAVE_MIGRATE_IDLE = 0,
	SERVER_SAVE_MIGRATE_FIXTURE,
	SERVER_SAVE_MIGRATE_RELOAD,
	SERVER_SAVE_MIGRATE_VERIFY,
	SERVER_SAVE_MIGRATE_PASS,
	SERVER_SAVE_MIGRATE_FAIL,
};

struct server_local_save_migrate_metrics {
	uint32_t state;
	uint32_t elapsed_ms;
	uint32_t regions_scanned;
	uint32_t chunks_scanned;
	uint32_t signs_rewritten;
	uint32_t signs_expected;
	uint32_t signs_verified;
	// Save version 3 (FH-07 chain link): bare-95 iron-chest blocks the fixture
	// planted, converted to BLOCK_IRON_CHEST by the SAME production reopen.
	uint32_t chests_rewritten;
	uint32_t chests_expected;
	uint32_t chests_verified;
	// Save version 4 (FH-17 chain link): legacy metadata-encoded spawner
	// blocks the fixture planted, given a synthesized MobSpawner TE.
	uint32_t spawners_synthesized;
	uint32_t spawners_expected;
	uint32_t spawners_verified;
};

void server_local_save_migrate_metrics_read(
	struct server_local_save_migrate_metrics* out);
void server_local_portal_travel(struct server_local* s);
// Advances one bounded destination-search slice. Returns true when no portal
// job remains (success or rollback), false while work is still pending.
bool server_local_portal_travel_step(struct server_local* s);
#ifdef CAVEWII_SERVER_LOCAL_TEST
// Fail the next N session populate-scratch reservations. Passing 2 models
// both the eager init attempt and the pre-world load retry returning OOM.
void server_local_test_fail_populate_scratch_allocs(unsigned count);
// One-shot archive-open failure used to prove portal rollback and safe load
// failure behavior without depending on host filesystem races.
bool server_local_test_save_player_state(struct server_local* s);
void server_local_test_fail_next_world_load(void);
void server_local_test_fail_world_loads(unsigned count);
// Fail the portal transaction after every destination chunk is durable but
// before the destination Player tag is written. Models a power loss at the
// chunks-first transaction boundary.
void server_local_test_fail_next_portal_after_chunk_checkpoint(void);
// Fail the NEXT region durability barrier (the fsync boundary that must gate
// death-journal removal and the portal destination Player commit). Proves the
// barrier is a GATE, not merely present.
void server_local_test_fail_next_region_sync(void);
// Number of times the production portal path reached the destination Player
// write. Lets the chunk-failure regression prove the write was never attempted.
unsigned server_local_test_portal_player_commit_attempts(void);
// Fail a chunk-entity restore after this many successful entity allocations.
// The one-shot seam resets itself when it fires.
void server_local_test_fail_entity_restore_after(unsigned count);
// Fail the next entity object allocation after reserving its dictionary id.
// Drives no-loss inventory-close tests through the real spawn failure path.
void server_local_test_fail_next_entity_alloc(void);
void server_local_test_fail_entity_alloc_after(unsigned count);
// Fail dictionary-capacity preflight before a detached TNT mutates its block.
void server_local_test_fail_next_entity_dict_reserve(void);
// Fail only if the production tombstone threshold requests a dictionary rehash.
void server_local_test_fail_next_entity_dict_realloc(void);
size_t server_local_test_planned_drop_publish_total(void);
#ifdef CAVEWII_FAR_SLOT_DEADLOCK_TEST
/* Require this many successful near sends before the elapsed-time batch gate
 * (CHUNK_SEND_BUDGET_MS) may stop a manual-drive tick. Zero restores shipping
 * scheduling. Originally added because a sanitizer-instrumented send can
 * exceed the shipping wall-clock budget by itself; the identical mechanism
 * -- wall-clock time is not a reliable governor once something OTHER than
 * real per-chunk work (ASan overhead, or here, OS scheduler preemption under
 * parallel-ctest/build CPU contention) inflates the reading -- also root-
 * caused the test_far_stream_waits_for_near_completion flake in
 * test_fluid_lifecycle_env.c: passing server_local_test_chunk_send_batch()
 * removes CHUNK_SEND_BUDGET_MS from that convergence loop entirely, making
 * it depend only on deterministic per-call counts. */
void server_local_test_set_near_send_minimum(unsigned count);
void server_local_test_expire_near_budget(bool expired);
// The shipping CHUNK_SEND_BATCH cap, for a caller that needs to pass the
// exact "always allow the full batch" value to server_local_test_set_near_
// send_minimum above without duplicating the constant (and tripping its
// assert(count <= CHUNK_SEND_BATCH) should the two ever drift).
unsigned server_local_test_chunk_send_batch(void);
/* Invoke the production fixture, including its synchronous completion hold. */
void server_local_test_light_queue_stress_drive(struct server_local* s);
void server_local_test_light_queue_stress_demo(struct server_local* s);
void server_local_test_stress_checkpoint_retry_hook(
	void (*hook)(struct server_local*));
#endif
#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
// Near-send backoff instrumentation (root-caused the test_far_stream_waits_
// for_near_completion flake -- see server_local.c's NEAR_SEND_FAIL_STREAK_
// THRESHOLD doc comment). mark_calls counts every backoff
// insertion/renewal; first_time counts only ones that were a chunk's FIRST
// failure (a fresh table entry); wasted_calls counts server_local_update()
// calls where the near-send scan found nothing to even attempt while the
// backoff table held at least one entry -- i.e. calls where the backoff
// itself, not real generation work, produced zero near-send progress.
void server_local_test_near_fail_counters_reset(void);
void server_local_test_near_fail_counters(
	unsigned* mark_calls, unsigned* first_time, unsigned* wasted_calls);
// zero_progress_calls counts every update() call that sent zero near chunks,
// for ANY reason (backoff-table exclusion, the CHUNK_SEND_BUDGET_MS
// wall-clock batch cap, a blocked chunk-slot/RPC-token send, ...);
// total_sent/calls gives the observed near-chunks-per-call throughput.
void server_local_test_near_throughput_counters(
	unsigned* zero_progress_calls, unsigned* total_sent);
// Cumulative (across the whole convergence attempt, not just the last call)
// breakdown of which near-send loop-exit condition fired: nothing left to
// try, NEAR_SEND_ATTEMPTS_MAX reached, chunk-slot pool busy, or the
// CHUNK_SEND_BUDGET_MS wall-clock gate itself (should read 0 under the
// test's near-send-minimum seam), plus a running total of ensure_populated()
// failures across every candidate/attempt. Found the SECOND residual
// mechanism behind the near-ring convergence flake (see
// s_near_exit_no_candidate's doc comment, server_local.c) -- a "no_candidate"
// count near the total call count WITH ensure_populated_fails==0 means the
// near-send scan and the caller's own "is it sent" check are looking at
// DIFFERENT rings (a player-position desync), not a genuinely stuck chunk.
void server_local_test_near_exit_counters(unsigned* no_candidate,
										   unsigned* attempts_max,
										   unsigned* slot_busy,
										   unsigned* budget,
										   unsigned* ensure_populated_fails);
#endif
void server_local_test_seed_math_random(int64_t seed);
// Corrupt only the live copy of the next PREPARED plan before close. The disk
// copy remains valid, exercising the invariant-failure recovery escape hatch.
void server_local_test_diverge_next_death_close(void);
// Fail one non-empty chunk entity gather before allocation; proves save/unload
// retains the live dictionary when an entity snapshot cannot be built.
void server_local_test_fail_next_entity_gather(void);
/* Drive exactly the production entity pass, including mid-pass appends. False
 * means scratch growth failed and no pre-existing entity was partially ticked. */
bool server_local_test_tick_entities(struct server_local* s);
/* G7: the production per-actor Q-drop and SRPC_WINDOW_DROP cursor spill. */
struct entity* server_local_test_drop_hotbar_item_for(struct server_local* s,
	struct server_player* player, bool all);
struct entity* server_local_test_window_drop_spawn(struct server_local* s,
	struct server_player* player, struct item_data* dropped);
/* A1c: the same world services per EXPLICIT world (the `(s)` hooks above and
 * below are their slot-0 adapters). tick_spawners_w reports the tracker rows
 * owned by `w` after the pass; near_unload_send_w is the raw per-world
 * CRPC_UNLOAD_CHUNK try-send (true = sent or nothing to send). */
bool server_local_test_tick_entities_w(struct server_local* s,
									   struct server_world* w);
void server_local_test_tick_item_updates(struct server_local* s);
void server_local_test_tick_item_updates_w(struct server_local* s,
										   struct server_world* w);
void server_local_test_spawn_mobs_w(struct server_local* s,
									struct server_world* w);
void server_local_test_spawn_animals(struct server_local* s);
void server_local_test_spawn_animals_w(struct server_local* s,
									   struct server_world* w);
void server_local_test_spawn_squid(struct server_local* s);
void server_local_test_spawn_squid_w(struct server_local* s,
									 struct server_world* w);
int server_local_test_tick_spawners(struct server_local* s);
int server_local_test_tick_spawners_w(struct server_local* s,
									  struct server_world* w);
bool server_local_test_near_unload_send_w(struct server_world* w, w_coord_t cx,
										  w_coord_t cz);
struct residency_debt;
struct residency_debt* server_local_test_residency_debt_find(
	struct server_local* s, int32_t dimension, int32_t x, int32_t z,
	bool create);
// Drive only the bounded post-physics minecart snapshot scheduler.
void server_local_test_publish_minecart_snapshots(struct server_local* s);

// Fail the next query-snapshot growth before a moving TE advances.
void server_local_test_fail_next_entity_query_reserve(void);
// Admit a test-owned entity through the same Chunk.addEntity/list-order path.
bool server_local_test_entity_join(struct server_local* s, uint32_t entity_id);
/* Probe the Beta EntityLiving.getCanSpawnHere box clearance (entity + block +
 * liquid clauses) the natural spawn passes gate on, at the cell-centre snap
 * for `mob`'s own physics box. */
bool server_local_test_mob_spawn_bbox_clear(struct server_local* s, int mob,
											w_coord_t x, w_coord_t y,
											w_coord_t z);
/* Probe the real natural-spawn current-player exclusion without consuming RNG.
 * Shipping policy is the existing horizontal 16-block strict boundary. */
bool server_local_test_mob_spawn_far_from_players(struct server_local* s,
										   float x, float z);
bool server_local_test_mob_spawn_anchor_far_from_players(
	struct server_local* s, float x, float z);
// Drive the natural hostile-spawn pass (ring-scan + pack-spawn + the ghast
// channel) alone -- no weather/redstone/fluid/random-tick sharing rand_src.
void server_local_test_spawn_mobs(struct server_local* s);
struct java_random;
bool server_local_test_spawn_candidate(struct server_local* s,
	struct server_world* w, int pick, w_coord_t x, w_coord_t y, w_coord_t z,
	uint32_t* out_id, int* out_mob, struct java_random* out_rng);
void server_local_test_spawner_fire(struct server_local* s, w_coord_t x,
									w_coord_t y, w_coord_t z, int mob);
void server_local_test_spawn_candidate_placed(struct server_local* s,
	struct server_world* w, int pick, uint32_t* out_id, int* out_mob,
	struct java_random* out_rng);
// The two spawn-cap constants (MOB_SPAWN_CAP / GHAST_SPAWN_CAP), so tests
// assert against the real values instead of duplicating them.
int server_local_test_mob_spawn_cap(void);
int server_local_test_ghast_spawn_cap(void);
// Drive the spider-jockey roll+mount path against an already-fixtured
// spider entity, bypassing the picker/light/pack-scatter plumbing (spawn
// lane, wolf/jockey parity).
void server_local_test_spawn_spider_jockey(
	struct server_local* s, struct entity* spider);
// Fail the next chest/furnace content batch specifically, without allowing a
// preceding explosion block-item allocation to consume the test seam.
void server_local_test_fail_next_container_drop(void);
// Fail the outer heap object for the next authoritative container open.
void server_local_test_fail_next_window_alloc(void);
// Drive the exact async world-entry inventory finalizer without racing the
// server thread. Used to prove load-reason policy (normal/death vs portal).
void server_local_test_finish_load_inventory(struct server_local* s);
// Drain the real client->server transport through server_local_process without
// starting/racing the server thread. Used to pin delayed-action load barriers.
void server_local_test_process_messages(struct server_local* s);
/* G6 L1: the player-readable reason of the last refused load ("" if none). */
const char* server_local_load_refusal_reason(void);
void server_local_test_process_rpc(struct server_local* s,
								   struct server_rpc* call);
bool server_local_test_respawn_target_clear(struct server_local*,
	double x, double y, double z);
void server_local_test_fail_next_host_respawn_plan(void);
/* Production archive snapshot of one entity (server_entity_archive_copy). */
struct archived_entity;
void server_local_test_archive_entity(const struct entity* e,
									  struct archived_entity* out);
// Initialize the complete production server state without spawning its infinite
// background thread, so integration tests can deterministically own update calls.
void server_local_test_create_without_thread(
	struct server_local* s, bool give_test_kit, bool chest_selftest,
	bool fluid_demo, bool water_demo, bool zombie_demo, bool superflat,
	bool perf_stress);
// Drive one complete production server update without starting a racing thread.
// Portal tests use this to exercise real near-chunk streaming and the actual
// finished_loading/CRPC_PLAYER_POS completion gate.
void server_local_test_update_once(struct server_local* s);
bool server_local_test_checkpoint_live_world(struct server_local* s,
											 const char* reason);
/* Pin level.dat LastPlayed (0 = the wall clock) for byte-exact runs. */
void server_local_test_set_last_played(int64_t ms);
/* G8-BLAST-VELOCITY v4: replay the queued packet events only (test fixtures
 * that drain RPCs by hand; production runs it in server_local_update). */
void server_local_test_replay_actor_motion(struct server_local* s);
/* v5: called after each RPC the admitted drain processed (test refill). */
struct server_rpc;
extern void (*server_local_test_motion_replay_hook)(struct server_player* body,
	unsigned remaining);
extern void (*server_local_test_rpc_done_hook)(struct server_local* s,
	const struct server_rpc* call);
/* v5: the riding branch's motion before updateRidden zeroes it. */
extern double server_local_test_riding_branch_motion[3];
extern unsigned server_local_test_riding_branch_count;
extern unsigned server_local_test_sleeping_branch_count;
/* G9 phase probe: phase 0 = world prelude (spawning), 1 = entity pass,
 * 2 = tile entities; called for every ticked world. */
extern void (*server_local_test_phase_hook)(struct server_local* s,
											struct server_world* w, int phase);
/* G9 item 3: every consumer light read and every hostile spawn candidate's
 * two light rolls (Java Random state before/after, decision). */
extern void (*server_local_test_light_read_hook)(struct server_local* s,
												 struct server_world* w,
												 w_coord_t x, w_coord_t y,
												 w_coord_t z, int subtract,
												 int value);
extern void (*server_local_test_spawn_roll_hook)(struct server_world* w,
												 w_coord_t x, w_coord_t y,
												 w_coord_t z, int raw_sky,
												 uint64_t seed_before,
												 uint64_t seed_after,
												 bool decision);
uint32_t server_local_test_mob_lava_demo_build(struct server_local* s,
	float x_offset);
struct far_rpc_slot;
struct server_chunk;
bool server_local_test_finish_far_column(struct server_local* s,
	w_coord_t x, w_coord_t z, struct server_chunk* chunk,
	struct far_rpc_slot* slot);
// Finding-1 repro hook (2026-08-20 provider-swap redesign): make the SERVER
// thread emit N benign CRPC_TIME_SET per tick, so test_server_shutdown_env
// can wedge it inside clin_rpc_send's token backpressure against an
// undrained inbox and prove server_local_shutdown's drain-join returns.
void server_local_test_set_tick_crpc_flood(int per_tick);
// Force the production collision-center watchdog into its one-shot terminal UI.
void server_local_test_force_center_load_timeout(struct server_local* s);
// Drive exactly the production point-sampled hazard sequence. false means a
// lethal contact committed respawn and terminated the old-position sweep.
bool server_local_test_apply_player_hazards(
	struct server_local* s, bool in_water, bool in_lava, bool in_fire,
	bool in_cactus);
// Drive the REAL production cactus/fire body-box contact scans (the Beta
// Entity.moveEntity post-move cell sweep and World.isBoundingBoxBurning) at the
// player's current position, without a full server tick. Either out may be NULL.
void server_local_test_player_block_contact(struct server_local* s,
											bool* in_cactus, bool* in_fire);
// Drive the deferred Nether-death unload/reload transaction without the live
// server thread; false leaves a failed-close source world live and re-armed.
bool server_local_test_death_world_reload(struct server_local* s);

// Deterministic scheduler-handoff seam. The callback runs after the production
// thread_yield() that follows CRPC_BEGIN_WORLD_SWITCH, before preflight or
// persistence resumes. NULL disables it.
typedef void (*server_local_test_world_switch_handoff_fn)(
	enum server_world_switch_phase phase, void* user);
void server_local_test_set_world_switch_handoff(
	server_local_test_world_switch_handoff_fn hook, void* user);
// Manual-drive tests do not run the async load-finalizer that queues PLAYER_POS;
// this marks that exact completion boundary after they have asserted load state.
void server_local_test_world_switch_player_pos_queued(struct server_local* s);
// Drive the exact two-owner player/container close transaction without a
// world teardown, for ordering and allocation-failure regression tests.
bool server_local_test_close_active_inventory(struct server_local* s);
#endif
// Transactionally create/populate the authoritative container before exposing
// a client window. On success OPEN is FIFO-ahead of a complete slot snapshot;
// on any allocation/backing failure active_inventory and the client are unchanged.
bool server_local_open_inventory(struct server_local* s,
							 struct inventory_logic* logic, size_t capacity,
							 w_coord_t x, w_coord_t y, w_coord_t z,
							 uint8_t window, uint8_t type);
bool server_local_open_linked_inventory(
	struct server_local* s, struct inventory_logic* logic, size_t capacity,
	w_coord_t x, w_coord_t y, w_coord_t z, w_coord_t x2, w_coord_t y2,
	w_coord_t z2, uint8_t window, uint8_t type);
/* Entity-keyed sibling of server_local_open_inventory (minecart-slice): same
 * transactional open contract, but the backing owner is an entity id, not a
 * block position (inventory_create_entity, not inventory_create). Kept as its
 * OWN small self-contained function rather than threading a third
 * discriminator through server_local_open_inventory_impl's already-loaded
 * signature (linked/x2/y2/z2) -- server_local.c is shared-WIP, so this is the
 * more isolated edit even at the cost of ~15 duplicated lines
 * (CODESTYLE.md S3.2). */
bool server_local_open_entity_inventory(struct server_local* s,
									struct inventory_logic* logic,
									size_t capacity, uint32_t entity_id,
									uint8_t window, uint8_t type);
void server_local_send_inv_changes(set_inv_slot_t changes,
								   struct inventory* inv, uint8_t window);
/* Actor-bound local container entry points. The provider supplies the actor;
 * the request payload cannot choose another player's inventory. Bind reply and
 * reply_user on the base inventory before opening a window. Call on the server
 * thread, after the actor's world/input handoff; remote Beta transport stays P1. */
void server_local_window_reply(struct inventory* inv, struct client_rpc* call);
bool server_local_has_chest_view(struct server_local* s,
	w_coord_t x, w_coord_t y, w_coord_t z);
void server_local_process_window_for(struct server_local* s,
	struct server_player* player, const struct server_rpc* call);
bool server_local_open_inventory_for(struct server_local* s,
	struct server_player* player, struct inventory_logic* logic, size_t capacity,
	w_coord_t x, w_coord_t y, w_coord_t z, uint8_t window, uint8_t type);
bool server_local_open_linked_inventory_for(struct server_local* s,
	struct server_player* player, struct inventory_logic* logic, size_t capacity,
	w_coord_t x, w_coord_t y, w_coord_t z, w_coord_t x2, w_coord_t y2,
	w_coord_t z2, uint8_t window, uint8_t type);
bool server_local_open_entity_inventory_for(struct server_local* s,
	struct server_player* player, struct inventory_logic* logic, size_t capacity,
	uint32_t entity_id, uint8_t window, uint8_t type);
struct entity* server_local_spawn_item_for(vec3 pos, struct item_data* it,
	bool throw, struct server_local* s, const struct server_player* player);
bool server_local_close_active_inventory_for(struct server_local* s,
	struct server_player* player);
bool server_local_spawn_sourced_item_batch_for(vec3 pos,
	const struct item_data* items, const struct death_drop_provenance* provenance,
	size_t count, bool throw, bool container_rng, struct server_local* s,
	struct server_player* player);
// Lethal updates (new_health <= 0) commit only the DEAD state: health, the
// death position latch, sleep/pause cleanup, and the client broadcast. The
// drop+journal+respawn transaction runs later, in
// server_local_respawn_request. While dead, every further health write is
// rejected (returns false): Beta gates damage on health<=0
// [740c583 EntityLiving.attackEntityFrom:312] and healing on health>0
// [EntityLiving.heal:296]; only the respawn transaction revives.
enum server_player_damage_cause {
	PLAYER_DAMAGE_UNKNOWN, PLAYER_DAMAGE_FALL, PLAYER_DAMAGE_DROWN,
	PLAYER_DAMAGE_LAVA, PLAYER_DAMAGE_FIRE, PLAYER_DAMAGE_CACTUS,
	PLAYER_DAMAGE_COMBAT, PLAYER_DAMAGE_VOID,
	/* next-gaps-0924/W1A: telemetry only -- Beta attackEntityFrom(null, 1)
	 * carries no cause. Appended so earlier ordinals stay stable. */
	PLAYER_DAMAGE_SUFFOCATE
};
const char* server_player_damage_cause_name(uint32_t cause);
/* Session-scoped authoritative death evidence, independent of anchor mode.
 * SESSION == 0 means unavailable; counts must then NOT be used. Cause IDs are
 * server_player_damage_cause, never inferred from client health. Guest counts
 * survive Leave/rejoin and both owners' respawns until session retirement.
 * The accessor borrows no server/actor pointer; four bounded atomic attempts,
 * with a 1ms blocking sleep between contended attempts (no stale fallback). */
enum server_local_death_field {
	SLD_SESSION, SLD_HOST_DEATHS, SLD_HOST_CAUSE,
	SLD_GUEST_DEATHS, SLD_GUEST_CAUSE, SERVER_LOCAL_DEATH_FIELDS
};
bool server_local_death_snapshot(uint32_t out[SERVER_LOCAL_DEATH_FIELDS]);
#ifdef CAVEWII_TESTING
/* Pause a real publisher inside its odd sequence window; observe retries. */
void server_local_test_telemetry_hook(void (*hook)(bool guard, bool writing));
#endif
struct hw_telemetry_heartbeat;
/* Shared by the real main heartbeat and the producer regression. `local`
 * requires a local provider AND a client body; false publishes unavailable. */
bool server_local_death_heartbeat(bool local, struct hw_telemetry_heartbeat* hb);
/* Final interval record, only after this local server's orderly stop/join.
 * Ordinary live/disconnected reads continue to reject retired sessions. */
bool server_local_completion_heartbeat(const struct server_local* s,
	struct hw_telemetry_heartbeat* hb);
#define SERVER_LOCAL_PERF_GUARD_VERSION 2u
#define SERVER_LOCAL_PERF_GUARD_SUMMARY 10u
#define SERVER_LOCAL_PERF_GUARD_CONTEXT 4u
#define SERVER_LOCAL_PERF_GUARD_EVENT_WORDS 9u
#define SERVER_LOCAL_PERF_GUARD_WORDS 47u
enum server_local_perf_guard_field {
	SG_VERSION, SG_SESSION, SG_REQUESTED, SG_EFFECTIVE, SG_REFILLS,
	SG_MIN_HEALTH, SG_PREVENTED, SG_EVENTS, SG_DROPPED, SG_OVERFLOW,
	/* Appended after the unchanged four event slots. */
	SG_BARRIERS = 46
};
enum server_local_perf_guard_barrier {
	SGB_PREARM = 1, SGB_LOADING = 2, SGB_PAUSE = 4, SGB_ACK = 8,
	SGB_RESIDENCY = 16, SGB_TRANSFER = 32, SGB_TERMINAL = 64,
	SGB_WORLD = 128, SGB_SCOPE = 256, SGB_DEAD = 512
};
/* Configure quiescent, before start. local_arm is supplied only by the named
 * local hardware profile; root is its isolated run save directory. */
void server_local_perf_guard_configure(struct server_local*, bool requested,
	bool local_arm, const char* root);
void server_local_perf_guard_before_damage(struct server_local*);
bool server_local_perf_guard_snapshot(uint32_t out[SERVER_LOCAL_PERF_GUARD_WORDS]);
bool server_local_perf_guard_heartbeat(bool local, struct hw_telemetry_heartbeat*);
bool server_local_set_player_health(struct server_local* s, short new_health);
/* S1-a3 (Astra call #8 Q2): the ACTOR-AWARE healing entry. Beta 1.7.3 has no
 * hunger -- ItemFood and BlockCake.eatCakeSlice call EntityPlayer.heal()
 * directly on the eater, gated on health < 20 [740c583] -- so food and cake
 * heal the ACTING actor and cap at MAX_PLAYER_HEALTH inside the writer. A
 * zero-health body is refused (Beta EntityLiving.heal gates on health > 0).
 * The health reply rides that actor owner+generation envelope. */
bool server_local_heal_actor(
	struct server_local* s, struct server_player* player, short new_health);
/* S1-a3: the LOCAL PLAYER BODIES a world hit may damage, in a fixed order --
 * index 0 the host, index 1 the admitted split guest (NULL when none). The
 * projectile/hook sweeps enumerate through this instead of reading s->player,
 * so they can never be handed a stale actor pointer. */
struct server_player* server_local_damage_candidate(
	struct server_local* s, unsigned index);
// The death->respawn transaction, verbatim the old lethal-hit path: bed/spawn
// resolution (Beta does it HERE, at respawn time -- Minecraft.respawn), the
// Nether death-drop journal, the transactional inventory close, item drops at
// the death position, the fresh-body resets and the DEATH world switch.
// Driven by SRPC_RESPAWN (the death screen's Respawn button). Returns false
// on a retryable pre-commit failure (unreadable bed chunk, journal begin/close
// OOM): the player simply stays dead and may request again. No-op (false)
// unless dead, loaded, and no world switch is in flight.
bool server_local_respawn_request(struct server_local* s);
/* Split owner-0 same-world transaction state, exposed narrowly for the guest
 * admission gate and focused lifecycle tests. The source-0 dispatcher remains
 * internal to server_local.c. */
bool server_local_host_respawn_active(const struct server_local* s);
void server_local_host_respawn_poll(struct server_local* s);
bool server_local_prepare_guest_respawn(struct server_local* s,
	const struct server_player* player, double* x, double* y, double* z,
	bool* keep_bed_spawn);
// Beta mob->player melee hit: applies flat damage + knockback to the player,
// gated by the hurt-invulnerability window (i-frames). Called from a mob's
// server tick when it swings and the player is in reach. No-op while the player
// MP0 seam (lberwa fork analysis, adoption #2): resolve "the player" near a
// position. Returns THE single local player today, ignoring pos; when
// splitscreen MP0 pluralizes the server player this becomes an argmin over
// players[] and every routed call site (item pickup, mob closest-player
// queries as M2 touches them) is already multiplayer-correct. New code that
// wants "the player" relative to a world position MUST use this, never
// s->player directly.
// Interface split (build_architecture step 7 slice 4): no longer `static
// inline` here -- the body dereferences the full struct server_local, which
// this now-opaque header cannot see. Defined in server_local.c instead,
// beside server_local_hurt_player, where server_local_state.h is included.
struct server_player*
server_nearest_player(struct server_local* s, const float pos[3]);

// is still i-framed.
void server_local_hurt_player(struct server_local* s, struct entity* attacker);
/* S1-a3: the same hit against a NAMED actor (the mob AI passes the target it
 * actually resolved). &s->player is byte-identical to the wrapper above. */
void server_local_hurt_player_for(struct server_local* s,
	struct server_player* player, struct entity* attacker);
uint32_t server_local_next_player_correction(struct server_local* s);
/* residency-1005/2: player residency counters for the heartbeat "rz" array
 * (ASTRA-RESIDENCY-REVIEW item 2: refusal streaks, recovery latency,
 * progress, loader backlog). Cumulative per session over both bodies except
 * the *_MAX entries (session maxima) and PENDING (live request count).
 * Latencies are SERVER TICKS. Published once per server update. */
enum server_local_rz_tel {
	RZ_TEL_REJECTS = 0,			/* refused fresh samples */
	RZ_TEL_FAR,					/* ...beyond request reach (not loaded) */
	RZ_TEL_STREAK_MAX,			/* longest consecutive-refusal streak */
	RZ_TEL_LATENCY_MAX,			/* first refusal -> admission, ticks */
	RZ_TEL_RESTORES,			/* UNKNOWN body restored to its pin */
	RZ_TEL_RECOVERIES,			/* recovery barriers entered */
	RZ_TEL_RECOVERY_FAILURES,	/* ...that hit their deadline */
	RZ_TEL_RECOVERY_TICKS_MAX,	/* longest recovery, ticks */
	RZ_TEL_REQUESTS,			/* destination requests created */
	RZ_TEL_SERVED,				/* ...picked by a loader */
	RZ_TEL_PENDING,				/* live requests (loader backlog) */
	RZ_TEL_VETOES,				/* evictions refused for a pinned footprint */
	RZ_TEL_NOTICES,				/* HUD loading/failure notices sent */
	SERVER_LOCAL_RZ_TEL_COUNT
};
void server_local_rz_telemetry(uint32_t out[SERVER_LOCAL_RZ_TEL_COUNT]);
/* residency-1005/2 round 3 + residency-1005/4 closure item 8: PER-ACTOR
 * evidence for the hardware gates, wire schema v2 -- heartbeat "rzv":2,
 * "rzt" (server tick), "rza":[[host...],[guest...]] and "rzi" (induced-attempt
 * marks, closure item 12). Identity = row (0 host, 1 guest) + world slot +
 * world generation + BODY_EPOCH (+1 at every body lifecycle edge or pose
 * discontinuity not explained by admitted movement) + ATTEMPT. Cumulative
 * fields are PER ACTOR SLOT FOR THE SESSION: a body reset, a guest Leave/
 * rejoin or a world change never erases them. Values are full uint32 (no
 * saturation, no aliasing). Ticks are server ticks (near_send_seq). */
enum server_local_rza_field {
	RZA_WORLD_SLOT = 0,		/* 255 = no body / no live world */
	RZA_WORLD_GEN,			/* that slot's world instance generation */
	RZA_BODY_EPOCH,			/* session-monotonic lifecycle/discontinuity id */
	RZA_ATTEMPT,			/* id of the current / most recent attempt */
	RZA_STATE,				/* enum server_local_rza_state */
	RZA_STREAK_AGE,			/* open refusal streak age, else 0 */
	RZA_STATE_AGE,			/* age of RECOVERING / FAILED (dwell) / WAKE */
	RZA_LAST_ATTEMPT_TICKS,	/* FROZEN duration of the last terminated one */
	RZA_LAST_ATTEMPT_RESULT,/* 0 none, 1 ok, 2 failed */
	RZA_PENDING,			/* live requests */
	RZA_OLDEST_REQ_AGE,		/* oldest live request */
	RZA_REQUESTS,			/* session: requests created */
	RZA_LOADED,				/* session: requests whose column became resident */
	RZA_SERVICE_DELAY_MAX,	/* session max request born -> resident */
	RZA_LATENCY_MAX,		/* session max closed refusal streak */
	RZA_RECOVERY_EXITS_OK,	/* session: attempts that ended resident */
	RZA_RECOVERY_FAILURES,	/* session: attempts that hit the deadline */
	RZA_RECOVERY_TICKS_MAX,	/* session max successful attempt duration */
	RZA_RETRIES,			/* session: owner retries */
	RZA_NOTICES,			/* session: residency toasts to this pane */
	RZA_VETO_ATTEMPTS,		/* session: evictions refused (report only) */
	RZA_MOVED_DM,			/* session: admitted horizontal movement, 0.1 block */
	SERVER_LOCAL_RZA_FIELDS
};
enum server_local_rza_state {
	RZA_STATE_OK = 0,
	RZA_STATE_REFUSING = 1,
	RZA_STATE_RECOVERING = 2,
	RZA_STATE_FAILED = 3,
	RZA_STATE_WAKE_HOLD = 4,
};
#define SERVER_LOCAL_RZI_MAX 4u /* induced-attempt marks */
#define SERVER_LOCAL_RZI_FIELDS 5u /* actor, slot, world gen, epoch, attempt */
struct server_local_rz_wire {
	uint32_t tick;
	uint32_t rza[2][SERVER_LOCAL_RZA_FIELDS];
	uint32_t rze[2][8]; /* retained terminal/retry/completion receipts */
	uint32_t rzi[SERVER_LOCAL_RZI_MAX][SERVER_LOCAL_RZI_FIELDS];
	uint32_t rzi_count;
	uint32_t rzf; /* enum server_local_rzf phases seen */
};
void server_local_rz_actor_telemetry(struct server_local_rz_wire* out);
/* Closure item 12: the runtime residency fault (route E). Any thread may
 * REQUEST it; the server thread consumes the request at its next update and
 * arms it there for the named actor (0 host, 1 guest) -- its CURRENT live
 * body, world slot + generation and footprint columns. Arming saves and
 * unloads those columns and withdraws the body pin, so the body enters ONE
 * bounded recovery attempt whose loads of exactly those columns fail; the
 * attempt is marked in "rzi". The fault disarms when that attempt
 * terminates (at its deadline), when the body/world changes, or on
 * server_local_rz_fault_disarm / session init; the owner's deliberate retry
 * then loads normally. It is REFUSED (rzf bit6) when the actor has no live,
 * loaded, settled body, or when the partner's footprint shares a column.
 * Phases seen this session: "rzf" bitmask (enum server_local_rzf). */
enum server_local_rzf {
	RZF_ARMED = 1u << 0,
	RZF_ENTERED = 1u << 1,
	RZF_TERMINAL = 1u << 2,
	RZF_NOTICE = 1u << 3,
	RZF_RETRY = 1u << 4,
	RZF_DISARMED = 1u << 5,
	RZF_REFUSED = 1u << 6,
};
void server_local_rz_fault_request(int actor);
void server_local_rz_fault_disarm(struct server_local* s);
uint32_t server_local_rz_fault_phases(void);
/* Retire a body's TRANSIENT residency state (pin, requests, recovery,
 * streak, velocity) across a lifecycle edge; cumulative counters stay. */
struct server_player;
void server_local_rz_retire_body(struct server_player* p);
/* residency-1005/2 S2: the loader hook. The highest-priority, oldest
 * eligible destination request either body holds for world `w` (refused
 * footprint / recovery first, then directional load-ahead), served
 * round-robin between the bodies; false when none. A pick marks the request
 * attempted for this update. Called by the slot-0 near-send scan and the
 * A2-W stream step of every other live world, ahead of their nearest-first
 * order and inside their existing per-tick budgets. */
struct server_world;
bool server_local_rz_pick(struct server_local* s, struct server_world* w,
	int32_t* x, int32_t* z);
/* residency-1005/2 S3: eviction protection. True when column (cx, cz) of
 * `w` holds part of a live body's accepted collision/hazard footprint -- its
 * pin (world slot + generation, while the body still inhabits `w`) or its
 * current resident pose. The streaming EVICTORS (the slot-0 near evictor
 * and the A2-W secondary-world evictor) never pick such a column; explicit
 * whole-world saves/teardowns are not evictors and are unaffected. */
bool server_local_rz_evict_veto(struct server_local* s,
	struct server_world* w, int32_t cx, int32_t cz);
/* residency-1005/2 S3: the transfer COMMIT barrier. True when the body
 * footprint at (x, z) in `w` is resident; otherwise loads ONE of its missing
 * columns through the bounded ensure_populated step and returns false. */
bool server_local_rz_landing_ready(struct server_local* s,
	struct server_world* w, double x, double z);
/* S1 / R-S1a2 F2: raise the PER-ACTOR input latch
 * (struct server_player.reject_pos_this_update) on every present body, so a
 * world-authored position correction drops the movement input both actors
 * already queued against the pre-correction state. The SERVER-WIDE
 * s->reject_player_pos_this_update keeps its own, different meaning ("a body
 * was replaced, end this logical update") -- see server_local_state.h.
 * Used by the piston push correction (source/block/redstone_piston_server.c),
 * which re-authors a player position mid-update. */
void server_local_raise_actor_input_barriers(struct server_local* s);
/* R-S1a3 F1/F2: the two PER-TICK steps of the damage pipeline, driven once per
 * server update (server_local_update) for EVERY present body in
 * server_local_damage_candidate order -- host first, guest second. S1-a3 left
 * both on s->player alone, which meant the guest's hurt-resistance window
 * never closed and its knockback was never integrated or delivered. With no
 * guest admitted both are the host-only statements they replace, byte for
 * byte (test_host_damage_golden, test_split_off_equiv). */
void server_local_tick_actor_hurt_resistance(struct server_local* s);
/* G8-BLAST-VELOCITY: the server motion step of every live body -- Beta
 * EntityLiving.moveEntityWithHeading(0, 0) on the EntityPlayerMP's own
 * motion (water / lava / air branches, ladder clamps, block-collision clip
 * that zeroes each clipped axis, gravity + drag on motionY, friction on X/Z),
 * which NetServerHandler.handleFlying -> onUpdateEntity runs once per
 * movement packet. HARNESS ONLY (v4): one stationary packet for every
 * present body; production replays each accepted packet's event (physics,
 * own push loop, collision move, deferred landing) in server_local_update's
 * trailing tracker phase. The body's position is never moved. */
void server_local_tick_actor_knockback(struct server_local* s);
/* G8: Entity.addVelocity on a player body, delivered to its own client as an
 * additive CRPC_LOCAL_PLAYER_VELOCITY (server copy kept for knockBack). The
 * cart shove and the fishing self-pull only -- explosions use the three
 * G8-BLAST-VELOCITY entries below. */
struct server_player;
void server_local_push_actor_velocity(struct server_local* s,
	struct server_player* body, float dx, float dz);
/* G8-BLAST-VELOCITY (ASTRA-RB06-RESIDUALS section 2). The explosion leg of
 * Beta Explosion.doExplosionA:100-103 on a player body:
 *  - server_local_blast_damage_actor: attackEntityFrom(exploder, dmg). The
 *    same i-frame / armor / health / knockBack transaction as the flat-damage
 *    entries (from_entity_mob selects the creeper difficulty scale). Like
 *    EVERY damage path, a committed fresh hit only raises beenAttacked; no
 *    hit path sends velocity. Return value as server_local_damage_actor.
 *  - server_local_add_actor_motion: motionX/Y/Z += impulse on the server
 *    motion (vel_x, motion_y, vel_z), unconditionally; no send.
 *  - server_local_flush_actor_velocity: the EntityTrackerEntry:125-127 step.
 *    server_local_update runs it ONCE, at its very end -- after every
 *    world's entity / tile pass, the player hazards and the server motion
 *    step. Each present body whose flag is set (same lifetime, live route)
 *    gets ONE absolute CRPC_LOCAL_PLAYER_VELOCITY of its current server
 *    motion, Packet28 quantized (clamp +-3.9, (int)(v*8000), /8000), on its
 *    own reply route; the flag is then cleared. A body with no live route
 *    keeps its flag until it has one (or its lifetime ends). */
#define SERVER_LOCAL_HAS_ACTOR_VELOCITY_FLUSH 1
#define SERVER_LOCAL_HAS_SERVER_MOTION_Y 1 /* struct server_player.motion_y */
/* v5: Beta's attackEntityFrom attacker -- present = `var1 != null`,
 * dx/dz = var1.posX - this.posX, var1.posZ - this.posZ (doubles). */
struct actor_attacker {
	bool present;
	double dx, dz;
};
/* v5: has_exploder + the exploder-minus-victim delta (the exploder is
 * attackEntityFrom's attacker, Explosion.java:100). */
bool server_local_blast_damage_actor(struct server_local* s,
	struct server_player* player, int damage, bool has_exploder, double dx,
	double dz, bool from_entity_mob);
void server_local_add_actor_motion(struct server_local* s,
	struct server_player* body, double dx, double dy, double dz);
void server_local_flush_actor_velocity(struct server_local* s);
/* v3 (ASTRA-V2-REVIEW (a)): record one ACCEPTED movement sample's server
 * physics (Beta handleFlying) before the new pose is stored; the tracker
 * phase replays it. rvx/rvz = the sample's rider input. */
void server_local_actor_motion_enqueue(struct server_local* s,
	struct server_player* body, double nx, double ny, double nz, float rvx,
	float rvz, bool packet_on_ground);
/* v5: BEFORE any side effect of a sample. The admitted drain guarantees
 * room (it holds, never refuses); false = the counted invariant violation
 * actor_motion_overflow. An event never runs early. */
bool server_local_actor_motion_reserve(struct server_local* s,
	struct server_player* body);
/* v4: after the sample's fall accounting -- a paused sample without a
 * damaging landing is no event (the local pause contract). */
void server_local_actor_motion_settle_for(struct server_player* body);
/* v4: the movement routes' fall accounting -- a damaging landing is
 * DEFERRED into the sample's event (applied at the replay, after the
 * packet's physics and the world's hits: NetServerHandler.java:154,173,
 * 211-213). server_local_player_vertical_sample_for (below) is the direct
 * entry: same accounting, the landing billed at once. */
bool server_local_player_vertical_sample_actor(struct server_local* s,
	struct server_player* player, double previous_y, float new_vel_y,
	bool new_on_ground);
/* v3: lifetime boundary -- no pending events, flag or server motion. */
void server_local_actor_motion_reset(struct server_player* body);
bool server_local_player_body_in_water_for(
	struct server_local* s, const struct server_player* player);
bool server_local_player_on_ladder_for(
	struct server_local* s, const struct server_player* player);
bool server_local_player_vertical_sample_for(struct server_local* s,
	struct server_player* player, double previous_y, float new_vel_y,
	bool new_on_ground);
// Returns true if the player SURVIVED the hit (or it was absorbed by i-frames),
// false if it killed + respawned them. Callers stamping post-damage state (e.g.
// the on-fire flag) MUST skip it when this returns false -- the player is a fresh
// full-health respawn at spawn, nowhere near the hazard.
bool server_local_damage_player(struct server_local* s, int damage, float kx,
								float kz);
/* UNKNOWN-cause actor sibling used by piston fall/cactus callbacks; the host
 * wrapper above remains the original telemetry path. */
bool server_local_damage_actor(struct server_local* s,
	struct server_player* player, int damage, float kx, float kz);
/* Same operation, with `accepted` distinguishing Beta attackEntityFrom(true)
 * from an absorbed/OOM-uncommitted hit. Survival alone cannot make that
 * distinction (arrows bounce only for the latter). */
bool server_local_damage_player_result(struct server_local* s, int damage,
	float kx, float kz, bool* accepted);
/* S1-a3: the same flat-damage hit against a NAMED actor, for the sweeps that
 * have already decided WHICH body the projectile/hook struck. A NULL actor is
 * a no-op that reports "survived, not accepted". */
bool server_local_damage_actor_result(struct server_local* s,
	struct server_player* player, int damage, float kx, float kz,
	bool* accepted);
/* G8-BLAST-VELOCITY v4: the hit whose knockBack is measured from the
 * ATTACKER'S CURRENT POSITION (EntityArrow.java:147 -> EntityLiving.
 * attackEntityFrom:321-332); has_attacker false = Beta's null attacker: a
 * fresh hit, no knockBack. from_entity_mob selects the difficulty scale. */
bool server_local_damage_actor_from_attacker(struct server_local* s,
	struct server_player* player, int damage, bool has_attacker,
	double attacker_x, double attacker_z, bool from_entity_mob,
	bool* accepted);
/* S1-a2: `shooter` is the lifetime-safe identity of the ACTING actor for a
 * player shot (ACTOR_IDENT_HOST for a mob-fired arrow -- a skeleton is not an
 * actor, and from_player/from_skeleton already carry that distinction). */
void server_local_spawn_arrow_for(vec3 pos, vec3 vel, uint32_t owner_id,
	uint16_t item_id, int damage, bool from_player, struct server_local* s,
	struct actor_ident shooter);
/* A2-E review F4: queue an arrow into an EXPLICIT world (a block-driven
 * shot: the dispenser). `w` must be a world of this session; a non-live `w`
 * queues nothing. */
void server_local_spawn_arrow_w(struct server_world* w, vec3 pos, vec3 vel,
								uint32_t owner_id, uint16_t item_id, int damage,
								bool from_player);
/* I02 (board #8): the most recent enqueue (queue length `before` beforehand)
 * draws Beta's setArrowHeading spread from the NEW arrow's own stream at the
 * flush: `discard` gaussians first, then X/Y/Z on the normalized `aim`, scaled
 * by `speed`; `advance` moves the spawn point by that velocity. */
void server_local_pending_arrow_spread(struct server_local* s, int before,
	const float aim[3], float speed, float deviation, uint8_t discard,
	bool advance);
/* I02 test hook: the real post-tick arrow flush. */
void server_local_test_flush_arrows(struct server_local* s);
void server_local_spawn_arrow(vec3 pos, vec3 vel, uint32_t owner_id,
							  uint16_t item_id, int damage, bool from_player,
							  struct server_local* s);
// GHAST fireball launch (batch 1b): queued like the arrows (called from the
// entity tick), flushed after the tick loop. dir is the unnormalized aim.
void server_local_spawn_fireball(vec3 pos, const float dir[3],
								 uint32_t owner_id, struct server_local* s);
void server_local_spawner_stats(int* us, int* active);
/* Last complete server-tick truth for the local host. False zeroes `out` when
 * no local world has published a sample. `sky` is exposed for F3; PERCEIVE's
 * stable field is the full `rained_on` exposure decision. */
struct server_local_host_weather_diagnostic {
	bool wet;
	int fire_ticks;
	bool rained_on;
	uint8_t height;
	bool biome_rains;
	bool sky;
	w_coord_t fx, fy, fz;
};
/* Keep the server-owned snapshot warm for about two seconds. Consumers refresh
 * this request while sampling; without one, the host tick performs no extra
 * weather/exposure query and read() reports not published. */
void server_local_host_weather_diagnostic_request(void);
bool server_local_host_weather_diagnostic_read(
	struct server_local_host_weather_diagnostic* out);
/* One coherent TNT fixture verdict. A failed bounded take leaves valid=0. */
struct server_local_tnt_evidence {
	uint32_t valid, st, bits, primed_left, carved, pad, gen, tick;
};
bool server_local_tnt_evidence_take(struct server_local_tnt_evidence* out);
/* mob_motion_2m fixture verdict (lane C, mob motion units S2 proof). valid=0
 * until the rig publishes (the heartbeat then omits the mm_* keys); st uses the
 * TNT vocabulary 0 inactive / 1 pending / 2 PASS / 3 FAIL / 4 SKIP; bits are
 * mob_motion_probe.h MM_PROBE_BIT_*. */
struct server_local_mob_motion_evidence {
	uint32_t valid, st, bits, recur_ok, recur_bad, rise_milli, step_milli, gen,
		tick;
};
bool server_local_mob_motion_evidence_take(
	struct server_local_mob_motion_evidence* out);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* 0 idle, >0 running, -1 verdict published. */
int server_local_test_mob_motion_rig_tick(void);
#endif
#ifdef CAVEWII_SERVER_LOCAL_TEST
uint32_t server_local_test_host_weather_diagnostic_sequence(void);
uint32_t server_local_test_host_weather_diagnostic_exposure_queries(void);
uint32_t server_local_test_host_weather_diagnostic_sequence_exchange(
	uint32_t value);
#endif
// P0 perf instrument: last + session-max wall-clock (ms) of one server tick
// (server_local_update), for the F3 overlay. The server-side counterpart to the
// client "tck" -- on the single Broadway core this time is stolen from the frame.
void server_local_tick_stats(float* last_ms, float* max_ms);
#include "server_work_profile.h"
// M1 deep reach instrument: cumulative far chunks streamed + the live far_sent
// count (the bounded far ring). Either pointer may be NULL.
void server_local_far_stats(unsigned* total, int* live);
// Far-slot pool STARVATION: the CURRENT run of consecutive far-batch bails, reset by any
// successful acquire. A short run is ordinary backpressure; a run that never ends is a
// far slot the client consumed and never released, i.e. far streaming has silently
// stopped. The non-blocking acquire made that failure symptomless, and its only alarm is
// an fprintf(stderr) that the shipping -DNDEBUG .dol compiles away, so the number needs a
// reader that survives to hardware -- the F3 far row and the watchdog hang report.
uint32_t server_local_far_slot_starve(void);
// W1-c measurement instrument (docs/far_reach_program_2026-09-03.md
// "Server-side scan cost"): cumulative far-pick template entries examined by
// the far send loop, across every tick this process has run. A caller wants
// a before/after DELTA (this process-lifetime total, not a per-tick value).
uint64_t server_local_far_pick_iterations(void);

#ifdef CAVEWII_SERVER_LOCAL_TEST
bool server_local_test_near_unload_send_or_defer(struct server_local* s,
												 w_coord_t cx, w_coord_t cz);
bool server_local_test_near_unload_flush(struct server_local* s);
int server_local_test_near_unload_pending_count(struct server_local* s);
// W1-c golden proof hooks (tests/test_far_pick_frontier_golden.c): drive the
// REAL production far-pick frontier (server_local.c's far_pick_template_build
// / far_pick_next) directly, without a full disk-backed server tick.
// T2 far-send-share hooks (tests/test_far_send_share.c, 2026-09-04): the REAL
// production per-tick share decision, the promotion-debt guard predicate, and
// the sticky chunk-crossing heading update, driven without a full server tick.
void server_local_test_far_send_share(int near_owed, int near_sent,
									  int share_limit, bool* allowed,
									  int* budget, int* lead);
bool server_local_test_far_send_enters_near(w_coord_t dx, w_coord_t dz,
											int heading_x, int heading_z,
											int lead);
void server_local_test_far_send_heading_update(struct server_local* s,
											   w_coord_t px, w_coord_t pz);
void server_local_test_far_gen_one_per_update(bool enable);
int server_local_test_near_send_batch(void);
void server_local_test_far_pick_rebuild(struct server_local* s);
bool server_local_test_far_pick_next(struct server_local* s, w_coord_t px,
									 w_coord_t pz, int* cursor,
									 w_coord_t* out_x, w_coord_t* out_z);
#endif

// Beta overworld sky-light subtraction from the celestial angle, CLEAR WEATHER ONLY: 0 at
// noon, 11 at midnight. Pure + server-safe (unlike daytime_brightness, which reads client
// gstate). Kept as the pinned clear-weather curve (tests compare against it); simulation
// light reads use server_local_skylight_subtracted / server_local_block_light_value (F1).
int server_local_sky_subtract(uint64_t world_time);
/* next-gaps-0924/F1 + w2: Beta World.skylightSubtracted [740c583 World.java:991-1007]: the day/night
 * curve PLUS rain and rain-weighted thunder darkening (5/16 each); 11 in the Nether. It is the value
 * CACHED once per server tick (World.tick:1757-1759), not a live function of the clock.
 * server_local_sky_subtract above is the clear-weather curve only. */
int server_local_skylight_subtracted(const struct server_local* s);
/* w2: World.tick's per-tick recompute; world load initializes the cache separately. */
void server_local_skylight_recompute(struct server_local* s);
/* World.isDaytime:692-694 -- the cached skylightSubtracted < 4 (bed gate, sleep wake, sun-burn). */
bool server_local_is_daytime(const struct server_local* s);
/* w5 debug.sleep_spawn_stress fixture counters (completed re-sleeps / of which a sleep spawn). */
int server_local_sleep_stress_cycles(const struct server_local* s);
int server_local_sleep_stress_spawns(const struct server_local* s);
/* Beta World.getBlockLightValue(x,y,z) [World.java:533-583] with the current
 * server_local_skylight_subtracted: slab 44 / farmland 60 / stairs 53, 67 return the max of their 5
 * neighbours (y+1, x+1, x-1, z+1, z-1); y<0 -> 0; y>=128 reads y=127; a non-resident cell reads 0
 * (Beta EmptyChunk); outside +-32000000 -> 15. A pure read: no RNG, no chunk loads. */
int server_local_block_light_value(struct server_local* s, w_coord_t x,
								   w_coord_t y, w_coord_t z);
/* The same read with an explicit skylightSubtracted (Beta callers that temporarily override it). */
int server_local_block_light_value_sub(struct server_local* s, w_coord_t x,
									   w_coord_t y, w_coord_t z, int subtract);
/* EntityMob.getCanSpawnHere's var4 [EntityMob.java:76-82]: getBlockLightValue, with
 * skylightSubtracted forced to 10 during a thunderstorm (func_27160_B; never in the Nether). */
int server_local_hostile_spawn_light(struct server_local* s, w_coord_t x,
									 w_coord_t y, w_coord_t z);
/* Harness setup/teardown; caller must stop the server thread first. */
bool server_local_set_anchor_sep(struct server_local* s, int blocks);
/* colcap-1006 R5 item 3: live synthetic-anchor offset, 0..anchor_sep (any
 * thread; relaxed). False when not in synthetic two-anchor mode. */
bool server_local_set_anchor_offset(struct server_local* s, int blocks);
/* Experimental local split-screen ("experimental.local_splitscreen", default
 * OFF), BOOT path. Main thread, server thread stopped. Pre-allocates the
 * residency scheduler so a mid-session guest-body admission never allocates on
 * the server thread; refuses to turn OFF while a body is admitted.
 *
 * Once the server thread is running this still refuses: the LIVE route is
 * SRPC_SPLIT_SET (Astra option A, docs/local_splitscreen.md "Live toggle"),
 * which runs the SAME allocate/arm transition on the owning thread between
 * ticks and acknowledges it with CRPC_SPLIT_STATE. There is no cross-thread
 * write path to s->split_enabled. */
bool server_local_set_split_enabled(struct server_local* s, bool enabled);
bool server_local_residency_dependency(struct server_local* s, int32_t x,
									   int32_t z);
void server_local_residency_refresh(struct server_local* s);
/* Retire one residency anchor's debt/owner bits and owed-oldest counters.
 * server_local_residency_refresh stops running the instant two-anchor mode
 * ends, so the departing owner's state must be cleared explicitly or the next
 * body inherits it. Anchor 1 is the split-screen guest; anchor 0 is the host. */
void server_local_residency_release_anchor(struct server_local* s,
										   unsigned anchor);
/* Server-owner-only classification, shared by the startup retire gate/census. */
unsigned server_local_startup_class(struct server_local* s, int32_t x, int32_t z);
void server_local_test_startup_retirement(bool enable);
#ifdef CAVEWII_SERVER_LOCAL_TEST
void server_local_test_retire_startup_support(struct server_local* s);
void server_local_test_startup_retirement_limit(unsigned saves);
#endif
bool server_local_residency_pick(struct server_local* s, bool far, int32_t* x,
								 int32_t* z);

/* Split render reach as seen by the server (0 = full presentation). */
void server_local_set_split_reach(int chunks);
void server_local_split_policy_apply(struct server_local* s);
int server_local_split_reach(void);

/* ===== S1 fix round 2: the player BODIES present in the world ===========
 * Every local player body that currently occupies space: the host (always)
 * and the split-screen guest body while one is bound. Neither is in
 * s->entities, so anything that sweeps the world for collision has to
 * enumerate them explicitly -- this is the place that says which bodies
 * are present. Sweeps that use it (block placement obstruction, S1 fix
 * round 2) agree with each other; sweeps that still read s->player only
 * (pressure plates, piston push, mob-spawn suppression, the redstone door
 * swing) see the host body alone and are tracked as S1 follow-ups in
 * tmp/jobs/mp-split-0912/s1-design.md until they are converted.
 *
 * PRESENCE IS DELIBERATELY WEAKER THAN server_local_actor_resolve(): that one
 * answers "does this stored {slot, generation} ROLE still name a live actor",
 * and refuses a body that is bound but not yet admitted to gameplay. A
 * collision sweep asks a different question -- "is there a body standing
 * here" -- and a guest body occupies space from the moment it is bound
 * (server_local_guest_create sets has_pos and a real x/y/z BEFORE
 * server_local_bind_guest, and guest_release/unbind clear s->guest_actor),
 * whatever its gameplay-admission or generation state.
 *
 * Read-only; the server thread owns both bodies. Writes the host first, then
 * the guest, and returns the count written, never more than `cap`. */
#define SERVER_LOCAL_MAX_PRESENT_ACTORS 2
size_t server_local_present_actors(struct server_local* s,
								   struct server_player** out, size_t cap);

/* Temporary hardware-stall diagnostic; sampled by the main heartbeat. Values
 * name the server update span currently entered, 0 means outside an update. */
enum server_diag_stage {
	SERVER_DIAG_OUTSIDE = 0, SERVER_DIAG_RPC = 1,
	SERVER_DIAG_POST_RPC = 2, SERVER_DIAG_WEATHER = 3,
	SERVER_DIAG_SLEEP = 4, SERVER_DIAG_TRANSFER = 5,
	SERVER_DIAG_RESEND = 6, SERVER_DIAG_A2W_STREAM = 7,
	SERVER_DIAG_ENTITIES = 8, SERVER_DIAG_A2W_PICK = 9,
	SERVER_DIAG_A2W_EVICT = 10, SERVER_DIAG_A2W_ENSURE = 11,
	SERVER_DIAG_A2W_SEND = 12, SERVER_DIAG_A2W_FLUSH = 13,
	SERVER_DIAG_A2W_MARKER = 14,
	SERVER_DIAG_TRANSFER_PENDING = 15, SERVER_DIAG_TRANSFER_BUDGET = 16,
	SERVER_DIAG_TRANSFER_RELEASES = 17, SERVER_DIAG_TRANSFER_PICK = 18,
	SERVER_DIAG_TRANSFER_RESTORE = 19, SERVER_DIAG_TRANSFER_REQUEST = 20,
	SERVER_DIAG_TRANSFER_DRAIN = 21, SERVER_DIAG_TRANSFER_WAIT = 22,
	SERVER_DIAG_TRANSFER_PREPARE = 23,
	SERVER_DIAG_COMMIT_MARKER = 24, SERVER_DIAG_COMMIT_BODY = 25,
	SERVER_DIAG_COMMIT_CAPACITY = 26, SERVER_DIAG_COMMIT_PUBLISH = 27,
	SERVER_DIAG_COMMIT_HOLD_END = 28,
	SERVER_DIAG_HOST_BEGIN = 29, SERVER_DIAG_HOST_RESET = 30,
	SERVER_DIAG_HOST_HEALTH = 31, SERVER_DIAG_HOST_STREAM = 32,
	SERVER_DIAG_SAVE_SNAPSHOT = 33, SERVER_DIAG_SAVE_WRITE_PLAYER = 34,
	SERVER_DIAG_SAVE_ENTRY = 35, SERVER_DIAG_SAVE_RETURN = 36,
	SERVER_DIAG_FLUSH_CLONE = 37, SERVER_DIAG_FLUSH_COMPARE = 38,
	SERVER_DIAG_FLUSH_OPEN = 39, SERVER_DIAG_FLUSH_DUMP = 40,
	SERVER_DIAG_FLUSH_FFLUSH = 41, SERVER_DIAG_FLUSH_FSYNC = 42,
	SERVER_DIAG_FLUSH_CLOSE = 43, SERVER_DIAG_FLUSH_PARSE = 44,
	SERVER_DIAG_FLUSH_CHECK_CLONE = 45, SERVER_DIAG_FLUSH_EQUAL = 46,
	SERVER_DIAG_FLUSH_REMOVE_OLD = 47, SERVER_DIAG_FLUSH_RENAME_OLD = 48,
	SERVER_DIAG_FLUSH_RENAME_NEW = 49, SERVER_DIAG_FLUSH_CLEANUP = 50
};
uint32_t server_local_diag_stage(void);
void server_diag_set(enum server_diag_stage stage);

/* ASTRA-G9-R5 point 9: take-and-clear the per-window maxima of the
 * random-tick (rt) and block/TE (wt) component timings (us). Heartbeat
 * reader only; see server_local.c. */
void server_local_tick_window_max_take(uint32_t* rt_us, uint32_t* wt_us);

/* PAR-BED-META save-version-5 load stage. From exactly version 4 it converts
 * legacy CaveWii bed metadata to Beta's layout (region_archive_migrate_bed_
 * metadata) ONLY when the world carries CaveWii creation provenance
 * (level_archive_was_created_by_cavewii); an imported real Beta world is
 * stamped 5 with no region byte changed. Any other version: no-op, *ran
 * false. False = the load must abort (nothing stamped; retry-safe). */
struct level_archive;
struct bed_metadata_migration_stats;
bool server_local_migrate_bed_metadata_stage(
	struct level_archive* la, string_t level_name,
	struct bed_metadata_migration_stats* stats, bool* ran, bool* converted);

/* IMP-MIGRATE-FAST: the whole CaveWiiSaveVersion chain a world load runs
 * (v2 wall signs, v3 iron chests, v4 spawner TEs, v5 bed metadata) -- same
 * order, gating, stamps and staged write paths as before; stages 2-4 share one
 * read-only file-order survey and run their staged pass only when the survey
 * cannot prove them a no-op. False = the load must abort (caller discards).
 * `profile`/`profile_regions` feed the save_migrate hardware profile. */
bool server_local_save_version_chain(struct level_archive* level,
	string_t level_name, bool profile, uint32_t profile_regions);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* dropin-1029 review: simulate a power cut right after the stage that stamps
 * `version` (its own durable save, if it wrote regions, has happened); the
 * chain returns false and nothing later reaches the disk. -1 disables. */
void server_local_test_chain_power_cut_after(int32_t version);
#endif
/* Server-thread menu service. Opens an independent archive, invokes exactly
 * the load-time chain, then discards the archive without entering a world.
 * Cancellation settles at the chain boundary; it never interrupts a write. */
void server_local_convert_service(struct server_local* s);
/* R3: server-thread half of the create-world spawn search (svin_spawn_*). */
void server_local_spawn_search_service(struct server_local* s);
/* R3: the create flow's spawn search. With a local server up it is served on
 * the server thread (Beta order: chunks load + populate through the world's
 * own provider, World.rand fizz draws included) and this call blocks until the
 * result; without one, the in-thread worldgen search. False = no Beta spawn
 * (the caller must not create the world, REL-03b). */
bool server_local_create_spawns(const char* dir, int64_t seed, bool superflat,
								bool start_nether, float world_spawn[3],
								float player_spawn[3],
								void (*idle)(void* user), void* user);
/* Telemetry only (hw1030n): true while server_local_create_spawns is waiting
 * on the server-thread search; then the chunks it has loaded so far and ms
 * since the wait began. Main thread. Never read by the search itself. */
bool server_local_spawn_search_progress(uint32_t* probes, uint32_t* elapsed_ms);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Peak resident chunks of the most recent live spawn search (cap proof). */
size_t server_local_test_spawn_search_peak_resident(void);
#endif
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Test seam: run instead of sleeping while the create flow waits for the
 * server half (a single-threaded test services the mailbox from here). */
extern void (*server_local_test_spawn_search_wait_hook)(void);
#endif
/* Loading-screen progress of an import conversion. False when no conversion
 * is running. *percent is 0..100 while the survey runs, UINT32_MAX while a
 * staged pass with no known total runs. Any thread. */
bool server_local_import_progress(uint32_t* percent);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* false: the chain skips the survey (every stage runs its staged pass, the
 * pre-IMP-MIGRATE-FAST behaviour) -- the equivalence test's reference. */
void server_local_test_migration_survey_enable(bool enabled);
#endif

#endif
