"""Phase 3-C vs 3-D process probe: startup, CPU and memory per scenario (warm runs only)."""
import json
import math
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1]
rows = json.loads((root / sys.argv[1] / 'process/rows.json').read_text(encoding='utf-8'))
builds = ['studio-phase3c-release', sys.argv[2] if len(sys.argv) > 2 else 'studio-phase3d-release']


def p50(values):
    v = sorted(values)
    return v[max(0, math.ceil(.5 * len(v)) - 1)] if v else float('nan')


result = {}
for scenario in sorted({r['scenario'] for r in rows}):
    for metric, get in [('first_ui_ms', lambda r: r['telemetry']['samples_ms']['first_ui_ms'][0]),
                        ('ui_construction_ms', lambda r: r['telemetry']['samples_ms']['ui_construction'][0]),
                        ('settled_cpu_one_core_percent', lambda r: r['cpu_one_core_percent']),
                        ('working_set_mib', lambda r: r['working_set_mib']),
                        ('private_mib', lambda r: r['private_mib']),
                        ('handles', lambda r: r['handles'])]:
        row = {}
        for build in builds:
            # Run 0 is the first (cold) observation; warm comparisons use the rest.
            values = [get(r) for r in rows if r['build'] == build and r['scenario'] == scenario and r['run'] > 0]
            row[build] = dict(n=len(values), p50=p50(values))
        b, f = row[builds[0]]['p50'], row[builds[1]]['p50']
        row['median_change_percent'] = round((f - b) / b * 100, 2) if b else None
        result[f'{scenario}.{metric}'] = row
        print(f"{scenario:6} {metric:30} n={row[builds[0]]['n']}/{row[builds[1]]['n']} {b:9.2f} -> {f:9.2f}  ({row['median_change_percent']}%)")
(root / sys.argv[1] / 'process-summary.json').write_text(json.dumps(result, indent=2) + '\n')
