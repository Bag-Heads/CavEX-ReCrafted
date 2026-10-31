/*
	Saddle-slice end-to-end env test (SADDLE_DISPATCH.md; the
	test_animal_interact_env.c harness pattern: threadless server, direct RPC
	processing, dict scans, real disk chunk save/load for the archive round
	trip). Ground truth: decompile 740c583 (EntityPig, ItemSaddle, Item,
	ItemStack, Entity, EntityPlayer).

	Coverage:
	  - ItemSaddle.hitEntity -> saddleEntity (attack, NOT right-click -- b1.7.3
	    has no use-on-entity path for the saddle item): hitting an unsaddled
	    pig with a saddle saddles it and consumes exactly one; hitting an
	    already-saddled pig consumes nothing; hitting with a non-saddle item
	    does not saddle; the saddled flag rides the next CRPC_ENTITY_MOVE (the
	    sheared precedent).
	  - EntityPig.interact + Entity.mountEntity: refuses on an unsaddled pig
	    (no mount); mounts a saddled pig; interacting with your OWN mount
	    again dismounts (mountEntity's `ridingEntity == var1` toggle -- Beta
	    has no separate dismount path); a pig already ridden by someone/
	    something else refuses a second rider.
	  - Persistence: saddled survives the real disk archive round trip
	    (archived as Beta's own "Saddle" tag); the rider relationship does
	    NOT (Beta writes no riding relationship for any entity -- the
	    minecart-slice precedent, SADDLE_DISPATCH.md).
	  - Negative control: a ridden pig's motion over N ticks, under a pinned
	    RNG seed and identical start state, is BIT-IDENTICAL to an unridden
	    pig's -- proof that mounting adds no steering (EntityPig has no
	    moveEntityWithHeading override, no rider-input handling, in b1.7.3).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_pig_saddle_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct inventory_logic inventory_logic_player;

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* CRPC_ENTITY_MOVE saddled-flag capture for one watched entity id (the
   test_animal_interact_env.c sheared-flag capture pattern). */
static uint32_t g_watch_move_id = 0;
static bool g_watch_move_seen = false;
static bool g_watch_move_saddled = false;

static void capture_move(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_ENTITY_MOVE
	   && call->payload.entity_move.entity_id == g_watch_move_id) {
		g_watch_move_seen = true;
		g_watch_move_saddled = call->payload.entity_move.saddled;
	}
}

/* CRPC_PLAYER_POS/PLAYER_CORRECTION_POSITION_ONLY capture (review finding 1:
   the rider-follow correction must actually be networked to the client). */
static bool g_watch_correction_seen = false;
static double g_watch_correction_pos[3];

static void capture_correction(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_PLAYER_POS
	   && (call->payload.player_pos.correction_flags
		   & PLAYER_CORRECTION_POSITION_ONLY)) {
		g_watch_correction_seen = true;
		g_watch_correction_pos[0] = call->payload.player_pos.position[0];
		g_watch_correction_pos[1] = call->payload.player_pos.position[1];
		g_watch_correction_pos[2] = call->payload.player_pos.position[2];
	}
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s.entities);
}

static size_t count_monsters(int monster_id) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == monster_id)
			count++;
		dict_entity_next(it);
	}
	return count;
}

/* Find monsters of an id in dict order; index picks among multiple. */
static struct entity* find_monster(int monster_id, size_t index) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == monster_id) {
			if(index == 0)
				return e;
			index--;
		}
		dict_entity_next(it);
	}
	return NULL;
}

static void use_entity(uint32_t id) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_USE,
		.payload.entity_use.entity_id = id,
	};
	server_local_test_process_rpc(&s, &rpc);
}

static void attack_entity(uint32_t id) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = id,
	};
	server_local_test_process_rpc(&s, &rpc);
}

/* SRPC_MINECART_RIDE_INTENT (test_minecart_authority.c's send_ride_intent
   pattern) -- used only by S6 (finding 2's pig<->minecart cross-check). */
static void ride_intent(uint32_t id, uint32_t seq, bool desired) {
	struct server_rpc rpc = {
		.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {
			.entity_id = id,
			.input_epoch = s.input_epoch_counter,
			.request_seq = seq,
			.desired_occupied = desired,
		},
	};
	server_local_test_process_rpc(&s, &rpc);
}

static struct item_data* hotbar_slot(size_t k) {
	return &s.player.inventory.items[INVENTORY_SLOT_HOTBAR + k];
}

int main(void) {
	test_fixture_dir("test_pig_saddle_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = UINT32_C(0xA1B2C3D4);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	/* Superflat surface probe: highest non-air block in the (8, *, 8) column. */
	int gy = -1;
	for(int y = 127; y >= 0; y--) {
		struct block_data probe;
		if(server_world_get_block(&s.world, 8, y, 8, &probe)
		   && probe.type != BLOCK_AIR) {
			gy = y;
			break;
		}
	}
	assert(gy > 0);
	const float feet = (float)(gy + 1);
	const double eye = (double)(gy + 1) + PLAYER_EYE_HEIGHT;

	inventory_set_hotbar(&s.player.inventory, 0);
	s.player.x = 10.5;
	s.player.y = eye;
	s.player.z = 8.5;

	/* ================= S1: hit-to-saddle (ItemSaddle.hitEntity) =========== */
	struct entity* pig = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG, &s);
	assert(pig && !pig->data.monster.saddled);
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_SADDLE, .count = 1};

	attack_entity(pig->id);
	assert(pig->data.monster.saddled);
	assert(hotbar_slot(0)->id == 0 && hotbar_slot(0)->count == 0);
	puts("  S1: hit unsaddled pig with saddle -> saddled + consumed exactly 1");

	/* saddled rides the pig's next trailing CRPC_ENTITY_MOVE (sheared
	   precedent: on_fire/swelling/sheared all ride every MOVE). */
	g_watch_move_id = pig->id;
	g_watch_move_seen = false;
	g_watch_move_saddled = false;
	clin_test_set_rpc_observer(capture_move, NULL);
	assert(!pig->tick_server(pig, &s));
	clin_test_set_rpc_observer(NULL, NULL);
	assert(g_watch_move_seen && g_watch_move_saddled);
	puts("  S1: saddled flag rides the next CRPC_ENTITY_MOVE");

	/* Hitting an ALREADY-saddled pig with a saddle consumes nothing
	   [ItemSaddle.saddleEntity: `if (!var3.getSaddled())` guards the
	   decrement -- Beta's own double-saddle guard]. */
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_SADDLE, .count = 1};
	attack_entity(pig->id);
	assert(pig->data.monster.saddled);
	assert(hotbar_slot(0)->id == ITEM_SADDLE && hotbar_slot(0)->count == 1);
	puts("  S1: hitting an already-saddled pig consumes nothing");

	/* Hitting with a NON-saddle item does not saddle (a fresh pig, so this
	   is not shadowed by the already-saddled guard above). */
	clear_entities();
	struct entity* pig2 = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG, &s);
	assert(pig2 && !pig2->data.monster.saddled);
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_STICK, .count = 1};
	attack_entity(pig2->id);
	assert(!pig2->data.monster.saddled);
	assert(hotbar_slot(0)->id == ITEM_STICK && hotbar_slot(0)->count == 1);
	puts("  S1: hitting with a non-saddle item does not saddle");

	/* ================= S2: interact/mount ================================= */
	/* Refuses on an UNSADDLED pig: no mount [`!this.getSaddled()` -> false]. */
	use_entity(pig2->id);
	assert(!pig2->data.monster.pig_ridden);
	puts("  S2: interact on an unsaddled pig refuses (no mount)");

	/* Saddle it directly -- isolates the mount test from the hit mechanic,
	   already independently proven in S1 -- then mount. */
	pig2->data.monster.saddled = true;
	use_entity(pig2->id);
	assert(pig2->data.monster.pig_ridden);
	assert(pig2->data.monster.pig_rider_id == SERVER_LOCAL_PLAYER_ENTITY_ID);
	puts("  S2: interact on a saddled pig mounts");

	/* Interacting with your OWN mount again TOGGLES you off: Beta
	   Entity.mountEntity's `this.ridingEntity == var1` branch IS the
	   dismount path -- there is no separate one. */
	use_entity(pig2->id);
	assert(!pig2->data.monster.pig_ridden);
	puts("  S2: interacting with your own mount again dismounts (toggle)");

	/* A second rider is refused while occupied by someone/something else
	   [`riddenByEntity != null && riddenByEntity != var1`]. This engine
	   models exactly one player, so the refusal branch is proven by directly
	   manufacturing an "occupied by someone else" state -- the same
	   whitebox-negative-control style the rest of this suite uses for
	   scenarios a single-player harness cannot reach through real input. */
	pig2->data.monster.pig_ridden = true;
	pig2->data.monster.pig_rider_id = 0xDEADBEEFu; /* not the local player */
	use_entity(pig2->id);
	assert(pig2->data.monster.pig_ridden
		   && pig2->data.monster.pig_rider_id == 0xDEADBEEFu);
	puts("  S2: a second player's interact is refused while occupied");

	/* ================= S3: persistence -- saddled survives, rider does NOT */
	pig2->data.monster.saddled = true;
	pig2->data.monster.pig_ridden = true;
	pig2->data.monster.pig_rider_id = SERVER_LOCAL_PLAYER_ENTITY_ID;

	test_drain_quiesce();
	assert(server_world_save_chunk(&s.world, true, 0, 0));
	assert(!server_world_is_chunk_loaded(&s.world, 0, 0));
	assert(count_monsters(MONSTER_PIG) == 0);
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	struct entity* restored = find_monster(MONSTER_PIG, 0);
	assert(restored && "saddled pig must survive the disk round trip");
	assert(restored->data.monster.saddled);
	assert(!restored->data.monster.pig_ridden
		   && "Beta persists NO rider relationship for any entity");
	assert(restored->data.monster.pig_rider_id == 0);
	puts("  S3: saddled survives save/evict/reload; rider does NOT");

	/* ================= S4: negative control -- no steering added ========== */
	/* Two otherwise-identical pigs, spawned at the same position under the
	   same pinned mob-owned seed and canonical zero newborn velocity.
	   One is saddled +
	   ridden; the other is left at its plain constructor defaults. If
	   mounting ever reads pig_ridden on the movement path, the two
	   trajectories diverge; EntityPig has no moveEntityWithHeading override
	   in b1.7.3, so the true expectation is bit-identical output, hand-
	   derived from the decompile's absence of any such override -- not from
	   running this engine's own code twice and hoping they match. */
	clear_entities();
	const uint32_t seed = UINT32_C(777);
	const int ticks = 60;

	s.rand_src.seed = seed;
	struct entity* control = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG, &s);
	assert(control);
	jrand_set_seed(&control->gameplay_rng, (int64_t)(uint64_t)seed);
	for(int i = 0; i < ticks; i++)
		(void)control->tick_server(control, &s);
	vec3 control_pos, control_vel;
	glm_vec3_copy(control->pos, control_pos);
	glm_vec3_copy(control->vel, control_vel);
	uint64_t seed_after_control = control->gameplay_rng.seed;
	clear_entities();

	s.rand_src.seed = seed;
	struct entity* ridden = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG, &s);
	assert(ridden);
	jrand_set_seed(&ridden->gameplay_rng, (int64_t)(uint64_t)seed);
	ridden->data.monster.saddled = true;
	ridden->data.monster.pig_ridden = true;
	ridden->data.monster.pig_rider_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
	for(int i = 0; i < ticks; i++)
		(void)ridden->tick_server(ridden, &s);

	assert(control_pos[0] == ridden->pos[0] && control_pos[1] == ridden->pos[1]
		   && control_pos[2] == ridden->pos[2]);
	assert(control_vel[0] == ridden->vel[0] && control_vel[1] == ridden->vel[1]
		   && control_vel[2] == ridden->vel[2]);
	assert(seed_after_control == ridden->gameplay_rng.seed
		   && "ridden pig must draw exactly the same RNG as the control");
	puts("  S4: a ridden pig's motion is bit-identical to an unridden pig's "
		 "under the same pinned RNG (no steering added)");

	/* ===== S5 (adversarial review finding 1, CRITICAL): a mounted rider is
	   actually force-followed onto the pig every tick; free movement is
	   gated; the mount's death auto-dismounts its rider. Before this fix,
	   pig_ridden/pig_rider_id were pure state flags nothing else read. ===== */
	clear_entities();
	s.rand_src.seed = UINT32_C(0xF01D);
	struct entity* mount = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG, &s);
	assert(mount);
	mount->data.monster.saddled = true;
	mount->data.monster.pig_ridden = true;
	mount->data.monster.pig_rider_id = SERVER_LOCAL_PLAYER_ENTITY_ID;

	/* Manufacture the EXACT "player freely walks off" scenario finding 1
	   describes: a free/rogue position (as if an ordinary SRPC_PLAYER_POS
	   had just been accepted) and nonzero velocity, set BEFORE ticking the
	   mount. If the follow is real, both must be overwritten this tick.
	   20 blocks off (not e.g. 500): server_tick_monster's PRE-EXISTING
	   distance-based despawn check (combat.h mob_despawn_action, Beta's
	   own AGE-INDEPENDENT >128-block instant despawn) reads s->player.xyz
	   too -- a wildly distant rogue position would silently despawn the
	   pig before this tick ever reached the new follow code, which tests
	   the despawn gate by accident instead of the follow. Staying well
	   under 128 blocks keeps this a clean, single-variable test. */
	s.player.x = mount->pos[0] + 20.0;
	s.player.y = eye;
	s.player.z = mount->pos[2];
	s.player.vel_x = 3.0f;
	s.player.vel_y = 3.0f;
	s.player.vel_z = 3.0f;
	uint32_t seq_before = s.player.correction_seq;

	g_watch_correction_seen = false;
	clin_test_set_rpc_observer(capture_correction, NULL);
	bool reaped_this_tick = mount->tick_server(mount, &s);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(!reaped_this_tick);

	double expect_y = (double)mount->pos[1] + (double)PIG_MOUNTED_Y_OFFSET
		+ (double)PLAYER_EYE_HEIGHT;
	assert(s.player.x == (double)mount->pos[0]
		   && "a mounted rider's free-walk position must be overwritten by "
			  "the server-authoritative follow (finding 1: gate free "
			  "movement)");
	assert(s.player.y == expect_y
		   && "rider Y must be pig.y + PIG_MOUNTED_Y_OFFSET + "
			  "PLAYER_EYE_HEIGHT [740c583 Entity.updateRiderPosition]");
	assert(s.player.z == (double)mount->pos[2]);
	assert(s.player.vel_x == 0.0f && s.player.vel_y == 0.0f
		   && s.player.vel_z == 0.0f
		   && "Entity.updateRidden zeroes the rider's own motion every tick");
	assert(s.player.correction_seq != seq_before
		   && "the follow must own a fresh position-correction token");
	assert(g_watch_correction_seen
		   && "the follow must be networked to the client (finding 1)");
	/* Epsilon, not ==: the wire payload's `position` is a vec3 (float[3]), so
	   s.player.y (a double SUM of three doubled floats -- e->pos[1] +
	   PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT) legitimately loses a few ULP
	   narrowing through that float on the wire. That precision boundary is
	   the SAME one every other position-carrying RPC in this codebase
	   crosses (CRPC_ENTITY_MOVE's pos field, etc.) -- not a defect. */
	assert(fabs(g_watch_correction_pos[0] - s.player.x) < 1e-3
		   && fabs(g_watch_correction_pos[1] - s.player.y) < 1e-3
		   && fabs(g_watch_correction_pos[2] - s.player.z) < 1e-3
		   && "the networked correction must match the server's followed "
			  "position (within the wire vec3's float precision)");
	puts("  S5: a mounted rider is force-followed onto the pig every tick "
		 "(free movement gated, velocity zeroed, correction networked)");

	/* Mount death auto-dismounts: drive health to 0 and tick through the
	   ~1s death flop to the reap transition (Beta deathTime > 20). */
	mount->health = 0;
	bool reaped = false;
	for(int i = 0; i < 25 && !reaped; i++)
		reaped = mount->tick_server(mount, &s);
	assert(reaped && "the pig must reap within the 20-tick flop + margin");
	assert(!mount->data.monster.pig_ridden
		   && "a mount's death must auto-dismount its rider (finding 1)");
	assert(mount->data.monster.pig_rider_id == 0);
	puts("  S5: a mount's death auto-dismounts its rider");

	/* ===== S6 (adversarial review finding 2): cross-mount exclusivity --
	   Entity.mountEntity's single-rider invariant. Before this fix, mounting
	   a second pig (or a minecart) left BOTH mounts simultaneously claiming
	   the player. ===== */
	clear_entities();
	s.player.x = 10.5; s.player.y = eye; s.player.z = 8.5;
	struct entity* pigA = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG, &s);
	struct entity* pigB = server_local_spawn_monster_at(
		(vec3) {9.5f, feet, 8.5f}, MONSTER_PIG, &s);
	assert(pigA && pigB);
	pigA->data.monster.saddled = true;
	pigB->data.monster.saddled = true;

	use_entity(pigA->id);
	assert(pigA->data.monster.pig_ridden);
	use_entity(pigB->id);
	assert(pigB->data.monster.pig_ridden
		   && pigB->data.monster.pig_rider_id == SERVER_LOCAL_PLAYER_ENTITY_ID);
	assert(!pigA->data.monster.pig_ridden
		   && "mounting pig B must release the previously ridden pig A "
			  "(Entity.mountEntity's single-rider invariant)");
	assert(pigA->data.monster.pig_rider_id == 0);
	puts("  S6: mounting a new pig releases the previously ridden pig");

	use_entity(pigB->id); /* toggle dismount, proven by S2 already */
	assert(!pigB->data.monster.pig_ridden);

	/* pig -> minecart: mounting a cart while riding a pig releases the pig. */
	s.input_epoch_counter = 1; /* F3: ride-intent requires nonzero epoch */
	struct entity* cart = server_local_spawn_minecart(
		(vec3) {8.5f, feet, 9.4f}, &s);
	assert(cart && cart->type == ENTITY_MINECART);

	use_entity(pigA->id);
	assert(pigA->data.monster.pig_ridden);
	ride_intent(cart->id, 1, true);
	assert(cart->data.minecart.occupied
		   && cart->data.minecart.occupant_id
				  == SERVER_LOCAL_PLAYER_ENTITY_ID
		   && "the ride-intent mount itself must succeed");
	assert(!pigA->data.monster.pig_ridden
		   && "mounting a minecart must release a previously ridden pig "
			  "(finding 2)");
	assert(pigA->data.monster.pig_rider_id == 0);
	puts("  S6: mounting a minecart releases a previously ridden pig");

	/* minecart -> pig: mounting a pig while riding a cart releases the cart. */
	use_entity(pigB->id);
	assert(pigB->data.monster.pig_ridden
		   && "mounting pig B must succeed while a minecart is ridden");
	assert(!cart->data.minecart.occupied
		   && "mounting a pig must release a previously ridden minecart "
			  "(finding 2)");
	puts("  S6: mounting a pig releases a previously ridden minecart");

	/* ===== S7 (adversarial review finding 4): a non-pig target is IMMUNE to
	   the saddle item -- both at the call site's outer MONSTER_PIG gate AND
	   (via server_local_saddle_pig's own defense-in-depth check) inside the
	   function itself. Uses a cow: `saddled` is a plain field shared across
	   every monster kind in the entity_monster struct (not a per-type
	   union), so asserting it stays false is a real, non-tautological check
	   -- nothing else in this engine ever writes it for a cow either way. */
	clear_entities();
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 10.5; s.player.y = eye; s.player.z = 8.5;
	s.player.active_inventory = &s.player.inventory;
	struct entity* cow = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_COW, &s);
	assert(cow && !cow->data.monster.saddled);
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_SADDLE, .count = 1};
	attack_entity(cow->id);
	assert(!cow->data.monster.saddled
		   && "a non-pig monster must never be saddled by ItemSaddle.hitEntity");
	assert(hotbar_slot(0)->id == ITEM_SADDLE && hotbar_slot(0)->count == 1
		   && "the saddle must not be consumed on a non-pig target");
	puts("  S7: hitting a non-pig monster (cow) with a saddle is a no-op "
		 "(saddle not consumed, target not saddled)");

	/* ===== S8 (adversarial review finding 5): SRPC_ENTITY_ATTACK's saddle
	   consumption is reach-gated (distSq < 36.0, the same 6-block feet-basis
	   reach SRPC_ENTITY_USE and the vehicle-breaking handler already use).
	   Before this fix, ANY entity id, at ANY distance, could be saddled. ==== */
	clear_entities();
	struct entity* far_pig = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 100.5f}, MONSTER_PIG, &s); /* ~92 blocks in Z */
	assert(far_pig && !far_pig->data.monster.saddled);
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_SADDLE, .count = 1};
	attack_entity(far_pig->id);
	assert(!far_pig->data.monster.saddled
		   && "a pig outside the 6-block reach must not be saddled");
	assert(hotbar_slot(0)->id == ITEM_SADDLE && hotbar_slot(0)->count == 1
		   && "the saddle must not be consumed on an out-of-reach attack");
	puts("  S8: attacking a pig outside 6-block reach with a saddle held "
		 "does not saddle it (finding 5 reach gate)");

	/* ===== S9 (adversarial review finding 3): player death dismounts a
	   ridden pig, mirroring the pre-existing minecart death-dismount sweep
	   in server_local_prepare_respawn_plan. Before this fix, pig_ridden
	   survived the dead player's own respawn and silently absorbed the
	   FIRST post-respawn interact as a phantom dismount. Placed LAST: a
	   real death commits an M5 world-switch (server_local_world_switch_begin,
	   SERVER_WORLD_SWITCH_DEATH) that leaves s->player.has_pos/finished_
	   loading false until further server ticks complete the reload -- this
	   test does not drive those ticks, so every SRPC_ENTITY_ATTACK/USE
	   handler's OWN top-of-case liveness gate would silently no-op any
	   later section placed after this one, making it pass for the wrong
	   reason (discovered empirically: S7/S8 originally sat after this
	   section and stayed green under a mutated, broken finding-5 gate). ===== */
	clear_entities();
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 10.5; s.player.y = eye; s.player.z = 8.5;
	s.player.spawn_x = 8; s.player.spawn_y = gy + 1; s.player.spawn_z = 8;
	s.player.has_bed_spawn = false;
	s.player.active_inventory = &s.player.inventory;
	struct entity* doomed_pig = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG, &s);
	assert(doomed_pig);
	doomed_pig->data.monster.saddled = true;
	doomed_pig->data.monster.pig_ridden = true;
	doomed_pig->data.monster.pig_rider_id = SERVER_LOCAL_PLAYER_ENTITY_ID;

	assert(server_local_set_player_health(&s, 0)
		   && "the shared lethal-damage funnel must accept the kill");
	/* Deviation-#18 split: the dismount sweep lives in the RESPAWN
	 * transaction (prepare_respawn_plan) -- the corpse stays seated through
	 * the death-screen dwell, and the sweep runs at the explicit request. */
	assert(doomed_pig->data.monster.pig_ridden
		   && "the corpse stays seated through the dwell");
	assert(server_local_respawn_request(&s)
		   && "the explicit respawn must run the transaction");
	assert(!doomed_pig->data.monster.pig_ridden
		   && "the respawn must dismount a ridden pig (finding 3)");
	assert(doomed_pig->data.monster.pig_rider_id == 0);
	puts("  S9: respawn dismounts a ridden pig (no phantom-dismount "
		 "trap on the next post-respawn interact)");

	clear_entities();
	g_drain_run = 0;
	thread_join(&drain);
	puts("test_pig_saddle_env: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
