/*
	BOOT-CREDITS-1028 T5: the fast first-boot settle produces the SAME scene
	as the pre-T5 (legacy) settle.

	Runs the real bake (worldgen terrain + Beta populate, default recipe)
	twice into a REAL client struct world, with the REAL settle-light policy
	main.c ships (backdrop_settle.h): legacy at the old 8 ms step budget, and
	fast at the boot-card 25 ms budget. Each run settles light to its fixed
	point, then dumps every installed column cell (block id, metadata, sky
	and torch light). Meshes are a pure function of exactly these bytes, and
	the fast path only changes WHEN meshes are dispatched (after light is
	idle), never what they read, so equal dumps mean an equal captured frame.
	The dumps must be byte-identical; the captured 1024x576 frames (derived
	from the dump hash) too. Also prints the step counts of both runs.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NDEBUG
#include <assert.h>

#include "block/blocks.h"
#include "game/game_state.h"
#include "game/gui/backdrop_bake.h"
#include "game/gui/backdrop_settle.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/mem2.h"
#include "world.h"

#define FRAME_W 640
#define FRAME_H 480
#define MAX_COLS 512

struct run {
	struct world w;
	bool open, legacy;
	int cols[MAX_COLS][2];
	size_t ncols;
	unsigned steps, light_calls;
	uint8_t* dump;
	size_t dump_size;
	uint64_t hash;
};

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
	for(size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

static bool r_alive(void* u) {
	(void)u;
	return false;
}

static bool r_begin(void* u, const struct backdrop_bake_recipe* r) {
	struct run* x = u;
	(void)r;
	world_create(&x->w);
	x->w.dimension = WORLD_DIM_OVERWORLD;
	x->open = true;
	return true;
}

static bool r_install(void* u, int cx, int cz, const uint8_t* ids,
	const uint8_t* meta, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* hm, const uint8_t* gi, const uint8_t* gj, bool visible) {
	struct run* x = u;
	(void)visible;
	assert(x->ncols < MAX_COLS);
	/* the same two calls as main.c bake_host_install */
	if(!world_build_full_section(&x->w, cx * CHUNK_SIZE, cz * CHUNK_SIZE, ids,
		   meta, sky, torch, hm, gi, gj))
		return false;
	world_light_repair_mark(&x->w, cx, cz);
	x->cols[x->ncols][0] = cx;
	x->cols[x->ncols][1] = cz;
	x->ncols++;
	return true;
}

static enum backdrop_bake_settle r_settle(void* u, uint32_t budget_us) {
	struct run* x = u;
	x->steps++;
	x->light_calls += backdrop_settle_light(&x->w, time_get(), budget_us,
		x->legacy);
	return backdrop_settle_light_busy(&x->w) ? BACKDROP_BAKE_SETTLE_PENDING
											 : BACKDROP_BAKE_SETTLE_READY;
}

static void dump_scene(struct run* x) {
	x->dump_size = x->ncols * CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT * 3;
	x->dump = malloc(x->dump_size);
	assert(x->dump);
	uint8_t* p = x->dump;
	for(size_t c = 0; c < x->ncols; c++)
		for(int y = 0; y < WORLD_HEIGHT; y++)
			for(int z = 0; z < CHUNK_SIZE; z++)
				for(int xx = 0; xx < CHUNK_SIZE; xx++) {
					struct block_data b = world_get_block(&x->w,
						x->cols[c][0] * CHUNK_SIZE + xx, y,
						x->cols[c][1] * CHUNK_SIZE + z);
					*p++ = b.type;
					*p++ = b.metadata;
					*p++ = (uint8_t)(b.sky_light << 4 | b.torch_light);
				}
	x->hash = fnv(1469598103934665603ull, x->dump, x->dump_size);
}

static bool r_capture(void* u, backdrop_bake_rows_fn sink, void* su) {
	struct run* x = u;
	dump_scene(x);
	static uint8_t band[FRAME_W * 32 * 4];
	for(size_t y0 = 0; y0 < FRAME_H; y0 += 32) {
		for(size_t y = 0; y < 32; y++)
			for(size_t px = 0; px < FRAME_W; px++) {
				uint64_t v = x->hash ^ ((y0 + y) * 2654435761u) ^ (px * 40503u);
				uint8_t* q = band + (y * FRAME_W + px) * 4;
				q[0] = (uint8_t)v;
				q[1] = (uint8_t)(v >> 8);
				q[2] = (uint8_t)(v >> 16);
				q[3] = 255;
			}
		if(!sink(su, band, y0, 32, FRAME_W, FRAME_H)) return false;
	}
	return true;
}

static void r_end(void* u) {
	struct run* x = u;
	world_destroy(&x->w);
	x->open = false;
}

static uint8_t* bake(struct run* x, bool legacy, uint32_t budget_us) {
	memset(x, 0, sizeof(*x));
	x->legacy = legacy;
	struct backdrop_bake_host h = {
		.user = x,
		.world_alive = r_alive,
		.scene_begin = r_begin,
		.install_column = r_install,
		.settle_step = r_settle,
		.render_capture = r_capture,
		.scene_end = r_end,
	};
	enum backdrop_bake_error err;
	struct backdrop_bake* b
		= backdrop_bake_create(&h, backdrop_bake_default_recipe(), &err);
	assert(b && err == BACKDROP_BAKE_OK);
	enum backdrop_bake_state s = BACKDROP_BAKE_TERRAIN;
	for(unsigned i = 0; i < 1000000; i++) {
		s = backdrop_bake_step(b, budget_us);
		if(s == BACKDROP_BAKE_DONE || s == BACKDROP_BAKE_FAILED
		   || s == BACKDROP_BAKE_CANCELLED)
			break;
	}
	assert(s == BACKDROP_BAKE_DONE);
	assert(!x->open && x->dump);
	uint8_t* px = backdrop_bake_take_pixels(b);
	assert(px);
	backdrop_bake_destroy(b);
	return px;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	assert(clin_init() && svin_init());
	static struct run legacy, fast;
	uint8_t* a = bake(&legacy, true, 8000);
	uint8_t* b = bake(&fast, false, 25000);
	assert(legacy.ncols == fast.ncols && legacy.ncols > 0);
	assert(memcmp(legacy.cols, fast.cols, sizeof(legacy.cols)) == 0);
	assert(legacy.dump_size == fast.dump_size);
	assert(memcmp(legacy.dump, fast.dump, legacy.dump_size) == 0);
	assert(memcmp(a, b, (size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4)
		   == 0);
	/* light really moved: the repair is not a no-op on this scene */
	assert(legacy.light_calls > legacy.steps || legacy.steps > 1);
	printf("PASS settle equivalence: %zu columns, scene hash %016llx; legacy "
		   "(8 ms) %u settle steps / %u light calls, fast (25 ms) %u steps / "
		   "%u light calls; cells, light and 1024x576 frame byte-identical\n",
		legacy.ncols, (unsigned long long)legacy.hash, legacy.steps,
		legacy.light_calls, fast.steps, fast.light_calls);
	free(legacy.dump);
	free(fast.dump);
	backdrop_bake_pixels_free(a);
	backdrop_bake_pixels_free(b);
	return 0;
}
