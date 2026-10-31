/* Execute both REAL backends on PC, with a delayed GPU and poisoned frees.
 * The fake keeps a byte snapshot of every submitted read. Completion checks
 * the original allocation is live and unchanged BEFORE it permits retirement.
 * Freed allocations stay poisoned/quarantined until the test exits. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#ifdef _WIN32
#include <malloc.h>
#endif

#define MODEL_ALLOCS 32768
static struct allocation {
	void* p;
	size_t bytes;
	bool freed, aligned;
} s_alloc[MODEL_ALLOCS];
static unsigned s_alloc_count, s_deleted_vbos, s_generated_vbos;
static bool s_fail_mem2;
static pthread_t s_main;
static uint16_t s_raw, s_last_seal;
static uint32_t s_fake_completed;
static unsigned s_waits, s_seals, s_reads;
static bool s_auto_complete;
static struct gpu_read {
	void* p;
	size_t bytes;
	uint8_t* expected;
	uint32_t stamp;
} s_reads_pending[4096];
static unsigned s_read_count;

static struct allocation* allocation_of(void* p) {
	for(unsigned k = 0; k < s_alloc_count; k++) {
		uintptr_t off = (uintptr_t)p - (uintptr_t)s_alloc[k].p;
		if(off < s_alloc[k].bytes) return &s_alloc[k];
	}
	assert(!"unknown allocation");
	return NULL;
}
static void* model_alloc(size_t bytes, bool aligned) {
	assert(s_alloc_count < MODEL_ALLOCS);
	void* p;
#ifdef _WIN32
	p = aligned ? _aligned_malloc(bytes, 32) : malloc(bytes);
#else
	if(aligned) { if(posix_memalign(&p, 32, bytes)) p = NULL; }
	else p = malloc(bytes);
#endif
	assert(p);
	memset(p, 0xA5, bytes);
	s_alloc[s_alloc_count++] = (struct allocation){p, bytes, false, aligned};
	return p;
}
static void* model_malloc(size_t bytes) {
	return model_alloc(bytes, false);
}
void* mem2_alloc(size_t bytes) {
	if(s_fail_mem2) { s_fail_mem2 = false; return NULL; }
	return model_alloc(bytes, true);
}
static void model_free(void* p) {
	if(!p) return;
	struct allocation* a = allocation_of(p);
	assert(a->p == p && !a->freed);
	a->freed = true;
	memset(p, 0xDD, a->bytes);
}
static void* model_realloc(void* p, size_t bytes) {
	if(!p) return model_malloc(bytes);
	struct allocation* a = allocation_of(p);
	assert(!a->freed);
	void* q = model_alloc(bytes, a->aligned);
	memcpy(q, p, bytes < a->bytes ? bytes : a->bytes);
	model_free(p);
	return q;
}
static bool freed(void* p) { return allocation_of(p)->freed; }
static void queue_gpu_read(void* p, size_t bytes, uint32_t stamp) {
	assert(s_read_count < 4096 && !freed(p));
	uint8_t* expected = malloc(bytes);
	assert(expected);
	memcpy(expected, p, bytes);
	s_reads_pending[s_read_count++] = (struct gpu_read){p, bytes, expected, stamp};
}
static void fake_complete(uint16_t token) {
	uint32_t completed = s_fake_completed + (uint16_t)(token - s_raw);
	for(unsigned k = 0; k < s_read_count;) {
		struct gpu_read r = s_reads_pending[k];
		if(r.stamp > completed) { k++; continue; }
		assert(!freed(r.p));
		assert(!memcmp(r.expected, r.p, r.bytes));
		free(r.expected);
		s_reads++;
		s_reads_pending[k] = s_reads_pending[--s_read_count];
	}
	s_fake_completed = completed;
	s_raw = token;
}
static uint16_t fake_read(void) { return s_raw; }
static bool fake_is_main(void) { return pthread_equal(pthread_self(), s_main); }
#ifdef CAVEWII_RETIRE_WII_MODEL
static void real_wait(uint32_t token);
#endif
static void fake_seal(uint16_t token) {
	assert(fake_is_main());
	s_seals++;
	s_last_seal = token;
	if(s_auto_complete) fake_complete(token);
}
static void fake_wait(uint32_t token) {
	assert(fake_is_main());
	assert((uint16_t)(s_last_seal - token) < 0x8000);
	s_waits++;
#ifdef CAVEWII_RETIRE_WII_MODEL
	real_wait(token);
#else
	fake_complete((uint16_t)token);
#endif
}

/* Macro substitution affects allocator calls in the real backend, never the
 * fake's snapshots/quarantine cleanup. No special engine compile profile. */
#define malloc model_malloc
#define realloc model_realloc
#define free model_free
/* Compile the real lifetime guard as RELEASE code; test assertions below
 * stay active, so wrong-size/double release cannot hide behind assert(). */
#define NDEBUG
#ifdef CAVEWII_RETIRE_WII_MODEL
#undef PLATFORM_PC
#define PLATFORM_WII
#include <gccore.h>
void mem2_free_sized(void* p, size_t bytes) {
	assert(!p || allocation_of(p)->bytes == bytes);
	model_free(p);
}
void* mem2_realloc(void* p, size_t old, size_t bytes) {
	assert(allocation_of(p)->bytes == old);
	return model_realloc(p, bytes);
}
void* mem2_shrink(void* p, size_t old, size_t bytes) {
	(void)p; (void)old; (void)bytes;
	return NULL;
}
#include "../source/platform/wii/displaylist.c"
carve_callback mem2_carve_observe;
#else
#define PLATFORM_PC
#include <GL/glew.h>
/* GL state calls are a sink; submission still executes the actual backend.
 * Deferred CPU/VBO release is checked separately by the fake GPU above. */
static void fake_gen(GLsizei n, int* ids) {
	static int id;
	s_generated_vbos += n;
	while(n--) *ids++ = ++id;
}
static void fake_delete(GLsizei n, const int* ids) {
	assert(fake_is_main());
	(void)ids; s_deleted_vbos += n;
}
static void fake_bind(GLenum target, GLuint id) { (void)target; (void)id; }
static void fake_data(GLenum t, GLsizeiptr n, const void* p, GLenum u) {
	(void)t; (void)n; (void)p; (void)u;
}
static void fake_attrib(GLuint i) { (void)i; }
static void fake_pointer(GLuint i, GLint n, GLenum t, GLboolean b, GLsizei s,
						 const void* p) {
	(void)i; (void)n; (void)t; (void)b; (void)s; (void)p;
}
static void fake_white(GLuint i, GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
	(void)i; (void)r; (void)g; (void)b; (void)a;
}
static void fake_draw(GLenum m, GLint i, GLsizei n) { (void)m; (void)i; (void)n; }
#undef glGenBuffers
#undef glDeleteBuffers
#undef glBindBuffer
#undef glBufferData
#undef glEnableVertexAttribArray
#undef glDisableVertexAttribArray
#undef glVertexAttribPointer
#undef glVertexAttrib4f
#define glGenBuffers fake_gen
#define glDeleteBuffers fake_delete
#define glBindBuffer fake_bind
#define glBufferData fake_data
#define glEnableVertexAttribArray fake_attrib
#define glDisableVertexAttribArray fake_attrib
#define glVertexAttribPointer fake_pointer
#define glVertexAttrib4f fake_white
#define glDrawArrays fake_draw
#include "../source/platform/pc/displaylist.c"
#endif
#undef malloc
#undef realloc
#undef free
#undef NDEBUG
#include <assert.h>
static unsigned batch_allocations(void) {
	unsigned count = 0;
	for(unsigned k = 0; k < s_alloc_count; k++)
		if(s_alloc[k].bytes == sizeof(struct dl_retire_batch)) {
			assert(s_alloc[k].aligned && !s_alloc[k].freed);
			count++;
		}
	return count;
}

/* Compile the actual production bounded wait, including its MAIN hook,
 * flush/poll/timeout/abort composition. Only unrelated GX/VI rendering is
 * excluded by the seam in gfx.c; there is no duplicate wait algorithm. */
#ifdef CAVEWII_RETIRE_WII_MODEL
#include "../source/platform/time.h"
typedef struct { uint32_t unused; } GXTexObj; /* no textures in wait seam */
#include "../source/platform/gfx.h"
#include "../source/platform/frame_wait_timing.h"
typedef uint32_t lwp_t;
static lwp_t LWP_GetSelf(void) { return fake_is_main() ? 1 : 2; }
static unsigned s_flushes, s_aborts;
static bool s_stall;
static uint64_t s_clock_ms;
static bool s_wait_timing_enabled;
static struct gfx_wait_timing s_wait_timing;
static volatile uint32_t g_gp_reset_count;
ptime_t time_get(void) { s_clock_ms += 250; return s_clock_ms; }
float time_diff_s(ptime_t from, ptime_t to) { return (float)(to - from) / 1000; }
static uint64_t gettime(void) { return s_clock_ms * 1000; }
static uint64_t ticks_to_microsecs(uint64_t ticks) { return ticks; }
static uint16_t GX_GetDrawSync(void) { return fake_read(); }
static void GX_SetDrawSync(uint16_t token) { fake_seal(token); }
static void GX_Flush(void) {
	assert(fake_is_main());
	s_flushes++;
	if(!s_stall) fake_complete(s_last_seal);
}
static void GX_AbortFrame(void) {
	assert(fake_is_main() && s_gpu_aborting && s_flushes);
	for(unsigned k = 0; k < s_read_count; k++) {
		assert(!freed(s_reads_pending[k].p));
		free(s_reads_pending[k].expected);
	}
	s_read_count = 0;
	s_raw = 0; /* PI abort resetting PE_TOKEN is the wrap regression */
	s_fake_completed = s_gpu_issued + (s_gpu_open ? 1u : 0u);
	s_fake_completed += (uint16_t)(s_raw - (uint16_t)s_fake_completed);
	s_aborts++;
}
#define CAVEWII_RETIRE_GFX_TEST
#include "../source/platform/wii/gfx.c"
static void real_wait(uint32_t token) { gfx_wait_token_bounded(token); }
#endif

static void setup(uint16_t initial) {
	assert(!displaylist_gpu_pending() && !s_read_count);
	uint32_t lists, bytes;
	displaylist_storage_bytes(&lists, &bytes);
	assert(!lists && !bytes);
	s_raw = s_last_seal = initial;
	s_fake_completed = initial;
	assert(s_generated_vbos == s_deleted_vbos);
	s_waits = s_seals = s_reads = s_deleted_vbos = s_generated_vbos = 0;
	s_auto_complete = false;
#ifdef CAVEWII_RETIRE_WII_MODEL
	s_gpu_main_thread = LWP_GetSelf();
	s_flushes = s_aborts = g_gp_reset_count = 0;
	s_stall = false; s_clock_ms = 0;
	const struct displaylist_gpu_hooks hooks = {
		gfx_gpu_completed, gfx_gpu_seal, fake_wait, gfx_gpu_is_main
	};
#else
	const struct displaylist_gpu_hooks hooks = {
		fake_read, fake_seal, fake_wait, fake_is_main
	};
#endif
	displaylist_gpu_configure(&hooks, initial);
}
static void build(struct displaylist* l, unsigned seed) {
	displaylist_init(l, 4, 9);
	displaylist_allow_scratch(l);
	for(unsigned k = 0; k < 4; k++)
		displaylist_vertex(l, k + seed, k * 2, -k, seed, k, k + 1);
	displaylist_finalize(l, 4);
}
static void submit(struct displaylist* l) {
	displaylist_render(l);
	assert(l->submitted);
#ifdef CAVEWII_RETIRE_WII_MODEL
	assert(gx_call_ptr && gx_call_bytes);
	queue_gpu_read(gx_call_ptr, gx_call_bytes, l->last_submit);
#else
	queue_gpu_read(l->data, l->index * (l->tinted ? 25 : 22), l->last_submit);
#endif
}
static void complete_frame(void) {
	uint32_t token = displaylist_gpu_seal();
	fake_complete((uint16_t)token);
	displaylist_gpu_reap();
}
static void delayed_and_repeated(void) {
	setup(0);
	struct displaylist l;
	build(&l, 7); submit(&l);
	void* p = l.data;
	displaylist_destroy(&l);
	assert(!l.data && !l.index && !l.finished && !freed(p));
	displaylist_destroy(&l);
	displaylist_render(&l); /* stale has_displist must not read poison */
	assert(displaylist_gpu_pending() == 1);
	uint32_t lists, bytes;
	displaylist_storage_bytes(&lists, &bytes);
	assert(lists == 1 && bytes);
	displaylist_gpu_reap();
	assert(!freed(p) && !s_deleted_vbos);
	complete_frame();
	assert(freed(p) && s_reads == 1 && !displaylist_gpu_pending());
	displaylist_storage_bytes(&lists, &bytes);
	assert(!lists && !bytes);
#ifndef CAVEWII_RETIRE_WII_MODEL
	assert(s_deleted_vbos == 1);
#endif
	puts("PASS delayed completion, accounting, repeated destroy, poisoned stale draw");
}
static void wrap_and_old_resident(void) {
	setup(65534);
	struct displaylist a, b;
	build(&a, 1); submit(&a);
	assert(displaylist_gpu_seal() == 65535);
	fake_complete(65535); displaylist_gpu_reap();
	build(&b, 2); submit(&b);
	assert(displaylist_gpu_seal() == 65536);
	void* p = b.data;
	displaylist_destroy(&b); displaylist_gpu_reap();
	assert(!freed(p));
	fake_complete(0); displaylist_gpu_reap();
	assert(freed(p));
	/* A resident list's last use can be older than the 16-bit half range. */
	s_auto_complete = true;
	for(unsigned k = 0; k < 40000; k++) displaylist_gpu_seal();
	p = a.data;
	displaylist_destroy(&a);
	assert(freed(p));
	puts("PASS token 65535 -> 0 and old resident beyond 32768 seals");
}
static void abort_quiescence(void) {
	setup(0);
	struct displaylist a, b;
	build(&a, 1); submit(&a); /* deliberately unsealed */
	void* p = a.data;
	displaylist_gpu_abort_begin();
	displaylist_destroy(&a); displaylist_gpu_reap();
	assert(!freed(p) && displaylist_gpu_pending() == 1);
	/* The abort witness cancels GPU reads, it does not pretend they ran. */
	for(unsigned k = 0; k < s_read_count; k++) free(s_reads_pending[k].expected);
	s_read_count = 0;
	/* Match the post-abort epoch used by the production implementation. */
	s_raw = 0; s_fake_completed = 65536;
	displaylist_gpu_abort_complete();
	assert(freed(p) && !displaylist_gpu_pending() && !s_reads);
	/* PE still reports pre-abort zero; it cannot retire a post-abort draw. */
	build(&b, 2); submit(&b);
	p = b.data;
	displaylist_destroy(&b); displaylist_gpu_reap();
	assert(!freed(p));
	assert(displaylist_gpu_seal() == 65537);
	fake_complete(1); displaylist_gpu_reap();
	assert(freed(p));
	puts("PASS abort requires quiescence, skips unsealed token, rejects stale PE");
}
static void pre_abort_token_after_rebase(void) {
	setup(98);
	displaylist_gpu_seal();
	uint32_t token = displaylist_gpu_seal();
	assert(token == 100 && !displaylist_gpu_token_complete(token));
	displaylist_gpu_abort_begin();
	assert(!displaylist_gpu_token_complete(token));
	/* Quiescent PE still reads 98; rebase jumps past the old token 100. */
	displaylist_gpu_abort_complete();
	assert(s_gpu_completed == 65634 && displaylist_gpu_token_complete(token));
	assert(!displaylist_gpu_token_complete(displaylist_gpu_seal()));
#ifdef CAVEWII_RETIRE_WII_MODEL
	s_stall = true;
	real_wait(token);
	assert(!s_aborts && s_flushes == 1);
	s_stall = false;
#endif
	fake_complete(s_last_seal); displaylist_gpu_reap();
	puts("PASS extended pre-abort token 100 completes after PE=98 rebase, no second abort");
}
static void exhaustion(void) {
	setup(0);
	struct displaylist l;
	for(unsigned k = 0; k < DISPLAYLIST_RETIRE_CAPACITY; k++) {
		build(&l, k); submit(&l); displaylist_destroy(&l);
	}
	assert(displaylist_gpu_pending() == DISPLAYLIST_RETIRE_CAPACITY);
	build(&l, 99); submit(&l);
	void* p = l.data;
	s_fail_mem2 = true; /* actual OOM, not a full batch, may wait */
	displaylist_destroy(&l); /* must seal the open frame BEFORE waiting */
	assert(s_waits == 1 && s_seals == 1 && freed(p));
	assert(s_reads == DISPLAYLIST_RETIRE_CAPACITY + 1);
#ifdef CAVEWII_RETIRE_WII_MODEL
	assert(s_flushes == 1 && !s_aborts);
#endif
	puts("PASS batch allocation failure mid-frame seals/flushes/waits safely");
}

static void split_unpacked_batches(void) {
	setup(0);
	displaylist_set_block_mode(false); /* memory.mesh_blocks=0 */
	struct displaylist l;
	/* Demote 78 unpacked generations (26 lists each) between split passes:
	 * seven full overflow boundaries, with NO token wait/abort on capacity. */
	const unsigned count = 78 * 26;
	unsigned allocations = batch_allocations();
	for(unsigned burst = 0; burst < 3; burst++) {
		for(unsigned k = 0; k < count; k++) {
			build(&l, k); submit(&l); displaylist_destroy(&l);
		}
		assert(displaylist_gpu_pending() == count && !s_waits);
		assert(s_retired.next);
		complete_frame();
		assert(s_reads == count * (burst + 1) && !displaylist_gpu_pending()
			   && !s_retired.next && s_retire_tail == &s_retired);
		assert(batch_allocations() == allocations + 7);
	}
	puts("PASS three 2028-list bursts reuse 7 MEM2 batches, no further allocations/waits");
}
static void reuse_drained_head(void) {
	setup(0);
	struct displaylist l;
	for(unsigned wave = 0; wave < 2; wave++) {
		for(unsigned k = 0; k < DISPLAYLIST_RETIRE_CAPACITY; k++) {
			build(&l, k); submit(&l); displaylist_destroy(&l);
		}
		displaylist_gpu_seal();
	}
	struct dl_retire_batch* tail = s_retire_tail;
	unsigned allocations = batch_allocations();
	fake_complete(1); displaylist_gpu_reap();
	assert(!s_retired.count && tail->count == DISPLAYLIST_RETIRE_CAPACITY);
	for(unsigned k = 0; k < DISPLAYLIST_RETIRE_CAPACITY; k++) {
		build(&l, k); submit(&l); displaylist_destroy(&l);
	}
	assert(s_retire_tail == tail && s_retired.count == DISPLAYLIST_RETIRE_CAPACITY
		   && batch_allocations() == allocations && !s_waits);
	complete_frame();
	puts("PASS drained static head reused while overflow tail remains GPU-pending");
}

#ifdef CAVEWII_RETIRE_WII_MODEL
static void timeout_abort_wrap(void) {
	setup(65534);
	struct displaylist a, b;
	build(&a, 1); submit(&a);
	void* old = a.data;
	displaylist_destroy(&a);
	uint32_t token = displaylist_gpu_seal();
	assert(token == 65535 && !freed(old));
	s_stall = true;
	fake_wait(token); /* real timeout -> begin -> GX abort -> complete -> reap */
	assert(s_flushes == 1 && s_aborts == 1 && g_gp_reset_count == 1);
	assert(freed(old) && !s_read_count && s_raw == 0);
	assert(displaylist_gpu_token_complete(token));
	build(&b, 2); submit(&b);
	void* fresh = b.data;
	displaylist_destroy(&b);
	uint32_t fresh_token = displaylist_gpu_seal();
	displaylist_gpu_reap();
	assert(!freed(fresh)); /* reset zero is NOT post-abort completion */
	assert(fresh_token == 65537); /* extended stamp retains the new epoch */
	s_stall = false;
	fake_wait(fresh_token);
	assert(freed(fresh) && s_reads == 1 && s_flushes == 2 && s_aborts == 1);
	puts("PASS production gfx timeout/abort/PE reset at 65535, post-abort fence");
}
#endif

static void main_only_draw(void) {
	setup(0);
	struct displaylist l;
	build(&l, 1);
	dl_lock(); /* A per-draw lock would deadlock this actual backend draw. */
	submit(&l);
	dl_unlock();
	displaylist_destroy(&l);
	complete_frame();
	puts("PASS MAIN-only draw stamp does not acquire retirement metadata lock");
}
static void token_window_backpressure(void) {
	setup(0);
	struct displaylist l;
	build(&l, 1); submit(&l);
	void* p = l.data;
	displaylist_destroy(&l);
	for(unsigned k = 0; k < 0x7fff; k++) displaylist_gpu_seal();
	assert(!freed(p) && !s_waits);
	assert(displaylist_gpu_seal() == 0x8000);
	assert(s_waits == 1 && freed(p));
	fake_complete(0x8000); displaylist_gpu_reap();
	puts("PASS token window backpressure before half-range ambiguity");
}
static struct displaylist s_worker_list;
static uint32_t s_worker_started, s_worker_done;
static void* worker_destroy(void* ignored) {
	(void)ignored;
	__atomic_store_n(&s_worker_started, 1, __ATOMIC_RELEASE);
	displaylist_destroy(&s_worker_list);
	__atomic_store_n(&s_worker_done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void worker_unsubmitted(void) {
	setup(0);
	/* Production Wii workers destroy only unsubmitted, unpublished lists. */
	build(&s_worker_list, 3);
	void* p = s_worker_list.data;
	pthread_t worker;
	s_worker_started = s_worker_done = 0;
	assert(!pthread_create(&worker, NULL, worker_destroy, NULL));
	while(!__atomic_load_n(&s_worker_started, __ATOMIC_ACQUIRE)) usleep(200);
	assert(!pthread_join(worker, NULL));
	displaylist_gpu_reap();
	assert(freed(p) && !s_waits && !s_seals && !s_gpu_open && !displaylist_gpu_pending());
	puts("PASS production worker destroys only unsubmitted storage, no GX hooks");
}
static void unsubmitted_and_reset(void) {
	setup(0);
	struct displaylist l;
	build(&l, 1);
	void* p = l.data;
	displaylist_destroy(&l); displaylist_destroy(&l);
	assert(freed(p) && !displaylist_gpu_pending() && !s_seals);
	displaylist_init(&l, 4, 9);
	displaylist_vertex(&l, 1, 2, 3, 4, 5, 6);
	p = l.data;
	displaylist_reset(&l);
	assert(l.data == p && !freed(p) && !l.submitted);
	displaylist_destroy(&l);
	assert(freed(p));
	puts("PASS never-submitted destroy and CPU-stream reset");
}
static void shared_and_split(void) {
	setup(0);
	struct displaylist lists[2];
	bool has[] = {true, true};
	displaylist_set_block_mode(true);
	displaylist_scratch_begin();
	build(&lists[0], 1); build(&lists[1], 2);
	void* block; size_t bytes;
	assert(displaylist_pack_block(lists, has, 2, &block, &bytes) && block);
	displaylist_scratch_end();
	submit(&lists[0]);
	uint32_t pass1 = lists[0].last_submit;
	submit(&lists[0]); submit(&lists[1]);
	assert(lists[0].last_submit == pass1 && lists[1].last_submit == pass1);
	displaylist_destroy(&lists[0]);
	displaylist_block_free(block, bytes); /* early owner release: member still live */
	assert(!freed(block));
	complete_frame();
	assert(!freed(block));
	uint32_t resident_lists, resident_bytes;
	displaylist_storage_bytes(&resident_lists, &resident_bytes);
	assert(resident_lists == 2 && resident_bytes == bytes);
	submit(&lists[1]); /* shared member is used again in a later frame */
	displaylist_destroy(&lists[1]);
	displaylist_gpu_reap(); assert(!freed(block));
	complete_frame();
	assert(freed(block) && s_reads == 4 && !displaylist_gpu_pending());
	displaylist_set_block_mode(false);
	puts("PASS shared backing waits for last member/use and two split passes");
}
static size_t s_final_bytes;
static void observe_final(enum carve_action action, uintptr_t p, uintptr_t q,
						  size_t bytes, size_t aux) {
	(void)p; (void)q; (void)aux;
	if(action == CARVE_FINAL) s_final_bytes = bytes;
}
static void bad_backing_free(void) {
	setup(0);
	struct displaylist l;
	bool has = true;
	displaylist_set_block_mode(true);
	displaylist_scratch_begin();
	build(&l, 1);
	void* block; size_t bytes;
	displaylist_carve_observe = observe_final;
	assert(displaylist_pack_block(&l, &has, 1, &block, &bytes) && block);
	displaylist_carve_observe = NULL;
	displaylist_scratch_end();
	size_t allocated = displaylist_block_allocation_bytes(bytes);
	assert(allocation_of(block)->bytes == allocated);
#ifdef CAVEWII_RETIRE_WII_MODEL
	assert(s_final_bytes == allocated);
#endif
	uint8_t* snapshot = malloc(allocated);
	memcpy(snapshot, block, allocated);
	uint32_t bad = displaylist_block_bad_free_count();
	/* The deliberately wrong record lies inside the stream allocation. */
	displaylist_block_free(block, bytes - 32);
	assert(displaylist_block_bad_free_count() == bad + 1 && !freed(block));
	assert(!memcmp(snapshot, block, allocated));
	displaylist_block_free(block, bytes);
	memcpy(snapshot, block, allocated);
	displaylist_block_free(block, bytes); /* owner repeated, member still live */
	assert(displaylist_block_bad_free_count() == bad + 2 && !freed(block));
	assert(!memcmp(snapshot, block, allocated));
	displaylist_destroy(&l);
	assert(freed(block));
	memcpy(snapshot, block, allocated); /* quarantined poison after real release */
	displaylist_block_free(block, bytes);
	assert(displaylist_block_bad_free_count() == bad + 3 && !memcmp(snapshot, block, allocated));
	free(snapshot);
	displaylist_set_block_mode(false);
	printf("PASS RELEASE wrong-size/double slab free refused+counted without writes; bytes %zu -> %zu\n",
		bytes, allocated);
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	s_main = pthread_self();
	delayed_and_repeated();
	wrap_and_old_resident();
	abort_quiescence();
	pre_abort_token_after_rebase();
	exhaustion();
	split_unpacked_batches();
	reuse_drained_head();
#ifdef CAVEWII_RETIRE_WII_MODEL
	timeout_abort_wrap();
#endif
	main_only_draw();
	token_window_backpressure();
	worker_unsubmitted();
	unsubmitted_and_reset();
	shared_and_split();
	bad_backing_free();
	uint32_t lists, bytes;
	displaylist_storage_bytes(&lists, &bytes);
	assert(!lists && !bytes && !s_read_count);
	assert(s_generated_vbos == s_deleted_vbos);
	for(unsigned k = 0; k < s_alloc_count; k++) {
#ifdef _WIN32
		if(s_alloc[k].aligned) _aligned_free(s_alloc[k].p);
		else free(s_alloc[k].p);
#else
		free(s_alloc[k].p);
#endif
	}
	puts("PASS displaylist retirement: real backend, delayed fake GPU, poison checks");
	return 0;
}
