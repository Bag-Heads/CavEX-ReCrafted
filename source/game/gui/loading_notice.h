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

#ifndef GAME_GUI_LOADING_NOTICE_H
#define GAME_GUI_LOADING_NOTICE_H

/*
	"Loading sounds..." feedback (lane C, Astra ruling 1 of
	tmp/jobs/c3-loading-sounds-0919, 2026-09-19 -- Astra AUTHORED the visual;
	do not restyle it without a new ruling).

	The sound registry scan decodes every sound file on the MAIN thread:
	~31 s on the real Wii at boot (measured: audio_load ms=30815) and again
	on a sound-pack switch. Until now the TV showed one plain white frame
	for the whole boot scan. This module says what is happening -- a STATIC
	message, presented BEFORE the first byte is decoded, which then stays on
	the TV as the last presented frame for as long as the scan blocks.

	Deliberately no counter, bar, spinner or animated dots: nothing here
	can advance while the main thread is inside the scan, and "keys scanned"
	is neither variants decoded nor time elapsed -- any motion or number
	would be invented progress (Astra V2).

	Geometry (s = GFX_GUI_SCALE, W/H = full GUI size, every line centred on
	its measured gutil_font_width, shadow on, identical in 4:3 and 16:9):
	  BOOT   opaque black #000000 full screen;
	         "Loading sounds..." white, 8*s, y = H/2 - 10*s;
	         "Please wait."      #A0A0A0, 8*s, y = H/2 + 2*s.
	  STRIP  full-width black alpha 200, band_h = 20*s + 24,
	         band_y = H/2 - band_h/2; primary white 8*s at band_y + 8;
	         "Please wait." #A0A0A0 8*s at band_y + 12 + 10*s.
	         (== the pack-switch strip screen_texturepacks.c always drew; the
	         texture phase keeps its own primary "Loading texture pack...".)
	  SCOPE  boot, and the MENU pack switch. The in-game pack screen never
	         re-decodes sounds (screen_texturepacks.c gates the audio half
	         on !ingame_mode, Astra ui9), so it never shows the sound phase.
*/

#include <stdbool.h>

#define LOADING_NOTICE_SOUNDS "Loading sounds..."
#define LOADING_NOTICE_TEXTURES "Loading texture pack..."
#define LOADING_NOTICE_WAIT "Please wait."

/* Pure geometry, so the authored numbers are testable without a GPU.
   `w1`/`w2` are the measured pixel widths of the two lines. */
struct loading_notice_layout {
	int band_y, band_h; /* STRIP only; 0/0 for BOOT */
	int line1_x, line1_y;
	int line2_x, line2_y;
};
void loading_notice_boot_layout(int width, int height, int scale, int w1,
								int w2, struct loading_notice_layout* out);
void loading_notice_strip_layout(int width, int height, int scale, int w1,
								 int w2, struct loading_notice_layout* out);

/* Draw into the CURRENT GUI-mode frame (caller owns clear/mode/present). */
void loading_notice_draw_boot(int width, int height);
void loading_notice_draw_strip(int width, int height, const char* primary);

/* BOOT: establish GUI render state, paint the whole notice and REALLY
   present it (the main loop's own finish + flip sequence -- on the Wii a
   bare gfx_finish(true) only copies the EFB, it does not select the
   displayed buffer), then paint it once more into the next draw buffer so
   the main loop's first flip shows the same image instead of a stale one.
   No dwell: returns as soon as the frame is on its way. Returns the time
   the whole thing took, in ms (the measured cost of the feedback itself),
   or -1 when nothing could be presented (headless unit test). */
int loading_notice_present_boot(void);

/* PROOF CAPTURE, debug only (config "debug.loading_notice_capture" = a path
   prefix; absent = off, the default, and then both calls below cost one
   pointer test). On a PC the scan takes well under a second, so the notice
   cannot be photographed from outside; this reads back the frame that was
   JUST DRAWN -- before it is copied out -- and writes <prefix><tag>.png. It
   adds no dwell and changes nothing that is presented. Never set in a play
   or release config. On the Wii give an ABSOLUTE prefix ("sd:/..."): it is
   not SD-root resolved, and the read-back + PNG encode cost ~3.7 MB of MEM1
   for a moment at boot. */
void loading_notice_set_capture(const char* path_prefix);
void loading_notice_capture(const char* tag);

/* ===== world-entry gate (asyncsnd-0926, Astra ruling 3) ===================
   With the opt-in `audio.async_load` on, pass C of the sound registry is
   still loading behind the title (platform/audio.h "async pass C"). A local
   world may not start loading until the SELECTED generation is complete, so
   every local world-load request goes through here:

     - registry complete (or the opt-in off): the SRPC_LOAD_WORLD is sent NOW
       and the loading screen is set -- exactly the pre-gate sequence;
     - pass C pending: the request is PARKED and screen_sound_gate is shown --
       the authored strip (LOADING_NOTICE_SOUNDS) over the menu background,
       the live key count, and a Back hint. Its update polls the registry:
       complete -> the parked request is sent and the loading screen follows;
       Back -> the request is dropped and `back` is restored (the loader keeps
       running behind the title, nothing is replayed); a progress timeout ->
       "Couldn't load sounds." with Retry (a full registry reload, new
       generation) and Back.

   Returns false only when a request that could be sent right now was refused
   by svin_rpc_send (the caller's pre-gate failure branch); a parked request
   returns true. The remote-connection half of the same gate lives in main.c's
   provider-swap seam (the pending connect is not taken while
   audio_registry_async_pending()). */
struct screen;
bool loading_notice_gate_load_world(const char* world_path,
									struct screen* back);
/* The REMOTE half, called by main.c's provider-swap seam after it CONSUMED a
   pending connect request while the registry is still pending: shows the same
   gate; when the registry is whole it re-enters screen_multiplayer_connect
   (whose reset queues a fresh request from the same config), Back returns to
   the join screen, a progress timeout offers Retry / Back exactly like a local
   load. False (nothing shown) when no run is pending. */
bool loading_notice_gate_remote_connect(void);
extern struct screen screen_sound_gate;
/* Test/diagnostic view of the gate: is a request parked, and how many parked
   requests were sent / dropped / retried since boot. */
struct loading_notice_gate_stats {
	bool parked;
	bool remote; /* the parked request is a remote connect */
	unsigned sent, dropped, retries;
	char path[512];
};
void loading_notice_gate_stats(struct loading_notice_gate_stats* out);
/* The gate's per-frame decision, pure so it is testable without a GPU:
   `state` is the registry's async state, `back`/`retry` the pressed inputs. */
enum loading_notice_gate_action {
	LOADING_NOTICE_GATE_WAIT = 0, /* keep showing the strip */
	LOADING_NOTICE_GATE_SEND,	  /* registry complete: send the request */
	LOADING_NOTICE_GATE_BACK,	  /* player left: drop the request */
	LOADING_NOTICE_GATE_RETRY,	  /* FAILED + retry: reload the registry */
	LOADING_NOTICE_GATE_FAILED	  /* FAILED, no input: show the error */
};
enum loading_notice_gate_action
loading_notice_gate_decide(int async_state, bool back, bool retry);

/* ===== boot gate (BOOT-CREDITS-1028) =====================================
   The boot credit cards (startup_credits.h) replace the routine "Loading
   sounds..." boot frame. main.c owns the cards; this is the gate's pure
   decision -- may the title be admitted, is something still loading (and
   what, for the stall footer), or did a required asset fail -- plus the
   presenter for frames shown before the main loop exists. */
enum loading_notice_boot_bg {
	LOADING_NOTICE_BG_READY = 0, /* bundled/author/verified cache, uploaded */
	LOADING_NOTICE_BG_PENDING,   /* the boot bake is running */
	LOADING_NOTICE_BG_FAILED,    /* the bake/publish failed (reason given) */
};
struct loading_notice_boot_inputs {
	bool textures_done;  /* tex_ready_state() == TEX_READY */
	bool textures_failed;
	const char* textures_reason;
	bool audio_done;     /* audio_registry_ready_state() == READY */
	bool audio_failed;
	const char* audio_reason;
	/* AUDIO_REGISTRY_DISABLED: permanent (no MEM2 arena / silent protected
	   keys); the pre-T2b silent boot. Ready, never a footer. */
	bool audio_disabled;
	int background;      /* enum loading_notice_boot_bg */
	const char* background_reason;
	/* The bake SUCCEEDED but its SD cache write failed (full, read-only,
	   write/rename/encode). Never a boot refusal: the title is admitted with
	   the pack's dirt (gutil_bg); the pack screen carries the note. */
	bool background_cache_write_failed;
	/* The bake and its cache write SUCCEEDED but the in-session GPU upload
	   failed (hw1028c: MEM1 too fragmented after the bake). The same as a
	   cache-write failure: admitted with the pack's dirt, the pack screen
	   carries the note; the saved cache loads on the next boot. */
	bool background_upload_failed;
};
enum loading_notice_boot_state {
	LOADING_NOTICE_BOOT_WAIT = 0,
	LOADING_NOTICE_BOOT_READY,
	LOADING_NOTICE_BOOT_ERROR,
};
/* `phase` (WAIT): the real step for "Still loading <phase>..."; `reason`
   (ERROR): the player-facing failure. Order: textures, sounds, background.
   A background FAILED only by its cache write or its in-session upload is
   READY (dirt backdrop). */
enum loading_notice_boot_state loading_notice_boot_decide(
	const struct loading_notice_boot_inputs* in, const char** phase,
	const char** reason);
/* The pack-screen note for a failed cache write: "Couldn't save the menu
   background: <publish reason without its final period>" (the screen
   appends " Apply Pack retries."). */
void loading_notice_cache_write_note(const char* publish_reason, char* out,
	unsigned size);

/* ERROR page: A retries (only when `retry_allowed`: e.g. not while a stalled
   pass-C worker still owns the sound arena), B exits; nothing else. */
enum loading_notice_boot_choice {
	LOADING_NOTICE_BOOT_NONE = 0,
	LOADING_NOTICE_BOOT_RETRY,
	LOADING_NOTICE_BOOT_EXIT,
};
enum loading_notice_boot_choice loading_notice_boot_error_choice(bool a_edge,
	bool b_edge, bool retry_allowed);
/* Paint one frame with `paint` (black GUI frame already set up) and REALLY
   present it, exactly like loading_notice_present_boot. -1 headless. */
int loading_notice_present_custom(void (*paint)(void* user), void* user);
/* The next frame of an already-presented sequence (inside a loader
   service): paint + present once, no extra retrace wait or back-buffer
   repaint. -1 headless. */
int loading_notice_present_next(void (*paint)(void* user), void* user);

#endif
