// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>
class LayerGuiTests : public QObject {
    Q_OBJECT
    QString evidence;
    void capture(QWidget &widget, const QString &name) {
        QTest::qWait(100);
        QVERIFY(widget.grab().save(evidence + '/' + name + ".png"));
    }
    void click(nova::Window &window, const QString &name) {
        auto b = window.findChild<QPushButton *>(name);
        QVERIFY(b);
        QVERIFY(b->isEnabled());
        QTest::mouseClick(b, Qt::LeftButton);
    }
    void discardClose(nova::Window &window) {
        QTimer::singleShot(50, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box)
                box->button(QMessageBox::Discard)->click();
        });
        QVERIFY(window.close());
    }
  private slots:
    void workflow() {
        QTemporaryDir tmp;
        evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        if (evidence.isEmpty())
            evidence = tmp.path() + "/screenshots";
        QDir().mkpath(evidence);
        const auto root = tmp.path() + "/Layer Pack",
                   path = root + "/assets/minecraft/textures/block/test.png",
                   settings = tmp.path() + "/prefs/studio.ini";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QVERIFY(nova::savePng(path, image));
        QFile meta(root + "/pack.mcmeta");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write("{\"pack\":{\"pack_format\":34,\"description\":\"Layers\"}}");
        meta.close();
        qApp->setProperty("settingsFile", settings);
        QElapsedTimer time;
        time.start();
        nova::Window window(root, {}, time);
        window.show();
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        QTRY_COMPARE(tabs->count(), 1);
        auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        auto doc = canvas->document;
        auto list = window.findChild<QListWidget *>("layerList");
        QCOMPARE(list->count(), 1);
        const auto origin = QPointF(canvas->width() / 2.0, canvas->height() / 2.0) -
                            QPointF(8 * canvas->zoom, 8 * canvas->zoom);
        auto point = (origin + QPointF(2.5 * canvas->zoom, 2.5 * canvas->zoom)).toPoint();
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(doc->layers()[0].pixels.pixel(2, 2), qRgba(130, 141, 255, 255));
        click(window, "addLayer");
        QCOMPARE(list->count(), 2);
        auto ink = doc->activeLayer();
        list->currentItem()->setText("Highlights");
        QCOMPARE(doc->layers()[1].name, QString("Highlights"));
        // Eraser and pencil use the selected layer through the same document command.
        canvas->document->draw({4, 4}, {10, 10}, qRgba(240, 150, 90, 255));
        canvas->document->finish();
        canvas->changed();
        QCOMPARE(doc->layers()[0].pixels.pixel(5, 5), QRgb(0));
        capture(window, "layers-01-visible");
        list->currentItem()->setCheckState(Qt::Unchecked);
        QCOMPARE(doc->layers()[1].visible, false);
        auto hiddenPixels = doc->layers()[1].pixels;
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(doc->layers()[1].pixels, hiddenPixels);
        capture(window, "layers-02-hidden");
        list->currentItem()->setCheckState(Qt::Checked);
        auto opacity = window.findChild<QSpinBox *>("layerOpacity");
        opacity->setValue(128);
        QCOMPARE(doc->layers()[1].opacity, 128);
        capture(window, "layers-03-opacity");
        click(window, "lowerLayer");
        QCOMPARE(doc->layers()[0].id, ink);
        capture(window, "layers-04-reordered");
        click(window, "raiseLayer");
        click(window, "duplicateLayer");
        QCOMPARE(doc->layers().size(), size_t(3));
        auto duplicate = doc->activeLayer();
        click(window, "deleteLayer");
        QCOMPARE(doc->layers().size(), size_t(2));
        window.raise();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        canvas->setFocus();
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(doc->layers().size(), size_t(3));
        QCOMPARE(doc->activeLayer(), duplicate);
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(canvas, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(doc->layers().size(), size_t(2));
        auto cube = static_cast<nova::Cube *>(window.findChild<QOpenGLWidget *>());
        QCOMPARE(cube->document, doc);
        QTRY_VERIFY(doc->dirty.isEmpty());
        QVERIFY(tabs->tabText(0).contains("●"));
        QVERIFY(tabs->tabText(0).contains("↑PNG"));
        window.findChild<QAction *>("saveNativeCommand")->trigger();
        QVERIFY(!doc->modified());
        QVERIFY(doc->exportModified());
        const auto native = doc->encodeNative("assets/minecraft/textures/block/test.png");
        QVERIFY(QFileInfo::exists(doc->editablePath()));
        QCOMPARE(nova::loadPng(path), image);
        capture(window, "layers-05-native-saved");
        window.findChild<QAction *>("exportPngCommand")->trigger();
        QCOMPARE(nova::loadPng(path), doc->image);
        QVERIFY(!doc->exportModified());
        window.findChild<QAction *>("saveCommand")->trigger();
        QVERIFY(!doc->modified());
        capture(window, "layers-06-exported");
        const auto expected = doc->encodeNative("assets/minecraft/textures/block/test.png");
        auto dock = window.findChild<QDockWidget *>("layers");
        dock->setFloating(true);
        dock->resize(360, 310);
        capture(*dock, "layers-07-floating");
        dock->setFloating(false);
        window.resize(900, 640);
        capture(window, "layers-08-compact");
        window.resize(1400, 900);
        capture(window, "layers-09-resized");
        QImage external(16, 16, QImage::Format_RGBA8888);
        external.fill(QColor(25, 100, 190));
        QVERIFY(nova::savePng(path, external));
        QTimer::singleShot(80, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            capture(*box, "layers-10-conflict");
            for (auto b : box->buttons())
                if (b->text() == "Keep current")
                    b->click();
        });
        window.findChild<QAction *>("exportPngCommand")->trigger();
        QCOMPARE(nova::loadPng(path), external);
        QCOMPARE(doc->encodeNative("assets/minecraft/textures/block/test.png"), expected);
        QVERIFY(window.close());
        qApp->setProperty("settingsFile", tmp.path() + "/reopen.ini");
        nova::Window reopened(root, {}, time);
        reopened.show();
        auto reopenedTabs = reopened.findChild<QTabWidget *>("editorTabs");
        QTRY_COMPARE(reopenedTabs->count(), 1);
        auto reopenedDoc = static_cast<nova::Canvas *>(reopenedTabs->currentWidget())->document;
        QCOMPARE(reopenedDoc->encodeNative("assets/minecraft/textures/block/test.png"), expected);
        QVERIFY(reopenedDoc->externalChanged());
        capture(reopened, "layers-11-reopened");
        QVERIFY(reopened.close());
        Q_UNUSED(native);
        // Actual native workspace, asynchronous safety worker, then forced process termination.
        auto crashSettings = tmp.path() + "/crash/studio.ini";
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {"--layer-gui-child", root, crashSettings});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForReadyRead(20000));
        QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("snapshot-ready"));
        child.kill();
        QVERIFY(child.waitForFinished());
        nova::RecoveryStore store(QFileInfo(crashSettings).absolutePath() + "/recovery");
        auto entries = store.entries();
        QCOMPARE(entries.size(), 1);
        auto entry = store.load(entries[0].id);
        QVERIFY(entry.error.isEmpty());
        nova::Document expectedRecovery(entry.path, QImage{});
        QVERIFY(expectedRecovery.decodeNative(entry.nativeData, "recovery.png"));
        QCOMPARE(expectedRecovery.layers().size(), size_t(3));
        auto disk = nova::Revision::read(path);
        qApp->setProperty("settingsFile", crashSettings);
        QTimer restoreTimer;
        restoreTimer.setInterval(50);
        connect(&restoreTimer, &QTimer::timeout, this, [&] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->objectName() != "recoveryDialog")
                return;
            restoreTimer.stop();
            capture(*box, "layers-12-recovery-dialog");
            for (auto b : box->buttons())
                if (b->text() == "Restore")
                    b->click();
        });
        restoreTimer.start();
        nova::Window recovered(root, {}, time);
        recovered.show();
        auto recoveredTabs = recovered.findChild<QTabWidget *>("editorTabs");
        QTRY_VERIFY_WITH_TIMEOUT(!restoreTimer.isActive(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(recoveredTabs->count(), 1, 5000);
        auto restored = static_cast<nova::Canvas *>(recoveredTabs->currentWidget())->document;
        QTRY_VERIFY_WITH_TIMEOUT(restored->modified(), 5000);
        QCOMPARE(restored->encodeNative("recovery.png"),
                 expectedRecovery.encodeNative("recovery.png"));
        QVERIFY(disk.sameContent(nova::Revision::read(path)));
        capture(recovered, "layers-13-recovered");
        discardClose(recovered);
        // Persisted docking is restored in the new normal-layout workspace.
        qApp->setProperty("settingsFile", settings);
        nova::Window layout(root, {}, time);
        layout.show();
        QTRY_VERIFY(layout.findChild<QDockWidget *>("layers")->isVisible());
        QVERIFY(layout.close());
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Nova Layer Acceptance");
    if (app.arguments().contains("--layer-gui-child")) {
        auto args = app.arguments();
        app.setProperty("settingsFile", args[3]);
        QElapsedTimer time;
        time.start();
        nova::Window window(args[2], {}, time);
        window.show();
        QTimer edit;
        edit.setInterval(100);
        QObject::connect(&edit, &QTimer::timeout, &window, [&] {
            auto tabs = window.findChild<QTabWidget *>("editorTabs");
            if (!tabs->count())
                return;
            auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
            auto doc = canvas->document;
            doc->editLayer({nova::LayerCommand::Add, {}, "Recovered ink"});
            auto id = doc->activeLayer();
            doc->draw({3, 3}, {8, 8}, qRgba(80, 230, 120, 255));
            doc->finish();
            doc->editLayer({nova::LayerCommand::Opacity, id, {}, 90});
            doc->editLayer({nova::LayerCommand::Visibility, id, {}, 0});
            doc->editLayer({nova::LayerCommand::Move, id, {}, 0});
            canvas->changed();
            edit.stop();
        });
        edit.start();
        QTimer ready;
        ready.setInterval(100);
        QObject::connect(&ready, &QTimer::timeout, &window, [&] {
            nova::RecoveryStore store(QFileInfo(args[3]).absolutePath() + "/recovery");
            auto entries = store.entries();
            if (entries.isEmpty())
                return;
            auto e = store.load(entries[0].id);
            if (e.nativeData.isEmpty())
                return;
            ready.stop();
            fprintf(stdout, "snapshot-ready\n");
            fflush(stdout);
        });
        ready.start();
        return app.exec();
    }
    LayerGuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "layer_gui_test.moc"
