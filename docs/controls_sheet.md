# CaveWii_v2 controls — all three controllers, on one page

**Revision 2 (2026-09-19).** The maintainer agreed revision 1 in full on
2026-09-19 ("I agree with all wiimote+nunchuck controls... Menus: agree with
all 3 sets... chat keyboard: same I agree with all binds") and then asked Astra
for an ergonomics study, whose recommendations they accepted with one
exception: *"All but 5, and we workshop 5 later."* What changed in revision 2,
and nothing else did:

1. **Classic sneak is Y, and it is a toggle** (it was Minus, held) — the right
   thumb owns the camera stick, so a held sneak costs you the camera at a ledge.
2. **GameCube sneak is still Z, but it is a toggle** — the right index finger
   owns R (break), so a held sneak needs a special grip to mine.
3. **The F3 debug overlay is gone from the controllers.** It lives in
   `tools/presets/config_wii_developer.json` now. Developer tools should not
   compete with play, and it is what freed the Remote's Minus/Plus and the
   GameCube's L+R.
4. **The GameCube hotbar is B / X** (it was D-pad Left / Right) and **chat is
   D-pad Up** (it was X) — so the left thumb can keep retreating while the
   right thumb selects.
5. **No controller has a gameplay throw any more** (Classic B and GameCube B
   lost theirs) — dropping is an inventory visit, on every pad.

*Recommendation 5 — swapping the Remote's sneak and inventory buttons (1 and
A) — is DEFERRED to a console workshop at the maintainer's request. The Remote
scheme on this page is unchanged.*

**Status: candidate controls, pending physical acceptance by the maintainer.**
Nothing on this page is accepted until somebody has played it at the console.
PC tests prove the rules are *right*; they cannot prove they *feel* right.

**This sheet describes `config_wii.json` as it stands at this commit** — the
file the Wii build ships with. If the card in your Wii has its own
`sd:/config.json`, that file wins: see *"If your config is older than this
sheet"* near the bottom.

The scheme itself, and why it was chosen, is
`docs/wii_controller_playability.md` **#24** (the Wii Remote layout) and
**#25** (the sneak toggle). Menus follow the approved
`docs/ui_design/control-revision-2026-09-10/CONTROLS_MATRIX.md`.
`tests/test_controls_sheet.py` re-reads the config, the fallbacks in
`source/platform/input.c` and the gesture headers on every build and fails if
this page and the game have drifted apart.

---

## 1. Playing — what the buttons do while you are in the world

| What you want to do | Wii Remote + Nunchuk | Classic Controller | GameCube |
|---|---|---|---|
| Walk around | **D-pad** (Up / Down / Left / Right) | left stick | control stick |
| Look around | **Nunchuk stick** | right stick | C-stick |
| Jump | **B** (the trigger under the Remote) | **A** | **A** |
| Break a block / attack | **Nunchuk Z** | **ZR** | **R** |
| Place a block / use | **Nunchuk C** | **ZL** | **L** |
| Open the inventory | **A** | **X** | **Y** |
| Sneak | **Button 1** — a **toggle**: tap on, tap off | **Y** — a **toggle**: tap on, tap off | **Z** — a **toggle**: tap on, tap off |
| Previous / next hotbar slot | **Minus / Plus** (an ordinary press) | **full L / full R** | **B / X** |
| Open chat (online games) | **tap Button 2** (a quick tap) | **D-pad Up** | **D-pad Up** |
| Debug (F3) overlay | *not bound* — install `tools/presets/config_wii_developer.json` | *not bound* | *not bound* |
| Pause | **Home** | **Home** | **Start** |
| Throw the held item away | *nothing in gameplay* — drop it from the inventory screen instead | *nothing in gameplay* | *nothing in gameplay* |
| Screenshot | *not bound on any Wii controller* | *not bound* | *not bound* |

Why no controller has a throw button: on the Remote, with your right thumb on
the D-pad, only B, Nunchuk Z and Nunchuk C can be reached *while walking*, and
jump, break and place take all three. Nothing is left for a throw — which also
means nothing valuable is ever thrown by accident. Revision 2 extends the same
judgement to the other two pads: an occasional destructive action does not
deserve a prominent button beside jump. Drop from the inventory screen on every
controller. (B is still take-half / backspace in menus everywhere.) **While you
are holding a stack the bottom hint row shows that pad's drop button next to
the word `Drop`** — Remote 2, Classic Minus, GameCube Z — so you never have to
remember where the discard lives.

## 2. Menus — inventory, chests, crafting, options, pause

| What you want to do | Wii Remote + Nunchuk | Classic Controller | GameCube |
|---|---|---|---|
| Move the selection | **D-pad**, or the Nunchuk stick | **D-pad** or the left stick | **D-pad** or the control stick |
| Reach the buttons under a LIST (Select World, Multiplayer) | **D-pad RIGHT** | **D-pad RIGHT** | **D-pad RIGHT** |
| Go back from those buttons to the list | **D-pad UP** | **D-pad UP** | **D-pad UP** |
| Pick up / place / confirm | **A** | **A** | **A** |
| Take half / place one | **B** | **B** | **B** |
| Quick-move a stack | **Nunchuk Z** | **Y** | **X** |
| Drop the stack you are holding | **Button 2** | **Minus** | **Z** |
| Previous container view | **Minus** | **full L** | **L** |
| Next container page | **Plus** | **full R** | **R** |
| Swap Inventory / Crafting (inventory and workbench; chests and furnaces have no page swap) | **tap Nunchuk C** | **tap ZL** | **brief C-stick left or right, then let it centre** |
| Close / Back / Cancel | **Button 1** | **X** | **Y** |
| Pause menu | **Home** | **Home** | **Start** |

Those two list rows are the only menu navigation the game has to TELL you
about, so the two list screens say it themselves: a one-line hint sits in the
gap between the list and its buttons -- "RIGHT: actions" while the ring is on
a row, "UP: list" while it is on the buttons -- and it hides while the IR
pointer is the thing choosing (Astra ruling 6, push-0921 A19).

On Multiplayer that gap has a second tenant: the red "Couldn't save
multiplayer settings" line sits in the same 16 px band, and it stays up until
a later save succeeds. **While that line is visible the hint is not drawn --
the failure line wins** (push-0921 A23, review A20-3). Select World has no
message in its band, so its hint is always the one there.

**Texture Packs shows no hint yet.** The honest reason is narrower than
"there is no gap": the band the other two screens use -- between the list and
the button row -- is 6 px on that screen, too small for the 16 px line, but
the screen does have a free footer below its single button row that the same
hint could use. Ruling 6 named three screens, so leaving one unhinted is a
deviation the lead takes back to Astra at gate 3 rather than a settled design;
until it is ruled, nothing is drawn there (asserted from the render, not just
from the absence of a call site). Its only enabled button, Done, is the Close
button as well.

On the Classic and the GameCube pad the same physical button opens *and*
closes the inventory (X, Y). On the Wii Remote **A opens and 1 closes** —
because the approved menu design needs A for *Select* inside the window, and
one button could not be both without picking a slot on the way out.

**In the pause menu that same Close button means Back / Resume** — it puts you
straight back into the world, exactly like Home / Start, on all three
controllers and in split-screen. *Save and quit* (or *Leave game* online) only
happens when you move to that row and press **A**; there is no shortcut for it
and there never will be one by accident. (Maintainer decision, 2026-09-19: the
pause menu used to quit on Close, which was the one place in the game where
Close did not mean Back.)

## 3. The chat keyboard (online games)

| What you want to do | Wii Remote + Nunchuk | Classic Controller | GameCube |
|---|---|---|---|
| Move around the keys | **D-pad**, or the Nunchuk stick | **D-pad** or the left stick | **D-pad** or the control stick |
| Type the highlighted key | **A** | **A** | **A** |
| Backspace | **B** | **B** | **B** |
| Previous / next key page | **Minus** / **Plus** | **full L** / **full R** | **L** / **R** |
| Send the message | **Nunchuk Z** | **Y** | **X** |
| Cancel and close | **Button 1** | **X** | **Y** |

There are three key pages — **Letters**, **Numbers**, **Symbols** — and the
two buttons above step backwards and forwards through them. They are *not* a
Shift: **capitals are their own key** on the utility row, beside Space and
Backspace (`widget_osk.c`), and the Letters page remembers which case you left
it in.

The keyboard is also pointer-operated: point at a key with the Wii Remote and
press A.

---

## 4. Rules you can feel

- **Chat is a TAP on the Wii Remote, a press everywhere else.** On the Remote
  the composer opens when you *let go* of Button 2 within **500 ms**; hold it
  longer and nothing opens, because a held 2 is the "recentre my aim" hold. On
  the Classic it is **D-pad Up**, and on the GameCube pad **D-pad Up** too —
  both plain presses. (Revision 2 item 4 moved the GameCube opener off X; X is
  the next hotbar slot now.) *Why the Classic uses the D-pad:* when the opener
  was chosen, sneak there was **Minus held** and the developer overlay chord
  was **Plus + Minus**, so an opener on Plus could not be pressed while you
  were sneaking. (Revision 2 has since made Classic sneak a **Y toggle** and
  moved the overlay chord into the developer preset; the D-pad opener stays,
  because Plus is still half of that chord for anyone who installs the
  preset.) The Classic D-pad is free in the world
  (you walk with the left stick and cycle the hotbar with full L/R), so Up
  costs nothing. Up is refused while you are holding **Left or Right** on that
  same D-pad, so rocking the pad never opens the composer by accident; let go
  and press Up again. (A config that names Plus explicitly still gets the old
  release-and-cancel behaviour.)
- **The on-screen chat prompt shows your button and names the gesture.** In an
  online game the HUD carries a small framed *socket* tucked against the right
  end of your hotbar, holding *your* pad's own binding as one full-size button
  glyph, with the word **Chat** beside it — and **Tap** above that word for any
  opener that opens when you let go (the Wii Remote's **2**, and an explicitly
  configured Classic Plus). The Classic and the GameCube open chat with **D-pad
  Up**, and the socket draws the *Up arrow* itself, so nothing has to spell the
  direction out in words any more. It stays up for the whole online session,
  not just until you first open chat.
- **The Remote's hotbar is an ordinary press.** Tap Minus or Plus and the slot
  moves immediately — one press, one slot, no hold-to-repeat and no waiting for
  the release. It works this way *because* revision 2 took the F3 chord off
  those two buttons; nothing else shares them any more. Classic full L/R and
  GameCube B/X are the same plain press.
- **F3 is not on any controller.** The debug overlay left the release
  configuration in revision 2 — developer tools should not compete with play.
  Install `tools/presets/config_wii_developer.json` (the shipped file with that
  one key put back) and it returns: **hold Minus, then press Plus** on the
  Remote, **Plus + Minus** on the Classic, **L + R** on the GameCube pad. With
  that preset installed the Remote's hotbar goes back to the release-tap rule
  (a step on the release of a tap ≤ 500 ms, any overlap cancelling both), and
  on the GameCube pad L + R is also place + break — which is exactly why it is
  not in the release config.
- **Sneak on the Wii Remote is a toggle.** Tap 1 to crouch, tap 1 again to
  stand. It is cleared by dying or respawning, by leaving or changing worlds,
  by unplugging or swapping controllers, and by **getting into** a minecart, a
  boat or a bed — once, as you board. Once aboard you can tap 1 again and it
  stays on for the whole ride; getting out does not turn it off, and boarding
  again turns it off again. It is *kept* while you are in the inventory, in chat or on
  the pause menu, so opening a screen never quietly takes away your ledge
  protection — and the 1 that closed that screen does not toggle sneak on the
  way out: it has to be released first.
- **Sneak is a toggle on all three controllers** (revision 2). Classic **Y**
  and GameCube **Z** work exactly like Remote 1 above — same clears, same
  preservation through screens, same "let go before it counts again". Each pad
  has its own switch back to Beta's held sneak:
  `controls.wiimote_sneak_toggle`, `controls.classic_sneak_toggle`,
  `controls.gamecube_sneak_toggle` — set the one you want to `0`. Y is also
  Quick Move / sign Done / chat Send, and Z is also the menu stack drop, so a
  held sneak button never acts in the menu it walked into.
- **The chat socket tucks against the right end of your hotbar** (Astra
  placement PL-B, maintainer 2026-09-21): a 48×48 framed panel 8 px right of
  the ninth slot's selection frame, holding one full-size 32×32 button glyph,
  with `Tap` / `Chat` stacked beside it — or `Chat` alone, level with the
  socket, when the opener is a plain press. Because the socket draws the D-pad
  *direction's own* arrow, every binding is the same size and nothing about a
  remap can make the prompt grow. If chat is unbound entirely you get
  `Chat: menu` instead, and the pause menu's Chat row is the route.
- **A `SNEAK` rail can light up along the top of your hotbar while the toggle
  is on — but it is OFF until the maintainer approves it.** Turn it on with
  `"ui": {"sneak_toggle_indicator": true}`; leaving the key out, or setting it
  to `false`, means no rail. Either way the crouch, the camera dip and the
  ledge protection are unchanged — this key moves the indicator and nothing
  else. When it is on, a 4 px bar in your texture pack's own selection colour
  runs along the hotbar's top edge and the word `SNEAK` sits just left of the
  bar; the two appear and disappear together with the toggle, silently (no
  sound, no rumble). The selected slot's frame draws over the bar, so you can
  still see which slot you are holding. In split-screen only the player who is
  actually crouched gets one, on that player's own hotbar — there is no
  separate split placement any more. It hides under the pause menu, the
  inventory and every other screen, and while a bed message is on screen; it
  stays up during chat and F3.
- **A opens, 1 closes, and neither leaks.** The A press that opens the
  inventory cannot also pick up a slot in it, and the 1 press that closes a
  screen cannot reach gameplay. Both directions need the button to be let go
  first.
- **Legacy configs keep working.** A config written before these roles existed
  still plays: see the next section — every missing key has a defined answer,
  and a key a config *does* name is always obeyed.

## 5. Buttons that do two jobs

Some buttons carry more than one role. That is fine when the two can never be
live at once (gameplay vs. a menu screen), and deliberate where they can:

- **Wii Remote Button 2** = open chat (in the world) and drop the held stack
  (in a menu). Two different screens, never both.
- **GameCube X** = the next hotbar slot (in the world) and quick-move a stack
  (in a menu); **GameCube B** = the previous slot and take-half / backspace.
- **Classic Y** = sneak (in the world) and Quick Move / sign Done / chat Send
  (in a screen). Tap it to sneak and it cannot act in the window you then open;
  press it in the window and it cannot un-sneak you on the way out.
- **GameCube Z** = sneak (in the world) and the menu stack drop, on the same
  rule.
- **Classic and GameCube D-pad Up** = open chat (in the world) and move the
  selection (in a menu). Two different screens, never both.
- **Wii Remote Minus / Plus** = the hotbar and the menu paging — no F3 chord in
  the release config, so the hotbar is a plain press.
- Everywhere: **A is confirm in a menu**, and the **directional control moves
  the player in the world and the selection in a menu** — the D-pad on the
  Remote, the left stick or D-pad on the Classic, the control stick or D-pad on
  the GameCube. What A does in the *world* is NOT the same on all three: it
  opens the inventory on the Remote only, where the Classic uses X and the
  GameCube uses Y.

The machine block below lists **every** one of these, with the exact roles, so
a new double-binding cannot appear unnoticed.

## 6. If your config is older than this sheet

One sentence each — this is exactly what `input_read_bindings()` does:

- **A key your config names always wins.** Nothing in the game ever rewrites
  your config or overrides a binding you wrote down.
- **An empty array turns a role off** (`"screenshot": []` is how the shipped
  config disables screenshots).
- **`open_inventory` missing** falls back to *your own* `inventory` codes, so a
  config written before A opened the inventory still opens and closes it with
  the same button it always did.
- **`chat`, `gui_split`, `gui_drop`, `gui_page`, `gui_previous` and
  `gui_quick_move` missing** fall back to the built-in Wii defaults, which are
  the same codes this sheet lists.
- **`controls.wiimote_sneak_toggle`, `controls.classic_sneak_toggle` and
  `controls.gamecube_sneak_toggle` missing** each mean **toggle** on their own
  family (the shipped behaviour is also the fallback); an explicit `0` restores
  Beta's held sneak on the family it names and on no other.
- **`controls.wiimote_ir_look` missing** means **0** — the Nunchuk stick is the
  camera, which is what this sheet describes.

## 7. The pointer-look preset

If you would rather aim with the Wii Remote's pointer and walk with the
Nunchuk stick, `tools/presets/config_wii_pointer_look.json` is a complete,
tested alternative. It is a **documented preset, not an option in the menus**,
and both of its blocks must be copied together —
see `tools/presets/README.md`.

---

## Machine-readable binding block

The block below is what `tests/test_controls_sheet.py` reads. Every number in
it is checked against the effective bindings (`config_wii.json` plus the
fallbacks), the control modes and the gesture constants the game actually
compiles with — so if a binding changes and this page does not, the build
fails and names the controller and the role. The tables above say the same
thing in words; keep them together.

```json
{
 "describes": {
  "config": "config_wii.json",
  "scheme": "original CavEX Wii Remote layout (maintainer decision 2026-09-19)",
  "menus": "docs/ui_design/control-revision-2026-09-10/CONTROLS_MATRIX.md"
 },
 "status": "candidate controls, pending physical acceptance by the maintainer",
 "gesture_constants": {
  "chat_tap_ms": 500,
  "hotbar_tap_ms": 500,
  "hotbar_hitch_polls": 2
 },
 "families": {
  "wii_remote_nunchuk": {
   "modes": {
    "wiimote_ir_look": 0,
    "sneak": "toggle",
    "camera": "nunchuk_stick"
   },
   "unbound_by_design": {
    "player_drop": "No thumb button is free while walking, so nothing is thrown in gameplay; the held stack is dropped from the inventory screen (Button 2 there).",
    "screenshot": "No Wii controller binds a screenshot key; the shipped config disables the role with an empty array.",
    "player_drop": "No Wii controller has a gameplay Drop any more (Astra ergonomics 4 and 6, 2026-09-19): an occasional destructive action does not deserve a prominent button beside jump, and the Wii Remote never had one. The held stack is dropped from the inventory screen instead (gui_drop).",
    "debug_overlay": "The F3 debug overlay is a DEVELOPER tool and left the release configuration on 2026-09-19 (Astra ergonomics 2): input.debug_overlay is an explicit EMPTY array here, so no controller chord can summon it and nothing competes with the building controls. tools/presets/config_wii_developer.json binds it back."
   },
   "roles": {
    "player_forward": {"codes": [0], "button": "D-pad Up", "context": "gameplay"},
    "player_backward": {"codes": [1], "button": "D-pad Down", "context": "gameplay"},
    "player_left": {"codes": [2], "button": "D-pad Left", "context": "gameplay"},
    "player_right": {"codes": [3], "button": "D-pad Right", "context": "gameplay"},
    "player_jump": {"codes": [5], "button": "B trigger", "context": "gameplay"},
    "player_sneak": {"codes": [6], "button": "Button 1", "context": "gameplay"},
    "player_drop": {"codes": [], "button": "", "context": "gameplay"},
    "item_action_left": {"codes": [100], "button": "Nunchuk Z", "context": "gameplay"},
    "item_action_right": {"codes": [101], "button": "Nunchuk C", "context": "gameplay"},
    "scroll_left": {"codes": [9], "button": "Minus", "context": "gameplay"},
    "scroll_right": {"codes": [8], "button": "Plus", "context": "gameplay"},
    "open_inventory": {"codes": [4], "button": "A", "context": "gameplay"},
    "open_menu": {"codes": [10], "button": "Home", "context": "gameplay"},
    "chat": {"codes": [7], "button": "Button 2", "context": "gameplay",
     "gesture": {"kind": "tap_release", "opener_code": 7, "threshold_ms": 500}},
    "debug_overlay": {"codes": [], "button": "", "context": "gameplay"},
    "screenshot": {"codes": [], "button": "", "context": "gameplay"},
    "inventory": {"codes": [6], "button": "Button 1", "context": "menus"},
    "gui_up": {"codes": [0, 900], "button": "D-pad Up / Nunchuk stick up", "context": "menus"},
    "gui_down": {"codes": [1, 901], "button": "D-pad Down / Nunchuk stick down", "context": "menus"},
    "gui_left": {"codes": [2, 902], "button": "D-pad Left / Nunchuk stick left", "context": "menus"},
    "gui_right": {"codes": [3, 903], "button": "D-pad Right / Nunchuk stick right", "context": "menus"},
    "gui_click": {"codes": [4], "button": "A", "context": "menus"},
    "gui_click_alt": {"codes": [5], "button": "B trigger", "context": "menus"},
    "gui_split": {"codes": [5], "button": "B trigger", "context": "menus"},
    "gui_drop": {"codes": [7], "button": "Button 2", "context": "menus"},
    "gui_page": {"codes": [8], "button": "Plus", "context": "menus"},
    "gui_previous": {"codes": [9], "button": "Minus", "context": "menus"},
    "gui_page_swap": {"codes": [101], "button": "Nunchuk C", "context": "menus"},
    "gui_quick_move": {"codes": [100], "button": "Nunchuk Z", "context": "menus"}
   }
  },
  "classic_controller": {
   "modes": {
    "sneak": "toggle",
    "camera": "right_stick"
   },
   "unbound_by_design": {
    "screenshot": "No Wii controller binds a screenshot key; the shipped config disables the role with an empty array.",
    "player_drop": "No Wii controller has a gameplay Drop any more (Astra ergonomics 4 and 6, 2026-09-19): an occasional destructive action does not deserve a prominent button beside jump, and the Wii Remote never had one. The held stack is dropped from the inventory screen instead (gui_drop).",
    "debug_overlay": "The F3 debug overlay is a DEVELOPER tool and left the release configuration on 2026-09-19 (Astra ergonomics 2): input.debug_overlay is an explicit EMPTY array here, so no controller chord can summon it and nothing competes with the building controls. tools/presets/config_wii_developer.json binds it back."
   },
   "roles": {
    "player_forward": {"codes": [910], "button": "left stick up", "context": "gameplay"},
    "player_backward": {"codes": [911], "button": "left stick down", "context": "gameplay"},
    "player_left": {"codes": [912], "button": "left stick left", "context": "gameplay"},
    "player_right": {"codes": [913], "button": "left stick right", "context": "gameplay"},
    "player_jump": {"codes": [204], "button": "A", "context": "gameplay"},
    "player_sneak": {"codes": [207], "button": "Y", "context": "gameplay"},
    "player_drop": {"codes": [], "button": "", "context": "gameplay"},
    "item_action_left": {"codes": [209], "button": "ZR", "context": "gameplay"},
    "item_action_right": {"codes": [208], "button": "ZL", "context": "gameplay"},
    "scroll_left": {"codes": [210], "button": "full L", "context": "gameplay"},
    "scroll_right": {"codes": [211], "button": "full R", "context": "gameplay"},
    "open_inventory": {"codes": [206], "button": "X", "context": "gameplay"},
    "open_menu": {"codes": [214], "button": "Home", "context": "gameplay"},
    "chat": {"codes": [200], "button": "D-pad Up", "context": "gameplay"},
    "debug_overlay": {"codes": [], "button": "", "context": "gameplay"},
    "screenshot": {"codes": [], "button": "", "context": "gameplay"},
    "inventory": {"codes": [206], "button": "X", "context": "menus"},
    "gui_up": {"codes": [200, 910], "button": "D-pad Up / left stick up", "context": "menus"},
    "gui_down": {"codes": [201, 911], "button": "D-pad Down / left stick down", "context": "menus"},
    "gui_left": {"codes": [202, 912], "button": "D-pad Left / left stick left", "context": "menus"},
    "gui_right": {"codes": [203, 913], "button": "D-pad Right / left stick right", "context": "menus"},
    "gui_click": {"codes": [204], "button": "A", "context": "menus"},
    "gui_click_alt": {"codes": [205], "button": "B", "context": "menus"},
    "gui_split": {"codes": [205], "button": "B", "context": "menus"},
    "gui_drop": {"codes": [213], "button": "Minus", "context": "menus"},
    "gui_page": {"codes": [211], "button": "full R", "context": "menus"},
    "gui_previous": {"codes": [210], "button": "full L", "context": "menus"},
    "gui_page_swap": {"codes": [208], "button": "ZL", "context": "menus"},
    "gui_quick_move": {"codes": [207], "button": "Y", "context": "menus"}
   }
  },
  "gamecube": {
   "modes": {
    "sneak": "toggle",
    "camera": "c_stick"
   },
   "unbound_by_design": {
    "screenshot": "No Wii controller binds a screenshot key; the shipped config disables the role with an empty array.",
    "player_drop": "No Wii controller has a gameplay Drop any more (Astra ergonomics 4 and 6, 2026-09-19): an occasional destructive action does not deserve a prominent button beside jump, and the Wii Remote never had one. The held stack is dropped from the inventory screen instead (gui_drop).",
    "debug_overlay": "The F3 debug overlay is a DEVELOPER tool and left the release configuration on 2026-09-19 (Astra ergonomics 2): input.debug_overlay is an explicit EMPTY array here, so no controller chord can summon it and nothing competes with the building controls. tools/presets/config_wii_developer.json binds it back."
   },
   "roles": {
    "player_forward": {"codes": [930], "button": "control stick up", "context": "gameplay"},
    "player_backward": {"codes": [931], "button": "control stick down", "context": "gameplay"},
    "player_left": {"codes": [932], "button": "control stick left", "context": "gameplay"},
    "player_right": {"codes": [933], "button": "control stick right", "context": "gameplay"},
    "player_jump": {"codes": [404], "button": "A", "context": "gameplay"},
    "player_sneak": {"codes": [408], "button": "Z", "context": "gameplay"},
    "player_drop": {"codes": [], "button": "", "context": "gameplay"},
    "item_action_left": {"codes": [410], "button": "R trigger", "context": "gameplay"},
    "item_action_right": {"codes": [409], "button": "L trigger", "context": "gameplay"},
    "scroll_left": {"codes": [405], "button": "B", "context": "gameplay"},
    "scroll_right": {"codes": [406], "button": "X", "context": "gameplay"},
    "open_inventory": {"codes": [407], "button": "Y", "context": "gameplay"},
    "open_menu": {"codes": [411], "button": "Start", "context": "gameplay"},
    "chat": {"codes": [400], "button": "D-pad Up", "context": "gameplay"},
    "debug_overlay": {"codes": [], "button": "", "context": "gameplay"},
    "screenshot": {"codes": [], "button": "", "context": "gameplay"},
    "inventory": {"codes": [407], "button": "Y", "context": "menus"},
    "gui_up": {"codes": [400, 930], "button": "D-pad Up / control stick up", "context": "menus"},
    "gui_down": {"codes": [401, 931], "button": "D-pad Down / control stick down", "context": "menus"},
    "gui_left": {"codes": [402, 932], "button": "D-pad Left / control stick left", "context": "menus"},
    "gui_right": {"codes": [403, 933], "button": "D-pad Right / control stick right", "context": "menus"},
    "gui_click": {"codes": [404], "button": "A", "context": "menus"},
    "gui_click_alt": {"codes": [405], "button": "B", "context": "menus"},
    "gui_split": {"codes": [405], "button": "B", "context": "menus"},
    "gui_drop": {"codes": [408], "button": "Z", "context": "menus"},
    "gui_page": {"codes": [410], "button": "R trigger", "context": "menus"},
    "gui_previous": {"codes": [409], "button": "L trigger", "context": "menus"},
    "gui_page_swap": {"codes": [942, 943], "button": "C-stick left / C-stick right", "context": "menus"},
    "gui_quick_move": {"codes": [406], "button": "X", "context": "menus"}
   }
  }
 },
 "shared_codes": [
  {"family": "wii_remote_nunchuk", "code": 0, "roles": ["gui_up", "player_forward"], "why": "The D-pad walks in the world and moves the selection in a menu; the two screens are never live together."},
  {"family": "wii_remote_nunchuk", "code": 1, "roles": ["gui_down", "player_backward"], "why": "Same D-pad, world vs. menu."},
  {"family": "wii_remote_nunchuk", "code": 2, "roles": ["gui_left", "player_left"], "why": "Same D-pad, world vs. menu."},
  {"family": "wii_remote_nunchuk", "code": 3, "roles": ["gui_right", "player_right"], "why": "Same D-pad, world vs. menu."},
  {"family": "wii_remote_nunchuk", "code": 4, "roles": ["gui_click", "open_inventory"], "why": "A opens the inventory in the world and confirms inside it; the opening press is blocked until it is released, so it cannot also pick a slot."},
  {"family": "wii_remote_nunchuk", "code": 5, "roles": ["gui_click_alt", "gui_split", "player_jump"], "why": "B jumps in the world; in a menu it is take-half / backspace."},
  {"family": "wii_remote_nunchuk", "code": 6, "roles": ["inventory", "player_sneak"], "why": "1 toggles sneak in the world and is Close/Back in a screen; the in-screen suppression plus a neutral re-arm stops the closing press from toggling sneak."},
  {"family": "wii_remote_nunchuk", "code": 7, "roles": ["chat", "gui_drop"], "why": "DECLARED PAIR: 2 opens chat in the world (a tap) and drops the held stack in a menu; the composer's neutral re-arm stops the opening press from acting there."},
  {"family": "wii_remote_nunchuk", "code": 8, "roles": ["gui_page", "scroll_right"], "why": "Plus is the next hotbar slot in the world and the next container page in a menu. With the F3 chord out of the release config it is an ORDINARY PRESS again -- no tap window, no release delay."},
  {"family": "wii_remote_nunchuk", "code": 9, "roles": ["gui_previous", "scroll_left"], "why": "Minus is the previous hotbar slot in the world and the previous container view in a menu, on the same ordinary press edge."},
  {"family": "wii_remote_nunchuk", "code": 100, "roles": ["gui_quick_move", "item_action_left"], "why": "Nunchuk Z breaks blocks in the world and quick-moves a stack in a menu."},
  {"family": "wii_remote_nunchuk", "code": 101, "roles": ["gui_page_swap", "item_action_right"], "why": "DECLARED PAIR (design-round-1026e CONSENSUS, Fable YES 2026-10-26): Nunchuk C places blocks in the world and, in the inventory/workbench, a tap swaps between Inventory and Crafting; the two screens are never live together."},
  {"family": "classic_controller", "code": 205, "roles": ["gui_click_alt", "gui_split"], "why": "B is take-half / backspace in a menu and does NOTHING in gameplay: its throw left on 2026-09-19 (Astra ergonomics 6)."},
  {"family": "classic_controller", "code": 204, "roles": ["gui_click", "player_jump"], "why": "A jumps in the world and confirms in a menu."},
  {"family": "classic_controller", "code": 206, "roles": ["inventory", "open_inventory"], "why": "X both opens and closes the inventory on this pad, exactly as it always has."},
  {"family": "classic_controller", "code": 200, "roles": ["chat", "gui_up"], "why": "D-pad Up opens chat in the world and moves the selection in a menu; the two screens are never live together. It is the opener BECAUSE this pad's sneak is Minus HELD and Plus+Minus is the F3 chord, so an opener on Plus could not be pressed while sneaking (Astra ruling 4 D1)."},
  {"family": "classic_controller", "code": 210, "roles": ["gui_previous", "scroll_left"], "why": "Full L is the previous hotbar slot in the world and the previous container view in a menu."},
  {"family": "classic_controller", "code": 211, "roles": ["gui_page", "scroll_right"], "why": "Full R is the next hotbar slot in the world and the next container page in a menu."},
  {"family": "classic_controller", "code": 208, "roles": ["gui_page_swap", "item_action_right"], "why": "DECLARED PAIR (design-round-1026e CONSENSUS, Fable YES 2026-10-26): ZL places blocks in the world and, in the inventory/workbench, a tap swaps between Inventory and Crafting; the two screens are never live together."},
  {"family": "classic_controller", "code": 207, "roles": ["gui_quick_move", "player_sneak"], "why": "DECLARED PAIR: Y toggles sneak in the world; in a menu it is Quick Move, the sign editor's Done and the chat keyboard's Send. Every one of those reads a PRESS EDGE off the owner frame, so a Y held across a screen boundary acts on one side only and a fresh press is needed for the other."},
  {"family": "classic_controller", "code": 910, "roles": ["gui_up", "player_forward"], "why": "The left stick walks in the world and moves the selection in a menu."},
  {"family": "classic_controller", "code": 911, "roles": ["gui_down", "player_backward"], "why": "Same stick, world vs. menu."},
  {"family": "classic_controller", "code": 912, "roles": ["gui_left", "player_left"], "why": "Same stick, world vs. menu."},
  {"family": "classic_controller", "code": 913, "roles": ["gui_right", "player_right"], "why": "Same stick, world vs. menu."},
  {"family": "gamecube", "code": 400, "roles": ["chat", "gui_up"], "why": "DECLARED PAIR: D-pad Up opens chat in the world and moves the selection in a menu; the two screens are never live together, and the legacy Left/Right rock guard still refuses a press made while steering."},
  {"family": "gamecube", "code": 404, "roles": ["gui_click", "player_jump"], "why": "A jumps in the world and confirms in a menu."},
  {"family": "gamecube", "code": 405, "roles": ["gui_click_alt", "gui_split", "scroll_left"], "why": "DECLARED PAIR: B is the previous hotbar slot in the world (Astra ergonomics 4: the left thumb keeps retreating while the right thumb selects) and take-half / backspace in a menu. It no longer throws anything."},
  {"family": "gamecube", "code": 406, "roles": ["gui_quick_move", "scroll_right"], "why": "DECLARED PAIR: X is the next hotbar slot in the world and quick-moves a stack in a menu; both are press edges, so one held across a screen boundary acts on one side only."},
  {"family": "gamecube", "code": 407, "roles": ["inventory", "open_inventory"], "why": "Y both opens and closes the inventory on this pad, exactly as it always has."},
  {"family": "gamecube", "code": 408, "roles": ["gui_drop", "player_sneak"], "why": "DECLARED PAIR: Z toggles sneak in the world and drops the held stack in a menu; both read a press edge, so a held Z acts on one side only."},
  {"family": "gamecube", "code": 409, "roles": ["gui_previous", "item_action_right"], "why": "L places blocks in the world and is the previous container view in a menu. With the F3 chord out of the release config, L+R is just place+break -- the collision Astra ergonomics 2 removed."},
  {"family": "gamecube", "code": 410, "roles": ["gui_page", "item_action_left"], "why": "R breaks blocks in the world and is the next container page in a menu."},
  {"family": "gamecube", "code": 930, "roles": ["gui_up", "player_forward"], "why": "The control stick walks in the world and moves the selection in a menu."},
  {"family": "gamecube", "code": 931, "roles": ["gui_down", "player_backward"], "why": "Same stick, world vs. menu."},
  {"family": "gamecube", "code": 932, "roles": ["gui_left", "player_left"], "why": "Same stick, world vs. menu."},
  {"family": "gamecube", "code": 933, "roles": ["gui_right", "player_right"], "why": "Same stick, world vs. menu."}
 ],
 "presets": {
  "config_wii_pointer_look.json": {
   "unbound_by_design": {
    "wii_remote_nunchuk": {
     "screenshot": "No Wii controller binds a screenshot key; the preset keeps the empty array.",
    "debug_overlay": "The F3 debug overlay is a DEVELOPER tool and left the release configuration on 2026-09-19 (Astra ergonomics 2): input.debug_overlay is an explicit EMPTY array here, so no controller chord can summon it and nothing competes with the building controls. tools/presets/config_wii_developer.json binds it back."
    },
    "classic_controller": {
     "screenshot": "No Wii controller binds a screenshot key; the preset keeps the empty array.",
     "player_drop": "No Wii controller has a gameplay Drop any more (Astra ergonomics 4 and 6, 2026-09-19): an occasional destructive action does not deserve a prominent button beside jump, and the Wii Remote never had one. The held stack is dropped from the inventory screen instead (gui_drop).",
    "debug_overlay": "The F3 debug overlay is a DEVELOPER tool and left the release configuration on 2026-09-19 (Astra ergonomics 2): input.debug_overlay is an explicit EMPTY array here, so no controller chord can summon it and nothing competes with the building controls. tools/presets/config_wii_developer.json binds it back."
    },
    "gamecube": {
     "screenshot": "No Wii controller binds a screenshot key; the preset keeps the empty array.",
     "player_drop": "No Wii controller has a gameplay Drop any more (Astra ergonomics 4 and 6, 2026-09-19): an occasional destructive action does not deserve a prominent button beside jump, and the Wii Remote never had one. The held stack is dropped from the inventory screen instead (gui_drop).",
    "debug_overlay": "The F3 debug overlay is a DEVELOPER tool and left the release configuration on 2026-09-19 (Astra ergonomics 2): input.debug_overlay is an explicit EMPTY array here, so no controller chord can summon it and nothing competes with the building controls. tools/presets/config_wii_developer.json binds it back."
    }
   },
   "shared_codes": [
    {"family": "wii_remote_nunchuk", "code": 1, "roles": ["gui_down", "player_drop"], "why": "In this preset the D-pad is free of movement, so D-pad Down throws the held item; in a menu it still moves the selection."},
    {"family": "wii_remote_nunchuk", "code": 2, "roles": ["gui_left", "scroll_left"], "why": "D-pad Left is the previous hotbar slot in the world and moves the selection in a menu."},
    {"family": "wii_remote_nunchuk", "code": 3, "roles": ["gui_right", "scroll_right"], "why": "D-pad Right is the next hotbar slot in the world and moves the selection in a menu."},
    {"family": "wii_remote_nunchuk", "code": 4, "roles": ["gui_click", "player_jump"], "why": "A jumps in the world and confirms in a menu."},
    {"family": "wii_remote_nunchuk", "code": 5, "roles": ["gui_click_alt", "gui_split", "item_action_right"], "why": "B places blocks in the world; in a menu it is take-half / backspace."},
    {"family": "wii_remote_nunchuk", "code": 6, "roles": ["inventory", "open_inventory"], "why": "The preset omits open_inventory, so 1 both opens and closes the inventory."},
    {"family": "wii_remote_nunchuk", "code": 7, "roles": ["chat", "gui_drop"], "why": "DECLARED PAIR: a tap of 2 opens chat, a HELD 2 recentres the pointer look, and in a menu 2 drops the held stack."},
    {"family": "wii_remote_nunchuk", "code": 100, "roles": ["gui_quick_move", "item_action_left"], "why": "Nunchuk Z breaks blocks in the world and quick-moves a stack in a menu."},
    {"family": "wii_remote_nunchuk", "code": 101, "roles": ["gui_page_swap", "player_sneak"], "why": "DECLARED PAIR: in this preset Nunchuk C is sneak in the world; in the inventory/workbench a tap swaps between Inventory and Crafting (design-round-1026e CONSENSUS). The two screens are never live together."},
    {"family": "wii_remote_nunchuk", "code": 900, "roles": ["gui_up", "player_forward"], "why": "The Nunchuk stick walks in the world and moves the selection in a menu."},
    {"family": "wii_remote_nunchuk", "code": 901, "roles": ["gui_down", "player_backward"], "why": "Same stick, world vs. menu."},
    {"family": "wii_remote_nunchuk", "code": 902, "roles": ["gui_left", "player_left"], "why": "Same stick, world vs. menu."},
    {"family": "wii_remote_nunchuk", "code": 903, "roles": ["gui_right", "player_right"], "why": "Same stick, world vs. menu."},
    {"family": "classic_controller", "code": 205, "roles": ["gui_click_alt", "gui_split"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 204, "roles": ["gui_click", "player_jump"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 206, "roles": ["inventory", "open_inventory"], "why": "The preset omits open_inventory, so X keeps doing both, as it already did."},
    {"family": "classic_controller", "code": 200, "roles": ["chat", "gui_up"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 210, "roles": ["gui_previous", "scroll_left"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 211, "roles": ["gui_page", "scroll_right"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 208, "roles": ["gui_page_swap", "item_action_right"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 207, "roles": ["gui_quick_move", "player_sneak"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 910, "roles": ["gui_up", "player_forward"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 911, "roles": ["gui_down", "player_backward"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 912, "roles": ["gui_left", "player_left"], "why": "Unchanged from the shipped config."},
    {"family": "classic_controller", "code": 913, "roles": ["gui_right", "player_right"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 400, "roles": ["chat", "gui_up"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 404, "roles": ["gui_click", "player_jump"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 405, "roles": ["gui_click_alt", "gui_split", "scroll_left"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 406, "roles": ["gui_quick_move", "scroll_right"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 407, "roles": ["inventory", "open_inventory"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 408, "roles": ["gui_drop", "player_sneak"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 409, "roles": ["gui_previous", "item_action_right"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 410, "roles": ["gui_page", "item_action_left"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 930, "roles": ["gui_up", "player_forward"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 931, "roles": ["gui_down", "player_backward"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 932, "roles": ["gui_left", "player_left"], "why": "Unchanged from the shipped config."},
    {"family": "gamecube", "code": 933, "roles": ["gui_right", "player_right"], "why": "Unchanged from the shipped config."}
   ]
  }
 }
}
```
