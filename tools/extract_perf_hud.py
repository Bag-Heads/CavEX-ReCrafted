import json
import re
import sys
from pathlib import Path

from PIL import Image


LINE_HEIGHT = 17
TEXT_SCALE = 16
TEXT_HEIGHT = 16
TEXT_X = 4
BRIGHT_THRESHOLD = 200
ALLOWED_CHARS = " 0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ./:-"


def build_font(font_path: Path):
    img = Image.open(font_path).convert("RGBA")
    width, height = img.size
    char_width = width // 16
    char_height = height // 16
    widths = {}
    glyphs = {}

    for code in range(256):
        cell_x = (code % 16) * char_width
        cell_y = (code // 16) * char_height
        cell = img.crop((cell_x, cell_y, cell_x + char_width, cell_y + char_height))

        glyph_width = 0
        for x in range(char_width):
            has_pixel = False
            for y in range(char_height):
                r, g, b, a = cell.getpixel((x, y))
                if a and (r or g or b):
                    has_pixel = True
                    break
            if not has_pixel:
                break
            glyph_width += 1

        if code == ord(" "):
            glyph_width = 4

        advance = max(1, (glyph_width + 1) * TEXT_SCALE // 8)
        widths[code] = advance

        resized = cell.resize((TEXT_SCALE, TEXT_SCALE), Image.Resampling.NEAREST)
        mask = []
        for y in range(TEXT_HEIGHT):
            row = []
            for x in range(advance):
                r, g, b, a = resized.getpixel((x, y))
                row.append(1 if a and (r or g or b) else 0)
            mask.append(row)
        glyphs[chr(code)] = mask

    return widths, glyphs


def first_active_row(image: Image.Image):
    sample_x = image.width // 2
    for y in range(image.height):
        r, g, b = image.getpixel((sample_x, y))
        if r or g or b:
            return y
    return 0


def image_mask(image: Image.Image):
    mask = []
    for y in range(image.height):
        row = []
        for x in range(image.width):
            r, g, b = image.getpixel((x, y))
            row.append(1 if max(r, g, b) >= BRIGHT_THRESHOLD else 0)
        mask.append(row)
    return mask


def glyph_score(mask, glyph, x0, y0):
    height = len(glyph)
    width = len(glyph[0]) if glyph else 0
    score = 0

    for y in range(height):
        yy = y0 + y
        if yy >= len(mask):
            score += width
            continue
        row = mask[yy]
        for x in range(width):
            xx = x0 + x
            sample = row[xx] if xx < len(row) else 0
            if sample != glyph[y][x]:
                score += 1

    return score


def parse_line(mask, glyphs, widths, y0, max_width):
    text = []
    x = TEXT_X
    consecutive_spaces = 0

    while x < max_width:
        best_char = None
        best_score = None

        for ch in ALLOWED_CHARS:
            glyph = glyphs[ch]
            score = glyph_score(mask, glyph, x, y0)
            if best_score is None or score < best_score:
                best_score = score
                best_char = ch

        if best_char is None:
            break

        text.append(best_char)
        x += widths[ord(best_char)]

        if best_char == " ":
            consecutive_spaces += 1
            if consecutive_spaces >= 8:
                break
        else:
            consecutive_spaces = 0

    return "".join(text).rstrip()


def parse_metrics(lines):
    result = {"lines": lines}

    patterns = {
        "fps": re.compile(r"(?P<fps>\d+\.\d)\s+fps"),
        "loaded": re.compile(
            r"(?P<loaded>\d+)\s+loaded\s+(?P<draw>\d+)\s+draw\s+(?P<ents>\d+)\s+ents\s+(?P<mesh>\d+)\s+mesh\s+(?P<fluid>\d+)\s+fluid"
        ),
        "benchmark": re.compile(
            r"bench\s+(?P<samples>\d+)\s+min\s+(?P<min>\d+\.\d)\s+avg\s+(?P<avg>\d+\.\d)\s+max\s+(?P<max>\d+\.\d)"
        ),
        "benchmark_draw": re.compile(
            r"bdraw\s+(?P<draw>\d+)\s+bload\s+(?P<load>\d+)\s+bmesh\s+(?P<mesh>\d+)\s+bfluid\s+(?P<fluid>\d+)"
        ),
    }

    for line in lines:
        if "fps" not in result:
            match = patterns["fps"].search(line)
            if match:
                result["fps"] = float(match.group("fps"))

        if "loaded" not in result:
            match = patterns["loaded"].search(line)
            if match:
                result["loaded"] = {
                    "loaded": int(match.group("loaded")),
                    "draw": int(match.group("draw")),
                    "ents": int(match.group("ents")),
                    "mesh": int(match.group("mesh")),
                    "fluid": int(match.group("fluid")),
                }

        if "benchmark" not in result:
            match = patterns["benchmark"].search(line)
            if match:
                result["benchmark"] = {
                    "samples": int(match.group("samples")),
                    "min": float(match.group("min")),
                    "avg": float(match.group("avg")),
                    "max": float(match.group("max")),
                }

        if "benchmark_draw" not in result:
            match = patterns["benchmark_draw"].search(line)
            if match:
                result["benchmark_draw"] = {
                    "draw": int(match.group("draw")),
                    "load": int(match.group("load")),
                    "mesh": int(match.group("mesh")),
                    "fluid": int(match.group("fluid")),
                }

    return result


def main():
    if len(sys.argv) != 3:
        print("usage: extract_perf_hud.py <screenshot> <font_png>", file=sys.stderr)
        return 1

    shot_path = Path(sys.argv[1])
    font_path = Path(sys.argv[2])

    screenshot = Image.open(shot_path).convert("RGB")
    widths, glyphs = build_font(font_path)
    mask = image_mask(screenshot)
    top = first_active_row(screenshot)

    lines = []
    for index in (0, 1, 5, 6):
        y = top + 4 + index * LINE_HEIGHT
        lines.append(parse_line(mask, glyphs, widths, y, min(screenshot.width, 420)))

    print(json.dumps(parse_metrics(lines), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
