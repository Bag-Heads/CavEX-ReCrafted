/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include <string.h>

#include "chat_composer_model.h"

size_t chat_composer_submit(const char* draft, char* out, size_t out_size) {
	if(!out || out_size == 0)
		return 0;
	out[0] = 0;
	if(!draft)
		return 0;
	/* Java String.trim(): drop leading and trailing chars <= ' ' (space and
	 * every control code). The composer only ever holds printable ASCII, so
	 * in practice this strips spaces -- but the rule is Beta's, not ours. */
	const char* start = draft;
	while(*start && (unsigned char)*start <= ' ')
		start++;
	const char* end = start + strlen(start);
	while(end > start && (unsigned char)end[-1] <= ' ')
		end--;
	size_t len = (size_t)(end - start);
	if(len == 0)
		return 0;
	if(len > CHAT_COMPOSER_CAP)
		len = CHAT_COMPOSER_CAP; /* unreachable through the composer's own cap */
	if(len >= out_size)
		len = out_size - 1;
	memcpy(out, start, len);
	out[len] = 0;
	return len;
}

bool chat_open_gate(bool up_edge, bool left_held, bool right_held) {
	return up_edge && !left_held && !right_held;
}

size_t chat_wrap(const char* text, int max_width, chat_width_fn width_fn,
				 char out[][CHAT_LINE_BYTES], size_t max_pieces) {
	if(!text || !width_fn || !out || max_pieces == 0)
		return 0;
	size_t pieces = 0;
	const char* s = text;
	char piece[CHAT_LINE_BYTES];
	while(width_fn(s) > max_width && pieces + 1 < max_pieces) {
		size_t len = strlen(s);
		/* Largest prefix that still fits: Beta grows i while s[0..i+1]
		 * fits, so the emitted head is s[0..i] -- at least ONE character
		 * even if that single character is already too wide. */
		size_t i = 1;
		for(; i < len; i++) {
			size_t probe = i + 1;
			if(probe >= sizeof(piece))
				break;
			memcpy(piece, s, probe);
			piece[probe] = 0;
			if(width_fn(piece) > max_width)
				break;
		}
		if(i >= sizeof(piece))
			i = sizeof(piece) - 1;
		memcpy(out[pieces], s, i);
		out[pieces][i] = 0;
		pieces++;
		s += i;
	}
	size_t rest = strlen(s);
	if(rest >= CHAT_LINE_BYTES)
		rest = CHAT_LINE_BYTES - 1;
	memcpy(out[pieces], s, rest);
	out[pieces][rest] = 0;
	return pieces + 1;
}

/* ---- geometry ------------------------------------------------------------
 * Every literal below is a cell of the SPEC rect table
 * (docs/ui_design/chat-keyboard/SPEC.md, "Coordinate system and safe area").
 * This is the ONE place the composer's panel-level numbers are written; the
 * renderer, the pointer targets and the tests all read them from here. */
void chat_geometry(struct chat_geometry* g, int ox) {
	if(!g)
		return;
	g->safe = (struct chat_rect) {ox + CHAT_SAFE_X, CHAT_SAFE_Y, CHAT_SAFE_W,
								  CHAT_SAFE_H};
	g->panel = (struct chat_rect) {ox + 44, 120, 552, 312};
	g->header = (struct chat_rect) {ox + 56, 132, 528, 24};
	g->input = (struct chat_rect) {ox + 56, 164, 528, 32};
	g->keyboard = (struct chat_rect) {ox + 56, 204, 528, 216};
	g->tabs = (struct chat_rect) {ox + 56, 204, 528, 32};
	g->grid = (struct chat_rect) {ox + 56, 244, 528, 128};
	g->utility = (struct chat_rect) {ox + 56, 380, 528, 40};
	g->notice = (struct chat_rect) {ox + 44, 24, 552, 28};
	g->history = (struct chat_rect) {ox + 44, 60, 552, 54};
	g->draft = (struct chat_rect) {ox + 64, 172, 416, CHAT_FONT_LABEL};
	g->counter = (struct chat_rect) {ox + 488, 172, CHAT_COUNTER_RESERVE,
									 CHAT_FONT_LABEL};
	g->title_x = ox + 56;
	g->title_y = 136;
	g->type_hint_x = ox + 152;
	g->type_hint_y = 132;
	g->hint_y = 132;
	g->hint_right = ox + 584;
	g->hint_gap = 16;
	g->history_newest_y = 98;
}

bool chat_rect_inside(const struct chat_rect* outer, const struct chat_rect* r) {
	if(!outer || !r)
		return false;
	return r->x >= outer->x && r->y >= outer->y
		&& r->x + r->w <= outer->x + outer->w
		&& r->y + r->h <= outer->y + outer->h;
}

size_t chat_clip_display(const char* text, char* out, size_t out_size,
						 int max_width, chat_width_fn width_fn) {
	if(!out || out_size == 0)
		return 0;
	out[0] = 0;
	if(!text || !width_fn)
		return 0;
	size_t n = strlen(text);
	if(n >= out_size)
		n = out_size - 1;
	memcpy(out, text, n);
	out[n] = 0;
	while(n > 0 && width_fn(out) > max_width)
		out[--n] = 0;
	/* Repair a colour pair the cut split. Walk from the START so an 0xA7 that
	 * is itself the SELECTOR of an earlier pair is never mistaken for an
	 * opener, then drop a trailing opener that lost its selector. */
	size_t scan = 0, last_open = (size_t)-1;
	while(scan < n) {
		if((unsigned char)out[scan] == 0xA7) {
			last_open = scan;
			scan += 2;
		} else {
			scan++;
		}
	}
	if(last_open != (size_t)-1 && last_open == n - 1)
		out[--n] = 0;
	return n;
}

/* Written only by the app-only agent bridge, read only by the composer when it
 * opens. Plain bool: the bridge writes it from its reader thread at most once
 * per command and the composer samples it on a frame boundary, so a torn read
 * is impossible and a stale value costs at most one composer open. */
static bool s_controller_mode_requested;

void chat_controller_mode_set(bool requested) {
	s_controller_mode_requested = requested;
}

bool chat_controller_mode_requested(void) {
	return s_controller_mode_requested;
}

bool chat_rect_overlap(const struct chat_rect* a, const struct chat_rect* b) {
	if(!a || !b || a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0)
		return false;
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h
		&& b->y < a->y + a->h;
}
