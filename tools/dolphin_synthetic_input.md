# Driving CaveWii in Dolphin with synthetic input

Verified 2026-06-18. Goal: control CaveWii in the Dolphin emulator with no human at
the keyboard, and break/place/move verified by the on-screen F3 overlay + screenshots.

## TL;DR — what works and what doesn't

| Verb | Input (host key → emulated Wiimote) | Status via **computer-use MCP** |
|---|---|---|
| Walk / strafe | W/A/S/D → Wiimote **D-Pad** (ids 0-3) | ✅ works (position changes) |
| Jump | SPACE → Wiimote **B** (id 5) | ✅ works |
| Mine / break | J → **Nunchuk Z** (id 100) hold | ✅ works (block breaks, item drops) |
| Place | K → **Nunchuk C** (id 101) tap | ✅ works (block appears, hotbar count decrements) |
| Inventory / buttons | I → Wiimote **1**, etc. | ✅ works (inventory opens) |
| **Turn / look** | T/G/F/H → **Nunchuk analog stick** | ❌ **does not work** — see below |

**Bottom line:** move + break + place are fully drivable via synthetic input. The only
blocked verb is **look/turn**.

## The two hard findings (both verified exhaustively)

1. **Win32 `SendInput` from Claude's Bash/PowerShell tools does NOT reach Dolphin.**
   The OS registers the keys (`GetAsyncKeyState` flips true) but Dolphin's emulated
   controller and even Dolphin's own hotkeys never respond — focus, `BackgroundInput=True`,
   `AttachThreadInput`, click-to-activate, `dangerouslyDisableSandbox`, and a separate
   `Start-Process` child all failed. Shell-tree injected input goes to a virtualized
   queue Dolphin doesn't read.
   → **Use the computer-use MCP** (separate process, reaches the real input queue).
   Dolphin is a portable exe, so create a Start-menu shortcut
   (`%APPDATA%\…\Start Menu\Programs\Dolphin.lnk` → `Dolphin.exe`) for `request_access`
   to resolve it, then approve the access dialog (must be done with the user present).

2. **The emulated Nunchuk ANALOG STICK won't deflect from synthetic input** — neither a
   digital key bound to `Nunchuk/Stick/*` nor an absolute mouse cursor bound to it moves
   the look (`ry` stays exactly 90.0° = level). Dolphin's emulated Nunchuk *buttons*
   (Z/C = mine/place) and the *base Wiimote* (D-Pad/buttons) DO respond — only the analog
   stick axis is the problem. Confirmed NOT a CaveWii bug: `capture_input` is `true`
   in-world (`screen_ingame.c:52`; only GUI screens clear it), so the look path
   (`camera.c:211`) is reached — the stick value arriving from Dolphin is just ~0.

## Repeatable setup

- **Dolphin profile** (`<user>\Config\WiimoteNew.ini`, `[Wiimote1]` = `DInput/0/Keyboard Mouse`):
  clean 1:1 mapping — `W/A/S/D`→D-Pad, `SPACE`→B, `J`→Nunchuk Z (mine), `K`→Nunchuk C
  (place), `C/V`→`-`/`+` (scroll), `I`→`1` (inventory), `P`→`2` (in-game shot), `E`→A,
  `ESCAPE`→Home, mouse→IR (menus). `Nunchuk/Stick`→`T/G/F/H` (intended look binding; see
  finding #2). Removed the old `WiimoteProfile1` override so the inline block is authoritative.
- **Dolphin.ini**: `[Input]`/`[Interface] BackgroundInput = True`.
- **SD config.json**: `debug.piston_demo:off`, `debug.autopilot:off`, `superflat:on`,
  `give:on`, `overlay:on`, Beta textures, `autoload:"+new"`. (All originals backed up `*.orig.bak`.)
- **Launcher / capture**: `tools/dolphin_drive.ps1` (`launch`/`kill`/`shot`); Dolphin renders
  fullscreen on DISPLAY2 (x 1920-3840). F3 overlay = ground truth: position `(x,y,z)` then
  look `(rx,ry°)`, `ry=90`=level, larger=look down.

## Driving recipe (computer-use)

1. `tools\dolphin_drive.ps1 -Action launch`; wait ~25 s; `request_access(["Dolphin"])`.
2. `switch_display` to the Dolphin monitor (`LS24D300G`). Center crosshair ≈ image (728,408).
3. Move: `hold_key "w"` (duration N). Mine: `hold_key "j"` (hold through dig delay). Place:
   `key "k"` (single tap; edge-triggered). Inventory: `key "i"`.
4. Verify each by F3 (position/`ry`) and the hotbar count (place decrements 64→63).
5. **Aiming without look:** walk the player INTO a solid wall so the fixed center crosshair
   lands on a block face (hint flips "Z Punch"→"Z Mine", "C Use"→"C Place"); then mine/place.
   Proven: walked into a stone cliff → placed a block (64→63) → mined it (block gone + drop).

## Look + place: SOLVED via autopilot "aim" mode (built into the .dol)

Dolphin won't deflect the emulated analog stick from synthetic input, so look is authored in
code instead — `debug.autopilot:"aim"` (lever B). Config keys (in the SD-**root** config):

| Key | Effect |
|---|---|
| `debug.autopilot` | `"aim"` (or `"look"`) = hold an exact authored look, force no buttons; `"walk"`/`"on"` = the old survey rig; `"off"` = normal |
| `debug.autopilot_pitch_deg` | camera pitch, degrees. **90 = level, larger = look down** (default 130 ≈ 40° down → crosshair on ground ~2 blocks ahead) |
| `debug.autopilot_yaw_deg` | camera yaw, degrees (0 = face +Z) |
| `debug.autopilot_place` | `"on"` = the GAME injects the block-place itself — **no input at all** |
| `debug.autopilot_place_period` | ticks between auto-places; **0 = one single place** ~2 s after spawn; `>0` = one every N ticks |

In aim mode the camera is locked to the authored look (`camera.rx/ry`); computer-use still
drives walk/mine/place normally (aim mode forces no buttons). With `autopilot_place:"on"` the
place needs **zero input** — immune to Dolphin's flaky emulated Nunchuk and to window-focus
churn. **Verified:** with `superflat:on` it placed a chest on the flat ground with no input
(`tmp/shots/drive/bulletproof_full.png`, F3 look `(0.0,130.0)`).

Code: `source/game/autopilot.c|.h`, `main.c`, `game_state.h`, `game/gui/screen_ingame.c`
(autopilot raises a 0/1-latched `pending_place`; `autopilot_take_place_pulse()` is ORed into the
block-place edge in `screen_ingame.c`). A 3-agent adversarial review fixed a periodic-burst, a
guard, and a short-circuit bug before the build. **For a piston/redstone showcase:** aim down
(`pitch_deg`), scroll the give kit to the piston, set `autopilot_place:"on"` with a `period`, and
optionally drive movement with computer-use to lay a row — the placements fire deterministically.

### Earlier fallback (still valid): aim without code
Walk the player INTO a solid wall so the fixed center crosshair lands on a block face
("Z Punch"→"Z Mine"); then mine/place via computer-use. Covers wall placement but not the ground.
