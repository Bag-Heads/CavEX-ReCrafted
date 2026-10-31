"""mp_chat_login_3m's collector verdict, replayed OFFLINE -- no Wii, no server.

The B1c contract (tmp/jobs/b1-chat-keyboard-0917/b1c-contract-v1-ruled.md,
sections 3, 5, 6 and the section 8 rulings) is graded by
tools/wii_telemetry_server.py's ChatLoginEvidence. This suite finds every
harness rule offline instead of spending one real-Wii run per rule -- the
robot-recovery replay approach (tmp/jobs/robot-recovery-0917/replay_collector.py)
without its loopback socket, because the marker stream is synthesized here
rather than recorded: the rows go straight through Collector.process(), which
is the same entry point the UDP/TCP readers feed.

The valid stream is built ONCE below and every rejection case is that same
stream with exactly one thing broken, so a case can never pass for a reason the
valid stream does not also have. Written BEFORE any Wii run, as section 5
requires.
"""
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))  # the collector imports its siblings
_spec = importlib.util.spec_from_file_location(
    'wii_telemetry_server', ROOT / 'tools' / 'wii_telemetry_server.py')
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

# One synthesized frame is 20 ms (50 fps) and the device reports the same as
# its worst frame, so the contract's tolerance is +-20 ms plus the collector's
# one 20 fps frame on the LATE side (see _cadence_window).
PERIOD = 20
MAX_FRAME_MS = 20
# Section 9 ruling 5's mask bits, by name.
LEFT, RIGHT, UP, DOWN, CLICK, BACK, PAGE, PREVIOUS = (1 << i for i in range(8))
# Section 2's route: (presses before the click, row, col, page, char). 37
# directional + the one GUI_PAGE press (entry 14, which is why it carries 3)
# + 15 clicks = the 53 p4.key markers of section 9 ruling 1.
ROUTE = [(3, 3, 9, 0, '/'), (2, 2, 8, 0, 'l'), (1, 1, 8, 0, 'o'),
         (5, 2, 4, 0, 'g'), (4, 1, 7, 0, 'i'), (4, 3, 5, 0, 'n'),
         (1, 4, 1, 0, ' '), (4, 2, 5, 0, 'h'), (2, 1, 6, 0, 'u'),
         (3, 3, 5, 0, 'n'), (3, 1, 4, 0, 't'), (2, 1, 2, 0, 'e'),
         (1, 1, 3, 0, 'r'), (3, 1, 1, 3, '2'), (0, 1, 1, 3, '2')]


def hold_leg(repeats=5, period=PERIOD, delay=250, rate=80, tail=3):
    """Per-frame records for leg (a): the edge step, `repeats` repeat steps at
    the contract cadence, the requested release, then quiet frames.

    `rem` is the repeat timer as the frame's step decision saw it -- after this
    frame's decrement, before any reload -- so a stepping frame carries 0 and
    section 3's "never a step while rem > 0" is production's own condition."""
    records = [dict(t=0, req=RIGHT, edge='^', cp=RIGHT, cd=RIGHT, rep=1, rem=0,
                    r=1, c=0)]
    t, rem, col, done = 0, delay, 0, 0
    while done < repeats:
        t += period
        rem -= period
        if rem <= 0:
            col += 1
            records.append(dict(t=t, req=RIGHT, cp=0, cd=RIGHT, rep=2, rem=0,
                                r=1, c=col))
            rem, done = rate, done + 1
        else:
            records.append(dict(t=t, req=RIGHT, cp=0, cd=RIGHT, rep=0, rem=rem,
                                r=1, c=col))
    t += period  # release REQUESTED; production still holds it this frame
    records.append(dict(t=t, req=RIGHT, edge='v', cp=0, cd=RIGHT, rep=0, rem=max(rem, 0),
                        r=1, c=col))
    for _ in range(tail):  # 500 ms quiet, no step after the consumed release
        t += period
        records.append(dict(t=t, req=0, cp=0, cd=0, rep=0, rem=0, r=1, c=col))
    return records


def tap_leg(frames=6, period=PERIOD, delay=250):
    """Leg (b): press and release inside the 250 ms delay -- one step, ever."""
    records = [dict(t=0, req=RIGHT, edge='^', cp=RIGHT, cd=RIGHT, rep=1, rem=0,
                    r=1, c=0)]
    t, rem = 0, delay
    for i in range(frames):
        t += period
        rem -= period
        held = i < frames - 3
        records.append(dict(t=t, req=RIGHT if held else 0,
                            edge='v' if i == frames - 3 else '',
                            cp=0, cd=RIGHT if held else 0, rep=0,
                            rem=max(rem, 0), r=1, c=1))
    return records


def reconnect_leg(period=PERIOD, delay=250, frozen=6, blocked=3):
    """Leg P3 (section 9 ruling 6 gives it its own p3.trace batches): RIGHT is
    held, the pad disconnects, reconnects still held, and only a NEUTRAL frame
    re-arms it. Nothing may move while the owner is gone, and the re-armed
    press is a FRESH edge -- which restarts the repeat chain, so the long gap
    is not a late repeat."""
    records = [dict(t=0, req=RIGHT, edge='^', cp=RIGHT, cd=RIGHT, rep=1, rem=0,
                    r=1, c=0)]
    t, rem = 0, delay
    for _ in range(2):  # still held, well inside the 250 ms delay
        t += period
        rem -= period
        records.append(dict(t=t, req=RIGHT, cp=0, cd=RIGHT, rep=0, rem=rem, r=1,
                            c=0))
    for _ in range(frozen):  # owner gone: no consumption, no movement
        t += period
        records.append(dict(t=t, req=RIGHT, cp=0, cd=0, rep=0, rem=0, r=1, c=0))
    for _ in range(blocked):  # reconnected, still held: the re-arm blocks it
        t += period
        records.append(dict(t=t, req=RIGHT, cp=0, cd=0, rep=0, rem=0, r=1, c=0))
    t += period  # the neutral frame that clears the re-arm
    records.append(dict(t=t, req=0, edge='v', cp=0, cd=0, rep=0, rem=0, r=1, c=0))
    t += period  # and the fresh press that may finally move
    records.append(dict(t=t, req=RIGHT, edge='^', cp=RIGHT, cd=RIGHT, rep=1,
                        rem=0, r=1, c=1))
    t += period
    records.append(dict(t=t, req=0, edge='v', cp=0, cd=0, rep=0, rem=0, r=1, c=1))
    return records


def page_leg(frames=9, period=PERIOD, delay=250, tap_at=4):
    """Leg (c): DOWN held while GUI_PAGE is tapped. The page change reloads the
    repeat delay (widget_osk.c:968-976), so the hold produces no extra step."""
    records = [dict(t=0, req=DOWN, edge='^', cp=DOWN, cd=DOWN, rep=1, rem=0,
                    r=1, c=0)]
    t, rem = 0, delay
    for i in range(1, frames + 1):
        t += period
        rem -= period
        if i == tap_at:
            # The GUI_PAGE press is CONSUMED this frame and moves no cell --
            # a non-DIRECTIONAL press, so the "immediate step on the edge
            # frame" rule does not cover it (section 9 ruling 5 is what makes
            # that distinction checkable). The page change then RELOADS the
            # delay (widget_osk.c:968-976).
            records.append(dict(t=t, req=DOWN | PAGE, edge='^', cp=PAGE,
                                cd=DOWN | PAGE, rep=0, rem=max(rem, 0), r=1,
                                c=0))
            rem = delay
            continue
        records.append(dict(t=t, req=DOWN, cp=0, cd=DOWN, rep=0,
                            rem=max(rem, 0), r=1, c=0))
    return records


def encode(records):
    """Section 13 ruling 1: req/cp/cd are HEX masks (with req's optional ^/v
    edge marker); t/rep/rem/r/c are decimal. A `req` given as a string is
    written verbatim, which is how the malformed-tuple cases are built."""
    def req_of(r):
        if isinstance(r['req'], str):
            return r['req']
        return '%x%s' % (r['req'], r.get('edge', ''))
    return ';'.join('%d:%s:%x:%x:%d:%d:%d:%d'
                    % (r['t'], req_of(r), r['cp'], r['cd'], r['rep'], r['rem'],
                       r['r'], r['c']) for r in records)


def stream(run_id='chat', skip=(), repeat=None, hold=None, keys=None,
           reconnect=None, platform='wii', refused=None, frame_ms=MAX_FRAME_MS,
           run_frame_ms=None, contract=mod.CHAT_LOGIN_CONTRACT_CURRENT,
           hold_ms=850):
    """The one valid marker stream, section 3 as ruled by sections 9 and 10, in
    its emission order. `platform` switches the two PLATFORM-specific P1
    readings (section 10 rulings 2 and 5): the console binds WPAD channel 3 as
    a Nunchuk and MUST refuse the gated press; the PC binds {KEYBOARD,0} and
    has no opener gate at all.

    `contract` is the marker contract the run was RECORDED under (Astra ruling
    2 D2): 1 is an old run directory, whose P1 refusal leg is the Up+Left
    CHORD and whose p0.session names no contract at all; 2 is this build,
    whose refusal leg is an opener held past its 500 ms tap window. The two
    marker names are mutually exclusive, which is what stops an 800 ms hold
    refusal from ever being read as hardware chord proof."""
    rows, seq = [], 0
    wii = platform == 'wii'
    refusal = mod.CHAT_LOGIN_REFUSAL_MARKER[contract]
    if refused is None:
        refused = wii

    def emit(phase, marker, corr=0, **fields):
        nonlocal seq
        if (phase, marker) in skip:
            return 0
        last = 0
        for _ in range(2 if (phase, marker) == repeat else 1):
            seq += 1
            row = dict(event='chat_login', seq=seq, phase=phase, marker=marker,
                       ms=seq * 100, corr=seq if corr == 'self' else corr,
                       ok=True, run_id=run_id)
            row.update(fields)
            rows.append(row)
            last = seq
        return last

    def trace(records, leg, phase='p2'):
        # Section 12: every batch names its leg, because each leg restarts `t`
        # at its own first press.
        for i in range(0, len(records), 16):
            batch = records[i:i + 16]
            emit(phase, 'trace', leg=leg, frames=encode(batch), n=len(batch),
                 first_t=batch[0]['t'])

    session = dict(epoch=4, x=8.5, y=65.6, z=8.5, platform=platform,
                   username=mod.chat_login_username(run_id))
    if contract != mod.CHAT_LOGIN_CONTRACT_DEFAULT:
        # An old run directory carries no `contract` key at all -- that
        # ABSENCE is what dates it, so contract 1 must not synthesize one.
        session['contract'] = contract
    emit('p0', 'session', **session)
    kind, channel = mod.CHAT_LOGIN_BINDING[platform]
    emit('p1', 'pad_bound', bound=True, kind=kind, channel=channel,
         category=(mod.CHAT_LOGIN_CATEGORY_NUNCHUK if wii else 4))
    # Section 11 M4: refused = chat_pressed && !screen_changed. The opener gate
    # is Wii-only, so on PC the composer legitimately opens.
    fields = dict(refused=refused, chat_pressed=True,
                  screen_changed=not refused)
    if contract >= 2:
        # Contract 2 publishes the OBSERVED hold, well past the 500 ms tap
        # window (review finding 2).
        fields['hold_ms'] = hold_ms
    emit('p1', refusal, **fields)
    emit('p1', 'chat_open', row=1, col=0, page=0, draft_len=0)
    records = hold if hold is not None else hold_leg()
    steps = [r['t'] for r in records if r['rep']]
    trace(records, 'hold')
    released = next((r['t'] for r in records if r.get('edge') == 'v'), 0)
    consumed = next((r['t'] for r in records
                     if r['t'] > released and not r['cd'] & 0b1111), released)
    # Section 11 M1/M2: every reported count is what the rows show, and
    # max_frame_ms is measured PER LEG (H1).
    emit('p2', 'hold_result', steps=len(steps),
         first_repeat_ms=steps[1] - steps[0],
         mean_repeat_ms=(steps[-1] - steps[1]) // max(1, len(steps) - 2),
         max_frame_ms=frame_ms, release_lag_ms=consumed - released,
         after_release_steps=0)
    trace(tap_leg(), 'tap')
    emit('p2', 'tap_result', steps=1, max_frame_ms=frame_ms)
    trace(page_leg(), 'page')
    emit('p2', 'page_hold', page_before=0, page_after=3, extra_steps=0,
         remaining_ms=250, max_frame_ms=frame_ms)
    # P3 narrates its own leg (section 9 ruling 6), and its batches straddle
    # p3.frozen exactly as a real driver's would.
    p3 = reconnect if reconnect is not None else reconnect_leg()
    trace(p3[:8], 'reconnect', phase='p3')
    # rearm_mask is the device's true readback and is REPORTED, never scored
    # (section 10 ruling 3), so the golden carries a PARTIAL mask on purpose.
    emit('p3', 'frozen', moves_while_down=0, rearm_mask=0b101, draft_len=0,
         row=1, col=0, page=3)
    trace(p3[8:], 'reconnect', phase='p3')
    emit('p3', 'rearmed', frames_blocked=7, max_frame_ms=frame_ms)
    emit('p4', 'origin', row=1, col=0, page=mod.CHAT_LOGIN_PAGE_LOWER)
    index, length = 0, 0
    for moves, row, col, page, char in (keys if keys is not None else ROUTE):
        for _ in range(moves):
            emit('p4', 'key', i=index, want_row=row, want_col=col,
                 want_page=page, row=row, col=col, page=page, draft_len=length,
                 ch='')
            index += 1
        length += 1
        emit('p4', 'key', i=index, want_row=row, want_col=col, want_page=page,
             row=row, col=col, page=page, draft_len=length, ch=char)
        index += 1
    emit('p4', 'draft', text=mod.CHAT_LOGIN_DRAFT, len=mod.CHAT_LOGIN_DRAFT_LEN)
    send = emit('p5', 'sent_requested', corr='self', lines_before=5,
                ring_head=0, ring_count=5)
    # draft_len is REPORTED, not scored (section 11 L2): the composer is closed
    # by now, so the golden carries a non-zero one on purpose.
    emit('p5', 'composer_cleared', open=False, screen_ingame=True, draft_len=3)
    emit('p5', 'lines_sent', before=5, after=6)
    emit('p6', 'ack', corr=send, ring_index=5, wait_ms=140,
         text='CaveWii fixture: login OK')
    emit('run', 'ok', max_frame_ms_run=run_frame_ms)
    return rows


def index_of(rows, key, occurrence=0):
    hits = [i for i, r in enumerate(rows) if (r['phase'], r['marker']) == key]
    return hits[occurrence]


def patched(key, occurrence=0, **fields):
    """The valid stream with one marker rewritten."""
    rows = stream()
    rows[index_of(rows, key, occurrence)].update(fields)
    return rows


def patched_trace(which, occurrence=0, **fields):
    """The valid stream with one trace batch of leg `which` rewritten
    (section 12)."""
    rows = stream()
    hits = [i for i, r in enumerate(rows)
            if r['marker'] == 'trace' and r.get('leg') == which]
    rows[hits[occurrence]] = {**rows[hits[occurrence]], **fields}
    return rows


def reordered(first, second):
    """Two markers delivered the other way round. Only their CONTENT swaps:
    seq stays gapless and each corr stays with its slot, so the stream is wrong
    in exactly one way -- the order."""
    rows = stream()
    i, j = index_of(rows, first), index_of(rows, second)
    fixed = ('event', 'seq', 'ms', 'corr', 'run_id')
    content = [{k: v for k, v in rows[n].items() if k not in fixed}
               for n in (i, j)]
    rows[i] = {**{k: rows[i][k] for k in fixed}, **content[1]}
    rows[j] = {**{k: rows[j][k] for k in fixed}, **content[0]}
    return rows


def verdict(rows, run_id='chat', done='ok', done_at=None):
    """The DEVICE half of the verdict: the marker contract alone. `done_at`
    delivers profile_done after that many markers instead of at the end, which
    is what a device claiming success early looks like."""
    evidence = mod.ChatLoginEvidence(run_id)
    for i, row in enumerate(rows):
        if done_at is not None and i == done_at:
            evidence.note_profile_done(done)
        evidence.note(row)
    if done is not None and done_at is None:
        evidence.note_profile_done(done)
    return evidence.complete_error()


def receipt_text(run_id, player=None, length=8, secret=mod.CHAT_LOGIN_SECRET,
                 copies=1, prefix=''):
    player = player or mod.chat_login_username(run_id)
    line = ('%s[login-gate] %s /login len=%d sha256=%s'
            % (prefix, player, length, hashlib.sha256(secret).hexdigest()))
    return ''.join('%s\n' % line for _ in range(copies))


# How mc173's tracing logger really writes it (measured on the first full PC
# run, pc_chat_login_20260917_214444): ISO timestamp + level, in ANSI colour.
# The refusal marker of the CURRENT contract: every rejection case below
# patches the leg by this key, so the day a contract 3 renames it again the
# cases follow the collector instead of drifting from it.
REFUSAL = ('p1', mod.CHAT_LOGIN_REFUSAL_MARKER[mod.CHAT_LOGIN_CONTRACT_CURRENT])


def renamed_refusal(contract, marker):
    """A stream of `contract` whose refusal leg wears the OTHER contract's
    marker name -- the confusion Astra ruling 2 D2 forbids."""
    rows = stream(contract=contract)
    rows[index_of(rows, ('p1', mod.CHAT_LOGIN_REFUSAL_MARKER[contract]))][
        'marker'] = marker
    return rows


MC173_PREFIX = '\x1b[2m2026-09-18T01:44:47.533115Z\x1b[0m \x1b[33m WARN\x1b[0m '


class ChatLoginEvidenceTest(unittest.TestCase):
    def test_marker_contract_versions(self):
        """Astra ruling 2 D2: P1's refusal leg stopped being a CHORD refusal
        (Up+Left) and became an OVERLONG-HOLD refusal when the opener moved to
        Remote button 2, so the marker was RENAMED rather than redefined --
        "an 800 ms refusal must never be presented as hardware chord proof".
        p0.session's `contract` integer dates the run, its absence means the
        old contract, and the two marker sets are mutually exclusive."""
        self.assertEqual(mod.CHAT_LOGIN_REFUSAL_MARKER,
                         {1: 'chord_refused', 2: 'hold_refused'})
        # An old run directory still passes, graded by contract 1.
        self.assertIsNone(verdict(stream(contract=1)))
        # ... and so does a run of this build, graded by contract 2.
        self.assertIsNone(verdict(stream()))
        self.assertEqual(mod.CHAT_LOGIN_CONTRACT_CURRENT, 2)
        # Neither can borrow the other's name for the same leg.
        self.assertIn('unexpected marker p1.hold_refused: not a marker of '
                      'contract 1',
                      verdict(renamed_refusal(1, 'hold_refused')))
        self.assertIn('unexpected marker p1.chord_refused: not a marker of '
                      'contract 2',
                      verdict(renamed_refusal(2, 'chord_refused')))
        # Contract 2's refusal leg must have OUTLASTED the tap window, and
        # must say by how much (review finding 2): a 450 ms "refusal" is the
        # race a load burst used to produce, and it proves nothing.
        self.assertIn('hold_ms=450 -- the opener was not held past the 500 ms '
                      'tap window',
                      verdict(stream(hold_ms=450)))
        self.assertIn('hold_ms=None is not an int',
                      verdict(patched(REFUSAL, hold_ms=None)))
        # ... and contract 1 never carried the field, so it is not demanded.
        self.assertIsNone(verdict(stream(contract=1)))
        # An unknown contract is a malformed session, never a default.
        self.assertIn('malformed p0.session: contract=3',
                      verdict(patched(('p0', 'session'), contract=3)))
        self.assertIn("malformed p0.session: contract='2'",
                      verdict(patched(('p0', 'session'), contract='2')))
        # The version key reaches the artifact, so a stored run says which
        # contract graded it.
        evidence = mod.ChatLoginEvidence('chat')
        for row in stream():
            evidence.note(row)
        evidence.note_profile_done('ok')
        self.assertEqual(evidence.summary()['contract'], 2)

    def test_golden_stream_passes(self):
        """Both platform goldens pass, and each one's PLATFORM-specific P1
        readings are required on wii and optional on pc (section 10)."""
        self.assertIsNone(verdict(stream()))
        self.assertIsNone(verdict(stream(platform='pc')))
        # The chord gate is PLATFORM_WII only: refused:false is a legitimate
        # PC reading and a hard failure on the console.
        self.assertIsNone(verdict(stream(platform='pc', refused=False)))
        self.assertIn('refused=False on wii',
                      verdict(stream(platform='wii', refused=False)))
        # ... and a pc run may not wear the real-Wii PASS wording (ruling 7).
        evidence = mod.ChatLoginEvidence('chat')
        for row in stream(platform='pc'):
            evidence.note(row)
        self.assertEqual(evidence.pass_label(), mod.CHAT_LOGIN_PC_PASS_LABEL)
        self.assertIn('HARDWARE-UNCONFIRMED', evidence.pass_label())
        self.assertEqual(evidence.summary()['platform'], 'pc')

    def test_profile_shape(self):
        """mp_chat_login_3m is an mp_connect-gated, marker-certified profile --
        NOT a split/anchor one, and not performance-certifying."""
        profile = mod.PROFILES[mod.CHAT_LOGIN_PROFILE]
        self.assertEqual(profile['duration'], 180)
        self.assertEqual(profile['scenario'], 'mp_connect')
        self.assertFalse(profile['expect_dump'])
        self.assertEqual(profile['tps_settle_ms'], 5000)
        for table in (mod.SPLIT_PROFILES, mod.ANCHOR_PROFILES,
                      mod.SPLIT_CERTIFYING, mod.SPLIT_GUEST,
                      mod.RELEASE_FRAME_PROFILES):
            self.assertNotIn(mod.CHAT_LOGIN_PROFILE, table)
        # ANCHOR_PROFILES is untouched by this profile's arrival: it is the
        # split set minus the FUNCTIONAL (scripted, anchor-less) profiles --
        # split_recovery_6m and, since push-0921 (6b), split_portal_quit_4m --
        # and split_regrow_8m (colcap-1006 R5 item 3), whose anchor moves on
        # purpose and is graded by RegrowEvidence instead.
        self.assertEqual(mod.ANCHOR_PROFILES,
                         tuple(p for p in mod.SPLIT_PROFILES
                               if p not in ('split_recovery_6m',
                                            'split_portal_quit_4m',
                                            'split_regrow_8m')))

    def test_per_leg_tolerance_and_release_attribution(self):
        """Section 11 H1 and M2, the two rules a run-wide number would break."""
        # H1: a 1200 ms frame SOMEWHERE in the run (a P0 world-load hitch, the
        # run-wide figure run.ok may carry) is reported and tolerated -- and it
        # must NOT widen any leg's window, so a 40 ms repeat inside a 20 ms-
        # frame leg still rejects.
        self.assertIsNone(verdict(stream(run_frame_ms=1200)))
        evidence = mod.ChatLoginEvidence('chat')
        for row in stream(run_frame_ms=1200):
            evidence.note(row)
        self.assertEqual(evidence.summary()['max_frame_ms_run'], 1200)
        self.assertEqual([leg['max_frame_ms']
                          for leg in evidence.summary()['legs']],
                         [MAX_FRAME_MS] * 4)
        self.assertIn('later repeat 2 after 40 ms',
                      verdict(stream(run_frame_ms=1200, hold=hold_leg(rate=40))))
        # ... and a leg whose OWN worst frame is over the bound is inconclusive,
        # never "tolerated": 300 ms cannot speak about a 250/80 ms cadence.
        self.assertIn('frame stall in trace leg hold',
                      verdict(stream(frame_ms=300)))
        # M2: a step REQUESTED-released but not yet CONSUMED-released is
        # production-correct and must pass; only a step after the consumed
        # release (cd cleared) fails.
        late_delivery = hold_leg()
        last = max(i for i, r in enumerate(late_delivery) if r['rep'] == 2)
        late_delivery[last - 2] = dict(late_delivery[last - 2], req=RIGHT,
                                       edge='v')
        self.assertIsNone(verdict(stream(hold=late_delivery)))
        consumed = hold_leg()
        consumed[last] = dict(consumed[last], cd=0)
        self.assertIn('while no direction was held',
                      verdict(stream(hold=consumed)))

    def test_heartbeat_floor_is_a_cadence(self):
        """The run ends on its last assertion, so the floor is beats per second
        of the driver's own clock -- a marker-less run keeps the default."""
        evidence = mod.ChatLoginEvidence('chat')
        self.assertEqual(evidence.heartbeat_floor(60), 60)
        evidence.note(dict(event='chat_login', seq=1, phase='p0',
                           marker='session', ms=34224, corr=0, ok=True,
                           epoch=1, x=0.5, y=6.62, z=0.5, run_id='chat'))
        self.assertEqual(evidence.heartbeat_floor(60), 27)

    def test_rejections(self):
        """Section 5's rejection set, each with its own reason string."""
        bad_rate = hold_leg(rate=40)  # a repeat at 40 ms of an 80 ms cadence
        two_steps = hold_leg()
        step = next(i for i, r in enumerate(two_steps) if r['rep'] == 2)
        two_steps.insert(step + 1, dict(two_steps[step]))  # same frame, again
        early_step = hold_leg()
        idle = next(i for i, r in enumerate(early_step)
                    if r['rep'] == 0 and r['rem'] > 0)
        early_step[idle] = dict(early_step[idle], rep=2)  # step while rem > 0
        # Section 9 ruling 5's decoded masks: a step after production consumed
        # the release, and a consumed directional press that did not step.
        late_step = hold_leg()
        late = next(i for i, r in enumerate(late_step) if r['rep'] == 2)
        late_step[late] = dict(late_step[late], cd=0)
        # The leg's OPENING consumed press that did not step. A LATER consumed
        # directional press legitimately moves nothing (the real PC run's page
        # leg does exactly that), so the rule is scoped to the edge frame.
        missed_edge = hold_leg()
        missed_edge[0] = dict(missed_edge[0], rep=0)
        bad_req = hold_leg()
        bad_req[idle] = dict(bad_req[idle], req='x')
        # A frozen owner that moved anyway, in P3's own leg.
        moved_frozen = reconnect_leg()
        gone = next(i for i, r in enumerate(moved_frozen)
                    if r['cd'] == 0 and r['t'])
        moved_frozen[gone] = dict(moved_frozen[gone], rep=2)
        # Section 11 L3: the re-armed press that was NOT preceded by a neutral
        # frame -- the row before it still holds the direction.
        no_neutral = reconnect_leg()
        rearmed = len(no_neutral) - 2
        no_neutral[rearmed - 1] = dict(no_neutral[rearmed - 1], cd=RIGHT)
        cases = [
            # -- no evidence / missing evidence -------------------------------
            ('no chat_login markers', verdict([])),
            ('missing marker p3.rearmed',
             verdict(stream(skip={('p3', 'rearmed')}))),
            ('p2.hold_result closed a leg with no trace frames',
             verdict(stream(skip={('p2', 'trace')}))),
            ('p3.rearmed closed a leg with no trace frames',
             verdict(stream(skip={('p3', 'trace')}))),
            ('missing marker p4.key: 52 of the 53',
             verdict(stream(keys=ROUTE[:-1]))),
            ('missing marker run.ok', verdict(stream(skip={('run', 'ok')}))),
            ('duplicate marker p4.key',
             verdict(stream(keys=ROUTE + [(0, 1, 1, 3, '2')]))),
            ('duplicate marker p6.ack',
             verdict(stream(repeat=('p6', 'ack')))),
            # -- ordering ------------------------------------------------------
            ('reordered markers: p5.composer_cleared arrived after '
             'p5.lines_sent',
             verdict(reordered(('p5', 'composer_cleared'),
                               ('p5', 'lines_sent')))),
            ('phases out of order: p4.key arrived after phase p5',
             verdict(reordered(('p4', 'origin'), ('p5', 'lines_sent')))),
            ('markers continued after run.ok',
             verdict(stream() + [dict(event='chat_login', seq=999, phase='p6',
                                      marker='ack', ms=1, corr=0, ok=True,
                                      run_id='chat')])),
            # trace batches are accepted in P2 and P3 only (ruling 6).
            ('unexpected marker p4.trace',
             verdict(patched(('p4', 'key'), marker='trace'))),
            ('unexpected marker p3.rearmed_soon',
             verdict(patched(('p3', 'rearmed'), marker='rearmed_soon'))),
            # -- seq -----------------------------------------------------------
            ('seq not strictly increasing',
             verdict(patched(('p4', 'draft'), seq=2))),
            ('seq gap', verdict(patched(('p4', 'draft'), seq=999))),
            # -- device-side failure and premature success ---------------------
            ('phase failure p5.lines_sent: send refused',
             verdict(patched(('p5', 'lines_sent'), ok=False,
                             diag='send refused'))),
            ('phase failure p4.fail: focus off route',
             verdict(patched(('p4', 'draft'), marker='fail', ok=False,
                             diag='focus off route'))),
            ('run.timeout: p6:no ack within 25 s',
             verdict(patched(('run', 'ok'), marker='timeout', ok=False,
                             diag='p6:no ack within 25 s'))),
            ('premature success: profile_done "ok" arrived before run.ok',
             verdict(stream(), done_at=len(stream()) - 1)),
            ('profile_done status=\'p6:no ack\'',
             verdict(stream(), done='p6:no ack')),
            ('profile_done was never received', verdict(stream(), done=None)),
            # -- the draft and the single submission ---------------------------
            ('the composed draft is \'/login hunter2\'',
             verdict(patched(('p4', 'draft'), text='/login hunter2'))),
            ('the composed draft reports len=14',
             verdict(patched(('p4', 'draft'), len=14))),
            ('p5.lines_sent after=7 is not before+1',
             verdict(patched(('p5', 'lines_sent'), after=7))),
            ('p5.lines_sent after=5 is not before+1',
             verdict(patched(('p5', 'lines_sent'), after=5))),
            # -- cadence (section 3's trace rules) -----------------------------
            ('cadence violation in leg hold: later repeat 2 after 40 ms',
             verdict(stream(hold=bad_rate))),
            ('cadence violation: two steps in one frame',
             verdict(stream(hold=two_steps))),
            ('while the repeat timer still had rem=',
             verdict(stream(hold=early_step))),
            ('while no direction was held (cd=0)',
             verdict(stream(hold=late_step))),
            ('production consumed RIGHT at t=',
             verdict(stream(hold=missed_edge))),
            ('malformed trace tuple', verdict(stream(hold=bad_req))),
            ('cadence violation: a step at t=60 while no direction was held',
             verdict(stream(reconnect=moved_frozen))),
            # -- section 12: every batch names its leg ------------------------
            ('malformed p2.trace batch: leg=None',
             verdict(patched_trace('hold', leg=None))),
            ('malformed p2.trace batch: leg=\'reconnect\'',
             verdict(patched_trace('tap', leg='reconnect'))),
            ('malformed p3.trace batch: leg=\'hold\'',
             verdict(patched_trace('reconnect', leg='hold'))),
            ('trace legs interleaved: a \'hold\' batch arrived while leg '
             '\'tap\' was still open',
             verdict(patched_trace('hold', leg='tap'))),
            ('trace batch for leg \'hold\' arrived after its own result marker',
             verdict(patched_trace('tap', leg='hold'))),
            # -- section 11's per-leg tolerance and rows cross-checks ----------
            ('frame stall in trace leg hold: max_frame_ms=300 ms',
             verdict(patched(('p2', 'hold_result'), max_frame_ms=300))),
            ('frame stall in trace leg reconnect: max_frame_ms=1200 ms',
             verdict(patched(('p3', 'rearmed'), max_frame_ms=1200))),
            ('malformed p2.tap_result: max_frame_ms=None',
             verdict(patched(('p2', 'tap_result'), max_frame_ms=None))),
            ('p2.hold_result steps=7, but its trace rows show 6',
             verdict(patched(('p2', 'hold_result'), steps=7))),
            ('p2.hold_result after_release_steps=2, but its trace rows show 0',
             verdict(patched(('p2', 'hold_result'), after_release_steps=2))),
            ('p3.rearmed -- the first post-reconnect step at t=',
             verdict(stream(reconnect=no_neutral))),
            ('p1.hold_refused chat_pressed=False',
             verdict(patched(REFUSAL, chat_pressed=False))),
            ('section 11 M4 defines refused as chat_pressed && !screen_changed',
             verdict(patched(REFUSAL, screen_changed=True))),
            ('p5.composer_cleared screen_ingame=False',
             verdict(patched(('p5', 'composer_cleared'), screen_ingame=False))),
            ('run id missing', verdict(stream(), run_id='')),
            ('cadence violation: p2.hold_result first_repeat_ms=140',
             verdict(patched(('p2', 'hold_result'), first_repeat_ms=140))),
            ('cadence violation: p2.page_hold remaining_ms=80',
             verdict(patched(('p2', 'page_hold'), remaining_ms=80))),
            ('p2.hold_result steps=4',
             verdict(patched(('p2', 'hold_result'), steps=4))),
            ('p2.tap_result steps=3',
             verdict(patched(('p2', 'tap_result'), steps=3))),
            ('p2.page_hold extra_steps=1',
             verdict(patched(('p2', 'page_hold'), extra_steps=1))),
            # -- the ack, its correlation and its ring slot --------------------
            ('stale ack: p6.ack ring_index=3 was already occupied at send time',
             verdict(patched(('p6', 'ack'), ring_index=3))),
            ('p6.ack corr=1 does not answer p5.sent_requested',
             verdict(patched(('p6', 'ack'), corr=1))),
            ('p5.sent_requested corr=1 must be its own seq',
             verdict(patched(('p5', 'sent_requested'), corr=1))),
            ('the send-time chat ring snapshot',
             verdict(patched(('p5', 'sent_requested'), ring_head=None))),
            # -- the rest of section 3's fixed readings ------------------------
            ('p1.pad_bound observed (kind,channel)=(2, 0)',
             verdict(patched(('p1', 'pad_bound'), channel=0))),
            ('p1.pad_bound observed (kind,channel)=(1, 3)',
             verdict(patched(('p1', 'pad_bound'),
                             kind=mod.CHAT_LOGIN_KIND_KEYBOARD))),
            ('p1.pad_bound category=0',
             verdict(patched(('p1', 'pad_bound'), category=0))),
            ('p1.hold_refused refused=False on wii',
             verdict(patched(REFUSAL, refused=False))),
            ('malformed p1.hold_refused: refused=None',
             verdict(patched(REFUSAL, refused=None))),
            ('malformed p0.session: platform=\'switch\'',
             verdict(patched(('p0', 'session'), platform='switch'))),
            ('p3.frozen moves_while_down=2',
             verdict(patched(('p3', 'frozen'), moves_while_down=2))),
            # Section 10 ruling 1: the controller-recovery modal opening for
            # player 0 fails its phase through the ordinary ok:false path, with
            # the device's own diag carried verbatim.
            ('phase failure p1.chat_open: recovery modal',
             verdict(patched(('p1', 'chat_open'), ok=False,
                             diag=mod.CHAT_LOGIN_RECOVERY_MODAL_DIAG))),
            ('phase failure p3.frozen: recovery modal',
             verdict(patched(('p3', 'frozen'), ok=False,
                             diag=mod.CHAT_LOGIN_RECOVERY_MODAL_DIAG))),
            ('p3.rearmed frames_blocked=0',
             verdict(patched(('p3', 'rearmed'), frames_blocked=0))),
            ('p4.origin (row,col,page)=(1, 3, 0)',
             verdict(patched(('p4', 'origin'), col=3))),
            ('p4.key i=5 landed on',
             verdict(patched(('p4', 'key'), occurrence=5, row=9))),
            ('reordered markers: p4.key i=9',
             verdict(patched(('p4', 'key'), occurrence=3, i=9))),
            # -- the run nonce (section 9 ruling 7) -----------------------------
            ('stale-run evidence: p0.session username=\'cwdeadbe\'',
             verdict(patched(('p0', 'session'), username='cwdeadbe'))),
            ('malformed p0.session: username=None',
             verdict(patched(('p0', 'session'), username=None))),
            ('malformed p0.session: username=\'wii12345\'',
             verdict(patched(('p0', 'session'), username='wii12345'))),
            # -- stale run -----------------------------------------------------
            ('stale-run evidence: marker run_id=\'another_run\'',
             verdict(patched(('p6', 'ack'), run_id='another_run'))),
            ('malformed chat_login marker',
             verdict(patched(('p0', 'session'), ok='yes'))),
        ]
        for expected, actual in cases:
            with self.subTest(expected=expected):
                self.assertIsNotNone(actual, expected)
                self.assertIn(expected, actual)
        # Every case must be rejected for its OWN reason, never a shared one.
        self.assertEqual(len({actual for _e, actual in cases}), len(cases))

    def test_receipt(self):
        """Section 6: exactly one [login-gate] line, this run's player, len=8,
        and the SHA-256 of the bytes the console typed."""
        run_id = '0917_101500'
        # Ruling 7's derivation, byte-identical to the rig scripts' PowerShell
        # copy (proved against it in test_rig_username_derivation below).
        self.assertEqual(
            mod.chat_login_username(run_id),
            'cw' + hashlib.sha256(run_id.encode()).hexdigest()[:6])
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            path = out / mod.CHAT_LOGIN_RECEIPT_NAME
            evidence = mod.ChatLoginEvidence(run_id)
            self.assertIn('is missing from the run directory',
                          evidence.receipt_error(out))
            for text, expected in (
                    (receipt_text(run_id), None),
                    # The real fixture line: a timestamp, a level and ANSI
                    # colour in front of the receipt. The RECEIPT is still
                    # matched exactly; the log prefix is tolerated.
                    (receipt_text(run_id, prefix=MC173_PREFIX), None),
                    (receipt_text(run_id, prefix=MC173_PREFIX, copies=2),
                     'carries 2 [login-gate] lines'),
                    (receipt_text(run_id, copies=2),
                     'carries 2 [login-gate] lines'),
                    (receipt_text(run_id, copies=0),
                     'carries 0 [login-gate] lines'),
                    (receipt_text(run_id, player='cwdeadbe'),
                     "player 'cwdeadbe' is not this run's username"),
                    (receipt_text(run_id, player='someone'),
                     "player 'someone' is not a run nonce"),
                    (receipt_text(run_id, secret=b'hunter23'),
                     'does not hash the argument the console typed'),
                    (receipt_text(run_id, length=7), 'reports len=7'),
                    ('[login-gate] cw091710 /login hunter22\n',
                     "does not match section 6's syntax")):
                with self.subTest(text=text[:60]):
                    path.write_text(text, encoding='utf-8')
                    problem = evidence.receipt_error(out)
                    if expected is None:
                        self.assertIsNone(problem)
                        self.assertTrue(evidence.receipt['ok'])
                        self.assertEqual(
                            evidence.receipt['sha256'],
                            hashlib.sha256(b'hunter22').hexdigest())
                    else:
                        self.assertIn(expected, problem)
                        self.assertFalse(evidence.receipt['ok'])

    def collect(self, td, name, rows, receipt='', done='ok'):
        """The same streams through the real Collector, not just the evidence
        class: process() -> _finish_normal() -> write_result()."""
        collector = mod.Collector(mod._selftest_args(
            Path(td) / name, name, mod.CHAT_LOGIN_PROFILE))
        collector.hb_count = 60
        collector.world_loaded_seen = True
        if receipt:
            (collector.out / mod.CHAT_LOGIN_RECEIPT_NAME).write_text(
                receipt, encoding='utf-8')
        for row in rows:
            collector.process({**row, 'run_id': name})
        collector.process(dict(event='profile_done', run_id=name, status=done))
        return collector

    def test_collector_pass_and_failures(self):
        with tempfile.TemporaryDirectory() as td:
            # 1. The whole contract, device + receipt: PASS.
            name = '0917_110001'
            good = self.collect(td, name, stream(run_id=name),
                                receipt=receipt_text(name))
            self.assertTrue(good.done)
            self.assertIsNone(good.fail_reason, good.fail_reason)
            self.assertEqual(good.write_result(), 0)
            proof = json.loads((good.out / 'PASS.json').read_text())
            self.assertIn(mod.CHAT_LOGIN_PASS_LABEL, proof['reason'])
            self.assertEqual(proof['certification_scope'],
                             'chat_login_scripted_osk_delivery')
            evidence = proof['chat_login']
            self.assertTrue(evidence['device_complete'])
            self.assertTrue(evidence['receipt']['ok'])
            self.assertEqual(evidence['draft'], mod.CHAT_LOGIN_DRAFT)
            self.assertEqual(evidence['keys'], mod.CHAT_LOGIN_KEY_PRESSES)
            self.assertEqual(evidence['keys'], 53)
            self.assertEqual(evidence['lines_sent'], [5, 6])
            # Reported from the rows, never scored (section 11 M2).
            self.assertEqual(evidence['release_lag_ms'], 20)
            self.assertEqual(evidence['legs'][0]['steps_after_consumed_release'],
                             0)
            self.assertEqual(evidence['username'],
                             mod.chat_login_username(name))
            self.assertEqual(evidence['receipt']['player'],
                             evidence['username'])
            self.assertEqual([leg['leg'] for leg in evidence['legs']],
                             ['hold', 'tap', 'page', 'reconnect'])
            self.assertEqual(evidence['platform'], 'wii')
            # Reported, never scored (section 10 ruling 3).
            self.assertEqual(evidence['frozen_rearm_mask'], 0b101)
            # Release attribution is REPORTED per leg, never scored.
            self.assertEqual(evidence['legs'][0]['observed_release_lag_ms'], 20)

            # 1b. The same run on PC: it PASSES, and its artifact says it is a
            #     PC proof -- never the real-Wii claim (section 10 ruling 7).
            name = '0917_110009'
            pc = self.collect(td, name, stream(run_id=name, platform='pc'),
                              receipt=receipt_text(name))
            self.assertIsNone(pc.fail_reason, pc.fail_reason)
            self.assertEqual(pc.write_result(), 0)
            proof = json.loads((pc.out / 'PASS.json').read_text())
            self.assertIn(mod.CHAT_LOGIN_PC_PASS_LABEL, proof['reason'])
            self.assertNotIn(mod.CHAT_LOGIN_PASS_LABEL, proof['reason'])
            self.assertEqual(proof['chat_login']['platform'], 'pc')
            self.assertEqual(proof['chat_login']['proof_scope'],
                             mod.CHAT_LOGIN_PC_PASS_LABEL)

            # 2. DEVICE PASS, receipt MISSING: the device half is reported as
            #    complete and the run still FAILS (section 5's two gates).
            name = '0917_110002'
            missing = self.collect(td, name, stream(run_id=name))
            self.assertEqual(missing.failure_class, 'chat_login_receipt_failed')
            self.assertIn('is missing from the run directory',
                          missing.fail_reason)
            self.assertEqual(missing.write_result(), 1)
            proof = json.loads((missing.out / 'FAIL.json').read_text())
            self.assertTrue(proof['chat_login']['device_complete'])
            self.assertIsNone(proof['chat_login']['device_error'])
            self.assertNotIn(mod.CHAT_LOGIN_PASS_LABEL, proof['reason'])

            # 3. A duplicate receipt for a perfect device run.
            name = '0917_110003'
            dupe = self.collect(td, name, stream(run_id=name),
                                receipt=receipt_text(name, copies=2))
            self.assertEqual(dupe.failure_class, 'chat_login_receipt_failed')
            self.assertIn('carries 2 [login-gate] lines', dupe.fail_reason)

            # 3b. A well-formed receipt written for ANOTHER run (ruling 7: the
            #     player binds the receipt to one run id, nothing else does).
            name = '0917_110008'
            other = self.collect(td, name, stream(run_id=name),
                                 receipt=receipt_text('0917_999999'))
            self.assertEqual(other.failure_class, 'chat_login_receipt_failed')
            self.assertIn("the receipt belongs to another run",
                          other.fail_reason)

            # 4. A device stream that never acked, with a valid receipt: the
            #    marker contract still fails the run.
            name = '0917_110004'
            stale = self.collect(
                td, name,
                [dict(row, ring_index=3) if (row['phase'], row['marker'])
                 == ('p6', 'ack') else row for row in stream(run_id=name)],
                receipt=receipt_text(name))
            self.assertEqual(stale.failure_class, 'chat_login_evidence_failed')
            self.assertIn('stale ack', stale.fail_reason)

            # 5. A clean console that never ran the driver: heartbeat evidence
            #    alone must NEVER certify this profile.
            name = '0917_110005'
            silent = self.collect(td, name, [], receipt=receipt_text(name))
            self.assertEqual(silent.failure_class, 'chat_login_evidence_failed')
            self.assertIn('no chat_login markers', silent.fail_reason)

            # 6. Nor may the clock: the duration completion reaches the same
            #    marker gate, so a silent 180 s cannot pass either.
            name = '0917_110006'
            timed = mod.Collector(mod._selftest_args(
                Path(td) / name, name, mod.CHAT_LOGIN_PROFILE, duration=180))
            timed.hb_count = 60
            timed.world_loaded_seen = True
            timed.process(dict(event='heartbeat', load_screen_ms=3000, run_id=name, tick=1))
            timed.started -= 181
            timed.tick()
            self.assertTrue(timed.done)
            self.assertEqual(timed.failure_class, 'chat_login_evidence_failed')

            # 7. The heartbeat floor is the CADENCE, not the table's 60: this
            #    stream's own clock is short, so a fast honest run passes.
            name = '0917_110007'
            fast = self.collect(td, name, stream(run_id=name),
                                receipt=receipt_text(name))
            self.assertLess(fast.chat_login.heartbeat_floor(60), 60)
            self.assertIsNone(fast.fail_reason)

    def test_other_profiles_ignore_the_markers(self):
        """A chat_login record on another profile is recorded and ignored, and
        no other profile grows a chat-login gate."""
        with tempfile.TemporaryDirectory() as td:
            name = 'mp_smoke'
            collector = mod.Collector(mod._selftest_args(
                Path(td) / name, name, 'mp_connect_smoke'))
            collector.hb_count = 100
            collector.world_loaded_seen = True
            collector.join_ms = 3000
            for row in stream(run_id=name):
                collector.process({**row, 'run_id': name})
            collector.process(dict(event='profile_done', run_id=name,
                                   status='ok'))
            self.assertEqual(collector.chat_login.markers, 0)
            self.assertIsNone(collector.fail_reason)
            self.assertEqual(collector.write_result(), 0)
            proof = json.loads((collector.out / 'PASS.json').read_text())
            self.assertIsNone(proof['chat_login'])
            lines = (collector.out / 'telemetry.ndjson').read_text().splitlines()
            self.assertTrue(any('"chat_login"' in line for line in lines))

    def test_cli_accepts_the_profile(self):
        """argparse's choices follow PROFILES, so the new name is selectable
        and a typo still is not."""
        self.assertIn(mod.CHAT_LOGIN_PROFILE, sorted(mod.PROFILES))
        script = str(ROOT / 'tools' / 'wii_telemetry_server.py')
        known = subprocess.run(
            [sys.executable, script, '--profile', mod.CHAT_LOGIN_PROFILE,
             '--release-gate'], capture_output=True, text=True, timeout=60)
        self.assertEqual(known.returncode, 2)
        self.assertIn('--release-gate requires', known.stderr)
        typo = subprocess.run(
            [sys.executable, script, '--profile', 'mp_chat_login_3',
             '--release-gate'], capture_output=True, text=True, timeout=60)
        self.assertEqual(typo.returncode, 2)
        self.assertIn('invalid choice', typo.stderr)

    def test_rig_scripts_accept_the_profile(self):
        """The five rig entry points that gate on a profile name (contract
        section 7, brief 2's list), plus the two that carry the run's clock and
        its nonce (section 9 ruling 7)."""
        for name in ('run_wii_hardware.ps1', 'run_staged_wii_hardware.ps1',
                     'collect_staged_wii.ps1', 'stage_wii_dol.ps1',
                     'validate_wii_pass.ps1'):
            with self.subTest(script=name):
                text = (ROOT / 'tools' / name).read_text(encoding='utf-8',
                                                         errors='replace')
                validate = [line for line in text.splitlines()
                            if 'ValidateSet(' in line]
                self.assertTrue(validate, name)
                self.assertTrue(
                    any('"%s"' % mod.CHAT_LOGIN_PROFILE in line
                        for line in validate), name)
        for name in ('run_wii_hardware.ps1', 'stage_wii_dol.ps1'):
            with self.subTest(script=name, rule='duration+username'):
                text = (ROOT / 'tools' / name).read_text(encoding='utf-8',
                                                         errors='replace')
                self.assertIn('"%s" { %d }' % (mod.CHAT_LOGIN_PROFILE,
                                               mod.PROFILES[mod.CHAT_LOGIN_PROFILE]['duration']),
                              text)
                self.assertIn('--cw-mp-username', text)
                self.assertIn('function Get-ChatLoginUsername', text)

    @unittest.skipUnless(shutil.which('powershell'),
                         'powershell.exe is not available on this host')
    def test_rig_username_derivation_matches(self):
        """The rig's PowerShell derivation and the collector's Python one must
        agree byte for byte -- a mismatch would FAIL every receipt (ruling 7).
        Proved by running the .ps1 function itself, not a transcription."""
        for name in ('run_wii_hardware.ps1', 'stage_wii_dol.ps1'):
            text = (ROOT / 'tools' / name).read_text(encoding='utf-8',
                                                     errors='replace')
            start = text.index('function Get-ChatLoginUsername')
            snippet = text[start:text.index('\n}', start) + 2]
            if name == 'stage_wii_dol.ps1':  # it reuses the sha256 helper
                helper = text.index('function Get-BytesSha256Hex')
                snippet = text[helper:text.index('\n}', helper) + 2] + snippet
            for run_id in ('0917_101500', 'wii_mp_chat_login_3m_20260917_2200',
                           ''):
                with self.subTest(script=name, run_id=run_id):
                    result = subprocess.run(
                        ['powershell', '-NoProfile', '-NonInteractive',
                         '-Command',
                         "%s\nGet-ChatLoginUsername -Name '%s'"
                         % (snippet, run_id)],
                        capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.strip(),
                                     mod.chat_login_username(run_id))


if __name__ == '__main__':
    unittest.main()
