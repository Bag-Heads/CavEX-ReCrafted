/*
	gutil_clouds' GX state contract (audit finding, 2026-09-02 followups):
	gutil_clouds MUST bracket its gfx_draw_quads calls with
	gfx_lighting(false) ... gfx_lighting(true), matching every other
	immediate-mode draw pass in this render layer (render_rain_snow,
	render_name_tag_draw_pass, render_sign_text_draw, both render_item.c
	paths, and -- since 95159f6 -- gutil_sky_box in this SAME file).

	On Wii, gfx_lighting toggles the GLOBAL GX vertex descriptor for CLR0
	between INDEX8 (world/entity display lists) and DIRECT (immediate RGBA
	quads). gfx_draw_quads pushes 4 direct colour bytes per vertex, so a
	caller that leaves CLR0=INDEX8 armed feeds the CP a stream it parses
	expecting a 1-byte index -- FIFO desync, the documented GP-wedge hang
	class (render_item.c, render_weather.c, and gutil_sky_box's own prior
	bug, all cited in gfx_util.c's gutil_sky_box comment).

	Prior to this fix, gutil_clouds called gfx_lighting(false) at its top
	and never restored it -- the mirror image of the bug 95159f6 fixed in
	gutil_sky_box (same file, the function right below this one). Unlike
	gutil_sky_box, gutil_clouds' sole caller (main.c) is IMMEDIATELY
	followed by gfx_mode_gui(), which itself unconditionally re-asserts
	gfx_lighting(false) -- so the omission produced no observable symptom
	in the current call graph (confirmed by reading every gutil_clouds call
	site: exactly one, main.c:2055, with gfx_mode_gui() as the very next
	statement). That is still the same accidental-ordering hazard gutil_sky_box
	had before 95159f6: correctness depended on what happens to run next,
	not on the function's own contract. This test locks the explicit bracket
	so a future caller (e.g. inserting a debug/HUD draw between clouds and
	gfx_mode_gui) can't silently reintroduce the desync.

	Links gfx_util.c against a RECORDER gfx layer (no GL/GX), mirroring
	tests/test_precip_gx_state.c's isolation pattern.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/game_state.h"
#include "graphics/gfx_util.h"
#include "platform/gfx.h"
#include "platform/texture.h"

// --- the globals gfx_util.c references ---------------------------------
struct game_state gstate;
struct block* blocks[256];
struct tex_gfx texture_clouds;
struct tex_gfx texture_sun;
struct tex_gfx texture_moon;

static unsigned char command_bytes[1024 * 1024], expected_bytes[1024 * 1024];
static size_t command_size;
static void record_bytes(const void* p, size_t size) {
	assert(command_size + size <= sizeof(command_bytes));
	memcpy(command_bytes + command_size, p, size);
	command_size += size;
}
#define RECORD(value) record_bytes(&(value), sizeof(value))

// --- recorder gfx layer --------------------------------------------------
enum ev { EV_LIGHT_OFF, EV_LIGHT_ON, EV_DRAW };
static enum ev s_log[8192];
static size_t s_log_n;
static void rec(enum ev e) {
	assert(s_log_n < sizeof(s_log) / sizeof(s_log[0]));
	s_log[s_log_n++] = e;
	RECORD(e);
}
static void log_reset(void) {
	s_log_n = 0;
	command_size = 0;
}
static size_t log_count(enum ev e) {
	size_t n = 0;
	for(size_t i = 0; i < s_log_n; i++)
		if(s_log[i] == e)
			n++;
	return n;
}

void gfx_lighting(bool enable) {
	rec(enable ? EV_LIGHT_ON : EV_LIGHT_OFF);
}
void gfx_draw_quads(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors, const uint16_t* texcoords) {
	RECORD(vertex_count);
	record_bytes(vertices, vertex_count * 3 * sizeof(*vertices));
	record_bytes(colors, vertex_count * 4);
	record_bytes(texcoords, vertex_count * 2 * sizeof(*texcoords));
	rec(EV_DRAW);
}
// State/draw calls the contract under test does not cover: accepted silently.
void gfx_draw_quads_flt(size_t vertex_count, const float* vertices,
						const uint8_t* colors, const float* texcoords) {
	RECORD(vertex_count);
	record_bytes(vertices, vertex_count * 3 * sizeof(*vertices));
	record_bytes(colors, vertex_count * 4);
	record_bytes(texcoords, vertex_count * 2 * sizeof(*texcoords));
}
void gfx_draw_lines(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors) {
	(void)vertex_count;
	(void)vertices;
	(void)colors;
}
void gfx_cull_func(enum cull_func func) { RECORD(func); }
void gfx_blending(enum gfx_blend mode) { RECORD(mode); }
void gfx_alpha_test(bool enable) { RECORD(enable); }
void gfx_alpha_test_ref(bool enable, float ref) { (void)enable; (void)ref; }
void gfx_matrix_projection(mat4 proj, bool is_perspective) {
	(void)proj;
	(void)is_perspective;
}
void gfx_texture(bool enable) { RECORD(enable); }
void gfx_fog(bool enable) { RECORD(enable); }
void gfx_fog_pos(float dx, float dz, float distance) {
	RECORD(dx); RECORD(dz); RECORD(distance);
}
void gfx_write_buffers(bool color, bool depth, bool depth_test) {
	RECORD(color); RECORD(depth); RECORD(depth_test);
}
void gfx_matrix_modelview(mat4 mv) { record_bytes(mv, sizeof(mat4)); }
void gfx_bind_texture(struct tex_gfx* tex) { RECORD(tex); }

// gutil_sky_box (same TU, not exercised here) calls this.
float daytime_get_time(void) {
	return 6000.0f;
}

// Every cloud cell reads opaque, so the full 33x33 grid draws on both passes
// (depth-only + colour) -- the bracket must span every one of those draws.
void tex_gfx_lookup(struct tex_gfx* tex, int x, int y, uint8_t* color) {
	(void)tex;
	RECORD(x); RECORD(y);
	color[0] = color[1] = color[2] = 0xFF;
	color[3] = 0xFF; // opaque
}

#include "daytime.h"
#include "support/clouds_render_reference.h"

static void test_owned_cloud_bytes(void) {
	const float coords[] = {-511.75F, -12.0F, -.5F, 0, 12.0F, 241.5F};
	ptime_t now = time_get();
	for(unsigned x = 0; x < 6; x++) for(unsigned z = 0; z < 6; z++) {
		struct camera camera = {.x=coords[x],.y=70,.z=coords[z]};
		mat4 view;
		glm_translate_make(view, (vec3){-coords[x], -70, -coords[z]});
		gstate.players[0].camera = camera;
		log_reset();
		clouds_reference(view, .625F, now);
		size_t expected_size = command_size;
		memcpy(expected_bytes, command_bytes, expected_size);
		gstate.players[0].camera = (struct camera){.x=9999,.y=-9999,.z=9999};
		log_reset();
		gutil_clouds_owned(view, .625F, &camera, now);
		assert(command_size == expected_size);
		assert(!memcmp(command_bytes, expected_bytes, command_size));
	}
	puts("Cloud draw byte equivalence: 36 camera pairs, vertices/UVs/colors/matrices/fog/state match frozen renderer");
}

int main(void) {
	memset(&gstate, 0, sizeof(gstate));
	gstate.players[0].camera.x = 0.0f;
	gstate.players[0].camera.y = 70.0f;
	gstate.players[0].camera.z = 0.0f;
	gstate.config.fog_distance = 128.0f;
	gstate.world_time = 6000;
	time_reset();
	gstate.world_time_start = time_get();

	mat4 view;
	glm_mat4_identity(view);

	gutil_clouds(view, 1.0f);

	// The scene must actually draw, or the bracket check below is vacuous.
	size_t first_draw = s_log_n, last_draw = 0, draws = 0;
	for(size_t i = 0; i < s_log_n; i++)
		if(s_log[i] == EV_DRAW) {
			if(draws == 0)
				first_draw = i;
			last_draw = i;
			draws++;
		}
	assert(draws > 0);
	printf("gutil_clouds emitted %zu quads across the two overdraw passes\n",
		  draws);

	// CLR0=DIRECT armed before the first immediate-mode quad: the LAST
	// lighting event before the first draw exists and is lighting(false).
	bool seen_off_before_draw = false;
	for(size_t i = 0; i < first_draw; i++) {
		if(s_log[i] == EV_LIGHT_OFF)
			seen_off_before_draw = true;
		else if(s_log[i] == EV_LIGHT_ON)
			seen_off_before_draw = false;
	}
	assert(seen_off_before_draw);

	// THE FIX under test: the INDEX8 world/entity contract is restored
	// after the last quad, i.e. gfx_lighting(true) is called somewhere
	// after the final gfx_draw_quads -- not left to the next caller.
	bool restored = false;
	for(size_t i = last_draw + 1; i < s_log_n; i++)
		if(s_log[i] == EV_LIGHT_ON)
			restored = true;
	assert(restored);

	// Exactly one off/on pair per call -- no redundant toggling mid-pass
	// (the k==0/k==1 two-pass loop shares one bracket, matching the
	// function's own single entry/exit).
	assert(log_count(EV_LIGHT_OFF) == 1);
	assert(log_count(EV_LIGHT_ON) == 1);

	// Calling twice (as main.c's per-frame loop does) must not accumulate
	// an unbalanced bracket -- each call is fully self-contained.
	log_reset();
	gutil_clouds(view, 0.5f);
	assert(log_count(EV_LIGHT_OFF) == 1);
	assert(log_count(EV_LIGHT_ON) == 1);
	assert(log_count(EV_DRAW) == draws);

	test_owned_cloud_bytes();
	printf("PASS: test_clouds_gx_state (gutil_clouds gfx_lighting bracket)\n");
	return 0;
}
