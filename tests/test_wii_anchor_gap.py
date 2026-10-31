#!/usr/bin/env python3
"""The split_play_8m frame stream never certifies an unseen interval."""
import importlib.util
import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "wii_telemetry_server", ROOT / "tools" / "wii_telemetry_server.py")
telemetry = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = telemetry
spec.loader.exec_module(telemetry)


def run_case(out, missing_ticks, drop_tick=None, counter_values=None,
             absent_counters=()):
    base = dict(**telemetry.PC_POOL_EVIDENCE, near_radius=2, far_view_distance=6, far_storage=0,
                anchor_frame_dropped=0, tick_backlog=0,
                anchor_load_carve_min=1000, anchor_run_carve_min=2246,
                anchor_near_owed=[0, 0], anchor_oldest=[0, 0],
                anchor_missing=[0, 0], anchor_mesh_pending=[0, 0],
                anchor_far_missing=[0, 0], anchor_far_max=[0, 0],
                anchor_ready_streak=3, anchor_far_mesh_pending=[0, 0],
                near_missing=0, near_mesh_pending=0, far_missing=0,
                anchor_load_faults=[0, 0, 0], anchor_run_faults=[0, 0, 0],
                oom=0, spill=0, mesh_trunc=0, tel_drop=0, tel_fail=0,
                tel_wto=0, tel_abd=0, tel_hb_repl=0, anchor_measuring=True,
                anchor_ready=True)
    c = telemetry.Collector(telemetry._selftest_args(
        out, out.name, "split_play_8m"))
    for tick in range(361):
        if tick in missing_ticks:
            continue
        hb = {**base, "tick": tick, "anchor_ms": tick * 1000,
              "server_time": tick * 20, "anchor_frame_total": tick + 1,
              "anchor_frame_us": [16667],
              "anchor_frame_dropped": 1 if tick == drop_tick else 0}
        for counter, (before, after) in (counter_values or {}).items():
            hb[counter] = before if tick < 101 else after
        for counter in absent_counters:
            hb.pop(counter, None)
        c.anchor.note(hb)
    c.hb_count = int(c.profile["min_hb"])
    c.world_loaded_seen = True
    c.walk_min_x = c.walk_min_z = 0.0
    c.walk_max_x = c.walk_max_z = 64.0
    with redirect_stdout(io.StringIO()):
        c.process(dict(event="profile_done", run_id=c.args.run_id, status="ok"))
        rc = c.write_result()
    path = out / ("PASS.json" if (out / "PASS.json").exists() else "FAIL.json")
    return rc, json.loads(path.read_text(encoding="utf-8")), path.name, (
        out / "summary.md").read_text(encoding="utf-8")


def main():
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        rc, result, name, summary = run_case(root / "single", {100})
        assert rc == 1 and name == "FAIL.json" and result["status"] == "INCONCLUSIVE"
        anchor = result["anchor"]
        assert anchor["inconclusive_windows"] == 1
        assert anchor["missing_ticks"] == [100]
        assert anchor["missing_frames"] == 1 and anchor["frames"] == 360
        assert anchor["frame_coverage"] == 360 / 361
        assert "telemetry coverage gap" in summary
        assert "inconclusive windows=1" in summary

        rc, result, name, _ = run_case(
            root / "zero_deltas", {100}, counter_values={
                counter: (3, 3) for counter in
                ("tel_drop", "tel_fail", "tel_wto", "tel_abd", "tel_hb_repl")})
        assert rc == 1 and name == "FAIL.json" and result["status"] == "INCONCLUSIVE"

        for label, kwargs in (("tel_wto_increase", {"counter_values": {"tel_wto": (0, 1)}}),
                              ("tel_wto_absent", {"absent_counters": ("tel_wto",)})):
            rc, result, name, _ = run_case(root / label, {100}, **kwargs)
            assert rc == 1 and name == "FAIL.json" and result["status"] == "FAIL", label
            assert result["failure_class"] == "anchor_feasibility_failed", label
            assert "missing per-frame samples" in result["reason"], label

        for label, ticks, drop in (("two", {100, 200}, None),
                                   ("drop", {100}, 101),
                                   ("delta3", {100, 101}, None)):
            rc, result, name, _ = run_case(root / label, ticks, drop)
            assert rc == 1 and name == "FAIL.json" and result["status"] == "FAIL", label
            assert result["failure_class"] == "anchor_feasibility_failed", label
            assert "missing per-frame samples" in result["reason"], label
    print("PASS split anchor coverage gap verdicts: zero deltas INCONCLUSIVE; "
          "two/drop/delta3/tel_wto increase/absent FAIL")


if __name__ == "__main__":
    main()
