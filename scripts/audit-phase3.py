"""Non-destructive Phase 3 inventory and preservation verification."""
import gzip
import hashlib
import json
import pathlib
import subprocess
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs/phase-3'

def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

if '--verify' in sys.argv:
    baseline = json.loads((OUT / 'starting-audit.json').read_text(encoding='utf-8'))
    changed = [p for p, h in baseline['protected_hashes'].items()
               if not (ROOT / p).is_file() or digest(ROOT / p) != h]
    missing = []
    for kind in ('tracked', 'untracked', 'ignored'):
        with gzip.open(OUT / f'{kind}-at-start.txt.gz', 'rt', encoding='utf-8') as stream:
            missing += [p for p in stream.read().splitlines() if not (ROOT / p).exists()]
    result = dict(head=git('rev-parse', 'HEAD'), branch=git('branch', '--show-current'),
                  status=git('status', '--short'), protected_hash_mismatches=changed,
                  missing_paths=missing,
                  deleted=git('diff', '--diff-filter=D', '--name-only', baseline['head']).splitlines())
    if '--stdout-only' not in sys.argv:
        (OUT / 'preservation.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    sys.exit(bool(changed or missing or result['deleted']))

OUT.mkdir(parents=True, exist_ok=True)
if (OUT / 'starting-audit.json').exists():
    raise SystemExit('Starting inventory already exists; refusing replacement')
baseline = dict(utc=datetime.now(timezone.utc).isoformat(), head=git('rev-parse', 'HEAD'),
                branch=git('branch', '--show-current'), status=git('status', '--short'),
                counts={}, tracked_hashes={}, protected_hashes={})
for kind, args in [('tracked', ('ls-files',)),
                   ('untracked', ('ls-files', '--others', '--exclude-standard')),
                   ('ignored', ('ls-files', '--others', '--ignored', '--exclude-standard'))]:
    paths = [p for p in git(*args).splitlines() if not p.startswith('docs/phase-3/')]
    baseline['counts'][kind] = len(paths)
    with gzip.open(OUT / f'{kind}-at-start.txt.gz', 'wt', encoding='utf-8') as stream:
        stream.write('\n'.join(paths) + '\n')
    for relative in paths:
        path = ROOT / relative
        protected = (relative.startswith(('docs/', 'fixtures/', 'prototypes/', 'logs/'))
                     or path.suffix in ('.nvdoc', '.ini', '.log')
                     or 'recovery' in relative.lower() or '/.nova/' in relative)
        if path.is_file() and (kind == 'tracked' or protected):
            hashed = digest(path)
            if kind == 'tracked':
                baseline['tracked_hashes'][relative] = hashed
            if protected:
                baseline['protected_hashes'][relative] = hashed
(OUT / 'starting-audit.json').write_text(json.dumps(baseline, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: v for k, v in baseline.items() if not k.endswith('_hashes')}, indent=2))
print('Protected hashes:', len(baseline['protected_hashes']))
