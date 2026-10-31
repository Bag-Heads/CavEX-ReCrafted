/* Test-only primitive draw logger for archived/current golden attribution.
 * Raw return values and RNG transitions are preserved by the generated rig.
 * Caller addresses resolve against that exact executable's DWARF; ASLR is
 * removed here. gzip keeps complete traces inside the evidence disk budget. */
#include "rng_site_trace.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <windows.h>
#include <zlib.h>
static struct server_local* session;
static const int* scenario_tick;
static gzFile output;
static int initialized;
static void close_trace(void) {
	if(output) { assert(gzclose(output) == Z_OK); output = NULL; }
}
static void abort_trace(int signal_number) {
	close_trace();
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}
void rng_site_trace_bind(struct server_local* s, const int* tick) {
	session = s;
	scenario_tick = tick;
}
void rng_site_trace_emit(unsigned kind, const void* stream, uint64_t value,
                         uint64_t seed) {
	if(!initialized) {
		initialized = 1;
		const char* path = getenv("CW_RNG_SITE_TRACE");
		if(path && *path) {
			output = gzopen(path, "wb1"); assert(output);
			assert(gzbuffer(output, 1024 * 1024) == 0);
			assert(atexit(close_trace) == 0);
			signal(SIGABRT, abort_trace);
		}
	}
	if(!output) return;
	/* 12 little-endian u64 fields. kind 0=xorshift, 1=Java48; semantic
	 * streams 1=session, 2=World.rand, 3=body, 4=entity, 0=other. */
	uint64_t record[12] = {kind, (uintptr_t)stream,
		scenario_tick ? (uint64_t)*scenario_tick
			: session ? session->worlds[0].current_tick : UINT64_MAX,
		value, seed, 0, 0};
	if(session) {
		if(stream == &session->rand_src) record[5] = 1;
#ifdef RNG_TRACE_WORLD_RAND
		else if(stream == &session->worlds[0].rand) record[5] = 2;
#endif
#ifdef RNG_TRACE_BODY_RAND
		else if(stream == &session->player.entity_rand.r) record[5] = 3;
#endif
		else {
#ifdef RNG_TRACE_ENTITY_RAND
			dict_entity_it_t it;
			for(dict_entity_it(it, session->entities); !dict_entity_end_p(it);
			    dict_entity_next(it)) {
				struct entity* e = dict_entity_ref(it)->value;
				if(stream == &e->gameplay_rng) {
					record[5] = 4; record[6] = e->id; break;
				}
			}
#endif
		}
	}
	void* stack[5] = {0};
	USHORT count = CaptureStackBackTrace(1, 5, stack, NULL);
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
	for(unsigned i = 0; i < count; i++) record[7 + i] = (uintptr_t)stack[i] - base;
	assert(gzwrite(output, record, sizeof record) == sizeof record);
}
