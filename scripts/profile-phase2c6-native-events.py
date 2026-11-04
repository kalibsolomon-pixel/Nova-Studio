"""One diagnostic of the unexplained high-wakeup empty case; counts, not keystrokes.

All-event/native-message instrumentation adds overhead and is not a matched CPU
benchmark. No input is synthesized, and only this owned process is observed.
"""
import hashlib
import json
import os
import pathlib
import subprocess
import time
import psutil

root = pathlib.Path(__file__).resolve().parents[1]
out = root / "docs/phase-2c6/idle-native-diagnostic"
out.mkdir(exist_ok=False)
exe = root / "build/studio-c6-release/tests/nova-c6-activity.exe"
assert not any((p.info["name"] or "").startswith("nova-c6-")
               for p in psutil.process_iter(["name"]))
lines = (root / "docs/phase-2c6/native/studio.ini").read_text().splitlines()
index = next(i for i, line in enumerate(lines) if line.startswith("geometry="))
geometry = [lines[index]]
while geometry[-1].endswith("\\"):
    index += 1
    geometry.append(lines[index])
settings = out / "settings.ini"
settings.write_text("[workspace]\n" + "\n".join(geometry) + "\n")
env = {**os.environ, "PATH": str(root / ".tools/qt/bin") + os.pathsep + os.environ["PATH"]}
args = [str(exe), "--scenario", "empty", "--settings", str(settings), "--seconds", "75",
        "--native-events", "--activity", str(out / "activity.json"),
        "--telemetry", str(out / "telemetry.json")]
with (out / "process.txt").open("w") as log:
    child = subprocess.Popen(args, cwd=root, env=env, creationflags=subprocess.CREATE_NO_WINDOW,
                             stdout=log, stderr=log)
    process = psutil.Process(child.pid)
    time.sleep(12)
    start, before = time.perf_counter(), process.cpu_times()
    threads = {t.id: t.user_time + t.system_time for t in process.threads()}
    samples = []
    while time.perf_counter() - start < 60:
        time.sleep(5)
        cpu, memory = process.cpu_times(), process.memory_info()
        samples.append({"seconds": time.perf_counter() - start,
                        "cpu_ms": 1000 * (cpu.user + cpu.system - before.user - before.system),
                        "working_set_bytes": memory.rss, "private_bytes": getattr(memory, "private", 0),
                        "thread_cpu_ms": {str(t.id): 1000 * (t.user_time + t.system_time - threads.get(t.id, 0))
                                          for t in process.threads()}})
    child.wait(timeout=15)
    assert child.returncode == 0
events = json.loads((out / "activity.json").read_text())["events"]
report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
          "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "exit": child.returncode,
          "samples": samples, "cpu_percent_one_processor": samples[-1]["cpu_ms"] / samples[-1]["seconds"] / 10,
          "qt_input_counts": {str(kind): events.get("qt_event_" + str(kind), 0)
                              for kind in (2, 3, 4, 5, 6, 7, 31, 82)},
          "native_messages": {k: v for k, v in events.items() if k.startswith("native_")},
          "boundary": "Counts of own-process messages/events only. No text or global input capture. Instrumentation adds overhead; no causal CPU attribution."}
(out / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({k: report[k] for k in ("head", "cpu_percent_one_processor", "qt_input_counts", "native_messages")}, indent=2))
