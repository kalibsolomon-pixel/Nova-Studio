# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-G.1: GPL licensing and corresponding-source verification of a release candidate.

Reads a candidate made by scripts/package-phase4g.py and checks, without installing anything:
  repository  unmodified GPL text, copyright notice, SPDX identifiers on Nova sources
  payload     license, copyright, notices, Qt license texts for every license in Qt's SBOM for the
              shipped modules, Qt source and build configuration, version resources, bridge notice
  source      the corresponding-source archive equals the Git tree of the build revision, blob by
              blob, and holds what is needed to build, test and package
  packages    the ZIP carries the same license and source files; the installer's version resource;
              the installer's license page (opened in the real installer UI, then cancelled before
              any folder is chosen or file written)
  checksums   SHA256SUMS.txt matches the artifacts
  microsoft   (Phase 4-H.2) end-user terms for the Microsoft runtime files shipped and presented,
              every shipped runtime DLL named, Microsoft's Redist.txt unchanged, no endorsement
Writes licensing-verification.json into the candidate folder, or to --report.

Usage: python scripts/verify-phase4g1-licensing.py --candidate <label> [--skip-installer-ui] [--report <file>]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import time
import zipfile

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--candidate', required=True)
p.add_argument('--skip-installer-ui', action='store_true')
p.add_argument('--report', type=pathlib.Path, help='where to write the JSON report (default: the candidate folder)')
a = p.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_.+-]+', a.candidate):
    raise SystemExit('Candidate must be a simple directory name')
cand = root / 'build/phase4g-distribution' / a.candidate
payload = cand / 'payload' / 'Nova Studio'
manifest = json.loads((payload / 'package-manifest.json').read_text(encoding='utf-8'))
release = json.loads((cand / 'release-manifest.json').read_text(encoding='utf-8'))
version, revision = manifest['version'], manifest['source_revision']
commit = manifest['corresponding_source']['revision']
GPL3_SHA256 = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
checks = []


def check(name, ok, detail=''):
    checks.append(dict(check=name, ok=bool(ok), detail=str(detail)[:400]))
    print(('PASS ' if ok else 'FAIL ') + name + (f' — {detail}' if detail and not ok else ''), flush=True)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git(*args):
    return subprocess.run(['git', '-c', 'core.autocrlf=false', *args], cwd=root, capture_output=True,
                          check=True).stdout


def at_revision(path):
    return git('show', f'{commit}:{path}')


def blob(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


# ---------------------------------------------------------------- repository at the build revision
license_text = at_revision('LICENSE')
check('repository LICENSE is the unmodified GPL-3.0 text', sha256(license_text) == GPL3_SHA256, sha256(license_text))
cmake_text = at_revision('CMakeLists.txt').decode()
copyright_line = re.search(r'set\(NOVA_COPYRIGHT "([^"]+)"\)', cmake_text)[1]
nova_license = re.search(r'set\(NOVA_LICENSE "([^"]+)"\)', cmake_text)[1]
check('product license is GPL-3.0-or-later', nova_license == 'GPL-3.0-or-later', nova_license)
copyright_md = at_revision('COPYRIGHT.md').decode()
check('COPYRIGHT.md carries the same copyright line as the build', copyright_line in copyright_md, copyright_line)
check('COPYRIGHT.md grants GPL version 3 or later and disclaims warranty',
      'either version 3 of the' in copyright_md and 'any later version' in copyright_md
      and 'WITHOUT ANY WARRANTY' in copyright_md)
check('COPYRIGHT.md lists material that keeps its own license',
      all(k in copyright_md for k in ('gradle-wrapper-9.8.2.jar', 'CC0-1.0', 'Minecraft Java Edition client')))
decision = at_revision('LICENSE-DECISION.md').decode()
check('LICENSE-DECISION.md records GPL-3.0-or-later', 'GPL-3.0-or-later' in decision)
tracked = git('ls-tree', '-r', '--name-only', commit, '--', 'src', 'tests', 'cmake', 'packaging',
              'CMakeLists.txt').decode().split('\n')
coded = [f for f in tracked if re.search(r'(\.cpp|\.h|\.py|\.cmake|CMakeLists\.txt|\.h\.in|\.rc\.in|NSIS\.template\.in)$', f)]
missing_spdx, other_spdx = [], []
for f in coded:
    head = at_revision(f).decode('utf-8', 'replace').splitlines()[:3]
    if not any('SPDX-License-Identifier: GPL-3.0-or-later' in line for line in head):
        missing_spdx.append(f)
    text = at_revision(f).decode('utf-8', 'replace')
    for ident in re.findall(r'SPDX-License-Identifier: ([^\s"\\]+)', text):
        if ident != 'GPL-3.0-or-later':
            other_spdx.append(f'{f}: {ident}')
check(f'SPDX GPL-3.0-or-later identifier on all {len(coded)} Nova source and build files', not missing_spdx, missing_spdx)
check('no Nova source file declares a different SPDX license', not other_spdx, other_spdx)

# ---------------------------------------------------------------- payload
def read(rel):
    return (payload / rel).read_bytes()


check('payload LICENSE.txt is the unmodified GPL-3.0 text', sha256(read('LICENSE.txt')) == GPL3_SHA256)
check('payload COPYRIGHT.md equals the build revision', read('licenses/COPYRIGHT.md') == at_revision('COPYRIGHT.md').replace(b'\n', b'\r\n')
      or read('licenses/COPYRIGHT.md') == at_revision('COPYRIGHT.md'))
notices = read('licenses/THIRD_PARTY_NOTICES.md').decode('utf-8').replace('\r\n', '\n')
check('third-party notices equal the build revision', notices == at_revision('packaging/THIRD_PARTY_NOTICES.md').decode())
check('third-party notices state the GPL and the LGPL path for Qt',
      'GPL-3.0-or-later' in notices and re.search(r'uses Qt under the\s+LGPL-3\.0-only', notices))
check('no pre-GPL license decision in the payload', not any('LICENSE-DECISION' in f for f in manifest['files']))
readme = read('README.txt').decode('utf-8')
check('README.txt has the copyright, GPL grant, warranty disclaimer and source location',
      all(k in readme for k in (copyright_line, 'GPL-3.0-or-later', 'NO WARRANTY', 'source\\', revision.split('+')[0]))
      and 'LICENSE-DECISION' not in readme)
qt_licenses = payload / 'licenses/qt/LICENSES'
check('Qt LGPL-3.0 and the GPL-3.0 text it incorporates are shipped',
      (qt_licenses / 'LGPL-3.0-only.txt').is_file() and (qt_licenses / 'GPL-3.0-only.txt').is_file())
qt_source = json.loads(read('licenses/qt-source.json'))
check('Qt Base source archive matches its published SHA-256',
      sha256(read('licenses/qtbase-everywhere-src-6.12.0.tar.xz')) == qt_source['sha256'] ==
      'a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307')
check('Qt build configuration is shipped unchanged',
      read('licenses/qt-build/config_qtbase.opt') == (root / '.tools/qt/config_qtbase.opt').read_bytes())
sbom = json.loads(read('licenses/qtbase-6.12.0.spdx.json'))
shipped = ('Core', 'Gui', 'Widgets', 'OpenGL', 'OpenGLWidgets', 'Network', 'QWindowsIntegrationPlugin')
# Bundled libraries used by the shipped build (config_qtbase.summary: not the system copies).
# BundledLibjpeg* serve only the JPEG image plugin, which is not shipped.
bundled = ('BundledPcre2', 'BundledZLIB', 'BundledLibpng', 'BundledFreetype', 'BundledHarfbuzz')
ids, without_text = set(), []
for package in sbom['packages']:
    if package['name'] in shipped + bundled or package['name'].split('_Attribution_')[0] in shipped and '_Attribution_' in package['name']:
        for ident in re.findall(r'[A-Za-z0-9.+-]+', package.get('licenseConcluded', '')):
            if ident not in ('OR', 'AND', 'WITH', 'NOASSERTION'):
                ids.add(ident)
# Every license file named by a Qt attribution (component copyright notices) must be shipped too;
# such a file also counts as the text of the attribution's license identifiers.
by_id, unshipped = {}, []
for attribution in (payload / 'licenses/qt').rglob('qt_attribution.json'):
    data = json.loads(attribution.read_text(encoding='utf-8'), strict=False)
    for entry in data if isinstance(data, list) else [data]:
        for name in entry.get('LicenseFiles') or ([entry['LicenseFile']] if entry.get('LicenseFile') else []):
            target = (attribution.parent / name).resolve()
            if not target.is_file():
                unshipped.append(f"{entry.get('Id')}: {name}")
            for ident in re.findall(r'[A-Za-z0-9.+-]+', entry.get('LicenseId', '')):
                by_id.setdefault(ident, []).append(target)
check('every license file named by a Qt attribution is shipped', not unshipped, unshipped)
for ident in sorted(ids):
    if not (qt_licenses / f'{ident}.txt').is_file() and not any(t.is_file() for t in by_id.get(ident, [])):
        without_text.append(ident)
check(f'license text shipped for each of the {len(ids)} licenses in Qt\'s SBOM for the shipped modules', not without_text, without_text)
jar = zipfile.ZipFile(root / 'assets/fabric-runtime/gradle-wrapper-9.8.2.jar')
check('Gradle wrapper license equals the one inside the embedded jar',
      read('licenses/gradle-wrapper-9.8.2-LICENSE.txt') == jar.read('META-INF/LICENSE'))
check('Microsoft redistribution terms are shipped', (payload / 'licenses/MICROSOFT-REDIST.txt').stat().st_size > 0)
check('build guide is shipped', b'Building Nova Studio from source' in read('docs/BUILDING.md'))

# ---------------------------------------------------------------- Microsoft runtime (Phase 4-H.2)
MSVC_RUNTIME = ['concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
                'msvcp140_codecvt_ids.dll', 'vccorlib140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
                'vcruntime140_threads.dll']
shipped_runtime = sorted(f for f in manifest['files'] if '/' not in f
                         and re.match(r'(?i)(msvcp|vcruntime|concrt|vccorlib)\d', f) and f.lower().endswith('.dll'))
check('the payload ships exactly the 10 Microsoft Visual C++ runtime DLLs', shipped_runtime == MSVC_RUNTIME, shipped_runtime)
terms_rel = 'licenses/MICROSOFT-VC-RUNTIME-TERMS.txt'
terms = (payload / terms_rel).read_bytes() if (payload / terms_rel).is_file() else b''
try:
    terms_source = at_revision('packaging/MICROSOFT-VC-RUNTIME-TERMS.txt')
except subprocess.CalledProcessError:
    terms_source = None
check('end-user terms for the Microsoft runtime files are shipped and equal the build revision',
      terms and terms_source is not None and terms.replace(b'\r\n', b'\n') == terms_source.replace(b'\r\n', b'\n'), terms_rel)
terms_text = terms.decode('utf-8', 'replace')
check('the runtime terms name every shipped Microsoft runtime DLL', terms and all(n in terms_text for n in shipped_runtime),
      [n for n in shipped_runtime if n not in terms_text])
flat_terms = ' '.join(terms_text.split())
check('the runtime terms are marked as not Microsoft\'s text, not GPL, and not endorsed by Microsoft',
      all(k in flat_terms for k in ("not Microsoft's license text", 'NOT COVERED BY THE GPL', 'does not endorse')))
check('the runtime terms require agreement and pass-on to further distributors',
      all(k in flat_terms for k in ('you agree to these terms', 'require each person who receives the files from you to agree')))
check('third-party notices name every shipped runtime DLL, the terms file and that the GPL does not cover them',
      all(n in notices for n in shipped_runtime) and 'MICROSOFT-VC-RUNTIME-TERMS.txt' in notices
      and 'not covered by the GPL' in ' '.join(notices.split()) and 'does not endorse' in ' '.join(notices.split()))
check('README.txt points to the runtime terms', 'MICROSOFT-VC-RUNTIME-TERMS.txt' in readme)
redist_txt = read('licenses/MICROSOFT-REDIST.txt')
toolchain = manifest.get('toolchain') or {}
check("MICROSOFT-REDIST.txt is Microsoft's Build Tools Redist.txt, unmodified",
      b'Distributable Code for Microsoft Visual Studio' in redist_txt
      and (not toolchain or redist_txt == pathlib.Path(toolchain['redist_txt']).read_bytes()))

ver = ctypes.windll.version


def version_strings(path):
    size = ver.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return {}
    buffer = ctypes.create_string_buffer(size)
    ver.GetFileVersionInfoW(str(path), 0, size, buffer)
    pointer, length = ctypes.c_void_p(), wt.UINT()
    ver.VerQueryValueW(buffer, '\\VarFileInfo\\Translation', ctypes.byref(pointer), ctypes.byref(length))
    language, codepage = ctypes.cast(pointer, ctypes.POINTER(wt.WORD))[0:2]
    values = {}
    for key in ('ProductName', 'ProductVersion', 'FileVersion', 'CompanyName', 'LegalCopyright', 'Comments'):
        if ver.VerQueryValueW(buffer, f'\\StringFileInfo\\{language:04x}{codepage:04x}\\{key}',
                              ctypes.byref(pointer), ctypes.byref(length)) and length.value:
            values[key] = ctypes.wstring_at(pointer.value, length.value).rstrip('\0')
    return values


expected_legal = f'{copyright_line}. Licensed under GPL-3.0-or-later.'
resources = {}
for exe in ('nova-studio.exe', 'nova-mcp.exe'):
    values = resources[exe] = version_strings(payload / exe)
    check(f'{exe} version resource: version, revision and copyright',
          values.get('ProductVersion') == version and values.get('LegalCopyright') == expected_legal
          and revision in values.get('Comments', ''), values)
bare = {'PATH': r'C:\Windows\System32;C:\Windows', 'SystemRoot': r'C:\Windows'}
notice = subprocess.run([str(payload / 'nova-mcp.exe'), '--license'], capture_output=True, text=True, env=bare, timeout=30)
check('nova-mcp.exe --license prints copyright, GPL and no-warranty notice',
      notice.returncode == 0 and copyright_line in notice.stdout and 'GPL-3.0-or-later' in notice.stdout
      and 'NO WARRANTY' in notice.stdout, notice.stdout + notice.stderr)

# ---------------------------------------------------------------- corresponding source
info = manifest['corresponding_source']
source = payload / info['file']
check('corresponding source is for the build revision, from clean committed sources',
      info['matches_binaries'] and commit == revision and re.fullmatch(r'[0-9a-f]{40}', revision))
prefix = f'nova-studio-{version}-source/'
expected = {}
for line in git('ls-tree', '-r', '--full-tree', commit, '--', *info['paths']).decode().splitlines():
    meta, path = line.split('\t', 1)
    expected[path] = meta.split()[2]
archived = {}
with zipfile.ZipFile(source) as z:
    for entry in z.infolist():
        if not entry.is_dir():
            archived[entry.filename.removeprefix(prefix)] = blob(z.read(entry))
check('source archive records its revision', archived.pop('.nova-source-revision', '') == blob(commit.encode()))
check(f'source archive equals the Git tree of {commit[:8]} blob by blob ({len(expected)} files)', archived == expected,
      sorted(set(archived) ^ set(expected))[:10])
needed = ['CMakeLists.txt', 'LICENSE', 'COPYRIGHT.md', 'src/CMakeLists.txt', 'src/mcp/bridge.cpp', 'src/app/main.cpp',
          'tests/CMakeLists.txt', 'cmake/NovaPackaging.cmake', 'packaging/nsis/NSIS.template.in',
          'packaging/CPackProjectConfig.cmake', 'packaging/THIRD_PARTY_NOTICES.md', 'assets/branding/nova-studio-icon.ico',
          'assets/fabric-runtime/gradle-wrapper-9.8.2.jar', 'fixtures/phase-2c5-models/pack.mcmeta',
          'scripts/build-studio.ps1', 'scripts/package-phase4g.py', 'scripts/install-qt.py',
          'scripts/prepare-phase3-qt.py', 'docs/release/BUILDING.md',
          # Phase 4-H.2: toolchain pinning, runtime selection, payload policy, runtime terms
          'scripts/nova-toolchain.ps1', 'scripts/run-ctest.ps1', 'scripts/check-release-payload.py',
          'cmake/NovaMsvcRuntime.cmake', 'packaging/MICROSOFT-VC-RUNTIME-TERMS.txt']
check('source archive holds the build, test, install and packaging inputs', all(n in archived for n in needed),
      [n for n in needed if n not in archived])
cmake_sources = set(re.findall(r'[\w/]+\.(?:cpp|h|rc\.in|h\.in)', at_revision('src/CMakeLists.txt').decode()))
check('every source named by src/CMakeLists.txt is in the archive',
      all(('src/' + s) in archived for s in cmake_sources), [s for s in cmake_sources if 'src/' + s not in archived])
check('source archive carries no historical evidence or Minecraft captures',
      not any(n.startswith(('docs/phase', 'prototypes/', 'logs/')) for n in archived))

# ---------------------------------------------------------------- packages
zip_path = cand / release['artifacts']['ZIP']['file']
with zipfile.ZipFile(zip_path) as z:
    top = z.namelist()[0].split('/')[0] + '/'
    check('ZIP: LICENSE.txt is the GPL-3.0 text', sha256(z.read(top + 'LICENSE.txt')) == GPL3_SHA256)
    check('ZIP: corresponding source equals the payload copy', z.read(top + info['file']) == source.read_bytes())
    check('ZIP: copyright and third-party notices present',
          z.read(top + 'licenses/COPYRIGHT.md') == read('licenses/COPYRIGHT.md')
          and z.read(top + 'licenses/THIRD_PARTY_NOTICES.md') == read('licenses/THIRD_PARTY_NOTICES.md'))
installer = cand / release['artifacts']['NSIS']['file']
installer_values = version_strings(installer)
check('installer version resource: version and copyright',
      installer_values.get('ProductVersion') == version and installer_values.get('LegalCopyright') == expected_legal,
      installer_values)
sums = dict(reversed(line.split('  ', 1)) for line in (cand / 'SHA256SUMS.txt').read_text(encoding='utf-8').splitlines())
check('SHA256SUMS.txt covers installer, ZIP and source archive and matches them',
      len(sums) == 3 and all(sha256((cand / n).read_bytes()) == h for n, h in sums.items()), sums)

# ---------------------------------------------------------------- installer license page (real UI)
license_page = None
if not a.skip_installer_ui:
    user32 = ctypes.windll.user32
    WM_GETTEXT, WM_GETTEXTLENGTH, WM_COMMAND, BN_CLICKED = 0x000D, 0x000E, 0x0111, 0
    EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def text_of(hwnd):
        n = user32.SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0)
        buffer = ctypes.create_unicode_buffer(n + 1)
        user32.SendMessageW(hwnd, WM_GETTEXT, n + 1, buffer)
        return buffer.value

    def class_of(hwnd):
        buffer = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buffer, 256)
        return buffer.value

    def windows_of(pid):
        found = []

        def each(hwnd, _):
            owner = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True
        user32.EnumWindows(EnumProc(each), 0)
        return found

    def descendants(hwnd):
        found = []

        def each(child, _):
            found.append(child)
            return True
        user32.EnumChildWindows(hwnd, EnumProc(each), 0)
        return found

    def wait(predicate, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.2)
        return None

    def click(dialog, control_id):
        user32.PostMessageW(dialog, WM_COMMAND, control_id | (BN_CLICKED << 16), user32.GetDlgItem(dialog, control_id))

    process = subprocess.Popen([str(installer)])
    try:
        setup = wait(lambda: next((w for w in windows_of(process.pid) if text_of(w).endswith('Setup')), None))
        check('installer UI opens', setup, 'no Setup window')
        rich = None
        if setup:
            click(setup, 1)  # Next on the welcome page
            rich = wait(lambda: next((c for c in descendants(setup) if class_of(c).lower().startswith('richedit')
                                      and user32.IsWindowVisible(c) and len(text_of(c)) > 30000), None))
        page = text_of(rich) if rich else ''
        next_text = text_of(user32.GetDlgItem(setup, 1)) if setup else ''
        texts = [text_of(c) for c in descendants(setup)] if setup else []
        license_page = dict(next_button=next_text, characters=len(page), first_line=page.split('\r')[0])
        normalized = page.replace('\r\n', '\n').replace('\r', '\n')
        check('installer license page shows the Nova notice and the full GPL-3.0 text',
              normalized.startswith(f'Nova Studio {version}\n{copyright_line}\n')
              and license_text.decode() in normalized, license_page)
        check('installer license page only informs: Next button, not an agreement',
              next_text.replace('&', '').startswith('Next') and any('do not have to accept' in t for t in texts),
              dict(next_button=next_text))
        check('installer license page presents the Microsoft runtime terms and how they are accepted',
              terms and terms_text.replace('\r\n', '\n') in normalized
              and any('runtime terms' in t and 'you agree to them' in t for t in texts))
        if setup:
            click(setup, 2)  # Cancel; MUI asks to confirm
            box = wait(lambda: next((w for w in windows_of(process.pid) if w != setup and class_of(w) == '#32770'), None), 10)
            if box:
                click(box, 6)  # IDYES
        process.wait(timeout=30)
        check('installer cancelled from its license page without installing', process.returncode is not None)
    finally:
        if process.poll() is None:
            process.kill()

report = dict(candidate=a.candidate, version=version, revision=revision, copyright=copyright_line,
              license=nova_license, version_resources=dict(resources, installer=installer_values),
              installer_license_page=license_page, checks=checks,
              passed=sum(c['ok'] for c in checks), failed=sum(not c['ok'] for c in checks))
(a.report or cand / 'licensing-verification.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(f"{report['passed']}/{len(checks)} licensing checks passed")
sys.exit(1 if report['failed'] else 0)
