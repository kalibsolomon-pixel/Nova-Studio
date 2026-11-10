# Third-party components in Nova Studio

Nova Studio itself is free software under the GNU General Public License version 3 or later
(GPL-3.0-or-later): see `LICENSE.txt` and `licenses/COPYRIGHT.md`. The components below are not
Nova Studio's work. They keep their own licenses, which Nova Studio's license does not change.

This preview ships the components below. Their license texts are in the `licenses` folder:
`qt/` holds the Qt Base notices and license texts (167 files from the Qt Base 6.12.0 source, including every license file that Qt's
attribution metadata names),
`qtbase-6.12.0.spdx.json` is Qt's software bill of materials, `qt-build/config_qtbase.opt` records
how The Qt Company configured the shipped Qt build, `MICROSOFT-VC-RUNTIME-TERMS.txt` and
`MICROSOFT-REDIST.txt` cover the Microsoft runtime, `MaterialSymbolsOutlined-LICENSE.txt` covers the
Material Symbols font, and `gradle-wrapper-9.8.2-LICENSE.txt` covers the Gradle wrapper.

## Qt Base 6.12.0

`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6OpenGL.dll`, `Qt6OpenGLWidgets.dll`,
`Qt6Network.dll` and `plugins/platforms/qwindows.dll` are unmodified Qt Base 6.12.0 libraries,
built by The Qt Company and dynamically linked by `nova-studio.exe` and `nova-mcp.exe`. Qt is
available under the GNU Lesser General Public License version 3 (LGPL-3.0-only), GPL-2.0-only,
GPL-3.0-only, or a commercial license from The Qt Company. **Nova Studio uses Qt under the
LGPL-3.0-only** (`qt/LICENSES/LGPL-3.0-only.txt`, which incorporates `qt/LICENSES/GPL-3.0-only.txt`).
Copyright (C) The Qt Company Ltd. and other contributors.

You may replace these DLLs with your own build of the same Qt version. Nova Studio adds no
signature or integrity check that prevents replacement. The complete corresponding Qt Base source
is included as `qtbase-everywhere-src-6.12.0.tar.xz` (SHA-256
`a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307`, as published at
download.qt.io). The configure options of the shipped build are in `qt-build/config_qtbase.opt`.
`nova-studio.exe` also uses Qt's private ZIP reader, so a replacement Qt must be version 6.12.0.

The Qt libraries contain the following third-party code, as recorded in Qt's SBOM:

| Component | Version | License | In |
| --- | --- | --- | --- |
| FreeType | 2.14.3 | FTL (FreeType License) OR GPL-2.0-only; used under the FTL | Qt6Gui |
| HarfBuzz | 14.5.0 | MIT | Qt6Gui |
| libpng | 1.6.58 | Libpng AND libpng-2.0 | Qt6Gui |
| zlib | 1.3.2 | Zlib | Qt6Core, Qt6Gui, Qt6Network |
| PCRE2 (and sljit) | 10.48 | BSD-3-Clause with the PCRE2 binary-like packages exception; BSD-2-Clause | Qt6Core |
| double-conversion | 3.4.0 | BSD-3-Clause | Qt6Core |
| TinyCBOR | 7.0 | MIT | Qt6Core |
| Easing equations (Robert Penner) | — | BSD-3-Clause | Qt6Core |
| RFC 6234 SHA code | — | BSD-3-Clause | Qt6Core |
| SHA-3 (Keccak, endian helpers) | 3.2 / 1.0.0 | CC0-1.0; BSD-2-Clause | Qt6Core |
| BLAKE2, SipHash, MD4, MD5, tl-expected | — | CC0-1.0 (BLAKE2: CC0-1.0 or Apache-2.0) | Qt6Core |
| SHA-1 (Dominik Reichl) | — | Public domain | Qt6Core |
| Apache Tika MIME types | — | Apache-2.0 | Qt6Core |
| Unicode Character Database and CLDR | CLDR 48.2 | Unicode-3.0 | Qt6Core |
| md4c | 0.5.3 | MIT | Qt6Gui |
| Grayraster (FreeType) | — | FTL OR GPL-2.0-only; used under the FTL | Qt6Gui |
| Smooth scaling (Imlib2) | — | BSD-2-Clause and Imlib2 | Qt6Gui |
| X server helper, XSVG | — | X11 and HPND; HPND-sell-variant | Qt6Gui |
| Adobe Glyph List for New Fonts | 1.7 | BSD-3-Clause | Qt6Gui |
| Khronos OpenGL / OpenGL ES headers, Vulkan XML | — | MIT; Apache-2.0 or MIT | Qt6Gui |
| Vulkan / D3D12 Memory Allocator, D3D12 mipmap shader | 3.3.0 / 3.1.0 | MIT | Qt6Gui |
| Emoji segmenter | 0.4.0 | Apache-2.0 | Qt6Gui |
| WebGradients | — | MIT | Qt6Gui |
| ICC sRGB profile | — | ICC license | Qt6Gui |
| libpsl | — | BSD-3-Clause | Qt6Network |
| Public Suffix List data | 2026-05-14 | MPL-2.0 | Qt6Network |
| Wintab interface | — | LCS/Telegraphics license | qwindows.dll |

Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org).
All rights reserved.

The Public Suffix List data is covered by the Mozilla Public License 2.0. Its source form is
included in the Qt Base source archive above (`src/3rdparty/libpsl`) and is published at
https://publicsuffix.org/.

The license texts for every component above are in `qt/` and in the Qt Base source archive.

## Microsoft Visual C++ runtime

`concrt140.dll`, `msvcp140.dll`, `msvcp140_1.dll`, `msvcp140_2.dll`, `msvcp140_atomic_wait.dll`,
`msvcp140_codecvt_ids.dll`, `vccorlib140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` and
`vcruntime140_threads.dll` are the Microsoft Visual C++ 14.51 runtime (file version
14.51.36247.0), copyright Microsoft Corporation. They are copied unmodified from the
`VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT` folder of Visual Studio Build Tools 2026 and
distributed as Microsoft "Distributable Code" (Microsoft's list: https://aka.ms/vs/18/redistribution;
`MICROSOFT-REDIST.txt` is Microsoft's own pointer to it).

**They are not covered by the GPL**, and Nova Studio's license gives no rights to them. By
installing, copying or using them you agree to the terms in `MICROSOFT-VC-RUNTIME-TERMS.txt`, which
protect them as Microsoft requires. Microsoft does not endorse, sponsor or support Nova Studio.
They are the runtime of the compiler that built Nova Studio, which GPL-3.0 section 1 treats as a
System Library, so their source code is not part of Nova Studio's corresponding source.

## Material Symbols Outlined

`nova-studio.exe` embeds the unmodified Material Symbols Outlined variable font, copyright 2026
Google LLC, from the Google Material Design Icons project at commit
`10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7` (SHA-256
`f7837ba8573d9a7a74931618ced8307bc81f96d49af296bf6c8052c772ed5282`). It supplies command and
editor-tool glyphs under the Apache License 2.0 (`MaterialSymbolsOutlined-LICENSE.txt`). The font
and its license are also in the corresponding source archive under `assets/fonts/`.

## Gradle wrapper 9.8.2

`nova-studio.exe` embeds `gradle-wrapper-9.8.2.jar` (Gradle, Apache License 2.0; SHA-256
`3b8a25775a69158b5ad2b1d17a80a88dc7b40a352a73eb27d06c612a7ce68e98`) and writes it into the
managed Minecraft test workspace. The jar is unmodified. Its license is
`gradle-wrapper-9.8.2-LICENSE.txt`. Gradle's source code is published by the Gradle project at
https://github.com/gradle/gradle (tag `v9.8.2`).

## Not shipped

Downloaded only when you choose Test in Minecraft, from their publishers, under their own terms,
and not redistributed by Nova Studio: Eclipse Temurin (GPL-2.0 with the Classpath Exception),
the Gradle distribution, Fabric Loader and Fabric API (Apache-2.0), Fabric Loom (MIT), Forge
Config API Port, RRLS, and Minecraft Java Edition (Minecraft EULA). Nova Studio contains no
Minecraft game assets.

The Windows installer was built with NSIS 3.13 (https://nsis.sourceforge.io/) using its zlib
compressor. The NSIS installer code it contains is under the zlib/libpng license.
The official MCP Python SDK, Python, CMake and Ninja are development tools and are not shipped.
