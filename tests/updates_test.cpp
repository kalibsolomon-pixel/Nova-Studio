// SPDX-License-Identifier: GPL-3.0-or-later
// Release selection for the updater: versions, the GitHub "latest release" answer and SHA256SUMS.txt.
#include "updates.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace nova::updates;

namespace {
QJsonObject asset(const QString &tag, const QString &name, const QString &host = "github.com") {
    return {{"name", name},
            {"browser_download_url",
             QString("https://%1/%2/releases/download/%3/%4").arg(host, repository, tag, name)}};
}
QByteArray release(const QString &tag, QJsonArray assets, bool prerelease = false) {
    return QJsonDocument(QJsonObject{{"tag_name", tag}, {"draft", false}, {"prerelease", prerelease},
                                     {"html_url", QString("https://github.com/%1/releases/tag/%2").arg(repository, tag)},
                                     {"assets", assets}})
        .toJson();
}
} // namespace

class UpdatesTest : public QObject {
    Q_OBJECT
  private slots:
    void versions() {
        QVERIFY(newer("0.1.3", "0.1.2"));
        QVERIFY(newer("0.2.0", "0.1.12"));
        QVERIFY(newer("0.1.10", "0.1.9"));
        QVERIFY(newer("0.1.0", "0.1.0-preview.1"));
        QVERIFY(!newer("0.1.2", "0.1.2"));
        QVERIFY(!newer("0.1.1", "0.1.2"));
        QVERIFY(!newer("", "0.1.2"));
        QVERIFY(!newer("0.1.3", ""));
    }

    void latestRelease() {
        const auto tag = QString("v0.1.3");
        const auto parsed = parseLatestRelease(release(tag, {asset(tag, "nova-studio-0.1.3-windows-x64-mingw.exe"),
                                                             asset(tag, "nova-studio-0.1.3-windows-x64-mingw.zip"),
                                                             asset(tag, "SHA256SUMS.txt")}));
        QVERIFY(parsed);
        QCOMPARE(parsed->version, "0.1.3");
        QCOMPARE(parsed->installerName, "nova-studio-0.1.3-windows-x64-mingw.exe");
        QCOMPARE(parsed->installer.toString(),
                 QString("https://github.com/%1/releases/download/v0.1.3/nova-studio-0.1.3-windows-x64-mingw.exe")
                     .arg(repository));
        QVERIFY(parsed->checksums.toString().endsWith("/v0.1.3/SHA256SUMS.txt"));
        QCOMPARE(parsed->page.toString(), QString("https://github.com/%1/releases/tag/v0.1.3").arg(repository));
    }

    void refusedReleases() {
        QString error;
        QVERIFY(!parseLatestRelease("not json", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!parseLatestRelease(release("v0.1.3", {}, true)));
        QVERIFY(!parseLatestRelease(release("v0.1.3-rc.1", {})));
        QVERIFY(!parseLatestRelease(release("native-v0.1.3", {})));
        auto draft = QJsonDocument::fromJson(release("v0.1.3", {})).object();
        draft["draft"] = true;
        QVERIFY(!parseLatestRelease(QJsonDocument(draft).toJson()));
    }

    // Assets outside this release's folder in the Nova Studio repository are ignored.
    void foreignAssetsIgnored() {
        const auto tag = QString("v0.1.3");
        const auto name = QString("nova-studio-0.1.3-windows-x64-mingw.exe");
        auto other = asset(tag, name);
        other["browser_download_url"] = QString("https://github.com/someone/else/releases/download/v0.1.3/") + name;
        auto parsed = parseLatestRelease(release(tag, {other, asset(tag, "SHA256SUMS.txt", "example.com")}));
        QVERIFY(parsed);
        QVERIFY(parsed->installer.isEmpty());
        QVERIFY(parsed->checksums.isEmpty());
        auto older = asset("v0.1.2", name);
        parsed = parseLatestRelease(release(tag, {older}));
        QVERIFY(parsed && parsed->installer.isEmpty());
    }

    void checksums() {
        const QByteArray sums =
            "D077C641E4A01F17978F8045F514822C20C36D7117200F7D35494BE734BB6DDB  nova-studio-0.1.3-windows-x64-mingw.exe\n"
            "7db7c6eb52d66f16cad639fc0f0a10ed657b548b6fb8d792e7ca9fcdcadcf4fe *nova-studio-0.1.3-windows-x64-mingw.zip\r\n"
            "short  nova-studio-0.1.3-source.zip\n";
        QCOMPARE(checksumFor(sums, "nova-studio-0.1.3-windows-x64-mingw.exe"),
                 "d077c641e4a01f17978f8045f514822c20c36d7117200f7d35494be734bb6ddb");
        QCOMPARE(checksumFor(sums, "nova-studio-0.1.3-windows-x64-mingw.zip"),
                 "7db7c6eb52d66f16cad639fc0f0a10ed657b548b6fb8d792e7ca9fcdcadcf4fe");
        QVERIFY(checksumFor(sums, "nova-studio-0.1.3-source.zip").isEmpty());
        QVERIFY(checksumFor(sums, "missing.exe").isEmpty());
    }
};

QTEST_GUILESS_MAIN(UpdatesTest)
#include "updates_test.moc"
