/* Copyright (c) 2026 CaveWii_v2 contributors
 * REL-FLK (FLICKER-1017, tmp/jobs/loop/FLICKER-1017/PLAN.md): a full-column
 * refresh of an already-visible column must never make that column vanish.
 *
 * Real server chunks (split fixture), real CRPC_CHUNK install through
 * clin_update_drain, real mesher worker held with chunk_mesher_test_hold_worker,
 * real world_pre_render_views / world_build_chunks / world_render_view /
 * world_render_completed. Only the GL leaf calls are link-wrapped: chunk_render
 * names the chunk being drawn and displaylist_render records every list that
 * chunk SUBMITS (bytes, flags) per column, per chunk, per pass, per frame.
 *
 * Contract, per case (owned/leased transitions, stale pre-refresh job,
 * repeated refresh, failed preflight, refresh while the last frame is still
 * pinned for the GPU, empty final snapshot):
 *  - while the worker is held, column (0,0) submits exactly its pre-refresh
 *    geometry every frame;
 *  - after release every frame shows, per chunk and pass, either that old
 *    geometry or the final one (no hole, no stale/intermediate publication);
 *  - once settled, its submissions and every list's bytes/flags equal a fresh
 *    client build of the identical snapshots at the same camera and LOD;
 *  - the install never destroys a list of the frame still pinned for the GPU;
 *    every backing reference returns to zero.
 *
 * Delayed GPU fence (the Wii case, no GL context needed): the "fence" cases
 * install the production displaylist_gpu_hooks with a FAKE GPU whose
 * completion lags FENCE_LAG sealed frames (gfx_finish seals one token per
 * frame; a reap runs at the next frame start, like gfx_flip_buffers). The
 * render wrap stamps submission exactly as dl_submit does (public fields), so
 * destroy -> dl_retire runs the real deferred-retirement path. The carve
 * observer sees every list free: freeing storage the fake GPU has not
 * completed yet is RED, as is a retirement queue that never drains or a main
 * thread stalled on the fence by a mesh swap.
 * Contract failures print "REL-FLK RED:" and exit 1 after all cases ran;
 * fixture/precondition failures print "REL-FLK FIXTURE:" and exit 2. */
#include "support/split_residency_fixture.h"
#include "carve_observer.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "game/camera.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/recipe.h"
#include "platform/displaylist.h"
#include "platform/displaylist_scene_hash.h"
#include "platform/gfx.h"
#include "world.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#define FIX(c) do { if(!(c)) { printf("REL-FLK FIXTURE: %s (line %d)\n", #c, __LINE__); exit(2); } } while(0)
#define CH COLUMN_HEIGHT
#define MAX_LOG 2048

static const char* s_case;
static unsigned s_red, s_case_red;
static void red(const char* fmt, ...) {
	s_red++;
	if(s_case_red++ >= 6) return;
	va_list a;
	va_start(a, fmt);
	printf("REL-FLK RED: [%s] ", s_case);
	vprintf(fmt, a);
	putchar('\n');
	va_end(a);
}

/* ---- GL leaves: no context in ctest. Everything above them is real. ---- */
void __wrap_gfx_fog(bool e) { (void)e; }
void __wrap_gfx_lighting(bool e) { (void)e; }
void __wrap_gfx_tint(bool e) { (void)e; }
void __wrap_gfx_alpha_test(bool e) { (void)e; }
void __wrap_gfx_alpha_test_ref(bool e, float r) { (void)e; (void)r; }
void __wrap_gfx_bind_texture(struct tex_gfx* t) { (void)t; }
void __wrap_gfx_blending(enum gfx_blend m) { (void)m; }
void __wrap_gfx_cull_func(enum cull_func f) { (void)f; }
void __wrap_gfx_write_buffers(bool c, bool d, bool t) { (void)c; (void)d; (void)t; }
void __wrap_gfx_matrix_modelview(mat4 m) { (void)m; }
void __wrap_gfx_matrix_texture(bool e, mat4 m) { (void)e; (void)m; }
void __wrap_gfx_fog_pos(float x, float z, float d) { (void)x; (void)z; (void)d; }

struct frame {
	unsigned draws[CH][2];
	uint64_t hash[CH][2];
};
static struct frame s_frame;
static struct chunk* s_cur;
static bool s_cur_pass;

static uint64_t list_hash(uint64_t h, const struct displaylist* l, unsigned k) {
	h = dl_scene_fnv64_u32(h, k);
	h = dl_scene_fnv64_u32(h, l->tinted);
	h = dl_scene_fnv64_u32(h, (uint32_t)l->index);
	return l->data ? dl_scene_fnv64(h, l->data, l->index * (l->tinted ? 25u : 22u)) : h;
}

void __real_chunk_render(struct chunk* c, bool pass, float x, float y, float z);
void __wrap_chunk_render(struct chunk* c, bool pass, float x, float y, float z) {
	s_cur = c;
	s_cur_pass = pass;
	__real_chunk_render(c, pass, x, y, z);
	s_cur = NULL;
}

/* Data pointers submitted in the last frame, still pinned for the GPU. */
static const void* s_pinned[CH * CHUNK_MESH_LISTS * 2];
static unsigned s_pinned_n, s_pin_destroyed;
static bool s_install_window;

/* ---- Delayed GPU fence model (fence cases only). Tokens are the production
 * displaylist_gpu_seal counter: one seal per frame, completion FENCE_LAG
 * frames later. All list storage ever submitted is tracked by data pointer
 * with its latest submit token; a CARVE_FREE of storage whose token the fake
 * GPU has not completed is a use-after-free on the real console. ---- */
#define FENCE_LAG 2u
#define SUBMIT_TABLE 16384u
static bool s_fence_mode;
static pthread_t s_main_thread;
static uint32_t s_issued, s_completed; /* full 32-bit tokens like the backend */
static struct { uint32_t token; unsigned frame; } s_sealed[64];
static unsigned s_sealed_n, s_frame_no, s_waits, s_fence_red, s_pending_max;
static struct { uintptr_t p; uint32_t token; } s_submitted[SUBMIT_TABLE];
static unsigned s_submitted_n;
static uint32_t s_submit_lock;

static void submit_lock(void) {
	while(__atomic_exchange_n(&s_submit_lock, 1, __ATOMIC_ACQUIRE)) {}
}
static void submit_unlock(void) { __atomic_store_n(&s_submit_lock, 0, __ATOMIC_RELEASE); }
static int submit_find(uintptr_t p) {
	for(unsigned i = 0; i < s_submitted_n; i++)
		if(s_submitted[i].p == p) return (int)i;
	return -1;
}
static bool token_done(uint32_t token) {
	return (uint32_t)(s_completed - token) < UINT32_C(0x80000000);
}
static bool fake_is_main(void) { return pthread_equal(pthread_self(), s_main_thread); }
static uint16_t fake_read(void) { return (uint16_t)s_completed; }
static void fake_seal(uint16_t token) {
	FIX(fake_is_main() && s_sealed_n < sizeof(s_sealed) / sizeof(*s_sealed));
	s_issued++;
	FIX((uint16_t)s_issued == token);
	s_sealed[s_sealed_n].token = s_issued;
	s_sealed[s_sealed_n++].frame = s_frame_no;
}
static void fence_complete_through(uint32_t token) {
	if(!token_done(token)) s_completed = token;
	unsigned keep = 0;
	for(unsigned i = 0; i < s_sealed_n; i++)
		if(!token_done(s_sealed[i].token)) s_sealed[keep++] = s_sealed[i];
	s_sealed_n = keep;
}
static void fake_wait(uint32_t token) {
	FIX(fake_is_main());
	s_waits++;
	fence_complete_through(token);
}
/* Frame start: the fake GPU finished every frame sealed FENCE_LAG frames ago. */
static void fence_frame_begin(void) {
	s_frame_no++;
	unsigned pending = displaylist_gpu_pending(); /* lists waiting on the fence */
	if(pending > s_pending_max) s_pending_max = pending;
	for(unsigned i = 0; i < s_sealed_n; i++)
		if(s_sealed[i].frame + FENCE_LAG <= s_frame_no) fence_complete_through(s_sealed[i].token);
	displaylist_gpu_reap();
}
static void fence_frame_end(void) {
	uint32_t before = s_issued;
	FIX(displaylist_gpu_seal() == s_issued && s_issued == before + 1);
}
static void fence_note_submit(const struct displaylist* l) {
	if(!s_fence_mode || !l->data) return;
	/* dl_submit's publication, on the public fields the backend fences on. */
	struct displaylist* w = (struct displaylist*)l;
	w->last_submit = s_issued + 1;
	w->submitted = true;
	submit_lock();
	int i = submit_find((uintptr_t)l->data);
	if(i < 0) {
		FIX(s_submitted_n < SUBMIT_TABLE);
		i = (int)s_submitted_n++;
		s_submitted[i].p = (uintptr_t)l->data;
	}
	s_submitted[i].token = w->last_submit;
	submit_unlock();
}
static void fence_observe(enum carve_action a, uintptr_t p, uintptr_t q, size_t n, size_t aux) {
	(void)q; (void)n; (void)aux;
	if(a != CARVE_FREE && a != CARVE_ALLOC && a != CARVE_RESIZE) return;
	submit_lock();
	int i = submit_find(p);
	if(i >= 0) {
		if(a == CARVE_FREE && !token_done(s_submitted[i].token)) {
			s_fence_red++;
			if(s_fence_red <= 6)
				printf("REL-FLK RED: [%s] list storage %p submitted at fence token %u freed while "
					   "the GPU completed only token %u (frame %u)\n", s_case, (void*)p,
					   s_submitted[i].token, s_completed, s_frame_no);
		}
		/* Freed, or the address was recycled: the old stamp no longer applies. */
		s_submitted[i] = s_submitted[--s_submitted_n];
	}
	submit_unlock();
}
static void fence_configure(bool fence) {
	FIX(!displaylist_gpu_pending());
	s_fence_mode = fence;
	s_issued = s_completed = 0;
	s_sealed_n = s_frame_no = s_waits = s_pending_max = 0;
	s_submitted_n = 0;
	static const struct displaylist_gpu_hooks hooks = {fake_read, fake_seal, fake_wait, fake_is_main};
	displaylist_gpu_configure(fence ? &hooks : NULL, 0);
	displaylist_carve_observe = fence ? fence_observe : NULL;
}
/* Case end: every retired list must leave the queue once its fence lands. */
static void fence_drain(void) {
	/* The model is only meaningful if swaps really parked displayed lists on the fence. */
	FIX(s_pending_max > 0);
	for(unsigned i = 0; i <= FENCE_LAG + 1 && displaylist_gpu_pending(); i++) {
		fence_frame_end();
		fence_frame_begin();
	}
	if(displaylist_gpu_pending())
		red("%u retired lists never left the GPU retirement queue after their fences completed",
			displaylist_gpu_pending());
	submit_lock();
	unsigned live = s_submitted_n;
	submit_unlock();
	if(live) red("%u submitted list storages were never released after the world was destroyed", live);
	if(s_waits)
		red("the main thread blocked on the GPU fence %u times during mesh swaps "
			"(retirement queue overflow)", s_waits);
	s_red += s_fence_red;
	s_case_red += s_fence_red;
	s_fence_red = 0;
}

void __wrap_displaylist_render(struct displaylist* l) {
	FIX(s_cur && l >= s_cur->mesh && l < s_cur->mesh + CHUNK_MESH_LISTS);
	if(!l->data || l->truncated) return;
	fence_note_submit(l);
	if(s_cur->x != 0 || s_cur->z != 0) return;
	int cy = s_cur->y / CHUNK_SIZE;
	FIX(cy >= 0 && cy < CH);
	s_frame.draws[cy][s_cur_pass]++;
	s_frame.hash[cy][s_cur_pass] = list_hash(s_frame.hash[cy][s_cur_pass], l,
		(unsigned)(l - s_cur->mesh));
	if(s_pinned_n < sizeof(s_pinned) / sizeof(*s_pinned)) s_pinned[s_pinned_n++] = l->data;
}

void __real_displaylist_destroy(struct displaylist* l);
void __wrap_displaylist_destroy(struct displaylist* l) {
	if(s_install_window && l->data)
		for(unsigned i = 0; i < s_pinned_n; i++)
			if(s_pinned[i] == l->data) s_pin_destroyed++;
	__real_displaylist_destroy(l);
}

/* Rejection witness: every full-column install attempt that returned false. */
static unsigned s_install_fails;
bool __real_world_build_full_section(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* height, const uint8_t* grass_i, const uint8_t* grass_j);
bool __wrap_world_build_full_section(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* height, const uint8_t* grass_i, const uint8_t* grass_j) {
	bool ok = __real_world_build_full_section(w, x, z, ids, metadata, sky, torch, height,
		grass_i, grass_j);
	s_install_fails += !ok;
	return ok;
}
bool __real_world_build_full_section_leased(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* height, const uint8_t* grass_i, const uint8_t* grass_j,
	struct column_backing* backing);
bool __wrap_world_build_full_section_leased(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* height, const uint8_t* grass_i, const uint8_t* grass_j,
	struct column_backing* backing) {
	bool ok = __real_world_build_full_section_leased(w, x, z, ids, metadata, sky, torch,
		height, grass_i, grass_j, backing);
	s_install_fails += !ok;
	return ok;
}

/* ---- fixture ---- */
struct snap {
	uint8_t ids[sizeof(((struct chunk_rpc_slot*)0)->ids)];
	uint8_t meta[sizeof(((struct chunk_rpc_slot*)0)->metadata)];
	uint8_t sky[sizeof(((struct chunk_rpc_slot*)0)->lighting_sky)];
	uint8_t torch[sizeof(((struct chunk_rpc_slot*)0)->lighting_torch)];
	uint8_t height[CHUNK_SIZE * CHUNK_SIZE];
};
static const int s_nb[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
static struct snap s_snap[5]; /* [0] = target's final snapshot, [1..4] neighbours */
static struct server_local* s_srv;
static struct camera s_cam;

static struct server_chunk* sget(int x, int z) {
	struct server_chunk* sc = dict_server_chunks_get(s_srv->worlds[0].chunks, S_CHUNK_ID(x, z));
	FIX(sc && sc->backing);
	return sc;
}

static void client_start(void) {
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD}});
}

static void client_stop(void) {
	clin_drain_discard();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player);
	gstate.players[0].local_player = NULL;
}

static struct chunk_rpc_slot* slot_acquire(void) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	FIX(slot && !slot->backing);
	memset(slot->grass_i, 0, sizeof(slot->grass_i));
	memset(slot->grass_j, 0, sizeof(slot->grass_j));
	slot->piston_moving.count = 0;
	return slot;
}

static void snap_take(struct snap* d, struct chunk_rpc_slot* s) {
	memcpy(d->ids, s->ids, sizeof(d->ids));
	memcpy(d->meta, s->metadata, sizeof(d->meta));
	memcpy(d->sky, s->lighting_sky, sizeof(d->sky));
	memcpy(d->torch, s->lighting_torch, sizeof(d->torch));
	memcpy(d->height, s->heightmap, sizeof(d->height));
}

/* Leased: a real local snapshot sharing the server backing. Owned: the same
 * bytes with no backing (the remote/copied install path). */
static struct chunk_rpc_slot* make_slot(int x, int z, bool leased, struct snap* keep) {
	struct chunk_rpc_slot* slot = slot_acquire();
	clin_chunk_slot_copy_local(slot, sget(x, z));
	if(keep) snap_take(keep, slot);
	if(!leased) {
		column_backing_unref(slot->backing);
		slot->backing = NULL;
		slot->local_revision = (struct column_revision){0};
	}
	return slot;
}

static struct chunk_rpc_slot* slot_of(const struct snap* d) {
	struct chunk_rpc_slot* slot = slot_acquire();
	memcpy(slot->ids, d->ids, sizeof(d->ids));
	memcpy(slot->metadata, d->meta, sizeof(d->meta));
	memcpy(slot->lighting_sky, d->sky, sizeof(d->sky));
	memcpy(slot->lighting_torch, d->torch, sizeof(d->torch));
	memcpy(slot->heightmap, d->height, sizeof(d->height));
	return slot;
}

static void publish(struct chunk_rpc_slot* slot, int x, int z) {
	clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK,
		.payload.chunk = {.x = x * CHUNK_SIZE, .z = z * CHUNK_SIZE,
			.sx = CHUNK_SIZE, .sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}});
}

static void server_set(int x, int y, int z, uint8_t type) {
	FIX(server_world_set_block(&s_srv->worlds[0], x, y, z, (struct block_data){.type = type}));
}
static void server_settle(void) {
	FIX(server_world_light_drain(&s_srv->worlds[0]));
	clin_drain_discard(); /* the per-block RPCs; the full column follows */
}

/* One main-loop frame in production order. The install happens while the
 * previous frame is still pinned for the GPU. */
static void frame(struct frame* out) {
	if(s_fence_mode) fence_frame_begin(); /* gfx_flip_buffers: reap completed fences */
	chunk_mesher_receive();
	s_install_window = true;
	clin_update_drain();
	s_install_window = false;
	struct camera* views[] = {&s_cam};
	FIX(world_pre_render_views(&gstate.world, views, 1));
	world_build_chunks(&gstate.world, CHUNK_MESHER_QLENGTH, true);
	memset(&s_frame, 0, sizeof(s_frame));
	for(unsigned i = 0; i < CH; i++) s_frame.hash[i][0] = s_frame.hash[i][1] = DL_SCENE_FNV64_INIT;
	s_pinned_n = 0;
	world_render_view(&gstate.world, &s_cam, 0, false);
	world_render_view(&gstate.world, &s_cam, 0, true);
	world_render_completed(&gstate.world, true);
	if(s_fence_mode) fence_frame_end(); /* gfx_finish: seal this frame's token */
	if(out) *out = s_frame;
}

static bool settled(void) {
	if(chunk_mesher_pending()) return false;
	for(unsigned n = 0; n < 5; n++)
		for(int cy = 0; cy < CH; cy++) {
			struct chunk* c = world_find_chunk(&gstate.world, s_nb[n][0] * CHUNK_SIZE,
				cy * CHUNK_SIZE, s_nb[n][1] * CHUNK_SIZE);
			if(c && (c->rebuild_displist || c->mesh_in_flight)) return false;
		}
	return true;
}

/* Frames until settled; optionally logs every frame. Returns frame count,
 * which never exceeds the limit: one slot stays reserved for the final
 * settled frame, so every counted frame is stored when a log is given. */
static unsigned settle(struct frame* log, unsigned cap) {
	unsigned limit = log ? cap : MAX_LOG;
	FIX(limit >= 2 && limit <= MAX_LOG);
	unsigned n = 0;
	ptime_t start = time_get();
	do {
		FIX(n + 1 < limit && time_diff_s(start, time_get()) < 60);
		frame(log ? &log[n] : NULL);
		n++;
		if(chunk_mesher_pending()) thread_msleep(1);
	} while(!settled());
	frame(log ? &log[n] : NULL); /* draw the settled state (n < limit) */
	return n + 1;
}

/* Every list of every column-(0,0) chunk, incl. has/reachable flags. */
static void column_lists(uint64_t out[CH]) {
	for(int cy = 0; cy < CH; cy++) {
		struct chunk* c = world_find_chunk(&gstate.world, 0, cy * CHUNK_SIZE, 0);
		FIX(c);
		uint64_t h = dl_scene_fnv64(DL_SCENE_FNV64_INIT, c->reachable, sizeof(c->reachable));
		for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++) {
			h = dl_scene_fnv64_u32(h, c->has_displist[k]);
			if(c->has_displist[k]) h = list_hash(h, c->mesh + k, k);
		}
		out[cy] = h;
	}
}

static unsigned frame_draws(const struct frame* f) {
	unsigned d = 0;
	for(int cy = 0; cy < CH; cy++) d += f->draws[cy][0] + f->draws[cy][1];
	return d;
}

static void expect_retained(const struct frame* f, const struct frame* b, const char* when, unsigned fi) {
	for(int cy = 0; cy < CH; cy++)
		for(int p = 0; p < 2; p++)
			if(f->draws[cy][p] != b->draws[cy][p] || f->hash[cy][p] != b->hash[cy][p])
				red("column (0,0) chunk cy=%d %s pass: %u submissions in frame %u %s; "
					"expected the %u retained pre-refresh lists", cy, p ? "translucent" : "opaque",
					f->draws[cy][p], fi, when, b->draws[cy][p]);
}

static struct frame s_log[MAX_LOG];

/* Install a refresh while the worker is held and check continuity over 3 frames. */
static void held_refresh(struct chunk_rpc_slot* slot, const struct frame* b, const char* when) {
	unsigned seq_before = s_pin_destroyed;
	publish(slot, 0, 0);
	for(unsigned i = 1; i <= 3; i++) {
		struct frame f;
		frame(&f);
		expect_retained(&f, b, when, i);
	}
	ptime_t start = time_get(); /* bounded rendezvous with the enqueued job */
	while(!chunk_mesher_test_worker_held() && time_diff_s(start, time_get()) < 30)
		thread_msleep(1);
	FIX(chunk_mesher_test_worker_held());
	if(s_pin_destroyed != seq_before)
		red("%s destroyed %u lists of the frame still pinned for the GPU", when,
			s_pin_destroyed - seq_before);
}

enum target_kind { OWNED, LEASED, EMPTY };

static void run_case(const char* name, enum target_kind initial, const enum target_kind* refresh,
					 unsigned n_refresh, bool stale_job, bool failed_preflight, bool fence) {
	s_case = name;
	s_case_red = 0;
	fence_configure(fence); /* worker idle: no case is in flight here */
	client_start();
	s_srv = split_fixture();
	struct server_chunk* sc;
	for(unsigned n = 0; n < 5; n++)
		FIX(server_world_load_chunk(&s_srv->worlds[0], s_nb[n][0], s_nb[n][1], &sc));
	/* Stone platform with a lava pool, high above the generated terrain. */
	for(int x = 4; x < 12; x++)
		for(int z = 4; z < 12; z++) server_set(x, 100, z, BLOCK_STONE);
	for(int x = 6; x < 10; x++)
		for(int z = 6; z < 10; z++) server_set(x, 101, z, BLOCK_LAVA_STILL);
	server_settle();
	for(unsigned n = 1; n < 5; n++) { /* neighbours: owned copies, kept for the reference */
		publish(make_slot(s_nb[n][0], s_nb[n][1], false, &s_snap[n]), s_nb[n][0], s_nb[n][1]);
		clin_update_drain();
	}
	publish(make_slot(0, 0, initial == LEASED, &s_snap[0]), 0, 0);
	clin_update_drain();
	struct chunk* c6 = world_find_chunk(&gstate.world, 0, 6 * CHUNK_SIZE, 0);
	FIX(c6 && (initial == LEASED) == (c6->ownership == LOCAL_SHARED_VIEW));
	settle(NULL, 0);
	struct frame b;
	frame(&b);
	FIX(b.draws[6][0] > 0); /* the platform/lava chunk is visible */

	chunk_mesher_test_hold_worker(true);
	if(stale_job) { /* client-side edit: a pre-refresh job is in flight */
		world_set_block(&gstate.world, 5, 101, 5, (struct block_data){.type = BLOCK_STONE}, false);
		struct frame f;
		frame(&f);
		c6 = world_find_chunk(&gstate.world, 0, 6 * CHUNK_SIZE, 0);
		FIX(c6 && c6->mesh_in_flight);
		expect_retained(&f, &b, "after the client edit", 0);
	}
	ptime_t start = time_get();
	while(chunk_mesher_pending() && !chunk_mesher_test_worker_held()) {
		FIX(time_diff_s(start, time_get()) < 30);
		thread_msleep(1);
	}

	static const uint8_t change[][2] = {{6, BLOCK_GLOWSTONE}, {9, BLOCK_STONE}, {7, BLOCK_AIR}};
	struct column_backing* backing = sget(0, 0)->backing;
	for(unsigned r = 0; r < n_refresh; r++) {
		char when[96];
		if(failed_preflight && r == 1) {
			/* Distinguishable incoming column: glass the resident one lacks. */
			server_set(3, 101, 3, BLOCK_GLASS);
			server_settle();
			backing = sget(0, 0)->backing;
			uint8_t old_type = world_get_block(&gstate.world, 3, 101, 3).type;
			FIX(old_type != BLOCK_GLASS);
			struct chunk* shell[CH];
			enum chunk_ownership own[CH];
			struct column_backing* bk[CH];
			for(int cy = 0; cy < CH; cy++) {
				shell[cy] = world_find_chunk(&gstate.world, 0, cy * CHUNK_SIZE, 0);
				FIX(shell[cy]);
				own[cy] = shell[cy]->ownership;
				bk[cy] = shell[cy]->backing;
			}
			for(unsigned fail = 1; fail <= CH; fail += CH - 1) {
				uint32_t refs = column_backing_references(backing);
				unsigned fails = s_install_fails;
				snprintf(when, sizeof(when), "after failed refresh preflight (shell %u)", fail);
				struct chunk_rpc_slot* slot = make_slot(0, 0, refresh[r] == LEASED, NULL);
				chunk_test_fail_alloc_after(fail);
				held_refresh(slot, &b, when);
				chunk_test_fail_alloc_after(0);
				struct frame f; /* the payload is consumed, not deferred */
				frame(&f);
				expect_retained(&f, &b, when, 4);
				FIX(s_install_fails == fails + 1); /* the injected failure occurred */
				for(int cy = 0; cy < CH; cy++) {
					struct chunk* c = world_find_chunk(&gstate.world, 0, cy * CHUNK_SIZE, 0);
					if(c != shell[cy] || c->ownership != own[cy] || c->backing != bk[cy])
						red("%s replaced chunk cy=%d shell/ownership/backing", when, cy);
				}
				if(world_get_block(&gstate.world, 3, 101, 3).type != old_type)
					red("%s installed the rejected column's data", when);
				if(column_backing_references(backing) != refs)
					red("failed preflight changed backing references %u -> %u", refs,
						column_backing_references(backing));
			}
		}
		struct chunk_rpc_slot* slot;
		if(refresh[r] == EMPTY) {
			memset(&s_snap[0], 0, sizeof(s_snap[0]));
			memset(s_snap[0].sky, 0xFF, sizeof(s_snap[0].sky));
			slot = slot_of(&s_snap[0]);
		} else {
			unsigned v = r % 3;
			server_set(change[v][0], 101, change[v][0], change[v][1]); /* block + light change */
			server_settle();
			backing = sget(0, 0)->backing;
			slot = make_slot(0, 0, refresh[r] == LEASED, &s_snap[0]);
		}
		snprintf(when, sizeof(when), "after %s refresh %u",
			refresh[r] == LEASED ? "leased" : "owned", r + 1);
		held_refresh(slot, &b, when);
	}
	c6 = world_find_chunk(&gstate.world, 0, 6 * CHUNK_SIZE, 0);
	FIX(c6 && (refresh[n_refresh - 1] == LEASED) == (c6->ownership == LOCAL_SHARED_VIEW));

	chunk_mesher_test_hold_worker(false);
	unsigned logged = settle(s_log, MAX_LOG);
	FIX(logged >= 1 && logged <= MAX_LOG);
	uint64_t got[CH];
	column_lists(got);
	struct frame s = s_log[logged - 1];

	/* Fresh client build of the identical snapshots, camera and LOD. */
	client_stop();
	client_start();
	for(unsigned n = 1; n < 5; n++) {
		publish(slot_of(&s_snap[n]), s_nb[n][0], s_nb[n][1]);
		clin_update_drain();
	}
	enum target_kind last = refresh[n_refresh - 1];
	publish(last == LEASED ? make_slot(0, 0, true, NULL) : slot_of(&s_snap[0]), 0, 0);
	clin_update_drain();
	settle(NULL, 0);
	struct frame ref;
	frame(&ref);
	uint64_t want[CH];
	column_lists(want);

	for(unsigned i = 0; i < logged; i++)
		for(int cy = 0; cy < CH; cy++)
			for(int p = 0; p < 2; p++) {
				const struct frame* f = &s_log[i];
				bool old = f->draws[cy][p] == b.draws[cy][p] && f->hash[cy][p] == b.hash[cy][p];
				bool now = f->draws[cy][p] == ref.draws[cy][p] && f->hash[cy][p] == ref.hash[cy][p];
				if(!old && !now)
					red("column (0,0) chunk cy=%d %s pass: frame %u after release submitted %u lists "
						"matching neither the retained (%u) nor the final (%u) geometry",
						cy, p ? "translucent" : "opaque", i + 1, f->draws[cy][p],
						b.draws[cy][p], ref.draws[cy][p]);
			}
	for(int cy = 0; cy < CH; cy++) {
		if(got[cy] != want[cy])
			red("column (0,0) chunk cy=%d: settled lists/flags differ from a fresh build", cy);
		for(int p = 0; p < 2; p++)
			if(s.draws[cy][p] != ref.draws[cy][p] || s.hash[cy][p] != ref.hash[cy][p])
				red("column (0,0) chunk cy=%d %s pass: settled submissions %u differ from a "
					"fresh build's %u", cy, p ? "translucent" : "opaque", s.draws[cy][p],
					ref.draws[cy][p]);
	}
	if(last == EMPTY && frame_draws(&s))
		red("empty final snapshot still submits %u column lists", frame_draws(&s));
	if(last != EMPTY) FIX(frame_draws(&ref) > 0);

	client_stop();
	if(fence) fence_drain();
	split_destroy(s_srv);
	uint32_t made, freed, refs;
	column_backing_test_counts(&made, &freed, &refs);
	if(made != freed || refs)
		red("backing leases leaked: created %u destroyed %u live refs %u", made, freed, refs);
	printf("%s [%s]: %u frames after release, %u contract failures\n",
		s_case_red ? "RED" : "PASS", name, logged, s_case_red);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	s_main_thread = pthread_self();
	blocks_init();
	items_init();
	recipe_init();
	FIX(clin_init() && svin_init());
	FIX(chunk_mesher_init());
	gstate.config.fog_distance = 4096;
	s_cam = (struct camera){.x = 8.5F, .y = 106.0F, .z = 8.5F};
	glm_mat4_identity(s_cam.view);
	for(unsigned p = 0; p < 6; p++) s_cam.frustum_planes[p][3] = 1;

	static const enum target_kind owned_twice[] = {OWNED, OWNED};
	static const enum target_kind to_leased[] = {LEASED, LEASED, LEASED};
	static const enum target_kind to_empty[] = {OWNED, EMPTY};
	run_case("owned->owned x2, stale pre-refresh job", OWNED, owned_twice, 2, true, false, false);
	run_case("owned->leased x3, failed preflight", OWNED, to_leased, 3, false, true, false);
	run_case("leased->owned->empty, stale job", LEASED, to_empty, 2, true, false, false);
	/* Delayed GPU fence (Wii): swaps and the empty-column teardown must retire
	 * the displayed lists through the fence, never free them under the GPU. */
	run_case("owned->owned x2, stale job, delayed GPU fence", OWNED, owned_twice, 2, true, false, true);
	run_case("leased->owned->empty, stale job, delayed GPU fence", LEASED, to_empty, 2, true, false, true);
	fence_configure(false);
	chunk_mesher_shutdown();
	if(s_red) {
		printf("REL-FLK RED: %u contract failures (resident column vanished or diverged during refresh)\n", s_red);
		return 1;
	}
	puts("PASS test_chunk_refresh_draw_continuity");
	return 0;
}
