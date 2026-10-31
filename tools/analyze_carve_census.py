#!/usr/bin/env python3
"""Validate paired carve packets and tabulate PC residency / explicit Wii estimates.

No PC free-heap reading is treated as a carve: cap=0 is an unavailable gauge.
The spatial projection uses observed radius-5 columns/views, radius-6 GX mesh
capacities, and a separately flagged heightmap-span estimate for far voxels.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import statistics
from pathlib import Path

TENANTS = ('other', 'col', 'view', 'pool', 'private', 'vox', 'far', 'dl', 'pop', 'res', 'tex')
RESERVE = 2246 * 1024
CAP = 36 * 1024 * 1024

def median(values):
    return statistics.median(values)

def analyze(root: Path, duration: int):
    path = root / 'telemetry.ndjson'
    rows = []
    pending = {}
    beats = {}
    sealed = set()
    max_packet = 0
    total_frames = 0
    max_heartbeat = 0
    for raw in path.read_text(encoding='utf-8-sig').splitlines():
        e = json.loads(raw)
        kind = e.get('event')
        if 'anchor_frame_us' in e:
            assert not e['anchor_frame_dropped'], 'Dropped frame evidence'
            assert e['anchor_frame_total'] - total_frames == len(e['anchor_frame_us'])
            total_frames = e['anchor_frame_total']
        if kind == 'heartbeat':
            assert e['tick'] not in beats, 'Duplicate heartbeat'
            beats[e['tick']] = e
            max_heartbeat = max(max_heartbeat, len(raw.encode()))
        elif kind == 'carve_tenant':
            max_packet = max(max_packet, len(raw.encode()))
            assert e['t'] in TENANTS
            group = pending.setdefault(e['tick'], {})
            assert e['t'] not in group, 'Duplicate tenant'
            group[e['t']] = e
        elif kind == 'carve_census':
            assert e['tick'] not in sealed, 'Duplicate census seal'
            sealed.add(e['tick'])
            max_packet = max(max_packet, len(raw.encode()))
            b = beats[e['tick']]
            t = pending.pop(e['tick'])
            assert set(t) == set(TENANTS), 'Incomplete tenant group'
            assert all(x['m'] == e['m'] for x in t.values())
            assert bool(e['m']) == b['anchor_measuring']
            assert e['err'] == 0, 'Ledger error'
            assert sum(x['c'] for x in t.values()) + e['guard'] + e['over'] == e['used']
            assert e['used'] + e['free'] == e['cap']
            # Heartbeat and census are separate instants; workers can allocate
            # between them. Never require their cumulative spill totals equal.
            assert e['pages'] <= e['peak_pages'] <= 8192
            if not e['m']:
                continue
            pc = {name: t[name]['b'] for name in TENANTS}
            projected = {name: t[name]['w5'] for name in TENANTS}
            # The PC build has no synthetic far ring at the prescribed distance.
            # Keep this estimate independent of the measured far allocation row.
            projected['far_est'] = e['w6_far_slices_est'] * 10240
            # libogc rounds requested+256-guard to 32, then adds 40 bytes per
            # block (page alignment + used-block header). All column sizes are
            # 32-byte multiples; estimate round overhead for the remaining rows.
            n = e['w5_col'] * 6 + e['w6_dl_n'] + e['w6_far_slices_est']
            n += sum(t[name]['n'] for name in ('other', 'pool', 'private', 'pop', 'res', 'tex'))
            projected['guard_over_est'] = n * (256 + 40) + 8
            rows.append({'hb': b, 'census': e, 'tenants': t, 'pc': pc,
                         'wii_est': projected, 'free_est': CAP - sum(projected.values())})
    assert not pending, 'Unsealed census group'
    assert set(beats) == sealed, 'Heartbeat without census'
    # PC emits heartbeat/observer records to stdout, but profile_done uses
    # the transport sink. Completion is the harness's zero-exit check plus
    # the final heartbeat reaching the full measurement duration below.
    assert rows, 'No measured census'
    assert max_packet < 8192 and max_heartbeat < 8192
    completion = json.loads((root/'run_completion.json').read_text(encoding='utf-8-sig'))
    assert completion['completed'] and completion['exit_code'] == 0
    assert completion['duration_seconds'] >= duration
    # anchor_ms is sampled at frame START; profile completion checks wall time
    # later in that same frame. Preserve/report this tail instead of pretending
    # the frame-start counter is the completion clock. Reject >one heartbeat
    # of unexplained missing tail even if the process exited successfully.
    tail_ms = max(0, duration * 1000 - rows[-1]['hb']['anchor_ms'])
    assert tail_ms < 1000, 'Incomplete measured heartbeat window'
    assert all(r['hb']['anchor_run_faults'][0] == 0 for r in rows), 'OOM'
    peak_pc = max(rows, key=lambda r: sum(r['pc'].values()))
    minimum = min(rows, key=lambda r: r['free_est'])
    # One entry for every contiguous projected reserve crossing. Real Wii dip
    # attribution requires actual carve data and is intentionally not invented.
    dips = []
    prior = rows[0]
    start = None
    for r in rows:
        if r['free_est'] < RESERVE:
            if start is None:
                changes = {k: r['wii_est'][k] - prior['wii_est'][k] for k in r['wii_est']}
                start = {'start_ms': r['hb']['anchor_ms'], 'end_ms': r['hb']['anchor_ms'],
                         'heartbeats': 0, 'min_free_kib': r['free_est']/1024,
                         'growth': sorted(changes.items(), key=lambda x:x[1], reverse=True),
                         'entered_already_low': r is rows[0]}
                dips.append(start)
            start['heartbeats'] += 1
            start['end_ms'] = r['hb']['anchor_ms']
            start['min_free_kib'] = min(start['min_free_kib'], r['free_est']/1024)
        else:
            start = None
        prior = r
    first, last = rows[0], rows[-1]
    seconds = (last['hb']['anchor_ms'] - first['hb']['anchor_ms']) / 1000
    pc_mid = {k: median([r['pc'][k] for r in rows])/1024 for k in TENANTS}
    scaled_mid = {k: median([r['wii_est'][k] for r in rows])/1024 for k in minimum['wii_est']}
    def moment(r):
        return {'ms': r['hb']['anchor_ms'], 'free_est_kib': r['free_est']/1024,
                'pc_kib': {k:v/1024 for k,v in r['pc'].items()},
                'wii_est_kib': {k:v/1024 for k,v in r['wii_est'].items()},
                'census': r['census']}
    result = {
        'root': str(root), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'seed': json.loads((root/'run_provenance.json').read_text(encoding='utf-8-sig'))['seed'] if (root/'run_provenance.json').exists() else None,
        'sep': rows[0]['hb']['anchor_sep'], 'measured_hb': len(rows),
        'measured_ms': last['hb']['anchor_ms'], 'frames': total_frames,
        'terminal_frame_tail_ms': tail_ms,
        'max_packet': max_packet, 'max_heartbeat': max_heartbeat,
        'pc_median_kib': pc_mid, 'wii_est_median_kib': scaled_mid,
        'max_pc_moment': moment(peak_pc), 'minimum_est_moment': moment(minimum),
        'free_est_median_kib': median([r['free_est'] for r in rows])/1024,
        'below_est_reserve': sum(r['free_est'] < RESERVE for r in rows),
        'actual_carve_available': bool(last['census']['cap']),
        'free_est_semantics': 'signed 36-MiB budget minus partial projected allocation demand; NOT allocator free bytes (fallback placement is unknown)',
        'projection_scope': 'partial: published radius-5 backings/views and radius-6 GX-sized PC geometry; excludes unpublished backings, GPU-only departed lists and Wii-only textures; synthetic far geometry is not reproduced',
        'dl_alloc_s': (last['tenants']['dl']['a'] - first['tenants']['dl']['a']) / seconds,
        'dl_grow_s': (last['census']['grow'] - first['census']['grow']) / seconds,
        'dl_moved_s': (last['census']['move'] - first['census']['move']) / seconds,
        'dl_copy_kib_s': (last['census']['copy'] - first['census']['copy']) / seconds / 1024,
        'dl_slack_median_kib': median([r['census']['dl_slack'] for r in rows])/1024,
        'w6_slack_median_kib': median([r['census']['w6_slack'] for r in rows])/1024,
        'peak_pages': max(r['census']['peak_pages'] for r in rows),
        'observer_peak_kib': max(r['census']['obs'] for r in rows)/1024,
        'dep_gx_peak_kib': max(r['census']['dep_gx_peak'] for r in rows)/1024,
        'dep_pc_peak_kib': max(r['census']['dep_peak'] for r in rows)/1024,
        'far_air_peak': max(r['census']['far_air'] for r in rows),
        'dips_est': dips,
    }
    return result

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('roots', type=Path, nargs='+')
    ap.add_argument('--duration', type=int, default=360)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    result = [analyze(root, args.duration) for root in args.roots]
    args.out.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    for r in result:
        print(r['root'], 'hb',r['measured_hb'], 'min nominal budget balance KiB',round(r['minimum_est_moment']['free_est_kib'],1), 'pages',r['peak_pages'])
if __name__ == '__main__':
    main()
