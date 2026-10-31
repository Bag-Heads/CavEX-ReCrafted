"""Convert the Wii's banded raw framebuffer dumps (gfx_write_framebuffer_raw:
"CWFB", u32 width, u32 height big-endian, then RGBA8 rows top-down) to PNG.
Usage: python tools/wii_rgba_to_png.py <file-or-dir> [more ...]
Each ss_NNNNNN.rgba becomes ss_NNNNNN.png beside it; truncated files (a run
that died mid-write) are padded with black and reported. Needs Pillow.
"""
import glob
import os
import struct
import sys

from PIL import Image


def convert(path):
    with open(path, "rb") as fp:
        data = fp.read()
    if len(data) < 12 or data[:4] != b"CWFB":
        print(f"{path}: not a CWFB dump")
        return False
    w, h = struct.unpack(">II", data[4:12])
    need = w * h * 4
    body = data[12:12 + need]
    short = need - len(body)
    if short > 0:
        body += b"\0" * short
    Image.frombytes("RGBA", (w, h), body).save(os.path.splitext(path)[0] + ".png")
    print(f"{path}: {w}x{h}" + (f" (truncated by {short} bytes, padded)" if short else ""))
    return True


def main(args):
    files = []
    for a in args:
        if os.path.isdir(a):
            files += sorted(glob.glob(os.path.join(a, "*.rgba")))
        else:
            files.append(a)
    ok = sum(convert(f) for f in files)
    print(f"converted {ok}/{len(files)}")


if __name__ == "__main__":
    main(sys.argv[1:])
