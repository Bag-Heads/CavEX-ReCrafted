Beta pack base -- helper input, not a selectable texture pack
=============================================================

A Minecraft Beta 1.7.3 texture pack (made from YOUR OWN copy of the game by the
Pack Helper, tools/import_owned_beta_assets.py or tools/make_beta_pack.ps1)
lacks two CaveWii files that Beta never had. Since 2026-10-11 the bundled
default art in assets/ is Earth & Ash, so a Beta pack no longer borrows these
from assets/; every Beta preparation route copies them from this folder:

  pointer.png  The Wii pointer hand from the Homebrew Channel
               (https://github.com/fail0verflow/hbc, channel/channelapp/data),
               licensed GNU GPL v2 -- full text LICENSES/GPL-2.0.txt. Kept
               byte-identical to the file assets/pointer.png held until
               2026-10-11 (sha256 f5b81b0348c5...).
  anim.png     The animated-texture sheet baked from CaveWii's faithful ports
               of Beta 1.7.3's procedural fire, lava, water, flowing water and
               portal (tools/gen_anim_fx/bake_anim_fx.sh, no colour ramp, so
               Beta's own colours). Byte-identical to the assets/anim.png held
               until 2026-10-11 (sha256 51df2cde33dd...); pinned texel-for-texel
               by tests/test_anim_pack_art.c. The helper composites the pack's
               own Beta ice over it.

The player's own jar supplies everything else, including font/default.png.

assets/beta-base/anim.png is a byte copy of anim.png above, shipped on the
SD card so the game itself can compose anim.png (and gui_2.png, and read
gui/container.png as the chest) when a player unzips b1.7.3.jar straight
into texturepacks/<name>/ without the helper (source/platform/
tex_beta_compose.h; tests/test_tex_beta_compose.py keeps both identical).
