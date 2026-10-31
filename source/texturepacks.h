/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#ifndef TEXTUREPACKS_H
#define TEXTUREPACKS_H

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stddef.h>

#include "config.h"

// List installed texture packs: the subdirectories of `dir` (e.g.
// "texturepacks"). Each written out[i] is initialised by this function and
// must be string_clear()'d by the caller. The implicit "default" (bundled
// `assets`) pack is NOT included -- the caller adds it. Returns the number of
// packs written (<= max).
//
// Uses stat() to test for directories rather than dirent.d_type, so it is
// portable to the host toolchain (and therefore unit-testable headless),
// unlike screen_select_world's d_type-based scan.
size_t texturepacks_list(const char* dir, string_t* out, size_t max);

/* Do these two config values name the SAME pack? The config may hold either
 * the relative form the screen writes ("texturepacks/<name>") or the absolute
 * field-mitigation form ("sd:/texturepacks/<name>"). Compares the COMPLETE
 * roots -- there is no length limit and no buffer (REVIEW-LIBRARY-1). The
 * screen and the transactions share this one function, so "is this already my
 * selection?" cannot be answered differently in the two places. */
bool texturepacks_value_eq(const char* a, const char* b);

typedef bool (*texturepack_reload_fn)(void);

/* LEGACY entry point, unchanged on purpose (Astra gate 1 item 1): one call
 * that moves paths.texturepack AND, when the pack carries loadable audio or
 * the config has no explicit audio choice, paths.audiopack too. Still used by
 * tests/test_texture_packs.c and tests/test_pack_helper_audio.c's transaction
 * suite. New callers use the two explicit entry points below; the native
 * screen no longer uses this one.
 *
 * Apply a pack as one transaction: reload first, persist only after the
 * renderer accepted the pack, and restore the previous live/configured pack
 * if either step fails. The reload callback must read paths.texturepack from
 * `config`, as tex_init does. */
bool texturepacks_apply(struct config* config, const char* pack,
						const char* config_path,
						texturepack_reload_fn reload);

/* What a transaction did. NO_CHANGE is distinct from OK on purpose (gate 1
 * item 4): "you already have this" must not be reported as a reload that was
 * paid for, and it writes nothing at all. */
enum texturepacks_result {
	TEXTUREPACKS_OK = 0,	  /* selection moved, reloaded and persisted */
	TEXTUREPACKS_NO_CHANGE,	  /* same effective selection: no reload, no save */
	TEXTUREPACKS_INVALID,	  /* bad argument, missing/unusable directory */
	TEXTUREPACKS_RELOAD_FAILED, /* the runtime refused it; config restored */
	TEXTUREPACKS_SAVE_FAILED,	/* it loaded but could not be persisted */
};

/* An EXPLICIT texture choice: paths.texturepack ONLY.
 *
 * The one exception, and it is a migration rather than a change of choice: a
 * config whose paths.audiopack is absent or not a string has no explicit audio
 * choice, and its effective audio root is whatever paths.texturepack says. That
 * root is pinned into paths.audiopack BEFORE the texture choice moves, so the
 * player keeps hearing exactly what they heard. An audio key that IS a string
 * is never written by this function, whatever the pack ships. */
enum texturepacks_result texturepacks_apply_textures(struct config* config,
													 const char* pack,
													 const char* config_path,
													 texturepack_reload_fn reload);

/* An EXPLICIT audio choice: paths.audiopack ONLY. paths.texturepack is never
 * read as a choice and never written. `reload` is the caller's audio reload
 * (music first, then effects) and returns false to REJECT the root -- the
 * transaction then restores the exact previous JSON and re-runs the callback
 * so the previous root is live again, and nothing is persisted. */
enum texturepacks_result texturepacks_apply_audio(struct config* config,
												  const char* pack,
												  const char* config_path,
												  texturepack_reload_fn reload);

/* Does this pack ship audio that actually LOADS (>=1 file under sound/, music/
 * or records/ that the scanners could open)? This is what makes a pack
 * eligible for the Sounds list. It is a FILE-ELIGIBILITY test only: it proves
 * files are present and non-empty, NOT that every sample decodes. */
bool texturepacks_has_audio(const char* pack);

/* Does this pack CLAIM music (a music/ directory)? A pack that claims music
 * and then resolves no playable track is rejected by the audio transaction; a
 * pack that claims none is a legitimate sound-only choice with no music. */
bool texturepacks_claims_music(const char* pack);

/* Is this pack an unconverted b1.7.3.jar unzip (gui/container.png, no
 * gui/chest.png)? The loader composes the Pack Helper's missing sheets for it
 * (platform/tex_beta_compose.h); the pack list labels it as a Beta jar. */
bool texturepacks_is_raw_jar(const char* pack);

/* The name the pack list SHOWS for a pack folder, when the pack carries one:
 * the top-level "name" of its .cavewii-pack.json (written by the Pack Helper
 * and the in-game importer). Lets the importer use the space-free folder
 * "Beta-1.7.3" while the list still says "Beta 1.7.3". Returns false (out
 * untouched) when there is no manifest or no usable name; the caller then
 * shows the folder name. Printable ASCII only, truncated to out_len - 1. */
bool texturepacks_display_name(const char* pack, char* out, size_t out_len);

#endif
