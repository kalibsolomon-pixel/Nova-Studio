"""Phase 3-E: produce the Windows installer candidate.

Reuses the accepted Phase 3 portable recipe (scripts/package-phase3.py) as the
payload source of truth, adjusts the payload read-me for an installed
application, re-signs the package manifest for that adjustment, and compiles a
per-user Inno Setup installer around the payload (scripts/installer/nova-studio.iss).
Never overwrites outputs. Produces:
  build/phase3-release-candidates/<label>/NovaStudio-<label>-setup.exe
  build/phase3-release-candidates/<label>/installer-SHA256SUMS.txt
  build/phase3-release-candidates/<label>/installer.json
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', default='studio-phase3e-release')
p.add_argument('--label')
p.add_argument('--iscc', default=str(root / '.tools/innosetup/ISCC.exe'))
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
label = a.label or f'nova-studio-{version}-windows-x64-installer-{revision[:8]}'
if not re.fullmatch(r'[A-Za-z0-9_.-]+', label):
    raise SystemExit('Label must be a simple directory name')
out = root / 'build/phase3-release-candidates' / label
if out.exists():
    raise SystemExit(f'Refusing to overwrite existing candidate: {out}')
iscc = pathlib.Path(a.iscc)
if not iscc.is_file():
    raise SystemExit('Inno Setup compiler not found; see docs/phase-3e/PHASE_3E_PACKAGING.md')

# 1. Portable payload from the accepted recipe (validates committed source and
#    bundles Qt runtime, licenses, notices, docs and acceptance evidence).
subprocess.run(['python', str(root / 'scripts/package-phase3.py'), '--build', a.build,
                '--label', label], cwd=root, check=True)
package = out / 'Nova Studio'

# 2. The payload read-me must describe an installed application, not a portable
#    extraction; keep the manifest truthful about the adjusted file.
readme = package / 'READ_ME_FIRST.md'
readme.write_text(
    f'# Nova Studio {version} — installed application\n\n'
    'Installed per user under %LOCALAPPDATA%\\Programs\\Nova Studio. Requires Windows 11 and an '
    'OpenGL 2.1 compatible graphics driver. Start it from the Start Menu entry ("Nova Studio"); '
    'uninstall it through Windows Settings > Apps or the "Uninstall Nova Studio" Start Menu entry. '
    'No administrator rights, service, telemetry, file association or automatic updater is '
    'involved.\n\n'
    'Create or open a pack from File; choose its Minecraft Java profile explicitly. F1 opens the '
    'local guide. Personal settings remain under the Qt AppConfigLocation (normally '
    '%APPDATA%/Nova Studio/Nova Studio/studio.ini); diagnostics and recovery snapshots use the Qt '
    'AppLocalDataLocation and the settings folder. The Fabric test runtime lives under '
    '%LOCALAPPDATA%\\NovaStudio\\fabric-runtime and can be moved in Advanced settings.\n\n'
    'Uninstalling removes installed application files only. Resource-pack projects, settings, '
    'recovery snapshots and diagnostics are preserved. The Fabric runtime cache (including the '
    'Nova Test World) is offered as a separate uninstall-time choice that defaults to keeping it.\n\n'
    f'Source revision: {revision}. See package-manifest.json and licenses/. Not authorized for '
    'public distribution; see known limitations and the license decision.\n', encoding='utf-8')
manifest_path = package / 'package-manifest.json'
manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
entry = manifest['files'].get('READ_ME_FIRST.md')
if entry is None:
    raise SystemExit('Payload manifest lacks READ_ME_FIRST.md; recipe output changed')
entry['bytes'] = readme.stat().st_size
entry['sha256'] = hashlib.file_digest(readme.open('rb'), 'sha256').hexdigest()
manifest['phase3e'] = dict(installer='Inno Setup per-user install; scripts/installer/nova-studio.iss',
                           read_me_adjusted=True)
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

# 3. Compile the installer from the reviewed payload.
number = re.match(r'(\d+\.\d+\.\d+)', version)[1]
setup_base = f'{label}'
result = subprocess.run([str(iscc),
                         f'/DPayload={package}', f'/DOutputDir={out}', f'/DSetupBaseName={setup_base}',
                         f'/DAppVersionNumber={number}', f'/DAppDisplayVersion={version}',
                         f'/DRevision={revision}',
                         f'/DSetupIcon={root / "assets/branding/nova-studio-icon.ico"}',
                         str(root / 'scripts/installer/nova-studio.iss')],
                        cwd=root, capture_output=True, timeout=1800)
(out / 'iscc.log').write_bytes(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit('ISCC failed; output retained in iscc.log')
setup = out / f'{setup_base}.exe'
if not setup.is_file():
    raise SystemExit('ISCC reported success but the installer is missing')

# 4. Verification metadata: checksums and build inputs.
compiler = re.search(r'CMAKE_CXX_COMPILER:FILEPATH=(.*)', cache)[1].strip()
checksums = {setup.name: hashlib.file_digest(setup.open('rb'), 'sha256').hexdigest(),
             'Nova Studio/nova-studio.exe':
                 hashlib.file_digest((package / 'nova-studio.exe').open('rb'), 'sha256').hexdigest(),
             'Nova Studio/package-manifest.json':
                 hashlib.file_digest(manifest_path.open('rb'), 'sha256').hexdigest()}
(out / 'installer-SHA256SUMS.txt').write_text(
    ''.join(f'{h}  {name}\n' for name, h in checksums.items()), encoding='utf-8')
inno_version = subprocess.run([str(iscc), '/?'], capture_output=True, text=True)
inno_text = (inno_version.stdout + inno_version.stderr)[:2000]
inno_match = re.search(r'Inno Setup (\d[\w.]*)', inno_text)
(out / 'installer.json').write_text(json.dumps(dict(
    setup=dict(path=str(setup), bytes=setup.stat().st_size, sha256=checksums[setup.name]),
    payload_dir=str(package), payload_files=len(manifest['files']),
    application_version=version, numeric_version=number, source_revision=revision,
    configuration='Release', architecture='windows-x64', compiler=pathlib.Path(compiler).name,
    inno_setup=dict(path=str(iscc), version=inno_match.group(1) if inno_match else 'unknown',
                    script='scripts/installer/nova-studio.iss',
                    signing='unsigned; no credentials used'),
    public_distribution_authorized=False), indent=2) + '\n', encoding='utf-8')
print((out / 'installer.json').read_text(encoding='utf-8'))
