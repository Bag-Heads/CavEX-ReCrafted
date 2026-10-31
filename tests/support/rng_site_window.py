"""Explicit first-divergence window; preserve whole-capture inventories."""
from pathlib import Path
import gzip, hashlib, json, os, struct, sys
root = Path(__file__).resolve().parents[2]
ev = root / 'tmp/jobs/integ-1011/evidence'
max_tick = int(os.environ.get('CW_RNG_WINDOW_TICK', '40'))
assert max_tick >= 0
for pattern in sys.argv[1:]:
    for p in ev.glob(pattern):
        out = p.with_name(p.stem + '-window.gz'); total = kept = 0
        with gzip.open(p, 'rb') as source, gzip.open(out, 'wb', compresslevel=6) as target:
            while block := source.read(96 * 8192):
                assert len(block) % 96 == 0
                window = []
                for offset in range(0, len(block), 96):
                    total += 1
                    if struct.unpack_from('<Q', block, offset + 16)[0] <= max_tick:
                        kept += 1; window.append(block[offset:offset+96])
                target.write(b''.join(window))
        h = hashlib.sha256()
        with p.open('rb') as original:
            while block := original.read(1024 * 1024): h.update(block)
        p.with_name(p.stem + '-window.json').write_text(json.dumps({'input':p.name,
            'input_sha256':h.hexdigest(), 'input_draws':total, 'kept_draws':kept, 'max_tick':max_tick}))
        p.unlink(); print(p.name, total, '->', kept, flush=True)
