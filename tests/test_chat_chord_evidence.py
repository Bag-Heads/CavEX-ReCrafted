"""mp_chat_chord_1m's collector verdict, replayed OFFLINE -- no Wii, no server.

Astra ruling 2 D2 asked for the debug-chord leg to live in its OWN short
profile, because the login profile's P1 leg now refuses an over-long opener
HOLD and "an 800 ms refusal must never be presented as hardware chord proof".
This suite grades `tools/wii_telemetry_server.py`'s ChatChordEvidence the way
tests/test_chat_login_evidence.py grades ChatLoginEvidence: the valid marker
stream is built ONCE and every rejection case is that same stream with exactly
one thing broken, so a case can never pass for a reason the valid stream does
not also have. Written BEFORE any Wii run.

WHAT THE PROFILE CLAIMS, and nothing more (contract 103, brief 12 D-1): from a
KNOWN overlay state (forced off), the chord pressed in the RULED order (hold
Minus, then press Plus) made the overlay ACTION fire, the WRONG order (Plus
first) did NOT, neither order opened chat OR moved the hotbar, after a neutral
re-arm the ordinary opener still opened the composer -- which was closed again
with the overlay in the state that ONE legal toggle implies -- and a SECOND
correct-order chord then fired exactly once and put the overlay back OFF.

Contract 101 required BOTH orders to fire; it never produced a hardware run,
but because the meaning of p2.chord order 0 inverted, the number is spent
rather than reused. Contract 102 kept 101's "p3.closed.overlay must be false"
after making order 0 a negative leg, so with one legal toggle it could NEVER
pass on hardware -- the real-Wii run mp_chat_chord_1m_db01e56_0919_173900
failed on exactly that check with every product assertion holding. It is spent
too, and refused by number here.

The virtual pad always presents INPUT_CAT_NUNCHUK, so a scripted run exercises
the REMOTE family; the CLASSIC shared-button case (Plus is both the opener and
a chord half there) is covered by tests/test_input_wii_adapter.c and stays owed
to physical acceptance, which is why the PASS label says so.

NOT PROVEN HERE (stated rather than implied): this is the collector's half.
That the device really emits these markers in this order is
tests/test_chat_login_script.c's and the hardware run's job -- the same split
the chat-login replay makes.
"""
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
_spec = importlib.util.spec_from_file_location(
    'wii_telemetry_server', ROOT / 'tools' / 'wii_telemetry_server.py')
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)


def fired_for(order, platform='wii'):
    """What the contract expects `fired` to be for one press order. Order 0 is
    the WRONG order (Plus first) and must never toggle the overlay, on any
    platform. Order 1 is the ruled one (hold Minus, then press Plus) and must
    toggle it on the device; the PC config binds a single key rather than a
    chord, so a PC run legitimately records false there too."""
    return order == 1 and platform == 'wii'


def stream(run_id='chord', platform='wii', skip=(), repeat=None, orders=(0, 1),
           fired=None, contract=mod.CHAT_CHORD_CONTRACT_CURRENT, slots=None,
           restore=None, end_overlay=None, closed_overlay=None, bound=None):
    """The one valid marker stream, in its emission order. `platform` switches
    the single platform-specific reading (see fired_for); the "a chord never
    opens chat" and "a chord never moves the hotbar" halves are asserted on
    both. `fired` forces the same value onto BOTH orders, which is how the
    order-specific rejections are built. `restore`/`end_overlay`/
    `closed_overlay` break the contract-103 restore leg the same way."""
    rows, seq = [], 0
    wii = platform == 'wii'
    if slots is None:
        slots = (4, 4)
    # The ruled order fires only on a device that binds the chord, so that is
    # also what the restore leg must do and what the overlay reads between the
    # two: ON after P2's single legal toggle, OFF once P4 spends the second.
    ruled = fired_for(1, platform) if fired is None else fired
    if restore is None:
        restore = ruled
    if closed_overlay is None:
        closed_overlay = ruled
    if end_overlay is None:
        end_overlay = False

    def emit(phase, marker, **fields):
        nonlocal seq
        if (phase, marker) in skip:
            return
        for _ in range(2 if (phase, marker) == repeat else 1):
            seq += 1
            row = dict(event=mod.CHAT_CHORD_EVENT, seq=seq, phase=phase,
                       marker=marker, ms=seq * 100, corr=0, ok=True,
                       run_id=run_id)
            row.update(fields)
            rows.append(row)

    emit('p0', 'session', platform=platform, contract=contract, overlay=False,
         screen=3)
    # Contract 104 (C-1): the E-2 binding gate is its own marker and it lands
    # BEFORE pad_bound. It cannot live on p0.session, which is published
    # before anything owns player 0 and therefore before the device can have
    # scanned its own binding table at all.
    emit('p1', 'chord_bound', bound=wii if bound is None else bound)
    kind, channel = mod.CHAT_LOGIN_BINDING[platform]
    emit('p1', 'pad_bound', bound=True, kind=kind, channel=channel,
         category=(mod.CHAT_LOGIN_CATEGORY_NUNCHUK if wii else 4))
    # Both press orders, each with its own neutral gap: order 0 presses the
    # Plus half first, order 1 the Minus half first.
    for order in orders:
        shot = fired_for(order, platform) if fired is None else fired
        # Contract 104 (hostile review 17, M-1): p2 COUNTS its fires the way
        # p4.chord_restore already does, and the overlay reading beside it is
        # what that count did to the console -- ON only where the ruled order
        # really toggled it.
        emit('p2', 'chord', order=order, fired=shot,
             fires=(1 if shot else 0), chat_opened=False,
             slot_before=slots[0], slot_after=slots[1],
             overlay=bool(shot))
    emit('p3', 'opened', opened=True, overlay=closed_overlay)
    emit('p3', 'closed', closed=True, overlay=closed_overlay,
         ruled_fired=ruled)
    # Contract 103: the RESTORE leg. One more correct-order chord, firing
    # exactly once, leaving the console's overlay OFF.
    emit('p4', 'chord_restore', fired=restore, fires=1 if restore else 0,
         chat_opened=False, slot_before=slots[0], slot_after=slots[1],
         overlay=end_overlay)
    emit('run', 'ok', seq_total=seq + 1, max_frame_ms_run=20)
    return rows


def index_of(rows, key, occurrence=0):
    hits = [i for i, r in enumerate(rows) if (r['phase'], r['marker']) == key]
    return hits[occurrence]


def patched(key, occurrence=0, **fields):
    rows = stream()
    rows[index_of(rows, key, occurrence)].update(fields)
    return rows


def verdict(rows, run_id='chord', done='ok', done_at=None):
    evidence = mod.ChatChordEvidence(run_id)
    for i, row in enumerate(rows):
        if done_at is not None and i == done_at:
            evidence.note_profile_done(done)
        evidence.note(row)
    if done is not None and done_at is None:
        evidence.note_profile_done(done)
    return evidence.complete_error()


class ChatChordEvidenceTest(unittest.TestCase):
    def test_heartbeat_floor_fits_a_six_second_run(self):
        # Real Wii, 2026-09-19, build 0c5a7d8_09.19, the FIRST hardware run of
        # contract 104: every marker green (chord_bound, fired [False, True],
        # overlay off -> on -> off, restored) with the driver clock ending at
        # 5821 ms and 7 one-per-second heartbeats -- and the run was FAILED
        # "only 7 heartbeats; need 10", because the floor copied the login
        # profile's "never fewer than 10". This profile is over in about six
        # seconds of the driver's own clock, so ten beats can never exist. The
        # question is still CADENCE (0.8 per second of that clock); the fixed
        # part only has to prove the stream is alive at all.
        evidence = mod.ChatChordEvidence('chord')
        self.assertEqual(evidence.heartbeat_floor(30), 30)  # no marker yet
        for row in stream():
            evidence.note(row)
        evidence.last_ms = 5821
        self.assertEqual(evidence.heartbeat_floor(30), 4)
        self.assertLessEqual(evidence.heartbeat_floor(30), 7)
        # A dead stream is still refused: three beats is the least that shows
        # a cadence at all, however short the run.
        evidence.last_ms = 900
        self.assertEqual(evidence.heartbeat_floor(30), 3)
        # ... and a run that really lasts a minute is held to a minute's worth.
        evidence.last_ms = 60000
        self.assertEqual(evidence.heartbeat_floor(30), 48)

    def test_golden_stream_passes(self):
        self.assertIsNone(verdict(stream()))
        # A PC run passes too, and wears a label that cannot be mistaken for
        # the console one.
        self.assertIsNone(verdict(stream(platform='pc')))
        evidence = mod.ChatChordEvidence('chord')
        for row in stream(platform='pc'):
            evidence.note(row)
        evidence.note_profile_done('ok')
        self.assertIn('scripted PC (NOT hardware)', evidence.pass_label())
        self.assertIn('scripted real-Wii', mod.CHAT_CHORD_PASS_LABEL)
        # ... and the label never lets a PASS imply the Classic case.
        self.assertIn('CLASSIC shared-button case', mod.CHAT_CHORD_PASS_LABEL)

    def test_contract_and_summary(self):
        """The version key exists from day one, so a contract 2 can be told
        from a contract 1 the way the login profile already does."""
        # Finding 4: DISJOINT from the login series, so a PASS.json that
        # says contract 104 can only be a chord run.
        self.assertEqual(mod.CHAT_CHORD_CONTRACT_CURRENT, 104)
        for n in (1, 2, mod.CHAT_LOGIN_CONTRACT_CURRENT):
            self.assertIn('malformed p0.session: contract=%d' % n,
                          verdict(patched(('p0', 'session'), contract=n)))
        self.assertIn('malformed p0.session: contract=None',
                      verdict(patched(('p0', 'session'), contract=None)))
        # Brief 07 A: 101 is SPENT, not merely superseded. Order 0 used to
        # mean "this order fired too" and now means "this order must not
        # fire", so an old stream graded by the new tables would invert a
        # claim. It is refused by number, with the reason in the message.
        self.assertNotIn(101, mod.CHAT_CHORD_CONTRACTS)
        self.assertIn('malformed p0.session: contract=101',
                      verdict(patched(('p0', 'session'), contract=101)))
        # Brief 12 D-1: 102 is spent for a harder reason -- it is UNPASSABLE
        # on hardware. It demanded "p3.closed.overlay false" after one legal
        # toggle had turned the overlay on, which is exactly how the real run
        # mp_chat_chord_1m_db01e56_0919_173900 failed. Refused by number.
        self.assertNotIn(102, mod.CHAT_CHORD_CONTRACTS)
        self.assertIn('malformed p0.session: contract=102',
                      verdict(patched(('p0', 'session'), contract=102)))
        self.assertIn('unpassable', mod.CHAT_CHORD_SPENT[102])
        # Hostile review 17, M-1: 103 -> 104. p2.chord gained a `fires` COUNT
        # beside its bool, so a 103 stream carries strictly less evidence than
        # these tables now grade -- and 103 never produced a hardware run
        # either (C-1: the E-2 gate it shipped with refused the profile on
        # every card), so the number is spent rather than merely superseded.
        self.assertNotIn(103, mod.CHAT_CHORD_CONTRACTS)
        self.assertIn('malformed p0.session: contract=103',
                      verdict(patched(('p0', 'session'), contract=103)))
        self.assertIn('never ran on hardware', mod.CHAT_CHORD_SPENT[103])
        evidence = mod.ChatChordEvidence('chord')
        for row in stream():
            evidence.note(row)
        evidence.note_profile_done('ok')
        summary = evidence.summary()
        self.assertEqual(summary['contract'], 104)
        self.assertEqual(summary['profile'], mod.CHAT_CHORD_PROFILE)
        self.assertEqual(summary['contract_event'], mod.CHAT_CHORD_EVENT)
        self.assertEqual(summary['orders'], [0, 1])
        # The order rule, read straight off the summary: the wrong order did
        # not toggle, the ruled one did.
        self.assertEqual(summary['fired'], [False, True])
        self.assertEqual(summary['slots'], [[4, 4], [4, 4]])
        self.assertIs(summary['overlay_at_start'], False)
        # The console was left as the run found it -- but only because the
        # RESTORE leg spent the second toggle; between P2 and P4 it was on.
        self.assertIs(summary['overlay_at_end'], False)
        self.assertIs(summary['overlay_after_chord'], True)
        self.assertIs(summary['restored'], True)
        self.assertTrue(summary['device_complete'])

    def test_cross_profile_grading_is_impossible(self):
        """Finding 4: neither profile's stream can be graded by the other's
        tables, whichever direction it is fed from."""
        import test_chat_login_evidence as login
        # A chord stream handed to the LOGIN grader is refused at its very
        # first marker: p0.session carries no login username, and even with
        # one the chord markers are outside the login contract.
        chord_by_login = login.verdict(stream(), run_id='chord')
        self.assertIsNotNone(chord_by_login)
        self.assertIn('malformed p0.session', chord_by_login)
        named = [dict(r) for r in stream()]
        named[0]['username'] = mod.chat_login_username('chord')
        # Even with a valid login username the number itself is out of the
        # login series -- that is what the 101 offset buys.
        self.assertIn('contract=104 is not one of [1, 2]',
                      login.verdict(named, run_id='chord'))
        # ... and with the number forced into the login series the chord
        # markers are still outside the login contract. Contract 104 moved the
        # E-2 gate to p1.chord_bound, so that is now the FIRST chord-only
        # marker the login grader meets -- it is refused there instead of at
        # p2.chord, which is the same claim one marker earlier.
        named[0]['contract'] = 2
        self.assertIn('unexpected marker p1.chord_bound',
                      login.verdict(named, run_id='chord'))
        self.assertNotIn(('p1', 'chord_bound'), mod.CHAT_LOGIN_EXPECTED)
        self.assertNotIn(('p2', 'chord'), mod.CHAT_LOGIN_EXPECTED)
        # The login profile's own p4 marker name is `key`, never
        # `chord_restore`, so the two contracts stay disjoint at P4 as well.
        self.assertNotIn(('p4', 'chord_restore'), mod.CHAT_LOGIN_EXPECTED)
        login_by_chord = verdict(login.stream(run_id='chord'), run_id='chord')
        self.assertIsNotNone(login_by_chord)
        self.assertIn('malformed p0.session: contract=', login_by_chord)

    def test_rejections(self):
        """Every rejection the contract names, each from the valid stream with
        exactly one thing broken."""
        cases = [
            # -- the claim itself ------------------------------------------
            ('chat_opened=True -- a chord must never open the composer',
             verdict(patched(('p2', 'chord'), chat_opened=True))),
            ('chat_opened=True -- a chord must never open the composer',
             verdict(patched(('p2', 'chord'), occurrence=1,
                             chat_opened=True))),
            # The ruled order MUST toggle on the device ...
            ('order=1 fired=False on wii -- the ruled order (hold Minus, then '
             'press Plus) must reach IB_DEBUG_OVERLAY',
             verdict(stream(fired=False))),
            # ... and the WRONG order must NOT, on any platform.
            ('order=0 fired=True -- the wrong press order (Plus first) must '
             'never toggle the overlay',
             verdict(stream(fired=True))),
            ('order=0 fired=True -- the wrong press order (Plus first) must '
             'never toggle the overlay',
             verdict(stream(platform='pc', fired=True))),
            # -- M-1: p2 COUNTS its fires, it does not merely flag them ------
            # A bool accepts three toggles, which end the overlay ON again;
            # the stream would then be rejected at p3.closed with "the ruled
            # chord fired but the overlay is off", a counting defect wearing a
            # product defect's diagnosis. Graded here exactly as
            # p4.chord_restore already is.
            ('p2.chord order=1 fires=2 -- the ruled chord must fire EXACTLY '
             'once',
             verdict(patched(('p2', 'chord'), occurrence=1, fires=2))),
            ('p2.chord order=1 fires=0 contradicts fired=True',
             verdict(patched(('p2', 'chord'), occurrence=1, fires=0))),
            ('p2.chord order=0 fires=1 contradicts fired=False',
             verdict(patched(('p2', 'chord'), fires=1))),
            ('malformed p2.chord: fires=None',
             verdict(patched(('p2', 'chord'), fires=None))),
            # -- M-2: the overlay reading is cross-checked against the fire --
            # p2.chord has always carried `overlay` and the collector never
            # read it, so a stream where the action fired and the product
            # toggled NOTHING (or toggled without firing) was accepted here
            # and only caught later, indirectly, on another leg.
            ('p2.chord order=1 overlay=False but the leg fired 1 time(s)',
             verdict(patched(('p2', 'chord'), occurrence=1, overlay=False))),
            ('p2.chord order=0 overlay=True but the leg fired 0 time(s)',
             verdict(patched(('p2', 'chord'), overlay=True))),
            ('malformed p2.chord: overlay=None',
             verdict(patched(('p2', 'chord'), overlay=None))),
            # Minus and Plus are also the hotbar taps: a chord that stepped a
            # slot is the regression this leg exists to catch.
            ('slot_before=4 slot_after=5 -- a chord must never move the '
             'hotbar',
             verdict(stream(slots=(4, 5)))),
            ('malformed p2.chord: slot_before=None',
             verdict(patched(('p2', 'chord'), slot_before=None))),
            # An unobservable hotbar is not proof on the console.
            ('slot_before=-1 on wii -- the hotbar was never observable, so '
             '"the chord did not move it" is unproven',
             verdict(stream(slots=(-1, -1)))),
            # -- C-1: the E-2 gate, at its new stage -----------------------
            # On a console the chord IS the profile, so a run whose live
            # binding table forms none must be REFUSED at this gate (ok:false,
            # which note() latches as a phase failure), never carried past it
            # with a cheerful bound=False.
            ('p1.chord_bound bound=False on wii with ok:true -- a console run '
             'whose config binds no F3 chord must be REFUSED',
             verdict(stream(bound=False))),
            ('malformed p1.chord_bound: bound=None',
             verdict(patched(('p1', 'chord_bound'), bound=None))),
            ('phase failure p1.chord_bound: no F3 chord is bound',
             verdict(patched(('p1', 'chord_bound'), ok=False,
                             diag='no F3 chord is bound: this card carries '
                                  'the RELEASE config'))),
            # The OTHER cause the device keeps separate: nothing ever scanned
            # the table. A different fix, so a different message -- and the
            # collector reports whichever the device sent rather than
            # flattening the two.
            ('phase failure p1.chord_bound: the binding table was never '
             'scanned after the pad bind',
             verdict(patched(('p1', 'chord_bound'), ok=False,
                             diag='the binding table was never scanned after '
                                  'the pad bind'))),
            ('missing marker p1.chord_bound: seen 0 of 1',
             verdict(stream(skip=(('p1', 'chord_bound'),)))),
            ('p3.opened opened=False',
             verdict(patched(('p3', 'opened'), opened=False))),
            # Contract 103: p3.closed no longer demands "overlay off" -- one
            # legal toggle has been spent, so the honest assertion is that the
            # overlay is on EXACTLY when the ruled order fired. Both
            # disagreements are refused.
            ('p3.closed overlay=False but the ruled chord fired',
             verdict(patched(('p3', 'closed'), overlay=False))),
            ('p3.closed overlay=True but the ruled chord did not fire',
             verdict(stream(platform='pc', closed_overlay=True))),
            ('p3.closed ruled_fired=False contradicts p2.chord order=1 '
             'fired=True',
             verdict(patched(('p3', 'closed'), ruled_fired=False))),
            # -- the RESTORE leg (contract 103) ----------------------------
            ('p4.chord_restore overlay=True -- the run must leave the debug '
             'overlay OFF',
             verdict(stream(end_overlay=True))),
            ('p4.chord_restore fired=False on wii -- a second correct-order '
             'chord must reach IB_DEBUG_OVERLAY',
             verdict(stream(restore=False))),
            ('p4.chord_restore fires=2 -- the restoring chord must fire '
             'EXACTLY once',
             verdict(patched(('p4', 'chord_restore'), fires=2))),
            ('p4.chord_restore fires=0 contradicts fired=True',
             verdict(patched(('p4', 'chord_restore'), fires=0))),
            ('p4.chord_restore chat_opened=True -- a chord must never open '
             'the composer',
             verdict(patched(('p4', 'chord_restore'), chat_opened=True))),
            ('p4.chord_restore slot_before=4 slot_after=5 -- a chord must '
             'never move the hotbar',
             verdict(patched(('p4', 'chord_restore'), slot_after=5))),
            ('missing marker p4.chord_restore: seen 0 of 1',
             verdict(stream(skip=(('p4', 'chord_restore'),)))),
            ('p0.session overlay=True -- the run must start from a forced-OFF '
             'debug overlay',
             verdict(patched(('p0', 'session'), overlay=True))),
            # -- both press orders, each exactly once ----------------------
            ('the contract presses BOTH orders [0, 1], each once',
             verdict(stream(orders=(0, 0)))),
            ('malformed p2.chord: order=7',
             verdict(patched(('p2', 'chord'), order=7))),
            ('the two p2.chord markers carry press orders [1, 0]',
             verdict(stream(orders=(1, 0)))),
            ('missing marker p2.chord: seen 1 of 2',
             verdict(stream(orders=(0,)))),
            # A repeated p2.chord is caught by the ORDER rule first (the same
            # press order cannot happen twice), so the count rule is pinned on
            # a marker that carries no order of its own.
            ('malformed p2.chord: order=0',
             verdict(stream(repeat=('p2', 'chord')))),
            ('duplicate marker p3.opened: seen 2 times',
             verdict(stream(repeat=('p3', 'opened')))),
            # -- structure --------------------------------------------------
            ('missing marker p3.opened',
             verdict(stream(skip=(('p3', 'opened'),)))),
            ('missing marker run.ok: seen 0 of 1',
             verdict(stream(skip=(('run', 'ok'),)))),
            ('no chat_chord markers were received',
             verdict([])),
            ('run id missing', verdict(stream(), run_id='')),
            ('unexpected marker p1.hold_refused: not a marker of the '
             'mp_chat_chord_1m contract',
             verdict(patched(('p1', 'pad_bound'), phase='p1',
                             marker='hold_refused'))),
            ('phase failure p2.chord: the chord opened the chat composer',
             verdict(patched(('p2', 'chord'), ok=False,
                             diag='the chord opened the chat composer'))),
            ('run.timeout: phase timeout',
             verdict(patched(('p3', 'opened'), phase='run', marker='timeout',
                             ok=False, diag='phase timeout'))),
            ('p1.pad_bound bound=False',
             verdict(patched(('p1', 'pad_bound'), bound=False))),
            ('p1.pad_bound observed (kind,channel)=(2, 0)',
             verdict(patched(('p1', 'pad_bound'), channel=0))),
            ("malformed p0.session: platform='switch'",
             verdict(patched(('p0', 'session'), platform='switch'))),
            # -- completion -------------------------------------------------
            ('profile_done never arrived', verdict(stream(), done=None)),
            ('profile_done reported', verdict(stream(), done='p2:stuck')),
            ('profile_done arrived before run.ok',
             verdict(stream(), done_at=3)),
        ]
        for expected, got in cases:
            self.assertIsNotNone(got, expected)
            self.assertIn(expected, got)

    def test_seq_and_order(self):
        rows = stream()
        rows[3]['seq'] = 9
        self.assertIn('seq gap before p2.chord', verdict(rows))
        rows = stream()
        rows.append(dict(rows[-2], seq=len(rows) + 1))
        self.assertIn('markers continued after run.ok', verdict(rows))
        rows = stream()
        rows[2], rows[4] = dict(rows[4], seq=3), dict(rows[2], seq=5)
        self.assertIn('reordered markers', verdict(rows))


if __name__ == '__main__':
    unittest.main(verbosity=0)
