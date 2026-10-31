#ifndef CLIENT_WORLD_PAYLOAD_H
#define CLIENT_WORLD_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../platform/time.h"
#include "../world.h"

/* Split-dims B2 (PREPARATION, default OFF): world-bound client payloads.
 *
 * B1 gave the client one world CONTEXT per server world slot but kept ONE
 * payload (gstate.world, gstate.entities, the particle rings) that only the
 * payload slot's context may mutate (the single-payload rule, Fable B1 F1).
 * With the world-payloads flag ON each slot's context owns its own payload
 * STORAGE instead:
 *   slot 0 -> gstate.world + gstate.entities + the primary particle system
 *             (the storage main.c and the entity code already read);
 *   slot 1 -> a second struct world + entity dictionary + particle system,
 *             allocated when the flag is enabled and freed when it is
 *             disabled.
 * Each slot's world-scoped traffic applies into its own storage, a
 * WORLD_RESET tears down and reinstalls only its own slot's payload (the
 * other slot's context and payload are untouched), and each storage's world
 * carries a lease owner bound to its own slot, so mesher/GPU leases drain to
 * the exact context they came from in either world.
 *
 * With the flag OFF (the default) every slot maps to the single gstate
 * storage and the client behaves byte-for-byte as B1 (single-payload rule,
 * FOREIGN drops, one lease owner following the payload slot).
 *
 * Each storage also owns its sign-text store, moving-piston store and clock
 * + weather (slot 0: the primary stores and the gstate clock fields main.c
 * reads; slot 1: its own), so they are reset with, and follow, their world.
 *
 * NOT ACTIVATION-READY -- gates that must close before set_enabled(true) is
 * called outside a test (Fable B2 review, 2026-09-26):
 *  - producers are unstamped: unstamped traffic with two live slots is
 *    AMBIGUOUS and dropped;
 *  - an ordinary WORLD_RESET is player 0's world switch; the IN-PLACE guest
 *    world reset (payload.world_reset.guest_world) is consumed here (B2 gate
 *    1, b2-gates-0926) but no server producer sends it yet -- the lane-A
 *    handoff (B2-GATES-handoff) adds the producer for A2's guest respawn /
 *    guest dimension change (Fable F2). guest_view = 1 makes it the guest's
 *    FIRST move into a slot it does not view yet (binds view 1; the slot may
 *    be empty). The guest's gameplay_ready is cleared by it: the server must
 *    re-issue LOCAL_GUEST_GAMEPLAY_READY, whose body lands in the store of the
 *    slot view 1 shows. The guest moving back INTO player 0's world is the
 *    same message naming player 0's slot, its CURRENT generation and
 *    guest_view = 1: bind only, the guest's previous world is released, and
 *    the same re-READY follows;
 *  - readiness is per payload (clin_payload_apply_ready for world-scoped
 *    handlers, clin_terrain_gate_open for terrain -- Fable F1, closed here),
 *    but player-0-scoped handlers (PLAYER_POS, windows, BEGIN_WORLD_SWITCH)
 *    still gate on the primary flag by design;
 *  - main.c still renders/ticks only gstate storage (the primary stores and
 *    clock) and sets only the primary particle camera (the owner calls
 *    clin_world_payload_sync_particle_cameras for slot 1); entity subclasses
 *    iterate gstate.entities directly;
 *  - slot-1 GPU / display-list lifetime is proven only on the headless
 *    render path (test_b2_gpu_lifetime, Fable F5); the real draw of slot 1
 *    arrives with the main.c second-view wiring;
 *  - a guest world's SOUND and particle/effect traffic applies into its own
 *    payload, but the audio has no per-world/per-view listener: it plays
 *    through player 0's audio as if in player 0's world (Fable R2-2);
 *  - activation must read the VIEWED slot's dimension and clock
 *    (clin_world_payload_of_view -> terrain->dimension,
 *    clin_world_payload_clock), never gstate.world.dimension / gstate clock
 *    fields, which are slot 0's (Fable R2-3);
 *  - a lost column in a non-primary world raises its marker and the client
 *    offers a world-addressed resend request to the registered sender, and
 *    consumes the world-stamped CRPC_STREAM_RESET confirmation (B2 gate 2);
 *    the transport SRPC_WORLD_RESEND, its server handler and the sender
 *    registration are the lane-C/A handoff (B2-GATES-handoff);
 *  - clin_world_payload_memory figures are the BUILD's own sizes: only a
 *    Wii-built run gives the Wii budget (Fable F7).
 * See clin_world_payloads_set_enabled.
 *
 * THREADING: owner (main/client) thread only, like every clin_* call. */

struct particle_system;
struct dict_entity_s;
struct sign_text_store;
struct piston_moving_store;

struct clin_world_payload {
	struct world* terrain;
	struct dict_entity_s* entities;
	struct particle_system* particles;
	struct sign_text_store* signs;
	struct piston_moving_store* pistons;
};

/* Enable/disable world-bound payloads. Refused (false, nothing changed)
 * while any client world context is in use (current or retiring with leases
 * out) -- the flag may only flip between sessions. Enabling allocates the
 * slot-1 storage and fails closed (false, flag stays OFF) when that
 * allocation fails; disabling tears it down and frees it. Setting the
 * current value is a successful no-op. */
bool clin_world_payloads_set_enabled(bool enabled);
bool clin_world_payloads_enabled(void);

/* The storage a slot's traffic applies into (flag OFF: the single gstate
 * payload for every slot). False for an out-of-range slot. */
bool clin_world_payload_of_slot(unsigned slot, struct clin_world_payload* out);
/* The payload rendered by `view`: the storage of the slot whose CURRENT
 * context has that view bound. False when no current context holds it. */
bool clin_world_payload_of_view(unsigned view, struct clin_world_payload* out);
/* Bind `view` to the slot's CURRENT context, unbinding it from every other
 * slot's current context first (a view renders exactly one payload). A
 * WORLD_RESET does this for view 0 (player 0) when the flag is ON; a guest
 * view is bound by its owner. False: no current context, or view > 1. */
bool clin_world_payload_bind_view(unsigned slot, unsigned view);

/* split_dims_6m evidence (read-only, main thread): the client world slot a
 * view is bound to (-1 = none bound), and the count of CRPC_WORLD_RELEASE
 * messages this client APPLIED (payload freed; ignored ones do not count),
 * with the last applied slot (-1 = none). */
int clin_world_view_slot(unsigned view);
uint32_t clin_world_release_applied(int* last_slot);
/* The slot whose context owns player 0's world (the last WORLD_RESET). */
unsigned clin_world_payload_primary_slot(void);
/* Has this slot's payload completed its own WORLD_RESET install (and not
 * been released since)? */
bool clin_world_payload_ready(unsigned slot);
/* Terrain installs into a NON-primary payload that failed. Such a payload
 * has no epoch-marker ACK/NACK path of its own until the producers stamp a
 * per-world marker (A2), so a failure is counted here, never folded into the
 * primary stream's NACK latch. */
uint32_t clin_world_payload_install_failures(unsigned slot);
/* Per-world "request resend" marker: raised by a failed terrain install into
 * a NON-primary payload (a column the server already marked sent), cleared
 * only by that slot's next WORLD_RESET or release. The client half of the
 * per-world resend; see the NOT ACTIVATION-READY note for the A/C half. */
bool clin_world_payload_resend_pending(unsigned slot);

/* World-addressed terrain resend (B2 gate 2). The client raises a slot's
 * resend marker when a column the server marked sent could not be installed;
 * while it is raised and not yet accepted by the transport, every clin_update
 * offers ONE request per slot to the registered sender, naming the world:
 * sender(world_index, world_generation) -> true once the request is queued
 * (the transport's SRPC_WORLD_RESEND, lane C/A handoff), false to retry next
 * frame. The server answers with a CRPC_STREAM_RESET stamped for that world:
 * the client re-streams that payload in place (same identity, fresh epoch)
 * and clears the marker -- the per-world confirmation. A stamped reset with
 * no request outstanding for that generation is ignored. If the successor
 * context cannot be installed the marker stays raised, the install is
 * retried each frame and a fresh request follows it. NULL sender (the
 * default) = no requests; the marker stays raised. A request the transport
 * ACCEPTED but the server ignores (world not live, other generation, no
 * client route) is never re-sent: the hole stays for that generation, by
 * design, until the slot's next reset or release clears the marker. Owner
 * thread only. */
typedef bool (*clin_world_resend_sender)(unsigned world_index,
										 uint32_t world_generation);
void clin_world_payload_set_resend_sender(clin_world_resend_sender sender);
/* The slot's request state: true once the sender accepted it (cleared with
 * the marker by that world's stamped STREAM_RESET, reset or release). */
bool clin_world_payload_resend_requested(unsigned slot);

/* Flag ON: set each slot-1 particle system's spawn-cull camera from the
 * camera of the lowest view bound to that slot's current context (main.c
 * only sets the primary's). clin_update calls it once per drain; a no-op
 * with the flag OFF. */
void clin_world_payload_sync_particle_cameras(void);

/* Snapshot of one world's clock + weather (flag OFF: gstate's for every
 * slot). */
struct clin_world_payload_clock {
	uint64_t world_time;
	ptime_t world_time_start;
	bool raining, thundering;
	float rain_strength, thunder_strength;
};
bool clin_world_payload_clock(unsigned slot,
							  struct clin_world_payload_clock* out);

/* VIEW SCOPE (B2-MAINC-PLAN section 0): "this view's world" for one tick or
 * draw pass. _enter fills the viewed slot's terrain / entities / dimension /
 * clock (the slot whose current context has `view` bound, else player 0's
 * slot). Flag OFF: the gstate objects, and NOTHING is re-selected. Flag ON:
 * also selects that payload's particle system, sign store and piston store
 * and sets world_view_current()/world_view_entities() to it; _leave restores
 * every previous selection. Scopes nest (restore order is LIFO). */
struct clin_view_world {
	struct world* terrain;
	struct dict_entity_s* entities;
	unsigned slot;
	int dimension;
	struct clin_world_payload_clock clock;
	bool selected;
	struct particle_system* prev_particles;
	struct sign_text_store* prev_signs;
	struct piston_moving_store* prev_pistons;
	struct world* prev_terrain;
	struct dict_entity_s* prev_entities;
};
bool clin_world_view_enter(unsigned view, struct clin_view_world* out);
/* The same scope for a world SLOT (a live payload no view may show, e.g. for
 * its 20 Hz tick). Flag OFF: the gstate objects, nothing selected. */
bool clin_world_slot_enter(unsigned slot, struct clin_view_world* out);
/* The payload slots to tick/build this frame, player 0's first: flag OFF
 * exactly one (the primary); ON every slot with a current context (at least
 * the primary). Returns the count (1 or 2). */
unsigned clin_world_live_slots(unsigned out[2]);
void clin_world_view_leave(struct clin_view_world* scope);

/* Flag ON only: retire the slot's current context with NO successor and
 * release its payload contents (terrain, entities, particles). The storage
 * itself stays allocated until the flag is disabled; the retired context is
 * reclaimed once its leases drain. False when the flag is OFF, the slot is
 * out of range, is the primary slot (player 0's world: WORLD_RESET owns its
 * teardown) or has no current context. An UNSTAMPED WORLD_RESET (a producer
 * that knows no slots: a single-world session) releases every other slot
 * this way when the flag is ON. A release clears the slot's own sign-text and
 * moving-piston stores; the other payload's are untouched. */
bool clin_world_payload_release(unsigned slot);

/* A2-T landing (gap-doc blocker 3): the server streams the arrival core of the
 * DESTINATION world before COMMIT, stamped with that world. Those columns need
 * a client context to land in, and no view may show that world yet (the
 * transferring player is still in its source world until COMMIT). prepare
 * makes sure slot `slot` holds a current context at exactly `generation`:
 *  - already there (player 0's world, the partner's world, or prepared
 *    earlier): nothing changes, true;
 *  - otherwise an UNVIEWED context is installed for it (a stale unviewed
 *    predecessor in that slot is released first; a failed install is retried
 *    each frame like gate 1). True.
 * False (nothing changed): flag OFF, bad slot, generation 0, player 0's slot
 * at another generation, an older generation than the slot's live world, or a
 * VIEWED world of another generation in that slot.
 * The prepared context is bound, never rebuilt, by the COMMIT that follows:
 * player 0's stamped WORLD_RESET of that slot+generation joins it (like the
 * host join), and the guest's in-place reset with guest_view 1 binds view 1.
 * unprepare (landing cancelled) releases a context THIS call prepared that is
 * still unviewed and holds no local body; anything else is left alone. */
bool clin_world_payload_prepare_landing(unsigned slot, uint32_t generation,
										int dimension);
void clin_world_payload_unprepare_landing(unsigned slot, uint32_t generation);
/* Diagnostics / tests: the generation prepared (and not yet bound) in the
 * slot, 0 = none. */
uint32_t clin_world_payload_prepared(unsigned slot);

/* MEASURED per-payload cost (Wii budget accounting).
 *  fixed_mem1:   storage that exists only because of this payload
 *                (slot 1: its struct world + dictionary head + particle
 *                system struct; slot 0: 0 -- gstate is always resident).
 *  terrain:      world_memory_usage of its terrain.
 *  entity_mem1:  entity dictionary buffers (capacity) + one struct entity
 *                per tracked entity.
 *  particle_mem2: particle ring capacity.
 *  mem1_bytes = fixed_mem1 + terrain.mem1_bytes + entity_mem1;
 *  mem2_bytes = terrain.mem2_bytes + particle_mem2. */
struct clin_world_payload_memory {
	size_t fixed_mem1;
	struct world_memory terrain;
	size_t entities, entity_mem1;
	size_t particle_mem2;
	size_t mem1_bytes, mem2_bytes;
};
bool clin_world_payload_memory(unsigned slot,
							   struct clin_world_payload_memory* out);

#endif
