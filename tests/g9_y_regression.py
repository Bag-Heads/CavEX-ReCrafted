"""SHA/line-pinned verbatim Y evidence; never repairs a hardware receipt."""
import hashlib
import json
from pathlib import Path

PINNED = {
    "y_g9a_s12345": "3d820d9d6b3bd30810f5b143420f1909e85be1c96d5a0e454ad7d2d39cc61d51",
    "y_g9b_s12345": "2166c47269fef1ef7c43251226eb723be60457d62352ac12324269ee81949165",
}
EXTRACT_PINNED = {
    "y_g9a_s12345": ("260018e87fe9e48242d3a683d747479e3c43203c2c73fd272ad48470d15e72d1",
                       [*range(1, 11), *range(3090, 3099), 3119, *range(13720, 13746)]),
    "y_g9b_s12345": ("45728f279556d2495abd1d24845f470114594be26bd50b584855532bbc1062a3",
                       [*range(1, 11), *range(12960, 12989)]),
}


def replay(wire, root, collector_factory):
    manifest = json.loads((root / "tests/fixtures/g9_y_provenance.json").read_text())
    for run, pin in PINNED.items():
        info = manifest[run]
        assert info["tape_sha256"] == pin
        assert (info["sha256"], info["lines"]) == EXTRACT_PINNED[run]
        raw = (root / "tests/fixtures" / info["fixture"]).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == info["sha256"]
        lines = raw.splitlines(keepends=True)
        assert [hashlib.sha256(row).hexdigest() for row in lines] == info["row_sha256"]
        tape = root / info["source"]
        if tape.exists():
            whole = tape.read_bytes()
            assert hashlib.sha256(whole).hexdigest() == pin
            assert lines == [whole.splitlines(keepends=True)[n-1] for n in info["lines"]]
        c = collector_factory(run)
        for line in lines:
            if json.loads(line).get("event") == "service_observer":
                wire.ingest_telemetry_line(c, line.decode())
        report = c.service_observer_report()
        assert not report["coverage_complete"] and report["final_seen"]
        assert "sequence gap/duplicate" in report["errors"]
        assert "missing=4" in report["errors"]
        assert f"dropped={info['initial_dropped']}" in report["errors"]
        assert not report["receipt_latency_proven"]
        if "a_" in run:
            by_line = dict(zip(info["lines"], map(json.loads, lines)))
            assert (by_line[3093]["seq"], by_line[3095]["seq"]) == (2463, 2464)
            assert (by_line[3093]["loss"][2], by_line[3095]["loss"][2]) == (2, 3)
            assert (by_line[3090]["tel_wto"], by_line[3119]["tel_wto"]) == (0, 1)
            assert by_line[3119]["tel_fail"] == by_line[3119]["tel_abd"] == 0
            assert by_line[3119]["tel_xmax_us"] == 501191
        print("PASS verbatim Y parser/gate rejection", run, "SHA", pin)
