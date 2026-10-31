/*
	"Loading sounds..." feedback -- the AUTHORED geometry and wording (Astra
	ruling 1, tmp/jobs/c3-loading-sounds-0919, 2026-09-19), pinned without a GPU.

	Pure layout only: the numbers Astra wrote, in both aspect ratios, the
	centring rule, and the proof that the shared strip layout is the pack-switch
	strip screen_texturepacks.c always drew (so moving it changed no pixel).
	What the frame LOOKS like is proven by native captures, and that it is on
	the TV before the scan starts by the boot-trace order on hardware -- not
	here.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/gui/loading_notice.h"
#include "graphics/gfx_settings.h"

static void test_wording(void) {
	/* Static text, three fixed dots, no counter / percent / spinner glyphs. */
	assert(strcmp(LOADING_NOTICE_SOUNDS, "Loading sounds...") == 0);
	assert(strcmp(LOADING_NOTICE_TEXTURES, "Loading texture pack...") == 0);
	assert(strcmp(LOADING_NOTICE_WAIT, "Please wait.") == 0);
	assert(strpbrk(LOADING_NOTICE_SOUNDS, "0123456789%/") == NULL);
	assert(strpbrk(LOADING_NOTICE_WAIT, "0123456789%/") == NULL);
	printf("PASS wording\n");
}

static void check_boot(int w, int h, int s, int w1, int w2) {
	struct loading_notice_layout l;
	loading_notice_boot_layout(w, h, s, w1, w2, &l);
	assert(l.band_y == 0 && l.band_h == 0); /* full-screen black, no band */
	assert(l.line1_y == h / 2 - 10 * s);
	assert(l.line2_y == h / 2 + 2 * s);
	assert(l.line1_x == (w - w1) / 2);
	assert(l.line2_x == (w - w2) / 2);
	/* The two lines never overlap: 8*s tall text, 12*s between the tops. */
	assert(l.line2_y - l.line1_y == 12 * s);
	assert(l.line2_y - l.line1_y >= 8 * s);
}

static void test_boot_layout(void) {
	const int s = GFX_GUI_SCALE;
	check_boot(640, 480, s, 204, 144); /* 4:3 */
	check_boot(854, 480, s, 204, 144); /* 16:9: same geometry, wider centre */
	check_boot(640, 480, 1, 102, 72);
	check_boot(640, 480, 3, 306, 216);
	/* Same vertical placement in both aspect ratios. */
	struct loading_notice_layout a, b;
	loading_notice_boot_layout(640, 480, s, 204, 144, &a);
	loading_notice_boot_layout(854, 480, s, 204, 144, &b);
	assert(a.line1_y == b.line1_y && a.line2_y == b.line2_y);
	assert(b.line1_x - a.line1_x == (854 - 640) / 2);
	printf("PASS boot_layout\n");
}

/* The strip as screen_texturepacks.c drew it before the move, verbatim. */
static void legacy_strip(int width, int height, int scale, int w1, int w2,
						 struct loading_notice_layout* out) {
	int band_h = 40 * scale / 2 + 24;
	int band_y = height / 2 - band_h / 2;
	out->band_h = band_h;
	out->band_y = band_y;
	out->line1_x = (width - w1) / 2;
	out->line1_y = band_y + 8;
	out->line2_x = (width - w2) / 2;
	out->line2_y = band_y + 8 + 10 * scale + 4;
}

static void test_strip_layout(void) {
	static const int dims[][2] = {{640, 480}, {854, 480}, {640, 528},
								  {320, 240}};
	for(size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); d++)
		for(int s = 1; s <= 4; s++)
			for(int w1 = 0; w1 <= 400; w1 += 37) {
				struct loading_notice_layout now, was;
				loading_notice_strip_layout(dims[d][0], dims[d][1], s, w1,
											w1 / 2, &now);
				legacy_strip(dims[d][0], dims[d][1], s, w1, w1 / 2, &was);
				assert(memcmp(&now, &was, sizeof(now)) == 0);
				/* Astra's statement of the same numbers. */
				assert(now.band_h == 20 * s + 24);
				assert(now.band_y == dims[d][1] / 2 - now.band_h / 2);
				assert(now.line1_y == now.band_y + 8);
				assert(now.line2_y == now.band_y + 12 + 10 * s);
				/* Both lines sit inside the band. */
				assert(now.line2_y + 8 * s <= now.band_y + now.band_h);
			}
	printf("PASS strip_layout\n");
}

/* BOOT-CREDITS-1028: the boot gate's decision. */
static void test_boot_gate(void) {
	const char *phase, *reason;
	struct loading_notice_boot_inputs in = {.textures_done = false,
		.audio_done = false, .background = LOADING_NOTICE_BG_PENDING};
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_WAIT);
	assert(!strcmp(phase, "textures"));
	in.textures_done = true;
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_WAIT);
	assert(!strcmp(phase, "sounds"));
	in.audio_done = true;
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_WAIT);
	assert(!strcmp(phase, "the menu background"));
	in.background = LOADING_NOTICE_BG_READY;
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_READY);
	/* never a silent dirt-backed or silent title */
	in.background = LOADING_NOTICE_BG_FAILED;
	in.background_reason = "SD card is locked or full.";
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	assert(!strcmp(reason, "SD card is locked or full."));
	in.background_reason = NULL;
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	assert(reason[0]);
	in.background = LOADING_NOTICE_BG_READY;
	in.audio_failed = true;
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	assert(!strcmp(reason, "Sounds couldn't be loaded."));
	in.audio_reason = "Required sound menu.click is missing.";
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	assert(!strcmp(reason, "Required sound menu.click is missing."));
	/* texture failure first: it is the earliest phase */
	in.textures_failed = true;
	in.textures_reason = "terrain.png couldn't be decoded.";
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	assert(!strcmp(reason, "terrain.png couldn't be decoded."));
	in.textures_reason = NULL;
	assert(loading_notice_boot_decide(&in, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	assert(!strcmp(reason, "Textures couldn't be loaded."));
	assert(loading_notice_boot_error_choice(false, false, true) == LOADING_NOTICE_BOOT_NONE);
	assert(loading_notice_boot_error_choice(true, false, true) == LOADING_NOTICE_BOOT_RETRY);
	assert(loading_notice_boot_error_choice(false, true, true) == LOADING_NOTICE_BOOT_EXIT);
	assert(loading_notice_boot_error_choice(true, true, true) == LOADING_NOTICE_BOOT_RETRY);
	/* stalled pass-C worker alive: Exit only, A does nothing */
	assert(loading_notice_boot_error_choice(true, false, false) == LOADING_NOTICE_BOOT_NONE);
	assert(loading_notice_boot_error_choice(true, true, false) == LOADING_NOTICE_BOOT_EXIT);
	/* AUDIO_REGISTRY_DISABLED (no MEM2 arena): ready, no footer */
	struct loading_notice_boot_inputs dis = {.textures_done = true,
		.audio_done = false, .audio_disabled = true,
		.background = LOADING_NOTICE_BG_READY};
	assert(loading_notice_boot_decide(&dis, &phase, &reason) == LOADING_NOTICE_BOOT_READY);
	dis.background = LOADING_NOTICE_BG_PENDING; /* the bake still follows */
	assert(loading_notice_boot_decide(&dis, &phase, &reason) == LOADING_NOTICE_BOOT_WAIT);
	assert(!strcmp(phase, "the menu background"));
	/* cache write failed after a good bake: admitted (dirt), never refused */
	struct loading_notice_boot_inputs cw = {.textures_done = true,
		.audio_done = true, .background = LOADING_NOTICE_BG_FAILED,
		.background_reason = "Couldn't save the menu background: SD card is locked or full.",
		.background_cache_write_failed = true};
	assert(loading_notice_boot_decide(&cw, &phase, &reason) == LOADING_NOTICE_BOOT_READY);
	cw.background_cache_write_failed = false; /* a real bake failure still errors */
	assert(loading_notice_boot_decide(&cw, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	/* hw1028c: the bake and its save worked, the in-session upload failed
	   (MEM1). Admitted with the dirt + note, never a footer that waits for a
	   press (the autopilot boot sat 240 s on it). */
	struct loading_notice_boot_inputs up = {.textures_done = true,
		.audio_done = true, .background = LOADING_NOTICE_BG_FAILED,
		.background_reason = "The background couldn't be uploaded.",
		.background_upload_failed = true};
	assert(loading_notice_boot_decide(&up, &phase, &reason) == LOADING_NOTICE_BOOT_READY);
	up.audio_done = false; /* still waits for real loaders first */
	assert(loading_notice_boot_decide(&up, &phase, &reason) == LOADING_NOTICE_BOOT_WAIT);
	assert(!strcmp(phase, "sounds"));
	up.audio_done = true;
	up.background_upload_failed = false; /* without the witness: explicit Retry/Exit */
	assert(loading_notice_boot_decide(&up, &phase, &reason) == LOADING_NOTICE_BOOT_ERROR);
	assert(!strcmp(reason, "The background couldn't be uploaded."));
	char note[160];
	loading_notice_cache_write_note("SD card is locked or full.", note, sizeof(note));
	assert(!strcmp(note, "Couldn't save the menu background: SD card is locked or full."));
	loading_notice_cache_write_note("SD card refused to save the background.", note, sizeof(note));
	assert(!strcmp(note, "Couldn't save the menu background: SD card refused to save the background."));
	loading_notice_cache_write_note(NULL, note, sizeof(note));
	assert(!strcmp(note, "Couldn't save the menu background: the SD card refused it."));
	/* headless: nothing presented, the painter never runs */
	assert(loading_notice_present_custom(NULL, NULL) == -1);
	assert(loading_notice_present_next(NULL, NULL) == -1);
	printf("PASS boot_gate\n");
}

int main(void) {
	test_wording();
	test_boot_layout();
	test_strip_layout();
	test_boot_gate();
	printf("ALL PASS test_loading_notice\n");
	return 0;
}
