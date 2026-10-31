/* Copyright (c) 2026 CaveWii_v2 contributors
 * Separate, short frozen scene. The two original walks remain untouched.
 * Pins the inherited failed-RPC sidecar rollback hole without repairing it.
 * Re-pin 2026-10-11: every changed byte is 3ae40e9d9's Beta grass overlay
 * (RenderBlocks.java:1954-1970; BlockGrass.java:12-28). Complete P + predicted
 * overlays == HEAD in both placement arms; see ATTRIBUTION-test_column_dedup_scene_equiv.md.
 * Old pin P=728a5f6f8, decoded SHA256:
 * render ba0ea057226aaebe78d0d163741178d7440da8c66f49c68641c1d6ed8db54d04;
 * moving 50408a79f02fc8526619402524102258ee7fbd1e9757af04d0be1ca94e92e7cf. */
#include "support/split_residency_fixture.h"
#include "column_observer.h"
#include "column_view.h"
#include "game/game_state.h"
#include "graphics/biome_color.h"
#include "graphics/render_piston_moving.h"
#include "item/recipe.h"
#include <zlib.h>

static gzFile moving_golden;
static bool capture;
static unsigned tinted_lists, plain_lists, moving_batches;
static void (*observe_mesh)(const struct chunk*, uint32_t,
	const struct displaylist*, const bool*, const uint8_t*, bool);

static void bytes(const void* data, size_t size) {
	if(capture) assert(gzwrite(moving_golden, data, (unsigned)size) == (int)size);
	else {
		uint8_t* expected = malloc(size ? size : 1);
		assert(expected);
		assert(gzread(moving_golden, expected, (unsigned)size) == (int)size);
		assert(!memcmp(data, expected, size));
		free(expected);
	}
}

static void number(uint32_t n) {
	uint8_t b[4] = {n, n >> 8, n >> 16, n >> 24};
	bytes(b, sizeof(b));
}

static void mesh_observer(const struct chunk* c, uint32_t generation,
	const struct displaylist* lists, const bool* has, const uint8_t* reachable,
	bool retry) {
	assert(!retry);
	for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++)
		if(has[k]) {
			if(lists[k].tinted) tinted_lists++;
			else plain_lists++;
		}
	observe_mesh(c, generation, lists, has, reachable, retry);
}

static bool get_block(void* unused, int x, int y, int z, struct block_data* out) {
	(void)unused;
	*out = world_get_block(&gstate.world, x, y, z);
	return world_find_chunk(&gstate.world, x, y, z) != NULL;
}
static bool get_moving(void* unused, int x, int y, int z,
	struct piston_moving_state* out) {
	(void)unused;
	return client_piston_moving_query(x, y, z, out);
}
static uint32_t get_tint(void* unused, int x, int z, enum biome_tint tint) {
	(void)unused;
	uint8_t ci[256], cj[256];
	assert(world_get_section_tint(&gstate.world, x, z, ci, cj));
	/* Same [x*16+z] lookup as render_piston_moving.c:render_world_tint. */
	unsigned at = W2C_COORD(x) * 16 + W2C_COORD(z);
	return tint == BIOME_TINT_GRASS ? biome_grass_color(ci[at], cj[at])
		: biome_foliage_color(ci[at], cj[at]);
}

static void moving_emit(const struct piston_moving_client_entry* entry, void* unused) {
	(void)unused;
	struct piston_moving_render_access access = {
		.get_block = get_block, .get_moving = get_moving, .get_tint = get_tint};
	number((uint32_t)entry->x); number((uint32_t)entry->y); number((uint32_t)entry->z);
	struct piston_moving_wire_state wire;
	assert(piston_moving_wire_encode(&wire, &entry->state));
	bytes(&wire, sizeof(wire));
	for(unsigned partial = 0; partial < 3; partial++) {
		struct piston_moving_render_plan plan;
		float offset[3];
		piston_moving_make_render_plan(&entry->state, partial * 0.5f, &plan);
		render_piston_moving_model_offset(entry->x, entry->y, entry->z,
			plan.translation, offset);
		number(plan.branch); number(plan.full_rod); number(plan.stationary_base);
		bytes(offset, sizeof(offset));
		struct displaylist list;
		displaylist_init(&list, 256, 32);
		displaylist_set_tinted(&list, true);
		size_t quads = render_piston_moving_test_emit(
			&list, entry, partial * 0.5f, &access, false);
		assert(quads && !list.truncated);
		number((uint32_t)quads); number((uint32_t)list.index);
		bytes(list.data, list.index); // pre-finalize PC byte count
		displaylist_destroy(&list);
		moving_batches++;
	}
}

static void freeze_scene(void) {
	for(int z = -1; z <= 1; z++)
		for(int x = -1; x <= 1; x++)
			for(int y = 48; y <= 64; y += 16) {
				struct chunk* c = world_find_chunk(&gstate.world, x * 16, y, z * 16);
				assert(c);
				struct block_data padded[5832];
				chunk_mesher_gather(c, padded);
				/* Pin every halo value, including id-36 on both sides of X/Y/Z
				 * borders, independently of the emitted display-list records. */
				for(unsigned i = 0; i < 5832; i++) {
					uint8_t b[4] = {padded[i].type, padded[i].metadata,
						padded[i].sky_light, padded[i].torch_light};
					bytes(b, sizeof(b));
				}
				assert(chunk_mesher_send(c));
				c->mesh_in_flight = true;
				ptime_t start = time_get();
				while(c->mesh_in_flight) {
					chunk_mesher_receive();
					assert(time_diff_s(start, time_get()) < 30);
					if(c->mesh_in_flight) thread_msleep(1);
				}
			}
	number((uint32_t)client_piston_moving_count());
	client_piston_moving_visit(moving_emit, NULL);
}

static void publish(struct server_local* s, int x, int z) {
	struct server_chunk* sc = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, z));
	assert(sc);
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot);
	clin_chunk_slot_copy_local(slot, sc);
	for(unsigned i = 0; i < 256; i++) {
		slot->grass_i[i] = (i * 13) & 255;
		slot->grass_j[i] = (i * 7) & 255;
	}
	assert(piston_moving_server_snapshot(s, x, z, &slot->piston_moving));
	clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK,
		.payload.chunk = {.x = x * 16, .z = z * 16, .sx = 16, .sy = 128,
			.sz = 16, .slot = slot}});
	clin_update_drain();
}

int main(int argc, char** argv) {
	capture = argc >= 2 && !strcmp(argv[1], "--capture");
	const char* prefix = argc >= 3 ? argv[2]
		: "tests/golden/column_dedup/slice2_scene_";
	char path[160];
	snprintf(path, sizeof(path), "%smoving.gz", prefix);
	moving_golden = gzopen(path, capture ? "wb1" : "rb");
	assert(moving_golden);
	blocks_init(); items_init(); recipe_init(); biome_color_init();
	assert(biome_grass_color(64, 64) != 0xFFFFFF);
	assert(biome_foliage_color(64, 64) != 0xFFFFFF);
	assert(clin_init() && svin_init() && chunk_mesher_init());
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	assert(column_observer_start(prefix, 2, !capture));
	observe_mesh = mesher_column_observe_complete;
	mesher_column_observe_complete = mesh_observer;
	struct server_local* s = split_fixture();
	for(int z = -1; z <= 1; z++)
		for(int x = -1; x <= 1; x++) {
			struct server_chunk* sc;
			assert(server_world_load_chunk(&s->world, x, z, &sc));
			memset(sc->ids, 0, 32768); memset(sc->metadata, 0, 16384);
			memset(sc->lighting_sky, 0xFF, 16384);
			memset(sc->lighting_torch, 0x21, 16384);
			memset(sc->heightmap, 66, 256);
			for(unsigned i = 0; i < 256; i++) {
				sc->ids[i * 128 + 63] = BLOCK_GRASS;
				sc->ids[i * 128 + 64] = i % 3 ? BLOCK_LEAVES : BLOCK_TALL_GRASS;
				sc->metadata[(i * 128 + 64) / 2] = i % 3;
			}
			/* Static base/head placements beside the moving border fixtures. */
			unsigned base = (4 * 16 + 4) * 128 + 64;
			unsigned head = (5 * 16 + 4) * 128 + 64;
			sc->ids[base] = BLOCK_PISTON_BASE; sc->metadata[base / 2] = 5;
			sc->ids[head] = BLOCK_PISTON_HEAD; sc->metadata[head / 2] = 5;
		}
	/* Paired border cells across X, Z and the 63/64 slice boundary. */
	const int pos[][3] = {{15,64,8}, {16,64,8}, {8,64,15}, {8,64,16},
		{0,63,0}, {-1,64,0}, {0,64,-1}, {0,64,0}};
	for(unsigned i = 0; i < sizeof(pos) / sizeof(*pos); i++) {
		int x = pos[i][0], y = pos[i][1], z = pos[i][2];
		struct server_chunk* sc = dict_server_chunks_get(s->world.chunks,
			S_CHUNK_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
		sc->ids[(W2C_COORD(x) * 16 + W2C_COORD(z)) * 128 + y] = BLOCK_PISTON_MOVING;
		struct piston_moving_state state = {.stored_id = i & 1 ? BLOCK_STONE : BLOCK_GRASS,
			.facing = i % 6, .flags = PISTON_MOVING_FLAG_EXTENDING,
			.current_progress = 0.75f, .last_progress = 0.25f};
		if(i == 6) {
			state.stored_id = BLOCK_PISTON_BASE;
			state.flags = PISTON_MOVING_FLAG_SOURCE;
		} else if(i == 7)
			state.stored_id = BLOCK_PISTON_HEAD;
		assert(piston_moving_store_upsert_owned(&s->piston_moving,
			server_world_piston_owner(&s->worlds[0]), x, y, z, &state, NULL) /* A1b */);
	}
	for(int z = -1; z <= 1; z++)
		for(int x = -1; x <= 1; x++) publish(s, x, z);
	assert(client_piston_moving_count() == 8);
	freeze_scene();
	{
		/* colcap-1006 (ASTRA-CC-R4 point 7): the display-list owner census
		 * reconciles exactly with the storage counters once every result is
		 * received -- installed == all finalized lists, nothing completed-
		 * but-unreceived, nothing GPU-pinned, every byte < 5 s old. */
		struct world_dl_census census;
		world_dl_census(&gstate.world, chunk_mesher_clock_ms(), &census);
		uint32_t lists, bytes, done_b, done_n;
		displaylist_storage_bytes(&lists, &bytes);
		chunk_mesher_completed_stats(&done_b, &done_n);
		assert(lists > 0 && census.installed_lists == lists
			   && census.installed_bytes == bytes);
		assert(done_b == 0 && done_n == 0);
		assert(census.pinned_lists == 0 && census.pinned_bytes == 0);
		assert(census.age_bytes[0] == bytes && !census.age_bytes[1]
			   && !census.age_bytes[2]);
		assert(census.own_alloc_lists <= census.installed_lists);
		printf("PASS dl owner census: %u lists / %u B installed == storage "
			   "counters; completed 0, pinned 0, own-allocation %u\n",
			   lists, bytes, census.own_alloc_lists);
	}
	assert(tinted_lists > 0 && plain_lists > 0 && moving_batches == 24);
	struct chunk* old = world_find_chunk(&gstate.world, 0, 64, 0);
	uint32_t refs = column_backing_references(old->backing);
	assert(client_piston_moving_query(15, 64, 8, NULL));
	struct server_chunk* replacement = dict_server_chunks_get(s->world.chunks,
		S_CHUNK_ID(0, 0));
	/* A published backing edit must preserve the old client image, including
	 * this deliberately failed replacement's inherited sidecar-rollback pin. */
	column_backing_write_begin(replacement->backing, 1, (1 * 16 + 1) * 128 + 65, 1);
	replacement->ids[(1 * 16 + 1) * 128 + 65] = BLOCK_GLASS;
	column_backing_write_end(replacement->backing, 1, (1 * 16 + 1) * 128 + 65, 1);
	assert(world_get_block(&gstate.world, 1, 65, 1).type == BLOCK_AIR);
	chunk_test_fail_alloc_after(4);
	publish(s, 0, 0); // FAILED REPLACEMENT through the real queued CRPC_CHUNK
	assert(world_find_chunk(&gstate.world, 0, 64, 0) == old);
	assert(column_backing_references(old->backing) == refs);
	assert(world_get_block(&gstate.world, 1, 65, 1).type == BLOCK_AIR);
	assert(world_get_block(&gstate.world, 15, 64, 8).type == BLOCK_PISTON_MOVING);
	/* Explicit inherited-bug pin: old voxels survive, their four sidecars do
	 * not. Deliberately NOT a desired-behavior assertion or a rollback fix. */
	assert(!client_piston_moving_query(15, 64, 8, NULL));
	assert(client_piston_moving_count() == 4);
	freeze_scene();
	assert(column_observer_stop());
	if(!capture) assert(gzgetc(moving_golden) == -1);
	assert(gzclose(moving_golden) == Z_OK);
	chunk_mesher_shutdown();
	clin_drain_discard(); world_destroy(&gstate.world);
	client_piston_moving_clear(); dict_entity_clear(gstate.entities);
	piston_moving_store_destroy(&s->piston_moving);
	split_destroy(s);
	uint32_t created, destroyed, references;
	column_backing_test_counts(&created, &destroyed, &references);
	assert(created == destroyed && references == 0);
	printf("PASS dedicated golden: %u tinted / %u plain lists; %u moving batches; "
		"border gathers; failed RPC preserves voxels and PINNED sidecar hole\n",
		tinted_lists, plain_lists, moving_batches);
	return 0;
}
