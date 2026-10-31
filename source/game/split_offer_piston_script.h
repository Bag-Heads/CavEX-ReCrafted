/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef GAME_SPLIT_OFFER_PISTON_SCRIPT_H
#define GAME_SPLIT_OFFER_PISTON_SCRIPT_H
#include "vehicle_smoke_script.h"
#include "../network/split_offer_piston_probe.h"

/* A disclosed fixture delays the real ACCEPT handler. Join, pose, correction,
 * Confirm and GAMEPLAY_READY stay authored by their production paths. The
 * main-thread driver uses the existing controller/autopilot actions/hooks;
 * its independent server FIFO records the complete piston extension. */
void split_offer_piston_arm(const struct hw_run_config* cfg,
	const struct vehicle_smoke_hooks* hooks);
void split_offer_piston_reset(void);
bool split_offer_piston_armed(void);
void split_offer_piston_frame(const struct vehicle_smoke_ctx* ctx,
	struct vehicle_smoke_actions* out);
bool split_offer_piston_finished(void);
const char* split_offer_piston_failure(void);
void screen_sworld_split_offer_piston_authorize(bool on);
bool screen_sworld_create_split_offer_piston_rig(const char* display,
	const char* directory, bool seed_set, int64_t seed);
#endif
