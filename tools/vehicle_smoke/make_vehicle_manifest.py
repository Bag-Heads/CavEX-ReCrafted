#!/usr/bin/env python3
"""O18: write the frozen -VehicleManifest JSON for host_vehicle_smoke_8m.

Fills the packet convention of docs/vehicle_smoke_profile.md ("Frozen manifest
and calibration") from a BUILT release DOL plus the in-tree collector, so the
runner never hand-types a hash:

  contract         CONTRACT-v1.3 (fixture/claim amendment of the v1.2 wire schema)
  build_id         the stamp embedded in the DOL ("CaveWii (impl. B1.7.3) <id>"),
                   or --build-id, which must then match the embedded one
  release_sha256   SHA256 of the DOL
  collector_sha256 SHA256 of tools/wii_telemetry_server.py (as it is NOW)
  source_sha256    SHA256 of the source freeze file (e.g. O18-freeze.sha256)
  kinds.{boat,pig,cart}
                   transfer_mode (default fixture_request: contact geometry is
                   unproven, CONTRACT-v1.3), geometry_proven=true, and a per-kind
                   copy of the effective config JSON with its SHA256

Calibration / transfer_ack_proof are NOT invented here: the first live capture
is calibration-only (CONTRACT certification gate); add them when they exist.

Usage:
  python tools/vehicle_smoke/make_vehicle_manifest.py --dol CaveWii_v2.dol \
      --config <effective config.json> --source-freeze <O18-freeze.sha256> \
      --out <dir>   [--build-id <stamp>] [--mode-boat contact] ...
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STAMP = re.compile(rb"CaveWii \(impl\. B1\.7\.3\) ([A-Za-z0-9_.+-]{1,63})")
KINDS = ("boat", "pig", "cart")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dol", required=True)
    ap.add_argument("--config", required=True, help="effective config JSON (overrides applied)")
    ap.add_argument("--source-freeze", required=True, help="the frozen source hash list")
    ap.add_argument("--out", required=True, help="output directory (manifest + config copies)")
    ap.add_argument("--build-id", default="")
    for k in KINDS:
        ap.add_argument("--mode-" + k, default="fixture_request", choices=("contact", "fixture_request"))
    a = ap.parse_args(argv)

    dol = Path(a.dol)
    stamps = sorted(set(m.group(1).decode() for m in STAMP.finditer(dol.read_bytes())))
    if len(stamps) != 1:
        sys.exit("expected exactly one embedded build stamp in %s, found %r" % (dol, stamps))
    build_id = stamps[0]
    if a.build_id and a.build_id != build_id:
        sys.exit("--build-id %s does not match the DOL's embedded %s" % (a.build_id, build_id))
    config = Path(a.config)
    json.loads(config.read_text(encoding="utf-8-sig"))  # must be valid JSON
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    kinds = {}
    for k in KINDS:
        copy = out / ("%s-config.json" % k)
        shutil.copyfile(config, copy)
        kinds[k] = {"transfer_mode": getattr(a, "mode_" + k), "geometry_proven": True,
                    "config_path": copy.name, "config_sha256": sha256(copy)}
    manifest = {
        "contract": "CONTRACT-v1.3",
        "build_id": build_id,
        "release_sha256": sha256(dol),
        "source_sha256": sha256(a.source_freeze),
        "collector_sha256": sha256(REPO / "tools" / "wii_telemetry_server.py"),
        "kinds": kinds,
    }
    path = out / "vehicle-manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("wrote %s (build %s, release %s)" % (path, build_id, manifest["release_sha256"][:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
