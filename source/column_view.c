/* Copyright (c) 2026 CaveWii_v2 contributors
 * Presentation pages have 256 DECODED cells in one plane (census convention).
 * The base and emergency private image retain Beta's packed plane layout.
 * All view data/list/ref access is protected by its backing's existing stripe.
 * Pool ownership uses atomic bitmap bits: no allocator or second mutex under
 * a stripe, no ABA free-list, and no process-wide spinlock on Broadway.
 */
#include "column_view.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/time.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CELLS 32768u
#define PAGES_PER_PLANE (CELLS / COLUMN_VIEW_PAGE_CELLS)
#define SLOT_MASK 0xffffu

struct column_view {
	union {
		struct {
			struct column_backing* backing;
			struct column_view* next;
			uint8_t* private_data;
			uint32_t references;
			uint32_t presentation_epoch;
			struct column_revision revision;
			uint32_t mutation_serial, bound_serial;
		};
		uint8_t header[64];
	};
	/* Low 16 bits: pool slot + 1 (zero = base). High 16: differing cells.
	 * Point writes update this count without scanning a page. */
	uint32_t pages[COLUMN_VIEW_PAGES];
};
_Static_assert(sizeof(struct column_view) == COLUMN_VIEW_INDEX_BYTES,
	"census index/header budget");

static uint8_t* s_pool; // created on main before any view can be registered
static uint32_t s_used[COLUMN_VIEW_POOL_PAGES / 32];
static uint32_t s_hint;
/* One complete packed presentation covers all 512 decoded pages, beyond the
 * census's 152-page divergent column. Never lent to initial installation. */
static uint8_t* s_emergency;
static struct column_view_stats s_stats;
static uint32_t s_emergency_used, s_slow_writers, s_recovery, s_presentation_epoch;

static uint32_t s_light_enabled, s_light_epoch = 1;
static struct column_light_stats s_light_stats;
_Static_assert(sizeof(struct column_light_publication) * COLUMN_LIGHT_PUBLICATIONS
	+ 1024 <= COLUMN_LIGHT_PUBLICATION_BYTES, "R23 publication and probe cap");

bool column_light_enabled(void) {
	return __atomic_load_n(&s_light_enabled, __ATOMIC_ACQUIRE) != 0;
}
void column_light_configure(bool enabled) {
	/* Startup/quiescent only. Old queued certificates reject a new mode epoch. */
	uint32_t epoch = __atomic_load_n(&s_light_epoch, __ATOMIC_RELAXED);
	if(epoch && epoch < UINT32_MAX)
		__atomic_store_n(&s_light_epoch, epoch + 1, __ATOMIC_RELEASE);
	else
		__atomic_store_n(&s_light_epoch, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&s_light_enabled, enabled, __ATOMIC_RELEASE);
}
uint32_t column_light_mutation_epoch(void) {
	return __atomic_load_n(&s_light_epoch, __ATOMIC_ACQUIRE);
}
uint32_t column_view_presentation_epoch(void) {
	return __atomic_load_n(&s_presentation_epoch, __ATOMIC_ACQUIRE);
}
static void revision_advance(uint32_t* revision) {
	if(!*revision || *revision == UINT32_MAX) *revision = 0;
	else ++*revision;
}
void column_light_mutated(struct column_backing* b, bool light) {
	if(!column_light_enabled()) return;
	if(b) revision_advance(light ? &b->revision.light : &b->revision.content);
	uint32_t old = __atomic_load_n(&s_light_epoch, __ATOMIC_RELAXED);
	while(old && !__atomic_compare_exchange_n(&s_light_epoch, &old,
		old == UINT32_MAX ? 0 : old + 1, false,
		__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
}
static void light_add(uint32_t* value, uint32_t amount) {
	uint32_t old = __atomic_load_n(value, __ATOMIC_RELAXED);
	while(!__atomic_compare_exchange_n(value, &old,
		amount > UINT32_MAX - old ? UINT32_MAX : old + amount, false,
		__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
void column_light_note_fallback(enum column_light_fallback reason) {
	assert(reason < COLUMN_LIGHT_FALLBACKS);
	light_add(&s_light_stats.fallback[reason], 1);
}
static void light_peak(uint32_t* peak, uint32_t value) {
	uint32_t old = __atomic_load_n(peak, __ATOMIC_RELAXED);
	while(old < value && !__atomic_compare_exchange_n(peak, &old, value,
		false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
struct column_light_publication* column_light_publication_alloc(void) {
	uint32_t live = __atomic_load_n(&s_light_stats.live, __ATOMIC_RELAXED);
	do {
		if(live >= COLUMN_LIGHT_PUBLICATIONS) {
			column_light_note_fallback(COLUMN_LIGHT_CAP);
			return NULL;
		}
	} while(!__atomic_compare_exchange_n(&s_light_stats.live, &live, live + 1,
		false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
	uint32_t bytes = __atomic_add_fetch(&s_light_stats.bytes,
		sizeof(struct column_light_publication), __ATOMIC_RELAXED);
	light_peak(&s_light_stats.peak_live, live + 1);
	light_peak(&s_light_stats.peak_bytes, bytes);
	struct column_light_publication* p = calloc(1, sizeof(*p));
	if(!p) {
		__atomic_fetch_sub(&s_light_stats.bytes, sizeof(*p), __ATOMIC_RELAXED);
		__atomic_fetch_sub(&s_light_stats.live, 1, __ATOMIC_RELEASE);
		column_light_note_fallback(COLUMN_LIGHT_OOM);
	}
	return p;
}
void column_light_publication_release(struct column_light_publication* p) {
	if(!p) return;
	for(unsigned n = 0; n < p->dependencies; n++)
		column_backing_unref(p->dependency[n].backing);
	free(p);
	__atomic_fetch_sub(&s_light_stats.bytes, sizeof(*p), __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_light_stats.live, 1, __ATOMIC_RELEASE);
}
void column_light_note_probe(uint32_t pops, uint32_t us, bool published) {
	light_add(&s_light_stats.probe_pops, pops);
	light_add(&s_light_stats.copy_us, us);
	if(published) light_add(&s_light_stats.published, 1);
}
void column_light_note_hit(uint32_t pops) {
	light_add(&s_light_stats.hits, 1);
	light_add(&s_light_stats.avoided_pops, pops);
}
void column_light_note_validation(uint32_t us) {
	light_add(&s_light_stats.validate_us, us);
}
void column_light_get_stats(struct column_light_stats* out) {
#define LIGHT_GET(f) out->f = __atomic_load_n(&s_light_stats.f, __ATOMIC_RELAXED)
	LIGHT_GET(published); LIGHT_GET(hits); LIGHT_GET(avoided_pops);
	LIGHT_GET(probe_pops); LIGHT_GET(live); LIGHT_GET(peak_live);
	LIGHT_GET(bytes); LIGHT_GET(peak_bytes); LIGHT_GET(copy_us); LIGHT_GET(validate_us);
#undef LIGHT_GET
	for(unsigned n = 0; n < COLUMN_LIGHT_FALLBACKS; n++)
		out->fallback[n] = __atomic_load_n(&s_light_stats.fallback[n], __ATOMIC_RELAXED);
}

static bool revision_valid(const struct column_revision* r) {
	return r->owner && r->incarnation && r->content && r->light && r->events;
}
static bool revision_equal(const struct column_revision* a,
	const struct column_revision* b) {
	return a->owner == b->owner && a->incarnation == b->incarnation
		&& a->content == b->content && a->light == b->light && a->events == b->events;
}
void column_view_bind_revision(struct column_view* v, struct column_revision r) {
	thread_stripe_lock(v->backing->mutex);
	v->revision = r;
	v->bound_serial = v->mutation_serial;
	thread_stripe_unlock(v->backing->mutex);
}
bool column_view_apply_revision(struct column_view* v, struct column_revision r,
	uint32_t previous_content) {
	thread_stripe_lock(v->backing->mutex);
	bool valid = revision_valid(&r) && revision_valid(&v->revision)
		&& v->revision.owner == r.owner && v->revision.incarnation == r.incarnation
		&& v->revision.events != UINT32_MAX && r.events == v->revision.events + 1
		&& previous_content && previous_content == v->revision.content
		&& r.content >= v->revision.content && r.light >= v->revision.light
		&& v->bound_serial != UINT32_MAX && v->mutation_serial == v->bound_serial + 1;
	v->revision = valid ? r : (struct column_revision){0};
	v->bound_serial = v->mutation_serial;
	thread_stripe_unlock(v->backing->mutex);
	return valid;
}
bool column_view_revision_matches(struct column_view* v,
	const struct column_revision* r) {
	thread_stripe_lock(v->backing->mutex);
	bool valid = revision_valid(r) && revision_equal(&v->revision, r)
		&& revision_equal(&v->backing->revision, r) && v->backing->resident
		&& v->backing->settled_light_revision == r->light
		&& v->mutation_serial != UINT32_MAX
		&& v->mutation_serial == v->bound_serial
		&& v->presentation_epoch == column_view_presentation_epoch();
	thread_stripe_unlock(v->backing->mutex);
	return valid;
}

bool column_view_pool_present(void) { return s_pool != NULL; }
bool column_view_recovery_pending(void) {
	return __atomic_load_n(&s_recovery, __ATOMIC_ACQUIRE) != 0;
}
bool column_view_recovery_begin(void) {
	/* 1=requested, 2=accepted by main, 3=server invalidation in progress,
	 * 4=server invalidated, awaiting main. Never retire/reset across state 3. */
	uint32_t requested = __atomic_load_n(&s_recovery, __ATOMIC_ACQUIRE);
	if(requested != 1 && requested != 4) return false;
	bool invalidated = requested == 4;
	if(!__atomic_compare_exchange_n(&s_recovery, &requested, 2, false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
	__atomic_fetch_add(&s_stats.recovery_entries, 1, __ATOMIC_RELAXED);
	/* Main has accepted recovery. All current presentations are obsolete,
	 * including shells awaiting worker/GPU retirement; later installs use
	 * the next epoch. Only presentation is discarded, never server bytes. */
	if(!invalidated) __atomic_fetch_add(&s_presentation_epoch, 1, __ATOMIC_RELEASE);
	return true;
}
void column_view_recovery_reset(void) {
	__atomic_store_n(&s_recovery, 0, __ATOMIC_RELEASE);
}
static void request_recovery(void) {
	uint32_t idle = 0;
	if(__atomic_compare_exchange_n(&s_recovery, &idle, 1, false,
		__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
		__atomic_fetch_add(&s_stats.recovery_cycles, 1, __ATOMIC_RELAXED);
		fprintf(stderr, "[column_view] storage exhausted; recovering voxel stream\n");
	}
}
static bool expire_recovery_wait(void) {
	uint32_t requested = 1;
	if(__atomic_compare_exchange_n(&s_recovery, &requested, 3, false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		/* The same invalidation main performs, without touching world lists,
		 * ownership or GPU data. Pending recovery already gates presentation.
		 * A pinned worker may finish, but its obsolete shell is retired by main. */
		__atomic_fetch_add(&s_presentation_epoch, 1, __ATOMIC_RELEASE);
		__atomic_fetch_add(&s_stats.recovery_expiries, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&s_recovery, 4, __ATOMIC_RELEASE);
		return true;
	}
	return false;
}
static uint8_t* emergency_take(void) {
	uint32_t idle = 0;
	if(!__atomic_compare_exchange_n(&s_emergency_used, &idle, 1, false,
		__ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return NULL;
	if(s_emergency) __atomic_fetch_add(&s_stats.emergency_uses, 1, __ATOMIC_RELAXED);
	return s_emergency;
}
static void fallback_free(uint8_t* data) {
	if(data && data == s_emergency)
		__atomic_store_n(&s_emergency_used, 0, __ATOMIC_RELEASE);
	else mem2_free_sized(data, COLUMN_VIEW_PRIVATE_BYTES);
}
#ifdef CAVEWII_SERVER_LOCAL_TEST
static unsigned s_page_limit = COLUMN_VIEW_POOL_PAGES;
void column_view_test_page_limit(unsigned limit) {
	assert(limit <= COLUMN_VIEW_POOL_PAGES);
	s_page_limit = limit; // test sets this only while both participants are idle
}
#endif

static void stat_add(uint32_t* counter, int delta) {
	__atomic_fetch_add(counter, delta, __ATOMIC_RELAXED);
}
void column_view_get_stats(struct column_view_stats* out) {
#define GET(f) out->f = __atomic_load_n(&s_stats.f, __ATOMIC_RELAXED)
	GET(live_views); GET(pages); GET(peak_pages); GET(fallback_live); GET(fallback_total);
	GET(page_failures); GET(emergency_uses); GET(recovery_entries);
	GET(recovery_cycles); GET(recovery_expiries); GET(recovery_wait_max_us);
#undef GET
}

static unsigned pool_take(void) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(__atomic_load_n(&s_stats.pages, __ATOMIC_RELAXED) >= s_page_limit) {
		stat_add(&s_stats.page_failures, 1);
		return 0;
	}
#endif
	unsigned start = __atomic_fetch_add(&s_hint, 1, __ATOMIC_RELAXED);
	for(unsigned i = 0; i < COLUMN_VIEW_POOL_PAGES / 32; i++) {
		unsigned word = (start + i) % (COLUMN_VIEW_POOL_PAGES / 32);
		uint32_t used = __atomic_load_n(&s_used[word], __ATOMIC_RELAXED);
		while(used != UINT32_MAX) {
			unsigned bit = (unsigned)__builtin_ctz(~used);
			uint32_t next = used | (UINT32_C(1) << bit);
			if(__atomic_compare_exchange_n(&s_used[word], &used, next, false,
				__ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				uint32_t n = __atomic_add_fetch(&s_stats.pages, 1, __ATOMIC_RELAXED);
				uint32_t peak = __atomic_load_n(&s_stats.peak_pages, __ATOMIC_RELAXED);
				while(n > peak && !__atomic_compare_exchange_n(&s_stats.peak_pages,
					&peak, n, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
				return word * 32 + bit + 1;
			}
		}
	}
	stat_add(&s_stats.page_failures, 1);
	return 0;
}
static uint8_t* pool_data(unsigned entry) {
	return s_pool + ((entry & SLOT_MASK) - 1) * COLUMN_VIEW_PAGE_CELLS;
}
static void pool_release(uint32_t* entry) {
	unsigned slot = (*entry & SLOT_MASK) - 1;
	*entry = 0;
	stat_add(&s_stats.pages, -1); // retire accounting before the slot can be reused
	__atomic_fetch_and(&s_used[slot / 32], ~(UINT32_C(1) << (slot % 32)),
		__ATOMIC_RELEASE);
}
static uint8_t* base_plane(struct column_backing* b, unsigned plane) {
	switch(plane) {
	case COLUMN_IDS: return b->ids;
	case COLUMN_META: return b->metadata;
	case COLUMN_SKY: return b->lighting_sky;
	default: return b->lighting_torch;
	}
}
static uint8_t* private_plane(struct column_view* v, unsigned plane) {
	return v->private_data + (plane ? CELLS + (plane - 1) * CELLS / 2 : 0);
}
static uint8_t read_plane(const uint8_t* data, unsigned plane, unsigned index) {
	return plane ? (data[index / 2] >> ((index & 1) * 4)) & 15 : data[index];
}
static void write_plane(uint8_t* data, unsigned plane, unsigned index, uint8_t value) {
	if(!plane) data[index] = value;
	else {
		unsigned shift = (index & 1) * 4;
		data[index / 2] = (data[index / 2] & ~(15u << shift)) | (value << shift);
	}
}
static uint8_t view_read(struct column_view* v, unsigned plane, unsigned index) {
	if(v->private_data) return read_plane(private_plane(v, plane), plane, index);
	unsigned entry = v->pages[plane * PAGES_PER_PLANE + index / 256];
	return entry ? pool_data(entry)[index % 256]
		: read_plane(base_plane(v->backing, plane), plane, index);
}
bool column_view_content_matches(struct column_view* v, const uint8_t* heightmap) {
	thread_stripe_lock(v->backing->mutex);
	bool equal = !memcmp(heightmap, v->backing->heightmap, 256);
	for(unsigned i = 0; equal && i < CELLS; i++)
		equal = view_read(v, COLUMN_IDS, i) == v->backing->ids[i]
			&& view_read(v, COLUMN_META, i) == read_plane(v->backing->metadata, COLUMN_META, i);
	thread_stripe_unlock(v->backing->mutex);
	return equal;
}
static bool view_obsolete(struct column_view* v) {
	return v->presentation_epoch != __atomic_load_n(&s_presentation_epoch, __ATOMIC_ACQUIRE);
}
static bool preserve_page(struct column_view* v, unsigned page) {
	if(view_obsolete(v)) return true;
	if(v->private_data || v->pages[page]) return true;
	unsigned slot = pool_take();
	if(!slot) return false;
	unsigned plane = page / PAGES_PER_PLANE, first = (page % PAGES_PER_PLANE) * 256;
	uint8_t* dst = pool_data(slot);
	const uint8_t* src = base_plane(v->backing, plane);
	for(unsigned i = 0; i < 256; i++) dst[i] = read_plane(src, plane, first + i);
	v->pages[page] = slot;
	return true;
}
static void rejoin_page(struct column_view* v, unsigned page) {
	if(view_obsolete(v) || v->private_data || !v->pages[page]) return;
	unsigned plane = page / PAGES_PER_PLANE, first = (page % PAGES_PER_PLANE) * 256;
	uint8_t* data = pool_data(v->pages[page]);
	const uint8_t* base = base_plane(v->backing, plane);
	unsigned differences = 0;
	for(unsigned i = 0; i < 256; i++) differences += data[i] != read_plane(base, plane, first + i);
	if(!differences) pool_release(&v->pages[page]);
	else v->pages[page] = (v->pages[page] & SLOT_MASK) | (differences << 16);
}
static void materialize(struct column_view* v, uint8_t* private_data) {
	/* stripe held; private_data was allocated before locking. */
	for(unsigned p = 0; p < 4; p++) {
		uint8_t* dst = private_data + (p ? CELLS + (p - 1) * CELLS / 2 : 0);
		memcpy(dst, base_plane(v->backing, p), p ? CELLS / 2 : CELLS);
		for(unsigned page = 0; page < PAGES_PER_PLANE; page++) {
			uint32_t* entry = &v->pages[p * PAGES_PER_PLANE + page];
			if(!*entry) continue;
			const uint8_t* src = pool_data(*entry);
			for(unsigned i = 0; i < 256; i++) write_plane(dst, p, page * 256 + i, src[i]);
			pool_release(entry);
		}
	}
	v->private_data = private_data;
	stat_add(&s_stats.fallback_live, 1);
	stat_add(&s_stats.fallback_total, 1);
}
bool column_view_pool_shutdown(void) {
	/* Installation and final view retirement run on main. A server that left
	 * its stripe to allocate may still hold an emergency pointer: defer free
	 * until it has retried, released any spare, and left the slow path. */
	if(__atomic_load_n(&s_stats.live_views, __ATOMIC_ACQUIRE)
	   || __atomic_load_n(&s_slow_writers, __ATOMIC_ACQUIRE)) return false;
	assert(!s_stats.pages && !s_emergency_used);
	mem2_free_sized(s_pool, COLUMN_VIEW_POOL_PAGES * COLUMN_VIEW_PAGE_CELLS);
	mem2_free_sized(s_emergency, COLUMN_VIEW_PRIVATE_BYTES);
	s_pool = s_emergency = NULL;
	return true;
}

void column_backing_write_begin(struct column_backing* b, unsigned planes,
	unsigned first, unsigned count) {
	assert(first < CELLS && count && count <= CELLS - first && !(planes & ~15u));
	if(!b) return;
	/* The whole COW transaction stays at the server's base priority, including
	 * the unlocked MEM2/libc allocation gap and MAIN-owned recovery request.
	 * On success this function RETURNS WITH THE STRIPE HELD; dropping this
	 * outer guard cannot reboost until write_end releases that stripe. */
	thread_g9_lease_dependency();
	uint8_t* reserve = NULL;
	bool attempted = false, slow = false;
	bool waiting = false;
	ptime_t deadline, wait_start;
	for(;;) {
		thread_stripe_lock(b->mutex);
		bool complete = true;
		for(struct column_view* v = b->views; v && complete; v = v->next)
			for(unsigned p = 0; p < 4 && complete; p++) {
				if(!(planes & (1u << p))) continue;
				for(unsigned page = first / 256; page <= (first + count - 1) / 256; page++) {
					if(preserve_page(v, p * PAGES_PER_PLANE + page)) continue;
					/* This rescan precedes BOTH reserve use and recovery. A
					 * retirement/rejoin in the allocation gap needs neither. */
					if(!reserve && attempted) reserve = emergency_take();
					if(reserve) { materialize(v, reserve); reserve = NULL; attempted = false; break; }
					complete = false;
					break;
				}
			}
		if(complete && !reserve) {
			if(waiting) {
				/* Includes sleep overshoot and final stripe reacquisition. Saturate
				 * before conversion; never wrap a long stall into a small reading. */
				double us = (double)time_diff_s(wait_start, time_get()) * 1000000.0;
				uint32_t elapsed = us >= UINT32_MAX ? UINT32_MAX : (uint32_t)us;
				uint32_t peak = __atomic_load_n(&s_stats.recovery_wait_max_us, __ATOMIC_RELAXED);
				while(elapsed > peak && !__atomic_compare_exchange_n(
					&s_stats.recovery_wait_max_us, &peak, elapsed, false,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
			}
			if(slow) __atomic_fetch_sub(&s_slow_writers, 1, __ATOMIC_RELEASE);
			thread_g9_lease_dependency_done();
			return;
		}
		if(!slow) {
			__atomic_fetch_add(&s_slow_writers, 1, __ATOMIC_RELEASE);
			slow = true;
		}
		thread_stripe_unlock(b->mutex);
		if(reserve) { fallback_free(reserve); reserve = NULL; attempted = false; }
		else if(!attempted) {
			reserve = mem2_tag(mem2_alloc(COLUMN_VIEW_PRIVATE_BYTES), COLUMN_VIEW_PRIVATE_BYTES, CARVE_PRIVATE); // NULL is recoverable
			attempted = reserve == NULL; // emergency token requires a fresh failure
		} else {
			request_recovery();
			if(!waiting) {
				wait_start = time_get();
				deadline = time_add_ms(wait_start, COLUMN_VIEW_RECOVERY_WAIT_MS);
				waiting = true;
			} else if(time_reached(time_get(), deadline)) {
				if(expire_recovery_wait()) continue;
			}
			/* Keep the simulation write on its original call stack. Main can
			 * drain RPCs/retire views while this stripe is unlocked; no write
			 * is dropped, replayed, or reordered, including bulk callers. */
			thread_drain_recovery_sleep();
		}
	}
}
void column_backing_write_end(struct column_backing* b, unsigned planes,
	unsigned first, unsigned count) {
	if(!b) return;
	/* planes=0 includes height, and bulk light rebuilds may change height too. */
	column_light_mutated(b, false);
	if(planes & 12u) column_light_mutated(b, true);
	for(struct column_view* v = b->views; v; v = v->next)
		for(unsigned p = 0; p < 4; p++)
			if(planes & (1u << p))
				for(unsigned page = first / 256; page <= (first + count - 1) / 256; page++)
					rejoin_page(v, p * PAGES_PER_PLANE + page);
	thread_stripe_unlock(b->mutex);
}

/* One plane of the point writer. Entered and left with the stripe held (the
 * slow path drops it only inside column_backing_write_begin, which returns
 * with it held again). Body identical to the former set_cell. */
static inline void backing_set_cell_locked(struct column_backing* b,
	unsigned plane, unsigned index, uint8_t value) {
	uint8_t* base = base_plane(b, plane);
	uint8_t old = read_plane(base, plane, index);
	if(old == value) return;
	unsigned page = plane * PAGES_PER_PLANE + index / 256;
	for(struct column_view* v = b->views; v; v = v->next) {
		if(!preserve_page(v, page)) {
			/* The slow path may allocate with the stripe released. Only the
			 * server writes base bytes, so old remains valid across that gap. */
			thread_g9_lease_dependency();
			thread_stripe_unlock(b->mutex);
			column_backing_write_begin(b, 1u << plane, index, 1);
			thread_g9_lease_dependency_done();
			break;
		}
	}
	for(struct column_view* v = b->views; v; v = v->next) {
		if(view_obsolete(v) || v->private_data) continue;
		uint8_t presented = pool_data(v->pages[page])[index % 256];
		int delta = (presented != value) - (presented != old);
		v->pages[page] += delta * 65536;
		if(!(v->pages[page] >> 16)) pool_release(&v->pages[page]);
	}
	write_plane(base, plane, index, value);
	column_light_mutated(b, plane >= COLUMN_SKY);
}

void column_backing_set_cell(struct column_backing* b, unsigned plane,
	unsigned index, uint8_t value) {
	assert(b && plane < 4 && index < CELLS && (!plane || value < 16));
	thread_stripe_lock(b->mutex);
	backing_set_cell_locked(b, plane, index, value);
	thread_stripe_unlock(b->mutex);
}

/* g9-compute-1009 (T1): the drain's SKY-then-TORCH pair under ONE stripe
 * hold. Same per-plane bodies in the same order as two set_cell calls, so
 * the base bytes, view divergence counts, page COW and light revisions end
 * identical; a concurrent reader can only miss the intermediate
 * (new SKY, old TORCH) state, which no reader relies on. */
void column_backing_set_light_locked(struct column_backing* b, unsigned index,
	unsigned planes, uint8_t sky, uint8_t torch) {
	assert(b && index < CELLS && sky < 16 && torch < 16 && planes && planes < 4);
	if(planes & 1u) backing_set_cell_locked(b, COLUMN_SKY, index, sky);
	if(planes & 2u) backing_set_cell_locked(b, COLUMN_TORCH, index, torch);
}
void column_backing_set_light(struct column_backing* b, unsigned index,
	unsigned planes, uint8_t sky, uint8_t torch) {
	assert(b);
	thread_stripe_lock(b->mutex);
	column_backing_set_light_locked(b, index, planes, sky, torch);
	thread_stripe_unlock(b->mutex);
}

struct column_view* column_view_create(struct column_backing* b,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky,
	const uint8_t* torch) {
	assert(b && ids && metadata && sky && torch);
	/* Main-thread-only installation. Reserve recovery capacity BEFORE the
	 * first view is published. MEM2 allocations use the carve, then general MEM2. */
	if(column_view_recovery_pending()) return NULL;
	if(!s_pool) {
		s_pool = mem2_tag(mem2_alloc(COLUMN_VIEW_POOL_PAGES * COLUMN_VIEW_PAGE_CELLS), COLUMN_VIEW_POOL_PAGES * COLUMN_VIEW_PAGE_CELLS, CARVE_OVERLAY);
		if(!s_pool) return NULL;
		s_emergency = mem2_tag(mem2_alloc(COLUMN_VIEW_PRIVATE_BYTES), COLUMN_VIEW_PRIVATE_BYTES, CARVE_PRIVATE);
		if(!s_emergency) { column_view_pool_shutdown(); return NULL; }
	}
	/* VIEW-SLOT STORE (platform/mem2.h, FIXED-TENANT POOLS): this call site is
	 * the view pool's tenant, so the index block comes from a bitmap slot
	 * rather than a first-fit block wedged between two display lists. A full
	 * pool falls through to the shared heap and is counted (pool_view_over).
	 * Freed by the ordinary mem2_free_sized calls below -- a pooled pointer is
	 * recognised by range. */
	struct column_view* v = mem2_tag(mem2_alloc_pooled(sizeof(*v), MEM2_POOL_VIEW), sizeof(*v), CARVE_VIEW);
	if(!v) return NULL;
	memset(v, 0, sizeof(*v));
	v->backing = b;
	v->references = 1;
	v->presentation_epoch = __atomic_load_n(&s_presentation_epoch, __ATOMIC_ACQUIRE);
	const uint8_t* src[] = {ids, metadata, sky, torch};
	column_backing_ref(b);
	thread_stripe_lock(b->mutex);
	for(unsigned p = 0; p < 4; p++)
		for(unsigned page = 0; page < PAGES_PER_PLANE; page++) {
			unsigned first = page * 256, differences = 0;
			const uint8_t* base = base_plane(b, p);
			for(unsigned i = 0; i < 256; i++)
				differences += read_plane(src[p], p, first + i) != read_plane(base, p, first + i);
			if(!differences) continue;
			unsigned slot = pool_take();
			if(!slot) {
				/* Not registered yet: slot snapshot is immutable, so allocation
				 * can leave the stripe without retaining any presentation state. */
				for(unsigned j = 0; j < COLUMN_VIEW_PAGES; j++)
					if(v->pages[j]) pool_release(&v->pages[j]);
				thread_stripe_unlock(b->mutex);
				v->private_data = mem2_tag(mem2_alloc(COLUMN_VIEW_PRIVATE_BYTES), COLUMN_VIEW_PRIVATE_BYTES, CARVE_PRIVATE);
				if(!v->private_data) { mem2_free_sized(v, sizeof(*v)); column_backing_unref(b); return NULL; }
				for(unsigned k = 0; k < 4; k++)
					memcpy(private_plane(v, k), src[k], k ? CELLS / 2 : CELLS);
				stat_add(&s_stats.fallback_live, 1);
				stat_add(&s_stats.fallback_total, 1);
				thread_stripe_lock(b->mutex);
				goto install;
			}
			v->pages[p * PAGES_PER_PLANE + page] = slot | (differences << 16);
			uint8_t* dst = pool_data(slot);
			for(unsigned i = 0; i < 256; i++) dst[i] = read_plane(src[p], p, first + i);
		}
install:
	v->next = b->views;
	b->views = v;
	stat_add(&s_stats.live_views, 1);
	thread_stripe_unlock(b->mutex);
	return v;
}
void column_view_ref(struct column_view* v) {
	column_backing_ref(v->backing);
	thread_stripe_lock(v->backing->mutex);
	v->references++;
	thread_stripe_unlock(v->backing->mutex);
}
void column_view_unref(struct column_view* v) {
	if(!v) return;
	struct column_backing* b = v->backing;
	thread_stripe_lock(b->mutex);
	bool last = --v->references == 0;
	if(last) {
		struct column_view** at = &b->views;
		while(*at != v) at = &(*at)->next;
		*at = v->next;
		for(unsigned i = 0; i < COLUMN_VIEW_PAGES; i++) if(v->pages[i]) pool_release(&v->pages[i]);
		stat_add(&s_stats.live_views, -1);
		if(v->private_data) stat_add(&s_stats.fallback_live, -1);
	}
	thread_stripe_unlock(b->mutex);
	if(last) {
		fallback_free(v->private_data);
		mem2_free_sized(v, sizeof(*v));
	}
	column_backing_unref(b);
	if(last) column_view_pool_shutdown();
}
struct block_data column_view_get_locked(struct column_view* v, unsigned index) {
	assert(index < CELLS);
	/* block_data has an unused half-byte. Keep the complete gathered byte
	 * representation deterministic as well as the four logical fields. */
	struct block_data result;
	memset(&result, 0, sizeof(result));
	result.type = view_read(v, COLUMN_IDS, index);
	result.metadata = view_read(v, COLUMN_META, index);
	result.sky_light = view_read(v, COLUMN_SKY, index);
	result.torch_light = view_read(v, COLUMN_TORCH, index);
	return result;
}
struct block_data column_view_get(struct column_view* v, unsigned index) {
	thread_stripe_lock(v->backing->mutex);
	struct block_data result = column_view_get_locked(v, index);
	thread_stripe_unlock(v->backing->mutex);
	return result;
}

void column_view_get_light(struct column_view* v, unsigned index,
	uint8_t* sky, uint8_t* torch) {
	assert(index < CELLS);
	thread_stripe_lock(v->backing->mutex);
	*sky = view_read(v, COLUMN_SKY, index);
	*torch = view_read(v, COLUMN_TORCH, index);
	thread_stripe_unlock(v->backing->mutex);
}

bool column_view_copy(struct column_view* v, uint8_t* ids, uint8_t* metadata,
	uint8_t* sky, uint8_t* torch) {
	uint8_t* destinations[] = {ids, metadata, sky, torch};
	bool changed = false;
	thread_stripe_lock(v->backing->mutex);
	for(unsigned p = 0; p < 4; p++) {
		const uint8_t* base = v->private_data ? private_plane(v, p) : base_plane(v->backing, p);
		unsigned length = p ? 128 : 256;
		for(unsigned page = 0; page < PAGES_PER_PLANE; page++) {
			uint8_t packed[128];
			unsigned entry = v->pages[p * PAGES_PER_PLANE + page];
			const uint8_t* src = base + page * length;
			if(entry) {
				src = pool_data(entry);
				if(p) {
					for(unsigned i = 0; i < 128; i++) packed[i] = src[2*i] | (src[2*i+1] << 4);
					src = packed;
				}
			}
			uint8_t* dst = destinations[p] + page * length;
			if(memcmp(dst, src, length)) { memcpy(dst, src, length); changed = true; }
		}
	}
	thread_stripe_unlock(v->backing->mutex);
	return changed;
}

void column_view_torch_census(struct column_view* v, uint32_t* view_lit,
	uint32_t* backing_lit) {
	uint32_t vl = 0, bl = 0;
	thread_stripe_lock(v->backing->mutex);
	const uint8_t* bt = v->backing->lighting_torch;
	for(unsigned i = 0; i < CELLS / 2; i++)
		bl += ((bt[i] & 15u) != 0) + ((bt[i] >> 4) != 0);
	const uint8_t* base = v->private_data ? private_plane(v, COLUMN_TORCH) : bt;
	for(unsigned page = 0; page < PAGES_PER_PLANE; page++) {
		unsigned entry = v->pages[COLUMN_TORCH * PAGES_PER_PLANE + page];
		if(entry) { /* pool pages hold one unpacked value per byte */
			const uint8_t* src = pool_data(entry);
			for(unsigned i = 0; i < COLUMN_VIEW_PAGE_CELLS; i++)
				vl += src[i] != 0;
		} else {
			const uint8_t* src = base + page * (COLUMN_VIEW_PAGE_CELLS / 2);
			for(unsigned i = 0; i < COLUMN_VIEW_PAGE_CELLS / 2; i++)
				vl += ((src[i] & 15u) != 0) + ((src[i] >> 4) != 0);
		}
	}
	thread_stripe_unlock(v->backing->mutex);
	*view_lit = vl;
	*backing_lit = bl;
}

void column_view_set(struct column_view* v, unsigned index, struct block_data block,
	bool light_only) {
	/* MAIN-only ordinal; retries below belong to this one mutation. */
	if(!light_only && column_light_enabled()) {
		thread_stripe_lock(v->backing->mutex);
		if(v->mutation_serial != UINT32_MAX) ++v->mutation_serial;
		thread_stripe_unlock(v->backing->mutex);
	}
	assert(index < CELLS);
	const uint8_t values[] = {block.type, block.metadata, block.sky_light, block.torch_light};
	struct column_backing* b = v->backing;
	uint8_t* reserve = NULL;
	bool attempted = false;
retry:
	thread_stripe_lock(b->mutex);
	if(column_view_recovery_pending() || view_obsolete(v)) {
		thread_stripe_unlock(b->mutex);
		fallback_free(reserve);
		return; // obsolete presentation only; main retires it before rendering
	}
	for(unsigned p = light_only ? 2 : 0; p < 4; p++) {
		if(v->private_data) {
			uint8_t* data = private_plane(v, p);
			if(!light_only || read_plane(data, p, index) != values[p])
				write_plane(data, p, index, values[p]);
			continue;
		}
		uint8_t old = view_read(v, p, index);
		if(old == values[p]) continue;
		unsigned page = p * PAGES_PER_PLANE + index / 256;
		if(!preserve_page(v, page)) {
			if(!reserve && attempted) reserve = emergency_take();
			if(reserve) {
				materialize(v, reserve);
				reserve = NULL;
				write_plane(private_plane(v, p), p, index, values[p]);
				continue;
			}
			thread_stripe_unlock(b->mutex);
			if(attempted) { request_recovery(); return; }
			reserve = mem2_tag(mem2_alloc(COLUMN_VIEW_PRIVATE_BYTES), COLUMN_VIEW_PRIVATE_BYTES, CARVE_PRIVATE);
			attempted = reserve == NULL;
			goto retry; // server may have freed pages or satisfied this write
		}
		uint8_t base = read_plane(base_plane(b, p), p, index);
		int delta = (values[p] != base) - (old != base);
		v->pages[page] += delta * 65536;
		pool_data(v->pages[page])[index % 256] = values[p];
		if(!(v->pages[page] >> 16)) pool_release(&v->pages[page]);
	}
	thread_stripe_unlock(b->mutex);
	fallback_free(reserve);
}
