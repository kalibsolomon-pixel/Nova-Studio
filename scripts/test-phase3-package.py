"""Verify a local portable ZIP with a clean child environment and retained lifecycle evidence."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import time
import zipfile
import psutil

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('candidate', type=pathlib.Path)
p.add_argument('output', type=pathlib.Path)
a = p.parse_args()
candidate = a.candidate.resolve()
out = (root / a.output).resolve()
out.mkdir(parents=True, exist_ok=False)
work = root/'build/phase3-package-rehearsals'/a.output.name
work.mkdir(parents=True, exist_ok=False)
summary = json.loads((candidate / 'candidate.json').read_text(encoding='utf-8'))
zip_name = next(name for name in summary['checksums'] if name.endswith('.zip'))
zip_path = candidate / zip_name
assert hashlib.file_digest(zip_path.open('rb'), 'sha256').hexdigest() == summary['checksums'][zip_name]
environment = {k: v for k, v in os.environ.items()
               if not k.upper().startswith(('QT_', 'QML', 'QTDIR'))}
environment['PATH'] = os.pathsep.join([str(pathlib.Path(os.environ['SystemRoot'])/'System32'), os.environ['SystemRoot']])
data = work / 'user data'
data.mkdir()
document = data / 'keep.nvdoc'
shutil.copy2(root / 'tests/fixtures/c2-legacy.nvdoc', document)
document_hash = hashlib.sha256(document.read_bytes()).hexdigest()
fixture = work / 'test pack'
shutil.copytree(root / 'fixtures/phase-2c5-models', fixture, ignore=shutil.ignore_patterns('.nova'))
rows = []
def extract(destination):
    destination.mkdir()
    with zipfile.ZipFile(zip_path) as archive:
        assert sum(i.file_size for i in archive.infolist()) < 1024 * 1024 * 1024
        for info in archive.infolist():
            target = (destination / info.filename).resolve()
            assert target.is_relative_to(destination.resolve())
        archive.extractall(destination)
    return destination / 'Nova Studio'

def verify(package):
    manifest = json.loads((package/'package-manifest.json').read_text(encoding='utf-8'))
    for relative, entry in manifest['files'].items():
        file = (package/relative).resolve()
        assert file.is_relative_to(package.resolve()) and file.is_file()
        assert file.stat().st_size == entry['bytes']
        assert hashlib.file_digest(file.open('rb'), 'sha256').hexdigest() == entry['sha256']
    actual = {f.relative_to(package).as_posix() for f in package.rglob('*') if f.is_file()}
    assert actual == set(manifest['files']) | {'package-manifest.json'}
    assert not any(pathlib.Path(name).suffix.lower() in ('.pdb', '.nvr', '.nvdoc', '.ini', '.jar') for name in actual)
    assert not any('Qt6Test' in name or 'Qt6Network' in name for name in actual)
    assert 'plugins/platforms/qwindows.dll' in actual
    assert manifest['executable_manifest']['requested_execution_level'] == 'asInvoker'
    assert manifest['executable_manifest']['aslr'] and manifest['executable_manifest']['nx']
    binary = (package/'nova-studio.exe').read_bytes()
    pe = struct.unpack_from('<I', binary, 0x3c)[0]
    assert binary[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', binary, pe+4)[0] == 0x8664
    return manifest

def run(package, label, fresh):
    telemetry = out/(label+'-telemetry.json')
    cmd = [str(package/'nova-studio.exe'), '--project', str(fixture), '--settings', str(data/'studio.ini'),
           '--smoke-seconds', '4', '--telemetry', str(telemetry)]
    if fresh: cmd += ['--fresh-layout']
    with (out/(label+'.log')).open('wb') as log:
        child = subprocess.Popen(cmd, cwd=out, env=environment, stdout=log, stderr=log,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        process = psutil.Process(child.pid)
        time.sleep(2)
        before = process.cpu_times()
        measured = time.perf_counter()
        time.sleep(1)
        after = process.cpu_times()
        elapsed = time.perf_counter()-measured
        memory = process.memory_info()
        code = child.wait(timeout=40)
    assert code == 0, (label, code)
    observed = json.loads(telemetry.read_text(encoding='utf-8'))
    assert observed['environment'].get('gpu') and observed['environment'].get('backend')
    assert observed['counters'].get('documents_opened', 0) >= 1
    assert hashlib.sha256(document.read_bytes()).hexdigest() == document_hash
    rows.append(dict(case=label, exit_code=code, environment=observed['environment'],
                     idle_sample_seconds=elapsed,
                     idle_cpu_one_core_percent=100*(after.user+after.system-before.user-before.system)/elapsed,
                     working_set_mib=memory.rss/1048576, private_mib=memory.private/1048576,
                     startup_samples_ms={key: value for key,value in observed['samples_ms'].items()
                                         if key in ('first_ui_ms','ready_capture_ms','index')},
                     native_document_preserved=True, application_files_unchanged=bool(verify(package))))

installed = extract(work/'fresh install Ω')
manifest = verify(installed)
run(installed, 'fresh-install-first-launch', False)
settings_hash = hashlib.sha256((data/'studio.ini').read_bytes()).hexdigest()
# Side-by-side replacement rehearsal uses the same candidate, not a certified old-version upgrade.
replacement = extract(work/'side by side replacement')
run(replacement, 'side-by-side-replacement', True)
assert hashlib.sha256((data/'studio.ini').read_bytes()).hexdigest() == settings_hash
removed = installed.parent.with_name('removed application retained for evidence')
assert installed.parent.resolve().is_relative_to(work.resolve())
assert removed.resolve().is_relative_to(work.resolve())
installed.parent.rename(removed)
assert not installed.exists() and document.exists() and (data/'studio.ini').exists()
reinstalled = extract(work/'fresh install Ω')
run(reinstalled, 'reinstall-preserving-user-data', True)
assert hashlib.sha256((data/'studio.ini').read_bytes()).hexdigest() == settings_hash
export = out/'explicit-diagnostics.json'
result = subprocess.run([str(reinstalled/'nova-studio.exe'), '--settings', str(data/'studio.ini'),
                         '--diagnostics-export', str(export)], cwd=out, env=environment,
                        capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
(out/'diagnostic-export.log').write_bytes(result.stdout+result.stderr)
assert result.returncode == 0
diagnostic = json.loads(export.read_text(encoding='utf-8'))
assert diagnostic['source_revision'] == manifest['source_revision']
assert diagnostic['application_version'] == manifest['version']
assert not diagnostic['automatic_upload']
# Tamper detection acts only on a copied temporary file and never executes it.
test_file = work/'tamper-copy.dll'
shutil.copy2(reinstalled/'Qt6Core.dll', test_file)
with test_file.open('ab') as file: file.write(b'test integrity marker')
assert hashlib.file_digest(test_file.open('rb'), 'sha256').hexdigest() != manifest['files']['Qt6Core.dll']['sha256']
result = dict(candidate=str(candidate), retained_rehearsal_directory=str(work),
    version=manifest['version'], source_revision=manifest['source_revision'],
    zip_sha256_verified=True, manifest_files_verified=len(manifest['files']), clean_path=environment['PATH'],
    runs=rows, removal='application directory detached by rename, retained as evidence; no registry entries',
    settings_preserved=True, native_document_preserved=True, explicit_diagnostic_export=True,
    tampered_copy_detected=True, no_admin_install=True,
    limitations=['developer workstation; not a pristine VM', 'same-version side-by-side replacement rehearsal',
                 'no installer repair or registered file association exists', 'no signing or SmartScreen claim'])
(out/'summary.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
print(json.dumps(result, indent=2))
