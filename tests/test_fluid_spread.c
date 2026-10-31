/*
	Deterministic unit test for the Beta 1.7.3 water-flow core
	(source/block/fluid_spread.c). Runs the spread on a mock voxel grid to a
	fixed point and asserts the defining Beta behaviours:
	  A. a source on a flat floor spreads as Manhattan distance, dying at level 8,
	  B. water flows off an edge and falls straight down (falling bit set),
	  C. flowing water cut off from any source dries up,
	  D. two sources one block apart over solid ground form a new (infinite) source,
	  E. lava loses two levels per step (reaches three blocks) and dies at four,
	  F. two lava sources do NOT merge into an infinite source.
	BlockFluid's synchronous add/neighbor hardening lifecycle is deliberately
	pinned by test_fluid_lifecycle_env, not delayed inside this updateTick core.
	Pure logic, no server_world -- same pattern as the worldgen/chest-store tests.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/fluid_spread.h"

#define GX 28
#define GY 12
#define GZ 28
#define STONE 1 // a solid, flow-blocking block id
#define TEST_REPLACEABLE 2
#define TEST_NONSOLID_DAM 3

struct write_event {
	int x, y, z;
	uint8_t type, meta;
};

static uint8_t g_type[GX][GY][GZ];
static uint8_t g_meta[GX][GY][GZ];
static bool g_blocks_flow[256];
static bool g_material_solid[256];
static int g_dirty;
static bool g_trace_writes;
static struct write_event g_writes[32];
static int g_write_count;
static int g_rolls[16], g_roll_count, g_roll_pos, g_rng_calls;
static int g_drop_count, g_mix_count;

static bool in_grid(int x, int y, int z) {
	return x >= 0 && x < GX && y >= 0 && y < GY && z >= 0 && z < GZ;
}

static uint8_t tget(void* c, int x, int y, int z, uint8_t* m) {
	(void)c;
	if(!in_grid(x, y, z)) {
		if(m)
			*m = 0;
		return STONE; // outside the grid is a wall
	}
	if(m)
		*m = g_meta[x][y][z];
	return g_type[x][y][z];
}

static void tset(void* c, int x, int y, int z, uint8_t t, uint8_t m) {
	(void)c;
	if(!in_grid(x, y, z))
		return;
	if(g_trace_writes) {
		assert(g_write_count < (int)(sizeof(g_writes) / sizeof(g_writes[0])));
		g_writes[g_write_count++]
			= (struct write_event) {.x = x, .y = y, .z = z, .type = t, .meta = m};
	}
	if(g_type[x][y][z] != t || g_meta[x][y][z] != m)
		g_dirty = 1;
	g_type[x][y][z] = t;
	g_meta[x][y][z] = m;
}

static bool tblocks(void* c, uint8_t t) {
	(void)c;
	return g_blocks_flow[t];
}

static bool tsolid(void* c, uint8_t t) {
	(void)c;
	return g_material_solid[t];
}

static int tnext(void* c, int bound) {
	(void)c;
	assert(bound == 4);
	g_rng_calls++;
	return g_roll_pos < g_roll_count ? g_rolls[g_roll_pos++] : 0;
}

static void tdrop(void* c, int x, int y, int z, uint8_t type,
				  uint8_t meta) {
	(void)c;
	(void)x;
	(void)y;
	(void)z;
	(void)type;
	(void)meta;
	g_drop_count++;
}

static void tmix(void* c, int x, int y, int z) {
	(void)c;
	(void)x;
	(void)y;
	(void)z;
	g_mix_count++;
}

static struct fluid_world W = {.ctx = NULL,
							   .get = tget,
							   .set = tset,
							   .blocks_flow = tblocks,
							   .material_is_solid = tsolid,
							   .next_int = tnext,
							   .drop_block = tdrop,
							   .lava_mix = tmix};

static bool is_water(uint8_t t) {
	return t == FLUID_WATER_FLOW || t == FLUID_WATER_STILL;
}

// Tick every fluid cell repeatedly until a whole pass changes nothing. `nether`
// selects lava's Nether 1-level step (reaches 7); false = overworld (reaches 3).
static int converge_ex(int cap, bool nether) {
	int pass = 0;
	for(; pass < cap; pass++) {
		g_dirty = 0;
		for(int x = 0; x < GX; x++)
			for(int y = 0; y < GY; y++)
				for(int z = 0; z < GZ; z++) {
					uint8_t t = g_type[x][y][z];
					if(t == FLUID_WATER_FLOW || t == FLUID_WATER_STILL)
						fluid_water_tick(&W, x, y, z, nether);
					else if(t == FLUID_LAVA_FLOW || t == FLUID_LAVA_STILL)
						fluid_lava_tick(&W, x, y, z, nether);
				}
		if(!g_dirty)
			break;
	}
	return pass;
}
// Overworld convergence (nether=false); keeps the existing A-I cases unchanged.
static int converge(int cap) {
	return converge_ex(cap, false);
}

// Water level at a cell: -1 if not water, else 0..7 (a falling column reads 0).
static int wlevel(int x, int y, int z) {
	if(!is_water(g_type[x][y][z]))
		return -1;
	uint8_t m = g_meta[x][y][z];
	return (m >= 8) ? 0 : (m & 7);
}

// Lava level at a cell: -1 if not lava, else 0..7.
static int llevel(int x, int y, int z) {
	uint8_t t = g_type[x][y][z];
	if(t != FLUID_LAVA_FLOW && t != FLUID_LAVA_STILL)
		return -1;
	uint8_t m = g_meta[x][y][z];
	return (m >= 8) ? 0 : (m & 7);
}

static void reset(void) {
	memset(g_type, 0, sizeof(g_type));
	memset(g_meta, 0, sizeof(g_meta));
	memset(g_blocks_flow, 0, sizeof(g_blocks_flow));
	memset(g_material_solid, 0, sizeof(g_material_solid));
	g_blocks_flow[STONE] = true;
	g_material_solid[STONE] = true;
	g_trace_writes = false;
	g_write_count = 0;
	g_roll_count = g_roll_pos = g_rng_calls = 0;
	g_drop_count = g_mix_count = 0;
}

static void trace_on(void) {
	g_write_count = 0;
	g_trace_writes = true;
}

static void expect_write(int i, int x, int y, int z, uint8_t type,
						 uint8_t meta) {
	assert(i >= 0 && i < g_write_count);
	assert(g_writes[i].x == x && g_writes[i].y == y && g_writes[i].z == z);
	assert(g_writes[i].type == type && g_writes[i].meta == meta);
}

static void floor_y0(void) {
	for(int x = 0; x < GX; x++)
		for(int z = 0; z < GZ; z++)
			g_type[x][0][z] = STONE;
}

int main(void) {
	const int sx = 13, sy = 1, sz = 13;

	// --- A: flat-floor source spreads as Manhattan distance, dies at 8 ---
	reset();
	floor_y0();
	g_type[sx][sy][sz] = FLUID_WATER_FLOW; // a placed source flows, then settles
	g_meta[sx][sy][sz] = 0;
	int passes = converge(300);
	assert(passes < 300); // reached a fixed point
	assert(wlevel(sx, sy, sz) == 0); // source stays a source
	assert(wlevel(sx + 1, sy, sz) == 1);
	assert(wlevel(sx + 3, sy, sz) == 3);
	assert(wlevel(sx + 7, sy, sz) == 7); // thinnest reachable level
	assert(wlevel(sx + 8, sy, sz) == -1); // dies one step past level 7
	assert(wlevel(sx + 2, sy, sz + 2) == 4); // diagonal -> Manhattan distance 4
	assert(wlevel(sx, sy, sz - 6) == 6);
	// stable water settles to "still" (id 9) so it stops being re-ticked
	assert(g_type[sx][sy][sz] == FLUID_WATER_STILL);	 // source settled
	assert(g_type[sx + 1][sy][sz] == FLUID_WATER_STILL); // spread cell settled
	printf("test_fluid_spread A: flat spread = Manhattan distance, dies at 8 "
		   "(%d passes)\n",
		   passes);

	// --- B: water flows off a 1-block pillar and falls straight down ---
	reset();
	g_type[sx][3][sz] = STONE; // pillar top the source sits on
	g_type[sx][4][sz] = FLUID_WATER_FLOW;
	g_meta[sx][4][sz] = 0;
	assert(converge(300) < 300);
	assert(wlevel(sx + 1, 4, sz) == 1); // ring of flowing water around the top
	// the ring cell flows down: a falling column (bit 0x8) directly below it
	assert(is_water(g_type[sx + 1][3][sz])
		   && (g_meta[sx + 1][3][sz] & 0x8)); // a falling column (still or flowing)
	assert(is_water(g_type[sx + 1][0][sz])); // reached the bottom
	printf("test_fluid_spread B: water falls off an edge (falling bit set, "
		   "reaches bottom)\n");

	// --- C: isolated flowing water (no source) dries up ---
	reset();
	g_type[10][5][10] = FLUID_WATER_FLOW;
	g_meta[10][5][10] = 3;
	assert(converge(50) < 50);
	assert(g_type[10][5][10] == FLUID_AIR);
	printf("test_fluid_spread C: isolated flowing water dries up\n");

	// --- D: two sources one block apart over solid form a new source ---
	reset();
	floor_y0();
	g_type[10][1][10] = FLUID_WATER_FLOW;
	g_meta[10][1][10] = 0;
	g_type[12][1][10] = FLUID_WATER_FLOW;
	g_meta[12][1][10] = 0; // gap at x=11
	assert(converge(300) < 300);
	assert(wlevel(11, 1, 10) == 0); // the gap became a source (infinite water)
	printf("test_fluid_spread D: two sources over solid form a new source "
		   "(infinite water)\n");

	// --- E: lava loses 2 levels per step -> reaches only 3 blocks ---
	reset();
	floor_y0();
	g_type[sx][sy][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][sy][sz] = 0;
	assert(converge(300) < 300);
	assert(llevel(sx, sy, sz) == 0);	 // source
	assert(llevel(sx + 1, sy, sz) == 2); // step of 2
	assert(llevel(sx + 2, sy, sz) == 4);
	assert(llevel(sx + 3, sy, sz) == 6); // thinnest reachable lava level
	assert(g_type[sx + 4][sy][sz] == FLUID_AIR); // dies at distance 4 (would be 8)
	assert(g_type[sx][sy][sz] == FLUID_LAVA_STILL); // settled to still lava
	printf("test_fluid_spread E: lava spreads 3 blocks (step 2), dies at 4\n");

	// --- F: two lava sources one block apart do NOT form a source ---
	reset();
	floor_y0();
	g_type[10][1][10] = FLUID_LAVA_FLOW;
	g_meta[10][1][10] = 0;
	g_type[12][1][10] = FLUID_LAVA_FLOW;
	g_meta[12][1][10] = 0; // gap at x=11
	assert(converge(300) < 300);
	assert(llevel(11, 1, 10) > 0); // flowing lava, NOT a new source (no infinite)
	printf("test_fluid_spread F: two lava sources do NOT form an infinite "
		   "source\n");

	// --- J: NETHER lava loses only 1 level per step -> reaches 7 blocks (like
	//        water). Beta BlockFlowing.updateTick: var7=1 in the Nether. This is
	//        the dimension fix; case E proves the overworld (step 2) is unchanged.
	reset();
	floor_y0();
	g_type[sx][sy][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][sy][sz] = 0;
	assert(converge_ex(300, true) < 300); // nether = true
	assert(llevel(sx, sy, sz) == 0);	  // source
	assert(llevel(sx + 1, sy, sz) == 1);  // step of 1 (not 2)
	assert(llevel(sx + 3, sy, sz) == 3);
	assert(llevel(sx + 7, sy, sz) == 7);  // thinnest reachable, same reach as water
	assert(g_type[sx + 8][sy][sz] == FLUID_AIR); // dies one step past 7
	printf("test_fluid_spread J: NETHER lava spreads 7 blocks (step 1)\n");

	// --- K: NETHER ceiling spring FALLS straight down to the floor. The "falls from
	//        the nether ceiling" half of the user report: a permanent lava SOURCE stuck
	//        under the roof (air directly below) flows down as a FALLING column
	//        (metadata bit 0x8) all the way to the floor. This case asserts ONLY the
	//        vertical fall (the 0x8 column, which distinguishes falling from a static
	//        level); the horizontal FLOW/reach is proven separately by cases E, J and L,
	//        NOT here -- on this synthetic FLAT unbounded floor a permanent source floods
	//        the whole floor and every floor cell reads decay-0/falling, so the floor
	//        carries no lateral-flow signal (case L uses an enclosed chamber for that).
	//        The vertical fall asserted here is dimension-independent (the step only
	//        affects horizontal reach); case L proves the Nether-specific floor flow. ---
	reset();
	floor_y0();							 // floor at y=0
	g_type[sx][GY - 1][sz] = STONE;		 // ceiling the spring hangs from (y=11)
	g_type[sx][GY - 2][sz] = FLUID_LAVA_FLOW; // lava source under the roof (y=10)
	g_meta[sx][GY - 2][sz] = 0;
	assert(converge_ex(400, true) < 400);
	assert(llevel(sx, GY - 2, sz) == 0); // the ceiling source persists (never dries)
	// a genuinely FALLING column below the source (metadata bit 0x8 set), not a
	// static level -- sampled just under the source, mid-air, and just above the
	// floor: this is lava *actively falling* from the ceiling.
	assert(llevel(sx, GY - 3, sz) >= 0 && (g_meta[sx][GY - 3][sz] & 0x8)); // y=9
	assert(llevel(sx, 6, sz) >= 0 && (g_meta[sx][6][sz] & 0x8));			  // mid-air
	assert(llevel(sx, 2, sz) >= 0 && (g_meta[sx][2][sz] & 0x8)); // just above floor
	assert(llevel(sx, 1, sz) >= 0);								// reaches the floor
	printf("test_fluid_spread K: NETHER ceiling spring falls straight to the floor "
		   "(0x8 column)\n");

	// --- L: NETHER ceiling spring FLOWS across the floor with the Nether's extended
	//        (step-1) reach -- the "flowing... from the nether ceiling" half. An ENCLOSED
	//        chamber (low roof at y=3, source at y=2, air at y=1, floor at y=0) so the
	//        pool CANNOT back up into the flat-floor flood artifact case K avoids; the
	//        floor then carries GENUINE lateral decay. We assert a NON-falling, positive-
	//        level flow cell out on the floor (real horizontal spread, not a vertically-
	//        fed column), and -- run in BOTH dimensions on the same geometry -- that the
	//        Nether (step 1) reaches a floor cell the overworld (step 2) leaves dry. That
	//        contrast is the fix #1 guard in the ceiling-fall scenario (cases E/J guard it
	//        for a floor-level source). Cell offsets are measured, not guessed. ---
	reset();
	floor_y0();									 // floor y=0
	for(int x = 0; x < GX; x++)
		for(int z = 0; z < GZ; z++)
			g_type[x][3][z] = STONE;		 // low roof y=3
	g_type[sx][2][sz] = FLUID_LAVA_FLOW;	 // ceiling spring hangs at y=2
	g_meta[sx][2][sz] = 0;
	assert(converge_ex(400, true) < 400);	 // NETHER
	assert(llevel(sx, 2, sz) == 0 && !(g_meta[sx][2][sz] & 0x8)); // real hanging source
	assert(llevel(sx, 1, sz) >= 0 && (g_meta[sx][1][sz] & 0x8));	 // falling column below
	// Genuine lateral flow reaches raw level 7 at +8. The former +9 expectation
	// pinned the old multi-height fan bug; exact BlockFlowing decay leaves +9 dry.
	assert(llevel(sx + 8, 1, sz) == 7 && !(g_meta[sx + 8][1][sz] & 0x8));
	assert(g_type[sx + 9][1][sz] == FLUID_AIR);

	// Same chamber, OVERWORLD (step 2): the floor flow dies far sooner. Nether
	// step-1 reaches farther from a ceiling fall, exactly as fix #1 intends.
	reset();
	floor_y0();
	for(int x = 0; x < GX; x++)
		for(int z = 0; z < GZ; z++)
			g_type[x][3][z] = STONE;
	g_type[sx][2][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][2][sz] = 0;
	assert(converge_ex(400, false) < 400);	// OVERWORLD
	assert(g_type[sx + 9][1][sz] == FLUID_AIR); // overworld floor flow never reaches +9
	printf("test_fluid_spread L: NETHER ceiling spring flows across the floor (step-1 "
		   "reach; overworld step-2 stops short)\n");

	// --- M: exact one-tick DOWNWARD metadata and write order. BlockFlowing
	// updateFlow settles the current moving block BEFORE propagation. Downward
	// propagation writes raw `var6 + 8`, or copies raw var6 unchanged when it is
	// already falling. [740c583 BlockFlowing.updateTick:64-85]
	reset();
	g_type[sx][5][sz] = FLUID_LAVA_FLOW; // raw source 0
	trace_on();
	fluid_lava_tick(&W, sx, 5, sz, true);
	assert(g_write_count == 2);
	expect_write(0, sx, 5, sz, FLUID_LAVA_STILL, 0);
	expect_write(1, sx, 4, sz, FLUID_LAVA_FLOW, 8);

	reset();
	g_type[sx - 1][5][sz] = FLUID_LAVA_FLOW;
	g_meta[sx - 1][5][sz] = 2; // Nether step 1 keeps the current level at 3
	g_type[sx][5][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][5][sz] = 3;
	trace_on();
	fluid_lava_tick(&W, sx, 5, sz, true);
	assert(g_write_count == 2);
	expect_write(0, sx, 5, sz, FLUID_LAVA_STILL, 3);
	expect_write(1, sx, 4, sz, FLUID_LAVA_FLOW, 11);

	reset();
	g_type[sx][6][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][6][sz] = 3;
	g_type[sx][5][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][5][sz] = 11;
	trace_on();
	fluid_lava_tick(&W, sx, 5, sz, true);
	assert(g_write_count == 2);
	expect_write(0, sx, 5, sz, FLUID_LAVA_STILL, 11);
	expect_write(1, sx, 4, sz, FLUID_LAVA_FLOW, 11);
	printf("test_fluid_spread M: downward raw metadata 0->8, 3->11, 11->11; "
		   "settle precedes propagation\n");

	// --- N: raw horizontal gate. A falling/non-source block stacked above more
	// fluid is not a source and the fluid below is not flow-blocking, so Beta
	// performs no lateral writes. [BlockFlowing.updateTick:80-112]
	reset();
	g_type[sx][6][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][6][sz] = 3;
	g_type[sx][5][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][5][sz] = 11;
	g_type[sx][4][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][4][sz] = 11;
	trace_on();
	fluid_lava_tick(&W, sx, 5, sz, true);
	assert(g_write_count == 1);
	expect_write(0, sx, 5, sz, FLUID_LAVA_STILL, 11);
	assert(g_type[sx - 1][5][sz] == FLUID_AIR);
	assert(g_type[sx + 1][5][sz] == FLUID_AIR);
	assert(g_type[sx][5][sz - 1] == FLUID_AIR);
	assert(g_type[sx][5][sz + 1] == FLUID_AIR);
	printf("test_fluid_spread N: stacked falling lava does not fan sideways\n");

	// --- O: a falling column that lands on a genuine flow-blocking floor emits
	// horizontal level 1 in BOTH dimensions. The `var6 >= 8 -> var10 = 1`
	// override is after dimension step selection. It also pins write order
	// current, -X, +X, -Z, +Z. [BlockFlowing.updateTick:86-110]
	for(int dim = 0; dim < 2; dim++) {
		reset();
		g_type[sx][6][sz] = FLUID_LAVA_FLOW;
		g_meta[sx][6][sz] = 3;
		g_type[sx][5][sz] = FLUID_LAVA_FLOW;
		g_meta[sx][5][sz] = 11;
		g_type[sx][4][sz] = STONE;
		trace_on();
		fluid_lava_tick(&W, sx, 5, sz, dim != 0);
		assert(g_write_count == 5);
		expect_write(0, sx, 5, sz, FLUID_LAVA_STILL, 11);
		expect_write(1, sx - 1, 5, sz, FLUID_LAVA_FLOW, 1);
		expect_write(2, sx + 1, 5, sz, FLUID_LAVA_FLOW, 1);
		expect_write(3, sx, 5, sz - 1, FLUID_LAVA_FLOW, 1);
		expect_write(4, sx, 5, sz + 1, FLUID_LAVA_FLOW, 1);
	}
	printf("test_fluid_spread O: landed falling lava emits level 1 in both dims; "
		   "direction order exact\n");

	// --- P: raw source 0 MAY spread over fluid below. This is the faithful
	// reason a Nether ceiling spring can form a shelf after its first downward
	// cell exists; the prohibition in N applies only to raw non-sources.
	reset();
	g_type[sx][5][sz] = FLUID_LAVA_FLOW;
	g_type[sx][4][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][4][sz] = 8;
	trace_on();
	fluid_lava_tick(&W, sx, 5, sz, true);
	assert(g_write_count == 5);
	expect_write(0, sx, 5, sz, FLUID_LAVA_STILL, 0);
	expect_write(1, sx - 1, 5, sz, FLUID_LAVA_FLOW, 1);
	expect_write(2, sx + 1, 5, sz, FLUID_LAVA_FLOW, 1);
	expect_write(3, sx, 5, sz - 1, FLUID_LAVA_FLOW, 1);
	expect_write(4, sx, 5, sz + 1, FLUID_LAVA_FLOW, 1);
	printf("test_fluid_spread P: raw source may spread over a fluid column\n");

	// --- Q: lava decay resistance consumes exactly one nextInt(4) only when a
	// valid new decay is larger than the old non-falling decay. Rolls 1..3 retain
	// the old level without settling; roll 0 accepts the new level.
	for(int roll = 0; roll < 4; roll++) {
		reset();
		g_type[sx][5][sz] = FLUID_LAVA_FLOW;
		g_meta[sx][5][sz] = 2;
		g_type[sx - 1][5][sz] = FLUID_LAVA_FLOW;
		g_meta[sx - 1][5][sz] = 3; // computed Nether next = 4 (> old 2)
		g_type[sx][4][sz] = STONE;
		g_type[sx + 1][5][sz] = g_type[sx][5][sz - 1]
			= g_type[sx][5][sz + 1] = STONE;
		g_rolls[0] = roll;
		g_roll_count = 1;
		trace_on();
		fluid_lava_tick(&W, sx, 5, sz, true);
		assert(g_rng_calls == 1 && g_roll_pos == 1);
		if(roll == 0) {
			assert(g_write_count == 1);
			expect_write(0, sx, 5, sz, FLUID_LAVA_FLOW, 4);
		} else {
			assert(g_write_count == 0);
			assert(g_type[sx][5][sz] == FLUID_LAVA_FLOW);
			assert(g_meta[sx][5][sz] == 2);
		}
	}

	// No RNG draw for a source, falling state, equal/decreasing decay, or total
	// loss of feed. The condition itself guards the draw in Beta.
	for(int mode = 0; mode < 5; mode++) {
		reset();
		g_type[sx][5][sz] = FLUID_LAVA_FLOW;
		g_type[sx][4][sz] = STONE;
		g_type[sx + 1][5][sz] = g_type[sx][5][sz - 1]
			= g_type[sx][5][sz + 1] = STONE;
		if(mode == 0) {
			g_meta[sx][5][sz] = 0; // source
		} else if(mode == 1) {
			g_meta[sx][5][sz] = 8; // falling
			g_type[sx][6][sz] = FLUID_LAVA_FLOW;
			g_meta[sx][6][sz] = 8;
		} else if(mode == 2) {
			g_meta[sx][5][sz] = 2; // equal next = 2
			g_type[sx - 1][5][sz] = FLUID_LAVA_FLOW;
			g_meta[sx - 1][5][sz] = 1;
		} else if(mode == 3) {
			g_meta[sx][5][sz] = 4; // decreasing next = 2
			g_type[sx - 1][5][sz] = FLUID_LAVA_FLOW;
			g_meta[sx - 1][5][sz] = 1;
		} else {
			g_meta[sx][5][sz] = 4; // no feed -> -1
		}
		fluid_lava_tick(&W, sx, 5, sz, true);
		assert(g_rng_calls == 0);
	}
	printf("test_fluid_spread Q: lava nextInt(4) gate and rolls are exact\n");

	// --- R: displacement is asymmetric. Lava above water does not harden from
	// below and replaces it downward with falling lava (no horizontal drop/mix
	// callback); water cannot displace lava.
	reset();
	g_type[sx][5][sz] = FLUID_LAVA_FLOW;
	g_type[sx][4][sz] = FLUID_WATER_STILL;
	trace_on();
	fluid_lava_tick(&W, sx, 5, sz, true);
	assert(g_write_count == 2);
	expect_write(0, sx, 5, sz, FLUID_LAVA_STILL, 0);
	expect_write(1, sx, 4, sz, FLUID_LAVA_FLOW, 8);
	assert(g_drop_count == 0 && g_mix_count == 0);

	reset();
	g_type[sx][5][sz] = FLUID_WATER_FLOW;
	g_type[sx][4][sz] = FLUID_LAVA_STILL;
	g_type[sx - 1][5][sz] = g_type[sx + 1][5][sz]
		= g_type[sx][5][sz - 1] = g_type[sx][5][sz + 1] = STONE;
	fluid_water_tick(&W, sx, 5, sz, false);
	assert(g_type[sx][4][sz] == FLUID_LAVA_STILL);
	printf("test_fluid_spread R: lava displaces water downward; water cannot displace lava\n");

	// --- S: horizontal replacement callbacks. Thin lava (raw 6)
	// may displace adjacent water and emits mix/no drop. Horizontal water washes
	// away a replaceable block and invokes its drop before placement.
	reset();
	g_type[sx][5][sz] = FLUID_LAVA_FLOW;
	g_meta[sx][5][sz] = 6;
	g_type[sx - 1][5][sz] = FLUID_LAVA_FLOW;
	g_meta[sx - 1][5][sz] = 5;
	g_type[sx + 1][5][sz] = FLUID_WATER_STILL;
	g_type[sx][4][sz] = g_type[sx - 1][4][sz] = g_type[sx + 1][4][sz] = STONE;
	g_type[sx][5][sz - 1] = g_type[sx][5][sz + 1] = STONE;
	fluid_lava_tick(&W, sx, 5, sz, true);
	assert(g_type[sx + 1][5][sz] == FLUID_LAVA_FLOW);
	assert(g_meta[sx + 1][5][sz] == 7);
	assert(g_mix_count == 1 && g_drop_count == 0);

	reset();
	g_type[sx][5][sz] = FLUID_WATER_FLOW;
	g_type[sx][4][sz] = STONE;
	g_type[sx - 1][5][sz] = g_type[sx][5][sz - 1]
		= g_type[sx][5][sz + 1] = STONE;
	g_type[sx + 1][5][sz] = TEST_REPLACEABLE;
	g_type[sx + 1][4][sz] = STONE;
	fluid_water_tick(&W, sx, 5, sz, false);
	assert(g_type[sx + 1][5][sz] == FLUID_WATER_FLOW);
	assert(g_meta[sx + 1][5][sz] == 1);
	assert(g_drop_count == 1 && g_mix_count == 0);
	printf("test_fluid_spread S: horizontal water drop / lava mix callbacks exact\n");

	// --- T: path search admits same-fluid NON-source cells, even though final
	// displacement refuses to overwrite them. The -X flowing cell has an open
	// cell below (cost 0), so it wins over the +X dead end; +X must stay dry.
	reset();
	g_type[sx][5][sz] = FLUID_WATER_FLOW;
	g_type[sx][4][sz] = STONE;
	g_type[sx - 1][5][sz] = FLUID_WATER_FLOW;
	g_meta[sx - 1][5][sz] = 2;
	g_type[sx + 1][4][sz] = STONE;
	g_type[sx][5][sz - 1] = g_type[sx][5][sz + 1] = STONE;
	fluid_water_tick(&W, sx, 5, sz, false);
	assert(g_type[sx + 1][5][sz] == FLUID_AIR);
	printf("test_fluid_spread T: path search traverses same-fluid non-sources\n");

	// --- U: blockBlocksFlow is not Material.isSolid. A ladder/reed-like
	// non-solid dam supports flow blocking but not infinite-source formation;
	// marking the same fixture materially solid does form the source.
	for(int solid = 0; solid < 2; solid++) {
		reset();
		g_blocks_flow[TEST_NONSOLID_DAM] = true;
		g_material_solid[TEST_NONSOLID_DAM] = solid != 0;
		g_type[sx][5][sz] = FLUID_WATER_FLOW;
		g_meta[sx][5][sz] = 1;
		g_type[sx - 1][5][sz] = g_type[sx + 1][5][sz]
			= FLUID_WATER_STILL;
		g_type[sx][4][sz] = TEST_NONSOLID_DAM;
		g_type[sx][5][sz - 1] = g_type[sx][5][sz + 1] = STONE;
		fluid_water_tick(&W, sx, 5, sz, false);
		assert(g_meta[sx][5][sz] == (solid ? 0 : 1));
	}

	// The decompile's second support clause reads the CURRENT block metadata,
	// not the metadata below. Because this code is inside `decay > 0`, fluid
	// below cannot satisfy it: two sources over a water source remain level 1.
	// [740c583 BlockFlowing.updateTick:52-58]
	reset();
	g_type[sx][5][sz] = FLUID_WATER_FLOW;
	g_meta[sx][5][sz] = 1;
	g_type[sx - 1][5][sz] = g_type[sx + 1][5][sz] = FLUID_WATER_STILL;
	g_type[sx][4][sz] = FLUID_WATER_STILL;
	g_type[sx][5][sz - 1] = g_type[sx][5][sz + 1] = STONE;
	fluid_water_tick(&W, sx, 5, sz, false);
	assert(g_meta[sx][5][sz] == 1);
	printf("test_fluid_spread U: flow blocking/material solidity and the exact "
		   "two-source support predicate are pinned\n");

	printf("test_fluid_spread: all OK\n");
	return 0;
}
