"""Every hw_run profile must be registered in EVERY tool that names profiles.

Hostile-review finding 1 (b1-chat-opener-0919/review04): mp_chat_chord_1m was
added to hw_run.c, to the collector and to run_staged_wii_hardware.ps1 -- and
to nothing else, so the run could be requested but never staged, launched,
collected or validated, and would simply have timed out on the console. That is
a HALF-REGISTERED profile, and it is invisible until a whole hardware slot is
wasted on it.

This test makes the next one impossible: `source/game/hw_run.c`'s
`hw_run_profile_name` switch is the CANONICAL list (it is what `--cw-profile`
accepts and what the device echoes), and every profile in it must appear in

  * the collector's PROFILES table (tools/wii_telemetry_server.py), and
  * the -Profile ValidateSet of every tool in TOOLS below.

A tool that legitimately supports FEWER profiles must say so HERE, in
ALLOWED_OMISSIONS, with a reason -- silence is what this test exists to
prevent. The sentinel values the tools add for "no profile" ("none", "") are
not profiles and are ignored in both directions.
"""
import importlib.util
import io
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
_spec = importlib.util.spec_from_file_location(
    'wii_telemetry_server', ROOT / 'tools' / 'wii_telemetry_server.py')
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

# Every tool whose -Profile parameter names the profiles it accepts.
TOOLS = (
    'tools/stage_wii_dol.ps1',
    'tools/run_wii_hardware.ps1',
    'tools/collect_staged_wii.ps1',
    'tools/validate_wii_pass.ps1',
    'tools/run_staged_wii_hardware.ps1',
)

# "no profile" sentinels: not profiles, never expected in hw_run.c.
SENTINELS = {'none', ''}

# {tool: {profile: why it is legitimately absent}}. EMPTY on purpose today:
# every tool currently accepts every profile, and the day one does not, the
# reason belongs here rather than in a silent gap.
ALLOWED_OMISSIONS: dict = {}

# {profile: why its tools-side registration has not landed YET}. This is NOT
# ALLOWED_OMISSIONS: an entry here says "half-registered ON PURPOSE, for the
# length of one parallel lane", and it is SELF-CLEANING -- the moment the
# tools half lands, test_pending_registrations_are_still_pending FAILS and
# tells whoever integrated the two halves to delete the row. A permanent
# weakening of this guard is therefore impossible; the worst case is one
# loud failure at the merge.
#
# push-0921 splits the mp_play_10m card down the middle: B4c owns source/**
# (the enum, the profile's config, the device wiring) and B5 owns tools/**
# (the collector's MpPlayEvidence, the five ValidateSets, the run-nonce
# condition), in two worktrees, at the same time, under Astra ruling 1 answer
# C1 ("whole-file ownership transferred explicitly... the collector and build
# manifests shared across lanes"). Neither worker may edit the other's files,
# so between the two lanes landing the profile IS half-registered -- and the
# honest record of that is this dict, not a green test.
PENDING_REGISTRATION = {
}


def hw_run_profiles():
    """The canonical list: hw_run_profile_name's switch arms, in order."""
    src = io.open(ROOT / 'source/game/hw_run.c', encoding='utf-8').read()
    names = re.findall(
        r'case HW_RUN_PROFILE_[A-Z0-9_]+: return "([a-z0-9_]+)";', src)
    assert names, 'hw_run_profile_name could not be parsed'
    return names


def tool_profiles(path):
    """The -Profile ValidateSet of one tool."""
    text = io.open(ROOT / path, encoding='utf-8').read()
    match = re.search(r'\[ValidateSet\(([^)]*)\)\]\s*(?:#[^\n]*\n\s*)*'
                      r'\[string\]\$Profile', text)
    if not match:  # the profile set is the first one in every tool today
        match = re.search(r'\[ValidateSet\(([^)]*)\)\]', text)
    assert match, '%s has no -Profile ValidateSet' % path
    return set(re.findall(r'"([^"]*)"', match.group(1))) - SENTINELS


class ProfileRegistrationTest(unittest.TestCase):
    def test_every_profile_is_registered_everywhere(self):
        profiles = hw_run_profiles()
        self.assertEqual(len(profiles), len(set(profiles)),
                         'hw_run_profile_name returns a duplicate name')
        # The two profiles this lane added/changed must be in the list at all,
        # so a rename cannot make the sweep below vacuously pass.
        for required in ('mp_chat_login_3m', 'mp_chat_chord_1m',
                         'mp_play_10m'):
            self.assertIn(required, profiles)
        for path in TOOLS:
            have = tool_profiles(path)
            allowed = ALLOWED_OMISSIONS.get(path, {})
            missing = sorted(p for p in profiles
                             if p not in have and p not in allowed
                             and p not in PENDING_REGISTRATION)
            self.assertEqual(missing, [],
                             '%s does not accept %s -- a half-registered '
                             'profile can be requested but never run. Add it '
                             'to the ValidateSet, or record the reason in '
                             'ALLOWED_OMISSIONS.' % (path, missing))
            unknown = sorted(p for p in have if p not in profiles)
            self.assertEqual(unknown, [],
                             '%s accepts %s, which hw_run.c does not define -- '
                             'the device would reject it at boot'
                             % (path, unknown))
        collector = set(mod.PROFILES)
        self.assertEqual(sorted(p for p in profiles if p not in collector
                                and p not in PENDING_REGISTRATION), [],
                         'the collector grades no such profile')
        self.assertEqual(sorted(p for p in collector if p not in profiles), [],
                         'the collector names a profile hw_run.c does not')

    def test_pending_registrations_are_still_pending(self):
        """PENDING_REGISTRATION is self-cleaning.

        Every entry must be (a) a real profile hw_run.c defines, and (b) still
        genuinely absent from at least one of the places the sweep above
        checks. The moment the other lane lands its half, (b) stops holding
        and THIS test fails -- which is the reminder to delete the row and
        restore the full guard. An entry that was never a profile in the first
        place fails on (a), so a typo cannot silently disable a check either.
        """
        profiles = set(hw_run_profiles())
        collector = set(mod.PROFILES)
        for profile, why in PENDING_REGISTRATION.items():
            self.assertIn(profile, profiles,
                          'PENDING_REGISTRATION names "%s", which hw_run.c '
                          'does not define -- remove the entry' % profile)
            self.assertTrue(why.strip(), 'every entry states its reason')
            absent_from = [path for path in TOOLS
                           if profile not in tool_profiles(path)]
            if profile not in collector:
                absent_from.append('tools/wii_telemetry_server.py')
            self.assertNotEqual(
                absent_from, [],
                '"%s" is now registered EVERYWHERE -- delete its '
                'PENDING_REGISTRATION entry so the sweep guards it again. '
                'Reason recorded when it was added: %s' % (profile, why))

    def test_world_copy_marker_mirror(self):
        """ui-build-1026/T16a: the collector's WORLD_COPY_MARKERS is a COPY of
        the walker's enum menu_ap_copy_marker (menu_autopilot_script.h), in
        emission order, and its required sets match
        menu_ap_copy_required_markers(). A marker added on one side only
        would either never be required (silent pass) or always be missing.

        The profile itself is NOT registered yet (hw_run.c is a product file;
        tmp/jobs/ui-build-1026/T16a-RIG.md carries the owed hunks). Until
        hw_run.c names it, no tool may accept it -- the sweep above enforces
        that -- and once it does, every tool and PROFILES must."""
        hdr = io.open(ROOT / 'source/game/menu_autopilot_script.h',
                      encoding='utf-8').read()
        body = hdr[hdr.index('enum menu_ap_copy_marker {'):]
        body = body[:body.index('};')]
        names = re.findall(r'MENU_AP_WCC_([A-Z_]+)', body)
        self.assertEqual(names[-1], 'COUNT')
        self.assertEqual(tuple('wcc_' + n.lower() for n in names[:-1]),
                         mod.WORLD_COPY_MARKERS)
        variants = re.findall(r'MENU_AP_COPY_([A-Z_]+)[ =,]', hdr[
            hdr.index('enum menu_ap_copy_variant {'):])
        self.assertEqual(tuple(v.lower() for v in variants[:5]),
                         mod.WORLD_COPY_VARIANTS)
        for variant in mod.WORLD_COPY_VARIANTS:
            missing = set(mod.WORLD_COPY_MARKERS) - set(
                mod.WORLD_COPY_REQUIRED[variant])
            self.assertEqual(missing, {'wcc_result', 'wcc_list_post'}
                             if variant == 'interrupt' else set(), variant)
        registered = mod.WORLD_COPY_PROFILE in hw_run_profiles()
        self.assertEqual(mod.WORLD_COPY_PROFILE in mod.PROFILES, registered,
                         'world_copy_convert is half-registered')

    def test_mp_profiles_get_the_endpoint_and_the_nonce(self):
        """A driver profile that joins the LAN fixture must be handed
        --cw-mp-endpoint and the run nonce. stage_wii_dol.ps1 derives the nonce
        per profile, so the condition has to name every such profile -- the
        chord run would otherwise never join and would time out."""
        stage = io.open(ROOT / 'tools/stage_wii_dol.ps1',
                        encoding='utf-8').read()
        nonce = re.search(r'if\(\$Profile -(?:eq|in) ([^)]*)\)\s*-and\s*'
                          r'\[string\]::IsNullOrWhiteSpace\(\$MpUsername\)\)',
                          stage.replace('\r\n', '\n'))
        if nonce is None:
            nonce = re.search(r'\n\s*if\(([^\n]*MpUsername[^\n]*)\)\s*\{',
                              stage.replace('\r\n', '\n'))
        self.assertIsNotNone(nonce, 'the run-nonce condition moved')
        condition = nonce.group(0)
        for profile in ('mp_chat_login_3m', 'mp_chat_chord_1m'):
            self.assertIn(profile, condition,
                          '%s never gets a run nonce from stage_wii_dol.ps1'
                          % profile)
        # mp_play_10m needs the nonce more than either of them (its username
        # IS the run nonce every fixture receipt is matched against), but
        # stage_wii_dol.ps1 belongs to push-0921/B5's worktree today -- see
        # PENDING_REGISTRATION. The DEVICE half below is asserted for it
        # unconditionally, because that half lives in source/game/hw_run.c.
        for profile in PENDING_REGISTRATION:
            if profile in condition:  # the other lane landed: guard it again
                self.assertIn(profile, condition)
        # ... and the device accepts both flags only on an MP profile, so the
        # same names must be in hw_run.c's MP-profile gate.
        hw = io.open(ROOT / 'source/game/hw_run.c', encoding='utf-8').read()
        gate = hw[hw.index('bool mp_profile ='):]
        gate = gate[:gate.index(';')]
        for profile in ('MP_CHAT_LOGIN_3M', 'MP_CHAT_CHORD_1M',
                        'MP_PLAY_10M'):
            self.assertIn(profile, gate,
                          'hw_run.c refuses --cw-mp-endpoint for %s' % profile)


if __name__ == '__main__':
    unittest.main(verbosity=0)
