"""Quiet five-minute idle comparison: Phase 2-C3 Release vs a control executable.

Usage: idle-phase2c3.py <c3-exe> <control-exe>. Launches each executable with the
1,500-asset fixture and with no project, samples CPU/memory every five seconds for
320 seconds and reports CPU over the steady window (60 s to the end). Do not run
other heavy work concurrently.
"""
from pathlib import Path
import json, os, statistics, subprocess, sys, time
import psutil

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs/phase-2c3/performance' / ('idle-quiet-' + str(time.time_ns()))
OUT.mkdir(parents=True)
ENV = {**os.environ, 'PATH': str(ROOT / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
c3, control = Path(sys.argv[1]), Path(sys.argv[2])
variants = [('c3-small', c3, 'pack-1500'), ('c3-empty', c3, None),
            ('control-small', control, 'pack-1500'), ('control-empty', control, None)]
processes, obs = {}, {}
for name, exe, pack in variants:
    args = [str(exe), '--fresh-layout', '--settings', str(OUT / f'{name}.ini'), '--smoke-seconds', '330']
    if pack:
        args += ['--project', str(ROOT / 'fixtures/generated' / pack)]
    p = subprocess.Popen(args, env=ENV)
    processes[name] = (p, psutil.Process(p.pid))
    obs[name] = []
start = time.perf_counter()
for _ in range(64):
    time.sleep(5)
    for name, (_, proc) in processes.items():
        m, c = proc.memory_info(), proc.cpu_times()
        obs[name].append({'seconds': time.perf_counter() - start, 'working_set_mib': m.rss / 2**20,
                          'private_mib': m.private / 2**20, 'cpu_seconds': c.user + c.system,
                          'threads': proc.num_threads(), 'handles': proc.num_handles()})
(OUT / 'idle-raw.json').write_text(json.dumps(obs, indent=2))
summary = {}
for name, values in obs.items():
    a = next(v for v in values if v['seconds'] >= 60)
    b = values[-1]
    summary[name] = {
        'cpu_percent_one_processor_60s_to_end': 100 * (b['cpu_seconds'] - a['cpu_seconds']) / (b['seconds'] - a['seconds']),
        'working_set_mib_last_four_median': statistics.median(v['working_set_mib'] for v in values[-4:]),
        'private_mib_last_four_median': statistics.median(v['private_mib'] for v in values[-4:]),
        'threads': b['threads'], 'handles': b['handles']}
(OUT / 'summary.json').write_text(json.dumps(summary, indent=2))
for p, _ in processes.values():
    assert p.wait(timeout=30) == 0
print(OUT)
