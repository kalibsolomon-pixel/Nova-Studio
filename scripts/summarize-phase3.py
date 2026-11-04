"""Derive compact performance tables from retained raw Phase 3 observations."""
import json
import pathlib
import statistics

root = pathlib.Path(__file__).resolve().parents[1]/'docs/phase-3'
out = {}
def median(values): return statistics.median(values)
rows = json.loads((root/'performance-process/rows.json').read_text(encoding='utf-8'))
for build in sorted({r['build'] for r in rows}):
    for scenario in ('empty', 'small', '50k'):
        cell = [r for r in rows if r['build'] == build and r['scenario'] == scenario and not r['first_observed']]
        data = {k: median(r[k] for r in cell) for k in ('cpu_one_core_percent', 'working_set_mib', 'private_mib', 'handles')}
        for key in ('first_ui_ms', 'ready_capture_ms', 'index'):
            values = [r['telemetry']['samples_ms'][key][0] for r in cell if key in r['telemetry']['samples_ms']]
            if values: data[key] = median(values)
        out.setdefault('process', {})[f'{build}/{scenario}'] = data
for stage in ('before', 'after'):
    directory = root/('idle-welcome-'+stage)
    cells = json.loads((directory/'rows.json').read_text(encoding='utf-8'))
    events = [json.loads(f.read_text(encoding='utf-8'))['events'] for f in sorted(directory.glob('welcome-?.json'))]
    out.setdefault('welcome_idle', {})[stage] = dict(
        process_cpu_percent=[r['cpu_percent_one_core'] for r in cells],
        dispatcher_awake_per_second=[r['dispatcher_awake_per_second'] for r in cells],
        all_paint_events=[r['paint_events'] for r in cells],
        path_field_paints=[e.get('12:QLineEdit:projectPath',0) for e in events],
        move_events=[e.get('qt_event_13',0) for e in events],
        resize_events=[e.get('qt_event_14',0) for e in events])
work = root/'performance-workloads'
for build in ('studio-c8-accepted-release', 'studio-phase3-release'):
    cells = [json.loads(f.read_text(encoding='utf-8')) for f in sorted(work.glob(build+'-nova-c8-bench-*.json'))]
    if cells:
        out.setdefault('c8_workloads', {})[build] = {
            key: {metric: median(c[key][metric] for c in cells) for metric in ('median_ms','p95_ms','max_ms')}
            for key in ('bulk_uv_12_faces','display_preview_update')}
        out['c8_workloads'][build]['large_animation'] = {
            kind: median(c['large_animation_native'][kind]['median_ms'] for c in cells) for kind in ('save','load')}
        out['c8_workloads'][build]['native_batch'] = [c.get('native_batch') for c in cells]
    cells = [json.loads(f.read_text(encoding='utf-8')) for f in sorted(work.glob(build+'-nova-c7-bench-*.json'))]
    if cells:
        keys = ('validation_ms','cached_validation_ms','resource_pack_export_ms','model_texture_batch_ms',
                'dependency_resolution_ms','problems_async_population_ms')
        out.setdefault('authoring', {})[build] = {k: median(c[k] for c in cells) for k in keys if all(k in c for c in cells)}
    for kind in ('repeated','unique'):
        cells = [json.loads(f.read_text(encoding='utf-8')) for f in sorted(work.glob(build+'-'+kind+'-256-*.json'))]
        if cells:
            out.setdefault('storage', {})[build+'/'+kind] = {k: median(c[k] for c in cells) for k in (
                'cold_encode_ms','decode_ms','guarded_save_ms','recovery_ms','native_bytes','decoded_bytes',
                'private_before','private_after_encode','private_after_decode','private_after_recovery')}
directory = root/'memory-lifecycle'
for build in ('studio-c8-accepted-release', 'studio-phase3-release'):
    cells = []
    for file in sorted(directory.glob(build+'-cycles-*.json')):
        data = json.loads(file.read_text(encoding='utf-8'))
        closed = [s for s in data['samples'] if s['label'].endswith('-closed')]
        cells.append(dict(run=file.name, closed_documents=[s['documents'] for s in closed],
            first_closed=closed[0], last_closed=closed[-1],
            closed_working_set_mib=[s['working_set_mib'] for s in closed],
            closed_private_mib=[s['private_mib'] for s in closed],
            closed_gpu_counts=[s['gpu'] for s in closed]))
    out.setdefault('lifecycle', {})[build] = cells
for build in ('studio-c8-accepted-release', 'studio-phase3-release'):
    cells = [json.loads(f.read_text(encoding='utf-8')) for f in sorted(work.glob(build+'-models-*.json'))]
    keys = ('index_ms','catalog_parse_ms','cached_model_1000_ms','uncached_parent_resolution_1000_ms',
            'mesh_1000_ms','uv_selection_100_ms','animated_model_playback_cpu_ms','animated_model_playback_wall_ms')
    out.setdefault('models', {})[build] = {key: median(c[key] for c in cells)
                                           for key in keys if cells and all(isinstance(c.get(key), (int,float)) for c in cells)}
    cells = [json.loads(f.read_text(encoding='utf-8')) for f in sorted(work.glob(build+'-nova-editor-bench-*.json'))]
    out.setdefault('editor_and_png_batch', {})[build] = {
        'raw_repeats': len(cells),
        'batch_64px_3steps': [c.get('batch_64px_3steps') for c in cells],
        'stroke_segment': [c.get('stroke_segment') for c in cells]}
file = root/'idle-scenarios/summary.json'
if file.exists(): out['idle_scenarios'] = json.loads(file.read_text(encoding='utf-8'))['summary']
(root/'performance-summary.json').write_text(json.dumps(out, indent=2)+'\n', encoding='utf-8')
print(json.dumps(out, indent=2))
