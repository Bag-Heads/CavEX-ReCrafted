"""Retain exact named witness-window draws and a complete call-site inventory.
Large raw captures may be deleted after this explicitly bounded window and
whole-capture SHA256/count inventory are written. No re-pin proof is inferred
from this scoped extraction: the omitted draws are not replayed.
"""
from pathlib import Path
import collections, gzip, hashlib, json, struct, sys
root = Path(__file__).resolve().parents[2]
ev = root / 'tmp/jobs/integ-1011/evidence'
for pattern in sys.argv[1:]:
    for p in ev.glob(pattern):
        sites = json.loads(p.with_name(p.stem + '-sites.json').read_text())
        counts = collections.Counter(); total = named = 0
        kept = p.with_name(p.stem + '-named.gz')
        with gzip.open(p, 'rb') as source, gzip.open(kept, 'wb', compresslevel=1) as output:
            while block := source.read(96 * 8192):
                assert len(block) % 96 == 0
                for r in struct.iter_unpack('<12Q', block):
                    total += 1
                    counts[(r[0], r[5], r[7:])] += 1
                    # First unresolved golden records occur by world tick35.
                    # This explicit witness window is not a whole-golden proof.
                    if r[5] and r[2] <= 40:
                        named += 1
                        output.write(struct.pack('<12Q', *r))
        h = hashlib.sha256()
        with p.open('rb') as f:
            while block := f.read(1024 * 1024): h.update(block)
        inventory = {'capture':p.name, 'sha256':h.hexdigest(), 'draws':total, 'named':named, 'retained_max_tick':40,
                     'sites':[{'kind':k[0], 'stream':k[1], 'stack':[sites.get(str(a), ['?', '?']) for a in k[2] if a], 'count':v} for k,v in counts.items()]}
        p.with_name(p.stem + '-inventory.json').write_text(json.dumps(inventory, indent=2))
        print(p.name, total, 'draws,', named, 'named; compacted', flush=True)
        p.unlink()
