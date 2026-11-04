// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelpanel.h"
#include "playback.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace nova;
class ModelGuiTest : public QObject {
    Q_OBJECT
  private slots:
    void liveUvAndCamera() {
        const auto root = QString(QFINDTESTDATA("../fixtures/phase-2c5-models"));
        QVERIFY(!root.isEmpty());
        auto library = std::make_shared<mc::Library>();
        library->update(root, Index::scan(root));
        Metrics metrics;
        DocumentSession session;
        session.setProject(root);
        QString error;
        auto doc = session.open(root + "/assets/nova/textures/block/check.png", &error);
        QVERIFY2(doc, qPrintable(error));
        QWidget window;
        auto layout = new QHBoxLayout(&window);
        auto canvas = new Canvas(doc, &metrics);
        auto cube = new Cube(&metrics);
        auto panel = new ModelPanel(cube, &session);
        layout->addWidget(canvas, 2);
        layout->addWidget(cube, 2);
        layout->addWidget(panel, 1);
        cube->setDocument(doc);
        panel->setLibrary(library, root);
        panel->setDocument(doc, canvas);
        window.resize(1200, 780);
        window.show();
        QTest::qWait(180);
        QVERIFY(cube->isValid());
        panel->openAsset("assets/nova/models/block/complex.json");
        QTest::qWait(80);
        QCOMPARE(cube->meshData().faces.size(), 18);
        const auto before = cube->grabFramebuffer();
        QVERIFY(!before.isNull());
        const int picked = cube->faceAt(cube->rect().center());
        QVERIFY(picked >= 0);
        QTest::mouseClick(cube, Qt::LeftButton, Qt::NoModifier, cube->rect().center());
        QCOMPARE(cube->selectedFace, picked);
        panel->selectFace(0);
        QVERIFY(canvas->uvVisible);
        QVERIFY(!canvas->uvRegions.isEmpty());
        QVERIFY(doc->hasSelection());
        QCOMPARE(cube->selectedFace, 0);
        const auto uploads = metrics.counters["model_geometry_uploads"].toInteger();
        doc->deselect();
        doc->draw({1, 1}, {2, 2}, qRgba(255, 0, 0, 255));
        doc->finish();
        cube->setDocument(doc);
        cube->update();
        QTest::qWait(30);
        auto after = cube->grabFramebuffer();
        QCOMPARE(metrics.counters["model_geometry_uploads"].toInteger(), uploads);
        QVERIFY(after != before);
        doc->undo();
        cube->update();
        QTest::qWait(20);
        doc->redo();
        cube->update();
        auto contexts = panel->findChild<QComboBox *>("modelContext");
        contexts->setCurrentText("gui");
        QTest::qWait(20);
        QVERIFY(!cube->grabFramebuffer().isNull());
        auto perspective = panel->findChild<QCheckBox *>("previewPerspective");
        perspective->setChecked(true);
        QTest::qWait(20);
        QVERIFY(cube->perspective);
        auto wire = panel->findChild<QCheckBox *>("previewWireframe");
        const auto noWire = cube->grabFramebuffer();
        wire->setChecked(true);
        QTest::qWait(30);
        QVERIFY(cube->wireframe);
        const auto withWire = cube->grabFramebuffer();
        QVERIFY(withWire != noWire);
        const auto evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        auto capture = [&](const QString &name) {
            if (evidence.isEmpty())
                return;
            QDir().mkpath(evidence);
            QTest::qWait(25);
            QVERIFY(window.grab().save(evidence + '/' + name + ".png"));
        };
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(window.grab().save(evidence + "/model-uv-live.png"));
        }
        panel->openAsset("assets/nova/models/block/missing.json");
        QVERIFY(panel->findChild<QLabel *>("modelDiagnostics")->text().contains("Missing"));
        capture("model-missing-texture");
        panel->openAsset("assets/nova/items/gem.json");
        QCOMPARE(cube->meshData().faces.size(), 6); // C6: front/back and four opaque contour walls.
        const auto deps = panel->findChild<QListWidget *>("modelDependencies");
        for (int row = 0; row < deps->count(); ++row)
            QVERIFY(!deps->item(row)->text().contains("builtin/generated"));
        capture("model-generated-item");
        panel->openAsset("assets/nova/models/block/inherited.json");
        capture("model-inherited");
        panel->openAsset("assets/nova/models/block/check.json");
        cube->resetView();
        contexts->setCurrentText("raw");
        capture("model-basic-block");
        panel->openAsset("assets/nova/models/block/complex.json");
        capture("model-complex-cuboids");
        panel->selectFace(3);
        capture("model-selected-face-uv");
        auto animated = session.open(root + "/assets/nova/textures/block/animated.png", &error);
        QVERIFY(animated && animated->animated());
        canvas->document = animated;
        cube->setDocument(animated);
        panel->setDocument(animated, canvas);
        panel->openAsset("assets/nova/models/block/animated.json");
        PlaybackController playback;
        connect(&playback, &PlaybackController::displayed, &window, [&](const QImage &image, int) {
            canvas->setPlaybackPreview(image);
            cube->setPlaybackPreview(image);
        });
        playback.setDocument(animated);
        playback.play();
        playback.pause();
        playback.seek(1);
        QTest::qWait(30);
        QVERIFY(canvas->playbackVisible());
        QCOMPARE(canvas->displayed(),
                 animated->frameComposite(animated->animationState().frames[1].id));
        capture("model-animated-frame");
        playback.stop();
        canvas->setPlaybackPreview({});
        cube->setPlaybackPreview({});
        panel->openAsset("assets/nova/models/block/cyclic.json");
        QVERIFY(cube->meshData().faces.isEmpty());
        QVERIFY(panel->findChild<QLabel *>("modelDiagnostics")->text().contains("Cyclic"));
        cube->setModel({});
        QCOMPARE(cube->meshData().faces.size(), 6);
    }
};
QTEST_MAIN(ModelGuiTest)
#include "model_gui_test.moc"
