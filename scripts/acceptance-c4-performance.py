"""Quiet same-session static-startup/idle comparison against the preserved pre-acceptance C4 binary."""
import json
import math
import os
import pathlib
import statistics
import subprocess
import time
import psutil

root = pathlib.Path(__file__).resolve().parents[1]
out = root / "docs/phase-2c4/acceptance/performance/static"
out.mkdir(exist_ok=False)
env = {**os.environ, "PATH": str(root / ".tools/qt/bin") + os.pathsep + os.environ["PATH"]}
variants = {"current": root / "build/studio-c4-acceptance-release/src/nova-studio.exe",
            "c4_control": root / "build/studio-c4-release/src/nova-studio.exe"}
def run(command, timeout=120):
    result = subprocess.run(list(map(str, command)), cwd=root, env=env, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError((command, result.returncode, result.stderr.decode(errors="replace")))
    return result
def stats(values):
    return {"n": len(values), "p50": statistics.median(values), "p95": sorted(values)[math.ceil(.95 * len(values)) - 1]}
startup = {key: [] for key in variants}
for repetition in range(5):
    for key, exe in variants.items():
        telemetry = out / f"startup-{key}-{repetition}.json"
        run([exe, "--project", root / "fixtures/generated/pack-1500", "--fresh-layout", "--settings",
             root / f"build/studio-c4-acceptance/profile-{key}-{repetition}/settings.ini", "--telemetry", telemetry,
             "--smoke-seconds", "2"])
        samples = json.loads(telemetry.read_text())["samples_ms"]
        startup[key].append({name: samples[name][0] for name in ("ui_construction", "first_ui_ms", "ready_capture_ms")})
for key, exe in variants.items():
    run([exe, "--bench", root / "fixtures/generated/pack-50000", "--output", out / f"core-50000-{key}.json"])
processes = {}
observations = {key: [] for key in variants}
for key, exe in variants.items():
    process = subprocess.Popen([str(exe), "--project", str(root / "fixtures/generated/pack-1500"), "--fresh-layout",
                                "--settings", str(root / f"build/studio-c4-acceptance/idle-{key}/settings.ini"), "--smoke-seconds", "100"], cwd=root, env=env)
    processes[key] = (process, psutil.Process(process.pid))
start = time.perf_counter()
for sample in range(18):
    time.sleep(5)
    for key, (_, process) in processes.items():
        memory, cpu = process.memory_info(), process.cpu_times()
        observations[key].append({"seconds": time.perf_counter() - start, "working_set_mib": memory.rss / 2**20,
                                  "private_mib": memory.private / 2**20, "cpu_seconds": cpu.user + cpu.system})
summary = {"startup_ms": {key: {name: stats([row[name] for row in rows]) for name in rows[0]} for key, rows in startup.items()},
           "idle": {}}
for key, rows in observations.items():
    first = next(row for row in rows if row["seconds"] >= 30)
    last = rows[-1]
    summary["idle"][key] = {"cpu_percent_one_processor_30s_to_90s": 100 * (last["cpu_seconds"] - first["cpu_seconds"]) / (last["seconds"] - first["seconds"]),
                            "working_set_mib_last_four_median": statistics.median(row["working_set_mib"] for row in rows[-4:]),
                            "private_mib_last_four_median": statistics.median(row["private_mib"] for row in rows[-4:])}
for process, _ in processes.values():
    assert process.wait(timeout=30) == 0
(out / "idle-raw.json").write_text(json.dumps(observations, indent=2))
(out / "summary.json").write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
