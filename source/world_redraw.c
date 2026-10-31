/*
	Copyright (c) 2026

	This file is part of CavEX.
*/

#include <assert.h>

#include "world.h"

void world_redraw_chunks(struct world* w) {
	assert(w);

	dict_wsection_it_t it;
	dict_wsection_it(it, w->sections);

	while(!dict_wsection_end_p(it)) {
		struct world_section* s = &dict_wsection_ref(it)->value;
		for(int i = 0; i < COLUMN_HEIGHT; i++) {
			if(s->column[i])
				s->column[i]->rebuild_displist = true;
		}
		dict_wsection_next(it);
	}
}
