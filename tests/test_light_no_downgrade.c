/* GPL-3.0-or-later. Original-JAR comparison, seed 4242, both dimensions.
 * The external script matches beta_jni.py: x-major loads -9..13, snapshot
 * each load, then the entire resident domain before/after one complete drain.
 * Expectations are original JVM bytes, never candidate-generated lighting. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "item/items.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/thread.h"
#include "worldgen/worldgen.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p,0777)
#endif

static struct server_local session;
static gzFile reference;
static unsigned snapshots;
static unsigned long long totals[2][3][3];

static void* consume(void* unused) {
	(void)unused;
	for(;;) { clin_drain_discard(); thread_msleep(1); }
	return NULL;
}

static void compare(int dim, int phase, int x, int z) {
	struct server_chunk* sc = dict_server_chunks_get(session.worlds[0].chunks,
		S_CHUNK_ID(x,z));
	assert(sc);
	/* Archive order is explicit in the independently pinned manifest. Keep
	 * IDs/meta to authenticate each full-JAR snapshot, compare light nibbles
	 * rather than packed bytes (two independently differing cells per byte). */
	unsigned char raw[82176];
	assert(gzread(reference,raw,sizeof raw)==sizeof raw);
	const unsigned char* planes[]={sc->lighting_sky,sc->lighting_torch,sc->heightmap};
	const unsigned offsets[]={49152,65536,81920};
	unsigned long long counts[3]={0};
	for(unsigned p=0;p<3;p++) {
		unsigned n=p==2 ? 256 : 32768;
		for(unsigned i=0;i<n;i++) {
			unsigned a=p==2 ? planes[p][i] : (planes[p][i/2] >> ((i&1)*4))&15;
			unsigned b=p==2 ? raw[offsets[p]+i] : (raw[offsets[p]+i/2] >> ((i&1)*4))&15;
			counts[p]+=a!=b;
		}
		totals[dim][phase][p]+=counts[p];
	}
	printf("COUNT,%d,%d,%d,%d,%llu,%llu,%llu\n",dim ? -1 : 0,phase,x,z,
		counts[0],counts[1],counts[2]);
	snapshots++;
}

int main(int argc,char** argv) {
	assert(argc==2);
	setvbuf(stdout,NULL,_IONBF,0);
	reference=gzopen(argv[1],"rb"); assert(reference);
	assert(clin_init() && svin_init()); blocks_init(); items_init();
	struct thread drain; assert(thread_create(&drain,consume,NULL,64));
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	for(int dim=0;dim<2;dim++) {
		char dir[256]; test_fixture_dir(dim ? "light_no_downgrade_nether" : "light_no_downgrade_overworld",dir,sizeof dir);
		assert(MKDIR(dir)==0);
		memset(&session,0,sizeof session); dict_entity_init(session.entities);
		string_t level; string_init_set_str(level,dir);
		struct server_world* w=&session.worlds[0];
		server_world_create(w,level,dim ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD,4242,false);
		string_clear(level); w->session=w->local=&session; jrand_set_seed(&w->rand,4242);
		for(int x=-9;x<=13;x++) for(int z=-9;z<=13;z++) {
			struct server_chunk* sc=NULL;
			assert(server_world_load_chunk(w,x,z,&sc) && sc);
			compare(dim,0,x,z);
		}
		for(int phase=1;phase<=2;phase++) {
			if(phase==2) assert(server_world_light_drain(w));
			assert(dict_server_chunks_size(w->chunks)==529);
			for(int x=-9;x<=13;x++) for(int z=-9;z<=13;z++) compare(dim,phase,x,z);
		}
		/* Assert the absent rim; loading a neighbour during comparison changes
		 * the lighting problem and must not silently improve its score. */
		for(int x=-10;x<=14;x++) for(int z=-10;z<=14;z++)
			assert(server_world_is_chunk_loaded(w,x,z)==(x>=-9 && x<=13 && z>=-9 && z<=13));
		server_world_discard(w); dict_entity_clear(session.entities); test_fixture_dir_cleanup(dir);
	}
	unsigned char extra; assert(gzread(reference,&extra,1)==0 && gzeof(reference));
	assert(gzclose(reference)==Z_OK && snapshots==3174);
	for(int d=0;d<2;d++) for(int phase=0;phase<3;phase++)
		printf("METRIC,%d,%d,%llu,%llu,%llu\n",d ? -1 : 0,phase,
			totals[d][phase][0],totals[d][phase][1],totals[d][phase][2]);
	return 0;
}
