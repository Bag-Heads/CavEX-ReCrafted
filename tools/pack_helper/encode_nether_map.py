"""Offline only: compress synthetic PC scene captures into the importer's map.

Usage: python tools/pack_helper/encode_nether_map.py CAPTURES OUTPUT_DIRECTORY
CAPTURES contains layers.json and each layer's ids/black/white PNG captures.
No pack artwork is read. See scene_data/README.md for capture provenance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

from PIL import Image


def encode(captures, output):
    palette, lookup, layers = [], {}, []
    descriptions = json.loads((captures / "layers.json").read_text())
    for number, description in enumerate(descriptions):
        images = []
        for name in ("ids", "black", "white"):
            with Image.open(captures / (description["prefix"] + "-" + name + ".png")) as im:
                if im.size != (1024, 576):
                    raise ValueError("Capture dimensions must be 1024x576")
                images.append(list(im.convert("RGB").getdata()))
        active = description["active"] if number else range(1024 * 576)
        records = []
        for pixel in active:
            record = tuple(value for image in images for value in image[pixel])
            if record not in lookup:
                lookup[record] = len(palette)
                palette.append(record)
            records.append((pixel, lookup[record]))
        layers.append(records)
    raw = bytearray(b"CWNSMAP1" + struct.pack("<I", len(palette)))
    for record in palette:
        raw.extend(bytes(record))
    for _, index in layers[0]:
        raw.extend(struct.pack("<I", index))
    for layer in layers[1:]:
        raw.extend(struct.pack("<I", len(layer)))
        for pixel, index in layer:
            raw.extend(struct.pack("<II", pixel, index))
    data = zlib.compress(raw, 9)
    output.mkdir(parents=True, exist_ok=True)
    (output / "nether_scene_map.bin").write_bytes(data)
    metadata = {
        "format": "CWNSMAP1 (zlib)", "size": [1024, 576],
        "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
        "uncompressed_bytes": len(raw), "palette_entries": len(palette),
        "layer_samples": [len(layer) for layer in layers],
        "recipe": {"seed": 42, "eye": [-59.5, 36.62, 100.5], "yaw": 90,
                   "polar_pitch": 78, "animation_frame": 0},
        "capture_inputs": json.loads((captures / "capture-inputs.json").read_text()),
        "capture_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                           for d in descriptions for name in ("ids", "black", "white", "depth")
                           for p in [captures / (d["prefix"] + "-" + name + ".png")]},
        "provenance": "CaveWii-generated geometry/shading; synthetic atlases only; no Mojang pixels",
    }
    # Working-directory paths are not part of the recipe or distributable data.
    metadata["capture_inputs"]["config"].pop("paths", None)
    (output / "nether_scene_map.json").write_text(json.dumps(metadata, indent=2) + "\n",
                                                encoding="utf-8", newline="\n")
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(encode(args.captures, args.output), indent=2))
