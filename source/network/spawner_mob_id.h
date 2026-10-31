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

/*
	FH-17: Beta MobSpawner EntityId <-> engine-monster-id mapping, split out of
	spawner.h/spawner.c (the RNG-deterministic burst core) into its own
	dependency-light unit on purpose: spawner.c's burst core pulls in
	block_is_normal_cube (block/blocks.c) and rand_gen_flt/rand_gen_int
	(util.c) via spawner_spot_ok/spawner_burst, but region_archive.c's
	save-version-4 migration (and any other network-layer caller) needs only
	the pure string<->id table below, with none of that. Splitting keeps every
	caller's link graph exactly as narrow as what it actually calls -- no
	block registry or RNG machinery dragged into a plain lookup table. Zero
	dependencies beyond stdint.h; safe to include from any layer (worldgen
	deliberately does NOT -- see populate.c's own dungeon_pick_mob_spawner,
	which transcribes Beta's pickMobSpawner independently to stay decoupled
	from the network/server_local layer).
*/

#ifndef SPAWNER_MOB_ID_H
#define SPAWNER_MOB_ID_H

#include <stdint.h>

// FH-17: longest Beta EntityId string this port's spawner path recognises,
// plus a NUL. Mirrors tile_entities_nbt.h's TE_SPAWNER_MOB_ID_MAX -- the two
// are independently defined (dependency-light duplication, matching
// TE_SIGN_LINE_COLS/TE_SIGN_LINE_MAX's own precedent in tile_entities_nbt.h)
// rather than one #including the other.
#define SPAWNER_MOB_ID_MAX 24

// FH-17: the runtime (position-less) half of a MobSpawner tile entity --
// Beta TileEntityMobSpawner's persisted mobID/delay fields [decompile 740c583:
// readFromNBT/writeToNBT: EntityId string, Delay short]. Position is tracked
// separately (server_local's spawner_te_pos[], mirroring furnace_pos/
// dispenser_pos), matching furnace_te/dispenser_te's own position-less shape.
// mob_id == "" is the faithful "inert" state (see spawner_mob_from_id below).
struct spawner_te {
	char mob_id[SPAWNER_MOB_ID_MAX]; // Beta "EntityId" (NUL-terminated)
	int16_t delay;					  // Beta "Delay"
};

// FH-17 (save version 3->4 migration ONLY -- see region_archive.c's
// region_archive_migrate_mob_spawner_tes): legacy CaveWii saves smuggled
// Beta's pickMobSpawner index (0..3) into the spawner block's metadata
// nibble instead of a real MobSpawner tile entity. Beta pickMobSpawner index
// -> engine monster id: 0=Skeleton(2), 1=Zombie(0), 2=Zombie(0), 3=Spider(3).
// The LIVE runtime path (server_local_tick_spawners) no longer calls this --
// it resolves the mob type from the spawner's TE (spawner_mob_from_id)
// instead of the block's metadata.
int spawner_mob_from_meta(uint8_t metadata);

// FH-17 migration helper: the same 0..3 pickMobSpawner index -> the EXACT
// Beta EntityId string WorldGenDungeons.pickMobSpawner would have written
// [decompile 740c583], for synthesizing a MobSpawner TE from a legacy
// metadata nibble. NULL for an out-of-range index (defensive; every caller
// masks the nibble to &3 first).
const char* spawner_mob_to_id(uint8_t pick_index);

// FH-17 runtime path: Beta EntityId string (a MobSpawner TE's "EntityId", as
// written by WorldGenDungeons.pickMobSpawner or restored from a real Beta
// save) -> engine monster id. Beta's own createEntityInWorld(mobID) returns
// null (no spawn, no crash) for any mobID it has no EntityList mapping for;
// -1 here is that same "inert" outcome for a mobID this engine's spawner path
// does not implement. Only the three types Beta's OWN dungeon spawner ever
// writes are recognised (spawner_mob_to_id's range) -- real Beta 1.7.3 has no
// survival-reachable way to create any other MobSpawner EntityId, so a wider
// table would be untestable dead code, not missing faithfulness. Hardcoded
// engine ids (entity/monsters_object.h: ZOMBIE=0, SKELETON=2, SPIDER=3)
// rather than an #include, matching spawner_mob_from_meta's existing
// dependency-light style.
int spawner_mob_from_id(const char* mob_id);

#endif
