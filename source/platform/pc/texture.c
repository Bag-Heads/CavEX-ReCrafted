/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <GL/glew.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../texture.h"

// Mirrors wii/texture.c's tex_build_mip_chain_rgba16 LOD count: 4 levels
// (0..3). PC has no RGB5A3 4x4-block constraint, so unlike the Wii path this
// is a soft cap -- tex_gfx_load_atlas builds as many of these as the atlas
// dimensions actually support and uses GL_TEXTURE_MAX_LEVEL to stop there.
enum { PC_ATLAS_MAX_LODS = 4 };

void tex_init_pre() { }
void tex_init_post() { }

void tex_gfx_clear(struct tex_gfx* tex) {
	assert(tex);
	if(tex->data)
		free(tex->data);
	if(tex->id)
		glDeleteTextures(1, &tex->id);
	*tex = (struct tex_gfx) {0};
}

bool tex_gfx_load(struct tex_gfx* tex, void* img, size_t width, size_t height,
				  enum tex_format type, bool linear) {
	assert(tex && img && width > 0 && height > 0);

	/* GL errors are sticky. Drain and report any older error so the check below
	 * attributes only this upload instead of either ignoring an OOM or blaming a
	 * previous draw call for a valid texture. */
	GLenum stale_error;
	while((stale_error = glGetError()) != GL_NO_ERROR)
		fprintf(stderr, "[texture] clearing stale GL error 0x%x before upload\n",
				(unsigned)stale_error);

	GLuint new_id = 0;
	glGenTextures(1, &new_id);
	if(!new_id || glGetError() != GL_NO_ERROR) {
		if(new_id)
			glDeleteTextures(1, &new_id);
		free(img);
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, new_id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
				 GL_UNSIGNED_BYTE, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLenum upload_error = glGetError();
	if(upload_error != GL_NO_ERROR) {
		fprintf(stderr, "[texture] GL upload failed with error 0x%x\n",
				(unsigned)upload_error);
		glDeleteTextures(1, &new_id);
		free(img);
		return false;
	}

	// Re-entrant pack switch: replace only after the new GL object and storage exist.
	tex_gfx_clear(tex);
	tex->fmt = type;
	tex->data = img;
	tex->width = width;
	tex->height = height;
	tex->id = new_id;
	tex->data_bytes = width * height * 4;
	tex->lods = 1;
	tex->linear = linear;
	return true;
}

bool tex_gfx_load_atlas(struct tex_gfx* tex, void* img, size_t width,
						size_t height, enum tex_format type,
						const struct tex_atlas_grid* grid) {
	assert(tex && img && width > 0 && height > 0);

	// Only the atlas format flows through the mip-chain path (terrain.png,
	// particles.png) -- same gate as wii/texture.c. Anything else (or a grid
	// the caller couldn't build) keeps the existing single-LOD load so no
	// other caller's behaviour changes.
	if(type != TEX_FMT_RGBA16 || !grid || !grid->cell_pitch) {
		return tex_gfx_load(tex, img, width, height, type, true);
	}

	/* Same stale-error drain as tex_gfx_load, for the same reason. */
	GLenum stale_error;
	while((stale_error = glGetError()) != GL_NO_ERROR)
		fprintf(stderr,
				"[texture] clearing stale GL error 0x%x before atlas upload\n",
				(unsigned)stale_error);

	GLuint new_id = 0;
	glGenTextures(1, &new_id);
	if(!new_id || glGetError() != GL_NO_ERROR) {
		if(new_id)
			glDeleteTextures(1, &new_id);
		free(img);
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, new_id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
				 GL_UNSIGNED_BYTE, img);

	// DC-GRASS G3: the terrain atlas samples like Beta 1.7.3 -- RenderEngine
	// .java:20 `useMipmaps = false` and :168-169 GL_NEAREST for both MIN and
	// MAG in setupTexture -- so level 0 only, no mip chain. Every terrain path
	// (fresh load, pack reload, pack-cache restore) funnels through here with
	// &texture_terrain. Other atlases (particles) keep the mip chain below.
	// PC only: the Wii GX path keeps nearest-mag/trilinear-min (recorded
	// deviation, Astra DC-GRASS ruling 3).
	if(tex == &texture_terrain) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	} else {
		// Box-filter LODs 1.. from the SAME atlas-aware downsampler the Wii GX path
		// uses (tex_mip_downsample_rgba8, platform/texture_mip.c): every output
		// texel clamps its sample box to the logical atlas cell it falls in, so a
		// cell boundary can never bleed a neighbouring tile into a mip level. Plain
		// box filtering (or glGenerateMipmap, which knows nothing about the atlas
		// grid) would reintroduce exactly that bleed at LOD>=1. Builds however many
		// of the PC_ATLAS_MAX_LODS levels the atlas size actually supports (the
		// downsampler returns NULL once a level would be 0px) instead of hard-
		// failing like the Wii path does -- there is no block-encoder alignment
		// constraint to fail against here, so a smaller atlas (e.g. particles.png)
		// just gets a shallower, still bleed-free, chain.
		int built = 0;
		for(int n = 1; n < PC_ATLAS_MAX_LODS; n++) {
			uint8_t* mip
				= tex_mip_downsample_rgba8(img, width, height, grid, (unsigned)n);
			if(!mip)
				break;
			glTexImage2D(GL_TEXTURE_2D, n, GL_RGBA, (GLsizei)(width >> n),
						 (GLsizei)(height >> n), 0, GL_RGBA, GL_UNSIGNED_BYTE, mip);
			free(mip);
			built = n;
		}

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, built);
		// Trilinear minification (linear within a LOD + linear between LODs) so
		// distant terrain stays clean instead of shimmering -- matches the Wii GX
		// path's GX_LIN_MIP_LIN. Magnification stays GL_LINEAR, unchanged from the
		// single-LOD load this replaces (the Wii path's GX_NEAR blocky magnifier is
		// a separate, unscoped visual-parity decision, not part of "add mipmaps").
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
						built > 0 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glBindTexture(GL_TEXTURE_2D, 0);

	GLenum upload_error = glGetError();
	if(upload_error != GL_NO_ERROR) {
		fprintf(stderr, "[texture] GL atlas mip upload failed with error 0x%x\n",
				(unsigned)upload_error);
		glDeleteTextures(1, &new_id);
		free(img);
		return false;
	}

	tex_gfx_clear(tex); // re-entrant reload (pack switch): drop the old GL object first
	tex->fmt = type;
	tex->data = img;
	tex->width = width;
	tex->height = height;
	tex->id = new_id;
	tex->data_bytes = width * height * 4;
	tex->lods = 1;
	tex->linear = true;
	return true;
}

/* pack-cache-0927 backend hooks (texture.h). PC keeps the RGBA image as the
   CPU mirror, so a restore is the ordinary upload of the identical image. */
void* tex_cache_texel_alloc(size_t bytes) {
	return malloc(bytes);
}

void tex_cache_texel_free(void* p) {
	free(p);
}

bool tex_gfx_restore(struct tex_gfx* tex, void* data, size_t data_bytes,
					 size_t width, size_t height, enum tex_format type,
					 unsigned lods, bool linear, bool atlas) {
	if(lods != 1 || data_bytes != width * height * 4 || !width || !height) {
		free(data);
		return false;
	}
	if(atlas) {
		struct tex_atlas_grid grid;
		if(!tex_atlas_grid_init(&grid, width, height)) {
			free(data);
			return false;
		}
		return tex_gfx_load_atlas(tex, data, width, height, type, &grid);
	}
	return tex_gfx_load(tex, data, width, height, type, linear);
}

void tex_gfx_subimage(struct tex_gfx* tex, int x, int y, int w, int h,
					  const uint8_t* rgba) {
	assert(tex && rgba && w > 0 && h > 0);
	if(!tex->data || !tex->id)
		return;
	if(x < 0 || y < 0 || (size_t)(x + w) > tex->width
	   || (size_t)(y + h) > tex->height)
		return; // never write outside the atlas
	glBindTexture(GL_TEXTURE_2D, tex->id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glBindTexture(GL_TEXTURE_2D, 0);
	// keep the CPU mirror (tex->data) consistent for any tex_gfx_lookup
	for(int row = 0; row < h; row++)
		memcpy(tex->data + (((size_t)(y + row) * tex->width) + x) * 4,
			   rgba + (size_t)row * w * 4, (size_t)w * 4);
}

void tex_gfx_wrap_mode(struct tex_gfx* tex, bool repeat) {
	assert(tex);

	glBindTexture(GL_TEXTURE_2D, tex->id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
					repeat ? GL_REPEAT : GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
					repeat ? GL_REPEAT : GL_CLAMP);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void tex_gfx_bind(struct tex_gfx* tex, int slot) {
	assert(tex);
	glBindTexture(GL_TEXTURE_2D, tex->id);
}

void tex_gfx_lookup(struct tex_gfx* tex, int x, int y, uint8_t* color) {
	assert(tex && color);

	memcpy(color,
		   tex->data
			   + (((unsigned int)x % tex->width)
				  + ((unsigned int)y % tex->height) * tex->width)
				   * 4,
		   4);
}
