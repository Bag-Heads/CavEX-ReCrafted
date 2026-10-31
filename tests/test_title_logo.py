#!/usr/bin/env python3
"""Approved title asset and production draw/encoder contracts.

python -I tests/test_title_logo.py --asset-only never invokes a compiler.
The default CTest path compiles the real GX encoder and lifted production
functions against a recording sink, like test_pc_scissor_coordinates.py.
It does not need GL, Dolphin, or a new engine compile profile.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PALETTE = {
    (0, 0, 0, 0), (4, 3, 3, 255), (41, 40, 42, 255),
    (89, 85, 88, 255), (134, 126, 126, 255), (171, 161, 158, 255),
    (174, 164, 161, 255), (180, 169, 165, 255), (225, 220, 215, 255),
}


def asset_checks(source):
    spec = importlib.util.spec_from_file_location(
        "make_title_logo", ROOT / "tools/make_title_logo.py")
    maker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(maker)
    source = source or maker.SOURCE
    path = ROOT / "assets/gui/title-logo.png"
    with Image.open(path) as image:
        assert image.mode == "RGBA" and image.size == (320, 148)
        rgba = image.tobytes()
        assert image.getbbox() == (0, 0, 320, maker.ALPHA_BOTTOM) == (0, 0, 320, 124)
        assert {colour for _, colour in image.getcolors()} == PALETTE
        assert image.crop((0, 124, 320, 148)).tobytes() == bytes(320 * 24 * 4)
    provenance = json.loads(path.with_suffix(".provenance.json").read_text())
    assert provenance["source_sha256"] == maker.SOURCE_SHA256
    assert provenance["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()
    assert provenance["crop"] == list(maker.CROP)
    assert provenance["face_centre_x"] == 160
    assert provenance["original_art"] and not provenance["mojang_logo_pixels"]
    assert provenance["creeper_face_choice"]["date"] == "2026-10-06"
    assert provenance["authors"] == ["Opus B2", "Sol M1", "Opus R2"]
    print("PASS asset: 320x148 RGBA, exact 9-colour palette, alpha bbox/padding, provenance")
    # A checkout away from this machine can use the committed asset digest
    # without the non-redistributed reference. An explicit --source must exist.
    if source.exists():
        approved = maker.source_image(source)
        assert maker.slim(approved).crop(maker.CROP).tobytes() == rgba
        with tempfile.TemporaryDirectory() as td:
            rebuilt = Path(td) / "title-logo.png"
            subprocess.run([sys.executable, "-I", str(ROOT / "tools/make_title_logo.py"),
                            "--source", str(source), "--out", str(rebuilt)],
                           check=True, stdout=subprocess.DEVNULL)
            assert rebuilt.read_bytes() == path.read_bytes()
            assert json.loads(rebuilt.with_suffix(".provenance.json").read_text()) == provenance
            bad_source = Path(td) / "changed.png"
            bad_source.write_bytes(source.read_bytes() + b"changed")
            try:
                maker.source_image(bad_source)
            except ValueError:
                pass
            else:
                raise AssertionError("maker accepted a changed source digest")
        print("PASS source: byte-exact crop, reproducible rebuild, changed SHA256 rejected")
    else:
        print("SKIP source crop/rebuild: approved local M1 reference is unavailable")
    return rgba


def function(source, name):
    match = re.search(r"^[\w *]+\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source, re.M)
    assert match, name
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def production_harness():
    title = (ROOT / "source/game/gui/screen_title.c").read_text()
    texture = (ROOT / "source/platform/texture.c").read_text()
    cache = (ROOT / "source/platform/texture_cache.c").read_text()
    gui = (ROOT / "source/graphics/gui_util.c").read_text()
    frontend = (ROOT / "source/game/gui/frontend_presentation.h").read_text()
    # Guards for the real load, optional-asset clearing and existing SD root.
    assert re.search(r'tex_gfx_load_file\(&texture_title_logo,\s*"gui/title-logo.png",\s*'
                     r'TEX_FMT_RGBA32,\s*false\)', texture)
    assert "tex_cache_note_slot(&texture_title_logo);" in texture
    assert "tex_gfx_clear(tex);" in function(texture, "tex_gfx_load_file")
    assert 'pack = "sd:/assets";' in function(texture, "tex_read_impl")
    assert "title_draw_wordmark(&c, width);" in title
    assert "title_draw_splash(&c);" in title
    # The logo slot needed policy v2; later slots (DC-GRASS overlay) bump it further.
    policy = re.search(r"#define TEX_CACHE_POLICY_VERSION (\d+)u", (ROOT / "source/platform/texture_cache.h").read_text())
    assert policy and int(policy.group(1)) >= 2
    slots = re.search(r"static const struct tex_slot s_slots\[\] = \{.*?\n\};", cache, re.S).group()
    slot_type = re.search(r"struct tex_slot \{.*?\n\};", cache, re.S).group()
    bundled = re.search(r"static const char\* const tex_bundled_only_files\[\] = \{.*?\n\};",
                        texture, re.S).group()
    names = sorted(set(re.findall(r"\btexture_\w+", slots)))
    constants = "\n".join(re.findall(r"^#define TT_.*$", title, re.M))
    canvas = re.search(r"struct title_canvas \{.*?\n\};", title, re.S).group()
    return PRELUDE + "\n" + "\n".join([
        "struct tex_gfx " + ", ".join(names) + ";",
        slot_type, "#define S(t, f, fmt) {&t, f, fmt, false, false}", slots,
        bundled, function(texture, "tex_bundled_only"), constants, canvas,
        function(title, "title_canvas_of"), function(title, "tt_d"), function(title, "tt_x"),
        function(title, "title_row_rect"), function(gui, "gutil_font_width"),
        function(frontend, "fep_text_fit"),
        function(gui, "gutil_text_rot_zoom").replace("void gutil_text_rot_zoom(",
                                                   "static void real_text_rot_zoom("),
        ROTATION_RECORDER,
        function(title, "title_draw_wordmark"), function(title, "title_draw_splash"),
    ]) + MAIN


PRELUDE = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/texture.h"
#include "platform/tex_gx_conv.h"
#ifdef NDEBUG
#error "title logo assertions must be enabled"
#endif
#define glm_rad(a) ((a) * 0.017453292519943295F)
struct fep_rect { int x, y, w, h; };
static struct fep_rect fep_rect(int x, int y, int w, int h) {
    return (struct fep_rect){x, y, w, h};
}
static uint8_t font_char_width[256];
static char splash_text[128];
static float menu_motion_time;
static int screen_width, pulse_phase;
static struct tex_gfx* bound;
static float vertices[12], uvs[8];
static int quad_calls, text_calls, text_x[2], text_y[2];
static int minx, miny, maxx, maxy, glyph_calls;
static void gfx_bind_texture(struct tex_gfx* tex) { bound = tex; }
static void gutil_keepout(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static void gfx_draw_quads_flt(size_t n, const float* v, const uint8_t* col,
                               const float* uv) {
    assert(n == 4);
    memcpy(vertices, v, sizeof(vertices)); memcpy(uvs, uv, sizeof(uvs));
    for(int k = 0; k < 16; k++) assert(col[k] == 255);
    quad_calls++;
}
static void gfx_draw_quads(size_t n, const int16_t* v, const uint8_t* col,
                           const uint16_t* uv) {
    assert(n == 4); (void)col; (void)uv;
    for(int k = 0; k < 4; k++) {
        int x = v[k*3], y = v[k*3+1];
        if(x < minx) minx = x;
        if(x > maxx) maxx = x;
        if(y < miny) miny = y;
        if(y > maxy) maxy = y;
    }
    glyph_calls++;
}
static void gutil_text_rgb(int x, int y, const char* s, int font, bool shadow,
                           uint8_t r, uint8_t g, uint8_t b) {
    assert(text_calls < 2 && !strcmp(s, "CavEX") && font == 76 && !shadow);
    int ink = text_calls ? 255 : 63;
    assert(r == ink && g == ink && b == ink);
    text_x[text_calls] = x; text_y[text_calls++] = y;
}
'''

ROTATION_RECORDER = r'''
static void gutil_text_rot_zoom(int x, int y, const char* str, int font,
                                float angle, float zoom, bool shadow,
                                uint8_t r, uint8_t g, uint8_t b) {
    const float pulses[5] = {1.0F, 0.96464466F, 0.95F, 0.96464466F, 1.0F};
    assert(angle == -9.0F && fabsf(zoom-pulses[pulse_phase]) < 0.000001F);
    assert(font == 14 && shadow && r == 255 && g == 255 && b == 160);
    if(texture_title_logo.data) {
        assert(x == (screen_width-640)/2+551 && y == 157);
    } else {
        assert(x == (screen_width-640)/2+592-gutil_font_width(str,font)/2 && y == 136);
    }
    real_text_rot_zoom(x, y, str, font, angle, zoom, shadow, r, g, b);
}
'''

MAIN = r'''
static bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx+bw && bx < ax+aw && ay < by+bh && by < ay+ah;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    enum { W = 320, H = 148, N = W*H*4 };
    uint8_t* rgba = malloc(N), *gx = malloc(N);
    assert(rgba && gx);
    FILE* f = fopen(argv[1], "rb"); assert(f);
    assert(fread(rgba, 1, N, f) == N); fclose(f);
    assert(tex_gx_bytes(TEX_FMT_RGBA32, W, H) == N);
    assert(tex_gx_encode(TEX_FMT_RGBA32, rgba, W, H, gx));
    for(int y = 0; y < H; y++) for(int x = 0; x < W; x++) {
        size_t tile = ((y/4)*(W/4) + x/4)*64;
        size_t p = ((y%4)*4 + x%4)*2, at = (y*W+x)*4;
        assert(gx[tile+p] == rgba[at+3] && gx[tile+p+1] == rgba[at]);
        assert(gx[tile+32+p] == rgba[at+1] && gx[tile+32+p+1] == rgba[at+2]);
    }
    puts("PASS real tex_gx_encode: every asset RGBA byte survives GX RGBA8");
    assert(tex_bundled_only("gui/title-logo.png"));
    assert(!tex_bundled_only("gui/menu-background.png"));
    assert(!tex_bundled_only(NULL));
    size_t slot = 0, count = sizeof(s_slots)/sizeof(s_slots[0]);
    while(slot < count && s_slots[slot].tex != &texture_title_logo) slot++;
    assert(slot > 0 && slot+1 < count);
    assert(!strcmp(s_slots[slot].file, "gui/title-logo.png"));
    assert(s_slots[slot].fmt == TEX_FMT_RGBA32 && !s_slots[slot].linear && !s_slots[slot].atlas);
    assert(s_slots[slot-1].tex == &texture_menu_background);
    assert(s_slots[slot+1].tex == &texture_menu_reception);
    puts("PASS bundled-only brand and actual ordered cache slot: RGBA32, nearest");
    texture_title_logo = (struct tex_gfx){.data=rgba, .width=W, .height=H};
    for(int screen = 640; screen <= 854; screen += 214) {
        screen_width = screen;
        struct title_canvas c = title_canvas_of(screen, 480);
        quad_calls = text_calls = 0;
        title_draw_wordmark(&c, screen);
        assert(quad_calls == 1 && text_calls == 0 && bound == &texture_title_logo);
        assert(vertices[0]+160 == screen/2 && vertices[1] == 20);
        assert(vertices[3]-vertices[0] == 320 && vertices[7]-vertices[1] == 148);
        const float want_uv[8] = {0,0,1,0,1,1,0,1};
        assert(!memcmp(uvs, want_uv, sizeof(uvs)));
        struct fep_rect row = title_row_rect(screen, 480, 0);
        /* ui-build-1026 T03: five rows (166 + 48*i, 44 tall). The art's
         * visible ink is its top 124 rows (the bottom 24 are transparent),
         * so the first row may sit inside the transparent margin, never on
         * the ink. */
        assert(row.x == (screen-640)/2+160 && row.y == 166 && row.w == 320 && row.h == 44);
        assert(!overlap((int)vertices[0],20,320,124,row.x,row.y,row.w,row.h));
        /* All normalized font advances (0..8), short/long strings and pulse
         * phases; the REAL rotated quads include last-glyph padding/shadow.
         * A conservative quad bbox is stricter than the visible-ink bbox. */
        for(int advance = 0; advance <= 8; advance++) {
            memset(font_char_width, advance, sizeof(font_char_width));
            for(int len = 0; len < 128; len++) {
                memset(splash_text, 'W', len); splash_text[len] = 0;
                for(int phase = 0; phase <= 4; phase++) {
                    pulse_phase = phase;
                    menu_motion_time = phase * 0.25F;
                    minx=miny=10000; maxx=maxy=-10000; glyph_calls=0;
                    title_draw_splash(&c);
                    if(glyph_calls) {
                        assert(!overlap(minx,miny,maxx-minx+1,maxy-miny+1,
                                        (int)vertices[0],20,320,124));
                        assert(!overlap(minx,miny,maxx-minx+1,maxy-miny+1,
                                        row.x,row.y,row.w,row.h));
                    }
                }
            }
        }
        texture_title_logo.data = NULL;
        quad_calls = text_calls = 0;
        title_draw_wordmark(&c, screen);
        assert(quad_calls == 0 && text_calls == 2);
        int x = (screen-640)/2 + (640-gutil_font_width("CavEX",76))/2;
        assert(text_x[0] == x+3 && text_y[0] == 41);
        assert(text_x[1] == x && text_y[1] == 37);
        title_draw_splash(&c); /* absent art keeps the original splash layout */
        texture_title_logo.data = rgba;
    }
    {   /* A 960-line PC window: the logo scales with the canvas (2x). */
        screen_width = 1280;
        struct title_canvas c = title_canvas_of(1280, 960);
        quad_calls = 0;
        title_draw_wordmark(&c, 1280);
        assert(quad_calls == 1);
        assert(vertices[0]+320 == 640 && vertices[1] == 40);
        assert(vertices[3]-vertices[0] == 640 && vertices[7]-vertices[1] == 296);
    }
    puts("PASS real title helpers: 640/854 centre, 1:1 normalized UVs, clearance, original text fallback");
    free(rgba); free(gx);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset-only", action="store_true")
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    if args.source is not None and not args.source.exists():
        parser.error("--source does not exist")
    rgba = asset_checks(args.source)
    code = production_harness()
    print("PASS source wiring guards: optional RGBA32/nearest load, SD root, cache policy bump, draw calls")
    if args.asset_only:
        print("NOT RUN: C draw/cache-policy/GX runtime checks (--asset-only; no compilation)")
        return
    gcc = Path("C:/msys64/ucrt64/bin/gcc.exe")
    compiler = str(gcc) if gcc.exists() else shutil.which("gcc") or shutil.which("cc")
    assert compiler, "no C compiler found"
    with tempfile.TemporaryDirectory() as td:
        temp = Path(td)
        c, exe, raw = temp / "title.c", temp / "title.exe", temp / "title.rgba"
        c.write_text(code)
        raw.write_bytes(rgba)
        subprocess.run([compiler, "-std=c99", "-UNDEBUG", "-Wall", "-Wextra",
                        "-I" + str(ROOT / "source"), "-I" + str(ROOT / "third_party"),
                        str(c), str(ROOT / "source/platform/tex_gx_conv.c"),
                        str(ROOT / "source/platform/texture_mip.c"), "-lm", "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe), str(raw)], check=True)


if __name__ == "__main__":
    main()
