/*
	End-to-end Beta 1.7.3 primed-TNT lifecycle golden.

	This deliberately fills the production scheduled-tick ring with DISTINCT
	future fluid entries before placing/igniting TNT.  The hardware regression was
	that both scheduling attempts failed at capacity while the metadata/fuse state
	still committed, leaving a permanent primed block.  Beta cannot have that
	failure mode: BlockTNT removes the world block and spawns EntityTNTPrimed;
	entity updates are independent of World.scheduledTickTreeSet.

	Ground truth (decompile 740c583):
	  BlockTNT.onBlockDestroyedByPlayer -> spawn EntityTNTPrimed, set air;
	  EntityTNTPrimed constructor -> fuse=80;
	  EntityTNTPrimed.onUpdate -> gravity/move/drag/bounce, then
	  `if (fuse-- <= 0) explode else smoke`.
	Therefore 80 entity updates leave a live fuse value 0; update 81 explodes.

	The test asserts only user-visible/lifecycle facts before the implementation
	detail: ignition creates one independent entity and clears the source block,
	the entity survives updates 1..80, then disappears/explodes on update 81 even
	while the fluid queue remains full.  It does not bless the old block-side-table
	architecture.
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_primed_tnt.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "particle.h"
#include "game/game_state.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_tnt_primed_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
extern struct item item_flint_steel;

static volatile int g_drain_run = 1;
static size_t g_tnt_correction_rpcs = 0;
static size_t g_explosion_rpcs = 0;
static size_t g_watched_set_block_rpcs = 0;
static size_t g_watched_neighbour_callbacks = 0;
static bool g_watch_block = false;
static w_coord_t g_watch_x, g_watch_y, g_watch_z;
static enum client_rpc_type g_watched_order[16];
static size_t g_watched_order_count = 0;
static enum client_rpc_type g_terminal_order[16];
static uint32_t g_terminal_destroy_ids[16];
static size_t g_terminal_order_count = 0;
static enum client_rpc_type g_prepass_order[16];
/* CRPC_CHUNK column of each captured entry (G9X-6 republish attribution) */
static int g_prepass_chunk_x[16], g_prepass_chunk_z[16];
static size_t g_prepass_order_count = 0;
static bool g_capture_prepass_order = false;
static vec3 g_prepass_explosion_pos;
static bool g_captured_input_handoff = false;
static struct client_rpc g_input_handoff;
static size_t g_prepass_window_transactions = 0;
static size_t g_terminal_load_failures = 0;

static void capture_tnt_rpcs(const struct client_rpc* call, void* user) {
	(void)user;
	if(g_capture_prepass_order && call->type == CRPC_PLAYER_POS
	   && call->payload.player_pos.input_epoch != 0) {
		assert(!g_captured_input_handoff);
		g_input_handoff = *call;
		g_captured_input_handoff = true;
	}
	if(g_capture_prepass_order && call->type == CRPC_WINDOW_TRANSACTION)
		g_prepass_window_transactions++;
	if(g_capture_prepass_order
	   && (call->type == CRPC_ENTITY_EXPLODE
		   || call->type == CRPC_UNLOAD_CHUNK
		   || call->type == CRPC_CHUNK
		   || call->type == CRPC_PLAYER_POS
		   || call->type == CRPC_PLAYER_SET_HEALTH
		   || call->type == CRPC_PLAYER_HOTBAR_SLOT
		   || call->type == CRPC_TIME_SET)) {
		assert(g_prepass_order_count
			   < sizeof(g_prepass_order) / sizeof(g_prepass_order[0]));
		if(call->type == CRPC_CHUNK) {
			g_prepass_chunk_x[g_prepass_order_count] = call->payload.chunk.x / CHUNK_SIZE;
			g_prepass_chunk_z[g_prepass_order_count] = call->payload.chunk.z / CHUNK_SIZE;
		}
		g_prepass_order[g_prepass_order_count++] = call->type;
		if(call->type == CRPC_UNLOAD_CHUNK) {
			g_prepass_chunk_x[g_prepass_order_count - 1]
				= call->payload.unload_chunk.x;
			g_prepass_chunk_z[g_prepass_order_count - 1]
				= call->payload.unload_chunk.z;
			fprintf(stderr, "captured unload (%d,%d)\n",
				call->payload.unload_chunk.x, call->payload.unload_chunk.z);
		}
		if(call->type == CRPC_ENTITY_EXPLODE)
			memcpy(g_prepass_explosion_pos, call->payload.entity_explode.pos,
				   sizeof(g_prepass_explosion_pos));
	}
	if(call->type == CRPC_PRIMED_TNT_CORRECTION)
		g_tnt_correction_rpcs++;
	else if(call->type == CRPC_LOAD_FAILED)
		g_terminal_load_failures++;
	else if(call->type == CRPC_ENTITY_EXPLODE) {
		g_explosion_rpcs++;
		assert(g_terminal_order_count
			   < sizeof(g_terminal_order) / sizeof(g_terminal_order[0]));
		g_terminal_order[g_terminal_order_count] = call->type;
		g_terminal_destroy_ids[g_terminal_order_count++] = 0;
	} else if(call->type == CRPC_ENTITY_DESTROY) {
		assert(g_terminal_order_count
			   < sizeof(g_terminal_order) / sizeof(g_terminal_order[0]));
		g_terminal_order[g_terminal_order_count] = call->type;
		g_terminal_destroy_ids[g_terminal_order_count++]
			= call->payload.entity_destroy.entity_id;
	} else if(g_watch_block && call->type == CRPC_SET_BLOCK
			&& call->payload.set_block.x == g_watch_x
			&& call->payload.set_block.y == g_watch_y
			&& call->payload.set_block.z == g_watch_z)
		g_watched_set_block_rpcs++;
	if(g_watch_block
	   && (call->type == CRPC_SPAWN_PRIMED_TNT
		   || (call->type == CRPC_SET_BLOCK
			   && call->payload.set_block.x == g_watch_x
			   && call->payload.set_block.y == g_watch_y
			   && call->payload.set_block.z == g_watch_z))) {
		assert(g_watched_order_count
			   < sizeof(g_watched_order) / sizeof(g_watched_order[0]));
		g_watched_order[g_watched_order_count++] = call->type;
	}
}

static void count_watched_neighbour_callback(
	struct server_local* s, struct block_info* info) {
	(void)s;
	(void)info;
	g_watched_neighbour_callbacks++;
}

/* G9X-6 attribution: true when column (cx,cz) is the ONLY sent column that
 * owes the client a publication refresh (server_chunk.pub_debt). */
static bool only_sent_debt_column(struct server_local* s, int cx, int cz) {
	bool found = false;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, s->worlds[0].chunks);
		!dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
		const int64_t key = dict_server_chunks_cref(it)->key;
		const struct server_chunk* c = &dict_server_chunks_cref(it)->value;
		if(!c->sent || !c->pub_debt)
			continue;
		if(S_CHUNK_X(key) != cx || S_CHUNK_Z(key) != cz)
			return false;
		found = true;
	}
	return found;
}
static bool no_sent_debt_column(struct server_local* s) {
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, s->worlds[0].chunks);
		!dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
		const struct server_chunk* c = &dict_server_chunks_cref(it)->value;
		if(c->sent && c->pub_debt)
			return false;
	}
	return true;
}
static bool column_debt_cleared(struct server_local* s, int cx, int cz) {
	struct server_chunk* c
		= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(cx, cz));
	return c && c->sent && !c->pub_debt;
}
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

unsigned server_local_test_publish_debt_w(struct server_local*, struct server_world*);
/* Flake fix (r5a-1024 PRIMED-ENV-DIAG): the G9X-6 publication refresh of
 * column (0,0) only runs on spare chunk slots, so whether it lands in the
 * captured update or one update later depends on how fast the drain thread
 * returns slots. Freezing the drain does not help (the 4 near sends then use
 * every spare slot and the refresh never fits), so instead settle: if the
 * refresh was deferred, run quiesced publication passes (test seam; a full
 * update is not usable while paused) until the debt is gone. The refresh itself is still required (bounded loop + assert). */
static void settle_publication_refresh(struct server_local* s, int cx, int cz) {
	for(int i = 0; i < 16 && !column_debt_cleared(s, cx, cz); i++) {
		test_drain_quiesce();
		(void)server_local_test_publish_debt_w(s, &s->worlds[0]);
	}
	assert(column_debt_cleared(s, cx, cz));
}

/* Counts CRPC_CHUNK sends for column (0,0) without the 16-entry order array
 * (used by the unpaused normal-update refresh check). */
static unsigned g_origin_chunk_sends = 0;
static void count_origin_chunk_rpcs(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_CHUNK && call->payload.chunk.x == 0
	   && call->payload.chunk.z == 0)
		g_origin_chunk_sends++;
}

static void tick_entities(struct server_local* s) {
	assert(server_local_test_tick_entities(s));
}

static void attach_manual_tick_entity(struct server_local* s,
									  struct entity* e) {
	/* Direct dictionary fixtures stand in for entities that production spawn or
	 * restore paths have already attached to a resident chunk.  Establish that
	 * precondition explicitly so the first pass tests their callback behavior,
	 * rather than Beta's addedToChunk=false reattachment-only frame. */
	e->pos[0] = (float)s->player.x;
	e->pos[1] = (float)s->player.y;
	e->pos[2] = (float)s->player.z;
	w_coord_t min_cx = WCOORD_CHUNK_OFFSET(
		(w_coord_t)floorf(e->pos[0]) - 32);
	w_coord_t max_cx = WCOORD_CHUNK_OFFSET(
		(w_coord_t)floorf(e->pos[0]) + 32);
	w_coord_t min_cz = WCOORD_CHUNK_OFFSET(
		(w_coord_t)floorf(e->pos[2]) - 32);
	w_coord_t max_cz = WCOORD_CHUNK_OFFSET(
		(w_coord_t)floorf(e->pos[2]) + 32);
	for(w_coord_t cx = min_cx; cx <= max_cx; cx++)
		for(w_coord_t cz = min_cz; cz <= max_cz; cz++) {
			if(server_world_is_chunk_loaded(&s->worlds[0], cx, cz))
				continue;
			struct server_chunk* chunk = NULL;
			assert(server_world_load_chunk(&s->worlds[0], cx, cz, &chunk) && chunk);
		}
	server_local_refresh_entity_membership(s, e);
	assert(e->added_to_chunk);
}

static void acknowledge_input_direct(struct server_local* s) {
	assert(s && s->input_ack_pending != 0);
	struct server_rpc ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = s->input_ack_pending,
	};
	server_local_test_process_rpc(s, &ack);
	assert(s->input_ack_pending == 0);
}

static void finish_same_world_respawn(struct server_local* s) {
	assert(s && s->world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(!s->player.finished_loading && s->input_ack_pending == 0);
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->player.x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->player.z));
	struct server_chunk* center
		= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(cx, cz));
	if(!center)
		assert(server_world_load_chunk(&s->worlds[0], cx, cz, &center));
	assert(center);
	center->sent = true; /* simulate the client collision-ready send */
	struct worldgen* generator = s->worlds[0].generator;
	s->worlds[0].generator = NULL; /* ignore unrelated unsent fixture neighbours */
	/* Respawn now drains the departed ring through bounded unload batches
	 * before admitting its center handoff. Let that real lifecycle finish;
	 * simulation remains suspended until acknowledge_input_direct below. */
	for(unsigned tick = 0; tick < 128 && !s->player.finished_loading; tick++) {
		server_local_test_update_once(s);
		/* REGRESS-1015: the client consumes each update's output before the
		 * next (the background drain thread stays the only consumer). A bare
		 * yield made the 128-update bound a host-scheduling input: with the
		 * G9X-6 refresh adding CHUNKs to the same 16-token FIFO, a lagging
		 * drain thread let the updates spin until the bound expired
		 * (observed 1 in 7 ctest runs). */
		test_drain_quiesce();
	}
	s->worlds[0].generator = generator;
	assert(s->player.finished_loading && s->input_ack_pending != 0);
	acknowledge_input_direct(s);
}

static uint32_t g_tick_order[700];
static size_t g_tick_order_count = 0;

static bool count_tick(struct entity* e, struct server_local* s) {
	(void)s;
	assert(g_tick_order_count < sizeof(g_tick_order) / sizeof(g_tick_order[0]));
	g_tick_order[g_tick_order_count++] = e->id;
	return false;
}

static struct entity* g_midpass_tnt = NULL;
static bool spawn_tnt_tick(struct entity* e, struct server_local* s) {
	(void)e;
	if(!g_midpass_tnt)
		g_midpass_tnt = server_local_spawn_primed_tnt(
			s, (vec3) {8.5f, 90.5f, 8.5f}, TNT_FUSE_DEFAULT);
	return false;
}

static size_t g_fresh_body_damage_ticks = 0;

static bool kill_old_player_body_tick(
	struct entity* e, struct server_local* s) {
	(void)e;
	/* Deviation-#18 split: a mid-pass kill commits only the DEAD state --
	 * no teleport happens here, so there is no fresh body to protect and
	 * the position-rejection latch stays down (it arms in the respawn
	 * transaction, which can only start from the RPC prepass). */
	assert(!s->reject_player_pos_this_update);
	assert(server_local_set_player_health(s, 0));
	assert(!s->reject_player_pos_this_update);
	assert(s->player.health <= 0);
	return false;
}

static bool damage_fresh_player_body_tick(
	struct entity* e, struct server_local* s) {
	(void)e;
	g_fresh_body_damage_ticks++;
	/* The later entity now ticks against the CORPSE: the dead-guard absorbs
	 * the hit with no health/armor/i-frame churn (Beta
	 * attackEntityFrom:312), shaped like an i-frame absorb for the caller. */
	assert(server_local_damage_player(
		s, HEALTH_PER_HEART / 2, 0.0f, 0.0f));
	assert(s->player.health <= 0);
	return false;
}

static void clear_entities(struct server_local* s) {
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	assert(s->entity_dict_reserved == 0);
	dict_entity_reset(s->entities);
}

/* 740c583 Minecraft.changeWorld -> func_6255_d loads terrain/lighting
 * synchronously before runTick resumes entity damage and the world clock.
 * Our split-thread equivalent must keep streaming while that primary
 * presentation acknowledgement is held. This witness invokes production
 * damage from the entity pass; it does not substitute damage/AI rules. */
static size_t g_loading_damage_ticks;
static bool loading_damage_tick(struct entity* e, struct server_local* s) {
	(void)e;
	g_loading_damage_ticks++;
	assert(server_local_damage_player(s, HEALTH_PER_HEART, 0.0f, 0.0f));
	return false;
}

static void test_loading_stream_and_damage_barrier(struct server_local* s,
												  struct server_chunk* center) {
	struct server_player saved_player = s->player;
	struct random_gen saved_rng = s->rand_src;
	uint64_t saved_time = s->world_time;
	struct worldgen* generator = s->worlds[0].generator;
	bool populated = center->populated, sent = center->sent;
	bool modified = center->modified;
	/* One already-populated unsent column represents a near-send watchdog
	 * fallback. Disable unrelated generation, not streaming of that column. */
	center->populated = true;
	center->modified = true;
	assert(server_world_save_chunk(&s->worlds[0], false, 0, 0));
	s->worlds[0].generator = NULL;
	center->sent = false;
	s->player.x = s->player.z = 8.5;
	s->player.y = 100.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.hurt_resist_time = 0;
	s->player.finished_loading = true;
	s->input_ack_pending = 0x51A7u;
	struct entity* damage = calloc(1, sizeof(*damage));
	assert(damage);
	damage->id = 0x51A7u;
	damage->delay_destroy = -1;
	damage->tick_server = loading_damage_tick;
	*dict_entity_safe_get(s->entities, damage->id) = damage;
	attach_manual_tick_entity(s, damage);
	g_loading_damage_ticks = 0;

	/* Paused container recovery also needs its near stream to finish. */
	s->paused = true;
	server_local_test_update_once(s);
	assert(center->sent);
	s->paused = false;
	for(unsigned tick = 0; tick < 600; tick++) {
		struct server_rpc stale = {.type = SRPC_INPUT_READY,
			.payload.input_ready.epoch = 0x51A6u};
		server_local_test_process_rpc(s, &stale);
		server_local_test_update_once(s);
		assert(s->input_ack_pending == 0x51A7u);
		assert(s->world_time == saved_time);
		assert(s->player.health == MAX_PLAYER_HEALTH);
		assert(g_loading_damage_ticks == 0);
		assert(memcmp(&s->rand_src, &saved_rng, sizeof(saved_rng)) == 0);
	}
	acknowledge_input_direct(s);
	server_local_test_update_once(s);
	assert(g_loading_damage_ticks == 1);
	assert(s->world_time == saved_time + 1);
	assert(s->player.health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART);
	clear_entities(s);
	s->player = saved_player;
	s->rand_src = saved_rng;
	s->world_time = saved_time;
	s->worlds[0].generator = generator;
	center->populated = populated;
	center->sent = sent;
	center->modified = modified;
	puts("PASS loading: near stream advances while paused/ACK-held; 600 updates "
		 "and stale ACKs preserve clock/health/RNG; matching ACK restores damage");
}

static struct entity* only_primed_tnt(struct server_local* s) {
	assert(dict_entity_size(s->entities) == 1);
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	assert(!dict_entity_end_p(it));
	struct entity* e = dict_entity_ref(it)->value;
	assert(e && e->type == ENTITY_PRIMED_TNT);
	return e;
}

static size_t count_primed_tnt(struct server_local* s) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_PRIMED_TNT)
			count++;
		dict_entity_next(it);
	}
	return count;
}

static void assert_tnt_motion_equal(const struct entity* a,
									const struct entity* b) {
	assert(memcmp(a->pos, b->pos, sizeof(a->pos)) == 0);
	assert(memcmp(a->pos_old, b->pos_old, sizeof(a->pos_old)) == 0);
	assert(memcmp(a->vel, b->vel, sizeof(a->vel)) == 0);
	assert(a->on_ground == b->on_ground);
	assert(a->data.primed_tnt.fuse == b->data.primed_tnt.fuse);
}

int main(void) {
	test_fixture_dir("test_tnt_primed_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	/* EntityTNTPrimed.onUpdate -> one smoke request at posY + 0.5. The
	 * ambient helper intentionally emits four jittered sprites, so exercise the
	 * dedicated production helper directly and pin the user-visible contract. */
	particle_init();
	particle_clear_all();
	particle_set_camera((vec3) {2.0f, 3.5f, 4.0f});
	gstate.rand_src.seed = UINT32_C(0x13579BDF);
	particle_generate_tnt_smoke((vec3) {2.0f, 3.0f, 4.0f});
	assert(particle_test_count() == 1);
	struct particle smoke;
	assert(particle_test_get(0, &smoke));
	assert(fabsf(smoke.pos[0] - 2.0f) < 1.0e-6f);
	assert(fabsf(smoke.pos[1] - 3.5f) < 1.0e-6f);
	assert(fabsf(smoke.pos[2] - 4.0f) < 1.0e-6f);
	assert(smoke.lifetime >= 8 && smoke.lifetime <= 40);
	particle_clear_all();
	for(int i = 0; i < 12000; i++)
		particle_generate_tnt_smoke(
			(vec3) {2.0f + (float)i * 0.001f, 3.0f, 4.0f});
	assert(particle_test_count() == 4000);
	assert(particle_test_evictions(TEXTURE_ATLAS_PARTICLES) == 8000);
	assert(particle_test_get(0, &smoke));
	assert(fabsf(smoke.pos[0] - 10.0f) < 1.0e-5f); /* logical oldest */
	particle_clear_all();
	assert(particle_test_evictions(TEXTURE_ATLAS_PARTICLES) == 0);

	/* Exercise the actual wire adapter, not an on_server=true surrogate: spawn
	 * creates a renderable TNT client entity; the interval-10 authoritative
	 * correction updates position, velocity and fuse without emitting smoke.
	 * [server decompile 740c583 EntityTracker: EntityTNTPrimed 160,10,true] */
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	struct client_rpc client_reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = 0,
		},
	};
	clin_process(&client_reset);
	assert(gstate.players[0].local_player);
	struct client_rpc client_spawn = {
		.type = CRPC_SPAWN_PRIMED_TNT,
		.payload.spawn_primed_tnt = {
			.entity_id = 42,
			.pos = {1.5f, 70.5f, 2.5f},
			.vel = {0.02f, 0.2f, -0.01f},
			.fuse = 80,
		},
	};
	clin_process(&client_spawn);
	struct entity** client_tnt_slot = dict_entity_get(gstate.entities, 42);
	assert(client_tnt_slot && *client_tnt_slot);
	struct entity* rpc_tnt = *client_tnt_slot;
	assert(rpc_tnt->type == ENTITY_PRIMED_TNT && rpc_tnt->render);
	rpc_tnt->data.primed_tnt.awaiting_destroy = true;
	size_t smoke_before_correction = particle_test_count();
	struct client_rpc client_correction = {
		.type = CRPC_PRIMED_TNT_CORRECTION,
		.payload.primed_tnt_correction = {
			.entity_id = 42,
			.pos = {3.25f, 71.0f, 4.75f},
			.vel = {-0.03f, 0.1f, 0.04f},
			.fuse = 37,
		},
	};
	clin_process(&client_correction);
	assert(memcmp(rpc_tnt->pos,
			  client_correction.payload.primed_tnt_correction.pos,
			  sizeof(rpc_tnt->pos)) == 0);
	assert(memcmp(rpc_tnt->vel,
			  client_correction.payload.primed_tnt_correction.vel,
			  sizeof(rpc_tnt->vel)) == 0);
	assert(rpc_tnt->data.primed_tnt.fuse == 37);
	assert(!rpc_tnt->data.primed_tnt.awaiting_destroy);
	assert(particle_test_count() == smoke_before_correction);
	struct client_rpc client_destroy = {
		.type = CRPC_ENTITY_DESTROY,
		.payload.entity_destroy.entity_id = 42,
	};
	clin_process(&client_destroy);
	assert(dict_entity_get(gstate.entities, 42) == NULL);
	for(size_t i = 0; i < 256; i++) {
		if(gstate.players[0].windows[i]) {
			windowc_destroy(gstate.players[0].windows[i]);
			free(gstate.players[0].windows[i]);
			gstate.players[0].windows[i] = NULL;
		}
	}
	free(gstate.players[0].local_player);
	gstate.players[0].local_player = NULL;
	dict_entity_erase(gstate.entities, 0);
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	struct server_local s;
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 128; /* shipping Wii policy */
	s.rand_src = (struct random_gen) {.seed = 97531};
	/* F4-RNG: chain membership/order depends on World.rand's blast rays;
	 * body pitch and TNT launch draw their own streams. */
	jrand_set_seed(&s.worlds[0].rand, 4242);
	server_local_test_seed_body_rand(&s, &s.player, 13579);
	server_local_test_seed_math_random(1357);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
						INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;

	/* Beta's entity update gate requires every chunk in the entity's
	 * position +/-32 block neighborhood.  Keep the origin scenes resident
	 * across that whole footprint so this fixture exercises the TNT/damage
	 * behavior rather than the unloaded-neighborhood suspension rule. */
	struct server_chunk* sc = NULL;
	for(int lcx = -2; lcx <= 4; lcx++)
		for(int lcz = -2; lcz <= 4; lcz++) {
			sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], lcx, lcz, &sc) && sc);
			sc->sent = true;
		}
	sc = dict_server_chunks_get(s.worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert(sc);
	struct entity ordinary_fuse_zero;
	entity_primed_tnt(
		UINT32_C(999), &ordinary_fuse_zero, true, &s.worlds[0], 0);
	assert(!entity_primed_tnt_has_pending_explosion(&ordinary_fuse_zero));
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Deviation-#18: zero-init health now reads as DEAD (lethal writes and
	 * TOGGLE_PAUSE reject; crosshair actions drain inert). The old atomic
	 * death path even RESURRECTED a zero-health harness player on the first
	 * lethal touch, masking this hole; be explicitly alive like every live
	 * player is. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 100.0;
	s.player.y = 70.0;
	s.player.z = 100.0; /* safely outside the blast */
	test_loading_stream_and_damage_barrier(&s, sc);

	/* Both production routing functions must execute the same Beta onUpdate
	 * physics/fuse step. Give the client route the same server-world collision
	 * adapter solely for this A/B fixture, then demand byte-identical state after
	 * every live update. The client route additionally emits exactly one smoke.
	 * [decompile 740c583 EntityTNTPrimed.onUpdate] */
	struct entity server_tnt, client_tnt;
	entity_primed_tnt(1001, &server_tnt, true, &s.worlds[0], 40);
	entity_primed_tnt(1002, &client_tnt, false, &s.worlds[0], 40);
	client_tnt.on_server = true;
	vec3 equiv_pos = {4.5f, 70.5f, 4.5f};
	vec3 equiv_vel = {0.015f, 0.2f, -0.011f};
	server_tnt.teleport(&server_tnt, equiv_pos);
	client_tnt.teleport(&client_tnt, equiv_pos);
	glm_vec3_copy(equiv_vel, server_tnt.vel);
	glm_vec3_copy(equiv_vel, client_tnt.vel);
	particle_clear_all();
	particle_set_camera((vec3) {4.5f, 71.0f, 4.5f});
	for(int update = 0; update < 20; update++) {
		assert(!server_tnt.tick_server(&server_tnt, &s));
		assert(!client_tnt.tick_client(&client_tnt));
		assert_tnt_motion_equal(&server_tnt, &client_tnt);
	}
	assert(particle_test_count() == 20);
	particle_clear_all();

	/* Remove load-time fixture ticks, then saturate the ring with unique future
	 * moving-fluid entries. Future entries are conserved/requeued and consume no
	 * per-tick fluid budget, so the ring remains full across the fuse. */
	struct scheduled_tick discarded;
	while(server_world_scheduled_tick_pop(&s.worlds[0], &discarded)) {
	}
	for(size_t i = 0; i < s.worlds[0].scheduled_tick_capacity; i++)
		assert(server_world_schedule_tick_raw(
			&s.worlds[0], (w_coord_t)(10000 + i), 64, 0, BLOCK_WATER_FLOW, 10000));
	assert(s.worlds[0].scheduled_tick_count == s.worlds[0].scheduled_tick_capacity);

	/* Detached memory and live-list capacity are insufficient if M-lib safe_get
	 * can still rehash after AIR. Force the dictionary-capacity preflight itself
	 * to fail and require the exact same no-mutation outcome. */
	const int dict_fail_x = 4, dict_fail_y = 70, dict_fail_z = 8;
	assert(server_world_set_block(
		&s.worlds[0], dict_fail_x, dict_fail_y, dict_fail_z,
		(struct block_data) {.type = BLOCK_TNT}));
	g_watch_x = dict_fail_x;
	g_watch_y = dict_fail_y;
	g_watch_z = dict_fail_z;
	g_watch_block = true;
	g_watched_set_block_rpcs = 0;
	g_watched_order_count = 0;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	server_local_test_fail_next_entity_dict_reserve();
	server_world_begin_update(&s.worlds[0]);
	server_world_explode(
		&s.worlds[0],
		(vec3) {dict_fail_x + 0.5f, dict_fail_y + 0.5f,
				  dict_fail_z + 0.5f},
		4.0f);
	clin_test_set_rpc_observer(NULL, NULL);
	g_watch_block = false;
	struct block_data dict_after;
	assert(server_world_get_block(
		&s.worlds[0], dict_fail_x, dict_fail_y, dict_fail_z, &dict_after));
	assert(dict_after.type == BLOCK_TNT);
	assert(g_watched_set_block_rpcs == 0);
	assert(count_primed_tnt(&s) == 0);
	assert(server_world_set_block(
		&s.worlds[0], dict_fail_x, dict_fail_y, dict_fail_z, (struct block_data) {0}));
	puts("  chain dictionary OOM: TNT retained before AIR");

	/* Explosion failure must be transactional. Beta's AIR notify precedes the
	 * chain constructor, so allocating afterward and restoring on OOM leaks a
	 * false AIR transition to both client and neighbours. The production code
	 * now reserves detached memory/worklist capacity first: forced failure keeps
	 * the TNT, emits no SET_BLOCK for its cell, and never calls an adjacent
	 * indestructible block's neighbour hook. */
	const int fail_x = 13, fail_y = 70, fail_z = 8;
	assert(server_world_set_block(
		&s.worlds[0], fail_x, fail_y, fail_z,
		(struct block_data) {.type = BLOCK_TNT}));
	assert(server_world_set_block(
		&s.worlds[0], fail_x + 1, fail_y, fail_z,
		(struct block_data) {.type = BLOCK_TNT}));
	assert(server_world_set_block(
		&s.worlds[0], fail_x - 1, fail_y, fail_z,
		(struct block_data) {.type = BLOCK_BEDROCK}));
	void (*old_bedrock_neighbour)(struct server_local*, struct block_info*)
		= block_bedrock.onNeighbourBlockChange;
	block_bedrock.onNeighbourBlockChange = count_watched_neighbour_callback;
	g_watch_x = fail_x;
	g_watch_y = fail_y;
	g_watch_z = fail_z;
	g_watch_block = true;
	g_watched_set_block_rpcs = 0;
	g_watched_neighbour_callbacks = 0;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	/* F4-RNG (f049fa3d8) uses the Java-6 HashSet list, not ray-insertion
	 * order. With the pinned rays, this arena reserves x=14 before x=13.
	 * Fail the second reservation and watch its bedrock neighbour at x=12:
	 * the successful x=14 removal cannot itself notify that neighbour. */
	server_local_test_fail_entity_alloc_after(2);
	server_world_begin_update(&s.worlds[0]);
	server_world_explode(
		&s.worlds[0], (vec3) {fail_x + 0.5f, fail_y + 0.5f, fail_z + 0.5f},
		4.0f);
	clin_test_set_rpc_observer(NULL, NULL);
	g_watch_block = false;
	block_bedrock.onNeighbourBlockChange = old_bedrock_neighbour;
	struct block_data failed_chain;
	assert(server_world_get_block(
		&s.worlds[0], fail_x, fail_y, fail_z, &failed_chain));
	assert(failed_chain.type == BLOCK_TNT);
	assert(server_world_get_block(
		&s.worlds[0], fail_x + 1, fail_y, fail_z, &failed_chain));
	assert(failed_chain.type == BLOCK_AIR);
	assert(count_primed_tnt(&s) == 1);
	assert(g_watched_set_block_rpcs == 0);
	assert(g_watched_neighbour_callbacks == 0);
	clear_entities(&s);
	/* Remove the retained center; the successful peer is already AIR, so
	 * its identical Beta setBlock is a false-returning no-op. */
	assert(server_world_set_block(
		&s.worlds[0], fail_x, fail_y, fail_z, (struct block_data) {0}));
	assert(!server_world_set_block(
		&s.worlds[0], fail_x + 1, fail_y, fail_z, (struct block_data) {0}));
	assert(server_world_set_block(
		&s.worlds[0], fail_x - 1, fail_y, fail_z, (struct block_data) {0}));
	printf("  chain OOM: one TNT joined; failed peer retained with no AIR "
		   "RPC/callback\n");

	/* BlockTNT.onBlockDestroyedByExplosion: clear the block and spawn a moving
	 * entity with World.rand.nextInt(20)+10, even while the block-tick ring is
	 * full. This is a separate constructor path from the default 80-tick fuse. */
	const int chain_x = 12, chain_y = 70, chain_z = 8;
	assert(server_world_set_block(
		&s.worlds[0], chain_x, chain_y, chain_z,
		(struct block_data) {.type = BLOCK_TNT}));
	server_world_begin_update(&s.worlds[0]);
	server_world_explode(
		&s.worlds[0], (vec3) {chain_x + 0.5f, chain_y + 0.5f, chain_z + 0.5f},
		4.0f);
	struct block_data chain_after;
	assert(server_world_get_block(
		&s.worlds[0], chain_x, chain_y, chain_z, &chain_after));
	assert(chain_after.type == BLOCK_AIR);
	struct entity* chained = only_primed_tnt(&s);
	assert(chained->data.primed_tnt.fuse >= 10
		   && chained->data.primed_tnt.fuse <= 29);
	free(chained);
	dict_entity_reset(s.entities);

	const int x = 8, y = 70, z = 8;
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_TNT, .metadata = 0}));
	struct block_data tnt;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &tnt));
	struct block_info on = {
		.block = &tnt, .neighbours = NULL, .x = x, .y = y, .z = z,
	};
	struct item_data flint = {
		.id = ITEM_FLINT_STEEL, .durability = 0, .count = 1,
	};
	/* Beta BlockTNT.blockActivated returns false; the Wii's documented
	 * right-click ignition adaptation belongs to the flint item so other held
	 * items can still reach their normal use/placement fallback. The separate
	 * test_flint_steel fixture drives the full SRPC dispatch; this lifecycle
	 * fixture invokes the item seam directly while the scheduled ring is full. */
	assert(!blocks[BLOCK_TNT]->onRightClick);
	assert(item_flint_steel.onUseOnBlock);
	g_watch_x = x;
	g_watch_y = y;
	g_watch_z = z;
	g_watch_block = true;
	g_watched_order_count = 0;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	assert(item_flint_steel.onUseOnBlock(&s, &s.player, &flint, &on, SIDE_TOP));
	clin_test_set_rpc_observer(NULL, NULL);
	g_watch_block = false;
	assert(g_watched_order_count >= 2);
	assert(g_watched_order[0] == CRPC_SPAWN_PRIMED_TNT);
	assert(g_watched_order[1] == CRPC_SET_BLOCK);
	assert(flint.id == ITEM_FLINT_STEEL && flint.count == 1
		   && flint.durability == 1);

	/* Beta contract: the block is gone and the primed object is an entity, not a
	 * metadata block waiting for admission to the scheduled-tick ring. */
	struct block_data after;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &after));
	assert(after.type == BLOCK_AIR);
	only_primed_tnt(&s);
	assert(s.worlds[0].scheduled_tick_count == s.worlds[0].scheduled_tick_capacity);

	g_tnt_correction_rpcs = 0;
	g_explosion_rpcs = 0;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	/* Tracker corrections are deliberately non-blocking in production. Establish
	 * the test's intended uncongested-transport precondition synchronously;
	 * relying only on the drain thread made this exact cadence assertion depend
	 * on host scheduling under parallel CTest load. */
	test_drain_quiesce(); /* REGRESS-1015: drain thread is the only consumer */
	for(int update = 1; update <= 80; update++) {
		server_world_begin_update(&s.worlds[0]);
		tick_entities(&s);
		assert(dict_entity_size(s.entities) == 1); /* fuse 79 .. 0, no early blast */
		test_drain_quiesce(); /* REGRESS-1015: drain thread is the only consumer */
		server_world_tick(&s.worlds[0], &s);
		test_drain_quiesce(); /* REGRESS-1015: drain thread is the only consumer */
		assert(s.worlds[0].scheduled_tick_count == s.worlds[0].scheduled_tick_capacity);
	}

	server_world_begin_update(&s.worlds[0]);
	tick_entities(&s); /* update 81 enters with fuse 0 -> power-4 explosion */
	clin_test_set_rpc_observer(NULL, NULL);
	assert(g_tnt_correction_rpcs == 8); /* EntityTracker interval 10 */
	assert(g_explosion_rpcs == 1); /* terminal event emitted on actual begin */
	assert(count_primed_tnt(&s) == 0); /* blast drops may now be item entities */
	server_world_tick(&s.worlds[0], &s);
	assert(server_world_get_block(&s.worlds[0], x, y, z, &after));
	assert(after.type == BLOCK_AIR);

	/* A normal fuse 1->0 update has not attempted its final physics/explosion
	 * edge yet. It must remain ordinary entity state until the next entity pass,
	 * never enter the cap-owner pre-RPC retry merely because its fuse is zero. */
	clear_entities(&s);
	struct entity* natural_zero = server_local_spawn_primed_tnt(
		&s, (vec3) {8.5f, 110.5f, 8.5f}, 1);
	assert(natural_zero);
	tick_entities(&s);
	assert(natural_zero->data.primed_tnt.fuse == 0);
	assert(!natural_zero->data.primed_tnt.explosion_pending);
	uint32_t natural_zero_id = natural_zero->id;
	struct server_rpc pause_before_final = {.type = SRPC_TOGGLE_PAUSE};
	assert(svin_rpc_send(&pause_before_final));
	g_prepass_order_count = 0;
	g_capture_prepass_order = true;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	/* F2a d86e8d673: the regular world/entity pass precedes the queued
	 * pause. Pin the near stream to four sends so the full event order is
	 * reproducible, rather than making wall-clock admission an input. */
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	/* G9X-6: exactly one sent column, (0,0), owes the client a refresh */
	const bool natural_zero_debt_before = only_sent_debt_column(&s, 0, 0);
	/* The refresh runs only on spare chunk slots (4 held in reserve), so
	 * whether it fires in THIS update depends on how many slots the client
	 * has returned. Wait until the background drain thread has emptied the
	 * client inbox (all slots back) so the order is not a scheduler input;
	 * test_drain_quiesce keeps that thread the only consumer (the inbox's
	 * packed-message replay state is unsynchronised). */
	test_drain_quiesce();
	server_local_test_update_once(&s);
	/* The refresh needs spare chunk slots; it lands here (7 events) or, if the
	 * drain thread was slow, in the settle passes below (capture stays ON, so
	 * the republish is still observed and counted). */
	const bool natural_zero_refresh_in_window = g_prepass_order_count == 7;
	settle_publication_refresh(&s, 0, 0);
	g_capture_prepass_order = false;
	clin_test_set_rpc_observer(NULL, NULL);
	server_local_test_set_near_send_minimum(0);
	server_local_test_expire_near_budget(false);
	fprintf(stderr, "natural-zero world/network order (%zu):", g_prepass_order_count);
	for(size_t i = 0; i < g_prepass_order_count; i++)
		fprintf(stderr, " %d", g_prepass_order[i]);
	fputc('\n', stderr);
	/* G9X-6 re-record (REGRESS-1015, maintainer ruling 2026-10-14; old pin:
	 * 6 = EXPLODE, 4 near CHUNKs, TIME_SET): after the four near sends the
	 * post-drain publication refresh republishes column (0,0), the only
	 * sent column holding publication debt -- the light stores of the TNT
	 * placements/explosions at (12,70,8) and (8,70,8) above (this fixture
	 * has no generator, so no population notify write can set it; Beta
	 * publishes every stored light change, CFR World.java:512-517).
	 * Attribution: tmp/jobs/loop/REGRESS-1015/RERECORD.md. */
	assert(natural_zero_debt_before);
	/* Across both phases (update + settle): exactly 7 events, EXPLODE, the 4
	 * near sends, exactly ONE (0,0) CHUNK and one TIME_SET in the last two
	 * slots (refresh then TIME_SET if it fit the window, else TIME_SET then
	 * the settled refresh). */
	assert(g_prepass_order_count == 7);
	assert(g_prepass_order[0] == CRPC_ENTITY_EXPLODE);
	for(size_t i = 1; i <= 4; i++) { /* the near sends: unsent columns */
		assert(g_prepass_order[i] == CRPC_CHUNK);
		assert(g_prepass_chunk_x[i] != 0 || g_prepass_chunk_z[i] != 0);
	}
	if(natural_zero_refresh_in_window) {
		assert(g_prepass_order[5] == CRPC_CHUNK);
		assert(g_prepass_chunk_x[5] == 0 && g_prepass_chunk_z[5] == 0);
		assert(g_prepass_order[6] == CRPC_TIME_SET);
	} else {
		assert(g_prepass_order[5] == CRPC_TIME_SET);
		assert(g_prepass_order[6] == CRPC_CHUNK);
		assert(g_prepass_chunk_x[6] == 0 && g_prepass_chunk_z[6] == 0);
	}
	assert(column_debt_cleared(&s, 0, 0));
	struct entity** natural_zero_slot
		= dict_entity_get(s.entities, natural_zero_id);
	assert(!natural_zero_slot); /* final ordinary entity step exploded/reaped it */
	assert(s.paused);
	s.paused = false;
	clear_entities(&s);
	/* The settle helper calls the publisher directly, which would hide it if
	 * the normal update ever stopped calling it. Prove the NORMAL, unpaused
	 * update path republishes a debted (0,0): force the debt (and drop the
	 * accepted digest so it cannot cancel out), drain the transport before
	 * each update, keep the near-send minimum at 0 (no forced competing near
	 * sends), then require exactly one republish within 16 updates. 16: the
	 * refresh needs only spare chunk slots, which one drained update
	 * restores; 16 is far above that yet bounded. */
	{
		struct server_chunk* origin_sc
			= dict_server_chunks_get(s.worlds[0].chunks, S_CHUNK_ID(0, 0));
		assert(origin_sc && origin_sc->sent);
		origin_sc->pub_debt = true;
		origin_sc->pub_digest_valid = false;
		g_origin_chunk_sends = 0;
		clin_test_set_rpc_observer(count_origin_chunk_rpcs, NULL);
		server_local_test_set_near_send_minimum(0);
		server_local_test_expire_near_budget(false);
		for(int u = 0; u < 16 && origin_sc->pub_debt; u++) {
			test_drain_quiesce();
			server_local_test_update_once(&s);
		}
		clin_test_set_rpc_observer(NULL, NULL);
		assert(!origin_sc->pub_debt);
		assert(g_origin_chunk_sends == 1);
	}
	printf("  natural fuse zero: regular entity step exploded before network pause; "
		   "no cap-owner prepass retry OK\n");

	/* Wii pacing may delay an explosion, but it may not change Beta's lifecycle
	 * contract into "entity vanished before exploding." EntityTNTPrimed.onUpdate
	 * calls createExplosion synchronously before setEntityDead returns. Under the
	 * two-blast Wii cap, the third fuse-zero source must therefore remain a live,
	 * archiveable entity until a later update actually admits its blast; only then
	 * may the reaper publish DESTROY, after EXPLODE. This is the real-Wii loss case,
	 * not a queue-unit surrogate.
	 * [decompile 740c583 EntityTNTPrimed.onUpdate + explode;
	 *  Explosion.doExplosionA/doExplosionB] */
	clear_entities(&s);
	server_world_begin_update(&s.worlds[0]);
	g_explosion_rpcs = 0;
	g_terminal_order_count = 0;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	uint32_t capped_ids[3];
	for(int i = 0; i < 3; i++) {
		struct entity* capped = server_local_spawn_primed_tnt(
			&s, (vec3) {2.5f + i * 5.0f, 100.5f, 2.5f}, 0);
		assert(capped);
		capped_ids[i] = capped->id;
	}
	tick_entities(&s);
	assert(count_primed_tnt(&s) == 1);
	assert(g_explosion_rpcs == FCAVEX_MAX_EXPLOSIONS_PER_TICK);
	assert(g_terminal_order_count == 4);
	for(size_t i = 0; i < g_terminal_order_count; i += 2) {
		assert(g_terminal_order[i] == CRPC_ENTITY_EXPLODE);
		assert(g_terminal_order[i + 1] == CRPC_ENTITY_DESTROY);
		assert(g_terminal_destroy_ids[i + 1] == capped_ids[i / 2]);
	}
	struct entity* retained = only_primed_tnt(&s);
	assert(retained->id == capped_ids[2]);
	assert(retained->data.primed_tnt.fuse == 0);
	assert(retained->data.primed_tnt.explosion_pending);

	/* Portal/menu teardown checkpoints resident chunks before freeing the live
	 * dictionary. Pin that production gather/encode boundary here: the capped TNT
	 * is durable entity state, never a volatile queued blast that teardown drops. */
	struct chunk_entities checkpoint = {0};
	assert(server_local_gather_chunk_entities(&s, 0, 0, &checkpoint));
	bool found_retained = false;
	for(size_t i = 0; i < checkpoint.count; i++) {
		const struct archived_entity* archived = &checkpoint.entities[i];
		if(archived->source_id == capped_ids[2]) {
			assert(archived->kind == ARCHIVED_ENTITY_PRIMED_TNT);
			assert(archived->data.primed_tnt.fuse == 0);
			assert(archived->data.primed_tnt.explosion_pending);
			found_retained = true;
		}
	}
	assert(found_retained);
	void* checkpoint_arena = NULL;
	assert(chunk_entities_build(&checkpoint, &checkpoint_arena));
	chunk_entities_build_free(checkpoint_arena);
	/* Model the actual teardown/reload boundary, not only encodability. A normal
	 * live fuse-zero entity would restore with pending=false; this cap owner must
	 * restore with the explicit pending bit and remain eligible for pre-RPC retry. */
	clear_entities(&s);
	assert(server_local_restore_chunk_entities(&s, &checkpoint));
	retained = only_primed_tnt(&s);
	assert(retained->data.primed_tnt.fuse == 0);
	assert(retained->data.primed_tnt.explosion_pending);
	capped_ids[2] = retained->id;
	chunk_entities_free(&checkpoint);

	server_world_tick(&s.worlds[0], &s); /* same window: source remains live */
	assert(g_explosion_rpcs == 2);
	assert(count_primed_tnt(&s) == 1);
	server_world_begin_update(&s.worlds[0]);
	tick_entities(&s);
	assert(count_primed_tnt(&s) == 0);
	assert(g_explosion_rpcs == 3);
	server_world_tick(&s.worlds[0], &s);
	assert(g_explosion_rpcs == 3); /* no queued duplicate */
	assert(g_terminal_order_count == 6);
	assert(g_terminal_order[4] == CRPC_ENTITY_EXPLODE);
	assert(g_terminal_order[5] == CRPC_ENTITY_DESTROY);
	assert(g_terminal_destroy_ids[5] == capped_ids[2]);
	assert(s.worlds[0].scheduled_tick_count == s.worlds[0].scheduled_tick_capacity);

	/* Ownerless/direct calls (Nether beds and debug fixtures) remain synchronous
	 * even when the entity cap is occupied; they never enter a volatile teardown
	 * queue. Entity-owned TNT and creepers retain their complete source state and
	 * retry after the next logical-update reset. */
	server_world_begin_update(&s.worlds[0]);
	for(int i = 0; i < FCAVEX_MAX_EXPLOSIONS_PER_TICK; i++)
		assert(server_world_try_begin_explosion(&s.worlds[0]));
	server_world_explode(
		&s.worlds[0], (vec3) {13.5f, 120.5f, 13.5f}, PRIMED_TNT_EXPLOSION_POWER);
	assert(g_explosion_rpcs == 4);
	uint32_t deferrals_before = s.worlds[0].explosion_deferrals;
	struct entity* retry = server_local_spawn_primed_tnt(
		&s, (vec3) {8.5f, 120.5f, 8.5f}, 0);
	assert(retry);
	uint32_t retry_id = retry->id;
	struct entity retry_before = *retry;
	struct entity* capped_creeper = server_local_spawn_monster(
		(vec3) {18.5f, 120.5f, 18.5f}, MONSTER_CREEPER, &s);
	assert(capped_creeper);
	capped_creeper->data.monster.fuse = CREEPER_FUSE_TICKS;
	uint32_t creeper_id = capped_creeper->id;
	tick_entities(&s);
	struct entity** retry_slot = dict_entity_get(s.entities, retry_id);
	assert(retry_slot && *retry_slot);
	retry = *retry_slot;
	assert_tnt_motion_equal(retry, &retry_before);
	struct entity** capped_creeper_slot
		= dict_entity_get(s.entities, creeper_id);
	assert(capped_creeper_slot && *capped_creeper_slot);
	capped_creeper = *capped_creeper_slot;
	assert(capped_creeper->data.monster.fuse == CREEPER_FUSE_TICKS);
	assert(s.worlds[0].explosion_deferrals == deferrals_before + 2);
	assert(g_explosion_rpcs == 4);
	server_world_begin_update(&s.worlds[0]);
	tick_entities(&s);
	assert(count_primed_tnt(&s) == 0);
	assert(dict_entity_get(s.entities, creeper_id) == NULL);
	assert(g_explosion_rpcs == 6);
	clin_test_set_rpc_observer(NULL, NULL);
	printf("  lossless cap: TNT + terminal-fuse creeper retained as sources; "
		   "direct blast stayed synchronous; both retried exactly once\n");

	/* The death-switch section below intentionally proves a bounded client RPC
	 * prefix with an eight-entry recorder.  Its pending explosions run in the
	 * prepass, before ordinary entity activation.  Retire the activation-only
	 * neighbors now so this independent section retains its original one-column
	 * stream premise. */
	server_world_light_drain(&s.worlds[0]); /* G9: unloads wait for the drain */
	for(int lcx = -2; lcx <= 4; lcx++)
		for(int lcz = -2; lcz <= 4; lcz++) {
			if(lcx == 0 && lcz == 0)
				continue;
			test_drain_quiesce();
			assert(server_world_save_chunk(&s.worlds[0], true, lcx, lcz));
		}

	/* A cap-retained terminal creeper is an immutable explosion owner, not a
	 * merely swollen ordinary mob.  It must survive hazard/death/despawn/motion,
	 * persist through the chunk archive, reject a stale attack, and retry before
	 * any queued client input can relocate the player.  The final assertions use
	 * a real production update: EXPLODE must precede TOGGLE_PAUSE's TIME_SET, and
	 * the position sample queued by the dead player must not move the fresh body
	 * away from spawn. */
	clear_entities(&s);
	server_world_begin_update(&s.worlds[0]);
	for(int i = 0; i < FCAVEX_MAX_EXPLOSIONS_PER_TICK; i++)
		assert(server_world_try_begin_explosion(&s.worlds[0]));
	/* Keep the fixture player inside Beta's 128-block natural-despawn radius.
	 * The cap is full, so this tick cannot damage the player; the later lethal
	 * retry deliberately relocates them to owned_center. */
	s.player.x = 20.5f;
	s.player.y = 100.5f;
	s.player.z = 20.5f;
	struct entity* durable_creeper = server_local_spawn_monster(
		(vec3) {8.5f, 100.5f, 8.5f}, MONSTER_CREEPER, &s);
	assert(durable_creeper);
	durable_creeper->data.monster.fuse = CREEPER_FUSE_TICKS;
	assert(!durable_creeper->tick_server(durable_creeper, &s));
	assert(durable_creeper->data.monster.explosion_pending);
	assert(durable_creeper->data.monster.fuse == CREEPER_FUSE_TICKS);

	vec3 owned_center;
	memcpy(owned_center, durable_creeper->pos, sizeof(owned_center));
	durable_creeper->health = 1;
	durable_creeper->data.monster.fire_time = 20;
	durable_creeper->data.monster.entity_age = 100000;
	durable_creeper->vel[0] = 0.25f;
	durable_creeper->vel[1] = -0.75f;
	durable_creeper->vel[2] = -0.125f;
	test_mob_sync(durable_creeper); /* motion units S2 */
	vec3 owned_velocity;
	memcpy(owned_velocity, durable_creeper->vel, sizeof(owned_velocity));
	assert(!durable_creeper->tick_server(durable_creeper, &s));
	assert(durable_creeper->health == 1);
	assert(durable_creeper->data.monster.fire_time == 20);
	assert(durable_creeper->data.monster.entity_age == 100000);
	assert(memcmp(durable_creeper->pos, owned_center, sizeof(owned_center)) == 0);
	assert(memcmp(durable_creeper->vel, owned_velocity,
			  sizeof(owned_velocity)) == 0);

	w_coord_t owner_cx
		= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(owned_center[0]));
	w_coord_t owner_cz
		= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(owned_center[2]));
	struct chunk_entities creeper_checkpoint = {0};
	assert(server_local_gather_chunk_entities(
		&s, owner_cx, owner_cz, &creeper_checkpoint));
	assert(creeper_checkpoint.count == 1);
	assert(creeper_checkpoint.entities[0].kind == ARCHIVED_ENTITY_MONSTER);
	assert(creeper_checkpoint.entities[0].data.monster.monster_id
		   == MONSTER_CREEPER);
	assert(creeper_checkpoint.entities[0].data.monster.fuse
		   == CREEPER_FUSE_TICKS);
	assert(creeper_checkpoint.entities[0].health == 1);
	void* creeper_checkpoint_arena = NULL;
	assert(chunk_entities_build(
		&creeper_checkpoint, &creeper_checkpoint_arena));
	chunk_entities_build_free(creeper_checkpoint_arena);

	clear_entities(&s);
	assert(server_local_restore_chunk_entities(&s, &creeper_checkpoint));
	chunk_entities_free(&creeper_checkpoint);
	assert(dict_entity_size(s.entities) == 1);
	dict_entity_it_t restored_it;
	dict_entity_it(restored_it, s.entities);
	assert(!dict_entity_end_p(restored_it));
	durable_creeper = dict_entity_ref(restored_it)->value;
	assert(durable_creeper && durable_creeper->type == ENTITY_MONSTER);
	assert(durable_creeper->data.monster.id == MONSTER_CREEPER);
	assert(durable_creeper->data.monster.explosion_pending);
	assert(durable_creeper->data.monster.fuse == CREEPER_FUSE_TICKS);
	assert(durable_creeper->health == 1);
	assert(memcmp(durable_creeper->pos, owned_center, sizeof(owned_center)) == 0);
	uint32_t durable_creeper_id = durable_creeper->id;

	/* Pin the explicit attack-path guard while the update's cap is still full. */
	struct server_rpc stale_attack = {
		.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = durable_creeper_id,
	};
	server_local_test_process_rpc(&s, &stale_attack);
	assert(durable_creeper->health == 1);
	assert(durable_creeper->data.monster.explosion_pending);
	struct entity* second_pending = server_local_spawn_primed_tnt(
		&s, (vec3) {40.5f, 100.5f, 40.5f}, 0);
	assert(second_pending);
	assert(!second_pending->tick_server(second_pending, &s));
	assert(second_pending->data.primed_tnt.explosion_pending);
	uint32_t second_pending_id = second_pending->id;
	vec3 second_pending_center;
	memcpy(second_pending_center, second_pending->pos,
		   sizeof(second_pending_center));

	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = owned_center[0];
	s.player.y = owned_center[1];
	s.player.z = owned_center[2];
	const w_coord_t respawn_cx = 10, respawn_cz = 10;
	/* This fixture exercises stream/epoch ordering, not populate slicing. Mark the
	 * four quadrant origins covering the center complete so ensure_populated can
	 * send it in the same deterministic update after the deferred unload flush. */
	for(int dz = -1; dz <= 0; dz++)
		for(int dx = -1; dx <= 0; dx++) {
			struct server_chunk* covering = NULL;
			assert(server_world_load_chunk(
				&s.worlds[0], respawn_cx + dx, respawn_cz + dz, &covering)
				&& covering);
			covering->populated = true;
		}
	struct server_chunk* respawn_sc = dict_server_chunks_get(
		s.worlds[0].chunks, S_CHUNK_ID(respawn_cx, respawn_cz));
	assert(respawn_sc);
	respawn_sc->sent = false;
	struct server_chunk* death_sc = NULL;
	death_sc = dict_server_chunks_get(s.worlds[0].chunks, S_CHUNK_ID(0, 0));
	if(!death_sc)
		assert(server_world_load_chunk(&s.worlds[0], 0, 0, &death_sc));
	assert(death_sc);
	death_sc->sent = true;
	/* Loading another inline dictionary value may rehash and move every chunk.
	 * Never retain respawn_sc across that insertion. */
	respawn_sc = dict_server_chunks_get(
		s.worlds[0].chunks, S_CHUNK_ID(respawn_cx, respawn_cz));
	assert(respawn_sc);
	respawn_sc->sent = false;
	struct worldgen* saved_generator = s.worlds[0].generator;
	s.player.spawn_x = respawn_cx * CHUNK_SIZE + 2;
	s.player.spawn_y = 100;
	s.player.spawn_z = respawn_cz * CHUNK_SIZE + 2;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
	/* An already-paused world must not consume a pending simulation event. */
	s.paused = true;
	server_local_test_update_once(&s);
	assert(dict_entity_get(s.entities, durable_creeper_id) != NULL);
	assert(dict_entity_get(s.entities, second_pending_id) != NULL);
	assert(s.worlds[0].explosions_this_tick == 0);
	s.paused = false;

	/* Every reliable gameplay action below was authored by the body standing at
	 * owned_center. The pending blast runs before this inbox is drained, so a
	 * lethal result must invalidate all body-owned actions, not just PLAYER_POS.
	 * Keep the three targets far below the blast so only RPC dispatch can mutate
	 * them. */
	const w_coord_t stale_dig_x = 1, stale_dig_y = 70, stale_dig_z = 1;
	const w_coord_t stale_lever_x = 3, stale_lever_y = 71, stale_lever_z = 3;
	assert(server_world_set_block(
		&s.worlds[0], stale_dig_x, stale_dig_y, stale_dig_z,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(
		&s.worlds[0], stale_lever_x, stale_lever_y - 1, stale_lever_z,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(
		&s.worlds[0], stale_lever_x, stale_lever_y, stale_lever_z,
		(struct block_data) {.type = BLOCK_LEVER, .metadata = 5}));
	struct entity* stale_attack_target = server_local_spawn_monster(
		(vec3) {14.5f, 70.5f, 14.5f}, MONSTER_ZOMBIE, &s);
	assert(stale_attack_target);
	short stale_attack_health = stale_attack_target->health;
	uint32_t stale_attack_target_id = stale_attack_target->id;

	struct server_rpc stale_pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = 31.5,
			.y = 110.0,
			.z = 31.5,
			.rx = 0.0f,
			.ry = 0.0f,
			.vel_y = 0.0f,
		},
	};
	struct server_rpc stale_body_dig = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {
			.x = stale_dig_x,
			.y = stale_dig_y,
			.z = stale_dig_z,
			.side = SIDE_TOP,
			.finished = true,
		},
	};
	struct server_rpc stale_body_place = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {
			.x = stale_lever_x,
			.y = stale_lever_y,
			.z = stale_lever_z,
			.side = SIDE_TOP,
		},
	};
	struct server_rpc stale_body_attack = {
		.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = stale_attack_target_id,
	};
	/* Hotbar and window clicks mutate the client optimistically before transport.
	 * They need both a server-side rejection and an authoritative client reset at
	 * the epoch boundary, not merely a silent stale-action drop. */
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&client_reset);
	assert(gstate.players[0].local_player && gstate.players[0].windows[WINDOWC_INVENTORY]);
	struct window_container* client_inventory
		= gstate.players[0].windows[WINDOWC_INVENTORY];
	windowc_discard_pending_actions(client_inventory);
	assert(windowc_revision_count(client_inventory) == 1);
	size_t server_hotbar_before = inventory_get_hotbar(&s.player.inventory);
	size_t stale_client_hotbar
		= (server_hotbar_before + 1) % INVENTORY_SIZE_HOTBAR;
	inventory_set_hotbar(
		windowc_get_latest(client_inventory), stale_client_hotbar);
	struct server_rpc stale_body_hotbar = {
		.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot.slot = stale_client_hotbar,
	};
	uint16_t stale_window_action_id;
	assert(windowc_new_action(
		client_inventory, &stale_window_action_id, false,
		INVENTORY_SLOT_HOTBAR + server_hotbar_before));
	assert(windowc_revision_count(client_inventory) == 2);
	struct server_rpc stale_body_window_click = {
		.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {
			.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_HOTBAR + server_hotbar_before,
			.action_id = stale_window_action_id,
		},
	};
	/* Every old-body action must drain without executing. The final authoritative
	 * PLAYER_POS carries an input epoch; its client ACK is the FIFO boundary after
	 * already-authored dead-body input and before fresh gameplay input. */
	struct server_rpc stale_body_pause = {.type = SRPC_TOGGLE_PAUSE};
	assert(svin_rpc_send(&stale_body_pause));
	/* residency-1005: a PLAYER_POS sample is admitted only when its SWEPT
	 * footprint -- the body box from the kept pose (8.5, 8.5) to the sample
	 * (31.5, 31.5), i.e. Beta's handleFlying moveEntity read region
	 * boundingBox.addCoord(dx, dy, dz) (CFR NetServerHandler.java:242,
	 * Entity.java:302) -- is resident (server_local_actor_admit_pose). This
	 * case pins corpse physics, not residency, so every column that box
	 * crosses, (0..1, 0..1), must be resident like the rest of the corpse's
	 * world; the diagonal crosses the (16,16) corner of all four. */
	for(int pcz = 0; pcz <= 1; pcz++)
		for(int pcx = 0; pcx <= 1; pcx++)
			if(!server_world_is_chunk_loaded(&s.worlds[0], pcx, pcz)) {
				struct server_chunk* pos_sc = NULL;
				assert(server_world_load_chunk(&s.worlds[0], pcx, pcz, &pos_sc)
					   && pos_sc);
			}
	assert(svin_rpc_send(&stale_pos));
	assert(svin_rpc_send(&stale_body_dig));
	assert(svin_rpc_send(&stale_body_place));
	assert(svin_rpc_send(&stale_body_attack));
	assert(svin_rpc_send(&stale_body_hotbar));
	assert(svin_rpc_send(&stale_body_window_click));
	g_prepass_order_count = 0;
	g_captured_input_handoff = false;
	g_prepass_window_transactions = 0;
	memset(&g_input_handoff, 0, sizeof(g_input_handoff));
	memset(g_prepass_explosion_pos, 0, sizeof(g_prepass_explosion_pos));
	g_capture_prepass_order = true;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	/* Make the far spawn center deterministically unsendable until the generator is
	 * restored. This isolates the collision-readiness gate without generating the
	 * full surrounding superflat ring. */
	s.worlds[0].generator = NULL;
	/* REGRESS-1015: an empty client inbox first (the drain thread stays the
	 * only consumer), so how many old-world columns this update streams --
	 * and therefore which of them the respawn window must UNLOAD -- does not
	 * depend on whether the drain thread has returned the earlier windows'
	 * chunk slots yet (the G9X-6 refresh uses that same pool). */
	/* Pin the near-send budget (as the respawn window does) so the number of
	 * old-world columns streamed -- hence the UNLOADs owed later -- is not a
	 * wall-clock input. */
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	test_drain_quiesce();
	const bool cap_owner_debt_before_ok = no_sent_debt_column(&s)
		|| only_sent_debt_column(&s, 0, 0);
	server_local_test_update_once(&s);
	server_local_test_set_near_send_minimum(0);
	server_local_test_expire_near_budget(false);
	/* The G9X-6 refresh of (0,0) may be deferred by chunk-slot pressure;
	 * settle it with capture still ON so it is counted below. */
	settle_publication_refresh(&s, 0, 0);
	g_capture_prepass_order = false;
	clin_test_set_rpc_observer(NULL, NULL);

	/* Deviation-#18: the old atomic death doubled as a mid-prepass respawn
	 * barrier that froze the REST of the explosion pass AND the near
	 * streamer. The dead dwell keeps the world running
	 * (GuiGameOver.doesGuiPauseGame()==false): the cap-retained second blast
	 * is admitted in this same update right after the death commit, and the
	 * streamer may keep serving CHUNK sends to the corpse's client -- pin
	 * the event PREFIX exactly and require every later capture to be
	 * streaming, nothing else. (Blast #2's chain scan may also log
	 * fail-closed "chain reservation failed" lines: the pre-existing
	 * reservation budget is already held by the retained blasts; the chain
	 * blocks are retained loudly, a budget behaviour that predates the
	 * split.) */
	assert(g_prepass_order_count >= 3);
	assert(g_prepass_order[0] == CRPC_ENTITY_EXPLODE);
	assert(g_prepass_order[1] == CRPC_PLAYER_SET_HEALTH);
	assert(g_prepass_order[2] == CRPC_ENTITY_EXPLODE);
	for(size_t tail = 3; tail < g_prepass_order_count; tail++)
		assert(g_prepass_order[tail] == CRPC_CHUNK);
	/* G9X-6 (REGRESS-1015): the tail ends with the post-drain publication
	 * refresh of column (0,0) -- the light stores of this update's
	 * explosions in it (Beta publishes every stored light change, CFR
	 * World.java:512-517) -- sent exactly once, after the near sends, and
	 * no sent column owes a refresh afterwards. Before it, no column but
	 * (0,0) owed one. */
	{
		assert(cap_owner_debt_before_ok);
		size_t origin = 0;
		for(size_t tail = 3; tail < g_prepass_order_count; tail++)
			origin += g_prepass_chunk_x[tail] == 0 && g_prepass_chunk_z[tail] == 0;
		/* Exactly one (0,0) republish across update + settle (a deferred
		 * refresh keeps its debt, server_local.c "FIFO full / older output
		 * pending", and lands in the settle pass); it is the last event
		 * either way. */
		assert(origin == 1);
		assert(g_prepass_order_count > 3
			   && g_prepass_chunk_x[g_prepass_order_count - 1] == 0
			   && g_prepass_chunk_z[g_prepass_order_count - 1] == 0);
		assert(column_debt_cleared(&s, 0, 0) && no_sent_debt_column(&s));
	}
	assert(!g_captured_input_handoff);
	assert(s.input_ack_pending == 0);
	/* Deviation-#18 split: the pre-RPC explosion kill commits only the DEAD
	 * state -- no world switch, no loading epoch, no teleport. The stale
	 * dead-body FIFO then drains against the CORPSE: crosshair actions
	 * (dig/place/attack/hotbar) are inert via server_rpc_blocked_while_dead
	 * (Beta swallows them the moment GuiGameOver substitutes in --
	 * runTick:982-985 pushes leftClickCounter 10000 out), the TOGGLE is
	 * dead-rejected, the POS sample is ACCEPTED (real corpse physics), and
	 * the window-0 click EXECUTES -- Beta's trigger requires
	 * currentScreen==null, so an inventory screen open at death stays open
	 * and its clicks keep working over the dead player. */
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
	assert(s.player.finished_loading);
	assert(g_prepass_window_transactions == 1);
	/* The observer keeps the LAST explosion's centre: blast #2's. The
	 * retained TNT kept its ordinary physics through the dwell (it falls a
	 * fraction of a block before admission -- Beta detonates at the entity's
	 * LIVE position), so this is an identity check, not a byte compare. */
	assert(fabsf(g_prepass_explosion_pos[0] - second_pending_center[0]) < 1.0f
		   && fabsf(g_prepass_explosion_pos[1] - second_pending_center[1])
			   < 1.0f
		   && fabsf(g_prepass_explosion_pos[2] - second_pending_center[2])
			   < 1.0f);
	assert(dict_entity_get(s.entities, durable_creeper_id) == NULL);
	/* Its blast admitted in this update, the retained source is consumed
	 * and reaped. */
	assert(dict_entity_get(s.entities, second_pending_id) == NULL);
	assert(s.player.health <= 0);
	assert(fabs(s.player.x - 31.5) < 1.0e-6);
	assert(fabs(s.player.y - 110.0) < 1.0e-6);
	assert(fabs(s.player.z - 31.5) < 1.0e-6);
	assert(!s.reject_player_pos_this_update);
	assert(!s.paused);
	struct block_data stale_dig_after, stale_lever_after;
	assert(server_world_get_block(
		&s.worlds[0], stale_dig_x, stale_dig_y, stale_dig_z,
		&stale_dig_after));
	assert(server_world_get_block(
		&s.worlds[0], stale_lever_x, stale_lever_y, stale_lever_z,
		&stale_lever_after));
	struct entity** stale_attack_after
		= dict_entity_get(s.entities, stale_attack_target_id);
	unsigned stale_body_action_mask = 0;
	if(stale_dig_after.type != BLOCK_STONE)
		stale_body_action_mask |= 1u;
	if(stale_lever_after.type != BLOCK_LEVER
	   || stale_lever_after.metadata != 5)
		stale_body_action_mask |= 2u;
	if(!stale_attack_after || !*stale_attack_after
	   || (*stale_attack_after)->health != stale_attack_health)
		stale_body_action_mask |= 4u;
	if(inventory_get_hotbar(&s.player.inventory) != server_hotbar_before)
		stale_body_action_mask |= 8u;
	unsigned death_barrier_failure_mask = stale_body_action_mask;

	/* The explicit Respawn press: the transaction (drops at the LATCHED death
	 * position, the teleport to spawn, the DEATH switch + loading epoch, the
	 * position-rejection latch) runs only now. The stale window-click's
	 * cursor pickup drops back out here (B9). */
	struct server_rpc respawn_press = {.type = SRPC_RESPAWN};
	assert(svin_rpc_send(&respawn_press));
	server_local_test_update_once(&s);
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(fabs(s.player.x - (s.player.spawn_x + 0.5)) < 1.0e-6);
	assert(s.player.y
		   == (double)(s.player.spawn_y + PLAYER_EYE_HEIGHT));
	assert(fabs(s.player.z - (s.player.spawn_z + 0.5)) < 1.0e-6);
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(!s.player.finished_loading);
	assert(s.reject_player_pos_this_update);
	assert(!s.paused);

	/* No epoch may be advertised until the spawn center is collision-ready. The
	 * phase barrier survives tick rollover and rejects an action authored after
	 * death while the far spawn remains unsent. */
	struct server_rpc late_old_pause = {.type = SRPC_TOGGLE_PAUSE};
	assert(svin_rpc_send(&late_old_pause));
	server_local_test_update_once(&s);
	assert(!s.paused);
	assert(s.input_ack_pending == 0);
	assert(!s.player.finished_loading);
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(!g_captured_input_handoff);

	/* An older deferred unload for the respawn center must enter the client FIFO
	 * before its replacement full chunk. Saturate the pipe and prove neither a
	 * new chunk nor the marker can overtake it. */
	/* Append after any UNLOAD already waiting (do not discard one). */
	const int waiting_unloads_after_inject = s.pending_near_unload_count + 1;
	{
		const int slot = s.pending_near_unload_count;
		assert(slot < (int)(sizeof(s.pending_near_unloads)
					   / sizeof(s.pending_near_unloads[0])));
		s.pending_near_unloads[slot] = (struct pending_chunk_unload) {
			.x = respawn_cx,
			.z = respawn_cz,
		};
		s.pending_near_unload_count = slot + 1;
	}
	/* Freeze the test consumer before filling the pipe. Otherwise ASan's slower
	 * server update gives the background drain thread time to recycle a token
	 * between the failed try-send and the assertion below, making this transport
	 * saturation proof scheduler-dependent. */
	g_drain_run = 0;
	thread_join(&drain);
	while(clin_try_rpc_send(&(struct client_rpc) {.type = CRPC_TIME_SET})) {
	}
	g_prepass_order_count = 0;
	g_capture_prepass_order = true;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	server_local_test_update_once(&s);
	g_capture_prepass_order = false;
	clin_test_set_rpc_observer(NULL, NULL);
	assert(g_prepass_order_count == 0);
	assert(s.pending_near_unload_count == waiting_unloads_after_inject);
	assert(!respawn_sc->sent && !s.player.finished_loading);
	assert(s.input_ack_pending == 0);
	clin_drain_discard();
	g_drain_run = 1;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	/* Once capacity returns, UNLOAD must precede the center CHUNK, then the
	 * same-world marker may release without filling the entire second near ring. */
	s.worlds[0].generator = saved_generator;
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	/* G9X-6: no sent column owes the client a refresh (see the cap-owner
	 * window above) */
	const bool respawn_no_debt_before = no_sent_debt_column(&s);
	/* Expected UNLOAD set for this window = every waiting unload (incl. the
	 * injected (10,10)) plus the sent old-world columns outside the new view
	 * radius (computed after the window, from geometry); deduped. */
	int expect_ux[128], expect_uz[128];
	size_t expect_n = 0;
	int sent_ux[64], sent_uz[64];
	size_t sent_n = 0;
	for(int i = 0; i < s.pending_near_unload_count; i++) {
		expect_ux[expect_n] = (int)s.pending_near_unloads[i].x;
		expect_uz[expect_n++] = (int)s.pending_near_unloads[i].z;
	}
	{
		dict_server_chunks_it_t it;
		for(dict_server_chunks_it(it, s.worlds[0].chunks);
			!dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
			if(!dict_server_chunks_cref(it)->value.sent)
				continue;
			assert(sent_n < 64);
			sent_ux[sent_n] = (int)S_CHUNK_X(dict_server_chunks_cref(it)->key);
			sent_uz[sent_n++] = (int)S_CHUNK_Z(dict_server_chunks_cref(it)->key);
		}
	}
	g_prepass_order_count = 0;
	g_capture_prepass_order = true;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	for(int stream_ticks = 0;
		stream_ticks < 128 && s.input_ack_pending == 0; stream_ticks++) {
		server_local_test_update_once(&s);
		/* Population can legitimately defer across generation-budget windows.
		 * Recycle client transport slots while preserving the send observer's
		 * complete FIFO trace. REGRESS-1015: through the drain thread
		 * (test_drain_quiesce), never a second concurrent consumer -- the
		 * inbox's packed-message replay state is unsynchronised. */
		test_drain_quiesce();
	}
	server_local_test_set_near_send_minimum(0);
	server_local_test_expire_near_budget(false);
	g_capture_prepass_order = false;
	clin_test_set_rpc_observer(NULL, NULL);
	assert(!server_world_is_chunk_sent(
		&s.worlds[0], respawn_cx + MAX_VIEW_DISTANCE,
		respawn_cz + MAX_VIEW_DISTANCE));
	/* F2a d86e8d673 also streams the old world before the queued Respawn
	 * press. The newly sent old column (0,-1) must retire after the already
	 * deferred center unload (10,10), before any replacement CHUNK/marker. */
	fprintf(stderr, "respawn-center world/network order (%zu):", g_prepass_order_count);
	for(size_t i = 0; i < g_prepass_order_count; i++)
		fprintf(stderr, " %d", g_prepass_order[i]);
	fputc('\n', stderr);
	/* G9X-6 (REGRESS-1015): the cap-owner window above already republished
	 * column (0,0), so no sent column owes a refresh here and the old pin
	 * holds unchanged: 2 UNLOADs, 4 respawn-ring CHUNKs, TIME_SET,
	 * PLAYER_POS. */
	assert(respawn_no_debt_before);
	/* The departed old-world columns may all UNLOAD ahead of the injected
	 * (10,10) (1 to 3 prefix UNLOADs depending on how many columns the
	 * cap-owner window had sent); what is pinned is: an UNLOAD prefix, then
	 * 4 respawn-ring CHUNKs, TIME_SET, PLAYER_POS. */
	size_t unload_prefix = 0;
	while(unload_prefix < g_prepass_order_count
		  && g_prepass_order[unload_prefix] == CRPC_UNLOAD_CHUNK)
		unload_prefix++;
	/* Retirement set from GEOMETRY, independent of any sent/eviction flag
	 * (a deferred eviction leaves the flag set; a lost unload must still
	 * fail): every column sent before the window that lies outside the view
	 * radius around the respawn centre (the near evictor's own criterion,
	 * server_world_furthest_chunk), added to the waiting unloads. */
	{
		const int radius = server_local_view_radius(&s);
		for(size_t i = 0; i < sent_n; i++) {
			if(abs(sent_ux[i] - (int)respawn_cx) <= radius
			   && abs(sent_uz[i] - (int)respawn_cz) <= radius)
				continue;
			bool dup = false;
			for(size_t e = 0; e < expect_n; e++)
				dup |= expect_ux[e] == sent_ux[i] && expect_uz[e] == sent_uz[i];
			if(!dup) {
				expect_ux[expect_n] = sent_ux[i];
				expect_uz[expect_n++] = sent_uz[i];
			}
		}
	}
	/* Every expected unload exactly once (incl. (10,10)), all before the
	 * replacement chunks, nothing unexpected. */
	assert(unload_prefix == expect_n);
	for(size_t e = 0; e < expect_n; e++) {
		unsigned seen = 0;
		for(size_t i = 0; i < unload_prefix; i++)
			seen += g_prepass_chunk_x[i] == expect_ux[e]
				 && g_prepass_chunk_z[i] == expect_uz[e];
		assert(seen == 1);
	}
	{
		unsigned center = 0;
		for(size_t i = 0; i < unload_prefix; i++)
			center += g_prepass_chunk_x[i] == respawn_cx
				   && g_prepass_chunk_z[i] == respawn_cz;
		assert(center == 1);
	}
	assert(g_prepass_order_count == unload_prefix + 6);
	assert(g_prepass_order[g_prepass_order_count - 2] == CRPC_TIME_SET);
	assert(g_prepass_order[g_prepass_order_count - 1] == CRPC_PLAYER_POS);
	for(size_t i = unload_prefix; i + 2 < g_prepass_order_count; i++)
		assert(g_prepass_order[i] == CRPC_CHUNK);
	assert(s.pending_near_unload_count == 0);
	assert(g_captured_input_handoff);
	assert(g_input_handoff.payload.player_pos.input_epoch != 0);
	assert(g_input_handoff.payload.player_pos.fresh_body);
	assert(g_input_handoff.payload.player_pos.selected_hotbar
		   == server_hotbar_before);
	assert(g_input_handoff.payload.player_pos.velocity[0] == 0.0f
		   && g_input_handoff.payload.player_pos.velocity[1] == 0.0f
		   && g_input_handoff.payload.player_pos.velocity[2] == 0.0f);
	assert(g_input_handoff.payload.player_pos.oxygen == MAX_OXYGEN);
	assert(!g_input_handoff.payload.player_pos.on_fire);
	assert(g_input_handoff.payload.player_pos.world_time == s.world_time);
	uint32_t input_epoch = g_input_handoff.payload.player_pos.input_epoch;
	assert(s.input_ack_pending == input_epoch);
	assert(s.player.finished_loading);
	/* A client-side section allocation failure consumes this one-shot marker but
	 * must not strand the server behind it. A wrong token is inert; the exact
	 * retry retains the death/input barrier, invalidates every client-stream
	 * claim, and starts a bounded center-first restream. Discarding the captured
	 * FIFO here models the client having consumed it and releases its chunk slots. */
	test_drain_quiesce(); /* REGRESS-1015: drain thread is the only consumer */
	struct server_rpc wrong_retry = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready = {
			.epoch = input_epoch + 1,
			.retry = true,
		},
	};
	if(wrong_retry.payload.input_ready.epoch == 0)
		wrong_retry.payload.input_ready.epoch = 1;
	server_local_test_process_rpc(&s, &wrong_retry);
	assert(s.input_ack_pending == input_epoch);
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(s.player.finished_loading);

	/* Exhaustion is explicit and terminal, not an indistinguishable spinner. The
	 * server publishes exactly one client error, retains the exact epoch/phase,
	 * and ignores both duplicate NACKs and a later ACK for the uninstalled body. */
	struct server_local capped = {0};
	capped.input_ack_pending = 0xC0DEu;
	capped.input_handoff_retry_count = INPUT_HANDOFF_RETRY_MAX;
	capped.world_switch_phase = SERVER_WORLD_SWITCH_DEATH;
	struct server_rpc capped_retry = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready = {
			.epoch = capped.input_ack_pending,
			.retry = true,
		},
	};
	g_terminal_load_failures = 0;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	server_local_test_process_rpc(&capped, &capped_retry);
	server_local_test_process_rpc(&capped, &capped_retry);
	struct server_rpc capped_ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = capped.input_ack_pending,
	};
	server_local_test_process_rpc(&capped, &capped_ack);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(g_terminal_load_failures == 1);
	assert(capped.input_handoff_retry_count == INPUT_HANDOFF_RETRY_MAX + 1);
	assert(capped.input_ack_pending == 0xC0DEu);
	assert(capped.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	test_drain_quiesce(); /* REGRESS-1015: drain thread is the only consumer */
	/* A server-side center generation/MEM failure never reaches an epoch marker,
	 * so it cannot rely on client NACK exhaustion. Its own watchdog publishes the
	 * same terminal UI exactly once and retains every load/simulation barrier. */
	struct server_local center_timeout = {0};
	center_timeout.player.has_pos = true;
	center_timeout.player.finished_loading = false;
	center_timeout.world_switch_phase = SERVER_WORLD_SWITCH_DEATH;
	g_terminal_load_failures = 0;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	server_local_test_force_center_load_timeout(&center_timeout);
	server_local_test_force_center_load_timeout(&center_timeout);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(center_timeout.load_failure_terminal);
	assert(g_terminal_load_failures == 1);
	assert(!center_timeout.player.finished_loading);
	assert(center_timeout.input_ack_pending == 0);
	assert(center_timeout.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	test_drain_quiesce(); /* REGRESS-1015: drain thread is the only consumer */

	struct server_rpc exact_retry = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready = {
			.epoch = input_epoch,
			.retry = true,
		},
	};
	server_local_test_process_rpc(&s, &exact_retry);
	assert(s.input_ack_pending == 0);
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(!s.player.finished_loading);
	assert(s.input_handoff_retry_count == 1);
	assert(s.pending_near_unload_count == 0);
	assert(dict_far_sent_size(s.worlds[0].far_sent) == 0);
	size_t invalidated_claims = 0;
	dict_server_chunks_it_t retry_it;
	dict_server_chunks_it(retry_it, s.worlds[0].chunks);
	while(!dict_server_chunks_end_p(retry_it)) {
		assert(!dict_server_chunks_ref(retry_it)->value.sent);
		invalidated_claims++;
		dict_server_chunks_next(retry_it);
	}
	assert(invalidated_claims > 0);

	/* Same-world death retries remain center-first: the client has freed its old
	 * voxel ring, while the server must not build a second 11x11 MEM2 ring beside
	 * the retained first one. The center is actually resent before a fresh epoch;
	 * an outer near coordinate is deliberately still unsent at that boundary. */
	g_prepass_order_count = 0;
	g_captured_input_handoff = false;
	g_capture_prepass_order = true;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	server_local_test_update_once(&s);
	g_capture_prepass_order = false;
	clin_test_set_rpc_observer(NULL, NULL);
	assert(g_prepass_order_count >= 3);
	assert(g_prepass_order[0] == CRPC_CHUNK);
	assert(g_prepass_order[g_prepass_order_count - 2] == CRPC_TIME_SET);
	assert(g_prepass_order[g_prepass_order_count - 1] == CRPC_PLAYER_POS);
	for(size_t i = 0; i + 2 < g_prepass_order_count; i++)
		assert(g_prepass_order[i] == CRPC_CHUNK);
	assert(g_captured_input_handoff);
	assert(!server_world_is_chunk_sent(
		&s.worlds[0], respawn_cx + MAX_VIEW_DISTANCE,
		respawn_cz + MAX_VIEW_DISTANCE));
	uint32_t retry_epoch = g_input_handoff.payload.player_pos.input_epoch;
	assert(retry_epoch != 0 && retry_epoch != input_epoch);
	assert(s.input_ack_pending == retry_epoch);
	assert(s.player.finished_loading);
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(g_input_handoff.payload.player_pos.fresh_body);
	struct server_rpc stale_old_ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = input_epoch,
	};
	server_local_test_process_rpc(&s, &stale_old_ack);
	assert(s.input_ack_pending == retry_epoch);
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	input_epoch = retry_epoch;

	struct server_rpc wrong_ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = input_epoch + 1,
	};
	if(wrong_ack.payload.input_ready.epoch == 0)
		wrong_ack.payload.input_ready.epoch = 1;
	assert(svin_rpc_send(&wrong_ack));
	assert(svin_rpc_send(&late_old_pause));
	server_local_test_update_once(&s);
	assert(!s.paused);
	assert(s.input_ack_pending == input_epoch);
	/* Reproduce the exact stale client body the hardware can carry into the
	 * marker frame. The marker must atomically replace camera/body/base hotbar,
	 * clear mount/input ownership, and collapse speculative revisions. */
	assert(gstate.players[0].local_player);
	gstate.players[0].camera.x = 91.5f;
	gstate.players[0].camera.y = 17.0f;
	gstate.players[0].camera.z = -43.5f;
	gstate.players[0].local_player->vel[0] = 0.75f;
	gstate.players[0].local_player->vel[1] = -2.5f;
	gstate.players[0].local_player->vel[2] = -0.5f;
	gstate.players[0].local_player->data.local_player.jump_ticks = 7;
	gstate.players[0].local_player->on_fire = true;
	gstate.players[0].digging.active = true;
	gstate.autopilot.pending_place = 1;
	gstate.players[0].oxygen = 0;
	gstate.paused = true;
	/* Death does not WORLD_RESET the Overworld entity dictionary. Seed two carts
	 * which still claim the dead local player plus one foreign rider; the fresh
	 * body handoff must release every local claim without touching the foreign one. */
	const uint32_t local_cart_ids[] = {200, 201};
	for(size_t i = 0; i < sizeof(local_cart_ids) / sizeof(local_cart_ids[0]); i++) {
		struct client_rpc spawn_cart = {
			.type = CRPC_SPAWN_MINECART,
			.payload.spawn_minecart = {
				.entity_id = local_cart_ids[i],
				.pos = {30.0f + (float)i, 70.0f, 30.0f},
			},
		};
		clin_process(&spawn_cart);
		struct entity** cart_slot
			= dict_entity_get(gstate.entities, local_cart_ids[i]);
		assert(cart_slot && *cart_slot);
		(*cart_slot)->data.minecart.occupied = true;
		(*cart_slot)->data.minecart.occupant_id = gstate.players[0].local_player->id;
		(*cart_slot)->rightClickText = "Dismount";
	}
	const uint32_t foreign_cart_id = 202;
	struct client_rpc spawn_foreign_cart = {
		.type = CRPC_SPAWN_MINECART,
		.payload.spawn_minecart = {
			.entity_id = foreign_cart_id,
			.pos = {32.0f, 70.0f, 30.0f},
		},
	};
	clin_process(&spawn_foreign_cart);
	struct entity** foreign_cart_slot
		= dict_entity_get(gstate.entities, foreign_cart_id);
	assert(foreign_cart_slot && *foreign_cart_slot);
	(*foreign_cart_slot)->data.minecart.occupied = true;
	(*foreign_cart_slot)->data.minecart.occupant_id = 99;
	(*foreign_cart_slot)->rightClickText = "Dismount";
	/* Model the split-frame failure directly: an optimistic scroll has mutated the
	 * sole/base snapshot before the epoch marker arrives. */
	windowc_discard_pending_actions(client_inventory);
	assert(windowc_revision_count(client_inventory) == 1);
	inventory_set_hotbar(
		windowc_get_latest(client_inventory), stale_client_hotbar);
	uint16_t post_snapshot_action_id;
	assert(windowc_new_action(
		client_inventory, &post_snapshot_action_id, false,
		INVENTORY_SLOT_HOTBAR + server_hotbar_before));
	assert(windowc_revision_count(client_inventory) == 2);
	/* AIR into an absent client section is deliberately a no-op. Materialize the
	 * spawn section without changing the eye voxel used by the handoff. */
	world_set_block(
		&gstate.world, (w_coord_t)floor(s.player.x),
		(w_coord_t)floor(s.player.y) - 1, (w_coord_t)floor(s.player.z),
		(struct block_data) {.type = BLOCK_STONE}, false);
	clin_process(&g_input_handoff);
	assert(gstate.world_switch_pending);
	/* Collision installation alone does not expose the respawn. Advance the
	 * existing presentation fallback before expecting the input ACK. */
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	assert(!gstate.world_switch_pending);
	assert(!svin_input_ready_pending(input_epoch));
	assert(!gstate.paused);
	assert(fabs(gstate.players[0].camera.x - s.player.x) < 1.0e-6);
	assert(fabs(gstate.players[0].camera.y - s.player.y) < 1.0e-6);
	assert(fabs(gstate.players[0].camera.z - s.player.z) < 1.0e-6);
	assert(gstate.players[0].local_player->vel[0] == 0.0f
		   && gstate.players[0].local_player->vel[1] == 0.0f
		   && gstate.players[0].local_player->vel[2] == 0.0f);
	assert(gstate.players[0].local_player->data.local_player.jump_ticks == 0);
	assert(!gstate.players[0].local_player->on_fire && !gstate.players[0].digging.active);
	assert(gstate.autopilot.pending_place == 0);
	assert(gstate.players[0].oxygen == MAX_OXYGEN);
	assert(windowc_revision_count(client_inventory) == 1);
	assert(inventory_get_hotbar(windowc_get_latest(client_inventory))
		   == server_hotbar_before);
	for(size_t i = 0; i < sizeof(local_cart_ids) / sizeof(local_cart_ids[0]); i++) {
		struct entity** cart_slot
			= dict_entity_get(gstate.entities, local_cart_ids[i]);
		assert(cart_slot && *cart_slot);
		assert(!(*cart_slot)->data.minecart.occupied);
		assert((*cart_slot)->data.minecart.occupant_id == 0);
		assert(strcmp((*cart_slot)->rightClickText, "Ride") == 0);
	}
	assert((*foreign_cart_slot)->data.minecart.occupied);
	assert((*foreign_cart_slot)->data.minecart.occupant_id == 99);
	assert(strcmp((*foreign_cart_slot)->rightClickText, "Dismount") == 0);
	assert(fabsf(gstate.players[0].local_player->pos[0] - (float)s.player.x) < 1.0e-6f
		   && fabsf(gstate.players[0].local_player->pos[1] - (float)s.player.y) < 1.0e-6f
		   && fabsf(gstate.players[0].local_player->pos[2] - (float)s.player.z) < 1.0e-6f);
	/* Same-frame clin_update must not echo the corpse camera/velocity after ACK. */
	clin_update();
	g_prepass_order_count = 0;
	g_capture_prepass_order = true;
	clin_test_set_rpc_observer(capture_tnt_rpcs, NULL);
	struct server_rpc fresh_body_pause = {.type = SRPC_TOGGLE_PAUSE};
	assert(svin_rpc_send(&fresh_body_pause));
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	server_local_test_update_once(&s);
	server_local_test_set_near_send_minimum(0);
	server_local_test_expire_near_budget(false);
	g_capture_prepass_order = false;
	clin_test_set_rpc_observer(NULL, NULL);
	assert(s.input_ack_pending == 0);
	assert(s.input_handoff_retry_count == 0);
	assert(s.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
	assert(s.paused);
	assert(fabs(s.player.x - (s.player.spawn_x + 0.5)) < 1.0e-6);
	assert(s.player.y == (double)(s.player.spawn_y + PLAYER_EYE_HEIGHT));
	assert(fabs(s.player.z - (s.player.spawn_z + 0.5)) < 1.0e-6);
	fprintf(stderr, "fresh-body world/network order (%zu):", g_prepass_order_count);
	for(size_t i = 0; i < g_prepass_order_count; i++)
		fprintf(stderr, " %d", g_prepass_order[i]);
	fprintf(stderr, "\n");
	/* F2a: the fresh body's world pass streams before the queued pause. */
	assert(g_prepass_order_count == 5);
	for(size_t i = 0; i < 4; i++)
		assert(g_prepass_order[i] == CRPC_CHUNK);
	assert(g_prepass_order[4] == CRPC_TIME_SET);
	s.paused = false;

	/* Deviation-#18 split: a mid-pass kill leaves only a CORPSE -- the fresh
	 * body can never appear inside an entity pass (the respawn starts from
	 * the RPC prepass alone). Entity A kills the old Overworld body; entity B
	 * comes later in the same worklist and now ticks against the corpse,
	 * whose dead-guard absorbs the hit with no state churn. */
	clear_entities(&s);
	inventory_clear(&s.player.inventory);
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 12.5;
	s.player.y = 100.5;
	s.player.z = 12.5;
	s.player.spawn_x = 5;
	s.player.spawn_y = 100;
	s.player.spawn_z = 5;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
	s.reject_player_pos_this_update = false;
	struct entity** lethal_tick = dict_entity_safe_get(s.entities, 10000);
	*lethal_tick = calloc(1, sizeof(**lethal_tick));
	assert(*lethal_tick);
	(*lethal_tick)->id = 10000;
	(*lethal_tick)->type = ENTITY_ITEM;
	(*lethal_tick)->delay_destroy = -1;
	(*lethal_tick)->tick_server = kill_old_player_body_tick;
	attach_manual_tick_entity(&s, *lethal_tick);
	struct entity** later_tick = dict_entity_safe_get(s.entities, 10001);
	*later_tick = calloc(1, sizeof(**later_tick));
	assert(*later_tick);
	(*later_tick)->id = 10001;
	(*later_tick)->type = ENTITY_ITEM;
	(*later_tick)->delay_destroy = -1;
	(*later_tick)->tick_server = damage_fresh_player_body_tick;
	attach_manual_tick_entity(&s, *later_tick);
	g_fresh_body_damage_ticks = 0;
	assert(server_local_test_tick_entities(&s));
	/* B DID tick (exactly once) and hit the corpse; the dead-guard absorbed
	 * it (asserted inside the callback). Only the explicit request below
	 * constructs the fresh body -- structurally out of reach of this pass. */
	if(g_fresh_body_damage_ticks != 1 || s.player.health > 0)
		death_barrier_failure_mask |= 32u;
	assert(death_barrier_failure_mask == 0);
	assert(server_local_respawn_request(&s));
	assert(s.player.health == MAX_PLAYER_HEALTH);
	finish_same_world_respawn(&s);
	puts("  death barrier: the respawn boundary is prepass-only; corpse hits "
		 "absorb, no same-pass fresh-body damage is possible");
	s.paused = false;
	s.reject_player_pos_this_update = false;

	/* The same FIFO ordering is possible for an ordinary Overworld death. The
	 * successful respawn owns a fresh unpaused gameplay state, so an earlier HOME
	 * toggle must not survive while RETURN_INGAME clears only the client mirror. */
	clear_entities(&s);
	s.death_world_evict = false;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 12.5;
	s.player.y = 100.5;
	s.player.z = 12.5;
	s.player.spawn_x = 5;
	s.player.spawn_y = 100;
	s.player.spawn_z = 5;
	s.player.health = 1;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.fall_distance = 12.0F;
	s.player.old_vel_y = -1.0f;
	s.player.vel_y = -1.0f;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
	struct server_rpc pause_before_overworld_death = {.type = SRPC_TOGGLE_PAUSE};
	struct server_rpc lethal_overworld_landing = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = s.player.x,
			.y = s.player.y,
			.z = s.player.z,
			.rx = 0.0f,
			.ry = 0.0f,
			.vel_y = 0.0f,
			.on_ground = true,
			.sneaking = true,
			.correction_seq = s.player.correction_seq,
		},
	};
	assert(svin_rpc_send(&pause_before_overworld_death));
	assert(svin_rpc_send(&lethal_overworld_landing));
	server_local_test_update_once(&s);
	/* G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW P1 damage ordering + paused
	 * replay): the pause processed while ALIVE; the landing right behind it is
	 * ACCOUNTED at receive, but its handleFalling hit is that packet's event,
	 * applied in the tracker phase -- which a pause-HELD update does not run.
	 * So the held update leaves the body alive, paused, its landing pending. */
	printf("  v4 paused lethal landing: health %d paused %d q %d fall %d\n",
		   (int)s.player.health, (int)s.paused, (int)s.player.motion_q_n,
		   s.player.motion_q_n ? (int)s.player.motion_q[0].fall_damage : -1);
	assert(s.player.health == 1 && s.paused && s.player.fall_distance == 0.0F);
	assert(s.player.motion_q_n == 1 && !s.player.motion_q[0].physics
		   && s.player.motion_q[0].fall_damage > 1);
	server_local_test_update_once(&s); /* still paused: still held */
	assert(s.player.health == 1 && s.paused && s.player.motion_q_n == 1);
	/* Resume: the first simulating update applies the landing once; the
	 * lethal commit owns an unpaused dwell (deviation #18) -- never dead AND
	 * paused. */
	assert(svin_rpc_send(&pause_before_overworld_death));
	server_local_test_update_once(&s);
	assert(s.player.health <= 0);
	assert(!s.reject_player_pos_this_update);
	assert(!s.paused);
	assert(server_local_respawn_request(&s));
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(!s.player.on_ground && !s.player.sneaking);
	assert(s.reject_player_pos_this_update);
	finish_same_world_respawn(&s);
	puts("  death barrier: pause-before-lethal-Overworld-landing clears at the "
		 "death commit; the explicit respawn owns the fresh body");
	s.reject_player_pos_this_update = false;

	/* A pause authored immediately before a lethal Nether landing is processed
	 * first from the reliable FIFO. Death then owns a mandatory Overworld reload:
	 * it must clear that pause, because the loading-screen ownership barrier rejects
	 * every later toggle and a paused recovery loop would otherwise be permanent. */
	clear_entities(&s);
	s.paused = false;
	s.death_world_evict = false;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 12.5;
	s.player.y = 100.5;
	s.player.z = 12.5;
	s.player.spawn_x = 5;
	s.player.spawn_y = 100;
	s.player.spawn_z = 5;
	s.player.health = 1;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.fall_distance = 12.0F;
	s.player.old_vel_y = -1.0f;
	s.player.vel_y = -1.0f;
	s.player.dimension = WORLD_DIM_NETHER;
	s.worlds[0].dimension = WORLD_DIM_NETHER;
	/* Keep this focused fixture out of the separate on-disk death journal. */
	void* level_data = s.level.data;
	s.level.data = NULL;
	struct server_rpc pause_before_nether_death = {.type = SRPC_TOGGLE_PAUSE};
	struct server_rpc lethal_landing = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = s.player.x,
			.y = s.player.y,
			.z = s.player.z,
			.rx = 0.0f,
			.ry = 0.0f,
			.vel_y = 0.0f,
			.on_ground = true,
			.sneaking = true,
			.correction_seq = s.player.correction_seq,
		},
	};
	assert(svin_rpc_send(&pause_before_nether_death));
	assert(svin_rpc_send(&lethal_landing));
	server_local_test_update_once(&s);
	/* G8-BLAST-VELOCITY v4: the paused update is HELD -- the landing's hit
	 * (its packet's handleFalling) waits for the first simulating update. */
	assert(s.player.health == 1 && s.paused && s.player.motion_q_n == 1
		   && s.player.motion_q[0].fall_damage > 1 && !s.death_world_evict);
	assert(svin_rpc_send(&pause_before_nether_death)); /* resume */
	server_local_test_update_once(&s);
	/* Dead commit only: the pause cleared, no eviction latched yet. */
	assert(!s.death_world_evict);
	assert(s.player.health <= 0);
	assert(!s.paused);
	/* The explicit respawn owns the mandatory Overworld reload. Keep the
	 * level archive detached until after it runs, so this focused fixture
	 * stays out of the on-disk death journal exactly as before. */
	assert(server_local_respawn_request(&s));
	s.level.data = level_data;
	assert(s.death_world_evict);
	assert(!s.player.on_ground && !s.player.sneaking);
	assert(!s.paused);
	puts("  death barrier: pause-before-lethal-Nether-landing cannot wedge reload");
	/* Isolate the remaining entity-worklist goldens from this artificial world
	 * switch; the regression above ends at the committed barrier by design. */
	s.death_world_evict = false;
	s.death_world_retry_delay = 0;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
	s.player.health = MAX_PLAYER_HEALTH;
	s.paused = false;
	s.reject_player_pos_this_update = false;

	/* The production worklist must not truncate at the deleted 512 stack cap,
	 * and initial hash insertion order must not replace Beta join order. */
	clear_entities(&s);
	/* Entity erases leave M-lib tombstones. Fill to one below the rehash
	 * threshold, erase every live key, and fail the production allocator only if
	 * server_local_entity_dict_ensure notices that the next insertion must grow. */
	size_t server_tombstone_count = (size_t)s.entities->upper_limit - 1;
	assert(server_tombstone_count > 0);
	for(size_t i = 0; i < server_tombstone_count; i++)
		*dict_entity_safe_get(s.entities, 20000 + (uint32_t)i) = NULL;
	for(size_t i = 0; i < server_tombstone_count; i++)
		assert(dict_entity_erase(s.entities, 20000 + (uint32_t)i));
	assert(dict_entity_size(s.entities) == 0);
	assert((size_t)s.entities->count_delete + 1
		   >= (size_t)s.entities->upper_limit);
	server_local_test_fail_next_entity_dict_realloc();
	struct item_data tombstone_item = {.id = ITEM_APPLE, .count = 1};
	assert(!server_local_spawn_item(
		(vec3) {0.5f, 70.0f, 0.5f}, &tombstone_item, false, &s));
	assert(dict_entity_size(s.entities) == 0);
	clear_entities(&s);
	puts("  entity dictionary: server tombstone rehash fails closed");

	g_tick_order_count = 0;
	enum { DENSE_TICK_COUNT = 609 };
	for(uint32_t i = 0; i < DENSE_TICK_COUNT; i++) {
		uint32_t id = UINT32_C(3000) + (DENSE_TICK_COUNT - 1 - i);
		struct entity** slot = dict_entity_safe_get(s.entities, id);
		*slot = calloc(1, sizeof(**slot));
		assert(*slot);
		(*slot)->id = id;
		(*slot)->delay_destroy = -1;
		(*slot)->tick_server = count_tick;
		attach_manual_tick_entity(&s, *slot);
	}
	assert(server_local_test_tick_entities(&s));
	assert(g_tick_order_count == DENSE_TICK_COUNT);
	for(uint32_t i = 0; i < DENSE_TICK_COUNT; i++)
		assert(g_tick_order[i] == UINT32_C(3000) + i);

	/* World.updateEntities observes its live list size. A TNT joined while the
	 * parent is ticking must therefore move and decrement 80->79 later in this
	 * SAME pass, even though its monotonic id is lower than the test parent's. */
	clear_entities(&s);
	s.next_entity_id = 0;
	struct entity** parent_slot = dict_entity_safe_get(s.entities, 9000);
	*parent_slot = calloc(1, sizeof(**parent_slot));
	assert(*parent_slot);
	(*parent_slot)->id = 9000;
	(*parent_slot)->delay_destroy = -1;
	(*parent_slot)->tick_server = spawn_tnt_tick;
	attach_manual_tick_entity(&s, *parent_slot);
	g_midpass_tnt = NULL;
	assert(server_local_test_tick_entities(&s));
	assert(g_midpass_tnt);
	assert(g_midpass_tnt->data.primed_tnt.fuse == TNT_FUSE_DEFAULT - 1);
	assert(g_midpass_tnt->pos[1] > 90.5f); /* launch + gravity movement ran */
	printf("  entity pass: 609 ordered ticks; mid-pass TNT advanced same tick\n");

	clear_entities(&s);
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_destroy(&s.worlds[0]);
	mem2_free(s.entity_tick_keys);
	s.entity_tick_keys = NULL;
	dict_entity_clear(s.entities);
	string_clear(level);
	for(size_t i = 0; i < 256; i++) {
		if(gstate.players[0].windows[i]) {
			windowc_destroy(gstate.players[0].windows[i]);
			free(gstate.players[0].windows[i]);
			gstate.players[0].windows[i] = NULL;
		}
	}
	dict_entity_it_t client_it;
	dict_entity_it(client_it, gstate.entities);
	while(!dict_entity_end_p(client_it)) {
		free(dict_entity_ref(client_it)->value);
		dict_entity_next(client_it);
	}
	gstate.players[0].local_player = NULL;
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);
	g_drain_run = 0;
	thread_join(&drain);
	printf("test_tnt_primed_env: OK (independent entity; full fluid ring; "
		   "80 live updates, blast on 81)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
