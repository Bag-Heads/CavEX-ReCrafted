/*
	Tests source/worldgen/worldgen_hell.c -- the Beta 1.7.3 Nether (Hell)
	generator (ChunkProviderHell terrain/surface + MapGenCavesHell + populate).

	The byte-exact noise/PRNG foundations are proven in test_noise /
	test_java_random; this test exercises the Nether pipeline for its structural
	invariants, determinism, and a self-golden change-detector:
	  - every emitted TERRAIN id is one of the small Nether set (air / bedrock /
	    lavaStill / netherrack / soulsand / gravel) -- this doubles as the
	    no-natural-bricks guard for the Nether (crafted-only ids never appear),
	  - the roof (y=127) and floor (y=0) are ALWAYS bedrock (Beta func_4058_b),
	  - lavaStill never appears at/above sea level (y >= 64),
	  - regenerating is bit-identical; different chunk / different seed differ,
	  - a committed FNV digest pins the exact terrain bytes for a fixed seed/chunk.

	populate() is non-deterministic per-chunk in Beta (no reseed -- see
	worldgen_hell.c), so it is tested against an EXPLICIT RNG over a flat-buffer
	world-view: given a fixed RNG state the draw order / placements are byte-exact
	(one committed digest), every placed block is a decoration block, and each
	placement obeys its Beta rule (fire sits on netherrack). The two documented
	gaps (deferred lava updateTick draw; deferred mushroom light gate) live in the
	decoration layer only and do not affect the terrain guarantees above.

	Like test_worldgen.c, worldgen references the global block table + nibble
	helpers but the terrain ids don't consult the table, so a null table lets the
	test stand alone.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network/server_world.h"
#include "worldgen/java_random.h"
#include "worldgen/populate.h"
#include "worldgen/worldgen.h"
#include "worldgen/worldgen_internal.h" // worldgen_hell_populate

// --- stubbed external dependencies (mirror test_worldgen.c) ---------------
struct block;
struct block* blocks[256] = {0};

uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((data & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (data & 0x0F));
}
// -------------------------------------------------------------------------

#define NIDX(x, y, z)                                                          \
	((size_t)(y) + ((size_t)(z) + (size_t)(x) * CHUNK_SIZE) * WORLD_HEIGHT)

// Nether block ids (from blocks_data.h).
#define B_AIR 0
#define B_GRASS 2
#define B_DIRT 3
#define B_BEDROCK 7
#define B_LAVA_FLOW 10
#define B_LAVA_STILL 11
#define B_GRAVEL 13
#define B_BROWN_MUSHROOM 39
#define B_RED_MUSHROOM 40
#define B_FIRE 51
#define B_NETHERRACK 87
#define B_SOULSAND 88
#define B_GLOWSTONE 89
// Crafted-only ids that must NEVER appear in generated Nether (worldgen invariant).
#define B_PLANKS 5
#define B_BRICKS 45
#define B_BOOKSHELF 47
#define B_FURNACE 61

// Committed self-golden digests (change-detectors for CaveWii's OWN Nether
// output; NOT a Beta-parity proof -- that needs a real DIM-1 capture, which
// mc173 cannot provide as its src/gen/nether.rs is empty). A value of 0 means
// "unpinned" and skips the check; both are pinned from the first run below.
#define GOLDEN_TERRAIN 0x99cd09539ce8d3bdULL
#define GOLDEN_POPULATE 0x85d86c6497b0324eULL
#define GOLDEN_POPULATE_RNG_STATE 0x6d686e4380daULL

static const size_t CHUNK_BYTES
	= (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;

static bool valid_nether_terrain_id(uint8_t id) {
	switch(id) {
		case B_AIR:
		case B_BEDROCK:
		case B_LAVA_STILL:
		case B_NETHERRACK:
		case B_SOULSAND:
		case B_GRAVEL: return true;
		default: return false;
	}
}

static void free_chunk(struct server_chunk* sc) {
	free(sc->ids); // on PC mem2_free == free (see CLAUDE.md MEM1-RELIEF note)
	free(sc->metadata);
	free(sc->lighting_sky);
	free(sc->lighting_torch);
	free(sc->heightmap);
}

// FNV-1a/64 -- stable digest for the golden regression guards.
static uint64_t fnv1a(const uint8_t* data, size_t n) {
	uint64_t h = 1469598103934665603ULL;
	for(size_t i = 0; i < n; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	return h;
}

// ---- terrain tests -------------------------------------------------------

static void test_terrain(void) {
	const int64_t SEED = 2024;
	struct worldgen* wg = worldgen_create(SEED, WORLD_DIM_NETHER);
	assert(wg);

	struct server_chunk c;
	assert(worldgen_generate_chunk(wg, &c, 0, 0));

	// Determinism: regenerating (0,0) with the same generator is bit-identical.
	struct server_chunk c_again;
	assert(worldgen_generate_chunk(wg, &c_again, 0, 0));
	assert(memcmp(c.ids, c_again.ids, CHUNK_BYTES) == 0);
	free_chunk(&c_again);

	// A fresh generator with the same seed is bit-identical too.
	struct worldgen* wg_same = worldgen_create(SEED, WORLD_DIM_NETHER);
	assert(wg_same);
	struct server_chunk c_same;
	assert(worldgen_generate_chunk(wg_same, &c_same, 0, 0));
	assert(memcmp(c.ids, c_same.ids, CHUNK_BYTES) == 0);
	free_chunk(&c_same);
	worldgen_destroy(wg_same);

	// A different chunk and a different seed both differ.
	struct server_chunk c_far;
	assert(worldgen_generate_chunk(wg, &c_far, 5, -3));
	assert(memcmp(c.ids, c_far.ids, CHUNK_BYTES) != 0);

	struct worldgen* wg2 = worldgen_create(SEED + 1, WORLD_DIM_NETHER);
	assert(wg2);
	struct server_chunk c_seed;
	assert(worldgen_generate_chunk(wg2, &c_seed, 0, 0));
	assert(memcmp(c.ids, c_seed.ids, CHUNK_BYTES) != 0);

	// Structure sweep across several chunks (incl. negative coords): every id is
	// a valid Nether terrain id (=> no crafted-only bricks/planks/etc.), the roof
	// and floor of every column are bedrock, lava never reaches sea level, and
	// both netherrack and air are present.
	const int coords[][2] = {{0, 0}, {5, -3}, {-7, 2}, {-1, -1}, {13, 21}};
	for(size_t k = 0; k < sizeof(coords) / sizeof(coords[0]); k++) {
		struct server_chunk s;
		assert(worldgen_generate_chunk(wg, &s, coords[k][0], coords[k][1]));
		bool has_netherrack = false, has_air = false;
		for(int x = 0; x < CHUNK_SIZE; x++)
			for(int z = 0; z < CHUNK_SIZE; z++) {
				size_t col = ((size_t)z + (size_t)x * CHUNK_SIZE) * WORLD_HEIGHT;
				assert(s.ids[col + 0] == B_BEDROCK);   // floor
				assert(s.ids[col + 127] == B_BEDROCK); // roof
				for(int y = 0; y < WORLD_HEIGHT; y++) {
					uint8_t id = s.ids[col + y];
					assert(valid_nether_terrain_id(id));
					assert(id != B_BRICKS && id != B_PLANKS
						   && id != B_BOOKSHELF && id != B_FURNACE);
					if(id == B_LAVA_STILL)
						assert(y < 64); // lava sea never reaches sea level
					if(id == B_NETHERRACK)
						has_netherrack = true;
					if(id == B_AIR)
						has_air = true;
				}
			}
		assert(has_netherrack);
		assert(has_air);
		free_chunk(&s);
	}

	// Golden change-detector: exact terrain bytes for seed 2024, chunk (0,0).
	uint64_t g = fnv1a(c.ids, CHUNK_BYTES);
	printf("test_worldgen_hell: terrain golden (2024,0,0)=0x%016llx\n",
		   (unsigned long long)g);
	fflush(stdout);
	assert(GOLDEN_TERRAIN == 0x0ULL || g == GOLDEN_TERRAIN);

	free_chunk(&c);
	free_chunk(&c_far);
	free_chunk(&c_seed);
	worldgen_destroy(wg);
	worldgen_destroy(wg2);
}

static void test_nether_dimension_lighting(void) {
	const size_t nibble_bytes = CHUNK_BYTES / 2;
	struct worldgen* wg = worldgen_create(2024, WORLD_DIM_NETHER);
	assert(wg);
	worldgen_set_superflat(wg, true);

	struct server_chunk sc;
	assert(worldgen_generate_chunk(wg, &sc, 0, 0));
	for(int x = 0; x < CHUNK_SIZE; x++)
		for(int z = 0; z < CHUNK_SIZE; z++) {
			assert(sc.ids[NIDX(x, 0, z)] == B_BEDROCK);
			assert(sc.ids[NIDX(x, 1, z)] == B_DIRT);
			assert(sc.ids[NIDX(x, 2, z)] == B_DIRT);
			assert(sc.ids[NIDX(x, 3, z)] == B_GRASS);
		}
	for(size_t i = 0; i < nibble_bytes; i++)
		assert(sc.lighting_sky[i] == 0);

	// Production post-populate/final-relight contract: even poisoned input must
	// return with no Nether skylight, while reset_torch=false preserves block light.
	memset(sc.lighting_sky, 0xFF, nibble_bytes);
	memset(sc.lighting_torch, 0xA5, nibble_bytes);
	worldgen_lighting(&sc, false, WORLD_DIM_NETHER);
	for(size_t i = 0; i < nibble_bytes; i++) {
		assert(sc.lighting_sky[i] == 0);
		assert(sc.lighting_torch[i] == 0xA5);
	}

	free_chunk(&sc);
	worldgen_destroy(wg);
}

// ---- populate test (flat-buffer world-view, explicit RNG) ---------------

#define PW_W 48   // world x/z span [0,48): covers chunk (0,0) placements + spread
#define PW_H 128

struct pworld {
	uint8_t buf[(size_t)PW_W * PW_W * PW_H];
	int immediate_fluid_ticks;
	struct java_random* expected_feature_rng;
	int light_x, light_y, light_z, stored_light;
};

static int pw_full_light(void* ctx, int x, int y, int z) {
	struct pworld* p = ctx;
	return x == p->light_x && y == p->light_y && z == p->light_z
		? p->stored_light : 0;
}

static size_t pw_idx(int x, int y, int z) {
	return ((size_t)x * PW_W + (size_t)z) * PW_H + (size_t)y;
}
static uint8_t pw_get(void* ctx, int x, int y, int z) {
	struct pworld* p = ctx;
	if(x < 0 || x >= PW_W || z < 0 || z >= PW_W || y < 0 || y >= PW_H)
		return B_AIR; // out-of-world reads are AIR (wgen_world contract)
	return p->buf[pw_idx(x, y, z)];
}
static void pw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	(void)meta;
	struct pworld* p = ctx;
	if(x < 0 || x >= PW_W || z < 0 || z >= PW_W || y < 0 || y >= PW_H)
		return; // out-of-world writes ignored
	p->buf[pw_idx(x, y, z)] = id;
}

static void pw_tick_fluid_immediate(void* ctx, struct java_random* feature_rng,
									int x, int y, int z, uint8_t type) {
	struct pworld* p = ctx;
	assert(feature_rng != NULL);
	assert(feature_rng == p->expected_feature_rng);
	assert(type == B_LAVA_FLOW);
	assert(pw_get(ctx, x, y, z) == type); /* placement precedes updateTick */
	p->immediate_fluid_ticks++;
}

// Pre-fill: a JAGGED netherrack floor + ceiling with a deterministic per-column
// height, so "air over netherrack" (fire/mushroom seats) exists across y in
// ~[41,72] and "air under netherrack" (glowstone seats) across ~[95,126]. A flat
// single-layer interface would leave the features' random probes almost nothing
// to hit; the varied surface guarantees decoration is placed.
static void pw_prefill(struct pworld* p) {
	memset(p, 0, sizeof(*p));
	for(int x = 0; x < PW_W; x++)
		for(int z = 0; z < PW_W; z++) {
			int floor_h = 40 + ((x * 31 + z * 17) & 31); // 40..71
			int ceil_h = 96 + ((x * 13 + z * 29) & 31);	 // 96..127
			for(int y = 0; y < PW_H; y++)
				p->buf[pw_idx(x, y, z)]
					= (y <= floor_h || y >= ceil_h) ? B_NETHERRACK : B_AIR;
		}
}

static void test_populate(void) {
	struct pworld* orig = malloc(sizeof(*orig));
	struct pworld* a = malloc(sizeof(*a));
	struct pworld* b = malloc(sizeof(*b));
	assert(orig && a && b);
	pw_prefill(orig);

	struct wgen_world w = {0};
	w.get = pw_get;
	w.set = pw_set;
	w.tick_fluid_immediate = pw_tick_fluid_immediate;

	// Run 1.
	memcpy(a, orig, sizeof(*orig));
	w.ctx = a;
	struct java_random r1;
	jrand_set_seed(&r1, 12345);
	a->expected_feature_rng = &r1;
	worldgen_hell_populate(&r1, 0, 0, &w);

	// Run 2 with the SAME RNG seed must be byte-identical (determinism given RNG).
	memcpy(b, orig, sizeof(*orig));
	w.ctx = b;
	struct java_random r2;
	jrand_set_seed(&r2, 12345);
	b->expected_feature_rng = &r2;
	worldgen_hell_populate(&r2, 0, 0, &w);
	assert(memcmp(a->buf, b->buf, sizeof(a->buf)) == 0);
	assert(a->immediate_fluid_ticks == b->immediate_fluid_ticks);
	assert(a->immediate_fluid_ticks > 0);

	// Something was decorated (negative control: not a vacuous pass).
	assert(memcmp(a->buf, orig->buf, sizeof(a->buf)) != 0);

	// Validity: every changed cell is a decoration block, never a crafted-only
	// block; every placed FIRE sits directly on netherrack (Beta WorldGenFire).
	size_t changed = 0;
	for(int x = 0; x < PW_W; x++)
		for(int z = 0; z < PW_W; z++)
			for(int y = 0; y < PW_H; y++) {
				uint8_t before = orig->buf[pw_idx(x, y, z)];
				uint8_t after = a->buf[pw_idx(x, y, z)];
				if(before == after)
					continue;
				changed++;
				assert(after == B_FIRE || after == B_GLOWSTONE
					   || after == B_LAVA_FLOW || after == B_BROWN_MUSHROOM
					   || after == B_RED_MUSHROOM);
				assert(after != B_BRICKS && after != B_PLANKS);
				if(after == B_FIRE)
					assert(pw_get(a, x, y - 1, z) == B_NETHERRACK);
			}
	assert(changed > 0);

	// Golden change-detector for the populated buffer (fixed RNG seed 12345).
	uint64_t g = fnv1a(a->buf, sizeof(a->buf));
	printf("test_worldgen_hell: populate golden (seed 12345)=0x%016llx "
		   "rng=0x%012llx changed=%zu\n",
		   (unsigned long long)g, (unsigned long long)r1.seed, changed);
	fflush(stdout);
	assert(GOLDEN_POPULATE == 0x0ULL || g == GOLDEN_POPULATE);
	/* WorldGenHellLava passes this same feature RNG to the DIRECT source tick.
	   A raw-meta-0 source cannot enter BlockFlowing's nextInt(4) decay gate, so
	   the direct tick consumes zero feature draws; recursively immediate child
	   ticks use World.rand instead. test_fluid_spread Q pins the conditional
	   draw gate, while this final state pins every downstream decoration draw. */
	assert(GOLDEN_POPULATE_RNG_STATE == 0x0ULL
		   || r1.seed == GOLDEN_POPULATE_RNG_STATE);

	free(orig);
	free(a);
	free(b);
}

/* CFR BlockMushroom.java:53-55 reads STORED light, not a flood inferred
 * from emitter IDs. Original-JAR full-domain generation witnesses include
 * mushrooms next to untouched dark lava before the deferred drain. */
static void test_mushroom_light_gate_unit(void) {
    struct pworld* p = calloc(1,sizeof(*p));
    assert(p);
    struct wgen_world w = {.ctx=p,.get=pw_get,.set=pw_set,.full_light=pw_full_light};
    p->light_x=24; p->light_y=64; p->light_z=24;
    const uint8_t sources[] = {B_LAVA_FLOW,B_LAVA_STILL,B_FIRE,B_GLOWSTONE};
    for(size_t i=0;i<sizeof sources;i++) {
        p->buf[pw_idx(25,64,24)]=sources[i];
        assert(!worldgen_hell_mushroom_light_gate_blocks(&w,24,64,24));
    }
    p->stored_light=12;
    assert(!worldgen_hell_mushroom_light_gate_blocks(&w,24,64,24));
    p->stored_light=13;
    assert(worldgen_hell_mushroom_light_gate_blocks(&w,24,64,24));
    p->stored_light=15;
    assert(worldgen_hell_mushroom_light_gate_blocks(&w,24,64,24));
    free(p);
}

// End-to-end: a manually-placed light source changes what
// worldgen_hell_populate actually WRITES, for a real seed/fixture -- not just
// the isolated unit test above. Seed 12345 + pw_prefill's floor/ceiling
// deterministically place a red mushroom at (21,62,10) with no rejection
// (test_populate's unchanged GOLDEN_POPULATE digest already proves no fire/
// lava/glowstone happens to land near it for this exact seed). Pre-seeding a
// glowstone one cell away (+X, hop 1) before populate runs must suppress
// exactly that placement and nothing else -- the light check never draws
// randomness (see hell_lit_13_or_more's comment), so the RNG stream and every
// OTHER decoration in the run stay identical.
static void test_populate_mushroom_light_gate_e2e(void) {
	struct pworld* orig = malloc(sizeof(*orig));
	struct pworld* control = malloc(sizeof(*control));
	struct pworld* lit = malloc(sizeof(*lit));
	assert(orig && control && lit);
	pw_prefill(orig);

	struct wgen_world w = {0};
	w.get = pw_get;
	w.set = pw_set;
	w.tick_fluid_immediate = pw_tick_fluid_immediate;
	w.full_light = pw_full_light;

	// Control: unmodified prefill.
	memcpy(control, orig, sizeof(*orig));
	w.ctx = control;
	struct java_random rc;
	jrand_set_seed(&rc, 12345);
	control->expected_feature_rng = &rc;
	worldgen_hell_populate(&rc, 0, 0, &w);
	assert(pw_get(control, 21, 62, 10) == B_RED_MUSHROOM);

	// Treatment: same seed and prefill, but a glowstone sits at (22,62,10)
	// (hop 1 from the mushroom candidate) before populate runs.
	memcpy(lit, orig, sizeof(*orig));
	lit->buf[pw_idx(22, 62, 10)] = B_GLOWSTONE;
    /* Explicit stored light fixture; the raw source alone remains unlit. */
    lit->light_x=21; lit->light_y=62; lit->light_z=10; lit->stored_light=14;
	w.ctx = lit;
	struct java_random rl;
	jrand_set_seed(&rl, 12345);
	lit->expected_feature_rng = &rl;
	worldgen_hell_populate(&rl, 0, 0, &w);
	assert(pw_get(lit, 21, 62, 10) == B_AIR);		 // rejected, not overwritten
	assert(pw_get(lit, 22, 62, 10) == B_GLOWSTONE); // the seed cell, untouched

	// The RNG draw stream is identical either way.
	assert(rc.seed == rl.seed);

	// Every other decoration in the run is unaffected: the two buffers must
	// be identical outside the mushroom cell and the deliberately-seeded
	// glowstone cell.
	size_t diffs = 0;
	for(int x = 0; x < PW_W; x++)
		for(int z = 0; z < PW_W; z++)
			for(int y = 0; y < PW_H; y++) {
				if(control->buf[pw_idx(x, y, z)] == lit->buf[pw_idx(x, y, z)])
					continue;
				diffs++;
				bool is_mush_cell = (x == 21 && y == 62 && z == 10);
				bool is_seed_cell = (x == 22 && y == 62 && z == 10);
				assert(is_mush_cell || is_seed_cell);
			}
	assert(diffs == 2); // exactly the mushroom cell + the seeded glowstone cell

	free(orig);
	free(control);
	free(lit);
}

int main(void) {
	test_terrain();
	test_nether_dimension_lighting();
	test_populate();
	test_mushroom_light_gate_unit();
	test_populate_mushroom_light_gate_e2e();
	printf("test_worldgen_hell: all assertions passed\n");
	return 0;
}
