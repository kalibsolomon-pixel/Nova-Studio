"""Validate fixture counts and byte hashes after native QA/benchmark runs."""
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
results={}
for count in (1500,10000,50000):
    folder=ROOT/f'fixtures/generated/pack-{count}'
    files=sorted(folder.rglob('*.png'),key=lambda p:int(p.stem.rsplit('_',1)[1]))
    assert len(files)==count
    digest=hashlib.sha256()
    for path in files:
        digest.update(path.relative_to(folder).as_posix().encode());digest.update(path.read_bytes())
    manifest=json.loads((folder/'fixture.json').read_text())
    assert digest.hexdigest()==manifest['sha256'],f'{folder} bytes differ from the generated manifest'
    results[str(count)]={'count':len(files),'sha256':digest.hexdigest(),'unchanged':True}
    print(count,'PNGs unchanged and manifest verified',flush=True)
(ROOT/'docs/phase-1/fixture-verification.json').write_text(json.dumps(results,indent=2))
