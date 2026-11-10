# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-I.1: native acceptance and performance comparison, GCC/MinGW-w64 build vs MSVC build.

Both builds run the same workloads on the same machine, interleaved, from COPIES of their release
payloads (the MSVC release candidate is only read), with an isolated profile per launch
(LOCALAPPDATA, APPDATA, TEMP, USERPROFILE, CLAUDE_CONFIG_DIR, CODEX_HOME and --settings inside the
run folder) and only Windows folders on PATH. Fixture packs are copied; nothing outside
build/phase4i1-evidence/<run> is written.

Parts (default all):
  native  scripted UI walk-through (--qa-dir) with screenshots at Qt scale 1, 1.5 and 2, GL context
          check, layout save/restore round trip, missing-project handling, GCC-vs-MSVC pixel comparison
  startup external readiness, settled CPU, working set / private bytes / handles, WM_CLOSE shutdown,
          for an empty start, a 10k-asset pack and a 50k-asset pack
  bench   nova-studio --bench (indexing, PNG decode/resize, document operations, saves), and the
          editor, animation, model and C8 benchmark programs
  frames  --exercise (canvas and 3D preview repaint pacing at 60/120/180 Hz requests)
  mcp     request latency through nova-mcp.exe with the official MCP SDK after Allow read-only

Usage: python scripts/phase4i1-compare.py --gcc <candidate label> --msvc-payload <dir>
       --msvc-tests <MSVC build tests dir> --msvc-qt-bin <MSVC Qt bin> --fixtures <fixtures/generated>
       --sdk-python <python with the MCP SDK> [--runs 5] [--parts native,startup,bench,frames,mcp]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import json
import os
import pathlib
import shutil
import statistics
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--gcc', required=True, help='candidate label under build/phase4i-distribution')
p.add_argument('--gcc-build', default='mingw-ucrt64-source-Release')
p.add_argument('--msvc-payload', required=True, type=pathlib.Path)
p.add_argument('--msvc-tests', required=True, type=pathlib.Path)
p.add_argument('--msvc-qt-bin', required=True, type=pathlib.Path)
p.add_argument('--fixtures', required=True, type=pathlib.Path)
p.add_argument('--sdk-python', required=True, type=pathlib.Path)
p.add_argument('--runs', type=int, default=5)
p.add_argument('--parts', default='native,startup,bench,frames,mcp')
a = p.parse_args()
parts = set(a.parts.split(','))
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
out = root / 'build/phase4i1-evidence' / stamp
out.mkdir(parents=True)
log = open(out / 'run.log', 'w', encoding='utf-8')
system_root = os.environ['SystemRoot']
WINDOWS_PATH = os.pathsep.join([f'{system_root}\\System32', system_root, f'{system_root}\\System32\\Wbem'])
gcc_qt = pathlib.Path(os.environ.get('NOVA_MINGW_QT', r'C:\Dev\nova-toolchains\qt-6.12.0-ucrt64-gcc16'))
gcc_bin = pathlib.Path(os.environ.get('NOVA_MSYS2_ROOT', r'C:\Dev\nova-toolchains\msys64-20260927')) / 'ucrt64/bin'
BENCHES = ['nova-editor-bench.exe', 'nova-animation-bench.exe', 'nova-model-bench.exe', 'nova-c8-bench.exe']


def say(text):
    print(text, flush=True)
    log.write(text + '\n')
    log.flush()


def save(name, data):
    (out / name).write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


# ---------------------------------------------------------------- set-up (copies only)
builds = {
    'gcc': dict(payload=root / 'build/phase4i-distribution' / a.gcc / 'payload/Nova Studio',
                tests=root / 'build' / a.gcc_build / 'tests', dlls=[gcc_qt / 'bin', gcc_bin], plugins=gcc_qt / 'plugins'),
    'msvc': dict(payload=a.msvc_payload, tests=a.msvc_tests, dlls=[a.msvc_qt_bin], plugins=a.msvc_qt_bin.parent / 'plugins'),
}
for name, b in builds.items():
    if not (b['payload'] / 'nova-studio.exe').is_file():
        raise SystemExit(f'{name}: no nova-studio.exe in {b["payload"]}')
    b['app'] = out / name / 'app'
    shutil.copytree(b['payload'], b['app'])
    b['bench'] = out / name / 'bench'
    b['bench'].mkdir(parents=True)
    for exe in BENCHES:
        shutil.copy2(b['tests'] / exe, b['bench'] / exe)
    manifest = json.loads((b['app'] / 'package-manifest.json').read_text(encoding='utf-8'))
    b['identity'] = dict(revision=manifest['source_revision'], compiler=manifest.get('compiler'),
                         toolchain=manifest.get('toolchain_family', 'msvc'))
fixtures = out / 'fixtures'
for pack in ('pack-1500', 'pack-10000', 'pack-50000'):
    shutil.copytree(a.fixtures / pack, fixtures / pack, ignore=shutil.ignore_patterns('.nova'))
shutil.copytree(root / 'fixtures/phase-2c5-models', fixtures / 'models', ignore=shutil.ignore_patterns('.nova'))
# One project copy per build, so layout and index state never cross between builds.
for name, b in builds.items():
    for pack in ('pack-10000', 'pack-50000'):
        shutil.copytree(fixtures / pack, out / name / 'projects' / pack)
cpu_name = subprocess.run(['powershell', '-NoProfile', '-Command', '(Get-CimInstance Win32_Processor).Name;'
                           '(Get-CimInstance Win32_VideoController).Name;(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory'],
                          capture_output=True, text=True).stdout.split('\n')
report = dict(format='nova-phase4i1-comparison', run=stamp, runs=a.runs,
              machine=dict(cpu=cpu_name[0].strip(), gpu=cpu_name[1].strip(), ram_bytes=int(cpu_name[2].strip() or 0),
                           logical_processors=os.cpu_count(), windows=sys.getwindowsversion()._asdict() if hasattr(sys.getwindowsversion(), '_asdict') else str(sys.getwindowsversion())),
              builds={k: v['identity'] for k, v in builds.items()})
save('report.json', report)

# ---------------------------------------------------------------- Win32 helpers
user32, kernel32, psapi = ctypes.windll.user32, ctypes.windll.kernel32, ctypes.windll.psapi
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.GetProcessTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
kernel32.GetProcessHandleCount.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


class PMC(ctypes.Structure):
    _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t), ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPagedPoolUsage', ctypes.c_size_t), ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                ('QuotaNonPagedPoolUsage', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                ('PeakPagefileUsage', ctypes.c_size_t), ('PrivateUsage', ctypes.c_size_t)]


psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]


class Proc:
    def __init__(self, pid):
        self.handle = kernel32.OpenProcess(0x1000 | 0x0010, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ

    def cpu_seconds(self):
        times = [wt.FILETIME() for _ in range(4)]
        kernel32.GetProcessTimes(self.handle, *[ctypes.byref(t) for t in times])
        return sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in times[2:]) / 1e7

    def memory(self):
        counters = PMC()
        counters.cb = ctypes.sizeof(PMC)
        psapi.GetProcessMemoryInfo(self.handle, ctypes.byref(counters), counters.cb)
        handles = wt.DWORD()
        kernel32.GetProcessHandleCount(self.handle, ctypes.byref(handles))
        return counters.WorkingSetSize / 1048576, counters.PrivateUsage / 1048576, handles.value

    def close(self):
        kernel32.CloseHandle(self.handle)


def windows_of(pid, title=None):
    found = []

    def callback(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buffer = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buffer, 512)
            if title is None or title in buffer.value:
                found.append((hwnd, buffer.value))
        return True
    user32.EnumWindows(WNDENUMPROC(callback), 0)
    return found


def wait_window(pid, title, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = windows_of(pid, title)
        if found:
            return found[0][0]
        time.sleep(0.01)
    return None


def isolated(name, tag, extra=None):
    iso = out / name / 'iso' / tag
    for d in ('localappdata', 'appdata', 'temp', 'profile', 'claude', 'codex', 'settings'):
        (iso / d).mkdir(parents=True, exist_ok=True)
    env = {k: os.environ[k] for k in ('SystemRoot', 'windir', 'SystemDrive', 'COMSPEC', 'PATHEXT', 'OS',
                                       'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'USERNAME', 'USERDOMAIN')
           if k in os.environ}
    env.update(PATH=WINDOWS_PATH, LOCALAPPDATA=str(iso / 'localappdata'), APPDATA=str(iso / 'appdata'),
               TEMP=str(iso / 'temp'), TMP=str(iso / 'temp'), USERPROFILE=str(iso / 'profile'),
               CLAUDE_CONFIG_DIR=str(iso / 'claude'), CODEX_HOME=str(iso / 'codex'))
    env.update(extra or {})
    return iso, env


def read_json(path):
    try:
        return json.loads(pathlib.Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def order(run):
    return ['gcc', 'msvc'] if run % 2 == 0 else ['msvc', 'gcc']


def close_window(proc, hwnd, timeout=60):
    begin = time.perf_counter()
    user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    try:
        code = proc.wait(timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = 'timeout'
    return code, (time.perf_counter() - begin) * 1000


# ---------------------------------------------------------------- native acceptance
if 'native' in parts:
    native = {}
    for name, b in builds.items():
        result = native[name] = dict(qa={}, layout=None, missing_project=None)
        for scale in ('1', '1.5', '2'):
            project = out / name / 'native' / f'qa-{scale}' / 'pack'
            shutil.copytree(fixtures / 'pack-1500', project)
            shots = out / name / 'native' / f'qa-{scale}' / 'screenshots'
            telemetry = out / name / 'native' / f'qa-{scale}' / 'telemetry.json'
            iso, env = isolated(name, f'qa-{scale}', {'QT_SCALE_FACTOR': scale})
            proc = subprocess.run([str(b['app'] / 'nova-studio.exe'), '--project', str(project), '--qa-dir', str(shots),
                                   '--telemetry', str(telemetry), '--settings', str(iso / 'settings/studio.ini')],
                                  cwd=str(b['app']), env=env, timeout=120, capture_output=True)
            data = read_json(telemetry)
            pngs = sorted(f.name for f in shots.glob('*.png')) if shots.exists() else []
            result['qa'][scale] = dict(exit_code=proc.returncode, screenshots=pngs, gpu=data.get('environment', {}).get('gpu'),
                                       gl=data.get('environment', {}).get('backend'),
                                       native_float_confirmed=data.get('counters', {}).get('native_float_confirmed'),
                                       stderr_tail=proc.stderr.decode('utf-8', 'replace')[-400:])
            say(f'native {name} qa scale {scale}: exit {proc.returncode}, {len(pngs)} screenshots, GL {result["qa"][scale]["gl"]}')
        # Layout round trip and missing-project handling.
        project = out / name / 'native' / 'layout' / 'pack'
        shutil.copytree(fixtures / 'pack-1500', project)
        traces = []
        for i in range(2):
            iso, env = isolated(name, f'layout-{i}')
            settings = out / name / 'native' / 'layout' / 'studio.ini'
            trace = out / name / 'native' / 'layout' / f'telemetry-{i}.json'
            proc = subprocess.run([str(b['app'] / 'nova-studio.exe'), '--project', str(project), '--telemetry', str(trace),
                                   '--smoke-seconds', '2.5', '--settings', str(settings)], cwd=str(b['app']), env=env,
                                  timeout=60, capture_output=True)
            traces.append(dict(exit_code=proc.returncode, restored=read_json(trace).get('counters', {}).get('layout_restored', 0)))
        # The layout is kept with the settings (earlier phases kept it in <project>/.nova/qt-layout.ini).
        result['layout'] = dict(launches=traces, ok=[t['exit_code'] for t in traces] == [0, 0] and
                                traces[0]['restored'] == 0 and traces[1]['restored'] == 1)
        missing = out / name / 'native' / 'missing-project'
        iso, env = isolated(name, 'missing')
        proc = subprocess.run([str(b['app'] / 'nova-studio.exe'), '--project', str(missing), '--smoke-seconds', '1.5',
                               '--settings', str(iso / 'settings/studio.ini')], cwd=str(b['app']), env=env, timeout=60,
                              capture_output=True)
        result['missing_project'] = dict(exit_code=proc.returncode, created=missing.exists(),
                                         ok=proc.returncode == 0 and not missing.exists())
        say(f'native {name}: layout round trip {result["layout"]["ok"]}, missing project handled {result["missing_project"]["ok"]}')
    # GCC vs MSVC screenshots.
    try:
        from PIL import Image, ImageChops
        comparison = {}
        for scale in ('1', '1.5', '2'):
            for png in native['gcc']['qa'][scale]['screenshots']:
                g = Image.open(out / 'gcc/native' / f'qa-{scale}' / 'screenshots' / png).convert('RGBA')
                m_path = out / 'msvc/native' / f'qa-{scale}' / 'screenshots' / png
                if not m_path.exists():
                    comparison[f'{scale}/{png}'] = dict(msvc_missing=True)
                    continue
                m = Image.open(m_path).convert('RGBA')
                if g.size != m.size:
                    comparison[f'{scale}/{png}'] = dict(size_gcc=g.size, size_msvc=m.size)
                    continue
                diff = ImageChops.difference(g, m).convert('L')
                histogram = diff.histogram()
                total = g.size[0] * g.size[1]
                comparison[f'{scale}/{png}'] = dict(size=g.size, identical=diff.getbbox() is None,
                                                    pixels_differing_over_8=sum(histogram[9:]) / total,
                                                    mean_abs_difference=sum(i * c for i, c in enumerate(histogram)) / total)
        native['screenshot_comparison'] = comparison
        say(f'screenshots: {sum(1 for c in comparison.values() if c.get("identical"))}/{len(comparison)} pixel-identical')
    except ImportError:
        native['screenshot_comparison'] = 'PIL not available'
    report['native'] = native
    save('report.json', report)

# ---------------------------------------------------------------- startup, idle, shutdown
if 'startup' in parts:
    rows = []
    for run in range(a.runs):
        for name in order(run):
            b = builds[name]
            for scenario in (('empty', '50k', '10k') if run % 2 else ('10k', '50k', 'empty')):
                project = {'empty': None, '10k': out / name / 'projects/pack-10000', '50k': out / name / 'projects/pack-50000'}[scenario]
                iso, env = isolated(name, f'startup-{scenario}-{run}')
                telemetry = iso / 'telemetry.json'
                args = [str(b['app'] / 'nova-studio.exe'), '--telemetry', str(telemetry), '--settings', str(iso / 'settings/studio.ini')]
                if project:
                    args += ['--project', str(project)]
                started = time.perf_counter()
                proc = subprocess.Popen(args, cwd=str(b['app']), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                info = Proc(proc.pid)
                hwnd = wait_window(proc.pid, 'Nova Studio')
                window_ms = (time.perf_counter() - started) * 1000 if hwnd else None
                ready_ms = None
                if project:
                    # Telemetry is flushed when the 3D preview swaps its first frame with a document open.
                    while time.perf_counter() - started < 60 and proc.poll() is None:
                        if read_json(telemetry).get('samples_ms', {}).get('ready_capture_ms'):
                            ready_ms = (time.perf_counter() - started) * 1000
                            break
                        time.sleep(0.002)
                ws_ready, private_ready, _ = info.memory()
                time.sleep(10)  # past indexing and start-up churn
                cpu0, t0 = info.cpu_seconds(), time.perf_counter()
                samples = []
                for _ in range(15):
                    time.sleep(1)
                    samples.append(info.memory())
                cpu_percent = 100 * (info.cpu_seconds() - cpu0) / (time.perf_counter() - t0)
                code, shutdown_ms = close_window(proc, hwnd) if hwnd else ('no window', None)
                info.close()
                data = read_json(telemetry).get('samples_ms', {})
                rows.append(dict(build=name, scenario=scenario, run=run, window_ms=window_ms, external_ready_ms=ready_ms,
                                 internal_ready_ms=(data.get('ready_capture_ms') or [None])[0],
                                 first_ui_ms=(data.get('first_ui_ms') or [None])[0],
                                 ws_at_ready_mib=ws_ready, private_at_ready_mib=private_ready,
                                 idle_cpu_one_core_percent=cpu_percent, idle_ws_mib=samples[-1][0],
                                 idle_private_mib=samples[-1][1], handles=samples[-1][2], exit_code=code, shutdown_ms=shutdown_ms))
                say(f'startup {name} {scenario} run {run}: window {window_ms and round(window_ms)} ms, ready {ready_ms and round(ready_ms)} ms, '
                    f'idle cpu {cpu_percent:.2f}%, ws {samples[-1][0]:.0f} MiB, private {samples[-1][1]:.0f} MiB, exit {code}, '
                    f'shutdown {shutdown_ms and round(shutdown_ms)} ms')
                save('startup-rows.json', rows)
    summary = {}
    for name in builds:
        for scenario in ('empty', '10k', '50k'):
            mine = [r for r in rows if r['build'] == name and r['scenario'] == scenario]
            warm = [r for r in mine if r['run'] > 0]
            summary[f'{name}/{scenario}'] = {k: median([r[k] for r in warm]) for k in
                                             ('window_ms', 'external_ready_ms', 'internal_ready_ms', 'ws_at_ready_mib',
                                              'idle_cpu_one_core_percent', 'idle_ws_mib', 'idle_private_mib', 'handles', 'shutdown_ms')}
            summary[f'{name}/{scenario}']['first_run'] = {k: mine[0][k] for k in ('window_ms', 'external_ready_ms')} if mine else None
            summary[f'{name}/{scenario}']['exit_codes'] = sorted({str(r['exit_code']) for r in mine})
    report['startup'] = summary
    save('report.json', report)

# ---------------------------------------------------------------- CPU benchmarks
if 'bench' in parts:
    bench = {name: {} for name in builds}
    for run in range(min(a.runs, 3)):
        for name in order(run):
            b = builds[name]
            iso, env = isolated(name, f'bench-{run}')
            env_bench = dict(env, PATH=os.pathsep.join([str(d) for d in b['dlls']] + [WINDOWS_PATH]), QT_PLUGIN_PATH=str(b['plugins']))
            jobs = [('app-bench-10k', [str(b['app'] / 'nova-studio.exe'), '--bench', str(fixtures / 'pack-10000'), '--output', '{out}'], env),
                    ('app-bench-50k', [str(b['app'] / 'nova-studio.exe'), '--bench', str(fixtures / 'pack-50000'), '--output', '{out}'], env),
                    ('editor-bench', [str(b['bench'] / 'nova-editor-bench.exe'), '{out}'], env_bench),
                    ('animation-bench', [str(b['bench'] / 'nova-animation-bench.exe'), '{out}'], env_bench),
                    ('model-bench', [str(b['bench'] / 'nova-model-bench.exe'), str(fixtures / 'models'), '{out}'], env_bench),
                    ('c8-bench', [str(b['bench'] / 'nova-c8-bench.exe'), '{out}'], env_bench)]
            for job, command, job_env in jobs:
                target = out / name / 'bench' / f'{job}-{run}.json'
                begin = time.perf_counter()
                proc = subprocess.run([c.replace('{out}', str(target)) for c in command], cwd=str(iso), env=job_env,
                                      capture_output=True, timeout=1800)
                wall = time.perf_counter() - begin
                bench[name].setdefault(job, []).append(dict(exit_code=proc.returncode, wall_s=wall, result=read_json(target),
                                                            stderr_tail=proc.stderr.decode('utf-8', 'replace')[-300:]))
                say(f'bench {name} {job} run {run}: exit {proc.returncode}, {wall:.1f} s')
            save('bench-raw.json', bench)
    report['bench_wall_seconds'] = {f'{n}/{j}': median([r['wall_s'] for r in runs if r['exit_code'] == 0])
                                    for n in bench for j, runs in bench[n].items()}
    save('report.json', report)

# ---------------------------------------------------------------- repaint pacing
if 'frames' in parts:
    frames = {name: [] for name in builds}
    for run in range(min(a.runs, 3)):
        for name in order(run):
            b = builds[name]
            project = out / name / 'frames' / f'pack-{run}'
            shutil.copytree(fixtures / 'pack-1500', project)
            iso, env = isolated(name, f'frames-{run}')
            telemetry = iso / 'telemetry.json'
            proc = subprocess.run([str(b['app'] / 'nova-studio.exe'), '--project', str(project), '--exercise', '--telemetry',
                                   str(telemetry), '--smoke-seconds', '13', '--settings', str(iso / 'settings/studio.ini')],
                                  cwd=str(b['app']), env=env, timeout=120, capture_output=True)
            samples = read_json(telemetry).get('samples_ms', {})
            frames[name].append(dict(exit_code=proc.returncode, **{k: dict(n=len(v), median=median(v),
                                     p95=sorted(v)[int(0.95 * (len(v) - 1))] if v else None)
                                     for k, v in samples.items() if k.startswith(('exercise', 'frame', 'paint', 'cube'))}))
            say(f'frames {name} run {run}: exit {proc.returncode}, keys {sorted(k for k in samples if k.startswith("exercise"))}')
    report['frames'] = frames
    save('report.json', report)

# ---------------------------------------------------------------- MCP latency
if 'mcp' in parts:
    uia = root / 'build' / a.gcc_build / 'tests/nova-phase4f-uia-client.exe'
    uia_env = dict(os.environ, PATH=os.pathsep.join([str(gcc_qt / 'bin'), str(gcc_bin), WINDOWS_PATH]))
    mcp = {name: [] for name in builds}
    for run in range(min(a.runs, 3)):
        for name in order(run):
            b = builds[name]
            iso, env = isolated(name, f'mcp-{run}')
            proc = subprocess.Popen([str(b['app'] / 'nova-studio.exe'), '--project', str(out / name / 'projects/pack-10000'),
                                     '--settings', str(iso / 'settings/studio.ini')], cwd=str(b['app']), env=env)
            hwnd = wait_window(proc.pid, 'Nova Studio')
            time.sleep(4)  # project indexed
            for names, label in (('Resources|MCP Integration…|Agent Setup', 'open'), ('Access|Allow read-only', 'allow')):
                subprocess.run([str(uia), str(hwnd), 'invoke', '45000', names, str(iso / f'uia-{label}.json')], env=uia_env, timeout=120)
            handoff = iso / 'localappdata/Nova Studio/Nova Studio/mcp/pending-connection.json'
            for _ in range(100):
                if handoff.exists():
                    break
                time.sleep(0.1)
            sdk_env = dict(env, PATH=os.pathsep.join([str(a.sdk_python.parent), WINDOWS_PATH]))
            result = subprocess.run([str(a.sdk_python), '-I', str(root / 'scripts/phase4i1-mcp-latency.py'),
                                     str(b['app'] / 'nova-mcp.exe'), '50'], env=sdk_env, capture_output=True, text=True, timeout=300)
            data = json.loads(result.stdout) if result.returncode == 0 else dict(error=result.stderr[-600:])
            for dialog in windows_of(proc.pid, 'MCP Integration'):
                user32.PostMessageW(dialog[0], 0x0010, 0, 0)
            time.sleep(0.5)
            code, _ = close_window(proc, hwnd)
            mcp[name].append(dict(handoff_offered=handoff.exists() or 'error' not in data, exit_code=code, **data))
            say(f'mcp {name} run {run}: {json.dumps(data.get("timings_ms", data))[:300]}')
    report['mcp'] = mcp
    save('report.json', report)

say(f'done: {out}')
