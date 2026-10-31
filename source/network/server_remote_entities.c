/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "server_remote_internal.h"

#ifdef CAVEWII_MP_CLIENT


/* --- S3: bounded remote-entity table -------------------------------------
 * Pump-thread-exclusive (see the struct server_remote_impl field comment) --
 * no locking. */

void remote_entities_clear(struct server_remote_impl* remote) {
	memset(&remote->remote_entities, 0, sizeof(remote->remote_entities));
}

struct remote_entity* remote_entity_find(struct server_remote_impl* remote,
												 int32_t id) {
	struct remote_entity_table* t = &remote->remote_entities;
	for(size_t i = 0; i < REMOTE_ENTITY_CAPACITY; i++)
		if(t->entries[i].used && t->entries[i].id == id)
			return &t->entries[i];
	return NULL;
}

/* Finds-or-creates. A spawn packet for an id already tracked (a server
 * re-sending a spawn, or a stale id reused after a dropped DESTROY) replaces
 * the existing entry in place, mirroring Beta's own `addEntityToWorld` map
 * semantics (a duplicate key simply overwrites). Returns NULL only when the
 * table is genuinely full of OTHER live ids -- counted, never fatal. */
struct remote_entity* remote_entity_create(struct server_remote_impl* remote,
												   int32_t id,
												   enum remote_entity_kind kind) {
	struct remote_entity_table* t = &remote->remote_entities;
	struct remote_entity* existing = remote_entity_find(remote, id);
	struct remote_entity* slot = existing;
	if(!slot) {
		for(size_t i = 0; i < REMOTE_ENTITY_CAPACITY; i++)
			if(!t->entries[i].used) {
				slot = &t->entries[i];
				break;
			}
		if(!slot) {
			t->capacity_drops++;
			return NULL;
		}
		t->count++;
	}
	memset(slot, 0, sizeof(*slot));
	slot->used = true;
	slot->id = id;
	slot->kind = kind;
	return slot;
}

void remote_entity_destroy(struct server_remote_impl* remote, int32_t id) {
	struct remote_entity* entry = remote_entity_find(remote, id);
	if(!entry)
		return;
	entry->used = false;
	remote->remote_entities.count--;
}

/* Beta EntityList's registered numeric ids for Packet24MobSpawn.type
 * [740c583 EntityList.<clinit>] mapped to this engine's own monster_id space
 * (monsters_object.h -- a DIFFERENT, engine-local numbering that predates
 * this slice and is NOT the wire value). EntitySlime's id resolves to a
 * size-dependent monster_id and is handled by the caller (its size lives in
 * DataWatcher index 16, decoded after this table lookup runs); passing it
 * here returns the id for "some slime", to be refined by the caller.
 * Giant(53)/Mob(48)/Monster(49) have no engine equivalent yet -- returns
 * false (documented residual: giant-in-scope-low-pri ruling 2026-08-17;
 * "generic Mob/Monster" is never a real EntityList spawn output). */
bool beta_mob_type_to_monster_id(uint8_t beta_type, int* out_monster_id) {
	switch(beta_type) {
		case 50: *out_monster_id = MONSTER_CREEPER; return true;
		case 51: *out_monster_id = MONSTER_SKELETON; return true;
		case 52: *out_monster_id = MONSTER_SPIDER; return true;
		case 54: *out_monster_id = MONSTER_ZOMBIE; return true;
		case 55: *out_monster_id = MONSTER_SLIME_BIG; return true; /* refined below */
		case 56: *out_monster_id = MONSTER_GHAST; return true;
		case 57: *out_monster_id = MONSTER_PIG_ZOMBIE; return true;
		case 90: *out_monster_id = MONSTER_PIG; return true;
		case 91: *out_monster_id = MONSTER_SHEEP; return true;
		case 92: *out_monster_id = MONSTER_COW; return true;
		case 93: *out_monster_id = MONSTER_CHICKEN; return true;
		case 94: *out_monster_id = MONSTER_SQUID; return true;
		case 95: *out_monster_id = MONSTER_WOLF; return true;
		default: return false; /* 48/49/53 and anything unregistered */
	}
}

/* Finds a decoded DataWatcher entry by index/type, or NULL. Non-static: see
 * server_remote_internal.h's declaration comment (MP visual-parity pass,
 * server_remote_protocol.c's own MP_EVENT_ENTITY_METADATA case reuses this
 * for the remote-player sneak flag). */
const struct mp_watchable_entry* metadata_find(
	const struct mp_metadata* metadata, uint8_t index, uint8_t type) {
	for(uint8_t i = 0; i < metadata->count; i++)
		if(metadata->entries[i].index == index && metadata->entries[i].type == type)
			return &metadata->entries[i];
	return NULL;
}

/* Extracts the per-species DataWatcher fields this engine already models
 * directly into `entry`'s cache, refining EntitySlime's size-dependent
 * monster_id along the way. Every index/bit layout is transcribed from the
 * real Beta subclass, not from this engine's own (coincidentally identical
 * for wolf_flags) CRPC field layout. [740c583 EntitySheep.getFleeceColor/
 * getSheared] [740c583 EntityWolf.isSitting/isAngry/isTamed/getHealth]
 * [740c583 EntityPig.getSaddled] [740c583 EntityCreeper.getPowered]
 * [740c583 EntityGhast (dataWatcher 16 attacking bit)] [740c583
 * EntitySlime.getSlimeSize] */
void apply_mob_metadata(struct remote_entity* entry,
							   const struct mp_metadata* metadata) {
	const struct mp_watchable_entry* e16 = metadata_find(metadata, 16, 0);
	switch(entry->monster_id) {
		case MONSTER_SHEEP:
			if(e16) {
				entry->fleece_color = (uint8_t)(e16->value.byte_value & 0x0F);
				entry->sheared = (e16->value.byte_value & 0x10) != 0;
			}
			break;
		case MONSTER_PIG:
			if(e16)
				entry->saddled = (e16->value.byte_value & 0x01) != 0;
			break;
		case MONSTER_WOLF:
			if(e16)
				entry->wolf_flags = (uint8_t)e16->value.byte_value;
			{
				const struct mp_watchable_entry* e18
					= metadata_find(metadata, 18, 2);
				if(e18)
					entry->wolf_health = e18->value.int_value;
			}
			break;
		case MONSTER_GHAST:
			if(e16)
				entry->ghast_attacking = e16->value.byte_value != 0;
			break;
		case MONSTER_SLIME_SMALL:
		case MONSTER_SLIME_MEDIUM:
		case MONSTER_SLIME_BIG:
			if(e16) {
				int size = e16->value.byte_value;
				entry->monster_id = size <= 1 ? MONSTER_SLIME_SMALL
					: size == 2 ? MONSTER_SLIME_MEDIUM : MONSTER_SLIME_BIG;
			}
			break;
		default: break;
	}
	if(entry->monster_id == MONSTER_CREEPER) {
		const struct mp_watchable_entry* e17 = metadata_find(metadata, 17, 0);
		if(e17)
			entry->charged = e17->value.byte_value == 1;
	}
}

/* Find-or-create the mirror for one map id (pump-thread-exclusive).
 * Beta's analog is ItemMap.getMPMapData: fetch the cached MapData or
 * create a blank one [740c583 ItemMap.getMPMapData -> new MapData +
 * World.setItemData]. Restore previously received colors from the bounded
 * client cache (including disk spill) before applying a new strip. Otherwise
 * evict/revisit would erase all columns the server has NOT changed. Fresh
 * identities use the shared zero plane. The pump is on the main thread. */
struct remote_map_mirror*
remote_map_mirror_for(struct server_remote_impl* remote, int16_t map_id) {
	struct remote_map_mirror* lru = NULL;
	for(int i = 0; i < REMOTE_MAP_MIRRORS; i++) {
		struct remote_map_mirror* m = &remote->map_mirrors[i];
		if(m->used && m->map_id == map_id) {
			m->last_touch = ++remote->map_touch_counter;
			return m;
		}
		if(!m->dirty && (!lru || !m->used
		   || (lru->used && m->last_touch < lru->last_touch)))
			lru = m;
	}
	if(!lru) return NULL; // all dirty: leave the strip queued until next pump
	const uint8_t* colors = clin_map_recover(map_id, NULL);
	if(!colors) return NULL; // ABSENT/unknown, never seed from a failed reload
	if(lru->used)
		remote->map_mirror_evictions++;
	memset(lru, 0, sizeof(*lru));
	lru->used = true;
	lru->map_id = map_id;
	memcpy(lru->colors, colors, sizeof(lru->colors));
	lru->last_touch = ++remote->map_touch_counter;
	return lru;
}

/* Main-thread pump tail. Each dirty plane gets one delivery attempt per pump.
 * [740c583 NetClientHandler.handleMapData; ItemMap.onUpdate]: the client only
 * receives strips; it never requests them. Retry retains bytes, sends no request.
 * Saturating counters bound diagnostic state, never the lifetime of pending data. */
void flush_map_mirrors(struct server_remote_impl* remote) {
	for(int i = 0; i < REMOTE_MAP_MIRRORS; i++) {
		struct remote_map_mirror* m = &remote->map_mirrors[i];
		if(!m->used || !m->dirty) continue;
		if(!clin_map_deliver(m->map_id, m->colors)) {
			if(remote->map_push_deferred != UINT32_MAX) remote->map_push_deferred++;
			continue;
		}
		m->dirty = false;
		remote->map_pushes++;
	}
}

/* One cosmetic-only CRPC_ENTITY_MOVE for an entry: position republish (no
 * approach arming -- a status packet moves nothing, the metadata-carrier
 * precedent) plus every cached flag INCLUDING the Packet38 hurt/death
 * cosmetics. Used by the ENTITY_STATUS case (immediate flash/roll start)
 * and the pump-tail sweep (flash expiry, death-roll advance). */
void remote_emit_status_move(struct server_remote_impl* remote,
									const struct remote_entity* entry) {
	struct client_rpc call = {.type = CRPC_ENTITY_MOVE};
	call.payload.entity_move.entity_id = (uint32_t)entry->id;
	call.payload.entity_move.pos[0]
		= (float)mp_fixed32_to_double(entry->server_x);
	call.payload.entity_move.pos[1]
		= (float)mp_fixed32_to_double(entry->server_y);
	call.payload.entity_move.pos[2]
		= (float)mp_fixed32_to_double(entry->server_z);
	call.payload.entity_move.hurt = entry->hurt_active;
	// Animation-parity pass (2026-09-03) item 2: death_time is now shared by
	// mobs AND players (RenderLiving.rotateCorpse's deathTime flop applies to
	// every EntityLiving [740c583] -- see the MP_EVENT_ENTITY_STATUS
	// status==3 comment above this file's caller). Every OTHER field here
	// (fleece/sheared/saddled/wolf/charged/ghast) stays MOB-only: they are
	// monster-union state with no player equivalent.
	if(entry->kind == REMOTE_ENTITY_MOB || entry->kind == REMOTE_ENTITY_PLAYER)
		call.payload.entity_move.death_time = entry->death_time_sent;
	if(entry->kind == REMOTE_ENTITY_MOB) {
		call.payload.entity_move.fleece_color = entry->fleece_color;
		call.payload.entity_move.sheared = entry->sheared;
		call.payload.entity_move.saddled = entry->saddled;
		call.payload.entity_move.wolf_flags = entry->wolf_flags;
		call.payload.entity_move.wolf_health = entry->wolf_health;
		call.payload.entity_move.charged = entry->charged;
		call.payload.entity_move.ghast_attacking = entry->ghast_attacking;
	}
	clin_process_direct_remote(&call);
}

/* Pump-tail Packet38 cosmetic clock (the map-mirror flush precedent --
 * same thread as process_event). Wall-clock stands in for game ticks
 * because the remote layer has no 20 TPS pass: the hurt flash clears at
 * 500ms (Beta hurtTime=10 ticks) and the death roll advances at
 * elapsed/50ms+1 up to Beta's terminal 20 [740c583 EntityLiving.
 * handleHealthUpdate:850-861, onEntityUpdate deathTime>=20; combat.h
 * mob_death_finished pins the same constant]. The server's own
 * Packet29DestroyEntity removes the corpse; the cap here just stops the
 * republish stream if it never comes. */
void flush_entity_status(struct server_remote_impl* remote) {
	ptime_t now = time_get();
	for(size_t i = 0; i < REMOTE_ENTITY_CAPACITY; i++) {
		struct remote_entity* entry = &remote->remote_entities.entries[i];
		if(!entry->used)
			continue;
		bool emit = false;
		if(entry->hurt_active && time_reached(now, entry->hurt_until)) {
			entry->hurt_active = false;
			emit = true;
		}
		if(entry->death_active) {
			int32_t elapsed_ms = time_diff_ms(entry->death_started, now);
			int8_t target = (int8_t)((elapsed_ms / 50) + 1);
			if(target > 20)
				target = 20;
			if(target != entry->death_time_sent) {
				entry->death_time_sent = target;
				emit = true;
				if(target >= 20)
					entry->death_active = false; /* terminal pose holds */
			}
		}
		if(emit)
			remote_emit_status_move(remote, entry);
	}
}

/* Residual item 4 (2026-08-26): a dedicated, unsynced cosmetic RNG for the
 * hurt/death sound pitch wobble -- mirrors server_local.c's own
 * server_local_sound_rng/server_local_sound_pitch_wobble precedent (a private
 * static singleton, never the deterministic simulation RNG), reimplemented
 * here rather than pulling in server_local.h (a singleplayer-only header this
 * translation unit has no other reason to depend on). Beta's own source is
 * per-entity (Entity.rand, a fresh unseeded Random) and inherently unsynced across
 * clients -- a single process-wide stream is equally faithful for this
 * client's OWN presentation. [740c583 EntityLiving.handleHealthUpdate:856,859] */
static struct random_gen s_status_sound_rand;
static bool s_status_sound_rand_initialized = false;

static float remote_status_sound_pitch_wobble(void) {
	if(!s_status_sound_rand_initialized) {
		rand_gen_seed(&s_status_sound_rand);
		s_status_sound_rand_initialized = true;
	}
	/* (rand.nextFloat() - rand.nextFloat()) * 0.2F + 1.0F -- Beta's standard
	 * living-sound roll [740c583: EntityLiving.handleHealthUpdate:856,859],
	 * identical formula to server_local_sound_pitch_wobble. */
	return (rand_gen_flt(&s_status_sound_rand) - rand_gen_flt(&s_status_sound_rand))
		* 0.2F + 1.0F;
}

/* Residual item 4: the mob/player hurt+death SOUND that ENTITY_STATUS status
 * 2/3 arms alongside the existing visual flash/death-roll
 * (remote_emit_status_move). Called ONCE per status arrival from
 * process_event's MP_EVENT_ENTITY_STATUS case -- never from the pump-tail
 * sweep above, matching Beta's own placement of playSoundAtEntity inside
 * handleHealthUpdate itself (a one-shot on packet arrival, not a repeating
 * clock). [740c583 NetClientHandler.handleEntityStatus ->
 * EntityLiving.handleHealthUpdate:850-861] */
void remote_emit_status_sound(struct server_remote_impl* remote,
							   const struct remote_entity* entry,
							   int8_t status) {
	uint16_t sound_id = AUDIO_SOUND_NONE;
	float vol = 1.0F;
	if(entry->kind == REMOTE_ENTITY_MOB) {
		uint16_t hurt_sid, death_sid;
		float mob_vol;
		audio_mob_sounds(entry->monster_id, NULL, &hurt_sid, &death_sid,
						  &mob_vol);
		sound_id = status == 2 ? hurt_sid : death_sid;
		vol = mob_vol;
	} else if(entry->kind == REMOTE_ENTITY_PLAYER) {
		/* EntityPlayer overrides no sound getter -- hurt AND death are both
		 * plain "random.hurt" at volume 1.0F, exactly server_local.c's own
		 * hurt_player/damage_player_result precedent for the LOCAL player.
		 * [740c583: EntityLiving.attackEntityFrom:351-360; EntityPlayer] */
		sound_id = SOUND_RANDOM_HURT;
	} else {
		return; /* no other kind reaches ENTITY_STATUS's hurt/death branch. */
	}
	if(sound_id == AUDIO_SOUND_NONE)
		return; /* e.g. an unmapped/silent monster_id -- fails visibly via the
				 * caller's own counters, never a crash. */
	struct client_rpc call = {
		.type = CRPC_SOUND,
		.payload.sound = {
			.sound_id = sound_id,
			.pos = {(float)mp_fixed32_to_double(entry->server_x),
				(float)mp_fixed32_to_double(entry->server_y),
				(float)mp_fixed32_to_double(entry->server_z)},
			.vol = vol,
			.pitch = remote_status_sound_pitch_wobble(),
		},
	};
	clin_process_direct_remote(&call);
}

#endif /* CAVEWII_MP_CLIENT */
