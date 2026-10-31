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

/*
	THIN GX/GL draw pass for render_name_tag.c's pure quad builder -- see that
	file for every decompile citation on the geometry/billboard math itself.
	This file owns: iterating dict_entity for ENTITY_REMOTE_PLAYER, batching
	every in-range player's quads across ONE background draw + ONE two-pass
	text draw (not per-entity submits), and the GX-safe state bracket.

	STATE-DISCIPLINE PRECEDENTS MIRRORED (three):

	1. render_sign_text_draw.c's lazy-init bracket: gfx_lighting(false) is set
	   ONCE, only after the first player that actually produced a tag (an
	   empty/solo-player frame costs one dict walk and nothing else), and
	   restored to true before returning -- the same CLR0/FIFO-desync
	   discipline that file's own header comment explains in full.

	2. render_entity.c's render_entity_slime gel pass (the gfx_blending(
	   MODE_BLEND) + gfx_alpha_test(false) bracket, MANDATORY restore to
	   MODE_OFF/true right after): the background quad's alpha 0.25 and the
	   through-walls text pass's alpha 32/255 both need real alpha blending,
	   unlike sign text's always-opaque black -- so this pass is the first of
	   the two "world overlay" batches (sign text, name tags) that actually
	   needs to touch gfx_blending/gfx_alpha_test, and it follows that file's
	   own established restore convention rather than inventing a new one.

	3. [740c583 RenderLiving.renderLivingLabel]'s own two-pass text: pass 1 is
	   drawn through-walls (Beta disables GL_DEPTH_TEST entirely for the
	   background quad + this dim pass), pass 2 is the normal opaque,
	   depth-tested/depth-written finish. gfx.h's gfx_write_buffers(color,
	   depth_write, depth_test) ALREADY exposes an independent depth-TEST
	   toggle on both platforms (glEnable/glDisable(GL_DEPTH_TEST) on PC,
	   GX_SetZMode's own `enable` argument on Wii -- see gfx.c in both
	   source/platform/pc and source/platform/wii) -- so the through-walls
	   effect is `gfx_write_buffers(true, false, false)` (no new gfx.h
	   surface needed), and the opaque finish is the ordinary
	   `gfx_write_buffers(true, true, true)`.

	Draw order per Beta: background (untextured) -> text pass 1 (through
	walls, dim) -> text pass 2 (opaque). Batched ACROSS every visible player
	so there are at most 3 gfx_draw_quads_flt calls per frame regardless of
	player count (capped by NAME_TAG_RENDER_MAX), not 3 per player.

	MP visual-parity pass (2026-08-31) items 2+5 add a SECOND, SEPARATE
	sneaking-player batch (at most 2 more gfx_draw_quads_flt calls, only when
	at least one selected player is sneaking):

	  - [740c583 RenderPlayer.renderName]'s isSneaking() branch draws the
	    label DIFFERENTLY from RenderLiving.renderLivingLabel's standing
	    path -- it never disables GL_DEPTH_TEST at all (only toggles the
	    depth WRITE mask: false for the background quad, true for the text),
	    and it issues exactly ONE text draw (not two), at the SAME dim alpha
	    (0x20FFFFFF, 32/255) the standing path's throughwalls PASS 1 uses --
	    literally reused rather than the opaque -1 pass 2 uses. Verified
	    directly against the decompile (not assumed): this genuinely is
	    Beta's own behaviour, not a copy-paste bug this port is choosing to
	    fix -- "Beta 1.7.3 parity FIRST" means porting it exactly, faint
	    look and all.
	  - The 32-block distance cap (vs. standing's 64) lives in
	    render_name_tag.c's own is_sneaking parameter -- this file only
	    decides WHICH draw-pass shape to use per player and reads
	    e->data.remote_player.sneaking directly to split the two groups.
	  - Standing and sneaking players share the SAME static output buffers
	    (built and drawn sequentially, standing group first) rather than
	    doubling the already-bounded-resource BSS footprint documented below.

	Bounded-resource note (item 5): NAME_TAG_RENDER_MAX caps how many
	ENTITY_REMOTE_PLAYER tags can be built and drawn in a single frame --
	raised 8 -> 16 (2026-08-31) after the field report that a 9th+ concurrent
	player's tag silently vanished (dict-iteration-order cutoff, not a
	distance one). Static per-frame buffers below (the boot-death-bisect
	2026-08-26 64KB-static-hygiene precedent -- this pass runs on the main
	render thread only, so static reuse across frames is safe) size to this
	cap: measured at MAX=16, text_verts+bg_verts+the six pos/uv/col arrays
	total ~46.7KB BSS (vs. ~23.4KB at the old MAX=8) -- verified by exact
	byte-count, not estimated: NAME_TAG_MAX_TEXT_VERTS=64,
	sizeof(name_tag_vertex)=20B, NAME_TAG_BG_VERTS=4; text_verts
	16*64*20=20480B, bg_verts 16*4*20=1280B, text pos/uv/col
	16*64*(12+8+4)=24576B, bg pos/uv/col 16*4*(12+8+4)=1536B; sum 47872B.
	Selection is now NEAREST-K by distance (a small O(dict_size *
	NAME_TAG_RENDER_MAX) partial top-K scan against a 16-entry stack array,
	"small N, trivial") rather than first-K-in-dict-iteration-order, so a
	9th+ player only drops out of the tag pass when a CLOSER 17th+ player
	displaces them, not merely by existing later in the dict.
*/

#include <assert.h>
#include <string.h>

#include "../game/camera.h"
#include "../platform/gfx.h"
#include "../platform/texture.h"
#include "gui_util.h"
#include "render_name_tag.h"
#include "render_name_tag_draw.h"

// Per-frame ceiling -- see this file's header "Bounded-resource note" above
// for the exact BSS byte-count at this value and the field-report rationale
// for raising it from the original 8.
#define NAME_TAG_RENDER_MAX 16

struct name_tag_candidate {
	struct entity* e;
	vec3 pos; // render-interpolated (pos_old->pos lerp), computed once here
			 // and reused by both the selection and the quad builder.
	float dist2;
};

// Nearest-K partial selection over every ENTITY_REMOTE_PLAYER in `entities`:
// O(dict_size * NAME_TAG_RENDER_MAX) worst case -- "small N, trivial" per
// this file's header note. Replaces the old first-K-in-dict-order cutoff.
static size_t select_nearest_players(dict_entity_t entities,
									 const vec3 camera_pos, float tick_delta,
									 struct name_tag_candidate out[NAME_TAG_RENDER_MAX]) {
	size_t count = 0;
	size_t farthest_idx = 0; // valid only once count == NAME_TAG_RENDER_MAX

	dict_entity_it_t it;
	dict_entity_it(it, entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;

		if(e->type == ENTITY_REMOTE_PLAYER) {
			vec3 p;
			glm_vec3_lerp(e->pos_old, e->pos, tick_delta, p);
			float dx = p[0] - camera_pos[0];
			float dy = p[1] - camera_pos[1];
			float dz = p[2] - camera_pos[2];
			float dist2 = dx * dx + dy * dy + dz * dz;

			if(count < NAME_TAG_RENDER_MAX) {
				out[count].e = e;
				glm_vec3_copy(p, out[count].pos);
				out[count].dist2 = dist2;
				count++;
				if(count == NAME_TAG_RENDER_MAX) {
					farthest_idx = 0;
					for(size_t k = 1; k < NAME_TAG_RENDER_MAX; k++)
						if(out[k].dist2 > out[farthest_idx].dist2)
							farthest_idx = k;
				}
			} else if(dist2 < out[farthest_idx].dist2) {
				out[farthest_idx].e = e;
				glm_vec3_copy(p, out[farthest_idx].pos);
				out[farthest_idx].dist2 = dist2;
				farthest_idx = 0;
				for(size_t k = 1; k < NAME_TAG_RENDER_MAX; k++)
					if(out[k].dist2 > out[farthest_idx].dist2)
						farthest_idx = k;
			}
		}

		dict_entity_next(it);
	}

	return count;
}

// Shared per-batch static accumulators, REUSED sequentially for the
// standing group then the sneaking group (see this file's header comment --
// avoids doubling the already-bounded BSS footprint).
static struct name_tag_vertex
	s_text_verts[NAME_TAG_RENDER_MAX * NAME_TAG_MAX_TEXT_VERTS];
static struct name_tag_vertex s_bg_verts[NAME_TAG_RENDER_MAX * NAME_TAG_BG_VERTS];

// Builds one batch (either the standing group or the sneaking group,
// selected by `want_sneaking`) from `candidates[0..count)` into the shared
// static accumulators above. Returns the TEXT vertex count and, via
// `out_players`, how many players actually contributed a tag (bg_count ==
// *out_players * NAME_TAG_BG_VERTS).
static size_t build_batch(const struct name_tag_candidate* candidates,
						  size_t count, bool want_sneaking,
						  const vec3 camera_pos, float camera_rx,
						  float camera_ry, const uint8_t* glyph_width,
						  size_t* out_players) {
	size_t text_count = 0;
	size_t players = 0;

	for(size_t i = 0; i < count; i++) {
		struct entity* e = candidates[i].e;
		if(e->data.remote_player.sneaking != want_sneaking)
			continue;

		struct name_tag_vertex text_out[NAME_TAG_MAX_TEXT_VERTS];
		struct name_tag_vertex bg_out[NAME_TAG_BG_VERTS];
		size_t n = name_tag_build_quads(
			e->data.remote_player.username, candidates[i].pos, camera_pos,
			camera_rx, camera_ry, glyph_width, want_sneaking, text_out,
			bg_out);

		if(n > 0) {
			memcpy(&s_text_verts[text_count], text_out, n * sizeof(*text_out));
			memcpy(&s_bg_verts[players * NAME_TAG_BG_VERTS], bg_out,
				  NAME_TAG_BG_VERTS * sizeof(*bg_out));
			text_count += n;
			players++;
		}
	}

	*out_players = players;
	return text_count;
}

// Draws whatever this call filled into s_text_verts/s_bg_verts. `sneaking`
// selects the pass SHAPE: standing gets Beta's normal background
// (through-walls) + two-pass text (dim through-walls, then opaque
// depth-tested); sneaking gets the depth-tested background + Beta's own
// SINGLE dim-alpha depth-tested text pass -- see this file's header comment
// for the exact [740c583 RenderPlayer.renderName] citation.
static void draw_batch(size_t text_count, size_t players, bool sneaking) {
	if(text_count == 0)
		return;

	size_t bg_count = players * NAME_TAG_BG_VERTS;

	// --- Background: untextured. ---
	{
		static float pos[NAME_TAG_RENDER_MAX * NAME_TAG_BG_VERTS * 3];
		static float uv[NAME_TAG_RENDER_MAX * NAME_TAG_BG_VERTS * 2];
		static uint8_t col[NAME_TAG_RENDER_MAX * NAME_TAG_BG_VERTS * 4];
		for(size_t i = 0; i < bg_count; i++) {
			pos[i * 3 + 0] = s_bg_verts[i].x;
			pos[i * 3 + 1] = s_bg_verts[i].y;
			pos[i * 3 + 2] = s_bg_verts[i].z;
			uv[i * 2 + 0] = 0.0f;
			uv[i * 2 + 1] = 0.0f;
			// [RenderLiving.renderLivingLabel]/[RenderPlayer.renderName]:
			// setColorRGBA_F(0,0,0,0.25F) either way.
			col[i * 4 + 0] = 0;
			col[i * 4 + 1] = 0;
			col[i * 4 + 2] = 0;
			col[i * 4 + 3] = 64; // round(0.25F * 255)
		}
		gfx_texture(false);
		// Standing: through walls, no depth write/test [740c583
		// RenderLiving.renderLivingLabel]. Sneaking: depth TEST stays on,
		// only the WRITE is off [740c583 RenderPlayer.renderName's crouch
		// branch never disables GL_DEPTH_TEST].
		gfx_write_buffers(true, false, sneaking);
		gfx_draw_quads_flt(bg_count, pos, col, uv);
		gfx_texture(true);
	}

	gfx_bind_texture(&texture_font);

	static float pos[NAME_TAG_RENDER_MAX * NAME_TAG_MAX_TEXT_VERTS * 3];
	static float uv[NAME_TAG_RENDER_MAX * NAME_TAG_MAX_TEXT_VERTS * 2];
	static uint8_t col[NAME_TAG_RENDER_MAX * NAME_TAG_MAX_TEXT_VERTS * 4];
	for(size_t i = 0; i < text_count; i++) {
		pos[i * 3 + 0] = s_text_verts[i].x;
		pos[i * 3 + 1] = s_text_verts[i].y;
		pos[i * 3 + 2] = s_text_verts[i].z;
		uv[i * 2 + 0] = s_text_verts[i].u;
		uv[i * 2 + 1] = s_text_verts[i].v;
	}

	if(sneaking) {
		// [740c583 RenderPlayer.renderName]: ONE depth-tested text draw at
		// the SAME dim alpha (32/255, 0x20FFFFFF) the standing path's
		// through-walls pass uses -- literally reused, not opaque -1. Byte-
		// faithful, not a bug this port is "fixing".
		for(size_t i = 0; i < text_count; i++) {
			col[i * 4 + 0] = 255;
			col[i * 4 + 1] = 255;
			col[i * 4 + 2] = 255;
			col[i * 4 + 3] = 32;
		}
		gfx_write_buffers(true, true, true);
		gfx_draw_quads_flt(text_count, pos, col, uv);
		return;
	}

	// --- Standing: text pass 1, through walls, dim white (alpha 32/255). ---
	for(size_t i = 0; i < text_count; i++) {
		col[i * 4 + 0] = 255;
		col[i * 4 + 1] = 255;
		col[i * 4 + 2] = 255;
		col[i * 4 + 3] = 32;
	}
	gfx_write_buffers(true, false, false); // still through walls
	gfx_draw_quads_flt(text_count, pos, col, uv);

	// --- Standing: text pass 2, normal depth test, opaque, depth write on. ---
	for(size_t i = 0; i < text_count; i++)
		col[i * 4 + 3] = 255;
	gfx_write_buffers(true, true, true);
	gfx_draw_quads_flt(text_count, pos, col, uv);
}

void render_name_tag_draw_pass(dict_entity_t entities, struct camera* c,
							   float tick_delta) {
	assert(c);

	// Field-defect investigation (2026-08-30 maintainer live Wii-vs-PC MP
	// session, PROJECT_MEMORY.md "NO NAME TAGS over remote players on the
	// Wii"): render_weather.c's task_cc056031 fix (5ef2add, same MP visual-
	// parity batch as this pass) already pinned the exact failure CLASS this
	// guard closes -- tex_gfx_bind's own documented contract (source/
	// platform/wii/texture.c) is that binding a tex_gfx whose .data never
	// loaded is a SILENT no-op that leaves whatever texture the PRIOR draw
	// call in the frame left bound, rather than failing loudly or skipping.
	// Every other gfx_draw_quads_flt-driven overlay this codebase ships
	// (rain/snow, entity_shadow) now self-guards on its own texture's
	// .data before emitting a single quad; this pass previously did not,
	// so a texture_font load failure (a real possibility on the Wii's much
	// tighter MEM2 budget vs. PC's effectively unlimited VRAM -- the asset
	// is core-shipped and GUI text proves it normally loads, but "normally"
	// is not "always" under memory pressure) would silently submit glyph-
	// shaped quads textured with WHATEVER the last entity/particle draw left
	// bound instead of cleanly drawing nothing -- exactly the "PC-only font/
	// texture path... no-ops on GX" shape this investigation was asked to
	// rule in/out. Root-cause note: extensive code review plus a new
	// end-to-end regression test (tests/test_name_tag_draw_pass.c, driving
	// this exact production entry point through select_nearest_players/
	// build_batch/draw_batch against a recorder gfx layer) found the
	// selection/dispatch logic itself platform-agnostic and correct on both
	// PC and Wii builds of this SAME source file -- no #ifdef, no early
	// return, no entity-kind gate anywhere in the chain. This guard is
	// applied as defense-in-depth against the one concrete, precedented
	// failure class this codebase has already hit once for an identical
	// symptom (rain), not as a confirmed mechanism for this specific report.
	if(!texture_font.data)
		return; // never loaded -- draw nothing rather than a leftover texture

	const uint8_t* glyph_width = gutil_font_glyph_widths();
	vec3 camera_pos = {c->x, c->y, c->z};

	struct name_tag_candidate candidates[NAME_TAG_RENDER_MAX];
	size_t candidate_count
		= select_nearest_players(entities, camera_pos, tick_delta, candidates);
	if(candidate_count == 0)
		return; // no player in range this frame -- zero state touched

	size_t standing_players, sneaking_players;
	size_t standing_text = build_batch(candidates, candidate_count, false,
									   camera_pos, c->rx, c->ry, glyph_width,
									   &standing_players);
	// Re-run build_batch for the sneaking group AFTER reading out the
	// standing group's counts (both draw calls below read s_text_verts/
	// s_bg_verts fresh each time -- build immediately before each draw).
	if(standing_text == 0) {
		size_t sneaking_text = build_batch(candidates, candidate_count, true,
										   camera_pos, c->rx, c->ry,
										   glyph_width, &sneaking_players);
		if(sneaking_text == 0)
			return; // every selected player was out of range for their cap
		gfx_matrix_modelview(c->view);
		gfx_lighting(false);
		gfx_blending(MODE_BLEND);
		gfx_alpha_test(false);
		draw_batch(sneaking_text, sneaking_players, true);
	} else {
		gfx_matrix_modelview(c->view);
		gfx_lighting(false);
		gfx_blending(MODE_BLEND);
		gfx_alpha_test(false);
		draw_batch(standing_text, standing_players, false);

		size_t sneaking_text = build_batch(candidates, candidate_count, true,
										   camera_pos, c->rx, c->ry,
										   glyph_width, &sneaking_players);
		draw_batch(sneaking_text, sneaking_players, true);
	}

	// Mandatory restore (precedent 2 above) -- never leak blend/alpha-test
	// state into the next world/entity/GUI draw in this frame.
	gfx_blending(MODE_OFF);
	gfx_alpha_test(true);
	gfx_lighting(true);
}
