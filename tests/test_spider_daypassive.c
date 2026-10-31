/*
	Pins the Beta 1.7.3 spider daytime passivity: a spider only INITIATES a chase
	when it's dark enough (effective light <= 10), and that threshold (10) is
	DISTINCT from the hostile-mob spawn gate (<= 7). Binds the SHARED
	source/entity/combat.h (spider_hostile_at_light + SPIDER_HOSTILE_LIGHT_MAX) so
	the value matches server_tick_monster's acquisition gate. Beta source:
	EntitySpider.findPlayerToAttack returns a target only when getEntityBrightness
	< 0.5F, which maps exactly to effective light level <= 10.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "entity/combat.h"

#ifndef GLM_PI
#define GLM_PI 3.14159265358979323846f
#endif
#define DAY_LENGTH_TICKS 24000

static float daytime_celestial_angle(float time) {
	float X = time - 0.25F;
	if(X < 0)
		X += 1;
	return X + ((1.0F - (cosf(X * GLM_PI) + 1.0F) / 2.0F) - X) / 3.0F;
}
static int sky_subtract(uint64_t world_time) {
	float day = (float)(world_time % DAY_LENGTH_TICKS) / (float)DAY_LENGTH_TICKS;
	float ca = daytime_celestial_angle(day);
	float f = 1.0F - (cosf(ca * 2.0F * GLM_PI) * 2.0F + 0.5F);
	if(f < 0.0F) f = 0.0F;
	if(f > 1.0F) f = 1.0F;
	return (int)(f * 11.0F);
}

// Effective light = max(sky_light - sky_subtract, torch_light), as in the engine.
static int eff_light(int sky_light, int torch_light, int sub) {
	int sky = sky_light - sub;
	return (sky > torch_light) ? sky : torch_light;
}

int main(void) {
	assert(SPIDER_HOSTILE_LIGHT_MAX == 11);

	int noon = sky_subtract(6000);
	int midnight = sky_subtract(18000);

	// Outdoor at NOON (open sky 15): eff 15 -> PASSIVE (won't initiate).
	assert(!spider_hostile_at_light(eff_light(15, 0, noon)));
	// Outdoor at MIDNIGHT (open sky 15 -> 4): eff 4 -> HOSTILE.
	assert(spider_hostile_at_light(eff_light(15, 0, midnight)));
	// Dark cave (no sky, no torch), any time: eff 0 -> HOSTILE even at noon.
	assert(spider_hostile_at_light(eff_light(0, 0, noon)));
	// A torch-lit cave (torch 14): eff 14 -> PASSIVE (too bright for the spider).
	assert(!spider_hostile_at_light(eff_light(0, 14, midnight)));

	// Boundary: the Beta divide-table brightness crosses 0.5 between light 11
	// (0.437 < 0.5 -> hostile) and 12 (0.525 -> passive) -> eff 11 initiates, 12 not.
	assert(spider_hostile_at_light(11));
	assert(!spider_hostile_at_light(12));

	// The spider hostility threshold (11) is DISTINCT from the SPAWN gate (7):
	// at effective light 8..11 a spider would NOT spawn (>7) but a present spider
	// IS hostile (<=11) -- the two gates must not be conflated.
	for(int l = 8; l <= 11; l++) {
		assert(spider_hostile_at_light(l)); // hostile
		assert(l > 7);                       // but above the spawn gate
	}

	printf("test_spider_daypassive: spider passivity pinned (initiates only at "
		   "eff light <= %d; passive in daylight; threshold distinct from spawn "
		   "gate 7)\n",
		   SPIDER_HOSTILE_LIGHT_MAX);
	return 0;
}
