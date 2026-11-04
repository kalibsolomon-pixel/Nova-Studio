"""Execute native Qt suites and preserve old evidence; outputs only under phase-2c."""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--build',default='studio-release')
parser.add_argument('--configuration',choices=['release','debug'],default='release')
parser.add_argument('--preservation',action='store_true')
args=parser.parse_args()
OUT=ROOT/'docs/phase-2c'
env={**os.environ,'PATH':str(ROOT/'.tools/qt/bin')+os.pathsep+os.environ['PATH'],
     'NOVA_LARGE_FIXTURE':str(ROOT/'fixtures/generated/pack-50000'),
     'NOVA_EVIDENCE_DIR':str(OUT/('screenshots' if args.configuration=='release' else 'screenshots-debug')),
     'NOVA_LAYER_BENCHMARK':str(OUT/f'layer-benchmarks-{args.configuration}.json')}
summary={'configuration':args.configuration,'build':args.build,'tests':{}}
exe=ROOT/'build'/args.build/'src/nova-studio.exe'
summary['executable_sha256']=hashlib.sha256(exe.read_bytes()).hexdigest()
for name in ['core','workspace','session','project','integrity','indexing','acceptance','layered','layer-gui']:
    log=OUT/f'{name}-{args.configuration}.txt'
    result=subprocess.run([str(ROOT/'build'/args.build/'tests'/f'nova-{name}-tests.exe'),'-o',str(log)+',txt'],env=env,timeout=180)
    match=re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped',log.read_text())
    summary['tests'][name]={'returncode':result.returncode,'counts':list(map(int,match.groups())) if match else None}
    (OUT/f'validation-{args.configuration}.json').write_text(json.dumps(summary,indent=2))
    assert result.returncode==0 and match and match.group(2)=='0' and match.group(3)=='0',name
    print(name,match.group(0),flush=True)
if args.preservation:
    for inventory in ['protected-sha256.json','protected-local-sha256.json']:
        data=json.loads((OUT/inventory).read_text(encoding='utf-8-sig'))
        entries=[(r['path'],r['sha256']) for r in data] if isinstance(data,list) else list(data.items())
        for name,sha in entries:
            assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest().lower()==sha.lower(),name
        summary[inventory]={'files':len(entries),'unchanged':True}
    fixtures={}
    for count in (1500,10000,50000):
        folder=ROOT/f'fixtures/generated/pack-{count}'
        files=sorted(folder.rglob('*.png'),key=lambda p:int(p.stem.rsplit('_',1)[1]))
        assert len(files)==count
        digest=hashlib.sha256()
        for path in files:
            digest.update(path.relative_to(folder).as_posix().encode());digest.update(path.read_bytes())
        expected=json.loads((folder/'fixture.json').read_text())['sha256']
        assert digest.hexdigest()==expected
        fixtures[str(count)]={'count':count,'sha256':expected,'unchanged':True}
    (OUT/'fixture-verification.json').write_text(json.dumps(fixtures,indent=2))
    with (OUT/'fixture-tests.txt').open('w') as log:
        subprocess.run([str(ROOT/'.tools/python/Scripts/python.exe'),'-m','unittest','discover','-s',str(ROOT/'fixtures/generator'),'-v'],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    subprocess.run([str(ROOT/'build/qt-review/nova-qt-tests.exe'),'-o',str(OUT/'prototype-tests.txt')+',txt'],env=env,check=True,timeout=60)
(OUT/f'validation-{args.configuration}.json').write_text(json.dumps(summary,indent=2))
