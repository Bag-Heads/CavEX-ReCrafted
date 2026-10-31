"""REL-16-collector: SoakIdlePlayEvidence (tools/wii_telemetry_server.py) grades
the heartbeat tape by the REL-16a core rules (source/game/soak_idle_play_core.h).
A complete stable >=10800 s fake-time series PASSes; short, restarted,
wrong-world, no-play, gapped, leaking, floor, crash/watchdog and unapproved
controls FAIL with the named cause. A stuck driver that
never stops walking fails idle_not_idle (Sol real-run finding, attempt 2). LIMITS are test-only mocks."""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("soak_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules["soak_collector"] = collector
spec.loader.exec_module(collector)

LIMITS = dict(idle_s=60, play_s=60, warmup_s=120, min_play_blocks=16,
              min_play_chunks=2, max_drop_kb=512)
T0 = 7000


def series(seconds=10900, play_rate=1):
    """1 Hz heartbeats from T0 (ms); counters advance only in play phases."""
    beats, moved = [], 0
    for i in range(seconds + 1):
        moved += play_rate * (i % 120 >= 60)
        beats.append({"event": "heartbeat", "seq": i + 1, "up_ms": T0 + i * 1000,
                      "soak_process_id": 77, "soak_world_id": 12345,
                      "moved_blocks": moved, "chunks_loaded": moved,
                      "mem1_free_kb": 9000, "mem2_carve_free_kb": 6000, "oom": 0})
    return beats


def edit(start, **fields):
    def apply(beats):
        for b in beats[start:]:
            b.update({k: v(b) if callable(v) else v for k, v in fields.items()})
    return apply


def cut(lo, hi):
    def apply(beats):
        del beats[lo:hi]
    return apply


def insert(event):
    return lambda beats: beats.insert(4000, event)


def both(*fns):
    return lambda beats: [f(beats) for f in fns]


# (name, expected verdict, series kwargs, mutation, limits)
CASES = [
    ("complete_stable", "pass", {}, None, LIMITS),
    ("gap_5s_drop_512_ok", "pass", {}, both(cut(500, 504), edit(3000, mem1_free_kb=8488)), LIMITS),
    ("short_run", "short", {"seconds": 10790}, None, LIMITS),
    ("process_restarted", "process_reset", {}, edit(5000, soak_process_id=78), LIMITS),
    ("uptime_went_back", "process_reset", {}, edit(5000, up_ms=lambda b: b["up_ms"] - 4000000), LIMITS),
    ("wrong_world", "process_reset", {}, edit(5000, soak_world_id=999), LIMITS),
    ("counter_reset", "process_reset", {}, edit(5000, moved_blocks=0), LIMITS),
    ("seq_reset", "process_reset", {}, edit(5000, seq=lambda b: b["seq"] - 4999), LIMITS),
    ("no_play", "no_progress", {"play_rate": 0}, None, LIMITS),
    ("stuck_driver_walks_forever", "idle_not_idle", {},
     edit(0, moved_blocks=lambda b: b["seq"] - 1, chunks_loaded=lambda b: (b["seq"] - 1) // 16), LIMITS),
    ("phase_never_changes", "missing_phase", {}, None, dict(LIMITS, idle_s=20000)),
    ("no_heartbeats", "missing_phase", {}, cut(0, None), LIMITS),
    ("gapped_6s", "telemetry_gap", {}, cut(500, 505), LIMITS),
    ("mem1_slow_leak", "leak", {}, edit(0, mem1_free_kb=lambda b: 9000 - (b["up_ms"] - T0) // 20000), LIMITS),
    ("carve_leak", "leak", {}, edit(3000, mem2_carve_free_kb=5400), LIMITS),
    ("mem1_floor", "memory_floor", {}, edit(4000, mem1_free_kb=4095), LIMITS),
    ("carve_floor", "memory_floor", {}, edit(4000, mem2_carve_free_kb=2047), LIMITS),
    ("oom", "memory_floor", {}, edit(4000, oom=1), LIMITS),
    ("warmup_longer_than_run", "no_memory_history", {}, None, dict(LIMITS, warmup_s=20000)),
    ("zero_limit", "unapproved_limits", {}, None, dict(LIMITS, max_drop_kb=0)),
    ("missing_limits", "unapproved_limits", {}, None, None),
    ("watchdog_dump", "watchdog", {}, insert({"event": "watchdog_dump", "cause": "main_stall"}), LIMITS),
    ("fault_dump", "crash", {}, insert({"event": "fault", "cause": "dsi"}), LIMITS),
    ("missing_field", "bad_heartbeat", {}, lambda beats: beats[4000].pop("soak_world_id"), LIMITS),
]


def main():
    bad = 0
    for name, expect, kwargs, mutate, limits in CASES:
        beats = series(**kwargs)
        if mutate:
            mutate(beats)
        ev = collector.SoakIdlePlayEvidence(limits)
        for event in beats:
            ev.note(event)
        got = ev.verdict()
        bad += got != expect
        print(f"{'ok  ' if got == expect else 'FAIL'} {name}: expected {expect}, got {got}")
    print(f"{len(CASES) - bad}/{len(CASES)} cases passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
