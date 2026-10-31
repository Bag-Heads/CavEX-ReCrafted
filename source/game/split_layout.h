/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#ifndef SPLIT_LAYOUT_H
#define SPLIT_LAYOUT_H

#include <stdbool.h>
#include <math.h>

/* Presentation only. A hidden owner still has a body, inventory and anchor.
 * Durations are controller UX choices, not simulation tick changes. */
#define SPLIT_LAYOUT_GRACE_SECONDS 5.0F
#define SPLIT_LAYOUT_TRANSITION_SECONDS 0.35F
struct split_layout {
	float boundary, start, target, elapsed;
	float lost_seconds[2];
	bool suspended[2];
};
struct split_layout_rect { int y, height; };

static inline void split_layout_reset(struct split_layout* layout) {
	*layout = (struct split_layout){.boundary = 1, .start = 1, .target = 1};
}

/* suspended includes the neutral + deliberate Resume gate, not just the
 * physical connection. Both lost preserves recovery space for both owners. */
static inline void split_layout_step(struct split_layout* layout, bool admitted,
	const bool suspended[2], float dt) {
	if(!isfinite(dt) || dt < 0) dt = 0;
	for(unsigned p = 0; p < 2; p++) {
		bool lost = admitted && suspended[p];
		if(!lost || !layout->suspended[p]) layout->lost_seconds[p] = 0;
		else layout->lost_seconds[p] = fminf(SPLIT_LAYOUT_GRACE_SECONDS,
			layout->lost_seconds[p] + dt);
		layout->suspended[p] = lost;
	}
	float target = admitted ? .5F : 1;
	if(admitted && suspended[0] != suspended[1]) {
		unsigned lost = suspended[0] ? 0 : 1;
		if(layout->lost_seconds[lost] >= SPLIT_LAYOUT_GRACE_SECONDS)
			target = lost ? 1 : 0;
	}
	if(target != layout->target) {
		layout->start = layout->boundary;
		layout->target = target;
		layout->elapsed = 0;
	} else layout->elapsed = fminf(SPLIT_LAYOUT_TRANSITION_SECONDS, layout->elapsed + dt);
	float t = fminf(1, layout->elapsed / SPLIT_LAYOUT_TRANSITION_SECONDS);
	float ease = t * t * (3 - 2 * t);
	layout->boundary = layout->start + (layout->target - layout->start) * ease;
}

/* One rounded boundary gives exact coverage even on odd-sized framebuffers.
 * Zero-height views are skipped by drawing, never handed to GX/projection. */
static inline struct split_layout_rect split_layout_rect(
	const struct split_layout* layout, unsigned owner, int height) {
	int boundary = (int)(layout->boundary * height + .5F);
	return owner ? (struct split_layout_rect){boundary, height - boundary}
		: (struct split_layout_rect){0, boundary};
}

#endif
