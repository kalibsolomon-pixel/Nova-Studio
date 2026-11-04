"""Focused final Release/Debug evidence using production binaries and isolated settings."""
from pathlib import Path
import hashlib,json,os,subprocess
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'docs/phase-2b'
ENV={**os.environ,'PATH':str(ROOT/'.tools/qt/bin')+os.pathsep+os.environ['PATH'],
     'NOVA_LARGE_FIXTURE':str(ROOT/'fixtures/generated/pack-50000')}
def run(exe,*args,env=ENV):
    subprocess.run([str(exe),*map(str,args)],cwd=ROOT,env=env,check=True,timeout=120)
for configuration in ('release','debug'):
    folder=ROOT/f'build/studio-{configuration}'
    for suite in ('core','session','project','integrity','indexing','workspace'):
        run(folder/f'tests/nova-{suite}-tests.exe','-o',str(OUT/f'{suite}-{configuration}.txt')+',txt')
    if configuration=='release':
        run(folder/'tests/nova-acceptance-tests.exe','-o',str(OUT/'acceptance-release.txt')+',txt',
            env={**ENV,'NOVA_EVIDENCE_DIR':str(OUT/'screenshots')})
exe=ROOT/'build/studio-release/src/nova-studio.exe'
run(exe,'--cycle-project',ROOT/'fixtures/generated/pack-50000','--settings',ROOT/'build/phase2b-cycle-final.ini','--fresh-layout','--telemetry',OUT/'memory-cycle.json')
run(exe,'--project',ROOT/'fixtures/generated/pack-50000','--fresh-layout','--exercise','--telemetry',OUT/'exercise-50000.json')
run(exe,'--project',ROOT/'fixtures/generated/pack-10000','--fresh-layout','--smoke-seconds','3','--telemetry',OUT/'workspace-10000.json')
protected=json.loads((OUT/'protected-sha256.json').read_text())
changed=[name for name,sha in protected.items() if hashlib.sha256((ROOT/name).read_bytes()).hexdigest()!=sha]
assert not changed,changed
(OUT/'preservation-check.json').write_text(json.dumps({'protected_files_checked':len(protected),'changed':changed},indent=2))
(OUT/'delivery-binaries.json').write_text(json.dumps({c:hashlib.sha256((ROOT/f'build/studio-{c}/src/nova-studio.exe').read_bytes()).hexdigest() for c in ('release','debug')},indent=2))
print('Final suites, screenshots, 50k exercise, memory cycle and protected-file verification passed',flush=True)
