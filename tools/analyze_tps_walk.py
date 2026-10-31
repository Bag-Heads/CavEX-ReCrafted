#!/usr/bin/env python3
"""Analyze exact ticks or bounded TPS summaries (standard library only).

No Wii/PC scaling, percentile reconstruction from averages, or fabricated CPU
time. clock_mode=3 is active wall time, still including involuntary preemption.
Nested caller times overlap phases and must not be added to the phase total.
"""
import argparse
import json
import math
from pathlib import Path

PHASES = 'prep sim save evict scan gen slot copy send far tail'.split()
NESTED = ('scheduler load terrain initial_light decorate vertical_quad '
          'block_quad sky_quad vertical_final sky_final save_object rearm '
          'region_read region_parse region_get region_compress region_commit '
          'region_sync channel_cpu mutex_cpu mem2_cpu').split()

CENSUS = ('save_calls save_writes save_unchanged save_unknown save_compress_ns '
          'save_commit_ns save_compare_ns block_count_ns block_fill_ns block_pairs '
          'block_duplicates block_count_cells block_fill_cells sky_count_ns sky_fill_ns '
          'sky_pairs sky_duplicates sky_count_cells sky_fill_cells vertical_calls '
          'vertical_unchanged block_calls block_unchanged sky_calls sky_unchanged '
          'light_lookup_misses unchanged_compress_ns unchanged_commit_ns compare_bytes mutations '
          'block_memo_fallbacks sky_memo_fallbacks '
          'block_consumed_cells block_consumed_sources '
          'sky_consumed_cells sky_consumed_sources').split()


def census(events, records):
    summaries = [e for e in events if e.get('event') == 'tps_summary']
    if not summaries or all('census_version' not in r and 'census' not in r for r in summaries):
        return None
    total = [0] * len(CENSUS)
    covered = 0
    consumed_available = True
    for row in summaries:
        values = row.get('census', [])
        count = row.get('count', row.get('records'))
        version = row.get('census_version')
        expected = {1: 30, 2: 32, 3: len(CENSUS)}.get(version)
        consumed_available &= version == 3
        if (version not in (1, 2, 3)
                or ('seq' not in row and row.get('first_record') != covered)
                or type(count) is not int or count <= 0
                or len(values) != expected
                or any(type(v) is not int or v < 0 for v in values)):
            raise ValueError('Invalid, missing or overlapping census aggregate')
        values = values + [0] * (len(CENSUS) - len(values))
        covered += count
        total = [a + b for a, b in zip(total, values)]
    if covered != records:
        raise ValueError('Incomplete census coverage')
    totals = dict(zip(CENSUS, total))
    if totals['save_unchanged'] + totals['save_unknown'] > totals['save_writes']:
        raise ValueError('Impossible save census counts')
    for name in ('block', 'sky'):
        if totals[name + '_duplicates'] > totals[name + '_pairs']:
            raise ValueError('Impossible scan census counts')
    for name in ('vertical', 'block', 'sky'):
        if totals[name + '_unchanged'] > totals[name + '_calls']:
            raise ValueError('Impossible light census counts')
    return dict(totals=totals, consumed_available=consumed_available, per_update={
        k: v / records / (1e6 if k.endswith('_ns') else 1)
        for k, v in totals.items()},
        unchanged_save_share=(totals['save_unchanged'] / totals['save_writes']
                              if totals['save_writes'] else None),
        unknown_save_share=(totals['save_unknown'] / totals['save_writes']
                            if totals['save_writes'] else None))


def census_markdown(result):
    if result is None:
        return ['', 'Census: unavailable in this capture (not zero).']
    v, t = result['per_update'], result['totals']
    share = lambda a, b: f'{100 * a / b:.3f}%' if b else 'n/a'
    lines = ['', 'Save and admission-light census:', '',
             f"Save calls/update: {v['save_calls']:.6f}; committed writes/update: {v['save_writes']:.6f}.",
             f"Unchanged committed inputs: {share(t['save_unchanged'], t['save_writes'])}; "
             f"unverified/cold: {share(t['save_unknown'], t['save_writes'])}.",
             f"Serialization+compress / commit / comparison: {v['save_compress_ns']:.6f} / "
             f"{v['save_commit_ns']:.6f} / {v['save_compare_ns']:.6f} ms/update.", '',
             f"Of that, proven unchanged-write compress / commit: {v['unchanged_compress_ns']:.6f} / "
             f"{v['unchanged_commit_ns']:.6f} ms/update. No commit skipping is implied.", '',
             '| Scan | Count ms/update | Fill ms/update | Pairs/update | Repeated predicate source sets | Count/fill predicate evaluations per update | Fill consumed cells/sources per update |',
             '|---|---:|---:|---:|---:|---:|---:|']
    for name in ('block', 'sky'):
        consumed = (f"{v[name+'_consumed_cells']:.3f} / {v[name+'_consumed_sources']:.3f}"
                    if result['consumed_available'] else 'unavailable')
        lines.append(f"| {name} | {v[name+'_count_ns']:.6f} | {v[name+'_fill_ns']:.6f} | "
                     f"{v[name+'_pairs']:.6f} | {share(t[name+'_duplicates'], t[name+'_pairs'])} | "
                     f"{v[name+'_count_cells']:.3f} / {v[name+'_fill_cells']:.3f} | {consumed} |")
    lines += ['', f"Memo fallback calls: block={t['block_memo_fallbacks']}, sky={t['sky_memo_fallbacks']}.",
              'Fill cells count predicate evaluations; memo reads evaluate none. '
              'Repeated predicate source sets counts recomputed matching source sets; zero does not mean no cells consumed.', '', 'Whole-pass inputs: conservative exact certificates; unchanged shares are lower bounds. '
              'Unrelated mutations, new updates and table misses invalidate certificates. '
              'A non-match does not prove changed bytes.', '',
              '| Pass | Participating columns/update | Proven unchanged |', '|---|---:|---:|']
    for name in ('vertical', 'block', 'sky'):
        lines.append(f"| {name} | {v[name+'_calls']:.6f} | "
                     f"{share(t[name+'_unchanged'], t[name+'_calls'])} |")
    lines += [f"Certificate lookup misses: {t['light_lookup_misses']}; observer comparison and "
              'bitmap work is included in measured update time, not a demonstrated optimization.']
    return lines


def percentile(values, percent):
    """Nearest rank, including the exact maximum."""
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * percent / 100) - 1)]


def delta(a, b):
    return (b - a) & 0xffffffff


def analyze(events, allow_partial=False):
    beats = [e for e in events if e.get('event') == 'heartbeat']
    if any(e.get('event') == 'tps_summary' and 'seq' in e for e in events):
        return analyze_summaries(events, allow_partial=allow_partial)
    rows = [e for e in events if e.get('event') == 'tps_tick']
    ends = [e for e in events if e.get('event') == 'tps_end']
    if not rows:
        raise ValueError('No exact tps_tick records. Historical heartbeat '
                         'averages cannot recover percentiles or worst ticks; '
                         'capture with the Slice 1b observer and --cw-tps-cpu.')
    if len(ends) != 1 or ends[0].get('records') != len(rows) or not ends[0].get('stream_valid'):
        raise ValueError('Missing/invalid tps_end or incomplete tick stream')
    if any(b.get('cpu_ticks', 0) and not b.get('cpu_valid', 0) for b in beats):
        raise ValueError('Observer reported invalid evidence')
    updates = [r['update'] for r in rows]
    if updates != list(range(updates[0], updates[0] + len(rows))):
        raise ValueError('Lost, reordered or duplicate tps_tick records')
    clocks = {r['clock'] for r in rows}
    if len(clocks) != 1:
        raise ValueError('Clock changed inside capture')
    if not clocks <= {'thread_cycles_tsc_qpc_estimate', 'thread_cpu',
                      'activity_masked_wall_estimate'}:
        raise ValueError('Unknown or synthetic clock; not live timing evidence')
    for r in rows:
        if type(r['step_ns']) is not int or type(r['step_ns']) is not int or not 0 < r['step_ns'] <= 100000:
            raise ValueError('Clock resolution did not qualify')
        if len(r['phase_ns']) != len(PHASES) or len(r['nested_ns']) != len(NESTED):
            raise ValueError('Unknown observer schema')
        if any(type(v) is not int or v < 0 for v in
               [r['cpu_ns'], r['wall_ns']] + r['phase_ns'] + r['nested_ns']):
            raise ValueError('Invalid duration')
        if sum(r['phase_ns']) > r['cpu_ns']:
            raise ValueError('Phase accounting exceeds whole tick')
    n = len(rows)
    avg = lambda key, names: {name: sum(r[key][i] for r in rows) / n / 1e6
                             for i, name in enumerate(names)}
    phase = avg('phase_ns', PHASES)
    nested = avg('nested_ns', NESTED)
    measured = [b for b in beats if b.get('anchor_measuring')]
    duration = max((b.get('anchor_ms', 0) for b in measured), default=0) / 1000
    if not duration:
        raise ValueError('No measured-window heartbeat')
    # tps_end is the observer's explicit seal. The PC telemetry backend does
    # not print the ordinary profile_done packet; do not invent one here.
    # Main-thread installation samples use complete adjacent heartbeat windows;
    # exclude the first crossing interval, just as the exact server ticks do.
    main = {}
    if len(measured) >= 2:
        tick_count = sum(delta(a['cpu_ticks'], b['cpu_ticks'])
                         for a, b in zip(measured, measured[1:]))
        if tick_count:
            for key in ('cpu_main_us', 'cpu_mesher_us', 'cpu_packed_us'):
                if next(iter(clocks)) == 'activity_masked_wall_estimate' and key != 'cpu_packed_us':
                    continue  # No native whole-thread accounting on this backend.
                if all(key in b for b in measured):
                    main[key] = sum(delta(a[key], b[key])
                                    for a, b in zip(measured, measured[1:])) / tick_count / 1000
    walls = [r['wall_ns'] / 1e6 for r in rows]
    worst = []
    for r in sorted(rows, key=lambda r: r['wall_ns'], reverse=True)[:20]:
        winner = max(range(len(PHASES)), key=lambda i: r['phase_ns'][i])
        worst.append(dict(update=r['update'], world_tick=r['world_tick'],
                          wall_ms=r['wall_ns'] / 1e6, cpu_ms=r['cpu_ns'] / 1e6,
                          wall_minus_cpu_ms=(r['wall_ns'] - r['cpu_ns']) / 1e6,
                          dominant=PHASES[winner],
                          dominant_ms=r['phase_ns'][winner] / 1e6,
                          anchor=r['anchor']))
    candidates = {
        'relighting': sum(nested[k] for k in ('vertical_quad', 'block_quad',
                                             'sky_quad', 'vertical_final', 'sky_final')),
        'save_serialization_reuse': nested['region_compress'],
        'residency_scheduler_cache': nested['scheduler'],
    }
    return dict(clock=next(iter(clocks)), observed_step_ns=min(r['step_ns'] for r in rows),
                records=n, census=census(events, n),
                measured_seconds=duration, achieved_tps=n / duration,
                phase_ms_per_tick=phase, nested_ms_per_tick=nested,
                server_cpu_ms_per_tick=sum(r['cpu_ns'] for r in rows) / n / 1e6,
                main_ms_per_tick=main,
                wall_ms={str(p): percentile(walls, p) for p in (50, 95, 100)},
                over_50_ms=sum(v > 50 for v in walls), worst_20=worst,
                candidate_upper_bounds_ms=candidates,
                proven_removable_ms={k: 0 for k in candidates})


def analyze_summaries(events, allow_partial=False):
    """Exact aggregate totals; percentiles and individual worst ticks unavailable."""
    rows = [e for e in events if e.get('event') == 'tps_summary']
    beats = [e for e in events if e.get('event') == 'heartbeat']
    ends = [e for e in events if e.get('event') == 'tps_end']
    if any(e.get('event') == 'tps_tick' for e in events):
        raise ValueError('Mixed exact and aggregate streams')
    previous = None
    for seq, r in enumerate(rows, 1):
        if (type(r['seq']) is not int or r['seq'] != seq
                or type(r['count']) is not int or r['count'] <= 0
                or type(r['first']) is not int or type(r['last']) is not int
                or r['last'] - r['first'] + 1 != r['count']
                or (previous is not None and r['first'] != previous + 1)):
            raise ValueError('Lost, duplicate or reordered summary')
        previous = r['last']
        if (len(r['phase_ns']) != len(PHASES) or len(r['nested_ns']) != len(NESTED)
                or any(type(v) is not int or v < 0 for v in
                       [r['cpu_ns'], r['wall_ns'], r['max_ns'], r['over50']]
                       + r['phase_ns'] + r['nested_ns'])
                or sum(r['phase_ns']) > r['cpu_ns']
                or r['max_ns'] > r['wall_ns'] or r['over50'] > r['count']):
            raise ValueError('Invalid aggregate accounting')
        if r['clock'] not in {'thread_cycles_tsc_qpc_estimate', 'thread_cpu',
                              'activity_masked_wall_estimate'} or type(r['step_ns']) is not int or not 0 < r['step_ns'] <= 100000:
            raise ValueError('Unqualified clock')
    if len({r['clock'] for r in rows}) != 1:
        raise ValueError('Clock changed inside capture')
    n = sum(r['count'] for r in rows)
    unsealed = not ends and allow_partial
    if not unsealed and (len(ends) != 1 or type(ends[0].get('records')) is not int
                         or ends[0]['records'] != n
                         or type(ends[0].get('stream_valid')) is not bool):
        raise ValueError('Missing/invalid tps_end or incomplete aggregate stream')
    partial = unsealed or not ends[0]['stream_valid']
    reason = 'missing_tps_end' if unsealed else ends[0].get('reason', '')
    if partial and (not isinstance(reason, str) or not reason.strip()):
        raise ValueError('Partial tps_end must give a reason')
    end = len(events) if unsealed else events.index(ends[0])
    if any(e.get('event') == 'tps_summary' for e in events[end:]):
        raise ValueError('Reordered summary after tps_end')
    if any(b.get('cpu_ticks', 0) and not b.get('cpu_valid') for b in beats):
        raise ValueError('Observer reported invalid evidence')
    windows = [e for e in events if e.get('event') == 'tps_window']
    if windows:
        if len(windows) != 1 or windows[0].get('kind') != 'single_anchor':
            raise ValueError('Invalid single-anchor measurement window')
        start = events.index(windows[0])
        if start >= end or any(e.get('event') == 'tps_summary' for e in events[:start]):
            raise ValueError('Reordered single-anchor measurement window')
        measured = [b for b in events[start + 1:end] if b.get('event') == 'heartbeat']
        if unsealed:
            # Historical packets lack timestamps. Bracket the observed window
            # with the last preceding heartbeat and the final following one.
            # tick counts nominal one-second heartbeats, NOT measured wall time;
            # the result is explicitly an estimate and cannot certify the tail.
            before = [b for b in events[:start] if b.get('event') == 'heartbeat']
            if (not before or not measured
                    or events.index(rows[-1]) > events.index(measured[-1])):
                raise ValueError('Unsealed window needs bracketing heartbeats')
            bracket = before[-1:] + measured
            if (any(type(b.get('tick')) is not int for b in bracket)
                    or any(b['tick'] != a['tick'] + 1 for a, b in zip(bracket, bracket[1:]))):
                raise ValueError('Lost or reordered bracketing heartbeat')
            duration = bracket[-1]['tick'] - bracket[0]['tick']
        else:
            if type(ends[0].get('measured_ns')) is not int or ends[0]['measured_ns'] <= 0:
                raise ValueError('Invalid single-anchor measurement window')
            duration = ends[0]['measured_ns'] / 1e9
    else:
        if unsealed:
            raise ValueError('Unsealed partial mode requires a single-anchor window')
        measured = [b for b in beats if b.get('anchor_measuring')]
        duration = max((b.get('anchor_ms', 0) for b in measured), default=0) / 1000
    if not duration:
        raise ValueError('No measured-window heartbeat')
    avg = lambda key, names: {name: sum(r[key][i] for r in rows) / n / 1e6
                             for i, name in enumerate(names)}
    nested = avg('nested_ns', NESTED)
    candidates = dict(relighting=sum(nested[k] for k in ('vertical_quad', 'block_quad', 'sky_quad', 'vertical_final', 'sky_final')),
                      save_serialization_reuse=nested['region_compress'],
                      residency_scheduler_cache=nested['scheduler'])
    main = {}
    intervals = list(zip(measured, measured[1:]))
    count = sum(delta(a['cpu_ticks'], b['cpu_ticks']) for a, b in intervals)
    if count:
        for key in ('cpu_main_us', 'cpu_mesher_us', 'cpu_packed_us'):
            if rows[0]['clock'] == 'activity_masked_wall_estimate' and key != 'cpu_packed_us':
                continue
            if all(key in b for b in measured):
                main[key] = sum(delta(a[key], b[key]) for a, b in intervals) / count / 1000
    return dict(clock=rows[0]['clock'], observed_step_ns=min(r['step_ns'] for r in rows),
                records=n, census=census(events, n), measured_seconds=duration, achieved_tps=n / duration,
                phase_ms_per_tick=avg('phase_ns', PHASES), nested_ms_per_tick=nested,
                server_cpu_ms_per_tick=sum(r['cpu_ns'] for r in rows) / n / 1e6,
                main_ms_per_tick=main, wall_ms={'50': None, '95': None,
                    '100': max(r['max_ns'] for r in rows) / 1e6},
                over_50_ms=sum(r['over50'] for r in rows), worst_20=[],
                candidate_upper_bounds_ms=candidates,
                proven_removable_ms={k: 0 for k in candidates}, aggregated=True,
                single_anchor=bool(windows), partial=partial, sealed=not unsealed,
                partial_reason=reason if partial else None,
                duration_estimated=unsealed)


def markdown(result, title):
    r = result
    heading = f'UNSEALED/PARTIAL: {title}' if r.get('partial') else title
    duration_label = 'estimated heartbeat-span' if r.get('duration_estimated') else 'measured'
    lines = [f'# {heading}', '']
    if r.get('partial'):
        lines += [f"Reason: `{r['partial_reason']}`. " +
                  ('No tps_end: completeness of the final tail is unknown; '
                   'duration uses nominal one-second heartbeats bracketing the window, '
                   'so updates/s is approximate. Received aggregate totals are exact.'
                   if not r['sealed'] else
                   'Explicit partial seal; received aggregate coverage matches the seal.'), '']
    lines += [
             f"Clock: `{r['clock']}`; minimum observed increment {r['observed_step_ns']} ns.",
             f"{r['records']} complete updates / {r['measured_seconds']:.3f} {duration_label} seconds "
             f"= {r['achieved_tps']:.3f} updates/s.", '',
             '| Exclusive server bucket | ms/tick |', '|---|---:|']
    lines += [f'| {k} | {v:.6f} |' for k, v in r['phase_ms_per_tick'].items()]
    lines += [f"| whole update (includes probe gaps) | {r['server_cpu_ms_per_tick']:.6f} |", '',
              '| Inclusive caller (do not sum with phases) | ms/tick |', '|---|---:|']
    lines += [f'| {k} | {v:.6f} |' for k, v in r['nested_ms_per_tick'].items()]
    lines += ['', 'Main/mesher/packed installs: complete heartbeat intervals, normalized by server updates.', '']
    lines += [f'- {k}: {v:.6f} ms/tick' for k, v in r['main_ms_per_tick'].items()]
    if r.get('aggregated'):
        lines += ['', f"Tick wall max: {r['wall_ms']['100']:.3f} ms; ticks >50 ms: {r['over_50_ms']}.",
                  'Per-tick percentiles and worst-tick attribution are unavailable in the bounded aggregate stream.']
        lines += census_markdown(r['census'])
        return '\n'.join(lines) + '\n'
    w = r['wall_ms']
    lines += ['', f"Tick wall p50/p95/max: {w['50']:.3f} / {w['95']:.3f} / {w['100']:.3f} ms; "
              f"ticks >50 ms: {r['over_50_ms']}.", '',
              'Dominance below is CPU/active-time dominance; the wall-minus-CPU residual is '
              'unattributed delay, not a preemption count or proof that this phase caused the stall.', '',
              '| Update | World tick | Wall ms | CPU/active ms | Wall minus CPU ms | Dominant CPU phase | Phase ms |',
              '|---:|---:|---:|---:|---:|---|---:|']
    lines += [f"| {t['update']} | {t['world_tick']} | {t['wall_ms']:.3f} | {t['cpu_ms']:.3f} | "
              f"{t['wall_minus_cpu_ms']:.3f} | {t['dominant']} | {t['dominant_ms']:.3f} |" for t in r['worst_20']]
    lines += ['', 'Candidate ceilings (measured caller cost, **not demonstrated savings**):', '']
    lines += [f'- {k}: 0 to {v:.6f} ms/tick; removable fraction unproven.'
              for k, v in r['candidate_upper_bounds_ms'].items()]
    lines += census_markdown(r['census'])
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('telemetry', type=Path)
    p.add_argument('--out', type=Path, help='Markdown output; also writes adjacent .json')
    p.add_argument('--allow-partial', action='store_true',
                   help='Report an unsealed single-anchor aggregate capture using its '
                        'nominal bracketing heartbeat span; sequence checks still apply')
    p.add_argument('--min-seconds', type=float,
                   help='Reject short windows (default: 360 for split-anchor/legacy; '
                        'positive sealed duration for single-anchor profiles)')
    args = p.parse_args()
    try:
        with args.telemetry.open(encoding='utf-8-sig') as f:
            events = [json.loads(line) for line in f if line.strip()]
        result = analyze(events, allow_partial=args.allow_partial)
        minimum = args.min_seconds
        if minimum is None:
            minimum = 0 if result.get('single_anchor') or result.get('partial') else 360
        if result['measured_seconds'] < minimum:
            raise ValueError(f"Only {result['measured_seconds']} measured seconds; "
                             f"need {minimum}")
    except (ValueError, KeyError, OSError) as exc:
        p.exit(2, f'Invalid evidence: {exc}\n')
    report = markdown(result, str(args.telemetry))
    if args.out:
        args.out.write_text(report, encoding='utf-8', newline='\n')
        args.out.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n',
                                               encoding='utf-8', newline='\n')
    else:
        print(report, end='')


if __name__ == '__main__':
    main()
