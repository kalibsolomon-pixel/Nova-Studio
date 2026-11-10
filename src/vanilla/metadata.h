// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Vanilla texture baseline: strict parsers for Mojang's published metadata
// (version manifest -> version JSON -> client jar / asset index) and the
// path, identity and download rules shared by the cache and the installer.
// See docs/vanilla-baseline/DESIGN.md sections 2-3.
#include "testing/installer.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>
#include <atomic>
#include <memory>
#include <optional>

namespace nova::vanilla {

inline constexpr auto defaultManifestUrl =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
inline constexpr auto defaultObjectBaseUrl = "https://resources.download.minecraft.net/";
inline constexpr auto metaHost = "piston-meta.mojang.com";
inline constexpr auto dataHost = "piston-data.mojang.com";
inline constexpr auto resourcesHost = "resources.download.minecraft.net";
inline constexpr auto texturePrefix = "assets/minecraft/textures/";
inline constexpr qint64 maxClientBytes = 256LL * 1024 * 1024;
inline constexpr qint64 maxAssetIndexBytes = 4LL * 1024 * 1024;
inline constexpr qint64 maxTextureBytes = 16LL * 1024 * 1024; // per file (jar entry or object)

struct Download {
    QUrl url;
    QString sha1; // 40 lowercase hex
    qint64 size = 0;
};
struct VersionRef {
    QString id, type;
    QUrl url;
    QString sha1; // of the version JSON
};
struct VersionMetadata {
    QString id;
    QUrl versionUrl;
    QString versionSha1; // provenance only, not part of the identity
    Download client;
    QString assetIndexId;
    Download assetIndex;
};
struct AssetTexture {
    QString path; // assets/minecraft/textures/...
    QString sha1;
    qint64 size = 0;
};

// The three official hosts only, https only, 256 MiB per download.
testing::DownloadPolicy vanillaPolicy();

// `policy` decides which URLs metadata may point at (vanillaPolicy() in production; tests
// add loopback http). An https URL must additionally use the host of its role:
// piston-meta for version JSON / asset index, piston-data for the client.
std::optional<VersionRef> parseVersionManifest(const QByteArray &bytes, const QString &versionId,
                                               QString *error = nullptr,
                                               const testing::DownloadPolicy &policy = vanillaPolicy());
// "Minecraft Java <id> is not listed in Mojang's version manifest" (the absent-version error).
QString notListedMessage(const QString &versionId);
// Also checks that SHA-1(bytes) equals ref.sha1 and that the id matches.
std::optional<VersionMetadata> parseVersionJson(const QByteArray &bytes, const VersionRef &ref,
                                                QString *error = nullptr,
                                                const testing::DownloadPolicy &policy = vanillaPolicy());
// Textures (minecraft/textures/**.png|.png.mcmeta) mapped to assets/minecraft/textures/...,
// sorted by path. Every object entry is validated, textures or not.
std::optional<QList<AssetTexture>> parseAssetIndexTextures(const QByteArray &bytes,
                                                           QString *error = nullptr);

// DESIGN "Path rules" for every baseline path.
bool validBaselinePath(const QString &relative, QString *error = nullptr);
// Lowercase release-style ids only ("1.21.11", "26.1", "25w14a", "1.21.11-pre1").
bool validVersionId(const QString &version);
bool validSha1(const QString &hex);
bool validSha256(const QString &hex);

// sha256("nova-vanilla-baseline/1|java|<id>|<client.sha1>|<client.size>|<assetIndex.id>|<assetIndex.sha1>")
QString identityKey(const VersionMetadata &metadata);

// https://resources.download.minecraft.net/<sha1[0:2]>/<sha1> (base injectable for tests).
QUrl assetObjectUrl(const QString &sha1, const QUrl &base = QUrl(defaultObjectBaseUrl));

// QCryptographicHash::Sha1 / Sha256 as lowercase hex. File variants return "" on failure
// or cancellation and read in 4 MiB chunks. (testing's hashFile("sha1") is SHA-256: never use it.)
QString sha1Hex(const QByteArray &bytes);
QString sha256Hex(const QByteArray &bytes);
QString sha1File(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel = {});
QString sha256File(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel = {});

} // namespace nova::vanilla
