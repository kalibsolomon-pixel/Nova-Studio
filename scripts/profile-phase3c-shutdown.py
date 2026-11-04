"""Reuse the retained shutdown probe; each run owns its inputs and settings."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time
from random import Random
from PIL import Image

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', default='docs/phase-3c/final-shutdown')
args = parser.parse_args()
out = root / args.output
out.mkdir(parents=True, exist_ok=False)
env = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
builds = ['studio-phase3c-baseline-release', 'studio-phase3c-release']
pack = out / 'pack'
shutil.copytree(root / 'fixtures/phase-2c5-models', pack, ignore=shutil.ignore_patterns('.nova'))
large = out / 'large-pack'
path = large / 'assets/nova/textures/block/t.png'
path.parent.mkdir(parents=True)
Image.frombytes('RGB', (1024, 1024), Random(0x3c).randbytes(1024 * 1024 * 3)).convert('RGBA').save(path)
batch = out / 'batch-pack'
for i in range(32):
    path = batch / f'assets/nova/textures/block/t{i}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new('RGBA', (64, 64), (i * 7, 80, 140, 255)).save(path)
rows = []
identities = {}
for build in builds:
    executable = root / 'build' / (build + '-shutdown/nova-shutdown-probe.exe')
    identities[build] = dict(probe_sha256=hashlib.file_digest(executable.open('rb'), 'sha256').hexdigest(),
        build_identity=(root / 'build' / build / 'generated/build_identity.h').read_text(),
        archives={f.name: hashlib.file_digest(f.open('rb'), 'sha256').hexdigest()
                  for f in (root / 'build' / build / 'src').glob('*.lib')})
(out / 'identities.json').write_text(json.dumps(dict(builds=identities,
    fixture_sha256={str(f.relative_to(out)): hashlib.file_digest(f.open('rb'), 'sha256').hexdigest()
                    for folder in (pack, large, batch) for f in folder.rglob('*') if f.is_file()}), indent=2)+'\n')
for n in range(3):
    for mode in ('settled', 'index', 'thumbnails', 'recovery', 'preview', 'batch', 'floating'):
        for build in builds if n % 2 == 0 else list(reversed(builds)):
            name = f'{build}-{mode}-{n}'
            # Keep repeated experiments isolated from earlier task-owned files.
            run = root / 'build/phase3c-shutdown-runs' / out.name / name
            run.mkdir(parents=True, exist_ok=False)
            command = [str(root / 'build' / (build + '-shutdown/nova-shutdown-probe.exe')),
                '--fresh-layout', '--project', str(large if mode in ('recovery', 'preview') else
                    root / 'fixtures/generated/pack-50000' if mode in ('index', 'thumbnails') else pack),
                '--mode', mode, '--batch-root', str(batch), '--run-dir', str(run)]
            start = time.perf_counter()
            with (out / (name + '.txt')).open('wb') as log:
                result = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=log,
                    timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
            events = []
            for line in (out / (name + '.txt')).read_text(errors='replace').splitlines():
                try:
                    event = json.loads(line)
                    if isinstance(event, dict) and 'event' in event: events.append(event)
                except ValueError: pass
            times = {event['event']: event['ms'] for event in events}
            intervals = {}
            for label, first, last in [('close_event_ms','close_enter','close_return'),
                ('close_to_window_destroyed_ms','close_requested','window_destroyed'),
                ('window_destructor_ms','window_destructor_enter','window_destroyed'),
                ('application_destructor_ms','window_destroyed','application_destroyed'),
                ('close_to_application_destroyed_ms','close_requested','application_destroyed'),
                ('batch_cancel_join_ms','batch_destroy_begin','batch_destroy_end')]:
                if first in times and last in times: intervals[label] = times[last] - times[first]
            rows.append(dict(build=build, mode=mode, run=n, exit_code=result.returncode,
                wall_seconds=time.perf_counter()-start, intervals=intervals, events=events))
            (out / 'rows.json').write_text(json.dumps(rows, indent=2)+'\n')
            print(name, result.returncode, intervals, flush=True)
            if result.returncode or 'application_destroyed' not in times:
                raise SystemExit('Shutdown probe failed; retained its observations')
