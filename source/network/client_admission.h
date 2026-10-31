/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * Split-dims B2 -> A2-T admission, CLIENT half (lane B, b2-a2t-cap-0927).
 * Astra astra-a2t-0927: before a guest is admitted into a SECOND live world
 * the server reserves aggregate capacity, including client/GPU capacity
 * ("insufficient budget means visible refusal with records and host state
 * unchanged"), and it publishes membership only after that world is LIVE
 * and its "landing/client capacity is ready". This header is the client's
 * side of both. FINAL CONTRACT (lane A reply, 2026-09-27): lane A owns the
 * server half; the wire messages are the ones listed at the end, and the
 * client answers them by default through svin (see the senders below).
 *
 * Everything here is world-bound-payload territory: with
 * clin_world_payloads_set_enabled(false) every reserve answers
 * CLIN_CAP_DISABLED and landing reports never fire -- the single-world
 * client is untouched. Owner (main/client) thread only, like every clin_*.
 *
 * ---- (1) client/GPU capacity: reserve -> commit -> release --------------
 * A reservation earmarks, for one world slot, the client resources a second
 * live world will consume once its payload streams in:
 *   - one client world-context pool entry (client_ctx_registry: the
 *     install that gate 1 performs must not find the pool full);
 *   - MEM1 and MEM2 bytes for that world's terrain columns and their meshes
 *     (display lists live in the MEM2 carve on the Wii), as the SERVER sizes
 *     them from the landing radius (it knows the columns it will stream).
 * The client checks the need against what is free NOW minus every other
 * live reservation minus a fixed safety margin; a refusal names the short
 * resource and changes nothing. A reservation is a promise, not an
 * allocation: nothing is allocated here, and the payload's own allocations
 * later draw from the same heaps. Commit marks the admission as going ahead
 * (the reservation then lives until release); release returns it (refused
 * admission, rollback, or the world retiring). A ticket is never reused.
 *
 * ---- (2) landing ready ---------------------------------------------------
 * The server arms a landing for (slot, world generation, landing x/z,
 * radius). The client reports READY exactly once, through the registered
 * sender, on the first frame on which ALL of:
 *   - the slot's CURRENT world context is bound to that generation,
 *   - the slot's payload is ready, and
 *   - every column within `radius` columns of the landing column (Chebyshev,
 *     (2r+1)^2 columns) is installed in that slot's terrain
 * hold. No view needs to show the world: the transferring player is still in
 * its source world until COMMIT (gap-doc blocker 3). The ARM itself prepares
 * an unviewed context for a slot the client does not hold yet
 * (clin_world_payload_prepare_landing), so the server's stamped arrival
 * columns have somewhere to land; the COMMIT binds the view to it. A cancel
 * releases a context the arm prepared. A refused send retries next frame. A context that moves to another
 * generation (or goes away) turns the landing STALE: nothing is reported,
 * the server must re-arm. The server publishes membership only on READY.
 */

#ifndef CLIENT_ADMISSION_H
#define CLIENT_ADMISSION_H

#include <stdbool.h>
#include <stdint.h>

#include "../world.h"

/* ---- (1) capacity ---- */

struct clin_capacity_need {
	uint32_t mem1_bytes; /* terrain + mesh bytes the server expects in MEM1 */
	uint32_t mem2_bytes; /* ... and in MEM2 (the display-list carve, Wii) */
};

enum clin_capacity_result {
	CLIN_CAP_OK = 0,
	CLIN_CAP_DISABLED,		/* world-bound payloads are off */
	CLIN_CAP_BAD_SLOT,		/* slot out of range, or player 0's own slot */
	CLIN_CAP_BUSY,			/* the slot already holds a reservation */
	CLIN_CAP_NO_CONTEXT,	/* the context pool cannot take another world */
	CLIN_CAP_NO_MEM1,
	CLIN_CAP_NO_MEM2,
};

/* High-bit namespace shared by the client reply mailbox and the server's
 * single outstanding guest respawn capacity request. */
#define CLIN_RESPAWN_CAP_ID_BIT 0x40000000u

typedef uint32_t clin_capacity_ticket; /* 0 = none */

/* The free-memory view the budget is judged against, in bytes. Default: the
 * platform gauges (Wii: MEM1 arena + MEM2 carve free; PC: unlimited). */
struct clin_capacity_budget {
	uint64_t mem1_free;
	uint64_t mem2_free;
};
typedef void (*clin_capacity_budget_fn)(struct clin_capacity_budget* out);
void clin_capacity_set_budget_provider(clin_capacity_budget_fn fn);
/* Bytes held back from every reservation (the rest of the session needs
 * headroom too). Defaults: CLIN_CAP_MEM1_MARGIN / CLIN_CAP_MEM2_MARGIN. */
#define CLIN_CAP_MEM1_MARGIN (4u * 1024u * 1024u)
#define CLIN_CAP_MEM2_MARGIN (2u * 1024u * 1024u)

enum clin_capacity_result clin_capacity_reserve(unsigned slot,
	const struct clin_capacity_need* need, clin_capacity_ticket* out);
/* The same for a known world instance (the stamped CRPC_CAPACITY_REQUEST's
 * generation). A second reservation for the SAME slot+generation -- two
 * actors entering one world back to back -- SHARES the first (OK, its own
 * ticket, no extra bytes; the budget must still hold what is held), and a
 * reservation for player 0's own world at its current generation is a share
 * too. A ready, viewed context of the same generation also shares when the
 * host has moved primary to another slot but the guest kept that world live.
 * This is per-world sharing; it does not measure the incremental column union
 * of two distant actors. A different generation in a held slot stays BUSY.
 * Generation 0 (the legacy form above) never shares. Releasing the owning
 * ticket while shares remain hands the bytes to one of them. */
enum clin_capacity_result clin_capacity_reserve_world(unsigned slot,
	uint32_t generation, const struct clin_capacity_need* need,
	clin_capacity_ticket* out);
/* RESERVED -> COMMITTED. False (nothing changed) for an unknown, released
 * or already committed ticket. */
bool clin_capacity_commit(clin_capacity_ticket ticket);
/* Either state -> free. False (nothing changed) for an unknown ticket. */
bool clin_capacity_release(clin_capacity_ticket ticket);
/* Diagnostics / tests: the totals currently held. */
void clin_capacity_held(uint64_t* mem1, uint64_t* mem2, unsigned* contexts);
/* Session teardown (flag flip): drop every reservation. */
void clin_capacity_reset(void);

/* ---- (2) landing ready ---- */

struct clin_landing {
	uint32_t token;			/* the server's id for this admission */
	unsigned slot;
	uint32_t generation;	/* the world the guest lands in */
	w_coord_t x, z;			/* landing position, blocks */
	int radius;				/* columns around the landing column */
};

enum clin_landing_state {
	CLIN_LANDING_IDLE = 0,
	CLIN_LANDING_WAITING,
	CLIN_LANDING_REPORTING,	/* ready; the sender refused, retrying */
	CLIN_LANDING_READY,		/* reported */
	CLIN_LANDING_STALE,		/* the world moved on; nothing reported */
};

typedef bool (*clin_landing_ready_sender)(uint32_t token, unsigned slot,
										   uint32_t generation);
/* NULL restores the default: queue SRPC_LANDING_READY through svin. */
void clin_landing_set_sender(clin_landing_ready_sender sender);
/* O15: the production READY sender itself (SRPC_LANDING_READY through the
 * owned inbox), so a test seam can OBSERVE the real report and forward it
 * unchanged instead of re-implementing it. */
bool clin_landing_ready_send_default(uint32_t token, unsigned slot,
									 uint32_t generation);
/* Arm (replacing any previous landing). False for a bad slot / radius or
 * with world-bound payloads off. */
bool clin_landing_arm(const struct clin_landing* landing);
void clin_landing_cancel(void);
/* Once per frame (clin_update / clin_update_drain call it). */
enum clin_landing_state clin_landing_poll(void);
enum clin_landing_state clin_landing_state(void);

/* ---- wire consumers (client side landed; SRPC replies are the handoff) ----
 * CRPC_CAPACITY_REQUEST (client_interface.h) runs one op and answers through
 * the capacity reply sender; CRPC_LANDING_ARM arms (or cancels) the landing
 * of the world it is STAMPED with. */
enum clin_capacity_op {
	CLIN_CAP_OP_RESERVE = 0,
	CLIN_CAP_OP_COMMIT = 1,
	CLIN_CAP_OP_RELEASE = 2,
};
/* result: an enum clin_capacity_result for RESERVE; for COMMIT/RELEASE
   CLIN_CAP_OK on success, CLIN_CAP_BUSY for an unknown/stale ticket. The
   reply is offered once for ordinary requests. A refused guest-respawn RESERVE
   reply is retained and retried each client frame, including refusal results;
   other refused replies keep the existing server timeout/re-ask rule. */
typedef bool (*clin_capacity_reply_sender)(uint32_t request_id,
										   enum clin_capacity_result result,
										   clin_capacity_ticket ticket);
/* NULL restores the default: queue SRPC_CAPACITY_REPLY through svin. */
void clin_capacity_set_reply_sender(clin_capacity_reply_sender sender);
struct client_rpc;
void clin_admission_handle_capacity(const struct client_rpc* call);
void clin_admission_handle_landing(const struct client_rpc* call);
/* CRPC_WORLD_RELEASE applied for (slot, generation): a landing armed into
 * exactly that world goes STALE (the server must re-arm); when `retiring`,
 * the slot's capacity reservation (either state) is also returned -- the
 * world it was held for is gone. A drained LIVE world (retiring false) keeps
 * its reservation: the server may show that world again. */
void clin_admission_world_released(unsigned slot, uint32_t generation,
								   bool retiring);

/* ---- wire messages (lane A reply 2026-09-27; final) ----
 * CRPC_CAPACITY_REQUEST  {request_id, op: RESERVE|COMMIT|RELEASE, slot,
 *                         mem1_bytes, mem2_bytes, ticket}
 * SRPC_CAPACITY_REPLY    {request_id, result (enum clin_capacity_result),
 *                         ticket}
 * CRPC_LANDING_ARM       {token, world_index, world_generation, x, z,
 *                         radius}   (and a cancel form)
 * SRPC_LANDING_READY     {token, slot, generation}
 * CRPC_WORLD_RELEASE     {world_index, world_generation, retiring}
 * The client handlers are thin: they call the functions above and answer
 * through the registered senders, whose defaults queue the two SRPCs with
 * svin_rpc_send (server_interface.h). The server-side consumers are lane
 * A's. */

#endif
