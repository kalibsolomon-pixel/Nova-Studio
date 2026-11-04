"""Native startup/layout restoration against new, isolated fixture copies."""
import json
from pathlib import Path
import shutil
import time
from validate import ROOT,EXES,run

out=ROOT/'docs/phase-1/native-smoke.json'
results={}
base=ROOT/'fixtures/generated'/f'layout-smoke-{time.time_ns()}'
assert base.resolve().is_relative_to((ROOT/'fixtures/generated').resolve())
base.mkdir(parents=True,exist_ok=False)
source=ROOT/'fixtures/generated/pack-1500'
pngs=sorted(source.rglob('*.png'))[:2]
for name,exe in EXES.items():
    project=base/name
    for png in pngs:
        dest=project/png.relative_to(source);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(png,dest)
    traces=[]
    for iteration in range(2):
        trace=base/f'{name}-{iteration}.json'
        run([exe,'--project',project,'--telemetry',trace,'--smoke-seconds','1.5'],15)
        traces.append(json.loads(trace.read_text()))
    assert traces[0]['counters'].get('layout_restored',0)==0
    assert traces[1]['counters']['layout_restored']==1
    sidecar=project/('.nova/egui-layout.ron' if name=='rust' else '.nova/qt-layout.ini')
    assert sidecar.exists() and sidecar.stat().st_size>100
    results[name]={'launches':2,'exit_codes':[0,0],'second_launch_restored':True,'sidecar_bytes':sidecar.stat().st_size}
    missing=base/f'{name}-missing-project'
    run([exe,'--project',missing,'--smoke-seconds','1'],15)
    assert not missing.exists(),'A failed project open must not create that directory during layout save'
    results[name]['missing_project_graceful_without_creation']=True
    print(name,'native layout round trip passed',flush=True)
out.write_text(json.dumps(results,indent=2))
