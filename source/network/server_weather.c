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

// Beta 1.7.3 weather (World.updateWeather / World.updateBlocksAndPlayCaveSounds
// / World.canBlockBeRainedOn / World.findTopSolidBlock, WorldInfo, BlockFire's
// rain branches, EntityLightningBolt, EntityCreeper/EntityPig.onStruckByLightning
// [decompile 740c583]), faithfully transcribed. See WEATHER_SLICE_NOTES.md for
// the full citation list and the documented simplifications:
//  - the 80-random-tick tail of updateBlocksAndPlayCaveSounds is intentionally
//    NOT reproduced here (server_world_random_tick already owns ordinary block
//    random ticks as a separate, tested, T2-tunable mechanism);
//  - EntityLightningBolt's FULL multi-tick lightningState/boltLivingTime
//    lifecycle IS reproduced (post-review F2) via `s->weather.active_bolts`,
//    an explicit small list ticked once per server tick
//    (server_weather_tick_lightning_bolts) -- restrikes and their extra fire
//    placements + repeated struck-entity passes all happen exactly as Beta
//    deals them;
//  - the RNG faithfulness bar is MECHANISM (same gates/constants/draw order),
//    not byte-stream equality with java.util.Random -- see AGENTS.md's
//    rand_gen_* caveat, already the engine-wide convention.

#include <assert.h>
#include <math.h>

#include "server_weather.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)
#include "server_world.h"
#include "client_interface.h"
#include "../block/aabb.h"
#include "../block/blocks.h"
#include "../block/blocks_data.h"
#include "../chunk.h"
#include "../entity/combat.h"
#include "../entity/entity_monster.h"
#include "../entity/entity_boat.h" // BOAT_DESTROY_DAMAGE_THRESHOLD
#include "../entity/entity_minecart.h" // entity_minecart_mark_snapshot_dirty
#include "../entity/entity_painting.h" // entity_painting_hit_box / mark_broken
#include "../game/difficulty.h" // W1F: lightning fire gate
#include "../worldgen/worldgen.h"
#include "../platform/audio.h"
#include "../util.h"
#include "../world.h"

// --- BlockFire.canPlaceBlockAt export (block_fire.c) -------------------------
bool block_fire_can_place_at(struct server_local* s, w_coord_t x, w_coord_t y,
							 w_coord_t z);

// --- World construction: field_9437_g / soundCounter seeding (F3) -----------

void server_weather_seed(struct server_local* s) {
	assert(s);
	// [740c583: World, all three ctor overloads] field_9437_g starts
	// with an unbounded 32-bit random draw; rand_gen()
	// is the direct analogue (rand_gen_int's *bounded* nextInt(n) wrapper
	// would not match the unbounded-range call here).
	s->weather.pos_lcg = rand_gen(&s->rand_src);
	// soundCounter starts with a random value below 12000, matching
	// rand_gen_int exactly.
	s->weather.sound_counter = rand_gen_int(&s->rand_src, 12000);

	// debug.weather_force lever (see server_local_state.h): applied AFTER
	// the faithful seeding so the RNG draws above stay byte-identical; only
	// the resulting state/timers are overridden, and only when armed.
	if(s->weather.debug_force != 0) {
		s->weather.raining = true;
		s->weather.rain_time = 24000; // ~20min hold
		if(s->weather.debug_force == 2) {
			s->weather.thundering = true;
			s->weather.thunder_time = 24000;
		}
	}
}

/* A1c: slot 0 is NEVER gated -- the legacy (s) adapters must run exactly as
 * they did before A1c on every fixture, including hand-built sessions whose
 * slot 0 has no generation/chunks yet. Any other slot must be live. */
static bool weather_world_ok(const struct server_local* s,
							 const struct server_world* w) {
	return w && (w == &s->worlds[0] || server_local_world_live(w));
}

// --- updateWeather -----------------------------------------------------------

void server_weather_tick(struct server_local* s) {
	server_weather_tick_w(s, &s->worlds[0]);
}

void server_weather_tick_w(struct server_local* s, struct server_world* w) {
	assert(s);
	if(!weather_world_ok(s, w))
		return;

	// Beta skips weather entirely in worlds without sky, remaining inert in
	// the Nether (weather never runs; strengths/timers stay frozen at
	// whatever they last held, matching Beta leaving them untouched).
	// A1c: the gate reads the EXPLICIT world; the state stays the session's
	// one sky-world weather (server_weather.h, weather state contract).
	if(w->dimension == WORLD_DIM_NETHER)
		return;

	// field_27168_F-- FIRST, verbatim order. [740c583: World.updateWeather]
	if(s->weather.flash_ticks > 0)
		s->weather.flash_ticks--;

	int32_t thunder_time = s->weather.thunder_time;
	if(thunder_time <= 0) {
		s->weather.thunder_time = s->weather.thundering
			? rand_gen_int(&s->rand_src, 12000) + 3600
			: rand_gen_int(&s->rand_src, 168000) + 12000;
	} else {
		thunder_time--;
		s->weather.thunder_time = thunder_time;
		if(thunder_time <= 0)
			s->weather.thundering = !s->weather.thundering;
	}

	int32_t rain_time = s->weather.rain_time;
	if(rain_time <= 0) {
		s->weather.rain_time = s->weather.raining
			? rand_gen_int(&s->rand_src, 12000) + 12000
			: rand_gen_int(&s->rand_src, 168000) + 12000;
	} else {
		rain_time--;
		s->weather.rain_time = rain_time;
		if(rain_time <= 0)
			s->weather.raining = !s->weather.raining;
	}

	// F4 (adversarial review, New-Hire measured a systematic 1-ULP divergence
	// from tick 5 onward): Beta promotes to DOUBLE for the add, then narrows
	// back to float after adding 0.01 in double precision, rather than using a
	// plain `float += 0.01f`. The two are NOT interchangeable: 0.01 is not
	// exactly representable in either precision, and rounding
	// double(float_val)+0.01(double) then narrowing to float can differ from
	// float_val + 0.01f(float) by 1 ULP depending on float_val's exact bit
	// pattern. Replicate the exact promotion shape. [740c583: World.
	// updateWeather: rainingStrength changes by 0.01 in double precision,
	// then narrows to float, with subtraction on the dry branch and the
	// identical pair for thunderingStrength]
	s->weather.prev_rain_strength = s->weather.rain_strength;
	s->weather.rain_strength = s->weather.raining
		? (float)((double)s->weather.rain_strength + 0.01)
		: (float)((double)s->weather.rain_strength - 0.01);
	if(s->weather.rain_strength < 0.0f)
		s->weather.rain_strength = 0.0f;
	if(s->weather.rain_strength > 1.0f)
		s->weather.rain_strength = 1.0f;

	s->weather.prev_thunder_strength = s->weather.thunder_strength;
	s->weather.thunder_strength = s->weather.thundering
		? (float)((double)s->weather.thunder_strength + 0.01)
		: (float)((double)s->weather.thunder_strength - 0.01);
	if(s->weather.thunder_strength < 0.0f)
		s->weather.thunder_strength = 0.0f;
	if(s->weather.thunder_strength > 1.0f)
		s->weather.thunder_strength = 1.0f;
}

bool server_weather_is_raining(const struct server_local* s) {
	assert(s);
	// Beta World.isRaining(): getRainStrength(1.0F) > 0.2 -- at a full (1.0F)
	// interpolation factor getRainStrength collapses to the current (not
	// previous) strength: prev + (cur-prev)*1 == cur.
	return s->weather.rain_strength > 0.2f;
}

bool server_weather_is_thundering(const struct server_local* s) {
	assert(s);
	// Beta World.getIsThundering(): getWeightedThunderStrength(1.0F) > 0.9,
	// and getWeightedThunderStrength ITSELF multiplies by getRainStrength --
	// thunder is gated on rain being (nearly) fully ramped in too.
	return (s->weather.thunder_strength * s->weather.rain_strength) > 0.9f;
}

// --- Beta biome getEnableSnow / canSpawnLightningBolt ------------------------
// [740c583: BiomeGenBase] canSpawnLightningBolt() = enableSnow ? false :
// enableRain, and canBlockBeRainedOn's biome gate is itself
// `enableSnow ? false : canSpawnLightningBolt()` -- substituting collapses to
// exactly `!enableSnow && enableRain`. taiga/iceDesert/tundra setEnableSnow();
// desert/iceDesert setDisableRain(); every other overworld biome defaults both
// enableRain=true, enableSnow=false.
static bool weather_biome_enable_snow(enum biome b) {
	return b == BIOME_TAIGA || b == BIOME_ICE_DESERT || b == BIOME_TUNDRA;
}

static bool weather_biome_enable_rain(enum biome b) {
	return b != BIOME_DESERT && b != BIOME_ICE_DESERT;
}

bool world_can_block_see_sky(struct server_local* s, w_coord_t x, w_coord_t y,
							 w_coord_t z) {
	return world_can_block_see_sky_w(s, &s->worlds[0], x, y, z);
}

bool world_can_block_see_sky_w(struct server_local* s, struct server_world* w,
							   w_coord_t x, w_coord_t y, w_coord_t z) {
	assert(s);
	uint8_t height;
	if(!weather_world_ok(s, w) || !server_world_get_height(w, x, z, &height))
		return false; // chunk not resident: fail closed
	return y >= (w_coord_t)height;
}

bool server_entity_is_wet(struct server_local* s, bool in_water,
	double x, double y, double z) {
	return server_entity_is_wet_w(s, &s->worlds[0], in_water, x, y, z);
}

bool server_entity_is_wet_w(struct server_local* s, struct server_world* w,
							bool in_water, double x, double y, double z) {
	// Beta Entity.java:535-537: isWet uses floor(posX/Y/Z), not the body AABB.
	return in_water || world_can_block_be_rained_on_w(s, w,
		(w_coord_t)floor(x), (w_coord_t)floor(y), (w_coord_t)floor(z));
}

bool server_weather_exposure_diagnostic(struct server_local* s, w_coord_t x,
	w_coord_t y, w_coord_t z, bool* sky, uint8_t* height, bool* biome_rains) {
	return server_weather_exposure_diagnostic_w(s, &s->worlds[0], x, y, z, sky,
												height, biome_rains);
}

bool server_weather_exposure_diagnostic_w(struct server_local* s,
										  struct server_world* w, w_coord_t x,
										  w_coord_t y, w_coord_t z, bool* sky,
										  uint8_t* height, bool* biome_rains) {
	assert(s);
	uint8_t h = 0;
	bool live = weather_world_ok(s, w);
	bool has_height = live && server_world_get_height(w, x, z, &h);
	bool can_rain = false;
	if(live && w->generator) {
		enum biome b = worldgen_biome_at(w->generator, x, z);
		can_rain = !weather_biome_enable_snow(b)
			&& weather_biome_enable_rain(b);
	}
	if(sky)
		*sky = has_height && y >= (w_coord_t)h;
	if(height)
		*height = has_height ? h : 0;
	if(biome_rains)
		*biome_rains = can_rain;
	return world_can_block_be_rained_on_w(s, w, x, y, z);
}

bool world_can_block_be_rained_on(struct server_local* s, w_coord_t x,
								  w_coord_t y, w_coord_t z) {
	return world_can_block_be_rained_on_w(s, &s->worlds[0], x, y, z);
}

bool world_can_block_be_rained_on_w(struct server_local* s,
									struct server_world* w, w_coord_t x,
									w_coord_t y, w_coord_t z) {
	assert(s);
	if(!weather_world_ok(s, w))
		return false; // no usable world: nothing is rained on (fail closed)
	if(!server_weather_is_raining(s))
		return false;
	if(!world_can_block_see_sky_w(s, w, x, y, z))
		return false;
	// Beta re-derives findTopSolidBlock(x,z) > y as a SEPARATE check; our
	// engine's heightmap already backs both canBlockSeeTheSky and
	// findTopSolidBlock (server_world_get_height's own doc comment), so this
	// is a no-op restatement of the check just above -- kept only for
	// 1:1 structural fidelity with the decompile.
	uint8_t height;
	if(!server_world_get_height(w, x, z, &height))
		return false;
	if((w_coord_t)height > y)
		return false;
	if(!w->generator)
		return false; // no worldgen attached (e.g. a bare test world): fail closed
	/* Beta World.java:2117-2126 asks the world's own chunk manager: the Hell
	 * provider (WorldProviderHell.java:5, WorldChunkManagerHell.java:20-22)
	 * answers BiomeGenBase.hell, whose rain is disabled (BiomeGenBase.java:19,
	 * :136-138). The Overworld climate must never be sampled for the Nether. */
	if(w->dimension == WORLD_DIM_NETHER)
		return false;
	enum biome b = worldgen_biome_at(w->generator, x, z);
	if(weather_biome_enable_snow(b))
		return false;
	return weather_biome_enable_rain(b);
}

// Beta World.findTopSolidBlock(x,z): scans y=127..1 for the first solid/liquid
// material and returns y+1, or -1 if the column is entirely non-solid. Our
// heightmap ("top non-transparent block + 1") already IS this value for
// ordinary terrain -- see server_world_get_height's doc comment, the same
// primitive world_can_block_be_rained_on uses above. Returns -1 (Beta's own
// not-found sentinel) when the chunk is not resident.
static int32_t world_find_top_solid_block(struct server_world* w, w_coord_t x,
										   w_coord_t z) {
	uint8_t height;
	if(!server_world_get_height(w, x, z, &height))
		return -1;
	return (int32_t)height;
}

// Beta World.getFullBlockLightValue [740c583 World.java:521-530]: Chunk.getBlockLightValue with
// subtract 0 -- max(RAW skylight, blocklight), no time-of-day or weather adjustment (the same raw
// read EntityAnimal's spawn gate uses; server_local.c animal_spawn_spot_ok). next-gaps-0924/F1:
// this used to subtract the day/night value; outcome-identical here (the cave-sound gate also
// requires raw sky light <= 0, where both reads equal the block light, and the nextInt(8) draw
// happens either way), now literal Beta.
static int world_full_block_light(struct server_world* w, w_coord_t x,
								  w_coord_t y, w_coord_t z) {
	struct block_data blk;
	if(!server_world_get_block(w, x, y, z, &blk))
		return 15; // unloaded: treat as lit (never a cave-sound candidate)
	return ((int)blk.sky_light > (int)blk.torch_light) ? (int)blk.sky_light
													  : (int)blk.torch_light;
}

// --- EntityLightningBolt: the FULL multi-tick lifecycle (F2, adversarial
// review) -- constructor (world_lightning_bolt_spawn), then onUpdate ticked
// once per active bolt per server tick (server_weather_tick_lightning_bolts).
// [740c583: EntityLightningBolt] ---------------------------------------------

static void lightning_try_place_fire(struct server_local* s,
									 struct server_world* w, w_coord_t x,
									 w_coord_t y, w_coord_t z) {
	struct block_data here;
	if(!server_world_get_block(w, x, y, z, &here))
		return; // chunk not resident: Beta's doChunksNearChunkExist analogue
	/* A1c: block_fire_can_place_at is a block CALLBACK -- under the A1a
	 * bridge (D4) callbacks read slot 0 only, so a strike in any other world
	 * cannot evaluate Block.fire.canPlaceBlockAt yet and places no fire (the
	 * documented D4 activation blocker; the rain/thunder draws around it are
	 * unchanged). Lane C's #2 row inherits this site with the world threaded. */
	if(here.type == BLOCK_AIR && w == &s->worlds[0]
	   && block_fire_can_place_at(s, x, y, z))
		server_world_set_block(w, x, y, z,
							   (struct block_data) {.type = BLOCK_FIRE,
													.torch_light = 15});
}

struct lightning_pig_conversion {
	uint32_t pig_id;
	vec3 pos;
	float yaw, pitch;
};

// Beta EntityLightningBolt.onUpdate's struck-entity pass, run once per active
// tick (the initial 2 ticks AND every restrike's 2 ticks -- see
// server_weather_tick_lightning_bolts): every entity within the 3.0-block
// XZ / -3..+9 Y AABB centered on the bolt's (fixed) position gets
// onStruckByLightning. [740c583: EntityLightningBolt.onUpdate,
// `getEntitiesWithinAABBExcludingEntity` + the per-entity dispatch loop]
/* Entity.onStruckByLightning's raw dealFireDamage(5) [Entity.java:1045] for
 * NON-living victims: EntityItem health is Beta's own int (5 at spawn,
 * entity_item.c), and the vehicles scale it x10 themselves. (Players/mobs
 * take LIGHTNING_STRIKE_DAMAGE, the same 5 in the engine's health units.) */
#define LIGHTNING_ITEM_DAMAGE 5

/* Beta World.getEntitiesWithinAABBExcludingEntity selects by
 * `entity.boundingBox.intersectsWith(box)` (Chunk.getEntitiesWithinAABB-
 * ForEntity :500; AxisAlignedBB.intersectsWith :185 is STRICT), not by the
 * entity's position point. */
static bool lightning_box_hits(const struct AABB* entity_box, float xmin,
							   float xmax, float ymin, float ymax, float zmin,
							   float zmax) {
	struct AABB bolt = {.x1 = xmin, .y1 = ymin, .z1 = zmin,
						.x2 = xmax, .y2 = ymax, .z2 = zmax};
	struct AABB e = *entity_box;
	return aabb_intersection_strict(&e, &bolt);
}

/* Beta minecart damage gains ten times the hit amount, saturated on this engine's int16
 * (the explosion pass's expl_vehicle_damage rule). */
static int16_t lightning_vehicle_damage(int16_t current, int damage) {
	int32_t next = (int32_t)current + damage * 10;
	if(next > INT16_MAX)
		next = INT16_MAX;
	return (int16_t)next;
}

static void lightning_entity_pass(struct server_local* s,
								  struct server_world* w, w_coord_t x,
								  w_coord_t y, w_coord_t z) {
	float xmin = (float)x - 3.0f, xmax = (float)x + 3.0f;
	float ymin = (float)y - 3.0f, ymax = (float)y + 9.0f;
	float zmin = (float)z - 3.0f, zmax = (float)z + 3.0f;

	// Players. lane C #2: every eligible actor committed to the struck world
	// -- host first, then the guest (server_local_eligible_actors_w, A1c) --
	// is a candidate, exactly as Beta's getEntitiesWithinAABBExcludingEntity
	// scans the bolt's OWN world [EntityLightningBolt.java:62-69; box
	// posX+-3, posY-3..posY+6+3, posZ+-3], by the player's BOUNDING BOX
	// (0.6 x 1.8 from posY - 1.62, Entity.setPosition; strict intersection).
	// An actor in another world is never struck. Each hit is
	// Entity.onStruckByLightning [Entity.java:1044-1051]: dealFireDamage(5);
	// ++fire; if(fire == 0) fire = 300 -- the fire bump whatever the damage
	// outcome (a body the hit killed stays a dead body until its respawn).
	// Players are never fire-immune; no knockback (null attacker).
	struct server_player* struck[SERVER_LOCAL_MAX_PRESENT_ACTORS];
	size_t struck_n = server_local_eligible_actors_w(
		s, w, struck, SERVER_LOCAL_MAX_PRESENT_ACTORS);
	for(size_t i = 0; i < struck_n; i++) {
		struct server_player* p = struck[i];
		struct AABB pbox;
		aabb_setsize_centered(&pbox, 0.6F, 1.8F, 0.6F);
		aabb_translate(&pbox, (float)p->x,
					   (float)p->y - PLAYER_EYE_HEIGHT + 0.9F, (float)p->z);
		if(!lightning_box_hits(&pbox, xmin, xmax, ymin, ymax, zmin, zmax))
			continue;
		(void)server_local_damage_actor(s, p, LIGHTNING_STRIKE_DAMAGE, 0.0f,
										0.0f);
		p->fire++;
		if(p->fire == 0)
			p->fire = 300;
	}

	// Monsters. Two-phase: scan + queue pig conversions, THEN apply them --
	// server_local_spawn_monster_at/erase must not run while iterating
	// s->entities (the established deferred-mutation pattern; see
	// server_local_spawn_arrow's own comment on the same hazard). F5
	// (adversarial review): the queue is a realloc-grown DYNAMIC array, not a
	// fixed-size cap -- house rule, no silently dropping a struck pig past an
	// arbitrary count. Beta's own iteration order is the entity list's
	// insertion order, which this engine cannot reproduce exactly (a hash
	// dict, not a Java ArrayList) -- but the pig-conversion path draws NO RNG
	// (EntityPig.onStruckByLightning has none), so iteration order has no
	// observable effect on the RNG stream; only WHICH pigs convert matters,
	// and every struck pig converts regardless of order or count.
	struct lightning_pig_conversion* pig_queue = NULL;
	size_t pig_queue_n = 0, pig_queue_cap = 0;

	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		/* A1c: only the entities bound to the struck world; Beta selects by
		 * the entity's bounding box (strict). */
		struct AABB ebox;
		bool in_box = e && e->getBoundingBox
			&& server_local_entity_world(s, e) == w
			&& e->getBoundingBox(e, &ebox) != 0
			&& lightning_box_hits(&ebox, xmin, xmax, ymin, ymax, zmin, zmax);
		/* Paintings keep e->getBoundingBox NULL (Beta's collision accessor
		 * is null for them) but DO have Entity.boundingBox, the frame box
		 * the bolt's AABB scan selects by. Entity.dealFireDamage :521-525
		 * (not fire-immune) -> EntityPainting.attackEntityFrom :200-208:
		 * dies and drops the painting item. */
		struct AABB pbox;
		if(e && e->type == ENTITY_PAINTING
		   && server_local_entity_world(s, e) == w
		   && entity_painting_hit_box(e, &pbox)
		   && lightning_box_hits(&pbox, xmin, xmax, ymin, ymax, zmin, zmax))
			entity_painting_mark_broken(e);
		if(in_box && e->type == ENTITY_ITEM && e->health > 0) {
			// EntityItem.dealFireDamage :83-85 ALWAYS calls
			// attackEntityFrom(null, 5) (no fire-immunity test);
			// attackEntityFrom :87-93: health -= 5, <= 0 -> setEntityDead.
			// A fresh item has health 5, so a strike destroys it; the item's
			// own tick removes it (the same death edge fire/cactus use).
			e->health -= LIGHTNING_ITEM_DAMAGE;
		} else if(in_box && e->type == ENTITY_MINECART) {
			// Entity.dealFireDamage :521-525 -> EntityMinecart.
			// attackEntityFrom :73-79: rock flip, 10 shake ticks, damage +=
			// 5 * 10 > 40 -> destroyed with its drops. The break cannot run
			// inside this dict walk, so it is latched exactly like the
			// explosion pass (server_world.c expl_hit_entity) and consumed at
			// the top of the cart's own next tick.
			e->data.minecart.shake_dir = (int8_t)-e->data.minecart.shake_dir;
			e->data.minecart.shake_ticks = 10;
			e->data.minecart.damage_taken = lightning_vehicle_damage(
				e->data.minecart.damage_taken, LIGHTNING_ITEM_DAMAGE);
			if(e->data.minecart.damage_taken > 40)
				e->data.minecart.blast_break_pending = true;
			entity_minecart_mark_snapshot_dirty(e, true);
		} else if(in_box && e->type == ENTITY_BOAT) {
			// Entity.dealFireDamage -> EntityBoat.attackEntityFrom :63-69:
			// the same rock/shake/damage*10 > 40 break, latched likewise.
			e->data.boat.shake_dir = (int8_t)-e->data.boat.shake_dir;
			e->data.boat.shake_ticks = 10;
			e->data.boat.damage_taken = lightning_vehicle_damage(
				e->data.boat.damage_taken, LIGHTNING_ITEM_DAMAGE);
			if(e->data.boat.damage_taken > BOAT_DESTROY_DAMAGE_THRESHOLD)
				e->data.boat.blast_break_pending = true;
		}
		/* Everything else Beta strikes is a no-op there too: arrows, primed
		 * TNT, falling sand, fish hooks keep Entity.attackEntityFrom's
		 * default (setBeenAttacked only, Entity.java:705-708). Paintings are
		 * handled above through their frame box. */
		if(in_box && e->type == ENTITY_MONSTER && e->health > 0) {
			if(e->data.monster.id == MONSTER_PIG) {
				// EntityPig.onStruckByLightning: no super call (no damage/fire
				// to the pig) -- spawn a zombie pigman at the pig's exact
				// position/orientation and mark this pig for removal.
				if(pig_queue_n >= pig_queue_cap) {
					size_t new_cap = pig_queue_cap ? pig_queue_cap * 2 : 4;
					struct lightning_pig_conversion* grown = realloc(
						pig_queue, new_cap * sizeof(*pig_queue));
					if(grown) {
						pig_queue = grown;
						pig_queue_cap = new_cap;
					}
					// OOM: this one pig's conversion is skipped (not a silent
					// CAP -- an actual allocation failure, logged the same way
					// every other OOM path in this codebase degrades).
					// Everything else in this pass is unaffected.
				}
				if(pig_queue_n < pig_queue_cap) {
					pig_queue[pig_queue_n].pig_id = e->id;
					glm_vec3_copy(e->pos, pig_queue[pig_queue_n].pos);
					pig_queue[pig_queue_n].yaw = e->orient[0];
					pig_queue[pig_queue_n].pitch = e->orient[1];
					pig_queue_n++;
				} else {
					fprintf(stderr,
							"[weather] OOM growing lightning pig-conversion "
							"queue -- pig id %u not converted this strike\n",
							e->id);
				}
			} else {
				// Entity.onStruckByLightning via the wolf-aware environmental
				// path (dealFireDamage is a no-op for fire-immune mobs; the
				// fire counter still ticks either way, matching the base
				// Entity method's unconditional ++fire).
				if(!monster_def_get(e->data.monster.id)->fire_immune)
					(void)monster_apply_environment_damage(
						e, s, LIGHTNING_STRIKE_DAMAGE);
				e->data.monster.fire_time++;
				if(e->data.monster.fire_time == 0)
					e->data.monster.fire_time = 300;
				if(e->data.monster.id == MONSTER_CREEPER)
					// EntityCreeper.onStruckByLightning: super call (above)
					// THEN set "powered" permanently.
					e->data.monster.charged = true;
			}
		}
		dict_entity_next(it);
	}

	for(size_t i = 0; i < pig_queue_n; i++) {
		struct entity** slot = dict_entity_get(s->entities, pig_queue[i].pig_id);
		if(slot && *slot) {
			struct entity* pig = *slot;
			clin_rpc_send(&(struct client_rpc) {
				.type = CRPC_ENTITY_DESTROY,
				.payload.entity_destroy.entity_id = pig->id,
			});
			free(pig);
			dict_entity_erase(s->entities, pig_queue[i].pig_id);
		}
		struct entity* pigman = server_local_spawn_monster_at_w(
			w, pig_queue[i].pos, MONSTER_PIG_ZOMBIE);
		if(pigman) {
			pigman->orient[0] = pig_queue[i].yaw;
			pigman->orient[1] = pig_queue[i].pitch;
		}
	}
	free(pig_queue);
}

// Append `bolt` to s->weather.active_bolts, growing the array as needed
// (realloc, no fixed cap -- see the struct's own doc comment in
// server_weather.h). Returns false only on genuine OOM.
static bool active_bolts_push(struct server_local* s,
							  struct active_lightning_bolt bolt) {
	if(s->weather.active_bolts_count >= s->weather.active_bolts_cap) {
		size_t new_cap = s->weather.active_bolts_cap
			? s->weather.active_bolts_cap * 2
			: 4;
		struct active_lightning_bolt* grown = realloc(
			s->weather.active_bolts, new_cap * sizeof(*grown));
		if(!grown) {
			fprintf(stderr,
					"[weather] OOM growing active-bolts list -- this strike's "
					"multi-tick lifecycle is dropped (fire placement already "
					"landed; no restrikes/repeated entity hits for it)\n");
			return false;
		}
		s->weather.active_bolts = grown;
		s->weather.active_bolts_cap = new_cap;
	}
	s->weather.active_bolts[s->weather.active_bolts_count++] = bolt;
	return true;
}

void world_lightning_bolt_spawn(struct server_local* s, w_coord_t x,
								w_coord_t y, w_coord_t z) {
	world_lightning_bolt_spawn_w(s, &s->worlds[0], x, y, z);
}

void world_lightning_bolt_spawn_w(struct server_local* s,
								  struct server_world* w, w_coord_t x,
								  w_coord_t y, w_coord_t z) {
	assert(s);
	if(!weather_world_ok(s, w))
		return;

	/* CFR EntityLightningBolt.java:35-36 precedes the fire draws at :46-47.
	 * A bolt has no struct entity, but shares the entity lifetime-ID allocator.
	 * The render seed (field_27029_a) is not used by the simulation; it is
	 * only published to the client renderer below (CRPC_LIGHTNING_BOLT). */
	struct java_random rng = {0};
	entity_lifetime_rng_seed(&rng, w->seed, w->dimension, server_local_gen_id(s));
	const int64_t render_seed = jrand_next_long(&rng);
	int32_t bolt_living_time = jrand_next_int_bound(&rng, 3) + 1;

	// Fire placement. Beta gates the WHOLE block -- the strike cell and the
	// four offset attempts with their rand draws -- on
	// `difficultySetting >= 2 && doChunksNearChunkExist(...)`
	// [EntityLightningBolt.java:16]. The difficulty half is the latched world
	// difficulty (next-gaps-0924 W1F); lightning_try_place_fire itself no-ops
	// on an unloaded chunk, the faithful-enough analogue of
	// doChunksNearChunkExist(10).
	if(difficulty_lightning_ignites(server_local_difficulty(s))) {
		lightning_try_place_fire(s, w, x, y, z);
		for(int i = 0; i < 4; i++) {
			w_coord_t ox = x + jrand_next_int_bound(&rng, 3) - 1;
			w_coord_t oy = y + jrand_next_int_bound(&rng, 3) - 1;
			w_coord_t oz = z + jrand_next_int_bound(&rng, 3) - 1;
			lightning_try_place_fire(s, w, ox, oy, oz);
		}
	}

	// boltLivingTime starts with a bound-three draw plus one [740c583]. The
	// constructor does NOT touch entities or play any sound -- both happen on
	// the bolt's first onUpdate below.
	active_bolts_push(s, (struct active_lightning_bolt) {
		.x = x, .y = y, .z = z,
		.lightning_state = 2,
		.bolt_living_time = bolt_living_time,
		.gameplay_rng = rng,
		.world_slot = w->world_index,
		.world_generation = w->generation,
	});

	/* Client render bolt: the same RenderLightningBolt mesh + sky flash an
	 * online Packet71 produces (graphics/render_lightning.h). Sends the
	 * post-constructor render state so the client's onUpdate replay
	 * (restrikes, new render seeds) matches this bolt's. Render-only: no
	 * simulation value reads it. Same world gate + stamped send as the
	 * thunder sound (server_local_emit_sound_w). */
	if(server_local_world_fx_ok(w)) {
		const uint64_t seed_bits = (uint64_t)render_seed;
		struct client_rpc call = {
			.type = CRPC_LIGHTNING_BOLT,
			.payload.lightning_bolt = {
				.x = (int32_t)x, .y = (int32_t)y, .z = (int32_t)z,
				.living_time = bolt_living_time,
				.render_seed_hi = (uint32_t)(seed_bits >> 32),
				.render_seed_lo = (uint32_t)seed_bits,
				.rng_state_hi = (uint32_t)(rng.seed >> 32),
				.rng_state_lo = (uint32_t)rng.seed,
				.dimension = (int32_t)w->dimension,
			},
		};
		clin_rpc_stamp(&call, w->world_index, w->generation);
		clin_rpc_send(&call);
	}
}

void server_weather_tick_lightning_bolts(struct server_local* s) {
	server_weather_tick_lightning_bolts_w(s, &s->worlds[0]);
}

void server_weather_tick_lightning_bolts_w(struct server_local* s,
										   struct server_world* w) {
	assert(s);
	if(!weather_world_ok(s, w))
		return;

	size_t write = 0;
	for(size_t i = 0; i < s->weather.active_bolts_count; i++) {
		struct active_lightning_bolt* b = &s->weather.active_bolts[i];
		bool dead = false;
		/* A1c: this sweep ticks only the bolts that struck in `w`. A bolt
		 * whose world is gone (slot re-created or discarded) is dropped; a
		 * bolt of another live world is kept untouched for its own sweep. */
		if(b->world_slot != w->world_index
		   || b->world_generation != w->generation) {
			struct server_world_handle owner
				= {.world_index = b->world_slot,
				   .generation = b->world_generation};
			if(server_local_world_from_handle(s, owner))
				s->weather.active_bolts[write++] = *b;
			continue;
		}

		// State 2 triggers the initial sound pair, which fires on
		// exactly the bolt's first onUpdate call (state is only ever 2 at
		// spawn or... it never returns to 2 again; a restrike resets it to 1,
		// not 2, so the sound plays exactly ONCE per bolt, matching Beta).
		if(b->lightning_state == 2) {
			/* CFR :56-57: preserve the two pitch draws in the bolt stream.
			 * Actual pitches retain the documented S0 cosmetic sound stream. */
			(void)jrand_next_float(&b->gameplay_rng);
			(void)jrand_next_float(&b->gameplay_rng);
			struct random_gen* sg = server_local_sound_rng();
			vec3 at = {(float)b->x, (float)b->y, (float)b->z};
			server_local_emit_sound_w(w, SOUND_AMBIENT_WEATHER_THUNDER, at, 10000.0f,
									0.8f + rand_gen_flt(sg) * 0.2f);
			server_local_emit_sound_w(w, SOUND_RANDOM_EXPLODE, at, 2.0f,
									0.5f + rand_gen_flt(sg) * 0.2f);
		}

		// Decrement the lightning state before testing expiry.
		b->lightning_state--;

		if(b->lightning_state < 0) {
			if(b->bolt_living_time == 0) {
				// The bolt has expired; mark it dead.
				dead = true;
			} else if(b->lightning_state < -jrand_next_int_bound(&b->gameplay_rng, 10)) {
				// Restrike: consume one boltLivingTime, reset lightningState
				// to 1 (NOT 2 -- the sound does not replay), and place ONE
				// more fire attempt at the bolt's fixed position (unlike the
				// constructor's 5 attempts).
				b->bolt_living_time--;
				b->lightning_state = 1;
				(void)jrand_next_long(&b->gameplay_rng); // CFR :69: new render seed
				lightning_try_place_fire(s, w, b->x, b->y, b->z);
			}
			// else: still waiting to restrike or die -- do nothing this tick,
			// lightningState stays negative (more negative next tick).
		}

		if(!dead && b->lightning_state >= 0) {
			// Repeated struck-entity pass -- Beta re-runs the FULL AABB scan
			// every tick the bolt is active, including the tick a restrike
			// just landed (the restrike branch above falls through into this
			// check with the freshly-reset lightningState=1).
			lightning_entity_pass(s, w, b->x, b->y, b->z);
			// field_27168_F ("a bolt just struck" flash) is set by the
			// SPAWNER (weather_effects_for_chunk), matching Beta's own
			// call site (updateBlocksAndPlayCaveSounds, not onUpdate). Beta's
			// SEPARATE field_27172_i (set by the bolt itself, every active
			// tick) is render-only telemetry this slice does not reproduce
			// (see WEATHER_SLICE_NOTES.md).
		}

		if(!dead)
			s->weather.active_bolts[write++] = *b;
	}
	s->weather.active_bolts_count = write;
}

void server_weather_bolts_destroy(struct server_local* s) {
	assert(s);
	free(s->weather.active_bolts);
	s->weather.active_bolts = NULL;
	s->weather.active_bolts_count = 0;
	s->weather.active_bolts_cap = 0;
}

// --- updateBlocksAndPlayCaveSounds's cave-sound / lightning / snow-freeze ----
// branches, sharing the persistent position LCG exactly as Beta interleaves
// them. The trailing 80-random-tick loop is deliberately NOT reproduced (see
// file header). [740c583: World.updateBlocksAndPlayCaveSounds]

// F6(c) (adversarial review): field_9437_g is a signed Java `int`, and Java's
// `>>` is ARITHMETIC (sign-extending) for a negative operand -- our `pos_lcg`
// is `uint32_t`, so `pos_lcg >> 2` is LOGICAL (zero-filling) in C. This is
// NOT a faithfulness gap: every consumer below only ever reads bits 0-22 of
// the ALREADY-shifted value (`v & 15` = bits 0-3, `(v>>8) & 15` = bits 8-11,
// `(v>>16) & 127` = bits 16-22, all of `v = pos_lcg >> 2`, i.e. bits 2-24 of
// the original 32-bit `pos_lcg`). Arithmetic vs. logical right-shift differ
// ONLY in what fills in from the TOP (bit 31 downward, sign-extended vs.
// zero-filled) -- a difference that only reaches bit 29 after a >>2 shift,
// nowhere near the bits 0-22 this code actually consumes. The two shift
// semantics are therefore bit-identical in every position we read.
static void weather_effects_for_chunk(struct server_local* s,
									  struct server_world* w, w_coord_t cx0,
									  w_coord_t cz0) {
	// Cave-sound branch: advances the LCG (and rolls) on EVERY position while
	// soundCounter==0 -- soundCounter itself decrements ONCE per tick, not
	// per-position (see server_weather_effects_tick). The sound trigger is a
	// documented no-op: SOUND_AMBIENT_CAVE is already driven client-side by
	// its own mood counter (client_display_tick.c) and has no server-side
	// equivalent to fire; only the LCG advance + gate draws are reproduced so
	// the shared position stream stays Beta-shaped for the branches below.
	if(s->weather.sound_counter == 0) {
		s->weather.pos_lcg = s->weather.pos_lcg * 3u + 1013904223u;
		uint32_t v = s->weather.pos_lcg >> 2;
		w_coord_t lx = (w_coord_t)(v & 15);
		w_coord_t lz = (w_coord_t)((v >> 8) & 15);
		w_coord_t ly = (w_coord_t)((v >> 16) & 127);
		w_coord_t wx = cx0 + lx, wz = cz0 + lz;
		struct block_data here;
		if(server_world_get_block(w, wx, ly, wz, &here)
		   && here.type == BLOCK_AIR
		   && world_full_block_light(w, wx, ly, wz)
				  <= rand_gen_int(&s->rand_src, 8)
		   && (int)here.sky_light <= 0) {
			/* A1c: Beta's getClosestPlayer(8.0) is a same-World query -- the
			 * host counts only while committed to `w` (lane C's #2 row widens
			 * this to every eligible actor of `w`). */
			bool host_here = server_local_actor_in_world(s, &s->player, w);
			float dx = s->player.x - ((float)wx + 0.5f);
			float dy = s->player.y - ((float)ly + 0.5f);
			float dz = s->player.z - ((float)wz + 0.5f);
			float dist_sq = dx * dx + dy * dy + dz * dz;
			if(host_here && dist_sq <= 64.0f && dist_sq > 4.0f) {
				// no-op: see comment above.
				s->weather.sound_counter = rand_gen_int(&s->rand_src, 12000) + 6000;
			}
		}
	}

	// Lightning branch.
	if(rand_gen_int(&s->rand_src, 100000) == 0 && server_weather_is_raining(s)
	   && server_weather_is_thundering(s)) {
		s->weather.pos_lcg = s->weather.pos_lcg * 3u + 1013904223u;
		uint32_t v = s->weather.pos_lcg >> 2;
		w_coord_t wx = cx0 + (w_coord_t)(v & 15);
		w_coord_t wz = cz0 + (w_coord_t)((v >> 8) & 15);
		int32_t wy = world_find_top_solid_block(w, wx, wz);
		if(wy >= 0 && world_can_block_be_rained_on_w(s, w, wx, wy, wz)) {
			// F2 (adversarial review): spawn the bolt into the multi-tick
			// active list (world_lightning_bolt_spawn is the CONSTRUCTOR
			// only -- fire placement + boltLivingTime roll); the struck-
			// entity effects and restrikes are delivered by
			// server_weather_tick_lightning_bolts on this and every later
			// tick the bolt stays active, not synchronously here.
			world_lightning_bolt_spawn_w(s, w, wx, wy, wz);
			// field_27168_F: set at the SAME call site Beta sets it --
			// updateBlocksAndPlayCaveSounds, right after addWeatherEffect --
			// not inside the bolt's own onUpdate. [740c583]
			s->weather.flash_ticks = 2;
		}
	}

	// Snowfall / water-freeze branch.
	if(rand_gen_int(&s->rand_src, 16) == 0) {
		s->weather.pos_lcg = s->weather.pos_lcg * 3u + 1013904223u;
		uint32_t v = s->weather.pos_lcg >> 2;
		w_coord_t lx = (w_coord_t)(v & 15);
		w_coord_t lz = (w_coord_t)((v >> 8) & 15);
		w_coord_t wx = cx0 + lx, wz = cz0 + lz;
		int32_t wy = world_find_top_solid_block(w, wx, wz);

		bool snow_ok = false;
		if(w->generator && wy >= 0 && wy < WORLD_HEIGHT) {
			enum biome b = worldgen_biome_at(w->generator, wx, wz);
			if(weather_biome_enable_snow(b)) {
				// BLOCK-light channel only, no day/night subtract (Beta:
				// chunk.getSavedLightValue(EnumSkyBlock.Block, ...) < 10).
				struct block_data at_top;
				if(server_world_get_block(w, wx, wy, wz, &at_top)
				   && (int)at_top.torch_light < 10)
					snow_ok = true;
			}
		}

		if(snow_ok) {
			struct block_data below, here;
			if(server_world_get_block(w, wx, wy - 1, wz, &below)
			   && server_world_get_block(w, wx, wy, wz, &here)) {
				// Beta: below != air, below != ice, and the outer material
				// must be solid [740c583: World.updateBlocksAndPlayCaveSounds],
				// PLUS Block.snow.canPlaceBlockAt's own opaque-cube + solid
				// gate. can_see_through is this engine's existing opacity
				// proxy (the same substitution block_snow.c's own
				// player-placement onItemPlace already makes for
				// isOpaqueCube -- kept consistent here rather than
				// introducing a second, differently-derived predicate).
				if(server_weather_is_raining(s) && here.type == BLOCK_AIR
				   && below.type != BLOCK_AIR && below.type != BLOCK_ICE
				   && block_beta_material_is_solid(below.type)
				   && blocks[below.type] && !blocks[below.type]->can_see_through)
					server_world_set_block(
						w, wx, wy, wz, (struct block_data) {.type = BLOCK_SNOW});
				if(below.type == BLOCK_WATER_STILL && below.metadata == 0)
					server_world_set_block(
						w, wx, wy - 1, wz,
						(struct block_data) {.type = BLOCK_ICE});
			}
		}
	}
}

void server_weather_effects_tick(struct server_local* s, w_coord_t px,
								 w_coord_t pz) {
	server_weather_effects_tick_w(s, &s->worlds[0], px, pz);
}

/* G9: the per-chunk rolls of World.updateBlocksAndPlayCaveSounds (part of
 * World.tick, before the light drain); false when the world takes none. The
 * bolt sweep is Beta updateEntities (weatherEffects, after the drain), so the
 * live tick calls server_weather_tick_lightning_bolts_w separately. */
bool server_weather_effects_rolls_w(struct server_local* s,
									struct server_world* w, w_coord_t px,
									w_coord_t pz) {
	assert(s);
	if(!weather_world_ok(s, w))
		return false;

	if(w->dimension == WORLD_DIM_NETHER)
		return false;
	if(server_world_light_halted(w))
		return false; /* G9 R3: no roll after a light stop */

	if(s->weather.sound_counter > 0)
		s->weather.sound_counter--;

	// Beta's positionsToUpdate: a 19x19 (radius 9) ring of CHUNK coordinates
	// around each player. Single local player -> one ring, centered on px/pz.
	// F6(b) (adversarial review) DOCUMENTED DEVIATION: Beta builds
	// positionsToUpdate from raw chunk COORDINATES and processes all 361 of
	// them regardless of whether that chunk is actually loaded --
	// getChunkFromChunkCoords lazily loads/generates it on demand. This
	// engine walks `s->world.chunks` (the RESIDENT set) and intersects it
	// with the 19x19 ring instead of iterating the ring directly, so a
	// chunk within the ring that is NOT currently resident gets no
	// cave-sound/lightning/snow roll THIS TICK (it will once it loads). This
	// is an engine memory-footprint constraint (this engine does not lazily
	// materialize a chunk purely to run a weather roll on it, unlike Beta's
	// eager generate-on-demand chunk provider) -- see docs/deviations.md for
	// the full writeup. It does not change the GATES/constants/draw order
	// for any chunk that IS resident, only which of the 361 positions get a
	// roll on a tick where the player's own chunk load radius hasn't
	// resident-filled the ring's outer edge yet.
	// lane C #2 follow-up: Beta positionsToUpdate is the UNION of one ring
	// per player of THIS world [World.updateBlocksAndPlayCaveSounds,
	// World.java:1869-1877: floor(posX/16), floor(posZ/16), +-9, into a
	// Set], so a player far from another gets its own weather rolls (and
	// can be struck). The caller's (px, pz) ring (the host's chunk) is kept
	// as one centre; every eligible actor committed to `w` adds its own
	// (server_local_eligible_actors_w, host first, then the guest). A
	// resident chunk inside ANY ring is processed exactly once, in the
	// existing resident-dict order -- so a single player (or players
	// sharing a ring) sees the identical chunk sequence and RNG stream.
	const w_coord_t radius = 9;
	w_coord_t ring_x[1 + SERVER_LOCAL_MAX_PRESENT_ACTORS];
	w_coord_t ring_z[1 + SERVER_LOCAL_MAX_PRESENT_ACTORS];
	size_t rings = 0;
	ring_x[rings] = px;
	ring_z[rings++] = pz;
	{
		struct server_player* ring_actor[SERVER_LOCAL_MAX_PRESENT_ACTORS];
		size_t n = server_local_eligible_actors_w(
			s, w, ring_actor, SERVER_LOCAL_MAX_PRESENT_ACTORS);
		for(size_t i = 0; i < n; i++) {
			if(!ring_actor[i]->has_pos)
				continue;
			ring_x[rings] = (w_coord_t)floor(ring_actor[i]->x / 16.0);
			ring_z[rings++] = (w_coord_t)floor(ring_actor[i]->z / 16.0);
		}
	}
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);
	while(!dict_server_chunks_end_p(it)) {
		int64_t id = dict_server_chunks_ref(it)->key;
		w_coord_t cx = S_CHUNK_X(id), cz = S_CHUNK_Z(id);
		bool in_ring = false;
		for(size_t r = 0; r < rings && !in_ring; r++)
			in_ring = cx >= ring_x[r] - radius && cx <= ring_x[r] + radius
				&& cz >= ring_z[r] - radius && cz <= ring_z[r] + radius;
		if(server_world_light_halted(w))
			break; /* G9 R3: a light stop ends the loop at once */
		if(in_ring)
			weather_effects_for_chunk(s, w, cx * CHUNK_SIZE, cz * CHUNK_SIZE);
		dict_server_chunks_next(it);
	}

	return true;
}

void server_weather_effects_tick_w(struct server_local* s,
								   struct server_world* w, w_coord_t px,
								   w_coord_t pz) {
	if(!server_weather_effects_rolls_w(s, w, px, pz))
		return;
	// F2 (adversarial review): the bolt-lifecycle sweep runs ONCE per server
	// tick (not per chunk) -- a bolt persists and restrikes independent of
	// which chunk-ring iteration originally spawned it.
	server_weather_tick_lightning_bolts_w(s, w);
}
