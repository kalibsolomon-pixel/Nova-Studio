"""Seed original C6 comparison items after normal Save and Quit to Title.

Only Inventory/SelectedItemSlot change in the owned copy. Every other NBT field,
including demo time, game mode, permissions and world rules, is verified unchanged.
"""
import gzip
import hashlib
import importlib.util
import json
import pathlib
import shutil

root = pathlib.Path(__file__).resolve().parents[1]
world = root / "build/studio-c6-runtime/minecraft-1.21.11/saves/Demo_World"
out = root / "docs/phase-2c6/runtime/inventory-fixture"
out.mkdir(exist_ok=False)
spec = importlib.util.spec_from_file_location("c4_nbt", root / "scripts/compatibility-c4-runtime.py")
nbt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nbt)
changes = []
for path in [world / "level.dat", *sorted((world / "playerdata").glob("*.dat"))]:
    original = path.read_bytes()
    raw = gzip.decompress(original)
    reader = nbt.NBT(raw)
    kind, name = reader.scalar("B"), reader.text()
    value = reader.payload(kind)
    assert bytes([kind]) + nbt.string(name) + nbt.payload(kind, value) == raw
    player = value["Data"][1]["Player"][1] if path.name == "level.dat" else value
    unchanged = repr({k: v for k, v in player.items() if k not in ("Inventory", "SelectedItemSlot")})
    previous = repr(player.get("Inventory"))
    player["Inventory"] = (9, (10, [
        {"Slot": (1, i), "id": (8, "minecraft:" + item), "count": (3, 64)}
        for i, item in enumerate(("bricks", "apple", "torch", "stone", "white_stained_glass"))]))
    player["SelectedItemSlot"] = (3, 0)
    assert unchanged == repr({k: v for k, v in player.items() if k not in ("Inventory", "SelectedItemSlot")})
    shutil.copy2(path, out / path.name)
    result = gzip.compress(bytes([kind]) + nbt.string(name) + nbt.payload(kind, value), mtime=0)
    path.write_bytes(result)
    changes.append({"path": str(path), "before_sha256": hashlib.sha256(original).hexdigest(),
                    "after_sha256": hashlib.sha256(result).hexdigest(), "previous_inventory": previous,
                    "changed_fields": ["Inventory", "SelectedItemSlot"], "other_player_fields_unchanged": True})
(out / "changes.json").write_text(json.dumps({"changes": changes,
    "boundary": "New owned C6 demo copy only; no demo-time/rules/game-mode/authentication changes"}, indent=2) + "\n")
print(json.dumps(changes, indent=2))
