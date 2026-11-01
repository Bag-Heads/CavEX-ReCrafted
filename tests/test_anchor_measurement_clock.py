"""Run main.c's real completion block on a host with the Wii time backend.

Only the heartbeat/IO edges are mocked. Extracting the block keeps the clock
ordering under test in production code without booting GX or copying its logic.

push-0921/C1c (review MEDIUM-1): the claim under test is "the integer the
completion block decides on is the integer the heartbeat publishes", so BOTH
ends are sliced verbatim now, not hand-modelled -- main.c's frame-top
hw_anchor_evidence_note call, hw_telemetry.c's hw_anchor_evidence_note (the
only writer of elapsed_ms) and main.c's `hb.anchor = s_anchor` publication join
the completion block. Mutating any one of them in main.c or hw_telemetry.c now
turns this test RED.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/hw_run.h"
#include "platform/hw_telemetry.h"
#include "platform/time.h"

static ptime_t clock_ticks;
uint64_t gettime(void) { return clock_ticks; }
void settime(uint64_t value) { clock_ticks = value; }
static struct hw_run_config hw_run;
static struct hw_anchor_evidence s_anchor;
static ptime_t s_anchor_started, hw_started, hw_last_heartbeat, hw_profile_done_at;
static bool hw_heartbeat_sent, hw_watchdog_injected, hw_profile_done_sent;
/* The completion slice includes the named-save proof's auto-exit interlock.
 * Normal clock scenarios leave it disabled; the focused scenario below pins
 * the safety rule that a failed live-world save cannot exit the process. */
enum {
    HW_SAVE_QUIT_OFF,
    HW_SAVE_QUIT_WAIT_WORLD,
    HW_SAVE_QUIT_PAUSE_QUEUED,
    HW_SAVE_QUIT_EXIT_PENDING,
    HW_SAVE_QUIT_SUCCEEDED,
    HW_SAVE_QUIT_FAILED,
    HW_SAVE_QUIT_CAPTURE_WAIT,
    HW_SAVE_QUIT_CAPTURE_ARMED,
};
static bool hw_save_quit_enabled;
static int hw_save_quit_state;
/* split_recovery_6m (2026-09-17) added a second completion arm to the extracted
 * block. Disarmed (false / empty) it must leave this clock exactly as it was --
 * which is what every assertion below keeps proving. */
static bool s_split_recovery_done_pending;
static char s_split_recovery_status[96];
/* mp_chat_login_3m adds another completion arm to the same production block.
 * Keep its state dormant while exercising the anchor clock below. */
static bool s_chat_login_done_pending;
static char s_chat_login_status[96];
/* host_vehicle_smoke_8m (vehicle-smoke-1004) adds a third completion arm,
 * keyed on hw_run.vehicle_smoke; dormant here like the other two. */
static bool s_vehicle_done_pending;
static char s_vehicle_status[96];
static uint32_t hw_tick, hw_frame_counter;
/* push-0921/C1: the integer millisecond the NEXT emitted heartbeat carries as
 * "anchor_ms" (main.c's frame-top hw_anchor_evidence_note -> hw_telemetry.c
 * a->elapsed_ms -> the "anchor_ms" wire field), and what the fake emitter
 * below actually serialized -- the collector grades the run on THAT integer,
 * so the block's completion decision has to agree with it. */
static uint32_t last_heartbeat_anchor_ms, completion_anchor_ms;
/* Milliseconds between the frame-top anchor note and the completion block's
 * own time sample. Zero reproduces the original single-sample harness; one
 * millisecond is what a real frame has (heartbeat build, client tick, render
 * all happen in between) and is exactly how run 0917_092103 failed. */
static uint32_t frame_skew_ms;
static int far_vd = 6;
/* 9561a42d: the completion block passes the server's EFFECTIVE far distance
 * (split policy: the split radius while two anchors are active). */
struct anchor_test_thread { int unused; };
static struct {
    int far_view_distance; struct anchor_test_thread thread;
    struct { bool checkpoint_failed; } worlds[1];
} server = {.far_view_distance = 6};
/* R24's perf-stress drain arm is dormant in this anchor-clock harness.
 * These are compile-only IO/shutdown doubles. Assignment25 separately drives
 * the real local-server stop/join and final death/guard heartbeat producer. */
#define SERVER_LOCAL_WORLD_SLOTS 1u
#define SERVER_CHECKPOINT_STATS 12u
#define WDM_SHUTDOWN_SERVER 1
#define region_archive_deferred_io_enabled() false
#define watchdog_heartbeat() ((void)0)
#define server_local_shutdown(s) ((void)(s))
#define server_world_checkpoint_stats(out) memset(out, 0, sizeof(out))
#define g9_capture_pending() false
static const void* s_completion_heartbeat_server;
/* These diagnostics are adjacent to the heartbeat cadence block in main.c,
 * but are not part of the anchor/completion clock contract under test. Keep
 * the production cadence and completion statements verbatim while compiling
 * out only their unrelated observer payload dependencies. */
#define emit_freeze_observer(thread, tick) ((void)0)
#define svin_active_owned_rpc_marker() (0u)
#define thread_stack_lowwater_bytes(thread) (0u)
#define server_local_diag_stage() (0u)
#define mem2_diag_op() (0u)
#define mem2_diag_thread() (0u)
#define mem2_diag_bad() (0u)
#define mem2_diag_scan_count() (0u)
#define mem2_diag_scan_total_us() (0u)
#define mem2_diag_scan_max_us() (0u)
#define watchdog_loop_seq() (0u)
#define region_archive_io_busy() (0u)
#define watchdog_main_census_current() (0u)
#define hw_telemetry_emit_observer(line) ((void)0)
/* G9 service observer (default OFF): its shutdown-state edge sits inside
 * the sliced completion block; compile it out like the other observers. */
#define SERVICE_OBSERVER(call) ((void)0)
/* split_offer_piston_2m shares the vehicle completion arm; dormant here. */
#define split_offer_piston_armed() (false)
/* split_offer_piston_2m paced save-path summaries: dormant here. */
#define hw_telemetry_save_path_pump(now_ms) ((void)(now_ms), true)
#define hw_telemetry_save_path_finish() ((void)0)
#define hw_telemetry_drop_names_line(out, cap, seen) ((void)(out), (void)(cap), (void)(seen), false)
#define hw_telemetry_emit_observer_retry(line) ((void)(line), true)
#define thread_msleep(ms) ((void)(ms))
/* colcap-1006 R5 item 2: the pool_selftest_3m completion arm is dormant
 * here (hw_run.pool_selftest is false), like the other driver arms. */
#define pool_selftest_status() ((const char*)0)
/* SPEC R12: the save_reload_3m completion arm is dormant here too. */
#define save_reload_armed() (false)
#define save_reload_status() ((const char*)0)
/* colcap-1006 R5 item 3: split_regrow_8m's anchor route is dormant here
 * (hw_run.anchor_cycle is false). */
#define anchor_cycle_step(s, sep, ms) ((void)(s), (void)(sep), (void)(ms))
#define CW_BUILD_ID "anchor-clock-test"
uint32_t g_srv_tclk_seq;
/* Only the fields the two sliced main.c regions below actually read. */
static struct {
    bool quit;
    struct { float dt; } stats;
    struct { bool active; } autopilot;
} gstate;
/* The real heartbeat record, so the anchor publication is the production
 * assignment into the production struct. */
static struct hw_telemetry_heartbeat hb;
static bool start_next, quiesced;
static unsigned heartbeats, completions;
#define WDM_HEARTBEAT 0
/* Carve accounting is not what this test grades: -1 is the documented "no
 * reading" value, which hw_anchor_evidence_note skips. */
static int mem2_stat_carve_free_kb(void) { return -1; }
/* The per-frame packet path is real production code but needs the sender; the
 * note function below only asks whether the emit succeeded. */
bool hw_telemetry_emit_anchor_frames(const struct hw_anchor_evidence* a) {
    (void)a;
    return true;
}

/* VERBATIM from source/platform/hw_telemetry.c -- the ONLY writer of
 * elapsed_ms, the integer this whole test is about. */
/* HW_TELEMETRY_ANCHOR_NOTE */
static void watchdog_phase_main(int phase) { (void)phase; }
/* round 5 (3c6bedef): main drains the region I/O helper at the profile_done
 * seam; the harness has no helper, so the quiesce is a no-op here. */
static void region_archive_io_quiesce(void) {}
static void hw_boot_trace_write(const char* phase,
    const struct hw_run_config* cfg, const char* detail, bool sync) {
    (void)phase; (void)cfg; (void)detail; (void)sync;
}
static void emit_hw_heartbeat(uint32_t tick, int far_radius, int far_effective,
                              uint32_t frame) {
    (void)tick; (void)far_radius; (void)far_effective; (void)frame;
    heartbeats++;
    if(start_next) {
        /* Readiness is established inside the emitter, after its caller's
         * time sample. One millisecond is sufficient to reproduce the wrap.
         * hw_anchor_evidence_begin latches `measuring` and main.c:800-801
         * then re-bases the epoch and notes a zero -- through the REAL note
         * function sliced above. */
        clock_ticks = time_add_ms(clock_ticks, 1);
        s_anchor_started = time_get();
        s_anchor.measuring = true;
        hw_anchor_evidence_note(&s_anchor, 0, 0, mem2_stat_carve_free_kb());
        start_next = false;
    }
    /* VERBATIM from source/main.c (inside emit_hw_heartbeat): the record the
     * line is built from takes the whole anchor snapshot. */
/* MAIN_HEARTBEAT_ANCHOR_PUBLISH */
    /* hw_telemetry.c prints hb.anchor.elapsed_ms as the wire's "anchor_ms";
     * the emitter never re-samples the clock for that field (:1522). */
    last_heartbeat_anchor_ms = hb.anchor.elapsed_ms;
}
void hw_telemetry_emit_profile_done(const char* status) {
    assert(!strcmp(status, "ok"));
    completion_anchor_ms = last_heartbeat_anchor_ms;
    completions++;
}
void hw_telemetry_quiesce(void) { quiesced = true; }
void tps_observer_frame(bool measuring) { (void)measuring; }
void tps_observer_close(bool complete) { (void)complete; }

static void frame(void) {
    /* VERBATIM from source/main.c: the frame-top note is what the NEXT emitted
     * heartbeat serializes, and it reads `this_frame` -- a timestamp sampled
     * BEFORE the completion block's own. */
    ptime_t this_frame = time_get();
/* MAIN_ANCHOR_NOTE_BLOCK */
    clock_ticks = time_add_ms(clock_ticks, frame_skew_ms);
/* MAIN_COMPLETION_BLOCK */
    (void)profile_hb;
}

static void reset(void) {
    memset(&s_anchor, 0, sizeof s_anchor);
    s_anchor_started = hw_started = hw_last_heartbeat = hw_profile_done_at = 0;
    hw_heartbeat_sent = hw_watchdog_injected = hw_profile_done_sent = false;
    hw_save_quit_enabled = false;
    hw_save_quit_state = HW_SAVE_QUIT_OFF;
    s_split_recovery_done_pending = s_chat_login_done_pending = false;
    s_vehicle_done_pending = false;
    hw_tick = hw_frame_counter = 0;
    gstate.quit = start_next = quiesced = false;
    heartbeats = completions = 0;
    last_heartbeat_anchor_ms = completion_anchor_ms = 0;
    clock_ticks = 0;
}

/* The original single-sample window: the note and the block read the same
 * timestamp, so the float seconds and the emitted integer always agree. */
static int scenario_no_skew(void) {
    hw_run_config_init(&hw_run);
    hw_run_apply_profile(&hw_run, HW_RUN_PROFILE_SPLIT_ANCHOR_6M);
    hw_run.enabled = true;
    /* main.c arms the anchor observer for exactly these profiles; the sliced
     * note block and the sliced writer are both gated on it. */
    s_anchor.enabled = true;
    frame_skew_ms = 0;
    clock_ticks = time_add_ms(0, 37000);
    frame();
    assert(!s_anchor.measuring && !completions && !quiesced);

    start_next = true;
    clock_ticks = time_add_ms(clock_ticks, 1000);
    unsigned before = heartbeats;
    frame();
    assert(s_anchor.measuring);
    if(completions || quiesced || heartbeats != before + 1) {
        fprintf(stderr, "FAIL window-open: heartbeats=%u completions=%u "
            "quiesced=%d (expected 1,0,0)\n",
            heartbeats - before, completions, quiesced);
        return 1;
    }
    /* All 360 seconds must remain live, including the last millisecond. */
    for(unsigned ms = 1000; ms < 360000; ms += 1000) {
        clock_ticks = time_add_ms(s_anchor_started, ms);
        frame();
        assert(!completions && !quiesced && !gstate.quit);
    }
    clock_ticks = time_add_ms(s_anchor_started, 359999);
    frame();
    assert(!completions && !quiesced);
    clock_ticks = time_add_ms(s_anchor_started, 360000);
    frame();
    assert(completions == 1 && quiesced && !gstate.quit);
    /* The emitted contract: "ok" went out behind a heartbeat that had
     * actually reached the collector's integer gate. */
    assert(completion_anchor_ms == 360000);
    clock_ticks = time_add_ms(s_anchor_started, 364999);
    frame();
    assert(completions == 1 && !gstate.quit);
    clock_ticks = time_add_ms(s_anchor_started, 365000);
    frame();
    assert(completions == 1 && gstate.quit);
    puts("PASS main completion + Wii unsigned clock: window opens without "
        "quiescing; completes at 360 s; exits after 5 s grace");
    return 0;
}

/* push-0921/C1 deliverable 1. The SAME window with one millisecond between the
 * frame-top anchor note and the block's own time sample -- i.e. every real
 * frame. Real-Wii run 0917_092103 (split_play_8m) died exactly here: the block
 * saw 360.000 float seconds and sent "profile_done ok", while the final
 * heartbeat still reported anchor_ms 359999, and the collector failed the run
 * on its integer gate ("need six measured minutes after installed readiness",
 * tools/wii_telemetry_server.py). A complete, healthy eight minutes graded
 * FAIL. Completion must ride the emitted integer, so the run continues one
 * more frame instead. */
static int scenario_one_ms_skew(void) {
    reset();
    hw_run_config_init(&hw_run);
    hw_run_apply_profile(&hw_run, HW_RUN_PROFILE_SPLIT_PLAY_8M);
    hw_run.enabled = true;
    s_anchor.enabled = true;
    frame_skew_ms = 1;
    clock_ticks = time_add_ms(0, 37000);
    frame();
    assert(!s_anchor.measuring && !completions);

    start_next = true;
    clock_ticks = time_add_ms(clock_ticks, 1000);
    frame();
    assert(s_anchor.measuring && !completions && !quiesced);

    /* The note says 359999; the block's own sample says 360000.0 seconds. */
    clock_ticks = time_add_ms(s_anchor_started, 359999);
    frame();
    if(completions || quiesced) {
        fprintf(stderr, "FAIL 1ms skew: completed with a final heartbeat "
            "reporting anchor_ms=%u; the collector's gate is 360000 "
            "(run 0917_092103)\n", completion_anchor_ms);
        return 1;
    }
    /* One more frame, and the emitted integer has genuinely arrived. */
    clock_ticks = time_add_ms(s_anchor_started, 360000);
    frame();
    if(completions != 1 || !quiesced || completion_anchor_ms < 360000) {
        fprintf(stderr, "FAIL 1ms skew: completions=%u quiesced=%d "
            "anchor_ms=%u (expected 1,1,>=360000)\n",
            completions, quiesced, completion_anchor_ms);
        return 1;
    }
    puts("PASS completion rides the emitted integer millisecond: a frame whose "
        "heartbeat still says 359999 ms does not send profile_done ok");
    return 0;
}

static int scenario_failed_save_blocks_auto_exit(void) {
    reset();
    hw_run_config_init(&hw_run);
    hw_run_apply_profile(&hw_run, HW_RUN_PROFILE_FAR_IDLE_2M);
    hw_run.enabled = true;
    hw_run.duration_seconds = 1;
    hw_run.auto_exit = true;
    hw_run.auto_exit_grace_seconds = 5.0F;
    clock_ticks = time_add_ms(0, 1000);
    frame();
    assert(hw_profile_done_sent && completions == 1 && !gstate.quit);

    hw_save_quit_enabled = true;
    hw_save_quit_state = HW_SAVE_QUIT_FAILED;
    clock_ticks = time_add_ms(hw_profile_done_at, 5000);
    frame();
    assert(!gstate.quit && "a failed named save must remain retryable");

    hw_save_quit_state = HW_SAVE_QUIT_SUCCEEDED;
    frame();
    assert(gstate.quit && "a proven save may use the existing profile auto-exit");
    puts("PASS failed named save suppresses auto-exit until success");
    return 0;
}

int main(void) {
    int status = scenario_no_skew();
    if(status)
        return status;
    status = scenario_one_ms_skew();
    if(status)
        return status;
    return scenario_failed_save_blocks_auto_exit();
}
'''


def slice_between(text, start_marker, end_marker, what):
    """Verbatim production text between two anchors. A rename or a move raises
    ValueError here -- loud, never a silently detached copy."""
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    body = text[start:end]
    if not body.strip():
        raise SystemExit("empty slice for " + what)
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    source = (ROOT / "source/main.c").read_text(encoding="utf-8")
    telemetry = (ROOT / "source/platform/hw_telemetry.c").read_text(
        encoding="utf-8")
    start = source.index("\t\tptime_t profile_hb =")
    end = source.index("\n\t\tif(hw_run.enabled) frame_heartbeat_ms", start)
    # The three pieces that make "the block decides on the integer the
    # heartbeat publishes" true (review MEDIUM-1): the frame-top note, its
    # writer, and the publication into the heartbeat record.
    note_block = slice_between(
        source,
        "\t\tif(s_anchor.enabled) {\n\t\t\thw_anchor_evidence_note(",
        "\n\t\t/* Release-gate admission",
        "frame-top anchor note")
    publish = slice_between(source, "\t\thb.anchor = s_anchor;", "\n",
                            "heartbeat anchor publication")
    writer = slice_between(telemetry, "void hw_anchor_evidence_note(",
                           "\nstatic bool emit_observer_packet(",
                           "hw_anchor_evidence_note")
    # Fail clearly if this source slice crosses the real function boundary.
    expected_writer_fragments = (
        "a->elapsed_ms = elapsed_ms;",
        "a->frame_total++;",
        "a->frame_us[a->frame_count++] = frame_us;",
    )
    if any(fragment not in writer for fragment in expected_writer_fragments):
        raise AssertionError("anchor evidence writer slice lost expected clock/frame updates")
    if "emit_observer_packet(" in writer or "TEL_LOAD(" in writer:
        raise AssertionError("anchor evidence writer slice crossed into telemetry observer helpers")
    with tempfile.TemporaryDirectory(prefix="anchor_clock_") as scratch:
        work = Path(scratch)
        (work / "ogc").mkdir()
        # Fake 1 MHz hardware counter; the arithmetic is the real Wii time.c.
        (work / "ogc/lwp_watchdog.h").write_text(
            "#include <stdint.h>\n#define TB_TIMER_CLOCK 1000u\n"
            "#define ticks_to_microsecs(t) (t)\n"
            "uint64_t gettime(void);\nvoid settime(uint64_t);\n", encoding="utf-8")
        fixture = work / "anchor_clock.c"
        fixture.write_text(
            HARNESS
            .replace("/* HW_TELEMETRY_ANCHOR_NOTE */", writer)
            .replace("/* MAIN_ANCHOR_NOTE_BLOCK */", note_block)
            .replace("/* MAIN_HEARTBEAT_ANCHOR_PUBLISH */", publish)
            .replace("/* MAIN_COMPLETION_BLOCK */", source[start:end]),
            encoding="utf-8")
        exe = work / "anchor_clock.exe"
        subprocess.run([args.cc, "-std=c99", "-UNDEBUG", "-DPLATFORM_WII",
                        "-I", str(work), "-I", str(ROOT / "source"),
                        str(fixture), str(ROOT / "source/platform/time.c"),
                        str(ROOT / "source/game/hw_run.c"), "-o", str(exe)],
                       check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
