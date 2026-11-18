// SPDX-License-Identifier: GPL-3.0-or-later
// 3D preview regression on fixtures/model-preview: the panel shows the texture's resolved
// Minecraft model (block, slab with rotated element, generated and handheld items), and a
// texture without a usable model is shown flat with a visible, named fallback reason -
// never silently as a cube. The texture stays editable throughout. With NOVA_EVIDENCE_DIR
// set, each case is captured from the real OpenGL view.
#include "assets.h"
#include "modelpanel.h"
#include "display.h"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
using namespace nova;

class ModelPreviewGuiTests : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;
    QString pack, stack, evidence;
    Metrics metrics;
    DocumentSession session;
    std::shared_ptr<mc::Library> library;
    QWidget *window = nullptr;
    Canvas *canvas = nullptr;
    Cube *cube = nullptr;
    ModelPanel *panel = nullptr;

    std::shared_ptr<Document> show(const QString &texture) {
        QString error;
        auto doc = session.open(pack + "/assets/minecraft/textures/" + texture, &error);
        if (!doc)
            qWarning() << error;
        canvas->document = doc;
        cube->setDocument(doc);
        panel->setDocument(doc, canvas);
        QTest::qWait(120);
        return doc;
    }
    void capture(const QString &name) {
        if (evidence.isEmpty())
            return;
        QDir().mkpath(evidence);
        QVERIFY(cube->grab().save(evidence + "/preview-" + name + ".png")); // includes the notice
        QVERIFY(window->grab().save(evidence + "/preview-panel-" + name + ".png"));
    }
    QString target() { return panel->findChild<QComboBox *>("modelTarget")->currentData().toString(); }
    // Share of the view showing the texture's hue (not the dark background or the banner).
    double coverage(QRgb texture) {
        const auto image = cube->grabFramebuffer();
        int hits = 0, total = 0;
        for (int y = image.height() / 4; y < image.height() * 3 / 4; y += 2)
            for (int x = image.width() / 4; x < image.width() * 3 / 4; x += 2, ++total) {
                const QColor c = image.pixelColor(x, y), t = QColor::fromRgb(texture);
                hits += std::abs(c.hueF() - t.hueF()) < 0.08 && c.saturationF() > 0.25;
            }
        return double(hits) / std::max(1, total);
    }

    void prepareLayeredItem(int baseAlpha, int overlayAlpha, bool missingOverlay) {
        pack = tmp.filePath(QString("layered-%1-%2-%3").arg(baseAlpha).arg(overlayAlpha).arg(missingOverlay));
        const auto models = pack + "/assets/minecraft/models/item";
        const auto textures = pack + "/assets/minecraft/textures/item";
        QVERIFY(QDir().mkpath(models));
        QVERIFY(QDir().mkpath(textures));
        QFile file(models + "/layers.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"parent\":\"minecraft:item/handheld\",\"textures\":{\"layer0\":\"minecraft:item/layers\",\"layer1\":\"minecraft:item/overlay\"}}");
        file.close();
        QImage base(16, 16, QImage::Format_RGBA8888);
        base.fill(Qt::transparent);
        for (int y = 2; y < 14; ++y)
            for (int x = 2; x < 14; ++x)
                base.setPixelColor(x, y, QColor(255, 0, 0, baseAlpha));
        QVERIFY(base.save(textures + "/layers.png"));
        if (!missingOverlay) {
            QImage overlay(16, 16, QImage::Format_RGBA8888);
            overlay.fill(Qt::transparent);
            for (int y = 5; y < 11; ++y)
                for (int x = 5; x < 11; ++x)
                    overlay.setPixelColor(x, y, QColor(0, 0, 255, overlayAlpha));
            QVERIFY(overlay.save(textures + "/overlay.png"));
        }
        session.setProject(pack);
        library = std::make_shared<mc::Library>();
        library->update(pack, Index::scan(pack));
        panel->setLibrary(library, pack);
        QString error;
        library->setFallbacks({mc::ResourceSource::open(stack, library->version(), &error)});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        panel->findChild<QComboBox *>("modelContext")->setCurrentText("raw");
    }

  private slots:
    void initTestCase() {
        evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        pack = QFINDTESTDATA("../fixtures/model-preview/pack");
        stack = QFINDTESTDATA("../fixtures/model-preview/stack");
        QVERIFY(!pack.isEmpty() && !stack.isEmpty());
        qApp->setProperty("settingsFile", tmp.filePath("studio.ini"));
        session.setProject(pack);
        library = std::make_shared<mc::Library>();
        library->update(pack, Index::scan(pack));
        window = new QWidget;
        auto layout = new QHBoxLayout(window);
        QImage blank(16, 16, QImage::Format_RGBA8888);
        blank.fill(Qt::transparent);
        canvas = new Canvas(std::make_shared<Document>(QString(), blank), &metrics);
        cube = new Cube(&metrics);
        panel = new ModelPanel(cube, &session);
        layout->addWidget(canvas, 2);
        layout->addWidget(cube, 3);
        layout->addWidget(panel, 2);
        panel->setLibrary(library, pack);
        QString error;
        library->setFallbacks({mc::ResourceSource::open(stack, library->version(), &error)});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        window->resize(1200, 640);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(150);
        QVERIFY(cube->isValid());
    }
    void cleanupTestCase() { delete window; }

    void blockModels() {
        QVERIFY(show("block/fx_cube.png"));
        QCOMPARE(target(), QString("model:minecraft:block/fx_cube"));
        QVERIFY(cube->notice().isEmpty());
        QCOMPARE(cube->meshData().faces.size(), 6);
        QVERIFY(coverage(qRgb(90, 140, 210)) > 0.2);
        capture("block-cube");
        QVERIFY(show("block/fx_slab.png"));
        QCOMPARE(target(), QString("model:minecraft:block/fx_slab"));
        QVERIFY(cube->notice().isEmpty());
        QCOMPARE(cube->meshData().faces.size(), 9);
        capture("block-slab-rotated-element");
        QVERIFY(show("block/fx_override.png"));
        const auto &m = cube->meshData();
        QVERIFY(m.maximum.y() - m.minimum.y() < 0.5f * (m.maximum.x() - m.minimum.x())); // pack's slab wins
        capture("pack-override");
    }
    void itemModels() {
        QVERIFY(show("item/fx_gem.png"));
        QCOMPARE(target(), QString("model:minecraft:item/fx_gem"));
        QVERIFY(cube->notice().isEmpty());
        const auto &m = cube->meshData();
        QVERIFY(m.faces.size() > 6); // extruded sprite outline
        QVERIFY(m.maximum.z() - m.minimum.z() < 0.2f * (m.maximum.x() - m.minimum.x()));
        capture("item-generated");
        QVERIFY(show("item/fx_sword.png"));
        QCOMPARE(target(), QString("model:minecraft:item/fx_sword"));
        auto contexts = panel->findChild<QComboBox *>("modelContext");
        QVERIFY(contexts);
        contexts->setCurrentText("thirdperson_righthand");
        QTest::qWait(120);
        QVERIFY(cube->notice().isEmpty());
        capture("item-handheld-thirdperson");
        contexts->setCurrentText("gui");
        QTest::qWait(80);
        capture("item-handheld-gui");
        contexts->setCurrentText("raw");
    }
    void textureWithoutModelIsNotACube() {
        auto doc = show("block/fx_orphan.png");
        QVERIFY(doc);
        QCOMPARE(target(), QString());
        QVERIFY2(cube->notice().startsWith("No Minecraft model · No model in the resource pack or resource stack uses this texture"),
                 qPrintable(cube->notice()));
        QVERIFY(cube->notice().contains("this is not a model render"));
        QCOMPARE(cube->accessibleDescription(), cube->notice());
        // The notice is a real child widget over the view, so it is on screen (not only in
        // framebuffer captures) and readable by assistive technology.
        auto notice = cube->findChild<QLabel *>("previewNotice");
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(notice->text(), cube->notice());
        QVERIFY(cube->rect().contains(notice->geometry()));
        QVERIFY(!notice->visibleRegion().isEmpty());
        QVERIFY(notice->testAttribute(Qt::WA_TransparentForMouseEvents));
        const auto &m = cube->meshData();
        QCOMPARE(m.faces.size(), 2); // one flat square, both sides
        QVERIFY(qFuzzyIsNull(m.maximum.z() - m.minimum.z()));
        QVERIFY(coverage(qRgb(180, 90, 180)) > 0.1); // the texture itself is still shown
        QVERIFY(panel->findChild<QLabel *>("modelDiagnostics")->text().startsWith("No Minecraft model"));
        QVERIFY(panel->findChild<QComboBox *>("modelTarget")->toolTip().contains("No Minecraft model is shown"));
        capture("no-model-fallback");
        // Editing still works and reaches the preview.
        doc->draw({1, 1}, {3, 3}, qRgba(255, 255, 255, 255));
        doc->finish();
        QCOMPARE(doc->image.pixel(2, 2), qRgb(255, 255, 255));
        // An explicitly chosen generic cube says what it is.
        auto targets = panel->findChild<QComboBox *>("modelTarget");
        targets->setCurrentIndex(targets->findData("cube:"));
        QTest::qWait(80);
        QCOMPARE(cube->meshData().faces.size(), 6);
        QVERIFY(cube->notice().startsWith("Generic cube: not a Minecraft model"));
        capture("explicit-generic-cube");
        // Reset Preview returns to the automatic choice (the flat fallback here).
        panel->findChild<QPushButton *>("resetPreview")->click();
        QTest::qWait(80);
        QCOMPARE(cube->meshData().faces.size(), 2);
        QVERIFY(cube->notice().startsWith("No Minecraft model"));
    }
    void noticeHidesForResolvedModels() {
        QVERIFY(show("block/fx_orphan.png"));
        QVERIFY(cube->findChild<QLabel *>("previewNotice")->isVisible());
        QVERIFY(show("block/fx_cube.png"));
        QVERIFY(cube->notice().isEmpty());
        QVERIFY(!cube->findChild<QLabel *>("previewNotice")->isVisible());
    }
    void brokenModelsShowTheirReason() {
        QVERIFY(show("block/fx_broken.png"));
        QVERIFY2(cube->notice().contains("minecraft:block/fx_broken could not be resolved"), qPrintable(cube->notice()));
        QCOMPARE(cube->meshData().faces.size(), 2);
        capture("invalid-json");
        QVERIFY(show("block/fx_cycle.png"));
        QVERIFY2(cube->notice().contains("Cyclic or excessive parent chain"), qPrintable(cube->notice()));
        QCOMPARE(cube->meshData().faces.size(), 2);
        capture("parent-cycle");
        QVERIFY(show("block/fx_lost.png"));
        QVERIFY2(cube->notice().contains("Missing parent/model"), qPrintable(cube->notice()));
        // Pinning an unresolvable model by hand: the view says it is not shown.
        auto targets = panel->findChild<QComboBox *>("modelTarget");
        const int lost = targets->findText("minecraft:block/fx_lost");
        if (lost >= 0) {
            targets->setCurrentIndex(lost);
            QTest::qWait(80);
            QVERIFY2(cube->notice().startsWith("Model not shown · minecraft:block/fx_lost could not be resolved"),
                     qPrintable(cube->notice()));
        }
        // A good model afterwards clears the notice.
        panel->findChild<QPushButton *>("resetPreview")->click();
        QVERIFY(show("block/fx_cube.png"));
        QVERIFY(cube->notice().isEmpty());
    }
    void layeredGeneratedItems_data() {
        QTest::addColumn<int>("baseAlpha");
        QTest::addColumn<int>("overlayAlpha");
        QTest::newRow("translucent-under-opaque") << 128 << 255;
        QTest::newRow("opaque-under-translucent") << 255 << 128;
    }
    void layeredGeneratedItems() {
        QFETCH(int, baseAlpha);
        QFETCH(int, overlayAlpha);
        prepareLayeredItem(baseAlpha, overlayAlpha, false);
        QVERIFY(show("item/layers.png"));
        QCOMPARE(target(), QString("model:minecraft:item/layers"));
        QVERIFY(cube->notice().isEmpty());
        const auto raw = cube->meshData();
        QVERIFY(raw.faces.size() > 6);
        QCOMPARE(raw.maximum.z() - raw.minimum.z(), .125f); // one Minecraft pixel thick
        QCOMPARE(panel->findChild<QComboBox *>("previewLightingMode")->currentText(),
                 QString("Vanilla lighting"));
        panel->findChild<QComboBox *>("previewLightingMode")->setCurrentText("Studio lighting");
        cube->neutral = true;
        cube->setContextFraming(true, true);
        QTest::qWait(100);
        capture(QString("layered-front-%1-%2").arg(baseAlpha).arg(overlayAlpha));
        auto assertPixel = [&](const QImage &image) {
            const auto center = image.pixelColor(image.width() / 2, image.height() / 2);
            const bool correct = overlayAlpha == 255
                ? center.red() < 16 && center.blue() > 240
                : std::abs(center.red() - center.blue()) < 8 && center.red() > 100 && center.blue() > 100;
            QVERIFY2(correct && center.green() < 16,
                     qPrintable(QString("layer composite at center: %1,%2,%3").arg(center.red()).arg(center.green()).arg(center.blue())));
        };
        assertPixel(cube->grabFramebuffer());
        auto view = cube->viewSettings();
        view["yaw"] = 180;
        view["pitch"] = 0;
        cube->restoreView(view);
        QTest::qWait(100);
        capture(QString("layered-back-%1-%2").arg(baseAlpha).arg(overlayAlpha));
        assertPixel(cube->grabFramebuffer());

        // The configured stack supplies handheld transforms; the project supplies both layers.
        const auto model = library->model("minecraft:item/layers");
        QCOMPARE(mc::resolveDisplay(model, "thirdperson_righthand").origin, QString("minecraft:item/handheld"));
        const auto transform = mc::displayTransform(model, "thirdperson_righthand");
        QVERIFY(!transform.isIdentity());
        auto contexts = panel->findChild<QComboBox *>("modelContext");
        contexts->setCurrentText("thirdperson_righthand");
        QTest::qWait(100);
        QCOMPARE(cube->meshData().faces.size(), raw.faces.size());
        for (int i = 0; i < raw.faces.size(); ++i)
            for (int p = 0; p < 4; ++p)
                QVERIFY((cube->meshData().faces[i].points[p] - transform.map(raw.faces[i].points[p])).length() < 1e-4f);
        // Orbit the transformed model so the capture shows its face and thin edge.
        view["yaw"] = 38;
        view["pitch"] = 25;
        cube->restoreView(view);
        QTest::qWait(100);
        capture(QString("layered-handheld-%1-%2").arg(baseAlpha).arg(overlayAlpha));
        panel->findChild<QComboBox *>("previewLightingMode")->setCurrentText("Vanilla lighting");
        cube->neutral = false;
    }
    void missingGeneratedSpriteIsExplicit() {
        prepareLayeredItem(255, 255, true);
        QVERIFY(show("item/layers.png"));
        capture("generated-missing-sprite");
        QVERIFY2(cube->notice().contains("flat approximation"), qPrintable(cube->notice()));
        QVERIFY(cube->notice().contains("assets/minecraft/textures/item/overlay.png"));
        auto notice = cube->findChild<QLabel *>("previewNotice");
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(cube->accessibleDescription(), cube->notice());
        QVERIFY(cube->rect().contains(notice->geometry()));
        // Supplying the missing project sprite restores the extruded preview and clears the notice.
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        QVERIFY(image.save(pack + "/assets/minecraft/textures/item/overlay.png"));
        library->update(pack, Index::scan(pack), {"assets/minecraft/textures/item/overlay.png"});
        panel->setLibrary(library, pack);
        QTest::qWait(100);
        QVERIFY(cube->notice().isEmpty());
        QCOMPARE(cube->meshData().maximum.z() - cube->meshData().minimum.z(), .125f);
        capture("generated-sprite-restored");
    }
    void inventorySlotAndOversizedBounds() {
        mc::Model model;
        model.valid = true;
        model.version = "1.21.11";
        model.guiLight = mc::GuiLight::Front;
        const QString texture = "assets/nova/textures/item/solid.png";
        model.resolved["elements"] = QJsonArray{
            QJsonObject{{"from", QJsonArray{-8, 0, 8}}, {"to", QJsonArray{24, 16, 8}},
                        {"faces", QJsonObject{{"south", QJsonObject{
                            {"texture", "nova:item/solid"}, {"uv", QJsonArray{0, 0, 16, 16}}}}}}}};
        QImage sprite(16, 16, QImage::Format_RGBA8888);
        sprite.fill(Qt::green);
        cube->setDocument({});
        cube->setModel(std::make_shared<mc::Mesh>(mc::mesh(model)), {}, {{texture, sprite}});
        cube->setContextTransform({});
        cube->setPreviewContext("gui", false);
        QTest::qWait(50);
        const auto outside = cube->rect().center() + QPoint(std::min(cube->width(), cube->height()) / 3, 0);
        QCOMPARE(cube->faceAt(outside), -1);
        cube->setPreviewContext("gui", false, {}, true);
        QTest::qWait(50);
        QCOMPARE(cube->faceAt(outside), 0);
        const auto icon = cube->captureInventoryIcon(2);
        QCOMPARE(icon.size(), QSize(32, 32));
        QCOMPARE(icon.pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(icon.pixelColor(31, 31), QColor(Qt::green));
    }
    void explorerUsesInventoryModel() {
        const auto dir = tmp.filePath("icons");
        const QString model = "assets/nova/models/item/diagonal.json";
        const QString texture = "assets/nova/textures/item/diagonal.png";
        QVERIFY(QDir().mkpath(dir + "/assets/nova/models/item"));
        QVERIFY(QDir().mkpath(dir + "/assets/nova/textures/item"));
        QFile json(dir + '/' + model);
        QVERIFY(json.open(QIODevice::WriteOnly));
        json.write("{\"gui_light\":\"front\",\"elements\":[{\"from\":[0,0,8],\"to\":[8,16,8],"
                   "\"faces\":{\"south\":{\"texture\":\"nova:item/diagonal\",\"uv\":[0,0,16,16]}}}]}");
        json.close();
        QImage sprite(16, 16, QImage::Format_RGBA8888);
        sprite.fill(Qt::red);
        QVERIFY(sprite.save(dir + '/' + texture));
        const auto index = Index::scan(dir);
        auto catalog = std::make_shared<mc::Library>("1.21.11");
        catalog->update(dir, index);
        AssetModel explorer;
        explorer.setIndex(index, dir);
        explorer.setModelSources(catalog);
        const auto row = explorer.indexFor(model);
        QVERIFY(row.isValid());
        auto icon = [&] { return qvariant_cast<QIcon>(explorer.data(row, Qt::DecorationRole)); };
        icon();
        QTRY_VERIFY(explorer.cachedBytes() > 0);
        const auto image = icon().pixmap(64, 64).toImage();
        QCOMPARE(image.pixelColor(16, 32), QColor(Qt::red));
        QCOMPARE(image.pixelColor(48, 32).alpha(), 0);
    }
    void specialInventoryPixels_data() {
        QTest::addColumn<QJsonObject>("renderer");
        QTest::addColumn<QString>("texture");
        QTest::addColumn<QColor>("expected");
        const struct {
            const char *name;
            QRgb rgb;
        } dyes[] = {{"white", 0xf9fffe}, {"orange", 0xf9801d}, {"magenta", 0xc74ebd},
                    {"light_blue", 0x3ab3da}, {"yellow", 0xfed83d}, {"lime", 0x80c71f},
                    {"pink", 0xf38baa}, {"gray", 0x474f52}, {"light_gray", 0x9d9d97},
                    {"cyan", 0x169c9c}, {"purple", 0x8932b8}, {"blue", 0x3c44aa},
                    {"brown", 0x835432}, {"green", 0x5e7c16}, {"red", 0xb02e26},
                    {"black", 0x1d1d21}};
        for (const auto &dye : dyes)
            QTest::newRow(dye.name) << QJsonObject{{"type", "banner"}, {"color", dye.name}}
                << QString("banner/base") << QColor::fromRgb(dye.rgb);
        const struct {
            const char *kind;
            const char *skin;
        } heads[] = {{"dragon", "enderdragon/dragon"}, {"piglin", "piglin/piglin"}};
        const QColor sample(160, 100, 50);
        for (const auto &head : heads)
            QTest::newRow(head.kind) << QJsonObject{{"type", "head"}, {"kind", head.kind}}
                << QString(head.skin) << sample;
        QTest::newRow("pot") << QJsonObject{{"type", "decorated_pot"}}
            << QString("decorated_pot/decorated_pot_side") << sample;
        for (const auto *wax : {"", "waxed_"})
            for (const auto *oxidation : {"", "exposed_", "weathered_", "oxidized_"}) {
                const auto item = QString(wax) + oxidation + "copper_golem_statue";
                const auto skin = QString("copper_golem/") + oxidation + "copper_golem";
                QTest::newRow(qPrintable(item))
                    << QJsonObject{{"type", "copper_golem_statue"}, {"pose", "standing"},
                                   {"texture", "minecraft:textures/entity/" + skin + ".png"}}
                    << skin << sample;
            }
    }
    void specialInventoryPixels() {
        QFETCH(QJsonObject, renderer);
        QFETCH(QString, texture);
        QFETCH(QColor, expected);
        const auto root = tmp.filePath(QString("special-") + QTest::currentDataTag());
        auto write = [&](const QString &path, const QJsonObject &object) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0;
        };
        auto save = [&](const QString &name, QColor color) {
            const auto path = root + "/assets/minecraft/textures/entity/" + name + ".png";
            QDir().mkpath(QFileInfo(path).absolutePath());
            QImage sheet(64, 64, QImage::Format_RGBA8888);
            sheet.fill(color);
            return sheet.save(path);
        };
        const auto type = renderer["type"].toString();
        const bool banner = type == "banner", statue = type == "copper_golem_statue";
        const QString asset = "assets/minecraft/items/special_fixture.json";
        QJsonObject node{{"type", "special"}, {"base", "minecraft:item/special_fixture"},
                         {"model", renderer}};
        if (statue)
            node = QJsonObject{{"type", "select"}, {"property", "minecraft:block_state"},
                               {"block_state_property", "copper_golem_pose"}, {"fallback", node}};
        QVERIFY(write(root + '/' + asset, {{"model", node}}));
        const QJsonObject pose{{"rotation", QJsonArray{0, 0, statue ? 180 : 0}}};
        QVERIFY(write(root + "/assets/minecraft/models/item/special_fixture.json",
                      {{"gui_light", "front"}, {"display", QJsonObject{{"gui", pose}}}}));
        const QColor wood(90, 60, 30);
        if (banner)
            QVERIFY(save("banner_base", wood));
        if (type == "decorated_pot")
            QVERIFY(save("decorated_pot/decorated_pot_base", Qt::blue));
        QVERIFY(save(texture, banner ? QColor(Qt::white) : expected));

        mc::Library lib("1.21.11");
        lib.update(root, Index::scan(root));
        const auto icon = renderInventoryIcon(lib, asset, 3);
        QCOMPARE(icon.size(), QSize(48, 48));
        // At x=8 the front panel or head faces +Z. The flat light rig saturates there.
        const int modelY = banner || type == "decorated_pot" ? 8 : 5;
        QCOMPARE(icon.pixelColor(24, 48 - 3 * modelY), expected);
        QCOMPARE(icon.pixelColor(0, 47).alpha(), 0);
        if (banner)
            QCOMPARE(icon.pixelColor(24, 45), wood);
    }
    void blockInventoryPixels_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("bare-slot-and-translation") << QString("heavy_core");
        QTest::newRow("inverted-cage") << QString("spawner");
    }
    void blockInventoryPixels() {
        QFETCH(QString, kind);
        const auto root = tmp.filePath(kind);
        const auto modelDir = root + "/assets/minecraft/models/block";
        const auto textureDir = root + "/assets/minecraft/textures/block";
        QVERIFY(QDir().mkpath(modelDir));
        QVERIFY(QDir().mkpath(textureDir));
        const bool cage = kind == "spawner";
        const QColor solid(150, 80, 30);
        QImage sprite(16, 16, QImage::Format_RGBA8888);
        sprite.fill(solid);
        QVERIFY(sprite.save(textureDir + "/solid.png"));
        sprite.fill(Qt::transparent);
        QVERIFY(sprite.save(textureDir + "/holes.png"));
        const auto panel = [](QJsonArray from, QJsonArray to, const char *side, const char *texture) {
            return QJsonObject{{"from", from}, {"to", to},
                               {"faces", QJsonObject{{side, QJsonObject{{"texture", texture}}}}}};
        };
        QJsonArray elements;
        QJsonObject gui;
        if (cage) {
            elements.append(panel({0, 0, 0}, {16, 16, 16}, "south", "#holes"));
            // The reversed X axis turns the rear north quad toward the slot viewer.
            elements.append(panel({14, 2, 2}, {2, 14, 14}, "north", "#all"));
        } else {
            gui["translation"] = QJsonArray{0, 3, 0};
            elements.append(panel({4, 0, 4}, {12, 8, 12}, "south", "all"));
        }
        const QJsonObject source{{"gui_light", "front"}, {"elements", elements},
            {"display", QJsonObject{{"gui", gui}}},
            {"textures", QJsonObject{{"all", "block/solid"}, {"holes", "block/holes"}}}};
        QFile file(modelDir + '/' + kind + ".json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QJsonDocument(source).toJson()) > 0);
        file.close();
        mc::Library lib("1.21.11");
        lib.update(root, Index::scan(root));
        const auto icon = renderInventoryIcon(lib, "assets/minecraft/models/block/" + kind + ".json", 3);
        QCOMPARE(icon.size(), QSize(48, 48));
        const QRect pixels = cage ? QRect(6, 6, 36, 36) : QRect(12, 15, 24, 24);
        for (int y = 0; y < icon.height(); ++y)
            for (int x = 0; x < icon.width(); ++x)
                QCOMPARE(icon.pixelColor(x, y), pixels.contains(x, y) ? solid : QColor(Qt::transparent));
    }
    void inventoryAtlasTexel() {
        mc::Model model;
        model.valid = true;
        model.version = "1.21.11";
        const QString texture = "assets/nova/textures/block/atlas.png";
        model.display["gui"] = QJsonObject{{"rotation", QJsonArray{30, 225, 0}},
                                           {"scale", QJsonArray{.625, .625, .625}}};
        const QJsonObject up{{"texture", "nova:block/atlas"}};
        model.resolved["elements"] = QJsonArray{
            QJsonObject{{"from", QJsonArray{0, 0, 0}}, {"to", QJsonArray{16, 16, 16}},
                        {"faces", QJsonObject{{"up", up}}}}};
        QImage sprite(16, 16, QImage::Format_RGBA8888);
        sprite.fill(Qt::white);
        sprite.setPixelColor(11, 4, Qt::red);
        sprite.setPixelColor(12, 4, Qt::green);
        const QMap<QString, QImage> materials{{texture, sprite}};
        const auto geometry = mc::displayMesh(model, {}, "gui", materials);
        cube->setDocument({});
        cube->setContextTransform({});
        cube->setPreviewContext("gui", false, "minecraft:stone");
        cube->setModel(std::make_shared<mc::Mesh>(geometry), {}, materials);
        const auto actual = cube->captureInventoryIcon(4);
        QCOMPARE(actual.size(), QSize(64, 64));
        // GuiRenderer's 64 px projection places this centre at about (11.99968, 4.283)
        // in the tile. The block-atlas interpolant selects the texel in column 12.
        QCOMPARE(actual.pixelColor(31, 21), QColor(Qt::green));
    }
    void inventoryPixels_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("scale");
        for (const auto kind : {"cube_all", "generated", "handheld", "front", "tinted",
                                "cutout", "translucent", "frogspawn"}) {
            QTest::newRow(qPrintable(QString("%1-16").arg(kind))) << QString(kind) << 1;
            QTest::newRow(qPrintable(QString("%1-32").arg(kind))) << QString(kind) << 2;
        }
    }
    void inventoryPixels() {
        QFETCH(QString, kind);
        QFETCH(int, scale);
        const auto root = tmp.filePath("golden");
        const QString asset = "assets/nova/models/item/card.json";
        QVERIFY(QDir().mkpath(root + "/assets/nova/models/item"));
        const bool cubeAll = kind == "cube_all";
        const bool generated = kind == "generated" || kind == "handheld";
        const bool translucent = kind == "translucent";
        const bool cutout = kind == "cutout";
        const bool frogspawn = kind == "frogspawn";
        const QString spriteId = cubeAll || cutout || translucent || frogspawn ? "nova:block/card"
                                                                               : "nova:item/card";
        const auto texture = mc::Location::parse(spriteId).asset("textures");
        QJsonObject source;
        if (cubeAll || generated || frogspawn) {
            const QString parent = cubeAll ? "minecraft:block/cube_all"
                                           : "minecraft:item/" + (frogspawn ? QString("generated") : kind);
            source["parent"] = parent;
            source["textures"] = QJsonObject{{cubeAll ? "all" : "layer0", spriteId}};
        } else {
            source["gui_light"] = "front";
            source["elements"] = QJsonArray{QJsonObject{
                {"from", QJsonArray{0, 0, 8}}, {"to", QJsonArray{16, 16, 8}},
                {"faces", QJsonObject{{"south", QJsonObject{{"texture", spriteId},
                    {"tintindex", 0}, {"uv", QJsonArray{0, 0, 16, 16}}}}}}}};
        }
        QFile file(root + '/' + asset);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(QJsonDocument(source).toJson()) > 0);
        file.close();
        auto index = Index::scan(root);
        mc::Library lib("1.21.11");
        lib.update(root, index);
        QString error;
        lib.setFallbacks({mc::ResourceSource::open(stack, "1.21.11", &error)});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto model = lib.model("nova:item/card");
        QVERIFY2(model.valid, qPrintable(model.diagnostics.join('\n')));
        const QRgb tint = kind == "tinted" ? qRgb(128, 200, 64) : qRgb(255, 255, 255);
        mc::Instance instance{model.id, {}};
        instance.tints << tint;
        QImage sprite(16, 16, QImage::Format_RGBA8888);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const int alpha = translucent ? (x < 8 ? 25 : 128)
                                : cutout ? (x < 8 ? 25 : 26)
                                : frogspawn ? (x < 8 ? 0 : 170)
                                : generated && (x < 3 || y < 3 || x > 12 || y > 12) ? 0 : 255;
                sprite.setPixelColor(x, y, QColor(160, 80, 40, alpha));
            }
        const QMap<QString, QImage> materials{{texture, sprite}};
        const auto geometry = mc::displayMesh(model, instance, "gui", materials);
        cube->setDocument({});
        cube->setContextTransform({});
        cube->setPreviewContext("gui", false, cubeAll ? "minecraft:stone"
                                               : cutout ? "minecraft:iron_bars"
                                               : frogspawn ? "minecraft:frogspawn"
                                               : translucent ? "minecraft:glass" : QString());
        cube->setModel(std::make_shared<mc::Mesh>(geometry), {}, materials);
        const auto actual = cube->captureInventoryIcon(scale);
        QCOMPARE(actual.size(), QSize(16 * scale, 16 * scale));
        if (!evidence.isEmpty() && scale == 2 && (cubeAll || kind == "generated")) {
            QDir().mkpath(evidence);
            const auto save = [&](const QImage &pixels, const QString &lighting) {
                return pixels.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                    .save(evidence + '/' + kind + '-' + lighting + ".png");
            };
            cube->studioLighting = true;
            const auto studio = cube->captureInventoryIcon(scale);
            cube->studioLighting = false;
            QVERIFY(save(studio, "studio"));
            QVERIFY(save(actual, "vanilla"));
        }

        // Lighting's normalized vectors rotated by Ry(-22.5) Rx(135) for flat,
        // and Ry(62) Rx(185.5) Ry(-22.5) Rx(135) for side, expressed in Y-up.
        // The guarded dot products in light.glsl contribute 60% over 40% ambient.
        const QVector3D first = cubeAll ? QVector3D(-.9334393f, .2626947f, -.2443002f)
                                        : QVector3D(-.2225190f, .1714986f, .9597258f);
        const QVector3D second = cubeAll ? QVector3D(-.1035714f, .9766068f, .1884464f)
                                         : QVector3D(-.2150121f, .9718253f, .0965678f);
        // Independently intersect each slot pixel's centre ray with the fixture cube.
        // A slab intersection in unrotated cube space identifies its visible face.
        QMatrix4x4 rotation;
        rotation.rotate(30, 1, 0, 0);
        rotation.rotate(225, 0, 1, 0);
        const auto inverse = rotation.transposed();
        const auto ray = inverse.mapVector({0, 0, -1});
        for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x) {
                int alpha = sprite.pixelColor(x / scale, y / scale).alpha();
                QVector3D normal(0, 0, 1);
                if (cubeAll) {
                    const auto start = inverse.mapVector({
                        2.f * (x + .5f) / actual.width() - 1,
                        1 - 2.f * (y + .5f) / actual.height(), 4}) / .6f;
                    float near = -100, far = 100;
                    for (int axis = 0; axis < 3; ++axis) {
                        if (qFuzzyIsNull(ray[axis]))
                            continue;
                        const float a = (-1 - start[axis]) / ray[axis];
                        const float b = (1 - start[axis]) / ray[axis];
                        if (std::min(a, b) > near) {
                            near = std::min(a, b);
                            normal = {};
                            normal[axis] = a < b ? -1.f : 1.f;
                        }
                        far = std::min(far, std::max(a, b));
                    }
                    normal = rotation.mapVector(normal);
                    alpha = near <= far ? 255 : 0;
                }
                const auto illum = std::min(1.f, .4f + .6f *
                    (std::max(0.f, QVector3D::dotProduct(first, normal)) +
                     std::max(0.f, QVector3D::dotProduct(second, normal))));
                alpha = alpha < 26 ? 0 : alpha;
                // Cutout atlas writes straight texture RGB even with sub-255 alpha.
                const auto pixel = cutout || frogspawn ? QColor::fromRgba(actual.pixel(x, y))
                                                      : actual.pixelColor(x, y);
                const int expected[] = {
                    alpha ? qRound(160 * illum * qRed(tint) / 255) : 0,
                    alpha ? qRound(80 * illum * qGreen(tint) / 255) : 0,
                    alpha ? qRound(40 * illum * qBlue(tint) / 255) : 0, alpha};
                const int observed[] = {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
                for (int channel = 0; channel < 4; ++channel)
                    QVERIFY2(std::abs(observed[channel] - expected[channel]) <= 1,
                             qPrintable(QString("%1,%2 channel %3 expected %4 got %5")
                                        .arg(x).arg(y).arg(channel).arg(expected[channel]).arg(observed[channel])));
            }
    }

};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    application.setStyle("Fusion");
    ModelPreviewGuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "model_preview_gui_test.moc"
