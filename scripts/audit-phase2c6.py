"""Read-only final repository/preservation audit; output belongs to ignored build/."""
import gzip
import hashlib
import json
import pathlib
import subprocess
from datetime import datetime, timezone

root = pathlib.Path(__file__).resolve().parents[1]
out = root / "build/studio-c6-delivery/final-audit.json"
assert not out.exists()
out.parent.mkdir(exist_ok=True)
def git(*args):
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()
base = "45a8117e7c21a38b4982e1ac19877fa26c0dce55"
report = {"utc": datetime.now(timezone.utc).isoformat(), "branch": git("branch", "--show-current"),
          "starting_head": base, "final_head": git("rev-parse", "HEAD"),
          "status": git("status", "--short"), "commits": git("log", "--format=%H %s", base + "..HEAD").splitlines(),
          "deleted_tracked_paths": git("diff", "--diff-filter=D", "--name-only", base).splitlines(),
          "inventories": {}, "validation": {}, "preserved_native_additions": [],
          "historical_tracked_changes": git("diff", "--name-only", base, "--", "docs/phase-1",
              "docs/phase-2b", "docs/phase-2c1", "docs/phase-2c2", "docs/phase-2c3",
              "docs/phase-2c4", "docs/phase-2c5", "fixtures/phase-2c5-models").splitlines(),
          "boundary": "Local commits only. No push, release, publication or C7 implementation."}
changed = git("diff", "--name-only", base).splitlines()
report["historical_tracked_changes"] += [p for p in changed if p.startswith("docs/")
    and not p.startswith("docs/phase-2c6/") and p not in report["historical_tracked_changes"]]
for kind in ("tracked", "untracked", "ignored"):
    path = root / f"docs/phase-2c6/{kind}-at-start.txt"
    content = gzip.open(str(path) + ".gz", "rt", encoding="utf-8-sig").read() if kind == "ignored" else path.read_text(encoding="utf-8-sig")
    paths = content.splitlines()
    report["inventories"][kind] = {"count": len(paths), "missing": [p for p in paths if not (root / p).exists()]}
for configuration in ("release", "debug"):
    data = json.loads((root / f"docs/phase-2c6/{configuration}-inspection/summary.json").read_text())
    actual = hashlib.sha256((root / f"build/studio-c6-{configuration}/src/nova-studio.exe").read_bytes()).hexdigest()
    report["validation"][configuration] = {k: data[k] for k in
        ("source_head", "suite_count", "seconds", "exit_code", "totals_including_init_cleanup", "executable_sha256")}
    report["validation"][configuration]["executable_matches_tested"] = actual == data["executable_sha256"]
    assert data["exit_code"] == 0 and data["suite_count"] == 28
    assert data["totals_including_init_cleanup"] == {"passed": 240, "failed": 0, "skipped": 0, "blacklisted": 0}
    assert actual == data["executable_sha256"]
manifest = json.loads((root / "docs/phase-2c6/preserved-local-native-additions.json").read_text())
for row in manifest["files"]:
    path = root / row["path"]
    match = path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"]
    report["preserved_native_additions"].append({"path": row["path"], "sha256_matches": match})
    assert match
assert report["branch"] == "codex/phase-2c6"
assert not report["deleted_tracked_paths"] and not report["historical_tracked_changes"]
assert all(not data["missing"] for data in report["inventories"].values())
assert not git("diff", "--name-only") and not git("diff", "--cached", "--name-only")
assert report["status"] == "?? fixtures/phase-2c5-models/.nova/", report["status"]
out.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"final_head": report["final_head"], "status": report["status"],
                  "preservation": report["inventories"], "output": str(out)}, indent=2))
