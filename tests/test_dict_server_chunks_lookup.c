/*
	Investigation (followups lane, 2026-09-02): the server-local lane reported
	dict_server_chunks_get() returning NULL for a chunk id that
	dict_server_chunks_it() iteration had just shown present, reproduced for
	more than one coordinate, from test code "outside the world/near-send
	machinery". This test exercises the REAL API end to end -- insert via
	dict_server_chunks_set_at(chunks, S_CHUNK_ID(x,z), ...), iterate via
	dict_server_chunks_it()/_ref()/_next(), then dict_server_chunks_get() each
	iterated key -- across a coordinate set that includes negative x/z (Beta
	worlds extend negative in both axes) and values that would expose a
	sign-extension or operand-order slip in the 64-bit key packing.

	S_CHUNK_ID's own contract (server_world.h): pack x into the LOW 32 bits,
	z into the HIGH 32 bits, casting through (uint32_t) before the shift so a
	negative coordinate's bit pattern is preserved rather than sign-extended
	into the wrong half:
		S_CHUNK_ID(x, z) = ((uint64_t)(uint32_t)z << 32) | (uint32_t)x
		S_CHUNK_X(id)     = (int32_t)(id & 0xFFFFFFFF)
		S_CHUNK_Z(id)     = (int32_t)(id >> 32)
	This test locks the round-trip identity (S_CHUNK_ID(S_CHUNK_X(id),
	S_CHUNK_Z(id)) == id for every id this dict ever hands back) and proves
	get() never disagrees with what it() just enumerated, using ONLY the
	documented macros/functions -- no ad hoc key math of the test's own that
	could itself be the mismatch. See the commit message for the conclusion
	this test supports (root cause found here vs. not reproducible).

	Header-only: DICT_DEF2 (m-lib) generates the whole dict_server_chunks_*
	API as static inline functions at the point server_world.h expands it, so
	this links no .c file at all -- genuinely isolated from the 16k-line
	server_local.c / server_world.c bodies the real anomaly report came from.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "network/server_world.h"

// A minimal stand-in payload distinguishable per (x,z) without needing a
// real chunk's owned buffers (struct server_chunk's M_POD_OPLIST is a bare
// memcpy, so a zeroed-but-tagged value is a legitimate dict value).
static struct server_chunk make_tagged_chunk(int32_t x, int32_t z) {
	struct server_chunk sc;
	memset(&sc, 0, sizeof(sc));
	// Encode (x,z) into a field with no aliasing semantics of its own, purely
	// as an identity check after get() -- proves we got THIS entry, not just
	// "an" entry.
	sc.load_tick_rearm_cursor = (uint32_t)(((uint32_t)x) ^ ((uint32_t)z * 2654435761u));
	return sc;
}

static bool chunk_matches(const struct server_chunk* sc, int32_t x, int32_t z) {
	return sc->load_tick_rearm_cursor
		== (uint32_t)(((uint32_t)x) ^ ((uint32_t)z * 2654435761u));
}

int main(void) {
	dict_server_chunks_t chunks;
	dict_server_chunks_init(chunks);

	// Coordinate set deliberately covers all four sign combinations plus
	// zero and the int32 extremes -- exactly where a sign-extension or
	// operand-order bug in a hand-rolled key would show up first.
	static const int32_t coords[][2] = {
		{0, 0},		 {1, 1},		{-1, -1},	  {1, -1},
		{-1, 1},	 {5, -5},		{-5, 5},	  {100, -200},
		{-100, 200}, {INT32_MAX, 0}, {INT32_MIN, 0}, {0, INT32_MAX},
		{0, INT32_MIN}, {INT32_MIN, INT32_MIN}, {INT32_MAX, INT32_MAX},
	};
	const size_t n_coords = sizeof(coords) / sizeof(coords[0]);

	for(size_t i = 0; i < n_coords; i++) {
		int32_t x = coords[i][0], z = coords[i][1];
		struct server_chunk sc = make_tagged_chunk(x, z);
		dict_server_chunks_set_at(chunks, S_CHUNK_ID(x, z), sc);
	}

	// --- Phase 1: get() immediately after set_at(), using the SAME
	// S_CHUNK_ID(x, z) construction the insert used. This is the baseline
	// "engine self-consistency" check -- if this fails, the bug is in
	// S_CHUNK_ID/dict_server_chunks itself.
	for(size_t i = 0; i < n_coords; i++) {
		int32_t x = coords[i][0], z = coords[i][1];
		struct server_chunk* got = dict_server_chunks_get(chunks, S_CHUNK_ID(x, z));
		assert(got && "dict_server_chunks_get(S_CHUNK_ID(x,z)) NULL right "
					  "after set_at with the identical key -- engine bug");
		assert(chunk_matches(got, x, z));
	}
	printf("Phase 1 PASS: get(S_CHUNK_ID(x,z)) finds every inserted chunk "
		  "(%zu coords, incl. all sign combinations + INT32 extremes)\n",
		  n_coords);

	// --- Phase 2: the reported repro shape -- iterate via
	// dict_server_chunks_it()/_ref()/_next() to collect the keys the dict
	// itself reports present, then get() EACH of those exact keys. If
	// iteration and get() ever disagree on the same dict with no mutation in
	// between, that is the anomaly, isolated with zero test-side key math
	// beyond what it() handed back.
	int64_t iterated_keys[64];
	size_t n_iterated = 0;
	{
		dict_server_chunks_it_t it;
		dict_server_chunks_it(it, chunks);
		while(!dict_server_chunks_end_p(it)) {
			assert(n_iterated < sizeof(iterated_keys) / sizeof(iterated_keys[0]));
			iterated_keys[n_iterated++] = dict_server_chunks_ref(it)->key;
			dict_server_chunks_next(it);
		}
	}
	assert(n_iterated == n_coords);

	size_t get_after_iterate_failures = 0;
	for(size_t i = 0; i < n_iterated; i++) {
		int64_t key = iterated_keys[i];
		struct server_chunk* got = dict_server_chunks_get(chunks, key);
		if(!got) {
			get_after_iterate_failures++;
			fprintf(stderr,
					"REPRO: dict_server_chunks_get(id=%lld) NULL for a key "
					"dict_server_chunks_it() just enumerated (x=%d, z=%d "
					"per S_CHUNK_X/S_CHUNK_Z)\n",
					(long long)key, S_CHUNK_X(key), S_CHUNK_Z(key));
			continue;
		}
		assert(chunk_matches(got, S_CHUNK_X(key), S_CHUNK_Z(key)));
	}
	assert(get_after_iterate_failures == 0
		  && "dict_server_chunks_get() disagreed with dict_server_chunks_it() "
			 "on a key it just enumerated, real API, no mutation in between "
			 "-- see stderr for which (x,z)");
	printf("Phase 2 PASS: get() agrees with it() for all %zu iterated keys "
		  "-- no reproduction with the real API on this dict\n",
		  n_iterated);

	// --- Phase 3: the key-construction round trip named in the task brief.
	// Decompose each iterated key with S_CHUNK_X/S_CHUNK_Z, then re-form it
	// with S_CHUNK_ID and require BYTE-IDENTICAL int64 back -- this is
	// exactly "S_CHUNK_ID(x, z) vs whatever the iteration exposes": if a
	// caller ever decomposed a key one way and re-packed it another (e.g. an
	// operand swap, S_CHUNK_ID(z, x)), this catches it independent of the
	// dict entirely.
	for(size_t i = 0; i < n_iterated; i++) {
		int64_t key = iterated_keys[i];
		int32_t x = S_CHUNK_X(key), z = S_CHUNK_Z(key);
		int64_t reformed = S_CHUNK_ID(x, z);
		assert(reformed == key
			  && "S_CHUNK_ID(S_CHUNK_X(id), S_CHUNK_Z(id)) != id -- the "
				 "64-bit chunk key does not round-trip");
		// ...and the deliberately-wrong swapped order must NOT match one of
		// our other inserted coordinates by accident (guards against a
		// symmetric coordinate set hiding a real swap bug).
		int64_t swapped = S_CHUNK_ID(z, x);
		if(x != z)
			assert(swapped != key);
	}
	printf("Phase 3 PASS: S_CHUNK_ID/S_CHUNK_X/S_CHUNK_Z round-trip "
		  "byte-identically for all %zu keys\n",
		  n_iterated);

	dict_server_chunks_clear(chunks);

	printf("PASS: test_dict_server_chunks_lookup (no engine-side dict/key "
		  "bug found with the real API -- see commit message)\n");
	return 0;
}
