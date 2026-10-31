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

#ifndef CHAT_RING_H
#define CHAT_RING_H

#include <stddef.h>
#include <string.h>

#include "gui/chat_composer_model.h"

/* Received-chat history for CRPC_CHAT_LINE (Packet3Chat), Beta's
 * GuiIngame.chatMessageList: newest line at the logical front, the list
 * capped at 50 entries [740c583 GuiIngame.addChatMessage], every entry
 * carrying its own ChatLine.updateCounter [740c583 ChatLine]. Stored as a
 * standard ring buffer (head = OLDEST live index, count = live lines) over a
 * fixed array -- no per-line heap, the Wii rule. Header-only and free of
 * engine dependencies so the focused unit test compiles it as-is.
 * [740c583 Packet3Chat] [740c583 NetClientHandler.handleChat]
 *
 * History: the S4 lane shipped this as a 6-line "minimal HUD display" with no
 * composer; the 2026-09-11 chat composer lane raised it to Beta's 50 and
 * added the addChatMessage wrap (docs/mp_chat_auth_research_2026-09-11.md). */
#define CHAT_RING_CAPACITY CHAT_HISTORY_LINES
struct chat_ring_state {
	char lines[CHAT_RING_CAPACITY][CHAT_LINE_BYTES]; // Packet3's 119 + NUL
	// Per-line client-tick age, the Beta ChatLine.updateCounter: starts 0 on
	// push, +1 every 20Hz client tick [740c583 ChatLine;
	// GuiIngame.updateTick]. The HUD stops drawing a line at 200 with a
	// quadratic fade over the last 20 ticks [740c583
	// GuiIngame.renderGameOverlay chat block].
	int age[CHAT_RING_CAPACITY];
	size_t count; // live lines, 0..CHAT_RING_CAPACITY
	size_t head;  // index of the OLDEST live line
};

/* Push one already-wrapped line, evicting the oldest once the ring is full
 * (`chatMessageList.remove(size - 1)` past 50). */
static inline void chat_ring_push(struct chat_ring_state* ring,
								  const char* text) {
	size_t index = (ring->head + ring->count) % CHAT_RING_CAPACITY;
	/* Bounded copy: a line is at most Packet3's 119 units; anything longer
	 * is cut at the ring's width (never a heap string, never an overrun). */
	size_t n = 0;
	if(text)
		while(text[n] && n < CHAT_LINE_BYTES - 1)
			n++;
	if(n)
		memcpy(ring->lines[index], text, n);
	ring->lines[index][n] = 0;
	ring->age[index] = 0; // fresh line [740c583 ChatLine ctor]
	if(ring->count < CHAT_RING_CAPACITY)
		ring->count++;
	else
		ring->head = (ring->head + 1) % CHAT_RING_CAPACITY;
}

/* GuiIngame.addChatMessage: split at `max_width` logical px through
 * `width_fn`, then push the pieces oldest-first (Beta adds the head piece
 * before recursing on the tail). A 119-unit line is at most a few pieces;
 * the bound is generous and the tail is never dropped. */
static inline void chat_ring_push_wrapped(struct chat_ring_state* ring,
										  const char* text,
										  chat_width_fn width_fn,
										  int max_width) {
	char pieces[8][CHAT_LINE_BYTES];
	size_t n = chat_wrap(text, max_width, width_fn, pieces, 8);
	for(size_t i = 0; i < n; i++)
		chat_ring_push(ring, pieces[i]);
}

/* One 20Hz client tick of chat aging [740c583 GuiIngame.updateTick: ++every
 * ChatLine.updateCounter]. Beta's counter is an unbounded int; same here. */
static inline void chat_ring_tick(struct chat_ring_state* ring) {
	for(size_t i = 0; i < ring->count; i++)
		ring->age[(ring->head + i) % CHAT_RING_CAPACITY]++;
}

static inline void chat_ring_reset(struct chat_ring_state* ring) {
	*ring = (struct chat_ring_state) {0};
}

#endif
