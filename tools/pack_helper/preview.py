"""Artwork-only previews from already validated, prepared standard sheets."""
from pathlib import Path
from PIL import Image, ImageDraw

# Beta's standard atlas coordinates, not inferred from filenames or modern packs.
EXAMPLES = (("Stone", "terrain.png", 1, 0),
            ("Wood", "terrain.png", 4, 0),
            ("Leaves", "terrain.png", 4, 3),
            ("Glass", "terrain.png", 1, 3))


def preview_sheet(prepared):
    samples = []
    for label, filename, col, row in EXAMPLES:
        path = Path(prepared) / filename
        if path.is_file():
            with Image.open(path) as sheet:
                samples.append((label, sheet.convert("RGBA").crop(
                    (col * 16, row * 16, col * 16 + 16, row * 16 + 16))))
    path = Path(prepared) / "gui/items.png"
    if path.is_file():
        with Image.open(path) as image:
            sheet = image.convert("RGBA")
            # Show a real nonempty item from this pack, rather than a fallback.
            for row in range(16):
                for col in range(16):
                    tile = sheet.crop((col * 16, row * 16, col * 16 + 16, row * 16 + 16))
                    if tile.getchannel("A").getbbox():
                        samples.append(("Item", tile))
                        break
                else:
                    continue
                break
    canvas = Image.new("RGB", (max(1, len(samples)) * 100, 215), "#ecebe5")
    draw = ImageDraw.Draw(canvas)
    for index, (label, tile) in enumerate(samples):
        x = index * 100
        draw.text((x + 8, 5), label, fill="#222222")
        enlarged = tile.resize((80, 80), Image.Resampling.NEAREST)
        for y, background in ((26, "#f8f7f2"), (121, "#242925")):
            draw.rectangle((x, y, x + 99, y + 89), fill=background)
            canvas.paste(enlarged, (x + 10, y + 5), enlarged)
    return canvas
