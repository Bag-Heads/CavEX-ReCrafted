import argparse
import json
import os
from pathlib import Path

from pyfatfs.PyFat import PyFat


TARGET_FILES = (
    "trace.log",
    "benchmark-runtime.csv",
    "bench.txt",
)


def read_entry_bytes(fs, entry):
    size = entry.get_size()
    if size <= 0:
        return b""

    data = bytearray()
    for cluster in fs.get_cluster_chain(entry.get_cluster()):
        data.extend(fs.read_cluster_contents(cluster))

    return bytes(data[:size])


def open_entry(root, parts):
    current = root
    for part in parts:
        current = current.get_entry(part)
    return current


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--app-name", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    extracted = []
    with PyFat.open_fs(args.image) as fs:
        base_parts = ["APPS", args.app_name, "SAVES"]
        try:
            base = open_entry(fs.root_dir, base_parts)
        except Exception:
            print(json.dumps({"extracted": extracted}))
            return

        for name in TARGET_FILES:
            try:
                entry = base.get_entry(name)
            except Exception:
                continue

            out_path = output_dir / name
            out_path.write_bytes(read_entry_bytes(fs, entry))
            extracted.append(
                {
                    "name": name,
                    "path": str(out_path),
                    "size": out_path.stat().st_size,
                }
            )

    print(json.dumps({"extracted": extracted}))


if __name__ == "__main__":
    main()
