/* ci-1101/31: split-screen compass/clock must follow THEIR OWN player.
 *
 * item_needle_fx_tick drove ONE needle state from gstate.players[0] and
 * wrote it into the shared items atlas cell, so player 2's held/hotbar
 * compass pointed for player 1. Now every viewport has its own spring state;
 * viewport 0 keeps writing the atlas cell exactly as before (single player
 * and viewport 0 byte-identical), and viewport 1 renders its compass/clock
 * from a private 16x16 texture pair the draw path binds instead of the atlas
 * cell (render_item_flat, when render_item_viewport() == 1).
 *
 * Pinned here, PC backend (tex->data is the CPU mirror of the GL texture):
 *   (1) two local players at different positions/yaws, N ticks: the atlas
 *       compass cell (viewport 0) and the viewport-1 compass texture differ,
 *       and each equals a fresh compass_fx_tick run on ITS player's inputs;
 *   (2) single player (no players[1] body): the atlas cell is unchanged
 *       from (1)'s player-0 reference and the draw path gets NO split texture
 *       for viewport 1 (and never for viewport 0);
 *   (3) the draw path's viewport selector round-trips and defaults to 0;
 *   (4) R7: with a dial, P2's clock equals viewport 0's at several times of
 *       day and moves across the day; the extrusion strip UVs step one texel
 *       on the atlas and 16 units on the private surface. */
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "graphics/compass_fx.h"
#include "graphics/render_item.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "platform/texture.h"
#include "platform/time.h"

#define TICKS 12

static void read_cell(const struct tex_gfx* tex, int cell_x, int cell_y,
					  uint8_t out[COMPASS_FX_BYTES]) {
	assert(tex->data);
	for(int y = 0; y < 16; y++)
		memcpy(out + y * 16 * 4,
			   tex->data + ((cell_y * 16 + y) * tex->width + cell_x * 16) * 4,
			   16 * 4);
}

static void reference(const uint8_t base[COMPASS_FX_BYTES],
					  const struct entity* player, unsigned ticks,
					  uint8_t out[COMPASS_FX_BYTES]) {
	struct compass_fx_state st;
	compass_fx_state_init(&st);
	for(unsigned i = 0; i < ticks; i++)
		compass_fx_tick(&st, base, true, false, 0.0,
						(double)gstate.world_spawn_x,
						(double)gstate.world_spawn_z, (double)player->pos[0],
						(double)player->pos[2], (double)player->orient[0], out);
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	memset(&gstate, 0, sizeof(gstate));

	assert(glfwInit());
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* gl
		= glfwCreateWindow(32, 32, "needle per viewport", NULL, NULL);
	assert(gl);
	glfwMakeContextCurrent(gl);
	assert(glewInit() == GLEW_OK);

	/* A vanilla-layout 256x256 items atlas with recognisable compass/clock
	 * cells (opaque, non-uniform) so the needle composite is visible. */
	uint8_t* atlas = calloc(256 * 256, 4);
	assert(atlas);
	for(int y = 0; y < 256; y++)
		for(int x = 0; x < 256; x++) {
			uint8_t* p = atlas + (y * 256 + x) * 4;
			p[0] = (uint8_t)(x * 3);
			p[1] = (uint8_t)(y * 5);
			p[2] = (uint8_t)(x ^ y);
			p[3] = 255;
		}
	/* The clock cell (6,4): TextureWatchFX keys the dial on magenta texels
	 * (r == b, g == 0), so give it a magenta disc with an opaque rim. */
	for(int y = 0; y < 16; y++)
		for(int x = 0; x < 16; x++) {
			uint8_t* p = atlas + ((64 + y) * 256 + 96 + x) * 4;
			int dx = x - 8, dy = y - 8;
			bool disc = dx * dx + dy * dy < 36;
			p[0] = disc ? 200 : 60;
			p[1] = disc ? 0 : 40;
			p[2] = disc ? 200 : 20;
			p[3] = 255;
		}
	assert(tex_gfx_load(&texture_items, atlas, 256, 256, TEX_FMT_RGBA32,
						false));
	/* R7: a real misc/dial.png so the clock FX is armed: a non-symmetric
	 * gradient, so a rotated dial produces a different frame. */
	uint8_t* dial = calloc(16 * 16, 4);
	assert(dial);
	for(int y = 0; y < 16; y++)
		for(int x = 0; x < 16; x++) {
			uint8_t* p = dial + (y * 16 + x) * 4;
			p[0] = (uint8_t)(x * 16);
			p[1] = (uint8_t)(y * 16);
			p[2] = (uint8_t)(x < 8 ? 255 : 30);
			p[3] = 255;
		}
	assert(tex_gfx_load(&texture_dial, dial, 16, 16, TEX_FMT_RGBA32, false));
	uint8_t compass_base[COMPASS_FX_BYTES], atlas_clock_static[COMPASS_FX_BYTES];
	read_cell(&texture_items, 6, 3, compass_base);
	read_cell(&texture_items, 6, 4, atlas_clock_static);

	struct entity p1, p2;
	memset(&p1, 0, sizeof(p1));
	memset(&p2, 0, sizeof(p2));
	p1.pos[0] = 100.0F; p1.pos[1] = 64.0F; p1.pos[2] = -40.0F;
	p1.orient[0] = 0.7F;
	p2.pos[0] = -250.0F; p2.pos[1] = 64.0F; p2.pos[2] = 310.0F;
	p2.orient[0] = -2.1F;
	gstate.world_spawn_x = 8;
	gstate.world_spawn_z = -3;
	gstate.world_loaded = true;
	gstate.world.dimension = WORLD_DIM_OVERWORLD;
	gstate.players[0].local_player = &p1;
	gstate.players[1].local_player = &p2;

	/* (1) split: two players, two needles. */
	item_needle_fx_init();
	for(unsigned i = 0; i < TICKS; i++) item_needle_fx_tick(true);

	uint8_t cell0[COMPASS_FX_BYTES], ref0[COMPASS_FX_BYTES],
		ref1[COMPASS_FX_BYTES];
	read_cell(&texture_items, 6, 3, cell0);
	reference(compass_base, &p1, TICKS, ref0);
	reference(compass_base, &p2, TICKS, ref1);
	assert(memcmp(ref0, ref1, sizeof(ref0)) != 0); /* the inputs do differ */
	assert(memcmp(cell0, ref0, sizeof(ref0)) == 0); /* viewport 0 = player 1 */

	struct tex_gfx* split = item_needle_fx_split_texture(1, false);
	assert(split);
	assert(split != &texture_items);
	assert(split->width == 16 && split->height == 16 && split->data);
	assert(memcmp(split->data, ref1, sizeof(ref1)) == 0); /* viewport 1 = P2 */
	assert(memcmp(split->data, cell0, sizeof(ref1)) != 0);
	assert(item_needle_fx_split_texture(0, false) == NULL); /* atlas path */
	assert(item_needle_fx_split_texture(0, true) == NULL);
	/* R7: the clock. Beta's clock reads only the world time, and both local
	 * players share the Overworld clock here, so at any time of day P2's
	 * private clock equals viewport 0's atlas cell; across the day it moves.
	 * (Both players read the clock inside one item_needle_fx_tick, microseconds
	 * apart; the needle is drawn on a 16x16 grid, so that sub-tick jitter is
	 * far below one pixel.) The old /24000 double-wrap pinned P2 near dawn
	 * all day: it fails the equality at noon/dusk/midnight and the "moves". */
	struct tex_gfx* split_clock = item_needle_fx_split_texture(1, true);
	assert(split_clock && split_clock != split);
	uint8_t clock_cell[COMPASS_FX_BYTES], clock_prev[COMPASS_FX_BYTES];
	uint8_t clock_static[COMPASS_FX_BYTES];
	memcpy(clock_static, atlas_clock_static, sizeof(clock_static));
	static const uint64_t times[] = {0, 6000, 12000, 18000, 23000};
	memset(clock_prev, 0, sizeof(clock_prev));
	for(size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
		gstate.world_time = times[i];
		gstate.world_time_start = time_get();
		for(unsigned k = 0; k < 60; k++) item_needle_fx_tick(true); /* settle */
		read_cell(&texture_items, 6, 4, clock_cell);
		assert(memcmp(split_clock->data, clock_cell, sizeof(clock_cell)) == 0);
		assert(memcmp(clock_cell, clock_static, sizeof(clock_cell)) != 0);
		if(i > 0) assert(memcmp(clock_cell, clock_prev, sizeof(clock_cell)) != 0);
		memcpy(clock_prev, clock_cell, sizeof(clock_prev));
	}

	/* (2) single player: viewport 0 output unchanged, no split texture.
	 * A real re-init follows a fresh items.png load (tex_init), so restore the
	 * pristine cells first: init captures the icon FROM the atlas. */
	gstate.players[1].local_player = NULL;
	tex_gfx_subimage(&texture_items, 6 * 16, 3 * 16, 16, 16, compass_base);
	tex_gfx_subimage(&texture_items, 6 * 16, 4 * 16, 16, 16, atlas_clock_static);
	item_needle_fx_init();
	for(unsigned i = 0; i < TICKS; i++) item_needle_fx_tick(true);
	read_cell(&texture_items, 6, 3, cell0);
	assert(memcmp(cell0, ref0, sizeof(ref0)) == 0);
	assert(item_needle_fx_split_texture(1, false) == NULL);
	assert(item_needle_fx_split_texture(1, true) == NULL);
	assert(item_needle_fx_split_texture(0, false) == NULL);

	/* (3) the draw path's viewport selector. */
	assert(render_item_viewport() == 0);
	render_item_set_viewport(1);
	assert(render_item_viewport() == 1);
	render_item_set_viewport(0);
	assert(render_item_viewport() == 0);

	/* (4) R7: extrusion strip UVs. Atlas cell (viewport 0): one unit per
	 * strip, far edge 256 -> 255 on a row/column-15 tile, exactly the old
	 * `s + k` / clamp(s + k + 1) values. Private surface (viewport 1): 16
	 * units per strip so the sides sample the whole icon, far edge 255. */
	for(int k = 0; k <= 16; k++) {
		assert(render_item_flat_uv(96, 16, k) == 96 + k);
		assert(render_item_flat_uv(240, 16, k) == (k == 16 ? 255 : 240 + k));
		assert(render_item_flat_uv(0, 256, k) == (k == 16 ? 255 : 16 * k));
	}
	assert(render_item_flat_uv(96, 16, 16) == 112);  /* s1 of cell (6,x) */
	assert(render_item_flat_uv(0, 256, 1) == 16);    /* strip 0 spans a texel */

	tex_gfx_clear(&texture_items);
	tex_gfx_clear(&texture_dial);
	glfwDestroyWindow(gl);
	glfwTerminate();
	puts("PASS needle per viewport: P1 atlas cell, P2 private texture, "
		 "single player unchanged");
	return 0;
}
