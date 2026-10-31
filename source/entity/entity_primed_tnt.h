/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef ENTITY_PRIMED_TNT_H
#define ENTITY_PRIMED_TNT_H

#include <stdbool.h>
#include <stdint.h>

#include "../cglm/cglm.h"
#include "../worldgen/beta_math.h"

struct entity;
struct server_local;

/* Beta 1.7.3 EntityTNTPrimed constants (decompile 740c583). */
#define TNT_FUSE_DEFAULT 80
#define PRIMED_TNT_FUSE_TICKS TNT_FUSE_DEFAULT
#define PRIMED_TNT_EXPLOSION_POWER 4.0f
#define PRIMED_TNT_SIZE 0.98f

/*
	Beta's constructor launch from one Math.random() sample in [0,1). The
	apparently redundant PI/180 conversion is present in the Beta bytecode and is
	intentional here; mh_sin/mh_cos reproduce MathHelper's lookup table.
*/
static inline void primed_tnt_launch_velocity(double random_sample, vec3 out) {
	/* Beta narrows the product, not Math.random() itself. The order is
	 * observable at MathHelper lookup boundaries. */
	float angle = (float)(random_sample * 3.1415927410125732 * 2.0);
	float lookup_angle = angle * 3.1415927f / 180.0f;
	out[0] = -mh_sin(lookup_angle) * 0.02f;
	out[1] = 0.20000000298023224f;
	out[2] = -mh_cos(lookup_angle) * 0.02f;
}

/* Exact `if (fuse-- <= 0)` operation. The fuse is decremented on every call. */
static inline bool primed_tnt_fuse_update_should_explode(int* fuse) {
	bool explode = *fuse <= 0;
	(*fuse)--;
	return explode;
}

/* RenderTNTPrimed final-ten-tick inflation and white-overlay decisions. */
static inline float primed_tnt_render_scale(int fuse, float tick_delta) {
	float remaining = (float)fuse - tick_delta + 1.0f;
	if(remaining >= 10.0f)
		return 1.0f;

	float progress = 1.0f - remaining / 10.0f;
	if(progress < 0.0f)
		progress = 0.0f;
	if(progress > 1.0f)
		progress = 1.0f;
	progress *= progress;
	progress *= progress;
	return 1.0f + progress * 0.3f;
}

static inline bool primed_tnt_flash_active(int fuse) {
	return fuse >= 0 && fuse / 5 % 2 == 0;
}

static inline float primed_tnt_flash_amount(int fuse, float tick_delta) {
	if(!primed_tnt_flash_active(fuse))
		return 0.0f;
	return (1.0f - ((float)fuse - tick_delta + 1.0f) / 100.0f) * 0.8f;
}

void entity_primed_tnt(uint32_t id, struct entity* e, bool server, void* world,
					   int fuse);
/* Shared Beta onUpdate physics/fuse step. The multiplayer/client caller runs
 * the same motion and smoke but never creates the authoritative explosion. */
bool primed_tnt_advance(struct entity* e);
bool entity_primed_tnt_has_pending_explosion(const struct entity* e);
// Runs the exact final physics/fuse step and retry admission. true means the
// blast committed and the caller must destroy the source entity.
bool entity_primed_tnt_retry_pending_explosion(
	struct entity* e, struct server_local* s);

#endif
