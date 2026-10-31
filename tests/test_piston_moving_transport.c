#include "network/piston_moving_transport.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct server_local server_fixture;

#define HOST_INDEX(X, Y, Z) \
	((size_t)(Y) + ((size_t)(Z) + (size_t)(X) * CHUNK_SIZE) * WORLD_HEIGHT)

static struct piston_moving_state state_for(uint8_t id, uint8_t meta,
										 uint8_t facing, uint8_t flags,
										 float current, float last) {
	return (struct piston_moving_state) {
		.stored_id = id,
		.stored_meta = meta,
		.facing = facing,
		.flags = flags,
		.current_progress = current,
		.last_progress = last,
	};
}

static void assert_same_bits(float a, float b) {
	uint32_t aa, bb;
	memcpy(&aa, &a, sizeof(aa));
	memcpy(&bb, &b, sizeof(bb));
	assert(aa == bb);
}

int main(void) {
	assert(sizeof(struct piston_moving_wire_state) == 11);
	assert(sizeof(struct piston_moving_chunk_record) == 13);

	struct piston_moving_state original = state_for(
		35, 13, 5, PISTON_MOVING_FLAG_EXTENDING, 0.75F, 0.25F);
	struct piston_moving_wire_state wire;
	struct piston_moving_state decoded;
	assert(piston_moving_wire_encode(&wire, &original));
	assert(piston_moving_wire_decode(&wire, &decoded));
	assert(decoded.stored_id == original.stored_id);
	assert(decoded.stored_meta == original.stored_meta);
	assert(decoded.facing == original.facing);
	assert(decoded.flags == original.flags);
	assert_same_bits(decoded.current_progress, original.current_progress);
	assert_same_bits(decoded.last_progress, original.last_progress);
	struct piston_moving_wire_state invalid = wire;
	invalid.facing_flags |= 0x80;
	assert(!piston_moving_wire_decode(&invalid, &decoded));
	invalid = wire;
	uint32_t quiet_nan = UINT32_C(0x7fc00000);
	memcpy(invalid.current_progress_bits, &quiet_nan, sizeof(quiet_nan));
	assert(!piston_moving_wire_decode(&invalid, &decoded));

	struct piston_moving_chunk_record record;
	assert(piston_moving_record_encode(
		&record, -32, 16, -17, 127, 31, &original));
	w_coord_t x, y, z;
	assert(piston_moving_record_decode(
		&record, -32, 16, &x, &y, &z, &decoded));
	assert(x == -17 && y == 127 && z == 31);
	assert_same_bits(decoded.current_progress, original.current_progress);

	client_piston_moving_clear();
	struct piston_moving_state start = state_for(
		35, 4, 5, PISTON_MOVING_FLAG_EXTENDING, 0.0F, 0.0F);
	assert(client_piston_moving_upsert(5, 40, 5, &start));
	client_piston_moving_tick();
	assert(client_piston_moving_query(5, 40, 5, &decoded));
	assert(decoded.current_progress == 0.5F && decoded.last_progress == 0.0F);
	client_piston_moving_tick();
	client_piston_moving_tick();
	assert(client_piston_moving_query(5, 40, 5, &decoded));
	assert(decoded.current_progress == 1.0F && decoded.last_progress == 1.0F);
	/* Completion stays owned until the ordinary materialization overwrite. */
	assert(client_piston_moving_count() == 1);
	assert(client_piston_moving_remove(5, 40, 5));

	client_piston_moving_clear();
	/* Regression: the former 64-entry client array dropped the 65th legal
	 * moving block. Grow well beyond both that cap and an 8-bit chunk count. */
	const int dynamic_client_count = 257;
	for(int i = 0; i < dynamic_client_count; i++)
		assert(client_piston_moving_upsert(i, 40, 0, &original));
	assert(client_piston_moving_count() == (size_t)dynamic_client_count);
	struct piston_moving_state replacement = original;
	replacement.current_progress = 1.0F;
	assert(client_piston_moving_upsert(0, 40, 0, &replacement));
	assert(client_piston_moving_query(0, 40, 0, &decoded));
	assert(decoded.current_progress == 1.0F);
	client_piston_moving_remove_chunk(0, 0);
	assert(client_piston_moving_count() == 241);
	client_piston_moving_clear();

	memset(&server_fixture, 0, sizeof(server_fixture));
	piston_moving_store_init(&server_fixture.piston_moving);
	dict_server_chunks_init(server_fixture.world.chunks);
	/* A1b: the snapshot streams only the sent world's owned entries; give
	 * slot 0 a live instance identity and stamp the entries with it. */
	server_fixture.worlds[0].generation = 1;
	static uint8_t host_ids[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT];
	memset(host_ids, 0, sizeof(host_ids));
	struct server_chunk host_chunk = {.ids = host_ids};
	dict_server_chunks_set_at(
		server_fixture.world.chunks, S_CHUNK_ID(0, 0), host_chunk);
	const int slots[3] = {9, 2, 41};
	const uint64_t order[3] = {30, 10, 20};
	const int px[3] = {3, 1, 2};
	for(int i = 0; i < 3; i++) {
		(void)slots[i]; /* retained to document the former sparse-slot ordering case */
		struct piston_moving_state server_state = state_for(
			(uint8_t)(20 + px[i]), (uint8_t)px[i], 5,
			PISTON_MOVING_FLAG_EXTENDING, 0.5F, 0.0F);
		size_t slot;
		assert(piston_moving_store_upsert_owned(&server_fixture.piston_moving,
			server_world_piston_owner(&server_fixture.worlds[0]),
			px[i], 64, 7, &server_state, &slot));
		/* Deliberately disagree with backing-slot order; snapshot order is the
		 * monotonic loaded TileEntity order, not allocator placement. */
		server_fixture.piston_moving.entries[slot].order = order[i];
		host_ids[HOST_INDEX(px[i], 64, 7)]
			= px[i] == 3 ? BLOCK_STONE : BLOCK_PISTON_MOVING;
	}
	server_fixture.piston_moving.next_order = 30;
	struct piston_moving_chunk_snapshot snapshot = {0};
	assert(piston_moving_server_snapshot(&server_fixture, 0, 0, &snapshot));
	/* The x=3 slot is stale: its host voxel is stone, not id36. Never stream a
	 * phantom render/collision owner for it. */
	assert(snapshot.count == 2);
	const int expected_x[2] = {1, 2};
	for(int i = 0; i < 2; i++) {
		assert(piston_moving_record_decode(
			&snapshot.records[i], 0, 0, &x, &y, &z, &decoded));
		assert(x == expected_x[i] && y == 64 && z == 7);
	}
	assert(client_piston_moving_replace_chunk(
		0, 0, snapshot.records, snapshot.count));
	assert(client_piston_moving_count() == 2);
	assert(client_piston_moving_query(2, 64, 7, &decoded));
	assert(decoded.stored_meta == 2);

	/* Invalid chunk replacement never leaves a partially applied sidecar set. */
	assert(client_piston_moving_upsert(32, 64, 0, &original));
	struct piston_moving_chunk_record bad_records[2]
		= {snapshot.records[0], snapshot.records[1]};
	bad_records[1].state.facing_flags |= 0x80;
	assert(!client_piston_moving_replace_chunk(0, 0, bad_records, 2));
	assert(client_piston_moving_count() == 3);
	assert(client_piston_moving_query(32, 64, 0, &decoded));
	assert(client_piston_moving_query(1, 64, 7, NULL));
	assert(client_piston_moving_query(2, 64, 7, NULL));
	/* Duplicate coordinates are also rejected atomically. */
	struct piston_moving_chunk_record duplicate_records[2]
		= {snapshot.records[0], snapshot.records[0]};
	assert(!client_piston_moving_replace_chunk(
		0, 0, duplicate_records, 2));
	assert(client_piston_moving_query(1, 64, 7, NULL));

	/* A single chunk snapshot and replacement may exceed UINT8_MAX records. */
	piston_moving_store_clear(&server_fixture.piston_moving);
	memset(host_ids, 0, sizeof(host_ids));
	for(int i = 0; i < 300; i++) {
		int px_stress = i & 15;
		int pz_stress = (i >> 4) & 15;
		int py_stress = 1 + (i >> 8);
		struct piston_moving_state stress = state_for(
			BLOCK_STONE, (uint8_t)(i & 15), 5,
			PISTON_MOVING_FLAG_EXTENDING, 0.5F, 0.0F);
		assert(piston_moving_store_upsert_owned(&server_fixture.piston_moving,
			server_world_piston_owner(&server_fixture.worlds[0]),
			px_stress, py_stress, pz_stress, &stress, NULL));
		host_ids[HOST_INDEX(px_stress, py_stress, pz_stress)]
			= BLOCK_PISTON_MOVING;
	}
	assert(piston_moving_server_snapshot(&server_fixture, 0, 0, &snapshot));
	assert(snapshot.count == 300);
	client_piston_moving_clear();
	assert(client_piston_moving_replace_chunk(
		0, 0, snapshot.records, snapshot.count));
	assert(client_piston_moving_count() == 300);

	piston_moving_chunk_snapshot_destroy(&snapshot);
	piston_moving_store_destroy(&server_fixture.piston_moving);
	dict_server_chunks_clear(server_fixture.world.chunks);

	puts("piston moving transport golden: OK");
	return 0;
}
