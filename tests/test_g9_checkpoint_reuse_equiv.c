/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_g9_checkpoint_reuse_equiv -- ASTRA-G9-R19-IO step 3 (g9-ckpt-1009).
 *
 * The settled-trial checkpoint (server_world_save_chunk_coherent /
 * server_world_checkpoint_chunks) snapshots the Sky/Block planes of every
 * chunk the pending drain can write, runs the trial drain, saves, and rolls
 * the live world back. The candidate captures each 64-cell page before its
 * first changing store and restores only captured pages, with an 8-byte diff
 * scan that issues the SAME stores in the SAME order (T1). This test pairs
 * the shipping path against the frozen per-cell reference
 * (server_world_test_light_trial_reference) on two identical worlds and,
 * after EVERY step, requires equality of:
 *   - every resident chunk's ids/meta/sky/block/heightmap and dirty flags,
 *     the column backing revisions, the light queue (seeds/undo/pins/
 *     frontier), the light fault state and the RNG;
 *   - the ordered light write trace, the ordered client publication trace,
 *     the census mutation counts and three COW column-view snapshots;
 *   - the saved region directory, every byte (timestamp stamp pinned).
 * Paths: pinned (pending) checkpoints, settled (nothing pending: no trial),
 * all-chunk checkpoints with the sync barrier, lazy/eager page capture,
 * exact-input compressed memo vs the real uncached codec, edits between the checkpoint
 * and the drain, residency changes (evict + reload) before a checkpoint,
 * snapshot allocation failure + retry, and trial-drain OOM (stopped world)
 * + retry. No settled result is cached or reused across checkpoints.
 *
 * It also prints the host single-thread checkpoint split per arm (median
 * over the pinned checkpoints) from the R5 ckpt window record.
 */

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "support/g9_frozen_flood.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "column_view.h"
#include "item/items.h"
#include "lighting.h"
#include "network/client_interface.h"
#include "network/region_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/g9_drain_workload.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "tps_observer.h"
#include "util.h"

#ifndef CAVEWII_SERVER_LOCAL_TEST
#error "test_g9_checkpoint_reuse_equiv needs the test engine pools"
#endif

#define RADIUS G9_DRAIN_RADIUS
#define CLOCK_STAMP 0x5F5E1234u

static struct server_local s_world[2]; /* 0 = shipping, 1 = reference */
static char s_dir[2][220];
static unsigned s_phase;
struct u32s { uint32_t* data; size_t n, cap; };
static struct u32s s_writes[2], s_pubs[2];
static uint64_t s_mut[2][5];
static struct column_view* s_views[2][3];
static uint8_t s_view_bytes[2][COLUMN_VIEW_PRIVATE_BYTES];
static int s_y;
static unsigned s_steps;
#define TIMED_MAX 64
static uint32_t s_t_snap[2][TIMED_MAX], s_t_restore[2][TIMED_MAX],
	s_t_total[2][TIMED_MAX], s_t_drain[2][TIMED_MAX], s_t_save[2][TIMED_MAX];
static unsigned s_timed;

static void push(struct u32s* b, const uint32_t* v, size_t n) {
	if(b->n + n > b->cap) {
		size_t cap = (b->n + n) * 2;
		void* p = realloc(b->data, cap * sizeof *b->data);
		assert(p);
		b->data = p;
		b->cap = cap;
	}
	memcpy(b->data + b->n, v, n * sizeof *v);
	b->n += n;
}
static void on_write(w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	const uint32_t v[4] = {(uint32_t)x, (uint32_t)y, (uint32_t)z, light};
	push(&s_writes[s_phase], v, 4);
}
static void on_mutation(unsigned kind) {
	assert(kind < 5);
	s_mut[s_phase][kind]++;
}
static void on_rpc(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	uint32_t v[8] = {rpc->type, 0, 0, 0, 0, 0, 0, 0};
	if(rpc->type == CRPC_SET_BLOCK) {
		v[1] = (uint32_t)rpc->payload.set_block.x;
		v[2] = (uint32_t)rpc->payload.set_block.y;
		v[3] = (uint32_t)rpc->payload.set_block.z;
		v[4] = rpc->payload.set_block.block.type;
		v[5] = rpc->payload.set_block.block.metadata;
		v[6] = rpc->payload.set_block.block.sky_light;
		v[7] = rpc->payload.set_block.block.torch_light;
	}
	push(&s_pubs[s_phase], v, 8);
}
static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static void open_world(unsigned k) {
	test_fixture_dir(k ? "g9_ckpt_reuse_ref" : "g9_ckpt_reuse_new", s_dir[k],
					 sizeof s_dir[k]);
	MKDIR(s_dir[k]);
	char region[260];
	snprintf(region, sizeof region, "%s/region", s_dir[k]);
	MKDIR(region);
	struct server_local* host = &s_world[k];
	memset(host, 0, sizeof *host);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(host))
		abort();
	dict_entity_init(host->entities);
	string_t level;
	string_init_set_str(level, s_dir[k]);
	server_world_create(&host->worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	host->worlds[0].session = host;
	host->worlds[0].local = host;
	host->worlds[0].world_index = 0;
	host->scheduled_tick_budget = 128;
	host->rand_src.seed = 0x9E11u;
	host->world_time = 6000;
	server_local_skylight_recompute(host);
	for(int cx = -RADIUS; cx <= RADIUS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++) {
			struct server_chunk* sc = NULL;
			bool ok = server_world_load_chunk(&host->worlds[0], cx, cz, &sc);
			assert(ok && sc);
			(void)ok;
		}
}
static struct server_world* W(unsigned k) { return &s_world[k].worlds[0]; }

static void make_views(void) {
	static const int at[3][2] = {{-1, 1}, {0, 1}, {1, 1}};
	for(unsigned k = 0; k < 2; k++)
		for(unsigned v = 0; v < 3; v++) {
			struct server_chunk* sc = dict_server_chunks_get(
				W(k)->chunks, S_CHUNK_ID(at[v][0], at[v][1]));
			assert(sc && sc->backing);
			s_views[k][v] = column_view_create(sc->backing, sc->ids, sc->metadata,
				sc->lighting_sky, sc->lighting_torch);
			assert(s_views[k][v]);
		}
}
static void drop_views(void) {
	for(unsigned k = 0; k < 2; k++)
		for(unsigned v = 0; v < 3; v++) {
			column_view_unref(s_views[k][v]);
			s_views[k][v] = NULL;
		}
}

static void compare_traces(void) {
	assert(s_writes[0].n == s_writes[1].n);
	if(s_writes[0].n)
		assert(!memcmp(s_writes[0].data, s_writes[1].data,
					   s_writes[0].n * sizeof(uint32_t)));
	if(s_pubs[0].n != s_pubs[1].n)
		printf("step %u: publications %zu vs %zu\n", s_steps, s_pubs[0].n / 8,
			   s_pubs[1].n / 8);
	assert(s_pubs[0].n == s_pubs[1].n);
	if(s_pubs[0].n)
		assert(!memcmp(s_pubs[0].data, s_pubs[1].data,
					   s_pubs[0].n * sizeof(uint32_t)));
	s_writes[0].n = s_writes[1].n = s_pubs[0].n = s_pubs[1].n = 0;
	assert(!memcmp(s_mut[0], s_mut[1], sizeof s_mut[0]));
	memset(s_mut, 0, sizeof s_mut);
	for(unsigned v = 0; v < 3; v++) {
		for(unsigned k = 0; k < 2; k++)
			(void)column_view_copy(s_views[k][v], s_view_bytes[k],
				s_view_bytes[k] + 32768, s_view_bytes[k] + 49152,
				s_view_bytes[k] + 65536);
		assert(!memcmp(s_view_bytes[0], s_view_bytes[1], sizeof s_view_bytes[0]));
	}
}

static void compare_worlds(void) {
	struct server_world* a = W(0);
	struct server_world* b = W(1);
	assert(dict_server_chunks_size(a->chunks) == dict_server_chunks_size(b->chunks));
	for(int cx = -RADIUS - 1; cx <= RADIUS + 1; cx++)
		for(int cz = -RADIUS - 1; cz <= RADIUS + 1; cz++) {
			struct server_chunk* ac = dict_server_chunks_get(a->chunks, S_CHUNK_ID(cx, cz));
			struct server_chunk* bc = dict_server_chunks_get(b->chunks, S_CHUNK_ID(cx, cz));
			assert(!ac == !bc);
			if(!ac)
				continue;
			assert(!ac->light_trial_capture && !bc->light_trial_capture);
			assert(!memcmp(ac->ids, bc->ids, 32768));
			assert(!memcmp(ac->metadata, bc->metadata, 16384));
			assert(!memcmp(ac->lighting_sky, bc->lighting_sky, 16384));
			assert(!memcmp(ac->lighting_torch, bc->lighting_torch, 16384));
			assert(!memcmp(ac->heightmap, bc->heightmap, 256));
			assert(ac->modified == bc->modified && ac->save_pending == bc->save_pending);
			assert(!ac->backing == !bc->backing);
			if(ac->backing) {
				/* owner/incarnation are process-global identities (two
				 * worlds in one process); the per-column counters must match */
				const struct column_revision* ra = &ac->backing->revision;
				const struct column_revision* rb = &bc->backing->revision;
				if(ra->content != rb->content || ra->light != rb->light
				   || ra->events != rb->events)
					printf("step %u chunk %d,%d rev c%u/%u l%u/%u e%u/%u\n",
						   s_steps, cx, cz, ra->content, rb->content, ra->light,
						   rb->light, ra->events, rb->events);
				assert(ra->content == rb->content && ra->light == rb->light
					   && ra->events == rb->events);
				assert(ac->backing->settled_light_revision
					   == bc->backing->settled_light_revision);
			}
		}
	assert(s_world[0].rand_src.seed == s_world[1].rand_src.seed);
	assert(a->light_frontier_live == b->light_frontier_live);
	assert(a->light_fault == b->light_fault
		   && a->light_fault_reason == b->light_fault_reason);
	assert(a->light_seed_n == b->light_seed_n && a->light_undo_n == b->light_undo_n
		   && a->light_pin_n == b->light_pin_n);
	for(size_t i = 0; i < a->light_seed_n; i++)
		assert(a->light_seeds[i].x == b->light_seeds[i].x
			   && a->light_seeds[i].y == b->light_seeds[i].y
			   && a->light_seeds[i].z == b->light_seeds[i].z);
	for(size_t i = 0; i < a->light_undo_n; i++) {
		assert(a->light_undo[i].x == b->light_undo[i].x
			   && a->light_undo[i].z == b->light_undo[i].z);
		assert(!memcmp(a->light_undo[i].sky, b->light_undo[i].sky, WORLD_HEIGHT / 2));
	}
	if(a->light_pin_n)
		assert(!memcmp(a->light_pins, b->light_pins,
					   a->light_pin_n * sizeof *a->light_pins));
	assert(a->light_frontier.index == b->light_frontier.index);
	for(size_t i = 0; i < a->light_frontier.index; i++) {
		struct g9_frozen_entry ae, be;
		stack_at(&a->light_frontier, &ae, i);
		stack_at(&b->light_frontier, &be, i);
		assert(ae.x == be.x && ae.y == be.y && ae.z == be.z && ae.forced == be.forced);
	}
}

static unsigned char* slurp(const char* path, size_t* n) {
	FILE* f = fopen(path, "rb");
	assert(f);
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* p = malloc(len > 0 ? (size_t)len : 1);
	assert(p);
	size_t got = len > 0 ? fread(p, 1, (size_t)len, f) : 0;
	assert(got == (size_t)(len > 0 ? len : 0));
	fclose(f);
	*n = got;
	return p;
}
/* Every file of both region directories, every byte. */
static unsigned compare_saves(void) {
	char rd[2][260];
	for(unsigned k = 0; k < 2; k++)
		snprintf(rd[k], sizeof rd[k], "%s/region", s_dir[k]);
	unsigned files = 0;
	for(unsigned pass = 0; pass < 2; pass++) {
		DIR* d = opendir(rd[pass]);
		assert(d);
		struct dirent* e;
		while((e = readdir(d))) {
			if(e->d_name[0] == '.')
				continue;
			char p[2][400];
			for(unsigned k = 0; k < 2; k++)
				snprintf(p[k], sizeof p[k], "%s/%s", rd[k], e->d_name);
			size_t n0, n1;
			unsigned char* b0 = slurp(p[0], &n0);
			unsigned char* b1 = slurp(p[1], &n1);
			assert(n0 == n1 && (n0 == 0 || !memcmp(b0, b1, n0)));
			free(b0);
			free(b1);
			if(pass == 0)
				files++;
		}
		closedir(d);
	}
	return files;
}

/* Run op on both worlds (reference loops on world 1), then compare all. */
typedef bool (*op_fn)(struct server_world* w, void* arg);
static bool run_pair(op_fn op, void* arg, bool timed) {
	bool r[2];
	for(unsigned k = 0; k < 2; k++) {
		s_phase = k;
		server_world_test_light_trial_reference(k == 1);
		struct server_world_light_drain_hb hb;
		server_world_light_drain_hb_take(&hb); /* clear the window */
		r[k] = op(W(k), arg);
		server_world_light_drain_hb_take(&hb);
		server_world_test_light_trial_reference(false);
		if(timed && hb.ckpt.n && s_timed < TIMED_MAX) {
			s_t_snap[k][s_timed] = hb.ckpt.snap_us;
			s_t_restore[k][s_timed] = hb.ckpt.restore_us;
			s_t_total[k][s_timed] = hb.ckpt.us;
			s_t_drain[k][s_timed] = hb.ckpt.drain_us;
			s_t_save[k][s_timed] = hb.ckpt.save_us;
		}
	}
	if(timed && s_timed < TIMED_MAX && s_t_total[0][s_timed])
		s_timed++;
	s_phase = 0;
	assert(r[0] == r[1]);
	compare_traces();
	compare_worlds();
	s_steps++;
	return r[0];
}

struct xz { int x, z; };
static bool op_edits(struct server_world* w, void* arg) {
	g9_drain_edits(w, s_y, *(int*)arg);
	return true;
}
static bool op_floor(struct server_world* w, void* arg) {
	(void)arg;
	g9_drain_floor(w, s_y);
	return true;
}
static bool op_drain(struct server_world* w, void* arg) {
	(void)arg;
	return server_world_light_drain(w);
}
static bool op_coherent(struct server_world* w, void* arg) {
	const struct xz* c = arg;
	return server_world_save_chunk_coherent(w, c->x, c->z);
}
static bool op_stress_checkpoint(struct server_world* w, void* arg) {
	const int t = *(int*)arg;
	if(t & 1) server_world_save_batch_begin(w);
	struct server_local* s = w->session;
	s->light_queue_bx = -16;
	s->light_queue_by = s_y;
	s->light_queue_bz = 16;
	s->light_queue_tick = 600;
	server_local_test_seed_math_random(0x600);
	server_local_test_light_queue_stress_drive(s);
	assert(s->light_queue_tick == 601);
	assert(server_world_save_batch_end(w));
	return true;
}
static bool op_checkpoint_all(struct server_world* w, void* arg) {
	(void)arg;
	bool ok = server_world_checkpoint_chunks(w);
	return server_world_sync_regions(w) && ok;
}
static bool op_sync(struct server_world* w, void* arg) {
	(void)arg;
	return server_world_sync_regions(w);
}
static bool op_extra_edit(struct server_world* w, void* arg) {
	const int t = *(int*)arg;
	return server_world_set_block(w, -16 + 2 + (t / 40) % 4, s_y + 2, 16 + 9,
		(struct block_data){.type = (t / 40) & 1 ? BLOCK_GLOWSTONE : BLOCK_AIR});
}
static bool op_page_edge(struct server_world* w, void* arg) {
	unsigned n = *(unsigned*)arg;
	static const unsigned y[] = {0, 63, 64, 127};
	return server_world_set_block(w, 31, y[n / 2], 31,
		(struct block_data){.type = n & 1 ? BLOCK_AIR : BLOCK_GLOWSTONE});
}
static bool op_evict_reload(struct server_world* w, void* arg) {
	const struct xz* c = arg;
	if(!server_world_save_chunk(w, true, c->x, c->z))
		return false;
	assert(!server_world_is_chunk_loaded(w, c->x, c->z));
	struct server_chunk* sc = NULL;
	return server_world_load_chunk(w, c->x, c->z, &sc) && sc;
}
static bool op_fail_snapshot(struct server_world* w, void* arg) {
	server_world_test_light_trial_fail(1);
	bool ok = op_coherent(w, arg);
	server_world_test_light_trial_fail(0);
	return ok;
}
static bool op_eager_snapshot(struct server_world* w, void* arg) {
	server_world_test_light_trial_eager(true); /* optional metadata OOM fallback */
	bool ok = op_coherent(w, arg);
	server_world_test_light_trial_eager(false);
	return ok;
}
static bool op_fail_trial_drain(struct server_world* w, void* arg) {
	lighting_test_drain_fail_reserve_after(1);
	bool ok = op_coherent(w, arg);
	lighting_test_drain_fail_reserve_after(-1);
	return ok;
}
static bool op_clear_fault(struct server_world* w, void* arg) {
	(void)arg;
	server_world_test_light_clear_fault(w);
	return true;
}

static int cmp_u32(const void* a, const void* b) {
	uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
	return x < y ? -1 : x > y;
}
static uint32_t median(uint32_t* v, unsigned n) {
	if(!n)
		return 0;
	uint32_t tmp[TIMED_MAX];
	memcpy(tmp, v, n * sizeof *v);
	qsort(tmp, n, sizeof *tmp, cmp_u32);
	return tmp[n / 2];
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;
	setvbuf(stdout, NULL, _IONBF, 0);
	bool ok = clin_init() && svin_init();
	assert(ok);
	struct thread drain;
	ok = thread_create(&drain, drain_entry, NULL, 64);
	assert(ok);
	(void)ok;
	blocks_init();
	items_init();
	region_archive_test_clock(CLOCK_STAMP);
	open_world(0);
	open_world(1);
	server_world_test_light_write_hook = on_write;
	__atomic_store_n(&world_tps_mutation, on_mutation, __ATOMIC_RELEASE);
	clin_test_set_rpc_observer(on_rpc, NULL);
	make_views();
	uint8_t h = 0;
	ok = server_world_get_height(W(0), 8, 8, &h);
	assert(ok);
	s_y = h + 12;
	run_pair(op_floor, NULL, false);
	run_pair(op_drain, NULL, false);

	const struct xz roof = {(-16 + 8) >> 4, (16 + 8) >> 4};
	const struct xz far = {RADIUS, -RADIUS}; /* outside every pin's 3x3 */
	unsigned pinned = 0, settled = 0, failed = 0, files = 0;
	for(int t = 1; t <= G9_DRAIN_TICKS; t++) {
		run_pair(op_edits, &t, false);
		if(t % 40 == 0) {
			const int v = (t / 40) % 6;
			if(v == 2) /* residency change before the checkpoint */
				run_pair(op_evict_reload, (void*)&far, false);
			if(v == 4) { /* snapshot OOM: fails, nothing touched, retry */
				assert(server_world_light_pending(W(0)));
				assert(!run_pair(op_fail_snapshot, (void*)&roof, false));
				failed++;
			}
			if(v == 5) { /* trial-drain OOM: stopped world, then retry */
				assert(!run_pair(op_fail_trial_drain, (void*)&roof, false));
				run_pair(op_clear_fault, NULL, false);
				failed++;
			}
			assert(server_world_light_pending(W(0)));
			if(v == 0) assert(run_pair(op_eager_snapshot, (void*)&roof, false));
			if(v == 3)
				assert(run_pair(op_checkpoint_all, NULL, true));
			else
				assert(run_pair(op_coherent, (void*)&roof, true));
			pinned++;
			if(v == 1) /* an edit between the checkpoint and the drain */
				run_pair(op_extra_edit, &t, false);
			run_pair(op_sync, NULL, false);
			files = compare_saves();
		}
		assert(run_pair(op_drain, NULL, false));
		if(t % 120 == 60) { /* settled: nothing pending, no trial */
			assert(!server_world_light_pending(W(0)));
			assert(run_pair(op_coherent, (void*)&roof, false));
			run_pair(op_sync, NULL, false);
			files = compare_saves();
			settled++;
		}
	}
	/* The first/last cells and both sides of a lazy 64-cell page boundary,
	 * at a chunk corner/seam, with an active full COW snapshot. */
	const struct xz corner = {1, 1};
	for(unsigned edge = 0; edge < 8; edge++) {
		assert(run_pair(op_page_edge, &edge, false));
		assert(server_world_light_pending(W(0)));
		assert(run_pair(op_coherent, (void*)&corner, false));
		run_pair(op_sync, NULL, false);
		files = compare_saves();
		assert(run_pair(op_drain, NULL, false));
	}
	/* Additional real-caller steps, leaving the original OOM/golden workload
	 * intact. Compare live queues/flags/RNG/COW observations at every step,
	 * and ALL region bytes/order at each caller's own covering barrier. */
	for(int t = 0; t < 12; t++) {
		assert(run_pair(op_edits, &t, false));
		if(t % 3 == 0) assert(run_pair(op_evict_reload, (void*)&far, false));
		assert(run_pair(op_stress_checkpoint, &t, true));
		files = compare_saves();
		run_pair(op_extra_edit, &t, false); /* unchanged block legitimately returns false */
		assert(run_pair(op_drain, NULL, false));
	}
	drop_views();
	uint32_t memo[4];
	region_archive_encode_cache_stats(W(0)->checkpoint_encode_cache, memo);
	assert(memo[0] > 0 && memo[1] > 0 && memo[2] <= 1133 * 1024);
	assert(!W(1)->checkpoint_encode_cache); /* real uncached codec reference */
	printf("exact-input checkpoint memo: hits %u misses %u bytes %u\n",
		memo[0], memo[1], memo[2]);
	clin_test_set_rpc_observer(NULL, NULL);
	__atomic_store_n(&world_tps_mutation, NULL, __ATOMIC_RELEASE);
	server_world_test_light_write_hook = NULL;
	region_archive_test_clock(0);

	printf("steps %u: pinned checkpoints %u, settled %u, injected failures %u; "
		   "region files compared %u\n", s_steps, pinned, settled, failed, files);
	assert(pinned >= 30 && settled >= 10 && failed >= 8 && files >= 1);
	printf("host checkpoint split, median of %u pinned checkpoints (us), "
		   "shipping vs reference:\n", s_timed);
	const char* names[5] = {"total", "snapshot", "trial drain", "save", "restore"};
	uint32_t(*cols[5])[TIMED_MAX] = {s_t_total, s_t_snap, s_t_drain, s_t_save,
									 s_t_restore};
	for(unsigned c = 0; c < 5; c++)
		printf("  %-12s %8u vs %8u\n", names[c], median(cols[c][0], s_timed),
			   median(cols[c][1], s_timed));
	for(unsigned k = 0; k < 2; k++) {
		dict_entity_clear(s_world[k].entities);
		server_world_discard(W(k));
		test_fixture_dir_cleanup(s_dir[k]);
	}
	puts("test_g9_checkpoint_reuse_equiv: PASS (lazy/eager/uncached/"
		 "pinned/settled/all-chunk/edit/residency/OOM-retry checkpoints: worlds, queues, RNG, ordered "
		 "writes + publications, census, COW views and every saved byte equal "
		 "to the per-cell reference)");
	return 0;
}
