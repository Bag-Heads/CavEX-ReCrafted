/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	hw1029c/HEAP: a real Beta 1.7.3 world (region files of several MB each)
	must load within the same malloc-heap range as a CavEX-written world.
	Measurement harness: every malloc/calloc/realloc/free of the linked
	engine objects is wrapped (ld --wrap) and charged to its call site; the
	harness reports the global high-water and the sites holding it. Pointers
	the mem2 layer tags (MEM2 on the Wii) are tallied apart from the MEM1 heap.
	The production load (SRPC_LOAD_WORLD) streams into the real client world
	(gstate.world) whose per-frame lighting driver runs each tick.
*/
#include "support/owner_swap_harness.h"
#include "lighting.h"
#include "stack.h"
#include "world.h"
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

/* ---- counting allocator -------------------------------------------------- */
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);

#define HB_PTR_SLOTS (1u << 20)
#define HB_SITE_SLOTS 65536
struct hb_ptr { void* p; size_t n; unsigned site; bool mem2; };
struct hb_site { void* ra; size_t live; size_t peak; size_t at_peak; };
static struct hb_ptr s_ptrs[HB_PTR_SLOTS];
static struct hb_site s_sites[HB_SITE_SLOTS];
static unsigned s_nsites;
static size_t s_live, s_peak, s_snap_peak, s_mem2_live, s_mem2_peak;
static bool s_armed;
static volatile int s_lock; /* the region IO worker and mesher threads allocate too */
static void lock(void) { while(__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_clear(&s_lock, __ATOMIC_RELEASE); }

static unsigned site_of(void* ra) {
	uintptr_t h = ((uintptr_t)ra * 0x9E3779B97F4A7C15ull) >> 40;
	for(unsigned i = 0; i < HB_SITE_SLOTS; i++) {
		unsigned k = (unsigned)(h + i) & (HB_SITE_SLOTS - 1);
		if(s_sites[k].ra == ra) return k;
		if(!s_sites[k].ra) { s_sites[k].ra = ra; s_nsites++; return k; }
	}
	fprintf(stderr, "site table full: %u sites\n", s_nsites);
	abort();
}
static void snapshot(void) {
	for(unsigned k = 0; k < HB_SITE_SLOTS; k++)
		s_sites[k].at_peak = s_sites[k].live;
	s_snap_peak = s_live;
}
static struct hb_ptr* slot_of(void* p) {
	uintptr_t h = ((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 44;
	for(unsigned i = 0; i < HB_PTR_SLOTS; i++) {
		unsigned k = (unsigned)(h + i) & (HB_PTR_SLOTS - 1);
		if(s_ptrs[k].p == p) return &s_ptrs[k];
		if(!s_ptrs[k].p) return NULL; /* allocated before arming */
	}
	return NULL;
}
static void note_alloc(void* p, size_t n, void* ra) {
	if(!p || !s_armed) return;
	uintptr_t h = ((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 44;
	for(unsigned i = 0; i < HB_PTR_SLOTS; i++) {
		unsigned k = (unsigned)(h + i) & (HB_PTR_SLOTS - 1);
		if(!s_ptrs[k].p || s_ptrs[k].p == (void*)1) { /* empty or tombstone */
			unsigned site = site_of(ra);
			s_ptrs[k] = (struct hb_ptr) {p, n, site, false};
			s_live += n;
			s_sites[site].live += n;
			if(s_sites[site].live > s_sites[site].peak)
				s_sites[site].peak = s_sites[site].live;
			if(s_live > s_peak) {
				s_peak = s_live;
				if(s_peak - s_snap_peak >= 64 * 1024) snapshot();
			}
			return;
		}
	}
	fprintf(stderr, "pointer table full\n");
	abort();
}
static size_t note_free(void* p) {
	if(!p) return 0;
	struct hb_ptr* e = slot_of(p);
	if(!e) return 0;
	size_t n = e->n;
	if(e->mem2) s_mem2_live -= n;
	else { s_live -= n; s_sites[e->site].live -= n; }
	e->p = (void*)1; /* tombstone keeps probe chains intact */
	return n;
}
/* mem2_alloc/mem2_realloc on PC are plain malloc/realloc + a carve note with
 * the pointer: move those bytes to the MEM2 tally (Wii: the MEM2 carve). */
static void note_mem2(void* p) {
	struct hb_ptr* e = slot_of(p);
	if(!e || e->mem2) return;
	e->mem2 = true;
	s_live -= e->n;
	s_sites[e->site].live -= e->n;
	s_mem2_live += e->n;
	if(s_mem2_live > s_mem2_peak) s_mem2_peak = s_mem2_live;
}
static void carve(enum carve_action action, uintptr_t p, uintptr_t q,
				  size_t bytes, size_t aux) {
	(void)bytes; (void)aux;
	if(action == CARVE_ALLOC) { lock(); note_mem2((void*)p); unlock(); }
	if(action == CARVE_RESIZE) { lock(); note_mem2((void*)q); unlock(); }
}
void* __wrap_malloc(size_t n) {
	void* p = __real_malloc(n);
	lock(); note_alloc(p, n, __builtin_return_address(0)); unlock();
	return p;
}
void* __wrap_calloc(size_t a, size_t b) {
	void* p = __real_calloc(a, b);
	lock(); note_alloc(p, a * b, __builtin_return_address(0)); unlock();
	return p;
}
void* __wrap_realloc(void* old, size_t n) {
	lock();
	size_t had = note_free(old);
	unlock();
	void* p = __real_realloc(old, n);
	lock();
	if(p) note_alloc(p, n, __builtin_return_address(0));
	else if(old && n) note_alloc(old, had, __builtin_return_address(0));
	unlock();
	return p;
}
void __wrap_free(void* p) {
	lock(); note_free(p); unlock();
	__real_free(p);
}

static int site_cmp(const void* a, const void* b) {
	const struct hb_site* x = a; const struct hb_site* y = b;
	return x->at_peak < y->at_peak ? 1 : x->at_peak > y->at_peak ? -1 : 0;
}
static void report(const char* label) {
	lock();
	snapshot();
	uintptr_t base = 0;
#ifdef _WIN32
	base = (uintptr_t)GetModuleHandle(NULL);
#endif
	printf("[%s] mem1 live=%zu KB peak=%zu KB | mem2 live=%zu KB peak=%zu KB | sites=%u\n",
		   label, s_live / 1024, s_peak / 1024, s_mem2_live / 1024,
		   s_mem2_peak / 1024, s_nsites);
	static struct hb_site sorted[HB_SITE_SLOTS];
	memcpy(sorted, s_sites, sizeof sorted);
	unlock();
	qsort(sorted, HB_SITE_SLOTS, sizeof sorted[0], site_cmp);
	for(unsigned i = 0; i < 10 && sorted[i].at_peak; i++)
		printf("  live %8zu KB (site peak %8zu KB) rva=0x%llx\n",
			   sorted[i].at_peak / 1024, sorted[i].peak / 1024,
			   (unsigned long long)((uintptr_t)sorted[i].ra - base));
	fflush(stdout);
}
static void client_report(const char* label) {
	printf("[%s] client: cpark=%u coom=%u cflood=%u repairs pending=%u done=%u "
		   "flood_q=%zu KB updates_q=%zu KB sections=%zu\n",
		   label, lighting_client_ceiling_parks(), lighting_client_oom_parks(),
		   lighting_client_flood_max_pops(),
		   world_light_repair_pending(&gstate.world), world_light_repairs_done(),
		   lighting_drain_queue_bytes(&gstate.world.lighting_flood_queue) / 1024,
		   lighting_drain_queue_bytes(&gstate.world.lighting_updates) / 1024,
		   (size_t)dict_wsection_size(gstate.world.sections));
	fflush(stdout);
}

/* ---- fixture copy -------------------------------------------------------- */
static void file_copy(const char* from, const char* to) {
	FILE* a = fopen(from, "rb");
	FILE* b = fopen(to, "wb");
	assert(a && b);
	static unsigned char buf[1 << 16];
	size_t n;
	while((n = fread(buf, 1, sizeof buf, a)) > 0)
		assert(fwrite(buf, 1, n, b) == n);
	assert(fclose(a) == 0 && fclose(b) == 0);
}
static void tree_copy(const char* from, const char* to) {
	assert(MKDIR(to) == 0);
	DIR* dir = opendir(from);
	assert(dir);
	struct dirent* e;
	while((e = readdir(dir))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char a[1024], b[1024];
		snprintf(a, sizeof a, "%s/%s", from, e->d_name);
		snprintf(b, sizeof b, "%s/%s", to, e->d_name);
		if(test_fixture_is_dir(from, e->d_name)) tree_copy(a, b);
		else file_copy(a, b);
	}
	assert(closedir(dir) == 0);
}

/* ---- what seeds the client relight: count the server's block changes ------ */
static unsigned s_rpc_count[CRPC_SET_BLOCK_PACK + 64];
static unsigned s_set_blocks, s_set_blocks_shown;
static unsigned s_set_block_by_type[256];
static void seen_set_block(const struct crpc_set_block* b) {
	s_set_blocks++;
	s_set_block_by_type[b->block.type]++;
	if(s_set_blocks_shown < 24) {
		s_set_blocks_shown++;
		printf("  SET_BLOCK (%d,%d,%d) type=%u meta=%u sky=%u torch=%u\n",
			   (int)b->x, (int)b->y, (int)b->z, b->block.type, b->block.metadata,
			   b->block.sky_light, b->block.torch_light);
	}
}
static void rpc_seen(const struct client_rpc* rpc, void* unused) {
	consumed(rpc, unused);
	if((unsigned)rpc->type < sizeof s_rpc_count / sizeof s_rpc_count[0])
		s_rpc_count[rpc->type]++;
	if(rpc->type == CRPC_SET_BLOCK)
		seen_set_block(&rpc->payload.set_block);
	if(rpc->type == CRPC_SET_BLOCK_PACK)
		for(uint32_t i = 0; i < rpc->payload.set_block_pack.n; i++)
			if(!((rpc->payload.set_block_pack.spawn_mask | rpc->payload.set_block_pack.other_mask) & (1u << i)))
				seen_set_block(&rpc->payload.set_block_pack.rec[i].set_block);
}
static void rpc_report(void) {
	printf("  rpcs: CHUNK=%u SET_BLOCK=%u SET_BLOCK_PACK=%u set_blocks=%u\n",
		   s_rpc_count[CRPC_CHUNK], s_rpc_count[CRPC_SET_BLOCK],
		   s_rpc_count[CRPC_SET_BLOCK_PACK], s_set_blocks);
	printf("  set_block types:");
	for(unsigned t = 0; t < 256; t++)
		if(s_set_block_by_type[t]) printf(" %u:%u", t, s_set_block_by_type[t]);
	printf("\n");
	fflush(stdout);
}

/* ---- DIAG: which flood explodes, and what it rewrites ---------------------- */
#define SNAP_SECTIONS 1024
struct snap_section { int64_t id; uint8_t* light; /* [cell] = sky<<4|torch */ uint8_t* type; };
static struct snap_section s_snap[SNAP_SECTIONS];
static unsigned s_snap_n;
static struct world_modification_entry s_seeds[64];
static unsigned s_seeds_n;
static uint32_t s_cflood_before;
static bool s_snapped;
static void snap_light(void) {
	for(unsigned i = 0; i < s_snap_n; i++) { free(s_snap[i].light); free(s_snap[i].type); }
	s_snap_n = 0;
	dict_wsection_it_t it;
	for(dict_wsection_it(it, gstate.world.sections); !dict_wsection_end_p(it);
		dict_wsection_next(it)) {
		int64_t id = dict_wsection_ref(it)->key;
		if(dict_wsection_ref(it)->value.is_far_lod || s_snap_n == SNAP_SECTIONS) continue;
		struct snap_section* ss = &s_snap[s_snap_n++];
		ss->id = id;
		ss->light = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
		ss->type = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
		w_coord_t cx = (int32_t)(uint32_t)(uint64_t)id, cz = (int32_t)(uint32_t)((uint64_t)id >> 32);
		for(int lx = 0; lx < CHUNK_SIZE; lx++)
			for(int lz = 0; lz < CHUNK_SIZE; lz++)
				for(int y = 0; y < WORLD_HEIGHT; y++) {
					struct block_data b = world_get_block(&gstate.world,
						cx * CHUNK_SIZE + lx, y, cz * CHUNK_SIZE + lz);
					size_t k = (size_t)y + ((size_t)lz + (size_t)lx * CHUNK_SIZE) * WORLD_HEIGHT;
					ss->light[k] = (uint8_t)((b.sky_light << 4) | b.torch_light);
					ss->type[k] = b.type;
				}
	}
}
struct diff_key { uint8_t type, kind, from, to; uint32_t n; };
static void diff_light(void) {
	struct diff_key keys[512]; unsigned nk = 0; size_t total = 0;
	int minx = INT32_MAX, maxx = INT32_MIN, miny = 127, maxy = 0, minz = INT32_MAX, maxz = INT32_MIN;
	for(unsigned i = 0; i < s_snap_n; i++) {
		struct snap_section* ss = &s_snap[i];
		w_coord_t cx = (int32_t)(uint32_t)(uint64_t)ss->id, cz = (int32_t)(uint32_t)((uint64_t)ss->id >> 32);
		for(int lx = 0; lx < CHUNK_SIZE; lx++)
			for(int lz = 0; lz < CHUNK_SIZE; lz++)
				for(int y = 0; y < WORLD_HEIGHT; y++) {
					w_coord_t x = cx * CHUNK_SIZE + lx, z = cz * CHUNK_SIZE + lz;
					struct block_data b = world_get_block(&gstate.world, x, y, z);
					size_t k = (size_t)y + ((size_t)lz + (size_t)lx * CHUNK_SIZE) * WORLD_HEIGHT;
					uint8_t now = (uint8_t)((b.sky_light << 4) | b.torch_light), was = ss->light[k];
					if(now == was) continue;
					total++;
					if(ss->type[k] == 1 && (now >> 4) == 15 && (was >> 4) == 0 && total % 997 == 1) {
						struct world_section* sec = dict_wsection_get(gstate.world.sections, ss->id);
						printf("    sample stone (%d,%d,%d): client heightmap[x+z*16]=%u heightmap[x*16+z]=%u\n",
							   (int)x, y, (int)z, sec ? sec->heightmap[lx + lz * CHUNK_SIZE] : 255,
							   sec ? sec->heightmap[lx * CHUNK_SIZE + lz] : 255);
					}
					if(x < minx) minx = x; if(x > maxx) maxx = x; if(z < minz) minz = z; if(z > maxz) maxz = z;
					if(y < miny) miny = y; if(y > maxy) maxy = y;
					for(int kind = 0; kind < 2; kind++) {
						uint8_t from = kind ? (was & 15) : (was >> 4), to = kind ? (now & 15) : (now >> 4);
						if(from == to) continue;
						unsigned j;
						for(j = 0; j < nk; j++)
							if(keys[j].type == ss->type[k] && keys[j].kind == kind && keys[j].from == from && keys[j].to == to) break;
						if(j == nk && nk < 512) keys[nk++] = (struct diff_key) {ss->type[k], (uint8_t)kind, from, to, 0};
						if(j < nk) keys[j].n++;
					}
				}
	}
	printf("  DIAG flood rewrote %zu cells, bbox x[%d..%d] y[%d..%d] z[%d..%d]\n",
		   total, minx, maxx, miny, maxy, minz, maxz);
	for(unsigned pass = 0; pass < 12; pass++) {
		unsigned best = 0; for(unsigned j = 1; j < nk; j++) if(keys[j].n > keys[best].n) best = j;
		if(nk == 0 || keys[best].n == 0) break;
		printf("    type=%u %s %u->%u : %u cells\n", keys[best].type, keys[best].kind ? "torch" : "sky", keys[best].from, keys[best].to, keys[best].n);
		keys[best].n = 0;
	}
	fflush(stdout);
}

/* ---- drive ---------------------------------------------------------------- */
static bool s_in_flood;
static unsigned s_flood_frames;
static void client_frame(void) {
	clin_update_drain();
	bool diag = getenv("CW_HEAP_DIAG") != NULL;
	if(diag && !s_in_flood && lighting_flood_queue_empty(&gstate.world.lighting_flood_queue)
	   && !stack_empty(&gstate.world.lighting_updates)) {
		s_seeds_n = 0;
		for(size_t i = stack_size(&gstate.world.lighting_updates); i > 0 && s_seeds_n < 64; i--)
			stack_at(&gstate.world.lighting_updates, &s_seeds[s_seeds_n++], i - 1);
		s_cflood_before = lighting_client_flood_max_pops();
		snap_light();
		s_snapped = true;
	}
	world_update_lighting(&gstate.world);
	if(!diag) return;
	bool queued = !lighting_flood_queue_empty(&gstate.world.lighting_flood_queue);
	if(!s_in_flood && queued && s_snapped) { s_in_flood = true; s_flood_frames = 0; }
	if(s_in_flood) {
		s_flood_frames++;
		if(!queued || s_flood_frames == 40) {
			printf("  DIAG flood %s after %u frames (max pops %u -> %u); first seeds (top first):\n",
				   queued ? "STILL RUNNING" : "ended", s_flood_frames, s_cflood_before,
				   lighting_client_flood_max_pops());
			for(unsigned i = 0; i < s_seeds_n && i < 6; i++)
				printf("    seed (%d,%d,%d) type=%u\n", (int)s_seeds[i].x, (int)s_seeds[i].y,
					   (int)s_seeds[i].z, s_seeds[i].blk.type);
			diff_light();
			if(!queued) { s_in_flood = false; s_snapped = false; }
		}
	}
}

static struct server_local* s_srv;
static void probe_cell(const char* when, w_coord_t x, w_coord_t y, w_coord_t z) {
	struct block_data sb = {0}, cb = world_get_block(&gstate.world, x, y, z);
	bool have = server_world_get_block(&s_srv->worlds[0] /* Overworld-only harness: dj_orig spawn */, x, y, z, &sb);
	printf("  PROBE %s (%d,%d,%d): server %s type=%u sky=%u | client type=%u sky=%u\n", when,
		   (int)x, (int)y, (int)z, have ? "has" : "LACKS", sb.type, sb.sky_light, cb.type, cb.sky_light);
}
static void load_and_stream(const char* world, unsigned ticks) {
	struct server_local* s = calloc(1, sizeof *s);
	assert(s);
	s_srv = s;
	server_local_test_create_without_thread(s, false, false, false, false,
		false, false, false);
	svin_set_local_motion(true);
	if(getenv("CW_HEAP_NOFAR"))
		s->far_view_distance = 0; /* experiment: no far-LOD ring */
	s_armed = true;
	mem2_carve_observe = carve;
	report("before load");
	struct server_rpc call = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(call.payload.load_world.name, world);
	assert(svin_rpc_send(&call));
	drain_server(s);
	assert(s->player.has_pos && !s->load_failure_terminal);
	report("after load");
	client_report("after load");
	if(getenv("CW_HEAP_DIAG")) {
		/* Transport only (no simulation) until the view square is resident
		 * and the client relight is idle; then simulate with a stable client. */
		s->paused = true;
		size_t resident = 0; unsigned quiet = 0;
		for(unsigned n = 0; n < 4000 && quiet < 8; n++) {
			client_acks_marker(s);
			tick(s);
			clin_update_drain();
			size_t now = dict_server_chunks_size(s->worlds[0].chunks /* Overworld-only harness */);
			quiet = now == resident ? quiet + 1 : 0;
			resident = now;
		}
		for(int n = 0; n < 100000 && !(stack_empty(&gstate.world.lighting_updates)
			&& lighting_flood_queue_empty(&gstate.world.lighting_flood_queue)); n++)
			world_update_lighting(&gstate.world);
		char label[64];
		snprintf(label, sizeof label, "paused-settled srv_chunks=%zu", resident);
		report(label); client_report(label); rpc_report();
		s->paused = false;
	}
	for(unsigned t = 0; t < ticks; t++) {
		client_acks_marker(s);
		tick(s);
		client_frame();
		if(getenv("CW_HEAP_DIAG") && t < 12) {
			char when[32]; snprintf(when, sizeof when, "t%u", t);
			probe_cell(when, 168, 31, -47);
			struct world_section* sec = dict_wsection_get(gstate.world.sections, SECTION_TO_ID(10, -3));
			printf("    section(10,-3) %s is_far_lod=%d; FAR_CHUNK rpcs=%u CHUNK rpcs=%u\n",
				   sec ? "present" : "absent", sec ? sec->is_far_lod : -1,
				   s_rpc_count[CRPC_FAR_CHUNK], s_rpc_count[CRPC_CHUNK]);
		}
		if(t % 10 == 9) {
			char label[64];
			snprintf(label, sizeof label, "tick %u srv_chunks=%zu", t + 1,
					 dict_server_chunks_size(s->worlds[0].chunks /* Overworld-only harness */));
			report(label);
			client_report(label);
			rpc_report();
			if(getenv("CW_HEAP_DIAG")) {
				probe_cell(label, 168, 31, -47); /* file: air, sky 15, heightmap 7 */
				probe_cell(label, 168, 5, -47);  /* file: slab 43 */
				probe_cell(label, 174, 10, -48); /* file: stone 1 */
			}
		}
	}
	/* THE BUDGET (hw1029c): the client relight frontier is MEM1 on the Wii
	 * (16-byte entries, doubling realloc). A CavEX-written world never grows
	 * it past its 16-entry creation size during a load; dj_orig grew it to
	 * 2 MB on PC and to the 500k-pop park + realloc OOM (coom=1, cpark=18)
	 * on the Wii. No ceiling park, no failed grow, frontier under 256 KB. */
	size_t flood_q = lighting_drain_queue_bytes(&gstate.world.lighting_flood_queue);
	printf("BUDGET: flood_q=%zu B cpark=%u coom=%u cflood=%u mem1_peak=%zu KB\n",
		   flood_q, lighting_client_ceiling_parks(), lighting_client_oom_parks(),
		   lighting_client_flood_max_pops(), s_peak / 1024);
	bool within = flood_q <= 256u * 1024u && lighting_client_ceiling_parks() == 0
		&& lighting_client_oom_parks() == 0;
	s->paused = true;
	if(!server_local_unload_world_impl(s, false))
		puts("NOTE: unload refused (checkpoint failure) -- separate finding");
	clin_update_drain();
	report("after unload");
	mem2_carve_observe = NULL;
	s_armed = false;
	server_local_shutdown(s);
	svin_set_local_motion(false);
	free(s);
	if(!within) {
		puts("test_dropin_heap_budget: FAIL (client relight frontier over budget)");
		exit(1);
	}
}

int main(int argc, char** argv) {
	harness_init();
	const char* src = argc > 1 ? argv[1] : getenv("CW_DROPIN_HEAP_WORLD");
	if(!src) src = "tests/fixtures/beta_import/world";
	unsigned ticks = argc > 2 ? (unsigned)atoi(argv[2]) : 100;
	char root[256], world[512];
	test_fixture_dir("test_dropin_heap_budget", root, sizeof root);
	assert(MKDIR(root) == 0);
	snprintf(world, sizeof world, "%s/world", root);
	tree_copy(src, world);
	clin_test_set_consume_observer(rpc_seen, NULL);
	load_and_stream(world, ticks);
	clin_test_set_consume_observer(NULL, NULL);
	test_fixture_dir_cleanup(root);
	puts("test_dropin_heap_budget: PASS");
	return 0;
}
