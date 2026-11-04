# SPDX-License-Identifier: GPL-3.0-or-later
"""Install only Qt Base into ignored .tools; aqt 3.3 cannot find Qt 6.12's split metadata."""
from pathlib import Path
import hashlib
import xml.etree.ElementTree as ET
import requests
import py7zr

BASE = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120_msvc2022_64/"
OUT = Path(__file__).resolve().parents[1] / ".tools/qt"

def get(url):
    response = requests.get(url, timeout=120)
    response.raise_for_status()
    return response.content

if __name__ == "__main__":
    xml = get(BASE + "Updates.xml")
    # Official HTTPS metadata and published archive hash; SHA1 is the vendor's checksum.
    expected = get(BASE + "Updates.xml.sha256").decode().strip().split()[0]
    assert hashlib.sha256(xml).hexdigest() == expected
    root = ET.fromstring(xml)
    package = next(p for p in root.findall("PackageUpdate") if p.findtext("Name") == "qt.qt6.6120.win64_msvc2022_64")
    name, version = package.findtext("Name"), package.findtext("Version")
    archive = next(a.strip() for a in package.findtext("DownloadableArchives").split(",") if a.strip().startswith("qtbase-"))
    url = BASE + name + "/" + version + archive
    expected = get(url + ".sha1").decode().strip().split()[0]
    data = get(url)
    assert hashlib.sha1(data).hexdigest() == expected
    OUT.mkdir(parents=True, exist_ok=True)
    archive_path = OUT / "qtbase.7z"
    if archive_path.exists(): raise FileExistsError(archive_path)
    archive_path.write_bytes(data)
    print("Verified", len(data), "bytes", expected, flush=True)
    with py7zr.SevenZipFile(archive_path) as z:
        for name in z.getnames():
            target = (OUT/name).resolve()
            if not target.is_relative_to(OUT.resolve()): raise ValueError("Archive escapes tools directory")
        z.extractall(OUT)
    print("Qt Base extracted under", OUT, flush=True)
