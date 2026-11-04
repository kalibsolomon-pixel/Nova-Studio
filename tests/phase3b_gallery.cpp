// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3-B native screenshot gallery and layout measurements.
// Drives the production Window through representative states and saves
// QWidget::grab() captures (native Qt Widgets/OpenGL rendering on Windows).
// It runs unchanged against the pre-3-B baseline: newer controls are optional.
#include "batchdialog.h"
#include "modelpanel.h"
#include "timeline.h"
#include "window.h"
#include <QtTest>
#include <QtWidgets>

namespace nova {
class EditorTestAccess {
  public:
    static ModelPanel *panel(Window &w) { return w.modelPanel; }
    static QDockWidget *models(Window &w) { return w.modelsDock; }
    static void problems(Window &w) { w.showProblems(true); }
    static void batch(Window &w) { w.batchDialog(); }
    static void palette(Window &w) { w.commandPalette(); }
    static bool busy(Window &w) { return w.indexBusy; }
};
} // namespace nova
using namespace nova;

namespace {
QString galleryDir() { return qEnvironmentVariable("NOVA_PHASE3B_GALLERY_DIR"); }
void copyTree(const QString &from, const QString &to) {
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto file = it.next();
        const auto relative = QDir(from).relativeFilePath(file);
        if (relative.startsWith(".nova"))
            continue;
        QDir().mkpath(QFileInfo(to + '/' + relative).absolutePath());
        QFile::copy(file, to + '/' + relative);
    }
}
// Click the least destructive button of any message box that appears (Discard
// for close prompts) so closing the window never blocks the run.
class AutoAnswer : public QTimer {
  public:
    AutoAnswer() {
        setInterval(25);
        connect(this, &QTimer::timeout, this, [] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                for (auto choice : {QMessageBox::Discard, QMessageBox::No, QMessageBox::Ok, QMessageBox::Close})
                    if (auto button = box->button(choice)) {
                        button->click();
                        return;
                    }
        });
        start();
    }
};
} // namespace

class Phase3BGallery : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QJsonObject measurements;
    QString pack;

    void capture(QWidget &widget, const QString &name) {
        QTest::qWait(140);
        QApplication::processEvents();
        QJsonObject m{{"width", widget.width()}, {"height", widget.height()}};
        if (auto window = qobject_cast<QMainWindow *>(&widget)) {
            if (auto central = window->centralWidget()) {
                const auto r = QRect(central->mapTo(window, QPoint()), central->size());
                m["central_width"] = r.width();
                m["central_height"] = r.height();
                m["central_share"] = double(r.width()) * r.height() / (double(window->width()) * window->height());
                m["chrome_top"] = r.top();
            }
            int visibleDocks = 0;
            for (auto dock : window->findChildren<QDockWidget *>())
                visibleDocks += dock->isVisible() && !dock->isFloating();
            m["visible_docks"] = visibleDocks;
            int toolbarRows = 0, top = -1;
            for (auto bar : window->findChildren<QToolBar *>())
                if (bar->isVisible() && window->toolBarArea(bar) == Qt::TopToolBarArea && bar->y() != top) {
                    ++toolbarRows;
                    top = bar->y();
                }
            m["top_toolbar_rows"] = toolbarRows;
        }
        measurements[name] = m;
        const auto dir = galleryDir();
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            QVERIFY(widget.grab().save(dir + '/' + name + ".png"));
        }
    }
    // Capture whichever modal dialog the callback opens, then close it.
    void captureModal(const QString &name, std::function<void()> open) {
        auto timer = new QTimer(this);
        timer->setInterval(40);
        auto elapsed = std::make_shared<QElapsedTimer>();
        elapsed->start();
        connect(timer, &QTimer::timeout, this, [=, this] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog && elapsed->elapsed() > 250) {
                timer->stop();
                timer->deleteLater();
                capture(*dialog, name);
                dialog->reject();
            } else if (elapsed->elapsed() > 8000) {
                timer->stop();
                timer->deleteLater();
            }
        });
        timer->start();
        open();
    }
    void setTheme(Window &window, bool dark) {
        if (auto theme = window.findChild<QAction *>("darkThemeCommand"))
            theme->setChecked(dark);
        QTest::qWait(60);
    }
    void preset(Window &window, const QString &name) {
        // 3-B workspace presets; the baseline has none.
        if (auto action = window.findChild<QAction *>("workspacePreset-" + name))
            action->trigger();
        QTest::qWait(60);
    }

  private slots:
    void initTestCase() {
        const auto fixture = QString(QFINDTESTDATA("../fixtures/phase-2c6-models"));
        QVERIFY(!fixture.isEmpty());
        pack = temporary.filePath("Gallery Pack");
        copyTree(fixture, pack);
        // A broader texture set so grid navigation has representative content.
        for (int i = 0; i < 24; ++i) {
            QImage image(16, 16, QImage::Format_RGBA8888);
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x)
                    image.setPixelColor(x, y, QColor::fromHsv((i * 15 + x * 3) % 360, 90 + (y * 9) % 120,
                                                              110 + ((x ^ y) % 4) * 30));
            QVERIFY(savePng(pack + QString("/assets/nova/textures/block/tile_%1.png").arg(i, 2, 10, QChar('0')), image));
        }
        qApp->setProperty("settingsFile", temporary.filePath("gallery.ini"));
    }
    void welcome() {
        QElapsedTimer timer;
        timer.start();
        Window window({}, {}, timer);
        window.resize(1280, 820);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        setTheme(window, true);
        capture(window, "01-welcome-dark");
        setTheme(window, false);
        capture(window, "01-welcome-light");
        window.resize(800, 550);
        capture(window, "01-welcome-compact-light");
        setTheme(window, true);
        QVERIFY(window.close());
    }
    void workspaces() {
        AutoAnswer answers;
        QElapsedTimer timer;
        timer.start();
        Window window(pack, {}, timer);
        window.resize(1920, 1080);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY_WITH_TIMEOUT(!EditorTestAccess::busy(window), 10000);
        QTRY_VERIFY(window.activeCanvas());
        setTheme(window, true);
        preset(window, "essentials");
        capture(window, "02-general-dark-1920");
        window.resize(1280, 800);
        QTest::qWait(60);
        preset(window, "essentials"); // presets size panels for the current window
        if (auto fit = window.findChild<QAction *>("fitCanvasCommand"))
            fit->trigger();
        capture(window, "02-general-dark-1280");
        // Asset explorer: grid mode with thumbnails.
        for (auto b : window.findChildren<QPushButton *>())
            if (b->text() == "Grid")
                b->click();
        capture(window, "03-asset-grid-dark");
        for (auto b : window.findChildren<QPushButton *>())
            if (b->text() == "Tree")
                b->click();
        // Texture editing with an active selection and the selection tool.
        preset(window, "texture");
        auto canvas = window.activeCanvas();
        canvas->zoom = 24;
        window.findChild<QAction *>("tool-rect-select")->trigger();
        window.findChild<QAction *>("selectAll")->trigger();
        capture(window, "04-texture-selection-dark");
        window.findChild<QAction *>("selectNone")->trigger();
        window.findChild<QAction *>("tool-brush")->trigger();
        capture(window, "04-texture-brush-dark");
        window.findChild<QAction *>("tool-shape")->trigger();
        capture(window, "04-texture-shape-dark");
        window.findChild<QAction *>("tool-pencil")->trigger();
        // Layers.
        if (auto add = window.findChild<QAbstractButton *>("addLayer"))
            add->click();
        else if (auto addAction = window.findChild<QAction *>("layerAdd"))
            addAction->trigger();
        if (auto layers = window.findChild<QDockWidget *>("layers")) {
            layers->show();
            layers->raise();
        }
        capture(window, "05-layers-dark");
        // Compact window sizes.
        auto sized = [&](int w, int h, const QString &name) {
            window.resize(w, h);
            QTest::qWait(60);
            preset(window, "texture");
            if (auto fit = window.findChild<QAction *>("fitCanvasCommand"))
                fit->trigger();
            capture(window, name);
        };
        sized(1024, 768, "14-compact-1024-dark");
        sized(800, 550, "14-compact-minimum-dark");
        sized(2560, 1440, "14-large-2560-dark");
        window.resize(1600, 960);
        // Command palette.
        captureModal("15-command-palette-dark", [&] { EditorTestAccess::palette(window); });
        // Animation.
        preset(window, "animation");
        window.findChild<QAction *>("convertAnimation")->trigger();
        QTest::qWait(60);
        if (auto add = window.findChild<QAbstractButton *>("duplicateFrame"))
            add->click();
        if (auto dock = window.findChild<QDockWidget *>("animationDock")) {
            dock->show();
            dock->raise();
        }
        capture(window, "07-animation-dark");
        // Model, UV and item display.
        preset(window, "model");
        auto panel = EditorTestAccess::panel(window);
        panel->openAsset("assets/nova/models/block/uv.json");
        EditorTestAccess::models(window)->show();
        EditorTestAccess::models(window)->raise();
        QTest::qWait(120);
        capture(window, "08-model-dark");
        preset(window, "uv");
        panel->selectFace(0);
        if (auto begin = panel->findChild<QPushButton *>("uvBegin"); begin && begin->isEnabled())
            begin->click();
        capture(window, "09-uv-dark");
        if (auto cancel = panel->findChild<QPushButton *>("uvCancel"))
            cancel->click();
        preset(window, "model");
        if (auto toggle = panel->findChild<QToolButton *>("displayToggle"))
            toggle->click();
        QTest::qWait(60);
        capture(window, "10-display-transforms-dark");
        // Validation / Problems.
        preset(window, "essentials");
        EditorTestAccess::problems(window);
        QTest::qWait(600);
        capture(window, "12-problems-dark");
        // Batch processing dialog.
        captureModal("11-batch-dark", [&] { EditorTestAccess::batch(window); });
        // Settings / preferences (3-B).
        if (auto preferences = window.findChild<QAction *>("preferencesCommand"))
            captureModal("13-settings-dark", [&] { preferences->trigger(); });
        if (auto shortcuts = window.findChild<QAction *>("keyboardShortcutsCommand")) {
            shortcuts->trigger();
            QTest::qWait(120);
            if (auto dialog = window.findChild<QDialog *>("keyboardShortcutsDialog")) {
                capture(*dialog, "16-shortcuts-dark");
                dialog->close();
            }
        }
        // Light theme.
        setTheme(window, false);
        window.resize(1600, 960);
        capture(window, "17-light-model");
        preset(window, "texture");
        capture(window, "17-light-texture");
        preset(window, "essentials");
        captureModal("17-light-batch", [&] { EditorTestAccess::batch(window); });
        setTheme(window, true);
        window.close();
        QTest::qWait(100);
        if (window.isVisible()) {
            QTest::qWait(500);
            window.close();
        }
    }
    void cleanupTestCase() {
        const auto dir = galleryDir();
        if (dir.isEmpty())
            return;
        measurements["device_pixel_ratio"] = qApp->devicePixelRatio();
        QFile file(dir + "/measurements.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(measurements).toJson());
    }
};
QTEST_MAIN(Phase3BGallery)
#include "phase3b_gallery.moc"
