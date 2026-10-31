/* Copyright (c) 2026 CaveWii_v2 contributors
 * Wrap the REAL thread mutex constructor with libogc's 64-slot ceiling.
 * column_backing_init creates SERVER_WORLD_PERSISTENT_MUTEXES slots in one
 * atomic transaction: the eight backing stripes plus the G9 light-drain
 * heartbeat lock (bf92f3e33; CD1, Astra ruling 2026-10-08 item 3). */
#include "support/split_residency_fixture.h"

static unsigned live, peak, calls, limit = 64, fail_call;
bool __real_thread_mutex_init(struct thread_mutex* m);
void __real_thread_mutex_destroy(struct thread_mutex* m);

bool __wrap_thread_mutex_init(struct thread_mutex* m) {
	calls++;
	if(live == limit || calls == fail_call) return false;
	if(!__real_thread_mutex_init(m)) return false;
	if(++live > peak) peak = live;
	return true;
}

void __wrap_thread_mutex_destroy(struct thread_mutex* m) {
	if(m->initialized) {
		assert(live);
		live--;
	}
	__real_thread_mutex_destroy(m);
}

/* One failed boot at constructor `k` (1-based, relative to this attempt):
 * the attempt stops exactly there and releases every slot it took. */
static void fail_at(unsigned k) {
	const unsigned base = calls;
	fail_call = base + k;
	assert(!column_backing_init());
	assert(live == 0);
	assert(calls - base == k); // stopped AT position k, nothing after it
	fail_call = 0;
}

/* Fresh process per position: fail at k, then the retry is complete and a
 * re-init is idempotent (no extra constructors). */
static int fresh_position(unsigned k) {
	assert(calls == 0 && live == 0);
	fail_at(k);
	const unsigned before = calls;
	assert(column_backing_init());
	assert(live == SERVER_WORLD_PERSISTENT_MUTEXES);
	assert(calls - before == SERVER_WORLD_PERSISTENT_MUTEXES);
	const unsigned boot_calls = calls;
	assert(column_backing_init() && calls == boot_calls
		&& live == SERVER_WORLD_PERSISTENT_MUTEXES);
	printf("PASS position %u: unwound to live=0, retry live=%u, re-init idempotent\n",
		k, live);
	return 0;
}

int main(int argc, char** argv) {
	_Static_assert(SERVER_WORLD_PERSISTENT_MUTEXES == COLUMN_BACKING_LOCK_STRIPES + 1,
		"eight backing stripes + the G9 light-drain heartbeat lock");
	if(argc == 3 && !strcmp(argv[1], "--fail-at"))
		return fresh_position((unsigned)strtoul(argv[2], NULL, 10));
	/* Each initialization position 1..9, each in its own fresh process. */
	for(unsigned k = 1; k <= SERVER_WORLD_PERSISTENT_MUTEXES; k++) {
		char cmd[1200];
		snprintf(cmd, sizeof cmd, "\"%s\" --fail-at %u", argv[0], k);
		fflush(stdout);
		assert(system(cmd) == 0);
	}
	/* The same sweep in one process (relative call indices): every failed
	 * boot leaves the init state retryable. */
	for(unsigned k = 1; k <= SERVER_WORLD_PERSISTENT_MUTEXES; k++) fail_at(k);
	assert(column_backing_init());
	assert(live == SERVER_WORLD_PERSISTENT_MUTEXES);
	unsigned boot_calls = calls;
	assert(column_backing_init() && calls == boot_calls);
	blocks_init();
	struct server_local* s = split_fixture();
	struct column_backing* retained[121];
	unsigned n = 0;
	for(int z = -5; z <= 5; z++)
		for(int x = -5; x <= 5; x++) {
			struct server_chunk* sc;
			assert(server_world_load_chunk(&s->world, x, z, &sc));
			retained[n++] = sc->backing;
			column_backing_ref(sc->backing);
		}
	assert(n == 121 && live == SERVER_WORLD_PERSISTENT_MUTEXES);
	assert(calls == boot_calls); // not one constructor per retained backing
	split_destroy(s);
	for(unsigned i = 0; i < n; i++) {
		assert(column_backing_references(retained[i]) == 1);
		assert(retained[i]->ids[0] == BLOCK_BEDROCK);
		column_backing_unref(retained[i]);
	}
	uint32_t created, destroyed, references;
	column_backing_test_counts(&created, &destroyed, &references);
	assert(created == 121 && destroyed == 121 && references == 0);
	/* Prove the mock actually refuses constructor 65, not just its counter. */
	struct thread_mutex extra[64] = {0}, rejected = {0};
	unsigned available = 64 - live;
	for(unsigned i = 0; i < available; i++) assert(thread_mutex_init(&extra[i]));
	assert(!thread_mutex_init(&rejected) && peak == 64);
	for(unsigned i = 0; i < available; i++) thread_mutex_destroy(&extra[i]);
	assert(live == SERVER_WORLD_PERSISTENT_MUTEXES);
	printf("PASS 121 simultaneous backings under 64-slot mutex ceiling; "
		"%u persistent mutexes (%u backing stripes + 1 light-drain heartbeat "
		"lock), zero per-column slots; boot unwind at positions 1..%u\n", live,
		(unsigned)COLUMN_BACKING_LOCK_STRIPES,
		(unsigned)SERVER_WORLD_PERSISTENT_MUTEXES);
	return 0;
}
