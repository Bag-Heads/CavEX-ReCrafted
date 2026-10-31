/* Goldens: [740c583: EntityWolf.getLivingSound/setTailRotation]. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "entity/entity_wolf.h"
#include "entity/monsters_object.h"
#include "particle.h"

static int feq(float a, float b) { return fabsf(a - b) < 1.0e-6F; }

int main(void) {
	assert(feq(wolf_tail_angle(WOLF_FLAG_ANGRY, 8), 1.5393804F));
	assert(feq(wolf_tail_angle(0, 8), 0.62831855F));
	assert(feq(wolf_tail_angle(WOLF_FLAG_TAMED, 20), 0.55F * 3.1415927F));
	assert(feq(wolf_tail_angle(WOLF_FLAG_TAMED, 10), 0.35F * 3.1415927F));
	assert(wolf_living_sound_pick(WOLF_FLAG_ANGRY, 8, 2)
		== WOLF_LIVING_GROWL);
	assert(wolf_living_sound_pick(0, 8, 0) == WOLF_LIVING_PANTING);
	assert(wolf_living_sound_pick(0, 8, 1) == WOLF_LIVING_BARK);
	assert(wolf_living_sound_pick(WOLF_FLAG_TAMED, 9, 0)
		== WOLF_LIVING_WHINE);
	assert(wolf_living_sound_pick(WOLF_FLAG_TAMED, 10, 0)
		== WOLF_LIVING_PANTING);
	assert(feq(wolf_shading_while_shaking(0.5F, 0.7F, 0.5F), 0.825F));
	assert(feq(wolf_interested_angle(0.0F, 1.0F, 0.5F),
		0.075F * 3.1415927F));

	// [740c583: Entity.isWet] inWater || canBlockBeRainedOn(...) -- the shake
	// trigger is wet on EITHER term, not water contact alone.
	assert(!wolf_is_wet(false, false));
	assert(wolf_is_wet(true, false));  // in water, dry sky: still wet
	assert(wolf_is_wet(false, true));  // NOT in water, but rained on: wet too
	assert(wolf_is_wet(true, true));

	// [740c583: EntityWolf.showHeartsOrSmokeFX] geometry: 7 particles, pos
	// uniform over the wolf's own 0.8x0.8 box (+-0.8 xz, [+0.5,+1.3) y from
	// center), velocity a ZERO-MEAN gaussian*0.02 per axis -- not the old
	// +-0.4 xz spread / 0.08-0.12 upward-biased velocity.
	{
		particle_init();
		vec3 center = {100.0F, 64.0F, 100.0F};
		particle_set_camera(center);

		bool saw_negative_vel_y = false;
		bool saw_beyond_old_xz_bound = false; // beyond the pre-fix +-0.4 spread
		for(int call = 0; call < 40; call++) {
			particle_clear_all();
			particle_generate_wolf_tame_fx(center, true);
			assert(particle_test_count() == 7);
			for(size_t i = 0; i < 7; i++) {
				struct particle p;
				assert(particle_test_get(i, &p));
				assert(p.pos[0] >= center[0] - 0.8F
					&& p.pos[0] < center[0] + 0.8F);
				assert(p.pos[1] >= center[1] + 0.5F
					&& p.pos[1] < center[1] + 1.3F);
				assert(p.pos[2] >= center[2] - 0.8F
					&& p.pos[2] < center[2] + 0.8F);
				if(p.vel[1] < 0.0F)
					saw_negative_vel_y = true;
				if(fabsf(p.pos[0] - center[0]) > 0.42F)
					saw_beyond_old_xz_bound = true;
			}
		}
		assert(saw_negative_vel_y);     // zero-mean Gaussian, not upward-biased
		assert(saw_beyond_old_xz_bound); // true +-0.8 spread, not the old +-0.4

		particle_clear_all();
		particle_generate_wolf_tame_fx(center, false);
		assert(particle_test_count() == 7);
		for(size_t i = 0; i < 7; i++) {
			struct particle p;
			assert(particle_test_get(i, &p));
			assert(p.pos[0] >= center[0] - 0.8F
				&& p.pos[0] < center[0] + 0.8F);
			assert(p.pos[1] >= center[1] + 0.5F
				&& p.pos[1] < center[1] + 1.3F);
			assert(p.pos[2] >= center[2] - 0.8F
				&& p.pos[2] < center[2] + 0.8F);
		}
		particle_clear_all();
	}

	puts("test_wolf_tail_sound: PASS");
	return 0;
}
