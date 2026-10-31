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

/* "Loading sounds..." feedback. Design, authorship and the reason there is
   no progress indicator: loading_notice.h. */

#include "screen_load_world.h"
#include "loading_notice.h"

#include <stdbool.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/server_interface.h" /* the parked SRPC_LOAD_WORLD */
#include "../../platform/audio.h"			/* the async registry's state */
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h" /* input_prompt_frame: the hints */
#include "../../platform/input_owner.h"
#include "../../platform/time.h"
#include "../../platform/watchdog.h"
#include "screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../lodepng/lodepng.h"

static char s_capture_prefix[192]; /* "" = off */

void loading_notice_set_capture(const char* path_prefix) {
	snprintf(s_capture_prefix, sizeof(s_capture_prefix), "%s",
			 path_prefix ? path_prefix : "");
}

void loading_notice_capture(const char* tag) {
	if(!s_capture_prefix[0] || !tag || !gfx_can_present())
		return;
	size_t width = 0, height = 0;
	gfx_copy_framebuffer(NULL, &width, &height);
	if(!width || !height)
		return;
	void* image = malloc(width * height * 4);
	if(!image)
		return;
	gfx_copy_framebuffer(image, &width, &height);
	char name[256];
	snprintf(name, sizeof(name), "%s%s.png", s_capture_prefix, tag);
	lodepng_encode32_file(name, image, (unsigned)width, (unsigned)height);
	free(image);
}

void loading_notice_boot_layout(int width, int height, int scale, int w1,
								int w2, struct loading_notice_layout* out) {
	out->band_y = 0;
	out->band_h = 0;
	out->line1_x = (width - w1) / 2;
	out->line1_y = height / 2 - 10 * scale;
	out->line2_x = (width - w2) / 2;
	out->line2_y = height / 2 + 2 * scale;
}

void loading_notice_strip_layout(int width, int height, int scale, int w1,
								 int w2, struct loading_notice_layout* out) {
	out->band_h = 20 * scale + 24;
	out->band_y = height / 2 - out->band_h / 2;
	out->line1_x = (width - w1) / 2;
	out->line1_y = out->band_y + 8;
	out->line2_x = (width - w2) / 2;
	out->line2_y = out->band_y + 12 + 10 * scale;
}

void loading_notice_draw_boot(int width, int height) {
	const int scale = GFX_GUI_SCALE;
	struct loading_notice_layout l;
	loading_notice_boot_layout(
		width, height, scale,
		gutil_font_width(LOADING_NOTICE_SOUNDS, 8 * scale),
		gutil_font_width(LOADING_NOTICE_WAIT, 8 * scale), &l);

	/* The WHOLE background is painted, opaque, as geometry: the two
	   backends disagree about when a clear colour takes effect (GX applies
	   it at the next copy, GL at the next swap), and this frame must not
	   depend on either. No texture is sampled for it. */
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 255);
	gfx_texture(true);
	gutil_text(l.line1_x, l.line1_y, LOADING_NOTICE_SOUNDS, 8 * scale, true);
	gutil_text_rgb(l.line2_x, l.line2_y, LOADING_NOTICE_WAIT, 8 * scale, true,
				   0xA0, 0xA0, 0xA0);
}

void loading_notice_draw_strip(int width, int height, const char* primary) {
	const int scale = GFX_GUI_SCALE;
	struct loading_notice_layout l;
	loading_notice_strip_layout(width, height, scale,
								gutil_font_width(primary, 8 * scale),
								gutil_font_width(LOADING_NOTICE_WAIT,
												 8 * scale),
								&l);
	gfx_texture(false);
	gutil_texquad_col(0, l.band_y, 0, 0, 0, 0, width, l.band_h, 0, 0, 0, 200);
	gfx_texture(true);
	gutil_text(l.line1_x, l.line1_y, primary, 8 * scale, true);
	gutil_text_rgb(l.line2_x, l.line2_y, LOADING_NOTICE_WAIT, 8 * scale, true,
				   0xA0, 0xA0, 0xA0);
}

static void loading_notice_paint_boot_frame(bool capture) {
	gfx_clear_buffers(0, 0, 0);
	gfx_mode_gui();
	/* This is the FIRST draw of the process, before any world frame, and on
	   the PC backend only gfx_mode_world ever writes the texture-matrix
	   uniform: left at GL's all-zero default, every glyph texcoord collapses
	   to (0,0) and the text is alpha-discarded -- the first PC capture of
	   this frame was pure black (c3 brief02). GX defaults to identity, so the
	   Wii never had the problem; stated explicitly here it costs nothing
	   (the Wii side caches the last state). */
	gfx_matrix_texture(false, NULL);
	loading_notice_draw_boot(gfx_width(), gfx_height());
	if(capture)
		loading_notice_capture("boot"); /* no-op unless the debug key is set */
	gfx_finish(true);
}

int loading_notice_present_boot(void) {
	if(!gfx_can_present())
		return -1; /* headless unit test: nothing to draw on (gfx.h) */
	ptime_t start = time_get();
	float gpu_wait = 0.0F, vsync_wait = 0.0F;

	/* 1. paint + copy out, then the REAL present: the main loop's own
	      sequence (finish copies the EFB into the draw buffer, flip hands
	      that buffer to the display and takes a free one). */
	loading_notice_paint_boot_frame(true);
	gfx_flip_buffers(&gpu_wait, &vsync_wait);

	/* 2. the buffer the flip just handed back does NOT hold the notice, and
	      the main loop's first act is to flip it to the display: leave the
	      same image in it, so that first flip cannot flash a stale or
	      never-drawn frame between the notice and the first menu frame.
	      Copied out only -- no second flip, no dwell.
	   BOOT-ONLY hazard (hostile review 03 #2): gfx_setup pre-seeds the free
	   ring with the very buffer the display is still scanning out, so the
	   buffer just handed back may be LIVE until the next retrace latches
	   the notice. Wait that one retrace before painting into it, or the
	   old white frame tears into the notice for a field. */
	gfx_wait_vsync();
	loading_notice_paint_boot_frame(false);

	/* gfx_flip_buffers left the watchdog phase at its own GFX tag; nothing
	   is waiting on the GPU any more. A stall from here on (the sound scan,
	   the autoload) must not be reported as a graphics hang (review 03 #1). */
	watchdog_phase_main(WDM_IDLE);

	return (int)time_diff_ms(start, time_get());
}

/* ===== boot gate (BOOT-CREDITS-1028) =================================== */

enum loading_notice_boot_state loading_notice_boot_decide(
	const struct loading_notice_boot_inputs* in, const char** phase,
	const char** reason) {
	const char* dummy;
	if(!phase) phase = &dummy;
	if(!reason) reason = &dummy;
	*phase = "";
	*reason = "";
	if(in->textures_failed) {
		*reason = in->textures_reason && in->textures_reason[0]
			? in->textures_reason : "Textures couldn't be loaded.";
		return LOADING_NOTICE_BOOT_ERROR;
	}
	if(in->audio_failed && !in->audio_disabled) {
		*reason = in->audio_reason && in->audio_reason[0]
			? in->audio_reason : "Sounds couldn't be loaded.";
		return LOADING_NOTICE_BOOT_ERROR;
	}
	/* cosmetic: the bake worked; only its SD copy or its upload did not */
	bool bg_cosmetic = in->background_cache_write_failed
		|| in->background_upload_failed;
	if(in->background == LOADING_NOTICE_BG_FAILED && !bg_cosmetic) {
		*reason = in->background_reason && in->background_reason[0]
			? in->background_reason : "The menu background couldn't be made.";
		return LOADING_NOTICE_BOOT_ERROR;
	}
	if(!in->textures_done) {
		*phase = "textures";
		return LOADING_NOTICE_BOOT_WAIT;
	}
	if(!in->audio_done && !in->audio_disabled) {
		*phase = "sounds";
		return LOADING_NOTICE_BOOT_WAIT;
	}
	if(in->background != LOADING_NOTICE_BG_READY
	   && !(in->background == LOADING_NOTICE_BG_FAILED && bg_cosmetic)) {
		*phase = "the menu background";
		return LOADING_NOTICE_BOOT_WAIT;
	}
	return LOADING_NOTICE_BOOT_READY;
}

void loading_notice_cache_write_note(const char* publish_reason, char* out,
	unsigned size) {
	if(!out || !size)
		return;
	const char* r = publish_reason && publish_reason[0] ? publish_reason
		: "the SD card refused it";
	size_t n = strlen(r);
	while(n && (r[n - 1] == '.' || r[n - 1] == ' '))
		n--;
	snprintf(out, size, "Couldn't save the menu background: %.*s.", (int)n, r);
}

enum loading_notice_boot_choice loading_notice_boot_error_choice(bool a_edge,
	bool b_edge, bool retry_allowed) {
	if(a_edge && retry_allowed) return LOADING_NOTICE_BOOT_RETRY;
	if(b_edge) return LOADING_NOTICE_BOOT_EXIT;
	return LOADING_NOTICE_BOOT_NONE;
}

static void loading_notice_paint_custom(void (*paint)(void*), void* user) {
	gfx_clear_buffers(0, 0, 0);
	gfx_mode_gui();
	gfx_matrix_texture(false, NULL); /* see loading_notice_paint_boot_frame */
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, gfx_width(), gfx_height(), 0, 0, 0,
					  255);
	gfx_texture(true);
	paint(user);
	gfx_finish(true);
}

int loading_notice_present_custom(void (*paint)(void* user), void* user) {
	if(!gfx_can_present() || !paint)
		return -1; /* headless unit test */
	ptime_t start = time_get();
	float gpu_wait = 0.0F, vsync_wait = 0.0F;
	/* Same present sequence and boot-only retrace wait as
	 * loading_notice_present_boot (hostile review 03 #2). */
	loading_notice_paint_custom(paint, user);
	gfx_flip_buffers(&gpu_wait, &vsync_wait);
	gfx_wait_vsync();
	loading_notice_paint_custom(paint, user);
	watchdog_phase_main(WDM_IDLE);
	return (int)time_diff_ms(start, time_get());
}

int loading_notice_present_next(void (*paint)(void* user), void* user) {
	if(!gfx_can_present() || !paint)
		return -1; /* headless unit test */
	ptime_t start = time_get();
	float gpu_wait = 0.0F, vsync_wait = 0.0F;
	loading_notice_paint_custom(paint, user);
	gfx_flip_buffers(&gpu_wait, &vsync_wait);
	return (int)time_diff_ms(start, time_get());
}

/* ===== world-entry gate (asyncsnd-0926) ================================= */

/* One parked request at a time: a LOCAL world load (path) or a REMOTE connect
   (the connect screen is re-entered, and its reset queues a fresh request). */
enum gate_kind { GATE_LOCAL = 0, GATE_REMOTE };

static struct {
	bool parked;
	enum gate_kind kind;
	char path[512];
	struct screen* back;
	unsigned sent, dropped, retries;
	bool failed_shown; /* the FAILED page is up: A = retry, B = back */
} s_gate;

#ifdef CAVEWII_TESTING
/* Test seam: the gate's two inputs, injected instead of read from input.c
   (a headless test has no controller). -1 = read the real input. */
static int s_test_back = -1, s_test_retry = -1;
void loading_notice_gate_test_input(int back, int retry);
void loading_notice_gate_test_input(int back, int retry) {
	s_test_back = back;
	s_test_retry = retry;
}
#endif

/* Retry is a full registry reload; if the loader thread cannot be created it
   finishes pass C synchronously on this thread (~5 s for the Beta pack on the
   Wii, after the directory index) -- a DECLARED stall, like the pack screen's
   apply, never a spurious HANG report (review L2). Same value as
   screen_texturepacks.c's TEXPACKS_RELOAD_GRACE_MS. */
#define LOADING_NOTICE_RETRY_GRACE_MS 60000

/* The pre-gate sequence, verbatim: queue SRPC_LOAD_WORLD, and on success move
   to the loading screen. False = svin refused (the caller's failure branch,
   or the gate's own return to `back`). */
static bool gate_send_load(const char* world_path) {
	lworld_set_world_path(world_path); /* UI thread, before server owns the archive */
	struct server_rpc rpc;
	rpc.type = SRPC_LOAD_WORLD;
	string_init_set_str(rpc.payload.load_world.name, world_path);
	if(svin_rpc_send(&rpc)) {
		screen_set(&screen_load_world);
		return true;
	}
	string_clear(rpc.payload.load_world.name);
	return false;
}

bool loading_notice_gate_load_world(const char* world_path,
									struct screen* back) {
	if(!world_path)
		return false;
	if(!audio_registry_async_pending())
		return gate_send_load(world_path);
	/* Park it. One request at a time: a second one while parked replaces the
	   first (the player changed their mind on the gate's own Back and picked
	   another world -- the parked one was dropped with that Back anyway). */
	snprintf(s_gate.path, sizeof(s_gate.path), "%s", world_path);
	s_gate.kind = GATE_LOCAL;
	s_gate.back = back;
	s_gate.parked = true;
	s_gate.failed_shown = false;
	screen_set(&screen_sound_gate);
	return true;
}

bool loading_notice_gate_remote_connect(void) {
	if(!audio_registry_async_pending())
		return false;
	/* The connect request itself was CONSUMED and dropped by the caller
	   (finding-3 "consume, never retain"); the gate re-enters the connect
	   screen when the registry is whole, and that screen's reset queues a
	   fresh request from the same config. Back returns to the join screen. */
	s_gate.path[0] = '\0';
	s_gate.kind = GATE_REMOTE;
	s_gate.back = screen_multiplayer_setup ? screen_multiplayer_setup
										   : &screen_title;
	s_gate.parked = true;
	s_gate.failed_shown = false;
	screen_set(&screen_sound_gate);
	return true;
}

void loading_notice_gate_stats(struct loading_notice_gate_stats* out) {
	if(!out)
		return;
	out->parked = s_gate.parked;
	out->remote = s_gate.parked && s_gate.kind == GATE_REMOTE;
	out->sent = s_gate.sent;
	out->dropped = s_gate.dropped;
	out->retries = s_gate.retries;
	snprintf(out->path, sizeof(out->path), "%s", s_gate.parked ? s_gate.path : "");
}

enum loading_notice_gate_action
loading_notice_gate_decide(int async_state, bool back, bool retry) {
	/* Back wins over everything the same frame: a player leaving must never
	   be carried into a world by a completion that landed on the same tick. */
	if(back)
		return LOADING_NOTICE_GATE_BACK;
	switch(async_state) {
		case AUDIO_ASYNC_RUNNING: return LOADING_NOTICE_GATE_WAIT;
		case AUDIO_ASYNC_FAILED:
			return retry ? LOADING_NOTICE_GATE_RETRY : LOADING_NOTICE_GATE_FAILED;
		default: /* OFF (opt-in off / joined) or DONE: the registry is whole */
			return LOADING_NOTICE_GATE_SEND;
	}
}

static void gate_leave(void) {
	struct screen* back = s_gate.back ? s_gate.back : &screen_select_world;
	s_gate.parked = false;
	s_gate.failed_shown = false;
	screen_set(back);
}

static void screen_gate_reset(struct screen* s, int width, int height) {
	(void)s, (void)width, (void)height;
	input_pointer_enable(true);
}

static void screen_gate_update(struct screen* s, float dt) {
	(void)s, (void)dt;
	if(!s_gate.parked) {
		gate_leave(); /* nothing to wait for (re-entered without a request) */
		return;
	}
	struct audio_async_status st;
	audio_registry_async_status(&st);
	bool back = input_pressed(IB_INVENTORY) || input_pressed(IB_HOME)
		|| input_pressed(IB_GUI_CLICK_ALT);
	bool retry = input_pressed(IB_GUI_CLICK);
#ifdef CAVEWII_TESTING
	if(s_test_back >= 0)
		back = s_test_back != 0;
	if(s_test_retry >= 0)
		retry = s_test_retry != 0;
#endif
	switch(loading_notice_gate_decide((int)st.state, back, retry)) {
		case LOADING_NOTICE_GATE_WAIT: break;
		case LOADING_NOTICE_GATE_SEND:
			s_gate.parked = false;
			s_gate.sent++;
			if(s_gate.kind == GATE_REMOTE) {
				/* Re-enter the connect screen: its reset queues the connect,
				   which main.c's seam now takes (the registry is whole). */
				struct screen* next = screen_multiplayer_connect
					? screen_multiplayer_connect
					: s_gate.back;
				s_gate.failed_shown = false;
				screen_set(next);
			} else if(!gate_send_load(s_gate.path)) {
				gate_leave(); /* svin refused: back where the player was */
			}
			break;
		case LOADING_NOTICE_GATE_BACK:
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			s_gate.dropped++;
			gate_leave();
			break;
		case LOADING_NOTICE_GATE_RETRY:
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			s_gate.retries++;
			s_gate.failed_shown = false;
			/* A full reload: cancel + join the stalled loader (a genuine wedge
			   there is the main-thread hang the watchdog reports), reset the
			   arena and restart pass C under a new generation. The parked
			   request stays parked and is sent when that generation lands.
			   Declared to the watchdog like the pack screen's apply (L2). */
			watchdog_phase_main(WDM_PACK_RELOAD);
			watchdog_expect_stall_ms(LOADING_NOTICE_RETRY_GRACE_MS);
			audio_registry_reload();
			watchdog_phase_main(WDM_SCREEN_UPDATE);
			break;
		case LOADING_NOTICE_GATE_FAILED: s_gate.failed_shown = true; break;
	}
}

static void screen_gate_render2D(struct screen* s, int width, int height) {
	(void)s;
	const int scale = GFX_GUI_SCALE;
	gutil_bg();
	struct audio_async_status st;
	audio_registry_async_status(&st);
	if(s_gate.failed_shown || st.state == AUDIO_ASYNC_FAILED) {
		loading_notice_draw_strip(width, height, "Couldn't load sounds.");
	} else {
		loading_notice_draw_strip(width, height, LOADING_NOTICE_SOUNDS);
	}
	/* Below the authored band: the live count (REAL progress -- published
	   keys, not an invented spinner) and the controller hints. */
	struct loading_notice_layout l;
	loading_notice_strip_layout(width, height, scale, 0, 0, &l);
	int y = l.band_y + l.band_h + 6 * scale;
	char count[64];
	snprintf(count, sizeof(count), "%d / %d sounds ready", st.keys_done,
			 st.keys_total);
	gutil_text_rgb((width - gutil_font_width(count, 8 * scale)) / 2, y, count,
				   8 * scale, true, 0xA0, 0xA0, 0xA0);
	y += 14 * scale;
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	if(st.state == AUDIO_ASYNC_FAILED) {
		int w_retry = gutil_menu_hint_width_owned(&prompts, IB_GUI_CLICK, "Retry");
		int w_back = gutil_menu_hint_width_owned(&prompts, IB_INVENTORY, "Back");
		int gap = 16 * scale;
		int x = (width - (w_retry + gap + w_back)) / 2;
		gutil_menu_hint_owned(&prompts, x, y, IB_GUI_CLICK, "Retry");
		gutil_menu_hint_owned(&prompts, x + w_retry + gap, y, IB_INVENTORY,
							  "Back");
	} else {
		int w_back = gutil_menu_hint_width_owned(&prompts, IB_INVENTORY, "Back");
		gutil_menu_hint_owned(&prompts, (width - w_back) / 2, y, IB_INVENTORY,
							  "Back");
	}
}

struct screen screen_sound_gate = {
	.reset = screen_gate_reset,
	.update = screen_gate_update,
	.render2D = screen_gate_render2D,
	.render3D = NULL,
	.render_world = false,
};
