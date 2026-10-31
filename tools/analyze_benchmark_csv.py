import csv
import json
import sys
from pathlib import Path


def to_float(row, key):
    try:
        return float(row.get(key, 0) or 0)
    except ValueError:
        return 0.0


def to_int(row, key):
    try:
        return int(float(row.get(key, 0) or 0))
    except ValueError:
        return 0


def main():
    if len(sys.argv) != 2:
        print("usage: analyze_benchmark_csv.py <benchmark-runtime.csv>")
        sys.exit(1)

    path = Path(sys.argv[1])
    with path.open("r", newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))

    if not rows:
        print(json.dumps({"samples": 0}))
        return

    fps = [to_float(r, "fps") for r in rows]
    loaded = [to_int(r, "loaded") for r in rows]
    draw = [to_int(r, "draw") for r in rows]
    mesh = [to_int(r, "mesh") for r in rows]
    fluid = [to_int(r, "fluid") for r in rows]
    heap = [to_int(r, "heap_kib") for r in rows]
    draw_cap = [to_int(r, "draw_cap") for r in rows]
    draw_candidates = [to_int(r, "draw_candidates") for r in rows]
    draw_trimmed = [to_int(r, "draw_trimmed") for r in rows]
    draw_governed = [to_int(r, "draw_governed") for r in rows]
    elapsed = [to_float(r, "elapsed_s") for r in rows]

    # Action 4 columns may be absent in pre-instrumentation CSVs.
    mesher_rebuilds = [to_int(r, "mesher_rebuilds") for r in rows]
    mesher_us_avg = [to_int(r, "mesher_us_avg") for r in rows]
    mesher_us_max = [to_int(r, "mesher_us_max") for r in rows]
    vlight_calls = [to_int(r, "vlight_calls") for r in rows]
    vlight_us_avg = [to_int(r, "vlight_us_avg") for r in rows]
    vlight_us_max = [to_int(r, "vlight_us_max") for r in rows]

    summary = {
        "samples": len(rows),
        "elapsed_s": round(max(elapsed), 2),
        "fps_min": round(min(fps), 2),
        "fps_avg": round(sum(fps) / len(fps), 2),
        "fps_max": round(max(fps), 2),
        "loaded_max": max(loaded),
        "draw_max": max(draw),
        "mesh_max": max(mesh),
        "fluid_max": max(fluid),
        "heap_kib_min": min(heap),
        "heap_kib_max": max(heap),
        "draw_cap_max": max(draw_cap),
        "draw_candidates_max": max(draw_candidates),
        "draw_trimmed_max": max(draw_trimmed),
        "draw_governed_samples": sum(1 for v in draw_governed if v != 0),
    }

    if any(mesher_rebuilds):
        nonzero_avg = [v for v in mesher_us_avg if v > 0]
        nonzero_vl_avg = [v for v in vlight_us_avg if v > 0]
        total_rebuilds = sum(mesher_rebuilds)
        total_vlight = sum(vlight_calls)
        summary["mesher_rebuilds_total"] = total_rebuilds
        summary["mesher_rebuilds_per_s_avg"] = round(
            total_rebuilds / max(len(rows), 1), 2)
        summary["mesher_rebuilds_per_s_max"] = max(mesher_rebuilds)
        summary["mesher_us_avg_overall"] = (
            round(sum(nonzero_avg) / len(nonzero_avg), 1)
            if nonzero_avg else 0)
        summary["mesher_us_max"] = max(mesher_us_max)
        summary["vlight_calls_total"] = total_vlight
        summary["vlight_us_avg_overall"] = (
            round(sum(nonzero_vl_avg) / len(nonzero_vl_avg), 1)
            if nonzero_vl_avg else 0)
        summary["vlight_us_max"] = max(vlight_us_max)
        summary["vlight_to_rebuild_ratio"] = (
            round(sum(vlight_us_avg) / sum(mesher_us_avg), 3)
            if sum(mesher_us_avg) > 0 else 0)

    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
