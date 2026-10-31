#!/usr/bin/env python3
"""Read settle v2 summaries (and legacy v1 records) in a far_walk_6m telemetry.ndjson.

Paths are chronological wall intervals for the LAST blocker, clipped to the
episode. Worker service includes preemption. Dirty clocks are frame-quantized.
No overlapping per-job totals are added to estimate settle latency.
"""
import argparse
import base64
import binascii
from collections import Counter, defaultdict
from dataclasses import dataclass
import json
from pathlib import Path
import struct

STAGES = """dirty_install dirty_neighbour dirty_light dirty_lod dirty_retry
dirty_edit visit gate_neighbour gate_inflight gate_retry gate_far gate_token
gate_notvisited gather submit worker vl_begin emit_begin fin_begin bfs_begin
complete consume accept stale retry cancel draw culled evict ready slot_begin
slot_end send_begin enqueue send_end dequeue install_begin install_ok install_fail
stop_unload stop_budget stop_pool stop_notready stop_backoff stripe_begin stripe_end
skin_begin skin_end sleep_begin sleep_end alloc_begin alloc_end light_begin light_end
phases reset ep_begin ep_end ep_cancel blocker dirty_wait retire job_dequeue""".split()
WIRE = struct.Struct("<QIIiiIHBB")
assert WIRE.size == 32 and len(STAGES) <= 64


@dataclass(frozen=True)
class Event:
    us: int
    frame: int
    job_id: int
    cx: int
    cz: int
    generation: int
    epoch: int
    cy: int
    flags: int
    order: int = 0

    @property
    def stage(self):
        return STAGES[self.flags & 63]

    @property
    def mode(self):
        return "gameplay" if self.flags & 128 else "loading"

    @property
    def band(self):
        return "far" if self.flags & 64 else "near"

    @property
    def coordinate(self):
        return self.epoch, self.cx, self.cz, self.cy

    @property
    def identity(self):
        return (*self.coordinate, self.generation, self.job_id)


def decode(rows):
    events, errors = [], []
    expected = 0
    seen_run = None
    for row in rows:
        if not row.get("event", "").startswith("settle"):
            continue
        run = row.get("run_id")
        if seen_run is None:
            seen_run = run
        elif run != seen_run:
            errors.append("mixed run_id streams")
        if row["event"] == "settle_counts":
            if not row.get("valid") or row.get("overflow") or row.get("lost"):
                errors.append(f"producer invalid: overflow={row.get('overflow')} lost={row.get('lost')}")
            for left, right in (
                ("enqueue", ("dequeue", "rpc_outstanding")),
                ("submitted", ("completed", "cancelled", "outstanding")),
                ("completed", ("accepted", "stale", "retry", "result_outstanding", "result_cancelled")),
            ):
                if row.get(left) != sum(row.get(k, 0) for k in right):
                    errors.append(f"{left} reconciliation failed")
        if row["event"] != "settle":
            continue
        if row.get("v") != 1 or row.get("seq") != expected:
            errors.append(f"event sequence/version gap: expected {expected}, got {row.get('seq')}")
        expected = row.get("seq", expected) + 1
        try:
            raw = bytes.fromhex(row["hex"])
            if not raw or len(raw) % WIRE.size:
                raise ValueError("event payload is not a nonempty multiple of 32")
            for values in WIRE.iter_unpack(raw):
                if values[-1] & 63 >= len(STAGES):
                    raise ValueError("unknown stage")
                events.append(Event(*values, order=len(events)))
        except (KeyError, TypeError, ValueError) as exc:
            errors.append(f"malformed settle payload: {exc}")
    if not any(r.get("event") == "settle_counts" for r in rows):
        errors.append("no reconciliation snapshot (legacy or incomplete capture)")
    snapshots = [r for r in rows if r.get("event") == "settle_counts"]
    if snapshots and (not snapshots[-1].get("finished") or snapshots[-1].get("ring_used", 0)):
        errors.append("capture has no fully drained final snapshot")
    return events, list(dict.fromkeys(errors))


def episodes(events):
    opened, result = None, []
    for event in events:  # ring order, including frame-quantized dirty marks
        if event.stage == "ep_begin":
            if opened:
                result.append((opened, None, "missing terminal"))
            opened = event
        elif event.stage in ("ep_end", "ep_cancel"):
            if opened:
                result.append((opened, event, "complete" if event.stage == "ep_end" else "cancelled"))
                opened = None
    if opened:
        result.append((opened, None, "unfinished"))
    return result


def critical_path(events, begin, end):
    """Partition wall time using actual causal stage boundaries, not totals.

    Keep all generations of the final chunk: a superseded in-flight job can
    prevent admission of its successor. Priority selects that in-flight work
    over overlapping eligibility/dirty observations. No sum across chunks.
    """
    if end is None:
        return []
    relevant = [e for e in events if e.order < end.order
                and e.coordinate == end.coordinate]
    columns = [e for e in events if e.order < end.order and e.cy == 255
               and e.epoch == end.epoch and (e.cx, e.cz) == (end.cx, end.cz)
               and e.stage not in ("ep_begin", "ep_end", "ep_cancel", "blocker", "reset")]
    first_install = next((e for e in relevant if e.stage == "dirty_install"
                          and e.us >= begin.us), None)
    if first_install:
        start = next((e for e in reversed(columns) if e.stage == "install_begin"
                      and e.order < first_install.order), None)
        columns = [e for e in columns if start and e.job_id == start.job_id]
    else:
        columns = [] if end.cy != 255 else columns
    intervals = []

    def add(a, b, label, priority):
        lo, hi = max(begin.us, a), min(end.us, b)
        if hi > lo:
            intervals.append((lo, hi, label, priority))

    by_job = defaultdict(list)
    for e in relevant:
        if e.stage not in ("blocker", "draw", "culled", "evict", "retire"):
            by_job[e.job_id].append(e)
    transitions = {"gather": "snapshot", "submit": "worker_queue",
                   "job_dequeue": "dequeued_to_build", "worker": "worker_other", "vl_begin": "vertex_light",
                   "emit_begin": "emit", "fin_begin": "finalize",
                   "bfs_begin": "reachability", "complete": "result_queue",
                   "consume": "mesh_install"}
    for job_id, job_events in by_job.items():
        dependency_start = begin.us
        if job_id != end.job_id:
            submitted = next((e.us for e in job_events if e.stage == "submit"), None)
            retired = next((e.us for e in job_events if e.stage in ("accept", "stale", "retry", "cancel")), end.us)
            gates_for_job = [e.us for e in relevant if e.stage == "gate_inflight"
                             and submitted is not None and submitted <= e.us < retired]
            if not gates_for_job:
                continue  # same coordinates alone do not establish causality
            dependency_start = min(gates_for_job)
        current, start = None, 0
        sleeping = None
        for e in sorted(job_events, key=lambda e: (e.us, e.order)):
            if e.stage == "sleep_begin":
                sleeping = e.us
            elif e.stage == "sleep_end" and sleeping is not None:
                add(max(sleeping, dependency_start), e.us, "worker_sleep", 4)
                sleeping = None
            if e.stage in transitions or e.stage in ("accept", "stale", "retry", "cancel"):
                if current:
                    add(max(start, dependency_start), e.us, current, 3)
                current, start = transitions.get(e.stage), e.us
        if current:
            add(max(start, dependency_start), end.us, current, 3)
        if sleeping is not None:
            add(max(sleeping, dependency_start), end.us, "worker_sleep", 4)
    gates = [e for e in relevant if e.stage.startswith("gate_")]
    for i, e in enumerate(gates):
        stop = gates[i + 1].us if i + 1 < len(gates) else end.us
        closes = [x.us for x in relevant if x.us >= e.us and x.order > e.order
                  and x.stage in ("gather", "accept", "retire", "evict")]
        if closes:
            stop = min(stop, min(closes))
        add(e.us, stop, e.stage, 1)
    stream_labels = {"ready": "ready_to_send", "slot_begin": "slot_wait",
                     "slot_end": "server_payload", "send_begin": "rpc_send_wait",
                     "enqueue": "rpc_queue", "dequeue": "client_dispatch",
                     "install_begin": "column_install"}
    # Column job identity prevents a later re-send mixing with an earlier one.
    for i, e in enumerate(columns):
        if e.stage in stream_labels:
            nxt = next((x for x in columns[i + 1:] if x.job_id == e.job_id
                        and x.stage != "send_end"), None)
            if nxt:
                add(e.us, nxt.us, stream_labels[e.stage], 2)
    # Gaps are explicitly unattributed; never label them compute/admission.
    points = sorted({begin.us, end.us} | {p for a, b, _, _ in intervals for p in (a, b)})
    path = []
    for a, b in zip(points, points[1:]):
        active = [(priority, lo, label) for lo, hi, label, priority in intervals
                  if lo <= a and hi >= b]
        label = max(active)[2] if active else "unattributed"
        if path and path[-1][2] == label and path[-1][1] == a:
            path[-1] = (path[-1][0], b, label)
        else:
            path.append((a, b, label))
    assert sum(b - a for a, b, _ in path) == end.us - begin.us
    return path


def report(rows):
    events, errors = decode(rows)
    lines = ["Attribution: " + ("INVALID — " + "; ".join(errors) if errors else "valid")]
    for begin, end, status in episodes(events):
        lines.append(f"Episode {begin.job_id} {begin.mode} center=({begin.cx},{begin.cz}) {status}")
        if not end:
            continue
        lines.append(f"  last blocker={end.identity} elapsed={(end.us-begin.us)/1000:.3f} ms")
        dirties = [e for e in events if e.coordinate == end.coordinate
                   and e.order < end.order and e.stage.startswith("dirty_")
                   and e.stage != "dirty_wait" and e.generation == end.generation]
        if dirties:
            reasons = Counter(e.stage.removeprefix("dirty_") for e in dirties)
            lines.append(f"  generation {end.generation}: dirty_count={len(dirties)} "
                         f"first_dirty_us={min(e.us for e in dirties)} latest_dirty_us={max(e.us for e in dirties)} "
                         f"reasons={dict(reasons)}")
        if status != "complete":
            lines.append("  cancelled: no completed final-blocker claim")
        if errors:
            lines.append("  critical path suppressed: incomplete attribution")
        else:
            for a, b, label in critical_path(events, begin, end):
                lines.append(f"  {(a-begin.us)/1000:9.3f}..{(b-begin.us)/1000:9.3f} ms {label}")
    latest = {}
    for r in rows:
        if r.get("event") == "settle_hist":
            for i, pair in enumerate(r["pairs"], r["start"]):
                latest[r["mode"], r["far"], i] = pair
    lines.append("Stage histogram (cumulative counts / wall microseconds; overlapping totals are not a path):")
    for (m, b, i), (count, us) in sorted(latest.items()):
        if count or us:
            lines.append(f"  {('loading','gameplay')[m]} {('near','far')[b]} {STAGES[i]}: {count} / {us}")
    dirty = Counter((e.coordinate, e.generation, e.stage) for e in events
                    if e.stage.startswith("dirty_") and e.stage != "dirty_wait")
    lines.append(f"Dirty generation/reason groups: {len(dirty)} (frame-quantized timestamps)")
    worst = {r["mode"]: r for r in rows if r.get("event") == "settle_worst"}
    for m, row in sorted(worst.items()):
        identity = Event(*WIRE.unpack(bytes.fromhex(row["key"]))).identity
        lines.append(f"Worst {('loading','gameplay')[m]} job {identity}: " +
                     ", ".join(f"{k}={v}" for k, v in row.items() if k.endswith("_us")))
    return "\n".join(lines)



COUNTERS = "enqueue dequeue rpc_outstanding submitted completed cancelled outstanding worker_dequeued queue_cancelled queue_outstanding accepted stale retry result_outstanding result_cancelled calls clock_reads overflow lost".split()


def decode_summary(row):
    raw = base64.b64decode(row["b"], validate=True)
    # LENGTH-SWITCHED, back-compatible. 436 B is the pre-diagnostics v2 record
    # every hardware capture taken before the teardown fields existed; the two
    # forms are byte-identical in their first 436 bytes, so an old capture is
    # decoded in full and only the trailing pair is reported as ABSENT (None).
    # Absent is not 0: a real 0/0 means "no teardown was reached", which is a
    # measurement, while an old record simply never carried one.
    if len(raw) not in (436, 438):
        raise ValueError("summary must be 436 (pre-diagnostics) or 438 bytes")
    hist = struct.unpack_from("<63I", raw)
    counters = dict(zip(COUNTERS, struct.unpack_from("<19I", raw, 252)))
    episode, elapsed, x, z, generation, epoch, cy, mode, closed, hops, flags = struct.unpack_from("<IIiiIHBBHBB", raw, 328)
    path = [struct.unpack_from("<BII", raw, 356 + 9*i) for i in range(8)]
    end, = struct.unpack_from("<Q", raw, 428)
    # Teardown diagnostics (module statics, not capture state -- they survive a
    # retire-in-place world reset on purpose). Two saturating u8 that fit inside
    # base64's 3-byte rounding, so the record did not grow. deferred>0 means a
    # stop()/start() drain expired and carried the armed buffer; drain_ms is the
    # LAST drain's wall-clock cost (the drain polls at 1 ms, so ms is its real
    # resolution), which separates "the drain was slow" (starvation, near the
    # 200 ms budget) from "the stall never reached the drain" (0).
    #
    # SCOPE: they describe the SHUTDOWN and ARM paths only. In production
    # start()/stop() run twice per session (the --cw-tps-cpu arm, the shutdown
    # stop), so an in-session record reads 0/0 by construction -- which is the
    # answer to "did the stall reach a teardown?", not a missing number. The
    # world-reset path never drains: it retires in place, and its cost is a
    # single memset reported out of band on sd:/cw_boot_trace.txt as
    # "settle_retire_in_place n=<count> us=<memset>".
    deferred, drain_ms = (struct.unpack_from("<BB", raw, 436)
                          if len(raw) == 438 else (None, None))
    if hops > 8 or flags & ~7 or mode & 63:
        raise ValueError("invalid flags/hop count")
    if any(stage > 63 for stage, _, _ in path[:hops]):
        raise ValueError("invalid path stage")
    if (not closed and (episode or elapsed or hops or end or flags & 4)):
        raise ValueError("open episode exported as closed")
    if hops and (flags & 5 != 5 or sum(us for _, _, us in path[:hops]) != elapsed):
        raise ValueError("path does not partition closed episode")
    for left, right in (("enqueue", ("dequeue", "rpc_outstanding")),
                        ("submitted", ("completed", "cancelled", "outstanding")),
                        ("submitted", ("worker_dequeued", "queue_cancelled", "queue_outstanding")),
                        ("completed", ("accepted", "stale", "retry", "result_cancelled", "result_outstanding"))):
        if counters[left] != sum(counters[k] for k in right):
            raise ValueError(left + " reconciliation failed")
    return dict(hist=hist, counters=counters, episode=episode, elapsed=elapsed,
                key=(epoch,x,z,cy,generation), closed=closed, path=path[:hops], flags=flags,
                deferred_retirements=deferred, last_drain_ms=drain_ms)


legacy_report = report


def report(rows):
    summaries = [r for r in rows if r.get("event") == "settle"]
    if not any(r.get("v") == 2 for r in summaries):
        return legacy_report(rows)
    errors, decoded = [], []
    run = summaries[0].get("run_id")
    for seq, row in enumerate(summaries):
        try:
            if row.get("v") != 2 or row.get("seq") != seq:
                raise ValueError("sequence/version gap")
            if row.get("run_id") != run:
                raise ValueError("mixed run_id streams")
            data = decode_summary(row)
            if not data["flags"] & 1 or data["counters"]["lost"]:
                raise ValueError("producer invalid/lost")
            decoded.append(data)
        except (KeyError, TypeError, ValueError, binascii.Error) as exc:
            errors.append(str(exc))
    if not decoded or not decoded[-1]["flags"] & 2:
        errors.append("no final snapshot")
    lines = ["INVALID: " + "; ".join(errors) if errors else "VALID settle v2"]
    totals = Counter()
    for data in decoded:
        totals.update(dict(zip(STAGES, data["hist"])))
        if data["closed"]:
            lines.append(f"Closed episodes={data['closed']}; longest id={data['episode']} last blocker={data['key']} elapsed={data['elapsed']} us")
            if errors or not data["flags"] & 4:
                lines.append("  critical path unavailable: incomplete history/attribution")
            else:
                for stage, job, us in data["path"]:
                    lines.append(f"  {STAGES[stage] if stage < 63 else 'unattributed'} id={job} us={us}")
    lines.append("Stage histogram hit deltas, summed across records/mode/band:")
    lines.extend(f"  {stage}: {count}" for stage, count in totals.items() if count)
    if decoded:
        lines.append("Reconciliation: " + str(decoded[-1]["counters"]))
        diagnosed = [d for d in decoded if d["last_drain_ms"] is not None]
        if diagnosed:
            last = diagnosed[-1]
            worst_drain = max(d["last_drain_ms"] for d in diagnosed)
            lines.append("Teardown: deferred_retirements=%d last_drain_ms=%d worst_drain_ms=%d"
                         % (last["deferred_retirements"], last["last_drain_ms"], worst_drain))
        if len(diagnosed) != len(decoded):
            lines.append("Teardown: not reported by %d/%d record(s) "
                         "(436 B pre-diagnostics capture)"
                         % (len(decoded) - len(diagnosed), len(decoded)))
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("telemetry", type=Path)
    args = parser.parse_args()
    rows = []
    for n, line in enumerate(args.telemetry.read_text(encoding="utf-8-sig").splitlines(), 1):
        if line.strip():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"Invalid JSON at line {n}: {exc}") from exc
    print(report(rows))


if __name__ == "__main__":
    main()
