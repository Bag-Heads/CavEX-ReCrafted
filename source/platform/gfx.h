/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "../cglm/cglm.h"
#include "texture.h"

enum gfx_blend {
	MODE_BLEND,
	MODE_BLEND2,
	MODE_BLEND3,
	MODE_INVERT,
	MODE_OFF,
};

enum depth_func {
	MODE_LEQUAL,
	MODE_EQUAL,
};

enum cull_func {
	MODE_NONE,
	MODE_BACK,
	MODE_FRONT,
};

bool gfx_setup(void);
// `daytime` is daytime_brightness()'s clamped sun factor; rain/thunder are the
// world's CURRENT weather strengths (0 in the Nether -- main.c gates), which add to
// Beta's integer sky-subtract so a storm dims the whole world. See daytime.h.
void gfx_update_light(float daytime, float rain_strength, float thunder_strength,
					  const float* light_lookup);
float gfx_lookup_light(uint8_t light);
void gfx_finish(bool vsync);
void gfx_flip_buffers(float* gpu_wait, float* vsync_wait);
/* Fence all submitted commands, including an unfinished frame, without
 * rotating buffers. Wii retirement also accepts a synchronous pipeline-abort
 * witness; a timeout alone never releases GPU-owned storage. */
void gfx_wait_idle(void);
/* True once a frame can really be drawn and presented (display + draw buffer
   up). False in the headless unit tests that drive GUI update code with no
   window at all -- an out-of-loop present (loading_notice.c, the pack-switch
   sound phase) must be skipped there, never crash. Always true in the app:
   main() exits when gfx_setup fails. */
bool gfx_can_present(void);
/* Block until the next vertical retrace (Wii: VIDEO_WaitVSync; PC: no-op).
   For out-of-loop presents that must not touch a buffer the display is still
   scanning out -- see loading_notice_present_boot. */
void gfx_wait_vsync(void);
struct gfx_wait_timing { uint32_t vi_ready_us, gpu_poll_late_us; };
/* Main-thread call: returns the last flip and arms diagnostic timestamps for
 * subsequent flips. Normal play leaves the instrumentation dormant. */
struct gfx_wait_timing gfx_frame_wait_timing(bool enabled);
void gfx_bind_texture(struct tex_gfx* tex);
void gfx_clear_buffers(uint8_t r, uint8_t g, uint8_t b);
int gfx_width(void);
int gfx_height(void);

void gfx_copy_framebuffer(uint8_t* dest, size_t* width, size_t* height);
/* Wii only: banded raw RGBA dump of the EFB (debug capture, see wii/gfx.c). */
bool gfx_write_framebuffer_raw(const char* path);
/* BG-BAKE-1028: read back the colour buffer of the frame currently being
 * drawn (before any present) as RGBA8, top-down, in bands of rows handed to
 * `sink(user, rgba, y0, rows, width, height)`; the sink returns false to
 * cancel. Scratch is allocated per call (one band, ~160 KiB at 640 wide)
 * and released before return. Whatever the outcome -- success, cancel,
 * allocation or GPU-wait failure -- the colour and depth buffers are then
 * reset to the current clear colour / far depth (Wii: copy-with-clear of
 * every band; PC: glClear with scissor off), so the caller can draw the rest
 * of the frame as if nothing had been rendered. Returns true only when every
 * row was delivered. */
typedef bool (*gfx_capture_rows_fn)(void* user, const uint8_t* rgba,
	size_t y0, size_t rows, size_t width, size_t height);
bool gfx_capture_frame_rows(gfx_capture_rows_fn sink, void* user);

void gfx_matrix_projection(mat4 proj, bool is_perspective);
void gfx_matrix_modelview(mat4 mv);
void gfx_matrix_texture(bool enable, mat4 tex);

void gfx_mode_world(void);
void gfx_mode_gui(void);
void gfx_mode_gui_viewport(int width, int height);
void gfx_fog_color(uint8_t r, uint8_t g, uint8_t b);
void gfx_fog_pos(float dx, float dz, float distance);
void gfx_fog(bool enable);
// Enable the per-vertex biome tint channel for the chunk render pass. On Wii
// this switches chunk meshes to the 2-colour-channel format + tint TEV stage;
// on PC it is a no-op (the shader/displaylist handle the tint per list). Bracket
// the world chunk render with gfx_tint(true)/gfx_tint(false).
void gfx_tint(bool enable);
// Emissive colour-tint overlay for textured ENTITY draws (audit #17f): lerp the
// drawn pixels toward (r,g,b) by `amount` in [0,1]. amount==0 renders the model
// EXACTLY as today (graceful degrade -- no extra GX TEV stage / no GL mix), so a
// backend issue can only drop the overlay, never the model. Bracket each tinted mob
// draw: gfx_entity_tint(R,G,B,t) ... draw ... gfx_entity_tint(0,0,0,0). On Wii this
// adds a KONST/register-colour TEV stage that needs NO vertex-format/descriptor
// change (avoiding the CLR0-desync FIFO-hang class) and fully restores GX state when
// amount==0. On PC it sets the fragment-shader tint uniforms. r,g,b are 0..1.
void gfx_entity_tint(float r, float g, float b, float amount);
// Per-channel MULTIPLY overlay for entity draws (Beta fleece-colour tint = wool*colour per
// channel). Byte-exact for ALL colours where gfx_entity_tint's scalar lerp only matches the
// neutral (R==G==B) subset. (1,1,1) is the identity/reset; FIFO-safe (a register-colour TEV
// stage, no vertex-descriptor change), and gfx_entity_tint(.,.,.,0) also fully resets it on GX.
void gfx_entity_modulate(float r, float g, float b);
void gfx_blending(enum gfx_blend mode);
void gfx_alpha_test(bool enable);
// Alpha test with an explicit GREATER-than reference in [0,1] (Beta's
// glAlphaFunc(GL_GREATER, ref)). gfx_alpha_test(bool) keeps its own fixed
// threshold (~0.0625, GEQUAL) for every existing caller unchanged; this is
// purely additive, for callers (precipitation render) that need one of
// Beta's OWN non-default thresholds. Always sets BOTH the enable flag and
// the reference in one call (no partial-state leak between callers) --
// gfx_alpha_test(bool) itself also re-asserts its fixed reference every call
// for the same reason. enable=false disables the test; `ref` is then unused.
void gfx_alpha_test_ref(bool enable, float ref);
void gfx_write_buffers(bool color, bool depth, bool depth_test);
// Set the active render viewport in gfx_width()/gfx_height() logical coordinates,
// with a top-left origin on both backends. Wii converts to physical EFB pixels and caches the rect so
// gfx_depth_range re-applies the current viewport (GX folds depth range into
// GX_SetViewport) instead of resetting to full-screen -- required the day any
// second viewport exists (splitscreen/PiP). PC maps to glViewport.
void gfx_set_viewport(float x, float y, float w, float h);
void gfx_depth_range(float near, float far);
void gfx_depth_func(enum depth_func func);
void gfx_texture(bool enable);
void gfx_lighting(bool enable);
void gfx_cull_func(enum cull_func func);
// Clip rectangle, in ABSOLUTE framebuffer pixels with a TOP-LEFT origin:
// x grows right, y grows DOWN from the top of the screen, and NO viewport
// offset is applied (the rect is screen-absolute even while a partial
// viewport is bound). Both backends honour this contract -- the Wii maps it
// onto the EFB top-origin scissor, the PC flips it to GL's bottom origin --
// so callers never branch on the platform. Rects are clipped to the
// framebuffer; enable=false disables clipping and ignores the rectangle.
void gfx_scissor(bool enable, uint32_t x, uint32_t y, uint32_t width,
				 uint32_t height);
void gfx_draw_lines(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors);
void gfx_draw_quads(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors, const uint16_t* texcoords);
void gfx_draw_quads_flt(size_t vertex_count, const float* vertices,
						const uint8_t* colors, const float* texcoords);

#endif
