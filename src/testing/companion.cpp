// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion.h"
#include <QDateTime>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>
#include <utility>

namespace nova::testing {
namespace {
constexpr qint64 fileLimit = 16384;

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
    for (const auto &name : folder.entryList({"icons-*"}, QDir::Dirs | QDir::NoDotAndDotDot))
        QDir(folder.filePath(name)).removeRecursively();
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

bool CompanionLink::requestReload(QString *error) { return send("reload", {}, timeoutMs, error); }

bool CompanionLink::requestIcons(const QStringList &items, QString *error, int guiScale, bool glint) {
    if (items.isEmpty() || items.size() > 64) {
        if (error)
            *error = "Choose 1 to 64 items";
        return false;
    }
    pendingItems = int(items.size());
    QJsonObject request{{"items", QJsonArray::fromStringList(items)}};
    if (guiScale > 0)
        request.insert("guiScale", qMin(guiScale, 8));
    if (!glint)
        request.insert("glint", false);
    return send("icons", request, qMin(timeoutMs, 60000), error);
}

bool CompanionLink::send(const QString &type, QJsonObject request, int timeout, QString *error) {
    auto refuse = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!pending.isEmpty())
        return refuse(pendingType == "reload" ? "Minecraft is already reloading" : "Minecraft is busy with Nova's last request");
    if (!available())
        return refuse("Nova's companion is not active in the running game");
    const auto id = newLaunchId().left(16);
    request.insert("protocol", companionProtocol);
    request.insert("launch", launch);
    request.insert("id", id);
    request.insert("type", type);
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
    pendingType = type;
    pendingTimeout = timeout;
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
        if (pendingType == "reload")
            emit reloadStarted();
    }
    const auto responsePath = path("response-" + pending + ".json");
    const auto response = readSmall(responsePath);
    if (response.value("launch").toString() == launch && response.value("id").toString() == pending) {
        QFile::remove(responsePath);
        const auto status = response.value("status").toString();
        return finish(status == "finished", qint64(response.value("durationMs").toDouble()),
                      status == "finished" ? QString()
                      : response.value("error").toString().isEmpty()
                          ? QString(pendingType == "reload" ? "Minecraft could not reload resources"
                                                            : "Minecraft could not draw the icons")
                          : response.value("error").toString().left(300),
                      response);
    }
    if (QDateTime::currentMSecsSinceEpoch() - sentMs > pendingTimeout) {
        QFile::remove(path("request-" + pending + ".json"));
        finish(false, 0, !taken ? QString("Minecraft did not take the request; it may be paused or busy")
                         : pendingType == "reload" ? QString("Minecraft did not finish reloading in time")
                                                   : QString("Minecraft did not finish drawing the icons in time"));
    }
}

void CompanionLink::finish(bool ok, qint64 durationMs, const QString &error, QJsonObject result) {
    timer->stop();
    const auto type = std::exchange(pendingType, QString());
    pending.clear();
    taken = false;
    if (type != "icons")
        return emit reloadFinished(ok, durationMs, error);
    // Only PNGs inside this request's own folder are reported.
    const auto name = result.value("folder").toString();
    const QDir folder(game + '/' + companionFolder + '/' + name);
    QJsonArray files;
    for (int i = 0; i < pendingItems; ++i) {
        const auto file = folder.filePath(QString::number(i) + ".png");
        files.append(ok && name.startsWith("icons-") && !name.contains('/') && !name.contains('\\') &&
                             QFileInfo(file).isFile()
                         ? file
                         : QString());
    }
    result.insert("files", files);
    result.insert("durationMs", durationMs);
    emit iconsFinished(ok, result, error);
}

} // namespace nova::testing
