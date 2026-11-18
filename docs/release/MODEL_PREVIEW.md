# Minecraft model preview

The Models / UV panel resolves models from your pack and configured resource stack.
It supports the 1.21.11, 26.1 and 26.3 model profiles. The lighting and render-layer
rules below use Java Edition 1.21.11 as the reference.

- **GUI** applies the authored display transform in a centered 16×16 logical inventory slot.
  The slot clips ordinary item models. `oversized_in_gui` extends the visible bounds when the
  item is larger than the slot. The viewport scales the slot by whole pixels.
- **First person** uses a fully raised, stationary item and a level 70° camera. **Third person**
  uses an upright classic-width adult player's idle held-item arm at −18° pitch. **Head** uses
  the upright head pivot. **Ground** holds one item at zero spin and bob with its model bottom
  1/16 block above the floor. **Fixed** uses a visible south-facing item frame at rotation zero.
- **Placed** uses overworld face shade and the model's `ambientocclusion` setting. It starts
  with an isolated block surrounded by air. To test a flat solid floor, enter
  `{"_neighbors":"floor"}` in Preview inputs. For individual opaque full-cube neighbors, use
  offsets from the preview block, such as `{"_neighbors":[[1,0,0],[0,-1,0]]}`. Only offsets
  within one block are supported. The preview uses the neighbors for culling and lighting.
- **Vanilla lighting** is the default. GUI lighting follows the resolved `gui_light` and
  generated-item parents default to `front`. **Studio lighting** enables the ambient, direction
  and Neutral inspection controls.

Block tint uses plains biome colors by default. Grass, foliage and dry foliage use the
resource stack's 256×256 colormaps when available. Item tints use the item definition and
its fallback colors. Enter `{"_tints":[16711680,null,65280]}` to override runtime or block
tint slots 0 and 2.
The preview also reads block properties such as redstone `power` and crop `age`.

Explorer model icons use the GUI view when the catalog can resolve geometry and the OpenGL
context is available. Animated textures use their first frame in Explorer. Texture rows
show the texture itself.

The preview uses nearest sprite sampling and uniform world light. Full level lighting,
GPU atlas behavior, fluid geometry, and Minecraft's scene and pose animations require a
game capture. Some item definitions still need preview inputs to resolve predicates or
choose a weighted alternative. Player profiles, component-driven entity patterns and the
remaining special item renderers require further work.

## Checking against the game

With Minecraft running from **Test in Minecraft**, **Compare Icons…** asks the game (through
Nova's companion mod) to draw a list of items in inventory slots on the vanilla slot grey at its
current GUI scale. Nova draws the same items with the inventory pose above and shows Nova,
Minecraft and a difference image for each, counting pixels whose channels differ by more than 2.
The reference set covers full blocks, cutout and translucent blocks, tinted items, generated and
handheld items and special models; **This Pack's Items** uses the pack's own item definitions.
The images and a `report.json` stay in the test instance's `nova-companion` folder.

### Vanilla baseline (Java 1.21.11)

`tests/parity/vanilla-1.21.11-gui.json` records a sweep of every vanilla item (1,505 item
definitions) drawn by the game and by Nova at each GUI scale, glint off. Rerun it with
`nova-companion-live-probe <scratch> --full` (see the file header) and compare the counts.

| GUI scale | Match | Differ | Nova cannot draw |
| --- | ---: | ---: | ---: |
| 1 | 1,454 of 1,477 | 23 | 28 |
| 2 | 1,451 of 1,477 | 26 | 28 |
| 3 | 1,447 of 1,477 | 30 | 28 |
| 4 | 1,190 of 1,477 | 287 | 28 |

- Nova cannot draw banners, the decorated pot, dragon and piglin heads and copper golem statues
  (special renderers) or `air`.
- Interpolated animations (prismarine, magma, sculk, stems and hyphae, command blocks) are
  captured between frames; the closest whole frame is within 4 to 22 levels.
- `light`, `heavy_core`, spawners, the vault, frogspawn and the dripleaf and azalea plants still
  differ. Beds differ in 2 pixels at scale 3.
- At GUI scale 4 most other cubes differ in 1 or 2 pixels on the top ridge, where the game assigns
  the pixel on the shared edge to the top face.

## Java 1.21.11 rendering references

| Behavior | Reference and constants |
| --- | --- |
| Item light | `Lighting` and `assets/minecraft/shaders/include/light.glsl`. Normalize `(0.2, 1, -0.7)` and `(-0.2, 1, 0.7)`. Multiply RGB by `min(1, 0.4 + 0.6 × (max(0, L0·N) + max(0, L1·N)))`. In Minecraft's GUI coordinates, the flat rig uses `Ry(-22.5°) Rx(135°)` and the side rig prepends `scale(1,-1,1) Ry(62°) Rx(185.5°)`. |
| Placed light | `ClientLevel.getShade` gives up/down/north-south/east-west brightness of `1.0/0.5/0.8/0.6`, with `shade:false` giving `1.0`. `ModelBlockRenderer.AmbientOcclusionRenderStorage.calculate` averages four brightness samples per corner at weight `0.25`. Nova's neighborhood uses `0.2` for opaque full cubes and `1.0` for air. |
| Alpha | `ItemBlockRenderTypes`, `ChunkSectionLayer`, `BlockModelWrapper.detectRenderType`, `Sheets`, and `RenderPipelines`. Entity cutout and translucent item sheets use `0.1`. Terrain cutout uses `0.5`, translucent terrain uses `0.01`, tripwire uses `0.1` and solid terrain draws unconditionally. The version has one terrain cutout layer. `MissingTextureAtlasSprite.generateMissingImage` uses 16×16 pixels with black on the top-left and bottom-right quarters and `#F800F8` on the others. |
| Context poses | `GuiRenderer.renderItemToAtlas` centers the slot at `(8,8)`, scales by `(16,-16,16)`, and uses a `[-1000,1000]` depth range at GUI scale one. `GuiItemRenderState.calculateOversizedItemBounds` uses ceiled extent sizes and floored origins. `ItemInHandRenderer.applyItemArmTransform` uses `(±0.56,-0.52,-0.72)` blocks at full equip. `ItemInHandLayer.submitArmWithItem` then applies `Rx(-90°) Ry(180°)` and `(±1/16,1/8,-5/8)` at the arm pivot. `CustomHeadLayer.translateToHead` uses Y translation `-0.25`, Y rotation `180°`, and scale `(0.625,-0.625,-0.625)` in player-model coordinates. `ItemEntityRenderer.submit` offsets by `1/16 - minY` before bob. `ItemFrameRenderer.submit` uses Z offset `0.4375` and item scale `0.5` for the visible frame. |
| Generated sprites | `ItemModelGenerator.processFrames`, `createSideElements`, and `SpriteContents.isTransparent`. Front and back at `z = 8.5` and `7.5` in model pixels, edge UVs inset by `0.1` texel, layer tint index `0–4`, and a boundary where the neighbor has alpha zero. |
| Tint colors | `ColorMapColorUtil.get` samples integer coordinates `255 × (1−temperature)` and `255 × (1−temperature×downfall)`. `OverworldBiomes.plains` uses temperature `0.8`, downfall `0.4`, and water `#3F76E4`. `BlockColors` supplies fixed tints, including spruce `#619961` and birch `#80A755`. `RedStoneWireBlock` uses `p = power/15`, red `0.6p + (power > 0 ? 0.4 : 0.3)`, green `max(0, 0.7p²−0.5)` and blue zero. Crop stems use RGB `(32×age, 255−8×age, 4×age)`. `GrassColorSource` defaults to `(0.5,1)`. Item defaults from `Potion`, `Firework`, and `MapItemColor` are `#385DC6`, `#8A8A8A`, and `#46402E`. |
| Special geometry | Box and UV dimensions come from `ChestModel.createSingleBodyLayer`, `ShulkerModel.createBoxLayer`, `BedRenderer.createHeadLayer` and `createFootLayer`, `SkullModel.createHeadModel` and `createHumanoidHeadLayer`, `ShieldModel.createLayer`, and `ConduitRenderer.createShellLayer`. Entity textures come from the matching `textures/entity/` paths in the resource stack. |
