/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * Split-dims lane B, card B-NOW (astra-next-0926 rows "B NOW" / "B1, alongside
 * A/C"): shared rig for the tests/test_b1_reset_*.c real-producer reset /
 * reconnect isolation tests.
 *
 * What "real producer" means here: the RPCs under test are emitted by the
 * production server_local emitters (server_local_load_world_impl /
 * _unload_world_impl, the near-unload sender, the eviction destroy batch,
 * server_local_sleep_tick) of a real server_local loaded through the real
 * world-load path, travel the real clin_inbox token channel, and are consumed
 * by the real client consumer (clin_update_drain -> clin_process). Nothing
 * here writes world_index / world_generation into a message: whatever stamp a
 * message carries is the stamp its producer gave it.
 *
 * Two kinds of clause:
 *  - GREEN invariants that already hold (single payload, pooled payload
 *    release, lease/epoch rejection, fresh client epochs): plain assert().
 *  - RED clauses whose isolation RELIES on producer stamps that do not exist
 *    yet (Astra B1 accept limits 0925: "zero producer families are stamped").
 *    They go through b1r_red(), which names the producer that must stamp and
 *    counts the miss. In "red" mode main returns 1 on any miss (never abort),
 *    and CMake registers that run WILL_FAIL. In "base" mode the same binary
 *    runs every setup and GREEN assert and only RECORDS the misses (exit 0),
 *    registered as an ordinary test -- so a setup abort can never hide behind
 *    WILL_FAIL. When A/C stamp the named producer, the red run starts to pass:
 *    WILL_FAIL then fails the suite, which is the signal to drop it. */
#ifndef TEST_B1_RESET_HARNESS_H
#define TEST_B1_RESET_HARNESS_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define B1R_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define B1R_MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "entity/combat.h"

static struct client_ctx_registry* b1r_reg;
static bool b1r_red_mode;
static unsigned b1r_unmet;

/* One RED clause. `producer` is the file::function (+ message) that must
 * adopt clin_rpc_stamp / the WORLD_RESET identity fields for it to hold. */
static inline void b1r_red(bool ok, const char* producer, const char* clause) {
	if(ok) {
		printf("  held   : %s\n", clause);
		return;
	}
	b1r_unmet++;
	printf("  UNMET  : %s\n           producer must stamp: %s\n", clause,
		   producer);
}

static inline void b1r_mode(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	b1r_red_mode = argc > 1 && strcmp(argv[1], "red") == 0;
	if(argc > 1 && !b1r_red_mode && strcmp(argv[1], "base") != 0) {
		fprintf(stderr, "usage: %s base|red\n", argv[0]);
		exit(2);
	}
}

static inline int b1r_finish(const char* name) {
	if(!b1r_unmet) {
		printf("PASS %s: every clause holds\n", name);
		return 0;
	}
	if(b1r_red_mode) {
		printf("RED %s: %u stamp-dependent clause(s) unmet -- production "
			   "traffic is UNSTAMPED (exit 1, registered WILL_FAIL)\n",
			   name, b1r_unmet);
		return 1;
	}
	printf("PASS %s (base): setup + GREEN invariants hold; %u RED clause(s) "
		   "recorded, enforced by the WILL_FAIL red run\n", name, b1r_unmet);
	return 0;
}

/* The client half: real inbox + real consumer, exactly the order the
 * route/column tests use. */
static inline void b1r_client_start(void) {
	blocks_init();
	items_init();
	assert(svin_init());
	assert(clin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	b1r_reg = clin_world_contexts();
	assert(b1r_reg);
}

/* The server half: a real server_local brought up by the production
 * world-load path (server_local_load_world_impl), superflat so the load is
 * cheap and deterministic. Recipe: tests/test_a1b_spawner_tracker_reload.c. */
struct b1r_server {
	struct server_local s;
	char dir[128];
	string_t level;
};

static inline void b1r_server_create(struct b1r_server* b, const char* fixture) {
	test_fixture_dir(fixture, b->dir, sizeof(b->dir));
	B1R_MKDIR(b->dir);
	server_local_test_create_without_thread(&b->s, false, false, false, false,
											false, true /*superflat*/, false);
	string_init_set_str(b->level, b->dir);
	struct level_archive la;
	assert(level_archive_create_new(&la, b->level, 4242,
		(vec3) {8.5F, 4.0F + PLAYER_EYE_HEIGHT, 8.5F}));
	level_archive_destroy(&la);
}

/* Load (or reload) the world through the production path. Its CRPC_WORLD_RESET
 * and the rest of the load burst stay QUEUED in clin_inbox for the caller. */
static inline struct server_world* b1r_server_load(struct b1r_server* b) {
	assert(server_local_load_world_impl(&b->s, b->level));
	struct server_world* w = &b->s.worlds[0];
	assert(w->generation != 0);
	struct server_chunk* c = NULL;
	if(!dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0)))
		assert(server_world_load_chunk(w, 0, 0, &c) && c);
	return w;
}

static inline void b1r_server_destroy(struct b1r_server* b) {
	if(b->s.player.has_pos)
		server_local_unload_world_impl(&b->s, false);
	clin_drain_discard();
	string_clear(b->level);
	test_fixture_dir_cleanup(b->dir);
}

/* Consume-side capture: the LAST consumed message of one type, copied as the
 * real consumer saw it (before routing). */
static struct {
	enum client_rpc_type type;
	struct client_rpc last;
	unsigned seen;
} b1r_cap[8];
static unsigned b1r_cap_count;

static inline void b1r_consume_observer(const struct client_rpc* call, void* unused) {
	(void)unused;
	for(unsigned i = 0; i < b1r_cap_count; i++)
		if(b1r_cap[i].type == call->type) {
			b1r_cap[i].last = *call;
			b1r_cap[i].seen++;
		}
}

static inline void b1r_capture(enum client_rpc_type type) {
	assert(b1r_cap_count < sizeof(b1r_cap) / sizeof(b1r_cap[0]));
	b1r_cap[b1r_cap_count].type = type;
	b1r_cap[b1r_cap_count].seen = 0;
	b1r_cap_count++;
	clin_test_set_consume_observer(b1r_consume_observer, NULL);
}

static inline const struct client_rpc* b1r_captured(enum client_rpc_type type,
											 unsigned* seen) {
	for(unsigned i = 0; i < b1r_cap_count; i++)
		if(b1r_cap[i].type == type) {
			if(seen) *seen = b1r_cap[i].seen;
			return b1r_cap[i].seen ? &b1r_cap[i].last : NULL;
		}
	if(seen) *seen = 0;
	return NULL;
}

/* Did the producer stamp this message with the world that produced it? */
static inline bool b1r_stamped_by(const struct client_rpc* m,
						   const struct server_world* w, uint32_t generation) {
	return m && m->world_generation != 0 && m->world_generation == generation
		   && m->world_index == w->world_index;
}

/* GREEN single-payload invariant: at most one live slot, and it is slot 0
 * whenever every producer is unstamped. */
static inline void b1r_assert_single_payload(void) {
	unsigned live = 0;
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++)
		live += client_ctx_registry_current(b1r_reg, slot) != NULL;
	assert(live <= 1 && "single payload: never two live client world slots");
}

/* Drain the inbox through the real consumer until it is empty. */
static inline void b1r_drain(void) {
	for(unsigned i = 0; i < 64; i++) {
		clin_update_drain();
		if(!clin_world_message_held())
			break;
	}
	assert(!clin_world_message_held());
}

#endif
