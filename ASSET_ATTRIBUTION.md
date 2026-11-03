# CaveWii asset rights and attribution inventory (R18)

This is an inventory, not legal clearance. The machine copy is `tools/release-assets.json`
(schema 1 of `tools/release_scan.py`: one record per distinct sha256 and group, with every
path, licence, author and source). It was generated from git blobs at HEAD and, on
2026-10-30, filtered to the paths that `docs/release-allowlist.json` exports: 162 records.
The private `artwork/` tree and every `docs/` file except `docs/release-install.md`,
`docs/controls_sheet.md` and `docs/site/` are not part of the public repository
(maintainer rulings 2026-10-30, R18 Q1 and Q2), so their records were dropped.
Statuses are `ok` or `credited-licence-unstated`. Nothing here is guessed.

## R18 status

The four shipped files that blocked R18 are resolved by maintainer rulings:

- `icon.png` (the new CAVEX logo): project-own CaveWii art, GPL-3.0-or-later like the code
  (ruling 2026-10-30, Q3).
- `assets/gui/controller-pictures.png`: project-own controller drawings made by the team on
  2026-10-29, GPL-3.0-or-later (ruling 2026-10-30, Q3).
- `packs/beta-base/anim.png` and its byte copy `assets/beta-base/anim.png`: project-own
  output of our generator `tools/gen_anim_fx/bake_anim_fx.sh`, GPL-3.0-or-later (ruling
  2026-10-30, Q3). It is copied into packs built from the player's own game, and the release
  zip ships the `assets/` copy so the game can build `anim.png` for an unconverted jar unzip.
- `assets/gui/approved-controls.png`: CREDITED, LICENCE UNSTATED. Controller button glyphs
  (26 cells) match WiiBrew's Category:Controller_Buttons images (uploaders Clorox,
  Hiker13526, 2008; https://wiibrew.org/wiki/Category:Controller_Buttons). No licence is
  stated on wiibrew.org. Kept unchanged and credited in `NOTICE` (rulings 2026-10-22 and
  2026-10-30). Cells 14, 15, 22 and 23 (Classic L, R, ZL, ZR) are project-own blank
  keycaps (2026-10-31, GPL-3.0-or-later); the game draws the letters with the pack font.

Test fixtures recorded 2026-10-30 (lead ruling): `tests/fixtures/startup_credits/*.png` (3 text-only
renders of our BagHeads boot-credit cards) and `tests/fixtures/achievement/achievement-02-warm-bell.wav`
(the approved original Warm bell cue) are project-own, GPL-3.0-or-later.

Maintainer notes (status `ok`, but confirm):

- `assets/gui/title-logo.png` and `reception.png`: original per their provenance JSON, but no licence dedication is stated (repository GPL-3.0-or-later assumed; CC0 not claimed).
- Isabella II's own `About Isabella.txt` says "Some textures are based off the work of Mojang". The CC BY 3.0 grant covers only Bonemouse's rights; this is an upstream residual question.
- `assets/pointer.png` and `packs/beta-base/pointer.png`: Homebrew Channel, GPL v2, credited in `NOTICE`.

## What ships and what never does

| Rule | Detail |
|---|---|
| Default pack | Earth & Ash, CC0 (`assets/earth-and-ash-manifest.json`, 57 files, licence text `assets/CC0-1.0.txt`) |
| Optional pack | Isabella II (CC BY 3.0, Bonemouse), `texturepacks/isabella-ii/` only. No Isabella II file is in `assets/` |
| Never shipped | Mojang/Beta textures and sounds; owned oracle JARs, decompiles, `beta_pack`, `.minecraft` (denied by `clean_export.py`; no package paths) |
| User-owned packs | Imported by the Pack Helper (`tools/import_owned_beta_assets.py`) into `texturepacks/beta-1.7.3` on the player's card. Local ownership is not redistribution permission |
| Tutorial world | `saves/CavEX Tutorial` is untracked and is not in the export or zip. It needs its own record if it ever ships |
| Music | No music files ship in `assets/`. Earth & Ash music candidates live only in the private `artwork/` tree |

## Inventory by group

| Group | Files | Source and author | Licence | Attribution needed and where | Status |
|---|---|---|---|---|---|
| ea-default (textures, mobs, armor, GUI sheets, font, anim, arrows, paintings, weather) | 56 | CaveWii "Earth & Ash", original scripted pixel art. `assets/terrain.png` bed cells 134/135/149-152 redrawn 2026-10-30 by the project (`artwork/cc0-pack/full_terrain.py` `bed()`), project-own; follows Beta's bed tile layout only, no Mojang pixels | CC0-1.0 | none; `assets/README.md` | ok |
| controls.png | 1 | `tools/gen_cc0_controls.py`; GameCube cells by Sol | CC0-1.0 | none | ok |
| approved-controls.png | 1 | WiiBrew users (cells 0-31, except project-own blank Classic keycaps 14, 15, 22, 23) | not stated on wiibrew.org | credited in `NOTICE` and here (maintainer 2026-10-22, 2026-10-30) | credited-licence-unstated |
| controller-pictures.png | 1 | CaveWii team drawings, 2026-10-29 | GPL-3.0-or-later | none | ok |
| icon.png (CAVEX logo) | 1 | CaveWii project original | GPL-3.0-or-later | none | ok |
| tools/pack_helper/art (computer.png/.ico app icon, logo.png) | 3 | CaveWii project: icon derived from icon.png (CAVEX wordmark), no Mojang art | GPL-3.0-or-later | none | ok |
| title-logo, reception | 2 | CaveWii originals (`*.provenance.json`) | project original (GPL repo) | none | ok |
| menu-background | 1 | Earth & Ash valley illustration | CC0-1.0 | none | ok |
| misc colormaps (grass, foliage) | 2 | `tools/gen_cc0_colormaps.py` | CC0-1.0 | none | ok |
| sound-cc0 (placeholder effects) | 60 | procedural tones/noise | CC0-1.0 | none | ok |
| sound-menu (8 cues incl. Sync Spark, plus `achievement.wav`) | 9 | original synthesis, maintainer-approved | GPL-3.0-or-later | none | ok |
| startup-credits art and font | 2 | BagHeads card (CC BY-ND 4.0); CC0 font copy | CC-BY-ND-4.0; CC0-1.0 | yes, `NOTICE` (BagHeads) | ok |
| isabella-ii (images + adapted `anim.png`) | 14 | Bonemouse, "Isabella II" | CC BY 3.0 | yes (below) | ok |
| isabella-extras (`pointer.png`, `font/default.png`) | 2 | CaveWii originals | CC0-1.0 | none | ok |
| Homebrew Channel pointer | 1 (2 paths) | Homebrew Channel (fail0verflow) | GPL-2.0 | licence text; `NOTICE`, `LICENSES/GPL-2.0.txt` | ok |
| beta-base anim | 1 (2 paths) | CaveWii generator `tools/gen_anim_fx/bake_anim_fx.sh` | GPL-3.0-or-later | none | ok |
| tests-fixtures (synthetic ogg) | 4 | ffmpeg sine/noise | CC0-1.0 | none | ok |
| startup-credits fixtures | 3 | CaveWii renders of the BagHeads boot cards | GPL-3.0-or-later | BagHeads name: `NOTICE` | ok |
| achievement cue fixture | 1 | CaveWii original synthesis (Warm bell) | GPL-3.0-or-later | none | ok |

## Isabella II attribution (CC BY 3.0)

"Isabella II" texture pack by **Bonemouse**, licensed under the Creative Commons Attribution 3.0
Unported License (CC BY 3.0), http://creativecommons.org/licenses/by/3.0/ . Forum thread (dead
link, verified 2026-09-02): http://www.minecraftforum.net/topic/242175-Isabella/ .
Extracted from "Isabella 2 1.2vb1.zip". The pack's credits also name frejid for the original menu
background (not shipped).

**Change notice:** the CaveWii_v2 copies are modified. Files were edited and re-arranged for
CaveWii_v2, and `anim.png` is an adaptation (Beta 1.7.3 animation motion in Isabella II tile
colours, with Isabella II's own ice tile). They are not the original Isabella II files.

Covered files, all under `packs/isabella-ii/` and released as `texturepacks/isabella-ii/`:
`terrain.png`, `particles.png`, `gui/items.png`, `terrain/sun.png`, `terrain/moon.png`,
`environment/clouds.png`, `gui/crafting.png`, `gui/furnace.png`, `gui/gui.png`, `gui/icons.png`,
`gui/inventory.png`, `anim.png`. `pointer.png` and `font/default.png` in that folder are CC0
CaveWii originals, not Isabella II.

Where the credit appears: `LICENSES/Isabella-II/LICENSE.txt` and unedited `About Isabella.txt`
(repository root and release zip), `packs/isabella-ii/` (`README.txt`, `About Isabella.txt`,
`LICENSE.txt`, shipped in the zip), `NOTICE`, `README.md` Credits, `assets/README.md`.

## Other credits

- Earth & Ash: CC0 1.0 Universal, `assets/CC0-1.0.txt`. Credit is not required.
- GameCube glyphs by Zacksly (CC BY 3.0) exist only in private `artwork/` proofs, which are not exported. Production cells 32-41 are Sol's CC0 style C.
- Dependency licences are in `DEPENDENCIES.md`.
