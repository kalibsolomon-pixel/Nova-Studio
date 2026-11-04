"""Prepare a disposable official 1.21.11 demo instance using installed read-only artifacts.

No launcher-account files or personal authentication tokens are read. Run with --launch
to start the actual client; pack parsing alone is not a runtime animation assertion.
"""
import hashlib, json, os, pathlib, shutil, subprocess, sys, zipfile

root = pathlib.Path(__file__).resolve().parents[1]
installed = pathlib.Path(os.environ['APPDATA']) / '.minecraft'
version = '1.21.11'
meta_path = installed / f'versions/{version}/{version}.json'
meta = json.loads(meta_path.read_text())
instance = root / 'build/studio-c4-acceptance/minecraft-1.21.11'
out = root / 'docs/phase-2c4/acceptance/minecraft'
instance.mkdir(parents=True, exist_ok=True); out.mkdir(exist_ok=True)
natives = instance / 'natives'; natives.mkdir(exist_ok=True)
libraries, required, missing = [], [], []
for lib in meta['libraries']:
    allowed = not lib.get('rules')
    for rule in lib.get('rules', []):
        system = rule.get('os', {})
        if system.get('name', 'windows') != 'windows': continue
        if system.get('arch', 'x86_64') not in ('x86_64', 'amd64'): continue
        allowed = rule['action'] == 'allow'
    if not allowed: continue
    artifact = lib.get('downloads', {}).get('artifact')
    if not artifact: continue
    path = installed / 'libraries' / artifact['path']
    required.append(str(path))
    if not path.exists(): missing.append(str(path)); continue
    if hashlib.sha1(path.read_bytes()).hexdigest() != artifact['sha1']: raise RuntimeError('Installed artifact hash mismatch: ' + str(path))
    libraries.append(str(path))
    if 'natives-windows' in path.name:
        with zipfile.ZipFile(path) as archive:
            for member in archive.namelist():
                if member.lower().endswith('.dll'):
                    (natives / pathlib.PurePosixPath(member).name).write_bytes(archive.read(member))
client = installed / f'versions/{version}/{version}.jar'
client_hash = hashlib.sha1(client.read_bytes()).hexdigest()
assert client_hash == meta['downloads']['client']['sha1']
libraries.append(str(client))
pack = instance / 'resourcepacks/Nova-C4-AE'
if not pack.exists(): shutil.copytree(root / 'docs/phase-2c4/acceptance/fixtures/packs/1.21.11', pack)
options = instance / 'options.txt'
if not options.exists(): options.write_text('resourcePacks:["vanilla","file/Nova-C4-AE"]\nfullscreen:false\nrenderDistance:4\nsimulationDistance:4\nmaxFps:60\n')
java = pathlib.Path(r'C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot\bin\java.exe')
arguments = ['-Xms256M', '-Xmx2G', '-Djava.library.path=' + str(natives), '-cp', os.pathsep.join(libraries), meta['mainClass'],
             '--username', 'NovaAcceptance', '--version', version, '--gameDir', str(instance), '--assetsDir', str(installed / 'assets'),
             '--assetIndex', meta['assetIndex']['id'], '--uuid', '000000000000400080000000000000c4', '--accessToken', '0', '--userType', 'legacy',
             '--versionType', 'release', '--demo', '--width', '1100', '--height', '700']
report = {'version': version, 'instance': str(instance), 'mode': 'official client demo with placeholder token, no personal credentials',
          'java': str(java), 'client_sha1': client_hash, 'required_libraries': required, 'missing_required_libraries': missing,
          'pack': str(pack), 'pack_format': [75, 0], 'args': arguments}
if '--launch' in sys.argv:
    assert not missing, missing
    log = (out / 'client-console.txt').open('wb')
    child = subprocess.Popen([str(java), *arguments], cwd=instance, stdout=log, stderr=subprocess.STDOUT)
    report['pid'] = child.pid
(out / 'instance.json').write_text(json.dumps(report, indent=2))
print(json.dumps({key: value for key, value in report.items() if key not in ('args', 'required_libraries')}, indent=2))
