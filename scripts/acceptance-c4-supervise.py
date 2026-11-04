"""Supervise one explicitly selected native acceptance process without sending UI input."""
import argparse, datetime, hashlib, json, os, pathlib, subprocess, time
import psutil

parser = argparse.ArgumentParser()
parser.add_argument('--case', required=True)
parser.add_argument('--exe', type=pathlib.Path, required=True)
parser.add_argument('--timeout', type=float, default=1200)
parser.add_argument('arguments', nargs=argparse.REMAINDER)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
assert args.case.replace('-', '').replace('_', '').isalnum()
out = root / 'docs/phase-2c4/acceptance-final/processes' / args.case
out.mkdir(parents=True, exist_ok=False)
exe = args.exe.resolve(strict=True)
command = [str(exe), *[v for i, v in enumerate(args.arguments) if not (i == 0 and v == '--')]]
environment = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH'],
               'QT_FORCE_STDERR_LOGGING': '1', 'QT_ASSUME_STDERR_HAS_CONSOLE': '1',
               'QT_MESSAGE_PATTERN': '[%{time yyyy-MM-ddTHH:mm:ss.zzz} %{type} %{category}] %{message}'}
def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(record):
    temporary = out / 'supervisor.json.partial'
    temporary.write_text(json.dumps(record, indent=2), encoding='utf-8')
    deadline = time.monotonic() + 5
    while True:
        try:
            temporary.replace(out / 'supervisor.json')
            break
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)
record = {'case': args.case, 'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
          'source_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
          'arguments': command[1:], 'launch_utc': utc(), 'timeout_seconds': args.timeout,
          'pid': None, 'exit_utc': None, 'exit_code': None, 'timeout': False, 'classification': 'running',
          'windows': [], 'last_screenshot': None, 'children': [], 'samples': [], 'diagnostic_log': str(out / 'stderr.txt')}
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 1  # The user explicitly requested a visible native GUI test.
started = time.monotonic()
with (out / 'stdout.txt').open('wb') as stdout, (out / 'stderr.txt').open('wb') as stderr:
    child = subprocess.Popen(command, cwd=root, env=environment, stdout=stdout, stderr=stderr, startupinfo=startup)
    record['pid'] = child.pid
    process = psutil.Process(child.pid)
    save(record)
    while child.poll() is None:
        observations = out / 'observations.jsonl'
        if observations.exists():
            lines = observations.read_text(encoding='utf-8').splitlines()
            if lines:
                try:
                    latest = json.loads(lines[-1]); record['windows'] = latest.get('windows', [])
                    record['last_screenshot'] = latest.get('screenshots', [])
                    record['last_observation'] = latest
                except json.JSONDecodeError: pass
        try:
            record['children'] = [{'pid': p.pid, 'exe': p.exe()} for p in process.children(recursive=True)]
            cpu = process.cpu_times()
            record['samples'].append({'seconds': time.monotonic()-started, 'working_set': process.memory_info().rss, 'cpu_seconds': cpu.user+cpu.system})
        except (psutil.NoSuchProcess, psutil.AccessDenied): pass
        if time.monotonic()-started > args.timeout:
            record['timeout'] = True; child.kill(); break  # Only this owned child.
        save(record)
        time.sleep(1)
    record['exit_code'] = child.wait(timeout=15)
record['exit_utc'] = utc(); record['wall_seconds'] = time.monotonic()-started
code = record['exit_code']
record['classification'] = 'watchdog timeout' if record['timeout'] else 'normal successful exit' if code == 0 else 'application crash (NTSTATUS)' if code & 0x80000000 else 'application nonzero exit; cause requires diagnostics'
record['last_diagnostic_lines'] = (out / 'stderr.txt').read_text(errors='replace').splitlines()[-40:]
save(record)
print(json.dumps({k: record[k] for k in ('case', 'pid', 'exit_code', 'classification', 'wall_seconds')}), flush=True)
