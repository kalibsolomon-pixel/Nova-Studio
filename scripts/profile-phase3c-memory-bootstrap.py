"""Factor the standalone memory probe's context/style bootstrap, not production code."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', default='docs/phase-3c/memory-bootstrap')
args = parser.parse_args()
out = root / args.output
out.mkdir(parents=True, exist_ok=False)
exe = root / 'build/studio-phase3c-release/tests/nova-c8-memory-probe.exe'
env = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
variants = [('original', []), ('shared', ['--shared-contexts']), ('fusion', ['--fusion']),
            ('shared-fusion', ['--shared-contexts', '--fusion']), ('empty-first', ['--empty-first'])]
rows = []
for n in range(3):
    for label, flags in variants if n % 2 == 0 else list(reversed(variants)):
        name = f'{label}-{n}'
        target = out / (name + '.json')
        command = [str(exe), str(root / 'fixtures/phase-2c5-models'),
            str(root / 'fixtures/generated/pack-50000'), str(target), *flags]
        with (out / (name + '.txt')).open('wb') as log:
            result = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=log,
                timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        rows.append(dict(variant=label, run=n, exit_code=result.returncode, command=command))
        (out / 'runs.json').write_text(json.dumps(rows, indent=2)+'\n')
        print(name, result.returncode, flush=True)
        if result.returncode: raise SystemExit('Bootstrap memory probe failed')
(out / 'identity.json').write_text(json.dumps(dict(executable_sha256=hashlib.file_digest(exe.open('rb'), 'sha256').hexdigest(),
    build_identity=(root / 'build/studio-phase3c-release/generated/build_identity.h').read_text(),
    note='Same archive set; vary shared contexts, Fusion, or whether the empty-preview QPainter path runs first. Not a full production bootstrap.'), indent=2)+'\n')
