// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata.h"
#include "revision.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>

namespace nova::vanilla {
namespace {
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
std::optional<QJsonObject> object(const QByteArray &bytes) {
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}
// Integral JSON number in [minimum, maximum].
bool readSize(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *out) {
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!(d >= double(minimum)) || d > double(maximum) || std::floor(d) != d)
        return false;
    *out = qint64(d);
    return true;
}
// https on the role's host (always also passing the policy); loopback http only when the
// policy allows it (tests). No queries, fragments or user info.
bool roleUrl(const QJsonValue &value, const char *host, const testing::DownloadPolicy &policy,
             QUrl *out) {
    if (!value.isString())
        return false;
    const QUrl url(value.toString(), QUrl::StrictMode);
    if (!url.isValid() || url.hasQuery() || !testing::urlAllowed(url, policy))
        return false;
    if (url.scheme() == "https" && url.host().toLower() != QLatin1String(host))
        return false;
    *out = url;
    return true;
}
bool readDownload(const QJsonValue &value, const char *host, qint64 maximum,
                  const testing::DownloadPolicy &policy, Download *out) {
    if (!value.isObject())
        return false;
    const auto o = value.toObject();
    out->sha1 = o.value("sha1").toString();
    return validSha1(out->sha1) && readSize(o.value("size"), 1, maximum, &out->size) &&
           roleUrl(o.value("url"), host, policy, &out->url);
}
QString hashFile(const QString &path, QCryptographicHash::Algorithm algorithm,
                 const std::shared_ptr<std::atomic_bool> &cancel) {
    QFile file;
    if (!openSharedRead(path, file))
        return {};
    QCryptographicHash hash(algorithm);
    while (!file.atEnd()) {
        if (cancel && cancel->load())
            return {};
        const auto chunk = file.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return {};
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}
} // namespace

testing::DownloadPolicy vanillaPolicy() {
    return {{metaHost, dataHost, resourcesHost}, false, maxClientBytes,
            "NovaStudio-VanillaBaseline"};
}

QString notListedMessage(const QString &versionId) {
    return QString("Minecraft Java %1 is not listed in Mojang's version manifest").arg(versionId);
}

std::optional<VersionRef> parseVersionManifest(const QByteArray &bytes, const QString &versionId,
                                               QString *error,
                                               const testing::DownloadPolicy &policy) {
    const auto root = object(bytes);
    if (!root)
        return fail(error, "Mojang's version manifest is not valid JSON"), std::nullopt;
    const auto versions = root->value("versions");
    if (!versions.isArray())
        return fail(error, "Mojang's version manifest has an unexpected structure"), std::nullopt;
    std::optional<VersionRef> found;
    for (const auto &value : versions.toArray()) {
        if (!value.isObject() || !value.toObject().value("id").isString())
            return fail(error, "Mojang's version manifest has an unexpected structure"),
                   std::nullopt;
        const auto o = value.toObject();
        if (o.value("id").toString() != versionId)
            continue; // exact match only: another version is never substituted
        if (found)
            return fail(error, QString("Mojang's version manifest lists %1 more than once")
                                   .arg(versionId)),
                   std::nullopt;
        VersionRef ref;
        ref.id = versionId;
        ref.type = o.value("type").toString();
        ref.sha1 = o.value("sha1").toString();
        if (!o.value("type").isString() || !validSha1(ref.sha1) ||
            !roleUrl(o.value("url"), metaHost, policy, &ref.url))
            return fail(error, QString("Mojang's version manifest entry for %1 is incomplete or "
                                       "points at an unexpected address")
                                   .arg(versionId)),
                   std::nullopt;
        found = ref;
    }
    if (!found)
        fail(error, notListedMessage(versionId));
    return found;
}

std::optional<VersionMetadata> parseVersionJson(const QByteArray &bytes, const VersionRef &ref,
                                                QString *error,
                                                const testing::DownloadPolicy &policy) {
    if (sha1Hex(bytes) != ref.sha1)
        return fail(error, QString("The version metadata for %1 does not match the SHA-1 "
                                   "published in Mojang's manifest")
                               .arg(ref.id)),
               std::nullopt;
    const auto root = object(bytes);
    if (!root)
        return fail(error, "The version metadata is not valid JSON"), std::nullopt;
    if (root->value("id").toString() != ref.id)
        return fail(error, QString("The version metadata does not describe %1").arg(ref.id)),
               std::nullopt;
    VersionMetadata m;
    m.id = ref.id;
    m.versionUrl = ref.url;
    m.versionSha1 = ref.sha1;
    if (!readDownload(root->value("downloads").toObject().value("client"), dataHost,
                      maxClientBytes, policy, &m.client))
        return fail(error, "The version metadata's client download is incomplete or points at an "
                           "unexpected address"),
               std::nullopt;
    static const QRegularExpression indexId("^[A-Za-z0-9._-]{1,64}$");
    const auto index = root->value("assetIndex");
    m.assetIndexId = index.toObject().value("id").toString();
    if (!indexId.match(m.assetIndexId).hasMatch() || m.assetIndexId.contains("..") ||
        !readDownload(index, metaHost, maxAssetIndexBytes, policy, &m.assetIndex))
        return fail(error, "The version metadata's asset index is incomplete or points at an "
                           "unexpected address"),
               std::nullopt;
    return m;
}

std::optional<QList<AssetTexture>> parseAssetIndexTextures(const QByteArray &bytes,
                                                           QString *error) {
    const auto root = object(bytes);
    if (!root || !root->value("objects").isObject())
        return fail(error, "The asset index is not valid"), std::nullopt;
    const auto objects = root->value("objects").toObject();
    QList<AssetTexture> textures;
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        const auto o = it.value().toObject();
        AssetTexture t;
        t.sha1 = o.value("hash").toString();
        if (!it.value().isObject() || !validSha1(t.sha1) ||
            !readSize(o.value("size"), 0, 4LL * 1024 * 1024 * 1024, &t.size))
            return fail(error, "The asset index has a malformed object entry"), std::nullopt;
        const auto key = it.key();
        if (!key.startsWith("minecraft/textures/", Qt::CaseInsensitive) ||
            !(key.endsWith(".png", Qt::CaseInsensitive) ||
              key.endsWith(".png.mcmeta", Qt::CaseInsensitive)))
            continue; // sounds, languages and other assets are never stored
        t.path = "assets/" + key;
        QString why;
        if (!validBaselinePath(t.path, &why))
            return fail(error, "The asset index lists unexpected data: " + why), std::nullopt;
        if (t.size < 1 || t.size > maxTextureBytes)
            return fail(error, "The asset index lists a texture with an unexpected size"),
                   std::nullopt;
        textures.append(t);
    }
    std::sort(textures.begin(), textures.end(),
              [](const AssetTexture &a, const AssetTexture &b) { return a.path < b.path; });
    return textures;
}

bool validBaselinePath(const QString &relative, QString *error) {
    const auto shown = relative.left(160);
    auto reject = [&](const char *why) {
        return fail(error, QString("%1 (%2)").arg(shown, QLatin1String(why)));
    };
    if (relative.isEmpty())
        return reject("empty path");
    if (relative.contains(QChar(0)))
        return reject("NUL character");
    if (relative.contains('\\'))
        return reject("backslash");
    if (relative.contains(':'))
        return reject("colon");
    if (!relative.startsWith(QLatin1String(texturePrefix)))
        return reject("outside assets/minecraft/textures/");
    if (!relative.endsWith(".png") && !relative.endsWith(".png.mcmeta"))
        return reject("not a .png or .png.mcmeta file");
    const auto segments = relative.split('/');
    if (segments.size() > 33)
        return reject("too many folders");
    static const QRegularExpression allowed("^[a-z0-9_.-]+$");
    static const QRegularExpression device("^(con|prn|aux|nul|com[0-9]|lpt[0-9])$");
    for (const auto &segment : segments) {
        if (segment.isEmpty())
            return reject("empty folder name");
        if (segment == "." || segment == "..")
            return reject("relative folder reference");
        if (segment.size() > 255)
            return reject("name longer than 255 characters");
        if (segment.endsWith('.') || segment.endsWith(' '))
            return reject("name ends with a dot or space");
        if (!allowed.match(segment).hasMatch())
            return reject("uppercase or unsupported character");
        if (device.match(segment.section('.', 0, 0)).hasMatch())
            return reject("reserved Windows device name");
    }
    return true;
}

bool validVersionId(const QString &version) {
    static const QRegularExpression rule("^[0-9a-z](?:[0-9a-z._-]{0,62}[0-9a-z])?$");
    return rule.match(version).hasMatch() && !version.contains("..");
}
bool validSha1(const QString &hex) {
    static const QRegularExpression rule("^[0-9a-f]{40}$");
    return rule.match(hex).hasMatch();
}
bool validSha256(const QString &hex) {
    static const QRegularExpression rule("^[0-9a-f]{64}$");
    return rule.match(hex).hasMatch();
}

QString identityKey(const VersionMetadata &m) {
    return sha256Hex(QString("nova-vanilla-baseline/1|java|%1|%2|%3|%4|%5")
                         .arg(m.id, m.client.sha1, QString::number(m.client.size), m.assetIndexId,
                              m.assetIndex.sha1)
                         .toUtf8());
}

QUrl assetObjectUrl(const QString &sha1, const QUrl &base) {
    return base.resolved(QUrl(sha1.left(2) + '/' + sha1));
}

QString sha1Hex(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}
QString sha256Hex(const QByteArray &bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString sha1File(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel) {
    return hashFile(path, QCryptographicHash::Sha1, cancel);
}
QString sha256File(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel) {
    return hashFile(path, QCryptographicHash::Sha256, cancel);
}

} // namespace nova::vanilla
