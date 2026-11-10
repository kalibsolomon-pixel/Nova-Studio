# SPDX-License-Identifier: GPL-3.0-or-later
"""Release payload dependency policy check (Phase 4-H.2).

Checks a payload folder (an unpacked portable ZIP, or <candidate>/payload/Nova Studio) without
running any Microsoft build tool. Pure Python: PE headers and import tables are parsed here.

  binaries      exactly the expected executables and DLLs, nothing missing, nothing extra
  forbidden     no Windows SDK DirectX compilers, software OpenGL, debug or UCRT app-local DLLs
  pe            every image is x64; every import (normal and delay-load) resolves inside the
                payload, in System32, or is an API set
  msvc runtime  the 10 Visual C++ runtime DLLs share one file version and, when the Build Tools
                folder is known, are byte-identical to <redist>\\x64\\Microsoft.VC145.CRT
  qt            Qt DLLs and the platform plugin are byte-identical to the Qt kit, when given
  manifest      package-manifest.json (if present) lists exactly the payload's files and hashes
  baseline      with --baseline, files added or removed relative to an accepted payload, and
                changed third-party binaries

  python scripts/check-release-payload.py --payload <dir> [--qt <qt root>]
         [--msvc-runtime <...\\Microsoft.VC145.CRT> | --toolchain <build>/nova-toolchain.json]
         [--baseline <payload dir>] [--report <file.json>]

It never writes into the payload. Exit code 0 when every check passes, 1 otherwise.
"""
import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import re
import struct
import sys

EXECUTABLES = ['nova-studio.exe', 'nova-mcp.exe']
QT_DLLS = ['Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6OpenGL.dll', 'Qt6OpenGLWidgets.dll', 'Qt6Network.dll']
QT_PLUGINS = ['plugins/platforms/qwindows.dll']
MSVC_RUNTIME = ['concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
                'msvcp140_codecvt_ids.dll', 'vccorlib140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
                'vcruntime140_threads.dll']
EXPECTED = set(EXECUTABLES + QT_DLLS + QT_PLUGINS + MSVC_RUNTIME)
# Files that must never be in the payload: DirectX shader compilers that windeployqt copies from a
# Windows SDK on PATH, the software OpenGL rasterizer, debug runtimes, app-local Universal CRT.
FORBIDDEN = re.compile(r'(?i)(^|/)(d3dcompiler_\d+\.dll|dxcompiler\.dll|dxil\.dll|opengl32sw\.dll|'
                       r'ucrtbased?\.dll|api-ms-win-[^/]*\.dll|vcruntime140d[^/]*\.dll|msvcp140d[^/]*\.dll|'
                       r'concrt140d\.dll|vccorlib140d\.dll|Qt6\w+d\.dll)$')
BINARY = re.compile(r'(?i)\.(dll|exe|pyd|sys|ocx)$')


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


class PE:
    """Minimal PE32+ reader: machine, DLL characteristics, import and delay-import names."""

    def __init__(self, data):
        self.data = data
        if data[:2] != b'MZ':
            raise ValueError('not an MZ image')
        self.offset = struct.unpack_from('<I', data, 0x3c)[0]
        if data[self.offset:self.offset + 4] != b'PE\0\0':
            raise ValueError('no PE signature')
        coff = self.offset + 4
        self.machine, sections = struct.unpack_from('<HH', data, coff)[:2]
        optional_size = struct.unpack_from('<H', data, coff + 16)[0]
        optional = coff + 20
        magic = struct.unpack_from('<H', data, optional)[0]
        if magic != 0x20b:
            raise ValueError(f'not PE32+ (magic {magic:#x})')
        self.dll_characteristics = struct.unpack_from('<H', data, optional + 70)[0]
        count = struct.unpack_from('<I', data, optional + 108)[0]
        self.directories = [struct.unpack_from('<II', data, optional + 112 + 8 * i) for i in range(count)]
        table = optional + optional_size
        self.sections = []
        for i in range(sections):
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from('<IIII', data, table + 40 * i + 8)
            self.sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def at(self, rva):
        for address, size, pointer in self.sections:
            if address <= rva < address + size:
                return rva - address + pointer
        raise ValueError(f'RVA {rva:#x} outside all sections')

    def name(self, rva):
        start = self.at(rva)
        return self.data[start:self.data.index(b'\0', start)].decode('ascii')

    def imports(self):
        names = []
        if len(self.directories) > 1 and self.directories[1][0]:
            entry = self.at(self.directories[1][0])
            while True:
                lookup, _, _, name_rva, thunk = struct.unpack_from('<IIIII', self.data, entry)
                if not (lookup or name_rva or thunk):
                    break
                names.append(self.name(name_rva))
                entry += 20
        if len(self.directories) > 13 and self.directories[13][0]:
            entry = self.at(self.directories[13][0])
            while True:
                fields = struct.unpack_from('<8I', self.data, entry)
                if not any(fields):
                    break
                names.append(self.name(fields[1]))
                entry += 32
        return sorted(set(names), key=str.lower)


def file_version(path):
    """Fixed file version (a.b.c.d) from the version resource, or None."""
    if os.name != 'nt':
        return None
    version = ctypes.windll.version
    size = version.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return None
    buffer = ctypes.create_string_buffer(size)
    version.GetFileVersionInfoW(str(path), 0, size, buffer)
    pointer, length = ctypes.c_void_p(), ctypes.c_uint()
    if not version.VerQueryValueW(buffer, '\\', ctypes.byref(pointer), ctypes.byref(length)):
        return None
    ms, ls = struct.unpack_from('<II', ctypes.string_at(pointer.value, length.value), 8)
    return f'{ms >> 16}.{ms & 0xffff}.{ls >> 16}.{ls & 0xffff}'


def files_of(folder):
    return {p.relative_to(folder).as_posix(): p for p in sorted(folder.rglob('*')) if p.is_file()}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--payload', required=True, type=pathlib.Path)
    p.add_argument('--qt', type=pathlib.Path, help='Qt kit root (bin/, plugins/)')
    p.add_argument('--msvc-runtime', type=pathlib.Path, help='...\\VC\\Redist\\MSVC\\<ver>\\x64\\Microsoft.VC145.CRT')
    p.add_argument('--toolchain', type=pathlib.Path, help='nova-toolchain.json written by build-studio.ps1')
    p.add_argument('--baseline', type=pathlib.Path, help='an accepted payload folder to compare with')
    p.add_argument('--report', type=pathlib.Path)
    a = p.parse_args(argv)
    payload = a.payload.resolve()
    if not payload.is_dir():
        raise SystemExit(f'Not a folder: {payload}')
    if a.report and payload in a.report.resolve().parents:
        raise SystemExit('The report must not be written into the payload')
    runtime_dir = a.msvc_runtime
    if a.toolchain and not runtime_dir:
        runtime_dir = pathlib.Path(json.loads(a.toolchain.read_text(encoding='utf-8-sig'))['msvc_runtime'])
    checks = []

    def check(name, ok, detail=''):
        checks.append(dict(check=name, ok=bool(ok), detail=detail if not ok else ''))
        print(('PASS ' if ok else 'FAIL ') + name + (f' — {detail}' if detail and not ok else ''), flush=True)

    files = files_of(payload)
    binaries = {n for n in files if BINARY.search(n)}
    lower = {n.lower(): n for n in binaries}
    missing = sorted(n for n in EXPECTED if n.lower() not in lower)
    unexpected = sorted(n for n in binaries if n.lower() not in {e.lower() for e in EXPECTED})
    check(f'exactly the {len(EXPECTED)} expected executables and DLLs', not missing and not unexpected,
          dict(missing=missing, unexpected=unexpected))
    forbidden = sorted(n for n in files if FORBIDDEN.search(n))
    check('no DirectX compiler, software OpenGL, debug runtime or app-local UCRT files', not forbidden, forbidden)

    system = pathlib.Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
    present = {n.lower() for n in files if '/' not in n}
    images, bad_machine, unresolved, unreadable = {}, [], [], []
    for name in sorted(binaries):
        try:
            image = PE(files[name].read_bytes())
        except (ValueError, struct.error) as error:
            unreadable.append(f'{name}: {error}')
            continue
        if image.machine != 0x8664:
            bad_machine.append(f'{name}: {image.machine:#x}')
        imports = image.imports()
        images[name] = dict(imports=imports, dll_characteristics=hex(image.dll_characteristics))
        for imported in imports:
            low = imported.lower()
            if low in present or low.startswith(('api-ms-win-', 'ext-ms-win-')) or (system / imported).is_file():
                continue
            unresolved.append(f'{name}: {imported}')
    check('every binary is a readable PE32+ image', not unreadable, unreadable)
    check('every binary is x64', not bad_machine, bad_machine)
    check('every import resolves in the payload, System32 or an API set', not unresolved, unresolved)
    for exe in EXECUTABLES:
        flags = int(images.get(exe, {}).get('dll_characteristics', '0x0'), 16)
        check(f'{exe}: ASLR, NX and high-entropy VA', flags & 0x160 == 0x160, hex(flags))

    runtime = {n: files[lower[n.lower()]] for n in MSVC_RUNTIME if n.lower() in lower}
    versions = {n: file_version(path) for n, path in runtime.items()}
    distinct = sorted({v for v in versions.values() if v})
    check('the Visual C++ runtime DLLs share one file version', len(distinct) == 1, versions)
    runtime_source = None
    if runtime_dir:
        runtime_dir = runtime_dir.resolve()
        differs = [n for n, path in runtime.items()
                   if not (runtime_dir / n).is_file() or sha256(runtime_dir / n) != sha256(path)]
        check(f'Visual C++ runtime DLLs are byte-identical to {runtime_dir.name} of the Build Tools', not differs, differs)
        check('Visual C++ runtime folder is a redistributable (not debug_nonredist or onecore) folder',
              runtime_dir.name.startswith('Microsoft.VC') and runtime_dir.name.endswith('.CRT')
              and not any(part.lower() in ('debug_nonredist', 'onecore') for part in runtime_dir.parts), str(runtime_dir))
        runtime_source = str(runtime_dir)
    if a.qt:
        qt = a.qt.resolve()
        qt_differs = [n for n in QT_DLLS if n.lower() in lower and sha256(files[lower[n.lower()]]) != sha256(qt / 'bin' / n)]
        qt_differs += [n for n in QT_PLUGINS if n.lower() in lower and sha256(files[lower[n.lower()]]) != sha256(qt / n)]
        check('Qt DLLs and platform plugin are byte-identical to the Qt kit', not qt_differs, qt_differs)

    manifest_file = payload / 'package-manifest.json'
    manifest = None
    if manifest_file.is_file():
        manifest = json.loads(manifest_file.read_text(encoding='utf-8'))
        listed = manifest.get('files', {})
        actual = {n for n in files if n != 'package-manifest.json'}
        wrong = sorted(n for n in actual & set(listed) if listed[n]['sha256'] != sha256(files[n]))
        check('package-manifest.json lists exactly the payload files with their hashes',
              set(listed) == actual and not wrong,
              dict(unlisted=sorted(actual - set(listed)), missing=sorted(set(listed) - actual), changed=wrong))

    comparison = None
    if a.baseline:
        base = files_of(a.baseline.resolve())
        added = sorted(set(files) - set(base))
        removed = sorted(set(base) - set(files))
        third_party = set(QT_DLLS + QT_PLUGINS + MSVC_RUNTIME)
        changed_third_party = sorted(n for n in set(files) & set(base) if n in third_party and sha256(files[n]) != sha256(base[n]))
        changed_other = sorted(n for n in set(files) & set(base) if n not in third_party
                               and n != 'package-manifest.json' and sha256(files[n]) != sha256(base[n]))
        comparison = dict(baseline=str(a.baseline), added=added, removed=removed,
                          changed_third_party_binaries=changed_third_party, changed_other=changed_other)
        check('no binary added or removed relative to the baseline',
              not [n for n in added + removed if BINARY.search(n)], dict(added=added, removed=removed))
        check('third-party binaries unchanged relative to the baseline', not changed_third_party, changed_third_party)
        print(f'baseline: {len(added)} added, {len(removed)} removed, {len(changed_other)} other files changed')
        for label, items in (('A', added), ('D', removed), ('M', changed_other)):
            for item in items:
                print(f'  {label} {item}')

    report = dict(format='nova-payload-check', payload=str(payload), msvc_runtime_source=runtime_source,
                  msvc_runtime_versions=versions, images=images, baseline=comparison, checks=checks,
                  passed=sum(c['ok'] for c in checks), failed=sum(not c['ok'] for c in checks))
    if a.report:
        a.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{report['passed']}/{len(checks)} payload checks passed")
    return 1 if report['failed'] else 0


if __name__ == '__main__':
    sys.exit(main())
