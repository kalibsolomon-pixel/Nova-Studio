"""Isolated production-process startup/idle samples and model readback metrics."""
import json
import argparse
import hashlib
import os
import pathlib
import statistics
import subprocess
import time
import psutil

root=pathlib.Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--output',default='docs/phase-2c6/performance-final')
options=parser.parse_args()
output=root/options.output
output.mkdir(exist_ok=False)
environment={**os.environ,'PATH':str(root/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
exe=root/'build/studio-c6-release/src/nova-studio.exe'
flags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0
rows=[]
for label,project in [('models',root/'fixtures/phase-2c5-models'),('assets50000',root/'fixtures/generated/pack-50000')]:
    for n in range(3):
        telemetry=output/f'{label}-{n}.json'
        started=time.perf_counter()
        result=subprocess.run([str(exe),'--project',str(project),'--settings',str(output/f'{label}-{n}.ini'),'--fresh-layout','--telemetry',str(telemetry),'--smoke-seconds','2'],env=environment,cwd=root,creationflags=flags,timeout=30,capture_output=True)
        if result.returncode: raise RuntimeError(result.stderr.decode(errors='replace'))
        data=json.loads(telemetry.read_text())
        rows.append({'project':label,'run':n,'process_total_ms':(time.perf_counter()-started)*1000,'telemetry':data})
idle=[]
for label,project in [('empty',None),('models',root/'fixtures/phase-2c5-models'),('assets50000',root/'fixtures/generated/pack-50000')]:
    args=[str(exe),'--settings',str(output/f'idle-{label}.ini'),'--fresh-layout','--smoke-seconds','10']
    if project:args+=['--project',str(project)]
    child=subprocess.Popen(args,cwd=root,env=environment,creationflags=flags,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    proc=psutil.Process(child.pid)
    time.sleep(3)
    before=proc.cpu_times();start=time.perf_counter();time.sleep(5);after=proc.cpu_times()
    memory=proc.memory_info();seconds=time.perf_counter()-start
    idle.append({'project':label,'sample_seconds':seconds,'process_cpu_ms':((after.user+after.system)-(before.user+before.system))*1000,'single_core_cpu_percent':((after.user+after.system)-(before.user+before.system))/seconds*100,'working_set_bytes':memory.rss,'private_bytes':getattr(memory,'private',0),'threads':proc.num_threads()})
    child.wait(timeout=20)
model=output/'models-rendering.json'
result=subprocess.run([str(root/'build/studio-c6-release/tests/nova-model-bench.exe'),str(root/'fixtures/phase-2c5-models'),str(model)],env=environment,cwd=root,creationflags=flags,timeout=60,capture_output=True)
if result.returncode:raise RuntimeError(result.stderr.decode(errors='replace'))
summary={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'startup_samples':rows,'idle':idle,'models':json.loads(model.read_text()),'notes':'3 warm startup samples per pack; 5-second idle samples, not a long soak; readback timings include synchronization. No GPU-only or game-runtime certification.'}
(output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({'idle':idle,'head':summary['head']},indent=2))
