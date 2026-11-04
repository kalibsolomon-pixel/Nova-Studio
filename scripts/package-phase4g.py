# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4-G: build the 0.1.0-preview.1 release candidate (NSIS installer + portable ZIP).

  1. Refuses Debug builds and any build whose product or packaging sources differ from HEAD.
  2. `cmake --install` creates the payload (cmake/NovaPackaging.cmake).
  3. Validates the payload: expected files only, x64 PE images, asInvoker manifests, ASLR/NX/
     high-entropy VA, every import resolved inside the payload or Windows, no debug runtime,
     the Qt source archive checksum, and nova-mcp.exe's version and self-test with only Windows
     folders on PATH.
  4. Writes package-manifest.json into the payload.
  5. Runs CPack (NSIS, ZIP) on that exact payload (packaging/CPackProjectConfig.cmake).
  6. Writes SHA256SUMS.txt and release-manifest.json. Never overwrites, signs or publishes.
Phase 4-G.1: before validation it also writes the GPL corresponding source of the build revision
(`git archive`, checked blob by blob against Git) into the output folder and the payload's source/,
and it checks the unmodified GPL text and the bridge's license notice.
Output: build/phase4g-distribution/<label>/
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

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', default='studio-phase4g-Release')
p.add_argument('--label')
p.add_argument('--allow-modified', action='store_true', help='development rehearsal only; marked in the manifest')
a = p.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_-]+', a.build):
    raise SystemExit('Build name must be a simple directory name')
build = root / 'build' / a.build
cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache:
    raise SystemExit('Only a Release build can be packaged')
identity = (build / 'generated/build_identity.h').read_text(encoding='utf-8')
revision = re.search(r'BuildRevision\[\] = "([^"]+)"', identity)[1]
version = re.search(r'AppVersion\[\] = "([^"]+)"', identity)[1]
packaging_paths = ['src', 'CMakeLists.txt', 'cmake', 'packaging', 'docs/release', 'LICENSE', 'COPYRIGHT.md',
                   'assets/branding', 'assets/fabric-runtime']
# Phase 4-G.1: the GPL corresponding source of the binaries. Everything needed to configure, build,
# test, install and package them, at the build's exact revision. Historical acceptance evidence,
# development logs and the Phase 1 prototypes are not part of the binaries and are left out.
SOURCE_PATHS = ['.clang-format', '.gitattributes', '.gitignore', 'CMakeLists.txt', 'LICENSE', 'COPYRIGHT.md',
                'LICENSE-DECISION.md', 'README.md', 'src', 'tests', 'cmake', 'packaging', 'assets/branding',
                'assets/fabric-runtime', 'fixtures', 'docs/release',
                'scripts/build-studio.ps1', 'scripts/run-ctest.ps1', 'scripts/install-qt.py',
                'scripts/prepare-phase3-qt.py', 'scripts/package-phase4g.py', 'scripts/verify-phase4g1-licensing.py',
                'scripts/generate-brand-icons.mjs', 'scripts/requirements.txt', 'scripts/requirements-lock.txt']
clean = bool(re.fullmatch(r'[a-f0-9]{40}', revision))
if clean:
    for path in packaging_paths:
        expected = subprocess.run(['git', 'rev-parse', f'{revision}:{path}'], cwd=root, capture_output=True, text=True).stdout
        actual = subprocess.run(['git', 'rev-parse', f'HEAD:{path}'], cwd=root, capture_output=True, text=True).stdout
        if expected != actual:
            clean = False
    if subprocess.call(['git', 'diff', '--quiet', 'HEAD', '--', *packaging_paths], cwd=root):
        clean = False
if not clean and not a.allow_modified:
    raise SystemExit('Commit product and packaging sources, rebuild, then package (or use --allow-modified for a rehearsal)')
label = a.label or f'nova-studio-{version}-windows-x64-{revision[:8]}'
if not re.fullmatch(r'[A-Za-z0-9_.+-]+', label):
    raise SystemExit('Label must be a simple directory name')
out = root / 'build/phase4g-distribution' / label
out.mkdir(parents=True, exist_ok=False)
payload = out / 'payload' / 'Nova Studio'
cmake = pathlib.Path(re.search(r'CMAKE_COMMAND:INTERNAL=(.*)', cache)[1].strip())
cpack = cmake.with_name('cpack.exe')
log = open(out / 'packaging.log', 'w', encoding='utf-8')


def run(cmd, **kw):
    log.write('> ' + ' '.join(map(str, cmd)) + '\n')
    log.flush()
    result = subprocess.run(cmd, capture_output=True, text=True, **kw)
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
run([str(cmake), '--install', str(build), '--prefix', str(payload)])

# ---- 2b. corresponding source (Phase 4-G.1): `git archive` of the exact build revision, with the
# stored bytes (no line-ending conversion), checked blob by blob against the Git tree.
source_commit = revision if clean else run(['git', 'rev-parse', 'HEAD'], cwd=root).stdout.strip()
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
source_info = dict(file=f'source/{source_name}', revision=source_commit, matches_binaries=clean,
                   files=len(expected_blobs), paths=SOURCE_PATHS, sha256=sha256(source_archive),
                   bytes=source_archive.stat().st_size)

# ---- 3. validation
files = sorted(f for f in payload.rglob('*') if f.is_file())
names = {f.relative_to(payload).as_posix() for f in files}
required = {'nova-studio.exe', 'nova-mcp.exe', 'qt.conf', 'README.txt', 'plugins/platforms/qwindows.dll',
            'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6OpenGL.dll', 'Qt6OpenGLWidgets.dll', 'Qt6Network.dll',
            'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'LICENSE.txt', 'licenses/COPYRIGHT.md',
            'licenses/qt-build/config_qtbase.opt', f'source/{source_name}', 'docs/BUILDING.md',
            'licenses/THIRD_PARTY_NOTICES.md', 'licenses/MICROSOFT-REDIST.txt', 'licenses/qt-source.json',
            'licenses/qtbase-everywhere-src-6.12.0.tar.xz', 'licenses/qtbase-6.12.0.spdx.json',
            'licenses/gradle-wrapper-9.8.2-LICENSE.txt', 'docs/RELEASE_NOTES.md', 'docs/INSTALLATION.md',
            'docs/FIRST_RUN.md', 'docs/MCP_SETUP.md', 'docs/SEARCH.md', 'docs/KNOWN_LIMITATIONS.md'}
missing = required - names
if missing:
    raise SystemExit(f'Payload is missing: {sorted(missing)}')
binaries = [f for f in files if f.suffix.lower() in ('.dll', '.exe')]
allowed_binary = re.compile(r'^(nova-studio|nova-mcp)\.exe$|^Qt6(Core|Gui|Widgets|OpenGL|OpenGLWidgets|Network)\.dll$|'
                            r'^plugins/platforms/qwindows\.dll$|^(msvcp140.*|vcruntime140.*|concrt140|vccorlib140)\.dll$', re.I)
unexpected = [f.relative_to(payload).as_posix() for f in binaries if not allowed_binary.match(f.relative_to(payload).as_posix())]
if unexpected:
    raise SystemExit(f'Unexpected binaries in the payload: {unexpected}')
if any(re.search(r'd\.dll$', f.name, re.I) and f.name.startswith('Qt6') for f in binaries) or (payload / 'msvcp140d.dll').exists():
    raise SystemExit('Debug runtime in the payload')
metadata = json.loads((payload / 'licenses/qt-source.json').read_text(encoding='utf-8'))
if sha256(payload / 'licenses/qtbase-everywhere-src-6.12.0.tar.xz') != metadata['sha256']:
    raise SystemExit('Qt source archive integrity failure')
# The GPL text must be the unmodified FSF text (https://www.gnu.org/licenses/gpl-3.0.txt).
GPL3_SHA256 = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
if sha256(payload / 'LICENSE.txt') != GPL3_SHA256:
    raise SystemExit('LICENSE.txt is not the unmodified GPL-3.0 text')
if any('LICENSE-DECISION' in n for n in names) or 'LICENSE-DECISION' in (payload / 'README.txt').read_text(encoding='utf-8'):
    raise SystemExit('The payload still carries the pre-GPL license decision')
compiler = pathlib.Path(re.search(r'CMAKE_CXX_COMPILER:FILEPATH=(.*)', cache)[1].strip())
dumpbin = compiler.with_name('dumpbin.exe')
mt = re.search(r'CMAKE_MT:FILEPATH=(.*)', cache)[1].strip()
system = pathlib.Path(os.environ['SystemRoot']) / 'System32'
pe, dependencies, mitigations = {}, {}, {}
for f in binaries:
    rel = f.relative_to(payload).as_posix()
    data = f.read_bytes()
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    machine = struct.unpack_from('<H', data, offset + 4)[0]
    flags = struct.unpack_from('<H', data, offset + 24 + 70)[0]
    if machine != 0x8664:
        raise SystemExit(f'{rel} is not x64 (machine {machine:#x})')
    pe[rel] = dict(machine='x64', dll_characteristics=hex(flags))
    output = run([str(dumpbin), '/dependents', str(f)]).stdout
    imports = sorted(set(re.findall(r'^\s+([\w.-]+\.dll)\s*$', output, re.M | re.I)))
    for imported in imports:
        if not ((payload / imported).is_file() or (system / imported).is_file() or
                imported.lower().startswith(('api-ms-win-', 'ext-ms-win-'))):
            raise SystemExit(f'Unresolved dependency: {rel}: {imported}')
    dependencies[rel] = imports
for exe in ('nova-studio.exe', 'nova-mcp.exe'):
    manifest_file = out / f'{exe}.manifest.xml'
    run([mt, '-nologo', f'-inputresource:{payload / exe};#1', f'-out:{manifest_file}'])
    text = manifest_file.read_text(encoding='utf-8-sig')
    if 'asInvoker' not in text or 'uiAccess="false"' not in text:
        raise SystemExit(f'Unexpected elevation manifest in {exe}')
    flags = int(pe[exe]['dll_characteristics'], 16)
    if flags & 0x160 != 0x160:
        raise SystemExit(f'ASLR/NX/high-entropy VA missing in {exe}')
    mitigations[exe] = dict(requested_execution_level='asInvoker', ui_access=False, aslr=True, nx=True,
                            high_entropy_va=True, dll_characteristics=hex(flags))
# The bridge runs with only Windows folders on PATH: the payload is self-contained.
bare = {k: v for k, v in os.environ.items() if k.upper() not in ('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH')}
bare['PATH'] = os.pathsep.join([str(system), os.environ['SystemRoot'], str(system / 'Wbem')])
bridge_version = run([str(payload / 'nova-mcp.exe'), '--version'], env=bare).stdout.strip()
self_test = json.loads(run([str(payload / 'nova-mcp.exe'), '--self-test'], env=bare).stdout)
if f' {version} ' not in f' {bridge_version} ' + ' ' or self_test['tools'] != 26 or self_test['version'] != version:
    raise SystemExit(f'Bridge identity mismatch: {bridge_version} / {self_test}')
bridge_license = run([str(payload / 'nova-mcp.exe'), '--license'], env=bare).stdout
if 'GPL-3.0-or-later' not in bridge_license or 'NO WARRANTY' not in bridge_license:
    raise SystemExit(f'Bridge license notice missing: {bridge_license}')

# ---- 4. manifest inside the payload
manifest = dict(format='nova-release-payload', version=version, source_revision=revision,
                clean_source=clean, architecture='windows-x64', configuration='Release', qt='6.12.0',
                compiler=compiler.name, packaging_recipe_sha256=sha256(__file__),
                signing='unsigned; no credentials used', public_distribution_authorized=False,
                license=dict(nova='GPL-3.0-or-later', text='LICENSE.txt', gpl_text_sha256=GPL3_SHA256,
                             copyright='licenses/COPYRIGHT.md', third_party='licenses/THIRD_PARTY_NOTICES.md'),
                corresponding_source=source_info,
                bridge=dict(version=bridge_version, self_test=self_test),
                executable_manifests=mitigations, portable_executables=pe, dependencies=dependencies,
                files={f.relative_to(payload).as_posix(): dict(bytes=f.stat().st_size, sha256=sha256(f)) for f in files})
(payload / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

# ---- 5. packages from the validated payload
makensis = pathlib.Path(re.search(r'NOVA_MAKENSIS:FILEPATH=(.*)', cache)[1].strip())
if not makensis.is_file():
    raise SystemExit(f'makensis not found: {makensis}')
artifacts = {}
# makensis is a 32-bit program without long-path support: stage CPack in a short folder.
staging_root = root / 'build' / 'cpk'
staging_root.mkdir(exist_ok=True)
for generator, suffix in (('NSIS', '.exe'), ('ZIP', '.zip')):
    staging = staging_root / f'{revision[:8]}-{generator.lower()}'
    if staging.exists():
        raise SystemExit(f'Staging folder already exists: {staging}')
    run([str(cpack), '-G', generator, '--config', str(build / 'CPackConfig.cmake'),
         '-D', f'NOVA_PAYLOAD={payload}', '-B', str(staging)], cwd=str(build))
    produced = sorted(staging.glob(f'*{suffix}'))
    if len(produced) != 1:
        raise SystemExit(f'Expected one {generator} package, found {produced}')
    final = out / produced[0].name
    shutil.move(str(produced[0]), final)
    artifacts[generator] = final
longest = max((f.relative_to(payload).as_posix() for f in payload.rglob('*') if f.is_file()), key=len)
# The ZIP must equal the payload byte for byte.
payload_hashes = {f.relative_to(payload).as_posix(): sha256(f) for f in payload.rglob('*') if f.is_file()}
with zipfile.ZipFile(artifacts['ZIP']) as z:
    zipped = {}
    for info in z.infolist():
        if info.is_dir():
            continue
        name = info.filename.split('/', 1)[1]
        zipped[name] = hashlib.sha256(z.read(info)).hexdigest()
if zipped != payload_hashes:
    raise SystemExit('The ZIP does not match the validated payload')
installer = artifacts['NSIS'].read_bytes()
installer_machine = struct.unpack_from('<H', installer, struct.unpack_from('<I', installer, 0x3c)[0] + 4)[0]

# ---- 6. checksums and release manifest
checksums = {path.name: sha256(path) for path in [*artifacts.values(), source_archive]}
if checksums[source_name] != sha256(payload / 'source' / source_name):
    raise SystemExit('The packaged source archive differs from the published one')
(out / 'SHA256SUMS.txt').write_text(''.join(f'{h}  {n}\n' for n, h in checksums.items()), encoding='utf-8')
cmake_version = run([str(cmake), '--version']).stdout.splitlines()[0]
nsis_version = run([str(makensis), '-VERSION']).stdout.strip()
release = dict(format='nova-release-candidate', product='Nova Studio', version=version, source_revision=revision,
               clean_source=clean, preview=True, platform='Windows 11 x64',
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
               bridge=manifest['bridge'], signing='unsigned', published=False, public_distribution_authorized=False)
(out / 'release-manifest.json').write_text(json.dumps(release, indent=2) + '\n', encoding='utf-8')
print(json.dumps(release, indent=2))
