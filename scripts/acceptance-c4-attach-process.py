"""Retain the existing owned GUI process's Windows handle for exact exit collection.

This uses query/synchronize access only. It sends no UI input and cannot kill it.
"""
import ctypes, datetime, hashlib, json, pathlib, time
from ctypes import wintypes
import psutil

root = pathlib.Path(__file__).resolve().parents[1]
case = root / 'docs/phase-2c4/acceptance-final/processes/B-pan-human'
original = json.loads((case / 'supervisor.json').read_text())
process = psutil.Process(original['pid'])
exe = pathlib.Path(process.exe()).resolve()
assert exe == pathlib.Path(original['exe']).resolve()
assert hashlib.sha256(exe.read_bytes()).hexdigest() == original['exe_sha256']
assert abs(process.create_time()-datetime.datetime.fromisoformat(original['launch_utc']).timestamp()) < 5
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.WaitForSingleObject.restype = wintypes.DWORD
kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel.GetExitCodeProcess.restype = wintypes.BOOL
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
handle = kernel.OpenProcess(0x00100000 | 0x1000, False, process.pid)
assert handle, ctypes.WinError(ctypes.get_last_error())
record = {'pid': process.pid, 'exe': str(exe), 'exe_sha256': original['exe_sha256'],
          'original_launch_utc': original['launch_utc'],
          'attached_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'original_record_last_write_utc': datetime.datetime.fromtimestamp((case/'supervisor.json').stat().st_mtime, datetime.timezone.utc).isoformat(),
          'supervision_gap': True, 'original_supervisor_stop_cause': 'unknown; original supervisor stderr/exit was not retained',
          'method': 'OpenProcess QUERY_LIMITED_INFORMATION|SYNCHRONIZE; WaitForSingleObject; GetExitCodeProcess',
          'timeout': False, 'exit_code': None, 'exit_utc': None, 'classification': 'reattached; waiting for normal exit'}
def persist():
    temporary = case / 'exit-recovery.json.partial'
    temporary.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    deadline = time.monotonic()+5
    while True:
        try:
            temporary.replace(case / 'exit-recovery.json')
            break
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)
persist()
try:
    status = kernel.WaitForSingleObject(handle, 600_000)
    if status == 0:
        code = wintypes.DWORD()
        assert kernel.GetExitCodeProcess(handle, ctypes.byref(code))
        record.update(exit_code=code.value, exit_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      classification='normal successful exit' if code.value == 0 else 'nonzero process exit; inspect diagnostics')
    elif status == 258:
        record.update(timeout=True, classification='reattached observation timed out; application left untouched')
    else:
        raise ctypes.WinError(ctypes.get_last_error())
finally:
    kernel.CloseHandle(handle)
persist()
