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

#ifndef STARTUP_CREDITS_H
#define STARTUP_CREDITS_H

/*
	BOOT-CREDITS-1028: the boot credit cards (BagHeads with its idle loop,
	then "Built on" CavEX / fCavEX), shown over startup loading.

	Self-contained and pack-independent: two raw RGBA8 textures in
	assets/gui/startup-credits (the animation frame sheet and a private copy
	of the bundled 8x8 font) plus credits.txt (layout, timing, animation,
	card text). build_atlas.py makes them offline. No PNG decode, no pack
	font and no gfx dependency here: the caller uploads the texture pixels
	and draws the tinted quads this module hands it.

	Rules (maintainer 2026-10-28, design v3):
	  - Each card: fade_in, dwell (fully readable), fade_out. FIRST boot
	    (no valid marker): every card plays in full; the last card holds
	    until the game is ready, then fades out to the title. A is ignored.
	  - LATER boots: no forced dwell. As soon as the game is ready the
	    fade-out starts; a FRESH A press after readiness cuts it. A pressed
	    before readiness is never buffered; the A that cut (or any A held at
	    the end) never reaches the title until released.
	  - Footer: empty, unless the game is still not ready `stall` ms past the
	    nominal sequence ("Still loading <phase>...") or the caller reports
	    an error (its text). Drawn with the private font, never faded.
	  - Marker startup-credits-v1.seen beside the user config, written
	    atomically by the caller only after a first boot played every dwell
	    AND the title was admitted (startup_credits_marker_due).
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STARTUP_CREDITS_MARKER "startup-credits-v1.seen"
/* Both textures together. */
#define STARTUP_CREDITS_TEXTURE_BUDGET (1u << 20)

#define SC_MAX_TEXTURES 2
#define SC_MAX_SPRITES 16
#define SC_MAX_CARDS 4
#define SC_MAX_ITEMS 12
#define SC_MAX_STEPS 24 /* animation steps (a step reuses any sprite) */
#define SC_TEXT_MAX 48
#define SC_TEX_FONT 1 /* the font is always texture 1 */
#define SC_MAX_FRAME_STEP_MS 100

struct sc_sprite {
	uint8_t tex;
	uint16_t x, y, w, h;
};

enum sc_item_kind { SC_ITEM_ANIM, SC_ITEM_TEXT };

struct sc_item {
	uint8_t kind;
	int16_t x, y;           /* 640x480 GUI space, top-left */
	/* anim: a looping table of steps, each (sprite, ms); sprites may repeat.
	 * loop_ms 0 = one static step. */
	uint8_t nsteps;
	uint8_t step_sprite[SC_MAX_STEPS];
	uint16_t step_ms[SC_MAX_STEPS];
	uint32_t loop_ms;
	uint8_t size;           /* text: pixel size (16 = 2x the 8 px font) */
	uint8_t r, g, b;
	char text[SC_TEXT_MAX];
};

struct sc_card {
	uint8_t nitems;
	struct sc_item items[SC_MAX_ITEMS];
};

struct sc_texture {
	char name[16], file[64];
	uint16_t w, h;
	uint8_t* rgba; /* heap; NULL once taken */
};

struct startup_credits_data {
	uint8_t ntextures, nsprites, ncards;
	struct sc_texture tex[SC_MAX_TEXTURES];
	struct sc_sprite sprites[SC_MAX_SPRITES];
	char sprite_names[SC_MAX_SPRITES][16];
	struct sc_card cards[SC_MAX_CARDS];
	uint32_t fade_in_ms, dwell_ms, fade_out_ms, stall_ms;
	int16_t footer_y;
	uint8_t footer_size, footer_r, footer_g, footer_b;
	uint8_t glyph_w[256]; /* from the font's alpha, after load */
	bool glyphs_ready;
};

enum startup_credits_load_result {
	SC_LOAD_OK = 0,
	SC_LOAD_ERR_MANIFEST,  /* missing/unreadable credits.txt */
	SC_LOAD_ERR_PARSE,     /* malformed or out-of-range manifest */
	SC_LOAD_ERR_TEXTURE,   /* missing/short/wrong-format texture */
	SC_LOAD_ERR_ALLOC,
};

/* Parse a manifest held in memory (textures not read). */
int startup_credits_parse(struct startup_credits_data* d, const char* text);
/* Load <dir>/credits.txt and its textures. On failure nothing stays
 * allocated. */
int startup_credits_load(struct startup_credits_data* d, const char* dir);
/* Hand texture k's pixels to the uploader (which then owns them). */
uint8_t* startup_credits_take_texture(struct startup_credits_data* d, int k);
void startup_credits_data_free(struct startup_credits_data* d);
/* Sum of every card's fade_in + dwell + fade_out (6800 ms shipped). */
uint32_t startup_credits_nominal_ms(const struct startup_credits_data* d);
/* The step of an anim item shown `since_ms` after its card first appeared
 * (the table loops every loop_ms). */
int startup_credits_anim_step(const struct sc_item* it, uint32_t since_ms);
/* Width in GUI pixels of `s` at `size` (Beta metrics), as drawn. */
int startup_credits_text_width(const struct startup_credits_data* d,
	const char* s, int size);

enum startup_credits_phase {
	SC_SHOWING,
	SC_EXITING, /* fade-out to the title */
	SC_DONE,
};

enum startup_credits_footer {
	SC_FOOTER_NONE,
	SC_FOOTER_STALL,
	SC_FOOTER_ERROR,
};

struct startup_credits {
	const struct startup_credits_data* d;
	bool first_run;
	uint32_t start_ms, now_ms;
	/* PRESENTED time: advances by at most SC_MAX_FRAME_STEP_MS per update,
	 * so a frame the main thread could not present (the blocking sound
	 * scan) never consumes a card's readable dwell. All timing uses it. */
	uint32_t clock_ms;
	enum startup_credits_phase phase;
	uint32_t exit_start_ms;
	uint8_t exit_card, exit_alpha;
	bool ready_seen;      /* ready at an earlier update (A-cut eligibility) */
	bool full_play;       /* the first-run dwells all completed */
	bool block_title_a;   /* swallow A at the title until released */
	bool error;
	char error_text[96];
	char phase_word[32];  /* the real loading phase, for the stall footer */
	bool marker_written;
};

void startup_credits_begin(struct startup_credits* s,
	const struct startup_credits_data* d, bool first_run, uint32_t now_ms);
/* Once per presented frame. `ready`: everything the title needs is done.
 * `a_edge`: A went down this frame; `a_held`: A is down now. */
void startup_credits_update(struct startup_credits* s, uint32_t now_ms,
	bool ready, bool a_edge, bool a_held);
/* The current loading phase word ("textures", "sounds", "the menu
 * background"...), named in the stall footer only. */
void startup_credits_set_phase(struct startup_credits* s, const char* word);
/* An error holds the cards (no exit) with the text in the footer; NULL
 * clears it. Retry/Exit handling belongs to the caller. */
void startup_credits_set_error(struct startup_credits* s, const char* text);
bool startup_credits_done(const struct startup_credits* s);
/* True while the title must ignore A: the press that cut the fade (or an A
 * held when the cards ended) until it is released. Clears on release. */
bool startup_credits_title_input_blocked(struct startup_credits* s,
	bool a_held);
enum startup_credits_footer startup_credits_footer_state(
	const struct startup_credits* s);
/* The footer line to show now ("" when empty). */
void startup_credits_footer_text(const struct startup_credits* s, char* out,
	size_t size);

/* What to draw this frame: the card and its content alpha 0..255. Returns
 * 0 (nothing) or 1. */
struct sc_visible { uint8_t card, alpha; };
int startup_credits_visible(const struct startup_credits* s,
	struct sc_visible out[1]);
/* Every quad of this frame, in order: texture, source rect (texels),
 * destination rect (GUI pixels), tint and alpha (texture colour x tint;
 * the font texture is white so the tint is the glyph colour). The black
 * background is the caller's. */
typedef void (*startup_credits_quad_fn)(void* user, int tex, int sx, int sy,
	int sw, int sh, int dx, int dy, int dw, int dh, uint8_t r, uint8_t g,
	uint8_t b, uint8_t a);
void startup_credits_draw(const struct startup_credits* s,
	startup_credits_quad_fn quad, void* user);

/* Marker: due once a first boot played every dwell and the cards are done;
 * the caller writes it after admitting the title, then notes it. */
bool startup_credits_marker_due(const struct startup_credits* s);
void startup_credits_marker_noted(struct startup_credits* s);
/* <dir of config_path>/startup-credits-v1.seen. False if it does not fit. */
bool startup_credits_marker_path(const char* config_path, char* out,
	size_t size);
/* Exact-content check; missing/corrupt = not seen (first-run behaviour). */
bool startup_credits_marker_seen(const char* path);
enum startup_credits_marker_result {
	SC_MARKER_OK = 0,
	SC_MARKER_ERR_OPEN,
	SC_MARKER_ERR_WRITE,  /* short write / flush / close */
	SC_MARKER_ERR_RENAME,
};
/* Atomic: <path>.tmp written + flushed + closed, then renamed over. */
int startup_credits_marker_write(const char* path);

/* Runtime-armed test seam: the n-th marker file step of the kind fails
 * (0 open, 1 write, 2 rename); n 0 = off. Never armed by the engine. */
void startup_credits_test_fail(int op, unsigned nth);

#endif
