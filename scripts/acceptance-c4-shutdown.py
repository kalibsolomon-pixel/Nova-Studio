"""Bounded class-A teardown campaign; uses isolated settings and owned fixture copies."""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, time

parser = argparse.ArgumentParser()
parser.add_argument('--label', default='shutdown')
args = parser.parse_args()

root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-2c4/acceptance' / args.label
out.mkdir(exist_ok=False)
work = root / 'build/studio-c4-acceptance' / args.label
work.mkdir(exist_ok=False)
env = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
probe = root / 'build/studio-c4-acceptance-release/tests/nova-animation-acceptance-probe.exe'
exe = probe.parent.parent / 'src/nova-studio.exe'
rows = []
def run(label, command, timeout=30):
    start = time.perf_counter()
    child = subprocess.Popen(list(map(str, command)), cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    timed_out = False
    try:
        log, _ = child.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        child.kill()  # Only the child created in this call, never an existing user process.
        log, _ = child.communicate(timeout=10)
    (out / f'{label}.txt').write_bytes(log)
    row = {'label': label, 'pid': child.pid, 'exit': child.returncode, 'timeout': timed_out, 'wall_ms': (time.perf_counter()-start)*1000}
    rows.append(row)
    (out / 'summary.json').write_text(json.dumps({'evidence_class': 'A: programmatic application lifecycle, not native pointer acceptance', 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'runs': rows}, indent=2))
    print(json.dumps(row), flush=True)

scenarios = ('empty', 'static', 'animated', 'playback', 'scrolling', 'onion', 'native-save', 'export-cancel', 'frame-operation', 'floating', 'indexing')
for repeat in range(2):
    for scenario in scenarios:
        label = f'{scenario}-{repeat}'
        fixture = work / label
        # Journals bind absolute destinations; copying them would intentionally trigger
        # recovery validation against a different path before the shutdown case starts.
        shutil.copytree(root / 'docs/phase-2c4/acceptance/fixtures', fixture, ignore=shutil.ignore_patterns('animation-exports'))
        run(label, [probe, '--shutdown', scenario, fixture, fixture / 'settings.ini', out / f'{label}.json'])
for repeat in range(3):
    for scenario in ('empty', 'static', 'animated'):
        label = f'production-{scenario}-{repeat}'
        args = [exe, '--settings', work / f'{label}.ini', '--fresh-layout', '--smoke-seconds', '2', '--telemetry', out / f'{label}.json']
        if scenario != 'empty':
            fixture = work / f'production-fixture-{scenario}-{repeat}'
            shutil.copytree(root / 'docs/phase-2c4/acceptance/fixtures' / ('static' if scenario == 'static' else 'sources/A'), fixture, ignore=shutil.ignore_patterns('animation-exports'))
            args += ['--project', fixture]
        run(label, args)
raise SystemExit(any(row['timeout'] or row['exit'] for row in rows))
