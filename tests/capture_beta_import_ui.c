/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * PC captures of the "Import Beta 1.7.3" UI (ingame-import-1030, coder C):
 * the REAL Texture Packs screen and the REAL import screen drawn by their
 * own render2D into the PC OpenGL framebuffer (854x480, the TV GUI size),
 * with the bundled controls art and the shipped config_wii.json bindings per
 * controller family -- the same method as the glyphs-*-05ad10dd hub images.
 * The worker's state comes from screen_beta_import_test_override; no worker
 * runs and no SD card is touched. Not a ctest (it needs a GL window):
 *   capture_beta_import_ui <out_dir>        (run from the repo root)
 * Writes import-ui-<state>-<family>.png at 1x; the hub sheet is scaled 2x
 * nearest by the caller. */
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game_state.h"
#include "game/gui/screen.h"
#include "game/gui/screen_beta_import.h"
#include "graphics/gui_util.h"
#include "lodepng/lodepng.h"
#include "parson/parson.h"
#include "platform/gfx.h"
#include "platform/input_glyph.h"
#include "platform/input_owner.h"
#include "platform/texture.h"
#include "platform/time.h"

extern int screen_texpacks_test_import_override;

/* The family whose bindings the prompts show (config_wii.json input.*). */
static enum input_category s_cat = INPUT_CAT_NUNCHUK;
static int s_click = 4, s_close = 6, s_up = 0;

void __real_input_prompt_frame(unsigned player, struct input_player_frame* f);
void __wrap_input_prompt_frame(unsigned player, struct input_player_frame* f) {
	(void)player;
	memset(f, 0, sizeof(*f));
	f->connected = true;
	f->category = s_cat;
	for(int i = 0; i < IB_COUNT; i++)
		f->glyphs[i] = -1;
	f->glyphs[IB_GUI_UP] = input_glyph_key(s_cat, s_up);
	f->glyphs[IB_GUI_CLICK] = input_glyph_key(s_cat, s_click);
	f->glyphs[IB_INVENTORY] = input_glyph_key(s_cat, s_close);
}

static void family(int k) {
	static const enum input_category cat[3]
		= {INPUT_CAT_NUNCHUK, INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	static const int click[3] = {4, 204, 404}, close[3] = {6, 206, 407},
					 up[3] = {0, 200, 400};
	s_cat = cat[k];
	s_click = click[k];
	s_close = close[k];
	s_up = up[k];
}

static void shoot(struct screen* s, const char* out, const char* name) {
	gfx_write_buffers(true, true, true);
	gfx_clear_buffers(128, 128, 128);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	gfx_matrix_texture(false, NULL);
	gfx_mode_gui();
	s->render2D(s, gfx_width(), gfx_height());
	size_t w, h;
	uint8_t* px = malloc(854 * 480 * 4);
	assert(px);
	gfx_copy_framebuffer(px, &w, &h);
	assert(w == 854 && h == 480);
	char file[512];
	snprintf(file, sizeof(file), "%s/import-ui-%s.png", out, name);
	assert(lodepng_encode32_file(file, px, (unsigned)w, (unsigned)h) == 0);
	free(px);
	printf("wrote %s\n", file);
}

static void progress(bi_state st, int done, int total, const char* msg,
					 bool sounds, int count) {
	bi_progress p;
	memset(&p, 0, sizeof(p));
	p.state = st;
	p.done = done;
	p.total = total;
	snprintf(p.msg, sizeof(p.msg), "%s", msg);
	p.sounds_imported = sounds;
	p.sounds_count = count;
	screen_beta_import_test_override(&p);
	screen_beta_import.update(&screen_beta_import, 1.0F / 60.0F);
}

int main(int argc, char** argv) {
	assert(argc == 2);
	const char* out = argv[1];
	gstate.config_user.root = json_value_init_object();
	json_object_dotset_string(json_value_get_object(gstate.config_user.root),
							  "paths.texturepack", "texturepacks/beta-1.7.3");
	assert(gfx_setup());
	glfwSetWindowSize(glfwGetCurrentContext(), 854, 480);
	glfwPollEvents();
	assert(tex_init());

	/* pack-lag-1030: PC wall clock of the blocking part of a texture Apply
	 * (tex_init for the pack), 3 runs per pack. */
	{
		static const char* packs[3]
			= {"assets", "texturepacks/beta-1.7.3", "texturepacks/earth-and-ash"};
		for(int k = 0; k < 3; k++)
			for(int run = 0; run < 3; run++) {
				json_object_dotset_string(
					json_value_get_object(gstate.config_user.root),
					"paths.texturepack", packs[k]);
				ptime_t t0 = time_get();
				assert(tex_init());
				fprintf(stderr, "tex_init %s run %d: %d ms\n", packs[k], run,
						(int)time_diff_ms(t0, time_get()));
			}
		json_object_dotset_string(json_value_get_object(gstate.config_user.root),
								  "paths.texturepack", "texturepacks/beta-1.7.3");
		assert(tex_init());
	}

	static const char* fam[3] = {"remote", "classic", "gamecube"};
	for(int k = 0; k < 3; k++) {
		family(k);
		char name[64];
		screen_texturepacks_set_return(&screen_title);
		screen_texturepacks_set_ingame(false);
		screen_texpacks_test_import_override = 1;
		screen_set(&screen_texturepacks);
		snprintf(name, sizeof(name), "packs-button-%s", fam[k]);
		shoot(&screen_texturepacks, out, name);
		if(k == 0) {
			screen_texpacks_test_import_override = 0;
			screen_set(&screen_texturepacks);
			shoot(&screen_texturepacks, out, "packs-nojar");
		}

		bi_progress start;
		memset(&start, 0, sizeof(start));
		start.state = BI_SCANNING;
		screen_beta_import_test_override(&start);
		screen_beta_import_open();
		progress(BI_SCANNING, 0, 0, "Looking for b1.7.3.jar", false, 0);
		snprintf(name, sizeof(name), "scanning-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
		progress(BI_TEXTURES, 61, 142, "Reading textures", false, 0);
		snprintf(name, sizeof(name), "textures-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
		progress(BI_SOUNDS, 48, 175, "Converting sound/random/click.ogg", false, 0);
		snprintf(name, sizeof(name), "sounds-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
		progress(BI_SWAPPING, 0, 0, "Installing the pack", false, 0);
		snprintf(name, sizeof(name), "finishing-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
		screen_beta_import_test_request_cancel();
		progress(BI_SOUNDS, 52, 175, "Converting sound/step/grass1.ogg", false, 0);
		snprintf(name, sizeof(name), "cancelling-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
		progress(BI_CANCELLED, 0, 0, "", false, 0);
		snprintf(name, sizeof(name), "cancelled-%s", fam[k]);
		shoot(&screen_beta_import, out, name);

		screen_beta_import_open();
		progress(BI_DONE, 0, 0, "", true, 175);
		snprintf(name, sizeof(name), "done-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
		screen_beta_import_open();
		progress(BI_DONE, 0, 0, "", false, 0);
		snprintf(name, sizeof(name), "done-nosounds-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
		screen_beta_import_open();
		progress(BI_FAILED, 0, 0,
				 "This jar isn't Beta 1.7.3 (it's a newer version). Use b1.7.3.jar.",
				 false, 0);
		snprintf(name, sizeof(name), "failed-%s", fam[k]);
		shoot(&screen_beta_import, out, name);
	}
	screen_beta_import_test_override(NULL);
	return 0;
}
