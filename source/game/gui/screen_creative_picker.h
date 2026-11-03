/* Owner-local creative picker presentation. GPL-3.0-or-later. */
#ifndef SCREEN_CREATIVE_PICKER_H
#define SCREEN_CREATIVE_PICKER_H
#include "container_menu.h"
struct input_player_frame;
struct creative_picker {
	int cursor, pointer_target;
	size_t page;
	struct gutil_menu_focus focus;
	int picker_origin; /* enum gutil_menu_origin */
	bool compact, pointer_available, suspended;
	float pointer_x, pointer_y, pointer_angle;
};
enum creative_picker_action {
	CREATIVE_PICKER_KEEP, CREATIVE_PICKER_INVENTORY, CREATIVE_PICKER_CLOSE
};
void creative_picker_open(struct creative_picker*);
enum creative_picker_action creative_picker_update(struct creative_picker*,
	const struct input_player_frame*, int x, int y, int width, int height,
	container_send_fn, void*);
void creative_picker_render(struct creative_picker*, unsigned player, int width, int height);
void screen_creative_picker_to_compact(struct creative_picker*);
void screen_creative_picker_from_compact(const struct creative_picker*);
#endif
