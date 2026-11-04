"""Five-minute settle observation. Three quiescent native processes; no UI input."""
import json
import os
from pathlib import Path
import subprocess
import time
import psutil
from validate import ROOT,EXES,ENV

run_id=str(time.time_ns())
states={}
for name,exe,backend in [('rust-dx12',EXES['rust'],'dx12'),('rust-vulkan',EXES['rust'],'vulkan'),('qt-opengl',EXES['qt'],None)]:
    env=ENV.copy()
    if backend:env['WGPU_BACKEND']=backend
    telemetry=ROOT/f'benchmarks/steady-{run_id}-{name}.json'
    proc=subprocess.Popen([str(exe),'--project','fixtures/generated/pack-1500','--fresh-layout','--telemetry',str(telemetry),'--smoke-seconds','330'],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    info=psutil.Process(proc.pid);info.cpu_percent()
    states[name]={'proc':proc,'info':info,'rows':[],'telemetry':telemetry}
start=time.perf_counter()
for second in range(1,321):
    time.sleep(max(0,start+second-time.perf_counter()))
    for state in states.values():
        if state['proc'].poll() is not None:raise RuntimeError('Test process exited early')
        state['rows'].append({'elapsed_seconds':time.perf_counter()-start,'cpu_one_core_percent':state['info'].cpu_percent(),'working_set_bytes':state['info'].memory_info().rss})
    if second%30==0:print('Soak',second,'seconds', {k:v['rows'][-1]['cpu_one_core_percent'] for k,v in states.items()},flush=True)
result={'method':'Three concurrent quiescent native processes; warm small pack; last 20s after 300s settle. No UI input or changes to display settings. Process CPU is percent of one logical processor.','samples':{}}
for name,state in states.items():
    out,err=state['proc'].communicate(timeout=25)
    if state['proc'].returncode:raise RuntimeError((name,state['proc'].returncode,err))
    result['samples'][name]={'rows':state['rows'],'telemetry':json.loads(state['telemetry'].read_text()),'exit':0}
(ROOT/'docs/benchmarks/raw/steady-idle.json').write_text(json.dumps(result,indent=2))
print('Five-minute soak complete',flush=True)
