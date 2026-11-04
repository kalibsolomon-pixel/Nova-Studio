"""Extend the existing native probes; fixed fixtures, isolated settings, no cleanup.

Never run alongside a build/test/other benchmark. All timings are CPU or software
repaint proxies; GPU readbacks remain explicitly named by the retained probes.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import time

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--builds', nargs='+', required=True)
p.add_argument('--runs', type=int, default=3)
p.add_argument('--mode', choices=['workloads', 'animation', 'playback', 'workspaces', 'memory', 'index', 'assets', 'storage', 'gpu'], default='workloads')
a = p.parse_args()
out = root / a.output
out.mkdir(parents=True, exist_ok=False)
pack = out / 'pack'
shutil.copytree(root / 'fixtures/phase-2c5-models', pack, ignore=shutil.ignore_patterns('.nova'))
env = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH'],
       'NOVA_LARGE_FIXTURE': str(root / 'fixtures/generated/pack-50000')}
identities = {}
for build in a.builds:
    folder = root / 'build' / build
    identities[build] = dict(identity=(folder / 'generated/build_identity.h').read_text(),
        binaries={str(f.relative_to(folder)): hashlib.file_digest(f.open('rb'), 'sha256').hexdigest()
                  for part in ('src', 'tests') for f in (folder / part).glob('*.exe')})
(out / 'identities.json').write_text(json.dumps(identities, indent=2) + '\n')
rows = []
def run(build, n, binary, args, suffix=''):
    name = f'{build}-{binary}{suffix}-{n}'
    destination = out / (name + '.json')
    command = [str(root / 'build' / build / 'tests' / (binary + '.exe')),
               *[str(destination) if arg == '$output' else str(arg) for arg in args]]
    start = time.perf_counter()
    with (out / (name + '.log')).open('wb') as log:
        result = subprocess.run(command, env=env, cwd=root, stdout=log, stderr=log, timeout=900,
                                creationflags=subprocess.CREATE_NO_WINDOW)
    rows.append(dict(build=build, run=n, binary=binary, wall_seconds=time.perf_counter()-start,
                     exit_code=result.returncode, command=command))
    (out / 'runs.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(name, result.returncode, round(rows[-1]['wall_seconds'], 2), flush=True)
    if result.returncode: raise RuntimeError(name)
for n in range(a.runs):
    # Reverse order every run to limit consistent temperature/order bias.
    for build in a.builds if n % 2 == 0 else list(reversed(a.builds)):
        if a.mode == 'workloads':
            for binary in ('nova-editor-bench', 'nova-c7-bench', 'nova-c8-bench', 'nova-animation-bench'):
                run(build, n, binary, ['$output'])
            run(build, n, 'nova-model-bench', [pack, '$output'])
        elif a.mode == 'animation':
            run(build, n, 'nova-animation-bench', ['$output'])
        elif a.mode == 'playback':
            run(build, n, 'nova-animation-bench', ['$output', '--playback-timing'])
        elif a.mode == 'index':
            run(build, n, 'nova-editor-bench', ['$output', '--index-only'])
        elif a.mode == 'storage':
            for kind in ('repeated', 'unique'):
                run(build, n, 'nova-c6-storage-bench', ['$output', kind, '256', '--cycles'], '-' + kind)
        elif a.mode == 'gpu':
            run(build, n, 'nova-model-bench', [pack, '$output', '--gpu'])
        elif a.mode in ('workspaces', 'assets'):
            run(build, n, 'nova-c8-activity', ['--project', env['NOVA_LARGE_FIXTURE'] if a.mode == 'assets' else pack,
                '--fresh-layout', '--settings', out / f'{build}-{n}.ini', '--scenario', a.mode, '--activity', '$output'])
        else:
            run(build, n, 'nova-c8-memory-probe', [pack, env['NOVA_LARGE_FIXTURE'], '$output'])
