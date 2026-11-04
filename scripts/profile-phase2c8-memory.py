"""C8 memory attribution for the standalone model/render benchmark.

Runs the unchanged nova-model-bench executable from several builds, each with the
fixture next to the 50,000-asset pack (the benchmark then indexes and catalogs that
pack before its first memory sample) and with an isolated copy of the same fixture
(the 50k phase is skipped). Three fresh processes per cell. Read-only on fixtures.
"""
import argparse, hashlib, json, os, pathlib, statistics, subprocess

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("output")
parser.add_argument("--builds", nargs="+", default=["studio-c6-release", "studio-c7-release", "studio-c8-release"])
parser.add_argument("--runs", type=int, default=3)
args = parser.parse_args()
out = root / args.output
out.mkdir(parents=True, exist_ok=False)
env = {**os.environ, "PATH": str(root / ".tools/qt/bin") + os.pathsep + os.environ["PATH"]}
flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
fixtures = {"with-50k-phase": root / "fixtures/phase-2c5-models",
            "isolated": root / "build/c8-memory/isolated/phase-2c5-models"}
assert fixtures["isolated"].is_dir(), "copy fixtures/phase-2c5-models to build/c8-memory/isolated first"
rows = []
for build in args.builds:
    exe = root / "build" / build / "tests/nova-model-bench.exe"
    for label, fixture in fixtures.items():
        for n in range(args.runs):
            target = out / f"{build}-{label}-{n}.json"
            result = subprocess.run([str(exe), str(fixture), str(target)], cwd=root, env=env,
                                    creationflags=flags, capture_output=True, timeout=180)
            (out / f"{build}-{label}-{n}.log").write_bytes(result.stdout + result.stderr)
            assert result.returncode == 0, (build, label, n, result.returncode)
            data = json.loads(target.read_text())
            mib = lambda key: data[key] / (1024 * 1024)
            rows.append(dict(build=build, fixture=label, run=n,
                             sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                             large_index=data.get("large_index", {}),
                             before_cycles_mib=mib("working_set_before_model_cycles"),
                             after_cycles_mib=mib("working_set_after_25_model_cycles"),
                             after_close_mib=mib("working_set_after_generic_close")))
            print(build, label, n, round(rows[-1]["before_cycles_mib"], 2), round(rows[-1]["after_cycles_mib"], 2),
                  flush=True)
summary = {}
for build in args.builds:
    for label in fixtures:
        cell = [r for r in rows if r["build"] == build and r["fixture"] == label]
        summary[f"{build}/{label}"] = {k: dict(median=statistics.median(r[k] for r in cell),
                                               min=min(r[k] for r in cell), max=max(r[k] for r in cell))
                                       for k in ("before_cycles_mib", "after_cycles_mib", "after_close_mib")}
(out / "summary.json").write_text(json.dumps(dict(
    head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    rows=rows, summary=summary,
    notes="Working set from GetProcessMemoryInfo inside the benchmark. Fresh process per run."), indent=2) + "\n")
print(json.dumps(summary, indent=2))
