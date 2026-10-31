/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "carve_observer.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chunk.h"
#include "chunk_mesher.h"
#include "network/client_interface.h"
#include "column_view.h"
#include "network/server_world.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "platform/hw_telemetry.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "world.h"

/* The ledger lives on the general heap, never in the carve being measured.
 * No engine pointer is retained or dereferenced by a worker callback. The main
 * thread alone walks its resident/render lists. Hash tombstones prevent address
 * reuse hiding frees. Allocation failures invalidate evidence, never gameplay. */
struct allocation {
	uintptr_t key;
	size_t bytes, used, gx_capacity, gx_used, gx_start;
	unsigned tenant, flags, marks, owner;
	bool control;
};
static struct {
	bool active;
	struct thread_mutex lock;
	struct allocation* slots;
	/* capacity is fixed at arm time (CARVE_LEDGER_SLOTS) or 0 when the one
	 * reservation failed -- see LEDGER TABLE below. `occupied` is gone with
	 * the tombstones: backward-shift deletion keeps occupied == live. */
	size_t capacity, live, ledger_peak;
	uint64_t ledger_full; /* inserts refused because the fixed table was full */
	struct carve_totals totals;
	uint64_t retained, retained_gx, gpu, gpu_gx, union_count;
	uint64_t far_air, far_count, skin_bytes, full5, far6_slices;
	int cx[2], cz;
	uint32_t owner_tick, frame, eligible, retired, save_failed;
	unsigned emit_point;
	struct retirement_burst { uint32_t frame, tick, count, retired, failed, us,
		folded; } *bursts;
	size_t burst_count, burst_capacity, burst_sent;
	/* LOADING-PHASE CADENCE (2026-09-08). Three real-Wii split_anchor_6m runs
	 * lost the stream between 53 and 68 heartbeats, always DURING the
	 * two-anchor reload: the observer offers ~12 records/s (11 carve_tenant +
	 * 1 carve_census, plus attribution and retirement records) exactly while
	 * the server thread is in a libfat fsync storm and the sender's net_send
	 * shares that IOS IPC path (tel_smax_us 17 -> 48 -> 166 ms, tel_it 142 ->
	 * 11 -> 0). The reload is also the phase where NOTHING in a per-tenant
	 * sample is yet meaningful -- the heap is still being built. So while
	 * `loading` is set (main: world not loaded, or the anchors not ready --
	 * i.e. the loading high-water point not yet passed), the observer emits
	 * only what the memory gate actually needs from this phase:
	 *   - the three snapshot-point TRANSITIONS (loading high-water /
	 *     readiness / low-water), each still never delayed or coalesced away;
	 *   - at most one carve_census per second (CARVE_LOADING_CENSUS_FRAMES).
	 * The eleven carve_tenant records of each set are suppressed (counted in
	 * loading_suppressed, reported on the census line), the per-point repeat
	 * sends are held, and retirement bursts FOLD into one pending record
	 * (counts summed, latest totals kept) instead of queueing ~20/s of
	 * startup retirement for a post-readiness flood. Nothing is discarded
	 * that carries information: every suppressed tenant record is re-derived
	 * from the next set, and every folded burst keeps its totals.
	 * Cadence during loading: <= 1 record/s plus the three one-shot
	 * transitions. Readiness restores the full pre-existing cadence. */
	bool loading, loading_census_valid;
	uint32_t loading_census_frame, loading_suppressed;
	bool points_valid[3], points_pending[3], points_dirty[3];
	struct carve_attrib points[3], pending_points[3];
	/* Emission-cadence throttle (see carve_observer.h): coalesced[k]
	 * accumulates observations folded into point k's next outgoing record.
	 * sent_once[k] exempts each point's very first actual send (its true
	 * transition) from its own last_sent_frame[k] clock, which then caps
	 * that point's later sends to at most one per ~1 s. Per-point, not
	 * shared: a single clock let one continuously-churning point (e.g. the
	 * session low-water point, active the whole run) perpetually win every
	 * reopening and starve another (e.g. loading high-water, only active
	 * pre-ready) -- freezing its reported value at its very first sample
	 * forever, which defeats the whole point of tracking an extremum. */
	uint32_t coalesced[3], last_sent_frame[3];
	bool sent_once[3];
	/* Staged census burst (2026-09-07 sender-saturation fix). The per-second
	 * census set is CARVE_TENANTS carve_tenant records PLUS one carve_census
	 * -- twelve lines. Publishing them back-to-back inside one frame handed
	 * twelve records to a four-slot admission FIFO whose sender drains one
	 * blocking net_send at a time: two got in behind that frame's heartbeat
	 * and settle record, the third was refused, and the old whole-set loop
	 * returned at the first refusal so the remaining nine tenants and the
	 * census NEVER went out at all (the real Wii saw 188 carve_tenant in
	 * 63 s = exactly 3/s, and zero carve_census). The set is now snapshotted
	 * once and drained ONE record per frame by carve_observer_emit_next, so
	 * the same twelve records go out ~16 ms apart instead of in a burst and
	 * the sender keeps up. staged_next is the drain cursor; a set still
	 * draining is never overwritten (the older COMPLETE set wins; the new
	 * snapshot is skipped and counted). Costs ~3 KB of static observer
	 * state, only meaningful with --cw-carve-census armed. */
	struct carve_totals staged_totals;
	/* 2048 is also what tests/test_carve_observer.c gates every emitted packet
	 * against. After the round-3 pool/ledger keys the census format is 869
	 * literal characters plus 40 uint64, 24 uint32 and 3 size_t conversions:
	 * 1,969 characters even with every field at its absurd maximum width (a
	 * real one runs ~1,200, since these are byte counts inside a 36 MB carve).
	 * A future addition that does not fit is not silently truncated -- the
	 * snprintf return is checked below and the census slot is skipped -- but it
	 * WOULD lose the record, so re-do this arithmetic before adding keys. */
	char staged_census[2048];
	uint32_t staged_tick, staged_skipped;
	unsigned staged_next;
	bool staged_active, staged_measuring;
	/* ADAPTIVE OFFER RATE (2026-09-07 sender-starvation fix). One record per
	 * frame is the right cadence only while the sender is actually keeping
	 * up. During the two-anchor reload it is not: on build 0077cd09
	 * (split_anchor_6m, seed 42, sep 256, far 6, voxel) the ring refused 571
	 * of ~583 offers over 53 s -- an admitted rate near 2 records/s against a
	 * ~12/s offer rate -- so every frame's offer was a refusal that bought a
	 * drop-counter increment and nothing else. The retry interval is
	 * therefore DERIVED FROM THE MEASURED DRAIN RATE rather than fixed: each
	 * refusal doubles the wait (1, 2, 4 ... frames) up to
	 * CARVE_STAGED_BACKOFF_MAX_FRAMES = 60 (one offer per second at the Wii's
	 * 60 Hz main loop, matching the ~1-2 records/s the sender demonstrably
	 * had left after the heartbeat), and the first success resets it to the
	 * full per-frame cadence. Nothing is discarded: the cursor does not
	 * advance on a refusal, so the set stays complete and in order -- a
	 * saturated sender only makes a set take longer, and stops the observer
	 * from generating drop-counter noise that hid the real signal. */
	uint32_t staged_retry_frame, staged_backoff;
	/* PER-FRAME RECONCILIATION THROTTLE (2026-09-08, the far-extract crawl
	 * fix -- see the contract on carve_observer_frame in carve_observer.h).
	 * walk_interval is the minimum number of frames between two full
	 * carve_observer_world + carve_observer_snapshot reconciliations driven
	 * from carve_observer_frame; walk_valid says one has happened at all yet
	 * (the first frame after arming always reconciles), last_walk_frame is
	 * the frame the last one ran on, and walks counts them for the cost
	 * gate. The census path (stage_set) is NOT throttled here -- it has its
	 * own once-per-second cadence and is the walk this one now matches. */
	uint32_t walk_interval, last_walk_frame;
	unsigned walks;
	bool walk_valid;
} obs;
static const char* names[CARVE_TENANTS] = {
	"other", "col", "view", "pool", "private", "vox", "far", "dl", "pop", "res",
	"tex", "slot", "mpbk"
};
/* The table is indexed by tenant, so a new enum value without a name here
 * would format as a NULL pointer. Proven at compile time rather than trusted. */
_Static_assert(sizeof(names) / sizeof(names[0]) == CARVE_TENANTS,
			   "every carve tenant needs a census name");
static size_t hash(uintptr_t p) {
	p ^= p >> 16;
	p *= (uintptr_t)UINT32_C(0x7feb352d);
	p ^= p >> 15;
	return (size_t)p;
}
/* Index of `key`, or -1. No tombstones exist (see erase), so a probe run is
 * contiguous and the first empty slot ends the search. */
static size_t find_index(uintptr_t key) {
	if(key <= 1 || !obs.capacity) return (size_t)-1;
	size_t i = hash(key) & (obs.capacity - 1);
	for(size_t n = 0; n < obs.capacity; n++, i = (i + 1) & (obs.capacity - 1)) {
		if(!obs.slots[i].key) return (size_t)-1;
		if(obs.slots[i].key == key) return i;
	}
	return (size_t)-1;
}
static struct allocation* find(uintptr_t key) {
	size_t i = find_index(key);
	return i == (size_t)-1 ? NULL : &obs.slots[i];
}
/*
	LEDGER TABLE: RESERVED ONCE AT ARM TIME (2026-09-08 round 3).

	It used to grow on demand: reserve() doubled the table whenever it filled,
	which meant a calloc of the NEXT size in the middle of a loaded run. Real
	Wii 703aef00 (split_anchor_6m 42/256 fv6 voxel) measured what that costs
	when the general heap is under pressure -- the 1.44 MB calloc for the
	32,768-record step FAILED, insert() started returning NULL, tracked blocks
	collapsed 4,888 -> 745 and errors ran 3 -> 11,468 over t104-137, recovering
	only when memory freed again. Every census number between those ticks was
	derived from a ledger that was tracking a seventh of the heap.

	So the table is now allocated ONCE, at CARVE_LEDGER_SLOTS records, in
	carve_observer_start -- at boot, before the world loads, when the general
	heap is virgin -- and NEVER reallocated. On the Wii that calloc is
	32,768 * 44 = 1,441,792 B and it comes from the GENERAL malloc heap, which
	under this file's own MALLOC_MEM2 pin is arena2 ABOVE the MEM2 carve: not
	MEM1, and not the 36 MB carve the observer is measuring. That is the same
	1.44 MB the growing table reached on hardware anyway; the change is that it
	is claimed when the claim cannot fail, instead of at the worst moment.

	If even that fails the observer does not degrade silently: the ledger is
	left DISABLED (capacity 0), every insert is a quiet no-op rather than an
	error-counting one, and the census says so in its `ledger` field -- the
	heap-side numbers (capacity/free/largest, which come from
	__lwp_heap_getinfo, not from the ledger) stay usable and the per-tenant
	numbers are visibly marked as absent instead of quietly wrong.

	CARVE_LEDGER_SLOTS is sized off that same hardware run: 4,888 live records
	measured at the point the table gave up, against a 16,384 live-record limit
	here (half the table -- linear probing wants headroom), i.e. 3.35x the
	measured peak. The cost of the fixed size is that snapshot_locked's
	O(capacity) walk runs over 32,768 records from the first second instead of
	growing into it; that walk is throttled to once per second
	(CARVE_FRAME_WALK_FRAMES) and the run reached this capacity anyway, so the
	steady-state cost is unchanged.
*/
static bool s_ledger_arm_fail; /* test hook only; outside obs, survives memset */
static bool ledger_arm(void) {
	bool forced = s_ledger_arm_fail;
	s_ledger_arm_fail = false; /* one-shot fixture: affects this arm only */
	obs.slots = NULL;
	obs.capacity = 0;
	if(forced) return false;
	obs.slots = calloc(CARVE_LEDGER_SLOTS, sizeof(*obs.slots));
	if(!obs.slots) return false;
	obs.capacity = CARVE_LEDGER_SLOTS;
	return true;
}
static void erase(uintptr_t key); /* self-healing collision path below */
static struct allocation* insert(uintptr_t key) {
	if(key <= 1) return NULL;
	/* Ledger disabled at arm time: track nothing, and do NOT count an error
	 * per allocation -- eleven thousand of them is noise, and the census
	 * already reports `ledger:0` once, which is the honest signal. */
	if(!obs.capacity) return NULL;
	/* SELF-HEALING COLLISION (2026-09-08 fix round 2). An allocation on an
	 * address the ledger still holds live means a FREE was never noted (or was
	 * noted for a different address). The old code counted the error and
	 * REFUSED the insert, which left the stale record in place: every later
	 * allocation that landed on the same address collided too, and the real
	 * block stayed untracked for the rest of the run -- one missed note could
	 * blind the ledger permanently. Drop the stale record instead, count the
	 * error so the miss stays VISIBLE in the census, and track the block that
	 * is actually live. The counter is what a fix round chases; the ledger
	 * staying honest is what every other number in the census depends on. */
	if(find(key)) { obs.totals.errors++; erase(key); }
	/* Hard ceiling, never a reallocation. Linear probing degrades badly past a
	 * half-full table and MUST never reach a full one (find/erase both rely on
	 * an empty slot existing), so the limit is half the capacity. Reaching it
	 * is a real finding -- the bound was derived from a measured peak -- so it
	 * IS counted, unlike the disabled case above. */
	if(obs.live >= obs.capacity / 2) {
		obs.totals.errors++; obs.ledger_full++; return NULL;
	}
	size_t i = hash(key) & (obs.capacity - 1);
	while(obs.slots[i].key) i = (i + 1) & (obs.capacity - 1);
	obs.live++;
	if(obs.live > obs.ledger_peak) obs.ledger_peak = obs.live;
	obs.slots[i] = (struct allocation){.key = key};
	return &obs.slots[i];
}
/*
	BACKWARD-SHIFT DELETION (Knuth 6.4 Algorithm R), replacing the key = 1
	tombstone. A tombstone keeps a probe run walkable but never frees its cell,
	so `occupied` climbed with every free and the table had to be rebuilt --
	which is what the mid-run calloc above was FOR. Closing the gap instead
	makes occupied == live for the life of the run, so a fixed table needs no
	compaction pass and therefore no second buffer.

	The cost is that erase() may MOVE records, invalidating any struct
	allocation* held across it. Every call site in this file already re-finds
	or copies first (event()'s CARVE_RESIZE copies into `saved` before erasing;
	CARVE_ALLOC dereferences its control record before insert; CARVE_DL_INIT
	and CARVE_FINAL do not touch their pointer after the erase) -- that is a
	contract on this file, and a new caller must honour it.
*/
static void erase(uintptr_t key) {
	size_t i = find_index(key);
	if(i == (size_t)-1) return;
	struct allocation* a = &obs.slots[i];
	if(a->marks & 16) {
		obs.totals.departed -= a->bytes;
		obs.totals.departed_gx -= a->gx_capacity;
	}
	obs.live--;
	const size_t mask = obs.capacity - 1;
	size_t hole = i, j = i;
	for(;;) {
		j = (j + 1) & mask;
		if(!obs.slots[j].key) break;
		size_t home = hash(obs.slots[j].key) & mask;
		/* j may fill the hole only when the hole lies inside j's probe run,
		 * i.e. j is at least as far from its home as it is from the hole. */
		if(((j - home) & mask) < ((j - hole) & mask)) continue;
		obs.slots[hole] = obs.slots[j];
		hole = j;
	}
	memset(&obs.slots[hole], 0, sizeof(obs.slots[hole]));
}
static void label(struct allocation* a, unsigned tenant) {
	if(!a || a->control || tenant >= CARVE_TENANTS || tenant == a->tenant) return;
	/* A newly allocated block starts in other; tagging only transfers this
	 * allocation's attribution. Earlier grow history stays in its real tenant. */
	obs.totals.allocs[a->tenant]--;
	obs.totals.allocs[tenant]++;
	if(a->flags & CARVE_SPILLED) {
		obs.totals.spills[a->tenant]--;
		obs.totals.spill_bytes[a->tenant] -= a->bytes;
		obs.totals.spills[tenant]++;
		obs.totals.spill_bytes[tenant] += a->bytes;
	}
	a->tenant = tenant;
}
static size_t align32(size_t n) { return (n + 31u) & ~(size_t)31u; }
static void event(enum carve_action action, uintptr_t p, uintptr_t q,
	size_t bytes, size_t aux) {
	if(action == CARVE_BEGIN) { thread_mutex_lock(&obs.lock); return; }
	if(action == CARVE_END) { thread_mutex_unlock(&obs.lock); return; }
	struct allocation* a = find(p);
	switch(action) {
	case CARVE_ALLOC: {
		obs.totals.allocs[CARVE_OTHER]++;
		if(aux & CARVE_SPILLED) {
			obs.totals.spills[CARVE_OTHER]++;
			obs.totals.spill_bytes[CARVE_OTHER] += bytes;
		}
		struct allocation* control = find(q);
		size_t initial = control && control->control ? control->gx_capacity : 0;
		a = insert(p);
		if(a) { a->bytes = bytes; a->flags = aux; a->gx_capacity = a->gx_start = initial; }
		break;
	}
	case CARVE_FREE: erase(p); if(q) erase(q); break;
	case CARVE_TAG:
		if(p) label(a, aux);
		/* A NULL allocation has no live identity; move its one failed call. */
		else if(aux < CARVE_TENANTS && obs.totals.allocs[CARVE_OTHER]) {
			obs.totals.allocs[CARVE_OTHER]--;
			obs.totals.allocs[aux]++;
			/* On Wii any post-init NULL also failed the carve. */
#ifdef PLATFORM_WII
			if(mem2_stat_carve_size()) {
				obs.totals.spills[CARVE_OTHER]--;
				obs.totals.spill_bytes[CARVE_OTHER] -= bytes;
				obs.totals.spills[aux]++;
				obs.totals.spill_bytes[aux] += bytes;
			}
#endif
		}
		break;
	case CARVE_OWNER:
		if(aux < CARVE_A_COLUMN_UNKNOWN || aux > CARVE_A_LEASED) obs.totals.errors++;
		else if(a) a->owner = aux;
		break;
	case CARVE_TRANSFER: {
		unsigned tenant = a ? a->tenant : CARVE_OTHER;
		struct allocation* next = find(q);
		if(next) label(next, tenant);
		if(!q && tenant != CARVE_OTHER) event(CARVE_TAG, 0, 0, bytes, tenant);
		if(q && tenant == CARVE_LIST) {
			if(bytes > aux) obs.totals.grows++;
			obs.totals.moved++;
			obs.totals.copy_bytes += aux < bytes ? aux : bytes;
			if(aux > obs.totals.old_peak) obs.totals.old_peak = aux;
		}
		break;
	}
	case CARVE_RESIZE: {
		unsigned tenant = a ? a->tenant : CARVE_OTHER;
		obs.totals.allocs[tenant]++;
		if(!q && bytes) break; /* standard realloc failure keeps old storage */
		struct allocation saved = a ? *a : (struct allocation){.flags = aux};
		erase(p);
		if(q) {
			a = insert(q);
			if(a) { *a = saved; a->key = q; a->bytes = bytes; }
		}
		if(p && q && tenant == CARVE_LIST) {
			if(bytes > saved.bytes) obs.totals.grows++;
			if(p != q) {
				obs.totals.moved++;
				obs.totals.copy_bytes += saved.bytes < bytes ? saved.bytes : bytes;
			}
			if(saved.bytes > obs.totals.old_peak) obs.totals.old_peak = saved.bytes;
		}
		break;
	}
	case CARVE_DL_INIT:
		if(a) erase(p); /* empty/reset control from a prior unused build */
		a = insert(p);
		if(a) { a->control = true; a->gx_capacity = align32(bytes * aux + 3); }
		break;
	case CARVE_DL_HINT:
		if(a && a->control && bytes) {
			size_t prior = align32(35 + bytes * (aux ? 12 : 9));
			if(prior > a->gx_capacity) a->gx_capacity = prior;
		}
		break;
	case CARVE_FINAL:
		if(a) {
			a->used = bytes;
			a->gx_used = align32(aux);
#ifdef PLATFORM_PC
			/* Project the successful GX in-place final shrink. Actual Wii
			 * allocation (including any retained failure slack) is below.
			 * Still byte-exact after the 2026-09-08 ladder: the ladder governs
			 * the capacity a list GROWS to, while compaction -- the only thing
			 * this projection models -- kept its byte-exact tail release (see
			 * wii/displaylist.c's displaylist_finalize for the churn-fixture
			 * measurement that settled that). */
			a->gx_capacity = a->gx_used;
#else
			a->gx_capacity = a->bytes;
#endif
		}
		if(q) erase(q);
		break;
	default: break;
	}
}

static void depart(const struct chunk* c) {
	thread_mutex_lock(&obs.lock);
	/* M2 mesh blocks: one record per generation, keyed by the block. */
	if(c->mesh_block) {
		struct allocation* a = find((uintptr_t)c->mesh_block);
		if(a && !(a->marks & 16)) {
			a->marks |= 16;
			obs.totals.departed += a->bytes;
			obs.totals.departed_gx += a->gx_capacity;
		}
	}
	/* Per-list walk unchanged (records stay byte-identical flag-off); a
	 * block-owned list resolves to its block record, already marked above. */
	for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++) if(c->has_displist[k]) {
		struct allocation* a = find((uintptr_t)c->mesh[k].data);
		if(a && !(a->marks & 16)) {
			a->marks |= 16;
			obs.totals.departed += a->bytes;
			obs.totals.departed_gx += a->gx_capacity;
		}
	}
	if(obs.totals.departed > obs.totals.departed_peak) obs.totals.departed_peak = obs.totals.departed;
	if(obs.totals.departed_gx > obs.totals.departed_gx_peak) obs.totals.departed_gx_peak = obs.totals.departed_gx;
	thread_mutex_unlock(&obs.lock);
}

bool carve_observer_active(void) { return obs.active; }
void carve_observer_ledger(struct carve_ledger_stat* out) {
	if(!out) return;
	memset(out, 0, sizeof(*out));
	if(!obs.active) return; /* the lock does not exist while disarmed */
	/* Read under the ledger lock: every field moves inside insert/erase, which
	 * run on producer threads between CARVE_BEGIN and CARVE_END. */
	thread_mutex_lock(&obs.lock);
	out->enabled = obs.capacity != 0;
	out->capacity = obs.capacity;
	out->live = obs.live;
	out->peak = obs.ledger_peak;
	out->full = obs.ledger_full;
	thread_mutex_unlock(&obs.lock);
}
void carve_observer_test_ledger_arm_fail(void) { s_ledger_arm_fail = true; }
static void server_boundary(struct server_local* s) {
	/* The observer arms at boot (main.c, before config load) and stays armed
	 * across every world load/unload, so the server tick boundary reaches this
	 * hook while the server world has NO chunk dict: from the server thread's
	 * very first tick until the first world is created, and again from each
	 * unload until the next load. m*lib's cleared dict has index == NULL, so
	 * iterating it then is a NULL dereference -- asserts on PC, and on Wii (asserts
	 * compiled out) killed the server thread on its first tick, which is why an
	 * armed --cw-carve-census run never reached main_loop_enter and sent zero
	 * heartbeats. Nothing is owed for a world that has no columns yet; the labels
	 * below are recomputed from scratch on every boundary that does have one. */
	if(!server_world_chunks_ready(&s->world)) return;
	/* Only the server traverses its dictionary. Publish all owner labels under
	 * one ledger lock. Classification takes stripes but never allocates; stripe
	 * holders never acquire the ledger or allocator (column_view's contract). */
	thread_mutex_lock(&obs.lock);
	for(size_t i = 0; i < obs.capacity; i++)
		if(obs.slots[i].key > 1 && obs.slots[i].tenant == CARVE_COLUMN)
			obs.slots[i].owner = CARVE_A_LEASED;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, s->world.chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		const struct server_chunk* c = &dict_server_chunks_ref(it)->value;
		int64_t key = dict_server_chunks_ref(it)->key;
		unsigned owner = server_local_startup_class(s, S_CHUNK_X(key), S_CHUNK_Z(key));
		uintptr_t planes[5] = {(uintptr_t)c->ids, (uintptr_t)c->metadata,
			(uintptr_t)c->lighting_sky, (uintptr_t)c->lighting_torch,
			(uintptr_t)c->heightmap};
		for(unsigned i = 0; i < 5; i++)
			event(CARVE_OWNER, planes[i], 0, 0, owner);
	}
	obs.eligible = 0;
	for(dict_server_chunks_it(it, s->world.chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		int64_t key = dict_server_chunks_ref(it)->key;
		bool nearby = clin_world_payloads_enabled()
			? residency_contains_in(s->residency_anchors, 2,
				server_local_view_radius(s) + 1, s->worlds[0].dimension,
				S_CHUNK_X(key), S_CHUNK_Z(key))
			: residency_contains(s->residency_anchors, 2,
				server_local_view_radius(s) + 1, S_CHUNK_X(key), S_CHUNK_Z(key));
		if(nearby
		   && server_local_startup_class(s, S_CHUNK_X(key), S_CHUNK_Z(key)) == CARVE_A_SUPPORT_READY)
			obs.eligible++;
	}
	obs.retired = s->startup_retired;
	obs.save_failed = s->startup_save_failed;
	obs.owner_tick = s->near_send_seq;
	thread_mutex_unlock(&obs.lock);
}
static void retirement_burst(struct server_local* s, unsigned count, uint32_t us) {
	thread_mutex_lock(&obs.lock);
	/* Loading phase: fold into the pending (unsent) burst instead of queueing
	 * a new one. Startup retirement calls this up to once per server tick
	 * (20/s); queueing them all would either flood the sender now or, if the
	 * lane is held, flood it the moment readiness opens the cadence back up.
	 * count/us are per-burst deltas (summed); retired/save_failed are the
	 * server's absolute counters and frame/tick the newest sample (kept). */
	if(obs.loading && obs.burst_sent < obs.burst_count) {
		struct retirement_burst* b = &obs.bursts[obs.burst_count - 1];
		b->frame = obs.frame;
		b->tick = s->near_send_seq;
		b->count += count;
		b->retired = s->startup_retired;
		b->failed = s->startup_save_failed;
		b->us += us;
		b->folded++;
		thread_mutex_unlock(&obs.lock);
		return;
	}
	if(obs.burst_count == obs.burst_capacity) {
		size_t cap = obs.burst_capacity ? obs.burst_capacity * 2 : 32;
		void* p = realloc(obs.bursts, cap * sizeof(*obs.bursts));
		if(!p) { obs.totals.errors++; thread_mutex_unlock(&obs.lock); return; }
		obs.bursts = p; obs.burst_capacity = cap;
	}
	obs.bursts[obs.burst_count++] = (struct retirement_burst){
		obs.frame, s->near_send_seq, count, s->startup_retired,
		s->startup_save_failed, us, 0};
	thread_mutex_unlock(&obs.lock);
}
bool carve_observer_start(void) {
	if(obs.active) return false;
	memset(&obs, 0, sizeof(obs));
	if(!thread_mutex_init(&obs.lock)) return false;
	/* The ONE ledger allocation of the run (LEDGER TABLE above). A failure
	 * here no longer refuses to arm: the observer arms with the ledger
	 * DISABLED and says so on the census line, because the heap-side numbers
	 * -- which come from __lwp_heap_getinfo, not the ledger -- are exactly
	 * what a run that is short of memory most needs to report. */
	ledger_arm();
	obs.walk_interval = CARVE_FRAME_WALK_FRAMES;
	obs.active = true;
	mem2_carve_observe = displaylist_carve_observe = event;
	/* The slot-pool reservations were carved in mem2_init, before this observer
	 * existed. Replay them now so CARVE_SLOTS accounts them; otherwise their
	 * bytes are simply absent from the per-tenant totals while being absent
	 * from the heap's free bytes too (mem2.h, FIXED-TENANT POOLS). */
	mem2_pool_note_reservations();
	world_carve_depart = depart;
	server_carve_boundary = server_boundary;
	server_carve_retirement = retirement_burst;
	return true;
}
bool carve_observer_stop(void) {
	if(!obs.active) return true;
	mem2_carve_observe = displaylist_carve_observe = NULL;
	world_carve_depart = NULL;
	server_carve_boundary = NULL;
	server_carve_retirement = NULL;
	bool ok = obs.totals.errors == 0;
	free(obs.bursts);
	free(obs.slots);
	obs.slots = NULL;
	obs.capacity = obs.live = 0;
	thread_mutex_destroy(&obs.lock);
	obs.active = false;
	return ok;
}
static struct carve_totals* snapshot_out;
static void snapshot_locked(size_t capacity, size_t available, size_t largest, size_t used_blocks) {
	thread_mutex_lock(&obs.lock);
	struct carve_totals t = obs.totals;
	t.capacity = capacity; t.free = available; t.largest = largest;
	t.used = capacity - available;
	uint64_t payload = 0, blocks = 0;
	for(size_t i = 0; i < obs.capacity; i++) {
		const struct allocation* a = &obs.slots[i];
		if(a->key <= 1 || a->control) continue;
		unsigned cls = CARVE_A_OTHER;
		if(a->tenant == CARVE_COLUMN) {
			cls = a->owner ? a->owner : CARVE_A_COLUMN_UNKNOWN;
			/* Only the ID plane has this size among the five tagged arrays.
			 * Counts unpublished/in-flight columns too, before owner labeling. */
			if(a->bytes == CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
				t.attribution.columns[cls - CARVE_A_COLUMN_UNKNOWN]++;
		}
		else if(a->tenant == CARVE_FAR) cls = CARVE_A_FAR;
		else if(a->tenant == CARVE_OVERLAY) cls = CARVE_A_OVERLAY;
		else if(a->tenant == CARVE_LIST) cls = (a->marks & 1) ? CARVE_A_LIST_LIVE
			: (a->marks & (2 | 16)) ? CARVE_A_LIST_RETAINED : CARVE_A_LIST_PENDING;
		/* A pool SLOT is not a heap block: its physical bytes were already
		 * counted once by the pool reservation (tenant CARVE_SLOTS, flagged
		 * IN_HEAP like any other block). Keep it in the per-tenant b/n
		 * columns -- that is how the census still shows WHO is using the pool
		 * -- and out of every physical total, including the block count the
		 * __lwp_heap_getinfo reconciliation below depends on. */
		bool pooled = (a->flags & CARVE_POOLED) != 0;
		unsigned placement = (a->flags & CARVE_IN_HEAP) ? 0 : 2;
#ifdef PLATFORM_WII
		if(placement && a->key >= UINT32_C(0x80000000)
		   && a->key < UINT32_C(0x81800000)) placement = 1;
		if(placement && (a->flags & CARVE_MEM2)) {
			t.attribution.bytes[CARVE_A_GUARD][placement] += 256;
			t.attribution.count[CARVE_A_GUARD][placement]++;
		}
#endif
		if(!pooled) {
			t.attribution.bytes[cls][placement] += a->bytes;
			t.attribution.count[cls][placement]++;
		}
		t.bytes[a->tenant] += a->bytes;
		t.count[a->tenant]++;
		if((a->flags & CARVE_IN_HEAP) && !pooled) {
			t.carve[a->tenant] += a->bytes;
			payload += a->bytes;
			blocks++;
		}
		if(a->tenant == CARVE_LIST) {
			if(a->marks & 1) t.dl_live += a->bytes;
			if((a->marks & 3) == 2) t.dl_retired += a->bytes;
			t.dl_used += a->used;
			t.dl_slack += a->used ? a->bytes - a->used : 0;
			t.gx_used += a->gx_used;
			t.gx_capacity += a->gx_capacity;
			t.gx_slack += a->gx_used ? a->gx_capacity - a->gx_used : 0;
		}
		/* Spatial projection: observed buffers inside Wii's radius-five
		 * column union, and GX-sized lists inside its radius-six draw union. */
		if(a->tenant == CARVE_LIST) {
			if(a->marks & 8) {
				t.scaled[CARVE_LIST] += a->gx_capacity;
				t.scaled_lists++;
				if(a->gx_used) t.scaled_slack += a->gx_capacity - a->gx_used;
			}
		} else if(a->tenant == CARVE_COLUMN || a->tenant == CARVE_VIEW) {
			if(a->marks & 4) t.scaled[a->tenant] += a->bytes;
		} else t.scaled[a->tenant] += a->bytes;
	}
	t.full5 = obs.full5;
	t.far6_slices = obs.far6_slices;
	/* libogc getinfo counts the terminal dummy as one used block. Match
	 * the PHYSICAL allocation count, independently of payload summation. */
	if(capacity && used_blocks != blocks + 1) t.errors++;
	t.guards = blocks * 256;
	if(t.used >= payload + t.guards) t.overhead = t.used - payload - t.guards;
	else t.errors++;
	t.observer_bytes = obs.capacity * sizeof(*obs.slots) + sizeof(obs);
	t.attribution.bytes[CARVE_A_GUARD][0] = t.guards;
	t.attribution.count[CARVE_A_GUARD][0] = blocks;
	t.attribution.bytes[CARVE_A_OVERHEAD][0] = t.overhead;
	t.attribution.used = t.used;
	t.attribution.free = t.free;
	t.attribution.largest = t.largest;
	t.attribution.errors = t.errors;
	t.observer_bytes += obs.burst_capacity * sizeof(*obs.bursts);
	t.attribution.owner_tick = obs.owner_tick;
	t.attribution.eligible = obs.eligible;
	t.attribution.retired = obs.retired;
	t.attribution.save_failed = obs.save_failed;
	*snapshot_out = t;
	thread_mutex_unlock(&obs.lock);
}
void carve_observer_snapshot(struct carve_totals* out) {
	memset(out, 0, sizeof(*out));
	if(!obs.active) return;
	snapshot_out = out; /* main-thread/test-owner only */
	mem2_carve_snapshot(snapshot_locked);
	snapshot_out = NULL;
}
void carve_observer_test_snapshot(struct carve_totals* out, size_t capacity,
	size_t available, size_t largest, size_t blocks) {
	memset(out, 0, sizeof(*out));
	if(!obs.active) return;
	snapshot_out = out;
	snapshot_locked(capacity, available, largest, blocks);
	snapshot_out = NULL;
}

bool carve_observer_attrib(unsigned point, struct carve_attrib* out) {
	if(!obs.active || point >= 3 || !obs.points_valid[point]) return false;
	*out = obs.points[point];
	return true;
}
void carve_observer_loading(bool loading) {
	if(!obs.active) return;
	/* Written by main, read by main (emit/stage) and by the server thread
	 * (retirement_burst) -- always under the observer lock. */
	thread_mutex_lock(&obs.lock);
	obs.loading = loading;
	thread_mutex_unlock(&obs.lock);
}

void carve_observer_walk_interval(uint32_t frames) {
	obs.walk_interval = frames ? frames : 1;
}
unsigned carve_observer_walk_count(void) { return obs.walks; }

void carve_observer_frame(struct world* w, uint32_t frame, uint32_t tick,
	uint32_t phase, bool ready) {
	if(!obs.active) return; /* no world/tenant traversal when unarmed */
	thread_mutex_lock(&obs.lock);
	obs.frame = frame;
	/* Reconcile on the census cadence, not on every frame -- the frame path
	 * is what starved the server thread's far extraction on the single-core
	 * Wii (carve_observer.h). Two things are never throttled: the very first
	 * frame after arming, and the frame readiness actually arrives on, which
	 * is point 1's one-shot transition and must be sampled where it happens.
	 * Everything below this gate is dead work on a frame that cannot produce
	 * a record: `a` is consumed only inside `if(take[k])`. */
	bool skip = obs.walk_valid && !(ready && !obs.points_valid[1])
		&& (uint32_t)(frame - obs.last_walk_frame) < obs.walk_interval;
	if(!skip) {
		obs.walk_valid = true;
		obs.last_walk_frame = frame;
		obs.walks++;
	}
	thread_mutex_unlock(&obs.lock);
	if(skip) return;
	carve_observer_world(w);
	struct carve_totals t;
	carve_observer_snapshot(&t);
	struct carve_attrib a = t.attribution;
	a.frame = frame; a.tick = tick; a.phase = phase;
	/* PC has no carve: retain the actual mem2-routed payload high-water,
	 * explicitly distinguishable by used=free=largest=0. Never fake Wii KiB. */
	uint64_t used = 0, prior[3] = {0};
	for(unsigned c = 0; c < CARVE_CLASSES; c++) {
		for(unsigned p = 0; p < 3; p++) {
			used += a.bytes[c][p];
			for(unsigned k = 0; k < 3; k++) prior[k] += obs.points[k].bytes[c][p];
		}
	}
	bool take[3] = {!ready && (!obs.points_valid[0]
		|| (t.capacity ? a.used > obs.points[0].used : used > prior[0])),
		ready && !obs.points_valid[1],
		!obs.points_valid[2] || (t.capacity ? a.free < obs.points[2].free : used > prior[2])};
	/* Track every extremum every frame -- obs.points[k] (read by
	 * carve_observer_attrib) is always exact, regardless of emission
	 * cadence. Content queued for the wire (pending_points[k]) always takes
	 * the latest observation; an observation that arrives while a record
	 * for this point is still awaiting its turn to actually go out instead
	 * folds into it (dirty, obs.coalesced[k] counts the folds) rather than
	 * minting a second one. The SEND-rate throttle itself lives in
	 * carve_observer_emit_next -- see the contract comment on
	 * carve_observer_frame in carve_observer.h. */
	for(unsigned k = 0; k < 3; k++) if(take[k]) {
		obs.points[k] = a;
		obs.points_valid[k] = true;
		if(obs.points_pending[k]) { obs.points_dirty[k] = true; obs.coalesced[k]++; }
		else { obs.pending_points[k] = a; obs.points_pending[k] = true; }
	}
}

void carve_observer_anchor(double x, double z, int separation) {
	obs.cx[0] = (int)floor(x / 16.0);
	obs.cx[1] = (int)floor((x + separation) / 16.0);
	obs.cz = (int)floor(z / 16.0);
}
static void mark(uintptr_t p, unsigned bits) {
	struct allocation* a = find(p);
	if(a) a->marks |= bits;
}
static void mark_chunk(const struct chunk* c, unsigned bits) {
	if(!c) return;
	mark((uintptr_t)c->blocks, bits);
	mark((uintptr_t)c->view, bits);
	if(c->backing) {
		mark((uintptr_t)c->backing->ids, bits);
		mark((uintptr_t)c->backing->metadata, bits);
		mark((uintptr_t)c->backing->lighting_sky, bits);
		mark((uintptr_t)c->backing->lighting_torch, bits);
		mark((uintptr_t)c->backing->heightmap, bits);
	}
	if(c->mesh_block) mark((uintptr_t)c->mesh_block, bits);
	for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++)
		if(c->has_displist[k]) mark((uintptr_t)c->mesh[k].data, bits);
}
void carve_observer_world(struct world* w) {
	if(!obs.active) return;
	thread_mutex_lock(&obs.lock);
	obs.retained = obs.retained_gx = obs.gpu = obs.gpu_gx = 0;
	obs.far_air = obs.far_count = obs.skin_bytes = obs.union_count = 0;
	obs.full5 = obs.far6_slices = 0;
	for(size_t i = 0; i < obs.capacity; i++) obs.slots[i].marks &= 16;
	dict_wsection_it_t it;
	for(dict_wsection_it(it, w->sections); !dict_wsection_end_p(it); dict_wsection_next(it)) {
		const struct world_section* s = &dict_wsection_ref(it)->value;
		const struct chunk* origin = NULL;
		for(unsigned cy = 0; cy < COLUMN_HEIGHT && !origin; cy++) origin = s->column[cy];
		unsigned spatial = 0;
		if(origin) {
			int x = WCOORD_CHUNK_OFFSET(origin->x), z = WCOORD_CHUNK_OFFSET(origin->z);
			int d0 = abs(x - obs.cx[0]), d1 = abs(x - obs.cx[1]);
			int dx = d0 < d1 ? d0 : d1, dz = abs(z - obs.cz);
			if(dx <= 5 && dz <= 5) { spatial |= 4; obs.full5++; }
			if(dx <= 6 && dz <= 6) spatial |= 8;
			/* Approximate synthetic far storage from the OBSERVED heightmap
			 * span; this is separately labelled, never called an allocation. */
			if((spatial & 8) && !(spatial & 4)) {
				unsigned lo = 128, hi = 0;
				for(unsigned j = 0; j < 256; j++) if(s->heightmap[j]) {
					unsigned y = s->heightmap[j] - 1;
					if(y < lo) lo = y;
					if(y + 2 > hi) hi = y + 2;
				}
				if(hi > 127) hi = 127;
				if(lo <= hi) obs.far6_slices += hi / 16 - lo / 16 + 1;
			}
		}
		for(unsigned cy = 0; cy < COLUMN_HEIGHT; cy++) {
			const struct chunk* c = s->column[cy];
			if(!c) continue;
			mark_chunk(c, 1 | spatial);
			if(s->is_far_lod) {
				if(c->far_skin) obs.skin_bytes += sizeof(*c->far_skin);
				if(c->blocks) {
					obs.far_count++;
					bool air = true;
					for(unsigned i = 0; i < CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE && air; i++)
						air = c->blocks[(i / 2) * 5 + i % 2] == 0;
					if(air) obs.far_air++;
				}
			}
		}
	}
	ilist_chunks2_it_t gpu;
	size_t steps = 0;
	/* Every held node was admitted under this high-water bound. Reading it
	 * must not call the public getter, which updates world diagnostics. */
	size_t cap = w->ilist_walk_cap_max;
	for(ilist_chunks2_it(gpu, w->gpu_busy_chunks); !ilist_chunks2_end_p(gpu); ilist_chunks2_next(gpu)) {
		if(++steps > cap) { obs.totals.errors++; break; }
		obs.union_count++;
		mark_chunk(ilist_chunks2_ref(gpu), 2);
	}
	for(size_t i = 0; i < obs.capacity; i++) {
		struct allocation* a = &obs.slots[i];
		if(a->key <= 1 || a->control || !(a->marks & 2)) continue;
		obs.gpu += a->bytes;
		if(a->tenant == CARVE_LIST) obs.gpu_gx += a->gx_capacity;
		if(!(a->marks & 1)) {
			obs.retained += a->bytes;
			if(a->tenant == CARVE_LIST) obs.retained_gx += a->gx_capacity;
		}
	}
	thread_mutex_unlock(&obs.lock);
}

static bool emit_attrib(unsigned k, const struct carve_attrib* a,
	bool (*emit)(const char*)) {
	char line[2304];
#ifdef PLATFORM_PC
	const unsigned pc = 1;
#else
	const unsigned pc = 0;
#endif
	int n = snprintf(line, sizeof(line),
		"{\"event\":\"carve_attrib\",\"v\":3,\"pc\":%u,\"point\":%u,\"frame\":%u,"
		"\"tick\":%u,\"phase\":%u,\"owner_tick\":%u,\"used\":%" PRIu64
		",\"free\":%" PRIu64 ",\"largest\":%" PRIu64 ",\"errors\":%" PRIu64
		",\"eligible\":%u,\"retired\":%u,\"save_failed\":%u,\"coalesced\":%u,"
		"\"columns\":[%u,%u,%u,%u,%u],\"classes\":[",
		pc, k, a->frame, a->tick, a->phase, a->owner_tick,
		a->used, a->free, a->largest, a->errors, a->eligible, a->retired, a->save_failed,
		a->coalesced,
		a->columns[0], a->columns[1], a->columns[2], a->columns[3], a->columns[4]);
	if(n < 0 || (size_t)n >= sizeof(line)) return false;
	size_t at = (size_t)n;
	for(unsigned c = 0; c < CARVE_CLASSES; c++) for(unsigned p = 0; p < 3; p++) {
		n = snprintf(line + at, sizeof(line) - at, "%s%" PRIu64 ",%" PRIu64,
		c || p ? "," : "", a->bytes[c][p], a->count[c][p]);
		if(n < 0 || (size_t)n >= sizeof(line) - at) return false;
		at += (size_t)n;
	}
	if(at + 3 > sizeof(line)) return false;
	memcpy(line + at, "]}", 3);
	return emit(line);
}

static bool staged_drain_one(bool (*emit)(const char*));

bool carve_observer_emit_next(bool (*emit)(const char*)) {
	if(!obs.active) return true;
	/* A staged per-second census set drains one record per frame ahead of
	 * everything else, so the twelve-record burst never meets the four-slot
	 * FIFO all at once. It costs at most CARVE_TENANTS+1 frames (~200 ms at
	 * 60 Hz) of the attribution round-robin, which is itself throttled to
	 * one send per point per 60 frames. */
	if(obs.staged_active) {
		/* See obs.staged_retry_frame: hold off after a refusal instead of
		 * re-offering the same record on the very next frame. */
		if((int32_t)(obs.frame - obs.staged_retry_frame) < 0)
			return true;
		if(staged_drain_one(emit)) {
			obs.staged_backoff = 0;
			return true;
		}
		obs.staged_backoff = obs.staged_backoff ? obs.staged_backoff * 2u : 1u;
		if(obs.staged_backoff > (uint32_t)CARVE_STAGED_BACKOFF_MAX_FRAMES)
			obs.staged_backoff = (uint32_t)CARVE_STAGED_BACKOFF_MAX_FRAMES;
		obs.staged_retry_frame = obs.frame + obs.staged_backoff;
		return false;
	}
	thread_mutex_lock(&obs.lock);
	const bool loading = obs.loading;
	thread_mutex_unlock(&obs.lock);
	/* Alternate extrema and burst history; never discard a rejected record.
	 * During the loading phase the retirement lane is HELD, not dropped: the
	 * bursts fold into one pending record (retirement_burst), so what is
	 * waiting here is a single summed record that goes out at readiness. */
	if(obs.emit_point % 2 && !loading) {
		thread_mutex_lock(&obs.lock);
		bool pending = obs.burst_sent < obs.burst_count;
		struct retirement_burst b = {0};
		if(pending) b = obs.bursts[obs.burst_sent];
		thread_mutex_unlock(&obs.lock);
		if(pending) {
			char line[256];
			snprintf(line, sizeof(line), "{\"event\":\"carve_retire\",\"frame\":%u,\"owner_tick\":%u,\"count\":%u,\"retired\":%u,\"save_failed\":%u,\"io_us\":%u,\"folded\":%u}", b.frame, b.tick, b.count, b.retired, b.failed, b.us, b.folded);
			if(!emit(line)) return false;
			obs.burst_sent++; obs.emit_point++; return true;
		}
	}
	unsigned k = (obs.emit_point / 2) % 3;
	if(obs.points_pending[k]) {
		/* Emission-cadence throttle (source of the sender-saturation fix,
		 * 2026-09-07): a point's very first actual send -- its true
		 * transition into loading-high-water / readiness / low-water -- is
		 * NEVER delayed. Every later send for THAT SAME point is capped to
		 * at most one per ~1 s (60 frames at the Wii's 60 Hz main loop),
		 * independently per point (not a channel-wide clock: a single
		 * shared one lets one continuously-churning point perpetually win
		 * every reopening and starve another's extremum from ever updating
		 * past its first sample). A record that isn't yet allowed to send
		 * simply waits for a later round-robin visit; meanwhile
		 * carve_observer_frame keeps folding new observations into it
		 * (points_dirty/coalesced), so what eventually goes out is never
		 * stale beyond one window. */
		enum { CARVE_ATTRIB_COALESCE_FRAMES = 60 };
		bool transition = !obs.sent_once[k];
		/* Loading phase (2026-09-08): ONLY the true transition goes out. A
		 * later send for the same point is held -- not dropped -- and keeps
		 * folding new observations in, so the record that eventually goes
		 * out at readiness carries the real extremum plus its coalesced
		 * count. Three one-shot records for the whole reload. */
		bool window_open = transition
			|| (!loading
				&& (obs.frame - obs.last_sent_frame[k])
					>= CARVE_ATTRIB_COALESCE_FRAMES);
		if(window_open) {
			struct carve_attrib record = obs.pending_points[k];
			record.coalesced = obs.coalesced[k];
			if(!emit_attrib(k, &record, emit)) return false;
			obs.sent_once[k] = true;
			obs.last_sent_frame[k] = obs.frame;
			obs.coalesced[k] = 0;
			if(obs.points_dirty[k]) {
				/* More observations arrived since this record was queued:
				 * refresh to the latest extremum for the next cycle. */
				obs.pending_points[k] = obs.points[k];
				obs.points_dirty[k] = false;
			} else obs.points_pending[k] = false;
		}
		/* else: window closed -- leave pending/dirty exactly as-is and try
		 * this point again on its next round-robin turn. */
	}
	obs.emit_point++;
	return true;
}

/* Format staged record `idx` (0..CARVE_TENANTS-1 = carve_tenant, CARVE_TENANTS
 * = the pre-formatted carve_census line). Returns false only on a formatting
 * overflow, which cannot happen for the fixed-width tenant records. */
static bool staged_format(unsigned idx, char* line, size_t cap) {
	if(idx >= CARVE_TENANTS) {
		size_t n = strlen(obs.staged_census);
		/* An empty line is rejected by every transport, so it would retry
		 * forever and wedge the set: skip the slot instead. */
		if(!n || n + 1 > cap) return false;
		memcpy(line, obs.staged_census, n + 1);
		return true;
	}
	const struct carve_totals* t = &obs.staged_totals;
	int n = snprintf(line, cap,
		"{\"event\":\"carve_tenant\",\"tick\":%u,\"m\":%u,\"t\":\"%s\","
		"\"b\":%" PRIu64 ",\"c\":%" PRIu64 ",\"n\":%" PRIu64 ","
		"\"w5\":%" PRIu64 ",\"a\":%" PRIu64 ",\"s\":%" PRIu64 ",\"sb\":%" PRIu64 "}",
		obs.staged_tick, obs.staged_measuring, names[idx], t->bytes[idx],
		t->carve[idx], t->count[idx], t->scaled[idx], t->allocs[idx],
		t->spills[idx], t->spill_bytes[idx]);
	return n > 0 && (size_t)n < cap;
}

/* Drain exactly ONE staged census record. Returns false when the transport
 * refused it -- the cursor does not advance, so the same record is retried on
 * the next frame and the set stays complete and in order. */
static bool staged_drain_one(bool (*emit)(const char*)) {
	char line[2048];
	if(!staged_format(obs.staged_next, line, sizeof(line))) {
		obs.staged_next++; /* unformattable record: skip, never wedge the set */
	} else {
		if(!emit(line)) return false;
		obs.staged_next++;
	}
	if(obs.staged_next > CARVE_TENANTS)
		obs.staged_active = false;
	return true;
}

/* Minimum spacing between two loading-phase carve_census lines, in main-loop
 * frames. The RATE is set by the caller -- main stages one set per heartbeat,
 * i.e. ~1 Hz -- and this is only the ceiling that stops a burst of stage
 * calls inside the same instant from putting several census lines on the
 * wire. It is deliberately well BELOW one second of frames: during the very
 * reload this cadence exists for, the Wii is not at 60 fps, so "60 frames"
 * can be two wall-clock seconds, and a 60-frame guard would silently halve
 * the census rate the memory gate is asking for. Frames are the only clock
 * this module has (deterministic under a test harness that drives frames
 * without sleeping); 20 of them can never suppress a heartbeat-paced set. */
enum { CARVE_LOADING_CENSUS_FRAMES = 20 };

/* `apply_loading_rules` is false only for carve_observer_emit, whose contract
 * is one COMPLETE set per synchronous call (tests + the offline gate-attrib
 * tooling); the production per-second path always passes true. */
static bool stage_set(struct world* w, uint32_t tick, bool measuring,
	bool apply_loading_rules) {
	if(!obs.active) return true;
	if(obs.staged_active) {
		/* Previous set still draining (a slow frame, or a refusing
		 * transport). Keep the older COMPLETE set rather than restarting
		 * the cursor and never finishing one. */
		obs.staged_skipped++;
		return true;
	}
	thread_mutex_lock(&obs.lock);
	const bool loading = apply_loading_rules && obs.loading;
	thread_mutex_unlock(&obs.lock);
	if(loading && obs.loading_census_valid
	   && (int32_t)(obs.frame - obs.loading_census_frame)
			  < CARVE_LOADING_CENSUS_FRAMES) {
		/* A second census inside the same loading second: the whole set is
		 * skipped and counted. Nothing here is a delta, so the next set
		 * re-derives every value. */
		obs.loading_suppressed++;
		return true;
	}
	/* Counted BEFORE the census line is formatted, so the line that goes out
	 * includes itself: a reader must never have to add one. */
	if(loading) obs.loading_suppressed++;
	carve_observer_world(w);
	carve_observer_snapshot(&obs.staged_totals);
	const struct carve_totals t = obs.staged_totals;
	struct column_view_stats v;
	column_view_get_stats(&v);
	size_t scratch, mesh_rpc;
	chunk_mesher_census_static(&scratch, &mesh_rpc);
	struct mem2_pool_stat pool_far, pool_view, pool_col;
	mem2_pool_stats(&pool_far, &pool_view);
	mem2_pool_column_stats(&pool_col);
	struct carve_ledger_stat ledger;
	carve_observer_ledger(&ledger);
	obs.staged_tick = tick;
	obs.staged_measuring = measuring;
	obs.staged_next = 0;
	/* A freshly staged set starts at the full per-frame cadence. */
	obs.staged_backoff = 0;
	obs.staged_retry_frame = obs.frame;
	char* line = obs.staged_census;
	const size_t line_cap = sizeof(obs.staged_census);
	int n = snprintf(line, line_cap,
		"{\"event\":\"carve_census\",\"tick\":%u,\"m\":%u,"
		"\"cap\":%" PRIu64 ",\"free\":%" PRIu64 ",\"used\":%" PRIu64 ","
		"\"largest\":%" PRIu64 ",\"guard\":%" PRIu64 ",\"over\":%" PRIu64 ","
		"\"err\":%" PRIu64 ",\"obs\":%" PRIu64 ","
		"\"pages\":%u,\"peak_pages\":%u,\"fallback\":%u,"
		"\"dl_used\":%" PRIu64 ",\"dl_slack\":%" PRIu64 ","
		"\"gx_used\":%" PRIu64 ",\"gx_cap\":%" PRIu64 ",\"gx_slack\":%" PRIu64 ","
		"\"grow\":%" PRIu64 ",\"move\":%" PRIu64 ",\"copy\":%" PRIu64 ",\"old_peak\":%" PRIu64 ","
		"\"ret\":%" PRIu64 ",\"ret_gx\":%" PRIu64 ",\"gpu\":%" PRIu64 ",\"gpu_gx\":%" PRIu64 ","
		"\"dl_live\":%" PRIu64 ",\"dl_ret\":%" PRIu64 ","
		"\"dep\":%" PRIu64 ",\"dep_peak\":%" PRIu64 ","
		"\"dep_gx\":%" PRIu64 ",\"dep_gx_peak\":%" PRIu64 ","
		"\"mesh_static\":%zu,\"rpc_static\":%zu,\"light_heap\":%zu,"
		"\"w5_col\":%" PRIu64 ",\"w6_far_slices_est\":%" PRIu64 ","
		"\"w6_dl_n\":%" PRIu64 ",\"w6_slack\":%" PRIu64 ","
		/* tel_drop: admission-FIFO refusals, reported HERE rather than in the
		 * heartbeat because the heartbeat's wire identity is frozen. This is
		 * the counter that used to be summed into tel_fail (with a
		 * synthesized -105) and made a benign queue refusal read as a socket
		 * storm on the 2026-09-07 hardware runs. */
		"\"tel_drop\":%u,"
		/* staged_skipped (brief finding #3): obs.staged_skipped increments
		 * every time carve_observer_stage() is called while a previous
		 * staged set is still mid-drain (a slow frame, or a refusing
		 * transport) -- the new set is skipped entirely, not queued. It was
		 * tracked but never surfaced anywhere until now, so a run that was
		 * silently losing whole census sets to a starved drain looked
		 * identical to one that wasn't. */
		"\"staged_skipped\":%u,"
		/* Loading-phase cadence (2026-09-08): `loading` says which cadence
		 * produced THIS census (1 = the reload cadence, tenant records
		 * suppressed), `loading_suppressed` is the running count of census
		 * sets whose eleven carve_tenant records did not go out (or that
		 * were skipped whole for arriving inside the same loading second).
		 * A gate analysis reading only carve_census can therefore tell a
		 * deliberately thinned reload apart from a lost stream. */
		"\"loading\":%u,\"loading_suppressed\":%u,"
		"\"union\":%" PRIu64 ",\"far_n\":%" PRIu64 ",\"far_air\":%" PRIu64 ",\"skin\":%" PRIu64 ","
		/* FIXED-TENANT POOLS (2026-09-08, mem2.h). Appended KEYS on the
		 * existing carve_census record -- the record FORMAT is unchanged
		 * (key-addressed JSON), CARVE_TENANTS and the eleven carve_tenant
		 * records are untouched, and per-tenant byte totals stay correct on
		 * their own because a pooled allocation is still tagged
		 * CARVE_VOXEL/CARVE_FAR/CARVE_VIEW by its existing mem2_tag call
		 * site. What was missing was the POOL as a tenant of the carve: how
		 * many of its slots are live, its high-water, and how often it
		 * overflowed back onto the shared heap (the number that says the
		 * bound is too small for the scene). Zero on PC, which has no carve
		 * and therefore no pools. */
		/* Round 3: pool_bytes is the CARVED total, not the cap -- the pools
		 * start empty and buy a slab only when a tagged request finds no free
		 * slot, so pool_*_slabs (of pool_*_slabs_max) is what says how much of
		 * the ceiling this scene actually took. pool_*_over keeps its round-2
		 * meaning (tagged requests the pool could not serve, served by the
		 * shared heap); pool_*_cfail is the subset of those a FAILED slab
		 * carve caused rather than the cap, which is a different finding.
		 * Every key here is additive on a key-addressed record: the collector
		 * (tools/wii_telemetry_server.py) reads carve_census by .get(), and
		 * the offline gate analyzers only read keys they name. */
		"\"pool_bytes\":%" PRIu64 ","
		"\"pool_far_slots\":%u,\"pool_far_used\":%u,\"pool_far_peak\":%u,"
		"\"pool_far_over\":%u,\"pool_far_slabs\":%u,"
		"\"pool_far_slabs_max\":%u,\"pool_far_cfail\":%u,"
		"\"pool_view_slots\":%u,\"pool_view_used\":%u,\"pool_view_peak\":%u,"
		"\"pool_view_over\":%u,\"pool_view_slabs\":%u,"
		"\"pool_view_slabs_max\":%u,\"pool_view_cfail\":%u,"
		"\"pool_col_slots\":%u,\"pool_col_used\":%u,\"pool_col_peak\":%u,"
		"\"pool_col_over\":%u,\"pool_col_slabs\":%u,\"pool_col_cfail\":%u,"
		/* LEDGER (round 3, carve_observer.c LEDGER TABLE). `ledger` is 0 only
		 * when the one arm-time table reservation failed -- every per-tenant
		 * number on this set is then absent rather than wrong, and this field
		 * is how a reader knows. ledger_full must stay 0: it counts inserts
		 * refused because the FIXED table was full, which is the one way the
		 * new sizing can still under-track. */
		"\"ledger\":%u,\"ledger_cap\":%" PRIu64 ",\"ledger_live\":%" PRIu64 ","
		"\"ledger_peak\":%" PRIu64 ",\"ledger_full\":%" PRIu64 "}",
		tick, measuring, t.capacity, t.free, t.used, t.largest, t.guards, t.overhead,
		t.errors, t.observer_bytes, v.pages, v.peak_pages, v.fallback_live,
		t.dl_used, t.dl_slack, t.gx_used, t.gx_capacity, t.gx_slack,
		t.grows, t.moved, t.copy_bytes, t.old_peak,
		obs.retained, obs.retained_gx, obs.gpu, obs.gpu_gx,
		t.dl_live, t.dl_retired, t.departed, t.departed_peak, t.departed_gx, t.departed_gx_peak,
		scratch, mesh_rpc + clin_census_static(),
		w->lighting_updates.length * w->lighting_updates.element_size
		+ w->lighting_flood_queue.length * w->lighting_flood_queue.element_size,
		t.full5, t.far6_slices, t.scaled_lists, t.scaled_slack,
		hw_telemetry_observer_drop_count(),
		obs.staged_skipped,
		loading ? 1u : 0u, obs.loading_suppressed,
		obs.union_count, obs.far_count, obs.far_air, obs.skin_bytes,
		pool_far.bytes + pool_view.bytes,
		pool_far.slots, pool_far.used, pool_far.peak, pool_far.overflow,
		pool_far.slabs, pool_far.slabs_max, pool_far.carve_fail,
		pool_view.slots, pool_view.used, pool_view.peak, pool_view.overflow,
		pool_view.slabs, pool_view.slabs_max, pool_view.carve_fail,
		pool_col.slots, pool_col.used, pool_col.peak, pool_col.overflow,
		pool_col.slabs, pool_col.carve_fail,
		ledger.enabled ? 1u : 0u, ledger.capacity, ledger.live, ledger.peak,
		ledger.full);
	if(n <= 0 || (size_t)n >= line_cap) {
		/* Census line unformattable: stage the tenant records anyway and let
		 * staged_format skip the census slot. */
		obs.staged_census[0] = '\0';
	}
	if(loading) {
		/* Census only: staged_format treats index CARVE_TENANTS as the
		 * pre-formatted census line, so starting the drain cursor there
		 * suppresses exactly the eleven carve_tenant records. */
		obs.staged_next = CARVE_TENANTS;
		obs.loading_census_frame = obs.frame;
		obs.loading_census_valid = true;
	}
	obs.staged_active = true;
	return true;
}

bool carve_observer_stage(struct world* w, uint32_t tick, bool measuring) {
	return stage_set(w, tick, measuring, true);
}

/* Synchronous whole-set emission, kept for tests and the offline gate-attrib
 * tooling that needs one complete NDJSON set per call. The production main
 * loop uses carve_observer_stage + the per-frame drain in
 * carve_observer_emit_next instead -- see the staging comment on `obs`. */
bool carve_observer_emit(struct world* w, uint32_t tick, bool measuring,
	bool (*emit)(const char*)) {
	if(!obs.active) return true;
	/* FIX (brief finding #4, 2026-09-07): this API used to force-clear
	 * staged_active unconditionally before staging its own set, tearing any
	 * still-active set left mid-drain by the normal per-frame
	 * carve_observer_stage + carve_observer_emit_next path (a slow frame, or
	 * a refusing transport) -- its undrained tail was silently discarded,
	 * never counted anywhere. Chosen fix: clear staged_active only once a
	 * drain has genuinely completed. Finish any in-flight set to completion
	 * first (emitting every record it already committed to, exactly as the
	 * production round-robin eventually would have), THEN stage and drain
	 * the fresh snapshot the caller asked for. Nothing is ever torn; the
	 * only behavior change is that this synchronous call may emit one extra
	 * old set's tail before the new one. */
	while(obs.staged_active)
		if(!staged_drain_one(emit)) return false;
	if(!stage_set(w, tick, measuring, false)) return false;
	while(obs.staged_active)
		if(!staged_drain_one(emit)) return false;
	return true;
}
