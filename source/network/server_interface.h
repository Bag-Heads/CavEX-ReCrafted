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

#ifndef SERVER_INTERFACE_H
#define SERVER_INTERFACE_H

#include "../item/item_types.h"
#include "recipe_craft.h"
#include "../world.h"
#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_PLAYER_HEALTH 160
/* Convert-only is a bounded LOCAL mailbox, never an SRPC or a remote packet.
 * Main thread: request/cancel/read/release. Server thread: take/finish.
 * One outstanding request including its retained terminal receipt. Tickets
 * never repeat, even across provider restarts. Paths over the bound fail
 * admission rather than being truncated. */
#define SVIN_CONVERT_PATH_MAX 512
enum svin_convert_outcome {
	SVIN_CONVERT_PENDING, SVIN_CONVERT_SUCCEEDED, SVIN_CONVERT_FAILED,
	SVIN_CONVERT_CANCELLED, SVIN_CONVERT_WORLD_ACTIVE,
	SVIN_CONVERT_JOURNAL_REFUSED, SVIN_CONVERT_OPEN_FAILED,
	SVIN_CONVERT_PROVIDER_STOPPED,
	/* Current schema, but load-time normalization still needs a checkpoint.
	 * Convert-only deliberately does not perform a player/world save. */
	SVIN_CONVERT_SAVE_PENDING
};
struct svin_convert_result {
	uint64_t ticket;
	enum svin_convert_outcome outcome;
	/* True once the unchanged migration chain was entered. Failure/cancel
	 * then cannot promise that disk is unchanged; this is not a rollback. */
	bool may_have_changed;
};
uint64_t svin_convert_request(const char* path);
bool svin_convert_cancel(uint64_t ticket);
bool svin_convert_read(uint64_t ticket, struct svin_convert_result* result);
bool svin_convert_release(uint64_t ticket);
bool svin_convert_take(uint64_t* ticket, const char** path, bool* cancelled);
bool svin_convert_finish(uint64_t ticket, enum svin_convert_outcome outcome,
	bool may_have_changed);

/* R3 new-world spawn search: a bounded LOCAL mailbox like convert-only, never
 * an SRPC or a remote packet. Main thread (the create flow): request/read/
 * release; server thread: take/finish. The search runs on the server thread
 * because Beta's World.getInitialSpawnLocation loads AND populates chunks
 * through the world's own chunk provider (server_local_beta_spawn_search),
 * and only the server thread owns struct server_local. One outstanding
 * request. svin_spawn_provider() is true while a local server is up
 * (svin_set_local_motion); without one the create flow has no server to ask. */
#define SVIN_SPAWN_DIR_MAX 512
enum svin_spawn_outcome {
	SVIN_SPAWN_PENDING, SVIN_SPAWN_FOUND, SVIN_SPAWN_FAILED,
	SVIN_SPAWN_WORLD_ACTIVE, SVIN_SPAWN_PROVIDER_STOPPED
};
struct svin_spawn_request {
	int64_t seed;
	bool superflat;
	bool start_nether;
};
struct svin_spawn_result {
	uint64_t ticket;
	enum svin_spawn_outcome outcome;
	float world_spawn[3];
	float player_spawn[3];
};
uint64_t svin_spawn_request(const char* dir, const struct svin_spawn_request* r);
bool svin_spawn_read(uint64_t ticket, struct svin_spawn_result* result);
bool svin_spawn_release(uint64_t ticket);
bool svin_spawn_take(uint64_t* ticket, const char** dir,
					 struct svin_spawn_request* r);
bool svin_spawn_finish(uint64_t ticket, enum svin_spawn_outcome outcome,
					   const float world_spawn[3], const float player_spawn[3]);
bool svin_spawn_provider(void);
/* Main thread, on its bounded wait expiring: ask the running search to stop
 * (its next probe fails, the server half discards the temp world and
 * finishes FAILED). The server half polls svin_spawn_cancelled(). */
bool svin_spawn_cancel(uint64_t ticket);
bool svin_spawn_cancelled(void);
/* Main thread: drop a FINISHED result its owner never released (the create
 * flow is the only requester, so a leftover is always its own). */
void svin_spawn_release_stale(void);

#define HEALTH_PER_HEART 16
/* Packet8UpdateHealth boundary adapter: Beta's wire health is half-hearts
 * (2 per heart, full = 20 -- EntityPlayer ctor `this.health = 20`; the client
 * applies the short raw: NetClientHandler.handleHealth ->
 * setHealth(healthMP) [740c583 Packet8UpdateHealth]). CaveWii's internal
 * scale is HEALTH_PER_HEART per heart, so one wire half-heart =
 * HEALTH_PER_HEART/2 internal units. Convert in int32 (a hostile server can
 * send 32767; x8 overflows int16), clamp to MAX_PLAYER_HEALTH exactly like
 * server_local_set_player_health, and floor dead-or-below at 0 so the death
 * gate (health <= 0) still fires on any wire value <= 0. */
static inline int16_t health_wire_to_internal(int16_t wire) {
	if(wire <= 0)
		return 0;
	int32_t scaled = (int32_t)wire * (HEALTH_PER_HEART / 2);
	return scaled > MAX_PLAYER_HEALTH ? MAX_PLAYER_HEALTH : (int16_t)scaled;
}
// Beta Entity.onEntityUpdate: an entity below y = -64 is instantly killed.
#define VOID_KILL_Y -64.0
// A failed center-section install gets this many complete restream attempts.
// The following NACK is terminal and leaves the input barrier fail-closed.
#define INPUT_HANDOFF_RETRY_MAX 3
/* ACK alone does not replenish recovery attempts: require five seconds of
 * stable server updates so repeated fail/rebuild/fail episodes stay bounded. */
#define INPUT_HANDOFF_CLEAN_TICKS 100
/* Maximum forward distance between two accepted cumulative minecart-input
 * snapshots. At the existing three-client-ticks-per-server-tick catch-up cap,
 * 64 steps is roughly one second of recovery room; a larger gap rotates the
 * ride epoch instead of synthesizing an unbounded impulse. Tuning candidate
 * pending real-Wii queue-pressure observation. */
#define MINECART_CONTROL_MAX_GAP 64u

/* Local mailbox protocol only; never encoded as a Beta packet. */
enum local_guest_action {
	LOCAL_GUEST_JOIN, LOCAL_GUEST_ACCEPT, LOCAL_GUEST_CONFIRM,
	LOCAL_GUEST_CANCEL, LOCAL_GUEST_LEAVE,
	/* Gameplay admission (experimental local split-screen). Requested by the
	 * ATTACHED guest only; the server answers GAMEPLAY_READY with the body's
	 * entity id, or GAMEPLAY_DENIED. Appended so every existing wire value is
	 * unchanged. */
	LOCAL_GUEST_GAMEPLAY,
	LOCAL_GUEST_RESPAWN_READY
};
enum local_guest_result {
	LOCAL_GUEST_OFFER, LOCAL_GUEST_INSTALLED, LOCAL_GUEST_ATTACHED,
	LOCAL_GUEST_LEFT, LOCAL_GUEST_REJECTED, LOCAL_GUEST_LEAVE_FAILED,
	LOCAL_GUEST_GAMEPLAY_READY, LOCAL_GUEST_GAMEPLAY_DENIED,
	/* Authoritative snap-back for an admitted guest body whose movement sample
	 * was rejected. Carries the last accepted position/rotation AND the new
	 * correction_seq the guest must echo. Appended so every existing value is
	 * unchanged. */
	LOCAL_GUEST_CORRECTION,
	LOCAL_GUEST_RESPAWN_LOADING,
	/* An accepted current death-card request reached the server but stopped
	 * before any drop/body mutation. RETRY keeps a PREPARED plan when
	 * respawn_epoch is nonzero; TERMINAL is the nonwrapping counter ceiling.
	 * Appended: preserve every established local-mailbox value. */
	LOCAL_GUEST_RESPAWN_RETRY,
	LOCAL_GUEST_RESPAWN_TERMINAL,
	/* residency-1005/5: a LOADING plan (respawn_epoch) TERMINATED at its
	 * bound without committing -- its arrival terrain never loaded. Distinct
	 * from RETRY, which a LOADING client ignores as a stale reply to the
	 * consumed press. The plan is gone; the death screen offers a retry.
	 * Appended: preserve every established local-mailbox value. */
	LOCAL_GUEST_RESPAWN_LOADING_FAILED,
	/* residency-1005/6 R2: a confirmed Join's terrain wait (the body's saved
	 * column never published) TERMINATED at its bound. This Join generation
	 * can no longer be admitted; the window session stays ATTACHED until its
	 * Leave, and a deliberate re-Join restores the saved record again.
	 * Appended: preserve every established local-mailbox value. */
	LOCAL_GUEST_JOIN_LOADING_FAILED
};

/* Chat composer draft cap [740c583 GuiChat.keyTyped: message.length() < 100];
 * the composer trims before it submits, so the wire never sees more. */
#define SRPC_CHAT_TEXT_CAP 100

enum server_rpc_type {
	SRPC_PLAYER_POS,
	/* Reliable/coalesced movement-state edge. PLAYER_POS repeats these fields
	 * but may be dropped under pressure; this snapshot prevents stale sneak or
	 * onGround from surviving indefinitely on the authoritative server. */
	SRPC_PLAYER_STATE,
	SRPC_LOAD_WORLD,
	SRPC_ENTITY_ATTACK,
	// Right-click USE on a targeted entity (Beta Packet7UseEntity flag 0 ->
	// NetServerHandler.func_6006_a -> EntityPlayer.useCurrentItemOnEntity).
	// Server-authoritative M4 animal interactions: cow milking, sheep shearing,
	// dye-on-sheep. Mirrors SRPC_ENTITY_ATTACK's shape; the server validates the
	// target + the Beta 6-block reach (distSq < 36) before dispatching
	// entity-interact-first-then-item-fallback. [740c583: NetServerHandler:475]
	SRPC_ENTITY_USE,
	/* Cumulative minecart throttle authored by the RIDER. The transport may
	 * coalesce snapshots for one cart+ride epoch; the server derives the bounded
	 * delta from the last accepted counters, so a full inbox delays input without
	 * silently losing acceleration steps. The throttle itself is the documented
	 * Wii-controller playability deviation (Beta couples no rider motion into
	 * minecarts); routing it through the authority is what makes the persisted
	 * cart honest. */
	SRPC_MINECART_CONTROL,
	SRPC_UNLOAD_WORLD,
	SRPC_HOTBAR_SLOT,
	SRPC_BLOCK_PLACE,
	SRPC_BLOCK_DIG,
	SRPC_WINDOW_CLICK,
	SRPC_WINDOW_CLOSE,
	SRPC_TOGGLE_PAUSE,
	// Use the held item into open air (no block/entity targeted). Currently the
	// PLAYER BOW (audit #17c): an instant-fire shot. `charge` (0..1) is carried for
	// API symmetry with a possible future draw adaptation, but Beta 1.7.3's bow is
	// instant-fire (no charge), so the server maps any valid use to a fixed-speed
	// shot. The Wii control layer sends charge=1.0 on the IB_ACTION2 press.
	SRPC_USE_ITEM,
	// Drop the held cursor item outside the window (Beta Container.slotClick
	// slotId == -999): right=false drops the whole stack, right=true drops one.
	// The window_click.slot field is uint8_t and can't carry Beta's -999 sentinel,
	// so this is a distinct RPC rather than an overloaded slot.
	SRPC_WINDOW_DROP,
	// In-world drop of the held HOTBAR item during gameplay (Beta 'Q' ->
	// EntityPlayer.dropCurrentItem). all=false drops one (the Q default), all=true
	// drops the whole stack (Ctrl+Q). Distinct from SRPC_WINDOW_DROP (the GUI cursor).
	SRPC_DROP_ITEM,
	// FIFO acknowledgement emitted by the client while consuming an authoritative
	// respawn RETURN_INGAME. It separates dead-body input already in flight from
	// fresh-body input authored after the client installed the new gameplay epoch.
	SRPC_INPUT_READY,
	/* A full voxel-column allocation failed after the prior input epoch was
	 * already acknowledged. The epoch tags the last snapshot the client fully
	 * installed; the server accepts it only in that same stable generation and
	 * restarts the voxel stream without replacing player/window ownership. */
	SRPC_STREAM_RECOVER,
	/* Idempotent minecart ride-state request (F3). Replaces the toggle semantics
	 * of SRPC_ENTITY_USE for carts: `desired_occupied` is the target state, not
	 * a toggle, so duplicates are safe. `input_epoch` tags the world/input
	 * generation so the server rejects stale-world requests; `request_seq`
	 * correlates the authoritative occupancy reply. Has its own dedicated
	 * pending reserve so a full ordinary-action backlog cannot reject it. */
	SRPC_MINECART_RIDE_INTENT,
	/* Explicit respawn request from the death screen's Respawn button (Beta
	 * GuiGameOver id 1 -> EntityPlayer.respawnPlayer -> Minecraft.respawn).
	 * No payload: the server validates against its own dead-player state
	 * (health <= 0, no world switch in flight) so stale/duplicate presses are
	 * no-ops. The death->respawn transaction itself is unchanged from the old
	 * lethal-hit path; only its trigger moved behind this request. */
	SRPC_RESPAWN,
	/* Sign-editor set operation. Appended so every existing in-process RPC
	 * ordinal remains stable; this enum is an internal provider boundary, not
	 * the Beta packet id space. */
	SRPC_SIGN_SET,
	/* Creative phase-2 item-picker grant (NET-NEW, docs/creative_mode.md;
	 * handler lives in server_creative.c, not server_local.c). Appended,
	 * ordinal-stable like SRPC_SIGN_SET above. */
	SRPC_CREATIVE_GIVE,
	SRPC_LOCAL_GUEST, /* local-only extension; preserve existing RPC numeric IDs */
	SRPC_WINDOW_TRANSACTION, // client response to a rejected Beta Packet106
	SRPC_WINDOW_SORT_ADMIT, /* local-only query; never serialized to Beta */
	/* Local authoritative selected-recipe transaction. A Beta remote server has
	 * no selected-recipe packet; lane 4 exposes this only for local providers. */
	SRPC_RECIPE_CRAFT,
	/* Chat / command composer submit (screen_chat.c): ONE trimmed, non-empty
	 * Beta message of at most SRPC_CHAT_TEXT_CAP characters [740c583
	 * GuiChat.keyTyped -> EntityClientPlayerMP.sendChatMessage ->
	 * Packet3Chat]. Commands ("/register ...") are plain chat the server
	 * interprets. The remote provider sanitizes + encodes it; server_local
	 * ignores it (Beta SP's EntityPlayer.sendChatMessage is a no-op).
	 * Appended, ordinal-stable like SRPC_SIGN_SET. */
	SRPC_CHAT_SEND,
	/* LIVE experimental-split-screen switch (Astra option A, S-A1). The owner
	 * asks the server to apply the split switch WITHOUT a restart; the server
	 * thread performs the allocate/arm transition between ticks and answers
	 * exactly one CRPC_SPLIT_STATE. `world_generation` is the client's last
	 * ACKed input epoch (clin_input_epoch) -- the same stable-generation stamp
	 * SRPC_STREAM_RECOVER and SRPC_MINECART_RIDE_INTENT carry -- so a request
	 * authored before an unload/reload is rejected instead of applied to the
	 * replacement world. `request_seq` correlates the ack. A request that
	 * matches the current state is a no-op ack, so retries are idempotent.
	 * Local-only: server_remote never encodes it. Appended, ordinal-stable. */
	SRPC_SPLIT_SET,
	/* The local player swung its arm (card m1, docs/intent/m1-mp-arm-swing.md;
	 * the call sites and their Beta citations are in game/mp_arm_swing.h).
	 * Beta's EntityClientPlayerMP.swingItem sends Packet18Animation(self, 1)
	 * [EntityClientPlayerMP.java:118-121]; EntityPlayerSP sends nothing, so
	 * the client only emits this in a remote session and server_local
	 * consumes it as a no-op. No payload. Appended, ordinal-stable. */
	SRPC_ARM_SWING,
	/* Split-dims A2-T admission (lane A's messages; client producers in
	 * client_admission.c): the answer to CRPC_CAPACITY_REQUEST, and the
	 * one-shot landing-ready report for a CRPC_LANDING_ARM. Appended,
	 * ordinal-stable. */
	SRPC_CAPACITY_REPLY,
	SRPC_LANDING_READY,
	/* Split-dims B2 gate 2: a world-addressed terrain resend request. The
	 * client lost a column of world {world_index, world_generation} that the
	 * server had marked sent (client_world_payload.h resend marker); the server
	 * invalidates that world's client stream claims and answers with a
	 * CRPC_STREAM_RESET stamped for that world, then re-streams it. A request
	 * for a world that is not live, not this generation, has no client
	 * route, or is player 0's current world is ignored (the marker stays; a
	 * later reset/release clears it).
	 * Appended, ordinal-stable. */
	SRPC_WORLD_RESEND,
};

struct server_rpc {
	enum server_rpc_type type;
	union {
		struct { uint8_t window; uint32_t ticket; bool chest; } window_sort;
		/* Craft one batch of a registered recipe. `recipe_index` is the flat
		 * numbering documented in recipe_craft.h; `player` is the OWNING local
		 * player and `window` its open crafting window -- the server rejects
		 * the request outright when either does not match the actor the queue
		 * envelope resolved, so a split-screen guest can never drive the
		 * host's inventory. `ticket` is a per-client monotonic id; the server
		 * applies each one at most once. */
		struct {
			uint32_t ticket;
			uint16_t recipe_index;
			uint8_t player;
			uint8_t window;
		} recipe_craft;
		struct {
			uint64_t ticket;
			enum local_guest_action action;
			uint8_t owner;
			uint64_t generation, respawn_epoch;
		} local_guest;
		struct {
			uint8_t owner;
			uint64_t generation, respawn_epoch;
		} respawn;
		struct {
			double x, y, z;
			float rx, ry;
			float vel_y;
			// Rider steering SOURCE (docs/deviations.md "Boat rider
			// steering"): the local player's own current horizontal input
			// velocity [740c583: Entity motionX/Z, as re-derived every tick
			// while boat-mounted]. Continuous/idempotent like vel_y above,
			// NOT the minecart's discrete step-counter protocol. Consumed
			// server-side ONLY while s->player.riding_boat (stored into
			// struct server_player.rider_vel_x/z, distinct from that
			// struct's own server-authored KNOCKBACK vel_x/z) -- harmless
			// when not riding, since nothing reads it otherwise. Normal
			// on-foot horizontal movement stays client-authoritative, same
			// as before this field existed.
			float vel_x, vel_z;
			bool on_ground;
			bool sneaking;
			// Echo of the last authoritative CRPC_PLAYER_POS correction installed by the client.
			uint32_t correction_seq;
			/* V6 F1 round 3 (ASTRA-F1-DESIGN-RULING 5-7): the movement
			 * CREDIT this local sample carries -- local-only metadata,
			 * never encoded for a Beta server. Set by svin_credit_publish;
			 * zero/false on every remote or uncredited sample. */
			bool credited;
			uint8_t credit_slot;
			uint32_t credit_epoch;
			uint64_t credit_gen;
		} player_pos;
		struct {
			bool on_ground;
			bool sneaking;
			uint32_t correction_seq;
			/* MP-BED: a request counter bumped each time the player presses
			 * Beta's GuiSleepMP "Leave Bed" (GuiSleepMP.java:45-49 sends
			 * Packet19EntityAction state 3). The remote provider emits one
			 * Packet19(3) per new value; the local server never reads it
			 * (single-player Beta has no leave-bed control). 0 = never. */
			uint32_t leave_bed_seq;
		} player_state;
		struct {
			string_t name;
		} load_world;
		/* IMP-QUIT-UNSAVABLE: SRPC_UNLOAD_WORLD. Zero (every existing sender)
		 * = the ordinary all-or-nothing Save and quit. accept_unsaved = the
		 * player answered "Quit anyway" to the unsaved-chunk dialog: the
		 * server may quit without exactly the chunks it named (count, first
		 * chunk x/z) -- never more, never different ones. */
		struct {
			bool accept_unsaved;
			int32_t chunk_x, chunk_z;
			uint32_t count;
		} unload_world;
		struct {
			w_coord_t x, y, z;
			enum side side;
			// The client's own held-hotbar-item snapshot, mirrored from the same
			// inventory_get_hotbar_item() read the caller already performs for its
			// place-animation trigger. server_local ignores this (it derives the
			// held item from its own authoritative inventory); server_remote needs
			// it verbatim to fill Packet15Place's ItemStack tail. [740c583
			// Packet15Place] [740c583 PlayerControllerMP.sendPlaceBlock]
			struct item_data held_item;
			bool has_held_item;
		} block_place;
		struct {
			bool finished;
			w_coord_t x, y, z;
			enum side side;
			/* R1d: the client's id for ONE logical dig-start (Beta
			 * PlayerControllerMP.clickBlock's packet): process-monotonic,
			 * never 0 from the in-game client. The local server drops a
			 * repeated start carrying the actor's last accepted id, so a
			 * duplicate can never run Block.onBlockClicked twice. 0 =
			 * unsequenced (scripts/tests): always accepted. Not encoded
			 * for a remote Beta server (Packet14 has no such field). */
			uint32_t start_seq;
		} block_dig;
		struct {
			size_t slot;
		} hotbar_slot;
		struct {
			uint8_t window;
			uint8_t slot;
			bool right_click;
			bool quick_move;
			bool sort_chest; /* local admission guard; no extra wire bytes */
			struct item_data expected; // Container.slotClick return (ordinary: pre-click slot)
			uint16_t action_id;
		} window_click;
		struct {
			uint8_t window;
			uint8_t slot;
			uint8_t ch;
			uint16_t action_id;
			// Backing block position, mirrored from struct inventory's own
			// x/y/z at the emitter (windowc_get_latest(...)->x/y/z). server_local
			// ignores these (it already knows the position via its own
			// active_inventory); server_remote needs them to compose Beta's
			// whole-sign Packet130UpdateSign on window close, since Beta edits
			// signs outside the window/container system entirely and the wire
			// packet carries a position server_local's per-cell model never did.
			// [740c583 Packet130UpdateSign] [740c583 GuiEditSign.onGuiClosed]
			w_coord_t x, y, z;
		} sign_set;
		struct {
			uint8_t window;
		} window_close;
		struct {
			uint8_t window;
			bool right; // false = drop whole held stack, true = drop one
			uint16_t action_id;
			struct item_data expected; // -999 always returns null
		} window_drop;
		struct {
			uint8_t window;
			uint16_t action_id;
			bool accepted;
		} window_transaction;
		struct {
			uint32_t entity_id;
		} entity_attack;
		struct {
			uint32_t entity_id;
			// BOAT-ONLY (adversarial-review finding #6): the state the client
			// WANTS the boat's occupancy to become, computed once from the
			// client's own not-yet-corrected local view of e->data.boat.occupied
			// -- NOT a toggle. Makes a duplicate press (a double-click racing
			// ahead of the first round trip) idempotent instead of flipping the
			// mount back off, without needing the minecart's epoch/sequence
			// machinery (mirrors SRPC_MINECART_RIDE_INTENT's desired-state
			// principle at a scale boats actually need). Ignored by every other
			// SRPC_ENTITY_USE target (monster interact, legacy minecart traffic).
			bool desired_occupied;
		} entity_use;
		struct {
			uint32_t entity_id;
			uint32_t ride_epoch;
			uint32_t input_seq;
			uint32_t positive_steps;
			uint32_t negative_steps;
		} minecart_control;
		struct {
			float charge; // 0..1 draw level (b1.7.3 bow ignores it: instant fire)
			// Same held-item mirror as block_place, for Packet15Place's air-use
			// sentinel form (-1,-1,-1,255 + held stack). [740c583 Packet15Place]
			// [740c583 PlayerControllerMP.sendUseItem]
			struct item_data held_item;
			bool has_held_item;
		} use_item;
		struct {
			bool all; // false = drop one (Beta 'Q'), true = drop the stack (Ctrl+Q)
		} drop_item;
		struct {
			uint32_t epoch;
			/* false = snapshot installed/ACK; true = center section missing,
			 * retain the barrier and restream before issuing a new epoch. */
			bool retry;
		} input_ready;
		struct {
			uint32_t epoch;
		} stream_recover;
		struct {
			uint8_t world_index;
			uint32_t world_generation;
		} world_resend;
		struct {
			uint32_t entity_id;
			uint32_t input_epoch;
			uint32_t request_seq;
			bool desired_occupied;
		} minecart_ride_intent;
		/* SRPC_CREATIVE_GIVE: item id + damage/color variant to grant. The
		 * server validates against the registry and its own creative_mode
		 * flag; count is server-chosen (max_stack), never client-supplied. */
		struct {
			uint16_t id;
			uint16_t durability;
		} creative_give;
		struct { char text[SRPC_CHAT_TEXT_CAP + 1]; } chat_send;
		/* SRPC_SPLIT_SET: the DESIRED state, not a toggle, so a duplicate
		 * press is idempotent. request_seq must be nonzero (0 is reserved for
		 * "not a correlated request" and is rejected). */
		struct {
			bool enabled;
			uint32_t world_generation;
			uint32_t request_seq;
		} split_set;
		struct {
			uint32_t request_id;
			uint8_t result; /* enum clin_capacity_result */
			uint32_t ticket;
		} capacity_reply;
		struct {
			uint32_t token;
			uint8_t slot;
			uint32_t generation;
		} landing_ready;
	} payload;
};

/* Local transport metadata, never a Beta packet or client RPC payload field.
 * Slot 0/generation 0 is the existing primary provider. Guest tokens are issued
 * on the server thread and invalidated before their actor can be released. */
struct server_rpc_source {
	uint8_t slot;
	uint64_t generation;
};
static inline bool svin_source_equal(struct server_rpc_source a,
	struct server_rpc_source b) {
	return a.slot == b.slot && a.generation == b.generation;
}
bool svin_init(void);
/* Arm/disarm the experimental local split-screen accept class at the QUEUE
 * boundary (main thread before the server thread starts, or the server thread
 * itself between ticks; svin_init leaves it off). While off, a slot-1
 * SRPC_PLAYER_POS is rejected by svin_rpc_send_from exactly as it was before
 * split-screen existed -- no inbox token, no dispatch.
 * Owned by server_local's split transition -- the boot setter, the live
 * SRPC_SPLIT_SET handler and server_local_split_policy_apply, which all route
 * through one helper; nothing else may call it. The armed value is "switch on
 * OR an admitted guest body still streams its own movement", so a live OFF
 * blocks new joins without cutting the transport out from under a guest that is
 * already playing. */
void svin_set_split_accept(bool enabled);
void svin_process_owned_messages(void (*process)(struct server_rpc_source,
	struct server_rpc*, void*), void* user, bool block);
/* v5: the local server's drain. `admit` is asked, in FIFO order, whether
 * each message may be processed NOW; a declined message (and every later one
 * of its source in this drain) is held -- token and order kept -- and
 * offered first by the next call. Never blocks. See server_interface.c. */
void svin_process_owned_messages_admitted(
	void (*process)(struct server_rpc_source, struct server_rpc*, void*),
	bool (*admit)(struct server_rpc_source, const struct server_rpc*, void*),
	void* user);
/* Messages currently held by the admitted drain (diagnostic / tests). */
size_t svin_held_count(void);

/* ===== V6 F1: LOCAL production-to-replay losslessness ===================
 * MODE. svin_init leaves local-motion mode OFF: SRPC_PLAYER_POS that finds no
 * inbox token is dropped and counted, the remote/legacy contract (a Beta
 * server's movement packets are re-sent from fresh state; server_remote's
 * transport owns its own cadence). server_local_start arms it ON (main
 * thread, before the server thread exists) and server_local_shutdown /
 * a remote provider start disarm it, so only the LOCAL provider -- whose
 * server replays one physics step per accepted sample -- gets the lossless
 * class below. Main/client thread only, like every queue mutation here. */
void svin_set_local_motion(bool enabled);
bool svin_local_motion(void);
/* Backpressure probe the PRODUCER asks BEFORE authoring a sample (before any
 * irreversible progress: cadence advance, last_pos_update). true = a
 * SRPC_PLAYER_POS from `source` would be accepted now (direct or into its
 * owner's bounded motion reserve). Always true with local-motion mode off
 * (the remote path keeps its drop semantics). */
bool svin_motion_room(struct server_rpc_source source);
/* Pending (client-side, not yet in the inbox) motion samples of one owner. */
size_t svin_motion_pending_count(unsigned slot);
/* Local motion samples refused because the owner's reserve was full: the
 * producer contract (svin_motion_room first) makes this 0 in production; it
 * is counted, never silent. Distinct from svin_dropped_count. */
uint32_t svin_motion_refused_count(void);

/* The v6 admitted drain. `admit` answers per message, in FIFO order:
 * ADMIT (process now), DEFER (this update's budget is spent: hold the message
 * and every later one of its source; the next drain resumes) or
 * DEFER_STORAGE (the message needs server-side storage that only a later
 * update can free). For a DEFER_STORAGE source the drain offers the source's
 * later PROGRESS CONTROLS (`control`: pause/resume, leave/unload) a route
 * past the held messages, so the update that frees the storage can happen:
 *  - before the control runs, every overtaken held message of that source
 *    that carries no stamp yet is stamped with `context(source)` -- the
 *    server state the message would have been processed under in FIFO order
 *    (nonzero; read back by its handler through svin_dispatch_context());
 *  - `lifetime(source)` is read before and after the control runs; if it
 *    changed (the control ended that source's lifetime: unload, leave), the
 *    overtaken messages are CANCELLED -- never processed; `cancel` is told
 *    (payload ownership), tokens returned, svin_cancelled_count++. A
 *    control that did not end the lifetime (a pause toggle, a refused
 *    leave) cancels nothing: the overtaken messages run later, stamped.
 * Gameplay FIFO is preserved: non-control messages never reorder among
 * themselves, controls never reorder among themselves. A control still
 * spends the update budget through `admit`. Never blocks. */
enum svin_admit_result { SVIN_ADMIT, SVIN_DEFER, SVIN_DEFER_STORAGE };
struct svin_drain_ops {
	void (*process)(struct server_rpc_source, struct server_rpc*, void*);
	enum svin_admit_result (*admit)(struct server_rpc_source,
		const struct server_rpc*, void*);
	bool (*control)(struct server_rpc_source, const struct server_rpc*, void*);
	uint32_t (*context)(struct server_rpc_source, void*);
	uint64_t (*lifetime)(struct server_rpc_source, void*);
	void (*cancel)(struct server_rpc_source, struct server_rpc*, void*);
};
void svin_process_owned_messages_ops(const struct svin_drain_ops* ops,
	void* user);
/* Stamp of the message currently being processed by the v6 drain (0 = none:
 * processed in FIFO order under the live state). Server thread only. */
uint32_t svin_dispatch_context(void);
uint32_t svin_cancelled_count(void);
/* ===== V6 F1 round 3: end-to-end movement CREDITS (ASTRA-F1-DESIGN-RULING)
 * One credit = one local movement sample, from the reservation the
 * producer takes BEFORE the owner's physics tick runs, through client
 * pending, inbox/held, the server's actor queue and replay, until its
 * terminal settlement (rejected / settled without an event / replayed /
 * lifetime-cancelled). Per owner at most SVIN_MOTION_CREDITS (101 =
 * ACTOR_MOTION_QUEUE) are outstanding, and a reservation also reserves one
 * slot of the owner's pending motion reserve, so a reserved sample can always
 * be published (the physical transport limit -- reserve + 16 shared tokens
 * -- is smaller than 101 while the consumer is stopped; disclosed).
 * Identity: (local-session epoch, owner slot, owner generation); a release
 * whose identity is no longer the owner's current one is ignored (counted).
 * Reserve / publish / abandon: main thread. Release: either thread (locked).
 * With local-motion mode off every reservation succeeds, nothing is counted
 * and publish is the plain remote send. */
#define SVIN_MOTION_CREDITS 101
bool svin_credit_reserve(struct server_rpc_source source);
#ifdef CAVEWII_SERVER_LOCAL_TEST
extern void (*svin_test_credit_publish_hook)(struct server_rpc_source,
	const struct server_rpc*);
#endif
/* Publish the sample of the tick the reservation covered; never refused. */
void svin_credit_publish(struct server_rpc_source source, struct server_rpc* call);
/* The reserved tick did not execute: return the credit unused. */
void svin_credit_abandon(struct server_rpc_source source);
/* Terminal settlement of one credited sample (no-op if uncredited/stale). */
void svin_credit_release(const struct server_rpc* call);
void svin_credit_release_tag(unsigned slot, uint32_t epoch, uint64_t gen,
	unsigned n);
/* Enqueue invariant: a differing old body tag must belong to an ended lifetime. */
bool svin_credit_old_tag_stale(unsigned slot, uint32_t epoch, uint64_t gen);
uint32_t svin_credit_outstanding(unsigned slot);
uint32_t svin_credit_stale_releases(void);
/* The host body's lifetime changed on the client (world reset / respawn
 * install): its later credits are a new identity; main thread. */
void svin_credit_host_lifetime_bump(void);
/* Server handler side, inside a dispatch of a credited PLAYER_POS: the sample
 * became an actor-queue event, so its credit now belongs to that event
 * (released at replay/reset). untake: the event was settled away again in
 * the same dispatch. Otherwise the drain releases it when the handler
 * returns. */
bool svin_dispatch_credit_take(uint32_t* epoch, uint64_t* gen);
void svin_dispatch_credit_untake(void);

/* V6 F1 round 2 (SOL-REVIEW-F1 P1-2): the server is about to change the
 * SHARED pause. Every message it has already received and still holds keeps
 * the state in force when it arrived: each unstamped held message whose
 * source slot != except_slot (-1 = none) is stamped with `ctx`. The toggling
 * source is excepted because its own held messages FOLLOW the toggle in its
 * FIFO (the progress route stamps the ones that precede it). Server thread. */
void svin_stamp_received(int except_slot, uint32_t ctx);
/* V6 F1 lifecycle: the local provider's consumer has exited
 * (server_local_shutdown, after the thread join; main thread). Every token
 * the admitted drain still HOLDS goes back to the pool (cancelled; a
 * LOAD_WORLD name is released), every queued local SRPC_PLAYER_POS sample is
 * dropped (its session ended; a later provider must never replay it), and
 * local-motion mode is disarmed. Returns the number of messages retired. */
size_t svin_retire_local(void);
/* Cross-thread diagnostic: 0 when idle; otherwise low 16 bits are RPC type+1
 * and bits 16..23 are the owning source slot. Never a gameplay gate. */
uint32_t svin_active_owned_rpc_marker(void);
/* Guest transport admits owner-routed container transactions, recipes and sign edits. Other
 * gameplay verbs require actor-aware dispatch before they can be enabled. */
bool svin_rpc_send_from(struct server_rpc_source source, struct server_rpc* call);
void svin_process_messages(void (*process)(struct server_rpc*, void*),
						   void* user, bool block);
// Returns true when delivered/queued (or coalesced), false when the bounded
// transport rejected it. Window-click callers must roll back their optimistic
// revision on false; lifecycle CLOSE has reserved FIFO capacity.
bool svin_rpc_send(struct server_rpc* call);

// Retry queued (reliable) messages against the inbox; called from clin_update
// every frame and from every svin_rpc_send. Client/main thread only.
void svin_rpc_flush_pending(void);

// Total messages waiting for inbox room (actions plus reserved/coalesced
// ownership snapshots).
size_t svin_pending_count(void);
// Main-thread ownership query: true while this exact respawn ACK is still in
// the client-side pending ring (and therefore has not reached the server FIFO).
bool svin_input_ready_pending(uint32_t epoch);

/* S1-a2 / Astra gate-2 D4 teardown release: drop every pending entry belonging
 * to a local split guest session, releasing its per-owner action reservation in
 * the same pass, and return how many were dropped. `generation` 0 means "every
 * slot-1 entry"; a nonzero value drops only that session's. Slot 0 is refused
 * (returns 0) -- the host has no session boundary. Relative FIFO order of the
 * surviving entries is preserved. CLIENT/MAIN thread only, like every other
 * function on this queue; see the definition for why the server thread must
 * not call it and which call site (S1-b) owns it. */
size_t svin_rpc_drop_owner(unsigned slot, uint64_t generation);

// Count of client->server messages rejected under pressure. Normally this is
// only periodic/idempotent SRPC_PLAYER_POS; a truly wedged server can exhaust
// the 64-action queue, in which case the caller gets false (GUI callers undo
// their optimistic revision). Surfaced on the F3 far row as "rpcdrop".
uint32_t svin_dropped_count(void);

#endif
