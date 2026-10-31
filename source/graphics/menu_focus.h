/* Controller menu state without renderer/texture dependencies. */
#ifndef MENU_FOCUS_H
#define MENU_FOCUS_H
#include <stdbool.h>

/* Pointer loss retains selection; navigation wins until the pointer moves. */
struct gutil_menu_focus {
	bool initialized, pointer_valid, pointer_owner;
	float x, y;
};
bool gutil_menu_pointer_focus(struct gutil_menu_focus* focus, bool valid,
	float x, float y, bool navigating);

struct gutil_menu_repeat {
	int direction;
	float remaining;
};
/* Direction 0 releases. First repeat after 350ms, then 10Hz. */
int gutil_menu_repeat_step(struct gutil_menu_repeat* repeat, int direction,
	float dt);
#endif
