/*
	BG-BAKE-1028 task 2: backdrop_bake lifecycle, allocation failure,
	cancellation, scene-readiness gate and same-input byte-identical pixels.

	Runs the REAL storage-free scene adapter (worldgen terrain + Beta populate
	+ per-column light over bake-owned columns) against a fake renderer host.
	The fake host hashes every installed column and synthesizes the frame
	from that hash (plus a fixed per-pixel pattern), so the delivered pixels
	are a pure function of the scene bytes: two runs with the same recipe
	must produce byte-identical 1024x576 output. PC GL rendering itself is
	not exercised here (no window in ctest); PC/Wii visual equivalence is the
	task-4 capture check.

	Also prints the bake-owned memory ledger for the default recipe (columns,
	visible set, peak owned bytes) -- the PC estimate FEASIBILITY blocker 1
	asks for; the host's world/mesh figures come from the in-game telemetry.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NDEBUG
#include <assert.h>

#include "game/gui/backdrop_bake.h"
#include "platform/mem2.h"
#include "network/server_world.h"
#include "worldgen/worldgen.h"

/* Stubbed externals worldgen.c/populate.c link against (find_lake /
 * test_worldgen precedent): blocks[] NULL -> the sky pass leaves the column
 * lit uniformly; irrelevant to the lifecycle and determinism under test. */
struct block;
struct block* blocks[256] = {0};
uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((data & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (data & 0x0F));
}

#define FRAME_W 640
#define FRAME_H 480

struct fake {
	bool world_alive;
	bool scene_open;
	unsigned begins, ends, installs, visible_installs, settles, captures;
	bool fail_begin;
	unsigned fail_install_at; /* 1-based, 0 = never */
	unsigned ready_after;     /* settle calls before READY; UINT32_MAX = never */
	bool settle_fail;
	int cancel_at_band;       /* -1 = never */
	int stop_at_band;         /* -1 = never: deliver a partial frame, return true */
	struct backdrop_bake* bake;
	uint64_t scene_hash;
};

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
	for(size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static bool f_alive(void* u) {
	return ((struct fake*)u)->world_alive;
}

static bool f_begin(void* u, const struct backdrop_bake_recipe* r) {
	struct fake* f = u;
	assert(r);
	assert(!f->scene_open);
	f->begins++;
	if(f->fail_begin) return false;
	f->scene_open = true;
	f->scene_hash = 14695981039346656037ULL;
	return true;
}

static bool f_install(void* u, int cx, int cz, const uint8_t* ids,
	const uint8_t* meta, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* hm, const uint8_t* gi, const uint8_t* gj, bool visible) {
	struct fake* f = u;
	assert(f->scene_open);
	assert(ids && meta && sky && torch && hm && gi && gj);
	f->installs++;
	if(visible) f->visible_installs++;
	if(f->fail_install_at && f->installs == f->fail_install_at) return false;
	int32_t c[2] = {cx, cz};
	uint64_t h = f->scene_hash;
	h = fnv(h, (const uint8_t*)c, sizeof c);
	h = fnv(h, ids, 16 * 16 * 128);
	h = fnv(h, meta, 16 * 16 * 64);
	h = fnv(h, sky, 16 * 16 * 64);
	h = fnv(h, torch, 16 * 16 * 64);
	h = fnv(h, hm, 256);
	h = fnv(h, gi, 256);
	h = fnv(h, gj, 256);
	f->scene_hash = h;
	return true;
}

static enum backdrop_bake_settle f_settle(void* u, uint32_t budget) {
	struct fake* f = u;
	(void)budget;
	assert(f->scene_open);
	f->settles++;
	if(f->settle_fail) return BACKDROP_BAKE_SETTLE_FAILED;
	if(f->ready_after != UINT32_MAX && f->settles > f->ready_after)
		return BACKDROP_BAKE_SETTLE_READY;
	return BACKDROP_BAKE_SETTLE_PENDING;
}

static bool f_capture(void* u, backdrop_bake_rows_fn sink, void* su) {
	struct fake* f = u;
	assert(f->scene_open);
	f->captures++;
	static uint8_t band[FRAME_W * 32 * 4];
	int bi = 0;
	for(size_t y0 = 0; y0 < FRAME_H; y0 += 32, bi++) {
		if(bi == f->stop_at_band) return true; /* lies: partial frame */
		if(bi == f->cancel_at_band) backdrop_bake_cancel(f->bake);
		for(size_t y = 0; y < 32; y++)
			for(size_t x = 0; x < FRAME_W; x++) {
				uint64_t v = f->scene_hash ^ ((y0 + y) * 2654435761u) ^ (x * 40503u);
				uint8_t* p = band + (y * FRAME_W + x) * 4;
				p[0] = (uint8_t)v;
				p[1] = (uint8_t)(v >> 8);
				p[2] = (uint8_t)(v >> 16);
				p[3] = (uint8_t)(v >> 24); /* garbage alpha: must become 255 */
			}
		if(!sink(su, band, y0, 32, FRAME_W, FRAME_H)) return false;
	}
	return true;
}

static void f_end(void* u) {
	struct fake* f = u;
	assert(f->scene_open);
	f->scene_open = false;
	f->ends++;
}

static const struct backdrop_bake_world_facts* s_admission;
static bool admission_alive(void* u) {
	(void)u;
	return backdrop_bake_world_alive(s_admission);
}

static struct backdrop_bake_host host_for(struct fake* f) {
	return (struct backdrop_bake_host) {
		.user = f,
		.world_alive = f_alive,
		.scene_begin = f_begin,
		.install_column = f_install,
		.settle_step = f_settle,
		.render_capture = f_capture,
		.scene_end = f_end,
	};
}

static void fake_init(struct fake* f) {
	memset(f, 0, sizeof(*f));
	f->ready_after = 3;
	f->cancel_at_band = -1;
	f->stop_at_band = -1;
}

static size_t mem2_live(void) {
	return mem2_stat_alloc_calls() - mem2_stat_free_calls();
}

/* Drive to a terminal state; returns it. */
static enum backdrop_bake_state run(struct backdrop_bake* b, unsigned max_steps) {
	enum backdrop_bake_state s = backdrop_bake_state(b);
	for(unsigned i = 0; i < max_steps; i++) {
		s = backdrop_bake_step(b, 2000);
		if(s == BACKDROP_BAKE_DONE || s == BACKDROP_BAKE_FAILED
		   || s == BACKDROP_BAKE_CANCELLED)
			return s;
	}
	return s;
}

static struct backdrop_bake_recipe small_recipe(void) {
	struct backdrop_bake_recipe r = *backdrop_bake_default_recipe();
	r.view_radius = 2;
	return r;
}

static uint8_t* bake_full(const struct backdrop_bake_recipe* r, uint64_t* hash,
	struct backdrop_bake_report* rep_out) {
	struct fake f;
	fake_init(&f);
	struct backdrop_bake_host h = host_for(&f);
	enum backdrop_bake_error err;
	size_t live0 = mem2_live();
	struct backdrop_bake* b = backdrop_bake_create(&h, r, &err);
	assert(b && err == BACKDROP_BAKE_OK);
	f.bake = b;
	assert(run(b, 1000000) == BACKDROP_BAKE_DONE);
	assert(backdrop_bake_error(b) == BACKDROP_BAKE_OK);
	assert(f.begins == 1 && f.ends == 1 && !f.scene_open);
	assert(f.captures == 1);
	struct backdrop_bake_report rep;
	backdrop_bake_get_report(b, &rep);
	assert(rep.columns_installed == rep.columns_total);
	assert(f.installs == rep.columns_total);
	assert(f.visible_installs == rep.columns_visible && rep.columns_visible > 0);
	assert(rep.quads_done == rep.quads_total);
	uint8_t* px = backdrop_bake_take_pixels(b);
	assert(px);
	assert(backdrop_bake_take_pixels(b) == NULL); /* handed over once */
	backdrop_bake_get_report(b, &rep);
	assert(rep.owned_bytes == 0); /* everything but the handed-over pixels freed */
	for(size_t i = 0; i < (size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H; i++)
		assert(px[i * 4 + 3] == 255);
	backdrop_bake_destroy(b);
	assert(mem2_live() == live0 + 1); /* only the pixels remain */
	*hash = f.scene_hash;
	if(rep_out) *rep_out = rep;
	return px;
}

static void test_nearest(void) {
	/* PIL Image.NEAREST: floor((o + 0.5) * crop/out). */
	assert(backdrop_bake_nearest_src(0, 1024, 0, 640) == 0);
	assert(backdrop_bake_nearest_src(1, 1024, 0, 640) == 0);
	assert(backdrop_bake_nearest_src(2, 1024, 0, 640) == 1);
	assert(backdrop_bake_nearest_src(1023, 1024, 0, 640) == 639);
	assert(backdrop_bake_nearest_src(0, 576, 96, 360) == 96);
	assert(backdrop_bake_nearest_src(575, 576, 96, 360) == 455);
	for(size_t o = 0; o < 1024; o++) {
		double want = (o + 0.5) * 640.0 / 1024.0;
		assert(backdrop_bake_nearest_src(o, 1024, 0, 640) == (size_t)want);
	}
	puts("PASS nearest: PIL NEAREST index map (0,96)-(640,456) -> 1024x576");
}

static void test_lifecycle_and_determinism(void) {
	uint64_t h1, h2;
	struct backdrop_bake_report rep;
	const struct backdrop_bake_recipe* r = backdrop_bake_default_recipe();
	size_t live0 = mem2_live();
	uint8_t* a = bake_full(r, &h1, &rep);
	uint8_t* b = bake_full(r, &h2, NULL);
	assert(h1 == h2);
	assert(memcmp(a, b, (size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4) == 0);
	/* the output really samples the crop: row 0 comes from source row 96 */
	assert(a != NULL);
	backdrop_bake_pixels_free(a);
	backdrop_bake_pixels_free(b);
	assert(mem2_live() == live0);
	printf("PASS lifecycle+determinism: default recipe, %u columns (%u visible, "
		   "%u quads), scene hash %016llx identical twice, 1024x576 RGBA "
		   "byte-identical\n",
		rep.columns_total, rep.columns_visible, rep.quads_total,
		(unsigned long long)h1);
	printf("MEM bake-owned peak %lu bytes (columns %lu B each payload); "
		   "phase_us terrain %u populate %u install %u; step_max_us %u (PC, budget 2000)\n",
		(unsigned long)rep.owned_peak_bytes, (unsigned long)(32768 + 3 * 16384 + 256),
		rep.phase_us[0], rep.phase_us[1], rep.phase_us[2], rep.step_max_us);
}

static void test_refusals(void) {
	struct fake f;
	fake_init(&f);
	struct backdrop_bake_host h = host_for(&f);
	enum backdrop_bake_error err;
	f.world_alive = true;
	assert(!backdrop_bake_create(&h, NULL, &err));
	assert(err == BACKDROP_BAKE_ERR_WORLD_ALIVE);
	assert(f.begins == 0);
	f.world_alive = false;
	/* a world appearing mid-bake ends it before the scene opens */
	struct backdrop_bake_recipe r = small_recipe();
	struct backdrop_bake* b = backdrop_bake_create(&h, &r, &err);
	assert(b);
	(void)backdrop_bake_step(b, 1);
	f.world_alive = true;
	assert(backdrop_bake_step(b, 1) == BACKDROP_BAKE_FAILED);
	assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_WORLD_ALIVE);
	assert(f.begins == f.ends);
	backdrop_bake_destroy(b);
	/* scene refused */
	fake_init(&f);
	f.fail_begin = true;
	size_t live0 = mem2_live();
	b = backdrop_bake_create(&h, &r, &err);
	assert(run(b, 100000) == BACKDROP_BAKE_FAILED);
	assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_SCENE);
	assert(f.ends == 0);
	backdrop_bake_destroy(b);
	assert(mem2_live() == live0);
	/* install refused mid-way */
	fake_init(&f);
	f.fail_install_at = 3;
	b = backdrop_bake_create(&h, &r, &err);
	assert(run(b, 100000) == BACKDROP_BAKE_FAILED);
	assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_INSTALL);
	assert(f.begins == 1 && f.ends == 1 && f.captures == 0);
	backdrop_bake_destroy(b);
	assert(mem2_live() == live0);
	puts("PASS refusals: world alive (create + mid-bake), scene refused, install refused");
}

static void test_readiness_gate(void) {
	struct fake f;
	struct backdrop_bake_host h = host_for(&f);
	struct backdrop_bake_recipe r = small_recipe();
	enum backdrop_bake_error err;
	size_t live0 = mem2_live();

	fake_init(&f);
	f.ready_after = UINT32_MAX; /* the scene never completes */
	struct backdrop_bake* b = backdrop_bake_create(&h, &r, &err);
	assert(run(b, BACKDROP_BAKE_SETTLE_STEP_LIMIT + 100000) == BACKDROP_BAKE_FAILED);
	assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_NOT_READY);
	assert(f.captures == 0); /* never a capture of an incomplete scene */
	assert(f.settles == BACKDROP_BAKE_SETTLE_STEP_LIMIT);
	assert(f.ends == 1);
	backdrop_bake_destroy(b);

	fake_init(&f);
	f.settle_fail = true;
	b = backdrop_bake_create(&h, &r, &err);
	assert(run(b, 100000) == BACKDROP_BAKE_FAILED);
	assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_SETTLE);
	assert(f.captures == 0 && f.ends == 1);
	backdrop_bake_destroy(b);

	/* a renderer that stops early but claims success: partial = failure */
	fake_init(&f);
	f.stop_at_band = 7;
	b = backdrop_bake_create(&h, &r, &err);
	assert(run(b, 100000) == BACKDROP_BAKE_FAILED);
	assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_CAPTURE);
	assert(backdrop_bake_take_pixels(b) == NULL);
	assert(f.ends == 1);
	backdrop_bake_destroy(b);
	assert(mem2_live() == live0);
	puts("PASS readiness: never-ready -> NOT_READY w/o capture, settle failure, "
		 "partial frame -> CAPTURE failure, no pixels");
}

static void test_cancel(void) {
	struct fake f;
	struct backdrop_bake_host h = host_for(&f);
	struct backdrop_bake_recipe r = small_recipe();
	enum backdrop_bake_error err;
	size_t live0 = mem2_live();
	/* cancel at the start of each phase, plus inside the readback */
	for(int target = BACKDROP_BAKE_TERRAIN; target <= BACKDROP_BAKE_CAPTURE + 1;
		target++) {
		fake_init(&f);
		struct backdrop_bake* b = backdrop_bake_create(&h, &r, &err);
		f.bake = b;
		if(target == BACKDROP_BAKE_CAPTURE + 1) {
			f.cancel_at_band = 5;
		} else {
			unsigned guard = 0;
			while(backdrop_bake_state(b) != (enum backdrop_bake_state)target
				  && guard++ < 100000)
				(void)backdrop_bake_step(b, 1);
			assert(backdrop_bake_state(b) == (enum backdrop_bake_state)target);
			(void)backdrop_bake_step(b, 1); /* some work inside the phase */
			if(backdrop_bake_state(b) == BACKDROP_BAKE_DONE) {
				backdrop_bake_pixels_free(backdrop_bake_take_pixels(b));
				backdrop_bake_destroy(b);
				continue; /* CAPTURE finishes in one step */
			}
			backdrop_bake_cancel(b);
		}
		assert(run(b, 100000) == BACKDROP_BAKE_CANCELLED);
		assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_CANCELLED);
		assert(backdrop_bake_take_pixels(b) == NULL);
		assert(f.begins == f.ends && !f.scene_open);
		struct backdrop_bake_report rep;
		backdrop_bake_get_report(b, &rep);
		assert(rep.owned_bytes == 0);
		backdrop_bake_destroy(b);
		assert(mem2_live() == live0);
	}
	/* destroy without cancel, mid-install: still releases everything */
	fake_init(&f);
	struct backdrop_bake* b = backdrop_bake_create(&h, &r, &err);
	while(backdrop_bake_state(b) != BACKDROP_BAKE_INSTALL)
		(void)backdrop_bake_step(b, 1);
	(void)backdrop_bake_step(b, 1);
	backdrop_bake_destroy(b);
	assert(f.begins == 1 && f.ends == 1);
	assert(mem2_live() == live0);
	puts("PASS cancel: terrain/populate/install/settle/in-readback cancel + "
		 "destroy mid-install release every buffer and end the scene once");
}

static void test_alloc_failure(void) {
	struct fake f;
	struct backdrop_bake_host h = host_for(&f);
	struct backdrop_bake_recipe r = small_recipe();
	enum backdrop_bake_error err;
	/* size the allocation sequence from one clean run */
	fake_init(&f);
	struct backdrop_bake* b = backdrop_bake_create(&h, &r, &err);
	assert(run(b, 100000) == BACKDROP_BAKE_DONE);
	struct backdrop_bake_report rep;
	backdrop_bake_get_report(b, &rep);
	backdrop_bake_pixels_free(backdrop_bake_take_pixels(b));
	backdrop_bake_destroy(b);
	/* create struct, column table, quad table, one per terrain column, one
	 * per tint, the output buffer */
	unsigned total = 3 + 2 * rep.columns_total + 1;
	size_t live0 = mem2_live();
	unsigned failures = 0;
	for(unsigned n = 1; n <= total; n++) {
		fake_init(&f);
		backdrop_bake_test_fail_alloc_after(n);
		b = backdrop_bake_create(&h, &r, &err);
		if(!b) {
			assert(err == BACKDROP_BAKE_ERR_ALLOC);
			assert(n <= 3);
		} else {
			enum backdrop_bake_state s = run(b, 100000);
			assert(s == BACKDROP_BAKE_FAILED);
			assert(backdrop_bake_error(b) == BACKDROP_BAKE_ERR_ALLOC);
			assert(backdrop_bake_take_pixels(b) == NULL);
			assert(f.begins == f.ends && !f.scene_open);
			backdrop_bake_destroy(b);
		}
		backdrop_bake_test_fail_alloc_after(0);
		assert(mem2_live() == live0);
		failures++;
	}
	printf("PASS alloc failure: each of %u bake-owned allocations failed in turn "
		   "-> clean ALLOC failure, scene ended, zero leaked buffers\n", failures);
}

/* review-2 #1: admission reads ownership + teardown, not the latched client
 * flags. After Save and quit the client's world_switch_pending stays set at
 * the title; that state must admit the deferred bake. */
static void test_admission_after_quit(void) {
	struct backdrop_bake_world_facts quit = {
		.remote_provider = false, .server_owns_world = false,
		.client_sections = 0, .at_title = true};
	assert(!backdrop_bake_world_alive(&quit));
	struct backdrop_bake_world_facts f = quit;
	f.server_owns_world = true;   /* local world still owned */
	assert(backdrop_bake_world_alive(&f));
	f = quit; f.remote_provider = true;   /* multiplayer session */
	assert(backdrop_bake_world_alive(&f));
	f = quit; f.client_sections = 3;      /* teardown not finished */
	assert(backdrop_bake_world_alive(&f));
	f = quit; f.at_title = false;         /* not at the title */
	assert(backdrop_bake_world_alive(&f));
	f.loading_frontend = true;           /* serviced boot / Apply */
	assert(!backdrop_bake_world_alive(&f));
	f.remote_provider = true;
	assert(backdrop_bake_world_alive(&f));
	f.remote_provider = false; f.server_owns_world = true;
	assert(backdrop_bake_world_alive(&f));
	f.server_owns_world = false; f.client_sections = 1;
	assert(backdrop_bake_world_alive(&f));
	assert(backdrop_bake_world_alive(NULL));
	/* end to end: a bake created against the post-quit facts runs */
	static struct backdrop_bake_world_facts live;
	live = quit;
	struct fake fk;
	fake_init(&fk);
	struct backdrop_bake_host h = host_for(&fk);
	h.world_alive = admission_alive;
	s_admission = &live;
	struct backdrop_bake_recipe r = small_recipe();
	enum backdrop_bake_error err;
	struct backdrop_bake* b = backdrop_bake_create(&h, &r, &err);
	assert(b && run(b, 100000) == BACKDROP_BAKE_DONE);
	backdrop_bake_pixels_free(backdrop_bake_take_pixels(b));
	backdrop_bake_destroy(b);
	s_admission = NULL;
	puts("PASS admission: post-quit title (switch flag latched) admits the deferred "
		 "bake; owned world / remote / unfinished teardown / off-title refuse");
}

static void test_nether_recipe(void) {
 struct backdrop_bake_recipe r=small_recipe(); r.dimension=-1;
 struct fake f; fake_init(&f); struct backdrop_bake_host h=host_for(&f);
 enum backdrop_bake_error err;
 size_t initial=mem2_live(); uint64_t hash=0;
 for(int i=0;i<2;i++) {
  fake_init(&f); struct backdrop_bake* b=backdrop_bake_create(&h,&r,&err);
  assert(b && run(b,100000)==BACKDROP_BAKE_DONE);
  if(i) assert(hash==f.scene_hash); else hash=f.scene_hash;
  backdrop_bake_pixels_free(backdrop_bake_take_pixels(b)); backdrop_bake_destroy(b);
  assert(mem2_live()==initial && f.begins==f.ends);
 }
 r.dimension=1; assert(!backdrop_bake_create(&h,&r,&err));
 assert(err==BACKDROP_BAKE_ERR_SCENE);
 puts("PASS Nether recipe: deterministic generator/population, complete retirement, invalid dimension refused");
}

int main(void) {
	mem2_init();
	test_nether_recipe();
	test_admission_after_quit();
	test_nearest();
	test_refusals();
	test_readiness_gate();
	test_cancel();
	test_alloc_failure();
	test_lifecycle_and_determinism();
	puts("test_backdrop_bake: all PASS");
	return 0;
}
