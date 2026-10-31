#ifndef SPLIT_GUEST_EYE_H
#define SPLIT_GUEST_EYE_H

#include <stdbool.h>

/* Split-dims B1 / C1 hand-off: the guest-body sample every heartbeat carries
 * as gy / guest_head_block / guest_air (hw_telemetry_split_guest_eye).
 *
 * main.c's emit_hw_heartbeat samples it ONCE per heartbeat for EVERY profile,
 * right after the telemetry-active guard, from the guest body as it stands
 * after this frame's owned walk. Before this, the only feed was gated to
 * split_play_8m, so every other guest profile (split_dims_6m included)
 * reported the initialized gy 0.0 whatever the body did -- and the
 * split_dims_6m collector rejects a guest heartbeat without a real, finite gy.
 *
 * The policy is kept here, free of engine types, so a test can pin it:
 *  - join / every heartbeat with a body: report that body's CURRENT feet Y,
 *    head block and air (a changed Y is always re-sampled, never latched);
 *  - leave (no body): report the neutral 0 / 0 / 0 sample, so a later join
 *    can never inherit the departed body's values;
 *  - respawn: the new body is simply the body present at the next heartbeat.
 * The caller resolves head_block from the world at the body's feet cell. */
struct split_guest_eye_sample {
	bool present;
	float y;
	int head_block;
	int air;
};

static inline struct split_guest_eye_sample
split_guest_eye_heartbeat(const float* body_pos, int head_block, int air) {
	struct split_guest_eye_sample out = {0};
	if(!body_pos) return out;
	out.present = true;
	out.y = body_pos[1];
	out.head_block = head_block;
	out.air = air;
	return out;
}

#endif
