"""Summarize retained Phase 3-C observations without synthesizing samples."""
import argparse
import json
import math
import pathlib
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', nargs='?', default='docs/phase-3c')
args = parser.parse_args()
out = pathlib.Path(__file__).resolve().parents[1] / args.output
builds = {'baseline': 'studio-phase3c-baseline-release', 'final': 'studio-phase3c-release'}

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def stats(values):
    v = sorted(values)
    if not v: return {'n': 0}
    def q(p): return v[max(0, math.ceil(p * len(v)) - 1)]
    return dict(n=len(v), p50=q(.5), p95=q(.95), p99=q(.99), mean=statistics.mean(v),
                variance=statistics.pvariance(v), minimum=v[0], maximum=v[-1])

def files(mode, build, binary):
    return sorted((out / ('final-' + mode)).glob(build + '-' + binary + '-*.json'))

summary = {'quantiles': 'Nearest rank. Units follow each key; variance has squared units. Small-sample tails are maxima, not reliable population estimates.'}
for label, build in builds.items():
    result = {}
    result['index_ms'] = {}
    rows = [read(f) for f in files('index', build, 'nova-editor-bench')]
    for key in ('scan', 'refresh_file', 'refresh_assets_subtree', 'folder_navigation', 'search'):
        result['index_ms'][key] = stats([v for r in rows for v in r[key]['raw_ms']])
    asset_rows = [read(f) for f in files('assets', build, 'nova-c8-activity')]
    result['assets_ms'] = {key: stats([v for r in asset_rows for v in r[key]])
                           for key, value in asset_rows[0].items() if isinstance(value, list)}
    editor_rows = [read(f) for f in files('workloads', build, 'nova-editor-bench')]
    result['editor_ms'] = {}
    for group, cell in editor_rows[0].items():
        if not isinstance(cell, dict): continue
        for key, value in cell.items():
            if isinstance(value, dict) and 'raw_ms' in value:
                result['editor_ms'][group + '/' + key] = stats([v for r in editor_rows for v in r[group][key]['raw_ms']])
    result['batch'] = [r['batch_1024px_24images_3steps'] for r in editor_rows]
    workspace = [r for f in files('workspaces', build, 'nova-c8-activity') for r in read(f)['rows'] if r['cycle'] > 0]
    result['workspace_ms'] = {key: stats([r[key] for r in workspace]) for key in ('blocking_ms', 'last_paint_ms')}
    result['workspace_by_preset_ms'] = {name: stats([r['last_paint_ms'] for r in workspace if r['preset'] == name])
                                      for name in sorted({r['preset'] for r in workspace})}
    result['model_ms'] = {}
    model_rows = [read(f) for f in files('workloads', build, 'nova-model-bench')]
    for key in ('camera_readback_ms', 'pixel_update_readback_ms', 'render_submit_cpu_ms'):
        result['model_ms'][key] = stats([v for r in model_rows for v in r[key]])
    result['gpu_command_interval_ms'] = stats([v for f in files('gpu', build, 'nova-model-bench') for v in read(f)['gpu_draw_ms']])
    result['c7'] = [read(f) for f in files('workloads', build, 'nova-c7-bench')]
    result['c8'] = [read(f) for f in files('workloads', build, 'nova-c8-bench')]
    result['animation'] = [read(f) for f in files('workloads', build, 'nova-animation-bench')]
    followup = [read(f)['workloads'][5] for f in sorted((out / 'animation-followup').glob(build + '-nova-animation-bench-*.json'))]
    assert len(followup) == 6
    result['animation_followup_ms'] = {key: stats([r[key] for r in followup]) for key in
        ('creation_ms', 'native_guarded_load_ms', 'native_guarded_save_ms', 'timeline_scroll_and_thumbnails_ms')}
    cadence = [read(f)['workloads'][0] for f in sorted((out / 'playback-cadence').glob(build + '-nova-animation-bench-*.json'))]
    assert len(cadence) == 3
    result['playback_display_interval_ms'] = stats([v for r in cadence for v in r['playback_display_intervals_ms']])
    result['memory_attribution'] = [read(f) for f in files('memory', build, 'nova-c8-memory-probe')]
    result['storage'] = {kind: [read(f) for f in files('storage', build, 'nova-c6-storage-bench-' + kind)]
                         for kind in ('unique', 'repeated')}
    result['lifecycle'] = [read(f) for f in (out / 'final-lifecycle').glob(build + '-cycles-*.json')]
    process = [r for r in read(out / 'final-process/rows.json') if r['build'] == build and not r['first_observed']]
    result['process'] = {}
    for scenario in ('empty', 'small', '50k'):
        cell = [r for r in process if r['scenario'] == scenario]
        group = {key: stats([r[key] for r in cell]) for key in ('working_set_mib', 'private_mib', 'cpu_one_core_percent', 'handles', 'process_lifetime_ms')}
        for key in ('first_ui_ms', 'ready_capture_ms'):
            group[key] = stats([v for r in cell for v in r['telemetry']['samples_ms'].get(key, [])])
            group['external_' + key] = stats([r['external_marker_observed_ms'][key] for r in cell if key in r['external_marker_observed_ms']])
        group['startup_stages_ms'] = {key: stats([v for r in cell for v in r['telemetry']['samples_ms'].get(key, [])])
            for key in ('application_init', 'settings_load', 'theme_apply', 'editor_menus_icons',
                'editor_docks', 'ui_construction', 'workspace_shell', 'thumbnail_service_init',
                'index', 'opengl_initialize')}
        result['process'][scenario] = group
    result['idle'] = read(out / ('idle-baseline' if label == 'baseline' else 'idle-final') / 'summary.json')
    summary[label] = result
(out / 'performance-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')

lines = ['# Phase 3-C measured comparisons', '', 'All times below are milliseconds. See PHASE_3C_REPORT.md for scope and limitations.', '',
         '| Measurement | Baseline n / p50 / p95 / p99 | Final n / p50 / p95 / p99 | Median change |',
         '|---|---:|---:|---:|']
def row(name, before, after):
    def cell(s): return f"{s['n']} / {s['p50']:.3f} / {s['p95']:.3f} / {s['p99']:.3f}"
    change = 100 * (after['p50'] / before['p50'] - 1) if before['p50'] else 0
    lines.append(f'| {name} | {cell(before)} | {cell(after)} | {change:+.1f}% |')
for key in ('scan', 'refresh_file', 'refresh_assets_subtree'):
    row('50k ' + key, summary['baseline']['index_ms'][key], summary['final']['index_ms'][key])
row('50k content-only view installation', summary['baseline']['assets_ms']['apply_file_cpu_ms'], summary['final']['assets_ms']['apply_file_cpu_ms'])
for key in ('compositing/16layers_1024px_full_recomposite', 'compositing/64layers_512px_full_recomposite'):
    row(key, summary['baseline']['editor_ms'][key], summary['final']['editor_ms'][key])
row('transform preview: 512px, four layers', summary['baseline']['editor_ms']['transform/preview_composite_512_4layers'],
    summary['final']['editor_ms']['transform/preview_composite_512_4layers'])
for key in ('blocking_ms', 'last_paint_ms'):
    row('Workspace ' + key, summary['baseline']['workspace_ms'][key], summary['final']['workspace_ms'][key])
for scenario in ('empty', 'small', '50k'):
    row(scenario + ' first UI (internal)', summary['baseline']['process'][scenario]['first_ui_ms'], summary['final']['process'][scenario]['first_ui_ms'])
for scenario in ('small', '50k'):
    row(scenario + ' capture-ready proxy (internal)', summary['baseline']['process'][scenario]['ready_capture_ms'], summary['final']['process'][scenario]['ready_capture_ms'])
row('Focused follow-up: 1024-source / 16384-entry timeline (one sample per invocation)',
    summary['baseline']['animation_followup_ms']['timeline_scroll_and_thumbnails_ms'],
    summary['final']['animation_followup_ms']['timeline_scroll_and_thumbnails_ms'])
lines += ['', 'Full distributions, means, variance, memory, idle counters and operation-specific raw results are in performance-summary.json and the final-* directories.', '']
(out / 'PERFORMANCE.md').write_text('\n'.join(lines), encoding='utf-8')
print('\n'.join(lines))
