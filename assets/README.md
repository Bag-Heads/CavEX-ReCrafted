## Texture licenses

Please note that the general license of this repository does not apply to the files listed below, which are contained in this directory only.

The bundled default textures are **Earth & Ash**: the 57 files in
[earth-and-ash-manifest.json](earth-and-ash-manifest.json) are original CC0
art. The rest of this directory is **not CC0 as a whole**: every other file
carries the license of its own entry below (controller glyph sheets,
colormaps, sounds and so on). No
Isabella II file is in this directory any more.

**Earth & Ash is the full default; Isabella II is only an optional pack
(2026-10-10).** Following the maintainer's rulings of 2026-10-10, the last
eight sheets that were Isabella II art or fork art were replaced by the
Earth & Ash pack's own files: `terrain.png`, `gui/items.png`,
`particles.png`, `terrain/sun.png`, `terrain/moon.png`,
`environment/clouds.png`, `gui_2.png` and `gui/iron_chest.png` (the terrain
and item atlases include the 38 round-2 tile redraws approved that day).
The Isabella II art ships ONLY as an optional, PARTIAL texture pack: tracked
source in `packs/isabella-ii/`, copied by `tools/make_release.sh` into the
release zip at `texturepacks/isabella-ii/`. Pick it on the Texture Packs
screen (listed as `isabella-ii`). It holds only 11 images (`terrain.png`,
`particles.png`, `gui/items.png`, `terrain/sun.png`, `terrain/moon.png`,
`environment/clouds.png`, `gui/crafting.png`, `gui/furnace.png`,
`gui/gui.png`, `gui/icons.png`, `gui/inventory.png`), its own original CC0
`pointer.png` and `font/default.png`, its own adapted `anim.png`, plus its
credit files;
every file it lacks comes from the Earth & Ash default in this directory.
`anim.png` (fire, lava, water, flowing water, portal) moves exactly like
Beta 1.7.3 -- the bake of the faithful animated-texture ports -- in Earth &
Ash colours (2026-10-11, maintainer: every pack keeps Beta's behaviour in its
own colours; `artwork/fx_ramps.py`, checked by `tests/test_anim_pack_colours.py`).
The Beta-coloured bake moved to `packs/beta-base/anim.png` for Beta packs
(pinned by `tests/test_anim_pack_art.c`).
`item/arrows.png` (flying arrows) is new Earth & Ash art drawn on
2026-10-11; before it no bundled set had an arrow sheet. Importing your own Beta 1.7.3 pack is unchanged.

#### Earth & Ash textures — integrated 2026-09-11

The PNG files enumerated in [earth-and-ash-manifest.json](earth-and-ash-manifest.json)
are the bundled defaults: since 2026-09-11 all 19 mob/player skins, ten
armor layers, seven GUI sheets, minecart, paintings, rain/snow, map
background, clock dial, underwater overlay and entity shadow (44 files),
since 2026-10-05 the map icons, and since 2026-10-10 the terrain and item
atlases, particles, sun, moon, clouds, `gui_2.png` and `gui/iron_chest.png`
(53 files), and since 2026-10-11 the flying-arrow sheet `item/arrows.png`
(54 files), and since 2026-10-11 also its pointer, font and animated-texture
sheet `anim.png` (57 files in all). This includes the revised player skin.
They are original CC0 artwork authored for CaveWii, inspired by Isabella II's
muted, weathered palette; no Mojang or Isabella raster pixels were copied.
The manifest records dimensions, hashes and per-file provenance.

License: [CC0 1.0 Universal — full text](CC0-1.0.txt).
Reproducible artwork sources and design prompts: `artwork/cc0-pack/` in the
repository. The player design study used built-in imagegen; final PNGs use
original scripted/hand-placed native pixels. Previews and design studies are
not runtime textures and are not included in this manifest.

This entry supersedes the earlier placeholder-generator credits below for
paths listed in the manifest, and supersedes the Isabella catch-all for the
new GUI sheets. In particular `misc/water.png` is now original CC0 artwork;
release packaging admits its verified hash while excluding older unverified
water files. Historical entries below describe the earlier revisions only.
All remaining textures, controller glyphs, fonts and sounds retain their
existing licenses. The combined default asset tree is not entirely CC0.
User-supplied packs still override the bundled defaults.

#### font/default.png

Since 2026-10-11 this is the Earth & Ash font (CC0, listed in the manifest
above; same 256 advance widths as before). Until then it was the 8x8px tiled
bitmap font by xtreme8000, licensed
[Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/);
that font is no longer bundled. Each texture pack carries its own font: a
Beta 1.7.3 pack uses the one from the player's own game, and the optional
Isabella II pack an original CC0 font.

#### controls.png

(2026-09-02) Replaced. The previous version credited the [WiiBrew wiki](https://wiibrew.org/wiki/Category:Controller_Buttons)
(users Clorox/Flark/Hiker13526/Crayon) under a license recorded here as
"[seems to be unknown](https://wiibrew.org/wiki/Category_talk:Controller_Buttons#License)" --
an unresolved provenance that `tools/make_release.sh` handled by excluding
the file from every release zip. It is now an original 256x256 icon atlas
generated from scratch by `tools/gen_cc0_controls.py`: an 8x8 grid of 32x32
button/stick glyphs (simple outlined shapes + text labels, colour-coded by
controller family per `docs/wii_controller_playability.md`'s control
scheme) at the exact grid layout `source/graphics/gui_util.c`'s
`gutil_control_icon()` already indexes into (`symbol_help % 8`, `symbol_help
/ 8`, per `source/platform/input.c`'s symbol tables) -- so every existing
per-button on-screen hint keeps pointing at the right cell. Correction (2026-10-27): the generated cells were later overwritten with the WiiBrew button
cells 0-31 (tools/apply_gc_glyph_style_c.py, artwork/controller-glyphs/README.md), so this file
DOES carry WiiBrew pixels (uploaders Clorox, Hiker13526, Flark, Crayon; no licence stated; see
NOTICE). We did not make those cells; we use them with credit. The remaining cells are CC0 1.0.

License: [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)

#### armor/*.png

(2026-09-02) Replaced, all 10 files (`{chain,cloth,gold,iron,diamond}_{1,2}.png`).
The previous files arrived in the same source commit as confirmed Mojang
rips (`docs/m3_legal_pack_plan.md` S3) and were never provenance-cleared --
`tools/make_release.sh` excluded the whole `armor/` directory from every
release zip as a result, and two of the ten (`iron_1.png`/`iron_2.png`) were
128x64 against every other file's 64x32, a mismatch with what
`source/graphics/render_model.c`'s `render_model_box()` actually expects
(it hard-codes a 64x32 UV scale for every box it draws, armor included --
so the old iron files were silently mis-mapped on top of being suspect).
The replacements are generated from scratch by `tools/gen_cc0_armor.py`:
flat/shaded box-net renders (one material colour per tier -- leather/cloth
brown, chain grey with a procedural ring-mesh pattern, iron light grey,
gold yellow, diamond cyan) painted at the exact classic Beta humanoid UV
regions (head/body/arm/leg) `render_model_player()` samples, all at the
code's real 64x32. No Mojang pixels were read, referenced, or traced.
Dedicated to the public domain under CC0 1.0 Universal.

License: [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)

#### pointer.png

Since 2026-10-11 this is the Earth & Ash pointer (CC0, listed in the
manifest above). The Homebrew Channel pointer described below moved to
`packs/beta-base/pointer.png` and is copied only into Beta 1.7.3 packs
prepared from the player's own game; the optional Isabella II pack has an
original CC0 pointer. Historical entry for the Homebrew pointer:

Taken from [Homebrew channel](https://github.com/fail0verflow/hbc/tree/master/channel/channelapp/data), licensed GNU GPL v2, according to [wiki](http://wiibrew.org/wiki/Homebrew_Channel).
Kept as-is (known license, not suspect provenance -- unlike controls.png
and armor/*.png above, which had unresolved/suspect provenance and were
replaced with original CC0 art instead). The full GPLv2 license text ships
with every release under `LICENSES/GPL-2.0.txt` and is recorded in the
repo-root `NOTICE` file.

#### sound/

Except for the approved `sound/menu/` pack documented in
[sound/menu/README.md](sound/menu/README.md), files under `assets/sound/` are original CC0 placeholder sounds,
procedurally generated for this project (S0 set 2026-07-28; S2 additions —
`random/{splash,fizz,drr}`, `fire/`, `liquid/`, `portal/`, `tile/piston/`,
`mob/slime*`, `ambient/cave/` — same day, same method) — simple synthesized
tones/noise bursts, 16-bit mono PCM WAV at 22050 Hz. They exist so every
wired sound event resolves a file out of the box; a user-supplied texture
pack's `sound/` directory overrides them per sound KEY (the S1 pool rule).
No Mojang audio is included or ever will be (legal-assets rule).

License: [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)

#### mob/wolf*.png

The three wolf variants are original procedural placeholders generated by
`tools/make_default_pack.py --wolf`. They contain no Mojang pixels and are
dedicated to the public domain under CC0 1.0 Universal.

#### art/kz.png

The painting motive atlas is an original procedural placeholder generated by
`tools/gen_kz_placeholder.py`. Every one of the 25 EnumArt tiles (plus the
generic back/side frame-border strip) is drawn from the script's own flat
fills/gradients/geometric motifs at the exact Beta pixel offset/size table --
it contains no Mojang pixels and no tracing of any real kz.png (none is
present in this repo to trace from). Dedicated to the public domain under
CC0 1.0 Universal. A user's own Beta texture pack's `art/kz.png` overrides
this file automatically (the pack-first lookup every other asset here uses).

#### misc/grasscolor.png, misc/foliagecolor.png

(2026-09-02) Replaced. The 2026-09-02 Isabella II provenance audit
(`docs/asset_provenance_2026-09-02.md`) found both files 0% pixel-identical
to an archived copy of the Isabella II pack despite matching 256x256
dimensions, and neither had any other documented credit entry -- i.e. their
real origin was untraceable (most likely Mojang's own jar colormaps, which
is exactly why they trace to nothing here). A first pass excluded them from
the release zip, but `source/graphics/biome_color.c`'s `biome_color_init()`
loads both **unconditionally** on every `tex_init()` and treats a missing
colormap as "no tint" (pure white) -- so excluding them would have made
every grass/leaf face render white, not a graceful missing-overlay case
like `misc/water.png`. Regenerated from scratch instead by
`tools/gen_cc0_colormaps.py`: an original 2D bilinear gradient across four
hand-picked corner colours (warm-dry olive/yellow-green, warm-wet vivid
jungle green, cold-dry pale grayish-green, cold-wet deep saturated green
for grass; the same four corners shifted darker/bluer for foliage),
addressed with the exact temperature/humidity index math
`source/worldgen/worldgen.c`'s `worldgen_climate()` actually uses (`ci =
(1-temp)*255`, `cj = (1-humidity*temp)*255`) -- see the script's own
docstring for the full derivation. No Mojang or Isabella pixels were read,
referenced, or traced. Dedicated to the public domain under CC0 1.0
Universal.

License: [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)

#### mob/char.png, mob/creeper.png, mob/pig.png, item/cart.png, misc/shadow.png

(2026-06-15, commit `9ccbe33` -- "M3(B): generate CC0 default art -- replace
the mob rips + fill the gaps at HEAD") Original, from-scratch CC0
placeholders generated by `tools/make_default_pack.py` (its default,
no-flag run -- P0 replaces the tracked Mojang mob rips, P1 fills the
default-pack gaps the M3(A) coverage gate flagged). `mob/char.png`,
`mob/creeper.png`, and `mob/pig.png` replaced tracked Mojang-derived rips
(creeper is the universal mob fallback, so every mob renders CC0 art even
before the rest of the roster ships); the old blobs are gone from this
commit's tree (only reachable via history, which `tools/make_release.sh`
never reads -- it packages a commit's tree via `git show`, so they can
never end up in a release zip). `item/cart.png` and `misc/shadow.png` are
new files filling gaps, not replacements. All five are procedural
flat/shaded renders at the exact UV layout `render_entity.c`/the relevant
render code already expects (functional, not copyrightable); only the
pixels are original, and they are original here. No Mojang pixels were
read, referenced, or traced. Minimal CC0 placeholders -- a user's own
Beta texture pack overrides every one of these automatically. Dedicated
to the public domain under CC0 1.0 Universal.

License: [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)

#### Isabella II (historical default; now only the optional pack)

Until 2026-10-10 this "everything else" entry covered the bundled
`terrain.png`, `particles.png`, `gui/items.png`, `terrain/sun.png`,
`terrain/moon.png` and `environment/clouds.png`. Those are Earth & Ash CC0
art now (entry above), so this entry no longer covers any file in this
directory. It is kept because the same Isabella II art ships as the optional
`texturepacks/isabella-ii/` pack (source `packs/isabella-ii/`), and the
credit below applies to that pack.

Those textures are from the [Isabella II](http://www.minecraftforum.net/topic/242175-Isabella/) texture pack by Bonemouse, extracted from the "1.2.5 Beta 1" release (`Isabella 2 1.2vb1.zip`).
They are licensed under Creative Commons Attribution 3.0 Unported (CC BY 3.0),
**not CC0**: credit Bonemouse and link the thread
http://www.minecraftforum.net/topic/242175-Isabella/ when redistributing.
The pack's unedited `About Isabella.txt` and the CC BY 3.0 license text ship in
`LICENSES/Isabella-II/` (repository root, and in every release zip).
The original minecraftforum.net thread is now a dead link (verified
2026-09-02: every Wayback Machine snapshot at that URL is a bare redirect,
no captured content). The Bonemouse authorship and CC-BY 3.0 license are
independently corroborated by a third-party continuation project's own
README: [minetest-texture-packs/Isabella-II](https://github.com/minetest-texture-packs/Isabella-II)
("A moody texture pack by 'Bonemouse', licensed CC-BY 3.0"), and a
closely-matching Beta-1.7.3-era copy of the pack (`IsabellaIIRetro.zip`,
not confirmed to be byte-identical to the originally-cited
`Isabella 2 1.2vb1.zip`, but sharing the same directory layout and, file by
file, either byte-identical or majority-identical pixel content with what
ships here) is preserved at the Internet Archive:
[150+ Minecraft Beta 1.7.3 Texture Packs Collection](https://archive.org/details/minecraft_beta_texture_packs_collection_by_beezle),
compiled by Beezle. See `docs/asset_provenance_2026-09-02.md` for the
full per-file hash/pixel-comparison audit against that archived copy,
including the files this blanket "everything else" statement does NOT
actually cover (`misc/foliagecolor.png`, `misc/grasscolor.png`, and
`misc/water.png` all compared 0% identical to the archived Isabella pack
despite matching dimensions -- their real origin was unconfirmed. The two
colormaps have since been replaced with original CC0 art, own entry above;
the earlier `misc/water.png` was excluded from release zips (now replaced by
Earth & Ash CC0 artwork above), see
`tools/make_release.sh`. Also not covered: `mob/char.png`,
`mob/creeper.png`, `mob/pig.png`, `item/cart.png`, and `misc/shadow.png`
-- original CC0 art per commit `9ccbe33`, own entry above -- which never
traced to Isabella II in the first place and should not have been left to
fall through to this catch-all).

Also NOT Isabella II: `anim.png`, `gui_2.png`, `gui/iron_chest.png` and
`gui/chest.png`, once fork-made sheets (`docs/asset_provenance_2026-09-02.md`),
are now Earth & Ash CC0 art (entry above). The optional
`texturepacks/isabella-ii/` pack has its own adapted `anim.png`.

License: [Creative Commons Attribution 3.0 Unported License](https://creativecommons.org/licenses/by/3.0/)

#### gui/menu-background.png — front-end landscape

This optional runtime backdrop is a resized derivative of the existing original
Earth & Ash generated valley illustration, dedicated under CC0 1.0 Universal.
It is the explicit exception to the historical statement above that design
studies are not runtime textures: only this derivative is promoted. It is
illustration, not an in-game capture or simulation output. Source/output hashes,
dimensions and the exact conversion are recorded in
[its provenance manifest](gui/menu-background.provenance.json) and
[asset notes](gui/menu-background.md). Pack-provided `gui/menu-background.png`
overrides it through ordinary texture lookup; other packs without a backdrop
use this bundled fallback. No existing atlas or controller artwork is changed.

`gui/reception.png` is the optional nine-frame server status sheet rasterized exactly
from the committed original Earth SVG. Packs may supply their own sheet through normal
pack-first resolution; missing art uses the truthful neutral native status indicator.
See `gui/reception.provenance.json` for source/output hashes. Controller glyph sheets
remain shared and unchanged.

#### gui/controller-pictures.png ? controller pictures

Original CaveWii team art, maintainer-approved 2026-10-28; GPL-3.0-or-later
(see NOTICE). Byte-for-byte FINAL sheet from
`tmp/jobs/design-round-1028c/opus/controller-pictures-final.png`: 256x256 RGBA,
20287 bytes, SHA-256
`85b6a6b05efa4336d084af89364f24d7c213bcc8f8f883ae3d5d060a87617c18`. Large cells are 24/56/96/120x64; minis are 40x24.
