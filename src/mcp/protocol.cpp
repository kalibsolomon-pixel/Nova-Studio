// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.h"
#include "build_identity.h"
#include "guides.h"
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringConverter>
#include <cmath>

namespace nova::mcp {
namespace {
constexpr auto Version = "2025-11-25";

QJsonObject reply(const QJsonValue &id, const QJsonObject &result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
QJsonObject stringField(int max, const QString &pattern = {}) {
    QJsonObject field{{"type", "string"}, {"maxLength", max}};
    if (!pattern.isEmpty())
        field["pattern"] = pattern;
    return field;
}
QJsonObject described(QJsonObject field, const QString &description) {
    field["description"] = description;
    return field;
}
QJsonObject integerField(int min, int max) {
    return {{"type", "integer"}, {"minimum", min}, {"maximum", max}};
}
QJsonObject objectSchema(const QJsonObject &properties, const QJsonArray &required) {
    return {{"type", "object"}, {"properties", properties}, {"required", required},
            {"additionalProperties", false}};
}
QJsonObject outputSchema(const QString &name) {
    QJsonObject fields;
    QJsonArray required;
    auto add = [&](const char *key, QJsonValue type) {
        fields[key] = QJsonObject{{"type", type}};
        required.append(key);
    };
    if (name == "nova.project.info") {
        for (auto key : {"projectId", "generation", "name", "minecraftVersion"})
            add(key, "string");
        add("assetCount", "integer");
        add("indexRevision", "string");
        add("namespaces", "array");
        add("validationAvailable", "boolean");
        for (auto key : {"packFormat", "minFormat", "maxFormat"})
            add(key, QJsonArray{"integer", "array", "null"});
        add("styleNotes", "object");
        add("unsavedCount", "integer");
    } else if (name == "nova.guide.list") {
        for (auto key : {"guideVersions", "novaVersions", "topics"})
            add(key, "array");
        add("uriTemplate", "string");
        add("index", "string");
    } else if (name == "nova.guide.get") {
        for (auto key : {"uri", "topic", "version", "title", "mimeType", "text"})
            add(key, "string");
    } else if (name == "nova.drafts.list") {
        add("projectId", "string");
        add("generation", "string");
        add("unsavedCount", "integer");
        add("hiddenCount", "integer");
        add("drafts", "array");
    } else if (name == "nova.drafts.adopt") {
        for (auto key : {"documentId", "assetId", "path", "type", "revision", "draftRevision"})
            add(key, "string");
        for (auto key : {"dirty", "adopted", "undoAvailable"})
            add(key, "boolean");
    } else if (name == "nova.drafts.save" || name == "nova.drafts.save_all") {
        add("projectId", "string");
        add("status", "string");
        add("saved", "array");
        add("failed", "array");
        add("unsavedCount", "integer");
    } else if (name == "nova.assets.list") {
        add("generation", "string");
        add("assets", "array");
        add("nextOffset", QJsonArray{"integer", "null"});
    } else if (name == "nova.assets.inspect_batch" || name == "nova.workflow.prepare") {
        add("generation", "string");
        add("projectId", "string");
        add("results", "array");
        if (name == "nova.workflow.prepare") {
            add("permission", "string");
            add("constraints", "object");
        }
    } else if (name == "nova.assets.context") {
        add("generation", "string");
        add("projectId", "string");
        add("assets", "array");
        add("references", "array");
        add("warnings", "array");
        add("truncated", "boolean");
    } else if (name == "nova.edit.preview") {
        for (auto key : {"assetId", "revision", "permission"})
            add(key, "string");
        add("operations", "array");
        add("undoAvailable", "boolean");
        add("validationRequired", "boolean");
    } else if (name == "nova.pack.validate" || name == "nova.workflow.validate_changes") {
        if (name == "nova.workflow.validate_changes") {
            add("projectId", "string");
            add("revisions", "array");
            add("validationSources", "object");
        }
        if (name == "nova.pack.validate") {
            add("drafts", "object");
            add("minecraftVersion", "string");
            add("sources", "object");
            add("limitations", "array");
        }
        add("generation", "string");
        add("diagnostics", "array");
        add("counts", "object");
        for (auto key : {"files", "inspected", "milliseconds"})
            add(key, "integer");
        add("truncated", "boolean");
    } else if (name == "nova.vanilla.list" || name == "nova.vanilla.texture") {
        for (auto key : {"projectId", "generation", "version", "identity", "selection", "cached", "projectVersion"})
            add(key, "string");
        add("matchesProjectVersion", "boolean");
        add("provenance", "object");
        add("verification", "object");
        if (name == "nova.vanilla.list") {
            add("textures", "array");
            add("matched", "integer");
            add("entryFiles", "integer");
            add("nextOffset", QJsonArray{"integer", "null"});
        } else {
            for (auto key : {"path", "sha256", "origin", "mimeType"})
                add(key, "string");
            add("bytes", "integer");
            add("width", QJsonArray{"integer", "null"});
            add("height", QJsonArray{"integer", "null"});
            add("imageIncluded", "boolean");
            add("projectOverride", "boolean");
            add("limits", "object");
            add("animationMetadata", QJsonArray{"object", "null"});
        }
    } else if (name == "nova.asset.relationships") {
        for (auto key : {"generation", "projectId", "path", "kind", "minecraftVersion", "versionSource"})
            add(key, "string");
        add("found", "boolean");
        add("providers", "array");
        add("relationships", "array");
        add("detail", "object");
        add("sources", "object");
        add("limitations", "array");
        add("truncated", "boolean");
    } else {
        for (auto key : {"assetId", "path", "type", "format", "category", "namespace"})
            add(key, "string");
        add("bytes", "integer");
        add("metadata", "object");
        add("warnings", "array");
    }
    return objectSchema(fields, required);
}
QJsonObject arrayField(const QJsonObject &item, int min, int max) {
    return {{"type", "array"}, {"items", item}, {"minItems", min}, {"maxItems", max}};
}
QJsonObject commandSchema(const QString &name, QJsonObject fields) {
    fields["command"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{name}}};
    return objectSchema(fields, QJsonArray::fromStringList(fields.keys()));
}
QJsonObject editOutputSchema(bool capabilities) {
    QJsonObject fields;
    auto add = [&](const QString &name, const char *type) { fields[name] = QJsonObject{{"type", type}}; };
    add("revision", "string");
    for (const auto key : {"dirty", "undoAvailable", "redoAvailable"})
        add(key, "boolean");
    const auto common = QJsonArray::fromStringList(fields.keys());
    for (const auto key : {"width", "height", "elementCount", "frameCount", "playbackCount", "defaultTicks"})
        add(key, "integer");
    for (const auto key : {"activeFrame", "activeLayer"})
        add(key, "string");
    for (const auto key : {"layers", "frames"})
        add(key, "array");
    add("animated", "boolean");
    add("interpolate", "boolean");
    if (capabilities) {
        const auto document = objectSchema(fields, common);
        fields = {};
        for (const auto key : {"projectId", "generation", "permission"})
            add(key, "string");
        for (const auto key : {"schemaVersion", "maxOperations", "maxDimension", "maxPixelBytes"})
            add(key, "integer");
        add("assetTypes", "array");
        add("tools", "array");
        const auto required = QJsonArray::fromStringList(fields.keys());
        fields["document"] = document;
        return objectSchema(fields, required);
    }
    add("changeSummary", "object");
    add("transactionId", "string");
    add("appliedOperations", "integer");
    add("changed", "boolean");
    add("warnings", "array");
    auto required = common;
    for (const auto key : {"transactionId", "appliedOperations", "changed", "warnings"})
        required.append(key);
    return objectSchema(fields, required);
}
void addEditingTools(QJsonArray &list, const QJsonObject &token) {
    const auto uuid = stringField(36, "^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$");
    const auto color = stringField(7, "^#[A-Fa-f0-9]{6}$");
    const auto name = stringField(128, "^.*\\S.*$");
    const auto coord = integerField(0, 255);
    const auto boolean = QJsonObject{{"type", "boolean"}};
    const QJsonArray texture{
        commandSchema("fill_rect", {{"layer", uuid}, {"x", coord}, {"y", coord},
            {"width", integerField(1, 256)}, {"height", integerField(1, 256)},
            {"color", color}, {"opacity", integerField(1, 255)}}),
        commandSchema("stroke", {{"layer", uuid}, {"points", arrayField(arrayField(coord, 2, 2), 1, 16)},
                                 {"color", color}, {"size", integerField(1, 8)}, {"opacity", integerField(1, 255)}}),
        commandSchema("fill", {{"layer", uuid}, {"x", coord}, {"y", coord}, {"color", color},
                               {"tolerance", integerField(0, 255)}, {"contiguous", boolean},
                               {"opacity", integerField(1, 255)}})};
    const QJsonArray layers{
        commandSchema("layer_add", {{"layer", uuid}, {"name", name}}),
        commandSchema("layer_duplicate", {{"layer", uuid}}),
        commandSchema("layer_rename", {{"layer", uuid}, {"name", name}}),
        commandSchema("layer_move", {{"layer", uuid}, {"value", integerField(0, 63)}}),
        commandSchema("layer_visibility", {{"layer", uuid}, {"value", integerField(0, 1)}}),
        commandSchema("layer_opacity", {{"layer", uuid}, {"value", integerField(0, 255)}}),
        commandSchema("layer_remove", {{"layer", uuid}})};
    const QJsonArray animation{
        commandSchema("frame_add", {{"frame", uuid}}),
        commandSchema("frame_duplicate", {{"frame", uuid}}),
        commandSchema("frame_remove", {{"frame", uuid}}),
        commandSchema("frame_move", {{"frame", uuid}, {"value", integerField(0, 1023)}}),
        commandSchema("animation_timing", {{"ticks", integerField(1, 32767)}, {"interpolate", boolean}}),
        commandSchema("animation_duration", {{"entry", integerField(0, 16383)}, {"ticks", integerField(0, 32767)}})};
    const auto element = integerField(0, 4095);
    const auto face = QJsonObject{{"type", "string"},
        {"enum", QJsonArray{"up", "down", "north", "south", "east", "west"}}};
    const auto ref = stringField(256, "^[#a-z0-9_.:/-]+$");
    auto number = [](int minimum, int maximum) {
        return QJsonObject{{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
    };
    const auto rotation = QJsonObject{{"oneOf", QJsonArray{
        objectSchema({{"enabled", QJsonObject{{"enum", QJsonArray{false}}}}}, {"enabled"}),
        objectSchema({{"enabled", QJsonObject{{"enum", QJsonArray{true}}}},
                      {"origin", arrayField(number(-10000, 10000), 3, 3)},
                      {"angles", arrayField(number(-36000, 36000), 3, 3)}, {"rescale", boolean}},
                     {"enabled", "origin", "angles", "rescale"})}}};
    const auto bounds = arrayField(number(-16, 32), 3, 3);
    const QJsonArray model{
        commandSchema("model_add", {{"texture", ref}}),
        commandSchema("model_remove", {{"element", element}}),
        commandSchema("model_transform", {{"element", element}, {"from", bounds}, {"to", bounds},
                                          {"rotation", rotation}}),
        commandSchema("model_face", {{"element", element}, {"face", face}, {"texture", ref},
            {"tint", integerField(-1, 255)}, {"cullface", QJsonObject{{"type", "string"},
                {"enum", QJsonArray{"", "up", "down", "north", "south", "east", "west"}}}}}),
        commandSchema("model_label", {{"element", element}, {"name", name}}),
        commandSchema("model_texture", {{"alias", stringField(128, "^[a-z0-9_.-]+$")}, {"texture", ref}})};
    const auto uvCoord = QJsonObject{{"type", "number"}, {"minimum", -4096}, {"maximum", 4096}};
    const QJsonArray uv{
        commandSchema("uv_set", {{"element", element}, {"face", face},
                                 {"uv", arrayField(uvCoord, 4, 4)},
                                 {"rotation", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{0, 90, 180, 270}}}}}),
        commandSchema("uv_mirror_u", {{"element", element}, {"face", face}}),
        commandSchema("uv_mirror_v", {{"element", element}, {"face", face}}),
        commandSchema("uv_rotate", {{"element", element}, {"face", face}})};
    auto add = [&](const QString &tool, const QString &description, const QJsonObject &properties,
                   const QJsonArray &required, bool readOnly = false) {
        list.append(QJsonObject{{"name", tool}, {"description", description},
            {"inputSchema", objectSchema(properties, required)},
            {"outputSchema", editOutputSchema(readOnly)},
            {"annotations", QJsonObject{{"readOnlyHint", readOnly}, {"openWorldHint", false}}}});
    };
    add("nova.edit.capabilities", "Describe permissions and optionally obtain an open document revision.",
        {{"generation", token}, {"assetId", token}}, {"generation"}, true);
    const QJsonObject target{{"generation", token}, {"projectId", token}, {"assetId", token},
                             {"expectedRevision", token}};
    const auto required = QJsonArray::fromStringList(target.keys());
    auto single = [&](const QString &tool, const QJsonArray &variants) {
        auto fields = target;
        fields["operation"] = QJsonObject{{"oneOf", variants}};
        auto keys = required;
        keys.append("operation");
        add(tool, "Apply one native in-memory edit (schema v1).", fields, keys);
    };
    single("nova.texture.edit", texture);
    single("nova.layer.edit", layers);
    single("nova.animation.edit", animation);
    single("nova.model.edit", model);
    single("nova.uv.edit", uv);
    QJsonArray commands;
    for (const auto &group : {texture, layers, animation, model, uv})
        for (const auto &command : group)
            commands.append(command);
    auto batch = target;
    batch["operations"] = arrayField(QJsonObject{{"oneOf", commands}}, 1, 8);
    auto stage = batch;
    stage.remove("generation");
    stage.remove("projectId");
    const QJsonObject workflow{{"generation", token}, {"projectId", token},
        {"stages", arrayField(objectSchema(stage, QJsonArray::fromStringList(stage.keys())), 2, 4)}};
    for (const auto &tool : {"nova.workflow.plan", "nova.workflow.execute"}) {
        const bool plan = QString(tool).endsWith(".plan");
        list.append(QJsonObject{{"name", tool},
            {"description", plan ? "Preflight ordered batches on distinct open documents." :
                "Apply ordered batches on distinct open documents. Earlier commits survive later failures."},
            {"inputSchema", objectSchema(workflow, {"generation", "projectId", "stages"})},
            {"annotations", QJsonObject{{"readOnlyHint", plan}, {"openWorldHint", false}}}});
    }
    const auto choice = [](QJsonArray values) {
        return QJsonObject{{"type", "string"}, {"enum", values}};
    };
    const QJsonObject raster{{"width", integerField(1, 256)}, {"height", integerField(1, 256)},
        {"pixelFormat", choice({"rgba8888"})}, {"layerName", name},
        {"fill", stringField(11, "^(transparent|#[a-fA-F0-9]{6})$")}};
    auto animationProperties = raster;
    animationProperties["frames"] = integerField(1, 15);
    animationProperties["ticks"] = integerField(1, 32767);
    animationProperties["interpolate"] = boolean;
    const auto resource = stringField(200, "^([a-z0-9_.-]+:)?[a-z0-9_./-]+$");
    const auto modelCreation = objectSchema({{"command", choice({"model"})},
        {"parent", resource}, {"cubeTexture", resource}}, {"command"});
    const QJsonObject creation{{"projectId", token}, {"generation", token}, {"indexRevision", token},
        {"namespace", stringField(64, "^[a-z0-9_.-]+$")},
        {"path", stringField(200, "^[a-z0-9_./-]+$")}, {"conflictPolicy", choice({"reject"})},
        {"parameters", QJsonObject{{"oneOf", QJsonArray{
            commandSchema("texture", raster), commandSchema("animation", animationProperties), modelCreation}}}}};
    for (const auto &tool : {"nova.asset.create_preview", "nova.asset.create"})
        list.append(QJsonObject{{"name", tool},
            {"description", "Preflight or create a managed unsaved texture, animation, or model document."},
            {"inputSchema", objectSchema(creation, QJsonArray::fromStringList(creation.keys()))},
            {"annotations", QJsonObject{{"readOnlyHint", QString(tool).endsWith("_preview")},
                                        {"openWorldHint", false}}}});
    auto image = target;
    image["frameId"] = uuid;
    image["view"] = choice({"model", "uv"});
    image["faceIndex"] = integerField(0, 1535);
    list.append(QJsonObject{{"name", "nova.asset.preview_image"},
        {"description", "Return a bounded PNG of a live texture, animation frame, model viewport, or model face UV."},
        {"inputSchema", objectSchema(image, required)},
        {"annotations", QJsonObject{{"readOnlyHint", true}, {"openWorldHint", false}}}});
    auto batchKeys = required;
    batchKeys.append("operations");
    add("nova.edit.batch", "Apply up to eight compatible operations as one history step (schema v1).",
        batch, batchKeys);
    list.append(QJsonObject{{"name", "nova.edit.preview"},
        {"description", "Preflight a native batch against an isolated draft."},
        {"inputSchema", objectSchema(batch, batchKeys)},
        {"outputSchema", outputSchema("nova.edit.preview")},
        {"annotations", QJsonObject{{"readOnlyHint", true}, {"openWorldHint", false}}}});
    for (const auto &name : {"undo", "redo"})
        add(QString("nova.edit.") + name, "Move only through this connection's contiguous edits.",
            target, required);
}
// MCP Phase 2: compact texture edits. Each call is one native in-memory edit of an open texture
// (one history step), committed like nova.texture.edit; the runtime checks that need the canvas
// (region bounds, row widths, palette indices, stop order) report the same JSON pointers.
void addCompactTextureTools(QJsonArray &list, const QJsonObject &token) {
    const auto uuid = described(stringField(36, "^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$"),
                                "A layer id from the document state; defaults to the active layer.");
    const auto color = described(stringField(9, "^#[A-Fa-f0-9]{6}([A-Fa-f0-9]{2})?$"),
                                 "A colour #RRGGBB (opaque) or #RRGGBBAA (straight alpha, CSS order).");
    const auto choice = [](const QJsonArray &values, const QString &description) {
        return QJsonObject{{"type", "string"}, {"enum", values}, {"description", description}};
    };
    const QJsonObject rect{{"x", integerField(0, 255)}, {"y", integerField(0, 255)},
                           {"width", integerField(1, 256)}, {"height", integerField(1, 256)}};
    const auto region = described(objectSchema(rect, {"x", "y", "width", "height"}),
                                  "A rectangle inside the canvas; x + width and y + height may not exceed its size.");
    auto output = editOutputSchema(false);
    auto fields = output["properties"].toObject();
    auto required = output["required"].toArray();
    for (const auto key : {"documentId", "path", "assetId", "layer"}) {
        fields[key] = QJsonObject{{"type", "string"}};
        required.append(key);
    }
    fields["region"] = objectSchema({{"x", QJsonObject{{"type", "integer"}}}, {"y", QJsonObject{{"type", "integer"}}},
                                     {"width", QJsonObject{{"type", "integer"}}}, {"height", QJsonObject{{"type", "integer"}}}},
                                    {"x", "y", "width", "height"});
    fields["changedBounds"] = QJsonObject{{"oneOf", QJsonArray{fields["region"], QJsonObject{{"type", "null"}}}}};
    fields["pixelsChanged"] = QJsonObject{{"type", "integer"}};
    for (const auto key : {"region", "changedBounds", "pixelsChanged"})
        required.append(key);
    output = objectSchema(fields, required);
    auto add = [&](const QString &tool, const QString &description, QJsonObject properties, QJsonArray keys) {
        for (const auto key : {"generation", "projectId", "assetId", "expectedRevision"}) {
            properties[key] = token;
            keys.append(key);
        }
        properties["layer"] = uuid;
        list.append(QJsonObject{{"name", tool}, {"description", description},
            {"inputSchema", objectSchema(properties, keys)}, {"outputSchema", output},
            {"annotations", QJsonObject{{"readOnlyHint", false}, {"openWorldHint", false}}}});
    };
    add("nova.texture.put_pixels",
        "Write a rectangle of pixels from a palette in one edit. rows[r] has one character per pixel: a palette "
        "index (0-9 = 0..9, a-z = 10..35, A-Z = 36..61) or '.' to keep that pixel. Replaces RGBA exactly unless "
        "mode is \"over\". Edits the open document in memory; it is not saved.",
        {{"x", rect["x"]}, {"y", rect["y"]}, {"width", rect["width"]}, {"height", rect["height"]},
         {"palette", described(arrayField(color, 1, 62), "Up to 62 colours #RRGGBB or #RRGGBBAA.")},
         {"rows", described(arrayField(stringField(256, "^[0-9A-Za-z.]+$"), 1, 256),
                            "One string per pixel row, top to bottom; height rows of width characters.")},
         {"mode", choice({"replace", "over"}, "replace (default): set each pixel to the palette RGBA exactly; over: "
                                              "composite the palette colour over the pixel.")}},
        {"x", "y", "width", "height", "palette", "rows"});
    add("nova.texture.palette_remap",
        "Replace colours in one edit; unmatched pixels are unchanged and each pixel is matched against its original "
        "colour (the first matching mapping wins). #RRGGBB keeps each pixel's alpha. Edits the open document in "
        "memory; it is not saved.",
        {{"mappings", described(arrayField(objectSchema({{"from", color}, {"to", color}}, {"from", "to"}), 1, 64),
                                "from #RRGGBB matches visible pixels of that RGB at any alpha; from #RRGGBBAA also "
                                "matches alpha (fully transparent pixels compare as #00000000). to #RRGGBB keeps the "
                                "pixel's alpha; to #RRGGBBAA sets it.")},
         {"tolerance", described(integerField(0, 255), "Largest allowed per-channel difference, 0 (default) for exact.")},
         {"region", region}},
        {"mappings"});
    add("nova.texture.gradient_map",
        "Recolour pixels by luma through a gradient in one edit. Luma is (2126 R + 7152 G + 722 B) / 10000 on encoded "
        "8-bit sRGB, 0-255. Fully transparent pixels are unchanged. Edits the open document in memory; it is not "
        "saved.",
        {{"stops", described(arrayField(objectSchema({{"position", integerField(0, 255)}, {"color", color}},
                                                     {"position", "color"}), 2, 16),
                             "2-16 stops with strictly increasing luma positions; luma outside them takes the end colour.")},
         {"interpolation", choice({"linear", "step"}, "linear (default): rounded integer interpolation of encoded "
                                                      "channels between stops; step: the colour of the last stop at or "
                                                      "below the luma.")},
         {"alpha", choice({"preserve", "multiply"}, "preserve (default): keep each pixel's alpha, stops are #RRGGBB; "
                                                    "multiply: pixel alpha * stop alpha / 255.")},
         {"source", choice({"luma709"}, "The source value; only luma709 is supported.")}, {"region", region}},
        {"stops"});
    add("nova.texture.erase",
        "Make a region of the active layer transparent in one edit, clipped by the selection, like Nova's eraser: "
        "alpha * (255 - coverage) / 255; pixels reaching zero alpha are stored as #00000000. Never deletes files or "
        "layers. Edits the open document in memory; it is not saved.",
        {{"region", region},
         {"strength", described(integerField(1, 255), "Eraser coverage, 255 (default) for fully transparent.")}},
        {"region"});
}
const QJsonArray &tools() {
    static const auto definitions = [] {
        QJsonArray list;
        auto add = [&](const QString &name, const QString &description, QJsonObject fields,
                       QJsonArray required) {
            list.append(QJsonObject{{"name", name}, {"description", description},
                {"inputSchema", objectSchema(fields, required)},
                {"outputSchema", outputSchema(name)},
                {"annotations", QJsonObject{{"readOnlyHint", true}, {"openWorldHint", false}}}});
        };
        const auto generation = stringField(64, "^[a-f0-9]{64}$");
        const auto asset = stringField(64, "^[a-f0-9]{64}$");
        add("nova.project.info", "Describe the explicitly authorized active resource pack.", {}, {});
        add("nova.assets.list", "Page through indexed resource pack assets.",
            {{"generation", generation},
             {"category", QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(categories())}}},
             {"namespace", stringField(64, "^[a-z0-9_.-]+$")}, {"search", stringField(128)},
             {"offset", integerField(0, 60000)}, {"limit", integerField(1, 100)}}, {"generation"});
        for (const QString name : {"asset", "texture", "animation", "model"})
            add("nova." + name + ".inspect", "Inspect authorized " + name + " metadata.",
                {{"generation", generation}, {"assetId", asset}}, {"generation", "assetId"});
        add("nova.pack.validate", "Run read-only validation of saved files and open unsaved drafts. References resolve "
            "through the project, the resource stack the user configured, and Nova's verified vanilla texture cache "
            "(textures only); each finding has a rule, source, evidence and next step. Never downloads.",
            {{"generation", generation}, {"assetId", asset}, {"limit", integerField(1, 200)}},
            {"generation"});
        QJsonObject targets{{"generation", generation}, {"projectId", generation},
                            {"assetIds", arrayField(asset, 1, 8)}};
        const auto targetKeys = QJsonArray::fromStringList(targets.keys());
        auto choice = [](const QJsonArray &values) {
            return QJsonObject{{"type", "string"}, {"enum", values}};
        };
        auto context = targets;
        context["depth"] = integerField(0, 3);
        context["relationships"] = arrayField(choice({"parent", "texture", "animation", "model", "alias"}), 1, 5);
        add("nova.assets.context", "Traverse direct and reverse resource-pack relationships.", context, targetKeys);
        auto batch = targets;
        batch["types"] = arrayField(choice({"texture", "animation", "model", "resource"}), 1, 4);
        batch["detail"] = choice({"summary", "metadata"});
        add("nova.assets.inspect_batch", "Inspect up to eight authorized assets with per-asset results.", batch, targetKeys);
        add("nova.workflow.prepare", "Read document availability, revisions and native edit constraints.", targets, targetKeys);
        targets["scope"] = choice({"assets", "animation_sidecars"});
        targets["limit"] = integerField(1, 200);
        targets["expectedRevisions"] = arrayField(
            objectSchema({{"assetId", asset}, {"revision", generation}}, {"assetId", "revision"}), 1, 8);
        add("nova.workflow.validate_changes", "Validate selected assets and reject results for stale revisions.", targets, targetKeys);
        addEditingTools(list, generation);
        auto define = [&](const QString &name, const QString &description, QJsonObject fields,
                          QJsonArray required, bool readOnly) {
            list.append(QJsonObject{{"name", name}, {"description", description},
                {"inputSchema", objectSchema(fields, required)}, {"outputSchema", outputSchema(name)},
                {"annotations", QJsonObject{{"readOnlyHint", readOnly}, {"openWorldHint", false}}}});
        };
        define("nova.guide.list", "List Nova's resource-pack guide topics and the Minecraft versions they cover.",
               {}, {}, true);
        define("nova.guide.get", "Read a versioned Nova guide; the same text as resource nova://rules/{version}/{topic}.",
               {{"topic", described(stringField(32, "^[a-z][a-z0-9_-]*$"), "A guide topic from nova.guide.list, such as items.")},
                {"version", described(stringField(16, "^[0-9]{1,3}(\\.[0-9]{1,3}){1,2}$"),
                                      "A Minecraft Java version such as 1.21.11.")}},
               {"topic", "version"}, true);
        const auto project = described(generation, "The projectId from nova.project.info.");
        const auto current = described(generation, "The generation from nova.project.info.");
        const auto documentId = described(stringField(32, "^[a-f0-9]{32}$"), "A documentId from nova.drafts.list.");
        define("nova.drafts.list", "List this project's open documents with their saved state and revisions.",
               {{"generation", current}, {"projectId", project}}, {"generation", "projectId"}, true);
        define("nova.drafts.adopt", "Reconnect to a document that is still open in Nova; returns this connection's handles.",
               {{"generation", current}, {"projectId", project}, {"documentId", documentId},
                {"path", described(stringField(256, "^assets/[a-z0-9_.-]+/[a-z0-9_./-]+$"),
                                   "The document's resource-pack path from nova.drafts.list, such as "
                                   "assets/minecraft/textures/block/stone.png.")},
                {"draftRevision", described(stringField(32, "^[a-f0-9]{32}$"), "The draftRevision from nova.drafts.list.")}},
               {"generation", "projectId", "documentId", "path", "draftRevision"}, true);
        define("nova.drafts.save", "Save one open document to its own resource path in the pack.",
               {{"generation", current}, {"projectId", project}, {"documentId", documentId},
                {"expectedRevision", described(generation, "The document's current revision for this connection.")}},
               {"generation", "projectId", "documentId", "expectedRevision"}, false);
        define("nova.drafts.save_all", "Save every unsaved open document of this project; reports each result.",
               {{"generation", current}, {"projectId", project}}, {"generation", "projectId"}, false);
        addCompactTextureTools(list, generation);
        // MCP Phase 3: read-only vanilla texture cache access and asset relationships.
        const auto version = described(stringField(16, "^[0-9]{1,3}(\\.[0-9]{1,3}){1,2}$"),
                                       "A Minecraft Java version such as 1.21.11.");
        const auto identity = described(stringField(64, "^[a-f0-9]{64}$"),
                                        "A cache entry identity from nova.vanilla.list; defaults to the project's baseline "
                                        "entry for that version, else the newest verified one.");
        define("nova.vanilla.list",
               "List textures in Nova's verified local vanilla texture cache for one Minecraft version: path, size, "
               "SHA-256 and dimensions. Textures only (no models). Never downloads; reports vanilla_not_cached instead.",
               {{"generation", current}, {"projectId", project}, {"version", version}, {"identity", identity},
                {"prefix", described(stringField(128, "^[a-z0-9_./-]*$"),
                                     "Only paths under assets/minecraft/textures/<prefix>, such as block/ or item/.")},
                {"search", described(stringField(64, "^[a-z0-9_./-]+$"), "Only paths containing this text.")},
                {"animated", described(QJsonObject{{"type", "boolean"}}, "Only textures with .png.mcmeta animation metadata.")},
                {"offset", integerField(0, 100000)}, {"limit", integerField(1, 200)}},
               {"generation", "projectId", "version"}, true);
        define("nova.vanilla.texture",
               "Return one texture from Nova's verified local vanilla texture cache as a PNG image block, with its "
               "SHA-256 (checked on read), dimensions, animation metadata and provenance. Never downloads.",
               {{"generation", current}, {"projectId", project}, {"version", version}, {"identity", identity},
                {"path", described(stringField(256, "^assets/minecraft/textures/[a-z0-9_./-]+\\.png$"),
                                   "A cached texture path from nova.vanilla.list, such as "
                                   "assets/minecraft/textures/block/stone.png.")},
                {"include", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"image", "metadata"}},
                    {"description", "image (default): include the PNG (at most 262144 bytes); metadata: omit it."}}}},
               {"generation", "projectId", "version", "path"}, true);
        define("nova.asset.relationships",
               "Explain how one resource-pack asset is selected and used: item definitions and blockstates, model parent "
               "chains, texture variables, faces and UVs, display contexts, animation metadata and companion assets, each "
               "with its source (project, open draft, resource stack) and evidence. Unresolvable links are reported, "
               "never guessed.",
               {{"generation", current}, {"projectId", project},
                {"path", described(stringField(256, "^assets/[a-z0-9_.-]+/[a-z0-9_./-]+$"),
                                   "A resource-pack path such as assets/minecraft/textures/item/diamond_sword.png.")},
                {"version", described(version, "The Minecraft version; must be the project's target version (the default).")}},
               {"generation", "projectId", "path"}, true);
        return list;
    }();
    return definitions;
}
QString escapePointer(QString key) {
    return key.left(64).replace('~', "~0").replace('/', "~1");
}
QString expected(const QJsonObject &schema, const QString &fallback) {
    const auto description = schema["description"].toString();
    return description.isEmpty() ? fallback : "expected " + description.left(1).toLower() + description.mid(1);
}
// Same acceptance rules as before; on failure it names the JSON pointer and the rule.
SchemaError check(const QJsonValue &value, const QJsonObject &schema, const QString &pointer) {
    auto fail = [&](const QString &why) {
        return SchemaError{pointer, QString("Invalid argument at %1: %2").arg(pointer.isEmpty() ? "/" : pointer, why)};
    };
    if (schema.contains("oneOf")) {
        const auto options = schema["oneOf"].toArray();
        int matches = 0;
        for (const auto &option : options)
            matches += check(value, option.toObject(), pointer).isEmpty();
        if (matches == 1)
            return {};
        if (matches > 1)
            return fail("the value matches more than one allowed form.");
        // Commands: explain the variant that has the same command name.
        const auto command = value.toObject().value("command");
        for (const auto &option : options) {
            const auto names = option.toObject()["properties"].toObject()["command"].toObject()["enum"].toArray();
            if (command.isString() && names.contains(command))
                return check(value, option.toObject(), pointer);
        }
        if (command.isString())
            return fail(QString("unknown command \"%1\".").arg(command.toString().left(48)));
        return fail("the value does not match any allowed form.");
    }
    if (schema.contains("enum")) {
        const auto values = schema["enum"].toArray();
        if (values.contains(value))
            return {};
        QStringList names;
        for (const auto &option : values)
            names << QString::fromUtf8(QJsonDocument(QJsonArray{option}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
        return fail("expected one of " + names.join(", ") + '.');
    }
    const auto type = schema["type"];
    if (type == "object") {
        if (!value.isObject())
            return fail("expected an object.");
        const auto args = value.toObject();
        const auto properties = schema["properties"].toObject();
        for (const auto &key : schema["required"].toArray())
            if (!args.contains(key.toString())) {
                const auto child = pointer + '/' + escapePointer(key.toString());
                return {child, QString("Missing required argument %1.").arg(child)};
            }
        for (auto it = args.begin(); it != args.end(); ++it) {
            const auto child = pointer + '/' + escapePointer(it.key());
            if (!properties.contains(it.key()))
                return {child, QString("Unknown argument %1. Allowed: %2.").arg(child,
                                   properties.isEmpty() ? QString("none") : properties.keys().join(", "))};
            if (const auto error = check(it.value(), properties[it.key()].toObject(), child); !error.isEmpty())
                return error;
        }
        return {};
    }
    if (type == "array") {
        const auto values = value.toArray();
        const int min = schema["minItems"].toInt(), max = schema["maxItems"].toInt();
        if (!value.isArray())
            return fail("expected an array.");
        if (values.size() < min || values.size() > max)
            return fail(QString("expected %1 to %2 items.").arg(min).arg(max));
        for (qsizetype i = 0; i < values.size(); ++i)
            if (const auto error = check(values[i], schema["items"].toObject(), pointer + '/' + QString::number(i));
                !error.isEmpty())
                return error;
        return {};
    }
    if (type == "boolean")
        return value.isBool() ? SchemaError{} : fail("expected true or false.");
    if (type == "integer" || type == "number") {
        const auto n = value.toDouble();
        const auto range = QString("from %1 to %2").arg(schema["minimum"].toDouble()).arg(schema["maximum"].toDouble());
        if (!value.isDouble() || (type == "integer" && n != std::floor(n)))
            return fail(QString(type == "integer" ? "expected an integer %1." : "expected a number %1.").arg(range));
        if (n < schema["minimum"].toDouble() || n > schema["maximum"].toDouble())
            return fail(QString("expected a value %1.").arg(range));
        return {};
    }
    const auto string = value.toString();
    if (!value.isString())
        return fail(expected(schema, "expected a string."));
    if (string.size() < schema["minLength"].toInt() || string.size() > schema["maxLength"].toInt())
        return fail(expected(schema, QString("expected at most %1 characters.").arg(schema["maxLength"].toInt())));
    if (!schema.contains("pattern"))
        return {};
    const auto match = QRegularExpression(schema["pattern"].toString()).match(string);
    if (match.hasMatch() && match.capturedLength() == string.size())
        return {};
    return fail(expected(schema, "expected a value of the form " + schema["pattern"].toString() + '.'));
}
bool validId(const QJsonValue &value) {
    if (value.isString())
        return value.toString().size() <= 256;
    const auto n = value.toDouble();
    return value.isDouble() && std::floor(n) == n && std::abs(n) <= 9007199254740991.0;
}
}

QStringList categories() {
    return {"Animations", "Blocks", "Items", "Entities", "GUI", "Particles", "Environment",
            "Models", "Blockstates", "Fonts", "Sounds", "Other assets"};
}
QString randomToken() {
    QByteArray bytes(32, '\0');
    for (qsizetype i = 0; i < bytes.size(); i += 4) {
        const auto word = QRandomGenerator::system()->generate();
        for (int j = 0; j < 4; ++j)
            bytes[i + j] = char(word >> (8 * j));
    }
    return QString::fromLatin1(bytes.toHex());
}
QJsonObject rpcError(const QJsonValue &id, int code, const QString &message, const QJsonValue &data) {
    QJsonObject error{{"code", code}, {"message", message}};
    if (!data.isUndefined() && !data.isNull())
        error["data"] = data;
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}
QJsonObject toolError(const QString &code, const QString &message, const QJsonObject &details) {
    QJsonObject error{{"code", code}, {"message", message}};
    if (!details.isEmpty())
        error["details"] = details;
    return {{"error", error}};
}
QJsonObject resourceReply(const QJsonValue &id, const QString &uri, const QString &mimeType, const QString &text) {
    return reply(id, {{"contents", QJsonArray{QJsonObject{{"uri", uri}, {"mimeType", mimeType}, {"text", text}}}}});
}
SchemaError checkArguments(const QJsonValue &value, const QJsonObject &schema) { return check(value, schema, {}); }
const QJsonArray &toolDefinitions() { return tools(); }
QString compactTextureCommand(const QString &tool) {
    for (const auto name : {"put_pixels", "palette_remap", "gradient_map", "erase"})
        if (tool == QString("nova.texture.") + name)
            return name;
    return {};
}
QJsonObject toolReply(const QJsonValue &id, const QJsonObject &payload) {
    auto metadata = payload;
    const auto image = metadata.take("_pngImage").toString();
    const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    QJsonObject result{{"content", QJsonArray{QJsonObject{{"type", "text"},
                        {"text", QString::fromUtf8(json)}}}}};
    // Output schemas describe successful results, and clients may validate any structured
    // content against them. A tool error is therefore reported in its text block only.
    if (payload.contains("error"))
        result["isError"] = true;
    else
        result["structuredContent"] = metadata;
    if (!image.isEmpty()) {
        auto content = result["content"].toArray();
        content.append(QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", image}});
        result["content"] = content;
    }
    return reply(id, result);
}

Request Protocol::receive(const QByteArray &line) {
    Request result;
    auto fail = [&](int code, const QString &message) {
        result.response = rpcError(result.id, code, message);
        return result;
    };
    if (line.size() > RequestLimit)
        return fail(-32600, "Request exceeds the framing limit");
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString decoded = utf8(line);
    Q_UNUSED(decoded);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(line, &error);
    if (utf8.hasError())
        return fail(-32700, "Invalid UTF-8");
    if (error.error != QJsonParseError::NoError) {
        // Qt's document parser rejects valid scalar JSON. JSON-RPC rejects its shape.
        QJsonParseError scalarError;
        QJsonDocument::fromJson("[" + line + "]", &scalarError);
        return scalarError.error == QJsonParseError::NoError
            ? fail(-32600, "Expected a JSON-RPC object") : fail(-32700, "Invalid JSON");
    }
    const auto message = document.object();
    const auto method = message["method"].toString();
    const bool notification = !message.contains("id");
    if (!notification && validId(message["id"]))
        result.id = message["id"];
    if (message["jsonrpc"] != "2.0" || method.isEmpty() || (!notification && result.id.isNull()) ||
        message.contains("result") || message.contains("error"))
        return fail(-32600, "Invalid JSON-RPC request");
    if (message.contains("params") && !message["params"].isObject())
        return notification ? result : fail(-32602, "Expected object params");
    const auto params = message["params"].toObject();
    if (notification) {
        if (method == "notifications/initialized" && state == State::AwaitingNotification)
            state = State::Ready;
        if (method == "notifications/cancelled" && state == State::Ready && validId(params["requestId"]))
            result.cancelId = params["requestId"];
        return result;
    }
    if (method == "initialize") {
        if (state != State::Initial)
            return fail(-32600, "Session already initialized");
        const auto info = params["clientInfo"].toObject();
        if (params["protocolVersion"].toString().isEmpty() || !params["capabilities"].isObject() ||
            info["name"].toString().isEmpty() || info["version"].toString().isEmpty())
            return fail(-32602, "Invalid initialization parameters");
        client = info["name"].toString().left(80).simplified();
        state = State::AwaitingNotification;
        result.response = reply(result.id, {{"protocolVersion", Version},
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}, {"resources", QJsonObject{}}}},
            {"serverInfo", QJsonObject{{"name", "Nova Studio"}, {"version", nova::AppVersion}}},
            {"instructions", guides::instructions()}});
    } else if (method == "ping") {
        result.response = reply(result.id, {});
    } else if (method != "tools/list" && method != "tools/call" && method != "resources/list" &&
               method != "resources/read" && method != "resources/templates/list") {
        return fail(-32601, "Method not found");
    } else if (state != State::Ready) {
        return fail(-32002, "Initialize the session first");
    } else if (method == "resources/list" || method == "resources/templates/list") {
        if (params.contains("cursor"))
            return fail(-32602, "Unknown resources cursor");
        result.response = method == "resources/list" ? reply(result.id, {{"resources", guides::resources()}})
                                                     : reply(result.id, {{"resourceTemplates", guides::resourceTemplates()}});
    } else if (method == "resources/read") {
        const auto uri = params["uri"].toString();
        if (!params["uri"].isString() || uri.isEmpty() || uri.size() > 256)
            return fail(-32602, "Expected params.uri: a Nova resource URI from resources/list");
        QString version, topic;
        if (uri == guides::IndexUri) {
            result.response = resourceReply(result.id, uri, "text/markdown", guides::indexText());
        } else if (guides::parseUri(uri, &version, &topic)) {
            const auto found = guides::guide(version, topic);
            result.response = found.code.isEmpty()
                ? resourceReply(result.id, uri, "text/markdown", found.guide.text)
                : rpcError(result.id, -32002, found.message,
                           QJsonObject{{"uri", uri}, {"reason", found.code}, {"details", found.details}});
        } else if (uri == guides::StyleNotesUri) {
            result.resource = uri;
        } else {
            result.response = rpcError(result.id, -32002,
                "Resource not found. Nova resources are listed by resources/list; guides use "
                "nova://rules/{version}/{topic}.", QJsonObject{{"uri", uri}});
        }
    } else if (method == "tools/list") {
        if (params.contains("cursor"))
            return fail(-32602, "Unknown tools cursor");
        result.response = reply(result.id, {{"tools", tools()}});
    } else {
        const auto name = params["name"].toString();
        for (const auto &entry : tools()) {
            const auto def = entry.toObject();
            if (def["name"] != name)
                continue;
            const auto args = params["arguments"].toObject();
            const auto invalid = params.contains("arguments") && !params["arguments"].isObject()
                ? SchemaError{"", "Invalid argument at /: expected an object of named arguments."}
                : check(args, def["inputSchema"].toObject(), {});
            if (!invalid.isEmpty()) {
                result.response = toolReply(result.id, toolError("invalid_arguments", invalid.message,
                                                                 {{"pointer", invalid.pointer}}));
                return result;
            }
            if (name == "nova.guide.list") {
                result.response = toolReply(result.id, guides::catalog());
                return result;
            }
            if (name == "nova.guide.get") {
                const auto found = guides::guide(args["version"].toString(), args["topic"].toString());
                result.response = toolReply(result.id, found.code.isEmpty()
                    ? QJsonObject{{"uri", found.guide.uri}, {"topic", found.guide.topic},
                                  {"version", found.guide.version}, {"title", found.guide.title},
                                  {"mimeType", "text/markdown"}, {"text", found.guide.text}}
                    : toolError(found.code, found.message, found.details));
                return result;
            }
            result.tool = name;
            result.arguments = args;
            return result;
        }
        return fail(-32602, "Unknown tool");
    }
    return result;
}
}
