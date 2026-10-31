/*
	Equivalence test for the optimized smooth-lighting (AO) path in
	source/chunk_mesher.c.

	The optimization replaces four BLK_DATA macro expansions per axis with
	constant pointer offsets from one base, and a per-sample blocks[type]
	predicate with a precomputed table. This test embeds BOTH the original
	reference sampling/averaging and the optimized one (the predicate is
	modelled identically by a passes[] table in both, so the test isolates
	the risky part: that the pointer offsets address exactly the same four
	neighbours the macro version did, including across the padded boundary
	shell). It asserts byte-for-byte identical light_data over randomized
	grids. If this passes, the optimization cannot have changed a single
	lighting value — Beta's smooth lighting is preserved exactly.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE 16
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define P (CHUNK_SIZE + 2)

// Identical to the macros in chunk_mesher.c.
#define BLK_INDEX(x, y, z) ((x) + ((z) + (y) * P) * P)
#define BLK_DATA(b, x, y, z) ((b)[BLK_INDEX((x) + 1, (y) + 1, (z) + 1)])

typedef uint32_t c_coord_t;

// Stand-in for struct block_data: the AO code only reads these three fields.
struct bd_t {
	uint8_t type;
	uint8_t sky_light;
	uint8_t torch_light;
};

static uint8_t passes[256];

// ---- Reference: the original inline algorithm (predicate via passes[]). ----
static void vlight_ref(struct bd_t* bd, uint8_t* light_data) {
	const int shade_table[5] = {0, 5, 3, 1, 0};
	for(c_coord_t y = 0; y < P; y++)
		for(c_coord_t z = 0; z < P; z++)
			for(c_coord_t x = 0; x < P; x++) {
				if(x != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct bd_t b1[4] = {
						BLK_DATA(bd, x + 0, y - 1, z + 0),
						BLK_DATA(bd, x - 1, y - 1, z + 0),
						BLK_DATA(bd, x + 0, y - 1, z - 1),
						BLK_DATA(bd, x - 1, y - 1, z - 1),
					};
					int ss = 0, st = 0, c = 0;
					for(int k = 0; k < 4; k++)
						if(passes[b1[k].type]) {
							ss += b1[k].sky_light;
							st += b1[k].torch_light;
							c++;
						}
					st = c > 0 ? (st + c - 1) / c : 0;
					ss = c > 0 ? (ss + c - 1) / c : 0;
					st = MAX(st - shade_table[c], 0);
					ss = MAX(ss - shade_table[c], 0);
					light_data[BLK_INDEX(x, y, z) * 3 + 0] = (st << 4) | ss;
				}
				if(y != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct bd_t b2[4] = {
						BLK_DATA(bd, x - 1, y + 0, z + 0),
						BLK_DATA(bd, x - 1, y - 1, z + 0),
						BLK_DATA(bd, x - 1, y + 0, z - 1),
						BLK_DATA(bd, x - 1, y - 1, z - 1),
					};
					int ss = 0, st = 0, c = 0;
					for(int k = 0; k < 4; k++)
						if(passes[b2[k].type]) {
							ss += b2[k].sky_light;
							st += b2[k].torch_light;
							c++;
						}
					st = c > 0 ? (st + c - 1) / c : 0;
					ss = c > 0 ? (ss + c - 1) / c : 0;
					st = MAX(st - shade_table[c], 0);
					ss = MAX(ss - shade_table[c], 0);
					light_data[BLK_INDEX(x, y, z) * 3 + 1] = (st << 4) | ss;
				}
				if(x != CHUNK_SIZE + 1 && y != CHUNK_SIZE + 1) {
					struct bd_t b3[4] = {
						BLK_DATA(bd, x + 0, y + 0, z - 1),
						BLK_DATA(bd, x - 1, y + 0, z - 1),
						BLK_DATA(bd, x + 0, y - 1, z - 1),
						BLK_DATA(bd, x - 1, y - 1, z - 1),
					};
					int ss = 0, st = 0, c = 0;
					for(int k = 0; k < 4; k++)
						if(passes[b3[k].type]) {
							ss += b3[k].sky_light;
							st += b3[k].torch_light;
							c++;
						}
					st = c > 0 ? (st + c - 1) / c : 0;
					ss = c > 0 ? (ss + c - 1) / c : 0;
					st = MAX(st - shade_table[c], 0);
					ss = MAX(ss - shade_table[c], 0);
					light_data[BLK_INDEX(x, y, z) * 3 + 2] = (st << 4) | ss;
				}
			}
}

// ---- Optimized: pointer-offset sampling (mirrors chunk_mesher.c). ----
static inline uint8_t ao_avg(const struct bd_t* s0, const struct bd_t* s1,
							 const struct bd_t* s2, const struct bd_t* s3) {
	static const int shade_table[5] = {0, 5, 3, 1, 0};
	const struct bd_t* s[4] = {s0, s1, s2, s3};
	int ss = 0, st = 0, c = 0;
	for(int k = 0; k < 4; k++)
		if(passes[s[k]->type]) {
			ss += s[k]->sky_light;
			st += s[k]->torch_light;
			c++;
		}
	st = c > 0 ? (st + c - 1) / c : 0;
	ss = c > 0 ? (ss + c - 1) / c : 0;
	st = MAX(st - shade_table[c], 0);
	ss = MAX(ss - shade_table[c], 0);
	return (uint8_t)((st << 4) | ss);
}

static void vlight_opt(struct bd_t* bd, uint8_t* light_data) {
	const int SX = 1, SZ = P, SY = P * P;
	for(c_coord_t y = 0; y < P; y++)
		for(c_coord_t z = 0; z < P; z++)
			for(c_coord_t x = 0; x < P; x++) {
				size_t out = BLK_INDEX(x, y, z) * 3;
				if(x != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct bd_t* b = &BLK_DATA(bd, x, y - 1, z);
					light_data[out + 0] = ao_avg(b, b - SX, b - SZ, b - SZ - SX);
				}
				if(y != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct bd_t* b = &BLK_DATA(bd, x - 1, y, z);
					light_data[out + 1] = ao_avg(b, b - SY, b - SZ, b - SY - SZ);
				}
				if(x != CHUNK_SIZE + 1 && y != CHUNK_SIZE + 1) {
					struct bd_t* b = &BLK_DATA(bd, x, y, z - 1);
					light_data[out + 2] = ao_avg(b, b - SX, b - SY, b - SY - SX);
				}
			}
}

int main(void) {
	static struct bd_t bd[P * P * P];
	static uint8_t la_ref[P * P * P * 3];
	static uint8_t la_opt[P * P * P * 3];

	for(int seed = 1; seed <= 16; seed++) {
		srand((unsigned)seed);
		for(int t = 0; t < 256; t++)
			passes[t] = (uint8_t)(rand() & 1);
		for(int i = 0; i < P * P * P; i++) {
			bd[i].type = (uint8_t)(rand() & 0xFF);
			bd[i].sky_light = (uint8_t)(rand() % 16);
			bd[i].torch_light = (uint8_t)(rand() % 16);
		}
		// 0xCC sentinel: cells skipped by the (identical) guards must stay
		// untouched in BOTH, so they still compare equal.
		memset(la_ref, 0xCC, sizeof(la_ref));
		memset(la_opt, 0xCC, sizeof(la_opt));
		vlight_ref(bd, la_ref);
		vlight_opt(bd, la_opt);
		assert(memcmp(la_ref, la_opt, sizeof(la_ref)) == 0);
	}

	return 0;
}
