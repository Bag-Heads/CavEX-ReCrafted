"""Build the ignored Pack Helper resources required by source-checkout tests.

Only committed redistributable assets and procedural effects enter this tree;
no owned jar or imported pack is read. Use the release builder, so tests and
the packaged app consume the same mapping, defaults and native transcoder.
"""
import argparse
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "tools" / "pack_helper"
sys.path.insert(0, str(HELPER))
from build_resources import build  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    destination = HELPER / "resources"
    # Build to completion before replacing the previous generated resources.
    with tempfile.TemporaryDirectory(prefix=".test-resources-", dir=HELPER) as tmp:
        staged = Path(tmp) / "resources"
        build(ROOT, staged, args.cc)
        if destination.is_symlink():
            destination.unlink()
        elif destination.exists():
            shutil.rmtree(destination)
        staged.rename(destination)


if __name__ == "__main__":
    main()
