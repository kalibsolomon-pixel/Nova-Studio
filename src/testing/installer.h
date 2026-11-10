// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Confirmed download of a launcher installer from a pinned source. Nova
// only downloads after the user confirms the source and size, verifies the
// result before offering to run it, and runs the installer with its own
// visible UI (never silently).
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
namespace nova::testing {

struct DownloadPolicy {
    QStringList hosts;              // exact https hosts every redirect hop must use
    bool allowLoopbackHttp = false; // tests only
    qint64 maxBytes = 256LL * 1024 * 1024;
    QByteArray userAgent = "NovaStudio-LauncherSetup"; // sent on every hop
};
bool urlAllowed(const QUrl &url, const DownloadPolicy &policy);

struct InstallerPlan {
    QString product, version, fileName;
    QUrl url;
    qint64 expectedSize = -1; // -1 when the source publishes none
    QByteArray sha256;        // lowercase hex; empty when the source publishes none
    QString requiredSigner;   // Authenticode signer that must be present ("" = not required)
    bool msi = false;
};

inline constexpr auto auroraRepository = "kalibsolomon-pixel/Aurora-Launcher";
DownloadPolicy officialLauncherPolicy();
DownloadPolicy auroraPolicy();
InstallerPlan officialLauncherPlan();
QUrl auroraReleasesUrl();
// Picks the newest published (non-draft, non-prerelease) native release and
// returns its asset-manifest URL.
std::optional<QUrl> pickAuroraRelease(const QByteArray &releasesJson, QString *version,
                                      QString *error = nullptr);
std::optional<InstallerPlan>
parseAuroraAssetManifest(const QByteArray &json, const QUrl &manifestUrl, QString *error = nullptr);

struct SignatureCheck {
    enum Status { Valid, Unsigned, Invalid } status = Invalid;
    QString signer;
};
SignatureCheck checkAuthenticode(const QString &path);
QByteArray sha256Hex(const QString &path);
// Size, hash and signer checks for a finished download. Empty = acceptable.
QString verifyInstaller(const QString &path, const InstallerPlan &plan, SignatureCheck *signature);
bool runInstaller(const QString &path, const InstallerPlan &plan, QString *error = nullptr);

// Single bounded HTTPS fetch with per-hop redirect validation.
class Fetch : public QObject {
    Q_OBJECT
  public:
    Fetch(QNetworkAccessManager *network, DownloadPolicy policy, QObject *parent = nullptr);
    ~Fetch() override;
    // destination empty => response kept in memory (bounded to 4 MiB).
    void start(const QUrl &url, const QString &destination = {});
    void cancel();
    QByteArray body() const { return data; }

  signals:
    void progress(qint64 received, qint64 total);
    void finished(const QString &error); // empty on success

  private:
    void request(const QUrl &url);
    void fail(const QString &error);
    QNetworkAccessManager *network;
    DownloadPolicy policy;
    QPointer<QNetworkReply> reply;
    QFile *file = nullptr;
    QString target;
    QByteArray data;
    qint64 received = 0;
    int hops = 0;
    bool done = false;
};

} // namespace nova::testing
