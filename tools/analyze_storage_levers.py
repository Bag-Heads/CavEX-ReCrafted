"""Read general storage heartbeat fields; no --cw-tps-cpu capture required."""
import argparse
import json
from pathlib import Path

FIELDS = dict(op='overlay pages', oph='overlay boot high-water pages',
              opc='overlay capacity pages', opf='page-allocation failures',
              oer='emergency uses', orc='accepted recovery entries',
              db='resident backings', ds='shared backings',
              dln='resident finalized lists', dls='capacity slack bytes (includes pad)',
              dlp='trailing allocation pad bytes')


RECOVERY_FIELDS = dict(ocy='recovery request cycles', oex='deadline expiries',
                       owu='maximum completed recovery wait us')


class StorageEvidence:
    def __init__(self):
        self.beats = 0
        self.samples = 0
        self.ranges = {}
        self.recovery_samples = 0

    def note(self, event):
        if event.get('event') != 'heartbeat':
            return
        self.beats += 1
        if not all(type(event.get(k)) is int and 0 <= event[k] <= 0xffffffff
                   for k in FIELDS):
            return  # Old/malformed records are unavailable, never zero evidence.
        self.samples += 1
        keys = dict(FIELDS)
        if type(event.get('dlc')) is int and event['dlc'] in (0, 1):
            keys['dlc'] = 'display-list compaction enabled (0/1)'
        if all(type(event.get(k)) is int and 0 <= event[k] <= 0xffffffff
               for k in RECOVERY_FIELDS):
            self.recovery_samples += 1
            keys.update(RECOVERY_FIELDS)
        for key in keys:
            value = event[key]
            r = self.ranges.setdefault(key, dict(min=value, peak=value))
            r['min'] = min(r['min'], value)
            r['peak'] = max(r['peak'], value)

    def summary(self):
        complete = self.samples > 0 and self.samples == self.beats
        peak = self.ranges.get('oph', {}).get('peak')
        return dict(samples=self.samples, heartbeats=self.beats, complete=complete,
                    ranges=self.ranges, recovery_samples=self.recovery_samples,
                    overlay_256_margin_pages=256-peak if complete else None)


def render_storage(summary):
    if not summary or not summary.get('samples'):
        return '- Storage evidence unavailable (no complete storage heartbeat).\n'
    lines = [f"- Storage samples: {summary['samples']}/{summary['heartbeats']} (whole run, including loading)"]
    for key, label in FIELDS.items():
        r = summary['ranges'][key]
        lines.append(f"- {key} ({label}): min={r['min']} peak={r['peak']}")
    compact = summary['ranges'].get('dlc')
    lines.append(f"- dlc (compaction enabled): min={compact['min']} peak={compact['peak']}"
                 if compact else '- dlc (compaction enabled): unavailable')
    count = summary.get('recovery_samples', 0)
    lines.append(f"- Recovery telemetry: {count}/{summary['heartbeats']} heartbeats (missing is unavailable, not zero)")
    if count:
        for key, label in RECOVERY_FIELDS.items():
            r = summary['ranges'][key]
            lines.append(f"- {key} ({label}): min={r['min']} peak={r['peak']}")
    capacity = summary['ranges'].get('opc', {}).get('min', 256)
    high_water = summary['ranges'].get('oph', {}).get('peak')
    margin = capacity - high_water if high_water is not None else None
    lines.append(f'- Overlay {capacity}-page headroom against boot high-water: {margin if margin is not None else "unavailable (incomplete fields)"} pages')
    lines.append('- Sampled list slack includes pad; do not add dlp to dls. This run does not bound arbitrary edits or prove carve recovery.')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('telemetry', type=Path, help='JSONL file or run directory containing telemetry.jsonl')
    args = parser.parse_args()
    path = args.telemetry / 'telemetry.jsonl' if args.telemetry.is_dir() else args.telemetry
    evidence = StorageEvidence()
    runs = set()
    for line in path.read_text(encoding='utf-8').splitlines():
        event = json.loads(line)
        if event.get('event') == 'heartbeat':
            runs.add(event.get('run_id'))
        evidence.note(event)
    if len(runs) > 1:
        parser.error('Input contains more than one run_id; analyze each run separately')
    print(render_storage(evidence.summary()), end='')


if __name__ == '__main__':
    main()
