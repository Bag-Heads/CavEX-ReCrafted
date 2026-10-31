/* Copyright (c) 2026 CaveWii_v2 contributors
 * Additional state equivalence without changing a single frozen walk byte.
 * Replay production RPC prefixes first through the original OWNED installer,
 * then through the differential installer. Compare serialized values directly.
 */
#include "split_residency_fixture.h"
#include "column_view.h"
#include "game/game_state.h"
#include "block/redstone_piston_server.h"
#include "network/piston_moving_transport.h"
#include "lighting.h"
#include "particle.h"

static FILE* reference;
static bool owned_install, recording, tracing;
static unsigned checkpoints;

bool __real_world_build_full_section_leased(struct world*, w_coord_t, w_coord_t,
	const uint8_t*, const uint8_t*, const uint8_t*, const uint8_t*,
	const uint8_t*, const uint8_t*, const uint8_t*, struct column_backing*);
bool __wrap_world_build_full_section_leased(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* meta, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* height, const uint8_t* gi, const uint8_t* gj, struct column_backing* b) {
	return owned_install ? world_build_full_section(w, x, z, ids, meta, sky, torch, height, gi, gj)
		: __real_world_build_full_section_leased(w, x, z, ids, meta, sky, torch, height, gi, gj, b);
}

static void bytes(const void* data, size_t size) {
	if(recording) assert(fwrite(data, 1, size, reference) == size);
	else {
		uint8_t expected[16384];
		assert(size <= sizeof(expected));
		assert(fread(expected, 1, size, reference) == size);
		if(memcmp(expected, data, size)) {
			fprintf(stderr, "prefix checkpoint %u differs (%zu bytes)\n", checkpoints, size);
			abort();
		}
	}
}
#define VALUE(x) do { __typeof__(x) value = (x); bytes(&value, sizeof(value)); } while(0)

static void moving(const struct piston_moving_client_entry* entry, void* user) {
	(void)user;
	VALUE(entry->x); VALUE(entry->y); VALUE(entry->z);
	struct piston_moving_wire_state wire;
	assert(piston_moving_wire_encode(&wire, &entry->state));
	bytes(&wire, sizeof(wire));
}
static void checkpoint(unsigned event) {
	checkpoints++;
	VALUE(event);
	for(int z = 0; z <= 0; z++)
		for(int x = 0; x <= 2; x++) {
			struct world_section* s = dict_wsection_get(gstate.world.sections, SECTION_TO_ID(x, z));
			VALUE(s != NULL);
			if(!s) continue;
			VALUE(s->is_far_lod);
			bytes(s->heightmap, 256); bytes(s->grass_i, 256); bytes(s->grass_j, 256);
			for(unsigned cy = 0; cy < 8; cy++) {
				struct chunk* c = s->column[cy];
				VALUE(c != NULL);
				if(!c) continue;
				VALUE(c->dirty_generation); VALUE(c->rebuild_displist); VALUE(c->mesh_urgent);
				uint8_t cells[16384];
				unsigned i = 0;
				for(unsigned y = 0; y < 16; y++)
					for(unsigned lz = 0; lz < 16; lz++)
						for(unsigned lx = 0; lx < 16; lx++) {
							struct block_data b = chunk_get_block(c, lx, y, lz);
							cells[i++] = b.type; cells[i++] = b.metadata;
							cells[i++] = b.sky_light; cells[i++] = b.torch_light;
						}
				bytes(cells, sizeof(cells));
			}
		}
	VALUE(gstate.world.lighting_updates.index);
	for(size_t i = 0; i < gstate.world.lighting_updates.index; i++) {
		struct world_modification_entry e;
		stack_at(&gstate.world.lighting_updates, &e, i);
		VALUE(e.x); VALUE(e.y); VALUE(e.z);
		uint8_t b[] = {e.blk.type, e.blk.metadata, e.blk.sky_light, e.blk.torch_light};
		bytes(b, sizeof(b));
	}
	/* lighting_update_entry is private; serialize its declared coordinates
	 * and forced byte, excluding the C struct's trailing padding. */
	struct stack* queue = &gstate.world.lighting_flood_queue;
	VALUE(queue->index);
	assert(queue->element_size >= 3 * sizeof(w_coord_t) + sizeof(bool));
	for(size_t i = 0; i < queue->index; i++)
		bytes((uint8_t*)queue->data + i * queue->element_size, 3 * sizeof(w_coord_t) + sizeof(bool));
	VALUE(client_piston_moving_count());
	client_piston_moving_visit(moving, NULL);
	VALUE(particle_test_count());
	for(size_t i = 0; i < particle_test_count(); i++) {
		struct particle p;
		assert(particle_test_get(i, &p));
		bytes(p.pos, sizeof(p.pos)); bytes(p.pos_old, sizeof(p.pos_old));
		bytes(p.vel, sizeof(p.vel)); bytes(p.tex_uv, sizeof(p.tex_uv));
		VALUE(p.size); VALUE(p.age); VALUE(p.lifetime); VALUE(p.tex);
		VALUE(p.gravity); VALUE(p.r); VALUE(p.g); VALUE(p.b); VALUE(p.ignore_light); VALUE(p.atlas);
	}
}
static void consume(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(tracing) checkpoint(rpc->type);
}
static void pump(const struct client_rpc* rpc, void* unused) {
	(void)rpc; (void)unused;
	clin_update_drain();
}
static void send_column(struct server_local* s, int x) {
	struct server_chunk* sc = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, 0));
	assert(sc);
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot);
	clin_chunk_slot_copy_local(slot, sc);
	memset(slot->grass_i, 0, 256); memset(slot->grass_j, 0, 256);
	assert(piston_moving_server_snapshot(s, x, 0, &slot->piston_moving));
	clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK, .payload.chunk = {
		.x = x * 16, .sx = 16, .sy = 128, .sz = 16, .slot = slot}});
}
static void set(struct server_local* s, int x, int y, int z, uint8_t id, uint8_t meta) {
	assert(server_world_set_block(&s->worlds[0], x, y, z, (struct block_data){.type = id, .metadata = meta}));
}
static void run(bool owned) {
	owned_install = recording = owned;
	tracing = false;
	checkpoints = 0;
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD}});
	gstate.rand_src.seed = 4321;
	particle_clear_all();
	particle_set_camera((vec3){14, 65, 8});
	clin_update(); // real frame boundary resets the dig-burst budget for both runs
	world_lighting_driver_diag_reset();
	struct server_local* s = split_fixture();
	s->rand_src.seed = 12345;
	server_local_test_seed_math_random(9876);
	clin_test_set_rpc_observer(pump, NULL);
	clin_test_set_consume_observer(consume, NULL);
	for(int z = -1; z <= 1; z++)
		for(int x = -1; x <= 2; x++) {
			struct server_chunk* sc;
			server_world_begin_update(&s->world);
			assert(server_world_load_chunk(&s->world, x, z, &sc));
			column_backing_write_begin(sc->backing, 12, 0, 32768);
			memset(sc->lighting_sky, 0, 16384); memset(sc->lighting_torch, 0, 16384);
			column_backing_write_end(sc->backing, 12, 0, 32768);
		}
	send_column(s, 0); send_column(s, 1);
	uint8_t skin[256], ids[256], tint[256] = {0};
	memset(skin, 40, 256); memset(ids, BLOCK_GRASS, 256);
	world_set_far_storage(WORLD_FAR_STORAGE_SKIN);
	assert(world_build_far_section(&gstate.world, 32, 0, skin, ids, tint, tint));
	tracing = true;
	checkpoint(10000);
	/* Actual 4096-pop frame followed by replacement while its frontier lives. */
	world_set_block(&gstate.world, 8, 64, 8, (struct block_data){0}, true);
	world_update_lighting(&gstate.world);
	assert(world_lighting_driver_last_work() == 4096);
	assert(!lighting_flood_queue_empty(&gstate.world.lighting_flood_queue));
	checkpoint(10001);
	send_column(s, 0);
	for(unsigned frame = 0; !stack_empty(&gstate.world.lighting_updates)
		|| !lighting_flood_queue_empty(&gstate.world.lighting_flood_queue); frame++) {
		assert(frame < 256);
		world_update_lighting(&gstate.world);
		assert(world_lighting_driver_last_work() <= 4096);
		checkpoint(10002);
	}
	/* The client flood never touched the server's zero sky or the far skin. */
	for(int x = 0; x < 2; x++) {
		struct server_chunk* sc = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, 0));
		for(unsigned i = 0; i < 16384; i++) assert(sc->lighting_sky[i] == 0);
	}
	assert(world_get_block(&gstate.world, 32, 41, 0).sky_light == 15);

	/* Quiet metadata produces no client advance; the later ordinary notify
	 * consumes the identical nibble through the real inbox. */
	assert(server_world_set_block_metadata_quiet(&s->worlds[0], 15, 64, 8, 7));
	checkpoint(10003);
	assert(server_world_set_block_metadata_with_notify(&s->worlds[0], 15, 64, 8, 7));
	/* Rectangular RPC straddles both the X column and Y slice boundary. */
	struct chunk_rpc_slot* rect = clin_chunk_slot_try_acquire();
	assert(rect);
	memset(rect->ids, BLOCK_GLASS, 6); memset(rect->metadata, 0x21, 3);
	memset(rect->lighting_sky, 0x43, 3); memset(rect->lighting_torch, 0x65, 3);
	clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK, .payload.chunk = {
		.x = 15, .y = 63, .z = 8, .sx = 2, .sy = 3, .sz = 1, .slot = rect}});
	/* Real piston neighbour cascade crosses X=15/16, with ID-36 sidecars,
	 * two settle ticks and a retract. Every individual emitted RPC is read. */
	for(int x = 14; x <= 18; x++) for(int z = 7; z <= 9; z++) set(s, x, 63, z, BLOCK_STONE, 0);
	set(s, 14, 64, 8, BLOCK_PISTON_BASE, 5);
	set(s, 15, 64, 8, BLOCK_STONE, 0); set(s, 16, 64, 8, BLOCK_STONE, 0);
	set(s, 14, 64, 7, BLOCK_REDSTONE_TORCH_LIT, 5);
	assert(client_piston_moving_count() > 0);
	for(unsigned tick = 0; tick < 2; tick++) { piston_moving_te_tick(s); checkpoint(10004); }
	set(s, 14, 64, 7, BLOCK_AIR, 0);
	for(unsigned tick = 0; tick < 2; tick++) { piston_moving_te_tick(s); checkpoint(10005); }
	assert(particle_test_count() > 0);
	/* The pre-enqueue pump deliberately leaves the last message pending.
	 * Consume and observe it too; discard is not a consumption checkpoint. */
	unsigned before_final = checkpoints;
	clin_update_drain();
	assert(checkpoints == before_final + 1);
	tracing = false;
	clin_test_set_rpc_observer(NULL, NULL);
	clin_test_set_consume_observer(NULL, NULL);
	world_destroy(&gstate.world); split_destroy(s);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player); gstate.players[0].local_player = NULL;
	world_set_far_storage(WORLD_FAR_STORAGE_VOXEL);
	owned_install = false;
}
void column_dedup_prefix_equiv(void) {
	reference = tmpfile();
	assert(reference);
	run(true);
	unsigned expected = checkpoints;
	rewind(reference);
	run(false);
	assert(checkpoints == expected && fgetc(reference) == EOF);
	fclose(reference);
	printf("PASS %u owned/shared prefixes: every cell/height/tint/dirty flag, ordered relight frontier, piston sidecar and particle value; mid-flood replacement + far seam + rectangular RPC + quiet metadata + piston cascade\n", checkpoints);
}
