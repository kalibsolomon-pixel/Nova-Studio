"""Inspect CPU thread origins of our own test processes, without desktop input."""
import ctypes
import json
import subprocess
import time
from pathlib import Path
import psutil
ROOT=Path(__file__).resolve().parents[1]
results={}
for label,extra in [('long-deadline',['--telemetry','benchmarks/idle-diagnostic-long.json'])]:
    proc=subprocess.Popen([str(ROOT/'build/nova-egui.exe'),'--project','fixtures/generated/pack-1500','--fresh-layout','--smoke-seconds','38',*extra],cwd=ROOT)
    info=psutil.Process(proc.pid);time.sleep(10);before={t.id:t.user_time+t.system_time for t in info.threads()};time.sleep(5)
    hot=[]
    for thread in info.threads():
        delta=thread.user_time+thread.system_time-before.get(thread.id,0)
        if delta<.1:continue
        kernel=ctypes.windll.kernel32;kernel.OpenThread.restype=ctypes.c_void_p
        handle=kernel.OpenThread(0x0040,False,thread.id)
        addr=ctypes.c_void_p()
        ctypes.windll.ntdll.NtQueryInformationThread.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_ulong,ctypes.c_void_p]
        result=ctypes.windll.ntdll.NtQueryInformationThread(handle,9,ctypes.byref(addr),ctypes.sizeof(addr),None)
        kernel.CloseHandle(ctypes.c_void_p(handle))
        module='unknown'
        for region in info.memory_maps(grouped=False):
            start=int(region.addr.split('-')[0],16)
            if start<=int(addr.value or 0)<start+region.rss:module=region.path;break
        hot.append({'tid':thread.id,'cpu_seconds':delta,'start':hex(addr.value or 0),'module':module,'query_result':result})
    proc.wait(timeout=35);results[label]=hot;print(label,hot,flush=True)
(ROOT/'docs/benchmarks/raw/idle-diagnostic.json').write_text(json.dumps(results,indent=2))
