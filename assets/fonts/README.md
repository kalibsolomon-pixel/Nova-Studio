# Material Symbols Outlined

Nova Studio embeds a subset of the `MaterialSymbolsOutlined.ttf` variable font containing the 73
unique codepoints mapped in `src/ui/theme.cpp` (preserving all four variable axes — `FILL`,
`GRAD`, `opsz`, `wght` — and all upstream copyright/name tables). The font is loaded from the Qt
resource system; no runtime download or web dependency is used.

- Upstream: https://github.com/google/material-design-icons
- Pinned commit: `10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7`
- Upstream font: https://raw.githubusercontent.com/google/material-design-icons/10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7/variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf
- Upstream font SHA-256: `f7837ba8573d9a7a74931618ced8307bc81f96d49af296bf6c8052c772ed5282`
- Codepoints: https://raw.githubusercontent.com/google/material-design-icons/10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7/variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.codepoints
- Codepoints SHA-256: `a7404d9e5a1b63f496cf05ace45d386075886b5a7f123fba9f29527cb6006fde`
- License: Apache License 2.0, copied verbatim as `MaterialSymbolsOutlined-LICENSE.txt`.

## Modification notice (Apache-2.0 §4(b))

`assets/fonts/MaterialSymbolsOutlined.ttf` was subsetted from the pinned upstream font above using
`scripts/subset-material-font.py` (with `fontTools.subset` 4.68.0) to retain only the 73 unique
glyph codepoints mapped by Nova Studio in `src/ui/theme.cpp`, keeping all four variable axes and
all name/copyright/license tables intact:

- Subset size: `122,576` bytes (down from `10,744,832` bytes)
- Subset SHA-256: `81cd837372f05491a2d8b163e9aa61f968fd178eb5d5dbb21043259cbd149778`

The font defaults are retained: outline style, FILL 0, GRAD 0, weight 400, and optical size 24. Nova renders glyph ink using the active normal or disabled palette color. The codepoint mapping is maintained in `src/ui/theme.cpp`.
