/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* In-game "Import Beta 1.7.3" (tmp/jobs/ingame-import-1030, coder C): the
 * progress / done / failed screen the Texture Packs screen opens. The worker
 * (source/platform/beta_import.h) does all SD work on its own thread; this
 * screen only polls it once per frame, so the main loop keeps presenting
 * frames and heartbeats for the whole import. */

#ifndef GAME_GUI_SCREEN_BETA_IMPORT_H
#define GAME_GUI_SCREEN_BETA_IMPORT_H

#include <stdbool.h>

#include "../../platform/beta_import.h"
#include "screen.h"

extern struct screen screen_beta_import;

/* What the screen is showing. RUNNING covers SCANNING..SWAPPING; CANCELLING is
 * RUNNING after the player asked to stop (the worker stops at its next file
 * boundary). The three results wait for the player's OK. */
enum screen_beta_import_view {
	SCREEN_BETA_IMPORT_RUNNING,
	SCREEN_BETA_IMPORT_CANCELLING,
	SCREEN_BETA_IMPORT_DONE,
	SCREEN_BETA_IMPORT_FAILED,
	SCREEN_BETA_IMPORT_CANCELLED,
};

/* Start the import and show this screen. Called by the Texture Packs screen's
 * Import button; returns to the Texture Packs screen when the player leaves. */
void screen_beta_import_open(void);

/* The Texture Packs screen's main-thread selector (beta_import_set_selector):
 * it QUEUES the texture apply of `pack_value` and the pack screen runs it,
 * behind its own loading strip, when the player returns to it. Defined in
 * screen_texturepacks.c. */
bool screen_texturepacks_import_select(const char* pack_value);

/* Pure geometry on the 640x480 canvas the pack screen uses (centred, scaled
 * by height/480), shared by render and pointer hit test. */
struct screen_beta_import_rect {
	int x, y, w, h;
};
struct screen_beta_import_layout {
	struct screen_beta_import_rect heading; /* text line box */
	struct screen_beta_import_rect bar;		/* the inner 0x808080 track */
	struct screen_beta_import_rect status;	/* first status line box */
	struct screen_beta_import_rect button;	/* Cancel / OK */
	struct screen_beta_import_rect footer;	/* glyph prompt row */
};
void screen_beta_import_layout(int width, int height,
							   struct screen_beta_import_layout* out);

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
enum screen_beta_import_view screen_beta_import_test_view(void);
/* The status lines exactly as render draws them (count, then each line). */
int screen_beta_import_test_lines(const char** lines, int cap);
const char* screen_beta_import_test_heading(void);
const char* screen_beta_import_test_button_label(void);
bool screen_beta_import_test_button_enabled(void);
/* Bar fill in permille, or -1 when no fraction is known (no fill drawn). */
int screen_beta_import_test_bar_permille(void);
/* PC captures only: replace beta_import_poll's answer (NULL restores). */
void screen_beta_import_test_override(const bi_progress* p);
/* PC captures: the Cancel action without an input edge. */
void screen_beta_import_test_request_cancel(void);
#endif

#endif
