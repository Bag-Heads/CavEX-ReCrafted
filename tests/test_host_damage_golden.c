/* Copyright (c) 2026 CaveWii_v2 contributors
 * S1-a3: the HOST-path golden for "the whole damage/heal pipeline now takes
 * the target actor".
 *
 * THE CLAIM. Parameterizing hurt resistance (player_iframe_damage_for), the
 * armor absorb + wear + echo (server_local_apply_player_armor_for /
 * _restore_ / _sync_), knockback, the health mutation
 * (server_local_set_player_health_for) and the health reply
 * (server_local_actor_health_reply) on the TARGET ACTOR is a PURE PARAMETER
 * MOVE for the host: every pre-existing call site passes &s->player, so
 * nothing the host can observe changes. Astra call #8 Q2 required the host
 * path to stay byte-identical; "pure move" is a claim, so this file is the
 * proof, in the same two-hash shape as test_block_hooks_actor_golden.c
 * (S1-a1b) which Astra gate-2 D1 asked for.
 *
 * WHY THIS FILE COMPILES ON BOTH TREES, which is what makes the comparison
 * meaningful: it names NO symbol this card added. Every step goes through a
 * pre-existing public entry point -- server_local_hurt_player,
 * server_local_damage_player_result, server_local_set_player_health,
 * server_local_test_apply_player_hazards, and SRPC_BLOCK_PLACE through
 * server_local_test_process_rpc for eating -- so the same source text, byte
 * for byte, builds and runs at the base sha 14a84643 (before the damage
 * pipeline was parameterized) and after the change.
 *
 * WHAT THE TWO HASHES COVER.
 *   state  A rolling digest folded after EVERY scripted step (not only at the
 *          end, so a divergence that later re-converges cannot hide): the step
 *          index and the full host damage-relevant actor state -- position,
 *          health, hurt_resist_time, last_damage, damage_remainder, oxygen,
 *          fire, fall_distance, the server-authored knockback velocity, the
 *          death latch, and every armor + inventory slot with its durability.
 *   rpc    The ORDER and identity of every client_rpc the server emitted,
 *          folded in emission order by clin_test_set_rpc_observer: the type
 *          ordinal for every RPC plus the named payload fields of the types
 *          this script produces (health, inventory slot, sound, player pos).
 *          Field-by-field, never a raw memcpy -- three payload arms carry heap
 *          pointers and several have interior padding.
 *
 * THE SCRIPT covers every branch of the pipeline the card touched: a fresh
 * mob-melee hit (damage + armor wear + knockback + the hurt sound), an
 * absorbed in-window repeat, a STRONGER in-window hit (Beta excess-only, no
 * fresh window, no knockback), the flat-damage path with and without a
 * knockback direction, armored and unarmored hits so the 1/25 remainder carry
 * is exercised both ways, every environmental cause (fire ramp, lava, cactus,
 * drowning, void), the lethal hit and the death-screen dwell that follows it
 * (corpse hits must be absorbed with no armor wear), and the healing paths --
 * a direct heal, all nine foods eaten to a cap, and a cake slice.
 */
#include "support/split_residency_fixture.h"
#include <limits.h>

#include "item/items.h"
#include "platform/audio.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The goldens, recorded from a build of this file at the base sha 14a84643,
 * where the damage pipeline is host-singular. See
 * tmp/jobs/mp-split-0912/s1a3-report.md for the command lines and both
 * printed values. */
/* Re-baselined 2026-09-26 (blist-0926, Astra ruling 8: attributed before recapture; per-RPC dumps diffed index-less, base vs stew tip, tmp/jobs/split-dimensions-plan-0924/out/BLIST-0926.md): stew-0925 (1266c2e0) makes mushroom stew RETURN the empty bowl to the held slot -- ItemSoup.onItemRightClick:8-10 returns `new ItemStack(Item.bowlEmpty)` after ItemFood:14-17 consumed the stew, and PlayerController.sendUseItem:47-56 stores the returned stack in the selected slot. STATE first moves at step 25 "eat mushroom stew": hotbar slot 36 holds 281/1 (bowl) instead of 282/1 (one stew left). RPC: the stream loses exactly one row, #839 CRPC_SPAWN_ITEM eid 9 item 281 at (0,4,0) (the old bowl ENTITY), and #840 CRPC_INVENTORY_SLOT slot 36 carries 281/1 instead of 282/1; every other row (1039 of 1040, positions included) is byte-identical -- the host never collected that bowl here, so no pickup/sound-stream effect (contrast test_block_hooks_actor_golden). Previous values: STATE 0xa74f3f18d8078bd3, RPC 0xf3ffea1e16cf13df. */
/* Re-baselined 2026-09-26 (damage-client-0926, Beta knockBack motion + its client delivery; dumps in the lane report): FIRST divergence step 2 "spider melee, fresh window": host y 6.0200000107 -> 5.6200000048 (the old one-shot `y += 0.4` pop is gone: knockBack writes motionY only, EntityLiving.java:392-403, and the rise is integrated by the hit body's own client from CRPC_LOCAL_PLAYER_VELOCITY -- EntityTrackerEntry.java:124-127 -> NetClientHandler.java:165-170; proven by test_knockback_client_motion), x/z 1-ulp from knockBack's double arithmetic (:395-400); step 7 x/z further from the halved PREVIOUS motion Beta keeps (:394-396, motion /= 2 then -= impulse, not an assignment). Every later step carries those positions; health/window/armor/inventory rows identical. */
/* Combined 2026-09-26 at the G4 merge of codex/a1c-kb-0926 (knockback) onto G4 (stew, blist-0926): the two recaptures are independent. Knockback alone = STATE 0xa9aaf9cd9b85d3aa / RPC 0x54fb7d1917f5eef5 (1044 rows); stew alone = 0x6627597e19f09200 / 0x80672bdd649c016a (1039 rows). On the merged tree, with item_mushroom_stew.c reverted to its pre-stew body, the file prints exactly the knockback pair. A scratch per-row/per-step dump (reverted) diffed merged vs that stew-reverted tree differs ONLY at step 25 "eat mushroom stew": the CRPC_SPAWN_ITEM (bowl entity) row is gone and CRPC_INVENTORY_SLOT slot 36 carries 281/1 instead of 282/1, and the step-25 state has slot 36 = 281/1; every knockback row/position is byte-identical. 1044 - 1 = 1043 rows. The same STATE value was measured independently in BLIST-0926.md (damage-fix server half on the stew tip = 0x9a688e72fe68f9b5). */
/* Re-pinned 2026-10-09: lead evidence acceptance under GOLDEN-0db9
 * (ASTRA-GOLDEN-0db9.md), regress-1008/RB08-RESULT.md, both rows
 * FULLY-ATTRIBUTED and ACCEPTED. P reproduces the old pins; P+C and
 * candidate complete STATE/RPC streams are byte-identical, eight repeats.
 * C SHA256: 9277269772c1d4426a8beb6d4618a4f44d87d2f75bf9c7830c7bcf77f30a1448.
 * Old pins: STATE 0x9a688e72fe68f9b5, RPC 0xcd3ca2d5d290bbda;
 * old record counts: 32 STATE / 1050 RPC.
 * Accepted record counts: 32 STATE / 911 RPC.
 * Fixtures, observers, determinism and assertions remain unchanged. */
/* pre-V6 pin (P=090d8948a): 0x29bce2d5006fb750.
 * FAST re-pin (maintainer 2026-10-11): first divergence STATE step2/actor field67/global634 vel_x in source/network/server_local.c:14948-14949 (90708c579). */
#define GOLDEN_STATE UINT64_C(0xc35065347bb09ebf)
/* GOLDEN_RPC re-baselined 2026-09-24 (next-gaps-0924 M2b): bisected across the M2 first-parent merges (tmp/jobs/next-gaps-0924/m2b-bisect-*.log): W1C item physics (deb39952, EntityItem.java:45-69 per-entity Random/pushOut/bounce), W1A player physics + suffocation (d4cf78ee, EntityLiving.moveEntityWithHeading:455-497, Entity.isEntityInsideOpaqueBlock:851-863) and W1B mob movement (7c7a8ff2, EntityLiving.moveEntityWithHeading:479-497) moved the observed stream; every value is the one printed by this file at 0b33e352 and again after the F1b/W1E-e merges. The STATE golden is unchanged. */
/* Re-baselined 2026-09-25 (next-gaps-0924 GOLDEN-0925, Astra kickoff ruling 8): bisected commit-by-commit inside the W2-MOVE wave on main 5f03e259 (tmp/jobs/next-gaps-0924/bisect-move.log, bisect-move2.log, bisect-column.log; per-field dumps in golden-dump/): aa8bfa1c (Entity.applyEntityCollision:668-691) first moves sheep 2/3 -- spawned as one pack 1.0 apart in Z at tick 60 -- at tick 61 (sheep 2's loop pushes 3 at its spawn cell: the full 0.05F Z half, dx/|dz|*0.05F on X), and that push is what the hash first sees: the first HASHED divergence (golden-dump/base- vs p4-host_damage_golden.txt, CRPC_ENTITY_MOVE hashes its type only) is sheep 3's footstep CRPC_SOUND position at RPC #775 (script step 21), x -16.4021 -> -16.4405, z 52.8351 -> 52.8305; the extra CRPC_ENTITY_MOVE rows are sheep 1's (52 -> 90 over the two determinism runs), the chicken 8 rows are identical (G1 review F4 corrected the earlier "chicken 8 at tick 101" note); b0931e6e (swept clipping, AxisAlignedBB.calculateYOffset) then lands every mob exactly on the face (sheep 1 y 4.004537 -> 4.000000 in the first CRPC_ENTITY_MOVE of step 15). No later commit moves it (integ3 chain to d60214dd checked). Pinned by tests/test_golden_move_divergence.c S2 + S4; the STATE golden is unchanged; this value is the one this file printed at the integ3 tip dadbffd1. */
/* Re-baselined 2026-09-26 (damage-client-0926): 1040 -> 1044 rows = exactly one CRPC_LOCAL_PLAYER_VELOCITY per FRESH knockback hit (steps 2, 6, 7, 8; none for the no-direction flat hit at step 9 or the absorbed/excess-only hits -- Beta sends Packet28 only when knockBack ran, EntityTrackerEntry.java:124-127); the only other row changes are VALUES of 23 CRPC_PLAYER_POS (y without the +0.4 pop, x/z per the STATE note) and 10 hurt CRPC_SOUND positions (the same feet); every pitch, count and order otherwise identical. */
/* Re-baselined 2026-09-27 (lane C mob motion units S2, docs/intent/mob-motion-units.md; Astra rulings 2026-09-27): GOLDEN_STATE is UNCHANGED (the player damage/heal pipeline is untouched); only the RPC stream moved, 1043 -> 1049 rows. The rows this script emits that depend on MOB motion are the wandering/pushed mobs' CRPC_ENTITY_MOVE rows (type-only in the hash, so a changed COUNT moves it) and their footstep CRPC_SOUND positions: with Beta's vertical physics (move, then (motionY - 0.08D) * 0.98F; jump() (double)0.42F; no engine auto-jump / ySize bank) a mob settles and walks on different ticks and cells. ATTRIBUTION NOTE (Astra ruling 8): the per-row base-vs-S2 dump was NOT produced on this pass -- the host disk was full (a second build tree could not be made); it is listed as an open item in the S2 report. Previous value: 0xa124608d0830e744. */
/* Re-baselined 2026-09-27 at the G8 pre-stack (drown-fx-0927 server half,
 * out/DROWN-FX-handoff.patch, on top of motion S2): attributed on the same
 * tree with vs without that one server_local.c hunk -- without it the file
 * prints exactly the motion-S2 pair (STATE 0x9a688e72fe68f9b5 / RPC
 * 0x7197c0427006d514, 1049 rows); with it STATE is unchanged and the stream
 * grows by exactly one row (1049 -> 1050: the CRPC_DROWN_BUBBLES of the
 * drowning step, Beta EntityLiving.onEntityUpdate:127-132 bubbles before the
 * hit). */
/* pre-V6 pin (P=090d8948a): 0x57d97e67f436f9dd.
 * FAST re-pin (maintainer 2026-10-11): first divergence RPC envelope509 type66->type1 in source/network/server_local.c:14731 (d86e8d673/90cf80703). */
#define GOLDEN_RPC UINT64_C(0x1415c8674031877a)

#define FNV_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

/* Superflat (the fixture world): bedrock 0, dirt 1-2, grass 3, air above. */
#define GROUND_Y 3
#define LOAD_TICKS 400u
#define SETTLE_TICKS 4u

/* Write-only attribution records; assertions and folded bytes are unchanged.
 * CW_ATTR_DUMP=<prefix> writes the complete stream and a field/offset map. */
static FILE* attr_bytes;
static FILE* attr_map;
static uint64_t attr_offset;
static void attr_record(const void* data, size_t n, const char* field,
						const char* function) {
	const char* prefix = getenv("CW_ATTR_DUMP");
	if(!prefix || !prefix[0]) return;
	if(!attr_bytes) {
		char path[1024];
		int written = snprintf(path, sizeof(path), "%s.bin", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_bytes = fopen(path, "wb"); assert(attr_bytes);
		written = snprintf(path, sizeof(path), "%s.map", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_map = fopen(path, "w"); assert(attr_map);
	}
	assert(fprintf(attr_map, "%" PRIu64 " %zu %s %s\n",
				   attr_offset, n, function, field) > 0);
	assert(fwrite(data, 1, n, attr_bytes) == n);
	attr_offset += n;
}

static uint64_t hash_bytes_at(uint64_t hash, const void* data, size_t n,
                              const char* field, const char* function) {
	attr_record(data, n, field, function);
	const uint8_t* p = data;
	while(n--) {
		hash ^= *p++;
		hash *= FNV_PRIME;
	}
	return hash;
}
#define hash_bytes(h, data, n) \
	hash_bytes_at((h), (data), (n), #data, __func__)


#define HASH_FIELD(h, v) ((h) = hash_bytes((h), &(v), sizeof(v)))

/* ---- the RPC-ORDER digest ------------------------------------------------ */
static uint64_t s_rpc_hash;
static unsigned long s_rpc_count;
static unsigned long s_health_rpcs;

static uint64_t hash_item(uint64_t h, const struct item_data* it) {
	HASH_FIELD(h, it->id);
	HASH_FIELD(h, it->count);
	HASH_FIELD(h, it->durability);
	return h;
}

static void observe_send(const struct client_rpc* call, void* user) {
	(void)user;
	s_rpc_count++;
	int type = (int)call->type;
	s_rpc_hash = hash_bytes(s_rpc_hash, &type, sizeof(type));
	switch(call->type) {
	case CRPC_PLAYER_SET_HEALTH:
		s_health_rpcs++;
		HASH_FIELD(s_rpc_hash, call->payload.player_set_health.health);
		break;
	case CRPC_INVENTORY_SLOT:
		HASH_FIELD(s_rpc_hash, call->payload.inventory_slot.window);
		HASH_FIELD(s_rpc_hash, call->payload.inventory_slot.slot);
		s_rpc_hash = hash_item(s_rpc_hash, &call->payload.inventory_slot.item);
		break;
	case CRPC_SOUND:
		HASH_FIELD(s_rpc_hash, call->payload.sound.sound_id);
		for(unsigned k = 0; k < 3; k++)
			HASH_FIELD(s_rpc_hash, call->payload.sound.pos[k]);
		HASH_FIELD(s_rpc_hash, call->payload.sound.vol);
		HASH_FIELD(s_rpc_hash, call->payload.sound.pitch);
		HASH_FIELD(s_rpc_hash, call->payload.sound.ui);
		break;
	case CRPC_PLAYER_POS:
		for(unsigned k = 0; k < 3; k++)
			HASH_FIELD(s_rpc_hash, call->payload.player_pos.position[k]);
		break;
	case CRPC_SET_BLOCK:
		HASH_FIELD(s_rpc_hash, call->payload.set_block.x);
		HASH_FIELD(s_rpc_hash, call->payload.set_block.y);
		HASH_FIELD(s_rpc_hash, call->payload.set_block.z);
		{
			uint8_t ty = call->payload.set_block.block.type;
			uint8_t md = call->payload.set_block.block.metadata;
			HASH_FIELD(s_rpc_hash, ty);
			HASH_FIELD(s_rpc_hash, md);
		}
		break;
	default:
		/* Type ordinal only -- includes the pointer-carrying arms. */
		break;
	}
	clin_drain_discard();
}

/* ---- the STATE digest ---------------------------------------------------- */
static uint64_t actor_hash(uint64_t h, struct server_local* s) {
	const struct server_player* p = &s->player;
	HASH_FIELD(h, p->x);
	HASH_FIELD(h, p->y);
	HASH_FIELD(h, p->z);
	HASH_FIELD(h, p->vel_x);
	HASH_FIELD(h, p->vel_z);
	HASH_FIELD(h, p->health);
	HASH_FIELD(h, p->hurt_resist_time);
	HASH_FIELD(h, p->last_damage);
	HASH_FIELD(h, p->damage_remainder);
	HASH_FIELD(h, p->oxygen);
	HASH_FIELD(h, p->fire);
	HASH_FIELD(h, p->fall_distance);
	HASH_FIELD(h, p->sleeping);
	HASH_FIELD(h, p->has_death_pos);
	HASH_FIELD(h, p->death_x);
	HASH_FIELD(h, p->death_y);
	HASH_FIELD(h, p->death_z);
	bool plain = p->active_inventory == &p->inventory;
	HASH_FIELD(h, plain);
	size_t hotbar = inventory_get_hotbar(&s->player.inventory);
	HASH_FIELD(h, hotbar);
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data item;
		bool present = inventory_get_slot(&s->player.inventory, slot, &item);
		HASH_FIELD(h, present);
		if(present)
			h = hash_item(h, &item);
	}
	return h;
}

static uint64_t s_state_hash;
static unsigned s_step;

static void settle(struct server_local* s) {
	for(unsigned t = 0; t < SETTLE_TICKS; t++) {
		server_local_test_update_once(s);
		clin_drain_discard();
	}
}

static void step_done(struct server_local* s, const char* what) {
	(void)what;
	settle(s);
	s_step++;
	HASH_FIELD(s_state_hash, s_step);
	s_state_hash = actor_hash(s_state_hash, s);
}

/* ---- drivers ------------------------------------------------------------- */
static struct entity mob_attacker(uint8_t id, float x, float y, float z) {
	struct entity e = {0};
	e.type = ENTITY_MONSTER;
	e.data.monster.id = id;
	e.pos[0] = x;
	e.pos[1] = y;
	e.pos[2] = z;
	return e;
}

static void hold(struct server_local* s, uint16_t id, uint8_t count) {
	inventory_set_hotbar(&s->player.inventory, 0);
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = id, .count = count});
}

static void equip_armor(struct server_local* s, uint16_t helmet,
						uint16_t chest, uint16_t legs, uint16_t boots) {
	const uint16_t ids[INVENTORY_SIZE_ARMOR] = {helmet, chest, legs, boots};
	for(size_t k = 0; k < INVENTORY_SIZE_ARMOR; k++)
		inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_ARMOR + k,
						   (struct item_data) {.id = ids[k], .count = 1});
}

static void strip_armor(struct server_local* s) {
	for(size_t k = 0; k < INVENTORY_SIZE_ARMOR; k++)
		inventory_clear_slot(&s->player.inventory, INVENTORY_SLOT_ARMOR + k);
}

/* Eat one `food` by right-clicking the ground under the actor: no block
 * handles that click, so the food item onItemPlace hook runs -- the Beta eat
 * path, through the real slot-0 dispatch. */
static void eat(struct server_local* s, uint16_t food, bool expect_heal) {
	short before = s->player.health;
	hold(s, food, 2);
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place
		= {.x = 0, .y = GROUND_Y, .z = 0, .side = SIDE_TOP}};
	server_local_test_process_rpc(s, &call);
	if(expect_heal)
		assert(s->player.health > before);
	else
		assert(s->player.health == before);
}

struct result {
	uint64_t state, rpc;
	unsigned steps;
	unsigned long rpcs, health_rpcs;
};

static struct result run(void) {
	s_state_hash = FNV_BASIS;
	s_rpc_hash = FNV_BASIS;
	s_rpc_count = 0;
	s_health_rpcs = 0;
	s_step = 0;

	struct server_local* s = split_fixture();
	s->rand_src = (struct random_gen) {.seed = 24680};
	/* F4-RNG (f049fa3d8): hurt/death pitch and death scatter use this
	 * body's own runtime-seeded Entity.rand; block drops use World.rand. */
	server_local_test_seed_body_rand(s, &s->player, 13579);
	jrand_set_seed(&s->worlds[0].rand, 4242);
	*server_local_sound_rng() = (struct random_gen) {.seed = 13579};
	*audio_cosmetic_rng() = (struct random_gen) {.seed = 2468};
	server_local_test_seed_math_random(1357);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	/* The far-column gen batch is the last wall-clock governor: under host
	 * load FAR_GEN_BUDGET_MS cut a batch short and reordered the far-chunk
	 * RPC stream (flaky rpc hash on Linux). Never expire it here, so every
	 * batch runs to its FAR_SEND_BUDGET count cap. */
	s->far_gen_budget_ms = INT_MAX;
	assert(s->autosave_ticks == 0);
	s->player.active_inventory = &s->player.inventory;
	inventory_set_hotbar(&s->player.inventory, 0);

	clin_test_set_rpc_observer(observe_send, NULL);
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	for(unsigned load = 0; load < LOAD_TICKS; load++) {
		server_local_test_update_once(s);
		clin_drain_discard();
	}
	assert(s->world.current_tick == 0);
	s->input_ack_pending = 0;

	/* ---- 1. UNARMORED MELEE: the three i-frame arms ---------------------- */
	struct entity zombie = mob_attacker(MONSTER_ZOMBIE, 1.0f, GROUND_Y + 1.0f,
										0.0f);
	struct entity spider = mob_attacker(MONSTER_SPIDER, -1.0f,
										GROUND_Y + 1.0f, 0.0f);
	strip_armor(s);
	assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH));
	s->player.hurt_resist_time = 0;
	s->player.last_damage = 0;
	s->player.damage_remainder = 0;
	step_done(s, "baseline: full health, window closed");

	/* FRESH hit: full damage, a new window, the hurt sound, knockback away
	 * from the attacker. */
	server_local_hurt_player(s, &spider);
	assert(s->player.hurt_resist_time > 0);
	step_done(s, "spider melee, fresh window");

	/* ABSORBED: an equal hit inside the window changes nothing. */
	short before_absorb = s->player.health;
	server_local_hurt_player(s, &spider);
	assert(s->player.health == before_absorb);
	step_done(s, "spider melee, absorbed in-window");

	/* EXCESS-ONLY: a STRONGER hit inside the window deals only the excess and
	 * does NOT re-open the window or apply knockback (Beta var3 = false). */
	server_local_hurt_player(s, &zombie);
	assert(s->player.health < before_absorb);
	step_done(s, "zombie melee, excess-only in-window");

	/* ---- 2. ARMORED MELEE: absorb, wear and the remainder carry ---------- */
	assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH));
	s->player.hurt_resist_time = 0;
	s->player.last_damage = 0;
	s->player.damage_remainder = 0;
	equip_armor(s, ITEM_DIAMOND_HELMET, ITEM_DIAMOND_CHESTPLATE,
				ITEM_DIAMOND_LEGGINGS, ITEM_DIAMOND_BOOTS);
	step_done(s, "equip diamond armor");

	server_local_hurt_player(s, &zombie);
	step_done(s, "armored zombie melee: mitigation + wear + slot echo");

	s->player.hurt_resist_time = 0;
	server_local_hurt_player(s, &spider);
	step_done(s, "armored spider melee: second wear, remainder carry");

	/* ---- 3. FLAT DAMAGE (arrows, hazards) -------------------------------- */
	s->player.hurt_resist_time = 0;
	bool accepted = false;
	bool survived = server_local_damage_player_result(s, 32, 0.7f, -0.7f,
													 &accepted);
	assert(survived && accepted);
	step_done(s, "flat damage with knockback direction");

	s->player.hurt_resist_time = 0;
	accepted = false;
	survived = server_local_damage_player_result(s, 8, 0.0f, 0.0f, &accepted);
	assert(survived && accepted);
	step_done(s, "flat damage, no knockback direction");

	/* An i-framed repeat must be absorbed and reported as NOT accepted -- the
	 * distinction an arrow bounce depends on. */
	accepted = true;
	survived = server_local_damage_player_result(s, 8, 0.0f, 0.0f, &accepted);
	assert(survived && !accepted);
	step_done(s, "flat damage absorbed in-window (bounce case)");

	/* ---- 4. ENVIRONMENTAL causes ----------------------------------------- */
	strip_armor(s);
	assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH));
	s->player.hurt_resist_time = 0;
	s->player.fire = -PLAYER_FIRE_RESISTANCE;
	s->player.oxygen = MAX_OXYGEN;
	step_done(s, "reset for hazards");

	assert(server_local_test_apply_player_hazards(s, false, false, true,
												  false));
	step_done(s, "standing in fire: contact damage + ignition ramp");

	s->player.hurt_resist_time = 0;
	assert(server_local_test_apply_player_hazards(s, false, true, false,
												  false));
	step_done(s, "lava: 4 points + the 600-tick burn");

	s->player.hurt_resist_time = 0;
	assert(server_local_test_apply_player_hazards(s, true, false, false,
												  false));
	step_done(s, "water quench: fizz + fire cleared");

	s->player.hurt_resist_time = 0;
	assert(server_local_test_apply_player_hazards(s, false, false, false,
												  true));
	step_done(s, "cactus contact");

	/* Drowning: the oxygen counter has to walk down to the threshold, so run
	 * the head-under pass until the first hit lands. */
	s->player.oxygen = OXYGEN_THRESHOLD + 2;
	for(int k = 0; k < 4; k++) {
		s->player.hurt_resist_time = 0;
		assert(server_local_test_apply_player_hazards(s, true, false, false,
													  false));
	}
	step_done(s, "drowning: oxygen to the threshold, then the hit");

	/* ---- 5. HEALING ------------------------------------------------------ */
	assert(server_local_set_player_health(s, HEALTH_PER_HEART));
	step_done(s, "direct heal write (one heart)");

	eat(s, ITEM_APPLE, true);
	step_done(s, "eat apple");
	eat(s, ITEM_BREAD, true);
	step_done(s, "eat bread");
	eat(s, ITEM_COOKIE, true);
	step_done(s, "eat cookie");
	eat(s, ITEM_FISH, true);
	step_done(s, "eat raw fish");
	eat(s, ITEM_FISH_COOKED, true);
	step_done(s, "eat cooked fish");
	eat(s, ITEM_PORKCHOP, true);
	step_done(s, "eat raw porkchop");
	assert(server_local_set_player_health(s, HEALTH_PER_HEART));
	eat(s, ITEM_PORKCHOP_COOKED, true);
	step_done(s, "eat cooked porkchop");
	eat(s, ITEM_MUSHROOM_STEW, true);
	step_done(s, "eat mushroom stew");
	assert(server_local_set_player_health(s, HEALTH_PER_HEART));
	eat(s, ITEM_APPLE_GOLDEN, true);
	step_done(s, "eat golden apple (heals to full)");

	/* At full health Beta refuses the bite outright and consumes nothing. */
	assert(s->player.health == MAX_PLAYER_HEALTH);
	eat(s, ITEM_BREAD, false);
	step_done(s, "eat at full health: refused, nothing consumed");

	/* Cake: place a slab and take one slice through the same dispatch. */
	assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH / 2));
	hold(s, BLOCK_CAKE, 1);
	{
		struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
			.payload.block_place
			= {.x = 2, .y = GROUND_Y, .z = 0, .side = SIDE_TOP}};
		server_local_test_process_rpc(s, &place);
	}
	{
		struct block_data cake;
		assert(server_world_get_block(&s->world, 2, GROUND_Y + 1, 0, &cake)
			   && cake.type == BLOCK_CAKE);
	}
	step_done(s, "place cake");
	{
		short before = s->player.health;
		inventory_clear_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR);
		struct server_rpc bite = {.type = SRPC_BLOCK_PLACE,
			.payload.block_place
			= {.x = 2, .y = GROUND_Y + 1, .z = 0, .side = SIDE_TOP}};
		server_local_test_process_rpc(s, &bite);
		assert(s->player.health > before);
	}
	step_done(s, "eat one cake slice");

	/* ---- 6. LETHAL HIT + the death-screen dwell -------------------------- */
	equip_armor(s, ITEM_DIAMOND_HELMET, ITEM_DIAMOND_CHESTPLATE,
				ITEM_DIAMOND_LEGGINGS, ITEM_DIAMOND_BOOTS);
	assert(server_local_set_player_health(s, HEALTH_PER_HEART / 2));
	s->player.hurt_resist_time = 0;
	survived = server_local_damage_player_result(s, 8 * HEALTH_PER_HEART,
												 0.0f, 0.0f, NULL);
	assert(!survived && s->player.health <= 0);
	step_done(s, "lethal flat hit: death committed, no respawn yet");

	/* Corpse hits are absorbed BEFORE i-frames and armor: no wear, no window
	 * churn, no sound [Beta attackEntityFrom:312]. */
	{
		struct item_data helm_before;
		assert(inventory_get_slot(&s->player.inventory, INVENTORY_SLOT_ARMOR,
								  &helm_before));
		short resist_before = s->player.hurt_resist_time;
		server_local_hurt_player(s, &zombie);
		accepted = true;
		(void)server_local_damage_player_result(s, 24, 0.0f, 0.0f, &accepted);
		assert(!accepted);
		struct item_data helm_after;
		assert(inventory_get_slot(&s->player.inventory, INVENTORY_SLOT_ARMOR,
								  &helm_after));
		assert(helm_after.durability == helm_before.durability);
		assert(s->player.hurt_resist_time == resist_before);
	}
	step_done(s, "corpse hits absorbed: no wear, no window, no sound");

	/* Healing a corpse is refused [EntityLiving.heal gates on health > 0]. */
	assert(!server_local_set_player_health(s, MAX_PLAYER_HEALTH));
	eat(s, ITEM_BREAD, false);
	step_done(s, "corpse heal + corpse bite refused");

	clin_test_set_rpc_observer(NULL, NULL);

	struct result r = {.state = s_state_hash, .rpc = s_rpc_hash,
					   .steps = s_step, .rpcs = s_rpc_count,
					   .health_rpcs = s_health_rpcs};
	/* Non-vacuity: the script must actually have moved the pipeline. */
	assert(s->player.health <= 0 && s->player.has_death_pos);
	assert(r.steps >= 25 && r.rpcs > 0 && r.health_rpcs > 20);

	printf("host_damage state=%016" PRIx64 " rpc=%016" PRIx64
		   " steps=%u rpcs=%lu health_rpcs=%lu tick=%u\n",
		r.state, r.rpc, r.steps, r.rpcs, r.health_rpcs, s->world.current_tick);
	fflush(stdout);
	split_destroy(s);
	if(attr_bytes) { assert(fflush(attr_bytes) == 0); assert(fflush(attr_map) == 0); }
	return r;
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();

	struct result a = run();
	/* Determinism first: a golden is only assertable if the script is
	 * reproducible in the SAME process. */
	struct result b = run();
	if(a.state != b.state || a.rpc != b.rpc) {
		fprintf(stderr,
			"test_host_damage_golden: NON-DETERMINISTIC state %016" PRIx64
			"/%016" PRIx64 " rpc %016" PRIx64 "/%016" PRIx64 "\n",
			a.state, b.state, a.rpc, b.rpc);
		assert(false && "scripted host damage sequence is not reproducible");
	}
	printf("PASS reproducible: two runs agree (steps=%u rpcs=%lu)\n", a.steps,
		   a.rpcs);
	fflush(stdout);

	if(a.state != GOLDEN_STATE) {
		fprintf(stderr,
			"test_host_damage_golden: STATE %016" PRIx64 " != GOLDEN %016"
			PRIx64 " (base 14a84643)\n",
			a.state, GOLDEN_STATE);
		assert(false && "host damage STATE diverged from the pre-actor golden");
	}
	if(a.rpc != GOLDEN_RPC) {
		fprintf(stderr,
			"test_host_damage_golden: RPC-ORDER %016" PRIx64 " != GOLDEN "
			"%016" PRIx64 " (base 14a84643)\n",
			a.rpc, GOLDEN_RPC);
		assert(false && "host damage RPC order/payloads diverged");
	}
	printf("PASS host damage STATE golden %016" PRIx64 "\n", GOLDEN_STATE);
	printf("PASS host damage RPC-ORDER golden %016" PRIx64 "\n", GOLDEN_RPC);
	fflush(stdout);
	puts("PASS test_host_damage_golden: the actor-parameterized damage/heal "
		 "pipeline is byte-identical on the host path (S1-a3)");
	return 0;
}
