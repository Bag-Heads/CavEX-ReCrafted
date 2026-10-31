/*
	stage22-0927 (tmp/jobs/split-dimensions-plan-0924/out/SAVE-STAGE22-RCA.md):
	the real-Wii split_play_8m save_fail_n=1 at stage 22 (ENTITY_RETIRE),
	reproduced on PC through the PRODUCTION tick.

	Hardware: a near eviction of a resident chunk that holds live entities
	found the 16-token client RPC pool empty when it tried to admit the
	chunk's entity-destroy batch. The region record had already COMMITTED;
	the refusal then re-dirtied the chunk, reported a save FAILURE (stage 22,
	save_fail_n +1), bumped estk and parked ALL near eviction for 20 ticks.

	Fixture: one resident chunk (0,0) holding one live item entity, far
	outside the player's view (the player stands on a disk-streamed island
	at chunk 64); the client pool is filled just before one server tick, so
	that chunk -- the furthest, the only eviction candidate -- meets a full
	inbox exactly as on the console.

	CONTRACT (GREEN after the fix):
	  R1 a full inbox is an eviction DEFERRAL, not a save failure:
	     region_save_failure_count() does not rise and the stage is never
	     RSF_ENTITY_RETIRE; estk (g_evict_stuck) does not rise.
	  R2 nothing is committed and thrown away: the chunk keeps its pre-tick
	     dirty bit and persisted-entity marker (no record was written for an
	     eviction that could not retire its owners).
	  R3 every owner is still live and the chunk still resident.
	  R4 the retry is the NEXT tick, not 20 ticks later
	     (chunk_save_retry_delay stays 0): after the client drains, one tick
	     evicts the chunk, admits exactly one destroy batch for the entity,
	     and frees it.
	  R5 the evicted record names the entity exactly once: a reload restores
	     one entity.
	On the base (f32f5f6f) R1/R2/R4 fail: stage 22 + estk + delay 20.
*/

#include "support/split_residency_fixture.h"

#include <stdatomic.h>
#include <stdio.h>

#include "entity/entity.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/region_archive.h"

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static bool draining = true;
static size_t destroy_batches, batched_ids;
static uint32_t destroyed_id;

/* The client's consumer role, inline: drain after every admitted send while
   `draining` (the respawn fixture's pattern). Also counts destroy batches. */
static void observe(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_ENTITY_DESTROY_BATCH) {
		destroy_batches++;
		batched_ids = rpc->payload.entity_destroy_batch.count;
		destroyed_id = rpc->payload.entity_destroy_batch.ids[0];
	}
	if(draining)
		clin_update_drain();
}

static void tick(struct server_local* s) {
	server_local_test_update_once(s);
	if(draining)
		clin_update_drain();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
}

/* Deadlock breaker for the measured tick: if some BLOCKING producer inside
   the tick waits on the full pool, drain after 3 s and record it -- the
   test is then invalid (asserted below), never hung. */
static atomic_bool tick_done = ATOMIC_VAR_INIT(false);
static atomic_bool rescued = ATOMIC_VAR_INIT(false);
static void* rescue_entry(void* arg) {
	(void)arg;
	for(int ms = 0; ms < 3000; ms++) {
		if(atomic_load(&tick_done))
			return NULL;
		thread_msleep(1);
	}
	atomic_store(&rescued, true);
	while(!atomic_load(&tick_done)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static int fill_client_inbox(void) {
	int sent = 0;
	while(sent < 128
		  && clin_try_rpc_send(&(struct client_rpc) {.type = CRPC_TIME_SET}))
		sent++;
	assert(sent < 128 && "client RPC token pool did not saturate");
	return sent;
}

#define ITEM_ID 900001u

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_evict_retire_pool_full");
	assert(clin_init() && svin_init());
	blocks_init();
	items_init();
	recipe_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
								.local_entity = 0},
	});
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	/* Two populated islands on disk: the victim at chunk 0, the player's at
	   chunk 64 (far beyond any view radius). */
	for(int island = 0; island < 2; island++)
		for(int z = -2; z <= 2; z++)
			for(int x = -2; x <= 2; x++)
				split_populate(s, island * 64 + x, z);
	/* Overworld-only fixture: worlds[0] named explicitly (world alias gate). */
	while(dict_server_chunks_size(s->worlds[0].chunks)) {
		dict_server_chunks_it_t it;
		dict_server_chunks_it(it, s->worlds[0].chunks);
		int64_t id = dict_server_chunks_ref(it)->key;
		assert(server_world_save_chunk(&s->worlds[0], true, S_CHUNK_X(id),
									   S_CHUNK_Z(id)));
	}
	s->worlds[0].generator = NULL; /* disk-only streaming, no population noise */

	/* The player on island 64; converge its near ring with the client
	   draining (held transport epoch: no simulation while streaming). */
	s->player.x = 64 * CHUNK_SIZE + 8.5;
	s->player.z = 8.5;
	s->player.y = 4.0 + PLAYER_EYE_HEIGHT;
	s->input_ack_pending = 1;
	s->input_epoch_counter = 1;
	s->player.finished_loading = true;
	clin_test_set_rpc_observer(observe, NULL);
	for(unsigned n = 0; n < 120; n++)
		tick(s);
	s->input_ack_pending = 0; /* loaded: the near evictor runs */
	for(unsigned n = 0; n < 40; n++)
		tick(s);
	clin_update_drain();

	/* The victim: chunk (0,0) resident, clean, holding one live item. */
	struct server_chunk* c = NULL;
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &c) && c);
	c = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert(c);
	c->modified = false;
	bool persisted_before = c->persisted_entities_nonempty;
	struct entity* item = malloc(sizeof(*item));
	assert(item);
	entity_item(ITEM_ID, item, true, &s->worlds[0],
				(struct item_data) {.id = ITEM_STICK, .count = 1});
	item->teleport(item, (vec3) {8.5F, 70.0F, 8.5F});
	dict_entity_set_at(s->entities, ITEM_ID, item);
	assert(server_world_is_chunk_loaded(&s->worlds[0], 0, 0));
	assert(s->chunk_save_retry_delay == 0);

	/* THE TICK: client pool full, nobody draining. */
	uint32_t fail_before = region_save_failure_count();
	uint32_t estk_before = g_evict_stuck;
	destroy_batches = 0;
	draining = false;
	fill_client_inbox();
	struct thread rescue;
	atomic_store(&tick_done, false);
	assert(thread_create(&rescue, rescue_entry, NULL, 64));
	server_local_test_update_once(s);
	atomic_store(&tick_done, true);
	thread_join(&rescue);
	check(!atomic_load(&rescued),
		  "fixture validity: no blocking producer waited on the full pool");
	c = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
	check(c != NULL && dict_entity_get(s->entities, ITEM_ID)
			  && *dict_entity_get(s->entities, ITEM_ID) == item
			  && destroy_batches == 0,
		  "R3 full inbox: the chunk stays resident, its entity live, "
		  "nothing notified");
	check(region_save_failure_count() == fail_before
			  && region_save_failure_stage() != RSF_ENTITY_RETIRE,
		  "R1 a full inbox is not reported as a save failure (no stage 22)");
	check(g_evict_stuck == estk_before, "R1 estk does not rise");
	check(c && !c->modified && c->persisted_entities_nonempty == persisted_before,
		  "R2 no record was committed for an eviction that could not retire "
		  "its owners");
	check(s->chunk_save_retry_delay == 0,
		  "R4 no 20-tick eviction stall for a transient full inbox");

	/* The client drains; the next tick evicts. */
	draining = true;
	clin_drain_discard();
	tick(s);
	check(!server_world_is_chunk_loaded(&s->worlds[0], 0, 0)
			  && !dict_entity_get(s->entities, ITEM_ID)
			  && destroy_batches == 1 && batched_ids == 1
			  && destroyed_id == ITEM_ID,
		  "R4 the next tick evicts, one destroy batch for the entity");

	/* R5: the record names the entity exactly once. */
	size_t live_before = dict_entity_size(s->entities);
	c = NULL;
	check(server_world_load_chunk(&s->worlds[0], 0, 0, &c) && c
			  && dict_entity_size(s->entities) == live_before + 1,
		  "R5 a reload restores exactly one entity");

	clin_test_set_rpc_observer(NULL, NULL);
	clin_update_drain();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	dict_entity_clear(gstate.entities);
	split_destroy(s);
	printf("%s test_evict_retire_pool_full (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
