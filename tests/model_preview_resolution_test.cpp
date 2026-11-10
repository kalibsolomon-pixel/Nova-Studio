// SPDX-License-Identifier: GPL-3.0-or-later
// Which Minecraft model the 3D preview shows for a texture, on the original fixtures in
// fixtures/model-preview (pack = editable resource pack, stack = lower-priority resource
// folder with synthetic block/cube_all, item/generated and item/handheld parents).
// Covers lookup and namespaces, parent and texture-variable inheritance, elements, UVs,
// rotations, display transforms, generated/handheld items, pack precedence and the
// explicit no-model reasons (missing, invalid JSON, parent cycle, missing parent).
#include "display.h"
#include "models.h"
#include <QtTest>
using namespace nova;

class ModelPreviewResolutionTests : public QObject {
    Q_OBJECT
    QString pack, stack;
    std::shared_ptr<mc::Library> library;

    const mc::Face *face(const mc::Mesh &m, const QString &name, int element = 0) {
        for (const auto &f : m.faces)
            if (f.name == name && f.elementIndex == element)
                return &f;
        return nullptr;
    }

  private slots:
    void initTestCase() {
        pack = QFINDTESTDATA("../fixtures/model-preview/pack");
        stack = QFINDTESTDATA("../fixtures/model-preview/stack");
        QVERIFY(!pack.isEmpty() && !stack.isEmpty());
        library = std::make_shared<mc::Library>();
        library->update(pack, Index::scan(pack));
        QString error;
        auto source = mc::ResourceSource::open(stack, library->version(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        library->setFallbacks({source});
    }
    void cubeThroughParentsAndTextureVariables() {
        const auto choice = mc::previewModelFor(*library, "assets/minecraft/textures/block/fx_cube.png");
        QCOMPARE(choice.model, QString("minecraft:block/fx_cube"));
        QVERIFY(choice.reason.isEmpty());
        const auto model = library->model(choice.model);
        QVERIFY(model.valid);
        // fx_cube -> cube_all -> cube -> block; #north -> #all -> the pack texture.
        // Parents are listed from the root down.
        QCOMPARE(model.parents, QStringList({"minecraft:block/block", "minecraft:block/cube", "minecraft:block/cube_all"}));
        QCOMPARE(model.textures.value("north"), QString("assets/minecraft/textures/block/fx_cube.png"));
        const auto m = mc::mesh(model);
        QCOMPARE(m.faces.size(), 6);
        for (const auto &f : m.faces) {
            QCOMPARE(f.texture, QString("assets/minecraft/textures/block/fx_cube.png"));
            QCOMPARE(f.cullface, f.name);
            QCOMPARE(f.uvRect(), QRectF(0, 0, 16, 16)); // default UVs of a full cube face
        }
        // GUI display comes from the stack's block/block parent.
        const auto gui = mc::resolveDisplay(model, "gui");
        QCOMPARE(gui.origin, QString("minecraft:block/block"));
        QCOMPARE(gui.value.rotation, (std::array<double, 3>{30, 225, 0}));
        QCOMPARE(gui.value.scale, (std::array<double, 3>{0.6, 0.6, 0.6}));
    }
    void elementsUvsAndRotations() {
        const auto choice = mc::previewModelFor(*library, "assets/minecraft/textures/block/fx_slab.png");
        QCOMPARE(choice.model, QString("minecraft:block/fx_slab"));
        const auto model = library->model(choice.model);
        const auto m = mc::mesh(model);
        QCOMPARE(m.faces.size(), 9); // 6 slab faces + 3 faces on the rotated post
        auto north = face(m, "north");
        QVERIFY(north);
        QCOMPARE(north->uvRect(), QRectF(0, 8, 16, 8));
        QCOMPARE(face(m, "up")->rotation, 90);
        QCOMPARE(face(m, "up")->texture, QString("assets/minecraft/textures/block/fx_slab.png")); // #top -> #side
        // The slab (y 0..8) is 8/14 of the model's height (the post reaches y 14).
        float top = -1e9f, bottom = 1e9f;
        for (const auto &f : m.faces)
            if (f.elementIndex == 0)
                for (const auto &p : f.points) {
                    top = std::max(top, p.y());
                    bottom = std::min(bottom, p.y());
                }
        QVERIFY(std::abs((top - bottom) / (m.maximum.y() - m.minimum.y()) - 8.f / 14.f) < 1e-3f);
        // The post is rotated 22.5° about y: its north face is no longer axis-aligned.
        auto post = face(m, "north", 1);
        QVERIFY(post);
        QCOMPARE(post->uvRect(), QRectF(6, 2, 4, 6));
        QVERIFY(std::abs(post->normal.x()) > 0.3f && std::abs(post->normal.z()) > 0.8f);
    }
    void generatedAndHandheldItems() {
        const auto gem = mc::previewModelFor(*library, "assets/minecraft/textures/item/fx_gem.png");
        QVERIFY2(gem.model == "minecraft:item/fx_gem", qPrintable(gem.reason));
        // Regression: resolving item/generated must not leave "elements": null behind for
        // its children (that made every generated/handheld child invalid).
        QVERIFY(!library->model("minecraft:item/generated").resolved.contains("elements"));
        const auto model = library->model(gem.model);
        QVERIFY(model.generated);
        QCOMPARE(model.parents, QStringList({"minecraft:builtin/generated", "minecraft:item/generated"}));
        // A flat sprite, not a cube: generated geometry has (near) zero depth.
        QImage sprite(QString(pack + "/assets/minecraft/textures/item/fx_gem.png"));
        const auto m = mc::generatedMesh(model, {{"assets/minecraft/textures/item/fx_gem.png", sprite}});
        QVERIFY(!m.faces.isEmpty());
        QVERIFY(m.maximum.z() - m.minimum.z() < 0.2f * (m.maximum.x() - m.minimum.x()));
        const auto sword = library->model(mc::previewModelFor(*library, "assets/minecraft/textures/item/fx_sword.png").model);
        QVERIFY(sword.valid && sword.generated);
        // Hand view from item/handheld; GUI view inherited from item/generated.
        const auto hand = mc::resolveDisplay(sword, "thirdperson_righthand");
        QCOMPARE(hand.origin, QString("minecraft:item/handheld"));
        QCOMPARE(hand.value.rotation, (std::array<double, 3>{0, -90, 50}));
        const auto left = mc::resolveDisplay(sword, "thirdperson_lefthand");
        QCOMPARE(left.value.rotation, (std::array<double, 3>{0, -90, 50})); // reused from the right hand
        QCOMPARE(mc::resolveDisplay(sword, "gui").origin, QString("minecraft:item/generated"));
        QVERIFY(!mc::displayTransform(sword, "thirdperson_righthand").isIdentity());
        QCOMPARE(mc::resolveDisplay(library->model("minecraft:item/fx_gem"), "thirdperson_righthand").origin,
                 QString("minecraft:item/generated"));
    }
    void packOverridesTheResourceStack() {
        const QString path = "assets/minecraft/models/block/fx_override.json";
        QCOMPARE(library->providers(path).size(), 2);
        QVERIFY(library->editable(path));
        const auto choice = mc::previewModelFor(*library, "assets/minecraft/textures/block/fx_override.png");
        QCOMPARE(choice.model, QString("minecraft:block/fx_override"));
        const auto m = mc::mesh(library->model(choice.model));
        QCOMPARE(m.faces.size(), 6);
        // The pack's 4-high slab wins over the stack's full cube.
        QVERIFY(m.maximum.y() - m.minimum.y() < 0.5f * (m.maximum.x() - m.minimum.x()));
        // Without the pack's file, the stack's cube_all version applies (fallback order).
        mc::Library stackOnly;
        QTemporaryDir empty;
        stackOnly.update(empty.path(), Index::scan(empty.path()));
        QString error;
        stackOnly.setFallbacks({mc::ResourceSource::open(stack, stackOnly.version(), &error)});
        const auto fallback = mc::mesh(stackOnly.model("minecraft:block/fx_override"));
        QCOMPARE(fallback.faces.size(), 6);
        QVERIFY(qFuzzyCompare(fallback.maximum.y() - fallback.minimum.y(), fallback.maximum.x() - fallback.minimum.x()));
    }
    void explicitReasonsWhenNoModelApplies() {
        // No model at all.
        auto orphan = mc::previewModelFor(*library, "assets/minecraft/textures/block/fx_orphan.png");
        QVERIFY(orphan.model.isEmpty());
        QCOMPARE(orphan.reason, QString("No model in the resource pack or resource stack uses this texture"));
        // Invalid JSON in the model named like the texture.
        auto broken = mc::previewModelFor(*library, "assets/minecraft/textures/block/fx_broken.png");
        QVERIFY(broken.model.isEmpty());
        QVERIFY2(broken.reason.startsWith("minecraft:block/fx_broken could not be resolved: "), qPrintable(broken.reason));
        // Parent cycle.
        auto cycle = mc::previewModelFor(*library, "assets/minecraft/textures/block/fx_cycle.png");
        QVERIFY(cycle.model.isEmpty());
        QVERIFY2(cycle.reason.contains("Cyclic or excessive parent chain"), qPrintable(cycle.reason));
        // Missing parent.
        auto lost = mc::previewModelFor(*library, "assets/minecraft/textures/block/fx_lost.png");
        QVERIFY(lost.model.isEmpty());
        QVERIFY2(lost.reason.contains("Missing parent/model: minecraft:block/fx_no_such_parent"), qPrintable(lost.reason));
        // Not a pack texture.
        auto outside = mc::previewModelFor(*library, "textures/loose.png");
        QVERIFY(outside.model.isEmpty());
        QVERIFY(outside.reason.startsWith("Not a resource-pack texture"));
        // Without the stack, fx_cube's parents are missing: no model, with the reason.
        mc::Library packOnly;
        packOnly.update(pack, Index::scan(pack));
        auto noParents = mc::previewModelFor(packOnly, "assets/minecraft/textures/block/fx_cube.png");
        QVERIFY(noParents.model.isEmpty());
        QVERIFY2(noParents.reason.contains("Missing parent/model: minecraft:block/cube_all"), qPrintable(noParents.reason));
    }
    void namespacesAndPreferenceOrder() {
        // The model named like the texture wins over other users of the same texture.
        mc::Library lib;
        QTemporaryDir dir;
        auto put = [&](const QString &path, const QByteArray &bytes) {
            QDir().mkpath(QFileInfo(dir.path() + '/' + path).absolutePath());
            QFile f(dir.path() + '/' + path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        };
        const QByteArray flat = R"({"textures":{"t":"demo:block/oak"},"elements":[{"from":[0,0,0],"to":[16,1,16],"faces":{"up":{"texture":"#t"}}}]})";
        put("assets/demo/models/block/oak_button.json", flat);
        put("assets/demo/models/block/a_oak_fence.json", flat);
        put("assets/demo/models/block/oak.json", flat);
        put("assets/demo/models/item/oak.json", R"({"parent":"demo:block/oak"})");
        put("assets/demo/textures/block/oak.png", QByteArray());
        lib.update(dir.path(), Index::scan(dir.path()));
        const auto choice = mc::previewModelFor(lib, "assets/demo/textures/block/oak.png");
        QCOMPARE(choice.model, QString("demo:block/oak"));
        QCOMPARE(choice.candidates.value(1), QString("demo:item/oak"));
        QCOMPARE(choice.candidates.size(), 4);
        // A texture in another namespace is not matched by name across namespaces.
        put("assets/other/textures/block/oak.png", QByteArray());
        lib.update(dir.path(), Index::scan(dir.path()));
        QVERIFY(mc::previewModelFor(lib, "assets/other/textures/block/oak.png").model.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ModelPreviewResolutionTests)
#include "model_preview_resolution_test.moc"
