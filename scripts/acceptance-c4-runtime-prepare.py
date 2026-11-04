"""Prepare a separate runtime case from already installed, read-only client files."""
import hashlib, json, pathlib, shutil

root = pathlib.Path(__file__).resolve().parents[1]
old = json.loads((root / 'docs/phase-2c4/acceptance/minecraft/instance.json').read_text())
work = root / 'build/studio-c4-acceptance-final'
instance = work / 'minecraft-1.21.11'
instance.mkdir(exist_ok=False)
(instance / 'resourcepacks').mkdir()
shutil.copytree(pathlib.Path(old['instance']) / 'natives', instance / 'natives')
shutil.copy2(pathlib.Path(old['instance']) / 'options.txt', instance / 'options.txt')
source = work / 'F-export-source'
shutil.copytree(root / 'docs/phase-2c4/acceptance/fixtures/packs/1.21.11', source)
arguments = [arg.replace(old['instance'], str(instance)) for arg in old['args']]
record = {**old, 'instance': str(instance), 'args': arguments,
          'pack': str(instance / 'resourcepacks/Nova-C4-AE'), 'source': str(source),
          'preparation': 'Copies only owned prepared natives/options and original exported A-E fixtures; installed client, libraries and assets remain read-only.',
          'java_sha256': hashlib.sha256(pathlib.Path(old['java']).read_bytes()).hexdigest()}
out = root / 'docs/phase-2c4/acceptance-final/minecraft'
out.mkdir()
(out / 'instance.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps({k: record[k] for k in ('instance', 'source', 'pack', 'java_sha256')}, indent=2))
