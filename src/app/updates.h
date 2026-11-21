// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Nova Studio releases on GitHub: which one is newest, and where its installer and checksums are.
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

namespace nova::updates {
inline constexpr auto repository = "kalibsolomon-pixel/Nova-Studio";
QUrl latestReleaseUrl();

struct Release {
    QString version, tag;
    QUrl page;
    QString installerName;
    QUrl installer, checksums; // empty when the release does not publish them
};
// The GitHub "latest release" answer. Drafts, prereleases and tags that are not vX.Y.Z are refused.
std::optional<Release> parseLatestRelease(const QByteArray &json, QString *error = nullptr);
// Whether release `candidate` (X.Y.Z) is newer than `current` (X.Y.Z, or X.Y.Z-suffix for a preview).
bool newer(const QString &candidate, const QString &current);
// The lowercase SHA-256 that a SHA256SUMS.txt lists for `fileName`, or empty.
QByteArray checksumFor(const QByteArray &sums, const QString &fileName);
} // namespace nova::updates
