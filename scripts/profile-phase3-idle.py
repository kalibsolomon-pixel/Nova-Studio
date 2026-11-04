"""Phase 3 controlled idle laboratory (retained C8 probe) (read-only on fixtures; no native input).

For each scenario, launches a fresh nova-c8-activity process (the production Window),
waits for its settle marker, and over a fixed window records:
  * process CPU and per-thread CPU / context switches (NtQuerySystemInformation),
    with each thread's start module (Win32 start address);
  * Qt event, timer, repaint, dispatcher-wakeup and native-message counts (probe);
  * whole-machine CPU and the busiest other processes, as a desktop-noise indicator.
"""
import argparse, ctypes as c, hashlib, json, os, pathlib, shutil, statistics, subprocess, time
from ctypes import wintypes as w
import psutil

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("output")
parser.add_argument("--build", default="studio-phase3-release")
parser.add_argument("--runs", type=int, default=3)
parser.add_argument("--settle", type=int, default=15)
parser.add_argument("--seconds", type=int, default=30)
parser.add_argument("--scenarios", nargs="+", default=[
    "empty", "texture", "model", "model-hidden", "display-editor", "animation-paused",
    "animation-playing", "problems", "large-50k"])
args = parser.parse_args()
out = root / args.output
out.mkdir(parents=True, exist_ok=False)
exe = root / "build" / args.build / "tests/nova-c8-activity.exe"
assert exe.is_file(), exe
fixture = root / "build" / ("phase3-idle-" + out.name + "-pristine")  # task-owned copy
shutil.copytree(root / "fixtures/phase-2c5-models", fixture, ignore=shutil.ignore_patterns(".nova"))
large = root / "fixtures/generated/pack-50000"
env = {**os.environ, "PATH": str(root / ".tools/qt/bin") + os.pathsep + os.environ["PATH"]}

# ---- per-thread counters via NtQuerySystemInformation(SystemProcessInformation) ----
class UNICODE_STRING(c.Structure):
    _fields_ = [("Length", w.USHORT), ("MaximumLength", w.USHORT), ("Buffer", c.c_void_p)]
class THREAD(c.Structure):
    _fields_ = [("KernelTime", c.c_longlong), ("UserTime", c.c_longlong), ("CreateTime", c.c_longlong),
                ("WaitTime", w.ULONG), ("StartAddress", c.c_void_p), ("UniqueProcess", c.c_void_p),
                ("UniqueThread", c.c_void_p), ("Priority", c.c_long), ("BasePriority", c.c_long),
                ("ContextSwitches", w.ULONG), ("ThreadState", w.ULONG), ("WaitReason", w.ULONG)]
class PROCESS(c.Structure):
    _fields_ = [("NextEntryOffset", w.ULONG), ("NumberOfThreads", w.ULONG), ("WorkingSetPrivateSize", c.c_longlong),
                ("HardFaultCount", w.ULONG), ("NumberOfThreadsHighWatermark", w.ULONG), ("CycleTime", c.c_ulonglong),
                ("CreateTime", c.c_longlong), ("UserTime", c.c_longlong), ("KernelTime", c.c_longlong),
                ("ImageName", UNICODE_STRING), ("BasePriority", c.c_long), ("UniqueProcessId", c.c_void_p),
                ("InheritedFromUniqueProcessId", c.c_void_p), ("HandleCount", w.ULONG), ("SessionId", w.ULONG),
                ("UniqueProcessKey", c.c_void_p), ("PeakVirtualSize", c.c_size_t), ("VirtualSize", c.c_size_t),
                ("PageFaultCount", w.ULONG), ("PeakWorkingSetSize", c.c_size_t), ("WorkingSetSize", c.c_size_t),
                ("QuotaPeakPagedPoolUsage", c.c_size_t), ("QuotaPagedPoolUsage", c.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", c.c_size_t), ("QuotaNonPagedPoolUsage", c.c_size_t),
                ("PagefileUsage", c.c_size_t), ("PeakPagefileUsage", c.c_size_t), ("PrivatePageCount", c.c_size_t),
                ("ReadOperationCount", c.c_longlong), ("WriteOperationCount", c.c_longlong),
                ("OtherOperationCount", c.c_longlong), ("ReadTransferCount", c.c_longlong),
                ("WriteTransferCount", c.c_longlong), ("OtherTransferCount", c.c_longlong)]
assert c.sizeof(PROCESS) == 256 and c.sizeof(THREAD) == 80
ntdll = c.WinDLL("ntdll")
kernel = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi")
kernel.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]; kernel.OpenThread.restype = w.HANDLE
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]; kernel.OpenProcess.restype = w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]
ntdll.NtQueryInformationThread.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.ULONG, c.c_void_p]
psapi.GetMappedFileNameW.argtypes = [w.HANDLE, c.c_void_p, w.LPWSTR, w.DWORD]

def threads(pid):
    size = 1 << 21
    while True:
        buffer = c.create_string_buffer(size)
        needed = w.ULONG()
        status = ntdll.NtQuerySystemInformation(5, buffer, size, c.byref(needed))
        if status == 0:
            break
        size = max(size * 2, needed.value + 65536)
    offset = 0
    while True:
        process = PROCESS.from_buffer_copy(buffer, offset)
        if (process.UniqueProcessId or 0) == pid:
            rows = {}
            for i in range(process.NumberOfThreads):
                t = THREAD.from_buffer_copy(buffer, offset + c.sizeof(PROCESS) + i * c.sizeof(THREAD))
                rows[t.UniqueThread] = dict(cpu=(t.KernelTime + t.UserTime) / 1e4, switches=t.ContextSwitches)
            return rows
        if not process.NextEntryOffset:
            return {}
        offset += process.NextEntryOffset

def module(pid, tid):
    handle = kernel.OpenThread(0x40, False, tid)
    process = kernel.OpenProcess(0x410, False, pid)
    name = c.create_unicode_buffer(1024)
    address = c.c_void_p()
    if handle and process:
        ntdll.NtQueryInformationThread(handle, 9, c.byref(address), c.sizeof(address), None)
        psapi.GetMappedFileNameW(process, address, name, 1024)
    for h in (handle, process):
        if h:
            kernel.CloseHandle(h)
    return pathlib.PureWindowsPath(name.value).name if name.value else "?"

rows = []
order = [s for _ in range(args.runs) for s in args.scenarios]
for k, scenario in enumerate(order):
    run = k // len(args.scenarios)
    name = f"{scenario}-{run}"
    activity = out / f"{name}.json"
    project = "" if scenario == "welcome" else large if scenario == "large-50k" else fixture
    probe_scenario = "empty" if scenario in ("large-50k", "welcome") else scenario
    command = [str(exe), "--project", str(project), "--settings", str(out / f"{name}.ini"), "--scenario",
               probe_scenario, "--activity", str(activity), "--settle", str(args.settle), "--seconds",
               str(args.seconds), "--telemetry", str(out / f"{name}-telemetry.json")]
    with (out / f"{name}.log").open("w") as log:
        child = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=log)
        marker = pathlib.Path(str(activity) + ".start")
        deadline = time.time() + args.settle + 60
        while not marker.exists() and time.time() < deadline and child.poll() is None:
            time.sleep(0.05)
        proc = psutil.Process(child.pid)
        psutil.cpu_percent(None)
        others_before = {p.pid: p.cpu_times() for p in psutil.process_iter() if p.pid not in (0, child.pid)}
        before_threads = threads(child.pid)
        before = proc.cpu_times()
        switches_before = proc.num_ctx_switches()
        start = time.perf_counter()
        time.sleep(max(1, args.seconds - 3))  # margin: the probe exits at the window end
        try:
            after = proc.cpu_times()
            switches_after = proc.num_ctx_switches()
        except psutil.NoSuchProcess:
            print(f"{name}: process ended before the sample; discarded", flush=True)
            child.wait(timeout=60)
            continue
        after_threads = threads(child.pid)
        elapsed = time.perf_counter() - start
        system = psutil.cpu_percent(None)
        others = []
        for p in psutil.process_iter(["name"]):
            if p.pid in others_before:
                try:
                    t = p.cpu_times()
                    b = others_before[p.pid]
                    others.append((100 * (t.user + t.system - b.user - b.system) / elapsed, p.info["name"]))
                except psutil.Error:
                    pass
        thread_rows = []
        for tid, a in after_threads.items():
            b = before_threads.get(tid, dict(cpu=0, switches=0))
            thread_rows.append(dict(tid=tid, module=module(child.pid, tid), cpu_ms=a["cpu"] - b["cpu"],
                                    switches=a["switches"] - b["switches"]))
        try:
            child.wait(timeout=60)
        except subprocess.TimeoutExpired:
            child.kill()  # task-owned probe; its activity file is already written
            child.wait()
    data = json.loads(activity.read_text(encoding='utf-8')) if activity.exists() else {}
    events = data.get("events", {})
    event_seconds = data.get("measured_ms", elapsed * 1000) / 1000  # probe's own counting window
    rows.append(dict(scenario=scenario, run=run, seconds=elapsed,
                     cpu_percent_one_core=100 * (after.user + after.system - before.user - before.system) / elapsed,
                     context_switches_per_second=(switches_after[0] - switches_before[0]) / elapsed,
                     thread_switches_per_second=sum(t["switches"] for t in thread_rows) / elapsed,
                     dispatcher_awake_per_second=events.get("dispatcher_awake", 0) / event_seconds,
                     uia_queries_per_second=events.get("native_0x3d", 0) / event_seconds,
                     wm_user_per_second=events.get("native_0x400", 0) / event_seconds,
                     timer_events=sum(v for k2, v in events.items() if k2.startswith("1:")),
                     paint_events=sum(v for k2, v in events.items() if k2.startswith("12:")),
                     update_requests=sum(v for k2, v in events.items() if k2.startswith("77:")),
                     system_cpu_percent=system,
                     busiest_other=sorted(others, reverse=True)[:5],
                     threads=sorted(thread_rows, key=lambda t: -t["switches"]),
                     memory=data.get("memory"), exit_code=child.returncode))
    r = rows[-1]
    print(f"{name}: cpu {r['cpu_percent_one_core']:.3f}%  wakeups {r['thread_switches_per_second']:.1f}/s  "
          f"awake {r['dispatcher_awake_per_second']:.2f}/s  system {system:.1f}%", flush=True)
    (out / "rows.json").write_text(json.dumps(rows, indent=2) + "\n", encoding='utf-8')
summary = {}
for scenario in args.scenarios:
    cell = [r for r in rows if r["scenario"] == scenario]
    if not cell:
        continue
    summary[scenario] = {key: dict(median=statistics.median(r[key] for r in cell), min=min(r[key] for r in cell),
                                   max=max(r[key] for r in cell))
                         for key in ("cpu_percent_one_core", "thread_switches_per_second",
                                     "dispatcher_awake_per_second", "uia_queries_per_second",
                                     "wm_user_per_second", "system_cpu_percent")}
(out / "summary.json").write_text(json.dumps(dict(
    head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    build=args.build, settle_seconds=args.settle, window_seconds=args.seconds, runs=args.runs,
    executable_sha256=hashlib.file_digest(exe.open('rb'), 'sha256').hexdigest(),
    build_identity=(root/'build'/args.build/'generated/build_identity.h').read_text(encoding='utf-8'),
    cpu_count=psutil.cpu_count(), summary=summary,
    notes="One logical processor = 100%. Fresh process per sample; interleaved scenario order."), indent=2) + "\n", encoding='utf-8')
# Preserve the task-owned fixture and raw observations; no cleanup.
print(json.dumps(summary, indent=2))
