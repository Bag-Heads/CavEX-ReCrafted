/* ci-1101/23: split-screen held maps must not share one live texture.
 *
 * Both split viewports render in the same GX frame. With a single
 * texture_map_live and a single uploaded_id/uploaded_version, P2's upload
 * overwrote the surface while P1's draw was still queued (DCStoreRange +
 * GX_InvalidateTexAll do not wait for the FIFO), so P1 saw P2's map, and the
 * 64 KB conversion ran twice per frame forever. Each viewport now owns its
 * own live surface and its own change gate.
 *
 * Pinned here, PC backend (tex->data is the CPU mirror of the GL texture):
 *   (1) two owners holding different map ids in one frame -> each upload
 *       lands on ITS viewport's texture with that map's pixels, and the two
 *       textures differ;
 *   (2) the next frame re-uploads NEITHER (a scribble on each mirror
 *       survives the second call, so the change gate is per viewport);
 *   (3) a real version change on one map re-uploads only that viewport;
 *   (4) the single-player entry point still targets viewport 0. */
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "game/game_state.h"
#include "graphics/render_item.h"
#include "item/items.h"
#include "item/items/item_map.h"
#include "network/client_interface.h"
#include "network/map_data.h"
#include "platform/texture.h"

static void fill(uint8_t* colors, uint8_t base) {
	/* Explored palette bytes (>= 4) so alpha is 255 and RGB is map-specific. */
	for(unsigned i = 0; i < MAP_DATA_COLORS_SIZE; i++)
		colors[i] = (uint8_t)(4 + ((base + i) % 48));
}

static void expect_pixels(const struct tex_gfx* tex, const uint8_t* colors) {
	static uint8_t expected[128 * 128 * 4];
	item_map_colors_to_rgba(colors, expected);
	assert(tex->data);
	assert(memcmp(tex->data, expected, sizeof(expected)) == 0);
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	memset(&gstate, 0, sizeof(gstate));

	assert(glfwInit());
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* gl = glfwCreateWindow(32, 32, "map live per viewport", NULL, NULL);
	assert(gl);
	glfwMakeContextCurrent(gl);
	assert(glewInit() == GLEW_OK);

	struct tex_gfx* live[CLIENT_LOCAL_PLAYERS];
	for(unsigned v = 0; v < CLIENT_LOCAL_PLAYERS; v++) {
		live[v] = render_item_map_live_texture(v);
		assert(live[v]);
		assert(tex_gfx_load(live[v], calloc(128 * 128, 4), 128, 128,
			TEX_FMT_RGBA32, false));
	}
	assert(live[0] != live[1]);
	assert(live[0] == &texture_map_live); /* single-player surface unchanged */

	static uint8_t map_a[MAP_DATA_COLORS_SIZE], map_b[MAP_DATA_COLORS_SIZE];
	fill(map_a, 0);
	fill(map_b, 17);
	assert(clin_map_deliver(1, map_a));
	assert(clin_map_deliver(2, map_b));

	/* (1) One frame: P1 holds map 1, P2 holds map 2. */
	assert(render_item_map_upload_viewport(0, 1));
	assert(render_item_map_upload_viewport(1, 2));
	expect_pixels(live[0], map_a);
	expect_pixels(live[1], map_b);
	assert(memcmp(live[0]->data, live[1]->data, 128 * 128 * 4) != 0);

	/* (2) Next frame, nothing changed: neither viewport re-uploads. */
	live[0]->data[0] ^= 0xFF;
	live[1]->data[0] ^= 0xFF;
	uint8_t probe0 = live[0]->data[0], probe1 = live[1]->data[0];
	assert(render_item_map_upload_viewport(0, 1));
	assert(render_item_map_upload_viewport(1, 2));
	assert(live[0]->data[0] == probe0);
	assert(live[1]->data[0] == probe1);

	/* (3) Map 2's pixels change: only viewport 1 re-uploads. */
	fill(map_b, 29);
	assert(clin_map_deliver(2, map_b));
	assert(render_item_map_upload_viewport(0, 1));
	assert(render_item_map_upload_viewport(1, 2));
	assert(live[0]->data[0] == probe0);
	expect_pixels(live[1], map_b);

	/* (4) The single-player entry point is viewport 0, and swapping P1's map
	 * never touches P2's surface. */
	probe1 = live[1]->data[0];
	assert(render_item_map_upload(2));
	expect_pixels(live[0], map_b);
	assert(live[1]->data[0] == probe1);

	/* (5) R5: a texture-pack reload recreates BOTH surfaces zero-filled.
	 * The next frame must re-upload each viewport's unchanged map instead
	 * of drawing blank until its version moves. */
	for(unsigned v = 0; v < CLIENT_LOCAL_PLAYERS; v++) tex_gfx_clear(live[v]);
	uint32_t gen = tex_map_live_generation();
	tex_map_live_init();
	assert(tex_map_live_generation() != gen);
	for(unsigned v = 0; v < CLIENT_LOCAL_PLAYERS; v++) {
		assert(live[v]->data);
		for(unsigned i = 0; i < 128 * 128 * 4; i++) assert(live[v]->data[i] == 0);
	}
	assert(render_item_map_upload_viewport(0, 2));
	assert(render_item_map_upload_viewport(1, 2));
	expect_pixels(live[0], map_b);
	expect_pixels(live[1], map_b);

	for(unsigned v = 0; v < CLIENT_LOCAL_PLAYERS; v++) tex_gfx_clear(live[v]);
	glfwDestroyWindow(gl);
	glfwTerminate();
	puts("PASS held map: one live texture and one change gate per split viewport; reload re-uploads");
	return 0;
}
