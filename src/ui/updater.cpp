// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"
#include "build_identity.h"
#include "installer.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>

namespace nova {
namespace {
testing::DownloadPolicy releasePolicy() {
    return {{"api.github.com", "github.com", "objects.githubusercontent.com",
             "release-assets.githubusercontent.com"},
            false,
            256LL * 1024 * 1024,
            QByteArray("NovaStudio-Updater/") + AppVersion};
}
// Setup writes its marker and uninstaller next to nova-studio.exe; the portable ZIP and builds have neither.
bool installedBySetup() {
    const auto dir = QCoreApplication::applicationDirPath();
    return QFileInfo(dir + "/nova-studio-install.txt").isFile() && QFileInfo(dir + "/Uninstall Nova Studio.exe").isFile();
}
// A running program cannot be opened for writing, as Setup checks it.
bool inUse(const QString &path) {
    QFile file(path);
    if (!file.exists())
        return false;
    if (!file.open(QIODevice::Append))
        return true;
    file.close();
    return false;
}
} // namespace

Updater::Updater(QMainWindow *window, QString settingsFile)
    : QObject(window), window(window), settingsFile(std::move(settingsFile)) {}

QString Updater::folder() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/updates";
}

void Updater::tell(bool manual, const QString &message, bool problem) {
    if (!manual) {
        qInfo().noquote() << "Update check:" << message;
        return;
    }
    if (problem)
        QMessageBox::warning(window, "Check for Updates", message);
    else
        QMessageBox::information(window, "Check for Updates", message);
}

void Updater::startup(bool checkEnabled) {
    finishPending();
    if (checkEnabled)
        check(false);
}

// The previous run started Setup for an update: say whether it worked.
void Updater::finishPending() {
    QSettings settings(settingsFile, QSettings::IniFormat);
    const auto pending = settings.value("updates/pending").toString();
    const auto installer = settings.value("updates/installer").toString();
    if (pending.isEmpty())
        return;
    settings.remove("updates/pending");
    settings.remove("updates/installer");
    settings.sync();
    if (!updates::newer(pending, AppVersion)) {
        QFile::remove(installer);
        window->statusBar()->showMessage(QString("Nova Studio was updated to %1").arg(AppVersion), 8000);
        return;
    }
    auto box = new QMessageBox(QMessageBox::Warning, "Update not installed",
                               QString("The update to Nova Studio %1 did not finish. This is still Nova Studio %2.")
                                   .arg(pending, AppVersion),
                               QMessageBox::Close, window);
    box->setObjectName("updateFailedDialog");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText("Close any AI agent that uses Nova Studio, then run the installer again. "
                            "It shows what stopped the installation.");
    if (QFileInfo(installer).isFile()) {
        auto run = box->addButton("Run Installer", QMessageBox::AcceptRole);
        connect(run, &QPushButton::clicked, this, [this, pending, installer] {
            updates::Release release;
            release.version = pending;
            install(release, installer, false);
        });
    }
    box->open();
}

void Updater::check(bool manual) {
    if (busy) {
        if (manual)
            window->statusBar()->showMessage("Already checking for updates…", 4000);
        return;
    }
    busy = true;
    if (!network)
        network = new QNetworkAccessManager(this);
    if (manual)
        window->statusBar()->showMessage("Checking for updates…");
    auto fetch = new testing::Fetch(network, releasePolicy(), this);
    connect(fetch, &testing::Fetch::finished, this, [this, fetch, manual](const QString &error) {
        fetch->deleteLater();
        if (manual)
            window->statusBar()->clearMessage();
        QString problem = error;
        const auto release = error.isEmpty() ? updates::parseLatestRelease(fetch->body(), &problem) : std::nullopt;
        if (!release) {
            busy = false;
            return tell(manual, "Nova Studio could not check for updates.\n" + problem, true);
        }
        if (!updates::newer(release->version, AppVersion)) {
            busy = false;
            return tell(manual, QString("Nova Studio %1 is the latest version.").arg(AppVersion));
        }
        if (!manual && QSettings(settingsFile, QSettings::IniFormat).value("updates/skip").toString() == release->version) {
            busy = false;
            return tell(false, "skipping " + release->version + " as asked");
        }
        downloaded(*release, manual);
    });
    fetch->start(updates::latestReleaseUrl());
}

// Fetches the installer (unless it is already here) and checks it, then offers it.
void Updater::downloaded(const updates::Release &release, bool manual) {
    if (!installedBySetup() || release.installer.isEmpty() || release.checksums.isEmpty()) {
        busy = false;
        return offer(release, {});
    }
    if (manual)
        window->statusBar()->showMessage(QString("Downloading Nova Studio %1…").arg(release.version));
    auto sums = new testing::Fetch(network, releasePolicy(), this);
    connect(sums, &testing::Fetch::finished, this, [this, sums, release, manual](const QString &error) {
        sums->deleteLater();
        const auto expected = error.isEmpty() ? updates::checksumFor(sums->body(), release.installerName) : QByteArray{};
        const auto fail = [this, manual, release](const QString &why) {
            busy = false;
            if (manual)
                window->statusBar()->clearMessage();
            tell(manual, QString("Nova Studio %1 could not be downloaded.\n%2").arg(release.version, why), true);
        };
        if (!error.isEmpty())
            return fail(error);
        if (expected.isEmpty())
            return fail("The release does not list a checksum for its installer.");
        QDir().mkpath(folder());
        const auto target = folder() + '/' + release.installerName;
        // Only installers this updater downloaded live here; an older one is no longer needed.
        for (const auto &old : QDir(folder()).entryInfoList({"nova-studio-*-windows-x64-mingw.exe"}, QDir::Files))
            if (old.fileName() != release.installerName)
                QFile::remove(old.absoluteFilePath());
        const auto verified = [this, release, target, expected, fail](bool hashed) {
            if (!hashed && testing::sha256Hex(target) != expected) {
                QFile::remove(target);
                return fail("The downloaded installer does not match the release's SHA256SUMS.txt.");
            }
            if (testing::checkAuthenticode(target).status == testing::SignatureCheck::Invalid) {
                QFile::remove(target);
                return fail("The downloaded installer has an invalid digital signature.");
            }
            busy = false;
            window->statusBar()->clearMessage();
            offer(release, target);
        };
        if (QFileInfo(target).isFile() && testing::sha256Hex(target) == expected)
            return verified(true);
        auto download = new testing::Fetch(network, releasePolicy(), this);
        connect(download, &testing::Fetch::finished, this, [download, verified, fail](const QString &error) {
            download->deleteLater();
            if (!error.isEmpty())
                return fail(error);
            verified(false);
        });
        download->start(release.installer, target);
    });
    sums->start(release.checksums);
}

// Asks before installing; without an installer, points to the release page.
void Updater::offer(const updates::Release &release, const QString &installer) {
    if (auto open = window->findChild<QMessageBox *>("updateDialog"))
        open->close();
    auto box = new QMessageBox(QMessageBox::Information, "Update available",
                               QString("Nova Studio %1 is available. You have %2.").arg(release.version, AppVersion),
                               QMessageBox::NoButton, window);
    box->setObjectName("updateDialog");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextFormat(Qt::PlainText);
    QPushButton *installButton = nullptr;
    if (!installer.isEmpty()) {
        box->setInformativeText("The installer is downloaded and checked. Nova Studio closes, installs the update "
                                "and starts again. Your resource packs and settings are kept; you are asked to "
                                "save unsaved work first.");
        installButton = box->addButton("Install and Restart", QMessageBox::AcceptRole);
        installButton->setObjectName("updateInstall");
    } else {
        box->setInformativeText("This copy of Nova Studio was not installed by Setup. Download the new version "
                                "from the release page.");
    }
    auto notes = box->addButton(installer.isEmpty() ? "Open Release Page" : "Release Notes", QMessageBox::HelpRole);
    notes->setObjectName("updateReleasePage");
    box->addButton("Later", QMessageBox::RejectRole)->setObjectName("updateLater");
    auto skip = box->addButton("Skip This Version", QMessageBox::DestructiveRole);
    skip->setObjectName("updateSkip");
    box->setDefaultButton(installButton ? installButton : notes);
    connect(box, &QMessageBox::buttonClicked, this,
            [this, release, installer, installButton, notes, skip](QAbstractButton *clicked) {
        if (clicked == notes) {
            QDesktopServices::openUrl(release.page);
            // Any button closes the box; the offer stays open while the notes are read.
            if (!installer.isEmpty())
                QMetaObject::invokeMethod(this, [this, release, installer] { offer(release, installer); },
                                          Qt::QueuedConnection);
        } else if (clicked == skip) {
            QSettings(settingsFile, QSettings::IniFormat).setValue("updates/skip", release.version);
        } else if (clicked == installButton) {
            // Leave the box before the window closes.
            QMetaObject::invokeMethod(this, [this, release, installer] { install(release, installer, true); },
                                      Qt::QueuedConnection);
        }
    });
    box->open();
}

// Closes Nova (which asks about unsaved work), then starts Setup. Setup waits for Nova to exit and,
// when silent, starts the new version.
void Updater::install(const updates::Release &release, const QString &installer, bool silent) {
    const auto dir = QCoreApplication::applicationDirPath();
    if (inUse(dir + "/nova-mcp.exe")) {
        QMessageBox::warning(window, "Update",
                             "An AI agent is using the Nova Studio MCP bridge (nova-mcp.exe). Close the agent or "
                             "stop its Agent Session, then choose Help › Check for Updates again.");
        return;
    }
    {
        QSettings settings(settingsFile, QSettings::IniFormat);
        settings.setValue("updates/pending", release.version);
        settings.setValue("updates/installer", installer);
    }
    if (!window->close()) {
        QSettings settings(settingsFile, QSettings::IniFormat);
        settings.remove("updates/pending");
        settings.remove("updates/installer");
        return;
    }
    QProcess setup;
    setup.setProgram(installer);
    // NSIS reads /D= unquoted, as the last argument.
    setup.setNativeArguments(QString(silent ? "/S /UPDATE /D=%1" : "/UPDATE /D=%1").arg(QDir::toNativeSeparators(dir)));
    if (!setup.startDetached())
        qWarning().noquote() << "The update installer could not be started:" << installer;
}
} // namespace nova
