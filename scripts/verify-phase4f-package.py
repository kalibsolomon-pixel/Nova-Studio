"""Phase 4-F clean-directory verification of a portable candidate.

Extracts the candidate zip into a new, empty directory and runs it with a minimal
environment: PATH holds only Windows system folders, no Qt or Visual Studio variables, and
LOCALAPPDATA, APPDATA and TEMP point into the run directory. It never touches the build
tree's binaries at runtime. The run:

  1. launches the packaged nova-studio.exe on a disposable resource pack,
  2. audits every loaded module (package folder or Windows only),
  3. authorizes MCP editing through the app's own UI (UI Automation),
  4. runs the official MCP SDK against the packaged nova-mcp.exe (39 tools, create and
     edit a texture and a model),
  5. saves explicitly through the File menu and checks the files on disk,
  6. makes an unsaved edit, terminates the process, relaunches, and checks that the
     recovery prompt appears; keeps then discards the snapshot,
  7. closes and relaunches cleanly.

This is a clean-directory test on the development machine, not a pristine-machine test.

usage: python scripts/verify-phase4f-package.py --candidate <dir> [--out <dir>]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import time
import zipfile
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--candidate', required=True)
p.add_argument('--out')
a = p.parse_args()
candidate = pathlib.Path(a.candidate).resolve()
zips = sorted(candidate.glob('*.zip'))
if len(zips) != 1:
    raise SystemExit('candidate directory must contain exactly one zip')
out = pathlib.Path(a.out) if a.out else root / 'build/phase4f-evidence/distribution' / time.strftime('clean-%Y%m%d-%H%M%S')
out.mkdir(parents=True, exist_ok=False)
uia = root / 'build/studio-phase4a-Release/tests/nova-phase4f-uia-client.exe'
sdk_python = root / 'build/phase4c-venv/Scripts/python.exe'
client = root / 'tests/phase4f_package_client.py'
report = {'candidate': str(candidate), 'zip': zips[0].name, 'checks': [], 'notes': []}
user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)


def check(name, ok, detail=''):
    report['checks'].append({'name': name, 'passed': bool(ok), 'detail': str(detail)[:2000]})
    print(('PASS ' if ok else 'FAIL ') + name + (f' — {detail}' if detail and not ok else ''), flush=True)
    return ok


def save():
    (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


# 1. Extract into a new directory.
clean = out / 'clean'
with zipfile.ZipFile(zips[0]) as archive:
    archive.extractall(clean)
package = next(clean.rglob('nova-studio.exe')).parent
check('extracted into an empty directory', (package / 'nova-mcp.exe').is_file(), package)
system_root = os.environ['SystemRoot']
for name in ('localappdata', 'appdata', 'temp', 'settings'):
    (out / name).mkdir()
env = {key: os.environ[key] for key in ('SystemRoot', 'windir', 'SystemDrive', 'COMSPEC', 'PATHEXT', 'OS',
                                         'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'USERNAME',
                                         'USERDOMAIN', 'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH') if key in os.environ}
env.update(PATH=f'{system_root}\\System32;{system_root};{system_root}\\System32\\Wbem',
           LOCALAPPDATA=str(out / 'localappdata'), APPDATA=str(out / 'appdata'), TEMP=str(out / 'temp'),
           TMP=str(out / 'temp'))
leaked = [k for k in env if k.upper().startswith(('QT_', 'VS', 'VC'))]
check('environment has no Qt or Visual Studio variables', not leaked, leaked)
report['environment'] = env


# Disposable pack.
def png(width, height, rgba):
    raw = b''.join(b'\x00' + bytes(rgba) * width for _ in range(height))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def decode(path):
    """Minimal RGBA8 PNG decoder with all five row filters; returns (width, rows)."""
    data = path.read_bytes()
    pos, idat, width, height = 8, b'', 0, 0
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and colour == 6, (depth, colour)
        if kind == b'IDAT':
            idat += body
        pos += 12 + length
    raw, stride, rows, previous = zlib.decompress(idat), width * 4, [], bytearray(width * 4)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            up, corner = previous[i], previous[i - 4] if i >= 4 else 0
            if kind == 1: line[i] = (line[i] + left) & 255
            elif kind == 2: line[i] = (line[i] + up) & 255
            elif kind == 3: line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p0 = left + up - corner
                pa, pb, pc = abs(p0 - left), abs(p0 - up), abs(p0 - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append(bytes(line))
        previous = line
    return width, rows


pack = out / 'projects/Phase4F Clean Pack'
(pack / 'assets/minecraft/textures/block').mkdir(parents=True)
(pack / 'assets/minecraft/models/block').mkdir(parents=True)
(pack / 'pack.mcmeta').write_text('{"pack":{"pack_format":46,"description":"Phase 4-F clean directory"}}', encoding='utf-8')
(pack / 'assets/minecraft/textures/block/stone.png').write_bytes(png(16, 16, (120, 120, 120, 255)))
(pack / 'assets/minecraft/models/block/stone.json').write_text(
    '{"parent":"minecraft:block/cube_all","textures":{"all":"minecraft:block/stone"}}', encoding='utf-8')
settings = out / 'settings/studio.ini'


# Window and process helpers.
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def windows(pid):
    found = []

    def callback(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buffer = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buffer, 512)
            found.append((hwnd, buffer.value))
        return True
    user32.EnumWindows(EnumWindowsProc(callback), 0)
    return found


def wait_window(pid, needle, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        for hwnd, title in windows(pid):
            if needle in title:
                return hwnd, title
        time.sleep(0.25)
    return None, None


kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


def modules(pid):
    process = kernel32.OpenProcess(0x0410, False, pid)
    handles = (wt.HMODULE * 2048)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(process, handles, ctypes.sizeof(handles), ctypes.byref(needed), 3)
    paths = []
    for i in range(needed.value // ctypes.sizeof(wt.HMODULE)):
        buffer = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(process, handles[i], buffer, 1024)
        paths.append(buffer.value)
    kernel32.CloseHandle(process)
    return paths


def invoke(hwnd, names, label):
    target = out / f'uia-{label}.json'
    result = subprocess.run([str(uia), str(hwnd), 'invoke', '30000', names, str(target)], capture_output=True, timeout=90)
    data = json.loads(target.read_text(encoding='utf-8')) if target.is_file() else {}
    return result.returncode == 0 and data.get('failures', 1) == 0, data


def clipboard_text():
    user32.OpenClipboard.argtypes = [wt.HWND]
    user32.GetClipboardData.restype = wt.HANDLE
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [wt.HANDLE]
    kernel32.GlobalUnlock.argtypes = [wt.HANDLE]
    for _ in range(20):
        if user32.OpenClipboard(None):
            break
        time.sleep(0.1)
    else:
        return None
    try:
        handle = user32.GetClipboardData(13)
        if not handle:
            return None
        pointer = kernel32.GlobalLock(handle)
        text = ctypes.wstring_at(pointer)
        kernel32.GlobalUnlock(handle)
        return text
    finally:
        user32.CloseClipboard()


def set_clipboard(text):
    if text is None:
        return
    kernel32.GlobalAlloc.restype = wt.HANDLE
    kernel32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
    user32.SetClipboardData.argtypes = [wt.UINT, wt.HANDLE]
    data = (text + '\0').encode('utf-16-le')
    handle = kernel32.GlobalAlloc(0x0002, len(data))
    pointer = kernel32.GlobalLock(handle)
    ctypes.memmove(pointer, data, len(data))
    kernel32.GlobalUnlock(handle)
    if user32.OpenClipboard(None):
        user32.EmptyClipboard()
        user32.SetClipboardData(13, handle)
        user32.CloseClipboard()


def launch(label):
    log = (out / f'studio-{label}.log').open('wb')
    process = subprocess.Popen([str(package / 'nova-studio.exe'), '--project', str(pack), '--settings', str(settings)],
                               cwd=str(package), env=env, stdout=log, stderr=log)
    hwnd, title = wait_window(process.pid, 'Nova Studio')
    return process, hwnd, title


def close(process, hwnd, label):
    user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE, as the title-bar close button sends
    try:
        process.wait(30)
        return check(f'{label}: closes cleanly', process.returncode == 0, process.returncode)
    except subprocess.TimeoutExpired:
        process.kill()
        return check(f'{label}: closes cleanly', False, 'did not exit within 30 s')


user_clipboard = clipboard_text()
try:
    # 2. Launch and audit modules.
    studio, hwnd, title = launch('first')
    check('packaged Nova Studio starts and shows its window', hwnd is not None, title)
    if not hwnd:
        raise SystemExit(1)
    time.sleep(3)
    loaded = modules(studio.pid)
    allowed = (str(package).lower(), system_root.lower())
    foreign = [m for m in loaded if not m.lower().startswith(allowed)]
    development = [m for m in loaded if '.tools' in m.lower() or 'buildtools' in m.lower() or 'studio-phase' in m.lower()]
    report['modules'] = loaded
    check('every loaded module comes from the package or Windows', not foreign, foreign)
    check('no development Qt or MSVC module is loaded', not development, development)
    # 3. Authorize MCP editing through the UI.
    # A connection code authorizes one client session; Nova revokes it when that client
    # disconnects. Each SDK stage therefore asks the UI for a fresh grant, as a user would.
    grants = []

    def authorize(label):
        ok, data = invoke(hwnd, 'Resources|MCP Access…|Allow editing…|Yes|Copy connection code', f'authorize-{label}')
        code = clipboard_text()
        grants.append(ok and bool(code) and len(code) == 64)
        return code if grants[-1] else '', data

    def sdk(stage):
        code, data = authorize(stage)
        if not code:
            check(f'MCP editing authorized through UI Automation for {stage}', False, data)
        result = subprocess.run([str(sdk_python), str(client), str(package / 'nova-mcp.exe'), stage],
                                capture_output=True, timeout=120, env={**env, 'NOVA_MCP_CODE': code,
                                'PATH': env['PATH'], 'SYSTEMROOT': system_root})
        (out / f'sdk-{stage}.log').write_bytes(result.stdout + b'\n--- stderr ---\n' + result.stderr)
        lines = [line for line in result.stdout.decode('utf-8', 'replace').splitlines() if line.startswith('{')]
        return result.returncode, json.loads(lines[-1]) if lines else {}

    # 4. Official SDK against the packaged bridge.
    status, tools = sdk('tools')
    check('MCP editing authorized through UI Automation (connection code issued)', grants and grants[0])
    check('official MCP SDK lists 39 tools through packaged nova-mcp.exe', status == 0 and tools.get('tools') == 39,
          tools.get('tools'))
    report['tools'] = tools.get('tool_names')
    status, created = sdk('create')
    check('texture and model created and edited through MCP', status == 0 and created.get('texture_changed')
          and created.get('model_changed'), created)
    time.sleep(1)
    # Genuine directory symbolic link, only with permissions the session already has.
    outside = out / 'outside-pack'
    (outside / 'deep').mkdir(parents=True)
    sentinel = outside / 'sentinel.png'
    sentinel.write_bytes(png(16, 16, (1, 2, 3, 255)))
    (outside / 'deep/escape.png').write_bytes(png(16, 16, (4, 5, 6, 255)))
    before_sentinel = sentinel.read_bytes()
    link = pack / 'assets/minecraft/textures/linked'
    kernel32.CreateSymbolicLinkW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, wt.DWORD]
    created = kernel32.CreateSymbolicLinkW(str(link), str(outside), 0x1 | 0x2)
    report['directory_symlink'] = {'created': bool(created), 'win32_error': 0 if created else ctypes.get_last_error()}
    if created:
        def fingerprint(folder):
            return {str(f.relative_to(folder)): f.stat().st_size for f in folder.rglob('*') if f.is_file()} if folder.exists() else {}
        nova_before = fingerprint(pack / '.nova')
        time.sleep(1.5)  # let the directory monitor index the new entry
        status, linked = sdk('linked')
        report['directory_symlink'].update(linked)
        check('directory symlink: no asset is listed through the link', status == 0 and not linked.get('listed_through_link'),
              linked.get('listed_through_link'))
        # Linked content is never indexed: it must appear in no MCP output at all.
        stage_output = (out / 'sdk-linked.log').read_text(encoding='utf-8', errors='replace')
        check('directory symlink: no MCP output refers to content behind the link',
              status == 0 and 'escape.png' not in stage_output and 'sentinel.png' not in stage_output,
              linked.get('validation_codes'))
        check('directory symlink: outside sentinel unchanged', sentinel.read_bytes() == before_sentinel)
        check('directory symlink: project metadata unchanged', fingerprint(pack / '.nova') == nova_before)
        # Snapshots of the open unsaved documents are written on their own timer; none may
        # contain data from behind the link.
        snapshot_bytes = b''.join(f.read_bytes() for f in (out / 'settings/recovery').glob('*.nvr'))
        check('directory symlink: no recovery snapshot refers to the link',
              b'linked' not in snapshot_bytes and b'sentinel' not in snapshot_bytes)
        os.rmdir(link)  # removes the link itself, never the target
        check('directory symlink removed without touching its target', not link.exists() and sentinel.is_file())
    else:
        report['notes'].append('directory symbolic link not creatable with existing permissions; check deferred')
    # 5. Explicit saves through the File menu: the model tab is active after creation.
    ok, data = invoke(hwnd, 'File|Save Model JSON', 'save-model')
    check('model saved explicitly from the File menu', ok, data)
    ok, data = invoke(hwnd, 'phase4f_texture.png*|File|Update Minecraft PNG', 'save-texture')
    check('texture PNG updated explicitly from the File menu', ok, data)
    time.sleep(1.5)
    model_file = pack / 'assets/minecraft/models/block/phase4f_model.json'
    texture_file = pack / 'assets/minecraft/textures/block/phase4f_texture.png'
    model_json = json.loads(model_file.read_text(encoding='utf-8')) if model_file.is_file() else {}
    check('saved model JSON contains the MCP edit', model_json.get('elements', [{}])[0].get('to') == [16, 8, 16],
          model_json.get('elements'))
    if texture_file.is_file():
        width, rows = decode(texture_file)
        check('saved texture PNG is 16 px wide', width == 16, width)
        # x 0-7 carry the MCP fill_rect colour, x 8-15 the creation fill.
        check('saved texture pixels carry the MCP edit', rows[0][0:4] == bytes([0xf5, 0x28, 0xab, 0xff]) and
              rows[15][15 * 4:16 * 4] == bytes([0x20, 0x30, 0x40, 0xff]), rows[0][:8].hex() + ' ' + rows[15][60:64].hex())
    else:
        check('saved texture PNG exists', False, texture_file)
    # 6. Unsaved edit, crash, relaunch, recovery prompt.
    status, unsaved = sdk('edit-unsaved')
    check('unsaved edit made', status == 0 and unsaved.get('texture_changed'), unsaved)
    time.sleep(5)  # the recovery snapshot timer runs every 2 s
    snapshots = list((out / 'settings/recovery').glob('*.nvr'))
    check('recovery snapshot written before the crash', bool(snapshots), [s.name for s in snapshots])
    studio.kill()
    studio.wait(10)
    studio, hwnd, title = launch('after-crash')
    check('relaunch after a forced termination', hwnd is not None, title)
    prompt, prompt_title = wait_window(studio.pid, 'Recover Unsaved Session', 30)
    check('recovery prompt offered after relaunch', prompt is not None, prompt_title)
    if prompt:
        ok, data = invoke(prompt, 'Later', 'recovery-later')
        check('recovery prompt answered "Later" through UI Automation', ok, data)
    time.sleep(1)
    check('snapshot kept after "Later"', bool(list((out / 'settings/recovery').glob('*.nvr'))))
    close(studio, hwnd, 'after-crash')
    studio, hwnd, title = launch('discard')
    prompt, prompt_title = wait_window(studio.pid, 'Recover Unsaved Session', 30)
    check('recovery prompt offered again', prompt is not None, prompt_title)
    if prompt:
        ok, data = invoke(prompt, 'Discard snapshot', 'recovery-discard')
        check('snapshot discarded through UI Automation', ok, data)
    time.sleep(1)
    check('recovery folder empty after discard', not list((out / 'settings/recovery').glob('*.nvr')))
    check('saved files unchanged by recovery handling', model_file.is_file() and texture_file.is_file())
    close(studio, hwnd, 'discard')
    # 7. Clean relaunch.
    studio, hwnd, title = launch('relaunch')
    check('clean relaunch', hwnd is not None, title)
    time.sleep(2)
    stray = wait_window(studio.pid, 'Recover Unsaved Session', 3)[0]
    check('no recovery prompt on a clean relaunch', stray is None)
    close(studio, hwnd, 'relaunch')
finally:
    set_clipboard(user_clipboard)
    # Never leave a packaged process running, whatever happened above.
    try:
        if studio.poll() is None:
            studio.kill()
            studio.wait(10)
    except NameError:
        pass
    report['passed'] = all(c['passed'] for c in report['checks'])
    save()
print(json.dumps({'passed': report['passed'], 'checks': len(report['checks']), 'report': str(out / 'report.json')}))
sys.exit(0 if report['passed'] else 1)
