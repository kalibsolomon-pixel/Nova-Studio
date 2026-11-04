# Nova Studio icon

`nova-studio-icon.svg` is the editable application icon master. Its matching
`nova-studio-icon.png` is 1024 × 1024 with transparent outer corners. PNG exports
are also provided at 16, 24, 32, 48, 64, 128, 256 and 512 pixels.

`nova-studio-mark.svg` and `nova-studio-mark.png` contain the starburst alone on
a transparent 1024 × 1024 canvas.

The eight-point starburst uses paired triangular gem facets. Palette and app
tile treatment follow the current Aurora native launcher reference:
`C:/Dev/aurora-launcher-native/native/assets/aurora.svg` and its icon generator
(November 1, 2026). The tile uses the same dark gradient, n=5 superellipse and
40 px outer inset. The mark retains Aurora's teal, cyan, indigo and violet.

Regenerate from the repository root with Node.js and a build-time Sharp install:

```powershell
node scripts/generate-brand-icons.mjs <absolute-path-to-sharp-package>
```

The SVG uses standard paths and gradients, without embedded raster images,
fonts, filters or external resources. Exports were visually checked at native
16–128 px sizes and verified for dimensions and alpha transparency. These are
embedded in the application via Qt resources. The welcome screen and toolbar
use the transparent mark; windows and dialogs use the multi-resolution ICO.
The same ICO is embedded as the Windows executable icon by `version.rc.in`.
No external image files or SVG plugins are needed at runtime.
