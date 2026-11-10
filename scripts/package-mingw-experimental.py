# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-I.1 (EXPERIMENTAL): package a GCC/MinGW-w64 build as a portable ZIP and NSIS installer.

The GCC counterpart of scripts/package-phase4g.py. It never touches build/phase4g-distribution (the
MSVC candidates) and its packages are named *-windows-x64-mingw-experimental, so they cannot be
mistaken for an MSVC candidate. Not for publication.

  1. Refuses anything but a Release build made by scripts/build-mingw.ps1 (nova-mingw-toolchain.json,
     whose g++ must be the configured compiler), and product/packaging sources that differ from HEAD
     (unless --allow-modified, recorded in the manifest).
  2. `cmake --install` creates the payload (cmake/NovaPackagingMinGW.cmake).
  3. Corresponding source: `git archive` of the build revision, checked blob by blob against Git,
     copied into the payload's source/ folder.
  4. Validates the payload: required files and notices, the unmodified GPL text, the Qt source
     archive checksum, the Qt build record, and nova-mcp.exe's version, self-test and licence with
     only Windows folders on PATH.
  5. Writes package-manifest.json (same schema as the MSVC candidates, so the installer acceptance
     harness can use it), then runs scripts/check-mingw-payload.py on the final payload.
  6. CPack NSIS and ZIP from that exact payload; the ZIP must equal the payload byte for byte.
  7. SHA256SUMS.txt and release-manifest.json. Never signs, publishes or overwrites.

Output: build/phase4i-distribution/<label>/
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from xml.etree import ElementTree

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', default='mingw-ucrt64-source-Release')
p.add_argument('--label')
p.add_argument('--allow-modified', action='store_true', help='development rehearsal only; marked in the manifest')
p.add_argument('--source-tree', help='immutable qualification Git tree; verified against packaged source files')
p.add_argument('--gcc-runtime-source', type=pathlib.Path,
               default=root / 'build/phase4i2-evidence/compiler-source/gcc-runtime-source.json')
a = p.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_-]+', a.build):
    raise SystemExit('Build name must be a simple directory name')
build = root / 'build' / a.build
cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache:
    raise SystemExit('Only a Release build can be packaged')
toolchain_file = build / 'nova-mingw-toolchain.json'
if not toolchain_file.is_file():
    raise SystemExit('nova-mingw-toolchain.json is missing: configure and build with scripts/build-mingw.ps1')
toolchain = json.loads(toolchain_file.read_text(encoding='utf-8-sig'))
compiler = pathlib.Path(re.search(r'(?m)^CMAKE_CXX_COMPILER:[A-Z]+=(.*)', cache)[1].strip())
toolchain_bin = pathlib.Path(toolchain['gcc_runtime'])
if compiler.resolve() != (toolchain_bin / 'g++.exe').resolve():
    raise SystemExit('nova-mingw-toolchain.json does not describe the compiler this build was configured with')
qt_root = pathlib.Path(re.search(r'(?m)^Qt6_DIR:[A-Z]+=(.*)', cache)[1].strip()).parents[2]
if qt_root.resolve() != pathlib.Path(toolchain['qt']).resolve():
    raise SystemExit('The build uses a different Qt than nova-mingw-toolchain.json records')
identity = (build / 'generated/build_identity.h').read_text(encoding='utf-8')
revision = re.search(r'BuildRevision\[\] = "([^"]+)"', identity)[1]
version = re.search(r'AppVersion\[\] = "([^"]+)"', identity)[1]
packaging_paths = ['src', 'CMakeLists.txt', 'cmake', 'packaging', 'docs/release', 'LICENSE', 'COPYRIGHT.md',
                   'assets/branding', 'assets/fabric-runtime', 'assets/fonts']
# Everything needed to configure, build, test, install and package the binaries with GCC.
SOURCE_PATHS = ['.clang-format', '.gitattributes', '.gitignore', 'CMakeLists.txt', 'CMakePresets.json', 'LICENSE',
                'COPYRIGHT.md', 'LICENSE-DECISION.md', 'README.md', 'src', 'tests', 'cmake', 'packaging',
                'assets/branding', 'assets/fabric-runtime', 'assets/fonts', 'fixtures', 'docs/release', 'docs/phase-4i/MINGW_BUILD_GUIDE.md',
                'scripts']
source_tree = a.source_tree
snapshot_verified = False
if source_tree:
    if not re.fullmatch(r'[a-f0-9]{40}', source_tree):
        raise SystemExit('--source-tree must be a full Git tree SHA')
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    if revision != f'{head}+snapshot.{source_tree}':
        raise SystemExit('Executable identity does not match HEAD plus the qualification source tree')
    subprocess.run([sys.executable, '-I', str(root / 'scripts/phase4i2-source-snapshot.py'),
                    '--verify', source_tree], cwd=root, check=True)
    snapshot_verified = True
clean = bool(re.fullmatch(r'[a-f0-9]{40}', revision))
if clean:
    for path in packaging_paths:
        expected = subprocess.run(['git', 'rev-parse', f'{revision}:{path}'], cwd=root, capture_output=True, text=True).stdout
        actual = subprocess.run(['git', 'rev-parse', f'HEAD:{path}'], cwd=root, capture_output=True, text=True).stdout
        if expected != actual:
            clean = False
    if subprocess.call(['git', 'diff', '--quiet', 'HEAD', '--', *packaging_paths], cwd=root):
        clean = False
if not clean and not snapshot_verified and not a.allow_modified:
    raise SystemExit('Commit product and packaging sources, rebuild, then package (or use --allow-modified for a rehearsal)')
label = a.label or f'nova-studio-{version}-windows-x64-mingw-experimental-{revision[:8]}'
if not re.fullmatch(r'[A-Za-z0-9_.+-]+', label):
    raise SystemExit('Label must be a simple directory name')
out = root / 'build/phase4i-distribution' / label
out.mkdir(parents=True, exist_ok=False)
payload = out / 'payload' / 'Nova Studio'
cmake = pathlib.Path(re.search(r'(?m)^CMAKE_COMMAND:[A-Z]+=(.*)', cache)[1].strip())
cpack = cmake.with_name('cpack.exe')
if cmake.parent.resolve() != toolchain_bin.resolve():
    raise SystemExit(f'The build was configured with a CMake outside the toolchain: {cmake}')
log = open(out / 'packaging.log', 'w', encoding='utf-8')
system = pathlib.Path(os.environ['SystemRoot']) / 'System32'
# Process-local PATH: the toolchain, then Windows. No Visual Studio, Windows SDK or other MinGW.
tool_env = dict(os.environ)
tool_env['PATH'] = os.pathsep.join([str(toolchain_bin), str(system), os.environ['SystemRoot']])


def run(cmd, **kw):
    log.write('> ' + ' '.join(map(str, cmd)) + '\n')
    log.flush()
    result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', **kw)
    log.write(result.stdout + result.stderr + '\n')
    log.flush()
    if result.returncode:
        raise SystemExit(f'Failed ({result.returncode}): {cmd[0]}; see {out / "packaging.log"}')
    return result


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def git_blob_sha1(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


# ---- 2. payload
# --strip removes the COFF symbol tables GNU ld leaves in nova-studio.exe and nova-mcp.exe (the MSVC
# executables keep symbols in separate PDB files). Qt and the GCC runtime arrive already stripped.
run([str(cmake), '--install', str(build), '--prefix', str(payload), '--strip'], env=tool_env)

# ---- 3. corresponding source
source_commit = source_tree or (revision if clean else run(['git', 'rev-parse', 'HEAD'], cwd=root).stdout.strip())
source_prefix = f'nova-studio-{version}-source/'
source_name = f'nova-studio-{version}-source.zip'
source_archive = out / source_name
run(['git', '-c', 'core.autocrlf=false', 'archive', '--format=zip', f'--prefix={source_prefix}',
     f'--add-virtual-file={source_prefix}.nova-source-revision:{source_commit}', '-o', str(source_archive),
     source_commit, '--', *SOURCE_PATHS], cwd=root)
listing = run(['git', 'ls-tree', '-r', '--full-tree', source_commit, '--', *SOURCE_PATHS], cwd=root).stdout
expected_blobs = {}
for line in listing.splitlines():
    meta, path = line.split('\t', 1)
    mode, kind, obj = meta.split()
    if kind != 'blob':
        raise SystemExit(f'Unexpected {kind} in the source tree: {path}')
    expected_blobs[path] = obj
archived_blobs = {}
with zipfile.ZipFile(source_archive) as z:
    for info in z.infolist():
        if info.is_dir():
            continue
        if not info.filename.startswith(source_prefix):
            raise SystemExit(f'Source archive entry outside its folder: {info.filename}')
        archived_blobs[info.filename[len(source_prefix):]] = git_blob_sha1(z.read(info))
if archived_blobs.pop('.nova-source-revision', None) != git_blob_sha1(source_commit.encode()):
    raise SystemExit('Source archive revision file is missing or wrong')
if archived_blobs != expected_blobs:
    raise SystemExit('The source archive does not match the Git tree of the build revision')
(payload / 'source').mkdir()
shutil.copy2(source_archive, payload / 'source' / source_name)
source_info = dict(file=f'source/{source_name}', revision=revision, git_tree=source_commit,
                   matches_binaries=clean or snapshot_verified,
                   files=len(expected_blobs), paths=SOURCE_PATHS, sha256=sha256(source_archive),
                   bytes=source_archive.stat().st_size)

# ---- 4. validation
files = sorted(f for f in payload.rglob('*') if f.is_file())
names = {f.relative_to(payload).as_posix() for f in files}
required = {'nova-studio.exe', 'nova-mcp.exe', 'qt.conf', 'README.txt', 'plugins/platforms/qwindows.dll',
            'plugins/tls/qschannelbackend.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6OpenGL.dll',
            'Qt6OpenGLWidgets.dll', 'Qt6Network.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll',
            'LICENSE.txt', 'licenses/COPYRIGHT.md', 'licenses/THIRD_PARTY_NOTICES.md',
            'licenses/gcc-runtime/COPYING3', 'licenses/gcc-runtime/COPYING.RUNTIME', 'licenses/winpthreads/COPYING',
            'licenses/mingw-w64-runtime/COPYING.MinGW-w64-runtime.txt', 'licenses/qt-build/nova-qtbase-build.json',
            'licenses/qt-build/config_qtbase.summary', f'source/{source_name}', 'docs/BUILDING.md',
            'licenses/qt-source.json', 'licenses/qtbase-everywhere-src-6.12.0.tar.xz',
            'licenses/gradle-wrapper-9.8.2-LICENSE.txt', 'licenses/MaterialSymbolsOutlined-LICENSE.txt',
            'docs/RELEASE_NOTES.md', 'docs/INSTALLATION.md',
            'docs/FIRST_RUN.md', 'docs/MCP_SETUP.md', 'docs/SEARCH.md', 'docs/KNOWN_LIMITATIONS.md'}
missing = required - names
if not {'licenses/qtbase-6.12.0.spdx', 'licenses/qtbase-6.12.0.spdx.json'} & names:
    missing.add('licenses/qtbase-6.12.0.spdx (Qt SBOM, either form)')
if missing:
    raise SystemExit(f'Payload is missing: {sorted(missing)}')
microsoft = [n for n in names if re.search(r'(?i)microsoft|vcruntime|msvcp|concrt|vccorlib', n)]
if microsoft:
    raise SystemExit(f'Microsoft runtime material in a GCC payload: {microsoft}')
metadata = json.loads((payload / 'licenses/qt-source.json').read_text(encoding='utf-8'))
if sha256(payload / 'licenses/qtbase-everywhere-src-6.12.0.tar.xz') != metadata['sha256']:
    raise SystemExit('Qt source archive integrity failure')
qt_build = json.loads((payload / 'licenses/qt-build/nova-qtbase-build.json').read_text(encoding='utf-8-sig'))
if qt_build.get('source_sha256') != metadata['sha256']:
    raise SystemExit('The shipped Qt source archive does not match the build record')
for patch in qt_build.get('patches', []):
    name = patch.get('file', '')
    if not name or pathlib.PurePath(name).name != name or '/' in name or '\\' in name:
        raise SystemExit('Invalid Qt patch filename')
    shipped = payload / 'licenses/qt-build' / name
    if not shipped.is_file() or sha256(shipped) != patch.get('sha256'):
        raise SystemExit(f'Qt corresponding-source patch missing or changed: {name}')
GPL3_SHA256 = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
if sha256(payload / 'LICENSE.txt') != GPL3_SHA256:
    raise SystemExit('LICENSE.txt is not the unmodified GPL-3.0 text')
if (payload / 'licenses/MaterialSymbolsOutlined-LICENSE.txt').read_bytes() != (
        root / 'assets/fonts/MaterialSymbolsOutlined-LICENSE.txt').read_bytes():
    raise SystemExit('The installed Material Symbols license differs from the source asset')
if any('LICENSE-DECISION' in n for n in names) or 'LICENSE-DECISION' in (payload / 'README.txt').read_text(encoding='utf-8'):
    raise SystemExit('The payload still carries the pre-GPL license decision')
notices = (payload / 'licenses/THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
for needed in ('GCC Runtime Library Exception', 'winpthreads', 'MinGW-w64', 'Qt'):
    if needed not in notices:
        raise SystemExit(f'THIRD_PARTY_NOTICES.md does not mention {needed}')
for needed in ('FreeType Project', '2025', 'Public Suffix List', 'MPL-2.0', 'corresponding'):
    if needed not in notices:
        raise SystemExit(f'Third-party attribution missing: {needed}')
if not (payload / 'licenses/qt/LICENSES/LGPL-3.0-only.txt').is_file():
    raise SystemExit('LGPL license text is missing')
for document in ('BUILDING.md', 'RELEASE_NOTES.md', 'KNOWN_LIMITATIONS.md'):
    text = (payload / 'docs' / document).read_text(encoding='utf-8')
    if 'MICROSOFT-VC-RUNTIME-TERMS' in text or 'msvcp140' in text or 'win64_msvc2022_64' in text:
        raise SystemExit(f'Stale MSVC redistribution instructions: {document}')
pe, mitigations = {}, {}
for f in files:
    if f.suffix.lower() not in ('.dll', '.exe'):
        continue
    rel = f.relative_to(payload).as_posix()
    data = f.read_bytes()
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    machine = struct.unpack_from('<H', data, offset + 4)[0]
    flags = struct.unpack_from('<H', data, offset + 24 + 70)[0]
    if machine != 0x8664:
        raise SystemExit(f'{rel} is not x64 (machine {machine:#x})')
    pe[rel] = dict(machine='x64', dll_characteristics=hex(flags))
for exe in ('nova-studio.exe', 'nova-mcp.exe'):
    data = (payload / exe).read_bytes()
    start = data.find(b'<assembly')
    text = data[start:data.find(b'</assembly>', start) + 11].decode('utf-8', 'replace') if start >= 0 else ''
    (out / f'{exe}.manifest.xml').write_text(text, encoding='utf-8')
    # GNU ld links MinGW-w64's default manifest (windows-default-manifest): asInvoker without a
    # uiAccess attribute, whose default is false. Exactly one requestedExecutionLevel, asInvoker,
    # and uiAccess absent or false; anything else (requireAdministrator, uiAccess=true) fails.
    try:
        levels = [e for e in ElementTree.fromstring(text).iter() if e.tag.endswith('}requestedExecutionLevel')]
    except ElementTree.ParseError:
        levels = []
    ui_access = levels[0].get('uiAccess', 'false').lower() if len(levels) == 1 else None
    if len(levels) != 1 or levels[0].get('level') != 'asInvoker' or ui_access != 'false':
        raise SystemExit(f'Missing or unexpected elevation manifest in {exe}')
    supported_os = [e.get('Id') for e in ElementTree.fromstring(text).iter() if e.tag.endswith('}supportedOS')]
    flags = int(pe[exe]['dll_characteristics'], 16)
    if flags & 0x160 != 0x160:
        raise SystemExit(f'ASLR/NX/high-entropy VA missing in {exe}')
    mitigations[exe] = dict(requested_execution_level='asInvoker', ui_access=False,
                            ui_access_attribute='uiAccess' in levels[0].attrib, supported_os=supported_os,
                            aslr=True, nx=True, high_entropy_va=True, dll_characteristics=hex(flags))
# The bridge runs with only Windows folders on PATH: the payload is self-contained.
bare = {k: v for k, v in os.environ.items() if k.upper() not in ('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH')}
bare['PATH'] = os.pathsep.join([str(system), os.environ['SystemRoot'], str(system / 'Wbem')])
bridge_version = run([str(payload / 'nova-mcp.exe'), '--version'], env=bare).stdout.strip()
self_test = json.loads(run([str(payload / 'nova-mcp.exe'), '--self-test'], env=bare).stdout)
if f' {version} ' not in f' {bridge_version} ' or self_test['tools'] != 39 or self_test['version'] != version:
    raise SystemExit(f'Bridge identity mismatch: {bridge_version} / {self_test}')
bridge_license = run([str(payload / 'nova-mcp.exe'), '--license'], env=bare).stdout
if 'GPL-3.0-or-later' not in bridge_license or 'NO WARRANTY' not in bridge_license:
    raise SystemExit(f'Bridge license notice missing: {bridge_license}')
gxx_version = run([str(compiler), '--version'], env=tool_env).stdout.splitlines()[0]

# ---- 5. manifest inside the payload, then the dependency policy on the final payload
files = sorted(f for f in payload.rglob('*') if f.is_file())
manifest = dict(format='nova-release-payload', version=version, source_revision=revision,
                clean_source=clean, source_snapshot_verified=snapshot_verified, source_tree=source_tree,
                architecture='windows-x64', configuration='Release', qt='6.12.0',
                compiler=gxx_version, toolchain_family='gcc-mingw-w64-ucrt64', experimental=True,
                packaging_recipe_sha256=sha256(__file__),
                signing='unsigned; no credentials used', public_distribution_authorized=False,
                license=dict(nova='GPL-3.0-or-later', text='LICENSE.txt', gpl_text_sha256=GPL3_SHA256,
                             copyright='licenses/COPYRIGHT.md', third_party='licenses/THIRD_PARTY_NOTICES.md'),
                corresponding_source=source_info,
                toolchain=dict(found=toolchain.get('found'), qt_build=dict((k, qt_build.get(k)) for k in
                               ('qt_version', 'source', 'source_sha256', 'patches', 'cmake_options', 'compiler'))),
                bridge=dict(version=bridge_version, self_test=self_test),
                executable_manifests=mitigations, portable_executables=pe,
                files={f.relative_to(payload).as_posix(): dict(bytes=f.stat().st_size, sha256=sha256(f)) for f in files})
(payload / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
run([sys.executable, '-I', str(root / 'scripts/check-mingw-payload.py'), '--payload', str(payload), '--qt', str(qt_root),
     '--toolchain-bin', str(toolchain_bin), '--report', str(out / 'payload-check.json')])

# ---- 6. packages from the validated payload
makensis = pathlib.Path(re.search(r'(?m)^NOVA_MAKENSIS:[A-Z]+=(.*)', cache)[1].strip())
if not makensis.is_file():
    raise SystemExit(f'makensis not found: {makensis}')
artifacts = {}
# makensis is a 32-bit program without long-path support: stage CPack in a short folder.
staging_root = root / 'build' / 'cpk-4i'
staging_root.mkdir(exist_ok=True)
for generator, suffix in (('NSIS', '.exe'), ('ZIP', '.zip')):
    staging = staging_root / f'{(source_tree or revision)[:8]}-{generator.lower()}'
    if staging.exists():
        raise SystemExit(f'Staging folder already exists: {staging}')
    run([str(cpack), '-G', generator, '--config', str(build / 'CPackConfig.cmake'),
         '-D', f'NOVA_PAYLOAD={payload}', '-B', str(staging)], cwd=str(build), env=tool_env)
    produced = sorted(staging.glob(f'*{suffix}'))
    if len(produced) != 1:
        raise SystemExit(f'Expected one {generator} package, found {produced}')
    final = out / produced[0].name
    shutil.move(str(produced[0]), final)
    artifacts[generator] = final
longest = max((f.relative_to(payload).as_posix() for f in payload.rglob('*') if f.is_file()), key=len)
payload_hashes = {f.relative_to(payload).as_posix(): sha256(f) for f in payload.rglob('*') if f.is_file()}
with zipfile.ZipFile(artifacts['ZIP']) as z:
    zipped = {}
    for info in z.infolist():
        if info.is_dir():
            continue
        zipped[info.filename.split('/', 1)[1]] = hashlib.sha256(z.read(info)).hexdigest()
if zipped != payload_hashes:
    raise SystemExit('The ZIP does not match the validated payload')
installer = artifacts['NSIS'].read_bytes()
installer_machine = struct.unpack_from('<H', installer, struct.unpack_from('<I', installer, 0x3c)[0] + 4)[0]

# ---- 7. checksums and release manifest
runtime_source = json.loads(a.gcc_runtime_source.read_text(encoding='utf-8'))
runtime_name = runtime_source.get('file')
if runtime_name != 'mingw-w64-gcc-16.2.0-6.src.tar.zst':
    raise SystemExit('Unexpected GCC runtime source package identity')
runtime_archive = a.gcc_runtime_source.parent / runtime_name
if sha256(runtime_archive) != runtime_source.get('sha256'):
    raise SystemExit('GCC runtime corresponding-source checksum mismatch')
shutil.copy2(runtime_archive, out / runtime_name)
shutil.copy2(a.gcc_runtime_source, out / 'gcc-runtime-source.json')
checksums = {path.name: sha256(path) for path in [*artifacts.values(), source_archive, out / runtime_name,
                                                out / 'gcc-runtime-source.json']}
if checksums[source_name] != sha256(payload / 'source' / source_name):
    raise SystemExit('The packaged source archive differs from the one beside it')
(out / 'SHA256SUMS.txt').write_text(''.join(f'{h}  {n}\n' for n, h in checksums.items()), encoding='utf-8')
cmake_version = run([str(cmake), '--version'], env=tool_env).stdout.splitlines()[0]
nsis_version = run([str(makensis), '-VERSION']).stdout.strip()
release = dict(format='nova-release-candidate', product='Nova Studio', version=version, source_revision=revision,
               clean_source=clean, source_snapshot_verified=snapshot_verified, source_tree=source_tree,
               preview=True, experimental='GCC/MinGW-w64 UCRT64 qualification build (Phase 4-I.2)',
               platform='Windows 11 x64',
               artifacts={g: dict(file=p.name, bytes=p.stat().st_size, sha256=checksums[p.name]) for g, p in artifacts.items()},
               installer=dict(technology=f'NSIS {nsis_version} via CPack ({cmake_version})', stub_machine=hex(installer_machine),
                              compression='zlib (solid)', scope='per-user, no elevation',
                              default_folder='%LOCALAPPDATA%\\Programs\\Nova Studio'),
               longest_relative_path=dict(path=longest, characters=len(longest),
                                          max_install_folder_characters=259 - 1 - len(longest)),
               payload=dict(files=len(payload_hashes), bytes=sum(f.stat().st_size for f in payload.rglob('*') if f.is_file()),
                            manifest_sha256=sha256(payload / 'package-manifest.json')),
               license=manifest['license'],
               corresponding_source=dict(source_info, file=source_name, also_in_packages=f'source/{source_name}'),
               gcc_runtime_corresponding_source=runtime_source,
               bridge=manifest['bridge'], signing='unsigned', published=False, public_distribution_authorized=False)
(out / 'release-manifest.json').write_text(json.dumps(release, indent=2) + '\n', encoding='utf-8')
print(json.dumps(release, indent=2))
