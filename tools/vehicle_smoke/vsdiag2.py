"""DIAGNOSTIC ONLY (not a verdict): grade a tape with Sol's unmodified
VehicleSmokeEvidence, but (1) record every failed require and (2) answer the
single lookup one("pending", 4, stage="blocked", cause="global_pause") with the
producer's retained/global_pause row, so the predicates AFTER that known
grader/contract disagreement are evaluated too. Then run the end-of-run
finish path if the module exposes one."""
import json, sys, inspect
# O16: frozen in-tree copy; the grader module lives in ../ (tools/).
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import wii_telemetry_server as W

fails = []
def rec_require(condition, reason):
    if not condition:
        fr = inspect.stack()[1]
        fails.append((reason, fr.lineno, fr.function))
        if fr.function in ("one", "validate", "operation", "positive"):
            raise W.VehicleSmokeError(reason)
W.VehicleSmokeEvidence.require = staticmethod(rec_require)
orig_one = W.VehicleSmokeEvidence.one
def one(self, name, phase=None, **kw):
    if name == "pending" and kw.get("stage") == "blocked" and kw.get("cause") == "global_pause":
        kw = dict(kw, stage="retained")
    return orig_one(self, name, phase, **kw)
W.VehicleSmokeEvidence.one = one

if len(sys.argv) != 3 or sys.argv[2] not in ("boat", "pig", "cart"):
    # O11 / S03 #4: strict kind -- a malformed kind (e.g. "pig_") used to
    # produce a cascade of false diagnostics; refuse it instead.
    sys.stderr.write("usage: vsdiag2.py <telemetry.ndjson> boat|pig|cart (got %r)\n"
                     % (sys.argv[2:] or None,))
    sys.exit(2)
tape, kind = sys.argv[1], sys.argv[2]
rows = [json.loads(l) for l in open(tape, encoding="utf-8") if l.strip()]
vrows = [r for r in rows if str(r.get("event", "")).startswith("vehicle_smoke.")]
ev = W.VehicleSmokeEvidence(vrows[0]["run_id"], kind, vrows[0]["build_id"], "fixture_request")
for r in vrows:
    ev.error = None
    try:
        ev._note(r)
    except Exception as e:
        if not isinstance(e, W.VehicleSmokeError):
            fails.append((f"EXC {type(e).__name__}: {e}", r.get("seq"), r.get("event")))
seen = set()
for f in fails:
    if (f[0], f[1]) not in seen:
        seen.add((f[0], f[1]))
        print("FAIL", f)
print("claims", ev.claims)
print("total distinct", len(seen))
