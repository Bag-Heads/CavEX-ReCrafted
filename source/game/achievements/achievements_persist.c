/* Copyright (c) 2026 CaveWii_v2 contributors */
/* Versioned per-profile `.cwstats` file [docs/achievements_design.md 4].
 * stdio + zlib's crc32 only: no NBT, no parson, no JSON. */

/* Strict -std=c99 hides fileno unless POSIX declarations are requested before
 * any system header; the same preamble source/network/complex_block_archive.c
 * uses. Wii libfat implements fsync through FatFs f_sync. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "achievements_persist.h"

#define CW_STATS_LINE_MAX 256

/* ---- Injected clock [design 4 "Clock"] ---------------------------------- */

static uint32_t (*s_clock)(void) = NULL;

void stats_set_clock(uint32_t (*now)(void)) {
	s_clock = now;
}

static uint32_t stats_now(void) {
	if(s_clock)
		return s_clock();
	time_t t = time(NULL);
	/* "unix seconds if the clock is valid, else 0" -- a console with no RTC
	 * set writes 0 rather than a misleading 1970 stamp. */
	return (t == (time_t)-1) ? 0u : (uint32_t)t;
}

/* ---- Fault injection (runtime-armed, one-shot) --------------------------
 * A function that is always compiled in and inert until a test arms it, never
 * a new #ifdef compile profile (docs/build_conventions.md, the
 * runtime-armed-seam rule). */

static int s_fail_stage = STATS_IO_STAGE_NONE;

void stats_io_fail_at(int stage) {
	s_fail_stage = stage;
}

int stats_io_fail_armed(void) {
	return s_fail_stage;
}

static bool fail_fires(int stage) {
	if(s_fail_stage != stage)
		return false;
	s_fail_stage = STATS_IO_STAGE_NONE;
	return true;
}

/* ---- Paths -------------------------------------------------------------- */

bool stats_profile_path(char* out, size_t out_size, const char* dir,
						const char* profile_id, const char* suffix) {
	if(!out || out_size == 0)
		return false;
	out[0] = '\0';
	if(!dir || !profile_id || !suffix || profile_id[0] == '\0')
		return false;

	/* ".bak"/".tmp" REPLACE the extension (they are rotations of the whole
	 * file); ".bad" is appended to it, so a quarantined file still reads as
	 * "this was a .cwstats". */
	const char* ext = ".cwstats";
	const char* tail = suffix;
	if(strcmp(suffix, ".bak") == 0 || strcmp(suffix, ".tmp") == 0) {
		ext = suffix;
		tail = "";
	}

	int n = snprintf(out, out_size, "%s/%s%s%s", dir, profile_id, ext, tail);
	if(n < 0 || (size_t)n >= out_size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

static bool file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

static bool file_sync(FILE* f) {
	if(!f || fflush(f) != 0)
		return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

/* The durability barrier of design 4 "Write", run AFTER both renames have
 * succeeded (R1 MED 6). The file's DATA is already committed -- the rename
 * published a fully written, already-fsynced body -- so this exists only to
 * push the directory entries the two renames touched out of a cache. On Wii
 * libfat that is FatFs f_sync on the partition; on Windows/POSIX it is a
 * no-op or close to one. It therefore CANNOT fail the save, and its result is
 * deliberately discarded.
 *
 * Opened "r+b" (not the writer's stream, which is long closed, and not "rb":
 * fflush on an input stream is undefined, and a read-only handle is not a
 * sync target on every platform). A medium that refuses the write handle
 * falls back to a read handle; a failure to open at all is simply skipped. */
static void sync_committed(const char* path) {
	FILE* f = fopen(path, "r+b");
	if(!f)
		f = fopen(path, "rb");
	if(!f)
		return;
#ifdef _WIN32
	(void)_commit(_fileno(f));
#else
	(void)fsync(fileno(f));
#endif
	fclose(f);
}

/* ---- Writer ------------------------------------------------------------- */

struct cw_writer {
	FILE* f;
	uLong crc;	/* over every byte written so far (the `end` line excluded) */
	bool ok;
	uint32_t records; /* `s` + `a` lines: what `end <count>` declares */
};

static void wr_emit(struct cw_writer* w, const char* buf, int len) {
	if(!w->ok)
		return;
	if(len < 0 || (size_t)len >= CW_STATS_LINE_MAX) {
		w->ok = false; /* a line that did not fit was never written */
		return;
	}
	if(fwrite(buf, 1, (size_t)len, w->f) != (size_t)len) {
		w->ok = false;
		return;
	}
	w->crc = crc32(w->crc, (const Bytef*)buf, (uInt)len);
}

static void wr_counter(struct cw_writer* w, uint32_t id,
					   const struct stats_pair* c) {
	/* Only non-zero counters are written [design 4]; Beta writes only
	 * touched stats too. */
	if(c->local == 0 && c->remote == 0)
		return;
	if(!w->ok)
		return;
	char buf[CW_STATS_LINE_MAX];
	int n = snprintf(buf, sizeof(buf), "s %lu %lu %lu\n", (unsigned long)id,
					 (unsigned long)c->local, (unsigned long)c->remote);
	wr_emit(w, buf, n);
	w->records++;
}

static void wr_achievement(struct cw_writer* w, const struct stats_profile* p,
						   int i) {
	const struct stats_pair* c = &p->ach[i];
	if(c->local == 0 && c->remote == 0)
		return;
	if(!w->ok)
		return;
	char letter = (char)p->ach_first[i];
	if(letter != (char)STATS_FIRST_LOCAL && letter != (char)STATS_FIRST_REMOTE)
		/* Never recorded (a profile filled before B2's rules layer exists):
		 * derive it from the columns rather than writing a letter the format
		 * does not define. */
		letter = (c->local > 0) ? (char)STATS_FIRST_LOCAL
								: (char)STATS_FIRST_REMOTE;
	char buf[CW_STATS_LINE_MAX];
	int n = snprintf(buf, sizeof(buf), "a %lu %lu %lu %c\n",
					 (unsigned long)(CW_ACH_BASE + (uint32_t)i),
					 (unsigned long)c->local, (unsigned long)c->remote, letter);
	wr_emit(w, buf, n);
	w->records++;
}

static enum stats_save_status publish_save(struct stats_save_job* j) {
	const struct stats_profile* p = j->profile;
	/* Rotate the committed file to `.bak` (Beta's `.old`) so the previous
	 * good file always survives. A missing committed file is the first-ever
	 * save, not a failure. */
	if(fail_fires(STATS_IO_STAGE_RENAME_BAK)) {
		remove(j->tmp_path);
		return STATS_SAVE_ERR_RENAME_BAK;
	}
	bool had_main = file_exists(j->main_path);
	if(had_main && p->main_was_corrupt && !j->rescue) {
		/* THE R1 HIGH 1 RULE. This main file is the one our own parser
		 * rejected at load (the quarantine rename could not move it aside).
		 * Rotating it into `.bak` would install a known-corrupt file as the
		 * only backup and destroy the good copy this profile was recovered
		 * from -- so it is REMOVED instead, and `.bak` keeps holding a file
		 * this writer produced. */
		remove(j->main_path);
		had_main = false;
	}
	if(had_main) {
		/* Windows rename() (unlike POSIX) fails when the destination exists;
		 * source/network/map_data_archive.c documents the same shape. */
		remove(j->bak_path);
		if(rename(j->main_path, j->bak_path) != 0) {
			remove(j->tmp_path);
			return STATS_SAVE_ERR_RENAME_BAK;
		}
	}

	bool promoted;
	if(fail_fires(STATS_IO_STAGE_RENAME_MAIN)) {
		promoted = false;
	} else {
		remove(j->main_path); /* rotated away already; defensive on Windows */
		promoted = rename(j->tmp_path, j->main_path) == 0;
	}
	if(!promoted) {
		/* Put the previous good file back where a load will find it and drop
		 * the tmp. Unlike map_data_archive.c we do NOT keep the tmp as a
		 * survivor: the caller's in-memory profile still holds everything
		 * that was in it and stays dirty, so the retry rewrites it -- an
		 * orphan file no loader reads would only be another way to get
		 * confused later. */
		if(had_main)
			rename(j->bak_path, j->main_path);
		remove(j->tmp_path);
		return STATS_SAVE_ERR_RENAME_MAIN;
	}

	return STATS_SAVE_OK;
}

void stats_save_begin(struct stats_save_job* j, const char* dir,
	const struct stats_profile* p) {
	*j = (struct stats_save_job){.profile = p, .ok = true};
	if(!dir || !p || !stats_profile_id_valid(p->profile_id)
				|| !stats_profile_path(j->main_path, sizeof(j->main_path), dir, p->profile_id, "")
				|| !stats_profile_path(j->tmp_path, sizeof(j->tmp_path), dir, p->profile_id, ".tmp")
				|| !stats_profile_path(j->bak_path, sizeof(j->bak_path), dir, p->profile_id, ".bak")) {
		j->result = STATS_SAVE_ERR_BAD_ARG; j->done = true;
	} else if(p->write_refused) {
		j->result = p->write_refused == STATS_WRITE_REFUSED_FOREIGN
			? STATS_SAVE_REFUSED_FOREIGN : STATS_SAVE_REFUSED_NEWER;
		j->done = true;
	}
	j->saved = stats_now();
}

static void save_records(struct stats_save_job* j, struct cw_writer* w) {
	const struct stats_profile* p = j->profile;
	const unsigned total = CW_STAT_COUNT + CW_ACH_COUNT + CW_STAT_BLOCK_COUNT
		+ 3 * ITEMS_MAX + p->foreign_count;
	while(j->cursor < total && j->scanned < CW_STATS_SAVE_STEP_RECORDS) {
		unsigned at = j->cursor++; j->scanned++;
		if(at < CW_STAT_COUNT) { wr_counter(w, stat_general_id(at), &p->general[at]); continue; }
		at -= CW_STAT_COUNT;
		if(at < CW_ACH_COUNT) { wr_achievement(w, p, at); continue; }
		at -= CW_ACH_COUNT;
		if(at < CW_STAT_BLOCK_COUNT) { wr_counter(w, CW_STAT_MINE_BASE + at, &p->mine[at]); continue; }
		at -= CW_STAT_BLOCK_COUNT;
		if(at < ITEMS_MAX) { wr_counter(w, CW_STAT_CRAFT_BASE + at, &p->craft[at]); continue; }
		at -= ITEMS_MAX;
		if(at < ITEMS_MAX) { wr_counter(w, CW_STAT_USE_BASE + at, &p->use[at]); continue; }
		at -= ITEMS_MAX;
		if(at < ITEMS_MAX) { wr_counter(w, CW_STAT_BREAK_BASE + at, &p->brk[at]); continue; }
		at -= ITEMS_MAX;
		char buf[CW_STATS_LINE_MAX];
		const char* line = p->foreign[at];
		wr_emit(w, buf, snprintf(buf, sizeof(buf), "%s\n", line));
		if((line[0] == 's' || line[0] == 'a') && line[1] == ' ') w->records++;
	}
	if(j->cursor == total) j->phase++;
}

bool stats_save_step(struct stats_save_job* j) {
	j->scanned = 0;
	if(j->done) return true;
	const struct stats_profile* p = j->profile;
	struct cw_writer w = {.f = j->file, .crc = j->crc, .ok = j->ok, .records = j->records};
	char buf[CW_STATS_LINE_MAX];
	switch(j->phase) {
	case 0:
		remove(j->tmp_path);
		if(fail_fires(STATS_IO_STAGE_TMP_OPEN) || !(j->file = fopen(j->tmp_path, "wb"))) {
			j->result = STATS_SAVE_ERR_TMP_OPEN; j->done = true; break;
		}
		j->crc = crc32(0, NULL, 0); j->phase++; return false;
	case 1:
		wr_emit(&w, buf, snprintf(buf, sizeof(buf), "cwstats %d\n", STATS_FILE_VERSION));
		wr_emit(&w, buf, snprintf(buf, sizeof(buf), "profile %s\n", p->profile_id));
		wr_emit(&w, buf, snprintf(buf, sizeof(buf), "saved %lu\n", (unsigned long)j->saved));
		if(p->progress_incomplete) wr_emit(&w, "incomplete 1\n", 13);
		j->phase++; break;
	case 2: save_records(j, &w); break;
	case 3: {
		int n = snprintf(buf, sizeof(buf), "end %lu %08lx\n", (unsigned long)w.records,
			(unsigned long)(w.crc & 0xfffffffful));
		if(n < 0 || (size_t)n >= sizeof(buf) || fwrite(buf, 1, n, j->file) != (size_t)n
					|| fail_fires(STATS_IO_STAGE_WRITE)) w.ok = false;
		j->phase++; break;
	}
	case 4: {
		bool synced = w.ok && !fail_fires(STATS_IO_STAGE_FSYNC) && file_sync(j->file);
		if(fclose(j->file) != 0) w.ok = false;
		j->file = NULL;
		if(!w.ok || !synced) {
			j->result = w.ok ? STATS_SAVE_ERR_FSYNC : STATS_SAVE_ERR_WRITE;
			remove(j->tmp_path); j->done = true;
		} else j->phase++;
		break;
	}
	case 5:
		/* Rotation and promotion are one step: cancellation never strands main. */
		j->result = publish_save(j);
		if(j->result != STATS_SAVE_OK) j->done = true;
		else j->phase++;
		break;
	case 6:
		if(!fail_fires(STATS_IO_STAGE_RENAME_SYNC)) sync_committed(j->main_path);
		j->done = true; break;
	}
	j->crc = w.crc; j->records = w.records; j->ok = w.ok;
	return j->done;
}

void stats_save_cancel(struct stats_save_job* j) {
	if(j->file) { fclose(j->file); j->file = NULL; }
	if(!j->done && j->phase < 6) remove(j->tmp_path);
	j->done = true;
}

enum stats_save_status stats_profile_save(const char* dir, const struct stats_profile* p) {
	struct stats_save_job job;
	stats_save_begin(&job, dir, p);
	while(!stats_save_step(&job)) {}
	return job.result;
}

void stats_rescue_begin(struct stats_save_job* job, const char* dir, const struct stats_profile* p) {
	stats_save_begin(job, dir, p);
	if(!dir || !p || !stats_profile_id_valid(p->profile_id)
				|| !stats_profile_path(job->main_path, sizeof(job->main_path), dir, p->profile_id, ".unsaved")
				|| !stats_profile_path(job->tmp_path, sizeof(job->tmp_path), dir, p->profile_id, ".unsaved.tmp")
				|| !stats_profile_path(job->bak_path, sizeof(job->bak_path), dir, p->profile_id, ".unsaved.bak")) {
		job->done = true; job->result = STATS_SAVE_ERR_BAD_ARG; return;
	}
	/* Refusal protects the canonical file. A separate supported-format survivor
	 * may still preserve all counters this build knows, without touching it. */
	job->done = false; job->result = STATS_SAVE_OK; job->rescue = true;
}

enum stats_save_status stats_profile_rescue(const char* dir, const struct stats_profile* p) {
	struct stats_save_job job;
	stats_rescue_begin(&job, dir, p);
	while(!stats_save_step(&job)) {}
	return job.result;
}

/* ---- Reader ------------------------------------------------------------- */

enum parse_result {
	PARSE_OK = 0,
	PARSE_MISSING,
	PARSE_CORRUPT,
	PARSE_UNSUPPORTED,
};

/* Hand-rolled field scanners instead of sscanf: they reject a field that
 * overflows uint32 (sscanf silently saturates), reject trailing junk, and do
 * not depend on the C library's locale. */
static bool scan_u32(const char** cursor, uint32_t* out) {
	const char* s = *cursor;
	while(*s == ' ')
		s++;
	if(*s < '0' || *s > '9')
		return false;
	uint64_t v = 0;
	while(*s >= '0' && *s <= '9') {
		v = v * 10u + (uint64_t)(*s - '0');
		if(v > (uint64_t)UINT32_MAX)
			return false;
		s++;
	}
	*cursor = s;
	*out = (uint32_t)v;
	return true;
}

static bool scan_hex32(const char** cursor, uint32_t* out) {
	const char* s = *cursor;
	while(*s == ' ')
		s++;
	uint64_t v = 0;
	int digits = 0;
	for(;; s++) {
		int d;
		if(*s >= '0' && *s <= '9')
			d = *s - '0';
		else if(*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if(*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;
		v = v * 16u + (uint64_t)d;
		if(v > (uint64_t)UINT32_MAX)
			return false;
		digits++;
	}
	if(digits == 0)
		return false;
	*cursor = s;
	*out = (uint32_t)v;
	return true;
}

static bool scan_end(const char* cursor) {
	while(*cursor == ' ')
		cursor++;
	return *cursor == '\0';
}

/* ---- Representable-slot map --------------------------------------------
 * ONE flat index per counter this build can hold, in the writer's own order.
 * It is what the duplicate-id detector marks (a duplicate `s`/`a` id is
 * corrupt: last-wins silently changes which value survives, and our writer
 * never produces one -- design 4, R1 LOW 12), and what tells the two apply
 * functions whether the id has a slot at all. */
enum {
	CW_STATS_SLOT_ACH = CW_STAT_COUNT,
	CW_STATS_SLOT_MINE = CW_STATS_SLOT_ACH + CW_ACH_COUNT,
	CW_STATS_SLOT_CRAFT = CW_STATS_SLOT_MINE + CW_STAT_BLOCK_COUNT,
	CW_STATS_SLOT_USE = CW_STATS_SLOT_CRAFT + ITEMS_MAX,
	CW_STATS_SLOT_BREAK = CW_STATS_SLOT_USE + ITEMS_MAX,
	CW_STATS_SLOT_COUNT = CW_STATS_SLOT_BREAK + ITEMS_MAX,
};

/* Flat index of a counter id, or -1 when this build has no slot for it. */
static int counter_flat(uint32_t id) {
	int gi = stat_general_index(id);
	if(gi >= 0)
		return gi;
	if(id >= CW_STAT_MINE_BASE
	   && id < CW_STAT_MINE_BASE + (uint32_t)CW_STAT_BLOCK_COUNT)
		return CW_STATS_SLOT_MINE + (int)(id - CW_STAT_MINE_BASE);
	if(id >= CW_STAT_CRAFT_BASE && id < CW_STAT_CRAFT_BASE + (uint32_t)ITEMS_MAX)
		return CW_STATS_SLOT_CRAFT + (int)(id - CW_STAT_CRAFT_BASE);
	if(id >= CW_STAT_USE_BASE && id < CW_STAT_USE_BASE + (uint32_t)ITEMS_MAX)
		return CW_STATS_SLOT_USE + (int)(id - CW_STAT_USE_BASE);
	if(id >= CW_STAT_BREAK_BASE && id < CW_STAT_BREAK_BASE + (uint32_t)ITEMS_MAX)
		return CW_STATS_SLOT_BREAK + (int)(id - CW_STAT_BREAK_BASE);
	return -1;
}

/* Flat index of an achievement id, or -1. */
static int ach_flat(uint32_t id) {
	if(id < CW_ACH_BASE || id >= CW_ACH_BASE + (uint32_t)CW_ACH_COUNT)
		return -1;
	return CW_STATS_SLOT_ACH + (int)(id - CW_ACH_BASE);
}

/* Apply one counter record. Returns false when this build has no slot for the
 * id -- a valid-crc file may legitimately carry ids a bigger registry wrote.
 * The caller then KEEPS THE LINE VERBATIM (see keep_foreign) rather than
 * discarding the whole file or, worse, dropping the record on the next save:
 * the crc is the corruption detector, an unrepresentable id is a different
 * problem, and a save that silently rewrites the file without those counters
 * is permanent invisible loss [R1 HIGH 2]. */
static bool apply_counter(struct stats_profile* p, uint32_t id, uint32_t local,
						  uint32_t remote) {
	int flat = counter_flat(id);
	if(flat < 0)
		return false;
	int gi = stat_general_index(id);
	if(gi >= 0) {
		p->general[gi].local = local;
		p->general[gi].remote = remote;
		return true;
	}
	struct stats_pair* slot;
	if(flat >= CW_STATS_SLOT_BREAK)
		slot = &p->brk[flat - CW_STATS_SLOT_BREAK];
	else if(flat >= CW_STATS_SLOT_USE)
		slot = &p->use[flat - CW_STATS_SLOT_USE];
	else if(flat >= CW_STATS_SLOT_CRAFT)
		slot = &p->craft[flat - CW_STATS_SLOT_CRAFT];
	else
		slot = &p->mine[flat - CW_STATS_SLOT_MINE];
	slot->local = local;
	slot->remote = remote;
	return true;
}

static bool apply_achievement(struct stats_profile* p, uint32_t id,
							  uint32_t local, uint32_t remote, char letter) {
	int flat = ach_flat(id);
	if(flat < 0)
		return false;
	if(letter != (char)STATS_FIRST_LOCAL && letter != (char)STATS_FIRST_REMOTE)
		return false; /* a letter the format does not define */
	int i = flat - CW_STATS_SLOT_ACH;
	p->ach[i].local = local;
	p->ach[i].remote = remote;
	p->ach_first[i] = (uint8_t)letter;
	return true;
}

/* Keep a record this build cannot hold, verbatim and without its line ending,
 * so the next save re-emits it [design 4, R1 HIGH 2]. Always flags the
 * profile incomplete. Returns false when the record cannot be kept -- the
 * list is full, or the line is longer than CW_STATS_FOREIGN_LINE_MAX (see the
 * width derivation there) -- and the caller then write-refuses the profile
 * rather than letting a later save drop it. */
static bool keep_foreign(struct stats_profile* p, const char* body) {
	p->progress_incomplete = 1;
	if(p->foreign_count >= CW_STATS_FOREIGN_MAX)
		return false;
	size_t n = strlen(body);
	if(n + 1 > CW_STATS_FOREIGN_LINE_MAX)
		return false;
	memcpy(p->foreign[p->foreign_count], body, n + 1);
	p->foreign_count++;
	return true;
}

/* Record that this `s`/`a` id has been seen, and answer whether it is the
 * FIRST time [design 4 "A duplicate `s`/`a` id is corrupt", R1 LOW 12].
 * A representable id gets a bit in the flat map; an id this build has no slot
 * for is tracked in the small foreign-id array instead (bounded by the same
 * 64 the keep-list is, and a 65th foreign record write-refuses the profile
 * anyway, so an id past the array cannot silently escape the check). */
static bool mark_id_seen(unsigned char* seen, uint32_t* foreign_id,
						 int* foreign_ids, int flat, uint32_t id) {
	if(flat >= 0) {
		if(seen[flat >> 3] & (unsigned char)(1u << (flat & 7)))
			return false;
		seen[flat >> 3] |= (unsigned char)(1u << (flat & 7));
		return true;
	}
	for(int i = 0; i < *foreign_ids; i++)
		if(foreign_id[i] == id)
			return false;
	if(*foreign_ids < CW_STATS_FOREIGN_MAX)
		foreign_id[(*foreign_ids)++] = id;
	return true;
}

/* Parse one file into `*p` (which the caller reset first). `*p` is left
 * partially filled on PARSE_CORRUPT -- the caller resets again before trying
 * the next candidate, because a crc mismatch is only knowable at the end. */
static enum parse_result parse_file(const char* path, const char* profile_id,
									struct stats_profile* p) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return PARSE_MISSING;

	uLong crc = crc32(0, NULL, 0);
	char line[CW_STATS_LINE_MAX];
	char body[CW_STATS_LINE_MAX];
	/* One bit per representable counter, plus the ids of the foreign records
	 * (which have no bit): together they make every `s`/`a` id in the file
	 * unique or the file corrupt. ~1.2 KB of frame on a main-thread load. */
	unsigned char seen[(CW_STATS_SLOT_COUNT + 7) / 8];
	uint32_t foreign_id[CW_STATS_FOREIGN_MAX];
	int foreign_ids = 0;
	bool saw_version = false, saw_profile = false, saw_end = false;
	bool identity_ok = false;
	uint32_t counted = 0, declared = 0, declared_crc = 0;
	enum parse_result result = PARSE_CORRUPT;

	memset(seen, 0, sizeof(seen));

	while(fgets(line, (int)sizeof(line), f)) {
		size_t len = strlen(line);
		if(len == 0)
			break;
		if(line[len - 1] != '\n' && !feof(f)) {
			/* A line longer than the buffer: not something this writer ever
			 * produces, so the file is damaged. */
			goto done;
		}

		bool is_end = strncmp(line, "end ", 4) == 0;
		if(!is_end)
			crc = crc32(crc, (const Bytef*)line, (uInt)len);

		/* Work on a NUL-terminated copy without the line ending; the crc
		 * above already consumed the raw bytes. */
		memcpy(body, line, len);
		body[len] = '\0';
		while(len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r'))
			body[--len] = '\0';

		if(is_end) {
			/* The terminator is excluded from the crc it carries. */
			const char* cur = body + 3;
			if(!scan_u32(&cur, &declared) || !scan_hex32(&cur, &declared_crc)
			   || !scan_end(cur))
				goto done;
			saw_end = true;
			break;
		}

		if(!saw_version) {
			if(strncmp(body, "cwstats ", 8) != 0)
				goto done;
			const char* cur = body + 7;
			uint32_t version = 0;
			if(!scan_u32(&cur, &version) || !scan_end(cur) || version < 1)
				goto done;
			if(version > (uint32_t)STATS_FILE_VERSION) {
				/* Newer than this build understands: stop reading, leave the
				 * file alone, refuse to write it [design 4]. */
				result = PARSE_UNSUPPORTED;
				goto done;
			}
			saw_version = true;
			continue;
		}

		if(strncmp(body, "profile ", 8) == 0) {
			if(saw_profile)
				goto done;
			saw_profile = true;
			/* Identity check [design 4]: the header must name the key the
			 * file was opened for, or this is somebody else's progress. */
			identity_ok = strcmp(body + 8, profile_id) == 0;
		} else if(strncmp(body, "saved ", 6) == 0) {
			/* Informational; never used to make a decision. */
		} else if(strncmp(body, "incomplete ", 11) == 0) {
			/* The persisted progress_incomplete flag [R1 HIGH 2]. A malformed
			 * value is damage like any other malformed known record. */
			const char* cur = body + 10;
			uint32_t v = 0;
			if(!scan_u32(&cur, &v) || !scan_end(cur))
				goto done;
			if(v != 0)
				p->progress_incomplete = 1;
		} else if(body[0] == 's' && body[1] == ' ') {
			const char* cur = body + 1;
			uint32_t id = 0, local = 0, remote = 0;
			if(!scan_u32(&cur, &id) || !scan_u32(&cur, &local)
			   || !scan_u32(&cur, &remote) || !scan_end(cur))
				goto done;
			if(!mark_id_seen(seen, foreign_id, &foreign_ids, counter_flat(id),
							 id))
				goto done; /* duplicate id [design 4, R1 LOW 12] */
			if(!apply_counter(p, id, local, remote) && !keep_foreign(p, body))
				p->write_refused = (uint8_t)STATS_WRITE_REFUSED_FOREIGN;
			counted++;
		} else if(body[0] == 'a' && body[1] == ' ') {
			const char* cur = body + 1;
			uint32_t id = 0, local = 0, remote = 0;
			if(!scan_u32(&cur, &id) || !scan_u32(&cur, &local)
			   || !scan_u32(&cur, &remote))
				goto done;
			while(*cur == ' ')
				cur++;
			char letter = *cur;
			if(letter == '\0' || !scan_end(cur + 1))
				goto done;
			if(!mark_id_seen(seen, foreign_id, &foreign_ids, ach_flat(id), id))
				goto done; /* duplicate id [design 4, R1 LOW 12] */
			if(!apply_achievement(p, id, local, remote, letter)
			   && !keep_foreign(p, body))
				p->write_refused = (uint8_t)STATS_WRITE_REFUSED_FOREIGN;
			counted++;
		} else {
			/* A record letter this build does not know: NOT counted, so
			 * `end <count>` keeps meaning "counter records" and a later
			 * version can add lines a v1 reader ignores [design 4] -- but it
			 * is KEPT, so that reader's next save does not delete the line it
			 * was told to ignore [R1 HIGH 2]. */
			if(!keep_foreign(p, body))
				p->write_refused = (uint8_t)STATS_WRITE_REFUSED_FOREIGN;
		}
	}

	if(saw_end && saw_version && saw_profile && identity_ok
	   && counted == declared && (uint32_t)(crc & 0xFFFFFFFFul) == declared_crc)
		result = PARSE_OK;
	else
		result = PARSE_CORRUPT;

done:
	fclose(f);
	return result;
}

void stats_profile_load(const char* dir, const char* profile_id,
						struct stats_profile* p,
						enum stats_load_status* status) {
	if(!p) {
		if(status)
			*status = STATS_LOAD_BAD_ARG;
		return;
	}
	stats_profile_reset(p, profile_id);
	/* The SAME key rule the writer uses (R1 MED 3): a key the writer would
	 * reject must never be turned into a path here either, or a 17-character
	 * key loads one file and saves another. */
	if(!dir || !stats_profile_id_valid(profile_id)) {
		if(status)
			*status = STATS_LOAD_BAD_ARG;
		return;
	}

	char main_path[CW_STATS_PATH_MAX];
	char bak_path[CW_STATS_PATH_MAX];
	char bad_path[CW_STATS_PATH_MAX];
	if(!stats_profile_path(main_path, sizeof(main_path), dir, profile_id, "")
	   || !stats_profile_path(bak_path, sizeof(bak_path), dir, profile_id,
							  ".bak")
	   || !stats_profile_path(bad_path, sizeof(bad_path), dir, profile_id,
							  ".bad")) {
		if(status)
			*status = STATS_LOAD_BAD_ARG;
		return;
	}

	enum parse_result r = parse_file(main_path, profile_id, p);
	if(r == PARSE_OK) {
		if(status)
			*status = STATS_LOAD_OK;
		return;
	}
	if(r == PARSE_UNSUPPORTED) {
		stats_profile_reset(p, profile_id);
		p->write_refused = (uint8_t)STATS_WRITE_REFUSED_NEWER;
		if(status)
			*status = STATS_LOAD_UNSUPPORTED_VERSION;
		return;
	}
	uint8_t main_corrupt = 0;
	if(r == PARSE_CORRUPT) {
		/* Quarantine, never delete -- and THE NEWEST QUARANTINE WINS (R1
		 * HIGH 1). The old rule kept the FIRST `.bad` and left the damaged
		 * main file where it was; the next save then rotated that
		 * known-corrupt file over `.bak`, i.e. over the only good copy this
		 * load had just recovered from, and a promote failure after that left
		 * the profile with nothing loadable at all. The newest evidence is
		 * also the more useful one to keep. */
		remove(bad_path);
		main_corrupt = 1; /* even if the rename below works: see the flag's
							 contract -- it is cleared by the owner after the
							 first save our own writer performed. */
		rename(main_path, bad_path);
	}

	/* `.bak` is also the crash window between the two renames of a save: a
	 * MISSING main file with a good `.bak` is recoverable progress, not a
	 * fresh profile. */
	stats_profile_reset(p, profile_id);
	p->main_was_corrupt = main_corrupt;
	enum parse_result rb = parse_file(bak_path, profile_id, p);
	if(rb == PARSE_OK) {
		if(status)
			*status = STATS_LOAD_RECOVERED_BAK;
		return;
	}
	if(rb == PARSE_UNSUPPORTED) {
		stats_profile_reset(p, profile_id);
		p->main_was_corrupt = main_corrupt;
		p->write_refused = (uint8_t)STATS_WRITE_REFUSED_NEWER;
		if(status)
			*status = STATS_LOAD_UNSUPPORTED_VERSION;
		return;
	}

	stats_profile_reset(p, profile_id);
	p->main_was_corrupt = main_corrupt;
	if(status)
		*status = (r == PARSE_MISSING && rb == PARSE_MISSING)
			? STATS_LOAD_MISSING
			: STATS_LOAD_CORRUPT;
}
