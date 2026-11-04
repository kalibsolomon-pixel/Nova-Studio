"""Create an unsigned, local-only portable Windows candidate. Never overwrites outputs."""
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
p.add_argument('--build', default='studio-phase3-distribution')
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
out = root / 'build/phase3-release-candidates' / label
out.mkdir(parents=True, exist_ok=False)
package = out / 'Nova Studio'
package.mkdir()
qt = root / '.tools/qt'
shutil.copy2(build / 'src/nova-studio.exe', package)
cmd = [str(qt / 'bin/windeployqt.exe'), '--release', '--no-plugins', '--no-translations',
       '--no-compiler-runtime', '--no-opengl-sw', str(package / 'nova-studio.exe')]
environment = {**os.environ, 'PATH': str(qt / 'bin') + os.pathsep + os.environ['PATH']}
result = subprocess.run(cmd, env=environment, capture_output=True, timeout=180)
(out / 'windeployqt.log').write_bytes(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit('windeployqt failed; output retained')
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
(licenses / 'THIRD_PARTY_NOTICES.md').write_text(
    '# Third-party components\n\n'
    'Qt Base 6.12.0 is dynamically linked. Qt libraries are replaceable DLLs; no code-signature '
    'restriction on replacement is added by Nova. The exact Qt Base source archive, license '
    'texts, third-party attribution files and vendor SPDX inventory accompany this candidate. '
    'Qt modules: Core, Gui, Widgets, Concurrent, Network, OpenGL, OpenGLWidgets and the Windows '
    'platform plugin. Qt Base embeds its configured PCRE2, zlib, libpng, FreeType, HarfBuzz, image and '
    'other support code; consult the vendor inventory and qt/ notices for exact component '
    'licenses. The full SDK inventory also describes tools not shipped here.\n\n'
    'MSVC runtime DLLs are copied only from the installed Visual Studio redistributable '
    'directory, not System32 or a Debug toolchain. See MICROSOFT-REDIST.txt and the current '
    'Microsoft license terms. Windows system libraries and the OpenGL driver are OS/hardware '
    'prerequisites, not bundled. No Minecraft game assets, custom fonts, icon libraries, '
    'AI artwork, Python, Rust runtime or historical prototype is included.\n\n'
    'Nova Studio licensing and public distribution authorization are pending. This local '
    'candidate is for owner acceptance only. Third-party notices do not grant a product license.\n', encoding='utf-8')
guide = package / 'READ_ME_FIRST.md'
guide.write_text(
    f'# Nova Studio {version} — local candidate\n\n'
    'Unsigned Windows x64 portable application. Requires Windows 11 and an OpenGL 2.1 '
    'compatible graphics driver. Extract the entire archive to a user-writable application '
    'folder and run nova-studio.exe. No administrator rights, service, registry file '
    'association, telemetry or automatic updater is installed. Do not run from inside ZIP.\n\n'
    'Create/open a pack from File; choose its Minecraft Java profile explicitly. F1 opens '
    'the local guide. User settings remain under the existing Qt AppConfigLocation '
    '(normally %APPDATA%/Nova Studio/Nova Studio/studio.ini); local diagnostics use '
    'Qt AppLocalDataLocation. Project editable sources remain with the project. '
    'Copying or removing the application folder must not remove those files.\n\n'
    'For upgrade, close Nova and extract a new version beside the old one. Keep the old '
    'folder until acceptance; switch back for rollback. For removal, close Nova and '
    'remove only its application folder, preserving projects/settings/recovery. No '
    'installer repair or registry uninstall operation exists.\n\n'
    f'Source revision: {revision}. See package-manifest.json and licenses/. '
    'Not authorized for public distribution; see known limitations and license decision.\n', encoding='utf-8')
for name in ('LIMITATIONS.md', 'MINECRAFT_COMPATIBILITY.md', 'UPDATE_STRATEGY.md',
             'FULL_ACCEPTANCE_MATRIX.md', 'DISTRIBUTION.md', 'DEPENDENCIES_AND_LICENSES.md',
             'BUILDING.md', 'ACCESSIBILITY.md', 'PERFORMANCE.md', 'MEMORY_CPU_ANALYSIS.md'):
    file = root / 'docs/phase-3' / name
    if not file.is_file():
        raise SystemExit(f'Required candidate documentation missing: {name}')
    shutil.copy2(file, package / name)
acceptance = package/'acceptance'
acceptance.mkdir()
for configuration in ('release', 'debug'):
    evidence = root/'docs/phase-3'/(configuration+'-accepted')
    for name in ('summary.json', 'ctest.txt', 'build-identity.h.txt'):
        if not (evidence/name).is_file():
            raise SystemExit(f'Final {configuration} acceptance evidence missing: {name}')
        shutil.copy2(evidence/name, acceptance/(configuration+'-'+name))
    test_summary = json.loads((evidence/'summary.json').read_text(encoding='utf-8'))
    if test_summary['exit_code'] or test_summary['totals_including_init_cleanup']['failed'] or test_summary['totals_including_init_cleanup']['skipped']:
        raise SystemExit(f'Final {configuration} acceptance is not fully passing')
for name in ('build-environment.json', 'official-artifact-recheck.json', 'focus-contrast.json'):
    shutil.copy2(root/'docs/phase-3'/name, acceptance/name)
shutil.copy2(root/'docs/phase-3/dpi/summary.json', acceptance/'dpi-summary.json')
compiler = re.search(r'CMAKE_CXX_COMPILER:FILEPATH=(.*)', cache)[1].strip()
dumpbin = pathlib.Path(compiler).with_name('dumpbin.exe')
mt = re.search(r'CMAKE_MT:FILEPATH=(.*)', cache)[1].strip()
executable_manifest = out/'windows-executable.manifest.xml'
subprocess.run([mt, '-nologo', '-inputresource:'+str(package/'nova-studio.exe')+';#1',
                '-out:'+str(executable_manifest)], check=True, capture_output=True)
manifest_text = executable_manifest.read_text(encoding='utf-8-sig')
if 'asInvoker' not in manifest_text or 'uiAccess="false"' not in manifest_text:
    raise SystemExit('Unexpected elevation/access manifest')
pe_bytes = (package/'nova-studio.exe').read_bytes()
pe_offset = struct.unpack_from('<I', pe_bytes, 0x3c)[0]
pe_flags = struct.unpack_from('<H', pe_bytes, pe_offset+24+70)[0]
if pe_flags & 0x160 != 0x160:
    raise SystemExit('Required x64 ASLR/NX/high-entropy mitigation flags are missing')
system = pathlib.Path(os.environ['SystemRoot']) / 'System32'
dependencies = {}
for file in sorted(package.rglob('*')):
    if file.suffix.lower() not in ('.dll', '.exe'): continue
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
    executable_manifest=dict(requested_execution_level='asInvoker', ui_access=False,
                             dll_characteristics=hex(pe_flags), aslr=True, nx=True, high_entropy_va=True),
    files=files, dependencies=dependencies)
(package / 'package-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
zip_path = out / (label + '.zip')
with zipfile.ZipFile(zip_path, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zip_file:
    for file in sorted(package.rglob('*')):
        if file.is_file(): zip_file.write(file, file.relative_to(out))
checksums = {zip_path.name: hashlib.file_digest(zip_path.open('rb'), 'sha256').hexdigest(),
             'Nova Studio/nova-studio.exe': hashlib.file_digest((package/'nova-studio.exe').open('rb'), 'sha256').hexdigest(),
             'Nova Studio/package-manifest.json': hashlib.file_digest((package/'package-manifest.json').open('rb'), 'sha256').hexdigest()}
(out / 'SHA256SUMS.txt').write_text(''.join(f'{h}  {name}\n' for name, h in checksums.items()), encoding='utf-8')
(out / 'candidate.json').write_text(json.dumps(dict(directory=str(out), source_revision=revision,
    version=version, files=len(files)+1, bytes=sum(v['bytes'] for v in files.values()),
    zip_bytes=zip_path.stat().st_size, checksums=checksums), indent=2)+'\n', encoding='utf-8')
print((out / 'candidate.json').read_text(encoding='utf-8'))
