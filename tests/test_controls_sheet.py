#!/usr/bin/env python3
"""docs/controls_sheet.md must describe the controls the GAME ACTUALLY HAS.

Astra ruling 3 D5 asked for "a MEANINGFUL consistency check against effective
bindings, modes and gestures -- not merely a file checksum".  So nothing here
compares the sheet to a copy of itself.  Every expected value is RECOMPUTED
from the three places the running game gets its controls from:

  1. `config_wii.json`          -- the shipped bindings and the control modes;
  2. `source/platform/input.c`  -- the built-in FALLBACKS for the keys a config
                                   may omit, the role -> config-key table, the
                                   per-family code namespaces, the chat-opener
                                   gesture rule, the code -> button-name tables
                                   and the hotbar/chord derivation;
  3. `source/platform/input_tap_gesture.h` + `input_sneak_toggle.h`
                                -- the gesture windows and the sneak default.

The sheet then has to AGREE, per controller family and per role, about the
button CODES, the button NAMES, the gesture kinds and thresholds, the modes,
and every button that carries two roles at once.  A binding changed in the
config without the sheet following is a failure that names the family and the
role; a role ADDED to the game and forgotten in the sheet is a failure too.

Finally the same COHERENCE rule runs over the shipped config AND over
tools/presets/config_wii_pointer_look.json: the mode flag and the movement
bindings must agree.  That is the defect the live card actually shipped with
(STATE.md 2026-09-19 13:50) -- new stick-movement bindings without the
`wiimote_ir_look` flag they were written for, so the Nunchuk stick both moved
and looked.  A flag alone, or bindings alone, fails here from now on.
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHEET = ROOT / "docs" / "controls_sheet.md"
CONFIG = ROOT / "config_wii.json"
PRESET = ROOT / "tools" / "presets" / "config_wii_pointer_look.json"
DEV_PRESET = ROOT / "tools" / "presets" / "config_wii_developer.json"
INPUT_C = ROOT / "source" / "platform" / "input.c"
OWNER_H = ROOT / "source" / "platform" / "input_owner.h"
TAP_H = ROOT / "source" / "platform" / "input_tap_gesture.h"
SNEAK_H = ROOT / "source" / "platform" / "input_sneak_toggle.h"

SHEET_HEADING = "## Machine-readable binding block"

# The sheet's family names <-> the enum names input.c/input_owner.h use.
FAMILIES = {
    "wii_remote_nunchuk": "INPUT_CAT_NUNCHUK",
    "classic_controller": "INPUT_CAT_CLASSIC_CONTROLLER",
    "gamecube": "INPUT_CAT_GAMECUBE",
}

# Roles a player uses while the WORLD is on screen.  Every one of them must be
# reachable on every controller, or the sheet must say -- in writing, with a
# reason -- that the family deliberately leaves it unbound (the Wii Remote has
# no gameplay Drop, and nothing binds the screenshot key on any Wii pad).
REQUIRED_GAMEPLAY_ROLES = [
    "player_forward", "player_backward", "player_left", "player_right",
    "player_jump", "player_sneak", "item_action_left", "item_action_right",
    "scroll_left", "scroll_right", "open_inventory", "open_menu", "chat",
    "debug_overlay",
]
OPTIONAL_GAMEPLAY_ROLES = ["player_drop", "screenshot"]

failures = []


def fail(msg):
    failures.append(msg)


def read(path):
    if not path.exists():
        fail("MISSING FILE: %s" % path.relative_to(ROOT).as_posix())
        return None
    return path.read_text(encoding="utf-8", errors="replace")


# ---------------------------------------------------------------------------
# 1. what the GAME says
# ---------------------------------------------------------------------------

def parse_role_table(src):
    """input_config_translate(): every role the game can bind, and its key."""
    body = re.search(r"static const char\* input_config_translate\("
                     r".*?\n\}", src, re.S)
    if not body:
        fail("input.c: could not find input_config_translate()")
        return {}
    roles = {}
    for enum, key in re.findall(r"case\s+(IB_\w+):\s*return\s+\"input\.(\w+)\";",
                                body.group(0)):
        roles[key] = enum
    if not roles:
        fail("input.c: input_config_translate() yielded no roles")
    return roles


def parse_fallbacks(src):
    """The built-in Wii defaults for the keys a config may leave out.

    Both halves are parsed: the `static const int <name>[] = {..}` arrays and
    the ternary chain that says which ROLE each array serves, so renaming an
    array or re-pointing a role is picked up rather than assumed.
    """
    body = re.search(r"static bool input_read_bindings\(.*?\n\}\n", src, re.S)
    if not body:
        fail("input.c: could not find input_read_bindings()")
        return {}, False
    body = body.group(0)
    wii = re.search(r"#ifdef PLATFORM_WII(.*?)#else", body, re.S)
    if not wii:
        fail("input.c: input_read_bindings() has no PLATFORM_WII fallback block")
        return {}, False
    arrays = {}
    for name, values in re.findall(
            r"static const int (\w+)\[\]\s*=\s*\{([^}]*)\};", wii.group(1)):
        arrays[name] = [int(v) for v in re.findall(r"-?\d+", values)]
    chain = re.search(r"const int\* defaults\s*=(.*?);", body, re.S)
    if not chain:
        fail("input.c: input_read_bindings() has no `const int* defaults` chain")
        return {}, False
    by_enum = {}
    for enum, array in re.findall(r"b == (IB_\w+)\s*\?\s*(\w+)", chain.group(1)):
        if array not in arrays:
            fail("input.c: fallback chain names unknown array `%s` for %s"
                 % (array, enum))
            continue
        by_enum[enum] = arrays[array]
    # The one fallback that is not an array: the gameplay OPEN falls back to
    # this config's own CLOSE codes (b1-chat-opener-0919/07 C).
    open_inv = bool(re.search(
        r"if\(b == IB_OPEN_INVENTORY\)\s*return config_read_int_array\("
        r"&gstate\.config_user,\s*\"input\.inventory\"", body, re.S))
    if not open_inv:
        fail("input.c: IB_OPEN_INVENTORY no longer falls back to input.inventory"
             " -- the sheet's fallback sentence is now wrong")
    return by_enum, open_inv


def parse_family_codes(src):
    """input_owner_accepts(): which code namespace belongs to which family."""
    body = re.search(r"static inline bool input_owner_accepts\(.*?\n\}", src, re.S)
    if not body:
        fail("input_owner.h: could not find input_owner_accepts()")
        return {}
    body = body.group(0)
    # Split the function into one clause per category, in source order.
    marks = [(m.start(), m.group(1)) for m in
             re.finditer(r"category == (INPUT_CAT_\w+)", body)]
    clauses = {}
    for idx, (pos, cat) in enumerate(marks):
        end = marks[idx + 1][0] if idx + 1 < len(marks) else len(body)
        clauses.setdefault(cat, body[pos:end])
    accepted = {}
    for cat, text in clauses.items():
        codes = set()
        for lo, hi in re.findall(r"key >= (\d+) && key <= (\d+)", text):
            codes.update(range(int(lo), int(hi) + 1))
        for exact in re.findall(r"key == (\d+)", text):
            codes.add(int(exact))
        accepted[cat] = codes
    for cat in FAMILIES.values():
        if not accepted.get(cat):
            fail("input_owner.h: no accepted code range parsed for %s" % cat)
    return accepted


def parse_combo_threshold(src):
    m = re.search(r"static bool input_combo_decode\(.*?if\(code < (\d+)\)",
                  src, re.S)
    if not m:
        fail("input.c: could not find input_combo_decode()'s chord threshold")
        return 1008
    return int(m.group(1))


def parse_chat_opener_rules(src):
    """input_chat_opener_rule(): the openers that are a gesture, not an edge."""
    body = re.search(r"static bool input_chat_opener_rule\(.*?\n\}", src, re.S)
    if not body:
        fail("input.c: could not find input_chat_opener_rule()")
        return {}
    rules = {}
    for code, partner, hold in re.findall(
            r"case (\d+):\s*\*partner = (\d+);\s*\*hold_ms = (\w+);",
            body.group(0)):
        rules[int(code)] = (int(partner), hold)
    if not rules:
        fail("input.c: input_chat_opener_rule() yielded no cases")
    return rules


def parse_define(text, name, path):
    if text is None:
        return None
    m = re.search(r"#define\s+%s\s+(\d+)" % re.escape(name), text)
    if not m:
        fail("%s: #define %s not found" % (path, name))
        return None
    return int(m.group(1))


# ---------------------------------------------------------------------------
# 1b. the GESTURE MACHINERY itself, checked against the C rather than mirrored
#
# Review 09 finding 4: everything above re-derives the sheet's numbers from the
# game, but the three NEW rules (the hotbar release-tap, the ordered chord, the
# sneak-toggle scoping) were only checked against hand-written Python copies of
# them -- deleting input_hotbar_step_edge outright would not have failed this
# file.  So each rule is now pinned the way the open_inventory fallback already
# was: the function has to EXIST, its CALL SITE has to exist, and the constants
# it carries have to be the ones the sheet states.
# ---------------------------------------------------------------------------

INGAME_C = ROOT / "source" / "game" / "gui" / "screen_ingame.c"


def check_gesture_machinery(src, tap, sneak, ingame, consts):
    """The C the sheet's gesture rows describe: defined AND called."""
    def need(ok, msg):
        if not ok:
            fail(msg)

    # (a) the hotbar release-tap gate, per player, and the site that consumes
    #     it.  screen_ingame.c used to gate this on slot 0, which left the
    #     split guest on a plain press edge (review 09 finding 1).
    need(re.search(r"bool input_hotbar_step_edge\(unsigned player,\s*"
                   r"const struct input_player_frame\* frame,\s*"
                   r"enum input_button b\)", src),
         "input.c: input_hotbar_step_edge(player, button) is gone -- the "
         "sheet's hotbar release-tap rows describe code that no longer exists")
    if ingame is not None:
        for role, btn in (("scroll_left", "IB_SCROLL_LEFT"),
                          ("scroll_right", "IB_SCROLL_RIGHT")):
            need(re.search(r"input_hotbar_step_edge\(slot,\s*frame,\s*%s\)" % btn,
                           ingame),
                 "screen_ingame.c: the hotbar no longer consumes "
                 "input_hotbar_step_edge(slot, %s) -- the sheet's `%s` gesture "
                 "is not what the game runs" % (btn, role))

    # (b) the PAIR DERIVATION.  The rule is "the codes this family binds to
    #     scroll_left/scroll_right, when those two also form a chord", never
    #     the literal 9 and 8 -- which is exactly what hotbar_pair() above
    #     recomputes, so the C function it models has to be there and used.
    need(re.search(r"static bool input_hotbar_chord_pair\(enum input_category",
                   src),
         "input.c: input_hotbar_chord_pair() is gone -- the sheet's "
         "per-family gesture rows are derived from a rule the game dropped")
    need(len(re.findall(r"input_hotbar_chord_pair\(", src)) >= 3,
         "input.c: input_hotbar_chord_pair() is no longer called from both "
         "the chord-order gate and the hotbar gate")

    # (c) the ORDERED chord (hold Minus, then press Plus) and its call site in
    #     the per-player poll.
    need(re.search(r"static void input_chord_order_apply\(unsigned player",
                   src),
         "input.c: input_chord_order_apply() is gone -- the sheet's "
         "`chord_ordered` gesture is not implemented")
    need(re.search(r"input_chord_order_apply\(player, read, user, "
                   r"frame\.category,\s*\n?\s*frame\.down\)", src),
         "input.c: input_chord_order_apply() is no longer called from "
         "input_players_poll -- the ordered chord never runs")

    # (d) the sneak toggle's SCOPING and the substitution site.  The scoping
    #     guard itself is checked in main(); this is the call that puts the
    #     toggled level into the ordinary IB_SNEAK level.
    need(re.search(r"static bool input_sneak_apply\(unsigned player", src),
         "input.c: input_sneak_apply() is gone -- the sheet's sneak `toggle` "
         "mode describes code that no longer exists")
    need(re.search(r"frame\.down\[IB_SNEAK\] = input_sneak_apply\(player,",
                   src),
         "input.c: the sneak toggle is no longer substituted into "
         "frame.down[IB_SNEAK] -- the sheet's sneak rows are now wrong")
    need(re.search(r"input_sneak_toggle_step\(&s->toggle,", src),
         "input.c: input_sneak_apply() no longer runs the pure "
         "input_sneak_toggle_step machine")

    # (e) the CONSTANTS the sheet states, each read out of the header the game
    #     compiles with and each reachable from the gate that uses it.
    need(sneak is not None and "INPUT_SNEAK_TOGGLE_DEFAULT" in (sneak or ""),
         "input_sneak_toggle.h: INPUT_SNEAK_TOGGLE_DEFAULT is gone -- the "
         "sheet's sneak fallback sentence has nothing behind it")
    need(tap is not None
         and "input_tap_gesture_step_hitch" in (tap or ""),
         "input_tap_gesture.h: the hitch-tolerant step is gone -- the sheet "
         "publishes hotbar_hitch_polls")
    for name, key in (
            ("INPUT_CHAT_TAP_MS", "chat_tap_ms"),
            ("INPUT_HOTBAR_TAP_MS", "hotbar_tap_ms"),
            ("INPUT_HOTBAR_HITCH_POLLS", "hotbar_hitch_polls")):
        need(name in src,
             "input.c: %s is no longer used -- the sheet publishes it as `%s`"
             % (name, key))
    # The hitch tolerance is the hotbar's ALONE (Astra ruling 4 D2: "chat
    # remains unchanged"), so the chat gate must pass a literal 0.
    chat_gate = re.search(r"bool input_chat_open_edge\(void\).*?\n\}", src, re.S)
    if not chat_gate:
        fail("input.c: could not find input_chat_open_edge()")
    elif "INPUT_HOTBAR_HITCH_POLLS" in chat_gate.group(0):
        fail("input.c: the chat opener took the hotbar's hitch tolerance -- "
             "the sheet says chat is a pure %d ms tap"
             % (consts.get("chat_tap_ms") or 0))


# Tokens that are a button's NAME rather than a word: they stay upper case, so
# "ZR" does not become "Zr" and "UP" does become "Up".
_BUTTON_LETTERS = {"A", "B", "C", "X", "Y", "Z", "L", "R", "ZL", "ZR"}


def _pretty_button(symbol):
    """WPAD_CLASSIC_BUTTON_FULL_L -> "Full L"; PAD_TRIGGER_Z -> "Z"."""
    for prefix in ("WPAD_CLASSIC_BUTTON_", "WPAD_NUNCHUK_BUTTON_",
                   "WPAD_BUTTON_", "PAD_BUTTON_", "PAD_TRIGGER_"):
        if symbol.startswith(prefix):
            symbol = symbol[len(prefix):]
            break
    parts = [p if p in _BUTTON_LETTERS else p.capitalize()
             for p in symbol.split("_")]
    return " ".join(parts)


def parse_button_names(src):
    """code -> the human button name, taken from the two translate tables."""
    names = {}
    for fn in ("input_wpad_translate", "input_pad_translate"):
        body = re.search(r"static uint32_t %s\(.*?\n\}\n" % fn, src, re.S)
        if not body:
            fail("input.c: could not find %s()" % fn)
            continue
        for code, symbol in re.findall(
                r"case (\d+): return (\w+);", body.group(0)):
            names.setdefault(int(code), _pretty_button(symbol))
    if not names:
        fail("input.c: no button names parsed")
    return names


# ---------------------------------------------------------------------------
# 2. the EFFECTIVE bindings a config produces
# ---------------------------------------------------------------------------

def effective_bindings(cfg, roles, fallbacks, open_inv_fallback):
    """What input_read_bindings() would hand back for every role.

    Explicit config array wins; an explicit EMPTY array disables the role (the
    documented way to turn an action off); otherwise the built-in fallback --
    and, for the gameplay open, this config's own `input.inventory` codes.
    """
    block = cfg.get("input", {})
    out = {}
    for key, enum in roles.items():
        if key in block:
            out[key] = list(block[key])
        elif enum == "IB_OPEN_INVENTORY" and open_inv_fallback:
            out[key] = list(block.get("inventory", fallbacks.get(enum, [])))
        elif enum in fallbacks:
            out[key] = list(fallbacks[enum])
        else:
            out[key] = []
    return out


def family_codes(codes, accepted, combo_threshold):
    """The codes of `codes` this family can physically produce.

    A chord (>= the combo threshold) belongs to the family only when BOTH of
    its halves do -- the same test input_binding_levels applies when it reads
    the two components through one device.
    """
    out = []
    for code in codes:
        if code >= combo_threshold:
            hold, press = code // 1000, code % 1000
            if hold in accepted and press in accepted:
                out.append(code)
        elif code in accepted:
            out.append(code)
    return out


def physical_halves(code, combo_threshold):
    if code >= combo_threshold:
        return [code // 1000, code % 1000]
    return [code]


def hotbar_pair(eff, accepted, combo_threshold):
    """input_hotbar_chord_pair(): this family's double-duty Minus/Plus pair.

    True only when the family's FIRST accepted scroll_left/scroll_right codes
    are also, together, somebody's chord -- exactly the condition input.c
    derives rather than writing "9 and 8".  Classic (hotbar on full L/R, chord
    on Plus+Minus) and GameCube (D-pad L/R vs L+R) do not match, so they keep
    a plain press edge.
    """
    left = family_codes(eff.get("scroll_left", []), accepted, combo_threshold)
    right = family_codes(eff.get("scroll_right", []), accepted, combo_threshold)
    left = [c for c in left if c < combo_threshold]
    right = [c for c in right if c < combo_threshold]
    if not left or not right or left[0] == right[0]:
        return None
    for codes in eff.values():
        for code in codes:
            if code < combo_threshold or code // 1000 >= combo_threshold:
                continue
            halves = {code // 1000, code % 1000}
            if halves == {left[0], right[0]}:
                return (left[0], right[0], code)
    return None


def expected_gesture(role, codes, pair, chat_rules, chat_tap, hotbar_tap,
                     hotbar_hitch, combo_threshold):
    """The gesture the game would run for one family's binding of one role."""
    if role == "chat":
        for code in codes:
            if code in chat_rules:
                partner, hold = chat_rules[code]
                window = chat_tap if hold != "0" else None
                kind = "tap_release" if window else "release"
                g = {"kind": kind, "opener_code": code}
                if window:
                    g["threshold_ms"] = window
                if partner:
                    g["cancel_partner_code"] = partner
                return g
        return None
    if role in ("scroll_left", "scroll_right") and pair:
        prev, nxt, _chord = pair
        opener = nxt if role == "scroll_right" else prev
        partner = prev if role == "scroll_right" else nxt
        return {"kind": "clean_tap_release", "opener_code": opener,
                "threshold_ms": hotbar_tap, "hitch_polls": hotbar_hitch,
                "cancel_partner_code": partner}
    if role == "debug_overlay":
        for code in codes:
            if code < combo_threshold:
                continue
            hold, press = code // 1000, code % 1000
            if pair and {hold, press} == {pair[0], pair[1]}:
                return {"kind": "chord_ordered", "hold_code": hold,
                        "press_code": press}
            return {"kind": "chord_simultaneous", "hold_code": hold,
                    "press_code": press}
        return None
    return None


def shared_code_map(eff, accepted, combo_threshold):
    """Every PHYSICAL button this family gives more than one role.

    A chord counts through its halves: holding L and R on a GameCube pad
    toggles the overlay AND consumes place/break, which is precisely the kind
    of thing a controls sheet has to admit to.
    """
    by_code = {}
    for role, codes in sorted(eff.items()):
        for code in family_codes(codes, accepted, combo_threshold):
            for half in physical_halves(code, combo_threshold):
                by_code.setdefault(half, [])
                if role not in by_code[half]:
                    by_code[half].append(role)
    return {code: roles for code, roles in by_code.items() if len(roles) > 1}


# ---------------------------------------------------------------------------
# 3. the sheet
# ---------------------------------------------------------------------------

def sheet_block(text):
    if text is None:
        return None
    idx = text.find(SHEET_HEADING)
    if idx < 0:
        fail("controls_sheet.md: heading %r not found" % SHEET_HEADING)
        return None
    fence = re.search(r"```json\s*\n(.*?)\n```", text[idx:], re.S)
    if not fence:
        fail("controls_sheet.md: no ```json block under %r" % SHEET_HEADING)
        return None
    try:
        return json.loads(fence.group(1))
    except ValueError as exc:
        fail("controls_sheet.md: the machine block is not valid JSON (%s)" % exc)
        return None


def check_family(fam, block_fam, eff, accepted, roles, names, pair, chat_rules,
                 chat_tap, hotbar_tap, hotbar_hitch, combo_threshold, where):
    sheet_roles = block_fam.get("roles", {})
    unbound = block_fam.get("unbound_by_design", {})

    for role in sorted(roles):
        expect = family_codes(eff.get(role, []), accepted, combo_threshold)
        if role not in sheet_roles:
            fail("%s: role `%s` exists in the game's role table but the sheet "
                 "does not list it for %s" % (where, role, fam))
            continue
        got = sheet_roles[role].get("codes")
        if got != expect:
            fail("%s: %s / %s -- sheet says codes %s, the effective bindings "
                 "are %s" % (where, fam, role, got, expect))
        # Button names come from input.c's translate tables, so a sheet that
        # renames Minus into Plus fails even with the right number.
        label = (sheet_roles[role].get("button") or "").strip()
        if expect and not label:
            fail("%s: %s / %s -- the sheet gives no button name" % (where, fam,
                                                                    role))
        for code in expect:
            for half in physical_halves(code, combo_threshold):
                if half in names and names[half].lower() not in label.lower():
                    fail("%s: %s / %s -- the sheet calls code %d %r, input.c "
                         "calls it %r" % (where, fam, role, half, label,
                                          names[half]))
                elif half not in names and 900 <= half < 944 \
                        and "stick" not in label.lower():
                    fail("%s: %s / %s -- code %d is an emulated STICK "
                         "direction but the sheet says %r"
                         % (where, fam, role, half, label))

    for role in sorted(sheet_roles):
        if role not in roles:
            fail("%s: %s -- the sheet lists role `%s`, which the game has no "
                 "config key for" % (where, fam, role))

    # Gestures and their thresholds.
    for role in sorted(roles):
        if role not in sheet_roles:
            continue
        expect = expected_gesture(
            role, family_codes(eff.get(role, []), accepted, combo_threshold),
            pair, chat_rules, chat_tap, hotbar_tap, hotbar_hitch,
            combo_threshold)
        got = sheet_roles[role].get("gesture")
        if expect is None and got is not None:
            fail("%s: %s / %s -- the sheet claims a gesture (%s) the game does "
                 "not run" % (where, fam, role, got))
        elif expect is not None and got != expect:
            fail("%s: %s / %s -- sheet gesture %s, the game runs %s"
                 % (where, fam, role, got, expect))

    # Required gameplay roles: bound, or deliberately not.
    for role in REQUIRED_GAMEPLAY_ROLES + OPTIONAL_GAMEPLAY_ROLES:
        if role not in roles:
            continue
        bound = family_codes(eff.get(role, []), accepted, combo_threshold)
        reason = (unbound.get(role) or "").strip()
        if not bound and not reason:
            fail("%s: %s leaves gameplay role `%s` UNBOUND and the sheet gives "
                 "no reason -- a forgotten binding, or say so on purpose"
                 % (where, fam, role))
        if bound and reason:
            fail("%s: %s / %s is listed as unbound-by-design but is bound to %s"
                 % (where, fam, role, bound))


def check_shared(block, configs, where):
    """Every double-duty button must be declared, with its exact role set."""
    declared = {}
    for entry in block.get("shared_codes", []):
        key = (entry.get("family"), entry.get("code"))
        declared[key] = entry
    for fam, eff, accepted, combo_threshold in configs:
        actual = shared_code_map(eff, accepted, combo_threshold)
        for code, sharers in sorted(actual.items()):
            entry = declared.pop((fam, code), None)
            if entry is None:
                fail("%s: %s -- code %d carries %s and the sheet declares no "
                     "such pair" % (where, fam, code, sharers))
                continue
            if sorted(entry.get("roles", [])) != sorted(sharers):
                fail("%s: %s -- code %d is declared for %s but actually "
                     "carries %s" % (where, fam, code,
                                     sorted(entry.get("roles", [])),
                                     sorted(sharers)))
            if not (entry.get("why") or "").strip():
                fail("%s: %s -- shared code %d is declared with no reason"
                     % (where, fam, code))
    for (fam, code) in sorted(declared, key=lambda k: (str(k[0]), k[1] or 0)):
        fail("%s: the sheet declares a shared code %s/%s that no longer exists"
             % (where, fam, code))


def check_coherence(cfg, eff, accepted_by_fam, combo_threshold, where):
    """The defect the live card shipped with, made impossible.

    With `wiimote_ir_look` 0 the Nunchuk stick IS the camera; binding that same
    stick's emulated D-pad codes to movement makes it move AND look at once.
    With the flag at 1 the camera is the IR pointer and the stick is free to
    move.  So the flag and the movement bindings are two halves of one
    decision, and one without the other is the hybrid.
    """
    controls = cfg.get("controls", {})
    ir = int(controls.get("wiimote_ir_look", 0))
    nunchuk = accepted_by_fam["INPUT_CAT_NUNCHUK"]
    move = []
    for role in ("player_forward", "player_backward", "player_left",
                 "player_right"):
        move += family_codes(eff.get(role, []), nunchuk, combo_threshold)
    on_stick = [c for c in move if 900 <= c <= 903]
    on_dpad = [c for c in move if 0 <= c <= 3]
    if on_stick and on_dpad:
        fail("%s: the Wii Remote family moves on BOTH the Nunchuk stick %s and "
             "the D-pad %s -- pick one" % (where, on_stick, on_dpad))
    if on_stick and ir != 1:
        fail("%s: movement is on the Nunchuk stick %s but controls."
             "wiimote_ir_look is %d -- that stick is then the CAMERA too (the "
             "hybrid the live card shipped with, STATE.md 2026-09-19 13:50)"
             % (where, on_stick, ir))
    if on_dpad and ir != 0:
        fail("%s: movement is on the D-pad %s but controls.wiimote_ir_look is "
             "%d -- the Nunchuk stick then steers nothing" % (where, on_dpad, ir))
    if not on_stick and not on_dpad:
        fail("%s: the Wii Remote family has no movement bindings at all" % where)
    return ir


# The config key input.c reads for each family's sneak mode (E-1). Kept beside
# the families table so a new family cannot be added without deciding this.
SNEAK_TOGGLE_KEY = {"wii_remote_nunchuk": "wiimote_sneak_toggle",
                    "classic_controller": "classic_sneak_toggle",
                    "gamecube": "gamecube_sneak_toggle"}


def check_modes(block_fam, fam, cfg, sneak_default, ir, where):
    controls = cfg.get("controls", {})
    modes = block_fam.get("modes", {})
    # Every gameplay family now has its own toggle key, ABSENT meaning the
    # header's default -- so the sheet's mode is computed from the config the
    # same way input_init does, never asserted.
    toggle = int(controls.get(SNEAK_TOGGLE_KEY[fam], sneak_default)) != 0
    expect = {"sneak": "toggle" if toggle else "hold"}
    if fam == "wii_remote_nunchuk":
        expect["wiimote_ir_look"] = ir
        expect["camera"] = "ir_pointer" if ir else "nunchuk_stick"
    else:
        expect["camera"] = ("right_stick" if fam == "classic_controller"
                            else "c_stick")
    if modes != expect:
        fail("%s: %s modes %s, computed %s" % (where, fam, modes, expect))


def main():
    src = read(INPUT_C)
    owner = read(OWNER_H)
    tap = read(TAP_H)
    sneak = read(SNEAK_H)
    sheet_text = read(SHEET)
    if src is None or owner is None or tap is None or sneak is None:
        return report()

    roles = parse_role_table(src)
    fallbacks, open_inv = parse_fallbacks(src)
    accepted = parse_family_codes(owner)
    combo_threshold = parse_combo_threshold(src)
    chat_rules = parse_chat_opener_rules(src)
    names = parse_button_names(src)
    chat_tap = parse_define(tap, "INPUT_CHAT_TAP_MS", "input_tap_gesture.h")
    hotbar_tap = parse_define(tap, "INPUT_HOTBAR_TAP_MS", "input_tap_gesture.h")
    hotbar_hitch = parse_define(tap, "INPUT_HOTBAR_HITCH_POLLS",
                                "input_tap_gesture.h")
    sneak_default = parse_define(sneak, "INPUT_SNEAK_TOGGLE_DEFAULT",
                                 "input_sneak_toggle.h")
    # E-1 (Astra ergonomics 1 + 3, maintainer-accepted): the toggle is no
    # longer Remote-only. Each gameplay family picks its own key, so the sheet's
    # per-family `sneak` mode is read out of THIS table in the C rather than
    # trusted -- a family silently dropped here would otherwise leave the sheet
    # claiming a toggle the input layer never applies.
    if not re.search(r"category == INPUT_CAT_NUNCHUK \? wiimote_sneak_toggle\s*"
                     r":\s*category == INPUT_CAT_CLASSIC_CONTROLLER \? "
                     r"classic_sneak_toggle\s*"
                     r":\s*category == INPUT_CAT_GAMECUBE \? "
                     r"gamecube_sneak_toggle", src):
        fail("input.c: the per-family sneak-toggle table is gone -- the "
             "sheet's `sneak` mode rows can no longer be derived from the C")
    if not re.search(r"wiimote_ir_look && owner->category == INPUT_CAT_NUNCHUK",
                     src):
        fail("input.c: wiimote_ir_look is no longer scoped to INPUT_CAT_NUNCHUK"
             " -- the sheet's camera rows are now wrong")

    block = sheet_block(sheet_text)
    if block is None or failures:
        return report()

    if block.get("describes", {}).get("config") != "config_wii.json":
        fail("controls_sheet.md: the block must name the config it describes "
             "(config_wii.json)")
    if "candidate controls" not in (block.get("status") or ""):
        fail("controls_sheet.md: the block must carry the candidate-controls "
             "status")
    consts = block.get("gesture_constants", {})
    if consts != {"chat_tap_ms": chat_tap, "hotbar_tap_ms": hotbar_tap,
                  "hotbar_hitch_polls": hotbar_hitch}:
        fail("controls_sheet.md: gesture constants %s, the headers define "
             "chat_tap_ms=%s hotbar_tap_ms=%s hotbar_hitch_polls=%s"
             % (consts, chat_tap, hotbar_tap, hotbar_hitch))
    # Review 09 finding 4: the rules themselves, against the C.
    check_gesture_machinery(src, tap, sneak, read(INGAME_C), consts)

    cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
    eff = effective_bindings(cfg, roles, fallbacks, open_inv)
    ir = check_coherence(cfg, eff, accepted, combo_threshold, "config_wii.json")

    shipped = []
    for fam, cat in sorted(FAMILIES.items()):
        block_fam = block.get("families", {}).get(fam)
        if block_fam is None:
            fail("controls_sheet.md: no `%s` family in the machine block" % fam)
            continue
        pair = hotbar_pair(eff, accepted[cat], combo_threshold)
        check_family(fam, block_fam, eff, accepted[cat], roles, names, pair,
                     chat_rules, chat_tap, hotbar_tap, hotbar_hitch,
                     combo_threshold, "config_wii.json")
        check_modes(block_fam, fam, cfg, sneak_default, ir, "config_wii.json")
        shipped.append((fam, eff, accepted[cat], combo_threshold))
    check_shared(block, shipped, "config_wii.json")

    check_preset(block, roles, fallbacks, open_inv, accepted, combo_threshold)
    check_developer_preset()
    return report()


def check_developer_preset():
    """E-2 (Astra ergonomics 2): the F3 chords left the RELEASE config.

    The developer preset is not an alternative SCHEME -- it is the shipped
    file with exactly one key put back, so that installing it can never
    silently change anything else a player is used to. That is checked as an
    exact diff rather than described: any other key drifting apart is a
    failure that names the key.
    """
    shipped = json.loads(CONFIG.read_text(encoding="utf-8"))
    if not DEV_PRESET.exists():
        fail("tools/presets/%s is missing -- the release config unbinds F3, so "
             "there has to be a configuration that binds it back"
             % DEV_PRESET.name)
        return
    try:
        dev = json.loads(DEV_PRESET.read_text(encoding="utf-8"))
    except ValueError as exc:
        fail("%s is not valid JSON (%s)" % (DEV_PRESET.name, exc))
        return
    if shipped.get("input", {}).get("debug_overlay") != []:
        fail("config_wii.json: input.debug_overlay must be an EXPLICIT EMPTY "
             "ARRAY (an omitted key is a different statement), got %r"
             % (shipped.get("input", {}).get("debug_overlay"),))
    if not dev.get("input", {}).get("debug_overlay"):
        fail("%s: input.debug_overlay is empty -- this preset exists to BIND "
             "the chords" % DEV_PRESET.name)
    # Exactly one key apart, at any depth.
    def walk(a, b, path):
        if isinstance(a, dict) and isinstance(b, dict):
            for key in sorted(set(a) | set(b)):
                walk(a.get(key), b.get(key), path + [key])
            return
        if a != b and path != ["input", "debug_overlay"]:
            fail("%s differs from config_wii.json at `%s` (%r vs %r) -- the "
                 "developer preset must be the shipped file with ONLY the "
                 "F3 binding restored"
                 % (DEV_PRESET.name, ".".join(path), b, a))
    walk(shipped, dev, [])


def check_preset(block, roles, fallbacks, open_inv, accepted, combo_threshold):
    """The pointer-look preset must be a COMPLETE, coherent example.

    Astra ruling 3 D4: "a mode flag alone must not recreate the hybrid".  The
    preset therefore goes through the same coherence rule as the shipped
    config, plus: every gameplay role reachable on every family, and every
    double-duty button declared.
    """
    text = read(PRESET)
    if text is None:
        return
    try:
        cfg = json.loads(text)
    except ValueError as exc:
        fail("%s is not valid JSON (%s)" % (PRESET.name, exc))
        return
    where = "tools/presets/%s" % PRESET.name
    declared = block.get("presets", {}).get(PRESET.name)
    if declared is None:
        fail("controls_sheet.md: the machine block declares nothing for the "
             "preset `%s`" % PRESET.name)
        declared = {}
    eff = effective_bindings(cfg, roles, fallbacks, open_inv)
    check_coherence(cfg, eff, accepted, combo_threshold, where)
    for fam, cat in sorted(FAMILIES.items()):
        unbound = declared.get("unbound_by_design", {}).get(fam, {})
        for role in REQUIRED_GAMEPLAY_ROLES + OPTIONAL_GAMEPLAY_ROLES:
            if role not in roles:
                continue
            bound = family_codes(eff.get(role, []), accepted[cat],
                                 combo_threshold)
            reason = (unbound.get(role) or "").strip()
            if not bound and not reason:
                fail("%s: %s leaves gameplay role `%s` unbound with no stated "
                     "reason" % (where, fam, role))
            if bound and reason:
                fail("%s: %s / %s is listed as unbound-by-design but is bound "
                     "to %s" % (where, fam, role, bound))
    check_shared({"shared_codes": declared.get("shared_codes", [])},
                 [(fam, eff, accepted[cat], combo_threshold)
                  for fam, cat in sorted(FAMILIES.items())], where)


def report():
    if failures:
        print("test_controls_sheet: %d problem(s)" % len(failures))
        for msg in failures:
            print("  FAIL %s" % msg)
        return 1
    print("test_controls_sheet: the sheet matches the effective bindings, "
          "modes and gestures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
