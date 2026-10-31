/* Copyright (c) 2026 CaveWii_v2 contributors
 * IN-003: client light == server light after the initial generation-light
 * publication (single-player local session, seed 4242, Overworld).
 *
 * Beta 1.7.3 [CFR /c/tmp/mcb173-server-decompile/out/net/minecraft/src/
 * Chunk.java:474 getChunkData]: the chunk payload a client receives is a
 * straight copy of the server chunk's STORED blocks, metadata, skylight and
 * blocklight arrays, and every later stored light change in a watched column
 * is re-sent (World.setLightValue -> markBlockNeedsUpdate). So once loading
 * has settled, every column the client holds carries exactly the server's
 * stored Sky and Block nibbles.
 *
 * This drives the production path end to end: the real local session loads
 * the view-distance square around the player (server load + generation-light
 * drain, both through server_local_update), the real local publication path
 * (CRPC_CHUNK snapshots, settled-light republish) feeds the real client FIFO
 * consumer (clin_update_drain -> clin_process) into gstate.world, and the
 * client's own per-frame lighting driver runs until idle. Then every column
 * resident on both sides is compared cell by cell. On the first difference it
 * prints 'IN-003 RED: chunk (x,z) cell (x,y,z) client=a server=b' and exits 1.
 *
 * IN-004 continues after initial publication with watched edits and an
 * unwatched edit between real retirement and fresh re-admission. The server
 * is paused during initial loading; normal ticks resume for edits/movement. */
#include "support/owner_swap_harness.h"
#include "lighting.h"
#include "stack.h"
#include "world.h"

#define SEED 4242
#define IDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * CHUNK_SIZE) * WORLD_HEIGHT)

/* The client's normal per-frame lighting driver, run until idle. */
static void client_light_idle(void) {
	for(int n = 0; n < 1000000; n++) {
		if(stack_empty(&gstate.world.lighting_updates)
		   && lighting_flood_queue_empty(&gstate.world.lighting_flood_queue))
			return;
		world_update_lighting(&gstate.world);
	}
	assert(!"client lighting never went idle");
}

/* Consume the client FIFO, deferred output included, then relight. */
static void client_pump(void) {
	clin_update_drain();
	for(unsigned n = 0; !clin_rpc_flush_pending(); n++) {
		assert(n < 10000);
		clin_update_drain();
	}
	clin_update_drain();
	client_light_idle();
}

/* Server side settled: no queued generation light, no column owing a
 * republish of a changed sent snapshot. */
static bool server_settled(struct server_local* s) {
	struct server_world* w = &s->worlds[0];
	if(server_world_light_pending(w))
		return false;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		const struct server_chunk* c = &dict_server_chunks_ref(it)->value;
		if(c->sent && c->pub_debt)
			return false;
	}
	return true;
}

/* Setup failures are distinct from a successfully reproduced light mismatch. */
static void require(bool ok, const char* name, const char* reason) {
	if(!ok) {
		fprintf(stderr, "IN-004 SETUP: step %s: %s\n", name, reason);
		exit(2);
	}
}

static char first_red[256];
static bool compare_light(struct server_local* s, const char* name, bool initial) {
	unsigned columns = 0, stale_columns = 0;
	unsigned long sky_diff = 0, blk_diff = 0;
	bool reported = false;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, s->worlds[0].chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		struct server_chunk* c = &dict_server_chunks_ref(it)->value;
		int64_t id = dict_server_chunks_ref(it)->key;
		int cx = S_CHUNK_X(id), cz = S_CHUNK_Z(id);
		if(!c->sent || !world_find_chunk(&gstate.world, cx * 16, 0, cz * 16))
			continue;
		columns++;
		bool stale = false;
		for(int lx = 0; lx < 16; lx++) for(int lz = 0; lz < 16; lz++)
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				int x = cx * 16 + lx, z = cz * 16 + lz;
				size_t i = IDX(lx, y, lz);
				struct block_data b = world_get_block(&gstate.world, x, y, z);
				const uint8_t sv_sky = nibble_read(c->lighting_sky, i);
				const uint8_t sv_blk = nibble_read(c->lighting_torch, i);
				const bool ds = b.sky_light != sv_sky, db = b.torch_light != sv_blk;
				if(!ds && !db)
					continue;
				sky_diff += ds;
				blk_diff += db;
				stale = true;
				if(!reported) {
					printf("%s RED: step %s chunk (%d,%d) cell (%d,%d,%d) client=%u server=%u\n",
						initial ? "IN-003" : "IN-004", name, cx, cz, x, y, z,
						ds ? b.sky_light : b.torch_light, ds ? sv_sky : sv_blk);
					if(!first_red[0])
						snprintf(first_red, sizeof(first_red),
							"IN-004 RED: step %s chunk (%d,%d) cell (%d,%d,%d) client=%u server=%u (plane %s)",
							name, cx, cz, x, y, z, ds ? b.sky_light : b.torch_light,
							ds ? sv_sky : sv_blk, ds ? "Sky" : "Block");
					printf("  (plane %s)\n", ds ? "Sky" : "Block");
					reported = true;
				}
			}
		stale_columns += stale;
	}
	const unsigned want = (2 * MAX_VIEW_DISTANCE + 1) * (2 * MAX_VIEW_DISTANCE + 1);
	printf("IN-004: step %s: %u columns compared (view square %u), %u stale; stale cells Sky %lu Block %lu\n",
		name, columns, want, stale_columns, sky_diff, blk_diff);
	/* setup guard: the whole near view square was published and compared */
	require(columns >= want, name, "whole near view square must be compared");
	return reported;
}

/* Drain all four stages after each action, including publication that can
 * enqueue more client work. A stable count alone is insufficient after moving. */
static void settle(struct server_local* s, const char* name) {
	unsigned quiet = 0;
	size_t resident = dict_server_chunks_size(s->worlds[0].chunks);
	for(unsigned n = 0; n < 4000; n++) {
		tick(s); /* archive operations stay on the fixture's owning thread */
		client_pump();
		size_t now = dict_server_chunks_size(s->worlds[0].chunks);
		quiet = server_settled(s) && !s->anchor_near_owed[0]
			&& now == resident ? quiet + 1 : 0;
		resident = now;
		if(quiet == 8)
			return;
	}
	printf("IN-004 drain state: pending=%d fault=%d reason=%d near-owed=%u resident=%zu\n",
		server_world_light_pending(&s->worlds[0]), s->worlds[0].light_fault,
		s->worlds[0].light_fault_reason, s->anchor_near_owed[0], resident);
	require(false, name, "timeout draining server/publication/transport/client");
}

#define TARGET_CX (-MAX_VIEW_DISTANCE)
#define EDIT_X (TARGET_CX * CHUNK_SIZE + 8)
#define EDIT_Z 8
#define EDIT_Y (WORLD_HEIGHT - 6)

static unsigned target_snapshots, target_unloads;
static void edit_consumed(const struct client_rpc* rpc, void* unused) {
	consumed(rpc, unused);
	if(rpc->type == CRPC_CHUNK
	   && WCOORD_CHUNK_OFFSET(rpc->payload.chunk.x) == TARGET_CX
	   && WCOORD_CHUNK_OFFSET(rpc->payload.chunk.z) == 0)
		target_snapshots++;
	if(rpc->type == CRPC_UNLOAD_CHUNK
	   && rpc->payload.unload_chunk.x == TARGET_CX
	   && rpc->payload.unload_chunk.z == 0)
		target_unloads++;
}

static struct block_data probe(struct server_local* s) {
	struct block_data b;
	require(server_world_get_block(&s->worlds[0], EDIT_X, EDIT_Y - 1,
		EDIT_Z, &b), "probe", "target server cell absent");
	return b;
}

static void target_present(struct server_local* s, const char* name) {
	struct server_chunk* c = dict_server_chunks_get(s->worlds[0].chunks,
		S_CHUNK_ID(TARGET_CX, 0));
	require(c && c->sent && world_find_chunk(&gstate.world, EDIT_X, 0, EDIT_Z),
		name, "target must be published and present on both sides");
}

static void edit_case(struct server_local* s) {
	acknowledge(s); /* permit normal eviction and publication after loading */
	s->paused = false;
	s->player.y = WORLD_HEIGHT + PLAYER_EYE_HEIGHT; /* keep fixture body out of terrain */
	clin_test_set_consume_observer(edit_consumed, NULL);
	settle(s, "ready");
	target_present(s, "ready");
	compare_light(s, "ready", false);
	struct block_data before = probe(s);
	require(block_at(s, EDIT_X, EDIT_Y, EDIT_Z) == 0
		&& before.type == 0 && before.sky_light == 15 && before.torch_light == 0,
		"ready", "edit and probe cells must be open, sunlit and unlit by blocks");

	put_block(s, EDIT_X, EDIT_Y, EDIT_Z, 1, 0);
	settle(s, "stone");
	target_present(s, "stone");
	struct block_data after = probe(s);
	require(after.sky_light < before.sky_light, "stone", "stone must reduce Sky below it");
	compare_light(s, "stone", false);

	put_block(s, EDIT_X, EDIT_Y, EDIT_Z, 0, 0);
	settle(s, "remove-stone");
	target_present(s, "remove-stone");
	before = probe(s);
	require(before.sky_light > after.sky_light, "remove-stone", "removal must restore Sky");
	compare_light(s, "remove-stone", false);

	/* Wall-mounted torch with a real support; no unsupported fixture block. */
	struct block_data support_before, support_after;
	require(server_world_get_block(&s->worlds[0], EDIT_X + 1, EDIT_Y,
		EDIT_Z, &support_before) && support_before.type == 0,
		"torch-support", "support cell must initially be air");
	put_block(s, EDIT_X + 1, EDIT_Y, EDIT_Z, 1, 0);
	settle(s, "torch-support");
	target_present(s, "torch-support");
	require(server_world_get_block(&s->worlds[0], EDIT_X + 1, EDIT_Y,
		EDIT_Z, &support_after) && support_after.sky_light < support_before.sky_light,
		"torch-support", "support stone must reduce Sky in its cell");
	compare_light(s, "torch-support", false);
	before = probe(s);
	put_block(s, EDIT_X, EDIT_Y, EDIT_Z, 50, 2);
	settle(s, "torch");
	target_present(s, "torch");
	after = probe(s);
	require(after.torch_light > before.torch_light, "torch", "torch must increase Block");
	compare_light(s, "torch", false);

	/* Moving three columns puts this edge column beyond the keep/support ring. */
	s->player.x += 3 * CHUNK_SIZE;
	settle(s, "move-away");
	require(target_unloads > 0 && !world_find_chunk(&gstate.world, EDIT_X, 0, EDIT_Z)
		&& !dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(TARGET_CX, 0)),
		"move-away", "target must really retire on server and client");
	compare_light(s, "move-away", false);
	printf("IN-004: target (%d,0) retired; unloads=%u\n", TARGET_CX, target_unloads);

	/* Server-side edit of the retired column, without adding a client watcher.
	 * Drain before scheduler eviction so the changed probe remains observable. */
	struct server_chunk* c = NULL;
	require(server_world_load_chunk(&s->worlds[0], TARGET_CX, 0, &c)
		&& c && !c->sent, "unwatched-edit", "reload must remain unwatched");
	before = probe(s);
	put_block(s, EDIT_X, EDIT_Y, EDIT_Z, 1, 0); /* removes torch and closes the sky */
	for(unsigned n = 0; server_world_light_pending(&s->worlds[0]); n++) {
		require(n < 4000, "unwatched-edit", "timeout draining unwatched lighting");
		server_world_light_drain(&s->worlds[0]);
	}
	after = probe(s);
	require(after.sky_light < before.sky_light && after.torch_light < before.torch_light,
		"unwatched-edit", "replacement must change both Sky and Block");
	settle(s, "unwatched-edit");
	require(!world_find_chunk(&gstate.world, EDIT_X, 0, EDIT_Z),
		"unwatched-edit", "editing must not re-admit client target");
	compare_light(s, "unwatched-edit", false);

	unsigned snapshots = target_snapshots;
	s->player.x -= 3 * CHUNK_SIZE;
	settle(s, "re-admit");
	target_present(s, "re-admit");
	require(target_snapshots > snapshots, "re-admit", "fresh CHUNK must reach client");
	struct block_data readmitted = probe(s);
	require(block_at(s, EDIT_X, EDIT_Y, EDIT_Z) == 1
		&& readmitted.sky_light == after.sky_light
		&& readmitted.torch_light == after.torch_light,
		"re-admit", "unwatched edit and its light must survive retirement");
	compare_light(s, "re-admit", false);
	printf("IN-004: target re-admitted with %u fresh snapshots\n", target_snapshots - snapshots);
	puts("IN-004 EDITS-RAN");
}

int main(void) {
	harness_init();
	struct server_local* s = split_fixture();
	/* real terrain, seed 4242 (the fixture's own world is superflat) */
	server_world_discard(&s->worlds[0]);
	string_t path;
	string_init_set_str(path, split_path);
	assert(server_world_create_with_populate_scratch(&s->worlds[0], path,
		WORLD_DIM_OVERWORLD, SEED, false, s->world_populate_scratch));
	string_clear(path);
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	b1_bind_client_world(&s->worlds[0]);
	/* the session's own stamped WORLD_RESET (server_local.c session start) */
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset.dimension = WORLD_DIM_OVERWORLD,
		.payload.world_reset.world_index = s->worlds[0].world_index,
		.payload.world_reset.world_generation = s->worlds[0].generation});
	s->paused = true; /* transport only: the load stream, no simulation */

	/* 1. the production load: stream the view square until the input-handoff
	 *    marker reaches the client */
	stream_to_marker(s);
	/* 2. let the rest of the load stream (far ring generation lighting across
	 *    into sent columns), generation light and its republish settle; still
	 *    the loading phase. Settled = no light queued, no republish owed, and
	 *    no new column admitted, for 8 consecutive updates. */
	unsigned updates = 0, quiet = 0;
	size_t resident = dict_server_chunks_size(s->worlds[0].chunks);
	while(quiet < 8) {
		step(s);
		size_t now = dict_server_chunks_size(s->worlds[0].chunks);
		quiet = server_settled(s) && now == resident ? quiet + 1 : 0;
		resident = now;
		assert(++updates < 4000 && "server load never settled");
	}
	client_pump();
	printf("IN-003: seed %d loaded; %u settle updates after the marker, %zu server columns\n",
		SEED, updates, resident);

	bool initial_red = compare_light(s, "initial", true);
	if(!initial_red)
		puts("IN-003 PASS: every client column's Sky and Block equal the server's stored light");
	edit_case(s);
	clin_test_set_consume_observer(NULL, NULL);
	teardown(s);
	if(first_red[0]) {
		puts(first_red);
		return 1;
	}
	puts("IN-004 PASS");
	return 0;
}
