/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifdef ACHIEVEMENT_BELL_TEST
/* Real audio dispatcher; deterministic device boundary, no physical speaker. */
#include "platform/audio.h"
#include "platform/menu_sound.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef PLATFORM_PC
static unsigned starts, music_calls, picked;
static float gain;
static bool playing[16];
static uint32_t audio_backend_music_last_dur_ms(void) {
	return 0;
}
#include "../source/platform/audio.c"
#include "support/audio_deferred_stubs.h"
static bool audio_backend_init(void) {
	return true;
}
static void audio_backend_shutdown(void) { }
static void audio_backend_voice_start(int slot, const struct audio_sample* smp,
									  float rate, float l, float r) {
	assert(l == r);
	starts++;
	gain = l;
	playing[slot] = true;
}
static void audio_backend_voice_gain(int slot, float l, float r) {
	assert(l == r);
	gain = l;
}
static bool audio_backend_voice_playing(int slot) {
	return playing[slot];
}
static void audio_backend_stop_all(void) {
	memset(playing, 0, sizeof playing);
}
static void audio_backend_music_start(const int16_t* pcm, uint32_t frames,
									  float rate, float g) {
	music_calls++;
}
static void audio_backend_music_gain(float g) {
	music_calls++;
}
static bool audio_backend_music_playing(void) {
	return false;
}
static bool audio_backend_music_drained(void) {
	return true;
}
static void audio_backend_music_stop(void) {
	music_calls++;
}
static uint32_t audio_backend_music_native_rate(void) {
	return 48000;
}
void audio_registry_load(void) { }
void audio_registry_note_voice_started(int slot,
									   const struct audio_sample* smp) { }
const struct audio_sample* audio_registry_pick(uint16_t id) {
	static const int16_t pcm[] = {0, 1, 0};
	static const struct audio_sample sample
		= {.pcm = pcm, .frames = 3, .rate = 48000};
	picked = id;
	return &sample;
}
int main(void) {
	audio_init(true, 100);
	audio_achievement_bell();
	assert(starts == 1 && picked == SOUND_MENU_ACHIEVEMENT && gain == .25F);
	audio_achievement_bell();
	assert(starts == 1); /* simultaneous split earns */
	audio_update((vec3) {0}, 0);
	audio_achievement_bell();
	assert(starts == 1);
	assert(audio_set_master_volume(40) && fabsf(gain - .10F) < .00001F);
	assert(music_calls == 0); /* no duck or music-bus action */
	assert(audio_set_master_volume(0));
	audio_achievement_bell();
	assert(starts == 1);
	assert(audio_set_master_volume(100));
	audio_achievement_bell();
	assert(starts == 2);
	memset(playing, 0, sizeof playing);
	audio_update((vec3) {0}, 0);
	audio_achievement_bell();
	assert(starts == 3);
	audio_stop_all();
	audio_init(false, 100);
	audio_achievement_bell();
	assert(starts == 3);
	puts("PASS Warm bell single shared voice, no restart, Sound volume/mute, "
		 "no music duck");
	return 0;
}
#else
/* Brief E RED dependency: one drain must retain all notices in emit order.
 * Exercise the existing public API and real module, not a presenter double.
 * The current one-slot implementation discards the first two notices. */
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game_state.h"
#define CW_HAVE_ACTOR_IDENT
#include "block/blocks.h"
#include "game/achievements/achievements_api.h"
#include "game/gui/achievement_notice.h"
#include "graphics/render_item.h"
#include "item/items.h"
#include "light_brightness.h"
#include "lodepng/lodepng.h"
#include "platform/audio.h"
#include "platform/gfx.h"
#include "platform/input_glyph.h"
#include "support/test_fixture_dir.h"

static void emit(uint8_t kind, uint8_t authority, uint16_t item) {
	struct game_event ev = {0};
	ev.kind = kind;
	ev.authority = authority;
	ev.who.slot = 1;
	ev.who.generation = 1;
	ev.u.item.id = item;
	ev.u.item.count = 1;
	game_event_emit(&ev);
}

static void fifo(void) {
	char dir[128];
	struct ach_notice n;
	struct ach_status st;
	test_fixture_dir("test_achievements_notice_tmp", dir, sizeof dir);
#ifdef _WIN32
	assert(mkdir(dir) == 0);
#else
	assert(mkdir(dir, 0777) == 0);
#endif
	assert(achievements_init(dir));
	assert(achievements_bind(0, 1, "notice_p1", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	assert(achievements_bind(1, 1, "notice_p2", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);

	/* Parent chain: Taking Inventory -> Getting Wood -> Benchmarking.
	 * The duplicate log increments progress but must not add another notice. */
	emit(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0);
	emit(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17);
	emit(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17);
	emit(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58);
	achievements_tick();
	assert(achievements_status(1, &st) && st.notices_queued == 3);
	assert(!achievements_notice_peek(0, &n) && n.hint_active);

	int observed[3] = {-1, -1, -1};
	for(int i = 0; i < 3; i++) {
		if(achievements_notice_peek(1, &n))
			observed[i] = n.ach_id;
		achievements_notice_ack(1);
	}
	assert(!achievements_notice_peek(1, &n) && !n.hint_active);
	printf("Brief E FIFO: expected [0,1,2], observed [%d,%d,%d]; "
		   "module counted %u notices\n",
		   observed[0], observed[1], observed[2], st.notices_queued);
	fflush(stdout);
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
	assert(observed[0] == 0 && observed[1] == 1 && observed[2] == 2);
	puts("PASS Brief E FIFO, duplicate suppression and owner isolation");
}

/* Graphical boundary only. Native capture passes through these same wrappers.
 */
static bool native;
static unsigned bells, draws, glyph_calls;
static int last_glyph;
static int make_dir(const char* dir) {
#ifdef _WIN32
	return mkdir(dir);
#else
	return mkdir(dir, 0777);
#endif
}
void __real_audio_achievement_bell(void);
void __wrap_audio_achievement_bell(void) {
	bells++;
}
void __real_gfx_texture(bool);
void __wrap_gfx_texture(bool on) {
	if(native)
		__real_gfx_texture(on);
}
void __real_gfx_bind_texture(struct tex_gfx*);
void __wrap_gfx_bind_texture(struct tex_gfx* t) {
	if(native)
		__real_gfx_bind_texture(t);
}
void __real_gfx_draw_quads(size_t, const int16_t*, const uint8_t*,
						   const uint16_t*);
void __wrap_gfx_draw_quads(size_t n, const int16_t* v, const uint8_t* c,
						   const uint16_t* uv) {
	draws++;
	if(native)
		__real_gfx_draw_quads(n, v, c, uv);
}
void __real_gutil_draw_item_sized(struct item_data*, int, int, int, int);
void __wrap_gutil_draw_item_sized(struct item_data* i, int x, int y, int size,
								  int layer) {
	if(native)
		__real_gutil_draw_item_sized(i, x, y, size, layer);
}
void __real_gutil_text_rgb(int, int, const char*, int, bool, uint8_t, uint8_t,
						   uint8_t);
void __wrap_gutil_text_rgb(int x, int y, const char* s, int size, bool shadow,
						   uint8_t r, uint8_t g, uint8_t b) {
	if(native)
		__real_gutil_text_rgb(x, y, s, size, shadow, r, g, b);
}
int __real_gutil_prompt_glyph(int, int, int, int);
int __wrap_gutil_prompt_glyph(int x, int y, int size, int glyph) {
	glyph_calls++;
	last_glyph = glyph;
	int modifier, button;
	int width = input_glyph_cells(glyph, &modifier, &button) ?
		(modifier >= 0 ? size * 5 / 2 : size) :
		0;
	return native ? __real_gutil_prompt_glyph(x, y, size, glyph) : width;
}
bool __real_tex_gfx_load(struct tex_gfx*, void*, size_t, size_t,
						 enum tex_format, bool);
bool __wrap_tex_gfx_load(struct tex_gfx* t, void* p, size_t w, size_t h,
						 enum tex_format fmt, bool linear) {
	if(native)
		return __real_tex_gfx_load(t, p, w, h, fmt, linear);
	t->data = p;
	t->width = w;
	t->height = h;
	return true;
}
void __real_tex_gfx_clear(struct tex_gfx*);
void __wrap_tex_gfx_clear(struct tex_gfx* t) {
	if(native)
		__real_tex_gfx_clear(t);
	else {
		free(t->data);
		*t = (struct tex_gfx) {0};
	}
}
void __real_tex_gfx_lookup(struct tex_gfx*, int, int, uint8_t*);
void __wrap_tex_gfx_lookup(struct tex_gfx* t, int x, int y, uint8_t* c) {
	if(native)
		__real_tex_gfx_lookup(t, x, y, c);
	else {
		c[0] = c[1] = c[2] = 198;
		c[3] = 255;
	}
}
int __real_gutil_font_width(const char*, int);
int __wrap_gutil_font_width(const char* s, int size) {
	return native ? __real_gutil_font_width(s, size) : (int)strlen(s) * 13;
}

static struct input_player_frame prompt(enum input_category category) {
	struct input_player_frame f = {.connected = true, .category = category};
	for(int i = 0; i < IB_COUNT; i++)
		f.glyphs[i] = -1;
	f.glyphs[IB_INVENTORY]
		= input_glyph_key(category,
						  category == INPUT_CAT_NUNCHUK				   ? 6 :
							  category == INPUT_CAT_CLASSIC_CONTROLLER ? 206 :
																		 407);
	/* The hint names the gameplay OPEN role (config input.open_inventory,
	 * shipped [4, 206, 407] = Remote A), never the Close/Back role. */
	f.glyphs[IB_OPEN_INVENTORY]
		= input_glyph_key(category,
						  category == INPUT_CAT_NUNCHUK				   ? 4 :
							  category == INPUT_CAT_CLASSIC_CONTROLLER ? 206 :
																		 407);
	return f;
}
static bool draw(unsigned owner, unsigned ms, bool hidden,
				 const struct menu_rect* obstacle,
				 struct achievement_notice_frame* out) {
	achievement_notice_update(time_add_ms((ptime_t) {0}, ms));
	return achievement_notice_render(owner, NULL, 0, owner ? 240 : 0, 640, 240,
									 hidden, obstacle, obstacle ? 1 : 0, out);
}
static void geometry(void) {
	for(int width = 640; width <= 854; width += 214) {
		for(int split = 0; split < 2; split++) {
			struct achievement_notice_frame f;
			achievement_notice_layout(&f, 0, split ? 240 : 0, width,
									  split ? 240 : 480, 8, false, NULL);
			assert(f.card.x == width - (split ? 32 : 24) - (split ? 280 : 320));
			assert(f.card.y == (split ? 252 : 24)
				   && f.card.w == (split ? 280 : 320));
			assert(f.card.h == (split ? 56 : 64));
			assert(f.slot.x == f.card.x + (split ? 8 : 10));
			assert(f.caption.x == f.card.x + (split ? 56 : 64));
			assert(f.caption.y == f.card.y + (split ? 8 : 12));
			assert(f.name.y == f.card.y + (split ? 28 : 32));
			assert(f.name.w == (split ? 216 : 244));
			/* Fixed wide metrics exercise the two-line branch at split width.
			 */
			achievement_notice_layout(&f, 0, 0, width, split ? 240 : 480, 9,
									  false, NULL);
			assert(f.wrapped == !!split && f.card.h == (split ? 76 : 64));
		}
	}
}
static void timing(void) {
	char dir[128];
	test_fixture_dir("test_notice_timing", dir, sizeof dir);
	assert(make_dir(dir) == 0);
	assert(achievements_init(dir));
	assert(achievements_bind(1, 1, "notice_time", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	achievement_notice_init();
	bells = 0;
	struct achievement_notice_frame f;
	/* A silent hint, from this owner's live binding (including a remap). */
	for(int c = INPUT_CAT_NUNCHUK; c <= INPUT_CAT_GAMECUBE; c++) {
		struct input_player_frame p = prompt(c);
		achievement_notice_update(time_add_ms((ptime_t) {0}, 0));
		glyph_calls = 0;
		assert(achievement_notice_render(1, &p, 0, 240, 640, 240, false, NULL,
										 0, &f));
		assert(f.hint
			   && f.glyph
				   == (c == INPUT_CAT_NUNCHUK				 ? 0 :
						   c == INPUT_CAT_CLASSIC_CONTROLLER ? 12 :
															   35));
		assert(glyph_calls == 1 && last_glyph == p.glyphs[IB_OPEN_INVENTORY]
			   && bells == 0);
		p.glyphs[IB_INVENTORY] = 29; /* a Close rebind must not move the hint */
		p.glyphs[IB_OPEN_INVENTORY] = 28;
		glyph_calls = 0;
		assert(achievement_notice_render(1, &p, 0, 240, 640, 240, false, NULL,
										 0, &f));
		assert(f.glyph == 28 && last_glyph == 28);
		p.glyphs[IB_OPEN_INVENTORY] = 64 + 28;
		assert(achievement_notice_render(1, &p, 0, 240, 640, 240, false, NULL,
										 0, &f));
		assert(f.wrapped && f.card.h == 76 && f.name.h == 44);
		p.glyphs[IB_OPEN_INVENTORY] = -1;
		glyph_calls = 0;
		assert(achievement_notice_render(1, &p, 0, 240, 640, 240, false, NULL,
										 0, &f));
		assert(f.glyph == -1 && glyph_calls == 0);
	}
	emit(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0);
	emit(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17);
	emit(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17);
	achievements_tick();
	assert(!draw(1, 0, true, NULL, &f) && bells == 0);
	assert(!draw(1, 10000, true, NULL, &f));
	assert(draw(1, 10001, false, NULL, &f) && f.ach_id == 0
		   && f.elapsed_ms == 0);
	assert(draw(1, 10002, false, NULL, &f) && f.elapsed_ms == 1);
	assert(draw(1, 10003, false, NULL, &f) && bells == 0 && f.slide_y < 0);
	assert(draw(1, 10040, false, NULL, &f) && bells == 1);
	assert(draw(1, 10376, false, NULL, &f) && f.elapsed_ms == 375
		   && f.slide_y == 0);
	assert(!draw(1, 10377, true, NULL, &f)); /* menu, map or death */
	assert(!draw(1, 20000, true, NULL, &f));
	assert(draw(1, 20001, false, NULL, &f) && f.elapsed_ms == 375
		   && bells == 1);
	assert(draw(1, 22251, false, NULL, &f) && f.elapsed_ms == 2625
		   && f.slide_y == 0);
	assert(draw(1, 22625, false, NULL, &f) && f.elapsed_ms == 2999
		   && f.slide_y < 0);
	assert(draw(1, 22626, false, NULL, &f) && f.ach_id == 1
		   && f.elapsed_ms == 0);
	assert(draw(1, 22665, false, NULL, &f) && bells == 2);
	/* Real unsafe rectangles freeze the same clock, with the exact 8px gap. */
	struct menu_rect obstacle = {f.card.x - 7, f.card.y, 1, 20};
	assert(!draw(1, 22666, false, &obstacle, &f));
	assert(!draw(1, 50000, false, &obstacle, &f));
	assert(draw(1, 50001, false, NULL, &f) && f.elapsed_ms == 39 && bells == 2);
	assert(!draw(1, 53000, false, NULL, &f) && !f.hint && f.ach_id == -1);
	struct ach_notice n;
	assert(!achievements_notice_peek(1, &n));
	/* Replace identity while its notice is pending; progress still saved. */
	emit(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58);
	achievements_tick();
	assert(draw(1, 53001, false, NULL, &f) && f.ach_id == 2);
	achievements_unbind(1, 1);
	assert(achievements_bind(1, 2, "new_notice_owner", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	assert(draw(1, 53002, false, NULL, &f) && f.hint && bells == 2);
	achievements_unbind(1, 2);
	assert(achievements_bind(1, 3, "notice_time", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	struct ach_desc row;
	assert(achievements_get(1, 2, &row) && row.unlocked);
	assert(!draw(1, 53003, false, NULL, &f));
	achievement_notice_shutdown();
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
	puts("PASS notice 3000/375 timing, FIFO/duplicates, freeze/resume, no bell "
		 "replay, glyphs, identity reset");
}

static void capture_award(unsigned owner, uint64_t generation, int id) {
	struct game_event ev = {.kind = GE_REMOTE_STAT,
							.authority = GE_AUTH_PACKET200,
							.who = {.slot = owner, .generation = generation}};
	ev.u.remote.stat_id = CW_ACH_BASE + id;
	ev.u.remote.amount = 1;
	if(id == 0) {
		ev.kind = GE_INVENTORY_OPENED;
		ev.authority = GE_AUTH_CLIENT;
	}
	game_event_emit(&ev);
	achievements_tick();
}
static void fifo_capacity(void) {
	char dir[128];
	test_fixture_dir("test_notice_capacity", dir, sizeof dir);
	assert(make_dir(dir) == 0);
	assert(achievements_init(dir));
	printf("FIFO memory: %lu bytes in the existing single module allocation\n",
		   (unsigned long)achievements_block_bytes());
	for(int pass = 0; pass < 2; pass++) {
		assert(achievements_bind(1, pass + 1, pass ? "fifo_wrap" : "fifo_full",
								 ACH_MODE_REMOTE_SERVER)
			   == ACH_BIND_OK);
		for(int i = 0; i < CW_ACH_COUNT; i++) {
			capture_award(1, pass + 1, i);
			capture_award(1, pass + 1, i);
			if(pass && i < 8) {
				struct ach_notice n;
				assert(achievements_notice_peek(1, &n) && n.ach_id == i);
				achievements_notice_ack(1);
			}
		}
		struct ach_status st;
		assert(achievements_status(1, &st)
			   && st.notices_queued == CW_ACH_COUNT);
		for(int i = pass ? 8 : 0; i < CW_ACH_COUNT; i++) {
			struct ach_notice n;
			assert(achievements_notice_peek(1, &n) && n.ach_id == i);
			achievements_notice_ack(1);
		}
		struct ach_notice n;
		assert(!achievements_notice_peek(1, &n));
		achievements_unbind(1, pass + 1);
	}
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
	puts("PASS 16-notice capacity, duplicates at capacity, FIFO head wrap, "
		 "bind/unbind reset");
}
static void bell_registry(void) {
	char dir[128], path[256];
	test_fixture_dir("test_notice_audio_tmp", dir, sizeof dir);
	assert(make_dir(dir) == 0);
	snprintf(path, sizeof path, "%s/sound", dir);
	assert(make_dir(path) == 0);
	snprintf(path, sizeof path, "%s/sound/menu", dir);
	assert(make_dir(path) == 0);
	snprintf(path, sizeof path, "%s/sound/menu/achievement.wav", dir);
	/* A pack offers a different valid cue. The fixed approved bell must win. */
	FILE *src = fopen("assets/sound/menu/type.wav", "rb"),
		 *dst = fopen(path, "wb");
	assert(src && dst);
	int c;
	while((c = fgetc(src)) != EOF)
		assert(fputc(c, dst) != EOF);
	assert(!ferror(src));
	assert(fclose(src) == 0 && fclose(dst) == 0);
	gstate.config_user.root = json_value_init_object();
	assert(json_object_dotset_string(
			   json_value_get_object(gstate.config_user.root),
			   "paths.audiopack", dir)
		   == JSONSuccess);
	audio_init(false, 100); /* decode real registry, no speaker/device */
	assert(audio_registry_variant_count(SOUND_MENU_ACHIEVEMENT) == 1);
	const struct audio_sample* smp
		= audio_registry_variant(SOUND_MENU_ACHIEVEMENT, 0);
	assert(smp && smp->rate == 48000 && smp->frames == 36480);
	src = fopen(
		"tests/fixtures/achievement/achievement-02-warm-bell.wav",
		"rb");
	assert(src);
	assert(fseek(src, 44, SEEK_SET) == 0);
	for(unsigned i = 0; i < smp->frames; i++) {
		int lo = fgetc(src), hi = fgetc(src);
		assert(lo >= 0 && hi >= 0);
		assert(smp->pcm[i] == (int16_t)(lo | (hi << 8)));
	}
	assert(fgetc(src) == EOF && !ferror(src));
	assert(fclose(src) == 0);
	audio_shutdown();
	json_value_free(gstate.config_user.root);
	gstate.config_user.root = NULL;
	test_fixture_dir_cleanup(dir);
	puts("PASS exact approved Warm bell PCM (36480 frames / 48000 Hz), pack "
		 "override rejected");
}
static int captures(const char* pack, const char* out) {
	native = true;
	gstate.config_user.root = json_value_init_object();
	assert(json_object_dotset_string(
			   json_value_get_object(gstate.config_user.root),
			   "paths.texturepack", pack)
		   == JSONSuccess);
	assert(gfx_setup());
	assert(tex_init());
	blocks_init();
	items_init();
	render_item_init();
	gfx_update_light(1, 0, 0, light_lookup_overworld);
	char dir[256];
	test_fixture_dir("test_notice_capture", dir, sizeof dir);
	assert(make_dir(dir) == 0);
	assert(achievements_init(dir));
	char path[1024];
	snprintf(path, sizeof path, "%s/native-geometry.json", out);
	FILE* geometry = fopen(path, "wb");
	assert(geometry);
	fputs("[\n", geometry);
	unsigned capture = 0;
	for(int width = 640; width <= 854; width += 214) {
		glfwSetWindowSize(glfwGetCurrentContext(), width, 480);
		glfwPollEvents();
		for(int split = 0; split < 2; split++)
			for(int kind = 0; kind < 4; kind++) {
				unsigned owner = split ? 1 : 0;
				uint64_t gen = ++capture;
				char profile[17];
				snprintf(profile, sizeof profile, "capture%u", capture);
				assert(achievements_bind(owner, gen, profile,
										 ACH_MODE_REMOTE_SERVER)
					   == ACH_BIND_OK);
				achievement_notice_init();
				if(kind == 0) {
					const int chain[] = {0, 1, 2, 6, 7, 8};
					for(int i = 0; i < 6; i++) {
						capture_award(owner, gen, chain[i]);
						if(i < 5)
							achievements_notice_ack(owner);
					}
				}
				struct input_player_frame p
					= prompt(kind == 2	   ? INPUT_CAT_CLASSIC_CONTROLLER :
								 kind == 3 ? INPUT_CAT_GAMECUBE :
											 INPUT_CAT_NUNCHUK);
				const char* device = kind == 0 ? "none" :
					kind == 1				   ? "remote" :
					kind == 2				   ? "classic" :
												 "gc";
				int vy = split ? 240 : 0, vh = split ? 240 : 480;
				gfx_write_buffers(true, true, true);
				gfx_clear_buffers(80, 110, 128);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				gfx_matrix_texture(false, NULL);
				gfx_mode_gui();
				/* The two views have distinct pixels. Native framebuffer
				 * comparison, not a draw-counter double, proves P2 cannot
				 * modify the P1 pane. */
				if(split) {
					gfx_texture(false);
					gutil_texquad_col(0, 0, 0, 0, 0, 0, width, 240, 30, 65, 120,
									  255);
					gfx_texture(true);
					gutil_text(24, 160, "Player 1: native isolation fixture",
							   16, false);
				}
				gfx_scissor(true, 0, vy, width, vh);
				achievement_notice_update(time_add_ms((ptime_t) {0}, 0));
				assert(achievement_notice_render(owner, &p, 0, vy, width, vh,
												 false, NULL, 0, NULL));
				/* Clear the start-frame's clipped animation before the hold
				 * capture. */
				gfx_scissor(false, 0, 0, 0, 0);
				gfx_clear_buffers(80, 110, 128);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				if(split) {
					gfx_texture(false);
					gutil_texquad_col(0, 0, 0, 0, 0, 0, width, 240, 30, 65, 120,
									  255);
					gfx_texture(true);
					gutil_text(24, 160, "Player 1: native isolation fixture",
							   16, false);
				}
				uint8_t* before = malloc(width * 480 * 4);
				uint8_t* after = malloc(width * 480 * 4);
				assert(before && after);
				size_t fw, fh;
				gfx_copy_framebuffer(before, &fw, &fh);
				assert(fw == (size_t)width && fh == 480);
				gfx_scissor(true, 0, vy, width, vh);
				achievement_notice_update(time_add_ms((ptime_t) {0}, 1200));
				struct achievement_notice_frame f;
				assert(achievement_notice_render(owner, &p, 0, vy, width, vh,
												 false, NULL, 0, &f));
				assert(f.slide_y == 0);
				gfx_scissor(false, 0, 0, 0, 0);
				gfx_copy_framebuffer(after, &fw, &fh);
				if(split)
					assert(!memcmp(before, after, width * 240 * 4));
				snprintf(path, sizeof path, "%s/native-%s-%s-%d-%s.png", out,
						 kind ? "hint" : "toast", device, width,
						 split ? "split-p2" : "full");
				assert(lodepng_encode32_file(path, after, (unsigned)fw,
											 (unsigned)fh)
					   == 0);
				fprintf(geometry,
						"%s{\"screen\":\"%s\",\"device\":\"%s\",\"width\":%d,"
						"\"split\":%s,\"owner\":%u,\"card\":[%d,%d,%d,%d],"
						"\"slot\":[%d,%d,%d,%d],\"caption\":[%d,%d,%d,%d],"
						"\"name\":[%d,%d,%d,%d],\"glyph\":%d}",
						capture == 1 ? "" : ",\n", kind ? "hint" : "toast",
						device, width, split ? "true" : "false", owner,
						f.card.x, f.card.y, f.card.w, f.card.h, f.slot.x,
						f.slot.y, f.slot.w, f.slot.h, f.caption.x, f.caption.y,
						f.caption.w, f.caption.h, f.name.x, f.name.y, f.name.w,
						f.name.h, f.glyph);
				free(before);
				free(after);
				achievements_unbind(owner, gen);
			}
	}
	fputs("\n]\n", geometry);
	assert(fclose(geometry) == 0);
	achievement_notice_shutdown();
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
	puts("PASS native notice captures: full, P2 isolated, three live "
		 "controller glyphs, 640/854");
	return 0;
}
int main(int argc, char** argv) {
	if(argc == 4 && !strcmp(argv[1], "--capture"))
		return captures(argv[2], argv[3]);
	fifo();
	fifo_capacity();
	geometry();
	timing();
	bell_registry();
	return 0;
}
#endif
