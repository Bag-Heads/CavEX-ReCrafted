/*
	Correctness guard for the vendored cNBT int/long array deep-copy + compare paths
	(source/cNBT/nbt_treeops.c nbt_clone/nbt_filter, source/cNBT/nbt_util.c nbt_eq).

	Beta 1.7.3 NBT has neither TAG_INT_ARRAY(11) nor TAG_LONG_ARRAY(12) (added in 1.2.1 / 1.12), so
	CaveWii never produces these tags today -- but the vendored code declared them with owned heap
	buffers while clone/filter under-copied int arrays by 4x (memcpy'd `length` BYTES of a length*4-byte
	buffer) and never deep-copied long arrays (aliased the pointer -> nbt_free(orig)+nbt_free(clone)
	double-free), and nbt_eq compared only `length` bytes (false-equal) / had no long-array case
	(false-unequal). This pins all four faithful element-sized paths so the latent corruption can never
	resurface. Run under ASan to catch the long-array double-free directly.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cNBT/nbt.h"

static nbt_node* make_int_array(const int32_t* vals, int32_t len) {
	nbt_node* n = malloc(sizeof *n);
	assert(n);
	n->type = TAG_INT_ARRAY;
	n->name = NULL;
	n->payload.tag_int_array.length = len;
	n->payload.tag_int_array.data = malloc((size_t)len * sizeof(int32_t));
	assert(n->payload.tag_int_array.data);
	memcpy(n->payload.tag_int_array.data, vals, (size_t)len * sizeof(int32_t));
	return n;
}
static nbt_node* make_long_array(const int64_t* vals, int32_t len) {
	nbt_node* n = malloc(sizeof *n);
	assert(n);
	n->type = TAG_LONG_ARRAY;
	n->name = NULL;
	n->payload.tag_long_array.length = len;
	n->payload.tag_long_array.data = malloc((size_t)len * sizeof(int64_t));
	assert(n->payload.tag_long_array.data);
	memcpy(n->payload.tag_long_array.data, vals, (size_t)len * sizeof(int64_t));
	return n;
}
static bool always_true(const nbt_node* node, void* aux) {
	(void)node;
	(void)aux;
	return true;
}

int main(void) {
	/* --- TAG_INT_ARRAY: clone copies ALL element bytes (the 4x under-copy bug) --- */
	const int32_t iv[4] = {(int32_t)0x11223344, (int32_t)0x55667788, -1, INT32_MIN};
	nbt_node* ia = make_int_array(iv, 4);
	nbt_node* ic = nbt_clone(ia);
	assert(ic);
	assert(ic->payload.tag_int_array.data != ia->payload.tag_int_array.data); /* distinct heap buffer */
	assert(memcmp(ic->payload.tag_int_array.data, iv, 4 * sizeof(int32_t)) == 0);
	for(int i = 0; i < 4; i++)
		assert(ic->payload.tag_int_array.data[i] == iv[i]); /* upper 3 bytes intact, not garbage */
	assert(nbt_eq(ia, ic));

	/* nbt_eq must compare every element byte: a difference at a LATE element is detected */
	nbt_node* ic2 = nbt_clone(ia);
	ic2->payload.tag_int_array.data[3] ^= 0x40000000; /* a high-bit-region change in the last int */
	assert(!nbt_eq(ia, ic2)); /* before the fix this wrongly returned true (compared only length bytes) */

	/* filter (always-true) deep-copies the same way */
	nbt_node* iflt = nbt_filter(ia, always_true, NULL);
	assert(iflt);
	assert(iflt->payload.tag_int_array.data != ia->payload.tag_int_array.data);
	assert(memcmp(iflt->payload.tag_int_array.data, iv, 4 * sizeof(int32_t)) == 0);

	nbt_free(ia);
	nbt_free(ic);
	nbt_free(ic2);
	nbt_free(iflt);

	/* --- TAG_LONG_ARRAY: clone/filter DEEP-copy (was aliased -> double-free) + eq has a case --- */
	const int64_t lv[3] = {(int64_t)0x1122334455667788LL, -1LL, INT64_MIN};
	nbt_node* la = make_long_array(lv, 3);
	nbt_node* lc = nbt_clone(la);
	assert(lc);
	assert(lc->payload.tag_long_array.data != la->payload.tag_long_array.data); /* DEEP, not aliased */
	assert(memcmp(lc->payload.tag_long_array.data, lv, 3 * sizeof(int64_t)) == 0);
	assert(nbt_eq(la, lc)); /* before the fix: TAG_LONG_ARRAY fell to default -> false */

	nbt_node* lflt = nbt_filter(la, always_true, NULL);
	assert(lflt);
	assert(lflt->payload.tag_long_array.data != la->payload.tag_long_array.data);
	assert(memcmp(lflt->payload.tag_long_array.data, lv, 3 * sizeof(int64_t)) == 0);

	/* the load-bearing check: freeing all three independent owners must NOT double-free */
	nbt_free(la);
	nbt_free(lc);
	nbt_free(lflt);

	printf("test_cnbt_arrays: OK (int-array full element copy/compare; long-array deep clone/filter, "
		   "eq case, no double-free)\n");
	return 0;
}
