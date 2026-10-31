/* Frozen from 2f18d992b BEFORE production edits: ordered writes/dirty events,
 * frame budgets/frontier lengths, final bytes and dirty/mesh state. Real client
 * copied/leased storage, negative seams, Y seams, sky/torch and mid-flood eviction.
 * RE-RECORDED for LIGHT-CLIENT-LOSSLESS (2026-10-23): the 2f18d992b tape froze
 * the 500k-pop frontier ABANDON (mode 0 final bytes lacked torch light) and
 * relit cells beside an unloaded column as if it were dark. Every other record
 * kind and the frame budget bound are unchanged.
 * RE-RECORDED again for fix-r2 (2026-10-23): the parked frontier is now spilled
 * a charged slice per frame instead of in one uncharged go, so mode 0 runs 228
 * frames instead of 155. Ordered writes, dirty events and final bytes of all
 * four modes are identical to the previous tape (only frame records differ). */
#include <zlib.h>
#include "column_view.h"
#include "network/server_world.h"
#ifdef _WIN32
#include <windows.h>
#endif
static gzFile client_tape;
static bool client_record;
static bool client_private;
static unsigned long client_bytes;
static void client_emit(unsigned v) {
	if(!client_tape)
		return;
	for(unsigned k = 0; k < 4; k++) {
		unsigned b = (v >> (8 * k)) & 255;
		if(client_record)
			assert(gzputc(client_tape, b) != EOF);
		else {
			int expected = gzgetc(client_tape);
			if(expected != (int)b) {
				fprintf(stderr, "client golden mismatch byte %lu: %d != %u\n",
					client_bytes, expected, b);
				abort();
			}
		}
		client_bytes++;
	}
}
void __real_chunk_set_light(struct chunk*, c_coord_t, c_coord_t, c_coord_t, uint8_t);
void __wrap_chunk_set_light(struct chunk* c, c_coord_t x, c_coord_t y,
	c_coord_t z, uint8_t light) {
	client_emit(1);
	client_emit(c->x+x);
	client_emit(c->y+y);
	client_emit(c->z+z);
	client_emit(light);
	__real_chunk_set_light(c, x, y, z, light);
}
static void client_dirty(const struct settle_input* in) {
	if(!in->chunk)
		return;
	client_emit(2);
	client_emit(in->stage);
	client_emit(in->chunk->x);
	client_emit(in->chunk->y);
	client_emit(in->chunk->z);
	client_emit(in->chunk->dirty_generation);
}
static uint64_t client_cpu(void) {
#ifdef _WIN32
	ULONG64 cycles;
	assert(QueryThreadCycleTime(GetCurrentThread(), &cycles));
	return cycles;
#else
	struct timespec ts;
	assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0);
	return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;
#endif
}
static uint64_t client_fixture(unsigned mode) {
	struct world w;
	world_create(&w);
	w.dimension=mode&1 ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	struct server_world server;
	bool shared=mode>=2;
	struct column_view_stats before;
	column_view_get_stats(&before);
	if(shared) {
		string_t path;
		string_init_set_str(path, "tmp/r16_client_no_save");
		server_world_create(&server, path, w.dimension, 12345, true);
		string_clear(path);
		assert(server.generator);
	}
	uint8_t* image = calloc(81920, 1);
	assert(image);
	uint8_t height[256], tint[256]={0};
	memset(height, mode==0?0:128, sizeof(height));
	for(int z = -16; z <= 0; z += 16) for(int x = -16; x <= 0; x += 16) {
		if(shared) {
			struct server_chunk* sc;
			server_world_begin_update(&server);
			assert(server_world_load_chunk(&server, x/16, z/16, &sc));
			memcpy(sc->ids, image, 32768);
			memcpy(sc->metadata, image+32768, 16384);
			memcpy(sc->lighting_sky, image+49152, 16384);
			memcpy(sc->lighting_torch, image+65536, 16384);
			memcpy(sc->heightmap, height, 256);
			assert(world_build_full_section_leased(&w, x, z, sc->ids, sc->metadata,
				sc->lighting_sky, sc->lighting_torch, height, tint, tint, sc->backing));
		} else world_build_full_section(&w, x, z, image, image+32768, image+49152, image+65536, height, tint, tint);
	}
	free(image);
	if(client_private && shared) column_view_test_page_limit(0);
	client_emit(3);
	client_emit(mode);
	world_set_block(&w, -1, 31, -1, (struct block_data){.type=BLOCK_TORCH}, true);
	world_set_block(&w, 0, 32, 0, (struct block_data){.type=BLOCK_GLOWSTONE}, true);
	struct world_modification_entry stale={.x=4096, .y=64, .z=4096};
	stack_push(&w.lighting_updates, &stale);
	unsigned frames=0;
	uint64_t cycles=0;
	chunk_settle_observe=client_tape?client_dirty:NULL;
	do {
		uint64_t start=client_cpu();
		world_update_lighting(&w);
		cycles+=client_cpu()-start;
		client_emit(4);
		client_emit(world_lighting_driver_last_work());
		client_emit(world_lighting_driver_last_stale_drops());
		client_emit(stack_size(&w.lighting_flood_queue));
		client_emit(stack_size(&w.lighting_updates));
		assert(world_lighting_driver_last_work()<=LIGHTING_CLIENT_POPS_PER_FRAME);
		if(mode==3 && frames==0) world_unload_section(&w, -1, -1);
		for(int z = -16; z <= 0; z += 16) for(int x = -16; x <= 0; x += 16) for(int y = 0; y < 128; y += 16) {
			struct chunk* c=world_find_chunk(&w, x, y, z);
			client_emit(c!=NULL);
			if(c) {
				client_emit(c->dirty_generation);
				client_emit(c->rebuild_displist);
				client_emit(c->mesh_in_flight);
				client_emit(c->mesh_urgent);
			}
		}
		assert(++frames<512);
	/* LIGHT-CLIENT-LOSSLESS: a parked flood finishes in the column repair, so the
	 * fixture runs until that is empty too (the pre-fix tape froze the 500k
	 * ABANDON of modes 0 and 2: torch light missing in the final bytes). */
	} while(!stack_empty(&w.lighting_updates)||!lighting_flood_queue_empty(&w.lighting_flood_queue)
		|| world_light_repair_pending(&w));
	chunk_settle_observe=NULL;
	for(int z = -16; z < 16; z++) for(int x = -16; x < 16; x++) for(int y = 0; y < 128; y++) {
		struct block_data b=world_get_block(&w, x, y, z);
		client_emit(b.type|(b.metadata<<8)|(b.sky_light<<16)|(b.torch_light<<24));
	}
	if(client_private && shared) {
		struct column_view_stats after;
		column_view_get_stats(&after);
		assert(after.fallback_total > before.fallback_total);
	}
	world_destroy(&w);
	if(shared)
		server_world_discard(&server);
	if(client_private && shared) column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	printf("client fixture %u: %u frames, %llu CPU units\n", mode, frames,
		(unsigned long long)cycles);
	return cycles;
}
static void client_golden(const char* path, bool record) {
	client_record = record;
	client_bytes = 0;
	client_tape = gzopen(path, record ? "wb" : "rb");
	assert(client_tape);
	for(unsigned mode=0;mode<4;mode++) client_fixture(mode);
	if(!record) assert(gzgetc(client_tape)==EOF);
	assert(gzclose(client_tape) == Z_OK);
	client_tape = NULL;
	printf("frozen client tape: %lu bytes %s\n", client_bytes, record?"RECORDED":"PASS");
}
