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

#include "spawner_mob_id.h"

#include <string.h>

int spawner_mob_from_meta(uint8_t metadata) {
	// Beta WorldGenDungeons.pickMobSpawner nextInt(4) -> mob, mapped to the
	// engine monster ids (entity/monsters_object.h: ZOMBIE=0, SKELETON=2,
	// SPIDER=3). 0=Skeleton, 1=Zombie, 2=Zombie, 3=Spider.
	static const int map[4] = {2, 0, 0, 3};
	return map[metadata & 3];
}

const char* spawner_mob_to_id(uint8_t pick_index) {
	// [decompile 740c583: WorldGenDungeons.pickMobSpawner] var2==0->"Skeleton",
	// 1->"Zombie", 2->"Zombie", 3->"Spider" (var2 is always nextInt(4), so the
	// trailing "" default is unreachable from a valid 0..3 index).
	static const char* const map[4] = {"Skeleton", "Zombie", "Zombie",
										"Spider"};
	return pick_index < 4 ? map[pick_index] : NULL;
}

int spawner_mob_from_id(const char* mob_id) {
	if(!mob_id || !mob_id[0])
		return -1; // Beta's own "" default (never set) mobID: always inert
	if(strcmp(mob_id, "Skeleton") == 0)
		return 2;
	if(strcmp(mob_id, "Zombie") == 0)
		return 0;
	if(strcmp(mob_id, "Spider") == 0)
		return 3;
	// Beta EntityList addMapping(EntityGiantZombie.class, "Giant", 53): a
	// spawner TE naming "Giant" is live in Beta (only ever authored by
	// external tools/imports -- WorldGenDungeons never picks it, so
	// spawner_mob_from_meta/spawner_mob_to_id above stay 4-entry and the
	// worldgen stream is untouched). MONSTER_GIANT = 15.
	if(strcmp(mob_id, "Giant") == 0)
		return 15;
	return -1; // unrecognised EntityId: inert, matching Beta's null-return path
}
