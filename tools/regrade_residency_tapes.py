#!/usr/bin/env python3
"""Freeze and regrade retained real-Wii residency tapes without rewriting evidence.

The output is deliberately a new evaluation, not a replacement PASS/FAIL.json.
Run from any directory with the project Python interpreter. No Wii connection is made.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import wii_telemetry_server as wts  # noqa: E402

MEAS_ROOT = Path.home() / ".codex/worktrees/colcap-meas/CaveWii_v2"
I07_ROOT = Path.home() / ".codex/worktrees/i07-1006/CaveWii_v2"
OUTPUT = ROOT / "tmp/residency_regrades"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def revision(path: Path) -> str:
    proc = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=False)
    return proc.stdout.strip() if proc.returncode == 0 else "unknown"


def sources() -> list[Path]:
    local = ROOT / "tmp/wii_runs"
    names = [f"cc_{family}{n}_s12345" for family in ("c", "f") for n in range(1, 4)]
    found = [local / name for name in names]
    found.append(MEAS_ROOT / "tmp/wii_runs/cc_m1_s12345")
    found += sorted((local).glob("r6_*_s12345"))
    found += sorted((I07_ROOT / "tmp/wii_runs").glob("i07_*_s12345"))
    return found


def freeze(src: Path, dst: Path) -> dict:
    if not src.is_dir():
        raise FileNotFoundError(src)
    dst.mkdir(parents=True, exist_ok=True)
    files = ["telemetry.ndjson", "summary.md", "latest.json", "PASS.json", "FAIL.json",
             "stage_manifest_before_run.json"]
    for name in files:
        original = src / "stage_mailbox" / name if name == "stage_manifest_before_run.json" else src / name
        if not original.is_file():
            continue
        frozen = dst / name
        if frozen.exists():
            if sha256(original) != sha256(frozen):
                raise RuntimeError(f"source changed after freeze: {original}")
        else:
            shutil.copyfile(original, frozen)
    tape = dst / "telemetry.ndjson"
    old_results = [dst / n for n in ("FAIL.json", "PASS.json") if (dst / n).is_file()]
    if not tape.is_file() or len(old_results) != 1:
        raise RuntimeError(f"expected one tape and exactly one original PASS/FAIL verdict in {src}")
    old_result = old_results[0]
    result = json.loads(old_result.read_text(encoding="utf-8"))
    events, _ = wts.residency_parse_tape_lines(tape.read_text(encoding="utf-8"))
    boots = [e for e in events if isinstance(e, dict) and e.get("event") == "boot"]
    build_ids = {e.get("build_id") for e in boots}
    if len(build_ids) != 1 or not isinstance(next(iter(build_ids)), str) or not re.fullmatch(
            r"[0-9a-f]+(?:\+[0-9a-f]+)?_[0-9]{2}\.[0-9]{2}", next(iter(build_ids))):
        raise ValueError(f"missing or inconsistent embedded build ID in {tape}")
    build_id = next(iter(build_ids))
    heartbeats = [e for e in events if isinstance(e, dict) and e.get("event") == "heartbeat"]
    staged_manifest = dst / "stage_manifest_before_run.json"
    staged_files = (json.loads(staged_manifest.read_text(encoding="utf-8-sig")).get("files", [])
                    if staged_manifest.exists() else [])
    staged_hashes = {f.get("path"): f.get("sha256") for f in staged_files if f.get("exists")}
    config = {
        "profile": result.get("profile"), "seed": result.get("seed"),
        "run_id": result.get("run_id"),
        "first_heartbeat_configuration": {
            key: heartbeats[0][key] for key in ("scenario", "far_view_distance", "view_distance",
                                               "world_slot", "guest", "seed")
            if heartbeats and key in heartbeats[0]
        },
        "staged_config_sha256": staged_hashes.get("sd:/config.json"),
        "staged_run_args_sha256": staged_hashes.get("sd:/apps/CavEX/cw_run_args.txt"),
        "staged_config_contents": "not retained with source run",
    }
    metadata = {
        "metadata_revision": 3,
        "source_directory": str(src),
        "embedded_build_source_id": build_id.rsplit("_", 1)[0],
        "source_revision_at_capture": "not retained; embedded build source ID above is available",
        "collector_revision_at_capture": "not retained with source run",
        "collector_revision_at_freeze": revision(ROOT),
        "collector_sha256_at_freeze": sha256(ROOT / "tools/wii_telemetry_server.py"),
        "build_id": build_id,
        "profile": result.get("profile"), "configuration": config,
        "frozen_files_sha256": {p.name: sha256(p) for p in dst.iterdir() if p.name in files},
    }
    manifest = dst / "freeze.json"
    if manifest.exists():
        prior = json.loads(manifest.read_text(encoding="utf-8"))
        if prior["frozen_files_sha256"] != metadata["frozen_files_sha256"]:
            raise RuntimeError(f"frozen file hashes changed: {dst}")
        if prior["build_id"] != build_id:
            raise RuntimeError(f"frozen build ID changed: {dst}")
        if prior.get("metadata_revision") == 2:
            backup = dst / "freeze.v2.json"
            if not backup.exists():
                shutil.copyfile(manifest, backup)
            metadata["collector_revision_at_freeze"] = prior["collector_revision_at_regrade"]
            metadata["collector_sha256_at_freeze"] = prior["collector_sha256_at_regrade"]
            manifest.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
            return metadata
        if prior.get("metadata_revision") != 3:
            # v1 incorrectly labeled the source worktree's current HEAD as a
            # capture revision. Keep the superseded metadata for audit, then
            # correct only our generated manifest. Raw tape and verdicts stay.
            backup = dst / "freeze.v1.json"
            if not backup.exists():
                shutil.copyfile(manifest, backup)
            manifest.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
            return metadata
        return prior
    manifest.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def derived_overall(old_status: str, failure_class: str | None,
                    new_residency: str) -> tuple[str, str]:
    """Keep observed violations ahead of missing evidence and old failures.

    A regrade cannot certify an old residency-only FAIL without a complete
    collector replay, even when this one gate now passes.
    """
    if new_residency == "FAIL":
        basis = "new residency violation blocks overall PASS"
        if old_status == "FAIL" and failure_class != "residency_gate_failed":
            basis += "; original independent failure preserved"
        return "FAIL", basis
    if old_status == "FAIL" and failure_class != "residency_gate_failed":
        return "FAIL", "original independent failure preserved"
    if new_residency != "PASS":
        return "UNPROVEN", "new residency evidence or profile policy incomplete"
    if old_status == "FAIL":
        return "UNPROVEN", "residency-only original failure needs complete collector replay"
    if old_status == "PASS":
        return "PASS", "original independent gates PASS and regraded residency PASS"
    return "UNPROVEN", "original overall verdict unavailable"


def regrade(src: Path, output: Path) -> dict:
    dst = output / src.name
    frozen = freeze(src, dst)
    old_path = next(dst / n for n in ("FAIL.json", "PASS.json") if (dst / n).exists())
    old = json.loads(old_path.read_text(encoding="utf-8"))
    _, records = wts.residency_parse_tape_lines((dst / "telemetry.ndjson").read_text(encoding="utf-8"))
    profile = old.get("profile")
    if not isinstance(profile, str) or not profile:
        raise ValueError(f"original profile absent: {src}")
    new = wts.residency_verdict_for_profile(records, profile)
    old_rz = old.get("residency_gate") or {}
    overall, overall_basis = derived_overall(old.get("status"), old.get("failure_class"),
                                             new["status"])
    report = {
        "run_id": old.get("run_id", src.name), "profile": profile,
        "source_directory": str(src), "frozen_directory": str(dst),
        "tape_sha256": frozen["frozen_files_sha256"]["telemetry.ndjson"],
        "build_id": frozen["build_id"],
        "collector_revision_at_regrade": revision(ROOT),
        "collector_sha256_at_regrade": sha256(ROOT / "tools/wii_telemetry_server.py"),
        "original": {"residency": old_rz.get("status", "not graded"),
                     "residency_codes": old_rz.get("codes", []),
                     "overall": old.get("status", "unknown"),
                     "failure_class": old.get("failure_class"),
                     "reason": old.get("reason")},
        "regrade": {"residency": new, "overall": overall,
                    "overall_basis": overall_basis},
    }
    (dst / "regrade.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    reports, missing = [], []
    for src in sources():
        try:
            reports.append(regrade(src, args.output))
        except FileNotFoundError as exc:
            missing.append(str(exc))
    lines = ["# Retained residency tape regrades", "",
             "Original PASS/FAIL artifacts and byte-identical tapes are frozen beside each regrade. "
             "The overall column preserves independent original failures; it is not a new hardware certification.", "",
             "| Run | Original profile | Old residency | New residency | Old overall | New overall | Independent original failure |",
             "|---|---|---|---|---|---|---|"]
    for r in reports:
        old, new = r["original"], r["regrade"]
        independent = ""
        if old["overall"] == "FAIL" and old["failure_class"] != "residency_gate_failed":
            independent = ": ".join(part for part in (old["failure_class"], old["reason"]) if part)
            independent = independent.replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {r['run_id']} | {r['profile']} | {old['residency']} | "
                     f"{new['residency']['status']} | {old['overall']} | {new['overall']} | "
                     f"{independent} |")
    if missing:
        lines += ["", "Missing source evidence:"] + [f"- {m}" for m in missing]
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
