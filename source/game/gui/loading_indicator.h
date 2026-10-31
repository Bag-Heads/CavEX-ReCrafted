/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
/* Three-block loading activity indicator (design-crit-1010 LT-001, maintainer
 * pick 2026-10-10 option B). Activity, not completion: one block is lit at a
 * time and the lit block steps every 0.4 s of the supplied episode clock, so
 * it is frame-rate independent. Pure layout; the caller draws with fep_fill. */
#ifndef LOADING_INDICATOR_H
#define LOADING_INDICATOR_H

#include <math.h>
#include <stdbool.h>

#define LOADING_INDICATOR_SQUARES 3
#define LOADING_INDICATOR_BORDER 2 /* px, drawn outside each square */
#define LOADING_INDICATOR_BORDER_RGB 0x45413B
#define LOADING_INDICATOR_LIT_RGB 0x80C050
#define LOADING_INDICATOR_UNLIT_RGB 0x20241F
#define LOADING_INDICATOR_STEP_S 0.4F

struct loading_indicator_square {
	int x, y, size;
};

struct loading_indicator {
	struct loading_indicator_square squares[LOADING_INDICATOR_SQUARES];
	int lit; /* 0..LOADING_INDICATOR_SQUARES-1 */
};

static inline void loading_indicator_layout(int width, int height,
	float elapsed_s, struct loading_indicator* out) {
	bool split = height <= 240;
	int size = split ? 12 : 16;
	int pitch = split ? 20 : 28;
	int top = split ? 56 : 208;
	int span = pitch * (LOADING_INDICATOR_SQUARES - 1) + size;
	int left = width / 2 - span / 2;
	for(int k = 0; k < LOADING_INDICATOR_SQUARES; k++) {
		out->squares[k].x = left + k * pitch;
		out->squares[k].y = top;
		out->squares[k].size = size;
	}
	/* Negative, NaN and infinite clocks clamp to the first block. */
	if(!(elapsed_s >= 0.0F) || isinf(elapsed_s)) {
		out->lit = 0;
		return;
	}
	/* Huge finite clocks (FLT_MAX) overflow the quotient to infinity, and
	 * fmodf(inf) is NaN; clamp those too so the int cast stays defined. */
	float quotient = elapsed_s / LOADING_INDICATOR_STEP_S;
	if(isinf(quotient)) {
		out->lit = 0;
		return;
	}
	float steps = floorf(quotient);
	out->lit = (int)fmodf(steps, (float)LOADING_INDICATOR_SQUARES);
}

#endif
