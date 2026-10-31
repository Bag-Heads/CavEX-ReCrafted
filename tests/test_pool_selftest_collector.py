"""colcap-1006 (ASTRA-CC-R5 item 2): pool_selftest_3m grading through the
production collector, fed the REAL C formatter's event lines
(test_pool_selftest --emit-events: source/game/pool_selftest.c on the PC
column-pool emulation), plus the cross-run ordered-GX verdict (--gx-compare).

  * the lowest and rotating runs certify (PASS.json) on their own;
  * unstable frames, a damaged ballast, a never-settled scene, a failed or
    malformed churn, a missing/duplicate/out-of-order event and a placement
    switch mid-run all FAIL;
  * gx_compare: lowest vs rotating of one build = PASS; different builds,
    same placement twice or a non-PASS run = FAIL; equal placement digests,
    a different camera or different world content = INCONCLUSIVE; same
    content and order with different ordered bytes = FAIL.
"""
import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import wii_telemetry_server as telemetry  # noqa: E402

PROFILE = "pool_selftest_3m"


def emitted():
    exe_name = "test_pool_selftest.exe" if os.name == "nt" else "test_pool_selftest"
    exe = Path.cwd() / exe_name
    if not exe.exists():
        exe = ROOT / "build-verify" / exe_name
    out = subprocess.run([str(exe), "--emit-events"], capture_output=True,
                         text=True, check=True).stdout
    scenarios, cur = {}, None
    for line in out.splitlines():
        if line.startswith("#scenario "):
            cur = line.split()[1]
            scenarios[cur] = []
        elif line.startswith("{") and cur:
            scenarios[cur].append(json.loads(line))
    return scenarios


SC = emitted()


def beat(tick, build="abc_06.27"):
    return dict(event="heartbeat", run_id="ps-run", profile=PROFILE,
                build_id=build, tick=tick, fps=60, world_loaded=1)


def tape(events, build="abc_06.27", done="ok"):
    rows = [beat(t, build) for t in range(1, 6)]
    rows += [dict(e, run_id="ps-run") for e in events]
    rows += [beat(t, build) for t in range(6, 40)]
    if done is not None:
        rows.append(dict(event="profile_done", run_id="ps-run", status=done))
    return rows


def grade(rows, out=None):
    td = None
    if out is None:
        td = tempfile.TemporaryDirectory()
        out = Path(td.name)
    args = telemetry._selftest_args(Path(out), "ps-run", PROFILE)
    c = telemetry.Collector(args)
    c.world_loaded_seen = True
    for row in rows:
        c.process(row)
        if c.done:
            break
    c.hb_count = max(c.hb_count, c.profile["min_hb"])
    if not c.done:
        c._finish_normal("duration reached")
    c.write_result()
    files = {n for n in ("PASS.json", "FAIL.json", "NONCERTIFYING.json")
             if (Path(out) / n).is_file()}
    c._td = td
    return c, files


def test_real_formatter_runs_certify():
    for name in ("lowest", "rotating"):
        c, files = grade(tape(SC[name]))
        assert files == {"PASS.json"}, (name, c.fail_reason, c.pool_selftest.errors)
        s = c.pool_selftest.summary()
        assert s["placement"] == name and s["alloc_us_avg"] is not None
        print("PASS %s run certifies (alloc avg %s us, free avg %s us)"
              % (name, s["alloc_us_avg"], s["free_us_avg"]))


def test_failures():
    cases = {
        "unstable": SC["unstable"],
        "ballast": SC["ballast"],
        "unsettled": SC["unsettled"],
    }
    for name, events in cases.items():
        done = [e for e in events if e.get("phase") == "final"][0].get("status")
        c, files = grade(tape(events, done=done if done != "ok" else "ok"))
        assert files == {"FAIL.json"}, (name, files)
        print("PASS %s -> FAIL (%s)" % (name, c.fail_reason))
    good = SC["lowest"]
    churn, scene, final = good

    def mut(fn):
        ev = copy.deepcopy(good)
        fn(ev)
        return ev
    muts = {
        "churn ok=false": mut(lambda e: e[0].update(ok=False, reason="x")),
        "pattern_bad": mut(lambda e: e[0].update(pattern_bad=1)),
        "canary hit": mut(lambda e: e[0].update(canary_delta=2)),
        "refusal not exercised": mut(lambda e: e[0].update(shrink_fail=0)),
        "kept slab unused": mut(lambda e: e[0].update(kept_slab_reused=False)),
        "no growth": mut(lambda e: e[0].update(slabs_peak=4)),
        "lowest unchecked": mut(lambda e: e[0].update(lowest_checks=0)),
        "malformed churn": mut(lambda e: e[0].update(ops=-1)),
        "unstable scene": mut(lambda e: e[1].update(stable=False)),
        "empty scene": mut(lambda e: e[1].update(lists=0)),
        "bad hash": mut(lambda e: e[1].update(gx="xyz")),
        "no game takes": mut(lambda e: e[1].update(ptakes=0)),
        "ballast damaged": mut(lambda e: e[2].update(ballast_bad=1, ok=False)),
        "placement switched": mut(lambda e: e[2].update(placement="rotating")),
        "final missing": good[:2],
        "scene missing": [good[0], good[2]],
        "churn missing": good[1:],
        "duplicate scene": [good[0], good[1], good[1], good[2]],
    }
    for name, events in muts.items():
        c, files = grade(tape(events))
        assert files == {"FAIL.json"}, (name, files)
        assert c.failure_class in ("pool_selftest_failed", "profile_done_error"), (name, c.failure_class)
    print("PASS %d mutations of the good run -> FAIL" % len(muts))


def run_dir(events, build="abc_06.27"):
    td = tempfile.TemporaryDirectory()
    rows = tape(events, build)
    for r in rows:
        if r.get("event") == "heartbeat":
            r["build_id"] = build
    args = telemetry._selftest_args(Path(td.name), "ps-run", PROFILE,
                                    expected_build=build)
    c = telemetry.Collector(args)
    c.world_loaded_seen = True
    for row in rows:
        c.process(row)
        if c.done:
            break
    c.hb_count = max(c.hb_count, c.profile["min_hb"])
    if not c.done:
        c._finish_normal("duration reached")
    c.write_result()
    return td


def test_gx_compare():
    lo, ro = SC["lowest"], SC["rotating"]
    a, b = run_dir(lo), run_dir(ro)
    v = telemetry.gx_compare(a.name, b.name)
    assert v["status"] == "PASS", v
    print("PASS gx_compare lowest vs rotating: %s (%s vs %s)"
          % (v["status"], v["pdigest"][0], v["pdigest"][1]))

    def variant(events, **scene):
        ev = copy.deepcopy(events)
        ev[1].update(scene)
        return ev
    cases = [
        ("different build", run_dir(ro, build="abd_06.27"), "FAIL"),
        ("same placement twice", run_dir(lo), "FAIL"),
        ("equal placement digest", run_dir(variant(ro, pdigest=lo[1]["pdigest"])), "INCONCLUSIVE"),
        ("camera differs", run_dir(variant(ro, cam=[1, 2, 3, 4, 5])), "INCONCLUSIVE"),
        ("content differs", run_dir(variant(ro, content="0" * 16)), "INCONCLUSIVE"),
        ("order differs, same lists", run_dir(variant(ro, pos="1" * 16)), "INCONCLUSIVE"),
        ("order differs, lists differ", run_dir(variant(ro, pos="1" * 16, gx_set="2" * 16)), "FAIL"),
        ("ordered bytes differ", run_dir(variant(ro, gx="3" * 16)), "FAIL"),
    ]
    for name, other, want in cases:
        v = telemetry.gx_compare(a.name, other.name)
        assert v["status"] == want, (name, v)
        print("PASS gx_compare %s -> %s" % (name, want))
    v = telemetry.gx_compare(a.name, str(Path(a.name) / "missing"))
    assert v["status"] == "FAIL"
    # The CLI writes the verdict and exits 0 only on PASS.
    out = Path(a.name) / "GX_EQUIV.json"
    rc = subprocess.run([sys.executable, str(ROOT / "tools/wii_telemetry_server.py"),
                         "--gx-compare", a.name, b.name, "--gx-compare-out", str(out)],
                        capture_output=True, text=True).returncode
    assert rc == 0 and json.loads(out.read_text())["status"] == "PASS"
    print("PASS --gx-compare CLI writes GX_EQUIV.json, rc 0")


if __name__ == "__main__":
    # Boot ordering is part of the guarantee; the linked world golden proves
    # the behavior. No config key or later camera-settle latch may arm this.
    source = (ROOT / "source/main.c").read_text(encoding="utf-8")
    arm = source.index("server.pool_selftest_freeze = hw_run.enabled && hw_run.pool_selftest;")
    assert source.index("server_local_init(&server,") < arm < source.index("server_local_start(&server)")
    print("PASS pool freeze armed only by profile before first server tick")
    test_real_formatter_runs_certify()
    test_failures()
    test_gx_compare()
    print("PASS test_pool_selftest_collector")
