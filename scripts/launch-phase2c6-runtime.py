"""Launch only the prepared, owned C6 offline demo; never reads account files."""
import hashlib
import json
import pathlib
import subprocess
from datetime import datetime, timezone

root = pathlib.Path(__file__).resolve().parents[1]
evidence = root / "docs/phase-2c6/runtime"
record = json.loads((evidence / "instance.json").read_text())
instance = pathlib.Path(record["instance"]).resolve()
assert instance == (root / "build/studio-c6-runtime/minecraft-1.21.11").resolve()
args = record["args"]
assert "--demo" in args and args[args.index("--accessToken") + 1] in ("0", "none", "null", "")
assert pathlib.Path(args[args.index("--gameDir") + 1]).resolve() == instance
pack = instance / "resourcepacks/Nova-C6"
manifest = [{"path": p.relative_to(pack).as_posix(),
             "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(pack.rglob("*")) if p.is_file()]
assert not any(".nova" in row["path"].split("/") for row in manifest)
with (evidence / "pack-manifest-final.json").open("x") as file:
    json.dump(manifest, file, indent=2)
with (evidence / "runtime-stdout.txt").open("x") as stdout, (evidence / "runtime-stderr.txt").open("x") as stderr:
    child = subprocess.Popen([record["java"], *args], cwd=instance,
                             creationflags=subprocess.CREATE_NO_WINDOW,
                             stdout=stdout, stderr=stderr)
    launch = {"pid": child.pid, "instance": str(instance),
              "utc": datetime.now(timezone.utc).isoformat(), "version": record["version"],
              "mode": "existing offline demo arguments, fresh owned world copy"}
    (evidence / "launch.json").write_text(json.dumps(launch, indent=2) + "\n")
    print(json.dumps(launch), flush=True)
    exit_code = child.wait()
    (evidence / "exit.json").write_text(json.dumps({"exit_code": exit_code,
        "utc": datetime.now(timezone.utc).isoformat()}, indent=2) + "\n")
    print("Runtime exit:", exit_code, flush=True)
