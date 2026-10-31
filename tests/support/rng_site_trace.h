/* Opt-in attribution rig. Never included by the normal build. */
#ifndef TESTS_SUPPORT_RNG_SITE_TRACE_H
#define TESTS_SUPPORT_RNG_SITE_TRACE_H
#ifndef CAVEWII_RAND_SEED_TEST
#error "RNG attribution is test-only"
#endif
#include <stdint.h>
struct server_local;
void rng_site_trace_bind(struct server_local* s, const int* tick);
void rng_site_trace_emit(unsigned kind, const void* stream, uint64_t value,
                         uint64_t seed);
#endif
