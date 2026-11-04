"""Phase 3-D live test pack: visible overrides of vanilla 1.21.11 assets."""
import json
import pathlib
import sys

from PIL import Image

root = pathlib.Path(sys.argv[1])
if root.exists():
    raise SystemExit(f"refusing to overwrite {root}")
tex = root / 'assets/minecraft/textures'
models = root / 'assets/minecraft/models'


def put_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def checker(a, b, size=16, cell=4):
    image = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            image.putpixel((x, y), a if ((x // cell) + (y // cell)) % 2 == 0 else b)
    return image


put_json(root / 'pack.mcmeta', {
    'pack': {'description': 'Nova Studio Phase 3-D live test', 'min_format': [75, 0], 'max_format': [75, 0]},
    'x-nova-unknown-field': {'preserved': True},
})
# 1. Terrain texture: magenta/black checker stone (seen everywhere underground and in cliffs).
(tex / 'block').mkdir(parents=True)
checker((230, 40, 200, 255), (20, 20, 20, 255)).save(tex / 'block/stone.png')
# 2. Animated texture: 4 frames of solid colours on the diamond block.
frames = Image.new('RGBA', (16, 64))
for i, colour in enumerate([(255, 60, 60, 255), (60, 255, 60, 255), (60, 120, 255, 255), (255, 230, 40, 255)]):
    frames.paste(checker(colour, (255, 255, 255, 255), cell=8), (0, i * 16))
frames.save(tex / 'block/diamond_block.png')
put_json(tex / 'block/diamond_block.png.mcmeta', {'animation': {'frametime': 20}})
# 3. Model with a rotated element and per-face UVs: crafting table becomes a slab with a 22.5° post.
checker((250, 150, 20, 255), (90, 50, 10, 255), cell=2).save(tex / 'block/nova_live_post.png')
put_json(models / 'block/crafting_table.json', {
    'parent': 'block/block',
    'textures': {'particle': 'block/nova_live_post', 'post': 'block/nova_live_post', 'top': 'block/crafting_table_top'},
    'elements': [
        {'from': [0, 0, 0], 'to': [16, 8, 16],
         'faces': {f: {'uv': [0, 8, 16, 16] if f not in ('up', 'down') else [0, 0, 16, 16],
                       'texture': '#top' if f == 'up' else '#post'} for f in ('north', 'south', 'east', 'west', 'up', 'down')}},
        {'from': [6, 8, 6], 'to': [10, 16, 10],
         'rotation': {'origin': [8, 8, 8], 'axis': 'y', 'angle': 22.5},
         'faces': {f: {'uv': [0, 0, 4, 8], 'texture': '#post'} for f in ('north', 'south', 'east', 'west', 'up')}},
    ],
})
# 4. Item display transforms + generated item geometry: a huge red stick in first person.
(tex / 'item').mkdir(parents=True)
stick = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
for i in range(12):
    stick.putpixel((2 + i, 13 - i), (220, 30, 30, 255))
    stick.putpixel((3 + i, 13 - i), (120, 10, 10, 255))
stick.save(tex / 'item/stick.png')
put_json(models / 'item/stick.json', {
    'parent': 'item/handheld',
    'textures': {'layer0': 'item/stick'},
    'display': {'firstperson_righthand': {'rotation': [0, -90, 25], 'translation': [1.13, 3.2, 1.13], 'scale': [1.6, 1.6, 1.6]}},
    'x-nova-unknown-field': 1,
})
print('wrote', root)
