// SPDX-License-Identifier: GPL-3.0-or-later
#include "fabric_testing.h"
#include "theme.h"
#include "icon_parity.h"
#include "packsync.h"
#include "settings.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace nova {
using namespace testing;

namespace {
QLabel *value(const QString &name) {
    auto label = new QLabel;
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
} // namespace

FabricTestDialog::FabricTestDialog(Context ctx, QWidget *parent)
    : QDialog(parent), context(std::move(ctx)), log(new GameLogMonitor(this)),
      liveSyncTimer(new QTimer(this)) {
    setObjectName("fabricTestDialog");
    setWindowTitle("Test in Minecraft");
    setMinimumWidth(560);
    auto layout = new QVBoxLayout(this);
    auto intro = new QLabel(
        "Nova runs its own Minecraft development client (Fabric with Remove Reloading Screen) in a "
        "separate folder for resource-pack testing. It does not sign in, prove Minecraft ownership "
        "or "
        "support online play, and it never touches your normal Minecraft worlds.");
    intro->setObjectName("fabricTestIntro");
    intro->setWordWrap(true);
    intro->setProperty("novaRole", "secondary");
    layout->addWidget(intro);

    auto statusBox = new QGroupBox("Test environment");
    auto form = new QFormLayout(statusBox);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    versionValue = value("fabricVersionValue");
    runtimeValue = value("fabricRuntimeValue");
    rrlsValue = value("fabricRrlsValue");
    gameValue = value("fabricGameValue");
    syncValue = value("fabricSyncValue");
    reloadValue = value("fabricReloadValue");
    form->addRow("Minecraft", versionValue);
    form->addRow("Runtime", runtimeValue);
    form->addRow("Reloading screen", rrlsValue);
    form->addRow("Game", gameValue);
    form->addRow("Last sync", syncValue);
    form->addRow("Last reload", reloadValue);
    progress = new QProgressBar;
    progress->setObjectName("fabricProgress");
    progress->setTextVisible(false);
    progress->setMaximumHeight(6);
    progress->hide();
    form->addRow(progress);
    layout->addWidget(statusBox);

    guidance = value("fabricGuidance");
    guidance->setProperty("novaRole", "secondary");
    layout->addWidget(guidance);
    findings = new QListWidget;
    findings->setObjectName("fabricFindings");
    findings->setAccessibleName("Minecraft resource warnings");
    findings->setWordWrap(true);
    findings->setMinimumHeight(64);
    findings->setMaximumHeight(150);
    findings->hide();
    layout->addWidget(findings);
    connect(findings, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const auto assets = item->data(Qt::UserRole).toStringList();
        if (!assets.isEmpty() && this->context.openAsset)
            this->context.openAsset(assets.first());
    });
    ui::afterEvents(findings, {QEvent::PaletteChange}, [this] {
        renderedReload = -1;
        refreshFindings();
    });

    // Advanced settings stay collapsed: ordinary use needs none of them.
    advancedButton = new QPushButton("Advanced ▸");
    advancedButton->setObjectName("fabricAdvancedToggle");
    advancedButton->setFlat(true);
    advancedButton->setCheckable(true);
    layout->addWidget(advancedButton, 0, Qt::AlignLeft);
    advanced = new QWidget;
    advanced->setObjectName("fabricAdvanced");
    auto advancedForm = new QFormLayout(advanced);
    versionBox = new QComboBox;
    versionBox->setObjectName("fabricMinecraftVersion");
    for (const auto &profile : fabricProfiles())
        versionBox->addItem(profile.minecraftVersion);
    advancedForm->addRow("Minecraft version", versionBox);
    javaValue = value("fabricJavaValue");
    javaButton = new QPushButton("Choose JDK…");
    javaButton->setObjectName("fabricChooseJava");
    auto autoJava = new QPushButton("Automatic");
    autoJava->setObjectName("fabricAutomaticJava");
    auto javaRow = new QHBoxLayout;
    javaRow->addWidget(javaValue, 1);
    javaRow->addWidget(javaButton);
    javaRow->addWidget(autoJava);
    advancedForm->addRow("Java for Minecraft", javaRow);
    locationValue = value("fabricLocationValue");
    locationButton = new QPushButton("Change…");
    locationButton->setObjectName("fabricChangeLocation");
    auto locationRow = new QHBoxLayout;
    locationRow->addWidget(locationValue, 1);
    locationRow->addWidget(locationButton);
    advancedForm->addRow("Location", locationRow);
    openWorld = new QCheckBox("Open “Nova Test World” directly when it exists");
    openWorld->setObjectName("fabricOpenWorld");
    detailedLogs = new QCheckBox("Keep detailed Gradle logs");
    detailedLogs->setObjectName("fabricDetailedLogs");
    autoReload = new QCheckBox("Reload Minecraft's resources after each sync");
    autoReload->setObjectName("fabricAutoReload");
    liveReload = new QCheckBox("Sync and reload while Minecraft is running");
    liveReload->setObjectName("fabricLiveReload");
    liveReload->setToolTip("Sync saved pack files after a short pause, then reload the running game.");
    advancedForm->addRow(openWorld);
    advancedForm->addRow(autoReload);
    advancedForm->addRow(liveReload);
    advancedForm->addRow(detailedLogs);
    repairButton = new QPushButton("Repair runtime…");
    repairButton->setObjectName("fabricRepair");
    advancedForm->addRow(repairButton);
    advanced->hide();
    layout->addWidget(advanced);

    auto buttons = new QHBoxLayout;
    testButton = new QPushButton("Test in Minecraft");
    testButton->setObjectName("fabricTest");
    testButton->setDefault(true);
    testButton->setToolTip(
        "Prepare on first use, then sync the pack and start Minecraft (Ctrl+Shift+M)");
    syncButton = new QPushButton("Sync Pack");
    syncButton->setObjectName("fabricSync");
    syncButton->setToolTip("Export the saved pack into the test environment (Ctrl+Alt+M)");
    reloadButton = new QPushButton("Reload Resources");
    reloadButton->setObjectName("fabricReload");
    compareButton = new QPushButton("Compare Icons…");
    compareButton->setObjectName("fabricCompareIcons");
    compareButton->setToolTip("Compare Nova's inventory icons with the ones Minecraft draws");
    stopButton = new QPushButton("Stop Minecraft");
    stopButton->setObjectName("fabricStop");
    folderButton = new QPushButton("Open Game Folder");
    folderButton->setObjectName("fabricOpenFolder");
    logsButton = new QPushButton("Logs");
    logsButton->setObjectName("fabricLogs");
    auto close = new QPushButton("Close");
    buttons->addWidget(testButton);
    buttons->addWidget(syncButton);
    buttons->addWidget(reloadButton);
    buttons->addWidget(compareButton);
    buttons->addWidget(stopButton);
    buttons->addStretch();
    buttons->addWidget(folderButton);
    buttons->addWidget(logsButton);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    if (auto p = context.preferences) {
        versionBox->setCurrentText(p->fabricMinecraftVersion);
        detailedLogs->setChecked(p->fabricDetailedLogs);
        openWorld->setChecked(p->fabricOpenTestWorld);
        autoReload->setChecked(p->fabricAutoReload);
        liveReload->setChecked(p->fabricLiveReload);
    } else {
        openWorld->setChecked(true);
        autoReload->setChecked(true);
    }

    connect(advancedButton, &QPushButton::toggled, this, [this](bool open) {
        advanced->setVisible(open);
        advancedButton->setText(open ? "Advanced ▾" : "Advanced ▸");
        if (isVisible())
            fitToScreen();
    });
    connect(testButton, &QPushButton::clicked, this, &FabricTestDialog::testInMinecraft);
    connect(syncButton, &QPushButton::clicked, this, [this] { syncPack(); });
    connect(compareButton, &QPushButton::clicked, this, [this] {
        IconParityDialog::Context parity;
        parity.companion = engine->companion();
        parity.library = context.modelLibrary;
        parity.projectRoot = context.projectRoot;
        parity.folder = [this] { return engine->layout().game + '/' + testing::companionFolder; };
        auto dialog = new IconParityDialog(parity, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    connect(reloadButton, &QPushButton::clicked, this, [this] {
        QString error;
        if (!reloadInGame(&error))
            inform("Reload not started", error, true);
    });
    connect(stopButton, &QPushButton::clicked, this, [this] {
        if (engine->busy() || engine->game() == FabricRuntime::Game::Starting)
            engine->cancel();
        else if (engine->stopTimedOut() || engine->state() == FabricRuntime::State::Stopping) {
            if (ask("Stop Minecraft now",
                    "Minecraft has not closed. Stop it now? Unsaved progress in the test world "
                    "since its last autosave is lost."))
                engine->forceStop();
        } else if (ask("Stop Minecraft", "Ask Minecraft to close? It saves the test world first, "
                                         "like closing its window."))
            engine->requestStop();
    });
    connect(folderButton, &QPushButton::clicked, this, [this] {
        QDir().mkpath(engine->layout().game);
        QDesktopServices::openUrl(QUrl::fromLocalFile(engine->layout().game));
    });
    connect(logsButton, &QPushButton::clicked, this, [this] {
        QDir().mkpath(engine->layout().logs);
        QDesktopServices::openUrl(QUrl::fromLocalFile(engine->layout().logs));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto persist = [this] {
        if (auto p = context.preferences) {
            p->fabricMinecraftVersion = versionBox->currentText();
            p->fabricDetailedLogs = detailedLogs->isChecked();
            p->fabricOpenTestWorld = openWorld->isChecked();
            p->fabricAutoReload = autoReload->isChecked();
        }
    };
    connect(openWorld, &QCheckBox::toggled, this, persist);
    connect(autoReload, &QCheckBox::toggled, this, [this, persist] {
        persist();
        refresh();
    });
    connect(liveReload, &QCheckBox::toggled, this, [this](bool enabled) {
        if (auto p = context.preferences)
            p->fabricLiveReload = enabled;
        if (!enabled)
            cancelLiveSync();
    });
    liveSyncTimer->setSingleShot(true);
    liveSyncTimer->setTimerType(Qt::PreciseTimer);
    connect(liveSyncTimer, &QTimer::timeout, this, &FabricTestDialog::syncSavedPack);
    connect(detailedLogs, &QCheckBox::toggled, this, [this, persist] {
        persist();
        rebuildRuntime();
    });
    connect(versionBox, &QComboBox::currentTextChanged, this, [this, persist] {
        persist();
        rebuildRuntime();
    });
    connect(javaButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, "Choose a JDK folder");
        if (path.isEmpty())
            return;
        const auto java = inspectJavaHome(path, "chosen");
        if (!java || !java->jdk || java->major != engine->profile().gameJavaMajor) {
            inform("Unsuitable Java",
                   QString("Choose the folder of a Java %1 JDK (it contains bin\\javac.exe and a "
                           "release file).")
                       .arg(engine->profile().gameJavaMajor),
                   true);
            return;
        }
        if (auto p = context.preferences)
            p->fabricGameJava = java->path;
        rebuildRuntime();
    });
    connect(autoJava, &QPushButton::clicked, this, [this] {
        if (auto p = context.preferences)
            p->fabricGameJava.clear();
        rebuildRuntime();
    });
    connect(locationButton, &QPushButton::clicked, this, [this] {
        const auto path =
            QFileDialog::getExistingDirectory(this, "Choose a folder for the test environment");
        if (path.isEmpty())
            return;
        if (auto p = context.preferences)
            p->fabricLocation = QDir::fromNativeSeparators(path) + "/NovaStudio-fabric-runtime";
        rebuildRuntime();
    });
    connect(repairButton, &QPushButton::clicked, this, [this] {
        if (ask("Repair runtime",
                "Rebuild Nova's Fabric workspace and re-verify every download? Your test world, "
                "settings and synced packs are kept."))
            engine->prepare(true);
    });
    connect(log, &GameLogMonitor::changed, this, [this] {
        updateWarnings();
        refresh();
    });
    rebuildRuntime();
}

void FabricTestDialog::rebuildRuntime() {
    if (syncing || (engine && (engine->busy() || engine->running()))) {
        inform("Runtime in use",
               "Change runtime settings after Minecraft closes or preparation finishes.");
        refresh();
        return;
    }
    cancelLiveSync();
    auto options = context.runtime;
    if (auto p = context.preferences) {
        options.minecraftVersion = p->fabricMinecraftVersion;
        options.chosenGameJava = p->fabricGameJava;
        options.detailedLogs = p->fabricDetailedLogs;
        if (!p->fabricLocation.isEmpty())
            options.base = p->fabricLocation;
    }
    delete engine;
    engine = new FabricRuntime(options, this);
    connect(engine, &FabricRuntime::changed, this, &FabricTestDialog::refresh);
    connect(engine->companion(), &CompanionLink::reloadStarted, this, [this] {
        reloadNote = "Reloading in Minecraft…";
        refresh();
    });
    connect(engine->companion(), &CompanionLink::reloadFinished, this,
            &FabricTestDialog::companionReloadFinished);
    connect(engine->companion(), &CompanionLink::iconsFinished, this,
            &FabricTestDialog::resumeLiveSync);
    connect(engine, &FabricRuntime::preparationFinished, this,
            &FabricTestDialog::continueAfterPreparation);
    connect(engine, &FabricRuntime::gameExited, this, [this](int code) {
        cancelLiveSync();
        if (context.activity)
            context.activity(QString("Minecraft test client exited (code %1)").arg(code));
        updateMonitoring();
        refresh();
    });
    engine->attachExisting();
    updateMonitoring();
    refresh();
}

QString FabricTestDialog::packFolder() const {
    return testPackFolderName(context.packName ? context.packName() : QString());
}

bool FabricTestDialog::testWorldExists() const {
    return QFileInfo::exists(engine->layout().game + "/saves/" + testWorldName() + "/level.dat");
}

bool FabricTestDialog::ask(const QString &title, const QString &text) {
    return !context.interactive ||
           QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::Cancel,
                                 QMessageBox::Yes) == QMessageBox::Yes;
}

void FabricTestDialog::inform(const QString &title, const QString &text, bool warning) {
    if (context.activity)
        context.activity(title + ": " + text);
    if (!context.interactive)
        return;
    warning ? QMessageBox::warning(this, title, text) : QMessageBox::information(this, title, text);
}

void FabricTestDialog::testInMinecraft() {
    if (!context.projectRoot || context.projectRoot().isEmpty()) {
        inform("Open a resource pack", "Open a resource pack to test it in Minecraft.");
        return;
    }
    if (!isVisible())
        show();
    raise();
    if (!engine->supported())
        return refresh();
    if (engine->busy() || engine->state() == FabricRuntime::State::Launching ||
        engine->state() == FabricRuntime::State::Stopping)
        return refresh();
    if (engine->running()) {
        syncPack(); // never a second client
        return;
    }
    if (!engine->prepared() &&
        !ask("Prepare the test environment",
             QString("Nova will download and prepare its Minecraft test environment in\n%1\n\n"
                     "First use downloads about 1 GB: Gradle %2, Minecraft %3, Fabric Loader %4, "
                     "Fabric API, "
                     "Forge Config API Port, Remove Reloading Screen and, if this PC has none, "
                     "Java %5 and "
                     "%6 JDKs. Later launches reuse it.")
                 .arg(QDir::toNativeSeparators(engine->layout().base),
                      engine->profile().gradleVersion, engine->profile().minecraftVersion,
                      engine->profile().loaderVersion)
                 .arg(engine->profile().gameJavaMajor)
                 .arg(engine->profile().gradleJavaMinimum)))
        return;
    pendingLaunch = true;
    engine->prepare(); // quick when nothing changed; repairs the cache otherwise
}

void FabricTestDialog::continueAfterPreparation(bool ok) {
    if (context.activity && engine->lastPrepareMs() >= 0 && ok)
        context.activity(QString("Fabric runtime ready in %1 s")
                             .arg(engine->lastPrepareMs() / 1000.0, 0, 'f', 1));
    if (!pendingLaunch)
        return refresh();
    pendingLaunch = false;
    if (ok)
        launchAfterSync();
    refresh();
}

void FabricTestDialog::enablePackOnce() {
    // Pre-select the pack once per folder; afterwards the player's choice in
    // Options › Resource Packs is kept.
    auto enabled = engine->metadata().value("autoEnabledPacks").toArray();
    if (engine->running() || enabled.contains(packFolder()))
        return;
    QString error;
    if (enableTestPackOption(engine->layout().game, packFolder(), &error)) {
        enabled.append(packFolder());
        engine->recordMetadata("autoEnabledPacks", enabled);
    } else if (context.activity)
        context.activity("Could not pre-select the test pack: " + error);
}

void FabricTestDialog::launchAfterSync() {
    if (!syncPack())
        return;
    enablePackOnce();
    const auto world = openWorld->isChecked() && testWorldExists() ? testWorldName() : QString();
    QString error;
    if (!engine->launch(world, &error)) {
        inform("Minecraft did not start", error, true);
        return;
    }
    if (context.activity)
        context.activity(world.isEmpty() ? "Starting the Minecraft test client"
                                         : "Starting the Minecraft test client in " + world);
    updateMonitoring();
}

bool FabricTestDialog::syncPack() {
    return syncPack(false);
}

bool FabricTestDialog::canLiveSync() const {
    return liveReload->isChecked() && engine->running() && engine->companion()->available();
}

void FabricTestDialog::packFilesChanged(const QStringList &paths) {
    if (!canLiveSync())
        return;
    const auto root = context.projectRoot ? context.projectRoot() : QString();
    if (root.isEmpty())
        return;
    static const QRegularExpression output(
        R"(^(?:pack\.(?:mcmeta|png)|(?:[^/]+/)*assets/[^/]+/(?:.+\.png(?:\.mcmeta)?|(?:models|blockstates|items)/.+\.json))$)",
        QRegularExpression::CaseInsensitiveOption);
    for (const auto &path : paths)
        if (output.match(path).hasMatch() && QFileInfo(root + '/' + path).isFile()) {
            liveSyncPending = true;
            liveSyncTimer->start(1500);
            return;
        }
}

void FabricTestDialog::cancelLiveSync() {
    liveSyncPending = false;
    liveSyncTimer->stop();
}

void FabricTestDialog::resumeLiveSync() {
    if (liveSyncPending && !liveSyncTimer->isActive() && !syncing &&
        !engine->companion()->busy())
        liveSyncTimer->start(0);
}

void FabricTestDialog::syncSavedPack() {
    if (!liveSyncPending || !canLiveSync())
        return cancelLiveSync();
    if (syncing || engine->companion()->busy())
        return; // the in-flight operation resumes us after the debounce
    liveSyncPending = false;
    syncPack(true);
}

bool FabricTestDialog::syncPack(bool live) {
    if (syncing) {
        emit reloadReported("The test pack is already syncing");
        return false;
    }
    const auto syncFailed = [this, live](const QString &title, const QString &message) {
        if (live)
            emit reloadReported(title + " · " + message);
        else
            inform(title, message, true);
        return false;
    };
    const auto root = context.projectRoot ? context.projectRoot() : QString();
    if (root.isEmpty())
        return syncFailed("Open a resource pack", "Open a resource pack to sync it to Minecraft.");
    if (const auto unsaved = context.unsavedDocuments ? context.unsavedDocuments() : QStringList{};
        !unsaved.isEmpty()) {
        if (live)
            return syncFailed("Live sync paused", "Save or update " + unsaved.first() +
                                  (unsaved.size() > 1
                                       ? QString(" and %1 more").arg(unsaved.size() - 1)
                                       : QString()));
        return syncFailed("Save changes first",
                          "Minecraft can only load what is saved in the pack. Save or update these "
                          "documents, then sync again:\n" + unsaved.mid(0, 8).join('\n'));
    }
    const auto game = engine->layout().game;
    if (!QDir().mkpath(game))
        return syncFailed("Sync failed", "Nova could not create its Minecraft test folder.");
    syncing = true;
    refresh();
    const auto name = context.packName();
    const auto staging = engine->layout().base + "/staging";
    QFutureWatcher<SyncOutcome> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(
        [root, name, game, staging] { return syncTestPack(root, name, game, staging); }));
    if (!watcher.isFinished())
        loop.exec(); // the UI keeps painting while the export runs in the pool
    const auto outcome = watcher.result();
    syncing = false;
    const auto resume = qScopeGuard([this] { resumeLiveSync(); });
    refresh();
    if (!outcome.ok) {
        return syncFailed(live ? "Live sync failed" : "Sync failed", outcome.error);
    }
    lastSync = outcome.syncedAt;
    engine->recordMetadata("lastSync", lastSync.toString(Qt::ISODate));
    if (context.activity)
        context.activity(
            QString("Synced “%1” to the Fabric test environment").arg(outcome.folderName));
    refresh();
    if (root != context.projectRoot())
        return false;
    if (live && !canLiveSync())
        return true;
    if (engine->running()) {
        QString error;
        if ((live || autoReload->isChecked()) && engine->companion()->available() &&
            reloadInGame(&error)) {
            guidance->setText("Synced. Reloading resources in Minecraft…");
            if (live)
                emit reloadReported("Test pack synced · Reloading in Minecraft…");
        } else {
            guidance->setText("Synced. Press F3+T in Minecraft to reload resources.");
            if (live && !error.isEmpty())
                emit reloadReported("Test pack synced · " + error);
        }
    }
    return true;
}

bool FabricTestDialog::reloadInGame(QString *error) {
    if (syncing) {
        if (error)
            *error = "The test pack is still syncing.";
        return false;
    }
    if (!engine->running()) {
        if (error)
            *error = "Minecraft is not running from Nova's test environment.";
        return false;
    }
    if (!engine->companion()->requestReload(error))
        return false;
    reloadNote = "Reload requested by Nova…";
    refresh();
    return true;
}

void FabricTestDialog::companionReloadFinished(bool ok, qint64 durationMs, const QString &error) {
    log->poll(); // the reload's warnings are in the log by now
    const auto warningCount = log->state().warnings.size();
    QString message;
    if (ok) {
        const auto seconds = QString::number(double(durationMs) / 1000.0, 'f', 1);
        reloadNote = "Reloaded by Nova in " + seconds + " s";
        message = "Reloaded in Minecraft in " + seconds + " s · " +
                  (warningCount
                       ? QString("%1 resource warning%2").arg(warningCount).arg(warningCount == 1 ? "" : "s")
                       : QString("no resource warnings"));
    } else {
        reloadNote = "Reload failed: " + error;
        message = "Minecraft did not reload: " + error;
    }
    if (context.activity)
        context.activity(message);
    refresh();
    emit reloadReported(message);
    resumeLiveSync();
}

bool FabricTestDialog::confirmShutdown() {
    if (syncing) {
        emit reloadReported("Wait for the test pack to finish syncing");
        return false;
    }
    if (engine->busy()) {
        engine->cancel(); // preparation is Nova's own work
        return true;
    }
    if (!engine->running())
        return true;
    if (!context.interactive)
        return true; // leave the game running
    QMessageBox box(
        QMessageBox::Question, "Minecraft is running",
        "The Minecraft test client is still running. Leave it running, or ask it to close?",
        QMessageBox::NoButton, this);
    auto leave = box.addButton("Leave Running", QMessageBox::AcceptRole);
    auto stop = box.addButton("Stop Minecraft", QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(leave);
    box.exec();
    if (box.clickedButton() == leave)
        return true;
    if (box.clickedButton() != stop)
        return false;
    engine->requestStop();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(engine, &FabricRuntime::gameExited, &loop, &QEventLoop::quit);
    timeout.start(20000);
    if (engine->running())
        loop.exec();
    if (engine->running() &&
        QMessageBox::question(this, "Minecraft did not close",
                              "Stop it now? Unsaved progress since the last autosave is lost.") ==
            QMessageBox::Yes)
        engine->forceStop();
    return true;
}

void FabricTestDialog::updateMonitoring() {
    if (isVisible() || engine->running()) {
        const auto entry = "file/" + packFolder();
        if (!log->active() || log->gameDirectory() != engine->layout().game ||
            log->packEntry() != entry) {
            log->watch(engine->layout().game, entry);
            updateWarnings();
        }
    } else
        log->stop(); // no polling while nothing can change
}

void FabricTestDialog::projectChanged() {
    cancelLiveSync();
    updateMonitoring();
    updateWarnings();
    refresh();
}

void FabricTestDialog::updateWarnings() {
    const auto root = context.projectRoot ? context.projectRoot() : QString();
    const auto entry = "file/" + packFolder();
    QList<ResourceWarning> next;
    if (!root.isEmpty() && log->gameDirectory() == engine->layout().game && log->packEntry() == entry)
        for (const auto &warning : log->state().warnings)
            next.append(resolveResourceWarning(warning, root, entry));
    if (next == warnings)
        return;
    warnings = std::move(next);
    renderedReload = -1;
    emit resourceWarningsChanged();
}

void FabricTestDialog::refreshFindings() {
    const auto &state = log->state();
    const bool noWarnings = engine->running() && state.resourcesLoaded && warnings.isEmpty();
    if (renderedReload == state.reloads && renderedNoWarnings == noWarnings)
        return;
    renderedReload = state.reloads;
    renderedNoWarnings = noWarnings;
    findings->clear();
    for (const auto &warning : warnings) {
        auto text = warning.text;
        for (const auto prefix : {"WARN: ", "ERROR: ", "FATAL: "})
            if (text.startsWith(prefix)) {
                text.remove(0, qstrlen(prefix));
                break;
            }
        const auto source = warning.assets.isEmpty() ? QString("Not from this pack")
                                                     : warning.assets.first();
        auto item = new QListWidgetItem(
            (warning.isError() ? "Error · " : "Warning · ") + text + " · " + source, findings);
        item->setForeground(warning.isError() ? ui::current().danger : ui::current().warning);
        item->setData(Qt::UserRole, warning.assets);
        item->setToolTip(warning.assets.isEmpty() ? text : text + "\n" + warning.assets.join("\n"));
    }
    if (noWarnings)
        findings->addItem("No resource warnings in Minecraft's log for the latest reload.");
    findings->setVisible(findings->count() > 0);
}

void FabricTestDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    engine->attachExisting();
    updateMonitoring();
    refresh();
    fitToScreen();
}

void FabricTestDialog::fitToScreen() {
    // Grow to the content, never past the screen (Advanced at 200 % scaling).
    const auto available =
        screen() ? screen()->availableGeometry().height() - 48 : sizeHint().height();
    resize(width(), qMin(sizeHint().height(), qMax(minimumSizeHint().height(), available)));
}

void FabricTestDialog::hideEvent(QHideEvent *event) {
    QDialog::hideEvent(event);
    updateMonitoring();
}

void FabricTestDialog::refresh() {
    using S = FabricRuntime::State;
    const auto &p = engine->profile();
    const auto state = engine->state();
    const bool busy = engine->busy();
    const bool running = engine->running();
    versionValue->setText(engine->supported()
                              ? QString("%1 · Fabric Loader %2 · Loom %3")
                                    .arg(p.minecraftVersion, p.loaderVersion, p.loomVersion)
                              : engine->error());
    QString runtimeText = stateText(state);
    if (state == S::Failed)
        runtimeText += " — " + engine->error();
    else if (busy || state == S::Launching || state == S::Stopping)
        runtimeText += " — " + engine->status();
    else if (state == S::Ready && engine->lastPrepareMs() >= 0)
        runtimeText +=
            QString(" (checked in %1 s)").arg(engine->lastPrepareMs() / 1000.0, 0, 'f', 1);
    runtimeValue->setText(runtimeText);
    const auto mods = engine->components().loadedMods;
    if (mods.contains("rrls"))
        rrlsValue->setText("Remove Reloading Screen " + mods.value("rrls") +
                           " — loaded (confirmed by Fabric Loader)");
    else if (engine->prepared()) {
        QString pinned;
        for (const auto &mod : p.mods)
            if (mod.id == "rrls")
                pinned = mod.version.section('+', 0, 0);
        rrlsValue->setText(
            QString("Remove Reloading Screen%1 installed — confirmed when Minecraft starts")
                .arg(pinned.isEmpty() ? QString() : " " + pinned));
    } else
        rrlsValue->setText("Not installed yet");
    if (running)
        gameValue->setText(gameText(engine->game()) +
                           (engine->readyForInput() ? " · main menu reached" : QString()) +
                           (engine->game() == FabricRuntime::Game::LaunchFailed
                                ? " — " + engine->error()
                                : QString()));
    else if (engine->game() == FabricRuntime::Game::LaunchFailed)
        gameValue->setText("Launch failed — " + engine->error());
    else
        gameValue->setText(engine->game() == FabricRuntime::Game::Exited ? "Minecraft exited"
                                                                         : "Not running");
    auto synced = lastSync;
    if (!synced.isValid())
        synced =
            QDateTime::fromString(engine->metadata().value("lastSync").toString(), Qt::ISODate);
    const auto pack = inspectTestPack(engine->layout().game, packFolder());
    syncValue->setText(pack.novaOwned && pack.syncedAt.isValid()
                           ? QLocale().toString(pack.syncedAt.toLocalTime(), QLocale::ShortFormat) +
                                 " — " + packFolder()
                           : QString("Not synced yet"));
    const auto &s = log->state();
    QString reload = "No reload seen yet";
    if (s.reloads)
        reload = (s.packActive ? QString("Nova pack active") : QString("Nova pack not enabled")) +
                 (s.reloadFinished    ? " · reload finished (reported by Remove Reloading Screen)"
                  : s.resourcesLoaded ? " · block atlas rebuilt (from Minecraft's log)"
                                      : " · reloading…");
    if (!reloadNote.isEmpty() && running)
        reload = reloadNote + (s.reloads ? " · " + reload : QString());
    reloadValue->setText(reload);
    const bool showProgress = busy || state == S::Launching || state == S::Stopping;
    progress->setVisible(showProgress);
    if (engine->progressTotal() > 0 && busy) {
        progress->setRange(0, 1000);
        progress->setValue(int(engine->progressDone() * 1000 / engine->progressTotal()));
    } else
        progress->setRange(0, 0); // amount of work unknown: no invented percentage
    const bool project = context.projectRoot && !context.projectRoot().isEmpty();
    testButton->setEnabled(project && engine->supported() && !busy && !syncing && state != S::Launching &&
                           state != S::Stopping);
    testButton->setText(running              ? "Sync to Running Game"
                        : engine->prepared() ? "Test in Minecraft"
                                             : "Prepare && Test");
    syncButton->setEnabled(project && !busy && !syncing);
    const bool companion = running && engine->companion()->available();
    reloadButton->setVisible(running);
    compareButton->setVisible(running);
    compareButton->setEnabled(companion && !syncing && !engine->companion()->busy());
    reloadButton->setEnabled(companion && !syncing && !engine->companion()->busy());
    reloadButton->setToolTip(companion ? "Reload resources in the running game, like F3+T (Ctrl+Alt+R)"
                                       : "This game was started without Nova's companion. Press F3+T in "
                                         "Minecraft, or restart it from Nova.");
    if (busy || (running && !engine->readyForInput() &&
                 engine->game() != FabricRuntime::Game::LaunchFailed)) {
        stopButton->setText(busy ? "Cancel" : "Cancel Launch");
        stopButton->setEnabled(true);
    } else {
        stopButton->setText(engine->stopTimedOut() ? "Stop Now" : "Stop Minecraft");
        stopButton->setEnabled(running);
    }
    repairButton->setEnabled(!busy && !running && !syncing && engine->supported());
    versionBox->setEnabled(!busy && !running && !syncing);
    javaValue->setText(engine->components().gameJava.isEmpty()
                           ? (context.preferences && !context.preferences->fabricGameJava.isEmpty()
                                  ? QDir::toNativeSeparators(context.preferences->fabricGameJava)
                                  : QString("Automatic (Java %1 JDK)").arg(p.gameJavaMajor))
                           : QDir::toNativeSeparators(engine->components().gameJava));
    locationValue->setText(QDir::toNativeSeparators(engine->layout().base));
    QString tip;
    if (state == S::Failed)
        tip = "Use Repair runtime under Advanced if this keeps happening.";
    else if (!engine->prepared() && !busy)
        tip = "First use prepares the test environment (about 1 GB, a few minutes). Later launches "
              "reuse it.";
    else if (running && s.reloads && !s.packActive)
        tip = "In Minecraft open Options › Resource Packs and move “" + packFolder() +
              "” to Selected.";
    else if (companion)
        tip = autoReload->isChecked()
                  ? "Nova reloads Minecraft's resources after each sync and reports the result from the "
                    "game's log."
                  : "After a sync choose Reload Resources (Ctrl+Alt+R); Nova reports the result from the "
                    "game's log.";
    else if (running)
        tip = "After each sync press F3+T in Minecraft. Remove Reloading Screen hides the loading "
              "overlay; "
              "the reload itself still runs, and Nova reports it from the game's log.";
    else if (engine->prepared() && !testWorldExists())
        tip = "In Minecraft create a world named “" + testWorldName() +
              "” once; Nova opens it directly afterwards.";
    guidance->setText(tip);
    guidance->setVisible(!tip.isEmpty());
    refreshFindings();
}

} // namespace nova
