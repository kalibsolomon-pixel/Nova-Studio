// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-F Windows security boundaries: MCP pipe access control, project containment
// edge cases, the managed-runtime socket directory and recovery storage. Uses real
// Windows objects (junctions, hard links, tokens, DACLs) and never needs elevation.
#include "core.h"
#include "fabric_workspace.h"
#include "recovery.h"
#include "scope.h"
#include "service.h"
#include "window.h"
#include <QAbstractButton>
#include <QMessageBox>
#include <QTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <thread>
#include <qt_windows.h>
#include <aclapi.h>
#include <sddl.h>

using namespace nova;
using nova::testing::prepareSocketDirectory;
namespace {
std::wstring native(const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); }
void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}
QString currentUserSid() {
    HANDLE token = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> buffer(size);
    GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    wchar_t *text = nullptr;
    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text);
    const auto sid = QString::fromWCharArray(text);
    LocalFree(text);
    return sid;
}
struct Ace {
    QString sid;
    bool allow;
    ACCESS_MASK mask;
};
// Owner SID and DACL entries of an object, read through a handle.
QList<Ace> dacl(HANDLE handle, SE_OBJECT_TYPE type, QString *owner = nullptr) {
    PSID ownerSid = nullptr;
    PACL list = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QList<Ace> out;
    if (GetSecurityInfo(handle, type, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, &ownerSid,
                        nullptr, &list, nullptr, &descriptor) != ERROR_SUCCESS)
        return out;
    wchar_t *text = nullptr;
    if (owner && ownerSid && ConvertSidToStringSidW(ownerSid, &text)) {
        *owner = QString::fromWCharArray(text);
        LocalFree(text);
    }
    for (DWORD i = 0; list && i < list->AceCount; ++i) {
        void *raw = nullptr;
        if (!GetAce(list, i, &raw))
            continue;
        auto header = static_cast<ACE_HEADER *>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE)
            continue;
        auto ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
        if (ConvertSidToStringSidW(&ace->SidStart, &text)) {
            out.append({QString::fromWCharArray(text), header->AceType == ACCESS_ALLOWED_ACE_TYPE, ace->Mask});
            LocalFree(text);
        }
    }
    LocalFree(descriptor);
    return out;
}
HANDLE openPath(const QString &path, bool directory) {
    return CreateFileW(native(path).c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
}
// Broad principals that must never be granted access to user-private objects.
const QStringList broad = {"S-1-1-0" /* Everyone */, "S-1-5-11" /* Authenticated Users */,
                           "S-1-5-32-545" /* Users */, "S-1-5-7" /* Anonymous */, "S-1-5-2" /* Network */,
                           "S-1-5-4" /* Interactive */, "S-1-15-2-1" /* All app packages */};
bool grantsBroadWrite(const QList<Ace> &aces) {
    for (const auto &ace : aces)
        if (ace.allow && broad.contains(ace.sid) &&
            (ace.mask & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC)))
            return true;
    return false;
}
bool junction(const QString &link, const QString &target) {
    QProcess cmd;
    cmd.setProgram("cmd.exe");
    cmd.setNativeArguments("/d /c mklink /J \"" + QDir::toNativeSeparators(link) + "\" \"" +
                           QDir::toNativeSeparators(target) + "\"");
    cmd.start();
    return cmd.waitForFinished(10000) && cmd.exitCode() == 0;
}
} // namespace

class Phase4fSecurityTests : public QObject {
    Q_OBJECT
    QString root, outside;
    QTemporaryDir tmp;
  private slots:
    void initTestCase() {
        QVERIFY(tmp.isValid());
        root = QFileInfo(tmp.filePath("pack")).absoluteFilePath();
        outside = tmp.filePath("outside");
        write(root + "/pack.mcmeta", R"({"pack":{"pack_format":46,"description":"Security"}})");
        write(root + "/assets/nova/textures/block/a.png", "png");
        write(outside + "/secret.png", "secret");
    }
    // The MCP endpoint is a named pipe whose DACL grants only the current user, and a
    // low-integrity process of the same user cannot open it.
    void pipeIsPrivateToTheUser() {
        mcp::Service service([](mcp::Snapshot &, const QString &) { return false; }, nullptr);
        QVERIFY(service.authorize(QDir::cleanPath(root), false));
        const auto pipe = QString("\\\\.\\pipe\\nova-mcp-") + service.connectionCode().left(32);
        // A low-integrity token (as browsers and sandboxed apps use) is refused.
        HANDLE process = nullptr, low = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process));
        QVERIFY(DuplicateTokenEx(process, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenImpersonation, &low));
        PSID lowSid = nullptr;
        QVERIFY(ConvertStringSidToSidW(L"S-1-16-4096", &lowSid));
        TOKEN_MANDATORY_LABEL label{{lowSid, SE_GROUP_INTEGRITY}};
        QVERIFY(SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(lowSid)));
        DWORD lowError = 0;
        std::thread([&] {
            SetThreadToken(nullptr, low);
            HANDLE h = CreateFileW(pipe.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            lowError = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
            RevertToSelf();
        }).join();
        LocalFree(lowSid);
        CloseHandle(low);
        CloseHandle(process);
        QCOMPARE(lowError, DWORD(ERROR_ACCESS_DENIED));
        // The user's own medium-integrity client connects; the DACL names no broad group.
        HANDLE h = CreateFileW(pipe.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        QVERIFY2(h != INVALID_HANDLE_VALUE, qPrintable(QString::number(GetLastError())));
        const auto aces = dacl(h, SE_KERNEL_OBJECT);
        CloseHandle(h);
        QStringList described;
        bool user = false;
        for (const auto &ace : aces) {
            described << QString("%1 %2 %3").arg(ace.allow ? "allow" : "deny", ace.sid).arg(ace.mask, 0, 16);
            user |= ace.allow && ace.sid == currentUserSid();
            QVERIFY2(!(ace.allow && broad.contains(ace.sid)), qPrintable(described.join("; ")));
        }
        qInfo("pipe DACL: %s", qPrintable(described.join("; ")));
        QVERIFY(user);
        // A revoked grant closes the endpoint; the old name no longer accepts clients.
        service.revoke();
        h = CreateFileW(pipe.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        QCOMPARE(h, INVALID_HANDLE_VALUE);
    }
    // Containment edge cases beyond the Phase 4-E suite: Windows device names, 8.3 aliases,
    // trailing dots and spaces, alternate streams, case and separator tricks.
    void containmentRejectsWindowsPathTricks() {
        mcp::Scope scope(QDir::cleanPath(root));
        QVERIFY(!scope.identity().isEmpty());
        QVERIFY(scope.open("assets/nova/textures/block/a.png"));
        for (const QString path :
             {"assets/nova/textures/block/CON", "assets/nova/textures/block/nul.png", "assets/nova/textures/COM1/a.png",
              "assets/nova/textures/block/a.png.", "assets/nova/textures/block/a.png ",
              "assets/nova/textures/block/a.png::$DATA", "assets/nova/textures/block\\a.png",
              "assets/nova/textures/block/../block/a.png", "assets/nova/textures/./block/a.png",
              "Assets/nova/textures/block/a.png/", "/assets/nova/textures/block/a.png",
              "C:/assets/nova/textures/block/a.png", "assets/nova/.nova/documents/x.nvdoc",
              "assets/nova/textures/block/a?.png", "assets/nova/textures/block/a*.png",
              "assets//nova/textures/block/a.png"}) {
            QVERIFY2(!scope.vacant(path) && !scope.open(path), qPrintable(path));
        }
        // The 8.3 short alias of a directory resolves to a different final path, so it is
        // refused even though it designates a contained folder.
        write(root + "/assets/nova/textures/longfoldername/x.png", "x");
        wchar_t shortPath[MAX_PATH];
        const auto longPath = native(root + "/assets/nova/textures/longfoldername");
        if (GetShortPathNameW(longPath.c_str(), shortPath, MAX_PATH) && QString::fromWCharArray(shortPath).contains('~')) {
            const auto alias = QFileInfo(QString::fromWCharArray(shortPath)).fileName();
            QVERIFY(!scope.open("assets/nova/textures/" + alias + "/x.png"));
            qInfo("8.3 alias %s refused", qPrintable(alias));
        } else {
            qInfo("8.3 names are disabled on this volume; alias case not applicable");
        }
        // Native editable documents are always derived inside the project's .nova folder.
        QString error;
        QVERIFY(nativeDocumentPath(root, root + "/../outside/secret.png", &error).isEmpty() ||
                nativeDocumentPath(root, root + "/../outside/secret.png").startsWith(root + "/.nova/"));
        const auto inside = nativeDocumentPath(root, root + "/assets/nova/textures/block/a.png");
        QVERIFY2(inside.startsWith(root + "/.nova/"), qPrintable(inside));
    }
    // A junction placed where a save would land does not redirect the write outside.
    void saveDestinationThroughJunctionIsContainedByScope() {
        const auto link = root + "/assets/nova/textures/escape";
        QVERIFY(junction(link, outside));
        mcp::Scope scope(QDir::cleanPath(root));
        QVERIFY(!scope.vacant("assets/nova/textures/escape/new.png"));
        QVERIFY(!scope.open("assets/nova/textures/escape/secret.png"));
        // Editing paths refuse symbolic-link destinations outright.
        QVERIFY(RemoveDirectoryW(native(link).c_str()));
        QVERIFY(QFileInfo::exists(outside + "/secret.png"));
    }
    // Directory symbolic links need Developer Mode or elevation, which the owner deferred
    // to public-release acceptance. Only attempt creation with existing permissions.
    void directorySymbolicLinkIsDeferred() {
        const auto link = root + "/assets/nova/textures/dirlink";
        const bool created = CreateSymbolicLinkW(native(link).c_str(), native(outside).c_str(),
                                                 SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
        if (!created) {
            const auto error = GetLastError();
            QSKIP(qPrintable(QString("Deferred to public-release acceptance: directory symbolic links are unavailable "
                                     "under current permissions (Win32 error %1). Developer Mode and elevation not used.")
                                 .arg(error)));
        }
        mcp::Scope scope(QDir::cleanPath(root));
        const bool refused = !scope.open("assets/nova/textures/dirlink/secret.png") &&
                             !scope.vacant("assets/nova/textures/dirlink/new.png");
        RemoveDirectoryW(native(link).c_str());
        QVERIFY(refused);
    }
    // The socket directory is owned by and private to the user, its cleanup never reaches
    // outside it, and concurrent runtime launches agree on it.
    void socketDirectoryOwnershipCleanupAndConcurrency() {
        QTemporaryDir own;
        const auto local = QDir::toNativeSeparators(own.filePath("l"));
        const auto sockets = prepareSocketDirectory(local);
        QVERIFY(!sockets.isEmpty());
        HANDLE dir = openPath(sockets, true);
        QVERIFY(dir != INVALID_HANDLE_VALUE);
        QString owner;
        const auto aces = dacl(dir, SE_FILE_OBJECT, &owner);
        CloseHandle(dir);
        QStringList described;
        for (const auto &ace : aces)
            described << QString("%1 %2 %3").arg(ace.allow ? "allow" : "deny", ace.sid).arg(ace.mask, 0, 16);
        qInfo("NovaSockets owner %s DACL: %s", qPrintable(owner), qPrintable(described.join("; ")));
        QVERIFY2(owner == currentUserSid() || owner == "S-1-5-32-544", qPrintable(owner));
        QVERIFY2(!grantsBroadWrite(aces), qPrintable(described.join("; ")));
        // The real %LOCALAPPDATA% location, as used at runtime, has the same properties.
        const auto real = prepareSocketDirectory(qEnvironmentVariable("LOCALAPPDATA"));
        if (!real.isEmpty()) {
            HANDLE h = openPath(real, true);
            QVERIFY(h != INVALID_HANDLE_VALUE);
            const auto realAces = dacl(h, SE_FILE_OBJECT, &owner);
            CloseHandle(h);
            QVERIFY(!grantsBroadWrite(realAces));
            qInfo("runtime NovaSockets %s owner %s", qPrintable(real), qPrintable(owner));
        }
        // A stale "socket_" hard link to a file outside is removed as a link only.
        const auto dirPath = QDir::fromNativeSeparators(sockets);
        write(outside + "/keep.txt", "outside bytes");
        QVERIFY(CreateHardLinkW(native(dirPath + "/socket_3333333333").c_str(), native(outside + "/keep.txt").c_str(), nullptr));
        // A directory junction named like a socket is never followed or removed.
        QVERIFY(junction(dirPath + "/socket_4444444444", outside));
        int removed = -1;
        QCOMPARE(prepareSocketDirectory(local, &removed, 0), sockets);
        QCOMPARE(removed, 1);
        QFile kept(outside + "/keep.txt");
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("outside bytes"));
        QVERIFY(QFileInfo::exists(outside + "/secret.png"));
        QVERIFY(QFileInfo(dirPath + "/socket_4444444444").isJunction());
        QVERIFY(RemoveDirectoryW(native(dirPath + "/socket_4444444444").c_str()));
        // NovaSockets itself replaced by a junction, or by a file, is refused.
        QTemporaryDir other;
        QVERIFY(QDir().mkpath(other.filePath("l/Temp")));
        QVERIFY(junction(other.filePath("l/Temp/NovaSockets"), outside));
        QVERIFY(prepareSocketDirectory(QDir::toNativeSeparators(other.filePath("l"))).isEmpty());
        QVERIFY(RemoveDirectoryW(native(other.filePath("l/Temp/NovaSockets")).c_str()));
        write(other.filePath("l/Temp/NovaSockets"), "not a directory");
        QVERIFY(prepareSocketDirectory(QDir::toNativeSeparators(other.filePath("l"))).isEmpty());
        // Concurrent launches all receive the same directory and none fails.
        for (int round = 0; round < 10; ++round) {
            QTemporaryDir race;
            const auto raceLocal = QDir::toNativeSeparators(race.filePath("l"));
            std::vector<QString> seen(16);
            std::vector<std::thread> threads;
            for (int i = 0; i < 16; ++i)
                threads.emplace_back([&, i] { seen[size_t(i)] = prepareSocketDirectory(raceLocal); });
            for (auto &t : threads)
                t.join();
            QVERIFY(!seen.front().isEmpty());
            for (const auto &s : seen)
                QCOMPARE(s, seen.front());
        }
    }
    // Recovery snapshots are private to the user, record their project, and a failed or
    // interrupted snapshot write leaves the previous snapshot readable.
    void recoveryStorageIsolationAndPermissions() {
        QTemporaryDir profile;
        RecoveryStore store(profile.filePath("recovery"));
        const auto id = QUuid::createUuid(), other = QUuid::createUuid();
        QImage image(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        QString error;
        QVERIFY2(store.write(id, root + "/assets/nova/textures/block/a.png", root, {}, image, &error), qPrintable(error));
        QVERIFY(store.write(other, outside + "/secret.png", outside, {}, image, &error));
        HANDLE h = openPath(store.pathFor(id), false);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        const auto aces = dacl(h, SE_FILE_OBJECT);
        CloseHandle(h);
        QVERIFY(!grantsBroadWrite(aces));
        // Each entry carries its own project; the window offers only the open project's.
        int mine = 0, theirs = 0;
        for (const auto &entry : store.entries()) {
            mine += pathIdentity(entry.project) == pathIdentity(root);
            theirs += pathIdentity(entry.project) == pathIdentity(outside);
        }
        QCOMPARE(mine, 1);
        QCOMPARE(theirs, 1);
        // An interrupted snapshot write (oversized image) keeps the previous snapshot.
        QVERIFY(!store.write(id, root + "/assets/nova/textures/block/a.png", root, {}, QImage(), &error));
        QCOMPARE(store.load(id).image.convertToFormat(QImage::Format_RGBA8888),
                 image.convertToFormat(QImage::Format_RGBA8888));
        // A tampered snapshot is reported unreadable instead of being restored.
        QFile tamper(store.pathFor(other));
        QVERIFY(tamper.open(QIODevice::ReadWrite));
        tamper.seek(tamper.size() / 2);
        tamper.write("XXXX");
        tamper.close();
        QVERIFY(!store.load(other).error.isEmpty());
        // Discard removes exactly one snapshot.
        store.discard(other);
        QVERIFY(!QFileInfo::exists(store.pathFor(other)));
        QVERIFY(QFileInfo::exists(store.pathFor(id)));
    }
    // Startup recovery offers only snapshots recorded for the open project. Another
    // project's snapshot is neither offered nor deleted.
    void windowOffersOnlyOpenProjectSnapshots() {
        QTemporaryDir profile;
        const auto oldSettings = qApp->property("settingsFile");
        qApp->setProperty("settingsFile", profile.filePath("studio.ini"));
        RecoveryStore store(profile.filePath("recovery"));
        const auto mine = QUuid::createUuid(), theirs = QUuid::createUuid();
        QImage image(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        const auto texture = root + "/assets/nova/textures/block/a.png";
        QVERIFY(store.write(mine, texture, root, Revision::read(texture), image));
        QVERIFY(store.write(theirs, outside + "/secret.png", outside, Revision::read(outside + "/secret.png"), image));
        QStringList prompts;
        QTimer poll;
        connect(&poll, &QTimer::timeout, [&] {
            for (auto w : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(w); box && box->isVisible() && box->objectName() == "recoveryDialog") {
                    prompts << box->text();
                    box->escapeButton()->click(); // Later: keep the snapshot
                }
        });
        poll.start(20);
        {
            QElapsedTimer started;
            started.start();
            Window window(root, {}, started);
            window.show();
            QTest::qWait(3000);
            window.close();
        }
        poll.stop();
        qApp->setProperty("settingsFile", oldSettings);
        QCOMPARE(prompts.size(), 1);
        QVERIFY2(prompts.first().contains("a.png"), qPrintable(prompts.first()));
        QVERIFY(QFileInfo::exists(store.pathFor(mine)));
        QVERIFY(QFileInfo::exists(store.pathFor(theirs)));
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Nova Phase 4-F Security");
    Phase4fSecurityTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "phase4f_security_test.moc"
