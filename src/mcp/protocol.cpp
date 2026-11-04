// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.h"
#include "build_identity.h"
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
        add("generation", "string");
        add("diagnostics", "array");
        add("counts", "object");
        for (auto key : {"files", "inspected", "milliseconds"})
            add(key, "integer");
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
        add("nova.pack.validate", "Run read-only resource validation without fallback packs.",
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
        return list;
    }();
    return definitions;
}
bool validValue(const QJsonValue &value, const QJsonObject &schema) {
    if (schema.contains("oneOf")) {
        int matches = 0;
        for (const auto &option : schema["oneOf"].toArray())
            matches += validValue(value, option.toObject());
        return matches == 1;
    }
    if (schema.contains("enum"))
        return schema["enum"].toArray().contains(value);
    const auto type = schema["type"];
    if (type == "object") {
        if (!value.isObject())
            return false;
        const auto args = value.toObject();
        const auto properties = schema["properties"].toObject();
        for (const auto &key : schema["required"].toArray())
            if (!args.contains(key.toString()))
                return false;
        for (auto it = args.begin(); it != args.end(); ++it)
            if (!properties.contains(it.key()) || !validValue(it.value(), properties[it.key()].toObject()))
                return false;
        return true;
    }
    if (type == "array") {
        const auto values = value.toArray();
        if (!value.isArray() || values.size() < schema["minItems"].toInt() ||
            values.size() > schema["maxItems"].toInt())
            return false;
        for (const auto &item : values)
            if (!validValue(item, schema["items"].toObject()))
                return false;
        return true;
    }
    if (type == "boolean")
        return value.isBool();
    if (type == "integer" || type == "number") {
        const auto n = value.toDouble();
        return value.isDouble() && (type == "number" || n == std::floor(n)) &&
               n >= schema["minimum"].toDouble() && n <= schema["maximum"].toDouble();
    }
    const auto string = value.toString();
    if (!value.isString() || string.size() < schema["minLength"].toInt() ||
        string.size() > schema["maxLength"].toInt())
        return false;
    if (!schema.contains("pattern"))
        return true;
    const auto match = QRegularExpression(schema["pattern"].toString()).match(string);
    return match.hasMatch() && match.capturedLength() == string.size();
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
QJsonObject rpcError(const QJsonValue &id, int code, const QString &message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
QJsonObject toolError(const QString &code, const QString &message) {
    return {{"error", QJsonObject{{"code", code}, {"message", message}}}};
}
QJsonObject toolReply(const QJsonValue &id, const QJsonObject &payload) {
    auto metadata = payload;
    const auto image = metadata.take("_pngImage").toString();
    const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    QJsonObject result{{"content", QJsonArray{QJsonObject{{"type", "text"},
                        {"text", QString::fromUtf8(json)}}}}, {"structuredContent", metadata}};
    if (!image.isEmpty()) {
        auto content = result["content"].toArray();
        content.append(QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", image}});
        result["content"] = content;
    }
    if (payload.contains("error"))
        result["isError"] = true;
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
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
            {"serverInfo", QJsonObject{{"name", "Nova Studio"}, {"version", nova::AppVersion}}}});
    } else if (method == "ping") {
        result.response = reply(result.id, {});
    } else if (method != "tools/list" && method != "tools/call") {
        return fail(-32601, "Method not found");
    } else if (state != State::Ready) {
        return fail(-32002, "Initialize the session first");
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
            if ((params.contains("arguments") && !params["arguments"].isObject()) ||
                !validValue(args, def["inputSchema"].toObject())) {
                result.response = toolReply(result.id, toolError("invalid_arguments", "Arguments do not match the tool schema."));
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
