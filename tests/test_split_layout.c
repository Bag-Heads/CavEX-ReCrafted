#include <assert.h>
#include <stdio.h>
#include "game/split_layout.h"

static void finish(struct split_layout* s, bool admitted, bool a, bool b) {
	bool lost[2] = {a, b};
	split_layout_step(s, admitted, lost, 0);
	split_layout_step(s, admitted, lost, SPLIT_LAYOUT_TRANSITION_SECONDS);
}

int main(void) {
	struct split_layout s;
	split_layout_reset(&s);
	bool ok[2] = {false, false}, p2[2] = {false, true};
	assert(s.boundary == 1);
	split_layout_step(&s, true, ok, 0);
	split_layout_step(&s, true, ok, .175F);
	assert(fabsf(s.boundary - .75F) < .00001F);
	split_layout_step(&s, true, ok, .175F);
	assert(s.boundary == .5F);
	puts("PASS join: full screen -> three-quarter -> half in 350 ms");

	split_layout_step(&s, true, p2, 0);
	split_layout_step(&s, true, p2, 4.99F);
	assert(s.boundary == .5F && s.target == .5F);
	split_layout_step(&s, true, p2, .02F);
	assert(s.boundary == .5F && s.target == 1);
	split_layout_step(&s, true, p2, .175F);
	float before = s.boundary;
	split_layout_step(&s, true, ok, 0);
	assert(s.boundary == before); /* reconnect during collapse: no jump */
	split_layout_step(&s, true, ok, .35F);
	assert(s.boundary == .5F);
	puts("PASS loss: five-second grace, reversible collapse without a position jump");

	for(unsigned owner = 0; owner < 2; owner++) {
		bool lost[2] = {owner == 0, owner == 1};
		split_layout_step(&s, true, lost, 0);
		split_layout_step(&s, true, lost, 5);
		split_layout_step(&s, true, lost, .35F);
		assert(s.boundary == (owner ? 1 : 0));
		/* Merely reconnecting is still suspended until the recovery gate
		 * supplies false; repeated reports must not re-open the viewport. */
		split_layout_step(&s, true, lost, 20);
		assert(s.boundary == (owner ? 1 : 0));
		finish(&s, true, true, true);
		assert(s.boundary == .5F);
		finish(&s, true, false, false);
	}
	puts("PASS either owner survives full-screen; both lost retain both recovery views");

	/* A leave ACK, unlike controller loss, has no grace period. */
	finish(&s, false, false, false);
	assert(s.boundary == 1);
	for(int cycle = 0; cycle < 100; cycle++) {
		split_layout_step(&s, true, ok, 0);
		for(int frame = 0; frame < 30; frame++) {
			split_layout_step(&s, true, ok, 1.0F / 60);
			struct split_layout_rect a = split_layout_rect(&s, 0, 481);
			struct split_layout_rect b = split_layout_rect(&s, 1, 481);
			assert(a.y == 0 && a.height >= 0 && b.height >= 0);
			assert(a.height == b.y && a.height + b.height == 481);
		}
		finish(&s, false, false, false);
		assert(s.boundary == 1);
	}
	puts("PASS acknowledged leave and 100 rejoin cycles; exact odd-height coverage");
	return 0;
}
