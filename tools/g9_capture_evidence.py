"""R21 bounded windows. CPU shares are 1 kHz estimates, not scheduler time.

MAIN occupancy is elapsed region time and can overlap. IRQ lateness/missed
deadlines are retained as gaps; they cannot be relabeled server compute.
"""
from collections import Counter

REGIONS = {0: "frame", 1: "phase", 2: "RPC", 3: "relight", 4: "flood",
           5: "mesh_gather", 6: "mesh_submit", 7: "flip/VI", 8: "gfx_finish/VI",
           9: "screen", 10: "render", 11: "input", 12: "audio", 13: "mesh_retire"}
LEASE_EVENTS = {1: "generation", 2: "begin", 3: "drop", 4: "request",
                5: "ACK", 6: "stall", 7: "service_begin", 8: "end", 9: "fault"}
DROP_REASONS = {0: "none", 1: "end", 2: "dependency", 3: "cap",
                4: "timer", 5: "audio", 6: "cancel", 7: "sender_prepare"}


def uint(value):
    return type(value) is int and 0 <= value <= 0xffffffff


class G9CaptureEvidence:
    def __init__(self):
        self.windows = {}
        self.errors = []
        self.seal = None
        self.hz = None

    def note(self, event):
        if event.get("event") == "g9_capture_seal":
            if ((type(event.get("v")) is not int or event["v"] != 1) or self.seal is not None
                    or not all(uint(event.get(k)) for k in ("candidates", "saturated", "capacity", "slots"))
                    or not isinstance(event.get("ticks"), list)
                    or any(not uint(t) for t in event["ticks"])
                    or len(event["ticks"]) > 9 or event["capacity"] != 4096 or event["slots"] != 9
                    or len(set(event["ticks"])) != len(event["ticks"])):
                self.errors.append("invalid/duplicate capture seal")
            else:
                self.seal = dict(event)
            return
        rows = event.get("rows")
        if (event.get("event") != "g9_capture" or (type(event.get("v")) is not int or event["v"] != 1)
                or type(event.get("tb_hz")) is not int or event["tb_hz"] not in (1000000, 60750000)
                or self.hz not in (None, event["tb_hz"])
                or not all(uint(event.get(k)) for k in ("tick", "offset", "total", "rt_max", "drain_max", "pop_max", "checkpoint_tick_us", "incomplete"))
                or not 1 <= event["total"] <= 4096
                or not isinstance(rows, list) or not 1 <= len(rows) <= 8
                or any(not isinstance(r, list) or len(r) != 20 or any(not uint(x) for x in r) for r in rows)):
            self.errors.append("invalid capture envelope/row")
            return
        if self.seal is not None:
            self.errors.append("capture after seal")
            return
        self.hz = event["tb_hz"]
        tick = event["tick"]
        if tick not in self.windows and len(self.windows) >= 9:
            self.errors.append("too many capture windows")
            return
        meta = {k: event[k] for k in ("total", "rt_max", "drain_max", "pop_max", "checkpoint_tick_us", "incomplete")}
        w = self.windows.setdefault(tick, dict(meta, tick=tick, rows=[]))
        if event["offset"] != len(w["rows"]) or any(w[k] != v for k, v in meta.items()):
            self.errors.append(f"window {tick}: offset/metadata mismatch")
            return
        if len(w["rows"]) + len(rows) > w["total"]:
            self.errors.append(f"window {tick}: row count overflow")
            return
        w["rows"].extend(rows)

    def _window(self, w):
        errors = []
        rows = w["rows"]
        scale = 1000000 / self.hz
        points = [(r[0], (r[1] << 32) | r[2], r[3], r[4], r[5:]) for r in rows]
        if any(b[1] < a[1] for a, b in zip(points, points[1:])):
            errors.append("reversed timestamps")
        complete = (len(rows) == w["total"] and bool(points) and points[0][0] == 1
                    and points[-1][0] == 12 and not w["incomplete"])
        if points and (points[0][4][0] == 0 or any(p[0] == 1 for p in points[1:])
                       or any(p[0] == 12 for p in points[:-1])):
            errors.append("invalid tick boundaries/owner")
        if not complete:
            errors.append("incomplete or capacity-truncated window")
        start = points[0][1] if points else 0
        end = points[-1][1] if points else start
        elapsed = (end-start)*scale if complete else None
        owner = points[0][4][0] if points else 0
        regions = points[0][4][1] if points else 0
        last_region = start
        occupancy = Counter()
        state = Counter()
        sample_rows, leases, drains, checkpoints, audio = [], [], {}, [], []
        open_stages = {}
        missed, late, late_max, sample_last = 0, 0, 0, start
        sampler_irq_us = sampler_irq_max = 0
        components = None
        boosted = set()
        for kind, tb, drain, generation, d in points:
            if kind == 2:
                if tb <= sample_last:
                    errors.append("duplicate/reversed sample")
                sample_last = tb
                bucket = ("server_compute" if d[0] == owner else
                          "runnable_wait" if d[4] == 2 and d[5] == 0 else
                          "blocked" if d[4] in (3, 4) or d[5] else "unknown")
                state[bucket] += 1000
                missed += d[11]; late += d[10] > 50; late_max = max(late_max, d[10])
                sampler_irq_us += d[13]; sampler_irq_max = max(sampler_irq_max, d[13])
                sample_rows.append(dict(tb=tb, drain=drain, generation=generation,
                    thread=d[0], role=d[1], effective=d[2], base=d[3], state=d[4],
                    suspend=d[5], queue=d[6], token=d[7], main_phase=d[8], pc=d[9],
                    late_us=d[10], missed=d[11], regions=d[12], sampler_irq_us=d[13], bucket=bucket))
            elif kind == 3:
                if d[0] not in REGIONS or d[1] not in (1, 2):
                    errors.append("invalid MAIN mark")
                    continue
                for region, name in REGIONS.items():
                    if regions & (1 << region): occupancy[name] += (tb-last_region)*scale
                expected = regions | (1 << d[0]) if d[1] == 1 else regions & ~(1 << d[0])
                if expected != d[3]: errors.append("MAIN region mask mismatch")
                regions = d[3]; last_region = tb
            elif kind == 4:
                if d[0] not in (1, 2): errors.append("invalid random edge")
            elif kind == 5:
                if not drain or drain in drains: errors.append("duplicate/zero drain ID")
                drains[drain] = dict(id=drain, world=d[0], world_generation=d[1],
                    world_tick=d[2], stage=d[3], seeds=d[4], begin_tb=tb,
                    leases=0, drops=Counter(), ACKs=[])
            elif kind in (6, 7, 8, 9, 14, 15):
                record = drains.get(drain)
                if record is None:
                    errors.append("orphan drain boundary")
                    continue
                key = {6: "kernel_return_tb", 7: "restore_return_tb", 8: "record_enter_tb",
                       9: "record_return_tb", 14: "old_end_tb", 15: "record_restore_return_tb"}[kind]
                if key in record: errors.append("duplicate drain boundary")
                record[key] = tb
                if kind == 6:
                    record.update(pops=d[0], result=d[1], final_pop_tb=(d[2] << 32) | d[3])
                if kind == 14: record["old_elapsed_us"] = d[0]
            elif kind == 10:
                if d[0] not in LEASE_EVENTS: errors.append("invalid lease event")
                event = dict(tb=tb, drain=drain, generation=generation,
                    event=LEASE_EVENTS.get(d[0], "unknown"), reason=d[1], turn=d[2],
                    duration_us=d[3], accepted=d[4], complete=d[5], flags=d[6], pops=d[7])
                leases.append(event)
                if d[0] == 2:
                    if not generation or generation in boosted: errors.append("duplicate/zero lease begin")
                    boosted.add(generation)
                if d[0] == 3:
                    if generation not in boosted: errors.append("orphan lease drop")
                    boosted.discard(generation)
                record = drains.get(drain)
                if record:
                    if d[0] == 2: record["leases"] += 1
                    if d[0] == 3: record["drops"][DROP_REASONS.get(d[1], str(d[1]))] += 1
                    if d[0] == 5: record["ACKs"].append(event)
            elif kind == 11:
                key = (d[1], d[2])
                if d[0] == 1 and key not in open_stages: open_stages[key] = tb
                elif d[0] == 2 and key in open_stages:
                    a = open_stages.pop(key)
                    checkpoints.append(dict(world=d[1], stage=d[2], begin_tb=a,
                                            end_tb=tb, elapsed_us=(tb-a)*scale))
                else: errors.append("unpaired checkpoint stage")
            elif kind == 12:
                components = dict(rt_us=d[0], wt_us=d[1], valid=bool(d[2]), tail_missed=d[3])
                missed += d[3]
            elif kind == 16:
                audio.append(dict(tb=tb, reserve_us=d[0], worker_pending=bool(d[1]), fx_state=d[2]))
            elif kind == 13:
                errors.append(f"capacity exhausted before kind {d[0]}")
            elif kind != 1:
                errors.append("unknown capture kind")
        if complete and boosted: errors.append("lease still boosted at tick end")
        if open_stages: errors.append("incomplete checkpoint stage")
        for region, name in REGIONS.items():
            if regions & (1 << region): occupancy[name] += (end-last_region)*scale
        for record in drains.values():
            record["drops"] = dict(record["drops"])
            for name, a, b in (("pre_restore_us", "begin_tb", "kernel_return_tb"),
                    ("restore_tail_us", "kernel_return_tb", "restore_return_tb"),
                    ("cleanup_us", "restore_return_tb", "record_enter_tb"),
                    ("record_mutex_tail_us", "record_enter_tb", "old_end_tb"),
                    ("record_return_tail_us", "old_end_tb", "record_return_tb"),
                    ("last_pop_to_kernel_return_us", "final_pop_tb", "kernel_return_tb")):
                if a in record and b in record and record[b] >= record[a]:
                    record[name] = (record[b]-record[a])*scale
            if complete and any(k not in record for k in
                    ("kernel_return_tb", "record_enter_tb", "old_end_tb", "record_return_tb")):
                errors.append("incomplete drain boundaries")
            pop = record.get("final_pop_tb", 0)
            if pop and not record["begin_tb"] <= pop <= record.get("kernel_return_tb", 0):
                errors.append("final-pop timestamp outside drain")
        return dict(tick=w["tick"], rt_max=w["rt_max"], drain_max=w["drain_max"],
            pop_max=w["pop_max"], checkpoint_tick_us=w["checkpoint_tick_us"],
            complete=complete, errors=errors, whole_tick_us=elapsed, paired_components=components,
            state_est_us=dict(state) if sample_rows else dict(server_compute=None, runnable_wait=None, blocked=None), unattributed_us=max(0, elapsed-sum(state.values())) if elapsed is not None else None,
            main_occupancy_us=dict(occupancy), samples=sample_rows,
            irq=dict(late_samples=late, max_late_us=late_max, missed_deadlines=missed,
                     runtime_us=None, sampler_irq_us=sampler_irq_us, max_sampler_irq_us=sampler_irq_max), drains=list(drains.values()), leases=leases, checkpoints=checkpoints, audio=audio)

    def report(self):
        windows = [self._window(w) for w in self.windows.values()]
        seal_matches = self.seal is not None and set(self.seal["ticks"]) == set(self.windows)
        valid = (bool(windows) and seal_matches and not self.seal["saturated"]
                 and not self.errors and all(w["complete"] and not w["errors"] for w in windows))
        return dict(method="1kHz CPU/wait estimates; MAIN elapsed occupancy overlaps; IRQ runtime unknown",
                    tb_hz=self.hz, seal=self.seal, seal_matches=seal_matches,
                    valid=valid, errors=self.errors, windows=windows)


def render_g9_capture(report):
    lines = ["# G9 bounded attribution windows", "", report["method"], "",
        f"Complete loss-free capture: {report['valid']}; seal matches: {report['seal_matches']}", "",
        "| tick | rt max | drain max | whole tick | server compute est | runnable wait est | blocked est | unattributed | MAIN elapsed occupancy (overlapping) |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for w in sorted(report["windows"], key=lambda x: x["tick"]):
        s = w["state_est_us"]
        fields = [w["tick"], w["rt_max"], w["drain_max"], w["whole_tick_us"],
                  s.get("server_compute", 0), s.get("runnable_wait", 0), s.get("blocked", 0),
                  w["unattributed_us"], ", ".join(f"{k}={v:.0f}" for k, v in w["main_occupancy_us"].items())]
        lines.append("| " + " | ".join(str(x) for x in fields) + " |")
        for d in w["drains"]:
            lines.append(f"\nTick {w['tick']} drain {d['id']} world {d['world']}:{d['world_generation']} "
                f"world_tick {d['world_tick']} stage {d['stage']}: pops={d.get('pops')}, "
                f"leases={d['leases']}, drops={d['drops']}, ACKs={len(d['ACKs'])}; "
                + ", ".join(f"{k}={d.get(k, 'UNKNOWN')}" for k in
                    ("pre_restore_us", "restore_tail_us", "cleanup_us", "record_mutex_tail_us", "record_return_tail_us")))
        lines.append(f"\nPaired rt/wt: {w['paired_components']}; checkpoints: {w['checkpoints']}; IRQ: {w['irq']}; errors: {w['errors']}\n")
    if report["errors"]: lines.append(str(report["errors"]))
    return "\n".join(lines) + "\n"
