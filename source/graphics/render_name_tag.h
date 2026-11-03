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

#ifndef RENDER_NAME_TAG_H
#define RENDER_NAME_TAG_H

/*
	Pure geometry for Beta 1.7.3 remote-player name tags -- [740c583
	RenderPlayer.renderName + RenderLiving.renderLivingLabel]. NO GL/GX calls
	live in this file, mirroring render_sign_text.h/.c's split exactly: this
	module is pure math (golden-tested with no rendering context at all) and
	the GX/GL-facing pass lives in the SIBLING file render_name_tag_draw.c,
	which iterates dict_entity, calls this builder per ENTITY_REMOTE_PLAYER,
	and submits the batched result under the state-discipline bracket that
	file documents.

	Decompile chain this function bakes, quoted in call order (OpenGL
	post-multiplies the CURRENT matrix on each successive call, exactly like
	the sign-text and held-item precedents already established in this
	codebase -- see render_sign_text.c's header comment for the same idiom):

		GL11.glPushMatrix();
		GL11.glTranslatef(x, y, z);
		// x,y,z = entity pos + 2.3F up ("sleeping ? y-1.5F : y+2.3F" --
		// CaveWii does not track a sleep state on ENTITY_REMOTE_PLAYER, so
		// this port always takes the +2.3F branch; see the header comment on
		// name_tag_build_quads for the citation).
		GL11.glNormal3f(0.0F, 1.0F, 0.0F);
		GL11.glRotatef(-renderManager.playerViewY, 0.0F, 1.0F, 0.0F);
		GL11.glRotatef(renderManager.playerViewX, 1.0F, 0.0F, 0.0F);
		GL11.glScalef(-0.02666667F, -0.02666667F, 0.02666667F);
		// (lighting off / depthMask off / blend on -- draw pass's job)
		// Background (untextured, alpha 0.25 black):
		tessellator.addVertex(-i-1, -1, 0.0D);
		tessellator.addVertex(-i-1,  8, 0.0D);
		tessellator.addVertex( i+1,  8, 0.0D);
		tessellator.addVertex( i+1, -1, 0.0D);
		// where i = fontrenderer.getStringWidth(str) / 2 (Java int division).
		fontrenderer.drawString(str, -fontrenderer.getStringWidth(str)/2, 0,
								0x20FFFFFF); // pass 1: alpha 32/255, no depth
											 // test/write -- visible through
											 // walls
		GL11.glDepthMask(true);
		fontrenderer.drawString(str, -fontrenderer.getStringWidth(str)/2, 0,
								-1); // pass 2: opaque white, normal depth
		GL11.glPopMatrix();

	playerViewY/playerViewX are Beta's OWN camera yaw/pitch (radians here,
	NOT the label's target entity's orientation -- the tag always faces the
	VIEWER). At this engine's send-path boundary (client_interface.c's
	outgoing SRPC_PLAYER_POS): Beta_yaw_deg = -glm_deg(camera.rx),
	Beta_pitch_deg = glm_deg(camera.ry) - 90.0F [see
	network/client_interface.c ~line 3164]. In radians that is
	Beta_yaw = -camera_rx and Beta_pitch = camera_ry - PI/2, so:

		-playerViewY == -(-camera_rx)        == +camera_rx
		+playerViewX == +(camera_ry - PI/2)  ==  camera_ry - PI/2

	render_name_tag.c's build_model_matrix applies exactly those two
	rotations, in the same Y-then-X call order as the decompile above.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../cglm/cglm.h"

// One glyph-quad or background-quad corner, LABEL-LOCAL: the caller-supplied
// entity/camera position and orientation are already folded into (x,y,z) --
// see the header comment above -- so nothing further needs to be added by
// the draw pass (unlike render_sign_text.c's block-local convention, where
// the draw pass still adds the sign's integer world position; a name tag's
// anchor is a float position that varies every frame, so it is baked in
// here instead, same distance-cull short-circuit style as sign text's
// blank-grid return-0). UV is normalized to [0,1] over texture_font's
// 256-texel atlas, same 16x16-cell layout as render_sign_text.c and
// source/graphics/gui_util.c's gutil_text_rgb; unused (0,0) for background
// verts, which the draw pass submits untextured.
struct name_tag_vertex {
	float x, y, z;
	float u, v;
	// Beta colour code in force for this glyph (0xA7 + selector,
	// beta_text_color.h): `coded` false = the pass's own colour; true = r,g,b
	// at full alpha in every pass (name_tag_text_color).
	bool coded;
	uint8_t r, g, b;
};

// entity_remote_player's username[17] caps at 16 visible chars + NUL.
#define NAME_TAG_MAX_CHARS 16
#define NAME_TAG_MAX_TEXT_VERTS (NAME_TAG_MAX_CHARS * 4)
#define NAME_TAG_BG_VERTS 4

// [740c583 RenderPlayer.renderName]: the distance from the
// viewer (getDistanceToEntity) must be below 32.0F for a sneaking player,
// else 64.0F -- a non-squared distance compare against 32 or
// 64 blocks depending on sneak state (mathematically identical to this
// file's own dist² < cap² test, since distances are non-negative). MP
// visual-parity pass (2026-08-31) item 2: ENTITY_REMOTE_PLAYER now tracks
// sneak state (CRPC_ENTITY_FLAGS, entity.h's entity_remote_player.sneaking
// field) -- see name_tag_build_quads's own is_sneaking parameter.
#define NAME_TAG_RENDER_DISTANCE 64.0f
#define NAME_TAG_RENDER_DISTANCE_SNEAK 32.0f

// [740c583 RenderPlayer.renderName]: the label anchor is entity pos + 2.3F
// (sleeping targets use -1.5F instead). ENTITY_REMOTE_PLAYER has no tracked
// sleep state either (no equivalent of a "renderSleep"/bed-mount flag on
// this port's remote-player entity data), so this port always takes the
// +2.3F branch -- same documented-gap treatment as the sneak cap above.
#define NAME_TAG_EYE_OFFSET 2.3f

// [740c583]: `GL11.glScalef(-0.02666667F, -0.02666667F, 0.02666667F);`
#define NAME_TAG_SCALE 0.02666667f

/*
	Build the WORLD-SPACE quads for one remote player's name tag: up to
	NAME_TAG_MAX_TEXT_VERTS text (glyph) vertices in text_out, plus exactly
	NAME_TAG_BG_VERTS background-quad vertices in bg_out.

	username: NUL-terminated, at most NAME_TAG_MAX_CHARS visible chars
	          (entity_remote_player.username's own cap -- excess is
	          truncated defensively, though the field is never longer).
	entity_pos: the remote player's (lerped) render position this frame --
	          NAME_TAG_EYE_OFFSET is added internally, matching Beta's own
	          x,y+2.3F,z translate.
	camera_pos: the viewer's eye position, for the distance cull only (the
	          billboard rotation below depends on camera_rx/camera_ry, not
	          camera_pos).
	camera_rx, camera_ry: the viewer's OWN camera angles (game/camera.h's
	          convention: rx radians, ry radians with GLM_PI_2 == level) --
	          see the header comment above for the exact -playerViewY /
	          +playerViewX conversion this function applies.
	glyph_width: 256-entry ink-width table, same convention as
	          render_sign_text.h's glyph_width parameter (pass
	          gutil_font_glyph_widths() in production; golden tests inject a
	          synthetic table).
	is_sneaking: MP visual-parity pass item 2 -- halves the distance cap to
	          NAME_TAG_RENDER_DISTANCE_SNEAK (32 blocks) per [740c583
	          RenderPlayer.renderName]. The geometry itself (quad positions/
	          UVs) is unaffected -- only the draw PASS differs while
	          sneaking (single depth-tested pass, no through-walls dim pass),
	          which is the caller's job (render_name_tag_draw.c reads
	          e->data.remote_player.sneaking directly for that decision).
	text_out/bg_out: caller-owned, sized exactly NAME_TAG_MAX_TEXT_VERTS and
	          NAME_TAG_BG_VERTS.

	Returns the number of TEXT vertices written (always a multiple of 4, one
	quad per non-space glyph -- see render_sign_text.c's identical space-skip
	precedent). Returns 0 (bg_out left untouched) when username is empty, or
	when the entity is farther than the applicable distance cap from
	camera_pos -- the draw pass's cue to skip this player's tag entirely,
	same early-out contract as sign_text_build_quads's blank-grid return-0.
*/
size_t name_tag_build_quads(const char* username, const vec3 entity_pos,
							const vec3 camera_pos, float camera_rx,
							float camera_ry, const uint8_t glyph_width[256],
							bool is_sneaking,
							struct name_tag_vertex text_out[NAME_TAG_MAX_TEXT_VERTS],
							struct name_tag_vertex bg_out[NAME_TAG_BG_VERTS]);

// The submitted colour of one text vertex in a pass whose base colour is
// white at `pass_alpha` (32 for Beta's 0x20FFFFFF through-walls / sneaking
// pass, 255 for the -1 pass). [mcb173-retromcp FontRenderer.java:109,142,
// 155] a colour code calls glColor3f, which sets alpha to 1.0, so a coded
// glyph is its code colour at alpha 255 in EVERY pass.
void name_tag_text_color(const struct name_tag_vertex* v, uint8_t pass_alpha,
						 uint8_t out[4]);

#endif
