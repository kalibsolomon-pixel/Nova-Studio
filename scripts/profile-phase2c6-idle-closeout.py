"""Matched C5/C6 static idle observations, alternating order, no native input.

Run after native acceptance/builds/tests have finished. Every process, settings file
and model-fixture copy is owned by this run; no historical documents are opened.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import time
import psutil

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", default="docs/phase-2c6/idle-closeout-corrected")
parser.add_argument("--seconds", type=int, default=60)
options = parser.parse_args()
assert options.seconds >= 60
assert not any((p.info["name"] or "").startswith("nova-c6-")
               for p in psutil.process_iter(["name"])), "Close other owned C6 probes before profiling"
out = root / options.output
out.mkdir(exist_ok=False)
fixture = root / ("build/studio-c6-" + out.name + "-pristine")
assert not fixture.exists()
shutil.copytree(root / "fixtures/phase-2c5-models", fixture, ignore=shutil.ignore_patterns(".nova"))
after = root / ("build/studio-c6-release/tests/nova-c6-" + out.name + ".exe")
assert not after.exists()
shutil.copy2(root / "build/studio-c6-release/tests/nova-c6-activity.exe", after)
executables = {"before": after.with_name("nova-c6-idle-before.exe"), "after": after}
lines = (root / "docs/phase-2c6/native/studio.ini").read_text().splitlines()
index = next(i for i, line in enumerate(lines) if line.startswith("geometry="))
geometry_lines = [lines[index]]
while geometry_lines[-1].endswith("\\"):
    index += 1
    geometry_lines.append(lines[index])
geometry = "\n".join(geometry_lines)
assert geometry.endswith(")"), "Complete Qt geometry must be retained"
env = {**os.environ, "PATH": str(root / ".tools/qt/bin") + os.pathsep + os.environ["PATH"]}
summary = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
           "utc_start": datetime.now(timezone.utc).isoformat(),
           "duration_seconds": options.seconds, "external_settle_seconds": 12,
           "cpu_units": "percent of one logical processor, not whole-machine percent",
           "logical_processors": psutil.cpu_count(), "rows": [],
           "notes": "No concurrent agent builds, tests or native interactions. Other desktop activity is uncontrolled.",
           "binaries": {key: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                        for key, path in executables.items()},
           "fixture": {"path": str(fixture), "manifest": [
               {"path": p.relative_to(fixture).as_posix(), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
               for p in sorted(fixture.rglob("*")) if p.is_file()]}}
def save():
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
save()
for run in range(2):
    for scenario in ("empty", "model", "hidden", "large"):
        for side in (("before", "after") if run == 0 else ("after", "before")):
            name = f"{side}-{scenario}-{run}"
            settings = out / (name + ".ini")
            # Real saved native geometry keeps both revisions in the same visible
            # desktop bounds. No other saved project/resource/document state.
            settings.write_text("[workspace]\n" + geometry + "\n")
            pack = None if scenario == "empty" else root / "fixtures/generated/pack-50000" if scenario == "large" else fixture
            args = [str(executables[side]), "--scenario", scenario, "--settings", str(settings),
                    "--seconds", str(options.seconds), "--activity", str(out / (name + "-activity.json")),
                    "--telemetry", str(out / (name + "-telemetry.json"))]
            if pack:
                args += ["--project", str(pack)]
            with (out / (name + ".txt")).open("w") as log:
                child = subprocess.Popen(args, cwd=root, env=env, creationflags=subprocess.CREATE_NO_WINDOW,
                                         stdout=log, stderr=log)
                process = psutil.Process(child.pid)
                time.sleep(12)
                start = time.perf_counter()
                cpu_before = process.cpu_times()
                system_before = psutil.cpu_times()
                threads = {t.id: t.user_time + t.system_time for t in process.threads()}
                samples = []
                while time.perf_counter() - start < options.seconds - 15:
                    time.sleep(5)
                    cpu = process.cpu_times()
                    memory = process.memory_info()
                    samples.append({"seconds": time.perf_counter() - start,
                        "cpu_ms": 1000 * (cpu.user + cpu.system - cpu_before.user - cpu_before.system),
                        "working_set_bytes": memory.rss, "private_bytes": getattr(memory, "private", 0),
                        "threads": process.num_threads(), "thread_cpu_ms": {
                            str(t.id): 1000 * (t.user_time + t.system_time - threads.get(t.id, 0))
                            for t in process.threads()}})
                child.wait(timeout=15)
                assert child.returncode == 0, (name, child.returncode)
                assert (out / (name + "-activity.json")).exists(), name
                system_after = psutil.cpu_times()
                system_total = sum(system_after) - sum(system_before)
                system_idle = system_after.idle - system_before.idle
                row = {"side": side, "scenario": scenario, "run": run, "exit": child.returncode,
                       "utc_end": datetime.now(timezone.utc).isoformat(), "samples": samples,
                       "cpu_percent_one_processor": samples[-1]["cpu_ms"] / samples[-1]["seconds"] / 10,
                       "machine_busy_percent": 100 * (1 - system_idle / system_total) if system_total else None}
                summary["rows"].append(row)
                save()
                print(name, round(row["cpu_percent_one_processor"], 3), flush=True)
summary["utc_end"] = datetime.now(timezone.utc).isoformat()
save()
