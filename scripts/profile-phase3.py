"""Matched before/after process, storage, rendering and authoring measurements.

Fresh settings and fixture copies; no native input and no recursive cleanup.
Run while builds and acceptance suites are idle. First launch is retained as a
first-observed sample, not asserted to be OS-cache-cold. Subsequent runs are warm.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import statistics
import subprocess
import time
import psutil

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--builds', nargs='+', default=['studio-c8-accepted-release', 'studio-phase3-release'])
p.add_argument('--runs', type=int, default=3)
p.add_argument('--mode', choices=['process', 'workloads', 'lifecycle'], default='process')
p.add_argument('--observe-startup', action='store_true', help='Observe first-UI/capture telemetry writes externally (10 ms polling)')
a = p.parse_args()
out = root / a.output
out.mkdir(parents=True, exist_ok=False)
environment = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH'],
               'NOVA_LARGE_FIXTURE': str(root / 'fixtures/generated/pack-50000')}
fixture = out / 'pack'
shutil.copytree(root / 'fixtures/phase-2c5-models', fixture, ignore=shutil.ignore_patterns('.nova'))
rows = []
identities = {}
for build in a.builds:
    folder = root/'build'/build
    identities[build] = {'build_identity': (folder/'generated/build_identity.h').read_text(encoding='utf-8')
                         if (folder/'generated/build_identity.h').exists() else 'preserved C8 build; see baseline evidence',
                         'binaries': {str(file.relative_to(folder)): hashlib.file_digest(file.open('rb'), 'sha256').hexdigest()
                                      for part in ('src', 'tests') for file in (folder/part).glob('*.exe')}}
(out/'binary-identities.json').write_text(json.dumps(identities, indent=2)+'\n', encoding='utf-8')
def run(exe, arguments, label, timeout=300):
    with (out / (label + '.log')).open('wb') as log:
        result = subprocess.run([str(exe), *map(str, arguments)], env=environment, cwd=root,
                                stdout=log, stderr=log, timeout=timeout,
                                creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError((label, result.returncode))

if a.mode == 'process':
    # Interleave builds to reduce drift. Every observed sample is retained.
    for n in range(a.runs + 1):
        for label, project in [('empty', None), ('small', fixture),
                               ('50k', root / 'fixtures/generated/pack-50000')]:
            for build in a.builds:
                name = f'{build}-{label}-{n}'
                exe = root / 'build' / build / 'src/nova-studio.exe'
                telemetry = out / (name + '.json')
                command = [str(exe), '--settings', str(out / (name + '.ini')), '--fresh-layout',
                           '--smoke-seconds', '7', '--telemetry', str(telemetry)]
                if project: command += ['--project', str(project)]
                launched = time.perf_counter()
                observations = {}
                with (out / (name + '.log')).open('wb') as log:
                    child = subprocess.Popen(command, cwd=root, env=environment, stdout=log, stderr=log,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
                    proc = psutil.Process(child.pid)
                    if a.observe_startup:
                        modified = None
                        while child.poll() is None and time.perf_counter() - launched < 3:
                            try:
                                stamp = telemetry.stat().st_mtime_ns
                                if stamp != modified:
                                    data = json.loads(telemetry.read_text(encoding='utf-8'))
                                    modified = stamp
                                    for key in ('first_ui_ms', 'ready_capture_ms'):
                                        if data.get('samples_ms', {}).get(key) and key not in observations:
                                            observations[key] = 1000 * (time.perf_counter() - launched)
                            except (OSError, ValueError):
                                pass  # telemetry may be in the middle of its write
                            time.sleep(.01)
                    else:
                        time.sleep(3)
                    before = proc.cpu_times()
                    threads = {t.id: t.user_time + t.system_time for t in proc.threads()}
                    started = time.perf_counter()
                    time.sleep(2)
                    after = proc.cpu_times()
                    elapsed = time.perf_counter() - started
                    memory = proc.memory_info()
                    thread_rows = [dict(id=t.id, cpu_ms=1000*(t.user_time+t.system_time-threads.get(t.id,0)))
                                   for t in proc.threads()]
                    handles = proc.num_handles()
                    child.wait(timeout=25)
                    lifetime = time.perf_counter() - launched
                if child.returncode: raise RuntimeError((name, child.returncode))
                rows.append(dict(build=build, scenario=label, run=n,
                    first_observed=n == 0, seconds=elapsed,
                    external_marker_observed_ms=observations,
                    process_lifetime_ms=lifetime * 1000,
                    cpu_one_core_percent=100*(after.user+after.system-before.user-before.system)/elapsed,
                    working_set_mib=memory.rss/1048576, private_mib=memory.private/1048576,
                    handles=handles, thread_cpu_ms=thread_rows,
                    executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                    telemetry=json.loads(telemetry.read_text(encoding='utf-8'))))
                (out / 'rows.json').write_text(json.dumps(rows, indent=2)+'\n', encoding='utf-8')
                print(name, round(rows[-1]['cpu_one_core_percent'], 3), flush=True)
    summary = {}
    for build in a.builds:
        for scenario in ('empty', 'small', '50k'):
            cell = [r for r in rows if r['build'] == build and r['scenario'] == scenario and not r['first_observed']]
            keys = ('cpu_one_core_percent', 'working_set_mib', 'private_mib', 'handles')
            summary[f'{build}/{scenario}'] = {
                k: dict(median=statistics.median(r[k] for r in cell),
                        min=min(r[k] for r in cell), max=max(r[k] for r in cell)) for k in keys}
    (out / 'summary.json').write_text(json.dumps(dict(summary=summary, runs=a.runs,
        notes='One logical processor = 100%. First observed launch retained separately; no OS cache flushing.'), indent=2)+'\n', encoding='utf-8')
elif a.mode == 'workloads':
    for build in a.builds:
        base = root / 'build' / build / 'tests'
        for n in range(a.runs):
            for binary in ('nova-c8-bench', 'nova-c7-bench', 'nova-editor-bench'):
                name = f'{build}-{binary}-{n}'
                run(base / (binary + '.exe'), [out / (name + '.json')], name)
            name = f'{build}-models-{n}'
            run(base / 'nova-model-bench.exe', [fixture, out / (name + '.json')], name)
            for kind, side in [('repeated', 256), ('unique', 256)]:
                name = f'{build}-{kind}-{side}-{n}'
                run(base / 'nova-c6-storage-bench.exe', [out / (name + '.json'), kind, side], name)
            print(build, n, 'workloads complete', flush=True)
else:
    for build in a.builds:
        for n in range(a.runs):
            name = f'{build}-cycles-{n}'
            run(root / 'build' / build / 'tests/nova-c8-activity.exe',
                ['--project', fixture, '--settings', out / (name+'.ini'), '--scenario', 'cycles',
                 '--cycles', '12', '--activity', out / (name+'.json')], name, timeout=180)
            print(name, 'complete', flush=True)
