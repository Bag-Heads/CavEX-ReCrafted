/*
	next-gaps-0924/G2 -- Beta 1.7.3 explosion model goldens.

	The pre-port blast (docs/deviations.md #4, parity DEBT) cast 300 random
	unit vectors attenuated by digging hardness, admitted cells by a random
	roll, and hit only the player bodies (exposure fixed at 1). Beta's
	Explosion.doExplosionA casts 1352 deterministic grid rays with intensity
	size*(0.7+0.6*rand), step 0.3, resistance (getExplosionResistance+0.3)*0.3
	per step, collects EVERY traversed cell with intensity left, and then
	damages/shoves EVERY entity in the 2*size box by an exposure-scaled
	formula (World.func_675_a block density).

	Sections (each RED on 51f6beaf by construction):
	  (a) ray set / block destruction byte-exact vs a HAND TRANSCRIPTION of
	      doExplosionA (this file, independent of source/network/explosion_
	      model.h) on a dirt+stone monolith for a fixed rand_src seed;
	  (b) a mob 6 blocks away in open air takes Beta's damage (11) and the
	      motion impulse (+0.25 along the blast direction);
	  (c) the same mob behind a stone wall: exposure 0 -> damage 1, no impulse;
	  (d) an item entity 7.5 blocks away is pushed (+0.0625) and survives
	      (damage 3 of health 5); one at 6 blocks dies (11 > 5) and its own
	      tick removes it;
	  (e) getExplosionResistance pins (the float setter chain);
	  (f) the per-blast cost record (grid rays, march steps, block lookups)
	      printed for the Wii budget question -- 1352 rays asserted.

	Hand-derived expected values (Beta, with size2 = 2*power = 8 for TNT):
	  ratio = dist/size2; v = (1 - ratio) * density;
	  damage = (int)((v^2 + v)/2 * 8 * size2 + 1)
	  dist 6, density 1 : v 0.25   -> (0.0625+0.25)/2*64+1 = 11.0  -> 11
	  dist 6, density 0 : v 0      -> 1
	  dist 7.5, density 1: v 0.0625 -> (0.00390625+0.0625)/2*64+1 = 3.125 -> 3
	[740c583 Explosion.doExplosionA, World.func_675_a, EntityItem.attackEntityFrom,
	 Block.setHardness/setResistance/getExplosionResistance]
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define MKDIR(p) _mkdir(p)
#define GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define GETPID() getpid()
#endif

#include <m-lib/m-string.h>

#include "block/blocks.h"
#include "block/piston_moving.h"
#include "network/death_drop_journal.h"
#include "block/blocks_data.h"
#include "entity/combat.h" // PLAYER_EYE_HEIGHT, MOB_KNOCKBACK_TICKS
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "platform/time.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "game/game_state.h" /* gstate: the V6 F1 producer cases */
/* RED discipline: the file also compiles on the pre-port base (51f6beaf),
 * which has neither explosion_model.h nor the cost record; sections (e)/(f)
 * are skipped there and (a)-(d) fail by construction. */
#if defined(__has_include)
#if __has_include("network/explosion_model.h")
#include "network/explosion_model.h"
#define HAVE_EXPLOSION_MODEL 1
#endif
#endif
#include "block/dispenser_logic.h" /* DISPENSER_PROJECTILE_OWNER_SENTINEL */
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "worldgen/java_random.h"
#include "platform/audio.h" /* F4-RNG: SOUND_RANDOM_EXPLODE / SOUND_RANDOM_HURT */
#include "platform/mem2.h"
#include "platform/thread.h"
#include "util.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */
#include "support/mob_motion_fixture.h"

static char g_tmp_world[64];
#define TMP_WORLD g_tmp_world

static const char* tmp_path(const char* suffix) {
	static char buf[96];
	int n = snprintf(buf, sizeof(buf), "%s%s", g_tmp_world, suffix);
	assert(n > 0 && (size_t)n < sizeof(buf));
	return buf;
}

static atomic_bool g_drain_run = true;
static unsigned g_applied_messages;
static void record_applied(const struct client_rpc* rpc, void* user) {
	(void)rpc;
	(void)user;
	g_applied_messages++;
}
static void* drain_entry(void* arg) {
	(void)arg;
	while(atomic_load(&g_drain_run)) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void output_quiesce(void) {
	/* Pending output belongs to this producer; only the discard thread may
	 * consume the inbox (including its shared packed-message replay state). */
	test_drain_quiesce();
	while(!clin_rpc_flush_pending())
		test_drain_quiesce();
	test_drain_quiesce();
}

/* ---- (a) fixture: dirt monolith with a stone plane at x == WALL_X ------- */
#define POWER 4.0F /* PRIMED_TNT_EXPLOSION_POWER */
#define CX 40
#define CZ 40
#define CY 13.5
#define WALL_X 43
#define FILL_MIN_Y 4
#define FILL_MAX_Y 30
/* Max reach: intensity <= 1.3*4 = 5.2, every step costs >= 0.225 -> <= 24
 * steps of 0.3 = 7.2 blocks. The window is +-9 so the transcription can
 * assert no ray leaves it. */
#define WIN_R 9
#define WIN_MIN_X (CX - WIN_R)
#define WIN_MAX_X (CX + WIN_R)
#define WIN_MIN_Z (CZ - WIN_R)
#define WIN_MAX_Z (CZ + WIN_R)
#define WIN_MIN_Y 3
#define WIN_MAX_Y 23
#define WIN_NX (WIN_MAX_X - WIN_MIN_X + 1)
#define WIN_NY (WIN_MAX_Y - WIN_MIN_Y + 1)
#define WIN_NZ (WIN_MAX_Z - WIN_MIN_Z + 1)

static uint8_t g_pre[WIN_NX * WIN_NY * WIN_NZ];
static uint8_t g_marked[WIN_NX * WIN_NY * WIN_NZ];

static bool in_window(int x, int y, int z) {
	return x >= WIN_MIN_X && x <= WIN_MAX_X && y >= WIN_MIN_Y && y <= WIN_MAX_Y
		&& z >= WIN_MIN_Z && z <= WIN_MAX_Z;
}
static size_t widx(int x, int y, int z) {
	return ((size_t)(x - WIN_MIN_X) * WIN_NY + (size_t)(y - WIN_MIN_Y)) * WIN_NZ
		+ (size_t)(z - WIN_MIN_Z);
}

/* Block.getExplosionResistance for the three ids the fixture uses, spelled
 * out as the Beta setter chain rather than the production table. */
static float trans_resistance(uint8_t id) {
	float blockResistance = 0.0F;
	if(id == BLOCK_DIRT) { /* setHardness(0.5F) */
		if(blockResistance < 0.5F * 5.0F)
			blockResistance = 0.5F * 5.0F;
	} else if(id == BLOCK_STONE) { /* setHardness(1.5F).setResistance(10.0F) */
		if(blockResistance < 1.5F * 5.0F)
			blockResistance = 1.5F * 5.0F;
		blockResistance = 10.0F * 3.0F;
	} else {
		assert(id == BLOCK_AIR);
	}
	return blockResistance / 5.0F;
}

/* Hand transcription of Explosion.doExplosionA:29-75 over the captured
 * pre-blast grid, drawing from a COPY of the world RNG taken right before the
 * production blast (one nextFloat per surface lattice cell, x/y/z order). */
static void transcribe_doExplosionA(struct java_random* rng) {
	memset(g_marked, 0, sizeof(g_marked));
	const double explosionX = (double)(float)(CX + 0.5F);
	const double explosionY = (double)(float)CY;
	const double explosionZ = (double)(float)(CZ + 0.5F);
	int var2 = 16;
	for(int var3 = 0; var3 < var2; ++var3)
		for(int var4 = 0; var4 < var2; ++var4)
			for(int var5 = 0; var5 < var2; ++var5) {
				if(!(var3 == 0 || var3 == var2 - 1 || var4 == 0
					 || var4 == var2 - 1 || var5 == 0 || var5 == var2 - 1))
					continue;
				double var6 = (double)((float)var3 / ((float)var2 - 1.0F) * 2.0F - 1.0F);
				double var8 = (double)((float)var4 / ((float)var2 - 1.0F) * 2.0F - 1.0F);
				double var10 = (double)((float)var5 / ((float)var2 - 1.0F) * 2.0F - 1.0F);
				double var12 = sqrt(var6 * var6 + var8 * var8 + var10 * var10);
				var6 /= var12;
				var8 /= var12;
				var10 /= var12;
				float var14 = POWER * (0.7F + jrand_next_float(rng) * 0.6F);
				double var15 = explosionX, var17 = explosionY, var19 = explosionZ;
				for(float var21 = 0.3F; var14 > 0.0F; var14 -= var21 * (12.0F / 16.0F)) {
					int var22 = (int)floor(var15);
					int var23 = (int)floor(var17);
					int var24 = (int)floor(var19);
					assert(in_window(var22, var23, var24));
					uint8_t var25 = g_pre[widx(var22, var23, var24)];
					if(var25 > 0)
						var14 -= (trans_resistance(var25) + 0.3F) * var21;
					if(var14 > 0.0F)
						g_marked[widx(var22, var23, var24)] = 1;
					var15 += var6 * (double)var21;
					var17 += var8 * (double)var21;
					var19 += var10 * (double)var21;
				}
			}
}

static void set_block(struct server_local* s, int x, int y, int z, uint8_t id) {
	assert(server_world_set_block(&s->worlds[0], x, y, z,
		(struct block_data) {.type = id, .metadata = 0, .sky_light = 0,
							 .torch_light = 0}));
}

static void free_entities(struct server_local* s) {
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s->entities);
}

static void print_cost(const char* label) {
#ifdef HAVE_EXPLOSION_MODEL
	struct server_world_explosion_cost c;
	server_world_explosion_last_cost(&c);
	printf("  cost[%s]: rays=%u march_steps=%u block_lookups=%u cells=%u "
		   "broken=%u entities=%u density_rays=%u density_cells=%u\n",
		   label, c.rays, c.ray_steps, c.block_lookups, c.cells, c.broken,
		   c.entities, c.density_rays, c.density_cells);
	fflush(stdout);
	assert(c.rays == EXPLOSION_RAY_COUNT);
#else
	printf("  cost[%s]: (no cost record on the pre-port base)\n", label);
#endif
}

static bool feq(float a, float b) { return fabsf(a - b) < 1.0e-6f; }

/* (o) neighbour-notify recorder: installed on stone for the two flaming
 * blasts; records the notified cell relative to that fixture's z origin. */
static int notes[2][4096][4];
static size_t note_n[2];
static int note_slot;
static int note_oz;
/* ---- (v) G8-BLAST-VELOCITY capture (ASTRA-RB06-RESIDUALS section 2) -----
 * Beta: Explosion.doExplosionA:100-103 calls attackEntityFrom, THEN adds the
 * impulse to motionX/Y/Z unconditionally; EntityLiving.attackEntityFrom:
 * 293-335 sets beenAttacked only on a fresh hit (null exploder included);
 * EntityTrackerEntry:125-127 sends ONE Packet28 of the CURRENT motion to the
 * tracked player itself after entity + network processing, then clears the
 * flag; Packet28EntityVelocity:19-48 clamps +-3.9 and truncates *8000;
 * NetClientHandler:165-168 setVelocity()s (REPLACES) it. Every actor-private
 * reply of the body is captured from its inventory reply hook, in order. */
#ifdef SERVER_LOCAL_HAS_ACTOR_VELOCITY_FLUSH
#define FLUSH_VEL(sp) server_local_flush_actor_velocity(sp)
#else
#define FLUSH_VEL(sp) ((void)(sp)) /* RED base: no tracker-phase flush exists */
#endif
struct vel_msg {
	uint8_t owner;
	float v[3];
	bool add;
	int route;
};
static struct vel_msg g_vel[32];
static size_t g_vel_n;
/* (y): the ORDER of every actor-private reply (type), so a case can assert
 * the velocity message is the update's last reply, after the health reply. */
static int g_types[64];
static size_t g_types_n;
static void vel_capture(const struct client_rpc* call, void* user) {
	if(g_types_n < sizeof(g_types) / sizeof(g_types[0]))
		g_types[g_types_n++] = (int)call->type;
	if(call->type != CRPC_LOCAL_PLAYER_VELOCITY)
		return;
	assert(g_vel_n < sizeof(g_vel) / sizeof(g_vel[0]));
	struct vel_msg* m = &g_vel[g_vel_n++];
	m->owner = call->payload.local_player_velocity.owner;
	memcpy(m->v, call->payload.local_player_velocity.vel, sizeof(m->v));
	m->add = call->payload.local_player_velocity.add;
	m->route = (int)(intptr_t)user;
	printf("    [vel msg %zu] route %d owner %u add %d v (%.6f, %.6f, %.6f)\n",
		   g_vel_n, m->route, (unsigned)m->owner, (int)m->add, (double)m->v[0],
		   (double)m->v[1], (double)m->v[2]);
	fflush(stdout);
}
/* Independent Packet28 round trip: clamp, (int) truncation toward zero, then
 * the existing absolute consumer's /8000 (server_remote_protocol.c form). */
static float q28(double v) {
	if(v < -3.9)
		v = -3.9;
	if(v > 3.9)
		v = 3.9;
	return (float)(int32_t)(v * 8000.0) / 8000.0f;
}
/* Open-air (density 1) doExplosionA:87-103 terms for one player eye. */
static int blast_terms(const vec3 c, double ex, double ey, double ez,
					   double size2, double imp[3]) {
	double dx = ex - (double)c[0], dy = ey - (double)c[1], dz = ez - (double)c[2];
	double dist = (double)(float)sqrt(dx * dx + dy * dy + dz * dz);
	double var41 = 1.0 - dist / size2;
	imp[0] = dx / dist * var41;
	imp[1] = dy / dist * var41;
	imp[2] = dz / dist * var41;
	return (int)((var41 * var41 + var41) / 2.0 * 8.0 * size2 + 1.0);
}
/* v3: server motion X/Z are Beta doubles (no float rounding on store). */
static double addm(double pre, double imp) { return pre + imp; }

void server_local_test_set_difficulty(int d); /* test hook (server_local.c) */
/* Server motionY (ASTRA-V-REVIEW (c)): a field of its own on the repaired
 * tree; the RED base (2e3b93aa2) kept it in the client sample vel_y. */
#ifdef SERVER_LOCAL_HAS_SERVER_MOTION_Y
#define MOTY(p) ((p).motion_y)
#define MOTY_SET(p, v) ((p).motion_y = (v))
#else
#define MOTY(p) ((double)(p).vel_y)
#define MOTY_SET(p, v) ((p).vel_y = (float)(v))
#endif
/* Independent Beta witness of one EntityLiving.moveEntityWithHeading(0,0)
 * step in open air, not on ground, no collision, no ladder/liquid
 * (EntityLiving.java:459-516): motionY = (motionY - 0.08D) * (double)0.98F;
 * motionX/Z *= (double)0.91F (v3: X/Z stored as doubles). */
static double wit_xz(double v) { return v * (double)0.91F; }
static double wit_y(double v) { return (v - 0.08) * (double)0.98F; }

/* ---- (w) real-update fixture: a fresh two-slot session ---------------- */
static struct server_local W;
static int w_hook_world = -1;
static bool w_hook_fired;
static bool w_hook_lethal;
static vec3 w_center;
static void w_hook(struct server_local* sv, struct server_world* w, int phase) {
	if(w_hook_fired || phase != 1 || w_hook_world < 0
	   || w != &sv->worlds[w_hook_world])
		return;
	w_hook_fired = true;
	server_world_explode(w, w_center, POWER);
	if(w_hook_lethal) /* the same tick: the fresh body dies -> respawn */
		(void)server_local_damage_actor(sv, &sv->player,
										4 * MAX_PLAYER_HEALTH, 0.0F, 0.0F);
}

static void w_world(struct server_local* sv, int slot, const char* tag) {
	char dir[96], reg[112];
	snprintf(dir, sizeof dir, "%s_w%s", g_tmp_world, tag);
	MKDIR(dir);
	snprintf(reg, sizeof reg, "%s/region", dir);
	MKDIR(reg);
	string_t lv;
	string_init_set_str(lv, dir);
	server_world_create(&sv->worlds[slot], lv, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(lv);
	sv->worlds[slot].session = sv;
	sv->worlds[slot].local = sv;
	sv->worlds[slot].world_index = (uint8_t)slot;
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&sv->worlds[slot], cx, cz, &sc) && sc);
		}
}

/* One fresh session; the host body is committed to `slot`; a blast fires at
 * the start of that slot's entity pass; ONE update. */
static void w_session(int slot, bool lethal) {
	struct server_local* sv = &W;
	memset(sv, 0, sizeof *sv);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(sv))
		abort();
	dict_entity_init(sv->entities);
	clin_server_publication_stop(false);
	w_world(sv, 0, lethal ? "L0" : (slot ? "B0" : "A0"));
	w_world(sv, 1, lethal ? "L1" : (slot ? "B1" : "A1"));
	sv->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;
	sv->scheduled_tick_budget = 128;
	sv->rand_src.seed = 0x9E11u;
	sv->world_time = 6000;
	server_local_skylight_recompute(sv);
	assert(inventory_create(&sv->player.inventory, &inventory_logic_player, sv,
							&sv->player, INVENTORY_SIZE, 0, 0, 0));
	sv->player.active_inventory = &sv->player.inventory;
	sv->player.inventory.reply = vel_capture;
	sv->player.inventory.reply_user = (void*)(intptr_t)0;
	sv->player.has_pos = true;
	sv->player.finished_loading = true;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.world_slot = (uint8_t)slot;
	sv->player.x = 8.5;
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	sv->player.vel_x = 0.03F;
	sv->player.vel_z = -0.01F;
	sv->render_demo_mob = -1;
	w_center[0] = 3.5F;
	w_center[1] = 40.0F;
	w_center[2] = 6.5F;
}

static void w_close(void) {
	struct server_local* sv = &W;
	server_local_test_phase_hook = NULL;
	w_hook_world = -1;
	sv->player.inventory.reply = NULL;
	inventory_destroy(&sv->player.inventory, &sv->player);
	dict_entity_clear(sv->entities);
	sv->world_slot_state[1] = SERVER_WORLD_SLOT_EMPTY;
	server_world_discard(&sv->worlds[1]);
	server_world_discard(&sv->worlds[0]);
}

/* One stationary accepted movement sample through the REAL receive path
 * (server_local_test_process_rpc -> SRPC_PLAYER_POS): Beta's one Packet10
 * per client tick; it enqueues one server-physics event. */
static void w_sample(struct server_local* sv) {
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = sv->player.x, .y = sv->player.y, .z = sv->player.z,
			.rx = sv->player.rx, .ry = sv->player.ry,
			.on_ground = false,
			.correction_seq = sv->player.correction_seq,
		},
	};
	server_local_test_process_rpc(sv, &pos);
}

/* `samples` accepted packets are queued before the blast update; they are
 * replayed AFTER the world tick, at the tracker phase (Beta's
 * world -> network -> tracker order). Expected = that many Beta air steps of
 * the post-impulse motion. */
static void w_run_n(int slot, int samples);
static void w_run(int slot) {
	w_run_n(slot, 1);
}
static void w_run_n(int slot, int samples) {
	struct server_local* sv = &W;
	w_session(slot, false);
	server_local_test_update_once(sv); /* settle: no blast yet */
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	sv->player.x = 8.5;
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	const double pre_x = sv->player.vel_x, pre_z = sv->player.vel_z;
	const double pre_y = MOTY(sv->player);
	double imp[3];
	const int dmg = blast_terms(w_center, 8.5, 43.0, 8.5, 8.0, imp);
	w_hook_world = slot;
	w_hook_fired = false;
	w_hook_lethal = false;
	/* v4 (ASTRA-V3-REVIEW P1 queue): every accepted sample's step runs AFTER
	 * the world's hits -- 8, 9 and more than 9 alike (the v3 ninth sample
	 * ran early, inside the RPC drain, and took one decay fewer).
	 * v5 (ASTRA-V4-REVIEW queue ruling): NO sample is ever refused -- 32, 33,
	 * 34, 40 and Beta's whole 101-packet budget all step, each exactly once
	 * (the drain-level deferral beyond 101 is Q1). */
	const uint32_t over0 = sv->actor_motion_overflow;
	const int steps = samples;
	assert(samples <= ACTOR_MOTION_QUEUE);
	for(int k = 0; k < samples; k++)
		w_sample(sv);
	assert(sv->player.motion_q_n == steps);
	assert(sv->actor_motion_overflow == over0);
	assert(sv->player.vel_x == pre_x && MOTY(sv->player) == pre_y
		   && sv->player.vel_z == pre_z); /* nothing stepped in the drain */
	server_local_test_phase_hook = w_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	double ex = addm(pre_x, imp[0]), ey = pre_y + imp[1], ez = addm(pre_z, imp[2]);
	for(int k = 0; k < steps; k++) {
		ex = wit_xz(ex);
		ey = wit_y(ey);
		ez = wit_xz(ez);
	}
	printf("  (w) slot %d samples %d: fired %d health %d (want %d) msgs %zu", slot, samples,
		   (int)w_hook_fired, (int)sv->player.health,
		   MAX_PLAYER_HEALTH - dmg * 8, g_vel_n);
	if(g_vel_n)
		printf(" v (%.6f, %.6f, %.6f) add %d", (double)g_vel[0].v[0],
			   (double)g_vel[0].v[1], (double)g_vel[0].v[2], (int)g_vel[0].add);
	printf("\n");
	fflush(stdout);
	assert(w_hook_fired);
	assert(sv->player.health == MAX_PLAYER_HEALTH - dmg * 8);
	assert(g_vel_n == 1); /* delivered by the END of the blast's own update */
	assert(g_vel[0].route == 0 && g_vel[0].owner == 0 && !g_vel[0].add);
	assert(g_vel[0].v[0] == q28(ex));
	assert(g_vel[0].v[1] == q28(ey));
	assert(g_vel[0].v[2] == q28(ez));
	assert(sv->player.vel_x == ex && MOTY(sv->player) == ey && sv->player.vel_z == ez);
	assert(sv->player.y == 43.0 && sv->player.vel_y == 0.0F);
	server_local_test_update_once(sv);
	assert(g_vel_n == 1); /* no repeat on the next update */
	printf("  (w) slot %d, %d sample(s) (%d stepped, none refused): blast in that slot's entity pass -> one absolute message of the post-%d-step motion in the SAME update OK\n",
		   slot, samples, steps, steps);
	w_close();
}

/* Lethal commit only (NOT a respawn/replacement -- the real host respawn and
 * guest respawn/re-admission are test_owner_swap_pose_dead_sleep_barrier
 * L1-L3): a fresh blast hit, then a lethal hit in the same
 * tick: the lethal commit clears the pending flag and events. */
static void w_run_lethal(void) {
	struct server_local* sv = &W;
	w_session(0, true);
	server_local_test_update_once(sv);
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.x = 8.5;
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	w_hook_world = 0;
	w_hook_fired = false;
	w_hook_lethal = true;
	server_local_test_phase_hook = w_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	for(int i = 0; i < 4; i++)
		server_local_test_update_once(sv);
	printf("  (w) lethal same-tick commit: fired %d health %d been_attacked %d msgs %zu\n",
		   (int)w_hook_fired, (int)sv->player.health,
		   (int)sv->player.been_attacked, g_vel_n);
	fflush(stdout);
	assert(w_hook_fired);
	assert(g_vel_n == 0 && !sv->player.been_attacked);
	puts("  (w) fresh hit then lethal hit in one tick: the lethal commit clears the flag/events, nothing is sent OK");
	w_close();
}

/* ---- (y) G8-BLAST-VELOCITY v3 evidence: production hit SOURCES inside the
 * REAL update (ASTRA-V2-REVIEW "TEST strength"), each asserting the exact
 * Packet28 XYZ, add=0, owner/route 0, the reply ORDER (the one velocity
 * message is the update's last actor-private reply, after the health reply)
 * and no repeat on the next update, in both world slots where the source
 * can live there:
 *   Y1 arrow (its own tick_server, from the entity-phase hook) then an
 *      ABSORBED blast (tile phase, same update) -> ONE combined message;
 *   Y2 a REAL fall: two accepted SRPC_PLAYER_POS samples (6-block airborne
 *      drop, then the client-landing sample) -> fall damage through
 *      server_local_player_vertical_sample, two server steps, one message;
 *   Y3 a REAL fire contact: a fire block in the body, the hazard pass of the
 *      update deals dealFireDamage(1) -> one message; the next update's
 *      contact is absorbed (no flag, no repeat);
 *   Y4 a non-null (creeper) exploder through the update: knockBack away from
 *      it plus the impulse, ONE message (no knockBack duplicate).
 * "Player exploder": Beta 1.7.3 passes a non-null exploder ONLY from
 * EntityCreeper (createExplosion(this, ...), EntityCreeper.java:101-103);
 * TNT, beds and fireballs pass null (EntityTNTPrimed:68, BlockBed:45,
 * EntityFireball:110). CaveWii's exploder argument is a struct entity and no
 * player body is one, so a player exploder is unreachable in both; Y4 is the
 * only non-null-exploder production shape. */
#define Y_ARROW 0
#define Y_CREEPER 1
#define DISPENSER_OWNER DISPENSER_PROJECTILE_OWNER_SENTINEL
static int y_slot = -1;
static int y_kind;
static struct entity* y_arrow;
static struct entity* y_creeper;
static vec3 y_center;
static bool y_fired1, y_fired2;
static void y_hook(struct server_local* sv, struct server_world* w, int phase) {
	if(y_slot < 0 || w != &sv->worlds[y_slot])
		return;
	if(phase == 1 && !y_fired1) {
		y_fired1 = true;
		if(y_kind == Y_ARROW) {
			/* EntityArrow.onUpdate port: the arrow's own tick, hit + removal */
			assert(y_arrow->tick_server(y_arrow, sv));
		} else {
			assert(server_world_try_explode_by(w, y_creeper, y_center, POWER));
		}
	} else if(phase == 2 && !y_fired2 && y_kind == Y_ARROW) {
		y_fired2 = true;
		server_world_explode(w, y_center, POWER); /* absorbed by the arrow's window */
	}
}

static void y_begin(int slot) {
	struct server_local* sv = &W;
	w_session(slot, false);
	server_local_test_update_once(sv); /* settle: no hit yet */
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	sv->player.fall_distance = 0.0F;
	sv->player.x = 8.5;
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	sv->player.vel_x = (double)0.03F;
	MOTY_SET(sv->player, -0.0784);
	sv->player.vel_z = (double)-0.01F;
	assert(!sv->player.been_attacked && sv->player.motion_q_n == 0);
	g_vel_n = 0;
	g_types_n = 0;
	y_slot = slot;
	y_fired1 = y_fired2 = false;
}

/* One accepted SRPC_PLAYER_POS at an explicit pose / client onGround. */
static void y_sample(struct server_local* sv, double y, bool on_ground) {
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = sv->player.x, .y = y, .z = sv->player.z,
			.rx = sv->player.rx, .ry = sv->player.ry,
			.on_ground = on_ground,
			.correction_seq = sv->player.correction_seq,
		},
	};
	server_local_test_process_rpc(sv, &pos);
}

/* The update produced exactly one velocity message == Q(e), owner/route 0,
 * absolute; it is the LAST actor-private reply and a health reply precedes
 * it; the next update repeats nothing. */
static void y_check(const char* tag, int slot, double ex, double ey, double ez) {
	struct server_local* sv = &W;
	printf("  (y) %s slot %d: health %d msgs %zu types [", tag, slot,
		   (int)sv->player.health, g_vel_n);
	for(size_t i = 0; i < g_types_n; i++)
		printf("%s%d", i ? "," : "", g_types[i]);
	printf("] want (%.9g, %.9g, %.9g)\n", (double)q28(ex), (double)q28(ey),
		   (double)q28(ez));
	fflush(stdout);
	assert(g_vel_n == 1);
	assert(g_vel[0].route == 0 && g_vel[0].owner == 0 && !g_vel[0].add);
	assert(g_vel[0].v[0] == q28(ex));
	assert(g_vel[0].v[1] == q28(ey));
	assert(g_vel[0].v[2] == q28(ez));
	assert(sv->player.vel_x == ex && MOTY(sv->player) == ey && sv->player.vel_z == ez);
	assert(g_types_n >= 2 && g_types[g_types_n - 1] == CRPC_LOCAL_PLAYER_VELOCITY);
	size_t health_at = g_types_n;
	for(size_t i = 0; i < g_types_n; i++) {
		if(g_types[i] == CRPC_PLAYER_SET_HEALTH && health_at == g_types_n)
			health_at = i;
		if(i + 1 < g_types_n)
			assert(g_types[i] != CRPC_LOCAL_PLAYER_VELOCITY);
	}
	assert(health_at < g_types_n - 1);
	assert(!sv->player.been_attacked);
	server_local_test_update_once(sv);
	assert(g_vel_n == 1); /* no repeat without a new fresh hit */
}

/* Entity.applyEntityCollision (S/Entity.java:630-655) transcribed: `this`
 * is the pushed neighbour, `mover` the body whose loop runs; returns the
 * mover's (+) half. Independent of entity_collision_impulse. */
static bool beta_collision(double this_x, double this_z, double mover_x,
						   double mover_z, double* hx, double* hz) {
	double var2 = mover_x - this_x;
	double var4 = mover_z - this_z;
	double a2 = var2 < 0.0 ? -var2 : var2, a4 = var4 < 0.0 ? -var4 : var4;
	double var6 = a2 > a4 ? a2 : a4; /* MathHelper.abs_max */
	if(!(var6 >= (double)0.01F))
		return false;
	var6 = (double)(float)sqrt(var6); /* MathHelper.sqrt_double -> float */
	var2 /= var6;
	var4 /= var6;
	double var8 = 1.0 / var6;
	if(var8 > 1.0)
		var8 = 1.0;
	var2 *= var8;
	var4 *= var8;
	var2 *= (double)0.05F;
	var4 *= (double)0.05F;
	*hx = var2 * (double)(1.0F - 0.0F);
	*hz = var4 * (double)(1.0F - 0.0F);
	return true;
}

/* EntityLiving.knockBack(attacker) (S/EntityLiving.java:385-405 via
 * attackEntityFrom:321-332) on server motion m, the attacker at (ax, az),
 * the victim at (vx, vz): var4 = ax - vx, var6 = az - vz (doubles). */
static void beta_knockback(double m[3], double ax, double az, double vx,
						   double vz) {
	const double dx = ax - vx, dz = az - vz;
	const float len = (float)sqrt(dx * dx + dz * dz);
	m[0] = m[0] / 2.0 - dx / (double)len * (double)0.4F;
	m[1] = fmin(m[1] / 2.0 + (double)0.4F, (double)0.4F);
	m[2] = m[2] / 2.0 - dz / (double)len * (double)0.4F;
}

/* A mob that never ticks (no AI, no shot, no own push loop): a pure
 * position the hit sources / push loops read. */
static struct entity* frozen_mob(struct server_local* sv, int slot, float x,
								 float y, float z, int id) {
	struct entity* m = server_local_spawn_monster_at_w(&sv->worlds[slot],
		(vec3) {x, y, z}, id);
	assert(m);
	m->tick_server = NULL;
	m->pos[0] = x; m->pos[1] = y; m->pos[2] = z;
	m->data.monster.motion_d.pos[0] = (double)x;
	m->data.monster.motion_d.pos[1] = (double)y;
	m->data.monster.motion_d.pos[2] = (double)z;
	server_local_refresh_entity_membership(sv, m);
	return m;
}

#define Y_OWNER_SKELETON 0  /* a mob shooter that MOVED off the flight line */
#define Y_OWNER_DISPENSER 1 /* DISPENSER_PROJECTILE_OWNER_SENTINEL: Beta null */
#define Y_OWNER_RELOADED 2  /* owner == the arrow itself: Beta null after load */
static const char* const y_owner_name[] = {"skeleton moved off-axis",
	"dispenser (null owner)", "reloaded (null owner)"};
/* v4 (ASTRA-V3-REVIEW arrow): the arrow's knockBack is measured from the
 * owner's CURRENT position (S/EntityArrow.java:147 attackEntityFrom(this.
 * owner, 4); S/EntityLiving.java:321-332), never the flight heading; a null
 * owner gets NO directional knockBack (the fresh hit still flags). The
 * arrow flies +z; the shooter is fired-from somewhere else and stands at
 * (13.25, 4.0) when the arrow lands -- so heading, fire point and current
 * position all disagree. Then an ABSORBED blast; ONE combined message. */
static void y_arrow_absorbed_blast(int slot, int owner_kind) {
	struct server_local* sv = &W;
	y_begin(slot);
	server_local_test_set_difficulty(2); /* EntityMob arrow unscaled */
	struct entity* skel = NULL;
	uint32_t owner = DISPENSER_OWNER;
	bool from_player = true;
	const double SX = 13.25, SZ = 4.0;
	if(owner_kind == Y_OWNER_SKELETON) {
		skel = frozen_mob(sv, slot, 2.5F, 41.0F, 14.5F, MONSTER_SKELETON);
		owner = skel->id;
		from_player = false;
	}
	y_arrow = calloc(1, sizeof *y_arrow);
	assert(y_arrow);
	entity_arrow(900001u, y_arrow, true, &sv->worlds[slot],
				 owner_kind == Y_OWNER_RELOADED ? 900001u : owner, ITEM_ARROW,
				 ARROW_DAMAGE, from_player);
	y_arrow->data.arrow.from_skeleton = owner_kind == Y_OWNER_SKELETON;
	y_arrow->teleport(y_arrow, (vec3) {8.5F, 43.0F - 0.85F, 8.5F - 1.5F});
	glm_vec3_copy((vec3) {0.0F, 0.0F, 2.0F}, y_arrow->vel);
	if(skel) { /* the shooter moved since the shot: its CURRENT pose counts */
		skel->pos[0] = (float)SX;
		skel->pos[2] = (float)SZ;
		skel->data.monster.motion_d.pos[0] = SX;
		skel->data.monster.motion_d.pos[2] = SZ;
		server_local_refresh_entity_membership(sv, skel);
	}
	/* Weak blast 7.63 blocks off the eye: damage <= the arrow's -> absorbed. */
	y_center[0] = 2.5F;
	y_center[1] = 39.0F;
	y_center[2] = 6.0F;
	double imp[3];
	const int bdmg = blast_terms(y_center, 8.5, 43.0, 8.5, 8.0, imp);
	assert(bdmg * 8 <= ARROW_DAMAGE && imp[0] > 0.0 && imp[1] > 0.0 && imp[2] > 0.0);
	y_kind = Y_ARROW;
	w_sample(sv);
	server_local_test_phase_hook = y_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	free(y_arrow);
	y_arrow = NULL;
	assert(y_fired1 && y_fired2);
	assert(sv->player.health == MAX_PLAYER_HEALTH - ARROW_DAMAGE); /* blast absorbed */
	double m[3] = {(double)0.03F, -0.0784, (double)-0.01F};
	if(owner_kind == Y_OWNER_SKELETON)
		beta_knockback(m, SX, SZ, 8.5, 8.5); /* NOT the +z heading */
	m[0] = addm(m[0], imp[0]);
	m[1] += imp[1];
	m[2] = addm(m[2], imp[2]);
	char tag[96];
	snprintf(tag, sizeof tag, "Y1 arrow (%s) + absorbed blast",
			 y_owner_name[owner_kind]);
	y_check(tag, slot, wit_xz(m[0]), wit_y(m[1]), wit_xz(m[2]));
	if(skel) {
		dict_entity_erase(sv->entities, skel->id);
		free(skel);
	}
	w_close();
}

static void y_real_fall(int slot) {
	struct server_local* sv = &W;
	y_begin(slot);
	y_sample(sv, 43.0 - 6.0, false); /* airborne drop: fallDistance 6 */
	assert(sv->player.fall_distance == 6.0F && sv->player.health == MAX_PLAYER_HEALTH);
	y_sample(sv, 43.0 - 6.0, true);  /* landing: ceil(6 - 3) = 3 -> 24 units */
	/* v4 (ASTRA-V3-REVIEW P1 damage ordering): the landing is accounted at
	 * receive (fallDistance cleared) but EntityLiving.fall's hit is the
	 * packet's handleFalling (S/NetServerHandler.java:211-213), replayed
	 * after its physics and after the world's hits -- nothing at receive. */
	assert(sv->player.fall_distance == 0.0F);
	assert(sv->player.health == MAX_PLAYER_HEALTH);
	assert(!sv->player.been_attacked && g_vel_n == 0 && g_types_n == 0);
	assert(sv->player.motion_q_n == 2 && sv->player.motion_q[0].fall_damage == 0
		   && sv->player.motion_q[1].fall_damage == 3 * 8);
	server_local_test_update_once(sv);
	assert(sv->player.health == MAX_PLAYER_HEALTH - 3 * 8);
	/* No knockBack (null attacker); two accepted samples = two air steps
	 * (each event's lastPos is airborne, onGround = the previous packet's
	 * false); the packet moves (0,-6,0) and (0,0,0) clip nothing. */
	const double ex = wit_xz(wit_xz((double)0.03F));
	const double ey = wit_y(wit_y(-0.0784));
	const double ez = wit_xz(wit_xz((double)-0.01F));
	y_check("Y2 real fall", slot, ex, ey, ez);
	w_close();
}

static void y_real_fire(int slot) {
	struct server_local* sv = &W;
	y_begin(slot);
	struct block_data fire = {.type = BLOCK_FIRE, .metadata = 0};
	assert(server_world_set_block(&sv->worlds[slot], 8, 41, 8, fire)); /* feet cell */
	w_sample(sv);
	server_local_test_update_once(sv);
	assert(sv->player.health == MAX_PLAYER_HEALTH - 8); /* dealFireDamage(1) */
	y_check("Y3 real fire contact", slot, wit_xz((double)0.03F), wit_y(-0.0784),
			wit_xz((double)-0.01F));
	/* y_check ran a second update inside the fire: absorbed, no flag. */
	assert(sv->player.health == MAX_PLAYER_HEALTH - 8);
	assert(server_world_set_block(&sv->worlds[slot], 8, 41, 8,
		(struct block_data) {.type = BLOCK_AIR}));
	w_close();
}

static void y_creeper_exploder(void) {
	struct server_local* sv = &W;
	y_begin(0);
	server_local_test_set_difficulty(2); /* Normal: EntityMob damage unscaled */
	y_creeper = server_local_spawn_monster_at_w(&sv->worlds[0],
		(vec3) {4.5F, 41.0F, 6.5F}, MONSTER_CREEPER);
	assert(y_creeper);
	glm_vec3_copy(y_creeper->pos, y_center); /* Beta: the blast at posX/Y/Z */
	double imp[3];
	const int dmg = blast_terms(y_center, 8.5, 43.0, 8.5, 8.0, imp);
	assert(dmg * 8 < MAX_PLAYER_HEALTH);
	y_kind = Y_CREEPER;
	w_sample(sv);
	server_local_test_phase_hook = y_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	assert(y_fired1);
	assert(sv->player.health == MAX_PLAYER_HEALTH - dmg * 8);
	/* attackEntityFrom(creeper) knockBack: dx = creeper - victim (doubles of
	 * the float delta the entry passes), float length, 0.4F. */
	const double dx = -(double)(float)(8.5 - (double)y_creeper->pos[0]);
	const double dz = -(double)(float)(8.5 - (double)y_creeper->pos[2]);
	const float len = (float)sqrt(dx * dx + dz * dz);
	double mx = (double)0.03F / 2.0 - dx / (double)len * (double)0.4F;
	double my = fmin(-0.0784 / 2.0 + (double)0.4F, (double)0.4F);
	double mz = (double)-0.01F / 2.0 - dz / (double)len * (double)0.4F;
	mx = addm(mx, imp[0]);
	my += imp[1];
	mz = addm(mz, imp[2]);
	y_check("Y4 creeper exploder", 0, wit_xz(mx), wit_y(my), wit_xz(mz));
	w_close();
}

/* ---- (z) G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW) through the REAL update -- */

/* Z3 mixed fall + hit (P1 damage ordering): an airborne sample and a
 * landing sample are accepted (fall `drop` blocks), then the update's
 * entity phase lands a skeleton arrow (directional, from its current pose).
 * Beta: the world hit FIRST (fresh: window + knockBack), then each packet's
 * physics, then the landing's handleFalling -- inside the arrow's window:
 * fall 24 <= 32 is absorbed whole; fall 40 > 32 bills only the excess 8 and
 * is not fresh. v3 billed the landing in the drain, so it was the FRESH hit
 * and the arrow lost its knockBack (and, for the 40 case, bounced). */
static void y_fall_plus_arrow(int slot, double drop) {
	struct server_local* sv = &W;
	y_begin(slot);
	server_local_test_set_difficulty(2);
	const double SX = 4.75, SZ = 12.0;
	struct entity* skel = frozen_mob(sv, slot, (float)SX, 35.0F, (float)SZ,
									 MONSTER_SKELETON);
	const double land_y = 43.0 - drop;
	y_sample(sv, land_y, false);
	y_sample(sv, land_y, true);
	const int fall = (int)ceil(drop - 3.0) * 8;
	assert(sv->player.health == MAX_PLAYER_HEALTH && !sv->player.been_attacked
		   && sv->player.motion_q_n == 2
		   && sv->player.motion_q[1].fall_damage == fall);
	y_arrow = calloc(1, sizeof *y_arrow);
	assert(y_arrow);
	entity_arrow(900002u, y_arrow, true, &sv->worlds[slot], skel->id,
				 ITEM_ARROW, ARROW_DAMAGE, false);
	y_arrow->data.arrow.from_skeleton = true;
	y_arrow->teleport(y_arrow, (vec3) {8.5F, (float)land_y - 0.85F, 8.5F - 1.5F});
	glm_vec3_copy((vec3) {0.0F, 0.0F, 2.0F}, y_arrow->vel);
	y_kind = Y_ARROW;
	y_fired2 = true; /* no blast in this case */
	server_local_test_phase_hook = y_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	free(y_arrow);
	y_arrow = NULL;
	assert(y_fired1);
	const int want_health = MAX_PLAYER_HEALTH - (fall > ARROW_DAMAGE ? fall : ARROW_DAMAGE);
	printf("  (z) Z3 fall %d + arrow %d slot %d: health %d (want %d) last_damage %d\n",
		   fall, ARROW_DAMAGE, slot, (int)sv->player.health, want_health,
		   (int)sv->player.last_damage);
	assert(sv->player.health == want_health);
	assert(sv->player.last_damage == (fall > ARROW_DAMAGE ? fall : ARROW_DAMAGE));
	/* The arrow was the fresh hit: knockBack from the skeleton's pose (the
	 * victim at its CURRENT pose, the landing sample), then two packets'
	 * air steps (lastPos 43 then land_y, onGround false then false), the
	 * packet moves (0,-drop,0) and (0,0,0) clip nothing; the landing adds
	 * no motion. */
	double m[3] = {(double)0.03F, -0.0784, (double)-0.01F};
	beta_knockback(m, SX, SZ, 8.5, 8.5);
	for(int k = 0; k < 2; k++) {
		m[0] = wit_xz(m[0]);
		m[1] = wit_y(m[1]);
		m[2] = wit_xz(m[2]);
	}
	char tag[64];
	snprintf(tag, sizeof tag, "Z3 fall %d + arrow", fall);
	y_check(tag, slot, m[0], m[1], m[2]);
	dict_entity_erase(sv->entities, skel->id);
	free(skel);
	w_close();
}

/* Z2 the player's own push loop (P1 push schedule; S/EntityLiving.java:
 * 623-629): ONCE PER ACCEPTED PACKET, after that packet's
 * moveEntityWithHeading, at the box it left -- so its +half is added AFTER
 * that step's friction. A never-ticking zombie stands just +x of the body;
 * a fresh blast in the entity phase raises the flag. 0 samples: no own
 * push at all (v3 pushed once per update at the current pose, before the
 * replay, and then friction-decayed it). */
static void w_push_run(int slot, int samples) {
	struct server_local* sv = &W;
	w_session(slot, false);
	server_local_test_update_once(sv); /* settle */
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	sv->player.x = 8.5;
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	const double MOB_X = 9.05, MOB_Z = 8.5;
	const double feet = 43.0 - (double)PLAYER_EYE_HEIGHT;
	struct entity* zom = frozen_mob(sv, slot, (float)MOB_X, (float)feet,
									(float)MOB_Z, MONSTER_ZOMBIE);
	const double mob_x = (double)zom->pos[0], mob_z = (double)zom->pos[2];
	const double pre_x = sv->player.vel_x, pre_z = sv->player.vel_z;
	const double pre_y = MOTY(sv->player);
	double imp[3];
	(void)blast_terms(w_center, 8.5, 43.0, 8.5, 8.0, imp);
	w_hook_world = slot;
	w_hook_fired = false;
	w_hook_lethal = false;
	for(int k = 0; k < samples; k++)
		w_sample(sv);
	server_local_test_phase_hook = w_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	double m[3] = {addm(pre_x, imp[0]), pre_y + imp[1], addm(pre_z, imp[2])};
	int pushes = 0;
	for(int k = 0; k < samples; k++) {
		/* moveEntityWithHeading: the box (lastPos 8.5) moves by m, open air */
		const double cx = 8.5 + m[0], cz = 8.5 + m[2];
		const double min_y = feet + m[1], max_y = min_y + (double)1.8F;
		m[1] = wit_y(m[1]);
		m[0] = wit_xz(m[0]);
		m[2] = wit_xz(m[2]);
		/* boundingBox.expand(0.2F, 0, 0.2F) strict-intersects the zombie */
		const double zh = (double)(zom->width / 2.0F);
		const bool hit = cx + 0.3 + (double)0.2F > mob_x - zh
			&& cx - 0.3 - (double)0.2F < mob_x + zh
			&& max_y > (double)zom->pos[1]
			&& min_y < (double)zom->pos[1] + (double)zom->height
			&& cz + 0.3 + (double)0.2F > mob_z - zh
			&& cz - 0.3 - (double)0.2F < mob_z + zh;
		double hx, hz;
		if(hit && beta_collision(mob_x, mob_z, cx, cz, &hx, &hz)) {
			m[0] += hx; /* AFTER this packet's friction */
			m[2] += hz;
			pushes++;
		}
	}
	printf("  (z) Z2 push slot %d samples %d: pushes %d msgs %zu", slot, samples,
		   pushes, g_vel_n);
	if(g_vel_n)
		printf(" v (%.6f, %.6f, %.6f) want (%.6f, %.6f, %.6f)",
			   (double)g_vel[0].v[0], (double)g_vel[0].v[1], (double)g_vel[0].v[2],
			   (double)q28(m[0]), (double)q28(m[1]), (double)q28(m[2]));
	printf("\n");
	fflush(stdout);
	assert(w_hook_fired);
	assert(pushes == samples); /* the witness geometry pushes on every packet */
	assert(g_vel_n == 1 && g_vel[0].route == 0 && g_vel[0].owner == 0
		   && !g_vel[0].add);
	assert(g_vel[0].v[0] == q28(m[0]) && g_vel[0].v[1] == q28(m[1])
		   && g_vel[0].v[2] == q28(m[2]));
	assert(sv->player.vel_x == m[0] && MOTY(sv->player) == m[1]
		   && sv->player.vel_z == m[2]);
	server_local_test_update_once(sv);
	assert(g_vel_n == 1);
	dict_entity_erase(sv->entities, zom->id);
	free(zom);
	w_close();
}

/* Z4 riding replay (P2; S/NetServerHandler.java:71-95): updateRiderPosition
 * (the seat), the INCOMING onGround (:90), onUpdateEntity at the seat, then
 * moveEntity(input, 0, input) FROM THE BOX onUpdateEntity LEFT (:92), the
 * seat restored, motionX/Z = input (:94-95). The observables: motionY (the
 * physics), isInWeb (the final box's block sweep, consumed by the next
 * packet's first move -- motion zeroed) and X/Z = input. Each case is two
 * updates with one riding sample each; a fresh null hit (no knockBack) in
 * the entity phase raises the flag, so each update sends the exact motion.
 *   ZM move-from-moved-box: pig mount, motion 0.3 then input 0.25 -> the
 *      box travels 0.55 and enters the cobweb cell; v3 reset the box to
 *      lastPos and moved only 0.25 (no web).
 *   ZG incoming onGround: pig mount, sneaking, previous onGround false,
 *      incoming true, nothing below -> the sneak edge backs the 0.3
 *      physics move off to 0 -> 0.25 total, no web; with the previous
 *      (false) onGround the box would travel 0.55 into the web.
 *   ZB own boat in the collision list (ASTRA-V3-REVIEW boat; S/World.java:
 *      808-821 excludes only the queried player): boat mount, sneaking,
 *      incoming onGround true: the boat below makes the sneak-edge probe
 *      non-empty -> no back-off -> 0.55 -> web; excluding the boat would
 *      back off (no web).
 *   ZC mounted floor collision: pig mount over a stone floor 0.05 below
 *      the seat's feet, motionY -0.2 -> clipped -> 0 then gravity. */
#define ZK_M 0
#define ZK_G 1
#define ZK_B 2
#define ZK_C 3
static const char* const zk_name[] = {"ZM move from the moved box",
	"ZG incoming onGround", "ZB own boat collides", "ZC mounted floor"};
static bool z_hit_fired;
static void z_hit_hook(struct server_local* sv, struct server_world* w,
					   int phase) {
	if(z_hit_fired || phase != 1 || w != &sv->worlds[0])
		return;
	z_hit_fired = true;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	(void)server_local_damage_actor(sv, &sv->player, 8, 0.0F, 0.0F);
}
/* Beta's seat (Entity/EntityBoat.updateRiderPosition) transcribed. */
static void z_seat(const struct entity* v, bool boat, double* x, double* y,
				   double* z) {
	const double mounted = boat ? 0.0 * 0.0 - (double)0.3F
								: (double)0.9F * 0.75;
	double vx = boat ? (double)v->pos[0] : v->data.monster.motion_d.pos[0];
	double vy = boat ? (double)v->pos[1] : v->data.monster.motion_d.pos[1];
	double vz = boat ? (double)v->pos[2] : v->data.monster.motion_d.pos[2];
	if(boat) {
		vx += cos((double)v->orient[0]) * 0.4;
		vz += sin((double)v->orient[0]) * 0.4;
	}
	*x = vx;
	*y = vy + mounted + (double)(1.62F - 0.5F);
	*z = vz;
}
static void z_ride_update(struct server_local* sv, double x, double y,
						  double z, bool on_ground, bool sneaking) {
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = x, .y = y, .z = z, .rx = 0.0F, .ry = 0.0F,
			.vel_x = 0.25F, .vel_y = 0.0F, .vel_z = 0.0F,
			.on_ground = on_ground, .sneaking = sneaking,
			.correction_seq = sv->player.correction_seq,
		},
	};
	const uint8_t q0 = sv->player.motion_q_n;
	server_local_test_process_rpc(sv, &pos);
	assert(sv->player.motion_q_n == q0 + 1
		   && sv->player.motion_q[q0].riding); /* riding at receive */
	z_hit_fired = false;
	server_local_test_phase_hook = z_hit_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	assert(z_hit_fired);
}
static void z_ride(int kind) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle */
	const bool boat = kind == ZK_B;
	struct entity* v;
	if(boat) {
		v = server_local_spawn_boat((vec3) {19.9F, 50.0F, 20.5F}, sv);
		assert(v);
		v->tick_server = NULL; /* frozen: the seat is a pure pose */
		v->orient[0] = 0.0F;
		v->data.boat.occupied = true;
		v->data.boat.occupant_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		v->data.boat.rider_actor = ACTOR_IDENT_HOST;
		sv->player.riding_boat = true;
	} else {
		const float py = kind == ZK_C ? 0.0F : 50.0F;
		v = frozen_mob(sv, 0, 20.3F, py, 20.5F, MONSTER_PIG);
		if(kind == ZK_C) { /* seat feet 0.05 above a stone top at y 50 */
			const double k = (double)0.9F * 0.75 + (double)(1.62F - 0.5F)
				- (double)PLAYER_EYE_HEIGHT;
			v->data.monster.motion_d.pos[1] = 51.05 - k;
			v->pos[1] = (float)v->data.monster.motion_d.pos[1];
			for(int x = 19; x <= 21; x++)
				for(int zz = 19; zz <= 21; zz++)
					assert(server_world_set_block(&sv->worlds[0], x, 50, zz,
						(struct block_data) {.type = BLOCK_STONE}));
		}
		v->data.monster.pig_ridden = true;
		v->data.monster.pig_rider_actor = ACTOR_IDENT_HOST;
		v->data.monster.pig_rider_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		server_local_refresh_entity_membership(sv, v);
	}
	assert(server_local_actor_vehicle(sv, &sv->player) == v);
	double sx, sy, sz;
	z_seat(v, boat, &sx, &sy, &sz);
	const double feet = sy - (double)PLAYER_EYE_HEIGHT;
	/* cobwebs in the +x cell the 0.55 travel reaches and 0.25 does not */
	if(kind != ZK_C)
		for(int yy = (int)floor(feet); yy <= (int)floor(feet + 1.8 - 0.001); yy++)
			assert(server_world_set_block(&sv->worlds[0], 21, yy, 20,
				(struct block_data) {.type = BLOCK_COBWEB}));
	assert(floor(sx + 0.3 + 0.25 - 0.001) == 20.0
		   && floor(sx + 0.3 + 0.55 - 0.001) == 21.0
		   && floor(sx + 0.3 + 0.3 - 0.001) == 20.0);
	if(boat) { /* the precondition ZB rests on: the sneak probe box (seat
				* box offset (0.3, -1, 0)) strict-intersects the boat box */
		struct AABB bb;
		assert(v->getBoundingBox(v, &bb));
		assert(sx + 0.3 + 0.3 > (double)bb.x1 && sx - 0.3 + 0.3 < (double)bb.x2
			   && feet - 1.0 + 1.8 > (double)bb.y1 && feet - 1.0 < (double)bb.y2);
	}
	sv->player.x = sx;
	sv->player.y = sy;
	sv->player.z = sz;
	sv->player.on_ground = false; /* the PREVIOUS packet's onGround */
	sv->player.sneaking = kind == ZK_G || kind == ZK_B;
	sv->player.in_web = false;
	sv->player.vel_x = 0.3;
	sv->player.vel_z = 0.0;
	MOTY_SET(sv->player, kind == ZK_C ? -0.2 : 0.0);
	sv->player.health = MAX_PLAYER_HEALTH;
	g_vel_n = 0;
	const bool incoming = kind == ZK_G || kind == ZK_B;
	z_ride_update(sv, sx, sy, sz, incoming, sv->player.sneaking);
	const bool web = kind == ZK_M || kind == ZK_B;
	const double y1 = kind == ZK_C ? wit_y(0.0) /* clipped by the floor */
								   : wit_y(0.0);
	printf("  (z) %s update 1: in_web %d msgs %zu v (%.6f, %.6f, %.6f)\n",
		   zk_name[kind], (int)sv->player.in_web, g_vel_n,
		   g_vel_n ? (double)g_vel[0].v[0] : 0.0,
		   g_vel_n ? (double)g_vel[0].v[1] : 0.0,
		   g_vel_n ? (double)g_vel[0].v[2] : 0.0);
	fflush(stdout);
	assert(sv->player.in_web == web);
	/* v5 (ASTRA-V4-REVIEW riding): the riding BRANCH physics keeps every v4
	 * witness (moved-box move, incoming onGround, own boat, floor clip):
	 * motionX = the input, motionY = the step; then S/NetServerHandler.
	 * java:97/108 -> Entity.updateRidden:819-822 zero the rider's motion on
	 * a live vehicle, so the tracker sends zero. */
	assert(server_local_test_riding_branch_motion[0] == (double)0.25F
		   && server_local_test_riding_branch_motion[1] == y1
		   && server_local_test_riding_branch_motion[2] == 0.0);
	assert(g_vel_n == 1 && !g_vel[0].add && g_vel[0].owner == 0);
	assert(g_vel[0].v[0] == 0.0F && g_vel[0].v[1] == 0.0F
		   && g_vel[0].v[2] == 0.0F);
	assert(sv->player.vel_x == 0.0 && MOTY(sv->player) == 0.0
		   && sv->player.vel_z == 0.0);
	if(kind == ZK_C) {
		/* the unclipped motion would have been wit_y(-0.2) */
		assert(y1 != wit_y(-0.2));
	} else {
		/* update 2: v5 -- the previous packet's updateRidden left the motion
		 * at zero, so the branch restarts from 0 whether or not the web was
		 * entered, and the box (0 from onUpdateEntity + the 0.25 input) no
		 * longer reaches the web cell: inWeb is consumed and stays clear. */
		z_ride_update(sv, sx, sy, sz, incoming, sv->player.sneaking);
		const double y2 = wit_y(0.0);
		printf("  (z) %s update 2: in_web %d branch (%.6f, %.6f, %.6f) want Y %.6f, sent (%.6f, %.6f, %.6f)\n",
			   zk_name[kind], (int)sv->player.in_web,
			   server_local_test_riding_branch_motion[0],
			   server_local_test_riding_branch_motion[1],
			   server_local_test_riding_branch_motion[2], y2,
			   (double)g_vel[1].v[0], (double)g_vel[1].v[1],
			   (double)g_vel[1].v[2]);
		fflush(stdout);
		assert(!sv->player.in_web);
		assert(server_local_test_riding_branch_motion[0] == (double)0.25F
			   && server_local_test_riding_branch_motion[1] == y2
			   && server_local_test_riding_branch_motion[2] == 0.0);
		assert(g_vel_n == 2 && g_vel[1].v[0] == 0.0F && g_vel[1].v[1] == 0.0F
			   && g_vel[1].v[2] == 0.0F);
		assert(MOTY(sv->player) == 0.0);
	}
	printf("  (z) %s OK\n", zk_name[kind]);
	sv->player.riding_boat = false;
	sv->player.sneaking = false;
	dict_entity_erase(sv->entities, v->id);
	free(v);
	w_close();
}

/* Z5 paused replay/flush (ASTRA-V3-REVIEW paused): a sample accepted before
 * SRPC_TOGGLE_PAUSE (airborne, physics event) and a landing sample accepted
 * while paused (no physics, a deferred 24-unit landing) are held through
 * paused updates -- no motion evolution, no damage, no velocity send, even
 * with a pending flag -- and a paused stationary sample adds no event. The
 * update that unpauses runs the world (a fresh blast), then the held events
 * exactly once in order (one air step, then the landing -- absorbed by the
 * blast's window), then ONE flush. */
static void w_pause_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle */
	g_vel_n = 0;
	g_types_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	sv->player.fall_distance = 0.0F;
	sv->player.x = 8.5;
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	sv->player.vel_x = (double)0.03F;
	MOTY_SET(sv->player, -0.0784);
	sv->player.vel_z = (double)-0.01F;
	const double pre[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	struct server_rpc pause = {.type = SRPC_TOGGLE_PAUSE};
	y_sample(sv, 43.0 - 6.0, false);         /* physics event, fall 6 */
	server_local_test_process_rpc(sv, &pause); /* paused from here */
	assert(sv->paused);
	y_sample(sv, 43.0 - 6.0, true);          /* paused landing: 24 deferred */
	assert(sv->player.motion_q_n == 2 && sv->player.motion_q[0].physics
		   && !sv->player.motion_q[1].physics
		   && sv->player.motion_q[1].fall_damage == 24);
	/* a pending flag across the pause (hand-raised: no production producer
	 * runs in a held update) must neither be sent nor clear */
	sv->player.been_attacked = true;
	sv->player.been_attacked_life
		= server_local_actor_ident(sv, &sv->player).generation;
	for(int k = 0; k < 3; k++) {
		y_sample(sv, 43.0 - 6.0, true); /* paused, stationary: no event */
		server_local_test_update_once(sv);
		assert(sv->paused && sv->player.motion_q_n == 2);
		assert(sv->player.vel_x == pre[0] && MOTY(sv->player) == pre[1]
			   && sv->player.vel_z == pre[2]);
		assert(sv->player.health == MAX_PLAYER_HEALTH && g_vel_n == 0
			   && sv->player.been_attacked);
	}
	sv->player.been_attacked = false; /* the blast below raises it for real */
	double imp[3];
	const int dmg = blast_terms(w_center, 8.5, 43.0 - 6.0, 8.5, 8.0, imp);
	assert(dmg * 8 >= 24); /* the landing falls inside the blast's window */
	server_local_test_process_rpc(sv, &pause); /* resume: queued in-order */
	assert(!sv->paused);
	w_hook_world = 0;
	w_hook_fired = false;
	w_hook_lethal = false;
	server_local_test_phase_hook = w_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	assert(w_hook_fired && sv->player.motion_q_n == 0);
	assert(sv->player.health == MAX_PLAYER_HEALTH - dmg * 8); /* landing absorbed */
	/* ONE air step: the pre-pause packet (lastPos 43 -> 37, open air); the
	 * paused landing carries no physics. */
	const double ex = wit_xz(addm(pre[0], imp[0]));
	const double ey = wit_y(pre[1] + imp[1]);
	const double ez = wit_xz(addm(pre[2], imp[2]));
	y_check("Z5 pause/resume", 0, ex, ey, ez);
	puts("  (z) Z5 paused: held events/flag survive, no motion/damage/send while paused; resume replays once in phase, one message OK");
	w_close();
}

/* ======================================================================
 * G8-BLAST-VELOCITY v5 (ASTRA-V4-REVIEW finite checklist items 1-3)
 * ====================================================================== */

/* ---- Q1: lossless admission through the REAL drain (svin) -------------
 * S/NetworkManager.java:191-209: processReadPackets handles at most 101
 * packets of a connection per tick (`var1 = 100; while(!empty && var1-- >=
 * 0)`) and leaves the rest queued for the next tick. The client refills the
 * inbox WHILE the server drains (one new sample per processed sample, 250
 * in all): update 1 takes exactly 101 (the blast's update: ONE message of
 * the post-101-step motion), update 2 the next 101, update 3 the last 48,
 * update 4 none -- every sample stepped exactly once, in order, in its own
 * update's network phase; nothing refused (overflow 0), nothing dropped by
 * the transport (svin dropped count unchanged). */
static int q_left;
static int q_done;
static bool q_send_pos(struct server_local* sv, double y, bool on_ground) {
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = sv->player.x, .y = y, .z = sv->player.z,
			.rx = sv->player.rx, .ry = sv->player.ry,
			.on_ground = on_ground,
			.correction_seq = sv->player.correction_seq,
		},
	};
	return svin_rpc_send(&pos);
}
static void q_refill_hook(struct server_local* sv, const struct server_rpc* call) {
	if(call->type != SRPC_PLAYER_POS)
		return;
	q_done++;
	if(q_left > 0) {
		assert(q_send_pos(sv, sv->player.y, false));
		q_left--;
	}
}
static void q_drain_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle: no blast yet */
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	const double pre_x = sv->player.vel_x, pre_z = sv->player.vel_z;
	const double pre_y = MOTY(sv->player);
	double imp[3];
	const int dmg = blast_terms(w_center, 8.5, 43.0, 8.5, 8.0, imp);
	const uint32_t drop0 = svin_dropped_count();
	assert(svin_held_count() == 0 && sv->actor_motion_overflow == 0);
	q_left = 250;
	for(int k = 0; k < 8; k++) {
		assert(q_send_pos(sv, 43.0, false));
		q_left--;
	}
	server_local_test_rpc_done_hook = q_refill_hook;
	double ex = addm(pre_x, imp[0]), ey = pre_y + imp[1], ez = addm(pre_z, imp[2]);
	static const int want[4] = {101, 101, 48, 0};
	for(int u = 0; u < 4; u++) {
		q_done = 0;
		if(u == 0) {
			w_hook_world = 0;
			w_hook_fired = false;
			w_hook_lethal = false;
			server_local_test_phase_hook = w_hook;
		}
		server_local_test_update_once(sv);
		server_local_test_phase_hook = NULL;
		for(int k = 0; k < want[u]; k++) {
			ex = wit_xz(ex);
			ey = wit_y(ey);
			ez = wit_xz(ez);
		}
		printf("  (q) Q1 update %d: processed %d (want %d) held %zu motion (%.9g, %.9g, %.9g) msgs %zu\n",
			   u + 1, q_done, want[u], svin_held_count(), sv->player.vel_x,
			   MOTY(sv->player), sv->player.vel_z, g_vel_n);
		fflush(stdout);
		assert(q_done == want[u]);
		assert(sv->player.motion_q_n == 0); /* all replayed in their update */
		assert(sv->player.vel_x == ex && MOTY(sv->player) == ey
			   && sv->player.vel_z == ez);
		if(u == 0) {
			assert(w_hook_fired && sv->player.health == MAX_PLAYER_HEALTH - dmg * 8);
			assert(g_vel_n == 1 && !g_vel[0].add && g_vel[0].owner == 0
				   && g_vel[0].v[0] == q28(ex) && g_vel[0].v[1] == q28(ey)
				   && g_vel[0].v[2] == q28(ez));
			assert(svin_held_count() == 8); /* deferred, token kept, in order */
		}
	}
	server_local_test_rpc_done_hook = NULL;
	assert(g_vel_n == 1 && q_left == 0 && svin_held_count() == 0);
	assert(svin_dropped_count() == drop0 && sv->actor_motion_overflow == 0);
	puts("  (q) Q1 sustained refill: 101/101/48/0 per update (Beta's 101-packet budget), excess HELD not dropped, every sample stepped once in order, one exact message OK");
	w_close();
}

/* ---- Q2: a pause-held backlog through the REAL drain ------------------
 * 40 damaging landings (each a 6-block drop: 24 units) arrive while the
 * local session is paused, over several held updates: each is kept (no
 * physics, landing deferred), nothing evolves, nothing is sent. The resume
 * arrives in FIFO order behind them; its update replays all 40 in order --
 * the first fresh (24), the other 39 absorbed by its window -- and sends
 * ONE message of the untouched motion; nothing repeats afterwards. */
static void q_pause_backlog_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle */
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	sv->player.fall_distance = 0.0F;
	const double pre[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	const uint32_t drop0 = svin_dropped_count();
	assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	server_local_test_update_once(sv);
	assert(sv->paused);
	int landings = 0;
	while(landings < 40) {
		for(int k = 0; k < 4; k++) { /* 12 messages: never a full inbox */
			assert(q_send_pos(sv, 43.0 - 6.0, false)); /* the drop: fall 6 */
			assert(q_send_pos(sv, 43.0 - 6.0, true));  /* landing: 24 */
			assert(q_send_pos(sv, 43.0, false));       /* back up: no event */
			landings++;
		}
		server_local_test_update_once(sv);
		printf("  (q) Q2 paused update: landings %d queue %d paused %d y %.3f fall %.3f\n",
			   landings, (int)sv->player.motion_q_n, (int)sv->paused,
			   sv->player.y, (double)sv->player.fall_distance);
		fflush(stdout);
		assert(sv->paused && sv->player.motion_q_n == landings);
		assert(sv->player.health == MAX_PLAYER_HEALTH && g_vel_n == 0);
		assert(sv->player.vel_x == pre[0] && MOTY(sv->player) == pre[1]
			   && sv->player.vel_z == pre[2]);
	}
	for(int k = 0; k < 40; k++)
		assert(!sv->player.motion_q[k].physics
			   && sv->player.motion_q[k].fall_damage == 24);
	assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	server_local_test_update_once(sv);
	printf("  (q) Q2 resume: paused %d queue %d health %d msgs %zu\n",
		   (int)sv->paused, (int)sv->player.motion_q_n, (int)sv->player.health,
		   g_vel_n);
	fflush(stdout);
	assert(!sv->paused && sv->player.motion_q_n == 0);
	assert(sv->player.health == MAX_PLAYER_HEALTH - 24);
	assert(g_vel_n == 1 && g_vel[0].v[0] == q28(pre[0])
		   && g_vel[0].v[1] == q28(pre[1]) && g_vel[0].v[2] == q28(pre[2]));
	server_local_test_update_once(sv);
	assert(g_vel_n == 1 && sv->player.health == MAX_PLAYER_HEALTH - 24);
	assert(svin_dropped_count() == drop0 && sv->actor_motion_overflow == 0);
	puts("  (q) Q2 pause-held backlog: 40 landings kept across held updates, replayed once in order on resume (1 fresh, 39 absorbed), one message OK");
	w_close();
}

/* ---- R1: riding per packet around mount changes ------------------------
 * S/NetServerHandler.java:71-108 with the relationship EACH packet had.
 * One update, four samples: S1 before the mount (ordinary), S2/S3 riding
 * the boat (riding branch, then :97 func_12017_b(vehicle) ->
 * updateEntity(rider) -> Entity.updateRidden:819-822 and :108
 * updateEntity(player): motion ZERO), the dismount, S4 ordinary from zero.
 * Variant B: the boat is destroyed by the world tick (CaveWii ticks it there,
 * Beta inside the first riding packet): S1 takes the riding branch at its
 * recorded seat and is NOT zeroed (updateRidden finds the vehicle dead,
 * Entity.java:816-817), S2 is an ordinary packet. */
static bool r_break_boat;
static struct entity* r_boat;
static void r_hook(struct server_local* sv, struct server_world* w, int phase) {
	if(z_hit_fired || phase != 1 || w != &sv->worlds[0])
		return;
	z_hit_fired = true;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	(void)server_local_damage_actor(sv, &sv->player, 8, 0.0F, 0.0F);
	if(r_break_boat) {
		server_local_set_boat_occupancy_for(sv, r_boat, &sv->player, false);
		r_boat->delay_destroy = 0;
	}
}
static void r_sample(struct server_local* sv, double x, double y, double z) {
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = x, .y = y, .z = z, .rx = 0.0F, .ry = 0.0F,
			.vel_x = 0.25F, .vel_y = 0.0F, .vel_z = 0.0F,
			.on_ground = false, .sneaking = false,
			.correction_seq = sv->player.correction_seq,
		},
	};
	server_local_test_process_rpc(sv, &pos);
}
static void r_ride_run(bool break_boat) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle */
	r_boat = server_local_spawn_boat((vec3) {19.9F, 50.0F, 20.5F}, sv);
	assert(r_boat);
	r_boat->tick_server = NULL; /* frozen: the seat is a pure pose */
	r_boat->orient[0] = 0.0F;
	const uint32_t boat_id = r_boat->id;
	double sx, sy, sz;
	z_seat(r_boat, true, &sx, &sy, &sz);
	sv->player.x = sx;
	sv->player.y = sy;
	sv->player.z = sz;
	sv->player.on_ground = false;
	sv->player.sneaking = false;
	sv->player.in_web = false;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.vel_x = (double)0.03F;
	MOTY_SET(sv->player, -0.0784);
	sv->player.vel_z = (double)-0.01F;
	const double m0[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	g_vel_n = 0;
	const unsigned branch0 = server_local_test_riding_branch_count;
	if(!break_boat) {
		r_sample(sv, sx, sy, sz);                         /* S1: not riding */
		server_local_set_boat_occupancy_for(sv, r_boat, &sv->player, true);
		r_sample(sv, sx, sy, sz);                         /* S2: riding */
		r_sample(sv, sx, sy, sz);                         /* S3: riding */
		server_local_set_boat_occupancy_for(sv, r_boat, &sv->player, false);
		r_sample(sv, sx, sy, sz);                         /* S4: not riding */
		assert(sv->player.motion_q_n == 4);
		assert(!sv->player.motion_q[0].riding && !sv->player.motion_q[3].riding);
		for(int k = 1; k <= 2; k++)
			assert(sv->player.motion_q[k].riding
				   && sv->player.motion_q[k].vehicle_id == boat_id
				   && sv->player.motion_q[k].vehicle_type == ENTITY_BOAT);
	} else {
		server_local_set_boat_occupancy_for(sv, r_boat, &sv->player, true);
		r_sample(sv, sx, sy, sz);                         /* S1: riding */
		r_sample(sv, sx, sy, sz);                         /* S2: riding */
		assert(sv->player.motion_q_n == 2 && sv->player.motion_q[0].riding
			   && sv->player.motion_q[1].riding
			   && sv->player.motion_q[0].seat_x == sx
			   && sv->player.motion_q[0].seat_y == sy);
	}
	r_break_boat = break_boat;
	z_hit_fired = false;
	server_local_test_phase_hook = r_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	r_break_boat = false;
	assert(z_hit_fired);
	double e[3];
	unsigned want_branches;
	if(!break_boat) {
		/* S1's step is overwritten by S2's zero; S4 steps from zero. */
		e[0] = 0.0;
		e[1] = wit_y(0.0);
		e[2] = 0.0;
		want_branches = 2;
	} else {
		/* S1: riding branch in open air (friction 0.91F, gravity), the input
		 * move, motionX/Z = input -- vehicle dead: no zero. S2: ordinary. */
		double y1 = wit_y(m0[1]);
		e[0] = wit_xz((double)0.25F);
		e[1] = wit_y(y1);
		e[2] = wit_xz(0.0);
		want_branches = 1;
		assert(server_local_test_riding_branch_motion[0] == (double)0.25F
			   && server_local_test_riding_branch_motion[1] == y1
			   && server_local_test_riding_branch_motion[2] == 0.0);
	}
	printf("  (r) R1 %s: branches %u (want %u) msgs %zu v (%.9g, %.9g, %.9g) want (%.9g, %.9g, %.9g)\n",
		   break_boat ? "boat broken in the world tick" : "mount/dismount around four packets",
		   server_local_test_riding_branch_count - branch0, want_branches, g_vel_n,
		   g_vel_n ? (double)g_vel[0].v[0] : 0.0, g_vel_n ? (double)g_vel[0].v[1] : 0.0,
		   g_vel_n ? (double)g_vel[0].v[2] : 0.0, (double)q28(e[0]),
		   (double)q28(e[1]), (double)q28(e[2]));
	fflush(stdout);
	assert(server_local_test_riding_branch_count - branch0 == want_branches);
	assert(g_vel_n == 1 && !g_vel[0].add && g_vel[0].owner == 0);
	assert(g_vel[0].v[0] == q28(e[0]) && g_vel[0].v[1] == q28(e[1])
		   && g_vel[0].v[2] == q28(e[2]));
	assert(sv->player.vel_x == e[0] && MOTY(sv->player) == e[1]
		   && sv->player.vel_z == e[2]);
	struct entity** still = dict_entity_get(sv->entities, boat_id);
	if(still && *still) {
		dict_entity_erase(sv->entities, boat_id);
		free(r_boat);
	}
	r_boat = NULL;
	w_close();
}

/* ---- K1: the dead body's packets (ASTRA-V4-REVIEW dead-client loop) ----
 * A dead Beta EntityPlayerMP still gets one Packet10 per client tick until
 * deathTime > 20; each runs onUpdateEntity: deathTime++, moveEntityWith-
 * Heading (gravity, air friction) and S/EntityLiving.java:623-629 at the box
 * that move LEFT. CaveWii's dead client streams nothing, so the network
 * phase runs that one stationary corpse packet per update: 21 pushes of a
 * never-ticking zombie 0.55 +x, then none; the corpse's own half lands on
 * its evolving motion after that packet's friction, exactly. */
static void k_corpse_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle */
	sv->player.vel_x = 0.0;
	sv->player.vel_z = 0.0;
	MOTY_SET(sv->player, 0.0);
	sv->player.on_ground = false;
	const double feet = 43.0 - (double)PLAYER_EYE_HEIGHT;
	struct entity* zom = frozen_mob(sv, 0, 9.05F, (float)feet, 8.5F,
									MONSTER_ZOMBIE);
	(void)server_local_damage_actor(sv, &sv->player, 4 * MAX_PLAYER_HEALTH,
									0.0F, 0.0F);
	assert(sv->player.health <= 0);
	sv->player.x = 8.5; /* the corpse pose (the lethal commit may move it) */
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	double m[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	int pushes = 0;
	for(int u = 1; u <= 24; u++) {
		/* the zombie tracks the corpse's next moved box at +0.55 x, so the
		 * geometry (and the overlap) is the same every update */
		const float zx = (float)(sv->player.x + m[0] + 0.55);
		zom->pos[0] = zx;
		zom->data.monster.motion_d.pos[0] = (double)zx;
		zom->data.monster.motion_d.motion[0] = 0.0;
		zom->data.monster.motion_d.motion[2] = 0.0;
		glm_vec3_zero(zom->vel);
		const int dt_before = sv->player.death_ticks;
		server_local_test_update_once(sv);
		double hx = 0.0, hz = 0.0;
		bool want_push = false;
		if(dt_before <= 20) {
			/* the corpse packet: air move by m, gravity, friction 0.91F,
			 * then the push at the moved box (centre x = 8.5 + m[0]) */
			const double cx = sv->player.x + m[0], cz = sv->player.z + m[2];
			m[1] = wit_y(m[1]);
			m[0] = wit_xz(m[0]);
			m[2] = wit_xz(m[2]);
			want_push = beta_collision((double)zom->pos[0], (double)zom->pos[2],
									   cx, cz, &hx, &hz);
			if(want_push) {
				m[0] += hx;
				m[2] += hz;
			}
		}
		const bool pushed = zom->data.monster.motion_d.motion[0] != 0.0;
		pushes += pushed;
		if(u <= 2 || u >= 20)
			printf("  (k) K1 update %d: death_ticks %d pushed %d (want %d) corpse motion (%.9g, %.9g, %.9g)\n",
				   u, sv->player.death_ticks, (int)pushed, (int)want_push,
				   sv->player.vel_x, MOTY(sv->player), sv->player.vel_z);
		fflush(stdout);
		assert(pushed == want_push);
		if(pushed)
			assert(zom->data.monster.motion_d.motion[0] == -hx
				   && zom->data.monster.motion_d.motion[2] == -hz);
		assert(sv->player.vel_x == m[0] && MOTY(sv->player) == m[1]
			   && sv->player.vel_z == m[2]);
		/* deathTime advances only inside a packet: after the 21st corpse
		 * packet the dead client sends nothing more, so it stays 21 */
		assert(sv->player.death_ticks == (u < 21 ? u : 21));
	}
	assert(pushes == 21);
	dict_entity_erase(sv->entities, zom->id);
	free(zom);
	puts("  (k) K1 dead body: one stationary corpse packet per update in the network phase (deathTime 1..21), gravity + friction + push at the moved box, exact; none after OK");
	w_close();
}

/* ---- R2: fresh-hit Math.random draws (S/EntityLiving.java:321-332) ------
 * On a FRESH hit only: an attacker at (nearly) the same column draws the
 * separation loop (var4: two draws, then var6: two draws, per iteration,
 * left operand first) on java.lang.Math's stream; a null attacker draws ONE
 * (attackedAtYaw); a non-coincident attacker, an absorbed hit and an
 * excess-only hit draw none. The knockBack uses the drawn delta. A lethal
 * fresh hit draws before onDeath. Every assertion pins the exact stream
 * state and the exact server motion. */
static void r_ref_sep(struct java_random* ref, double* dx, double* dz) {
	while(*dx * *dx + *dz * *dz < 1.0E-4) {
		const double a = jrand_next_double(ref);
		const double b = jrand_next_double(ref);
		*dx = (a - b) * 0.01;
		const double c = jrand_next_double(ref);
		const double d = jrand_next_double(ref);
		*dz = (c - d) * 0.01;
	}
}
static void r_expect_stream(const struct java_random* ref, const char* tag) {
	struct server_local_math_random_state st;
	server_local_math_random_capture(&st);
	printf("  (r) R2 %s: stream %s\n", tag,
		   st.rng.seed == ref->seed ? "matches" : "DIFFERS");
	fflush(stdout);
	assert(st.initialized && st.rng.seed == ref->seed);
}
static void r_fresh_window(struct server_player* p) {
	p->hurt_resist_time = 0;
	p->last_damage = 0;
	p->health = MAX_PLAYER_HEALTH;
	p->vel_x = (double)0.03F;
	p->motion_y = -0.0784;
	p->vel_z = (double)-0.01F;
}
static void r_rng_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle */
	server_local_test_set_difficulty(2);
	struct server_player* p = &sv->player;
	const int64_t SEED = 0x5EEDBEEFLL;
	struct java_random ref;
	/* null attacker, fresh: one draw; absorbed / excess-only: none */
	r_fresh_window(p);
	server_local_test_seed_math_random(SEED);
	jrand_set_seed(&ref, SEED);
	(void)server_local_damage_actor(sv, p, 8, 0.0F, 0.0F);
	(void)jrand_next_double(&ref);
	r_expect_stream(&ref, "null attacker fresh = 1 draw");
	assert(p->vel_x == (double)0.03F && p->motion_y == -0.0784); /* no knockBack */
	(void)server_local_damage_actor(sv, p, 8, 0.0F, 0.0F);   /* absorbed */
	r_expect_stream(&ref, "absorbed = 0 draws");
	(void)server_local_damage_actor(sv, p, 16, 0.0F, 0.0F);  /* excess-only */
	r_expect_stream(&ref, "excess-only = 0 draws");
	/* a coincident attacker (arrow entry): the separation loop, then
	 * knockBack from the drawn delta */
	for(int slot = 0; slot < 3; slot++) {
		const int64_t seed = SEED + 17 * (slot + 1);
		r_fresh_window(p);
		server_local_test_seed_math_random(seed);
		jrand_set_seed(&ref, seed);
		double dx = 0.0, dz = 0.0;
		if(slot == 2) { /* nearly coincident: 0.005 < 0.01 */
			dx = 0.005;
			dz = 0.0;
		}
		const double ax = p->x + dx, az = p->z + dz;
		r_ref_sep(&ref, &dx, &dz);
		double m[3] = {(double)0.03F, -0.0784, (double)-0.01F};
		beta_knockback(m, dx, dz, 0.0, 0.0);
		bool acc = false;
		(void)server_local_damage_actor_from_attacker(sv, p, 8, true, ax, az,
													  false, &acc);
		r_expect_stream(&ref, slot == 2 ? "near-coincident attacker = separation loop"
										: "coincident attacker = separation loop");
		printf("  (r) R2 coincident seed %lld: motion (%.17g, %.17g, %.17g) want (%.17g, %.17g, %.17g)\n",
			   (long long)seed, p->vel_x, p->motion_y, p->vel_z, m[0], m[1], m[2]);
		assert(acc && p->vel_x == m[0] && p->motion_y == m[1] && p->vel_z == m[2]);
	}
	/* a non-coincident attacker: no draw */
	r_fresh_window(p);
	server_local_test_seed_math_random(SEED);
	jrand_set_seed(&ref, SEED);
	{
		double m[3] = {(double)0.03F, -0.0784, (double)-0.01F};
		beta_knockback(m, p->x + 2.0, p->z - 1.0, p->x, p->z);
		(void)server_local_damage_actor_from_attacker(sv, p, 8, true,
			p->x + 2.0, p->z - 1.0, false, NULL);
		r_expect_stream(&ref, "separated attacker = 0 draws");
		assert(p->vel_x == m[0] && p->motion_y == m[1] && p->vel_z == m[2]);
	}
	/* mob melee on the same column (server_local_hurt_player_for) */
	{
		struct entity* zom = frozen_mob(sv, 0, (float)p->x, (float)(p->y - 1.62),
										(float)p->z, MONSTER_ZOMBIE);
		r_fresh_window(p);
		const int64_t seed = SEED ^ 0x77;
		server_local_test_seed_math_random(seed);
		jrand_set_seed(&ref, seed);
		double dx = (double)zom->pos[0] - p->x, dz = (double)zom->pos[2] - p->z;
		r_ref_sep(&ref, &dx, &dz);
		double m[3] = {(double)0.03F, -0.0784, (double)-0.01F};
		beta_knockback(m, dx, dz, 0.0, 0.0);
		server_local_hurt_player_for(sv, p, zom);
		r_expect_stream(&ref, "coincident melee zombie = separation loop");
		assert(p->vel_x == m[0] && p->motion_y == m[1] && p->vel_z == m[2]);
		/* a lethal fresh coincident hit draws before onDeath (empty
		 * inventory: no drop draws follow) */
		r_fresh_window(p);
		p->health = 1;
		server_local_test_seed_math_random(seed + 1);
		jrand_set_seed(&ref, seed + 1);
		dx = (double)zom->pos[0] - p->x;
		dz = (double)zom->pos[2] - p->z;
		r_ref_sep(&ref, &dx, &dz);
		server_local_hurt_player_for(sv, p, zom);
		assert(p->health <= 0);
		r_expect_stream(&ref, "lethal coincident melee = separation loop before death");
		dict_entity_erase(sv->entities, zom->id);
		free(zom);
	}
	puts("  (r) R2 fresh-hit Math.random: null 1 draw, coincident separation loop (exact order/stream/knockBack), none for absorbed/excess/separated, lethal draws first OK");
	w_close();
}

/* ---- A1: an arrow's OWNER decides anger and aggro -----------------------
 * S/EntityArrow.java:147 attackEntityFrom(this.owner, 4); S/EntityPigZombie.
 * java:49-60 angers (becomeAngryAt: nextInt(400), nextInt(40) on the
 * struck mob's rand) ONLY for `var1 instanceof EntityPlayer`, before the
 * damage gate; S/EntityMob.java:33-38 sets playerToAttack = the attacker.
 * A player's arrow angers and targets the player; a dispensed arrow (null
 * owner) and a reloaded one (owner never saved: null) do neither and spend
 * no draw; a skeleton's arrow angers nobody. */
/* LCG steps from `a` to `b` (each java.util.Random next() is one step). */
static int a_lcg_steps(struct java_random a, const struct java_random* b) {
	for(int k = 0; k < 512; k++) {
		if(a.seed == b->seed)
			return k;
		(void)jrand_next(&a, 32);
	}
	return -1;
}
static int a_steps[2][4];
static void a_owner_run(void) {
	struct server_local* sv = &W;
	enum { AO_PLAYER, AO_DISPENSER, AO_RELOADED, AO_SKELETON };
	static const char* const ao_name[] = {"player arrow", "dispenser arrow",
		"reloaded arrow", "skeleton arrow"};
	for(int target = 0; target < 2; target++) /* 0 pig zombie, 1 zombie */
		for(int kind = AO_PLAYER; kind <= AO_SKELETON; kind++) {
			w_session(0, false);
			server_local_test_update_once(sv);
			server_local_test_set_difficulty(2);
			struct entity* mob = frozen_mob(sv, 0, 20.5F, 41.0F, 20.5F,
				target ? MONSTER_ZOMBIE : MONSTER_PIG_ZOMBIE);
			mob->data.monster.anger_level = 0;
			mob->data.monster.aggro_time = 0;
			struct entity* skel = kind == AO_SKELETON
				? frozen_mob(sv, 0, 26.5F, 41.0F, 26.5F, MONSTER_SKELETON) : NULL;
			const struct java_random rng0 = mob->gameplay_rng;
			(void)rng0;
			struct entity* a = calloc(1, sizeof *a);
			assert(a);
			const uint32_t aid = 900777u;
			uint32_t owner = 0;
			bool from_player = true;
			if(kind == AO_DISPENSER)
				owner = DISPENSER_OWNER;
			else if(kind == AO_RELOADED)
				owner = aid;
			else if(kind == AO_SKELETON) {
				owner = skel->id;
				from_player = false;
			}
			entity_arrow(aid, a, true, &sv->worlds[0], owner, ITEM_ARROW,
						 ARROW_DAMAGE, from_player);
			a->data.arrow.from_skeleton = kind == AO_SKELETON;
			a->data.arrow.shooter_actor = ACTOR_IDENT_HOST;
			a->teleport(a, (vec3) {20.5F, 42.0F, 18.5F});
			glm_vec3_copy((vec3) {0.0F, 0.0F, 2.0F}, a->vel);
			const bool hit_removed = a->tick_server(a, sv);
			const int drew = a_lcg_steps(rng0, &mob->gameplay_rng);
			a_steps[target][kind] = drew;
			const bool player_owner = kind == AO_PLAYER;
			printf("  (a) A1 %s -> %s: hit %d anger %d aggro %d drew %d\n",
				   ao_name[kind], target ? "zombie" : "pig zombie",
				   (int)hit_removed, (int)mob->data.monster.anger_level,
				   (int)mob->data.monster.aggro_time, (int)drew);
			fflush(stdout);
			assert(hit_removed);
			if(target == 0) {
				if(player_owner) {
					struct java_random r = rng0;
					const int anger = 400 + jrand_next_int_bound(&r, 400);
					const int snd = jrand_next_int_bound(&r, 40);
					assert(mob->data.monster.anger_level == anger
						   && mob->data.monster.anger_sound_delay == snd);
				} else {
					assert(mob->data.monster.anger_level == 0
						   && mob->data.monster.anger_sound_delay == 0);
				}
			}
			if(player_owner)
				assert(mob->data.monster.aggro_time == MOB_TARGET_RETENTION_TICKS
					   && actor_ident_equal(mob->data.monster.target_actor,
											ACTOR_IDENT_HOST));
			else
				assert(mob->data.monster.aggro_time == 0);
			if(!a->data.arrow.in_ground) {
				/* the arrow was consumed by the hit (tick returned true) */
			}
			free(a);
			dict_entity_erase(sv->entities, mob->id);
			free(mob);
			if(skel) {
				dict_entity_erase(sv->entities, skel->id);
				free(skel);
			}
			w_close();
		}
	/* the player's arrow spends EXACTLY the two becomeAngryAt draws more on
	 * the pig zombie's rand than every null / mob-owned arrow; on the zombie
	 * every owner spends the same */
	for(int k = AO_DISPENSER; k <= AO_SKELETON; k++) {
		assert(a_steps[0][k] >= 0 && a_steps[0][AO_PLAYER] == a_steps[0][k] + 2);
		assert(a_steps[1][k] == a_steps[1][AO_PLAYER]);
	}
	puts("  (a) A1 arrow owner: only a genuine player owner angers (exact draws) and targets; dispenser / reloaded / skeleton arrows anger and target nobody, no draw OK");
}

/* ======================================================================
 * V6 F1 (ASTRA-V6-PLAN item 3; ASTRA-V5-REVIEW R1/R2): LOCAL production to
 * replay through the REAL client producer (clin_local_position_produce, the
 * function clin_update calls at its 50 ms cadence), the real svin transport
 * and the real server drain/replay. Q1 above fed svin directly and so
 * bypassed the producer-side token exhaustion; Q2 never filled the paused
 * queue. Every sample carries its authoring index in its yaw (wire ry =
 * index), so the server-side stream is checked for exact count and order.
 * The server drain's per-message hook stands in for the concurrently
 * running client frame: it flushes the client pending ring as tokens free
 * and, for the sustained case, authors the next sample.
 * ====================================================================== */
static struct entity f1_lp[2];
#define F1_MAX 2048
static int f1_exp_type[F1_MAX], f1_exp_idx[F1_MAX];
static size_t f1_exp_n;
static int f1_got_type[F1_MAX], f1_got_idx[F1_MAX];
static size_t f1_got_n;
static int f1_left, f1_next, f1_acts;
static unsigned f1_owner;
static bool f1_refill;
static size_t f1_upd_msgs;
static float f1_cam_y;

static void f1_cam(unsigned owner, int idx) {
	struct camera* c = &gstate.players[owner].camera;
	struct server_player* body = owner ? W.guest_actor : &W.player;
	c->x = (float)body->x;
	c->y = f1_cam_y;
	c->z = (float)body->z;
	c->rx = 0.0F;
	c->ry = glm_rad((float)idx + 90.0F); /* wire ry = deg(ry) - 90 = idx */
}
/* One producer call. true = authored (and recorded in authoring order). */
/* Transport/replay fixture through the production reservation API:
 * clin_local_tick_begin reserves the credit BEFORE physics; false = the tick
 * is suspended and physics must not run. Then the tick's physics (here: the
 * body reaches its final pose) and clin_local_tick_end publishes exactly one
 * immutable sample of it. This fixture does not execute owner physics;
 * test_f1_r4 separately exercises main's actual dispatch and real physics.
 * true = fixture-authored (recorded in authoring order). */
static bool f1_author(void) {
	struct clin_local_tick_stats b, a;
	clin_local_tick_stats(f1_owner, &b);
	if(!clin_local_tick_begin(f1_owner))
		return false;
	f1_cam(f1_owner, f1_next);
	struct camera sample = gstate.players[f1_owner].camera;
	clin_local_tick_end(f1_owner, &sample);
	clin_local_tick_stats(f1_owner, &a);
	if(a.executed != b.executed + 1)
		printf("  (F1) executed tick %d of owner %u published NO sample (dropped=%u)\n",
			   f1_next, f1_owner, svin_dropped_count());
	fflush(stdout);
	assert(a.executed == b.executed + 1); /* physics ran -> its sample exists */
	assert(f1_exp_n < F1_MAX);
	f1_exp_type[f1_exp_n] = SRPC_PLAYER_POS;
	f1_exp_idx[f1_exp_n++] = f1_next++;
	f1_left--;
	if(f1_owner == 0 && f1_next % 7 == 0) { /* a mixed ordinary action */
		struct server_rpc a = {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot.slot = (size_t)(f1_acts % 9)};
		assert(svin_rpc_send(&a));
		f1_exp_type[f1_exp_n] = SRPC_HOTBAR_SLOT;
		f1_exp_idx[f1_exp_n++] = f1_acts++ % 9;
	}
	return true;
}
static double f1_only_x = -1.0; /* >= 0: record only samples at this x */
static void f1_hook(struct server_local* sv, const struct server_rpc* call) {
	(void)sv;
	if(call->type != SRPC_PLAYER_POS && call->type != SRPC_HOTBAR_SLOT)
		return;
	if(f1_only_x >= 0.0 && (call->type != SRPC_PLAYER_POS
							|| call->payload.player_pos.x != f1_only_x)) {
		svin_rpc_flush_pending();
		return;
	}
	assert(f1_got_n < F1_MAX);
	f1_got_type[f1_got_n] = (int)call->type;
	f1_got_idx[f1_got_n++] = call->type == SRPC_PLAYER_POS
		? (int)lroundf(call->payload.player_pos.ry)
		: (int)call->payload.hotbar_slot.slot;
	f1_upd_msgs++;
	svin_rpc_flush_pending(); /* the client frame flushes as tokens free */
	if(f1_refill && call->type == SRPC_PLAYER_POS && f1_left > 0)
		(void)f1_author(); /* one new sample per processed one (Q1 shape) */
}
static void f1_client_setup(unsigned owner) {
	memset(&f1_lp[owner], 0, sizeof f1_lp[owner]);
	gstate.remote_session = false;
	gstate.players[owner].local_player = &f1_lp[owner];
	memset(&gstate.players[owner].ride, 0, sizeof gstate.players[owner].ride);
	memset(&gstate.players[owner].camera.ysize, 0,
		   sizeof gstate.players[owner].camera.ysize);
	struct server_player* body = owner ? W.guest_actor : &W.player;
	gstate.players[owner].player_sync.correction_seq = body->correction_seq;
	f1_cam_y = (float)body->y;
	f1_owner = owner;
	f1_exp_n = f1_got_n = 0;
	f1_next = f1_acts = 0;
	f1_refill = false;
}
static void f1_check_stream(const char* what) {
	if(f1_got_n != f1_exp_n)
		printf("  (F1) %s: server processed %zu of %zu authored messages\n",
			   what, f1_got_n, f1_exp_n);
	fflush(stdout);
	assert(f1_got_n == f1_exp_n);
	for(size_t k = 0; k < f1_exp_n; k++)
		assert(f1_got_type[k] == f1_exp_type[k] && f1_got_idx[k] == f1_exp_idx[k]);
}

/* F1a: `total` host samples through the producer. Everything the producer
 * authors before the server drains piles up behind the 16 inbox tokens; the
 * blast update then drains at Beta's 101-message budget while the hook keeps
 * the pipeline full. Every sample steps the post-blast motion exactly once,
 * in its own update's network phase; one exact velocity message. */
static void f1_producer_host_run(int total) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv); /* settle: no blast yet */
	svin_set_local_motion(true);
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	const double pre_x = sv->player.vel_x, pre_z = sv->player.vel_z;
	const double pre_y = MOTY(sv->player);
	double imp[3];
	const int dmg = blast_terms(w_center, 8.5, 43.0, 8.5, 8.0, imp);
	const uint32_t drop0 = svin_dropped_count();
	assert(svin_held_count() == 0 && svin_pending_count() == 0);
	f1_client_setup(0);
	f1_left = total;
	/* the client authors ahead of a server that is not draining */
	while(f1_left > 0 && f1_author()) {}
	const int upfront = f1_next;
	f1_refill = true;
	server_local_test_rpc_done_hook = f1_hook;
	double ex = addm(pre_x, imp[0]), ey = pre_y + imp[1], ez = addm(pre_z, imp[2]);
	int u = 0, stepped = 0;
	for(; u < 40 && (f1_left > 0 || f1_got_n < f1_exp_n); u++) {
		f1_upd_msgs = 0;
		const size_t before = f1_got_n;
		if(u == 0) {
			w_hook_world = 0;
			w_hook_fired = false;
			w_hook_lethal = false;
			server_local_test_phase_hook = w_hook;
		}
		server_local_test_update_once(sv);
		server_local_test_phase_hook = NULL;
		int samples = 0;
		for(size_t k = before; k < f1_got_n; k++)
			samples += f1_got_type[k] == SRPC_PLAYER_POS;
		for(int k = 0; k < samples; k++) {
			ex = wit_xz(ex);
			ey = wit_y(ey);
			ez = wit_xz(ez);
		}
		stepped += samples;
		assert(f1_upd_msgs <= BETA_READ_PACKETS_PER_TICK);
		assert(sv->player.motion_q_n == 0); /* all replayed in their update */
		assert(sv->player.vel_x == ex && MOTY(sv->player) == ey
			   && sv->player.vel_z == ez);
		if(u == 0) {
			assert(w_hook_fired && sv->player.health == MAX_PLAYER_HEALTH - dmg * 8);
			assert(g_vel_n == 1 && !g_vel[0].add && g_vel[0].owner == 0
				   && g_vel[0].v[0] == q28(ex) && g_vel[0].v[1] == q28(ey)
				   && g_vel[0].v[2] == q28(ez));
		}
		if(f1_left == 0 && f1_got_n == f1_exp_n)
			break;
		/* the next client frame: author until backpressure */
		while(f1_left > 0 && f1_author()) {}
	}
	assert(u < 40); /* finite time */
	server_local_test_rpc_done_hook = NULL;
	server_local_test_update_once(sv); /* nothing left: no repeat message */
	f1_check_stream("F1a host");
	assert(stepped == total && f1_next == total && g_vel_n == 1);
	assert(svin_held_count() == 0 && svin_pending_count() == 0);
	assert(svin_dropped_count() == drop0 && svin_motion_refused_count() == 0
		   && sv->actor_motion_overflow == 0);
	printf("  (F1) F1a host producer %d samples + %d actions (%d authored before the first drain, %u backpressure waits): all stepped once in order, %d updates, one exact message OK\n",
		   total, f1_acts, upfront, clin_local_position_backpressure_count(0),
		   u + 1);
	gstate.players[0].local_player = NULL;
	svin_set_local_motion(false);
	w_close();
}

/* F1b: the GUEST owner through its producer and the guest position route
 * (hand-installed admitted body, as V6; the real admission path is F5's).
 * The host streams too, interleaved, so the two owners share the 16 tokens:
 * per-owner order and count exact, each owner's motion stepped once per
 * sample. */
static void f1_producer_guest_run(int total) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	svin_set_split_accept(true);
	struct server_player* g = calloc(1, sizeof(*g));
	assert(g);
	assert(inventory_create(&g->inventory, &inventory_logic_player, sv, g,
							INVENTORY_SIZE, 0, 0, 0));
	g->active_inventory = &g->inventory;
	g->inventory.reply = vel_capture;
	g->inventory.reply_user = (void*)(intptr_t)1;
	g->has_pos = true;
	g->finished_loading = true;
	g->health = MAX_PLAYER_HEALTH;
	g->dimension = sv->player.dimension;
	g->world_slot = 0;
	g->on_ground = false;
	g->x = 10.5;
	g->y = 43.0;
	g->z = 10.5;
	g->vel_x = 0.05;
	sv->guest_actor = g;
	sv->guest_gameplay = true;
	sv->guest_generation = 31;
	gstate.players[1].local_generation = 31;
	const double gpre_x = g->vel_x, gpre_z = g->vel_z, gpre_y = MOTY(*g);
	const double hpre_x = sv->player.vel_x, hpre_z = sv->player.vel_z;
	const double hpre_y = MOTY(sv->player);
	const uint32_t drop0 = svin_dropped_count();
	/* both producers author alternately ahead of the server */
	f1_client_setup(1);
	f1_lp[0] = (struct entity) {0};
	gstate.players[0].local_player = &f1_lp[0];
	gstate.players[0].player_sync.correction_seq = sv->player.correction_seq;
	int host_n = 0, guest_n = 0;
	f1_left = total;
	f1_only_x = 10.5;
	for(int k = 0; k < total; k++) {
		assert(f1_author());
		guest_n++;
		gstate.players[0].camera.x = (float)sv->player.x;
		gstate.players[0].camera.y = (float)sv->player.y;
		gstate.players[0].camera.z = (float)sv->player.z;
		gstate.players[0].camera.ry = glm_rad(90.0F);
		if(!clin_local_tick_begin(0)) { /* host suspended: the guest runs on */
			server_local_test_rpc_done_hook = f1_hook;
			server_local_test_update_once(sv);
			server_local_test_rpc_done_hook = NULL;
			continue;
		}
		{
			struct camera hs = gstate.players[0].camera;
			clin_local_tick_end(0, &hs);
		}
		host_n++;
	}
	server_local_test_rpc_done_hook = f1_hook;
	int u = 0;
	for(; u < 20 && (svin_pending_count() + svin_held_count() > 0
					 || f1_got_n < f1_exp_n); u++)
		server_local_test_update_once(sv);
	server_local_test_rpc_done_hook = NULL;
	assert(u < 20);
	server_local_test_update_once(sv); /* settle the trailing replay */
	f1_only_x = -1.0;
	f1_check_stream("F1b guest");
	double gx = gpre_x, gy = gpre_y, gz = gpre_z;
	for(int k = 0; k < guest_n; k++) {
		gx = wit_xz(gx);
		gy = wit_y(gy);
		gz = wit_xz(gz);
	}
	double hx = hpre_x, hy = hpre_y, hz = hpre_z;
	for(int k = 0; k < host_n; k++) {
		hx = wit_xz(hx);
		hy = wit_y(hy);
		hz = wit_xz(hz);
	}
	printf("  (F1) F1b guest %d + host %d samples: guest motion (%.9g, %.9g, %.9g) host (%.9g, %.9g, %.9g)\n",
		   guest_n, host_n, g->vel_x, MOTY(*g), g->vel_z, sv->player.vel_x,
		   MOTY(sv->player), sv->player.vel_z);
	fflush(stdout);
	assert(g->vel_x == gx && MOTY(*g) == gy && g->vel_z == gz);
	assert(sv->player.vel_x == hx && MOTY(sv->player) == hy
		   && sv->player.vel_z == hz);
	assert(svin_dropped_count() == drop0 && svin_motion_refused_count() == 0
		   && sv->actor_motion_overflow == 0);
	printf("  (F1) F1b both owners: guest %d / host %d samples through their producers sharing the 16 tokens, each stepped once in order OK\n",
		   guest_n, host_n);
	sv->guest_gameplay = false;
	sv->guest_actor = NULL;
	sv->guest_generation = 0;
	g->inventory.reply = NULL;
	inventory_destroy(&g->inventory, g);
	free(g);
	gstate.players[0].local_player = NULL;
	gstate.players[1].local_player = NULL;
	svin_set_split_accept(false);
	svin_set_local_motion(false);
	w_close();
}

/* F1c: a FULL paused backlog through the producer -- 101 damaging landings
 * held by the pause (the server queue is at ACTOR_MOTION_QUEUE), two more
 * landing triples behind them that need storage, then the resume, then one
 * more stationary triple. FIFO alone deadlocks (the resume sits behind
 * samples only an unpaused update can make room for). Required: the resume
 * progresses in finite time; every sample is processed exactly once in
 * order; the overtaken landings keep their paused meaning (no physics); the
 * post-resume samples step; one fresh landing, every other absorbed. */
static void f1_ground(bool on) { f1_lp[f1_owner].on_ground = on; }
/* one landing triple (the Q2 shape) as three executed owner ticks; returns
 * how many of the three ticks executed (a suspended tick runs no physics) */
static int f1_triple(void) {
	int ran = 0;
	f1_cam_y = 37.0F; f1_ground(false); ran += f1_author();
	f1_cam_y = 37.0F; f1_ground(true);  ran += f1_author();
	f1_cam_y = 43.0F; f1_ground(false); ran += f1_author();
	return ran;
}
/* one executed tick at (y, ground); a suspended attempt runs one server
 * update (the consumer drains) and retries -- the owner simply waits */
static void f1_tick_wait(float y, bool ground) {
	for(int k = 0;; k++) {
		if(k == 7)
			printf("  (F1) tick wait stuck: outstanding %u queue %d held %zu pending %zu motion-pending %zu paused %d\n",
				   svin_credit_outstanding(f1_owner), (int)W.player.motion_q_n,
				   svin_held_count(), svin_pending_count(),
				   svin_motion_pending_count(f1_owner), (int)W.paused);
		fflush(stdout);
		assert(k < 8);
		f1_cam_y = y;
		f1_ground(ground);
		if(f1_author())
			return;
		server_local_test_update_once(&W);
	}
}
static int f1_toggles_seen;
static void f1_count_hook(struct server_local* sv, const struct server_rpc* call) {
	if(call->type == SRPC_TOGGLE_PAUSE)
		f1_toggles_seen++;
	f1_hook(sv, call);
}

/* F1c / R2-A (round 3 liveness, ASTRA-F1-DESIGN-RULING 8, 17): a FULL paused
 * host queue (101 landings, each holding one credit). Every further host
 * tick is SUSPENDED before physics (no credit) -- nothing can demand actor
 * storage, so no token is ever storage-held; 20 ordinary actions wait; the
 * resume (and a later pause+resume pair) are serviced in a derived number
 * of updates: 1 for the resume (it is the next host message after the
 * actions, all within one 101-message budget). Conservation is exact at
 * every quiescent point: outstanding credits == queued events. */
static void f1_pause_full_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	sv->player.fall_distance = 0.0F;
	const double pre[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	const uint32_t drop0 = svin_dropped_count();
	f1_client_setup(0);
	f1_toggles_seen = 0;
	server_local_test_rpc_done_hook = f1_count_hook;
	assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	server_local_test_update_once(sv);
	assert(sv->paused && f1_toggles_seen == 1);
	f1_left = 1 << 20;
	int landings = 0;
	while(landings < ACTOR_MOTION_QUEUE) {
		for(int k = 0; k < 4 && landings < ACTOR_MOTION_QUEUE; k++) {
			f1_tick_wait(37.0F, false);
			f1_tick_wait(37.0F, true);
			landings++;
			/* the 101st landing holds the last credit: its rise waits */
			if(landings < ACTOR_MOTION_QUEUE)
				f1_tick_wait(43.0F, false);
		}
		server_local_test_update_once(sv);
		assert(sv->paused && sv->player.motion_q_n == landings);
		/* conservation: the settled drop/rise samples returned their
		 * credits; each kept landing holds exactly one */
		assert(svin_credit_outstanding(0) == (uint32_t)landings);
	}
	assert(sv->player.health == MAX_PLAYER_HEALTH && g_vel_n == 0);
	/* exhaustion: 18 more host ticks are all suspended BEFORE physics */
	const size_t exp_before = f1_exp_n;
	struct clin_local_tick_stats st0, st1;
	clin_local_tick_stats(0, &st0);
	for(int k = 0; k < 6; k++)
		assert(f1_triple() == 0);
	clin_local_tick_stats(0, &st1);
	assert(f1_exp_n == exp_before && st1.suspended == st0.suspended + 18
		   && st1.executed == st0.executed);
	/* pending ordinary actions, then the resume */
	for(int k = 0; k < 20; k++) {
		assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot.slot = (size_t)(k % 9)}));
		f1_exp_type[f1_exp_n] = SRPC_HOTBAR_SLOT;
		f1_exp_idx[f1_exp_n++] = k % 9;
	}
	assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	server_local_test_update_once(sv); /* derived bound: one update */
	printf("  (F1) F1c resume behind a full paused queue + 20 actions: paused %d queue %d outstanding %u held %zu pending %zu health %d\n",
		   (int)sv->paused, (int)sv->player.motion_q_n, svin_credit_outstanding(0),
		   svin_held_count(), svin_pending_count(), (int)sv->player.health);
	fflush(stdout);
	assert(!sv->paused && f1_toggles_seen == 2);
	assert(sv->player.motion_q_n == 0 && svin_credit_outstanding(0) == 0);
	assert(sv->player.health == MAX_PLAYER_HEALTH - 24); /* 1 fresh, rest absorbed */
	/* the 101 paused landings replayed with NO physics: motion untouched */
	assert(sv->player.vel_x == pre[0] && MOTY(sv->player) == pre[1]
		   && sv->player.vel_z == pre[2]);
	/* the owner resumes at normal cadence with current input: 3 executed
	 * ticks (each one sample; their physics starts from the landed body,
	 * F2 owns the ground-friction schedule) */
	f1_cam_y = 43.0F;
	f1_ground(false);
	for(int k = 0; k < 3; k++)
		assert(f1_author());
	server_local_test_update_once(sv);
	/* repeated toggles: pause then resume, each exactly once */
	assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	server_local_test_update_once(sv);
	assert(!sv->paused && f1_toggles_seen == 4);
	server_local_test_rpc_done_hook = NULL;
	f1_check_stream("F1c pause");
	assert(svin_dropped_count() == drop0 && svin_motion_refused_count() == 0
		   && sv->actor_motion_overflow == 0 && svin_held_count() == 0
		   && svin_credit_outstanding(0) == 0);
	printf("  (F1) F1c/R2-A full paused queue: 18 ticks suspended before physics, resume + 20 actions served in 1 update, repeated toggles exactly once, 3 post-resume steps, credits conserved OK\n");
	gstate.players[0].local_player = NULL;
	svin_set_local_motion(false);
	w_close();
}

/* ---- V6 F1 ROUND 2/3 (SOL-REVIEW-F1 P1-1..3, ASTRA-F1-DESIGN-RULING) --- */
static struct server_player* f1_guest_install(struct server_local* sv) {
	struct server_player* g = calloc(1, sizeof(*g));
	assert(g);
	assert(inventory_create(&g->inventory, &inventory_logic_player, sv, g,
							INVENTORY_SIZE, 0, 0, 0));
	g->active_inventory = &g->inventory;
	g->inventory.reply = vel_capture;
	g->inventory.reply_user = (void*)(intptr_t)1;
	g->has_pos = true;
	g->finished_loading = true;
	g->health = MAX_PLAYER_HEALTH;
	g->dimension = sv->player.dimension;
	g->world_slot = 0;
	g->x = 10.5;
	g->y = 43.0;
	g->z = 10.5;
	g->vel_x = 0.05;
	sv->guest_actor = g;
	sv->guest_gameplay = true;
	sv->guest_generation = 31;
	gstate.players[1].local_generation = 31;
	return g;
}
static void f1_guest_remove(struct server_local* sv, struct server_player* g) {
	sv->guest_gameplay = false;
	sv->guest_actor = NULL;
	sv->guest_generation = 0;
	g->inventory.reply = NULL;
	inventory_destroy(&g->inventory, g);
	free(g);
}

/* R2-B (P1-2, kept by ruling item 9): BUDGET deferral across a shared pause
 * change. The host is paused; the guest streams stationary samples through
 * its tick path faster than the 101-message budget (the drain hook refills
 * one per processed sample), so part of its stream is HELD by the budget
 * when the host's resume -- sent by the hook right after the guest's 101st
 * sample -- is processed in the same drain. Every guest sample authored
 * before the resume keeps the paused meaning (settled, no physics), however
 * late it is processed; only those authored after it step. */
static int f1_toggle_after = -1;
static int f1_guest_seen;
static int f1_toggle_idx = -1;
static void f1_budget_hook(struct server_local* sv, const struct server_rpc* call) {
	if(call->type == SRPC_PLAYER_POS && call->payload.player_pos.x == 10.5) {
		if(++f1_guest_seen == f1_toggle_after) {
			assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
			f1_toggle_idx = f1_next; /* guest samples >= this are post-resume */
			f1_toggle_after = -1;
		}
	}
	f1_hook(sv, call);
}
static void f1_guest_pause_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	svin_set_split_accept(true);
	struct server_player* g = f1_guest_install(sv);
	g_vel_n = 0;
	const double gpre[3] = {g->vel_x, MOTY(*g), g->vel_z};
	const uint32_t drop0 = svin_dropped_count();
	f1_client_setup(1);
	f1_only_x = 10.5;
	assert(svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	server_local_test_update_once(sv);
	assert(sv->paused);
	f1_cam_y = 43.0F;
	f1_ground(false);
	f1_left = 300;
	/* a 20-sample pipeline kept full one-for-one: when the resume is sent
	 * (after the guest's 95th processed sample) 20 earlier guest samples are
	 * still ahead of it, all within the 16 tokens + 4 pending, so the resume
	 * reaches the inbox in the SAME drain in which the guest's 101-message
	 * budget runs out -- it crosses 14 budget-HELD earlier guest samples. */
	for(int k = 0; k < 20; k++)
		assert(f1_author());
	f1_guest_seen = 0;
	f1_toggle_after = BETA_READ_PACKETS_PER_TICK - 6;
	f1_refill = true;
	server_local_test_rpc_done_hook = f1_budget_hook;
	int u = 0;
	for(; u < 8 && (f1_left > 0 || f1_got_n < f1_exp_n); u++) {
		server_local_test_update_once(sv);
		while(f1_left > 0 && f1_author()) {}
	}
	server_local_test_update_once(sv); /* the trailing replay */
	server_local_test_rpc_done_hook = NULL;
	f1_refill = false;
	f1_only_x = -1.0;
	int post = 0;
	for(size_t k = 0; k < f1_exp_n; k++)
		post += f1_exp_type[k] == SRPC_PLAYER_POS && f1_exp_idx[k] >= f1_toggle_idx;
	printf("  (F1) R2-B guest across the host resume: toggle at guest sample %d, %d post-resume of %zu, paused %d, motion (%.9g, %.9g, %.9g)\n",
		   f1_toggle_idx, post, f1_exp_n, (int)sv->paused, g->vel_x, MOTY(*g),
		   g->vel_z);
	fflush(stdout);
	assert(f1_toggle_idx > 0 && !sv->paused && u < 8);
	assert(post > 0 && f1_left == 0);
	f1_check_stream("R2-B guest");
	double ex = gpre[0], ey = gpre[1], ez = gpre[2];
	for(int k = 0; k < post; k++) {
		ex = wit_xz(ex);
		ey = wit_y(ey);
		ez = wit_xz(ez);
	}
	assert(g->vel_x == ex && MOTY(*g) == ey && g->vel_z == ez);
	assert(svin_dropped_count() == drop0 && sv->actor_motion_overflow == 0
		   && svin_credit_outstanding(1) == 0);
	puts("  (F1) R2-B budget-held guest samples authored before the HOST resume keep the paused meaning; only later ones step OK");
	f1_guest_remove(sv, g);
	gstate.players[1].local_player = NULL;
	svin_set_split_accept(false);
	svin_set_local_motion(false);
	w_close();
}

/* R2-C (P1-3 guard, ruling items 1, 10, 11, 16): the consumer is stopped;
 * frames run 0/1/2/3 owner ticks. Each attempted tick is either EXECUTED
 * (credit reserved first, physics, exactly one immutable sample) or
 * SUSPENDED before physics (counted, never owed later). After the consumer
 * resumes, the server receives exactly the executed samples, in order, each
 * stepped once; outstanding returns to 0. */
static void f1_backpressure_poses_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	f1_client_setup(0);
	g_vel_n = 0;
	sv->player.on_ground = false;
	const double pre[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	f1_left = 1 << 20;
	f1_cam_y = 43.0F;
	f1_ground(false);
	struct clin_local_tick_stats st0, st1;
	clin_local_tick_stats(0, &st0);
	static const int frame_ticks[] = {0, 1, 2, 3, 1, 0, 3, 2};
	int attempted = 0, executed = 0;
	for(int f = 0; f < 60; f++) /* consumer stopped: no server update */
		for(int t = 0; t < frame_ticks[f % 8]; t++) {
			attempted++;
			executed += f1_author();
		}
	clin_local_tick_stats(0, &st1);
	const uint32_t out_stopped = svin_credit_outstanding(0);
	printf("  (F1) R2-C consumer stopped: attempted %d executed %d suspended %u outstanding %u\n",
		   attempted, executed, st1.suspended - st0.suspended, out_stopped);
	fflush(stdout);
	assert(executed < attempted && executed <= SVIN_MOTION_CREDITS);
	assert(st1.executed - st0.executed == (uint32_t)executed);
	assert(st1.suspended - st0.suspended == (uint32_t)(attempted - executed));
	assert(out_stopped == (uint32_t)executed);
	assert(sv->player.vel_x == pre[0] && MOTY(sv->player) == pre[1]); /* no progress */
	server_local_test_rpc_done_hook = f1_hook;
	for(int u = 0; u < 6 && f1_got_n < f1_exp_n; u++)
		server_local_test_update_once(sv); /* drain only: no new tick */
	server_local_test_update_once(sv);
	server_local_test_rpc_done_hook = NULL;
	f1_check_stream("R2-C");
	double ex = pre[0], ey = pre[1], ez = pre[2];
	for(int k = 0; k < executed; k++) {
		ex = wit_xz(ex);
		ey = wit_y(ey);
		ez = wit_xz(ez);
	}
	assert(sv->player.vel_x == ex && MOTY(sv->player) == ey
		   && sv->player.vel_z == ez);
	assert(svin_credit_outstanding(0) == 0);
	printf("  (F1) R2-C every executed tick is exactly one server sample (%d), %d suspended before physics, credits back to 0 OK\n",
		   executed, attempted - executed);
	gstate.players[0].local_player = NULL;
	svin_set_local_motion(false);
	w_close();
}

static void observe(struct server_local* sv, struct block_info* info) {
	(void)sv;
	if(note_n[note_slot] >= 4096)
		return;
	int* r = notes[note_slot][note_n[note_slot]++];
	r[0] = info->x;
	r[1] = info->y;
	r[2] = info->z - note_oz;
	r[3] = info->block ? info->block->type : -1;
}

/* ==== F4-RNG (brief 18; ASTRA-RNG-RULING + ASTRA-RNG2-RULING) =============
 * Beta-owned random streams for the blast chain, verified against JVM-derived
 * java.util.Random witnesses. The witness block below is the verbatim output
 * of tmp/jobs/lead-1010/f4rng-witness/F4RngWitness.java on Microsoft OpenJDK
 * 21.0.7+6-LTS (source sha256 7f41cd2e1c8f9929c6cc2e4f37a3d3c30ad756cf6ec50
 * 9b50e58945f769ac2ad, output sha256 2736a03f59397f1fcfc38abd2d1572419352605f
 * c4beead5d6d3a30cdef525c9). Runtime-independent Random PRIMITIVES only
 * (ASTRA-RNG2 15): f4_anchor() first proves java_random.c reproduces every
 * anchor bit-for-bit, and only then drives the Beta transcriptions with it.
 *
 * COLLECTION ORDER: f4_j6_* is a transcription of the pinned OpenJDK 6
 * java.util.HashMap/HashSet (openjdk/jdk6 jdk/src/share/classes/java/util/
 * HashMap.java: DEFAULT_INITIAL_CAPACITY 16, load 0.75f, hash(h) = h ^=
 * (h>>>20)^(h>>>12); h^(h>>>7)^(h>>>4), indexFor h&(len-1), addEntry head
 * insertion + `if (size++ >= threshold) resize(2*len)`, transfer ascending j
 * with head insertion, HashIterator ascending buckets/head-first chains) and
 * ChunkPosition.hashCode x*8976890 + y*981131 + z (int wrap). Every assertion
 * that depends on it is PENDING ORACLE QUALIFICATION (ASTRA-RNG2 13/15: the
 * legacy-collection oracle seam qualifies it independently). */
struct jw_float { int index; uint32_t bits; };
/* ---- generated by F4RngWitness.java on OpenJDK 64-Bit Server VM 21.0.7+6-LTS */
#define JW_RAY_SEED INT64_C(0xf4a5eed)
#define JW_RAY_TOTAL 2000
static const struct jw_float jw_ray_FLOATS[] = {{0, 0x3f310723u}, {1, 0x3f251344u}, {2, 0x3eac05d2u}, {675, 0x3ef2a6bcu}, {1350, 0x3f53739au}, {1351, 0x3e82d090u}, {1352, 0x3e0c3f78u}, {1353, 0x3f10b4c8u}, {1354, 0x3e705524u}, {1999, 0x3e618cbcu}, };
#define JW_RAY_FINAL_STATE UINT64_C(0x38632f424fd0)
#define JW_BODY_SEED INT64_C(0x5eed0b0d)
#define JW_BODY_TOTAL 6
static const struct jw_float jw_body_FLOATS[] = {{0, 0x3e87b2a0u}, {1, 0x3ec24558u}, {2, 0x3e0781d4u}, {3, 0x3f44e317u}, {4, 0x3e18c79cu}, {5, 0x3f596885u}, };
#define JW_BODY_FINAL_STATE UINT64_C(0xd9688581a7be)
static const int jw_fire_int3[64] = {0, 0, 2, 0, 1, 2, 2, 2, 2, 2, 1, 2, 0, 1, 1, 1, 1, 2, 2, 0, 1, 0, 1, 1, 1, 0, 2, 1, 0, 0, 2, 0,
	0, 2, 2, 0, 2, 2, 0, 0, 1, 2, 2, 2, 2, 2, 1, 2, 0, 2, 2, 2, 2, 1, 0, 1, 1, 0, 0, 2, 2, 0, 2, 2,
	};
#define JW_FIRE_FINAL_STATE UINT64_C(0xd528f57c0bea)
#define JW_RAY_AMBIENT12000 5567
#define JW_GAUSS_SEED INT64_C(0x6A55CA5E)
#define JW_GAUSS_G0_BITS UINT64_C(0xbff0094a01b1648d)
#define JW_GAUSS_F1_BITS 0x3f1fdbb5u
#define JW_GAUSS_FINAL_STATE UINT64_C(0x9fdbb58daae6)
/* ---- end generated */

static uint32_t f4_fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t f4_dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* java_random.c must equal the JVM anchors before it may drive a model. */
static void f4_anchor(void) {
	struct java_random r;
	jrand_set_seed(&r, JW_RAY_SEED);
	size_t k = 0;
	for(int i = 0; i < JW_RAY_TOTAL; i++) {
		float f = jrand_next_float(&r);
		while(k < sizeof jw_ray_FLOATS / sizeof jw_ray_FLOATS[0]
			  && jw_ray_FLOATS[k].index == i)
			assert(f4_fbits(f) == jw_ray_FLOATS[k++].bits);
	}
	assert(r.seed == JW_RAY_FINAL_STATE);
	jrand_set_seed(&r, JW_BODY_SEED);
	for(int i = 0; i < JW_BODY_TOTAL; i++)
		assert(f4_fbits(jrand_next_float(&r)) == jw_body_FLOATS[i].bits);
	assert(r.seed == JW_BODY_FINAL_STATE);
	jrand_set_seed(&r, INT64_C(0xF1A3E5C7));
	for(int i = 0; i < 64; i++)
		assert(jrand_next_int_bound(&r, 3) == jw_fire_int3[i]);
	assert(r.seed == JW_FIRE_FINAL_STATE);
	jrand_set_seed(&r, JW_RAY_SEED);
	assert(jrand_next_int_bound(&r, 12000) == JW_RAY_AMBIENT12000);
	jrand_set_seed(&r, JW_GAUSS_SEED);
	assert(f4_dbits(jrand_next_gaussian(&r)) == JW_GAUSS_G0_BITS);
	assert(r.have_next_gaussian);
	assert(f4_fbits(jrand_next_float(&r)) == JW_GAUSS_F1_BITS);
	assert(r.seed == JW_GAUSS_FINAL_STATE);
	puts("  (F4.0) java_random == JVM anchors: rays 2000, body 6, fire nextInt(3) 64, "
		 "ambient nextInt(12000), gaussian cache OK");
}

/* --- OpenJDK 6 HashSet<ChunkPosition> model (PENDING ORACLE QUALIFICATION) */
#define F4_J6_MAX 16384
struct f4_j6 {
	int key[F4_J6_MAX][3];
	int32_t hash[F4_J6_MAX];
	int32_t next[F4_J6_MAX];
	int32_t* table;
	uint32_t len;
	uint32_t threshold;
	uint32_t size;
};
static int32_t f4_j6_hash(int32_t h) {
	uint32_t u = (uint32_t)h;
	u ^= (u >> 20) ^ (u >> 12);
	return (int32_t)(u ^ (u >> 7) ^ (u >> 4));
}
static void f4_j6_init(struct f4_j6* m) {
	free(m->table);
	m->len = 16;
	m->threshold = (uint32_t)(16 * 0.75f);
	m->size = 0;
	m->table = malloc(m->len * sizeof(int32_t));
	assert(m->table);
	for(uint32_t i = 0; i < m->len; i++) m->table[i] = -1;
}
static void f4_j6_resize(struct f4_j6* m) {
	uint32_t nl = m->len * 2;
	int32_t* nt = malloc(nl * sizeof(int32_t));
	assert(nt);
	for(uint32_t i = 0; i < nl; i++) nt[i] = -1;
	for(uint32_t j = 0; j < m->len; j++) {
		int32_t e = m->table[j];
		while(e >= 0) {
			int32_t nx = m->next[e];
			uint32_t i = (uint32_t)m->hash[e] & (nl - 1);
			m->next[e] = nt[i];
			nt[i] = e;
			e = nx;
		}
	}
	free(m->table);
	m->table = nt;
	m->len = nl;
	m->threshold = (uint32_t)((float)nl * 0.75f);
}
static void f4_j6_add(struct f4_j6* m, int x, int y, int z) {
	int32_t hc = (int32_t)((uint32_t)x * 8976890u + (uint32_t)y * 981131u
						   + (uint32_t)z);
	int32_t h = f4_j6_hash(hc);
	uint32_t i = (uint32_t)h & (m->len - 1);
	for(int32_t e = m->table[i]; e >= 0; e = m->next[e])
		if(m->hash[e] == h && m->key[e][0] == x && m->key[e][1] == y
		   && m->key[e][2] == z)
			return;
	assert(m->size < F4_J6_MAX);
	int32_t n = (int32_t)m->size;
	m->key[n][0] = x; m->key[n][1] = y; m->key[n][2] = z;
	m->hash[n] = h;
	m->next[n] = m->table[i];
	m->table[i] = n;
	if(m->size++ >= m->threshold)
		f4_j6_resize(m);
}
/* ArrayList.addAll(set) then the Explosion loops `for(i = size-1; i >= 0; --i)`. */
static size_t f4_j6_visit_order(const struct f4_j6* m, int (*out)[3]) {
	size_t n = 0;
	for(uint32_t j = 0; j < m->len; j++)
		for(int32_t e = m->table[j]; e >= 0; e = m->next[e]) {
			memcpy(out[m->size - 1 - n], m->key[e], sizeof(out[0]));
			n++;
		}
	assert(n == m->size);
	return n;
}
static struct f4_j6 g_f4_j6;
static int g_f4_order[F4_J6_MAX][3];
static int g_f4_prod[F4_J6_MAX][3];

static float f4_res(uint8_t id) {
	switch(id) {
	case BLOCK_AIR: return 0.0F;
	case BLOCK_TNT: return 0.0F;             /* setHardness(0.0F) */
	case BLOCK_DIRT: return (0.5F * 5.0F) / 5.0F;
	case BLOCK_STONE: return (10.0F * 3.0F) / 5.0F;
	default: assert(!"f4 fixture id"); return 0.0F;
	}
}
/* Explosion.doExplosion:39-75 transcribed against the LIVE pre-blast world,
 * every destroyedBlockPositions.add() fed to the OpenJDK 6 model. */
static void f4_transcribe(struct server_local* s, const vec3 cf, float power,
						  struct java_random* rng) {
	f4_j6_init(&g_f4_j6);
	const double ex = cf[0], ey = cf[1], ez = cf[2];
	for(int a = 0; a < 16; a++)
		for(int b2 = 0; b2 < 16; b2++)
			for(int c2 = 0; c2 < 16; c2++) {
				if(!(a == 0 || a == 15 || b2 == 0 || b2 == 15 || c2 == 0 || c2 == 15))
					continue;
				double v6 = (double)((float)a / (16.0F - 1.0F) * 2.0F - 1.0F);
				double v8 = (double)((float)b2 / (16.0F - 1.0F) * 2.0F - 1.0F);
				double v10 = (double)((float)c2 / (16.0F - 1.0F) * 2.0F - 1.0F);
				double v12 = sqrt(v6 * v6 + v8 * v8 + v10 * v10);
				v6 /= v12; v8 /= v12; v10 /= v12;
				float v14 = power * (0.7F + jrand_next_float(rng) * 0.6F);
				double x = ex, y = ey, z = ez;
				for(float v21 = 0.3F; v14 > 0.0F; v14 -= v21 * (12.0F / 16.0F)) {
					int ix = (int)floor(x), iy = (int)floor(y), iz = (int)floor(z);
					struct block_data bd = {0};
					uint8_t id = server_world_get_block(&s->worlds[0], ix, iy, iz, &bd)
						? bd.type : 0;
					if(id > 0)
						v14 -= (f4_res(id) + 0.3F) * v21;
					if(v14 > 0.0F)
						f4_j6_add(&g_f4_j6, ix, iy, iz);
					x += v6 * (double)v21; y += v8 * (double)v21; z += v10 * (double)v21;
				}
			}
}

struct f4_sound { uint16_t id; float pitch; };
static struct f4_sound g_f4_snd[256];
static size_t g_f4_snd_n;
static void f4_sound_observer(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SOUND && g_f4_snd_n < 256) {
		g_f4_snd[g_f4_snd_n].id = call->payload.sound.sound_id;
		g_f4_snd[g_f4_snd_n].pitch = call->payload.sound.pitch;
		g_f4_snd_n++;
	}
}
static bool f4_find_sound(uint16_t id, float* pitch) {
	for(size_t i = 0; i < g_f4_snd_n; i++)
		if(g_f4_snd[i].id == id) {
			*pitch = g_f4_snd[i].pitch;
			return true;
		}
	return false;
}

/* G9: live blasts queue their light; settle it so later fixture writes are
 * not refused by a pending-light reservation (no RNG is involved). */
static void f4_settle(struct server_world* w) {
	server_world_light_drain(w);
	if(w->light_fault)
		printf("  F4 FAIL: light fault %d after a blast\n", (int)w->light_fault_reason);
	assert(!w->light_fault);
}
static int f4_fails;
#define F4_CHECK(cond, ...) do { if(!(cond)) { printf("  F4 FAIL: " __VA_ARGS__); \
	printf("\n"); fflush(stdout); f4_fails++; } } while(0)

/* Every server-side entity created since `min_id`, by id (= creation order). */
static size_t f4_new_entities(struct server_local* s, uint32_t min_id,
							  struct entity** out, size_t cap) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->id >= min_id && n < cap)
			out[n++] = e;
	}
	for(size_t i = 1; i < n; i++)
		for(size_t j = i; j > 0 && out[j - 1]->id > out[j]->id; j--) {
			struct entity* t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
		}
	return n;
}


/* ---- F4.5-F4.7 (ASTRA-RNG2-RULING 2/6/7/8) ------------------------------
 * Shared Beta drop model (Block.java:294-326; BlockCrops.java:85-108; the six
 * random quantity/id overrides), driven by the JVM-anchored java_random. */
struct f4_drop { float pos[3]; uint16_t id; };
static struct f4_drop g_f4_want[512];
static size_t g_f4_want_n;
static void f4_want_pos(struct java_random* r, int x, int y, int z, uint16_t id,
						bool crop) {
	assert(g_f4_want_n < 512);
	struct f4_drop* d = &g_f4_want[g_f4_want_n++];
	const int at[3] = {x, y, z};
	d->id = id;
	for(int k = 0; k < 3; k++) {
		float f = jrand_next_float(r);
		if(crop)
			d->pos[k] = (float)at[k] + (f * 0.7F + (1.0F - 0.7F) * 0.5F);
		else
			d->pos[k] = (float)((double)at[k] + (double)(f * 0.7F)
								+ (double)(1.0F - 0.7F) * 0.5);
	}
}
/* dropBlockAsItemWithChance(world, x,y,z, meta, chance) for the fixture ids */
static void f4_model_drop(struct java_random* r, uint8_t id, uint8_t meta,
						  int x, int y, int z, float chance) {
	int q = 1;
	switch(id) {
	case BLOCK_CROPS: {
		if(jrand_next_float(r) <= chance && meta == 7)
			f4_want_pos(r, x, y, z, ITEM_WHEAT, false);
		for(int i = 0; i < 3; i++)
			if(jrand_next_int_bound(r, 15) <= meta)
				f4_want_pos(r, x, y, z, ITEM_SEED, true);
		return;
	}
	case BLOCK_LAPIS_ORE: q = 4 + jrand_next_int_bound(r, 5); break;
	case 73: q = 4 + jrand_next_int_bound(r, 2); break; /* redstone ore */
	case BLOCK_GLOWSTONE: q = 2 + jrand_next_int_bound(r, 3); break;
	case BLOCK_LEAVES: q = jrand_next_int_bound(r, 20) == 0 ? 1 : 0; break;
	case BLOCK_TNT: q = 0; break;
	default: q = 1; break;
	}
	for(int u = 0; u < q; u++) {
		if(!(jrand_next_float(r) <= chance))
			continue;
		uint16_t did = id;
		if(id == BLOCK_GRAVEL)
			did = jrand_next_int_bound(r, 10) == 0 ? ITEM_FLINT : BLOCK_GRAVEL;
		else if(id == BLOCK_TALL_GRASS)
			did = jrand_next_int_bound(r, 8) == 0 ? ITEM_SEED : 0;
		else if(id == BLOCK_LAPIS_ORE)
			did = ITEM_DYE;
		else if(id == 73)
			did = ITEM_REDSTONE;
		else if(id == BLOCK_GLOWSTONE)
			did = ITEM_GLOWSTONE_DUST;
		else if(id == BLOCK_LEAVES)
			did = BLOCK_SAPLING;
		if(did > 0)
			f4_want_pos(r, x, y, z, did, false);
	}
}
/* Compare every ENTITY_ITEM created since `first` with g_f4_want, in order. */
static bool f4_items_match(struct server_local* s, uint32_t first,
						   size_t* got_n, size_t* exact) {
	static struct entity* made[8192];
	size_t nm = f4_new_entities(s, first, made, 8192);
	size_t n = 0, ok = 0;
	for(size_t i = 0; i < nm; i++) {
		if(made[i]->type != ENTITY_ITEM)
			continue;
		if(n < g_f4_want_n && made[i]->pos[0] == g_f4_want[n].pos[0]
		   && made[i]->pos[1] == g_f4_want[n].pos[1]
		   && made[i]->pos[2] == g_f4_want[n].pos[2]
		   && made[i]->data.item.item.id == g_f4_want[n].id)
			ok++;
		n++;
	}
	*got_n = n;
	*exact = ok;
	return n == g_f4_want_n && ok == g_f4_want_n;
}

struct r2_drop_item { uint16_t id, meta; uint32_t pos[3]; };
struct r2_drop_case {
	int64_t seed;
	uint8_t stored_id, stored_meta;
	bool moving;
	bool blast;
	float chance;
	uint64_t final_state;
	size_t count;
	struct r2_drop_item items[8];
};
/* JVM 21.0.7+6-LTS; owned Beta server Block/BlockDoor/BlockPistonMoving/BlockCrops */
/* JVM 21.0.7+6-LTS; owned Beta server Block/BlockDoor/BlockPistonMoving/BlockCrops */
static const struct r2_drop_case r2_cases[] = {
{INT64_C(0x22f4a000), 64, 8, false, false, 0.0F, UINT64_C(0x369e9817ea74), 0, {}},
{INT64_C(0x22f4a001), 64, 8, false, false, 0.3F, UINT64_C(0x3698b92b0407), 0, {}},
{INT64_C(0x22f4a002), 64, 8, false, false, 1.0F, UINT64_C(0x36aa55f1b74e), 0, {}},
{INT64_C(0x22f4a003), 71, 8, false, false, 0.0F, UINT64_C(0x36a47704d0e1), 0, {}},
{INT64_C(0x22f4a004), 71, 8, false, false, 0.3F, UINT64_C(0x36871c6450c0), 0, {}},
{INT64_C(0x22f4a005), 71, 8, false, false, 1.0F, UINT64_C(0x36813d776a53), 0, {}},
{INT64_C(0x22f4a006), 64, 0, false, false, 0.0F, UINT64_C(0x3692da3e1d9a), 0, {}},
{INT64_C(0x22f4a007), 64, 0, false, false, 0.3F, UINT64_C(0x5a6093bad00e), 1, {{324, 0, {0x4220d2c5u, 0x414d9707u, 0x422196a8u}}}},
{INT64_C(0x22f4a008), 64, 0, false, false, 1.0F, UINT64_C(0x5bc73fdcf4b9), 1, {{324, 0, {0x4221ca2eu, 0x414be26au, 0x42219a94u}}}},
{INT64_C(0x22f4a009), 71, 0, false, false, 0.0F, UINT64_C(0x3669c1c3d09f), 0, {}},
{INT64_C(0x22f4a00a), 71, 0, false, false, 0.3F, UINT64_C(0xc19e2e35b2db), 1, {{330, 0, {0x4223154cu, 0x4148162eu, 0x4222b7bbu}}}},
{INT64_C(0x22f4a00b), 71, 0, false, false, 1.0F, UINT64_C(0x8eb2b70953ca), 1, {{330, 0, {0x42210957u, 0x4149fc4cu, 0x42222928u}}}},
{INT64_C(0x22f4a00c), 13, 0, true, false, 0.0F, UINT64_C(0xa2b9c99767dc), 1, {{13, 0, {0x422211ecu, 0x4148b450u, 0x4222613bu}}}},
{INT64_C(0x22f4a00d), 13, 0, true, false, 0.3F, UINT64_C(0x34b8cc9da79f), 1, {{13, 0, {0x42228b73u, 0x41467a02u, 0x42212d39u}}}},
{INT64_C(0x22f4a00e), 13, 0, true, false, 1.0F, UINT64_C(0x7ebbc38ae856), 1, {{13, 0, {0x42211eddu, 0x414d28eau, 0x4221fc74u}}}},
{INT64_C(0x22f4a00f), 59, 7, true, false, 0.0F, UINT64_C(0x8029291ff072), 4, {{296, 0, {0x42213fe8u, 0x41466191u, 0x4222bba7u}}, {295, 0, {0x4222679bu, 0x414d2287u, 0x4221739bu}}, {295, 0, {0x42212999u, 0x414731c4u, 0x422251c9u}}, {295, 0, {0x4222fe5fu, 0x4149c239u, 0x42220073u}}}},
{INT64_C(0x22f4a010), 59, 7, true, false, 0.3F, UINT64_C(0x1cc3233d3927), 2, {{296, 0, {0x42234f91u, 0x414b1c64u, 0x4220f864u}}, {295, 0, {0x4222f17fu, 0x4149f29bu, 0x42231d77u}}}},
{INT64_C(0x22f4a011), 59, 7, true, false, 1.0F, UINT64_C(0x2a72be76f2f3), 3, {{296, 0, {0x4221439cu, 0x414d0282u, 0x4223369du}}, {295, 0, {0x4222aa69u, 0x414955bdu, 0x42213442u}}, {295, 0, {0x42233629u, 0x4144cf3cu, 0x42211074u}}}},
{INT64_C(0x22f4a012), 59, 3, true, false, 0.0F, UINT64_C(0x87b15a5e9c73), 0, {}},
{INT64_C(0x22f4a013), 59, 3, true, false, 0.3F, UINT64_C(0xdfc2da6c787), 1, {{295, 0, {0x422186f7u, 0x41437c95u, 0x422164b7u}}}},
{INT64_C(0x22f4a014), 59, 3, true, false, 1.0F, UINT64_C(0xf74db38ba06e), 1, {{295, 0, {0x4221e06au, 0x41462b8du, 0x4223417eu}}}},
{INT64_C(0x22f4a015), 21, 0, true, false, 0.0F, UINT64_C(0x3cc37c63af1b), 6, {{351, 4, {0x422259f2u, 0x4143f13fu, 0x42220d7bu}}, {351, 4, {0x4222b1fdu, 0x4146b11eu, 0x4222b2b3u}}, {351, 4, {0x42225919u, 0x414b8718u, 0x4221f193u}}, {351, 4, {0x42226903u, 0x414cd9b0u, 0x42219a1bu}}, {351, 4, {0x42225477u, 0x41429dd1u, 0x422206b7u}}, {351, 4, {0x4222d485u, 0x4149e8b0u, 0x422143bdu}}}},
{INT64_C(0x22f4a016), 21, 0, true, false, 0.3F, UINT64_C(0x24889265db1e), 5, {{351, 4, {0x4220ed5bu, 0x414aa027u, 0x4222dcb7u}}, {351, 4, {0x4221b960u, 0x414ae2a6u, 0x42226366u}}, {351, 4, {0x42224bb2u, 0x4143b066u, 0x4222af23u}}, {351, 4, {0x4222c9bbu, 0x4144db33u, 0x422107dau}}, {351, 4, {0x42225d6du, 0x41435cf7u, 0x4220ffe5u}}}},
{INT64_C(0x22f4a017), 21, 0, true, false, 1.0F, UINT64_C(0x43d267d791a1), 5, {{351, 4, {0x422166e3u, 0x414865dau, 0x4221a8b4u}}, {351, 4, {0x42211d50u, 0x4145c113u, 0x42209ff7u}}, {351, 4, {0x4221613bu, 0x41429191u, 0x42218104u}}, {351, 4, {0x4222a97du, 0x414b4119u, 0x4221389bu}}, {351, 4, {0x42225a70u, 0x4146d8fbu, 0x42215780u}}}},
{INT64_C(0x22f4a018), 64, 8, true, false, 0.0F, UINT64_C(0x36cd8f7f1ddc), 0, {}},
{INT64_C(0x22f4a019), 64, 8, true, false, 0.3F, UINT64_C(0x36c7b092376f), 0, {}},
{INT64_C(0x22f4a01a), 64, 8, true, false, 1.0F, UINT64_C(0x36d94d58eab6), 0, {}},
{INT64_C(0x22f4a01b), 13, 0, false, false, 0.0F, UINT64_C(0x36d36e6c0449), 0, {}},
{INT64_C(0x22f4a01c), 13, 0, false, false, 0.3F, UINT64_C(0x82c999336bac), 1, {{13, 0, {0x4222dfdau, 0x414abf89u, 0x422207ceu}}}},
{INT64_C(0x22f4a01d), 13, 0, false, false, 1.0F, UINT64_C(0x14c89c39ab6f), 1, {{13, 0, {0x42235961u, 0x4148853cu, 0x4220d3cbu}}}},
{INT64_C(0x0), 13, 0, true, false, 1.0F, UINT64_C(0xa32dc9f68ab8), 1, {{13, 0, {0x42214604u, 0x414930eau, 0x42226280u}}}},
{INT64_C(0x1), 13, 0, true, false, 1.0F, UINT64_C(0x352cccfcca7b), 1, {{13, 0, {0x4221bf8cu, 0x4146f69cu, 0x42212e7du}}}},
{INT64_C(0x2), 13, 0, true, false, 1.0F, UINT64_C(0x7f2fc3ea0b32), 1, {{13, 0, {0x42231fc2u, 0x41427251u, 0x4221fdb9u}}}},
{INT64_C(0x3), 13, 0, true, false, 1.0F, UINT64_C(0x112ec6f04af5), 1, {{318, 0, {0x4220cc7du, 0x414b6b37u, 0x4220c9b6u}}}},
{INT64_C(0x4), 13, 0, true, false, 1.0F, UINT64_C(0xeb29d60f89c4), 1, {{13, 0, {0x42232c22u, 0x414b7ae8u, 0x42232c0fu}}}},
{INT64_C(0x5), 13, 0, true, false, 1.0F, UINT64_C(0x7d28d915c987), 1, {{13, 0, {0x4220d8ddu, 0x4149409bu, 0x4221f80cu}}}},
{INT64_C(0x6), 13, 0, true, false, 1.0F, UINT64_C(0xc72bd0030a3e), 1, {{13, 0, {0x42223913u, 0x4144bc4fu, 0x4222c747u}}}},
{INT64_C(0x7), 13, 0, true, false, 1.0F, UINT64_C(0x592ad3094a01), 1, {{13, 0, {0x4222b29bu, 0x41428202u, 0x42219345u}}}},
{INT64_C(0x8), 13, 0, true, false, 1.0F, UINT64_C(0x3325e22888d0), 1, {{13, 0, {0x42224574u, 0x414291b3u, 0x422128d0u}}}},
{INT64_C(0x9), 13, 0, true, false, 1.0F, UINT64_C(0xc524e52ec893), 1, {{13, 0, {0x4222befbu, 0x414b8a99u, 0x4222c19bu}}}},
{INT64_C(0xa), 13, 0, true, false, 1.0F, UINT64_C(0xf27dc1c094a), 1, {{318, 0, {0x42215265u, 0x4147064eu, 0x4220c409u}}}},
{INT64_C(0xb), 13, 0, true, false, 1.0F, UINT64_C(0xa126df22490d), 1, {{13, 0, {0x4221cbecu, 0x4144cc00u, 0x42225cd3u}}}},
{INT64_C(0xc), 13, 0, true, false, 1.0F, UINT64_C(0x7b21ee4187dc), 1, {{13, 0, {0x42215ec5u, 0x4144dbb2u, 0x4221f25fu}}}},
{INT64_C(0xd), 13, 0, true, false, 1.0F, UINT64_C(0xd20f147c79f), 1, {{318, 0, {0x4221d84du, 0x4142a164u, 0x4220be5cu}}}},
{INT64_C(0xe), 13, 0, true, false, 1.0F, UINT64_C(0x5723e8350856), 1, {{13, 0, {0x42233883u, 0x4149504cu, 0x42218d98u}}}},
{INT64_C(0xf), 13, 0, true, false, 1.0F, UINT64_C(0xe922eb3b4819), 1, {{13, 0, {0x4220e53du, 0x414715ffu, 0x42232662u}}}},
{INT64_C(0x10), 13, 0, true, false, 1.0F, UINT64_C(0x833d99928e88), 1, {{13, 0, {0x422213f2u, 0x414b3c24u, 0x42220913u}}}},
{INT64_C(0x11), 13, 0, true, false, 1.0F, UINT64_C(0x153c9c98ce4b), 1, {{318, 0, {0x42228d7au, 0x414901d6u, 0x4220d510u}}}},
{INT64_C(0x12), 13, 0, true, false, 1.0F, UINT64_C(0x5f3f93860f02), 1, {{13, 0, {0x422120e3u, 0x41447d8bu, 0x4221a44cu}}}},
{INT64_C(0x13), 13, 0, true, false, 1.0F, UINT64_C(0xf13e968c4ec5), 1, {{13, 0, {0x42219a6bu, 0x414d7671u, 0x42233d16u}}}},
{INT64_C(0x22f4b01e), 64, 8, false, true, 0.3F, UINT64_C(0x72b77b9a5f94), 0, {}},
{INT64_C(0x22f4b01f), 71, 8, false, true, 0.3F, UINT64_C(0xe2ad43af783f), 0, {}},
{INT64_C(0x22f4b020), 13, 0, true, true, 0.3F, UINT64_C(0x64439dcaf5a), 1, {{318, 0, {0x42224d60u, 0x4143427au, 0x4220ab25u}}}},
{INT64_C(0x22f4b021), 59, 7, true, true, 0.3F, UINT64_C(0x917751c738e0), 2, {{296, 0, {0x422190d1u, 0x414d76c4u, 0x4220c41bu}}, {295, 0, {0x422354d7u, 0x4146c9dcu, 0x4222ac9fu}}}},
};
static void f4rng_drop_cases(struct server_local* s,
	const struct r2_drop_case* cases, size_t case_count) {
	string_t lv;
	string_init_set_str(lv, TMP_WORLD);
	server_world_create(&s->worlds[1], lv, WORLD_DIM_NETHER, 4242, true);
	string_clear(lv);
	s->worlds[1].world_index = 1;
	s->worlds[1].session = s;
	piston_moving_store_init(&s->piston_moving);
	for(size_t k = 0; k < case_count; k++) {
		const struct r2_drop_case* c = &cases[k];
		struct server_world* w = &s->worlds[c->blast ? 0 : k % 2];
		struct server_world* other = &s->worlds[1 - w->world_index];
		struct block_data data = {.type = c->moving ? BLOCK_PISTON_MOVING : c->stored_id,
			.metadata = c->stored_meta};
		struct block_info info = {.x = 40, .y = 12, .z = 40, .block = &data};
		if(c->blast) {
			/* The full golden ran other scenes here. Establish the JVM's
			 * single-block air fixture before seeding; power 4 cannot reach
			 * beyond this cube or its unchanged superflat floor at y=3. */
			for(int x = 32; x <= 48; x++)
				for(int z = 32; z <= 48; z++)
					for(int y = 4; y <= 20; y++) {
						struct block_data old;
						assert(server_world_get_block(w, x, y, z, &old));
						if(old.type != BLOCK_AIR)
							assert(server_world_set_block(w, x, y, z,
								(struct block_data){.type=BLOCK_AIR}));
					}
			f4_settle(w);
			free_entities(s);
		}
		if(c->moving) {
			struct piston_moving_state state = {.stored_id = c->stored_id,
				.stored_meta = c->stored_meta, .facing = 1,
				.flags = PISTON_MOVING_FLAG_EXTENDING};
			assert(piston_moving_store_upsert_owned(&s->piston_moving,
				server_world_piston_owner(w), info.x, info.y, info.z, &state, NULL));
		}
		if(c->blast) {
			assert(server_world_set_block(w, info.x, info.y, info.z, data));
			f4_settle(w);
		}
		const struct java_random other_before = other->rand;
		const struct random_gen session_before = s->rand_src;
		jrand_set_seed(&w->rand, c->seed);
		/* A cached pair must survive these non-Gaussian drop draws. */
		w->rand.have_next_gaussian = true;
		w->rand.next_gaussian_cached = 0.125;
		uint32_t first = s->next_entity_id;
		if(c->blast) {
			server_world_begin_update(w);
			server_world_explode(w, (vec3){40.5F, 12.5F, 40.5F}, POWER);
			f4_settle(w);
			struct block_data now;
			assert(server_world_get_block(w, info.x, info.y, info.z, &now));
			F4_CHECK(now.type == BLOCK_AIR, "F4.R2 center blast did not remove its block");
		} else {
			/* Explicit world is authoritative even when the callback scope names
			 * slot 0 and both worlds hold a moving TE at these coordinates. */
			server_local_spawn_block_drops_with_chance(w, &info, c->chance);
		}
		static struct entity* made[32];
		/* Never silently truncate an unexpected fixture/callback spill. */
		assert(dict_entity_size(s->entities) <= 32);
		size_t n = f4_new_entities(s, first, made, 32);
		bool match = n == c->count;
		for(size_t i = 0; i < n && i < c->count; i++) {
			struct entity* e = made[i];
			match &= e->type == ENTITY_ITEM && e->data.item.item.id == c->items[i].id
				&& e->data.item.item.durability == c->items[i].meta;
			for(int a = 0; a < 3; a++) match &= f4_fbits(e->pos[a]) == c->items[i].pos[a];
		}
		printf("  (F4.R2) case %zu %s id=%u meta=%u chance=%.1f items=%zu/%zu state=%012llx/%012llx\n",
			k, c->moving ? "moving" : "direct", c->stored_id, c->stored_meta,
			(double)c->chance, n, c->count, (unsigned long long)w->rand.seed,
			(unsigned long long)c->final_state);
		F4_CHECK(match && w->rand.seed == c->final_state,
			"F4.R2 case %zu: JVM/decompile item/order/position/state mismatch", k);
		F4_CHECK(w->rand.have_next_gaussian && w->rand.next_gaussian_cached == 0.125
			&& !memcmp(&other_before, &other->rand, sizeof other_before)
			&& !memcmp(&session_before, &s->rand_src, sizeof session_before),
			"F4.R2 case %zu: wrong stream/cache", k);
		free_entities(s);
	}
	/* Orphaned moving blocks delegate nothing, unlike a top door's id=0. */
	struct block_data orphan = {.type = BLOCK_PISTON_MOVING};
	struct block_info info = {.x = 41, .y = 12, .z = 40, .block = &orphan};
	struct java_random before = s->worlds[0].rand;
	server_local_spawn_block_drops_with_chance(&s->worlds[0], &info, 1.0F);
	F4_CHECK(!memcmp(&before, &s->worlds[0].rand, sizeof before), "F4.R2 orphan consumed a draw");
	piston_moving_store_destroy(&s->piston_moving);
	server_world_destroy(&s->worlds[1]);
	assert(f4_fails == 0);
	puts("PASS F4.R2: upper doors and complete stored-block drops, interleaved worlds and cache");
}


static void f4rng_r2_drops(struct server_local* s) {
	f4rng_drop_cases(s, r2_cases, sizeof r2_cases / sizeof r2_cases[0]);
}
struct r3_air_case {
	const char* name;
	double center[3];
	float power;
	int64_t seed;
	const int (*visits)[3];
	size_t count;
};
/* RetroMCP minecraft/src_original/net/minecraft/src/BlockBed.java:45:
 * newExplosion(..., (double)((float)var2 + 0.5F),
 * (double)((float)var3 + 0.5F), (double)((float)var4 + 0.5F), 5.0F, true).
 * Compare the real bed activation's full ray-cell order with a direct blast
 * at those independently specified centers, including float rounding before
 * and after the addition. The blast is in air after the head is removed. */
static void bed_float_center_run(struct server_local* s) {
	static const struct {
		int x, z;
		double cx, cz;
	} cases[] = {
		{40, 40, 40.5, 40.5},
		{8388616, 8388616, 8388616.0, 8388616.0},
		{-8388616, -8388616, -8388616.0, -8388616.0},
		{25000009, -25000009, 25000008.0, -25000008.0},
	};
	struct server_world* w = &s->worlds[0];
	const struct server_player saved_player = s->player;
	const int saved_dimension = w->dimension;
	for(size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		const int x = cases[k].x, z = cases[k].z;
		struct server_chunk* sc = NULL;
		if(!server_world_is_chunk_loaded(w, x >> 4, (z - 1) >> 4))
			assert(server_world_load_chunk(w, x >> 4, (z - 1) >> 4, &sc) && sc);
		struct block_data head = {.type = BLOCK_BED, .metadata = 8};
		assert(server_world_set_block(w, x, 60, z - 1, head));
		struct block_info on = {.block = &head, .x = x, .y = 60, .z = z - 1};
		w->dimension = WORLD_DIM_NETHER;
		s->player.x = x;
		s->player.y = 60.0 + PLAYER_EYE_HEIGHT;
		s->player.z = z - 1;
		s->player.health = 16000; /* survive the close blast without respawning */
		s->player.sleeping = false;
		jrand_set_seed(&w->rand, 424242);
		blocks[BLOCK_BED]->onRightClick(s, &s->player, NULL, NULL, &on, SIDE_TOP);
		struct block_data after;
		assert(server_world_get_block(w, x, 60, z - 1, &after)
			   && after.type == BLOCK_AIR);
		size_t n = server_world_test_last_blast_order(NULL, 0);
		assert(n > 0);
		int (*got)[3] = malloc(n * sizeof *got);
		assert(got);
		assert(server_world_test_last_blast_order(got, n) == n);
		s->player.x = 200.5;
		s->player.z = 200.5;
		jrand_set_seed(&w->rand, 424242);
		server_world_explode_flaming_precise(w,
			(double[3]) {cases[k].cx, 60.5, cases[k].cz}, 5.0F);
		size_t want_n = server_world_test_last_blast_order(NULL, 0);
		int (*want)[3] = malloc(want_n * sizeof *want);
		assert(want);
		assert(server_world_test_last_blast_order(want, want_n) == want_n);
		bool exact = n == want_n && !memcmp(got, want, n * sizeof *got);
		printf("bed float center (%d, %d): visits=%zu/%zu %s\n",
			x, z, n, want_n, exact ? "PASS" : "FAIL");
		fflush(stdout);
		free(got);
		free(want);
		assert(exact && "BlockBed.java:45 requires float addition before double promotion");
		w->dimension = saved_dimension;
	}
	s->player = saved_player;
	puts("PASS bed float centers: ordinary, +/-2^23, +/-25 million");
}
#include "f4rng_r3_witness.h"
static void f4rng_r3_support(struct server_local* s) {
	struct server_world* w = &s->worlds[0];
	const struct r2_drop_case* c = NULL;
	for(size_t k=0;k<sizeof r3_cases/sizeof r3_cases[0];k++)
		if(r3_cases[k].stored_id==BLOCK_DEADBUSH && !r3_cases[k].moving && !r3_cases[k].blast && r3_cases[k].chance==1.0F) c=&r3_cases[k];
	assert(c);
	assert(server_world_set_block(w,40,11,40,(struct block_data){.type=BLOCK_SAND}));
	assert(server_world_set_block(w,40,12,40,(struct block_data){.type=BLOCK_DEADBUSH}));
	f4_settle(w);
	jrand_set_seed(&w->rand,c->seed);
	w->rand.have_next_gaussian=true;w->rand.next_gaussian_cached=0.125;
	const struct random_gen before=s->rand_src;
	uint32_t first=s->next_entity_id;
	assert(server_world_set_block(w,40,11,40,(struct block_data){.type=BLOCK_AIR}));
	struct block_data after;
	assert(server_world_get_block(w,40,12,40,&after) && after.type==BLOCK_AIR);
	struct entity* made[8];
	size_t n=f4_new_entities(s,first,made,8);
	printf("(F4.R3 support) dead bush items=%zu state=%012llx/%012llx\n",n,(unsigned long long)w->rand.seed,(unsigned long long)c->final_state);
	F4_CHECK(n==0 && w->rand.seed==c->final_state && w->rand.have_next_gaussian && w->rand.next_gaussian_cached==0.125 && !memcmp(&before,&s->rand_src,sizeof before),"F4.R3 synchronous dead-bush support loss omitted chance/wrong stream");
	assert(f4_fails==0);
	puts("PASS F4.R3 native support-loss cascade: zero items, one World.rand chance");
}

static void f4rng_r3_harvest(struct server_local* s) {
	struct server_world* w = &s->worlds[0];
	s->player.x = 40.5; s->player.y = 13.62; s->player.z = 40.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.inventory.items[INVENTORY_SLOT_HOTBAR] = (struct item_data){.id=ITEM_IRON_SHOVEL,.count=1};
	assert(server_world_set_block(w,40,11,40,(struct block_data){.type=BLOCK_STONE}));
	for(size_t k=0;k<sizeof r3_harvest/sizeof r3_harvest[0];k++) {
		const struct r2_drop_case* c = &r3_harvest[k];
		assert(server_world_set_block(w,40,12,40,(struct block_data){.type=c->stored_id}));
		f4_settle(w);
		jrand_set_seed(&w->rand,c->seed);
		w->rand.have_next_gaussian = true;w->rand.next_gaussian_cached = 0.125;
		uint32_t first = s->next_entity_id;
		struct server_rpc dig = {.type=SRPC_BLOCK_DIG,.payload.block_dig={.finished=true,.x=40,.y=12,.z=40,.side=SIDE_TOP}};
		server_local_test_process_rpc(s,&dig);
		struct block_data after;
		assert(server_world_get_block(w,40,12,40,&after) && after.type==BLOCK_AIR);
		struct entity* made[8];
		assert(dict_entity_size(s->entities)<=8);
		size_t n=f4_new_entities(s,first,made,8);
		bool exact=n==1 && made[0]->type==ENTITY_ITEM && made[0]->data.item.item.id==c->items[0].id;
		for(int a=0;a<3 && n==1;a++) exact &= f4_fbits(made[0]->pos[a])==c->items[0].pos[a];
		printf("(F4.R3 native harvest) id=%u items=%zu/1 state=%012llx/%012llx\n",c->stored_id,n,(unsigned long long)w->rand.seed,(unsigned long long)c->final_state);
		F4_CHECK(exact && w->rand.seed==c->final_state && w->rand.have_next_gaussian && w->rand.next_gaussian_cached==0.125,"F4.R3 direct harvest added chance or lost item");
		free_entities(s);
	}
	assert(f4_fails==0);
	puts("PASS F4.R3 native snow/TNT harvest override: JVM positions/state/cache");
}

static void f4rng_r3_air(struct server_local* s) {
	struct server_world* w = &s->worlds[0];
	for(size_t k = 0; k < sizeof r3_air / sizeof r3_air[0]; k++) {
		const struct r3_air_case* a = &r3_air[k];
		jrand_set_seed(&w->rand, a->seed);
		w->rand.have_next_gaussian = true;
		w->rand.next_gaussian_cached = 0.125;
		struct java_random expected = w->rand;
		for(int i = 0; i < 1354; i++) (void)jrand_next_float(&expected);
		assert(expected.seed == r3_air_states[k]); /* independent JVM final-state pin */
		const struct random_gen old = s->rand_src;
		server_world_explode_precise(w, a->center, a->power);
		size_t n = server_world_test_last_blast_order(NULL, 0);
		int (*visits)[3] = malloc(n * sizeof *visits);
		assert(visits);
		assert(server_world_test_last_blast_order(visits, n) == n);
		size_t first = 0;
		while(first < n && first < a->count && !memcmp(visits[first], a->visits[first], sizeof *visits)) first++;
		printf("(F4.R3 air) %s visits=%zu/%zu first_difference=%zu state=%012llx\n",
			a->name,n,a->count,first,(unsigned long long)w->rand.seed);
		F4_CHECK(n == a->count && first == n, "F4.R3 %s full reverse visits differ at %zu",a->name,first);
		F4_CHECK(!memcmp(&expected,&w->rand,sizeof expected) && !memcmp(&old,&s->rand_src,sizeof old),
			"F4.R3 air state/cache/wrong stream");
		free(visits);
	}
	assert(f4_fails == 0);
	puts("PASS F4.R3 precise center: all four v8 full air visit lists");
}
static void f4rng_r3_death(struct server_local* s) {
	struct death_drop_journal* j = death_drop_journal_alloc();
	assert(j);
	for(int close = 0; close < 2; close++) {
		memset(s->player.inventory.items,0,INVENTORY_SIZE * sizeof(struct item_data));
		s->player.inventory.picked_item = (struct item_data){0};
		size_t slot = close ? INVENTORY_SLOT_CRAFTING : INVENTORY_SLOT_HOTBAR;
		s->player.inventory.items[slot] = (struct item_data){.id=BLOCK_DIRT,.count=1};
		s->player.entity_rand = (struct server_body_rand){0};
		// Poison retired state/cache: lazy construction must replace both before the first drop.
		s->player.entity_rand.r.seed = UINT64_C(0x1234);
		s->player.entity_rand.r.have_next_gaussian = true;
		s->player.entity_rand.r.next_gaussian_cached = 0.125;
		server_world_test_seed_runtime_jrand(&s->player.entity_rand.r, INT64_C(0x5EED0B0D));
		j->count = 0;
		assert(server_local_test_plan_death_drops(s,j));
		F4_CHECK(s->player.entity_rand.seeded && !s->player.entity_rand.r.have_next_gaussian,
			"F4.R3 death %s bypassed lazy body seed",close ? "close" : "scatter");
		F4_CHECK(j->count == 1,"F4.R3 death record count");
		F4_CHECK(s->player.entity_rand.r.seed == r3_body_states[close],
			"F4.R3 death consumed retired state or lazy-seeded after its drop draws");
		server_world_test_seed_runtime_jrand(NULL, 0);
	}
	memset(s->player.inventory.items,0,INVENTORY_SIZE * sizeof(struct item_data));
	death_drop_journal_free(j);
	assert(f4_fails == 0);
	puts("PASS F4.R3 both death-drop paths lazily seed a fresh body and clear its cache");
}

static void f4rng_more(struct server_local* s) {
	struct server_world* w = &s->worlds[0];

	/* ---- F4.5 harvest -> air blast -> harvest: one World.rand ------------ */
	{
		static const struct { uint8_t id, meta; } kinds[] = {
			{BLOCK_LAPIS_ORE, 0}, {73, 0}, {BLOCK_GLOWSTONE, 0}, {BLOCK_GRAVEL, 0},
			{BLOCK_LEAVES, 0}, {BLOCK_TALL_GRASS, 1}, {BLOCK_CROPS, 7},
			{BLOCK_CROPS, 3}, {BLOCK_DIRT, 0},
		};
		const size_t nk = sizeof kinds / sizeof kinds[0];
		struct java_random ref;
		jrand_set_seed(&ref, JW_RAY_SEED ^ 0x55);
		g_f4_want_n = 0;
		for(int round = 0; round < 6; round++)
			for(size_t k = 0; k < nk; k++)
				f4_model_drop(&ref, kinds[k].id, kinds[k].meta, 120 + (int)k, 40,
							  130 + round, 1.0F);
		const size_t first_half = g_f4_want_n;
		/* an air blast between the two harvest halves: 1352 rays + 2 sound */
		const vec3 c = {125.5F, 90.5F, 125.5F};
		{
			struct java_random tmp = ref;
			f4_transcribe(s, c, POWER, &tmp);
			(void)jrand_next_float(&tmp);
			(void)jrand_next_float(&tmp);
			ref = tmp;
		}
		for(int round = 6; round < 12; round++)
			for(size_t k = 0; k < nk; k++)
				f4_model_drop(&ref, kinds[k].id, kinds[k].meta, 120 + (int)k, 40,
							  130 + round, 1.0F);
		jrand_set_seed(&w->rand, JW_RAY_SEED ^ 0x55);
		const struct random_gen session_before = s->rand_src;
		uint32_t first = s->next_entity_id;
		for(int round = 0; round < 12; round++) {
			if(round == 6) {
				server_world_begin_update(w);
				server_world_explode(w, (vec3) {c[0], c[1], c[2]}, POWER);
				f4_settle(w);
			}
			for(size_t k = 0; k < nk; k++) {
				struct block_data bd = {.type = kinds[k].id, .metadata = kinds[k].meta};
				struct block_info info = {.block = &bd, .x = 120 + (int)k, .y = 40,
										  .z = 130 + round};
				server_local_spawn_block_drops(s, &info);
			}
		}
		size_t got = 0, exact = 0;
		bool match = f4_items_match(s, first, &got, &exact);
		printf("  (F4.5) harvest x%zu kinds x12 rounds (+ air blast): items %zu/%zu "
			   "(first half %zu) exact %zu; World.rand %#llx (want %#llx); session "
			   "xorshift %s\n", nk, got, g_f4_want_n, first_half, exact,
			   (unsigned long long)w->rand.seed, (unsigned long long)ref.seed,
			   memcmp(&session_before, &s->rand_src, sizeof session_before) == 0
				   ? "untouched" : "ADVANCED");
		F4_CHECK(match, "F4.5 ordinary drops (6 random overrides + crops float "
				 "positions) not the world's World.rand in Beta order");
		F4_CHECK(w->rand.seed == ref.seed, "F4.5 World.rand continuity harvest->blast->harvest");
		F4_CHECK(memcmp(&session_before, &s->rand_src, sizeof session_before) == 0,
				 "F4.5 a drop advanced the session xorshift (wrong stream)");
		free_entities(s);
	}

	/* ---- F4.5b crops FLOAT seed offsets at a large coordinate ------------
	 * BlockCrops.java:89-93 adds the float offset to (float)x in FLOAT; the
	 * generic Block.dropBlockAsItem_do promotes to double first. At |x| ~
	 * 2^20 (float spacing 1/8) the two roundings differ, so this pins the
	 * crops-specific arithmetic, not just the draw order. */
	{
		const int FX = 1048577, FZ = -1048575;
		struct java_random ref;
		jrand_set_seed(&ref, JW_RAY_SEED ^ 0x5B);
		g_f4_want_n = 0;
		for(int k = 0; k < 16; k++)
			f4_model_drop(&ref, BLOCK_CROPS, 7, FX + k, 40, FZ, 1.0F);
		size_t crop_seeds = 0, differs = 0;
		for(size_t i = 0; i < g_f4_want_n; i++) {
			if(g_f4_want[i].id != ITEM_SEED)
				continue;
			crop_seeds++;
			/* would the generic double formula have landed elsewhere? */
			(void)differs;
		}
		jrand_set_seed(&w->rand, JW_RAY_SEED ^ 0x5B);
		uint32_t first = s->next_entity_id;
		for(int k = 0; k < 16; k++) {
			struct block_data bd = {.type = BLOCK_CROPS, .metadata = 7};
			struct block_info info = {.block = &bd, .x = FX + k, .y = 40, .z = FZ};
			server_local_spawn_block_drops(s, &info);
		}
		size_t got = 0, exact = 0;
		bool match = f4_items_match(s, first, &got, &exact);
		/* independent proof the fixture discriminates: replay with the
		 * double-promoted formula and count positions that would differ */
		struct java_random alt;
		jrand_set_seed(&alt, JW_RAY_SEED ^ 0x5B);
		size_t would_differ = 0;
		for(int k = 0; k < 16; k++) {
			(void)jrand_next_float(&alt); /* wheat chance */
			for(int a = 0; a < 3; a++) (void)jrand_next_float(&alt); /* wheat pos */
			for(int i = 0; i < 3; i++)
				if(jrand_next_int_bound(&alt, 15) <= 7) {
					const int at[3] = {FX + k, 40, FZ};
					for(int c = 0; c < 3; c++) {
						float f = jrand_next_float(&alt);
						float fl = (float)at[c] + (f * 0.7F + (1.0F - 0.7F) * 0.5F);
						float db = (float)((double)at[c] + (double)(f * 0.7F)
										   + (double)(1.0F - 0.7F) * 0.5);
						if(fl != db) { would_differ++; break; }
					}
				}
		}
		printf("  (F4.5b) far crops: %zu seeds, %zu would differ under the double "
			   "formula; items %zu/%zu exact %zu\n", crop_seeds, would_differ, got,
			   g_f4_want_n, exact);
		F4_CHECK(would_differ > 0, "F4.5b fixture does not discriminate float vs double");
		F4_CHECK(match && w->rand.seed == ref.seed,
				 "F4.5b crops seed positions not BlockCrops' float arithmetic");
		free_entities(s);
	}

	/* ---- F4.6 support-loss cascade interleaves with later cells ---------- */
	{
		const int X0 = 100, Z0 = 100, Y0 = 110;
		for(int x = X0; x < X0 + 7; x++)
			for(int z = Z0; z < Z0 + 7; z++)
				set_block(s, x, Y0, z, BLOCK_DIRT);
		/* torches standing on the slab (meta 5 = floor) */
		for(int x = X0; x < X0 + 7; x += 2)
			for(int z = Z0; z < Z0 + 7; z += 2)
				assert(server_world_set_block(w, x, Y0 + 1, z,
					(struct block_data) {.type = BLOCK_TORCH, .metadata = 5}));
		const vec3 c = {X0 + 3.5F, Y0 - 1.5F, Z0 + 3.5F};
		const float pw = 2.0F;
		struct java_random ref;
		jrand_set_seed(&ref, JW_RAY_SEED ^ 0x66);
		f4_transcribe(s, c, pw, &ref);
		size_t want_n = f4_j6_visit_order(&g_f4_j6, g_f4_order);
		(void)jrand_next_float(&ref);
		(void)jrand_next_float(&ref);
		/* live model: removed cells */
		static uint8_t gone[16][16][16];
		memset(gone, 0, sizeof gone);
		g_f4_want_n = 0;
		size_t cascades = 0;
		for(size_t i = 0; i < want_n; i++) {
			int x = g_f4_order[i][0], y = g_f4_order[i][1], z = g_f4_order[i][2];
			struct block_data bd;
			if(!server_world_get_block(w, x, y, z, &bd) || bd.type == 0)
				continue;
			if(x < X0 - 4 || x >= X0 + 12 || y < Y0 - 4 || y >= Y0 + 12
			   || z < Z0 - 4 || z >= Z0 + 12)
				continue;
			if(gone[x - X0 + 4][y - Y0 + 4][z - Z0 + 4])
				continue;
			f4_model_drop(&ref, bd.type, bd.metadata, x, y, z, 0.3F);
			gone[x - X0 + 4][y - Y0 + 4][z - Z0 + 4] = 1;
			struct block_data up;
			if(bd.type == BLOCK_DIRT
			   && server_world_get_block(w, x, y + 1, z, &up) && up.type == BLOCK_TORCH
			   && !gone[x - X0 + 4][y + 1 - Y0 + 4][z - Z0 + 4]) {
				/* BlockTorch.onNeighborBlockChange -> dropBlockAsItem (1.0F) */
				f4_model_drop(&ref, BLOCK_TORCH, 5, x, y + 1, z, 1.0F);
				gone[x - X0 + 4][y + 1 - Y0 + 4][z - Z0 + 4] = 1;
				cascades++;
			}
		}
		jrand_set_seed(&w->rand, JW_RAY_SEED ^ 0x66);
		uint32_t first = s->next_entity_id;
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {c[0], c[1], c[2]}, pw);
		f4_settle(w);
		size_t got = 0, exact = 0;
		bool match = f4_items_match(s, first, &got, &exact);
		printf("  (F4.6) dirt+torch blast: %zu support-loss cascades modelled; items "
			   "%zu/%zu exact %zu; World.rand %#llx (want %#llx) (order PENDING ORACLE "
			   "QUALIFICATION)\n", cascades, got, g_f4_want_n, exact,
			   (unsigned long long)w->rand.seed, (unsigned long long)ref.seed);
		F4_CHECK(cascades > 0, "F4.6 fixture produced no cascade");
		F4_CHECK(match && w->rand.seed == ref.seed,
				 "F4.6 cascade drops not interleaved per cell on World.rand");
		free_entities(s);
	}

	/* ---- F4.7 lethal fresh hit: dying-body death pitch (native lifetime in test_local_guest) ---- */
	{
		struct server_player* p = &s->player;
		const double px = 100.5, pz = 160.5;
		p->has_pos = true;
		p->x = px; p->y = 4.0 + PLAYER_EYE_HEIGHT; p->z = pz;
		p->health = 8; /* < the 88-unit blast below: lethal */
		p->hurt_resist_time = 0;
		p->last_damage = 0;
		server_local_test_seed_body_rand(s, p, JW_BODY_SEED);
		float f[6];
		for(int i = 0; i < 6; i++) memcpy(&f[i], &jw_body_FLOATS[i].bits, 4);
		g_f4_snd_n = 0;
		clin_test_set_rpc_observer(f4_sound_observer, NULL);
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {(float)(px - 6.0), (float)p->y, (float)pz}, POWER);
		f4_settle(w);
		float death = 0.0F;
		bool heard = f4_find_sound(SOUND_RANDOM_HURT, &death);
		const float wd = (f[0] - f[1]) * 0.2F + 1.0F;
		clin_test_set_rpc_observer(NULL, NULL);
		printf("  (F4.7) lethal: death pitch %.9g (want %.9g)\n", (double)death, (double)wd);
		F4_CHECK(heard && f4_fbits(death) == f4_fbits(wd),
				 "F4.7 lethal fresh hit: death pitch not the dying body's Entity.rand");
		/* Replacement-body lifetimes and exact cache retirement are exercised
		 * through native SRPC_RESPAWN/admission/retry handlers by
		 * test_local_guest's F4.R2 cases. Never reset the helper here. */
		p->x = 200.5; p->y = 4.5; p->z = 200.5;
		p->health = MAX_PLAYER_HEALTH;
		p->hurt_resist_time = 0;
		free_entities(s);
	}
}

static void f4rng_run(struct server_local* s) {
	setvbuf(stdout, NULL, _IONBF, 0); /* keep every F4 line if an assert aborts */
	f4_anchor();
	struct server_world* w = &s->worlds[0];
	clin_test_set_rpc_observer(f4_sound_observer, NULL);

	/* ---- F4.1 air blast: rays + doEffects(false) sound on World.rand ---- */
	{
		const vec3 c = {110.5F, 60.5F, 110.5F};
		struct java_random ref;
		jrand_set_seed(&ref, JW_RAY_SEED);
		f4_transcribe(s, c, POWER, &ref);
		size_t want_n = f4_j6_visit_order(&g_f4_j6, g_f4_order);
		float s1 = jrand_next_float(&ref), s2 = jrand_next_float(&ref);
		assert(f4_fbits(s1) == 0x3e0c3f78u && f4_fbits(s2) == 0x3f10b4c8u); /* JVM #1352/#1353 */
		const float want_pitch = (1.0F + (s1 - s2) * 0.2F) * 0.7F;
		jrand_set_seed(&w->rand, JW_RAY_SEED);
		g_f4_snd_n = 0;
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {c[0], c[1], c[2]}, POWER);
		f4_settle(w);
		size_t got_n = server_world_test_last_blast_order(g_f4_prod, F4_J6_MAX);
		bool same_set = got_n == want_n, same_order = got_n == want_n;
		for(size_t i = 0; i < want_n && same_order; i++)
			same_order = memcmp(g_f4_prod[i], g_f4_order[i], sizeof(g_f4_order[0])) == 0;
		if(!same_order && got_n == want_n) {
			/* membership alone, independent of order */
			for(size_t i = 0; i < want_n && same_set; i++) {
				bool found = false;
				for(size_t j = 0; j < got_n && !found; j++)
					found = memcmp(g_f4_prod[j], g_f4_order[i], sizeof(g_f4_order[0])) == 0;
				same_set = found;
			}
		}
		float pitch = 0.0F;
		bool heard = f4_find_sound(SOUND_RANDOM_EXPLODE, &pitch);
		printf("  (F4.1) air blast: model %zu cells, production %zu; set %s, order %s; "
			   "explode pitch %.9g (want %.9g); World.rand state %#llx (want %#llx)\n",
			   want_n, got_n, same_set ? "equal" : "DIFFERENT",
			   same_order ? "equal" : "DIFFERENT", (double)pitch, (double)want_pitch,
			   (unsigned long long)w->rand.seed, (unsigned long long)ref.seed);
		F4_CHECK(same_set, "F4.1 destroyed-cell membership != JVM-stream rays");
		F4_CHECK(same_order, "F4.1 doEffects visit order != OpenJDK 6 reverse iteration "
				 "(PENDING ORACLE QUALIFICATION)");
		F4_CHECK(heard && f4_fbits(pitch) == f4_fbits(want_pitch),
				 "F4.1 random.explode pitch not World.rand #1352/#1353 after the rays");
		F4_CHECK(w->rand.seed == ref.seed && !w->rand.have_next_gaussian,
				 "F4.1 World.rand final state != 1352 rays + 2 sound draws");
	}

	/* ---- F4.2 dirt + TNT: drops, chain fuse, cell order on World.rand ---- */
	{
		const int X0 = 104, Z0 = 104, Y0 = 50;
		for(int x = X0; x < X0 + 13; x++)
			for(int z = Z0; z < Z0 + 13; z++)
				for(int y = Y0; y < Y0 + 13; y++)
					set_block(s, x, y, z, BLOCK_DIRT);
		set_block(s, X0 + 7, Y0 + 6, Z0 + 6, BLOCK_TNT);
		const vec3 c = {X0 + 6.5F, Y0 + 6.5F, Z0 + 6.5F};
		struct java_random ref;
		jrand_set_seed(&ref, JW_RAY_SEED ^ 0x2A);
		f4_transcribe(s, c, POWER, &ref);
		size_t want_n = f4_j6_visit_order(&g_f4_j6, g_f4_order);
		float s1 = jrand_next_float(&ref), s2 = jrand_next_float(&ref);
		const float want_pitch = (1.0F + (s1 - s2) * 0.2F) * 0.7F;
		/* doEffects(false) per visited cell: live id; dirt: quantity 1 (no
		 * draw), nextFloat <= 0.3F, idDropped (no draw), 3 offset floats;
		 * TNT: quantity 0, then onBlockDestroyedByExplosion nextInt(20)+10. */
		static float want_pos[4096][3];
		size_t want_drops = 0;
		int want_fuse = -1;
		for(size_t i = 0; i < want_n; i++) {
			struct block_data bd;
			assert(server_world_get_block(w, g_f4_order[i][0], g_f4_order[i][1],
										  g_f4_order[i][2], &bd));
			if(bd.type == BLOCK_TNT) {
				want_fuse = jrand_next_int_bound(&ref, 80 / 4) + 80 / 8;
				continue;
			}
			if(bd.type != BLOCK_DIRT)
				continue;
			if(jrand_next_float(&ref) <= 0.3F) {
				assert(want_drops < 4096);
				for(int k = 0; k < 3; k++) {
					double off = (double)(jrand_next_float(&ref) * 0.7F)
						+ (double)(1.0F - 0.7F) * 0.5;
					want_pos[want_drops][k]
						= (float)((double)g_f4_order[i][k] + off);
				}
				want_drops++;
			}
		}
		jrand_set_seed(&w->rand, JW_RAY_SEED ^ 0x2A);
		uint32_t first_id = s->next_entity_id;
		g_f4_snd_n = 0;
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {c[0], c[1], c[2]}, POWER);
		f4_settle(w);
		size_t got_n = server_world_test_last_blast_order(g_f4_prod, F4_J6_MAX);
		bool same_order = got_n == want_n;
		for(size_t i = 0; i < want_n && same_order; i++)
			same_order = memcmp(g_f4_prod[i], g_f4_order[i], sizeof(g_f4_order[0])) == 0;
		static struct entity* made[8192];
		size_t nm = f4_new_entities(s, first_id, made, 8192);
		size_t items = 0, pos_ok = 0, tnts = 0;
		int fuse = -1;
		for(size_t i = 0; i < nm; i++) {
			if(made[i]->type == ENTITY_ITEM) {
				if(items < want_drops && made[i]->pos[0] == want_pos[items][0]
				   && made[i]->pos[1] == want_pos[items][1]
				   && made[i]->pos[2] == want_pos[items][2])
					pos_ok++;
				items++;
			} else if(made[i]->type == ENTITY_PRIMED_TNT) {
				tnts++;
				fuse = made[i]->data.primed_tnt.fuse;
			}
		}
		float pitch = 0.0F;
		bool heard = f4_find_sound(SOUND_RANDOM_EXPLODE, &pitch);
		printf("  (F4.2) dirt+TNT blast: cells %zu/%zu order %s; drops %zu (want %zu), "
			   "positions exact %zu; chain fuse %d (want %d); pitch %s; state %#llx (want %#llx)\n",
			   got_n, want_n, same_order ? "equal" : "DIFFERENT", items, want_drops,
			   pos_ok, fuse, want_fuse,
			   heard && f4_fbits(pitch) == f4_fbits(want_pitch) ? "exact" : "DIFFERENT",
			   (unsigned long long)w->rand.seed, (unsigned long long)ref.seed);
		F4_CHECK(same_order, "F4.2 visit order (PENDING ORACLE QUALIFICATION)");
		F4_CHECK(items == want_drops && pos_ok == want_drops,
				 "F4.2 drop chance/position draws not World.rand in visit order");
		F4_CHECK(tnts == 1 && fuse == want_fuse, "F4.2 chain-TNT fuse not World.rand.nextInt(20)+10 in order");
		F4_CHECK(heard && f4_fbits(pitch) == f4_fbits(want_pitch), "F4.2 explode pitch");
		F4_CHECK(w->rand.seed == ref.seed, "F4.2 World.rand final state");
		free_entities(s);
	}

	/* ---- F4.3 flaming: a fresh ExplosionRNG per blast, reverse order ---- */
	{
		const int X0 = 112, Z0 = 120, Y0 = 70;
		for(int x = X0 - 6; x <= X0 + 6; x++)
			for(int z = Z0 - 6; z <= Z0 + 6; z++)
				set_block(s, x, Y0, z, BLOCK_STONE);
		const vec3 c = {X0 + 0.5F, Y0 + 1.5F, Z0 + 0.5F};
		const float fpower = 2.0F;
		size_t want_lit[2] = {0, 0};
		struct server_world_explosion_cost cost[2];
		for(int pass = 0; pass < 2; pass++) {
			struct java_random ref;
			jrand_set_seed(&ref, JW_RAY_SEED ^ 0x77);
			f4_transcribe(s, c, fpower, &ref);
			size_t want_n = f4_j6_visit_order(&g_f4_j6, g_f4_order);
			size_t draws = 0;
			for(size_t i = 0; i < want_n; i++) {
				struct block_data cur, below;
				if(!server_world_get_block(w, g_f4_order[i][0], g_f4_order[i][1],
										   g_f4_order[i][2], &cur) || cur.type != 0)
					continue;
				if(!server_world_get_block(w, g_f4_order[i][0], g_f4_order[i][1] - 1,
										   g_f4_order[i][2], &below)
				   || below.type != BLOCK_STONE)
					continue;
				assert(draws < 64);
				if(jw_fire_int3[draws++] == 0)
					want_lit[pass]++;
			}
			jrand_set_seed(&w->rand, JW_RAY_SEED ^ 0x77);
			/* ExplosionRNG = new Random() per Explosion: the seam seeds the
			 * NEXT blast's fresh stream; pass 1 reseeds identically, proving no
			 * state carries across explosions. */
			server_world_test_seed_fire_rng(UINT32_C(0xF1A3E5C7));
			server_world_begin_update(w);
			server_world_explode_flaming(w, (vec3) {c[0], c[1], c[2]}, fpower);
			f4_settle(w);
			server_world_explosion_last_cost(&cost[pass]);
			/* restore the floor (an identical write is a no-op that reports
			 * false, so only rewrite cells the blast removed) and clear the
			 * air layer above it */
			for(int x = X0 - 6; x <= X0 + 6; x++)
				for(int z = Z0 - 6; z <= Z0 + 6; z++) {
					struct block_data cur;
					assert(server_world_get_block(w, x, Y0, z, &cur));
					if(cur.type != BLOCK_STONE)
						set_block(s, x, Y0, z, BLOCK_STONE);
				}
			free_entities(s);
		}
		printf("  (F4.3) flaming: lit %u/%u (want %zu/%zu)\n", cost[0].fire_lit,
			   cost[1].fire_lit, want_lit[0], want_lit[1]);
		F4_CHECK(want_lit[0] > 0 && cost[0].fire_lit == want_lit[0]
				 && cost[1].fire_lit == want_lit[1],
				 "F4.3 ExplosionRNG not a fresh java.util.Random per blast in reverse order");
	}

	/* ---- F4.4 host body Entity.rand: fresh/absorbed/excess/fresh --------- */
	{
		struct server_player* p = &s->player;
		const double px = 100.5, pz = 140.5, feet = 4.0;
		p->has_pos = true;
		p->x = px; p->y = feet + PLAYER_EYE_HEIGHT; p->z = pz;
		p->health = MAX_PLAYER_HEALTH;
		p->hurt_resist_time = 0;
		p->last_damage = 0;
		server_local_test_seed_body_rand(s, p, JW_BODY_SEED);
		float f[6];
		for(int i = 0; i < 6; i++) memcpy(&f[i], &jw_body_FLOATS[i].bits, 4);
		float got[4] = {0};
		int heard_n = 0;
		const float ey = (float)p->y;
		/* 1 fresh (11) -> pitch f0,f1 */
		g_f4_snd_n = 0;
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {(float)(px - 6.0), ey, (float)pz}, POWER);
		f4_settle(w);
		heard_n += f4_find_sound(SOUND_RANDOM_HURT, &got[0]);
		/* 2 absorbed (same 11 in the window): no draws, no sound */
		g_f4_snd_n = 0;
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {(float)(px - 6.0), ey, (float)pz}, POWER);
		f4_settle(w);
		bool absorbed_silent = !f4_find_sound(SOUND_RANDOM_HURT, &got[1]);
		/* 3 excess-only (closer: more damage in the window): no draws */
		g_f4_snd_n = 0;
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {(float)(px - 4.0), ey, (float)pz}, POWER);
		f4_settle(w);
		bool excess_silent = !f4_find_sound(SOUND_RANDOM_HURT, &got[1]);
		/* 4 fresh again after the window: pitch f2,f3 */
		p->hurt_resist_time = 0;
		p->health = MAX_PLAYER_HEALTH;
		g_f4_snd_n = 0;
		server_world_begin_update(w);
		server_world_explode(w, (vec3) {(float)(px - 6.0), ey, (float)pz}, POWER);
		f4_settle(w);
		heard_n += f4_find_sound(SOUND_RANDOM_HURT, &got[2]);
		const float w0 = (f[0] - f[1]) * 0.2F + 1.0F;
		const float w1 = (f[2] - f[3]) * 0.2F + 1.0F;
		printf("  (F4.4) host hurt pitch %.9g / %.9g (want %.9g / %.9g); absorbed %s, "
			   "excess %s\n", (double)got[0], (double)got[2], (double)w0, (double)w1,
			   absorbed_silent ? "silent" : "SOUNDED", excess_silent ? "silent" : "SOUNDED");
		F4_CHECK(heard_n == 2 && f4_fbits(got[0]) == f4_fbits(w0)
				 && f4_fbits(got[2]) == f4_fbits(w1),
				 "F4.4 fresh hurt pitch not two left-to-right body Entity.rand floats");
		F4_CHECK(absorbed_silent && excess_silent, "F4.4 absorbed/excess branch sounded");
		p->x = 200.5; p->y = 4.5; p->z = 200.5;
		p->health = MAX_PLAYER_HEALTH;
		p->hurt_resist_time = 0;
		free_entities(s);
	}

	f4rng_more(s);
	clin_test_set_rpc_observer(NULL, NULL);
	free(g_f4_j6.table);
	g_f4_j6.table = NULL;
	printf("  F4-RNG: %d failing check(s)\n", f4_fails);
	fflush(stdout);
	assert(f4_fails == 0);
	f4rng_r2_drops(s);
	f4rng_drop_cases(s, r3_cases, sizeof r3_cases / sizeof r3_cases[0]);
	f4rng_r3_air(s);
	f4rng_r3_death(s);
	f4rng_r3_harvest(s);
	f4rng_r3_support(s);
	bed_float_center_run(s);
	puts("  F4-RNG: World.rand rays/sound/drops/fuse, ExplosionRNG, body Entity.rand OK");
}

#include "network/server_local_guest.h" /* F2a (h): the real respawn poll */
/* ---- V6 F2a (ASTRA-F2-RULING 1-10): world -> network -> tracker -------
 * S/net/minecraft/server/MinecraftServer.java:328-341 ticks every world
 * (World.tick, light drain, updateEntities) and only THEN runs the network
 * (NetworkManager.processReadPackets:191-209, FIFO, <= 101 per connection),
 * then the trackers. Every case below sends through the REAL inbox
 * (svin_rpc_send / svin_rpc_send_from) before one real update; the message
 * is therefore serviceable when that update starts, and Beta services it
 * AFTER that update's world pass, at its own FIFO slot. */
static struct server_rpc f2a_pos(const struct server_player* p, float vx) {
	return (struct server_rpc) {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = p->x, .y = p->y, .z = p->z, .rx = p->rx, .ry = p->ry,
			.vel_x = vx, .on_ground = false,
			.correction_seq = p->correction_seq,
		},
	};
}
static bool f2a_send(unsigned owner, struct server_rpc rpc) {
	const struct server_rpc_source src = owner
		? (struct server_rpc_source) {.slot = 1, .generation = W.guest_generation}
		: (struct server_rpc_source) {0};
	if(rpc.type == SRPC_PLAYER_POS) { /* the real credited publication */
		if(!svin_credit_reserve(src))
			return false;
		svin_credit_publish(src, &rpc);
		return true;
	}
	return svin_rpc_send_from(src, &rpc);
}
static struct entity* f2a_mob;
static void f2a_attack_trace(struct server_local* sv, const struct server_rpc* call) {
	if(call->type == SRPC_ENTITY_ATTACK && f2a_mob)
		printf("    [F2a] attack dispatched at world_time %lld: target hurt_time %d\n",
			   (long long)sv->world_time, (int)f2a_mob->data.monster.hurt_time);
}

/* (a) attack around a mob AI step, both owners. S/EntityLiving.java
 * onEntityUpdate decrements hurtTime in the mob's world-pass update;
 * NetServerHandler.func_6006_a:475-483 -> attackTargetEntityWithCurrentItem
 * -> EntityLiving.attackEntityFrom sets hurtTime = maxHurtTime (10) in the
 * network phase AFTER it. So the update ends with hurtTime 10 (a pre-world
 * attack would end it at 9), and the next update's world pass makes it 9. */
static bool (*f2a_real_tick)(struct entity*, struct server_local*);
static unsigned f2a_ticks;
static bool f2a_count_tick(struct entity* e, struct server_local* sv) {
	f2a_ticks++;
	return f2a_real_tick(e, sv);
}
/* World.updateEntityWithOptionalForce:1017-1022 freezes an entity unless
 * every chunk within +-32 blocks exists; make the fixture area resident so
 * the witnesses are not vacuous (terrain residency, no entity placement). */
static void f2a_resident_slot(struct server_local* sv, int slot) {
	for(int cx = -3; cx <= 3; cx++)
		for(int cz = -3; cz <= 3; cz++) {
			struct server_chunk* sc = NULL;
			(void)server_world_load_chunk(&sv->worlds[slot], cx, cz, &sc);
		}
	assert(server_world_area_loaded_radius(&sv->worlds[slot], 12, 0, 10, 32));
}
static void f2a_resident(struct server_local* sv) {
	f2a_resident_slot(sv, 0);
	f2a_resident_slot(sv, 1);
}
static void f2a_mob_attack_run(unsigned owner, int slot) {
	struct server_local* sv = &W;
	w_session(slot, false);
	f2a_resident(sv);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	struct server_player* g = NULL;
	if(owner) {
		svin_set_split_accept(true);
		g = f1_guest_install(sv);
		g->world_slot = (uint8_t)slot;
		server_local_refresh_guest_membership(sv);
	}
	struct server_player* body = owner ? g : &sv->player;
	struct entity* m = server_local_spawn_monster_at_w(&sv->worlds[slot],
		(vec3) {(float)body->x + 1.5F, (float)(body->y - 1.62), (float)body->z},
		MONSTER_PIG);
	assert(m && m->tick_server);
	server_local_refresh_entity_membership(sv, m);
	/* Precondition: the mob is live in EVERY world pass (its hurtTime
	 * decrements once per update) -- otherwise the witness is vacuous. */
	server_local_test_update_once(sv);
	m->data.monster.hurt_time = 6;
	test_mob_sync(m);
	f2a_real_tick = m->tick_server;
	m->tick_server = f2a_count_tick;
	for(int k = 0; k < 4; k++) {
		f2a_ticks = 0;
		server_local_test_update_once(sv);
		printf("    settle update %d: mob ticks %u pos (%.3f %.3f %.3f)\n", k,
			   f2a_ticks, (double)m->pos[0], (double)m->pos[1], (double)m->pos[2]);
		assert(f2a_ticks == 1);
		printf("    settle update %d: world_time %lld hurt_time %d\n", k,
			   (long long)sv->world_time, (int)m->data.monster.hurt_time);
		fflush(stdout);
	}
	assert(m->data.monster.hurt_time == 2);
	m->data.monster.hurt_time = 0;
	test_mob_sync(m);
	const short h0 = m->health;
	assert(f2a_send(owner, (struct server_rpc) {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = m->id}));
	f2a_mob = m;
	server_local_test_rpc_done_hook = f2a_attack_trace;
	server_local_test_update_once(sv);
	server_local_test_rpc_done_hook = NULL;
	f2a_mob = NULL;
	printf("  (F2a) owner %u world %d attack around the mob step: hurt_time %d (want %d) health %d -> %d\n",
		   owner, slot, (int)m->data.monster.hurt_time, MOB_HURT_ANIMATION_TICKS,
		   (int)h0, (int)m->health);
	assert(m->health < h0); /* the hit landed */
	assert(m->data.monster.hurt_time == MOB_HURT_ANIMATION_TICKS);
	server_local_test_update_once(sv);
	assert(m->data.monster.hurt_time == MOB_HURT_ANIMATION_TICKS - 1);
	puts("  (F2a) attack runs after the target world-pass update, at its network slot OK");
	if(g)
		f1_guest_remove(sv, g);
	svin_set_split_accept(false);
	svin_set_local_motion(false);
	w_close();
}

/* (b) packet -> vehicle destruction, host. The riding packet comes FIRST in
 * FIFO, so S/NetServerHandler.handleFlying:71-108 runs it on the LIVE boat:
 * :94-95 motionX/Z = input, then :97 func_12017_b ->
 * World.updateEntityWithOptionalForce:1071-1076 -> updateEntity(rider) ->
 * Entity.updateRidden:819-822 zeroes motionX/Y/Z. Only then does Packet7
 * (func_6006_a) break the boat (EntityBoat.attackEntityFrom:60-81:
 * damageTaken 35, minus its world-pass decay, plus 10 > 40). A batched
 * replay after the break would find the boat dead (updateRidden:816-817:
 * unlink WITHOUT zeroing) and leave motionX = the 0.25 input. */
static void f2a_boat_break_run(unsigned owner, int slot) {
	struct server_local* sv = &W;
	w_session(slot, false);
	f2a_resident(sv);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	struct server_player* g = NULL;
	if(owner) {
		svin_set_split_accept(true);
		g = f1_guest_install(sv);
		g->world_slot = (uint8_t)slot;
		server_local_refresh_guest_membership(sv);
	}
	struct server_player* body = owner ? g : &sv->player;
	struct entity* v = server_local_spawn_boat_w((vec3) {19.9F, 50.0F, 20.5F},
		&sv->worlds[slot]);
	assert(v);
	v->orient[0] = 0.0F;
	if(owner)
		server_local_set_boat_occupancy_for(sv, v, g, true);
	else
		server_local_set_boat_occupancy(sv, v, true);
	v->data.boat.damage_taken = 35;
	const uint32_t vid = v->id;
	assert(server_local_actor_vehicle(sv, body) == v && body->riding_boat);
	double sx, sy, sz;
	z_seat(v, true, &sx, &sy, &sz);
	body->x = sx;
	body->y = sy;
	body->z = sz;
	body->on_ground = false;
	body->vel_x = 0.3;
	body->vel_z = 0.0;
	MOTY_SET(*body, 0.0);
	if(owner)
		server_local_refresh_guest_membership(sv);
	else
		server_local_refresh_player_membership(sv);
	const unsigned ride0 = server_local_test_riding_branch_count;
	assert(f2a_send(owner, f2a_pos(body, 0.25F)));
	assert(f2a_send(owner, (struct server_rpc) {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = vid}));
	server_local_test_update_once(sv);
	printf("  (F2a) owner %u world %d packet -> boat break: boat %s riding %d branch %u motion (%.9g, %.9g, %.9g)\n",
		   owner, slot, dict_entity_get(sv->entities, vid) ? "alive" : "gone",
		   (int)body->riding_boat,
		   server_local_test_riding_branch_count - ride0, body->vel_x,
		   MOTY(*body), body->vel_z);
	assert(!dict_entity_get(sv->entities, vid) && !body->riding_boat);
	assert(server_local_test_riding_branch_count == ride0 + 1);
	assert(server_local_test_riding_branch_motion[0] == (double)0.25F);
	assert(body->vel_x == 0.0 && MOTY(*body) == 0.0 && body->vel_z == 0.0);
	assert(body->motion_q_n == 0);
	puts("  (F2a) riding packet runs on the live boat before the breaking attack (updateRidden zero) OK");
	if(g)
		f1_guest_remove(sv, g);
	svin_set_split_accept(false);
	svin_set_local_motion(false);
	w_close();
}

/* (c) packet -> pause -> packet, host. The first sample finishes its
 * server effects at its FIFO slot (one Beta air step of the pre-motion,
 * EntityLiving.moveEntityWithHeading:459-516), then the pause takes effect
 * at its network position (ruling item 5): the second sample is paused (no
 * physics). Nothing remains queued, and the resume adds no second step. A
 * batched replay leaves the first sample unstepped behind the pause. */
static void f2a_pause_fifo_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	sv->player.on_ground = false;
	sv->player.x = 8.5;
	sv->player.y = 43.0;
	sv->player.z = 8.5;
	const double pre[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	assert(f2a_send(0, (struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	server_local_test_update_once(sv);
	printf("  (F2a) packet -> pause -> packet: paused %d queued %u motion (%.9g, %.9g, %.9g) want (%.9g, %.9g, %.9g)\n",
		   (int)sv->paused, (unsigned)sv->player.motion_q_n, sv->player.vel_x,
		   MOTY(sv->player), sv->player.vel_z, wit_xz(pre[0]), wit_y(pre[1]),
		   wit_xz(pre[2]));
	assert(sv->paused && sv->player.motion_q_n == 0);
	assert(sv->player.vel_x == wit_xz(pre[0]) && MOTY(sv->player) == wit_y(pre[1])
		   && sv->player.vel_z == wit_xz(pre[2]));
	assert(f2a_send(0, (struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}));
	server_local_test_update_once(sv);
	assert(!sv->paused && sv->player.vel_x == wit_xz(pre[0])
		   && MOTY(sv->player) == wit_y(pre[1]));
	assert(svin_credit_outstanding(0) == 0);
	puts("  (F2a) the sample before a pause steps at its FIFO slot; the paused one never does OK");
	svin_set_local_motion(false);
	w_close();
}

/* (e) placement at a due block tick, host, both worlds. S/World.tick:
 * worldTime+1 (:1516-1521) THEN TickUpdates (:1522, fires entries with
 * scheduledTime <= worldTime, :1722); the network phase follows. Packet15
 * places sand (S/NetServerHandler:227-320 -> BlockSand.onBlockAdded ->
 * scheduleUpdateTick(tickRate 3) = worldTime_N + 3), so the tick is due in
 * the world pass of update N+3: the sand still stands after N+1 and N+2
 * and has fallen (cell empty) after N+3. A pre-world placement would see
 * worldTime_N - 1 and fall one update earlier. */
static void f2a_place_tick_run(int slot) {
	struct server_local* sv = &W;
	w_session(slot, false);
	f2a_resident(sv);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	struct server_world* w = &sv->worlds[slot];
	assert(server_world_set_block(w, 8, 41, 10, (struct block_data) {.type = BLOCK_STONE}));
	(void)server_world_set_block(w, 8, 40, 10, (struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(w, 8, 39, 10, (struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(w, 8, 38, 10, (struct block_data) {.type = BLOCK_AIR});
	{
		struct block_data c;
		for(int y = 38; y <= 40; y++)
			assert(server_world_get_block(w, 8, y, 10, &c) && c.type == BLOCK_AIR);
	}
	inventory_set_hotbar(&sv->player.inventory, 0);
	sv->player.inventory.items[INVENTORY_SLOT_HOTBAR] = (struct item_data) {
		.id = BLOCK_SAND, .durability = 0, .count = 4};
	server_local_test_update_once(sv);
	struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 8, .y = 41, .z = 10, .side = SIDE_BOTTOM}};
	assert(f2a_send(0, place));
	server_local_test_update_once(sv); /* update N: placed in its network phase */
	struct block_data b;
	assert(server_world_get_block(w, 8, 40, 10, &b));
	printf("  (F2a) world %d place sand: after N cell %d\n", slot, (int)b.type);
	assert(b.type == BLOCK_SAND);
	int fell_after = -1;
	for(int k = 1; k <= 5 && fell_after < 0; k++) {
		server_local_test_update_once(sv);
		assert(server_world_get_block(w, 8, 40, 10, &b));
		printf("  (F2a) world %d place sand: after N+%d cell %d\n", slot, k, (int)b.type);
		if(b.type != BLOCK_SAND)
			fell_after = k;
	}
	assert(fell_after == 3);
	puts("  (F2a) a placement's scheduled tick counts from the post-world network phase OK");
	svin_set_local_motion(false);
	w_close();
}

/* (f) repeated host pause inside ONE drain: P1, pause, P2, resume, P3,
 * pause, P4. Each control takes effect at its own slot (ruling item 5):
 * P1 and P3 step once each (two Beta air steps), P2/P4 never; nothing stays
 * queued; the session ends paused so no tracker flush; the next resume adds
 * no step. */
static void f2a_pause_repeat_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	sv->player.on_ground = false;
	sv->player.x = 8.5; sv->player.y = 43.0; sv->player.z = 8.5;
	const double pre[3] = {sv->player.vel_x, MOTY(sv->player), sv->player.vel_z};
	const struct server_rpc tp = {.type = SRPC_TOGGLE_PAUSE};
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	assert(f2a_send(0, tp));
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	assert(f2a_send(0, tp));
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	assert(f2a_send(0, tp));
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	g_vel_n = 0;
	server_local_test_update_once(sv);
	const double ex = wit_xz(wit_xz(pre[0])), ey = wit_y(wit_y(pre[1])),
		ez = wit_xz(wit_xz(pre[2]));
	printf("  (F2a) repeated pause: paused %d queued %u motion (%.9g, %.9g, %.9g) want (%.9g, %.9g, %.9g) msgs %zu\n",
		   (int)sv->paused, (unsigned)sv->player.motion_q_n, sv->player.vel_x,
		   MOTY(sv->player), sv->player.vel_z, ex, ey, ez, g_vel_n);
	assert(sv->paused && sv->player.motion_q_n == 0);
	assert(sv->player.vel_x == ex && MOTY(sv->player) == ey && sv->player.vel_z == ez);
	assert(svin_credit_outstanding(0) == 0);
	assert(f2a_send(0, tp));
	server_local_test_update_once(sv);
	assert(!sv->paused && sv->player.vel_x == ex && MOTY(sv->player) == ey);
	puts("  (F2a) repeated host pause: every control at its own slot, P1/P3 step once, P2/P4 never OK");
	svin_set_local_motion(false);
	w_close();
}

/* (g) budget spill: 102 host samples become serviceable during one update
 * (the drain hook publishes the next one as each is processed). Beta
 * NetworkManager.processReadPackets:191-209 handles 101 and leaves the rest;
 * the 102nd is serviced in the NEXT update's network phase, i.e. AFTER that
 * update's world pass -- here a blast in its entity pass. So: 101 air steps,
 * then the blast impulse, then the 102nd step. A pre-world drain would step
 * the 102nd before the blast. */
static int f2a_spill_left;
static void f2a_spill_hook(struct server_local* sv, const struct server_rpc* call) {
	if(call->type == SRPC_PLAYER_POS && f2a_spill_left > 0) {
		f2a_spill_left--;
		assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	}
}
static void f2a_spill_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	g_vel_n = 0;
	sv->player.hurt_resist_time = 0;
	sv->player.last_damage = 0;
	sv->player.health = MAX_PLAYER_HEALTH;
	sv->player.on_ground = false;
	sv->player.x = 8.5; sv->player.y = 43.0; sv->player.z = 8.5;
	double ex = sv->player.vel_x, ey = MOTY(sv->player), ez = sv->player.vel_z;
	f2a_spill_left = BETA_READ_PACKETS_PER_TICK; /* 1 sent + 101 refills = 102 */
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	server_local_test_rpc_done_hook = f2a_spill_hook;
	server_local_test_update_once(sv);
	server_local_test_rpc_done_hook = NULL;
	for(int k = 0; k < BETA_READ_PACKETS_PER_TICK; k++) {
		ex = wit_xz(ex); ey = wit_y(ey); ez = wit_xz(ez);
	}
	printf("  (F2a) spill: held %zu left %d motion (%.9g, %.9g, %.9g) want (%.9g, %.9g, %.9g)\n",
		   svin_held_count(), f2a_spill_left, sv->player.vel_x, MOTY(sv->player),
		   sv->player.vel_z, ex, ey, ez);
	assert(f2a_spill_left == 0);
	assert(sv->player.vel_x == ex && MOTY(sv->player) == ey && sv->player.vel_z == ez);
	assert(svin_held_count() + svin_pending_count() >= 1 || svin_credit_outstanding(0) == 1);
	double imp[3];
	const int dmg = blast_terms(w_center, 8.5, 43.0, 8.5, 8.0, imp);
	(void)dmg;
	w_hook_world = 0;
	w_hook_fired = false;
	w_hook_lethal = false;
	server_local_test_phase_hook = w_hook;
	server_local_test_update_once(sv);
	server_local_test_phase_hook = NULL;
	assert(w_hook_fired);
	ex = wit_xz(addm(ex, imp[0])); ey = wit_y(ey + imp[1]); ez = wit_xz(addm(ez, imp[2]));
	printf("  (F2a) spill: after the blast update motion (%.9g, %.9g, %.9g) want (%.9g, %.9g, %.9g)\n",
		   sv->player.vel_x, MOTY(sv->player), sv->player.vel_z, ex, ey, ez);
	assert(sv->player.vel_x == ex && MOTY(sv->player) == ey && sv->player.vel_z == ez);
	assert(svin_credit_outstanding(0) == 0 && svin_held_count() == 0);
	puts("  (F2a) budget spill: 101 serviced, the 102nd after the next world pass OK");
	svin_set_local_motion(false);
	w_close();
}

/* (h) lifetime replacement inside ONE drain, guest: P1 (a corpse packet of
 * the dead body), SRPC_RESPAWN, the exact LOCAL_GUEST_RESPAWN_READY (sent by
 * the drain hook once the plan publishes its epoch), P2 (still the old
 * lifetime's correction token). Beta handleFlying runs P1 on the old
 * EntityPlayerMP (EntityLiving.onEntityUpdate:154-159 deathTime++) at its
 * slot; Packet9 handleRespawnPacket:488-492 then recreates the player, so
 * P2 never reaches a body. A batched replay would have dropped P1 with the
 * old lifetime instead. */
static unsigned f2a_old_events;
static uint64_t f2a_old_gen;
static void f2a_old_count_hook(struct server_player* body, unsigned remaining) {
	(void)remaining;
	if(body == W.guest_actor && W.guest_generation == f2a_old_gen
	   && body->health <= 0)
		f2a_old_events++;
}
static bool f2a_ready_sent;
static void f2a_ready_hook(struct server_local* sv, const struct server_rpc* call) {
	if(call->type == SRPC_RESPAWN && !f2a_ready_sent
	   && sv->guest_respawn_phase == GUEST_RESPAWN_LOADING) {
		f2a_ready_sent = true;
		assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
				.generation = sv->guest_generation},
			&(struct server_rpc) {.type = SRPC_LOCAL_GUEST,
				.payload.local_guest = {.ticket = sv->guest_ticket,
					.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
					.generation = sv->guest_generation,
					.respawn_epoch = sv->guest_respawn_epoch}}));
		struct server_rpc p2 = f2a_pos(sv->guest_actor, 0.0F);
		assert(f2a_send(1, p2));
	}
}
static void f2a_respawn_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	f2a_resident(sv);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	svin_set_split_accept(true);
	struct server_player* g = f1_guest_install(sv);
	sv->guest_owned = true;
	sv->guest_ticket = 77;
	sv->guest_confirmed = true;
	g->spawn_x = 10; g->spawn_y = 41; g->spawn_z = 10;
	server_local_refresh_guest_membership(sv);
	g->hurt_resist_time = 0;
	(void)server_local_damage_actor(sv, g, 4 * MAX_PLAYER_HEALTH, 0.0F, 0.0F);
	assert(g->health <= 0);
	g->death_ticks = 0;
	f2a_old_gen = sv->guest_generation;
	f2a_old_events = 0;
	f2a_ready_sent = false;
	assert(f2a_send(1, f2a_pos(g, 0.0F)));
	assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
			.generation = sv->guest_generation},
		&(struct server_rpc) {.type = SRPC_RESPAWN,
			.payload.respawn = {.owner = 1, .generation = sv->guest_generation,
				.respawn_epoch = 0}}));
	server_local_test_motion_replay_hook = f2a_old_count_hook;
	server_local_test_rpc_done_hook = f2a_ready_hook;
	server_local_test_update_once(sv);
	server_local_test_motion_replay_hook = NULL;
	server_local_test_rpc_done_hook = NULL;
	printf("  (F2a) respawn: old-lifetime corpse events %u, ready sent %d, phase %d, health %d, death_ticks %d, outstanding %u\n",
		   f2a_old_events, (int)f2a_ready_sent, (int)sv->guest_respawn_phase,
		   sv->guest_actor ? (int)sv->guest_actor->health : -1,
		   sv->guest_actor ? (int)sv->guest_actor->death_ticks : -1,
		   svin_credit_outstanding(1));
	assert(f2a_ready_sent && f2a_old_events == 1);
	/* The commit itself may wait for its destination column (not the F2a
	 * claim); let the real transaction finish, with no new samples. */
	if(sv->guest_respawn_phase != GUEST_RESPAWN_IDLE) {
		/* test_owner_swap_pose_dead_sleep_barrier's recipe: publish the
		 * destination column, then the production poll commits. */
		w_coord_t rcx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(sv->guest_respawn_x));
		w_coord_t rcz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(sv->guest_respawn_z));
		struct server_chunk* dc = dict_server_chunks_get(sv->worlds[0].chunks,
			S_CHUNK_ID(rcx, rcz));
		if(!dc)
			assert(server_world_load_chunk(&sv->worlds[0], rcx, rcz, &dc) && dc);
		dc->populated = true;
		dc->sent = true;
		server_local_guest_respawn_poll(sv);
	}
	assert(f2a_old_events == 1);
	/* The fresh body is installed (its later READY/ack bookkeeping is the
	 * respawn transaction's own, covered by test_owner_swap_pose_dead_*). */
	assert(sv->guest_actor->health == MAX_PLAYER_HEALTH
		   && sv->guest_actor->death_ticks == 0
		   && sv->guest_actor->motion_q_n == 0);
	assert(svin_credit_outstanding(1) == 0);
	puts("  (F2a) lifetime replacement: the corpse packet before RESPAWN runs at its slot, the stale one after the commit never OK");
	f1_guest_remove(sv, sv->guest_actor);
	svin_set_split_accept(false);
	svin_set_local_motion(false);
	w_close();
}

/* (d) zero / one / multiple packets, both owners, interleaved with a
 * gameplay action. Each owner's FIFO is serviced in order (NetworkManager
 * per-connection FIFO); every accepted sample steps exactly once, at its own
 * slot, so the attack that follows the host's two samples sees both already
 * stepped. The cross-owner order is the fixture's recorded arrival order
 * (ruling item 4), not a claimed Beta interconnection order. Zero packets:
 * the live body's server motion does not evolve at all. */
static unsigned f2a_order_n;
static int f2a_order[16];
static void f2a_order_hook(struct server_player* body, unsigned remaining) {
	(void)remaining;
	if(f2a_order_n < 16)
		f2a_order[f2a_order_n++] = body == W.guest_actor ? 1 : 0;
}
static double f2a_host_vx_at_attack;
static void f2a_attack_probe(struct server_local* sv, const struct server_rpc* call) {
	if(call->type == SRPC_ENTITY_ATTACK)
		f2a_host_vx_at_attack = sv->player.vel_x;
}
static void f2a_multi_run(void) {
	struct server_local* sv = &W;
	w_session(0, false);
	f2a_resident(sv);
	server_local_test_update_once(sv);
	svin_set_local_motion(true);
	svin_set_split_accept(true);
	struct server_player* g = f1_guest_install(sv);
	sv->player.on_ground = false;
	sv->player.x = 8.5; sv->player.y = 43.0; sv->player.z = 8.5;
	const double h0 = sv->player.vel_x, g0 = g->vel_x;
	/* zero packets */
	server_local_test_update_once(sv);
	assert(sv->player.vel_x == h0 && g->vel_x == g0);
	struct entity* m = server_local_spawn_monster_at_w(&sv->worlds[0],
		(vec3) {10.0F, 41.38F, 8.5F}, MONSTER_PIG);
	assert(m);
	server_local_refresh_entity_membership(sv, m);
	f2a_order_n = 0;
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	assert(f2a_send(1, f2a_pos(g, 0.0F)));
	assert(f2a_send(0, f2a_pos(&sv->player, 0.0F)));
	assert(f2a_send(0, (struct server_rpc) {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = m->id}));
	server_local_test_motion_replay_hook = f2a_order_hook;
	server_local_test_rpc_done_hook = f2a_attack_probe;
	f2a_host_vx_at_attack = -1.0;
	server_local_test_update_once(sv);
	server_local_test_motion_replay_hook = NULL;
	server_local_test_rpc_done_hook = NULL;
	printf("  (F2a) multi: order");
	for(unsigned k = 0; k < f2a_order_n; k++)
		printf(" %d", f2a_order[k]);
	printf(" | host vx at attack %.9g (want %.9g) guest vx %.9g (want %.9g)\n",
		   f2a_host_vx_at_attack, wit_xz(wit_xz(h0)), g->vel_x, wit_xz(g0));
	assert(f2a_order_n == 3 && f2a_order[0] == 0 && f2a_order[1] == 1
		   && f2a_order[2] == 0);
	assert(f2a_host_vx_at_attack == wit_xz(wit_xz(h0)));
	assert(sv->player.vel_x == wit_xz(wit_xz(h0)) && g->vel_x == wit_xz(g0));
	assert(svin_credit_outstanding(0) == 0 && svin_credit_outstanding(1) == 0);
	puts("  (F2a) zero packets: no evolution; 2 host + 1 guest samples step once each at their FIFO slots, before the following attack OK");
	f1_guest_remove(sv, g);
	svin_set_split_accept(false);
	svin_set_local_motion(false);
	w_close();
}

static void f2a_run_all(void) {
	const char* only = getenv("CW_F2A_CASE");
	if(!only || !strcmp(only, "mob0")) f2a_mob_attack_run(0, 0);
	if(!only || !strcmp(only, "mob1")) f2a_mob_attack_run(1, 0);
	if(!only || !strcmp(only, "mob0w1")) f2a_mob_attack_run(0, 1);
	if(!only || !strcmp(only, "mob1w1")) f2a_mob_attack_run(1, 1);
	if(!only || !strcmp(only, "boat")) f2a_boat_break_run(0, 0);
	if(!only || !strcmp(only, "boatg")) f2a_boat_break_run(1, 0);
	if(!only || !strcmp(only, "boatw1")) f2a_boat_break_run(0, 1);
	if(!only || !strcmp(only, "boatgw1")) f2a_boat_break_run(1, 1);
	if(!only || !strcmp(only, "place")) f2a_place_tick_run(0);
	if(!only || !strcmp(only, "placew1")) f2a_place_tick_run(1);
	if(!only || !strcmp(only, "pause2")) f2a_pause_repeat_run();
	if(!only || !strcmp(only, "spill")) f2a_spill_run();
	if(!only || !strcmp(only, "respawn")) f2a_respawn_run();
	if(!only || !strcmp(only, "pause")) f2a_pause_fifo_run();
	if(!only || !strcmp(only, "multi")) f2a_multi_run();
	puts("  (F2a) world -> network(FIFO) -> tracker phase cases OK");
}

int main(void) {
	assert(clin_init());
	assert(svin_init());
	clin_test_set_consume_observer(record_applied, NULL);
	/* Regression: an unloaded-client fixture must discard every publication,
	 * including output deferred beyond the sixteen inbox tokens. No server
	 * or client entity dictionary exists yet. */
	for(unsigned i = 0; i < 48; i++) {
		struct client_rpc call = {.type = CRPC_ENTITY_DESTROY,
			.payload.entity_destroy.entity_id = 1000 + i};
		clin_rpc_send(&call);
	}
	/* 16 take the tokens; the other 32 are deferred RECORDS. Since R10
	 * (160239fd8) ENTITY_DESTROY rides the pack envelope, so they occupy 2
	 * ring entries (19 + 13), not 32: count records, not entries. */
	uint32_t pending, deferred;
	clin_rpc_pending_stats(&pending, NULL, &deferred, NULL);
	assert(deferred == 32 && pending >= 1 && pending <= 32);
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	output_quiesce();
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	assert(pending == 0);
	assert(g_applied_messages == 0);
	puts("  headless output: 48 destroy messages discarded, pending empty OK");
	blocks_init();
	items_init();
	if(getenv("CW_F2A_ONLY")) { /* iterate on the V6 F2a cases alone */
		setvbuf(stdout, NULL, _IONBF, 0);
		f2a_run_all();
		g_drain_run = 0;
		thread_join(&drain);
		puts("test_explosion_beta_golden: F2A ONLY OK");
		return 0;
	}
	if(getenv("CW_F1_ONLY")) { /* iterate on the V6 F1 cases alone */
		static const int f1_only_counts[] = {32, 33, 34, 40, 101, 250};
		for(size_t k = 0; k < sizeof f1_only_counts / sizeof f1_only_counts[0]; k++)
			f1_producer_host_run(f1_only_counts[k]);
		f1_producer_guest_run(40);
		f1_pause_full_run();
		f1_guest_pause_run();
		f1_backpressure_poses_run();
		g_drain_run = 0;
		thread_join(&drain);
		puts("test_explosion_beta_golden: F1 ONLY OK");
		return 0;
	}

	/* ---- (e) resistance pins: the exact float chain per Block.java line --- */
#ifdef HAVE_EXPLOSION_MODEL
	assert(feq(explosion_block_resistance(BLOCK_STONE), (10.0F * 3.0F) / 5.0F));
	assert(feq(explosion_block_resistance(BLOCK_DIRT), (0.5F * 5.0F) / 5.0F));
	assert(feq(explosion_block_resistance(BLOCK_OBSIDIAN), (2000.0F * 3.0F) / 5.0F));
	assert(feq(explosion_block_resistance(BLOCK_BEDROCK), (6000000.0F * 3.0F) / 5.0F));
	assert(feq(explosion_block_resistance(85), (5.0F * 3.0F) / 5.0F)); /* fence */
	assert(feq(explosion_block_resistance(BLOCK_WATER_STILL), (100.0F * 5.0F) / 5.0F));
	assert(feq(explosion_block_resistance(BLOCK_TNT), 0.0F));
	assert(feq(explosion_block_resistance(BLOCK_GLASS), (0.3F * 5.0F) / 5.0F));
	assert(feq(explosion_block_resistance(BLOCK_PLANKS), (5.0F * 3.0F) / 5.0F));
	assert(feq(explosion_block_resistance(53), (5.0F * 3.0F) / 5.0F)); /* wood stairs */
	assert(feq(explosion_block_resistance(BLOCK_AIR), 0.0F));
	assert(feq(explosion_block_resistance(36), 0.0F)); /* piston moving, hardness -1 */
	puts("  (e) getExplosionResistance setter chain: stone 6, dirt 0.5, obsidian 1200, bedrock 3.6e6 OK");
#endif

	int n = snprintf(g_tmp_world, sizeof(g_tmp_world),
					 "test_explosion_beta_golden_tmp_%d", (int)GETPID());
	assert(n > 0 && (size_t)n < sizeof(g_tmp_world));
	MKDIR(TMP_WORLD);
	MKDIR(tmp_path("/region"));
	remove(tmp_path("/region/r.0.0.mcr"));
	remove(tmp_path("/level.dat"));
	remove(tmp_path("/level.dat_new"));
	remove(tmp_path("/level.dat_old"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	struct server_local s;
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s; /* the erase-save's entity gather runs through this */
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = UINT32_C(0xB1A57B1D);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 200.5;
	s.player.y = 4.5;
	s.player.z = 200.5; /* far outside every blast box below */

	/* -2..6: the entity tick only runs an entity whose +-32-block area is
	 * resident (server_local_tick_entities), and (d) ticks an item at x ~ 8. */
	for(int cx = -2; cx <= 8; cx++)
		for(int cz = -2; cz <= 8; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}

	/* F4-RNG runs LAST on this fixture (it leaves a respawned body and its
	 * own drops behind); CW_F4RNG_ONLY runs it alone, first. */
	if(getenv("CW_F4RNG_R3_ONLY")) {
		const char* mode = getenv("CW_F4RNG_R3_ONLY");
		if(!strcmp(mode,"bed")) bed_float_center_run(&s);
		else if(!strcmp(mode,"support")) f4rng_r3_support(&s);
		else if(!strcmp(mode,"harvest")) f4rng_r3_harvest(&s);
		else if(!strcmp(mode,"air")) f4rng_r3_air(&s);
		else if(!strcmp(mode,"death")) f4rng_r3_death(&s);
		else f4rng_drop_cases(&s, r3_cases, sizeof r3_cases / sizeof r3_cases[0]);
		g_drain_run = 0;
		thread_join(&drain);
		return 0;
	}
	if(getenv("CW_F4RNG_R2_ONLY")) {
		f4rng_r2_drops(&s);
		g_drain_run = 0;
		thread_join(&drain);
		return 0;
	}
	if(getenv("CW_F4RNG_ONLY")) {
		f4rng_run(&s);
		g_drain_run = 0;
		thread_join(&drain);
		puts("test_explosion_beta_golden: F4-RNG ONLY OK");
		return 0;
	}

	/* ---- (a) block destruction vs the hand transcription ---------------- */
	for(int x = CX - 16; x <= CX + 16; x++)
		for(int z = CZ - 16; z <= CZ + 16; z++)
			for(int y = FILL_MIN_Y; y <= FILL_MAX_Y; y++)
				set_block(&s, x, y, z, x == WALL_X ? BLOCK_STONE : BLOCK_DIRT);
	struct block_data b;
	for(int x = WIN_MIN_X; x <= WIN_MAX_X; x++)
		for(int y = WIN_MIN_Y; y <= WIN_MAX_Y; y++)
			for(int z = WIN_MIN_Z; z <= WIN_MAX_Z; z++) {
				assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
				g_pre[widx(x, y, z)] = b.type;
			}
	/* F4-RNG: the rays draw the exploding world World.rand (java.util.Random,
	 * anchored to the JVM in F4.0) -- no longer a copy of the session xorshift. */
	struct java_random rcopy = s.worlds[0].rand;
	transcribe_doExplosionA(&rcopy);
	server_world_begin_update(&s.worlds[0]);
	server_world_explode(&s.worlds[0], (vec3) {CX + 0.5F, (float)CY, CZ + 0.5F}, POWER);
	print_cost("(a) power 4, dirt+stone monolith");
	size_t expected = 0, broken = 0, mismatches = 0;
	for(int x = WIN_MIN_X; x <= WIN_MAX_X; x++)
		for(int y = WIN_MIN_Y; y <= WIN_MAX_Y; y++)
			for(int z = WIN_MIN_Z; z <= WIN_MAX_Z; z++) {
				assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
				uint8_t pre = g_pre[widx(x, y, z)];
				bool want_air = pre == BLOCK_AIR || g_marked[widx(x, y, z)];
				if(pre != BLOCK_AIR && g_marked[widx(x, y, z)])
					expected++;
				if(pre != BLOCK_AIR && b.type == BLOCK_AIR)
					broken++;
				if((b.type == BLOCK_AIR) != want_air)
					mismatches++;
			}
	printf("  (a) transcription expects %zu carved cells; blast carved %zu; "
		   "%zu cell mismatches\n", expected, broken, mismatches);
	fflush(stdout);
	assert(expected > 0);
	assert(mismatches == 0);
	assert(broken == expected);
	/* Stone (resistance 6: 1.89 per step) must survive far more than dirt
	 * (0.5): the wall plane keeps most of its cells inside the blast reach. */
	{
		size_t stone_left = 0, stone_total = 0;
		for(int y = 8; y <= 19; y++)
			for(int z = CZ - 5; z <= CZ + 5; z++) {
				assert(server_world_get_block(&s.worlds[0], WALL_X, y, z, &b));
				stone_total++;
				if(b.type == BLOCK_STONE)
					stone_left++;
			}
		printf("  (a) stone plane: %zu of %zu cells within the reach survived\n",
			   stone_left, stone_total);
		assert(stone_left * 2 > stone_total);
	}
	puts("  (a) doExplosionA grid rays + resistance: byte-exact vs hand transcription OK");

	/* ---- (b)/(c)/(d) entity pass, in open air over the superflat ground -- */
	const int EX = 8, EZ = 40; /* chunk 0/2 column, untouched by (a) */
	const float mob_y = 4.0F; /* feet on the y=3 superflat top */
	vec3 mpos = {EX + 6.5F, mob_y, EZ + 0.5F};
	struct entity* mob = server_local_spawn_monster_at(mpos, MONSTER_ZOMBIE, &s);
	assert(mob && mob->health == 20);
	glm_vec3_zero(mob->vel);
	test_mob_sync(mob); /* motion units S2: the fixture pins the doubles */
	server_local_refresh_entity_membership(&s, mob);
	assert(mob->added_to_chunk);
	vec3 center = {mob->pos[0] - 6.0F, mob->pos[1], mob->pos[2]};

	/* (b) open air: density 1 -> damage 11, impulse +0.25 along +x. */
	server_world_begin_update(&s.worlds[0]);
	server_world_explode(&s.worlds[0], center, POWER);
	print_cost("(b) power 4, open air, one mob");
	printf("  (b) mob: health %d vel (%.4f, %.4f, %.4f)\n", (int)mob->health,
		   (double)mob->vel[0], (double)mob->vel[1], (double)mob->vel[2]);
	fflush(stdout);
	assert(mob->health == 20 - 11);
	assert(feq(mob->vel[0], 0.25F));
	assert(feq(mob->vel[1], 0.0F) && feq(mob->vel[2], 0.0F));
	assert(mob->data.monster.knockback_time > 0); /* impulse carries past the Wii cap */
	puts("  (b) mob 6 blocks away, exposed: damage 11 + impulse 0.25 OK");

	/* (c) a stone wall between: density 0 -> damage 1 (the +1 floor), no impulse. */
	mob->health = 20;
	mob->data.monster.hurt_resist_time = 0;
	mob->data.monster.last_damage = 0;
	mob->data.monster.knockback_time = 0;
	glm_vec3_zero(mob->vel);
	test_mob_sync(mob); /* motion units S2: the fixture pins the doubles */
	for(int y = 4; y <= 9; y++)
		for(int z = EZ - 2; z <= EZ + 3; z++)
			set_block(&s, EX + 3, y, z, BLOCK_STONE);
	server_world_begin_update(&s.worlds[0]);
	server_world_explode(&s.worlds[0], center, POWER);
	print_cost("(c) power 4, wall, one mob");
	printf("  (c) mob: health %d vel (%.4f, %.4f, %.4f)\n", (int)mob->health,
		   (double)mob->vel[0], (double)mob->vel[1], (double)mob->vel[2]);
	fflush(stdout);
	assert(mob->health == 20 - 1);
	assert(feq(mob->vel[0], 0.0F) && feq(mob->vel[1], 0.0F) && feq(mob->vel[2], 0.0F));
	/* G2b (4): no impulse -> the Wii-cap carry gate is NOT armed; the mob keeps steering. */
	assert(mob->data.monster.knockback_time == 0);
	puts("  (c) mob behind a stone wall: exposure 0 -> damage 1, no impulse, steering kept OK");

	/* (d) items: 7.5 blocks -> pushed 0.0625 and alive (3 < 5); 6 blocks -> dead. */
	for(int y = 4; y <= 9; y++)
		for(int z = EZ - 2; z <= EZ + 3; z++) {
			/* the (c) blast carved part of the wall already: clear only what is left */
			assert(server_world_get_block(&s.worlds[0], EX + 3, y, z, &b));
			if(b.type != BLOCK_AIR)
				set_block(&s, EX + 3, y, z, BLOCK_AIR);
		}
	{ /* the mob leaves the fixture: (d) is items only */
		uint32_t mob_id = mob->id;
		dict_entity_erase(s.entities, mob_id);
		free(mob);
		mob = NULL;
	}
	/* A resting EntityItem (setSize .25, yOffset = height/2) has posY = box
	 * bottom + 0.125; centre the blast on that posY so the density lattice's
	 * bottom row (y = 4.0, the ground surface) and every ray stay in air. */
	vec3 dcenter = {center[0], 4.125F, center[2]};
	struct item_data stack = {.id = BLOCK_DIRT, .durability = 0, .count = 1};
	struct entity* far_item = server_local_spawn_item(
		(vec3) {dcenter[0], dcenter[1], dcenter[2] + 7.5F}, &stack, false, &s);
	struct entity* near_item = server_local_spawn_item(
		(vec3) {dcenter[0], dcenter[1], dcenter[2] - 6.0F}, &stack, false, &s);
	assert(far_item && near_item && far_item->health == 5 && near_item->health == 5);
	glm_vec3_copy((vec3) {dcenter[0], dcenter[1], dcenter[2] + 7.5F}, far_item->pos);
	glm_vec3_copy((vec3) {dcenter[0], dcenter[1], dcenter[2] - 6.0F}, near_item->pos);
	glm_vec3_zero(far_item->vel);
	glm_vec3_zero(near_item->vel);
	server_local_refresh_entity_membership(&s, far_item);
	server_local_refresh_entity_membership(&s, near_item);
	uint32_t near_id = near_item->id;
	server_world_begin_update(&s.worlds[0]);
	server_world_explode(&s.worlds[0], dcenter, POWER);
	print_cost("(d) power 4, two items");
	printf("  (d) far item: health %d vel_z %.5f; near item: health %d\n",
		   (int)far_item->health, (double)far_item->vel[2],
		   (int)near_item->health);
	fflush(stdout);
	assert(far_item->health == 5 - 3);
	assert(feq(far_item->vel[2], 0.0625F));
	assert(feq(far_item->vel[0], 0.0F));
	assert(near_item->health <= 0);
	/* EntityItem.setEntityDead: its own tick removes it (entity_item.c hook). */
	assert(dict_entity_get(s.entities, near_id) != NULL);
	s.player.has_pos = false; /* keep the entity tick off the far-away player */
	assert(server_local_test_tick_entities(&s));
	assert(dict_entity_get(s.entities, near_id) == NULL);
	puts("  (d) item 7.5 blocks: pushed 0.0625 + damage 3; item 6 blocks: killed and removed OK");

	/* ---- (g) partial occlusion: density computed independently ------------
	 * Zombie box 0.6x1.8 -> func_675_a lattice 3x5x3 = 45 sample rays (steps
	 * 1/(2*0.6+1) and 1/(2*1.8+1)); y rows at feet+0, .391, .783, 1.174,
	 * 1.565. Centre at mid-height (feet+0.9) 6 blocks -x; a one-block-tall
	 * stone wall at cell y=5 three blocks from the centre. The two top rows
	 * (5.174, 5.565) stay above y=5 across the wall's x span (their y at the
	 * wall's near face is 5.02..5.31) -> blocked; the three lower rows never
	 * reach y=5 -> clear. density = 27/45 = 0.6 exactly. */
	{
		const int PX = 8, PZ = 8;
		vec3 ppos = {PX + 6.5F, 4.0F, PZ + 0.5F};
		struct entity* pm = server_local_spawn_monster_at(ppos, MONSTER_ZOMBIE, &s);
		assert(pm && pm->health == 20);
		glm_vec3_zero(pm->vel);
		test_mob_sync(pm); /* motion units S2 */
		server_local_refresh_entity_membership(&s, pm);
		for(int z = PZ - 1; z <= PZ + 1; z++)
			set_block(&s, PX + 3, 5, z, BLOCK_STONE);
		vec3 pc = {PX + 0.5F, 4.0F + 0.9F, PZ + 0.5F};
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], pc, POWER);
		double dx = (double)pm->pos[0] - (double)pc[0];
		double dy = (double)pm->pos[1] - (double)pc[1];
		double dist = (double)(float)sqrt(dx * dx + dy * dy);
		double var13 = dist / 8.0;
		double var41 = (1.0 - var13) * (double)0.6F;
		int want = (int)((var41 * var41 + var41) / 2.0 * 8.0 * 8.0 + 1.0);
		printf("  (g) partial occlusion: health %d (want %d) vel (%.5f, %.5f) want (%.5f, %.5f)\n",
			   (int)pm->health, 20 - want, (double)pm->vel[0], (double)pm->vel[1],
			   dx / dist * var41, dy / dist * var41);
		fflush(stdout);
		/* dist 6.067, var13 .7584, var41 .14496 -> (.02101+.14496)/2*64 + 1 = 6.31 -> 6 */
		assert(want == 6);
		assert(pm->health == 20 - want);
		assert(fabs((double)pm->vel[0] - dx / dist * var41) < 1.0e-5);
		assert(fabs((double)pm->vel[1] - dy / dist * var41) < 1.0e-5);
		assert(feq(pm->vel[2], 0.0F));
		puts("  (g) partial occlusion: density 27/45 -> damage 6 + scaled impulse OK");
	}

	/* ---- (h) creeper exploder: excluded from its own blast; knockBack ----- */
	{
		vec3 cpos = {60.5F, 4.0F, 8.5F};
		struct entity* creeper = server_local_spawn_monster_at(cpos, MONSTER_CREEPER, &s);
		vec3 zpos = {64.5F, 4.0F, 8.5F};
		struct entity* zom = server_local_spawn_monster_at(zpos, MONSTER_ZOMBIE, &s);
		assert(creeper && zom && creeper->health == 20 && zom->health == 20);
		glm_vec3_zero(creeper->vel);
		test_mob_sync(creeper); /* motion units S2 */
		glm_vec3_zero(zom->vel);
		test_mob_sync(zom); /* motion units S2 */
		server_local_refresh_entity_membership(&s, creeper);
		server_local_refresh_entity_membership(&s, zom);
		vec3 boom = {creeper->pos[0], creeper->pos[1], creeper->pos[2]};
		server_world_begin_update(&s.worlds[0]);
		assert(server_world_try_explode_by(&s.worlds[0], creeper, boom, 3.0F));
		/* the creeper: not in its own entity pass */
		assert(creeper->health == 20);
		assert(feq(creeper->vel[0], 0.0F) && feq(creeper->vel[1], 0.0F) && feq(creeper->vel[2], 0.0F));
		/* the zombie 4 blocks +x, size2 6: var13 2/3, var41 1/3 -> damage
		 * (int)((1/9+1/3)/2*48+1) = 11, FRESH -> knockBack away from the
		 * creeper (0*0.5 + 1*0.4) then the blast impulse +1/3 -> 0.7333. */
		printf("  (h) creeper blast: zombie health %d vel (%.5f, %.5f, %.5f) kb=%d\n",
			   (int)zom->health, (double)zom->vel[0], (double)zom->vel[1],
			   (double)zom->vel[2], zom->data.monster.knockback_time);
		fflush(stdout);
		assert(zom->health == 20 - 11);
		assert(fabs((double)zom->vel[0] - (0.4 + 1.0 / 3.0)) < 1.0e-4);
		assert(feq(zom->vel[2], 0.0F));
		assert(zom->data.monster.knockback_time == MOB_KNOCKBACK_TICKS);
		puts("  (h) creeper exploder excluded; zombie knockBack 0.4 + impulse 1/3 OK");
	}

	/* ---- (i) wolf halving with a creeper exploder ------------------------
	 * EntityWolf.attackEntityFrom: var1 != null && !player && !arrow ->
	 * (dmg+1)/2; the creeper is exactly that attacker: 11 -> 6. */
	{
		vec3 cpos = {8.5F, 4.0F, 72.5F};
		struct entity* creeper = server_local_spawn_monster_at(cpos, MONSTER_CREEPER, &s);
		vec3 wpos = {8.5F, 4.0F, 76.5F};
		struct entity* wolf = server_local_spawn_monster_at(wpos, MONSTER_WOLF, &s);
		assert(creeper && wolf);
		short wolf_hp = wolf->health;
		glm_vec3_zero(wolf->vel);
		test_mob_sync(wolf); /* motion units S2 */
		server_local_refresh_entity_membership(&s, creeper);
		server_local_refresh_entity_membership(&s, wolf);
		vec3 boom = {creeper->pos[0], creeper->pos[1], creeper->pos[2]};
		server_world_begin_update(&s.worlds[0]);
		assert(server_world_try_explode_by(&s.worlds[0], creeper, boom, 3.0F));
		printf("  (i) wolf: health %d -> %d\n", (int)wolf_hp, (int)wolf->health);
		fflush(stdout);
		assert(wolf->health == wolf_hp - (11 + 1) / 2);
		puts("  (i) wolf takes (11+1)/2 = 6 from a creeper blast OK");
	}

	/* ---- (j) player: x8 boundary conversion + impulse ---------------------
	 * Beta EntityPlayer posY is eye level; feet on y=4 -> y = 5.62. Blast 6
	 * blocks -x at eye height, open air: 11 half-hearts -> 88 engine units. */
	{
		s.player.has_pos = true;
		s.player.x = 72.5;
		s.player.y = 4.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 72.5;
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.hurt_resist_time = 0;
		s.player.last_damage = 0;
		s.player.vel_x = 0.0F;
		s.player.vel_z = 0.0F;
		vec3 pc = {66.5F, (float)s.player.y, 72.5F};
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], pc, POWER);
		printf("  (j) player: health %d vel_x %.4f\n", (int)s.player.health,
			   (double)s.player.vel_x);
		fflush(stdout);
		assert(s.player.health == MAX_PLAYER_HEALTH - 11 * 8);
		assert(feq(s.player.vel_x, 0.25F));
		puts("  (j) player 6 blocks: 11 half-hearts x8 = 88 engine units + impulse 0.25 OK");
	}

	/* ---- (k) vehicles: saturating damage, ONE deferred break -------------- */
	{
		struct entity* cart = server_local_spawn_minecart((vec3) {72.5F, 4.0F, 40.5F}, &s);
		struct entity* boat = server_local_spawn_boat((vec3) {40.5F, 4.0F, 80.5F}, &s);
		assert(cart && boat);
		uint32_t cart_id = cart->id, boat_id = boat->id;
		server_local_refresh_entity_membership(&s, cart);
		server_local_refresh_entity_membership(&s, boat);
		size_t before = dict_entity_size(s.entities);
		for(int i = 0; i < 60; i++) {
			server_world_begin_update(&s.worlds[0]);
			server_world_explode(&s.worlds[0], (vec3) {cart->pos[0] + 0.5F, cart->pos[1], cart->pos[2]}, POWER);
			server_world_explode(&s.worlds[0], (vec3) {boat->pos[0] + 0.5F, boat->pos[1], boat->pos[2]}, POWER);
			assert(cart->data.minecart.damage_taken > 0);
			assert(boat->data.boat.damage_taken > 0);
		}
		printf("  (k) after 60 blasts: cart damage %d boat damage %d (INT16_MAX %d)\n",
			   (int)cart->data.minecart.damage_taken, (int)boat->data.boat.damage_taken,
			   (int)INT16_MAX);
		fflush(stdout);
		assert(cart->data.minecart.damage_taken == INT16_MAX);
		assert(boat->data.boat.damage_taken == INT16_MAX);
		assert(cart->data.minecart.blast_break_pending && boat->data.boat.blast_break_pending);
		assert(dict_entity_get(s.entities, cart_id) && dict_entity_get(s.entities, boat_id));
		(void)before;
		assert(server_local_test_tick_entities(&s));
		assert(dict_entity_get(s.entities, cart_id) == NULL);
		assert(dict_entity_get(s.entities, boat_id) == NULL);
		size_t cart_items = 0;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(e && e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_MINECART)
				cart_items++;
		}
		assert(cart_items == 1);
		assert(server_local_test_tick_entities(&s)); /* a second tick cannot break again */
		puts("  (k) 60 blasts: counters saturate at INT16_MAX, one deferred break each, one cart drop OK");
	}

	/* ---- (l) isFlaming order: fire lit at the end of doExplosionA, removed
	 * by doExplosionB's id>0 sweep -- none of the blast's own fire survives. */
	{
		vec3 fc = {100.5F, 4.5F, 8.5F};
		server_world_begin_update(&s.worlds[0]);
		server_world_explode_flaming(&s.worlds[0], fc, 1.0F);
		int fire = 0;
		for(int x = 96; x <= 105; x++)
			for(int y = 3; y <= 9; y++)
				for(int z = 4; z <= 13; z++) {
					assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
					if(b.type == BLOCK_FIRE)
						fire++;
				}
		printf("  (l) flaming power-1 blast over the superflat top: %d fire cells\n", fire);
		assert(fire == 0);
		puts("  (l) isFlaming: doExplosionA lights, doExplosionB removes (decompile order) OK");
	}

	/* ---- (n) OOM before the entity pass: nothing damaged, lit or written --
	 * `broken` is the FIRST staging allocation (server_world.c), so failing
	 * exactly one blast malloc fails it: the blast must abort before the
	 * entity pass, the isFlaming placement and pass 1. */
	{
		vec3 opos = {104.5F, 4.0F, 60.5F};
		struct entity* om = server_local_spawn_monster_at(opos, MONSTER_ZOMBIE, &s);
		assert(om && om->health == 20);
		glm_vec3_zero(om->vel);
		test_mob_sync(om); /* motion units S2 */
		server_local_refresh_entity_membership(&s, om);
		vec3 oc = {om->pos[0] - 3.0F, 4.5F, om->pos[2]};
		struct block_data g0, g1;
		assert(server_world_get_block(&s.worlds[0], 101, 3, 60, &g0) && g0.type != BLOCK_AIR);
		server_world_test_fail_blast_mallocs(1);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode_flaming(&s.worlds[0], oc, POWER);
		assert(om->health == 20 && feq(om->vel[0], 0.0F));
		assert(server_world_get_block(&s.worlds[0], 101, 3, 60, &g1) && g1.type == g0.type);
		int fire = 0;
		for(int x = 96; x <= 110; x++)
			for(int y = 3; y <= 9; y++)
				for(int z = 54; z <= 66; z++) {
					assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
					if(b.type == BLOCK_FIRE)
						fire++;
				}
		assert(fire == 0);
		puts("  (n) staging OOM on the `broken` list: mob untouched, no fire, no block written OK");
	}

	/* ---- (o) isFlaming light path equivalence: per-cell flood (World.
	 * setBlockWithNotify) vs raw write + the batched pass-2 flood (T1) ------
	 * The SAME stone fixture twice (restored between the passes), same seeds
	 * for rand_src, World.rand and ExplosionRNG; every block id/metadata/sky/
	 * torch value and the whole neighbour-notify sequence (observed on stone)
	 * must match cell for cell. F4-RNG: the destroyed-cell visit order is the
	 * OpenJDK 6 HashSet order of ABSOLUTE coordinates (ASTRA-RNG2-RULING 14),
	 * so a second fixture at another z would legitimately visit -- and notify
	 * -- in another order; the comparison therefore reruns the identical
	 * coordinates instead of translating them. */
	{
		const int OX = 104, OZA = 20;
		enum { ON = 16 * 11 * 16 };
		static struct block_data o_pre[ON], o_post0[ON];
		void (*saved_hook)(struct server_local*, struct block_info*)
			= blocks[BLOCK_STONE]->onNeighbourBlockChange;
		for(int x = OX - 8; x <= OX + 7; x++)
			for(int z = OZA - 8; z <= OZA + 7; z++)
				for(int y = 2; y <= 4; y++)
					set_block(&s, x, y, z, BLOCK_STONE);
		server_world_light_drain(&s.worlds[0]);
		size_t oi = 0;
		for(int x = OX - 8; x <= OX + 7; x++)
			for(int y = 0; y <= 10; y++)
				for(int dz = -8; dz <= 7; dz++)
					assert(server_world_get_block(&s.worlds[0], x, y, OZA + dz, &o_pre[oi++]));
		struct server_world_explosion_cost cost[2];
		for(int pass = 0; pass < 2; pass++) {
			if(pass == 1) {
				/* restore the pre-blast fixture (identical writes are no-ops) */
				oi = 0;
				for(int x = OX - 8; x <= OX + 7; x++)
					for(int y = 0; y <= 10; y++)
						for(int dz = -8; dz <= 7; dz++, oi++) {
							struct block_data cur;
							assert(server_world_get_block(&s.worlds[0], x, y, OZA + dz, &cur));
							if(cur.type != o_pre[oi].type || cur.metadata != o_pre[oi].metadata)
								assert(server_world_set_block(&s.worlds[0], x, y, OZA + dz,
									(struct block_data) {.type = o_pre[oi].type,
														 .metadata = o_pre[oi].metadata}));
						}
				server_world_light_drain(&s.worlds[0]);
				oi = 0;
				for(int x = OX - 8; x <= OX + 7; x++)
					for(int y = 0; y <= 10; y++)
						for(int dz = -8; dz <= 7; dz++, oi++) {
							struct block_data cur;
							assert(server_world_get_block(&s.worlds[0], x, y, OZA + dz, &cur));
							assert(cur.type == o_pre[oi].type && cur.metadata == o_pre[oi].metadata
								   && cur.sky_light == o_pre[oi].sky_light
								   && cur.torch_light == o_pre[oi].torch_light);
						}
			}
			note_slot = pass;
			note_oz = OZA;
			note_n[pass] = 0;
			blocks[BLOCK_STONE]->onNeighbourBlockChange = observe;
			server_world_test_set_fire_flood_path(pass == 0);
			server_world_test_seed_fire_rng(UINT32_C(0xF1A3E5C7));
			s.rand_src.seed = UINT32_C(0x0EA7C0DE);
			jrand_set_seed(&s.worlds[0].rand, 0x0EA7C0DE); /* F4-RNG: World.rand */
			server_world_begin_update(&s.worlds[0]);
			server_world_explode_flaming(&s.worlds[0], (vec3) {OX + 0.5F, 5.5F, OZA + 0.5F}, 2.0F);
			server_world_explosion_last_cost(&cost[pass]);
			blocks[BLOCK_STONE]->onNeighbourBlockChange = saved_hook;
			server_world_light_drain(&s.worlds[0]);
			if(pass == 0) {
				oi = 0;
				for(int x = OX - 8; x <= OX + 7; x++)
					for(int y = 0; y <= 10; y++)
						for(int dz = -8; dz <= 7; dz++)
							assert(server_world_get_block(&s.worlds[0], x, y, OZA + dz, &o_post0[oi++]));
			}
		}
		server_world_test_set_fire_flood_path(false);
		printf("  (o) flood path lit %u cells, raw path lit %u; notifies %zu vs %zu\n",
			   cost[0].fire_lit, cost[1].fire_lit, note_n[0], note_n[1]);
		fflush(stdout);
		assert(cost[0].fire_lit > 0 && cost[0].fire_lit == cost[1].fire_lit);
		assert(cost[0].cells == cost[1].cells && cost[0].broken == cost[1].broken);
		assert(note_n[0] == note_n[1]);
		for(size_t i = 0; i < note_n[0]; i++)
			for(int k = 0; k < 4; k++)
				assert(notes[0][i][k] == notes[1][i][k]);
		size_t cells_cmp = 0;
		oi = 0;
		for(int x = OX - 8; x <= OX + 7; x++)
			for(int y = 0; y <= 10; y++)
				for(int dz = -8; dz <= 7; dz++, oi++) {
					struct block_data bb;
					const struct block_data* ba = &o_post0[oi];
					assert(server_world_get_block(&s.worlds[0], x, y, OZA + dz, &bb));
					assert(ba->type == bb.type && ba->metadata == bb.metadata);
					assert(ba->sky_light == bb.sky_light && ba->torch_light == bb.torch_light);
					cells_cmp++;
				}
		printf("  (o) %zu cells identical (id/meta/sky/torch), %zu notifies identical\n",
			   cells_cmp, note_n[0]);
		puts("  (o) raw fire write + batched flood == per-cell flood path (T1 equivalence) OK");
		fflush(stdout);
	}

	/* ---- (p) vehicle break latch survives a save/reload in the window ------
	 * One blast pushes the counter past 40; the chunk is saved + retired and
	 * reloaded BEFORE the vehicle's tick; the restored vehicle (latch bool
	 * gone, CaveWiiBlastDamage tag kept) still breaks exactly once. */
	{
		struct entity* cart = server_local_spawn_minecart((vec3) {72.5F, 4.0F, 8.5F}, &s);
		struct entity* boat = server_local_spawn_boat((vec3) {72.5F, 4.0F, 56.5F}, &s);
		assert(cart && boat);
		server_local_refresh_entity_membership(&s, cart);
		server_local_refresh_entity_membership(&s, boat);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {cart->pos[0] + 0.5F, cart->pos[1], cart->pos[2]}, POWER);
		server_world_explode(&s.worlds[0], (vec3) {boat->pos[0] + 0.5F, boat->pos[1], boat->pos[2]}, POWER);
		assert(cart->data.minecart.damage_taken > 40 && cart->data.minecart.blast_break_pending);
		assert(boat->data.boat.damage_taken > 40 && boat->data.boat.blast_break_pending);
		int16_t cart_dmg = cart->data.minecart.damage_taken;
		int16_t boat_dmg = boat->data.boat.damage_taken;
		uint32_t cart_id = cart->id, boat_id = boat->id;
		/* quiesce-0926: prove the inbox empty, then one strict attempt each
		 * (was a 500 x 2 ms retry racing the async drain). */
		server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
		output_quiesce();
		bool saved_c = server_world_save_chunk(&s.worlds[0], true, 4, 0);
		output_quiesce();
		bool saved_b = server_world_save_chunk(&s.worlds[0], true, 4, 3);
		assert(saved_c && saved_b);
		assert(dict_entity_get(s.entities, cart_id) == NULL); /* retired with the chunk */
		assert(dict_entity_get(s.entities, boat_id) == NULL);
		assert(!server_world_is_chunk_loaded(&s.worlds[0], 4, 0));
		assert(!server_world_is_chunk_loaded(&s.worlds[0], 4, 3));
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&s.worlds[0], 4, 0, &sc) && sc);
		assert(server_world_load_chunk(&s.worlds[0], 4, 3, &sc) && sc);
		struct entity* rcart = NULL;
		struct entity* rboat = NULL;
		size_t cart_items = 0;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(!e)
				continue;
			if(e->type == ENTITY_MINECART && fabsf(e->pos[2] - 8.5F) < 4.0F)
				rcart = e;
			if(e->type == ENTITY_BOAT && fabsf(e->pos[2] - 56.5F) < 4.0F)
				rboat = e;
			if(e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_MINECART)
				cart_items++;
		}
		assert(rcart && rboat);
		printf("  (p) reloaded: cart damage %d (saved %d) latch %d; boat damage %d (saved %d) latch %d\n",
			   (int)rcart->data.minecart.damage_taken, (int)cart_dmg,
			   (int)rcart->data.minecart.blast_break_pending,
			   (int)rboat->data.boat.damage_taken, (int)boat_dmg,
			   (int)rboat->data.boat.blast_break_pending);
		fflush(stdout);
		assert(rcart->data.minecart.damage_taken == cart_dmg && !rcart->data.minecart.blast_break_pending);
		assert(rboat->data.boat.damage_taken == boat_dmg && !rboat->data.boat.blast_break_pending);
		uint32_t rcart_id = rcart->id, rboat_id = rboat->id;
		assert(server_local_test_tick_entities(&s));
		assert(dict_entity_get(s.entities, rcart_id) == NULL);
		assert(dict_entity_get(s.entities, rboat_id) == NULL);
		size_t cart_items_after = 0;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(e && e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_MINECART)
				cart_items_after++;
		}
		assert(cart_items_after == cart_items + 1);
		assert(server_local_test_tick_entities(&s));
		puts("  (p) blast -> save/retire -> reload -> tick: each vehicle breaks exactly once OK");
	}

	/* ---- (q) ordinary punch damage does NOT survive a save (Beta) --------
	 * damage_taken <= 40 writes no CaveWiiBlastDamage tag, so the reloaded
	 * record reads the tag-absent default 0 -- the same path a pre-tag save
	 * takes. Only the blast-break window (p) rides along. */
	{
		struct entity* cart = server_local_spawn_minecart((vec3) {72.5F, 4.0F, 88.5F}, &s);
		struct entity* boat = server_local_spawn_boat((vec3) {88.5F, 4.0F, 88.5F}, &s);
		assert(cart && boat);
		server_local_refresh_entity_membership(&s, cart);
		server_local_refresh_entity_membership(&s, boat);
		cart->data.minecart.damage_taken = 20; /* two fist punches */
		boat->data.boat.damage_taken = 20;
		/* quiesce-0926: prove the inbox empty, then one strict attempt each
		 * (was a 500 x 2 ms retry racing the async drain). */
		output_quiesce();
		bool saved_c = server_world_save_chunk(&s.worlds[0], true, 4, 5);
		output_quiesce();
		bool saved_b = server_world_save_chunk(&s.worlds[0], true, 5, 5);
		assert(saved_c && saved_b);
		assert(!server_world_is_chunk_loaded(&s.worlds[0], 4, 5));
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&s.worlds[0], 4, 5, &sc) && sc);
		assert(server_world_load_chunk(&s.worlds[0], 5, 5, &sc) && sc);
		struct entity* rcart = NULL;
		struct entity* rboat = NULL;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(e && e->type == ENTITY_MINECART && fabsf(e->pos[2] - 88.5F) < 4.0F)
				rcart = e;
			if(e && e->type == ENTITY_BOAT && fabsf(e->pos[2] - 88.5F) < 4.0F)
				rboat = e;
		}
		assert(rcart && rboat);
		assert(rcart->data.minecart.damage_taken == 0 && !rcart->data.minecart.blast_break_pending);
		assert(rboat->data.boat.damage_taken == 0 && !rboat->data.boat.blast_break_pending);
		uint32_t rcart_id = rcart->id, rboat_id = rboat->id;
		assert(server_local_test_tick_entities(&s));
		assert(dict_entity_get(s.entities, rcart_id) && dict_entity_get(s.entities, rboat_id));
		puts("  (q) punch damage 20 -> save/reload -> 0, vehicle survives (no tag outside the window) OK");
	}

	/* ---- (v) G8-BLAST-VELOCITY: fresh-hit flag + tracker-phase Packet28 ---
	 * Unit level (direct calls, the tracker phase's two steps in its order:
	 * server_local_tick_actor_knockback = the onUpdateEntity motion step,
	 * then FLUSH_VEL). Production scheduling is section (w). Open air high
	 * above the superflat (density 1), the eye +5 x / +3 y / +2 z off the
	 * centre, so every blast has an X, Y AND Z impulse. Expected payloads come
	 * from the independent Beta transcription below (ASTRA-V-REVIEW (c)):
	 * moveEntityWithHeading(0,0) air branch, not on ground, no collision:
	 *   motionY = (motionY - 0.08D) * (double)0.98F; motionX/Z *= (double)0.91F
	 * RED on d045a93fa (immediate additive X/Z send, Y position shove) and on
	 * 2e3b93aa2 (client-sample Y, no gravity/drag, explosion-only flagging).
	 * CW_BLASTV_W_ONLY=1 skips to (w) so its own RED can be shown on a base
	 * where (v) already aborts. */
	if(!getenv("CW_BLASTV_W_ONLY")) {
		const vec3 vc = {20.5F, 40.0F, 130.5F};
		const double EYE_X = 25.5, EYE_Y = 43.0, EYE_Z = 132.5;
		double imp[3];
		const int vdmg = blast_terms(vc, EYE_X, EYE_Y, EYE_Z, 8.0, imp);
		printf("  (v) blast terms: damage %d impulse (%.6f, %.6f, %.6f)\n", vdmg,
			   imp[0], imp[1], imp[2]);
		assert(vdmg == 10); /* dist 6.164, var41 .22946 -> 10.03 -> 10 */
		assert(imp[0] > 0.1 && imp[1] > 0.05 && imp[2] > 0.05);
		const int vunits = vdmg * 8; /* half-hearts x8 = engine units */
		s.player.inventory.reply = vel_capture;
		s.player.inventory.reply_user = (void*)(intptr_t)0;
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.on_ground = false; /* air friction 0.91F in the step */
		s.player.x = EYE_X;
		s.player.y = EYE_Y;
		s.player.z = EYE_Z;
#define VRESET(h, hrt, ld, vx, my, vz)                                          \
	do {                                                                        \
		s.player.health = (h);                                                  \
		s.player.hurt_resist_time = (hrt);                                      \
		s.player.last_damage = (ld);                                            \
		s.player.vel_x = (vx);                                                  \
		MOTY_SET(s.player, (my));                                               \
		s.player.vel_y = 0.0F;                                                  \
		s.player.vel_z = (vz);                                                  \
		g_vel_n = 0;                                                            \
	} while(0)

		/* V1: fresh null-exploder blast, nonzero pre-motion: no message at the
		 * blast, no Y shove, server motion += XYZ (client sample vel_y
		 * untouched), ONE absolute message after the motion step. */
		VRESET(MAX_PLAYER_HEALTH, 0, 0, 0.05F, -0.0784, -0.02F);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		printf("  (v) V1 after blast: health %d vel_x %.6f motY %.6f vel_z %.6f y %.6f msgs %zu\n",
			   (int)s.player.health, (double)s.player.vel_x, MOTY(s.player),
			   (double)s.player.vel_z, s.player.y, g_vel_n);
		fflush(stdout);
		assert(s.player.health == MAX_PLAYER_HEALTH - vunits);
		assert(g_vel_n == 0);
		assert(s.player.y == EYE_Y && s.player.vel_y == 0.0F);
		const double v1x = addm(0.05F, imp[0]), v1z = addm(-0.02F, imp[2]);
		const double v1y = -0.0784 + imp[1];
		assert(s.player.vel_x == v1x && MOTY(s.player) == v1y
			   && s.player.vel_z == v1z);
		server_local_tick_actor_knockback(&s);
		const double d1x = wit_xz(v1x), d1z = wit_xz(v1z);
		const double d1y = wit_y(v1y);
		assert(s.player.vel_x == d1x && s.player.vel_z == d1z
			   && MOTY(s.player) == d1y);
		FLUSH_VEL(&s);
		assert(g_vel_n == 1);
		assert(g_vel[0].route == 0 && g_vel[0].owner == 0 && !g_vel[0].add);
		assert(g_vel[0].v[0] == q28(d1x) && g_vel[0].v[1] == q28(d1y)
			   && g_vel[0].v[2] == q28(d1z));
		FLUSH_VEL(&s);
		assert(g_vel_n == 1);
		puts("  (v) V1 fresh null-exploder blast: one absolute post-step XYZ Packet28 (Beta air step), no repeat OK");

		/* V2 absorbed / V3 excess-only, flag clear: motion takes the impulse,
		 * no message at all. */
		VRESET(MAX_PLAYER_HEALTH, 20, (short)vunits, 0.01F, 0.0, 0.03F);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		assert(s.player.health == MAX_PLAYER_HEALTH);
		assert(s.player.vel_x == addm(0.01F, imp[0]) && MOTY(s.player) == imp[1]
			   && s.player.vel_z == addm(0.03F, imp[2]) && s.player.y == EYE_Y);
		server_local_tick_actor_knockback(&s);
		FLUSH_VEL(&s);
		assert(g_vel_n == 0);
		VRESET(MAX_PLAYER_HEALTH, 20, 40, 0.0F, 0.0, 0.0F);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		assert(s.player.health == MAX_PLAYER_HEALTH - (vunits - 40));
		assert(s.player.vel_x == addm(0.0F, imp[0]) && MOTY(s.player) == imp[1]);
		server_local_tick_actor_knockback(&s);
		FLUSH_VEL(&s);
		assert(g_vel_n == 0);
		puts("  (v) V2/V3 absorbed and excess-only blasts, flag clear: motion += impulse, no message OK");

		/* V4: fresh blast + absorbed blast in one tick -> ONE combined message. */
		VRESET(MAX_PLAYER_HEALTH, 0, 0, -0.1F, 0.02, 0.0F);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		assert(s.player.health == MAX_PLAYER_HEALTH - vunits && g_vel_n == 0);
		const double v4x = addm(addm(-0.1F, imp[0]), imp[0]);
		const double v4z = addm(addm(0.0F, imp[2]), imp[2]);
		const double v4y = 0.02 + imp[1] + imp[1];
		assert(s.player.vel_x == v4x && MOTY(s.player) == v4y && s.player.vel_z == v4z);
		server_local_tick_actor_knockback(&s);
		FLUSH_VEL(&s);
		assert(g_vel_n == 1 && !g_vel[0].add && g_vel[0].owner == 0);
		assert(g_vel[0].v[0] == q28(wit_xz(v4x)) && g_vel[0].v[1] == q28(wit_y(v4y))
			   && g_vel[0].v[2] == q28(wit_xz(v4z)));
		FLUSH_VEL(&s);
		assert(g_vel_n == 1);
		puts("  (v) V4 fresh + absorbed in one tick: one combined absolute message, then none OK");

		/* V4b: pending + EXCESS: a fresh blast then an in-window excess hit
		 * (raw flat damage above last_damage): the flag survives, the excess
		 * hit raises nothing new -> still exactly one message. */
		VRESET(MAX_PLAYER_HEALTH, 0, 0, 0.0F, 0.0, 0.0F);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		assert(server_local_damage_actor(&s, &s.player, vunits + 16, 0.0F, 0.0F));
		assert(s.player.health == MAX_PLAYER_HEALTH - vunits - 16 && g_vel_n == 0);
		server_local_tick_actor_knockback(&s);
		FLUSH_VEL(&s);
		FLUSH_VEL(&s);
		assert(g_vel_n == 1);
		puts("  (v) V4b pending flag + excess-only hit: one message OK");

		/* V5: Packet28 clamp (+-3.9) and truncation toward zero (not floor). */
		VRESET(MAX_PLAYER_HEALTH, 0, 0, 5.0F, -0.4, -4.5F);
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		server_local_tick_actor_knockback(&s);
		FLUSH_VEL(&s);
		assert(g_vel_n == 1 && !g_vel[0].add);
		{
			const double y5 = wit_y(-0.4 + imp[1]);
			int32_t t = (int32_t)(y5 * 8000.0);
			assert(y5 < 0.0 && (double)t != floor(y5 * 8000.0));
			assert(g_vel[0].v[0] == (float)31200 / 8000.0f);
			assert(g_vel[0].v[2] == (float)-31200 / 8000.0f);
			assert(g_vel[0].v[1] == (float)t / 8000.0f);
			assert(s.player.vel_x == wit_xz(addm(5.0F, imp[0]))); /* unclamped */
		}
		puts("  (v) V5 Packet28 quantization: +-3.9 clamp, *8000 truncation toward zero; server copy unclamped OK");

		server_local_test_set_difficulty(2); /* Normal: EntityMob damage unscaled */
		/* V8: mixed damage. A fresh MELEE hit (knockBack from the attacker)
		 * then a blast in the same tick: one combined message carrying the
		 * knockBack AND the impulse -- the melee sends nothing by itself. */
		{
			vec3 zp = {(float)EYE_X - 1.0F, (float)(EYE_Y - PLAYER_EYE_HEIGHT),
					   (float)EYE_Z};
			struct entity* zom = server_local_spawn_monster_at(zp, MONSTER_ZOMBIE, &s);
			assert(zom);
			VRESET(MAX_PLAYER_HEALTH, 0, 0, 0.02F, -0.0784, 0.0F);
			server_local_hurt_player_for(&s, &s.player, zom);
			assert(s.player.health < MAX_PLAYER_HEALTH);
			assert(g_vel_n == 0); /* no immediate knockBack send */
			double kdx = (double)zom->pos[0] - s.player.x;
			double kdz = (double)zom->pos[2] - s.player.z;
			float klen = (float)sqrt(kdx * kdx + kdz * kdz);
			double kx = (double)0.02F / 2.0 - kdx / (double)klen * (double)0.4F;
			double kz = 0.0 / 2.0 - kdz / (double)klen * (double)0.4F;
			double ky = fmin(-0.0784 / 2.0 + (double)0.4F, (double)0.4F);
			assert(s.player.vel_x == kx && s.player.vel_z == kz && MOTY(s.player) == ky);
			server_world_begin_update(&s.worlds[0]);
			server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
			assert(g_vel_n == 0);
			const double mx = addm(kx, imp[0]), mz = addm(kz, imp[2]);
			const double my = ky + imp[1];
			server_local_tick_actor_knockback(&s);
			FLUSH_VEL(&s);
			assert(g_vel_n == 1 && !g_vel[0].add);
			assert(g_vel[0].v[0] == q28(wit_xz(mx)) && g_vel[0].v[1] == q28(wit_y(my))
				   && g_vel[0].v[2] == q28(wit_xz(mz)));
			/* V9: a fresh FALL hit (no attacker, no knockBack) flags too. */
			VRESET(MAX_PLAYER_HEALTH, 0, 0, 0.0F, -0.5, 0.0F);
			assert(server_local_damage_actor(&s, &s.player, 16, 0.0F, 0.0F));
			assert(g_vel_n == 0);
			server_local_tick_actor_knockback(&s);
			FLUSH_VEL(&s);
			assert(g_vel_n == 1 && g_vel[0].v[0] == 0.0F && g_vel[0].v[2] == 0.0F
				   && g_vel[0].v[1] == q28(wit_y(-0.5)));
			/* V10: non-null (creeper) exploder: knockBack away from it plus the
			 * impulse, still exactly ONE message (no knockBack duplicate). */
			vec3 cp = {vc[0], vc[1] - (float)PLAYER_EYE_HEIGHT, vc[2]};
			struct entity* cre = server_local_spawn_monster_at(cp, MONSTER_CREEPER, &s);
			assert(cre);
			VRESET(MAX_PLAYER_HEALTH, 0, 0, 0.0F, 0.0, 0.0F);
			server_world_begin_update(&s.worlds[0]);
			assert(server_world_try_explode_by(&s.worlds[0], cre,
				(vec3) {vc[0], vc[1], vc[2]}, POWER));
			assert(g_vel_n == 0 && s.player.health < MAX_PLAYER_HEALTH);
			server_local_tick_actor_knockback(&s);
			FLUSH_VEL(&s);
			FLUSH_VEL(&s);
			assert(g_vel_n == 1 && !g_vel[0].add);
			uint32_t zid = zom->id, cid = cre->id;
			dict_entity_erase(s.entities, zid);
			free(zom);
			struct entity** cs = dict_entity_get(s.entities, cid);
			if(cs && *cs) {
				dict_entity_erase(s.entities, cid);
				free(cre);
			}
		}
		puts("  (v) V8-V10 melee+blast combined, fresh fall flags, creeper exploder: one message each OK");

		/* V6: guest routing (owner 1 on the guest reply route only). */
		struct server_player* g = calloc(1, sizeof(*g));
		assert(g);
		g->inventory.reply = vel_capture;
		g->inventory.reply_user = (void*)(intptr_t)1;
		g->has_pos = true;
		g->finished_loading = true;
		g->health = MAX_PLAYER_HEALTH;
		g->dimension = s.player.dimension;
		g->world_slot = 0;
		g->on_ground = false;
		g->x = EYE_X;
		g->y = EYE_Y;
		g->z = EYE_Z;
		g->vel_x = 0.07F;
		s.player.x = 200.5;
		s.player.z = 200.5; /* out of the blast box */
		s.player.been_attacked = false;
		s.guest_actor = g;
		s.guest_gameplay = true;
		s.guest_generation = 77;
		g_vel_n = 0;
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		assert(g->health == MAX_PLAYER_HEALTH - vunits);
		assert(g_vel_n == 0 && g->y == EYE_Y && g->vel_y == 0.0F);
		server_local_tick_actor_knockback(&s);
		FLUSH_VEL(&s);
		assert(g_vel_n == 1 && g_vel[0].route == 1 && g_vel[0].owner == 1
			   && !g_vel[0].add);
		assert(g_vel[0].v[0] == q28(wit_xz(addm(0.07F, imp[0])))
			   && g_vel[0].v[1] == q28(wit_y(imp[1]))
			   && g_vel[0].v[2] == q28(wit_xz(addm(0.0F, imp[2]))));
		puts("  (v) V6 guest blast: one owner-1 absolute message on the guest route, none to the host OK");

		/* V7: SYNTHETIC lifetime change (guest_generation set by hand) before the
		 * tracker phase: never delivered, flag gone. */
		g->health = MAX_PLAYER_HEALTH;
		g->hurt_resist_time = 0;
		g->last_damage = 0;
		g_vel_n = 0;
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {vc[0], vc[1], vc[2]}, POWER);
		assert(g->health == MAX_PLAYER_HEALTH - vunits);
		s.guest_generation = 78;
		FLUSH_VEL(&s);
		s.guest_generation = 77;
		FLUSH_VEL(&s);
		assert(g_vel_n == 0);
		puts("  (v) V7 hand-set guest generation change (synthetic; real re-admission is owner_swap L2): stale flag never delivered OK");

		s.guest_gameplay = false;
		s.guest_actor = NULL;
		s.guest_generation = 0;
		free(g);
		s.player.inventory.reply = NULL;
		s.player.inventory.reply_user = NULL;
		s.player.has_pos = false;
		s.player.vel_x = s.player.vel_y = s.player.vel_z = 0.0F;
		MOTY_SET(s.player, 0.0);
#undef VRESET
	}

	/* ---- (x) G8-BLAST-VELOCITY v3: server motion collision / liquid effects
	 * (ASTRA-V2-REVIEW (b)(d)(e)), one stationary accepted sample each
	 * (server_local_tick_actor_knockback), expected values transcribed from
	 * Entity.moveEntity / EntityLiving.moveEntityWithHeading / World.
	 * handleMaterialAcceleration independently of the product code. */
#ifdef SERVER_LOCAL_HAS_SERVER_MOTION_Y
	{
		const int FX = 64, FY = 80, FZ = 64;
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.sneaking = false;
		/* X4 (e): > 192 colliding boxes. A stone floor under a 40 x 40 swept
		 * hull (1600+ intersecting boxes) and a wall at the far end: the
		 * whole list must clip -- the pre-v3 cap dropped ALL of them and the
		 * motion went through the floor and the wall unclipped. */
		for(int x = FX; x <= FX + 45; x++)
			for(int z = FZ; z <= FZ + 45; z++)
				set_block(&s, x, FY - 1, z, BLOCK_STONE);
		for(int z = FZ; z <= FZ + 45; z++)
			for(int y = FY; y <= FY + 2; y++)
				set_block(&s, FX + 30, y, z, BLOCK_STONE);
		s.player.x = FX + 0.5;
		s.player.y = FY + (double)PLAYER_EYE_HEIGHT; /* feet on the floor top */
		s.player.z = FZ + 0.5;
		s.player.on_ground = true;
		s.player.in_web = false;
		s.player.vel_x = 40.0;
		s.player.vel_z = 40.0;
		MOTY_SET(s.player, -0.1);
		server_local_tick_actor_knockback(&s);
		printf("  (x) X4 dense floor+wall: vel (%.9g, %.9g, %.9g)\n",
			   s.player.vel_x, MOTY(s.player), s.player.vel_z);
		fflush(stdout);
		/* Y clipped by the floor (motionY 0, then gravity/drag), X clipped by
		 * the wall at FX+30 (motionX 0); Z free over the floor (40 * ground
		 * friction 0.6F * 0.91F). */
		assert(MOTY(s.player) == (0.0 - 0.08) * (double)0.98F);
		assert(s.player.vel_x == 0.0);
		assert(s.player.vel_z == 40.0 * (double)(0.6F * 0.91F));
		for(int x = FX; x <= FX + 45; x++)
			for(int z = FZ; z <= FZ + 45; z++)
				set_block(&s, x, FY - 1, z, BLOCK_AIR);
		for(int z = FZ; z <= FZ + 45; z++)
			for(int y = FY; y <= FY + 2; y++)
				set_block(&s, FX + 30, y, z, BLOCK_AIR);
		puts("  (x) X4 1600+ colliding boxes: floor and wall both clip (no cap) OK");

		/* X1 (b): cobweb. Event 1: the physics move's sweep sets isInWeb;
		 * the packet's own second move (:173) consumes it -- motion zeroed --
		 * and its sweep sets it again. Event 2 repeats: motion stays zero. */
		set_block(&s, FX, FY, FZ, BLOCK_COBWEB);
		set_block(&s, FX, FY + 1, FZ, BLOCK_COBWEB);
		s.player.x = FX + 0.5;
		s.player.y = FY + 10.0 + (double)PLAYER_EYE_HEIGHT - 10.0;
		s.player.z = FZ + 0.5;
		s.player.on_ground = false;
		s.player.in_web = false;
		s.player.vel_x = 0.1;
		s.player.vel_z = 0.0;
		MOTY_SET(s.player, 0.0);
		server_local_tick_actor_knockback(&s);
		assert(s.player.in_web);
		assert(s.player.vel_x == 0.0 && MOTY(s.player) == 0.0
			   && s.player.vel_z == 0.0);
		s.player.vel_x = 0.2;
		server_local_tick_actor_knockback(&s);
		assert(s.player.in_web && s.player.vel_x == 0.0 && MOTY(s.player) == 0.0);
		s.player.in_web = false;
		set_block(&s, FX, FY, FZ, BLOCK_AIR);
		set_block(&s, FX, FY + 1, FZ, BLOCK_AIR);
		puts("  (x) X1 cobweb: isInWeb set by the sweep, consumed by the next move (motion zeroed) OK");

		/* X5 (d): water flow push. Source water at the body cell, a meta-1
		 * flow +X of it, stone floor and walls of air: getFlowVector = (+1,
		 * 0, 0) -> normalised * 0.014D into motionX, then the WATER branch:
		 * *0.8F, motionY -= 0.02. */
		set_block(&s, FX, FY - 1, FZ, BLOCK_STONE);
		set_block(&s, FX + 1, FY - 1, FZ, BLOCK_STONE);
		set_block(&s, FX - 1, FY - 1, FZ, BLOCK_STONE);
		set_block(&s, FX, FY - 1, FZ + 1, BLOCK_STONE);
		set_block(&s, FX, FY - 1, FZ - 1, BLOCK_STONE);
		{
			struct block_data wsrc = {.type = BLOCK_WATER_STILL, .metadata = 0};
			struct block_data wflow = {.type = BLOCK_WATER_FLOW, .metadata = 1};
			assert(server_world_set_block(&s.worlds[0], FX, FY, FZ, wsrc));
			assert(server_world_set_block(&s.worlds[0], FX + 1, FY, FZ, wflow));
		}
		s.player.x = FX + 0.5;
		s.player.y = FY + (double)PLAYER_EYE_HEIGHT;
		s.player.z = FZ + 0.5;
		s.player.on_ground = true;
		s.player.vel_x = 0.0;
		s.player.vel_z = 0.0;
		MOTY_SET(s.player, 0.0);
		server_local_tick_actor_knockback(&s);
		printf("  (x) X5 water flow: vel (%.9g, %.9g, %.9g)\n", s.player.vel_x,
			   MOTY(s.player), s.player.vel_z);
		fflush(stdout);
		assert(s.player.vel_x == (0.0 + 1.0 * 0.014) * (double)0.8F);
		assert(MOTY(s.player) == 0.0 * (double)0.8F - 0.02);
		assert(s.player.vel_z == 0.0);
		set_block(&s, FX, FY, FZ, BLOCK_AIR);
		set_block(&s, FX + 1, FY, FZ, BLOCK_AIR);
		set_block(&s, FX, FY - 1, FZ, BLOCK_AIR);
		set_block(&s, FX + 1, FY - 1, FZ, BLOCK_AIR);
		set_block(&s, FX - 1, FY - 1, FZ, BLOCK_AIR);
		set_block(&s, FX, FY - 1, FZ + 1, BLOCK_AIR);
		set_block(&s, FX, FY - 1, FZ - 1, BLOCK_AIR);
		puts("  (x) X5 flowing water: normalised flow * 0.014D into server motion, water branch OK");

		/* X2 (b): sneak-edge. On ground, sneaking, at the +X edge of a single
		 * stone block, motionX 0.3 over the drop: the edge loop backs dx off
		 * in 0.05 steps while nothing is under (dx, -1, 0); ox follows dx, so
		 * no axis is "clipped" and motionX is NOT zeroed -- only friction. */
		set_block(&s, FX, FY - 1, FZ, BLOCK_STONE);
		s.player.x = FX + 0.95;
		s.player.y = FY + (double)PLAYER_EYE_HEIGHT;
		s.player.z = FZ + 0.5;
		s.player.on_ground = true;
		s.player.sneaking = true;
		s.player.vel_x = 0.3;
		s.player.vel_z = 0.0;
		MOTY_SET(s.player, 0.0);
		server_local_tick_actor_knockback(&s);
		printf("  (x) X2 sneak edge: vel (%.9g, %.9g, %.9g)\n", s.player.vel_x,
			   MOTY(s.player), s.player.vel_z);
		fflush(stdout);
		assert(s.player.vel_x == 0.3 * (double)(0.6F * 0.91F));
		assert(MOTY(s.player) == (0.0 - 0.08) * (double)0.98F);
		s.player.sneaking = false;
		set_block(&s, FX, FY - 1, FZ, BLOCK_AIR);
		puts("  (x) X2 sneak-edge: dx backed off, motion not zeroed, ground friction OK");

		/* X6 boat boxes (World.getCollidingBoundingBoxes:810-821, strict
		 * AxisAlignedBB.intersectsWith; EntityBoat.getBoundingBox non-null).
		 * Free boat in open air, the body level with it. */
		{
			struct entity* boat = server_local_spawn_boat(
				(vec3) {(float)FX + 10.5F, (float)FY + 0.5F, (float)FZ + 0.5F}, &s);
			assert(boat && boat->getBoundingBox);
			struct AABB bb;
			assert(boat->getBoundingBox(boat, &bb));
			const double half = (double)(0.6F / 2.0F);
			/* X6a: 0.1 gap to the boat's -x face, motionX 0.5: clipped to the
			 * gap -> isCollidedHorizontally on X -> motionX = 0 (Entity.
			 * moveEntity:369-371); Y/Z free (air step). */
			s.player.x = (double)bb.x1 - 0.1 - half;
			s.player.y = (double)bb.y1 + (double)PLAYER_EYE_HEIGHT;
			s.player.z = ((double)bb.z1 + (double)bb.z2) / 2.0;
			s.player.on_ground = false;
			s.player.sneaking = false;
			s.player.in_web = false;
			s.player.vel_x = 0.5;
			s.player.vel_z = 0.0;
			MOTY_SET(s.player, 0.0);
			server_local_tick_actor_knockback(&s);
			printf("  (x) X6a boat in path: vel (%.9g, %.9g, %.9g)\n", s.player.vel_x,
				   MOTY(s.player), s.player.vel_z);
			fflush(stdout);
			assert(s.player.vel_x == 0.0);
			assert(MOTY(s.player) == wit_y(0.0));
			assert(s.player.vel_z == 0.0);
			/* X6b: face contact only (body maxX == boat minX) moving +z,
			 * parallel to the face: strict intersects -> the boat is not in
			 * the list and clips nothing: air friction only. */
			s.player.x = (double)bb.x1 - half;
			assert(s.player.x + half == (double)bb.x1);
			s.player.vel_x = 0.0;
			s.player.vel_z = 0.3;
			MOTY_SET(s.player, 0.0);
			server_local_tick_actor_knockback(&s);
			assert(s.player.vel_x == 0.0 && s.player.vel_z == wit_xz(0.3));
			assert(MOTY(s.player) == wit_y(0.0));
			/* X6c: a boat marked for removal (delay_destroy == 0, Entity.
			 * isDead) is not a collider: the X6a move passes through. */
			const int keep = boat->delay_destroy;
			boat->delay_destroy = 0;
			s.player.x = (double)bb.x1 - 0.1 - half;
			s.player.vel_x = 0.5;
			s.player.vel_z = 0.0;
			MOTY_SET(s.player, 0.0);
			server_local_tick_actor_knockback(&s);
			assert(s.player.vel_x == wit_xz(0.5));
			boat->delay_destroy = keep;
			uint32_t bid = boat->id;
			dict_entity_erase(s.entities, bid);
			free(boat);
			puts("  (x) X6 boat box: clips motionX to zero, face contact does not clip, dead boat ignored OK");
		}

		/* X7 ladder edges (EntityLiving.moveEntityWithHeading:480-510): the
		 * clamps read isOnLadder at the PRE-move box; the 0.2 climb reads it
		 * at the MOVED box (posX, boundingBox.minY, posZ after moveEntity).
		 * Wall at x = FX+1 (cells FY..FY+3), not on ground, air friction. */
		for(int y = FY; y <= FY + 3; y++)
			set_block(&s, FX + 1, y, FZ, BLOCK_STONE);
		{
			struct block_data ladder = {.type = BLOCK_LADDER, .metadata = 4};
			/* X7a: ladder ONLY in the moved-to cell (FY+1). No clamp (mx 0.5
			 * stays), the X move hits the wall -> collidedHorizontally, the
			 * moved box's cell is a ladder -> motionY = 0.2. */
			assert(server_world_set_block(&s.worlds[0], FX, FY + 1, FZ, ladder));
			s.player.x = FX + 0.5;
			s.player.y = FY + 0.5 + (double)PLAYER_EYE_HEIGHT; /* minY FY+0.5 */
			s.player.z = FZ + 0.5;
			s.player.on_ground = false;
			s.player.vel_x = 0.5;
			s.player.vel_z = 0.05;
			MOTY_SET(s.player, 0.6); /* moves minY to FY+1.1 */
			server_local_tick_actor_knockback(&s);
			printf("  (x) X7a ladder only after the move: vel (%.9g, %.9g, %.9g)\n",
				   s.player.vel_x, MOTY(s.player), s.player.vel_z);
			fflush(stdout);
			assert(s.player.vel_x == 0.0);
			assert(MOTY(s.player) == wit_y(0.2));
			assert(s.player.vel_z == wit_xz(0.05));
			assert(server_world_set_block(&s.worlds[0], FX, FY + 1, FZ,
				(struct block_data) {.type = BLOCK_AIR}));
			/* X7b: ladder ONLY in the start cell (FY). Clamp mx 0.5 -> 0.15F,
			 * the wall still clips it (body maxX FX+0.9 + 0.15 > FX+1), but
			 * the moved box (minY FY+1.1) has no ladder: no 0.2. */
			assert(server_world_set_block(&s.worlds[0], FX, FY, FZ, ladder));
			s.player.x = FX + 0.6;
			s.player.y = FY + 0.5 + (double)PLAYER_EYE_HEIGHT;
			s.player.vel_x = 0.5;
			s.player.vel_z = 0.05;
			MOTY_SET(s.player, 0.6);
			server_local_tick_actor_knockback(&s);
			printf("  (x) X7b ladder only before the move: vel (%.9g, %.9g, %.9g)\n",
				   s.player.vel_x, MOTY(s.player), s.player.vel_z);
			fflush(stdout);
			assert(s.player.vel_x == 0.0);
			assert(MOTY(s.player) == wit_y(0.6));
			assert(s.player.vel_z == wit_xz(0.05));
			assert(server_world_set_block(&s.worlds[0], FX, FY, FZ,
				(struct block_data) {.type = BLOCK_AIR}));
		}
		for(int y = FY; y <= FY + 3; y++)
			set_block(&s, FX + 1, y, FZ, BLOCK_AIR);
		puts("  (x) X7 ladder: clamps from the pre-move cell, 0.2 climb from the MOVED box OK");

		s.player.has_pos = false;
		s.player.vel_x = s.player.vel_z = 0.0;
		MOTY_SET(s.player, 0.0);
	}
#endif

	/* ---- (m) cost table: single-threaded wall time per blast ------------- */
	{
		struct server_world_explosion_cost c;
		ptime_t t0;
		/* stone monolith 25^3 at (100..124, 4..24, 100..124) */
		for(int x = 100; x <= 124; x++)
			for(int z = 100; z <= 124; z++)
				for(int y = 4; y <= 24; y++)
					set_block(&s, x, y, z, BLOCK_STONE);
		server_world_begin_update(&s.worlds[0]);
		t0 = time_get();
		server_world_explode(&s.worlds[0], (vec3) {112.5F, 14.5F, 112.5F}, POWER);
		float ms_stone = time_diff_s(t0, time_get()) * 1000.0F;
		server_world_explosion_last_cost(&c);
		printf("  (m) power 4 in stone: %.3f ms (lookups %u, cells %u, entities %u)\n",
			   (double)ms_stone, c.block_lookups, c.cells, c.entities);
		/* open air, high above the ground */
		server_world_begin_update(&s.worlds[0]);
		t0 = time_get();
		server_world_explode(&s.worlds[0], (vec3) {112.5F, 60.0F, 8.5F}, POWER);
		float ms_air = time_diff_s(t0, time_get()) * 1000.0F;
		server_world_explosion_last_cost(&c);
		printf("  (m) power 4 in open air: %.3f ms (lookups %u, cells %u, entities %u)\n",
			   (double)ms_air, c.block_lookups, c.cells, c.entities);
		/* 30-blast chain proxy in a dirt block (72..94, 4..20, 100..124):
		 * 30 synchronous power-4 blasts clustered in a 10-block cube so their
		 * 30% drops pile up around the 31st, timed, blast. */
		for(int x = 72; x <= 94; x++)
			for(int z = 100; z <= 124; z++)
				for(int y = 4; y <= 20; y++)
					set_block(&s, x, y, z, BLOCK_DIRT);
		for(int i = 0; i < 30; i++) {
			server_world_begin_update(&s.worlds[0]);
			server_world_explode(&s.worlds[0], (vec3) {78.5F + (float)(i % 5) * 2.0F, 8.5F + (float)((i / 5) % 3) * 2.0F,
											  108.5F + (float)(i % 3) * 2.0F}, POWER);
		}
		size_t world_entities = dict_entity_size(s.entities);
		server_world_begin_update(&s.worlds[0]);
		t0 = time_get();
		server_world_explode(&s.worlds[0], (vec3) {82.5F, 10.5F, 110.5F}, POWER);
		float ms_chain = time_diff_s(t0, time_get()) * 1000.0F;
		server_world_explosion_last_cost(&c);
		printf("  (m) power 4 after a 30-blast chain, %zu entities in the world: %.3f ms "
			   "(lookups %u, cells %u, entities hit %u, density rays %u)\n",
			   world_entities, (double)ms_chain, c.block_lookups, c.cells, c.entities,
			   c.density_rays);
		fflush(stdout);
	}

	free_entities(&s);
	f4rng_run(&s);
	free_entities(&s);
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_destroy(&s.worlds[0]);
	mem2_free(s.entity_tick_keys);
	s.entity_tick_keys = NULL;
	dict_entity_clear(s.entities);
	string_clear(level);
	/* ---- (w) G8-BLAST-VELOCITY through the REAL server update -----------
	 * ASTRA-V-REVIEW (a): the tracker flush must follow EVERY participating
	 * world's entity pass and the player hazards. A fresh session with two
	 * live world slots runs server_local_test_update_once; the G9 phase hook
	 * detonates a null-exploder blast at the START of the chosen world's
	 * entity pass (phase 1), next to the host body committed to that slot.
	 * Exactly ONE velocity message must reach the host route by the END of
	 * that same update, carrying the post-step Beta motion.
	 * RED on 2e3b93aa2: the flush ran inside slot 0's block, before the
	 * secondary worlds -- a slot-1 blast arrived one update late. */
	if(!getenv("CW_BLASTV_SKIP_W0")) /* lets the slot-1 RED show on its own */
		w_run(0);
	w_run(1);
	w_run_n(0, 0); /* zero samples: no server step, the raw post-impulse motion */
	w_run_n(0, 2); /* two samples: two steps, in order */
	w_run_n(1, 0);
	/* v4 (ASTRA-V3-REVIEW P1 queue): 8, 9, >9 samples with the same-tick
	 * hit, both slots, plus the capacity edge and one beyond it. */
	w_run_n(0, 8);
	w_run_n(0, 9);
	w_run_n(0, 12);
	w_run_n(1, 9);
	w_run_n(1, 12);
	/* v5: 32, 33, more than 33, and the whole per-update budget -- none
	 * refused. */
	w_run_n(0, 32);
	w_run_n(0, 33);
	w_run_n(0, 34);
	w_run_n(1, 40);
	w_run_n(0, ACTOR_MOTION_QUEUE);
	w_run_lethal();
	/* (y) production hit sources, exact payload/order, both slots. */
	for(int k = Y_OWNER_SKELETON; k <= Y_OWNER_RELOADED; k++) {
		y_arrow_absorbed_blast(0, k);
		y_arrow_absorbed_blast(1, k);
	}
	y_real_fall(0);
	y_real_fall(1);
	y_real_fire(0);
	y_real_fire(1);
	y_creeper_exploder();
	puts("  (y) arrow+absorbed blast, real fall, real fire, creeper exploder through the update: one exact absolute message each, last reply, no repeat OK");
	/* (z) G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW) */
	for(int slot = 0; slot <= 1; slot++)
		for(int n = 0; n <= 2; n++)
			w_push_run(slot, n);
	puts("  (z) Z2 own push loop per accepted packet (0/1/2 samples, mixed blast hit, both slots): exact payloads OK");
	y_fall_plus_arrow(0, 6.0);
	y_fall_plus_arrow(1, 6.0);
	y_fall_plus_arrow(0, 8.0);
	y_fall_plus_arrow(1, 8.0);
	puts("  (z) Z3 landing + same-update arrow: arrow fresh first (knockBack), landing absorbed / excess-only OK");
	z_ride(ZK_M);
	z_ride(ZK_G);
	z_ride(ZK_B);
	z_ride(ZK_C);
	w_pause_run();
	/* (v5) ASTRA-V4-REVIEW finite checklist items 1-3 */
	q_drain_run();
	q_pause_backlog_run();
	r_ride_run(false);
	r_ride_run(true);
	k_corpse_run();
	r_rng_run();
	a_owner_run();
	/* (F1) V6 F1 (ASTRA-V6-PLAN item 3): production-to-replay through the
	 * real producer; CW_F1_CASE=host|guest|pause runs one (RED receipts). */
	{
		const char* only = getenv("CW_F1_CASE");
		if(!only || !strcmp(only, "host")) {
			static const int f1_counts[] = {32, 33, 34, 40, 101, 250};
			for(size_t k = 0; k < sizeof f1_counts / sizeof f1_counts[0]; k++)
				f1_producer_host_run(f1_counts[k]);
		}
		if(!only || !strcmp(only, "guest"))
			f1_producer_guest_run(40);
		if(!only || !strcmp(only, "pause"))
			f1_pause_full_run();
		if(!only || !strcmp(only, "guestpause"))
			f1_guest_pause_run();
		if(!only || !strcmp(only, "backpressure"))
			f1_backpressure_poses_run();
	}
	f2a_run_all();

	g_drain_run = 0;
	thread_join(&drain);
	assert(g_applied_messages == 0);
	puts("test_explosion_beta_golden: OK");
	return 0;
}
