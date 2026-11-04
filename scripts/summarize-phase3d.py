"""Compare Phase 3-C and Phase 3-D probe outputs without synthesizing samples.

Every numeric leaf (scalar or list of numbers) in each run's JSON is pooled per
metric path across runs, per build, and summarized with nearest-rank p50/p95.
"""
import json
import math
import pathlib
import re
import statistics
import sys

root = pathlib.Path(__file__).resolve().parents[1]
out = root / (sys.argv[1] if len(sys.argv) > 1 else 'docs/phase-3d/performance')
builds = {'baseline': 'studio-phase3c-release', 'final': 'studio-phase3d-release'}


def stats(values):
    v = sorted(values)
    if not v:
        return {'n': 0}
    q = lambda p: v[max(0, math.ceil(p * len(v)) - 1)]
    return dict(n=len(v), p50=q(.5), p95=q(.95), mean=statistics.mean(v), minimum=v[0], maximum=v[-1])


def leaves(value, path, sink):
    if isinstance(value, bool):
        return
    if isinstance(value, (int, float)):
        sink.setdefault(path, []).append(float(value))
    elif isinstance(value, list) and value and all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in value):
        sink.setdefault(path, []).extend(float(x) for x in value)
    elif isinstance(value, list):
        for i, item in enumerate(value):
            leaves(item, f'{path}[{i}]' if not isinstance(item, dict) or 'name' not in item else f"{path}[{item['name']}]", sink)
    elif isinstance(value, dict):
        for key, item in value.items():
            leaves(item, f'{path}.{key}' if path else key, sink)


summary = {'method': __doc__.strip(), 'builds': builds, 'modes': {}}
for mode_dir in sorted(p for p in out.iterdir() if p.is_dir() and (p / 'runs.json').exists()):
    mode = {}
    for label, build in builds.items():
        pooled = {}
        for f in sorted(mode_dir.glob(build + '-*.json')):
            binary = re.sub(r'-\d+$', '', f.stem[len(build) + 1:])
            leaves(json.loads(f.read_text(encoding='utf-8-sig')), binary, pooled)
        for key, values in pooled.items():
            mode.setdefault(key, {})[label] = stats(values)
    for key, row in mode.items():
        b, f = row.get('baseline', {}), row.get('final', {})
        if b.get('n') and f.get('n') and b['p50']:
            row['median_change_percent'] = round((f['p50'] - b['p50']) / abs(b['p50']) * 100, 2)
    summary['modes'][mode_dir.name] = mode
(out / 'performance-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')

pattern = re.compile(sys.argv[2] if len(sys.argv) > 2 else r'.')
for mode, rows in summary['modes'].items():
    for key, row in rows.items():
        if 'median_change_percent' in row and pattern.search(key):
            b, f = row['baseline'], row['final']
            print(f"{mode:10} {key[:88]:88} n={b['n']}/{f['n']} p50 {b['p50']:.3f} -> {f['p50']:.3f} ({row['median_change_percent']:+.1f}%)")
