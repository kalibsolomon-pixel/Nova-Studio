"""Windows-only production/prototype comparison; never writes fixture textures.

Run from the workspace venv. Each invocation has a unique evidence directory.
Startup is an OS-warm process-to-framebuffer-ready proxy, not input latency.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time
import psutil

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs/phase-2a/performance' / str(time.time_ns())
OUT.mkdir(parents=True, exist_ok=False)
ENV = {**os.environ, 'PATH': str(ROOT / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
EXES = {'production': ROOT / 'build/studio-release/src/nova-studio.exe',
        'prototype': ROOT / 'build/qt-review/nova-qt.exe'}

def write(path, data):
    path.write_text(json.dumps(data, indent=2), encoding='utf-8')

def run(name, *args, timeout=60):
    result = subprocess.run([str(EXES[name]), *map(str, args)], cwd=ROOT, env=ENV,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError((name, args, result.returncode, result.stdout.decode(errors='replace')))
    return result

def quantiles(values):
    values = sorted(values)
    return {'n': len(values), 'p50': statistics.median(values),
            'p95': values[max(0, math.ceil(len(values)*.95)-1)]}

summary = {'method': 'Windows 11, OS-warm caches, shared desktop, CPU percent of one logical processor',
           'binaries': {name: {'path': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
                        for name, exe in EXES.items()}, 'core': {}, 'startup_ms': {}}
write(OUT/'summary.json', summary)
if '--idle-only' not in sys.argv:
    for size in (1500, 50000):
        for name in EXES:
            samples = {}
            for iteration in range(3):
                output = OUT/f'{name}-core-{size}-{iteration}.json'
                run(name, '--bench', ROOT/f'fixtures/generated/pack-{size}', '--output', output)
                data = json.loads(output.read_text())
                assert data['counters']['assets'] == size
                for metric, values in data['samples_ms'].items(): samples.setdefault(metric, []).extend(values)
            summary['core'][f'{name}-{size}'] = {key: quantiles(values) for key, values in samples.items()}
            write(OUT/'summary.json', summary)
    print('Core comparisons completed', flush=True)

    starts = {name: [] for name in EXES}
    for iteration in range(10):
        for name in (list(EXES) if iteration % 2 == 0 else list(reversed(EXES))):
            trace = OUT/f'{name}-startup-{iteration}.json'
            before = time.perf_counter()
            proc = subprocess.Popen([str(EXES[name]), '--project', str(ROOT/'fixtures/generated/pack-1500'),
                '--fresh-layout', '--telemetry', str(trace), '--smoke-seconds', '1.5'], cwd=ROOT, env=ENV)
            ready = False
            while time.perf_counter()-before < 15:
                if trace.exists():
                    try:
                        ready = bool(json.loads(trace.read_text())['samples_ms'].get('ready_capture_ms'))
                    except (ValueError, KeyError): pass
                    if ready: break
                if proc.poll() is not None: break
                time.sleep(.01)
            if not ready: raise RuntimeError(f'{name} did not reach the ready marker')
            starts[name].append((time.perf_counter()-before)*1000)
            assert proc.wait(timeout=15) == 0
    summary['startup_ms'] = {name: quantiles(values) for name, values in starts.items()}
    write(OUT/'startup-raw.json', starts)
    write(OUT/'summary.json', summary)
    print('20 alternating startup runs completed', flush=True)

    run('production', '--project', ROOT/'fixtures/generated/pack-50000', '--fresh-layout',
        '--exercise', '--telemetry', OUT/'production-exercise-50000.json')
    print('50,000-asset editing exercise completed', flush=True)

# Same simultaneous Qt processes, five-minute settle + final twenty seconds as Phase 1.
processes = {}
for name, exe in EXES.items():
    proc = subprocess.Popen([str(exe), '--project', str(ROOT/'fixtures/generated/pack-1500'),
        '--fresh-layout', '--telemetry', str(OUT/f'{name}-idle.json'),
        '--smoke-seconds', '327'], cwd=ROOT, env=ENV)
    processes[name] = (proc, psutil.Process(proc.pid))
print('Five-minute idle soak started', flush=True)
time.sleep(300)
idle = {}
for name, (_, process) in processes.items():
    cpu = process.cpu_times()
    idle[name] = {'cpu_start': cpu.user+cpu.system, 'start': time.perf_counter(), 'working_set_mib': []}
for _ in range(20):
    time.sleep(1)
    for name, (_, process) in processes.items():
        idle[name]['working_set_mib'].append(process.memory_info().rss/1024**2)
    write(OUT/'idle-observations.json', idle)
for name, (proc, process) in processes.items():
    cpu = process.cpu_times()
    item = idle[name]
    item['cpu_percent_one_processor'] = 100*(cpu.user+cpu.system-item.pop('cpu_start'))/(time.perf_counter()-item.pop('start'))
    item['working_set_mib_median'] = statistics.median(item['working_set_mib'])
summary['five_minute_idle'] = idle
write(OUT/'summary.json', summary)
for proc, _ in processes.values():
    assert proc.wait(timeout=20) == 0
print(str(OUT), flush=True)
