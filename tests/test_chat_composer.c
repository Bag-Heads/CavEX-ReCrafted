/* Chat composer model + history rules (docs/mp_chat_auth_research_2026-09-11.md).
 *
 * Focused test: compiles source/game/gui/chat_composer_model.c directly and
 * exercises the pure model the screen and the HUD are built on:
 *   - chat_composer_submit: Beta Enter path (String.trim, non-empty only,
 *     the composer cap) [740c583 GuiChat.keyTyped]
 *   - chat_open_gate: the D-pad Up-alone opener gate (decision item 3)
 *   - chat_wrap: byte-exact GuiIngame.addChatMessage split with a fixed-width
 *     fake font, including the colour-code and single-wide-char edges
 *   - the game_state.h ring: Beta's 50-line cap, oldest-first eviction, and
 *     the wrapped push (pieces land oldest-first, ages reset per piece)
 * Asserts are forced on (CMake -UNDEBUG for every test_ target). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/game/gui/chat_composer_model.h"

/* The ring lives header-only in game_state.h behind the whole engine's
 * include web; this focused test re-declares the exact same shape through
 * the shared header so the two cannot drift. */
#include "../source/game/chat_ring.h"

static void test_submit(void) {
	char out[CHAT_COMPOSER_CAP + 1];
	assert(chat_composer_submit("hello", out, sizeof out) == 5);
	assert(strcmp(out, "hello") == 0);
	/* String.trim: both ends, anything <= ' ' */
	assert(chat_composer_submit("   /register hunter2  ", out, sizeof out) == 17);
	assert(strcmp(out, "/register hunter2") == 0);
	assert(chat_composer_submit("\t \n", out, sizeof out) == 0 && out[0] == 0);
	assert(chat_composer_submit("", out, sizeof out) == 0);
	assert(chat_composer_submit(NULL, out, sizeof out) == 0 && out[0] == 0);
	/* Interior whitespace is kept verbatim. */
	assert(chat_composer_submit("a  b", out, sizeof out) == 4);
	assert(strcmp(out, "a  b") == 0);
	/* The composer cap: exactly 100 survives, a 101st byte cannot exist in
	 * the draft (the OSK refuses it), but the model still clamps. */
	char big[140];
	memset(big, 'x', sizeof big);
	big[sizeof big - 1] = 0;
	assert(chat_composer_submit(big, out, sizeof out) == CHAT_COMPOSER_CAP);
	assert(strlen(out) == CHAT_COMPOSER_CAP);
	/* A small out buffer is honoured, never overrun. */
	char tiny[4];
	assert(chat_composer_submit("abcdef", tiny, sizeof tiny) == 3);
	assert(strcmp(tiny, "abc") == 0);
	printf("PASS submit\n");
}

static void test_gate(void) {
	/* Up alone opens; a rock (Up with Left or Right) is refused; a Left or
	 * Right release never opens anything because there is no Up edge. */
	assert(chat_open_gate(true, false, false));
	assert(!chat_open_gate(true, true, false));
	assert(!chat_open_gate(true, false, true));
	assert(!chat_open_gate(true, true, true));
	assert(!chat_open_gate(false, false, false));
	assert(!chat_open_gate(false, true, false));
	printf("PASS gate\n");
}

/* Fixed 6 px per visible char, \247 + next char zero-width (the shape of
 * gutil_font_width / FontRenderer.getStringWidth). */
static int fake_width(const char* s) {
	int w = 0;
	int skip = 0;
	while(*s) {
		if(*s == '\247')
			skip = 2;
		if(skip > 0)
			skip--;
		else
			w += 6;
		s++;
	}
	return w;
}

static void test_wrap(void) {
	char out[8][CHAT_LINE_BYTES];
	/* Fits: one piece, verbatim. 320 / 6 = 53.3 -> 53 chars fit. */
	char s53[54];
	memset(s53, 'a', 53);
	s53[53] = 0;
	assert(chat_wrap(s53, CHAT_WRAP_WIDTH, fake_width, out, 8) == 1);
	assert(strcmp(out[0], s53) == 0);
	/* 54 chars: head = 53, tail = 1 (Beta: i grows while s[0..i+1] fits). */
	char s54[55];
	memset(s54, 'b', 54);
	s54[54] = 0;
	assert(chat_wrap(s54, CHAT_WRAP_WIDTH, fake_width, out, 8) == 2);
	assert(strlen(out[0]) == 53 && strlen(out[1]) == 1 && out[1][0] == 'b');
	/* 119 (the wire cap): 53 + 53 + 13. */
	char s119[120];
	for(int i = 0; i < 119; i++)
		s119[i] = (char)('A' + i % 26);
	s119[119] = 0;
	assert(chat_wrap(s119, CHAT_WRAP_WIDTH, fake_width, out, 8) == 3);
	assert(strlen(out[0]) == 53 && strlen(out[1]) == 53 && strlen(out[2]) == 13);
	assert(memcmp(out[0], s119, 53) == 0 && memcmp(out[1], s119 + 53, 53) == 0
		   && strcmp(out[2], s119 + 106) == 0);
	/* Colour codes are zero-width, so 2 codes let 57 visible chars ride in
	 * one piece; and the continuation piece gets NO colour code copied
	 * (Beta's split is a plain substring). */
	char coloured[80];
	snprintf(coloured, sizeof coloured, "\247e%s\247b%s", "0123456789012345678901234567890123456789", "01234567890123456");
	assert(fake_width(coloured) == 57 * 6);
	assert(chat_wrap(coloured, CHAT_WRAP_WIDTH, fake_width, out, 8) == 2);
	assert(strlen(out[0]) == 57); /* 53 visible + 4 colour-code bytes */
	assert(strcmp(out[1], "3456") == 0 && out[1][0] != '\247');
	/* Empty and single-overwide-char inputs. */
	assert(chat_wrap("", CHAT_WRAP_WIDTH, fake_width, out, 8) == 1 && out[0][0] == 0);
	/* A char wider than the limit: Beta emits it alone, then recurses on the
	 * remainder until it is empty -- and adds that empty remainder too (the
	 * `s = s.substring(i)` loop exits only once width(s) <= max). Ported
	 * quirk-for-quirk: "z", "z", "". */
	assert(chat_wrap("zz", 3, fake_width, out, 8) == 3 && strcmp(out[0], "z") == 0
		   && strcmp(out[1], "z") == 0 && out[2][0] == 0);
	/* max_pieces bounds the output: the remainder is kept whole (never
	 * dropped) in the last slot. */
	assert(chat_wrap(s119, CHAT_WRAP_WIDTH, fake_width, out, 2) == 2);
	assert(strlen(out[0]) == 53 && strlen(out[1]) == 66);
	printf("PASS wrap\n");
}

static void test_ring(void) {
	struct chat_ring_state ring = {0};
	assert(CHAT_RING_CAPACITY == CHAT_HISTORY_LINES);
	for(int i = 0; i < CHAT_RING_CAPACITY + 3; i++) {
		char line[16];
		snprintf(line, sizeof line, "line%d", i);
		chat_ring_push(&ring, line);
	}
	assert(ring.count == CHAT_RING_CAPACITY);
	/* Oldest live line is line3 (line0..2 evicted), newest is line52. */
	assert(strcmp(ring.lines[ring.head], "line3") == 0);
	assert(strcmp(ring.lines[(ring.head + ring.count - 1) % CHAT_RING_CAPACITY],
				  "line52") == 0);
	/* Wrapped push: pieces land oldest-first, each fresh (age 0). */
	struct chat_ring_state w = {0};
	chat_ring_push(&w, "old");
	chat_ring_tick(&w);
	char s119[120];
	memset(s119, 'q', 119);
	s119[119] = 0;
	chat_ring_push_wrapped(&w, s119, fake_width, CHAT_WRAP_WIDTH);
	assert(w.count == 4);
	assert(w.age[w.head] == 1);
	assert(strlen(w.lines[(w.head + 1) % CHAT_RING_CAPACITY]) == 53);
	assert(strlen(w.lines[(w.head + 2) % CHAT_RING_CAPACITY]) == 53);
	assert(strlen(w.lines[(w.head + 3) % CHAT_RING_CAPACITY]) == 13);
	assert(w.age[(w.head + 3) % CHAT_RING_CAPACITY] == 0);
	printf("PASS ring\n");
}

int main(void) {
	test_submit();
	test_gate();
	test_wrap();
	test_ring();
	printf("test_chat_composer: all PASS\n");
	return 0;
}
