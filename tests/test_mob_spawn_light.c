/*
	Unit test for the living-world phase-1 spawn light gate (server_local.c).

	The day/night sky-light subtraction and the "dark enough for a hostile"
	gate are the Beta-fidelity-critical parts of the spawn loop, and they
	can't be observed easily in Dolphin (you'd have to wait for in-game
	night). This replicates the exact pure-math formulas used in
	server_local.c (daytime_celestial_angle is copied verbatim — it's pure;
	the real one lives in daytime.c which pulls in client gstate).

	2026-08-31 fix: the hostile-darkness gate below used to be pinned as a flat
	deterministic `eff_light <= 7` cutoff, described as "the key Beta
	behaviours". That is NOT what Beta 1.7.3 does. EntityMob.getCanSpawnHere()
	[740c583] runs TWO SEQUENTIAL PROBABILITY ROLLS:
	    if (getSavedLightValue(Sky,x,y,z) > rand.nextInt(32)) return false;
	    light = getBlockLightValue(x,y,z); // max(rawSky-subtract, torch)
	    while thundering, light is re-read with subtract forced to 10;
	    pass when light <= rand.nextInt(8).
	Roll 1 refuses using the RAW, time-of-day-UNADJUSTED stored sky light (an
	open-sky column, raw 15, has up to a 15/32 (~47%) chance of being refused
	OUTRIGHT regardless of how dark the effective light actually is). Only if
	that passes does roll 2 run against the day/night-adjusted effective light,
	giving light 0 a ~100% accept chance, light 7 ~12.5%, and light>=8 a hard
	0%. `hostile_light_roll` below is a verbatim, RNG-free copy (the caller
	supplies the two already-drawn roll values, so exact boundary/short-circuit
	cases are pinned without needing a live RNG) of spawner_hostile_light_ok
	(source/network/spawner.c) -- THE canonical implementation, shared by the
	natural hostile spawn passes AND the dungeon mob-spawner burst.

	The old deterministic claims that remain TRUE under the two-roll model
	(monotonic: adding roll 1 only ever ADDS refusals, never removes one; and
	eff>=8 is unconditionally refused by roll 2 for every possible roll) are
	kept below. The ones that are NOT true under the real model (e.g. "surface
	at midnight always spawns") are replaced with the correct probabilistic
	pin.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef GLM_PI
#define GLM_PI 3.14159265358979323846f
#endif
#define DAY_LENGTH_TICKS 24000

// --- verbatim copies of the pure math from the engine ---
static float daytime_celestial_angle(float time) {
	float X = time - 0.25F;
	if(X < 0)
		X += 1;
	return X + ((1.0F - (cosf(X * GLM_PI) + 1.0F) / 2.0F) - X) / 3.0F;
}

static int mob_sky_subtract(uint64_t world_time) {
	float day
		= (float)(world_time % DAY_LENGTH_TICKS) / (float)DAY_LENGTH_TICKS;
	float ca = daytime_celestial_angle(day);
	float f = 1.0F - (cosf(ca * 2.0F * GLM_PI) * 2.0F + 0.5F);
	if(f < 0.0F)
		f = 0.0F;
	if(f > 1.0F)
		f = 1.0F;
	return (int)(f * 11.0F);
}

// effective light for roll 2 (matches World.getBlockLightValue / server_local's
// `eff`): max(rawSky - subtract, torch). Still needed as roll 2's input.
static int eff_light(int sky_light, int torch_light, int sky_subtract) {
	int sky = sky_light - sky_subtract;
	return (sky > torch_light) ? sky : torch_light;
}

// Verbatim copy of spawner_hostile_light_ok (source/network/spawner.c) --
// Beta EntityMob.getCanSpawnHere()'s exact two-roll light gate [740c583].
// `roll32`/`roll8` are the CALLER-supplied nextInt(32)/nextInt(8) draws: this
// test controls them directly (rather than driving a live RNG) to pin the
// exact boundary + short-circuit behaviour deterministically.
static bool hostile_light_roll(int raw_sky_light, int torch_light,
							   int sky_subtract, int roll32, int roll8) {
	if(raw_sky_light > roll32)
		return false; // Beta: open-sky suppression, unadjusted by time of day
	int eff = eff_light(raw_sky_light, torch_light, sky_subtract);
	return eff <= roll8;
}

int main(void) {
	// Minecraft time: 0 = sunrise, 6000 = noon, 12000 = sunset, 18000 = night.
	int noon = mob_sky_subtract(6000);
	int midnight = mob_sky_subtract(18000);

	assert(noon == 0);      // full daylight: no sky reduction
	assert(midnight == 11); // full night: max sky reduction (Beta value)

	// Subtraction never goes negative or above 11 across a full day.
	for(int t = 0; t < DAY_LENGTH_TICKS; t += 250) {
		int s = mob_sky_subtract((uint64_t)t);
		assert(s >= 0 && s <= 11);
	}

	// --- claims that hold REGARDLESS of the roll values (still deterministic) ---

	// Cave (no sky, no torch): raw_sky=0 means roll 1 can NEVER refuse (0 > any
	// nextInt(32) draw is false), and eff is always 0, which passes roll 2
	// against ANY draw in [0,7] -- spawns regardless of time of day or rolls.
	for(int r32 = 0; r32 <= 31; r32 += 31)
		for(int r8 = 0; r8 <= 7; r8 += 7) {
			assert(hostile_light_roll(0, 0, noon, r32, r8));
			assert(hostile_light_roll(0, 0, midnight, r32, r8));
		}

	// A torch-lit spot (torch 14) is never dark enough, even at night, even at
	// roll 2's best possible draw (7): eff=max(0-11,14)=14, and 14<=7 is never
	// true for any legal roll8.
	for(int r32 = 0; r32 <= 31; r32 += 31)
		for(int r8 = 0; r8 <= 7; r8++)
			assert(!hostile_light_roll(0, 14, midnight, r32, r8));

	// Noon surface (raw_sky=15, subtract=0 -> eff=15): ALWAYS refused. Roll 1
	// may or may not refuse it first, but even when it falls through to roll 2,
	// eff=15 fails every possible roll8 (max draw 7) -- refused either way.
	for(int r32 = 0; r32 <= 31; r32++)
		for(int r8 = 0; r8 <= 7; r8++)
			assert(!hostile_light_roll(15, 0, noon, r32, r8));

	// eff>=8 is a HARD 0% regardless of raw_sky/roll1/subtract: nextInt(8)
	// tops out at 7, so 8<=roll8 is never satisfiable.
	for(int r32 = 0; r32 <= 31; r32++)
		for(int r8 = 0; r8 <= 7; r8++)
			assert(!hostile_light_roll(8, 0, 0, r32, r8));

	// --- claims that are now PROBABILISTIC (the actual Beta-faithful fix) ---

	// Open-sky suppression: roll 1 alone refuses a raw_sky=15 candidate whenever
	// nextInt(32) < 15 -- exactly 15 of the 32 possible draws (~47%), REGARDLESS
	// of subtract or roll8 (roll 2 is never reached on that branch).
	int refused_by_roll1 = 0;
	for(int r32 = 0; r32 <= 31; r32++)
		if(!hostile_light_roll(15, 0, 11, r32, 7)) // roll8=7: roll 2's best case
			refused_by_roll1++;
	assert(refused_by_roll1 == 15);

	// Midnight surface (raw_sky=15, subtract=11 -> eff=4): NOT the old "always
	// spawns". It spawns only when roll 1 passes (roll32>=15) AND roll 2 clears
	// eff=4 (roll8 in {4,5,6,7}, a 4/8 = 50% shape). Both outcomes are reachable:
	assert(hostile_light_roll(15, 0, 11, 31, 4));  // pass roll1, eff<=roll8 (4<=4)
	assert(!hostile_light_roll(15, 0, 11, 31, 3)); // pass roll1, eff>roll8 (4>3)
	assert(!hostile_light_roll(15, 0, 11, 10, 7)); // refused outright by roll 1

	// Boundary eff==7: a real ~12.5% chance (accepts ONLY when roll8==7), NOT
	// the old "always spawns". eff==8 (tested above) stays a hard 0% either way.
	assert(hostile_light_roll(7, 0, 0, 31, 7));  // roll8==7 -> accept
	assert(!hostile_light_roll(7, 0, 0, 31, 6)); // roll8==6 -> refuse (the other 7/8)

	return 0;
}
