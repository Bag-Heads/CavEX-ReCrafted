/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
/* LT-001: pure layout/timing of the three-block loading activity indicator
 * (maintainer pick 2026-10-10, option B). No graphics context. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "game/gui/loading_indicator.h"

static int failures;
#define CHECK(cond) do { if(!(cond)) { \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while(0)

static void check_layout(int width, int height, int size, int pitch, int top) {
	struct loading_indicator ind;
	loading_indicator_layout(width, height, 0.0F, &ind);
	for(int k = 0; k < LOADING_INDICATOR_SQUARES; k++) {
		const struct loading_indicator_square* s = &ind.squares[k];
		CHECK(s->size == size);
		CHECK(s->y == top);
		if(k > 0) CHECK(s->x - ind.squares[k - 1].x == pitch);
		/* Border is drawn outside the square; it must stay on screen too. */
		CHECK(s->x - LOADING_INDICATOR_BORDER >= 0);
		CHECK(s->y - LOADING_INDICATOR_BORDER >= 0);
		CHECK(s->x + s->size + LOADING_INDICATOR_BORDER <= width);
		CHECK(s->y + s->size + LOADING_INDICATOR_BORDER <= height);
	}
	int left = ind.squares[0].x;
	int right = ind.squares[LOADING_INDICATOR_SQUARES - 1].x + size;
	int centre2 = left + right; /* twice the group centre */
	CHECK(abs(centre2 - width) <= 2); /* centred on width/2 within 1 px */
}

static int lit_at(float elapsed) {
	struct loading_indicator ind;
	loading_indicator_layout(640, 480, elapsed, &ind);
	return ind.lit;
}

int main(void) {
	/* Full screen: 16 px squares, 28 px pitch, top y 208. */
	check_layout(640, 480, 16, 28, 208);
	check_layout(854, 480, 16, 28, 208);
	/* Split pane (height <= 240): 12 px squares, 20 px pitch, top y 56. */
	check_layout(320, 240, 12, 20, 56);
	check_layout(640, 240, 12, 20, 56);

	/* Colours are fixed palette values. */
	CHECK(LOADING_INDICATOR_BORDER == 2);
	CHECK(LOADING_INDICATOR_BORDER_RGB == 0x45413B);
	CHECK(LOADING_INDICATOR_LIT_RGB == 0x80C050);
	CHECK(LOADING_INDICATOR_UNLIT_RGB == 0x20241F);

	/* lit cycles 0,1,2,0 every 0.4 s. */
	CHECK(lit_at(0.0F) == 0);
	CHECK(lit_at(0.39F) == 0);
	CHECK(lit_at(0.41F) == 1);
	CHECK(lit_at(0.81F) == 2);
	CHECK(lit_at(1.21F) == 0);
	CHECK(lit_at(1.61F) == 1);
	CHECK(lit_at(120.01F) == 0); /* 300 steps */

	/* Frame-rate independent: same elapsed, any number of calls, same lit. */
	for(int n = 0; n < 100; n++) CHECK(lit_at(0.81F) == 2);
	CHECK(lit_at(0.41F) == 1);

	/* Negative / NaN / infinite elapsed clamp to 0. */
	CHECK(lit_at(-0.5F) == 0);
	CHECK(lit_at(-1000.0F) == 0);
	CHECK(lit_at(NAN) == 0);
	CHECK(lit_at(INFINITY) == 0);

	/* Large finite elapsed: FLT_MAX / 0.4 overflows to infinity, so it must
	 * clamp; 1e30 stays finite and must still land in [0,2]. */
	CHECK(lit_at(FLT_MAX) == 0);
	int big = lit_at(1e30F);
	CHECK(big >= 0 && big < LOADING_INDICATOR_SQUARES);
	big = lit_at(FLT_MAX * 0.5F);
	CHECK(big >= 0 && big < LOADING_INDICATOR_SQUARES);

	/* Layout independent of elapsed. */
	struct loading_indicator a, b;
	loading_indicator_layout(854, 480, 0.0F, &a);
	loading_indicator_layout(854, 480, 7.3F, &b);
	for(int k = 0; k < LOADING_INDICATOR_SQUARES; k++) {
		CHECK(a.squares[k].x == b.squares[k].x);
		CHECK(a.squares[k].y == b.squares[k].y);
	}

	if(failures) {
		fprintf(stderr, "test_loading_indicator: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_loading_indicator: PASS\n");
	return 0;
}
