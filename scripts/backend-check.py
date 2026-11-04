"""Diagnostic native wgpu backend check; single test process, no UI input."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import time
import psutil
ROOT=Path(__file__).resolve().parents[1]
results={}
for backend in ('vulkan','dx12'):
    env=os.environ.copy();env['WGPU_BACKEND']=backend
    telemetry=ROOT/f'benchmarks/backend-{backend}.json'
    proc=subprocess.Popen([str(ROOT/'build/nova-egui.exe'),'--project','fixtures/generated/pack-1500','--fresh-layout','--telemetry',str(telemetry),'--smoke-seconds','18'],cwd=ROOT,env=env,stderr=subprocess.PIPE)
    info=psutil.Process(proc.pid)
    time.sleep(6)
    before=info.cpu_times();threads0={t.id:t.user_time+t.system_time for t in info.threads()};start=time.perf_counter()
    time.sleep(6)
    after=info.cpu_times();wall=time.perf_counter()-start
    hot=[]
    for thread in info.threads():
        delta=thread.user_time+thread.system_time-threads0.get(thread.id,0)
        if delta>.1:
            kernel=ctypes.windll.kernel32
            kernel.OpenThread.restype=ctypes.c_void_p
            handle=kernel.OpenThread(0x0800,False,thread.id)
            description=ctypes.c_wchar_p()
            if handle:
                kernel.GetThreadDescription.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_wchar_p)]
                kernel.GetThreadDescription(handle,ctypes.byref(description))
                name=description.value
                kernel.CloseHandle(ctypes.c_void_p(handle))
            else:name=None
            hot.append({'tid':thread.id,'cpu_seconds':delta,'name':name})
    results[backend]={'one_core_cpu_percent':100*((after.user+after.system)-(before.user+before.system))/wall,'rss_bytes':info.memory_info().rss,'hot_threads':hot}
    _,err=proc.communicate(timeout=20)
    results[backend]['exit']=proc.returncode
    results[backend]['stderr']=err.decode(errors='replace')[-2000:]
    if telemetry.exists():results[backend]['telemetry']=json.loads(telemetry.read_text())
    print(backend,results[backend]['one_core_cpu_percent'],hot,flush=True)
(ROOT/'docs/benchmarks/raw/backend-check.json').write_text(json.dumps(results,indent=2))
