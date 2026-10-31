"""Pack original Wii and credited GameCube artwork for local UI proofs.

See artwork/controller-glyphs/README.md for provenance. Does not overwrite
the production atlas; the capture runner copies this into its scratch assets.
"""

from pathlib import Path

from PIL import Image


def main():
    root = Path(__file__).resolve().parent.parent
    sources = root / "artwork/controller-glyphs"
    atlas = Image.open(root / "assets/controls.png").convert("RGBA")
    original = Image.open(sources / "original-wii-atlas.png").convert("RGBA")
    assert atlas.size == original.size == (256, 256)
    atlas.paste(original.crop((0, 0, 256, 128)), (0, 0))
    names = ["A", "B", "X", "Y", "Right Bumper", "L Analog",
             "R Analog", "Start Pause", "Control Stick", "C Stick"]
    for cell, name in enumerate(names, start=32):
        icon = Image.open(sources / f"gc-{name}.png").convert("RGBA")
        assert max(icon.size) <= 32
        x, y = cell % 8 * 32, cell // 8 * 32
        atlas.paste((0, 0, 0, 0), (x, y, x + 32, y + 32))
        atlas.paste(icon, (x + (32 - icon.width) // 2,
                          y + (32 - icon.height) // 2))
    output = sources / "controls-proof.png"
    atlas.save(output)
    print(output)


if __name__ == "__main__":
    main()
