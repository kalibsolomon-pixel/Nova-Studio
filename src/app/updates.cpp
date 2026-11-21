// SPDX-License-Identifier: GPL-3.0-or-later
#include "updates.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace nova::updates {
namespace {
const QRegularExpression &releaseVersion() {
    static const QRegularExpression rule(R"(^(\d{1,4})\.(\d{1,4})\.(\d{1,4})$)");
    return rule;
}
// Only a download from this release's own folder in the Nova Studio repository.
QUrl releaseAsset(const QJsonObject &asset, const QString &tag) {
    const QUrl url(asset["browser_download_url"].toString());
    const auto expected = QString("/%1/releases/download/%2/%3").arg(repository, tag, asset["name"].toString());
    if (url.scheme() != "https" || url.host() != "github.com" || url.port() != -1 ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || url.path() != expected)
        return {};
    return url;
}
} // namespace

QUrl latestReleaseUrl() {
    return QUrl(QString("https://api.github.com/repos/%1/releases/latest").arg(repository));
}

std::optional<Release> parseLatestRelease(const QByteArray &json, QString *error) {
    const auto fail = [error](const QString &why) -> std::optional<Release> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    const auto release = QJsonDocument::fromJson(json).object();
    if (release.isEmpty())
        return fail("GitHub sent an unreadable release description");
    if (release["draft"].toBool(true) || release["prerelease"].toBool(true))
        return fail("The newest release is not a final release");
    Release result;
    result.tag = release["tag_name"].toString();
    if (!result.tag.startsWith('v') || !releaseVersion().match(result.tag.mid(1)).hasMatch())
        return fail("The newest release has an unexpected tag: " + result.tag.left(64));
    result.version = result.tag.mid(1);
    const QUrl page(release["html_url"].toString());
    if (page.scheme() == "https" && page.host() == "github.com" &&
        page.path() == QString("/%1/releases/tag/%2").arg(repository, result.tag))
        result.page = page;
    else
        result.page = QUrl(QString("https://github.com/%1/releases/tag/%2").arg(repository, result.tag));
    result.installerName = QString("nova-studio-%1-windows-x64-mingw.exe").arg(result.version);
    for (const auto &value : release["assets"].toArray()) {
        const auto asset = value.toObject();
        const auto name = asset["name"].toString();
        if (name == result.installerName)
            result.installer = releaseAsset(asset, result.tag);
        else if (name == "SHA256SUMS.txt")
            result.checksums = releaseAsset(asset, result.tag);
    }
    return result;
}

bool newer(const QString &candidate, const QString &current) {
    const auto next = QVersionNumber::fromString(candidate);
    qsizetype suffix = 0;
    const auto now = QVersionNumber::fromString(current, &suffix);
    if (next.isNull() || now.isNull())
        return false;
    const auto order = QVersionNumber::compare(next, now);
    // 0.1.0 is the final release of the 0.1.0-preview.1 build.
    return order > 0 || (order == 0 && suffix < current.size());
}

QByteArray checksumFor(const QByteArray &sums, const QString &fileName) {
    static const QRegularExpression line(R"(^([0-9a-fA-F]{64}) [ *](.+)$)");
    for (const auto &raw : sums.split('\n')) {
        const auto match = line.match(QString::fromUtf8(raw).trimmed());
        if (match.hasMatch() && match.captured(2) == fileName)
            return match.captured(1).toLower().toLatin1();
    }
    return {};
}
} // namespace nova::updates
