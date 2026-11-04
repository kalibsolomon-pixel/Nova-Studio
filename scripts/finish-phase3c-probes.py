"""Run supplementary retained probes only after the main controlled matrix exits."""
import json
import pathlib
import shutil
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
steps = root / 'docs/phase-3c/benchmarks-steps.json'
driver = root / 'build/phase3c-final-benchmarks-driver.log'
deadline = time.monotonic() + 7200
while True:
    rows = json.loads(steps.read_text()) if steps.exists() else []
    if any(r['exit_code'] for r in rows) or (driver.exists() and 'Traceback' in driver.read_text(errors='replace')):
        raise SystemExit('Main matrix failed; supplementary measurements were not started')
    if rows and rows[-1]['step'] == 'idle-final': break
    if time.monotonic() >= deadline: raise SystemExit('Main matrix did not complete within the observation limit')
    time.sleep(10)

def run(label, command):
    print('START', label, flush=True)
    with (root / 'build' / ('phase3c-' + label + '.log')).open('xb') as log:
        result = subprocess.run(command, cwd=root, stdout=log, stderr=log, timeout=1200)
    print('END', label, result.returncode, flush=True)
    if result.returncode: raise SystemExit(result.returncode)

for build in ('studio-phase3c-baseline-release', 'studio-phase3c-release'):
    run(build + '-shutdown-build', [sys.executable, root / 'scripts/build-phase3c.py', build, '--shutdown-probe'])
run('shutdown-measurements', [sys.executable, root / 'scripts/profile-phase3c-shutdown.py'])
retained = root / 'build/phase3c-retained-memory-probe'
retained.mkdir(exist_ok=False)
shutil.copy2(root / 'build/studio-phase3c-release/tests/nova-c8-memory-probe.exe', retained / 'nova-c8-memory-probe.exe')
run('memory-bootstrap-build', [sys.executable, root / 'scripts/build-phase3c.py',
    'studio-phase3c-release', '--probe-only', 'nova-c8-memory-probe'])
run('memory-bootstrap-measurements', [sys.executable, root / 'scripts/profile-phase3c-memory-bootstrap.py'])
shutdown = json.loads((root / 'docs/phase-3c/final-shutdown/rows.json').read_text())
bootstrap = json.loads((root / 'docs/phase-3c/memory-bootstrap/runs.json').read_text())
assert len(shutdown) == 42 and len(bootstrap) == 15
assert all(row['exit_code'] == 0 for row in shutdown + bootstrap)
(root / 'docs/phase-3c/supplementary-probes.json').write_text(json.dumps(dict(exit_code=0,
    shutdown_runs=len(shutdown), memory_bootstrap_runs=len(bootstrap),
    main_matrix_complete_before_builds=True), indent=2)+'\n')
