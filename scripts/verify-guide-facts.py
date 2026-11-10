# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the Minecraft facts quoted by Nova's MCP guides against a local official client jar.

The jar is read in memory and nothing is extracted. The output holds only resource paths, numbers and
JSON field names (no textures, models or code), so it can be kept as a checked test fixture:

    python -I scripts/verify-guide-facts.py --jar <1.21.11 client.jar> --write tests/fixtures/guides/minecraft-1.21.11-facts.json
    python -I scripts/verify-guide-facts.py --jar <1.21.11 client.jar> --check tests/fixtures/guides/minecraft-1.21.11-facts.json
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zipfile

# Official client jar hashes, as recorded in docs/phase-2c4/official-spec-evidence.json.
OFFICIAL = {'1.21.11': 'ba2df812c2d12e0219c489c4cd9a5e1f0760f5bd'}


def facts(jar):
    archive = zipfile.ZipFile(jar)
    names = set(archive.namelist())

    def read(path):
        return json.loads(archive.read(path))

    def size(path):
        width, height = struct.unpack('>II', archive.read(path)[16:24])
        return [width, height]

    version = read('version.json')
    pack = version['pack_version']
    sword_item = read('assets/minecraft/items/diamond_sword.json')['model']
    chest_item = read('assets/minecraft/items/diamond_chestplate.json')['model']
    diamond = read('assets/minecraft/equipment/diamond.json')['layers']
    elytra = read('assets/minecraft/equipment/elytra.json')['layers']['wings'][0]
    trims = read('assets/minecraft/atlases/armor_trims.json')['sources'][0]
    item_atlas = read('assets/minecraft/atlases/items.json')['sources']
    item_trims = [s for s in item_atlas if s.get('type') == 'minecraft:paletted_permutations']
    trim_model = read('assets/minecraft/models/item/diamond_chestplate_quartz_trim.json')
    animations = {}
    for name in ('magma', 'sea_lantern', 'prismarine'):
        meta = read(f'assets/minecraft/textures/block/{name}.png.mcmeta')['animation']
        animations[name] = {'size': size(f'assets/minecraft/textures/block/{name}.png'),
                            'frametime': meta.get('frametime'), 'interpolate': meta.get('interpolate', False),
                            'frames': len(meta['frames']) if 'frames' in meta else None}
    object_frames = sized = 0
    frame_keys = set()
    for name in names:
        if name.startswith('assets/minecraft/textures/') and name.endswith('.png.mcmeta'):
            meta = json.loads(archive.read(name)).get('animation', {})
            entries = [f for f in meta.get('frames', []) if isinstance(f, dict)]
            object_frames += bool(entries)
            for entry in entries:
                frame_keys |= set(entry)
            sized += 'width' in meta or 'height' in meta
    wind = read('assets/minecraft/textures/entity/conduit/wind.png.mcmeta')['animation']
    animations['conduit_wind'] = {'size': size('assets/minecraft/textures/entity/conduit/wind.png'),
                                  'height': wind.get('height')}
    return {
        'version': version['id'],
        'packFormat': [pack['resource_major'], pack['resource_minor']],
        'itemDefinitions': {
            'folder': 'assets/minecraft/items/',
            'count': sum(n.startswith('assets/minecraft/items/') and n.endswith('.json') for n in names),
            'diamondSword': {'type': sword_item['type'], 'model': sword_item['model']},
            'diamondChestplate': {'type': chest_item['type'], 'property': chest_item['property'],
                                  'fallback': chest_item['fallback']['model']},
        },
        'models': {
            'diamondSword': read('assets/minecraft/models/item/diamond_sword.json'),
            'diamondChestplate': read('assets/minecraft/models/item/diamond_chestplate.json'),
            'generatedParent': read('assets/minecraft/models/item/generated.json')['parent'],
            'handheldParent': read('assets/minecraft/models/item/handheld.json')['parent'],
        },
        'textureSizes': {p: size('assets/minecraft/textures/' + p) for p in (
            'block/stone.png', 'item/diamond_sword.png', 'item/diamond_chestplate.png',
            'entity/equipment/humanoid/diamond.png', 'entity/equipment/humanoid_leggings/diamond.png',
            'trims/items/chestplate_trim.png', 'trims/entity/humanoid/coast.png',
            'trims/color_palettes/trim_palette.png', 'trims/color_palettes/diamond.png')},
        'equipment': {
            'folder': 'assets/minecraft/equipment/',
            'diamondLayers': sorted(diamond),
            'diamondTexture': diamond['humanoid'][0]['texture'],
            'wornTextures': ['assets/minecraft/textures/entity/equipment/humanoid/diamond.png',
                             'assets/minecraft/textures/entity/equipment/humanoid_leggings/diamond.png'],
            'elytraUsesPlayerTexture': elytra.get('use_player_texture', False),
        },
        'trims': {
            'atlas': 'assets/minecraft/atlases/armor_trims.json',
            'sourceType': trims['type'],
            'paletteKey': trims['palette_key'],
            'materials': sorted(trims['permutations']),
            'entityFolders': sorted({t.split('/')[-2] for t in trims['textures']}),
            'itemAtlasPalettedSources': len(item_trims),
            'itemAtlasTrimTextures': item_trims[0]['textures'],
            'itemAtlasPaletteKey': item_trims[0]['palette_key'],
            'itemTrimModelLayers': trim_model['textures'],
        },
        'blockstates': {'stoneModels': sorted({v['model'] for v in read(
            'assets/minecraft/blockstates/stone.json')['variants']['']})},
        'animations': animations,
        'animationMetadata': {'objectFrameEntries': object_frames, 'objectFrameKeys': sorted(frame_keys),
                              'explicitFrameSize': sized},
        'paths': sorted(p for p in (
            'assets/minecraft/items/diamond_sword.json', 'assets/minecraft/models/item/diamond_sword.json',
            'assets/minecraft/textures/item/diamond_sword.png', 'assets/minecraft/equipment/diamond.json',
            'assets/minecraft/textures/item/diamond_chestplate.png',
            'assets/minecraft/textures/entity/equipment/humanoid/diamond.png',
            'assets/minecraft/textures/entity/equipment/humanoid_leggings/diamond.png',
            'assets/minecraft/atlases/armor_trims.json',
            'assets/minecraft/textures/trims/color_palettes/trim_palette.png',
            'assets/minecraft/textures/trims/entity/humanoid/coast.png',
            'assets/minecraft/textures/trims/items/chestplate_trim.png',
            'assets/minecraft/textures/block/magma.png.mcmeta', 'assets/minecraft/blockstates/stone.json',
            'assets/minecraft/atlases/items.json') if p in names),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--jar', type=pathlib.Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--write', type=pathlib.Path)
    group.add_argument('--check', type=pathlib.Path)
    arguments = parser.parse_args()
    digest = hashlib.sha1(arguments.jar.read_bytes()).hexdigest()
    version = json.loads(zipfile.ZipFile(arguments.jar).read('version.json'))['id']
    if OFFICIAL.get(version) != digest:
        sys.exit(f'Not the official {version} client jar (SHA-1 {digest}).')
    result = {'source': {'version': version, 'clientSha1': digest}, 'facts': facts(arguments.jar)}
    text = json.dumps(result, indent=2, sort_keys=True) + '\n'
    if arguments.write:
        arguments.write.parent.mkdir(parents=True, exist_ok=True)
        arguments.write.write_text(text, encoding='utf-8', newline='\n')
        print(json.dumps({'written': str(arguments.write.name), 'version': version}))
    else:
        expected = arguments.check.read_text(encoding='utf-8')
        if expected != text:
            sys.exit('Guide facts differ from the checked fixture.')
        print(json.dumps({'checked': str(arguments.check.name), 'version': version, 'match': True}))


if __name__ == '__main__':
    main()
