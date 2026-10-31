/*
	Regression test for the near-evict half of the far-ring deadlock.

	The near resident evictor used to free a server chunk and then call the
	BLOCKING clin_rpc_send(CRPC_UNLOAD_CHUNK). If the render/main thread was busy
	and not draining clin_inbox, the server thread could block in WDS_EVICT_NEAR.
	This test fills the client RPC token pool, then proves the near-unload path
	queues the unload and returns immediately; after draining, the queued unload
	flushes.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/b1_bind_client.h"
#include "support/test_fixture_dir.h"
#include "column_observer.h"
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/region_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "game/client_world_context.h"
#include "game/game_state.h"

static struct server_local s;

static int fill_client_inbox(void) {
	int sent = 0;
	while(sent < 128
		  && clin_try_rpc_send(&(struct client_rpc) {.type = CRPC_TIME_SET}))
		sent++;
	assert(sent > 0);
	assert(sent < 128 && "client RPC token pool did not saturate");
	assert(!clin_try_rpc_send(&(struct client_rpc) {.type = CRPC_TIME_SET}));
	return sent;
}


/* ------------------------------------------------------------------------
   S5 (caller-side eviction ownership). Two contracts the pre-S5 evictor did
   not meet:

   (1) An evicted chunk notifies the client with ONE NONBLOCKING entity-destroy
       batch, admitted BEFORE any server entity of that chunk is retired. The
       old path sent one BLOCKING scalar CRPC_ENTITY_DESTROY per entity from
       inside eviction -- the same server-thread block the near/far unload
       deadlock fix removed for CRPC_UNLOAD_CHUNK.
   (2) Any attempted write whose chunk REMAINS RESIDENT marks that chunk dirty,
       even if it began clean, so the retry writes a fresh snapshot. Without it
       a retained entity that moves away before the retry leaves a duplicate
       record on disk (the move-before-retry rule).
   ------------------------------------------------------------------------ */
static size_t scalar_destroys;
static size_t destroy_batches;
static size_t batched_ids;
static uint32_t destroyed_id;

static void observe_destroys(const struct client_rpc* rpc, void* user) {
	(void)user;
	if(rpc->type == CRPC_ENTITY_DESTROY) {
		scalar_destroys++;
		destroyed_id = rpc->payload.entity_destroy.entity_id;
	}
	if(rpc->type == CRPC_ENTITY_DESTROY_BATCH) {
		destroy_batches++;
		batched_ids = rpc->payload.entity_destroy_batch.count;
		assert(rpc->payload.entity_destroy_batch.ids);
		destroyed_id = rpc->payload.entity_destroy_batch.ids[0];
	}
}

/* (5) support: the CLIENT-VISIBLE event stream, read through the column
   observer's own RPC seams (source/column_observer.c's before_rpc/rpc pair,
   whose default tail traces call->type + client_seq -- i.e. one record per
   observed event boundary). The S5 envelope is a TRANSPORT aggregation, so
   this trace must be byte-identical in SHAPE to the pre-S5 stream: N destroy
   EVENTS, each bracketed, and NO record for the envelope itself. */
#define OBS_TRACE_MAX 64
struct obs_record {
	bool before;
	uint32_t type;
};
static struct obs_record obs_trace[OBS_TRACE_MAX];
static size_t obs_trace_count;
static size_t obs_trace_overflow;

static void obs_trace_push(bool before, const struct client_rpc* call) {
	if(obs_trace_count >= OBS_TRACE_MAX) {
		obs_trace_overflow++;
		return;
	}
	obs_trace[obs_trace_count].before = before;
	obs_trace[obs_trace_count].type = (uint32_t)call->type;
	obs_trace_count++;
}

static void obs_before_rpc(const struct client_rpc* call) {
	obs_trace_push(true, call);
}
static void obs_after_rpc(const struct client_rpc* call) {
	obs_trace_push(false, call);
}

static struct entity* spawn_test_item(uint32_t id, w_coord_t cx) {
	struct entity* e = malloc(sizeof(*e));
	assert(e);
	entity_item(id, e, true, &s.world,
				(struct item_data) {.id = ITEM_STICK, .count = 1});
	e->teleport(e, (vec3) {cx * 16 + 1.5F, 72.0F, 1.5F});
	dict_entity_set_at(s.entities, id, e);
	return e;
}

static struct server_chunk* load_clean_chunk(w_coord_t cx) {
	struct server_chunk* c = NULL;
	assert(server_world_load_chunk(&s.world, cx, 0, &c) && c);
	/* Dictionary insertion may move values: resolve the owner again. */
	c = dict_server_chunks_get(s.world.chunks, S_CHUNK_ID(cx, 0));
	assert(c);
	c->modified = false;
	return c;
}

static void test_evict_entity_ownership(void) {
	char path[128];
	test_fixture_dir("test_near_evict_own", path, sizeof(path));
	MKDIR(path);
	MKDIR(test_fixture_join(path, "/region"));

	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	dict_entity_init(s.entities);

	string_t level;
	string_init_set_str(level, path);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	b1_bind_client_world(&s.worlds[0]);
	clin_test_set_rpc_observer(observe_destroys, NULL);

	/* (1) one batch, no per-entity scalar send. */
	struct server_chunk* c = load_clean_chunk(96);
	(void)c;
	struct entity* item = spawn_test_item(100, 96);
	scalar_destroys = destroy_batches = 0;
	assert(server_world_save_chunk(&s.world, true, 96, 0));
	assert(!server_world_is_chunk_loaded(&s.world, 96, 0));
	assert(!dict_entity_get(s.entities, 100));
	assert(scalar_destroys == 0
		   && "eviction must not emit a per-entity blocking destroy");
	assert(destroy_batches == 1 && batched_ids == 1 && destroyed_id == 100
		   && "exactly one destroy batch carries the whole chunk");
	clin_drain_discard();

	/* (1b) a FULL client inbox is a retry, never a blocking send: every owner
	 *      of the chunk stays live and nothing is notified. stage22-0927: the
	 *      headroom check runs BEFORE the write, so the refusal is a DEFERRAL
	 *      -- nothing written, the chunk not re-dirtied, no save failure. */
	uint32_t fail_before = region_save_failure_count();
	load_clean_chunk(112);
	item = spawn_test_item(102, 112);
	scalar_destroys = destroy_batches = 0;
	fill_client_inbox();
	assert(!server_world_save_chunk(&s.world, true, 112, 0));
	assert(server_world_is_chunk_loaded(&s.world, 112, 0));
	assert(*dict_entity_get(s.entities, 102) == item);
	assert(destroy_batches == 0 && scalar_destroys == 0);
	c = dict_server_chunks_get(s.world.chunks, S_CHUNK_ID(112, 0));
	assert(c && !c->modified && s.worlds[0].evict_retire_deferred
		   && region_save_failure_count() == fail_before
		   && "a full inbox defers before writing: nothing to re-dirty, no failure");
	clin_drain_discard();
	assert(server_world_save_chunk(&s.world, true, 112, 0));
	assert(!server_world_is_chunk_loaded(&s.world, 112, 0));
	assert(!dict_entity_get(s.entities, 102));
	assert(destroy_batches == 1 && destroyed_id == 102);
	clin_drain_discard();

	/* (2) a FAILED write also retains every owner, but does NOT re-dirty: the
	 *     region rolled back, so disk still matches the chunk's persisted
	 *     flags and the caller's retry delay owns the retry. Pinning this
	 *     keeps the S5 re-dirty scoped to commit-then-retain (widening it
	 *     diverges test_column_dedup_state_equiv's frozen trace). */
	c = load_clean_chunk(128);
	item = spawn_test_item(101, 128);
	scalar_destroys = destroy_batches = 0;
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	assert(!server_world_save_chunk(&s.world, true, 128, 0));
	c = dict_server_chunks_get(s.world.chunks, S_CHUNK_ID(128, 0));
	assert(c && *dict_entity_get(s.entities, 101) == item);
	assert(destroy_batches == 0 && scalar_destroys == 0
		   && "a write that never committed notifies nothing");
	assert(!c->modified && "a rolled-back write leaves the dirty bit alone");
	clin_drain_discard();

	/* (3) move-before-retry: a retained entity that leaves the chunk before
	 *     the retry must not leave a duplicate record on disk. The retry sees
	 *     an entity-free chunk and would treat it as an untouched no-op if the
	 *     first attempt had not re-dirtied it. */
	c = load_clean_chunk(160);
	item = spawn_test_item(103, 160);
	scalar_destroys = destroy_batches = 0;
	fill_client_inbox();
	assert(!server_world_save_chunk(&s.world, true, 160, 0));
	c = dict_server_chunks_get(s.world.chunks, S_CHUNK_ID(160, 0));
	/* stage22-0927: the deferral wrote nothing, so no record names the entity
	 * and the move below cannot leave a duplicate even before the retry. */
	assert(c && !c->modified && !c->persisted_entities_nonempty
		   && s.worlds[0].evict_retire_deferred
		   && "the deferred attempt published no record");
	item->teleport(item, (vec3) {161 * 16 + 1.5F, 72.0F, 1.5F});
	clin_drain_discard();
	assert(server_world_save_chunk(&s.world, true, 160, 0));
	assert(!server_world_is_chunk_loaded(&s.world, 160, 0));
	assert(destroy_batches == 0 && "no entity left to notify");
	assert(*dict_entity_get(s.entities, 103) == item);
	size_t live_before = dict_entity_size(s.entities);
	c = NULL;
	assert(server_world_load_chunk(&s.world, 160, 0, &c) && c);
	c = dict_server_chunks_get(s.world.chunks, S_CHUNK_ID(160, 0));
	assert(c && !c->persisted_entities_nonempty
		   && "the retry cleared the old snapshot");
	assert(dict_entity_size(s.entities) == live_before
		   && "reload must not resurrect a duplicate of the moved entity");
	clin_drain_discard();

	/* (5) THE CLIENT-VISIBLE STREAM IS UNCHANGED BY THE ENVELOPE -- pinned,
	 *     not asserted in prose. Evict a chunk holding THREE archive-kind
	 *     entities, drain the inbox through the real client path
	 *     (clin_update_drain -> clin_process_queued -> clin_process) with the
	 *     column observer's RPC seams installed, and require the observed
	 *     event stream to be exactly three BRACKETED CRPC_ENTITY_DESTROY
	 *     events with NO record for CRPC_ENTITY_DESTROY_BATCH. This FAILS the
	 *     moment anyone makes the envelope visible as one client event (the
	 *     `observe_envelope` gate in clin_process, or the replayed per-id
	 *     bracket in its CRPC_ENTITY_DESTROY_BATCH handler). */
	dict_entity_init(gstate.entities);
	load_clean_chunk(176);
	for(uint32_t id = 201; id <= 203; id++) {
		spawn_test_item(id, 176);
		/* A client-side stand-in so the destroy EVENT has something to apply
		 * to: the trace shape is the contract, the removal is the evidence
		 * that these are real events and not bookkeeping. */
		struct entity** slot = dict_entity_safe_get(gstate.entities, id);
		assert(slot);
		*slot = calloc(1, sizeof(**slot));
		assert(*slot);
		(*slot)->id = id;
		(*slot)->type = ENTITY_MINECART;
	}
	scalar_destroys = destroy_batches = 0;
	obs_trace_count = obs_trace_overflow = 0;
	clin_column_observe_before_rpc = obs_before_rpc;
	clin_column_observe_rpc = obs_after_rpc;
	assert(server_world_save_chunk(&s.world, true, 176, 0));
	assert(destroy_batches == 1 && batched_ids == 3
		   && "three entities leave the server as ONE transport envelope");
	clin_update_drain();
	clin_column_observe_before_rpc = NULL;
	clin_column_observe_rpc = NULL;
	assert(!obs_trace_overflow && "observed stream longer than the trace");
	for(size_t i = 0; i < obs_trace_count; i++)
		assert(obs_trace[i].type != (uint32_t)CRPC_ENTITY_DESTROY_BATCH
			   && "the batch is TRANSPORT: it must never be a client event");
	size_t observed_destroys = 0;
	for(size_t i = 0; i < obs_trace_count; i++) {
		if(obs_trace[i].type != (uint32_t)CRPC_ENTITY_DESTROY) continue;
		assert(obs_trace[i].before && i + 1 < obs_trace_count
			   && obs_trace[i + 1].type == (uint32_t)CRPC_ENTITY_DESTROY
			   && !obs_trace[i + 1].before
			   && "every destroy event keeps its before/after bracket");
		observed_destroys++;
		i++;
	}
	assert(observed_destroys == 3
		   && "N ids in one envelope == N client-visible destroy events");
	assert(obs_trace_count == 6
		   && "three bracketed destroy events and NOTHING else -- an envelope "
			  "record would push this to 8");
	for(uint32_t id = 201; id <= 203; id++)
		assert(!dict_entity_get(gstate.entities, id)
			   && "each observed event actually applied its destroy");
	dict_entity_clear(gstate.entities);
	clin_drain_discard();

	/* (4) the transferred id array is released EXACTLY once on every path. */
	dict_entity_init(gstate.entities);
	clin_test_destroy_batch_releases_reset();
	uint32_t* applied_ids = malloc(2 * sizeof(*applied_ids));
	assert(applied_ids);
	applied_ids[0] = 901;
	applied_ids[1] = 902;
	struct entity** doomed = dict_entity_safe_get(gstate.entities, 901);
	assert(doomed);
	*doomed = calloc(1, sizeof(**doomed));
	assert(*doomed);
	(*doomed)->id = 901;
	(*doomed)->type = ENTITY_MINECART;
	struct client_rpc applied = {
		.type = CRPC_ENTITY_DESTROY_BATCH,
		.payload.entity_destroy_batch = {.ids = applied_ids, .count = 2},
	};
	clin_process(&applied);
	assert(!applied.payload.entity_destroy_batch.ids
		   && applied.payload.entity_destroy_batch.count == 0);
	assert(!dict_entity_get(gstate.entities, 901));
	assert(clin_test_destroy_batch_releases() == 1);
	/* Idempotent: a second consumption of the same envelope frees nothing. */
	clin_process(&applied);
	assert(clin_test_destroy_batch_releases() == 1);

	uint32_t* discarded_ids = malloc(sizeof(*discarded_ids));
	assert(discarded_ids);
	discarded_ids[0] = 903;
	struct client_rpc discarded = {
		.type = CRPC_ENTITY_DESTROY_BATCH,
		.payload.entity_destroy_batch = {.ids = discarded_ids, .count = 1},
	};
	assert(clin_try_rpc_send(&discarded));
	clin_drain_discard();
	assert(clin_test_destroy_batch_releases() == 2
		   && "discard releases ownership without applying destroys");

	uint32_t* rejected_ids = malloc(sizeof(*rejected_ids));
	assert(rejected_ids);
	rejected_ids[0] = 904;
	fill_client_inbox();
	struct client_rpc rejected = {
		.type = CRPC_ENTITY_DESTROY_BATCH,
		.payload.entity_destroy_batch = {.ids = rejected_ids, .count = 1},
	};
	assert(!clin_try_rpc_send(&rejected));
	assert(rejected.payload.entity_destroy_batch.ids == rejected_ids
		   && "a refused admission transfers nothing");
	assert(clin_test_destroy_batch_releases() == 2);
	free(rejected_ids);
	clin_drain_discard();
	dict_entity_clear(gstate.entities);

	clin_test_set_rpc_observer(NULL, NULL);
	server_world_destroy(&s.world);
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(s.entities);
	string_clear(level);
	test_fixture_dir_cleanup(path);
	puts("  S5 eviction ownership: batch notification + retained-write dirty OK");
}

int main(void) {
	clin_init();
	/* Split-dims B1: world-scoped RPCs (the entity destroys drained in (5))
	 * reach the client only through an installed world context -- a real
	 * client gets it from CRPC_WORLD_RESET. This fixture never resets a
	 * client world, so it installs the single unstamped context directly. */
	assert(client_ctx_registry_install(clin_world_contexts(), 0, 0, 0));
	memset(&s, 0, sizeof(s));
	g_near_evict_deferred = 0;

	assert(server_local_test_near_unload_send_or_defer(&s, 1, 2));
	assert(server_local_test_near_unload_pending_count(&s) == 0);
	assert(g_near_evict_deferred == 0);
	clin_drain_discard();

	int filled = fill_client_inbox();
	assert(server_local_test_near_unload_send_or_defer(&s, 33, -7));
	assert(server_local_test_near_unload_pending_count(&s) == 1);
	assert(g_near_evict_deferred == 1);
	assert(!server_local_test_near_unload_flush(&s));
	assert(server_local_test_near_unload_pending_count(&s) == 1);

	clin_drain_discard();
	assert(server_local_test_near_unload_flush(&s));
	assert(server_local_test_near_unload_pending_count(&s) == 0);
	clin_drain_discard();

	test_evict_entity_ownership();

	printf("test_near_evict_defer: OK (filled %d RPC tokens; near unload deferred "
		   "then flushed without blocking)\n",
		   filled);
	return 0;
}
