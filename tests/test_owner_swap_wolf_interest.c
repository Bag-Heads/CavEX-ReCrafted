/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924 lane C, C2 row #20: wolf interest (the ambient
 * look-at latch + looksWithInterest) selects an eligible actor of the wolf's
 * own world, owner-swapped.
 *
 * Beta 1.7.3 (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - EntityLiving.updatePlayerActionState :695-710: latch
 *     `worldObj.getClosestPlayerToEntity(this, 8.0)`; drop it when
 *     numTicksToChaseTarget runs out or the target is > 8 away.
 *   - World.getClosestPlayer :2166-2180: that world's players only; first
 *     strict minimum (host first on a tie).
 *   - EntityWolf.onLivingUpdate :115-130: looksWithInterest = latched target
 *     is a player holding a bone (untamed) / favourite meat (tamed), the
 *     wolf has no path and is not angry.
 *
 * One executable, one case per process (argv[1]):
 *   guest_bone   host 60 away empty-handed, guest 3 away holding a bone:
 *                the wolf keeps its latch and looks with interest (RED on
 *                base: the host alone was measured and consulted)
 *   host_bone    mirror: host 3 away with the bone, guest 60 away
 *   tie_rng      both at the same spot vs the host alone: same interest,
 *                same latch, same RNG stream
 *   no_actor     wolf in a world (slot 1) with no eligible actor while the
 *                guest stands next to it in slot 0 holding a bone: no
 *                interest, latch dropped (RED on base) */
#include "support/owner_swap_harness.h"
#include "entity/combat.h"
#include "entity/entity_wolf.h"

#define TEST_NAME "test_owner_swap_wolf_interest"

/* Fable C20 F1: a FIXED world-RNG seed, so the wolf's nextInt(80) wander
 * roll (which can hand it a path and close hasPath()) is the same on every
 * run instead of a ~2.5% flake. Seed chosen so no wander path is taken on the
 * tested tick (asserted by each case's "no path" check). */
#define WOLF_TEST_SEED 0x5EED0020u

static struct entity* wolf_at(struct server_local* s, float x, float z) {
	s->rand_src.seed = WOLF_TEST_SEED;
	struct entity* e = server_local_spawn_monster_at(
		(vec3) {x, 4.0F, z}, MONSTER_WOLF, s);
	assert(e && entity_is_wolf(e));
	/* Latched on a player, as after an accepted 0.02 roll. */
	e->data.monster.wolf_has_current_target = true;
	e->data.monster.wolf_current_target_hold = 25;
	return e;
}
static void place(struct server_player* p, double x, double z, bool bone) {
	p->x = x;
	p->z = z;
	p->y = 4.0 + PLAYER_EYE_HEIGHT;
	inventory_set_slot(&p->inventory,
		INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&p->inventory),
		bone ? (struct item_data) {.id = ITEM_BONE, .count = 1}
			 : (struct item_data) {0});
}
static void tick_mob(struct server_local* s, struct entity* e) {
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	(void)e->tick_server(e, s);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
}

static void case_bone(bool guest_has_it) {
	struct server_local* s = admitted_fixture();
	struct entity* e = wolf_at(s, 8.5F, 8.5F);
	place(&s->player, guest_has_it ? 68.5 : 11.5, 8.5, !guest_has_it);
	place(s->guest_actor, guest_has_it ? 11.5 : 68.5, 8.5, guest_has_it);
	tick_mob(s, e);
	bool pathless = e->data.monster.path.length == 0;
	check(e->data.monster.wolf_has_current_target,
		guest_has_it ? "the latch holds on the guest 3 blocks away (host 60 away)"
					 : "the latch holds on the host 3 blocks away");
	check(pathless, "setup: the wolf took no path this tick (hasPath false)");
	check(e->data.monster.wolf_interested,
		guest_has_it ? "the wolf looks with interest at the guest's bone"
					 : "the wolf looks with interest at the host's bone");
	teardown(s);
}

static void case_tie_rng(void) {
	struct server_local* s = admitted_fixture();
	struct entity* e = wolf_at(s, 8.5F, 8.5F);
	place(&s->player, 11.5, 8.5, true);
	place(s->guest_actor, 11.5, 8.5, false);
	struct entity latched = *e; /* pre-latched copy (wolf_at latches) */
	e->data.monster.wolf_has_current_target = false; /* exercise the roll */
	struct entity snap = *e;
	struct random_gen rng0 = s->rand_src;
	struct server_player* guest = s->guest_actor;

	s->guest_actor = NULL;
	tick_mob(s, e);
	struct random_gen rng_host = s->rand_src;
	bool latch_host = e->data.monster.wolf_has_current_target;
	bool interest_host = e->data.monster.wolf_interested;
	s->guest_actor = guest;

	*e = snap;
	s->rand_src = rng0;
	tick_mob(s, e);
	check(s->rand_src.seed == rng_host.seed,
		"tie: the RNG stream equals the host-only tick");
	check(e->data.monster.wolf_has_current_target == latch_host
		&& e->data.monster.wolf_interested == interest_host,
		"tie: the host governs (same latch and interest)");

	/* Non-vacuous tie: pre-latched, the tied host (bone) governs -- the
	 * guest beside it holds nothing, so interest proves the host won. */
	*e = latched;
	s->rand_src.seed = WOLF_TEST_SEED;
	tick_mob(s, e);
	check(e->data.monster.path.length == 0,
		"tie (latched): no wander path this tick");
	check(e->data.monster.wolf_interested,
		"tie (latched): the host wins the tie -- its bone interests the wolf");
	teardown(s);
}

static void case_no_actor(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	struct entity* e = wolf_at(s, 8.5F, 8.5F);
	place(&s->player, 11.5, 8.5, true);
	place(s->guest_actor, 9.5, 8.5, true);
	/* The wolf belongs to slot 1 (A2-E stamps entities by slot+generation,
	 * so the stamp moves with the pointer); both bodies are in slot 0. */
	uint8_t slot0 = e->world_slot;
	uint32_t gen0 = e->world_generation;
	e->world = b;
	e->world_slot = 1;
	e->world_generation = b->generation;
	check(server_local_entity_world(s, e) == b, "setup: the wolf resolves to slot 1");
	tick_mob(s, e);
	check(!e->data.monster.wolf_has_current_target
		&& !e->data.monster.wolf_interested,
		"no eligible actor in the wolf's world: the latch drops and no bone "
		"in another world interests it");
	e->world = &s->worlds[0];
	e->world_slot = slot0;
	e->world_generation = gen0;
	server_world_discard(b);
	teardown(s);
}

int main(int argc, char** argv) {
	harness_init();
	const char* which = argc > 1 ? argv[1] : "";
	if(!strcmp(which, "guest_bone"))
		case_bone(true);
	else if(!strcmp(which, "host_bone"))
		case_bone(false);
	else if(!strcmp(which, "tie_rng"))
		case_tie_rng();
	else if(!strcmp(which, "no_actor"))
		case_no_actor();
	else {
		printf("unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
