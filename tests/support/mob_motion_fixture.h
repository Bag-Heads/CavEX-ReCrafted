#ifndef TEST_SUPPORT_MOB_MOTION_FIXTURE_H
#define TEST_SUPPORT_MOB_MOTION_FIXTURE_H

/* Mob motion units S2 (docs/intent/mob-motion-units.md): a mob's position and
 * motion are the authoritative DOUBLES in e->data.monster.motion_d; e->pos /
 * e->vel are write-only mirrors, and the monster tick DISCARDS (and counts,
 * mob_motion_bypass_count) any direct float write. A fixture that hand-places
 * a mob by writing e->pos / e->vel declares those floats authoritative with
 * test_mob_sync() right after its writes -- an explicit fixture seam, never a
 * production path. */

#include "entity/entity.h"
#include "entity/mob_motion.h"

static inline void test_mob_sync(struct entity* e) {
	mob_motion_adopt_floats(e, &e->data.monster.motion_d);
}

#endif
