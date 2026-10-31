/*
	Field defect regression (2026-08-30 maintainer live Wii-vs-PC MP session,
	PROJECT_MEMORY.md "TWO MORE FIELD DEFECTS" item (a)): "NO NAME TAGS over
	remote players on the Wii -- the PC client renders 'WiiTuck' over the Wii
	player... the Wii never draws the PC player's name."

	Root-cause investigation found NO platform-conditional code anywhere in
	the call chain (main.c's render_name_tag_draw_pass call, this pass's own
	select_nearest_players/build_batch dict-iteration logic, name_tag_build_
	quads, entity_remote_player's username plumbing, or client_interface.c's
	CRPC_SPAWN_PLAYER handler) -- render_name_tag_draw.c compiles and runs
	IDENTICALLY on PC and Wii; the ONLY thing that differs per-platform is
	which real gfx_* backend (source/platform/pc/gfx.c vs
	source/platform/wii/gfx.c) receives the SAME calls this pass makes.

	The actual GAP this test closes: unlike test_name_tag_quads.c (which
	exercises ONLY the pure geometry builder, render_name_tag.c, with no
	entity dict and no draw calls at all), NOTHING previously exercised
	render_name_tag_draw_pass itself -- the dict_entity_t walk
	(select_nearest_players), the sneak/standing split (build_batch), and the
	actual gfx_draw_quads_flt emission (draw_batch) were completely untested.
	That is exactly the code a "hw-certified" claim resting on a non-visual
	far_idle_2m heartbeat (see PROJECT_MEMORY.md 2026-08-31 entries) would
	never have exercised either -- a heartbeat proves the Wii didn't crash,
	not that a quad reached the screen.

	This test closes that gap by linking the REAL production
	render_name_tag.c + render_name_tag_draw.c against a RECORDER gfx layer
	(mirrors test_precip_gx_state.c's rig exactly, itself pinning an
	analogous "does this pass actually reach gfx_draw_quads_flt" contract for
	render_weather.c) and a synthetic dict_entity_t populated with real
	ENTITY_REMOTE_PLAYER entities (mirrors test_audio_env.c's dict_entity_
	safe_get + malloc(sizeof(struct entity)) construction pattern). Because
	this is the exact same C source file compiled and run on PC here as on
	Wii (no #ifdef PLATFORM_WII/PC anywhere in render_name_tag_draw.c), a
	green result here is strong evidence the SELECTION/DISPATCH logic itself
	is not the Wii-side gap -- narrowing the remaining suspects to the real
	GX backend (asset presence, GX state left dirty by a prior draw in the
	SAME frame) rather than this engine-level C logic, which this test now
	pins byte-for-byte.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "entity/entity.h"
#include "game/camera.h"
#include "graphics/render_name_tag.h"
#include "graphics/render_name_tag_draw.h"
#include "platform/gfx.h"
#include "platform/texture.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- the extern the production file references -------------------------------
// Zero-initialized (BSS) like the real global -- .data starts NULL exactly
// like the "asset never loaded" state tests/test_precip_gx_state.c's Case C
// exercises for texture_rain/texture_snow. Tests that want the pass to
// actually draw must set a dummy non-NULL .data first (matching that same
// test's dummy_snow_data/dummy_rain_data pattern) -- see set_font_loaded().
struct tex_gfx texture_font;
static uint8_t s_dummy_font_data = 1;
static void set_font_loaded(bool loaded) {
	texture_font.data = loaded ? &s_dummy_font_data : NULL;
}

// --- font metrics stub (no gui_util.c link -- mirrors test_name_tag_quads.c's
// own synthetic g_width rig, just exposed under the REAL production symbol
// name render_name_tag_draw.c calls). ------------------------------------------
static uint8_t g_width[256];
const uint8_t* gutil_font_glyph_widths(void) {
	return g_width;
}

// --- recorder gfx layer (test_precip_gx_state.c's exact pattern) -------------
enum ev {
	EV_MODELVIEW,
	EV_LIGHT_OFF,
	EV_LIGHT_ON,
	EV_BLEND_ON,
	EV_BLEND_OFF,
	EV_ALPHA_TEST_ON,
	EV_ALPHA_TEST_OFF,
	EV_TEX_ON,
	EV_TEX_OFF,
	EV_BIND_FONT,
	EV_BIND_OTHER,
	EV_WRITE_BUFFERS,
	EV_DRAW,
};
struct draw_record {
	size_t vertex_count;
	uint8_t first_alpha; // colors[3] of the first emitted vertex
	bool textured;		 // gfx_texture(true) currently in effect
};
static enum ev s_log[4096];
static size_t s_log_n;
static struct draw_record s_draws[64];
static size_t s_draws_n;
static bool s_tex_on = false;

static void rec(enum ev e) {
	assert(s_log_n < sizeof(s_log) / sizeof(s_log[0]));
	s_log[s_log_n++] = e;
}
static void log_reset(void) {
	s_log_n = 0;
	s_draws_n = 0;
	s_tex_on = false;
}
static size_t log_count(enum ev e) {
	size_t n = 0;
	for(size_t i = 0; i < s_log_n; i++)
		if(s_log[i] == e)
			n++;
	return n;
}

void gfx_matrix_modelview(mat4 mv) {
	(void) mv;
	rec(EV_MODELVIEW);
}
void gfx_lighting(bool enable) {
	rec(enable ? EV_LIGHT_ON : EV_LIGHT_OFF);
}
void gfx_blending(enum gfx_blend mode) {
	rec(mode == MODE_BLEND ? EV_BLEND_ON : EV_BLEND_OFF);
}
void gfx_alpha_test(bool enable) {
	rec(enable ? EV_ALPHA_TEST_ON : EV_ALPHA_TEST_OFF);
}
void gfx_texture(bool enable) {
	s_tex_on = enable;
	rec(enable ? EV_TEX_ON : EV_TEX_OFF);
}
void gfx_bind_texture(struct tex_gfx* tex) {
	rec(tex == &texture_font ? EV_BIND_FONT : EV_BIND_OTHER);
}
void gfx_write_buffers(bool color, bool depth, bool depth_test) {
	(void) color;
	(void) depth;
	(void) depth_test;
	rec(EV_WRITE_BUFFERS);
}
void gfx_draw_quads_flt(size_t vertex_count, const float* vertices,
						const uint8_t* colors, const float* texcoords) {
	(void) vertices;
	(void) texcoords;
	assert(s_draws_n < sizeof(s_draws) / sizeof(s_draws[0]));
	s_draws[s_draws_n].vertex_count = vertex_count;
	s_draws[s_draws_n].first_alpha = vertex_count > 0 ? colors[3] : 0;
	s_draws[s_draws_n].textured = s_tex_on;
	s_draws_n++;
	rec(EV_DRAW);
}

// --- entity dict construction (test_audio_env.c's pattern) -------------------
static struct entity* spawn_remote_player(dict_entity_t entities, uint32_t id,
										  const char* username,
										  const vec3 pos, bool sneaking) {
	struct entity** slot = dict_entity_safe_get(entities, id);
	assert(slot);
	*slot = calloc(1, sizeof(struct entity));
	struct entity* e = *slot;
	assert(e);
	e->type = ENTITY_REMOTE_PLAYER;
	glm_vec3_copy((float*) pos, e->pos);
	glm_vec3_copy((float*) pos, e->pos_old);
	if(username) {
		size_t cap = sizeof(e->data.remote_player.username) - 1;
		size_t len = strlen(username);
		if(len > cap)
			len = cap;
		memcpy(e->data.remote_player.username, username, len);
		e->data.remote_player.username[len] = '\0';
	}
	e->data.remote_player.sneaking = sneaking;
	return e;
}

static void free_all_entities(dict_entity_t entities) {
	dict_entity_it_t it;
	dict_entity_it(it, entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(entities);
}

static void init_widths(void) {
	memset(g_width, 6, sizeof(g_width));
	g_width[(unsigned char) ' '] = 4;
}

static void make_camera(struct camera* c, float x, float y, float z) {
	memset(c, 0, sizeof(*c));
	c->x = x;
	c->y = y;
	c->z = z;
	c->rx = 0.0f;
	c->ry = (float) M_PI / 2.0f; // level, matches test_name_tag_quads.c's convention
	glm_mat4_identity(c->view);
}

// Test 1: one standing player, in range, real username -- the exact "WiiTuck"
// field-defect scenario. Must actually reach gfx_draw_quads_flt (the previously
// completely untested select_nearest_players -> build_batch -> draw_batch
// chain), bind texture_font (not some leftover texture), and emit Beta's own
// three-draw shape [740c583 RenderLiving.renderLivingLabel]: background (alpha
// 64 == round(0.25*255)), through-walls dim text pass (alpha 32), opaque text
// pass (alpha 255).
static void test_standing_player_draws(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(true);

	spawn_remote_player(entities, 1, "WiiTuck", (vec3) {5.0f, 64.0f, 0.0f},
					   false);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	// Exactly 3 gfx_draw_quads_flt calls: background, dim text, opaque text.
	assert(log_count(EV_DRAW) == 3);
	assert(s_draws_n == 3);
	assert(s_draws[0].first_alpha == 64);	// background, untextured
	assert(s_draws[0].textured == false);
	assert(s_draws[1].first_alpha == 32);	// through-walls dim text
	assert(s_draws[1].textured == true);
	assert(s_draws[2].first_alpha == 255); // opaque text
	assert(s_draws[2].textured == true);
	// Same glyph geometry drawn twice (pass 1 and pass 2 share the same quads).
	assert(s_draws[1].vertex_count == s_draws[2].vertex_count);
	assert(s_draws[1].vertex_count > 0);

	// texture_font specifically bound (not a leftover texture) before the
	// text passes -- the exact "PC-only font/texture path" hypothesis this
	// investigation was asked to rule in/out.
	assert(log_count(EV_BIND_FONT) >= 1);
	assert(log_count(EV_BIND_OTHER) == 0);

	// Mandatory GX-state restore (render_name_tag_draw.c's own documented
	// contract): lighting/blend/alpha-test all come back to the world/entity
	// pass's expected resting state before returning.
	assert(s_log[s_log_n - 1] == EV_LIGHT_ON);
	assert(log_count(EV_BLEND_OFF) == 1);
	assert(log_count(EV_ALPHA_TEST_ON) == 1);

	free_all_entities(entities);
	dict_entity_clear(entities);
}

// Test 2: player beyond the 64-block standing cap -- Beta's own
// RenderPlayer.renderName distance gate. MUST touch ZERO gfx state (the
// pass's own documented "zero state touched" early-out contract).
static void test_out_of_range_draws_nothing(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(true);

	spawn_remote_player(entities, 1, "WiiTuck", (vec3) {0.0f, 64.0f, 65.0f},
					   false);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	assert(s_log_n == 0);
	assert(s_draws_n == 0);

	free_all_entities(entities);
	dict_entity_clear(entities);
}

// Test 3: sneaking player within the 32-block sneak cap -- [740c583
// RenderPlayer.renderName]'s crouch branch: depth-tested background + ONE
// dim (alpha 32) text pass, NOT the standing two-pass shape.
static void test_sneaking_player_single_dim_pass(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(true);

	spawn_remote_player(entities, 1, "WiiTuck", (vec3) {5.0f, 64.0f, 0.0f},
					   true);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	assert(log_count(EV_DRAW) == 2); // background + ONE text pass
	assert(s_draws_n == 2);
	assert(s_draws[0].first_alpha == 64);
	assert(s_draws[1].first_alpha == 32); // dim, reused -- not opaque 255

	free_all_entities(entities);
	dict_entity_clear(entities);
}

// Test 4: sneaking player between the 32-block sneak cap and the 64-block
// standing cap. Proves the is_sneaking flag actually threads end-to-end
// through select_nearest_players -> build_batch -> name_tag_build_quads
// (candidates[] carries sneaking through the WHOLE dict-iteration/dispatch
// chain this test exists to cover) -- a standing player at this exact
// distance draws (test 1); a sneaking one at the same distance must not.
static void test_sneaking_player_beyond_sneak_cap_draws_nothing(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(true);

	spawn_remote_player(entities, 1, "WiiTuck", (vec3) {40.0f, 64.0f, 0.0f},
					   true);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	assert(s_log_n == 0);
	assert(s_draws_n == 0);

	free_all_entities(entities);
	dict_entity_clear(entities);
}

// Test 5: empty username -- name_tag_build_quads's own empty-name guard must
// propagate all the way through build_batch/draw_batch to zero draws (not a
// blank/zero-width box).
static void test_empty_username_draws_nothing(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(true);

	spawn_remote_player(entities, 1, "", (vec3) {5.0f, 64.0f, 0.0f}, false);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	assert(s_log_n == 0);
	assert(s_draws_n == 0);

	free_all_entities(entities);
	dict_entity_clear(entities);
}

// Test 6: a non-player entity (e.g. a mob) in the SAME dict must never
// contribute a tag -- select_nearest_players's own ENTITY_REMOTE_PLAYER type
// filter, proven against a real dict walk rather than assumed.
static void test_non_player_entity_ignored(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(true);

	struct entity** slot = dict_entity_safe_get(entities, 42);
	*slot = calloc(1, sizeof(struct entity));
	(*slot)->type = ENTITY_MONSTER;
	glm_vec3_copy((vec3) {5.0f, 64.0f, 0.0f}, (*slot)->pos);
	glm_vec3_copy((vec3) {5.0f, 64.0f, 0.0f}, (*slot)->pos_old);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	assert(s_log_n == 0);
	assert(s_draws_n == 0);

	free_all_entities(entities);
	dict_entity_clear(entities);
}

// Test 7: two remote players in the SAME dict (the real MP shape) -- both
// must draw, proving the dict-iteration/batch accumulation (not just a
// single-entity happy path) actually works end-to-end.
static void test_two_players_both_draw(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(true);

	spawn_remote_player(entities, 1, "WiiTuck", (vec3) {5.0f, 64.0f, 0.0f},
					   false);
	spawn_remote_player(entities, 2, "Ghost2", (vec3) {-5.0f, 64.0f, 0.0f},
					   false);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	// Batched: exactly 3 draws total (bg for both players in ONE call, text
	// pass 1 for both in ONE call, text pass 2 for both in ONE call) -- see
	// render_name_tag_draw.c's header "at most 3 gfx_draw_quads_flt calls per
	// frame regardless of player count".
	assert(log_count(EV_DRAW) == 3);
	assert(s_draws[0].vertex_count == 2 * NAME_TAG_BG_VERTS);

	free_all_entities(entities);
	dict_entity_clear(entities);
}

// Test 8 (the guard this investigation added, render_name_tag_draw.c's
// texture_font.data early-out): a player in range with a real username, but
// texture_font never loaded, must draw NOTHING -- not a leftover-textured
// box (tex_gfx_bind's own documented silent-no-op-on-NULL-data contract).
// Mirrors test_precip_gx_state.c's Case C. Also proves the guard is
// SPECIFIC to the missing texture (not a blanket regression): the exact
// same entity/camera pair draws normally once the texture is marked loaded
// again (same shape asserted by test_standing_player_draws).
static void test_missing_font_texture_draws_nothing(void) {
	dict_entity_t entities;
	dict_entity_init(entities);
	log_reset();
	set_font_loaded(false);

	spawn_remote_player(entities, 1, "WiiTuck", (vec3) {5.0f, 64.0f, 0.0f},
					   false);

	struct camera cam;
	make_camera(&cam, 0.0f, 64.0f, 0.0f);

	render_name_tag_draw_pass(entities, &cam, 1.0f);

	assert(s_log_n == 0); // not one GX/gfx call -- function-top early-out
	assert(s_draws_n == 0);

	// Same dict/entity/camera, texture now present -> normal draw resumes.
	log_reset();
	set_font_loaded(true);
	render_name_tag_draw_pass(entities, &cam, 1.0f);
	assert(log_count(EV_DRAW) == 3);
	assert(log_count(EV_BIND_FONT) >= 1);

	free_all_entities(entities);
	dict_entity_clear(entities);
}

int main(void) {
	init_widths();

	test_standing_player_draws();
	test_out_of_range_draws_nothing();
	test_sneaking_player_single_dim_pass();
	test_sneaking_player_beyond_sneak_cap_draws_nothing();
	test_empty_username_draws_nothing();
	test_non_player_entity_ignored();
	test_two_players_both_draw();
	test_missing_font_texture_draws_nothing();

	printf("test_name_tag_draw_pass: all assertions passed (%d cases)\n", 8);
	return 0;
}
