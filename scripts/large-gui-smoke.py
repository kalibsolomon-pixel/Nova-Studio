import json
from validate import ROOT,EXES,run

results={}
for name,exe in EXES.items():
    trace=ROOT/f'benchmarks/{name}-50000-gui.json'
    run([exe,'--project',ROOT/'fixtures/generated/pack-50000','--fresh-layout','--telemetry',trace,'--smoke-seconds','3'],20)
    data=json.loads(trace.read_text())
    assert data['samples_ms'].get('ready_capture_ms')
    results[name]=data
    print(name,'50k GUI native launch and indexed texture rendering passed',flush=True)
(ROOT/'docs/benchmarks/raw/large-gui-smoke.json').write_text(json.dumps(results,indent=2))
