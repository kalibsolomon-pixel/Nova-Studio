// SPDX-License-Identifier: GPL-3.0-or-later
#include "minecraft_testing.h"
#include "installer.h"
#include "packsync.h"
#include "project.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace nova {
using namespace testing;

namespace {
QLabel *wrapped(const QString &text = {}) {
    auto label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
QString officialImage(const TestTarget &t) {
    return t.kind == LauncherKind::Aurora ? QFileInfo(t.executable).fileName()
                                          : "MinecraftLauncher.exe";
}
} // namespace

MinecraftTestDialog::MinecraftTestDialog(Context ctx, QWidget *parent)
    : QDialog(parent), context(std::move(ctx)), log(new GameLogMonitor(this)) {
    setObjectName("minecraftTestDialog");
    setWindowTitle("Test in Minecraft");
    setMinimumWidth(560);
    auto layout = new QVBoxLayout(this);
    auto intro = wrapped(
        "Sign-in, the Minecraft: Java Edition ownership check and the game launch "
        "are handled by the launcher you choose. Nova never sees your Microsoft or "
        "Minecraft credentials; it only places your saved pack where the game can load it.");
    intro->setObjectName("minecraftTestIntro");
    intro->setProperty("novaRole", "secondary");
    layout->addWidget(intro);

    auto launcherBox = new QGroupBox("Launcher");
    auto launcherLayout = new QVBoxLayout(launcherBox);
    targetBox = new QComboBox;
    targetBox->setObjectName("minecraftTestTarget");
    targetBox->setAccessibleName("Minecraft test target");
    targetBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto refresh = new QPushButton("Refresh");
    refresh->setObjectName("refreshLaunchers");
    refresh->setToolTip("Look for launchers and instances again");
    auto targetRow = new QHBoxLayout;
    targetRow->addWidget(targetBox, 1);
    targetRow->addWidget(refresh);
    launcherLayout->addLayout(targetRow);
    targetDetail = wrapped();
    targetDetail->setObjectName("minecraftTestTargetDetail");
    launcherLayout->addWidget(targetDetail);
    auto getRow = new QHBoxLayout;
    getOfficial = new QPushButton("Get Minecraft Launcher…");
    getOfficial->setObjectName("getOfficialLauncher");
    getAurora = new QPushButton("Get Aurora Launcher…");
    getAurora->setObjectName("getAuroraLauncher");
    getRow->addWidget(getOfficial);
    getRow->addWidget(getAurora);
    getRow->addStretch();
    launcherLayout->addLayout(getRow);
    layout->addWidget(launcherBox);

    auto packBox = new QGroupBox("Pack");
    auto packLayout = new QVBoxLayout(packBox);
    packStatus = wrapped();
    packStatus->setObjectName("minecraftTestPackStatus");
    packLayout->addWidget(packStatus);
    layout->addWidget(packBox);

    auto gameBox = new QGroupBox("Game");
    auto gameLayout = new QVBoxLayout(gameBox);
    gameStatus = wrapped();
    gameStatus->setObjectName("minecraftTestGameStatus");
    gameLayout->addWidget(gameStatus);
    guidance = wrapped();
    guidance->setObjectName("minecraftTestGuidance");
    guidance->setProperty("novaRole", "secondary");
    gameLayout->addWidget(guidance);
    findings = new QListWidget;
    findings->setObjectName("minecraftTestFindings");
    findings->setAccessibleName("Minecraft resource warnings");
    findings->setMinimumHeight(72);
    findings->setUniformItemSizes(true);
    findings->setWordWrap(true);
    findings->hide(); // shown only when Minecraft reported something
    gameLayout->addWidget(findings);
    layout->addWidget(gameBox, 1);

    auto buttons = new QHBoxLayout;
    syncButton = new QPushButton("Sync Pack");
    syncButton->setObjectName("minecraftSyncPack");
    syncButton->setToolTip("Copy the saved pack into the test game folder (Ctrl+Alt+M)");
    launchButton = new QPushButton("Sync && Open Launcher");
    launchButton->setObjectName("minecraftOpenLauncher");
    launchButton->setDefault(true);
    folderButton = new QPushButton("Open Game Folder");
    folderButton->setObjectName("minecraftOpenFolder");
    auto close = new QPushButton("Close");
    buttons->addWidget(syncButton);
    buttons->addWidget(launchButton);
    buttons->addWidget(folderButton);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(targetBox, &QComboBox::currentIndexChanged, this,
            &MinecraftTestDialog::selectionChanged);
    connect(refresh, &QPushButton::clicked, this, &MinecraftTestDialog::rescan);
    connect(getOfficial, &QPushButton::clicked, this, [this] { getLauncher(false); });
    connect(getAurora, &QPushButton::clicked, this, [this] { getLauncher(true); });
    connect(syncButton, &QPushButton::clicked, this, [this] { syncPack(); });
    connect(launchButton, &QPushButton::clicked, this, &MinecraftTestDialog::openLauncher);
    connect(folderButton, &QPushButton::clicked, this, [this] {
        if (auto t = selected(); t && QDir().mkpath(t->gameDirectory))
            QDesktopServices::openUrl(QUrl::fromLocalFile(t->gameDirectory));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(log, &GameLogMonitor::changed, this, [this] {
        refreshGameStatus();
        if (!isVisible() && !log->state().running)
            log->stop(); // nothing left to observe while hidden
    });
    rescan();
}

QString MinecraftTestDialog::packEntry() const {
    return "file/" + testPackFolderName(context.packName ? context.packName() : QString());
}

QString MinecraftTestDialog::versionForPack() const {
    const auto root = context.projectRoot ? context.projectRoot() : QString();
    if (!root.isEmpty()) {
        const auto pack = Project::inspect(root).json.value("pack").toObject();
        const auto format = PackFormat::parse(
            pack.contains("min_format") ? pack.value("min_format") : pack.value("pack_format"));
        if (format)
            for (const auto &version : gameVersions())
                if (version.format.major == format->major)
                    return version.id;
    }
    return "1.21.11"; // Nova's primary verified development target
}

void MinecraftTestDialog::rescan() {
    const auto previous = selected()
                              ? selected()->key
                              : (context.rememberedTarget ? *context.rememberedTarget : QString());
    launchers = discoverLaunchers(context.environment);
    available.clear();
    bool official = false, aurora = false;
    const auto version = versionForPack();
    for (const auto &launcher : launchers) {
        if (launcher.kind != LauncherKind::Aurora) {
            if (official)
                continue;
            official = true;
            TestTarget t;
            t.kind = launcher.kind;
            t.key = "official";
            t.label = launcher.label + " — Nova test installation (" + version + ")";
            t.executable = launcher.executable;
            t.gameDirectory = novaTestGameDirectory(context.environment.localAppData);
            t.versionId = version;
            available.append(t);
            continue;
        }
        aurora = true;
        QString error;
        for (const auto &instance : readAuroraInstances(launcher.dataRoot, &error)) {
            if (instance.state != "ready")
                continue; // never write into an instance Aurora is installing or repairing
            TestTarget t;
            t.kind = LauncherKind::Aurora;
            t.key = "aurora:" + instance.id;
            t.instanceName = instance.displayName;
            t.modded = instance.platform != "vanilla";
            t.modpackManaged = instance.modpackManaged;
            t.label = QString("%1 — %2 (%3%4%5)")
                          .arg(launcher.label, instance.displayName, instance.minecraftVersion,
                               t.modded ? ", " + instance.platform : QString(),
                               t.modpackManaged ? ", modpack" : QString());
            t.executable = launcher.executable;
            t.gameDirectory = instance.gameDirectory;
            t.versionId = instance.minecraftVersion;
            available.append(t);
        }
    }
    QSignalBlocker block(targetBox);
    targetBox->clear();
    for (const auto &t : available)
        targetBox->addItem(t.label, t.key);
    if (available.isEmpty())
        targetBox->addItem("No launcher found — get one below");
    const int index = targetBox->findData(previous);
    targetBox->setCurrentIndex(index >= 0 ? index : 0);
    getOfficial->setVisible(!official);
    getAurora->setVisible(!aurora);
    selectionChanged();
}

const TestTarget *MinecraftTestDialog::selected() const {
    const auto key = targetBox->currentData().toString();
    for (const auto &t : available)
        if (t.key == key)
            return &t;
    return nullptr;
}

void MinecraftTestDialog::selectionChanged() {
    const auto t = selected();
    syncButton->setEnabled(t && !busy);
    launchButton->setEnabled(t && !busy);
    folderButton->setEnabled(t != nullptr);
    if (!t) {
        targetDetail->setText(
            "Install the Minecraft Launcher or Aurora Launcher to test this pack in "
            "the game. Editing never requires a launcher or a Microsoft account.");
        guidance->clear();
        log->stop();
        refreshPackStatus();
        refreshGameStatus();
        return;
    }
    if (context.rememberedTarget)
        *context.rememberedTarget = t->key;
    QStringList detail{"Game folder: " + QDir::toNativeSeparators(t->gameDirectory)};
    if (t->kind == LauncherKind::Aurora) {
        if (t->versionId != versionForPack())
            detail << "This instance runs Minecraft " + t->versionId + "; the pack targets " +
                          versionForPack() + ".";
        if (t->modded)
            detail
                << "Mods (for example renderer optimisations) can change how textures and models "
                   "look; a vanilla instance gives the most faithful result.";
        if (t->modpackManaged)
            detail << "Aurora may list the Nova folder as content added outside the modpack.";
    } else {
        detail
            << "Separate saves and settings from your normal Minecraft; reuses the game files the "
               "launcher already has.";
    }
    targetDetail->setText(detail.join('\n'));
    if (isVisible())
        log->watch(t->gameDirectory, packEntry());
    refreshPackStatus();
    refreshGameStatus();
}

void MinecraftTestDialog::refreshPackStatus() {
    const auto t = selected();
    const auto unsaved = context.unsavedDocuments ? context.unsavedDocuments() : QStringList{};
    QStringList lines;
    if (context.projectRoot && context.projectRoot().isEmpty())
        lines << "Open a resource pack to test it in Minecraft.";
    else if (t) {
        const auto state =
            inspectTestPack(t->gameDirectory, testPackFolderName(context.packName()));
        if (!state.present)
            lines << "Not synced to this game folder yet.";
        else if (!state.novaOwned)
            lines
                << "A folder with the test pack's name exists but was not created by Nova; it will "
                   "not be replaced.";
        else
            lines << "Minecraft has the export from " +
                         QLocale().toString(state.syncedAt.toLocalTime(), QLocale::ShortFormat) +
                         ".";
    }
    if (!unsaved.isEmpty())
        lines << QString("%1 open document(s) have changes that are not saved to the pack yet; "
                         "save them before syncing: %2")
                     .arg(unsaved.size())
                     .arg(unsaved.mid(0, 3).join(", "));
    packStatus->setText(lines.join('\n'));
}

void MinecraftTestDialog::refreshGameStatus() {
    const auto &s = log->state();
    const auto t = selected();
    QString status;
    if (!t)
        status = "No launcher selected.";
    else if (s.crashed && !s.running)
        status = "Minecraft stopped unexpectedly. The crash report is in the game folder.";
    else if (!s.running)
        status = "Minecraft is not running in this game folder.";
    else if (!s.reloads)
        status = "Minecraft is starting" +
                 (s.player.isEmpty() ? QString(".") : " for " + s.player + ".");
    else if (s.packActive)
        status = QString("Running%1 · Nova test pack active%2")
                     .arg(s.player.isEmpty() ? QString() : " as " + s.player,
                          s.resourcesLoaded ? " · resources loaded" : " · loading resources…");
    else
        status = "Running · the Nova test pack is not enabled yet";
    gameStatus->setText(status);
    QString tip;
    if (t && s.running && s.reloads && !s.packActive)
        tip = "In Minecraft open Options › Resource Packs and move “" +
              testPackFolderName(context.packName()) + "” to Selected.";
    else if (t && s.running)
        tip = "After each sync press F3+T in Minecraft to reload resources.";
    else if (t && t->kind == LauncherKind::Aurora)
        tip = "Play “" + t->instanceName +
              "” in Aurora. The first time, enable the Nova pack under "
              "Options › Resource Packs.";
    else if (t)
        tip = "In the launcher choose the “Nova Studio Test” installation and press Play. Create a "
              "new world there for testing.";
    guidance->setText(tip);
    findings->clear();
    for (const auto &warning : s.warnings)
        findings->addItem(warning);
    if (s.running && s.resourcesLoaded && s.warnings.isEmpty())
        findings->addItem("No resource warnings in Minecraft's log for the latest reload.");
    const bool show = findings->count() > 0;
    if (findings->isVisibleTo(this) != show) {
        findings->setVisible(show);
        if (isVisible())
            resize(width(), sizeHint().height());
    }
}

void MinecraftTestDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    if (auto t = selected())
        log->watch(t->gameDirectory, packEntry());
    refreshPackStatus();
    resize(width(), sizeHint().height());
}

void MinecraftTestDialog::hideEvent(QHideEvent *event) {
    QDialog::hideEvent(event);
    if (!log->state().running)
        log->stop(); // keep idle CPU at zero when there is nothing to watch
}

bool MinecraftTestDialog::ask(const QString &title, const QString &text) {
    return !context.interactive ||
           QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::Cancel,
                                 QMessageBox::Yes) == QMessageBox::Yes;
}
void MinecraftTestDialog::inform(const QString &title, const QString &text, bool warning) {
    if (context.activity)
        context.activity(title + ": " + text);
    if (!context.interactive)
        return;
    warning ? QMessageBox::warning(this, title, text) : QMessageBox::information(this, title, text);
}

bool MinecraftTestDialog::syncPack() {
    const auto t = selected();
    const auto root = context.projectRoot ? context.projectRoot() : QString();
    if (!t || root.isEmpty() || busy)
        return false;
    if (const auto unsaved = context.unsavedDocuments(); !unsaved.isEmpty()) {
        inform("Save changes first",
               "Minecraft can only load what is saved in the pack. Save or update these documents, "
               "then sync again:\n" +
                   unsaved.mid(0, 8).join('\n'),
               true);
        refreshPackStatus();
        return false;
    }
    if (t->kind != LauncherKind::Aurora && !QDir().mkpath(t->gameDirectory)) {
        inform("Sync failed", "Nova could not create its Minecraft test folder.", true);
        return false;
    }
    busy = true;
    selectionChanged();
    const auto name = context.packName();
    const auto game = t->gameDirectory;
    const auto staging = novaStagingRoot(context.environment.localAppData);
    QFutureWatcher<SyncOutcome> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(
        [root, name, game, staging] { return syncTestPack(root, name, game, staging); }));
    QProgressDialog progress("Exporting the pack for Minecraft…", QString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    if (!watcher.isFinished())
        loop.exec();
    progress.close();
    const auto outcome = watcher.result();
    busy = false;
    selectionChanged();
    if (!outcome.ok) {
        inform("Sync failed", outcome.error, true);
        return false;
    }
    // Only Nova's own isolated folder has its options enabled automatically;
    // Minecraft rewrites options.txt on exit, so never while it is running.
    if (t->kind != LauncherKind::Aurora && !log->state().running) {
        QString error;
        if (!enableTestPackOption(game, outcome.folderName, &error) && context.activity)
            context.activity("Could not pre-select the test pack: " + error);
    }
    if (context.activity)
        context.activity(QString("Synced “%1” to %2").arg(outcome.folderName, t->label));
    refreshPackStatus();
    refreshGameStatus();
    if (log->state().running)
        guidance->setText("Synced. Press F3+T in Minecraft to reload resources.");
    return true;
}

bool MinecraftTestDialog::prepareOfficialInstallation(const TestTarget &t) {
    const auto minecraft = officialMinecraftDirectory(context.environment);
    QString gameDir, version;
    if (launcherProfilePresent(minecraft, &gameDir, &version) &&
        QDir(gameDir) == QDir(t.gameDirectory) && version == t.versionId)
        return true;
    const auto manual = QString("In the Minecraft Launcher open Installations › New installation, "
                                "choose version %1 and set the game directory to:\n%2")
                            .arg(t.versionId, QDir::toNativeSeparators(t.gameDirectory));
    if (!QFileInfo::exists(minecraft + "/launcher_profiles.json")) {
        inform(
            "Open the launcher once",
            "The Minecraft Launcher has not created its installation list yet. Open it, sign in, "
            "close it, then use Test in Minecraft again.\n\nOr set it up yourself: " +
                manual);
        return false;
    }
    if (processRunning({"MinecraftLauncher.exe"})) {
        inform(
            "Close the Minecraft Launcher",
            "Close the Minecraft Launcher so Nova can add its test installation safely, or set it "
            "up yourself: " +
                manual);
        return false;
    }
    if (!ask("Add test installation",
             "Add a “Nova Studio Test” installation to the Minecraft Launcher?\n\nIt runs "
             "Minecraft " +
                 t.versionId +
                 " with its own game folder, so your normal worlds, settings and packs "
                 "are untouched, and it reuses game files the launcher already has. Nova keeps a "
                 "backup of the launcher's installation list.")) {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(t.gameDirectory));
        inform("Set up manually", manual + "\n\nThe folder path is on the clipboard.");
        return false;
    }
    const auto outcome = installLauncherProfile(minecraft, t.gameDirectory, t.versionId);
    if (!outcome.ok) {
        inform("Could not add the installation", outcome.error + "\n\n" + manual, true);
        return false;
    }
    if (context.activity)
        context.activity("Added the Nova Studio Test installation (backup: " +
                         QFileInfo(outcome.backupPath).fileName() + ")");
    return true;
}

void MinecraftTestDialog::openLauncher() {
    const auto t = selected();
    if (!t || !syncPack())
        return;
    const auto target = *t; // syncPack may rescan
    if (target.kind != LauncherKind::Aurora && !prepareOfficialInstallation(target))
        return;
    bool started = false;
    if (processRunning({officialImage(target)}))
        started = true; // already open: the user switches to it
    else if (target.kind == LauncherKind::OfficialStore)
        started = QProcess::startDetached("explorer.exe",
                                          {QString("shell:AppsFolder\\") + officialStoreAppId});
    else
        started =
            QProcess::startDetached(target.executable, {}, QFileInfo(target.executable).path());
    if (!started) {
        inform("Launcher did not start", "Nova could not start " + target.label + ".", true);
        return;
    }
    log->watch(target.gameDirectory, packEntry());
    refreshGameStatus();
    if (context.activity)
        context.activity("Opened " + target.label + " for testing");
}

void MinecraftTestDialog::getLauncher(bool aurora) {
    if (!network)
        network = new QNetworkAccessManager(this);
    const auto downloads = context.environment.localAppData + "/NovaStudio/downloads";
    InstallerPlan plan;
    QString error;
    auto fetchJson = [&](const QUrl &url, const DownloadPolicy &policy, QByteArray *out) {
        Fetch fetch(network, policy);
        QEventLoop loop;
        QString result;
        connect(&fetch, &Fetch::finished, &loop, [&](const QString &e) {
            result = e;
            loop.quit();
        });
        fetch.start(url);
        loop.exec();
        *out = fetch.body();
        return result;
    };
    if (aurora) {
        QByteArray releases, manifest;
        QString version;
        error = fetchJson(auroraReleasesUrl(), auroraPolicy(), &releases);
        std::optional<QUrl> manifestUrl;
        if (error.isEmpty())
            manifestUrl = pickAuroraRelease(releases, &version, &error);
        if (manifestUrl)
            error = fetchJson(*manifestUrl, auroraPolicy(), &manifest);
        if (manifestUrl && error.isEmpty())
            if (auto parsed = parseAuroraAssetManifest(manifest, *manifestUrl, &error))
                plan = *parsed;
        if (plan.url.isEmpty()) {
            inform("Aurora Launcher unavailable",
                   (error.isEmpty() ? QString("No installer was found") : error) +
                       ". You can download it yourself from github.com/" + auroraRepository +
                       "/releases.",
                   true);
            return;
        }
    } else
        plan = officialLauncherPlan();
    const auto size = plan.expectedSize > 0 ? QLocale().formattedDataSize(plan.expectedSize)
                                            : QString("about 2 MB");
    QString question =
        QString("Download %1 %2?\n\nFile: %3\nSource: %4\nSize: %5\n")
            .arg(plan.product, plan.version, plan.fileName, plan.url.toString(), size);
    question += plan.sha256.isEmpty() ? "Nova will check that the installer is digitally signed by "
                                        "Microsoft before offering to run it."
                                      : "Nova will check the published SHA-256 " +
                                            QString::fromLatin1(plan.sha256.left(16)) +
                                            "… before offering to run it.";
    if (!ask("Download launcher", question))
        return;
    const auto destination = downloads + '/' + plan.fileName;
    Fetch fetch(network, aurora ? auroraPolicy() : officialLauncherPolicy());
    QProgressDialog progress("Downloading " + plan.fileName + "…", "Cancel", 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    QEventLoop loop;
    connect(&fetch, &Fetch::progress, &progress, [&](qint64 got, qint64 total) {
        if (total > 0)
            progress.setValue(int(got * 100 / total));
    });
    connect(&progress, &QProgressDialog::canceled, &fetch, &Fetch::cancel);
    connect(&fetch, &Fetch::finished, &loop, [&](const QString &e) {
        error = e;
        loop.quit();
    });
    fetch.start(plan.url, destination);
    loop.exec();
    progress.close();
    if (!error.isEmpty()) {
        inform("Download failed", error, true);
        return;
    }
    SignatureCheck signature;
    error = verifyInstaller(destination, plan, &signature);
    if (!error.isEmpty()) {
        QFile::remove(destination);
        inform("Installer rejected", error + ". The download was deleted.", true);
        return;
    }
    const auto signerText =
        signature.status == SignatureCheck::Valid
            ? "Digitally signed by " + signature.signer + "."
            : QString("Not code-signed; its SHA-256 matches the published release.");
    if (!ask("Run installer", "The installer was verified. " + signerText +
                                  "\n\nRun it now? The installer opens its own window."))
        return;
    if (!runInstaller(destination, plan, &error))
        inform("Installer did not start", error, true);
    else if (context.activity)
        context.activity("Started the " + plan.product +
                         " installer; press Refresh when it finishes");
}

} // namespace nova
