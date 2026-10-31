"""Bounded, layout-aware PNG conversion for CavEX texture packs."""

from __future__ import annotations

import io
import json
import struct
from pathlib import Path
from typing import Any

from PIL import Image, UnidentifiedImageError


_MAP_PATH = Path(__file__).with_name("conversion_map.json")
_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_WII_VISIBLE_ALPHA = 19


class ConversionError(ValueError):
    """A pack image cannot be converted under the declared layout contract."""

    def __init__(self, code: str, asset_path: str, message: str):
        super().__init__(message)
        self.code = code
        self.asset_path = asset_path


def load_map(map_path: str | Path | None = None) -> dict[str, Any]:
    """Load and minimally validate a version-1 conversion map."""
    path = Path(map_path) if map_path is not None else _MAP_PATH
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConversionError("invalid_map", str(path), str(exc)) from exc
    if (result.get("schema_version") != 1
            or not isinstance(result.get("release_version"), str)
            or not isinstance(result.get("map_revision"), str)
            or not isinstance(result.get("assets"), dict)):
        raise ConversionError("invalid_map", str(path),
                              "expected schema_version 1 and release/map versions")
    return result


def _normal_path(asset_path: str) -> str:
    path = asset_path.replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    if (not path or path.startswith("/") or ":" in path
            or any(part in ("", ".", "..") for part in path.split("/"))):
        raise ConversionError("invalid_path", asset_path, "asset path must be relative")
    return path


def _entry_for(asset_path: str, conversion_map: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    path = _normal_path(asset_path)
    for canonical, entry in conversion_map["assets"].items():
        if path == canonical or path in entry.get("source_names", ()):
            return canonical, entry
    raise ConversionError("unsupported_asset", path, f"unsupported asset: {path}")


def _png_dimensions(data: bytes, asset_path: str, limits: dict[str, int]) -> tuple[int, int]:
    if len(data) > limits["max_encoded_bytes"]:
        raise ConversionError("encoded_too_large", asset_path, "PNG exceeds encoded-size limit")
    if (len(data) < 24 or data[:8] != _PNG_SIGNATURE
            or data[8:12] != struct.pack(">I", 13) or data[12:16] != b"IHDR"):
        raise ConversionError("invalid_png", asset_path, "missing PNG IHDR")
    width, height = struct.unpack(">II", data[16:24])
    if (not width or not height or width > limits["max_dimension"]
            or height > limits["max_dimension"]
            or width * height > limits["max_pixels"]):
        raise ConversionError("decoded_too_large", asset_path, "PNG dimensions exceed decode limits")
    return width, height


def _decode(data: bytes, asset_path: str, limits: dict[str, int]) -> Image.Image:
    expected = _png_dimensions(data, asset_path, limits)
    try:
        with Image.open(io.BytesIO(data)) as source:
            if source.format != "PNG" or source.size != expected:
                raise ConversionError("invalid_png", asset_path, "invalid PNG dimensions")
            if getattr(source, "n_frames", 1) != 1:
                raise ConversionError("unsupported_animation", asset_path,
                                      "Animated PNG is unsupported; use CavEX's animation sheet layout")
            source.load()
            return source.convert("RGBA")
    except (OSError, UnidentifiedImageError) as exc:
        raise ConversionError("invalid_png", asset_path, str(exc)) from exc


def _encode(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(output, format="PNG", optimize=False, compress_level=9)
    return output.getvalue()


def _resize_alpha_aware(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    """BOX-filter once in Pillow's premultiplied-alpha RGBa mode."""
    return image.convert("RGBa").resize(size, Image.Resampling.BOX).convert("RGBA")


def _resize(image: Image.Image, size: tuple[int, int], look: str) -> Image.Image:
    if look == "crisp":
        return image.resize(size, Image.Resampling.NEAREST)
    if look == "smooth":
        return _resize_alpha_aware(image, size)
    raise ConversionError("unsupported_look", "", "look must be 'crisp' or 'smooth'")


def flattened_pixels(image: Image.Image) -> tuple:
    """Every pixel in row-major order, one tuple per pixel.

    Pillow 12 deprecates Image.getdata() in favour of get_flattened_data()
    (its DeprecationWarning: getdata "will be removed in Pillow 14
    (2027-10-15)"); older Pillow -- e.g. Ubuntu 24.04's python3-pil 10.2 -- has
    only getdata(). Both yield the same per-pixel tuples, so feature-detect the
    new accessor and fall back to the old one, never touching the deprecated
    call on a Pillow that has the replacement."""
    getter = getattr(image, "get_flattened_data", None)
    if getter is not None:
        return tuple(getter())
    return tuple(image.getdata())


def _preserve_cutout_coverage(source: Image.Image, reduced: Image.Image,
                              scale: int) -> Image.Image:
    """Keep the source's Wii-visible sample count after integer reduction."""
    visible = sum(alpha >= _WII_VISIBLE_ALPHA
                  for _r, _g, _b, alpha in flattened_pixels(source))
    target_visible = (visible + scale * scale // 2) // (scale * scale)
    pixels = list(flattened_pixels(reduced))
    order = sorted(range(len(pixels)), key=lambda index: (pixels[index][3], -index),
                   reverse=True)
    keep = set(order[:target_visible])
    reduced.putdata([(r, g, b, 255 if index in keep else 0)
                     for index, (r, g, b, _alpha) in enumerate(pixels)])
    return reduced


def _cell_is_continuous(entry: dict[str, Any], column: int, row: int) -> bool:
    rules = entry.get("alpha_rules", {})
    return [column, row] in rules.get("continuous_cells", [])


def _convert_grid(image: Image.Image, target: tuple[int, int], entry: dict[str, Any],
                  look: str) -> Image.Image:
    grid = tuple(entry["grid"])
    src_cell = (image.width // grid[0], image.height // grid[1])
    dst_cell = (target[0] // grid[0], target[1] // grid[1])
    scale = src_cell[0] // dst_cell[0]
    result = Image.new("RGBA", target)
    for row in range(grid[1]):
        for column in range(grid[0]):
            left, top = column * src_cell[0], row * src_cell[1]
            cell = image.crop((left, top, left + src_cell[0], top + src_cell[1]))
            reduced = _resize(cell, dst_cell, look)
            rules = entry.get("alpha_rules", {})
            if (look == "smooth" and rules.get("default") == "cutout_coverage"
                    and not _cell_is_continuous(entry, column, row)):
                reduced = _preserve_cutout_coverage(cell, reduced, scale)
            result.paste(reduced, (column * dst_cell[0], row * dst_cell[1]))
    return result


def _convert_animation(image: Image.Image, target: tuple[int, int], entry: dict[str, Any],
                       look: str) -> Image.Image:
    layout = entry["animation_layout"]
    scale = image.width // target[0]
    result = image.resize(target, Image.Resampling.NEAREST)
    pixels = result.load()
    pitch = layout["cell_pitch"]
    content = layout["content_size"]
    source_origin = layout["content_origin"] * scale
    for row in range(layout["rows"]):
        for column in range(layout["columns"]):
            left = column * pitch * scale + source_origin
            top = row * pitch * scale + source_origin
            frame = image.crop((left, top, left + content * scale,
                                top + content * scale))
            frame = _resize(frame, (content, content), look)
            frame_pixels = frame.load()
            output_x = column * pitch + layout["content_origin"] - 1
            output_y = row * pitch + layout["content_origin"] - 1
            for y in range(-1, content + 1):
                for x in range(-1, content + 1):
                    pixels[output_x + x + 1, output_y + y + 1] = frame_pixels[
                        min(content - 1, max(0, x)),
                        min(content - 1, max(0, y))]
    return result


def convert_image(data: bytes, asset_path: str, look: str = "crisp", *,
                  conversion_map: dict[str, Any] | None = None) -> bytes:
    """Convert one declared pack PNG to CavEX's canonical pixel layout.

    *conversion_map* is the already-loaded map to convert against. It exists so
    a caller that loaded a NON-default map (core.prepare's `mapping_path`)
    converts against that same map: this function used to re-read the packaged
    default unconditionally, so a custom map changed which assets were selected
    but not the canonical sizes they were converted to -- two different
    contracts inside one prepare. Omit it for the packaged default.
    """
    if look not in ("crisp", "smooth"):
        raise ConversionError("unsupported_look", asset_path,
                              "look must be 'crisp' or 'smooth'")
    if conversion_map is None:
        conversion_map = load_map()
    canonical, entry = _entry_for(asset_path, conversion_map)
    source_size = _png_dimensions(data, canonical, conversion_map["limits"])
    target = tuple(entry["canonical_size"])
    strategy = entry["strategy"]
    if strategy in ("composite_only", "bundled_only"):
        code = "composition_required" if strategy == "composite_only" else "bundled_only"
        raise ConversionError(code, canonical,
                              f"{canonical} must use the bundled CavEX asset")
    if source_size == target:
        _decode(data, canonical, conversion_map["limits"])
        return data

    if strategy == "lookup_exact" and entry["supported_scales"] == [1]:
        raise ConversionError("unsupported_dimensions", canonical,
                              f"{canonical} only supports {target[0]}x{target[1]}")
    # Scale rule, stated once for square AND non-square canonicals (Beta ships
    # 64x256 rain/snow and 64x32 mob/armor sheets): the source must be the
    # canonical size times ONE integer factor on BOTH axes -- w/cw == h/ch --
    # and that factor must be declared in supported_scales. Messages name both
    # sizes: "unsupported scale 2x8" alone told a player nothing about which
    # dimensions the helper actually wanted (the real-jar rain.png abort).
    shape = (f"{source_size[0]}x{source_size[1]} against the "
             f"{target[0]}x{target[1]} layout")
    if source_size[0] % target[0] or source_size[1] % target[1]:
        raise ConversionError("unsupported_dimensions", canonical,
                              f"{canonical} is {shape}, which is not a whole multiple of it")
    scale_x, scale_y = source_size[0] // target[0], source_size[1] // target[1]
    if scale_x != scale_y:
        raise ConversionError("unsupported_dimensions", canonical,
                              f"{canonical} is {shape}, which stretches it "
                              f"{scale_x}x across and {scale_y}x down")
    if scale_x not in entry["supported_scales"]:
        raise ConversionError("unsupported_dimensions", canonical,
                              f"{canonical} is {shape}, a {scale_x}x size this layout does not support")

    image = _decode(data, canonical, conversion_map["limits"])
    effective_look = "crisp" if entry.get("look") == "crisp" else look
    if strategy == "cell_grid":
        converted = _convert_grid(image, target, entry, effective_look)
    elif strategy == "region_grid":
        converted = _convert_grid(image, target, entry, effective_look)
    elif strategy == "lookup_exact":
        # A colour map is a lookup table, never filtered: an HD pack's
        # integer-scaled grass/foliage map is sampled back with NEAREST so a
        # uniformly upscaled map returns the exact 256x256 original.
        converted = image.resize(target, Image.Resampling.NEAREST)
    elif strategy == "sheet":
        converted = _resize(image, target, effective_look)
    elif strategy == "animation_sheet":
        converted = _convert_animation(image, target, entry, effective_look)
    else:
        raise ConversionError("invalid_map", canonical, f"unknown strategy: {strategy}")
    return _encode(converted)


def compose_gui2(base_data: bytes, gui_data: bytes, icons_data: bytes) -> bytes:
    """Overlay Beta HUD regions while preserving every fork/controller base pixel."""
    limits = load_map()["limits"]
    base = _decode(base_data, "gui_2.png", limits)
    gui = _decode(gui_data, "gui/gui.png", limits)
    icons = _decode(icons_data, "gui/icons.png", limits)
    if base.size != (256, 256) or gui.size != (256, 256) or icons.size != (256, 256):
        raise ConversionError("unsupported_dimensions", "gui_2.png",
                              "HUD composition requires canonical 256x256 sheets")
    for source, box, dest in (
        (gui, (0, 0, 182, 22), (0, 0)),
        (gui, (0, 22, 24, 46), (208, 0)),
        (icons, (0, 0, 16, 16), (0, 229)),
        (icons, (16, 0, 25, 9), (16, 229)),
        (icons, (52, 0, 61, 9), (52, 229)),
    ):
        base.paste(source.crop(box), dest)
    return _encode(base)


def compose_anim(base_data: bytes, terrain_data: bytes) -> bytes:
    """Install canonical ice with the renderer's 18px cells and clamped border."""
    limits = load_map()["limits"]
    base = _decode(base_data, "anim.png", limits)
    terrain = _decode(terrain_data, "terrain.png", limits)
    if base.size != (256, 256) or terrain.size != (256, 256):
        raise ConversionError("unsupported_dimensions", "anim.png",
                              "animation composition requires canonical 256x256 sheets")
    ice = terrain.crop((3 * 16, 4 * 16, 4 * 16, 5 * 16))
    ice_pixels = ice.load()
    output = base.load()
    for column in (4, 11):
        for row in range(14):
            origin_x, origin_y = column * 18 + 2, row * 18 + 2
            for y in range(-1, 17):
                for x in range(-1, 17):
                    output[origin_x + x + 1, origin_y + y + 1] = ice_pixels[
                        min(15, max(0, x)), min(15, max(0, y))]
    return _encode(base)
