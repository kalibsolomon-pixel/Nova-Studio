"""Launch only the newly prepared owned C8 offline demo instance (no input automation).

The game enters the copied demo world with --quickPlaySingleplayer, runs for a fixed
time, and is then asked to close normally (WM_CLOSE via taskkill without /F), which
Minecraft handles as save-and-quit. Logs, pack manifest, launch and exit are recorded.
"""
import json, pathlib, subprocess, sys, hashlib, re, time
from datetime import datetime, timezone
root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-2c8/runtime'
r = json.loads((out / 'instance.json').read_text())
instance = pathlib.Path(r['instance']).resolve()
assert instance == (root / 'build/studio-c8-runtime/minecraft-1.21.11').resolve()
args = r['args']
assert '--demo' in args and args[args.index('--accessToken') + 1] in ('0', 'none', 'null', '')
assert pathlib.Path(args[args.index('--gameDir') + 1]).resolve() == instance
label = sys.argv[1] if len(sys.argv) > 1 else 'session'
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 90
assert re.fullmatch('[a-z0-9-]+', label)
pack = instance / 'resourcepacks/Nova-C8'
(out / f'pack-manifest-{label}.json').write_text(json.dumps(
    [{'path': p.relative_to(pack).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
     for p in sorted(pack.rglob('*')) if p.is_file()], indent=2) + '\n')
command = [r['java'], *args, '--quickPlaySingleplayer', 'Demo_World']
with (out / f'stdout-{label}.txt').open('x') as stdout, (out / f'stderr-{label}.txt').open('x') as stderr:
    child = subprocess.Popen(command, cwd=instance, creationflags=subprocess.CREATE_NO_WINDOW, stdout=stdout, stderr=stderr)
    (out / f'launch-{label}.json').write_text(json.dumps(dict(pid=child.pid, utc=datetime.now(timezone.utc).isoformat(),
                                                              instance=str(instance), quick_play='Demo_World',
                                                              seconds=seconds), indent=2) + '\n')
    try:
        code = child.wait(timeout=seconds)
        closed = 'exited by itself'
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/PID', str(child.pid)], capture_output=True)  # normal close request
        try:
            code = child.wait(timeout=60)
            closed = 'closed after normal close request'
        except subprocess.TimeoutExpired:
            child.kill()
            code = child.wait()
            closed = 'forced after close request timed out'
    (out / f'exit-{label}.json').write_text(json.dumps(dict(exit_code=code, how=closed,
                                                            utc=datetime.now(timezone.utc).isoformat()), indent=2) + '\n')
    print('exit', code, closed)
