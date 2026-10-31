#!/usr/bin/env python3
"""Export CaveWii device telemetry NDJSON as a Perfetto/Chrome JSON trace.

Only clocks carried by the device are used. Collector receipt order, heartbeat
ordinal, and aggregate durations are never converted into timestamps or spans.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any, Iterable


CLOCKS = {
    "anchor_ms": 1_000,
    "perf_duration_us": 1,
    "server_time": 50_000,  # Beta simulation ticks, explicitly not wall time.
}
IDENTITY_KEYS = {"event", "run_id", "profile", "build_id", "local_ip", "status"}


def read_ndjson(path: Path, skip_invalid: bool = False) -> list[tuple[int, dict[str, Any]]]:
    records: list[tuple[int, dict[str, Any]]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                if skip_invalid:
                    continue
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
            if not isinstance(value, dict):
                if skip_invalid:
                    continue
                raise ValueError(f"{path}:{line_number}: telemetry record is not an object")
            records.append((line_number, value))
    return records


def _device_number(value: Any) -> int | float | None:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float) and math.isfinite(value):
        return value
    return None


def flatten_counters(record: dict[str, Any]) -> dict[str, int | float]:
    counters: dict[str, int | float] = {}
    for key in sorted(record):
        if key in IDENTITY_KEYS:
            continue
        number = _device_number(record[key])
        if number is not None:
            counters[key] = number
            continue
        if isinstance(record[key], list):
            for index, value in enumerate(record[key]):
                number = _device_number(value)
                if number is not None:
                    counters[f"{key}[{index}]"] = number
    return counters


def choose_clock(records: Iterable[tuple[int, dict[str, Any]]], requested: str) -> str:
    if requested != "auto":
        return requested
    heartbeats = [record for _, record in records if record.get("event") == "heartbeat"]
    for key in ("anchor_ms", "perf_duration_us", "server_time"):
        values = [_device_number(record.get(key)) for record in heartbeats]
        if len({value for value in values if value is not None}) >= 2:
            return key
    raise ValueError("no usable device clock (anchor_ms, perf_duration_us, or server_time)")


def _timestamp_us(record: dict[str, Any], clock: str) -> int | None:
    value = _device_number(record.get(clock))
    if value is None or value < 0:
        return None
    scaled = value * CLOCKS[clock]
    if not math.isfinite(float(scaled)) or scaled < 0:
        return None
    return int(scaled)


def _sample_indices(samples: list[tuple[int, dict[str, Any], int]], every: int) -> set[int]:
    if not samples:
        return set()
    keep = {0, len(samples) - 1}
    keep.update(range(0, len(samples), every))
    # Downsampling must not erase a measured peak. Preserve every scalar/array
    # counter's first global maximum occurrence, including device running-max
    # fields whose final record may be missing after a truncated capture.
    maxima: dict[str, tuple[int | float, int]] = {}
    for index, (_, record, _) in enumerate(samples):
        for key, value in flatten_counters(record).items():
            old = maxima.get(key)
            if old is None or value > old[0]:
                maxima[key] = (value, index)
    keep.update(index for _, index in maxima.values())
    return keep


def convert_records(
    records: list[tuple[int, dict[str, Any]]],
    *,
    clock: str = "auto",
    sample_every: int = 1,
    gap_us: int = 2_000_000,
) -> dict[str, Any]:
    if sample_every < 1:
        raise ValueError("sample_every must be at least 1")
    if gap_us < 1:
        raise ValueError("gap_us must be positive")
    clock = choose_clock(records, clock)

    identity: dict[str, Any] = {}
    boundaries: list[dict[str, Any]] = []
    samples: list[tuple[int, dict[str, Any], int]] = []
    event_counts: dict[str, int] = {}
    for line_number, record in records:
        kind = str(record.get("event", "unknown"))
        event_counts[kind] = event_counts.get(kind, 0) + 1
        for key in ("run_id", "profile", "build_id"):
            if key in record:
                if key in identity and identity[key] != record[key]:
                    raise ValueError(
                        f"source line {line_number}: {key} changed; export one run/profile/build at a time")
                identity[key] = record[key]
        timestamp = _timestamp_us(record, clock)
        if kind in ("boot", "profile_done", "watchdog_dump", "crash_mailbox"):
            boundary = {"event": kind, "source_line": line_number, "timestamp_us": timestamp}
            for key in ("run_id", "profile", "build_id", "status", "cause"):
                if key in record:
                    boundary[key] = record[key]
            boundaries.append(boundary)
        if kind == "heartbeat" and timestamp is not None:
            samples.append((line_number, record, timestamp))

    if not samples:
        raise ValueError(f"no heartbeat carries the selected device clock {clock!r}")

    run_name = str(identity.get("run_id", "CaveWii run"))
    profile = str(identity.get("profile", "unknown profile"))
    build = str(identity.get("build_id", "unknown build"))
    trace_events: list[dict[str, Any]] = [
        {"ph": "M", "pid": 1, "tid": 0, "name": "process_name", "args": {"name": f"{run_name}; {profile}; {build}"}},
        {"ph": "M", "pid": 1, "tid": 0, "name": "process_labels", "args": {"labels": f"{profile}; {build}"}},
    ]

    keep = _sample_indices(samples, sample_every)
    # Keep both sides of every discontinuity even when neither contains a
    # global peak. Otherwise the counter trace can hide the observed reset.
    for index in range(1, len(samples)):
        delta = samples[index][2] - samples[index - 1][2]
        if delta < 0 or delta > gap_us:
            keep.update((index - 1, index))
    segment = 0
    previous_timestamp: int | None = None
    emitted = 0
    reset_count = 0
    gap_count = 0
    for index, (line_number, record, timestamp) in enumerate(samples):
        if previous_timestamp is not None and timestamp < previous_timestamp:
            segment += 1
            reset_count += 1
            trace_events.append({
                "ph": "i", "s": "t", "pid": 1, "tid": segment + 1,
                "ts": timestamp, "name": "device clock reset",
                "args": {"clock": clock, "previous_us": previous_timestamp,
                         "current_us": timestamp, "source_line": line_number},
            })
        elif previous_timestamp is not None and timestamp - previous_timestamp > gap_us:
            gap_count += 1
            trace_events.append({
                "ph": "i", "s": "t", "pid": 1, "tid": segment + 1,
                "ts": timestamp, "name": "device clock gap",
                "args": {"clock": clock, "gap_us": timestamp - previous_timestamp,
                         "source_line": line_number},
            })
        if index in keep:
            args = flatten_counters(record)
            trace_events.append({
                "ph": "C", "pid": 1, "tid": segment + 1, "ts": timestamp,
                # Perfetto imports JSON counters at process scope. A string
                # id separates reset segments even when tid is ignored.
                "id": f"segment-{segment}",
                "name": "device heartbeat", "args": args,
            })
            emitted += 1
        previous_timestamp = timestamp

    return {
        "traceEvents": trace_events,
        "displayTimeUnit": "ms",
        "cavewii": {
            **identity,
            "clock": clock,
            "clock_semantics": (
                "Beta simulation time at 20 ticks/s" if clock == "server_time"
                else "device cumulative measured time"
            ),
            "boundaries": boundaries,
            "event_counts": dict(sorted(event_counts.items())),
            "heartbeat_samples_seen": len(samples),
            "heartbeat_samples_emitted": emitted,
            "clock_resets": reset_count,
            "clock_gaps": gap_count,
            "untimed_boundaries_have_null_timestamp": True,
        },
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="telemetry.ndjson input")
    parser.add_argument("output", type=Path, help="Perfetto JSON output")
    parser.add_argument("--clock", choices=("auto", *CLOCKS), default="auto")
    parser.add_argument("--sample-every", type=int, default=1,
                        help="keep every Nth timestamped heartbeat; peaks are always retained")
    parser.add_argument("--gap-ms", type=float, default=2000.0,
                        help="mark consecutive device-clock gaps larger than this")
    parser.add_argument("--skip-invalid", action="store_true",
                        help="skip malformed/non-object NDJSON records")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    records = read_ndjson(args.input, args.skip_invalid)
    trace = convert_records(records, clock=args.clock, sample_every=args.sample_every,
                            gap_us=int(args.gap_ms * 1000))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(trace, sort_keys=True, separators=(",", ":")) + "\n",
                           encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
