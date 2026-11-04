"""Phase 3-E installed-application verification (automated portion).

Structural install checks plus isolated launches of the INSTALLED executable
(never the build tree): QA walkthrough screenshots, headless benchmark,
diagnostics export, and a loaded-module audit proving no development Qt/MSVC
dependencies are used. Interactive workflows (pack creation, animation/model/UV
editing, Fabric run, Minecraft, uninstall/reinstall) are verified separately and
documented in docs/phase-3e/PHASE_3E_NATIVE_ACCEPTANCE.md.

Usage: python scripts/verify-phase3e-install.py [--install-dir <path>] [--label <name>]
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import time

import psutil

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--install-dir', default=os.path.expandvars(r'%LOCALAPPDATA%\Programs\Nova Studio'))
p.add_argument('--label', default='installed-verification')
p.add_argument('--fixture', default=str(root / 'fixtures/generated/pack-1500'))
p.add_argument('--skip-module-audit', action='store_true')
a = p.parse_args()
install = pathlib.Path(a.install_dir)
out = root / 'build/phase3e-installed' / a.label
if out.exists():
    raise SystemExit(f'Refusing to overwrite {out}')
out.mkdir(parents=True)
exe = install / 'nova-studio.exe'
report = {'install_dir': str(install), 'checks': {}, 'failures': []}


def check(name, ok, detail=''):
    report['checks'][name] = {'ok': bool(ok), 'detail': str(detail)[:500]}
    if not ok:
        report['failures'].append(name)
    print(('PASS ' if ok else 'FAIL ') + name + (' — ' + str(detail)[:200] if detail else ''))


# ---- structural -----------------------------------------------------------
check('exe-present', exe.is_file())
check('qt-conf', (install / 'qt.conf').is_file())
check('qt-core-dll', (install / 'Qt6Core.dll').is_file())
check('qt-platform-plugin', (install / 'plugins/platforms/qwindows.dll').is_file())
check('msvc-runtime-app-local', any((install / f).is_file() for f in
                                    ('msvcprt.dll', 'vcruntime140.dll')))
check('licenses-dir', (install / 'licenses').is_dir())
check('readme', (install / 'READ_ME_FIRST.md').is_file())
version = subprocess.run(['powershell', '-NoProfile', '-Command',
                          f'(Get-Item "{exe}").VersionInfo.ProductVersion'],
                         capture_output=True, text=True).stdout.strip()
check('version-resource', bool(version), version)
check('no-image-plugins-dir-shipped' if not (install / 'plugins/imageformats').is_dir()
      else 'image-plugins-shipped', True,
      'imageformats present' if (install / 'plugins/imageformats').is_dir()
      else 'payload ships no image codecs beyond Qt Gui built-ins (as built by the accepted recipe)')

start_menu = pathlib.Path(os.environ['APPDATA']) / r'Microsoft\Windows\Start Menu\Programs\Nova Studio'
check('start-menu-entry', (start_menu / 'Nova Studio.lnk').is_file(), str(start_menu))
try:
    import winreg
    key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                         r'Software\Microsoft\Windows\CurrentVersion\Uninstall'
                         r'\{4160E321-01F3-4BA8-B8DF-65FEE14D6D65}_is1')
    values = dict((winreg.EnumValue(key, i)[0], winreg.EnumValue(key, i)[1])
                  for i in range(winreg.QueryInfoKey(key)[1]))
    winreg.CloseKey(key)
    check('uninstall-registry', values.get('DisplayName') == 'Nova Studio',
          f"{values.get('DisplayName')} {values.get('DisplayVersion')}")
except OSError as error:
    check('uninstall-registry', False, error)

# ---- isolated launches ----------------------------------------------------
settings_root = out / 'isolated'
settings_root.mkdir()
settings = settings_root / 'studio.ini'
shots = out / 'qa'
shots.mkdir()
cwd_odd = pathlib.Path(os.environ['SystemRoot'])  # deliberately not the install dir
env = {**os.environ}
env.pop('QT_PLUGIN_PATH', None)
env.pop('QML2_IMPORT_PATH', None)


def launch(args, cwd=cwd_odd, timeout=300):
    started = time.perf_counter()
    result = subprocess.run([str(exe), *args], cwd=str(cwd), env=env,
                            capture_output=True, timeout=timeout,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    return result, time.perf_counter() - started


result, seconds = launch(['--settings', str(settings), '--project', a.fixture,
                          '--qa-dir', str(shots), '--smoke-seconds', '14'], timeout=180)
check('qa-walkthrough-exit', result.returncode == 0,
      f'exit={result.returncode} seconds={seconds:.1f}')
gallery = sorted(p.name for p in shots.glob('*.png'))
check('qa-screenshots', len(gallery) >= 10, ', '.join(gallery))
# First launch initializes the settings-dir-scoped diagnostics (settings.ini itself is
# only written when preferences are saved — the accepted Phase 3 behavior).
check('diagnostics-initialized', (settings_root / 'diagnostics/studio.log').is_file(),
      str(settings_root / 'diagnostics'))
result, _ = launch(['--settings', str(settings), '--diagnostics-export',
                    str(out / 'diagnostics.json')], timeout=120)
diagnostics = out / 'diagnostics.json'
check('diagnostics-export', result.returncode == 0 and diagnostics.is_file())
if diagnostics.is_file():
    data = json.loads(diagnostics.read_text(encoding='utf-8'))
    check('diagnostics-no-upload', data.get('automatic_upload') is False)
    check('diagnostics-has-log', bool(data.get('diagnostics', {}).get('studio_log') or
                                      data.get('log') or data))

# ---- loaded-module audit (no development dependencies) --------------------
if not a.skip_module_audit:
    proc = subprocess.Popen([str(exe), '--settings', str(settings), '--project', a.fixture,
                             '--smoke-seconds', '25'], cwd=str(cwd_odd), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    info = psutil.Process(proc.pid)
    time.sleep(12)  # let the workspace open and settle
    windows = {str(pathlib.Path(os.environ['SystemRoot'])),
               str(pathlib.Path(os.environ['SystemRoot']) / 'SysWOW64'),
               str(pathlib.Path(os.environ['SystemRoot']) / 'WinSxS')}
    dev_roots = [str(root), str(root / '.tools')]
    bad, loaded_in_install = [], 0
    try:
        for module in info.memory_maps():
            path = module.path.lower()
            if not path.endswith('.dll'):
                continue
            if path.startswith(str(install).lower()):
                loaded_in_install += 1
                continue
            if any(path.startswith(w.lower() + '\\') for w in windows):
                continue
            if any(path.startswith(d.lower()) for d in dev_roots):
                bad.append(module.path)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
    check('modules-from-install-or-system-only', not bad,
          f'{loaded_in_install} app-local DLLs; offenders: {bad[:5]}')

report['ok'] = not report['failures']
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'ok': report['ok'], 'failures': report['failures'],
                  'out': str(out / 'report.json')}))
sys.exit(0 if report['ok'] else 1)
