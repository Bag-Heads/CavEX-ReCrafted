/* Scripted checklist 6b. Production actions remain in main.c's local pause
 * handler; this driver supplies owned input frames and verifies observations. */
#ifndef GAME_SPLIT_PORTAL_QUIT_SCRIPT_H
#define GAME_SPLIT_PORTAL_QUIT_SCRIPT_H
#include <stdbool.h>
#include <stdint.h>
#include "../platform/input_owner.h"

struct client_session;
struct hw_run_config;

struct split_portal_quit_hooks {
	bool (*emit)(const char* line);
	bool (*arm_portal)(void);
	bool (*arm_autoload)(const char* world);
	bool (*block_at)(int x, int y, int z, uint32_t* id, uint32_t* meta);
	bool (*saved_block_at)(int x, int y, int z, uint32_t* id,
		uint32_t* meta);
	void (*done)(const char* status);
};
struct split_portal_quit_ctx {
	struct client_session* host;
	struct client_session* guest;
	bool world_loaded, world_switch_pending, at_title;
	bool switch_started;
	bool request_after_switch;
	float portal_progress;
	int dimension;
	uint32_t tick, up_ms, unload_requests;
};
struct split_portal_quit_actions {
	bool suppress_autojoin;
	bool guest_frame_valid, host_frame_valid;
	struct input_player_frame guest_frame, host_frame;
};

void split_portal_quit_arm(const struct hw_run_config* cfg,
	const struct split_portal_quit_hooks* hooks);
void split_portal_quit_capture_recovery_marker(const char* line);
void split_portal_quit_confirm_sent(uint32_t tick, uint32_t up_ms);
void split_portal_quit_switch_begin(uint32_t tick, uint32_t up_ms);
void split_portal_quit_p3_enter(uint32_t* last_seen, uint32_t begin_seq);
void split_portal_quit_observe_switch_begin(uint32_t* last_seen,
	uint32_t begin_seq, uint32_t tick, uint32_t up_ms);
bool split_portal_quit_post_switch_position(float* x, float* y, float* z);
void split_portal_quit_frame(const struct split_portal_quit_ctx* ctx,
	struct split_portal_quit_actions* out);
bool split_portal_quit_armed(void);
#endif
