// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#endif

namespace nova::testing {
namespace {
constexpr qint64 maxMemoryBody = 4 * 1024 * 1024;
const QRegularExpression &shaRule() {
    static const QRegularExpression rule("^[0-9a-f]{64}$");
    return rule;
}
} // namespace

bool urlAllowed(const QUrl &url, const DownloadPolicy &policy) {
    if (!url.isValid() || !url.userInfo().isEmpty() || !url.fragment().isEmpty())
        return false;
    const auto host = url.host().toLower();
    if (url.scheme() == "https" && (url.port() == -1 || url.port() == 443))
        return policy.hosts.contains(host);
    return policy.allowLoopbackHttp && url.scheme() == "http" && host == "127.0.0.1";
}

DownloadPolicy officialLauncherPolicy() {
    return {{"launcher.mojang.com"}, false, 64LL * 1024 * 1024};
}
DownloadPolicy auroraPolicy() {
    return {{"api.github.com", "github.com", "objects.githubusercontent.com",
             "release-assets.githubusercontent.com"},
            false,
            256LL * 1024 * 1024};
}
InstallerPlan officialLauncherPlan() {
    InstallerPlan plan;
    plan.product = "Minecraft Launcher";
    plan.version = "current";
    plan.fileName = "MinecraftInstaller.msi";
    // Microsoft's own download link (aka.ms/minecraftClientWindows) resolves here.
    plan.url = QUrl("https://launcher.mojang.com/download/MinecraftInstaller.msi");
    plan.requiredSigner = "Microsoft";
    plan.msi = true;
    return plan;
}
QUrl auroraReleasesUrl() {
    return QUrl(
        QString("https://api.github.com/repos/%1/releases?per_page=20").arg(auroraRepository));
}

std::optional<QUrl> pickAuroraRelease(const QByteArray &json, QString *version, QString *error) {
    const auto releases = QJsonDocument::fromJson(json).array();
    for (const auto &value : releases) {
        const auto release = value.toObject();
        if (release.value("draft").toBool(true) || release.value("prerelease").toBool(true))
            continue;
        const auto tag = release.value("tag_name").toString();
        if (!tag.startsWith("native-v"))
            continue;
        for (const auto &asset : release.value("assets").toArray()) {
            const auto o = asset.toObject();
            if (o.value("name").toString() != "native-release-assets.json")
                continue;
            const QUrl url(o.value("browser_download_url").toString());
            const QString expected = QString("/%1/releases/download/%2/native-release-assets.json")
                                         .arg(auroraRepository, tag);
            if (url.scheme() != "https" || url.host() != "github.com" ||
                url.path().compare(expected, Qt::CaseInsensitive) != 0)
                continue;
            if (version)
                *version = tag.mid(8);
            return url;
        }
    }
    if (error)
        *error = "No published Aurora Launcher release with a verified asset manifest was found";
    return std::nullopt;
}

std::optional<InstallerPlan> parseAuroraAssetManifest(const QByteArray &json,
                                                      const QUrl &manifestUrl, QString *error) {
    auto fail = [&](const QString &message) -> std::optional<InstallerPlan> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const auto o = QJsonDocument::fromJson(json).object();
    if (o.value("schemaVersion").toInt() != 1)
        return fail("Aurora's release manifest has an unsupported schema");
    if (!o.value("product").toString().startsWith("com.aurora.launcher"))
        return fail("The release manifest does not describe Aurora Launcher");
    static const QRegularExpression installer("^[A-Za-z0-9._-]{1,96}_x64-setup\\.exe$");
    for (const auto &value : o.value("artifacts").toArray()) {
        const auto a = value.toObject();
        const auto name = a.value("name").toString();
        if (!installer.match(name).hasMatch())
            continue;
        InstallerPlan plan;
        plan.product = "Aurora Launcher";
        plan.version = o.value("version").toString().left(32);
        plan.fileName = name;
        plan.sha256 = a.value("sha256").toString().toLatin1();
        plan.expectedSize = static_cast<qint64>(a.value("sizeBytes").toDouble(-1));
        if (!shaRule().match(QString::fromLatin1(plan.sha256)).hasMatch() ||
            plan.expectedSize <= 0 || plan.expectedSize > auroraPolicy().maxBytes)
            return fail("The release manifest's installer entry is incomplete");
        plan.url = manifestUrl.resolved(QUrl(QString::fromLatin1(QUrl::toPercentEncoding(name))));
        return plan;
    }
    return fail("The release manifest lists no Windows installer");
}

SignatureCheck checkAuthenticode(const QString &path) {
    SignatureCheck check;
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = native.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (result == ERROR_SUCCESS) {
        check.status = SignatureCheck::Valid;
        if (auto provider = WTHelperProvDataFromStateData(data.hWVTStateData))
            if (auto signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
                signer && signer->csCertChain && signer->pasCertChain[0].pCert) {
                wchar_t name[256]{};
                CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                   nullptr, name, 256);
                check.signer = QString::fromWCharArray(name);
            }
    } else if (result == TRUST_E_NOSIGNATURE || result == TRUST_E_SUBJECT_FORM_UNKNOWN ||
               result == TRUST_E_PROVIDER_UNKNOWN) {
        check.status = SignatureCheck::Unsigned;
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
#else
    Q_UNUSED(path);
#endif
    return check;
}

QByteArray sha256Hex(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result().toHex() : QByteArray();
}

QString verifyInstaller(const QString &path, const InstallerPlan &plan, SignatureCheck *signature) {
    const QFileInfo info(path);
    if (!info.isFile())
        return "The downloaded installer is missing";
    if (plan.expectedSize >= 0 && info.size() != plan.expectedSize)
        return "The downloaded installer has the wrong size";
    if (!plan.sha256.isEmpty() && sha256Hex(path) != plan.sha256)
        return "The downloaded installer does not match its published SHA-256";
    const auto check = checkAuthenticode(path);
    if (signature)
        *signature = check;
    if (check.status == SignatureCheck::Invalid)
        return "The installer's digital signature is invalid";
    if (!plan.requiredSigner.isEmpty() &&
        (check.status != SignatureCheck::Valid ||
         !check.signer.contains(plan.requiredSigner, Qt::CaseInsensitive)))
        return "The installer is not signed by " + plan.requiredSigner;
    return {};
}

bool runInstaller(const QString &path, const InstallerPlan &plan, QString *error) {
    const auto native = QDir::toNativeSeparators(path);
    const bool started = plan.msi ? QProcess::startDetached("msiexec.exe", {"/i", native})
                                  : QProcess::startDetached(native, {});
    if (!started && error)
        *error = "The installer could not be started";
    return started;
}

Fetch::Fetch(QNetworkAccessManager *network, DownloadPolicy policy, QObject *parent)
    : QObject(parent), network(network), policy(std::move(policy)) {}

Fetch::~Fetch() {
    if (reply)
        reply->abort();
    if (file) {
        file->remove();
        delete file;
    }
}

void Fetch::start(const QUrl &url, const QString &destination) {
    target = destination;
    data.clear();
    received = 0;
    hops = 0;
    done = false;
    if (!target.isEmpty()) {
        QDir().mkpath(QFileInfo(target).absolutePath());
        file = new QFile(target + ".part");
        if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail("The download folder is not writable");
    }
    request(url);
}

void Fetch::request(const QUrl &url) {
    if (!urlAllowed(url, policy))
        return fail("The download was redirected to an unexpected address (" + url.host() + ")");
    QNetworkRequest outgoing(url);
    outgoing.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                          QNetworkRequest::ManualRedirectPolicy);
    outgoing.setHeader(QNetworkRequest::UserAgentHeader, "NovaStudio-LauncherSetup");
    outgoing.setTransferTimeout(30000);
    if (url.host() == "api.github.com")
        outgoing.setRawHeader("Accept", "application/vnd.github+json");
    reply = network->get(outgoing);
    connect(reply, &QNetworkReply::readyRead, this, [this] {
        const auto chunk = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200)
            return; // redirect and error bodies are never kept
        received += chunk.size();
        if (received > policy.maxBytes || (!file && received > maxMemoryBody))
            return fail("The download is larger than expected");
        if (file ? file->write(chunk) != chunk.size() : (data.append(chunk), false))
            return fail("The download could not be written to disk");
    });
    connect(reply, &QNetworkReply::downloadProgress, this, &Fetch::progress);
    connect(reply, &QNetworkReply::finished, this, [this] {
        if (done)
            return;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            const auto next = reply->url().resolved(
                reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            reply->deleteLater();
            if (++hops > 5)
                return fail("Too many redirects");
            return this->request(next);
        }
        if (reply->error() != QNetworkReply::NoError || status != 200)
            return fail(status ? QString("The server answered HTTP %1").arg(status)
                               : "Network error: " + reply->errorString());
        const auto rest = reply->readAll();
        received += rest.size();
        if (received > policy.maxBytes)
            return fail("The download is larger than expected");
        if (file) {
            if (file->write(rest) != rest.size() || !file->flush())
                return fail("The download could not be written to disk");
            file->close();
            QFile::remove(target);
            if (!file->rename(target))
                return fail("The download could not be finalized");
            delete file;
            file = nullptr;
        } else
            data.append(rest);
        reply->deleteLater();
        done = true;
        emit finished({});
    });
}

void Fetch::cancel() { fail("Download cancelled"); }

void Fetch::fail(const QString &error) {
    if (done)
        return;
    done = true;
    if (reply) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (file) {
        file->close();
        file->remove();
        delete file;
        file = nullptr;
    }
    data.clear();
    emit finished(error);
}

} // namespace nova::testing
