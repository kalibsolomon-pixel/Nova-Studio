"""Original deterministic C6 fixtures; never modifies the historical C5 pack."""
import json,pathlib,shutil
from PIL import Image
root=pathlib.Path(__file__).resolve().parents[1];out=root/'fixtures/phase-2c6-models'
assert not out.exists(),out
shutil.copytree(root/'fixtures/phase-2c5-models',out,ignore=shutil.ignore_patterns('.nova'))
def write(path,obj):
 p=out/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(obj,indent=2)+'\n')
def texture(path,colors):
 image=Image.new('RGBA',(16,16*len(colors)))
 for n,color in enumerate(colors):
  for y in range(16):
   for x in range(16):image.putpixel((x,n*16+y),color if (x//4+y//4)%2 else tuple(min(255,c+30) for c in color[:3])+(color[3],))
 image.save(out/path)
write('assets/nova/models/block/vanilla.json',{'parent':'minecraft:block/cube_all','textures':{'all':'nova:block/check'},'nova_note':'Editable child; geometry supplied by explicit vanilla fallback'})
base=json.loads((out/'assets/nova/models/block/check.json').read_text());base['nova_extension']={'keep':'unknown metadata'};base['elements'][0]['faces']['south']['nova_face']='keep';write('assets/nova/models/block/uv.json',base)
texture('assets/nova/textures/block/material_a.png',[(230,70,90,255),(80,150,240,255),(230,180,50,255)])
texture('assets/nova/textures/block/material_b.png',[(60,220,140,255),(180,80,220,255)])
write('assets/nova/textures/block/material_a.png.mcmeta',{'animation':{'frametime':2,'frames':[0,{'index':2,'time':5},1]}})
write('assets/nova/textures/block/material_b.png.mcmeta',{'animation':{'frametime':7,'interpolate':True}})
dual=json.loads(json.dumps(base));dual['textures']={'a':'nova:block/material_a','b':'nova:block/material_b'}
dual['elements']=[{'name':'Material A','from':[0,0,0],'to':[8,16,16],'faces':{d:{'texture':'#a'} for d in ('up','down','north','south','east','west')}},{'name':'Material B','from':[8,0,0],'to':[16,16,16],'faces':{d:{'texture':'#b'} for d in ('up','down','north','south','east','west')}}];write('assets/nova/models/block/dual.json',dual)
image=Image.new('RGBA',(16,16))
for y in range(16):
 for x in range(16):
  if abs(x-7.5)+abs(y-7.5)<=7 and not (6<=x<=9 and 6<=y<=9):image.putpixel((x,y),(80+x*10,200-y*6,220,255))
image.save(out/'assets/nova/textures/item/silhouette.png')
write('assets/nova/models/item/silhouette.json',{'parent':'minecraft:builtin/generated','textures':{'layer0':'nova:item/silhouette'},'display':{'gui':{'rotation':[0,25,0]},'ground':{'rotation':[0,0,90],'scale':[.5,.5,.5]}}})
write('assets/nova/items/silhouette.json',{'model':{'type':'minecraft:model','model':'nova:item/silhouette','tints':[{'type':'minecraft:constant','value':16777215}]}})
tinted=json.loads(json.dumps(base))
for face in tinted['elements'][0]['faces'].values():face['tintindex']=0
write('assets/nova/models/block/tinted.json',tinted)
write('assets/nova/items/transformed.json',{'model':{'type':'minecraft:composite','transformation':{'translation':[.15,0,0]},'models':[{'type':'minecraft:model','model':'nova:block/tinted','transformation':{'scale':[.6,.6,.6]},'tints':[{'type':'minecraft:constant','value':6736998}]},{'type':'minecraft:model','model':'nova:item/silhouette','transformation':{'translation':[.65,0,0]}}]}})
(out/'LICENSE.txt').write_text('All fixture artwork and JSON are original procedural Nova Studio test material, CC0-1.0. No vanilla client artwork is included. C5 fixtures are copied, not modified.\n')
print(out)
