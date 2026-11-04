// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include "window.h"
#include <QAction>
#include <QCheckBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class WorkspaceTests : public QObject {
    Q_OBJECT
  private slots:
    void transparencyMatchesPixelGrid() {
        nova::Metrics metrics;
        QImage pixels(16, 16, QImage::Format_RGBA8888);
        pixels.fill(Qt::transparent);
        nova::Canvas canvas(std::make_shared<nova::Document>(QString(), pixels), &metrics);
        canvas.resize(640, 600);
        canvas.grid = true;
        canvas.show();
        for (double zoom : {8.0, 16.0, 24.0, 7.5}) {
            canvas.zoom = zoom;
            auto screen = canvas.grab().toImage();
            const auto origin = QPointF(canvas.width()/2.0, canvas.height()/2.0) -
                                QPointF(8*zoom, 8*zoom);
            for (int y = 4; y < 10; ++y)
                for (int x = 4; x < 10; ++x) {
                    auto sample = [&](double dx, double dy) {
                        auto p = origin + QPointF((x+dx)*zoom, (y+dy)*zoom);
                        return screen.pixel((p * screen.devicePixelRatio()).toPoint());
                    };
                    QCOMPARE(sample(.25, .25), sample(.7, .7));
                    QCOMPARE(sample(.25, .7), sample(.7, .25));
                    QVERIFY(sample(.25, .25) != sample(1.25, .25));
                }
        }
        auto evidence = qEnvironmentVariable("NOVA_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            canvas.zoom = 24;
            QVERIFY(canvas.grab().save(evidence + "/pixel-aligned-checker.png"));
        }
    }
    void previewResourceLifetime() {
        nova::Metrics metrics;
        nova::Cube cube(&metrics);
        cube.resize(320, 240);
        cube.show();
        QImage pixels(1024, 1024, QImage::Format_RGBA8888);
        pixels.fill(Qt::red);
        auto doc = std::make_shared<nova::Document>(QString(), pixels);
        std::weak_ptr<nova::Document> weak = doc;
        cube.setDocument(doc);
        QTRY_VERIFY(metrics.counters.value("texture_allocations").toInt() == 1);
        doc.reset();
        cube.setDocument({});
        QVERIFY(weak.expired());
        QCOMPARE(metrics.counters.value("texture_releases").toInt(), 1);
        cube.setDocument(std::make_shared<nova::Document>(QString(), pixels));
        QTRY_VERIFY(metrics.counters.value("texture_allocations").toInt() == 2);
        cube.setDocument({});
        QCOMPARE(metrics.counters.value("texture_releases").toInt(), 2);
    }
    void largeFixtureResponsive() {
        auto root = qEnvironmentVariable("NOVA_LARGE_FIXTURE");
        if (root.isEmpty())
            QSKIP("Set NOVA_LARGE_FIXTURE to run the 50,000-asset integration test");
        QTemporaryDir tmp;
        qApp->setProperty("settingsFile", tmp.path() + "/settings.ini");
        QElapsedTimer time;
        time.start();
        qint64 last = 0, maxGap = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            auto now = time.elapsed();
            maxGap = std::max(maxGap, now - last);
            last = now;
        });
        heartbeat.start();
        nova::Window window(root, {}, time);
        window.show();
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
        const auto startupGap = maxGap;
        maxGap = 0;
        last = time.elapsed();
        auto tree = window.findChild<QTreeView *>("assetTree");
        auto proxy = qobject_cast<QSortFilterProxyModel *>(tree->model());
        auto source = static_cast<nova::AssetModel *>(proxy->sourceModel());
        auto search = window.findChild<QLineEdit *>("assetSearch");
        // A broad query forces filtering through the entire hierarchy.
        search->setFocus();
        QTest::keyClicks(search, "texture");
        QCOMPARE(search->text(), QString("texture"));
        for (auto b : window.findChildren<QPushButton *>())
            if (b->text() == "Grid")
                b->click();
        QTest::qWait(500);
        search->setText("49999");
        QTest::qWait(100);
        search->clear();
        QTest::qWait(100);
        QVERIFY(source->rowCount() > 0);
#ifdef QT_NO_DEBUG
        constexpr qint64 responsivenessLimit = 500;
#else
        constexpr qint64 responsivenessLimit = 1000;
#endif
        QVERIFY2(maxGap < responsivenessLimit, qPrintable(QString("UI stalled for %1 ms").arg(maxGap)));
        qInfo() << "50k max heartbeat gaps (ms), startup/index:" << startupGap
                << "browsing/search:" << maxGap;
        auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        auto original = canvas->document->image;
        maxGap = 0;
        last = time.elapsed();
        int strokes = 0;
        QTimer edits;
        edits.setInterval(16);
        connect(&edits, &QTimer::timeout, &window, [&] {
            canvas->document->draw({0, 0}, {0, 0}, qRgba(strokes + 1, 40, 80, 255));
            canvas->document->finish();
            canvas->changed();
            if (++strokes == 20)
                edits.stop();
        });
        window.findChild<nova::DirectoryMonitor *>()->simulateOverflow();
        QTimer::singleShot(450, &window, [&] { edits.start(); });
        QTest::qWait(1500);
        QCOMPARE(strokes, 20);
        while (canvas->document->canUndo())
            canvas->document->undo();
        QCOMPARE(canvas->document->image, original);
        QVERIFY(!canvas->document->modified());
        QVERIFY2(maxGap < responsivenessLimit,
                 qPrintable(QString("Concurrent indexing/editing stalled for %1 ms").arg(maxGap)));
        qInfo() << "50k forced rescan + canvas editing max heartbeat gap (ms):" << maxGap;
        QVERIFY(window.close());
    }
    void navigationAndUnsavedTabs() {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath("assets/blocks");
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        for (auto name : {"stone.png", "glass.png"})
            QVERIFY(nova::savePng(tmp.path() + "/assets/blocks/" + name, image));
        qApp->setProperty("settingsFile", tmp.path() + "/preferences.ini");
        QElapsedTimer time;
        time.start();
        nova::Window window(tmp.path(), {}, time);
        window.show();
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        QTRY_COMPARE(tabs->count(), 1);
        auto tree = window.findChild<QTreeView *>("assetTree");
        auto grid = window.findChild<QListView *>("assetGrid");
        auto proxy = qobject_cast<QSortFilterProxyModel *>(tree->model());
        auto source = static_cast<nova::AssetModel *>(proxy->sourceModel());
        auto selected = proxy->mapFromSource(source->indexFor("assets/blocks/stone.png"));
        tree->setCurrentIndex(selected);
        auto search = window.findChild<QLineEdit *>("assetSearch");
        search->setFocus();
        QTest::keyClicks(search, "STONE");
        QCOMPARE(source->rowCount(source->indexFor("assets/blocks")), 2);
        auto button = [&](QString text) {
            for (auto b : window.findChildren<QPushButton *>())
                if (b->text() == text)
                    return b;
            return static_cast<QPushButton *>(nullptr);
        };
        QVERIFY(button("Grid"));
        button("Grid")->click();
        QCOMPARE(grid->currentIndex().data(Qt::UserRole).toString(),
                 QString("assets/blocks/stone.png"));
        QCOMPARE(search->text(), QString("STONE"));
        QCOMPARE(tabs->count(), 1);
        button("Tree")->click();
        QCOMPARE(tree->currentIndex().data(Qt::UserRole).toString(),
                 QString("assets/blocks/stone.png"));
        search->clear();
        selected = proxy->mapFromSource(source->indexFor("assets/blocks/stone.png"));
        emit tree->doubleClicked(selected);
        QCOMPARE(tabs->count(), 2);
        auto canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        canvas->document->draw({0, 0}, {2, 2}, qRgba(1, 2, 3, 255));
        canvas->document->finish();
        canvas->changed();
        QVERIFY(tabs->tabText(tabs->currentIndex()).contains(QChar(0x25cf)));
        auto choose = [&](QMessageBox::StandardButton choice) {
            QTimer::singleShot(30, &window, [choice] {
                auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box)
                    box->button(choice)->click();
            });
        };
        choose(QMessageBox::Cancel);
        emit tabs->tabCloseRequested(tabs->currentIndex());
        QCOMPARE(tabs->count(), 2);
        choose(QMessageBox::Save);
        emit tabs->tabCloseRequested(tabs->currentIndex());
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(nova::loadPng(tmp.path() + "/assets/blocks/stone.png").pixel(1, 1),
                 qRgba(1, 2, 3, 255));
        canvas = static_cast<nova::Canvas *>(tabs->currentWidget());
        canvas->document->draw({0, 0}, {0, 0}, qRgba(8, 8, 8, 255));
        canvas->document->finish();
        choose(QMessageBox::Discard);
        emit tabs->tabCloseRequested(0);
        QCOMPARE(tabs->count(), 0);
        QVERIFY(!QFileInfo::exists(tmp.path() + "/.nova"));
        window.close();
    }
    // Phase 3-E regression: opening a texture (animated or plain) must leave it
    // unmodified, so closing an untouched tab never prompts and writes nothing;
    // a genuine edit on the animated document still prompts, and Save persists
    // the editable sidecar without touching the game PNG.
    void untouchedTexturesCloseWithoutSavePrompt() {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath("assets/blocks");
        QImage plain(16, 16, QImage::Format_RGBA8888);
        plain.fill(Qt::blue);
        QVERIFY(nova::savePng(tmp.path() + "/assets/blocks/plain.png", plain));
        QImage sheet(16, 32, QImage::Format_RGBA8888);
        sheet.fill(Qt::green);
        QVERIFY(nova::savePng(tmp.path() + "/assets/blocks/anim.png", sheet));
        QFile mcmeta(tmp.path() + "/assets/blocks/anim.png.mcmeta");
        QVERIFY(mcmeta.open(QIODevice::WriteOnly));
        mcmeta.write(R"({"animation":{"frametime":5}})");
        mcmeta.close();
        qApp->setProperty("settingsFile", tmp.path() + "/preferences.ini");
        QElapsedTimer time;
        time.start();
        nova::Window window(tmp.path(), {}, time);
        window.show();
        auto tabs = window.findChild<QTabWidget *>("editorTabs");
        // The workspace auto-opens the first indexed texture on startup; it must
        // already be clean, whichever of the two it is.
        QTRY_COMPARE(tabs->count(), 1);
        auto startup = static_cast<nova::Canvas *>(tabs->widget(0));
        QVERIFY(!startup->document->modified());
        auto tree = window.findChild<QTreeView *>("assetTree");
        auto proxy = qobject_cast<QSortFilterProxyModel *>(tree->model());
        auto source = static_cast<nova::AssetModel *>(proxy->sourceModel());
        bool prompted = false;
        // One poller at a time answers the next prompt: a single fixed-delay
        // shot can miss when the dialog is not yet the active modal (under
        // load), and a stale poller must never answer a later prompt.
        QTimer *watch = nullptr;
        quintptr watchGeneration = 0;
        auto stopWatchdog = [&] {
            if (watch) {
                watch->stop();
                watch->deleteLater();
                watch = nullptr;
            }
        };
        auto armWatchdog = [&](QMessageBox::StandardButton choice) {
            stopWatchdog();
            ++watchGeneration;
            const auto generation = watchGeneration;
            watch = new QTimer(&window);
            watch->setInterval(25);
            QObject::connect(watch, &QTimer::timeout, &window, [&, choice, generation] {
                auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!box)
                    return;
                prompted = true;
                box->button(choice)->click();
                if (generation == watchGeneration)
                    stopWatchdog();
            });
            QTimer::singleShot(4000, &window, [&, generation] {
                if (generation == watchGeneration)
                    stopWatchdog();
            });
            watch->start();
        };
        // The index settles asynchronously; keep mapping and emitting until the
        // tab appears instead of racing one incremental index update.
        auto openThroughTree = [&](const char *asset, int expected) {
            QTRY_COMPARE([&] {
                emit tree->doubleClicked(proxy->mapFromSource(source->indexFor(asset)));
                return tabs->count();
            }(), expected);
        };
        // Open the other texture so an animated and a plain document are both
        // present, then identify them by kind rather than by open order.
        openThroughTree(startup->document->animated() ? "assets/blocks/plain.png"
                                                      : "assets/blocks/anim.png",
                        2);
        auto canvasOf = [&](int i) { return static_cast<nova::Canvas *>(tabs->widget(i)); };
        nova::Canvas *animated = nullptr, *plainCanvas = nullptr;
        for (int i = 0; i < tabs->count(); ++i)
            if (canvasOf(i)->document->animated())
                animated = canvasOf(i);
            else
                plainCanvas = canvasOf(i);
        QVERIFY(animated && plainCanvas);
        QVERIFY(animated->document->animated());
        QVERIFY(!animated->document->modified());   // untouched animated open is clean
        QVERIFY(!animated->document->exportModified());
        QVERIFY(!plainCanvas->document->modified());
        QVERIFY(!tabs->tabText(tabs->indexOf(animated)).contains(QChar(0x25cf)));
        QVERIFY(!tabs->tabText(tabs->indexOf(plainCanvas)).contains(QChar(0x25cf)));
        QFile beforeFile(tmp.path() + "/assets/blocks/anim.png");
        QVERIFY(beforeFile.open(QIODevice::ReadOnly));
        const auto pngBefore = beforeFile.readAll();
        beforeFile.close();
        // Closing untouched tabs: no prompt, and nothing is written into the pack.
        auto closeQuietly = [&](nova::Canvas *canvas, int expected) {
            armWatchdog(QMessageBox::Discard); // fail-fast if a prompt appears
            emit tabs->tabCloseRequested(tabs->indexOf(canvas));
            QTRY_COMPARE(tabs->count(), expected);
            QVERIFY(!prompted);
            QVERIFY(!QApplication::activeModalWidget());
            stopWatchdog();
        };
        closeQuietly(plainCanvas, 1);
        closeQuietly(animated, 0);
        QVERIFY(!QFileInfo::exists(tmp.path() + "/.nova"));
        // A genuine edit on a reopened animated texture still prompts; Save
        // persists the editable sidecar and leaves the game PNG untouched.
        openThroughTree("assets/blocks/anim.png", 1);
        animated = canvasOf(tabs->currentIndex());
        QVERIFY(animated->document->animated());
        QVERIFY(!animated->document->modified()); // reopen of the untouched pair is clean
        animated->document->draw({0, 0}, {2, 2}, qRgba(1, 2, 3, 255));
        animated->document->finish();
        animated->changed();
        QVERIFY(animated->document->modified());
        QVERIFY(tabs->tabText(tabs->currentIndex()).contains(QChar(0x25cf)));
        prompted = false;
        armWatchdog(QMessageBox::Save);
        emit tabs->tabCloseRequested(tabs->currentIndex());
        QTRY_COMPARE(tabs->count(), 0);
        stopWatchdog();
        QVERIFY(prompted);
        QVERIFY(QFileInfo::exists(tmp.path() + "/.nova/documents"));
        QFile afterFile(tmp.path() + "/assets/blocks/anim.png");
        QVERIFY(afterFile.open(QIODevice::ReadOnly));
        QCOMPARE(afterFile.readAll(), pngBefore);
        window.close();
    }
    void settingsPersistence() {
        QTemporaryDir tmp;
        nova::Settings settings(tmp.path() + "/prefs.ini");
        auto p = settings.load();
        QVERIFY(p.dark);
        QCOMPARE(p.thumbnailSize, 72);
        p.dark = false;
        p.gridMode = true;
        p.thumbnailSize = 112;
        p.pixelGrid = false;
        p.accent = "blue";
        p.recentProjects = {"C:/pack"};
        p.layout = "layout";
        QVERIFY(settings.save(p));
        auto read = settings.load();
        QVERIFY(!read.dark);
        QVERIFY(read.gridMode);
        QVERIFY(!read.pixelGrid);
        QCOMPARE(read.thumbnailSize, 112);
        QCOMPARE(read.layout, p.layout);
        QCOMPARE(read.recentProjects, p.recentProjects);
        QCOMPARE(read.accent, p.accent);
        QSettings invalid(settings.fileName(), QSettings::IniFormat);
        invalid.setValue("explorer/thumbnailSize", "garbage");
        invalid.setValue("appearance/accent", "bad");
        invalid.sync();
        QCOMPARE(settings.load().thumbnailSize, 72);
        QCOMPARE(settings.load().accent, "violet");
        QFile blocker(tmp.path() + "/blocked");
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("preserve");
        blocker.close();
        QVERIFY(!nova::Settings(blocker.fileName() + "/prefs.ini").save(p));
        QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), QByteArray("preserve"));
    }
    void screenRecovery() {
        QList<QRect> screens{QRect(0, 0, 1920, 1080)};
        auto recovered = nova::Settings::visibleGeometry(QRect(8000, -200, 1280, 820), screens);
        QVERIFY(screens.first().contains(recovered));
        QCOMPARE(nova::Settings::visibleGeometry(QRect(100, 100, 800, 550), screens),
                 QRect(100, 100, 800, 550));
    }
    void layoutRoundtrip() {
        QTemporaryDir tmp;
        qApp->setProperty("settingsFile", tmp.path() + "/workspace.ini");
        QElapsedTimer time;
        time.start();
        {
            nova::Window window({}, {}, time);
            auto dock = window.findChild<QDockWidget *>("preview");
            QVERIFY(dock);
            window.addDockWidget(Qt::RightDockWidgetArea, dock);
            window.findChild<QCheckBox *>("pixelGrid")->setChecked(false);
            QVERIFY(window.close());
        }
        {
            nova::Window restored({}, {}, time);
            QCOMPARE(restored.dockWidgetArea(restored.findChild<QDockWidget *>("preview")),
                     Qt::RightDockWidgetArea);
            QVERIFY(!restored.findChild<QCheckBox *>("pixelGrid")->isChecked());
            restored.findChild<QAction *>("resetWorkspaceCommand")->trigger();
            QCOMPARE(restored.dockWidgetArea(restored.findChild<QDockWidget *>("preview")),
                     Qt::BottomDockWidgetArea);
            QVERIFY(restored.close());
        }
        QVERIFY(!QFileInfo::exists(tmp.path() + "/.nova"));
        QSettings corrupted(tmp.path() + "/workspace.ini", QSettings::IniFormat);
        corrupted.setValue("workspace/layout", QByteArray("not a Qt layout"));
        corrupted.setValue("workspace/geometry", QByteArray("not Qt geometry"));
        corrupted.sync();
        nova::Window recovered({}, {}, time);
        QCOMPARE(recovered.dockWidgetArea(recovered.findChild<QDockWidget *>("preview")),
                 Qt::BottomDockWidgetArea);
        QVERIFY(recovered.close());
    }
};
QTEST_MAIN(WorkspaceTests)
#include "workspace_test.moc"
