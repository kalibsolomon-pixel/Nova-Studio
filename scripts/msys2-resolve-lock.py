# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve an exact MSYS2 package lock from a pacman sync database (Phase 4-I).

Reads a downloaded repository database (for example ucrt64.db from
https://repo.msys2.org/mingw/ucrt64/), resolves the full dependency closure of the requested
packages, and writes a lock file listing every package with its exact version, file name,
SHA-256 and licence as recorded by the repository. Nothing is downloaded or installed here.

    python scripts/msys2-resolve-lock.py ucrt64.db --repo-url https://repo.msys2.org/mingw/ucrt64 \
        --out packaging/mingw/msys2-ucrt64-lock.json gcc cmake ninja qt6-base

Package names may be given without the mingw-w64-ucrt-x86_64- prefix. The database may be
zstd-compressed (needs Python 3.14, backports.zstd or zstandard), gzip-compressed or a plain tar.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import sys
import tarfile
from pathlib import Path

PREFIX = "mingw-w64-ucrt-x86_64-"


def open_database(path: Path) -> tarfile.TarFile:
    raw = path.read_bytes()
    if raw[:4] == b"\x28\xb5\x2f\xfd":  # zstd frame
        data = None
        for module in ("compression.zstd", "backports.zstd"):
            try:
                zstd = __import__(module, fromlist=["decompress"])
                data = zstd.decompress(raw)
                break
            except ImportError:
                continue
        if data is None:
            try:
                import zstandard  # type: ignore
                data = zstandard.ZstdDecompressor().decompressobj().decompress(raw)
            except ImportError:
                sys.exit("The database is zstd-compressed and no zstd module is available; "
                         "decompress it first (zstd -d) and pass the .tar file.")
        return tarfile.open(fileobj=io.BytesIO(data), mode="r:")
    return tarfile.open(fileobj=io.BytesIO(raw), mode="r:*")


def parse_desc(text: str) -> dict[str, list[str]]:
    fields: dict[str, list[str]] = {}
    key = None
    for line in text.splitlines():
        if line.startswith("%") and line.endswith("%"):
            key = line.strip("%")
            fields[key] = []
        elif line and key:
            fields[key].append(line)
    return fields


def load(path: Path) -> dict[str, dict]:
    packages: dict[str, dict] = {}
    with open_database(path) as tar:
        for member in tar.getmembers():
            if not member.isfile() or not member.name.endswith("/desc"):
                continue
            fields = parse_desc(tar.extractfile(member).read().decode("utf-8"))
            name = fields["NAME"][0]
            packages[name] = {
                "name": name,
                "version": fields["VERSION"][0],
                "filename": fields["FILENAME"][0],
                "sha256": fields.get("SHA256SUM", [""])[0],
                "compressed_size": int(fields.get("CSIZE", ["0"])[0]),
                "installed_size": int(fields.get("ISIZE", ["0"])[0]),
                "license": fields.get("LICENSE", []),
                "url": fields.get("URL", [""])[0],
                "base": fields.get("BASE", [name])[0],
                "builddate": int(fields.get("BUILDDATE", ["0"])[0]),
                "depends": fields.get("DEPENDS", []),
                "provides": fields.get("PROVIDES", []),
            }
    return packages


def dep_name(spec: str) -> str:
    return re.split(r"[<>=]", spec, maxsplit=1)[0]


def resolve(packages: dict[str, dict], roots: list[str]) -> tuple[list[dict], dict[str, list[str]]]:
    provides: dict[str, str] = {}
    for pkg in packages.values():
        for spec in pkg["provides"]:
            provides.setdefault(dep_name(spec), pkg["name"])
    chosen: dict[str, dict] = {}
    required_by: dict[str, list[str]] = {}
    pending = [(r if r.startswith(PREFIX) else PREFIX + r, "(requested)") for r in roots]
    while pending:
        name, parent = pending.pop()
        real = name if name in packages else provides.get(name)
        if real is None:
            raise SystemExit(f"unresolvable dependency {name!r} (required by {parent})")
        required_by.setdefault(real, []).append(parent)
        if real in chosen:
            continue
        chosen[real] = packages[real]
        for spec in packages[real]["depends"]:
            pending.append((dep_name(spec), real))
    return sorted(chosen.values(), key=lambda p: p["name"]), required_by


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("database", type=Path)
    parser.add_argument("packages", nargs="+")
    parser.add_argument("--repo-url", default="https://repo.msys2.org/mingw/ucrt64")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    db_sha = hashlib.sha256(args.database.read_bytes()).hexdigest()
    packages = load(args.database)
    locked, required_by = resolve(packages, args.packages)
    lock = {
        "schema": "nova-msys2-lock/1",
        "repository": args.repo_url,
        "database_sha256": db_sha,
        "requested": args.packages,
        "package_count": len(locked),
        "total_download_bytes": sum(p["compressed_size"] for p in locked),
        "total_installed_bytes": sum(p["installed_size"] for p in locked),
        "packages": [
            {
                "name": p["name"], "version": p["version"], "filename": p["filename"],
                "url": f"{args.repo_url}/{p['filename']}", "sha256": p["sha256"],
                "compressed_size": p["compressed_size"], "installed_size": p["installed_size"],
                "license": p["license"], "base": p["base"], "upstream": p["url"],
                "required_by": sorted(set(required_by[p["name"]])),
            }
            for p in locked
        ],
    }
    text = json.dumps(lock, indent=1) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8", newline="\n")
    print(f"{len(locked)} packages, download {lock['total_download_bytes'] / 2**20:.1f} MiB, "
          f"installed {lock['total_installed_bytes'] / 2**20:.1f} MiB")
    for p in locked:
        print(f"  {p['name'][len(PREFIX):] if p['name'].startswith(PREFIX) else p['name']:<34} "
              f"{p['version']:<28} {p['installed_size'] / 2**20:8.1f} MiB  {' '.join(p['license'])[:60]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
