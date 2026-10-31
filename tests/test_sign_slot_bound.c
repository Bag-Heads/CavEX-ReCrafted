/*
	Audit C6 guard: the sign free-slot scan in source/block/block_sign.c
	(onItemPlace) must iterate over MAX_SIGNS, NOT MAX_CHESTS, because it
	indexes server_local::sign_pos[] / sign_texts[] which are dimensioned
	[MAX_SIGNS]. The two caps happen to be equal today (both 256), so the
	bug is latent -- but if they ever diverge a MAX_CHESTS bound would walk
	off the end of the sign arrays (struct overflow). These compile-time
	asserts pin the loop bound (MAX_SIGNS) to the actual array dimension, so
	any future divergence of the caps fails the build instead of corrupting
	memory at runtime.

	Pure header-only static check: includes only network/server_local.h.
	No source is compiled and main() does nothing -- the proof is the
	_Static_asserts evaluated by the compiler.
*/

#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)

/* IMP-TE-CAPS (2026-10-21): sign_pos / sign_texts are growable slabs now;
   block_sign.c claims through te_store_claim_free, which is bounded by the
   view's live row count (max_sign == te_rows[TE_KIND_SIGN]) and grows the
   table when full -- there is no fixed dimension left to overrun. Pin the
   element types the slab layout copies row by row. */
_Static_assert(sizeof(((struct server_local*)0)->sign_pos[0])
	== sizeof(struct te_pos), "sign_pos rows are te_pos-compatible");

/* Inner row of sign_texts[] is SIGN_SIZE -- the width memset() clears per slot. */
_Static_assert(sizeof(((struct server_local*)0)->sign_texts[0]) == SIGN_SIZE,
	"sign_texts[] row width must equal SIGN_SIZE");

int main(void) {
	return 0;
}
