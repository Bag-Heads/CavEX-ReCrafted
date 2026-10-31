/* Copyright (c) 2026 CaveWii_v2 contributors
 * Independent packed reference images, real backing/transport/client writers.
 * Expectations are the saved RPC bytes, never a second call to the provider.
 */
#include "support/split_residency_fixture.h"
#include "column_view.h"
#include "game/game_state.h"
#include "item/recipe.h"
void column_dedup_prefix_equiv(void);

static unsigned index_of(unsigned x, unsigned y, unsigned z) {
	return (x * 16 + z) * 128 + y;
}
static struct chunk_rpc_slot* snapshot(struct server_chunk* sc) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot);
	clin_chunk_slot_copy_local(slot, sc);
	memset(slot->grass_i, 0, 256); memset(slot->grass_j, 0, 256);
	slot->piston_moving.count = 0;
	return slot;
}
static void install(struct chunk_rpc_slot* slot) {
	assert(world_build_full_section_leased(&gstate.world, 0, 0,
		slot->ids, slot->metadata, slot->lighting_sky, slot->lighting_torch,
		slot->heightmap, slot->grass_i, slot->grass_j, slot->backing));
}
static uint8_t nibble(const uint8_t* p, unsigned i) {
	return (p[i / 2] >> ((i & 1) * 4)) & 15;
}
/* Independent packed reference planes (ids 32768 + three 16384 nibble planes
 * = all 81,920 presentation bytes), owned by the test, never by the provider. */
struct image {
	uint8_t ids[32768], metadata[16384], sky[16384], torch[16384];
};
static void set_nibble(uint8_t* p, unsigned i, uint8_t v) {
	unsigned shift = (i & 1) * 4;
	p[i / 2] = (uint8_t)((p[i / 2] & ~(15u << shift)) | ((v & 15u) << shift));
}
static void compare_planes(struct column_view* view, const uint8_t* ids,
	const uint8_t* metadata, const uint8_t* sky, const uint8_t* torch) {
	for(unsigned i = 0; i < 32768; i++) {
		struct block_data b = column_view_get(view, i);
		assert(b.type == ids[i]);
		assert(b.metadata == nibble(metadata, i));
		assert(b.sky_light == nibble(sky, i));
		assert(b.torch_light == nibble(torch, i));
	}
	/* Packed export: every one of the 81,920 bytes, from a poisoned buffer. */
	static uint8_t packed[81920];
	memset(packed, 0xA5, sizeof packed);
	(void)column_view_copy(view, packed, packed + 32768, packed + 49152, packed + 65536);
	assert(!memcmp(packed, ids, 32768));
	assert(!memcmp(packed + 32768, metadata, 16384));
	assert(!memcmp(packed + 49152, sky, 16384));
	assert(!memcmp(packed + 65536, torch, 16384));
}
static void compare(struct column_view* view, const struct chunk_rpc_slot* image) {
	compare_planes(view, image->ids, image->metadata, image->lighting_sky,
		image->lighting_torch);
}
static void compare_image(struct column_view* view, const struct image* image) {
	compare_planes(view, image->ids, image->metadata, image->sky, image->torch);
}
static void server_id(struct server_chunk* sc, unsigned i, uint8_t id) {
	column_backing_write_begin(sc->backing, 1, i, 1);
	sc->ids[i] = id;
	column_backing_write_end(sc->backing, 1, i, 1);
}
static struct column_view_stats stats(void) {
	struct column_view_stats result;
	column_view_get_stats(&result);
	return result;
}

static void page_cow(struct server_local* s, struct server_chunk* sc) {
	struct chunk_rpc_slot* old = snapshot(sc);
	/* Quiet metadata BEFORE consumption must not replace the queued snapshot. */
	assert(server_world_set_block_metadata_quiet(&s->worlds[0], 0, 64, 0, 7));
	size_t allocations = mem2_stat_alloc_calls(), allocated_bytes = mem2_stat_alloc_bytes();
	install(old);
	assert(mem2_stat_alloc_calls() - allocations == 3); // pool + reserved recovery image + index
	assert(mem2_stat_alloc_bytes() - allocated_bytes == COLUMN_VIEW_POOL_PAGES * 256 + COLUMN_VIEW_PRIVATE_BYTES + COLUMN_VIEW_INDEX_BYTES);
	struct chunk* c = world_find_chunk(&gstate.world, 0, 64, 0);
	assert(c && c->view && !c->blocks && c->ownership == LOCAL_SHARED_VIEW);
	compare(c->view, old);
	assert(stats().pages == 1);
	assert(server_world_set_block_metadata_quiet(&s->worlds[0], 0, 64, 0, 0));
	assert(stats().pages == 0); // base catches up to the still-old presentation
	/* Negative control for the observer: an intentionally UNBARRIERED test
	 * poke must be visible even though no client dirty generation changed. */
	uint8_t* exported = malloc(81920);
	assert(exported);
	memcpy(exported, old->ids, 32768); memcpy(exported + 32768, old->metadata, 16384);
	memcpy(exported + 49152, old->lighting_sky, 16384);
	memcpy(exported + 65536, old->lighting_torch, 16384);
	assert(!column_view_copy(c->view, exported, exported + 32768, exported + 49152, exported + 65536));
	sc->ids[30000] ^= 1;
	assert(column_view_copy(c->view, exported, exported + 32768, exported + 49152, exported + 65536));
	assert(exported[30000] == sc->ids[30000]);
	sc->ids[30000] ^= 1;
	free(exported);

	/* Both sides of Y-pair/page/section boundaries; 256 cells, NOT 256
	 * packed bytes. Multiple edits within a page consume exactly one slot. */
	const unsigned indices[] = {0, 1, 15, 16, 127, 128, 255, 256, 32767};
	for(unsigned j = 0; j < sizeof(indices)/sizeof(*indices); j++)
		server_id(sc, indices[j], BLOCK_GLASS);
	assert(stats().pages == 3);
	compare(c->view, old);
	for(unsigned j = 0; j < sizeof(indices)/sizeof(*indices); j++)
		server_id(sc, indices[j], old->ids[indices[j]]);
	assert(stats().pages == 0);

	/* Client-owned light differs from BOTH old and later backing values. */
	unsigned i = index_of(0, 64, 0);
	chunk_set_light(c, 0, 0, 0, 0x73);
	assert(sc->lighting_torch[i / 2] == old->lighting_torch[i / 2]);
	assert(stats().pages == 2);
	column_backing_write_begin(sc->backing, 12, i, 1);
	sc->lighting_torch[i / 2] = 9; sc->lighting_sky[i / 2] = 1;
	column_backing_write_end(sc->backing, 12, i, 1);
	assert(chunk_get_block(c, 0, 0, 0).sky_light == 3);
	assert(chunk_get_block(c, 0, 0, 0).torch_light == 7);
	/* Whole packed byte above also changes the sibling nibble; restore it
	 * before checking rejoin, which must compare the ENTIRE decoded page. */
	column_backing_write_begin(sc->backing, 12, i, 2);
	sc->lighting_torch[i / 2] = old->lighting_torch[i / 2];
	sc->lighting_sky[i / 2] = old->lighting_sky[i / 2];
	column_backing_write_end(sc->backing, 12, i, 2);
	chunk_set_light(c, 0, 0, 0, (nibble(old->lighting_torch, i) << 4) | nibble(old->lighting_sky, i));
	assert(stats().pages == 0);
	compare(c->view, old);
	assert(server_world_test_populate_snapshot_rollback(&s->worlds[0], 0, 0, 65536));
	assert(stats().pages == 0 && stats().peak_pages >= 128);
	compare(c->view, old);
	clin_chunk_slot_release(old);
	world_unload_all(&gstate.world);
	assert(stats().live_views == 0 && stats().pages == 0);
	puts("PASS snapshot installation; quiet metadata; 256-cell COW boundaries; independent client light; rejoin");
}

struct race {
	struct server_chunk* sc;
	struct thread_channel ready, proceed;
};
static struct race* allocation_wait;
static uint32_t allocation_armed;
void* __real_mem2_alloc(size_t bytes);
void* __wrap_mem2_alloc(size_t bytes) {
	if(bytes == COLUMN_VIEW_PRIVATE_BYTES
	   && __atomic_exchange_n(&allocation_armed, 0, __ATOMIC_ACQ_REL)) {
		tchannel_send(&allocation_wait->ready, allocation_wait, true);
		void* token;
		assert(tchannel_receive(&allocation_wait->proceed, &token, true));
	}
	return __real_mem2_alloc(bytes);
}

static void* fallback_writer(void* user) {
	struct race* r = user;
	column_backing_set_cell(r->sc->backing, COLUMN_IDS, 64, r->sc->ids[64] ^ 1);
	return NULL;
}

static void retire_during_fallback(struct server_chunk* sc) {
	struct chunk_rpc_slot* old = snapshot(sc);
	install(old);
	column_view_test_page_limit(0);
	struct race race = {.sc = sc};
	assert(tchannel_init(&race.ready, 1) && tchannel_init(&race.proceed, 1));
	allocation_wait = &race;
	__atomic_store_n(&allocation_armed, 1, __ATOMIC_RELEASE);
	struct thread writer;
	assert(thread_create(&writer, fallback_writer, &race, 64));
	void* token;
	assert(tchannel_receive(&race.ready, &token, true));
	/* The writer is inside MEM2 allocation with its stripe released. Retire
	 * the very view that exhausted the pool and register a new one in its place. */
	world_unload_all(&gstate.world);
	assert(stats().live_views == 0);
	install(old);
	tchannel_send(&race.proceed, &race, true);
	thread_join(&writer);
	struct chunk* c = world_find_chunk(&gstate.world, 0, 64, 0);
	compare(c->view, old);
	assert(sc->ids[64] == (old->ids[64] ^ 1));
	assert(stats().live_views == 1 && stats().fallback_live == 1);
	world_unload_all(&gstate.world);
	clin_chunk_slot_release(old);
	tchannel_close(&race.ready); tchannel_close(&race.proceed);
	allocation_wait = NULL;
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	assert(stats().live_views == 0 && stats().pages == 0 && stats().fallback_live == 0);
	puts("PASS retirement + replacement during server fallback allocation: no nested allocator/stripe, stale view, or presentation advance");
}
static void* race_writer(void* user) {
	struct race* r = user;
	for(unsigned phase = 0; phase < 100; phase++) {
		server_id(r->sc, 64, phase & 1 ? BLOCK_STONE : BLOCK_GLASS);
		tchannel_send(&r->ready, r, true);
		void* token;
		assert(tchannel_receive(&r->proceed, &token, true));
	}
	return NULL;
}
static void rpc_write_order(struct server_chunk* sc) {
	struct chunk_rpc_slot* old = snapshot(sc);
	install(old);
	struct chunk* c = world_find_chunk(&gstate.world, 0, 64, 0);
	struct race race = {.sc = sc};
	assert(tchannel_init(&race.ready, 1) && tchannel_init(&race.proceed, 1));
	struct thread writer;
	assert(thread_create(&writer, race_writer, &race, 64));
	uint8_t presented = old->ids[64];
	for(unsigned phase = 0; phase < 100; phase++) {
		void* token;
		assert(tchannel_receive(&race.ready, &token, true));
		assert(chunk_get_block(c, 0, 0, 0).type == presented);
		/* Consume an older RPC AFTER the server has already written the next
		 * state. Each prefix presents exactly that message, never latest base. */
		presented = phase & 1 ? BLOCK_GLASS : BLOCK_STONE;
		clin_rpc_send(&(struct client_rpc){.type = CRPC_SET_BLOCK,
			.payload.set_block = {.x = 0, .y = 64, .z = 0,
				.block = {.type = presented, .sky_light = 15}}});
		clin_update_drain();
		assert(chunk_get_block(c, 0, 0, 0).type == presented);
		tchannel_send(&race.proceed, &race, true);
	}
	thread_join(&writer);
	tchannel_close(&race.ready); tchannel_close(&race.proceed);
	clin_chunk_slot_release(old);
	world_unload_all(&gstate.world);
	assert(stats().live_views == 0 && stats().pages == 0);
	puts("PASS 100 ordered cross-thread server-write / consumed-RPC prefixes");
}

static void exhaustion(struct server_chunk* sc) {
	struct chunk_rpc_slot* old = snapshot(sc);
	size_t calls = mem2_stat_alloc_calls(), bytes = mem2_stat_alloc_bytes();
	install(old);
	assert(mem2_stat_alloc_calls() - calls == 3);
	assert(mem2_stat_alloc_bytes() - bytes == COLUMN_VIEW_POOL_PAGES * 256 + COLUMN_VIEW_PRIVATE_BYTES + COLUMN_VIEW_INDEX_BYTES);
	struct chunk* c = world_find_chunk(&gstate.world, 0, 64, 0);
	uint32_t before = stats().fallback_total;
	column_view_test_page_limit(1);
	server_id(sc, 0, old->ids[0] ^ 1);
	server_id(sc, 256, old->ids[256] ^ 1);
	assert(stats().fallback_total == before + 1 && stats().fallback_live == 1);
	assert(stats().pages == 0);
	compare(c->view, old);
	chunk_set_block_raw(c, 1, 0, 0, (struct block_data){.type = BLOCK_GLASS, .metadata = 7});
	assert(chunk_get_block(c, 1, 0, 0).metadata == 7);
	assert(sc->ids[index_of(1, 64, 0)] == old->ids[index_of(1, 64, 0)]);
	world_unload_all(&gstate.world);
	assert(stats().fallback_live == 0 && stats().live_views == 0);
	/* Exhaustion during initial snapshot comparison also owns private bytes. */
	column_view_test_page_limit(0);
	install(old);
	c = world_find_chunk(&gstate.world, 0, 64, 0);
	compare(c->view, old);
	assert(stats().fallback_total == before + 2);
	world_unload_all(&gstate.world);
	/* Exhaustion first triggered by a client light write. */
	struct chunk_rpc_slot* fresh = snapshot(sc);
	install(fresh);
	c = world_find_chunk(&gstate.world, 0, 64, 0);
	chunk_set_light(c, 0, 0, 0, 0x47);
	assert(chunk_get_block(c, 0, 0, 0).torch_light == 4);
	assert(stats().fallback_total == before + 3);
	world_unload_all(&gstate.world);
	clin_chunk_slot_release(fresh); clin_chunk_slot_release(old);
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	assert(stats().pages == 0 && stats().fallback_live == 0 && stats().live_views == 0);
	puts("PASS bounded pool exhaustion: server COW, snapshot install, client light; exact private fallback and retirement");
}

/* Fill the ACTUAL configured capacity (COLUMN_VIEW_POOL_PAGES), never a
 * hard-coded 256: each view diverges from the backing in whole planes
 * (128 pages per plane, 512 per fully divergent view) until the pool is
 * exactly full; then one more fully divergent view must materialize a
 * private image while every retained presentation stays byte-exact. */
#define PLANE_PAGES (COLUMN_VIEW_PAGES / 4)
_Static_assert(COLUMN_VIEW_POOL_PAGES % PLANE_PAGES == 0,
	"pool capacity must be a whole number of 128-page planes for this fill");
#define FILL_VIEWS_MAX ((COLUMN_VIEW_POOL_PAGES + COLUMN_VIEW_PAGES - 1) / COLUMN_VIEW_PAGES + 1)
static struct image* diverged_image(struct server_chunk* sc, unsigned planes,
	unsigned salt) {
	struct image* im = malloc(sizeof *im);
	assert(im);
	memcpy(im->ids, sc->ids, 32768);
	memcpy(im->metadata, sc->metadata, 16384);
	memcpy(im->sky, sc->lighting_sky, 16384);
	memcpy(im->torch, sc->lighting_torch, 16384);
	/* Every cell of each selected plane differs from the backing; the salt
	 * keeps the views' patterns distinct from one another. */
	if(planes > 0)
		for(unsigned i = 0; i < 32768; i++) im->ids[i] ^= (uint8_t)(1 + salt);
	uint8_t* nibbles[] = {im->metadata, im->sky, im->torch};
	for(unsigned p = 1; p < planes; p++)
		for(unsigned i = 0; i < 16384; i++)
			nibbles[p - 1][i] ^= (salt & 1) ? 0xAA : 0xFF;
	return im;
}
static void physical_pool_exhaustion(struct server_chunk* sc) {
	struct image* images[FILL_VIEWS_MAX];
	struct column_view* views[FILL_VIEWS_MAX];
	unsigned count = 0, filled = 0;
	assert(stats().pages == 0 && stats().fallback_live == 0 && stats().live_views == 0);
	while(filled < COLUMN_VIEW_POOL_PAGES) {
		unsigned planes = (COLUMN_VIEW_POOL_PAGES - filled) / PLANE_PAGES;
		if(planes > 4) planes = 4;
		assert(count + 1 < FILL_VIEWS_MAX);
		images[count] = diverged_image(sc, planes, count);
		views[count] = column_view_create(sc->backing, images[count]->ids,
			images[count]->metadata, images[count]->sky, images[count]->torch);
		assert(views[count]);
		count++;
		filled += planes * PLANE_PAGES;
		assert(stats().pages == filled && stats().fallback_live == 0);
		for(unsigned k = 0; k < count; k++) compare_image(views[k], images[k]);
	}
	assert(stats().pages == COLUMN_VIEW_POOL_PAGES);
	/* Pool exactly full: one more 512-page divergence must go private. */
	images[count] = diverged_image(sc, 4, count);
	views[count] = column_view_create(sc->backing, images[count]->ids,
		images[count]->metadata, images[count]->sky, images[count]->torch);
	assert(views[count]);
	count++;
	for(unsigned k = 0; k < count; k++) compare_image(views[k], images[k]);
	assert(stats().pages == COLUMN_VIEW_POOL_PAGES);
	assert(stats().fallback_live == 1 && stats().peak_pages == COLUMN_VIEW_POOL_PAGES);
	for(unsigned n = 0; n < count; n++) {
		column_view_unref(views[n]);
		free(images[n]);
	}
	assert(stats().pages == 0 && stats().fallback_live == 0 && stats().live_views == 0);
	printf("PASS actual %u-slot exhaustion (%u filling views) then a 512-page divergence storm; exact fallback and all slots reusable\n",
		COLUMN_VIEW_POOL_PAGES, count - 1);
}

/* Plan item 3: the SAME operation sequence at admission limits 256 and 1024
 * (pool) and 0/1 (deterministic private fallback). After EVERY operation the
 * presentation (all 81,920 packed bytes) and all four backing planes are
 * compared against independent expected images the test maintains itself.
 * Covers server COW, snapshot install, client block/light edits, page
 * rejoin, replacement and retirement. */
struct limit_case {
	struct server_chunk* sc;
	struct image present, backing;
};
static void check(const struct limit_case* t) {
	struct chunk* c = world_find_chunk(&gstate.world, 0, 64, 0);
	assert(c && c->view && !c->blocks);
	compare_image(c->view, &t->present);
	assert(!memcmp(t->sc->ids, t->backing.ids, 32768));
	assert(!memcmp(t->sc->metadata, t->backing.metadata, 16384));
	assert(!memcmp(t->sc->lighting_sky, t->backing.sky, 16384));
	assert(!memcmp(t->sc->lighting_torch, t->backing.torch, 16384));
}
static void server_cell(struct limit_case* t, unsigned i, uint8_t id) {
	server_id(t->sc, i, id);
	t->backing.ids[i] = id;
}
static void server_meta(struct limit_case* t, unsigned i, uint8_t v) {
	column_backing_write_begin(t->sc->backing, 2, i, 1);
	set_nibble(t->sc->metadata, i, v);
	column_backing_write_end(t->sc->backing, 2, i, 1);
	set_nibble(t->backing.metadata, i, v);
}
static void server_light(struct limit_case* t, unsigned i, uint8_t sky, uint8_t torch) {
	column_backing_write_begin(t->sc->backing, 12, i, 1);
	set_nibble(t->sc->lighting_sky, i, sky);
	set_nibble(t->sc->lighting_torch, i, torch);
	column_backing_write_end(t->sc->backing, 12, i, 1);
	set_nibble(t->backing.sky, i, sky);
	set_nibble(t->backing.torch, i, torch);
}
static void present_from(struct image* dst, const struct chunk_rpc_slot* slot) {
	memcpy(dst->ids, slot->ids, 32768);
	memcpy(dst->metadata, slot->metadata, 16384);
	memcpy(dst->sky, slot->lighting_sky, 16384);
	memcpy(dst->torch, slot->lighting_torch, 16384);
}
static void limit_equivalence(struct server_chunk* sc, unsigned limit) {
	static struct limit_case t;
	static struct image original;
	t.sc = sc;
	memcpy(original.ids, sc->ids, 32768);
	memcpy(original.metadata, sc->metadata, 16384);
	memcpy(original.sky, sc->lighting_sky, 16384);
	memcpy(original.torch, sc->lighting_torch, 16384);
	t.backing = original;
	t.present = original;
	uint32_t total0 = stats().fallback_total;
	assert(stats().live_views == 0 && stats().pages == 0 && stats().fallback_live == 0);
	column_view_test_page_limit(limit);

	/* 1. Snapshot install: presentation = the saved RPC bytes. */
	struct chunk_rpc_slot* old = snapshot(sc);
	install(old);
	check(&t);
	struct chunk* c = world_find_chunk(&gstate.world, 0, 64, 0);

	/* 2. Server COW in every ids page and every metadata page (256 page
	 * preservations); the presentation stays the old snapshot. */
	for(unsigned k = 0; k < 128; k++) {
		unsigned i = k * 256 + (k % 7);
		server_cell(&t, i, (uint8_t)(t.backing.ids[i] ^ 1));
		check(&t);
		i = k * 256 + 128 + (k % 5);
		server_meta(&t, i, (uint8_t)(nibble(t.backing.metadata, i) ^ 9));
		check(&t);
	}
	if(limit >= 256) assert(stats().pages == 256 && stats().fallback_live == 0);
	else assert(stats().fallback_live == 1);

	/* 3. Server light COW: two more pages, beyond a 256-page limit. */
	unsigned li = index_of(3, 70, 5);
	server_light(&t, li, (uint8_t)(nibble(t.backing.sky, li) ^ 5),
		(uint8_t)(nibble(t.backing.torch, li) ^ 6));
	check(&t);
	if(limit >= 1024) assert(stats().pages == 258 && stats().fallback_live == 0);
	else assert(stats().fallback_live == 1);

	/* 4. Client block edit (all four planes), then a client light edit. */
	chunk_set_block_raw(c, 1, 2, 3, (struct block_data){.type = BLOCK_GLASS,
		.metadata = 7, .sky_light = 3, .torch_light = 9});
	unsigned bi = index_of(1, 66, 3);
	t.present.ids[bi] = BLOCK_GLASS;
	set_nibble(t.present.metadata, bi, 7);
	set_nibble(t.present.sky, bi, 3);
	set_nibble(t.present.torch, bi, 9);
	check(&t);
	chunk_set_light(c, 4, 5, 6, 0x5c); /* torch 5, sky 12 */
	unsigned ci = index_of(4, 69, 6);
	set_nibble(t.present.sky, ci, 12);
	set_nibble(t.present.torch, ci, 5);
	check(&t);

	/* 5. Page rejoin: the server catches the backing up to the presentation
	 * everywhere, including the client-edited cells. */
	for(unsigned k = 0; k < 128; k++) {
		unsigned i = k * 256 + (k % 7);
		server_cell(&t, i, t.present.ids[i]);
		check(&t);
		i = k * 256 + 128 + (k % 5);
		server_meta(&t, i, nibble(t.present.metadata, i));
		check(&t);
	}
	server_light(&t, li, nibble(t.present.sky, li), nibble(t.present.torch, li));
	check(&t);
	server_cell(&t, bi, t.present.ids[bi]);
	check(&t);
	server_meta(&t, bi, nibble(t.present.metadata, bi));
	check(&t);
	server_light(&t, bi, nibble(t.present.sky, bi), nibble(t.present.torch, bi));
	check(&t);
	server_light(&t, ci, nibble(t.present.sky, ci), nibble(t.present.torch, ci));
	check(&t);
	assert(!memcmp(&t.present, &t.backing, sizeof t.present));
	if(limit >= 1024) assert(stats().pages == 0 && stats().fallback_live == 0);
	else assert(stats().pages == 0 && stats().fallback_live == 1); /* private image keeps owning */

	/* 6. Replacement: a fresh snapshot of a further-diverged backing is
	 * installed over the live view, then diverges again under COW. */
	for(unsigned k = 0; k < 4; k++) {
		server_cell(&t, 4096 * k + 17, (uint8_t)(t.backing.ids[4096 * k + 17] ^ 2));
		check(&t);
	}
	struct chunk_rpc_slot* fresh = snapshot(sc);
	install(fresh);
	present_from(&t.present, fresh);
	assert(!memcmp(&t.present, &t.backing, sizeof t.present));
	check(&t);
	server_cell(&t, 99, (uint8_t)(t.backing.ids[99] ^ 4));
	check(&t);

	/* 7. Retirement returns every view, page and private image. */
	world_unload_all(&gstate.world);
	assert(stats().live_views == 0 && stats().pages == 0 && stats().fallback_live == 0);
	clin_chunk_slot_release(fresh); clin_chunk_slot_release(old);
	uint32_t fallbacks = stats().fallback_total - total0;
	if(limit >= 1024) assert(fallbacks == 0);
	else assert(fallbacks >= 1);

	/* Restore the shared backing for the next case (no view is live). */
	column_backing_write_begin(sc->backing, 15, 0, 32768);
	memcpy(sc->ids, original.ids, 32768);
	memcpy(sc->metadata, original.metadata, 16384);
	memcpy(sc->lighting_sky, original.sky, 16384);
	memcpy(sc->lighting_torch, original.torch, 16384);
	column_backing_write_end(sc->backing, 15, 0, 32768);
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	printf("PASS limit %u: install, server COW, client block/light, rejoin, replacement, retirement byte-exact after every op (%u private fallbacks)\n",
		limit, fallbacks);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD}});
	struct server_local* s = split_fixture();
	struct server_chunk* sc;
	for(int z = 0; z < 2; z++) for(int x = 0; x < 2; x++) {
		server_world_begin_update(&s->world);
		assert(server_world_load_chunk(&s->world, x, z, &sc));
	}
	sc = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(0, 0));
	assert(sc);
	page_cow(s, sc);
	rpc_write_order(sc);
	exhaustion(sc);
	physical_pool_exhaustion(sc);
	retire_during_fallback(sc);
	_Static_assert(COLUMN_VIEW_POOL_PAGES >= 1024, "limit 1024 must be admissible");
	{
		const unsigned limits[] = {256, 1024, 0, 1};
		for(unsigned k = 0; k < sizeof limits / sizeof *limits; k++)
			limit_equivalence(sc, limits[k]);
	}
	world_destroy(&gstate.world); split_destroy(s);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player); gstate.players[0].local_player = NULL;
	uint32_t made, freed, refs;
	column_backing_test_counts(&made, &freed, &refs);
	assert(made == freed && refs == 0);
	column_dedup_prefix_equiv();
	column_backing_test_counts(&made, &freed, &refs);
	assert(made == freed && refs == 0);
	return 0;
}
