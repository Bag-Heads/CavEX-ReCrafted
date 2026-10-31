/* Test-side client frames before a save that requires RPC headroom. */
#ifndef CAVEWII_TEST_CLIN_DRAIN_H
#define CAVEWII_TEST_CLIN_DRAIN_H

#include <assert.h>

#include "network/client_interface.h"

static inline void test_clin_drain(void) {
	for(unsigned pass = 0; pass < 64; pass++) {
		clin_update_drain();
		if(clin_rpc_flush_pending()) {
			/* The successful flush may have refilled the inbox. */
			clin_update_drain();
			return;
		}
	}
	assert(0 && "test_clin_drain: pending output after 64 client frames");
}

#endif
