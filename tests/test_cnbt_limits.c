/* Corrupt-NBT resource and arithmetic bounds used by both level and region loads. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "cNBT/nbt.h"

static void put_u16_be(uint8_t* out, uint16_t value) {
	out[0] = (uint8_t)(value >> 8);
	out[1] = (uint8_t)value;
}

static void put_u32_be(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static void test_oversized_array(void) {
	uint8_t raw[7] = {TAG_INT_ARRAY, 0, 0};
	put_u32_be(raw + 3, UINT32_C(0x7fffffff));
	assert(nbt_parse(raw, sizeof(raw)) == NULL);

	raw[0] = TAG_LONG_ARRAY;
	assert(nbt_parse(raw, sizeof(raw)) == NULL);
}

static void test_empty_arrays(void) {
	uint8_t raw[7] = {TAG_BYTE_ARRAY, 0, 0, 0, 0, 0, 0};
	for(uint8_t type = TAG_BYTE_ARRAY; type <= TAG_LONG_ARRAY; type++) {
		if(type == TAG_STRING || type == TAG_LIST || type == TAG_COMPOUND)
			continue;
		raw[0] = type;
		nbt_node* node = nbt_parse(raw, sizeof(raw));
		assert(node);
		nbt_free(node);
	}
}

static void test_nesting_limit(void) {
	enum { NESTED = 65 };
	uint8_t raw[3 + NESTED * 3 + NESTED + 1];
	size_t at = 0;
	raw[at++] = TAG_COMPOUND;
	put_u16_be(raw + at, 0);
	at += 2;
	for(int i = 0; i < NESTED; i++) {
		raw[at++] = TAG_COMPOUND;
		put_u16_be(raw + at, 0);
		at += 2;
	}
	for(int i = 0; i < NESTED + 1; i++)
		raw[at++] = TAG_INVALID;
	assert(at == sizeof(raw));
	assert(nbt_parse(raw, sizeof(raw)) == NULL);
}

static void test_node_limit(void) {
	enum { BYTE_TAGS = 131072 };
	size_t size = 3 + (size_t)BYTE_TAGS * 4 + 1;
	uint8_t* raw = malloc(size);
	assert(raw);
	size_t at = 0;
	raw[at++] = TAG_COMPOUND;
	put_u16_be(raw + at, 0);
	at += 2;
	for(int i = 0; i < BYTE_TAGS; i++) {
		raw[at++] = TAG_BYTE;
		put_u16_be(raw + at, 0);
		at += 2;
		raw[at++] = 0;
	}
	raw[at++] = TAG_INVALID;
	assert(at == size);
	assert(nbt_parse(raw, size) == NULL);
	free(raw);
}

static void test_decompression_limit(void) {
	uint8_t dummy = 0;
	assert(nbt_parse_compressed(&dummy, 8u * 1024u * 1024u + 1u) == NULL);

	const size_t raw_size = 8u * 1024u * 1024u + 1u;
	uint8_t* raw = calloc(raw_size, 1);
	assert(raw);
	uLongf compressed_size = compressBound((uLong)raw_size);
	uint8_t* compressed = malloc((size_t)compressed_size);
	assert(compressed);
	assert(compress2(compressed, &compressed_size, raw, (uLong)raw_size,
				 Z_BEST_COMPRESSION) == Z_OK);
	assert(nbt_parse_compressed(compressed, (size_t)compressed_size) == NULL);
	free(compressed);
	free(raw);
}

int main(void) {
	test_oversized_array();
	test_empty_arrays();
	test_nesting_limit();
	test_node_limit();
	test_decompression_limit();
	puts("test_cnbt_limits: bounded arrays, nesting, nodes, and decompression OK");
	return 0;
}
