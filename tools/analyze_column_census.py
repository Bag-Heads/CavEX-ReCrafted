#!/usr/bin/env python3
"""Aggregate every measured column/tick row; never sample a census stream."""
import argparse
import gzip
import json
import statistics
from pathlib import Path


def median_hist(hist):
    n = sum(hist)
    ranks = ((n - 1) // 2, n // 2)
    values = []
    for rank in ranks:
        count = 0
        for value, frequency in enumerate(hist):
            count += frequency
            if count > rank:
                values.append(value)
                break
    return sum(values) / 2 if values else None


def analyze(path):
    hist = [[0] * 129 for _ in range(4)]
    combined = [0] * 513
    ticks = []
    current = None
    rows = zero = height = 0
    with gzip.open(path, 'rt', encoding='ascii') as stream:
        for line in stream:
            parts = line.rstrip('\n').split(',')
            if parts[0] == 'T':
                if current is not None:
                    ticks.append(current)
                values = list(map(int, parts[1:]))
                assert len(values) in (12, 13, 15), (path, parts)
                seq, tick, client_seq, measuring, server, full, slices, slots, slot_peak, leases, orphan_slices, resets = values[:12]
                current = dict(seq=seq, tick=tick, client_seq=client_seq,
                               measuring=measuring, server=server, client_full=full,
                               slices=slices, slots=slots, slot_peak=slot_peak,
                               leases=leases, orphan_slices=orphan_slices,
                               resets=resets, lease_peak=values[12] if len(values) >= 13 else leases,
                               occupied_peak=values[13]/8 if len(values) == 15 else server+slices/8+slots,
                               allocated_peak=values[14]/8 if len(values) == 15 else server+slices/8+16,
                               columns=0, pages=[0]*4)
                continue
            if parts[0] == 'V':
                assert len(parts) == 7 and current is not None
                seq, live, pages, peak, fallback_live, fallback_total = map(int, parts[1:])
                assert seq == current['seq']
                current['view_stats'] = dict(live=live, pages=pages, peak=peak,
                                           fallback_live=fallback_live, fallback_total=fallback_total)
                continue
            assert parts[0] == 'C' and len(parts) == 8 and current is not None
            if not current['measuring']:
                continue
            d = list(map(int, parts[3:7]))
            hm = int(parts[7])
            for p, count in enumerate(d):
                assert 0 <= count <= 128
                hist[p][count] += 1
                current['pages'][p] += count
            combined[sum(d)] += 1
            rows += 1
            zero += sum(d) == 0 and hm == 0
            height += hm
            current['columns'] += 1
    if current is not None:
        ticks.append(current)
    measured = [t for t in ticks if t['measuring'] and t['columns']]
    assert rows and measured, f'no measured census: {path}'
    per_column = [sum(t['pages']) / t['columns'] for t in measured]
    plane_names = ['ids', 'meta', 'sky', 'torch']
    index_bytes = 512 * 4 + 64  # Wii 32-bit page entries + proposed view header
    result = dict(path=str(path), ticks=len(ticks), measured_ticks=len(measured),
                  column_ticks=rows, zero_divergence_share=zero/rows,
                  height_divergent_column_ticks=height,
                  plane_pages={name: dict(median=median_hist(h),
                                          peak=max(i for i, n in enumerate(h) if n),
                                          mean=sum(i*n for i, n in enumerate(h))/rows)
                               for name, h in zip(plane_names, hist)},
                  combined_pages_median=median_hist(combined),
                  combined_pages_peak=max(i for i, n in enumerate(combined) if n),
                  mean_pages_per_column=sum(i*n for i, n in enumerate(combined))/rows,
                  median_tick_mean_pages=statistics.median(per_column),
                  peak_tick_mean_pages=max(per_column),
                  peak_simultaneous_occupied_copies=max(t['occupied_peak'] for t in ticks),
                  peak_simultaneous_allocated_copies=max(t['allocated_peak'] for t in ticks),
                  peak_server_columns=max(t['server'] for t in ticks),
                  peak_client_full_columns=max(t['client_full'] for t in ticks),
                  peak_inflight_slots=max(t['slot_peak'] for t in ticks),
                  peak_retained_generations=max(t['lease_peak'] for t in ticks),
                  peak_orphan_slices=max(t['orphan_slices'] for t in ticks),
                  index_bytes=index_bytes)
    view_stats = [t['view_stats'] for t in ticks if 'view_stats' in t]
    if view_stats:
        result['overlay'] = {key: max(t[key] for t in view_stats)
                             for key in view_stats[0]}
    for label, pages in [('mean', result['mean_pages_per_column']),
                         ('peak_tick_mean', result['peak_tick_mean_pages']),
                         ('worst_column', result['combined_pages_peak'])]:
        result[f'saving_{label}_bytes'] = 81920 - (pages*256 + index_bytes)
    # Heartbeats establish the six-minute wall-clock window and actual deaths.
    log = path.parent / 'stdout.log'
    if log.exists():
        beats = []
        encoding = 'utf-16' if log.read_bytes()[:2] in (b'\xff\xfe', b'\xfe\xff') else 'utf-8-sig'
        with log.open(encoding=encoding, errors='strict') as stream:
            for line in stream:
                if line.startswith('FORCED_RESPAWN '):
                    result['forced_respawns'] = result.get('forced_respawns', 0) + 1
                if line.startswith('{"event":'):
                    event = json.loads(line)
                    if event.get('event') == 'heartbeat':
                        beats.append(event)
        if beats:
            result['window_ms'] = max(b.get('anchor_ms', 0) for b in beats)
            result['deaths'] = max(b.get('deaths', 0) for b in beats)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', nargs='+', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = [analyze(p / 'column_census.gz' if p.is_dir() else p) for p in args.runs]
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    for r in result:
        print(r['path'], 'zero=', round(r['zero_divergence_share']*100, 2),
              'D=', round(r['mean_pages_per_column'], 3),
              'peak mean D=', round(r['peak_tick_mean_pages'], 3),
              'saved KiB=', round(r['saving_peak_tick_mean_bytes']/1024, 3))


if __name__ == '__main__':
    main()
