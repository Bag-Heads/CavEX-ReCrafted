/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef GAME_GUI_LOADING_TIPS_H
#define GAME_GUI_LOADING_TIPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOADING_TIPS_COUNT 62
#define LOADING_TIPS_OWNERS 2
#define LOADING_TIPS_IDENTITIES 8
#define LOADING_TIPS_PROFILE_MAX 16
#define LOADING_TIPS_WORDS_MAX 22

enum loading_reason {
	NEW_WORLD, LOAD_SAVE, PORTAL_IN, PORTAL_OUT, RESPAWN, SPLIT_START,
	MP_TERRAIN, UNKNOWN
};
enum loading_tip_category {
	LOADING_TIP_SURVIVAL, LOADING_TIP_CRAFTING, LOADING_TIP_MINING,
	LOADING_TIP_MOBS, LOADING_TIP_FARMING, LOADING_TIP_REDSTONE,
	LOADING_TIP_NETHER, LOADING_TIP_CONTROLS, LOADING_TIP_SPLIT
};
enum loading_tip_action {
	LOADING_TIP_INVENTORY, LOADING_TIP_MINE, LOADING_TIP_USE,
	LOADING_TIP_PREVIOUS, LOADING_TIP_NEXT, LOADING_TIP_MENU,
	LOADING_TIP_ACTIONS
};
struct loading_tip {
	const char* id;
	const char* text;
	enum loading_tip_category category;
	uint16_t item, metadata; /* zero item means no icon slot */
};
extern const struct loading_tip loading_tips[LOADING_TIPS_COUNT];

/* The caller supplies a fresh owner/binding/viewport/pack snapshot. Font
 * entries are gutil_font_glyph_widths(): ink width in native 8-unit cells.
 * Glyphs use the existing input_glyph encoding; widths are from the existing
 * 24px prompt helper (24 for a cell, 60 for a chord). No artwork is changed.
 * visible=false pauses dwell and requires a fresh continuous gate on return.
 * blocked is sticky for this episode (errors/recovery/MP give-up notice). */
struct loading_tip_context {
	int width;
	bool split_pane, split_active, creative, peaceful, gamecube;
	bool enabled, visible, blocked;
	uint8_t font[256];
	int glyph[LOADING_TIP_ACTIONS], glyph_width[LOADING_TIP_ACTIONS];
};
struct loading_tip_token {
	char text[32]; /* text, or punctuation following a glyph */
	int glyph, width;
	uint8_t line;
};
struct loading_tip_layout {
	int x, y, width, height, body_x;
	bool icon;
	unsigned lines, count;
	int line_width[2];
	struct loading_tip_token tokens[LOADING_TIPS_WORDS_MAX];
};
struct loading_tip_history {
	uint64_t cycle_seen;
	uint8_t last_read[16], read_count, beginner_bits;
	uint8_t nether_bits; /* unread welcome priority survives the last-16 window */
};

/* Main/UI thread only. begin replaces an episode, end clears its reason and
 * drops the unfinished tip without recording it as read. First tick starts
 * the visible gate; call it on the first visible frame, including at time 0.
 * now_ms is a monotonic caller clock, never a simulation/frame-delta clock. */
void loading_reason_begin(unsigned owner, enum loading_reason reason);
void loading_reason_end(unsigned owner);
enum loading_reason loading_reason_get(unsigned owner);
const char* loading_reason_heading(enum loading_reason reason);
const char* loading_reason_stage(enum loading_reason reason);
bool loading_tips_set_identity(unsigned owner, const char* profile);
const struct loading_tip* loading_tips_tick(unsigned owner, uint64_t now_ms,
	const struct loading_tip_context* context);
const struct loading_tip_layout* loading_tips_current_layout(unsigned owner);
unsigned loading_tips_word_count(const char* text);
uint32_t loading_tips_read_ms(unsigned words);
uint32_t loading_tips_dwell_ms(unsigned words);
bool loading_tips_layout(const struct loading_tip* tip,
	const struct loading_tip_context* context, struct loading_tip_layout* out);
bool loading_tips_history(const char* profile, struct loading_tip_history* out);
void loading_tips_reset(void);

/* TODO TIPS-2: persistent loading_tips_v1.dat beside config, versioned,
 * checksummed, atomic <=4KiB, at normal config-save points only (v2 section 6).
 * This slice deliberately defines no serialization or on-disk save format. */
void loading_tips_save_history(void);

#endif
