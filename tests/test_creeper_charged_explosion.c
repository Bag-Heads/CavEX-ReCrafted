/*
	PARITY FIX (audit register 2026-08-25, entities-mobs #1): a charged (lightning-
	struck) creeper's blast power must stay 6.0F through BOTH detonation paths --
	the direct, same-tick success and the cap-DEFERRED retry -- never silently
	downgrading to the unpowered 3.0F. Golden-first, hand-derived from the
	decompile before the fix (entity_monster.c:
	entity_monster_retry_pending_explosion) landed.

	Ground truth: EntityCreeper.attackEntity [740c583]:
	    if (timeSinceIgnited >= 30) {
	        if (getPowered()) worldObj.createExplosion(this, posX, posY, posZ, 6.0F);
	        else               worldObj.createExplosion(this, posX, posY, posZ, 3.0F);
	        setEntityDead();
	    }
	ONE unconditional, single-attempt operation -- Beta never splits it into a
	deferred retry, so a charged creeper's blast strength can never silently
	change between "would explode now" and "actually explodes once the Wii
	admission cap has room".

	Harness shape from tests/test_tnt_primed_env.c's cap-saturation pattern
	(server_world_try_begin_explosion x FCAVEX_MAX_EXPLOSIONS_PER_TICK) and
	tests/test_mob_snow_walk_env.c's env boilerplate. The blast POWER itself is
	read directly off the CRPC_ENTITY_EXPLODE RPC's `payload.entity_explode.power`
	field via clin_test_set_rpc_observer -- a precise, deterministic signal,
	unlike inferring power from the probabilistic ray-cast block-destruction
	radius (server_world_explode_now uses the seeded s->rand_src per ray, so the
	destroyed-block SET is not a clean power discriminator; the RPC payload is).
*/

#include <assert.h>
#include <stdbool.h>
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
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h" // FCAVEX_MAX_EXPLOSIONS_PER_TICK

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_creeper_charged_explosion_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static bool g_saw_explode = false;
static float g_explode_power = -1.0f;

static void observe_explode(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_ENTITY_EXPLODE) {
		g_saw_explode = true;
		g_explode_power = call->payload.entity_explode.power;
	}
}

static void despawn_all_mobs(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_MONSTER) {
				uint32_t id = e->id;
				dict_entity_erase(s.entities, id);
				again = true;
				break;
			}
			dict_entity_next(it);
		}
	}
}

// Spawn a creeper primed to detonate on its very next tick_server call: fuse
// pre-set at the ready threshold (entity_monster.c: `ready_to_explode =
// e->data.monster.fuse >= CREEPER_FUSE_TICKS;` is computed BEFORE the
// proximity-gated increment branch, so this bypasses needing real player
// proximity/LOS -- the same shortcut test_tnt_primed_env.c's `durable_creeper`
// fixture already relies on).
static struct entity* spawn_primed_creeper(bool charged) {
	vec3 pos = {8.5f, 100.5f, 8.5f};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_CREEPER, &s);
	assert(m);
	m->data.monster.fuse = CREEPER_FUSE_TICKS;
	m->data.monster.charged = charged;
	return m;
}

// (A) Direct detonation (admission cap has room): power must match `charged`
// on the SAME tick the blast happens, for both the charged and unpowered case
// (the negative control -- CODESTYLE.md 4.3).
static void test_direct_detonation(void) {
	despawn_all_mobs();
	server_world_begin_update(&s.world);

	struct entity* charged = spawn_primed_creeper(true);
	g_saw_explode = false;
	g_explode_power = -1.0f;
	bool reaped = charged->tick_server(charged, &s);
	assert(reaped && "a successful direct detonation reaps the creeper this tick");
	assert(g_saw_explode && "CRPC_ENTITY_EXPLODE must fire on direct detonation");
	assert(g_explode_power == CREEPER_EXPLOSION_POWER_CHARGED
		   && "a charged creeper's DIRECT blast must be the powered 6.0F");
	despawn_all_mobs();

	// Negative control: an UNCHARGED creeper detonating directly must still be
	// the ordinary 3.0F -- the fix must not have flipped every creeper to
	// charged strength.
	server_world_begin_update(&s.world);
	struct entity* plain = spawn_primed_creeper(false);
	g_saw_explode = false;
	g_explode_power = -1.0f;
	reaped = plain->tick_server(plain, &s);
	assert(reaped);
	assert(g_saw_explode);
	assert(g_explode_power == CREEPER_EXPLOSION_POWER
		   && "an UNcharged creeper's direct blast stays the unpowered 3.0F");
	despawn_all_mobs();
}

// (B) Deferred (cap-retried) detonation: the FIRST attempt is rejected by a
// saturated admission cap (the creeper becomes the durable pending owner,
// explosion_pending=true, no RPC yet); once the cap resets and the retry
// succeeds (server_tick_monster's `entity_monster_has_pending_explosion` gate
// -> entity_monster_retry_pending_explosion, the PRODUCTION entry point, not
// the helper called directly -- CODESTYLE.md 4.3), the power must STILL be the
// charged 6.0F. This is the exact bug the audit found: the retry path used to
// hardcode CREEPER_EXPLOSION_POWER (3.0F), never reading `charged`.
static void test_deferred_detonation_preserves_charged_power(void) {
	despawn_all_mobs();

	// Saturate the per-update explosion admission cap so the creeper's first
	// attempt is rejected (test_explosion_caps.c / test_tnt_primed_env.c's
	// proven pattern).
	server_world_begin_update(&s.world);
	for(int i = 0; i < FCAVEX_MAX_EXPLOSIONS_PER_TICK; i++)
		assert(server_world_try_begin_explosion(&s.world));

	struct entity* charged = spawn_primed_creeper(true);
	g_saw_explode = false;
	g_explode_power = -1.0f;
	bool reaped = charged->tick_server(charged, &s);
	assert(!reaped && "a cap-rejected creeper is retained as the durable owner");
	assert(charged->data.monster.explosion_pending);
	assert(entity_monster_has_pending_explosion(charged));
	assert(!g_saw_explode
		   && "no CRPC_ENTITY_EXPLODE yet -- the blast has not happened");

	// Reset the cap (a fresh logical update) and retry through the SAME
	// production entry point (tick_server), not the retry helper directly.
	server_world_begin_update(&s.world);
	reaped = charged->tick_server(charged, &s);
	assert(reaped && "the retry succeeds once the cap has room again");
	assert(g_saw_explode && "the deferred blast must eventually fire");
	assert(g_explode_power == CREEPER_EXPLOSION_POWER_CHARGED
		   && "THE FIX: a charged creeper's DEFERRED/retried blast must still "
			  "be 6.0F, not silently downgraded to the unpowered 3.0F");
	despawn_all_mobs();

	// Negative control: an uncharged creeper's deferred retry stays 3.0F too
	// (the fix reads `charged`, it doesn't hardcode 6.0F either).
	server_world_begin_update(&s.world);
	for(int i = 0; i < FCAVEX_MAX_EXPLOSIONS_PER_TICK; i++)
		assert(server_world_try_begin_explosion(&s.world));
	struct entity* plain = spawn_primed_creeper(false);
	g_saw_explode = false;
	g_explode_power = -1.0f;
	reaped = plain->tick_server(plain, &s);
	assert(!reaped);
	assert(plain->data.monster.explosion_pending);
	server_world_begin_update(&s.world);
	reaped = plain->tick_server(plain, &s);
	assert(reaped);
	assert(g_saw_explode);
	assert(g_explode_power == CREEPER_EXPLOSION_POWER
		   && "an uncharged creeper's deferred retry stays the unpowered 3.0F");
	despawn_all_mobs();
}

int main(void) {
	test_fixture_dir("test_creeper_charged_explosion_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
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
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.rand_src.seed = 0x51DEB001u;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	clin_test_set_rpc_observer(observe_explode, NULL);
	test_direct_detonation();
	test_deferred_detonation_preserves_charged_power();
	clin_test_set_rpc_observer(NULL, NULL);

	printf("test_creeper_charged_explosion: charged-creeper blast power (6.0F) "
		   "survives BOTH the direct and the cap-deferred/retried detonation "
		   "path (uncharged 3.0F confirmed unchanged as a negative control)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
