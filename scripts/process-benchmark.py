"""Sequential native process measurements; OS-warm cache; no display changes."""
from pathlib import Path
import json
import os
import subprocess
import time
import psutil

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'docs/benchmarks/raw'
OUT.mkdir(parents=True,exist_ok=True)
ENV=os.environ.copy();ENV['PATH']=str(ROOT/'.tools/qt/bin')+os.pathsep+ENV['PATH']
EXES={'rust':ROOT/'build/nova-egui.exe','qt':ROOT/'build/qt-review/nova-qt.exe'}

def launch(name,seconds,tag):
    telemetry=ROOT/f'benchmarks/{name}-{tag}.json'
    if telemetry.exists():
        raise FileExistsError(telemetry)
    start=time.perf_counter()
    proc=subprocess.Popen([str(EXES[name]),'--project','fixtures/generated/pack-1500','--fresh-layout','--telemetry',str(telemetry),'--smoke-seconds',str(seconds)],cwd=ROOT,env=ENV,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    info=psutil.Process(proc.pid)
    initial=None
    while time.perf_counter()-start<15:
        if proc.poll() is not None: raise RuntimeError(f'{name} exited {proc.returncode} before readiness')
        if telemetry.exists():
            try:
                data=json.loads(telemetry.read_text())
                if data.get('samples_ms',{}).get('ready_capture_ms'):
                    initial={'external_ready_ms':(time.perf_counter()-start)*1000,'internal_ready_ms':data['samples_ms']['ready_capture_ms'][0],'initial_working_set_bytes':info.memory_info().rss,'environment':data.get('environment',{})}
                    break
            except (ValueError,PermissionError):pass
        time.sleep(.002)
    if initial is None:raise RuntimeError(f'{name} no ready marker')
    return proc,info,initial,telemetry

batch=str(time.time_ns())
results={'cache':'OS-warm; no cache flush','startup':{},'idle':{},'shutdown_timeouts':[],'logical_processors':psutil.cpu_count()}
for repeat in range(20):
    order=['rust','qt'] if repeat%2==0 else ['qt','rust']
    for name in order:
        proc,info,data,telemetry=launch(name,1.5,f'startup-{batch}-{repeat}')
        try:
            out,err=proc.communicate(timeout=20)
            if proc.returncode:raise RuntimeError((name,proc.returncode,err.decode(errors='replace')))
        except subprocess.TimeoutExpired:
            results['shutdown_timeouts'].append({'framework':name,'repeat':repeat,'stage':'after readiness; smoke deadline did not terminate process within 20s'})
            proc.terminate();proc.communicate(timeout=10)
        results['startup'].setdefault(name,[]).append(data)
        (OUT/'process.json').write_text(json.dumps(results,indent=2))
    if repeat%5==4:print('Startup pairs',repeat+1,flush=True)
for name in ('rust','qt'):
    proc,info,data,telemetry=launch(name,38,'idle-'+batch)
    time.sleep(10)
    cpu0=info.cpu_times();start=time.perf_counter();samples=[]
    for _ in range(20):
        time.sleep(1);samples.append(info.memory_info().rss)
    cpu1=info.cpu_times();wall=time.perf_counter()-start
    results['idle'][name]={'working_set_samples_bytes':samples,'cpu_one_core_percent':100*((cpu1.user+cpu1.system)-(cpu0.user+cpu0.system))/wall,'cpu_window_seconds':wall,'stabilization_seconds':10,'initial':data}
    proc.communicate(timeout=20)
    if proc.returncode:raise RuntimeError((name,proc.returncode))
    print(name,'idle measured',flush=True)
(OUT/'process.json').write_text(json.dumps(results,indent=2))
print('Wrote',OUT/'process.json',flush=True)
