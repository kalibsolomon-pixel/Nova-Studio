"""Phase 4-F: unsigned, local-only portable Windows candidate with the MCP bridge.

Follows the accepted Phase 3 portable recipe (scripts/package-phase3.py) and adds
nova-mcp.exe, which Phase 4 introduced. Refuses modified or uncommitted product source,
never overwrites outputs, and never publishes. Produces:
  build/phase4f-distribution/<label>/<label>.zip
  build/phase4f-distribution/<label>/SHA256SUMS.txt
  build/phase4f-distribution/<label>/candidate.json
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
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', default='studio-phase4a-Release')
p.add_argument('--label')
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
if not re.fullmatch(r'[a-f0-9]{40}', revision):
    raise SystemExit('Refusing a modified/unknown source build; commit and rebuild first')
for path in ('src', 'CMakeLists.txt'):
    expected = subprocess.check_output(['git', 'rev-parse', f'{revision}:{path}'], cwd=root)
    actual = subprocess.check_output(['git', 'rev-parse', f'HEAD:{path}'], cwd=root)
    if expected != actual:
        raise SystemExit('Packaged build does not match the current committed product source')
if subprocess.call(['git', 'diff', '--quiet', 'HEAD', '--', 'src', 'CMakeLists.txt'], cwd=root):
    raise SystemExit('Commit product source changes and rebuild before packaging')
label = a.label or f'nova-studio-{version}-windows-x64-{revision[:8]}'
if not re.fullmatch(r'[A-Za-z0-9_.-]+', label):
    raise SystemExit('Label must be a simple directory name')
out = root / 'build/phase4f-distribution' / label
out.mkdir(parents=True, exist_ok=False)
package = out / 'Nova Studio'
package.mkdir()
qt = root / '.tools/qt'
executables = ('nova-studio.exe', 'nova-mcp.exe')
for exe in executables:
    shutil.copy2(build / 'src' / exe, package)
    cmd = [str(qt / 'bin/windeployqt.exe'), '--release', '--no-plugins', '--no-translations',
           '--no-compiler-runtime', '--no-opengl-sw', str(package / exe)]
    result = subprocess.run(cmd, capture_output=True)
    (out / f'windeployqt-{exe}.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'windeployqt failed for {exe}; output retained')
platform = package / 'plugins/platforms'
platform.mkdir(parents=True)
shutil.copy2(qt / 'plugins/platforms/qwindows.dll', platform)
(package / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=plugins\nLibraries=.\n', encoding='utf-8')
vswhere = pathlib.Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = pathlib.Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property',
                                         'installationPath'], text=True).strip())
runtime_version = (vs / 'VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt').read_text(encoding='utf-8').strip()
runtime = vs / 'VC/Redist/MSVC' / runtime_version / 'x64/Microsoft.VC145.CRT'
if not runtime.is_dir():
    raise SystemExit(f'Approved app-local MSVC runtime directory unavailable: {runtime}')
for file in runtime.glob('*.dll'):
    shutil.copy2(file, package)
licenses = package / 'licenses'
licenses.mkdir()
shutil.copy2(root / 'LICENSE-DECISION.md', licenses)
shutil.copy2(vs / 'Licenses/1033/Redist.txt', licenses / 'MICROSOFT-REDIST.txt')
source = root / 'build/phase3-dependencies'
metadata = json.loads((source / 'qt-source.json').read_text(encoding='utf-8'))
archive = source / 'qtbase-everywhere-src-6.12.0.tar.xz'
if hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest() != metadata['sha256']:
    raise SystemExit('Qt source archive integrity failure')
shutil.copy2(archive, licenses)
shutil.copy2(source / 'qt-source.json', licenses)
shutil.copytree(source / 'qt-notices', licenses / 'qt')
shutil.copy2(qt / 'sbom/qtbase-6.12.0.spdx.json', licenses)
# The Gradle wrapper is embedded in nova-studio.exe as a Qt resource; ship its own license.
wrapper = root / 'assets/fabric-runtime/gradle-wrapper-9.8.2.jar'
with zipfile.ZipFile(wrapper) as jar:
    (licenses / 'gradle-wrapper-9.8.2-LICENSE.txt').write_bytes(jar.read('META-INF/LICENSE'))
(licenses / 'THIRD_PARTY_NOTICES.md').write_text(
    '# Third-party components\n\n'
    'Qt Base 6.12.0 is dynamically linked by nova-studio.exe and nova-mcp.exe. Qt libraries are '
    'replaceable DLLs; no code-signature restriction on replacement is added by Nova. The exact '
    'Qt Base source archive, license notices and SPDX bill of materials are included here.\n\n'
    'MSVC runtime DLLs are copied only from the installed Visual Studio redistributable '
    'directory and are redistributed under MICROSOFT-REDIST.txt. Windows system libraries and '
    'the OpenGL driver are provided by the operating system and hardware vendor and are not '
    'redistributed.\n\nnova-studio.exe embeds gradle-wrapper-9.8.2.jar (Gradle, Apache License 2.0; '
    'SHA-256 3b8a25775a69158b5ad2b1d17a80a88dc7b40a352a73eb27d06c612a7ce68e98) as a Qt resource and '
    'writes it into the managed test workspace. Its license is gradle-wrapper-9.8.2-LICENSE.txt.\n\n'
    'The official MCP SDK is a development and test dependency only; it is not '
    'shipped. Managed Minecraft development runtimes are downloaded by the user on demand under '
    'their own terms and are not shipped.\n\nNova Studio itself has no license grant: see '
    'LICENSE-DECISION.md. This candidate is for owner acceptance only.\n', encoding='utf-8')
(package / 'READ_ME_FIRST.md').write_text(
    f'# Nova Studio {version} — portable candidate\n\n'
    'Extract the folder anywhere you can write to and run nova-studio.exe. Requires Windows 11 '
    'and an OpenGL 2.1 compatible graphics driver. No administrator rights, service, registry '
    'file association, telemetry or automatic updater is involved. nova-mcp.exe is the local '
    'MCP bridge for AI clients; it connects only to a Nova Studio window of the same user that '
    'has issued a connection code from Resources > MCP Access.\n\n'
    f'Source revision: {revision}. See package-manifest.json and licenses/. '
    'Not authorized for public distribution; see the license decision.\n', encoding='utf-8')
compiler = re.search(r'CMAKE_CXX_COMPILER:FILEPATH=(.*)', cache)[1].strip()
dumpbin = pathlib.Path(compiler).with_name('dumpbin.exe')
mt = re.search(r'CMAKE_MT:FILEPATH=(.*)', cache)[1].strip()
mitigations = {}
for exe in executables:
    manifest_file = out / f'{exe}.manifest.xml'
    subprocess.run([mt, '-nologo', '-inputresource:' + str(package / exe) + ';#1', '-out:' + str(manifest_file)],
                   check=True, capture_output=True)
    manifest_text = manifest_file.read_text(encoding='utf-8-sig')
    if 'asInvoker' not in manifest_text or 'uiAccess="false"' not in manifest_text:
        raise SystemExit(f'Unexpected elevation/access manifest in {exe}')
    pe = (package / exe).read_bytes()
    offset = struct.unpack_from('<I', pe, 0x3c)[0]
    flags = struct.unpack_from('<H', pe, offset + 24 + 70)[0]
    if flags & 0x160 != 0x160:
        raise SystemExit(f'Required x64 ASLR/NX/high-entropy mitigation flags are missing in {exe}')
    mitigations[exe] = dict(requested_execution_level='asInvoker', ui_access=False,
                            dll_characteristics=hex(flags), aslr=True, nx=True, high_entropy_va=True)
system = pathlib.Path(os.environ['SystemRoot']) / 'System32'
dependencies = {}
for file in sorted(package.rglob('*')):
    if file.suffix.lower() not in ('.dll', '.exe'):
        continue
    output = subprocess.check_output([str(dumpbin), '/dependents', str(file)], text=True)
    imports = sorted(set(re.findall(r'^\s+([\w.-]+\.dll)\s*$', output, re.M | re.I)))
    for imported in imports:
        if not ((package / imported).is_file() or (system / imported).is_file() or
                imported.lower().startswith(('api-ms-win-', 'ext-ms-win-'))):
            raise SystemExit(f'Unresolved dependency: {file.name}: {imported}')
    dependencies[file.relative_to(package).as_posix()] = imports
files = {file.relative_to(package).as_posix(): dict(bytes=file.stat().st_size,
         sha256=hashlib.file_digest(file.open('rb'), 'sha256').hexdigest())
         for file in sorted(package.rglob('*')) if file.is_file()}
manifest = dict(format='nova-local-release-candidate', version=version, source_revision=revision,
                packaging_recipe_sha256=hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
                architecture='windows-x64', configuration='Release', qt='6.12.0',
                msvc_runtime=runtime_version, compiler=pathlib.Path(compiler).name,
                signing='unsigned; no credentials used', public_distribution_authorized=False,
                executable_manifests=mitigations, files=files, dependencies=dependencies)
(package / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
zip_path = out / (label + '.zip')
with zipfile.ZipFile(zip_path, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zip_file:
    for file in sorted(package.rglob('*')):
        if file.is_file():
            zip_file.write(file, file.relative_to(out))
checksums = {zip_path.name: hashlib.file_digest(zip_path.open('rb'), 'sha256').hexdigest()}
for name in executables + ('package-manifest.json',):
    checksums['Nova Studio/' + name] = hashlib.file_digest((package / name).open('rb'), 'sha256').hexdigest()
(out / 'SHA256SUMS.txt').write_text(''.join(f'{h}  {name}\n' for name, h in checksums.items()), encoding='utf-8')
(out / 'candidate.json').write_text(json.dumps(dict(directory=str(out), source_revision=revision, version=version,
    files=len(files) + 1, bytes=sum(v['bytes'] for v in files.values()), zip_bytes=zip_path.stat().st_size,
    checksums=checksums), indent=2) + '\n', encoding='utf-8')
print((out / 'candidate.json').read_text(encoding='utf-8'))
