# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-G: undo a test installation left by an interrupted verify-phase4g-installer.py run.

Only acts on things the acceptance run created: processes started from a folder under
build/phase4g-evidence/installer, and an NSIS installation whose InstallLocation is under that
folder (removed with its own uninstaller). Never touches any other Nova Studio installation.
"""
import os
import pathlib
import subprocess
import sys
import time
import winreg

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
root = pathlib.Path(__file__).resolve().parents[1]
evidence = (root / 'build/phase4g-evidence/installer').resolve()
KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\NovaStudio'

listing = subprocess.run(['powershell', '-NoProfile', '-Command',
                          "[Console]::OutputEncoding=[Text.Encoding]::UTF8;"
                          "Get-Process nova-studio,nova-mcp -ErrorAction SilentlyContinue | "
                          "ForEach-Object { \"$($_.Id)|$($_.Path)\" }"], capture_output=True).stdout.decode('utf-8')
for line in listing.splitlines():
    pid, _, path = line.partition('|')
    if path and pathlib.Path(path).resolve().is_relative_to(evidence):
        subprocess.run(['taskkill', '/PID', pid, '/T', '/F'], capture_output=True)
        print('stopped', pid, path)
try:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
        location = pathlib.Path(winreg.QueryValueEx(key, 'InstallLocation')[0])
except FileNotFoundError:
    location = None
if location is None:
    print('no NSIS test installation registered')
elif not location.resolve().is_relative_to(evidence):
    print('registered installation is not a test installation; left alone:', location)
else:
    time.sleep(1)
    subprocess.run(f'"{location / "Uninstall Nova Studio.exe"}" /S')
    time.sleep(1)
    for _ in range(240):
        if 'Au_.exe' not in subprocess.run(['tasklist', '/FI', 'IMAGENAME eq Au_.exe'], capture_output=True, text=True).stdout:
            break
        time.sleep(0.25)
    print('uninstalled', location, 'remaining:', sorted(p.name for p in location.iterdir()) if location.exists() else 'none')
shortcut = pathlib.Path(os.environ['APPDATA'], r'Microsoft\Windows\Start Menu\Programs\Nova Studio.lnk')
print('start menu shortcut present:', shortcut.exists())
