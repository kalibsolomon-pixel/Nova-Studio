"""Verify the GUI-exported disposable pack and launch its supervised demo client."""
import hashlib, json, pathlib, shutil, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-2c4/acceptance-final/minecraft'
record = json.loads((out / 'instance.json').read_text())
pack, source = pathlib.Path(record['pack']), pathlib.Path(record['source'])
files = sorted(p for p in pack.rglob('*') if p.is_file())
assert len(files) == 11 and not (pack / '.nova').exists()
manifest = []
for path in files:
    relative = path.relative_to(pack)
    assert path.read_bytes() == (source / relative).read_bytes(), relative
    manifest.append({'path': relative.as_posix(), 'bytes': path.stat().st_size,
                     'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
assert not list(source.glob('.nova/animation-exports/*/journal.json'))
shutil.copytree(pack, out / 'exported-pack')
(out / 'pack-verification.json').write_text(json.dumps({'match_original_exported_fixtures': True,
    'editor_metadata_absent': True, 'pending_pair_transactions': [], 'files': manifest}, indent=2)+'\n')
subprocess.run([sys.executable, str(root / 'scripts/acceptance-c4-supervise.py'), '--case',
    'MC-1_21_11', '--exe', record['java'], '--', *record['args']], cwd=root, check=True)
