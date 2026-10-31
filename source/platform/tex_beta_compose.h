/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Raw-jar texture packs: the load-time safety net for a player who simply
 * unzipped b1.7.3.jar into texturepacks/<name>/ instead of running the Pack
 * Helper. Such a pack has the jar's own names and lacks the four sheets the
 * helper composes or supplies, so without this the game silently mixed in
 * Earth & Ash art:
 *
 *   gui/chest.png  the jar calls it gui/container.png (helper source_names);
 *   gui_2.png      HUD sheet = bundled gui_2.png + regions of the pack's
 *                  gui/gui.png and gui/icons.png (conversion.compose_gui2);
 *   anim.png       = the shipped beta-base anim.png (project-own generator
 *                  output, R18 Q3) + the pack's terrain.png ice tile
 *                  (conversion.compose_anim);
 *   pointer.png    already bundled-only (texture.c tex_bundled_only_files):
 *                  every pack shows the shipped Homebrew hand, nothing to do.
 *
 * Every transform here is the helper's, pixel for pixel, at its default
 * "crisp" look: an HD sheet (2x/4x/8x of the 256x256 canonical) is sampled
 * NEAREST at source pixel x*k + k/2, which is exactly Pillow's NEAREST
 * reduction (tests/test_tex_beta_compose.py proves both against the helper's
 * own functions on a generated fixture jar). A sheet the helper would reject
 * (not a whole supported multiple) is rejected here too, with a log line, and
 * the bundled sheet is used -- the same outcome as before this file.
 *
 * Memory (Wii): one 256x256 RGBA result (256 KiB) plus ONE pack sheet decode
 * alive at a time; every temporary is freed before returning. No full atlas
 * is kept around. */

#ifndef TEX_BETA_COMPOSE_H
#define TEX_BETA_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tex_beta_layout.h" /* tex_beta_raw_jar_layout, tex_beta_file_exists */

#define TEX_BETA_CANON 256u
/* Where the beta-base animation sheet ships, relative to the bundled default
 * root ("assets" -> assets/beta-base/anim.png; a byte copy of
 * packs/beta-base/anim.png, same release-assets.json group). */
#define TEX_BETA_BASE_ANIM "beta-base/anim.png"

/* Integer scale k (1,2,4,8) of a w x h sheet against the 256x256 canonical,
 * under the helper's rule (whole multiple, same factor on both axes, declared
 * scale). Returns 0 when the helper would refuse the sheet. */
unsigned tex_beta_sheet_scale(size_t w, size_t h);

/* In-place pastes onto a 256x256 RGBA sheet. `src` is a pack sheet of
 * w x h; false (dst untouched) when tex_beta_sheet_scale rejects it. */
bool tex_beta_paste_gui(uint8_t* gui2, const uint8_t* gui, size_t w, size_t h);
bool tex_beta_paste_icons(uint8_t* gui2, const uint8_t* icons, size_t w,
						  size_t h);
bool tex_beta_paste_ice(uint8_t* anim, const uint8_t* terrain, size_t w,
						size_t h);

/* Full compositions, reading files. `service` (may be NULL) is called
 * between decodes (texture.c's keep-alive). Return a malloc'd 256x256 RGBA
 * image the caller owns, or NULL (logged) -> caller uses the bundled sheet.
 *   gui2: base_root/gui_2.png + pack_root/gui/gui.png + pack_root/gui/icons.png
 *   anim: base_anim_path + pack_root/terrain.png */
uint8_t* tex_beta_compose_gui2(const char* pack_root, const char* base_root,
							   void (*service)(void));
uint8_t* tex_beta_compose_anim(const char* pack_root,
							   const char* base_anim_path,
							   void (*service)(void));

#endif
