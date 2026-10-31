#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "graphics/texture_atlas.h"
#include "graphics/fire_overlay.h"

static unsigned read_calls = 0;
static size_t fixture_side;

uint8_t* tex_read_atlas(const char* filename, size_t* width, size_t* height) {
	assert(filename);
	assert(width && height);
	/* The archive wrapper must establish deterministic dimensions before a
	 * decoder failure. tex_atlas_compute receives these values even on NULL. */
	assert(*width == 0);
	assert(*height == 0);
	read_calls++;
	if(fixture_side) {
		*width = *height = fixture_side;
		uint8_t* pixels = malloc(fixture_side * fixture_side * 4);
		assert(pixels);
		for(size_t y = 0; y < fixture_side; y++) {
			for(size_t x = 0; x < fixture_side; x++) {
				uint8_t* p = pixels + (y * fixture_side + x) * 4;
				p[0] = x / (fixture_side / 16);
				p[1] = y / (fixture_side / 16);
				p[2] = 91;
				p[3] = 255;
			}
		}
		return pixels;
	}
	return NULL;
}

static void test_particle_uvs(size_t side) {
	/* Sample every texel center through the renderer's UV rectangle in an
	 * atlas built by the real packer. Source-cell colors are an independent
	 * oracle, including the packed row transition after particle 13. */
	static const uint8_t cells[][2] = {
		{0,0},{1,0},{2,0},{3,0},{4,0},{5,0},{6,0},{7,0},
		{0,1},{1,1},{2,1},{3,1},{4,1},{5,1},{6,1},{7,1},
		{0,2},{1,2},{0,3},{1,3},{0,4},{0,5},{0,6},{1,6},
	};
	fixture_side = side;
	size_t w = 0, h = 0;
	uint8_t* packed = tex_atlas_particles("fixture", &w, &h);
	assert(packed && w == side && h == side);
	for(size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
		uint8_t tile = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_0 + i);
		float uv[4];
		assert(tex_atlas_tile_uv(w, h, tile, uv));
		if(side == 256 || side == 512) {
			assert(uv[0] == (TEXTURE_X(tile) * 18 + 3) / 256.0f);
			assert(uv[1] == (TEXTURE_Y(tile) * 18 + 3) / 256.0f);
			assert(uv[2] == uv[0] + 16 / 256.0f);
			assert(uv[3] == uv[1] + 16 / 256.0f);
		}
		for(size_t y = 0; y < side / 16; y++) {
			for(size_t x = 0; x < side / 16; x++) {
				size_t px = (size_t)((uv[0] + (x + 0.5f) / side) * side);
				size_t py = (size_t)((uv[1] + (y + 0.5f) / side) * side);
				assert(px < side && py < side);
				const uint8_t* p = packed + (py * side + px) * 4;
				assert(p[0] == cells[i][0] && p[1] == cells[i][1]);
				assert(p[2] == 91 && p[3] == 255);
			}
		}
	}
	free(packed);
	printf("PASS: all 24 particle cells sample their own pixels at %zux%zu\n", side, side);
}

int main(void) {
	/* Beta ItemRenderer.renderFireInFirstPerson: two opposed quads, alpha 0.9.
	 * Quad 0 samples the fire strip, quad 1 the fire+16 strip. */
	assert(!fire_overlay_visible(false, false, true));
	assert(!fire_overlay_visible(true, true, true));
	assert(!fire_overlay_visible(true, false, false));
	assert(fire_overlay_visible(true, false, true));
	assert(fire_overlay_frame_ms(0) == 0);
	assert(fire_overlay_frame_ms(800) == 7);
	assert(fire_overlay_frame_ms(1600) == 14);
	assert(fire_overlay_frame_ms(3200) == 0);
	for(int frame = 0; frame < 28; frame++) {
		struct fire_overlay_quad left = fire_overlay_quad(frame, 0);
		struct fire_overlay_quad right = fire_overlay_quad(frame, 1);
		float u0 = (3 * 18 + 3 + (frame / 14) * 7 * 18) / 256.0f;
		float v0 = (3 + (frame % 14) * 18) / 256.0f;
		float u1 = u0 + 15.99f / 256.0f;
		float v1 = v0 + 15.99f / 256.0f;
		assert(left.x_offset == 0.24f && left.rotation_y_deg == -10.0f);
		assert(right.x_offset == -0.24f && right.rotation_y_deg == 10.0f);
		assert(left.uv[0] == u1 && left.uv[1] == v1);
		assert(left.uv[2] == u0 && left.uv[3] == v1);
		assert(left.uv[4] == u0 && left.uv[5] == v0);
		assert(left.uv[6] == u1 && left.uv[7] == v0);
		/* fire-anim-0924/F1: ItemRenderer.java:385 -- the second quad samples
		 * fire+16, baked as anim.png's second strip (cols 6 & 13). */
		float ru0 = (6 * 18 + 3 + (frame / 14) * 7 * 18) / 256.0f;
		float ru1 = ru0 + 15.99f / 256.0f;
		assert(right.uv[0] == ru1 && right.uv[1] == v1);
		assert(right.uv[2] == ru0 && right.uv[3] == v1);
		assert(right.uv[4] == ru0 && right.uv[5] == v0);
		assert(right.uv[6] == ru1 && right.uv[7] == v0);
		assert(ru1 < 1.0f);
		assert(u1 < 1.0f && v1 < 1.0f);
	}
	printf("PASS: fire overlay owner gate, opposed Beta quads, 28-frame UVs\n");

	size_t width = 123, height = 456;
	assert(tex_atlas_block("missing-terrain.png", &width, &height) == NULL);
	assert(width == 0 && height == 0);

	width = 789;
	height = 321;
	assert(tex_atlas_particles("missing-particles.png", &width, &height) == NULL);
	assert(width == 0 && height == 0);
	assert(read_calls == 2);
	test_particle_uvs(128);
	test_particle_uvs(256);
	test_particle_uvs(384);
	test_particle_uvs(512);
	float uv[4];
	assert(!tex_atlas_tile_uv(0, 0, 0, uv));
	assert(!tex_atlas_tile_uv(256, 128, 0, uv));
	assert(!tex_atlas_tile_uv(128, 128, 255, uv));
	return 0;
}
