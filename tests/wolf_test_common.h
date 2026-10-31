#ifndef WOLF_TEST_COMMON_H
#define WOLF_TEST_COMMON_H

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "entity/entity_wolf.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h) -- wolf_test_make/wolf_test_clear dereference s->world/s->entities

static struct entity* wolf_test_make(struct server_local* s, uint32_t id,
									  int monster_id) {
	struct entity* e = calloc(1, sizeof(*e));
	assert(e);
	entity_monster(id, e, true, &s->world, monster_id);
	struct entity** slot = dict_entity_safe_get(s->entities, id);
	assert(slot && !*slot);
	*slot = e;
	return e;
}

static void wolf_test_clear(struct server_local* s) {
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(s->entities);
}

#endif
