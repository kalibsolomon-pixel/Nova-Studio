"""Run the preserved C6/C7 regression driver with every registered suite, including C8.

Adds the C8 evidence directory and, when not already set, the verified task-local 26.1
vanilla cache (see docs/phase-2c8/BASELINE.md) so the optional vanilla tests run.
"""
import os, pathlib, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
environment = {**os.environ,
               'NOVA_C7_EVIDENCE_DIR': str(root / sys.argv[2] / 'c7-screenshots'),
               'NOVA_C8_EVIDENCE_DIR': str(root / sys.argv[2] / 'c8-screenshots')}
cache = root / 'build/studio-c8-vanilla/26.1'
if 'NOVA_VANILLA_CACHE' not in environment and cache.is_dir():
    environment['NOVA_VANILLA_CACHE'] = str(cache)
raise SystemExit(subprocess.call([sys.executable, str(root / 'scripts/validate-phase2c6.py'), *sys.argv[1:]],
                                 env=environment, cwd=root))
