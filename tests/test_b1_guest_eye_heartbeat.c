/* Split-dims B1 / C1 hand-off: the guest-body heartbeat sample (gy /
 * guest_head_block / guest_air) main.c's emit_hw_heartbeat feeds for EVERY
 * profile (source/game/split_guest_eye.h), serialized through the real
 * hw_telemetry heartbeat formatter under the split_dims_6m profile.
 *
 * Before B1 the only feed was gated to split_play_8m, so a split_dims_6m guest
 * heartbeat carried the initialized gy 0.0 whatever the body did (the
 * collector rejects a guest heartbeat without a real, finite gy). Scenario:
 * join -> changed Y -> leave -> respawn, one heartbeat each, exactly the order
 * main.c samples in (sampler, then hw_telemetry_split_guest_eye, then the
 * heartbeat whose `guest` field is `players[1].local_player != NULL`). */
#include "game/split_guest_eye.h"
#include "platform/hw_telemetry.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void hw_telemetry_split_guest_eye(float y, int head_block, int air);

/* The narrow link set of test_hw_telemetry (the heartbeat formatter, not the
 * collector or the RPC layer): the same two leaf stubs it provides. */
uint32_t svin_dropped_count(void) { return 0; }
void tps_observer_close(bool complete) { (void)complete; }

static char buf[16384];

/* One heartbeat as main.c builds it: body (or NULL) -> sample -> emit. */
static const char* beat(const float* body_pos, int head_block, int air) {
	struct split_guest_eye_sample eye
		= split_guest_eye_heartbeat(body_pos, head_block, air);
	hw_telemetry_split_guest_eye(eye.y, eye.head_block, eye.air);
	struct hw_telemetry_heartbeat hb;
	memset(&hb, 0, sizeof hb);
	hb.storage_enabled = true; /* main.c always sets it (the guest block lives there) */
	hb.guest = body_pos != NULL;
	hb.gx = body_pos ? body_pos[0] : 0.0F;
	hb.gz = body_pos ? body_pos[2] : 0.0F;
	size_t n = hw_telemetry_format_heartbeat(buf, sizeof buf, "b1run",
											 "split_dims_6m", &hb);
	assert(n > 0 && n < sizeof buf && buf[n - 1] == '}');
	return buf;
}

int main(void) {
	/* Before any guest: no guest block at all. */
	assert(!strstr(beat(NULL, 0, 0), "\"gy\""));

	/* Join: the new body's feet Y, head block and air are reported. */
	float body[3] = {10.5F, 64.5F, -3.5F};
	const char* line = beat(body, 0, 300);
	assert(strstr(line, "\"gy\":64.50"));
	assert(strstr(line, "\"guest_head_block\":0,\"guest_air\":300"));

	/* Changed Y (e.g. the body walked into water): re-sampled, not latched. */
	body[1] = 61.25F;
	line = beat(body, 9, 120);
	assert(strstr(line, "\"gy\":61.25"));
	assert(strstr(line, "\"guest_head_block\":9,\"guest_air\":120"));

	/* Leave: no guest block, and the neutral sample replaces the old one. */
	line = beat(NULL, 9, 120);
	assert(!strstr(line, "\"gy\""));
	struct split_guest_eye_sample gone = split_guest_eye_heartbeat(NULL, 9, 120);
	assert(!gone.present && gone.y == 0.0F && gone.head_block == 0
		   && gone.air == 0);

	/* Respawn (a new body at the spawn point): its own values, never the
	 * departed body's. */
	float respawned[3] = {0.5F, 70.0F, 0.5F};
	line = beat(respawned, 0, 300);
	assert(strstr(line, "\"gy\":70.00"));
	assert(strstr(line, "\"guest_head_block\":0,\"guest_air\":300"));
	assert(!strstr(line, "61.25"));

	/* splitdims-profile-0926: the per-view dimension block. Disabled (every
	 * profile but split_dims_6m) it adds nothing: the line carries no dims
	 * key. Enabled it appends exactly the five fields the collector's
	 * SplitDimsEvidence reads. */
	{
		static char plain[16384];
		struct hw_telemetry_heartbeat hb;
		memset(&hb, 0, sizeof hb);
		hb.storage_enabled = true;
		size_t n0 = hw_telemetry_format_heartbeat(plain, sizeof plain, "b1run",
												  "split_dims_6m", &hb);
		assert(n0 > 0 && !strstr(plain, "\"dim_v") && !strstr(plain, "\"dims_active\"")
			   && !strstr(plain, "\"dims_phase\""));
		hb.dims = (struct hw_split_dims_evidence) {.enabled = true,
			.dim_v0 = 0, .dim_v1 = -1, .views_live = 2, .dims_active = true,
			.phase = "across", .srv_dim_host = 0, .srv_dim_guest = -1,
			.host_phase = "done", .host_commits = 2, .slot1_cols = 0,
			.rel_sent = 1, .rel_rx = 1, .rel_slot = 1, .wslot_v0 = 0,
			.wslot_v1 = 1, .pb = {.column_pool_used = 162, .column_pool_peak = 176,
			.column_pool_slabs = 6, .column_pool_overflow = 3,
			.column_pool_carve_fail = 2, .budget_valid = true, .budget_wii = true,
			.budget_resident = 162, .budget_live_backings = 173,
			.budget_reserved = 3, .budget_prepeak = 489, .budget_peak = 176,
			.budget_waits = 7, .budget_phase = 2}, .transfer_request = 42,
			.guest_commits = 1, .transfer_actor = 1, .transfer_state = 2,
			.transfer_held = true, .transfer_source_draining = true,
			.transfer_restoring = false, .host_rb_id = 17,
			.host_rb_reason = 2, .guest_rb_id = 23, .guest_rb_reason = 3};
		size_t n1 = hw_telemetry_format_heartbeat(buf, sizeof buf, "b1run",
												  "split_dims_6m", &hb);
		assert(n1 > n0 && buf[n1 - 1] == '}');
		assert(strstr(buf, ",\"dim_v0\":0,\"dim_v1\":-1,\"views_live\":2,"
						   "\"dims_active\":true,\"dims_phase\":\"across\","
						   "\"srv_dim_host\":0,\"srv_dim_guest\":-1,"
						   "\"host_phase\":\"done\",\"host_commits\":2,"
						   "\"slot1_cols\":0,\"rel_sent\":1,\"rel_rx\":1,"
						   "\"rel_slot\":1,\"wslot_v0\":0,\"wslot_v1\":1"));
		/* The unarmed line is the armed line minus exactly that block. */
		assert(strstr(buf, "\"column_pool_used\":162,\"column_pool_peak\":176,"
			"\"column_pool_slabs\":6,\"column_pool_overflow\":3,"
			"\"column_pool_carve_fail\":2"));
		assert(strstr(buf, "\"budget_valid\":true,\"budget_wii\":true,\"budget_resident\":162,"
			"\"budget_live_backings\":173,\"budget_reserved\":3,"
			"\"budget_prepeak\":489,\"budget_peak\":176,\"budget_waits\":7,"
			"\"budget_phase\":2,\"transfer_request\":42,\"guest_commits\":1,"
			"\"transfer_actor\":1,\"transfer_state\":2,\"transfer_held\":true,"
			"\"transfer_source_draining\":true,\"transfer_restoring\":false,"
			"\"host_rb_id\":17,\"host_rb_reason\":2,\"guest_rb_id\":23,"
			"\"guest_rb_reason\":3"));
		const char* block = strstr(buf, ",\"dim_v0\"");
		const size_t pre = (size_t)(block - buf);
		const char* after = strstr(block, "\"guest_rb_reason\":3")
			+ strlen("\"guest_rb_reason\":3");
		assert(memcmp(buf, plain, pre) == 0
			   && strcmp(after, plain + pre) == 0);
		hb.dims = (struct hw_split_dims_evidence) {.enabled = true,
			.dim_v0 = 0, .dim_v1 = -128, .views_live = 1,
			.srv_dim_guest = -128};
		(void)hw_telemetry_format_heartbeat(buf, sizeof buf, "b1run",
											"split_dims_6m", &hb);
		assert(strstr(buf, "\"dim_v1\":-128,\"views_live\":1,"
						   "\"dims_active\":false,\"dims_phase\":\"pane_wait\""));
		assert(strstr(buf, "\"host_phase\":\"wait\""));
	}

	printf("PASS test_b1_guest_eye_heartbeat: join, changed Y, leave, respawn, "
		   "split_dims fields\n");
	return 0;
}
