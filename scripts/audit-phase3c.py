"""Non-destructive Phase 3-C inventory and final preservation check."""
import gzip
import ast
import hashlib
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-3c'
out.mkdir(exist_ok=True)

def git(*args):
    return subprocess.check_output(['git', *args], cwd=root)

def saved_paths(kind):
    # Initial text inventories retain Git's C-quoted display form. Decode UTF-8
    # octal escapes for existence checks, including protected Unicode filenames.
    lines = gzip.decompress((out / f'{kind}-at-start.txt.gz').read_bytes()).decode().splitlines()
    return [ast.literal_eval('b' + p).decode('utf-8') if p.startswith('"') else p for p in lines]

if '--verify' in sys.argv:
    initial = json.loads((out / 'baseline-inventory.json').read_text())
    result = {}
    native_paths = {}
    for kind in ('tracked', 'untracked', 'ignored'):
        paths = saved_paths(kind)
        result[kind] = {'count': len(paths), 'missing': [p for p in paths if not (root / p).exists()]}
        native_paths[kind] = {
            'native_documents': [p for p in paths if p.lower().endswith(('.nvdoc', '.nvld', '.nvan'))],
            'recovery_paths': [p for p in paths if '.nova/' in p or 'recovery' in p.lower()]}
    # Derived from the original saved inventories, not a new initial snapshot.
    (out / 'initial-native-paths.json').write_text(json.dumps(native_paths, indent=2) + '\n')
    result['protected_changed'] = [p for p, h in initial['protected_sha256'].items()
                                   if not (root / p).is_file() or hashlib.sha256((root / p).read_bytes()).hexdigest() != h]
    result['deleted_diff'] = git('diff', '--name-only', '--diff-filter=D', initial['head']).decode().splitlines()
    (out / 'preservation.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    sys.exit(any(v['missing'] for k, v in result.items() if isinstance(v, dict)) or
             bool(result['protected_changed']) or bool(result['deleted_diff']))

if (out / 'baseline-inventory.json').exists():
    raise SystemExit('Initial inventory already exists; refusing to overwrite.')
paths_by_kind = {}
for kind, args in [('tracked', ['ls-files']), ('untracked', ['ls-files', '--others', '--exclude-standard']),
                   ('ignored', ['ls-files', '--others', '--ignored', '--exclude-standard'])]:
    paths = [p for p in git(*args, '-z').decode('utf-8').split('\0') if p]
    # These audit outputs were created by this phase, not pre-existing files.
    paths = [p for p in paths if not p.startswith('docs/phase-3c/') and p != 'scripts/audit-phase3c.py']
    paths_by_kind[kind] = paths
    (out / f'{kind}-at-start.txt.gz').write_bytes(gzip.compress(('\n'.join(paths)+'\n').encode(), mtime=0))
protected = [p for kind in ('tracked', 'untracked') for p in paths_by_kind[kind]
             if p.startswith('fixtures/') or p.lower().endswith(('.nvdoc', '.nvld', '.nvan'))]
protected += [p for p in paths_by_kind['ignored'] if p.startswith('fixtures/phase-2c5-models/.nova/')]
data = dict(branch=git('branch', '--show-current').decode().strip(), head=git('rev-parse', 'HEAD').decode().strip(),
            status='\n'.join(line for line in git('status', '--short').decode().splitlines()
                if not line[3:].startswith('docs/phase-3c/') and line[3:] != 'scripts/audit-phase3c.py') + '\n',
            counts={k: len(v) for k, v in paths_by_kind.items()},
            protected_sha256={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in protected if (root / p).is_file()},
            groups={name: [p for kind in ('tracked', 'untracked') for p in paths_by_kind[kind] if predicate(p)]
                    for name, predicate in [('native_documents', lambda p: p.lower().endswith(('.nvdoc', '.nvld', '.nvan'))),
                                            ('recovery', lambda p: '.nova/' in p or 'recovery' in p.lower()),
                                            ('screenshots', lambda p: p.startswith('docs/') and p.endswith('.png')),
                                            ('benchmarks', lambda p: 'bench' in p.lower() or '/performance' in p)]})
(out / 'baseline-inventory.json').write_text(json.dumps(data, indent=2) + '\n')
print(json.dumps({k: v for k, v in data.items() if k not in ('groups', 'protected_sha256')}, indent=2))
