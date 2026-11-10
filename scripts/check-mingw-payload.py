# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-I (experimental): dependency policy check for a GCC/MinGW-w64 release payload.

Pure Python, like scripts/check-release-payload.py (whose PE reader it reuses); runs no build tool.

  binaries     the executables, Qt DLLs, the platform plugin and the GCC runtime are present, and
               nothing else unless explicitly allowed (--allow-dll, e.g. for a prebuilt MSYS2 Qt)
  forbidden    no Visual C++ runtime, app-local UCRT, DirectX compiler, software OpenGL, debug Qt,
               development executable, import/static library, object, PDB, log, CMake or toolchain
               record in the payload
  pe           every image is x64 PE32+; every import resolves inside the payload, in System32 or
               is an API set; no image imports a Visual C++ runtime DLL or the legacy msvcrt.dll
               (that would mean an MSVC-built or non-UCRT component); executables have ASLR, NX and
               high-entropy VA
  gcc runtime  with --toolchain-bin, the GCC runtime DLLs are byte-identical to the toolchain's
  qt           with --qt, Qt DLLs and the platform plugin are byte-identical to that Qt prefix
  manifest     package-manifest.json (if present) lists exactly the payload's files and hashes
  report       transitive DLL closure, embedded application manifests, debug-section presence

  python scripts/check-mingw-payload.py --payload <dir> [--qt <prefix>] [--toolchain-bin <ucrt64\\bin>]
         [--allow-dll NAME ...] [--report <file.json>]

It never writes into the payload. Exit code 0 when every check passes, 1 otherwise.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import struct
import sys

_spec = importlib.util.spec_from_file_location('nova_payload', pathlib.Path(__file__).with_name('check-release-payload.py'))
_base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_base)
PE, files_of, BINARY = _base.PE, _base.files_of, _base.BINARY

EXECUTABLES = ['nova-studio.exe', 'nova-mcp.exe']
QT_DLLS = list(_base.QT_DLLS)
# The MinGW payload also ships Qt's Schannel TLS backend (HTTPS downloads); see NovaPackagingMinGW.cmake.
QT_PLUGINS = list(_base.QT_PLUGINS) + ['plugins/tls/qschannelbackend.dll']
GCC_RUNTIME = ['libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll']
WINDOWS_SYSTEM_IMPORTS = frozenset('''
    advapi32.dll authz.dll bcrypt.dll comdlg32.dll crypt32.dll d3d9.dll d3d11.dll d3d12.dll
    dnsapi.dll dwmapi.dll dwrite.dll dxgi.dll gdi32.dll imm32.dll iphlpapi.dll kernel32.dll
    mpr.dll mscms.dll ncrypt.dll netapi32.dll ntdll.dll ole32.dll oleaut32.dll secur32.dll
    setupapi.dll shcore.dll shell32.dll shlwapi.dll user32.dll userenv.dll uxtheme.dll
    version.dll winhttp.dll winmm.dll wintrust.dll ws2_32.dll wtsapi32.dll opengl32.dll
    ucrtbase.dll
'''.split())


def is_windows_system_import(name, system):
    return name.lower() in WINDOWS_SYSTEM_IMPORTS and (system / name).is_file()

MSVC_IMPORT = re.compile(r'(?i)^(msvcp\d+[\w]*|vcruntime\d+[\w]*|concrt\d+|vccorlib\d+|msvcr\d+[\w]*|ucrtbased)\.dll$')
FORBIDDEN = re.compile(
    r'(?i)(^|/)('
    r'msvcp\d+\w*\.dll|vcruntime\d+\w*\.dll|concrt\d+\w*\.dll|vccorlib\d+\w*\.dll|msvcr\d+\w*\.dll|'
    r'ucrtbased?\.dll|api-ms-win-[^/]*\.dll|'
    r'd3dcompiler_\d+\.dll|dxcompiler\.dll|dxil\.dll|opengl32sw\.dll|Qt6\w+d\.dll|'
    r'(g\+\+|c\+\+|gcc|cc1|cc1plus|collect2|ld|ld\.bfd|as|ar|nm|objdump|strip|windres|gdb|cmake|ctest|cpack|ninja|'
    r'moc|rcc|uic|qmake6?|windeployqt6?|qtpaths6?|syncqt|pacman|bash|sh|python\d*|pythonw)\.exe|'
    r'[^/]*\.(a|lib|o|obj|pdb|debug|ilk|exp|log)|CMakeCache\.txt|nova-(mingw-)?toolchain\.json|'
    r'nova-toolchain-manifest\.txt|[^/]*\.ini'
    r')$')


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def section_names(data):
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    coff = offset + 4
    count = struct.unpack_from('<H', data, coff + 2)[0]
    table = coff + 20 + struct.unpack_from('<H', data, coff + 16)[0]
    names = []
    symtab, nsyms = struct.unpack_from('<II', data, coff + 8)
    strings = symtab + 18 * nsyms if symtab else 0
    for i in range(count):
        raw = data[table + 40 * i:table + 40 * i + 8].rstrip(b'\0').decode('ascii', 'replace')
        if raw.startswith('/') and strings:  # long name in the COFF string table (GNU ld debug sections)
            start = strings + int(raw[1:])
            raw = data[start:data.index(b'\0', start)].decode('ascii', 'replace')
        names.append(raw)
    return names, bool(symtab and nsyms)


def embedded_manifest(data):
    start = data.find(b'<assembly')
    end = data.find(b'</assembly>', start)
    return data[start:end + 11].decode('utf-8', 'replace') if start >= 0 and end > start else None


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--payload', required=True, type=pathlib.Path)
    p.add_argument('--qt', type=pathlib.Path, help='Qt prefix (bin/, plugins/)')
    p.add_argument('--toolchain-bin', type=pathlib.Path, help='MSYS2 ucrt64\\bin that provided the GCC runtime')
    p.add_argument('--allow-dll', action='append', default=[], help='an extra top-level DLL that is accepted')
    p.add_argument('--report', type=pathlib.Path)
    a = p.parse_args(argv)
    payload = a.payload.resolve()
    if not payload.is_dir():
        raise SystemExit(f'Not a folder: {payload}')
    if a.report and payload in a.report.resolve().parents:
        raise SystemExit('The report must not be written into the payload')
    checks = []

    def check(name, ok, detail=''):
        checks.append(dict(check=name, ok=bool(ok), detail=detail if not ok else ''))
        print(('PASS ' if ok else 'FAIL ') + name + (f' — {detail}' if detail and not ok else ''), flush=True)

    files = files_of(payload)
    binaries = {n for n in files if BINARY.search(n)}
    lower = {n.lower(): n for n in binaries}
    expected = EXECUTABLES + QT_DLLS + QT_PLUGINS + GCC_RUNTIME
    allowed = {n.lower() for n in expected + a.allow_dll}
    missing = sorted(n for n in expected if n.lower() not in lower)
    unexpected = sorted(n for n in binaries if n.lower() not in allowed)
    check(f'the {len(expected)} required executables and DLLs are present', not missing, dict(missing=missing))
    check('no executable or DLL beyond the required set' + (f' and {len(a.allow_dll)} allowed extras' if a.allow_dll else ''),
          not unexpected, dict(unexpected=unexpected))
    forbidden = sorted(n for n in files if FORBIDDEN.search(n) and n != 'qt.conf')
    check('no Visual C++ runtime, debug, development-only or local-configuration file', not forbidden, forbidden)

    system = pathlib.Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
    present = {n.lower() for n in files if '/' not in n}
    images, bad_machine, unresolved, unreadable, msvc_imports = {}, [], [], [], []
    for name in sorted(binaries):
        data = files[name].read_bytes()
        try:
            image = PE(data)
        except (ValueError, struct.error) as error:
            unreadable.append(f'{name}: {error}')
            continue
        if image.machine != 0x8664:
            bad_machine.append(f'{name}: {image.machine:#x}')
        imports = image.imports()
        sections, symbols = section_names(data)
        images[name] = dict(imports=imports, dll_characteristics=hex(image.dll_characteristics), bytes=len(data),
                            debug_sections=[s for s in sections if s.startswith('.debug')], coff_symbols=symbols,
                            crt='ucrt' if any(i.lower().startswith('api-ms-win-crt-') or i.lower() == 'ucrtbase.dll' for i in imports)
                            else ('msvcrt' if any(i.lower() == 'msvcrt.dll' for i in imports) else 'none'))
        for imported in imports:
            low = imported.lower()
            if MSVC_IMPORT.match(imported) or low == 'msvcrt.dll':
                msvc_imports.append(f'{name}: {imported}')
            # A Visual C++ runtime DLL found in System32 (an installed VC++ redistributable) does
            # not count: end users' machines need not have it, and this payload must not need it.
            in_system = is_windows_system_import(imported, system)
            if low in present or low.startswith(('api-ms-win-', 'ext-ms-win-')) or in_system:
                continue
            unresolved.append(f'{name}: {imported}')
    check('every binary is a readable PE32+ image', not unreadable, unreadable)
    check('every binary is x64', not bad_machine, bad_machine)
    check('every import resolves in the payload, System32 or an API set', not unresolved, unresolved)
    check('no binary imports a Visual C++ runtime DLL or the legacy msvcrt.dll', not msvc_imports, msvc_imports)
    check('host-installed third-party System32 DLLs cannot satisfy the Windows dependency policy',
          not any(imported.lower() not in WINDOWS_SYSTEM_IMPORTS and
                  imported.lower() not in present and
                  not imported.lower().startswith(('api-ms-win-', 'ext-ms-win-'))
                  for image in images.values() for imported in image['imports']))
    for exe in EXECUTABLES:
        flags = int(images.get(exe, {}).get('dll_characteristics', '0x0'), 16)
        check(f'{exe}: ASLR, NX and high-entropy VA', flags & 0x160 == 0x160, hex(flags))

    if a.toolchain_bin:
        tb = a.toolchain_bin.resolve()
        differs = [n for n in GCC_RUNTIME if n.lower() in lower and
                   (not (tb / n).is_file() or sha256(tb / n) != sha256(files[lower[n.lower()]]))]
        check(f'GCC runtime DLLs are byte-identical to {tb}', not differs, differs)
    if a.qt:
        qt = a.qt.resolve()
        qt_differs = [n for n in QT_DLLS if n.lower() in lower and sha256(files[lower[n.lower()]]) != sha256(qt / 'bin' / n)]
        qt_differs += [n for n in QT_PLUGINS if n.lower() in lower and sha256(files[lower[n.lower()]]) != sha256(qt / n)]
        check('Qt DLLs and platform plugin are byte-identical to the Qt prefix', not qt_differs, qt_differs)

    manifest_file = payload / 'package-manifest.json'
    if manifest_file.is_file():
        listed = json.loads(manifest_file.read_text(encoding='utf-8')).get('files', {})
        actual = {n for n in files if n != 'package-manifest.json'}
        wrong = sorted(n for n in actual & set(listed) if listed[n]['sha256'] != sha256(files[n]))
        check('package-manifest.json lists exactly the payload files with their hashes',
              set(listed) == actual and not wrong,
              dict(unlisted=sorted(actual - set(listed)), missing=sorted(set(listed) - actual), changed=wrong))

    # Transitive closure from the two executables and the plugin (payload DLLs, then Windows DLLs).
    closure, pending = {}, [n for n in EXECUTABLES + QT_PLUGINS if n in images]
    while pending:
        name = pending.pop()
        for imported in images.get(name, {}).get('imports', []):
            key = lower.get(imported.lower(), imported)
            if key in closure or key == name:
                continue
            closure[key] = 'payload' if key in images else ('api-set' if imported.lower().startswith(('api-ms-', 'ext-ms-')) else 'windows')
            if key in images:
                pending.append(key)
    unused = sorted(n for n in images if n not in closure and n not in EXECUTABLES + QT_PLUGINS)
    check('every payload DLL is reachable from the executables or the plugin', not unused, unused)
    manifests = {exe: embedded_manifest(files[lower[exe.lower()]].read_bytes()) for exe in EXECUTABLES if exe.lower() in lower}

    report = dict(format='nova-mingw-payload-check', payload=str(payload), images=images, closure=closure,
                  manifests=manifests, payload_bytes=sum(f.stat().st_size for f in files.values()),
                  binary_bytes=sum(files[n].stat().st_size for n in binaries), checks=checks,
                  passed=sum(c['ok'] for c in checks), failed=sum(not c['ok'] for c in checks))
    if a.report:
        a.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{report['passed']}/{len(checks)} payload checks passed")
    return 1 if report['failed'] else 0


if __name__ == '__main__':
    sys.exit(main())
