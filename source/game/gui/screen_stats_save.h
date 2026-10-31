/* Main-thread statistics exit transaction. Controller bindings stay shared. */
#ifndef SCREEN_STATS_SAVE_H
#define SCREEN_STATS_SAVE_H
#include <stdbool.h>
#include <stdint.h>
#include "../../graphics/gui_util.h"
struct input_player_frame;
enum stats_exit_result { STATS_EXIT_NONE, STATS_EXIT_BACK, STATS_EXIT_SAVED, STATS_EXIT_WITHOUT_SAVE };
struct stats_exit_dialog {
	bool active, failed, armed, pointer_available;
	unsigned mask, selected, needed;
	uint32_t target[2];
	struct gutil_menu_focus focus;
	enum gutil_menu_origin origin;
	float px, py, angle;
	enum input_category category;
	bool category_seen;
};
/* Pure owned input and production persistence poll, also used by tests. */
void stats_exit_open(struct stats_exit_dialog*, unsigned mask);
enum stats_exit_result stats_exit_update(struct stats_exit_dialog*,
	const struct input_player_frame*, int x, int y, int width, int height);
void stats_exit_render(const struct stats_exit_dialog*, const struct input_player_frame*, int width, int height);
void screen_stats_save_open(unsigned owner, unsigned mask,
	void (*done)(void*, enum stats_exit_result), void* user);
/* An already-failed terminal save: require user Retry before any new write. */
void screen_stats_save_error(unsigned owner, unsigned mask,
	void (*done)(void*, enum stats_exit_result), void* user);
bool screen_stats_save_active(unsigned owner);
bool screen_stats_save_update(unsigned owner, const struct input_player_frame*, int x, int y, int w, int h);
void screen_stats_save_render(unsigned owner, const struct input_player_frame*, int w, int h);
/* Providers must be quiescent. Interactive mode keeps input/render/events
 * alive; either local controller can answer. Unattended hw_run mode retries
 * twice, then attempts .unsaved and exits, with a 480-poll overall budget.
 * Filesystem calls themselves are synchronous main-thread platform calls. */
#define STATS_QUIT_MAX_POLLS 480u
bool screen_stats_save_quit(bool unattended);
#endif
