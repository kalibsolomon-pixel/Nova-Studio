"""Verify cached official client artifacts; record hashes, not proprietary game contents."""
import hashlib
import json
import pathlib
import urllib.request
import zipfile
import re
import argparse

parser = argparse.ArgumentParser()
parser.add_argument("--output", default="docs/phase-2c4/official-spec-evidence.json")
args = parser.parse_args()

root = pathlib.Path(__file__).resolve().parents[1]
cache = root / ".tools/minecraft-spec"
cache.mkdir(exist_ok=True)
manifest_url = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
manifest = json.load(urllib.request.urlopen(manifest_url))
evidence = {"checked": "2026-10-28", "manifest": manifest_url, "versions": []}
for version in ("1.21.11", "26.1", "26.3"):
    entry = next(item for item in manifest["versions"] if item["id"] == version)
    version_data = json.load(urllib.request.urlopen(entry["url"]))
    client = version_data["downloads"]["client"]
    target = cache / f"{version}.jar"
    if not target.exists():
        urllib.request.urlretrieve(client["url"], target)
    assert hashlib.sha1(target.read_bytes()).hexdigest() == client["sha1"]
    record = {"id": version, "version_metadata": entry["url"], "client": client["url"],
              "sha1": client["sha1"], "shader_sha256": {}}
    with zipfile.ZipFile(target) as archive:
        for name in ("assets/minecraft/shaders/core/animate_sprite.vsh",
                     "assets/minecraft/shaders/core/animate_sprite_interpolate.fsh"):
            payload = archive.read(name)
            record["shader_sha256"][name] = hashlib.sha256(payload).hexdigest()
            if name.endswith(".fsh"):
                assert "mix(currentColor, nextColor, fAnimationProgress)" in payload.decode()
            else:
                assert "/ 1000.0" in payload.decode()
        class_name = "net/minecraft/client/resources/metadata/animation/AnimationMetadataSection.class"
        if version == "1.21.11":
            mappings = version_data["downloads"]["client_mappings"]
            mapping_file = cache / "1.21.11-mappings.txt"
            if not mapping_file.exists():
                urllib.request.urlretrieve(mappings["url"], mapping_file)
            mapping_text = mapping_file.read_text()
            mapped = re.search(r"^net.minecraft.client.resources.metadata.animation.AnimationMetadataSection -> (.*):$", mapping_text, re.MULTILINE)
            class_name = mapped[1].replace(".", "/") + ".class"
            record["client_mappings"] = mappings["url"]
            record["client_mappings_sha1"] = hashlib.sha1(mapping_file.read_bytes()).hexdigest()
        if class_name in archive.namelist():
            payload = archive.read(class_name)
            assert all(field.encode() in payload for field in ("frames", "width", "height", "frametime", "interpolate"))
            record["metadata_class_sha256"] = hashlib.sha256(payload).hexdigest()
    record["shader_observation"] = "RGBA mix; game-tick progress is encoded at 1/1000 precision"
    evidence["versions"].append(record)
output = root / args.output
output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
print(output)
