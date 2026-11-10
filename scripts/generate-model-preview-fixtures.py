"""Original model-preview fixtures: an editable pack plus a lower-priority resource-stack
folder with synthetic parents shaped like common Minecraft ones (block/block, block/cube,
block/cube_all, item/generated, item/handheld). Every JSON value and pixel here is written
by this script; nothing is copied from the Minecraft client. Never overwrites a fixture."""
import json
import pathlib
import struct
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
base = root / 'fixtures' / 'model-preview'
if base.exists():
    raise SystemExit('Fixture already exists; preserved')


def write(path, value):
    file = base / path
    file.parent.mkdir(parents=True, exist_ok=True)
    text = value if isinstance(value, str) else json.dumps(value, indent=2) + '\n'
    file.write_text(text, encoding='utf-8', newline='\n')


def png(path, color, mark=(255, 240, 100), sprite=False):
    """16x16 checker in `color`, a corner mark for orientation; `sprite` keeps a diamond only."""
    rows = []
    for y in range(16):
        row = bytearray([0])
        for x in range(16):
            c = color if (x // 4 + y // 4) % 2 else tuple(max(0, n - 60) for n in color)
            if x < 4 and y < 4:
                c = mark
            alpha = 255
            if sprite and abs(x - 7.5) + abs(y - 7.5) > 7:
                c, alpha = (0, 0, 0), 0
            row.extend((*c, alpha))
        rows.append(row)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    file = base / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


write('LICENSE.txt', 'All fixture artwork and JSON are original procedural Nova Studio test material, '
      'CC0-1.0. The stack/ folder imitates the shape of common Minecraft parent models with '
      'values chosen for these tests; no Minecraft client asset is included.\n')
mcmeta = {'pack': {'min_format': 84, 'max_format': 84, 'description': 'Nova model-preview fixtures'}}
write('pack/pack.mcmeta', mcmeta)
write('stack/pack.mcmeta', {'pack': {'min_format': 84, 'max_format': 84,
                                     'description': 'Synthetic lower-priority parents'}})

# ---- stack: synthetic parents and a model the pack overrides
M = 'stack/assets/minecraft/models/'
write(M + 'block/block.json', {
    'gui_light': 'side',
    'display': {
        'gui': {'rotation': [30, 225, 0], 'translation': [0, 0, 0], 'scale': [0.6, 0.6, 0.6]},
        'ground': {'rotation': [0, 0, 0], 'translation': [0, 3, 0], 'scale': [0.25, 0.25, 0.25]},
        'thirdperson_righthand': {'rotation': [75, 45, 0], 'translation': [0, 2.5, 0], 'scale': [0.4, 0.4, 0.4]},
        'firstperson_righthand': {'rotation': [0, 45, 0], 'translation': [0, 0, 0], 'scale': [0.4, 0.4, 0.4]}}})
faces = {}
for name in ('down', 'up', 'north', 'south', 'west', 'east'):
    faces[name] = {'texture': '#' + name, 'cullface': name}
write(M + 'block/cube.json', {'parent': 'minecraft:block/block',
                              'elements': [{'from': [0, 0, 0], 'to': [16, 16, 16], 'faces': faces}]})
write(M + 'block/cube_all.json', {'parent': 'minecraft:block/cube', 'textures': {
    'particle': '#all', 'down': '#all', 'up': '#all', 'north': '#all', 'south': '#all', 'west': '#all', 'east': '#all'}})
write(M + 'item/generated.json', {'parent': 'builtin/generated', 'gui_light': 'front', 'display': {
    'gui': {'rotation': [0, 0, 0], 'translation': [0, 0, 0], 'scale': [1, 1, 1]},
    'ground': {'rotation': [0, 0, 0], 'translation': [0, 2, 0], 'scale': [0.5, 0.5, 0.5]},
    'thirdperson_righthand': {'rotation': [0, 0, 0], 'translation': [0, 3, 1], 'scale': [0.55, 0.55, 0.55]},
    'firstperson_righthand': {'rotation': [0, -90, 25], 'translation': [1.1, 3.2, 1.1], 'scale': [0.68, 0.68, 0.68]}}})
write(M + 'item/handheld.json', {'parent': 'minecraft:item/generated', 'display': {
    'thirdperson_righthand': {'rotation': [0, -90, 50], 'translation': [0, 4, 0.5], 'scale': [0.85, 0.85, 0.85]},
    'firstperson_righthand': {'rotation': [0, -90, 25], 'translation': [1.13, 3.2, 1.13], 'scale': [0.68, 0.68, 0.68]}}})
write(M + 'block/fx_override.json', {'parent': 'minecraft:block/cube_all',
                                     'textures': {'all': 'minecraft:block/fx_override'}})

# ---- pack: models and textures under test
P = 'pack/assets/minecraft/'
write(P + 'models/block/fx_cube.json', {'parent': 'minecraft:block/cube_all',
                                        'textures': {'all': 'minecraft:block/fx_cube'}})
side = {'uv': [0, 8, 16, 16], 'texture': '#side'}
write(P + 'models/block/fx_slab.json', {
    'parent': 'minecraft:block/block',
    'textures': {'side': 'minecraft:block/fx_slab', 'top': '#side', 'particle': '#side'},
    'elements': [
        {'from': [0, 0, 0], 'to': [16, 8, 16], 'faces': {
            'down': {'uv': [0, 0, 16, 16], 'texture': '#top', 'cullface': 'down'},
            'up': {'uv': [0, 0, 16, 16], 'texture': '#top', 'rotation': 90},
            'north': dict(side, cullface='north'), 'south': dict(side, cullface='south'),
            'west': dict(side, cullface='west'), 'east': dict(side, cullface='east')}},
        {'from': [6, 8, 6], 'to': [10, 14, 10], 'rotation': {'origin': [8, 8, 8], 'axis': 'y', 'angle': 22.5},
         'faces': {'north': {'uv': [6, 2, 10, 8], 'texture': '#side'},
                   'east': {'uv': [6, 2, 10, 8], 'texture': '#side'},
                   'up': {'uv': [6, 6, 10, 10], 'texture': '#top'}}}]})
write(P + 'models/block/fx_override.json', {
    'parent': 'minecraft:block/block', 'textures': {'all': 'minecraft:block/fx_override'},
    'elements': [{'from': [0, 0, 0], 'to': [16, 4, 16], 'faces': {
        n: {'texture': '#all'} for n in ('down', 'up', 'north', 'south', 'west', 'east')}}]})
write(P + 'models/item/fx_gem.json', {'parent': 'minecraft:item/generated',
                                      'textures': {'layer0': 'minecraft:item/fx_gem'}})
write(P + 'models/item/fx_sword.json', {'parent': 'minecraft:item/handheld',
                                        'textures': {'layer0': 'minecraft:item/fx_sword'}})
write(P + 'models/block/fx_broken.json', '{"parent": "minecraft:block/cube_all", "textures": {\n')
write(P + 'models/block/fx_cycle.json', {'parent': 'minecraft:block/fx_cycle_b',
                                         'textures': {'all': 'minecraft:block/fx_cycle'}})
write(P + 'models/block/fx_cycle_b.json', {'parent': 'minecraft:block/fx_cycle'})
write(P + 'models/block/fx_lost.json', {'parent': 'minecraft:block/fx_no_such_parent',
                                        'textures': {'all': 'minecraft:block/fx_lost'}})
png(P + 'textures/block/fx_cube.png', (90, 140, 210))
png(P + 'textures/block/fx_slab.png', (200, 120, 60))
png(P + 'textures/block/fx_override.png', (120, 190, 90))
png(P + 'textures/block/fx_orphan.png', (180, 90, 180))
png(P + 'textures/block/fx_broken.png', (210, 70, 70))
png(P + 'textures/block/fx_cycle.png', (70, 170, 170))
png(P + 'textures/block/fx_lost.png', (160, 160, 60))
png(P + 'textures/item/fx_gem.png', (60, 200, 160), sprite=True)
png(P + 'textures/item/fx_sword.png', (190, 190, 210), sprite=True)
