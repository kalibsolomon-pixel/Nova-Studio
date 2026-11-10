// SPDX-License-Identifier: GPL-3.0-or-later
// 3D preview regression on fixtures/model-preview: the panel shows the texture's resolved
// Minecraft model (block, slab with rotated element, generated and handheld items), and a
// texture without a usable model is shown flat with a visible, named fallback reason -
// never silently as a cube. The texture stays editable throughout. With NOVA_EVIDENCE_DIR
// set, each case is captured from the real OpenGL view.
#include "modelpanel.h"
#include "display.h"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
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
