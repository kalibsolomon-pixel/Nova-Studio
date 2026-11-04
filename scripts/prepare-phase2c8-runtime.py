"""Fresh owned copy of the already-authorized offline 1.21.11 demo instance for C8.

Copies natives/saves/options from the C7 instance (never the personal game), and builds a
Nova-C8 pack from models authored in Nova during native acceptance:
  bricks -> nova:block/c8_preset   (Nova cube template with its display preset)
  stone  -> nova:block/c8_custom   (display transforms edited in Nova's display editor)
"""
import hashlib, json, pathlib, shutil
root = pathlib.Path(__file__).resolve().parents[1]
record = json.loads((root / 'docs/phase-2c7/runtime/instance.json').read_text())
args = record['args']
assert '--demo' in args and args[args.index('--accessToken') + 1] in ('0', 'none', 'null', '')
source = pathlib.Path(record['instance']).resolve()
assert source == (root / 'build/studio-c7-runtime/minecraft-1.21.11').resolve()
instance = root / 'build/studio-c8-runtime/minecraft-1.21.11'
assert not instance.exists()
instance.mkdir(parents=True)
for name in ('natives', 'saves'):
    shutil.copytree(source / name, instance / name)
authored = root / 'build/studio-c8-qa/pack'
pack = instance / 'resourcepacks/Nova-C8'
def put(relative, value):
    p = pack / relative
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(value, indent=2) + '\n')
put('pack.mcmeta', {'pack': {'description': 'Original Nova C8 display-transform runtime comparison',
                             'min_format': 75, 'max_format': 75}})
textures = set()
for model in ('c8_preset', 'c8_custom'):
    data = json.loads((authored / f'assets/nova/models/block/{model}.json').read_text())
    put(f'assets/nova/models/block/{model}.json', data)
    for value in data.get('textures', {}).values():
        if isinstance(value, str) and not value.startswith('#'):
            textures.add(value)
for texture in sorted(textures):
    namespace, path = texture.split(':', 1) if ':' in texture else ('minecraft', texture)
    if namespace != 'nova':
        continue
    src = authored / f'assets/nova/textures/{path}.png'
    dst = pack / f'assets/nova/textures/{path}.png'
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
put('assets/minecraft/models/block/bricks.json', {'parent': 'nova:block/c8_preset'})
put('assets/minecraft/models/block/stone.json', {'parent': 'nova:block/c8_custom'})
options = (source / 'options.txt').read_text()
replacements = {'resourcePacks': '["vanilla","file/Nova-C8"]', 'incompatibleResourcePacks': '[]', 'maxFps': '60'}
(instance / 'options.txt').write_text('\n'.join(
    line.split(':', 1)[0] + ':' + replacements[line.split(':', 1)[0]] if line.split(':', 1)[0] in replacements else line
    for line in options.splitlines()) + '\n')
out = root / 'docs/phase-2c8/runtime'
out.mkdir(exist_ok=False)
new_args = [v.replace(str(source), str(instance)) for v in args]
assert pathlib.Path(new_args[new_args.index('--gameDir') + 1]).resolve() == instance
manifest = [{'path': p.relative_to(pack).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(pack.rglob('*')) if p.is_file()]
(out / 'instance.json').write_text(json.dumps({
    'java': record['java'], 'args': new_args, 'instance': str(instance), 'source': str(source),
    'version': '1.21.11', 'pack_manifest': manifest,
    'boundary': 'Copied natives/world/options only; dummy offline demo identity, no account access or world rule/time edits'},
    indent=2) + '\n')
print(instance)
