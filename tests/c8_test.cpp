// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 2-C8: display transforms, bulk UV, native-aware batches and recovery.
#include "creation.h"
#include "display.h"
#include "edit.h"
#include "native_batch.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuaternion>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
namespace {
bool write(const QString &root, const QString &asset, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(root + '/' + asset).absolutePath());
    QFile f(root + '/' + asset);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(); }
QByteArray fileHash(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256)
                                       : QByteArray();
}
mc::DisplayTransform transform(std::array<double, 3> r, std::array<double, 3> t, std::array<double, 3> s) {
    mc::DisplayTransform v;
    v.rotation = r;
    v.translation = t;
    v.scale = s;
    return v;
}
bool near(QVector3D a, QVector3D b) { return (a - b).length() < 1e-4f; }
QJsonObject cubeModel() {
    auto o = mc::modelTemplate(mc::ModelTemplate::MultiElement, "nova:block/a");
    o["extension"] = QJsonObject{{"preserve", 42}};
    auto display = o["display"].toObject();
    auto gui = display["gui"].toObject();
    gui["nova_extra"] = 7; // unknown key inside a context must survive edits
    display["gui"] = gui;
    o["display"] = display;
    auto elements = o["elements"].toArray();
    auto e = elements[1].toObject();
    auto faces = e["faces"].toObject();
    auto south = faces["south"].toObject();
    south["texture"] = "#big";
    south["face_extension"] = 17;
    faces["south"] = south;
    e["faces"] = faces;
    elements[1] = e;
    o["elements"] = elements;
    auto textures = o["textures"].toObject();
    textures["big"] = "nova:block/big";
    o["textures"] = textures;
    return o;
}
// ---- native documents ----
QImage solid(QRgb c, QSize size = {8, 8}) {
    QImage i(size, QImage::Format_RGBA8888);
    i.fill(QColor::fromRgba(c));
    return i;
}
QString makeStatic(const QString &root, const QString &relative) {
    const auto png = root + '/' + relative;
    QDir().mkpath(QFileInfo(png).absolutePath());
    if (!savePng(png, solid(qRgba(10, 20, 30, 255))))
        return {};
    Document doc(png, solid(qRgba(10, 20, 30, 255)));
    doc.editLayer({LayerCommand::Rename, doc.activeLayer(), "Base"});
    QUuid shade, hidden;
    doc.addLayerWithPixels("Shade", solid(qRgba(100, 0, 0, 128)), {}, nullptr, &shade);
    doc.addLayerWithPixels("Hidden", solid(qRgba(0, 100, 0, 255)), {}, nullptr, &hidden);
    doc.editLayer({LayerCommand::Visibility, hidden, {}, 0});
    doc.editLayer({LayerCommand::Opacity, shade, {}, 200});
    doc.editLayer({LayerCommand::Select, shade});
    return doc.saveNative(root) ? nativeDocumentPath(root, png) : QString();
}
QString makeAnimated(const QString &root, const QString &relative) {
    const auto png = root + '/' + relative;
    QDir().mkpath(QFileInfo(png).absolutePath());
    if (!savePng(png, solid(qRgba(1, 2, 3, 255))))
        return {};
    Document doc(png, solid(qRgba(1, 2, 3, 255)));
    doc.convertToAnimation();
    doc.editFrame(FrameCommand::Duplicate);
    doc.editFrame(FrameCommand::Duplicate);
    auto state = doc.animationState();
    int n = 0;
    for (auto &f : state.frames)
        f.layers[0].pixels = solid(qRgba(10 * ++n, 0, 0, 255));
    // Playback references frame 0 three times.
    state.entries = {{state.frames[0].id, 0}, {state.frames[1].id, 0}, {state.frames[0].id, 0},
                     {state.frames[2].id, 0}, {state.frames[0].id, 0}};
    if (!doc.replaceAnimation(state, "fixture"))
        return {};
    return doc.saveNative(root) ? nativeDocumentPath(root, png) : QString();
}
std::shared_ptr<Document> reopen(const QString &root, const QString &relative) {
    auto doc = Document::open(root + '/' + relative);
    if (doc && !doc->loadNative(root))
        return {};
    return doc;
}
batch::Pipeline invert() {
    batch::Pipeline p;
    p.name = "invert";
    p.steps.push_back({"invert", {}});
    return p;
}
QRgb inverted(QRgb c) { return qRgba(255 - qRed(c), 255 - qGreen(c), 255 - qBlue(c), qAlpha(c)); }
} // namespace

class C8Test : public QObject {
    Q_OBJECT
    QTemporaryDir outside;

  private slots:
    // ================= C8-A display transforms =================
    void contextsAreVersionChecked() {
        QCOMPARE(mc::displayContexts().size(), 9);
        for (const auto &v : {"1.21.11", "26.1", "26.3"})
            QCOMPARE(mc::displayContexts(v), mc::displayContexts());
        QVERIFY(mc::displayContexts("1.12").isEmpty());
        QCOMPARE(mc::pairedHand("thirdperson_lefthand"), QString("thirdperson_righthand"));
        QVERIFY(mc::leftHandContext("firstperson_lefthand"));
        QVERIFY(!mc::handContext("gui"));
    }
    void perContextChainResolution() {
        QTemporaryDir root;
        write(root.path(), "assets/nova/models/block/base.json",
              json({{"display",
                     QJsonObject{{"gui", QJsonObject{{"scale", QJsonArray{.5, .5, .5}}}},
                                 {"ground", QJsonObject{{"translation", QJsonArray{0, 3, 0}}}},
                                 {"thirdperson_lefthand", QJsonObject{{"rotation", QJsonArray{0, 90, 0}}}},
                                 {"firstperson_righthand", QJsonObject{{"rotation", QJsonArray{0, 45, 0}}}},
                                 {"firstperson_lefthand", QJsonObject{{"rotation", QJsonArray{0, 10, 0}}}}}}}));
        write(root.path(), "assets/nova/models/block/child.json",
              json({{"parent", "nova:block/base"},
                    {"display",
                     QJsonObject{{"gui", QJsonObject{{"rotation", QJsonArray{30, 225, 0}}}},
                                 {"thirdperson_righthand", QJsonObject{{"scale", QJsonArray{2, 2, 2}}}}}}}));
        mc::Library lib;
        lib.update(root.path(), Index::scan(root.path()));
        const auto m = lib.model("nova:block/child");
        // gui: the child's object wins as a whole; the parent's scale is NOT merged in.
        auto gui = mc::resolveDisplay(m, "gui");
        QCOMPARE(gui.source, mc::DisplaySource::Local);
        QCOMPARE(gui.value.scale, (std::array<double, 3>{1, 1, 1}));
        QCOMPARE(gui.value.rotation, (std::array<double, 3>{30, 225, 0}));
        QCOMPARE(mc::resolveDisplay(m, "ground").source, mc::DisplaySource::Inherited);
        QCOMPARE(mc::resolveDisplay(m, "ground").origin, QString("nova:block/base"));
        // Left hand falls back to THIS model's right hand before the parent's explicit left.
        auto third = mc::resolveDisplay(m, "thirdperson_lefthand");
        QCOMPARE(third.source, mc::DisplaySource::LocalFromRightHand);
        QCOMPARE(third.value.scale, (std::array<double, 3>{2, 2, 2}));
        auto first = mc::resolveDisplay(m, "firstperson_lefthand");
        QCOMPARE(first.source, mc::DisplaySource::Inherited);
        QCOMPARE(first.value.rotation[1], 10.0);
        QCOMPARE(mc::resolveDisplay(m, "head").source, mc::DisplaySource::Default);
        // Base alone: a missing right/left pair inherits the right hand mirrored in the parent.
        const auto base = lib.model("nova:block/base");
        QCOMPARE(mc::resolveDisplay(base, "thirdperson_righthand").source, mc::DisplaySource::Default);
        // Matrices: left hand negates translation X and rotation Y/Z.
        const auto t = transform({10, 20, 30}, {4, 5, 6}, {1, 2, 3});
        auto mirrored = t;
        mirrored.translation[0] = -4;
        mirrored.rotation = {10, -20, -30};
        QCOMPARE(mc::displayMatrix(t, true), mc::displayMatrix(mirrored, false));
        QCOMPARE(mc::displayTransform(m, "thirdperson_lefthand"),
                 mc::displayMatrix(third.value, true));
    }
    void rotationOrderTranslationAndClamping() {
        // rotationXYZ: R = Rx * Ry * Rz (Y applied to the point before X).
        auto m = mc::displayMatrix(transform({90, 90, 0}, {0, 0, 0}, {1, 1, 1}), false);
        const auto q = QQuaternion::fromAxisAndAngle(1, 0, 0, 90) * QQuaternion::fromAxisAndAngle(0, 1, 0, 90);
        QVERIFY(near(m.map(QVector3D(1, 0, 0)), q.rotatedVector(QVector3D(1, 0, 0))));
        QVERIFY(near(m.map(QVector3D(1, 0, 0)), QVector3D(0, 1, 0)));
        // 8 px translation = half a block = one mesh unit; scale applies before translation.
        m = mc::displayMatrix(transform({0, 0, 0}, {8, 0, 0}, {2, 2, 2}), false);
        QVERIFY(near(m.map(QVector3D(1, 0, 0)), QVector3D(3, 0, 0)));
        bool clampedT = false, clampedS = false;
        mc::clampedDisplay(transform({0, 0, 0}, {100, 0, 0}, {5, 1, 1}), &clampedT, &clampedS);
        QVERIFY(clampedT && clampedS);
        QCOMPARE(mc::displayMatrix(transform({0, 0, 0}, {100, 0, 0}, {1, 1, 1}), false),
                 mc::displayMatrix(transform({0, 0, 0}, {80, 0, 0}, {1, 1, 1}), false));
        QString error;
        QVERIFY(!mc::validateAuthoredDisplay(transform({0, 0, 0}, {81, 0, 0}, {1, 1, 1}), &error));
        QVERIFY(error.contains("clamped"));
        QVERIFY(!mc::validateAuthoredDisplay(transform({0, 0, 0}, {0, 0, 0}, {4.5, 1, 1})));
        QVERIFY(!mc::validateAuthoredDisplay(transform({qQNaN(), 0, 0}, {0, 0, 0}, {1, 1, 1})));
        QVERIFY(!mc::validateAuthoredDisplay(transform({0, 0, 0}, {0, qInf(), 0}, {1, 1, 1})));
        QVERIFY(mc::validateAuthoredDisplay(transform({-720, 0, 0}, {-80, 80, 0}, {-4, 4, 0})));
        mc::DisplayTransform parsed;
        QVERIFY(!mc::parseDisplayTransform(QJsonObject{{"rotation", QJsonArray{1, 2}}}, &parsed));
        QVERIFY(!mc::parseDisplayTransform(QJsonObject{{"scale", 2}}, &parsed)); // scalar unsupported
    }
    void itemTransformationComposesInMinecraftOrder() {
        QMatrix4x4 local;
        local.scale(2); // item-definition transformation, block space, corner origin
        QVERIFY(near(mc::itemPose({}, {}).map(QVector3D(.3f, -.2f, .7f)), QVector3D(.3f, -.2f, .7f)));
        // Block centre (mesh 0) -> local scale about the corner -> block 1.0 -> mesh 1.
        QVERIFY(near(mc::itemPose({}, local).map(QVector3D(0, 0, 0)), QVector3D(1, 1, 1)));
        // The display translation applies after the item transformation.
        const auto display = mc::displayMatrix(transform({0, 0, 0}, {8, 0, 0}, {1, 1, 1}), false);
        QVERIFY(near(mc::itemPose(display, local).map(QVector3D(0, 0, 0)), QVector3D(2, 1, 1)));
    }
    void templatesCarryDisplayTransforms() {
        const auto cube = mc::modelTemplate(mc::ModelTemplate::Cube, "nova:block/a");
        QVERIFY(cube["display"].isObject());
        QCOMPARE(cube["display"]["gui"]["scale"].toArray(), (QJsonArray{0.625, 0.625, 0.625}));
        mc::DisplayTransform third;
        QVERIFY(mc::parseDisplayTransform(cube["display"]["thirdperson_righthand"], &third));
        QVERIFY(third.scale[0] < 0.5);
        const auto flat = mc::modelTemplate(mc::ModelTemplate::FlatItem, "nova:item/a");
        QVERIFY(!flat.contains("display")); // inherits item/generated
        QCOMPARE(flat["parent"].toString(), QString("minecraft:item/generated"));
        // Every preset value is accepted by authoring (no clamping).
        const auto preset = mc::blockDisplayPreset();
        for (auto it = preset.begin(); it != preset.end(); ++it) {
            mc::DisplayTransform t;
            QVERIFY(mc::parseDisplayTransform(it.value(), &t));
            QVERIFY(mc::validateAuthoredDisplay(t));
        }
    }
    void displayEditsPreviewCommitUndoAndPreserveJson() {
        mc::ModelEdit edit;
        const auto source = cubeModel();
        QVERIFY(edit.create(source));
        QString error;
        QVERIFY(edit.previewDisplay("gui", transform({15, 30, 0}, {0.5, -1, 0}, {0.4, 0.4, 0.4}), &error));
        QVERIFY(edit.previewing());
        QCOMPARE(edit.authoritative(), source);
        auto gui = edit.source()["display"].toObject()["gui"].toObject();
        QCOMPARE(gui["nova_extra"].toInt(), 7);
        // Exact decimal JSON (no float noise such as 0.4000000059604645).
        QCOMPARE(gui["scale"].toArray()[0].toDouble(), 0.4);
        QCOMPARE(gui["translation"].toArray(), (QJsonArray{0.5, -1, 0}));
        edit.cancelPreview();
        QCOMPARE(edit.source(), source);
        QVERIFY(edit.previewDisplay("gui", transform({15, 30, 0}, {0.5, -1, 0}, {0.4, 0.4, 0.4})));
        QVERIFY(edit.commitPreview("Display · gui"));
        QVERIFY(edit.modified());
        QCOMPARE(edit.authoritative()["extension"].toObject()["preserve"].toInt(), 42);
        QVERIFY(edit.undo());
        QCOMPARE(edit.authoritative(), source);
        QVERIFY(edit.redo());
        QCOMPARE(edit.authoritative()["display"].toObject()["gui"].toObject()["rotation"].toArray(), (QJsonArray{15, 30, 0}));
        // Invalid values never stage anything.
        const auto before = edit.source();
        QVERIFY(!edit.previewDisplay("gui", transform({0, 0, 0}, {120, 0, 0}, {1, 1, 1}), &error));
        QVERIFY(!edit.previewDisplay("bogus", {}, &error));
        QCOMPARE(edit.source(), before);
        // Absent properties stay absent when default; unknown contexts are untouched.
        QVERIFY(edit.previewDisplay("head", transform({0, 180, 0}, {0, 0, 0}, {1, 1, 1})));
        QVERIFY(!edit.source()["display"].toObject()["head"].toObject().contains("translation"));
        QVERIFY(!edit.source()["display"].toObject()["head"].toObject().contains("scale"));
        QVERIFY(edit.commitPreview("head"));
    }
    void resetLinkPasteAndUnsupportedValues() {
        mc::ModelEdit edit;
        QVERIFY(edit.create(cubeModel()));
        QString error;
        // Reset removes the local context so inheritance applies again.
        QVERIFY(edit.resetDisplay("gui"));
        QVERIFY(!edit.source()["display"].toObject().contains("gui"));
        // Link: removing the explicit left hand lets Minecraft mirror the right.
        QVERIFY(edit.source()["display"].toObject().contains("firstperson_lefthand"));
        QVERIFY(edit.linkHands("firstperson_righthand", true, {}, &error));
        QVERIFY(!edit.source()["display"].toObject().contains("firstperson_lefthand"));
        const auto right = transform({0, 45, 0}, {0, 0, 0}, {0.4, 0.4, 0.4});
        QVERIFY(edit.linkHands("firstperson_lefthand", false, right, &error));
        mc::DisplayTransform left;
        QVERIFY(mc::parseDisplayTransform(edit.source()["display"].toObject()["firstperson_lefthand"], &left));
        QCOMPARE(left, right);
        QVERIFY(!edit.linkHands("gui", true, {}, &error));
        // Paste
        QVERIFY(edit.pasteDisplay("fixed", mc::displayTransformJson(right), &error));
        QVERIFY(!edit.pasteDisplay("fixed", QJsonObject{{"rotation", "bad"}}, &error));
        QVERIFY(edit.commitPreview("display edits"));
        // Removing every context removes the empty display object entirely.
        mc::ModelEdit small;
        QVERIFY(small.create({{"display", QJsonObject{{"gui", QJsonObject{}}}}}));
        QVERIFY(small.resetDisplay("gui"));
        QVERIFY(!small.source().contains("display"));
        // A context holding an unsupported value is never silently replaced.
        mc::ModelEdit bad;
        QVERIFY(bad.create({{"display", QJsonObject{{"gui", QJsonObject{{"rotation", "spin"}}}}}}));
        QVERIFY(!bad.previewDisplay("gui", {}, &error));
        QVERIFY(error.contains("unsupported"));
        QVERIFY(bad.resetDisplay("gui"));
        QVERIFY(bad.previewDisplay("gui", {}));
        // Preset is atomic: one invalid context leaves nothing staged.
        mc::ModelEdit preset;
        QVERIFY(preset.create(cubeModel()));
        const auto before = preset.source();
        QVERIFY(!preset.applyDisplayPreset({{"gui", mc::displayTransformJson({})},
                                            {"nowhere", mc::displayTransformJson({})}},
                                           &error));
        QCOMPARE(preset.source(), before);
    }
    void displaySaveReopenAndExternalConflict() {
        QTemporaryDir root;
        const QString asset = "assets/nova/models/block/a.json";
        write(root.path(), asset, json(cubeModel()));
        mc::ModelEdit edit;
        QVERIFY(edit.open(root.path(), asset));
        QVERIFY(edit.previewDisplay("ground", transform({0, 0, 0}, {0, 2.25, 0}, {0.3, 0.3, 0.3})));
        QString error;
        QVERIFY(!edit.save(&error)); // unapplied preview blocks save
        QVERIFY(edit.commitPreview("ground"));
        // External modification blocks the guarded save.
        QFile external(root.path() + '/' + asset);
        QVERIFY(external.open(QIODevice::Append));
        external.write("\n");
        external.close();
        QVERIFY(edit.externalChange());
        QVERIFY(!edit.save(&error));
        QVERIFY(edit.modified());
        mc::ModelEdit fresh;
        QVERIFY(fresh.open(root.path(), asset));
        QVERIFY(fresh.previewDisplay("ground", transform({0, 0, 0}, {0, 2.25, 0}, {0.3, 0.3, 0.3})));
        QVERIFY(fresh.commitPreview("ground"));
        QVERIFY(fresh.save(&error));
        mc::Library lib;
        lib.update(root.path(), Index::scan(root.path()));
        const auto r = mc::resolveDisplay(lib.model("nova:block/a"), "ground");
        QCOMPARE(r.source, mc::DisplaySource::Local);
        QCOMPARE(r.value.scale[1], 0.3);
        QCOMPARE(r.value.translation[1], 2.25);
        QCOMPARE(lib.model("nova:block/a").source["extension"].toObject()["preserve"].toInt(), 42);
        // Read-only fallbacks cannot be opened for editing.
        QTemporaryDir fallback;
        mc::ModelEdit outsideEdit;
        QVERIFY(!outsideEdit.open(root.path(), "../" + QFileInfo(fallback.path()).fileName() + "/x.json"));
    }
    void versionsShareDisplaySemantics() {
        QTemporaryDir root;
        write(root.path(), "assets/nova/models/item/x.json", json(cubeModel()));
        QList<QMatrix4x4> matrices;
        for (const auto &v : {"1.21.11", "26.1", "26.3"}) {
            mc::Library lib(v);
            lib.update(root.path(), Index::scan(root.path()));
            matrices << mc::displayTransform(lib.model("nova:item/x"), "thirdperson_lefthand");
        }
        QCOMPARE(matrices[0], matrices[1]);
        QCOMPARE(matrices[1], matrices[2]);
    }
    void vanillaBlockParentDisplay() {
        const auto cache = qEnvironmentVariable("NOVA_VANILLA_CACHE");
        if (cache.isEmpty())
            QSKIP("No authorized local vanilla cache configured");
        QString error;
        auto vanilla = mc::ResourceSource::open(cache, "26.1", &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTemporaryDir root;
        write(root.path(), "assets/nova/models/block/v.json",
              R"({"parent":"minecraft:block/cube_all","textures":{"all":"minecraft:block/stone"}})");
        mc::Library lib;
        lib.update(root.path(), Index::scan(root.path()));
        lib.setFallbacks({vanilla});
        const auto m = lib.model("nova:block/v");
        auto gui = mc::resolveDisplay(m, "gui");
        QCOMPARE(gui.source, mc::DisplaySource::Inherited);
        QCOMPARE(gui.origin, QString("minecraft:block/block"));
        // The vanilla block parent defines both first-person hands explicitly.
        QCOMPARE(mc::resolveDisplay(m, "firstperson_lefthand").originContext,
                 QString("firstperson_lefthand"));
        // The vanilla parent's own values match Nova's preset GUI framing.
        mc::DisplayTransform preset;
        QVERIFY(mc::parseDisplayTransform(mc::blockDisplayPreset()["gui"], &preset));
        QCOMPARE(gui.value, preset);
    }

    // ================= C8-C bulk numeric UV =================
    void bulkUvAcrossElementsAndResolutions() {
        mc::ModelEdit edit;
        QVERIFY(edit.create(cubeModel()));
        const QVector<mc::UvTarget> targets{{0, "north"}, {1, "north"}, {1, "south"}};
        auto size = [](const mc::UvTarget &t) { return t.face == "south" ? QSize(32, 32) : QSize(16, 16); };
        auto states = mc::uvStates(edit.source(), targets);
        QCOMPARE(states.size(), 3);
        QVERIFY(states[0].inferred && states[1].inferred);
        QString error;
        // Make explicit preserves rendering exactly.
        const auto before = mc::mesh([&] {
            mc::Model m;
            m.valid = true;
            m.source = m.resolved = edit.source();
            return m;
        }());
        mc::UvBulk op;
        op.kind = mc::UvBulk::Infer;
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        mc::Model explicitModel;
        explicitModel.valid = true;
        explicitModel.source = explicitModel.resolved = edit.source();
        const auto after = mc::mesh(explicitModel);
        QCOMPARE(after.faces.size(), before.faces.size());
        for (int i = 0; i < after.faces.size(); ++i)
            for (int k = 0; k < 4; ++k)
                QVERIFY((after.faces[i].uv[k] - before.faces[i].uv[k]).length() < 1e-6f);
        QVERIFY(!mc::uvStates(edit.source(), targets)[0].inferred);
        // Absolute position (U only), shared size, relative offset with fractions.
        op = {};
        op.kind = mc::UvBulk::Position;
        op.value = {2.25, 99};
        op.second = false;
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        states = mc::uvStates(edit.source(), targets);
        for (const auto &s : states)
            QCOMPARE(std::min(s.uv.x(), s.uv.z()), 2.25f);
        QVERIFY(states[0].uv.y() != 99);
        op = {};
        op.kind = mc::UvBulk::Size;
        op.value = {4, 0};
        op.second = false;
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        op = {};
        op.kind = mc::UvBulk::Offset;
        op.value = {0.5, -0.25};
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        states = mc::uvStates(edit.source(), targets);
        for (const auto &s : states) {
            QCOMPARE(s.uv.x(), 2.75f);
            QCOMPARE(s.uv.z() - s.uv.x(), 4.f);
        }
        // Mirror keeps the rectangle; rotation advances per face.
        op = {};
        op.kind = mc::UvBulk::MirrorU;
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        QVERIFY(mc::uvStates(edit.source(), targets)[0].uv.x() > mc::uvStates(edit.source(), targets)[0].uv.z());
        op.kind = mc::UvBulk::Size; // size keeps mirroring direction
        op.value = {2, 0};
        op.second = false;
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        states = mc::uvStates(edit.source(), targets);
        QCOMPARE(states[0].uv.x() - states[0].uv.z(), 2.f);
        op = {};
        op.kind = mc::UvBulk::Rotate;
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        QCOMPARE(mc::uvStates(edit.source(), targets)[2].rotation, 90);
        // Snap uses each face's own texel grid (16 px: 1 unit; 32 px: 0.5 unit).
        op = {};
        op.kind = mc::UvBulk::Offset;
        op.value = {0.65, 0};
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error)); // U edges now 5.4 / 3.4
        op.kind = mc::UvBulk::Snap;
        QVERIFY(edit.bulkUv(targets, op, size, "26.1", &error));
        states = mc::uvStates(edit.source(), targets);
        QCOMPARE(states[0].uv.x(), 5.f);   // 16 px texture: whole units
        QCOMPARE(states[0].uv.z(), 3.f);
        QCOMPARE(states[2].uv.x(), 5.5f);  // 32 px texture: half units
        QCOMPARE(states[2].uv.z(), 3.5f);
        // Unknown face fields survive; commit is one undo step.
        QCOMPARE(edit.source()["elements"].toArray()[1].toObject()["faces"].toObject()["south"].toObject()["face_extension"].toInt(), 17);
        QVERIFY(edit.commitPreview("Bulk UV"));
        QVERIFY(edit.undo());
        QCOMPARE(edit.authoritative(), cubeModel());
    }
    void bulkUvDistributePasteResetAndRollback() {
        mc::ModelEdit edit;
        QVERIFY(edit.create(cubeModel()));
        QString error;
        const QVector<mc::UvTarget> targets{{1, "east"}, {0, "east"}, {0, "east"}};
        mc::UvBulk op;
        op.kind = mc::UvBulk::Size;
        op.value = {3, 3};
        QVERIFY(edit.bulkUv(targets, op, {}, "26.1", &error)); // duplicates collapse
        op = {};
        op.kind = mc::UvBulk::Distribute;
        op.gap = 1;
        QVERIFY(edit.bulkUv(targets, op, {}, "26.1", &error));
        auto states = mc::uvStates(edit.source(), {{0, "east"}, {1, "east"}});
        QCOMPARE(std::abs(std::min(states[1].uv.x(), states[1].uv.z()) -
                          std::min(states[0].uv.x(), states[0].uv.z())),
                 4.f);
        op = {};
        op.kind = mc::UvBulk::Paste;
        op.clipboard = {{"uv", QJsonArray{1, 2, 3.5, 4.5}}, {"rotation", 180}};
        QVERIFY(edit.bulkUv(targets, op, {}, "26.1", &error));
        states = mc::uvStates(edit.source(), {{0, "east"}, {1, "east"}});
        QCOMPARE(states[1].uv, QVector4D(1, 2, 3.5f, 4.5f));
        QCOMPARE(states[1].rotation, 180);
        // Reset returns to inferred.
        op = {};
        op.kind = mc::UvBulk::Reset;
        QVERIFY(edit.bulkUv({{0, "east"}}, op, {}, "26.1", &error));
        QVERIFY(mc::uvStates(edit.source(), {{0, "east"}})[0].inferred);
        // Atomic rollback: one out-of-range target leaves every face unchanged.
        const auto before = edit.source();
        op = {};
        op.kind = mc::UvBulk::Offset;
        op.value = {5000, 0};
        QVERIFY(!edit.bulkUv(targets, op, {}, "26.1", &error));
        QCOMPARE(edit.source(), before);
        QVERIFY(!edit.bulkUv({{7, "east"}}, op, {}, "26.1", &error));
        QVERIFY(!edit.bulkUv({}, op, {}, "26.1", &error));
        op.kind = mc::UvBulk::Size;
        op.value = {-1, 2};
        QVERIFY(!edit.bulkUv(targets, op, {}, "26.1", &error));
        op.kind = mc::UvBulk::Paste;
        op.clipboard = {};
        QVERIFY(!edit.bulkUv(targets, op, {}, "26.1", &error));
        QCOMPARE(edit.source(), before);
    }

    // ================= C8-D native-aware batches =================
    void nativeStaticLayerScopes() {
        QTemporaryDir root;
        const auto nvdoc = makeStatic(root.path(), "assets/nova/textures/block/s.png");
        QVERIFY(!nvdoc.isEmpty());
        const auto pngHash = fileHash(root.path() + "/assets/nova/textures/block/s.png");
        auto original = reopen(root.path(), "assets/nova/textures/block/s.png");
        QVERIFY(original);
        QCOMPARE(int(original->layers().size()), 3);
        batch::NativeOptions o;
        o.transactionRoot = outside.path() + "/static-tx";
        o.layers = batch::LayerScope::Visible;
        auto plan = batch::planNative(invert(), root.path(), {"assets/nova/textures/block/s.png"}, o);
        QCOMPARE(plan.ready, 1);
        QCOMPARE(plan.items[0].targetLayers, (QStringList{"Base", "Shade"}));
        QVERIFY(!QDir(o.transactionRoot).exists()); // dry run writes nothing
        QImage before, after;
        QString error;
        QVERIFY(batch::previewNative(invert(), plan.items[0], root.path(), o, &before, &after, &error));
        QVERIFY(before != after);
        QCOMPARE(fileHash(nvdoc), plan.items[0].sourceRevision.hash);
        std::atomic_bool cancel = false;
        auto report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QCOMPARE(report.done, 1);
        QVERIFY(QFileInfo(report.journal).isFile());
        auto result = reopen(root.path(), "assets/nova/textures/block/s.png");
        QVERIFY(result);
        QCOMPARE(int(result->layers().size()), 3);
        for (int i = 0; i < 3; ++i) {
            const auto &a = original->layers()[size_t(i)], &b = result->layers()[size_t(i)];
            QCOMPARE(b.id, a.id);
            QCOMPARE(b.name, a.name);
            QCOMPARE(b.visible, a.visible);
            QCOMPARE(b.opacity, a.opacity);
            QCOMPARE(b.pixels.pixel(0, 0), a.visible ? inverted(a.pixels.pixel(0, 0)) : a.pixels.pixel(0, 0));
        }
        QCOMPARE(result->activeLayer(), original->activeLayer());
        // The pack PNG is never replaced by a flattened result in native mode.
        QCOMPARE(fileHash(root.path() + "/assets/nova/textures/block/s.png"), pngHash);
        // Active and selected-name scopes.
        o.layers = batch::LayerScope::Active;
        plan = batch::planNative(invert(), root.path(), {"assets/nova/textures/block/s.png"}, o);
        QCOMPARE(plan.items[0].targetLayers, QStringList{"Shade"});
        o.layers = batch::LayerScope::Selected;
        o.layerNames = {"Hidden"};
        plan = batch::planNative(invert(), root.path(), {"assets/nova/textures/block/s.png"}, o);
        QCOMPARE(plan.items[0].targetLayers, QStringList{"Hidden"});
        o.layerNames = {"Nope"};
        plan = batch::planNative(invert(), root.path(), {"assets/nova/textures/block/s.png"}, o);
        QCOMPARE(plan.invalid, 1);
    }
    void nativeAnimationFrameScopes() {
        QTemporaryDir root;
        const QString rel = "assets/nova/textures/block/anim.png";
        QVERIFY(!makeAnimated(root.path(), rel).isEmpty());
        const auto original = reopen(root.path(), rel);
        QVERIFY(original && original->animated());
        const auto state = original->animationState();
        batch::NativeOptions o;
        o.transactionRoot = outside.path() + "/anim-tx";
        o.layers = batch::LayerScope::All;
        o.frames = batch::FrameScope::PlaybackEntries;
        o.playbackIndices = {0, 2, 4}; // all reference frame 0
        auto plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(plan.ready, 1);
        QCOMPARE(plan.items[0].targetFrames, QList<int>{0});
        QVERIFY(plan.items[0].warnings.join(' ').contains("3 playback entries resolve to 1 unique"));
        std::atomic_bool cancel = false;
        QCOMPARE(batch::executeNative(invert(), plan, root.path(), o, {}, &cancel).done, 1);
        auto result = reopen(root.path(), rel);
        const auto next = result->animationState();
        QCOMPARE(next.frames.size(), state.frames.size());
        QCOMPARE(next.entries.size(), state.entries.size());
        for (size_t i = 0; i < next.entries.size(); ++i)
            QCOMPARE(next.entries[i].frame, state.entries[i].frame);
        // Frame 0 inverted exactly once; the other frames untouched.
        QCOMPARE(next.frames[0].layers[0].pixels.pixel(0, 0),
                 inverted(state.frames[0].layers[0].pixels.pixel(0, 0)));
        QCOMPARE(next.frames[1].layers[0].pixels.pixel(0, 0), state.frames[1].layers[0].pixels.pixel(0, 0));
        QCOMPARE(next.frames[0].id, state.frames[0].id);
        QCOMPARE(next.frames[0].layers[0].id, state.frames[0].layers[0].id);
        // Selected frames and current frame
        o.frames = batch::FrameScope::Selected;
        o.frameIndices = {2, 1, 9};
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(plan.items[0].targetFrames, (QList<int>{1, 2}));
        o.frames = batch::FrameScope::Current;
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(plan.items[0].targetFrames.size(), 1);
        // Canvas-changing operations need every frame and layer.
        batch::Pipeline resize;
        resize.steps.push_back({"resize_image", {{"width", 16}, {"height", 16}}});
        plan = batch::planNative(resize, root.path(), {rel}, o);
        QCOMPARE(plan.invalid, 1);
        QVERIFY(plan.items[0].message.contains("All layers"));
        o.frames = batch::FrameScope::All;
        plan = batch::planNative(resize, root.path(), {rel}, o);
        QCOMPARE(plan.ready, 1);
        QCOMPARE(batch::executeNative(resize, plan, root.path(), o, {}, &cancel).done, 1);
        result = reopen(root.path(), rel);
        QCOMPARE(result->image.size(), QSize(16, 16));
        QCOMPARE(result->animationState().frames.size(), state.frames.size());
        // Composite-only is refused for animations (not silently flattened).
        o.layers = batch::LayerScope::CompositeOnly;
        o.exportOutput.directory = outside.path() + "/anim-export";
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(plan.invalid, 1);
    }
    void nativeExportModesAndUnsupportedOperations() {
        QTemporaryDir root;
        const QString rel = "assets/nova/textures/block/e.png";
        const auto nvdoc = makeStatic(root.path(), rel);
        const auto pngHash = fileHash(root.path() + '/' + rel);
        batch::NativeOptions o;
        o.transactionRoot = outside.path() + "/export-tx";
        o.mode = batch::SourceMode::NativeWithExport;
        o.layers = batch::LayerScope::All;
        o.exportOutput.directory = outside.path() + "/export-out";
        auto plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(plan.ready, 1);
        std::atomic_bool cancel = false;
        QCOMPARE(batch::executeNative(invert(), plan, root.path(), o, {}, &cancel).done, 1);
        const auto exported = loadPng(plan.items[0].exportPath);
        QCOMPARE(exported, reopen(root.path(), rel)->image);
        QCOMPARE(fileHash(root.path() + '/' + rel), pngHash); // pack PNG untouched
        // Composite-only: native unchanged, processed composite exported.
        const auto nativeHash = fileHash(nvdoc);
        o.layers = batch::LayerScope::CompositeOnly;
        o.exportOutput.directory = outside.path() + "/composite-out";
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(batch::executeNative(invert(), plan, root.path(), o, {}, &cancel).done, 1);
        QCOMPARE(fileHash(nvdoc), nativeHash);
        QVERIFY(QFileInfo(plan.items[0].exportPath).isFile());
        // Existing export destination conflicts unless overwrite.
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(plan.conflicts, 1);
        // Exported-PNG mode and outputs inside the pack are refused here.
        o.exportOutput.directory = root.path() + "/inside";
        QCOMPARE(batch::planNative(invert(), root.path(), {rel}, o).invalid, 1);
        o.mode = batch::SourceMode::ExportedPng;
        QCOMPARE(batch::planNative(invert(), root.path(), {rel}, o).invalid, 1);
        o.mode = batch::SourceMode::Native;
        o.layers = batch::LayerScope::All;
        o.transactionRoot = root.path() + "/tx";
        QCOMPARE(batch::planNative(invert(), root.path(), {rel}, o).invalid, 1);
        o.transactionRoot = outside.path() + "/export-tx";
        for (const auto &id : {"gen_stone", "gaussian_blur", "trim"}) {
            batch::Pipeline p;
            p.steps.push_back({id, {}});
            QCOMPARE(batch::planNative(p, root.path(), {rel}, o).invalid, 1);
        }
    }
    void nativePlanRejectsInvalidMissingDuplicateBlockedAndOverBudget() {
        QTemporaryDir root;
        const QString a = "assets/nova/textures/block/a.png", b = "assets/nova/textures/block/b.png",
                      c = "assets/nova/textures/block/c.png";
        QVERIFY(!makeStatic(root.path(), a).isEmpty());
        const auto broken = makeStatic(root.path(), b);
        QFile f(broken);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("NVLD0001 not a native document");
        f.close();
        QVERIFY(savePng(root.path() + '/' + c, solid(qRgba(1, 1, 1, 255)))); // no native source
        batch::NativeOptions o;
        o.transactionRoot = outside.path() + "/plan-tx";
        o.blocked = {root.path() + "/assets/nova/textures/block/a.png"};
        auto plan = batch::planNative(invert(), root.path(), {a, a, b, c, "assets/missing.png"}, o);
        QCOMPARE(plan.items[0].status, batch::NativeItem::Status::Conflict); // open with edits
        QCOMPARE(plan.items[1].status, batch::NativeItem::Status::Conflict); // duplicate
        QVERIFY(plan.items[1].message.contains("Duplicate"));
        QCOMPARE(plan.items[2].status, batch::NativeItem::Status::Invalid);
        QVERIFY(plan.items[2].message.contains("Invalid native document"));
        QCOMPARE(plan.items[3].status, batch::NativeItem::Status::Invalid);
        QCOMPARE(plan.items[4].status, batch::NativeItem::Status::Invalid);
        o.blocked.clear();
        o.memoryBudget = 1024;
        plan = batch::planNative(invert(), root.path(), {a}, o);
        QCOMPARE(plan.invalid, 1);
        QVERIFY(plan.items[0].message.contains("memory budget"));
    }

    // ================= C8-E transactions and recovery =================
    void nativeFailuresPreserveOriginals() {
        QTemporaryDir root;
        const QString rel = "assets/nova/textures/block/f.png";
        const auto nvdoc = makeStatic(root.path(), rel);
        const auto originalHash = fileHash(nvdoc);
        batch::NativeOptions o;
        o.transactionRoot = outside.path() + "/fail-tx";
        std::atomic_bool cancel = false;
        // Simulated disk-full / write failure at the replacement.
        o.fault = [](const QString &stage, int) { return stage == "commit"; };
        auto plan = batch::planNative(invert(), root.path(), {rel}, o);
        auto report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QCOMPARE(report.failed, 1);
        QCOMPARE(fileHash(nvdoc), originalHash);
        // Encode/verification failure before writing.
        o.fault = [](const QString &stage, int) { return stage == "encode"; };
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(batch::executeNative(invert(), plan, root.path(), o, {}, &cancel).failed, 1);
        QCOMPARE(fileHash(nvdoc), originalHash);
        // Post-write verification failure restores the original.
        o.fault = [](const QString &stage, int) { return stage == "verify"; };
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QCOMPARE(batch::executeNative(invert(), plan, root.path(), o, {}, &cancel).rolledBack, 1);
        QCOMPARE(fileHash(nvdoc), originalHash);
        o.fault = {};
        // External modification after the dry run.
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        {
            auto doc = reopen(root.path(), rel);
            QVERIFY(doc->applyOperation(doc->activeLayer(), {"grayscale", {}}));
            QVERIFY(doc->saveNative(root.path()));
        }
        const auto externalHash = fileHash(nvdoc);
        report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QCOMPARE(report.failed, 1);
        QVERIFY(plan.items[0].message.contains("changed after the dry run"));
        QCOMPARE(fileHash(nvdoc), externalHash);
        // Read-only destination.
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        QVERIFY(QFile::setPermissions(nvdoc, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QFile::setPermissions(nvdoc, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ReadUser | QFileDevice::WriteUser);
        QCOMPARE(report.failed, 1);
        QCOMPARE(fileHash(nvdoc), externalHash);
        // Cancellation before processing.
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        cancel = true;
        report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QCOMPARE(report.canceled, 1);
        QCOMPARE(fileHash(nvdoc), externalHash);
    }
    void interruptedRunsRecoverIdempotently() {
        QTemporaryDir root;
        const QString rel = "assets/nova/textures/block/r.png";
        const auto nvdoc = makeStatic(root.path(), rel);
        const auto originalHash = fileHash(nvdoc);
        batch::NativeOptions o;
        o.transactionRoot = outside.path() + "/recover-tx";
        std::atomic_bool cancel = false;
        // Interrupted after staging: the original was never replaced.
        o.fault = [](const QString &stage, int) { return stage == "interrupt"; };
        auto plan = batch::planNative(invert(), root.path(), {rel}, o);
        auto report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QVERIFY(report.interrupted);
        QVERIFY(batch::pendingNativeJournals(o.transactionRoot).contains(report.journal));
        auto inspect = batch::recoverNative(report.journal, true, false);
        QVERIFY2(inspect.ok, qPrintable(inspect.error));
        QCOMPARE(inspect.unchanged, 1);
        QCOMPARE(batch::recoverNative(report.journal, true, true).unchanged, 1);
        QCOMPARE(batch::recoverNative(report.journal, true, true).unchanged, 1); // idempotent
        QCOMPARE(fileHash(nvdoc), originalHash);
        // Interrupted after the verified replacement: roll back restores the original copy.
        o.fault = [](const QString &stage, int) { return stage == "interrupt-after-commit"; };
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QVERIFY(report.interrupted);
        QVERIFY(fileHash(nvdoc) != originalHash);
        inspect = batch::recoverNative(report.journal, true, false);
        QCOMPARE(inspect.restored, 1);
        QVERIFY(fileHash(nvdoc) != originalHash); // inspection writes nothing
        auto applied = batch::recoverNative(report.journal, true, true);
        QCOMPARE(applied.restored, 1);
        QCOMPARE(fileHash(nvdoc), originalHash);
        QCOMPARE(batch::recoverNative(report.journal, true, true).unchanged, 1);
        // Completed run: confirm, then roll back; an external edit afterwards is never overwritten.
        o.fault = {};
        plan = batch::planNative(invert(), root.path(), {rel}, o);
        report = batch::executeNative(invert(), plan, root.path(), o, {}, &cancel);
        QCOMPARE(report.done, 1);
        QVERIFY(!batch::pendingNativeJournals(o.transactionRoot).contains(report.journal));
        QCOMPARE(batch::recoverNative(report.journal, false, true).completed, 1);
        {
            auto doc = reopen(root.path(), rel);
            QVERIFY(doc->applyOperation(doc->activeLayer(), {"grayscale", {}}));
            QVERIFY(doc->saveNative(root.path()));
        }
        const auto edited = fileHash(nvdoc);
        auto conflict = batch::recoverNative(report.journal, true, true);
        QCOMPARE(conflict.conflicts, 1);
        QCOMPARE(fileHash(nvdoc), edited);
        // Corrupted journals are refused without changes.
        QFile journal(report.journal);
        QVERIFY(journal.open(QIODevice::ReadOnly));
        auto bytes = journal.readAll();
        journal.close();
        auto tampered = bytes;
        tampered.replace("\"done\"", "\"staged\"");
        QVERIFY(tampered != bytes);
        QVERIFY(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        journal.write(tampered);
        journal.close();
        auto refused = batch::recoverNative(report.journal, true, true);
        QVERIFY(!refused.ok);
        QVERIFY(refused.error.contains("checksum"));
        QVERIFY(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        journal.write("{ not json");
        journal.close();
        QVERIFY(!batch::recoverNative(report.journal, true, true).ok);
        QVERIFY(!batch::recoverNative(outside.path() + "/missing.json", true, true).ok);
        QCOMPARE(fileHash(nvdoc), edited);
    }
    void processedDocumentsRemainCompatible() {
        QTemporaryDir root;
        const QString rel = "assets/nova/textures/block/c.png";
        const auto nvdoc = makeStatic(root.path(), rel);
        batch::Pipeline p;
        p.steps.push_back({"hue_saturation", {{"hue", 30}}});
        p.steps.push_back({"levels", {}});
        p.steps.push_back({"image_flip", {}});
        batch::NativeOptions o;
        o.transactionRoot = outside.path() + "/compat-tx";
        o.layers = batch::LayerScope::All;
        auto plan = batch::planNative(p, root.path(), {rel}, o);
        QCOMPARE(plan.ready, 1);
        std::atomic_bool cancel = false;
        QCOMPARE(batch::executeNative(p, plan, root.path(), o, {}, &cancel).done, 1);
        // Reopens through the normal editor path; undo history starts clean; re-save works.
        auto doc = reopen(root.path(), rel);
        QVERIFY(doc);
        QVERIFY(doc->nativeDocument());
        QVERIFY(!doc->modified());
        QVERIFY(doc->exportModified()); // pack PNG is now export pending, not silently replaced
        QVERIFY(doc->saveNative(root.path()));
        QVERIFY(QFileInfo(nvdoc).isFile());
    }
};
QTEST_MAIN(C8Test)
#include "c8_test.moc"
