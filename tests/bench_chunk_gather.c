/* Copyright (c) 2026 CaveWii_v2 contributors
 * Production 18^3 gathering, fully resident halo; thread CPU cycles exclude
 * preemption. The production worker's cycles include queue/build overhead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "block/blocks.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "column_view.h"
#include "network/server_world.h"
#include "util.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"
#include "lighting.h"
#include "worldgen/worldgen.h"

#define CHECK(x) do { if(!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
	__FILE__, __LINE__, #x); exit(1); } } while(0)
#include "support/column_stripe_timing.h"
#define PADDED 5832
static struct block_data owned[PADDED], generic[PADDED];
static uint64_t worker_cost;
static unsigned differential_phase;
static const char* differential_stage = "initial";

static uint64_t cpu_cost(void) {
#ifdef _WIN32
	ULONG64 cycles;
	CHECK(QueryThreadCycleTime(GetCurrentThread(), &cycles));
	return cycles;
#else
	struct timespec ts;
	CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

static void* (*worker_entry)(void*);
static void* worker_arg;
bool __real_thread_create(struct thread*, void* (*)(void*), void*, uint8_t);
static void* timed_worker(void* unused) {
	(void)unused;
#ifdef _WIN32
	CHECK(SetThreadAffinityMask(GetCurrentThread(), 1));
#endif
	uint64_t start = cpu_cost();
	void* result = worker_entry(worker_arg);
	worker_cost = cpu_cost() - start;
	return result;
}
bool __wrap_thread_create(struct thread* t, void* (*entry)(void*), void* arg,
	uint8_t priority) {
	worker_entry = entry;
	worker_arg = arg;
	return __real_thread_create(t, timed_worker, NULL, priority);
}

static void equivalent(struct chunk* c) {
	memset(owned, 0, sizeof(owned));
	memset(generic, 0, sizeof(generic));
	chunk_mesher_gather(c, owned);
	chunk_mesher_gather_generic(c, generic);
	CHECK(!memcmp(owned, generic, sizeof(owned)));
}

static double measure(struct chunk* c, bool use_generic) {
	const unsigned repeats = 4000;
	uint64_t start = cpu_cost();
	for(unsigned n = 0; n < repeats; n++) {
		if(use_generic) chunk_mesher_gather_generic(c, generic);
		else chunk_mesher_gather(c, owned);
	}
	return (double)(cpu_cost() - start) / repeats;
}

static int cmp_double(const void* a, const void* b) {
	double x = *(const double*)a, y = *(const double*)b;
	return (x > y) - (x < y);
}

static void compare_cells(void) {
	/* The fourth bitfield leaves four unspecified padding bits. Compare all
	 * four emitted field bytes, independently of the C ABI's padding value. */
	uint8_t a[PADDED * 4], b[PADDED * 4];
	for(unsigned i = 0; i < PADDED; i++) {
		a[4*i] = owned[i].type; a[4*i+1] = owned[i].metadata;
		a[4*i+2] = owned[i].sky_light; a[4*i+3] = owned[i].torch_light;
		b[4*i] = generic[i].type; b[4*i+1] = generic[i].metadata;
		b[4*i+2] = generic[i].sky_light; b[4*i+3] = generic[i].torch_light;
	}
	for(unsigned i = 0; i < sizeof(a); i++)
		if(a[i] != b[i]) {
			fprintf(stderr, "phase %u %s gather cell %u field %u: owned=%u shared=%u\n",
				differential_phase, differential_stage, i / 4, i % 4, a[i], b[i]);
			CHECK(false);
		}
}

static void shared_fixture(struct world* owned_world, struct world* shared,
	struct server_world* server) {
	string_t path;
	string_init_set_str(path, "tmp/bench_column_view_no_save");
	server_world_create(server, path, WORLD_DIM_OVERWORLD, 12345, true);
	CHECK(server->generator);
	string_clear(path);
	world_create(shared);
	uint8_t tint[256] = {0};
	for(int z = -16; z <= 16; z += 16)
		for(int x = -16; x <= 16; x += 16) {
			struct server_chunk* sc;
			server_world_begin_update(server);
			CHECK(server_world_load_chunk(server, x / 16, z / 16, &sc));
			for(unsigned lx = 0; lx < 16; lx++)
				for(unsigned lz = 0; lz < 16; lz++)
					for(unsigned y = 0; y < 128; y++) {
						unsigned i = (lx * 16 + lz) * 128 + y;
						struct block_data b = world_get_block(owned_world, x + lx, y, z + lz);
						sc->ids[i] = b.type;
						nibble_write(sc->metadata, i, b.metadata);
						nibble_write(sc->lighting_sky, i, b.sky_light);
						nibble_write(sc->lighting_torch, i, b.torch_light);
					}
			CHECK(world_build_full_section_leased(shared, x, z, sc->ids, sc->metadata,
				sc->lighting_sky, sc->lighting_torch, sc->heightmap, tint, tint, sc->backing));
		}
}

/* Real production flood with the same server backing point writer. The
 * harness supplies callbacks because server_world's versions are private. */
static bool flood_get(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
	struct block_data* b, uint8_t* height) {
	struct server_world* w = user;
	if(y < 0 || y >= 128) return false;
	struct server_chunk* sc = dict_server_chunks_get(w->chunks,
		S_CHUNK_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	if(!sc) return false;
	unsigned i = (W2C_COORD(x) * 16 + W2C_COORD(z)) * 128 + y;
	if(b) *b = (struct block_data){.type = sc->ids[i],
		.metadata = nibble_read(sc->metadata, i),
		.sky_light = nibble_read(sc->lighting_sky, i),
		.torch_light = nibble_read(sc->lighting_torch, i)};
	if(height) *height = sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * 16];
	return true;
}
static void flood_set(void* user, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	struct server_world* w = user;
	struct server_chunk* sc = dict_server_chunks_get(w->chunks,
		S_CHUNK_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	CHECK(sc);
	unsigned i = (W2C_COORD(x) * 16 + W2C_COORD(z)) * 128 + y;
	column_backing_set_cell(sc->backing, COLUMN_SKY, i, light & 15);
	column_backing_set_cell(sc->backing, COLUMN_TORCH, i, light >> 4);
}
struct differential_run {
	struct server_world* server;
	struct server_chunk* columns[9];
	uint8_t* initial[9];
	struct thread_channel go, done;
	unsigned rounds;
	struct stripe_times timing;
};
static void* differential_writer(void* user) {
	struct differential_run* r = user;
	for(unsigned n = 0; n < r->rounds; n++) {
		void* token;
		CHECK(tchannel_receive(&r->go, &token, true));
		for(unsigned c = 0; c < 9; c++) {
			struct server_chunk* sc = r->columns[c];
			/* Scattered pages in all four backing planes, including rejoin
			 * every other round. Presentation stays at consumed/client bytes. */
			for(unsigned j = 0; j < 128; j++) {
				unsigned i = j * 256 + (j * 17 + n / 2) % 256;
				for(unsigned p = 0; p < 4; p++) {
					uint8_t* base = r->initial[c] + (p ? 32768 + (p-1)*16384 : 0);
					uint8_t value = p ? nibble_read(base, i) : base[i];
					column_backing_set_cell(sc->backing, p, i, value ^ (n & 1));
				}
			}
		}
		struct server_chunk* sc = r->columns[4];
		unsigned i = (8 * 16 + 8) * 128 + 72;
		struct block_data old;
		CHECK(flood_get(r->server, 8, 72, 8, &old, NULL));
		column_backing_set_cell(sc->backing, COLUMN_IDS, i,
			n & 1 ? BLOCK_TORCH : BLOCK_AIR);
		lighting_update_at_block((struct world_modification_entry){
			.x = 8, .y = 72, .z = 8, .blk = old}, false, flood_get, flood_set, r->server);
		if(n % 8 == 0) {
			/* Exactly the runtime vertical-scan barrier used by server_world. */
			column_backing_write_begin(sc->backing, 4, 0, 32768);
			worldgen_lighting(sc, false, WORLD_DIM_OVERWORLD);
			column_backing_write_end(sc->backing, 4, 0, 32768);
		}
		tchannel_send(&r->done, r, true);
	}
	r->timing = stripe_times;
	return NULL;
}
/* There is one production client flood accumulator. Run that driver once,
 * and give the owned comparison world the same emitted light-write stream;
 * running two interleaved client drivers would corrupt its 500k-pop counter. */
static struct world* relight_shared;
static struct world* relight_owned;
void __real_chunk_set_light(struct chunk*, c_coord_t, c_coord_t, c_coord_t, uint8_t);
void __wrap_chunk_set_light(struct chunk* c, c_coord_t x, c_coord_t y, c_coord_t z, uint8_t light) {
	if(c->world == relight_shared) {
		struct chunk* owned = world_find_chunk(relight_owned, c->x+x, c->y+y, c->z+z);
		CHECK(owned);
		__real_chunk_set_light(owned, x, y, z, light);
	}
	__real_chunk_set_light(c, x, y, z, light);
}
static void differential_workload(struct world* w, struct world* shared,
	struct server_world* server, bool report) {
	struct differential_run r = {.server = server, .rounds = report ? 72 : 16};
	unsigned k = 0;
	for(int z = -1; z <= 1; z++) for(int x = -1; x <= 1; x++, k++) {
		struct server_chunk* sc = r.columns[k] = dict_server_chunks_get(server->chunks, S_CHUNK_ID(x,z));
		CHECK(sc);
		measure_stripe(sc->backing->mutex);
		r.initial[k] = malloc(81920);
		CHECK(r.initial[k]);
		memcpy(r.initial[k], sc->ids, 32768);
		memcpy(r.initial[k]+32768, sc->metadata, 16384);
		memcpy(r.initial[k]+49152, sc->lighting_sky, 16384);
		memcpy(r.initial[k]+65536, sc->lighting_torch, 16384);
		/* Give the owned oracle the same full-column residency and height
		 * metadata before relighting (the static benchmark needs only 3 slices). */
		uint8_t* presentation = malloc(81920);
		CHECK(presentation);
		struct world_section* section = dict_wsection_get(shared->sections, SECTION_TO_ID(x,z));
		column_view_copy(section->column[4]->view, presentation,
			presentation+32768, presentation+49152, presentation+65536);
		CHECK(world_build_full_section(w, x*16, z*16, presentation,
			presentation+32768, presentation+49152, presentation+65536,
			section->heightmap, section->grass_i, section->grass_j));
		free(presentation);
	}
	CHECK(tchannel_init(&r.go, 1) && tchannel_init(&r.done, 1));
	relight_shared = shared; relight_owned = w;
	stripe_times = (struct stripe_times){0};
	stripe_timing = true;
	struct thread writer;
	CHECK(__real_thread_create(&writer, differential_writer, &r, 64));
	uint64_t gather_cycles = 0, owned_cycles = 0;
	unsigned gathers = 0, relight_pops = 0, replacements = 0;
	uint32_t min_pages = UINT32_MAX, max_pages = 0;
	for(unsigned n = 0; n < r.rounds; n++) {
		differential_phase = n;
		differential_stage = "before update";
		chunk_mesher_gather(world_find_chunk(w,0,64,0), owned);
		chunk_mesher_gather(world_find_chunk(shared,0,64,0), generic);
		compare_cells();
		differential_stage = "after relight";
		tchannel_send(&r.go, &r, true);
		struct block_data block = {.type = n & 1 ? BLOCK_TORCH : BLOCK_AIR, .metadata = 5};
		world_set_block(w, 7, 72, 7, block, false);
		world_set_block(shared, 7, 72, 7, block, true);
		world_update_lighting(shared);
		relight_pops += world_lighting_driver_last_work();
		struct chunk* a = world_find_chunk(w, 0, 64, 0);
		struct chunk* b = world_find_chunk(shared, 0, 64, 0);
		for(unsigned j = 0; j < 16; j++) {
			uint64_t start = cpu_cost();
			chunk_mesher_gather(b, generic);
			gather_cycles += cpu_cost() - start;
			start = cpu_cost();
			chunk_mesher_gather(a, owned);
			owned_cycles += cpu_cost() - start;
			compare_cells(); // unchanged presentation while server floods
			gathers++;
		}
		void* token;
		CHECK(tchannel_receive(&r.done, &token, true));
		differential_stage = "after server done";
		chunk_mesher_gather(a, owned); chunk_mesher_gather(b, generic);
		compare_cells();
		struct column_view_stats st;
		column_view_get_stats(&st);
		if(st.pages < min_pages) min_pages = st.pages;
		if(st.pages > max_pages) max_pages = st.pages;
		if(n % 8 == 7) {
			/* Replacement snapshot after this writer phase, while relight
			 * frontiers survive. Mirror exact same bytes through OWNED. */
			struct server_chunk* sc = r.columns[4];
			uint8_t tint[256] = {0};
			CHECK(world_build_full_section(w, 0, 0, sc->ids, sc->metadata,
				sc->lighting_sky, sc->lighting_torch, sc->heightmap, tint, tint));
			CHECK(world_build_full_section_leased(shared, 0, 0, sc->ids, sc->metadata,
				sc->lighting_sky, sc->lighting_torch, sc->heightmap, tint, tint, sc->backing));
			replacements++;
		}
	}
	thread_join(&writer);
	CHECK(max_pages > min_pages && max_pages > 152 && relight_pops && replacements);
	check_stripe_ceiling("differential main", stripe_times);
	check_stripe_ceiling("server flood + vertical scan", r.timing);
	stripe_timing = false;
	relight_shared = relight_owned = NULL;
	printf("DIFFERENTIAL %u gathers, pages=%u..%u, relight pops=%u, replacements=%u; "
		"mean CPU %s/gather owned=%.0f shared=%.0f (lock instrumentation enabled)\n",
		gathers, min_pages, max_pages, relight_pops, replacements,
#ifdef _WIN32
		"cycles",
#else
		"ns",
#endif
		(double)owned_cycles/gathers, (double)gather_cycles/gathers);
	for(unsigned c = 0; c < 9; c++) free(r.initial[c]);
	tchannel_close(&r.go); tchannel_close(&r.done);
}

/* Exercise the longest preservation/materialization hold at REAL capacity,
 * separately from the mixed-occupancy measurement. No artificial page limit. */
static void preservation_ceiling(struct server_chunk* sc) {
	uint8_t* image = malloc(81920);
	uint8_t* exported = malloc(81920);
	CHECK(image && exported);
	memcpy(image, sc->ids, 32768); memcpy(image+32768, sc->metadata, 16384);
	memcpy(image+49152, sc->lighting_sky, 16384); memcpy(image+65536, sc->lighting_torch, 16384);
	struct column_view* target = column_view_create(sc->backing,
		image, image+32768, image+49152, image+65536);
	CHECK(target);
	/* 152 scattered pages on the target before full-pool exhaustion. */
	for(unsigned page = 0; page < 152; page++) {
		unsigned plane = page % 4, i = (page / 4 * 3) * 256;
		struct block_data b = column_view_get(target, i);
		switch(plane) {
		case 0: b.type ^= 1; image[i] = b.type; break;
		case 1: b.metadata ^= 1; nibble_write(image+32768, i, b.metadata); break;
		case 2: b.sky_light ^= 1; nibble_write(image+49152, i, b.sky_light); break;
		case 3: b.torch_light ^= 1; nibble_write(image+65536, i, b.torch_light); break;
		}
		column_view_set(target, i, b, false);
	}
	struct column_view* fillers[16];
	unsigned filled = 0;
	while(true) {
		struct column_view_stats st; column_view_get_stats(&st);
		if(st.pages == COLUMN_VIEW_POOL_PAGES) break;
		CHECK(filled < 16);
		unsigned pages = COLUMN_VIEW_POOL_PAGES - st.pages;
		if(pages > 512) pages = 512;
		memcpy(exported, sc->ids, 32768); memcpy(exported+32768, sc->metadata, 16384);
		memcpy(exported+49152, sc->lighting_sky, 16384); memcpy(exported+65536, sc->lighting_torch, 16384);
		for(unsigned page = 0; page < pages; page++) {
			unsigned p = page / 128, i = page % 128 * 256;
			uint8_t* plane = exported + (p ? 32768 + (p-1)*16384 : 0);
			if(p) nibble_write(plane, i, nibble_read(plane, i) ^ 1);
			else plane[i] ^= 1;
		}
		fillers[filled] = column_view_create(sc->backing, exported,
			exported+32768, exported+49152, exported+65536);
		CHECK(fillers[filled++]);
	}
	stripe_times = (struct stripe_times){0};
	stripe_timing = true;
	column_backing_write_begin(sc->backing, 15, 0, 32768);
	sc->ids[32767] ^= 1;
	column_backing_write_end(sc->backing, 15, 0, 32768);
	check_stripe_ceiling("full-pool bulk preservation + materialization", stripe_times);
	stripe_timing = false;
	column_view_copy(target, exported, exported+32768, exported+49152, exported+65536);
	CHECK(!memcmp(image, exported, 81920));
	for(unsigned i = 0; i < filled; i++) column_view_unref(fillers[i]);
	column_view_unref(target);
	free(image); free(exported);
}

int main(int argc, char** argv) {
#ifdef _WIN32
	if(argc > 1 && !strcmp(argv[1], "--bench"))
		CHECK(SetThreadAffinityMask(GetCurrentThread(), 1));
#endif
	blocks_init();
	struct world w;
	world_create(&w);
	for(int cy = 48; cy <= 80; cy += 16)
		for(int cz = -16; cz <= 16; cz += 16)
			for(int cx = -16; cx <= 16; cx += 16) {
				world_set_block(&w, cx, cy, cz,
					(struct block_data){.type = BLOCK_STONE}, false);
				struct chunk* c = world_find_chunk(&w, cx, cy, cz);
				CHECK(c);
				for(unsigned y = 0; y < 16; y++)
					for(unsigned z = 0; z < 16; z++)
						for(unsigned x = 0; x < 16; x++) {
							uint32_t n = (uint32_t)(cx + (int)x) * 31u
								+ (uint32_t)(cy + (int)y) * 17u + cz + z;
							chunk_set_block_raw(c, x, y, z, (struct block_data){
								.type = n % 7 ? BLOCK_STONE : BLOCK_AIR,
								.metadata = n & 15, .sky_light = (n >> 2) & 15,
								.torch_light = (n >> 4) & 15});
						}
			}
	struct chunk* center = world_find_chunk(&w, 0, 64, 0);
	unsigned halo = 0;
	for(int y = -1; y <= 16; y++)
		for(int z = -1; z <= 16; z++)
			for(int x = -1; x <= 16; x++) {
				CHECK(world_find_chunk(&w, x, y + 64, z));
				if(x < 0 || x == 16 || y < 0 || y == 16 || z < 0 || z == 16)
					halo++;
			}
	CHECK(halo == 1736);
	equivalent(center);
	puts("PASS real gather: 4096 interior + all 1736 resident halo cells byte-identical");
	struct server_world server;
	struct world shared;
	shared_fixture(&w, &shared, &server);
	struct chunk* shared_center = world_find_chunk(&shared, 0, 64, 0);
	CHECK(shared_center && shared_center->ownership == LOCAL_SHARED_VIEW && !shared_center->blocks);
	chunk_mesher_gather(shared_center, generic);
	compare_cells();
	equivalent(shared_center);
	puts("PASS shared gather: all 5832 cells equal independent owned world");
	if(argc > 1 && !strcmp(argv[1], "--bench")) {
		double a[9], b[9];
		for(unsigned i = 0; i < 9; i++) {
			/* Alternate order; medians over independent thread-CPU samples. */
			if(i & 1) { b[i] = measure(center, true); a[i] = measure(center, false); }
			else { a[i] = measure(center, false); b[i] = measure(center, true); }
			printf("sample %u owned=%.0f generic=%.0f\n", i, a[i], b[i]);
		}
		qsort(a, 9, sizeof(double), cmp_double);
		qsort(b, 9, sizeof(double), cmp_double);
		double shared_cost[9], owned_cost[9];
		for(unsigned i = 0; i < 9; i++) {
			if(i & 1) { shared_cost[i] = measure(shared_center, false); owned_cost[i] = measure(center, false); }
			else { owned_cost[i] = measure(center, false); shared_cost[i] = measure(shared_center, false); }
			printf("shared sample %u owned=%.0f shared=%.0f\n", i, owned_cost[i], shared_cost[i]);
		}
		qsort(shared_cost, 9, sizeof(double), cmp_double);
		qsort(owned_cost, 9, sizeof(double), cmp_double);
		CHECK(chunk_mesher_init());
		const unsigned builds = 200;
		for(unsigned i = 0; i < builds; i++) {
			CHECK(chunk_mesher_send(center));
			center->mesh_in_flight = true;
			ptime_t start = time_get();
			while(chunk_mesher_pending()) {
				chunk_mesher_receive();
				CHECK(time_diff_s(start, time_get()) < 30);
				if(chunk_mesher_pending()) thread_msleep(1);
			}
		}
		chunk_mesher_shutdown();
		double mesh = (double)worker_cost / builds;
		printf("MEDIAN shared provider owned=%.0f shared=%.0f; delta=%.2f%% of mesh\n",
			owned_cost[4], shared_cost[4], 100 * (shared_cost[4] - owned_cost[4]) / mesh);
#ifdef _WIN32
		printf("MEDIAN CPU cycles/gather owned=%.0f generic=%.0f; "
			"worker cycles/mesh=%.0f; generic delta=%.2f%% of mesh\n",
			a[4], b[4], mesh, 100 * (b[4] - a[4]) / mesh);
		printf("729 MHz same-cycle proxy (NOT PPC measurement): "
			"owned=%.2f us generic=%.2f us delta=%.2f us\n",
			a[4] / 729, b[4] / 729, (b[4] - a[4]) / 729);
#else
		printf("MEDIAN thread CPU ns/gather owned=%.0f generic=%.0f; "
			"worker ns/mesh=%.0f\n", a[4], b[4], mesh);
#endif
	}
	/* Divergent pages and a mixed near/far halo use the identical reader. */
	struct chunk* owned_edit = world_find_chunk(&w, 0, 64, 0);
	chunk_set_block_raw(owned_edit, 0, 0, 0, (struct block_data){.type = BLOCK_GLASS, .sky_light = 3});
	chunk_set_block_raw(shared_center, 0, 0, 0, (struct block_data){.type = BLOCK_GLASS, .sky_light = 3});
	chunk_mesher_gather(center, owned); chunk_mesher_gather(shared_center, generic);
	compare_cells();
	differential_workload(&w, &shared, &server, argc > 1 && !strcmp(argv[1], "--bench"));
	center = world_find_chunk(&w, 0, 64, 0);
	world_destroy(&shared);
	preservation_ceiling(dict_server_chunks_get(server.chunks, S_CHUNK_ID(0,0)));
	server_world_discard(&server);
	/* Exercise the unchanged mixed-provider halo and generic far center too. */
	uint8_t skin[256], ids[256], tint[256] = {0};
	memset(skin, 70, sizeof(skin)); memset(ids, BLOCK_GRASS, sizeof(ids));
	world_set_far_storage(WORLD_FAR_STORAGE_SKIN);
	CHECK(world_build_far_section(&w, 16, 0, skin, ids, tint, tint));
	equivalent(center);
	struct chunk* far_chunk = world_find_chunk(&w, 16, 70, 0);
	CHECK(far_chunk && far_chunk->ownership == FAR_SKIN);
	equivalent(far_chunk);
	world_destroy(&w);
	world_set_far_storage(WORLD_FAR_STORAGE_VOXEL);
	puts("PASS real gather: mixed far halo and far center byte-identical");
	return 0;
}
