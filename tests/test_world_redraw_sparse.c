#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"

/* This focused target deliberately links world_redraw.c without the rest of
 * world.c. Its direct dictionary setup still needs the ordinary, non-journaled
 * allocator path that world.c uses whenever no checked resize is armed. */
void* world_wsection_dict_realloc(void* ptr, size_t old_size, size_t new_size) {
	(void)old_size;
	return realloc(ptr, new_size);
}

void world_wsection_dict_free(void* ptr) {
	free(ptr);
}

void world_wsection_dict_memory_full(void) {
	abort();
}

int main(void) {
	struct world world;
	memset(&world, 0, sizeof(world));
	dict_wsection_init(world.sections);

	struct chunk bottom;
	struct chunk top;
	memset(&bottom, 0, sizeof(bottom));
	memset(&top, 0, sizeof(top));

	struct world_section section;
	memset(&section, 0, sizeof(section));
	section.column[0] = &bottom;
	section.column[COLUMN_HEIGHT - 1] = &top;

	dict_wsection_set_at(world.sections, SECTION_TO_ID(0, 0), section);

	world_redraw_chunks(&world);

	assert(bottom.rebuild_displist);
	assert(top.rebuild_displist);

	dict_wsection_clear(world.sections);
	return 0;
}
