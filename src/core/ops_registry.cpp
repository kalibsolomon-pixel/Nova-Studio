// SPDX-License-Identifier: GPL-3.0-or-later
#include "ops_internal.h"
#include <QJsonDocument>

namespace nova::ops {
using namespace detail;
namespace {
struct Entry {
    OperationInfo info;
    Fn fn;
};
ParamInfo I(const char *k, const char *label, double lo, double hi, double v, const char *tip = "") {
    ParamInfo p;
    p.key = k, p.label = label, p.tooltip = tip, p.type = ParamType::Int;
    p.minimum = lo, p.maximum = hi, p.value = v;
    return p;
}
ParamInfo D(const char *k, const char *label, double lo, double hi, double v, double step = .1,
            const char *tip = "") {
    auto p = I(k, label, lo, hi, v, tip);
    p.type = ParamType::Double;
    p.step = step;
    return p;
}
ParamInfo B(const char *k, const char *label, bool v, const char *tip = "") {
    auto p = I(k, label, 0, 1, v ? 1 : 0, tip);
    p.type = ParamType::Bool;
    return p;
}
ParamInfo C(const char *k, const char *label, const char *color, const char *tip = "") {
    auto p = I(k, label, 0, 0, 0, tip);
    p.type = ParamType::Color;
    p.text = color;
    return p;
}
ParamInfo Ch(const char *k, const char *label, QStringList choices, const char *tip = "") {
    auto p = I(k, label, 0, 0, 0, tip);
    p.type = ParamType::Choice;
    p.text = choices.value(0);
    p.choices = std::move(choices);
    return p;
}
ParamInfo Pal(const char *k, const char *label) {
    auto p = I(k, label, 0, 0, 0, "Comma-separated colours; empty uses the active palette");
    p.type = ParamType::Palette;
    return p;
}
const QStringList Dithers = {"None", "Ordered (Bayer 4×4)", "Floyd–Steinberg"};
const QStringList Resamplers = {"Nearest neighbour", "Bilinear", "Bicubic"};
const QStringList Anchors = {"Top left", "Top",         "Top right", "Left",        "Centre",
                             "Right",    "Bottom left", "Bottom",    "Bottom right"};
QStringList withDefault(QStringList list, const QString &first) {
    list.removeAll(first);
    list.prepend(first);
    return list;
}
std::vector<ParamInfo> gen(std::vector<ParamInfo> extra, int size = 16) {
    std::vector<ParamInfo> p{I("width", "Width", 1, 4096, size), I("height", "Height", 1, 4096, size),
                             I("seed", "Seed", 0, 2147483647, 1, "Same seed + parameters = same pixels")};
    p.insert(p.end(), extra.begin(), extra.end());
    return p;
}
OperationInfo op(const char *id, const char *name, const char *category, const char *description,
                 std::vector<ParamInfo> params) {
    OperationInfo info;
    info.id = id, info.name = name, info.category = category, info.description = description;
    info.params = std::move(params);
    return info;
}
const std::vector<Entry> &entries() {
    static const std::vector<Entry> list = [] {
        std::vector<Entry> e;
        auto add = [&](OperationInfo info, Fn fn) -> OperationInfo & {
            e.push_back({std::move(info), fn});
            return e.back().info;
        };
        // ---------------------------------------------------- Adjust
        add(op("brightness_contrast", "Brightness / Contrast", "Adjust", "Linear brightness offset and contrast around mid-grey",
               {I("brightness", "Brightness", -100, 100, 0), I("contrast", "Contrast", -100, 100, 0)}),
            brightnessContrast);
        add(op("hue_saturation", "Hue / Saturation / Lightness", "Adjust", "HSL adjustment in encoded sRGB",
               {I("hue", "Hue shift (°)", -180, 180, 0), I("saturation", "Saturation", -100, 100, 0),
                I("lightness", "Lightness", -100, 100, 0)}),
            hueSaturation);
        add(op("exposure", "Exposure", "Adjust", "Multiplies linear-light intensity by 2^stops",
               {D("stops", "Stops", -5, 5, 0, .1)}),
            exposure);
        add(op("gamma", "Gamma", "Adjust", "Power curve on encoded channels",
               {D("gamma", "Gamma", .1, 5, 1, .05)}),
            gammaOp);
        add(op("temperature_tint", "Temperature / Tint", "Adjust", "Warm/cool and green/magenta balance in linear light",
               {I("temperature", "Temperature", -100, 100, 0), I("tint", "Tint (magenta +)", -100, 100, 0)}),
            temperatureTint);
        add(op("levels", "Levels", "Adjust", "Input/output range remap with midtone gamma",
               {I("input_black", "Input black", 0, 254, 0), I("input_white", "Input white", 1, 255, 255),
                D("midtone", "Midtone gamma", .1, 9.99, 1, .05), I("output_black", "Output black", 0, 255, 0),
                I("output_white", "Output white", 0, 255, 255),
                Ch("channel", "Channel", {"RGB", "Red", "Green", "Blue", "Alpha"})}),
            levels);
        add(op("curves", "Curves", "Adjust", "Monotone cubic curve through five control points",
               {I("black", "Output at 0", 0, 255, 0), I("shadows", "Output at 64", 0, 255, 64),
                I("midtones", "Output at 128", 0, 255, 128), I("highlights", "Output at 192", 0, 255, 192),
                I("white", "Output at 255", 0, 255, 255), Ch("channel", "Channel", {"RGB", "Red", "Green", "Blue"})}),
            curves);
        add(op("channels", "Channel Adjust", "Adjust", "Per-channel gain and channel swaps; alpha changes only when its gain differs from 100%",
               {I("red", "Red %", 0, 200, 100), I("green", "Green %", 0, 200, 100), I("blue", "Blue %", 0, 200, 100),
                I("alpha", "Alpha %", 0, 200, 100), Ch("swap", "Swap", {"None", "Red ↔ Blue", "Red ↔ Green", "Green ↔ Blue"})}),
            channels);
        add(op("replace_color", "Replace Colour", "Adjust", "Selective colour replacement within a tolerance",
               {C("from", "Source colour", "#FFFF0000"), C("to", "Target colour", "#FF0000FF"),
                I("tolerance", "Tolerance", 0, 255, 32), Ch("metric", "Colour distance", distanceNames()),
                B("preserve_shading", "Preserve shading offsets", true), B("include_alpha", "Match alpha", false)}),
            replaceColor);
        add(op("grayscale", "Grayscale / Desaturate", "Adjust", "Luminance, average or lightness grey with adjustable amount",
               {Ch("method", "Method", {"Luminance (Rec. 709)", "Average", "Lightness"}), I("amount", "Amount %", 0, 100, 100)}),
            grayscale);
        add(op("invert", "Invert", "Adjust", "Inverts colour channels; alpha only on request",
               {B("invert_alpha", "Invert alpha", false)}),
            invert);
        add(op("colorize", "Colorize / Recolor", "Adjust", "Applies a hue (and optionally saturation/lightness) to every pixel",
               {C("color", "Colour", "#FF50A0FF"), B("lock_saturation_lightness", "Lock saturation and lightness", true),
                I("amount", "Amount %", 0, 100, 100)}),
            colorize);
        add(op("posterize", "Posterize", "Adjust", "Uniform channel quantization", {I("levels", "Levels", 2, 64, 4)}),
            posterize);
        add(op("quantize", "Colour Quantization", "Adjust", "Median-cut palette reduction with optional dithering",
               {I("colors", "Colours", 2, 256, 16), Ch("dither", "Dithering", Dithers)}),
            quantize);
        add(op("palette_map", "Map to Palette", "Adjust", "Nearest-colour palette mapping with optional dithering",
               {Pal("palette", "Palette"), Ch("dither", "Dithering", Dithers),
                Ch("metric", "Colour distance", withDefault(distanceNames(), "Perceptual (redmean)"))}),
            paletteMap);
        add(op("alpha_threshold", "Alpha Threshold", "Adjust", "Snaps alpha to fully opaque or fully transparent",
               {I("threshold", "Threshold", 1, 255, 128)}),
            alphaThreshold);
        add(op("transparency_cleanup", "Transparency Cleanup", "Adjust", "Clears near-transparent pixels and zeroes hidden RGB",
               {I("min_alpha", "Clear alpha ≤", 0, 254, 0)}),
            transparencyCleanup);
        add(op("hsv_noise", "HSV Noise", "Adjust", "Random per-pixel hue/saturation/value variation (seeded)",
               {I("hue", "Max hue (°)", 0, 180, 5), I("saturation", "Max saturation %", 0, 100, 12),
                I("value", "Max value %", 0, 100, 12), I("seed", "Seed", 0, 2147483647, 1)}),
            hsvNoise);
        // ---------------------------------------------------- Fill
        add(op("fill", "Fill", "Fill", "Fills the target (or selection) with a colour",
               {C("color", "Colour", "#FF828DFF"), I("opacity", "Opacity", 0, 255, 255)}),
            fillColor);
        add(op("gradient", "Gradient", "Fill", "Linear, radial or reflected gradient with steps and dithering",
               {D("x0", "Start X", -8192, 8192, 0, 1), D("y0", "Start Y", -8192, 8192, 0, 1),
                D("x1", "End X", -8192, 8192, 16, 1), D("y1", "End Y", -8192, 8192, 16, 1),
                C("color_a", "Start colour", "#FF000000"), C("color_b", "End colour", "#FFFFFFFF"),
                Ch("type", "Type", {"Linear", "Radial", "Reflected"}), I("steps", "Steps (0 = smooth)", 0, 64, 0),
                Ch("dither", "Dithering", {"None", "Ordered (Bayer 4×4)"}), I("opacity", "Opacity", 0, 255, 255)}),
            gradient);
        // ---------------------------------------------------- Filters
        auto wrap = B("wrap", "Wrap edges (tileable)", false);
        add(op("box_blur", "Box Blur", "Filters", "Uniform separable blur", {I("radius", "Radius", 1, 32, 1), wrap}), boxBlur)
            .expensive = true;
        add(op("gaussian_blur", "Gaussian Blur", "Filters", "Separable Gaussian, alpha-weighted",
               {D("sigma", "Sigma", .3, 32, 1, .1), wrap}),
            gaussianBlur).expensive = true;
        add(op("sharpen", "Sharpen", "Filters", "4-neighbour Laplacian sharpening", {I("amount", "Amount %", 0, 400, 100), wrap}),
            sharpen);
        add(op("unsharp_mask", "Unsharp Mask", "Filters", "Sharpens differences from a Gaussian blur",
               {D("radius", "Radius", .3, 16, 1, .1), I("amount", "Amount %", 0, 500, 100), I("threshold", "Threshold", 0, 255, 0), wrap}),
            unsharpMask).expensive = true;
        add(op("median", "Median", "Filters", "Noise-reducing median of visible neighbours", {I("radius", "Radius", 1, 8, 1), wrap}),
            median).expensive = true;
        add(op("noise", "Add Noise", "Filters", "Seeded uniform or Gaussian noise",
               {I("amount", "Amount", 0, 255, 24), B("monochrome", "Monochrome", true),
                Ch("distribution", "Distribution", {"Uniform", "Gaussian"}), I("seed", "Seed", 0, 2147483647, 1)}),
            noise);
        add(op("pixelate", "Pixelate", "Filters", "Block averaging / sampling",
               {I("block", "Block size", 2, 64, 2), Ch("mode", "Mode", {"Average", "Centre sample", "Dominant"})}),
            pixelate);
        add(op("edge_detect", "Edge Detection", "Filters", "Sobel or Laplacian edge magnitude",
               {Ch("method", "Method", {"Sobel", "Laplacian"}), Ch("output", "Output", {"Grayscale magnitude", "Coloured edges"}),
                I("threshold", "Threshold", 0, 255, 0), wrap}),
            edgeDetect);
        add(op("emboss", "Emboss", "Filters", "Directional relief from luminance",
               {I("angle", "Angle (°)", 0, 359, 135), D("strength", "Strength", .1, 10, 1), Ch("blend", "Result", {"Grey relief", "Over original"}), wrap}),
            emboss);
        add(op("outline", "Outline", "Filters", "Adds an outline around (or inside) visible pixels",
               {C("color", "Colour", "#FF000000"), I("thickness", "Thickness", 1, 16, 1), Ch("position", "Position", {"Outside", "Inside"}),
                Ch("corners", "Corners", {"Square", "Rounded"}), I("alpha_threshold", "Alpha threshold", 1, 255, 1)}),
            outline);
        add(op("edge_enhance", "Edge Enhance", "Filters", "8-neighbour edge emphasis", {I("amount", "Amount %", 0, 400, 50), wrap}),
            edgeEnhance);
        add(op("normal_map", "Normal Map (estimate)", "Filters",
               "Estimates a tangent-space normal map from luminance, alpha or a heightmap layer. A single diffuse texture does not determine real geometry.",
               {D("strength", "Strength", .1, 20, 2), Ch("source", "Height source", {"Luminance", "Alpha bevel", "Heightmap layer"}),
                B("invert_y", "Invert Y (DirectX)", false), B("wrap", "Wrap edges (tileable)", true), D("blur", "Pre-blur", 0, 4, 0)}),
            normalMap);
        add(op("heightmap", "Heightmap (estimate)", "Filters", "Grayscale height estimate from luminance or alpha distance",
               {Ch("source", "Source", {"Luminance", "Alpha bevel"}), B("invert", "Invert", false),
                I("contrast", "Contrast", -100, 100, 0), D("blur", "Blur", 0, 8, 0)}),
            heightmap);
        add(op("seamless", "Seamless Tile Assist (heuristic)", "Filters",
               "Cross-fades edges toward a half-offset copy so opposite edges agree. Heuristic repair, not exact tiling.",
               {I("blend", "Blend width (px)", 1, 128, 4)}),
            seamless);
        // ---------------------------------------------------- Transform (layer)
        auto &f = add(op("flip", "Flip Layer", "Transform", "Mirror horizontally or vertically", {Ch("direction", "Direction", {"Horizontal", "Vertical"})}), flip);
        f.transformsContent = true;
        add(op("rotate", "Rotate Layer", "Transform", "Rotate about the centre; exact for quarter turns of square content",
               {D("angle", "Angle (° clockwise)", -360, 360, 0, 1), Ch("resampling", "Resampling", Resamplers)}),
            rotate).transformsContent = true;
        add(op("offset", "Offset", "Transform", "Translate content; wrap for tileable textures",
               {I("dx", "Offset X", -4096, 4096, 0), I("dy", "Offset Y", -4096, 4096, 0), B("wrap", "Wrap around", true)}),
            offset).transformsContent = true;
        add(op("scale_content", "Scale Content", "Transform", "Scale about the centre without changing canvas size",
               {I("scale_x", "Scale X %", 1, 1600, 100), I("scale_y", "Scale Y %", 1, 1600, 100), Ch("resampling", "Resampling", Resamplers)}),
            scaleContent).transformsContent = true;
        add(op("align_content", "Align Content", "Transform", "Moves visible pixels to an anchor (Centre = centre content)",
               {Ch("anchor", "Anchor", withDefault(Anchors, "Centre")), I("margin", "Margin", 0, 1024, 0)}),
            alignContent);
        // ---------------------------------------------------- Image (document-wide)
        auto doc = [&](OperationInfo info, Fn fn) {
            add(std::move(info), fn).changesSize = true;
        };
        doc(op("image_rotate", "Rotate Image", "Image", "Rotates every layer by a quarter or half turn",
               {Ch("turns", "Rotation", {"90° clockwise", "180°", "90° counter-clockwise"})}),
            imageRotate);
        doc(op("image_flip", "Flip Image", "Image", "Mirrors every layer", {Ch("direction", "Direction", {"Horizontal", "Vertical"})}),
            imageFlip);
        doc(op("resize_image", "Resize Image", "Image", "Resamples every layer (nearest neighbour keeps pixel art crisp)",
               {I("width", "Width", 1, 4096, 16), I("height", "Height", 1, 4096, 16), Ch("resampling", "Resampling", Resamplers)}),
            resizeImage);
        doc(op("resize_canvas", "Resize Canvas", "Image", "Changes canvas size; padding is transparent, wrapped or extended",
               {I("width", "Width", 1, 4096, 16), I("height", "Height", 1, 4096, 16),
                Ch("anchor", "Anchor", withDefault(Anchors, "Centre")),
                Ch("padding", "Padding", {"Transparent", "Repeat (wrap)", "Extend edge"})}),
            resizeCanvas);
        doc(op("crop", "Crop", "Image", "Crops every layer to a rectangle",
               {I("x", "X", 0, 4095, 0), I("y", "Y", 0, 4095, 0), I("width", "Width", 1, 4096, 16), I("height", "Height", 1, 4096, 16)}),
            crop);
        doc(op("trim", "Trim Transparent Borders", "Image", "Crops to visible pixels", {I("alpha_threshold", "Alpha ≤ counts as empty", 0, 254, 0)}),
            trim);
        // ---------------------------------------------------- Shading
        add(op("directional_shading", "Directional Shading", "Shading",
               "Deterministic light/shadow from an estimated or supplied height field, with discrete pixel-art steps",
               {I("light_direction", "Light direction (° from up, clockwise)", 0, 359, 315),
                I("elevation", "Light elevation (°)", 5, 90, 45), I("intensity", "Intensity %", 0, 200, 100),
                I("ambient", "Ambient %", 0, 100, 30), I("shadow", "Shadow strength %", 0, 100, 55),
                I("highlight", "Highlight strength %", 0, 100, 45), I("temperature", "Colour temperature", -100, 100, 20),
                C("shadow_tint", "Shadow tint", "#FF1E2346"), C("highlight_tint", "Highlight tint", "#FFFFF4D6"),
                I("steps", "Brightness steps (0 = smooth)", 0, 8, 2), B("dither", "Dithered steps", false),
                Ch("height_source", "Height source", {"Luminance", "Alpha bevel", "Heightmap layer", "Normal map layer"}),
                D("height_strength", "Height strength", .1, 10, 2), I("bevel", "Bevel / smoothing", 0, 16, 2),
                I("edge_highlight", "Edge highlight %", 0, 100, 30), I("edge_darkening", "Ambient edge darkening %", 0, 100, 25),
                I("noise", "Noise modulation %", 0, 100, 0), I("seed", "Seed", 0, 2147483647, 1),
                B("preserve_clusters", "Preserve pixel clusters", true),
                Ch("palette_mode", "Colours", {"Free", "Image colours", "Active palette"}),
                Ch("output", "Output", {"Shading layer", "Modify pixels"})}),
            directionalShading).expensive = true;
        // ---------------------------------------------------- Generate
        auto g = [&](OperationInfo info, Fn fn) { add(std::move(info), fn).generator = true; };
        g(op("gen_stone", "Stone", "Generate", "Tileable fBm stone with optional cracks",
             gen({C("base", "Base colour", "#FF7D7D7D"), I("scale", "Noise cells", 1, 16, 4), I("roughness", "Roughness", 5, 100, 55),
                  I("cracks", "Cracks", 0, 100, 25), I("steps", "Shades", 2, 8, 4), I("contrast", "Contrast %", 5, 100, 55)})),
          genStone);
        g(op("gen_bricks", "Bricks", "Generate", "Running-bond bricks; tileable when the size is a multiple of the brick period",
             gen({C("base", "Brick colour", "#FF9A4E3C"), C("mortar_color", "Mortar colour", "#FFB4ADA0"),
                  I("brick_width", "Brick width", 2, 64, 7), I("brick_height", "Brick height", 1, 64, 3), I("mortar", "Mortar", 1, 4, 1),
                  I("row_offset", "Row offset %", 0, 100, 50), I("variation", "Colour variation", 0, 100, 40),
                  I("noise", "Surface noise", 0, 100, 35), B("bevel", "Bevel", true)})),
          genBricks);
        g(op("gen_tiles", "Tiles", "Generate", "Square tiles with grout",
             gen({C("base", "Tile colour", "#FFC8C2B4"), C("grout_color", "Grout colour", "#FF5A5650"),
                  I("tile_size", "Tile size", 2, 64, 7), I("grout", "Grout", 1, 4, 1), I("variation", "Variation", 0, 100, 30),
                  B("bevel", "Bevel", true)})),
          genTiles);
        g(op("gen_wood", "Wood", "Generate", "Planks, log rings or bark",
             gen({Ch("style", "Style", {"Planks", "Log rings", "Bark"}), C("base", "Wood colour", "#FFA07843"),
                  I("plank_height", "Plank height", 2, 32, 4), I("grain", "Grain", 0, 100, 60), I("knots", "Knots", 0, 10, 1)})),
          genWood);
        g(op("gen_ore", "Ore", "Generate", "Stone with seeded ore clusters",
             gen({C("base", "Stone colour", "#FF7D7D7D"), C("ore_color", "Ore colour", "#FF3FC8B4"),
                  I("density", "Ore density", 1, 100, 30), I("cluster_size", "Cluster size", 1, 8, 3),
                  I("scale", "Noise cells", 1, 16, 4), I("roughness", "Roughness", 5, 100, 55), I("cracks", "Cracks", 0, 100, 10),
                  I("steps", "Shades", 2, 8, 4), I("contrast", "Contrast %", 5, 100, 45)})),
          genOre);
        g(op("gen_metal", "Metal", "Generate", "Brushed metal, riveted plate or diamond plate",
             gen({Ch("style", "Style", {"Brushed", "Plate with rivets", "Diamond plate"}), C("base", "Metal colour", "#FFB9BEC6"),
                  I("rivet_spacing", "Rivet spacing", 2, 64, 12)})),
          genMetal);
        g(op("gen_gui_panel", "GUI Panel", "Generate", "Bevelled interface panel",
             gen({Ch("style", "Style", {"Raised", "Inset", "Flat"}), C("base", "Panel colour", "#FFC6C6C6"),
                  I("border", "Bevel width", 1, 8, 2), I("corner_radius", "Corner cut", 0, 6, 2), B("outline", "Dark outline", true)},
                 32)),
          genPanel);
        g(op("gen_item", "Item Silhouette", "Generate", "Original procedural item shapes with ramp shading",
             gen({Ch("shape", "Shape", {"Gem", "Ingot", "Orb", "Rod", "Bottle", "Shard"}), C("base", "Colour", "#FF3FB4E6"),
                  B("shading", "Shading", true), B("outline", "Outline", true)})),
          genItem);
        g(op("gen_noise", "Seamless Noise", "Generate", "Periodic fBm, ridged or cellular noise (mathematically tileable)",
             gen({Ch("type", "Type", {"Value fBm", "Cellular", "Ridged"}), I("scale", "Cells", 1, 16, 4), I("octaves", "Octaves", 1, 6, 3),
                  I("roughness", "Roughness", 5, 100, 50), C("color_a", "Colour A", "#FF2A2F3C"), C("color_b", "Colour B", "#FFB8C2D6"),
                  I("steps", "Steps (0 = smooth)", 0, 16, 0)})),
          genNoise);
        for (auto &entry : e)
            if (entry.info.expensive || entry.info.category == "Shading")
                entry.info.workingCopies = 6;
        return e;
    }();
    return list;
}
} // namespace

QJsonObject Spec::toJson() const {
    return {{"operation", id}, {"params", params}, {"enabled", enabled}};
}
Spec Spec::fromJson(const QJsonObject &json) {
    return {json.value("operation").toString(), json.value("params").toObject(),
            json.value("enabled").toBool(true)};
}
const std::vector<OperationInfo> &registry() {
    static const std::vector<OperationInfo> list = [] {
        std::vector<OperationInfo> out;
        for (const auto &e : entries())
            out.push_back(e.info);
        return out;
    }();
    return list;
}
const OperationInfo *find(const QString &id) {
    for (const auto &info : registry())
        if (info.id == id)
            return &info;
    return nullptr;
}
static Fn function(const QString &id) {
    for (const auto &e : entries())
        if (e.info.id == id)
            return e.fn;
    return nullptr;
}
QStringList categories() {
    QStringList out;
    for (const auto &info : registry())
        if (!out.contains(info.category))
            out.append(info.category);
    return out;
}
QJsonObject defaults(const QString &id) {
    QJsonObject out;
    if (auto info = find(id))
        for (const auto &p : info->params) {
            switch (p.type) {
            case ParamType::Int: out[p.key] = int(p.value); break;
            case ParamType::Double: out[p.key] = p.value; break;
            case ParamType::Bool: out[p.key] = p.value != 0; break;
            default: out[p.key] = p.text; break;
            }
        }
    return out;
}
bool validate(Spec &spec, QString *error) {
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    const auto info = find(spec.id);
    if (!info)
        return fail("Unknown image operation: " + spec.id);
    QJsonObject out;
    for (auto it = spec.params.begin(); it != spec.params.end(); ++it) {
        bool known = false;
        for (const auto &p : info->params)
            known |= p.key == it.key();
        if (!known)
            return fail(QString("%1 has no parameter '%2'").arg(info->name, it.key()));
    }
    for (const auto &p : info->params) {
        const auto v = spec.params.contains(p.key) ? spec.params.value(p.key) : defaults(spec.id).value(p.key);
        const QString where = info->name + " · " + p.label;
        switch (p.type) {
        case ParamType::Int:
        case ParamType::Double: {
            if (!v.isDouble() || !std::isfinite(v.toDouble()))
                return fail(where + " must be a number");
            const double d = v.toDouble();
            if (p.type == ParamType::Int && d != std::floor(d))
                return fail(where + " must be a whole number");
            if (d < p.minimum || d > p.maximum)
                return fail(QString("%1 must be between %2 and %3").arg(where).arg(p.minimum).arg(p.maximum));
            out[p.key] = p.type == ParamType::Int ? QJsonValue(int(d)) : QJsonValue(d);
            break;
        }
        case ParamType::Bool:
            if (!v.isBool())
                return fail(where + " must be true or false");
            out[p.key] = v;
            break;
        case ParamType::Color: {
            bool ok = false;
            parseColor(v.toString(), &ok);
            if (!v.isString() || !ok)
                return fail(where + " must be a colour such as #FF336699");
            out[p.key] = v;
            break;
        }
        case ParamType::Choice:
            if (!p.choices.contains(v.toString()))
                return fail(where + " must be one of: " + p.choices.join(", "));
            out[p.key] = v;
            break;
        case ParamType::Palette:
            if (!v.isString() || v.toString().size() > 64 * 1024)
                return fail(where + " must be a comma-separated colour list");
            out[p.key] = v;
            break;
        }
    }
    if (spec.id == "levels" && out["input_white"].toInt() <= out["input_black"].toInt())
        return fail("Levels · Input white must exceed input black");
    spec.params = out;
    return true;
}
QSize outputSize(const Spec &spec, QSize input) {
    const auto &p = spec.params;
    if (spec.id == "resize_image" || spec.id == "resize_canvas" || spec.id == "crop")
        return {p["width"].toInt(), p["height"].toInt()};
    if (spec.id == "image_rotate" && p["turns"].toString() != "180°")
        return input.transposed();
    if (auto info = find(spec.id); info && info->generator)
        return {p["width"].toInt(), p["height"].toInt()};
    return input;
}
qint64 estimateBytes(const Spec &spec, QSize input) {
    const auto info = find(spec.id);
    const auto out = outputSize(spec, input);
    const qint64 pixels = std::max(qint64(input.width()) * input.height(), qint64(out.width()) * out.height());
    return pixels * 4 * (info ? info->workingCopies : 3);
}
Result run(Spec spec, const QImage &input, const Context &ctx) {
    Result result;
    if (!validate(spec, &result.error))
        return result;
    const auto info = find(spec.id);
    const auto fn = function(spec.id);
    if (!info->generator && (input.isNull() || input.width() > 4096 || input.height() > 4096)) {
        result.error = "Image operations need a valid image up to 4096 pixels per side";
        return result;
    }
    if (estimateBytes(spec, input.size()) > ctx.memoryLimit) {
        result.error = QString("%1 would need about %2 MiB, above the %3 MiB operation budget")
                           .arg(info->name)
                           .arg(estimateBytes(spec, input.size()) >> 20)
                           .arg(ctx.memoryLimit >> 20);
        return result;
    }
    const QImage src = info->generator ? QImage(1, 1, QImage::Format_RGBA8888)
                                       : input.convertToFormat(QImage::Format_RGBA8888);
    const Params params{spec.params};
    const bool masked = ctx.mask && !ctx.mask->isNull() && ctx.mask->size() == src.size() &&
                        !info->changesSize && !info->generator;
    if (masked && maskEmpty(*ctx.mask)) {
        result.canceled = ctx.canceled();
        if (!result.canceled)
            result.image = src;
        return result;
    }
    QImage out;
    if (masked && info->transformsContent) {
        // Lift selected pixels, transform them alone, then composite over the hole.
        QImage content = src.copy(), hole = src.copy();
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x) {
                const int m = ctx.mask->constScanLine(y)[x];
                const QRgb c = px(src, x, y);
                setPx(content, x, y, qRgba(qRed(c), qGreen(c), qBlue(c), qAlpha(c) * m / 255));
                setPx(hole, x, y, qRgba(qRed(c), qGreen(c), qBlue(c), qAlpha(c) * (255 - m) / 255));
            }
        // Transform about the selection centre, not the canvas centre.
        const QRect bounds = maskBounds(*ctx.mask);
        QImage moved;
        if (spec.id == "flip") {
            // Exact mirror inside the selection bounds.
            moved = blank(src.size());
            const bool vertical = params.s("direction") == "Vertical";
            for (int y = bounds.top(); y <= bounds.bottom(); ++y)
                for (int x = bounds.left(); x <= bounds.right(); ++x)
                    setPx(moved, vertical ? x : bounds.left() + bounds.right() - x,
                          vertical ? bounds.top() + bounds.bottom() - y : y, px(content, x, y));
        } else {
            const QPoint shift = QRect(QPoint(), src.size()).center() - bounds.center();
            QImage centred = spec.id == "offset" ? content : wrapOffset(content, shift.x(), shift.y());
            Context inner = ctx;
            inner.mask = nullptr;
            moved = fn(centred, params, inner, &result.error);
            if (moved.isNull()) {
                result.canceled = ctx.canceled();
                return result;
            }
            if (spec.id != "offset")
                moved = wrapOffset(moved, -shift.x(), -shift.y());
        }
        out = hole;
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x)
                setPx(out, x, y, blend(px(hole, x, y), px(moved, x, y), 255));
    } else {
        Context inner = ctx;
        out = fn(src, params, inner, &result.error);
        if (!out.isNull() && masked)
            out = applyMask(src, out, ctx.mask);
    }
    if (ctx.canceled()) {
        result.canceled = true;
        result.image = {};
        return result;
    }
    if (out.isNull() && result.error.isEmpty())
        result.error = info->name + " failed";
    result.image = out;
    return result;
}
Result runPipeline(const std::vector<Spec> &steps, const QImage &source, const Context &ctx) {
    Result r;
    r.image = source.convertToFormat(QImage::Format_RGBA8888);
    for (const auto &step : steps) {
        if (!step.enabled)
            continue;
        Context inner = ctx;
        if (!inner.mask || inner.mask->size() != r.image.size())
            inner.mask = nullptr;
        auto next = run(step, r.image, inner);
        if (!next.ok()) {
            next.image = {};
            if (!next.canceled && !next.error.isEmpty())
                next.error = (find(step.id) ? find(step.id)->name : step.id) + ": " + next.error;
            return next;
        }
        r.image = next.image;
    }
    return r;
}
const std::vector<Preset> &builtInPresets() {
    static const std::vector<Preset> list = [] {
        std::vector<Preset> out;
        auto shading = [&](const char *name, QJsonObject changes) {
            auto params = defaults("directional_shading");
            for (auto it = changes.begin(); it != changes.end(); ++it)
                params[it.key()] = it.value();
            out.push_back({name, "directional_shading", params});
        };
        // Processing presets only; they do not infer materials from arbitrary images.
        shading("Stone", {{"steps", 3}, {"height_strength", 2.5}, {"edge_darkening", 35}, {"noise", 15}, {"temperature", 10}});
        shading("Metal", {{"steps", 4}, {"highlight", 75}, {"shadow", 60}, {"height_strength", 3.0}, {"temperature", -20},
                          {"highlight_tint", "#FFFFFFFF"}, {"edge_highlight", 55}});
        shading("Wood", {{"steps", 3}, {"temperature", 45}, {"shadow_tint", "#FF3A1E14"}, {"height_strength", 1.5}, {"noise", 10}});
        shading("Fabric", {{"steps", 2}, {"intensity", 60}, {"edge_highlight", 10}, {"edge_darkening", 40}, {"height_strength", 1.0}});
        shading("Glass", {{"steps", 2}, {"highlight", 85}, {"shadow", 25}, {"height_source", "Alpha bevel"}, {"bevel", 2},
                          {"highlight_tint", "#FFFFFFFF"}, {"edge_highlight", 70}});
        shading("Organic", {{"steps", 3}, {"dither", true}, {"temperature", 25}, {"noise", 20}, {"height_strength", 1.8}});
        shading("Emissive", {{"steps", 2}, {"shadow", 10}, {"ambient", 70}, {"highlight", 60}, {"highlight_tint", "#FFFFFFC8"},
                             {"edge_darkening", 0}});
        auto adjust = [&](const char *name, const char *operation, QJsonObject params) {
            auto all = defaults(operation);
            for (auto it = params.begin(); it != params.end(); ++it)
                all[it.key()] = it.value();
            out.push_back({name, operation, all});
        };
        adjust("Cool shift", "hue_saturation", {{"hue", -12}, {"saturation", -10}});
        adjust("Warm shift", "hue_saturation", {{"hue", 10}, {"saturation", 5}});
        adjust("Weathered", "hue_saturation", {{"saturation", -35}, {"lightness", -5}});
        adjust("Punchy", "brightness_contrast", {{"contrast", 25}});
        // Original seeded material templates; the shared generators own all pixels.
        adjust("C5 · Basalt 16px", "gen_stone", {{"base","#FF414955"},{"scale",4},{"cracks",35},{"seed",503}});
        adjust("C5 · Copper bricks 16px", "gen_bricks", {{"base","#FFB06D47"},{"brick_width",7},{"brick_height",3},{"mortar",1},{"seed",505}});
        adjust("C5 · Teal ceramic 16px", "gen_tiles", {{"base","#FF428D90"},{"tile_size",7},{"grout",1},{"seed",507}});
        adjust("C5 · Golden planks 16px", "gen_wood", {{"base","#FFBB904C"},{"plank_height",4},{"seed",509}});
        return out;
    }();
    return list;
}
} // namespace nova::ops
