"""No-downgrade gate: full original-JAR snapshots versus live production.

The baseline is a one-time run of b11193e03 with this same external script.
Every array in each dimension/phase is independently gated. Per-chunk rows
remain in the baseline fixture for provenance and diagnosis; counts are cells
(unpacked nibbles), not differing packed bytes. This is not a parity claim.
"""
import argparse
import gzip
import hashlib
import json
import subprocess
from pathlib import Path

if not __debug__:
    raise RuntimeError('no-downgrade requires enabled assertions (no python -O)')


def no_downgrade(base, current):
    assert len(base) == len(current) == 6
    return all(b[:2] == n[:2] and all(0 <= v <= old for old, v in zip(b[2:], n[2:]))
               for b, n in zip(base, current))


def run(binary, fixture, record=None):
    pin = json.loads((fixture / 'reference.json').read_text())
    archive = fixture / 'original-jar.bin.gz'
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == pin['archive_sha256']
    # Authenticate every complete snapshot and the exact prescribed ordering.
    expected = [f'{phase}-{dim}-{x}-{z}.bin' for dim in (0, -1)
                for phase in ('load', 'pre-drain', 'completion')
                for x in range(-9, 14) for z in range(-9, 14)]
    assert [r['name'] for r in pin['snapshots']] == expected
    with gzip.open(archive, 'rb') as stream:
        for row in pin['snapshots']:
            raw = stream.read(82176)
            assert len(raw) == 82176 and hashlib.sha256(raw).hexdigest() == row['sha256']
        assert stream.read(1) == b''
    p = subprocess.run([str(binary.resolve()), str(archive.resolve())],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    rows = [list(map(int, s.split(',')[1:])) for s in p.stdout.splitlines()
            if s.startswith('COUNT,')]
    coordinates = [[dim, phase, x, z] for dim in (0, -1) for phase in range(3)
                   for x in range(-9, 14) for z in range(-9, 14)]
    assert len(rows) == 3174 and [r[:4] for r in rows] == coordinates
    assert all(0 <= r[4] <= 32768 and 0 <= r[5] <= 32768 and 0 <= r[6] <= 256 for r in rows)
    metrics = []
    for dim in (0, -1):
        for phase in range(3):
            totals = [sum(r[4 + i] for r in rows if r[:2] == [dim, phase])
                      for i in range(3)]
            metrics.append([dim, phase, *totals])
    reported = [list(map(int, s.split(',')[1:])) for s in p.stdout.splitlines()
                if s.startswith('METRIC,')]
    assert metrics == reported, 'incomplete/inconsistent metric output'
    current = dict(rows=rows, metrics=metrics, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                   harness_sha256=hashlib.sha256(Path(__file__).with_suffix('.c').read_bytes()).hexdigest(),
                   reference_sha256=pin['archive_sha256'])
    if record:
        record.write_text(json.dumps(current, indent=2) + '\n')
        print('Recorded baseline metrics:', metrics)
        return
    base = json.loads((fixture / 'baseline-b11193e03.json').read_text())
    assert base['commit'] == 'b11193e03' and base['reference_sha256'] == pin['archive_sha256']
    assert base['harness_sha256'] == current['harness_sha256'], 'baseline harness drift'
    assert [r[:4] for r in base['rows']] == coordinates
    assert base['metrics'] == [[d, p, *[sum(r[4+i] for r in base['rows'] if r[:2]==[d,p])
                                      for i in range(3)]] for d in (0,-1) for p in range(3)]
    for old, new in zip(base['metrics'], metrics):
        print(f"METRIC dimension={new[0]} phase={('load','pre-drain','completion')[new[1]]} "
              f"Sky/Block/height baseline={old[2:]} HEAD={new[2:]}")
    assert no_downgrade(base['metrics'], metrics), 'lighting downgrade against b11193e03'
    # Reject a worsening in EVERY independent axis, including zero-baseline
    # Nether Sky and load-time Block arrays. Exercise the actual grader.
    for axis in range(18):
        mutant = [r.copy() for r in base['metrics']]
        mutant[axis // 3][2 + axis % 3] += 1
        assert not no_downgrade(base['metrics'], mutant), f'downgrade axis {axis} survived'
    print('PASS all 18 single-array downgrade mutants rejected')
    print('PASS no downgrade: all 18 dimension/phase/array counts <= pinned baseline')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--fixture', type=Path, default=Path(__file__).parent / 'oracle/lighting/g9_option_b/fixtures/no-downgrade')
    parser.add_argument('--record', type=Path)
    args = parser.parse_args()
    run(args.binary, args.fixture, args.record)
