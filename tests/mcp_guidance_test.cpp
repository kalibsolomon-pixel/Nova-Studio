// SPDX-License-Identifier: GPL-3.0-or-later
// MCP Phase 1: initialize instructions, versioned guide resources and nova.guide.get parity,
// verified version facts, and field-specific argument errors. Protocol-level; no project access.
#include "guides.h"
#include "project.h"
#include "protocol.h"
#include <QFile>
#include <QJsonDocument>
#include <QtTest>

using namespace nova::mcp;

namespace {
QByteArray request(int id, const QString &method, const QJsonObject &params = {}) {
    QJsonObject data{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (id >= 0)
        data["id"] = id;
    return QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
}
QByteArray call(int id, const QString &tool, const QJsonObject &args) {
    return request(id, "tools/call", {{"name", tool}, {"arguments", args}});
}
QJsonObject initialized(Protocol &protocol, QJsonObject *init = nullptr) {
    const auto reply = protocol.receive(request(1, "initialize", {{"protocolVersion", "2025-11-25"},
        {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "guidance tests"}, {"version", "1"}}}}));
    protocol.receive(request(-1, "notifications/initialized"));
    if (init)
        *init = reply.response["result"].toObject();
    return reply.response;
}
QJsonObject result(const Request &request) { return request.response["result"].toObject(); }
// A tool result: structured content on success, the JSON text block on error (never both).
QJsonObject payload(const QJsonObject &reply, bool *isError = nullptr) {
    if (isError)
        *isError = reply["isError"].toBool();
    if (!reply["isError"].toBool())
        return reply["structuredContent"].toObject();
    if (reply.contains("structuredContent"))
        return {{"unexpected", "structuredContent on an error"}};
    return QJsonDocument::fromJson(reply["content"].toArray().first().toObject()["text"].toString().toUtf8()).object();
}
QJsonObject facts() {
    QFile file(QStringLiteral(NOVA_GUIDE_FACTS));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object()["facts"].toObject();
}
QString size(const QJsonValue &value) {
    const auto pair = value.toArray();
    return QString("%1×%2").arg(pair[0].toInt()).arg(pair[1].toInt());
}
} // namespace

class GuidanceTest : public QObject {
    Q_OBJECT
  private slots:
    void initializeInstructions() {
        Protocol protocol;
        QJsonObject init;
        initialized(protocol, &init);
        QCOMPARE(init["protocolVersion"].toString(), "2025-11-25");
        QVERIFY(init["capabilities"].toObject()["tools"].isObject());
        QVERIFY(init["capabilities"].toObject()["resources"].isObject());
        const auto instructions = init["instructions"].toString();
        qInfo().noquote() << "Instructions (" << instructions.size() << "chars):\n" << instructions;
        // Concise: one short block; detail lives in the guides.
        QVERIFY2(instructions.size() > 200 && instructions.size() <= 1600, qPrintable(QString::number(instructions.size())));
        for (const auto &phrase : {"resource-pack paths", "never filesystem paths", "Minecraft version",
                                   "ask the user", "not proof", "Saving a texture writes it to its resource path",
                                   "nova.drafts.save", "Save All", "saved and failed", "item definitions, models, blockstates",
                                   ".png.mcmeta", "nova.guide.get", "never try to work around", "consent"})
            QVERIFY2(instructions.contains(QLatin1String(phrase)), phrase);
        QVERIFY(!instructions.contains("1.21.11")); // version rules belong in guides, not in instructions
    }

    void resourcesAreDiscoverableAndReadOnly() {
        Protocol protocol;
        QCOMPARE(protocol.receive(request(2, "resources/list")).response["error"].toObject()["code"].toInt(), -32002);
        initialized(protocol);
        const auto list = result(protocol.receive(request(2, "resources/list")))["resources"].toArray();
        QStringList uris;
        for (const auto &value : list) {
            const auto resource = value.toObject();
            uris << resource["uri"].toString();
            QVERIFY(!resource["name"].toString().isEmpty());
            QVERIFY(!resource["mimeType"].toString().isEmpty());
        }
        QVERIFY(uris.contains(guides::IndexUri));
        QVERIFY(uris.contains(guides::StyleNotesUri));
        for (const auto &topic : guides::topics())
            QVERIFY2(uris.contains("nova://rules/1.21.11/" + topic), qPrintable(topic));
        QCOMPARE(guides::topics(), (QStringList{"pack", "items", "textures", "armor", "trims", "animations", "pvp"}));
        QCOMPARE(list.size(), 1 + guides::topics().size() + 1);
        const auto templates = result(protocol.receive(request(3, "resources/templates/list")))["resourceTemplates"].toArray();
        QCOMPARE(templates.first().toObject()["uriTemplate"].toString(), "nova://rules/{version}/{topic}");
        // Read-only: no subscription or write methods exist.
        for (const auto &method : {"resources/subscribe", "resources/unsubscribe", "resources/write"})
            QCOMPARE(protocol.receive(request(4, method, {{"uri", guides::IndexUri}})).response["error"].toObject()["code"].toInt(),
                     -32601);
        QCOMPARE(protocol.receive(request(5, "resources/list", {{"cursor", "x"}})).response["error"].toObject()["code"].toInt(),
                 -32602);
        const auto index = result(protocol.receive(request(6, "resources/read", {{"uri", guides::IndexUri}})));
        QVERIFY(index["contents"].toArray().first().toObject()["text"].toString().contains("1.21.11"));
        // Guide tools are read-only and closed-world.
        const auto tools = result(protocol.receive(request(7, "tools/list")))["tools"].toArray();
        int seen = 0;
        for (const auto &value : tools) {
            const auto tool = value.toObject();
            if (!tool["name"].toString().startsWith("nova.guide."))
                continue;
            ++seen;
            QCOMPARE(tool["annotations"].toObject()["readOnlyHint"].toBool(), true);
            QCOMPARE(tool["annotations"].toObject()["openWorldHint"].toBool(), false);
        }
        QCOMPARE(seen, 2);
        // No tool writes style notes or accepts filesystem paths for saving.
        for (const auto &value : tools) {
            const auto name = value.toObject()["name"].toString();
            QVERIFY2(!name.contains("style"), qPrintable(name));
            const auto properties = value.toObject()["inputSchema"].toObject()["properties"].toObject();
            if (name.startsWith("nova.drafts.save"))
                QVERIFY(!properties.contains("path") && !properties.contains("target"));
        }
    }

    void resourceAndToolParity() {
        Protocol protocol;
        initialized(protocol);
        const auto catalog = payload(result(protocol.receive(call(2, "nova.guide.list", {}))));
        QCOMPARE(catalog["guideVersions"].toArray(), QJsonArray{"1.21.11"});
        QCOMPARE(catalog["novaVersions"].toArray(), (QJsonArray{"1.21.4", "1.21.9", "1.21.11", "26.1", "26.3"}));
        QCOMPARE(catalog["topics"].toArray().size(), guides::topics().size());
        int id = 10;
        for (const auto &topic : guides::topics()) {
            const auto uri = "nova://rules/1.21.11/" + topic;
            const auto read = result(protocol.receive(request(++id, "resources/read", {{"uri", uri}})))["contents"]
                                  .toArray().first().toObject();
            bool isError = true;
            const auto tool = payload(result(protocol.receive(call(++id, "nova.guide.get",
                {{"topic", topic}, {"version", "1.21.11"}}))), &isError);
            QVERIFY(!isError);
            QCOMPARE(read["uri"].toString(), uri);
            QCOMPARE(tool["uri"].toString(), uri);
            QCOMPARE(read["mimeType"].toString(), "text/markdown");
            QCOMPARE(tool["mimeType"].toString(), "text/markdown");
            QCOMPARE(tool["text"].toString(), read["text"].toString()); // byte-identical
            QCOMPARE(tool["topic"].toString(), topic);
            QCOMPARE(tool["version"].toString(), "1.21.11");
            const auto text = tool["text"].toString();
            QVERIFY(text.startsWith("# " + guides::title(topic) + " — Minecraft Java 1.21.11"));
            QVERIFY(text.contains("Applies to Minecraft Java Edition 1.21.11 only"));
            QVERIFY(!text.contains("26.1") && !text.contains("26.3"));
        }
    }

    void uriParsing_data() {
        QTest::addColumn<QString>("uri");
        QTest::addColumn<bool>("parsed");
        QTest::newRow("guide") << "nova://rules/1.21.11/items" << true;
        QTest::newRow("two-part version") << "nova://rules/26.1/items" << true;
        QTest::newRow("upper-case topic") << "nova://rules/1.21.11/Items" << false;
        QTest::newRow("traversal") << "nova://rules/../items" << false;
        QTest::newRow("nested") << "nova://rules/1.21.11/items/extra" << false;
        QTest::newRow("filesystem") << "C:/Windows/win.ini" << false;
        QTest::newRow("file uri") << "file:///C:/pack/assets" << false;
        QTest::newRow("query") << "nova://rules/1.21.11/items?x=1" << false;
        QTest::newRow("empty topic") << "nova://rules/1.21.11/" << false;
    }
    void uriParsing() {
        QFETCH(QString, uri);
        QFETCH(bool, parsed);
        QString version, topic;
        QCOMPARE(guides::parseUri(uri, &version, &topic), parsed);
        Protocol protocol;
        initialized(protocol);
        const auto reply = protocol.receive(request(2, "resources/read", {{"uri", uri}}));
        if (!parsed) {
            QCOMPARE(reply.response["error"].toObject()["code"].toInt(), -32002);
            QCOMPARE(reply.response["error"].toObject()["data"].toObject()["uri"].toString(), uri);
            QVERIFY(reply.resource.isEmpty());
        }
    }

    void unsupportedVersionsAndTopics() {
        Protocol protocol;
        initialized(protocol);
        struct Case { QString topic, version, code, pointer; };
        for (const auto &[topic, version, code, pointer] : {
                 Case{"items", "26.1", "guide_unavailable", "/version"}, Case{"items", "26.3", "guide_unavailable", "/version"},
                 Case{"items", "1.21.4", "guide_unavailable", "/version"}, Case{"items", "1.21.9", "guide_unavailable", "/version"},
                 Case{"items", "1.8.9", "unsupported_version", "/version"}, Case{"items", "1.21", "unsupported_version", "/version"},
                 Case{"xray", "1.21.11", "unknown_topic", "/topic"}}) {
            bool isError = false;
            const auto reply = payload(result(protocol.receive(call(3, "nova.guide.get", {{"topic", topic}, {"version", version}}))),
                                       &isError);
            QVERIFY(isError);
            const auto failure = reply["error"].toObject();
            QCOMPARE(failure["code"].toString(), code);
            QCOMPARE(failure["details"].toObject()["pointer"].toString(), pointer);
            QVERIFY(!failure["message"].toString().isEmpty());
            if (code != "unknown_topic") {
                QVERIFY(failure["message"].toString().contains(version));
                QCOMPARE(failure["details"].toObject()["guideVersions"].toArray(), QJsonArray{"1.21.11"});
            } else {
                QCOMPARE(failure["details"].toObject()["topics"].toArray().size(), guides::topics().size());
            }
            // The resource refuses the same request with the same explanation; nothing else is substituted.
            const auto read = protocol.receive(request(4, "resources/read", {{"uri", guides::uri(version, topic)}})).response;
            QVERIFY(!read.contains("result"));
            QCOMPARE(read["error"].toObject()["code"].toInt(), -32002);
            QCOMPARE(read["error"].toObject()["message"].toString(), failure["message"].toString());
            QCOMPARE(read["error"].toObject()["data"].toObject()["reason"].toString(), code);
        }
        QVERIFY(guides::guide("26.1", "items").guide.text.isEmpty());
    }

    void novaVersionsMatchProject() {
        QStringList ids;
        for (const auto &version : nova::gameVersions())
            ids << version.id;
        ids.sort();
        auto declared = guides::novaVersions();
        declared.sort();
        QCOMPARE(declared, ids);
        for (const auto &version : guides::guideVersions())
            QVERIFY(ids.contains(version));
    }

    // Each 1.21.11 fact the guides state is checked against the fixture derived from the official
    // 1.21.11 client resources by scripts/verify-guide-facts.py.
    void verifiedFacts() {
        const auto f = facts();
        QVERIFY2(!f.isEmpty(), NOVA_GUIDE_FACTS);
        QCOMPARE(f["version"].toString(), "1.21.11");
        auto text = [](const QString &topic) { return guides::guide("1.21.11", topic).guide.text; };
        auto has = [&](const QString &topic, const QString &value) {
            QVERIFY2(text(topic).contains(value), qPrintable(topic + ": " + value));
        };
        const auto format = f["packFormat"].toArray();
        has("pack", QString("**%1.%2**").arg(format[0].toInt()).arg(format[1].toInt()));
        has("pack", QString("`[%1, %2]`").arg(format[0].toInt()).arg(format[1].toInt()));
        auto all = QString();
        for (const auto &topic : guides::topics())
            all += text(topic);
        for (const auto &path : f["paths"].toArray())
            QVERIFY2(all.contains(path.toString()) || all.contains(path.toString().section('/', 3)) ||
                     all.contains(path.toString().section('/', 2)), qPrintable(path.toString()));
        const auto items = f["itemDefinitions"].toObject();
        has("items", QLocale(QLocale::English).toString(items["count"].toInt()));
        has("items", items["diamondSword"].toObject()["type"].toString());
        has("items", items["diamondSword"].toObject()["model"].toString());
        for (const auto &key : {"type", "property", "fallback"})
            has("items", items["diamondChestplate"].toObject()[key].toString());
        const auto models = f["models"].toObject();
        has("items", models["diamondSword"].toObject()["parent"].toString());
        has("items", models["diamondChestplate"].toObject()["parent"].toString());
        has("items", "`" + models["generatedParent"].toString() + "`");
        has("items", "`" + models["handheldParent"].toString() + "`");
        const auto sizes = f["textureSizes"].toObject();
        for (const auto &key : {"block/stone.png", "item/diamond_sword.png", "item/diamond_chestplate.png"})
            QCOMPARE(size(sizes[key]), QString("16×16"));
        has("textures", "16×16");
        has("textures", size(sizes["entity/equipment/humanoid/diamond.png"]));
        has("armor", size(sizes["entity/equipment/humanoid_leggings/diamond.png"]));
        has("trims", size(sizes["trims/entity/humanoid/coast.png"]));
        has("trims", size(sizes["trims/color_palettes/trim_palette.png"]));
        has("trims", "(" + size(sizes["trims/items/chestplate_trim.png"]) + ")");
        const auto stone = f["blockstates"].toObject()["stoneModels"].toArray();
        for (const auto &model : stone)
            has("textures", model.toString());
        const auto equipment = f["equipment"].toObject();
        for (const auto &layer : equipment["diamondLayers"].toArray())
            has("armor", "`" + layer.toString() + "`");
        has("armor", equipment["diamondTexture"].toString());
        for (const auto &path : equipment["wornTextures"].toArray())
            has("armor", path.toString());
        QVERIFY(equipment["elytraUsesPlayerTexture"].toBool());
        has("armor", "use_player_texture: true");
        const auto trims = f["trims"].toObject();
        has("trims", trims["sourceType"].toString());
        has("trims", trims["paletteKey"].toString());
        QCOMPARE(trims["itemAtlasPaletteKey"].toString(), trims["paletteKey"].toString());
        has("trims", QString("%1 material permutations").arg(trims["materials"].toArray().size()));
        for (const auto &material : trims["materials"].toArray())
            has("trims", material.toString());
        for (const auto &folder : trims["entityFolders"].toArray())
            has("trims", "trims/entity/" + folder.toString() + "/");
        for (const auto &texture : trims["itemAtlasTrimTextures"].toArray())
            has("trims", texture.toString().section('/', -1));
        const auto layers = trims["itemTrimModelLayers"].toObject();
        has("trims", "`layer0` `" + layers["layer0"].toString() + "`");
        has("trims", "`layer1` `" + layers["layer1"].toString() + "`");
        const auto animations = f["animations"].toObject();
        for (const auto &name : {"magma", "sea_lantern"}) {
            const auto a = animations[name].toObject();
            const auto frames = a["size"].toArray()[1].toInt() / a["size"].toArray()[0].toInt();
            has("animations", QString("`block/%1.png` is %2: %3 frames").arg(name, size(a["size"])).arg(frames));
            has("animations", QString("%1 %2").arg(name).arg(a["frametime"].toInt()));
        }
        QCOMPARE(animations["magma"].toObject()["frames"].toInt(), 3);
        QVERIFY(animations["magma"].toObject()["interpolate"].toBool());
        QVERIFY(animations["prismarine"].toObject()["interpolate"].toBool());
        const auto prismarine = animations["prismarine"].toObject();
        has("animations", QString("prismarine %1").arg(prismarine["frametime"].toInt()));
        has("animations", QString("%1 entries over its %2 frames (%3)").arg(prismarine["frames"].toInt())
                              .arg(prismarine["size"].toArray()[1].toInt() / prismarine["size"].toArray()[0].toInt())
                              .arg(size(prismarine["size"])));
        const auto wind = animations["conduit_wind"].toObject();
        const auto windSize = wind["size"].toArray();
        has("animations", QString("`entity/conduit/wind.png` is %1 with `\"height\": %2`").arg(size(wind["size"])).arg(wind["height"].toInt()));
        has("animations", QString("%1 frames of %2×%3").arg(windSize[1].toInt() / wind["height"].toInt())
                              .arg(windSize[0].toInt()).arg(wind["height"].toInt()));
        const auto metadata = f["animationMetadata"].toObject();
        QVERIFY(metadata["objectFrameEntries"].toInt() > 0);
        QCOMPARE(metadata["objectFrameKeys"].toArray(), (QJsonArray{"index", "time"}));
        has("animations", "`index` and `time`");
        QVERIFY(metadata["explicitFrameSize"].toInt() > 0);
        // PvP guidance is labelled policy, includes the required points, and states no game facts.
        for (const auto &phrase : {"policy recommendations, not game facts", "silhouettes", "material tiers",
                                   "x-ray", "server rules", "bans"})
            has("pvp", phrase);
    }

    void argumentErrorsNamePointers() {
        Protocol protocol;
        initialized(protocol);
        const auto generation = randomToken();
        const auto uuid = QString("00000000-0000-0000-0000-000000000001");
        struct Case { QString tool; QJsonObject args; QString pointer, message; };
        const QList<Case> cases{
            {"nova.guide.get", {{"version", "1.21.11"}}, "/topic", "Missing required argument /topic."},
            {"nova.guide.get", {{"topic", "items"}, {"version", "C:/Windows"}}, "/version",
             "Invalid argument at /version: expected a Minecraft Java version such as 1.21.11."},
            {"nova.guide.get", {{"topic", "items"}, {"version", 1.21}}, "/version",
             "Invalid argument at /version: expected a Minecraft Java version such as 1.21.11."},
            {"nova.assets.list", {{"generation", generation}, {"limit", 101}}, "/limit",
             "Invalid argument at /limit: expected a value from 1 to 100."},
            {"nova.assets.list", {{"generation", generation}, {"category", "bad"}}, "/category",
             "Invalid argument at /category: expected one of"},
            {"nova.drafts.adopt", {{"generation", generation}, {"projectId", generation}, {"documentId", QString(32, 'a')},
              {"path", "C:\\pack\\assets\\minecraft\\textures\\block\\stone.png"}, {"draftRevision", QString(32, 'a')}},
             "/path", "Invalid argument at /path: expected the document's resource-pack path"},
            {"nova.drafts.save", {{"generation", generation}, {"projectId", generation}, {"documentId", "../../x"},
              {"expectedRevision", generation}}, "/documentId", "Invalid argument at /documentId: expected a documentId"},
            {"nova.texture.edit", {{"generation", generation}, {"projectId", generation}, {"assetId", generation},
              {"expectedRevision", generation}, {"operation", QJsonObject{{"command", "fill_rect"}, {"layer", uuid},
              {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}, {"color", "red"}, {"opacity", 255}}}}, "/operation/color",
             "Invalid argument at /operation/color:"},
            {"nova.texture.edit", {{"generation", generation}, {"projectId", generation}, {"assetId", generation},
              {"expectedRevision", generation}, {"operation", QJsonObject{{"command", "paint_everything"}}}}, "/operation",
             "Invalid argument at /operation: unknown command \"paint_everything\"."},
            {"nova.edit.batch", {{"generation", generation}, {"projectId", generation}, {"assetId", generation},
              {"expectedRevision", generation}, {"operations", QJsonArray{QJsonObject{{"command", "layer_remove"}}}}},
             "/operations/0/layer", "Missing required argument /operations/0/layer."},
        };
        for (const auto &[tool, args, pointer, message] : cases) {
            bool isError = false;
            const auto failure = payload(result(protocol.receive(call(5, tool, args))), &isError)["error"].toObject();
            QVERIFY2(isError, qPrintable(tool));
            QCOMPARE(failure["code"].toString(), "invalid_arguments");
            QCOMPARE(failure["details"].toObject()["pointer"].toString(), pointer);
            QVERIFY2(failure["message"].toString().startsWith(message), qPrintable(failure["message"].toString()));
            QVERIFY2(!failure["message"].toString().contains("C:\\pack"), "argument values are not echoed");
        }
        // Arguments that are not an object.
        const auto reply = result(protocol.receive(request(6, "tools/call", {{"name", "nova.guide.list"}, {"arguments", 3}})));
        QCOMPARE(payload(reply)["error"].toObject()["details"].toObject()["pointer"].toString(), "");
        // Schema checks accept exactly what they accepted before: valid arguments still pass.
        QVERIFY(checkArguments(QJsonObject{{"generation", generation}, {"limit", 100}},
                               toolDefinitions()[1].toObject()["inputSchema"].toObject()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(GuidanceTest)
#include "mcp_guidance_test.moc"
