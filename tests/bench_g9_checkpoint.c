/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Production engine, one workload thread, same pre-drain roof/torch edits.
 * Link-only wrappers observe existing checkpoint stage brackets; no engine
 * profiling seam, altered workload, or Wii telemetry is needed. Windows
 * reports scheduled thread cycles (also as a TSC-scaled time estimate),
 * POSIX reports CLOCK_THREAD_CPUTIME_ID. Wall time includes durable IO.
 */
#include <inttypes.h>
#include <zlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <x86intrin.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <time.h>
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/g9_drain_workload.h"
#include "network/region_archive.h"
#include "tps_observer.h"
#include "column_view.h"
#ifdef CAVEWII_SERVER_LOCAL_TEST
#include "network/level_archive.h"
#include "entity/combat.h"
#include "game/difficulty.h"
#include "network/inventory_logic.h"
#include "platform/mem2.h"
#endif

#define SAMPLES 90
static struct server_local host;
static bool armed;
static unsigned stage;
static uint64_t start_cpu, start_wall, cpu[5], wall[5];
static uint64_t part_start[TPS_PART_COUNT], parts[TPS_PART_COUNT];
static uint64_t sample_cpu[5][SAMPLES], sample_wall[5][SAMPLES];
static uint64_t sample_parts[TPS_PART_COUNT][SAMPLES];
static double cpu_per_ns = 1.0;
static unsigned n;
static uint64_t deflate_start, sample_deflate[SAMPLES];
static uint64_t deflate_cpu, payload_bytes, sector_bytes, payloads;

static void require(bool ok, const char* what) {
	if(!ok) { fprintf(stderr, "bench failure: %s\n", what); exit(1); }
}
static uint64_t cpu_now(void) {
#ifdef _WIN32
	ULONG64 cycles;
	require(QueryThreadCycleTime(GetCurrentThread(), &cycles), "thread cycles");
	return cycles;
#else
	struct timespec t;
	require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0, "thread CPU clock");
	return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
#endif
}
int __real_deflate(z_streamp stream, int flush);
int __wrap_deflate(z_streamp stream, int flush) {
	uint64_t c = armed ? cpu_now() : 0;
	int status = __real_deflate(stream, flush);
	if(armed) deflate_cpu += cpu_now() - c;
	return status;
}
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* g9-savemiss-1009 attribution: inclusive CPU of the save-path callees,
 * observed at their cross-TU call boundary (link-time wraps; recursion
 * inside one TU is not re-entered). Counts are calls, bytes are payload. */
#include "cNBT/nbt.h"
#include "item/item_nbt.h"
#include "network/tile_entities_nbt.h"
#include "network/entity_archive.h"
enum { FN_INFLATE, FN_PARSE, FN_CLONE, FN_DUMP, FN_SCOPE, FN_LOSSLESS,
	FN_TEB, FN_ENB, FN_TEP, FN_ENP, FN_FREE, FN_FOPEN, FN_N };
static const char* const fn_name[FN_N] = {"inflate", "nbt_parse", "nbt_clone",
	"nbt_dump_binary_hint", "item_nbt_scope_writable", "item_nbt_clone_lossless",
	"tile_entities_build", "chunk_entities_build", "tile_entities_preserve",
	"chunk_entities_preserve", "nbt_free", "fopen"};
static uint64_t fn_cpu[FN_N][SAMPLES], fn_calls[FN_N][SAMPLES], fn_bytes[FN_N][SAMPLES];
static unsigned fn_depth;
#define FN_BEGIN(id) uint64_t fn_t0 = 0; const bool fn_top = armed && !fn_depth; \
	fn_depth++; if(fn_top) fn_t0 = cpu_now()
#define FN_END(id, nbytes) do { fn_depth--; if(fn_top && armed) { \
	fn_cpu[id][n] += cpu_now() - fn_t0; fn_calls[id][n]++; fn_bytes[id][n] += (nbytes); } } while(0)
int __real_inflate(z_streamp, int);
int __wrap_inflate(z_streamp s, int f) {
	uLong before = s->total_out; FN_BEGIN(FN_INFLATE);
	int r = __real_inflate(s, f); FN_END(FN_INFLATE, s->total_out - before); return r;
}
nbt_node* __real_nbt_parse(const void*, size_t);
nbt_node* __wrap_nbt_parse(const void* m, size_t l) {
	FN_BEGIN(FN_PARSE); nbt_node* r = __real_nbt_parse(m, l); FN_END(FN_PARSE, l); return r;
}
nbt_node* __real_nbt_clone(nbt_node*);
nbt_node* __wrap_nbt_clone(nbt_node* t) {
	FN_BEGIN(FN_CLONE); nbt_node* r = __real_nbt_clone(t); FN_END(FN_CLONE, 0); return r;
}
struct buffer __real_nbt_dump_binary_hint(const nbt_node*, size_t);
struct buffer __wrap_nbt_dump_binary_hint(const nbt_node* t, size_t h) {
	FN_BEGIN(FN_DUMP); struct buffer r = __real_nbt_dump_binary_hint(t, h);
	FN_END(FN_DUMP, r.len); return r;
}
bool __real_item_nbt_scope_writable(const nbt_node*);
bool __wrap_item_nbt_scope_writable(const nbt_node* t) {
	FN_BEGIN(FN_SCOPE); bool r = __real_item_nbt_scope_writable(t); FN_END(FN_SCOPE, 0); return r;
}
nbt_node* __real_item_nbt_clone_lossless(const nbt_node*);
nbt_node* __wrap_item_nbt_clone_lossless(const nbt_node* t) {
	FN_BEGIN(FN_LOSSLESS); nbt_node* r = __real_item_nbt_clone_lossless(t); FN_END(FN_LOSSLESS, 0); return r;
}
struct nbt_list* __real_tile_entities_build(const struct chunk_tile_entities*, int32_t, int32_t, void**);
struct nbt_list* __wrap_tile_entities_build(const struct chunk_tile_entities* te, int32_t x, int32_t z, void** a) {
	FN_BEGIN(FN_TEB); struct nbt_list* r = __real_tile_entities_build(te, x, z, a); FN_END(FN_TEB, 0); return r;
}
struct nbt_list* __real_chunk_entities_build(const struct chunk_entities*, void**);
struct nbt_list* __wrap_chunk_entities_build(const struct chunk_entities* e, void** a) {
	FN_BEGIN(FN_ENB); struct nbt_list* r = __real_chunk_entities_build(e, a); FN_END(FN_ENB, e ? e->count : 0); return r;
}
nbt_node* __real_tile_entities_preserve(const nbt_node*, const nbt_node*, const struct chunk_tile_entities*, bool, int32_t, int32_t);
nbt_node* __wrap_tile_entities_preserve(const nbt_node* o, const nbt_node* g, const struct chunk_tile_entities* c, bool nat, int32_t x, int32_t z) {
	FN_BEGIN(FN_TEP); nbt_node* r = __real_tile_entities_preserve(o, g, c, nat, x, z); FN_END(FN_TEP, 0); return r;
}
nbt_node* __real_chunk_entities_preserve(const nbt_node*, const nbt_node*, const struct chunk_entities*, bool, w_coord_t, w_coord_t);
nbt_node* __wrap_chunk_entities_preserve(const nbt_node* o, const nbt_node* g, const struct chunk_entities* c, bool nat, w_coord_t x, w_coord_t z) {
	FN_BEGIN(FN_ENP); nbt_node* r = __real_chunk_entities_preserve(o, g, c, nat, x, z); FN_END(FN_ENP, 0); return r;
}
void __real_nbt_free(nbt_node*);
void __wrap_nbt_free(nbt_node* t) { FN_BEGIN(FN_FREE); __real_nbt_free(t); FN_END(FN_FREE, 0); }
FILE* __real_fopen(const char*, const char*);
FILE* __wrap_fopen(const char* p, const char* m) {
	FN_BEGIN(FN_FOPEN); FILE* r = __real_fopen(p, m); FN_END(FN_FOPEN, 0); return r;
}
#endif
/* Remember written records at the external call boundary; read framing only
 * AFTER the measured checkpoint ends. No census/no-op predicate is changed. */
static struct { struct region_archive* ra; w_coord_t x, z; } written[128];
static unsigned nwritten;
bool __real_region_archive_set_chunk_cached(struct region_archive*, w_coord_t, w_coord_t,
	struct server_chunk*, const struct chunk_tile_entities*, const struct chunk_entities*, struct region_encode_cache**);
bool __wrap_region_archive_set_chunk_cached(struct region_archive* ra, w_coord_t x, w_coord_t z,
	struct server_chunk* sc, const struct chunk_tile_entities* te, const struct chunk_entities* en, struct region_encode_cache** cache) {
	bool ok = __real_region_archive_set_chunk_cached(ra,x,z,sc,te,en,cache);
	if(armed && ok) {
		require(nwritten < 128, "record observer capacity");
		written[nwritten].ra = ra; written[nwritten].x = x; written[nwritten++].z = z;
	}
	return ok;
}
static void observe_bytes(void) {
	for(unsigned k = 0; k < nwritten; k++) {
		struct region_archive* ra = written[k].ra;
		uint32_t e = ra->offsets[(written[k].x & 31) + (written[k].z & 31) * 32];
		long pos = ftell(ra->file); uint8_t h[5];
		require(pos >= 0 && !fseek(ra->file, (long)(e >> 8) * 4096, SEEK_SET), "record seek");
		require(fread(h,1,5,ra->file) == 5 && h[4] == 2, "record framing");
		uint32_t len = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
		require(len > 1 && len + 4 <= (e & 255) * 4096, "record bounds");
		payload_bytes += len - 1; sector_bytes += (e & 255) * 4096; payloads++;
		require(!fseek(ra->file,pos,SEEK_SET), "record seek restore");
	}
	nwritten = 0;
}
void __real_g9_probe_begin(uint32_t world, uint32_t which);
void __real_g9_probe_end(uint32_t pops, uint32_t us);
void __wrap_g9_probe_begin(uint32_t world, uint32_t which) {
	__real_g9_probe_begin(world, which);
	if(armed) {
		stage = which;
		start_wall = tps_wall_now_ns();
		start_cpu = cpu_now();
	}
}
void __wrap_g9_probe_end(uint32_t pops, uint32_t us) {
	if(armed) {
		cpu[stage] += cpu_now() - start_cpu;
		wall[stage] += tps_wall_now_ns() - start_wall;
	}
	__real_g9_probe_end(pops, us);
}
static void region_part(enum tps_action action, unsigned part) {
	if(!armed) return;
	if(action == TPS_ENTER) part_start[part] = cpu_now();
	else if(action == TPS_LEAVE) parts[part] += cpu_now() - part_start[part];
}
static int cmp_u64(const void* a, const void* b) {
	uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
	return x < y ? -1 : x > y;
}
static uint64_t median(uint64_t* values, unsigned n) {
	qsort(values, n, sizeof(*values), cmp_u64);
	return values[n / 2];
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
void __real_clin_rpc_send(struct client_rpc* call);
void __wrap_clin_rpc_send(struct client_rpc* call) {
	clin_drain_discard();
	__real_clin_rpc_send(call);
	clin_drain_discard();
}
bool __real_server_world_save_chunk_coherent(struct server_world*, w_coord_t, w_coord_t);
bool __wrap_server_world_save_chunk_coherent(struct server_world* w, w_coord_t x, w_coord_t z) {
	require(n < SAMPLES && !armed, "checkpoint sample capacity/no nesting");
	memset(cpu, 0, sizeof cpu); memset(wall, 0, sizeof wall);
	memset(parts, 0, sizeof parts);
	uint64_t c = cpu_now(), t = tps_wall_now_ns();
	deflate_start = deflate_cpu; armed = true;
	bool ok = __real_server_world_save_chunk_coherent(w, x, z);
	armed = false;
	cpu[0] = cpu_now() - c; wall[0] = tps_wall_now_ns() - t;
	observe_bytes();
	sample_deflate[n] = deflate_cpu - deflate_start;
	for(unsigned j = 0; j < 5; j++) {
		sample_cpu[j][n] = cpu[j]; sample_wall[j][n] = wall[j];
	}
	for(unsigned j = 0; j < TPS_PART_COUNT; j++) sample_parts[j][n] = parts[j];
	n++;
	return ok;
}

/* Run the real unchanged tile-phase driver through complete server updates.
 * A separate process per invocation isolates process-global simulation state.
 * The test-engine entry seam starts no server/consumer thread; publications
 * are consumed inline and the IO helper is OFF. No checkpoint is injected.
 * Pregeneration/deterministic streaming match test_g9_save_io_equiv's session.
 */
static void run_live(unsigned ticks, bool superflat, int64_t seed) {
	char dir[220]; test_fixture_dir("bench_g9_checkpoint_live", dir, sizeof dir); MKDIR(dir);
	struct server_local* s = &host;
	memset(s, 0, sizeof *s);
	server_local_test_create_without_thread(s, false, false, false, false, false, superflat, false);
	string_t level; string_init_set_str(level, dir);
	require(level_archive_create_new(&s->level, level, seed, (vec3){8,4,8}), "level");
	string_set(s->level_name, level);
	require(server_world_create_with_populate_scratch(&s->worlds[0], level,
		WORLD_DIM_OVERWORLD, seed, superflat, s->world_populate_scratch), "world");
	string_clear(level);
	s->worlds[0].local = s; s->worlds[0].session = s;
	s->player.inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data){.id=ITEM_DIAMOND,.count=3};
	s->scheduled_tick_budget = 128; s->rand_src.seed = 0x9E11u; s->world_time = 6000;
	server_local_skylight_recompute(s);
	for(int x = -MAX_VIEW_DISTANCE; x <= MAX_VIEW_DISTANCE; x++)
		for(int z = -MAX_VIEW_DISTANCE; z <= MAX_VIEW_DISTANCE; z++) {
			struct server_chunk* sc; bool done = false;
			for(unsigned i = 0; i < 1000 && !done; i++) {
				server_world_begin_update(&s->worlds[0]);
				done = server_world_ensure_populated(&s->worlds[0], s, x, z, &sc);
				clin_drain_discard();
			}
			require(done, "pregeneration");
		}
	uint8_t hh; require(server_world_get_height(&s->worlds[0], 8, 8, &hh), "height");
	s->player.has_pos = s->player.finished_loading = true;
	s->player.x = s->player.z = 8.5; s->player.y = hh + (double)PLAYER_EYE_HEIGHT;
	s->player.health = MAX_PLAYER_HEALTH; s->player.oxygen = MAX_OXYGEN;
	s->render_demo_mob = -1; s->light_queue_stress = true; s->light_queue_defer = 1;
	s->autosave_ticks = 200; s->autosave_timer = 0;
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(server_local_test_chunk_send_batch());
	server_local_test_set_last_played(INT64_C(1791532400000));
	server_local_request_difficulty(0);
	for(unsigned tick = 0; tick < ticks; tick++) {
		s->player.health = MAX_PLAYER_HEALTH;
		server_local_test_update_once(s);
		clin_drain_discard();
	}
	uint32_t memo[4]; region_archive_encode_cache_stats(s->worlds[0].checkpoint_encode_cache, memo);
	printf("driver tick=%d world_time=%lld loading=%d input_ack=%u stopped=%d pos=%d light_fault=%d\n",
		s->light_queue_tick, (long long)s->world_time, !s->player.finished_loading,
		s->input_ack_pending, s->load_failure_terminal, s->player.has_pos, s->worlds[0].light_fault);
	require(n > 0, "real driver produced coherent checkpoints");
	uint32_t rbs[2]; region_archive_encode_cache_readback_stats(s->worlds[0].checkpoint_encode_cache, rbs);
	printf("read-back memo: hits=%u arms=%u\n", rbs[0], rbs[1]);
	printf("real stress: %s seed=%" PRId64 ", %u ticks, %u coherent checkpoints, memo hits=%u (prefix=%u) misses=%u bytes=%u\n",
		superflat ? "superflat" : "natural", seed, ticks, n, memo[0], memo[3], memo[1], memo[2]);
	server_world_destroy(&s->worlds[0]); level_archive_destroy(&s->level);
	server_world_populate_scratch_destroy(s->world_populate_scratch);
	inventory_destroy(&s->player.inventory, &s->player); dict_entity_clear(s->entities);
	dict_autosave_saved_clear(s->autosave_saved_this_epoch);
	for(unsigned k = 0; k < SERVER_LOCAL_WORLD_SLOTS; k++) dict_autosave_saved_clear(s->autosave_saved_this_epoch_w[k]);
	string_clear(s->level_name); if(s->residency) mem2_free(s->residency);
	test_fixture_dir_cleanup(dir);
}
#endif
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* codec = getenv("CW_G9_SAVE_ZLIB_LEVEL");
	int level = codec && codec[0] >= '1' && codec[0] <= '6' && !codec[1] ? codec[0] - '0' : 6;
	region_archive_save_zlib_level_configure(level);
#ifndef CAVEWII_SERVER_LOCAL_TEST
	unsigned reps = argc > 1 ? (unsigned)atoi(argv[1]) : 3;
	require(reps > 0 && reps <= 3, "repetitions 1..3");
	/* g9-trial-1009: "views" gives every loaded column one presentation
	 * view, as the console's local client holds for the columns it shows,
	 * so changing light writes pay the view page copy-on-write as there. */
	const bool with_views = argc > 2 && !strcmp(argv[2], "views");
	require(argc <= 2 || with_views, "production arguments: [reps [views]]");
	enum { BENCH_VIEWS = (2 * G9_DRAIN_RADIUS + 1) * (2 * G9_DRAIN_RADIUS + 1) };
	struct column_view* views[BENCH_VIEWS];
	unsigned view_n = 0;
#endif
#ifdef _WIN32
	double lo = 1e30, hi = 0, sum = 0;
	for(unsigned i = 0; i < 3; i++) {
		uint64_t begin = tps_wall_now_ns(), cycles = __rdtsc(), end;
		do { end = tps_wall_now_ns(); } while(end - begin < 100000000);
		double rate = (double)(__rdtsc() - cycles) / (double)(end - begin);
		if(rate < lo) lo = rate;
		if(rate > hi) hi = rate;
		sum += rate;
	}
	cpu_per_ns = sum / 3;
	printf("clock: Windows scheduled thread cycles; TSC/QPC %.6f cycles/ns, "
		"spread %.4f%%; scaled CPU times are estimates\n", cpu_per_ns, (hi / lo - 1) * 100);
	require(hi / lo < 1.001, "stable cycle calibration");
#else
	puts("clock: POSIX thread CPU nanoseconds");
#endif
	require(clin_init() && svin_init(), "RPC init");
	g9_drain_discard_publications = true; /* consume inline, no other workload thread */
	blocks_init(); items_init();
	region_archive_io_async_enable(false); /* no IO worker in the CPU bench */
	__atomic_store_n(&region_tps_observe, region_part, __ATOMIC_RELEASE);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	unsigned ticks = argc > 1 ? (unsigned)atoi(argv[1]) : 6300;
	require(ticks >= 1300 && ticks <= 54000, "live ticks 1300..54000");
	require(argc <= 4 && (argc < 3 || !strcmp(argv[2], "natural") || !strcmp(argv[2], "flat")),
		"live arguments: ticks [natural|flat [seed]]");
	int64_t seed = 4242;
	if(argc == 4) {
		char* end;
		errno = 0;
		seed = strtoll(argv[3], &end, 10);
		require(argv[3][0] && !*end && errno != ERANGE, "integer seed");
	}
	run_live(ticks, argc < 3 || !strcmp(argv[2], "flat"), seed);
#else
	for(unsigned rep = 0; rep < reps; rep++) {
		char dir[220], region[260];
		test_fixture_dir("bench_g9_checkpoint", dir, sizeof dir);
		MKDIR(dir);
		snprintf(region, sizeof region, "%s/region", dir); MKDIR(region);
		memset(&host, 0, sizeof host);
		/* IMP-TE-CAPS: growable tile slabs (all rows free) */
		if(!server_local_te_tables_init(&host))
			abort();
		dict_entity_init(host.entities);
		string_t level; string_init_set_str(level, dir);
		server_world_create(&host.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
		string_clear(level);
		struct server_world* w = &host.worlds[0];
		w->local = w->session = &host;
		host.scheduled_tick_budget = 128;
		host.rand_src.seed = 0x9E11u;
		host.world_time = 6000;
		server_local_skylight_recompute(&host);
		for(int x = -G9_DRAIN_RADIUS; x <= G9_DRAIN_RADIUS; x++)
			for(int z = -G9_DRAIN_RADIUS; z <= G9_DRAIN_RADIUS; z++) {
				struct server_chunk* sc;
				require(server_world_load_chunk(w, x, z, &sc) && sc, "load");
				clin_drain_discard();
				if(with_views) {
					require(sc->backing && view_n < BENCH_VIEWS, "view backing");
					views[view_n] = column_view_create(sc->backing, sc->ids,
						sc->metadata, sc->lighting_sky, sc->lighting_torch);
					require(views[view_n] != NULL, "view create");
					view_n++;
				}
			}
		uint8_t height;
		require(server_world_get_height(w, 8, 8, &height), "height");
		g9_drain_floor(w, height + 12);
		require(server_world_light_drain(w), "floor drain");
		for(int tick = 1; tick <= G9_DRAIN_TICKS; tick++) {
			g9_drain_edits(w, height + 12, tick);
			if(tick % 40 == 0) {
				memset(cpu, 0, sizeof cpu); memset(wall, 0, sizeof wall);
				memset(parts, 0, sizeof parts);
				uint64_t c = cpu_now(), t = tps_wall_now_ns();
				deflate_start = deflate_cpu; armed = true;
				require(server_world_save_chunk_coherent(w, -1, 1), "checkpoint");
				armed = false;
				cpu[0] = cpu_now() - c; wall[0] = tps_wall_now_ns() - t;
				observe_bytes();
				sample_deflate[n] = deflate_cpu - deflate_start;
				for(unsigned j = 0; j < 5; j++) {
					sample_cpu[j][n] = cpu[j]; sample_wall[j][n] = wall[j];
				}
				for(unsigned j = 0; j < TPS_PART_COUNT; j++) sample_parts[j][n] = parts[j];
				n++;
				require(server_world_sync_regions(w), "durable barrier");
			}
			require(server_world_light_drain(w), "normal drain");
		}
		uint32_t memo[4];
		region_archive_encode_cache_stats(w->checkpoint_encode_cache, memo);
		printf("rep %u: encode memo hits=%u misses=%u retained=%u bytes\n",
			rep, memo[0], memo[1], memo[2]);
		if(with_views) {
			struct column_view_stats vs;
			column_view_get_stats(&vs);
			printf("rep %u: %u views, view pages live=%u peak=%u failures=%u\n",
				rep, view_n, vs.pages, vs.peak_pages, vs.page_failures);
			while(view_n) column_view_unref(views[--view_n]);
		}
		dict_entity_clear(host.entities); server_world_discard(w);
		test_fixture_dir_cleanup(dir);
	}
#endif
	__atomic_store_n(&region_tps_observe, NULL, __ATOMIC_RELEASE);
	printf("zlib level=%d version=%s deflate_cpu_units=%" PRIu64 " deflate_cpu_us=%.3f payloads=%" PRIu64 " payload_bytes=%" PRIu64 " sector_bytes=%" PRIu64 "\n",
		level, zlibVersion(), deflate_cpu, (double)deflate_cpu / cpu_per_ns / 1000,
		payloads, payload_bytes, sector_bytes);
	uint64_t max_deflate = 0;
	for(unsigned i = 0; i < n; i++) if(sample_deflate[i] > max_deflate) max_deflate = sample_deflate[i];
	printf("deflate per-checkpoint median_us=%.3f max_us=%.3f (cold misses included)\n",
		(double)median(sample_deflate,n) / cpu_per_ns / 1000, (double)max_deflate / cpu_per_ns / 1000);
	const char* names[5] = {"total", "trial", "snapshot", "save", "restore"};
#ifdef CAVEWII_SERVER_LOCAL_TEST
	for(unsigned i = 0; i < n; i++) {
		printf("checkpoint %u:", i);
		for(unsigned j = 0; j < 5; j++)
			printf(" %s=%.3f", names[j], (double)sample_cpu[j][i] / cpu_per_ns / 1000);
		printf(" NBT+deflate=%.3f us\n",
			(double)sample_parts[TPS_REGION_COMPRESS][i] / cpu_per_ns / 1000);
	}
	for(unsigned i = 0; i < n; i++) {
		printf("checkpoint %u fn:", i);
		for(unsigned f = 0; f < FN_N; f++) if(fn_calls[f][i])
			printf(" %s=%.1fus/%" PRIu64 "c/%" PRIu64 "B", fn_name[f],
				(double)fn_cpu[f][i] / cpu_per_ns / 1000, fn_calls[f][i], fn_bytes[f][i]);
		printf(" | read=%.1f parse=%.1f preserve=%.1f compress=%.1f commit=%.1f deflate=%.1f\n",
			(double)sample_parts[TPS_REGION_READ][i] / cpu_per_ns / 1000,
			(double)sample_parts[TPS_REGION_PARSE][i] / cpu_per_ns / 1000,
			(double)sample_parts[TPS_REGION_PRESERVE][i] / cpu_per_ns / 1000,
			(double)sample_parts[TPS_REGION_COMPRESS][i] / cpu_per_ns / 1000,
			(double)sample_parts[TPS_REGION_COMMIT][i] / cpu_per_ns / 1000,
			(double)sample_deflate[i] / cpu_per_ns / 1000);
	}
#endif
	printf("%s checkpoint: median of %u; pre-drain, no fixture shift\n",
#ifdef CAVEWII_SERVER_LOCAL_TEST
		"test-engine real stress",
#else
		"production roof/torch",
#endif
		n);
	for(unsigned j = 0; j < 5; j++) {
		uint64_t c = median(sample_cpu[j], n), w = median(sample_wall[j], n);
		printf("  %-10s cpu_units=%" PRIu64 " cpu_us=%.3f wall_us=%.3f\n",
			names[j], c, (double)c / cpu_per_ns / 1000, (double)w / 1000);
	}
	const unsigned p[] = {TPS_REGION_READ, TPS_REGION_PARSE, TPS_REGION_PRESERVE,
		TPS_REGION_COMPRESS, TPS_REGION_COMMIT};
	const char* pn[] = {"read", "parse", "preserve", "NBT+deflate", "commit"};
	for(unsigned j = 0; j < 5; j++)
		printf("  save %-11s cpu_us=%.3f\n", pn[j],
			(double)median(sample_parts[p[j]], n) / cpu_per_ns / 1000);
	return 0;
}
