"""Original C5 test artwork and models. Never copies client assets or overwrites a fixture."""
import json
import pathlib
import struct
import zlib
root = pathlib.Path(__file__).resolve().parents[1]
pack = root / 'fixtures' / 'phase-2c5-models'
if pack.exists():
    raise SystemExit('Fixture already exists; preserved')

def write(path, value):
    file = pack / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

def png(path, color, animated=False):
    w, h = 16, 32 if animated else 16
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            c = color if (x // 4 + y // 4) % 2 else tuple(max(0, n - 55) for n in color)
            if x < 3 and y % 16 < 5: c = (255, 240, 100)
            if animated and y >= 16: c = (c[2], c[0], c[1])
            row.extend((*c, 255))
        rows.append(row)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    file = pack / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))

write('pack.mcmeta', {'pack': {'min_format': 84, 'max_format': 84, 'description': 'Nova C5 original model inspection fixtures'}})
faces = {name: {'texture': '#all'} for name in ('north', 'south', 'west', 'east', 'up', 'down')}
base = {'textures': {'all': 'nova:block/check'}, 'display': {'gui': {'rotation': [30, 225, 0], 'scale': [.625]*3}, 'ground': {'translation': [0, 3, 0], 'scale': [.25]*3}, 'on_shelf': {'scale': [.5]*3}}, 'elements': [{'name': 'Main block', 'from': [0,0,0], 'to': [16,16,16], 'faces': faces}]}
write('assets/nova/models/block/check.json', base)
write('assets/nova/models/block/inherited.json', {'parent':'nova:block/check', 'textures':{'all':'#skin', 'skin':'nova:block/alternate'}})
complex_model = json.loads(json.dumps(base))
complex_model['elements'] = [
    {'name':'Pedestal','from':[1,0,1],'to':[15,3,15],'faces':faces},
    {'name':'Column','from':[5,3,5],'to':[11,13,11],'rotation':{'origin':[8,8,8],'x':12,'y':25,'z':8},'faces':faces},
    {'name':'Cap','from':[2,13,2],'to':[14,16,14],'faces':{name:{'texture':'#all','uv':[0,0,16,8],'rotation':90} for name in faces}}]
write('assets/nova/models/block/complex.json', complex_model)
write('assets/nova/models/block/missing.json', {'parent':'nova:block/check', 'textures':{'all':'nova:block/missing'}})
write('assets/nova/models/block/cyclic.json', {'parent':'nova:block/cyclic'})
write('assets/nova/models/block/animated.json', {'parent':'nova:block/check', 'textures':{'all':'nova:block/animated'}})
write('assets/nova/models/item/generated.json', {'parent':'minecraft:builtin/generated','textures':{'layer0':'nova:item/gem'},'display':{'gui':{'rotation':[0,0,0]}}})
write('assets/nova/items/gem.json', {'model':{'type':'minecraft:model','model':'nova:item/generated'}})
write('assets/nova/items/dynamic.json', {'model':{'type':'minecraft:condition','property':'minecraft:using_item','on_true':{'type':'minecraft:model','model':'nova:block/complex'},'on_false':{'type':'minecraft:model','model':'nova:item/generated'}}})
write('assets/nova/blockstates/check.json', {'variants':{'facing=north':[{'model':'nova:block/check','weight':1},{'model':'nova:block/complex','weight':2}], 'facing=east':{'model':'nova:block/check','y':90}}})
write('assets/nova/blockstates/multipart.json', {'multipart':[{'apply':{'model':'nova:block/check'}},{'when':{'powered':'true'},'apply':{'model':'nova:block/complex'}}]})
png('assets/nova/textures/block/check.png',(80,170,220))
png('assets/nova/textures/block/alternate.png',(190,110,220))
png('assets/nova/textures/item/gem.png',(70,210,170))
png('assets/nova/textures/block/animated.png',(210,110,90),True)
write('assets/nova/textures/block/animated.png.mcmeta',{'animation':{'frametime':4}})
(pack/'LICENSE.txt').write_text('All artwork and model fixtures in this directory are original procedural Nova Studio test material, dedicated to the public domain under CC0-1.0. No Minecraft or MCreator artwork is included.\n')
print(pack)
