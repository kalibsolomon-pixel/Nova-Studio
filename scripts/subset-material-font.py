#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Subset MaterialSymbolsOutlined.ttf to the exact codepoints mapped in src/ui/theme.cpp.

Retains all variable font axes (FILL, GRAD, opsz, wght) and all font name/copyright/license
tables (Apache-2.0 §4(b)-(c)) so glyph rasterization stays pixel-identical to the upstream
variable font while shrinking the embedded TTF from ~10.74 MB to ~90 KB.

Usage:
    python scripts/subset-material-font.py [--input path/to/upstream.ttf] [--output assets/fonts/MaterialSymbolsOutlined.ttf]
"""

import argparse
import hashlib
import pathlib
import re
import sys
from fontTools import subset, ttLib

UPSTREAM_COMMIT = "10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7"
UPSTREAM_SHA256 = "f7837ba8573d9a7a74931618ced8307bc81f96d49af296bf6c8052c772ed5282"


def extract_codepoints(theme_cpp: pathlib.Path) -> list[int]:
    text = theme_cpp.read_text(encoding="utf-8")
    match = re.search(
        r"QChar\s+materialGlyph\s*\([^)]*\)\s*\{(.*?)\n\}",
        text,
        flags=re.DOTALL,
    )
    if not match:
        raise RuntimeError(f"Could not locate materialGlyph() in {theme_cpp}")
    body = match.group(1)
    cps = sorted({int(m.group(1), 16) for m in re.finditer(r"QChar\(\s*0x([0-9a-fA-F]+)\s*\)", body)})
    if not cps:
        raise RuntimeError(f"No QChar(0x...) codepoints found in materialGlyph() in {theme_cpp}")
    return cps


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, default=root / "assets/fonts/MaterialSymbolsOutlined.ttf")
    parser.add_argument("--output", type=pathlib.Path, default=root / "assets/fonts/MaterialSymbolsOutlined.ttf")
    parser.add_argument("--theme", type=pathlib.Path, default=root / "src/ui/theme.cpp")
    args = parser.parse_args()

    codepoints = extract_codepoints(args.theme)
    in_sha = sha256_file(args.input)
    in_size = args.input.stat().st_size
    print(f"Input : {args.input} ({in_size:,} bytes, sha256={in_sha})")
    print(f"Codepoints from {args.theme.name}: {len(codepoints)} unique")

    font = ttLib.TTFont(args.input)
    options = subset.Options()
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.name_languages = ["*"]
    options.notdef_outline = True
    options.recalc_timestamp = False
    options.recalc_bounds = True
    options.drop_tables = []

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    tmp = args.output.with_suffix(".ttf.tmp")
    font.save(tmp)
    font.close()

    # Verify every mapped codepoint is present in the output cmap and all 4 axes remain.
    verify = ttLib.TTFont(tmp)
    cmap = verify.getBestCmap()
    missing = [hex(cp) for cp in codepoints if cp not in cmap]
    axes = [a.axisTag for a in verify["fvar"].axes] if "fvar" in verify else []
    verify.close()
    if missing:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(f"Subset font is missing codepoints: {missing}")
    if set(axes) != {"FILL", "GRAD", "opsz", "wght"}:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(f"Expected all 4 variable axes, got: {axes}")

    tmp.replace(args.output)
    out_sha = sha256_file(args.output)
    out_size = args.output.stat().st_size
    print(f"Output: {args.output} ({out_size:,} bytes, sha256={out_sha})")
    print(f"Axes  : {axes}")
    print(f"Saved : {in_size - out_size:,} bytes ({100.0 * (in_size - out_size) / in_size:.2f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
