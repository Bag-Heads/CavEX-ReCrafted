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

#ifndef SERVER_WEATHER_H
#define SERVER_WEATHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../block/blocks_data.h" // w_coord_t
#include "../worldgen/java_random.h"

struct server_local;

/* Beta EntityLightningBolt's own per-bolt lifecycle state [740c583]:
 * lightningState (2 -> 1 -> 0 -> negative -> restrike-or-die) and
 * boltLivingTime (the restrike budget, rand.nextInt(3)+1 at spawn, consumed
 * one per restrike). x/y/z is the bolt's FIXED strike position (Beta never
 * moves it after the constructor). Owned by struct server_local's
 * `weather.active_bolts` array (server_local.h) -- a realloc-grown list, not
 * a fixed cap (house rule: no silent caps on rare-but-real gameplay events). */
struct active_lightning_bolt {
	w_coord_t x, y, z;
	int32_t lightning_state;
	int32_t bolt_living_time;
	/* CFR Entity.java:96: one Random for this bolt's entire lifetime. */
	struct java_random gameplay_rng;
	/* A1c: the world instance the bolt struck in (slot + generation, the
	 * A1a identity). server_weather_tick_lightning_bolts_w ticks only the
	 * bolts owned by `w` and drops a bolt whose world is no longer live. */
	uint8_t world_slot;
	uint32_t world_generation;
};

struct server_world;

/* Beta World's THREE constructor overloads [740c583], all identical:
 * field_9437_g = nextInt() of a fresh Random; ... soundCounter =
 * the world rand's nextInt(12000). Call once per world create/load (F3,
 * adversarial review -- these were previously left at their zero-init
 * default on every load, unlike Beta which reseeds field_9437_g/soundCounter
 * at EVERY World construction). Draws both from `s->rand_src` (this engine's
 * one shared stream stands in for Beta's field_9437_g fresh-`Random()` AND
 * the world rand -- the established one-stream convention). NOT persisted,
 * exactly like Beta (a fresh Random() every construction, by definition
 * never saved). */
void server_weather_seed(struct server_local* s);

/* Beta World.updateWeather [740c583], run once per non-loading server tick,
 * BEFORE world_time advances (matches Beta's World.tick() calling order).
 * Fully inert (no-op) when the world has no sky (Nether) -- Beta's
 * `worldProvider.hasNoSky` gate. */
void server_weather_tick(struct server_local* s);

/* Beta World.updateBlocksAndPlayCaveSounds's cave-sound/lightning/snow-freeze
 * branches ONLY [740c583] -- see WEATHER_SLICE_NOTES.md for why the trailing
 * 80-random-tick loop is intentionally NOT re-implemented here (the engine's
 * existing server_world_random_tick already owns ordinary block random ticks
 * as a separate, already-tested, already-tuned T2 mechanism). Runs the exact
 * Beta 9-chunk-radius ring around the player and advances the shared position
 * LCG exactly where Beta does within these three branches.
 * F6(a) (adversarial review, doc correction): Beta's
 * updateBlocksAndPlayCaveSounds itself has NO hasNoSky/Nether gate -- the
 * method doesn't check dimension at all. In REAL Beta, the Nether's
 * WorldServer is a SEPARATE World instance with its own rainingStrength/
 * thunderingStrength fields that simply never get set (because THAT
 * instance's updateWeather is the one gated by hasNoSky, and it never runs),
 * so lightning/snow never fire there -- an emergent consequence of per-
 * dimension object separation, not an explicit check. THIS engine uses ONE
 * shared `struct server_local` (and therefore one shared `weather` struct)
 * across a dimension switch, so without an explicit gate HERE, a Nether
 * visit while it's raining in the overworld would incorrectly inherit that
 * raining state. The gate below REPRODUCES Beta's real Nether OBSERVABLE
 * behavior (weather effects never fire there) through this engine's
 * shared-struct architecture -- it is a mechanism substitution, not a
 * fabricated Beta rule.
 * px/pz are the player's CHUNK coordinates (WCOORD_CHUNK_OFFSET(floor(x/z))). */
void server_weather_effects_tick(struct server_local* s, w_coord_t px,
								 w_coord_t pz);

/* Beta World.isRaining(): getRainStrength(1.0F) > 0.2. */
bool server_weather_is_raining(const struct server_local* s);
/* Beta World.getIsThundering(): getWeightedThunderStrength(1.0F) > 0.9, i.e.
 * thunder_strength * rain_strength > 0.9 (thunder is WEIGHTED by rain
 * strength -- see World.getWeightedThunderStrength [740c583]). */
bool server_weather_is_thundering(const struct server_local* s);

/* Beta World.canBlockSeeTheSky(x,y,z): y >= the column heightmap (nothing
 * opaque above). Shared by canBlockBeRainedOn below and BlockFire's
 * rain-extinguish branch. */
bool world_can_block_see_sky(struct server_local* s, w_coord_t x, w_coord_t y,
							 w_coord_t z);
/* Beta Entity.java:535-537 isWet: inWater OR rain at floored entity position. */
bool server_entity_is_wet(struct server_local* s, bool in_water,
	double x, double y, double z);
/* Read-only diagnostic decomposition of the exact rain-exposure query. The
 * decision comes from world_can_block_be_rained_on; the other outputs expose
 * the same heightmap and biome inputs without changing the simulation path. */
bool server_weather_exposure_diagnostic(struct server_local* s, w_coord_t x,
	w_coord_t y, w_coord_t z, bool* sky, uint8_t* height, bool* biome_rains);
/* Beta World.canBlockBeRainedOn(x,y,z) [740c583]: isRaining() &&
 * canBlockSeeTheSky(x,y,z) && findTopSolidBlock(x,z) <= y && the biome allows
 * it (getEnableSnow() ? false : canSpawnLightningBolt(), which itself reduces
 * to `!enableSnow && enableRain`). Returns false when the target chunk/biome
 * data is not resident (fail-closed, matching "not rained on" for an
 * out-of-range query).
 * F1 (adversarial review) VERIFIED MATCH: the decompile's logic is
 *     canBlockBeRainedOn(x, y, z):
 *         not raining -> false;
 *         otherwise, the cell cannot see the sky -> false;
 *         otherwise, the top solid block at (x, z) above y -> false;
 *         otherwise, with the biome at (x, z):
 *             a snow-enabled biome -> false,
 *             else the biome's canSpawnLightningBolt answer.
 *
 *
 * The implementation below (server_weather.c) runs all four checks in this
 * SAME order, with the biome substitution proven algebraically equal
 * (`weather_biome_enable_snow`/`weather_biome_enable_rain`, server_weather.c)
 * -- this is the ONE World.canBlockBeRainedOn used by every caller in this
 * slice (the extinguish branch, the NEW ignition-loop gate below (F1), the
 * lightning strike-position filter, and BlockFarmland's hydration branch). */
bool world_can_block_be_rained_on(struct server_local* s, w_coord_t x,
								  w_coord_t y, w_coord_t z);

/* Beta EntityLightningBolt's CONSTRUCTOR ONLY [740c583]: places fire at the
 * strike cell + up to 4 random neighbours (gated on the target chunk being
 * loaded -- the doChunksNearChunkExist(10) analogue -- and
 * Block.fire.canPlaceBlockAt, and on the Beta `difficultySetting >= 2`
 * gate read from server_local_difficulty -- below Normal the whole fire
 * block and its draws are skipped), rolls
 * boltLivingTime = rand.nextInt(3)+1, and appends the new bolt (lightning-
 * State=2) to `s->weather.active_bolts`. Does NOT touch entities or play any
 * sound -- exactly like Beta's constructor, which does neither; both happen
 * on the bolt's first onUpdate via server_weather_tick_lightning_bolts
 * below (adversarial review F2: a single synchronous strike undercounts
 * Beta's real multi-tick/restrike lifecycle). */
void world_lightning_bolt_spawn(struct server_local* s, w_coord_t x,
								w_coord_t y, w_coord_t z);

/* Beta EntityLightningBolt.onUpdate [740c583], run once per server tick for
 * EVERY active bolt in `s->weather.active_bolts` (an explicit small list is
 * the F2-sanctioned architecture for a shared-struct, non-generic-entity-list
 * engine). Per bolt: the sound pair fires exactly once, on the tick
 * lightningState is still 2 (i.e. the bolt's first tick); lightningState then
 * decrements; going negative either kills the bolt (boltLivingTime==0) or
 * rolls a restrike (`lightningState < -rand.nextInt(10)`) that consumes one
 * boltLivingTime, resets lightningState to 1, and places ONE more fire
 * attempt at the (fixed) strike position; lightningState>=0 (after the
 * decrement, on any tick including a restrike tick) re-runs the full
 * struck-entity AABB pass (damage/ignite every entity in the 7x9x7 AABB,
 * arm a creeper's charged flag, convert a struck pig to a zombie pigman).
 * Dead bolts are removed from the list before returning. */
void server_weather_tick_lightning_bolts(struct server_local* s);

/* Free `s->weather.active_bolts` (world teardown). */
void server_weather_bolts_destroy(struct server_local* s);

/* ===== A1c world-explicit API (split-dimensions-plan-0924, "A2: A1c") =====
 * Every query/pass above reads ONE explicit `struct server_world* w`; the
 * `(s, ...)` names above are the inventoried LEGACY slot-0 adapters
 * (`_w(s, &s->worlds[0], ...)`), byte-identical to their pre-A1c bodies.
 *
 * WEATHER STATE CONTRACT (for lane C, lane B): the rain/thunder timers,
 * strengths, flash, position LCG, sound counter and the bolt list live in
 * `s->weather`, ONE per session, and belong to the session's SKY world (the
 * world whose dimension is not the Nether). This IS Beta's observable model:
 * the Nether WorldServer's own weather fields never ramp because its
 * updateWeather is gated on hasNoSky, so "no weather in the Nether" and "one
 * weather state per save" hold in both engines. Consequences:
 *   - server_weather_tick_w / server_weather_effects_tick_w on a Nether `w`
 *     are inert (no draw, no mutation), exactly like Beta's gate;
 *   - there is exactly one sky world per session; the A2 scheduler ticks it
 *     once per server tick (Overworld -> Nether order), so the timers advance
 *     once per tick as before;
 *   - a bolt is stamped with the world it struck in; the sweep for `w` ticks
 *     only its own bolts and drops bolts of a world that is no longer live.
 * A world that is not live (server_local_world_live false) makes every `_w`
 * entry point a no-op / fail-closed false. */
void server_weather_tick_w(struct server_local* s, struct server_world* w);
void server_weather_effects_tick_w(struct server_local* s,
								   struct server_world* w, w_coord_t px,
								   w_coord_t pz);
/* G9: the rolls alone (no bolt sweep); false if the world takes none. */
bool server_weather_effects_rolls_w(struct server_local* s,
									struct server_world* w, w_coord_t px,
									w_coord_t pz);
bool world_can_block_see_sky_w(struct server_local* s, struct server_world* w,
							   w_coord_t x, w_coord_t y, w_coord_t z);
bool server_entity_is_wet_w(struct server_local* s, struct server_world* w,
							bool in_water, double x, double y, double z);
bool server_weather_exposure_diagnostic_w(struct server_local* s,
										  struct server_world* w, w_coord_t x,
										  w_coord_t y, w_coord_t z, bool* sky,
										  uint8_t* height, bool* biome_rains);
bool world_can_block_be_rained_on_w(struct server_local* s,
									struct server_world* w, w_coord_t x,
									w_coord_t y, w_coord_t z);
void world_lightning_bolt_spawn_w(struct server_local* s,
								  struct server_world* w, w_coord_t x,
								  w_coord_t y, w_coord_t z);
void server_weather_tick_lightning_bolts_w(struct server_local* s,
										   struct server_world* w);

#endif
