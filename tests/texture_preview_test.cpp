// SPDX-License-Identifier: GPL-3.0-or-later
// Universal 3D Minecraft texture preview tests: armor worn on a standing player
// (outer, leggings, trims, companion layer, neutral mannequin fallback), entity-worn
// equipment (elytra, horse armor, wolf armor, llama decor, saddles), player skins,
// mobs, block entities, paintings, extruded items, fluids, manual templates, and
// real OpenGL ModelPanel rendering with live editing and UV overlays.
#include "modelpanel.h"
#include "texture_preview.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace nova;

namespace {
QImage solidImage(int w, int h, QColor c) {
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(c);
    return img;
}

void verifyValidMesh(const mc::Mesh &mesh, const QString &activeTexture, const char *label) {
    QVERIFY2(!mesh.faces.isEmpty(), label);
    bool hasActiveFace = false;
    for (const auto &face : mesh.faces) {
        QVERIFY2(!face.name.isEmpty(), label);
        QVERIFY2(!face.element.isEmpty(), label);
        QVERIFY2(!face.texture.isEmpty(), label);
        QVERIFY2(std::abs(face.normal.length() - 1.f) < 1e-3f, label);
        QVERIFY2(std::abs(face.lightNormal.length() - 1.f) < 1e-3f, label);
        for (const auto &p : face.points) {
            QVERIFY2(std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z()), label);
        }
        for (const auto &uv : face.uv) {
            QVERIFY2(std::isfinite(uv.x()) && std::isfinite(uv.y()), label);
        }
        if (face.texture == activeTexture)
            hasActiveFace = true;
    }
    QVERIFY2(hasActiveFace, label);
    const auto span = mesh.maximum - mesh.minimum;
    QVERIFY2(span.x() > 0.f || span.y() > 0.f || span.z() > 0.f, label);
}
} // namespace

class TexturePreviewTests : public QObject {
    Q_OBJECT

  private slots:
    void detectsMinecraftTextureCategories() {
        // Humanoid armor (modern 1.21.4+/26.1+ and legacy paths)
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/humanoid/diamond.png").id,
                 QString("armor_humanoid_outer"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/humanoid_leggings/diamond.png").id,
                 QString("armor_humanoid_leggings"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/models/armor/iron_layer_1.png").id,
                 QString("armor_humanoid_outer"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/models/armor/iron_layer_2.png").id,
                 QString("armor_humanoid_leggings"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/trims/entity/humanoid/coast.png").id,
                 QString("armor_trim_outer"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/trims/entity/humanoid_leggings/coast.png").id,
                 QString("armor_trim_leggings"));
        // Entity-worn equipment
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/wings/elytra.png").id,
                 QString("elytra"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/elytra.png").id,
                 QString("elytra"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/horse_body/diamond.png").id,
                 QString("horse_armor"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/wolf_body/armadillo.png").id,
                 QString("wolf_armor"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/llama_body/red.png").id,
                 QString("llama_decor"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/pig_saddle/saddle.png").id,
                 QString("pig_saddle"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/strider_saddle/saddle.png").id,
                 QString("strider_saddle"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/camel_saddle/saddle.png").id,
                 QString("camel_saddle"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/equipment/happy_ghast_body/blue.png").id,
                 QString("happy_ghast_harness"));
        // Players, block entities, mobs, paintings, items, fluids
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/player/wide/steve.png").id,
                 QString("player_wide"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/player/slim/alex.png").id,
                 QString("player_slim"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/chest/normal.png").id,
                 QString("chest"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/chest/normal_left.png").id,
                 QString("chest_double"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/bed/red.png").id,
                 QString("bed"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/creeper/creeper.png").id,
                 QString("creeper"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/creeper/creeper_armor.png").id,
                 QString("creeper_armor"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/illager/ravager.png").id,
                 QString("ravager"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/illager/vex.png").id,
                 QString("allay"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/entity/illager/pillager.png").id,
                 QString("villager"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/painting/kebab.png").id,
                 QString("painting"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/item/custom_gem.png").id,
                 QString("item_extruded"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/block/water_still.png").id,
                 QString("fluid_block"));
        QCOMPARE(mc::detectTexturePreview("assets/minecraft/textures/block/fire_0.png").id,
                 QString("flame_cross"));
        // CRITICAL CONTRACT: ordinary/orphan block textures stay empty so JSON models & no-model tests stay intact.
        QVERIFY(mc::detectTexturePreview("assets/minecraft/textures/block/fx_orphan.png").id.isEmpty());
        QVERIFY(mc::detectTexturePreview("assets/demo/textures/block/stone.png").id.isEmpty());
        QVERIFY(mc::detectTexturePreview("textures/loose.png").id.isEmpty());
    }

    void armorWornOnPlayerIncludesMannequinAndCompanionLayer() {
        QTemporaryDir tmp;
        const QString outer = "assets/minecraft/textures/entity/equipment/humanoid/diamond.png";
        const QString inner = "assets/minecraft/textures/entity/equipment/humanoid_leggings/diamond.png";
        QVERIFY(QDir().mkpath(tmp.filePath("assets/minecraft/textures/entity/equipment/humanoid")));
        QVERIFY(QDir().mkpath(tmp.filePath("assets/minecraft/textures/entity/equipment/humanoid_leggings")));
        const auto outerImg = solidImage(64, 32, QColor(40, 210, 220));
        const auto innerImg = solidImage(64, 32, QColor(30, 175, 190));
        QVERIFY(outerImg.save(tmp.filePath(outer)));
        QVERIFY(innerImg.save(tmp.filePath(inner)));

        // Without library: synthesizes neutral player mannequin + outer armor layer.
        const auto standalone = mc::buildTexturePreview(outer, outerImg);
        QVERIFY(standalone.valid());
        verifyValidMesh(standalone.mesh, outer, "standalone outer armor");
        QVERIFY(standalone.syntheticTextures.contains("nova:synthetic/mannequin_humanoid"));
        QVERIFY(standalone.referencedAssets.isEmpty());
        // Standing player height is ~2 blocks (4 mesh units), feet at y = -1.
        QVERIFY(standalone.mesh.minimum.y() < -0.9f && standalone.mesh.maximum.y() > 2.8f);

        // With library: automatically picks up the companion leggings layer!
        mc::Library lib("26.1");
        lib.update(tmp.path(), Index::scan(tmp.path()));
        const auto paired = mc::buildTexturePreview(outer, outerImg, &lib);
        QVERIFY(paired.valid());
        verifyValidMesh(paired.mesh, outer, "paired outer armor");
        QVERIFY(paired.referencedAssets.contains(inner));
        QVERIFY(paired.mesh.faces.size() > standalone.mesh.faces.size());

        // Viewing leggings also picks up the companion outer armor layer.
        const auto leggings = mc::buildTexturePreview(inner, innerImg, &lib);
        QVERIFY(leggings.valid());
        verifyValidMesh(leggings.mesh, inner, "paired leggings armor");
        QVERIFY(leggings.referencedAssets.contains(outer));
    }

    void legacyPlayerSkinAndPaintingsAndAllManualTemplates() {
        // Legacy 64x32 skin omits 64x64 outer body layers (keeps only hat + 6 inner boxes = 42 faces)
        const auto legacy = mc::buildTexturePreview(
            "assets/minecraft/textures/entity/steve.png", solidImage(64, 32, Qt::green));
        const auto modern = mc::buildTexturePreview(
            "assets/minecraft/textures/entity/player/wide/steve.png", solidImage(64, 64, Qt::green));
        QVERIFY(legacy.valid() && modern.valid());
        QVERIFY(modern.mesh.faces.size() > legacy.mesh.faces.size());

        // Wide 2:1 painting preserves aspect ratio and includes wooden back.
        const auto painting = mc::buildTexturePreview(
            "assets/minecraft/textures/painting/creebet.png", solidImage(32, 16, Qt::yellow));
        QVERIFY(painting.valid());
        verifyValidMesh(painting.mesh, "assets/minecraft/textures/painting/creebet.png", "painting");
        QVERIFY(painting.syntheticTextures.contains("nova:synthetic/painting_back"));
        const auto span = painting.mesh.maximum - painting.mesh.minimum;
        QVERIFY(span.x() > span.y() * 1.8f);
        QVERIFY(span.z() < 0.25f);

        // Every curated manual template builds valid 3D geometry.
        const auto templates = mc::manualTexturePreviews();
        QVERIFY(templates.size() >= 30);
        const QString tex = "assets/nova/textures/entity/custom.png";
        const auto img = solidImage(64, 64, QColor(120, 180, 210));
        for (const auto &spec : templates) {
            const auto res = mc::buildTexturePreview(tex, img, nullptr, spec.id);
            QVERIFY2(res.valid(), qPrintable(spec.id));
            verifyValidMesh(res.mesh, tex, qPrintable(spec.id));
        }
    }

    void modelPanelAutoPreviewsArmorInOpenGL() {
        QTemporaryDir tmp;
        const QString pack = tmp.filePath("pack");
        const QString diamondOuter = "assets/minecraft/textures/entity/equipment/humanoid/diamond.png";
        const QString diamondInner = "assets/minecraft/textures/entity/equipment/humanoid_leggings/diamond.png";
        const QString goldOuter = "assets/minecraft/textures/entity/equipment/humanoid/gold.png";
        QVERIFY(QDir().mkpath(pack + "/assets/minecraft/textures/entity/equipment/humanoid"));
        QVERIFY(QDir().mkpath(pack + "/assets/minecraft/textures/entity/equipment/humanoid_leggings"));
        QVERIFY(solidImage(64, 32, QColor(30, 220, 230)).save(pack + '/' + diamondOuter));
        QVERIFY(solidImage(64, 32, QColor(20, 170, 200)).save(pack + '/' + diamondInner));
        QVERIFY(solidImage(64, 32, QColor(240, 200, 30)).save(pack + '/' + goldOuter));

        qApp->setProperty("settingsFile", tmp.filePath("studio.ini"));
        Metrics metrics;
        DocumentSession session;
        session.setProject(pack);
        auto library = std::make_shared<mc::Library>("26.1");
        library->update(pack, Index::scan(pack));

        QWidget window;
        auto *layout = new QHBoxLayout(&window);
        auto *canvas = new Canvas(std::make_shared<Document>(QString(), solidImage(16, 16, Qt::transparent)), &metrics);
        auto *cube = new Cube(&metrics);
        auto *panel = new ModelPanel(cube, &session);
        layout->addWidget(canvas, 2);
        layout->addWidget(cube, 3);
        layout->addWidget(panel, 2);
        panel->setLibrary(library, pack);
        window.resize(1100, 620);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(120);
        QVERIFY(cube->isValid());

        auto openAndShow = [&](const QString &asset) -> std::shared_ptr<Document> {
            QString error;
            auto doc = session.open(pack + '/' + asset, &error);
            if (!doc)
                qWarning() << error;
            canvas->document = doc;
            cube->setDocument(doc);
            panel->setDocument(doc, canvas);
            QTest::qWait(100);
            return doc;
        };

        // Opening diamond outer armor auto-selects 3D worn armor on player with no fallback warning.
        auto docDiamond = openAndShow(diamondOuter);
        QVERIFY(docDiamond);
        QCOMPARE(panel->selectedTarget(), QString("preview:armor_humanoid_outer"));
        QVERIFY(cube->notice().isEmpty());
        QVERIFY(cube->meshData().faces.size() >= 78); // player mannequin + outer armor + companion leggings
        QVERIFY(panel->previewMaterials().contains(diamondOuter));
        QVERIFY(panel->previewMaterials().contains(diamondInner));
        // UV regions for the armor texture are populated on the 2D canvas.
        QVERIFY(!canvas->uvRegions.isEmpty());
        // Real OpenGL framebuffer shows cyan diamond armor pixels.
        const auto fbDiamond = cube->grabFramebuffer();
        int cyanHits = 0;
        for (int y = 0; y < fbDiamond.height(); y += 2)
            for (int x = 0; x < fbDiamond.width(); x += 2) {
                const QColor c = fbDiamond.pixelColor(x, y);
                if (c.blue() > 90 && c.green() > 90 && c.red() < 60)
                    ++cyanHits;
            }
        QVERIFY2(cyanHits > 100, qPrintable(QString("expected cyan armor pixels, got %1").arg(cyanHits)));

        // Switching to gold outer armor (same preview id, different texture) rebuilds for gold!
        auto docGold = openAndShow(goldOuter);
        QVERIFY(docGold);
        QCOMPARE(panel->selectedTarget(), QString("preview:armor_humanoid_outer"));
        QVERIFY(cube->notice().isEmpty());
        QVERIFY(panel->previewMaterials().contains(goldOuter));
        QVERIFY(!panel->previewMaterials().contains(diamondInner));
        const auto fbGold = cube->grabFramebuffer();
        int goldHits = 0;
        for (int y = 0; y < fbGold.height(); y += 2)
            for (int x = 0; x < fbGold.width(); x += 2) {
                const QColor c = fbGold.pixelColor(x, y);
                if (c.red() > 90 && c.green() > 70 && c.blue() < 40)
                    ++goldHits;
            }
        QVERIFY2(goldHits > 100, qPrintable(QString("expected gold armor pixels, got %1").arg(goldHits)));
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    TexturePreviewTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "texture_preview_test.moc"
