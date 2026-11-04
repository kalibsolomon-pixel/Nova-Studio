// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3-D.1 native gallery and live acceptance driver.
// Fixture mode (default): the production FabricTestDialog in every state,
// driven by a scripted fake Gradle in a disposable folder.
// Live mode (NOVA_PHASE3D1_LIVE=1): the production Window's Test in Minecraft
// action against the real default runtime: real downloads, real Gradle, a real
// Fabric development client. Results are written as JSON; nothing is simulated.
#include "fabric_testing.h"
#include "window.h"
#include <QtTest>
#include <QtWidgets>

using namespace nova;
using namespace nova::testing;

namespace {
void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(bytes);
}
void fakeJdk(const QString &home, const QString &version) {
    write(home + "/release", QString("JAVA_VERSION=\"%1\"\n").arg(version).toUtf8());
    write(home + "/bin/java.exe", "MZ");
    write(home + "/bin/javac.exe", "MZ");
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
// Accepts confirmation prompts (first-run setup, stop requests) in live mode.
class AutoAccept : public QTimer {
  public:
    QStringList seen;
    AutoAccept() {
        setInterval(50);
        connect(this, &QTimer::timeout, this, [this] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                seen << box->windowTitle() + ": " + box->text().left(160);
                // Yes/OK for setup and stop prompts; Discard for the existing
                // close-time prompt about an untouched imported animation.
                for (auto choice : {QMessageBox::Yes, QMessageBox::Ok, QMessageBox::Discard})
                    if (auto button = box->button(choice)) {
                        button->click();
                        return;
                    }
            }
        });
        start();
    }
};
} // namespace

class Phase3D1Gallery : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QJsonObject measurements;
    QString pack;
    int scriptNumber = 0;

    QString dir() const { return qEnvironmentVariable("NOVA_PHASE3D1_GALLERY_DIR"); }
    void capture(QWidget &widget, const QString &name) {
        QTest::qWait(150);
        QApplication::processEvents();
        measurements[name] =
            QJsonObject{{"width", widget.width()},
                        {"height", widget.height()},
                        {"dpr", widget.devicePixelRatioF()},
                        {"fits_screen", widget.frameGeometry().height() <=
                                            widget.screen()->availableGeometry().height()}};
        if (!dir().isEmpty()) {
            QDir().mkpath(dir());
            QVERIFY(widget.grab().save(dir() + '/' + name + ".png"));
        }
    }
    void setTheme(Window &window, bool dark) {
        if (auto theme = window.findChild<QAction *>("darkThemeCommand"))
            theme->setChecked(dark);
        QTest::qWait(60);
    }
    void script(const char *variable, const QString &body) {
        const auto path = temporary.filePath(QString("script-%1.txt").arg(++scriptNumber));
        write(path, body.toUtf8());
        qputenv(variable, path.toUtf8());
    }
    FabricTestDialog::Context fixtureContext(const QString &base) {
        auto profile = *fabricProfile("1.21.11");
        profile.mods.clear(); // no downloads in fixture mode
        FabricTestDialog::Context context;
        context.projectRoot = [this] { return pack; };
        context.packName = [] { return QString("Gallery Pack"); };
        context.unsavedDocuments = [] { return QStringList{}; };
        context.interactive = false;
        context.runtime.base = base;
        context.runtime.environment = Environment{temporary.filePath("env"), {}, {}, {}};
        context.runtime.profile = profile;
        context.runtime.processProgram =
            QCoreApplication::applicationDirPath() + "/nova-fake-gradle.exe";
        context.runtime.stopGraceMs = 3000;
        fakeJdk(base + "/java/temurin-21", "21.0.13");
        fakeJdk(base + "/java/temurin-25", "25.0.5");
        return context;
    }

  private slots:
    void initTestCase() {
        const auto fixture = QString(QFINDTESTDATA("../fixtures/phase-2c6-models"));
        QVERIFY(!fixture.isEmpty());
        pack = temporary.filePath("Gallery Pack");
        copyTree(fixture, pack);
        qApp->setProperty("settingsFile", temporary.filePath("gallery.ini"));
        qunsetenv("JAVA_HOME");
    }
    void fixtureStates() {
        if (qEnvironmentVariableIntValue("NOVA_PHASE3D1_LIVE"))
            QSKIP("live mode");
        QElapsedTimer timer;
        timer.start();
        Window window(pack, {}, timer);
        window.resize(1280, 820);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        setTheme(window, true);
        const auto base = temporary.filePath("runtime");
        FabricTestDialog dialog(fixtureContext(base), &window);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(dialog.findChild<QPushButton *>("fabricTest")->text().contains("Prepare"));
        capture(dialog, "01-first-run-dark");
        script("NOVA_FAKE_GRADLE_PREPARE",
               "print Downloading "
               "https://services.gradle.org/distributions/gradle-9.8.2-bin.zip\nsleep 1500\nprint "
               "> Task :downloadAssets\nsleep 1500\nprint BUILD SUCCESSFUL\n");
        dialog.runtime()->prepare();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.runtime()->state(),
                                  FabricRuntime::State::GeneratingRunConfig, 15000);
        QTest::qWait(300);
        QVERIFY(dialog.findChild<QProgressBar *>("fabricProgress")->isVisible());
        QCOMPARE(dialog.findChild<QProgressBar *>("fabricProgress")->maximum(),
                 0); // unknown work: no fake percentage
        capture(dialog, "02-preparing-dark");
        QTRY_COMPARE_WITH_TIMEOUT(dialog.runtime()->state(), FabricRuntime::State::Ready, 20000);
        capture(dialog, "03-ready-dark");
        setTheme(window, false);
        capture(dialog, "03-ready-light");
        // Running with Minecraft's own log reporting the pack and one warning.
        script("NOVA_FAKE_GRADLE_RUN",
               "print > Task :runClient\n"
               "print [1] [main/INFO] (FabricLoader/GameProvider) Loading Minecraft 1.21.11 with "
               "Fabric Loader 0.20.1\n"
               "print [1] [main/INFO] (FabricLoader) Loading 3 mods:\nprint \t- rrls "
               "5.1.15+mc.1.21.11\n"
               "print [2] [Render thread/INFO] (Minecraft) Setting user: Player1\n"
               "print [3] [Render thread/INFO] (Minecraft) Sound engine started\nwindow\n");
        const auto game = dialog.runtime()->layout().game;
        dialog.testInMinecraft();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.runtime()->readyForInput(), 20000);
        write(game + "/logs/latest.log",
              "[2] [Render thread/INFO]: Setting user: Player1\n"
              "[2] [Render thread/INFO]: Reloading ResourceManager: vanilla, fabric-api, file/Nova "
              "Test - Gallery Pack\n"
              "[3] [Worker-Main-4/WARN]: Unable to load model: 'nova:item/missing_part' referenced "
              "from: nova:item/lantern#inventory\n"
              "[3] [Render thread/INFO]: Created: 1024x512x4 "
              "minecraft:textures/atlas/blocks.png-atlas\n");
        dialog.monitor()->poll();
        QVERIFY(
            dialog.findChild<QLabel *>("fabricReloadValue")->text().contains("Nova pack active"));
        capture(dialog, "04-running-light");
        setTheme(window, true);
        capture(dialog, "04-running-dark");
        QVERIFY(dialog.syncPack());
        capture(dialog, "05-synced-running-dark");
        QSignalSpy exited(dialog.runtime(), &FabricRuntime::gameExited);
        dialog.runtime()->requestStop();
        QVERIFY(exited.wait(10000));
        script("NOVA_FAKE_GRADLE_RUN", "print > Task :runClient\nprint [1] [main/ERROR] "
                                       "(FabricLoader) Failed to remap mods!\nexit 1\n");
        QSignalSpy failed(dialog.runtime(), &FabricRuntime::gameExited);
        dialog.testInMinecraft();
        QVERIFY(failed.wait(15000));
        QVERIFY(dialog.findChild<QLabel *>("fabricGameValue")->text().contains("Launch failed"));
        capture(dialog, "06-launch-failed-dark");
        dialog.findChild<QPushButton *>("fabricAdvancedToggle")->click();
        setTheme(window, false);
        capture(dialog, "07-settings-light");
        setTheme(window, true);
        dialog.close();
        QVERIFY(!dialog.monitor()->active()); // no polling while closed and idle
        QVERIFY(window.close());
    }
    // Real runtime through the production menu actions. Every value recorded
    // here comes from the running system; nothing is simulated.
    void live() {
        if (!qEnvironmentVariableIntValue("NOVA_PHASE3D1_LIVE"))
            QSKIP("set NOVA_PHASE3D1_LIVE=1 (and QTEST_FUNCTION_TIMEOUT) for the live run");
        const auto livePack = qEnvironmentVariable("NOVA_PHASE3D1_PACK");
        QVERIFY(QFileInfo(livePack + "/pack.mcmeta").isFile());
        AutoAccept accept;
        QElapsedTimer timer;
        timer.start();
        Window window(livePack, {}, timer);
        window.resize(1280, 820);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        setTheme(window, true);
        auto testAction = window.findChild<QAction *>("testInMinecraft");
        auto syncAction = window.findChild<QAction *>("syncTestPack");
        QVERIFY(testAction && syncAction);
        QJsonObject live;
        auto gameLog = [](FabricRuntime *engine) {
            QFile f(engine->layout().game + "/logs/latest.log");
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        auto stopAndRecord = [&](FabricRuntime *engine, const QString &prefix) {
            QSignalSpy exited(engine, &FabricRuntime::gameExited);
            QElapsedTimer stop;
            stop.start();
            engine->requestStop(); // WM_CLOSE to the game window
            QVERIFY(exited.wait(180000));
            live[prefix + "_stop_ms"] = stop.elapsed();
            live[prefix + "_exit_code"] = exited.first().first().toInt();
            live[prefix + "_exit_phase"] = gameText(engine->game());
            live[prefix + "_log_saw_stopping"] = gameLog(engine).contains("Stopping!");
            live[prefix + "_processes_left"] = engine->running();
        };
        QElapsedTimer action;
        action.start();
        testAction->trigger();
        auto dialog = window.findChild<FabricTestDialog *>();
        QVERIFY(dialog);
        auto engine = dialog->runtime();
        live["runtime_base"] = QDir::toNativeSeparators(engine->layout().base);
        live["prepared_metadata"] = engine->metadata().value("prepared");
        if (engine->running()) {
            // A client from the earlier (aborted) harness: re-attach, no duplicate.
            const auto before =
                engine->metadata().value("session").toObject().value("pid").toInteger();
            live["reattached_existing_session"] = true;
            QTest::qWait(1500);
            live["reattach_kept_single_session"] =
                engine->metadata().value("session").toObject().value("pid").toInteger() == before;
            capture(*dialog, "live-00-reattached");
            stopAndRecord(engine, "reattached");
            action.restart();
            testAction->trigger();
        }
        QTest::qWait(800);
        capture(*dialog, "live-01-starting");
        QTRY_VERIFY_WITH_TIMEOUT(
            engine->running() || engine->state() == FabricRuntime::State::Failed, 1800000);
        QVERIFY2(engine->state() != FabricRuntime::State::Failed, qPrintable(engine->error()));
        live["cached_prepare_ms"] = engine->lastPrepareMs();
        QTRY_VERIFY_WITH_TIMEOUT(
            engine->readyForInput() || engine->game() == FabricRuntime::Game::LaunchFailed, 600000);
        live["action_to_main_menu_ms"] = action.elapsed();
        live["game_phase"] = gameText(engine->game());
        live["launch_error"] = engine->error();
        QVERIFY2(engine->readyForInput(), qPrintable(engine->error()));
        QJsonObject mods;
        for (const auto &id :
             {"rrls", "forgeconfigapiport", "fabric-api", "fabricloader", "minecraft", "java"})
            if (engine->components().loadedMods.contains(id))
                mods.insert(id, engine->components().loadedMods.value(id));
        live["fabric_loaded_mods"] = mods;
        live["loaded_mod_count"] = engine->components().loadedMods.size();
        QTRY_VERIFY_WITH_TIMEOUT(dialog->monitor()->state().reloads > 0 &&
                                     (dialog->monitor()->state().reloadFinished ||
                                      dialog->monitor()->state().resourcesLoaded),
                                 120000);
        QTest::qWait(8000); // let RRLS report the end of the reload
        live["monitor_rrls_reload_finished"] = dialog->monitor()->state().reloadFinished;
        live["monitor_atlas_rebuilt"] = dialog->monitor()->state().resourcesLoaded;
        live["monitor_pack_active"] = dialog->monitor()->state().packActive;
        live["monitor_reload_includes_pack"] = dialog->monitor()->state().lastReload.contains(
            "file/Nova Test - " + QFileInfo(livePack).fileName());
        live["monitor_warnings"] = QJsonArray::fromStringList(dialog->monitor()->state().warnings);
        live["rrls_log_lines"] =
            gameLog(engine).count("(rrls)") +
            gameLog(engine).count("[Render thread/INFO]: Creating new resource manager");
        capture(*dialog, "live-02-running");
        // Edit a texture on disk (as a save would), then Sync Test Pack while running.
        const auto stonePath = livePack + "/assets/minecraft/textures/block/stone.png";
        QImage stone(stonePath);
        QVERIFY(!stone.isNull());
        const auto original = stone.convertToFormat(QImage::Format_RGBA8888);
        auto edited = original;
        edited.invertPixels(QImage::InvertRgb);
        QVERIFY(edited.save(stonePath));
        QElapsedTimer sync;
        sync.start();
        syncAction->trigger();
        live["sync_ms"] = sync.elapsed();
        live["synced_texture_matches_edit"] =
            QImage(engine->layout().game + "/resourcepacks/Nova Test - " +
                   QFileInfo(livePack).fileName() + "/assets/minecraft/textures/block/stone.png")
                .convertToFormat(QImage::Format_RGBA8888) == edited;
        const auto session =
            engine->metadata().value("session").toObject().value("pid").toInteger();
        testAction->trigger(); // already running: sync only, never a second client
        QTest::qWait(2000);
        live["second_test_action_kept_session"] =
            engine->running() &&
            engine->metadata().value("session").toObject().value("pid").toInteger() == session;
        capture(*dialog, "live-03-synced");
        QVERIFY(original.save(stonePath)); // leave the pack as it was
        stopAndRecord(engine, "live");
        capture(*dialog, "live-04-exited");
        // Repeated launch reuses everything prepared.
        action.restart();
        testAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(
            engine->readyForInput() || engine->game() == FabricRuntime::Game::LaunchFailed, 600000);
        live["repeat_action_to_main_menu_ms"] = action.elapsed();
        live["repeat_prepare_ms"] = engine->lastPrepareMs();
        stopAndRecord(engine, "repeat");
        live["prompts_accepted"] = QJsonArray::fromStringList(accept.seen);
        measurements["live"] = live;
        QVERIFY(window.close());
    }
    void cleanupTestCase() {
        if (!dir().isEmpty())
            write(dir() + "/measurements.json", QJsonDocument(measurements).toJson());
    }
};

QTEST_MAIN(Phase3D1Gallery)
#include "phase3d1_gallery.moc"
