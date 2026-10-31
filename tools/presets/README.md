# Control presets

Alternative, complete control schemes you can copy into a card's config. They
are **documented presets, not options in the menus** (Astra ruling 3 D4 for
0.1.0): nothing in the game switches between them, so what you copy is exactly
what you get.

**Unconfirmed on hardware by a human.** Every preset here is checked by
`tests/test_controls_sheet.py` for coherence, and none of them has been played
at a console yet. Treat them as candidates.

---

## `config_wii_developer.json` — the F3 debug overlay on a controller

**This is the configuration the maintainer card and the hardware rig use.**

The shipped `config_wii.json` leaves `input.debug_overlay` as an **explicit
empty array**: no controller chord summons the F3 overlay during ordinary play
(Astra ergonomics study 2026-09-19, recommendation 2, accepted by the
maintainer — "developer tools must not compete with play"). The empty array is
deliberate rather than an omitted key, so nothing can restore a default behind
your back.

This preset is that same file with **only that one key put back**:

| Controller | F3 |
| --- | --- |
| Wii Remote + Nunchuk | **hold Minus, then press Plus** (that order; the other order does nothing) |
| Classic Controller | **Plus + Minus** together, either order |
| GameCube | **L + R** together, either order |

Everything else — every binding, every mode — is byte-identical to the shipped
file, and `tests/test_controls_sheet.py` fails if the two ever drift apart in
any other key.

Two consequences worth knowing before you install it:

* on the Wii Remote, Minus and Plus are also the hotbar. With the chord back,
  the hotbar returns to its **release-tap** rule (step on the release of a tap
  under 500 ms; any overlap of the two cancels both) instead of the ordinary
  press-edge step the shipped config gives you;
* on the GameCube pad, L and R are place and break, so holding both to toggle
  F3 is also a place+break — the collision that took the chord out of the
  release config in the first place.

The `mp_chat_chord_1m` hardware profile REQUIRES this preset: staged with the
release config it now refuses to start at `p0` and says so by name.

---

## `config_wii_pointer_look.json` — aim with the pointer, walk with the stick

The shipped scheme (`config_wii.json`, described in `docs/controls_sheet.md`)
is the original CavEX one: you walk with the D-pad and aim with the Nunchuk
stick. This preset is the other way round — **you walk with the Nunchuk stick
and aim by pointing the Wii Remote at the screen**, the way the Legacy Console
Edition pointer feels.

### How to use it

1. Open the preset file and the card's config side by side. The card's config
   is `sd:/config.json` — the one in the **SD card root**, not the copy in
   `sd:/apps/CavEX/`, which the game never reads.
2. Copy the whole `"input"` block **and** the whole `"controls"` block over
   the ones already in `config.json`. Leave everything else (`paths`, `lod`,
   `audio`, …) exactly as it is.
3. Save, put the card back, and start the game.

**Copy both blocks, always — never the `"controls"` flag on its own.** The
flag `wiimote_ir_look` decides what the Nunchuk stick *is*: with it at `0` the
stick is the camera, with it at `1` the camera is the pointer and the stick is
free to walk. Set the flag without the bindings and you can point *and* still
walk with the D-pad, which is not what the preset was written for; copy the
bindings without the flag and the Nunchuk stick both walks **and** steers the
camera at the same time — the exact hybrid the live card shipped with, and the
reason "the controls feel wrong" was reported on 2026-09-16. The test refuses
either half on its own from now on.

### What is different from the shipped scheme

Only the Wii Remote + Nunchuk changes. **The Classic Controller and the
GameCube pad are byte-for-byte the shipped bindings**, so a second player on a
different pad notices nothing.

| | Shipped (`config_wii.json`) | This preset |
|---|---|---|
| Walk | D-pad | **Nunchuk stick** |
| Aim | Nunchuk stick | **point the Wii Remote at the screen** |
| Jump | B trigger | **A** |
| Break / attack | Nunchuk Z | Nunchuk Z (same) |
| Place / use | Nunchuk C | **B trigger** |
| Sneak | Button 1, a **toggle** | **Nunchuk C, held** (the faithful hold) |
| Hotbar | tap Minus / tap Plus | **D-pad Left / Right**, one step per press |
| Throw the held item | *not possible in gameplay* | **D-pad Down** |
| Open / close inventory | A opens, 1 closes | **1 does both** |
| Chat | tap Button 2 | tap Button 2 (same) — and a **held Button 2 recentres your aim** |
| F3 overlay | hold Minus, then press Plus | **Minus + Plus, in either order** |
| Pause | Home | Home (same) |

Two consequences worth knowing before you play:

- **A held Button 2 recentres the pointer.** If your aim has drifted off
  centre, hold 2 and it snaps back. A quick tap (under half a second) opens
  chat instead — the same tap window as the shipped scheme.
- **The F3 chord loses its order rule.** In the shipped scheme Minus and Plus
  are the hotbar, so the overlay needs *hold Minus, then press Plus* and the
  hotbar waits for a clean release. Here the hotbar has moved to the D-pad, so
  Minus and Plus do nothing else in the world and the chord works in either
  order, like the Classic and GameCube ones.

### Going back

Copy the `"input"` and `"controls"` blocks out of `config_wii.json` (the
repository root) over your `config.json` again — or just delete both blocks:
every key has a documented fallback, listed in
`docs/controls_sheet.md` § "If your config is older than this sheet".
