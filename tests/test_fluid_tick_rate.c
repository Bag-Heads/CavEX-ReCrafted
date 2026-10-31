/*
	Faithfulness golden: flowing fluids schedule at Beta 1.7.3's BlockFluid.tickRate() (water = 5,
	lava = 30), NOT the queue's default of 1. Before this, block_water_flowing / block_lava_flowing set
	no .tick_delay, so server_world_schedule_block_tick fell back to 1 -- water spread 5x and lava 30x
	too fast. Pins the values on the REAL block structs and drives the REAL scheduled-tick queue so the
	delay is exercised end-to-end (not just a constant re-asserted).

	Beta citation: BlockFluid.tickRate() (1.7.3-LTS) -- `water -> 5; lava ? 30 : 0`. Lava is 30 in all
	dimensions; CaveWii only generates the overworld, so there is no nether special case.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "block/fluid_block.h"
#include "graphics/gfx_settings.h"
#include "graphics/render_block.h"
#include "network/server_world.h"

/*
	RenderBlocks.renderBlockFluids + func_1224_a, Beta 1.7.3 decompile
	740c583 (lines 1484-1663): each top corner averages the four touching
	columns; metadata 0 and >=8 contribute getPercentAir(meta)*10 before their
	normal contribution. BlockFluid.getPercentAir maps >=8 to 0, then returns
	(meta+1)/9. Therefore a flat source OR falling sheet is exactly 8/9 high.

	The production renderer writes PC display-list vertices as packed 22-byte
	records. These checks exercise the real renderer and decode only position/UV;
	they do not duplicate its implementation.
*/
#define RG 9
#define RX 4
#define RY 4
#define RZ 4
#define PC_VERTEX_BYTES 22

static struct block_data render_grid[RG][RG][RG];

static bool render_world_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
							 struct block_data* out) {
	(void)ctx;
	if(x < 0 || x >= RG || y < 0 || y >= RG || z < 0 || z >= RG) {
		*out = (struct block_data) {0};
		return true;
	}
	*out = render_grid[x][y][z];
	return true;
}

static void render_grid_clear(void) {
	memset(render_grid, 0, sizeof render_grid);
}

static void render_plane(uint8_t type, uint8_t meta) {
	render_grid_clear();
	for(int x = RX - 1; x <= RX + 1; x++)
		for(int z = RZ - 1; z <= RZ + 1; z++)
			render_grid[x][RY][z]
				= (struct block_data) {.type = type, .metadata = meta};
}

static struct block_info render_info(void) {
	return (struct block_info) {
		.block = &render_grid[RX][RY][RZ],
		.x = RX,
		.y = RY,
		.z = RZ,
		.get_block = render_world_get,
		.get_block_ctx = NULL,
	};
}

static struct block_info render_neighbour(enum side side) {
	int dx, dy, dz;
	blocks_side_offset(side, &dx, &dy, &dz);
	return (struct block_info) {
		.block = &render_grid[RX + dx][RY + dy][RZ + dz],
		.x = RX + dx,
		.y = RY + dy,
		.z = RZ + dz,
	};
}

static float packed_f32(const struct displaylist* dl, size_t vertex,
						size_t byte_offset) {
	float out;
	memcpy(&out, (const uint8_t*)dl->data + vertex * PC_VERTEX_BYTES + byte_offset,
		   sizeof out);
	return out;
}

static int packed_height(const struct displaylist* dl, size_t vertex) {
	return (int)lroundf((packed_f32(dl, vertex, 4) - (float)RY) * 256.0F);
}

static int packed_uv(const struct displaylist* dl, size_t vertex, bool v) {
	return (int)lroundf(packed_f32(dl, vertex, v ? 18 : 14) * 256.0F);
}

static size_t render_capture(enum side side, struct displaylist* dl) {
	struct block_info self = render_info();
	struct block_info neighbour = render_neighbour(side);
	uint8_t light[24];
	memset(light, 0xFF, sizeof light);
	size_t counted
		= render_block_fluid(NULL, &self, side, &neighbour, light, true);
	displaylist_init(dl, 4, 1);
	size_t emitted
		= render_block_fluid(dl, &self, side, &neighbour, light, false);
	assert(counted == emitted);
	assert(dl->index == emitted * 4 * PC_VERTEX_BYTES);
	return emitted;
}

static void assert_flat_top(uint8_t type, uint8_t meta, int expected_height) {
	render_plane(type, meta);
	struct displaylist dl;
	assert(render_capture(SIDE_TOP, &dl) == 1);
	assert(dl.index == 4 * PC_VERTEX_BYTES);
	for(size_t i = 0; i < 4; i++)
		assert(packed_height(&dl, i) == expected_height);
	displaylist_destroy(&dl);
}

static void test_fluid_render_goldens(void) {
	/* (1 - (0+1)/9) * 256 = 227.555..., nearest display unit = 228. */
	assert_flat_top(BLOCK_WATER_STILL, 0, 228);
	/* BlockFluid.getPercentAir first maps every falling metadata >=8 to 0. */
	assert_flat_top(BLOCK_WATER_FLOW, 8, 228);
	assert_flat_top(BLOCK_LAVA_FLOW, 15, 228);

	/* Moving/still is simulation state, not a texture selector: a zero-vector
	   moving source still uses the stationary top tile (anim cell 1). */
	render_plane(BLOCK_WATER_FLOW, 0);
	struct displaylist dl;
	assert(render_capture(SIDE_TOP, &dl) == 1);
	const int still_u0 = TEX_OFFSET(1), still_v0 = TEX_OFFSET(0);
	const int expected_still_uv[4][2] = {
		{still_u0, still_v0},
		{still_u0 + 16, still_v0},
		{still_u0 + 16, still_v0 + 16},
		{still_u0, still_v0 + 16},
	};
	for(size_t i = 0; i < 4; i++) {
		assert(packed_uv(&dl, i, false) == expected_still_uv[i][0]);
		assert(packed_uv(&dl, i, true) == expected_still_uv[i][1]);
	}
	displaylist_destroy(&dl);

	/* Fluid above the west sampled column makes the two west corners exactly 1. */
	render_plane(BLOCK_WATER_STILL, 0);
	render_grid[RX - 1][RY + 1][RZ]
		= (struct block_data) {.type = BLOCK_WATER_FLOW, .metadata = 8};
	assert(render_capture(SIDE_TOP, &dl) == 1);
	const int expected_overhang_h[4] = {256, 228, 228, 256};
	for(size_t i = 0; i < 4; i++)
		assert(packed_height(&dl, i) == expected_overhang_h[i]);
	displaylist_destroy(&dl);

	/*
		Three fluid columns, constant in Z: west/current decay 1, east decay 7.
		West corners: 1-(2/9)=7/9 -> 199 units. East corners average
		(2/9,2/9,8/9,8/9): 1-5/9=4/9 -> 114 units. The positive-X
		decay delta also gives an eastward flow vector.
	*/
	render_grid_clear();
	for(int x = RX - 1; x <= RX + 1; x++) {
		uint8_t meta = x == RX + 1 ? 7 : 1;
		for(int z = RZ - 1; z <= RZ + 1; z++)
			render_grid[x][RY][z]
				= (struct block_data) {.type = BLOCK_WATER_FLOW, .metadata = meta};
	}
	assert(render_capture(SIDE_TOP, &dl) == 1);
	const int expected_top_h[4] = {199, 114, 114, 199};
	for(size_t i = 0; i < 4; i++)
		assert(packed_height(&dl, i) == expected_top_h[i]);

	/*
		BlockFluid.getFlowDirection: atan2(0,+X)-PI/2 = -PI/2. CaveWii's
		16x16 anim cell is the architectural substitute for Beta's 32x32 flow
		tile; rotate it around its 8-texel centre in the exact Beta corner order.
	*/
	const int flow_u0 = TEX_OFFSET(5), flow_v0 = TEX_OFFSET(0);
	const int expected_uv[4][2] = {
		{flow_u0 + 16, flow_v0},
		{flow_u0 + 16, flow_v0 + 16},
		{flow_u0, flow_v0 + 16},
		{flow_u0, flow_v0},
	};
	for(size_t i = 0; i < 4; i++) {
		assert(packed_uv(&dl, i, false) == expected_uv[i][0]);
		assert(packed_uv(&dl, i, true) == expected_uv[i][1]);
	}
	displaylist_destroy(&dl);

	/* Expose the north face. Its recomputed touching-corner averages are
	   h10=1-(8/9+2/9+8/9+1)/4=1/4 -> 64 and
	   h00=1-(2/9+2/9+1+2/9)/4=7/12 -> 149 display units. */
	render_grid[RX][RY][RZ - 1] = (struct block_data) {0};
	assert(render_capture(SIDE_FRONT, &dl) == 1);
	assert(packed_height(&dl, 0) == 0);
	assert(packed_height(&dl, 1) == 0);
	assert(packed_height(&dl, 2) == 64);
	assert(packed_height(&dl, 3) == 149);
	const int expected_side_u[4] = {flow_u0 + 16, flow_u0, flow_u0, flow_u0 + 16};
	for(size_t i = 0; i < 4; i++)
		assert(packed_uv(&dl, i, false) == expected_side_u[i]);
	displaylist_destroy(&dl);

	/* +X,+Z flow rotates to a diamond. Beta's source texture is 32x32;
	   CaveWii fits that rotation inside its 16x16 anim cell, so no UV may bleed
	   into the neighbouring column. */
	render_plane(BLOCK_WATER_FLOW, 1);
	for(int z = RZ - 1; z <= RZ + 1; z++)
		render_grid[RX + 1][RY][z].metadata = 7;
	for(int x = RX - 1; x <= RX + 1; x++)
		render_grid[x][RY][RZ + 1].metadata = 7;
	assert(render_capture(SIDE_TOP, &dl) == 1);
	const int flow_uc = flow_u0 + 8, flow_vc = flow_v0 + 8;
	const int expected_diagonal_uv[4][2] = {
		{flow_uc, flow_v0},
		{flow_u0 + 16, flow_vc},
		{flow_uc, flow_v0 + 16},
		{flow_u0, flow_vc},
	};
	for(size_t i = 0; i < 4; i++) {
		int u = packed_uv(&dl, i, false), v = packed_uv(&dl, i, true);
		assert(u == expected_diagonal_uv[i][0]);
		assert(v == expected_diagonal_uv[i][1]);
		assert(u >= flow_u0 && u <= flow_u0 + 16);
		assert(v >= flow_v0 && v <= flow_v0 + 16);
	}
	displaylist_destroy(&dl);

	/*
		BlockFluid.shouldSideBeRendered suppresses the whole MATERIAL, not only
		the same block id: moving/still water and moving/still lava share no seam;
		ice is also explicitly suppressed. Water beside lava is a real boundary.
	*/
	render_grid_clear();
	render_grid[RX][RY][RZ]
		= (struct block_data) {.type = BLOCK_WATER_FLOW, .metadata = 1};
	struct block_info self = render_info();
	struct block_info neighbour = render_neighbour(SIDE_RIGHT);
	uint8_t light[24] = {0};
	render_grid[RX + 1][RY][RZ]
		= (struct block_data) {.type = BLOCK_WATER_STILL, .metadata = 0};
	neighbour = render_neighbour(SIDE_RIGHT);
	assert(render_block_fluid(NULL, &self, SIDE_RIGHT, &neighbour, light, true) == 0);
	render_grid[RX + 1][RY][RZ]
		= (struct block_data) {.type = BLOCK_ICE, .metadata = 0};
	neighbour = render_neighbour(SIDE_RIGHT);
	assert(render_block_fluid(NULL, &self, SIDE_RIGHT, &neighbour, light, true) == 0);
	render_grid[RX + 1][RY][RZ]
		= (struct block_data) {.type = BLOCK_LAVA_STILL, .metadata = 0};
	neighbour = render_neighbour(SIDE_RIGHT);
	assert(render_block_fluid(NULL, &self, SIDE_RIGHT, &neighbour, light, true) == 1);

	render_grid[RX][RY][RZ]
		= (struct block_data) {.type = BLOCK_LAVA_FLOW, .metadata = 1};
	self = render_info();
	render_grid[RX + 1][RY][RZ]
		= (struct block_data) {.type = BLOCK_LAVA_STILL, .metadata = 0};
	neighbour = render_neighbour(SIDE_RIGHT);
	assert(render_block_fluid(NULL, &self, SIDE_RIGHT, &neighbour, light, true) == 0);
}

int main(void) {
	blocks_init();
	test_fluid_render_goldens();

	// --- pin the Beta tick rates on the real flowing structs ---
	assert(blocks[BLOCK_WATER_FLOW]->tick_delay == FLUID_WATER_TICK_RATE);
	assert(blocks[BLOCK_WATER_FLOW]->tick_delay == 5);
	assert(blocks[BLOCK_LAVA_FLOW]->tick_delay == FLUID_LAVA_TICK_RATE);
	assert(blocks[BLOCK_LAVA_FLOW]->tick_delay == 30);
	// the STILL structs have no onWorldTick, so they must not be scheduled at all
	assert(blocks[BLOCK_WATER_STILL]->onWorldTick == NULL);
	assert(blocks[BLOCK_LAVA_STILL]->onWorldTick == NULL);

	// Beta BlockFluid uses render type 4 for BOTH moving and stationary IDs.
	// Under CaveWii's fancy-liquid pass that means every water/lava variant must
	// be translucent fluid geometry. The shipped moving-water registration was
	// instead opaque render_block_full while returning anim.png cell (5,0); the
	// opaque pass binds terrain, where packed cell 5 is the brick texture.
	#ifdef GFX_FANCY_LIQUIDS
	assert(blocks[BLOCK_WATER_FLOW]->transparent);
	assert(blocks[BLOCK_WATER_STILL]->transparent);
	assert(blocks[BLOCK_WATER_FLOW]->renderBlock == render_block_fluid);
	assert(blocks[BLOCK_WATER_STILL]->renderBlock == render_block_fluid);
	assert(blocks[BLOCK_LAVA_FLOW]->transparent);
	assert(blocks[BLOCK_LAVA_STILL]->transparent);
	assert(blocks[BLOCK_LAVA_FLOW]->renderBlock == render_block_fluid);
	assert(blocks[BLOCK_LAVA_STILL]->renderBlock == render_block_fluid);

	// bug-hunt D10: lava's getSideMask carries water's flow/still equivalence.
	// Beta BlockFluid.shouldSideBeRendered suppresses by MATERIAL, so a lava
	// column mixing ids 10/11 occludes its internal TOP seam face instead of
	// overdrawing it; a non-lava neighbour above leaves the face exposed.
	{
		struct block_data lf = {.type = BLOCK_LAVA_FLOW, .metadata = 1};
		struct block_data ls = {.type = BLOCK_LAVA_STILL, .metadata = 0};
		struct block_data st = {.type = BLOCK_STONE, .metadata = 0};
		struct block_info self
			= {.block = &lf, .neighbours = NULL, .x = 0, .y = 10, .z = 0};
		struct block_info above
			= {.block = &ls, .neighbours = NULL, .x = 0, .y = 11, .z = 0};
		assert(blocks[BLOCK_LAVA_FLOW]->getSideMask(&self, SIDE_TOP, &above)
			   == face_occlusion_full());
		self.block = &ls;
		above.block = &lf;
		assert(blocks[BLOCK_LAVA_STILL]->getSideMask(&self, SIDE_TOP, &above)
			   == face_occlusion_full());
		above.block = &st;
		assert(blocks[BLOCK_LAVA_STILL]->getSideMask(&self, SIDE_TOP, &above)
			   == face_occlusion_empty());
	}
	#endif

	// --- drive the real queue: a flowing fluid fires at current+tickRate, not current+1 ---
	struct server_world world;
	memset(&world, 0, sizeof(world));
	server_world_scheduled_ticks_create(&world,
										SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	world.current_tick = 100;

	assert(server_world_schedule_block_tick_raw(
		&world, 0, 64, 0, BLOCK_WATER_FLOW));
	assert(server_world_schedule_block_tick_raw(
		&world, 1, 64, 0, BLOCK_LAVA_FLOW));
	// the still fluids cannot be scheduled (no onWorldTick) -> schedule returns false
	assert(!server_world_schedule_block_tick_raw(
		&world, 2, 64, 0, BLOCK_WATER_STILL));
	assert(!server_world_schedule_block_tick_raw(
		&world, 3, 64, 0, BLOCK_LAVA_STILL));
	assert(world.scheduled_tick_count == 2);

	int saw_water = 0, saw_lava = 0;
	struct scheduled_tick t;
	while(server_world_scheduled_tick_pop(&world, &t)) {
		if(t.block_type == BLOCK_WATER_FLOW) {
			assert(t.fire_at_tick == 100 + 5); // water schedules +5
			saw_water = 1;
		} else if(t.block_type == BLOCK_LAVA_FLOW) {
			assert(t.fire_at_tick == 100 + 30); // lava schedules +30
			saw_lava = 1;
		}
	}
	assert(saw_water && saw_lava);

	server_world_scheduled_ticks_destroy(&world);
	printf("test_fluid_tick_rate: OK (water/lava render registrations; Beta source/falling "
		   "heights, corner slopes, material culling + flow UV; queue fires at +5/+30)\n");
	return 0;
}
