"""Read-only verification of cached official Minecraft identities and build provenance."""
import hashlib
import json
import pathlib
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
evidence = json.loads((root/'docs/phase-2c4/official-spec-evidence.json').read_text(encoding='utf-8'))
rows = []
for record in evidence['versions']:
    version = record['id']
    file = root/'.tools/minecraft-spec'/(version+'.jar')
    actual = hashlib.file_digest(file.open('rb'), 'sha1').hexdigest()
    assert actual == record['sha1'], (version, actual)
    with zipfile.ZipFile(file) as archive:
        entries = archive.namelist()
        rows.append(dict(version=version, client_sha1=actual,
            official_client_url=record['client'],
            resources=sum(name.startswith('assets/minecraft/') for name in entries),
            boundary='Cached official-artifact identity; not a runtime or rendering-parity test'))
out = root/'docs/phase-3/official-artifact-recheck.json'
assert not out.exists()
out.write_text(json.dumps(dict(versions=rows, game_artifacts_bundled=False), indent=2)+'\n', encoding='utf-8')
print(json.dumps(rows, indent=2))
