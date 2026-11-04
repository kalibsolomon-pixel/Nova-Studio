"""Repeatable Phase 2-C3 evidence capture against the final Release build.

Run after scripts/build-studio.ps1 -Configuration Release -BuildName studio-c3-closeout-release.
Usage: python scripts/validate-phase2c3-closeout.py {tests|bench} OUTPUT_DIR
OUTPUT_DIR must be new. Tests preserve all 13 generated CTest commands, adding Qt
text/XML output files only. No production source or historical evidence is changed.
"""
from pathlib import Path
import hashlib
import json
import math
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/studio-c3-closeout-release'
OUT = Path(sys.argv[2]).resolve()
OUT.mkdir(parents=True, exist_ok=False)
ENV = {**os.environ, 'PATH': str(ROOT / '.tools/qt/bin') + os.pathsep + os.environ['PATH'],
       'NOVA_LARGE_FIXTURE': str(ROOT / 'fixtures/generated/pack-50000'),
       'NOVA_EVIDENCE_DIR': str(BUILD / 'logged-screenshots')}
EXE = BUILD / 'src/nova-studio.exe'
def write(name, value):
    (OUT / name).write_text(json.dumps(value, indent=2) + '\n')
def run(command, timeout=120):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, env=ENV, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{command}: {result.returncode}\n{result.stdout!r}\n{result.stderr!r}')
    return result
def stats(values):
    values = sorted(values)
    return {'n': len(values), 'p50': statistics.median(values), 'p95': values[math.ceil(.95 * len(values)) - 1]}
write('identity.json', {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                        'configuration': 'Release', 'executable_sha256': hashlib.sha256(EXE.read_bytes()).hexdigest(),
                        'environment_overrides': {k: ENV[k] for k in ('NOVA_LARGE_FIXTURE', 'NOVA_EVIDENCE_DIR')}})
if sys.argv[1] == 'tests':
    generated = (BUILD / 'tests/CTestTestfile.cmake').read_text()
    tests = re.findall(r'add_test\(\[=\[(.*?)\]=\] "(.*?)"\)', generated)
    assert len(tests) == 13, tests
    # A new output directory gets its own logging driver, preserving previous runs.
    driver = OUT / 'ctest-driver'
    driver.mkdir(exist_ok=False)
    lines = ['# Same Release test executables, with explicit Qt logging.']
    for name, exe in tests:
        lines.append(f'add_test("{name}" "{exe}" "-o" "{(OUT / name).as_posix()}.txt,txt" "-o" "{(OUT / name).as_posix()}.xml,xunitxml")')
        lines.append(f'set_tests_properties("{name}" PROPERTIES WORKING_DIRECTORY "{(BUILD / "tests").as_posix()}" TIMEOUT 120)')
    (driver / 'CTestTestfile.cmake').write_text('\n'.join(lines) + '\n')
    cmake = re.search(r'CMAKE_COMMAND:INTERNAL=(.*)', (BUILD / 'CMakeCache.txt').read_text())[1]
    command = [Path(cmake).with_name('ctest.exe'), '--test-dir', driver, '--output-on-failure',
               '--output-junit', OUT / 'ctest-junit.xml', '--output-log', OUT / 'ctest.txt']
    before = time.perf_counter()
    result = run(command, timeout=600)
    (OUT / 'ctest-console.txt').write_bytes(result.stdout + result.stderr)
    rows = []
    for name, exe in tests:
        text = (OUT / f'{name}.txt').read_text()
        match = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted', text)
        assert match, name
        rows.append({'suite': name, **dict(zip(('passed', 'failed', 'skipped', 'blacklisted'), map(int, match.groups())))})
    write('summary.json', {'suites': rows, 'suite_count': len(rows), 'seconds': time.perf_counter() - before,
                           'totals_including_init_cleanup': {k: sum(r[k] for r in rows) for k in ('passed', 'failed', 'skipped', 'blacklisted')}})
    print((OUT / 'summary.json').read_text())
elif sys.argv[1] == 'bench':
    run([BUILD / 'tests/nova-editor-bench.exe', OUT / 'editor-bench-release.json'], timeout=300)
    for size in (1500, 50000):
        run([EXE, '--bench', ROOT / f'fixtures/generated/pack-{size}', '--output', OUT / f'core-{size}.json'])
    startup = []
    for i in range(5):
        # Place mutable settings outside the committed evidence folder.
        settings = BUILD / f'benchmark-start-{i}/settings.ini'
        trace = OUT / f'startup-{i}.json'
        run([EXE, '--project', ROOT / 'fixtures/generated/pack-1500', '--fresh-layout', '--settings', settings,
             '--telemetry', trace, '--smoke-seconds', '2'])
        data = json.loads(trace.read_text())
        startup.append({k: v[0] for k, v in data['samples_ms'].items() if k in ('ui_construction', 'first_ui_ms', 'ready_capture_ms')})
    run([EXE, '--fresh-layout', '--settings', BUILD / 'benchmark-cycle/settings.ini', '--telemetry', OUT / 'cycle.json',
         '--cycle-project', ROOT / 'fixtures/generated/pack-50000', '--layer-cycle'])
    write('summary.json', {'startup_ms': {k: stats([r[k] for r in startup]) for k in startup[0]},
                           'core': {str(size): {k: stats(v) for k, v in json.loads((OUT / f'core-{size}.json').read_text())['samples_ms'].items()}
                                    for size in (1500, 50000)}})
    print((OUT / 'summary.json').read_text())
else:
    raise ValueError('Expected tests or bench')
