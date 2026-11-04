# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch and verify the exact Qt Base source archive for local redistribution review."""
import hashlib
import json
import pathlib
import tarfile
import urllib.request

root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'build/phase3-dependencies'
out.mkdir(parents=True, exist_ok=True)
name = 'qtbase-everywhere-src-6.12.0.tar.xz'
base = 'https://download.qt.io/archive/qt/6.12/6.12.0/submodules/'
with urllib.request.urlopen(base + name + '.sha256', timeout=60) as response:
    checksum = response.read(1024).decode().strip()
expected = checksum.split()[0]
archive = out / name
if not archive.exists():
    with urllib.request.urlopen(base + name, timeout=120) as response, archive.open('xb') as file:
        total = 0
        for block in iter(lambda: response.read(1024 * 1024), b''):
            total += len(block)
            if total > 256 * 1024 * 1024:
                raise ValueError('Qt archive exceeds the download budget')
            file.write(block)
actual = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest()
if actual != expected:
    raise SystemExit('Qt source checksum mismatch; retained for investigation')
notices = out / 'qt-notices'
notices.mkdir(exist_ok=True)
count = 0
with tarfile.open(archive, 'r:xz') as tar:
    for entry in tar:
        parts = pathlib.PurePosixPath(entry.name).parts
        if not entry.isfile() or len(parts) < 2 or '..' in parts:
            continue
        relative = pathlib.PurePosixPath(*parts[1:])
        if (str(relative).startswith('LICENSES/') or
                relative.name == 'qt_attribution.json' or
                (str(relative).startswith('src/3rdparty/') and
                 relative.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')))):
            if entry.size > 4 * 1024 * 1024:
                raise ValueError('Oversized license text')
            destination = notices / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            data = tar.extractfile(entry).read()
            if destination.exists():
                assert destination.read_bytes() == data
            else:
                destination.write_bytes(data)
            count += 1
# Phase 4-G.1: also every license file that Qt's attribution metadata names (LicenseFile(s)), such
# as src/gui/util/LICENSE.XSVG.txt or src/3rdparty/pcre2/LICENCE.md. They carry the component
# copyright notices that the generic LICENSES/ texts do not.
referenced = set()
for attribution in notices.rglob('qt_attribution.json'):
    data = json.loads(attribution.read_text(encoding='utf-8'), strict=False)
    for entry in data if isinstance(data, list) else [data]:
        for name_ in entry.get('LicenseFiles') or ([entry['LicenseFile']] if entry.get('LicenseFile') else []):
            path = pathlib.PurePosixPath(*(attribution.parent / name_).resolve().relative_to(notices.resolve()).parts)
            if '..' in path.parts:
                raise ValueError(f'Attribution escapes the source: {attribution}')
            referenced.add(str(path))
with tarfile.open(archive, 'r:xz') as tar:
    for entry in tar:
        parts = pathlib.PurePosixPath(entry.name).parts
        if not entry.isfile() or len(parts) < 2:
            continue
        relative = pathlib.PurePosixPath(*parts[1:])
        if str(relative) not in referenced:
            continue
        referenced.discard(str(relative))
        if entry.size > 4 * 1024 * 1024:
            raise ValueError('Oversized license text')
        destination = notices / relative
        data = tar.extractfile(entry).read()
        if destination.exists():
            assert destination.read_bytes() == data
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        count += 1
if referenced:
    raise SystemExit(f'License files named by Qt attributions are missing from the source: {sorted(referenced)}')
count = sum(1 for f in notices.rglob('*') if f.is_file())
metadata = dict(url=base + name, sha256_url=base + name + '.sha256', sha256=actual,
                bytes=archive.stat().st_size, notice_files=count,
                sdk_sbom_sha256=hashlib.sha256((root / '.tools/qt/sbom/qtbase-6.12.0.spdx.json').read_bytes()).hexdigest())
(out / 'qt-source.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
print(json.dumps(metadata, indent=2))
