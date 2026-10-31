/*
	T1 equivalence test for the 2026-09-01 cNBT buffer pre-size fix
	(source/cNBT/buffer.c, source/cNBT/nbt_parsing.c, source/cNBT/nbt_loading.c).

	nbt_dump_binary's returned buffer used to ALWAYS start at lazy_init's
	1024-byte default and double its way up -- ~18 realloc+copy cycles to
	reach a full chunk's ~82KB uncompressed payload, a ~192KB MEM1 transient
	peak per chunk save/load. The fix adds an opt-in size-hint entry point
	(nbt_dump_binary_hint / nbt_dump_compressed_hint) that pre-sizes the
	buffer's single backing allocation via the new buffer_reserve_exact, and
	region_archive.c's chunk-save path (region_archive_set_chunk) now calls
	it with an ~82KB hint. The vendored zero-argument entry points
	(nbt_dump_binary / nbt_dump_compressed) are unchanged thin wrappers that
	pass size_hint=0, reproducing the exact prior behaviour.

	This test proves, on a chunk-shaped tree (five big byte arrays, like a
	real Level compound):
	  1. buffer_reserve_exact allocates the buffer's cap in ONE call, no
	     growth needed for a payload that fits within it.
	  2. nbt_dump_binary(tree) and nbt_dump_binary_hint(tree, HINT) produce
	     BYTE-IDENTICAL output (length and content) -- the pre-size changes
	     only the allocation shape, never the dumped bytes.
	  3. A hint SMALLER than the eventual payload still degrades correctly
	     (buffer_reserve's normal grow-by-doubling takes over) and still
	     produces the identical bytes -- a bad/undersized hint can never
	     corrupt output, only cost back some of the avoided reallocs.
	  4. nbt_dump_compressed_hint produces byte-identical COMPRESSED output
	     to nbt_dump_compressed for the same tree (the hint only touches the
	     intermediate uncompressed buffer, never reaches the compressor).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cNBT/nbt.h"
#include "cNBT/buffer.h"

// Mirrors a real chunk's Level byte arrays (16x16x128): Blocks + 3 nibble
// arrays half that size + a small HeightMap -- the ~82KB shape the fix
// targets. Sized down (still four-digit-KB) so the test runs fast; the
// SHAPE (several large byte arrays inside one compound) is what matters,
// not hitting the exact 82176-byte figure.
#define BLOCKS_LEN (16 * 16 * 128)
#define NIB_LEN	   (BLOCKS_LEN / 2)
#define HM_LEN	   (16 * 16)
#define TEST_HINT  (BLOCKS_LEN + 3 * NIB_LEN + HM_LEN + 4096)

// strdup is a GNU/POSIX extension, not standard C -- nbt_treeops.c avoids it
// for the same reason (see its own _nbt_strdup). nbt_free() frees ->name
// unconditionally, so every name here must be its own malloc'd buffer.
static char* dup_name(const char* s) {
	size_t n = strlen(s) + 1;
	char* r = malloc(n);
	assert(r);
	memcpy(r, s, n);
	return r;
}

static nbt_node* make_byte_array(const char* name, size_t len, uint8_t seed) {
	nbt_node* n = calloc(1, sizeof(*n));
	assert(n);
	n->type = TAG_BYTE_ARRAY;
	n->name = dup_name(name);
	assert(n->name);
	n->payload.tag_byte_array.length = (int32_t)len;
	n->payload.tag_byte_array.data = malloc(len);
	assert(n->payload.tag_byte_array.data);
	for(size_t i = 0; i < len; i++)
		n->payload.tag_byte_array.data[i] = (uint8_t)(i * 7 + 3 + seed);
	return n;
}

// Builds a fresh chunk-shaped compound tree (caller owns; free with nbt_free).
static nbt_node* build_chunk_shaped_tree(void) {
	struct nbt_list* level_sentinel = malloc(sizeof(*level_sentinel));
	struct nbt_list* root_sentinel = malloc(sizeof(*root_sentinel));
	assert(level_sentinel && root_sentinel);
	level_sentinel->data = NULL;
	root_sentinel->data = NULL;
	INIT_LIST_HEAD(&level_sentinel->entry);
	INIT_LIST_HEAD(&root_sentinel->entry);

	struct {
		const char* name;
		size_t len;
		uint8_t seed;
	} fields[] = {
		{"Blocks", BLOCKS_LEN, 1},
		{"Data", NIB_LEN, 2},
		{"SkyLight", NIB_LEN, 3},
		{"BlockLight", NIB_LEN, 4},
		{"HeightMap", HM_LEN, 5},
	};
	for(size_t k = 0; k < sizeof(fields) / sizeof(*fields); k++) {
		struct nbt_list* entry = malloc(sizeof(*entry));
		assert(entry);
		entry->data = make_byte_array(fields[k].name, fields[k].len,
									  fields[k].seed);
		list_add_tail(&entry->entry, &level_sentinel->entry);
	}

	nbt_node* level = calloc(1, sizeof(*level));
	assert(level);
	level->type = TAG_COMPOUND;
	level->name = dup_name("Level");
	assert(level->name);
	level->payload.tag_compound = level_sentinel;

	struct nbt_list* root_entry = malloc(sizeof(*root_entry));
	assert(root_entry);
	root_entry->data = level;
	list_add_tail(&root_entry->entry, &root_sentinel->entry);

	nbt_node* root = calloc(1, sizeof(*root));
	assert(root);
	root->type = TAG_COMPOUND;
	root->name = dup_name("");
	assert(root->name);
	root->payload.tag_compound = root_sentinel;

	return root;
}

int main(void) {
	// --- 1. buffer_reserve_exact pre-sizes in one shot, no growth needed ---
	{
		struct buffer b = BUFFER_INIT;
		assert(buffer_reserve_exact(&b, TEST_HINT) == 0);
		assert(b.data != NULL);
		assert(b.cap == TEST_HINT); // exact, unlike buffer_reserve's doubling
		assert(b.len == 0);
		// Appending well within the pre-sized capacity must not reallocate
		// the pointer (a cheap, if not airtight, proxy: cap stays put).
		uint8_t chunk[256];
		memset(chunk, 0x5a, sizeof(chunk));
		assert(buffer_append(&b, chunk, sizeof(chunk)) == 0);
		assert(b.cap == TEST_HINT);
		assert(b.len == sizeof(chunk));
		buffer_free(&b);
	}

	// --- 2/3. byte-identical dump: no hint, exact hint, undersized hint ---
	nbt_node* tree_a = build_chunk_shaped_tree();
	nbt_node* tree_b = build_chunk_shaped_tree();
	nbt_node* tree_c = build_chunk_shaped_tree();
	nbt_node* tree_d = build_chunk_shaped_tree();

	struct buffer dump_default = nbt_dump_binary(tree_a);
	struct buffer dump_hinted = nbt_dump_binary_hint(tree_b, TEST_HINT);
	struct buffer dump_undersized = nbt_dump_binary_hint(tree_c, 64);
	struct buffer dump_zero = nbt_dump_binary_hint(tree_d, 0);

	assert(dump_default.data && dump_hinted.data && dump_undersized.data
		   && dump_zero.data);
	assert(dump_default.len > (size_t)BLOCKS_LEN); // sanity: real payload, not a stub
	assert(dump_default.len == dump_hinted.len);
	assert(memcmp(dump_default.data, dump_hinted.data, dump_default.len) == 0);
	assert(dump_default.len == dump_undersized.len);
	assert(memcmp(dump_default.data, dump_undersized.data, dump_default.len)
		   == 0);
	assert(dump_default.len == dump_zero.len);
	assert(memcmp(dump_default.data, dump_zero.data, dump_default.len) == 0);

	// The hinted dump's buffer never had to grow past the hint (it fits),
	// so its cap should equal the hint exactly -- the pre-size actually
	// took effect, not merely "happened to produce the same bytes".
	assert(dump_hinted.cap == TEST_HINT);

	buffer_free(&dump_default);
	buffer_free(&dump_hinted);
	buffer_free(&dump_undersized);
	buffer_free(&dump_zero);
	nbt_free(tree_a);
	nbt_free(tree_b);
	nbt_free(tree_c);
	nbt_free(tree_d);

	// --- 4. compressed variant: byte-identical to the unhinted compressor ---
	nbt_node* tree_e = build_chunk_shaped_tree();
	nbt_node* tree_f = build_chunk_shaped_tree();
	struct buffer comp_default = nbt_dump_compressed(tree_e, STRAT_INFLATE);
	struct buffer comp_hinted
		= nbt_dump_compressed_hint(tree_f, STRAT_INFLATE, TEST_HINT);
	assert(comp_default.data && comp_hinted.data);
	assert(comp_default.len == comp_hinted.len);
	assert(memcmp(comp_default.data, comp_hinted.data, comp_default.len) == 0);
	buffer_free(&comp_default);
	buffer_free(&comp_hinted);
	nbt_free(tree_e);
	nbt_free(tree_f);

	printf("test_nbt_buffer_size_hint: OK (buffer_reserve_exact one-shot "
		   "sizing; nbt_dump_binary_hint/nbt_dump_compressed_hint "
		   "byte-identical to the unhinted default)\n");
	return 0;
}
