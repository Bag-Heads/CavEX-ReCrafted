/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef ENTITY_MOB_WINDUP_CUE_H
#define ENTITY_MOB_WINDUP_CUE_H

#include <stdbool.h>
#include <stdint.h>
#include "monsters_object.h"

/* Local presentation extension, never a Beta multiplayer packet. Version 0
 * (absent/remote) is inert. No client timer: phase is authored on sim ticks. */
#define MOB_WINDUP_CUE_VERSION 1
struct mob_windup_cue {
	uint8_t version;
	uint8_t phase; /* 0 inactive, 1 first active tick (half), 2 full */
};

static inline float mob_windup_cue_degrees(struct mob_windup_cue cue,
										int monster_id, bool enabled) {
	if(!enabled || cue.version != MOB_WINDUP_CUE_VERSION
	   || (cue.phase != 1 && cue.phase != 2))
		return 0.0F;
	float degrees;
	switch(monster_id) {
	case MONSTER_ZOMBIE: case MONSTER_PIG_ZOMBIE: degrees = 35.0F; break;
	case MONSTER_SPIDER: degrees = 25.0F; break;
	case MONSTER_WOLF: degrees = 20.0F; break;
	default: return 0.0F;
	}
	return cue.phase == 1 ? degrees * 0.5F : degrees;
}

#endif
