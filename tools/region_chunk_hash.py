#!/usr/bin/env python3
"""hw1029b/G1 drop-in leg: per-chunk hashes of a Beta 1.7.3 world's region files.

  snapshot <world_dir> <out.json>   hash every chunk (decompressed payload) of
                                    <world_dir>/region/*.mcr (+ DIM-1/region)
  diff <before.json> <after.json>   list chunks identical / changed / added /
                                    removed; exit 1 if any BEFORE chunk is not
                                    byte-identical AFTER unless it is named in
                                    --allow (';'-separated region/r.X.Z.mcr:x,z keys)

The runner takes a snapshot of the copy it stages onto the card, runs the
profile, copies the world back off the card and takes a second snapshot. An
untouched chunk must come back byte-identical (decompressed payload; the
compressed bytes may legitimately differ only if the chunk was re-saved, which
for an untouched chunk is itself a finding). level.dat is reported separately
(its version stamp is expected to change; level.dat_old must equal the
pre-run level.dat, see tests/test_dropin_beta_import.c D2).
Read-only on the world; stdlib only.
"""
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path


def chunk_hashes(mcr: Path) -> dict:
    data = mcr.read_bytes()
    out = {}
    if len(data) < 8192:
        return out
    for i in range(1024):
        off = int.from_bytes(data[i * 4:i * 4 + 3], "big")
        count = data[i * 4 + 3]
        if off == 0 or count == 0:
            continue
        base = off * 4096
        if base + 5 > len(data):
            out[f"{i % 32},{i // 32}"] = "TRUNCATED"
            continue
        length = struct.unpack_from(">i", data, base)[0]
        kind = data[base + 4]
        raw = data[base + 5:base + 4 + length]
        try:
            payload = zlib.decompress(raw) if kind == 2 else None
        except zlib.error:
            payload = None
        out[f"{i % 32},{i // 32}"] = (hashlib.sha256(payload).hexdigest()
                                      if payload is not None else "UNDECODABLE")
    return out


def snapshot(world: Path) -> dict:
    snap = {"chunks": {}, "files": {}}
    for sub in ("region", "DIM-1/region"):
        d = world / sub
        if not d.is_dir():
            continue
        for mcr in sorted(d.glob("*.mcr")):
            rel = f"{sub}/{mcr.name}"
            for key, h in chunk_hashes(mcr).items():
                snap["chunks"][f"{rel}:{key}"] = h
    for name in ("level.dat", "level.dat_old"):
        f = world / name
        if f.is_file():
            snap["files"][name] = hashlib.sha256(f.read_bytes()).hexdigest()
    return snap


def diff(before: dict, after: dict, allow: set) -> int:
    b, a = before["chunks"], after["chunks"]
    same = [k for k in b if a.get(k) == b[k]]
    changed = [k for k in b if k in a and a[k] != b[k]]
    removed = [k for k in b if k not in a]
    added = [k for k in a if k not in b]
    bad = [k for k in changed + removed if k not in allow]
    print(json.dumps({
        "identical": len(same), "changed": len(changed), "removed": len(removed),
        "added": len(added), "not_allowed": bad[:50],
        "level_dat_old_is_pre_run_level_dat":
            after["files"].get("level.dat_old") == before["files"].get("level.dat"),
    }, indent=1))
    return 1 if bad else 0


def main(argv) -> int:
    if len(argv) >= 3 and argv[0] == "snapshot":
        Path(argv[2]).write_text(json.dumps(snapshot(Path(argv[1])), indent=0))
        return 0
    if len(argv) >= 3 and argv[0] == "diff":
        allow = set()
        if len(argv) >= 5 and argv[3] == "--allow":
            allow = {s for s in argv[4].split(";") if s}
        return diff(json.loads(Path(argv[1]).read_text()),
                    json.loads(Path(argv[2]).read_text()), allow)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
