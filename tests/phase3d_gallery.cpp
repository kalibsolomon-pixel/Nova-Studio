// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3-D native gallery: drives the production Window's "Test in
// Minecraft" dialog against disposable launcher fixtures (never the real
// launcher data) and saves QWidget::grab() captures plus layout measurements.
#include "minecraft_testing.h"
#include "window.h"
#include <QtTest>
#include <QtWidgets>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace nova;

namespace {
void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(bytes);
}
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
constexpr auto vanillaId = "0123456789abcdef0123456789abcdef";
} // namespace

class Phase3DGallery : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QJsonObject measurements;
    QString pack, launchers, empty;

    void capture(QWidget &widget, const QString &name) {
        QTest::qWait(120);
        QApplication::processEvents();
        const auto hint = widget.minimumSizeHint();
        measurements[name] =
            QJsonObject{{"width", widget.width()},
                        {"height", widget.height()},
                        {"min_width", hint.width()},
                        {"min_height", hint.height()},
                        {"dpr", widget.devicePixelRatioF()},
                        {"fits_screen", widget.frameGeometry().height() <=
                                            widget.screen()->availableGeometry().height()}};
        const auto dir = qEnvironmentVariable("NOVA_PHASE3D_GALLERY_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            QVERIFY(widget.grab().save(dir + '/' + name + ".png"));
        }
    }
    void setTheme(Window &window, bool dark) {
        if (auto theme = window.findChild<QAction *>("darkThemeCommand"))
            theme->setChecked(dark);
        QTest::qWait(60);
    }
    MinecraftTestDialog *open(Window &window) {
        auto action = window.findChild<QAction *>("testInMinecraft");
        if (!action)
            return nullptr;
        action->trigger();
        auto dialog = window.findChild<MinecraftTestDialog *>();
        if (dialog)
            QTest::qWaitForWindowExposed(dialog);
        return dialog;
    }

  private slots:
    void initTestCase() {
        const auto fixture = QString(QFINDTESTDATA("../fixtures/phase-2c6-models"));
        QVERIFY(!fixture.isEmpty());
        pack = temporary.filePath("Gallery Pack");
        copyTree(fixture, pack);
        launchers = temporary.filePath("launchers");
        write(launchers + "/pf86/Minecraft Launcher/MinecraftLauncher.exe", "MZ");
        write(launchers + "/roaming/.minecraft/launcher_profiles.json", R"({"profiles":{}})");
        write(launchers + "/local/Aurora Launcher Native Preview/aurora-native.exe", "MZ");
        write(launchers + "/local/Aurora Launcher Native Preview/native-package.ini",
              "[Package]\nIdentity=com.aurora.launcher.native-preview\n");
        write(launchers + "/local/com.aurora.launcher.native-preview/launcher/instances.json",
              QString(R"({"schemaVersion":6,"instances":[
          {"id":"%1","displayName":"Vanilla Test","state":"ready","installed":{"minecraftVersion":"1.21.11","platform":{"kind":"vanilla"}},"pack":null},
          {"id":"fedcba9876543210fedcba9876543210","displayName":"Fabulously Optimized","state":"ready","installed":{"minecraftVersion":"1.21.11","platform":{"kind":"fabric"}},"pack":{"provider":"modrinth"}}]})")
                  .arg(vanillaId)
                  .toUtf8());
        QDir().mkpath(launchers + "/local/com.aurora.launcher.native-preview/instances/" +
                      vanillaId);
        empty = temporary.filePath("no-launchers");
        QDir().mkpath(empty);
        qApp->setProperty("settingsFile", temporary.filePath("gallery.ini"));
    }
    void noLauncher() {
        qputenv("NOVA_TEST_LAUNCHER_ROOT", empty.toUtf8());
        QElapsedTimer timer;
        timer.start();
        Window window(pack, {}, timer);
        window.resize(1280, 820);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        setTheme(window, true);
        auto dialog = open(window);
        QVERIFY(dialog);
        QVERIFY(dialog->targets().isEmpty());
        QVERIFY(dialog->findChild<QPushButton *>("getOfficialLauncher")->isVisible());
        QVERIFY(dialog->findChild<QPushButton *>("getAuroraLauncher")->isVisible());
        QVERIFY(!dialog->findChild<QPushButton *>("minecraftSyncPack")->isEnabled());
        capture(*dialog, "01-no-launcher-dark");
        dialog->close();
        QVERIFY(window.close());
    }
    void launchersAndGame() {
        qputenv("NOVA_TEST_LAUNCHER_ROOT", launchers.toUtf8());
        QElapsedTimer timer;
        timer.start();
        Window window(pack, {}, timer);
        window.resize(1280, 820);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        setTheme(window, true);
        auto dialog = open(window);
        QVERIFY(dialog);
        QCOMPARE(dialog->targets().size(), 3);
        auto box = dialog->findChild<QComboBox *>("minecraftTestTarget");
        box->setCurrentIndex(box->findData(QString("aurora:") + vanillaId));
        capture(*dialog, "02-targets-dark");
        QVERIFY(dialog->syncPack());
        capture(*dialog, "03-synced-dark");
        const auto game =
            launchers + "/local/com.aurora.launcher.native-preview/instances/" + vanillaId;
        write(game + "/logs/latest.log",
              "[10:00:00] [Render thread/INFO]: Setting user: NovaTester\n"
              "[10:00:02] [Render thread/INFO]: Reloading ResourceManager: vanilla, file/Nova Test "
              "- Gallery Pack\n"
              "[10:00:03] [Worker-Main-4/WARN]: Unable to load model: 'nova:item/missing_part' "
              "referenced from: nova:item/lantern#inventory\n"
              "[10:00:04] [Render thread/INFO]: Created: 1024x512x4 "
              "minecraft:textures/atlas/blocks.png-atlas\n");
        dialog->monitor()->poll();
        QVERIFY(dialog->monitor()->state().packActive);
        QVERIFY(dialog->findChild<QLabel *>("minecraftTestGameStatus")
                    ->text()
                    .contains("Nova test pack active"));
        capture(*dialog, "04-game-running-dark");
        setTheme(window, false);
        capture(*dialog, "04-game-running-light");
        box->setCurrentIndex(box->findData(QString("official")));
        capture(*dialog, "05-official-light");
        QVERIFY(!dialog->findChild<QListWidget *>("minecraftTestFindings")
                     ->isVisible()); // empty list hidden
        // Mixed-DPI: real movement to a second monitor when the machine has one.
        const auto screens = QGuiApplication::screens();
        if (screens.size() > 1) {
            const auto before = dialog->devicePixelRatioF();
            const auto home = dialog->pos();
            dialog->move(screens[1]->availableGeometry().topLeft() + QPoint(40, 40));
            QTest::qWait(400);
            measurements["mixed_dpi"] =
                QJsonObject{{"before_dpr", before},
                            {"after_dpr", dialog->devicePixelRatioF()},
                            {"screen", dialog->screen()->name()},
                            {"screen_logical_dpi", dialog->screen()->logicalDotsPerInch()}};
            capture(*dialog, "06-second-screen-light");
            dialog->move(home);
            QTest::qWait(200);
        }
        // Keyboard: the target list is reachable and the default button is the hand-off.
        QVERIFY(dialog->findChild<QPushButton *>("minecraftOpenLauncher")->isDefault());
        QVERIFY(box->focusPolicy() & Qt::TabFocus);
        box->setCurrentIndex(
            box->findData(QString("aurora:") + vanillaId)); // monitor follows the selection
        dialog->close();
        QVERIFY(dialog->monitor()->active()); // still observing: the game is running
        {
            QFile log(game + "/logs/latest.log");
            QVERIFY(log.open(QIODevice::Append));
            log.write("[10:05:00] [Render thread/INFO]: Stopping!\n");
        }
        dialog->monitor()->poll();
        QVERIFY(!dialog->monitor()->active()); // stops polling once hidden and the game stopped
        setTheme(window, true);
        QVERIFY(window.close());
    }
    // Idle cost of the open dialog while it follows a running game's log,
    // compared with the same window after the dialog closes and the game stops.
    void dialogIdleCost() {
#ifdef Q_OS_WIN
        auto cpuMs = [] {
            FILETIME created, exited, kernel, user;
            GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
            auto ms = [](FILETIME t) {
                return (quint64(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 10000.0;
            };
            return ms(kernel) + ms(user);
        };
        auto measure = [&](int ms) {
            QTest::qWait(2000); // settle
            const auto before = cpuMs();
            QElapsedTimer wall;
            wall.start();
            QTest::qWait(ms);
            return (cpuMs() - before) / wall.elapsed() * 100.0;
        };
        qputenv("NOVA_TEST_LAUNCHER_ROOT", launchers.toUtf8());
        QElapsedTimer timer;
        timer.start();
        Window window(pack, {}, timer);
        window.resize(1280, 820);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto baseline = measure(20000);
        auto dialog = open(window);
        QVERIFY(dialog);
        auto box = dialog->findChild<QComboBox *>("minecraftTestTarget");
        box->setCurrentIndex(box->findData(QString("aurora:") + vanillaId));
        const auto game =
            launchers + "/local/com.aurora.launcher.native-preview/instances/" + vanillaId;
        write(game + "/logs/latest.log", "[1] [Render thread/INFO]: Setting user: NovaTester\n"
                                         "[2] [Render thread/INFO]: Reloading ResourceManager: "
                                         "vanilla, file/Nova Test - Gallery Pack\n");
        dialog->monitor()->poll();
        QVERIFY(dialog->monitor()->active());
        const auto watching = measure(20000);
        {
            QFile log(game + "/logs/latest.log");
            QVERIFY(log.open(QIODevice::Append));
            log.write("[3] [Render thread/INFO]: Stopping!\n");
        }
        dialog->close();
        dialog->monitor()->poll();
        QVERIFY(!dialog->monitor()->active());
        const auto after = measure(20000);
        measurements["dialog_idle_cpu_one_core_percent"] =
            QJsonObject{{"window_before_dialog", baseline},
                        {"dialog_open_following_game_log", watching},
                        {"after_close_game_stopped", after},
                        {"window_seconds", 20}};
        qInfo().noquote() << QString("idle cpu %: before %1, dialog watching %2, after %3")
                                 .arg(baseline, 0, 'f', 3)
                                 .arg(watching, 0, 'f', 3)
                                 .arg(after, 0, 'f', 3);
        QVERIFY(watching < 1.0);
        QVERIFY(window.close());
#endif
    }
    void cleanupTestCase() {
        const auto dir = qEnvironmentVariable("NOVA_PHASE3D_GALLERY_DIR");
        if (!dir.isEmpty())
            write(dir + "/measurements.json", QJsonDocument(measurements).toJson());
    }
};

QTEST_MAIN(Phase3DGallery)
#include "phase3d_gallery.moc"
