/* Split-dims B2, Fable review F5: GPU / display-list lifetime of a SLOT-1
 * (second, world-bound) client payload, flag ON, through the headless render
 * path the column-dedup lifetime tests use: real CRPC_WORLD_RESET / CRPC_CHUNK
 * consumer, real mesher worker (real display lists), world_pre_render_views
 * (the per-frame BFS that takes one frame's chunk references for the draw),
 * world_render_completed (the flip hand-off that pins last frame's chunks for
 * the GPU under a GPU lease). No GL context: the draw call itself is not
 * issued; everything it would read is owned exactly as in a real frame.
 *
 * displaylist_destroy is linker-wrapped (--wrap) to record every destroyed
 * list, so the test proves, for a slot-1 world that is RESET and for one that
 * is RELEASED while its last frame is still pinned for the GPU:
 *  - no display list of that frame is destroyed while the GPU pin (and its
 *    lease against the retired context) is outstanding -- no use-after-free;
 *  - every such list is destroyed exactly once when the pin drains -- no leak,
 *    no double free;
 *  - the retired context is reclaimed only after that GPU lease is released;
 *  - the other world (slot 0) keeps its context and its lists throughout. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "game/camera.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/server_interface.h"
#include "platform/displaylist.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

#define LOCAL_EID 77

/* ---- displaylist_destroy wrap: count destructions inside a watched chunk */
void __real_displaylist_destroy(struct displaylist* l);
static const char* s_watch_lo;
static const char* s_watch_hi;
static unsigned s_watch_destroyed;
void __wrap_displaylist_destroy(struct displaylist* l) {
	const char* p = (const char*)l;
	if(s_watch_lo && p >= s_watch_lo && p < s_watch_hi)
		s_watch_destroyed++;
	__real_displaylist_destroy(l);
}

static struct client_ctx_registry* reg;

static void world_reset(uint8_t index, uint32_t generation) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID, .world_index = index,
			.world_generation = generation}});
}

static struct world* terrain(unsigned slot) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	return p.terrain;
}

static void column(uint8_t index, uint32_t generation) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(size_t lx = 0; lx < CHUNK_SIZE; lx++)
		for(size_t lz = 0; lz < CHUNK_SIZE; lz++)
			slot->ids[(lx * CHUNK_SIZE + lz) * WORLD_HEIGHT] = BLOCK_STONE;
	clin_process(&(struct client_rpc) {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = generation,
		.payload.chunk = {.x = 0, .y = 0, .z = 0, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}});
}

/* Mesh one chunk through the real worker and publish its lists. */
static unsigned mesh(struct chunk* c) {
	assert(chunk_mesher_send(c));
	c->mesh_in_flight = true;
	ptime_t start = time_get();
	while(chunk_mesher_pending()) {
		chunk_mesher_receive();
		assert(time_diff_s(start, time_get()) < 10);
		if(chunk_mesher_pending()) thread_msleep(1);
	}
	c->mesh_in_flight = false;
	c->rebuild_displist = false;
	unsigned lists = 0;
	for(size_t k = 0; k < CHUNK_MESH_LISTS; k++)
		lists += c->has_displist[k] ? 1u : 0u;
	return lists;
}

static void draw_frame(struct world* w) {
	struct camera camera = {.x = 8, .y = 8, .z = 8};
	glm_mat4_identity(camera.view);
	for(unsigned p = 0; p < 6; p++) camera.frustum_planes[p][3] = 1;
	struct camera* views[] = {&camera};
	gstate.config.fog_distance = 4096;
	assert(world_pre_render_views(w, views, 1));
	assert(w->render_views[0].count > 0 && "the slot-1 chunk is in the frame");
	/* Flip: this frame's chunks are handed to the GPU under a GPU lease. */
	world_render_completed(w, true);
}

/* Two live worlds; the guest world is slot 1 (non-primary), meshed + drawn. */
static struct chunk* setup(unsigned* lists, struct client_world_context** ctx) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(clin_world_payloads_set_enabled(false));
	assert(clin_world_payloads_set_enabled(true));
	world_reset(1, 200);
	world_reset(0, 100);
	assert(clin_world_payload_bind_view(1, 1));
	column(1, 200);
	column(0, 100);
	struct world* w1 = terrain(1);
	struct chunk* c = world_find_chunk(w1, 0, 0, 0);
	assert(c && c->world == w1);
	*lists = mesh(c);
	assert(*lists > 0 && "real display lists were built for the slot-1 chunk");
	draw_frame(w1);
	*ctx = client_ctx_registry_current(reg, 1);
	assert((*ctx)->leases[CLIENT_CTX_LEASE_GPU] == 1
		   && "the GPU pin leases against the slot-1 context");
	s_watch_lo = (const char*)c;
	s_watch_hi = (const char*)(c + 1);
	s_watch_destroyed = 0;
	return c;
}

static void drain_and_check(struct world* w1, struct chunk* c, unsigned lists,
							struct client_world_context* retired) {
	assert(retired->retiring && client_ctx_registry_owns(reg, retired)
		   && "the retired context lives while the GPU lease is out");
	assert(s_watch_destroyed == 0
		   && "no list of the pinned frame is destroyed before the GPU drains");
	assert(c->reference_count == 1 && "only the GPU pin keeps the chunk");
	/* Next frame's flip: nothing new drawn, last frame's pin drains. */
	world_pre_render_clear(w1);
	world_render_completed(w1, false);
	s_watch_lo = s_watch_hi = NULL;
	assert(s_watch_destroyed == lists
		   && "every list destroyed exactly once, only after the last draw");
	assert(!client_ctx_registry_owns(reg, retired)
		   && "the context is reclaimed once its GPU lease is released");
}

static void test_reset_while_pinned(void) {
	unsigned lists;
	struct client_world_context* ctx;
	struct chunk* c = setup(&lists, &ctx);
	struct world* w0 = terrain(0);
	struct chunk* c0 = world_find_chunk(w0, 0, 0, 0);
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	world_reset(1, 201);
	assert(!world_find_chunk(terrain(1), 0, 0, 0) && "slot-1 terrain unloaded");
	drain_and_check(terrain(1), c, lists, ctx);
	assert(client_ctx_registry_current(reg, 0) == ow
		   && world_find_chunk(w0, 0, 0, 0) == c0
		   && "the other world is untouched");
	puts("PASS reset while pinned: slot-1 lists outlive the reset until the "
		 "GPU drains, then are destroyed exactly once; context reclaimed after");
}

static void test_release_while_pinned(void) {
	unsigned lists;
	struct client_world_context* ctx;
	struct chunk* c = setup(&lists, &ctx);
	assert(clin_world_payload_release(1));
	assert(!client_ctx_registry_current(reg, 1));
	drain_and_check(terrain(1), c, lists, ctx);
	puts("PASS release while pinned: same lifetime through the guest-leave "
		 "release path");
}

static void test_disable_refused_while_pinned(void) {
	unsigned lists;
	struct client_world_context* ctx;
	struct chunk* c = setup(&lists, &ctx);
	assert(clin_world_payload_release(1));
	client_ctx_registry_retire(reg, 0);
	assert(!clin_world_payloads_set_enabled(false)
		   && "slot-1 storage is never freed while its GPU pin is out");
	drain_and_check(terrain(1), c, lists, ctx);
	assert(clin_world_payloads_set_enabled(false)
		   && "after the drain the storage frees cleanly");
	puts("PASS disable refused while pinned: world storage outlives the pin");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	assert(svin_init());
	assert(clin_init());
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	reg = clin_world_contexts();
	assert(chunk_mesher_init());
	test_reset_while_pinned();
	test_release_while_pinned();
	test_disable_refused_while_pinned();
	chunk_mesher_shutdown();
	puts("PASS test_b2_gpu_lifetime");
	return 0;
}
