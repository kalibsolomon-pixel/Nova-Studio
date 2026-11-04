"""Fresh copy of the already-authorized offline demo; original instances stay unchanged."""
import hashlib,json,pathlib,shutil
root=pathlib.Path(__file__).resolve().parents[1]
record=json.loads((root/'docs/phase-2c6/runtime/instance.json').read_text())
args=record['args'];assert '--demo' in args and args[args.index('--accessToken')+1] in ('0','none','null','')
source=pathlib.Path(record['instance']).resolve();assert source==(root/'build/studio-c6-runtime/minecraft-1.21.11').resolve()
instance=root/'build/studio-c7-runtime/minecraft-1.21.11';assert not instance.exists();instance.mkdir(parents=True)
for name in ('natives','saves'):shutil.copytree(source/name,instance/name)
pack=instance/'resourcepacks/Nova-C7';pack.mkdir(parents=True)
def put(relative,value):
 p=pack/relative;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(value,indent=2)+'\n')
authored=root/'build/studio-c7-qa/pack'
put('pack.mcmeta',{'pack':{'description':'Original Nova C7 authored model runtime comparison','min_format':75,'max_format':75}})
put('assets/nova/models/block/authored.json',json.loads((authored/'assets/nova/models/block/new_model.json').read_text()))
put('assets/minecraft/models/block/bricks.json',{'parent':'nova:block/authored'})
for texture in ('check.png','new_model.png'):
 p=pack/'assets/nova/textures/block'/texture;p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(authored/'assets/nova/textures/block'/texture,p)
options=(source/'options.txt').read_text();replacements={'resourcePacks':'["vanilla","file/Nova-C7"]','incompatibleResourcePacks':'[]','maxFps':'60'}
(instance/'options.txt').write_text('\n'.join(line.split(':',1)[0]+':'+replacements[line.split(':',1)[0]] if line.split(':',1)[0] in replacements else line for line in options.splitlines())+'\n')
out=root/'docs/phase-2c7/runtime';out.mkdir(exist_ok=False)
new_args=[v.replace(str(source),str(instance)) for v in args]
assert pathlib.Path(new_args[new_args.index('--gameDir')+1]).resolve()==instance
manifest=[{'path':p.relative_to(pack).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(pack.rglob('*')) if p.is_file()]
(out/'instance.json').write_text(json.dumps({'java':record['java'],'args':new_args,'instance':str(instance),'source':str(source),'version':'1.21.11','pack_manifest':manifest,'boundary':'Copied natives/world/options only; dummy offline demo identity, no account access or world rule/time edits'},indent=2)+'\n')
print(instance)
