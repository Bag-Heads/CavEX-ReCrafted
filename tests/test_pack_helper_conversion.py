import io
from pathlib import Path
import struct
import unittest

from PIL import Image

from tools.pack_helper.conversion import (
    ConversionError,
    compose_anim,
    compose_gui2,
    convert_image,
    flattened_pixels,
    load_map,
)
from tools.import_owned_beta_assets import extract_texture_dest_paths


def png(image):
    output = io.BytesIO()
    image.save(output, "PNG")
    return output.getvalue()


def decoded(data):
    with Image.open(io.BytesIO(data)) as image:
        image.load()
        return image.convert("RGBA")


class PackHelperConversionTests(unittest.TestCase):
    def test_animated_png_is_reported_instead_of_silently_using_first_frame(self):
        output = io.BytesIO()
        first = Image.new("RGBA", (256, 256), "red")
        second = Image.new("RGBA", (256, 256), "blue")
        first.save(output, "PNG", save_all=True, append_images=[second], duration=100)
        with self.assertRaises(ConversionError) as caught:
            convert_image(output.getvalue(), "terrain.png")
        self.assertEqual(caught.exception.code, "unsupported_animation")

    def test_crisp_is_plain_nearest_without_cutout_rebalancing(self):
        source = Image.new("RGBA", (512, 512), (31, 77, 103, 80))
        result = decoded(convert_image(png(source), "terrain.png", "crisp"))
        self.assertEqual(result.getpixel((64, 48)), (31, 77, 103, 80))

    def test_map_is_versioned_and_explicit(self):
        conversion_map = load_map()
        self.assertEqual(conversion_map["schema_version"], 1)
        self.assertEqual(conversion_map["release_version"], "0.1.1")
        self.assertEqual(conversion_map["release_version"],
                         (Path(__file__).resolve().parents[1] / "VERSION")
                         .read_text(encoding="utf-8").strip())
        # 1.0.0 -> 1.1.0 (2026-09-17): three canonical sizes were corrected to
        # the dimensions the real Beta 1.7.3 jar ships -- see
        # test_canonical_dimensions_are_the_beta_native_ones below. Any further
        # change to what the map promises a pack author moves this again.
        # 1.1.0 -> 1.2.0 (2026-10-05, I06): misc/mapicons.png joins the map
        # (held-map player markers, K01): a Beta 4x4 grid of 8x8 cells, with a
        # bundled CC0 arrow sheet as its fallback.
        # 1.2.0 -> 1.3.0 (2026-10-11, EAD-1): item/arrows.png joins the map
        # (RenderArrow's flying-arrow sheet, Beta 32x32), with the bundled
        # Earth & Ash CC0 sheet as its fallback; before this no set had one.
        # 1.3.0 -> 1.4.0 (2026-10-30, jar-import audit): the grass/foliage
        # colour maps accept HD packs' integer-scaled copies (NEAREST sample
        # back to 256x256), because an unconvertible pack file now stops the
        # import instead of silently falling back.
        self.assertEqual(conversion_map["map_revision"], "1.4.0")
        for entry in conversion_map["assets"].values():
            for key in ("source_names", "required", "fallback", "strategy",
                        "canonical_size", "supported_scales"):
                self.assertIn(key, entry)

    def test_every_loader_texture_has_an_explicit_policy(self):
        repo = Path(__file__).resolve().parents[1]
        loader_paths = set(extract_texture_dest_paths(
            (repo / "source/platform/texture.c").read_text(encoding="utf-8")))
        loader_paths.update(("misc/grasscolor.png", "misc/foliagecolor.png"))
        conversion_map = load_map()
        self.assertEqual(set(conversion_map["assets"]), loader_paths)
        for path in ("controls.png", "pointer.png", "gui/approved-controls.png",
                     "gui/controller-pictures.png", "gui/iron_chest.png"):
            self.assertEqual(conversion_map["assets"][path]["strategy"],
                             "bundled_only")

    # The bundled CC0 artwork whose size deliberately differs from the Beta
    # 1.7.3 canonical, with the Beta dimension the map now declares. The
    # canonical is the layout a PACK must be reduced to, and Beta is the parity
    # reference for that; the bundled file is only the fallback, and the engine
    # samples all three with full 0..1 UVs (render_weather.c, render_item.c) so
    # it imposes no size of its own. Measured against the owned jar 2026-09-17
    # (jar-dimension-audit). Keeping the old assertion for these three is what
    # made the helper reject the real jar: 64x256 rain cannot be reduced to a
    # 32x32 canonical by one integer factor, so prepare aborted.
    CC0_DEFAULT_DIFFERS = {
        "environment/rain.png": [64, 256],
        "environment/snow.png": [64, 256],
        "misc/mapbg.png": [64, 64],
    }

    def test_canonical_dimensions_match_every_tracked_runtime_asset(self):
        assets = Path(__file__).resolve().parents[1] / "assets"
        for path, entry in load_map()["assets"].items():
            with Image.open(assets / path) as image:
                if path in self.CC0_DEFAULT_DIFFERS:
                    # Still a real assertion: the exception is pinned to the
                    # exact Beta size, so a drifting canonical fails here too.
                    self.assertEqual(entry["canonical_size"],
                                     self.CC0_DEFAULT_DIFFERS[path], path)
                    self.assertNotEqual(list(image.size), entry["canonical_size"], path)
                    continue
                self.assertEqual(list(image.size), entry["canonical_size"], path)

    def test_canonical_dimensions_are_the_beta_native_ones(self):
        """Beta 1.7.3 is not all-square, and the map must say so.

        Audited file-by-file against the owned b1.7.3 jar (2026-09-17): every
        mapped path's canonical is the jar's own PNG size. The non-square ones
        are the skin/armor sheets (64x32) and the weather strips (64x256); the
        map declared the weather strips 32x32 and misc/mapbg.png 128x128 (both
        the CaveWii CC0 default's size, not Beta's), which no integer factor
        can reconcile with the jar.
        """
        assets = load_map()["assets"]
        for path in ("environment/rain.png", "environment/snow.png"):
            self.assertEqual(assets[path]["canonical_size"], [64, 256], path)
        self.assertEqual(assets["misc/mapbg.png"]["canonical_size"], [64, 64])
        for path in ("mob/char.png", "armor/iron_1.png", "item/cart.png"):
            self.assertEqual(assets[path]["canonical_size"], [64, 32], path)

    def test_mapicons_is_a_beta_4x4_cell_grid_with_a_bundled_fallback(self):
        """misc/mapicons.png: the Beta 32x32 sheet of 4x4 marker cells (8x8 each).

        Native 32x32 passes through byte-identical; 2x/4x/8x packs reduce each
        cell on its own, so no neighbouring marker bleeds into the player arrow.
        """
        entry = load_map()["assets"]["misc/mapicons.png"]
        self.assertEqual(entry["source_names"], ["misc/mapicons.png"])
        self.assertFalse(entry["required"])
        self.assertEqual(entry["fallback"], "bundled")
        self.assertEqual(entry["strategy"], "cell_grid")
        self.assertEqual(entry["grid"], [4, 4])
        self.assertEqual(entry["canonical_size"], [32, 32])
        self.assertEqual(entry["supported_scales"], [1, 2, 4, 8])
        native = png(Image.new("RGBA", (32, 32), (9, 8, 7, 255)))
        self.assertIs(convert_image(native, "misc/mapicons.png"), native)
        for scale in (2, 4, 8):
            cell = 8 * scale
            image = Image.new("RGBA", (32 * scale, 32 * scale), (0, 0, 0, 0))
            image.paste((250, 240, 10, 255), (0, 0, cell, cell))
            image.paste((10, 20, 230, 255), (cell, 0, 2 * cell, cell))
            result = decoded(convert_image(png(image), "misc/mapicons.png", "smooth"))
            self.assertEqual(result.size, (32, 32))
            self.assertEqual(result.getpixel((7, 4)), (250, 240, 10, 255))
            self.assertEqual(result.getpixel((8, 4)), (10, 20, 230, 255))
            self.assertEqual(result.getpixel((4, 8)), (0, 0, 0, 0))

    def test_arrows_is_a_beta_32px_sheet_with_a_bundled_fallback(self):
        """item/arrows.png: RenderArrow's 32x32 sheet (16x5 shaft strip at 0,0
        and 5x5 endcap at 0,5). Native passes through byte-identical; the
        bundled default is the Earth & Ash sheet, so the fallback is real."""
        entry = load_map()["assets"]["item/arrows.png"]
        self.assertEqual(entry["source_names"], ["item/arrows.png"])
        self.assertFalse(entry["required"])
        self.assertEqual(entry["fallback"], "bundled")
        self.assertEqual(entry["canonical_size"], [32, 32])
        self.assertEqual(entry["supported_scales"], [1, 2, 4, 8])
        native = png(Image.new("RGBA", (32, 32), (9, 8, 7, 255)))
        self.assertIs(convert_image(native, "item/arrows.png"), native)
        bundled = Image.open(Path(__file__).resolve().parents[1] / "assets/item/arrows.png")
        self.assertEqual(bundled.size, (32, 32))
        self.assertIsNotNone(bundled.convert("RGBA").crop((0, 0, 16, 5)).getbbox())
        self.assertIsNotNone(bundled.convert("RGBA").crop((0, 5, 5, 10)).getbbox())

    def test_non_square_canonical_takes_one_uniform_scale_only(self):
        """w/cw == h/ch, and that one factor must be a supported scale."""
        native = png(Image.new("RGBA", (64, 256), (12, 34, 56, 255)))
        # Exactly canonical: byte-identical passthrough, no resampling at all.
        self.assertIs(convert_image(native, "environment/rain.png"), native)
        doubled = Image.new("RGBA", (128, 512), (0, 0, 0, 0))
        doubled.paste((200, 10, 10, 255), (0, 0, 2, 2))
        result = decoded(convert_image(png(doubled), "environment/rain.png"))
        self.assertEqual(result.size, (64, 256))
        self.assertEqual(result.getpixel((0, 0)), (200, 10, 10, 255))
        # Stretched differently across than down -- exactly the real jar's
        # 64x256 against the old 32x32 canonical.
        with self.assertRaises(ConversionError) as caught:
            convert_image(png(Image.new("RGBA", (128, 256))), "environment/rain.png")
        self.assertEqual(caught.exception.code, "unsupported_dimensions")
        self.assertIn("64x256", str(caught.exception))
        # Not a whole multiple on either axis.
        with self.assertRaises(ConversionError) as caught:
            convert_image(png(Image.new("RGBA", (48, 192))), "environment/snow.png")
        self.assertEqual(caught.exception.code, "unsupported_dimensions")
        # A whole, uniform multiple that the entry does not declare.
        with self.assertRaises(ConversionError) as caught:
            convert_image(png(Image.new("RGBA", (64 * 16, 256 * 16))),
                          "environment/rain.png")
        self.assertEqual(caught.exception.code, "unsupported_dimensions")

    def test_a_caller_supplied_map_is_the_one_converted_against(self):
        """A non-default map must govern the canonical size too, not just selection."""
        custom = load_map()
        custom["assets"]["misc/dial.png"] = dict(custom["assets"]["misc/dial.png"],
                                                 canonical_size=[8, 8])
        data = png(Image.new("RGBA", (16, 16), (7, 7, 7, 255)))
        self.assertEqual(decoded(convert_image(data, "misc/dial.png",
                                               conversion_map=custom)).size, (8, 8))
        self.assertEqual(decoded(convert_image(data, "misc/dial.png")).size, (16, 16))

    def test_terrain_32_64_128_tiles_convert_to_16_without_cell_bleed(self):
        for scale in (2, 4, 8):
            image = Image.new("RGBA", (256 * scale, 256 * scale))
            for row in range(16):
                for column in range(16):
                    color = (column * 16, row * 16, (column + row) * 8, 255)
                    image.paste(color, (column * 16 * scale, row * 16 * scale,
                                        (column + 1) * 16 * scale,
                                        (row + 1) * 16 * scale))
            result = decoded(convert_image(png(image), "terrain.png"))
            self.assertEqual(result.size, (256, 256))
            self.assertEqual(result.getpixel((4 * 16 + 8, 3 * 16 + 8)),
                             (64, 48, 56, 255))
            self.assertEqual(result.getpixel((5 * 16, 3 * 16 + 8)),
                             (80, 48, 64, 255))

    def test_canonical_png_is_byte_identical(self):
        data = png(Image.new("RGBA", (256, 256), (1, 2, 3, 4)))
        self.assertIs(convert_image(data, "gui/items.png"), data)
        with self.assertRaises(ConversionError) as caught:
            convert_image(data[:24], "gui/items.png")
        self.assertEqual(caught.exception.code, "invalid_png")

    def test_smooth_alpha_has_one_analytical_premultiplied_box_filter(self):
        image = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        image.putpixel((0, 0), (255, 0, 0, 128))
        image.putpixel((1, 0), (0, 0, 255, 64))
        image.putpixel((0, 1), (255, 0, 0, 128))
        image.putpixel((1, 1), (0, 0, 255, 64))
        result = decoded(convert_image(png(image), "particles.png", "smooth"))
        self.assertEqual(result.getpixel((0, 0)), (170, 0, 85, 96))

    def test_smooth_cell_boundaries_do_not_bleed_at_any_supported_tile_size(self):
        for scale in (2, 4, 8):
            image = Image.new("RGBA", (256 * scale, 256 * scale), (0, 0, 0, 0))
            cell = 16 * scale
            image.paste((255, 0, 0, 128), (0, 0, cell, cell))
            image.paste((0, 0, 255, 64), (cell, 0, 2 * cell, cell))
            result = decoded(convert_image(png(image), "terrain.png", "smooth"))
            self.assertEqual(result.getpixel((15, 8)), (255, 0, 0, 255))
            self.assertEqual(result.getpixel((16, 8)), (0, 0, 255, 255))

    def test_cutout_coverage_uses_wii_effective_alpha_threshold(self):
        image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
        left, top = 4 * 32, 3 * 32  # Beta leaves, terrain tile (4,3).
        for index in range(40):
            image.putpixel((left + index % 32, top + index // 32),
                           (10, 200, 20, 19))
        for index in range(40, 80):
            image.putpixel((left + index % 32, top + index // 32),
                           (10, 200, 20, 18))
        result = decoded(convert_image(png(image), "terrain.png", "smooth"))
        tile = result.crop((4 * 16, 3 * 16, 5 * 16, 4 * 16))
        alphas = [pixel[3] for pixel in flattened_pixels(tile)]
        self.assertEqual(sum(alpha >= 19 for alpha in alphas), 10)
        self.assertEqual(set(alphas), {0, 255})

    def test_glass_and_ice_keep_continuous_alpha(self):
        image = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
        image.paste((90, 120, 150, 77), (1 * 32, 3 * 32, 2 * 32, 4 * 32))
        image.paste((80, 110, 140, 173), (3 * 32, 4 * 32, 4 * 32, 5 * 32))
        result = decoded(convert_image(png(image), "terrain.png", "smooth"))
        self.assertEqual(result.getpixel((1 * 16 + 8, 3 * 16 + 8))[3], 77)
        self.assertEqual(result.getpixel((3 * 16 + 8, 4 * 16 + 8))[3], 173)

    def test_crisp_font_keeps_glyph_cell_lookup(self):
        image = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        image.paste((17, 34, 51, 255), (3 * 16, 5 * 16, 4 * 16, 6 * 16))
        result = decoded(convert_image(png(image), "font/default.png", "crisp"))
        self.assertEqual(result.size, (128, 128))
        self.assertEqual(result.getpixel((3 * 8 + 4, 5 * 8 + 4)),
                         (17, 34, 51, 255))
        self.assertEqual(result.getpixel((2 * 8 + 4, 5 * 8 + 4)), (0, 0, 0, 0))
        self.assertEqual(convert_image(png(image), "font/default.png", "smooth"),
                         convert_image(png(image), "font/default.png", "crisp"))

    def test_gui_forces_crisp_when_smooth_is_requested(self):
        image = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
        image.paste((255, 255, 255, 255), (0, 0, 1, 1))
        data = png(image)
        for asset in ("gui/gui.png", "gui/icons.png"):
            self.assertEqual(convert_image(data, asset, "smooth"),
                             convert_image(data, asset, "crisp"))

    def test_item_art_honors_smooth_choice(self):
        image = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
        image.putpixel((0, 0), (255, 0, 0, 255))
        image.putpixel((1, 0), (0, 0, 255, 255))
        image.putpixel((0, 1), (255, 0, 0, 255))
        image.putpixel((1, 1), (0, 0, 255, 255))
        data = png(image)
        smooth = decoded(convert_image(data, "gui/items.png", "smooth"))
        crisp = decoded(convert_image(data, "gui/items.png", "crisp"))
        self.assertEqual(smooth.getpixel((0, 0)), (128, 0, 128, 255))
        self.assertNotEqual(smooth.getpixel((0, 0)), crisp.getpixel((0, 0)))

    def test_skin_aspect_is_strict_and_regions_do_not_bleed(self):
        image = Image.new("RGBA", (128, 64), (0, 0, 0, 0))
        image.paste((255, 0, 0, 128), (0, 0, 16, 16))
        image.paste((0, 0, 255, 64), (16, 0, 32, 16))
        result = decoded(convert_image(png(image), "mob/char.png", "smooth"))
        self.assertEqual(result.getpixel((7, 4)), (255, 0, 0, 128))
        self.assertEqual(result.getpixel((8, 4)), (0, 0, 255, 64))
        with self.assertRaises(ConversionError) as caught:
            convert_image(png(Image.new("RGBA", (128, 128))), "mob/char.png")
        self.assertEqual(caught.exception.code, "unsupported_dimensions")

    def test_source_alias_and_unsupported_layout_reporting(self):
        chest = png(Image.new("RGBA", (512, 512), (9, 8, 7, 255)))
        self.assertEqual(decoded(convert_image(chest, "gui/container.png")).size,
                         (256, 256))
        with self.assertRaisesRegex(ConversionError, "unsupported asset") as caught:
            convert_image(chest, "mob/custom.png")
        self.assertEqual(caught.exception.code, "unsupported_asset")
        with self.assertRaises(ConversionError) as caught:
            convert_image(png(Image.new("RGBA", (256, 256))), "gui_2.png")
        self.assertEqual(caught.exception.code, "composition_required")
        with self.assertRaises(ConversionError) as caught:
            convert_image(png(Image.new("RGBA", (256, 256))), "controls.png")
        self.assertEqual(caught.exception.code, "bundled_only")

    def test_scaled_animation_frames_are_isolated_and_borders_rebuilt(self):
        image = Image.new("RGBA", (512, 512), (9, 8, 7, 6))
        for column, color in ((0, (255, 0, 0, 128)), (1, (0, 0, 255, 64))):
            left, top = column * 36 + 6, 6
            image.paste(color, (left, top, left + 32, top + 32))
        result = decoded(convert_image(png(image), "anim.png", "smooth"))
        self.assertEqual(result.getpixel((3 + 7, 3 + 7)), (255, 0, 0, 128))
        self.assertEqual(result.getpixel((18 + 3 + 7, 3 + 7)), (0, 0, 255, 64))
        self.assertEqual(result.getpixel((2, 2)), (255, 0, 0, 128))
        self.assertEqual(result.getpixel((19, 19)), (255, 0, 0, 128))
        self.assertEqual(result.getpixel((20, 2)), (0, 0, 255, 64))

    def test_decode_bounds_are_checked_before_pillow(self):
        data = bytearray(_minimal_png_header(5000, 1))
        with self.assertRaises(ConversionError) as caught:
            convert_image(bytes(data), "terrain.png")
        self.assertEqual(caught.exception.code, "decoded_too_large")

    def test_gui_composite_changes_only_documented_regions(self):
        base = Image.new("RGBA", (256, 256), (10, 20, 30, 255))
        gui = Image.new("RGBA", (256, 256), (100, 0, 0, 255))
        icons = Image.new("RGBA", (256, 256), (0, 100, 0, 255))
        result = decoded(compose_gui2(png(base), png(gui), png(icons)))
        self.assertEqual(result.getpixel((181, 21)), (100, 0, 0, 255))
        self.assertEqual(result.getpixel((208, 23)), (100, 0, 0, 255))
        self.assertEqual(result.getpixel((0, 229)), (0, 100, 0, 255))
        self.assertEqual(result.getpixel((100, 100)), (10, 20, 30, 255))

    def test_gui_composite_replaces_transparent_overlay_pixels(self):
        base = Image.new("RGBA", (256, 256), (10, 20, 30, 255))
        transparent = Image.new("RGBA", (256, 256), (200, 100, 50, 0))
        result = decoded(compose_gui2(png(base), png(transparent), png(transparent)))
        self.assertEqual(result.getpixel((10, 10)), (200, 100, 50, 0))
        self.assertEqual(result.getpixel((0, 229)), (200, 100, 50, 0))
        self.assertEqual(result.getpixel((100, 100)), (10, 20, 30, 255))

    def test_anim_composite_writes_ice_content_and_clamped_border_only(self):
        base = Image.new("RGBA", (256, 256), (1, 2, 3, 4))
        terrain = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        for y in range(16):
            for x in range(16):
                terrain.putpixel((3 * 16 + x, 4 * 16 + y), (x, y, 99, 200))
        result = decoded(compose_anim(png(base), png(terrain)))
        origin_x, origin_y = 4 * 18 + 2, 2
        self.assertEqual(result.getpixel((origin_x, origin_y)), (0, 0, 99, 200))
        self.assertEqual(result.getpixel((origin_x + 17, origin_y + 17)),
                         (15, 15, 99, 200))
        self.assertEqual(result.getpixel((origin_x + 1 + 7, origin_y + 1 + 9)),
                         (7, 9, 99, 200))
        self.assertEqual(result.getpixel((10, 10)), (1, 2, 3, 4))


def _minimal_png_header(width, height):
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sII", 13, b"IHDR", width, height)



class ColourMapTests(unittest.TestCase):
    def test_hd_colour_map_samples_back_to_the_exact_original(self):
        import io as _io
        from PIL import Image as _Image
        original = _Image.new("RGBA", (256, 256))
        original.putdata([(x, y, (x * y) & 255, 255) for y in range(256) for x in range(256)])
        for scale in (1, 2, 4, 8):
            big = original.resize((256 * scale, 256 * scale), _Image.Resampling.NEAREST)
            buf = _io.BytesIO(); big.save(buf, "PNG")
            for name in ("misc/grasscolor.png", "misc/foliagecolor.png"):
                with _Image.open(_io.BytesIO(convert_image(buf.getvalue(), name))) as out:
                    self.assertEqual(out.convert("RGBA").tobytes(), original.tobytes(), (name, scale))
        odd = _io.BytesIO(); original.resize((300, 300)).save(odd, "PNG")
        with self.assertRaises(ConversionError) as caught:
            convert_image(odd.getvalue(), "misc/grasscolor.png")
        self.assertEqual(caught.exception.code, "unsupported_dimensions")

if __name__ == "__main__":
    unittest.main()
