/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#ifndef ENTITY_PICKUP_FLIGHT_H
#define ENTITY_PICKUP_FLIGHT_H

/*
	MP visual-parity pass (2026-08-31) item 4: PURE math for the cosmetic
	pickup flight, factored out of entity_item.c's entity_render (which is
	GX-coupled and cannot be linked in isolation) so it can be golden-tested
	directly -- the render_entity_anchor.h / test_head_anchor.c precedent.

	Transcribes [740c583 EntityPickupFX.renderParticle] exactly:

		progress = ((float)age + partial_tick) / (float)maxAge;
		progress is squared                          (ease-in quadratic)
		... x/y/z = start + (end - start) * progress ...

	where (in Beta) start = entityToPickUp.posX/Y/Z (frozen once the item was
	removeEntityFromWorld'd) and end = the collector's OWN interpolated
	render position (lastTickPos + (pos-lastTickPos)*partialTick) with yOffs
	(-0.5F) added to Y. age/maxAge are Beta's own EntityPickupFX fields
	(maxAge fixed at 3 ticks); `partial_tick` is the render-frame interpolation
	factor (this engine's tick_delta, Beta's partial tick).
*/

#include "../cglm/cglm.h"

static inline void entity_pickup_flight_lerp(const vec3 start,
											  const vec3 collector_pos,
											  int age, float partial_tick,
											  int max_age, vec3 out) {
	float t = ((float) age + partial_tick) / (float) max_age;
	if(t < 0.0f)
		t = 0.0f;
	if(t > 1.0f) // defensive clamp: Beta is structurally bounded (age never
		t = 1.0f; // reaches max_age before the FX self-destructs), this
				  // engine reaps the SAME tick age reaches max_age too.
	float t2 = t * t; // [740c583: `var8 *= var8;`]

	out[0] = start[0] + (collector_pos[0] - start[0]) * t2;
	out[1] = start[1] + (collector_pos[1] - start[1]) * t2;
	out[2] = start[2] + (collector_pos[2] - start[2]) * t2;
}

#endif
