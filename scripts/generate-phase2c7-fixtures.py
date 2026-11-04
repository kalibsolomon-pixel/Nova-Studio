"""Original authoring/diagnostic fixtures; refuses to replace any prior fixture."""
import json, pathlib, shutil
from PIL import Image
root=pathlib.Path(__file__).resolve().parents[1]
fixture=root/'fixtures/phase-2c7-authoring'
assert not fixture.exists()
def put(path,value):
 p=fixture/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(value,indent=2)+'\n')
put('pack.mcmeta',{'pack':{'description':'Original Nova C7 authoring fixture','min_format':[97,1],'max_format':[97,1]}})
faces={d:{'texture':'#all'} for d in ('down','up','north','south','west','east')}
faces['south']['custom_face']='preserved'
model={'credit':'Original Nova Studio test geometry and texture','custom_root':{'preserve':True},
 'textures':{'all':'nova:block/check','particle':'nova:block/check'},
 'elements':[{'name':'Main cuboid','from':[2,0,2],'to':[14,14,14],'shade':False,'faces':faces}]}
put('assets/nova/models/block/edit_me.json',model)
put('assets/nova/models/block/broken_texture.json',{'textures':{'all':'nova:misplaced/check'},'elements':[{'from':[0,0,0],'to':[16,16,16],'faces':faces}]})
put('assets/nova/blockstates/broken.json',{'variants':{'':{'model':'nova:block/absent'}}})
put('assets/nova/items/edit_me.json',{'model':{'type':'minecraft:model','model':'nova:block/edit_me'}})
p=fixture/'assets/nova/textures/block/check.png';p.parent.mkdir(parents=True,exist_ok=True)
image=Image.new('RGBA',(32,16))
for y in range(16):
 for x in range(32):image.putpixel((x,y),(90,145,155,255) if (x//4+y//4)%2 else (190,210,195,255))
image.save(p)
(fixture/'LICENSE.txt').write_text('Original Nova Studio C7 fixture geometry and procedural texture. CC0-1.0. No Minecraft artwork.\n')
working=root/'build/studio-c7-qa/pack';assert not working.exists();shutil.copytree(fixture,working)
print(working)
