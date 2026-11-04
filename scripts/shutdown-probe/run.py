"""Bounded process-exit campaign; does not modify existing fixtures or settings.

Usage: .tools/python/Scripts/python.exe scripts/shutdown-probe/run.py OUTPUT_DIR [NEW_WORK_DIR]
Build Release and build.ps1 first. Requires local psutil and Pillow.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time
from collections import Counter

import psutil
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=False)
EXE = ROOT / 'build/studio-c3-closeout-release/src/nova-studio.exe'
PROBE = ROOT / 'build/studio-c3-shutdown-probe/nova-shutdown-probe.exe'
ENV = {**os.environ, 'PATH': str(ROOT / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
WORK = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else ROOT / 'build/studio-c3-closeout-workloads'
WORK.mkdir(exist_ok=False)
# New files only: keep all original fixture bytes and recovery data intact.
batch_root = WORK / 'batch-pack'
for i in range(32):
    p = batch_root / f'assets/minecraft/textures/block/b{i}.png'
    p.parent.mkdir(parents=True, exist_ok=True)
    Image.new('RGBA', (64, 64), (i * 7, 80, 140, 255)).save(p)
recovery_root = WORK / 'large-texture-pack'
texture = recovery_root / 'assets/minecraft/textures/block/a.png'
texture.parent.mkdir(parents=True)
Image.effect_noise((1024, 1024), 80).convert('RGBA').save(texture)
rows = []
timeout_seconds = 20
def run(kind, executable, args, marker=None):
    n = len(rows)
    name = f'{n:03d}-{kind}'
    run_dir = WORK / name
    run_dir.mkdir()
    if executable == EXE:
        args += ['--settings', str(run_dir / 'settings.ini'), '--telemetry', str(OUT / f'{name}-telemetry.json')]
    else:
        args += ['--run-dir', str(run_dir)]
    command = [str(executable), *args]
    before = time.perf_counter()
    diagnostics = []
    with (OUT / f'{name}-stdout.txt').open('w') as stdout, (OUT / f'{name}-stderr.txt').open('w') as stderr:
        child = subprocess.Popen(command, cwd=ROOT, env=ENV, stdout=stdout, stderr=stderr)
        proc = psutil.Process(child.pid)
        timed_out = False
        while child.poll() is None:
            age = time.perf_counter() - before
            if age >= timeout_seconds:
                timed_out = True
                try:
                    diagnostics.append({'at_timeout': proc.as_dict(attrs=['pid', 'status', 'num_threads', 'num_handles', 'cpu_times', 'memory_info'])})
                except psutil.Error as exc:
                    diagnostics.append({'inspection_error': str(exc)})
                # Kill only this campaign child after recording the watchdog incident.
                child.kill()
                break
            if int(age * 10) % 10 == 0:
                try:
                    diagnostics.append({'seconds': age, 'threads': proc.num_threads(), 'handles': proc.num_handles(), 'cpu_seconds': sum(proc.cpu_times()[:2])})
                except psutil.Error:
                    pass
            time.sleep(.05)
        code = child.wait(timeout=5)
    text = (OUT / f'{name}-stdout.txt').read_text()
    marker_seen = marker is None or marker in text
    rows.append({'name': name, 'kind': kind, 'pid': child.pid, 'command': command,
                 'exit_code': code, 'timeout': timed_out, 'elapsed_seconds': time.perf_counter() - before,
                 'required_marker': marker, 'marker_seen': marker_seen, 'process_samples': diagnostics})
    (OUT / 'runs.json').write_text(json.dumps(rows, indent=2, default=list) + '\n')
    print(f'{name}: exit={code}, timeout={timed_out}, workload={marker_seen}', flush=True)
    if timed_out or code != 0 or not marker_seen:
        raise RuntimeError(f'Campaign stopped on incident: {name}; inspect retained evidence')

for i in range(12):
    # Alternate early close, 2-second deferred page boundary, and normal warm close.
    seconds = ('.1', '.75', '2', '3')[i % 4]
    for kind, pack, extra in (
        ('empty', None, []), ('small', ROOT / 'fixtures/generated/pack-1500', []),
        ('large', ROOT / 'fixtures/generated/pack-50000', []),
        ('floating', ROOT / 'fixtures/generated/pack-1500', ['--float-preview']),
    ):
        args = ['--fresh-layout', '--smoke-seconds', seconds, *extra]
        if pack: args += ['--project', str(pack)]
        run(kind, EXE, args)
for i in range(8):
    for mode, marker in (('index', 'rescan_started'), ('thumbnails', 'thumbnails_requested'),
                         ('recovery', 'snapshot_started'), ('preview', 'preview_worker_active'),
                         ('batch', 'batch_destroy_begin'), ('floating', 'docks_floated')):
        pack = recovery_root if mode in ('recovery', 'preview') else ROOT / 'fixtures/generated/pack-50000'
        run('probe-' + mode, PROBE, ['--fresh-layout', '--mode', mode, '--project', str(pack), '--batch-root', str(batch_root)], marker)
summary = {
    'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    'timeout_seconds_from_process_launch': timeout_seconds,
    'production_executable_sha256': hashlib.sha256(EXE.read_bytes()).hexdigest(),
    'probe_executable_sha256': hashlib.sha256(PROBE.read_bytes()).hexdigest(),
    'launches': len(rows), 'successful_exits': sum(r['exit_code'] == 0 and not r['timeout'] for r in rows),
    'timeouts': sum(r['timeout'] for r in rows), 'nonzero_exits': sum(r['exit_code'] != 0 for r in rows),
    'by_kind': dict(Counter(r['kind'] for r in rows)),
}
(OUT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2), flush=True)
