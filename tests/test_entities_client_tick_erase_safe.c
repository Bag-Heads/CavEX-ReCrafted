/*
	entities_client_tick erase-safety (entity.c). m-dict.h's open-addressing
	_erase() can trigger a resize-down that reshuffles the whole backing array
	(m-dict.h's IT_REMOVE note: "the dict may perform a resize down operation
	... This operation renders the computation of the 'next' element
	impossible"). The prior code called dict_entity_next() BEFORE
	dict_entity_erase() on the SAME tick an entity expired, which does not
	protect against this -- the iterator was already advanced against the OLD
	array layout, so a resize invalidates it regardless of erase/next order,
	and can skip a live entity or double-tick one relocated into an
	already-visited slot.

	Fix: never erase during the walk. Collect the keys of entities to remove
	then erase and free them in a second pass once iteration is done.

	This is pure engine bookkeeping (a C data-structure hazard, not a
	simulated value), so the proof obligation is a stress test, not a
	decompile citation: many entities, several expiring on the SAME tick via
	BOTH removal paths entities_client_tick recognizes
	(`bool remove = (e->delay_destroy == 0) || e->tick_client(e);` --
	short-circuiting means a delay_destroy==0 entity's tick_client is never
	even called, exactly like the pre-fix code), none skipped and none
	double-ticked.

	Three groups, skewed 1-in-5 to survive (id % 5 -- see the ratio note at
	the assignment site for why an even split does not exercise the hazard):
	  survive (1-in-5): delay_destroy != 0, tick_client returns false.
	     tick_count must be exactly 1.
	  delay-zero (2-in-5): delay_destroy == 0 -- expires WITHOUT its
	     tick_client ever running (the short-circuit). tick_count must stay 0.
	  self-report (2-in-5): delay_destroy != 0, tick_client returns true --
	     expires via its OWN report. tick_count must be exactly 1 (it must
	     still run, and run only once, even though it's also being erased
	     this same pass).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "entity/entity.h"

#define N_ENTITIES 40

enum group { GROUP_DELAY_ZERO = 0, GROUP_SURVIVE = 1, GROUP_SELF_REPORT = 2 };

static int g_tick_count[N_ENTITIES];
static enum group g_group[N_ENTITIES];
static dict_entity_t* g_dict;
static uint32_t g_scanner_id;
static int g_scan_count;

static bool stub_tick_client(struct entity* e) {
	assert(e->id < N_ENTITIES);
	g_tick_count[e->id]++;
	if(e->id == g_scanner_id) {
		// Recycle same-size allocations before the dictionary scan so a
		// dangling entry is observable even without an address sanitizer.
		struct entity* replacements[N_ENTITIES * 4];
		for(size_t i = 0; i < N_ENTITIES * 4; i++) {
			replacements[i] = malloc(sizeof(*e));
			assert(replacements[i]);
			replacements[i]->type = (enum entity_type)-1;
		}
		dict_entity_it_t scan;
		dict_entity_it(scan, *g_dict);
		while(!dict_entity_end_p(scan)) {
			struct entity* other = dict_entity_ref(scan)->value;
			volatile enum entity_type type = other->type;
			assert(type == ENTITY_ITEM);
			g_scan_count++;
			dict_entity_next(scan);
		}
		for(size_t i = 0; i < N_ENTITIES * 4; i++)
			free(replacements[i]);
	}
	return g_group[e->id] == GROUP_SELF_REPORT;
}

static void run_case(uint32_t count) {
	dict_entity_t dict;
	dict_entity_init(dict);
	g_dict = &dict;
	g_scanner_id = N_ENTITIES;
	g_scan_count = 0;
	memset(g_tick_count, 0, sizeof(g_tick_count));

	bool expire[N_ENTITIES];
	for(uint32_t id = 0; id < count; id++) {
		struct entity* e = calloc(1, sizeof(*e));
		assert(e);
		e->id = id;
		e->type = ENTITY_ITEM;
		e->tick_client = stub_tick_client;
		// Skewed 1-in-5 survival (not an even 3-way split): m-dict.h's
		// open-addressing _erase() only shrinks when count drops BELOW
		// dict->lower_limit (~0.2 * capacity, m-dict.h:1541-1543) -- for 40
		// inserts the table grows to capacity 64 (M_D1CT_OA_UPPER_BOUND=0.7
		// crossed at 16 and 32), so lower_limit is 64*0.2=12(.8): the walk
		// must remove enough that fewer than 12 entities remain for the
		// resize-down (and the iterator-invalidation hazard this test
		// exists to catch) to actually fire mid-walk. An even 1-in-3 split
		// (~13 survivors) sits just ABOVE that threshold and was verified
		// NOT to reproduce the bug even against the unfixed code -- this
		// ratio was tuned by actually crossing dict->lower_limit, not
		// guessed.
		int r = (int)(id % 5);
		enum group g = (r == 0) ? GROUP_SURVIVE
			: (r == 1 || r == 2) ? GROUP_DELAY_ZERO
			: GROUP_SELF_REPORT;
		g_group[id] = g;
		expire[id] = (g != GROUP_SURVIVE);
		e->delay_destroy = (g == GROUP_DELAY_ZERO) ? 0 : 5;
		struct entity** slot = dict_entity_safe_get(dict, id);
		assert(slot && !*slot);
		*slot = e;
	}
	// Choose traversal positions, not numeric IDs: both expiry paths must
	// precede the scanner regardless of the dictionary's bucket layout.
	dict_entity_it_t order;
	dict_entity_it(order, dict);
	for(uint32_t pos = 0; pos < count; pos++) {
		assert(!dict_entity_end_p(order));
		struct entity* e = dict_entity_ref(order)->value;
		if(pos < 2) {
			g_group[e->id] = pos == 0 ? GROUP_DELAY_ZERO : GROUP_SELF_REPORT;
			e->delay_destroy = pos == 0 ? 0 : 5;
			expire[e->id] = true;
		}
		if(pos == count - 1) {
			g_scanner_id = e->id;
			g_group[e->id] = GROUP_SURVIVE;
			e->delay_destroy = 5;
			expire[e->id] = false;
		}
		dict_entity_next(order);
	}
	size_t expected_remaining = 0;
	for(uint32_t id = 0; id < count; id++)
		if(!expire[id])
			expected_remaining++;

	entities_client_tick(dict);
	assert(g_scan_count == (int)count);

	// GROUP_DELAY_ZERO short-circuits tick_client entirely (matches the
	// pre-existing, unchanged `(delay_destroy==0) || tick_client(e)` order)
	// -- it must NEVER have run. Every other entity's tick_client must have
	// run EXACTLY once: no skip and no double-tick, even with many expiring
	// (via either removal path) on this same pass.
	bool bad = false;
	for(uint32_t id = 0; id < count; id++) {
		int want = (g_group[id] == GROUP_DELAY_ZERO) ? 0 : 1;
		if(g_tick_count[id] != want) {
			printf("    [MISMATCH] id=%u group=%d tick_count=%d want=%d\n",
				   id, (int)g_group[id], g_tick_count[id], want);
			bad = true;
		}
	}
	assert(!bad
		   && "every non-delay-zero entity's tick_client must run exactly "
			  "once; a delay-zero one must never run at all");

	// Every expiring entity (both groups) was actually erased; every
	// surviving one remains, untouched.
	size_t remaining = 0;
	dict_entity_it_t it;
	dict_entity_it(it, dict);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		assert(e && !expire[e->id] && "an expiring entity must have been erased");
		assert(e->delay_destroy == 5);
		remaining++;
		dict_entity_next(it);
	}
	assert(remaining == expected_remaining);
	assert(dict_entity_size(dict) == expected_remaining);

	dict_entity_clear(dict);
	printf("test_entities_client_tick_erase_safe: %u entities, %d expired "
		   "same-tick (delay-zero + self-reported), %d survivors -- every "
		   "tick_client that should run did, exactly once, no skip/"
		   "double-tick OK\n",
		   count, (int)(count - expected_remaining),
		   (int)expected_remaining);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	run_case(24); // stack key buffer
	run_case(N_ENTITIES); // heap key buffer
	printf("PASS test_entities_client_tick_erase_safe\n");
	return 0;
}
