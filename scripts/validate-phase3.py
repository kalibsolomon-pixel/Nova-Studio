"""Run every registered suite through the retained driver and preserve build identity."""
import os
import pathlib
import subprocess
import sys
root = pathlib.Path(__file__).resolve().parents[1]
if len(sys.argv) != 3:
    raise SystemExit('Usage: validate-phase3.py <build-name> <new-output-directory>')
output = root / sys.argv[2]
environment = {**os.environ, 'NOVA_PHASE3_EVIDENCE_DIR': str(output / 'phase3-screenshots')}
result = subprocess.call([sys.executable, str(root / 'scripts/validate-phase2c8.py'), *sys.argv[1:]],
                         cwd=root, env=environment)
if output.is_dir():
    identity = root / 'build' / sys.argv[1] / 'generated/build_identity.h'
    if identity.exists():
        (output / 'build-identity.h.txt').write_bytes(identity.read_bytes())
    (output / 'product-source.diff').write_bytes(subprocess.check_output(
        ['git', 'diff', 'HEAD', '--', 'src', 'CMakeLists.txt', 'tests', 'scripts'], cwd=root))
raise SystemExit(result)
