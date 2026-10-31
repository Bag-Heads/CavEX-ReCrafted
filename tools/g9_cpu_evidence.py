"""R7 timer tapes: occupancy estimates, never exact scheduler accounting."""
from collections import Counter

ROLES = {1: "main", 2: "server", 3: "mesh", 4: "sender", 5: "watchdog"}
STAGES = {0: "drain", 1: "trial", 2: "snapshot", 3: "save", 4: "restore"}
PACING_PHASES = {0: "other", 1: "rpc", 2: "lighting", 3: "build", 4: "screen", 5: "render"}
PACING_FIELDS = ("enabled", "slice_us", "sleep_ms", "cap_ms", "checks", "foreign",
    "no_drain", "self_drain", "slice_skips", "cap_skips", "cap_frames", "sleeps",
    "requested_ms", "wake_us", "wake_max_us", "saturated",
    *("phase_" + p for p in PACING_PHASES.values()))


def g9_pacing_record(event):
    a = event.get("g9p")
    if (type(event.get("g9p_v")) is not int or event.get("g9p_v") != 1 or not isinstance(a, list) or len(a) != len(PACING_FIELDS)
            or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in a)
            or a[0] not in (0, 1) or a[15] not in (0, 1)
            or not 500 <= a[1] <= 10000 or not 1 <= a[2] <= 2 or not 1 <= a[3] <= 16):
        return None
    if not a[15] and (a[4] != a[5] + a[6] + a[7]
            or (a[0] and a[5] != a[8] + a[9] + a[11])
            or (not a[0] and any(a[8:15]))
            or a[12] != a[11] * a[2] or a[10] > a[11]
            or a[14] > a[13] or sum(a[16:]) != a[11]):
        return None
    return dict(zip(PACING_FIELDS, a), tick=event.get("tick"))


MESH = ("queued", "building", "completed", "oldest_us", "submissions", "stales", "gathering")


class G9CpuEvidence:
    def __init__(self):
        self.spans = {}
        self.main_marks = []
        self.main_open = {}
        self.main_intervals = []
        self.main_frame = 0
        self.pacing = []
        self.sleeps = []
        self.waits = {}
        self.roles = {}
        self.seq = 0
        self.lost = 0
        self.row_lost = 0
        self.produced = 0
        self.gaps = 0
        self.errors = []
        self.hz = None

    def note(self, event):
        if event.get("event") == "heartbeat":
            if "g9p" in event or "g9p_v" in event:
                p = g9_pacing_record(event)
                if p is None:
                    self.errors.append("invalid pacing heartbeat")
                elif self.pacing and (any(p[k] != self.pacing[0][k] for k in PACING_FIELDS[:4])
                        or any(p[k] < self.pacing[-1][k] for k in PACING_FIELDS[4:])):
                    self.errors.append("changed settings or regressed pacing counters")
                else:
                    self.pacing.append(p)
            return
        if event.get("event") != "g9_cpu":
            return
        hz, rows = event.get("tb_hz"), event.get("rows")
        if (type(event.get("v")) is not int or event["v"] != 1
                or type(hz) is not int or hz not in (1000000, 60750000)
                or not isinstance(rows, list) or not 1 <= len(rows) <= 8
                or (self.hz is not None and hz != self.hz)):
            self.errors.append("invalid envelope or timebase")
            return
        self.hz = hz
        for key in ("lost", "produced"):
            value = event.get(key, 0)
            if type(value) is not int or not 0 <= value <= 0xffffffff:
                self.errors.append("invalid envelope counter")
                return
        self.lost = max(self.lost, event.get("lost", 0))
        self.produced = max(self.produced, event.get("produced", 0))
        for r in rows:
            if (not isinstance(r, list) or len(r) != 24
                    or any(type(x) is not int or not 0 <= x <= 0xffffffff for x in r)):
                self.errors.append("invalid row")
                continue
            seq, lost, kind, span, hi, lo, *d = r
            if seq <= self.seq or lost < self.row_lost:
                self.errors.append("duplicate/reordered row or regressed loss")
                continue
            self.gaps += seq - self.seq - 1
            self.seq, self.row_lost = seq, lost
            self.lost = max(self.lost, lost)
            tb = (hi << 32) | lo
            if kind == 5:
                self.roles[d[0]] = ROLES.get(d[1], "other")
            elif kind == 1:
                if span in self.spans or d[1] not in (0, 1) or d[3] not in STAGES:
                    self.errors.append("invalid/duplicate begin")
                    continue
                self.spans[span] = dict(id=span, owner=d[0], sampler=bool(d[1]),
                    world=d[2], stage=STAGES[d[3]], begin_tb=tb,
                    mesh_begin=dict(zip(MESH, d[4:11])), cpu=Counter(),
                    completed_begin=d[11], retired_begin=d[12],
                    states=Counter(), waits=Counter(), phases=Counter(), pcs=Counter(),
                    priorities=Counter(), samples=0, sample_missed=0,
                    sample_late=0, last_sample_tb=tb, complete=False)
            elif kind == 2:
                s = self.spans.get(span)
                if (s is None or s["complete"] or not s["sampler"]
                        or tb <= s["last_sample_tb"]):
                    self.errors.append("orphan/out-of-order sample")
                    continue
                tid, role, effective, base, state, suspend, queue, token, phase, pc, late, missed = d[:12]
                s["last_sample_tb"] = tb
                name = self.roles.get(tid, ROLES.get(role, "other"))
                s["cpu"][(tid, name)] += 1
                # The current interrupted owner is running even if a previous
                # tick callback just queued a deferred switch. Otherwise use
                # the live KThread runnable/suspended/wait state.
                bucket = ("server" if tid == s["owner"] else
                          "runnable_off_cpu" if state == 2 and suspend == 0 else
                          "blocked" if state in (3, 4) or suspend else "unknown_state")
                s["states"][bucket] += 1
                s["waits"][(state, suspend, queue, token)] += 1
                s["phases"][phase] += 1
                s["pcs"][(tid, pc)] += 1
                s["priorities"][(tid, effective, base)] += 1
                s["samples"] += 1
                s["sample_missed"] += missed
                s["sample_late"] += late > 50
            elif kind == 3:
                s = self.spans.get(span)
                if s is None or s["complete"] or tb < s["last_sample_tb"]:
                    self.errors.append("orphan/out-of-order end")
                    continue
                s.update(end_tb=tb, pops=d[0], elapsed_us=d[1],
                    emitted_samples=d[2], late=d[3], missed=d[4],
                    mesh_end=dict(zip(MESH, d[5:12])), complete=True)
                s["completed_jobs"] = (d[12] - s["completed_begin"]) & 0xffffffff
                s["retired_jobs"] = (d[13] - s["retired_begin"]) & 0xffffffff
                s["pair_valid"] = (s["samples"] == d[2]
                    and s["sample_late"] == d[3] and s["sample_missed"] <= d[4])
            elif kind == 6:
                if d[0] not in PACING_PHASES or not d[1] or not 1 <= d[2] <= 2 or d[4] > 16:
                    self.errors.append("invalid pacing sleep")
                    continue
                self.sleeps.append(dict(seq=seq, begin_tb=tb, span_at_wake=span,
                    phase=PACING_PHASES[d[0]], foreign_owner=d[1], requested_ms=d[2],
                    wake_us=d[3], remaining_ms=d[4]))
            elif kind == 7:
                frame, region, edge, work, backlog, age, phase, effective, base = d[:9]
                if region > 10 or edge not in (1, 2) or frame < self.main_frame:
                    self.errors.append("invalid MAIN boundary")
                    continue
                self.main_frame = frame
                point = dict(frame=frame, region=region, edge=edge, work=work,
                    backlog=backlog, oldest_us=age, phase=phase, effective=effective,
                    base=base, tb=tb, span=span, mesh=dict(zip(MESH, d[9:16])))
                self.main_marks.append(point)
                key = (frame, region, phase if region == 1 else 0)
                if edge == 1:
                    if key in self.main_open:
                        self.errors.append("duplicate MAIN begin")
                    self.main_open[key] = point
                else:
                    begin = self.main_open.pop(key, None)
                    if begin is None or tb < begin["tb"]:
                        self.errors.append("orphan/reversed MAIN end")
                    else:
                        self.main_intervals.append(dict(begin=begin, end=point,
                            elapsed_us=(tb-begin["tb"])*1000000/self.hz))
            elif kind == 4:
                wid, transition, tid, token = d[:4]
                w = self.waits.setdefault(wid, {"id": wid, "returns": []})
                point = dict(tb=tb, thread=tid, token=token, state=d[4],
                    suspend=d[5], main_phase=d[6], effective=d[7], base=d[8], span=span)
                if transition == 1 and "exhaustion" not in w:
                    w["exhaustion"] = point
                elif transition == 2:
                    w["returns"].append(point)
                elif transition in (3, 4) and "terminal" not in w:
                    w["terminal"] = point
                    w["cancelled"] = transition == 4
                else:
                    self.errors.append("invalid RPC transition")
            else:
                self.errors.append("unknown row kind")

    def report(self):
        spans = []
        for s in self.spans.values():
            # Nominal 1ms observations. Missed intervals and sub-ms edges
            # remain UNATTRIBUTED; never stretch a sample across an IRQ gap.
            row = {k: v for k, v in s.items() if k not in
                ("cpu", "states", "waits", "phases", "pcs", "priorities")}
            row["cpu_est_us"] = {f"{name}@{tid:08x}": n * 1000
                                  for (tid, name), n in s["cpu"].items()}
            row["state_est_us"] = {k: n * 1000 for k, n in s["states"].items()}
            row["phase_samples"] = dict(s["phases"])
            row["wait_samples"] = [{"state": state, "suspend": suspend,
                "queue": queue, "token": token, "samples": n}
                for (state, suspend, queue, token), n in s["waits"].items()]
            row["pc_samples"] = [{"thread": tid, "pc": pc, "samples": n}
                                  for (tid, pc), n in s["pcs"].items()]
            row["priority_samples"] = [{"thread": tid, "effective": e, "base": b, "samples": n}
                                       for (tid, e, b), n in s["priorities"].items()]
            if s["complete"]:
                row["unattributed_us"] = max(0, s["elapsed_us"] - s["samples"] * 1000)
                row["submissions"] = (s["mesh_end"]["submissions"] - s["mesh_begin"]["submissions"]) & 0xffffffff
                row["stales"] = (s["mesh_end"]["stales"] - s["mesh_begin"]["stales"]) & 0xffffffff
            spans.append(row)
        waits = []
        for w in self.waits.values():
            row = dict(w)
            e, terminal = w.get("exhaustion"), w.get("terminal")
            returns = [r for r in w["returns"] if self.roles.get(r["thread"]) == "main"]
            if e and terminal and returns and not w.get("cancelled"):
                first = returns[0]
                matching = [r for r in returns if r["token"] == terminal["token"]]
                if e["tb"] <= first["tb"] <= terminal["tb"]:
                    row["consumer_us"] = (first["tb"] - e["tb"]) * 1000000 / self.hz
                    row["post_return_us"] = (terminal["tb"] - first["tb"]) * 1000000 / self.hz
                    if matching:
                        row["matching_token_post_return_us"] = (terminal["tb"] - matching[-1]["tb"]) * 1000000 / self.hz
                else:
                    row["error"] = "RPC timestamp order"
            waits.append(row)
        phase_counts = dict(Counter(s["phase"] for s in self.sleeps))
        sleep_counts_match = bool(self.pacing) and (
            self.pacing[-1]["sleeps"] == len(self.sleeps)
            and self.pacing[-1]["requested_ms"] == sum(s["requested_ms"] for s in self.sleeps)
            and self.pacing[-1]["wake_us"] == sum(s["wake_us"] for s in self.sleeps)
            and all(self.pacing[-1]["phase_" + p] == phase_counts.get(p, 0)
                    for p in PACING_PHASES.values()))
        return dict(main_trace=dict(marks=self.main_marks, intervals=self.main_intervals,
                    incomplete=list(self.main_open.values()),
                    valid=bool(self.main_marks) and not self.main_open and not self.errors
                        and not self.lost and not self.gaps and self.produced <= self.seq
                        and not any(m["oldest_us"] == 0xffffffff for m in self.main_marks)),
                    method="1kHz occupancy estimates; IRQ time not separately measured",
                    tb_hz=self.hz, lost=self.lost,
                    sequence_gaps=self.gaps + max(0, self.produced - self.seq),
                    errors=list(self.errors), spans=spans, rpc=waits,
                    pacing=dict(heartbeats=self.pacing, sleeps=self.sleeps,
                        phase_counts=phase_counts, sleep_counts_match=sleep_counts_match,
                        valid=sleep_counts_match and not self.errors and not self.lost
                            and not self.gaps and self.produced <= self.seq
                            and not self.pacing[-1]["saturated"]))


def render_g9_cpu(report):
    lines = ["# G9 CPU occupancy (sample estimates)", "",
        f"Lost records: {report['lost']}; sequence gaps: {report['sequence_gaps']}; errors: {len(report['errors'])}.",
        "Missing samples, IRQ time and sub-ms edges are not assigned to a CPU owner.", "",
        "| Span/world/stage | pops | elapsed us | server~us | runnable off CPU~us | blocked~us | CPU owners~us | samples/late/missed | mesh Q/B/C/G begin→end; oldest us | submits/completes/retires/stales | unassigned us | complete/valid |",
        "|---|---:|---:|---:|---:|---:|---|---|---|---|---:|---|"]
    for s in report["spans"]:
        state = s["state_est_us"]
        owners = ", ".join(f"{k}={v}" for k, v in s["cpu_est_us"].items()) or "unobserved"
        def mesh(m):
            return "/".join(str(m[k]) for k in ("queued", "building", "completed", "gathering")) + f" age={m['oldest_us']}"
        mesh_text = mesh(s["mesh_begin"]) + " → " + (mesh(s["mesh_end"]) if s["complete"] else "?")
        lines.append(f"| {s['id']}/{s['world']}/{s['stage']} | {s.get('pops', '?')} | {s.get('elapsed_us', '?')} | {state.get('server', 0)} | {state.get('runnable_off_cpu', 0)} | {state.get('blocked', 0)} | {owners} | {s['samples']}/{s.get('late', '?')}/{s.get('missed', '?')} | {mesh_text} | {s.get('submissions', '?')}/{s.get('completed_jobs', '?')}/{s.get('retired_jobs', '?')}/{s.get('stales', '?')} | {s.get('unattributed_us', '?')} | {s['complete']}/{s.get('pair_valid', False)} |")
    lines += ["", "RPC exhaustion → first MAIN token publication → server reacquisition:",
              "| Wait | consumer us | post-return us | acquired-token post-return us | cancelled |", "|---|---:|---:|---:|---|"]
    for w in report["rpc"]:
        lines.append(f"| {w['id']} | {w.get('consumer_us', '?')} | {w.get('post_return_us', '?')} | {w.get('matching_token_post_return_us', '?')} | {w.get('cancelled', '?')} |")
    p = report["pacing"]
    lines += ["", "Pacing engagement (cumulative since startup; wake includes scheduler delay):",
              f"Valid evidence: {p['valid']}; sleep phases: {p['phase_counts']}."]
    if p["heartbeats"]:
        lines.append(str(p["heartbeats"][-1]))
    lines += ["", "| Sleep seq | start TB | observed owner | phase | requested ms | wake us | remaining ms |",
              "|---|---:|---:|---|---:|---:|---:|"]
    for s in p["sleeps"]:
        lines.append(f"| {s['seq']} | {s['begin_tb']} | {s['foreign_owner']} | {s['phase']} | {s['requested_ms']} | {s['wake_us']} | {s['remaining_ms']} |")
    mt = report["main_trace"]
    lines += ["", f"MAIN boundary trace: {len(mt['marks'])} markers, "
        f"{len(mt['intervals'])} paired intervals, {len(mt['incomplete'])} open; valid={mt['valid']}. "
        "Intervals are elapsed, not CPU time; raw frame/TB/phase/work/backlog/age/mesh and priorities are in JSON."]
    return "\n".join(lines) + "\n"
