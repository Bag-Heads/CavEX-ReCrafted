"""Paint the fixed Nether scene from a pack's own pixels; no engine at import time."""

import hashlib
import io
from functools import lru_cache
from pathlib import Path
import struct
import zlib

from PIL import Image


MAP_SHA256 = "fc66c89946b9ffe38a330793d8688670dddea556b3a92ae36a183b603a2fbc11"
SIZE = (1024, 576)
FILES = ("gui/nether-background.png", "gui/nether-background.md")


def map_path():
    root = Path(__file__).resolve().parent
    bundled = root / "resources/nether_scene_map.bin"
    return bundled if bundled.is_file() else root / "scene_data/nether_scene_map.bin"


@lru_cache(maxsize=1)
def _read_map():
    data = map_path().read_bytes()
    if hashlib.sha256(data).hexdigest() != MAP_SHA256:
        raise ValueError("Nether scene map checksum mismatch")
    raw = zlib.decompress(data)
    if raw[:8] != b"CWNSMAP1":
        raise ValueError("Unsupported Nether scene map")
    count, = struct.unpack_from("<I", raw, 8)
    end = 12 + count * 9
    palette = tuple(struct.iter_unpack("9B", raw[12:end]))
    base_end = end + SIZE[0] * SIZE[1] * 4
    base = struct.unpack_from("<" + str(SIZE[0] * SIZE[1]) + "I", raw, end)
    layers = []
    offset = base_end
    while offset < len(raw):
        count, = struct.unpack_from("<I", raw, offset)
        offset += 4
        layers.append(tuple(struct.iter_unpack("<II", raw[offset:offset + count * 8])))
        offset += count * 8
    if offset != len(raw):
        raise ValueError("Truncated Nether scene map")
    return palette, base, layers


def _atlas(data):
    with Image.open(io.BytesIO(data)) as image:
        if image.size != (256, 256):
            raise ValueError("Nether scene needs converted 256x256 atlases")
        return image.convert("RGBA")


def _prepare_fire(image):
    """Mirror flame_fx_anim_prepare_pack_atlas for the converted 256px sheet."""
    pixels = image.load()
    for col in (6, 13):
        for row in range(14):
            if not any(pixels[x, y][3] and pixels[x, y] not in
                       ((107, 63, 127, 255), (214, 127, 255, 255))
                       for y in range(row * 18 + 3, row * 18 + 19)
                       for x in range(col * 18 + 3, col * 18 + 19)):
                for dest in (110, 236):
                    image.paste(image.crop((dest - 54, 2, dest - 36, 254)), (dest, 2))
                return


def compose(terrain_data, anim_data):
    """Return PNG and provenance bytes before the caller publishes either file.

    Sparse underlying layers preserve mushroom cutouts and the engine's fire
    depth prepass. Geometry/shading come only from synthetic-atlas captures.
    The stored animation coordinates select exactly the engine's frame zero.
    """
    terrain, anim = _atlas(terrain_data), _atlas(anim_data)
    _prepare_fire(anim)
    sheets = (terrain.load(), anim.load())
    palette, base, layers = _read_map()
    colors, alphas = [], []
    for x, y, sheet, br, bg, bb, wr, wg, wb in palette:
        pixel = sheets[sheet][x, y] if sheet < 2 else (255, 255, 255, 255)
        alpha = pixel[3]
        if sheet == 0:
            alpha = 255 if alpha >= 16 else 0  # terrain alpha test, blending off
        colors.append(tuple(round(b + c * (w - b) / 255)
                            for c, b, w in zip(pixel, (br, bg, bb), (wr, wg, wb))))
        alphas.append(alpha)
    # Every terminal sample must be opaque: otherwise this atlas needs geometry
    # beyond the captured cutout layers. Fail the optional scene, never invent it.
    terminal = dict((i, index) for i, index in enumerate(base) if alphas[index] < 255)
    for layer in layers:
        for i, index in layer:
            if alphas[index] == 255:
                terminal.pop(i, None)
            else:
                terminal[i] = index
    if terminal:
        raise ValueError("Unsupported transparency in Nether scene terrain")
    pixels = [colors[index] for index in base]
    # Underlying samples first, then cover them with each pack's actual alpha.
    for layer in reversed(layers):
        for i, index in layer:
            alpha = alphas[index]
            if alpha == 255:
                pixels[i] = colors[index]
            elif alpha:
                pixels[i] = tuple(round((a * alpha + b * (255 - alpha)) / 255)
                                  for a, b in zip(colors[index], pixels[i]))
    for i, index in enumerate(base):
        alpha = alphas[index]
        if alpha == 255:
            pixels[i] = colors[index]
        elif alpha:
            pixels[i] = tuple(round((a * alpha + b * (255 - alpha)) / 255)
                              for a, b in zip(colors[index], pixels[i]))
    image = Image.new("RGB", SIZE)
    image.putdata(pixels)
    output = io.BytesIO()
    image.save(output, "PNG")
    png = output.getvalue()
    note = ("# Nether loading scene\n\n"
            "Composed by the CavEX Asset Importer from this pack's terrain.png and "
            "anim.png (frame zero), using CaveWii-generated geometry/shading data. "
            "Seed 42; eye (-59.5,36.62,100.5); yaw 90; polar pitch 78. "
            "No engine runs during import or portal loading. Texture rights remain "
            "with their owners. This is a fixed loading illustration, not a live-world "
            "parity capture.\n\n"
            "Map SHA256: " + MAP_SHA256 + "\n\n"
            "terrain.png SHA256: " + hashlib.sha256(terrain_data).hexdigest() + "\n\n"
            "anim.png SHA256: " + hashlib.sha256(anim_data).hexdigest() + "\n\n"
            "Scene SHA256: " + hashlib.sha256(png).hexdigest() + "\n")
    return png, note.encode("utf-8")
