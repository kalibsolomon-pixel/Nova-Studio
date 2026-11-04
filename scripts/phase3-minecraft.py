"""Prepare/launch only a copied owned offline 1.21.11 demo; no authentication changes."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
from datetime import datetime, timezone

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('operation', choices=['prepare', 'launch'])
p.add_argument('--seconds', type=int, default=150)
a = p.parse_args()
out = root / 'docs/phase-3/runtime'
instance = root / 'build/phase3-runtime/minecraft-1.21.11'
def put(base, relative, content):
    file = base / relative
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(json.dumps(content, indent=2)+'\n', encoding='utf-8')

if a.operation == 'prepare':
    out.mkdir(exist_ok=False)
    record = json.loads((root/'docs/phase-2c8/runtime/instance.json').read_text(encoding='utf-8'))
    source = pathlib.Path(record['instance']).resolve()
    assert source == (root/'build/studio-c8-runtime/minecraft-1.21.11').resolve()
    arguments = record['args']
    assert '--demo' in arguments
    assert arguments[arguments.index('--accessToken')+1] in ('0', 'none', 'null', '')
    instance.mkdir(parents=True, exist_ok=False)
    for name in ('natives', 'saves'):
        shutil.copytree(source/name, instance/name)
    authored = root/'build/phase3-runtime/authored-pack'
    authored.mkdir()
    put(authored, 'pack.mcmeta', {'pack': {'pack_format': 75, 'description': 'Original Nova Phase 3 fixture'}})
    environment = {**os.environ, 'PATH': str(root/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
    result = subprocess.run([str(root/'build/studio-phase3-release/tests/nova-c8-runtime-fixture.exe'), str(authored)],
                            cwd=root, env=environment, capture_output=True, timeout=60)
    (out/'authoring.log.txt').write_bytes(result.stdout+result.stderr)
    assert result.returncode == 0
    pack = instance/'resourcepacks/Nova-Phase3'
    for name in ('c8_preset', 'c8_custom'):
        file = authored/f'assets/nova/models/block/{name}.json'
        put(pack, f'assets/nova/models/block/{name}.json', json.loads(file.read_text(encoding='utf-8')))
    texture = pack/'assets/nova/textures/block/c8_check.png'
    texture.parent.mkdir(parents=True)
    shutil.copy2(authored/'assets/nova/textures/block/c8_check.png', texture)
    put(pack, 'assets/minecraft/models/block/bricks.json', {'parent': 'nova:block/c8_preset'})
    put(pack, 'assets/minecraft/models/block/stone.json', {'parent': 'nova:block/c8_custom'})
    animation = pack/'assets/minecraft/textures/block/diamond_block.png'
    animation.parent.mkdir(parents=True)
    for suffix in ('', '.mcmeta'):
        shutil.copy2(root/('fixtures/phase-2c5-models/assets/nova/textures/block/animated.png'+suffix),
                     pathlib.Path(str(animation)+suffix))
    put(pack, 'pack.mcmeta', {'pack': {'description': 'Original Nova Phase 3 runtime acceptance',
                                      'min_format': 75, 'max_format': 75}})
    replacements = {'resourcePacks': '["vanilla","file/Nova-Phase3"]', 'incompatibleResourcePacks': '[]', 'maxFps': '60'}
    options = (source/'options.txt').read_text(encoding='utf-8')
    (instance/'options.txt').write_text('\n'.join(
        line.split(':',1)[0]+':'+replacements[line.split(':',1)[0]]
        if line.split(':',1)[0] in replacements else line for line in options.splitlines())+'\n', encoding='utf-8')
    args = [v.replace(str(source), str(instance)) for v in arguments]
    assert pathlib.Path(args[args.index('--gameDir')+1]).resolve() == instance
    installed_jar = next(pathlib.Path(v) for v in args[args.index('-cp')+1].split(';')
                         if v.endswith('versions\\1.21.11\\1.21.11.jar') or v.endswith('versions/1.21.11/1.21.11.jar'))
    installed_sha1 = hashlib.file_digest(installed_jar.open('rb'), 'sha1').hexdigest()
    assert installed_sha1 == 'ba2df812c2d12e0219c489c4cd9a5e1f0760f5bd'
    manifest = {file.relative_to(pack).as_posix(): hashlib.sha256(file.read_bytes()).hexdigest()
                for file in sorted(pack.rglob('*')) if file.is_file()}
    (out/'instance.json').write_text(json.dumps(dict(java=record['java'], args=args, instance=str(instance),
        copied_from=str(source), version='1.21.11', installed_client_sha1=installed_sha1,
        pack_manifest=manifest, boundary='Official offline demo; no accounts, personal saves or world-rule edits'), indent=2)+'\n', encoding='utf-8')
    print(instance)
else:
    record = json.loads((out/'instance.json').read_text(encoding='utf-8'))
    assert pathlib.Path(record['instance']).resolve() == instance.resolve()
    args = record['args']
    assert '--demo' in args and args[args.index('--accessToken')+1] == '0'
    with (out/'stdout.txt').open('x') as stdout, (out/'stderr.txt').open('x') as stderr:
        child = subprocess.Popen([record['java'], *args, '--quickPlaySingleplayer', 'Demo_World'],
                                 cwd=instance, stdout=stdout, stderr=stderr,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        (out/'launch.json').write_text(json.dumps(dict(pid=child.pid, utc=datetime.now(timezone.utc).isoformat(),
            seconds=a.seconds, quick_play='Demo_World'), indent=2)+'\n', encoding='utf-8')
        print(child.pid, flush=True)
        try:
            code = child.wait(timeout=a.seconds)
            how = 'normal process exit during session'
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill', '/PID', str(child.pid)], capture_output=True)
            try:
                code = child.wait(timeout=60)
                how = 'normal close request after observation window'
            except subprocess.TimeoutExpired:
                how = 'close request timed out; owned process terminated'
                child.kill(); code = child.wait()
        shutil.copy2(instance/'logs/latest.log', out/'latest-game.log.txt')
        (out/'exit.json').write_text(json.dumps(dict(exit_code=code, how=how,
            utc=datetime.now(timezone.utc).isoformat()), indent=2)+'\n', encoding='utf-8')
        print(code, how)
