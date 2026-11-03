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

#ifndef CLIENT_INTERFACE_H
#define CLIENT_INTERFACE_H

#include "../entity/entity.h"
#include "../item/items.h"
#include "../item/window_container.h"
#include "piston_moving_transport.h"
#include "recipe_craft.h"
#include "../world.h" // transitively defines CHUNK_SIZE + WORLD_HEIGHT
                       // (world.h is the canonical entry point for the
                       // world.h/chunk.h include cycle), used by the slot
                       // struct below.
#include "../platform/thread.h"

#include "../cglm/cglm.h"

// Pre-allocated buffer set for a single chunk-data RPC. The server
// thread fills one of these (no malloc), hands the pointer to the
// client via a CRPC_CHUNK message, and the client returns the slot to
// the pool after consuming the data. Eliminates the ~80 KB malloc +
// memcpy + free cycle that the old send path performed for every
// chunk load.
//
// Layout note: ids is full-byte (one block id per cell), the other
// three are nibble-packed (one per cell, two cells per byte). Sizes
// match what world_set_block expects to read in clin_chunk.
struct chunk_rpc_slot {
	/* Local-only lease. Remote producers leave this NULL; the payload and
	 * copied slot payload stays identical; full local installs construct a
	 * differential view. Slot release drops the lease before pool return. */
	struct column_backing* backing;
	struct column_revision local_revision; /* ignored by remote ingress */
	uint8_t ids[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT];
	uint8_t metadata[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2];
	uint8_t lighting_sky[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2];
	uint8_t lighting_torch[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2];
	// Beta biome grass/foliage colormap indices, one (i,j) pair per column,
	// laid out [localX*16 + localZ]. The client tints grass/leaves via
	// grasscolor.png[j<<8|i] / foliagecolor.png[j<<8|i].
	uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t grass_j[CHUNK_SIZE * CHUNK_SIZE];
	// Server-authoritative column heightmap [x + z*16] (highest opaque block +1).
	// Sent so the BULK client install (world_build_full_section) can set it directly
	// instead of recomputing per cell -- the old per-cell world_set_block path was
	// ~32K calls/chunk and became the load bottleneck once gen was fast.
	uint8_t heightmap[CHUNK_SIZE * CHUNK_SIZE];
	/* Exact in-flight TileEntityPiston records for this column. The pooled slot
	 * retains a growable compact sidecar at its high-water mark; there is no
	 * simulation-visible simultaneous-piston cap. */
	struct piston_moving_chunk_snapshot piston_moving;
};

// Compact far-ring chunk payload (M1 deep reach). For a chunk BEYOND the full
// view distance the server pure-streams only one visible per-column surface:
// the highest water/lava tile when it lies above the opaque skin, otherwise the
// top non-see-through block (lod_surface_height's predicate), plus id + biome tint.
// ~1 KB vs the ~82 KB chunk_rpc_slot. The client synthesizes a cheap voxel surface
// from this and renders it through the unchanged lod=1 skin path. Layout
// [localX*CHUNK_SIZE + localZ], matching chunk_rpc_slot's tint arrays.
struct far_rpc_slot {
	uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE]; // skin-surface world-Y; FAR_NO_SURFACE if none
	uint8_t top_id[CHUNK_SIZE * CHUNK_SIZE]; // block id at skin_y
	uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t grass_j[CHUNK_SIZE * CHUNK_SIZE];
};

// skin_y sentinel for a column with neither a visible water/lava tile nor an
// opaque skin (normally fully air). Visible open water/lava is represented even
// when no seabed lies in range; the client emits no voxel only for this sentinel.
#define FAR_NO_SURFACE 0xFF

// Packet104WindowItems' own faithful range is a Java signed short (0..32767
// entries); this is a deliberately smaller in-process convenience cap for
// CRPC_WINDOW_ITEMS' embedded array, not a wire-format limit -- mp_codec's
// decoder already accepts the full faithful range independently. 128 covers
// the largest realistic Beta container (a double chest's 54 slots) plus the
// player inventory a Container appends after it (36), with headroom.
#define CRPC_WINDOW_ITEMS_MAX_SLOTS 128

#define PLAYER_CORRECTION_POSITION_ONLY  0x01u
#define PLAYER_CORRECTION_ZERO_VEL_X     0x02u
#define PLAYER_CORRECTION_ZERO_VEL_Y     0x04u
#define PLAYER_CORRECTION_ZERO_VEL_Z     0x08u
#define PLAYER_CORRECTION_SET_ON_GROUND  0x10u
#define PLAYER_CORRECTION_ON_GROUND      0x20u
/* Remote Packet12/13 corrections own look as well as position. Kept opt-in so
 * existing local position-only corrections with zero-initialized rotation do
 * not reset the camera. */
#define PLAYER_CORRECTION_SET_ROTATION   0x40u
#define PLAYER_CORRECTION_FINISH_REMOTE_LOAD 0x80u

enum client_rpc_type {
	CRPC_CHUNK,
	CRPC_UNLOAD_CHUNK,
	CRPC_INVENTORY_SLOT,
	// Restore the selected hotbar index after CRPC_WORLD_RESET rebuilt the
	// client inventory window. Item-slot updates cannot carry this state.
	CRPC_PLAYER_HOTBAR_SLOT,
	CRPC_PLAYER_POS,
	CRPC_TIME_SET,
	// Transient HUD toast (handoff P4(a)). Beta surfaces bed refusals/losses via chat
	// (addChatMessage); this engine has no chat, so a 3-second toast is the Wii-playable
	// stand-in. Emit ONLY strings Beta emits -- see block_bed.c / the respawn plan.
	CRPC_HUD_MESSAGE,
	// Beta's sleep state/fade (GuiIngame.java:150-163). The client cannot derive
	// either half: timer 100 is ambiguous (fully asleep OR already awake in the
	// fade-out tail), while sleeping itself lives only on the server. Carry both
	// explicitly so input/physics freeze for exactly the authoritative interval.
	CRPC_SLEEP_STATE,
	// Present loading feedback before a live portal/death transaction performs
	// synchronous source persistence. Unlike WORLD_RESET this is reversible: the
	// old client world and every public alias remain intact until the commit RPC.
	CRPC_BEGIN_WORLD_SWITCH,
	CRPC_WORLD_RESET,
	// The bounded client center-section restream could not produce an installable
	// world. Input remains fail-closed; show an explicit restart-required error.
	CRPC_LOAD_FAILED,
	/* Voxel-only reset for a stable in-play stream recovery. Unlike WORLD_RESET,
	 * this preserves the local player, entity dictionary, windows and dimension. */
	CRPC_STREAM_RESET,
	CRPC_SET_BLOCK,
	/* Atomic id-36 cell + TileEntityPiston state replacement. */
	CRPC_PISTON_MOVING,
	CRPC_WINDOW_TRANSACTION,
	// Furnace burn/cook progress for the open furnace window (Beta
	// Container.sendProgressBarUpdate). Display-only; updates the flame + cook arrow.
	CRPC_WINDOW_PROGRESS,
	CRPC_SPAWN_ITEM,
	CRPC_PICKUP_ITEM,
	// PARTIAL pickup re-sync (review fix 2026-06-11): on_collect drained part
	// of a ground stack into a nearly-full inventory but the entity survived;
	// without this the client's copy keeps the ORIGINAL count and renders a
	// stale 1/2/3/4-sprite stack until the entity finally dies. Cosmetic-only
	// (the server count is authoritative), so a queue-full drop is tolerable.
	CRPC_ITEM_COUNT,
	CRPC_ENTITY_DESTROY,
	CRPC_ENTITY_MOVE,
	/* Immediate authoritative correction for piston-authored non-player
	 * moveEntity state. Ordinary CRPC_ENTITY_MOVE remains presentation/lerp. */
	CRPC_ENTITY_PHYSICS_CORRECTION,
	CRPC_OPEN_WINDOW,
	// A fallible server close left the authoritative window unchanged. Rebuild
	// it if the client had an allocation failure, then apply the following full
	// authoritative snapshot instead of stranding a hidden server container.
	CRPC_REOPEN_WINDOW,
	// Return a still-live client to gameplay after a portal closes its GUI or a
	// fallible no-loss close cancels an unload before world teardown.
	CRPC_RETURN_INGAME,
	CRPC_PLAYER_SET_HEALTH,
	CRPC_SPAWN_MONSTER,
	CRPC_SPAWN_MINECART,
	/* Bounded authoritative minecart correction/ack. Unlike the generic piston
	 * physics correction, this carries the ride epoch and accepted cumulative
	 * input tuple needed to reconcile prediction without losing final input. */
	CRPC_MINECART_SNAPSHOT,
	/* AUTHORITATIVE minecart occupancy. The client predicts mount/dismount for
	 * responsiveness, then sends idempotent SRPC_MINECART_RIDE_INTENT desired
	 * state. Valid current-epoch requests receive this correlated result;
	 * server-authored changes use zero/zero correlation. Spawn initializes
	 * occupancy and this is its only writer afterward -- the 4 Hz physics
	 * snapshot cannot stale-remount a predicted dismount. */
	CRPC_MINECART_OCCUPANCY,
	CRPC_SPAWN_ARROW,
	CRPC_SPAWN_PRIMED_TNT,
	/* Beta's EntityTrackerEntry for TNT uses update interval 10 with velocity.
	 * This bounded authoritative correction keeps the locally simulated client
	 * fuse/trajectory from drifting without emitting smoke from the network path. */
	CRPC_PRIMED_TNT_CORRECTION,
	CRPC_ENTITY_EXPLODE,
	// One-shot cosmetic smoke puff (TNT fuse trail). Like ENTITY_EXPLODE,
	// exists so SERVER tick code never touches the client particle array
	// directly (the render thread owns it).
	CRPC_SMOKE_PUFF,
	/* Beta EntityWolf taming status particles: success hearts / failure smoke.
	 * The render thread owns the particle ring, so the server sends only the
	 * one-shot state and origin. [740c583: EntityWolf.handleHealthUpdate] */
	CRPC_WOLF_FX,
	/* Shake splashes are calculated by the authoritative wolf tick, then
	 * materialized here so only the render thread mutates the particle ring. */
	CRPC_WOLF_SPLASH,
	// S0 audio: one-shot server-caused sound event, byte-modeled on
	// CRPC_SMOKE_PUFF (same fire-and-forget boundary rule: the server tick
	// never touches the client voice pool; clin_process routes this to
	// audio_play3d on the render thread). EVERY server-caused sound goes
	// through here even in the single-process build (thin-client doctrine)
	// — the payload carries the server-rolled Beta volume/pitch, mirroring
	// Beta's World.playSound* -> IWorldAccess boundary. Client-LOCAL sounds
	// (GUI click, own mining hit ticks) bypass it, exactly the paths that
	// bypass World in Beta SP [740c583: PlayerControllerSP.java:74,
	// GuiScreen.mouseClicked:57-59].
	CRPC_SOUND,
	CRPC_FAR_CHUNK,
	// Beta World.rainingStrength/thunderingStrength + the raining/thundering
	// flags, mirroring the CRPC_TIME_SET pattern for global world state: sent
	// every non-loading server tick right after server_weather_tick runs
	// (same-process thin-client transport -- see CRPC_SOUND's own doctrine
	// note above; a few bytes/tick is not a real-socket bandwidth concern).
	// Carries the CURRENT interpolated strength directly rather than a
	// prev/cur pair + a client-side re-derivation, so there is no drift risk
	// between two independent lerp implementations.
	CRPC_WEATHER_SET,
	// Boat-slice (2026-08-13): spawn + periodic drift correction, mirroring
	// entity_primed_tnt.c's shared-physics/tracker-correction shape rather
	// than the minecart's bespoke rail-reconciliation protocol -- the boat's
	// buoyancy/drag physics are deterministic and shared between client and
	// server, so no cumulative-input epoch/sequence machinery is needed.
	CRPC_SPAWN_BOAT,
	CRPC_BOAT_CORRECTION,
	// Painting slice client wiring: a painting never moves/changes state once
	// placed [740c583: EntityPainting has no onUpdate movement, no health/
	// damage accumulation -- attackEntityFrom is an unconditional instant
	// kill], so this is spawn-only, like CRPC_SPAWN_ARROW/CRPC_SPAWN_ITEM --
	// no periodic correction/snapshot RPC exists for it. Carries the SAME
	// fields as Beta's own Packet25EntityPainting (entityId, x/y/zPosition --
	// the WALL block the frame hangs on, direction) plus the engine's own
	// art_index in place of Beta's `title` string (entity_painting.h's
	// existing "table index rather than a Java enum ref" convention).
	CRPC_SPAWN_PAINTING,
	// Noteblock/jukebox-slice: Beta's world event 1005 (RenderGlobal.
	// playAuxSFX case 1005 -- the jukebox record start/stop signal), byte-
	// modeled on CRPC_SOUND's fire-and-forget boundary rule. record_id 0 =
	// stop, nonzero = the inserted record's item id ("start playing that
	// record"). Wired (2026-08-31) to the S3 streaming machinery:
	// record_id 0 -> audio_music_stop_record(), nonzero ->
	// audio_music_play_record_for_item() -- both preempt/resume through the
	// SAME single hardware voice background music uses. Still NO Mojang
	// "13"/"cat" audio is ever bundled -- a client with no imported
	// <pack>/records/ plays nothing, exactly like S3 background music with
	// no imported <pack>/music/ (docs/deviations.md's per-client-import-only
	// legal model). [decompile 740c583: BlockJukeBox.ejectRecord /
	// func_28038_b_, ItemRecord.onItemUse, SoundManager.playStreaming]
	CRPC_RECORD_EVENT,
	/* Protocol-14 Packet6. Appended to preserve every existing in-process RPC
	 * ordinal; this boundary is never serialized. */
	CRPC_WORLD_SPAWN,
	/* S2 boundary-local additions (plan section 6.3: "wire packets with no
	 * adequate RPC today"). Never serialized -- these are the remote
	 * adapter's in-process events, not new Beta packet ids. */
	// Atomic full-window slot replacement (Packet104WindowItems). One
	// message applies every slot in a single clin_process call, which is
	// already atomic from every other CRPC's perspective -- no separate
	// begin/end barrier is needed. [740c583 Packet104WindowItems]
	CRPC_WINDOW_ITEMS,
	// Authoritative remote window close (inbound Packet101CloseWindow).
	// SRPC_WINDOW_CLOSE is the client-initiated opposite direction; this is
	// the server tearing the container down on its own (or acking a close
	// the client already left its screen for). [740c583
	// Packet101CloseWindow] [740c583 NetClientHandler.handleCloseWindow]
	CRPC_WINDOW_CLOSE_REMOTE,
	/* S3 boundary-local additions (plan section 6.3). Never serialized. */
	// Packet20NamedEntitySpawn has no adequate existing RPC: a remote player
	// is a render entity, never players[N]/a pluralized local player.
	// [740c583 Packet20NamedEntitySpawn]
	CRPC_SPAWN_PLAYER,
	// Packet5PlayerInventory targeting a REMOTE entity's equipped slot --
	// distinct from CRPC_INVENTORY_SLOT, which only ever targets the LOCAL
	// player's own gstate.players[0].windows[] containers. [740c583
	// Packet5PlayerInventory]
	CRPC_ENTITY_EQUIPMENT,
	// Packet18Animation applied to a remote entity (arm swing / hurt / wake
	// / crouch code). No existing CRPC carries an animation code generically.
	// [740c583 Packet18Animation]
	CRPC_ENTITY_ANIMATION,
	// MP visual-parity pass (2026-08-31) item 1, widened by the
	// animation-parity pass (2026-09-03) items 3+4: a remote player's
	// sneak/on-fire/riding tri-state, ALL THREE decoded together from the
	// SAME generic Entity DataWatcher flag byte (index 0) carried by Packet40
	// EntityMetadata -- bit 0 (0x01) fire, bit 1 (0x02) sneak, bit 2 (0x04)
	// riding [740c583 Entity.setEntityFlag/getEntityFlag,
	// Entity.isBurning/isSneaking/isRiding]. Sneaking is NOT
	// Packet18Animation's crouch code (see entity.h's
	// entity_remote_player.sneaking field comment for the full citation of
	// why that code is a genuine no-op); riding is NOT Packet39AttachEntity
	// either for an observer (see entity.h's entity_remote_player.is_riding
	// field comment). Was CRPC_ENTITY_SNEAK (one flag) before the
	// animation-parity pass widened the same METADATA decode site to read
	// all three bits of the one byte it already had in hand.
	CRPC_ENTITY_FLAGS,
	/* S4 (plan section 6.3): Packet3Chat has no adequate existing RPC.
	 * CRPC_HUD_MESSAGE is a transient 3s toast and must not silently become
	 * chat history -- this feeds a small bounded display ring instead
	 * (game_state.h chat_ring), never a composer/history UI. [740c583
	 * Packet3Chat] [740c583 NetClientHandler.handleChat] */
	CRPC_CHAT_LINE,
	/* Remote-player render follow-up (2026-08-19): Packet32EntityLook,
	 * Packet33RelEntityMoveLook, and Packet34EntityTeleport's look half all
	 * updated server_remote.c's remote_entity yaw/pitch tracking but never
	 * forwarded it (KNOWN GAP -- "documented S4 residual" at each of those
	 * three translation sites). CRPC_ENTITY_MOVE stays position-only (unlike
	 * CRPC_SPAWN_PLAYER, which already carries an initial yaw/pitch) so a
	 * generic per-entity orientation update needs its own carrier. Applied
	 * client-side to the tracked entity's e->orient (degrees, matching
	 * CRPC_SPAWN_PLAYER's own payload convention) -- currently consumed only
	 * by ENTITY_REMOTE_PLAYER's renderer (client_interface.c gates the same
	 * way CRPC_ENTITY_EQUIPMENT does). [740c583 Packet32EntityLook]
	 * [740c583 Packet33RelEntityMoveLook] [740c583 Packet34EntityTeleport] */
	CRPC_ENTITY_LOOK,
	/* Entity velocity residual (2026-08-26, item 1): Packet28EntityVelocity
	 * [740c583 NetClientHandler.handleEntityVelocity -> Entity.setVelocity]
	 * for the entity kinds whose client tick already integrates e->vel every
	 * frame (item/boat/minecart/arrow/primed_tnt/falling_sand -- the exact
	 * set CRPC_ENTITY_MOVE's remote_base_entity_snap switch already names).
	 * Beta's own class split: EntityOtherPlayerMP overrides onLivingUpdate
	 * and never calls moveEntityWithHeading, so a remote OTHER PLAYER's
	 * velocity has no movement effect at all [740c583
	 * EntityOtherPlayerMP.onLivingUpdate]; EntityLiving (mobs) DOES call
	 * moveEntityWithHeading unconditionally every tick, applying motionX/Y/Z
	 * as real physics [740c583 EntityLiving.onLivingUpdate:642,
	 * .moveEntityWithHeading:459] -- but THIS engine's client_tick_monster
	 * (entity_monster.c) has no e->vel consumer at all for a mob (100%
	 * network-position-approach driven), so forwarding it there would be a
	 * dead store regardless of Beta's own behaviour. Both kinds are skipped
	 * at the server_remote.c translation site; see the readiness doc row. */
	CRPC_ENTITY_VELOCITY,
	/* Client-side sign-text store feed (source/game/sign_text_store.h): the
	 * server sends this per sign, both when a chunk carrying it is first sent
	 * (so the world renderer can query text for a sign it hasn't opened) and
	 * as the edit-echo after a SRPC_SIGN_SET session commits on close. Also
	 * forwarded from an inbound multiplayer Packet130UpdateSign (server_remote.c
	 * MP_EVENT_UPDATE_SIGN). grid is the 4x16 packed, right-space-padded,
	 * no-NULs layout (te_sign convention; sign_text_store.h SIGN_TEXT_LINES/
	 * SIGN_TEXT_COLS) -- duplicated as a raw size here rather than an
	 * #include, matching this header's existing network/game layering (the
	 * network layer does not otherwise depend on source/game). */
	CRPC_SIGN_TEXT,
	// Companion removal for a broken sign (block_sign.c onBreak). Distinct
	// from CRPC_SET_BLOCK (which already tells the client the cell is air) --
	// the sign_text_store entry has no other owner to expire it.
	CRPC_SIGN_TEXT_REMOVED,
	// Batch I: Beta EntityFallingSand, mirroring CRPC_SPAWN_PRIMED_TNT's shape
	// (entity_id/pos/vel) plus the carried block id (BLOCK_SAND or
	// BLOCK_GRAVEL) so the shared render path (entity_falling_sand.c) can draw
	// the right terrain-atlas tile. Sent by server_local in single player and,
	// since next-gaps-0924 card m3, by server_remote_protocol.c for Beta's own
	// wire form, Packet23 vehicle types 70/71 [NetClientHandler.java:109-116]
	// (docs/intent/m3-mp-remote-falling-sand.md).
	CRPC_SPAWN_FALLING_SAND,
	/* Held-map render slice (2026-08-26): delivers one map's Beta colour
	 * byte array (colors[16384], the byte format item_map_update_data
	 * writes) from the server-side map cache to the client so a held map
	 * can actually draw. Pooled slot (16 KB -- a by-value union member
	 * would balloon the whole 16-deep inbox), released by clin_process
	 * after copying into the client mirror. Beta has no such packet in SP
	 * (renderer and world share a process there); this is the engine's
	 * client/server split making Beta's in-process read explicit --
	 * transport-only, no rule change. [740c583 ItemRenderer
	 * .renderItemInFirstPerson, MapItemRenderer.renderMap read
	 * mapData.colors directly] */
	CRPC_MAP_DATA,
	CRPC_LOCAL_GUEST, /* local-only extension; preserve existing RPC numeric IDs */
	CRPC_WINDOW_SORT_ADMIT,
	CRPC_RECIPE_CRAFT,
	/* S5 local same-process eviction boundary: every dynamic entity of ONE
	 * evicted chunk in a single nonblocking admission. The server retires no
	 * entity of that chunk until this envelope is in the inbox, so the client
	 * can never be left rendering a body the server has already freed. The
	 * client OWNS the transferred id array and frees it exactly once --
	 * whether it applies the destroys or discards them. */
	CRPC_ENTITY_DESTROY_BATCH,
	/* Online mounts (docs/mp_mounts_design_2026-09-11.md): the server told
	 * the LOCAL player it now rides `vehicle_id` (attach) or stopped riding it
	 * (detach) [740c583 NetClientHandler.func_6497_a -> Entity.mountEntity].
	 * Appended, ordinal-stable. */
	CRPC_ATTACH_ENTITY,
	/* Authoritative answer to SRPC_SPLIT_SET (Astra option A, S-A1): the state
	 * the SERVER now holds for the experimental local split switch, correlated
	 * by {generation, request_seq} with the request. `ok` false carries a
	 * readable `reason` and means NOTHING changed. Exactly one of these is
	 * emitted per processed request, including the idempotent no-op, so the
	 * requester can always retire its pending toggle. The client stores it and
	 * nothing else: arming input is the caller's job, after it reads an
	 * `ok && enabled` ack for its own request. Appended, ordinal-stable. */
	CRPC_SPLIT_STATE,
	/* Owner-0 same-world respawn control. Local-only, pointer-free, and
	 * appended so no established client RPC ordinal moves. */
	CRPC_HOST_RESPAWN,
	/* lava-aabb-0924/B3: the authoritative burning state (server `fire > 0`,
	 * Beta Entity.isBurning) of ONE local split-screen body, published every
	 * server tick while a guest body is admitted so the OTHER viewport's
	 * partner draw can show and clear its flames. Local-only and pointer-free:
	 * produced solely by server_local.c (host untagged, guest through
	 * clin_rpc_send_local's generation-tagged route), never by server_remote,
	 * never encoded as a Beta packet. Appended so no established client RPC
	 * ordinal moves. */
	CRPC_LOCAL_PLAYER_FIRE,
	/* damage-client-0926: one local body's knockback motion, the local
	 * equivalent of Beta's hit -> Packet28 path. EntityLiving.attackEntityFrom
	 * (EntityLiving.java:333-346) calls setBeenAttacked() and knockBack()
	 * (:392-403), which writes motionX/Y/Z; the server's EntityTrackerEntry
	 * (EntityTrackerEntry.java:124-127) then sends Packet28EntityVelocity to
	 * the tracked entity ITSELF, and the client's handleEntityVelocity
	 * (NetClientHandler.java:165-170) setVelocity()s the player, whose own
	 * moveEntityWithHeading integrates it. G8 (2026-10-05): the client installs
	 * all three components (or ADDS them, payload .add, for the server's
	 * addVelocity pushes) and integrates them with the ordinary local-player
	 * physics (entity_local_player.c), so the player steers through the push;
	 * the server only decays its own copy (server_local_tick_actor_knockback)
	 * and sends no position correction for it. A REMOTE
	 * Beta server's Packet28 for the own entity id (server_remote_protocol.c,
	 * mp-velocity-0926) arrives on this same RPC and installs X/Y/Z, as
	 * EntityPlayerSP does. Pointer-free: host untagged (owner 0), guest through
	 * clin_rpc_send_local's generation-tagged route (owner 1). Appended so no
	 * established client RPC ordinal moves. */
	CRPC_LOCAL_PLAYER_VELOCITY,
	/* Split-dims B2 (Fable B2-A2T F1; lane A's A2-T message, 2026-09-27):
	 * stamped {world_index, world_generation} + payload.world_release.retiring.
	 * The server tells the client to release a world it no longer shows:
	 * retiring == false -- a vacated world drained to zero resident columns
	 * that stays LIVE on the server (Astra A2 plan) -- or retiring == true --
	 * the server world instance is actually retiring. Either way the client
	 * releases that slot's payload, and only if world-bound payloads are ON
	 * and the slot is unviewed, body-less, not player 0's and at exactly that
	 * generation. retiring == true also returns the slot's capacity
	 * reservation and stales a landing armed into that world
	 * (clin_admission_world_released). Appended, ordinal-stable (was
	 * CRPC_WORLD_RETIRE, same ordinal). */
	CRPC_WORLD_RELEASE,
	/* Split-dims A2-T admission (client_admission.h, DRAFT with lane A): the
	 * server asks for client/GPU capacity (op RESERVE / COMMIT / RELEASE) and
	 * arms / cancels a landing-ready report. Replies go through the
	 * registered senders (client_admission.h); the SRPC side is the lane-A /
	 * lane-C transport handoff. Appended, ordinal-stable. */
	CRPC_CAPACITY_REQUEST,
	CRPC_LANDING_ARM,
	/* drown-fx-0927: Beta EntityLiving.onEntityUpdate:124-133 -- when an
	 * actor's air reaches -20 it resets to 0 and spawns EIGHT "bubble"
	 * particles around posX/posY/posZ (the eye for a player) with the
	 * entity's motion, BEFORE the 2-point drowning hit. The server decides
	 * the moment (it owns air); the client spawns the EntityBubbleFX burst
	 * (particle_generate_drown_bubbles). World-scoped cosmetic, like
	 * CRPC_WOLF_SPLASH. Appended, ordinal-stable. */
	CRPC_DROWN_BUBBLES,
	/* Per-owner movement hold during a transfer's source-ring drain. */
	CRPC_TRANSFER_HOLD,
	CRPC_MAP_MARKERS, /* MapData's transient icon list; independent of colors. */
	CRPC_LOCAL_SETTLED_LIGHT, /* local-only immutable owning envelope */
	/* G9 R21 (ASTRA-G9-R21-RING alternative): LOCAL-ONLY ordered packing of
	 * consecutive compatible CRPC_SET_BLOCK / CRPC_SPAWN_ITEM records inside
	 * the server's pending-output ring (client_interface.c). Never produced
	 * by any server code, never sent by the remote path, never reaches
	 * clin_process: the consumer replays every record, in order, as the
	 * CRPC_SET_BLOCK or CRPC_SPAWN_ITEM it was. Appended, ordinal-stable. */
	CRPC_SET_BLOCK_PACK,
	/* Local-authority lightning (server_weather.c): the bolt the server just
	 * constructed, so the client draws the same RenderLightningBolt mesh and
	 * World.field_27172_i sky flash as for an online Packet71 bolt
	 * (graphics/render_lightning.h). By value, pointer-free, world-scoped
	 * cosmetic; packable as an `other` record. Appended, ordinal-stable. */
	CRPC_LIGHTNING_BOLT,
};
/* 12 + 19 * 40 = 772 B <= the 776-B payload union (host and Wii alike; a
 * CRPC_SPAWN_ITEM record and an `other` record are no larger than a
 * CRPC_SET_BLOCK one). */
#define CRPC_SET_BLOCK_PACK_MAX 19u
/* An `other` pack record: 4-byte type + the payload member's bytes. */
#define CRPC_PACK_OTHER_BYTES 36u

enum host_respawn_result {
	HOST_RESPAWN_LOADING,
	HOST_RESPAWN_CORRECTION,
	HOST_RESPAWN_GAMEPLAY_READY,
	HOST_RESPAWN_RETRY,
	HOST_RESPAWN_TERMINAL,
};

/* Longest rejection text CRPC_SPLIT_STATE carries, NUL included. Fixed-size and
 * pointer-free: this reply crosses the same-process FIFO from the server thread,
 * which may not hand the client a pointer it owns. */
#define CRPC_SPLIT_REASON_CAP 48

struct entity_physics_correction_rpc {
	uint32_t entity_id;
	vec3 pos;
	vec3 vel;
	float y_size;
	float fall_distance;
	bool on_ground;
	bool collided_horizontally;
	bool collided_vertically;
	bool in_web;
	bool on_fire;
	/* Typed state used by the matching subclass only. */
	float minecart_speed;
	int8_t minecart_hx;
	int8_t minecart_hz;
	uint8_t minecart_rail_meta;
	uint8_t minecart_last_meta;
	int8_t fuse;
};

/* Complete minecart state shared by the reliable spawn and the bounded
 * correction stream. The transport is same-process and carries pointers to
 * fixed RPC envelopes, but protocol fields still use explicit widths so the
 * ownership/wrap contract is unambiguous. */
struct minecart_state_rpc {
	uint32_t entity_id;
	vec3 pos;
	vec3 vel;
	float y_size;
	float fall_distance;
	/* Beta minecartType: 0 rideable / 1 chest / 2 furnace [740c583:
	 * EntityMinecart.minecartType]. Immutable after spawn -- installed by
	 * clin_apply_minecart_state only when `spawn` is true, single-writer like
	 * occupied/occupant_id/rightClickText below. A periodic CRPC_MINECART_SNAPSHOT
	 * still carries this field (shared struct) but the client never reads it
	 * outside the spawn path. */
	uint8_t minecart_type;
	float speed;
	uint32_t occupant_id;
	uint32_t ride_epoch;
	uint32_t accepted_input_seq;
	uint32_t accepted_positive_steps;
	uint32_t accepted_negative_steps;
	int8_t hx;
	int8_t hz;
	uint8_t rail_meta;
	uint8_t last_meta;
	int16_t damage_taken;
	int8_t shake_ticks;
	int8_t shake_dir;
	bool occupied;
	bool on_ground;
	bool collided_horizontally;
	bool collided_vertically;
	bool in_web;
	bool on_fire;
};

/* Complete boat state shared by the spawn and the periodic tracker
 * correction, mirroring minecart_state_rpc's shape minus the rail/throttle
 * fields the boat has none of. */
struct boat_state_rpc {
	uint32_t entity_id;
	vec3 pos;
	vec3 vel;
	float yaw; // radians; e->orient[0]
	int16_t damage_taken;
	int8_t shake_ticks;
	int8_t shake_dir;
	bool occupied;
	uint32_t occupant_id;
	bool on_ground;
};

/* CRPC_SPAWN_ITEM's payload, named (layout unchanged) so a
 * CRPC_SET_BLOCK_PACK record can hold it (G9 startup packing). */
struct crpc_spawn_item {
	uint32_t entity_id;
	struct item_data item;
	vec3 pos;
	vec3 vel;
};

/* Split-dims B1 (ASTRA-B1-PHASE1-0925 #2, out/B1-rpc-audit.md): every
 * world-scoped client RPC carries ONE common world stamp outside the payload
 * union -- the producing server world's slot and its authoritative generation
 * (struct server_world.world_index / .generation). The client routes the
 * message to that world's context (client_ctx_registry_route) before any
 * payload is touched. world_generation 0 means UNSTAMPED: the producer has not
 * adopted the stamp yet (the stamps land inside the A/C server seams, which B
 * does not edit). Unstamped traffic is routed only while exactly one client
 * world context is live; with two it is dropped as ambiguous, never guessed.
 * Designated initializers leave both fields 0, so every existing producer is
 * unstamped by construction. Actor-routed and session-lifecycle RPCs ignore
 * the stamp (they carry their own identity). */
struct client_rpc {
	enum client_rpc_type type;
	uint8_t world_index;
	uint32_t world_generation;
	union {
		struct {
			uint64_t generation, respawn_epoch;
			uint8_t owner;
			int result; /* enum host_respawn_result */
			double x, y, z;
			float rx, ry;
			uint32_t correction_seq;
			uint8_t correction_flags;
		} host_respawn;
		/* IMP-QUIT-UNSAVABLE: CRPC_RETURN_INGAME. Zero (every other sender) =
		 * a plain refusal. unsaved_chunk = a Save and quit was refused ONLY
		 * because `count` chunk(s) could not be saved (first: chunk_x/z);
		 * the client offers "Quit anyway" / "Try again". */
		struct {
			bool unsaved_chunk;
			int32_t chunk_x, chunk_z;
			uint32_t count;
		} return_ingame;
		struct {
			uint8_t owner; /* 0 host body, 1 guest body; must match the route */
			bool on_fire;
		} local_player_fire;
		struct {
			uint8_t owner; /* 0 host body, 1 guest body; must match the route */
			/* Beta motionX/Y/Z, blocks per tick, installed on the body
			 * (G8: local and remote sessions alike). */
			vec3 vel;
			/* G8: false = setVelocity (knockBack / Packet28); true =
			 * addVelocity (server-side pushes on the player body). */
			bool add;
		} local_player_velocity;
		struct {
			uint32_t request_id;
			uint8_t op; /* enum clin_capacity_op */
			uint8_t slot;
			uint32_t mem1_bytes, mem2_bytes;
			uint32_t ticket; /* COMMIT / RELEASE */
		} capacity_request;
		struct {
			/* Stamped with the landing world (world_index/world_generation). */
			uint32_t token;
			int32_t x, z;
			uint8_t radius;
			bool cancel;
			/* The landing world's dimension (enum world_dim): the client
			 * PREPARES an unviewed context for a slot it does not hold yet
			 * (landing blocker 3), and a context needs its dimension. */
			int8_t dimension;
		} landing_arm;
		struct {
			uint32_t request_id;
			uint64_t actor_generation; /* host 0, guest route generation */
			uint8_t actor_slot; /* 0 host, 1 local guest */
			bool begin;
			bool restored_source; /* rollback END waits for restored source terrain */
			uint8_t restore_radius; /* exact source ring sent for rollback END, 1..4 */
		} transfer_hold;
		struct {
			bool retiring; /* true: the server instance retires */
		} world_release;
		struct {
			uint64_t ticket, generation;
			uint64_t respawn_epoch;
			/* RESPAWN_LOADING target identity; a dead guest may still view the
			 * source dimension while its Overworld destination streams. */
			uint8_t respawn_world_slot;
			uint32_t respawn_world_generation;
			int8_t respawn_dimension;
			uint8_t owner;
			int result; /* enum local_guest_result; no server header dependency */
			/* Only meaningful with LOCAL_GUEST_GAMEPLAY_READY: the shared
			 * entity-store key the client must install this guest's body
			 * under. Zero for every other result (id 0 is the primary
			 * player's, SERVER_LOCAL_PLAYER_ENTITY_ID). */
			uint32_t entity_id;
			/* Authoritative health on GAMEPLAY_READY, including saved zero.
			 * Install with the body before the death/gameplay pane can open. */
			short health;
			/* Also GAMEPLAY_READY only: where the server admitted the body.
			 * The client installs the entity and its camera here before it
			 * streams its first owned position, so the guest never starts
			 * from an uninitialized camera. Zero for every other result. */
			double x, y, z;
			float rx, ry;
			/* GAMEPLAY_READY or LOCAL_GUEST_CORRECTION: the movement token
			 * installed with the admitted body and echoed by SRPC_PLAYER_POS.
			 * Initial Join may use zero; A2-T COMMIT uses a fresh nonzero token. */
			uint32_t correction_seq;
			/* LOCAL_GUEST_CORRECTION only: PLAYER_CORRECTION_* applied by the
			 * owning session. Zero elsewhere. */
			uint8_t correction_flags;
			/* D2 (ASTRA-DEFECTS-RULING 7-10): GAMEPLAY_READY with
			 * respawn_epoch 0 only -- the AUTHORITATIVE world the server
			 * admitted the guest body into (server_local_world_of the guest
			 * actor). The client installs the body into, and binds view 1 to,
			 * exactly that world, never "player 0's current world".
			 * world_generation 0 = unstamped (a producer with no world
			 * identity: the legacy shared-world behaviour). */
			uint8_t world_slot;
			uint32_t world_generation;
			int8_t world_dimension;
			/* O07 / R04 D2-1: the guest-binding sequence this admission belongs
			 * to (same fence as world_reset.guest_bind_seq). 0 = unstamped. */
			uint32_t guest_bind_seq;
		} local_guest;
		struct {
			w_coord_t x, y, z;
			w_coord_t sx, sy, sz;
			// Points into the rpc_chunk_pool managed by client_interface.c.
			// Owned by the pool, not by this RPC; clin_process releases
			// the slot back to the pool after clin_chunk consumes it.
			struct chunk_rpc_slot* slot;
		} chunk;
		struct {
			w_coord_t x, z;
			// Pooled far_rpc_pool slot (~1 KB), released after clin_far_chunk
			// consumes it -- a pointer, NOT a by-value blob, so the 16-deep
			// by-value inbox is not inflated.
			struct far_rpc_slot* slot;
		} far_chunk;
		struct {
			w_coord_t x, z;
		} unload_chunk;
		struct {
			// Pooled map_rpc_pool slot (16 KB), released after clin_process
			// copies it into the client-side held-map mirror -- a pointer,
			// NOT a by-value blob (chunk/far slot precedent).
			struct map_rpc_slot* slot;
		} map_data;
		struct {
			int32_t map_id;
			uint8_t length;
			uint8_t bytes[255]; /* Packet131 marker payload, including type byte. */
		} map_markers;
		struct {
			uint8_t window;
			uint8_t slot;
			struct item_data item;
		} inventory_slot;
		struct {
			uint8_t slot;
		} player_hotbar_slot;
		struct {
			vec3 position;
			vec2 rotation;
			vec3 velocity;
			uint64_t world_time;
			// Monotonic position-correction ownership token echoed by SRPC_PLAYER_POS.
			uint32_t correction_seq;
			// PLAYER_CORRECTION_*; zero is the existing full teleport/snapshot path.
			uint8_t correction_flags;
			// Nonzero only for an authoritative body/world handoff. The client
			// installs the consolidated player snapshot, then echoes this as
			// SRPC_INPUT_READY before exposing any new-world input. Keeping hotbar,
			// motion, fire and air here makes the ownership boundary atomic even
			// when earlier snapshot RPCs were drained on a separate client frame.
			uint32_t input_epoch;
			int16_t oxygen;
			uint8_t selected_hotbar;
			bool on_fire;
			/* Same-EntityPlayer portal handoffs preserve these pre-call
			 * moveEntity inputs. The reliable client snapshot sent after the
			 * epoch ACK then converges held sneak to the current controller. */
			bool on_ground;
			bool sneaking;
			// A replacement EntityPlayer also drops death-only local ownership
			// (jump state and minecart rider claims).
			bool fresh_body;
		} player_pos;
		uint64_t time_set;
		struct {
			bool raining;
			bool thundering;
			float rain_strength;
			float thunder_strength;
		} weather_set;
		// Longest Beta bed string is 39 chars ("Your home bed was missing or obstructed").
		char hud_message[48];
		/* CRPC_LOAD_FAILED: why the load was REFUSED, for the failure card
		 * (G6 L1). Empty = the generic terrain-memory text. */
		char load_failed_reason[64];
		struct {
			// Beta EntityPlayer.sleepTimer: 0..100 while falling asleep, 100..110
			// as the wake fade-out runs, then 0.
			int32_t timer;
			bool sleeping;
		} sleep_state;
		struct {
			enum world_dim dimension;
			uint32_t local_entity;
			/* The installed server world's slot and generation; the client
			 * binds its new world context to them. 0 = unstamped producer
			 * (see struct client_rpc). */
			uint8_t world_index;
			uint32_t world_generation;
			/* Split-dims B2 gate: true = an IN-PLACE reset of the guest's
			 * world (slot world_index gets a new generation; player 0's
			 * world, body, windows and screens are untouched). false (every
			 * existing producer) = player 0's world switch, as always. Only
			 * honoured with world-bound payloads ON; dropped otherwise. */
			bool guest_world;
			/* With guest_world: 0 = re-generate the slot the guest already
			 * views; 1 = the guest's view to bind to the slot (the guest's FIRST
			 * move into a slot it does not view yet; the slot may be empty).
			 * With world_index = player 0's slot and its CURRENT generation,
			 * 1 = the guest moves back INTO player 0's world (bind only; its
			 * previous world is released). View 0 is player 0's and never
			 * valid. Ignored otherwise. */
			uint8_t guest_view;
			/* O07 / R04 D2-1: guest_world resets only -- the server's monotonic
			 * guest-binding sequence (server_local_guest_bind_seq). The client
			 * applies a guest reset/READY only if its sequence is not older than
			 * the newest binding it has applied, so a stale or replayed message
			 * can never undo a newer guest move. 0 = unstamped (legacy). */
			uint32_t guest_bind_seq;
		} world_reset;
		struct crpc_set_block {
			w_coord_t x, y, z;
			struct block_data block;
			struct column_revision local_revision;
			uint32_t local_previous_content;
		} set_block;
		/* CRPC_SET_BLOCK_PACK: n (2..CRPC_SET_BLOCK_PACK_MAX) consecutive
		 * packable payloads of ONE world_index/world_generation, untagged,
		 * in send order. Record i is a CRPC_SPAWN_ITEM when bit i of
		 * spawn_mask is set (G9 startup: a blast's drop rolls interleave
		 * SPAWN_ITEM between its SET_BLOCKs); an `other` record (its type
		 * and by-value payload bytes: CRPC_ENTITY_DESTROY, CRPC_ENTITY_EXPLODE,
		 * CRPC_SOUND -- BUG-TICK hw1030a: the perf_stress item reaper's
		 * ENTITY_DESTROY singles filled the 128 ring) when bit i of
		 * other_mask is set; else a CRPC_SET_BLOCK. Sized to fit the
		 * existing union (static-asserted in client_interface.c): the
		 * envelope does not grow. */
		struct {
			uint32_t n;
			uint32_t spawn_mask;
			uint32_t other_mask;
			union crpc_pack_record {
				struct crpc_set_block set_block;
				struct crpc_spawn_item spawn_item;
				struct crpc_pack_other {
					uint32_t type; /* enum client_rpc_type */
					uint8_t bytes[CRPC_PACK_OTHER_BYTES]; /* the payload member */
				} other;
			} rec[CRPC_SET_BLOCK_PACK_MAX];
		} set_block_pack;
		struct column_light_publication* settled_light;
		struct {
			w_coord_t x, y, z;
			struct piston_moving_wire_state state;
		} piston_moving;
		struct {
			uint8_t window;
			uint16_t action_id;
			bool accepted;
		} window_transaction;
		struct { uint8_t window; uint32_t ticket; bool accepted; } window_sort;
		/* Terminal reply to one SRPC_RECIPE_CRAFT. All three of ticket/player/
		 * window are echoed so a client can only retire the request it is
		 * actually waiting on; `reason` is OK or the single documented cause of
		 * a fully-rejected (zero-mutation) request. */
		struct {
			uint32_t ticket;
			uint8_t player;
			uint8_t window;
			enum recipe_craft_reason reason;
		} recipe_craft;
		struct {
			uint8_t window;
			uint16_t cook_time;				 // furnaceCookTime (0..200)
			uint16_t burn_time;				 // furnaceBurnTime
			uint16_t current_item_burn_time; // currentItemBurnTime (flame scale)
		} window_progress;
		struct {
			uint8_t window;
			enum window_type type;
			uint8_t slot_count;
		} window_open;
		struct {
			uint8_t window;
			enum window_type type;
			uint8_t slot_count;
		} window_reopen;
		struct crpc_spawn_item spawn_item;
		struct {
			struct mob_windup_cue windup_cue;
			uint32_t entity_id;
			int monster_id;
			uint8_t fleece_color;
			bool sheared; // sheep wool-overlay suppression (Beta dataWatcher 0x10);
			              // networked at spawn like fleece_color so an archived
			              // sheared sheep renders bare from its first frame
			bool saddled; // pig saddle (Beta dataWatcher byte 16 bit 0); networked
			              // at spawn like sheared so an archived saddled pig
			              // renders correctly from its first frame
			uint8_t wolf_flags;
			int32_t wolf_health; /* dataWatcher 18 is a Java Integer */
			bool wolf_shake_armed;
			bool wolf_shake_active;
			float wolf_shake_time;
			float wolf_prev_shake_time;
			float wolf_head_rotation;
			float wolf_head_rotation_old;
			vec3 pos;
			bool charged; // creeper "powered" DataWatcher byte 17 (weather slice);
			              // networked at spawn like sheared so an archived charged
			              // creeper renders tinted from its first frame.
		} spawn_monster;
		struct minecart_state_rpc spawn_minecart;
		struct minecart_state_rpc minecart_snapshot;
		struct boat_state_rpc spawn_boat;
		struct boat_state_rpc boat_state; // CRPC_BOAT_CORRECTION payload
		/* [740c583 Packet25EntityPainting] entity_id + xPosition/yPosition/
		 * zPosition (the WALL block, matching entity_painting.h's tile_x/y/z
		 * naming) + direction (0..3). art_index replaces Beta's `title`
		 * string -- entity_painting.c's g_painting_art table index, resolved
		 * server-side (server_local_spawn_painting's RNG pick, or
		 * painting_art_by_title on restore/remote-title decode) so no string
		 * ever needs to cross this same-process boundary. */
		struct {
			uint32_t entity_id;
			int32_t art_index;
			int32_t direction;
			int32_t tile_x, tile_y, tile_z;
		} spawn_painting;
		struct {
			uint32_t entity_id;
			uint32_t occupant_id;
			uint32_t ride_epoch;
			bool occupied;
			/* F3 correlation: echo the request's epoch/sequence pair so the
			 * client can retire its pending intent. Unsolicited authority
			 * changes use zero for both. */
			uint32_t input_epoch;
			uint32_t request_seq;
		} minecart_occupancy;
		struct {
			uint32_t entity_id;
			uint16_t item_id;
			bool fireball; /* Explicit visual kind; snowballs share the icon. */
			vec3 pos;
			vec3 vel;
			struct arrow_visual_state visual;
		} spawn_arrow;
		struct {
			uint32_t entity_id;
			vec3 pos;
			vec3 vel;
			int8_t fuse; // Beta valid live range 80..0
		} spawn_primed_tnt;
		struct {
			uint32_t entity_id;
			vec3 pos;
			vec3 vel;
			int8_t fuse;
		} primed_tnt_correction;
		struct {
			uint32_t entity_id;
			vec3 pos;
			vec3 vel;
			uint8_t block_id; // BLOCK_SAND or BLOCK_GRAVEL
		} spawn_falling_sand;
		struct {
			uint32_t entity_id;
			uint32_t collector_id;
		} pickup_item;
		struct {
			uint32_t entity_id;
			uint8_t count; // remaining stack count after a partial collect
		} item_count;
		struct {
			uint32_t entity_id;
		} entity_destroy;
		struct {
			/* Heap array TRANSFERRED to the client with the envelope. It is a
			 * COPY of the server's compact identity list, never a view into
			 * the live entity dictionary: the client may consume and free it
			 * on its own thread while the server thread is still iterating
			 * its own dictionary to retire the same ids. Released exactly
			 * once by whichever path consumes the envelope (apply or
			 * discard); a REFUSED clin_try_rpc_send transfers nothing and
			 * leaves the caller's pointer intact. */
			uint32_t* ids;
			size_t count;
		} entity_destroy_batch;
		struct {
			struct mob_windup_cue windup_cue;
			/* Versioned local presentation update; existing MOVE fields are inert. */
			bool windup_cue_only;
			uint32_t entity_id;
			vec3 pos;
			/* Beta newPosRotationIncrements to arm on the client entity: the
			 * remote provider sends 3 (every Beta move/teleport packet arms 3
			 * -- [740c583 NetClientHandler.handleEntity]/[.handleEntityTeleport]);
			 * 0 (the zero-initialized default, so server_local needs no edit)
			 * keeps the local provider's same-tick snap, which is Beta SP's
			 * direct per-tick movement. See entity.h network_increments. */
			uint8_t approach_ticks;
			bool on_fire;
			bool swelling; // creeper primed/swelling (fuse running)
			bool hurt;     // mob's persisted hurt animation is active (hurt_time > 0):
			               // drives the audit #17f red hurt-flash overlay. Networked
			               // like on_fire/swelling -- hurt_time is server-side only.
			bool sheared;  // sheep sheared flag (on_fire/swelling precedent): a LIVE
			               // shear updates already-spawned clients on the very next
			               // per-tick MOVE, dropping the wool overlay immediately.
			               // Applied only in the ENTITY_MONSTER branch (union safety).
			bool saddled;  // pig saddle flag, same live-update rule as sheared: a
			               // LIVE hit-to-saddle updates an already-spawned client on
			               // the very next per-tick MOVE. Monster branch only.
			uint8_t fleece_color; // sheep wool metadata, same live-update rule as
			                      // sheared (adversarial review F1: dye-on-sheep was
			                      // server-only -- the recolor never reached a live
			                      // client until a chunk reload re-sent SPAWN).
			int8_t fuse;   // creeper fuse phase (-1 idle .. CREEPER_FUSE_TICKS) for
			               // the per-instance swell flash; -1 for non-creepers
			int8_t death_time; // audit #18 item 3: Beta deathTime (0 alive .. >20 removed);
			                   // drives the client death-roll flop. 0 for a living mob.
			bool ghast_attacking; // ghast dataWatcher bit (attackCounter > 10):
			                      // selects ghast_fire.png (the sheared/on_fire
			                      // precedent; monster branch only)
			uint8_t wolf_flags;
			int32_t wolf_health; /* dataWatcher 18 is a Java Integer */
			bool wolf_shake_armed;
			bool wolf_shake_active;
			float wolf_shake_time;
			float wolf_prev_shake_time;
			float wolf_head_rotation;
			float wolf_head_rotation_old;
			bool charged; // creeper "powered" DataWatcher byte 17 (weather slice):
			              // same live-update rule as sheared -- a lightning strike
			              // updates an already-spawned client on the next MOVE.
			              // Applied only in the ENTITY_MONSTER branch (union safety).
			bool pig_ridden;     // saddle-slice review fix (finding 1): mirrors the
			uint32_t pig_rider_id; // minecart occupied/occupant_id pair so a client
			              // consumer (e.g. a future camera-follow slice) can tell
			              // which pig is ridden and by whom. Applied only in the
			              // ENTITY_MONSTER branch (union safety).
			bool pig_zombie_angry; // Pig Zombie anger_level > 0, for the client-
			              // side soft-snap attack assist only (docs/deviations.md
			              // "Soft-snap melee assist"). Monster branch only.
			struct arrow_visual_state arrow_visual;
		} entity_move;
		struct entity_physics_correction_rpc entity_physics_correction;
		struct {
			vec3 pos;
			float power; // blast power (creeper/TNT); render thread emits the puffs
		} entity_explode;
		struct {
			vec3 pos;
			float intensity;
		} smoke_puff;
		struct {
			vec3 pos;
			bool hearts;
		} wolf_fx;
		struct {
			vec3 pos;
			vec3 velocity;
			uint8_t count;
		} wolf_splash;
		struct {
			vec3 pos; /* the actor's posX/posY/posZ (player: eye) */
			vec3 vel; /* the actor's motionX/Y/Z */
		} drown_bubbles;
		/* CRPC_LIGHTNING_BOLT: the server bolt's render half right after
		 * its constructor -- strike block, boltLivingTime, the render seed
		 * (field_27029_a) and the raw 48-bit state of its Entity.rand, so
		 * the client's onUpdate replay (restrikes, new seeds) matches the
		 * server's bolt draw for draw. 64-bit values travel as 32-bit
		 * halves so the union's alignment (and the 792-B envelope on the
		 * Wii's EABI) cannot change. */
		struct {
			int32_t x, y, z;
			int32_t living_time;
			uint32_t render_seed_hi, render_seed_lo;
			uint32_t rng_state_hi, rng_state_lo;
			int32_t dimension; /* enum world_dim the bolt struck in */
		} lightning_bolt;
		struct {
			uint16_t sound_id; // enum audio_sound_id (platform/audio.h)
			vec3 pos;
			float vol;	 // Beta EVENT volume: >1 extends range, never gain
			float pitch; // Beta pitch, UNCLAMPED (server-rolled)
			// S2: true routes to audio_play_ui — Beta's NON-positional
			// playSoundFX quarter-volume path (portal.trigger/travel
			// [740c583: EntityPlayerSP.onLivingUpdate:39-62]); pos is
			// ignored. False (the zero default every existing designated
			// initializer produces) keeps the positional audio_play3d path.
			bool ui;
		} sound;
		struct {
			int16_t health;
		} player_set_health;
		struct {
			w_coord_t x, y, z;
		} world_spawn;
		struct {
			vec3 pos;		   // the block's RAW coordinates, no +0.5 centering
							   // (matches the dispenser 1000/1001/1002 events'
							   // "no centering except case 1003" precedent)
			uint16_t record_id; // 0 = stop; else the record's item id
		} record_event;
		struct {
			uint8_t window;
			uint16_t count; // <= CRPC_WINDOW_ITEMS_MAX_SLOTS
			struct item_data items[CRPC_WINDOW_ITEMS_MAX_SLOTS];
		} window_items;
		struct {
			uint8_t window;
		} window_close_remote;
		// S3: CRPC_SPAWN_PLAYER. held_item_id 0 = empty, matching Packet20's
		// own currentItem==0 sentinel (count 1 / damage 0 implied when
		// nonzero). [740c583 Packet20NamedEntitySpawn]
		struct {
			uint32_t entity_id;
			char username[17]; // Packet1's 16-code-unit login max + NUL
			vec3 pos;
			float yaw, pitch; // degrees
			uint16_t held_item_id;
		} spawn_player;
		// S3: CRPC_ENTITY_EQUIPMENT. slot mirrors Packet5's raw field (0 =
		// held item, 1..4 = armor, boots-to-helmet); item_id 0 = empty.
		// [740c583 Packet5PlayerInventory]
		struct {
			uint32_t entity_id;
			uint8_t slot;
			uint16_t item_id;
			int16_t item_damage;
		} entity_equipment;
		// S3: CRPC_ENTITY_ANIMATION. animate mirrors Packet18's raw code (1
		// swing, 2 hurt, 3 wake, 4 crouch/eat). [740c583 Packet18Animation]
		struct {
			uint32_t entity_id;
			uint8_t animate;
			/* MP-BED: Packet17 use-bed for ANOTHER player rides this payload
			 * (use_bed true, animate 0): NetClientHandler.func_22186_a ->
			 * EntityPlayer.sleepInBedAt(bed_x, bed_y, bed_z)
			 * (NetClientHandler.java:370-378). Remote-only producer. */
			bool use_bed;
			int32_t bed_x, bed_y, bed_z;
		} entity_animation;
		// MP visual-parity pass item 1, widened by the animation-parity pass
		// item 3/4: CRPC_ENTITY_FLAGS. All three decode the SAME DataWatcher
		// flag byte 0 -- bit 0 on_fire, bit 1 sneaking, bit 2 is_riding -- see
		// entity.h's entity_remote_player.{sneaking,is_riding} field comments
		// and entity.h's top-level on_fire field comment for the citations.
		struct {
			uint32_t entity_id;
			bool sneaking;
			bool on_fire;
			bool is_riding;
		} entity_flags;
		// S4: CRPC_CHAT_LINE. Packet3's own read-side cap is 119 code units;
		// +1 NUL. [740c583 Packet3Chat]
		struct {
			char text[120];
		} chat_line;
		// Remote-player render follow-up: CRPC_ENTITY_LOOK. yaw/pitch are
		// already degrees (mp_byte_angle_to_degrees's output, the same
		// conversion CRPC_SPAWN_PLAYER's yaw/pitch fields carry -- converted
		// exactly once, at server_remote.c's translation sites).
		struct {
			uint32_t entity_id;
			float yaw, pitch; // degrees
			/* Same contract as entity_move.approach_ticks: 3 = arm the Beta
			 * increments approach toward this look (a standalone Packet32 is
			 * Beta's handleEntity with a zero move delta -- the rotation
			 * still steps through the same 3 increments); 0 = legacy direct
			 * write. [740c583 NetClientHandler.handleEntity] */
			uint8_t approach_ticks;
		} entity_look;
		// CRPC_ENTITY_VELOCITY payload. vel is already converted to blocks/tick
		// (wire motion/8000.0, [740c583 NetClientHandler.handleEntityVelocity]);
		// consumed only for entity kinds whose client tick integrates e->vel
		// (see the CRPC_ENTITY_VELOCITY enum comment).
		struct {
			uint32_t entity_id;
			vec3 vel;
		} entity_velocity;
		// CRPC_SIGN_TEXT payload. grid is sign_text_store.h's SIGN_TEXT_LINES(4)
		// x SIGN_TEXT_COLS(16) packed grid, size duplicated as a raw 64 here
		// (see the CRPC_SIGN_TEXT enum comment for why).
		struct {
			w_coord_t x, y, z;
			char grid[64];
		} sign_text;
		// CRPC_SIGN_TEXT_REMOVED payload: the broken sign's former position.
		struct {
			w_coord_t x, y, z;
		} sign_text_removed;
		/* MP-RIDE: remote_rider false = the LOCAL player (the original
		 * form); true = the tracked remote player rider_id mounts vehicle_id
		 * (attach) or dismounts (attach false; vehicle_id then names the
		 * vehicle it left, or 0 when unknown). [NetClientHandler.java:439-449
		 * func_6497_a -> Entity.mountEntity for ANY entity pair] */
		struct { uint32_t vehicle_id; bool attach; bool remote_rider;
			uint32_t rider_id;
			/* respawn release: drop the link, leave the new body alone */
			bool no_reposition; } attach_entity;
		struct {
			bool enabled;
			bool ok;
			uint32_t generation;
			uint32_t request_seq;
			char reason[CRPC_SPLIT_REASON_CAP];
		} split_state;
	} payload;
};

/* The world-identity generation NAMESPACE (G6 S1, b1stamp-remote-0926).
 * Two independent minters stamp into the same client registry slot 0: the
 * local server (server_world.c, one generation per server_world created) and
 * the remote adapter (clin_remote_world_generation_mint, one per remote world
 * install). Both used to start at 1, so after local play -> MP login (or the
 * reverse) a dead session's queued, stamped traffic matched the new session's
 * identity and APPLIED. The ranges are disjoint by construction instead:
 * local generations live in [1, WORLD_GENERATION_REMOTE_BASE), remote ones in
 * [WORLD_GENERATION_REMOTE_BASE, UINT32_MAX]; each minter refuses to leave
 * its half (the local one aborts like its existing wrap refusal, the remote
 * one wraps inside its half), so no local identity can ever equal a remote
 * one. Disjoint halves rather than one shared counter: the two minters run on
 * different threads and in different layers (server thread vs client
 * adapter), and a partition needs no shared state between them. 0 stays
 * "unstamped". */
#define WORLD_GENERATION_REMOTE_BASE UINT32_C(0x80000000)
static inline bool world_generation_is_local(uint32_t generation) {
	return generation != 0 && generation < WORLD_GENERATION_REMOTE_BASE;
}
static inline bool world_generation_is_remote(uint32_t generation) {
	return generation >= WORLD_GENERATION_REMOTE_BASE;
}

/* Stamp a world-scoped RPC with its producing server world's identity
 * (w->world_index, w->generation). The ONE helper every producer uses, so the
 * A/C seams adopt the stamp without knowing the field layout. */
static inline void clin_rpc_stamp(struct client_rpc* call, unsigned world_index,
								  uint32_t world_generation) {
	call->world_index = (uint8_t)world_index;
	call->world_generation = world_generation;
}

bool clin_init(void);
/* R22 local-only, allocation-free scalar RPC quanta and safe mesh retirement.
 * Complex FIFO heads/recovery are retained and reject this service turn. */
enum g9_handoff_result clin_g9_handoff_service(unsigned operation, uint32_t budget_us);
void clin_g9_handoff_poll(bool mesh_safe);
void clin_update(void);
bool clin_transfer_actor_held(unsigned actor_slot);
/* Latest accepted transfer BEGIN, retained across an END drained in the same
 * client frame. Guest identity is scoped to its current bound generation. */
uint32_t clin_transfer_actor_request_id(unsigned actor_slot);
/* Same accepted host-transfer episode used by the split pane and the guest's
 * owner-routed pose sender; updated after each client RPC drain. */
bool clin_host_transfer_pane_active(void);
/* Split-dims B1 phase 2: the client world contexts (owner/main thread only).
 * _quiesce_pending: a current context is parked after a scoped reset until
 * its mesher/GPU leases drain -- the frame loop must keep draining them
 * (chunk_mesher_receive, world_render_completed) even on a paused frame.
 * _message_held: the drain loop is holding one world-scoped message for it. */
struct client_ctx_registry;
struct client_ctx_registry* clin_world_contexts(void);
bool clin_world_quiesce_pending(void);
bool clin_world_message_held(void);
/* One remote-primary movement sample after each executed client physics tick.
 * The caller supplies that tick's resulting camera state. */
void clin_remote_player_tick(const struct camera* sample);

/* V6 F1: the LOCAL position producer for one owner (0 = host, 1 = the
 * admitted split guest), called by clin_update at the 50 ms cadence. It asks
 * svin_motion_room BEFORE authoring, so an authored sample is never refused:
 * BACKPRESSURE = no sample authored, no cadence advance (retried next frame);
 * AUTHORED = the sample is owned by the transport until delivered; REFUSED =
 * a gate (transfer hold, closed mailbox) declined it before any queueing. */
enum clin_local_pos_result {
	CLIN_LOCAL_POS_AUTHORED,
	CLIN_LOCAL_POS_BACKPRESSURE,
	CLIN_LOCAL_POS_REFUSED,
};
enum clin_local_pos_result clin_local_position_produce(unsigned owner);
uint32_t clin_local_position_backpressure_count(unsigned owner);
/* V6 F1 round 3: the LOCAL per-executed-tick producer (main.c tick loop).
 * begin: reserve this owner's movement credit BEFORE its physics tick; false
 * = the owner is suspended this tick (do NOT run its physics; nothing is
 * owed later). end: the tick ran -- publish exactly one immutable sample of
 * the body's final pose (`sample`). cancel: begin succeeded but the tick did
 * not run. Remote sessions: begin is always true and end is not used (the
 * remote producer keeps clin_remote_player_tick). */
/* main.c's production owner dispatch (also exercised by the focused guard). */
struct input_player_frame;
void main_client_host_tick_dispatch(const unsigned* tick_slots, unsigned tick_n,
	bool host_entities_active, bool guest_only_active, int guest_tick_slot);
void main_client_guest_tick_dispatch(const struct input_player_frame* frame);
bool clin_local_tick_begin(unsigned owner);
/* Entity-local-player signals the executed owner step; main tests it after
 * dispatch. Terrain/sleep holds leave the reservation unused (cancelled). */
void clin_local_tick_physics_executed(unsigned owner);
bool clin_local_tick_physics_ran(unsigned owner);
void clin_local_tick_end(unsigned owner, const struct camera* sample);
void clin_local_tick_cancel(unsigned owner);
struct clin_local_tick_stats {
	uint32_t attempted, executed, suspended, cancelled;
};
void clin_local_tick_stats(unsigned owner, struct clin_local_tick_stats* out);
/* Count an executed, unpaused remote-primary physics tick at the tick loop. */
void clin_remote_movement_tick_executed(void);
void clin_remote_movement_reset(void);
void clin_remote_movement_stats(uint32_t* ticks, uint32_t* samples);
/* Main-thread only: offer the prepared local loading view to the exact pending
 * input epoch. Does not clear transport backpressure or remote/guest ownership. */
void clin_update_input_readiness(void);
/* Inbox-drain only -- no per-frame state resets (dig-burst budget). For
 * server_remote's same-thread retry path; see the definition's comment. */
void clin_update_drain(void);
bool clin_update_drain_budget(uint32_t budget_us);
void clin_recover_column_storage(void);
void clin_rpc_send(struct client_rpc* call);
/* ASTRA-G9-R5 point 10: the server's token-pool wait inside clin_rpc_send
 * over the current heartbeat window -- summed us, longest single wait and
 * count; take-and-clear (heartbeat reader). */
void clin_rpc_wait_take(uint32_t* sum_us, uint32_t* max_us, uint32_t* n);
/* ASTRA-G9-R5 order 2: the server-owned pending output ring (see
 * client_interface.c). flush: deliver pending envelopes, in order, into the
 * tokens the client has freed; true when nothing is left pending. Server
 * thread only until joined; shutdown then owns the final drain. discard:
 * release pending payloads, retaining terminal failure entries while
 * publication is stopped. stats: live count, peak, total
 * deferred sends, exhaustion waits. */
bool clin_rpc_flush_pending(void);
void clin_rpc_pending_discard(void);
/* Server-thread exit: leave unconsumed ownership for the post-join drain. */
void clin_rpc_pending_exit(void);
/* Shutdown raises this before joining: interrupts an exhaustion wait. */
void clin_rpc_cancel(bool cancel);
/* Exhaustion deadline: failed session, not a successful publication. */
bool clin_rpc_publication_faulted(void);
uint32_t clin_rpc_cancelled_sends(void);
void clin_rpc_pending_stats(uint32_t* pending, uint32_t* peak,
							uint32_t* deferred, uint32_t* exhausted);
/* Pending-output ring capacity in envelopes (ASTRA-G9-R5 order 2 accepted
 * 128; G9 R21 withdrew the unjustified 512). release-1031: 256, because
 * real-Wii perf_stress_5m hw1030r hb114 had a 135 ms server tick of which
 * 132 ms was the exhaustion wait in clin_rpc_send (16 tokens out, the 128
 * ring full) on a blast burst. A send waits only once 16 inbox tokens +
 * this many deferred envelopes are outstanding. A packed envelope
 * (CRPC_SET_BLOCK_PACK) is ONE entry holding up to CRPC_SET_BLOCK_PACK_MAX
 * records. Cost: sizeof(struct clin_rpc_pending_entry) = 800 B (792 B
 * envelope + 8 B generation) x 256 = 204,800 B, one lazy MEM2 allocation on
 * the Wii (was 102,400 B at 128: +100 KiB); never MEM1 .bss. Transport
 * only: token count, delivery order and packing are unchanged. */
#define CLIN_RPC_PENDING_CAPACITY 256u
/* G9 R21 packing counters (server thread; monotonic since clin_init): packed
 * envelopes created, and records appended into an already-pending entry. */
void clin_rpc_pack_stats(uint32_t* packs, uint32_t* packed_records);
/* Server thread: close the open pack (if any). The next SET_BLOCK starts a
 * new entry. Called at every tick start: a pack never spans server ticks. */
void clin_rpc_pack_seal(void);
/* G9 R3 (ASTRA-G9-REVIEW-2 item 1): the local server's publication barrier
 * after a light stop. While set, every server->client RPC except the one
 * CRPC_LOAD_FAILED terminal is dropped at the funnel (pooled payloads are
 * released, never leaked), so no window, gameplay, terrain or entity message
 * can follow the stop from ANY producer (blocks, entities, tile entities).
 * Set by server_world on the first light fault of a session world; cleared
 * when a session starts or loads a world. dropped = count since boot. */
void clin_server_publication_stop(bool stop);
bool clin_server_publication_stopped(void);
uint32_t clin_server_publication_dropped(void);
/* Server thread: pointer-free guest replies, including generation-scoped
 * LOCAL_GUEST status/corrections. Gameplay feedback still requires a bound
 * body when the client drains it; terrain/pooled payloads stay excluded. */
bool clin_rpc_send_local(const struct client_rpc* call, unsigned recipient,
	uint64_t generation);
/* Nonblocking, generation-tagged guest status/correction and private snapshot
 * route (window items/open/cursor/hotbar/health only, no pooled pointers).
 * False leaves the producer to retry. */
bool clin_try_rpc_send_local(const struct client_rpc* call, unsigned recipient,
	uint64_t generation);
/* CHIP B (task_18b9ae41, 2026-08-31): send SRPC_UNLOAD_WORLD with reliable
 * delivery -- a saturated svin_rpc_send failure latches a per-frame retry
 * (clin_update) instead of silently dropping the unload. Returns the first
 * attempt's own svin_rpc_send result; see the definition's comment. */
bool clin_send_unload_world(void);
/* S4 disconnect UX: run the client's own CRPC_WORLD_RESET teardown
 * synchronously on the CLIENT thread (main.c's non-graceful remote-end
 * branch). Exists because the dead session's world is client-side only --
 * a fresh server_local cannot unload it (release impl's has_pos
 * duplicate-unload assert; caught live 2026-08-26). Same handler a
 * dimension switch runs; call only from the client/main thread. */
void clin_force_world_reset(void);
/* S5.3 hunt PROBE (temporary, ROUND 6): scans the clin_inbox/clin_empty_msg
 * guard canaries declared in client_interface.c; no-op (PC and non-clobber
 * Wii). `label` identifies the calling checkpoint in any clobber report.
 * Main-thread-only, matching every other clin_* entry point here. */
void clin_canary_scan(const char* label);
/* S5.3 hunt fix: the payload-carrying sibling of clin_force_world_reset()
 * above, for the MP LOGIN handler's world INSTALL (not just teardown) --
 * see client_interface.c's definition comment for the full hardware-proven
 * root cause (remote_try_crpc/clin_try_rpc_send's clin_inbox tchannel was
 * never meant to be produced into from the client/main thread; this call
 * chain is the first caller that did). Call only from the client/main
 * thread, same contract as clin_force_world_reset(). */
void clin_force_world_reset_login(enum world_dim dimension,
								  uint32_t local_entity);
/* Split-dims B1 remote stamping: the remote session's server-world identity.
 * _mint issues a fresh one (process-lifetime monotonic, never 0) for a new
 * remote world install -- clin_force_world_reset_login mints for the login,
 * server_remote_protocol.c for a dimension-change respawn's WORLD_RESET;
 * _generation reads the current one, which remote-origin RPCs carry
 * (clin_rpc_stamp(call, 0, clin_remote_world_generation())). 0 before the
 * first remote install. Client/main thread only. */
uint32_t clin_remote_world_generation_mint(void);
uint32_t clin_remote_world_generation(void);
/* The remote adapter's dispatch (server_remote_*.c): stamp an unstamped RPC
 * with the current remote session identity, then clin_process_direct. Before
 * the first remote install (or after a disconnect retired the identity) the
 * identity is 0 and the RPC stays unstamped, exactly as before B1. A
 * WORLD_RESET carries its own identity in its payload; routing ignores the
 * envelope stamp of lifecycle messages. Inline so every remote call site
 * reaches clin_process_direct itself. */
void clin_process_direct(struct client_rpc* call);
static inline void clin_process_direct_remote(struct client_rpc* call) {
	if(call->world_generation == 0)
		clin_rpc_stamp(call, 0, clin_remote_world_generation());
	clin_process_direct(call);
}
/* S5.3 hunt fix, round 3 cont.: same main-thread-direct pattern as
 * clin_force_world_reset_login() above, for MP_EVENT_SPAWN_POSITION's
 * CRPC_WORLD_SPAWN -- see client_interface.c's definition comment. Call
 * only from the client/main thread. */
void clin_force_world_spawn(w_coord_t x, w_coord_t y, w_coord_t z);
/* S5.3 hunt fix, round 4: generic direct-dispatch sibling of the two above,
 * for MP remote code (server_remote_protocol.c/_chunks.c/_entities.c) to
 * opt into at any remote_try_crpc/clin_try_rpc_send call site individually
 * -- see client_interface.c's definition comment for why this is a
 * per-call-site opt-in rather than a change to clin_try_rpc_send itself.
 * Call only from the client/main thread. */
void clin_process_direct(struct client_rpc* call);

/* CRPC_ATTACH_ENTITY's pure half (unit-tested without a world): mark
 * `vehicle` (boat or minecart) as ridden by the local player, or release it
 * and, when `player` is given, place the player at Beta's dismount spot --
 * vehicle x/z, eye at the vehicle's box top (boundingBox.minY + height)
 * [740c583 Entity.mountEntity(null) -> setLocationAndAngles]. Returns false
 * for a vehicle that cannot be ridden (any other entity type). */
struct entity;
bool clin_apply_attach(struct entity* vehicle, bool attach, struct entity* player);

// Allocation fault-injection seam used by reset/load tests. Passing NULL
// restores malloc. The live game never changes this hook after startup.
void clin_set_alloc_hook(void* (*alloc_fn)(size_t));
/* Body-only position application after the caller's admission/epoch checks.
 * Does not expose gameplay, send an ACK, change the world clock or select UI. */
struct client_session;
bool clin_apply_player_position(struct client_session*, const struct client_rpc*);
/* Move an inert guest body to the exact ready respawn payload before its
 * epoch ACK. On failure the original body/store/view remain owned. */
bool clin_guest_respawn_handoff(unsigned slot, uint32_t generation,
	int dimension);
/* lava-aabb-0924/B3: install a CRPC_LOCAL_PLAYER_FIRE on the session's own
 * local body (the entity the OTHER split viewport draws). false when there is
 * no body; the caller owns the route/owner/generation checks. */
bool clin_apply_player_fire(struct client_session*, const struct client_rpc*);
/* damage-client-0926: install a CRPC_LOCAL_PLAYER_VELOCITY on the session's
 * own local body -- Entity.setVelocity: the vertical component only with the
 * local server, all three on a remote session (mp-velocity-0926; see the
 * enum comment). false when there is no body; the caller owns the
 * route/owner/generation checks. */
bool clin_apply_player_velocity(struct client_session*, const struct client_rpc*);
/* lava-aabb-0924/B3: whether the OTHER split viewport draws the fire
 * billboard on this session's local body (main.c's partner draw gate): the
 * body is burning AND entity_local_player_render_other would draw it at all
 * (body + inventory present, corpse not yet retired). */
bool clin_local_body_fire_visible(const struct client_session* owner);

/* ---- Live experimental-split-screen switch, client half (S-A1) -----------
 * S-A1 owns only the plumbing: the last CRPC_SPLIT_STATE the client consumed,
 * and the stable world generation a request has to be stamped with. The Options
 * row / input arming that reads them is S-A2. */
struct clin_split_ack {
	bool valid; /* false until one ack has been consumed this session */
	bool enabled;
	bool ok;
	uint32_t generation;
	uint32_t request_seq;
	char reason[CRPC_SPLIT_REASON_CAP];
};
/* Takes the ack the inbox drain stored, if there is one: copies it out and
 * CLEARS the slot, so every ack is delivered to the requester EXACTLY ONCE.
 * False (and *out untouched) when no new ack has arrived since the last call.
 * Main/client thread only, like every other clin_* accessor.
 *
 * The consume is load-bearing, not hygiene (R-A9 A9-H1/H2/M3): the requester's
 * reconcile adopts any current-generation ack from any row state, so a slot
 * that stayed valid made main.c re-apply one server statement on every frame
 * for the rest of the session -- cancelling the deferred config write,
 * stopping the pending-toggle timeout from ever aging, and pinning a stale
 * caption over every later row state. A caller that needs to look at the same
 * ack twice must keep its own copy. */
bool clin_split_last_ack(struct clin_split_ack* out);
/* The world generation an SRPC_SPLIT_SET must carry: the last input epoch this
 * client ACKed, i.e. the generation whose world it is actually looking at. Zero
 * before the first handoff -- the server rejects a zero stamp, so a caller must
 * not send a request until this is nonzero. */
uint32_t clin_input_epoch(void);

/* Main thread: request the ordinary epoch-checked voxel restream. Also used
 * by split_regrow_8m after its warmed anchor relocation; world simulation,
 * loading barriers, RESET and ACK remain owned by the normal handlers. */
void clin_begin_live_stream_recovery(void);

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
// Direct dispatch seam for deterministic reset/OOM integration tests.
void clin_process(struct client_rpc* call);
void clin_apply_position_only_correction(struct entity* player,
										struct camera* camera,
										const vec3 position, uint8_t flags);
void clin_apply_entity_physics_correction(struct entity* entity,
										 const struct client_rpc* call);
// Deterministically reject a new entity-slot admission, independent of whether
// M-lib's current dictionary capacity happens to require a realloc.
void clin_test_fail_entity_slot_after(unsigned occurrence);
// Capacity-specific seam for proving clin_entity_slot grows before a tombstone-
// threshold insertion can fall through to M-lib's fatal safe_get resize.
void clin_test_fail_entity_dict_realloc_after(unsigned occurrence);
// Independent test fixtures may model a process/server restart in one host
// process. Production never resets the monotonic accepted-epoch guard.
void clin_test_reset_input_epoch_guard(void);
/* Split-dims B1: arm the WAIT_RESET recovery phase (CRPC_STREAM_RESET accepted). */
void clin_test_arm_stream_reset(void);
/* Split-dims B1: the voxel-stream install-failed (NACK) latch. */
bool clin_test_stream_install_failed(void);
// Observe server->client sends before they enter the transport. Tests use this
// to pin transactional ordering; NULL disables the observer.
typedef void (*clin_test_rpc_observer)(const struct client_rpc* call, void* user);
void clin_test_set_rpc_observer(clin_test_rpc_observer observer, void* user);
/* Pop one envelope and tag; transfers pooled ownership to the caller. */
bool clin_test_inbox_pop(struct client_rpc* out, uint64_t* generation);
void clin_test_rpc_release(struct client_rpc* call); /* the popped copy */
/* the OLD blocking transport (parity baseline) / a ring allocation failure */
void clin_test_set_rpc_blocking(bool blocking);
void clin_test_fail_pending_alloc(bool fail);
/* Admission limit of the ring (1..CLIN_RPC_PENDING_CAPACITY; 0 restores the
 * shipping capacity): 128 replays the pre-R20 window for parity proofs. */
void clin_test_set_pending_capacity(unsigned cap);
/* ci-1101/61: the exhaustion (dead-consumer) deadline in ms; 0 restores the
 * shipping RPC_EXHAUSTION_TIMEOUT_MS. Lets a fail-closed proof run fast. */
void clin_test_set_rpc_exhaustion_timeout_ms(uint32_t ms);
/* G9 R21: packing on (default) / off -- the unpacked arm of equivalence
 * proofs. Only with the ring empty. */
void clin_test_set_block_pack(bool enabled);
/* G9 startup: CRPC_SPAWN_ITEM records join packs (default on); off = the
 * R21 SET_BLOCK-only pack. Ring must be empty. */
void clin_test_set_pack_spawn_items(bool enabled);
/* Main thread, immediately after each actual queued consumption (not discard). */
void clin_test_set_consume_observer(clin_test_rpc_observer observer, void* user);
/* CRPC_ENTITY_DESTROY_BATCH payload releases since the last reset. Pins the
 * exactly-once ownership rule across apply, tagged-reject and discard. */
size_t clin_test_destroy_batch_releases(void);
void clin_test_destroy_batch_releases_reset(void);
#endif
// Non-blocking try-send (returns false if the client token pool is exhausted). The far-ring
// evict/send paths use this to DEFER instead of BLOCK -- the two-way bounded-buffer deadlock
// fix (mirror of svin_try_send_now); see the .c.
bool clin_try_rpc_send(struct client_rpc* call);
/* Count an accepted enqueue before its inbox publication. The caller's
 * counter is untouched if no client RPC token is available. */
bool clin_try_rpc_send_counted(struct client_rpc* call,
	uint32_t* on_enqueue_counter);
// stage22-0927: true when at least `n` client RPC tokens are free right now.
// Nonblocking; takes up to n tokens from the pool and returns them before it
// returns, so it never consumes one. With the local server thread as the ONLY
// producer into the client inbox, a positive answer cannot be invalidated
// before that thread's own next sends (the client only ever returns tokens).
bool clin_rpc_headroom(unsigned n);
// S4: server_remote's MP_EVENT_NOTE_BLOCK seam. Mirrors Beta's
// World.playNoteAt gate (getBlockId(x,y,z) > 0). See the .c.
bool clin_world_block_present(int32_t x, int32_t y, int32_t z);
/* mp-effects (2026-10-29): server_remote's Packet54 seam. Beta's
 * NetClientHandler.handleNotePlay -> World.playNoteAt dispatches on the block
 * id at (x,y,z): Block.blocksList[id].playBlock(world,x,y,z,b1,b2). The note
 * block spawns its "note" particle here; a piston (29/33) runs its own
 * client-side extend (b1 0) / retract (b1 1) over the client world, exactly
 * like BlockPistonBase.playBlock does on WorldClient. Returns what happened
 * so the caller emits the matching sound. Main/client thread only. */
enum clin_block_event_result {
	CLIN_BLOCK_EVENT_NONE = 0, /* no chunk, air, or a block with no playBlock */
	CLIN_BLOCK_EVENT_NOTE,
	CLIN_BLOCK_EVENT_PISTON_OUT, /* tryExtend materialized */
	CLIN_BLOCK_EVENT_PISTON_IN,
};
enum clin_block_event_result clin_remote_block_event(
	int32_t x, int32_t y, int32_t z, uint8_t b1, uint8_t b2);
/* mp-effects: the host body's selected hotbar slot (0..8), or -1 when there
 * is no inventory window yet. server_remote re-sends Packet16 from it after
 * login/respawn. Main/client thread only. */
int clin_selected_hotbar_slot(void);
// Headless-test only: recycle inbox tokens and any pooled chunk payloads without
// processing them into a client world. See the .c.
void clin_drain_discard(void);

// Flush pending publication, then acquire a free chunk slot without waiting.
// NULL means busy: streaming retries the unsent chunk next update. The server
// fills ids / metadata / lighting_* and sends a CRPC_CHUNK owning the slot.
struct chunk_rpc_slot* clin_chunk_slot_acquire(void);
struct server_chunk;
/* Acquire the local lease and snapshot its five planes without allocation.
 * Called by the server thread after slot acquire, before sidecars/RPC send. */
void clin_chunk_slot_copy_local(struct chunk_rpc_slot* slot,
	const struct server_chunk* sc);

// Immediate pool acquisition without flushing publication; NULL means busy.
// Extra batch chunks use this after the first acquisition already flushed.
struct chunk_rpc_slot* clin_chunk_slot_try_acquire(void);

// Return a slot to the pool. Called by the client side after
// clin_chunk has consumed the buffers.
void clin_chunk_slot_release(struct chunk_rpc_slot* slot);

/* --- held-map colour transport (held-map render slice, 2026-08-26) -------- */
// One map's Beta colour bytes in flight from the server map cache to the
// client mirror. 16384 == MAP_DATA_COLORS_SIZE (map_data.h); pinned by a
// _Static_assert in client_interface.c without dragging map_data.h in here.
#define MAP_RPC_COLORS_SIZE 16384
#define CLIN_MAP_PLANES 8
struct map_rpc_slot {
	int32_t map_id; // the item damage value naming the map (Beta map_%d)
	uint8_t colors[MAP_RPC_COLORS_SIZE];
};
// Non-blocking acquire (server thread): NULL when both slots are in flight --
// the sender skips this tick and retries on the next change-scan, exactly the
// far-batch defer pattern; a held map tolerates a tick of latency.
struct map_rpc_slot* clin_map_slot_try_acquire(void);
void clin_map_slot_release(struct map_rpc_slot* slot);
// Client-side mirror read (main/render thread only): returns the colour bytes
// for map_id (shared static blank until DATA, including on OOM), and writes
// the mirror's monotonically-increasing version so the renderer can gate its
// texture re-upload on actual change (documented deviation: Beta re-uploads
// all 16384 texels every frame; we upload on change only).
const uint8_t* clin_held_map_colors(int32_t map_id, uint32_t* out_version);
// Main-thread recovery: NULL means unknown after failure; fresh IDs return blank.
const uint8_t* clin_map_recover(int32_t map_id, uint32_t* out_version);
bool clin_map_deliver(int32_t map_id, const uint8_t* colors);
struct item_map_marker;
size_t clin_held_map_markers(int32_t map_id, struct item_map_marker* out);
bool clin_map_markers_deliver(int32_t map_id, const uint8_t* bytes, uint8_t length);
void clin_map_forget(int16_t map_id);
uint32_t clin_map_allocation_failures(void);
uint32_t clin_map_io_failures(void);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
size_t clin_map_test_plane_count(void);
void clin_map_test_fail_reload(bool fail);
#endif

// Far-ring slot pool (M1 deep reach), mirroring the chunk slot pool: the server
// acquires a ~1 KB far slot, fills it, sends a CRPC_FAR_CHUNK pointing at it, and
// the client releases it after clin_far_chunk consumes it. NON-BLOCKING ONLY --
// unlike the chunk pool there is no blocking acquire, because the far ring is
// best-effort and a parked server thread here is one half of the bounded-buffer
// deadlock clin_try_rpc_send fixed for the other half. NULL == defer to next tick.
struct far_rpc_slot* clin_far_slot_try_acquire(void);
void clin_far_slot_release(struct far_rpc_slot* slot);
// Cumulative count of far acquires that found the pool exhausted (and therefore
// deferred). Backpressure instrument AND leak alarm: the non-blocking bail above
// turned an exhausted pool from a visible server-thread park into a SILENT defer, so a
// far slot the client consumed and never released would otherwise kill far streaming
// with no symptom at all. server_local.c's far batch is the production reader -- it
// counts CONSECUTIVE bails and reports once the run passes FAR_SLOT_LEAK_BAILS, which
// is what separates ordinary backpressure (a few bails, then a slot comes back) from a
// pool that will never recover. Also the far-deadlock test's observable.
uint32_t clin_far_slot_busy_count(void);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
// Test-only conservation oracle; avoids mistaking busy_count (failed acquires)
// for the number of live checked-out pool owners.
size_t clin_far_slot_test_capacity(void);
#endif

// CHIP A (task_11695e19, 2026-08-31): monotonic count of columns actually
// installed by clin_chunk's full-column path or clin_far_chunk, bumped
// regardless of whether world_loaded_chunks()'s NET section count moved.
// screen_load_world.c's lworld_stall_tracker feeds this alongside
// world_loaded_chunks() so a same-dim teleport's unload/promotion churn (or
// a column mid-zlib-decode across several frames) can't hide genuine
// streaming progress behind a flat net-cardinality reading. See client_
// interface.c's s_chunk_install_seq doc comment for the full rationale.
uint32_t clin_chunk_install_seq(void);

/* Main-thread-only last frame: full chunk, far chunk, other RPCs, queue overhead. */
void clin_work_profile_enable(bool enabled);
void clin_work_stats(float out[4]);

/* Passive census: these buffers are BSS, not carve allocations. */
size_t clin_census_static(void);
/* Restore presentation from an installed primary window; no close or reopen RPC. */
bool clin_show_window(uint8_t window);

#endif
