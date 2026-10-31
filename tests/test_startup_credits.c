/*
	BOOT-CREDITS-1028: startup credit cards -- shipped data, timing,
	readiness, A handling, footer, marker.

	Runs from the repo root (the shipped assets/gui/startup-credits data).
	The shipped cards are composited from the two textures exactly as the
	game's quad callback draws them (nearest-scaled, tinted, alpha over
	black) and compared with the approved v3 stills: card 1 (idle frame 0),
	card 2, and card 2 with the stall footer.
*/

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/gui/startup_credits.h"
#include "lodepng/lodepng.h"
#include "support/test_fixture_dir.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#define DIR_SHIPPED "assets/gui/startup-credits"
/* Tracked goldens (from design-opus-v3; card 1 with its art blanked: the
 * art is checked against the shipped B2-taps source frame). */
#define V3 "tests/fixtures/startup_credits/"
#define V4 "docs/ui_design/startup-credits-prototype/frames-B2-taps/"

static struct startup_credits_data g_data;

/* ---- composite quads like the renderer (texture x tint, alpha over) ---- */
struct canvas { uint8_t* px; int quads; };
static void blit(void* user, int tex, int sx, int sy, int sw, int sh, int dx,
				 int dy, int dw, int dh, uint8_t r, uint8_t g, uint8_t b,
				 uint8_t alpha) {
	struct canvas* c = user;
	const struct sc_texture* t = &g_data.tex[tex];
	const uint8_t tint[3] = {r, g, b};
	c->quads++;
	for(int j = 0; j < dh; j++)
		for(int i = 0; i < dw; i++) {
			int tx = sx + i * sw / dw, ty = sy + j * sh / dh; /* nearest */
			const uint8_t* s = t->rgba + ((size_t)ty * t->w + tx) * 4;
			int x = dx + i, y = dy + j;
			if(x < 0 || y < 0 || x >= 640 || y >= 480) continue;
			uint8_t* d = c->px + ((size_t)y * 640 + x) * 4;
			unsigned a = s[3] * alpha / 255;
			for(int k = 0; k < 3; k++) {
				unsigned col = s[k] * tint[k] / 255;
				d[k] = (uint8_t)((col * a + d[k] * (255 - a)) / 255);
			}
		}
}

/* Advance like presented frames (16 ms); edges only on the last frame. */
static void step(struct startup_credits* s, uint32_t to, bool ready,
				 bool a_edge, bool a_held) {
	while(!startup_credits_done(s) && (int32_t)(to - s->now_ms) > 16)
		startup_credits_update(s, s->now_ms + 16, ready, false, a_held);
	startup_credits_update(s, to, ready, a_edge, a_held);
}

static uint8_t g_px[640 * 480 * 4];
/* Compare the last composited frame with a 640x480 still, optionally
 * skipping one rectangle (x, y, w, h); w = 0 compares everything. */
static unsigned long diff_still(const char* png, int mx, int my, int mw, int mh) {
	unsigned char* ref = NULL;
	unsigned w, h;
	assert(!lodepng_decode32_file(&ref, &w, &h, png) && w == 640 && h == 480);
	unsigned long diff = 0;
	for(int y = 0; y < 480; y++)
		for(int x = 0; x < 640; x++) {
			if(mw && x >= mx && x < mx + mw && y >= my && y < my + mh) continue;
			size_t i = (size_t)y * 640 + x;
			for(int k = 0; k < 3; k++)
				diff += abs((int)g_px[i * 4 + k] - (int)ref[i * 4 + k]) > 2;
		}
	free(ref);
	return diff;
}

/* The art rectangle against a 192x192 frame PNG composited over black. */
static unsigned long diff_frame(const char* png, int ax, int ay) {
	unsigned char* f = NULL;
	unsigned w, h;
	assert(!lodepng_decode32_file(&f, &w, &h, png) && w == 192 && h == 192);
	unsigned long diff = 0;
	for(unsigned y = 0; y < 192; y++)
		for(unsigned x = 0; x < 192; x++) {
			const uint8_t* s = f + ((size_t)y * 192 + x) * 4;
			const uint8_t* d = g_px + ((size_t)(ay + y) * 640 + ax + x) * 4;
			for(int k = 0; k < 3; k++)
				diff += abs((int)(s[k] * s[3] / 255) - (int)d[k]) > 2;
		}
	free(f);
	return diff;
}

static void compare_still(const struct startup_credits* s, const char* png) {
	static uint8_t px[640 * 480 * 4];
	memset(px, 0, sizeof(px));
	struct canvas c = {px, 0};
	startup_credits_draw(s, blit, &c);
	unsigned char* ref = NULL;
	unsigned w, h;
	assert(!lodepng_decode32_file(&ref, &w, &h, png) && w == 640 && h == 480);
	unsigned long diff = 0, worst = 0;
	for(size_t i = 0; i < (size_t)640 * 480; i++)
		for(int k = 0; k < 3; k++) {
			int d = abs((int)px[i * 4 + k] - (int)ref[i * 4 + k]);
			diff += d > 2;
			if((unsigned)d > worst) worst = d;
		}
	free(ref);
	printf("  %s: %d quads, %lu channel samples differ by >2 (worst %lu)\n",
		   png, c.quads, diff, worst);
	assert(diff == 0);
}

static void test_shipped(void) {
	assert(startup_credits_load(&g_data, DIR_SHIPPED) == SC_LOAD_OK);
	assert(g_data.ntextures == 2 && g_data.ncards == 2);
	size_t bytes = 0;
	for(int k = 0; k < 2; k++) {
		assert(g_data.tex[k].rgba);
		bytes += (size_t)g_data.tex[k].w * g_data.tex[k].h * 4;
	}
	assert(bytes == 950272 && bytes <= STARTUP_CREDITS_TEXTURE_BUDGET);
	assert(g_data.fade_in_ms == 200 && g_data.dwell_ms == 3000
		   && g_data.fade_out_ms == 200 && g_data.stall_ms == 10000);
	assert(startup_credits_nominal_ms(&g_data) == 6800);
	const struct sc_item* art = &g_data.cards[0].items[1];
	assert(art->kind == SC_ITEM_ANIM && art->nsteps == 16 && art->loop_ms == 5700);
	for(int k = 0; k < art->nsteps; k++)
		assert(g_data.sprites[art->step_sprite[k]].w == 192
			   && g_data.sprites[art->step_sprite[k]].h == 192);
	/* every item stays in the 32 px safe area */
	for(int k = 0; k < g_data.ncards; k++)
		for(int i = 0; i < g_data.cards[k].nitems; i++) {
			const struct sc_item* it = &g_data.cards[k].items[i];
			int w = it->kind == SC_ITEM_ANIM ? g_data.sprites[it->step_sprite[0]].w
				: startup_credits_text_width(&g_data, it->text, it->size) + it->size / 8;
			int h = it->kind == SC_ITEM_ANIM ? g_data.sprites[it->step_sprite[0]].h
				: it->size + it->size / 8;
			assert(it->x >= 32 && it->y >= 32 && it->x + w <= 608 && it->y + h <= 448);
		}
	struct startup_credits s;
	startup_credits_begin(&s, &g_data, true, 0);
	step(&s, 400, false, false, false); /* B2 step 0 (0-499 ms) = frame 0, readable */
	memset(g_px, 0, sizeof(g_px));
	struct canvas c1 = {g_px, 0};
	startup_credits_draw(&s, blit, &c1);
	assert(diff_still(V3 "card1-text.png", 224, 123, 192, 192) == 0);
	assert(diff_frame(V4 "frame_0.png", 224, 123) == 0);
	printf("  card 1: text matches the v3 still, art = B2-taps frame 0\n");
	step(&s, 4000, false, false, false);
	compare_still(&s, V3 "card2-built-on.png");
	startup_credits_set_phase(&s, "sounds");
	step(&s, 16801, false, false, false);
	assert(startup_credits_footer_state(&s) == SC_FOOTER_STALL);
	compare_still(&s, V3 "card2-stall-footer.png");
	puts("PASS shipped data: 2 cards, 200/3000/200/10000 ms, B2-taps 16-step 5.7 s idle loop, "
		 "950,272 B of textures, safe area, cards + stall footer match the v3 stills");
}

static int vis_card(const struct startup_credits* s, int* alpha) {
	struct sc_visible v[1];
	if(!startup_credits_visible(s, v)) return -1;
	*alpha = v[0].alpha;
	return v[0].card;
}

static void test_first_boot(void) {
	struct startup_credits s;
	int a;
	startup_credits_begin(&s, &g_data, true, 100000);
	/* ready almost at once; A mashed throughout: ignored */
	for(uint32_t t = 0; t < 6600; t += 50) {
		step(&s, 100000 + t, t >= 300, true, true);
		if(t < 200) assert(vis_card(&s, &a) == 0 && a < 255);
		if(t >= 200 && t < 3200) assert(vis_card(&s, &a) == 0 && a == 255);
		if(t > 3200 && t < 3400) assert(vis_card(&s, &a) == 0 && a < 255);
		if(t > 3400 && t < 3600) assert(vis_card(&s, &a) == 1 && a < 255);
		if(t >= 3600) assert(vis_card(&s, &a) == 1 && a == 255);
		assert(s.phase == SC_SHOWING);
		assert(startup_credits_footer_state(&s) == SC_FOOTER_NONE);
	}
	/* both dwells done: card 2's fade-out is the exit; A still ignored */
	step(&s, 106600, true, true, true);
	assert(s.phase == SC_EXITING);
	step(&s, 106700, true, true, true);
	assert(vis_card(&s, &a) == 1 && a > 0 && a < 255 && !startup_credits_done(&s));
	step(&s, 106800, true, false, true);
	assert(startup_credits_done(&s));
	assert(startup_credits_title_input_blocked(&s, true)); /* held A */
	assert(!startup_credits_title_input_blocked(&s, false));
	assert(startup_credits_marker_due(&s));
	startup_credits_marker_noted(&s);
	assert(!startup_credits_marker_due(&s));
	/* animation runs through the fade-in: frame = (ms / 150) % 6 */
	struct startup_credits q;
	startup_credits_begin(&q, &g_data, true, 0);
	step(&q, 160, false, false, false);
	puts("PASS first boot: 200 ms fade in, 3 s readable, 200 ms fade out per card, A "
		 "ignored, exit only after both dwells, held A never reaches the title, marker due once");
}

static void test_first_boot_stall(void) {
	struct startup_credits s;
	int a;
	char foot[128];
	startup_credits_begin(&s, &g_data, true, 0);
	startup_credits_set_phase(&s, "the menu background");
	for(uint32_t t = 0; t <= 16800; t += 100) {
		step(&s, t, false, false, false);
		assert(startup_credits_footer_state(&s) == SC_FOOTER_NONE);
		startup_credits_footer_text(&s, foot, sizeof(foot));
		assert(!foot[0]);
	}
	step(&s, 16801, false, false, false);
	assert(startup_credits_footer_state(&s) == SC_FOOTER_STALL);
	startup_credits_footer_text(&s, foot, sizeof(foot));
	assert(!strcmp(foot, "Still loading the menu background..."));
	assert(vis_card(&s, &a) == 1 && a == 255); /* thanks card holds */
	assert(!startup_credits_marker_due(&s));
	step(&s, 30000, true, false, false);
	step(&s, 30200, true, false, false);
	assert(startup_credits_done(&s) && startup_credits_marker_due(&s));
	puts("PASS stall: footer empty until 10 s past the 6.8 s sequence, then \"Still "
		 "loading <real phase>...\" on the held thanks card; exits once ready");
}

static void record_frame(void* user, int tex, int sx, int sy, int sw, int sh,
						 int dx, int dy, int dw, int dh, uint8_t r, uint8_t g,
						 uint8_t b, uint8_t a) {
	(void)sw; (void)sh; (void)dx; (void)dy; (void)dw; (void)dh;
	(void)r; (void)g; (void)b; (void)a;
	if(tex == 0) *(int*)user = sx * 1000 + sy; /* frame sheet cell */
}

static void test_animation(void) {
	struct startup_credits s;
	startup_credits_begin(&s, &g_data, false, 0);
	const struct sc_item* art = &g_data.cards[0].items[1];
	for(uint32_t t = 75; t < 1800; t += 75) { /* t=0 is fully transparent */
		step(&s, t, false, false, false);
		int sx = -1;
		startup_credits_draw(&s, record_frame, &sx);
		const struct sc_sprite* want =
			&g_data.sprites[art->step_sprite[startup_credits_anim_step(art, t)]];
		assert(sx == want->x * 1000 + want->y); /* runs during the fade-in too */
	}
	puts("PASS idle loop: drawn frame follows the step table from card 1's start, including fades");
}

/* B2-taps table: loops seamlessly, every step valid. Glow level per frame
 * (design-opus-v4/NOTES.md B2): 0 rest, 1 +2%, 2 +4%, 3 +6%, 4 +8%, 5 -2%. */
static void test_b2_table(void) {
	static const int level[6] = {0, 1, 2, 3, 4, -1};
	static const struct { int frame, ms; } want[16] = {
		{0, 500}, {1, 300}, {2, 300}, {3, 400}, {2, 300}, {1, 300}, {0, 600},
		{5, 400}, {0, 400},
		{1, 250}, {2, 250}, {3, 300}, {4, 500}, {3, 300}, {2, 300}, {1, 300}};
	const struct sc_item* art = &g_data.cards[0].items[1];
	assert(art->nsteps == 16 && art->loop_ms == 5700);
	uint32_t sum = 0;
	int frame_of[16];
	for(int k = 0; k < 16; k++) {
		/* valid: a 192x192 cell of the frame sheet, one of its 6 frames */
		int sp = art->step_sprite[k];
		assert(sp >= 0 && sp < g_data.nsprites);
		const struct sc_sprite* s = &g_data.sprites[sp];
		assert(s->tex == 0 && s->w == 192 && s->h == 192);
		int f = (s->y / 192) * 3 + s->x / 192;
		assert(f >= 0 && f < 6 && f == want[k].frame && art->step_ms[k] == want[k].ms);
		frame_of[k] = f;
		sum += art->step_ms[k];
	}
	assert(sum == 5700);
	/* seamless: every step, the wrap included, moves exactly one glow level */
	for(int k = 0; k < 16; k++) {
		int d = level[frame_of[(k + 1) % 16]] - level[frame_of[k]];
		assert(d == 1 || d == -1);
	}
	/* periodic, and each step starts exactly where the previous ended */
	uint32_t t0 = 0;
	for(int k = 0; k < 16; k++) {
		assert(startup_credits_anim_step(art, t0) == k);
		assert(startup_credits_anim_step(art, t0 + art->step_ms[k] - 1) == k);
		assert(startup_credits_anim_step(art, t0 + 5700 * 3) == k);
		t0 += art->step_ms[k];
	}
	assert(startup_credits_anim_step(art, 5699) == 15 && startup_credits_anim_step(art, 5700) == 0);
	puts("PASS B2-taps table: 16 steps = 5,700 ms, every step a valid sheet frame, "
		 "one glow level per step incl. the wrap, periodic");
}

static void test_later_boot(void) {
	struct startup_credits s;
	int a;
	/* not ready yet: cards advance; ready at 3500 (card 2 fading in) */
	startup_credits_begin(&s, &g_data, false, 0);
	step(&s, 100, false, true, true); /* pre-ready A: not buffered */
	step(&s, 3450, false, false, true);
	assert(vis_card(&s, &a) == 1 && a < 255);
	step(&s, 3500, true, false, true);
	assert(s.phase == SC_EXITING);
	int from = s.exit_alpha;
	step(&s, 3600, true, false, true);
	assert(vis_card(&s, &a) == 1 && a < from && !startup_credits_done(&s));
	step(&s, 3700, true, false, true);
	assert(startup_credits_done(&s));
	assert(startup_credits_title_input_blocked(&s, true));
	assert(!startup_credits_title_input_blocked(&s, false));
	assert(!startup_credits_marker_due(&s));

	/* ready immediately: zero forced dwell */
	startup_credits_begin(&s, &g_data, false, 0);
	step(&s, 16, true, true, true); /* edge on the ready frame */
	assert(s.phase == SC_EXITING);
	step(&s, 32, true, false, false);
	assert(!startup_credits_done(&s));
	step(&s, 48, true, true, true); /* fresh A after ready: cut */
	assert(startup_credits_done(&s));
	assert(startup_credits_title_input_blocked(&s, true)); /* never a title press */
	assert(!startup_credits_title_input_blocked(&s, false));
	assert(!startup_credits_title_input_blocked(&s, true)); /* a NEW press is the title's */

	/* without A: the fade-out completes in 200 ms */
	startup_credits_begin(&s, &g_data, false, 0);
	step(&s, 10, true, false, false);
	step(&s, 209, true, false, false);
	assert(!startup_credits_done(&s));
	step(&s, 210, true, false, false);
	assert(startup_credits_done(&s) && !startup_credits_title_input_blocked(&s, false));
	puts("PASS later boot: no forced dwell, exit as soon as ready, pre-ready A never "
		 "buffered, fresh A cuts the fade and never reaches the title");
}

static void test_blocked_frames(void) {
	struct startup_credits s;
	int a;
	/* A 31 s frame the main thread could not present (the sound scan):
	 * at most 100 ms of credit time passes; card 1 is still readable and
	 * the first-boot dwells still lie ahead. */
	startup_credits_begin(&s, &g_data, true, 0);
	step(&s, 300, false, false, false);
	startup_credits_update(&s, 31300, true, false, false);
	assert(s.clock_ms == 400 && vis_card(&s, &a) == 0 && a == 255);
	assert(s.phase == SC_SHOWING && !startup_credits_marker_due(&s));
	assert(startup_credits_footer_state(&s) == SC_FOOTER_NONE);
	puts("PASS blocked frames: an unpresented 31 s gap advances the cards by 100 ms only");
}

static void test_exit_revoked(void) {
	struct startup_credits s;
	int a;
	/* first boot: exit starts, then readiness is revoked mid-fade */
	startup_credits_begin(&s, &g_data, true, 0);
	step(&s, 6600, true, false, false);
	assert(s.phase == SC_EXITING);
	step(&s, 6650, false, false, false);
	assert(s.phase == SC_SHOWING);
	step(&s, 9000, false, false, false); /* well past the old fade end */
	assert(!startup_credits_done(&s) && !startup_credits_marker_due(&s));
	assert(vis_card(&s, &a) == 1 && a == 255);
	/* an error mid-fade also cancels the exit */
	step(&s, 9100, true, false, false);
	assert(s.phase == SC_EXITING);
	startup_credits_set_error(&s, "Sounds couldn't be loaded.");
	step(&s, 9200, true, false, false);
	assert(s.phase == SC_SHOWING);
	step(&s, 12000, true, false, false);
	assert(!startup_credits_done(&s) && !startup_credits_marker_due(&s));
	startup_credits_set_error(&s, NULL);
	step(&s, 12300, true, false, false);
	assert(startup_credits_done(&s) && startup_credits_marker_due(&s));
	puts("PASS exit revoked: readiness lost or an error mid-fade brings the cards back; "
		 "never DONE or marker_due until ready again");
}

static void test_stall_real_time(void) {
	struct startup_credits s;
	/* one serviced frame per second: the presented clock crawls (100 ms per
	 * update) but the stall footer follows real elapsed time */
	startup_credits_begin(&s, &g_data, true, 0);
	for(uint32_t t = 1000; t <= 16000; t += 1000) {
		startup_credits_update(&s, t, false, false, false);
		assert(startup_credits_footer_state(&s) == SC_FOOTER_NONE);
	}
	startup_credits_update(&s, 17000, false, false, false);
	assert(s.clock_ms == 1700); /* dwell clock: presented time only */
	assert(startup_credits_footer_state(&s) == SC_FOOTER_STALL);
	puts("PASS stall timer: real elapsed time (16.8 s), independent of the capped dwell clock");
}

static void test_error(void) {
	struct startup_credits s;
	char foot[128];
	startup_credits_begin(&s, &g_data, false, 0);
	startup_credits_set_error(&s, "Sounds could not be loaded.");
	step(&s, 100, true, false, false);
	assert(s.phase == SC_SHOWING);
	assert(startup_credits_footer_state(&s) == SC_FOOTER_ERROR);
	startup_credits_footer_text(&s, foot, sizeof(foot));
	assert(!strcmp(foot, "Sounds could not be loaded."));
	startup_credits_set_error(&s, NULL);
	step(&s, 200, true, false, false);
	assert(s.phase == SC_EXITING);
	puts("PASS error: holds the cards with the reason in the footer; clears to exit");
}

#define HDR "startup-credits 2\ntexture frames f.bin 64 64\ntexture font t.bin 128 128\n"
static void test_parse(void) {
	static const char good[] = HDR
		"timing 100 1000 100 500\n"
		"sprite f0 frames 0 0 8 8\nsprite f1 frames 8 0 8 8\nsprite f2 frames 16 0 8 8\n"
		"card\nanim 100 100 120 f0 f1 f2\ntext 40 40 16 F5EEE3 Hello there\n"
		"footer 404 16 B8B8B8\nend\n";
	struct startup_credits_data d;
	assert(startup_credits_parse(&d, good) == SC_LOAD_OK);
	assert(startup_credits_nominal_ms(&d) == 1200);
	assert(!strcmp(d.cards[0].items[1].text, "Hello there"));
	static const char* bad[] = {
		"startup-credits 1\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 60 0 8 8\ncard\nanim 40 40 0 x\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nanim 636 40 0 x\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nanim 40 40 0 y\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nanim 40 40 0 x\nfooter 404 16 B8B8B8\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nanim 40 40 9 x x x x x x x x x x x x x x x x x x x x x x x x x\nfooter 404 16 B8B8B8\nend\n",
		"startup-credits 2\ntexture frames ../f.bin 64 64\ntexture font t.bin 128 128\ntiming 1 1000 1 1\ncard\ntext 40 40 16 FFFFFF a\nfooter 404 16 B8B8B8\nend\n",
		"startup-credits 2\ntexture frames f.bin 1024 1024\ntexture font t.bin 128 128\ntiming 1 1000 1 1\ncard\ntext 40 40 16 FFFFFF a\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\ncard\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nbogus\ncard\ntext 40 40 16 FFFFFF a\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\ncard\ntext 40 40 12 FFFFFF a\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\ncard\ntext 40 40 16 GGGGGG a\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\ncard\ntext 40 40 16 FFFFFF a\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 4294967295 0 9 8\ncard\nanim 40 40 0 x\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 4294967295 8 9\ncard\nanim 40 40 0 x\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 60 0 4294967295 8\ncard\nanim 40 40 0 x\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nseq 40 40 x\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nseq 40 40 x:0\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nseq 40 40 y:100\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nseq 40 40 x:10z\nfooter 404 16 B8B8B8\nend\n",
		HDR "timing 100 1000 100 1\nsprite x frames 0 0 8 8\ncard\nanim 40 40 100 x:100\nfooter 404 16 B8B8B8\nend\n",
		"startup-credits 2\ntexture frames f.bin 64 64\ntexture font t.bin 64 64\ntiming 1 1000 1 1\ncard\ntext 40 40 16 FFFFFF a\nfooter 404 16 B8B8B8\nend\n",
	};
	for(size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
		assert(startup_credits_parse(&d, bad[k]) == SC_LOAD_ERR_PARSE);
	puts("PASS manifest: data-driven steps/timing/text; 22 malformed manifests rejected (incl. wrapping sprite bounds)");
}

static void write_file(const char* path, const void* data, size_t n) {
	FILE* f = fopen(path, "wb");
	assert(f && fwrite(data, 1, n, f) == n && fclose(f) == 0);
}

static void test_load_failures(const char* root) {
	struct startup_credits_data d;
	char dir[256], p[300];
	assert(startup_credits_load(&d, "no/such/dir") == SC_LOAD_ERR_MANIFEST);
	snprintf(dir, sizeof(dir), "%s/data", root);
	assert(MKDIR(dir) == 0);
	static const char man[] = "startup-credits 2\ntexture frames a.bin 8 8\n"
		"texture font b.bin 128 128\ntiming 0 1000 0 1\nsprite x frames 0 0 8 8\n"
		"card\nanim 40 40 0 x\nfooter 404 16 B8B8B8\nend\n";
	snprintf(p, sizeof(p), "%s/credits.txt", dir);
	write_file(p, man, sizeof(man) - 1);
	char pa[300], pb[300];
	snprintf(pa, sizeof(pa), "%s/a.bin", dir);
	snprintf(pb, sizeof(pb), "%s/b.bin", dir);
	static uint8_t a[12 + 8 * 8 * 4 + 1], b[12 + 128 * 128 * 4];
	memset(a, 0x7f, sizeof(a));
	memcpy(a, "CWSA\0\0\0\x08\0\0\0\x08", 12);
	memset(b, 0, sizeof(b));
	memcpy(b, "CWSA\0\0\0\x80\0\0\0\x80", 12);
	write_file(pb, b, sizeof(b));
	assert(startup_credits_load(&d, dir) == SC_LOAD_ERR_TEXTURE); /* a missing */
	assert(!d.tex[0].rgba && !d.tex[1].rgba);
	write_file(pa, a, 12 + 100); /* short */
	assert(startup_credits_load(&d, dir) == SC_LOAD_ERR_TEXTURE);
	write_file(pa, a, sizeof(a)); /* trailing byte */
	assert(startup_credits_load(&d, dir) == SC_LOAD_ERR_TEXTURE);
	a[0] = 'X';
	write_file(pa, a, sizeof(a) - 1); /* bad magic */
	assert(startup_credits_load(&d, dir) == SC_LOAD_ERR_TEXTURE);
	a[0] = 'C';
	write_file(pa, a, sizeof(a) - 1);
	write_file(pb, b, 12 + 100); /* second texture short: first is released */
	assert(startup_credits_load(&d, dir) == SC_LOAD_ERR_TEXTURE);
	assert(!d.tex[0].rgba && !d.tex[1].rgba);
	write_file(pb, b, sizeof(b));
	assert(startup_credits_load(&d, dir) == SC_LOAD_OK && d.tex[0].rgba && d.tex[1].rgba);
	uint8_t* px = startup_credits_take_texture(&d, 0);
	assert(px && !d.tex[0].rgba && !startup_credits_take_texture(&d, 0));
	free(px);
	startup_credits_data_free(&d);
	puts("PASS load failures: missing manifest/texture, short, oversized, bad magic, "
		 "second texture bad -> error with nothing allocated; textures handed over once");
}

static void test_marker(const char* root) {
	char path[300], cfg[300];
	assert(startup_credits_marker_path("sd:/apps/cavewii/config.json", path, sizeof(path)));
	assert(!strcmp(path, "sd:/apps/cavewii/" STARTUP_CREDITS_MARKER));
	assert(startup_credits_marker_path("config.json", path, sizeof(path)));
	assert(!strcmp(path, STARTUP_CREDITS_MARKER));
	assert(startup_credits_marker_path("C:\\x\\config_pc.json", path, sizeof(path)));
	assert(!strcmp(path, "C:\\x/" STARTUP_CREDITS_MARKER));
	assert(!startup_credits_marker_path("config.json", path, 4));

	snprintf(cfg, sizeof(cfg), "%s/config.json", root);
	assert(startup_credits_marker_path(cfg, path, sizeof(path)));
	char tmp[320];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	assert(!startup_credits_marker_seen(path)); /* missing = first run */
	write_file(path, "garbage", 7);
	assert(!startup_credits_marker_seen(path)); /* corrupt = first run */
	for(int op = 0; op < 3; op++) {
		startup_credits_test_fail(op, 1);
		int r = startup_credits_marker_write(path);
		startup_credits_test_fail(op, 0);
		assert(r == (op == 0 ? SC_MARKER_ERR_OPEN : op == 1 ? SC_MARKER_ERR_WRITE
														 : SC_MARKER_ERR_RENAME));
		assert(!startup_credits_marker_seen(path));
		FILE* f = fopen(tmp, "rb");
		assert(!f);
	}
	assert(startup_credits_marker_write(path) == SC_MARKER_OK);
	assert(startup_credits_marker_seen(path));
	assert(startup_credits_marker_write(path) == SC_MARKER_OK); /* idempotent */
	assert(startup_credits_marker_seen(path));
	/* an existing VALID marker survives every injected failure */
	for(int op = 0; op < 3; op++) {
		startup_credits_test_fail(op, 1);
		(void)startup_credits_marker_write(path);
		startup_credits_test_fail(op, 0);
		assert(startup_credits_marker_seen(path));
	}
	/* exact bytes: an embedded NUL or a trailing byte is corrupt */
	static const char nul[] = "startup-credits-v1\n\0garbage";
	write_file(path, nul, sizeof(nul) - 1);
	assert(!startup_credits_marker_seen(path));
	write_file(path, "startup-credits-v1\nX", 20);
	assert(!startup_credits_marker_seen(path));
	write_file(path, "startup-credits-v1", 18);
	assert(!startup_credits_marker_seen(path));
	/* a corrupt marker is replaced by a valid one */
	assert(startup_credits_marker_write(path) == SC_MARKER_OK);
	assert(startup_credits_marker_seen(path));
	char ro[320];
	snprintf(ro, sizeof(ro), "%s/nodir/x/config.json", root);
	assert(startup_credits_marker_path(ro, path, sizeof(path)));
	assert(startup_credits_marker_write(path) == SC_MARKER_ERR_OPEN);
	puts("PASS marker: beside the config, missing/corrupt = first run, atomic write, "
		 "open/write/rename/unwritable failures reported with no temp left, a valid marker "
		 "never lost, exact bytes (embedded NUL / trailing byte = corrupt)");
}

int main(void) {
	char root[128];
	test_fixture_dir("test_startup_credits", root, sizeof(root));
	assert(MKDIR(root) == 0);
	test_shipped();
	test_first_boot();
	test_first_boot_stall();
	test_animation();
	test_b2_table();
	test_later_boot();
	test_blocked_frames();
	test_exit_revoked();
	test_stall_real_time();
	test_error();
	test_parse();
	test_load_failures(root);
	test_marker(root);
	startup_credits_data_free(&g_data);
	test_fixture_dir_cleanup(root);
	puts("test_startup_credits: all PASS");
	return 0;
}
