/* split-dims B1: per-slot lifetime generation high-water mark and exhaustion.
 *
 * Contract (ASTRA-B1-RULINGS-0925 #1): every successful init/reset issues a
 * generation greater than every generation previously issued for that
 * world_index slot during the client lifetime. The high-water mark survives
 * the destruction of the context that issued it (reconnect, respawn, world
 * switch). It never wraps and never reuses. When the slot is exhausted,
 * init and reset FAIL and leave the context they were given byte-for-byte
 * unchanged; the other slot is unaffected.
 *
 * client_ctx_slot_high_water_raise() only ever raises the mark (a floor), so
 * it cannot break monotonicity; it is how this test reaches the top of the
 * 32-bit range without 4 billion resets. */
#include "client_context_red.h"

#include <string.h>

int main(void) {
	struct client_world_context ctx;
	PRE(client_ctx_init(&ctx, 0, 0));
	PRE(client_ctx_bind_view(&ctx, 0));
	uint32_t g1 = ctx.generation;
	PRE(g1 != 0);

	/* The slot mark tracks what was issued; the other slot has issued nothing. */
	CONTRACT(client_ctx_slot_high_water(0) == g1,
			 "slot 0 high-water does not reflect the generation init issued");
	CONTRACT(client_ctx_slot_high_water(1) == 0,
			 "slot 1 high-water is not zero before any context used it");
	CONTRACT(client_ctx_slot_high_water(2) == 0,
			 "out-of-range slot must read as zero, not index memory");

	/* Raising is a floor: it never lowers, never touches a live context. */
	CONTRACT(client_ctx_slot_high_water_raise(0, UINT32_MAX - 1),
			 "raising slot 0's high-water to UINT32_MAX-1 refused");
	CONTRACT(client_ctx_slot_high_water(0) == UINT32_MAX - 1,
			 "slot 0 high-water not raised");
	CONTRACT(!client_ctx_slot_high_water_raise(0, 5),
			 "lowering the high-water was accepted (monotonicity broken)");
	CONTRACT(client_ctx_slot_high_water(0) == UINT32_MAX - 1,
			 "a refused lower raise changed the high-water");
	CONTRACT(!client_ctx_slot_high_water_raise(2, 1),
			 "out-of-range slot raise accepted");
	PRE(ctx.generation == g1);
	PRE(client_ctx_accept(&ctx, 0, g1));

	/* The last generation the slot can issue. */
	CONTRACT(client_ctx_reset(&ctx),
			 "reset refused while one generation is still available");
	CONTRACT(ctx.generation == UINT32_MAX,
			 "reset did not issue the slot's next generation (high-water + 1)");
	CONTRACT(client_ctx_slot_high_water(0) == UINT32_MAX,
			 "slot 0 high-water not advanced by reset");
	PRE(!client_ctx_accept(&ctx, 0, g1));
	PRE(client_ctx_accept(&ctx, 0, ctx.generation));

	/* Exhausted: reset fails and the context is unchanged. */
	struct client_world_context before;
	memcpy(&before, &ctx, sizeof(before));
	CONTRACT(!client_ctx_reset(&ctx),
			 "reset succeeded on an exhausted slot (wrap or reuse)");
	CONTRACT(memcmp(&before, &ctx, sizeof(before)) == 0,
			 "failed reset modified the context");
	CONTRACT(client_ctx_accept(&ctx, 0, UINT32_MAX),
			 "context stopped accepting its own generation after a failed reset");

	/* Exhausted: init of a new occupant fails and leaves ITS struct untouched. */
	struct client_world_context fresh, fresh_copy;
	memset(&fresh, 0xA5, sizeof(fresh));
	memcpy(&fresh_copy, &fresh, sizeof(fresh));
	CONTRACT(!client_ctx_init(&fresh, 0, 0),
			 "init succeeded in an exhausted slot (wrap or reuse)");
	CONTRACT(memcmp(&fresh, &fresh_copy, sizeof(fresh)) == 0,
			 "failed init modified the context it was given");
	CONTRACT(client_ctx_slot_high_water(0) == UINT32_MAX,
			 "failed init/reset moved the slot high-water");

	/* The exhausted context keeps working for everything but re-generation. */
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_unbind_view(&ctx, 0));
	client_ctx_retire(&ctx);
	PRE(client_ctx_try_free(&ctx));

	/* The other slot is independent: its own counter, starting from zero. */
	struct client_world_context other;
	CONTRACT(client_ctx_init(&other, -1, 1),
			 "slot 1 init refused because slot 0 is exhausted");
	CONTRACT(other.generation == 1 && client_ctx_slot_high_water(1) == 1,
			 "slot 1 did not issue its first generation from its own mark");
	PRE(client_ctx_reset(&other));
	CONTRACT(other.generation == 2 && client_ctx_slot_high_water(1) == 2,
			 "slot 1 reset did not advance its own mark");
	CONTRACT(client_ctx_slot_high_water(0) == UINT32_MAX,
			 "slot 1 activity moved slot 0's mark");

	/* Destruction of the issuing context does not lower the mark: a new
	 * occupant of slot 1 continues past everything the slot ever issued. */
	client_ctx_retire(&other);
	PRE(client_ctx_try_free(&other));
	struct client_world_context again;
	PRE(client_ctx_init(&again, -1, 1));
	CONTRACT(again.generation == 3,
			 "re-created slot 1 context did not continue from the slot mark");
	CONTRACT(!client_ctx_accept(&again, 1, 1) && !client_ctx_accept(&again, 1, 2),
			 "re-created slot 1 context accepts a predecessor's generation");

	return b1_contract_finish("test_client_ctx_red_exhaustion");
}
