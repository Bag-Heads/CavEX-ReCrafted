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

#ifndef ENTITY_COMBAT_H
#define ENTITY_COMBAT_H

// Beta 1.7.3 mob->player melee combat: the byte-faithful constants + the pure
// geometry/knockback helpers, shared by the engine (the proximity sweep in
// entity_monster.c, server_local_hurt_player in server_local.c) AND the unit
// test (tests/test_zombie_melee.c). Keeping them here means the Beta values
// cannot drift between the code and the test that pins them. Every constant
// cites its Beta 1.7.3 source.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>            // uint32_t for arrow_should_hit (owner/target ids)

#include "monsters_object.h"   // MONSTER_* ids for mob_melee_damage()
#include "../item/tool.h"      // tool_type/tool_tier for player_melee_damage()
#include "../worldgen/java_random.h" // bit-exact java.util.Random for the
                                     // Beta slime-chunk check (callers of
                                     // slime_chunk_is_slimy link java_random.c)

// Engine health scale: server_interface.h MAX_PLAYER_HEALTH=160, HEALTH_PER_HEART=16
// (10 hearts). One half-heart = 8 units. (Duplicated as a literal here to keep
// this header dependency-free; the test asserts the derivation.)
#define COMBAT_HEALTH_PER_HEART 16

// Zombie melee damage. Beta 1.7.3 EntityZombie's constructor sets
// attackStrength to 5 (half-hearts) and EntityMob.attackEntity passes it to
// attackEntityFrom with NO difficulty multiplier (per-difficulty melee scaling
// is a post-Beta / 1.8+ change). 5 half-hearts = 2.5 hearts = 5 * (16/2) = 40
// engine units. Confirmed against the mc173 faithful b1.7.3 reimpl
// (LivingKind::Zombie(_) => 5).
#define ZOMBIE_ATTACK_DAMAGE (5 * (COMBAT_HEALTH_PER_HEART / 2)) // 40 = 2.5 hearts

// Beta EntityGiantZombie ctor: attackStrength 50 [740c583] -- ten times the
// zombie's, same no-multiplier attackEntityFrom path. 50 half-hearts = 25
// hearts = 400 engine units: one unarmored hit is far beyond MAX_PLAYER_HEALTH
// (160), i.e. instantly lethal, exactly as in Beta.
#define GIANT_ATTACK_DAMAGE (50 * (COMBAT_HEALTH_PER_HEART / 2)) // 400 = 25 hearts

// Lightning strike damage. Beta Entity.onStruckByLightning deals 5 fire damage
// [740c583] -- dealFireDamage passes that amount on as a null-attacker hit
// when the target is not fire-immune, so this is the SAME 5-half-heart raw value
// and the SAME player-scale conversion as ZOMBIE_ATTACK_DAMAGE above (5 * 8 = 40
// = 2.5 hearts). Routes through the ordinary null-attacker damage path (armor
// mitigation applies to the player exactly as any other environmental hit).
#define LIGHTNING_STRIKE_DAMAGE (5 * (COMBAT_HEALTH_PER_HEART / 2)) // 40 = 2.5 hearts

// Attack reach. EntityMob.attackEntity gates on horizontal dist^2 < 2.0^2 plus
// a vertical AABB overlap.
#define MOB_ATTACK_REACH_SQ 4.0f

// Wolf melee reach [740c583: EntityWolf.attackEntity]: a TIGHTER 3D gate
// (3D distance under 1.5, entity_wolf_melee_in_range) than the generic 2.0
// EntityMob.attackEntity reach above -- wolves are the one mob on this roster
// whose actual bite range is narrower than MOB_ATTACK_REACH_SQ. Named here
// (rather than left as entity_wolf.c's own inline 1.5) so
// mob_path_follow_tail's chase-stop distance (FIX, mob-AI dead-zone lane) can
// share the SAME literal a wolf's melee dispatch uses -- two independent
// 1.5 values would silently drift out of sync the next time either one is tuned.
#define WOLF_MELEE_REACH 1.5f
#define WOLF_MELEE_REACH_SQ (WOLF_MELEE_REACH * WOLF_MELEE_REACH)

// Target-acquisition / follow range. Beta EntityMob.findPlayerToAttack ->
// World.getClosestVulnerablePlayerToEntity at 16 blocks; confirmed by the mc173
// faithful reimpl (16.0). (The 128 sometimes quoted online is a modded EntityMob
// template, not vanilla b1.7.3.)
#define MOB_FOLLOW_RANGE 16.0f
#define MOB_FOLLOW_RANGE_SQ (MOB_FOLLOW_RANGE * MOB_FOLLOW_RANGE)

// EntityLiving's own generic ambient idle-target range (8 blocks, set in
// updatePlayerActionState) -- the DIFFERENT, narrower 8-block latch every
// living mob's base class uses to occasionally look at (and, for a wolf,
// begin begging toward) a nearby player, distinct from MOB_FOLLOW_RANGE
// (16 blocks, EntityMob/EntityCreature's own hostile chase-acquisition range).
// [740c583: EntityLiving.updatePlayerActionState]
#define WOLF_CURRENT_TARGET_RANGE 8.0f
#define WOLF_CURRENT_TARGET_RANGE_SQ \
	(WOLF_CURRENT_TARGET_RANGE * WOLF_CURRENT_TARGET_RANGE)

// FH-13: wander-path search range. EntityCreature.updateWanderPath asks the
// world for a path to its chosen random nearby point within 10 blocks [740c583] --
// the search radius Beta uses for a wander target, distinct from
// MOB_FOLLOW_RANGE (16) which is the CHASE search range.
#define MOB_WANDER_SEARCH_RANGE 10.0f

// Mob swing cadence. attackTime is reset to 20 ticks (1.0s @ 20 TPS) on a swing
// and counts down each tick; the mob cannot swing again until it hits 0.
#define MOB_ATTACK_COOLDOWN_TICKS 20

// --- Wii-CONTROLLER PLAYABILITY tunables (NOT Beta values) ---------------------
// See CLAUDE.md "Beta 1.7.3 parity vs. Wii-controller playability" + docs/
// wii_controller_playability.md. These bend the FEEL/TIMING layer so a player on
// an IR-pointer + stick (no mouse) gets a fair reaction window; the simulation
// rules (damage, reach, cooldown, i-frames, knockback) above stay byte-exact.

// Cap on a mob's horizontal speed (blocks/tick). The fork's raw creeper tuning
// (CREEPER_MAX_SPEED 1.25 in entity_monster.c) lets a chasing mob reach an
// effective ~7-8 blocks/sec and close ~8 blocks in ~1s -- no time to turn, aim
// the crosshair, and react on a Wiimote. This caps the effective speed to a
// Wii-playable pace. EMPIRICALLY TUNED on Dolphin (see the doc); keep it a named
// constant so it can't silently revert.
#define MOB_MAX_SPEED_WII 0.18f

// Pre-attack wind-up: ticks a mob must stay in melee reach before it swings (a
// telegraph). Gives the player ~0.4s to read the threat and back off / swing
// first, instead of being hit on the first contact tick. Re-armed each swing.
#define MOB_ATTACK_WINDUP_TICKS 8

// Per-mob ground-speed cap. The faithful Beta moveSpeed ordering (monsters_object.c:
// zombie 0.5 < skeleton=creeper 0.7 < spider 0.8) is preserved under the Wii nerf,
// but only the SPIDER -- the one mob whose whole identity is to catch a walking
// player -- gets a RAISED cap; the skeleton (its real threat is the bow) and the
// creeper (its threat is the blast) are held at the base cap so they don't out-walk
// the player on an IR-pointer + stick (no sprint yet). Tunable, pending a real-Wii
// feel-test. Takes the mob's faithful move_speed; the spider (Beta 0.8) is the only
// one above the 0.7 tier.
#define MOB_SPIDER_SPEED_MULT_WII 1.4f
static inline float mob_speed_cap_wii(float faithful_move_speed) {
	return (faithful_move_speed > 0.7f)
		? MOB_MAX_SPEED_WII * MOB_SPIDER_SPEED_MULT_WII
		: MOB_MAX_SPEED_WII;
}

// Player hurt-invulnerability ("i-frames"). EntityLiving maxHurtResistantTime is
// 20; attackEntityFrom absorbs a hit while hurtResistantTime > max/2, so the
// effective re-damage window for equal-magnitude hits is 10 ticks (0.5s).
#define PLAYER_HURT_RESIST_TICKS 10

/* FAITHFUL SIMULATION -- Beta 1.7.3 ItemArmor.damageReduceAmountArray and
 * EntityPlayer.damageEntity (740c583).  Armor points are per SLOT, never per
 * material; material changes only maxDamage. */
#define ARMOR_REDUCTION_HELMET 3
#define ARMOR_REDUCTION_CHESTPLATE 8
#define ARMOR_REDUCTION_LEGGINGS 6
#define ARMOR_REDUCTION_BOOTS 3
#define ARMOR_DAMAGE_DIVISOR 25

// Player fall damage in engine hp. Beta EntityLiving.fall: damage =
// ceil(fallDistance - 3) HALF-hearts (one point == COMBAT_HEALTH_PER_HEART/2).
// fall_distance is the CONTINUOUS drop height (blocks), so the ceil must be
// applied here -- the old code billed a FULL heart per point (2x) AND truncated a
// pre-floored integer distance (under-billing fractional falls). Zero on a ladder
// (Beta zeroes fallDistance every ladder tick, EntityLiving.moveEntityWithHeading).
static inline int player_fall_damage_hp(float fall_distance, bool on_ladder) {
	if(on_ladder)
		return 0;
	int pts = (int)ceilf(fall_distance - 3.0f);
	if(pts <= 0)
		return 0;
	return (COMBAT_HEALTH_PER_HEART / 2) * pts;
}

// Knockback (EntityLiving.knockBack): 0.4 horizontal along the (target-attacker)
// direction and +0.4 vertical (clamped). Beta applies these as a velocity
// impulse integrated by full physics; this engine applies a simplified
// server-side decaying shove of the same magnitudes (see server_local.c) since
// the player is client-simulated and has no server-side physics integrator.
#define KNOCKBACK_H 0.4f
#define KNOCKBACK_V 0.4f

// Mob-side damage timers. Beta arms hurtTime/maxHurtTime to 10 for the animation
// and hurtResistantTime/maxHurtResistantTime to 20 for arbitration. Damage is
// blocked only during the latter timer's upper half (>10), so Cave stores that
// effective half-window as 10 ticks. The two clocks usually count together but
// are distinct: HurtTime persists in NBT, whereas hurtResistantTime and the
// strongest-hit field do not. Decremented once per living tick.
#define MOB_HURT_ANIMATION_TICKS 10
#define MOB_HURT_RESIST_TICKS 10

/* Pure EntityLiving.attackEntityFrom arbitration over Cave's effective
 * resistance half-window. Returns the health points to apply: full on a fresh
 * hit, zero for an equal/weaker in-window hit, or only the stronger-hit excess.
 * `fresh` is Beta's local fresh-hit flag (animation/knockback window). */
static inline int mob_iframe_damage(int incoming, int* hurt_resist_time,
									int* last_damage, bool* fresh) {
	*fresh = false;
	// M4 S5: incoming == 0 is a REAL Beta case (the thrown egg's
	// zero-damage attackEntityFrom from the thrower): outside a window it opens a FRESH window
	// with lastDamage 0 -- hurt animation + knockback, zero health change --
	// while inside a window 0 <= lastDamage always absorbs. Only negative input
	// (never produced by Beta) early-outs. [740c583: EntityLiving.attackEntityFrom;
	// EntityEgg.onUpdate hits the struck entity for 0 from the thrower]
	if(incoming < 0)
		return 0;
	if(*hurt_resist_time > 0) {
		if(incoming <= *last_damage)
			return 0;
		int excess = incoming - *last_damage;
		*last_damage = incoming;
		return excess;
	}
	*last_damage = incoming;
	*hurt_resist_time = MOB_HURT_RESIST_TICKS;
	*fresh = true;
	return incoming;
}

// --- audit #17f: emissive-tint render overlay (creeper swell + mob hurt flash) --
// The DECISION logic for the FIFO-safe emissive overlay lives here (pure, unit-
// tested in tests/test_emissive_tint.c) so the platform gfx/render code only has to
// call gfx_entity_tint(r,g,b,amount) with the values these helpers produce. The GX
// (Wii) + GL (PC) backends draw the same lerp(model, tint, amount); amount==0 ==
// the model exactly as today (graceful degrade). NOTHING here touches GX state.

// CREEPER WHITE SWELL FLASH (Beta RenderCreeper.updateCreeperColorMultiplier).
// Beta's behaviour [740c583]: the flash ratio is the partial-tick lerp of the swell
//   timer (previous value toward timeSinceIgnited) divided by 28, RAW and UNCLAMPED
//   (see below). Ten times the ratio, truncated, picks a bucket: an EVEN bucket
//   returns no overlay (the fast on/off toggle); an ODD bucket returns white whose
//   alpha is ratio * 0.2 * 255, truncated and clamped to 0..255 -- a CONTINUOUS
//   ramp, not a flat value.
// i.e. a WHITE overlay that pulses faster as detonation nears, whose ALPHA on the
// "on" bucket ramps continuously with the raw fuse ratio -- nearly invisible
// early in the fuse (ratio ~0.1 -> alpha ~2%) and intensifying pulse-by-pulse toward the
// ~20% ceiling only as the ratio nears 1 (near detonation), NOT a flat square-wave that snaps to
// full 20% on the very first primed pulse. Critically, the ratio here is NOT clamped to
// [0,1] before use (unlike updateCreeperScale's f^4 term, which explicitly clamps) --
// it can run slightly past 1.0 in the last tick before detonation (lastActiveTime 29,
// timeSinceIgnited 30 -> ratio up to 30/28 ~ 1.071), nudging the alpha a bit past the
// nominal 20% ceiling before the int clamp catches it.
// We render it as a lerp toward white of `amount` on the "on" phase (lerp `amount` ==
// frag*(1-amount) + white*amount; visually equivalent to Beta's additive alpha/255
// wash, never over-saturates). fuseTime-2 == CREEPER_FUSE_TICKS-2 == 28.
#define CREEPER_FLASH_TINT_R 1.0f
#define CREEPER_FLASH_TINT_G 1.0f
#define CREEPER_FLASH_TINT_B 1.0f
#define CREEPER_FLASH_TINT_AMOUNT 0.2f // RenderCreeper's ratio*0.2 coefficient (the
                                       // ceiling `amount` ramps toward as the ratio->1, NOT
                                       // a flat wash amount -- see creeper_flash_amount)

// RED HURT / DEATH WASH (Beta 1.7.3 RenderLiving.doRenderLiving, RenderLiving.java:64-75).
// Beta sets hurtTime and maxHurtTime to 10 on fresh damage and decrements it 1/tick, so the
// hurt half holds for 10 ticks (0.5s); the dying half holds while deathTime > 0 (gate:
// entity_living_red_tint). Its fresh duration matches the effective resistance
// half-window, but it is a separate persisted field. Beta's overlay pass, in words:
//   - the colour value b is the entity brightness at the partial tick (the same
//     value RenderManager.java:107-108 lights the model with);
//   - texturing and alpha test go off, standard alpha blending (source alpha /
//     one minus source alpha) goes on with an equal-depth test, and the model is
//     redrawn in (b, 0, 0) at alpha 0.4.
// i.e. each model pixel becomes 0.4*(b,0,0) + 0.6*dst -- a DARK wash in a dark cave,
// never a flat bright red. gfx_entity_tint's lerp (1-t)*CPREV + t*tint with t = 0.4 IS
// that blend (GX quantizes t to 102/255 == 0.4). b is the brightness the model was
// lit by: callers pass light_brightness_current() of the SAME packed light byte they fed
// render_entity_update_light (no second light lookup). Pinned by test_hurt_wash_parity.
// (next-gaps-0924/R1b: replaces a (1.0,0.4,0.4)@0.3 tone that had no Beta or controller
// basis; the old soft-red note about the 1.8+ (1.0,0.6,0.6) multiply still holds.)
#define HURT_FLASH_TINT_G 0.0f
#define HURT_FLASH_TINT_B 0.0f
#define HURT_FLASH_TINT_AMOUNT 0.4f // Beta overlay alpha, standard source-alpha blend

// The wash one hurt/dying model draws with, for the brightness its model is lit by.
struct hurt_wash {
	float r, g, b, amount;
};
static inline struct hurt_wash hurt_wash_for_brightness(float brightness) {
	return (struct hurt_wash) {brightness, HURT_FLASH_TINT_G, HURT_FLASH_TINT_B,
							   HURT_FLASH_TINT_AMOUNT};
}

// CHARGED CREEPER (weather slice, minimal render pass). Real Beta renders a charged
// creeper with a second alpha-blended "creeper_armor"-style lightning-arc overlay pass
// (RenderCreeper -- a distinct textured model layer, not a flat tint); that overlay is
// genuinely a separate render pass (new texture + blend state) and is DEFERRED as a
// documented simplification (see WEATHER_SLICE_NOTES.md). This is the honest minimal
// first pass: a persistent, low-amount blue wash reusing the SAME gfx_entity_tint hook
// as the hurt/swell flashes above, so a charged creeper reads as visually distinct at
// zero new render infrastructure. Below hurt/swell precedence (entity_monster.c).
#define CHARGED_TINT_R 0.6f
#define CHARGED_TINT_G 0.7f
#define CHARGED_TINT_B 1.0f
#define CHARGED_TINT_AMOUNT 0.15f
#define MOB_HURT_FLASH_TICKS MOB_HURT_ANIMATION_TICKS

// Clamp a tint amount into the [0,1] the backends expect. amount<=0 -> 0 == the model
// rendered exactly as today (the graceful-degrade contract); amount>=1 -> fully tinted.
static inline float tint_amount_clamp(float amount) {
	if(amount < 0.0f)
		return 0.0f;
	if(amount > 1.0f)
		return 1.0f;
	return amount;
}

// NOTE: creeper_flash_amount() needs CREEPER_FUSE_TICKS, which is defined in the
// creeper proximity-fuse section further down -- so the helper lives there, right
// after that #define (search "creeper_flash_amount"). The constants + the
// fuse-independent helpers (tint_amount_clamp, hurt_flash_active) stay here.

// Is the red hurt-flash active for a mob whose networked hurt timer is `hurt_time`
// ticks (> 0 while the mob is in its post-hit i-frame window)? The renderer wraps the
// mob draw in the red tint while this is true. Pure predicate so it is unit-testable.
static inline bool hurt_flash_active(int hurt_time) {
	return hurt_time > 0;
}

// Ticks a player-knocked mob suppresses its own chase/wander steer + speed cap so
// the knockback impulse actually carries (this engine re-accelerates + clamps the
// mob to walk speed every tick, which would otherwise shear the shove to nothing).
// 5 ticks (0.25s) of slide; friction decays the impulse over that window.
#define MOB_KNOCKBACK_TICKS 5

// --- Beta 1.7.3 persistent TARGET LATCH + retaliation (audit #14) -------------
// Beta EntityCreature.updatePlayerActionState keeps `entityToAttack` (playerToAttack
// in this decompile) set ACROSS ticks once findPlayerToAttack acquires it: the
// acquisition branch is skipped entirely while a target is held. A momentary loss of
// canEntityBeSeen does NOT clear the target -- it only routes that tick to the EMPTY
// attackBlockedEntity() (no swing) while pathToEntity is STILL re-planned to the live
// target and the mob keeps walking + faceEntity'ing it. Beta drops the target ONLY on:
//   (a) target dead/gone (not isEntityAlive -> cleared), and
//   (b) SPIDER ONLY: a 1-in-100 per-tick give-up while it can SEE the target AND it is
//       bright (getEntityBrightness > 0.5 == eff light >= 12). No generic distance/leash
//       drop exists -- the 16.0 only bounds getPathToEntity's SEARCH, not the target.
//
// v2 stand-in: a fixed per-mob patience COUNTDOWN (aggro_time, ticks). The latch is
// (re)armed to MOB_TARGET_RETENTION_TICKS on a fresh acquire (can_acquire && can_see &&
// in range) OR on retaliation (player hit, bypassing light/LOS), refreshed to full every
// tick the mob still can_acquire && can_see, and decremented otherwise. While it is > 0
// the mob CHASES (AI_CHASE, steer at the player) through brief LOS flicker; it reverts to
// wander only when it hits 0. We MUST also drop it when the player flees past a leash
// range -- v2's beeline chase (no A* path-range fail-to-path) would otherwise let a mob
// trail the player forever, whereas Beta's mob simply stops when it can't path. The leash
// is the observable equivalent of Beta's "fails to find a path past ~16 blocks and stands".
//
// RETENTION VALUE: Beta's persistence is open-ended (until death/path-loss), so there is
// no byte-exact tick count to copy -- this fixed window is a sanctioned playability/feel
// approximation (CLAUDE.md). 60 ticks (3s @ 20 TPS) is grounded in Beta's own give-up
// cadence: the spider daytime 1/100-per-tick give-up has an expected life of ~100 ticks
// (~5s) of clear-bright sight, and EntityCreature re-rolls a wander path on a ~1/80 chance
// only when it has NO target; 3s comfortably outlasts a wall-corner / doorway LOS flicker
// (a few ticks) without trailing a fled, fully-out-of-sight player indefinitely.
#define MOB_TARGET_RETENTION_TICKS 60 // 3.0s @ 20 TPS of chase past last sight (feel value)

// Leash range: the latch is dropped the instant the player is this far away, regardless of
// remaining patience, so a fled player frees the mob (the v2 beeline equivalent of Beta's
// mob failing to path past its search range and standing). Set to the follow range so a
// mob holds aggro out to exactly where it could have acquired -- no farther.
#define MOB_LEASH_RANGE_SQ MOB_FOLLOW_RANGE_SQ

// Pure latch decision (pinned by tests/test_mob_target_latch.c). Returns true iff a mob
// with `latch_ticks` of remaining patience should KEEP chasing its (implicit) player
// target this tick: it must still have patience left AND the player must be inside the
// leash. can_see is intentionally NOT a parameter -- the whole point of the latch is that
// chase persists through can_see flicker (the per-attack sites keep their own can_see gate
// separately). The caller arms/refreshes/decrements latch_ticks around this test.
static inline bool mob_should_hold_target(int latch_ticks, float dist_sq,
										  float leash_sq) {
	return latch_ticks > 0 && dist_sq < leash_sq;
}

// Beta 1.7.3 PLAYER melee damage vs a mob, in HALF-HEARTS on the mob's raw 20-pt
// health scale (mob health 20 == 10 hearts, 1 pt = half a heart). EntityPlayer's
// attack uses InventoryPlayer.getDamageVsEntity = the held item's weaponDamage, or
// 1 for an empty hand / non-weapon item (Item.getDamageVsEntity's base default
// [740c583]). NO difficulty/crit/enchant scaling exists in b1.7.3's base melee.
// The result is on the MOB scale (not *8), unlike mob->player damage, because it
// hits a 20-pt mob. Pure in (tool_type, tool_tier) so tests/test_player_melee.c
// can pin it.
//
// PARITY FIX (audit register 2026-08-25, items-recipes #1): the formula below was
// previously `4 + dve` for swords (dropping ItemSword's own `*2`) and one SHARED
// `1 + dve` table for pickaxe/shovel/axe (collapsing ItemTool's three distinct
// per-subclass bases into one) -- both independently re-derived and confirmed
// wrong against the decompile. Ground truth, cited per class:
//   ItemSword ctor [740c583]: weapon damage is 4 plus twice the material's value.
//   EnumToolMaterial.damageVsEntity [740c583]: WOOD=0, STONE=1, IRON=2,
//     EMERALD(diamond)=3, GOLD=0 (gold shares wood's raw 0, a real Beta quirk).
//   ItemTool ctor [740c583]: entity damage is a per-subclass base plus the material
//     value, the base being fixed by each subclass constructor when it calls the
//     ItemTool ctor: ItemSpade (shovel) passes 1, ItemPickaxe passes 2,
//     ItemAxe passes 3 -- three DIFFERENT tables, not one shared value.
// True per-tier damage: Sword WOOD/GOLD=4 STONE=6 IRON=8 DIAMOND=10; Shovel
// WOOD/GOLD=1 STONE=2 IRON=3 DIAMOND=4; Pickaxe WOOD/GOLD=2 STONE=3 IRON=4
// DIAMOND=5; Axe WOOD/GOLD=3 STONE=4 IRON=5 DIAMOND=6.
static inline int player_melee_damage(enum tool_type type, enum tool_tier tier) {
	int dve; // EnumToolMaterial.getDamageVsEntity() -- the RAW per-tier value,
	         // not the old +1-biased "mat" encoding.
	switch(tier) {
		case TOOL_TIER_WOOD:    dve = 0; break;
		case TOOL_TIER_GOLD:    dve = 0; break; // shares wood's raw 0 (real Beta quirk)
		case TOOL_TIER_STONE:   dve = 1; break;
		case TOOL_TIER_IRON:    dve = 2; break;
		case TOOL_TIER_DIAMOND: dve = 3; break;
		default:                dve = -1; break; // no/unknown tier -> not a real tool
	}
	if(dve < 0)
		return 1; // degenerate tier on any type: Item.getDamageVsEntity default (fist)
	switch(type) {
		case TOOL_TYPE_SWORD:
			return 4 + dve * 2;   // ItemSword: 4/4/6/8/10 (wood/gold/stone/iron/diamond)
		case TOOL_TYPE_SHOVEL:
			return 1 + dve;       // ItemSpade: base 1 -> 1/1/2/3/4
		case TOOL_TYPE_PICKAXE:
			return 2 + dve;       // ItemPickaxe: base 2 -> 2/2/3/4/5
		case TOOL_TYPE_AXE:
			return 3 + dve;       // ItemAxe: base 3 -> 3/3/4/5/6
		default:
			return 1;              // hoe / block / empty hand (fist)
	}
}

// Box geometry. The player network position is the EYE; feet are EYE_HEIGHT below it.
// Player body 0.6x1.8 (faithful). Zombie/skeleton/creeper also inherit Entity's
// 0.6x1.8 default; spiders and animals override it in their constructors.
#define PLAYER_EYE_HEIGHT 1.62f
#define PLAYER_BODY_HEIGHT 1.8f
#define MOB_BODY_HEIGHT 1.8f

// Saddle-slice review fix (finding 1): Entity.updateRiderPosition
// [740c583] -- the rider is placed at the mount's X/Z and at the mount's Y plus
// the mount offset plus the rider's own Y offset. The mount offset is 0.75 x height;
// the pig's constructor sizes it 0.9 x 0.9 -> height 0.9, so 0.9*0.75 = 0.675. Shared by
// the server follow (entity_monster.c) and the client follow
// (entity_local_player.c) so both sides compute the identical rider Y.
#define PIG_MOUNTED_Y_OFFSET 0.675f

// Spider jockey mount linkage (parity gap fill, spider/skeleton jockey lane).
// [740c583: EntitySpider.getMountedYOffset] -- the SPIDER is the ONE Beta
// 1.7.3 mob that OVERRIDES Entity.getMountedYOffset (base: 0.75 x height, no
// offset): it returns 0.75 x its height minus 0.5. height =
// SPIDER_BODY_HEIGHT (0.9), so 0.9*0.75 - 0.5 = 0.175 -- a jockey rides low
// over the spider's flat body, not high in the air like a saddled pig's
// rider (PIG_MOUNTED_Y_OFFSET above, which uses the un-overridden base
// formula). The rider's own Y offset adds 0 (EntitySkeleton/Entity's
// default yOffset is 0.0, never overridden) -- see Entity.
// updateRiderPosition's sum of mount Y, mount offset and rider
// offset, the SAME call the pig-rider precedent above already cites.
#define SPIDER_JOCKEY_MOUNTED_Y_OFFSET (SPIDER_BODY_HEIGHT * 0.75f - 0.5f)

// --- Beta 1.7.3 line-of-sight eye heights (canEntityBeSeen) -------------------
// EntityLiving.canEntityBeSeen ray-traces eye-to-eye from feet plus eye height to
// the target's feet plus its eye height. For a MOB the eye height is 0.85 x height and
// the mob's posY is at its FEET (yOffset 0), so the mob LOS endpoint is
// feet + height*0.85. The standard 1.8-tall biped hostiles (zombie, skeleton,
// creeper) give 1.8*0.85 = 1.53; the spider (setSize 1.4x0.9) gives 0.9*0.85 =
// 0.765. (These match the mc173 faithful reimpl's 0.85 eye factor and size table.)
// The player network position in this engine is ALREADY the eye (combat.h note
// above), so the player LOS endpoint is its raw {x,y,z} -- no offset added. (Beta's
// player would add another 0.12 atop its camera posY; the ~0.12 the engine omits is
// well within a block and not worth crossing the eye/feet conventions for.)
#define MOB_BODY_HEIGHT_BETA  1.8f  // Beta biped hostile height (EntityLiving size)
#define SPIDER_BODY_HEIGHT    0.9f  // EntitySpider ctor size: 1.4 wide, 0.9 tall

// --- Beta 1.7.3 spider COLLISION/INTERACTION box dimensions (audit #17a) -------
// EntitySpider constructor sizes the spider 1.4 wide by 0.9 tall. Entity.setSize
// stores width=1.4, height=0.9 and Entity.setPosition builds the AABB as
// width x height x width centered on X/Z with the FEET at posY (yOffset 0): so the
// box spans [posX-0.7, posX+0.7] x [posY, posY+0.9] x [posZ-0.7, posZ+0.7]. The
// AABB is therefore 1.4 (X) x 0.9 (Y) x 1.4 (Z), base AT THE FEET. mc173 corroborates
// byte-for-byte (LivingKind::Spider => DVec3::new(1.4, 0.9, 1.4)). This is the SAME
// feet-base, X/Z-centered shape make_spider_bbox builds with oy = height/2 (the
// other 3 biped hostiles keep the 0.6-wide creeper box). A faithful spider is WIDER
// (1.4 vs 0.6: half-extent 0.7 vs 0.3 -> cannot fit a 1-wide gap a biped squeezes
// through) and SHORTER (0.9 vs 1.7: fits under a 1-high overhang). SPIDER_BODY_HEIGHT
// (0.9) above is the same value reused for the box height; this WIDTH had no constant.
#define SPIDER_BBOX_WIDTH     1.4f  // EntitySpider ctor width 1.4: X/Z extent
#define SPIDER_BBOX_HEIGHT    SPIDER_BODY_HEIGHT // 0.9: EntitySpider ctor height
#define MOB_EYE_HEIGHT_BIPED  (MOB_BODY_HEIGHT_BETA * 0.85f) // 1.53: zombie/skel/creeper
#define MOB_EYE_HEIGHT_SPIDER (SPIDER_BODY_HEIGHT * 0.85f)   // 0.765: spider

// Per-type mob eye height (Beta getEyeHeight = height*0.85), data-driven from
// monster_def.eye_height so the LOS gate (mob_can_see_player) and the drown head-cell
// probe can't drift to different eye heights, and a new animal id gets its own eye from
// the table (biped 1.53, spider 0.765, pig 0.765, cow/sheep 1.105, chicken 0.34, ...).
static inline float mob_eye_height(int monster_id) {
	return monster_def_get(monster_id)->eye_height;
}

// Per-type mob BODY height (the AABB Y-extent = box_h), data-driven from
// monster_def.box_h -- the SAME height the collision box is built from (so the MELEE
// vertical-overlap test stays in lockstep with the collision + raycast boxes, audit
// #17a) and the height the brightness sample reads. Spider 0.9, biped 1.8, animals
// their own (pig 0.9, cow/sheep 1.3, chicken 0.4). The box makers read the same box_h.
static inline float mob_body_height(int monster_id) {
	return monster_def_get(monster_id)->box_h;
}

// Mob bounding-box height for the MELEE vertical-overlap gate (parity fix 2026-06-19).
// Beta EntityMob.attackEntity tests the mob's REAL bounding box: the biped hostiles
// (zombie/skeleton/creeper) are 1.8 tall (EntityLiving default,
// MOB_BODY_HEIGHT_BETA). Non-biped mobs keep their constructor box_h.
static inline float mob_melee_box_height(int monster_id) {
	return (monster_id == MONSTER_ZOMBIE || monster_id == MONSTER_SKELETON
			|| monster_id == MONSTER_CREEPER)
		? MOB_BODY_HEIGHT_BETA
		: mob_body_height(monster_id);
}

// --- AUDIT #18 ITEM 7: Beta 1.7.3 getEntityBrightness sample point -------------
// Entity.getEntityBrightness(float) reads the block light at
//   Y = floor(posY - yOffset + (maxY-minY)*0.66)  ==  floor(feet + 0.66*bodyHeight)
// i.e. a TORSO point at 66% of the AABB height -- NOT the foot block, and NOT the
// 0.85 canEntityBeSeen LOS eye. This getEntityBrightness drives BOTH the undead
// sun-burn gate (b > 0.5) AND the spider daytime-passivity gate (b < 0.5). v2 applies
// the EXACT Beta 0.66 fraction to its collision-box height (mob_body_height: 1.8 for
// the biped box, 0.9 for the spider). So the biped sample is feet + 1.188; the
// 0.9-tall spider samples feet + 0.594 (still the feet block in most cases). This is
// DISTINCT from mob_eye_height (height*0.85), which stays the LOS height for
// canEntityBeSeen. Usually the same column light value, but it differs when the mob
// straddles a light gradient (half-in shadow, under a 1-deep overhang) -- the faithful
// 0.66-body sample can ignite/passivate where the foot/eye block would not. Verbatim vs
// the PeytonPlayz595/Beta-1.7.3 Entity.getEntityBrightness.
#define MOB_BRIGHTNESS_SAMPLE_FRAC 0.66f // getEntityBrightness: 0.66 of the AABB height

// World Y at which Beta samples a mob's block-brightness (feet + 0.66*bodyHeight). The
// caller floors this to a block coord and reads sky/torch light there. Pure so the test
// can pin the exact biped (feet + 0.66*1.8 = feet+1.188) / spider (feet+0.594) sample.
static inline float mob_brightness_sample_y(float feet_y, int monster_id) {
	return feet_y + MOB_BRIGHTNESS_SAMPLE_FRAC * mob_body_height(monster_id);
}

// --- Beta 1.7.3 daylight sun-burn (UNDEAD: zombie + skeleton) -----------------
// EntityZombie.onLivingUpdate AND EntitySkeleton.onLivingUpdate each wrap the same
// branch: in daytime it reads the full-tick entity brightness b; when b exceeds 0.5,
// the mob's block can see the sky and a uniform float x 30 is below (b minus 0.4) x 2, the fire counter becomes 300.
// b1.7.3 sets the fire field DIRECTLY to 300 ticks (15s) -- the 8-second setFire form is
// POST-Beta. Fire then deals 1 half-heart every 20 ticks. Spiders inherit no burn
// branch and creepers have none, so ONLY zombie + skeleton burn. In this engine the
// faithful-equivalent gate is: isDaytime() (skylightSubtracted below 4 -- the OUTER
// wrap Beta checks BEFORE it ever samples getEntityBrightness) AND the mob's foot can
// see the sky (foot_y >= the column heightmap == canBlockSeeTheSky) AND the effective
// sky light (raw sky_light minus the day/night subtraction, OR the block/torch light
// at the same cell if brighter -- see AUDIT #18 ITEM 7 above) >= 12 (~
// getEntityBrightness > 0.5 at the sky-exposed surface). Standing in/under water
// cancels fire; a block overhead (can't see sky) prevents it. There is NO
// helmet/armor burn-prevention in b1.7.3.
// PARITY FIX (2026-09-01): isDaytime() was MISSING here entirely. The gate used to
// be just can_see_sky && eff_light>=12, which was a correct proxy for isDaytime()
// ONLY as long as eff_light was purely sky-derived (an open-sky column's raw
// sky_light is always 15, so eff>=12 iff sky_subtract<=3 iff isDaytime()). That
// equivalence broke the moment the audit fix above folded BLOCK/TORCH light into eff
// (max(sky,block)): a torch (block light >= 12) under open sky now clears eff>=12 at
// ANY time of day, including full night, letting an undead mob draw an ignition roll
// when Beta structurally cannot (isDaytime() short-circuits before getEntityBrightness
// is ever read). Fixed by threading is_daytime -- supplied by the caller via
// daytime_is_daytime_ticks (source/daytime.h:106, the byte-pinned isDaytime port; same
// wiring as block_bed.c's sleep gate) -- through as this function's own outer &&
// term, so the predicate can no longer be satisfied by light alone.
#define MOB_FIRE_TICKS 300           // 15s @ 20 TPS; b1.7.3 sets the fire field to 300
#define MOB_FIRE_DAMAGE_INTERVAL 20  // one fire-damage tick per second
// IMPORTANT scale note: MOBS use Beta's raw 20-point health (struct monster_def
// .health = 20 == 10 hearts, so 2 points = 1 heart, 1 point = HALF a heart), which
// is a DIFFERENT scale from the PLAYER's 160-point health (COMBAT_HEALTH_PER_HEART
// = 16). Beta's fire deals 1 (a half-heart) per second, so on the mob scale this is
// literally 1 -- NOT COMBAT_HEALTH_PER_HEART/2 (that would be a player-scale value
// and would burn a 20-pt mob to death in ~2.5s instead of the faithful ~20s).
// ZOMBIE_ATTACK_DAMAGE above is player-scale because it hits the PLAYER; this hits
// the MOB, so it is mob-scale.
#define MOB_FIRE_DAMAGE 1            // 1 = half a heart on the mob's 20-pt scale (Beta)
#define UNDEAD_SUNBURN_SKY_MIN 12    // eff light >= 12 ignites. Parity fix 2026-06-19:
                                     // the Beta FLOORED table (WorldProvider.generate-
                                     // LightBrightnessTable, *(1-0.05)+0.05) gives
                                     // brightness[11]=0.437 (<0.5) and [12]=0.525 (>0.5),
                                     // so getEntityBrightness above 0.5 first clears at 12. The
                                     // old 13 came from a floorLESS table that wrongly put
                                     // [12] at exactly 0.5 (light 12 then never burned).

// Deterministic part of the sun-burn gate (the caller applies the random ignition roll +
// the in-water suppress). True iff an undead mob is exposed to bright-enough daytime
// light: isDaytime() (is_daytime -- see the PARITY FIX note above; the caller supplies
// this via daytime_is_daytime_ticks) AND it can see the sky AND the effective light =
// max(skyLight-subtract, blockLight) clears the threshold. eff>=12 is EXACTLY
// light_lookup_overworld[eff] > 0.5 (the floored Overworld table is monotonic), so the
// light term alone is byte-equivalent to Beta's brightness-above-0.5 test WITHOUT pulling
// the table into combat.h (keeps the pure-helper test linkable; the caller uses the real
// table for the (b-0.4)*2 roll magnitude). is_daytime is a REQUIRED parameter (not an
// optional/defaulted extra) so no call site can silently reintroduce the light-only gap
// this parity fix closed.
static inline bool undead_sunburn_exposed(bool is_daytime, int eff_light,
										  bool can_see_sky) {
	return is_daytime && can_see_sky && eff_light >= UNDEAD_SUNBURN_SKY_MIN;
}

// Which mob ids run the daylight ignition mechanism at all (the mid == ... gate
// server_tick_monster's sun-burn block wraps around undead_sunburn_exposed).
// Beta EntityZombie.onLivingUpdate and EntitySkeleton.onLivingUpdate each
// implement this directly. EntityPigZombie EXTENDS EntityZombie and its own
// onLivingUpdate override only calls the parent implementation
// (decompile-verified, 740c583:EntityPigZombie.java) -- it inherits the
// mechanism wholesale, so it belongs in this gate too. What makes a pig
// zombie look unaffected by the sun is NOT this gate -- it is
// monster_def[MONSTER_PIG_ZOMBIE].fire_immune (the ctor marks it fire-immune):
// the next Entity.onEntityUpdate decays the 300 by 4 (:204-207), the burning
// flag is computed on that 296 (:226), and EntityLiving.onEntityUpdate:117
// then zeroes fire in the same call. So each daylight roll shows exactly ONE
// broadcast tick of fire and never damages -- a real but cosmetic one-tick
// flicker, distinct from "never ignites" (G6c; mob_fire_tick_decay below).
// Parity fix 2026-09-02: the gate previously omitted MONSTER_PIG_ZOMBIE, so a
// sky-exposed pig zombie never drew the ignition roll at all (no flicker, and
// the shared rand_src draw count for that mob silently diverged from Beta's).
static inline bool mob_is_undead_sunburn_candidate(int monster_id) {
	return monster_id == MONSTER_ZOMBIE || monster_id == MONSTER_SKELETON
		|| monster_id == MONSTER_PIG_ZOMBIE;
}

// Pure decision for ONE tick of the fire DoT branch (entity_monster.c), split out
// so the fire-immune divergence is a directly-testable production function rather
// than duplicated test-side logic. Beta Entity.onEntityUpdate (decompile-verified,
// 740c583:Entity.java:240-251):
//     while fire is still positive, a fire-immune entity loses 4 fire ticks
//     (floored at 0); any other entity takes 1 null-attacker damage when fire
//     is a multiple of 20, then loses one fire tick.
//     (Only this branch of the fire update is mirrored here.)
// Precondition: fire_time > 0 (the caller's existing outer gate). Returns the next
// fire_time and writes whether the caller should apply the periodic damage THIS
// tick (always false when fire_immune -- an immune mob never runs the multiple-of-20
// check at all, it isn't merely damage-suppressed at the mortal decay rate).
// Immune: the -4 above is followed in the SAME tick by EntityLiving.
// onEntityUpdate:117 zeroing fire for an immune entity -- the -4 only feeds the
// burning flag Beta computes between them (Entity.java:226), which this port
// takes from mob_immune_fire_flag below; the stored result is 0 outright
// (G6c).
static inline int mob_fire_tick_decay(int fire_time, bool fire_immune,
									  bool* out_apply_damage) {
	if(fire_immune) {
		*out_apply_damage = false;
		return 0;
	}
	*out_apply_damage = (fire_time % MOB_FIRE_DAMAGE_INTERVAL) == 0;
	return fire_time - 1;
}

// The burning flag Beta publishes for a fire-immune mob this tick:
// Entity.onEntityUpdate:204-207 decays by 4, :226 sets burning flag bit 0 from fire above 0
// reads that value, and only THEN EntityLiving.onEntityUpdate:117 zeroes fire
// in the same call. fire_time is the value entering the fire branch (after
// handleWaterMovement's clear). A daylight roll (fire set to 300 in onLivingUpdate,
// after this point) therefore shows on the tick AFTER the roll, for exactly
// one tick (G6c). Mortal mobs publish fire_time > 0 at the end of the tick.
static inline bool mob_immune_fire_flag(int fire_time) {
	return fire_time - 4 > 0;
}

// --- Beta 1.7.3 living-entity ENVIRONMENTAL HAZARDS (audit #15) ----------------
// Fall, drowning, and lava/fire contact for the 4 hostiles, all on the MOB raw
// 20-pt health scale (1 pt = HALF a heart). Beta's onLivingUpdate applies each of
// these via attackEntityFrom with a RAW point value -- those values are used here
// DIRECTLY (NOT *COMBAT_HEALTH_PER_HEART/8): that *8/*16 scaling is ONLY for damage
// that hits the 160-pt PLAYER (ZOMBIE_ATTACK_DAMAGE / ARROW_DAMAGE). Mixing them up
// is exactly the trap that bit the sun-burn slice; MOB_FIRE_DAMAGE=1 is the existing
// correct precedent. Beta source: PeytonPlayz595/Beta-1.7.3 decompiled Entity.java +
// EntityLiving.java + EntitySpider.java; mc173 (mindstorm38) corroborates fire_time
// = 600 and the i-frame absorb constants.

// FALL DAMAGE. EntityLiving.fall rounds (fall distance - 3) up and, when that is
// positive, deals it as null-attacker damage -- so a fall of 3.0 -> 0 (no damage),
// 3.01..4.0 -> 1, 4.01..5.0 -> 2, ... i.e. damage = ceil(fallDistance - 3) mob
// points, gated by the i-frames. fallDistance accumulates the per-tick downward
// motion while airborne (Entity.updateFallState adds -motionY each falling tick)
// and is zeroed on landing, in water (handleWaterMovement), or on a ladder
// (EntitySpider.isOnLadder returns isCollidedHorizontally, so a wall-climbing spider
// never accrues fall). NOTE (v2 mapping): this engine's vel[1] is NOT Beta's
// motionY in the same units, and it is ZEROED the instant the mob lands
// (entity_try_move_axis axis 1). The faithful OBSERVABLE is the actual per-tick
// vertical DISTANCE the mob descended (pos_old[1] - pos[1] when positive), which
// the caller accumulates each airborne tick and feeds to mob_fall_damage() on the
// airborne->onGround landing edge. See the hazard hook in entity_monster.c.
#define MOB_FALL_SAFE_BLOCKS 3.0f // <= 3 blocks falls deal no damage (Beta minus 3)

// ceil(fall_distance - 3) clamped to >= 0, in MOB-scale half-heart points. Pure so
// tests/test_mob_hazards.c can pin the exact Beta CEIL + threshold.
static inline int mob_fall_damage(float fall_distance) {
	int i = (int)ceilf(fall_distance - MOB_FALL_SAFE_BLOCKS);
	return i > 0 ? i : 0;
}

// DROWNING. Entity.air starts at MOB_AIR_MAX (300) and drains 1/tick while the mob's
// HEAD/EYE cell is water and it can't breathe underwater (none of the 4 hostiles
// can). EntityLiving.onEntityUpdate decrements air each tick; on reaching -20 it
// resets air to 0 and deals 2 null-attacker damage -- so the FIRST drown hit lands when air reaches
// exactly -20 (after 300 down to 0 = 300 ticks, then 0 to -20 = 20 more => 320 ticks
// / 16s), dealing 2 (= 1 heart) and snapping air back to 0; thereafter another 2
// every 20 ticks (0 -> -20 again). Out of water, air refills instantly to MOB_AIR_MAX.
// The 2 is i-frame gated but the 20-tick cadence already exceeds the 10-tick window,
// so it always lands. The head cell uses the per-type eye height (biped 1.53 /
// spider 0.765) -- a mob with its body wet but head dry (1-deep water) does NOT drown.
#define MOB_AIR_MAX 300            // Entity.maxAir: full breath in ticks (15s)
#define MOB_DROWN_AIR_THRESHOLD -20 // air hits this -> a drown hit fires, air resets 0
#define MOB_DROWN_DAMAGE 2          // Beta drowning damage = 2 half-hearts (1 heart)
#define MOB_DROWN_INTERVAL 20       // after the first hit, 2 every 20 ticks (Beta)

// LAVA + FIRE CONTACT. Entity.onEntityUpdate: while the lava check reports contact
// it calls setOnFireFromLava, which deals 4 null-attacker damage and sets
// the fire field to 600. So lava contact deals 4 (= 2 hearts) PER APPLICATION and sets
// the fire field to 600 ticks (30s) -- which the EXISTING fire DoT (MOB_FIRE_DAMAGE
// 1 / MOB_FIRE_DAMAGE_INTERVAL 20) then burns down at 1/20t. We REUSE fire_time (do
// NOT invent a parallel burn). IMPORTANT: lava sets the fire field to 600, NOT 300 --
// the 600 fire assignment is literal in the decompiled b1.7.3 Entity.java (mc173 agrees:
// its fire time is 600). The later 15-second setFire form (300) is a LATER version; using 300
// here would be UNFAITHFUL. Sun-burn correctly stays MOB_FIRE_TICKS (300) -- that is
// the EntityZombie/Skeleton daylight path that sets fire to 300, a genuinely different value.
//
// I-FRAME CADENCE: Beta calls attackEntityFrom for 4 EVERY tick the mob is in lava, but
// hurtResistantTime (the heartsLife absorb gate, max 20, absorbs an equal hit while
// > 10) collapses that to 4 landing only ~every 10 ticks. v2 stores that effective
// upper half in hurt_resist_time and sends every damage source through the same
// strongest-hit/excess arbitration. fire_time is set EVERY contact tick regardless
// of whether damage was absorbed (Beta sets fire to 600 outside the gate).
#define MOB_LAVA_DAMAGE 4          // Beta lava per-application = 4 half-hearts (2 hearts)
#define MOB_LAVA_FIRE_TICKS 600    // setOnFireFromLava sets fire to 600 (30s @ 20 TPS)
// Entity.fireResistance default (Entity.java:52; EntityPlayer overrides it to
// 20 = PLAYER_FIRE_RESISTANCE). Entity.java:489-490 rests an unlit mob's fire
// at -MOB_FIRE_RESISTANCE and :493-496 quenches a wet burning mob to it.
#define MOB_FIRE_RESISTANCE 1
#define MOB_CACTUS_DAMAGE 1        // Beta BlockCactus.onEntityCollidedWithBlock attackEntityFrom for 1

// --- Beta 1.7.3 spider daytime passivity --------------------------------------
// EntitySpider.findPlayerToAttack acquires a target ONLY when getEntityBrightness <
// 0.5F, which maps EXACTLY to an effective light level <= 10. So a spider is hostile
// in the dark (night / caves) and PASSIVE in daylight -- it won't INITIATE a chase
// (it still retaliates if hit; that target latch is a later slice). This 10 is
// DISTINCT from the spawn gate's <= 7. eff_light = max(sky_light - sky_subtract,
// torch_light), as in the spawn gate. Beta brightness = (1-f)/(f*3+1) crosses 0.5
// between light 11 (0.437 < 0.5 -> hostile) and 12 (0.525 >= 0.5 -> passive).
#define SPIDER_HOSTILE_LIGHT_MAX 11

static inline bool spider_hostile_at_light(int eff_light) {
	return eff_light <= SPIDER_HOSTILE_LIGHT_MAX;
}

// --- Beta 1.7.3 spider LEAP + WALL-CLIMB + MELEE (EntitySpider) ----------------
// EntitySpider.attackEntity: when the horizontal distance is strictly between 2.0 and 6.0
// AND a 1-in-10 roll hits AND it is on the ground, the spider POUNCES: vertical 0.4 and a
// horizontal lunge toward the player = dir*0.5*0.8 + oldMotion*0.2. The band is
// stored squared (4.0 < dist^2 < 36.0; both bounds exclusive, as Beta's f).
// Byte-confirmed vs EntitySpider.java + the mc173 Rust reimpl tick_spider_attack.
#define SPIDER_LEAP_MIN_DIST_SQ 4.0f   // f > 2.0  -> dist^2 > 4.0
#define SPIDER_LEAP_MAX_DIST_SQ 36.0f  // f < 6.0  -> dist^2 < 36.0
#define SPIDER_LEAP_VEL_Y 0.4f         // vertical motion 0.4 on the pounce (Beta)
#define SPIDER_LEAP_CHANCE_DEN 10      // 1-in-10 roll per tick
#define SPIDER_LEAP_FWD (0.5f * 0.8f)  // 0.4: unit-toward-player forward coeff
#define SPIDER_LEAP_KEEP 0.2f          // 0.2: retained-old-motion coeff

// Wall-climb. EntitySpider.isOnLadder() returns isCollidedHorizontally, so any
// wall the spider pushes into is climbable: Beta's moveEntityWithHeading then
// clamps the fall (motionY >= -0.15) and the heading carries it UP the face. v2
// maps that observable ascend to a positive climb velocity OVERWRITTEN onto vel[1]
// while the spider is horizontally collided (see the hook in entity_monster.c;
// documented simplification -- behavior identical, math simpler).
#define SPIDER_CLIMB_VEL_Y 0.2f

// --- Beta 1.7.3 spider ladder DESCENT clamp (audit #17b) ----------------------
// EntitySpider.isOnLadder returns isCollidedHorizontally, after which
// EntityLiving.moveEntityWithHeading floors the DESCENT:
//     vertical motion below -0.15 is raised to -0.15,
// and zeroes fallDistance every ladder tick. So a spider pressed against (but not
// actively climbing UP) a wall does NOT free-fall: it slides down at most -0.15/tick
// and accrues no fall damage. v2 already models the ACTIVE-CLIMB case (collided +
// AI_CHASE -> vel[1] = +SPIDER_CLIMB_VEL_Y, a positive ascend). This clamp covers the
// COMPLEMENT: a horizontally-collided spider that is NOT in the climb branch (idle /
// wandering against a wall, or a chasing spider that took the leap/else path while
// still collided) -- its gravity-pulled vel[1] is floored at -SPIDER_FALL_CLAMP so it
// slides instead of dropping. Climb (+0.2 up) and clamp (floor at -0.15) are disjoint
// by construction: the clamp only runs when the climb branch did NOT (see the spider
// block in entity_monster.c). The clamp path also zeroes fall_distance, mirroring
// Beta's per-ladder-tick fall-distance reset, so a wall-sliding spider never bills
// bogus fall damage either. The -0.15 is single-sourced from the PeytonPlayz595
// decompiled EntityLiving.moveEntityWithHeading (mc173 leaves ladder physics a TODO).
#define SPIDER_FALL_CLAMP 0.15f // ladder descent floor: vertical >= -0.15 (Beta)

// Floor a collided spider's downward velocity at -SPIDER_FALL_CLAMP (Beta's
// ladder floor at -0.15). Pure so tests can pin the
// exact Beta clamp value; only DOWNWARD motion is floored (upward is untouched).
static inline float spider_clamp_fall(float vel_y) {
	return (vel_y < -SPIDER_FALL_CLAMP) ? -SPIDER_FALL_CLAMP : vel_y;
}

// Beta EntityChicken.onLivingUpdate's fall-speed damping [740c583]:
// while airborne and moving down, vertical motion is multiplied by 0.6 --
// the signature "chicken glide": a falling chicken visibly slows its own
// descent, every tick it is airborne and moving down, regardless of which
// movement branch (water/dry/over-water) that tick took. This damps FALL
// SPEED only; fall-damage IMMUNITY is the separate, already-implemented
// monster_def.fall_immune flag (mirroring EntityChicken's empty fall()
// override) -- this helper does not touch that. Pure so tests can pin the
// exact 0.6 multiplier and its on-ground/rising gates.
static inline float chicken_fall_damp(float vel_y, bool on_ground) {
	return (!on_ground && vel_y < 0.0f) ? vel_y * 0.6f : vel_y;
}

// Per-mob MELEE damage (PLAYER scale, 16/heart). EntitySpider has NO
// attackStrength override -> inherits EntityMob's base attackStrength of 2
// half-hearts = 1 heart = 16. EntityZombie overrides to 5 = 40 (ZOMBIE_ATTACK_
// DAMAGE above). The skeleton likewise has no override -> 16 (its real threat is
// the bow). So the spider deals 1.5 hearts LESS than a zombie per hit.
#define SPIDER_MELEE_DAMAGE (2 * (COMBAT_HEALTH_PER_HEART / 2)) // 16 = 1 heart

// True iff the player is inside the leap distance band (2.0 < dist < 6.0). The
// caller still gates on the 1-in-10 roll, onGround, and aggro.
static inline bool spider_leap_in_band(float dist_sq) {
	return dist_sq > SPIDER_LEAP_MIN_DIST_SQ
		&& dist_sq < SPIDER_LEAP_MAX_DIST_SQ;
}

// The attackEntity pounce on the mob's authoritative doubles (mob motion units
// S2) [EntitySpider.java:48-53, EntityWolf.java:297-302]:
//     the horizontal distance is a float square root of dx^2 + dz^2; each
//     horizontal motion becomes the unit direction x 0.5 x widened float 0.8 plus
//     the old motion x widened float 0.2 (X and Z alike); vertical motion
//     becomes widened float 0.4.
// dx/dz are (target - mob) on XZ; motion[] is read (old) AND written (new).
// Beta divides by zero for a target in the same column; the engine keeps the
// retained term only (finite, no NaN). entity_monster.c's leap and the tests
// share THIS function (the float spider_leap_impulse it replaces was dead).
static inline void mob_leap_impulse_beta(double dx, double dz, double motion[3]) {
	float horiz_dist = (float)sqrt(dx * dx + dz * dz);
	if(horiz_dist == 0.0F) {
		motion[0] = motion[0] * (double)SPIDER_LEAP_KEEP;
		motion[2] = motion[2] * (double)SPIDER_LEAP_KEEP;
	} else {
		motion[0] = dx / (double)horiz_dist * 0.5 * (double)0.8F
			+ motion[0] * (double)SPIDER_LEAP_KEEP;
		motion[2] = dz / (double)horiz_dist * 0.5 * (double)0.8F
			+ motion[2] * (double)SPIDER_LEAP_KEEP;
	}
	motion[1] = (double)SPIDER_LEAP_VEL_Y;
}

// Per-mob mob->player melee damage, keyed on the MONSTER_* id (Beta attackStrength
// * 8 on the player's 160-pt scale). Zombie 5->40; spider/skeleton (no override)
// 2->16; the creeper never melees (routed around the hurt path). An unknown id
// clamps to the EntityMob baseline (16), matching monster_def_get's crash-safety
// AND the assertions in tests/test_spider_combat.c (mob_melee_damage(-1)==16,
// mob_melee_damage(9999)==16).
static inline int mob_melee_damage(int monster_id) {
	switch(monster_id) {
	case MONSTER_ZOMBIE:   return ZOMBIE_ATTACK_DAMAGE; // 40
	case MONSTER_SPIDER:   return SPIDER_MELEE_DAMAGE;  // 16
	case MONSTER_SKELETON: return SPIDER_MELEE_DAMAGE;  // 16 (EntityMob default)
	// Beta EntitySlime.onCollideWithPlayer: damage == size, ONLY when size > 1
	// (size 1 is harmless -- the melee site skips the swing entirely on 0).
	// Player scale = Beta half-hearts * 8: medium 2*8, big 4*8.
	case MONSTER_SLIME_SMALL:  return 0;
	case MONSTER_SLIME_MEDIUM: return 16;
	case MONSTER_SLIME_BIG:    return 32;
	// Beta EntityPigZombie attackStrength 5 (same as the zombie's), routed
	// through the same melee path -- but ONLY while angry (the melee site
	// gates on anger_level). [740c583: EntityPigZombie ctor]
	case MONSTER_PIG_ZOMBIE: return ZOMBIE_ATTACK_DAMAGE; // 40
	// The ghast has NO contact melee at all (EntityFlying; its attack is the
	// fireball, in its own AI branch) -- 0 keeps it out of the melee site.
	case MONSTER_GHAST: return 0;
	// Wild wolf strength 2. Tamed strength 4 is selected dynamically at the
	// wolf attack site. [740c583: EntityWolf.attackEntity]
	case MONSTER_WOLF: return 16;
	// Beta EntityGiantZombie attackStrength 50, the plain EntityMob melee
	// path (no anger gate, no special-case). [740c583 EntityGiantZombie ctor]
	case MONSTER_GIANT: return GIANT_ATTACK_DAMAGE; // 400
	default:               return SPIDER_MELEE_DAMAGE;  // baseline / crash-safe
	}
}

// EntityWolf shares EntitySpider's exact 2<d<6, 1-in-10, grounded leap math.
// Keep aliases rather than a second set of constants that could drift.
// [740c583: EntityWolf.attackEntity]
#define WOLF_LEAP_MIN_DIST_SQ SPIDER_LEAP_MIN_DIST_SQ
#define WOLF_LEAP_MAX_DIST_SQ SPIDER_LEAP_MAX_DIST_SQ
#define WOLF_LEAP_VEL_Y SPIDER_LEAP_VEL_Y
#define WOLF_LEAP_CHANCE_DEN SPIDER_LEAP_CHANCE_DEN
static inline bool wolf_leap_in_band(float dist_sq) {
	return spider_leap_in_band(dist_sq);
}

// --- Beta 1.7.3 creeper proximity fuse (EntityCreeper) ------------------------
// EntityCreeper.attackEntity drives the SERVER-side swell + detonation (onUpdate's
// timeSinceIgnited is only the CLIENT swell visual). Hysteresis: ignite when idle
// and within 3.0 blocks (dist^2 < 9); once swelling, keep priming until the player
// is 7.0+ blocks away (dist^2 >= 49 deflates). The swell timer counts to fuseTime
// = 30 ticks (1.5s), then a power-3 createExplosion. A creeper deals ZERO contact
// melee -- it fully overrides attackEntity and never calls the inherited melee, so
// it is routed AROUND the player-hurt path. (Defuse bubble is 7, NOT 3 -- a value
// the research's adversarial pass corrected.)
#define CREEPER_FUSE_TICKS 30         // fuseTime: swell -> detonate (1.5s @ 20 TPS)
#define CREEPER_IGNITE_DIST_SQ 9.0f   // start swelling within 3.0 blocks (idle)
#define CREEPER_RELEASE_DIST_SQ 49.0f // stay primed until 7.0 blocks away (hysteresis)
#define CREEPER_EXPLOSION_POWER 3.0f  // createExplosion strength, unpowered
// Beta EntityCreeper.attackEntity: a powered creeper explodes at strength 6,
// an unpowered one at strength 3 [740c583] -- a creeper struck by lightning
// (EntityCreeper.onStruckByLightning sets the "powered"/charged DataWatcher byte)
// detonates at DOUBLE strength. See entity.h's monster.charged field.
#define CREEPER_EXPLOSION_POWER_CHARGED 6.0f

// audit #17f + parity fix 2026-08-31 (dossier finding "White swell-flash tint
// amplitude"): creeper white swell-flash amount for a given fuse phase (defined here
// because it needs CREEPER_FUSE_TICKS above; the white/red tint CONSTANTS + the
// fuse-independent helpers live up by MOB_HURT_RESIST_TICKS). `fuse` is the networked
// phase (-1 idle .. CREEPER_FUSE_TICKS); `td` is the inter-tick lerp (0..1). Returns
// the amount to wash toward white THIS frame: 0 when idle / in the "off" half of the
// pulse; on the "on" half, a CONTINUOUS ramp toward the CREEPER_FLASH_TINT_AMOUNT
// (0.2) ceiling as the raw fuse fraction f approaches 1 -- NOT a flat/fixed amount.
// [740c583: RenderCreeper.updateCreeperColorMultiplier] the flash ratio r is the
// lerped swell time; an even truncated r x 10 bucket gives no overlay, an odd one
// gives white at alpha trunc(r x 0.2 x 255) clamped to 0..255 -- r is the
// RAW, UNCLAMPED flash-time ratio (unlike creeper_flash_ramp below, which clamps for
// updateCreeperScale's f^4 term): it is never capped to 1.0 here, so the alpha can
// nudge slightly past the nominal 0.2 ceiling in the final tick before detonation
// before the [0,255]/[0,1] clamp on the OUTPUT catches it -- the clamp belongs on
// the final alpha, not on r (the ramp fed into the bucket test).
static inline float creeper_flash_amount(int fuse, float td) {
	if(fuse < 0)
		return 0.0f; // not primed -> no flash
	// f = Beta's ratio r, deliberately UNCLAMPED here -- updateCreeperColorMultiplier
	// never clamps r before the bucket test or the alpha multiply.
	float f = ((float)fuse + td) / (float)(CREEPER_FUSE_TICKS - 2);
	// Beta's bucket test: an even truncated r x 10 gives no overlay -- the "even bucket"
	// is the OFF phase, so the flash is ON when the bucket is ODD.
	if(((int)(f * 10.0f) & 1) == 0)
		return 0.0f;
	// Beta's alpha is r x 0.2 x 255, clamped [0,255] -- expressed here as a [0,1] fraction
	// (alpha/255) so the amount ramps continuously with f instead of snapping to a
	// flat CREEPER_FLASH_TINT_AMOUNT on the very first primed pulse.
	return tint_amount_clamp(f * CREEPER_FLASH_TINT_AMOUNT);
}

// --- AUDIT #18 ITEM 2: Beta 1.7.3 creeper swell INFLATION (RenderCreeper) -------
// RenderCreeper.updateCreeperScale (preRenderCallback), in words:
//   f is the lerped flash time, (timeSinceIgnited + partial tick) / 28,
//   clamped to 0..1 and then squared twice (f^4, CRITICAL); the X/Z scale is
//   (1 + f^4 x 0.4) x wobble and the Y scale is (1 + f^4 x 0.1) / wobble,
//   applied as the model scale with X and Z sharing the first value
//   (wobble is the small high-frequency term described below).
// So the model bulges WIDER (max 1.4x XZ at f=1) than TALLER (max 1.1x Y), and -- because
// the flash is raised to the 4TH POWER before the 0.4/0.1 coefficients -- it stays ~normal
// size for most of the fuse and inflates HARD only in the final ~15% (f=0.5 -> ^4=0.0625 ->
// 1.025x; f=0.84 -> 1.2x; f=1.0 -> 1.4x). This is the SAME raw flash f the white tint uses
// (creeper_flash_amount's f-ramp), but POW'd -- the tint uses the RAW f, the scale uses f^4
// (do NOT confuse them: raw f would balloon the creeper far too early/linearly). The high-
// frequency wobble (1 + sin(f*100)*f*0.01, <=+-1%) is a near-imperceptible jitter -- DROPPED
// here (documented), it adds nothing on a Wii TV and would force a per-frame sin() in the GX
// path. Returns 1.0 (no inflation) while idle (fuse < 0) so a non-primed creeper is unscaled.
#define CREEPER_SWELL_SCALE_XZ_MAX 0.4f // (1 + f^4 * 0.4): max +40% width at f=1 (Beta)
#define CREEPER_SWELL_SCALE_Y_MAX  0.1f // (1 + f^4 * 0.1): max +10% height at f=1 (Beta)

// The raw clamped flash f = (fuse + td)/(fuseTime-2), 0..1 (the SAME ramp the white tint
// reads, before the on/off bucketing). Factored out so the scale (f^4) and the tint (raw f)
// share one definition of f. Idle (fuse < 0) -> 0.
static inline float creeper_flash_ramp(int fuse, float td) {
	if(fuse < 0)
		return 0.0f;
	float f = ((float)fuse + td) / (float)(CREEPER_FUSE_TICKS - 2);
	if(f < 0.0f)
		f = 0.0f;
	if(f > 1.0f)
		f = 1.0f;
	return f;
}

// X/Z model scale this frame = 1 + flash^4 * 0.4 (RenderCreeper, wobble dropped). 1.0 idle.
static inline float creeper_swell_scale_xz(int fuse, float td) {
	float f = creeper_flash_ramp(fuse, td);
	float f4 = f * f;
	f4 *= f4; // f^4 -- Beta's two successive in-place squarings
	return 1.0f + f4 * CREEPER_SWELL_SCALE_XZ_MAX;
}

// Y model scale this frame = 1 + flash^4 * 0.1 (RenderCreeper, wobble dropped). 1.0 idle.
static inline float creeper_swell_scale_y(int fuse, float td) {
	float f = creeper_flash_ramp(fuse, td);
	float f4 = f * f;
	f4 *= f4;
	return 1.0f + f4 * CREEPER_SWELL_SCALE_Y_MAX;
}

// Creeper proximity-fuse step. `was_primed` is Beta's PERSISTED creeperState
// hysteresis selector (a positive state in EntityCreeper.attackEntity), NOT a derived
// "does the ramp still hold a nonzero value" test -- the caller must latch this to
// false on every failing tick and back to true only on a successful priming tick
// (matching setCreeperState with -1 / with 1), independently of how much fuse
// ramp remains banked. A creeper that deflated by even one tick must re-pass the
// tight 3-block test, no matter how much unspent ramp it's still carrying -- see
// entity_monster.c's creeper_state field for the parity-fix rationale (dossier
// finding "Proximity-fuse hysteresis re-arm threshold after a partial deflate").
// Returns true iff it should be primed THIS tick (the caller advances the fuse
// toward CREEPER_FUSE_TICKS, or deflates toward idle when false). Encodes the
// 3-block ignite / 7-block release hysteresis + loses priming past follow range.
// [740c583: EntityCreeper.attackEntity -- it primes when the state is not positive
// and the distance is under 3, or the state is positive and the distance under 7]
static inline bool creeper_should_prime(float dist_sq, bool was_primed,
										float follow_range_sq) {
	if(dist_sq >= follow_range_sq)
		return false; // target lost -> deflate
	return was_primed ? (dist_sq < CREEPER_RELEASE_DIST_SQ)
					   : (dist_sq < CREEPER_IGNITE_DIST_SQ);
}

// 3D reach gate (center-to-center). Parity fix 2026-06-19: Beta EntityMob.attackEntity
// gates on getDistanceToEntity < 2.0, the FULL 3D distance (Entity.getDistanceToEntity
// includes the Y term); the old form dropped dy and admitted hits Beta rejects across a
// vertical step. The vertical AABB overlap (melee_y_overlap) is a SEPARATE conjunct --
// Beta requires BOTH -- so this is not redundant: a player can box-overlap a mob yet be
// > 2.0 away in 3D (e.g. 1.9 horizontal + 0.9 vertical = 3D^2 4.42 >= 4.0 -> no hit).
// The CALLER supplies dy on CaveWii's feet-to-feet basis (a documented deviation from
// Beta's eye-to-feet posY -- see the Y-BASIS DEVIATION note in entity_monster.c), so the
// flat-ground reach stays the established 2.0 blocks rather than Beta's ~1.17.
static inline bool melee_in_reach_sq(float dx, float dy, float dz) {
	return dx * dx + dy * dy + dz * dz < MOB_ATTACK_REACH_SQ;
}

// Vertical AABB overlap between the player box [p_feet, p_feet+1.8] and the mob
// box [m_feet, m_feet+m_height]. Both feet are world Y. m_height is the mob's REAL
// body height (mob_body_height(id): 0.9 for the spider, 1.8 for the biped box) so the
// melee box matches the per-id collision/raycast box -- a player whose feet sit between
// 0.9 and 1.7 above a SPIDER's feet (e.g. standing on a step the squat spider passed
// under) no longer registers a phantom overlap against the 0.8 blocks of empty air the
// old fixed-1.7 height assumed above the real 0.9 body. Passing m_height explicitly
// keeps this pure + unit-testable for both the biped (1.7) and spider (0.9) cases.
static inline bool melee_y_overlap(float p_feet, float m_feet, float m_height) {
	float p_top = p_feet + PLAYER_BODY_HEIGHT;
	float m_top = m_feet + m_height;
	return m_top > p_feet && m_feet < p_top;
}

// Horizontal knockback unit direction, pointing from the attacker to the target
// (i.e. the player is pushed AWAY from the mob). dx/dz are (player - attacker).
// On a degenerate perfect overlap, falls back to +X so the result is finite
// (Beta nudges by a tiny random epsilon; +X is a deterministic stand-in).
static inline void melee_knockback_dir(float dx, float dz, float* out_x,
									   float* out_z) {
	float len2 = dx * dx + dz * dz;
	if(len2 < 1.0e-4f) {
		*out_x = 1.0f;
		*out_z = 0.0f;
		return;
	}
	float inv = 1.0f / sqrtf(len2);
	*out_x = dx * inv;
	*out_z = dz * inv;
}

// Beta EntityLiving.knockBack [EntityLiving.java:392-405] is composed AT THE HIT
// on the mob's authoritative doubles by mob_motion_knockback (mob_motion.h; mob
// motion units S2): halve all of motion, push 0.4 away from the attacker,
// add widened float 0.4 vertically, capped at that same value. The float halve-then-add
// helpers and the x25 GRAVITY-2.0 fall bridge that stood in for it (a deferred,
// engine-units compose) are retired with the engine integrator.

// --- Beta 1.7.3 idle head-turn locomotion (EntityLiving.updatePlayerActionState) --
// EntityCreature's updatePlayerActionState falls back to
// the parent implementation (its own base class, EntityLiving) every
// tick it has NO active path -- which is every tick a mob is not mid-chase
// with a live A* route, i.e. most of a wandering/idle mob's life. That base
// method is NOT a no-op: moveForward/moveStrafing are zeroed (no walking),
// but the mob still TURNS -- a 2% roll picks the nearby player as a look
// target and smoothly faces them, or (absent a target) a 5% roll re-picks a
// random yaw drift applied every tick. mob_path_follow_tail (entity_monster.c)
// is the shared tail for both the chase and wander branches and is where this
// gets consumed. [740c583: EntityLiving.updatePlayerActionState]
#define MOB_IDLE_LOOK_RANGE_SQ 64.0f // Beta 8-block look radius -> 8^2 (closest-player probe)
#define MOB_IDLE_ACQUIRE_CHANCE 0.02f  // Beta 2% uniform-float roll
#define MOB_IDLE_YAW_REROLL_CHANCE 0.05f // Beta 5% uniform-float roll
// Beta's turn/drift magnitudes are DEGREES; this engine's yaw fields (head_yaw,
// atan2f(dx,dz) convention) are RADIANS, so the conversion is baked into these
// two named constants rather than repeated at each call site.
#ifndef M_PI
#define M_PI 3.14159265358979323846 // strict C99 (devkitPPC -pedantic) hides it
#endif
#define MOB_IDLE_YAW_TURN_RATE (10.0f * (float)M_PI / 180.0f) // Beta faceEntity 10-degree step
#define MOB_IDLE_RANDOM_YAW_RANGE (20.0f * (float)M_PI / 180.0f) // Beta (uniform - 0.5) x 20 deg

// Beta EntityLiving.updateRotation: step `current` toward `target` by up to
// `max_rate` per call, taking the short way around the circle (both angles
// and the rate are RADIANS here; Beta's own updateRotation is degrees).
// [740c583: EntityLiving.updateRotation]
static inline float mob_turn_toward(float current, float target,
									float max_rate) {
	float delta = target - current;
	while(delta < -(float)M_PI)
		delta += 2.0f * (float)M_PI;
	while(delta >= (float)M_PI)
		delta -= 2.0f * (float)M_PI;
	if(delta > max_rate)
		delta = max_rate;
	if(delta < -max_rate)
		delta = -max_rate;
	return current + delta;
}

// --- Beta 1.7.3 skeleton ranged attack + arrow (EntitySkeleton + EntityArrow) --
// EntitySkeleton.attackEntity fires an EntityArrow when the player is within ~10
// blocks (dist^2 < 100) and its cooldown is ready (~30 ticks). The arrow flies with
// gravity 0.03/tick and 0.99 air drag and deals a flat 4 half-hearts. The arrow's
// SWEEP (entity-vs-entity), STICK (in-ground on a block hit), PICKUP (player-fired
// arrows), and DESPAWN split (in-air vs in-ground) are implemented below (audit
// #17c/#17d) -- the old "vanish on impact" / "hits only the player" simplifications
// are gone. ARROW_DAMAGE hits the PLAYER, so it is on the player's 160-pt health
// scale (COMBAT_HEALTH_PER_HEART); a hit on a MOB uses the raw Beta value on the
// mob's 20-pt scale (ARROW_DAMAGE_BOW_BASE below) -- the two scales never cross.
#define SKELETON_SHOOT_RANGE_SQ 100.0f   // fire within 10 blocks (dist^2 < 100)
#define SKELETON_ARROW_COOLDOWN_TICKS 30 // ~1.5s between shots
// AUDIT #18 ITEM 1 (RE-VERIFIED, no change): the b1.7.3 EntitySkeleton.attackEntity
// launch spawns an arrow from the skeleton and heads it at speed 0.6, spread 12 -- speed
// 0.6, spread 12. The widely-cited 1.6 is from a POST-Beta 4-arg EntityArrow
// (world, shooter, target, speed, spread) constructor (wither-skeleton/mc-dev era) that
// does NOT exist in b1.7.3 -- changing 0.6 to 1.6 would be UNFAITHFUL. Likewise the
// on-hit damage is a FLAT 4 credited to the owner (no floored velocity x damage
// scaling, no crit -- those are post-Beta), so ARROW_DAMAGE 32 (player-scale 4*8) and
// the mob-scale flat 4 (ARROW_DAMAGE_BOW_BASE) are both correct. Confirmed verbatim vs
// the PeytonPlayz595/Beta-1.7.3 decompile. 0.6 is the FAITHFUL value; do not "correct" it.
#define ARROW_SPEED 0.6f                 // setArrowHeading 4th arg (b1.7.3 skeleton)
// Beta setArrowHeading 5th arg (deviation) + its internal scale: each axis of
// the NORMALIZED heading gets nextGaussian() * 0.0075 * deviation added BEFORE
// the speed scale. Skeletons fire at deviation 12 (sigma 0.09/axis) -- the
// signature Beta skeleton inaccuracy. (The player's instant bow uses 1.0 in
// Beta; v2's player shot keeps its existing no-spread instant-fire carve-out.)
#define SKELETON_ARROW_DEVIATION 12.0f
#define ARROW_SPREAD_PER_DEVIATION 0.0075f

// Beta EntitySkeleton.attackEntity arrow LAUNCH GEOMETRY (parity fix 2026-06-19; target
// corrected 2026-06-19; launch height corrected to the decompile 2026-11-01). The arrow spawns
// at the skeleton's feet + eye height 1.53 - 0.1 (EntityArrow ctor) + 1.4 (`var7.posY +=
// (double)1.4F` [EntitySkeleton.java:40], NOT a 1.0 lift) = feet + 2.83, and aims at the
// target's posY + eye height - 0.2 = player.posY + 0.12 - 0.2 = player.posY - 0.08. The player's
// posY IS the eye/camera (EntityPlayer.yOffset 1.62), which is exactly CaveWii's
// s->player.y -- so the target is s->player.y - SKELETON_ARROW_TARGET_DY (the head), NOT
// the player FEET. (The first parity pass mis-derived this as feet-0.08 by treating Beta's
// player.posY as the feet; that buried the shot 0.08 below the feet -- ddy -2.91 instead of
// the correct -1.29 on flat ground. Flat-ground ddy = player_eye(1.62) - 0.08 - 2.83 =
// -1.29.) The pre-fix engine launched at feet+1.55 and aimed at the eye (ddy +0.07 --
// slightly high, no lift at all).
#define SKELETON_ARROW_LAUNCH_DY 2.83f   // 1.53 eye - 0.1 ctor + 1.4 (EntitySkeleton.java:40)
#define SKELETON_ARROW_TARGET_DY 0.08f   // aim at the player's eye-level posY minus 0.08 (0.2 minus eye height 0.12)

// Beta EntityArrow.onUpdate widens each candidate target's bounding box by 0.3 per axis
// (the box grown by 0.3 on every axis) BEFORE the swept-segment intercept -- a wider hit
// volume than the raw body box. Parity fix 2026-06-19: the old arrow path tested the raw
// box, missing grazing shots Beta counts as hits.
#define ARROW_HIT_EXPAND 0.3f

// Apply Beta's arrow spread to a normalized heading, given three standard
// normals (passed in so the pure math is testable headless). Caller scales by
// arrow speed AFTER, exactly like setArrowHeading.
static inline void arrow_apply_spread(float dir[3], float g0, float g1,
									  float g2, float deviation) {
	dir[0] += g0 * ARROW_SPREAD_PER_DEVIATION * deviation;
	dir[1] += g1 * ARROW_SPREAD_PER_DEVIATION * deviation;
	dir[2] += g2 * ARROW_SPREAD_PER_DEVIATION * deviation;
}
#define ARROW_GRAVITY 0.03f              // per-tick downward accel (NOT the 0.05 bow path)
#define ARROW_AIR_DRAG 0.99f             // per-tick velocity multiplier
// Beta EntityArrow.onUpdate:200-218 selects a 0.8 per-tick multiplier while the
// arrow is in water (`var20 = 0.8F`), keeping the same 0.03 gravity.
#define ARROW_WATER_DRAG 0.8f            // per-tick velocity multiplier in water
#define ARROW_DAMAGE (4 * (COMBAT_HEALTH_PER_HEART / 2)) // 32 = 2 hearts (PLAYER scale)
// AUDIT #18 ITEM 6 (faithfulness DOC fix): this is a NON-BETA, Wii-FEEL value, NOT a
// Beta-derived constant. b1.7.3's EntityArrow.onUpdate hit branch does ONLY
// a 4-damage hit credited to the owner, the arrow dying if it lands -- there is NO
// addVelocity/knockbackStrength impulse at all (the arrow knockback + Punch enchant are
// POST-Beta). So a Beta arrow's only "knockback" is whatever generic recoil
// attackEntityFrom applies (the same ~0.4 melee recoil along the hit direction). v2
// keeps a DISTINCT, lighter 0.18 shove on a MOB struck by an arrow purely as a feel
// choice (a softer nudge than a melee KNOCKBACK_H 0.4 so an arrow doesn't ragdoll a
// mob), and the PLAYER-hit arrow path deliberately routes through the generic
// KNOCKBACK_H 0.4 recoil (server_local_damage_player) -- matching Beta's "arrow inherits
// the generic damage recoil" for the player while giving mobs a gentler push. The value
// is retained (it IS used by arrow_damage_mob) but is now documented as a v2 feel value,
// per CLAUDE.md (do not present a v2 invention as a Beta value).
#define ARROW_KNOCKBACK 0.18f            // v2 Wii-feel mob arrow-shove (NOT Beta; see above)

// --- Skeleton keep-distance + circle-strafe (audit #17e) ----------------------
// FAITHFULNESS CLASSIFICATION (split, per the research -- state it plainly, with the
// magnitude caveat the adversarial pass surfaced):
//   * The SIDEWAYS STRAFE is FAITHFUL Beta 1.7.3 in KIND. EntityCreature.updatePlayer-
//     ActionState, once it has attacked and holds a target, sets the strafe input to
//     -sin(a) x forward and the forward input to cos(a) x forward, where a is
//     the angle between the held A* PATH heading and the straight-to-player heading --
//     i.e. while facing the player it re-projects the SAME moveSpeed walk into a
//     forward + sideways pair. EntitySkeleton.attackEntity sets its attacked flag
//     every tick the player is within 10 blocks, so this runs throughout the shooting
//     band, and v2's prior skeleton reused the SHARED beeline chase and was MISSING it
//     entirely -- so restoring a tangential term is a faithfulness RESTORATION, not an
//     invention. (Confirmed verbatim vs PeytonPlayz595/Beta-1.7.3; mc173's should_strafe
//     corroborates the re-projection -- TREAT mc173 AS SECONDARY/UNVERIFIED-here; the
//     b1.7.3 decompile is the authoritative source.)
//     MAGNITUDE CAVEAT (do NOT overstate): Beta's strafe is a BYPRODUCT of facing the
//     player while A*-pathing, so its sideways magnitude is PATH-CURVATURE dependent.
//     a = path-vs-player angle: on OPEN ground the A* path runs ~straight at the
//     player so a ~ 0 -> cos ~ 1 / sin ~ 0 and the Beta skeleton walks ESSENTIALLY
//     STRAIGHT IN (the strafe only becomes large when the path curves around obstacles).
//     v2's IN-BAND term is a CONSTANT pure-tangent orbit (radial = 0), i.e. STRONGER and
//     more constant circling than Beta does on open terrain. So the in-band pure orbit
//     is a faithful-IN-KIND restoration that is DELIBERATELY AMPLIFIED for the bow-only
//     standoff -- the kind (strafe) is Beta, the constant magnitude is a v2 design choice.
//     SIMPLIFICATION: v2 also omits Beta's per-tick yaw-turn clamp (a is clamped to
//     [-30,+30] deg/tick in the path-follow block before the strafe re-projection); v2
//     snaps to the full blended heading each tick with no turn-rate limit, so the orbit
//     can pivot faster than a Beta mob's lagged heading. Not a faithfulness bug (the
//     strafe block itself re-faces the player), but it is why the orbit could read as
//     "sharp/twitchy" on a future Dolphin feel-test -- flagged, not byte-faithful turn.
//   * The explicit BACK-OFF below a min standoff is a DOCUMENTED Wii-CONTROLLER
//     PLAYABILITY deviation (NOT Beta -- b1.7.3 has NO retreat/fleeingTick/min-distance;
//     its strafe only deflects the walk and a skeleton genuinely can drift into melee
//     and get cornered). Justification (CLAUDE.md's sanctioned carve-out): the skeleton
//     is BOW-ONLY (the melee branch in entity_monster.c structurally excludes
//     MONSTER_SKELETON), so if it walks into <2-block melee range it can neither melee
//     nor cleanly shoot -- it just dies. Steering it OUT keeps it a RANGED threat
//     distinct from the melee mobs and gives the IR-pointer player a fairer window.
//     IMPORTANT LIMITATION (best-effort, NOT a hard standoff): the back-off is PURE
//     RADIAL AWAY but still moves at the Beta-parity move_speed (0.18/tick under the cap)
//     -- SLOWER than a walking/sprinting player -- so against forward pressure the
//     standoff only SLOWS the player's closing; it does NOT guarantee a minimum range.
//     A player who simply walks into the skeleton can still pin it point-blank (where it
//     keeps firing eye-to-eye). We deliberately keep move_speed at Beta parity rather
//     than speed the skeleton up to "win" the spacing; the band buys a window, not a wall.
//     HAZARD TRADEOFF: backing AWAY is the FIRST mob steer in this engine that drives a
//     mob backward, away from the player. The engine has NO edge/cliff avoidance and
//     treats lava as contact-damage only, so a cornered skeleton could back-pedal off a
//     ledge or into lava it would never have walked toward. The call site GATES the
//     away-steer on a cheap one-cell behind-the-mob solidity + non-lava probe (suppress
//     the back-off -> orbit in place when the cell behind is lava or air-over-a-drop);
//     see the skeleton branch in entity_monster.c. Tunable; pending a real-Wii feel-test.
//     See docs/wii_controller_playability.md #15. NOT attributed to Beta or mc173.
//
// The standoff BAND nests INSIDE the 10-block fire range (SKELETON_SHOOT_RANGE_SQ=100)
// so the skeleton always strafes where it can still shoot, and OUTSIDE melee reach
// (MOB_ATTACK_REACH_SQ=4, 2 blocks) so it never sits in the (excluded) melee zone.
// MIN < MAX gives a hysteresis band: inside it the steer is PURE TANGENT (orbit), so a
// skeleton settled in-band doesn't radially oscillate between back-off and close-in.
// STEADY STATE: a discrete tangent step always lands slightly OUTSIDE the circle it was
// tangent to, so the orbit drifts OUTWARD and settles riding the MAX boundary (one
// toward-correction per lap), NOT mid-band. MAX is kept comfortably inside the 10-block
// fire range (MAX 7 -> dist^2 49 << 100) so the outward-riding orbit still shoots with
// margin instead of sitting at the very edge of firing range.
#define SKELETON_STANDOFF_MIN 4.5f                                  // back off if closer
#define SKELETON_STANDOFF_MIN_SQ (SKELETON_STANDOFF_MIN * SKELETON_STANDOFF_MIN) // 20.25
#define SKELETON_STANDOFF_MAX 7.0f                                  // close in if farther
#define SKELETON_STANDOFF_MAX_SQ (SKELETON_STANDOFF_MAX * SKELETON_STANDOFF_MAX) // 49.0

// --- Beta 1.7.3 PLAYER BOW (ItemBow) + arrow stick/pickup/despawn (audit #17c/#17d) --
// IMPORTANT VERSION NOTE (the research correction): Beta 1.7.3's bow is the OLD
// INSTANT-FIRE bow. ItemBow.onItemRightClick consumes ONE arrow and spawns the arrow
// IMMEDIATELY -- there is NO draw/charge mechanic. onPlayerStoppedUsing,
// getMaxItemUseDuration, the (f*f+f*2)/3 charge curve, the 0.1 min-charge gate,
// charge-scaled velocity, and velocity-scaled ceil(velocity*base) damage with crits
// were ALL added in Beta 1.8 (the Adventure Update) / standardized in Release 1.0.
// Implementing the charge bow would be UNFAITHFUL to b1.7.3 SIMULATION, so we do NOT.
// (Source: PeytonPlayz595/Beta-1.7.3 ItemBow.java -- only onItemRightClick exists; MC
// Wiki Bow history: "Beta 1.8 Pre-release: ... they now need to be charged to fire.")
//
// LAUNCH SPEED: EntityArrow(World, EntityLiving) sets the player-bow arrow heading via
// heading speed 1.5 and spread 1.0 -- a FIXED 1.5 speed multiplier (vs the
// skeleton's 0.6). So a bow arrow flies ~2.5x faster and flatter than a skeleton's,
// but on-hit DAMAGE is identical: a HARDCODED 4 (credited to the owner) for
// every arrow, no velocity scaling, no crit, no random in b1.7.3.
#define ARROW_SPEED_BOW 1.5f // EntityArrow(World,EntityLiving) setArrowHeading speed mult

// On-hit damage in MOB-scale half-hearts (the arrow hits a 20-pt mob). Beta's flat 4
// credited to the owner; used DIRECTLY on the mob (NOT *8 -- that *8 is only
// for the PLAYER-scale ARROW_DAMAGE when a SKELETON arrow hits the 160-pt player). The
// task asked for a velocity-scaled ceil(speed*base); that is the POST-1.8 bow and is
// UNFAITHFUL to 1.7.3, so the faithful value is the flat 4. We expose the helper
// arrow_hit_damage() below so a deliberate (documented) non-faithful velocity scaling
// could be toggled in future without scattering magic numbers -- but the DEFAULT and
// the only call site use the faithful flat base.
#define ARROW_DAMAGE_BOW_BASE 4 // Beta flat arrow damage on a mob's 20-pt scale (1.7.3)

// --- Wii-CONTROLLER PLAYABILITY: bow control scheme + aim-assist (documented carve-out) --
// See CLAUDE.md "Beta 1.7.3 parity vs. Wii-controller playability". The SIMULATION
// (instant fire, flat 4 damage, 1.5 launch speed, gravity/drag flight) stays byte-
// faithful; the CONTROL SCHEME is the playability layer. b1.7.3 has no hold-to-draw, so
// a faithful INSTANT-FIRE bow maps cleanly to a Wiimote: IR pointer = aim (drives the
// player look rx/ry that the camera ray already follows), one button press (IB_ACTION2,
// "use") = consume one arrow + fire instantly. We deliberately do NOT add a draw/charge
// timer (that would import a 1.8 mechanic and is unfaithful). To keep a single button
// (IB_ACTION2 is shared with block-place) honest, the client fires the bow only when NO
// block is targeted by the crosshair (open-air aim), matching Beta's bow-into-air use.
//
// AIM-ASSIST CONE: an IR-pointer shot is twitchier than a mouse, so on release the
// SERVER biases the launch heading toward the nearest mob whose direction is within a
// small angular cone of the player's look ray (and within bow range). This is an
// EXPLICIT, documented, tunable playability aid -- NOT Beta behavior -- gated to player
// shots only. A wider cone = more forgiving; 0 disables it. Kept as a half-angle cosine
// so the per-tick test is a single dot-product compare.
#define ARROW_AIM_ASSIST_CONE_COS 0.985f // cos(~10 deg): mobs within ~10 deg of the look ray
#define ARROW_AIM_ASSIST_RANGE 24.0f     // only assist toward mobs within this many blocks
#define ARROW_AIM_ASSIST_RANGE_SQ (ARROW_AIM_ASSIST_RANGE * ARROW_AIM_ASSIST_RANGE)

// --- Arrow STICK + PICKUP + DESPAWN split (audit #17d) -------------------------
// Beta: on a BLOCK hit the arrow sets inGround, sets arrowShake to 7, and despawns after
// ticksInGround reaches 1200 (60s @ 20 TPS). An IN-AIR arrow has no age-based despawn;
// Entity.onEntityUpdate instead kills it below Y -64. The
// faithful in-GROUND path is implemented: stick + ARROW_STUCK_DESPAWN_TICKS (1200).
// PICKUP: Beta onCollideWithPlayer requires inGround and doesArrowBelongToPlayer (owner
// is a player) and the arrow shake is 0 or less -> +1 arrow, then dies. v2: from_player ==
// doesArrowBelongToPlayer (CONFIRMED), so player-fired stuck arrows are pickable, a
// skeleton's are not (it just despawns at 1200).
#define ARROW_STUCK_DESPAWN_TICKS 1200 // Beta ticksInGround at 1200 -> despawn (60s)
#define ARROW_SHAKE_TICKS 7            // Beta arrowShake 7 on stick; must hit 0 before pickup

// --- Owner-grace exclusion for the entity sweep (audit #17c) -------------------
// Beta EntityArrow.onUpdate gathers all collidable entities along its move segment and
// keeps the closest hit, with ONE exclusion: the shooter (owner) is immune ONLY while
// ticksInAir < 5 (any other candidate, or the owner from tick 5). Every OTHER living entity is a
// valid target from tick 0 -- there is NO friendly-fire exemption, so a skeleton's
// arrow CAN hit another mob (infighting). from_player is NOT a targeting filter; it
// only selects which HEALTH SCALE the hit applies (player 160-pt vs mob 20-pt). The
// engine's age field starts at 0 and is ++'d at the TOP of the tick, so by the time the
// sweep runs `age` is already 1 on the first moved tick -- the grace compares age (post-
// increment) against ARROW_OWNER_GRACE_TICKS.
#define ARROW_OWNER_GRACE_TICKS 5 // owner immune only while age < 5 (Beta ticksInAir<5)

// --- Per-tick MOVE SUBSTEPPING to prevent thin-wall tunneling (audit #17c fix) -
// The engine's entity_try_move does an ENDPOINT-ONLY collision test per axis: it
// only runs the swept binary-search when the END-of-tick AABB overlaps a block. If
// BOTH the start and end boxes are clear but a thin (e.g. 1-block) wall sits strictly
// BETWEEN them, the move passes THROUGH it. At the skeleton's ARROW_SPEED (0.6) the
// per-tick travel is short enough that the start/end boxes always straddle a full
// 1-block wall (no tunneling), but the PLAYER bow's ARROW_SPEED_BOW (1.5) moves far
// enough per tick to skip a thin wall. Faithful Beta arrows do NOT tunnel (Beta moves
// the arrow in ONE swept segment with full ray/voxel collision). v2's fix: split the
// per-tick travel into sub-steps no longer than ARROW_MAX_SUBSTEP_DIST blocks each and
// run the move + entity sweep per sub-step, so every sub-move's start/end boxes overlap
// any wall in between. 0.5 (< the 1-block wall thickness and < the arrow box, so a
// wall can never fit fully between two consecutive sub-step boxes) bounds bow travel
// (1.5) to 3 sub-steps/tick -- cheap, and the skeleton arrow (0.6) needs 2.
#define ARROW_MAX_SUBSTEP_DIST 0.5f

// Number of equal sub-steps to split a per-tick arrow displacement of length
// `travel_len` into, so no single sub-step exceeds ARROW_MAX_SUBSTEP_DIST. Always
// >= 1. Pure so tests/test_bow_arrow.c can pin the anti-tunneling guarantee (a 1.5
// bow tick -> 3 steps of 0.5; a 0.6 skeleton tick -> 2 steps of 0.3; a tiny move -> 1).
static inline int arrow_move_substeps(float travel_len) {
	int n = (int)ceilf(travel_len / ARROW_MAX_SUBSTEP_DIST);
	return n < 1 ? 1 : n;
}

// Pure: should this arrow be allowed to hit candidate `target_id` this tick? Excludes
// the owner during the first ARROW_OWNER_GRACE_TICKS ticks; everyone else is hittable
// from tick 0. (The id-0 / null-entity skip is the caller's job.) Pinned by
// tests/test_bow_arrow.c so the grace window can't silently drift.
static inline bool arrow_should_hit(uint32_t owner_id, uint32_t target_id, int age) {
	if(target_id != owner_id)
		return true;            // any non-owner is a valid target from tick 0
	return age >= ARROW_OWNER_GRACE_TICKS; // owner only after the grace window
}

/* S1-a2: the same Beta rule for a LOCAL PLAYER BODY candidate, decided by
 * ACTOR IDENTITY instead of entity id. Beta compares object references --
 * a candidate other than the shooter, or any candidate from tick 5 on [740c583
 * EntityArrow.onUpdate] -- and every local player body in this engine shares
 * the one client-side local-player entity id (SERVER_LOCAL_PLAYER_ENTITY_ID),
 * so the id compare above cannot tell the host apart from a split guest: a
 * GUEST-fired arrow was treated as the HOST's own and absorbed by the host
 * owner grace, while still being able to damage the host afterwards.
 * `target_is_shooter` is the caller's identity compare
 * (actor_ident_equal / actor_ident_is_host), which makes the grace apply to
 * the shooter ONLY -- so a guest arrow can hit the host from tick 0, which is
 * Beta (players damage players) and is what the S1-a2 brief pins. Pure, so
 * tests can pin the window; same constant, no behaviour change for a
 * host-fired arrow (target_is_shooter is then exactly the old compare). */
static inline bool arrow_should_hit_actor(bool target_is_shooter, int age) {
	if(!target_is_shooter)
		return true;
	return age >= ARROW_OWNER_GRACE_TICKS;
}

/* The owner id every LOCAL PLAYER BODY fires with. Both local bodies share it
 * (the client-side local-player entity id is the same for host and guest),
 * which is exactly why the shooter also carries an actor identity. Every
 * non-player shooter has a NONZERO owner id: a skeleton passes its own entity
 * id (entity_monster.c's server_local_spawn_arrow call) and a dispenser passes
 * DISPENSER_PROJECTILE_OWNER_SENTINEL (dispenser_logic.h, whose own comment
 * spells out the hazard: "passing 0 here would wrongly grant the PLAYER a
 * 5-tick immunity window against a dispensed projectile"). */
#define PROJECTILE_OWNER_LOCAL_PLAYER 0u

/* R-S1a2 F1: is the swept local player body THE shooter of this projectile?
 * Beta's exclusion operand is the shooting-entity REFERENCE, never "a player":
 * EntityArrow.onUpdate skips a candidate only when it IS the shooting entity
 * and fewer than 5 air ticks have passed [740c583; cross-checked in ../McBetaCpp
 * src/net/minecraft/src/EntityArrow.cpp:161, the same owner-and-under-5
 * condition], and EntityThrowable.onUpdate applies the identical rule
 * to its thrower for the egg/snowball [cross-checked in ../mc173
 * mc173/src/entity/tick.rs:381-399, where owner_id is only excluded while
 * state_time < 5, for arrow, snowball and egg alike].
 *
 * So BOTH legs are required, and the identity compare alone is not enough:
 * a mob/dispenser projectile leaves `shooter_actor` at the zero (= HOST)
 * default, so `actor_ident_is_host(shooter_actor)` is TRUE for it and would
 * grace the host against an arrow it never fired -- a point-blank skeleton
 * became harmless for five ticks (~3 blocks at ARROW_SPEED 0.6). Requiring
 * owner_id == PROJECTILE_OWNER_LOCAL_PLAYER first restores the Beta rule
 * exactly: the grace belongs to the body that actually fired, and to nobody
 * else. Pure, so tests/test_bow_arrow.c can pin both legs. */
static inline bool projectile_shooter_is_this_body(
	uint32_t owner_id, bool shooter_actor_is_this_body) {
	return owner_id == PROJECTILE_OWNER_LOCAL_PLAYER
		&& shooter_actor_is_this_body;
}

// Pure: is a (stuck) arrow pickable by the player? Beta gates onCollideWithPlayer on
// inGround, doesArrowBelongToPlayer and an arrow shake of 0 or less. from_player carries
// doesArrowBelongToPlayer (player-fired = true). Pinned by the test.
static inline bool arrow_pickable(bool from_player, bool in_ground, int shake_ticks) {
	return from_player && in_ground && shake_ticks <= 0;
}

// EntityArrow.java:107-125: only the in-ground 1200-tick timeout reaps by age.
// Entity.java:222-224 supplies the separate Y < -64 void reaper at the call site.
static inline bool arrow_should_despawn(bool in_ground, int stuck_ticks) {
	return in_ground && stuck_ticks >= ARROW_STUCK_DESPAWN_TICKS;
}

// Pure: MP client-side embed observation for a wire-position-driven remote
// arrow. Beta MP clients self-simulate the arrow and its own onUpdate plays
// random.drr on the embed [740c583 EntityArrow.onUpdate]; this engine's remote
// arrows only follow network positions, so the embed is inferred: the arrow
// MOVED (a per-tick delta above epsilon) and then went still for
// ARROW_CLIENT_STILL_TICKS consecutive ticks. Returns true exactly ONCE, on
// the tick the still-run completes (the play-the-drr edge). `moved` and
// `still_ticks` are the caller's persistent state. step_dist2 is this tick's
// squared displacement. Epsilon 0.01 blocks/tick: flight never drops below
// gravity's 0.03/tick, and the 0.7 network lerp converges under it within two
// ticks of the wire going quiet. Pinned by the test.
// ponytail: heuristic observation (~3-tick lag vs Beta's exact embed tick);
// upgrade path = client-simulating remote projectiles like Beta.
#define ARROW_CLIENT_STILL_EPS2 (0.01f * 0.01f)
#define ARROW_CLIENT_STILL_TICKS 2
static inline bool arrow_client_settled(float step_dist2, bool* moved,
										uint8_t* still_ticks) {
	if(step_dist2 > ARROW_CLIENT_STILL_EPS2) {
		*moved = true;
		*still_ticks = 0;
		return false;
	}
	if(!*moved || *still_ticks >= ARROW_CLIENT_STILL_TICKS)
		return false; // never flew yet, or the edge already fired
	return ++(*still_ticks) == ARROW_CLIENT_STILL_TICKS;
}

// Pure: launch speed for a fired arrow. Faithful b1.7.3 is a FIXED multiplier (bow 1.5,
// skeleton 0.6) -- there is NO charge curve in this version, so `charge` (0..1) is
// accepted for API symmetry but the FAITHFUL path ignores it (charge>=min just gates
// whether a shot happens at all; see arrow_charge_fires). Exposed + pinned so a future
// deliberate (documented) charge adaptation has one place to change, not a magic 1.5.
static inline float arrow_launch_speed(bool from_player) {
	return from_player ? ARROW_SPEED_BOW : ARROW_SPEED;
}

// Pure: faithful on-hit damage in the TARGET's own scale. Beta is a flat value for ALL
// arrows (no velocity scaling in 1.7.3): a hit on the PLAYER applies player-scale
// ARROW_DAMAGE (32); a hit on a MOB applies the raw mob-scale ARROW_DAMAGE_BOW_BASE (4).
// `hit_player` selects the scale (NOT from_player -- a skeleton arrow that hits another
// mob must deal mob-scale, and a player arrow only ever hits mobs here). Pinned by test.
static inline int arrow_hit_damage(bool hit_player) {
	return hit_player ? ARROW_DAMAGE : ARROW_DAMAGE_BOW_BASE;
}

// Pure: does a draw of `charge` (0..1) fire? Faithful b1.7.3 has no min-charge (the bow
// fires instantly), so a press always fires as long as it is >= 0 -- but the CONTROL
// layer maps a press to charge=1.0, so this is effectively "did the player press". We
// keep a min-charge PARAMETER (defaulting to 0 via ARROW_BOW_MIN_CHARGE) so the gate is
// explicit + testable and a future deliberate charge adaptation has a hook. The arrow
// CONSUME gate (must own an arrow) is enforced separately at the call site (Beta's
// consumeInventoryItem). Pinned by the test.
#define ARROW_BOW_MIN_CHARGE 0.0f // b1.7.3: instant fire, no minimum draw (post-1.8 only)
static inline bool arrow_charge_fires(float charge) {
	return charge >= ARROW_BOW_MIN_CHARGE;
}

// --- M4 S5: THROWN EGG (Beta EntityEgg / ItemEgg) -------------------------------
// Physics [740c583: EntityEgg]: setEggHeading renormalizes the throw to speed 1.5
// (with a tiny 0.0075-sigma gaussian spread -- omitted here, matching the engine's
// bow, which also omits its Beta spread; both are sub-degree cosmetic jitter); per
// tick position advances by motion, then drag x0.99 (x0.8 in water), then gravity 0.03
// -- the SAME 0.99/0.03 pair as the arrow, so the arrow constants are reused with
// egg-named aliases pinned by the test. On ANY hit (block or entity) the egg dies:
// the entity hit takes a thrower-sourced attackEntityFrom of 0 -- ZERO damage, hurt/knockback
// only -- then the server rolls 1-in-8 for 1 chicken, with a NESTED
// 1-in-32 roll upgrading to 4 (net 1/256); chicks are ADULTS (no babies in
// b1.7.3). The thrower is immune while ticksinAir < 5 (the arrow's owner grace).
#define EGG_LAUNCH_SPEED 1.5f  // EntityEgg ctor heading: speed 1.5, spread 1.0
#define EGG_GRAVITY ARROW_GRAVITY   // 0.03/tick    [EntityEgg.onUpdate gravity]
#define EGG_AIR_DRAG ARROW_AIR_DRAG // 0.99/tick    [EntityEgg.onUpdate air drag]
#define EGG_WATER_DRAG 0.8f         // 0.8 in water  [EntityEgg.onUpdate isInWater]
#define EGG_CHICK_DIE 8             // 1-in-8 roll -> spawn chicken(s)
#define EGG_CHICK_QUAD_DIE 32       // nested 1-in-32 roll -> 4 chickens
// Pure: the chick-count decision from the two Beta rolls (roll8/roll32 are the raw
// nextInt results). 0 = no chick (the 7/8 case); the quad roll is only consumed
// when roll8 hit -- callers must draw it NESTED, exactly like Beta.
static inline int egg_chick_count(int roll8, int roll32) {
	if(roll8 != 0)
		return 0;
	return roll32 == 0 ? 4 : 1;
}

// Direct-approach ("beeline") chase steering. dx/dz = (player - mob) on XZ.
// Returns true iff the player is within the follow range (the caller sets
// AI_CHASE and uses the written unit heading toward the player); false leaves
// the heading zeroed and the caller wanders. The degenerate same-XZ-column case
// falls back to +X (a finite unit vector), matching melee_knockback_dir.
//
// RETIRED from production as of FH-13 (Beta 1.7.3 A* pathfinding,
// source/entity/pathfinding.h): server_tick_monster's chase branch now steers off
// PathFinder waypoints (mob_acquire_chase_path + path_follow_steer in
// entity_monster.c), not this beeline. Kept -- not deleted -- for two reasons: (1)
// tests/test_mob_chase.c still pins its exact behaviour as an independent unit, and
// (2) it is the natural T2 fallback candidate (CLAUDE.md "Performance is a
// first-class goal") if the FH-13 hardware round's pathfinding_perf_counters
// (pathfinding.h) show real A* too expensive on Broadway -- a documented,
// measured, opt-in reversion to this straight-line steer, not a silent drift back
// to it. Do not wire it back into server_tick_monster without that measurement.
static inline bool mob_chase_dir(float dx, float dz, float follow_range_sq,
								 float* out_x, float* out_z) {
	float len2 = dx * dx + dz * dz;
	if(len2 >= follow_range_sq) {
		*out_x = 0.0f;
		*out_z = 0.0f;
		return false;
	}
	if(len2 < 1.0e-4f) {
		*out_x = 1.0f;
		*out_z = 0.0f;
		return true;
	}
	float inv = 1.0f / sqrtf(len2);
	*out_x = dx * inv;
	*out_z = dz * inv;
	return true;
}

// Skeleton keep-distance + circle-strafe steer (audit #17e). dx/dz = (player - mob)
// on XZ; dist_sq is dx*dx+dz*dz (passed in to avoid a recompute at the call site).
// circle_sign (+1 or -1) picks a STABLE handedness for the orbit (derived per-entity
// at the call site, e.g. from the entity id, so the skeleton circles one consistent
// way instead of vibrating tick-to-tick). allow_backoff gates the too-close AWAY
// steer: when false (the call site found the cell BEHIND the mob is lava or air-over-a-
// drop) the too-close case ORBITS in place instead of back-pedalling into the hazard.
// Writes a UNIT XZ heading into out_x/out_z and returns true iff the player is inside
// follow_range_sq (the caller drives AI_CHASE + the shared move/cap tail with the
// written heading); false leaves the heading zeroed so the caller wanders -- exactly
// like mob_chase_dir's contract.
//
// Heading by band (radial term = toward/away the player; tangent = the circle-strafe):
//   * dist_sq < standoff_in_sq  -> PURE RADIAL AWAY (back off, playability add) IF
//                                  allow_backoff, else pure tangent (orbit in place).
//                                  Pure radial (no tangent) so the retreat carries the
//                                  FULL move_speed straight away -- a 45-deg blend would
//                                  halve the effective back-off (the cos45 ~ 0.71
//                                  component) and lose even more ground to the player.
//   * dist_sq > standoff_out_sq -> radial TOWARD + tangent (close in while still arcing)
//   * in the band               -> NO radial (pure tangent orbit = the faithful strafe)
// The tangent of a unit radial (rx,rz) is (-rz, rx) (a 90-deg CCW rotation, already
// unit); circle_sign flips it for handedness. A blended (radial+tangent) sum is
// re-normalized so the heading stays unit -- the shared entity_clamp_speed then caps it
// to the same per-mob speed (no speed-up; Beta-parity move_speed kept). Degenerate
// same-XZ-column (dist_sq < 1e-4) falls back to +X like mob_chase_dir, so no NaN.
// standoff_in_sq < standoff_out_sq is required (the call site uses the
// SKELETON_STANDOFF_*_SQ band, which nests inside SKELETON_SHOOT_RANGE_SQ).
static inline bool skeleton_steer(float dx, float dz, float dist_sq,
								  float follow_range_sq, float standoff_in_sq,
								  float standoff_out_sq, int circle_sign,
								  bool allow_backoff, float* out_x, float* out_z) {
	if(dist_sq >= follow_range_sq) {
		*out_x = 0.0f;
		*out_z = 0.0f;
		return false; // out of follow range: caller wanders
	}
	// Unit radial toward the player (rx,rz). Degenerate column -> +X (finite, no NaN).
	float rx, rz;
	if(dist_sq < 1.0e-4f) {
		rx = 1.0f;
		rz = 0.0f;
	} else {
		float inv = 1.0f / sqrtf(dist_sq);
		rx = dx * inv;
		rz = dz * inv;
	}
	// Tangent (perpendicular) for the circle-strafe: (-rz, rx) rotated by handedness.
	float sign = (circle_sign < 0) ? -1.0f : 1.0f;
	float tx = -rz * sign;
	float tz = rx * sign;
	// TOO CLOSE: pure radial AWAY (max back-off) when the cell behind is safe; otherwise
	// orbit in place (do NOT back-pedal into lava / off a ledge). Pure radial here also
	// keeps the renormalize below trivially well-defined (no toward/away cancellation).
	if(dist_sq < standoff_in_sq) {
		if(allow_backoff) {
			*out_x = -rx; // straight away from the player at full move_speed
			*out_z = -rz;
		} else {
			*out_x = tx;  // hazard behind -> circle in place instead of retreating
			*out_z = tz;
		}
		return true;
	}
	// IN BAND (radial 0) -> pure tangent; BEYOND (radial +1) -> toward + tangent.
	float radial = (dist_sq > standoff_out_sq) ? 1.0f : 0.0f;
	float bx = rx * radial + tx;
	float bz = rz * radial + tz;
	// |bx,bz| is sqrt(2) (toward+tangent) or 1 (pure tangent), never near zero, so this
	// renormalize never divides by ~0. (The only paths that could degenerate -- the
	// same-column column and the pure-away/orbit too-close case -- already returned a
	// unit vector above, so no separate NaN guard is reachable here.)
	float binv = 1.0f / sqrtf(bx * bx + bz * bz);
	*out_x = bx * binv;
	*out_z = bz * binv;
	return true;
}

// --- Beta 1.7.3 natural mob DESPAWN (audit #16) -------------------------------
// EntityLiving.func_27021_X() == despawnEntity(), called once per tick from
// updatePlayerActionState() AFTER the entity age increment. Byte-faithful structure
// (verified vs the b1.7.3 decompile AND the mc173 faithful reimpl
// tick_natural_despawn -- both agree on the four constants below):
//
//   - look up the closest player at ANY distance (-1.0 radius);
//   - only when canDespawn() is true and such a player exists:
//       take the 3D squared feet-to-feet distance d2 to that player;
//       d2 above 16384 (> 128 blocks) kills the entity INSTANTLY;
//       then, when entityAge exceeds 600 AND a 1-in-800 roll hits:
//           d2 below 1024 (< 32 blocks) RESETS entityAge to 0 (mob stays),
//           otherwise (32..128) the entity is killed (despawn).
//   The two distance tests run in sequence (see below), in the
//   decompile's own order.
//
// CRITICAL branch facts (do NOT use the later persistenceRequired/oldAi version):
//  * canDespawn() is the OUTER gate; it returns TRUE by default and NONE of the 4
//    hostiles (zombie/skeleton/creeper/spider) override it -> ALL FOUR despawn.
//  * The two distance tests are SEQUENTIAL, not else-if. The >128 branch is
//    AGE-INDEPENDENT (no age/roll needed) and does NOT re-check the 32 band.
//  * The aged+roll branch draws the 1/800 roll ONLY on ticks where entityAge > 600
//    (Beta's age test SHORT-CIRCUITS before the 1-in-800 roll is drawn). The
//    engine MUST gate the rand draw the same way: s->rand_src is a shared seeded
//    stream consumed later this tick, so an unconditional draw would shift the seq
//    (a determinism bug). Inside the branch, <32 RESETS entityAge to 0 (mob persists);
//    32..128 despawns.
//  * setEntityDead() just sets isDead -- DESPAWN IS SILENT: no onDeath, no
//    dropFewItems, no XP. (Loot only drops on the health<=0 DEATH path.)
//  * Aggro/target does NOT block or reset the clock; only attackEntityFrom resets
//    entityAge to 0 -- and it does so as its FIRST statement, BEFORE the hurtResistant-
//    Time absorb gate, so EVERY contact (melee, lava, drown, fire-DoT, fall) resets
//    the clock even when the i-frames absorb the hit. (mc173 corroborates; its
//    `wander_time` == entityAge.)
//  * mc173's 0-players-loaded force-despawn is NON-Beta and intentionally NOT copied
//    (Beta's closest-player lookup with -1.0 always returns the single server player).
//  * AGING (corrected 2026-06-19): EntityLiving bumps entityAge +1/tick; EntityMob.on-
//    LivingUpdate adds +2 MORE on a tick where getEntityBrightness > 0.5 -- so a bright
//    EntityMob hostile ages +3/tick. This IS real Beta 1.7.3 (mc173 copies it faithfully);
//    the +2 was previously wrongly omitted as a "non-Beta embellishment" and is now applied
//    in entity_monster.c's despawn block for the four EntityMob hostiles.
#define MOB_DESPAWN_FAR        128.0f                 // instant despawn past 128 blocks
#define MOB_DESPAWN_FAR_SQ     (MOB_DESPAWN_FAR * MOB_DESPAWN_FAR)   // 16384 = 128^2
#define MOB_DESPAWN_NEAR       32.0f                  // within 32 blocks resets the clock
#define MOB_DESPAWN_NEAR_SQ    (MOB_DESPAWN_NEAR * MOB_DESPAWN_NEAR) // 1024 = 32^2
#define MOB_DESPAWN_MIN_AGE    600    // entityAge gate (30s @ 20 TPS) before the roll
#define MOB_DESPAWN_ROLL_DEN   800    // 1-in-800 per-tick roll

// The pure despawn decision, factored out of the RNG so it is deterministic +
// unit-testable (tests/test_mob_despawn.c). `roll_hit` is the PRE-COMPUTED
// 1-in-800 roll result the caller draws with the seeded rand_gen. Mirrors
// the Beta branch above exactly:
//   dist_sq > 128^2                                  -> DESPAWN (age/roll ignored)
//   age > 600 && roll_hit && dist_sq < 32^2          -> RESET_AGE (mob stays)
//   age > 600 && roll_hit && 32^2 <= dist_sq <= 128^2-> DESPAWN
//   otherwise (too young, roll missed, or in 32..128 band w/o roll) -> KEEP
// NOTE the >128 instant case is AGE-INDEPENDENT: a far mob despawns even at age 0.
enum mob_despawn_action {
	MOB_DESPAWN_KEEP = 0,  // mob lives on, clock keeps accumulating
	MOB_DESPAWN_RESET_AGE, // close+aged+roll: zero entityAge, mob persists
	MOB_DESPAWN_DESPAWN,   // remove the mob silently (no loot)
};
static inline enum mob_despawn_action
mob_despawn_action(int age, float dist_sq, bool roll_hit) {
	if(dist_sq > MOB_DESPAWN_FAR_SQ)
		return MOB_DESPAWN_DESPAWN; // > 128: instant, regardless of age/roll
	if(age > MOB_DESPAWN_MIN_AGE && roll_hit)
		return (dist_sq < MOB_DESPAWN_NEAR_SQ) ? MOB_DESPAWN_RESET_AGE
											   : MOB_DESPAWN_DESPAWN;
	return MOB_DESPAWN_KEEP;
}

// --- AUDIT #18 ITEM 4: Beta 1.7.3 randomized drop COUNT (EntityMob.dropFewItems) ---
// EntityMob.dropFewItems draws a uniform count in 0..2 and drops that many single
// items of the mob's drop id -- so 0, 1, or 2 of the per-mob drop item,
// uniform. There is NO looting/Fortune in b1.7.3 (the looting-widened count bound is
// post-Beta). v2 previously hardcoded drop_count = 1 (an explicit placeholder); this
// rolls the faithful 0..2 count with the SEEDED rand_gen at the death-drop site (the
// factory has no seeded source in scope -- see entity_monster.c). A roll of 0 means NO
// drop, so the caller must guard against spawning an empty stack. `roll` is the caller's
// rand_gen_int(rng, MOB_DROP_COUNT_DEN) result; this just clamps it to a uint8 count.
#define MOB_DROP_COUNT_DEN 3 // uniform {0,1,2} per drop item (b1.7.3)
static inline int mob_drop_count(int roll) {
	if(roll < 0)
		return 0;
	if(roll >= MOB_DROP_COUNT_DEN)
		return MOB_DROP_COUNT_DEN - 1; // defensive clamp (rand_gen_int never exceeds)
	return roll;
}

// --- AUDIT #18 ITEM 3: Beta 1.7.3 death animation (EntityLiving.deathTime) ------
// EntityLiving.onEntityUpdate: while health is at or below 0, deathTime goes up by one
// and, once it passes 20, the entity is finalized and killed. So once health hits 0 it is NOT
// removed instantly: deathTime counts 0,1,2,... while health<=0 and the mob is removed
// only when deathTime > 20 (on the 21st tick after death, a ~1.05s flop). onDeath()/
// dropFewItems runs on the DEATH path (b1.7.3 EntityLiving.onDeath drops loot when not
// a multiplayer client), so LOOT DROPS AT DEATH (deathTime==0 edge), NOT at removal. During
// the death window the AI is suppressed (no swing/move/path) and the renderer FLOPS the
// model over: RenderLiving rotates min(1, sqrt((deathTime + partialTick - 1)/20 * 1.6)) *
// getDeathMaxRotation degrees about the body's forward axis (see mob_death_roll_fraction)
// -- the classic "fall over sideways". v2 mirrors this
// with death_time plus a separate death_loot_dropped guard: on the health<=0 edge it
// drops loot ONCE, then holds the entity ~20 ticks (no AI), networks death_time so the
// client renders the tilt, and reaps at death_time > MOB_DEATH_TIME_MAX. The separate
// guard is required for a late moveEntity kill, where onDeath runs at death_time == 0.
#define MOB_DEATH_TIME_MAX 20 // deathTime > 20 -> remove (Beta: 21 ticks, ~1.05s)

// Death-roll rotation (next-gaps-0924/G8: the Beta curve; the earlier linear
// `(deathTime + td - 1)/20 * 90` stand-in reached flat 8 ticks late). Beta
// RenderLiving.rotateCorpse (RenderLiving.java:121-133):
//   only while deathTime is positive: f = (deathTime + partial - 1) / 20 x 1.6,
//   then its float square root, capped at 1; the body rolls f x the renderer's
//   getDeathMaxRotation degrees about the Z axis.
// so the body is flat once deathTime + partial reaches 13.5. The fraction is
// shared by every EntityLiving renderer; the maximum is per renderer.
static inline float mob_death_roll_fraction(int death_time, float td) {
	if(death_time <= 0)
		return 0.0f;
	float f = ((float)death_time + td - 1.0f) / 20.0f * 1.6f;
	if(!(f > 0.0f))
		return 0.0f; // sqrt domain guard (td is >= 0, so only f == 0 lands here)
	f = sqrtf(f); // == Beta's double sqrt narrowed to float: sqrt is correctly rounded
	if(f > 1.0f)
		f = 1.0f;
	return f;
}

// RenderLiving.getDeathMaxRotation returns 90 (RenderLiving.java:154-156); also the
// player's (RenderPlayer.java:238-247 awake branch calls super.rotateCorpse).
#define MOB_DEATH_ROLL_MAX_DEG 90.0f
// RenderSpider.setSpiderDeathMaxRotation returns 180 (RenderSpider.java:11-13,31-33).
#define SPIDER_DEATH_ROLL_MAX_DEG 180.0f

// Default renderer (players, and every mob without an override).
static inline float mob_death_roll_deg(int death_time, float td) {
	return mob_death_roll_fraction(death_time, td) * MOB_DEATH_ROLL_MAX_DEG;
}

// getDeathMaxRotation per mob renderer. The squid returns 0 because
// RenderSquid replaces rotateCorpse outright (RenderSquid.java:14-22,40-42:
// translate + yaw + its swim pitch/yaw, with NO deathTime term) -- a dying
// squid does not tilt. Every other v2 mob renderer (biped, creeper, cow, pig,
// sheep, chicken, wolf, slime, ghast, giant) inherits the RenderLiving 90.
static inline float mob_death_max_rotation_deg(int monster_id) {
	switch(monster_id) {
	case MONSTER_SPIDER: return SPIDER_DEATH_ROLL_MAX_DEG;
	case MONSTER_SQUID: return 0.0f;
	default: return MOB_DEATH_ROLL_MAX_DEG;
	}
}

static inline float mob_death_roll_deg_for(int monster_id, int death_time,
										   float td) {
	return mob_death_roll_fraction(death_time, td)
		* mob_death_max_rotation_deg(monster_id);
}

// True iff a dying mob (death_time already incremented this tick) should now be REMOVED.
// Beta removes when deathTime > 20. Pure so the 20-tick hold is pinned by the test.
static inline bool mob_death_finished(int death_time) {
	return death_time > MOB_DEATH_TIME_MAX;
}

// --- AUDIT #18 ITEM 5: Beta 1.7.3 PACK-SPAWN scatter (SpawnerAnimals.performSpawning) --
// Per eligible chunk Beta picks a pack ANCHOR, then runs 3 pack-SEED attempts, each
// reset to the anchor, and within each seed places up to 4 pack MEMBERS by RANDOM-WALKING
// off the previous member's position:
//   - each of the 3 seeds restarts at the anchor;
//   - each of up to 4 members then moves the position:
//       X by the difference of two 0..5 draws (triangular, range -5..+5),
//       Y by the difference of two bound-1 draws (ALWAYS 0, but 2 draws),
//       Z by the difference of two 0..5 draws;
//   - a spot that passes canCreatureSpawnAt is placed; at the chunk cap it moves on.
// CRITICAL determinism: the Y draws are NO-OPS in OUTPUT (a bound-1 draw is always 0) but are
// REAL RNG CONSUMPTION -- every member draws all 6 (X:2, Y:2, Z:2) in that order, or the
// seeded stream diverges. The horizontal scatter radius is 6 (the per-axis bound). v2 maps Beta's
// "per-chunk pack" onto its player-relative attempt loop: at a validated anchor it spawns
// the anchor then walks PACK members off it with the exact 6-draw order, re-validating each
// (the existing air/floor/light spot check), capped at the per-mob crowd cap. (v2 does not
// have Beta's chunk/seed nesting -- it spawns one pack per successful attempt -- so the
// 3-seed outer loop is folded into the existing MOB_SPAWN_ATTEMPTS; one pack per attempt.)
#define PACK_SPAWN_SCATTER 6   // per-axis draw bound 6: horizontal scatter radius
#define PACK_SPAWN_MEMBERS 4   // members per pack seed (Beta inner loop 0..4)

// One pack-member random-walk step off the previous member's (x,y,z), consuming the EXACT
// 6-draw Beta order (X:2, Y:2, Z:2). `next(rng_ctx, n)` returns rand_gen_int(rng, n) so the
// pure scatter logic is testable headless with a deterministic stub; the Y draws are made
// (and discarded as 0) for stream fidelity. Mutates *x/*z; *y is unchanged (Beta's bound-1
// draws cancel) but the two draws still happen. Returns nothing; the caller re-validates + caps.
static inline void pack_spawn_scatter_step(int (*next)(void* ctx, int n), void* ctx,
										   int* x, int* y, int* z) {
	*x += next(ctx, PACK_SPAWN_SCATTER) - next(ctx, PACK_SPAWN_SCATTER); // +-X
	*y += next(ctx, 1) - next(ctx, 1); // ALWAYS 0, but 2 real draws (determinism!)
	*z += next(ctx, PACK_SPAWN_SCATTER) - next(ctx, PACK_SPAWN_SCATTER); // +-Z
}

// --- Beta 1.7.3 natural-spawn WORLD-SPAWN exclusion (SpawnerAnimals.performSpawning) --
// Beta requires EVERY spawn candidate -- for ALL THREE creature categories (monster,
// animal, water; the category loop shares this one check) -- to clear TWO distance
// tests before an entity is even constructed: no player within 24 blocks
// (handled separately by v2's existing player-relative ring pick + the point-blank
// member reject) AND a 3D distance from the world spawn point (World.getSpawnPoint) of at least 24 blocks
// (576 = 24^2). The second test guarantees a hostile/animal/squid-free bubble
// around the world's original spawn point (typically the player's base) and was
// MISSING entirely from v2's natural-spawn passes. Checked for the anchor AND every
// pack member, exactly as Beta re-derives it inside the same per-member scatter loop
// that produces each candidate position. Candidate coords use Beta's exact MIXED
// offset -- X/Z get +0.5 (block-center), Y does not -- matching the candidate coordinates in
// the decompile; the spawn point itself is the raw stored int with no offset.
#define WORLD_SPAWN_EXCLUSION_DIST_SQ 576.0f // 24^2 [SpawnerAnimals spawn-distance test >= 576]

// True iff the candidate is FAR ENOUGH from the world spawn to be allowed to spawn
// (Beta's at-least-576 test, so the 24-block boundary itself is INCLUDED, not
// excluded). `spawn_x/y/z` is the world's stored spawn point (s->player.spawn_x/y/z);
// `cand_x/y/z` is the candidate's BLOCK coordinate (pre-offset), matching every other
// caller in this file (mob_spawn_bbox_clear et al. all take block coords).
static inline bool mob_spawn_far_from_world_spawn(float cand_x, float cand_y,
												  float cand_z, int spawn_x,
												  int spawn_y, int spawn_z) {
	float dx = (cand_x + 0.5f) - (float)spawn_x;
	float dy = cand_y - (float)spawn_y;
	float dz = (cand_z + 0.5f) - (float)spawn_z;
	return (dx * dx + dy * dy + dz * dz) >= WORLD_SPAWN_EXCLUSION_DIST_SQ;
}

// --- Beta 1.7.3 mob WATER MOVEMENT (M2: the mass-drowning fix) ---------------
// Beta structure, per EntityLiving/EntityCreature:
//   - EntityCreature.updatePlayerActionState: in water (or lava) the mob sets
//     its jump flag when a uniform float is below 0.8 -- the swim "paddle" roll, per tick.
//   - EntityLiving.onLivingUpdate: a jumping mob in water gains 0.04 vertical.
//   - moveEntityWithHeading water branch: moveFlying with input factor 0.02
//     -- a FLAT 0.02 input factor, NOT scaled by moveSpeed (the land branch
//     uses moveSpeed * 0.1) -- then the move, then horizontal motion x0.8 and
//     vertical x0.8 (drag), then vertical minus 0.02 (water gravity, UNDAMPED in
//     the tick it is applied: drag BEFORE gravity).
//
// THE BYTES ARE THE RATIOS HERE (review fix, 2026-06-11). This engine
// integrates exactly like Beta -- pos += vel once per 20Hz tick
// (entity_try_move) -- so Beta's raw per-tick water constants produce
// byte-faithful per-tick DISPLACEMENTS. The first cut of this unit scaled
// them off the engine's land GRAVITY (2.0/tick, 25x Beta's 0.08) on the false
// premise that the engine's velocity units differed; that made a swimming mob
// rise ~1.2 blocks/TICK (Beta: ~0.03) and porpoise out of pools. The land
// constants (GRAVITY 2.0, the 0.25 accel, the Wii speed cap) stay the fork's
// own snappy units -- but in water we replace ALL forces, so Beta's own
// numbers apply directly. The review also caught two more transplant errors,
// both fixed below and pinned by tests/test_mob_water.c:
//   - ORDER: drag THEN gravity (Beta), not gravity-then-drag (never-swim
//     terminal sink is -0.1 = -g/(1-d), not -0.08 = -g*d/(1-d)).
//   - HORIZONTAL INPUT: in water the walk push + Wii speed cap are REPLACED
//     by the flat 0.02 moveFlying input (MOB_WATER_INPUT) -- without
//     this, a chasing mob crossed ponds at full land speed (the cap 0.18 >>
//     Beta's 0.1 water equilibrium), erasing water as Beta's escape mechanic.
#define MOB_WATER_JUMP_CHANCE 0.8f   // Beta: 80% paddle roll in water
#define MOB_WATER_GRAVITY 0.02f      // Beta: vertical -0.02 (raw byte)
#define MOB_WATER_VEL_DAMP 0.8f      // Beta: vertical x0.8 in water
#define MOB_WATER_SWIM_IMPULSE 0.04f // Beta: paddling in water -> vertical +0.04
#define MOB_WATER_DRAG_XZ 0.8f       // Beta: horizontal x0.8 in water
#define MOB_WATER_INPUT 0.02f        // Beta: moveFlying input 0.02 -- flat, ALL mobs
#define MOB_WATER_CLIMB_OUT_VEL 0.3f // Beta EntityLiving.moveEntityWithHeading water tail:
                                     // isCollidedHorizontally + open air up-and-forward ->
                                     // vertical 0.3 (climb out of the pool onto the edge)

// The water step itself lives on the mob's authoritative doubles in
// entity_monster.c (mob motion units S2): the paddle's widened-float 0.04 swim
// impulse before the move, then motion x widened-float 0.8 and vertical -0.02
// after it, and the flat 0.02 moveFlying input in place of the land push. The
// float mob_water_vel_y_step / mob_water_input_step helpers that pinned the
// pre-S2 engine path were dead and are retired; test_mob_water pins the
// constants and Beta's recurrence, the production branch is pinned by
// test_mob_move_parity_golden and test_mob_trajectory_golden.

// --- Beta 1.7.3 SLIME locomotion + split (EntitySlime, decompile-verified) ----
// Hop / auto-jump vertical impulse: RETIRED (mob motion units S2). A slime hop
// and every walker's step jump are Beta's EntityLiving.jump(): vertical motion
// set to widened float 0.42 from isJumping, integrated by Beta's move-then-gravity (apex
// ~1.25 over ~6 ticks). The 3.2 / 3.3 engine-unit impulses and the ySize jump
// bank they needed are gone.

// Beta updatePlayerActionState: the jump delay is a 0..19 draw plus 10 (10-29
// ticks), then INTEGER-divided by 3 when a player is within 16 blocks (the
// aggro latch is v2's player-near proxy; Beta uses a plain closest-player probe
// with no LOS -- the latch needs one sighting first, a documented hair of
// difference). roll20 in 0..19.
static inline int slime_jump_delay(int roll20, bool player_near) {
	int d = roll20 + 10;
	return player_near ? d / 3 : d;
}

// Beta updatePlayerActionState sets the strafe input to 1 minus twice a uniform float
// ALONGSIDE moveForward on every hop-delay expiry (roll01 is the raw
// uniform float draw, [0,1)); the result lands in (-1, 1].
// [740c583: EntitySlime.updatePlayerActionState]
static inline float slime_strafe_roll(float roll01) {
	return 1.0f - roll01 * 2.0f;
}

// Beta's moveFlying (Entity.moveFlying) rotates the (moveStrafing, moveForward)
// pair into world space by rotationYaw, THEN renormalizes: for a slime,
// moveForward is the Beta SIZE (1/2/4) and moveStrafing is in (-1,1], so the
// pre-rotation magnitude sqrt(strafe^2 + size^2) is ALWAYS >= 1 (size >= 1) --
// moveFlying's own floor of the magnitude at 1 never engages, so the
// division that follows collapses the hop's resultant SPEED to the SAME
// friction-scaled magnitude regardless of strafe; only the ANGLE of the
// result differs from pure-forward, by exactly atan2(strafe, size) (a
// bigger slime deviates LESS per hop: atan2(1,4)~=14deg vs a size-1 slime's
// atan2(1,1)=45deg -- verified against [740c583: Entity.moveFlying]).
// CaveWii's hop model applies a flat scalar speed to a unit heading vector
// (no friction/renormalization system to replicate), so the faithful-in-kind
// translation is this heading ROTATION by the same angle, leaving the speed
// (computed separately, see the FIX-1 hop-speed comment at the call site)
// untouched. dir_x/dir_z is the unrotated steer heading (chase/wander);
// beta_size is the slime's Beta size (1/2/4, never 0 here -- callers only
// invoke this on a confirmed slime). [740c583: EntitySlime.updatePlayerActionState]
static inline void slime_hop_wobble_dir(float dir_x, float dir_z,
										float strafe_roll, int beta_size,
										float* out_x, float* out_z) {
	float theta = atan2f(strafe_roll, (float)beta_size);
	float s = sinf(theta), c = cosf(theta);
	*out_x = dir_x * c + dir_z * s;
	*out_z = dir_z * c - dir_x * s;
}

// AUDIT 2026-09-01 (REGRESSION-TODAY, CONFIRMED) -- "slime hops straight up
// (zero horizontal)": Beta's EntitySlime.updatePlayerActionState calls
// moveEntityWithHeading EVERY tick the hop-delay fires, UNCONDITIONALLY --
// moveForward set to getSlimeSize is never gated on having an active path, so a
// real Beta slime's hop heading can never degenerate to zero: an idle slime
// with no target simply keeps hopping along its OWN current rotationYaw.
// CaveWii's steer heading (entity_monster.c's shared mob_path_follow_tail
// output, e->data.monster.direction[]) is correct plumbing for the OTHER
// hostile mobs -- the inherited EntityLiving updatePlayerActionState
// no-path fallback genuinely IS zero WALKING for them -- but EntitySlime
// overrides that method and never takes it, so reusing the same "no path ->
// zero" convention for the slime hop was the bug: slime_hop_wobble_dir
// rotates whatever heading it is given, and rotating the zero vector stays
// the zero vector at any angle, collapsing the hop to vertical-only.
//
// This is the fallback, applied BEFORE slime_hop_wobble_dir: when the steer
// heading is the exact zero vector (the only degenerate value
// mob_path_follow_tail / the fresh-spawn ctor ever produce -- never a small
// nonzero epsilon), substitute a unit heading derived from the slime's own
// facing yaw instead, so moveForward set to the size always has a real direction to
// scale; the strafe wobble above then offsets THAT heading exactly as it
// already offsets a real steer vector. A genuinely nonzero steer heading
// passes through completely untouched -- byte-identical to the pre-fix
// behaviour for every chasing/wandering slime; only the previously-broken
// idle/out-of-range case changes. facing_yaw is e->data.monster.body_yaw
// (entity.h: an absolute world yaw) in this engine's atan2f(dx,dz) heading
// convention, so its unit vector is (sin(yaw), cos(yaw)) -- the same
// convention slime_face_player_yaw/mob_turn_toward already use. [740c583:
// EntitySlime.updatePlayerActionState]
static inline void slime_hop_steer_heading(float dir_x, float dir_z,
											float facing_yaw, float* out_x,
											float* out_z) {
	if(dir_x == 0.0f && dir_z == 0.0f) {
		*out_x = sinf(facing_yaw);
		*out_z = cosf(facing_yaw);
	} else {
		*out_x = dir_x;
		*out_z = dir_z;
	}
}

// Beta EntitySlime.updatePlayerActionState calls faceEntity on the player (yaw step 10, pitch 20)
// UNCONDITIONALLY every tick a player is within 16 blocks -- BEFORE and
// independent of the hop-delay gate below it, unlike every other EntityMob
// hostile whose facing rides its chase steer. The 16-block closest-player probe
// has NO line-of-sight requirement (a raw 3D distance probe), so this mirrors
// that: true + *out_yaw set (this engine's atan2(dx,dz) yaw convention, same
// as the movement-derived fallback it augments) when in range; false (yaw
// left untouched) when not, matching Beta leaving rotationYaw alone on a miss.
// Pure + position-only so it is usable from client-side rendering code with
// no server_local/world dependency. [740c583: EntitySlime.updatePlayerActionState]
static inline bool slime_face_player_yaw(float slime_x, float slime_y,
										 float slime_z, float player_x,
										 float player_y, float player_z,
										 float* out_yaw) {
	float dx = player_x - slime_x;
	float dy = player_y - slime_y;
	float dz = player_z - slime_z;
	if (dx * dx + dy * dy + dz * dz > 16.0f * 16.0f)
		return false;
	*out_yaw = atan2f(dx, dz);
	return true;
}

// Beta EntitySlime.setEntityDead split offsets: child i of 4 lands at
// x + ((i%2)-0.5)*size/4, z + ((i/2)-0.5)*size/4 -- the four (+-size/8,
// +-size/8) corners of a 2x2 grid. beta_size is the PARENT's size (2 or 4).
static inline void slime_split_offset(int i, int beta_size, float* dx,
									  float* dz) {
	*dx = ((float)(i % 2) - 0.5f) * (float)beta_size / 4.0f;
	*dz = ((float)(i / 2) - 0.5f) * (float)beta_size / 4.0f;
}

// Beta SLIME-CHUNK check, BIT-EXACT (Chunk.func_997_a + EntitySlime.getCan-
// SpawnHere, decompile-verified): a chunk is "slimy" iff
//   a java.util.Random seeded with worldSeed plus x*x*4987142, x*5947611,
//   (z*z)*4392871 and z*389711, the whole sum XORed with 987234911,
//   gives 0 from a bound-10 nextInt
// JAVA SEMANTICS REPRODUCED EXACTLY: the x*x*4987142 / x*5947611 / z*389711
// subterms and the z*z factor are computed in 32-bit INT (overflow wraps)
// and only then widened; z*z's widened value is then long-multiplied by
// 4392871; the XOR binds looser than the +s (the whole sum is XORed). The
// multiplies run in uint32 (signed overflow is UB in C; two's-complement
// wraparound is the Java behavior) and the uint64 sum is reinterpreted as
// the signed seed. jrand is the bit-exact java.util.Random reimpl already
// pinned by test_java_random -- so v2 slime chunks land on EXACTLY the same
// chunks as a real Beta world on the same seed (pinned in test_slime against
// an independently computed Java truth table, negative + overflow coords
// included). Pure; callers link worldgen/java_random.c.
static inline bool slime_chunk_is_slimy(int64_t world_seed, int32_t cx,
										int32_t cz) {
	uint32_t ux = (uint32_t)cx, uz = (uint32_t)cz;
	int32_t a = (int32_t)(ux * ux * 4987142u); // x*x*4987142 in Java int
	int32_t b = (int32_t)(ux * 5947611u);      // x*5947611 in Java int
	int64_t c = (int64_t)(int32_t)(uz * uz) * 4392871LL; // widened z*z times 4392871
	int32_t d = (int32_t)(uz * 389711u);       // z*389711 in Java int
	int64_t seed = (int64_t)((uint64_t)world_seed + (uint64_t)(int64_t)a
							 + (uint64_t)(int64_t)b + (uint64_t)c
							 + (uint64_t)(int64_t)d)
		^ 987234911LL;
	struct java_random r;
	jrand_set_seed(&r, seed);
	return jrand_next_int_bound(&r, 10) == 0;
}

// --- Beta 1.7.3 SQUID swim (EntitySquid, decompile-verified) ------------------
// All values are Beta's raw bytes -- same pos += vel integration argument as
// the mob water block above.

#ifndef M_PI
#define M_PI 3.14159265358979323846 // strict C99 (devkitPPC -pedantic) hides it
#endif

// updatePlayerActionState re-roll: a uniform angle in [0, 2PI); the vector is
// (cos x 0.2, -0.1 + uniform x 0.2, sin x 0.2). u_angle/u_y are uniform [0,1) draws.
static inline void squid_pick_swim_vector(float u_angle, float u_y,
										  float out[3]) {
	float a = u_angle * 2.0f * (float)M_PI;
	out[0] = cosf(a) * 0.2f;
	out[1] = -0.1f + u_y * 0.2f;
	out[2] = sinf(a) * 0.2f;
}

// Beta re-roll gate, exact clause order: a 1-in-50 roll hits, OR not in water,
// OR a zero swim vector -- the 50-roll is drawn EVERY tick (first clause), which the
// caller does before testing this. Pure for the test.
static inline bool squid_should_repick(bool roll50_hit, bool in_water,
									   bool vec_zero) {
	return roll50_hit || !in_water || vec_zero;
}

// SQUID WATER PROBES (2026-06-12 audit fix -- the "squids act funny in 1-block
// water" hardware report). Beta uses TWO different shrunk-bbox water tests for
// the squid, and neither matches a bare foot-cell sample (which stayed wet a
// full 0.6 blocks ABOVE Beta's boundary -> breach-and-hover at every surface):
//  - MOTION branch: EntitySquid OVERRIDES isInWater() to a LIVE post-move
//    handleMaterialAcceleration on the box contracted 0.6 in Y. On the 0.95-tall box the
//    shrink INVERTS the box ([foot+0.6, foot+0.35]) and the y-loop runs
//    floor(foot+0.6)..floor(foot+1.35) EXCLUSIVE -- a 0.75 span, so it covers
//    AT MOST ONE cell (floor(foot+0.6) when non-empty), and for frac(foot) in
//    [0.4, 0.65) it covers NONE: Beta is unconditionally DRY there even fully
//    submerged. That one beached tick reflects an ascending squid downward --
//    the bottom-hugging sink bias real Beta squids have. Wet therefore needs
//    BOTH: the loop covers a cell AND that cell holds water. At a surface over
//    water cell yw this lands the tight edge: wet while foot < yw+0.4.
//  - REPICK gate: reads the inherited `inWater` FIELD, cached PRE-move from
//    handleMaterialAcceleration on the box contracted 0.4 in Y -> the cell at
//    floor(foot+0.4): wet while foot < yw+0.6 (LOOSE edge).
// The 0.4..0.6 gap is Beta's hysteresis band: a surface-bobbing squid runs the
// beached MOTION branch there while the gate still sees water, so the swim
// vector is HELD across the bob instead of re-rolling (no heading thrash).
// DOCUMENTED APPROXIMATIONS (center-column, single-point): the 0.95-wide box
// can straddle 2 x/z columns (we sample the center column only), and the GATE
// box's y-loop can span 2 cells for frac(foot) in [0.45, 0.6) (we sample one;
// the miss only matters for water-above-air -- transient flowing water). Pure
// cell math here so the boundaries are unit-testable (test_squid).
#define SQUID_MOTION_PROBE_RAISE 0.6f
#define SQUID_GATE_PROBE_RAISE 0.4f
static inline int32_t squid_motion_probe_cell_y(float foot_y) {
	return (int32_t)floorf(foot_y + SQUID_MOTION_PROBE_RAISE);
}
// Beta's motion y-loop upper bound, EXCLUSIVE: floor(maxY + 1) with
// maxY = foot + 0.95 - 0.6. Empty loop (frac(foot) in [0.4, 0.65)) == DRY,
// no matter what block the probe cell holds.
static inline bool squid_motion_loop_covers_cell(float foot_y) {
	return (int32_t)floorf(foot_y + SQUID_MOTION_PROBE_RAISE)
		< (int32_t)floorf(foot_y + 1.35f);
}
static inline int32_t squid_gate_probe_cell_y(float foot_y) {
	return (int32_t)floorf(foot_y + SQUID_GATE_PROBE_RAISE);
}

// Beta out-of-water vertical step (onLivingUpdate else-branch): vertical minus
// 0.08 (plain air gravity), then x0.98. Horizontal is zeroed by the caller
// (a beached squid flops in place, it does not slide). Raw Beta bytes.
static inline float squid_fall_step(float vel_y) {
	return (vel_y - 0.08f) * 0.98f;
}

// Beta swim-speed pulse (onLivingUpdate in-water branch): during the
// contraction half (phase < PI) the propulsion kicks in at 75% of the stroke
// (randomMotionSpeed 1.0); during the glide half it decays x0.9. The
// phase advance + the 1-in-10 rotationVelocity re-roll on wrap stay with the
// caller (they consume RNG). Pure for the test.
static inline float squid_pulse_speed(float phase, float speed) {
	if(phase < (float)M_PI) {
		float t = phase / (float)M_PI;
		if(t > 0.75f)
			speed = 1.0f;
	} else {
		speed *= 0.9f;
	}
	return speed;
}

#endif
