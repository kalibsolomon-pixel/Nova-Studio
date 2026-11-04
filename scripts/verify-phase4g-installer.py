# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-G: install, upgrade and uninstall acceptance for the NSIS release candidate.

Runs on the development PC, which already has the owner's Phase 3-E (Inno Setup) copy of
Nova Studio in %LOCALAPPDATA%\\Programs\\Nova Studio and real data in %LOCALAPPDATA%\\Nova Studio.
Both are hashed before and after and must not change. Test installations go into folders under
build/phase4g-evidence/installer/<run>. Launched applications use --settings and environment
variables inside that run folder, so no real settings, recovery data or AI agent configuration
is read or written. The real Start menu and HKCU uninstall entry ARE used while a test install
exists (that is what is being tested) and are removed by the uninstall steps.

Usage: python scripts/verify-phase4g-installer.py --candidate <label> [--build studio-phase4g-Release]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time
import winreg
import zlib

sys.stdout.reconfigure(encoding='utf-8', errors='replace')  # test paths contain non-ASCII characters
root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--candidate', required=True)
p.add_argument('--build', default='studio-phase4g-Release')
a = p.parse_args()
cand = root / 'build/phase4g-distribution' / a.candidate
installer = next(cand.glob('nova-studio-*-windows-x64.exe'))
payload = cand / 'payload' / 'Nova Studio'
manifest = json.loads((payload / 'package-manifest.json').read_text(encoding='utf-8'))
version = manifest['version']
build = root / 'build' / a.build
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
out = root / 'build/phase4g-evidence/installer' / stamp
out.mkdir(parents=True)
uia = build / 'tests/nova-phase4f-uia-client.exe'
sdk_python = root / 'build/phase4c-venv/Scripts/python.exe'
local = pathlib.Path(os.environ['LOCALAPPDATA'])
appdata = pathlib.Path(os.environ['APPDATA'])
claude_cli = sorted(local.glob('Claude-Data/claude_*/claude-code/*/*/claude.exe'), key=lambda f: f.stat().st_mtime)[-1]
codex_cli = sorted(local.glob('OpenAI/Codex/bin/*/codex.exe'), key=lambda f: f.stat().st_mtime)[-1]
KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\NovaStudio'
INNO_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\{4160E321-01F3-4BA8-B8DF-65FEE14D6D65}_is1'
SHORTCUT = appdata / 'Microsoft/Windows/Start Menu/Programs/Nova Studio.lnk'
INNO_DIR = local / 'Programs/Nova Studio'
INNO_MENU = appdata / 'Microsoft/Windows/Start Menu/Programs/Nova Studio'
OWNER_DATA = local / 'Nova Studio'
MARKER, UNINSTALLER = 'nova-studio-install.txt', 'Uninstall Nova Studio.exe'
checks, report = [], {'candidate': a.candidate, 'installer': installer.name, 'version': version, 'run': stamp}
log = open(out / 'verify.log', 'w', encoding='utf-8')


def say(text):
    print(text, flush=True)
    log.write(text + '\n')
    log.flush()


def check(name, ok, detail=''):
    checks.append(dict(name=name, ok=bool(ok), detail=str(detail)[:600]))
    say(f'{"PASS" if ok else "FAIL"}  {name}' + (f'  ({detail})' if detail and not ok else ''))
    return ok


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def tree(path):
    path = pathlib.Path(path)
    if not path.exists():
        return None
    result = {}
    for f in sorted(path.rglob('*')):
        if f.is_file():
            st = f.stat()
            result[f.relative_to(path).as_posix()] = (st.st_size, sha256(f) if st.st_size < 64 << 20 else st.st_mtime_ns)
    return result


def registry(key=KEY):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            values, i = {}, 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(k, i)
                except OSError:
                    return values
                values[name] = value
                i += 1
    except FileNotFoundError:
        return None


def processes(name):
    out_ = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    return [line for line in out_.splitlines() if line.lower().startswith(f'"{name.lower()}"')]


def run_installer(target=None, exe=installer, timeout=600):
    # NSIS: /D= must be last and unquoted, even with spaces.
    command = f'"{exe}" /S' + (f' /D={target}' if target else '')
    started = time.perf_counter()
    code = subprocess.run(command, timeout=timeout).returncode
    say(f'      installer exit {code} in {time.perf_counter() - started:.1f}s: {command}')
    return code


def run_uninstaller(folder, timeout=300):
    # The uninstaller copies itself to %TEMP% and returns; wait for that copy (Au_.exe) to finish.
    code = subprocess.run(f'"{pathlib.Path(folder) / UNINSTALLER}" /S', timeout=timeout).returncode
    deadline = time.time() + timeout
    time.sleep(1)
    while processes('Au_.exe') and time.time() < deadline:
        time.sleep(0.25)
    return code


def payload_matches(folder):
    folder = pathlib.Path(folder)
    expected = {**{k: v['sha256'] for k, v in manifest['files'].items()},
                'package-manifest.json': sha256(payload / 'package-manifest.json')}
    actual = {f.relative_to(folder).as_posix(): f for f in folder.rglob('*') if f.is_file()}
    missing = [k for k in expected if k not in actual]
    different = [k for k in expected if k in actual and sha256(actual[k]) != expected[k]]
    extra = sorted(set(actual) - set(expected) - {MARKER, UNINSTALLER})
    return not missing and not different, dict(missing=missing[:5], different=different[:5], extra=extra, files=len(actual))


def shortcut_info(lnk):
    """Reads the Unicode target and working folder from the .lnk file itself ([MS-SHLLINK]).
    WScript.Shell is not used: it returns '?' for characters outside the ANSI code page."""
    data = pathlib.Path(lnk).read_bytes()
    flags = struct.unpack_from('<I', data, 0x14)[0]
    pos, info = 0x4C, {}
    if flags & 0x1:  # HasLinkTargetIDList
        pos += 2 + struct.unpack_from('<H', data, pos)[0]
    if flags & 0x2:  # HasLinkInfo
        size, header = struct.unpack_from('<II', data, pos)
        if header >= 0x24:
            start = end = pos + struct.unpack_from('<I', data, pos + 0x1C)[0]
            while data[end:end + 2] != b'\0\0':  # UTF-16 code units, aligned to the string start
                end += 2
            info['target'] = data[start:end].decode('utf-16-le')
        pos += size
    unicode = bool(flags & 0x80)
    for flag, key in ((0x4, 'name'), (0x8, 'relative'), (0x10, 'working'), (0x20, 'arguments'), (0x40, 'icon')):
        if flags & flag:
            count = struct.unpack_from('<H', data, pos)[0]
            raw = data[pos + 2:pos + 2 + count * (2 if unicode else 1)]
            info[key] = raw.decode('utf-16-le' if unicode else 'mbcs')
            pos += 2 + len(raw)
    info.setdefault('arguments', '')
    return info


def pe_machine(path):
    data = pathlib.Path(path).read_bytes()[:4096]
    return struct.unpack_from('<H', data, struct.unpack_from('<I', data, 0x3c)[0] + 4)[0]


# ---- Win32 helpers for the launched application
user32, kernel32, psapi, gdi32 = ctypes.windll.user32, ctypes.windll.kernel32, ctypes.windll.psapi, ctypes.windll.gdi32
# Per-monitor DPI awareness, so window rectangles and captures use physical pixels.
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
# 64-bit handles need explicit signatures.
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetDC.restype = wt.HDC
user32.GetDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]


def windows_of(pid, title=None):
    found = []

    def callback(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buffer = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buffer, 512)
            if title is None or title in buffer.value:
                found.append((hwnd, buffer.value))
        return True
    user32.EnumWindows(WNDENUMPROC(callback), 0)
    return found


def wait_window(pid, title, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = windows_of(pid, title)
        if found:
            return found[0][0]
        time.sleep(0.2)
    return None


def modules(pid):
    handle = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
    result = []
    if handle:
        array = (wt.HMODULE * 2048)()
        needed = wt.DWORD()
        if psapi.EnumProcessModulesEx(handle, array, ctypes.sizeof(array), ctypes.byref(needed), 3):
            for i in range(needed.value // ctypes.sizeof(wt.HMODULE)):
                name = ctypes.create_unicode_buffer(1024)
                psapi.GetModuleFileNameExW(handle, array[i], name, 1024)
                result.append(name.value)
        kernel32.CloseHandle(handle)
    return result


def capture(hwnd, path):
    """PrintWindow into a 32-bit DIB and write it as PNG (no imaging library needed)."""
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    screen = user32.GetDC(None)
    dc = gdi32.CreateCompatibleDC(screen)
    bitmap = gdi32.CreateCompatibleBitmap(screen, w, h)
    gdi32.SelectObject(dc, bitmap)
    user32.PrintWindow(hwnd, dc, 2)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG), ('biPlanes', wt.WORD),
                    ('biBitCount', wt.WORD), ('biCompression', wt.DWORD), ('biSizeImage', wt.DWORD),
                    ('biXPelsPerMeter', wt.LONG), ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD),
                    ('biClrImportant', wt.DWORD)]
    header = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(dc, bitmap, 0, h, pixels, ctypes.byref(header), 0)
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(dc)
    user32.ReleaseDC(None, screen)
    raw = bytearray()
    data = pixels.raw
    for y in range(h):
        row = data[y * w * 4:(y + 1) * w * 4]
        raw.append(0)
        for x in range(0, len(row), 4):
            raw += bytes((row[x + 2], row[x + 1], row[x], 255))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    pathlib.Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                                   chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def invoke(hwnd, names, label):
    result = out / f'uia-{label}.json'
    subprocess.run([str(uia), str(hwnd), 'invoke', '45000', names, str(result)], timeout=120)
    data = json.loads(result.read_text(encoding='utf-8')) if result.exists() else {}
    return data.get('failures', 1) == 0, data


# =============================================================== pre-state
say(f'Nova Studio {version} installer acceptance, run {stamp}')
before = dict(inno_dir=tree(INNO_DIR), inno_menu=tree(INNO_MENU), owner_data=tree(OWNER_DATA), inno_key=registry(INNO_KEY))
report['pre_state'] = dict(inno_install_files=len(before['inno_dir'] or {}), owner_data_files=len(before['owner_data'] or {}),
                           inno_registered=before['inno_key'] is not None)
if registry() is not None or SHORTCUT.exists():
    raise SystemExit('A Nova Studio NSIS installation already exists; uninstall it before running acceptance.')
check('installer is a 32-bit NSIS stub (expected); payload binaries are x64', pe_machine(installer) == 0x14c, hex(pe_machine(installer)))

# 1. Default folder holds the owner's Phase 3-E Inno Setup copy: refused, untouched.
code = run_installer()
check('default folder with the earlier Inno Setup copy is refused (exit 4)', code == 4, code)
check('...the earlier copy is unchanged', tree(INNO_DIR) == before['inno_dir'])
check('...no uninstall entry or shortcut was created', registry() is None and not SHORTCUT.exists())

# 2. A non-empty unrelated folder is refused.
busy = out / 'not empty'
busy.mkdir()
(busy / 'someone-elses-file.txt').write_text('keep me', encoding='utf-8')
code = run_installer(busy)
check('non-empty unrelated folder is refused (exit 4)', code == 4, code)
check('...and left exactly as it was', [f.name for f in busy.iterdir()] == ['someone-elses-file.txt'])

# 2b. A folder path too long for every installed file is refused before anything is written.
deep = out / ('deep folder ' + 'x' * max(1, 170 - len(str(out)) - 13))
code = run_installer(deep)
check(f'a {len(str(deep))}-character folder path is refused (exit 4) before anything is written', code == 4 and not deep.exists(), code)

# 3. Interrupted installation, then repair by running Setup again, then uninstall.
interrupted = out / 'interrupted install'
for delay in (0.6, 0.35, 0.2, 0.1):
    process = subprocess.Popen(f'"{installer}" /S /D={interrupted}')
    time.sleep(delay)
    still_running = process.poll() is None
    process.kill()
    process.wait()
    if still_running and (interrupted / MARKER).exists():
        break
    if interrupted.exists() and (interrupted / MARKER).exists():
        run_uninstaller(interrupted)
partial = payload_matches(interrupted)[1]['files'] if interrupted.exists() else 0
check('an interrupted installation leaves its marker and uninstaller first',
      (interrupted / MARKER).exists() and (interrupted / UNINSTALLER).exists(), partial)
report['interrupted_files_present'] = partial
code = run_installer(interrupted)
ok, detail = payload_matches(interrupted)
check('running Setup again repairs the interrupted installation', code == 0 and ok, detail)
run_uninstaller(interrupted)
check('...and its uninstaller removes it completely', not interrupted.exists() and registry() is None and not SHORTCUT.exists(),
      list(interrupted.iterdir()) if interrupted.exists() else '')

# 4. First installation into a path with spaces and non-ASCII characters.
target = out / 'install root' / 'Nova Studio Ünïcode 测试'
code = run_installer(target)
ok, detail = payload_matches(target)
check('first installation into a path with spaces and Unicode succeeds', code == 0 and ok, detail)
values = registry() or {}
report['registry'] = values
check('uninstall entry: name, version, folder, uninstaller, size',
      values.get('DisplayName') == 'Nova Studio (preview)' and values.get('DisplayVersion') == version and
      pathlib.Path(values.get('InstallLocation', '')) == target and UNINSTALLER in values.get('UninstallString', '') and
      values.get('EstimatedSize', 0) > 50000, values)
link = shortcut_info(SHORTCUT) if SHORTCUT.exists() else {}
report['shortcut'] = link
check('Start menu shortcut targets the installed nova-studio.exe',
      pathlib.Path(link.get('target', '')) == target / 'nova-studio.exe' and pathlib.Path(link.get('working', '')) == target, link)
machines = {f.relative_to(target).as_posix(): hex(pe_machine(f)) for f in target.rglob('*') if f.suffix.lower() in ('.exe', '.dll')
            and f.name != UNINSTALLER}
check('every installed program file is x64', set(machines.values()) == {'0x8664'}, machines)
check('the uninstaller is the NSIS x86 stub', pe_machine(target / UNINSTALLER) == 0x14c)

# 5. One installation per Windows user.
other = out / 'second folder'
code = run_installer(other)
check('a second installation folder is refused while one is installed (exit 4)', code == 4 and not other.exists(), code)

# 6. Launch from the Start menu shortcut's target with isolated data, MCP setup, running detection.
iso = out / 'isolated'
for name in ('localappdata', 'appdata', 'temp', 'profile', 'claude', 'codex', 'settings', 'pack/assets/nova/textures/block'):
    (iso / name).mkdir(parents=True, exist_ok=True)
(iso / 'pack/pack.mcmeta').write_text('{"pack":{"pack_format":75,"description":"installer acceptance"}}', encoding='utf-8')
system_root = os.environ['SystemRoot']
env = {k: os.environ[k] for k in ('SystemRoot', 'windir', 'SystemDrive', 'COMSPEC', 'PATHEXT', 'OS', 'NUMBER_OF_PROCESSORS',
                                  'PROCESSOR_ARCHITECTURE', 'USERNAME', 'USERDOMAIN') if k in os.environ}
env.update(PATH=os.pathsep.join([f'{system_root}\\System32', system_root, f'{system_root}\\System32\\Wbem',
                                 str(claude_cli.parent), str(codex_cli.parent)]),
           LOCALAPPDATA=str(iso / 'localappdata'), APPDATA=str(iso / 'appdata'), TEMP=str(iso / 'temp'), TMP=str(iso / 'temp'),
           USERPROFILE=str(iso / 'profile'), CLAUDE_CONFIG_DIR=str(iso / 'claude'), CODEX_HOME=str(iso / 'codex'))
(iso / 'codex/config.toml').write_text('# existing Codex settings stay\nmodel = "example"\n', encoding='utf-8')
report['tested_clients'] = dict(claude_code=str(claude_cli), codex=str(codex_cli))
settings = iso / 'settings/studio.ini'
app = subprocess.Popen([link.get('target') or str(target / 'nova-studio.exe'), '--settings', str(settings), '--project', str(iso / 'pack')],
                       cwd=link.get('working') or str(target), env=env)
hwnd = wait_window(app.pid, 'Nova Studio')
check('installed Nova Studio starts from the shortcut target without development folders', hwnd is not None)
time.sleep(2)
loaded = modules(app.pid)
outside = [m for m in loaded if not (m.lower().startswith(str(target).lower()) or m.lower().startswith(system_root.lower()))]
report['loaded_modules'] = len(loaded)
check('every loaded module comes from the installation folder or Windows', loaded and not outside, outside[:8])

ok, _ = invoke(hwnd, 'Resources|MCP Integration…|Agent Setup', 'open-setup')
check('MCP Integration › Agent Setup opens in the installed application', ok)
claude_config = iso / 'claude/.claude.json'
for attempt in range(6):
    time.sleep(1.5)  # detection runs on a worker thread
    invoke(hwnd, 'Add Nova Studio…|Add', f'add-claude-{attempt}')
    if claude_config.exists() and 'nova-studio' in claude_config.read_text(encoding='utf-8'):
        break
configured = json.loads(claude_config.read_text(encoding='utf-8')).get('mcpServers', {}) if claude_config.exists() else {}
report['claude_code_entry'] = configured.get('nova-studio')
check('Agent Setup configures the real Claude Code CLI with the INSTALLED nova-mcp.exe',
      configured.get('nova-studio', {}).get('command') == str(target / 'nova-mcp.exe'), configured)
dialog = [w for w in windows_of(app.pid) if w[1] == 'MCP Integration']
if dialog:
    capture(dialog[0][0], out / 'installed-mcp-agent-setup.png')


def claude_list():
    result = subprocess.run([str(claude_cli), 'mcp', 'list'], env=env, capture_output=True, text=True, encoding='utf-8', timeout=120)
    return result.stdout + result.stderr


listing = claude_list()
check('configuration alone grants nothing: Claude Code cannot connect before Allow',
      'nova-studio' in listing and 'Connected' not in listing.split('nova-studio', 1)[1].splitlines()[0], listing.strip())
ok, _ = invoke(hwnd, 'Access|Allow read-only', 'allow-1')
handoff = iso / 'localappdata/Nova Studio/Nova Studio/mcp/pending-connection.json'
for _ in range(50):  # UI Automation clicks are processed asynchronously by the application
    if handoff.exists():
        break
    time.sleep(0.2)
check('Allow read-only offers one connection (in the isolated profile)', ok and handoff.exists())
listing = claude_list()
(out / 'claude-mcp-list.txt').write_text(listing, encoding='utf-8')
nova_line = next((line for line in listing.splitlines() if line.startswith('nova-studio:')), '')
# Claude Code prints a check mark, or a square-root sign on consoles without UTF-8.
check('real Claude Code 2.x connects through the installed bridge after Allow',
      nova_line.endswith('Connected') and 'Failed' not in nova_line, listing.strip())
check('...and the single-use offer is withdrawn once used', not handoff.exists())
invoke(hwnd, 'Allow read-only', 'allow-2')
tools = subprocess.run([str(sdk_python), str(root / 'tests/phase4g_sdk_tools.py'), str(target / 'nova-mcp.exe')],
                       env={**env, 'PATH': env['PATH']}, capture_output=True, text=True, timeout=120)
listed = json.loads(tools.stdout) if tools.returncode == 0 else {}
report['sdk'] = listed
check('official MCP SDK lists 26 tools through the installed bridge', len(listed.get('tools', [])) == 26, tools.stderr[-400:])
# Installer refuses to replace a running copy.
before_running = tree(target)
code = run_installer(target)
check('Setup stops while Nova Studio runs from the folder (exit 5) and changes nothing', code == 5 and tree(target) == before_running, code)
user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
try:
    exit_code = app.wait(30)
except subprocess.TimeoutExpired:
    app.kill()
    exit_code = 'timeout'
check('installed Nova Studio closes cleanly', exit_code == 0, exit_code)

# A second session where Codex is the agent Nova finds first (Claude Code is not on PATH).
codex_env = dict(env, PATH=os.pathsep.join([f'{system_root}\\System32', system_root, f'{system_root}\\System32\\Wbem',
                                            str(codex_cli.parent)]), CLAUDE_CONFIG_DIR=str(iso / 'claude-unused'))
app = subprocess.Popen([str(target / 'nova-studio.exe'), '--settings', str(settings), '--project', str(iso / 'pack')],
                       cwd=str(target), env=codex_env)
hwnd = wait_window(app.pid, 'Nova Studio')
invoke(hwnd, 'Resources|MCP Integration…|Agent Setup', 'open-setup-codex')
codex_file = iso / 'codex/config.toml'
for attempt in range(6):
    time.sleep(1.5)
    invoke(hwnd, 'Add Nova Studio…|Add', f'add-codex-{attempt}')
    if 'nova-studio' in codex_file.read_text(encoding='utf-8'):
        break
codex_config = codex_file.read_text(encoding='utf-8')
codex_listing = subprocess.run([str(codex_cli), 'mcp', 'list'], env=codex_env, capture_output=True, text=True,
                               encoding='utf-8', timeout=120).stdout
(out / 'codex-mcp-list.txt').write_text(codex_listing + '\n----\n' + codex_config, encoding='utf-8')
check('Agent Setup configures the real Codex CLI with the installed bridge; existing settings and comments survive',
      codex_config.startswith('# existing Codex settings stay\nmodel = "example"\n') and str(target / 'nova-mcp.exe') in codex_listing,
      codex_config)
dialog = [w for w in windows_of(app.pid) if w[1] == 'MCP Integration']
if dialog:
    capture(dialog[0][0], out / 'installed-mcp-agent-setup-codex.png')
user32.PostMessageW(hwnd, 0x0010, 0, 0)
try:
    exit_code = app.wait(30)
except subprocess.TimeoutExpired:
    app.kill()
    exit_code = 'timeout'
check('the second session closes cleanly', exit_code == 0, exit_code)

# 7. Reinstall keeps user data and files the user added.
data_before = tree(iso / 'settings')
(target / 'notes-added-by-user.txt').write_text('mine', encoding='utf-8')
code = run_installer(target)
ok, detail = payload_matches(target)
check('reinstalling the same version succeeds and restores every installed file', code == 0 and ok, detail)
check('...files the user added to the folder are kept', (target / 'notes-added-by-user.txt').read_text(encoding='utf-8') == 'mine')
check('...settings and recovery data are unchanged', tree(iso / 'settings') == data_before)

# 8. Uninstall: only installed files go; data, user files and agent settings stay.
claude_before = claude_config.read_bytes()
run_uninstaller(target)
remaining = sorted(f.relative_to(target).as_posix() for f in target.rglob('*')) if target.exists() else []
check('uninstall removes every installed file and keeps the user-added file', remaining == ['notes-added-by-user.txt'], remaining)
check('...removes the uninstall entry and the Start menu shortcut', registry() is None and not SHORTCUT.exists())
check('...keeps settings and recovery data', tree(iso / 'settings') == data_before)
check('...does not touch AI agent configuration', claude_config.read_bytes() == claude_before)
listing = claude_list()
check('...after which the agent reports the server failed (disclosed in the uninstaller and docs)',
      'nova-studio' in listing and '✓ Connected' not in listing, listing.strip())
(target / 'notes-added-by-user.txt').unlink()
target.rmdir()

# 9. Upgrade from an older installer that shipped an extra file.
old_payload = out / 'previous payload' / 'Nova Studio'
shutil.copytree(payload, old_payload)
(old_payload / 'legacy-file-removed-in-new-version.txt').write_text('old', encoding='utf-8')
cpack = pathlib.Path(re.search(r'CMAKE_COMMAND:INTERNAL=(.*)', (build / 'CMakeCache.txt').read_text(encoding='utf-8'))[1].strip()).with_name('cpack.exe')
subprocess.run([str(cpack), '-G', 'NSIS', '--config', str(build / 'CPackConfig.cmake'), '-D', f'NOVA_PAYLOAD={old_payload}',
                '-D', 'NOVA_VERSION_OVERRIDE=0.1.0-preview.0', '-B', str(out / 'previous')], cwd=str(build), check=True,
               capture_output=True)
previous = next((out / 'previous').glob('*.exe'))
upgrade = out / 'upgrade folder'
code = run_installer(upgrade, exe=previous)
check('previous version (0.1.0-preview.0) installs', code == 0 and (registry() or {}).get('DisplayVersion') == '0.1.0-preview.0', code)
code = run_installer(upgrade)
ok, detail = payload_matches(upgrade)
check('upgrade replaces it: files match the new payload, the removed file is gone', code == 0 and ok and
      not (upgrade / 'legacy-file-removed-in-new-version.txt').exists(), detail)
check('...the uninstall entry shows the new version', (registry() or {}).get('DisplayVersion') == version)
check('...the marker names the new version', version in (upgrade / MARKER).read_text(encoding='utf-8'))
run_uninstaller(upgrade)
check('...and the upgraded installation uninstalls completely', not upgrade.exists() and registry() is None and not SHORTCUT.exists())

# 10. Nothing outside the test folders changed.
check("owner's Phase 3-E installation folder unchanged", tree(INNO_DIR) == before['inno_dir'])
check("owner's Phase 3-E Start menu folder unchanged", tree(INNO_MENU) == before['inno_menu'])
check("owner's Phase 3-E uninstall entry unchanged", registry(INNO_KEY) == before['inno_key'])
check("owner's Nova Studio data (%LOCALAPPDATA%\\Nova Studio) unchanged", tree(OWNER_DATA) == before['owner_data'])

report['checks'] = checks
report['passed'] = sum(c['ok'] for c in checks)
report['total'] = len(checks)
(out / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
say(f'{report["passed"]}/{report["total"]} checks passed; evidence in {out}')
sys.exit(0 if report['passed'] == report['total'] else 1)
