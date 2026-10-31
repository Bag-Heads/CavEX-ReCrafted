/*
	render-slice regression (task_b126e4e2): render_rain_snow MUST bracket its
	gfx_draw_quads_flt calls with gfx_lighting(false) ... gfx_lighting(true).

	On Wii, gfx_lighting toggles the GLOBAL GX vertex descriptor for CLR0
	between INDEX8 (world/entity display lists) and DIRECT (immediate RGBA
	quads). gfx_draw_quads_flt pushes 4 direct colour bytes per vertex, so a
	caller that leaves CLR0=INDEX8 armed feeds the CP a stream it parses
	expecting a 1-byte index -- FIFO desync, GP wedge, one 500ms
	bounded-drawdone abort per frame. main.c deliberately sets
	gfx_lighting(true) before entities_client_render, and render_rain_snow runs
	right after the entity pass, so the missing bracket wedged the GP on every
	storm frame: ~2fps client, and the starved single-core scheduler then froze
	the server world clock (the storm-load freeze this test pins).

	Every other gfx_draw_quads_flt caller (particle_render, the entity shadow,
	gutil stars) already brackets with the same pair; this test locks the one
	caller that historically did not. It links render_weather.c against a
	RECORDER gfx layer (no GL/GX) and asserts the call order.

	task_cc056031 regression (rain/snow texture-source audit): render_rain_snow
	MUST skip a pass whose own texture never loaded (texture_snow.data /
	texture_rain.data == NULL, the default-install / partial-pack state --
	assets/environment/ ships only clouds.png). tex_gfx_bind's own documented
	contract (source/platform/wii/texture.c) is that binding a NULL-data
	tex_gfx is a silent no-op that leaves whatever texture the PRIOR draw call
	left bound -- so without a guard, that pass would still iterate and emit
	quads textured with THAT leftover texture, sampled with this function's own
	V-scroll UVs: a wrongly-textured, wrongly-"animating" overlay instead of
	cleanly not drawing (the entity_shadow precedent, source/entity/entity.c).
	main below exercises this with the SAME stub world as the FIFO-bracket
	test (PLAINS everywhere -- enableSnow=false, enableRain=true), so only the
	RAIN pass would ever emit real draws; the guard cases below assert on
	bind/draw presence per pass via the recorder's tex-identity tracking.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "game/game_state.h"
#include "graphics/render_weather.h"
#include "graphics/texture_atlas.h"
#include "particle.h"
#include "platform/gfx.h"
#include "platform/texture.h"
#include "util.h"
#include "world.h"

// --- the globals render_weather.c references ---------------------------------
struct game_state gstate;
struct tex_gfx texture_rain;
struct tex_gfx texture_snow;

// --- recorder gfx layer -------------------------------------------------------
enum ev { EV_LIGHT_OFF, EV_LIGHT_ON, EV_DRAW, EV_BIND_SNOW, EV_BIND_RAIN };
static enum ev s_log[4096];
static size_t s_log_n;
static void rec(enum ev e) {
	assert(s_log_n < sizeof(s_log) / sizeof(s_log[0]));
	s_log[s_log_n++] = e;
}
static void log_reset(void) {
	s_log_n = 0;
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
void gfx_matrix_modelview(mat4 matrix) { (void)matrix; }
void gfx_draw_quads_flt(size_t vertex_count, const float* vertices,
						const uint8_t* colors, const float* texcoords) {
	(void)vertex_count;
	(void)vertices;
	(void)colors;
	(void)texcoords;
	rec(EV_DRAW);
}
// State calls the contract does not cover: accepted silently.
void gfx_cull_func(enum cull_func func) { (void)func; }
void gfx_blending(enum gfx_blend mode) { (void)mode; }
void gfx_alpha_test(bool enable) { (void)enable; }
void gfx_alpha_test_ref(bool enable, float ref) { (void)enable; (void)ref; }
void gfx_texture(bool enable) { (void)enable; }
// Tracks WHICH texture identity a bind targeted (task_cc056031): the guard
// under test is "never bind/draw a pass whose own texture.data is NULL", so
// the regression case needs to tell a snow-bind from a rain-bind, not just
// count binds.
void gfx_bind_texture(struct tex_gfx* tex) {
	if(tex == &texture_snow)
		rec(EV_BIND_SNOW);
	else if(tex == &texture_rain)
		rec(EV_BIND_RAIN);
}

// --- audio stub (rain-sound playback added 2026-08-31; this GX-state test
// cares only about draw brackets, so playback is a counted no-op) -----------
static int stub_audio_play3d_calls;
void audio_play3d(uint16_t sound_id, vec3 pos, float vol, float pitch) {
	(void)sound_id;
	(void)pos;
	(void)vol;
	(void)pitch;
	stub_audio_play3d_calls++;
}

// --- world/daytime/particle stubs --------------------------------------------
// Split-dims B2: render_weather.c reads the viewed world through
// world_view_current() (world.c); with no view scope it is &gstate.world.
struct world* world_view_current(void) {
	return &gstate.world;
}
// Flat world: solid ground at y=63, heightmap 64, full sky light, PLAINS
// biome everywhere (enableRain=true, enableSnow=false -> the RAIN pass draws).
w_coord_t world_get_height(struct world* w, w_coord_t x, w_coord_t z) {
	(void)w; (void)x; (void)z;
	return 64;
}
struct block_data world_get_block(struct world* w, w_coord_t x, w_coord_t y,
								  w_coord_t z) {
	(void)w; (void)x; (void)z;
	return (struct block_data) {
		.type = (y < 64) ? 1 /* stone */ : 0 /* air */,
		.sky_light = 15,
		.torch_light = 0,
	};
}
bool world_get_section_tint_at(struct world* w, w_coord_t x, w_coord_t z,
							   uint8_t* grass_i, uint8_t* grass_j) {
	(void)w; (void)x; (void)z;
	// precip_biome_from_tint reconstructs temp = 1-(i+0.5)/255 and
	// product = 1-(j+0.5)/255; (5,178) -> temp~0.978, product~0.3 -> PLAINS.
	*grass_i = 5;
	*grass_j = 178;
	return true;
}
float daytime_get_time(void) {
	return 6000.0f;
}
float daytime_brightness(float time) {
	(void)time;
	return 1.0f;
}
float rand_gen_flt(struct random_gen* g) {
	(void)g;
	return 0.5f;
}
void particle_add(vec3 pos, vec3 vel, uint8_t tex, float size, float lifetime,
				  bool gravity, uint8_t r, uint8_t g, uint8_t b,
				  bool ignore_light, particle_atlas_t atlas) {
	(void)pos; (void)vel; (void)tex; (void)size; (void)lifetime; (void)gravity;
	(void)r; (void)g; (void)b; (void)ignore_light; (void)atlas;
}
uint8_t tex_atlas_lookup_particle(enum tex_atlas_entry name) {
	(void)name;
	return 0;
}

int main(void) {
	// A player standing at the origin of the flat stub world, mid-storm.
	static struct entity player;
	player.pos[0] = 0.5f;
	player.pos[1] = 66.0f;
	player.pos[2] = 0.5f;
	memcpy(player.pos_old, player.pos, sizeof(player.pos));

	gstate.players[0].local_player = &player;
	gstate.world_loaded = true;
	gstate.fancy_graphics = false;
	gstate.weather_raining = true;
	gstate.weather_prev_rain_strength = 1.0f;
	gstate.weather_rain_strength = 1.0f;
	gstate.weather_prev_thunder_strength = 0.0f;
	gstate.weather_thunder_strength = 0.0f;
	gstate.precip_tick_count = 7;

	// texture_rain/texture_snow are file-scope globals -> zero-initialized,
	// i.e. .data == NULL exactly like the real "asset never loaded" state.
	// Case A (this block) is "both loaded": give each a distinct dummy
	// pointer so the recorder's tex-identity check in gfx_bind_texture can
	// tell them apart (a real tex_gfx's .data is a MEM2/malloc'd texel
	// buffer; only the pointer's non-NULL-ness and identity matter here).
	static uint8_t dummy_snow_data = 1, dummy_rain_data = 1;
	texture_snow.data = &dummy_snow_data;
	texture_rain.data = &dummy_rain_data;

	render_rain_snow(0.5f);

	// The scene must actually draw, or the contract below is vacuously green.
	size_t first_draw = s_log_n, last_draw = 0, draws = 0;
	for(size_t i = 0; i < s_log_n; i++)
		if(s_log[i] == EV_DRAW) {
			if(draws == 0)
				first_draw = i;
			last_draw = i;
			draws++;
		}
	assert(draws > 0);

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

	// The INDEX8 world/entity contract is restored after the last quad.
	bool restored = false;
	for(size_t i = last_draw + 1; i < s_log_n; i++)
		if(s_log[i] == EV_LIGHT_ON)
			restored = true;
	assert(restored);

	// Case A, continued: both textures present -> BOTH get bound (the SNOW
	// pass still binds even though this stub's PLAINS-everywhere biome makes
	// every snow column fail precip_biome_enable_snow and draw nothing --
	// the bind happens before the per-column biome gate, exactly like Beta's
	// own unconditional GL11.glBindTexture ahead of its column loop).
	assert(log_count(EV_BIND_SNOW) == 1);
	assert(log_count(EV_BIND_RAIN) == 1);
	size_t draws_both_loaded = draws;

	// Case B (task_cc056031 regression): texture_rain never loaded (the
	// default-install / partial-pack state). This stub world's biome is
	// PLAINS everywhere (enableSnow=false, enableRain=true), so RAIN is the
	// ONLY pass that ever emits real quads -- pre-fix, render_rain_snow bound
	// texture_rain anyway (a silent no-op on the real GX layer -- see
	// tex_gfx_bind's contract) and iterated the RAIN loop, drawing every
	// in-range column textured with whatever was left bound from the SNOW
	// pass immediately before it (itself possibly stale, since SNOW draws 0
	// quads here). Post-fix the entire RAIN block must be skipped: no
	// EV_BIND_RAIN, and consequently zero draws in this world.
	log_reset();
	texture_snow.data = &dummy_snow_data;
	texture_rain.data = NULL;
	render_rain_snow(0.5f);
	assert(log_count(EV_BIND_SNOW) == 1); // snow still loaded -> still binds
	assert(log_count(EV_BIND_RAIN) == 0); // rain missing -> must NOT bind
	assert(log_count(EV_DRAW) == 0); // and the only drawing biome was RAIN's
	assert(draws_both_loaded > 0); // (sanity: Case A really did draw)

	// Symmetric check: texture_snow missing, texture_rain present -> the
	// RAIN pass (the one this stub world can actually draw) is unaffected.
	log_reset();
	texture_snow.data = NULL;
	texture_rain.data = &dummy_rain_data;
	render_rain_snow(0.5f);
	assert(log_count(EV_BIND_SNOW) == 0); // snow missing -> must NOT bind
	assert(log_count(EV_BIND_RAIN) == 1);
	assert(log_count(EV_DRAW) == draws_both_loaded); // same RAIN geometry as A

	// Case C: NEITHER texture loaded -> render_rain_snow must return before
	// touching ANY GL state (the function-top early-out) -- not one event
	// recorded, not even the gfx_lighting bracket.
	log_reset();
	texture_snow.data = NULL;
	texture_rain.data = NULL;
	render_rain_snow(0.5f);
	assert(s_log_n == 0);

	printf("test_precip_gx_state: OK (%zu draws bracketed, texture-presence "
		   "guard verified)\n",
		   draws);
	return 0;
}
