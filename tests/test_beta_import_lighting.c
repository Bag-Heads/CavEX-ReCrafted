/* L2: actual original-JAR McRegion save; never fabricated lighting.
 * Beta ChunkLoader.loadChunkIntoWorldFromCompound restores SkyLight,
 * BlockLight and HeightMap. Chunk.setBlockID:242-276 may write Nether Sky;
 * World.scheduleLightingUpdate:1300 suppresses its queue, not stored bytes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "item/items.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/server_interface.h"
#include "network/level_archive.h"

#ifdef _WIN32
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif
#define NIB 16384
#define IMAGE (2*NIB + 256)
static unsigned failures;
static struct server_local session;
static unsigned char stored[32][IMAGE], east[40][IMAGE];

static void read_bytes(const char* path, void* data, size_t size) {
	FILE* f = fopen(path, "rb");
	assert(f && fread(data, 1, size, f) == size && fgetc(f) == EOF);
	fclose(f);
}
static void copy_file(const char* src, const char* dst) {
	FILE* a = fopen(src, "rb"), *b = fopen(dst, "wb");
	assert(a && b);
	unsigned char data[4096];
	size_t n;
	while((n = fread(data, 1, sizeof data, a))) assert(fwrite(data, 1, n, b) == n);
	assert(!ferror(a));
	fclose(a); assert(fclose(b) == 0);
}
static size_t differences(const unsigned char* a, const unsigned char* b, size_t n) {
	size_t count = 0;
	for(size_t i = 0; i < n; i++) count += a[i] != b[i];
	return count;
}
static void compare_stored(int d, const char* phase) {
	size_t sky = 0, block = 0, height = 0;
	for(int x = 0; x < 4; x++) for(int z = 0; z < 4; z++) {
		struct server_chunk* c = dict_server_chunks_get(session.worlds[d].chunks, S_CHUNK_ID(x,z));
		assert(c);
		unsigned char* expected = stored[d*16 + x*4 + z];
		sky += differences(c->lighting_sky, expected, NIB);
		block += differences(c->lighting_torch, expected+NIB, NIB);
		height += differences(c->heightmap, expected+2*NIB, 256);
	}
	printf("%s dim=%d stored byte differences: Sky=%zu Block=%zu height=%zu\n",
		phase, d ? -1 : 0, sky, block, height);
	failures += sky != 0 || block != 0 || height != 0;
}
static unsigned nibble(const unsigned char* p, size_t i) {
	return (p[i/2] >> ((i&1)*4)) & 15;
}
static void client_check(void) {
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	assert(clin_world_payloads_set_enabled(true));
	for(unsigned d = 0; d < 2; d++) {
		struct server_world* w = &session.worlds[d];
		clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET,
			.payload.world_reset={.dimension=w->dimension, .world_index=d,
				.world_generation=w->generation}});
		assert(clin_world_payload_bind_view(d, d));
		struct clin_world_payload payload;
		assert(clin_world_payload_of_slot(d, &payload));
		size_t delivery = 0, file = 0;
		for(int x=0;x<4;x++) for(int z=0;z<4;z++) {
			struct server_chunk* c = dict_server_chunks_get(w->chunks,S_CHUNK_ID(x,z));
			struct chunk_rpc_slot* slot = clin_chunk_slot_acquire(); assert(slot);
			clin_chunk_slot_copy_local(slot,c);
			memset(slot->grass_i,0,sizeof slot->grass_i);
			memset(slot->grass_j,0,sizeof slot->grass_j);
			slot->piston_moving.count=0;
			struct client_rpc call={.type=CRPC_CHUNK,
				.payload.chunk={.x=x*16,.z=z*16,.sx=16,.sy=128,.sz=16,.slot=slot}};
			clin_rpc_stamp(&call,d,w->generation);
			clin_process(&call);
			assert(world_find_chunk(payload.terrain,x*16,0,z*16));
			for(int lx=0;lx<16;lx++) for(int lz=0;lz<16;lz++) for(int y=0;y<128;y++) {
				size_t i=(lx*16+lz)*128+y;
				struct block_data b=world_get_block(payload.terrain,x*16+lx,y,z*16+lz);
				unsigned char* e=stored[d*16+x*4+z];
				delivery += b.sky_light != nibble(c->lighting_sky,i) || b.torch_light != nibble(c->lighting_torch,i);
				file += b.sky_light != nibble(e,i) || b.torch_light != nibble(e+NIB,i);
			}
		}
		printf("split client slot=%u: server delivery cell differences=%zu, saved-file differences=%zu\n",d,delivery,file);
		failures += delivery != 0 || file != 0;
	}
	/* Context retirement owns the secondary payload and column-view leases. */
	client_ctx_registry_retire(clin_world_contexts(),0);
	client_ctx_registry_retire(clin_world_contexts(),1);
	assert(clin_world_payloads_set_enabled(false));
	world_destroy(&gstate.world);
	for(unsigned d=0;d<2;d++) {
		free(gstate.players[d].local_player); gstate.players[d].local_player=NULL;
	}
	dict_entity_clear(gstate.entities);
}
struct seam_result {unsigned max; size_t cells;};
static struct seam_result seam_delta(struct server_world* w, int d, const char* name) {
	unsigned max=0;
	size_t cells=0;
	for(int z=0;z<4;z++) for(int x=3;x<=4;x++) {
		struct server_chunk* c=dict_server_chunks_get(w->chunks,S_CHUNK_ID(x,z)); assert(c);
		unsigned char* expected=east[d*20+x*4+z];
		int lx=x==3 ? 15 : 0;
		for(int lz=0;lz<16;lz++) for(int y=0;y<128;y++) {
			size_t i=(lx*16+lz)*128+y;
			for(int channel=0;channel<2;channel++) {
				int actual=nibble(channel ? c->lighting_torch : c->lighting_sky,i);
				int target=nibble(expected+channel*NIB,i);
				unsigned delta=(unsigned)abs(actual-target);
				if(delta>max) max=delta;
				cells += delta != 0;
			}
		}
	}
	printf("%s dim=%d east seam vs original JAR: max light delta=%u mismatching channel-cells=%zu\n",name,d ? -1:0,max,cells);
	return (struct seam_result){max,cells};
}
int main(int argc,char** argv) {
	assert(argc==2);
	setvbuf(stdout,NULL,_IONBF,0);
	blocks_init(); items_init(); assert(svin_init()); assert(clin_init());
	char path[1024],dir[128],dst[1024];
	snprintf(path,sizeof path,"%s/stored-light.bin",argv[1]); read_bytes(path,stored,sizeof stored);
	snprintf(path,sizeof path,"%s/east-light.bin",argv[1]); read_bytes(path,east,sizeof east);
	test_fixture_dir("test_beta_import_lighting",dir,sizeof dir); MKDIR(dir);
	snprintf(dst,sizeof dst,"%s/region",dir); MKDIR(dst);
	snprintf(dst,sizeof dst,"%s/DIM-1",dir); MKDIR(dst);
	snprintf(dst,sizeof dst,"%s/DIM-1/region",dir); MKDIR(dst);
	for(int d=0;d<2;d++) {
		snprintf(path,sizeof path,"%s/world/%sregion/r.0.0.mcr",argv[1],d ? "DIM-1/":"");
		snprintf(dst,sizeof dst,"%s/%sregion/r.0.0.mcr",dir,d ? "DIM-1/":""); copy_file(path,dst);
	}
	snprintf(path,sizeof path,"%s/world/level.dat",argv[1]);
	snprintf(dst,sizeof dst,"%s/level.dat",dir); copy_file(path,dst);
	dict_entity_init(session.entities);
	session.scheduled_tick_budget=128;
	string_t level; string_init_set_str(level,dir);
	struct level_archive archive;
	int64_t seed=0;
	assert(level_archive_create(&archive,level));
	assert(level_archive_read(&archive,LEVEL_RANDOM_SEED,&seed,0)&&seed==4242);
	level_archive_discard(&archive);
	for(unsigned d=0;d<2;d++) {
		struct server_world* w=&session.worlds[d];
		server_world_create(w,level,d ? WORLD_DIM_NETHER:WORLD_DIM_OVERWORLD,seed,false);
		w->session=&session; w->world_index=d;
		for(int x=0;x<4;x++) for(int z=0;z<4;z++) {
			struct server_chunk* c=NULL; assert(server_world_load_chunk(w,x,z,&c)&&c);
		}
		compare_stored(d,"after real disk load");
		for(int tick=0;tick<8;tick++) {
			(void)server_local_world_block_ticks_w(&session,w,false,0,0);
			assert(server_world_light_drain(w));
		}
		compare_stored(d,"after 8 unanchored server block ticks (no edits)");
	}
	client_check();
	for(int d=0;d<2;d++) {
		struct server_world* w=&session.worlds[d];
		struct server_world pure={0};
		char pure_dir[128]; test_fixture_dir(d ? "beta_pure_ne":"beta_pure_ow",pure_dir,sizeof pure_dir); MKDIR(pure_dir);
		string_t p; string_init_set_str(p,pure_dir);
		server_world_create(&pure,p,w->dimension,4242,false); string_clear(p);
		for(int x=0;x<5;x++) for(int z=0;z<4;z++) {
			struct server_chunk* c=NULL; assert(server_world_load_chunk(&pure,x,z,&c)&&c);
			if(x==4) {c=NULL; assert(server_world_load_chunk(w,x,z,&c)&&c);}
		}
		assert(server_world_light_drain(w)&&server_world_light_drain(&pure));
		struct seam_result imported=seam_delta(w,d,"imported/new"),baseline=seam_delta(&pure,d,"pure CaveWii");
		failures += imported.max>baseline.max || imported.cells>baseline.cells;
		server_world_discard(&pure); test_fixture_dir_cleanup(pure_dir);
	}
	for(unsigned d=0;d<2;d++) server_world_discard(&session.worlds[d]);
	dict_entity_clear(session.entities); string_clear(level); test_fixture_dir_cleanup(dir);
	printf("L2 %s: %u failed stored-byte/client/seam checks\n",failures ? "FAIL":"PASS",failures);
	return failures ? 1:0;
}
