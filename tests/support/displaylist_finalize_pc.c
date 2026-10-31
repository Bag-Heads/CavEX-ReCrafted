/* Run the actual PC emitter/finalizer, without creating a GL context. */
#define PLATFORM_PC
#define CAVEWII_DISPLAYLIST_TEST
#include "../../source/platform/pc/displaylist.c"
#include <stdio.h>
#include <stdlib.h>

void test_pc_finalize_equiv(void) {
	assert(!displaylist_compact_enabled());
	uint64_t before = 0, after = 0;
	unsigned cases = 0;
	for(unsigned tinted = 0; tinted < 2; tinted++)
	for(unsigned count = 1; count <= 1025; count++)
	for(unsigned hint = 0; hint < 3; hint++)
	for(unsigned fail = 0; fail < 3; fail++) {
		displaylist_set_compact(fail != 2);
		struct displaylist l;
		displaylist_init_presized(&l, 4, 12, tinted,
			hint == 0 ? 0 : hint == 1 ? count : count * 3);
		displaylist_set_tinted(&l, tinted);
		for(unsigned i = 0; i < count; i++) {
			displaylist_set_tint(&l, i, i * 3, i * 7);
			displaylist_vertex(&l, i * 13, -i, i * 31, i, i * 5, i * 11);
		}
		assert(!l.truncated);
		size_t used = count * (tinted ? 25 : 22), capacity = l.length;
		assert(l.index == used && capacity >= used);
		void* original = l.data;
		uint8_t* reference = malloc(used);
		assert(reference); memcpy(reference, l.data, used);
		displaylist_test_fail_realloc_after(fail ? 1 : 0);
		displaylist_finalize(&l, count);
		displaylist_test_fail_realloc_after(0);
		assert(l.index == count && !l.truncated);
		assert(!memcmp(reference, l.data, used));
		assert(l.length == (fail ? capacity : used));
		if(fail) assert(l.data == original);
		uint32_t n, slack, pad;
		displaylist_storage_stats(&n, &slack, &pad);
		assert(n == 1 && slack == l.length - used && pad == 0);
		if(!fail) { before += capacity - used; after += slack; cases++; }
		displaylist_destroy(&l); free(reference);
		displaylist_storage_stats(&n, &slack, &pad);
		assert(!n && !slack && !pad);
	}
	/* Failed initial allocation and failed grow still drop safely. */
	for(unsigned grow = 0; grow < 2; grow++) {
		struct displaylist l;
		displaylist_init(&l, 4, 12);
		if(grow) displaylist_test_fail_realloc_after(1);
		else displaylist_test_fail_malloc_after(1);
		for(unsigned i = 0; i < 1025; i++)
			displaylist_vertex(&l, i, i, i, i, i, i);
		assert(l.truncated);
		displaylist_destroy(&l);
	}
	printf("PASS PC actual finalizer 22/25 strides, boundaries/hints/failure: "
		"%u successful lists, summed dls %llu -> %llu B, dlp 0 -> 0 B\n",
		cases, (unsigned long long)before, (unsigned long long)after);
}
