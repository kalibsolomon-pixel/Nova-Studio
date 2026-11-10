"""Recursively scan candidate bytes and archive members without extracting or printing private matches."""
import argparse
import hashlib
import io
import json
import os
import pathlib
import re
import subprocess
import tarfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=pathlib.Path)
    parser.add_argument('--report', required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    home = os.environ['USERPROFILE']
    private = [home, home.replace('\\', '/'), home.replace('\\', '\\\\')]
    tokens = [value.encode(encoding).lower() for value in private for encoding in ('utf-8', 'utf-16le')]
    secrets = re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|sk-(?:proj-)?[A-Za-z0-9_-]{40,}|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----)')
    results = {'format': 'nova-phase4i2-privacy', 'files': 0, 'archive_members': 0,
               'personal_paths': [], 'credential_candidates': [], 'forbidden_entries': [],
               'generic_profile_path_files': [],
               'absolute_build_path_files': [], 'upstream_test_material': [], 'errors': [], 'inputs': {}}

    def scan(name, data, depth=0, upstream=False):
        lowered = data.lower()
        if any(token in lowered for token in tokens):
            results['personal_paths'].append(name)
        profile_bytes = lowered.replace(b'\0', b'') if b'\0' in lowered else lowered
        if re.search(rb'[a-z]:[\\/]+users[\\/]', profile_bytes):
            results['generic_profile_path_files'].append(name)
        if secrets.search(data):
            results['upstream_test_material' if upstream else 'credential_candidates'].append(name)
        if b'c:\\dev\\' in lowered or b'c:/dev/' in lowered:
            results['absolute_build_path_files'].append(name)
        components = name.replace('\\', '/').lower().split('/')
        if any(component in ('.git', '.nova', '.claude', '.codex') for component in components):
            results['forbidden_entries'].append(name)
        if depth > 3:
            results['errors'].append({'file': name, 'error': 'nested archive depth exceeded'})
            return
        try:
            if data[:4] == b'PK\x03\x04':
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    for member in archive.infolist():
                        if not member.is_dir():
                            results['archive_members'] += 1
                            scan(name + '::' + member.filename, archive.read(member), depth + 1, upstream)
            elif name.endswith(('.tar.xz', '.tar.gz', '.tgz')):
                pristine = upstream or hashlib.sha256(data).hexdigest() == 'a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307'
                with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
                    for member in archive:
                        if member.isfile():
                            results['archive_members'] += 1
                            scan(name + '::' + member.name, archive.extractfile(member).read(), depth + 1, pristine)
            elif name.endswith('.tar.zst'):
                pristine = hashlib.sha256(data).hexdigest() == 'a901d8afb78438abcff9ed020cd9aa9431db76de1992171c2454c3c07ad289e8'
                tool = pathlib.Path(os.environ.get('NOVA_MSYS2_ROOT', 'C:/Dev/nova-toolchains/msys64-20260927')) / 'ucrt64/bin/zstd.exe'
                if not tool.is_file():
                    raise OSError('Pinned zstd decompressor unavailable')
                process = subprocess.Popen([str(tool), '-dc', '--', str(current_source)], stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE)
                try:
                    with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
                        for member in archive:
                            if member.isfile():
                                results['archive_members'] += 1
                                scan(name + '::' + member.name, archive.extractfile(member).read(), depth + 1, pristine)
                    process.stdout.close()
                    if process.wait(timeout=30):
                        raise OSError('zstd decompression failed')
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
        except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
            results['errors'].append({'file': name, 'error': type(error).__name__})

    for source in arguments.paths:
        files = sorted(source.rglob('*')) if source.is_dir() else [source]
        for path in files:
            if not path.is_file():
                continue
            data = path.read_bytes()
            current_source = path
            name = (source.name + '/' + path.relative_to(source).as_posix()) if source.is_dir() else source.name
            results['files'] += 1
            results['inputs'][name] = hashlib.sha256(data).hexdigest()
            scan(name, data)
    results['passed'] = not any(results[key] for key in ('personal_paths', 'credential_candidates', 'forbidden_entries', 'errors'))
    arguments.report.write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: len(value) if isinstance(value, (list, dict)) else value
                      for key, value in results.items()}))
    raise SystemExit(0 if results['passed'] else 1)


if __name__ == '__main__':
    main()
