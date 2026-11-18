// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>

namespace nova::testing {
namespace {
constexpr qint64 fileLimit = 4096;

QJsonObject readSmall(const QString &path) {
    QFile file(path);
    if (QFileInfo(path).size() > fileLimit || !file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.read(fileLimit)).object();
}
} // namespace

QString newLaunchId() {
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QString id;
    for (const auto word : words)
        id += QString("%1").arg(word, 8, 16, QChar('0'));
    return id;
}

CompanionLink::CompanionLink(QObject *parent) : QObject(parent), timer(new QTimer(this)) {
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, &CompanionLink::poll);
}

void CompanionLink::attach(const QString &gameDirectory, const QString &launchId) {
    if (!pending.isEmpty())
        finish(false, 0, "Minecraft is no longer running from Nova's test environment");
    game = gameDirectory;
    launch = launchId;
}

void CompanionLink::reset(const QString &gameDirectory) {
    const QDir folder(gameDirectory + '/' + companionFolder);
    for (const auto &name : folder.entryList({"companion.json*", "request-*", "response-*"}, QDir::Files))
        QFile::remove(folder.filePath(name));
}

QString CompanionLink::path(const QString &name) const {
    return game + '/' + companionFolder + '/' + name;
}

bool CompanionLink::available() const {
    if (launch.isEmpty() || game.isEmpty())
        return false;
    const auto hello = readSmall(path("companion.json"));
    return hello.value("protocol").toInt() == companionProtocol && hello.value("launch").toString() == launch;
}

bool CompanionLink::requestReload(QString *error) {
    auto refuse = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!pending.isEmpty())
        return refuse("Minecraft is already reloading");
    if (!available())
        return refuse("Nova's companion is not active in the running game");
    const auto id = newLaunchId().left(16);
    const QJsonObject request{{"protocol", companionProtocol}, {"launch", launch}, {"id", id}, {"type", "reload"}};
    // Written under another name, then renamed: the mod only lists request-*.json.
    const auto partial = path("request-" + id + ".partial");
    QFile file(partial);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(QJsonDocument(request).toJson(QJsonDocument::Compact)) < 0)
        return refuse("Nova could not write the reload request");
    file.close();
    if (!QFile::rename(partial, path("request-" + id + ".json"))) {
        QFile::remove(partial);
        return refuse("Nova could not write the reload request");
    }
    pending = id;
    taken = false;
    sentMs = QDateTime::currentMSecsSinceEpoch();
    timer->start();
    return true;
}

void CompanionLink::poll() {
    if (pending.isEmpty())
        return;
    if (!taken && !QFileInfo::exists(path("request-" + pending + ".json"))) {
        taken = true;
        emit reloadStarted();
    }
    const auto responsePath = path("response-" + pending + ".json");
    const auto response = readSmall(responsePath);
    if (response.value("launch").toString() == launch && response.value("id").toString() == pending) {
        QFile::remove(responsePath);
        const auto status = response.value("status").toString();
        return finish(status == "finished", qint64(response.value("durationMs").toDouble()),
                      status == "finished" ? QString()
                      : response.value("error").toString().isEmpty() ? QString("Minecraft could not reload resources")
                                                                      : response.value("error").toString().left(300));
    }
    if (QDateTime::currentMSecsSinceEpoch() - sentMs > timeoutMs) {
        QFile::remove(path("request-" + pending + ".json"));
        finish(false, 0, taken ? "Minecraft did not finish reloading in time"
                               : "Minecraft did not take the reload request; it may be paused or busy");
    }
}

void CompanionLink::finish(bool ok, qint64 durationMs, const QString &error) {
    timer->stop();
    pending.clear();
    taken = false;
    emit reloadFinished(ok, durationMs, error);
}

} // namespace nova::testing
