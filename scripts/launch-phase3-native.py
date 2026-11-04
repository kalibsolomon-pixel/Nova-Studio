"""Launch one isolated native acceptance session; preserves fixture and settings."""
import argparse
import json
import os
import pathlib
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('build')
p.add_argument('session')
p.add_argument('--executable', type=pathlib.Path)
p.add_argument('--wait', action='store_true')
a = p.parse_args()
out = root / 'build/phase3-native' / a.session
out.mkdir(parents=True, exist_ok=False)
pack = out / 'pack'
shutil.copytree(root / 'fixtures/phase-2c5-models', pack,
                ignore=shutil.ignore_patterns('.nova'))
exe = a.executable.resolve() if a.executable else root / 'build' / a.build / 'src/nova-studio.exe'
cmd = [str(exe), '--project', str(pack), '--settings', str(out / 'studio.ini'),
       '--fresh-layout', '--telemetry', str(out / 'telemetry.json')]
env = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
if a.executable:
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('QT_', 'QML', 'QTDIR'))}
    env['PATH'] = os.pathsep.join([str(pathlib.Path(os.environ['SystemRoot'])/'System32'), os.environ['SystemRoot']])
with (out / 'process.log').open('w') as log:
    child = subprocess.Popen(cmd, cwd=out, env=env, stdout=log, stderr=log,
                             creationflags=subprocess.CREATE_NO_WINDOW)
(out / 'launch.json').write_text(json.dumps(dict(pid=child.pid, command=cmd), indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(pid=child.pid, session=str(out))), flush=True)
if a.wait:
    try:
        code = child.wait(timeout=900)
    except subprocess.TimeoutExpired:
        (out/'observation-timeout.json').write_text(json.dumps(dict(
            exit_code=None, process_left_running=True,
            reason='Observation window ended; no input, termination or close workaround used'))+'\n', encoding='utf-8')
        raise SystemExit('Observation timed out; application left untouched')
    (out/'exit.json').write_text(json.dumps(dict(exit_code=code))+'\n', encoding='utf-8')
    print('exit', code)
