"""Prepare one new owned demo instance using existing authorized C4 runtime files.
Never touches the live personal game, credentials, demo rules or historical worlds.
"""
import hashlib,json,pathlib,shutil
root=pathlib.Path(__file__).resolve().parents[1]
record=json.loads((root/'docs/phase-2c4/compatibility-final/autonomous/instance.json').read_text());args=record['args'];token=args[args.index('--accessToken')+1]
assert '--demo' in args and token in ('0','none','null',''), 'Only the existing offline demo fixture is authorized'
source=pathlib.Path(record['instance']).resolve();assert source.is_relative_to((root/'build').resolve())
instance=root/'build/studio-c6-runtime/minecraft-1.21.11';assert not instance.exists();instance.mkdir(parents=True)
for folder in ('natives','saves'):shutil.copytree(source/folder,instance/folder)
pack=instance/'resourcepacks/Nova-C6';shutil.copytree(root/'fixtures/phase-2c6-models',pack)
def write(path,obj):
 p=pack/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(obj,indent=2)+'\n')
write('pack.mcmeta',{'pack':{'min_format':75,'max_format':75,'description':'Original Nova C6 model comparison fixtures'}})
write('assets/minecraft/models/block/bricks.json',json.loads((pack/'assets/nova/models/block/dual.json').read_text()))
write('assets/minecraft/items/apple.json',{'model':{'type':'minecraft:model','model':'nova:item/silhouette','tints':[{'type':'minecraft:constant','value':16777215}]}})
options=(source/'options.txt').read_text();replacement={'resourcePacks':'["vanilla","file/Nova-C6"]','incompatibleResourcePacks':'[]','maxFps':'60'}
lines=[line.split(':',1)[0]+':'+replacement[line.split(':',1)[0]] if line.split(':',1)[0] in replacement else line for line in options.splitlines()];(instance/'options.txt').write_text('\n'.join(lines)+'\n')
new_args=[arg.replace(str(source),str(instance)) for arg in args];out=root/'docs/phase-2c6/runtime';out.mkdir()
(out/'instance.json').write_text(json.dumps({'java':record['java'],'instance':str(instance),'args':new_args,'version':'1.21.11','source':str(source),'credential_boundary':'Existing offline demo token only; no account access','world_changes':'Fresh copy only; demo time, game mode and authentication unchanged','pack_manifest':[{'path':p.relative_to(pack).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(pack.rglob('*')) if p.is_file()]},indent=2)+'\n')
print(instance)
