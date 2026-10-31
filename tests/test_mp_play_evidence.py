"""mp_play_10m's collector verdict, replayed OFFLINE -- no Wii, no server.

The contract (docs/mp_play_profile.md, from the push-0921 design under Astra
gate 1) is graded by tools/wii_telemetry_server.py's MpPlayEvidence. This suite
finds every harness rule offline instead of spending one ten-minute real-Wii run
per rule: the rows go straight through Collector.process(), which is the same
entry point the UDP/TCP readers and `--replay` feed.

The valid run is built ONCE below -- a two-generation tape plus the mc173-diag
receipt log that agrees with it -- and every rejection case is that same pair
with exactly one thing broken, so a case can never pass for a reason the valid
run does not also have. Written BEFORE any real run, as the contract requires.

The committed fixture under tests/fixtures/mp_play/ is the SAME pair, written to
disk: one test proves the two cannot drift, and another replays the directory
through the shipped `--replay` entry point end to end.
"""
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))  # the collector imports its siblings
_spec = importlib.util.spec_from_file_location(
    'wii_telemetry_server', ROOT / 'tools' / 'wii_telemetry_server.py')
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

# The shared login machine is CALLED, not copied (Astra answer B3), so its
# marker stream in an mp_play run is the very stream mp_chat_login_3m's own
# suite already builds. Reusing that generator rather than transcribing it is
# what keeps this contract honest about what "the same login" means.
_login_spec = importlib.util.spec_from_file_location(
    'test_chat_login_evidence', Path(__file__).with_name(
        'test_chat_login_evidence.py'))
login_suite = importlib.util.module_from_spec(_login_spec)
_login_spec.loader.exec_module(login_suite)

RUN_ID = '0921_120000'
NONCE = mod.chat_login_username(RUN_ID)
FIXTURES = ROOT / 'tests' / 'fixtures' / 'mp_play'

# The shape of the valid run. Fourteen cycles fit the measured window with the
# margin the contract asks for (about two skipped build legs), the first keeps
# its block under the RESERVE rule, and the four inventory moves ride on the
# odd cycles -- which is what spreads every counted action over all three
# thirds of measured play.
CYCLES = 14
MEASURED_MS = 601000
CYCLE_MS = 43000
INV_CYCLES = (1, 3, 5, 7)
DIG_ID = 2          # grass: hand-diggable and it drops a placeable block
DROP_ID = 3         # ... namely dirt
POS_STEP_MS = 5000  # the fixture writes a pos receipt every 5 s
POS_STEP_BLOCKS = 1.0
# ONE server position correction, past mp_play_core.h's 8-block teleport
# threshold, so the valid run itself proves the jump is REMOVED from the walked
# displacement rather than counted towards it (Astra answer B2).
TELEPORT_AT_MS = 300000
TELEPORT_BLOCKS = 12.0
ORIGIN_X, ORIGIN_Y, ORIGIN_Z = 8.5, 65.62, 8.5


def cycle_ms(cycle):
    return 1000 + cycle * CYCLE_MS


def dig_cell(cycle):
    """The cell under the feet of cycle `cycle`: the walker has carried the body
    +X along the walk leg, so each cycle digs its own column."""
    return (int(ORIGIN_X) + cycle * 8, 64, int(ORIGIN_Z))


def place_cell(cycle):
    x, y, z = dig_cell(cycle)
    return (x + 1, y, z)


def raw_x(t_ms):
    """The SERVER's x at measured time `t_ms`, teleport included."""
    walked = ORIGIN_X + (t_ms // POS_STEP_MS) * POS_STEP_BLOCKS
    return walked + (TELEPORT_BLOCKS if t_ms >= TELEPORT_AT_MS else 0.0)


FINAL_X = raw_x(MEASURED_MS)
# What the player is still carrying when it leaves: the reserve block the first
# cycle kept, plus the stack the inventory leg moved. A NON-EMPTY inventory is
# mandatory -- "empty == empty" across the rejoin proves nothing.
FINAL_INVENTORY = {36: (DROP_ID, 2, 0), 40: (DROP_ID, 1, 0)}
# REL-14b: generation 2's post-reconnect action moves slot 36's whole stack into
# the empty slot 37 (n = generation 1's move count, filled in by marker_rows).
POST_ACTION = dict(gen=2, window=0, action_pick=1, action_put=2, from_slot=36,
                   to_slot=37, item=DROP_ID, count=2, damage=0,
                   measured_ms=MEASURED_MS)


# ---------------------------------------------------------------------------
# the device's marker stream
# ---------------------------------------------------------------------------

def shift_seq(rows, offset):
    """A chat_login sub-stream renumbered onto the run's continuing seq. `corr`
    names an earlier seq, so it shifts with it."""
    out = []
    for row in rows:
        row = dict(row)
        row['seq'] += offset
        if row.get('corr'):
            row['corr'] += offset
        out.append(row)
    return out


def login_rows(gen, offset, platform='wii'):
    """REL-14b-0's ACTION-ONLY login, once per generation: p0.session and
    p1.pad_bound re-observed, then p1.actions_ready. Nothing is typed (vanilla
    never answers /login) and the seq continues across both generations."""
    rows = login_suite.stream(run_id=RUN_ID, platform=platform)[:2]
    rows.append(dict(event='chat_login', seq=3, phase='p1', ms=300, corr=0,
                     marker='actions_ready', ok=True, run_id=RUN_ID,
                     typed_login=False))
    return shift_seq(rows, offset)


def marker_rows(**knobs):
    """The valid mp_play marker stream, in emission order."""
    rows, state = [], {'seq': 0, 'login_seq': 0}

    def emit(marker, **body):
        state['seq'] += 1
        row = dict(event=mod.MP_PLAY_EVENT, seq=state['seq'], marker=marker,
                   run_id=RUN_ID)
        row.update(body)
        rows.append(row)
        return row

    def login(gen):
        batch = login_rows(gen, state['login_seq'],
                           knobs.get('platform', 'wii'))
        state['login_seq'] = batch[-1]['seq']
        rows.extend(batch)

    def proto_of(core):
        """mp_play_core.h's mpp_proto_slot: hotbar 0..8 -> 36..44, grid
        9..35 unchanged."""
        return 36 + core if core < 9 else core

    def snapshot(key, gen, x, y, z, dim, slots):
        emit(key, gen=gen, x=x, y=y, z=z, dim=dim,
             parts=mod.MP_PLAY_INV_PARTS)
        for part in range(mod.MP_PLAY_INV_PARTS):
            entries = [[proto_of(k)] + list(slots[proto_of(k)])
                       for k in range(part * 12, part * 12 + 12)
                       if proto_of(k) in slots]
            emit('inv', of=key, gen=gen, part=part, slots=entries)

    emit('session', contract=mod.MP_PLAY_CONTRACT_CURRENT,
         measured_floor_ms=mod.MP_PLAY_MEASURED_FLOOR_MS,
         total_cap_ms=mod.MP_PLAY_TOTAL_CAP_MS,
         platform=knobs.get('platform', 'wii'), nonce=NONCE)
    emit('ready', gen=1, timeout_admits=knobs.get('admits1', 0), x=ORIGIN_X,
         y=ORIGIN_Y, z=ORIGIN_Z, dim=0, health=20)
    emit('login_begin', gen=1)
    login(1)
    emit('login_ok', gen=1, since_connect_ms=knobs.get('since1', 4200))
    emit('p2_begin', t0_ms=41000)
    digs = places = moves = picks = 0
    stuck = knobs.get('walk_done_stuck')

    def walk_done_stuck():
        # hw1021f: the walker stuck after the floors were met; the honest
        # body carries the tape's own counts, `stuck` may override any field.
        body = dict(x=-120.5, y=81.0, z=-780.5, measured_ms=cycle_ms(cycle),
                    heading_deg=270, stuck=4, digs=digs, places=places,
                    inv_moves=moves, disp=276.0)
        body.update(stuck.get('body', {}))
        emit('walk_done_stuck', **body)

    cycle = 0
    for cycle in range(CYCLES):
        base = cycle_ms(cycle)
        if stuck is not None and stuck.get('at') == cycle:
            walk_done_stuck()
        x, y, z = dig_cell(cycle)
        digs += 1
        emit('dig', n=digs, x=x, y=y, z=z, old_id=DIG_ID, measured_ms=base)
        picks += 1
        emit('pickup', n=picks, slot=0, item=DROP_ID, count=1)
        if cycle in INV_CYCLES:
            moves += 1
            emit('inv_move', n=moves, window=0, action_pick=100 + moves * 2,
                 action_put=101 + moves * 2, from_slot=36, to_slot=40,
                 item=DROP_ID, count=1, damage=0, measured_ms=base + 1500)
        if cycle == 0:
            # The reserve rule: a place only happens while at least two
            # placeable blocks are held, so the FIRST cycle keeps its block and
            # the player leaves with a non-empty inventory.
            emit('reserve_kept', cycle=cycle, held=1)
            continue
        if knobs.get('narration') and cycle == 1:
            # push-0921/B12 narration, emitted where the device emits it: the
            # plant that stood on this cycle's place target, broken first, and
            # a dug item that never arrived. Generated here rather than spliced
            # in afterwards because seq is gapless by contract and the login
            # sub-streams carry `corr` back-references -- renumbering around an
            # insertion breaks them (it did, first try).
            emit('plant_cleared', n=1, x=x + 1, y=y + 1, z=z)
            emit('pickup_missed', n=1, x=x, y=y, z=z, measured_ms=base + 900)
        if knobs.get('escape') and cycle == 2:
            # The pit escape's narration, where the device emits it (inside
            # measured play, between legs): a wall cell dug for a block, then
            # one pillar step out of the pocket (hw1021e follow-up).
            emit('escape_centred', nudges=2, x=x + 0.5, y=float(y), z=z + 0.5)
            emit('escape_mine', n=1, x=x + 1, y=y + 1, z=z, old_id=DIG_ID)
            emit('escape_step', n=1, y=float(y + 2), tries=3, mines=1)
        px, py, pz = place_cell(cycle)
        places += 1
        emit('place', n=places, x=px, y=py, z=pz, id=DROP_ID,
             measured_ms=base + 2500)
        if cycle == 6:
            emit('teleport', n=1, x=raw_x(base), y=ORIGIN_Y, z=ORIGIN_Z)
    if stuck is not None and stuck.get('at', CYCLES) >= CYCLES:
        walk_done_stuck()
    emit('p2_end', measured_ms=knobs.get('measured_ms', MEASURED_MS),
         cycles=CYCLES, digs=digs, picks=picks, places=places,
         inv_moves=moves, build_skipped=0, teleports=1)
    snapshot('snapshot_pre', 1, FINAL_X, ORIGIN_Y, ORIGIN_Z, 0,
             knobs.get('pre_inventory', FINAL_INVENTORY))
    emit('disconnected', gen=1, ms=2400)
    emit('rejoin_begin', gen=2)
    emit('ready', gen=2, timeout_admits=knobs.get('admits2', 0), x=FINAL_X,
         y=ORIGIN_Y, z=ORIGIN_Z, dim=0, health=20)
    emit('login_begin', gen=2)
    login(2)
    emit('login_ok', gen=2, since_connect_ms=knobs.get('since2', 3900))
    snapshot('snapshot_post', 2, FINAL_X, ORIGIN_Y, ORIGIN_Z, 0,
             knobs.get('post_inventory', FINAL_INVENTORY))
    emit('verified', ok=True, rejoin_ms=knobs.get('rejoin_ms', 38000), diag='')
    emit('post_action', **{**POST_ACTION, 'n': moves, **knobs.get('post', {})})
    emit('ok', seq_total=state['seq'] + 1, teleports=1)
    return rows


# ---------------------------------------------------------------------------
# the fixture's receipt log
# ---------------------------------------------------------------------------

# How mc173's tracing logger really writes a receipt: an ISO timestamp and a
# level in ANSI colour, then the line itself (measured on the first full PC
# chat-login run). The prefix is tolerated and never parsed.
MC173_PREFIX = ('\x1b[2m2026-09-21T12:00:47.533115Z\x1b[0m \x1b[33m WARN\x1b[0m '
                'mc173_server: ')


def receipt(gen, ev, fields=''):
    tail = (' ' + fields) if fields else ''
    return '%s[mp-play] n=%s g=%d ev=%s%s' % (MC173_PREFIX, NONCE, gen, ev,
                                              tail)


def login_gate_line():
    return ('%s[login-gate] %s /login len=%d sha256=%s'
            % (MC173_PREFIX, NONCE, mod.CHAT_LOGIN_SECRET_LEN,
               hashlib.sha256(mod.CHAT_LOGIN_SECRET).hexdigest()))


def inv_list(slots):
    """receipt.rs's inv_list(): all 40 persisted slots, ascending in its own
    order, an empty one written as item 0."""
    return mod.mp_play_canonical_inventory(slots)


def snapshot_receipt(gen, x, y, z, dim, slots):
    listing = inv_list(slots)
    return receipt(gen, 'snapshot',
                   'x=%.3f y=%.3f z=%.3f dim=%d inv=[%s] inv_hash=%s'
                   % (x, y, z, dim, listing,
                      hashlib.sha256(listing.encode('utf-8')).hexdigest()))


def receipt_lines(**knobs):
    """The fixture log of the valid run: one line per observable event, in the
    order the server produced them."""
    lines = []
    pre_inventory = knobs.get('pre_inventory', FINAL_INVENTORY)
    lines.append(receipt(1, 'login_accept', 'since_accept_ms=4100'))
    lines.append(snapshot_receipt(1, ORIGIN_X, ORIGIN_Y, ORIGIN_Z, 0, {}))
    lines.append(receipt(1, 'health', 'h=20'))
    timed = []
    for t in range(0, MEASURED_MS + 1, POS_STEP_MS):
        fields = 'x=%.3f y=%.3f z=%.3f dim=0' % (raw_x(t), ORIGIN_Y, ORIGIN_Z)
        if t == TELEPORT_AT_MS:
            fields += ' tp=1'
        timed.append((t, 0, receipt(1, 'pos', fields)))
    moves = 0
    for cycle in range(CYCLES):
        base = cycle_ms(cycle)
        x, y, z = dig_cell(cycle)
        timed.append((base, 1, receipt(
            1, 'dig', 'x=%d y=%d z=%d old_id=%d old_meta=0' % (x, y, z,
                                                               DIG_ID))))
        if cycle in INV_CYCLES:
            moves += 1
            pick, put = 100 + moves * 2, 101 + moves * 2
            timed.append((base + 1500, 1, receipt(
                1, 'click', 'window=0 action=%d slot=36 button=0 shift=0 '
                            'accepted=true' % pick)))
            timed.append((base + 1500, 2, receipt(
                1, 'slot_state', 'window=0 slot=36 item=0 count=0 damage=0')))
            timed.append((base + 1500, 3, receipt(
                1, 'slot_state',
                'window=0 slot=-1 item=%d count=1 damage=0' % DROP_ID)))
            timed.append((base + 1500, 4, receipt(
                1, 'click', 'window=0 action=%d slot=40 button=0 shift=0 '
                            'accepted=true' % put)))
            timed.append((base + 1500, 5, receipt(
                1, 'slot_state',
                'window=0 slot=40 item=%d count=1 damage=0' % DROP_ID)))
            timed.append((base + 1500, 6, receipt(
                1, 'slot_state', 'window=0 slot=-1 item=0 count=0 damage=0')))
        if cycle == 0:
            continue
        px, py, pz = place_cell(cycle)
        timed.append((base + 2500, 1, receipt(
            1, 'place', 'x=%d y=%d z=%d id=%d meta=0' % (px, py, pz, DROP_ID))))
    timed.sort(key=lambda row: (row[0], row[1]))
    lines.extend(row[2] for row in timed)
    lines.append(snapshot_receipt(1, FINAL_X, ORIGIN_Y, ORIGIN_Z, 0,
                                  pre_inventory))
    lines.append(receipt(1, 'disconnect',
                         'reason=%s detail=disconnect.quit'
                         % knobs.get('reason', 'quit')))
    lines.append(receipt(2, 'login_accept', 'since_accept_ms=3800'))
    lines.append(snapshot_receipt(2, FINAL_X, ORIGIN_Y, ORIGIN_Z, 0,
                                  knobs.get('post_inventory', pre_inventory)))
    lines.append(receipt(2, 'pos', 'x=%.3f y=%.3f z=%.3f dim=0'
                         % (FINAL_X, ORIGIN_Y, ORIGIN_Z)))
    # REL-14b: POST_ACTION's two clicks, each with the slot states it set
    for v in ((1, 36), (36, 0, 0), (-1, DROP_ID, 2), (2, 37), (37, DROP_ID, 2),
              (-1, 0, 0)):
        lines.append(receipt(2, 'click', 'window=0 action=%d slot=%d button=0 '
                             'shift=0 accepted=true' % v) if len(v) == 2 else
                     receipt(2, 'slot_state', 'window=0 slot=%d item=%d '
                             'count=%d damage=0' % v))
    return lines


def server_log(port=25570, ids=(730, 5445), reason='disconnect.quitting',
               hosts=('192.0.2.27', '192.0.2.27'), extra=()):
    """REL-14b-ev2a: the pinned VANILLA server's server.log, record for record
    as REL-14a/run1/server.log shows it: ConsoleLogFormatter.java:24-43 stamps,
    MinecraftServer.java:113/130, NetLoginHandler.java:117, the chat players
    really type (NetServerHandler.java:442) and NetServerHandler.java:404."""
    info = ['Starting minecraft server version Beta 1.7.3',
            'Starting Minecraft server on *:%d' % port,
            'Done (11669892800ns)! For help, type "help" or "?"']
    for gen, eid in enumerate(ids, 1):
        info.append('%s [/%s:%d] logged in with entity id %d at (1.5, '
                    '65.62000000476837, -32.5)' % (NONCE, hosts[gen - 1],
                                                   65210 + gen, eid))
        info.append('<%s> plain chat %d' % (NONCE, gen))
        if gen == 1:
            info.append('%s lost connection: %s' % (NONCE, reason))
    return ['2026-10-19 10:00:%02d [INFO] %s' % (i, l)
            for i, l in enumerate(info)] + list(extra)  # extra: whole records


# ---------------------------------------------------------------------------
# harness
# ---------------------------------------------------------------------------

def index_of(rows, marker, occurrence=0):
    """The mp_play marker by that name. Scoped to this event on purpose: the
    shared login sub-streams carry markers of their own (`ok`, `ready`, ...)
    and a rewrite meant for one contract must never land in the other."""
    hits = [i for i, row in enumerate(rows)
            if row['event'] == mod.MP_PLAY_EVENT and row.get('marker') == marker]
    return hits[occurrence]


def patched(marker, occurrence=0, rows=None, **fields):
    """The valid stream with one marker rewritten."""
    rows = list(rows if rows is not None else marker_rows())
    i = index_of(rows, marker, occurrence)
    rows[i] = {**rows[i], **fields}
    return rows


def dropped(marker, occurrence=0, rows=None):
    """The valid stream with one marker removed and the rest renumbered, so the
    stream is wrong in exactly one way: the marker is missing, not the seq."""
    rows = list(rows if rows is not None else marker_rows())
    i = index_of(rows, marker, occurrence)
    victim = rows.pop(i)
    out = []
    for row in rows:
        if row['event'] != victim['event'] or row['seq'] < victim['seq']:
            out.append(row)
            continue
        row = dict(row)
        row['seq'] -= 1
        if row.get('corr', 0) > victim['seq']:
            row['corr'] -= 1
        if row.get('marker') == 'ok':
            row['seq_total'] -= 1
        out.append(row)
    return out


def swapped(first, second, rows=None):
    """Two markers delivered the other way round. Only their CONTENT swaps: seq
    stays gapless, so the stream is wrong in exactly one way -- the order."""
    rows = list(rows if rows is not None else marker_rows())
    i, j = index_of(rows, first), index_of(rows, second)
    fixed = ('event', 'seq', 'run_id')
    content = [{k: v for k, v in rows[n].items() if k not in fixed}
               for n in (i, j)]
    rows[i] = {**{k: rows[i][k] for k in fixed}, **content[1]}
    rows[j] = {**{k: rows[j][k] for k in fixed}, **content[0]}
    return rows


def without_receipt(lines, predicate):
    return [line for line in lines if not predicate(line)]


def replaced_receipt(lines, predicate, replacement, limit=1):
    out, hits = [], 0
    for line in lines:
        if predicate(line) and hits < limit:
            hits += 1
            out.append(replacement(line) if callable(replacement)
                       else replacement)
            continue
        out.append(line)
    assert hits, 'no receipt matched the rewrite'
    return out


def collect(td, name, rows, lines, done='ok', heartbeats=600, profile=None,
            server=None):
    """The whole path, not just the evidence class: Collector.process() ->
    _finish_normal() -> the profile's two gates."""
    collector = mod.Collector(mod._selftest_args(
        Path(td) / name, RUN_ID, profile or mod.MP_PLAY_PROFILE))
    if server is not None:  # the pinned vanilla server's own log
        (collector.out / 'server.log').write_text('\n'.join(server) + '\n')
    if lines:
        # An EMPTY list writes nothing at all -- which is how "the fixture log
        # never reached the run directory" is reproduced.
        (collector.out / mod.CHAT_LOGIN_RECEIPT_NAME).write_text(
            '\n'.join(lines) + '\n', encoding='utf-8')
    collector.process(dict(event='heartbeat', load_screen_ms=3000, run_id=RUN_ID, tick=1,
                           world_loaded=True, lw_to_admits=0))
    collector.hb_count = heartbeats
    collector.world_loaded_seen = True
    for row in rows:
        collector.process({**row, 'run_id': RUN_ID})
    if done is not None:
        collector.process(dict(event='profile_done', run_id=RUN_ID,
                               status=done))
    elif not collector.done:
        # A run whose completion never arrived is still graded, exactly as a
        # live run that reached its duration would be.
        collector._finish_normal('offline replay')
    return collector


def verdict(rows=None, lines=None, **kwargs):
    """The overall verdict of one run: its fail reason, or None for a PASS."""
    with tempfile.TemporaryDirectory() as td:
        collector = collect(td, 'run',
                            marker_rows() if rows is None else rows,
                            receipt_lines() if lines is None else lines,
                            **kwargs)
        return collector.fail_reason


def device_verdict(rows=None):
    """The DEVICE half alone: the marker contract, with no receipts involved."""
    evidence = mod.MpPlayEvidence(RUN_ID)
    for row in (marker_rows() if rows is None else rows):
        if row['event'] == mod.MP_PLAY_EVENT:
            evidence.note(row)
        else:
            evidence.note_login(row)
    evidence.note_profile_done('ok')
    return evidence.complete_error()


class MpPlayEvidenceTest(unittest.TestCase):

    def test_receipts_graded_on_device_failure(self):
        """A profile_done status!="ok" is still a FAIL, but the fixture receipts
        are matched first so the artifact reports real matched counts."""
        with tempfile.TemporaryDirectory() as td:
            bad = collect(td, 'devfail', marker_rows(), receipt_lines(),
                          done='failed:walk')
            self.assertIsNotNone(bad.fail_reason)
            self.assertIn('profile_done status=failed:walk', bad.fail_reason)
            self.assertEqual(bad.write_result(), 1)
            proof = json.loads((bad.out / 'FAIL.json').read_text())
            self.assertFalse((bad.out / 'PASS.json').exists())
            self.assertEqual(bad.mp_play.matched,
                             {'dig': CYCLES, 'place': CYCLES - 1,
                              'inv_move': len(INV_CYCLES)})
            self.assertEqual(proof['mp_play']['matched'],
                             {'dig': CYCLES, 'place': CYCLES - 1,
                              'inv_move': len(INV_CYCLES)})

    # -- the valid run --------------------------------------------------

    def test_valid_run_passes(self):
        """The whole contract, device + fixture: PASS, with the artifact stating
        what was matched and what the run is worth."""
        with tempfile.TemporaryDirectory() as td:
            good = collect(td, 'good', marker_rows(), receipt_lines())
            self.assertIsNone(good.fail_reason, good.fail_reason)
            self.assertTrue(good.done)
            self.assertEqual(good.write_result(), 0)
            proof = json.loads((good.out / 'PASS.json').read_text())
            self.assertIn(mod.MP_PLAY_PASS_LABEL, proof['reason'])
            self.assertEqual(proof['certification_scope'],
                             'mp_play_unattended_lan_session')
            play = proof['mp_play']
            self.assertTrue(play['device_complete'])
            self.assertTrue(play['receipt']['ok'])
            self.assertEqual(play['matched'],
                             {'dig': CYCLES, 'place': CYCLES - 1,
                              'inv_move': len(INV_CYCLES)})
            self.assertEqual(play['unmatched'], [])
            self.assertTrue(play['post_action']['matched'])
            self.assertEqual(play['measured_ms'], MEASURED_MS)
            self.assertEqual(play['nonce'], NONCE)
            self.assertEqual(play['timeout_admits'], {'1': 0, '2': 0})
            self.assertEqual(sorted(play['login_generations']), ['1', '2'])
            self.assertIsNone(play['login_error'])
            self.assertEqual(play['teleports'], 1)
            self.assertEqual(play['reserve_kept'], 1)
            # The teleport is REMOVED from the walked displacement, so the
            # 12-block server correction buys nothing. The sample that carried
            # it earns no walked distance either (its whole delta is attributed
            # to the server), which costs one 5 s interval of honest walking --
            # deliberately conservative, never in the run's favour.
            walked = ((MEASURED_MS // POS_STEP_MS) - 1) * POS_STEP_BLOCKS
            self.assertAlmostEqual(play['displacement'], walked, places=3)
            self.assertEqual(play['receipt']['disconnect']['reason'], 'quit')
            self.assertEqual(play['receipt']['login_cmd'], {})

    def test_narration_markers_are_tallied_and_grade_nothing(self):
        """plant_cleared and pickup_missed (8bf54845, push-0921/B12) are
        NARRATION, in the class build_skipped / heading_turned already occupy.

        Two halves, and the first is the one that bites: a marker the contract
        does not list REFUSES THE WHOLE TAPE ("unexpected marker ..."), which is
        how PC run pc_mp_play_20260921_130624 was lost. So the run below carries
        both markers and must still PASS unchanged.

        The second half is that they change no verdict and no counted total:
        p2_end's own tallies, the floors and the spread rule all see exactly
        what they saw before, because a cleared plant is not an action and a
        MISSED pickup is the absence of one -- feeding either to the spread rule
        would let a run satisfy a third of measured play with things that never
        happened."""
        rows = marker_rows(narration=True)
        self.assertEqual(
            sum(1 for r in rows if r.get('marker') == 'plant_cleared'), 1)
        self.assertEqual(
            sum(1 for r in rows if r.get('marker') == 'pickup_missed'), 1)

        with tempfile.TemporaryDirectory() as td:
            run = collect(td, 'narrated', rows, receipt_lines())
            self.assertIsNone(run.fail_reason, run.fail_reason)
            summary = run.mp_play.summary()
            self.assertEqual(summary['plants_cleared'], 1)
            self.assertEqual(summary['pickups_missed'], 1)
            # Counted totals untouched: same matched actions as the plain run.
            plain = collect(td, 'plain', marker_rows(), receipt_lines())
            self.assertEqual(summary['matched'],
                             plain.mp_play.summary()['matched'])
            self.assertIsNone(plain.fail_reason, plain.fail_reason)

    def test_escape_markers_are_tallied_and_grade_nothing(self):
        """escape_step / escape_mine (the mp_play walker's pit escape) are
        NARRATION. Unlisted, the first successful escape on hardware would have
        refused the whole tape ("unexpected marker 'escape_step'"); listed,
        they change no verdict and no counted total."""
        rows = marker_rows(escape=True)
        self.assertEqual(
            sum(1 for r in rows if r.get('marker') == 'escape_step'), 1)
        with tempfile.TemporaryDirectory() as td:
            run = collect(td, 'escaped', rows, receipt_lines())
            self.assertIsNone(run.fail_reason, run.fail_reason)
            summary = run.mp_play.summary()
            self.assertEqual(summary['escape_steps'], 1)
            self.assertEqual(summary['escape_mines'], 1)
            self.assertEqual(summary['escape_failed'], 0)
            self.assertEqual(summary['escape_centred'], 1)
            plain = collect(td, 'plain', marker_rows(), receipt_lines())
            self.assertEqual(summary['matched'],
                             plain.mp_play.summary()['matched'])
            self.assertEqual(summary['actions'],
                             plain.mp_play.summary()['actions'])

    def test_walk_done_stuck_needs_every_floor_met(self):
        """hw1021f: a walker stuck AFTER the walk-phase floors were met ends
        the walk as success (walk_done_stuck) and stands until p2_end. The
        collector accepts that marker only when the tape it rides on already
        has every action floor and the displacement floor met, never lets the
        walker act after it, and still grades every floor from the receipts."""
        rows = marker_rows(walk_done_stuck={})
        self.assertEqual(
            sum(1 for r in rows if r.get('marker') == 'walk_done_stuck'), 1)
        with tempfile.TemporaryDirectory() as td:
            run = collect(td, 'stuck_met', rows, receipt_lines())
            self.assertIsNone(run.fail_reason, run.fail_reason)
            summary = run.mp_play.summary()
            self.assertEqual(summary['walk_done_stuck']['x'], -120.5)
            plain = collect(td, 'plain', marker_rows(), receipt_lines())
            self.assertEqual(summary['matched'],
                             plain.mp_play.summary()['matched'])
        # Floors NOT met when it got stuck: refused, whatever came after.
        early = device_verdict(marker_rows(walk_done_stuck={'at': 3}))
        self.assertIsNotNone(early)
        self.assertIn('walk_done_stuck with digs=3', early)
        # A claim the tape does not support (counts inflated to the floors).
        lying = device_verdict(marker_rows(walk_done_stuck={
            'at': 3, 'body': {'digs': 8, 'places': 8, 'inv_moves': 4}}))
        self.assertIsNotNone(lying)
        self.assertIn('walk_done_stuck with digs=8 (tape has 3', lying)
        # Displacement below the floor.
        short = device_verdict(marker_rows(walk_done_stuck={
            'body': {'disp': 40.0}}))
        self.assertIsNotNone(short)
        self.assertIn('below the 64 block floor', short)
        # Missing stuck position.
        nopos = device_verdict(marker_rows(walk_done_stuck={
            'body': {'x': None}}))
        self.assertIsNotNone(nopos)
        self.assertIn('malformed walk_done_stuck', nopos)
        # The walker may not act after the walk ended.
        acted = device_verdict(marker_rows(walk_done_stuck={'at': 12}))
        self.assertIsNotNone(acted)
        self.assertIn('dig after walk_done_stuck', acted)

    def test_losing_most_dug_items_fails_by_name(self):
        """The other side of the same ruling: ONE missed pickup is narration,
        but a run that loses MOST of its drops has a collect-path defect, and
        the DEVICE says so itself (mp_play_core.h: ">= 3 misses and more than
        half the digs" -> fail "most dug items never reached the inventory").

        No new collector rule is needed for that and none was added -- the
        generic `fail` marker rule already refuses any run carrying one. This
        pins that it really does, with THIS diag, so a future rework of the fail
        handling cannot quietly let a collect-path defect through as a pass."""
        rows = marker_rows()
        at = index_of(rows, 'p2_end', 0)
        rows = rows[:at] + [
            dict(event=mod.MP_PLAY_EVENT, seq=rows[at]['seq'], marker='fail',
                 phase='build_pickup', gen=1,
                 diag='most dug items never reached the inventory',
                 measured_ms=cycle_ms(4), run_id=RUN_ID),
        ]
        reason = verdict(rows=rows, done='failed:build_pickup')
        self.assertIsNotNone(reason)
        # The OVERALL reason is the driver's own verdict line, which names the
        # phase rather than the diag -- the collector refuses the run because
        # the device declared it failed, which is the point.
        self.assertIn('failed:build_pickup', reason)
        # The DEVICE half is where the diag itself is reported, so a reader of
        # the artifact learns WHY without opening the tape.
        device = device_verdict(rows)
        self.assertIsNotNone(device)
        self.assertIn('most dug items never reached the inventory', device)

    def test_item_receipts_are_narration_and_classify_a_miss(self):
        """The three item-entity receipts (fixture 5f6d406b, push-0921/B13).

        FIRST, and this is the one that would have cost a run: an item the
        fixture cannot attribute is logged `n=- g=0`, and the foreign-nonce rule
        refuses any non-login receipt whose nonce is not this run's -- so before
        the exemption, a run whose dropped item merely DESPAWNED was rejected
        outright as "the log belongs to another run". The exemption is narrow
        and the second half of this case proves it: a receipt naming a different
        REAL player still refuses the tape.

        SECOND, the classification, which grades nothing and only says who owns
        each miss. Three misses, three cases, three owners."""
        rows = marker_rows(narration=True)
        base = receipt_lines()
        # One dig's drop is collected by the server into slot 37 and the client
        # never shows it; one despawns; one is still lying in the world.
        extra = [
            receipt(1, 'item_spawn',
                    'id=900 item=3 count=1 damage=0 x=10.5 y=64.5 z=10.5 '
                    'vx=0.1 vy=0.2 vz=-0.05'),
            receipt(1, 'item_collect', 'id=900 item=3 count=1 slot=37'),
            receipt(1, 'item_spawn',
                    'id=901 item=3 count=1 damage=0 x=20.5 y=64.5 z=20.5 '
                    'vx=0.0 vy=0.2 vz=0.0'),
            '[mp-play] n=- g=0 ev=item_gone id=901 reason=despawn',
            receipt(1, 'item_spawn',
                    'id=902 item=3 count=1 damage=0 x=30.5 y=64.5 z=30.5 '
                    'vx=0.3 vy=0.2 vz=0.3'),
        ]
        lines = base + extra
        with tempfile.TemporaryDirectory() as td:
            run = collect(td, 'items', rows, lines)
            # The tape is NOT refused: the unattributed item_gone is exempt and
            # the three unknown-to-the-old-contract events parse as narration.
            self.assertIsNone(run.fail_reason, run.fail_reason)

        # The exemption does not extend to a different real player.
        with tempfile.TemporaryDirectory() as td:
            hostile = collect(td, 'hostile', rows, base + [
                '[mp-play] n=cwdeadbe g=1 ev=item_gone id=903 reason=despawn'])
            self.assertIsNotNone(hostile.fail_reason)
            self.assertIn('belongs to another run', hostile.fail_reason)

        # And the classifier itself, driven from misses whose cells match the
        # spawns above.
        ev = mod.MpPlayEvidence(RUN_ID)
        ev.pickup_missed_rows = [
            {'n': 1, 'x': 10, 'y': 64, 'z': 10, 'measured_ms': 1000},
            {'n': 2, 'x': 20, 'y': 64, 'z': 20, 'measured_ms': 2000},
            {'n': 3, 'x': 30, 'y': 64, 'z': 30, 'measured_ms': 3000},
            {'n': 4, 'x': 99, 'y': 64, 'z': 99, 'measured_ms': 4000},
        ]
        play = ev._read_receipts_from_lines(extra) if hasattr(
            ev, '_read_receipts_from_lines') else None
        if play is None:
            play = []
            for line in extra:
                m = mod.MP_PLAY_RECEIPT_RE.search(line)
                self.assertIsNotNone(m, line)
                play.append({'n': m.group(1), 'gen': int(m.group(2)),
                             'ev': m.group(3),
                             'f': ev._receipt_fields(m.group(4)),
                             'raw': line, 'used': False})
        ev.classify_pickup_misses(play)
        cases = [c['case'] for c in ev.pickup_miss_cases]
        self.assertEqual(cases, ['handed_over', 'drifted', 'still_in_world',
                                 'no_spawn'])
        handed = ev.pickup_miss_cases[0]
        self.assertEqual(handed['slot'], '37')
        self.assertIn('CAVEWII', handed['owner'])
        self.assertEqual(ev.pickup_miss_cases[1]['reason'], 'despawn')
        # "still in the world" reports where it is and how far the body was.
        still = ev.pickup_miss_cases[2]
        self.assertEqual(still['spawn']['x'], '30.5')
        self.assertIn('owner', still)

    def test_pc_run_may_not_wear_the_console_label(self):
        """Same rule as mp_chat_login_3m: a PC proof is a PC proof."""
        with tempfile.TemporaryDirectory() as td:
            pc = collect(td, 'pc', marker_rows(platform='pc'), receipt_lines())
            self.assertIsNone(pc.fail_reason, pc.fail_reason)
            self.assertEqual(pc.write_result(), 0)
            proof = json.loads((pc.out / 'PASS.json').read_text())
            self.assertIn(mod.MP_PLAY_PC_PASS_LABEL, proof['reason'])
            self.assertNotIn(mod.MP_PLAY_PASS_LABEL, proof['reason'])
            self.assertIn('HARDWARE-UNCONFIRMED', proof['reason'])

    def test_profile_shape(self):
        """mp_play_10m is an mp_connect-gated, marker-and-receipt-certified
        profile -- not a split/anchor one, and not performance-certifying."""
        profile = mod.PROFILES[mod.MP_PLAY_PROFILE]
        # 1260 = hw_run.c's cfg->duration_seconds for this profile, 60 s past
        # mp_play_core.h's MPP_TOTAL_CAP_MS so the driver's own named failure
        # lands inside the window (reconciled 2026-09-21, push-0921/B6).
        self.assertEqual(profile['duration'], 1260)
        self.assertEqual(profile['min_hb'], 480)
        self.assertEqual(profile['scenario'], 'mp_connect')
        self.assertFalse(profile['expect_dump'])
        for table in (mod.SPLIT_PROFILES, mod.ANCHOR_PROFILES,
                      mod.SPLIT_CERTIFYING, mod.SPLIT_GUEST,
                      mod.RELEASE_FRAME_PROFILES):
            self.assertNotIn(mod.MP_PLAY_PROFILE, table)
        # The heartbeat floor is the run's OWN measured cadence, which the
        # contract's 600 000 ms floor keeps at or above the table's 480.
        evidence = mod.MpPlayEvidence(RUN_ID)
        self.assertEqual(evidence.heartbeat_floor(480), 480)
        evidence.measured_ms = mod.MP_PLAY_MEASURED_FLOOR_MS
        self.assertEqual(evidence.heartbeat_floor(480), 480)
        evidence.measured_ms = 900000
        self.assertEqual(evidence.heartbeat_floor(480), 720)

    def test_other_profiles_ignore_the_markers(self):
        """An mp_play record on another profile is recorded and ignored, and no
        other profile grows a play gate."""
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'smoke', marker_rows(), None,
                                profile='mp_connect_smoke', heartbeats=100)
            self.assertEqual(collector.mp_play.markers, 0)
            self.assertIsNone(collector.fail_reason, collector.fail_reason)
            self.assertEqual(collector.write_result(), 0)
            proof = json.loads((collector.out / 'PASS.json').read_text())
            self.assertIsNone(proof['mp_play'])
            tape = (collector.out / 'telemetry.ndjson').read_text()
            self.assertIn('"mp_play"', tape)

    # -- one negative per rule -------------------------------------------

    def test_marker_structure_negatives(self):
        rows = marker_rows()
        gap = [dict(row, seq=row['seq'] + 1)
               if row['event'] == mod.MP_PLAY_EVENT and row['seq'] > 4 else row
               for row in rows]
        truncated = [row for row in rows
                     if row['event'] == mod.MP_PLAY_EVENT and row['seq'] <= 3]
        truncated.append(dict(event=mod.MP_PLAY_EVENT, seq=4, marker='fail',
                              run_id=RUN_ID, phase='walk', gen=1,
                              diag='total timeout', measured_ms=540000))
        cases = [
            # missing phase marker. (A missing p2_begin or snapshot_post is
            # caught EARLIER and more specifically -- by the marker that then
            # has no window or no head to belong to -- so those two are proved
            # in test_measured_window_negatives and test_device_snapshot_parts
            # by the reason they really produce.)
            ('missing phase marker verified', device_verdict(
                dropped('verified'))),
            ('missing phase marker disconnected', device_verdict(
                dropped('disconnected'))),
            ('missing phase marker ready(gen 2)', device_verdict(
                dropped('ready', occurrence=1))),
            ('missing phase marker login_ok(gen 2)', device_verdict(
                dropped('login_ok', occurrence=1))),
            # reordered markers
            ('reordered markers: disconnected arrived after rejoin_begin',
             device_verdict(swapped('disconnected', 'rejoin_begin'))),
            ('reordered markers: ready(gen 1) arrived after '
             'login_begin(gen 1)',
             device_verdict(swapped('ready', 'login_begin'))),
            # seq gap
            ('seq gap before p2_begin', device_verdict(gap)),
            # the device's own failure marker, which is how the TOTAL CAP is
            # reported: it fires inside the profile duration, so a stuck run
            # always says why rather than merely timing out.
            ('total timeout', device_verdict(truncated)),
            # no final `ok`
            ('missing phase marker ok', device_verdict(dropped('ok'))),
            # ... and no profile_done at all
            ('profile_done was never received', verdict(done=None)),
        ]
        for expected, got in cases:
            with self.subTest(expected=expected):
                self.assertIsNotNone(got, expected)
                self.assertIn(expected, got)

    def test_profile_done_ordering(self):
        """The contract ends with the `ok` marker and THEN profile_done "ok".
        A completion that arrives first is a premature success, and a run that
        never emitted `ok` cannot be rescued by one."""
        rows = marker_rows()
        evidence = mod.MpPlayEvidence(RUN_ID)
        for i, row in enumerate(rows):
            if i == len(rows) // 2:
                evidence.note_profile_done('ok')
            if row['event'] == mod.MP_PLAY_EVENT:
                evidence.note(row)
            else:
                evidence.note_login(row)
        self.assertIn('premature success', evidence.complete_error())
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'no_ok', dropped('ok'), receipt_lines())
            self.assertEqual(collector.failure_class, 'mp_play_evidence_failed')
            self.assertIn('missing phase marker ok', collector.fail_reason)

    def test_readiness_and_login_deadline_negatives(self):
        cases = [
            ('ready(gen 1) timeout_admits=1',
             device_verdict(patched('ready', admits=None, timeout_admits=1))),
            ('ready(gen 2) timeout_admits=1',
             device_verdict(patched('ready', occurrence=1,
                                    timeout_admits=1))),
            ('login_ok(gen 1) since_connect_ms=60000',
             device_verdict(patched('login_ok', since_connect_ms=60000))),
            ('login_ok(gen 2) since_connect_ms=61000',
             device_verdict(patched('login_ok', occurrence=1,
                                    since_connect_ms=61000))),
        ]
        for expected, got in cases:
            with self.subTest(expected=expected):
                self.assertIsNotNone(got, expected)
                self.assertIn(expected, got)

    def test_measured_window_negatives(self):
        short = patched('p2_end', measured_ms=599999)
        self.assertIn("measured play was 599999 ms, below the contract's "
                      "600000 ms floor", device_verdict(short))
        # A counted action outside the p2_begin..p2_end window earns nothing
        # and says so: measured play is P2 only (Astra Lane B item 5).
        self.assertIn('dig arrived outside measured play',
                      device_verdict(swapped('p2_begin', 'dig')))
        # The device's own tallies must agree with the markers it emitted.
        self.assertIn('p2_end digs=13, but the run emitted 14',
                      device_verdict(patched('p2_end', digs=13)))

    def test_rejoin_and_verification_negatives(self):
        self.assertIn('rejoin took 90001 ms, past the contract\'s 90000 ms',
                      device_verdict(patched('verified', rejoin_ms=90001)))
        self.assertIn('verified ok=false', device_verdict(
            patched('verified', ok=False, diag='inventory changed')))

    def test_heartbeat_backstop_counter(self):
        """B-a: the counter must be present and zero from the FIRST heartbeat to
        the LAST, this profile and mp_lan_walk_10m only."""
        with tempfile.TemporaryDirectory() as td:
            for name, beats, expect in (
                    ('missing', [{}], 'lw_timeout_admits_missing'),
                    ('nonzero', [{'lw_to_admits': 0}, {'lw_to_admits': 1}],
                     'lw_timeout_admits_nonzero'),
                    ('clean', [{'lw_to_admits': 0}, {'lw_to_admits': 0}],
                     None)):
                collector = mod.Collector(mod._selftest_args(
                    Path(td) / name, RUN_ID, mod.MP_PLAY_PROFILE))
                for n, beat in enumerate(beats):
                    collector.process(dict(event='heartbeat', load_screen_ms=3000, run_id=RUN_ID,
                                           tick=100 + n, world_loaded=True,
                                           **beat))
                with self.subTest(case=name):
                    self.assertEqual(collector.failure_class, expect)

    def test_lan_walk_movement_cadence_tapes(self):
        """Consecutive live beats must account for every movement tick."""
        def run(name, beats):
            with tempfile.TemporaryDirectory() as td:
                collector = mod.Collector(mod._selftest_args(
                    Path(td) / name, RUN_ID, 'mp_lan_walk_10m'))
                for seq, (state, ticks, samples, refused) in enumerate(beats, 1):
                    collector.process(dict(event='heartbeat', load_screen_ms=3000, run_id=RUN_ID,
                                           seq=seq, up_ms=seq * 1000,
                                           tick=seq, world_loaded=True,
                                           lw_to_admits=0, mp_state=state,
                                           mp_gen=1, mp_move_ticks=ticks,
                                           mp_move_samples=samples,
                                           rpcdrop=refused, x=seq * 40.0,
                                           y=70.0, z=8.0))
                    if collector.done:
                        break
                if not collector.done:
                    collector.hb_count = 480
                    collector.process(dict(event='profile_done', run_id=RUN_ID,
                                           status='ok'))
                rc = collector.write_result()
                path = collector.out / ('PASS.json' if rc == 0 else 'FAIL.json')
                return rc, json.loads(path.read_text())

        equal = [(4, 10, 10, 0), (4, 12, 12, 0), (4, 15, 15, 0)]
        rc, proof = run('equal', equal)
        self.assertEqual(rc, 0, proof['reason'])
        self.assertEqual(proof['mp_move_cadence']['totals'],
                         {'ticks': 5, 'samples': 5, 'rpcdrop': 0})
        self.assertEqual(proof['mp_move_cadence']['worst_pair']['d_ticks'], 3)

        rc, proof = run('unexplained',
                        [(4, 10, 10, 0), (4, 12, 11, 0)])
        self.assertEqual(rc, 1)
        for fragment in ('seq=1->2', 'up_ms=1000->2000',
                         'd_ticks=2', 'd_samples=1', 'd_rpcdrop=0'):
            self.assertIn(fragment, proof['reason'])

        rc, proof = run('refused',
                        [(4, 10, 10, 0), (4, 12, 11, 1)])
        self.assertEqual(rc, 0, proof['reason'])
        self.assertEqual(proof['mp_move_cadence']['totals'],
                         {'ticks': 2, 'samples': 1, 'rpcdrop': 1})

        rc, proof = run('excess',
                        [(4, 10, 10, 0), (4, 12, 13, 0)])
        self.assertEqual(rc, 1)
        self.assertIn('d_samples=3', proof['reason'])

        rc, proof = run('prelive',
                        [(3, 10, 0, 0), (3, 20, 0, 0),
                         (4, 0, 0, 0), (4, 2, 2, 0)])
        self.assertEqual(rc, 0, proof['reason'])
        self.assertEqual(proof['mp_move_cadence']['totals'],
                         {'ticks': 2, 'samples': 2, 'rpcdrop': 0})

    def test_dig_and_place_receipts_earn_nothing_when_they_disagree(self):
        """A dig or place receipt that is absent, stale or about another cell
        matches NOTHING (Astra Lane B item 4). Fourteen digs against a floor of
        eight means one such marker does not by itself fail the run -- so the
        rule is proved where it actually lives: the counted total drops and the
        artifact names the marker that earned nothing. The FAILURE path, when
        enough of them go unmatched, is test_floors_met_only_by_unmatched."""
        first_dig = 'ev=dig x=%d y=%d z=%d' % dig_cell(0)
        first_place = 'ev=place x=%d y=%d z=%d' % place_cell(1)
        cases = [
            # no receipt at all
            ('dig', 'dig n=1', without_receipt(
                receipt_lines(), lambda l: first_dig in l)),
            ('place', 'place n=1', without_receipt(
                receipt_lines(), lambda l: first_place in l)),
            # a receipt from the WRONG generation
            ('dig', 'dig n=1', replaced_receipt(
                receipt_lines(), lambda l: first_dig in l,
                lambda l: l.replace('g=1 ev=dig', 'g=2 ev=dig'))),
            # a receipt for a DIFFERENT cell
            ('dig', 'dig n=1', replaced_receipt(
                receipt_lines(), lambda l: first_dig in l,
                lambda l: l.replace('x=%d y=' % dig_cell(0)[0],
                                    'x=%d y=' % (dig_cell(0)[0] + 400)))),
            # ... or a different resulting block id
            ('dig', 'dig n=1', replaced_receipt(
                receipt_lines(), lambda l: first_dig in l,
                lambda l: l.replace('old_id=%d' % DIG_ID, 'old_id=12'))),
            ('place', 'place n=1', replaced_receipt(
                receipt_lines(), lambda l: first_place in l,
                lambda l: l.replace('id=%d meta=0' % DROP_ID,
                                    'id=12 meta=0'))),
        ]
        with tempfile.TemporaryDirectory() as td:
            for i, (kind, named, lines) in enumerate(cases):
                with self.subTest(case=named, broken=i):
                    collector = collect(td, 'case%d' % i, marker_rows(), lines)
                    play = collector.mp_play.summary()
                    self.assertEqual(
                        play['matched'][kind],
                        (CYCLES if kind == 'dig' else CYCLES - 1) - 1)
                    self.assertEqual(len(play['unmatched']), 1)
                    self.assertTrue(play['unmatched'][0].startswith(named),
                                    play['unmatched'][0])

    def test_receipt_matching_negatives(self):
        """Every rule that needs the fixture log, each proved where its own
        failure is unambiguous. The inventory moves carry the four rules whose
        floor IS the count, so one broken receipt is one failed run."""
        cases = [
            # the click receipt is absent
            ('inv_move n=1 pick-up click: 0 generation 1',
             verdict(lines=without_receipt(receipt_lines(),
                                           lambda l: 'action=102 ' in l))),
            # ... or belongs to the OTHER generation
            ('inv_move n=1 pick-up click: 0 generation 1',
             verdict(lines=replaced_receipt(
                 receipt_lines(), lambda l: 'action=102 ' in l,
                 lambda l: l.replace('g=1 ev=click', 'g=2 ev=click')))),
            # ... or names a different action id
            ('inv_move n=1 pick-up click: 0 generation 1',
             verdict(lines=replaced_receipt(
                 receipt_lines(), lambda l: 'action=102 ' in l,
                 lambda l: l.replace('action=102 ', 'action=902 ')))),
            # a rejected click may never be credited
            ('was credited to a REJECTED pick-up click',
             verdict(lines=replaced_receipt(
                 receipt_lines(), lambda l: 'action=102 ' in l,
                 lambda l: l.replace('accepted=true', 'accepted=false')))),
            # the resulting slot state disagrees with the move
            ("the server's slot_state receipt says item 3 x9 dmg 0",
             verdict(lines=replaced_receipt(
                 receipt_lines(),
                 lambda l: 'slot=40 item=%d count=1' % DROP_ID in l,
                 lambda l: l.replace('count=1', 'count=9')))),
            # ... or never arrived, so nothing confirmed where the stack went
            ('has no slot_state receipt for its destination slot 40',
             verdict(lines=without_receipt(
                 receipt_lines(),
                 lambda l: 'slot=40 item=%d count=1' % DROP_ID in l))),
            # ... or the cursor is still holding the stack: not a transfer
            ('left the cursor holding item 3 x1',
             verdict(lines=replaced_receipt(
                 receipt_lines(),
                 lambda l: 'slot=-1 item=0 count=0' in l,
                 lambda l: l.replace('slot=-1 item=0 count=0',
                                     'slot=-1 item=3 count=1')))),
            # the server logged a death / a zero health reading
            ('logged a death receipt',
             verdict(lines=receipt_lines() + [receipt(1, 'death')])),
            ('logged health h=0',
             verdict(lines=replaced_receipt(receipt_lines(),
                                            lambda l: 'ev=health' in l,
                                            receipt(1, 'health', 'h=0')))),
            # the leave was not a production pause-menu quit
            ("left the server with reason='lost'",
             verdict(lines=receipt_lines(reason='lost'))),
            # the fixture log belongs to another run
            ("not this run's nonce",
             verdict(lines=[line.replace(NONCE, 'cwdeadbe')
                            for line in receipt_lines()])),
            # ... or is missing altogether
            ('is missing from the run directory', verdict(lines=[])),
        ]
        for expected, got in cases:
            with self.subTest(expected=expected):
                self.assertIsNotNone(got, expected)
                self.assertIn(expected, got)

    def test_login_gate_receipts(self):
        """REL-14b-0: the device types nothing, so ANY [login-gate] line, in
        either generation, contradicts the run; login_accept stays one per
        generation."""
        lines = receipt_lines()
        for at in (1, len(lines)):
            self.assertIn('typed /login', verdict(
                lines=lines[:at] + [login_gate_line()] + lines[at:]) or '')
        self.assertIn('2 login_accept receipts for generation 1',
                      verdict(lines=[lines[0]] + lines))

    def test_a_receipt_is_consumed_once(self):
        """Two device markers for ONE receipt: the second earns nothing.

        On the digs the run survives it (fourteen markers, one floor of eight),
        and the artifact is what reports the loss -- which is the honest
        outcome, and exactly why `unmatched` is a reported field. On the
        inventory moves, where the floor IS the count, the same duplication
        fails the run by name."""
        rows = marker_rows()
        x, y, z = dig_cell(0)
        second = index_of(rows, 'dig', 1)
        rows[second] = {**rows[second], 'x': x, 'y': y, 'z': z,
                        'old_id': DIG_ID}
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'dupe', rows, receipt_lines())
            self.assertIsNone(collector.fail_reason, collector.fail_reason)
            play = collector.mp_play.summary()
            self.assertEqual(play['matched']['dig'], CYCLES - 1)
            self.assertEqual(len(play['unmatched']), 1)
            self.assertIn('dig n=2', play['unmatched'][0])
        rows = marker_rows()
        first = rows[index_of(rows, 'inv_move')]
        second = index_of(rows, 'inv_move', 1)
        rows[second] = {**rows[second],
                        'action_pick': first['action_pick'],
                        'action_put': first['action_put']}
        problem = verdict(rows=rows)
        self.assertIsNotNone(problem)
        self.assertIn('inv_move n=2 pick-up click: 1 generation 1', problem)

    def test_floors_met_only_by_unmatched_markers(self):
        """The floors count MATCHED actions: a run that emitted enough markers
        but had only seven confirmed by the server fails, and says so."""
        lines = receipt_lines()
        for cycle in (0, 1, 2, 3, 4, 5, 6):
            target = 'ev=dig x=%d y=%d z=%d' % dig_cell(cycle)
            lines = without_receipt(lines, lambda l, t=target: t in l)
        problem = verdict(lines=lines)
        self.assertIn('action floor not met: 7 matched dig action(s) of the 14',
                      problem)
        self.assertIn('unmatched markers earn nothing', problem)

    def test_spread_rule(self):
        """At least one matched dig AND one matched place per third of measured
        play, inventory moves in at least two thirds -- thirds computed from the
        markers' own measured_ms."""
        # Every dig crammed into the first third: the floor is still met, the
        # spread rule is not.
        rows = marker_rows()
        for n in range(CYCLES):
            i = index_of(rows, 'dig', n)
            rows[i] = {**rows[i], 'measured_ms': 1000 + n * 100}
        self.assertIn('spread rule: no matched dig in third 2, 3',
                      verdict(rows=rows))
        # Every place in the last third.
        rows = marker_rows()
        for n in range(CYCLES - 1):
            i = index_of(rows, 'place', n)
            rows[i] = {**rows[i], 'measured_ms': 560000 + n * 100}
        self.assertIn('spread rule: no matched place in third 1, 2',
                      verdict(rows=rows))
        # All four inventory moves in one third.
        rows = marker_rows()
        for n in range(len(INV_CYCLES)):
            i = index_of(rows, 'inv_move', n)
            rows[i] = {**rows[i], 'measured_ms': 2000 + n * 100}
        self.assertIn('matched inventory moves fall in 1 third(s)',
                      verdict(rows=rows))

    def test_displacement(self):
        """Server-observed, teleports REMOVED."""
        short = [line for line in receipt_lines() if 'ev=pos' not in line]
        short.insert(3, receipt(1, 'pos', 'x=8.500 y=65.620 z=8.500 dim=0'))
        short.insert(4, receipt(1, 'pos', 'x=40.500 y=65.620 z=8.500 dim=0'))
        problem = verdict(lines=short)
        self.assertIn('walked displacement was 32.0 blocks', problem)
        self.assertIn('teleports removed', problem)
        # A run that only LOOKS far because the server teleported it: the jump
        # is subtracted, so it fails on the distance it actually walked.
        jumped = list(short)
        jumped[4] = receipt(1, 'pos', 'x=540.500 y=65.620 z=8.500 dim=0 tp=1')
        self.assertIn('walked displacement was 0.0 blocks', verdict(
            lines=jumped))

    def test_snapshot_comparison(self):
        """Inventory exact, position within 1.0 / 1.5, dimension equal -- for
        the fixture's own pair AND for the device's claims about it."""
        moved = {36: (DROP_ID, 1, 0), 40: (DROP_ID, 1, 0)}
        cases = [
            # the SERVER lost a block across the rejoin
            ("the server's own state across the rejoin disagree on the "
             "inventory", verdict(lines=receipt_lines(post_inventory=moved))),
            # ... or put the player somewhere else
            ("disagree on position", verdict(
                lines=replaced_receipt(
                    receipt_lines(),
                    lambda l: 'g=2 ev=snapshot' in l,
                    lambda l: l.replace('x=%.3f' % FINAL_X,
                                        'x=%.3f' % (FINAL_X + 2.0))))),
            ("disagree on dimension", verdict(
                lines=replaced_receipt(
                    receipt_lines(),
                    lambda l: 'g=2 ev=snapshot' in l,
                    lambda l: l.replace('dim=0 inv=', 'dim=-1 inv=')))),
            # the DEVICE claimed an inventory the server never had
            ("the device's snapshot_pre and the fixture's generation 1 "
             "disconnect snapshot disagree on the inventory",
             verdict(rows=marker_rows(pre_inventory=moved,
                                      post_inventory=moved))),
            # an EMPTY pre-disconnect inventory proves nothing (reserve rule)
            ('the pre-disconnect inventory was EMPTY',
             verdict(rows=marker_rows(pre_inventory={}, post_inventory={}),
                     lines=receipt_lines(pre_inventory={},
                                         post_inventory={}))),
        ]
        for expected, got in cases:
            with self.subTest(expected=expected):
                self.assertIsNotNone(got, expected)
                self.assertIn(expected, got)

    def test_device_snapshot_parts(self):
        """A snapshot is a head plus three `inv` parts, in order, of its own
        head -- a truncated one is never quietly compared."""
        self.assertIn('was cut short', device_verdict(dropped('inv', 2)))
        self.assertIn('arrived out of order',
                      device_verdict(patched('inv', 1, part=2)))
        self.assertIn("claims of='snapshot_post'",
                      device_verdict(patched('inv', 0, of='snapshot_post')))
        # Dropping the HEAD alone is caught by its orphaned parts ...
        self.assertIn('arrived with no open snapshot',
                      device_verdict(dropped('snapshot_pre')))
        # ... and dropping the whole snapshot is a missing phase marker. The
        # SERVER half is graded even when the device half already failed, so it
        # has to survive that stream too and say the same thing in its own
        # words rather than raising.
        rows = marker_rows()
        head = index_of(rows, 'snapshot_pre')
        headless = rows[:head] + rows[head + 1 + mod.MP_PLAY_INV_PARTS:]
        headless = [dict(row, seq=row['seq'] - 4)
                    if row['event'] == mod.MP_PLAY_EVENT
                    and row['seq'] > rows[head]['seq'] else row
                    for row in headless]
        i = index_of(headless, 'ok')
        headless[i] = dict(headless[i], seq_total=headless[i]['seq'])
        self.assertIn('missing phase marker snapshot_pre',
                      device_verdict(headless))
        evidence = mod.MpPlayEvidence(RUN_ID)
        for row in headless:
            if row['event'] == mod.MP_PLAY_EVENT:
                evidence.note(row)
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            (out / mod.CHAT_LOGIN_RECEIPT_NAME).write_text(
                '\n'.join(receipt_lines()) + '\n', encoding='utf-8')
            self.assertIn('emitted no complete snapshot_pre marker',
                          evidence.receipt_error(out))

    def test_shared_login_substream(self):
        """REL-14b-0 action-only login, per generation, by ChatLoginEvidence's
        own rules: a typed-login leg, a bare-Remote bind, a typed grant, a gap
        or a malformed field fails by name, never by exception."""
        rows = marker_rows()
        self.assertIsNone(device_verdict(rows))
        login = [i for i, r in enumerate(rows) if r['event'] == 'chat_login']
        self.assertEqual(len(login), 6)

        def edit(k, **fields):
            return [dict(r, **fields) if i == login[k] else r
                    for i, r in enumerate(rows)]
        for expected, got in (
                ('generation 1 login: unexpected marker p1.hold_refused',
                 edit(2, marker='hold_refused')),
                ('generation 2 login: missing marker p1.actions_ready',
                 [r for i, r in enumerate(rows) if i != login[5]]),
                ('generation 2 login: seq gap', edit(3, seq=99)),
                ('category=0', edit(4, category=0)),  # bare Wiimote on wii
                ('typed_login=True', edit(5, typed_login=True)),
                ('malformed chat_login marker', edit(1, seq='2')),
                ("username='cwdeadbe'", edit(3, username='cwdeadbe'))):
            with self.subTest(expected=expected):
                self.assertIn(expected, device_verdict(got) or '')
        # Every generation logs in on the RUN's platform and pad: a Wii run
        # whose generation 2 is a PC keyboard login fails, either way round.
        for gen, run, other in ((2, 'wii', 'pc'), (1, 'pc', 'wii')):
            got, swap = marker_rows(platform=run), login_rows(gen, 0, other)
            for k in range(2):  # p0.session and p1.pad_bound
                i = login[3 * gen - 3 + k]
                got[i] = dict(swap[k], seq=got[i]['seq'])
            expected = 'generation %d login: p0.session platform=' % gen
            with self.subTest(expected=expected):
                self.assertIn(expected, device_verdict(got) or '')

    def test_post_action_negatives(self):
        """REL-14b: gen 2's move is graded on the device and against the
        server's clicks (CFR NetServerHandler.java:534-543), never raising."""
        lines = receipt_lines()

        def g2(action):
            return lambda l: 'g=2 ev=click' in l and 'action=%d ' % action in l

        def edit(action, old, new, pick=None, rows=None):
            return verdict(rows=rows, lines=replaced_receipt(
                lines, pick or g2(action), lambda l: l.replace(old, new)))

        a102, dest = (lambda l: 'action=102 ' in l), (
            lambda l: 'slot=37 item=' in l)
        dest40 = (lambda l: 'slot=40 item=%d count=1' % DROP_ID in l)

        def post(**fields):
            return device_verdict(marker_rows(post=fields))
        # the LAST generation 1 move's slot state, moved past the rejoin
        i = max(i for i, l in enumerate(lines) if dest40(l))
        j = lines.index(next(filter(lambda l: 'g=2 ev=login_a' in l, lines)))
        bled = lines[:i] + lines[i + 1:j + 1] + [lines[i]] + lines[j + 1:]
        cur = (lambda l: 'slot=-1 item=0 count=0' in l)
        with patch.object(mod, 'MP_PLAY_FLOOR_INV_MOVES', 3):  # above floor
            gone = verdict(lines=without_receipt(lines, a102))
        for expected, got in (
                ('missing phase marker post_action',
                 verdict(rows=dropped('post_action'))),
                ('reordered markers: verified arrived after post_action',
                 device_verdict(swapped('verified', 'post_action'))),
                ('post_action carries gen=1', post(gen=1)),
                ('post_action n=3, but generation 1 made 4', post(n=3)),
                ('malformed post_action: from_slot=[]', post(from_slot=[])),
                ('post_action pick-up click: 1', edit(1, '=36 ', '=40 ')),
                ('post_action put-down click: 1 generation 2',
                 edit(2, 'button=0', 'button=1')),
                ('post_action put-down click: 1', edit(2, 'ft=0', 'ft=1')),
                ('post_action pick-up click: 1', edit(1, 'slot=36', 'slot=x')),
                ('post_action pick-up click: 0', edit(1, 'g=2 ev', 'g=1 ev')),
                ('post_action pick-up click: 2 generation 2',
                 verdict(lines=lines + [next(filter(g2(1), lines))])),
                ('post_action put-down click: 0 generation 2',
                 verdict(lines=without_receipt(lines, g2(2)))),
                ('post_action was credited to a REJECTED pick-up click',
                 edit(1, 'accepted=true', 'accepted=false')),
                ('post_action has no slot_state receipt for its destination '
                 'slot 37', verdict(lines=without_receipt(lines, dest))),
                ('inv_move n=4 has no slot_state receipt for its destination '
                 'slot 40', verdict(lines=bled)),
                ('inv_move n=1 pick-up click: 0 generation 1', gone),
                ('post_action reports one action number (1) for both', edit(
                    2, 'action=2 ', 'action=1 ', rows=marker_rows(post=dict(
                        action_put=1)))),
                ("malformed slot_state receipt: {'window': '0', 'slot': '-1', "
                 "'item': '0', 'count': 'x'", edit(0, '=0 d', '=x d', cur)),
                ('inv_move n=1 has no empty-cursor slot_state receipt after',
                 verdict(lines=without_receipt(lines, cur))),
                ('inv_move n=1 pick-up click: 1 generation 1',
                 edit(0, 'slot=36', 'slot=37', pick=a102)),
                ('inv_move n=1 pick-up click: 2 generation 1', verdict(
                    lines=lines + [next(filter(a102, lines))])),
                ('post_action put-down click precedes its pick-up',
                 verdict(lines=lines[:-6] + lines[-3:] + lines[-6:-3])),
                ('post_action pick-up click left slot 36=(0, 0, 0), cursor=(3, '
                 '1, 0)', edit(0, 'count=2', 'count=1', pick=lines[-4].__eq__)),
                ('post_action has no slot_state receipt for its destination '
                 'slot 37', edit(0, 'window=0', 'window=9', pick=dest)),
                # Beta accepts a no-op click (NetServerHandler.java:535):
                # empty 37 -> empty 38, receipts agreeing, moved nothing
                ('post_action claims item 0 x0: not a real transfer', verdict(
                    rows=marker_rows(post=dict(from_slot=37, to_slot=38,
                                               item=0, count=0)),
                    lines=lines[:-6] + [receipt(2, *v) for v in (
                        ('click', 'window=0 action=1 slot=37 button=0 shift=0 '
                         'accepted=true'), ('slot_state', 'window=0 slot=37 '
                         'item=0 count=0 damage=0'), ('slot_state', 'window=0'
                         ' slot=-1 item=0 count=0 damage=0'), ('click', 'windo'
                         'w=0 action=2 slot=38 button=0 shift=0 accepted=true'),
                        ('slot_state', 'window=0 slot=38 item=0 count=0 damage'
                         '=0'), ('slot_state', 'window=0 slot=-1 item=0 count=0'
                         ' damage=0'))]))):
            with self.subTest(expected=expected):
                self.assertIn(expected, got or '')

    def test_vanilla_server_log_binds_the_sessions(self):
        """REL-14b-ev2a: against vanilla the receipt is its own server.log,
        bound to this run's nonce, the pinned port, one client host and two
        ordered sessions with a clean quit between. Only complete records
        bind: chat is logged '<name> text', so typed lookalikes never do. No
        action/state witness yet (ev2b): FAIL CLOSED."""
        gap = 'vanilla server-side action/state witness not implemented'
        stamp = '2026-10-19 10:01:00 [%s] ' % 'INFO'
        lost = stamp + '%s lost connection: disconnect.%%s' % NONCE
        good = server_log()
        login2, quit1 = good[6], good[5]
        chat = [stamp + '<%s> %s' % (NONCE, l.split('] ', 1)[1])
                for l in (login2, quit1)]
        for log in (good, server_log(extra=[lost % 'quitting']),
                    good + chat, good[:4] + chat + good[4:]):
            self.assertIn(gap, verdict(lines=[], server=log) or '')
        for expected, log in (
                ('duplicate or unbound disconnect', good[:6] + good[5:]),
                ('duplicate or unbound disconnect', good[:3] + good[5:6]
                 + good[3:5] + good[6:]),
                ('duplicate or unbound disconnect',
                 server_log(extra=[lost % 'quitting'] * 2)),
                ('generation 2 lost connection',
                 server_log(extra=[lost % 'timeout'])),
                ('another player', server_log(extra=[
                    login2.replace(NONCE, 'cwdeadbe')])),
                ('2 server starts', good + good[:1]),
                ('0 server starts', good[1:]),
                ('logged in 1 times', server_log(ids=(730,))),
                ('logged in 1 times', good[:6] + chat),
                ('logged in 1 times', [l.replace('[INFO] ', '[INFO] <%s> '
                                                  % NONCE)
                                       if 'entity id 5445' in l else l
                                       for l in good]),
                ('not the pinned fixture port 25570', server_log(port=25565)),
                ('generation 2 joined from', server_log(
                    hosts=('192.0.2.27', '192.0.2.99'))),
                ('entity id 730 twice', server_log(ids=(730, 730))),
                ('generation 1 lost connection', good[:5] + good[6:]),
                ('generation 1 lost connection',
                 server_log(reason='disconnect.endOfStream')),
                ('warned about this player', server_log(extra=[
                    stamp.replace('INFO', 'WARNING') + NONCE
                    + ' moved too quickly!'])),
                ('typed /login', server_log(extra=[
                    stamp + NONCE + ' tried command: login x']))):
            with self.subTest(expected=expected):
                got = verdict(lines=[], server=log) or ''
                self.assertIn(expected, got)
                self.assertNotIn(gap, got)
        self.assertIn('two receipt sources', verdict(server=good) or '')

    def test_stale_and_unattributable_runs(self):
        self.assertIn('run id missing',
                      mod.MpPlayEvidence('').complete_error())
        other = mod.MpPlayEvidence(RUN_ID)
        other.note(dict(event=mod.MP_PLAY_EVENT, seq=1, marker='session',
                        run_id='someone_else'))
        self.assertIn('stale-run evidence', other.error)
        wrong_nonce = patched('session', nonce='cwdeadbe')
        self.assertIn('is not this run\'s derivation',
                      device_verdict(wrong_nonce))
        # A .dol built against a different contract cannot be graded by this
        # file's floors.
        self.assertIn('built against a different contract', device_verdict(
            patched('session', measured_floor_ms=300000)))

    # -- the committed fixture + the offline replay entry point -----------

    def test_committed_fixture_matches_the_generator(self):
        """tests/fixtures/mp_play/good/ is the very pair this file builds. If
        they drift, the fixture stops being evidence about the live rules."""
        tape = (FIXTURES / 'good' / 'telemetry.ndjson').read_text(
            encoding='utf-8')
        rows = [json.loads(line) for line in tape.splitlines() if line.strip()]
        self.assertEqual([row for row in rows
                          if row['event'] in (mod.MP_PLAY_EVENT, 'chat_login')],
                         marker_rows())
        log = (FIXTURES / 'good' / mod.CHAT_LOGIN_RECEIPT_NAME).read_text(
            encoding='utf-8')
        self.assertEqual(log.splitlines(), receipt_lines())

    def test_offline_replay_entry_point(self):
        """`--replay <run dir>`: verdict + reasons out, and the directory is
        never touched. This is what the lead runs on the first real PC-app run
        before any console time is spent."""
        script = str(ROOT / 'tools' / 'wii_telemetry_server.py')
        for name, code, expected in (
                ('good', 0, 'PASS'),
                ('duplicate_receipt', 1,
                 'inv_move n=2 pick-up click: 1 generation 1')):
            run_dir = FIXTURES / name
            before = {path.name: path.read_bytes()
                      for path in sorted(run_dir.iterdir())}
            with self.subTest(fixture=name):
                result = subprocess.run(
                    [sys.executable, script, '--replay', str(run_dir)],
                    capture_output=True, text=True, timeout=300)
                self.assertEqual(result.returncode, code,
                                 result.stdout + result.stderr)
                self.assertIn(expected, result.stdout)
                self.assertEqual(
                    {path.name: path.read_bytes()
                     for path in sorted(run_dir.iterdir())}, before,
                    'a replay must not modify the run directory')


def duplicate_receipt_rows():
    """The negative fixture: the second inventory move claims the SAME pair of
    action numbers as the first, so it is credited to receipts that are already
    spent -- and the inventory floor, where the floor IS the count, misses."""
    rows = marker_rows()
    first = rows[index_of(rows, 'inv_move')]
    second = index_of(rows, 'inv_move', 1)
    rows[second] = {**rows[second], 'action_pick': first['action_pick'],
                    'action_put': first['action_put']}
    return rows


def write_fixtures():
    """Materialise tests/fixtures/mp_play/ -- the same pair this file builds,
    written as a run directory the shipped `--replay` entry point can read.

    Regenerate with `python tests/test_mp_play_evidence.py --write-fixtures`;
    test_committed_fixture_matches_the_generator fails the day the two drift.
    The heartbeats are the run's own liveness evidence: the collector's floor
    for this profile is the measured cadence (0.8/s across 600 000 ms), so a
    tape that is short of them would fail for a reason that has nothing to do
    with the contract being demonstrated."""
    beats = [json.dumps(dict(event='heartbeat', load_screen_ms=3000, run_id=RUN_ID, tick=tick,
                             profile=mod.MP_PLAY_PROFILE, world_loaded=True,
                             lw_to_admits=0), separators=(',', ':'))
             for tick in range(1, 501)]
    for name, rows in (('good', marker_rows()),
                       ('duplicate_receipt', duplicate_receipt_rows())):
        out = FIXTURES / name
        out.mkdir(parents=True, exist_ok=True)
        markers = [json.dumps(row, separators=(',', ':')) for row in rows]
        tape = []
        for i, beat in enumerate(beats):
            tape.append(beat)
            if i < len(markers):
                tape.append(markers[i])
        tape.extend(markers[len(beats):])
        tape.append(json.dumps(dict(event='profile_done', run_id=RUN_ID,
                                    status='ok'), separators=(',', ':')))
        (out / 'telemetry.ndjson').write_text('\n'.join(tape) + '\n',
                                              encoding='utf-8', newline='\n')
        (out / mod.CHAT_LOGIN_RECEIPT_NAME).write_text(
            '\n'.join(receipt_lines()) + '\n', encoding='utf-8', newline='\n')
        print('wrote %s (%d tape lines)' % (out, len(tape)))


class MpPauseMenuEvidenceTest(unittest.TestCase):
    HOLD = 180000  # REL-15: 3x the vanilla 1200-tick read timeout

    def tape(self, falling=True, hold=HOLD):
        positions = [(0, 65.2, 0), (0, 65.1, 0), (0, 64.0, 0),
                     (0, 64.0 if falling else 65.2, 0),
                     (1.3, 64.0, 0), (1.3, 64.0, 0)]
        names = mod.MpPauseMenuEvidence.ORDER
        times = [1000, 1050, 1100 + hold, 1200 + hold, 3300 + hold, 3800 + hold]
        ticks = [100, 101, 162, 164, 206, 216]
        rows = []
        for i, name in enumerate(names):
            x, y, z = positions[i]
            rows.append(dict(event='mp_pause_menu', run_id=RUN_ID,
                             seq=i+1, marker=name, ms=times[i], tick=ticks[i],
                             x=x, y=y, z=z,
                             screen='pause' if i<3 else 'title' if i==5 else 'ingame',
                             remote=i<5, forward=i in (1, 2), login=True))
        return rows

    LONG = tuple(range(0, 185001, 1000))

    def collect_pause(self, falling, beat_times=LONG,
                      arrivals=None, missing_uptime=False, marker_offset=0,
                      hold=HOLD, session=None):
        with tempfile.TemporaryDirectory() as td:
            c = mod.Collector(mod._selftest_args(Path(td), RUN_ID,
                                                  'mp_pause_menu_3m'))
            arrival_base = mod.time.monotonic()
            def beat(tick):
                row = dict(event='heartbeat', load_screen_ms=3000, run_id=RUN_ID, tick=tick,
                           world_loaded=True, wd_hb_gap_max_ms=1000,
                           mp_login=1, mp_end=0, mp_gen=1, mp_reconn=0,
                           mp_rx_kb=40 + tick // 2)
                row.update((session or {}).get(tick, {}))
                if not (missing_uptime and tick == 3):
                    row['up_ms'] = beat_times[tick - 1]
                if arrivals is None:
                    c.process(row)
                else:
                    with patch.object(mod.time, 'monotonic',
                                      return_value=arrival_base + arrivals[tick - 1] / 1000):
                        c.process(row)
            beat(1)
            for i, row in enumerate(self.tape(falling, hold)):
                row['up_ms'] = row['ms'] + marker_offset
                c.process(row)
                if i == 1:
                    for tick in (2, 3):
                        beat(tick)
            for tick in range(4, len(beat_times) + 1):
                beat(tick)
            c.process(dict(event='profile_done', run_id=RUN_ID, status='ok'))
            return c.fail_reason, c.mp_pause_menu.summary()

    def test_pause_menu_pass_and_frozen_body_fail(self):
        self.assertEqual(mod.PROFILES['mp_pause_menu_3m']['duration'], 180)
        reason, summary = self.collect_pause(True)
        self.assertIsNone(reason, reason)
        self.assertEqual(summary['hold_heartbeats'], 2)
        reason, _ = self.collect_pause(False)
        self.assertIn('did not fall', reason)

    def test_pause_menu_short_dwell_fails(self):
        reason, _ = self.collect_pause(True, tuple(range(0, 8001, 1000)), hold=3000)
        self.assertEqual(reason, 'pause hold did not last 180000 ms in marker order')

    def test_pause_menu_session_must_stay_connected(self):
        """REL-15: a kick, timeout or reconnect inside the open-menu window
        fails even when heartbeats and markers look complete."""
        cases = (({90: dict(mp_gen=2)}, 'mp_gen 1 -> 2'),
                 ({90: dict(mp_reconn=1)}, 'mp_reconn 0 -> 1'),
                 ({90: dict(mp_end=3)}, 'mp_end 3'),
                 ({90: dict(mp_login=0)}, 'mp_login 0'),
                 ({t: dict(mp_rx_kb=40) for t in range(1, 187)}, 'no network progress'))
        for session, needle in cases:
            with self.subTest(needle=needle):
                reason, _ = self.collect_pause(True, session=session)
                self.assertIsNotNone(reason)
                self.assertIn(needle, reason)

    def test_pause_menu_completion_is_driver_owned(self):
        """The 180 s dwell outlives the staged --duration 180: only
        profile_done or the dedicated cap may end the collection."""
        with tempfile.TemporaryDirectory() as td:
            c = mod.Collector(mod._selftest_args(Path(td), RUN_ID,
                                                  'mp_pause_menu_3m', duration=180))
            c.process(dict(event='heartbeat', load_screen_ms=3000, run_id=RUN_ID, tick=1, up_ms=0,
                           world_loaded=True, wd_hb_gap_max_ms=1000))
            base = c.first_hb
            for t, final in ((200, False), (mod.MP_PAUSE_MENU_CAP_S + 2, True)):
                c.last_hb = base + t
                with patch.object(mod.time, 'monotonic', return_value=base + t):
                    c.tick()
                self.assertEqual(c.fail_reason is not None, final, (t, c.fail_reason))
            self.assertIn('completion deadline', c.fail_reason)

    def test_pause_menu_heartbeat_window_empty_fails(self):
        reason, _ = self.collect_pause(True, (0, 1000, 20000, 21000, 22000),
                                       marker_offset=10000, hold=3000)
        self.assertEqual(reason, 'mp_pause_menu heartbeat window empty '
                         '(0 heartbeats between up_ms 6000 and 16300, expected >= 9)')

    def test_pause_menu_heartbeat_window_one_fails(self):
        reason, _ = self.collect_pause(True, (0, 7000, 20000, 21000, 22000),
                                       marker_offset=10000, hold=3000)
        self.assertEqual(reason, 'mp_pause_menu heartbeat window empty '
                         '(1 heartbeats between up_ms 6000 and 16300, expected >= 9)')

    def test_pause_menu_heartbeat_hold_coverage_fails(self):
        reason, _ = self.collect_pause(True, (2000, 3000, 4000, 5000, 6000), hold=3000)
        self.assertEqual(reason, 'mp_pause_menu heartbeat hold coverage missing '
                         '(first pair starts at up_ms 2000, after pause_open up_ms 1000)')
        reason, _ = self.collect_pause(True, (0, 1000, 2000, 3000, 4000), hold=3000)
        self.assertEqual(reason, 'mp_pause_menu heartbeat hold coverage missing '
                         '(last pair ends at up_ms 4000, before pause_close up_ms 4200)')

    def test_pause_menu_elapsed_heartbeat_gap_fails(self):
        reason, _ = self.collect_pause(True, (0, 1000, 3600, 4600, 5600), hold=3000)
        self.assertIn('seq 2 -> 3', reason)
        self.assertIn('up_ms 1000 -> 3600', reason)
        self.assertIn('2600 ms', reason)

    def test_pause_menu_gap_before_window_passes(self):
        reason, _ = self.collect_pause(True, (0, 2600) + tuple(range(6000, 194001, 1000)),
                                       marker_offset=10000)
        self.assertIsNone(reason, reason)

    def test_pause_menu_gap_inside_window_has_full_attribution(self):
        reason, _ = self.collect_pause(True, (6000, 7000, 9600, 10600, 11600,
                                              12600, 13600, 14600, 15600),
                                       marker_offset=10000, hold=3000)
        self.assertIn('seq 2 -> 3 up_ms 7000 -> 9600 gap 2600 ms', reason)

    def test_pause_menu_gap_after_window_passes(self):
        reason, _ = self.collect_pause(True, tuple(range(6000, 194001, 1000)) + (197600,),
                                       marker_offset=10000)
        self.assertIsNone(reason, reason)

    def test_pause_menu_device_uptime_overrides_arrival(self):
        reason, _ = self.collect_pause(True, (0, 1000, 4000, 5000, 6000),
                                       arrivals=(0, 1000, 2000, 3000, 4000), hold=3000)
        self.assertIn('seq 2 -> 3', reason)
        self.assertIn('up_ms 1000 -> 4000', reason)
        reason, _ = self.collect_pause(True, arrivals=tuple(
            t + (2000 if t >= 2000 else 0) for t in self.LONG))
        self.assertIsNone(reason, reason)
        reason, _ = self.collect_pause(True, missing_uptime=True)
        self.assertIn('heartbeat_no_uptime', reason)

    def test_pause_menu_site_outcomes_are_distinct(self):
        for outcome in ('site_unverified', 'apex_short'):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as td:
                c = mod.Collector(mod._selftest_args(Path(td), RUN_ID,
                                                      'mp_pause_menu_3m'))
                c.process(dict(event='mp_pause_menu', run_id=RUN_ID,
                               marker=outcome, seq=1, ms=1000))
                c.process(dict(event='profile_done', run_id=RUN_ID,
                               status=outcome))
                self.assertEqual(c.fail_reason, outcome)

    def test_pause_menu_driver_failure_carries_cause(self):
        with tempfile.TemporaryDirectory() as td:
            c = mod.Collector(mod._selftest_args(Path(td), RUN_ID,
                                                  'mp_pause_menu_3m'))
            c.process(dict(event='mp_pause_menu', run_id=RUN_ID, marker='fail',
                           seq=1, ms=1000, reason='timeout:hold'))
            c.process(dict(event='profile_done', run_id=RUN_ID,
                           status='mp_pause_menu_failed'))
            self.assertIn('timeout:hold', c.fail_reason)

    def test_pause_menu_driver_failure_without_reason_fails(self):
        with tempfile.TemporaryDirectory() as td:
            c = mod.Collector(mod._selftest_args(Path(td), RUN_ID,
                                                  'mp_pause_menu_3m'))
            c.process(dict(event='mp_pause_menu', run_id=RUN_ID, marker='fail',
                           seq=1, ms=1000))
            c.process(dict(event='profile_done', run_id=RUN_ID,
                           status='mp_pause_menu_failed'))
            self.assertIn('failure missing reason', c.fail_reason)


class SplitPortalQuitEvidenceTest(unittest.TestCase):
    """Checklist 6b marker tapes: ordering and guest persistence are fatal."""

    @staticmethod
    def tape():
        names = ('portal_lit', 'dwell_trigger', 'quit_requested',
                 'race_unload_first', 'save_settled', 'title_entered',
                 'reload_verified', 'guest_rejoined', 'run_ok', 'run_summary')
        rows = [dict(event='split_portal_quit', run_id=RUN_ID, seq=i + 1,
                     marker=name, up_ms=1000 + i * 50, tick=20 + i,
                     remaining_ticks=2, ok=True)
                for i, name in enumerate(names)]
        rows[6].update(host_inventory_match=True, blocks_match=True,
                       position_match=True, region_intact=True)
        rows[7].update(guest_inventory_match=True, guest_phase='ACTIVE')
        return rows

    @staticmethod
    def grade(rows):
        proof = mod.PortalQuitEvidence(RUN_ID, 2)
        for row in rows:
            if row['event'] == 'split_recovery':
                proof.note_recovery(row)
            elif row['event'] == 'split_portal_quit_server':
                proof.note_server(row)
            else:
                proof.note(row)
        proof.note_profile_done('ok')
        return proof.complete_error()

    @staticmethod
    def switch_tape():
        rows = SplitPortalQuitEvidenceTest.tape()
        rows[3]['marker'] = 'confirm_sent'
        rows.insert(4, dict(rows[3], marker='switch_begin'))
        rows.insert(5, dict(rows[3], marker='race_switch_first'))
        rows[2], rows[3] = rows[3], rows[2]
        for i, row in enumerate(rows):
            row.update(seq=i + 1, up_ms=1000 + i * 50, tick=20 + i)
        rows[5].update(confirm_tick=rows[2]['tick'],
                       confirm_up_ms=rows[2]['up_ms'],
                       switch_tick=rows[4]['tick'],
                       switch_up_ms=rows[4]['up_ms'])
        rows[-1]['winning_leg'] = 2
        rows[-2]['winning_leg'] = 2
        return rows

    @staticmethod
    def server_marker(marker='unload_seen', switch='begun'):
        return dict(event='split_portal_quit_server', run_id=RUN_ID,
                    marker=marker, seq=1, up_ms=1150, tick=23,
                    switch=switch, switch_begin_up_ms=(1125 if switch != 'not_begun' else 0),
                    request_id=None)

    def test_pass_tape(self):
        rows = self.recovery_blocks() + self.switch_tape()
        rows.insert(9, self.server_marker())
        self.assertIsNone(self.grade(rows))
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'portal', rows, [],
                                heartbeats=180,
                                profile=mod.PORTAL_QUIT_PROFILE)
            self.assertIsNone(collector.fail_reason)
            self.assertTrue(collector.pass_reason)

    def test_frame_clock_skew_after_switch(self):
        rows = self.recovery_blocks() + self.switch_tape()
        rows.insert(9, self.server_marker())
        # Old main.c used hw_started for frame markers, which starts later
        # than the telemetry clock used by confirm_sent and switch_begin.
        old_clock = [dict(row) for row in rows]
        for row in old_clock:
            if row['event'] == 'split_portal_quit' and row['marker'] not in (
                    'confirm_sent', 'switch_begin'):
                row['up_ms'] -= 200
        self.assertEqual(old_clock[7]['marker'], 'switch_begin')
        self.assertEqual(old_clock[8]['marker'], 'race_switch_first')
        self.assertLess(old_clock[8]['up_ms'], old_clock[7]['up_ms'])
        self.assertIn('portal quit clock reversed', self.grade(old_clock))
        self.assertIsNone(self.grade(rows))

    def test_client_order_without_server_order_is_not_race(self):
        self.assertIn('race_not_reached', self.grade(
            self.recovery_blocks() + self.switch_tape()))

    def test_server_unload_seen_after_switch_qualifies(self):
        rows = self.recovery_blocks() + self.switch_tape()
        rows.insert(9, self.server_marker())
        self.assertIsNone(self.grade(rows))

    def test_server_wire_arrival_before_confirm_marker_uses_clock(self):
        rows = self.recovery_blocks() + self.switch_tape()
        rows.insert(3, self.server_marker())
        self.assertIsNone(self.grade(rows))

    def test_server_switch_began_before_confirm_is_not_race(self):
        rows = self.recovery_blocks() + self.switch_tape()
        marker = self.server_marker()
        marker['switch_begin_up_ms'] = 1050
        rows.insert(9, marker)
        self.assertIn('race_not_reached', self.grade(rows))

    def test_server_barrier_drop_is_named_defect(self):
        rows = self.recovery_blocks() + self.switch_tape()[:6]
        rows.append(self.server_marker('unload_dropped_at_barrier', 'barrier'))
        self.assertIn('unload_dropped_silently', self.grade(rows))

    def test_server_unload_seen_before_switch_is_unload_won(self):
        rows = self.recovery_blocks() + self.switch_tape()
        rows.insert(9, self.server_marker(switch='not_begun'))
        self.assertIn('race_not_reached', self.grade(rows))

    def test_unload_first_without_switch_is_not_race(self):
        self.assertIn('race_not_reached', self.grade(self.tape()))
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'no_switch', self.tape(), [],
                                heartbeats=180, profile=mod.PORTAL_QUIT_PROFILE)
            self.assertIn('race_not_reached', collector.fail_reason)

    def test_missing_recovery_blocks_fail(self):
        rows = self.switch_tape() + [self.server_marker()]
        self.assertIn('block_placed', self.grade(rows))
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'no_blocks', rows, [],
                                heartbeats=180, profile=mod.PORTAL_QUIT_PROFILE)
            self.assertIn('block_placed', collector.fail_reason)

    @staticmethod
    def recovery_blocks():
        return [dict(event='split_recovery', run_id=RUN_ID,
                     phase='p0', marker='block_placed', i=i,
                     x=i, y=64, z=0, id=3, meta=0, ok=True)
                for i in range(3)]

    def test_late_reply_is_distinct_from_drop(self):
        rows = self.switch_tape()[:6]
        rows += [dict(rows[-1], marker='unload_deadline', up_ms=32000,
                      tick=700), dict(rows[-1], marker='unload_replied_late',
                                      up_ms=33000, tick=720)]
        for i, row in enumerate(rows): row['seq'] = i + 1
        self.assertIn('unload_replied_late', self.grade(rows))
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'late', rows, [], heartbeats=180,
                                profile=mod.PORTAL_QUIT_PROFILE)
            self.assertIn('unload_replied_late', collector.fail_reason)
            self.assertNotIn('unload_dropped_silently', collector.fail_reason)

    def test_no_reply_by_run_end_is_drop(self):
        rows = self.switch_tape()[:6]
        rows.append(dict(rows[-1], marker='unload_deadline', up_ms=32000,
                         tick=700))
        for i, row in enumerate(rows): row['seq'] = i + 1
        rows = self.recovery_blocks() + rows
        rows.append(self.server_marker())
        self.assertIn('unload_dropped_silently', self.grade(rows))
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'drop', rows, [], heartbeats=180,
                                profile=mod.PORTAL_QUIT_PROFILE)
            self.assertIn('unload_dropped_silently', collector.fail_reason)
        self.assertIn('race_not_reached',
                      self.grade(self.recovery_blocks() + self.switch_tape()[:6]))

    def test_title_before_save_settled_fails(self):
        rows = self.tape()
        rows[4]['marker'], rows[5]['marker'] = (
            rows[5]['marker'], rows[4]['marker'])
        self.assertIn('title_before_save_settled', self.grade(rows))
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'early_title', rows, [],
                                heartbeats=180,
                                profile=mod.PORTAL_QUIT_PROFILE)
            self.assertIn('title_before_save_settled', collector.fail_reason)

    def test_guest_inventory_lost_fails(self):
        rows = self.tape()
        rows[7]['guest_inventory_match'] = False
        self.assertIn('guest_inventory_lost', self.grade(rows))
        with tempfile.TemporaryDirectory() as td:
            collector = collect(td, 'guest_lost', rows, [],
                                heartbeats=180,
                                profile=mod.PORTAL_QUIT_PROFILE)
            self.assertIn('guest_inventory_lost', collector.fail_reason)

    def test_switch_refusal_requires_visible_retry(self):
        rows = self.switch_tape()
        rows[6:6] = [dict(rows[6], marker='save_refused',
                          retry_visible=True),
                     dict(rows[6], marker='retry_requested',
                          production_srpc=True)]
        for i, row in enumerate(rows):
            row['seq'] = i + 1
            row['up_ms'] = 1000 + i * 50
            row['tick'] = 20 + i
        rows.append(self.server_marker())
        self.assertIsNone(self.grade(self.recovery_blocks() + rows))
        missing_retry = [row for row in rows
                         if row['marker'] != 'retry_requested']
        for i, row in enumerate(missing_retry):
            row['seq'] = i + 1
        self.assertIn('switch refusal without retry',
                      self.grade(self.recovery_blocks() + missing_retry))

    def test_silent_drop_is_named_failure(self):
        rows = self.tape()
        rows[3]['marker'] = 'race_switch_first'
        rows[4] = {**rows[4], 'marker': 'fail', 'ok': False,
                   'reason': 'unload_dropped_silently'}
        self.assertIn('unload_dropped_silently', self.grade(rows))

# Source: run dir tmp/jobs/push-0921/wii-runs-split3/a59_r2_split/, file telemetry.ndjson, line 54, artifact stamp fabac43_09.22, sha256 7ed2059f1d5c53b7fed2120ea41c2bb9bc05ce955e4f26920ab9f16d1758eec6, date 2026-09-23.
REAL_WII_HEARTBEAT_ROW = json.loads(r'''{"event":"heartbeat","run_id":"a59_r2_split","profile":"split_play_8m","build_id":"fabac43_09.22","tick":44,"fps":60.16,"gpu_ms":4.03,"vsync_ms":9.62,"ms_bfs":0.38,"ms_light":0.0,"ms_lod":0.11,"ms_build":0.11,"ms_recv":0.01,"ms_render":0.33,"ms_tick":0.59,"ms_bfs_max":1.06,"ms_light_max":4.14,"ms_lod_max":0.81,"ms_build_max":7.58,"chunks":193,"mesh_q":0,"mesh_stale":52,"mesh_trunc":0,"estk":0,"near_evict_defer":0,"srv_rt_us":893,"srv_wt_us":83,"heap_used_kb":6958,"heap_free_kb":395,"hblk":688,"htop_kb":16,"mem1_free_kb":9528,"mem2_free_kb":1371,"spill":0,"oom":0,"dlbad":0,"dlcall":0,"wpad":-1,"wksp":146216,"wkspT":262120,"wkspU":115912,"wkspB":1,"x":0.5,"y":86.62,"z":0.5,"rx_deg":-0.0,"ry_deg":90.0,"near_radius":2,"far_view_distance":6,"fring":0,"far_missing":0,"far_missing_max":0,"far_settle_ms":0,"srv_gen_us":13372,"srv_gen_cols":1,"srv_gen_backlog":47,"srv_loop_it":20,"srv_elapsed_ms":7,"srv_ticks_pend":0,"srv_partial_m":140,"srv_sleep_ms":43,"srv_far_us_max":0,"cw_set_n":0,"lw_to_admits":0,"cflood":949,"ndisp":307,"wd_main":20,"wd_srv":13,"wd_worker":1,"wd_hb_gap_max_ms":967,"world_loaded":true,"map_step":0,"map_max":0,"np_entered":0,"np_skipped":0,"np_focus":0,"np_flags":0,"np_rows":0,"map_sign":false,"smig_state":0,"smig_ms":0,"smig_regions":0,"smig_chunks":0,"smig_signs":0,"smig_expected":0,"smig_verified":0,"smig_chests":0,"smig_chests_expected":0,"smig_chests_verified":0,"smig_spawners":0,"smig_spawners_expected":0,"smig_spawners_verified":0,"mp_state":0,"mp_gen":0,"mp_login":0,"mp_ret_kb":0,"mp_ret_n":0,"mp_ready":0,"mp_backlog":0,"mp_evt_q":0,"mp_evict":0,"mp_alloc_fail":0,"mp_reconn":0,"wl_pending":0,"wl_failed":0,"mp_rx_kb":0,"mp_end":0,"mp_end_phase":0,"mp_end_rx":0,"mp_end_reason":"","mp_dec_phase":0,"mp_dec_pkt":0,"mp_dec_have":0,"mp_dec_need":0,"mp_adm_radius":0,"mp_resident_cols":0,"mp_backed_cols":0,"mp_backing_kb":0,"mp_backing_peak_kb":0,"mp_demotions":0,"mp_compactions":0,"mp_backing_refusals":0,"mp_ready_pending":0,"music_key":4,"music_ms":28245,"music_underruns":3,"music_starts":332,"music_rejects":0,"music_dur":85,"mrs_cycles":0,"mrs_target":0,"mrs_wedge":false,"creative":false,"dlcalls":352,"tris":40690,"ms_mesh_worker":10.78,"ms_mesh_worker_max":305.95,"mem2_lock_wait":2,"tel_fail":0,"tel_last_errno":0,"tel_quiesced":false,"frame_counter":2555,"mem2_carve_free_kb":16809,"chunks_meshed":2488,"near_missing":0,"near_mesh_pending":0,"near_missing_max":7,"near_settle_ms":0,"edit_to_visible_frames":273,"load_screen_ms":14764,"player_health":160,"loading_screen":false,"perf_frames":1668,"perf_max_us":109394,"perf_over_30ms":27,"perf_gpu_over_16600":0,"perf_gpu_over_8000":0,"perf_invalid":0,"perf_duration_us":28112760,"far_storage":0,"vi_ready_max_us":516,"vi_ready_over_8000":0,"gpu_poll_late_max_us":690,"gpu_poll_late_over_8000":0,"server_time":504,"sd_trace_max_us":92697,"sd_trace_over_30ms":7,"tel_drop":0,"tel_it":101,"tel_sends":1,"tel_smax_us":4766,"tel_qmax":3,"tel_hb_repl":0,"tel_wto":0,"tel_stg":0,"tel_stg_us":5000,"tel_pmax_us":238,"tel_xmax_us":1897,"tel_abd":0,"tel_pret":1,"tel_prev":8,"tel_sret":1,"srv_us":[223761,1323069,848305,3660665,236130,14167353,301,207662,1805,27136,88149],"guard_scan_us":[2635,658],"world_us":[9615713,2182260,1259369,553062,892272,1147359,3546105,189285,0,0,0,801439,1010036,6],"worst_parts_us":[0,2,344,388,1,146,131,4035,9770,21,10,1857,1,41,76,2,0,0,5,2],"split_live":1,"split_pending":0,"split_toggle_ms":0,"split_last_ok":-1,"deaths":0,"death_cause":"none","anchor_sep":0,"anchor_ready":true,"anchor_measuring":true,"anchor_ms":0,"anchor_collision":false,"anchor_near_owed":[0,0],"anchor_oldest":[0,0],"anchor_missing":[0,0],"anchor_mesh_pending":[0,0],"anchor_far_missing":[0,0],"anchor_far_max":[0,0],"anchor_far_mesh_pending":[0,0],"anchor_ready_streak":3,"anchor_load_faults":[0,0,0],"anchor_run_faults":[0,0,0],"anchor_deaths":0,"anchor_walk":[0,0,0,0,0],"actual_tps":19.981,"tick_backlog":1,"anchor_load_carve_min":15908,"anchor_run_carve_min":16809,"anchor_frame_total":0,"anchor_frame_dropped":0,"anchor_frame_us":[],"anchor_death_cause":"none","anchor_damage_cause":"unknown","act0":[0,0,0,0,0,0],"act1":[0,0,0,0,0,0],"act_health":[160,160],"item_nbt_exhaustions":0,"op":5,"oph":39,"opc":256,"opf":0,"oer":0,"orc":0,"ocy":0,"oex":0,"owu":0,"db":81,"ds":81,"dln":1549,"dls":0,"dlp":0,"dlbp":1691,"dlbo":0,"dlbf":0,"dlbm":3,"dlbs":0,"dlbc":34048,"dlbx":0,"srv_ph":[0,21260,0,41308,6320,1,23,43197,0,0,0,0,293,888252,0,0,0,0,0,160,0,0,0,0],"tclk":[40,1000,20,0,0,0,869,876,20,20,1,40,0,0],"gx":0.5,"gy":86.62,"gz":0.5,"guest_head_block":0,"guest_air":300,"split_reach":4,"server_radius":4}''')


class SplitGuestProgressTest(unittest.TestCase):
    """Live split guest positions must prove walking throughout measured play.

    Missing/non-finite progress fields are guest_progress_missing. Repeated or
    decreasing ticks are present-but-invalid evidence: guest_progress_invalid.
    Every tape row enters through ingest_telemetry_line(), the live TCP line
    parser, rather than passing a pre-shaped dictionary to Collector.process().
    """

    @staticmethod
    def ingest_line(c, line):
        ingest = getattr(mod, 'ingest_telemetry_line', None)
        if ingest is None:
            # RED-only compatibility: let the new assertions reach the
            # unchanged gate before the production line helper exists.
            c.process(json.loads(line))
            return
        ingest(c, line)

    def tape(self, td, positions, pre_positions=(), mutate=None):
        c = mod.Collector(mod._selftest_args(
            Path(td), RUN_ID, 'split_play_8m',
            expected_build=REAL_WII_HEARTBEAT_ROW['build_id']))
        for i, gx in enumerate([*pre_positions, *positions]):
            measured_i = i - len(pre_positions)
            beat = dict(REAL_WII_HEARTBEAT_ROW, **mod.PC_POOL_EVIDENCE, run_id=RUN_ID,
                        tick=i + 1, gx=gx, gz=0.0,
                        x=max(measured_i, 0) * 0.3,
                        anchor_measuring=measured_i >= 0,
                        anchor_ready=measured_i >= 0,
                        anchor_ms=max(measured_i, 0) * 1000,
                        anchor_frame_total=i + 1,
                        anchor_frame_us=[16667], server_time=i * 20,
                        anchor_load_carve_min=1000,
                        anchor_run_carve_min=2246)
            if measured_i >= 0 and mutate is not None:
                mutate(beat, measured_i)
            line = json.dumps(beat, separators=(',', ':'))
            self.ingest_line(c, line)
            if c.done:
                break
        if not c.done:
            line = json.dumps(dict(event='profile_done', run_id=RUN_ID,
                                   status='ok'), separators=(',', ':'))
            self.ingest_line(c, line)
        c.write_result()
        return c, json.loads((c.out / ('FAIL.json' if c.fail_reason else 'PASS.json')).read_text())

    def test_walk_throughout_passes(self):
        with tempfile.TemporaryDirectory() as td:
            c, proof = self.tape(td, [i * 0.3 for i in range(361)])
            self.assertEqual(proof['status'], 'PASS', c.fail_reason)
            self.assertGreaterEqual(proof['min_window_displacement'], 8)
            self.assertAlmostEqual(proof['guest_total_distance_walked'], 108, places=5)

    def test_tape_uses_live_line_ingest(self):
        self.assertTrue(callable(getattr(mod, 'ingest_telemetry_line', None)))

    def test_replay_and_direct_line_ingest_are_identical(self):
        ingest = getattr(mod, 'ingest_telemetry_line', None)
        self.assertTrue(callable(ingest))
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            run_dir = root / 'run'
            run_dir.mkdir()
            lines = []
            for i in range(12):
                beat = dict(REAL_WII_HEARTBEAT_ROW, **mod.PC_POOL_EVIDENCE, run_id=RUN_ID,
                            tick=i + 1, gx=float(i), gz=0.0,
                            anchor_ms=i * 6000, anchor_frame_total=i + 1,
                            anchor_frame_us=[16667], server_time=i * 120,
                            anchor_load_carve_min=1000,
                            anchor_run_carve_min=2246)
                if i == 11:
                    beat.pop('gx')
                lines.append(json.dumps(beat, separators=(',', ':')))
            lines.insert(5, '{malformed replay row')
            (run_dir / 'telemetry.ndjson').write_text(
                '\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
            (run_dir / 'FAIL.json').write_text(json.dumps(dict(
                run_id=RUN_ID, profile='split_play_8m')), encoding='utf-8')

            direct = mod.Collector(mod._selftest_args(
                root / 'direct', RUN_ID, 'split_play_8m',
                expected_build=REAL_WII_HEARTBEAT_ROW['build_id']))
            for line in lines:
                if direct.done or ingest(direct, line):
                    break

            made = []
            collector_type = mod.Collector

            def capture(args):
                collector = collector_type(args)
                made.append(collector)
                return collector

            replay_args = mod._selftest_args(
                root / 'unused', RUN_ID, 'split_play_8m')
            replay_args.replay = str(run_dir)
            replay_args.profile_override = ''
            mod.Collector = capture
            try:
                self.assertEqual(mod.replay_run_dir(replay_args), 1)
            finally:
                mod.Collector = collector_type
            replay = made[0]

            self.assertEqual(
                (replay.failure_class, replay.fail_reason,
                 replay.min_window_displacement,
                 replay.guest_total_distance_walked),
                (direct.failure_class, direct.fail_reason,
                 direct.min_window_displacement,
                 direct.guest_total_distance_walked))
            self.assertEqual(replay.failure_class, 'guest_progress_missing')
            self.assertEqual(replay.bad_lines, direct.bad_lines)
            self.assertEqual(replay.bad_lines, 1)
            self.assertEqual(replay.bad_lines_consecutive,
                             direct.bad_lines_consecutive)

    def test_mid_run_stop_fails_named_window(self):
        with tempfile.TemporaryDirectory() as td:
            positions = [min(i, 150) * 0.3 for i in range(361)]
            c, proof = self.tape(td, positions)
            self.assertEqual(proof['failure_class'], 'guest_stalled')
            self.assertEqual(proof['window_start_tick'], 125)
            self.assertEqual(proof['window_end_tick'], 185)
            self.assertLess(proof['displacement'], 8)

    def test_retrace_counts_path_but_tiny_oscillation_fails(self):
        with tempfile.TemporaryDirectory() as td:
            positions = [i * 0.3 if i <= 180 else (360 - i) * 0.3
                         for i in range(361)]
            _, proof = self.tape(Path(td) / 'retrace', positions)
            self.assertEqual(proof['status'], 'PASS', proof['reason'])
            self.assertGreaterEqual(proof['min_window_displacement'], 8)
            _, tiny = self.tape(Path(td) / 'tiny',
                                [0.01 if i % 2 else 0.0 for i in range(361)])
            self.assertEqual(tiny['failure_class'], 'guest_stalled')

    def test_pre_measurement_heartbeats_excluded(self):
        with tempfile.TemporaryDirectory() as td:
            _, proof = self.tape(td, [i * 0.3 for i in range(361)],
                                 pre_positions=[1000.0] * 70)
            self.assertEqual(proof['status'], 'PASS', proof['reason'])
            self.assertAlmostEqual(proof['guest_total_distance_walked'], 108, places=5)

    def test_missing_coordinates_or_time_fails(self):
        for field in ('gx', 'gz', 'anchor_ms'):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as td:
                def remove_field(beat, _measured_i, field=field):
                    beat.pop(field)

                _, proof = self.tape(td, [0.0], mutate=remove_field)
                self.assertEqual(proof['failure_class'], 'guest_progress_missing')

    def test_non_finite_coordinates_fail_as_missing(self):
        # Python's live json.loads accepts the wire literals NaN, Infinity and
        # -Infinity, so exercise those exact serialized forms rather than a
        # pre-shaped float or a replacement null/string.
        cases = (('gx_nan', 'gx', float('nan'), 'NaN'),
                 ('gx_inf', 'gx', float('inf'), 'Infinity'),
                 ('gz_nan', 'gz', float('nan'), 'NaN'),
                 ('gz_inf', 'gz', float('-inf'), '-Infinity'))
        for name, field, value, literal in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory() as td:
                def set_non_finite(beat, _measured_i,
                                   field=field, value=value, literal=literal):
                    beat[field] = value
                    self.assertIn('"%s":%s' % (field, literal),
                                  json.dumps(beat, separators=(',', ':')))

                _, proof = self.tape(td, [0.0], mutate=set_non_finite)
                self.assertEqual(proof['failure_class'], 'guest_progress_missing')

    def test_non_increasing_tick_fails_as_invalid(self):
        for name, bad_tick in (('repeat', 1), ('decrease', 0)):
            with self.subTest(case=name), tempfile.TemporaryDirectory() as td:
                def change_second_tick(beat, measured_i, bad_tick=bad_tick):
                    if measured_i == 1:
                        beat['tick'] = bad_tick

                _, proof = self.tape(td, [0.0, 0.3],
                                     mutate=change_second_tick)
                self.assertEqual(proof['failure_class'],
                                 'guest_progress_invalid')
                self.assertEqual(proof['reason'],
                                 'guest progress tick did not increase')


if __name__ == '__main__':
    if '--write-fixtures' in sys.argv:
        write_fixtures()
    else:
        unittest.main()
