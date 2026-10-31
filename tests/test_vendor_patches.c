/*
	Pin the CaveWii local patches to vendored third-party code (bug-hunt
	2026-07-17 D7). A wholesale re-vendor of third_party/m-lib would silently
	revert the OOM-transactional rehash/shrink patch that the setjmp/longjmp
	dict allocators in source/world.c and source/entity/entity.h depend on;
	upstream's grow-rehash scratch bypasses the transaction's M_MEMORY_FULL
	escape (fatal abort mid-rehash) and upstream's shrink NULL-derefs under
	NDEBUG on realloc failure. This marker check turns that silent revert into
	a loud compile-time suite failure. See third_party/m-lib/PATCHES.md.
*/

#include <m-lib/m-dict.h>

#ifndef M_DICT_CAVEWII_OOM_PATCH
#error "third_party/m-lib/m-dict.h lost the CAVEWII PATCH marker (re-vendored?). Reapply per third_party/m-lib/PATCHES.md -- the Wii dict OOM transactions are unsound without it."
#endif

#include <stdio.h>

int main(void) {
	printf("test_vendor_patches: OK (m-dict CaveWii OOM patch present)\n");
	return 0;
}
