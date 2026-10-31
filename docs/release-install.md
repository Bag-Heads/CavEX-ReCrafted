# Installing CaveWii from the release zip (clean SD card)

This page is for players and for whoever proves release row R1. The zip is
built by `tools/make_release.sh` (the only packager) and checked by
`tests/test_make_release.py`. Nothing on this page is proven on hardware by
that test. Hardware results go in the clean-SD receipt
(`tmp/jobs/release-plan/REL-01b/CHECKLIST.md`), and a PC test never stands in
for them.

## What you need
- A Wii with the Homebrew Channel.
- An SD card (FAT32). For the release proof, use an empty card.
- A Wii Remote with Nunchuk. A Classic Controller and a GameCube controller
  also work.
- The release zip: `CaveWii_<version>-<commit>-PERFORMANCE-VERIFIED.zip`.
  A zip whose name ends in `-UNCERTIFIED` has no real-Wii proof behind it.
  Treat it as a test build.

## 1. Check the download
Unzip somewhere on your PC first. Then run this inside the unzipped folder:

    sha256sum -c SHA256SUMS

Every line must say `OK`. `SHA256SUMS` lists every file in the zip except
itself. `provenance.json` records the commit, the `boot.dol` sha256/md5, its
build stamp (`dol.embedded_build_ids`), the gate results and the real-Wii run
that certified it.

## 2. Copy to the card
Copy everything in the zip onto the ROOT of the SD card, keeping the folders.
The card should then look like this:

    sd:/apps/CavEX/boot.dol          the game
    sd:/apps/CavEX/meta.xml          Homebrew Channel name and version
    sd:/apps/CavEX/icon.png          Homebrew Channel icon
    sd:/apps/CavEX/config.json       your settings
    sd:/apps/CavEX/config_wii.json   default settings (fallback)
    sd:/apps/CavEX/assets/           default art and sound
    sd:/apps/CavEX/saves/CaveWii Tutorial/  the tutorial world
    sd:/config.json                       same settings at the card root
    sd:/config_wii.json                   same default settings at the card root
    sd:/assets/                           same art and sound at the card root
    sd:/saves/CaveWii Tutorial/           same tutorial world at the card root
    sd:/texturepacks/README.txt           how to add your own Beta 1.7.3 pack
    sd:/texturepacks/isabella-ii/         optional Isabella II pack (CC BY 3.0)
    sd:/LICENSE, NOTICE, LICENSES/        licences and credits
    sd:/README-RELEASE.txt, SHA256SUMS, provenance.json

The settings and art are on the card twice on purpose. A Homebrew Channel
launch can start the game with either the card root or the app folder as its
working folder, so both places carry a full set. Worlds are saved under
`saves/` (the `paths.worlds` setting), and the tutorial world is in both
`saves/` folders for the same reason.

## 3. Boot
Start the Homebrew Channel and pick **CavEX: Re-Crafted Edition**. The title
screen should appear with the Earth & Ash default textures.

## 3a. The tutorial world
The zip includes a ready-made tutorial world, **Tutorial Island** (folder
`saves/CaveWii Tutorial/`). To start it: press **Tutorial** on the title
screen. It is not shown in the Play game > Load list. You land on
the beach; follow the numbered gates along the trail. Signs explain each
stop and chests hold what you need: controls, crafting, tools, mining,
smelting, farming and animals, beds, minecarts, redstone and monsters.

Unzipping a newer release over an old one replaces `saves/CaveWii Tutorial/`
with a fresh copy. To keep your progress there, copy that folder somewhere
else first (or rename it). Your other worlds are not touched.

The world is built at release time by `tools/make_release.sh` from
`tools/tutorial_world/build_tutorial.py` (never committed; `saves/` is
gitignored). The packager runs `tests/test_tutorial_build.py` first and
writes no zip if that test or the build fails.

## 4. If your settings file is lost or broken
The game reads `config.json`. If that file is missing, empty or not valid
JSON, the game loads `config_wii.json` next to it instead and still boots
with the default Wii controls (REL-01a; `config_load_user_or_default` in
`source/config.c`). To reset your settings by hand, copy `config_wii.json`
over `config.json`. Do not delete `config_wii.json`: it is the fallback.

## 5. Controllers
- **Wii Remote + Nunchuk** (main scheme): D-pad moves, Nunchuk stick looks.
- **Classic Controller** and **GameCube controller**: full schemes, see
  `docs/wii_controller_playability.md`.
- If a controller disconnects (batteries, out of range, Nunchuk unplugged),
  the game pauses and waits. When it comes back, let go of every control,
  then press a button to resume. A button held across the reconnect does not
  resume the game. In two-player split-screen, a new controller takes over a
  player whose controller is gone.
- Every screen shows one button-hint row at the bottom. Classic L/R/ZL/ZR
  keycaps are lettered in the pack's font; PC builds show keyboard and mouse
  key caps. A new controller shows a notice card; a lost one shows a
  disconnect card. GameCube pad hot-plug is fixed.
- Coordinates and FPS show in game. Set `ui.show_coordinates` or
  `ui.show_fps` to false in `config.json` to hide them (default on; hidden
  while F3 is open).

## 6. Your own Beta 1.7.3 look and sound (optional)
No Minecraft files ship in this zip. If you own Beta 1.7.3 you can build a pack
from your own `b1.7.3.jar` (textures; in `.minecraft/versions/b1.7.3/`) and
your `.minecraft/resources` folder (sounds and the C418 music, which streams
from the SD card; the launcher creates it the first time you start Beta 1.7.3).
Choosing `.minecraft` itself also works: the helper finds `resources` inside it. The pack is made
on your PC and written to your own SD card; nothing is downloaded.

- **Import Beta 1.7.3 on the Wii** (no PC needed):
  1. Copy your own `b1.7.3.jar` to `sd:/beta/b1.7.3.jar`.
  2. Optional, for sounds: copy these from your `.minecraft/resources` to
     `sd:/beta/resources/`: the folders `sound`, `newsound`, `music` and
     `newmusic`, plus `streaming/13.ogg` and `cat.ogg`.
  3. In the game, open **Texture Packs** and choose **Import Beta 1.7.3**.
     Wait for **Done**, then pick the "Beta 1.7.3" pack.
- **Alternative: the CavEX Asset Importer** (the Pack Helper, Windows;
  `docs/pack_helper.md`). It is a separate
  desktop program, not part of this SD-card zip (a combined download carries it
  in a `Pack Helper` folder). Three steps on one screen: choose your jar (it finds the
  sounds itself; or pick `.minecraft/resources`), choose your SD card, click **Import assets**. It converts
  the files, checks the copy, and names the pack "Beta 1.7.3" and sets `paths.texturepack` and
  `paths.audiopack` for your next start in both `sd:/apps/CavEX/config.json`
  (read by a Homebrew Channel launch) and `sd:/config.json`. A damaged, modded
  or 256x jar/pack is refused with the file's name; nothing is installed.
- **Alternative: the command line** (needs the source checkout and Python).
  `python tools/import_owned_beta_assets.py --jar <your minecraft.jar>
  --resources <your resources folder> --sd <card>` (add `--dry-run` first). It
  writes `texturepacks/beta-1.7.3/` with the Pack Helper's own conversion, so
  the textures are byte-identical to the helper's for the same jar. It does not
  change your settings: select the pack in game. See
  `docs/import_owned_assets.md`. (`tools/make_beta_pack.ps1` is a developer
  tool for building the repo's test pack, not a player route.)
- **Alternative: by hand.** This route has no sounds unless you add them.
  Copy your own pack into `texturepacks/beta-1.7.3/`
  and set `"paths": { "texturepack": "texturepacks/beta-1.7.3" }` in
  `sd:/config.json` (and `sd:/apps/CavEX/config.json`).

Pick a pack in game on the **Texture Packs** screen (it lists `isabella-ii`
and anything else in `texturepacks/`); the change applies at once, sounds and
music included. A pack may be incomplete: each file it lacks falls back, file by
file, to the bundled Earth & Ash art and sound. The C418 music and jukebox
records have no bundled version, so without them those stay silent. The
shipped `sd:/texturepacks/README.txt` has the same steps.

Your Beta files give 78 of the game's 80 sound keys. The other two,
`minecart.base` and `minecart.inside` (minecart rolling and riding), do not
exist in Beta 1.7.3 at all: Beta's minecart is silent. They stay silent unless
the command line's `--modern-minecraft` option imports them from a newer
Minecraft install (`docs/deviations.md`, "Minecart rolling sound"). If no
sounds are found where you pointed, the helper says so in plain words and
still imports the textures.

## 7. The F3 overlay (for proof shots)
The release settings keep the F3 overlay off and have no button for it. For a
proof shot, set `"debug": { "overlay": "on" }` in `sd:/config.json` (and
`sd:/apps/CavEX/config.json`). Record that change in the receipt, and
put the shipped file back afterwards. Never edit `config_wii.json` for this:
the packager refuses a fallback config with the overlay forced on, or with
any capture-rig setting (`debug.autopilot`, `debug.auto_screenshot`).

## What the packager refuses
`tools/make_release.sh` writes no zip, and exits non-zero, if:
- the `.dol` is not named exactly `CaveWii_v2.dol` (`-stock`, `-dev`, `-ci`,
  `-REFUSED-*` and renamed copies are refused), or its build stamp ends in
  `-dev` (a dev build renamed to the release name);
- the `.elf` fails the KERNEL_HEAP (256 KB sidecar), TLS or test-marker gate;
- there is no real-Wii `PASS.json`, or it is not a PASS, is stability-only,
  is not a six-minute walk profile, was run with other LOD settings, or names
  a different build than the `.dol` (a stale or mismatched proof) -- unless
  `--allow-uncertified` is given, which stamps the zip `UNCERTIFIED`;
- the packaged commit lacks `LICENSE`, `icon.png`, `config_wii.json`, the
  Isabella II licence and About files, or the `packs/isabella-ii/` README and
  licence;
- `config_wii.json` is not a JSON object with a controller map, or carries
  capture-rig or forced-overlay debug settings;
- the packaged commit lacks `tools/tutorial_world/build_tutorial.py` or
  `tests/test_tutorial_build.py`, that test fails, or the build fails or
  writes no world.

Exporting the public repository is REL-18e. The final rerun on the one export
package is IN-014.
