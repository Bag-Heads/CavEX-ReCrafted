"""Staged walk verdicts retain their world seed and name it on gate failure."""
from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("wii_telemetry_seed", ROOT / "tools/wii_telemetry_server.py")
telemetry = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = telemetry
spec.loader.exec_module(telemetry)


def check_walk(out: Path, run_id: str, moving: bool) -> dict:
    collector = telemetry.Collector(telemetry._selftest_args(
        out, run_id, "far_walk_6m", duration=360))
    with redirect_stdout(io.StringIO()):
        for i in range(300):
            collector.process({"event": "heartbeat", "run_id": run_id,
                               "profile": "far_walk_6m", "build_id": "abc_06.27",
                               "world_loaded": True, "far_view_distance": 6,
                               "fring": 1, "x": 8.0 + (i * 0.5 if moving else 0),
                               "y": 70.0, "z": 8.0, "fps": 60})
        collector.process({"event": "profile_done", "run_id": run_id,
                           "profile": "far_walk_6m", "status": "ok"})
        expected = "PASS" if moving else "FAIL"
        assert collector.write_result() == (0 if moving else 1)
    return json.loads((out / f"{expected}.json").read_text(encoding="utf-8"))


def main() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        passed = check_walk(root / "pass", "staged_far_walk_6m_s99_20260925_s12345", True)
        assert passed["seed"] == 12345
        failed = check_walk(root / "fail", "staged_far_walk_6m_20260925_s12345", False)
        assert failed["seed"] == 12345 and "seed=12345" in failed["reason"]
        unseeded = check_walk(root / "unseeded", "staged_far_walk_6m_20260925_unseeded", False)
        assert unseeded["seed"] is None and "seed=unseeded" in unseeded["reason"]
    print("PASS staged walk seed in PASS/FAIL and walk failure message")


if __name__ == "__main__":
    main()
