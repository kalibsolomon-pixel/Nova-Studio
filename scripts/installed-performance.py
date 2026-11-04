"""Phase 3-E installed-application performance probe.

Measures the INSTALLED executable (never the build tree) with the same metric
definitions as the Phase 3-C/3-D.1 process evidence (docs/phase-3d1/performance):
external first-UI marker from --telemetry readiness, settled CPU over a 20 s
window, working set / private bytes / handles, and external shutdown time.
Scenarios: empty folder project, small pack, 50k-asset pack.

Usage: .tools/python/Scripts/python.exe scripts/installed-performance.py <install-exe> <output-dir> [--runs N]
"""
import argparse
import json
import os
import pathlib
import subprocess
import time

import psutil

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('exe')
p.add_argument('output')
p.add_argument('--runs', type=int, default=6)
a = p.parse_args()
exe = pathlib.Path(a.exe)
out = root / a.output
if out.exists():
    raise SystemExit(f'Refusing to overwrite {out}')
out.mkdir(parents=True)
scenarios = {
    'empty': {'project': None},
    'small': {'project': str(root / 'fixtures/generated/pack-10000')},
    '50k': {'project': str(root / 'fixtures/generated/pack-50000')},
}
env = {**os.environ, 'NOVA_LARGE_FIXTURE': str(root / 'fixtures/generated/pack-50000')}
cwd = pathlib.Path(os.environ['SystemRoot'])
rows = []
for run_index in range(a.runs):
    for name in (sorted(scenarios) if run_index % 2 else reversed(sorted(scenarios))):
        telemetry = out / f'{name}-{run_index}-telemetry.json'
        project = scenarios[name]['project']
        args = [str(exe), '--telemetry', str(telemetry), '--smoke-seconds', '35',
                '--settings', str(out / f'{name}-{run_index}-settings.ini')]
        if project:
            args += ['--project', project]
        started = time.perf_counter()
        proc = subprocess.Popen(args, cwd=str(cwd), env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        info = psutil.Process(proc.pid)
        first_ui = None
        if project:
            # The app flushes telemetry when the 3D preview first swaps a frame
            # (a document is open), which is the readiness marker the Phase 3-C/D
            # process evidence used.
            while time.perf_counter() - started < 30:
                if telemetry.exists():
                    try:
                        data = json.loads(telemetry.read_text(encoding='utf-8'))
                        if data.get('samples_ms', {}).get('ready_capture_ms'):
                            first_ui = (time.perf_counter() - started) * 1000.0
                            break
                    except (ValueError, PermissionError):
                        pass
                if proc.poll() is not None:
                    break
                time.sleep(0.002)
        else:
            # No project => no document => no frame-swap marker; the window is up
            # once the process survives past its first second.
            time.sleep(2)
            first_ui = None if proc.poll() is not None else 2000.0
        time.sleep(10)  # settle past indexing/startup churn
        cpu0, rss, private, handles = info.cpu_times(), None, None, None
        window_start = time.perf_counter()
        samples = []
        for _ in range(20):
            time.sleep(1)
            mem = info.memory_info()
            samples.append(mem.rss)
            rss, private = mem.rss, mem.private or (mem.rss - mem.shared if hasattr(mem, 'shared') else mem.rss)
            handles = info.num_handles()
        cpu1 = info.cpu_times()
        cpu_percent = 100.0 * ((cpu1.user + cpu1.system) - (cpu0.user + cpu0.system)) / (time.perf_counter() - window_start)
        exit_begin = time.perf_counter()
        proc.communicate(timeout=60)
        shutdown_ms = (time.perf_counter() - exit_begin) * 1000.0
        telemetry_data = {}
        if telemetry.exists():
            try:
                telemetry_data = json.loads(telemetry.read_text(encoding='utf-8'))
            except ValueError:
                pass
        rows.append(dict(scenario=name, run=run_index, external_ready_ms=first_ui,
                         internal_first_ui_ms=(telemetry_data.get('samples_ms', {}).get('first_ui_ms') or [None])[0],
                         ui_construction_ms=(telemetry_data.get('samples_ms', {}).get('ui_construction') or [None])[0],
                         settled_cpu_one_core_percent=cpu_percent,
                         working_set_mib=(rss or 0) / 1048576.0,
                         private_mib=(private or 0) / 1048576.0,
                         handles=handles,
                         exit_code=proc.returncode, shutdown_ms=shutdown_ms,
                         working_set_samples_mib=[s / 1048576.0 for s in samples]))
        (out / 'rows.json').write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
        print(f"{name}-{run_index}: ready={first_ui and round(first_ui)}ms "
              f"cpu={cpu_percent:.2f}% ws={rows[-1]['working_set_mib']:.0f}MiB "
              f"exit={proc.returncode} shutdown={shutdown_ms:.0f}ms", flush=True)
        if proc.returncode:
            raise SystemExit(f'{name}-{run_index} exited {proc.returncode}')
print(json.dumps({'rows': len(rows), 'out': str(out / 'rows.json')}))
