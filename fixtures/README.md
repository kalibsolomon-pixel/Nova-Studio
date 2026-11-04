# Original synthetic resource packs

Install `scripts/requirements.txt` into a local Python venv. From the repository:

```powershell
python fixtures/generator/generate.py fixtures/generated/pack-1500 --count 1500
python fixtures/generator/generate.py fixtures/generated/pack-10000 --count 10000
python fixtures/generator/generate.py fixtures/generated/pack-50000 --count 50000
python -m unittest discover -s fixtures/generator -v
```

Output directories must not exist; the generator refuses to overwrite a pack.
Seed 20261027 reproduces file names and PNG bytes with Pillow 12.3.0. The manifests
record the aggregate SHA256 of sorted generation-order relative paths and PNG
bytes. Exactly the stated number of PNG assets are created, plus pack metadata
and a fixture manifest. Four namespaces, three categories and sixteen folder
sets are simulated. Sizes cycle 16/32/64/128/256 with flat/repeated/unique colors
and fully transparent pixels. Nothing is copied from Minecraft.

Only manifests are versioned; generated packs and their `.nova` state are ignored.
Benchmarks never save into fixture PNGs. To verify a pack after an interactive
session, recompute its hash or generate a new uniquely named fixture directory.
The sample pack.mcmeta uses format 15 solely as representative metadata; Phase 1
does not claim validation against any particular game release.
