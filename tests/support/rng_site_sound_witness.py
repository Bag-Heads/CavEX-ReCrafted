"""Find the exact F3 float pair for PR-03's first sheep SOUND payload."""
from pathlib import Path
import gzip, json, struct
root = Path(__file__).resolve().parents[2]
ev = root / 'tmp/jobs/integ-1011/evidence'
name = 'rng-HEAD-test_block_hooks_actor_golden'
sites = json.loads((ev / (name + '-sites.json')).read_text())
first_sites = {int(a) for a, (fn, line) in sites.items()
               if 'entity_monster.h:144' in line}
second_sites = {int(a) for a, (fn, line) in sites.items()
                if 'entity_monster.h:145' in line}
def f32(x): return struct.unpack('<f', struct.pack('<f', x))[0]
pending = {}; matches = []; pairs = 0
capture = ev / (name + '.gz')
if not capture.exists(): capture = ev / (name + '-window.gz')
with gzip.open(capture, 'rb') as source:
    while block := source.read(96 * 8192):
        for r in struct.iter_unpack('<12Q', block):
            if r[0] != 1: continue
            if any(a in first_sites for a in r[7:]): pending[r[1]] = r
            elif any(a in second_sites for a in r[7:]) and r[1] in pending:
                a = pending.pop(r[1]); pairs += 1
                pitch = f32(f32(f32(a[3] / (1 << 24) - r[3] / (1 << 24)) * f32(0.2)) + 1)
                if struct.pack('<f', pitch).hex() == '0444943f':
                    matches.append({'first': a, 'second': r, 'pitch_bits':'0444943f',
                        'first_stack':[sites.get(str(pc)) for pc in a[7:]],
                        'second_stack':[sites.get(str(pc)) for pc in r[7:]]})
(ev / '2c-rpc-sound-witness.json').write_text(json.dumps({'pairs':pairs, 'matches':matches}, indent=2))
print('owned sound pairs', pairs, 'exact PR-03 pitch matches', len(matches))
