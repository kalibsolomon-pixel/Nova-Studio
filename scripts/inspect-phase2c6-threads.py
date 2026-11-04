"""Read-only thread CPU/start-module evidence for this task's own idle probe."""
import ctypes as c,json,pathlib,time
from ctypes import wintypes as w
import psutil
root=pathlib.Path(__file__).resolve().parents[1]
target=next(p for p in psutil.process_iter(['exe']) if p.info['exe'] and str(root/'build').lower() in p.info['exe'].lower() and pathlib.Path(p.info['exe']).name in ('nova-c6-idle-before.exe','nova-c6-activity.exe'))
kernel=c.WinDLL('kernel32',use_last_error=True);native=c.WinDLL('ntdll');api=c.WinDLL('psapi')
kernel.OpenThread.argtypes=[w.DWORD,w.BOOL,w.DWORD];kernel.OpenThread.restype=w.HANDLE
kernel.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];kernel.OpenProcess.restype=w.HANDLE
kernel.CloseHandle.argtypes=[w.HANDLE]
native.NtQueryInformationThread.argtypes=[w.HANDLE,c.c_int,c.c_void_p,w.ULONG,c.c_void_p]
api.GetMappedFileNameW.argtypes=[w.HANDLE,c.c_void_p,w.LPWSTR,w.DWORD]
process=kernel.OpenProcess(0x410,False,target.pid);before={t.id:t.user_time+t.system_time for t in target.threads()};time.sleep(10);rows=[]
for t in target.threads():
 handle=kernel.OpenThread(0x40,False,t.id);address=c.c_void_p();module=c.create_unicode_buffer(2048)
 if handle:
  native.NtQueryInformationThread(handle,9,c.byref(address),c.sizeof(address),None)
  api.GetMappedFileNameW(process,address,module,2048);kernel.CloseHandle(handle)
 rows.append({'id':t.id,'cpu_ms_per_10_seconds':1000*(t.user_time+t.system_time-before.get(t.id,0)),'start_module':module.value,'start_address':hex(address.value or 0)})
kernel.CloseHandle(process)
out={'pid':target.pid,'executable':target.exe(),'threads':sorted(rows,key=lambda x:-x['cpu_ms_per_10_seconds'])}
path=root/'docs/phase-2c6'/('thread-sample-'+str(target.pid)+'.json');path.write_text(json.dumps(out,indent=2));print(path)
