/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#ifndef GAME_HUD_HURT_H
#define GAME_HUD_HURT_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846 // strict C99 (glibc/devkitPPC) hides it
#endif
#include <stdbool.h>
#include <stdint.h>

/* Beta 1.7.3 client HURT FEEDBACK, render/feel layer only (no simulation
 * state): the heart-row blink + low-health jitter of
 * [740c583 GuiIngame.renderGameOverlay:60-109] and the hurt/death camera roll
 * of [740c583 EntityRenderer.hurtCameraEffect:152-169].
 *
 * Our client is server-authoritative: the only thing it ever learns about
 * damage is a new health value (CRPC_PLAYER_SET_HEALTH, the Packet8 shape).
 * That is EXACTLY Beta's own multiplayer client, so the derivation below is a
 * faithful port of [740c583 EntityPlayerSP.setHealth:148-161], which arms all
 * of this state from the health DELTA alone:
 *   decrease -> prevHealth = old, heartsLife = 20, hurtTime = maxHurtTime = 10
 *   increase -> heartsLife = heartsHalvesLife/2 = 10 (the brief heal flash,
 *               same value the SP heal path uses [740c583 EntityLiving:303])
 * and never touches attackedAtYaw -- so the MP-client camera roll is always
 * the plain Z tilt (attackedAtYaw stays 0.0F; the SP-only directional term of
 * [740c583 EntityLiving.attackEntityFrom:333-349] cannot exist without a
 * damage-source position, which Packet8 does not carry). The roll helper
 * below still takes the yaw so the math is the complete hurtCameraEffect
 * port; the client passes 0.
 *
 * Deliberate non-port: EntityPlayerSP.setHealth:159 also calls
 * damageEntity(diff), which re-applies armor mitigation to an already
 * server-mitigated value and drifts the client's health ABOVE the server's
 * truth while wearing armor. Our health value stays the authoritative packet
 * value (client_interface.c applies it directly); only the FEEDBACK state is
 * derived here. Health-scale note: this module never interprets magnitudes,
 * only the delta sign, so it is agnostic to the engine's 160-point health
 * scale (combat.h) vs Beta's 20.
 *
 * Kept as a pure header (camera_bob.h precedent) so test_hud_hurt can pin the
 * constants without linking the client. State lives in struct camera next to
 * the bob state purely for plumbing (one existing 20 Hz call site feeds both;
 * Beta keeps it on the entity, but our client entity struct is the mob-shared
 * simulation surface and stays untouched). */

struct hud_hurt_state {
	int last_health; /* last authoritative health observed */
	int prev_health; /* Beta prevHealth [740c583 EntityLiving:27] */
	int hearts_life; /* Beta heartsLife [740c583 EntityLiving heartsHalvesLife=20] */
	int hurt_time;     /* Beta hurtTime [740c583 EntityLiving:29] */
	int max_hurt_time; /* Beta maxHurtTime [740c583 EntityLiving:30] */
	int death_time;    /* Beta deathTime [740c583 EntityLiving:32] */
	/* GuiIngame.updateCounter [740c583 GuiIngame:16,368]: 20 Hz tick counter
	 * seeding the low-health heart jitter and (via /3%2) nothing else -- the
	 * blink phase runs on heartsLife, not this. */
	uint32_t update_counter;
	bool primed; /* first observation latches silently (world join) */
};

static inline void hud_hurt_reset(struct hud_hurt_state* s) {
	s->last_health = 0;
	s->prev_health = 0;
	s->hearts_life = 0;
	s->hurt_time = 0;
	s->max_hurt_time = 0;
	s->death_time = 0;
	s->update_counter = 0;
	s->primed = false;
}

/* Once per 20 Hz client tick with the current authoritative player health;
 * has_player=false (menus, world teardown) resets to the fresh-entity state. */
static inline void hud_hurt_tick(struct hud_hurt_state* s, int health,
								 bool has_player) {
	if(!has_player) {
		hud_hurt_reset(s);
		return;
	}
	if(!s->primed) {
		/* world join: latch without a flash (a fresh Beta entity starts with
		 * all timers zero and health already set). */
		hud_hurt_reset(s);
		s->last_health = health;
		s->primed = true;
		return;
	}

	/* respawn = a brand-new client entity in Beta MP (every field zeroed
	 * before the fresh health arrives), observed here as dead -> alive. */
	if(s->last_health <= 0 && health > 0) {
		uint32_t counter = s->update_counter;
		hud_hurt_reset(s);
		s->last_health = health;
		s->primed = true;
		s->update_counter = counter + 1;
		return;
	}

	s->update_counter++; /* [740c583 GuiIngame:368] */

	/* per-tick decay [740c583 EntityLiving.onEntityUpdate:147-153] */
	if(s->hurt_time > 0)
		s->hurt_time--;
	if(s->hearts_life > 0)
		s->hearts_life--;
	/* death roll timer: ++ while dead, frozen once >20 (the entity is removed
	 * at deathTime>20 and stops ticking) [740c583 EntityLiving:155-160] */
	if(health <= 0 && s->death_time <= 20)
		s->death_time++;

	/* the Packet8 delta derivation [740c583 EntityPlayerSP.setHealth:148-161] */
	int diff = s->last_health - health;
	if(diff < 0) {
		s->hearts_life = 20 / 2; /* heal flash [:152-154] */
	} else if(diff > 0) {
		s->prev_health = s->last_health; /* [:157] */
		s->hearts_life = 20;             /* [:158] */
		s->hurt_time = s->max_hurt_time = 10; /* [:160] */
	}
	s->last_health = health;
}

/* Is the heart row on a white-flash frame? [740c583 GuiIngame:60-63]:
 * heartsLife/3 % 2 == 1, forced off below 10. */
static inline bool hud_hurt_blink(const struct hud_hurt_state* s) {
	return s->hearts_life >= 10 && (s->hearts_life / 3) % 2 == 1;
}

/* The per-frame seed of the low-health heart jitter rand
 * [740c583 GuiIngame:66]: setSeed((long)(updateCounter * 312871)). The
 * multiply is Java INT arithmetic -- it wraps at 32 bits BEFORE the widening
 * cast, so replicate the wrap then sign-extend. */
static inline int64_t hud_hurt_jitter_seed(const struct hud_hurt_state* s) {
	return (int64_t)(int32_t)(s->update_counter * 312871u);
}

/* Hurt camera roll in degrees about the view Z axis for partial tick
 * `partial`, plus the yaw conjugation angle Beta wraps around it
 * [740c583 EntityRenderer.hurtCameraEffect:161-167]:
 *   f = (hurtTime - partial) / maxHurtTime
 *   roll = -sin(f^4 * pi) * 14   applied as rotate(-yaw,Y) rotate(roll,Z)
 *                                rotate(+yaw,Y)
 * Returns 0 outside the hurt window. The max_hurt_time<=0 guard covers
 * Beta's own never-hurt 0.0F/0 NaN frame (only reachable at partial exactly
 * 0.0, where the roll is 0 anyway). The camera passes attacked_yaw_deg = 0
 * (see the header comment); the parameter exists so tests can pin the full
 * SP math. */
static inline float hud_hurt_camera_roll_deg(const struct hud_hurt_state* s,
											 float partial) {
	float f = (float)s->hurt_time - partial;
	if(f < 0.0F || s->max_hurt_time <= 0)
		return 0.0F;
	f /= (float)s->max_hurt_time;
	f = sinf(f * f * f * f * (float)M_PI);
	return -f * 14.0F;
}

/* Death camera roll in degrees about the view Z axis
 * [740c583 EntityRenderer.hurtCameraEffect:156-158]:
 *   40 - 8000 / (deathTime + partial + 200)
 * Active only while the observed health is <= 0. */
static inline float hud_hurt_death_roll_deg(const struct hud_hurt_state* s,
											float partial) {
	if(s->last_health > 0 || !s->primed)
		return 0.0F;
	float t = (float)s->death_time + partial;
	return 40.0F - 8000.0F / (t + 200.0F);
}

#endif
