/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Original server CFR World.java:469-515,1269-1331 and
 * MetadataChunkBlock.java:35-171. Included by server_world.c. */
struct server_beta_light_region {
	int channel;
	int x1, y1, z1, x2, y2, z2;
};

#ifdef CAVEWII_SERVER_LOCAL_TEST
static void server_beta_light_history(struct server_world* w, const char* op,
	struct server_beta_light_region m, size_t before, size_t target,
	const char* file, int line) {
	lighting_test_history_emit((struct lighting_history_event){
		.operation = op, .object = w, .channel = m.channel,
		.x = m.x1, .y = m.y1, .z = m.z1,
		.x2 = m.x2, .y2 = m.y2, .z2 = m.z2,
		.index = target,
		.before_depth = before, .after_depth = w->generation_light_n,
		.file = file, .line = line});
}
#define G9B_REGION(w, op, m, before) server_beta_light_history(w, op, m, before, 0, __FILE__, __LINE__)
#define G9B_REGION_MERGE(w, m, target) server_beta_light_history(w, "region-merge", m, (w)->generation_light_n, target, __FILE__, __LINE__)
#else
#define G9B_REGION(w, op, m, before) ((void)0)
#define G9B_REGION_MERGE(w, m, target) ((void)0)
#endif

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* G9X-4 test-only allocation fault points. While a population phase in
 * s_pop_fault_mask is open (s_pop_fault_phase), every reservation attempt
 * at a fault site is one numbered point, whether or not it would have to
 * grow: any of them can be the one that grows. Point s_pop_fault_at fails
 * exactly as the allocator failing there would (-1 = never). */
static unsigned s_pop_fault_mask, s_pop_fault_phase;
static long s_pop_fault_at = -1;
static size_t s_pop_fault_points;
static bool server_beta_light_test_fault(void) {
	if(!(s_pop_fault_phase & s_pop_fault_mask)) return false;
	const size_t point = s_pop_fault_points++;
	return s_pop_fault_at >= 0 && point == (size_t)s_pop_fault_at;
}
#define G9X_POP_FAULT() server_beta_light_test_fault()
#else
#define G9X_POP_FAULT() false
#endif

/* ---- G9X-5: the bounded, lossless paged region LIFO ----------------------
 * Beta keeps World.field_821_y as one ArrayList (CFR World.java:1293-1331):
 * append at the end, merge into one of the newest five, pop the last
 * (World.java:1268-1289, MetadataChunkBlock.java:58-139). The LOGICAL list
 * here is unchanged; only where its old entries are stored is bounded:
 *   - resident: at most `slots` page slots of SBL_PAGE entries, page p in
 *     slot p % slots (production 128 slots = 16,384 entries x 28 B = 448 KiB
 *     per world, the seven reserved relax slots included);
 *   - pages [0, base) are in an owned temporary backing file, written only
 *     when the resident window would overflow (oldest first) and read back
 *     only when the unchanged LIFO comes within six entries of them, so the
 *     newest five (the merge window) are always resident before any use.
 * Every step that can fail (open/seek/write/flush/read/slot alloc) happens
 * before the logical list changes, so a failure retains all work: never a
 * dropped, reordered or early-drained region. Failures are CLASSIFIED: a
 * backing-file I/O failure (open, seek, read, short read, write, short
 * write, flush) is recorded on the failing world (generation_light_io_*);
 * an allocation failure (page slot, ring, backing struct) is counted apart.
 * During population a backing I/O failure is TERMINAL for that world
 * (server_world.c, server_world_population_storage_stop: the sticky
 * POPULATION_STORAGE_FAILED stop, no automatic retry); a drain failure keeps
 * the existing DRAIN_OOM stop with its retained work.
 * Each backing page has two physical slots; a checkpoint trial writes a page
 * it spills to the slot it is NOT restoring from (copy-on-write), so the
 * trial restores by cursor (n, base, the resident window and the slot bits)
 * instead of copying the whole logical list. */
#define SBL_PAGE 128u
#define SBL_SLOTS 128u
#define SBL_MAX_REGIONS 1000000u /* World.java:1321 (Beta clears; we stop) */
#define SBL_MAX_PAGES ((SBL_MAX_REGIONS + SBL_PAGE) / SBL_PAGE)
#define SBL_BITMAP_BYTES ((SBL_MAX_PAGES + 7u) / 8u)
#define SBL_PAGE_BYTES (SBL_PAGE * sizeof(struct server_beta_light_region))
#define SBL_TAIL 5u
_Static_assert(sizeof(struct server_beta_light_region) == 28,
	"seven 32-bit fields, no padding (PPC and PC)");
_Static_assert(SBL_PAGE * 28u == 3584u, "3,584 B per page");
_Static_assert(SBL_SLOTS * SBL_PAGE == 16384u, "16,384 resident entries incl. 7 reserve");
_Static_assert(SBL_SLOTS * SBL_PAGE * 28u == 458752u, "448 KiB resident per world");
_Static_assert(2u * 458752u + 458752u + 131072u == 1472u * 1024u,
	"two worlds + one checkpoint copy + 128 KiB metadata = 1,472 KiB");

struct server_beta_light_backing {
	FILE* file;
	size_t file_pages;      /* physical page slots ever written (high water) */
	bool trial;             /* a checkpoint trial owns the copy-on-write bits */
	uint8_t patch_n;        /* rolled-back tail entries not yet resident */
	size_t patch_index[SBL_TAIL];
	struct server_beta_light_region patch[SBL_TAIL];
	uint8_t loc[SBL_BITMAP_BYTES];     /* which of the two physical slots */
	uint8_t flipped[SBL_BITMAP_BYTES]; /* moved during the current trial */
	char path[512];
};
_Static_assert(2u * (sizeof(struct server_beta_light_backing)
	+ SBL_SLOTS * sizeof(void*)) + SBL_BITMAP_BYTES <= 131072u,
	"queue metadata (two worlds + one trial bitmap) within 128 KiB");

/* ---- PPC (Wii) allocator ledger (G9X-5 item 17: "separately budget
 * allocator headers"). The payload ledger counts requested bytes; on the
 * Wii every allocation can consume more. Each charge below is an UPPER
 * bound read from the SDK sources/binaries the hardware build links
 * (G9X-5a P3, tmp/jobs/loop/G9X-5a/NOTES.txt):
 *   - mem2_alloc asks the lwp heap for size + MEM2_GUARD (256 B,
 *     source/platform/mem2.c:359; the heap is __lwp_heap_init(.., 32),
 *     mem2.c:579). libogc __lwp_heap_allocate (logc256kr/libogc/
 *     lwp_heap.c:56-62) takes dsize = size + pg_size(32) +
 *     HEAP_BLOCK_USED_OVERHEAD (2 pointers = 8, gc/ogc/lwp_heap.h:14),
 *     rounded up to a page; when the free block's remainder is at most
 *     pg_size + HEAP_BLOCK_USED_OVERHEAD (40 B) it takes the WHOLE block
 *     (lwp_heap.c:70-82), so one allocation consumes at most dsize + 40;
 *   - newlib malloc (dlmalloc): a 4-byte size header, 8-byte alignment,
 *     and a remainder below MINSIZE (16 B) left inside the chunk:
 *     at most round8(size + 4) + 16;
 *   - the backing stream (opened once per world, unbuffered: setvbuf
 *     _IONBF before any I/O, so no BUFSIZ stdio buffer is ever made):
 *       newlib fopen -> __sfp: at most one __sfmoreglue block of 4 FILEs
 *       (struct _glue, 3 words, + 4 x sizeof(FILE); sizeof(FILE) bounded by
 *       SBL_WII_SFILE_MAX, static-asserted on every build incl. the Wii's);
 *       libsysbase open -> one handle malloc(sizeof(__handle) + structSize):
 *       structSize = 40 B read from the linked sidecar libfat.a
 *       (_FAT_devoptab, fat_driver.o; tmp/jobs/loop/G9X-5a/
 *       libfat_handle_bound.py), the __handle header (3 words) bounded by
 *       64 B. Every malloc/calloc/aligned_alloc in that libfat.a is reached
 *       only from mount/create/destroy (dvmDiscCreate, dvmDiscCacheCreate,
 *       dvmMountVolume, the partition prober); ff.o and fat_driver.o
 *       (open/read/write/seek/close) import no allocator at all;
 *   - PPC32 pointers are 4 bytes; the backing struct's PPC size is bounded
 *     by SBL_WII_BACKING_BYTES (static-asserted on the Wii build). */
#define SBL_WII_LWP_PAGE 32u
#define SBL_WII_LWP_USED_OVERHEAD 8u
#define SBL_WII_MEM2_COST(sz) (((((size_t)(sz)) + 256u + SBL_WII_LWP_PAGE - 1u) / SBL_WII_LWP_PAGE) \
	* SBL_WII_LWP_PAGE + SBL_WII_LWP_PAGE + SBL_WII_LWP_USED_OVERHEAD \
	+ (SBL_WII_LWP_PAGE + SBL_WII_LWP_USED_OVERHEAD))
#define SBL_WII_MALLOC_COST(sz) (((((size_t)(sz)) + 4u + 7u) / 8u) * 8u + 16u)
#define SBL_WII_SFILE_MAX 256u      /* sizeof(FILE), PPC newlib */
#define SBL_WII_GLUE_HDR 12u        /* struct _glue: next, niobs, iobs */
#define SBL_WII_FAT_STRUCT 40u      /* libfat _FAT_devoptab.structSize */
#define SBL_WII_HANDLE_HDR_MAX 64u  /* libsysbase __handle (3 words) */
#define SBL_WII_FILE_BYTES (SBL_WII_MALLOC_COST(SBL_WII_GLUE_HDR + 4u * SBL_WII_SFILE_MAX) \
	+ SBL_WII_MALLOC_COST(SBL_WII_HANDLE_HDR_MAX + SBL_WII_FAT_STRUCT))
#define SBL_WII_PTR 4u
#define SBL_WII_BACKING_BYTES (4u + 4u + 4u + SBL_TAIL * 4u + SBL_TAIL * 28u \
	+ 2u * SBL_BITMAP_BYTES + 512u + 8u)
#ifdef PLATFORM_WII
_Static_assert(sizeof(struct server_beta_light_backing) <= SBL_WII_BACKING_BYTES,
	"PPC backing struct within its ledger bound");
_Static_assert(sizeof(void*) == SBL_WII_PTR, "PPC32 pointers");
#endif
_Static_assert(sizeof(FILE) <= SBL_WII_SFILE_MAX, "FILE within its ledger bound");
_Static_assert(SBL_WII_MEM2_COST(3584u) == 3920u,
	"a 3,584 B page costs at most 3,920 B of MEM2 carve (whole-block take included)");
_Static_assert(SBL_WII_FILE_BYTES == 1184u, "the backing stream's SDK state: at most 1,184 B");
/* the worst case, all at once: two worlds with full rings + their metadata
 * and FILE state, one checkpoint window copy (16,384 entries + its extra
 * byte) and one trial slot bitmap. */
#define SBL_WII_LEDGER_MAX (2u * (SBL_SLOTS * SBL_WII_MEM2_COST(SBL_PAGE_BYTES) \
	+ SBL_WII_MALLOC_COST(SBL_SLOTS * SBL_WII_PTR) + SBL_WII_MALLOC_COST(SBL_WII_BACKING_BYTES) \
	+ SBL_WII_FILE_BYTES) + SBL_WII_MALLOC_COST(SBL_SLOTS * SBL_PAGE_BYTES + 1u) \
	+ SBL_WII_MALLOC_COST(SBL_BITMAP_BYTES))
_Static_assert(SBL_WII_LEDGER_MAX == 1472080u, "the conservative PPC worst case: 1,472,080 B");
_Static_assert(SBL_WII_LEDGER_MAX <= 1472u * 1024u,
	"PPC allocator ledger of the whole queue subsystem within 1,472 KiB");
#define SBL_BACKING_WII (SBL_WII_MALLOC_COST(SBL_WII_BACKING_BYTES) + SBL_WII_FILE_BYTES)

/* failure sites (== enum server_world_test_lq_site) */
#define SBL_SITE_SPILL 1u
#define SBL_SITE_LOAD 2u
#define SBL_SITE_OPEN 3u
#define SBL_SITE_ALLOC 4u

static size_t s_sbl_bytes, s_sbl_peak, s_sbl_world_peak, s_sbl_ckpt_peak;
static size_t s_sbl_wii_bytes, s_sbl_wii_peak;
static size_t s_sbl_storage_fail, s_sbl_alloc_fail;
static unsigned s_sbl_fail_site, s_sbl_fail_cause;
/* A backing-file I/O failure of w: recorded on the world (the population
 * terminal stop reads it), counted, never conflated with allocation. */
static bool sbl_io_fail(struct server_world* w, unsigned site,
	enum server_population_storage_cause cause) {
	s_sbl_storage_fail++;
	s_sbl_fail_site = site;
	s_sbl_fail_cause = cause;
	w->generation_light_io_fails++;
	w->generation_light_io_site = (uint8_t)site;
	w->generation_light_io_cause = (uint8_t)cause;
	return false;
}
/* An allocation failure (page slot, ring, backing struct): not storage I/O. */
static bool sbl_alloc_fail(unsigned site) {
	s_sbl_alloc_fail++;
	s_sbl_fail_site = site;
	s_sbl_fail_cause = SERVER_POPULATION_STORAGE_NONE;
	return false;
}
#ifdef CAVEWII_SERVER_LOCAL_TEST
_Static_assert(SBL_SITE_SPILL == SERVER_WORLD_TEST_LQ_SITE_SPILL
	&& SBL_SITE_LOAD == SERVER_WORLD_TEST_LQ_SITE_LOAD
	&& SBL_SITE_OPEN == SERVER_WORLD_TEST_LQ_SITE_OPEN
	&& SBL_SITE_ALLOC == SERVER_WORLD_TEST_LQ_SITE_ALLOC, "site ids");
static long s_sbl_fault_at[SERVER_WORLD_TEST_LQ_FAULTS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
_Static_assert(SERVER_WORLD_TEST_LQ_FAULTS == 9, "one initializer per fault kind");
static size_t s_sbl_fault_n[SERVER_WORLD_TEST_LQ_FAULTS];
static uint32_t s_sbl_slots_default = SBL_SLOTS;
static bool s_sbl_force_spill; /* the next reserve first spills one page */
static bool sbl_fault(enum server_world_test_lq_fault k) {
	const size_t at = s_sbl_fault_n[k]++;
	return s_sbl_fault_at[k] >= 0 && at == (size_t)s_sbl_fault_at[k];
}
#define SBL_FAULT(k) sbl_fault(k)
#define SBL_SLOTS_DEFAULT s_sbl_slots_default
#else
#define SBL_FAULT(k) false
#define SBL_SLOTS_DEFAULT SBL_SLOTS
#endif

static void sbl_account(ptrdiff_t delta, ptrdiff_t wii) {
	s_sbl_bytes = (size_t)((ptrdiff_t)s_sbl_bytes + delta);
	if(s_sbl_bytes > s_sbl_peak) s_sbl_peak = s_sbl_bytes;
	s_sbl_wii_bytes = (size_t)((ptrdiff_t)s_sbl_wii_bytes + wii);
	if(s_sbl_wii_bytes > s_sbl_wii_peak) s_sbl_wii_peak = s_sbl_wii_bytes;
}

static size_t sbl_resident_bytes(const struct server_world* w) {
	size_t b = 0;
	for(uint32_t i = 0; w->generation_light_pages && i < w->generation_light_slots; i++)
		if(w->generation_light_pages[i]) b += SBL_PAGE_BYTES;
	return b;
}

static struct server_beta_light_region* sbl_at(struct server_world* w, size_t i) {
	/* nether-drain-1030 (T1): the slot count is a power of two in production
	 * (SBL_SLOTS 128; tests may pick any), so the page modulo is a mask there
	 * instead of a hardware divide (~20 cycles each, five per merge window). */
	const size_t p = i / SBL_PAGE, slots = w->generation_light_slots;
	const size_t slot = (slots & (slots - 1)) == 0 ? (p & (slots - 1)) : p % slots;
	return &w->generation_light_pages[slot][i % SBL_PAGE];
}

static bool sbl_bit(const uint8_t* m, size_t p) { return (m[p / 8] >> (p % 8)) & 1u; }
static void sbl_set(uint8_t* m, size_t p, bool v) {
	if(v) m[p / 8] |= (uint8_t)(1u << (p % 8));
	else m[p / 8] &= (uint8_t)~(1u << (p % 8));
}

static bool sbl_seek(FILE* f, size_t phys, size_t offset) {
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_SEEK)) return false;
	return fseek(f, (long)(phys * SBL_PAGE_BYTES + offset), SEEK_SET) == 0;
}

static bool sbl_backing_open(struct server_world* w) {
	if(w->generation_light_backing) return true;
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_OPEN))
		return sbl_io_fail(w, SBL_SITE_OPEN, SERVER_POPULATION_STORAGE_OPEN);
	struct server_beta_light_backing* b = calloc(1, sizeof *b);
	if(!b) return sbl_alloc_fail(SBL_SITE_OPEN);
	snprintf(b->path, sizeof b->path, "%s/cw_light_regions_%u_%d.tmp",
		string_get_cstr(w->level_name), (unsigned)w->world_index, (int)w->dimension);
	b->file = fopen(b->path, "w+b");
	if(!b->file) {
		free(b);
		return sbl_io_fail(w, SBL_SITE_OPEN, SERVER_POPULATION_STORAGE_OPEN);
	}
	setvbuf(b->file, NULL, _IONBF, 0);
	w->generation_light_backing = b;
	sbl_account((ptrdiff_t)sizeof *b, (ptrdiff_t)SBL_BACKING_WII);
	return true;
}

/* Spill the oldest resident page (base) to the backing store. */
static bool sbl_spill(struct server_world* w) {
	if(!sbl_backing_open(w)) return false;
	struct server_beta_light_backing* b = w->generation_light_backing;
	const size_t p = w->generation_light_base;
	if(p >= SBL_MAX_PAGES) return false;
	bool loc = sbl_bit(b->loc, p);
	if(b->trial && !sbl_bit(b->flipped, p)) loc = !loc; /* copy-on-write */
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_WRITE))
		return sbl_io_fail(w, SBL_SITE_SPILL, SERVER_POPULATION_STORAGE_WRITE);
	const size_t phys = 2 * p + loc;
	/* Any failure below leaves page p resident and base unchanged; the slot
	 * written is never one a restore or a later load reads (non-trial: the
	 * resident page is authoritative; trial: the copy-on-write slot). */
	const void* page = w->generation_light_pages[p % w->generation_light_slots];
	if(!sbl_seek(b->file, phys, 0))
		return sbl_io_fail(w, SBL_SITE_SPILL, SERVER_POPULATION_STORAGE_SEEK);
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_SHORT_WRITE)) {
		(void)fwrite(page, 1, SBL_PAGE_BYTES / 2, b->file); /* a real short write */
		return sbl_io_fail(w, SBL_SITE_SPILL, SERVER_POPULATION_STORAGE_SHORT_WRITE);
	}
	const size_t wrote = fwrite(page, 1, SBL_PAGE_BYTES, b->file);
	if(wrote != SBL_PAGE_BYTES)
		return sbl_io_fail(w, SBL_SITE_SPILL, wrote ? SERVER_POPULATION_STORAGE_SHORT_WRITE
			: SERVER_POPULATION_STORAGE_WRITE);
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_FLUSH) || fflush(b->file) != 0)
		return sbl_io_fail(w, SBL_SITE_SPILL, SERVER_POPULATION_STORAGE_FLUSH);
	if(b->trial) sbl_set(b->flipped, p, true);
	sbl_set(b->loc, p, loc);
	if(phys + 1 > b->file_pages) b->file_pages = phys + 1;
	w->generation_light_base = p + 1;
	w->generation_light_stat[0]++;
	return true;
}

static bool sbl_slot_alloc(struct server_world* w, size_t p) {
	struct server_beta_light_region** slot = &w->generation_light_pages[p % w->generation_light_slots];
	if(*slot) return true;
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_ALLOC)) return sbl_alloc_fail(SBL_SITE_ALLOC);
	*slot = mem2_alloc(SBL_PAGE_BYTES);
	if(!*slot) return sbl_alloc_fail(SBL_SITE_ALLOC);
	sbl_account((ptrdiff_t)SBL_PAGE_BYTES, (ptrdiff_t)SBL_WII_MEM2_COST(SBL_PAGE_BYTES));
	const size_t rb = sbl_resident_bytes(w);
	if(rb > s_sbl_world_peak) s_sbl_world_peak = rb;
	return true;
}

/* Load page base-1 back into its resident slot (+ pending rollback patches). */
static bool sbl_load(struct server_world* w) {
	struct server_beta_light_backing* b = w->generation_light_backing;
	assert(b && w->generation_light_base > 0);
	const size_t p = w->generation_light_base - 1;
	if(!sbl_slot_alloc(w, p)) { s_sbl_fail_site = SBL_SITE_LOAD; return false; }
	struct server_beta_light_region* page = w->generation_light_pages[p % w->generation_light_slots];
	/* Any failure below leaves base unchanged: the slot read into holds no
	 * live page (only the newest <= 5 entries are resident here). */
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_READ))
		return sbl_io_fail(w, SBL_SITE_LOAD, SERVER_POPULATION_STORAGE_READ);
	if(!sbl_seek(b->file, 2 * p + sbl_bit(b->loc, p), 0))
		return sbl_io_fail(w, SBL_SITE_LOAD, SERVER_POPULATION_STORAGE_SEEK);
	const size_t got = fread(page, 1, SBL_FAULT(SERVER_WORLD_TEST_LQ_SHORT_READ)
		? SBL_PAGE_BYTES / 2 : SBL_PAGE_BYTES, b->file); /* the fault: a real short read */
	if(got != SBL_PAGE_BYTES)
		return sbl_io_fail(w, SBL_SITE_LOAD, got ? SERVER_POPULATION_STORAGE_SHORT_READ
			: SERVER_POPULATION_STORAGE_READ);
	for(unsigned k = 0; k < b->patch_n;) {
		if(b->patch_index[k] / SBL_PAGE == p) {
			page[b->patch_index[k] % SBL_PAGE] = b->patch[k];
			b->patch_n--;
			b->patch_index[k] = b->patch_index[b->patch_n];
			b->patch[k] = b->patch[b->patch_n];
		} else k++;
	}
	w->generation_light_base = p;
	w->generation_light_stat[1]++;
	return true;
}

/* The newest min(n, 6) entries resident: the next pop's region and, after
 * it, the newest five (the merge window). */
static bool sbl_ensure_tail(struct server_world* w) {
	while(w->generation_light_base > 0
		  && w->generation_light_n < w->generation_light_base * SBL_PAGE + SBL_TAIL + 1)
		if(!sbl_load(w)) return false;
	assert(!w->generation_light_backing || w->generation_light_backing->patch_n == 0);
	return true;
}

/* G9X-5b preflight inputs, read without allocating: the page slots
 * sbl_ensure_tail would allocate now (the same loop, one mem2_alloc of
 * SBL_PAGE_BYTES per empty slot it loads into), and the slots a drain can
 * still allocate lazily (sbl_slot_alloc never frees a slot before sbl_free,
 * so at most every slot that is still empty). */
static size_t sbl_plan_tail_pages(const struct server_world* w) {
	size_t k = 0;
	for(size_t base = w->generation_light_base;
		base > 0 && w->generation_light_n < base * SBL_PAGE + SBL_TAIL + 1; base--)
		if(!w->generation_light_pages
		   || !w->generation_light_pages[(base - 1) % w->generation_light_slots])
			k++;
	return k;
}
static size_t sbl_plan_free_slots(const struct server_world* w) {
	const uint32_t slots = w->generation_light_slots ? w->generation_light_slots
		: SBL_SLOTS_DEFAULT;
	size_t k = slots;
	for(uint32_t i = 0; w->generation_light_pages && i < slots; i++)
		if(w->generation_light_pages[i]) k--;
	return k;
}

static bool sbl_ring_init(struct server_world* w) {
	if(w->generation_light_pages) return true;
	const uint32_t slots = w->generation_light_slots ? w->generation_light_slots : SBL_SLOTS_DEFAULT;
	w->generation_light_pages = calloc(slots, sizeof *w->generation_light_pages);
	if(!w->generation_light_pages) return sbl_alloc_fail(SBL_SITE_ALLOC);
	w->generation_light_slots = slots;
	sbl_account((ptrdiff_t)(slots * sizeof *w->generation_light_pages),
		(ptrdiff_t)SBL_WII_MALLOC_COST(slots * SBL_WII_PTR));
	return true;
}

static bool server_beta_light_reserve(struct server_world* w, size_t extra) {
	if(G9X_POP_FAULT()) return false;
	if(SBL_FAULT(SERVER_WORLD_TEST_LQ_RESERVE)) return false;
	if(!sbl_ring_init(w) || !sbl_ensure_tail(w)) return false;
	if(extra > SBL_MAX_REGIONS) return false;
	const size_t n = w->generation_light_n;
	const size_t window = (size_t)w->generation_light_slots * SBL_PAGE;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	/* test seam: spill one (spillable) page early -- logically invisible,
	 * it only moves where an old page lives -- so a storage fault can be
	 * placed at an exact reserve call */
	if(s_sbl_force_spill && (w->generation_light_base + 1) * SBL_PAGE + SBL_TAIL <= n) {
		s_sbl_force_spill = false;
		if(!sbl_spill(w)) return false;
	}
#endif
	while(n + extra > w->generation_light_base * SBL_PAGE + window) {
		/* keep the newest five resident; a request this large cannot fit */
		if((w->generation_light_base + 1) * SBL_PAGE + SBL_TAIL > n) return false;
		if(!sbl_spill(w)) return false;
	}
	for(size_t p = n / SBL_PAGE; p * SBL_PAGE < n + extra; p++)
		if(!sbl_slot_alloc(w, p)) return false;
	return true;
}

/* Population rollback (CaveWii infrastructure): the logical list returns to
 * n_before with its pre-transaction mergeable tail. No I/O: a tail entry
 * whose page was spilled inside the transaction is held as a patch and
 * applied when the unchanged LIFO loads that page back. */
static void sbl_rollback(struct server_world* w, size_t n_before,
	const struct server_beta_light_region* tail, size_t tail_n) {
	w->generation_light_n = n_before;
	if(w->generation_light_base * SBL_PAGE > n_before)
		w->generation_light_base = (n_before + SBL_PAGE - 1) / SBL_PAGE;
	for(size_t k = 0; k < tail_n; k++) {
		const size_t i = n_before - tail_n + k;
		if(i >= w->generation_light_base * SBL_PAGE) { *sbl_at(w, i) = tail[k]; continue; }
		struct server_beta_light_backing* b = w->generation_light_backing;
		assert(b && b->patch_n < SBL_TAIL);
		b->patch_index[b->patch_n] = i;
		b->patch[b->patch_n++] = tail[k];
	}
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Copy entry i of the logical list (test views; reads the backing file for
 * a spilled entry without changing the live list). */
static bool sbl_peek(const struct server_world* w, size_t i, struct server_beta_light_region* out) {
	if(i >= w->generation_light_n) return false;
	if(i >= w->generation_light_base * SBL_PAGE) {
		*out = *sbl_at((struct server_world*)w, i);
		return true;
	}
	const struct server_beta_light_backing* b = w->generation_light_backing;
	if(!b) return false;
	for(unsigned k = 0; k < b->patch_n; k++)
		if(b->patch_index[k] == i) { *out = b->patch[k]; return true; }
	const size_t p = i / SBL_PAGE;
	/* a raw seek: a test view never consumes an armed storage fault */
	return fseek(b->file, (long)((2 * p + sbl_bit(b->loc, p)) * SBL_PAGE_BYTES
		+ (i % SBL_PAGE) * sizeof *out), SEEK_SET) == 0
		&& fread(out, sizeof *out, 1, b->file) == 1;
}

/* G9X-5a copy-on-write witness view: the raw bytes of physical slot
 * `phys_slot` (0/1) of backing page p, and p's slot bits (raw seeks: a test
 * view never consumes an armed fault; every production access seeks first). */
static bool sbl_test_page(const struct server_world* w, size_t p, unsigned phys_slot,
	void* out, int* loc, bool* flipped, bool* trial) {
	const struct server_beta_light_backing* b = w->generation_light_backing;
	if(!b || p >= SBL_MAX_PAGES || phys_slot > 1) return false;
	if(loc) *loc = sbl_bit(b->loc, p);
	if(flipped) *flipped = sbl_bit(b->flipped, p);
	if(trial) *trial = b->trial;
	if(!out) return true;
	if(2 * p + phys_slot >= b->file_pages) return false;
	return fseek(b->file, (long)((2 * p + phys_slot) * SBL_PAGE_BYTES), SEEK_SET) == 0
		&& fread(out, 1, SBL_PAGE_BYTES, b->file) == SBL_PAGE_BYTES;
}

#endif

static void sbl_free(struct server_world* w) {
	for(uint32_t i = 0; w->generation_light_pages && i < w->generation_light_slots; i++)
		if(w->generation_light_pages[i]) {
			mem2_free(w->generation_light_pages[i]);
			sbl_account(-(ptrdiff_t)SBL_PAGE_BYTES, -(ptrdiff_t)SBL_WII_MEM2_COST(SBL_PAGE_BYTES));
		}
	if(w->generation_light_pages)
		sbl_account(-(ptrdiff_t)(w->generation_light_slots * sizeof *w->generation_light_pages),
			-(ptrdiff_t)SBL_WII_MALLOC_COST(w->generation_light_slots * SBL_WII_PTR));
	free(w->generation_light_pages);
	struct server_beta_light_backing* b = w->generation_light_backing;
	if(b) {
		fclose(b->file);
		remove(b->path);
		sbl_account(-(ptrdiff_t)sizeof *b, -(ptrdiff_t)SBL_BACKING_WII);
		free(b);
	}
	w->generation_light_pages = NULL;
	w->generation_light_backing = NULL;
	w->generation_light_n = w->generation_light_base = 0;
	w->generation_light_slots = 0;
}

/* Checkpoint trial cursor: the resident window, n/base and the slot bits. */
struct sbl_cursor {
	size_t n, base, count;
	struct server_beta_light_region* window;
	uint8_t* loc;
	size_t bytes, wii; /* payload incl. the extra byte; PPC allocator cost */
};

static bool sbl_trial_begin(struct server_world* w, struct sbl_cursor* c) {
	memset(c, 0, sizeof *c);
	if(!w->generation_light_n) return true;
	if(!sbl_ensure_tail(w)) return false;
	c->n = w->generation_light_n;
	c->base = w->generation_light_base;
	c->count = c->n - c->base * SBL_PAGE;
	c->bytes = c->count * sizeof *c->window
		+ (w->generation_light_backing ? SBL_BITMAP_BYTES : 0);
	c->wii = SBL_WII_MALLOC_COST(c->count * sizeof *c->window + 1)
		+ (w->generation_light_backing ? SBL_WII_MALLOC_COST(SBL_BITMAP_BYTES) : 0);
	c->bytes += 1; /* the window allocation's extra byte (no malloc(0)) */
	c->window = malloc(c->count * sizeof *c->window + 1);
	if(!c->window) return false;
	for(size_t k = 0; k < c->count; k++) c->window[k] = *sbl_at(w, c->base * SBL_PAGE + k);
	if(w->generation_light_backing) {
		c->loc = malloc(SBL_BITMAP_BYTES);
		if(!c->loc) { free(c->window); memset(c, 0, sizeof *c); return false; }
		memcpy(c->loc, w->generation_light_backing->loc, SBL_BITMAP_BYTES);
		memset(w->generation_light_backing->flipped, 0, SBL_BITMAP_BYTES);
		w->generation_light_backing->trial = true;
	}
	sbl_account((ptrdiff_t)c->bytes, (ptrdiff_t)c->wii);
	if(c->bytes > s_sbl_ckpt_peak) s_sbl_ckpt_peak = c->bytes;
	return true;
}

/* End the trial's ownership (after a restore, or keeping the live list when
 * the trial drain stopped and the world keeps its progressed state). */
static void sbl_trial_release(struct server_world* w, struct sbl_cursor* c) {
	if(w->generation_light_backing) w->generation_light_backing->trial = false;
	sbl_account(-(ptrdiff_t)c->bytes, -(ptrdiff_t)c->wii);
	free(c->window);
	free(c->loc);
	memset(c, 0, sizeof *c);
}

/* Restore the list exactly as at sbl_trial_begin (no I/O: pages the trial
 * spilled went to the other physical slot). */
static void sbl_trial_restore(struct server_world* w, const struct sbl_cursor* c) {
	if(!c->n) return;
	w->generation_light_n = c->n;
	w->generation_light_base = c->base;
	for(size_t k = 0; k < c->count; k++) *sbl_at(w, c->base * SBL_PAGE + k) = c->window[k];
	if(c->loc) memcpy(w->generation_light_backing->loc, c->loc, SBL_BITMAP_BYTES);
}

static bool server_beta_light_merge(struct server_beta_light_region* m,
	struct server_beta_light_region a) {
	if(a.x1 >= m->x1 && a.y1 >= m->y1 && a.z1 >= m->z1
	   && a.x2 <= m->x2 && a.y2 <= m->y2 && a.z2 <= m->z2) return true;
	if(a.x1 < m->x1 - 1 || a.y1 < m->y1 - 1 || a.z1 < m->z1 - 1
	   || a.x2 > m->x2 + 1 || a.y2 > m->y2 + 1 || a.z2 > m->z2 + 1) return false;
	const int64_t old_volume = (int64_t)(m->x2 - m->x1)
		* (m->y2 - m->y1) * (m->z2 - m->z1);
	if(a.x1 > m->x1) a.x1 = m->x1;
	if(a.y1 > m->y1) a.y1 = m->y1;
	if(a.z1 > m->z1) a.z1 = m->z1;
	if(a.x2 < m->x2) a.x2 = m->x2;
	if(a.y2 < m->y2) a.y2 = m->y2;
	if(a.z2 < m->z2) a.z2 = m->z2;
	const int64_t new_volume = (int64_t)(a.x2 - a.x1)
		* (a.y2 - a.y1) * (a.z2 - a.z1);
	if(new_volume - old_volume > 2) return false;
	*m = a;
	return true;
}

/* All regular resident chunks have Chunk.func_21101_g()==false, even air.
 * EmptyChunk is the absent/dummy provider result and must not be admitted. */
/* resident: the caller has already proved the chunk holding the region's
 * midpoint resident (nether-drain-1030 T1: sbl_neighbor_in's one-cell
 * requests lie inside a chunk the relax resolved this drain, so the
 * residency probe Beta's World.java:1269 makes would answer true). */
static bool server_beta_light_schedule_ex(struct server_world* w, int channel,
	int x1, int y1, int z1, int x2, int y2, int z2, bool resident) {
	struct server_beta_light_region a = {channel, x1, y1, z1, x2, y2, z2};
	G9B_REGION(w, "region-request", a, w->generation_light_n);
	if(channel == COLUMN_SKY && w->dimension == WORLD_DIM_NETHER) {
		G9B_REGION(w, "region-reject-sky", a, w->generation_light_n);
		return true;
	}
	if(!resident) {
		const int mx = (int)(((int64_t)x1 + x2) / 2);
		const int mz = (int)(((int64_t)z1 + z2) / 2);
		if(!server_world_is_chunk_loaded(w, WCOORD_CHUNK_OFFSET(mx), WCOORD_CHUNK_OFFSET(mz))) {
			G9B_REGION(w, "region-reject-absent", a, w->generation_light_n);
			return true;
		}
	}
	/* The merge window must be resident (a rolled-back tail may not be). */
	if(!sbl_ensure_tail(w)) return false;
	const size_t n = w->generation_light_n;
	const size_t recent = n < SBL_TAIL ? n : SBL_TAIL;
	/* nether-drain-1030 (T1): the five newest entries share one page unless
	 * the tail straddles a page boundary; address that page once. */
	struct server_beta_light_region* tail = recent && (n - 1) % SBL_PAGE + 1 >= recent
		? sbl_at(w, n - 1) + 1 : NULL;
	for(size_t i = 0; i < recent; i++) {
		struct server_beta_light_region* m = tail ? tail - i - 1 : sbl_at(w, n - i - 1);
		if(m->channel == channel && server_beta_light_merge(m, a)) {
			G9B_REGION_MERGE(w, *m, n - i - 1);
			return true;
		}
	}
	/* Retain work and stop explicitly at resource limits; never copy Beta's
	 * million-region silent-clear behavior into a successful publication. */
	if(w->generation_light_n == SBL_MAX_REGIONS || !server_beta_light_reserve(w, 1)) return false;
	*sbl_at(w, w->generation_light_n++) = a;
	if(w->generation_light_n > w->generation_light_stat[2]) w->generation_light_stat[2] = w->generation_light_n;
	G9B_REGION(w, "region-admit", a, w->generation_light_n - 1);
	return true;
}

static bool server_beta_light_schedule(struct server_world* w, int channel,
	int x1, int y1, int z1, int x2, int y2, int z2) {
	return server_beta_light_schedule_ex(w, channel, x1, y1, z1, x2, y2, z2, false);
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* The reference relax's residency test (the shipping relax answers the same
 * question through sbl_hood_near below). */
static bool server_beta_light_near(struct server_world* w, int x, int z) {
	for(int cx = WCOORD_CHUNK_OFFSET(x - 1); cx <= WCOORD_CHUNK_OFFSET(x + 1); cx++)
		for(int cz = WCOORD_CHUNK_OFFSET(z - 1); cz <= WCOORD_CHUNK_OFFSET(z + 1); cz++)
			if(!server_world_is_chunk_loaded(w, cx, cz)) return false;
	return true;
}

/* G9X-7: the frozen pre-T1 relax (the body shipped through 7de203bb5,
 * unchanged but for the SBL_WORK census), kept test-only as the reference test_light_queue_equiv
 * g9x_drain_t1_equiv pairs the shipping relax against, plus a work census:
 * [0] (x,z) columns entered, [1] (x,y,z) cells relaxed, [2]/[3] residency
 * dict probes / resolve-cache lookups made while a drain relaxes. */
static bool s_sbl_reference;
static uint64_t s_sbl_work[4];
#define SBL_WORK(k) (s_sbl_work[k]++)
/* The reference relax's per-cell readers (the shipping relax reads the
 * same nibbles through its hoisted column chunks: sbl_light, sbl_neighbor_in). */
static int server_beta_light_saved(struct server_world* w, int channel,
	int x, int y, int z) {
	struct block_data b;
	if(y < 0) y = 0;
	if(y >= WORLD_HEIGHT) y = WORLD_HEIGHT - 1;
	if(!server_world_get_block(w, x, y, z, &b)) return 0;
	return channel == COLUMN_SKY ? b.sky_light : b.torch_light;
}

static bool server_beta_light_neighbor(struct server_world* w, int channel,
	int x, int y, int z, int value) {
	if(channel == COLUMN_SKY && w->dimension == WORLD_DIM_NETHER) return true;
	struct block_data b;
	uint8_t height;
	if(!server_world_light_get_block(w, x, y, z, &b, &height)) return true;
	if(channel == COLUMN_SKY) {
		if(y >= height) value = 15;
	} else if(blocks[b.type] && blocks[b.type]->luminance > value) value = blocks[b.type]->luminance;
	if(server_beta_light_saved(w, channel, x, y, z) == value) return true;
	return server_beta_light_schedule(w, channel, x, y, z, x, y, z);
}

static bool server_beta_light_relax_reference(struct server_world* w,
	struct server_beta_light_region m) {
	if((int64_t)(m.x2 - m.x1 + 1) * (m.y2 - m.y1 + 1) * (m.z2 - m.z1 + 1) > 32768) return true;
	for(int x = m.x1; x <= m.x2; x++)
		for(int z = m.z1; z <= m.z2; z++) {
			SBL_WORK(0);
			if(!server_beta_light_near(w, x, z)) continue;
			if(m.y1 < 0) m.y1 = 0;
			if(m.y2 >= WORLD_HEIGHT) m.y2 = WORLD_HEIGHT - 1;
			for(int y = m.y1; y <= m.y2; y++) {
				SBL_WORK(1);
				struct block_data b;
				uint8_t height;
				if(!server_world_light_get_block(w, x, y, z, &b, &height)) continue;
				int opacity = server_world_beta_light_opacity(b.type);
				if(!opacity) opacity = 1;
				const int source = m.channel == COLUMN_SKY ? (y >= height ? 15 : 0)
					: (blocks[b.type] ? blocks[b.type]->luminance : 0);
				int value = 0;
				if(opacity < 15 || source) {
					const int xyz[6][3] = {{x-1,y,z},{x+1,y,z},{x,y-1,z},{x,y+1,z},{x,y,z-1},{x,y,z+1}};
					for(unsigned k = 0; k < 6; k++) {
						int v = server_beta_light_saved(w, m.channel, xyz[k][0], xyz[k][1], xyz[k][2]);
						if(v > value) value = v;
					}
					value -= opacity;
					if(value < 0) value = 0;
					if(source > value) value = source;
				}
				const int old = m.channel == COLUMN_SKY ? b.sky_light : b.torch_light;
				if(old == value) continue;
				/* Reserve before the store and all six possible neighbor appends.
				 * One additional slot retains the popped region on failure. */
				if(!server_beta_light_reserve(w, 7)) return false;
				struct server_chunk* sc = server_world_resolve_chunk(w, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
				server_chunk_store(sc, m.channel, S_CHUNK_IDX(x,y,z), (uint8_t)value);
				server_world_chunk_touch(sc);
				const int next = value > 0 ? value - 1 : 0;
				if(!server_beta_light_neighbor(w,m.channel,x-1,y,z,next)
				   || !server_beta_light_neighbor(w,m.channel,x,y-1,z,next)
				   || !server_beta_light_neighbor(w,m.channel,x,y,z-1,next)
				   || (x+1 >= m.x2 && !server_beta_light_neighbor(w,m.channel,x+1,y,z,next))
				   || (y+1 >= m.y2 && !server_beta_light_neighbor(w,m.channel,x,y+1,z,next))
				   || (z+1 >= m.z2 && !server_beta_light_neighbor(w,m.channel,x,y,z+1,next))) return false;
			}
		}
	return true;
}

#else
#define SBL_WORK(k) ((void)0)
#endif

/* G9X-7 (T1): the relax with its chunk lookups hoisted out of the cells.
 * The same Beta steps in the same order [CFR MetadataChunkBlock.java:37-139:
 * the 32768 skip, x-major/z/y walk, per-(x,z) doChunksNearChunkExist (its
 * cache flag var8 is never set, :47-62, so it runs every column), the
 * clamps, opacity/source, six getSavedLightValue reads, setLightValue, then
 * neighborLightPropagationChanged x-1, y-1, z-1, x+1, y+1, z+1 with the
 * same >= edge tests], the same values, stores and queue requests. Only
 * where the bytes are fetched from changes: server_beta_light_near has
 * just proved the whole 3x3 chunk neighbourhood of (x,z) resident, and
 * nothing a relax does installs, evicts or moves a chunk (stores, touches
 * and queue appends only), so every read of the column comes from one of
 * five chunks resolved once per column -- its own, and x-1/x+1/z-1/z+1's
 * (the column's own unless x/z lies on a chunk edge) -- instead of 7 to 19
 * resolve-cache lookups per cell (server_world_light_get_block, six
 * server_beta_light_saved -> server_world_get_block, the store's resolve,
 * six neighbour get_block + saved). Each read is the same nibble those
 * paths return: a resident chunk, y in range, the saved-read y clamp kept.
 * Proved byte-for-byte (ordered queue/store/RNG events and every final
 * byte, both dimensions, checkpoint rollback, disk reload) against the
 * frozen body above by test_light_queue_equiv g9x_drain_t1_equiv. */
static inline int sbl_light(const struct server_chunk* sc, int channel, unsigned idx) {
	const uint8_t* p = channel == COLUMN_SKY ? sc->lighting_sky : sc->lighting_torch;
	return (p[idx / 2] >> ((idx % 2) * 4)) & 0xF;
}

/* server_beta_light_neighbor for (x,y,z) inside the resident chunk sc:
 * server_world_light_get_block can then fail only on the y range, and the
 * saved read of an in-range cell is that cell's stored nibble. */
static bool sbl_neighbor_in(struct server_world* w, const struct server_chunk* sc,
	int channel, int x, int y, int z, int value) {
	if(channel == COLUMN_SKY && w->dimension == WORLD_DIM_NETHER) return true;
	if(y < 0 || y >= WORLD_HEIGHT) return true;
	const unsigned idx = S_CHUNK_IDX(x, y, z);
	if(channel == COLUMN_SKY) {
		if(y >= sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE]) value = 15;
	} else {
		const uint8_t t = sc->ids[idx];
		if(blocks[t] && blocks[t]->luminance > value) value = blocks[t]->luminance;
	}
	if(sbl_light(sc, channel, idx) == value) return true;
	/* sc is resident, so the one-cell region's midpoint chunk is: skip the probe */
	return server_beta_light_schedule_ex(w, channel, x, y, z, x, y, z, true);
}

/* nether-drain-1030 (T1): the chunk neighbourhood a relax column needs,
 * resolved once per chunk per drain instead of once per column. Beta's
 * doChunksNearChunkExist asks the residency set for every chunk that
 * x-1..x+1 / z-1..z+1 touch (1, 2 or 4 chunks); the answer is the same
 * through the Tier-0 resolve cache (server_world_chunk_resident), and it
 * cannot change while a drain runs: a relax installs, evicts and moves no
 * chunk (stores, touches and queue appends only), the residency dict is the
 * server thread's own, and the drain is that thread. The memo is dropped at
 * every point where control leaves the drain (start, yield, stop, end), so
 * a pointer it holds never outlives the dict layout it came from. */
struct sbl_hood {
	bool valid;
	w_coord_t cx, cz;
	uint16_t known;                 /* bit (dx+1)*3+(dz+1): c[] resolved */
	struct server_chunk* c[9];      /* [(dx+1)*3+(dz+1)], NULL = absent */
};
static struct sbl_hood s_sbl_hood;
static inline void sbl_hood_reset(void) { s_sbl_hood.valid = false; }

static inline struct server_chunk* sbl_hood_chunk(struct server_world* w,
	w_coord_t cx, w_coord_t cz, int dx, int dz) {
	struct sbl_hood* h = &s_sbl_hood;
	if(!h->valid || h->cx != cx || h->cz != cz) {
		h->valid = true;
		h->cx = cx;
		h->cz = cz;
		h->known = 0;
	}
	const unsigned k = (unsigned)(dx + 1) * 3u + (unsigned)(dz + 1);
	if(!(h->known & (1u << k))) {
		h->c[k] = server_world_resolve_chunk(w, cx + dx, cz + dz);
		h->known |= (uint16_t)(1u << k);
	}
	return h->c[k];
}

/* server_beta_light_near(w, x, z) through the memo: the same chunks asked,
 * in the same order, the same answer. */
static bool sbl_hood_near(struct server_world* w, w_coord_t cx, w_coord_t cz,
	int x, int z) {
	const int dx0 = WCOORD_CHUNK_OFFSET(x - 1) - cx, dx1 = WCOORD_CHUNK_OFFSET(x + 1) - cx;
	const int dz0 = WCOORD_CHUNK_OFFSET(z - 1) - cz, dz1 = WCOORD_CHUNK_OFFSET(z + 1) - cz;
	for(int dx = dx0; dx <= dx1; dx++)
		for(int dz = dz0; dz <= dz1; dz++)
			if(!sbl_hood_chunk(w, cx, cz, dx, dz)) return false;
	return true;
}

static bool server_beta_light_relax(struct server_world* w,
	struct server_beta_light_region m) {
	if((int64_t)(m.x2 - m.x1 + 1) * (m.y2 - m.y1 + 1) * (m.z2 - m.z1 + 1) > 32768) return true;
	const int ch = m.channel;
	for(int x = m.x1; x <= m.x2; x++)
		for(int z = m.z1; z <= m.z2; z++) {
			SBL_WORK(0);
			const w_coord_t cx = WCOORD_CHUNK_OFFSET(x), cz = WCOORD_CHUNK_OFFSET(z);
			if(!sbl_hood_near(w, cx, cz, x, z)) continue;
			if(m.y1 < 0) m.y1 = 0;
			if(m.y2 >= WORLD_HEIGHT) m.y2 = WORLD_HEIGHT - 1;
			struct server_chunk* sc = sbl_hood_chunk(w, cx, cz, 0, 0);
			const struct server_chunk* xm = W2C_COORD(x) == 0
				? sbl_hood_chunk(w, cx, cz, -1, 0) : sc;
			const struct server_chunk* xp = W2C_COORD(x) == CHUNK_SIZE - 1
				? sbl_hood_chunk(w, cx, cz, 1, 0) : sc;
			const struct server_chunk* zm = W2C_COORD(z) == 0
				? sbl_hood_chunk(w, cx, cz, 0, -1) : sc;
			const struct server_chunk* zp = W2C_COORD(z) == CHUNK_SIZE - 1
				? sbl_hood_chunk(w, cx, cz, 0, 1) : sc;
			assert(sc && xm && xp && zm && zp && "near() proved the 3x3 resident");
			const unsigned col = S_CHUNK_IDX(x, 0, z);
			const unsigned col_xm = S_CHUNK_IDX(x - 1, 0, z), col_xp = S_CHUNK_IDX(x + 1, 0, z);
			const unsigned col_zm = S_CHUNK_IDX(x, 0, z - 1), col_zp = S_CHUNK_IDX(x, 0, z + 1);
			const int height = sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE];
			for(int y = m.y1; y <= m.y2; y++) {
				SBL_WORK(1);
				const unsigned idx = col + (unsigned)y;
				const uint8_t type = sc->ids[idx];
				int opacity = server_world_beta_light_opacity(type);
				if(!opacity) opacity = 1;
				const int source = ch == COLUMN_SKY ? (y >= height ? 15 : 0)
					: (blocks[type] ? blocks[type]->luminance : 0);
				int value = 0;
				if(opacity < 15 || source) {
					/* server_beta_light_saved clamps y into the column */
					const unsigned yd = y > 0 ? (unsigned)y - 1 : 0;
					const unsigned yu = y < WORLD_HEIGHT - 1 ? (unsigned)y + 1 : WORLD_HEIGHT - 1;
					const int v[6] = {sbl_light(xm, ch, col_xm + (unsigned)y),
						sbl_light(xp, ch, col_xp + (unsigned)y), sbl_light(sc, ch, col + yd),
						sbl_light(sc, ch, col + yu), sbl_light(zm, ch, col_zm + (unsigned)y),
						sbl_light(zp, ch, col_zp + (unsigned)y)};
					for(unsigned k = 0; k < 6; k++)
						if(v[k] > value) value = v[k];
					value -= opacity;
					if(value < 0) value = 0;
					if(source > value) value = source;
				}
				if(sbl_light(sc, ch, idx) == value) continue;
				/* Reserve before the store and all six possible neighbor appends.
				 * One additional slot retains the popped region on failure. */
				if(!server_beta_light_reserve(w, 7)) return false;
				server_chunk_store(sc, ch, idx, (uint8_t)value);
				server_world_chunk_touch(sc);
				const int next = value > 0 ? value - 1 : 0;
				if(!sbl_neighbor_in(w, xm, ch, x - 1, y, z, next)
				   || !sbl_neighbor_in(w, sc, ch, x, y - 1, z, next)
				   || !sbl_neighbor_in(w, zm, ch, x, y, z - 1, next)
				   || (x + 1 >= m.x2 && !sbl_neighbor_in(w, xp, ch, x + 1, y, z, next))
				   || (y + 1 >= m.y2 && !sbl_neighbor_in(w, sc, ch, x, y + 1, z, next))
				   || (z + 1 >= m.z2 && !sbl_neighbor_in(w, zp, ch, x, y, z + 1, next))) return false;
			}
		}
	return true;
}

static bool server_beta_light_drain(struct server_world* w) {
	size_t regions = 0;
	sbl_hood_reset();
	while(w->generation_light_n) {
		/* load a spilled page only when the unchanged LIFO reaches it */
		if(!sbl_ensure_tail(w)) {
			sbl_hood_reset();
			server_world_light_stop(w, SERVER_LIGHT_FAULT_DRAIN_OOM, regions);
			return false;
		}
		struct server_beta_light_region m = *sbl_at(w, --w->generation_light_n);
		G9B_REGION(w, "region-pop", m, w->generation_light_n + 1);
#ifdef CAVEWII_SERVER_LOCAL_TEST
		const uint64_t probes0 = s_sbl_dict_probes, resolves0 = s_sbl_resolves;
		const bool relaxed = s_sbl_reference ? server_beta_light_relax_reference(w, m)
			: server_beta_light_relax(w, m);
		s_sbl_work[2] += s_sbl_dict_probes - probes0;
		s_sbl_work[3] += s_sbl_resolves - resolves0;
#else
		const bool relaxed = server_beta_light_relax(w, m);
#endif
		if(!relaxed) {
			/* The popped region remains pending even if its preceding cells
			 * progressed. A resource failure is terminal to all consumers. */
			*sbl_at(w, w->generation_light_n++) = m;
			sbl_hood_reset();
			server_world_light_stop(w, SERVER_LIGHT_FAULT_DRAIN_OOM, regions);
			return false;
		}
		if(++regions % 499 == 0) {
			G9B_REGION(w, "region-yield", m, w->generation_light_n);
			sbl_hood_reset(); /* control leaves the drain: re-resolve after */
			server_world_light_yield();
		}
	}
	sbl_hood_reset();
	return true;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
void server_world_test_beta_light_reference(bool on) { s_sbl_reference = on; }
void server_world_test_beta_light_work(uint64_t out[4], bool reset) {
	for(unsigned k = 0; k < 4; k++) {
		if(out) out[k] = s_sbl_work[k];
		if(reset) s_sbl_work[k] = 0;
	}
}
#endif
