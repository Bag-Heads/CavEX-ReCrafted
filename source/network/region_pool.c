/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S shared region-handle pool -- see region_pool.h. */
#include "region_pool.h"
#ifdef PLATFORM_WII
#include "save_path.h"
#endif

#include "server_world.h"
#include "../platform/time.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef CAVEWII_REGION_ARCHIVE_TEST
static bool s_pool_fail_next_open = false;
void region_pool_test_fail_next_open(void) {
	s_pool_fail_next_open = true;
}
#endif

void region_pool_init(struct region_pool* p) {
	assert(p);
	memset(p, 0, sizeof(*p));
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++)
		p->slot[i].state = REGION_SLOT_FREE;
	p->next_incarnation = 1;
	p->ready = true;
}

void region_pool_destroy(struct region_pool* p) {
	assert(p);
	if(!p->ready)
		return;
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++) {
		struct region_pool_slot* s = &p->slot[i];
		/* A world that is torn down releases its regions first
		 * (region_pool_release_world); anything else here is a leak, and a
		 * leak is reported, never silently closed. */
		assert(s->state == REGION_SLOT_FREE || s->state == REGION_SLOT_FAULTED);
		s->state = REGION_SLOT_FREE;
		s->owner = NULL;
	}
	assert(p->open_handles == 0);
	p->ready = false;
}

static bool slot_holds_handle(const struct region_pool_slot* s) {
	return s->state == REGION_SLOT_READY || s->state == REGION_SLOT_BUSY
		|| s->state == REGION_SLOT_EVICTING;
}

static bool slot_owned_by(const struct region_pool_slot* s,
						  const struct server_world* w) {
	return s->owner == w && s->world_index == w->world_index
		&& s->world_generation == w->generation;
}

/* Remove pool index `idx` from `w`'s compact vector, shifting the tail down
 * (indices only -- never the archives). */
static void world_vector_remove(struct server_world* w, uint8_t idx) {
	size_t k = 0;
	while(k < w->region_slot_count && w->region_slots[k] != idx)
		k++;
	if(k == w->region_slot_count)
		return;
	for(; k + 1 < w->region_slot_count; k++)
		w->region_slots[k] = w->region_slots[k + 1];
	w->region_slot_count--;
}

static void world_vector_append(struct server_world* w, uint8_t idx) {
	assert(w->region_slot_count < REGION_POOL_SLOTS);
	w->region_slots[w->region_slot_count++] = idx;
}

static void slot_free(struct region_pool* p, size_t i) {
	struct region_pool_slot* s = &p->slot[i];
	s->state = REGION_SLOT_FREE;
	s->owner = NULL;
	s->world_index = 0;
	s->world_generation = 0;
	s->pins = 0;
	s->last_use = 0;
}

/* Settle and close the donor in slot `i`. Returns STORAGE_OK when the slot
 * is FREE again, STORAGE_IO_ERROR otherwise (the donor stays READY with its
 * dirty state on a sync failure; a close failure leaves a FAULTED tombstone
 * -- the handle is consumed, so the only honest answer is the flag). */
static enum storage_rc pool_evict_slot(struct region_pool* p, size_t i) {
	struct region_pool_slot* s = &p->slot[i];
	struct server_world* owner = s->owner;
	assert(s->state == REGION_SLOT_READY && s->pins == 0 && owner);
	s->state = REGION_SLOT_EVICTING;
	/* Peek-and-sync first: a failed publication must leave the exact handle
	 * resident so the owner's next barrier retries the very same evidence
	 * (server_world_sync_regions) instead of a sticky bit. */
	if(!region_archive_sync(&s->archive)) {
		s->state = REGION_SLOT_READY;
		owner->region_evict_failed = true;
		p->evict_sync_failures++;
		return STORAGE_IO_ERROR;
	}
	bool closed = region_archive_destroy(&s->archive);
	assert(p->open_handles > 0);
	p->open_handles--;
	p->evictions++;
	world_vector_remove(owner, (uint8_t)i);
	if(!closed) {
		/* A2S-P1 review L1: the sync above SUCCEEDED, so the donor's data is
		 * durable; fclose then consumed the stream and reported failure, which
		 * no later walk can retry -- that is the owner's terminal
		 * region_close_failed latch (server_world_sync_regions stays false for
		 * this world instance). The slot holds no handle any more, so it is
		 * FREE and reusable: the candidate opens into it. (REGION_SLOT_FAULTED
		 * is kept for a failure that leaves a handle in an unknown state.) */
		owner->region_close_failed = true;
		p->close_failures++;
	}
	slot_free(p, i);
	return STORAGE_OK;
}

/* A2S-P1 review M1: does the region FILE exist? A `!create` request for an
 * absent region must answer STORAGE_ABSENT WITHOUT choosing a donor -- a load
 * probe (server_world_load_chunk -> chunk_region) into unexplored terrain
 * must never evict a live handle for a file that is not there. Same path
 * rule as region_archive_create / server_world_region_file_status. */
static bool region_pool_file_exists(const struct server_world* w, w_coord_t rx,
									w_coord_t rz) {
	string_t path;
	if(w->dimension == WORLD_DIM_OVERWORLD)
		string_init_printf(path, "%s/region/r.%i.%i.mcr",
						   string_get_cstr(w->level_name), rx, rz);
	else
		string_init_printf(path, "%s/DIM-1/region/r.%i.%i.mcr",
						   string_get_cstr(w->level_name), rx, rz);
	struct stat info;
#ifdef PLATFORM_WII
	bool profile = region_archive_work_profile_enabled();
	ptime_t start = profile ? time_get() : 0;
#endif
	bool exists = stat(string_get_cstr(path), &info) == 0;
#ifdef PLATFORM_WII
	int saved_errno = errno;
	if(profile) region_archive_path_profile_note(
		(uint32_t)(time_diff_s(start, time_get()) * 1e6f));
	errno = saved_errno;
#endif
	string_clear(path);
	return exists;
}

static int pool_choose_donor(const struct region_pool* p) {
	int best = -1;
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++) {
		const struct region_pool_slot* s = &p->slot[i];
		if(s->state != REGION_SLOT_READY || s->pins != 0)
			continue;
		if(best < 0 || s->last_use < p->slot[best].last_use)
			best = (int)i; /* strict <: ties keep the lower index */
	}
	return best;
}

struct region_archive* region_pool_oldest_donor(struct region_pool* p) {
	assert(p);
	int d = pool_choose_donor(p);
	return d < 0 ? NULL : &p->slot[d].archive;
}

static enum storage_rc region_pool_acquire_impl(struct region_pool* p,
									struct server_world* w, w_coord_t rx,
									w_coord_t rz, bool create, const bool* probed,
									struct region_archive** out) {
	assert(p && p->ready && w && out);
#ifdef PLATFORM_WII
	/* Resident hits serve SAVE too, after another world may have selected cwd. */
	save_path_set_world(string_get_cstr(w->level_name));
#endif
	*out = NULL;

	/* Hit: this world's handle for (rx, rz). */
	for(size_t k = 0; k < w->region_slot_count; k++) {
		struct region_pool_slot* s = &p->slot[w->region_slots[k]];
		assert(slot_holds_handle(s) && slot_owned_by(s, w));
		if(s->archive.x == rx && s->archive.z == rz) {
			s->last_use = ++p->clock;
			*out = &s->archive;
			return STORAGE_OK;
		}
	}

	/* M1: an absent file needs no handle -- report it before any admission. */
	if(!create && !(probed ? *probed : region_pool_file_exists(w, rx, rz))) {
		p->absent_probes++;
		return STORAGE_ABSENT;
	}

	/* Admission: a FREE slot, else the oldest unpinned donor across the
	 * session. The donor is closed BEFORE the candidate opens. */
	int target = -1;
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++)
		if(p->slot[i].state == REGION_SLOT_FREE) {
			target = (int)i;
			break;
		}
	if(target < 0) {
		int donor = pool_choose_donor(p);
		if(donor < 0) {
			p->busy_refusals++;
			return STORAGE_BUSY; /* every handle pinned/busy or tombstoned */
		}
		enum storage_rc rc = pool_evict_slot(p, (size_t)donor);
		if(rc != STORAGE_OK)
			return rc;
		target = donor;
	}
	assert(p->open_handles < REGION_POOL_SLOTS);

	struct region_pool_slot* s = &p->slot[target];
	s->state = REGION_SLOT_OPENING;
	bool opened;
#ifdef CAVEWII_REGION_ARCHIVE_TEST
	if(s_pool_fail_next_open) {
		/* the candidate's own allocation/open failed (MEM1 pressure, fd
		 * exhaustion): the donor is already settled and closed -- durable */
		s_pool_fail_next_open = false;
		opened = false;
	} else
#endif
	opened = region_archive_create(&s->archive, w->level_name, rx, rz,
								   w->dimension);
	if(!opened && create)
		opened = region_archive_create_new(&s->archive, w->level_name, rx, rz,
										   w->dimension);
	if(!opened) {
		slot_free(p, target);
		/* region_archive_create cannot tell an absent file from an
		 * unreadable one; server_world_chunk_region_checked stats first. */
		return create ? STORAGE_IO_ERROR : STORAGE_ABSENT;
	}
	p->open_handles++;
	if(p->open_handles > p->open_handles_peak)
		p->open_handles_peak = p->open_handles;
	assert(p->open_handles <= REGION_POOL_SLOTS);
	s->state = REGION_SLOT_READY;
	s->owner = w;
	s->world_index = w->world_index;
	s->world_generation = w->generation;
	s->incarnation = p->next_incarnation++;
	s->pins = 0;
	s->last_use = ++p->clock;
	world_vector_append(w, (uint8_t)target);
	/* EVICTION-PATH RULE (round 2, 2026-09-07): while the owner's save batch
	 * is open EVERY resident handle is batched, armed at installation. */
	if(w->save_batch_open)
		region_archive_batch_begin(&s->archive);
	*out = &s->archive;
	return STORAGE_OK;
}

static enum storage_rc region_pool_acquire_timed(struct region_pool* p,
	struct server_world* w, w_coord_t rx, w_coord_t rz, bool create,
	const bool* probed, struct region_archive** out) {
#ifdef PLATFORM_WII
	bool profile = region_archive_work_profile_enabled();
	ptime_t start = 0;
	uint32_t before[3];
	if(profile) {
		start = time_get();
		region_archive_path_profile_stats(before);
	}
#endif
	enum storage_rc rc = region_pool_acquire_impl(p, w, rx, rz, create, probed, out);
#ifdef PLATFORM_WII
	int saved_errno = errno;
	if(profile) {
		uint32_t after[3];
		region_archive_path_profile_stats(after);
		if(after[0] != before[0])
			region_archive_path_profile_emit(create || w->save_batch_open
				? "save_region" : "load_region",
				(uint32_t)(time_diff_s(start, time_get()) * 1e6f));
	}
	errno = saved_errno;
#endif
	return rc;
}

enum storage_rc region_pool_acquire(struct region_pool* p,
	struct server_world* w, w_coord_t rx, w_coord_t rz, bool create,
	struct region_archive** out) {
	return region_pool_acquire_timed(p, w, rx, rz, create, NULL, out);
}

enum storage_rc region_pool_acquire_probed(struct region_pool* p,
	struct server_world* w, w_coord_t rx, w_coord_t rz, bool exists,
	struct region_archive** out) {
	return region_pool_acquire_timed(p, w, rx, rz, false, &exists, out);
}

static struct region_pool_slot* slot_for(struct region_pool* p,
										 const struct region_archive* ra) {
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++)
		if(&p->slot[i].archive == ra && slot_holds_handle(&p->slot[i]))
			return &p->slot[i];
	return NULL;
}

const struct region_pool_slot* region_pool_slot_of(
	const struct region_pool* p, const struct region_archive* ra) {
	assert(p);
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++)
		if(&p->slot[i].archive == ra && slot_holds_handle(&p->slot[i]))
			return &p->slot[i];
	return NULL;
}

void region_pool_pin(struct region_pool* p, struct region_archive* ra) {
	struct region_pool_slot* s = slot_for(p, ra);
	assert(s && s->state != REGION_SLOT_EVICTING);
	s->pins++;
	s->state = REGION_SLOT_BUSY;
}

void region_pool_unpin(struct region_pool* p, struct region_archive* ra) {
	struct region_pool_slot* s = slot_for(p, ra);
	assert(s && s->state == REGION_SLOT_BUSY && s->pins > 0);
	if(--s->pins == 0)
		s->state = REGION_SLOT_READY;
}

size_t region_pool_open_count(const struct region_pool* p) {
	assert(p);
	return p->open_handles;
}

bool region_pool_release_world(struct region_pool* p, struct server_world* w) {
	assert(p && w);
	bool ok = true;
	/* Walk this world's vector oldest-first (index order is insertion order
	 * after compaction), popping the front each time. */
	while(w->region_slot_count > 0) {
		uint8_t idx = w->region_slots[0];
		struct region_pool_slot* s = &p->slot[idx];
		assert(slot_holds_handle(s) && slot_owned_by(s, w));
		s->state = REGION_SLOT_EVICTING;
		string_t failed_name;
		string_init_set(failed_name, s->archive.file_name);
		/* Both evaluated, never short-circuited: a `||` would skip the close
		 * on a failed sync and leak the fd and the index arrays with it. */
		bool synced = region_archive_sync(&s->archive);
		bool closed = region_archive_destroy(&s->archive);
		assert(p->open_handles > 0);
		p->open_handles--;
		world_vector_remove(w, idx);
		if(!synced || !closed) {
			ok = false;
			if(!synced)
				w->region_evict_failed = true;
			if(!closed) {
				w->region_close_failed = true;
				p->close_failures++;
			}
			fprintf(stderr,
					"[region %s] UNLOAD sync/close FAILED -- buffered chunk "
					"writes for this region may not have reached the card\n",
					string_get_cstr(failed_name));
		}
		string_clear(failed_name);
		slot_free(p, idx);
	}
	/* Tombstones this world left behind are reusable once it is gone. */
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++)
		if(p->slot[i].state == REGION_SLOT_FAULTED && p->slot[i].owner == w)
			slot_free(p, i);
	return ok;
}
