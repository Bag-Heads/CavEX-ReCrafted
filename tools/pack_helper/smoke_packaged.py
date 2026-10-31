"""Run the frozen app from an empty directory without development-tool paths.

The report also carries a hash of the WHOLE dist tree. The app's own self-test
verifies its bundled `resources/`, which is a fraction of what is shipped --
the frozen interpreter, the runtime DLLs/dylibs and every hook-collected
package were entirely unhashed, so two "identical" bundles could differ
everywhere outside resources/ and still pass (2026-09-17 review, finding H2).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def tree_digest(bundle: Path) -> dict:
    """Every entry under `bundle`: file content hashes and symlink targets.

    Symlinks are recorded by TARGET, never followed: a macOS .app bundle is
    full of them, and following them would both double-count and hide a
    retargeted link.

    The POSIX executable bit is part of the entry, because a bundle whose
    launcher lost +x is broken in a way no content hash can see (2026-09-17
    DELTA review, LOW). Windows has no such bit, so it always records x0 --
    that is a real platform difference, not drift, and the digests of a
    Windows and a macOS bundle are not comparable to each other anyway.
    """
    entries = {}
    combined = hashlib.sha256()
    for path in sorted(bundle.rglob("*"), key=lambda p: p.as_posix()):
        relative = path.relative_to(bundle).as_posix()
        if path.is_symlink():
            entry = "link:" + os.readlink(path).replace("\\", "/")
        elif path.is_file():
            executable = 0 if os.name == "nt" else int(
                bool(path.stat().st_mode & 0o111))
            entry = "file:x{0}:{1}".format(
                executable, hashlib.sha256(path.read_bytes()).hexdigest())
        else:
            continue
        entries[relative] = entry
        combined.update(relative.encode("utf-8") + b"\0" + entry.encode("utf-8") + b"\n")
    return {"entries": len(entries), "sha256": combined.hexdigest(),
            "records_exec_bit": os.name != "nt", "tree": entries}


def smoke(bundle, report):
    bundle, report = Path(bundle).resolve(), Path(report).resolve()
    if sys.platform == "win32":
        executable = bundle / "CavEX Asset Importer/CavEX Asset Importer.exe"
    elif sys.platform == "darwin":
        executable = bundle / "CavEX Asset Importer.app/Contents/MacOS/CavEX Asset Importer"
    else:
        executable = bundle / "CavEX Asset Importer/CavEX Asset Importer"
    environment = dict(os.environ)
    for key in ("PYTHONPATH", "PYTHONHOME", "TCL_LIBRARY", "TK_LIBRARY"):
        environment.pop(key, None)
    environment["PATH"] = (str(Path(environment.get("SystemRoot", "C:/Windows")) / "System32")
                           if os.name == "nt" else "/usr/bin:/bin")
    report.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cw-frozen-check-") as working:
        subprocess.run([str(executable), "--self-test", str(report)], cwd=working,
                       env=environment, check=True, timeout=120)
    result = json.loads(report.read_text())
    if not result.get("ok"):
        raise RuntimeError(str(result))
    result["bundle"] = tree_digest(bundle)
    result["checks"] = list(result.get("checks", [])) + [
        "whole dist tree hashed ({0} entries)".format(result["bundle"]["entries"])]
    report.write_text(json.dumps(result, indent=2), encoding="utf-8", newline="\n")
    # The per-file table is the audit trail; keep the console summary readable.
    summary = dict(result, bundle=dict(result["bundle"], tree="<{0} entries in {1}>".format(
        result["bundle"]["entries"], report)))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    smoke(args.bundle, args.report)
