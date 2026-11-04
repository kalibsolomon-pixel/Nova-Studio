"""Exercise actual evidence writers against a real Windows file-sharing lock."""
import ast, ctypes, datetime, json, pathlib, threading, time
from ctypes import wintypes

root = pathlib.Path(__file__).resolve().parents[1]
work = root / 'build/studio-c4-acceptance-final/supervision-self-check'
work.mkdir(exist_ok=False)
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                              wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]

def lock(path):
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x80, None)
    assert handle not in (None, ctypes.c_void_p(-1).value), ctypes.WinError(ctypes.get_last_error())
    return handle

results = []
for script, function, filename in (('acceptance-c4-supervise.py','save','supervisor.json'),
                                   ('acceptance-c4-watch-native.py','persist','watch.json')):
    folder = work / function
    folder.mkdir()
    destination = folder / filename
    destination.write_text('{"before":true}')
    temporary = folder / 'old-single-attempt.partial'
    temporary.write_text('{"candidate":true}')
    handle = lock(destination)
    rejected = False
    try:
        temporary.replace(destination)
    except PermissionError:
        rejected = True
    finally:
        kernel.CloseHandle(handle)
    assert rejected, 'The Windows sharing-conflict precondition was not established'
    tree = ast.parse((root/'scripts'/script).read_text())
    definition = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == function)
    environment = {'out':folder,'output':folder,'json':json,'time':time,'record':{'after':True}}
    exec(compile(ast.Module(body=[definition],type_ignores=[]),script,'exec'),environment)
    handle = lock(destination)
    release = threading.Thread(target=lambda h=handle: (time.sleep(0.2),kernel.CloseHandle(h)))
    release.start()
    started = time.monotonic()
    if function == 'save':
        environment[function]({'after':True})
    else:
        environment[function]()
    elapsed = time.monotonic()-started
    release.join()
    assert json.loads(destination.read_text()) == {'after':True}
    results.append({'script':script,'single_attempt_reproduced_PermissionError':rejected,
                    'bounded_retry_succeeded':True,'seconds':elapsed})
report = {'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'passed':True,'checks':results,
          'scope':'Validation writers only. Reproduced failure mode is not proof of the historical supervisor stop cause.'}
(root/'docs/phase-2c4/acceptance-final/processes/B-pan-human/supervision-self-check.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
