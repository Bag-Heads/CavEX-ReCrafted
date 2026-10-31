/*
	Unit tests for the Phase 2 hostile-mob mechanics slice: the per-mob stat
	table (monster_def_get) and the weighted spawn picker (monster_spawn_pick /
	monster_spawn_total_weight). Fully headless — no engine/world deps.

	1. Beta 1.7.3 stats are pinned exactly (health, drop item, drop count, name)
	   so a regression in the table is caught.
	2. monster_def_get clamps out-of-range ids to a valid def (crash-safe).
	3. The spawn distribution is proven to match the weight table EXACTLY: a
	   histogram over every roll in [0, total) must give each mob a number of
	   hits equal to its spawn_weight. This is stronger than spot-checking a few
	   rolls and is robust to future weight changes.
	4. Picker boundaries + out-of-range clamping behave.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "entity/monsters_object.h"
#include "entity/combat.h"
#include "item/items.h"
#include "util.h" // struct random_gen / rand_gen_int: the skeleton dual-drop-roll test

static void check_def(int id, const char* name, int health, uint16_t drop,
					  uint8_t drop_count);

static void test_enum_layout(void) {
	// Wire ids are frozen: ZOMBIE=0 and CREEPER=1 predate this slice and must
	// not move (CRPC_SPAWN_MONSTER sends these); new types are appended.
	assert(MONSTER_ZOMBIE == 0);
	assert(MONSTER_CREEPER == 1);
	assert(MONSTER_SKELETON == 2);
	assert(MONSTER_SPIDER == 3);
	assert(MONSTER_PIG == 4); // first passive animal, appended after the hostiles
	assert(MONSTER_COW == 5);
	assert(MONSTER_CHICKEN == 6);
	assert(MONSTER_SHEEP == 7);
	// M2 final units, appended after the passives (wire append-only rule). The three
	// slime SIZES are distinct wire ids (size-as-id) -- hostiles now exist on BOTH
	// sides of the passive 4..7 block, which is exactly why monster_is_passive must
	// be flag-driven (pinned in test_category_flags).
	assert(MONSTER_SQUID == 8);
	assert(MONSTER_SLIME_SMALL == 9);
	assert(MONSTER_SLIME_MEDIUM == 10);
	assert(MONSTER_SLIME_BIG == 11);
	// Batch 1b Nether natives (BiomeGenHell's exclusive pair), appended per
	// the wire rule.
	assert(MONSTER_PIG_ZOMBIE == 12);
	assert(MONSTER_GHAST == 13);
	assert(MONSTER_WOLF == 14);
	// The Giant (EntityGiantZombie, EntityList "Giant"/53), appended per the
	// wire rule. Never in any spawn list; full goldens in test_giant.c.
	assert(MONSTER_GIANT == 15);
	assert(MONSTER_COUNT == 16);
}

static void test_category_flags(void) {
	// The passive/hostile/water category map over ALL 12 ids -- the single source
	// of truth is the def table's flags, NOT an id-boundary compare (the old
	// `id >= MONSTER_PIG` would misclassify every slime as passive).
	assert(!monster_is_passive(MONSTER_ZOMBIE));
	assert(!monster_is_passive(MONSTER_CREEPER));
	assert(!monster_is_passive(MONSTER_SKELETON));
	assert(!monster_is_passive(MONSTER_SPIDER));
	assert(monster_is_passive(MONSTER_PIG));
	assert(monster_is_passive(MONSTER_COW));
	assert(monster_is_passive(MONSTER_CHICKEN));
	assert(monster_is_passive(MONSTER_SHEEP));
	assert(monster_is_passive(MONSTER_SQUID));         // never retaliates
	assert(!monster_is_passive(MONSTER_SLIME_SMALL));  // hostile ABOVE the passive block
	assert(!monster_is_passive(MONSTER_SLIME_MEDIUM));
	assert(!monster_is_passive(MONSTER_SLIME_BIG));
	// Nether natives are hostiles (the pigman's NEUTRALITY is the anger gate
	// in the AI, not the passive flag -- it must still retaliate).
	assert(!monster_is_passive(MONSTER_PIG_ZOMBIE));
	assert(!monster_is_passive(MONSTER_GHAST));
	/* Wolves consume the creature cap/list like EntityAnimal even though wild
	 * wolves can hunt sheep and become angry. [740c583: EntityWolf] */
	assert(monster_is_passive(MONSTER_WOLF));
	// The Giant extends EntityMob directly -- a plain hostile.
	assert(!monster_is_passive(MONSTER_GIANT));
	// water_creature: the squid ONLY (third spawn category; excluded from the
	// land-animal cap).
	for(int i = 0; i < MONSTER_COUNT; i++)
		assert(monster_is_water_creature(i) == (i == MONSTER_SQUID));
	// Out-of-range ids clamp to zombie: non-passive, non-water (crash-safe).
	assert(!monster_is_passive(-1) && !monster_is_passive(MONSTER_COUNT));
	assert(!monster_is_water_creature(-1) && !monster_is_water_creature(99999));
}

static void test_water_squid(void) {
	// Beta 1.7.3 squid (EntitySquid extends EntityWaterMob, decompile-verified
	// 2026-06-11): hp 10 (EntityLiving default), setSize(0.95, 0.95), eye 0.8075
	// (= 0.95*0.85). Drop = ITEM_DYE (ink sac = dyePowder durability 0; the 1-3
	// count is the death site's nextInt(3)+1). In NEITHER land picker: Beta puts
	// the squid alone in spawnableWaterCreatureList. NOT fall_immune -- b1.7.3
	// EntityWaterMob does not override fall(); a beached squid takes fall damage.
	check_def(MONSTER_SQUID, "Squid", 10, ITEM_DYE, 1);
	const struct monster_def* q = monster_def_get(MONSTER_SQUID);
	assert(q->box_w == 0.95f && q->box_h == 0.95f);
	assert(q->eye_height == 0.8075f);
	assert(q->passive == true);
	assert(q->water_creature == true);
	assert(q->fall_immune == false);
	assert(q->spawn_weight == 0);  // not in the hostile picker
	assert(q->animal_weight == 0); // not in the land-animal picker either
}

static void test_slime_defs(void) {
	// Beta 1.7.3 slime sizes as ids (decompile-verified 2026-06-11): setSlimeSize(n)
	// -> box 0.6n x 0.6n, health n*n, eye 0.6n*0.85. Only size 1 drops (slimeball
	// 341, getDropItemId returns 0 at size > 1). Only SMALL carries hostile spawn
	// weight: it is the SENTINEL for Beta's single weight-10 EntitySlime list
	// entry -- the picker returning it means "a slime pack"; the spawn pass gates
	// it (slime-chunk + y<16, no light) and re-rolls each member's actual size.
	check_def(MONSTER_SLIME_SMALL, "Slime", 1, ITEM_SLIME_BALL, 1);
	check_def(MONSTER_SLIME_MEDIUM, "Slime", 4, 0, 0);
	check_def(MONSTER_SLIME_BIG, "Slime", 16, 0, 0);
	static const int slime_ids[3] = {MONSTER_SLIME_SMALL, MONSTER_SLIME_MEDIUM,
									 MONSTER_SLIME_BIG};
	static const int slime_size[3] = {1, 2, 4};
	// Eye heights are pinned as LITERALS (the def-table convention -- see the pig's
	// 0.765f): 0.6n * 0.85 rounded to the stored float. A computed 0.6f*n*0.85f
	// differs in the last ulp from the literal, so literal == literal is the test.
	static const float slime_box[3] = {0.6f, 1.2f, 2.4f};
	static const float slime_eye[3] = {0.51f, 1.02f, 2.04f};
	for(int k = 0; k < 3; k++) {
		const struct monster_def* d = monster_def_get(slime_ids[k]);
		assert(d->box_w == slime_box[k] && d->box_h == slime_box[k]); // setSize(0.6n)
		assert(d->health == slime_size[k] * slime_size[k]);           // health = n^2
		assert(d->eye_height == slime_eye[k]);                        // h * 0.85
		assert(d->passive == false && d->water_creature == false);
		assert(d->animal_weight == 0);
		assert(d->fall_immune == false); // slimes take normal fall damage (no override)
	}
	assert(monster_def_get(MONSTER_SLIME_SMALL)->spawn_weight == 10); // the sentinel
	assert(monster_def_get(MONSTER_SLIME_MEDIUM)->spawn_weight == 0); // never picked
	assert(monster_def_get(MONSTER_SLIME_BIG)->spawn_weight == 0);    // (re-roll only)
	// ITEM_SLIME_BALL is Beta id 341 (Item.slimeBall = new Item(85) -> 256+85), and
	// the registry actually carries an object there (items_init registers it).
	assert(ITEM_SLIME_BALL == 341);
}

static void check_def(int id, const char* name, int health, uint16_t drop,
					  uint8_t drop_count) {
	const struct monster_def* d = monster_def_get(id);
	assert(d);
	assert(strcmp(d->name, name) == 0);
	assert(d->health == health);
	assert(d->drop == drop);
	assert(d->drop_count == drop_count);
}

static void test_beta_stats(void) {
	// Beta 1.7.3 hostile stats. All four are 20 HP (EntityLiving default; Beta
	// spiders are 20, not the modern 16). Drops are the Beta 1.7.3 dropFewItems
	// outputs (pre-1.8/1.9: feather/arrow/string, not rotten flesh/spider eye --
	// bone is NOT a post-1.7.3 addition, see the PARITY FIX note below).
	check_def(MONSTER_ZOMBIE, "Zombie", 20, ITEM_FEATHER, 1);
	check_def(MONSTER_CREEPER, "Creeper", 20, ITEM_GUNPOWDER, 1);
	check_def(MONSTER_WOLF, "Wolf", 8, 0, 0);
	// PARITY FIX 2026-09-01: the static def's .drop/.drop_count cover ONLY the
	// ARROW half of EntitySkeleton.dropFewItems -- this check_def call is still
	// correct for what the static table models. Beta 1.7.3 skeletons ALSO drop
	// 0-2 bones via a SECOND, independent nextInt(3) roll (740c583 decompile);
	// the old comment here (and in monsters_object.c) wrongly lumped bone in
	// with the real Beta 1.8/1.9 additions (rotten flesh, spider eye) -- bones
	// are genuinely in 1.7.3. The bone half is a runtime special case at the
	// entity_monster.c death-drop site (same pattern as the sheep/squid/pig
	// specials there, none of which show up in the static table either), so it
	// is pinned separately below by test_skeleton_dual_drop_roll, not here.
	check_def(MONSTER_SKELETON, "Skeleton", 20, ITEM_ARROW, 1);
	check_def(MONSTER_SPIDER, "Spider", 20, ITEM_STRING, 1);
}

// PARITY FIX 2026-09-01 regression test. entity_monster.c's death-drop site draws
// TWO independent rand_gen_int(g, MOB_DROP_COUNT_DEN) rolls for a skeleton kill --
// the first for arrows (the pre-existing generic count>0/drop_item path, unchanged),
// the second, new one for bones, drawn and applied via mob_drop_count UNCONDITIONALLY
// regardless of the arrow roll's outcome. This test cannot link entity_monster.c
// (heavy server/world deps; test_mob_types is deliberately headless -- see the file
// header), so it reproduces the EXACT two-call sequence and pure mob_drop_count
// mapping the production site uses, proving: (1) both rolls independently cover the
// full 0..MOB_DROP_COUNT_DEN-1 range, and (2) the second (bone) roll is NOT gated by
// the first (arrow) roll -- a bone can drop on a kill that drops zero arrows, which
// is only possible if the second roll is genuinely a separate, unconditional draw
// (the exact bug the old single-roll code could never produce).
// [740c583: EntitySkeleton.java dropFewItems -- two independent this.rand.nextInt(3)
// calls, arrows then bones]
static void test_skeleton_dual_drop_roll(void) {
	bool saw_zero_arrows_nonzero_bones = false;
	bool saw_arrow_count[MOB_DROP_COUNT_DEN];
	bool saw_bone_count[MOB_DROP_COUNT_DEN];
	memset(saw_arrow_count, 0, sizeof(saw_arrow_count));
	memset(saw_bone_count, 0, sizeof(saw_bone_count));

	// ONE seeded generator drawn from continuously across many simulated kills --
	// exactly like the real s->rand_src, which is NEVER reseeded between deaths.
	// (Deliberately NOT a fresh {.seed=N} per trial: xorshift32 needs a draw or
	// two to spread entropy out of a small/low-magnitude seed, so a fresh tiny
	// seed's FIRST rand_gen_int call is heavily biased toward 0 -- measured: seeds
	// 1..3999 each freshly constructed returned arrow_count==0 100% of the time.
	// This project's own seed idioms elsewhere are all large/well-mixed constants
	// for exactly this reason, e.g. test_animal_interact_env.c's 0xA1B2C3D4 and
	// test_block_behavior_parity.c's 0x9E3779B9 -- reused here.)
	struct random_gen g = {.seed = 0x9E3779B9u};
	for(int trial = 0; trial < 6000; trial++) {
		// Mirrors entity_monster.c's entity_monster_begin_death draw order
		// EXACTLY: count_roll (arrows) first, bone_roll second, both via the
		// same rand_gen_int(&s->rand_src, MOB_DROP_COUNT_DEN) + mob_drop_count.
		int arrow_count = mob_drop_count(rand_gen_int(&g, MOB_DROP_COUNT_DEN));
		int bone_count = mob_drop_count(rand_gen_int(&g, MOB_DROP_COUNT_DEN));

		assert(arrow_count >= 0 && arrow_count < MOB_DROP_COUNT_DEN);
		assert(bone_count >= 0 && bone_count < MOB_DROP_COUNT_DEN);
		saw_arrow_count[arrow_count] = true;
		saw_bone_count[bone_count] = true;

		if(arrow_count == 0 && bone_count > 0)
			saw_zero_arrows_nonzero_bones = true;
	}

	// Over 6000 simulated kills, every 0..2 outcome must appear for BOTH rolls --
	// neither roll is a degenerate always-same-value stub.
	for(int c = 0; c < MOB_DROP_COUNT_DEN; c++) {
		assert(saw_arrow_count[c]);
		assert(saw_bone_count[c]);
	}
	// The independence proof: bones with zero arrows is only reachable if the
	// bone roll is a genuinely SEPARATE draw, not derived from (or gated by) the
	// arrow roll -- the exact gap the missing second roll used to leave.
	assert(saw_zero_arrows_nonzero_bones);
}

static void test_passive_pig(void) {
	// Beta 1.7.3 pig (EntityAnimal): hp 10, raw PORKCHOP drop, 0.9 x 0.9 box, 0.765 eye
	// (= 0.9 * 0.85), EntityCreature speed 0.7, passive. spawn_weight 0 keeps it OUT of
	// the hostile spawn table so the picker distribution is byte-unchanged.
	check_def(MONSTER_PIG, "Pig", 10, ITEM_PORKCHOP, 1);
	const struct monster_def* p = monster_def_get(MONSTER_PIG);
	assert(p->move_speed == 0.7f);
	assert(p->box_w == 0.9f && p->box_h == 0.9f);
	assert(p->eye_height == 0.765f);
	assert(p->passive == true);
	assert(p->fall_immune == false);
	assert(p->spawn_weight == 0);   // OUT of the hostile picker
	assert(p->animal_weight == 10); // IN the animal picker (Beta creature list)
	// The passive/hostile category split: every hostile is NOT passive; the pig IS.
	assert(!monster_is_passive(MONSTER_ZOMBIE));
	assert(!monster_is_passive(MONSTER_CREEPER));
	assert(!monster_is_passive(MONSTER_SKELETON));
	assert(!monster_is_passive(MONSTER_SPIDER));
	assert(monster_is_passive(MONSTER_PIG));
}

static void test_passive_cow(void) {
	// Beta 1.7.3 cow (EntityCow): hp 10, LEATHER drop ONLY (no beef -- 1.8), 0.9 x 1.3 box,
	// 1.105 eye (= 1.3 * 0.85), speed 0.7, passive, animal_weight 8. Verified vs the
	// PeytonPlayz595 b1.7.3 decompile (the mc-dev source is post-1.7.3 and would add beef).
	check_def(MONSTER_COW, "Cow", 10, ITEM_LEATHER, 1);
	const struct monster_def* c = monster_def_get(MONSTER_COW);
	assert(c->move_speed == 0.7f);
	assert(c->box_w == 0.9f && c->box_h == 1.3f);
	assert(c->eye_height == 1.105f);
	assert(c->passive == true);
	assert(c->fall_immune == false);
	assert(c->spawn_weight == 0);
	assert(c->animal_weight == 8);
	assert(monster_is_passive(MONSTER_COW));
}

static void test_passive_chicken(void) {
	// Beta 1.7.3 chicken (EntityChicken): hp 4 (explicit override, NOT 10), FEATHER drop
	// ONLY (no meat -- 1.8), 0.3 x 0.4 box (smallest mob), 0.34 eye (= 0.4 * 0.85), speed
	// 0.7, passive, FALL-IMMUNE (empty fall()), animal_weight 10.
	check_def(MONSTER_CHICKEN, "Chicken", 4, ITEM_FEATHER, 1);
	const struct monster_def* c = monster_def_get(MONSTER_CHICKEN);
	assert(c->move_speed == 0.7f);
	assert(c->box_w == 0.3f && c->box_h == 0.4f);
	assert(c->eye_height == 0.34f);
	assert(c->passive == true);
	assert(c->fall_immune == true); // the only fall-immune mob
	assert(c->spawn_weight == 0);
	assert(c->animal_weight == 10);
	assert(monster_is_passive(MONSTER_CHICKEN));
}

// Beta EntityChicken.onLivingUpdate's fall-speed damping [740c583]:
// `if (!this.onGround && this.motionY < 0.0D) { this.motionY *= 0.6D; }` --
// the signature "chicken glide", DISTINCT from fall_immune (pinned above:
// fall damage is a separate, already-implemented mechanic). This only damps
// the descent SPEED.
static void test_chicken_fall_damp(void) {
	// Grounded: never damped, whatever the (nonsensical while grounded) vel.
	assert(chicken_fall_damp(-1.0f, true) == -1.0f);
	assert(chicken_fall_damp(1.0f, true) == 1.0f);
	// Airborne but RISING (vel >= 0): not damped -- only descent is affected.
	assert(chicken_fall_damp(0.0f, false) == 0.0f);
	assert(chicken_fall_damp(0.42f, false) == 0.42f); // e.g. a jump's initial pop
	// Airborne and falling: exactly *0.6, every tick, so repeated ticks decay
	// geometrically rather than accelerating under plain gravity.
	assert(chicken_fall_damp(-1.0f, false) == -0.6f);
	assert(chicken_fall_damp(-0.6f, false) == -0.36f);
	// Repeated damping with no further gravity input decays geometrically
	// toward 0 (0.6^20 ~= 3.66e-5) -- confirms the *0.6 compounds every call
	// rather than being a one-shot clamp.
	float v = -1.0f;
	for(int t = 0; t < 20; t++)
		v = chicken_fall_damp(v, false);
	assert(v < 0.0f && v > -0.001f);
}

static void test_passive_sheep(void) {
	// Beta 1.7.3 sheep (EntitySheep): hp 10 (EntityLiving default), drops 1 BLOCK_WOOL (the
	// COLOUR is set per-entity at the death site, not in the def), 0.9 x 1.3 box, 1.105 eye
	// (= 1.3 * 0.85), speed 0.7, passive, NOT fall-immune, animal_weight 12 (the highest of the
	// animals -- a herd skews sheep). The drop COUNT is a flat 1 (Beta drops exactly 1 wool, not
	// nextInt(3)); the def pins count 1 and the death site forces it.
	check_def(MONSTER_SHEEP, "Sheep", 10, BLOCK_WOOL, 1);
	const struct monster_def* sh = monster_def_get(MONSTER_SHEEP);
	assert(sh->move_speed == 0.7f);
	assert(sh->box_w == 0.9f && sh->box_h == 1.3f);
	assert(sh->eye_height == 1.105f);
	assert(sh->passive == true);
	assert(sh->fall_immune == false);
	assert(sh->spawn_weight == 0);
	assert(sh->animal_weight == 12);
	assert(monster_is_passive(MONSTER_SHEEP));

	// The per-entity fleece colour is a pure id-hash -> the SAME on client (render) and server
	// (drop) with no networking. Pin the Beta distribution: white dominates (~82%), every result
	// is a valid wool metadata index 0..15, and the 6 reachable colours are exactly the Beta set.
	int fhist[16] = {0};
	for(uint32_t id = 0; id < 200000u; id++) {
		int col = sheep_fleece_color(id);
		assert(col >= 0 && col < 16);
		fhist[col]++;
	}
	// Only the Beta fleece colours occur (white 0, pink 6, gray 7, lightgray 8, brown 12, black 15).
	for(int i = 0; i < 16; i++) {
		bool allowed = (i == 0 || i == 6 || i == 7 || i == 8 || i == 12 || i == 15);
		if(!allowed)
			assert(fhist[i] == 0);
	}
	assert(fhist[0] > fhist[15]);  // white is by far the most common
	assert(fhist[0] > 150000);     // ~81.84% of 200k
	assert(fhist[6] > 0);          // pink is rare but does occur (~0.16%)
}

static void test_animal_distribution(void) {
	// The animal picker realizes the Beta creature spawn list (pig 10, cow 8, chicken 10,
	// sheep 12) exactly, and -- crucially -- NEVER returns a hostile. This is the animal-side
	// mirror of test_distribution_exact, proving the two spawn lists stay category-separated.
	int total = animal_spawn_total_weight();
	assert(total == 40); // 10 + 8 + 10 + 12

	int hist[MONSTER_COUNT] = {0};
	for(int roll = 0; roll < total; roll++) {
		int a = animal_spawn_pick(roll);
		assert(a >= 0 && a < MONSTER_COUNT);
		assert(monster_is_passive(a)); // the animal picker only yields animals
		hist[a]++;
	}
	for(int i = 0; i < MONSTER_COUNT; i++)
		assert(hist[i] == monster_def_get(i)->animal_weight);
	// Hostiles (animal_weight 0) are never animal-picked -- including the slimes,
	// which sit ABOVE the passive id block; and the squid (water category) is
	// never LAND-animal-picked either.
	assert(hist[MONSTER_ZOMBIE] == 0 && hist[MONSTER_CREEPER] == 0
		   && hist[MONSTER_SKELETON] == 0 && hist[MONSTER_SPIDER] == 0);
	assert(hist[MONSTER_SQUID] == 0);
	assert(hist[MONSTER_SLIME_SMALL] == 0 && hist[MONSTER_SLIME_MEDIUM] == 0
		   && hist[MONSTER_SLIME_BIG] == 0);

	// First animal owns roll 0; last (sheep, the highest animal id) owns total-1; out-of-range
	// clamps to the ends.
	assert(animal_spawn_pick(0) == MONSTER_PIG);
	assert(animal_spawn_pick(total - 1) == MONSTER_SHEEP);
	assert(animal_spawn_pick(-1) == MONSTER_PIG);
	assert(animal_spawn_pick(total + 9999) == MONSTER_SHEEP);

	// The hostile picker total: the four original hostiles (10 each) + the
	// SLIME_SMALL sentinel (Beta's single weight-10 EntitySlime entry) = 50.
	// Animals + squid + the non-small slime sizes all carry spawn_weight 0.
	assert(monster_spawn_total_weight() == 50);
}

static void test_move_speed(void) {
	// Faithful Beta 1.7.3 EntityCreature moveSpeed (two independent decompiles):
	// zombie 0.5 < skeleton = creeper 0.7 < spider 0.8. mc173's flat 0.5 for
	// skeleton/creeper is the OUTLIER error -- pin against it explicitly.
	assert(monster_def_get(MONSTER_ZOMBIE)->move_speed == 0.5f);
	assert(monster_def_get(MONSTER_SKELETON)->move_speed == 0.7f);
	assert(monster_def_get(MONSTER_CREEPER)->move_speed == 0.7f);
	assert(monster_def_get(MONSTER_SPIDER)->move_speed == 0.8f);
	assert(monster_def_get(MONSTER_SKELETON)->move_speed != 0.5f); // not the mc173 error
	assert(monster_def_get(MONSTER_CREEPER)->move_speed != 0.5f);
	assert(monster_def_get(MONSTER_ZOMBIE)->move_speed
		   < monster_def_get(MONSTER_SKELETON)->move_speed);
	assert(monster_def_get(MONSTER_SKELETON)->move_speed
		   < monster_def_get(MONSTER_SPIDER)->move_speed);
}

static void test_wii_speed_cap(void) {
	// The Wii cap (mob_speed_cap_wii) keeps the faithful ordering but raises ONLY
	// the spider; the skeleton (threat = bow) and creeper (threat = blast) are
	// held at the base cap so they don't out-walk the player on a Wii controller.
	float z = mob_speed_cap_wii(monster_def_get(MONSTER_ZOMBIE)->move_speed);
	float sk = mob_speed_cap_wii(monster_def_get(MONSTER_SKELETON)->move_speed);
	float c = mob_speed_cap_wii(monster_def_get(MONSTER_CREEPER)->move_speed);
	float sp = mob_speed_cap_wii(monster_def_get(MONSTER_SPIDER)->move_speed);
	assert(z == MOB_MAX_SPEED_WII);
	assert(sk == MOB_MAX_SPEED_WII); // skeleton shaved to base
	assert(c == MOB_MAX_SPEED_WII);  // creeper shaved to base
	assert(sp > MOB_MAX_SPEED_WII);  // only the spider is raised
	assert(sp == MOB_MAX_SPEED_WII * MOB_SPIDER_SPEED_MULT_WII);
	// every cap stays in a playable band (~2-6 blocks/sec @ 20 TPS).
	assert(z * 20.0f > 2.0f && sp * 20.0f < 6.0f);
}

static void test_def_clamp(void) {
	// Out-of-range ids must not read past the table; they clamp to zombie.
	assert(monster_def_get(-1) == monster_def_get(MONSTER_ZOMBIE));
	assert(monster_def_get(MONSTER_COUNT) == monster_def_get(MONSTER_ZOMBIE));
	assert(monster_def_get(99999) == monster_def_get(MONSTER_ZOMBIE));
}

static void test_total_weight(void) {
	int sum = 0;
	for(int i = 0; i < MONSTER_COUNT; i++)
		sum += monster_def_get(i)->spawn_weight;
	assert(monster_spawn_total_weight() == sum);
	assert(sum > 0);
}

static void test_distribution_exact(void) {
	int total = monster_spawn_total_weight();
	int hist[MONSTER_COUNT] = {0};

	// Every roll the RNG can return must map to a valid mob; tally them.
	for(int roll = 0; roll < total; roll++) {
		int mob = monster_spawn_pick(roll);
		assert(mob >= 0 && mob < MONSTER_COUNT);
		hist[mob]++;
	}

	// The tally must equal each mob's weight exactly — proving the picker
	// realizes the table's distribution with no off-by-one gaps or overlaps.
	for(int i = 0; i < MONSTER_COUNT; i++)
		assert(hist[i] == monster_def_get(i)->spawn_weight);
}

static void test_picker_bounds(void) {
	int total = monster_spawn_total_weight();

	// First weighted mob owns roll 0; last weighted mob owns roll total-1.
	int first = -1, last = -1;
	for(int i = 0; i < MONSTER_COUNT; i++) {
		if(monster_def_get(i)->spawn_weight > 0) {
			if(first < 0)
				first = i;
			last = i;
		}
	}
	assert(monster_spawn_pick(0) == first);
	assert(monster_spawn_pick(total - 1) == last);

	// Out-of-range rolls clamp instead of returning garbage / reading OOB.
	assert(monster_spawn_pick(-1) == first);
	assert(monster_spawn_pick(-12345) == first);
	assert(monster_spawn_pick(total) == last);
	assert(monster_spawn_pick(total + 12345) == last);
}

int main(void) {
	test_enum_layout();
	test_category_flags();
	test_passive_pig();
	test_passive_cow();
	test_passive_chicken();
	test_chicken_fall_damp();
	test_passive_sheep();
	test_water_squid();
	test_slime_defs();
	test_animal_distribution();
	test_beta_stats();
	test_skeleton_dual_drop_roll();
	test_move_speed();
	test_wii_speed_cap();
	test_def_clamp();
	test_total_weight();
	test_distribution_exact();
	test_picker_bounds();
	printf("test_mob_types: all assertions passed\n");
	return 0;
}
