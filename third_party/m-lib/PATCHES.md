# Local patches to vendored m-lib (M*LIB)

This directory is NOT a pristine upstream copy. `m-dict.h` carries two local
patches, marked in-file by the `CAVEWII PATCH` banner and the
`M_DICT_CAVEWII_OOM_PATCH` macro, and pinned by `tests/test_vendor_patches.c`
(a re-vendor that drops the macro fails the suite at compile time).

Recorded per bug-hunt 2026-07-17 finding **D7** (the patch predates this file;
it shipped undocumented inside the correctness batch).

## Why these patches exist

`source/world.c` (world-section dictionary) and `source/entity/entity.h`
(entity dictionary) wrap m-lib dictionary mutations in setjmp/longjmp
"OOM transactions": a custom allocator journals every realloc, a scoped
`M_MEMORY_FULL` longjmps out on failure, and the wrapper rolls the dictionary
back byte-identical. On the Wii (24MB MEM1, no MMU, no overcommit) this is the
difference between "drop one chunk section" and "abort mid-rehash with a
corrupted table".

Upstream m-dict defeats that design in two places:

## Patch 1 — `_i_resize_up` grow-rehash scratch through the key oplist

Upstream keeps displaced entries during the in-place grow-rehash in an
`m_array` backed by plain `malloc` + the GLOBAL fatal `M_MEMORY_FULL` handler.
Under the transaction that means: the journal never sees the allocation, the
longjmp escape is bypassed, and a mid-rehash OOM aborts the process with the
index half-migrated.

Patched behaviour: the worst-case scratch (`h->count` entries — every live
entry has `index >= 2`, so displaced pushes cannot exceed it) is allocated
ONCE, up front, via `M_CALL_REALLOC(key_oplist, ...)` BEFORE `index[]` is
mutated. OOM invokes the dict's own key-oplist `MEMORY_FULL` policy (the
transaction's longjmp) and leaves the dictionary untouched.

## Patch 2 — `_i_resize_down` shrink built off to the side

Upstream shrinks the index with an in-place compaction followed by
`realloc` down, then `M_ASSERT(h->index != NULL)` — under NDEBUG (the Wii
release .dol) a failed realloc is a NULL dereference.

Patched behaviour: the smaller index is built completely off to the side; on
OOM the optional shrink is skipped (`h->lower_limit = h->count`) and the old
physical+logical table stays intact. The old allocation is later freed at its
exact allocated size, preserving the sized-allocator contract the world.c
allocator relies on.

## Re-vendoring instructions

1. Diff the incoming upstream `m-dict.h` against this one; the two patch
   regions are `_i_resize_up` (displaced-entry scratch) and `_i_resize_down`
   (side-built index).
2. Reapply both patches and the `CAVEWII PATCH` banner +
   `M_DICT_CAVEWII_OOM_PATCH` define at the top.
3. `tests/test_vendor_patches.c` must compile and the dict-transaction suites
   in `tests/test_portal_travel.c` must pass before the re-vendor lands.

Other m-lib headers in this directory are unmodified upstream copies (verified
against the 0376fac re-vendor import).
