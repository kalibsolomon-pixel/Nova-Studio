"""Original deterministic RGBA fixtures. Does not replace an existing output folder."""
import argparse
import hashlib
import json
from pathlib import Path
import random
from PIL import Image, ImageDraw

SIZES = (16, 32, 64, 128, 256)
SEED = 20261027

def generate(output: Path, count: int, seed: int = SEED):
    output.mkdir(parents=True, exist_ok=False)
    rng = random.Random(seed)
    digest = hashlib.sha256()
    for i in range(count):
        size = SIZES[i % len(SIZES)]
        path = output / f"assets/nova_{i % 4}/textures/{('block', 'item', 'nature')[i % 3]}/set_{i % 16:02d}/texture_{i:05d}.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        # Flat regions, alpha and small repeating blocks; no game artwork.
        base = tuple(rng.randrange(40, 230) for _ in range(3))
        image = Image.new("RGBA", (size, size), (*base, 255))
        draw = ImageDraw.Draw(image)
        step = max(1, size // 8)
        for y in range(8):
            for x in range(8):
                color = tuple(min(255, c + ((x * 3 + y * 5 + i) % 5 - 2) * 12) for c in base)
                draw.rectangle((x*step, y*step, (x+1)*step-1, (y+1)*step-1), fill=(*color, 0 if (x+y+i) % 13 == 0 else 255))
        image.save(path, compress_level=6)
        rel = path.relative_to(output).as_posix()
        digest.update(rel.encode()); digest.update(path.read_bytes())
    (output / "pack.mcmeta").write_text(json.dumps({"pack": {"description": "Nova synthetic fixture; no Minecraft assets", "pack_format": 15}}), encoding="utf-8")
    manifest = {"schema": 1, "count": count, "seed": seed, "sizes": list(SIZES), "sha256": digest.hexdigest(), "generator": "nova-fixture-v1"}
    (output / "fixture.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--count", type=int, choices=(1500, 10000, 50000), default=1500)
    parser.add_argument("--seed", type=int, default=SEED)
    args = parser.parse_args()
    print(json.dumps(generate(args.output, args.count, args.seed)))
