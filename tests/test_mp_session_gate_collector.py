"""REL-MP-GATE: an mp profile must FAIL (mp_session_ended) when the session
ends or the world unloads after the login window, on the REAL tapes.

hw1018_e677b79_8_mp_lan_walk_10m reported PASS on "duration 600s reached"
although its session ended at heartbeat 31 (mp_end_reason "remote deferred
edit log exhausted", world_loaded false on 565 of 593 beats). hw1014_e5 is the
healthy control (world loaded from beat 3, connected 591/593).

The fixtures are heartbeat-only excerpts of the two retained tapes, every
heartbeat in order, cut to the fields the mp_lan_walk_10m gates read
(event, run_id, profile, seq, up_ms, world_loaded, mp_state, mp_gen, mp_end,
mp_end_reason, mp_move_ticks, mp_move_samples, rpcdrop, lw_to_admits, x, z). Sources:
tmp/wii_runs/hw1018_e677b79_8_mp_lan_walk_10m_s12345/telemetry.ndjson
(md5 a7d304dc623eef89a0b6d97dce179384) and
tmp/wii_runs/hw1014_e5_mpwalk_s12345/telemetry.ndjson
(md5 e0c5ddca6a26e8c208b423c2183c8e86). Before the gate both replayed PASS. They are replayed through the
collector exactly as --replay does, into a temporary directory.

The boundary cases below are SYNTHETIC: the hw1014_e5 heartbeats with single
fields edited. mp_play_10m is NOT gated by this check (maintainer narrowing
2026-10-18; it has its own MpPlayEvidence collector), proved by grading the
RED tape as mp_play_10m and asserting the gate never latches."""
import importlib.util
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))  # the collector imports its siblings
spec = importlib.util.spec_from_file_location(
    "mp_session_collector", ROOT / "tools/wii_telemetry_server.py")
collector = importlib.util.module_from_spec(spec)
sys.modules["mp_session_collector"] = collector
spec.loader.exec_module(collector)

FIXTURES = ROOT / "tests/fixtures/mp_session_gate"
RED = (FIXTURES / "hw1018_e677b79_8_mp_lan_walk_10m.ndjson",
       "hw1018_e677b79_8_mp_lan_walk_10m_s12345")
GREEN = (FIXTURES / "hw1014_e5_mp_lan_walk_10m.ndjson", "hw1014_e5_mpwalk_s12345")


def grade(tape, run_id, profile="mp_lan_walk_10m", join_ms=3000):
    """join_ms: load_screen_ms stamped on heartbeat 3 (the fixtures predate the
    field; REL-MP-JOINGATE needs one). None leaves the tape's own value."""
    assert tape.is_file(), "missing fixture " + str(tape)
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "replay"
        out.mkdir()
        c = collector.Collector(collector._selftest_args(
            out, run_id, profile, expected_build="", duration=0))
        for n, line in enumerate(
                tape.read_text(encoding="utf-8").splitlines(True), 1):
            if join_ms is not None and line.strip():
                hb = json.loads(line)
                hb["load_screen_ms"] = join_ms if n == 3 else 0
                line = json.dumps(hb) + "\n"
            if c.done:
                break
            if collector.ingest_telemetry_line(c, line):
                break
        if not c.done:
            c._finish_normal("offline replay of " + str(tape))
        return c


def grade_lines(lines, run_id, profile, join_ms=3000):
    with tempfile.TemporaryDirectory() as td:
        tape = Path(td) / "synthetic.ndjson"
        tape.write_text("".join(lines), encoding="utf-8")
        return grade(tape, run_id, profile, join_ms)


def beats():
    """The hw1014_e5 heartbeats as dicts, in order."""
    return [json.loads(l) for l in GREEN[0].read_text(encoding="utf-8")
            .splitlines() if l.strip()]


def edited(edits):
    """hw1014_e5 heartbeats with `edits` {beat number (1-based): {field: value}}
    applied. Returns ndjson lines."""
    return [json.dumps(dict(hb, **edits.get(n, {}))) + "\n"
            for n, hb in enumerate(beats(), 1)]


def assert_pass(c, what):
    print(what + ":", c.failure_class, "|", c.fail_reason)
    assert c.failure_class is None and c.fail_reason is None, c.fail_reason
    assert c.mp_session_end is None, c.mp_session_end


def assert_ended(c, what, beat, reason, loaded):
    print(what + ":", c.failure_class, "|", c.fail_reason)
    assert c.failure_class == "mp_session_ended", c.failure_class
    assert c.mp_session_end == (beat, reason, loaded), c.mp_session_end


def main():
    window = collector.MP_SESSION_LOGIN_WINDOW_HB
    unload = {"world_loaded": False}

    # The real tapes: RED fails by name at beat 31, GREEN PASSes outright,
    # graded as both heartbeat-only gated profiles.
    for profile in ("mp_lan_walk_10m", "mp_connect_smoke"):
        red = grade(*RED, profile=profile)
        assert_ended(red, "hw1018 " + profile, 31,
                     "remote deferred edit log exhausted", False)
        assert "heartbeat 31" in red.fail_reason, red.fail_reason
        assert_pass(grade(*GREEN, profile=profile), "hw1014_e5 " + profile)

    # Profiles outside the gate never apply it, even to the RED tape:
    # mp_play_10m (judged by its own MpPlayEvidence collector) and an
    # unrelated profile.
    for profile in (collector.MP_PLAY_PROFILE, "heartbeat_selftest"):
        other = grade(*RED, profile=profile)
        print("hw1018 " + profile + ":", other.failure_class,
              other.fail_reason)
        assert other.failure_class != "mp_session_ended", other.fail_reason
        assert other.mp_session_end is None, other.mp_session_end
        assert "mp session ended" not in (other.fail_reason or ""),             other.fail_reason

    # First-loaded boundary: a terminal mp_end_reason on the very heartbeat
    # that first shows the world loaded (GREEN beat 3), with the session then
    # recovering into the next generation, still fails at that beat. The
    # current beat's loaded state counts toward "login has ended".
    first_loaded = {3: {"mp_end": 1, "mp_end_reason": "Kicked"}}
    first_loaded.update({n: {"mp_gen": 2} for n in range(4, len(beats()) + 1)})
    for profile in ("mp_lan_walk_10m", "mp_connect_smoke"):
        assert_ended(grade_lines(edited(first_loaded), GREEN[1], profile),
                     "synthetic first-loaded reason " + profile, 3,
                     "Kicked", True)

    walk = "mp_lan_walk_10m"
    # Reason-only (world still loaded) and unload-only (no reason), each on a
    # single beat with the session healthy again afterwards: the failure is
    # latched and later recovery does not clear it.
    assert_ended(grade_lines(edited({200: {"mp_end": 1, "mp_end_reason":
                                           "Kicked"}}), GREEN[1], walk),
                 "synthetic reason-only", 200, "Kicked", True)
    assert_ended(grade_lines(edited({200: unload}), GREEN[1], walk),
                 "synthetic unload-only", 200, "", False)
    # Login window: unloaded beats inside it are allowed (GREEN loads at beat
    # 3); a world still unloaded past it fails at the first beat beyond it.
    assert_pass(grade_lines(edited({n: unload for n in range(1, window + 1)}),
                            GREEN[1], walk), "synthetic load at window end")
    assert_ended(grade_lines(edited({n: unload for n in range(1, window + 6)}),
                             GREEN[1], walk),
                 "synthetic login window expired", window + 1, "", False)

    # REL-MP-JOINGATE: the first heartbeat with load_screen_ms > 0 is the join.
    joins = collector.MP_JOIN_GATE_PROFILES
    assert set(joins) == {"mp_connect_smoke", "mp_lan_walk_10m",
                          "mp_pause_menu_3m", "mp_play_10m"}, joins
    for profile in ("mp_lan_walk_10m", "mp_connect_smoke"):
        fast = grade(*GREEN, profile=profile, join_ms=3000)
        assert_pass(fast, "join 3000 " + profile)
        assert fast.join_ms == 3000, fast.join_ms
        warn = grade(*GREEN, profile=profile, join_ms=13583)
        assert_pass(warn, "join 13583 " + profile)
        assert warn.join_ms == 13583, warn.join_ms
        edge = grade(*GREEN, profile=profile, join_ms=15000)
        assert_pass(edge, "join exactly 15000 " + profile)
        slow = grade(*GREEN, profile=profile, join_ms=15001)
        assert slow.failure_class == "mp_join_too_slow", slow.failure_class
        assert slow.fail_reason == "join too slow: 15001 ms (limit 15000)",             slow.fail_reason
        missing = grade(*GREEN, profile=profile, join_ms=None)
        assert missing.failure_class == "mp_join_ms_missing",             missing.failure_class
        assert missing.join_ms is None
    # Verdict files carry join_ms and the warning field.
    for ms, want_warn, want_status in ((13583, True, "PASS"), (3000, False, "PASS"),
                                       (40077, False, "FAIL")):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "r"
            out.mkdir()
            c = collector.Collector(collector._selftest_args(
                out, GREEN[1], "mp_lan_walk_10m", expected_build="", duration=0))
            for n, line in enumerate(GREEN[0].read_text(encoding="utf-8")
                                     .splitlines(True), 1):
                hb = json.loads(line)
                hb["load_screen_ms"] = ms if n == 3 else 0
                if c.done or collector.ingest_telemetry_line(c, json.dumps(hb) + "\n"):
                    break
            if not c.done:
                c._finish_normal("offline replay")
            if not any((out / f).exists() for f in ("PASS.json", "FAIL.json")):
                c.write_result()
            doc = json.loads((out / (want_status + ".json")).read_text(encoding="utf-8"))
            assert doc["join_ms"] == ms and doc["join_ms_missing"] is False, doc["join_ms"]
            assert ("join_slow_warning" in doc) == want_warn, doc.get("join_slow_warning")
    # Profiles outside the join gate are untouched (no join fields, no fail).
    other = grade(*GREEN, profile="heartbeat_selftest", join_ms=None)
    assert other.failure_class not in ("mp_join_ms_missing", "mp_join_too_slow")

    print("test_mp_session_gate_collector: PASS")


if __name__ == "__main__":
    main()
