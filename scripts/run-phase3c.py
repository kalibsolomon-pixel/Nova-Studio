"""Sequential final validation/measurement driver. Never overlaps native probes."""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('stage', choices=['tests', 'benchmarks'])
p.add_argument('--label', default='', help='Unique suffix for a further complete validation pass')
p.add_argument('--output-root', default='docs/phase-3c', help='Fresh evidence directory for a repeated matrix')
a = p.parse_args()
if a.label and not all(c.isalnum() or c == '-' for c in a.label):
    raise SystemExit('Label must contain only letters, digits and hyphens')
suffix = '-' + a.label if a.label else ''
out = root / a.output_root
out.mkdir(parents=True, exist_ok=True)
env = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
builds = ['studio-phase3c-baseline-release', 'studio-phase3c-release']
steps = []
def run(label, command, extra=None):
    print('START', label, flush=True)
    start = time.perf_counter()
    with (out / (label + '-driver.log')).open('xb') as log:
        result = subprocess.run(command, cwd=root, env={**env, **(extra or {})}, stdout=log, stderr=log,
                                timeout=3600, creationflags=subprocess.CREATE_NO_WINDOW)
    steps.append(dict(step=label, seconds=time.perf_counter()-start, exit_code=result.returncode, command=list(map(str, command))))
    (out / (a.stage + suffix + '-steps.json')).write_text(json.dumps(steps, indent=2) + '\n')
    print('END', label, result.returncode, round(steps[-1]['seconds'], 2), flush=True)
    if result.returncode: raise SystemExit(result.returncode)

if a.stage == 'tests':
    for configuration in ('release', 'debug'):
        run('final-tests-' + configuration + suffix, [sys.executable, root / 'scripts/validate-phase3.py',
            'studio-phase3c-' + configuration, out / ('tests-' + configuration + suffix)])
    for label, build in zip(('baseline', 'final'), builds):
        gallery = out / ('gallery-' + label + suffix)
        gallery.mkdir(exist_ok=False)
        run('gallery-' + label + suffix, [root / 'build' / build / 'tests/nova-phase3b-gallery.exe',
            '-o', str(gallery / 'gallery.txt') + ',txt'], {'NOVA_PHASE3B_GALLERY_DIR': str(gallery)})
else:
    for mode, runs in [('index', 3), ('assets', 3), ('workloads', 3), ('workspaces', 3),
                       ('memory', 3), ('gpu', 3), ('storage', 2)]:
        run('final-' + mode, [sys.executable, root / 'scripts/profile-phase3c.py',
            out / ('final-' + mode), '--builds', *builds, '--runs', str(runs), '--mode', mode])
    run('final-lifecycle', [sys.executable, root / 'scripts/profile-phase3.py', out / 'final-lifecycle',
        '--builds', *builds, '--runs', '3', '--mode', 'lifecycle'])
    run('final-process', [sys.executable, root / 'scripts/profile-phase3.py', out / 'final-process',
        '--builds', *builds, '--runs', '10', '--mode', 'process', '--observe-startup'])
    for label, build, runs in [('baseline', builds[0], 1), ('final', builds[1], 2)]:
        run('idle-' + label, [sys.executable, root / 'scripts/profile-phase3-idle.py',
            out / ('idle-' + label), '--build', build, '--runs', str(runs), '--settle', '12', '--seconds', '35',
            '--scenarios', 'welcome', 'empty', 'texture', 'layered', 'animation-paused', 'model', 'model-paused', 'large-50k'])
