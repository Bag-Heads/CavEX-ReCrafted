/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef PLATFORM_HW_TELEMETRY_H
#define PLATFORM_HW_TELEMETRY_H

#include "../tps_observer.h"
#include "g9_priority_lease.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* R19 lease owner callback; IRQ-safe, stores/restores sender's known base. */
bool hw_telemetry_lease_priority(uint8_t priority);

// Bumped 1536 -> 2048 (save_migrate chain-widening, 2026-08-24): the six new
// smig_chests*/smig_spawners* fields pushed the UINT32_MAX-everywhere worst
// case (test_heartbeat_production_buffer_fits_full_payload) past the old
// 1536 ceiling -- caught by that test's own `n < sizeof(buf)` assert, not by
// inspection. 2048 leaves real headroom for the next field, not just enough
// to pass today's worst case.
// Bumped 2048 -> 2560 (2026-09-03, lane/perf-t1-frame-drops T1-a..d
// telemetry hook): same test measured 2067 bytes at the UINT32_MAX-everywhere
// worst case BEFORE the two new dlcalls/tris fields below pushed it past
// 2048 again. 2560 restores ~490 bytes of headroom for the next field.
//
// Bumped 2560 -> 2816 (2026-09-03 merge into candidates-6, lane/telemetry-
// far-ring-keepup): that lane independently measured its own six new
// far_missing/far_missing_max/far_settle_ms/srv_gen_us/srv_gen_cols/
// srv_gen_backlog fields as needing 2048 -> 2304 (+256 bytes) against a
// heartbeat that did NOT yet have this lane's dlcalls/tris fields. Both
// deltas are additive on the SAME worst-case line (dlcalls/tris AND the six
// far-ring/gen fields are both emitted every heartbeat), so the merged
// ceiling is the sum of both increments over the pre-either-lane 2048
// baseline: 2560 + (2304 - 2048) = 2816. Not re-measured from a combined
// build (no combined worst-case buffer existed until this merge); this is
// the additive-worst-case bound, and
// test_heartbeat_production_buffer_fits_full_payload is the actual gate --
// if the real combined payload comes in under 2816 the test still passes
// with headroom, and if a future field pushes it over, that test's own
// `n < sizeof(buf)` assert (not inspection) is what will catch it, same as
// both prior bumps.
//
// Bumped 2816 -> 2944 (2026-09-03, lane/perf-t1-ramp-slice P2-c telemetry
// hook): three new fields (ms_mesh_worker, ms_mesh_worker_max, mem2_lock_
// wait) added below. Same additive-worst-case reasoning as the 2560 bump: not
// re-measured from a combined build, just headroom sized to comfortably
// cover the new fields' worst-case JSON width (two %.2f floats + one %u,
// well under the ~130 bytes of slack this adds); test_heartbeat_production_
// buffer_fits_full_payload's own `n < sizeof(buf)` assert is still the real
// gate.
//
// Bumped 2944 -> 3200 (2026-09-03, lane/telemetry-robust, Round 3 (3)/(2)(a)
// fixes): five new fields (tel_fail, tel_last_errno, tel_quiesced,
// frame_counter, mem2_carve_free_kb) added below. Worst-case width is two
// int32 fields (up to 11 chars incl. sign), two uint32 fields (10 chars) and
// one bool (5 chars for "false"), well under the 256-byte headroom added
// here. Same additive-worst-case reasoning as every prior bump: not
// re-measured from a combined build; test_heartbeat_production_buffer_fits_
// full_payload's own `n < sizeof(buf)` assert is the real gate.
/* Deep world/RPC attribution extends the prior 3584-byte heartbeat. The
 * UINT32_MAX-everywhere formatter test gates the production buffer. The
 * split-anchor frame batch plus opt-in CPU fields requires 9216 bytes; the
 * UDP collector matches it. Old field bytes are checked against the prefix. */
//
// Bumped 9216 -> 10240 (2026-09-08, server-loop wedge instrumentation lane/
// carve-observer-boot): six new server-loop fields (srv_loop_it,
// srv_elapsed_ms, srv_ticks_pend, srv_partial_m, srv_sleep_ms,
// srv_far_us_max) added below. The prior worst case measured 9,194/9,216 (22
// B headroom -- see test_heartbeat_production_buffer_fits_full_payload's own
// comment), too little for even one more field. 1,024 B leaves >= 200 B of
// headroom past this bump's own measured worst case; the test's own
// `n < sizeof(buf)` assert (not this comment) is the real gate.
//
// +22 B (2026-09-08, lane/carve-observer-boot): one new unconditional field,
// cw_set_n (uint32_t, ",\"cw_set_n\":4294967295" at its widest). Measured
// worst case moved 9,360 -> 9,382 (see test_heartbeat_production_buffer_
// fits_full_payload); still >= 200 B of headroom (858 B) under the 10,240 B
// slot, so the field stays -- had headroom fallen below 200 B this field
// would have been skipped instead (brief's own gate).
//
// Bumped 10240 -> 12288 (2026-09-14, mp-split-0912/54 S-A7). NOT a new field:
// this is the bump the S-A3 lane deliberately deferred. mp-split-0912/49 added
// the four split-toggle fields (split_live, split_pending, split_toggle_ms,
// split_last_ok, +81 B measured), which moved the worst case 10,013 -> 10,094
// and left 146 B of the 10,240 B slot -- BELOW the 200 B floor every field
// since cw_set_n has been gated on, recorded as a HEADROOM WARNING in
// test_heartbeat_production_buffer_fits_full_payload rather than absorbed
// quietly. 12,288 restores 12,288 - 10,094 = 2,194 B, which is real headroom
// again (mp_end_reason's own 301 B worst case is the widest single field ever
// added, and 2,194 covers seven of it). Precedent: the 9216 -> 10240 bump for
// the same reason (22 B left) in lane/carve-observer-boot.
//
// +26 B (2026-09-21, push-0921/B4c): one new unconditional field,
// lw_to_admits (uint32_t, ",\"lw_to_admits\":4294967295" at its widest).
// Measured worst case moves 10,283 -> 10,309 (see test_heartbeat_production_
// buffer_fits_full_payload), leaving 1,979 B of the 12,288 B slot -- above
// the 200 B floor every field since cw_set_n has been gated on, and above
// the 512 B margin the S-A7 lane asked for, so the field stays. On the WIRE
// a healthy run spends 17 B on it (",\"lw_to_admits\":0"), not 26.
//
// WHAT THE SLOT COSTS, since it is not just a formatter bound (this is why the
// S-A3 lane would not spend it):
//   * hw_telemetry.c:1077 s_pre_lines[PRECONNECT_SLOTS=8][SIZE] static .bss:
//     81,920 -> 98,304 B, i.e. +16,384 B of MEM1 .bss. Fixed at link time, no
//     allocation, and not on any stack.
//   * hw_telemetry.c:2732 (hw_telemetry_emit_anchor_frames) and :2760
//     (hw_telemetry_emit_heartbeat) each hold one `char line[SIZE]` frame:
//     +2,048 B each. BOTH run only on the MAIN thread -- emit_heartbeat via
//     main.c:882 (emit_hw_heartbeat) from main.c:2997/3033, emit_anchor_frames
//     via hw_telemetry.c:2691 (hw_anchor_evidence_note) from main.c:793 and
//     main.c:2462 -- and they are SEQUENTIAL, never nested, so at most one of
//     the two frames is live at a time: the deepest main-thread path grows by
//     2,048 B once, not 4,096. libogc's main thread has a 128 KB stack
//     (tools/check_wii_stack_usage.sh, tools/wii_stack_allowlist.txt:7), and
//     that script's own 16,384 B per-frame gate still holds at 12,288 + the
//     function's few dozen bytes of spill, with an empty allowlist.
//   * the send path is unchanged: send_line_with (hw_telemetry.c:962) still
//     does the same one `len <= HW_TELEMETRY_HEARTBEAT_LINE_SIZE` producer
//     check and the same single bounded send_bytes loop -- no chunking, no MTU
//     assumption, and the libogc net_send partial-write/-EAGAIN handling is
//     byte-for-byte the same call pattern on a larger maximum line.
//   * tools/wii_telemetry_server.py's UDP `recvfrom` was a HARD cap at the old
//     slot (a datagram truncated by recvfrom cannot be reassembled), so it is
//     raised in lockstep; its anchor selftest bound moves with it.
// As before, this comment is not the gate: test_heartbeat_production_buffer_
// fits_full_payload's `n == 10094` + spare assertions are.
// G8 opt-in dimensions/budget block: the full-width production heartbeat plus
// guest row is 11,826 B, leaving 462 B. The maximal-payload test enforces the
// existing 200 B spare floor; no larger Wii buffer or stack frame is needed.
// residency-1005/4 closure item 8: the v2 per-actor residency block (two
// rows of 22 full-width uint32 + "rzv"/"rzt" + up to four 5-value "rzi"
// marks) adds up to 803 B at its widest, which the old 12,288 B slot cannot
// hold beside the G8 split-dims worst case with its 200 B spare floor. The
// slot grows to 13,312 B. The two line frames (hw_telemetry.c) stay
// sequential, never nested: 13,312 + spill is still under the 16,384 B
// per-frame gate of tools/check_wii_stack_usage.sh on the 128 KB main
// stack. The preconnect arena keeps its old 98,304 B (fixed bytes, not
// slots x line), so the .bss layout the boot-death gate checks does not
// grow. The collector's UDP recvfrom cap moves to 13,312 in lockstep.
/* R7 retained receipts and G9 R10 phase/pacing counters share the slot.
 * M3 remeasured BOTH additions: saturated P2 + dims pool + lqd v3 + g9p
 * occupies 13,795 bytes. The minimum slot with >=200 bytes spare is 13,995;
 * async CW_TEL_LINE and the collector cap must match this value. */
#define HW_TELEMETRY_HEARTBEAT_LINE_SIZE 13995u
#define HW_TELEMETRY_RZA_FIELDS 22u
#define HW_TELEMETRY_RZI_MAX 4u
#define HW_TELEMETRY_RZI_FIELDS 5u
// mp-split-0912/25: hw_heartbeat::mp_end_reason storage -- 47 characters of
// the peer's terminal reason text plus the NUL. Sized against the SAME gate
// the comment above describes: JSON-escaping expands a control byte to six
// emitted bytes (\u00xx), so this field's worst-case contribution to the
// heartbeat line is 16 (quoted key + colon) + 2 (its own quotes) + 47*6 + 1
// (comma) = 301 B, which moved the worst case 9,712 -> 10,013 and left 227 B
// of the 10,240 B slot -- still above the 200 B floor every prior field was
// gated on, but deliberately the tightest margin so far. WIDENING THIS
// CONSTANT IS NOT FREE: every extra byte costs six in the worst case, and
// 10 more characters would breach the floor. (Those two figures are as of
// mp-split-0912/25: the slot is now 12,288 with 2,194 B spare, so the floor is
// further away -- but the six-bytes-per-character cost of widening THIS
// constant is unchanged, and it is still the only variable-length field.)
// The real gate is
// test_heartbeat_production_buffer_fits_full_payload, which saturates this
// field with control characters, not this comment.
#define HW_TELEMETRY_MP_END_REASON_SIZE 48u
#define HW_ANCHOR_FRAME_CAP 256u
/* Fixed, streamed storage; no allocation at readiness or over run duration. */
#define HW_ANCHOR_FRAME_BUDGET_BYTES 1024u
/* S1-d: per-owner accepted-action slots, in the wire's fixed order
 * [dig, place, attack, use, hotbar, drop]. MUST equal
 * SERVER_ACTION_COUNT (server_local_state.h); main.c copies one array into
 * the other and a mismatch there is a silent truncation, so the copy site
 * carries a _Static_assert tying the two together. This header cannot include
 * the server state header (it is platform code), hence the duplicated
 * literal plus the assertion rather than a shared constant. */
#define HW_ANCHOR_ACTION_COUNT 6u
#define HW_TELEMETRY_DUMP_LINE_SIZE 1000u
#define HW_TELEMETRY_DUMP_TEXT_CHUNK 620u
// Keep telemetry bring-up from hiding early boot freezes behind a long black
// screen. If DHCP does not answer quickly, CaveWii continues and the SD/watchdog
// proof paths remain available.
#define HW_TELEMETRY_DHCP_MAX_RETRIES 3

// Crash-report mailbox paths, shared by the writer (wii/watchdog.c wd_emit_dump)
// and the reader (hw_telemetry_emit_crash_mailbox). On Wii these MUST be absolute
// sd:/ paths: the HBC-launch CWD is the device root, so the old CWD-relative
// names only landed at sd:/ by accident, and the stage-server fetch
// (tools/wii_stage_server stage_manifest_paths.h) hardcodes the absolute paths.
// PC unit tests keep CWD-relative names.
// FALLBACK report paths (2026-07-27): the app directory on the same card. A root open can
// fail on its own -- a full FAT root directory, or a root-only write error -- while a
// subdirectory open still succeeds. They live HERE, beside the primaries, because a
// fallback nobody collects is not a fallback: the writer (wii/watchdog.c), the crash-
// mailbox replay (hw_telemetry_emit_crash_mailbox) and the stage-server fetch
// (tools/wii_stage_server stage_manifest_paths.h, tools/run_staged_wii_hardware.ps1) must
// all name the same two paths, and one #define per path is what keeps them in step.
#ifdef PLATFORM_WII
#define HW_TELEMETRY_HANG_REPORT_PATH "sd:/hang_report.txt"
#define HW_TELEMETRY_FBGUARD_REPORT_PATH "sd:/fbguard_report.txt"
#define HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH "sd:/apps/CavEX/hang_report.txt"
#define HW_TELEMETRY_FBGUARD_REPORT_FALLBACK_PATH                              \
	"sd:/apps/CavEX/fbguard_report.txt"
#else
#define HW_TELEMETRY_HANG_REPORT_PATH "hang_report.txt"
#define HW_TELEMETRY_FBGUARD_REPORT_PATH "fbguard_report.txt"
#define HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH "hang_report_fallback.txt"
#define HW_TELEMETRY_FBGUARD_REPORT_FALLBACK_PATH "fbguard_report_fallback.txt"
#endif

struct hw_telemetry_config {
	bool enabled;
	const char* run_id;
	const char* profile;
	const char* host;
	const char* netmask;
	int port;
	bool pull;
	/* Suppress temporary per-send SD probes in performance profiles. */
	bool suppress_sd_probes;
};

/* Main-thread owned, cumulative across presentation-ready frames. Never
 * reset at heartbeat boundaries: one dropped heartbeat cannot hide a hitch. */
struct hw_frame_evidence {
	uint32_t frames, max_us, over_30ms, gpu_over_16600, gpu_over_8000, invalid;
	uint64_t duration_us;
	uint32_t vi_ready_max_us, vi_ready_over_8000;
	uint32_t gpu_poll_late_max_us, gpu_poll_late_over_8000;
	uint32_t sd_trace_max_us, sd_trace_over_30ms;
	/* Same worst frame: tick, mesh receive, world draw, BFS, light, LOD,
	 * build, GPU wait, VI wait, client RPC update, screen update, all draw
	 * (includes mesh receive/world draw), heartbeat, render completion, input poll, draw finish, full-chunk RPCs, far-chunk RPCs, other RPCs,
	 * RPC queue overhead.
	 * Overlapping wall intervals, not additive CPU times. */
	uint32_t worst_parts_us[20];
};
void hw_frame_evidence_note_parts(struct hw_frame_evidence* s, bool ready,
	float frame_seconds, const float parts_ms[20]);
void hw_frame_evidence_note(struct hw_frame_evidence* s, bool ready,
	float frame_seconds, float gpu_seconds);

void hw_frame_evidence_note_trace(struct hw_frame_evidence* s, bool ready, float seconds);
void hw_frame_evidence_note_wait(struct hw_frame_evidence* s, bool ready,
	uint32_t vi_ready_us, uint32_t gpu_poll_late_us);

/* Post-load settle window for the release frame gate (maintainer ruling
 * 2026-10-31, option 2): the gate ignores the first 10 s of gameplay after
 * the world loads and the loading screen is gone (real-Wii runs only ever
 * failed on the first 1-2 heartbeats after load, 21-22 ms frames); every
 * later frame is still graded by the unchanged 55 fps / 30 ms / GPU rules.
 * Diagnostics only (T1): simulation and the other telemetry are unchanged. */
#define HW_FRAME_EVIDENCE_SETTLE_S 10.0f
struct hw_frame_settle {
	float playable_s; /* continuous admitted-gameplay time so far */
};
/* playable = this frame AND the previous one belong to admitted gameplay
 * (the caller's existing predicate). Returns true once that has held for
 * HW_FRAME_EVIDENCE_SETTLE_S; any non-playable frame restarts the window. */
bool hw_frame_settle_admit(struct hw_frame_settle* s, bool playable, float dt_s);

#define HW_ANCHOR_READY_HEARTBEATS 3

struct hw_anchor_evidence {
	/* guest: a real second body is admitted (split_play_*), so both anchors
	 * gate readiness even when `separation` (the synthetic offset) is 0. */
	bool enabled, ready, measuring, collision, guest;
	float actual_tps;
	uint32_t separation, elapsed_ms, near_owed[2], oldest[2];
	uint32_t missing[2], mesh_pending[2], far_missing[2], far_max[2];
	uint32_t far_mesh_pending[2], ready_streak;
	/* Cumulative totals at the loading boundary, then window-only deltas:
	 * OOM, spill, mesh truncation. Never reset on subsequent readiness loss. */
	uint32_t faults_load[3], faults_run[3];
	uint32_t deaths, walk_tick, walk_stage;
	bool walk_brake, walk_water, walk_capture;
	const char* death_cause;
	const char* damage_cause;
	uint32_t tick_backlog, frame_count, frame_dropped, frame_total;
	uint32_t frame_us[HW_ANCHOR_FRAME_CAP];
	int carve_load_min, carve_run_min;
	/* S1-d (mp-split-0912/68), emitted as "act0"/"act1": the two owners'
	 * ACCEPTED gameplay-action tallies in one fixed order --
	 * [dig, place, attack, use, hotbar, drop] -- read straight off
	 * server_local.actor_actions (see enum server_player_action for what each
	 * slot counts). [0] is the host body, [1] the admitted guest body.
	 * split_actions_6m FAILs unless the guest's counts clear their floors and
	 * the host's are all zero, which is what makes "the guest acted on its
	 * OWN actor while the host did nothing" a machine-checkable claim instead
	 * of an eyeball.
	 *
	 * `health` is the same pair for the two bodies' live health, because the
	 * profile's gate is per-OWNER and the long-standing `player_health`
	 * heartbeat field only ever carried owner 0. -1 is the no-body sentinel
	 * (the same one player_health uses), so "no guest yet" and "guest at zero
	 * health" stay distinguishable. */
	uint32_t actions[2][HW_ANCHOR_ACTION_COUNT];
	int health[2];
};

typedef char hw_anchor_frame_budget_check[
	sizeof(((struct hw_anchor_evidence*)0)->frame_us)
		<= HW_ANCHOR_FRAME_BUDGET_BYTES ? 1 : -1];
/* S1-d: 1216 -> 1280. The struct is a heartbeat-local payload AND a member of
 * hw_telemetry.c's per-record staging, so its size is a real Wii stack/MEM1
 * figure, not a cosmetic one -- hence a hard cap rather than a comment. The
 * act0/act1 arrays and the per-owner health pair add 2*6*4 + 2*4 = 56 B,
 * measured 1,184 -> 1,240 B on the PC build (the Wii build is smaller: its two
 * `const char*` cause fields are 4 B each, giving ~1,168). 1280 restores the
 * same ~40 B of slack the 1216 cap left before this field landed; a further
 * field must re-measure rather than assume. */
typedef char hw_anchor_evidence_budget_check[
	sizeof(struct hw_anchor_evidence) <= 1280 ? 1 : -1];

/* The caller owns the start timestamp; begin returns true only at its edge.
 * note streams full batches, retaining at most FRAME_CAP exact microseconds. */
bool hw_anchor_evidence_begin(struct hw_anchor_evidence* a, bool playable,
	const uint32_t faults[3]);
void hw_anchor_evidence_note(struct hw_anchor_evidence* a, uint32_t elapsed_ms,
	uint32_t frame_us, int carve_kb);

// Sender self-telemetry (2026-09-07 starvation fix). The Wii's telemetry
// sender is an LWP whose service rate cannot be observed from a host: this is
// how a console run proves whether the sender is being SCHEDULED at all.
// `iterations`/`sends`/`max_send_us` are WINDOWED -- reading them through
// hw_telemetry_sender_stats_take() resets them, so the heartbeat carries the
// last second. `occupancy_max`/`drops`/`hb_replaced` are monotonic ring
// counters. max_send_us is Wii-only (the PC test build has no IOS transport
// to time) and reads 0 elsewhere. Read once per heartbeat from the main
// thread; every counter is a RELAXED atomic, never control flow.
// sends counts attempted wire records. max_send_us times a dequeued unit:
// a guarded heartbeat includes its context; send_max_us below remains the
// longest individual SDK send. OFF still has one record per heartbeat unit.
struct hw_telemetry_sender_stats {
	uint32_t iterations;
	uint32_t sends;
	uint32_t max_send_us;
	uint32_t occupancy_max;
	uint32_t drops;
	uint32_t hb_replaced;
	/* Lines abandoned by the bounded-send deadline (2026-09-08), monotonic.
	 * A line is abandoned when the socket did not become writable inside
	 * HW_TELEMETRY_SEND_DEADLINE_MS, or when that deadline expired between
	 * two 1 KiB chunks of the same line. It is NOT a transport failure (no
	 * errno, never counted toward tel_fail or the transient quiesce
	 * strikes): the link is alive, this one diagnostic line was too
	 * expensive to wait for. A non-zero tel_wto with a live heartbeat is
	 * exactly the evidence that the deadline did its job. 2026-09-26: an
	 * evidence line (hw_telemetry_line_is_critical) is never abandoned
	 * mid-line; each of its timed-out pre-byte polls (up to
	 * HW_TELEMETRY_CRITICAL_POLL_TRIES) counts here once. */
	uint32_t write_timeouts;
	/* STAGE INSTRUMENTATION (2026-09-08, sender-wedge lane). The 2026-09-07
	 * split_anchor_6m deaths proved the sender stops iterating for 4-5 s
	 * (tel_it 0, tel_sends 0) while net_poll's own 250 ms timeout keeps
	 * firing (tel_wto climbing) -- i.e. it is parked inside SOME call, and
	 * no counter said WHICH. `stage` is the call the sender was in at the
	 * moment this heartbeat was composed (enum hw_telemetry_sender_stage),
	 * and `stage_us` is how long it had already been there. Because the
	 * heartbeat rides its own priority lane and does get through every few
	 * seconds even mid-wedge, these two fields name the stuck call from the
	 * collector's side, live.
	 *
	 * `stage_us` is derived from a millisecond stamp (the sender publishes
	 * stage and entry time as ONE 32-bit atomic so they can never tear --
	 * 3 bits of stage, 29 bits of ms), so it is exact to 1 ms and reported
	 * in us for uniformity with the other _us fields. It is a LIVE gauge,
	 * not a window: it is not reset by the take. */
	uint32_t stage;
	uint32_t stage_us;
	/* WINDOWED per-stage maxima, reset by hw_telemetry_sender_stats_take()
	 * like iterations/sends/max_send_us. These separate the two IOS calls
	 * that max_send_us lumps together: max_send_us times a whole RECORD
	 * (poll + every chunk + framing), poll_max_us times the single longest
	 * net_poll, and send_max_us the single longest net_send. A wedge inside
	 * one syscall shows here even when the record-level number is diluted.
	 * Both are measured on PC too (the test build drives the same
	 * send_line_with/send_bytes_all path through a stub transport). */
	uint32_t poll_max_us;
	uint32_t send_max_us;
	/* Lines abandoned by the -TelemetryNonBlock experiment (monotonic):
	 * a short write or an -EAGAIN/-EWOULDBLOCK from a non-blocking socket.
	 * Distinct from write_timeouts, which counts the bounded-send DEADLINE's
	 * abandonments -- the two mechanisms must stay separately countable or a
	 * run cannot tell "the link had no room right now" from "the sender
	 * waited out its budget". Always 0 unless the experiment is armed. */
	uint32_t abandoned;
	/* Last raw transport results the sender saw, for the "which call, and
	 * what did it say?" question the stage fields raise. `last_poll_ret` is
	 * net_poll's return (>0 ready, 0 timeout, <0 IOS error), `last_poll_revents`
	 * the pollsd revents it filled in (POLLOUT/POLLERR/POLLHUP/POLLNVAL, from
	 * libogc's poll.h) -- Wii-only, 0 on PC where there is no pollsd --
	 * and `last_send_ret` net_send's return, INCLUDING the negative errno
	 * that libogc may return after having queued a partial (network_wii.c's
	 * send loop discards earlier progress on error). Live gauges, not
	 * windowed. */
	int32_t last_poll_ret;
	uint32_t last_poll_revents;
	int32_t last_send_ret;
};

// Which call the telemetry sender thread was inside. Published by the sender
// on every transition and folded into the heartbeat as tel_stg. Ordered
// cheapest-to-most-diagnostic; the wire value is the enum ordinal, so entries
// may be APPENDED but never reordered (a collector reading an older run's
// ndjson must keep resolving the same names).
enum hw_telemetry_sender_stage {
	HW_TEL_STAGE_IDLE = 0,     /* bottom-of-loop sleep, nothing to send */
	HW_TEL_STAGE_POLL = 1,     /* inside net_poll (the writability gate) */
	HW_TEL_STAGE_SEND = 2,     /* inside net_send (one <=1 KiB chunk) */
	HW_TEL_STAGE_ACCEPT = 3,   /* inside net_accept (pull-mode client) */
	HW_TEL_STAGE_CLOSE = 4,    /* inside net_close (shutdown) */
	HW_TEL_STAGE_RING_WAIT = 5,/* transient back-pressure pacing sleep */
	HW_TEL_STAGE_OTHER = 6,    /* formatting, ring bookkeeping */
	/* 2026-09-08 fix round: LOCK was folded into OTHER, so a park in
	 * send_lock_enter's blocking LWP_MutexLock (reached before any stage
	 * is entered) reported whatever stage the PREVIOUS record left behind,
	 * with that stage's stale entry timestamp -- an ever-growing dwell on
	 * a stage the sender was not actually in. Split out so a mutex park
	 * names itself. Still 3 bits (0-7 fit; see CW_TEL_STAGE_SHIFT). */
	HW_TEL_STAGE_LOCK = 7,     /* inside send_lock_enter's blocking mutex wait */
	HW_TEL_STAGE_COUNT = 8
};

/* colcap-1006 (ASTRA-CC-R3 item 2): the physical column pool and the A2-T
 * transfer budget, extracted so split_anchor_* heartbeats can carry them
 * without arming the split_dims scenario block. Physical column pool,
 * distinct from server dictionary residency and shared backing references.
 * All pool fields are zero on PC (no MEM2 pool). */
struct hw_pool_budget_evidence {
	uint32_t column_pool_used, column_pool_peak, column_pool_slabs;
	uint32_t column_pool_overflow, column_pool_carve_fail;
	bool budget_valid, budget_wii;
	uint32_t budget_resident, budget_live_backings, budget_reserved;
	uint32_t budget_prepeak, budget_peak, budget_waits;
	uint8_t budget_phase;
};

/* split_dims_6m only (enabled = that profile): what each split VIEW shows.
 * dim_v0 is player 0's view, dim_v1 the guest's (-128 = no guest view);
 * views_live counts views with an admitted body; dims_active = both views
 * live AND they show different dimensions -- the one fact the profile
 * certifies. Every other profile leaves it disabled, byte-identical. */
struct hw_split_dims_evidence {
	bool enabled;
	int dim_v0, dim_v1;
	uint32_t views_live;
	bool dims_active;
	/* The dimension the SERVER simulates each body in (Fable F1): host, and
	 * guest (-128 = no bound guest). */
	int srv_dim_host, srv_dim_guest;
	/* The guest walk's portal step (main.c split_dims_portal_step):
	 * "pane_wait" (walk released until the lit pane is seen ahead), "seek"
	 * (walking to it), "hold" (standing in it, walk released), "across" (the
	 * views diverged; the guest walks on the other side), "timeout" (no pane
	 * or no crossing in time; the guest walks on in the Overworld). NULL
	 * formats as "pane_wait". Host-switch run (splitdims-hostswitch-0927):
	 * "clear" (the guest walks out of the frame around the host first) and
	 * "host_trip" (walk released while the host goes to the Nether and back)
	 * come before "pane_wait". */
	const char* phase;
	/* The host switch (main.c split_dims_host_step): "wait" (host frame lit
	 * or pending), "away" (host view in the Nether), "back" (host view home,
	 * waiting for the vacated world to drain and be released), "done",
	 * "timeout". NULL formats as "wait". */
	const char* host_phase;
	uint32_t host_commits;	/* server: host transfer COMMITs */
	int slot1_cols;			/* server: world slot 1 resident columns, -1 = not LIVE */
	uint32_t rel_sent;		/* server: CRPC_WORLD_RELEASE admitted by the drain */
	uint32_t rel_rx;		/* client: releases APPLIED */
	int rel_slot;			/* client: last applied release slot, -1 = none */
	int wslot_v0, wslot_v1; /* client: world slot each view is bound to, -1 = none */
	/* Physical column pool + transfer budget (shared with the anchor pool
	 * block below; formatted at the same position, byte-identical). */
	struct hw_pool_budget_evidence pb;
	bool transfer_held, transfer_source_draining, transfer_restoring;
	uint32_t transfer_request;
	uint32_t guest_commits;
	uint32_t host_rb_id, guest_rb_id; /* sticky closed-request diagnostics */
	uint8_t transfer_actor, transfer_state;
	uint8_t host_rb_reason, guest_rb_reason; /* server_transfer_rollback_reason */
};

/* colcap-1006 (ASTRA-CC-R3 item 2): the anchor profiles' memory evidence,
 * WITHOUT the split_dims scenario block. enabled = the anchor observer is
 * armed. Emitted as "cpv":1 plus, when the dims block is NOT also present
 * (it already carries `pb` under the same keys), the shared pool/budget
 * fields. */
#define HW_POOL_SLAB_REPORT_MAX 16u /* = MEM2_POOL_MAX_SLABS (main.c asserts) */
#define HW_DLO_FIELDS 12u
#define HW_PAC_FIELDS 9u /* = MEM2_COST_FIELDS (main.c asserts) */
struct hw_anchor_pool_evidence {
	bool enabled;
	struct hw_pool_budget_evidence pb;
	/* colcap-1006 (ASTRA-CC-R3 item 3, "cpv":2): live columns per carved
	 * slab ("cps", carve order; boot slabs first) sampled with pb's column
	 * fields in one lock acquisition, and the resident display lists'
	 * count + capacity bytes ("dll"/"dlb"). Telemetry only. */
	uint32_t slab_count;
	uint8_t slab_used[HW_POOL_SLAB_REPORT_MAX];
	uint32_t dl_lists, dl_bytes;
	/* colcap-1006 (R4 points 9-10, "cpv":3) "cpe":[seq,shrinks,shrink_fail,
	 * lost]: the seq of the last QUEUED pool event at this snapshot (taken
	 * under the same lock as cps), cumulative slabs handed back, hand-backs
	 * the heap refused, and events the queue could not hold. */
	uint32_t event_seq, shrinks, shrink_fail, events_lost;
	/* colcap-1006 (ASTRA-CC-R4 point 7) "dlo": display-list bytes by owner,
	 * read-only, emitted on non-dims anchor heartbeats:
	 * [view-0 world installed bytes, lists, view-1 world (second distinct
	 *  world; 0 if both views share one) installed bytes, lists,
	 *  GPU-pinned evicted bytes, lists, completed-unreceived bytes, lists,
	 *  installed age <5 s, <30 s, >=30 s bytes, own-allocation lists].
	 * dlb - (installed + pinned + completed) is the remainder (worker-held,
	 * other owners); the collector reports it as such. */
	bool dlo_valid;
	uint32_t dlo[HW_DLO_FIELDS];
	/* colcap-1006 (ASTRA-CC-R5 item 3) "pac": allocator CPU/lock cost since
	 * the previous heartbeat (mem2_cost_take, mem2.h): [column allocs, us,
	 * max us, column frees, us, max us, contended lock waits, us, max us].
	 * Emitted beside "dlo" (never beside the dims block). */
	bool pac_valid;
	uint32_t pac[HW_PAC_FIELDS];
};

struct hw_telemetry_heartbeat {
	struct g9_priority_lease g9_lease; /* ON only; rides the priority heartbeat lane. */
	struct tps_cpu_evidence cpu;
	struct hw_split_dims_evidence dims;
	struct hw_anchor_evidence anchor;
	struct hw_anchor_pool_evidence pool;
	uint32_t tnt_valid, tnt_st, tnt_bits, tnt_primed_left;
	uint32_t tnt_carved, tnt_pad, tnt_gen, tnt_tick;
	/* mob_motion_2m fixture verdict (server_local_mob_motion_evidence) and the
	 * mob-move cost counters of the last window (mob_motion_cost). Formatted
	 * only when mm_valid or mm_moves is nonzero, so a run without either keeps
	 * the legacy wire bytes. */
	uint32_t mm_valid, mm_st, mm_bits, mm_recur_ok, mm_recur_bad;
	uint32_t mm_rise_milli, mm_step_milli, mm_gen, mm_tick;
	uint32_t mm_moves, mm_move_us, mm_env_walks, mm_env_ents;
	/* residency-1005/2: per-actor residency rows (server_local.h enum
	 * server_local_rza_field, same order; row 0 host, row 1 guest), emitted
	 * as "rza":[[..],[..]] only when either body refused, recovered or
	 * requested anything, so a run without residency activity keeps its
	 * legacy wire bytes. */
	uint32_t rza[2][HW_TELEMETRY_RZA_FIELDS];
	uint32_t rze[2][8]; /* slot, gen, epoch, failed attempt/ticks, retry, ok/ticks */
	uint32_t rz_tick; /* "rzt": the server tick of the rza snapshot */
	uint32_t rzi[HW_TELEMETRY_RZI_MAX][HW_TELEMETRY_RZI_FIELDS];
	uint32_t rzi_count; /* "rzi": induced-attempt marks (closure item 12) */
	uint32_t rzf;		/* "rzf": runtime fault phases seen (item 12) */
	/* Wire order: PREP, SIM, SAVE, EVICT, SCAN, GEN, SLOT, COPY, SEND,
	 * FAR, TAIL. main.c checks this schema against the server profiler. */
	uint32_t server_work_us[11];
	/* LOAD, TERRAIN, DECORATE, VERTICAL_LIGHT, BLOCK_LIGHT, SKY_BLEED,
	 * SAVE, REARM, REGION_READ, PARSE, GET, COMPRESS, COMMIT, SYNC.
	 * Nested cumulative elapsed microseconds, not additive CPU times. */
	uint32_t world_work_us[14];
	uint32_t guard_scan_us[2]; /* startup real-XFB byte/word scan averages */
	struct hw_frame_evidence frame_evidence;
	int far_storage;
	uint32_t server_time; /* Simulation clock increments; never inferred from client time. */
	const char* build_id;
	uint32_t tick;
	float fps;
	/* Emit window FPS as "wf" instead of legacy "fps". The shorter key keeps
	 * the maximal payload's 200-byte spare floor; collectors accept both.
	 * Unset preserves legacy formatter callers and their exact wire bytes. */
	bool fps_window_present;
	float gpu_ms;
	float vsync_ms;
	float ms_bfs;
	float ms_light;
	float ms_lod;
	float ms_build;
	float ms_recv;
	float ms_render;
	float ms_tick;
	float ms_bfs_max;
	float ms_light_max;
	float ms_lod_max;
	float ms_build_max;
	int chunks_rendered;
	int mesh_pending;
	uint32_t mesh_stale;
	uint32_t mesh_trunc;
	uint32_t evict_stuck;
	uint32_t near_evict_defer;
	int srv_rt_us;
	int srv_wt_us;
	int heap_used_kb;
	uint32_t item_nbt_exhaustions;
	int heap_free_kb;
	// Heap-triage discriminators (newlib mallinfo; Wii fills them, PC sends -1).
	// Read the trio with heap_free_kb: free HIGH + heap_blocks MANY + heap_top_kb
	// SMALL = FRAGMENTED (the space exists but cannot be placed; cutting
	// allocations will not help). free LOW = EXHAUSTED (cut what is allocated).
	// Healthy free but allocs failing = WRONG ARENA (check mem1/mem2_free_kb
	// against the MALLOC_MEM2 pin in mem2.c). Triage taxonomy re-derived from
	// the BetaPlusPlus Wii port's heap tracing (reference only, no code copied;
	// docs/mcbetacpp_source_2026-08-13.md).
	int heap_blocks;  // mallinfo ordblks: how many pieces the free space is in
	int heap_top_kb;  // mallinfo keepcost: the top chunk, the largest cheap grow
	int mem1_free_kb;
	int mem2_free_kb;
	uint32_t mem2_spill;
	uint32_t mem2_oom;
	uint32_t dlbad;
	uint32_t dlcall;
	// Wiimote/BT liveness (wiimote-dead-direct-launch 2026-08-18): raw
	// WPAD_Probe(chan 0) return. NOT_READY(-2)-forever = BT stack stuck in
	// its async enable (libogc-side); NO_CONTROLLER(-1) = stack healthy,
	// nothing paged in; 0 = a controller is attached. PC sends -99.
	int wpad_probe0;
	// lwp kernel workspace free bytes (Wii; PC -1): wiiuse's attach-time
	// queue pool -- the cornered Wiimote-accept suspect (2026-08-23).
	int wkspace_free;
	// pool total / used bytes and free-block count (fragmentation): the
	// 128KB round showed used GROWING when room appeared -- these three
	// identify the consumer and whether free space is contiguous.
	int wkspace_total;
	int wkspace_used;
	int wkspace_free_blocks;
	float x;
	float y;
	float z;
	float rx_deg;
	float ry_deg;
	// Guest (player 2) body position while a guest body exists; emitted as
	// gx/gz only then, so a split harness run can PROVE the guest moved
	// (2026-09-11 maintainer eyeball: the forced guest walk stood still).
	bool guest;
	float gx, gz;
	int near_radius;
	int far_view_distance;
	// far_vd - MAX_VIEW_DISTANCE: the far-ring annulus width, 0 = ring OFF.
	// The explicit ring-active signal for the collector's far-profile scenario
	// gate -- far_view_distance alone cannot express "off" because main.c
	// clamps it to >= MAX_VIEW_DISTANCE (5 on Wii) even when the ring is
	// disabled, and the near_radius field is the SKIN radius, not the ring
	// threshold.
	int far_ring_width;
	// Far-ring keep-up probe (2026-09-03, reach-sweep gate 4: "worldgen keeps
	// up while walking"). far_missing is a THROTTLED SCAN (main.c's
	// emit_hw_heartbeat, at the same <=1 Hz cadence as every other heartbeat
	// field -- NOT a per-frame cost) over the far annulus
	// (MAX_VIEW_DISTANCE < Chebyshev distance <= far_view_distance) counting
	// how many of those section slots the CLIENT has not yet received/built
	// (world_far_missing_count, world.c): O(1) dict_wsection lookups per
	// candidate slot, bounded by the annulus size (<=408 slots at the
	// reach-11 cap). A true receive/evict-ONLY incremental counter was
	// considered and rejected: the annulus recenters continuously with the
	// player's exact block position (server_local.c tracks px/pz every
	// tick, not just on chunk crossings) and the client has no visibility
	// into the server's far_sent membership set to diff against -- only
	// into what it has itself built/evicted -- so an incremental counter
	// would either drift or have to duplicate the server's own eviction
	// bookkeeping. far_missing_max is the running max of far_missing since
	// the world was (last) loaded (source/game/far_ring_keepup.h). 0/0 when
	// no world is loaded or the far ring is inactive (fring 0).
	int far_missing;
	int far_missing_max;
	// Time (ms) from the last detected player chunk-crossing until
	// far_missing next returned to 0, running max since world load --
	// the direct "does worldgen keep up while walking" number
	// (source/game/far_ring_keepup.h). 0 until the first crossing settles.
	uint32_t far_settle_ms;
	// Server-side far-skin generation cost, sampled once per SERVER tick
	// (single-tick sample like srv_rt_us/srv_wt_us above -- NOT cumulative
	// across the run) from the far-send batch loop
	// (source/network/server_local.c, the FAR_SEND_BUDGET loop around
	// :16771-16800) and server_world_extract_far_skin
	// (source/network/server_world.c:3491). srv_gen_us = microseconds spent
	// this tick generating+extracting far-skin columns; srv_gen_cols =
	// columns actually generated this tick (<= FAR_SEND_BUDGET);
	// srv_gen_backlog = far annulus slots still neither far_sent nor
	// chunk_sent, sampled once per tick (the first far-send scan of the
	// annulus, reused rather than re-scanned) -- the server's own view of
	// the send-queue depth the far_missing/far_settle_ms pair measures from
	// the client side.
	int srv_gen_us;
	int srv_gen_cols;
	int srv_gen_backlog;
	// Server-loop wedge instrumentation (2026-09-08, lane/carve-observer-boot):
	// real-Wii split_anchor_6m evidence showed wd_srv frozen at WDS_IDLE for
	// 400+ heartbeats after a single 109.6s FAR-phase stretch, and could not
	// distinguish "sleeping in thread_msleep" from "looping with 0 ticks" from
	// "blocked inside FAR" -- see the new WDS_IDLE_SAMPLE/WDS_IDLE_SLEEP/
	// WDS_FAR_EXTRACT/WDS_FAR_POPULATE/WDS_REGION_IO phases (watchdog.h) this
	// same lane adds. These six are the direct counter-evidence alongside
	// them, published by the SERVER thread with relaxed atomics
	// (server_local.c) and read by main with matching relaxed atomics.
	// srv_loop_it/srv_far_us_max are windowed over the trailing ~1s (the
	// heartbeat's own <=1Hz cadence); the other four are the LAST sampled
	// value, republished every server-loop iteration.
	//
	//   srv_loop_it     -- server-loop iterations completed in the last ~1s
	//                      window. A monotonic counter that keeps climbing
	//                      rules out BOTH "stuck forever asleep" and "wedged
	//                      inside FAR"; a flat counter across two heartbeats
	//                      means the loop itself has not returned in that
	//                      window, however wd_srv happens to be tagged.
	//   srv_elapsed_ms  -- the last `elapsed` value passed to
	//                      server_tick_clock_advance (int32_t; can be
	//                      negative -- a monotonic clock is assumed but not
	//                      guaranteed by every backend).
	//   srv_ticks_pend  -- the last ticks_pending value computed this window
	//                      (mirrors g_srv_tick_backlog's live value, sampled
	//                      at the same point).
	//   srv_partial_m   -- clock.partial_ticks * 1000, truncated to an
	//                      integer (server_tick_pacing.h's fractional-tick
	//                      accumulator; always in [0, 1000) once advanced).
	//   srv_sleep_ms    -- the last argument actually passed to
	//                      thread_msleep by this thread (either
	//                      server_tick_clock_wait_ms's result on the
	//                      ticks_pending==0 path, or the end-of-tick
	//                      sleep_ms/SERVER_TICK_OVERRUN_SLEEP_MS). Bounds how
	//                      long a WDS_IDLE_SLEEP-tagged report can have been
	//                      asleep, as opposed to "asleep forever".
	//   srv_far_us_max  -- the longest single far-column gen+extract call
	//                      (server_world_extract_far_skin /
	//                      server_world_ensure_populated_far) in the last
	//                      ~1s window, sampled unconditionally (not just on
	//                      success) -- the direct per-column analogue of the
	//                      109.6s single-column stretch the real-Wii evidence
	//                      measured only as a per-TICK total (srv_gen_us).
	uint32_t srv_loop_it;
	int srv_elapsed_ms;
	uint32_t srv_ticks_pend;
	int srv_partial_m;
	int srv_sleep_ms;
	int srv_far_us_max;
	// --cw-set key=value applied-override count (0..HW_RUN_CW_SET_MAX,
	// hw_run.h; run-scoped config overrides for staged hardware runs,
	// 2026-09-08, lane/carve-observer-boot). main.c copies hw_run.cw_set_n
	// here once at boot (see s_cw_set_n_applied) so a paired A/B run (e.g.
	// "memory.displaylist_compact" 0 vs 1) can confirm from telemetry alone
	// that the override actually applied, without a hand-inspected boot
	// log. Emitted unconditionally (unlike dlc below, which only appears
	// when nonzero) -- 0 is itself informative here (no override armed).
	uint32_t cw_set_n;
	// lworld_ready_timeout_admits() (source/game/gui/screen_load_world.h):
	// the CUMULATIVE, process-lifetime count of gameplay admissions that had
	// to fall back to the loading screen's 30 s backstop with in-radius
	// obligations still outstanding. Never reset -- not by a presentation
	// reset, not by a dimension change, not by a rejoin -- which is exactly
	// what makes "zero throughout the run, including the rejoin" a provable
	// claim rather than a sample (Astra gate 1 Lane B item 2: "Export the
	// cumulative timeout counter without resetting away failures. Require a
	// zero baseline and zero throughout each fresh-process proof run,
	// including rejoin. Missing telemetry fails acceptance.").
	//
	// Emitted UNCONDITIONALLY on every heartbeat of every profile, like
	// cw_set_n above and for a stronger reason: the collector FAILS an
	// mp_play_10m / mp_lan_walk_10m run whose heartbeat is missing the key,
	// so a profile-gated field would turn "the counter was never published"
	// into an indistinguishable "the counter was zero". 0 is the value a
	// healthy run reports from the first heartbeat to the last.
	uint32_t lw_to_admits;
	uint32_t cflood;
	// REL-MP-FIX3b: cumulative remote edits dropped because id+meta were
	// unchanged (world_remote_unchanged_skips). Emitted as "rskip" ONLY when
	// non-zero: it can only grow in a remote session, never beside the
	// split/dims/anchor blocks, so the local worst-case lines keep their bytes.
	uint32_t rskip;
	// REL-MP-FIX3d: cumulative remote relight seeds that found no reservation
	// slot (world_remote_relight_no_slots; kept on the legacy stack, nothing
	// dropped). Emitted as "rns" ONLY when non-zero, like rskip; a healthy
	// remote run keeps it 0, so its absence is the pass value.
	uint32_t rns;
	// LIGHT-CLIENT-LOSSLESS (hw1023a Finding 2): the CLIENT render relight's own
	// counters, split from lq[24]/lq[25] (server floods / process-wide stack
	// drops) so a client fault is never graded as a server-queue fault.
	// Emitted as "clq":[cfceil,coom,cpark,crep] ONLY when any is non-zero (the
	// rskip rule: worst-case lines keep their bytes; absence = all zero).
	// cfceil: client floods parked at the 500k per-flood ceiling; coom: client
	// frontier/seed/edit-queue grows that failed (work moved to a column
	// repair, nothing dropped); cpark: columns waiting for repair now (upper
	// bound); crep: columns repaired to the fixed point since boot.
	uint32_t clq[4];
	// REL-MP-COREFIRST (2026-10-22): the remote loading gate's safe-core
	// gauges, emitted as "lwc":[core_missing,core_mesh_pending,
	// core_resident_ms,core_meshed_ms] ONLY when lwc_present, which main.c
	// sets only in a REMOTE session -- local and split/dims/anchor lines keep
	// their exact bytes (the rskip rule). core_missing / core_mesh_pending:
	// player 0's radius-1 (3x3) core RIGHT NOW (world_near_ring_status, the
	// same instrument lworld_prepare_presentation gates on). core_resident_ms /
	// core_meshed_ms: ms after the current/last loading screen appeared at
	// which the core was first fully installed / installed AND meshed (0 = not
	// yet this episode); same clock as load_screen_ms, so meshed vs
	// load_screen_ms separates "core ready" from "admitted".
	bool lwc_present;
	uint32_t lwc[4];
	// nether-dark/1 (2026-10-30): client vs server block-light census,
	// emitted as "blc":[dim,cols,shared,client_lit,server_lit,age_s,us] ONLY
	// when blc_present (set by main.c once a census has run; sampled at most
	// once per 5 s over player 0's 3x3 near columns of its CURRENT dimension;
	// absent = no census yet or a split-dims line, so older lines keep their
	// bytes). dim: world
	// dimension (-1 Nether). cols: near columns resident in the client world.
	// shared: those backed by a local server column view (local authority).
	// client_lit: client cells with BlockLight > 0 (what the mesher reads).
	// server_lit: cells with BlockLight > 0 in the server's backing arrays of
	// the SHARED columns only (remote-session columns have no server copy).
	// age_s: seconds since that census. us: its main-thread cost.
	bool blc_present;
	int32_t blc_dim;
	uint32_t blc[6];
	// hw1030n (2026-10-31): the new-world spawn search, emitted as
	// "ssw":[probes,elapsed_ms] ONLY while main waits on it (ssw_present), so
	// every other line keeps its bytes. probes: chunks the Beta walk has
	// loaded so far; elapsed_ms: since the wait began.
	bool ssw_present;
	uint32_t ssw[2];
	// G9 R6 (ASTRA-G9-REVIEW-2 item 5): the server pending-light queue as
	// ONE positional array of HW_TELEMETRY_LQ_N uint16 values, each
	// SATURATING at 65535 (memory in KiB rounded UP, so any allocation
	// reads >= 1; times in us; counts). Layout version 2 (26 entries; the
	// 13-entry v1 array of the earlier G9 round is still accepted by the
	// collector for those older artifacts):
	//   0/1 seed_kib  2/3 undo_kib  4/5 pin_kib  -- per slot, peak ALLOCATED
	//       capacity of each queue array
	//   6/7 front_kib  peak drain-frontier (flood scratch), stopped floods
	//       included
	//   8/9 ckpt_kib   peak settled-trial checkpoint snapshot
	//   10/11 seeds_pk  12/13 drain_us  14/15 drain_cells
	//   16/17 debt     seeds pending right after a prescribed drain
	//   18 tick_us (largest per-update drain + trial sum) 19 ckpt_us
	//   20 fault 21 ceil 22 oom 23 ckpt_fail 24 fceil 25 stk_drop
	// The collector FAILs a run whose debt/fault/ceil/oom/ckpt_fail/fceil/
	// stk_drop is non-zero, and (with --require-g9-evidence) one whose
	// heartbeat lacks a well-formed v2 array (tools/wii_telemetry_server.py
	// LQ2_FIELDS).
#define HW_TELEMETRY_LQ_N 26
	uint16_t lq[HW_TELEMETRY_LQ_N];
	// I07 drain telemetry (ASTRA-G9-DRAIN-CAP item 4): the UNSATURATED
	// per-drain record, as ONE positional array of HW_TELEMETRY_LQD_N uint32
	// values. The lq v2 array above saturates at 65535 and keeps run maxima,
	// so a 0.2-1.1 s Wii drain read as "65535 cells / 65535 us" there with no
	// attribution; this array reports, per heartbeat, the heaviest drain
	// COMPLETED SINCE THE PREVIOUS HEARTBEAT (a window, so each spike tick is
	// attributed to its heartbeat) with its work and its preemption split
	// (server_world.h struct server_world_light_drain_rec). Layout v1, per
	// world slot k = 0 (Overworld) / 1 (Nether):
	//   0/1 seeds  2/3 pops  4/5 depth_peak (frontier entries)
	//   6/7 us (wall)  8/9 slice_min_us  10/11 slice_max_us
	//   12/13 drains (monotonic count)
	// Layout v2 (ASTRA-G9-R5 points 9-10, 22 entries; a 14-entry v1 array
	// is accepted by the collector for the g9_1 tape), all per heartbeat
	// WINDOW and unsaturated:
	//   14 srv_rt_max_us  15 srv_wt_max_us -- the window MAXIMA of the
	//      srv_rt_us / srv_wt_us component timings (those two keys stay the
	//      last-tick sample); checkpoint ticks included; the collector's
	//      perf_stress_5m 50,000 us gate reads these (and still the
	//      samples), so every tick is graded, not the last one per window
	//   16 ckpt_us  17 ckpt_snap_us  18 ckpt_drain_us  19 ckpt_save_us
	//   20 ckpt_restore_us -- the settled-trial checkpoint split (window
	//      maxima; temporary attribution, order 3)
	//   21 rpc_wait_us (window SUM of the server-side clin_rpc_send
	//      token-pool waits; the longest wait and the count stay in the
	//      F3/test getter clin_rpc_wait_take -- the 13,312 B line keeps its
	//      200 B floor only with 22 entries: dims worst case 13,105)
	// Explicit lqd_v=3 distinguishes old tapes; layout v3 reuses the 22-entry budget; 12=3 (version), 13=0.
	// 0/1 yield count, 2/3 pops, 4/5 recovery sleep us, 6/7 total us,
	// 8/9 yield-call us, 10/11 mutex wait us. Same winning drain per world.
	// 14..21 unchanged. All elapsed; residual includes other preemption.
#define HW_TELEMETRY_LQD_N 22
	uint32_t lqd[HW_TELEMETRY_LQD_N];
	bool g9p_present;
	uint32_t g9p[22]; /* g9p_v=1: thread.h THREAD_PACING_WORDS layout */
	// Monotonic neighbour-notify handler dispatches since boot, summed over BOTH
	// dispatch paths (server_world_notify_block + blocks.c notifyNeighbours). The
	// collector's redstone_clock_2m scenario gate requires this to grow -- it is
	// the positive signal that the clock actually ran, not just that the console
	// idled for the profile duration.
	uint32_t notify_dispatch_total;
	uint32_t wd_main;
	uint32_t wd_srv;
	uint32_t wd_worker;
	uint32_t wd_hb_gap_max_ms;
	bool world_loaded;
	// menus_dwell scenario evidence (source/game/menu_autopilot.h). max_step is
	// the monotonic high-water mark of script steps reached this run -- the
	// collector's menus scenario gate requires it to reach
	// menu_ap_script_step_required_for_pass() (title, texture-packs, select-
	// world, create-world's OSK, pause, and options all round-tripped for
	// real). sign_reached is BONUS evidence only (never gates PASS/FAIL) --
	// see menu_autopilot_script.h for why the sign step is soft. Both are 0/
	// false on every other profile (menu_autopilot_step()/_max_step()/
	// _sign_reached() all return the not-armed default).
	int menu_ap_step;
	int menu_ap_max_step;
	bool menu_ap_sign_reached;
	// WALKER PROOF FIELDS (push-0921 A11, review A9-3). max_step alone is a
	// HIGH-WATER MARK: the no-pointer leg reaches its terminal step whether it
	// ENTERED all seven of its targets or named all seven as skips, so a PASS
	// gated on max_step alone proves nothing about reachability. These carry
	// what it actually did. One bit per sub-leg, MENU_AP_NP_BIT(enum
	// menu_ap_sub) -- the table lives in source/game/menu_autopilot_script.h
	// and is mirrored in tools/wii_telemetry_server.py:
	//   0 Select World Create   1 Rename   2 Delete
	//   3 Multiplayer Direct    4 Add      5 Edit    6 player Name
	// uint8_t, not unsigned: seven bits by construction, so the widest line
	// costs 3 digits each instead of 10. Two _Static_asserts in
	// source/game/menu_autopilot.c (NOT in main.c -- corrected, review A16-8)
	// fail the build if a ninth sub-leg or a ninth np_flags bit ever appears,
	// either of which would truncate SILENTLY into a clean-looking tape.
	uint8_t np_entered;	 // entered AND left again
	uint8_t np_skipped;	 // named skip: row-gated, OSK-backed, or unobserved
	uint8_t np_focus;	 // the ring OBSERVABLY rested on the target
	uint8_t np_flags;	 // MENU_AP_NP_FLAG_*; zero is the only clean console run
	int np_rows;		 // the latched Select World row count (0 = none taken)
	// save_migrate production-path evidence. smig_state reaches 4 only after
	// the reopened world finishes streaming and every sign still names its
	// recorded supporting wall (0 idle, 1 fixture, 2 migrate/reload, 3 verify,
	// 4 pass, 5 fail).
	uint32_t save_migrate_state;
	uint32_t save_migrate_ms;
	uint32_t save_migrate_regions;
	uint32_t save_migrate_chunks;
	uint32_t save_migrate_signs;
	uint32_t save_migrate_expected;
	uint32_t save_migrate_verified;
	// save_migrate chain-link evidence (v3 iron-chest / v4 spawner-TE):
	// added alongside the v2 sign fields above so the SAME profile certifies
	// all three CaveWiiSaveVersion steps the fixture now exercises, not just
	// the first.
	uint32_t save_migrate_chests_rewritten;
	uint32_t save_migrate_chests_expected;
	uint32_t save_migrate_chests_verified;
	// lava-aabb-0924/H1 (far_walk_6m autosave-window hang probe): chunk-save
	// FAILURE visibility. Until now a failed save was only a stderr line
	// ("[region_save] result=fail ... stage=") that the .dol cannot show, so a
	// run whose journal publishes stopped advancing could not be told apart
	// from a run with nothing to write. save_fail_n = process-lifetime count
	// of failed chunk saves; save_fail_stage = enum region_save_fail_stage of
	// the LAST failure (region_archive.h); save_fail_sub = for an
	// entity_preserve failure, enum chunk_entities_preserve_fail (which
	// fail-closed exit; entity_archive.h), else 0; save_skip_n = saves that
	// were a whole-record no-op (publishes flat BY DESIGN). All four are
	// unconditional keys: a healthy run is a visible 0/0/0/N row.
	uint32_t save_fail_n;
	uint32_t save_fail_stage;
	uint32_t save_fail_sub;
	uint32_t save_skip_n;
	// H2 review fix (4): merges whose owner-index scratch malloc failed and
	// fell back to the linear scan (chunk_entities_preserve_index_fallbacks).
	uint32_t save_idx_fb;
	uint32_t save_migrate_spawners_synthesized;
	uint32_t save_migrate_spawners_expected;
	uint32_t save_migrate_spawners_verified;
	// MP world-install probe (S5.3 take-49 follow-up, 2026-08-29): the wire
	// layer logs in but `chunks` stays 0 on Wii -- these name which stage of
	// login-CRPC -> world-reset -> chunk-promotion is stuck. Filled from
	// mp_session_remote()'s status + gstate; all zero when no MP session.
	uint32_t mp_state;        // enum server_remote_state
	uint32_t mp_gen;          // connect generation
	uint32_t mp_logged_in;    // status.logged_in
	uint32_t mp_ret_kb;       // retained_bytes / 1024
	uint32_t mp_ret_entries;  // retained_entries
	uint32_t mp_ready;        // ready_entries (decoded, awaiting promotion)
	uint32_t mp_backlog;      // promotion_backlog
	uint32_t mp_evt_q;        // queued_events
	uint32_t mp_evict;        // retention_evictions
	uint32_t mp_alloc_fail;   // retention_allocation_failures
	uint32_t mp_reconn;       // forced_reconnects
	uint32_t wl_pending;      // gstate.world_switch_pending
	uint32_t wl_failed;       // gstate.world_load_failed
	uint32_t mp_rx_kb;        // raw bytes received / 1024 (proves rx flow)
	// Terminal-event latch (mp-split-0912/5). mp_rx_kb above proves bytes
	// FLOWED; these three say how the connection generation ENDED, so an
	// unattended run that comes back disconnected names its own cause
	// instead of needing a screenshot of the disconnect screen. First-wins
	// within a generation, cleared by the next connect
	// (server_remote_status::mp_end and friends).
	uint32_t mp_end;          // 0 none, 1 kicked-by-Packet255, 2 remote-eof,
	                          // 3 receive-error, 4 connect-error
	uint32_t mp_end_phase;    // enum server_remote_state at that moment --
	                          // the same value the diagnostic's phase word
	                          // ("play"/"login"/"handshake") comes from
	uint32_t mp_end_rx;       // EXACT rx bytes at the terminal event (mp_rx_kb
	                          // rounds a 21-byte death down to 0)
	// mp-split-0912/25: the terminal reason TEXT, so an unattended run can
	// tell an AFK kick ("You have been idle...") from a protocol complaint
	// without a maintainer reading it off the TV. mp_end above says WHICH
	// CLASS of event ended the generation; this says what the peer actually
	// wrote. Sourced from server_remote_status::reason, which the
	// mp-split-0912/7 teardown latch memcpy's into the outer handle before
	// the impl is freed, so it survives server_remote_destroy exactly like
	// mp_end/_phase/_rx do. EMPTY whenever mp_end == 0: a live session's
	// `reason` can hold a non-terminal diagnostic, and reporting that as a
	// terminal reason would be a lie the collector cannot detect.
	// Fixed array, not a const char*, because the string's owner (the impl,
	// or the latch inside it) can be freed between the fill and the format.
	// 48 bytes = 47 characters + NUL; see the escaped worst-case accounting
	// in test_heartbeat_production_buffer_fits_full_payload before widening
	// it -- a control-character reason costs 6 emitted bytes per source byte.
	char mp_end_reason[HW_TELEMETRY_MP_END_REASON_SIZE];
	uint32_t mp_dec_phase;    // decoder phase (enum in mp_codec.c)
	uint32_t mp_dec_pkt;      // packet id the decoder is inside
	uint32_t mp_dec_have;     // chunk_body bytes collected
	uint32_t mp_dec_need;     // chunk_body bytes wanted
	// Remote primary movement cadence, per session. Ticks count executed
	// unpaused physics ticks at main's tick loop; samples count inbox
	// acceptance. Their delta is explained by rpcdrop on a full inbox.
	uint32_t mp_move_ticks, mp_move_samples;
	// MP JOIN-BURST ADMISSION BUDGET (mp-split-0912 W4,
	// docs/mp_admission_budget_design_2026-09-13.md section 2.7). The
	// memory high-water evidence Astra gate-2 item 4 asked for: a 441-column
	// join burst has to be answerable with numbers, not a screenshot. All
	// nine are zero on every single-player profile (no MP session, no body
	// ever allocated in the reservation) and are filled from ONE
	// server_remote_get_status snapshot plus one readiness census, so they
	// are mutually consistent within a heartbeat.
	uint32_t mp_adm_radius;       // status.admit_radius, columns (W1 2.1)
	uint32_t mp_resident_cols;    // columns installed at full detail
	uint32_t mp_backed_cols;      // columns whose only copy is a compressed
	                              // body (RETAINED/PROMOTING) -- the W1
	                              // demote-on-exit tier
	uint32_t mp_backing_kb;       // CARVE_MP_BACKING in use / 1024 (W2 2.4)
	uint32_t mp_backing_peak_kb;  // its high-water this run / 1024 -- the
	                              // number a Wii budget claim rests on
	uint32_t mp_demotions;        // lossless re-encodes back to BACKED
	uint32_t mp_compactions;      // deferred-log folds into a backed head
	uint32_t mp_backing_refusals; // clean capacity failures (mp_end=5);
	                              // MUST be 0 on an acceptance workload
	uint32_t mp_ready_pending;    // W3 census: in-radius obligations the peer
	                              // announced and the pump has not installed
	                              // yet, for the ready radius. 0 whenever the
	                              // loading screen is not the one deciding
	                              // (see loading_screen below).
	// S3 streaming-voice proof (2026-08-31, debug.music_force lever):
	// machine evidence the decode thread + hardware voice actually ran on
	// real silicon, independent of a human's ears. All zero/silent on any
	// build with the same defaults as today. music_key: 0 = silent,
	// 1..AUDIO_MUSIC_KEY_COUNT = that background key + 1
	// (audio_music_table.h), AUDIO_MUSIC_KEY_COUNT+1 = a jukebox record.
	// music_ms: playback position (ms) of whatever is CURRENTLY sounding,
	// 0 when silent. music_underruns: audio_music_ring.h's decode-fell-
	// behind counter -- should stay 0 on a healthy run.
	uint32_t music_key;
	uint32_t music_ms;
	uint32_t music_underruns;
	// Pacing-bug probes (2026-09-01): successful backend chunk starts vs
	// wrapper-gate rejects (audio.h accessors). Names which gate lies when
	// music races/underruns.
	uint32_t music_starts;
	uint32_t music_rejects;
	uint32_t music_dur; // last computed Wii chunk deadline ms (probe)
	// mp_reconnect_soak scenario evidence (source/game/mp_reconnect_soak.h,
	// S5.3 hunt plumbing). mp_reconnect_cycles is the monotonic count of
	// successful connect/disconnect round-trips completed so far this run;
	// the collector's mp_reconnect scenario gate requires the LAST heartbeat
	// to show it reached mp_reconnect_target, with mp_reconnect_wedge never
	// having been true (a phase timeout -- a wedge is a hard FAIL, never a
	// retryable condition). All zero/false on every other profile
	// (mp_reconnect_soak_cycles()/_target()/_wedged() all return the
	// not-armed default).
	uint32_t mp_reconnect_cycles;
	uint32_t mp_reconnect_target;
	bool mp_reconnect_wedge;
	// menus_dwell hardware-risk probe (2026-09-02, staged_menus_dwell_
	// 20260902_063552's adversarial review): was the world the menu_autopilot
	// script just CREATED loaded as creative? Sourced from gstate.
	// config_creative -- the CLIENT half of creative mode (game_state.h),
	// NOT server_local.creative_mode (the SERVER half). Both are populated
	// independently from the SAME per-world LEVEL_GAME_MODE tag at load
	// (screen_select_world.c/server_local.c server_local_load_world_impl), so
	// for singleplayer they must agree; gstate.config_creative is picked
	// because it is a plain global already reachable from main.c's
	// heartbeat-building scope (emit_hw_heartbeat), whereas the live `struct
	// server_local` instance is a LOCAL of main() with no accessor threaded
	// to that scope -- reaching it would mean new plumbing for no additional
	// evidence value. Direct proof the create-world Game Mode selector (see
	// menu_autopilot_script.h's STEP_CWORLD_MODE_SURVIVAL hardening) actually
	// stuck, independent of any menu_ap_step evidence.
	bool creative;
	// Per-frame GP-load gauge (2026-09-03 far-ring frame-drop root-cause doc's
	// "instrument caveats": `dlcall` above is g_displaylist_badcall, a
	// CORRUPTION counter that reads 0 on any healthy run -- there was no
	// actual draw-call/triangle count to correlate against the gpu_ms/vsync_ms
	// dips). dlcalls = GX_CallDispList/glDrawArrays(GL_QUADS) issues this
	// frame (chunk.c's per-side draw loop, via displaylist_render); tris =
	// their combined emitted vertex count / 4 * 2 (GX_QUADS: 2 triangles per
	// quad). Reset once per frame in main.c before the world render pass;
	// zero on a frame that never calls world_render (menus, loading).
	uint32_t dlcalls;
	uint32_t tris;
	// P2-c (2026-09-03 round-2 frame-drop root-cause doc, "#3 Instrumentation
	// gap"): the mesher WORKER's own build wall-clock (chunk_mesher.c's
	// mesher_build_ms_last/_max via chunk_mesher_build_ms()) was previously
	// F3-only (screen_ingame.c) -- this is the direct "tens of ms of vertex_
	// light/BFS" number the root-cause doc's bench_mesher comparison refutes
	// as pure compute (AO 0.052 + BFS 0.025 ms/section), i.e. the gap between
	// this and bench_mesher's per-section cost IS the emit + MEM2 churn +
	// (now, P2-b) cooperative-slice sleep time. Last completed build and the
	// running max since boot, ms. 0.0 on a heartbeat with nothing meshed yet.
	float ms_mesh_worker;
	float ms_mesh_worker_max;
	// mem2_stat_lock_wait(): cumulative count of a thread finding the Wii
	// mem2 serialising mutex already held (mem2.c). 0 on PC (no lock).
	// Correlate against ms_mesh_worker/gpu_ms to test mechanism A (the
	// mesher's MEM2 churn contending with the GP/main thread's own MEM2
	// traffic) directly, rather than only by chunks/mesh_q proxies.
	uint32_t mem2_lock_wait;
	// T-a (2026-09-03, Round 3 (3) "walk wedge" telemetry self-quiesce fix):
	// hw_telemetry_send_fail_classify's cumulative view of failed heartbeat-
	// LINE sends this session. tel_fail is the monotonic count of dropped
	// lines (both transient -EAGAIN drops and the one hard-error drop that
	// precedes a close); tel_last_errno is the negative socket-send result
	// that caused the most recent one (0 = no failure yet); tel_quiesced
	// mirrors hw_telemetry_quiesced() -- true once no further heartbeat will
	// ever be sent this session, for ANY reason (this mechanism or an older
	// one, e.g. the post-"ok" grace-window quiesce). A last received
	// heartbeat with tel_fail == 0 cannot exclude later send failures:
	// after quiescence these counters remain available only on the console.
	uint32_t tel_fail;
	int tel_last_errno;
	bool tel_quiesced;
	// Sender self-telemetry for the run (2026-09-07). Emitted as
	// tel_drop/tel_it/tel_sends/tel_smax_us/tel_qmax/tel_hb_repl; +137 bytes
	// worst case on a 9,216 B heartbeat slot. tel_drop moves the ring's drop
	// counter OUT of carve_census (a droppable observer record could not
	// report its own losses) and into the one record with a priority lane.
	struct hw_telemetry_sender_stats tel;
	// T-b (2026-09-03, Round 3 (3) instrumentation-gap fix): monotonic count
	// of main-loop iterations since boot (incremented once per frame,
	// including frames with no heartbeat and frames before the first world
	// load). Independent proof the main thread is still making forward
	// progress even once telemetry itself has gone quiet (tel_quiesced) --
	// see main.c's low-rate write of this same counter into the SD boot
	// trace ring (hw_boot_trace_write, every 60 frames) for the corroborating
	// post-hoc channel that survives a fully quiesced/dead telemetry socket.
	uint32_t frame_counter;
	// T-c (2026-09-03, Round 3 (2)(a) MEM2 gauge fix): free bytes (in KB)
	// remaining in the Wii MEM2 CARVE heap itself (__lwp_heap_getinfo on
	// mem2.c's mem2_heap), NOT the arena2 remainder ABOVE the carve that
	// mem2_free_kb already reports. mem2_free_kb is blind to carve occupancy
	// (Round 3 (2)(a): SYS_GetArena2Size() stayed flat while carve spill
	// climbed into the tens of thousands at reach 11/14/16) -- this is the
	// number that actually predicts a display-list carve spill before
	// mem2_spill starts climbing. -1 on PC (no carve) and on a Wii build
	// whose carve heap failed to init (mem2_ready false).
	int mem2_carve_free_kb;
	// colcap-1006 (ASTRA-CC-R2 item 5): the largest single request the
	// carve can satisfy (mem2_carve_largest_allocatable_bytes: the largest
	// free block run through libogc's __lwp_heap_allocate size rule, NOT the
	// physical block size), with an explicit validity flag. Filled at
	// heartbeat cadence by the probe mem2_init registers
	// (hw_telemetry_set_carve_largest_probe) when the caller left it unset;
	// always emitted, so "no reading" is a visible false/0, never an absent key.
	bool mem2_carve_largest_valid;
	uint32_t mem2_carve_largest_allocatable_bytes;
	// H-1 (2026-09-03 responsiveness lane): cumulative chunk_mesher_build
	// completions since boot (chunk_mesher_meshed_count -- the mesher
	// worker's throughput counter). A delta of this over a wall-clock window
	// is "chunks meshed/s"; the near_walk/far_walk profiles use it to prove
	// the mesher keeps up while walking, not just that it eventually
	// converges.
	uint32_t chunks_meshed;
	// H-2/H-3 (2026-09-03 maintainer ruling): near-ring keep-up probe, the
	// exact mirror of far_missing/far_missing_max/far_settle_ms above but for
	// the near ring (Chebyshev <= MAX_VIEW_DISTANCE) instead of the far
	// annulus -- see world_near_ring_status (world.c/.h) and
	// source/game/near_ring_keepup.h for the shared definition and cost
	// bound.
	//
	// SPLIT 2026-09-04 (far_walk_6m RCA). near_missing used to be the SUM of
	// the two near-ring deficits, which made a pinned value unreadable: it
	// could mean the server was not streaming, or that the mesher was not
	// meshing, and on staged_far_walk_6m_20260904_083316 it meant the latter
	// while looking like it could be either. The instantaneous terms are now
	// reported separately:
	//
	//   near_missing      -- COLUMNS in the disk with no dict_wsection entry
	//                        at all (streaming deficit; server/network side).
	//   near_mesh_pending -- resident FULL-DETAIL (lod==0) SUB-CHUNKS still
	//                        carrying rebuild_displist or mesh_in_flight
	//                        (mesher deficit; render side). This is also the
	//                        exact quantity the band-2 admission gate reads
	//                        (world_band2_allow_far), so a heartbeat now shows
	//                        directly whether far chunks were being admitted.
	//
	// near_missing_max and near_settle_ms still track the COMBINED signal
	// (missing + mesh_pending), because "the near ring has settled" means both
	// terms are 0 -- see source/game/near_ring_keepup.h. Do NOT read
	// near_missing_max as the high-water mark of near_missing alone.
	// near_settle_ms is the time (ms) from the last detected player chunk-
	// crossing (spawn/teleport/dimension-switch are all chunk-coordinate
	// changes, so each is captured) until that combined signal next returned
	// to 0, running max since world load. All zero when no world is loaded.
	int near_missing;
	int near_mesh_pending;
	int near_missing_max;
	uint32_t near_settle_ms;
	// H-2 (2026-09-03 maintainer ruling): running max, across every band-0
	// urgent chunk (player edits + their lateral neighbours; a just-
	// installed section's lateral neighbours) this world has fully resolved,
	// of how many world_build_chunks frames it waited between being marked
	// urgent and becoming fully settled (world_edit_to_visible_frames_max,
	// world.c). 0 until the first urgent chunk settles.
	uint32_t edit_to_visible_frames;
	// H-2/H-3 (2026-09-03 maintainer ruling): wall-clock (ms) the "Building
	// terrain" loading screen was shown for, from screen reset to the
	// screen_ingame transition -- see screen_load_world.c's
	// lworld_load_screen_ms_result(). 0 while the screen is still showing (a
	// heartbeat sampled mid-load has not seen the result yet); the collector
	// should read this from a LATER heartbeat, once world_loaded is true and
	// a few seconds have passed, not the same tick world_loaded flips.
	uint32_t load_screen_ms;
	// Main-thread loading-safety observation. Health is -1 without a body.
	int player_health;
	/* Versioned d1 = [session, host deaths, host cause ID, guest deaths,
	 * guest cause ID]. Session zero is explicitly INVALID, even if counts are
	 * zero. Causes use server_player_damage_cause (0..8). Always supplied by
	 * main; `present` preserves legacy formatter fixtures only. Replaces the
	 * legacy top-level deaths/death_cause pair without growing the line cap. */
	bool death_present;
	/* Full guard context shares its heartbeat's queue ownership and reaches
	 * the blocking sender first. g1=1 requires that matching context; the
	 * compact marker fits d1's zero-death width saving (see formatter). */
	bool perf_guard_present;
	uint32_t perf_guard_words[47];
	uint32_t death_record[5];
	bool loading_screen;
	// General 1 Hz extension, enabled by main even without any observer flag.
	bool storage_enabled;
	uint32_t op, oph, opc, opf, oer, orc, ocy, oex, owu, db, ds, dln, dls, dlp;
	uint32_t dlc; // finalization compaction enabled (0/1); no list arena
	// M2 mesh blocks: generations packed, scratch overflows (lists that
	// fell back to the carve), pack failures (carve OOM). Emitted only
	// when nonzero, like dlc.
	uint32_t dlbp, dlbo, dlbf;
	// M2 hardening: dlbm = block mode requested<<1 | effective; dlbs =
	// scratch-arena allocation failures; dlbc / dlbx = bytes copied at
	// pack / spilled to the carve, in KiB. Emitted only when block mode
	// was requested.
	uint32_t dlbm, dlbs, dlbc, dlbx;
	// Server-phase profiler: microseconds per wd_phase_srv since the previous
	// heartbeat (watchdog_srv_phase_take), emitted as "srv_ph":[...] when any
	// slot is nonzero. Index = enum wd_phase_srv.
	uint32_t srv_ph[24];
	// Tick-first pacing: lean server iterations in the trailing ~1 s
	// (emitted when nonzero).
	uint32_t srv_lean;
	// srv_tclk: tick-clock accounting for the trailing ~1 s server-loop window
	// (Astra round 7 D; server_local.c TICK_CLOCK_TRACE): samples, credited
	// elapsed ms, ticks credited, ticks capped off (Beta cap 10), discarded
	// elapsed ms (< 0 / > 1000), clock resets, sleep requested ms, sleep
	// actual ms, ticks executed, overrun (1 ms catch-up) sleeps, peak pending
	// (owed) ticks, window sequence number (seqlock-coherent), catch-up limiter
	// sleep ms, spare. Emitted as
	// "tclk":[..] when any entry is non-zero.
	uint32_t srv_tclk[14];
	// Split render reach in effect (two cameras active), 0 otherwise. The
	// collector records it: a reduced-reach PASS never inherits the
	// full-presentation meaning (Astra debate 2026-09-11, G).
	uint32_t split_reach;
	// Effective server load radius in chunks (split: split.server_view_radius
	// while two anchors are active; else MAX_VIEW_DISTANCE). Always emitted.
	uint32_t server_radius;
	// S-A3 (Astra H3, mp-split-0912/49): the LIVE split toggle's own state,
	// sourced from screen_options_split_live_status() (see screen.h for the
	// per-field contract). The heartbeat already carries mem2_carve_free_kb
	// every second, so the MEM2 headroom either side of a live enable falls
	// out of the stream by itself; these four are the part it could not say --
	// when a toggle happened, whether one was still in flight at this
	// heartbeat, how many ms the server took to answer the last one, and
	// whether it said yes. ALWAYS emitted (see the formatter): "the toggle
	// never happened" has to be a visible 0/0/0/-1 line, because an absent
	// key and an untoggled run would otherwise read identically.
	// split_live/split_pending are bool so their wire width is a structural
	// one digit; split_last_ok is clamped to its -1/0/1 domain by the
	// formatter for the same reason (the tel.stage-mask precedent).
	bool split_live;
	bool split_pending;
	uint32_t split_toggle_ms;
	int split_last_ok; // 1 ok, 0 refused/lost, -1 nothing settled yet
	/* O20: the vehicle smoke driver's completion status ("ok" or
	 * "fail:<code>"), set once the driver finishes; NULL before. Emitted as
	 * "vs_status" only when set, so every other heartbeat is unchanged. The
	 * vehicle collector stops on the first heartbeat with a fault counter,
	 * so the reason must ride the heartbeat itself, not only a later row. */
	const char* vs_status;
};

bool hw_telemetry_init(const struct hw_telemetry_config* cfg);
void hw_telemetry_shutdown(void);
bool hw_telemetry_active(void);
// OUTER bring-up retry (Batch E item 5, 2026-08-23 telemetry DHCP design
// note -- docs/hwproof_audit_2026-06-30.md:112, HW_TELEMETRY_DHCP_MAX_RETRIES
// at hw_telemetry.h:17-20 above). The comment on that cap says its short
// value is deliberate -- a boot must never hide a genuine freeze behind a
// long SILENT black screen -- so this does NOT raise it: `attempt` (main.c
// wraps hw_telemetry_init itself) keeps its existing fast-fail cap. Instead
// this is a bounded OUTER loop: on failure it calls `sleep_fn` (main.c wires
// the real thread_msleep + a console progress line so the wait is visibly
// "retrying", never silent) and tries `attempt` again, until `attempt`
// succeeds or the retry budget is exhausted. Elapsed time is the SUM of
// sleep_ms already spent, not a wall-clock read -- exact budget math, and
// testable with a fake, call-counting sleep_fn with no real timing
// dependency (tests/test_hw_telemetry.c). Generic in its mechanism (not
// Wii- or telemetry-specific) so it is testable off-device; main.c is the
// sole real caller. Always makes at least one `attempt` call, even with
// budget_ms == 0. `*attempts_out` (may be NULL) receives the number of
// `attempt` calls made either way.
typedef bool (*hw_telemetry_retry_attempt_fn)(void* user);
typedef void (*hw_telemetry_retry_sleep_fn)(uint32_t ms, void* user);
bool hw_telemetry_retry_bringup(hw_telemetry_retry_attempt_fn attempt,
								hw_telemetry_retry_sleep_fn sleep_fn, void* user,
								uint32_t budget_ms, uint32_t sleep_ms,
								int* attempts_out);
const char* hw_telemetry_local_ip(void);
// Arm the deferred client-accept: call once the game main loop is running so the
// first net_accept happens in a settled context, not during early-boot emit_boot
// (which hard-faults net_accept on real hardware).
void hw_telemetry_arm_accept(void);
// Wii: current F_GETFL flags of the pull-mode listen socket (-1 when unavailable).
// Surfaced in the boot trace so one hardware run verifies the IOS non-blocking
// bit (0x04) actually latched -- see CW_IOS_O_NONBLOCK in hw_telemetry.c.
int hw_telemetry_debug_listen_flags(void);
void hw_telemetry_emit_boot(const char* build_id);
bool hw_telemetry_sd_probes_enabled(void);
void hw_telemetry_emit_heartbeat(const struct hw_telemetry_heartbeat* hb);
/* colcap-1006 (ASTRA-CC-R2 item 5): heartbeat-cadence source for the
 * mem2_carve_largest_* pair. mem2_init registers it on the Wii; NULL (PC,
 * tests) leaves the caller's fields as they are. The probe returns validity. */
void hw_telemetry_set_carve_largest_probe(bool (*probe)(uint32_t* bytes));
/* colcap-1006 (ASTRA-CC-R2 item 5): one column-pool demand-growth attempt,
 * captured under mem2's allocator lock at the instant of the carve and
 * published after the lock is released. *_before values come from one
 * bounded heap walk taken immediately before the slab carve; before_valid
 * false means that walk could not be trusted (then both are 0). */
struct hw_column_pool_grow {
	uint32_t seq;
	uint32_t requested_bytes;
	uint32_t free_bytes_before;
	uint32_t largest_allocatable_bytes_before;
	uint32_t slabs_before, slabs_after;
	bool before_valid;
	bool success;
};
size_t hw_telemetry_format_column_pool_grow(char* dst, size_t dst_size,
											const struct hw_column_pool_grow* ev);
bool hw_telemetry_emit_column_pool_grow(const struct hw_column_pool_grow* ev);
/* colcap-1006 (ASTRA-CC-R3 item 4): one wholly empty non-boot column slab
 * handed back to the carve, captured under mem2's lock and published after
 * it is released. `seq` shares the grow events' counter; `slab` is the
 * index the slab had (>= the boot slabs); released_bytes is the carve
 * request it had made (lead + slots * stride). */
struct hw_column_pool_shrink {
	uint32_t seq;
	uint32_t slab;
	uint32_t released_bytes;
	uint32_t slabs_before, slabs_after;
};
size_t hw_telemetry_format_column_pool_shrink(char* dst, size_t dst_size,
	const struct hw_column_pool_shrink* ev);
bool hw_telemetry_emit_column_pool_shrink(const struct hw_column_pool_shrink* ev);
void hw_telemetry_emit_profile_done(const char* status);
// Main publishes frame progress independently of heartbeat formatting/sends.
void hw_telemetry_note_frame(uint32_t frame);
// Close admission immediately. Wii sender drains accepted FIFO records before
// transport quiescence and continues diagnostic alive probes after failure.
// Wii startup is one-shot until reset. Shutdown cancels/joins on a stop worker;
// main waits at most 250 ms and never reuses timed-out state. PC is synchronous.
void hw_telemetry_quiesce(void);
bool hw_telemetry_quiesced(void);
void hw_telemetry_emit_dump(const char* cause, const char* text, bool fault);
void hw_telemetry_emit_crash_mailbox(void);

// T-a (2026-09-03, Round 3 (3) "walk wedge" fix): a PURE state machine for a
// failed heartbeat-line send, deliberately kept free of any socket I/O or
// PLATFORM_WII guard so tests/test_hw_telemetry.c can drive the exact same
// classification hw_telemetry.c's real Wii send path uses, via a caller-
// owned state struct instead of a real net_send. See the long design comment
// above hw_telemetry_send_fail_classify's definition (hw_telemetry.c) for the
// -EAGAIN-vs-hard-error rationale.
struct hw_telemetry_send_fail_state {
	uint32_t fail_count;  // cumulative failed-line count (never resets)
	int last_errno;       // negative socket-send result of the most recent failure
	unsigned consec_fail; // consecutive TRANSIENT (-EAGAIN/-EWOULDBLOCK/-ENOBUFS) failures
	bool quiesced;        // true once this state machine has asked to quiesce
	// Set when a line was abandoned mid-stream and an ambiguous prefix may
	// be on the wire. The next line is sent with a leading LF so that prefix
	// becomes exactly one malformed NDJSON line instead of poisoning every
	// record after it. Cleared by the next fully-sent line. The abandoned
	// bytes are NEVER retried (libogc queues a partial AND reports the
	// error -- see the 2026-09-02 stage-fetch corruption RCA).
	bool resync;
};
void hw_telemetry_send_fail_state_reset(struct hw_telemetry_send_fail_state* st);
// Call after a heartbeat line sends successfully: clears the consecutive-
// failure streak (fail_count/last_errno/quiesced are sticky evidence and are
// NOT touched here).
void hw_telemetry_send_fail_state_note_success(
	struct hw_telemetry_send_fail_state* st);
// Classify one failed send. `neg_errno` is the negative socket-send result
// (or any negative sentinel the caller uses for a non-errno failure).
// Updates `st` in place and, if `out_close_socket` is non-NULL, reports
// whether the caller must net_close() the socket. Check `st->quiesced`
// afterward to know whether to call hw_telemetry_quiesce().
void hw_telemetry_send_fail_classify(struct hw_telemetry_send_fail_state* st,
									 int neg_errno, bool* out_close_socket);
#ifdef CAVEWII_HW_TELEMETRY_TEST
// Test seam for the escalating transient back-pressure pacing ladder (2026-
// 09-07 fix round): consec_fail 1/2/3 -> 2000/8000/32000 (us), clamped to the
// last entry beyond that. consec_fail 0 -> 0. See hw_telemetry.c's
// transient_pace_us doc comment for the resulting 42ms tolerance bound before
// HW_TELEMETRY_SEND_FAIL_QUIESCE_STRIKES quiesces the stream.
uint32_t hw_telemetry_test_transient_pace_us(unsigned consec_fail);
// Test seam for the bounded-send deadline (2026-09-08). Installs the same
// writability gate + clock the Wii path builds from net_poll(POLLOUT)/gettime,
// so a PC test can model "the socket is not writable for 2 s" and "one chunk
// of this line took 2 s" without a socket. `ready` returns >0 writable, 0 not
// writable within its timeout_ms (the line is abandoned), <0 gate unusable
// (the sender falls back to sending, exactly as before this seam existed).
// `now_ms` is the deadline clock. Both NULL disarms the gate.
void hw_telemetry_test_set_send_gate(int (*ready)(void* user,
										 uint32_t timeout_ms),
									 uint64_t (*now_ms)(void* user),
									 void* user);
bool hw_telemetry_test_send_line_with_gate(const char* line,
	int (*send_fn)(const char*, size_t, void*), void* user,
	struct hw_telemetry_send_fail_state* state);
// Test seam for the sender STAGE clock (2026-09-08). The Wii reads the
// timebase; a PC test installs its own microsecond clock so it can model "the
// transport parked for 3 s" without a test that actually sleeps 3 s. NULL
// restores the default (the Wii timebase, or a constant 0 on a PC build --
// which is why an uninstalled clock reports stage_us 0 rather than noise).
void hw_telemetry_test_set_stage_clock(uint64_t (*now_us)(void* user),
									   void* user);
// Reads back the sender's live packed stage word, so a test can assert the
// stage WITHOUT going through a heartbeat compose. Returns the enum ordinal.
uint32_t hw_telemetry_test_sender_stage(void);
// PC stand-in for the real Wii LWP_MutexLock() park inside send_lock_enter
// (2026-09-08 fix round): off Wii there is nothing to block on, so an armed
// hook stands in for the wait, called from INSIDE the same HW_TEL_STAGE_LOCK
// enter/leave the real blocking call gets -- letting a test prove the LOCK
// stage and its dwell (tel_stg/tel_stg_us) without real hardware threading.
// NULL (the default) disarms it and the PC path behaves as before: an
// instant, unconditional success with no stage entered.
void hw_telemetry_test_set_lock_wait(void (*wait_fn)(void* user), void* user);
// Exercises cw_net_flags_confirm_nonblock -- the pure "was the non-blocking
// bit actually confirmed latched" decision cw_net_set_nonblock's F_GETFL
// read-back uses (2026-09-08 fix round) -- with synthetic net_fcntl results,
// so the fix (a SETFL success alone is no longer trusted) is provable
// without a real IOS socket. Mirrors net_fcntl's own s32 return via
// int32_t, portable to a PC build where s32 is not declared.
bool hw_telemetry_test_nonblock_flag_confirm(int32_t setfl_ret,
											 int32_t getfl_ret);
#endif
// The bounded-send deadline: the longest a single line may keep the sender
// waiting for a writable socket (2026-09-08). See hw_telemetry.c's send_gate
// doc comment for the worst-case timing this buys.
#define HW_TELEMETRY_SEND_DEADLINE_MS 250u

/* EVIDENCE LINES ARE NEVER ABANDONED MID-LINE (2026-09-26, telreliable).
 * The bounded-send deadline above exists so a slow DIAGNOSTIC line cannot
 * hold the heartbeat's turn; it was also applied to the heartbeat itself.
 * Real-Wii split_play_8m (G4, three FAILs): one net_send chunk of a 5.4 KB
 * heartbeat took 255 ms inside IOS (tel_xmax_us=255065, tel_wto 0->1 on the
 * very next heartbeat), the per-line deadline expired between chunks, and
 * the heartbeat -- with its 60 anchor frame times -- was abandoned after
 * 1 KiB had already reached the wire. TCP delivered exactly what it was
 * given; the host could not have received it. A line the collector's
 * verdict depends on (hw_telemetry_line_is_critical: heartbeat,
 * anchor_frames, profile_done) is therefore finished once its first byte is
 * committed, and a writability poll that times out BEFORE any byte (wire
 * framing still clean, so retrying is safe) is retried up to
 * HW_TELEMETRY_CRITICAL_POLL_TRIES times (<= 1 s at the 250 ms deadline,
 * well inside the collector's 5 s stale limit). Observer lines keep the
 * original abandon-on-deadline behaviour unchanged. */
#define HW_TELEMETRY_CRITICAL_POLL_TRIES 4u
bool hw_telemetry_line_is_critical(const char* line);

/* THE NON-BLOCKING SEND EXPERIMENT (2026-09-08, opt-in, default OFF).
 * Armed by the `--cw-tel-nonblock` run-arg (`-TelemetryNonBlock` through the
 * staging scripts) and by nothing else, so an unarmed run is byte-identical
 * to before this existed.
 *
 * WHY it is opt-in and not the new default: the 2026-09-06 root fix
 * deliberately made this socket BLOCKING on its own sender thread, because
 * the previous paced non-blocking path hit libogc's partial-then-EAGAIN trap
 * (network_wii.c's send loop queues the partial AND returns the error, so the
 * queued length is UNKNOWABLE) and quiesced the stream. That reasoning still
 * stands. This switch exists to answer ONE diagnostic question the blocking
 * socket cannot: when the sender parks for seconds, is it parked on TCP
 * back-pressure (which a non-blocking socket cannot do) or on IOS IPC itself
 * (which it still can)?
 *
 * WHAT it changes when armed:
 *   - the accepted client socket is made non-blocking (IOS's own O_NONBLOCK
 *     bit, see CW_IOS_O_NONBLOCK in hw_telemetry.c -- newlib's 0x4000 and
 *     network.h's 04000 both silently leave it BLOCKING on hardware);
 *   - the per-line net_poll gate is SKIPPED (a non-blocking send cannot wait,
 *     so polling first would add one IOS ioctl per line for nothing -- and
 *     that ioctl is itself IPC, the very thing under suspicion);
 *   - a short write, or -EAGAIN/-EWOULDBLOCK, ABANDONS the line: never
 *     retried (the 2026-09-02 duplicate-bytes trap), counted in `abandoned`,
 *     and the next line resyncs framing with a leading LF so the collector
 *     loses at most that one NDJSON line. Because libogc may have queued a
 *     partial before reporting -EAGAIN, ANY negative is treated as "unknown
 *     bytes queued, resync owed".
 *   - every OTHER negative errno still goes to the normal classifier, so the
 *     experiment can never mask a genuinely dead socket.
 * The heartbeat keeps its own priority lane in the ring either way. */
void hw_telemetry_set_nonblock_experiment(bool on);
bool hw_telemetry_nonblock_experiment(void);
// Session view of the module's own real (Wii) send-fail state, for main.c's
// heartbeat builder. 0 on PC (no socket, no failures possible).
void hw_telemetry_sender_stats_take(struct hw_telemetry_sender_stats* out);

uint32_t hw_telemetry_tel_fail_count(void);
int hw_telemetry_tel_last_errno(void);
// Admission-FIFO drops (a full or contended ring refusing an observer or
// pre-connect record). NOT a transport failure and deliberately NOT folded
// into tel_fail: a dropped diagnostic record costs one sample, a send failure
// costs the stream. Zero on PC unless the telemetry test seam is compiled in.
uint32_t hw_telemetry_observer_drop_count(void);
/* Diagnostic: a {"event":"tel_drop_names",...} line when new drops were
 * attributed since *seen_seq (event name -> refused count); false otherwise. */
bool hw_telemetry_drop_names_line(char* out, size_t cap, uint32_t* seen_seq);

#ifdef CAVEWII_HW_TELEMETRY_TEST
typedef bool (*hw_telemetry_test_sink_fn)(const char* line, void* user);
typedef int (*hw_telemetry_test_send_fn)(const char* data, size_t len, void* user);
void hw_telemetry_test_set_sink(hw_telemetry_test_sink_fn sink, void* user);
// PC proof seam for the production four-slot admission and byte-sender path.
void hw_telemetry_test_async_sender_begin(void);
const char* hw_telemetry_test_async_sender_peek(void);
bool hw_telemetry_test_heartbeat_off_equiv(void);
// The sender's real idle-park object (struct cw_tel_wake, hw_tel_wake.h),
// handed out so a PC test can run a sender-shaped loop that parks on the SAME
// object the production admission paths signal -- i.e. so the wake test
// covers send_line_lane/send_observer_line, not a copy of them. Valid only
// between _begin and _end. Declared void* so hw_telemetry.h keeps its current
// include set; the test casts it back.
void* hw_telemetry_test_sender_wake(void);
unsigned hw_telemetry_test_async_sender_drain(
	hw_telemetry_test_send_fn send_fn, void* user);
// Drain AT MOST max_records, so a test can model a sender thread that is only
// scheduled a few times per second -- the real-Wii failure mode this ring's
// heartbeat lane exists for (2026-09-07).
unsigned hw_telemetry_test_async_sender_drain_n(
	hw_telemetry_test_send_fn send_fn, void* user, unsigned max_records);
uint32_t hw_telemetry_test_async_sender_drops(void);
void hw_telemetry_test_async_sender_end(void);
void hw_telemetry_test_set_send_lock_busy(bool busy);
unsigned hw_telemetry_test_send_lock_blocks(void);
bool hw_telemetry_test_send_line_with(const char* line,
									  hw_telemetry_test_send_fn send_fn,
									  void* user);
// Positive counts are exact; negative results may hide queued bytes.
// Errors are never retried, matching libogc net_send semantics.
bool hw_telemetry_test_send_line_with_state(const char* line,
	hw_telemetry_test_send_fn send_fn, void* user,
	struct hw_telemetry_send_fail_state* state, bool* close_socket);
// Pre-connect buffer test hooks (the pull-mode boot-event ring).
struct cw_tel_trace;
const struct cw_tel_trace* hw_telemetry_test_trace(void);
void hw_telemetry_test_trace_reset(void);
void hw_telemetry_test_preconnect_reset(void);
bool hw_telemetry_test_preconnect_store(const char* line);
/* R08 F4: the number of records this store evicted (what tel_drop gains). */
unsigned hw_telemetry_test_preconnect_store_n(const char* line);
unsigned hw_telemetry_test_preconnect_count(void);
// Returns true only when EVERY buffered line was fully sent; *sent_out (may be
// NULL) receives the count of fully-sent lines either way.
bool hw_telemetry_test_preconnect_flush(hw_telemetry_test_send_fn send_fn,
										void* user, unsigned* sent_out);
// task_08716cec (2026-08-22 auto-exit strand fix): simulates the WII-ONLY
// trigger that fires when a NORMAL-mode send fails against an already-
// ESTABLISHED telemetry client (the real send/accept/close teardown only
// exists under #ifdef PLATFORM_WII, so it cannot be driven from a PC test
// directly -- see the long comment at hw_telemetry.c's send_line_mode
// PLATFORM_WII branch for the full root-cause narrative). This hook calls
// the SAME hw_telemetry_quiesce() the Wii branch calls, so a PC test can
// verify the "one failure is enough, no further net calls happen" contract
// end to end through the public emit API.
void hw_telemetry_test_note_established_client_send_failure(void);
#endif

size_t hw_telemetry_format_boot(char* dst, size_t dst_size, const char* run_id,
								const char* profile, const char* build_id,
								const char* local_ip);
struct hw_telemetry_fps_window {
	bool initialized;
	uint32_t frame_counter;
	uint64_t sampled_us;
};
struct hw_telemetry_fps_sample {
	float fps;
	uint32_t frames;
	uint64_t elapsed_us;
};
/* Main-thread sampler. Unsigned subtraction admits counter wrap; a delta
 * over half the uint32 range is a counter reset (no real window spans that
 * many frames). Equal timestamps retain the baseline; clock rollback resets
 * it. Sampling is independent of whether the transport delivers a heartbeat. */
struct hw_telemetry_fps_sample hw_telemetry_sample_fps(
	struct hw_telemetry_fps_window* window, uint32_t frame_counter, uint64_t now_us);

size_t hw_telemetry_format_heartbeat(
	char* dst, size_t dst_size, const char* run_id, const char* profile,
	const struct hw_telemetry_heartbeat* hb);
size_t hw_telemetry_format_profile_done(char* dst, size_t dst_size,
										const char* run_id,
										const char* profile,
										const char* status);
size_t hw_telemetry_format_dump_chunk(char* dst, size_t dst_size,
									  const char* run_id, const char* profile,
									  const char* event, const char* cause,
									  uint32_t dump_id, uint32_t seq, bool last,
									  const char* text, size_t text_len);

/* Overflow batches keep every frame when a headless PC exceeds one
 * heartbeat's capacity. The collector checks the cumulative sequence. */
bool hw_telemetry_emit_anchor_frames(const struct hw_anchor_evidence* a);
bool hw_telemetry_emit_observer(const char* line);
void hw_telemetry_save_path_note(const char* marker, uint32_t elapsed_us,
	const uint32_t stats[3], const unsigned cache[3]);
/* Offered-piston profile: cumulative per-marker count/total/max, paced at
 * four summaries/s, with exact-byte retry. Main calls once/frame; true means
 * the current tail is admitted before it may enqueue profile_done. */
bool hw_telemetry_save_path_pump(uint32_t now_ms);
void hw_telemetry_save_path_finish(void);
/* Sender-only consumer of the observer's bounded atomic request slot.
 * The existing <=10ms poll services it; refusals are explicit SO drops. */
bool hw_telemetry_emit_service_observer(const char* line);
size_t hw_telemetry_format_perf_guard(char* dst, size_t cap, const uint32_t words[47], uint32_t heartbeat_tick);
/* O22: admission for a LOSSLESS retrying producer (the vehicle smoke driver
 * keeps the exact line and resends it next frame on refusal). A refusal here
 * loses nothing, so it is NOT a drop: it is counted in
 * hw_telemetry_observer_deferred_count(), never in tel_drop, which keeps its
 * meaning "a record that never reached the wire". */
bool hw_telemetry_emit_observer_retry(const char* line);
uint32_t hw_telemetry_observer_deferred_count(void);
/* SPLIT-SPILL (2026-10-14): overlay fallback + carve-spill attribution.
 * The widest heartbeat already sits on its 200 B spare floor (13,795 of
 * 13,995 B, test_heartbeat_production_buffer_fits_full_payload), so these
 * counters travel as their own small observer record at heartbeat cadence
 * instead of growing the heartbeat slot:
 *   {"event":"overlay_spill","v":1,"tick":..,"fallback_live":..,
 *    "fallback_total":..,"spill":..,"mem2_last_spill_bytes":..}
 * fallback_live/fallback_total are column_view_stats' private-image
 * counters; spill repeats mem2_stat_carve_spill for correlation;
 * mem2_last_spill_bytes is mem2_stat_last_spill_bytes (guarded request size,
 * a size class, not a tenant identity). Main publishes through
 * hw_telemetry_emit_observer_retry only when a value changed since the last
 * ADMITTED record (an all-zero healthy run emits nothing), so a refusal is a
 * deferral retried next heartbeat, never a tel_drop. Diagnostic only: the
 * collector grades none of it. The buffer is far below the 2,303-byte
 * observer input cap enforced by emit_observer_packet_mode. */
struct hw_overlay_spill {
	uint32_t tick, fallback_live, fallback_total, spill, last_spill_bytes;
};
#define HW_TELEMETRY_OVERLAY_SPILL_LINE_SIZE 192u
_Static_assert(HW_TELEMETRY_OVERLAY_SPILL_LINE_SIZE <= 2303u,
	"overlay_spill must fit the observer line cap");
/* 0 on NULL arguments or truncation, else strlen of the record. */
size_t hw_telemetry_format_overlay_spill(char* dst, size_t dst_size,
	const struct hw_overlay_spill* s);
/* True when any counter (tick excluded) differs from `sent`. */
bool hw_telemetry_overlay_spill_due(const struct hw_overlay_spill* cur,
	const struct hw_overlay_spill* sent);
/* Watchdog-LWP-only HANG breadcrumb: lock-free best-effort publish, no wake/IOS. */
bool hw_telemetry_emit_watchdog_start(const char* line);
#ifdef PLATFORM_WII
struct thread_saved_context;
bool hw_telemetry_sender_context_snapshot(struct thread_saved_context* out);
#endif
bool hw_telemetry_emit_portal_quit_server(const char* marker,
					 uint32_t tick, const char* switch_state);
uint32_t hw_telemetry_portal_quit_up_ms(void);
void hw_telemetry_portal_quit_switch_begun(void);

#endif
