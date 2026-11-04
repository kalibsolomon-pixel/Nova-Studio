"""Run all generated CTest commands unchanged, with per-suite logs and preservation checks."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("build")
parser.add_argument("output")
parser.add_argument("--preservation", action="store_true")
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
build = root / "build" / args.build
output = root / args.output
output.mkdir(parents=True, exist_ok=False)
environment = {**os.environ, "PATH": str(root / ".tools/qt/bin") + os.pathsep + os.environ["PATH"],
               "NOVA_LARGE_FIXTURE": str(root / "fixtures/generated/pack-50000"),
               "NOVA_EVIDENCE_DIR": str(output / "c3-screenshots"),
               "NOVA_C4_EVIDENCE_DIR": str(output / "screenshots")}
generated = (build / "tests/CTestTestfile.cmake").read_text()
tests = re.findall(r'add_test\(\[=\[(.*?)\]=\] "(.*?)"\)', generated)
assert len(tests) == 18, tests
driver = output / "driver"
driver.mkdir()
lines = []
for name, executable in tests:
    lines.append(f'add_test("{name}" "{executable}" "-o" "{(output / name).as_posix()}.txt,txt" "-o" "{(output / name).as_posix()}.xml,xunitxml")')
    lines.append(f'set_tests_properties("{name}" PROPERTIES WORKING_DIRECTORY "{(build / "tests").as_posix()}" TIMEOUT 180)')
(driver / "CTestTestfile.cmake").write_text("\n".join(lines) + "\n")
cmake = re.search(r"CMAKE_COMMAND:INTERNAL=(.*)", (build / "CMakeCache.txt").read_text())[1]
started = time.perf_counter()
result = subprocess.run([str(pathlib.Path(cmake).with_name("ctest.exe")), "--test-dir", str(driver),
                         "--output-on-failure", "--output-junit", str(output / "junit.xml")],
                        cwd=root, env=environment, capture_output=True, timeout=900)
(output / "ctest.txt").write_bytes(result.stdout + result.stderr)
rows = []
for name, executable in tests:
    path = output / f"{name}.txt"
    match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted", path.read_text()) if path.exists() else None
    rows.append({"suite": name, **(dict(zip(("passed", "failed", "skipped", "blacklisted"), map(int, match.groups()))) if match else {"missing_summary": True})})
summary = {"source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
           "working_tree": subprocess.check_output(["git", "status", "--short"], cwd=root, text=True),
           "build": args.build, "suite_count": len(rows), "seconds": time.perf_counter() - started,
           "exit_code": result.returncode, "suites": rows,
           "totals_including_init_cleanup": {key: sum(row.get(key, 0) for row in rows) for key in ("passed", "failed", "skipped", "blacklisted")},
           "executable_sha256": hashlib.sha256((build / "src/nova-studio.exe").read_bytes()).hexdigest()}
if args.preservation:
    inventories = {}
    for name in ("tracked", "untracked", "ignored"):
        paths = (root / f"docs/phase-2c4/{name}-at-start.txt").read_text(encoding="utf-8-sig").splitlines()
        inventories[name] = {"count": len(paths), "missing": [path for path in paths if not (root / path).exists()]}
    summary["preservation"] = inventories
    summary["deleted_diff"] = subprocess.check_output(["git", "diff", "--diff-filter=D", "--name-only", "e795aa54a7e0d8f679965da5cc3dbcc1d2ac065d"], cwd=root, text=True).splitlines()
(output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
raise SystemExit(result.returncode)
