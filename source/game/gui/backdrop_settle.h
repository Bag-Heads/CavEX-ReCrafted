/*
	BOOT-CREDITS-1028 T5: the backdrop bake's settle-light policy, shared by
	the real host (main.c bake_host_settle) and its identity test
	(tests/test_backdrop_settle_equiv.c), so the test runs the shipped code.

	legacy (pre-T5): light runs only while a cross-column repair is pending,
	within half the step budget; meshing starts on the first step.
	fast (default): light runs while ANY client light work is queued, within
	the budget less a visibility reserve; no mesh is dispatched until light is
	idle, so each section is meshed once from final light. The completion
	test (light idle, every drawn chunk current) is the same in both, so the
	captured frame is the same.
*/

#ifndef BACKDROP_SETTLE_H
#define BACKDROP_SETTLE_H

#include <stdbool.h>
#include <stdint.h>

#include "lighting.h"
#include "platform/time.h"
#include "stack.h"
#include "world.h"

/* Time kept for visibility + mesh dispatch when light may use the budget. */
#define BACKDROP_SETTLE_VIS_RESERVE_US 3000u

/* Any client light work still queued (the light half of completion). */
static inline bool backdrop_settle_light_busy(struct world* w) {
	return world_light_repair_pending(w) || w->light_spill_active
		|| !stack_empty(&w->lighting_updates)
		|| !lighting_flood_queue_empty(&w->lighting_flood_queue);
}

/* One step's light slice; `start` is the step's start. Returns the number
 * of world_update_lighting calls (at least one). */
static inline unsigned backdrop_settle_light(struct world* w, ptime_t start,
	uint32_t budget_us, bool legacy) {
	uint32_t light_budget
		= legacy || budget_us <= 2 * BACKDROP_SETTLE_VIS_RESERVE_US
		? budget_us / 2 : budget_us - BACKDROP_SETTLE_VIS_RESERVE_US;
	unsigned calls = 0;
	do {
		world_update_lighting(w);
		calls++;
	} while((legacy ? world_light_repair_pending(w) != 0
					: backdrop_settle_light_busy(w))
			&& time_diff_us(start, time_get()) < light_budget);
	return calls;
}

/* May this step dispatch meshes? */
static inline bool backdrop_settle_may_mesh(struct world* w, bool legacy) {
	return legacy || !backdrop_settle_light_busy(w);
}

#endif
