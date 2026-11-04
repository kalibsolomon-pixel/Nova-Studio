"""Phase 4-G Workstream E: repeat the animation-gui suite and keep every log.

Modes (each run keeps its full QtTest output and exit status):
  isolated   animation-gui alone
  parallel   animation-gui while other GUI suites (mcp, editor-gui, layer-gui, model-gui,
             phase4f-lifecycle) run continuously beside it, as under `ctest -j`
  pair       two animation-gui processes at once
  cpu        animation-gui while busy loops occupy all but one logical processor
Writes build/phase4g-evidence/animation-gui/<mode>/run-NNN.txt and summary.json.
"""
import argparse
import json
import multiprocessing
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', default='studio-phase4g-Release')
p.add_argument('--mode', choices=['isolated', 'parallel', 'pair', 'cpu'], required=True)
p.add_argument('--runs', type=int, default=20)
p.add_argument('--out', default='build/phase4g-evidence/animation-gui')
p.add_argument('--function', help='run only this QtTest function of animation-gui')
p.add_argument('--tests-dir', help='a frozen copy of the test executables (default: the build tree)')
a = p.parse_args()
tests = pathlib.Path(a.tests_dir) if a.tests_dir else root / 'build' / a.build / 'tests'
out = root / a.out / a.mode
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
env['PATH'] = str(root / '.tools/qt/bin') + os.pathsep + env['PATH']
venv = root / 'build/phase4c-venv/Scripts/python.exe'
if venv.is_file():
    env.setdefault('NOVA_MCP_SDK_PYTHON', str(venv))


def run_suite(exe, log, extra_env=None):
    run_env = dict(env, **(extra_env or {}))
    with tempfile.TemporaryDirectory(prefix='nova-4g-') as scratch:
        run_env['NOVA_MCP_HANDOFF_DIR'] = str(pathlib.Path(scratch) / 'handoff')
        run_env['NOVA_C4_EVIDENCE_DIR'] = str(pathlib.Path(scratch) / 'evidence')
        started = time.perf_counter()
        try:
            selected = [a.function] if a.function and exe == 'nova-animation-gui-tests.exe' else []
            code = subprocess.call([str(tests / exe), *selected, '-o', f'{log},txt'], cwd=tests, env=run_env, timeout=600)
        except subprocess.TimeoutExpired:
            code = 'timeout'
        return code, time.perf_counter() - started


stop = threading.Event()
load_counts = {}


def load_loop(exe):
    n = 0
    while not stop.is_set():
        log = out / f'load-{exe}-{n:03d}.txt'
        code, _ = run_suite(exe, log)
        load_counts.setdefault(exe, []).append(code)
        if code == 0:
            log.unlink(missing_ok=True)  # keep only failing load logs
        n += 1


def spin():
    while True:
        pass


helpers = []
if a.mode == 'parallel':
    for exe in ('nova-mcp-tests.exe', 'nova-editor-gui-tests.exe', 'nova-layer-gui-tests.exe',
                'nova-model-gui-tests.exe', 'nova-phase4f-lifecycle-probe.exe'):
        if exe == 'nova-phase4f-lifecycle-probe.exe':
            continue  # needs arguments; mcp and the GUI suites provide the concurrent load
        t = threading.Thread(target=load_loop, args=(exe,), daemon=True)
        t.start()
        helpers.append(t)
elif a.mode == 'cpu':
    hogs = [multiprocessing.Process(target=spin, daemon=True) for _ in range(max(1, os.cpu_count() - 1))]
    for hog in hogs:
        hog.start()

results = []
for i in range(a.runs):
    log = out / f'run-{i:03d}.txt'
    if a.mode == 'pair':
        second = out / f'run-{i:03d}-b.txt'
        box = {}
        t = threading.Thread(target=lambda: box.update(b=run_suite('nova-animation-gui-tests.exe', second)))
        t.start()
        code, seconds = run_suite('nova-animation-gui-tests.exe', log)
        t.join()
        results.append(dict(run=i, exit=code, seconds=round(seconds, 2)))
        results.append(dict(run=f'{i}b', exit=box['b'][0], seconds=round(box['b'][1], 2)))
    else:
        code, seconds = run_suite('nova-animation-gui-tests.exe', log)
        results.append(dict(run=i, exit=code, seconds=round(seconds, 2)))
    print(results[-1], flush=True)

stop.set()
if a.mode == 'cpu':
    for hog in hogs:
        hog.terminate()
for t in helpers:
    t.join(timeout=700)
failures = [r for r in results if r['exit'] != 0]
summary = dict(mode=a.mode, runs=len(results), failures=failures,
               durations=sorted(r['seconds'] for r in results),
               load={k: dict(runs=len(v), failures=sum(1 for c in v if c != 0)) for k, v in load_counts.items()},
               logical_processors=os.cpu_count(), build=a.build)
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(mode=a.mode, runs=len(results), failures=len(failures), load=summary['load'])))
sys.exit(0)
