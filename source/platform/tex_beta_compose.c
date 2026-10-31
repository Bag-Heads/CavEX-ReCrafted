/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Raw-jar pack compositions. Contract: tex_beta_compose.h. Each function
 * names the tools/pack_helper/conversion.py code it reproduces. */

#include "tex_beta_compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texture_decode.h"

/* The engine's own decode bound (texture.c tex_read_impl): a sheet larger
 * than this could not load as a pack texture either. */
#define TEX_BETA_MAX_DIM 1024u

/* conversion.convert_image's scale rule for a 256x256 canonical with
 * supported_scales [1, 2, 4, 8] (gui/gui.png, gui/icons.png, terrain.png). */
unsigned tex_beta_sheet_scale(size_t w, size_t h) {
	if(!w || !h || w % TEX_BETA_CANON || h % TEX_BETA_CANON)
		return 0;
	size_t k = w / TEX_BETA_CANON;
	if(h / TEX_BETA_CANON != k)
		return 0;
	return (k == 1 || k == 2 || k == 4 || k == 8) ? (unsigned)k : 0;
}

/* Canonical pixel (cx, cy) of a k-scaled sheet: Pillow NEAREST reduction by
 * an integer factor samples floor((c + 0.5) * k) = c*k + k/2 (crisp look;
 * k = 1 is the identity, the helper's pass-through). */
static const uint8_t* canon_px(const uint8_t* src, size_t w, unsigned k,
							   size_t cx, size_t cy) {
	return src + 4 * ((cy * k + k / 2) * w + (cx * k + k / 2));
}

/* PIL Image.paste(src.crop(box), dest) without a mask: plain replacement of
 * all four channels. */
static void paste_region(uint8_t* dst, const uint8_t* src, size_t w,
						 unsigned k, size_t x0, size_t y0, size_t x1,
						 size_t y1, size_t dx, size_t dy) {
	for(size_t y = y0; y < y1; y++)
		for(size_t x = x0; x < x1; x++)
			memcpy(dst + 4 * ((dy + y - y0) * TEX_BETA_CANON + dx + x - x0),
				   canon_px(src, w, k, x, y), 4);
}

/* conversion.compose_gui2, the gui/gui.png rows. */
bool tex_beta_paste_gui(uint8_t* gui2, const uint8_t* gui, size_t w,
						size_t h) {
	unsigned k = tex_beta_sheet_scale(w, h);
	if(!gui2 || !gui || !k)
		return false;
	paste_region(gui2, gui, w, k, 0, 0, 182, 22, 0, 0);  /* hotbar */
	paste_region(gui2, gui, w, k, 0, 22, 24, 46, 208, 0); /* selection */
	return true;
}

/* conversion.compose_gui2, the gui/icons.png rows. */
bool tex_beta_paste_icons(uint8_t* gui2, const uint8_t* icons, size_t w,
						  size_t h) {
	unsigned k = tex_beta_sheet_scale(w, h);
	if(!gui2 || !icons || !k)
		return false;
	paste_region(gui2, icons, w, k, 0, 0, 16, 16, 0, 229);   /* crosshair */
	paste_region(gui2, icons, w, k, 16, 0, 25, 9, 16, 229);  /* heart bg */
	paste_region(gui2, icons, w, k, 52, 0, 61, 9, 52, 229);  /* full heart */
	return true;
}

/* conversion.compose_anim: terrain tile (3, 4) -- Beta ice -- into anim.png
 * columns 4 and 11, all 14 rows, 18px cells with the 1px clamped border. */
bool tex_beta_paste_ice(uint8_t* anim, const uint8_t* terrain, size_t w,
						size_t h) {
	unsigned k = tex_beta_sheet_scale(w, h);
	if(!anim || !terrain || !k)
		return false;
	static const size_t columns[2] = {4, 11};
	for(size_t c = 0; c < 2; c++) {
		for(size_t row = 0; row < 14; row++) {
			size_t ox = columns[c] * 18 + 2, oy = row * 18 + 2;
			for(int y = -1; y < 17; y++) {
				for(int x = -1; x < 17; x++) {
					size_t sx = 3 * 16 + (size_t)(x < 0 ? 0 : x > 15 ? 15 : x);
					size_t sy = 4 * 16 + (size_t)(y < 0 ? 0 : y > 15 ? 15 : y);
					memcpy(anim + 4 * ((oy + y + 1) * TEX_BETA_CANON + ox + x + 1),
						   canon_px(terrain, w, k, sx, sy), 4);
				}
			}
		}
	}
	return true;
}

static uint8_t* read_rgba(const char* root, const char* name, size_t* w,
						  size_t* h) {
	char path[512];
	int n = root ? snprintf(path, sizeof(path), "%s/%s", root, name)
				 : snprintf(path, sizeof(path), "%s", name);
	if(n < 0 || (size_t)n >= sizeof(path))
		return NULL;
	enum tex_png_decode_status status;
	uint8_t* img = tex_png_decode32_bounded(path, TEX_BETA_MAX_DIM,
											TEX_BETA_MAX_DIM, w, h, &status);
	if(!img)
		fprintf(stderr, "[texture] raw-jar pack: cannot decode %s (status %d)\n",
				path, (int)status);
	return img;
}

static uint8_t* read_base(const char* root, const char* name) {
	size_t w = 0, h = 0;
	uint8_t* img = read_rgba(root, name, &w, &h);
	if(img && (w != TEX_BETA_CANON || h != TEX_BETA_CANON)) {
		fprintf(stderr, "[texture] raw-jar pack: base %s%s%s is %zux%zu, "
				"not 256x256\n", root ? root : "", root ? "/" : "", name, w, h);
		free(img);
		img = NULL;
	}
	return img;
}

/* Decode root/name and apply `paste` onto dst, freeing the decode. */
static bool paste_from(uint8_t* dst, const char* root, const char* name,
					   bool (*paste)(uint8_t*, const uint8_t*, size_t, size_t)) {
	size_t w = 0, h = 0;
	uint8_t* src = read_rgba(root, name, &w, &h);
	if(!src)
		return false;
	bool ok = paste(dst, src, w, h);
	if(!ok)
		fprintf(stderr, "[texture] raw-jar pack: %s/%s is %zux%zu, not a "
				"supported multiple of 256x256 (Pack Helper rule)\n", root,
				name, w, h);
	free(src);
	return ok;
}

uint8_t* tex_beta_compose_gui2(const char* pack_root, const char* base_root,
							   void (*service)(void)) {
	uint8_t* out = read_base(base_root, "gui_2.png");
	if(service)
		service();
	if(!out)
		return NULL;
	bool ok = paste_from(out, pack_root, "gui/gui.png", tex_beta_paste_gui);
	if(service)
		service();
	ok = ok && paste_from(out, pack_root, "gui/icons.png", tex_beta_paste_icons);
	if(service)
		service();
	if(!ok) {
		free(out);
		return NULL;
	}
	return out;
}

uint8_t* tex_beta_compose_anim(const char* pack_root,
							   const char* base_anim_path,
							   void (*service)(void)) {
	uint8_t* out = read_base(NULL, base_anim_path);
	if(service)
		service();
	if(!out)
		return NULL;
	bool ok = paste_from(out, pack_root, "terrain.png", tex_beta_paste_ice);
	if(service)
		service();
	if(!ok) {
		free(out);
		return NULL;
	}
	return out;
}
