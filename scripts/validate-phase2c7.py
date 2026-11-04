"""Run the preserved C6 regression driver including all newly registered C7 suites."""
import os, pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[1]
environment={**os.environ, 'NOVA_C7_EVIDENCE_DIR':str(root/sys.argv[2]/'c7-screenshots')}
raise SystemExit(subprocess.call([sys.executable,str(root/'scripts/validate-phase2c6.py'),*sys.argv[1:]],env=environment,cwd=root))
