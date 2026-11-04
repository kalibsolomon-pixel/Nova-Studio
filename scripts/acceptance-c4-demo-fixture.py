"""Seed only the newly owned demo world's inventory with the five exported blocks.

This leaves demo mode, world rules, game mode, permissions and authentication alone.
Run only after Save and Quit has released the world. Preserve all original bytes.
"""
import gzip, io, json, pathlib, shutil, struct
root = pathlib.Path(__file__).resolve().parents[1]
world = root / 'build/studio-c4-acceptance-final/minecraft-1.21.11/saves/Demo_World'
out = root / 'docs/phase-2c4/acceptance-final/minecraft/demo-fixture'
out.mkdir(exist_ok=False)

class NBT:
    def __init__(self, data): self.stream = io.BytesIO(data)
    def scalar(self, fmt): return struct.unpack('>'+fmt, self.stream.read(struct.calcsize(fmt)))[0]
    def text(self): return self.stream.read(self.scalar('H')).decode('utf-8')
    def payload(self, kind):
        if kind in range(1, 7): return self.scalar({1:'b',2:'h',3:'i',4:'q',5:'f',6:'d'}[kind])
        if kind == 8: return self.text()
        if kind == 9:
            child, count = self.scalar('B'), self.scalar('i')
            return child, [self.payload(child) for _ in range(count)]
        if kind == 10:
            result = {}
            while (child := self.scalar('B')):
                name = self.text(); result[name] = (child, self.payload(child))
            return result
        if kind in (7, 11, 12):
            size = {7:1,11:4,12:8}[kind]; count = self.scalar('i')
            return self.stream.read(count*size)
        raise ValueError(kind)

def string(value):
    data = value.encode('utf-8'); return struct.pack('>H',len(data))+data
def payload(kind, value):
    if kind in range(1,7): return struct.pack('>'+{1:'b',2:'h',3:'i',4:'q',5:'f',6:'d'}[kind],value)
    if kind == 8: return string(value)
    if kind == 9:
        child, items = value
        return struct.pack('>Bi',child,len(items))+b''.join(payload(child,item) for item in items)
    if kind == 10:
        return b''.join(bytes([child])+string(name)+payload(child,item) for name,(child,item) in value.items())+b'\0'
    if kind in (7,11,12): return struct.pack('>i',len(value)//{7:1,11:4,12:8}[kind])+value
    raise ValueError(kind)

paths = [world/'level.dat', *list((world/'playerdata').glob('*.dat'))]
changes = []
for path in paths:
    original = gzip.decompress(path.read_bytes()); reader = NBT(original)
    kind, name = reader.scalar('B'), reader.text(); value = reader.payload(kind)
    assert bytes([kind])+string(name)+payload(kind,value) == original, 'Lossless round-trip required'
    player = value['Data'][1]['Player'][1] if path.name == 'level.dat' else value
    previous = player.get('Inventory')
    player['Inventory'] = (9, (10, [
        {'Slot': (1, slot), 'id': (8, 'minecraft:'+block), 'count': (3,64)}
        for slot,block in enumerate(('stone','oak_planks','bricks','gold_block','white_stained_glass'))]))
    player['SelectedItemSlot'] = (3,0)
    shutil.copy2(path, out/path.name)
    path.write_bytes(gzip.compress(bytes([kind])+string(name)+payload(kind,value)))
    changes.append({'file': str(path), 'previous_inventory': repr(previous),
                    'changed_fields':['Inventory','SelectedItemSlot']})
(out/'changes.json').write_text(json.dumps({'changes':changes,'demo_authentication_and_rules_unchanged':True},indent=2)+'\n')
print(json.dumps(changes,indent=2))
