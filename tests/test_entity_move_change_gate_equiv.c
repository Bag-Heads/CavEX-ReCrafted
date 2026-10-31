/*
	T1 equivalence proof for server_tick_monster's trailing CRPC_ENTITY_MOVE
	change-gate (entity_monster.c; the "server_tick_monster sends an
	unconditional 784-byte CRPC_ENTITY_MOVE every tick through the 16-slot
	BLOCKING client inbox" fix).

	This is pure wire-format bookkeeping, not a Beta value or a simulated
	behavior -- e->pos and every monster.* field are computed by the exact
	same code, in the exact same order, as before the gate; only whether a
	given tick's CRPC_ENTITY_MOVE actually goes out is new. So the proof
	obligation is not decompile parity, it is CLIENT-OBSERVABLE equivalence:
	drive a real server world for many ticks with several mobs, capture
	EVERY CRPC_ENTITY_MOVE the client would receive (clin_test_set_rpc_observer,
	the test_pig_saddle_env.c capture_move idiom, generalized to every
	networked field and to a per-entity send counter), reconstruct each mob's
	client-visible state from that received stream, and diff it against the
	server's live state.

	Coverage:
	  1. IDLE mob (a sitting wolf -- Beta's isMovementCeased draws zero AI
	     RNG and applies zero steering, so this is the one mob state
	     genuinely guaranteed motionless without hand-pinning position every
	     tick): sends exactly ONCE across a run shorter than the periodic
	     fallback window, proving the gate actually skips redundant sends.
	  2. The SAME idle mob run past MOB_MOVE_RESEND_INTERVAL_TICKS: a SECOND
	     send fires at exactly that boundary even though nothing changed --
	     the bounded fallback resend (the lane's documented answer to "client
	     reconnect / world reload must resend", an event this file cannot
	     observe without touching source/network/).
	  3. ACTIVE mob (the same kind of wolf, externally perturbed one axis of
	     position every tick, simulating "this entity's server position
	     genuinely changes every tick"): sends EVERY tick -- the gate never
	     suppresses a real change.
	  4. A NON-position field change (on_fire) on an otherwise-idle mob:
	     triggers an immediate resend on the very next tick, not just at the
	     periodic boundary.
	  5. Byte-identical final state: for every mob above, the client-side
	     mirror reconstructed purely from received CRPC_ENTITY_MOVE payloads
	     matches the server's live entity state field-for-field, at every
	     tick sampled -- the gate never lets the client observe stale data.
	  6. STATIONARY DEATH (coordinator fix-up, 2026-09-02): a mob killed
	     while otherwise perfectly static (position frozen the instant it
	     dies -- server_tick_monster skips all AI/movement for a dying mob)
	     still gets death_time delivered on EVERY tick of the ~20-tick
	     death-roll flop, never suppressed and never waiting for the
	     periodic keepalive -- because that flop is sent from a separate,
	     structurally-unconditional CRPC_ENTITY_MOVE site the T1-gated
	     trailing site (which hardcodes death_time=0) can never be reached
	     from. The mirror's death_time is asserted equal to the server's
	     death_time on every single flop tick.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/entity_wolf.h"
#include "entity/monsters_object.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "item/inventory.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_entity_move_change_gate_equiv_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct inventory_logic inventory_logic_player;

static struct server_local s;
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set_block(int x, int y, int z, int type) {
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = 0}));
}

// Per-entity client-visible mirror, rebuilt PURELY from received
// CRPC_ENTITY_MOVE payloads (the client_interface.c CRPC_ENTITY_MOVE
// handler's own field list, mirrored field-for-field -- position lands in
// "pos" here the same way the real client's network_pos is the wire value,
// not the render-smoothed e->pos).
struct move_mirror {
	uint32_t entity_id;
	bool seen;
	int send_count;
	float pos[3];
	bool on_fire, swelling, hurt;
	int8_t fuse;
	bool sheared, saddled;
	uint8_t fleece_color;
	bool ghast_attacking;
	uint8_t wolf_flags;
	int32_t wolf_health;
	bool wolf_shake_armed, wolf_shake_active;
	float wolf_shake_time, wolf_prev_shake_time;
	float wolf_head_rotation, wolf_head_rotation_old;
	bool charged, pig_ridden;
	uint32_t pig_rider_id;
	int8_t death_time;
};

#define MAX_WATCHED 4
static struct move_mirror g_mirrors[MAX_WATCHED];
static int g_mirror_count = 0;

static struct move_mirror* mirror_for(uint32_t id) {
	for(int i = 0; i < g_mirror_count; i++)
		if(g_mirrors[i].entity_id == id)
			return &g_mirrors[i];
	assert(g_mirror_count < MAX_WATCHED);
	struct move_mirror* m = &g_mirrors[g_mirror_count++];
	memset(m, 0, sizeof(*m));
	m->entity_id = id;
	return m;
}

static void observe_move(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type != CRPC_ENTITY_MOVE)
		return;
	struct move_mirror* m = mirror_for(call->payload.entity_move.entity_id);
	m->seen = true;
	m->send_count++;
	m->pos[0] = call->payload.entity_move.pos[0];
	m->pos[1] = call->payload.entity_move.pos[1];
	m->pos[2] = call->payload.entity_move.pos[2];
	m->on_fire = call->payload.entity_move.on_fire;
	m->swelling = call->payload.entity_move.swelling;
	m->hurt = call->payload.entity_move.hurt;
	m->fuse = call->payload.entity_move.fuse;
	m->sheared = call->payload.entity_move.sheared;
	m->saddled = call->payload.entity_move.saddled;
	m->fleece_color = call->payload.entity_move.fleece_color;
	m->ghast_attacking = call->payload.entity_move.ghast_attacking;
	m->wolf_flags = call->payload.entity_move.wolf_flags;
	m->wolf_health = call->payload.entity_move.wolf_health;
	m->wolf_shake_armed = call->payload.entity_move.wolf_shake_armed;
	m->wolf_shake_active = call->payload.entity_move.wolf_shake_active;
	m->wolf_shake_time = call->payload.entity_move.wolf_shake_time;
	m->wolf_prev_shake_time = call->payload.entity_move.wolf_prev_shake_time;
	m->wolf_head_rotation = call->payload.entity_move.wolf_head_rotation;
	m->wolf_head_rotation_old = call->payload.entity_move.wolf_head_rotation_old;
	m->charged = call->payload.entity_move.charged;
	m->pig_ridden = call->payload.entity_move.pig_ridden;
	m->pig_rider_id = call->payload.entity_move.pig_rider_id;
	m->death_time = call->payload.entity_move.death_time;
}

static void assert_mirror_matches_live(const struct move_mirror* m,
									   const struct entity* e) {
	assert(m->seen && "must have been sent at least once");
	assert(m->pos[0] == e->pos[0] && m->pos[1] == e->pos[1]
		   && m->pos[2] == e->pos[2]);
	assert(m->on_fire == (e->data.monster.fire_time > 0));
	assert(m->swelling == (e->data.monster.fuse >= 0));
	assert(m->fuse == (int8_t)e->data.monster.fuse);
	assert(m->sheared == e->data.monster.sheared);
	assert(m->saddled == e->data.monster.saddled);
	assert(m->fleece_color == e->data.monster.fleece_color);
	assert(m->ghast_attacking == e->data.monster.ghast_attacking);
	assert(m->wolf_flags == e->data.monster.wolf_flags);
	assert(m->wolf_health == e->data.monster.wolf_watched_health);
	assert(m->wolf_shake_armed == e->data.monster.wolf_shake_armed);
	assert(m->wolf_shake_active == e->data.monster.wolf_shake_active);
	assert(m->wolf_shake_time == e->data.monster.wolf_shake_time);
	assert(m->wolf_prev_shake_time == e->data.monster.wolf_prev_shake_time);
	assert(m->wolf_head_rotation == e->data.monster.wolf_head_rotation);
	assert(m->wolf_head_rotation_old == e->data.monster.wolf_head_rotation_old);
	assert(m->charged == e->data.monster.charged);
	assert(m->pig_ridden == e->data.monster.pig_ridden);
	assert(m->pig_rider_id == e->data.monster.pig_rider_id);
	assert(m->death_time == (int8_t)e->data.monster.death_time);
}

static struct entity* spawn_sitting_wolf(float x, float y, float z) {
	struct entity* w = server_local_spawn_monster_at((vec3) {x, y, z},
													 MONSTER_WOLF, &s);
	assert(w);
	w->pos[0] = x; w->pos[1] = y; w->pos[2] = z;
	w->vel[0] = w->vel[1] = w->vel[2] = 0.0f;
	wolf_flag_set(&w->data.monster.wolf_flags, WOLF_FLAG_TAMED, true);
	wolf_flag_set(&w->data.monster.wolf_flags, WOLF_FLAG_SITTING, true);
	w->data.monster.wolf_target_id = UINT32_MAX;
	w->on_ground = true;
	test_mob_sync(w); /* motion units S2: the fixture pins the doubles */
	return w;
}

int main(void) {
	test_fixture_dir("test_entity_move_change_gate_equiv_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 9001,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);
	// Some generic per-tick paths touch s.player.inventory unconditionally
	// (the test_pig_saddle_env.c / test_wolf_follow_sit.c precedent) even
	// when no player is nearby -- give it a real one rather than the zeroed
	// default.
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	for(int x = 0; x < 20; x++)
		for(int z = 0; z < 6; z++) {
			set_block(x, 69, z, BLOCK_STONE);
			// A roof: Entity.isWet()'s `canBlockBeRainedOn` term (the wolf-
			// shake trigger, entity_wolf.h wolf_is_wet) is a SKY-EXPOSURE
			// predicate independent of whether it is actually raining right
			// now -- roofing the whole test area makes that term false
			// unconditionally, so the "idle" wolf below is genuinely inert
			// regardless of ambient weather state (unrelated to this lane's
			// fix; this is just removing a confound from the test).
			set_block(x, 74, z, BLOCK_STONE);
		}

	s.player.has_pos = false; // no player -> no chase/aggro noise on either wolf
	s.player.finished_loading = false;

	printf("test_entity_move_change_gate_equiv:\n");

	// --- 1+2. IDLE mob: sends only while genuinely settling, then NOTHING
	// more until the periodic fallback boundary fires exactly once. ---
	struct entity* idle = spawn_sitting_wolf(4.5f, 70.0f, 2.5f);
	clin_test_set_rpc_observer(observe_move, NULL);
	struct move_mirror* m_idle = mirror_for(idle->id);

	// Settle: the first tick always sends (fresh spawn, last_sent_move_valid
	// starts false); a few more ticks may ALSO send while Entity.moveEntity's
	// ySize-release asymptote finishes converging the resting Y -- a REAL,
	// if minuscule, physics change every tick until it does, not something
	// this lane's fix should (or does) suppress. Run until two consecutive
	// ticks both land on the SAME send_count (the transient is over) before
	// measuring the STEADY-STATE idle behavior the change-gate targets, so
	// this needs no fixed-tick-count assumption about how long that takes.
	int settle_ticks = 0;
	int prev_count;
	do {
		prev_count = m_idle->send_count;
		assert(!idle->tick_server(idle, &s));
		settle_ticks++;
		assert(settle_ticks < 50 && "wolf should reach a stable resting Y quickly");
	} while(m_idle->send_count != prev_count);
	int steady_base = m_idle->send_count;
	printf("  idle wolf: settled after %d ticks (%d send(s) during settle)\n",
		   settle_ticks, steady_base);
	assert_mirror_matches_live(m_idle, idle);

	// Steady state: run right up to (but not across) the periodic fallback
	// boundary -- derived from the LIVE counter rather than a hand-derived
	// constant, so this does not depend on exactly how many ticks settling
	// took or consumed of the fallback window.
	// The gate checks `counter >= MOB_MOVE_RESEND_INTERVAL_TICKS` BEFORE
	// incrementing on each unchanged tick, using the counter value carried
	// in from the tick before -- so starting from counter_now, the tick that
	// FIRST sees a value >= the threshold is tick number
	// (MOB_MOVE_RESEND_INTERVAL_TICKS + 1 - counter_now) counting from here.
	int counter_now = idle->data.monster.ticks_since_move_send;
	int ticks_to_fire = MOB_MOVE_RESEND_INTERVAL_TICKS + 1 - counter_now;
	assert(ticks_to_fire > 1
		   && "settling alone must not already be near the fallback boundary");
	for(int i = 0; i < ticks_to_fire - 1; i++)
		assert(!idle->tick_server(idle, &s));
	printf("  idle wolf: %d more steady ticks -> %d send(s) total (expect "
		   "%d, unchanged)\n",
		   ticks_to_fire - 1, m_idle->send_count, steady_base);
	assert(m_idle->send_count == steady_base);
	assert_mirror_matches_live(m_idle, idle);

	// Cross the boundary: exactly one more tick fires the periodic fallback,
	// even though NOTHING about the mob changed.
	assert(!idle->tick_server(idle, &s));
	printf("  idle wolf: 1 more tick -> %d send(s) total (expect %d, the "
		   "periodic fallback firing)\n",
		   m_idle->send_count, steady_base + 1);
	assert(m_idle->send_count == steady_base + 1);
	assert_mirror_matches_live(m_idle, idle);

	// --- 4. A non-position field change resends IMMEDIATELY, not at the
	// next periodic boundary. ---
	int base_after_fallback = m_idle->send_count;
	idle->data.monster.fire_time = 100; // arms on_fire=true
	assert(!idle->tick_server(idle, &s));
	printf("  idle wolf + on_fire: %d send(s) (expect %d, immediate, not "
		   "waiting for the periodic window)\n",
		   m_idle->send_count, base_after_fallback + 1);
	assert(m_idle->send_count == base_after_fallback + 1);
	assert(m_idle->on_fire == true);
	assert_mirror_matches_live(m_idle, idle);
	// Extinguish so it goes back to being genuinely idle (fire ticks down
	// naturally; force it off for a clean re-idle baseline instead of
	// waiting out the DoT).
	idle->data.monster.fire_time = 0;
	assert(!idle->tick_server(idle, &s));
	assert(m_idle->send_count == base_after_fallback + 2); // on_fire false is ALSO a real change
	assert_mirror_matches_live(m_idle, idle);

	// --- 3. ACTIVE mob: externally perturbed every tick -> a real send
	// every tick, never suppressed. ---
	struct entity* active = spawn_sitting_wolf(8.5f, 70.0f, 2.5f);
	const int ACTIVE_TICKS = 12;
	for(int i = 0; i < ACTIVE_TICKS; i++) {
		active->pos[0] += 0.1f; // a genuine per-tick position change
		test_mob_sync(active);  // motion units S2: through the fixture seam
		assert(!active->tick_server(active, &s));
	}
	struct move_mirror* m_active = mirror_for(active->id);
	printf("  active wolf: %d ticks -> %d send(s) (expect %d, every tick)\n",
		   ACTIVE_TICKS, m_active->send_count, ACTIVE_TICKS);
	assert(m_active->send_count == ACTIVE_TICKS);
	assert_mirror_matches_live(m_active, active);

	// --- 5. Final cross-check: the idle mob's total send count over its
	// WHOLE run stayed far below its tick count (the T1 saving), while the
	// active mob's matched its tick count 1:1 (real changes never
	// suppressed) -- the headline claim in one assertion.
	int idle_ticks_total = settle_ticks + (ticks_to_fire - 1) + 1 + 1 + 1;
	printf("  headline: idle mob %d sends / %d ticks vs active mob %d sends "
		   "/ %d ticks\n",
		   m_idle->send_count, idle_ticks_total, m_active->send_count,
		   ACTIVE_TICKS);
	assert(m_idle->send_count < idle_ticks_total);
	assert(m_active->send_count == ACTIVE_TICKS);

	// --- 6. STATIONARY DEATH: coordinator fix-up (2026-09-02). The
	// death-roll flop is sent from a SEPARATE, structurally-ALWAYS-
	// unconditional CRPC_ENTITY_MOVE site (entity_monster.c:~1176, inside
	// `if (e->data.monster.death_time > 0) { ...; return false; }` at the
	// very top of server_tick_monster) that the T1-gated trailing site
	// (entity_monster.c:~3477, which hardcodes `.death_time = 0` because
	// it is ONLY ever reached when death_time == 0) can never be reached
	// from -- the death_time>0 block always returns before falling
	// through that far. So a mob that dies while otherwise perfectly
	// static (position frozen -- "AUDIT #18 ITEM 3": a dying mob skips
	// ALL AI/movement) must still see death_time delivered EVERY tick of
	// the ~20-tick roll, never suppressed by the change-gate and never
	// waiting for the periodic keepalive. This proves that directly,
	// rather than by code inspection alone.
	struct entity* dying = server_local_spawn_monster_at(
		(vec3) {14.5f, 70.0f, 2.5f}, MONSTER_ZOMBIE, &s);
	assert(dying);
	dying->vel[0] = dying->vel[1] = dying->vel[2] = 0.0f;
	test_mob_sync(dying);
	struct move_mirror* m_dying = mirror_for(dying->id);

	// The tick that FIRST observes health<=0 is a separate, pre-existing,
	// documented edge (mob_die_tick's onDeath housekeeping runs, but
	// Beta's own deathTime stays 0 until the NEXT tick, so this specific
	// tick sends nothing either way) -- unrelated to the T1 gate, so it is
	// consumed here without an assertion on send_count.
	dying->health = 0;
	bool dying_reaped = dying->tick_server(dying, &s);
	assert(!dying_reaped);
	assert(dying->data.monster.death_time == 1);

	int flop_ticks = 0;
	while(!dying_reaped) {
		int before = m_dying->send_count;
		dying_reaped = dying->tick_server(dying, &s);
		if(dying_reaped)
			break; // the reap tick sends CRPC_ENTITY_DESTROY, not MOVE
		flop_ticks++;
		assert(m_dying->send_count == before + 1
			   && "the death-roll flop must send EVERY tick, never gated");
		assert(m_dying->death_time == (int8_t)dying->data.monster.death_time
			   && "the mirror's death_time must track the server exactly, "
				  "every tick of the roll");
	}
	printf("  stationary death: %d flop ticks, every one sent, mirror "
		   "death_time tracked exactly OK\n", flop_ticks);
	assert(flop_ticks > 0);

	clin_test_set_rpc_observer(NULL, NULL);
	free(idle);
	free(active);
	free(dying);
	dict_entity_clear(s.entities);

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_entity_move_change_gate_equiv\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
