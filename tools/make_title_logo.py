#!/usr/bin/env python3
"""Build the approved title asset from the original M1 art without resampling.

Maintainer 2026-10-10: the RE-CRAFTED EDITION slab is the thinner "S3" cut
(2 px top plane, 4 px underside instead of 6/18) moved up to a 2 px gap under
CAVEX. It is made by copying whole source rows of the slab (no new pixels);
the lettering rides on its face rows unchanged. The CAVEX rows are untouched.

Run with python -I. Pillow is required. --source permits a relocated copy of
the source, but its bytes must still match the approved SHA256.
"""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tmp/jobs/menu-art-1006/team-sol-tile3/M1-art.png"
SOURCE_SHA256 = "4d79f81dad50797533290bc98c7d791c0093d332fccfe78e1cbe21d222b084d9"
CROP = (160, 20, 480, 168)

# Source slab rows (M1, y 116-165): outline 116-117, top plane 118-123, face
# 124-145 (lettering 124-142), underside 146-163, outline 164-165.
SLAB_ROWS = ([116, 117, 118, 119] + list(range(124, 144))
             + [146, 147, 162, 163, 164, 165])
SLAB_TOP = 114   # CAVEX ends at y=111: a 2 px gap
SLAB_FIRST, SLAB_END = 112, 168  # rows owned by the slab in the source
ALPHA_BOTTOM = SLAB_TOP + len(SLAB_ROWS) - CROP[1]  # 124: last opaque row + 1


def slim(source):
    """The approved S3 slab, 2 px under CAVEX, as whole copied source rows."""
    out = source.copy()
    out.paste(Image.new("RGBA", (source.width, SLAB_END - SLAB_FIRST)), (0, SLAB_FIRST))
    for i, row in enumerate(SLAB_ROWS):
        out.paste(source.crop((0, row, source.width, row + 1)), (0, SLAB_TOP + i))
    return out


def source_image(path):
    if hashlib.sha256(path.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("source SHA256 differs from the approved M1 art")
    with Image.open(path) as image:
        if image.mode != "RGBA" or image.size != (640, 480):
            raise ValueError("source must be 640x480 RGBA")
        return image.copy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "assets/gui/title-logo.png")
    args = parser.parse_args()
    source = source_image(args.source)
    logo = slim(source).crop(CROP)
    # Rows below the slab are transparent black, padding to the GX 4x4 height.
    if logo.crop((0, ALPHA_BOTTOM, 320, 148)).tobytes() != bytes(320 * (148 - ALPHA_BOTTOM) * 4):
        raise ValueError("approved bottom padding is not transparent black")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    logo.save(args.out, compress_level=9)
    provenance = {
        "schema_version": 1,
        "path": "gui/title-logo.png",
        "source": "tmp/jobs/menu-art-1006/team-sol-tile3/M1-art.png",
        "source_sha256": SOURCE_SHA256,
        "sha256": hashlib.sha256(args.out.read_bytes()).hexdigest(),
        "source_dimensions": [640, 480],
        "dimensions": [320, 148],
        "crop": list(CROP),
        "alpha_bbox": [0, 0, 320, ALPHA_BOTTOM],
        "transparent_bottom_rows": 148 - ALPHA_BOTTOM,
        "face_centre_x": 160,
        "original_art": True,
        "mojang_logo_pixels": False,
        "creeper_face_choice": {
            "by": "maintainer", "date": "2026-10-06",
            "description": "The creeper face in the A was an explicit choice."
        },
        "approval_date": "2026-10-07",
        "slab_revision": {"name": "S3, 2 px gap", "date": "2026-10-10",
            "by": "maintainer", "top_plane_px": 2, "underside_px": 4,
            "gap_px": 2, "slab_rows": SLAB_ROWS, "slab_top": SLAB_TOP},
        "authors": ["Opus B2", "Sol M1", "Opus R2"],
        "method": "Whole-row slab cut of the approved 2x art, then crop; no resampling.",
        "palette_note": "Source has 9 RGBA colours total: 8 opaque plus transparent."
    }
    args.out.with_suffix(".provenance.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.out}: 320x148 RGBA; source SHA256 verified")


if __name__ == "__main__":
    main()
