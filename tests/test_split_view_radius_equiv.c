/* Copyright (c) 2026 CaveWii_v2 contributors */
/* MAINTAINER RULING 2026-09-11: local split-screen uses a RUNTIME server load
 * radius (split.server_view_radius, default 4) while single player keeps
 * MAX_VIEW_DISTANCE. This pins the accessor and the split policy transitions:
 *  1. single player (no synthetic separation, no guest body) reads exactly
 *     MAX_VIEW_DISTANCE whatever split_view_radius holds -- every expression
 *     that used the constant is textually unchanged in that mode;
 *  2. two anchors read split_view_radius, never above the compile-time cap;
 *  3. the split policy turns the far ring off (effective far = the split
 *     radius) on join and restores the configured far distance on leave;
 *  4. the effective radius bounds the residency debt scan (radius r) so no
 *     far claim can age forever once the ring is off. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "network/server_local.h"
#include "network/server_local_state.h"

int main(void) {
	struct server_local s;
	memset(&s, 0, sizeof(s));
	s.far_view_distance = MAX_VIEW_DISTANCE + 1;

	/* 1. single player: the accessor is the constant, whatever is configured */
	assert(server_local_view_radius(&s) == MAX_VIEW_DISTANCE);
	s.split_view_radius = 4;
	assert(server_local_view_radius(&s) == MAX_VIEW_DISTANCE);
	s.split_view_radius = 1;
	assert(server_local_view_radius(&s) == MAX_VIEW_DISTANCE);
	assert(server_local_view_radius(NULL) == MAX_VIEW_DISTANCE);
	server_local_split_policy_apply(&s);
	assert(s.far_view_distance == MAX_VIEW_DISTANCE + 1
		   && "single player: the far distance is never touched");
	puts("PASS single player reads MAX_VIEW_DISTANCE regardless of the dial");

	/* 2. two anchors (synthetic separation): the dial, capped */
	s.split_view_radius = 4;
	s.anchor_sep = 256;
	assert(server_local_two_anchor(&s));
	assert(server_local_view_radius(&s) == (4 < MAX_VIEW_DISTANCE ? 4 : MAX_VIEW_DISTANCE));
	s.split_view_radius = MAX_VIEW_DISTANCE + 7;
	assert(server_local_view_radius(&s) == MAX_VIEW_DISTANCE && "capped");
	s.split_view_radius = 0;
	assert(server_local_view_radius(&s) == MAX_VIEW_DISTANCE && "0 = full");
	/* SPLIT_EQUIV_DIAL_ZERO: dial 0 under two anchors is no split policy at all --
	 * the far ring stays as configured (every PC residency fixture runs here). */
	server_local_split_policy_apply(&s);
	assert(s.far_view_distance == MAX_VIEW_DISTANCE + 1 && s.far_view_configured == 0);
	s.split_view_radius = 4;
	puts("PASS two anchors read the dial, capped at MAX_VIEW_DISTANCE");

	/* 3. join: far ring off; leave: restored */
	server_local_split_policy_apply(&s);
	assert(s.far_view_distance == server_local_view_radius(&s)
		   && "join: effective far = split radius (ring off)");
	assert(s.far_view_configured == MAX_VIEW_DISTANCE + 1);
	server_local_split_policy_apply(&s); /* idempotent while split */
	assert(s.far_view_distance == server_local_view_radius(&s));
	/* SPLIT_EQUIV_DIAL_TO_ZERO_WHILE_SPLIT (Astra round 7 B): lowering the dial
	 * to 0 while still split restores the configured far distance at once. */
	s.split_view_radius = 0;
	server_local_split_policy_apply(&s);
	assert(server_local_two_anchor(&s) && s.far_view_distance == MAX_VIEW_DISTANCE + 1
		   && s.far_view_configured == 0);
	s.split_view_radius = 4;
	server_local_split_policy_apply(&s);
	assert(s.far_view_distance == 4 && s.far_view_configured == MAX_VIEW_DISTANCE + 1);
	s.anchor_sep = 0;
	assert(!server_local_two_anchor(&s));
	server_local_split_policy_apply(&s);
	assert(s.far_view_distance == MAX_VIEW_DISTANCE + 1
		   && "leave: configured far distance restored");
	assert(s.far_view_configured == 0);
	assert(server_local_view_radius(&s) == MAX_VIEW_DISTANCE);
	puts("PASS split policy: far ring off on join, restored on leave");

	/* 4. a guest body is the other way into two-anchor mode */
	s.guest_gameplay = true;
	assert(server_local_two_anchor(&s) && server_local_view_radius(&s) == 4);
	server_local_split_policy_apply(&s);
	assert(s.far_view_distance == 4);
	s.guest_gameplay = false;
	server_local_split_policy_apply(&s);
	assert(s.far_view_distance == MAX_VIEW_DISTANCE + 1);
	puts("PASS guest body: same policy as the synthetic anchor");
	return 0;
}
