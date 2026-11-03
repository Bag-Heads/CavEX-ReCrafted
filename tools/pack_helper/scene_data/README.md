# Imported-pack Nether scene map

This is CaveWii-generated sampling and shading data, not texture artwork.
The importer paints it with the user's converted `terrain.png` and `anim.png`.
No Mojang pixels, world files, executable, shader runtime, or engine libraries
are shipped for scene composition. The Python runtime needs only Pillow and
the standard library. Normal import adds `gui/nether-background.png` and its
provenance Markdown; failure warns and leaves the rest of the import usable.

The fixed recipe is the shipped 57f scene: seed 42, eye
(-59.5, 36.62, 100.5), yaw 90, polar pitch 78, frame 640x480,
crop (0,96)-(640,456), pixel-centre nearest scaling to 1024x576.
Animation uses frame zero, including the game's second-fire-strip fallback.
No world-generation or gameplay rules are implemented in this tool.

## Generation

`capture_nether_map.py` runs a PC engine offline in a new private directory.
It uses synthetic atlas pixels `(source_x, source_y, sheet)` to export IDs,
and black/white captures to export per-channel lighting/fog endpoints.
Only that directory's fragment shader is instrumented. `sheet=0` denotes
the Beta 16x16 terrain grid; `sheet=1` denotes the engine's 18px-pitch
animation atlas, preserving the sampled lava/fire frame coordinates;
`sheet=255` is the untextured Nether background. Thus terrain cell index is
`(y//16)*16+x//16`, and its in-cell texel is `(x%16,y%16)`.

Mushrooms and fire need sparse underlying samples, because each pack owns
its alpha mask. The capture removes already sampled texel IDs at those
screen pixels, retaining coincident mushroom faces. Fire additionally keeps
the original depth-prepass limit (24-bit captured depth plus 1e-7 for
readback quantization); it does not expose farther animation surfaces when
a foreground fire texel is transparent. Capture fails rather than emitting
a truncated map if peeling does not terminate. This map reaches opaque
terrain/background after two sparse layers (1,772 and 298 samples).
Solid terrain with transparency beyond those captured layers causes the
optional scene to be skipped explicitly rather than inventing geometry.

The initial map used the retained 57d PC renderer that produced the shipped
57f artwork. Its SHA256 and the shader/config/capture hashes are recorded
in `nether_scene_map.json`. That historical executable is an offline input,
not part of the repository data or importer. To reproduce those inputs:

```text
python tools/pack_helper/capture_nether_map.py --engine <57d-scene-engine.exe> --legacy-57d-engine --out <new-capture-directory>
python tools/pack_helper/encode_nether_map.py <new-capture-directory> <new-map-directory>
```

Set PATH to the engine's required DLL directory first. Without the legacy
flag the capture tool uses the current PC engine's normal title-cache bake.
Run under the project build lock. Changing engine/driver/recipe requires
fresh paired comparisons before changing the map's pinned hash.

Beta references checked through beta-decompile: `RenderEngine.setupTexture`
uses nearest sampling by default (`RenderEngine.java:168-169`), and
`WorldProviderHell.func_4096_a` supplies Nether fog (0.2,0.03,0.03).
The map measures the existing CaveWii renderer, including its existing
fixed population/no-fluid-tick illustration recipe; it is not a new claim
of live-world parity or Wii proof.

## Format and proof

`nether_scene_map.bin` is a zlib stream. Its decoded bytes are:

1. Eight-byte magic `CWNSMAP1`, then a little-endian uint32 palette count.
2. Palette records of nine uint8 values: x, y, sheet, black RGB, white RGB.
3. 1024x576 little-endian uint32 palette indices, row-major.
4. Each sparse layer: uint32 count, then `(pixel index, palette index)` uint32 pairs.

RGB composition is `round(black + texture * (white-black)/255)`.
Terrain uses the engine's alpha-test threshold with blending off; animated
pixels blend over the underlying sample. Two 8-bit endpoint captures account
for the final rounding difference from the native engine.

Initial compressed size: **682,679 bytes** (3,370,034 decoded bytes).
The hash is independently pinned in the composer and importer test.
Paired 1024x576 comparisons against the shipped renders: maximum error
**(1,1,1)** for Earth & Ash and Isabella II; mean RGB errors respectively
(0.129900,0.139287,0.143346) and (0.134623,0.126877,0.149534).
The private Beta preview also has maximum (1,1,1), mean
(0.133865,0.144306,0.152544). Beta pixels remain outside the repository.
Evidence: `C:/tmp/cavex-jobs/ci-1101/57h/comparison.json` and `previews/`.
An independent rerun of the retained capture tool reproduced the decoded map
byte-for-byte (SHA256 `dcf204b560a650182f44a83c73624f84912747fc09856feb1c282e40c2404129`).
The original Python 3.14 and repeat Python 3.12 encoders emitted different
zlib streams for those identical bytes; the checked-in compressed file's
hash remains the distribution pin, not a cross-zlib reproducibility claim.
