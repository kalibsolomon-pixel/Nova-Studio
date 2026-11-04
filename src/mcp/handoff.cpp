// SPDX-License-Identifier: GPL-3.0-or-later
#include "handoff.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <qt_windows.h>
#include <sddl.h>
#include <shlobj.h>

namespace nova::mcp::handoff {
namespace {
constexpr auto Format = "nova-mcp-handoff-1";
constexpr qint64 FileLimit = 4096;

QString knownLocalAppData() {
    PWSTR path = nullptr;
    QString result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path)))
        result = QString::fromWCharArray(path);
    CoTaskMemFree(path);
    return result;
}
QString userSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(int(size), '\0');
    QString sid;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
            sid = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return sid;
}
// Owner-only access, and a medium mandatory label that low-integrity processes cannot read.
struct Security {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(bool directory) {
        const auto sid = userSid();
        if (sid.isEmpty())
            return;
        const auto inherit = directory ? QString("OICI") : QString();
        const auto sddl = QString("D:P(A;%1;FA;;;%2)S:(ML;%1;NRNWNX;;;ME)").arg(inherit, sid);
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &attributes.lpSecurityDescriptor, nullptr);
    }
    ~Security() { LocalFree(attributes.lpSecurityDescriptor); }
    bool valid() const { return attributes.lpSecurityDescriptor != nullptr; }
};
bool validCode(const QString &code) {
    static const QRegularExpression pattern("^[a-f0-9]{64}$");
    return pattern.match(code).hasMatch();
}
} // namespace

QString directory() {
    const auto overridden = qEnvironmentVariable("NOVA_MCP_HANDOFF_DIR");
    if (!overridden.isEmpty())
        return QDir::cleanPath(overridden);
    auto base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty() || !QDir::isAbsolutePath(base))
        base = knownLocalAppData();
    return base.isEmpty() ? QString{} : QDir::cleanPath(base + "/Nova Studio/Nova Studio/mcp");
}
QString filePath() {
    const auto dir = directory();
    return dir.isEmpty() ? QString{} : dir + "/pending-connection.json";
}

bool publish(const Pending &pending, QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto dir = directory();
    if (dir.isEmpty() || !validCode(pending.code))
        return fail("No per-user location is available for the connection offer.");
    Security dirSecurity(true), fileSecurity(false);
    if (!dirSecurity.valid() || !fileSecurity.valid())
        return fail("The connection offer could not be protected.");
    const auto parent = QFileInfo(dir).absolutePath();
    if (!QDir().mkpath(parent))
        return fail("The connection offer folder could not be created.");
    const auto nativeDir = QDir::toNativeSeparators(dir);
    if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(nativeDir.utf16()), &dirSecurity.attributes) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return fail("The connection offer folder could not be created.");
    // Refuse a junction or symbolic link planted in place of the folder.
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeDir.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return fail("The connection offer folder is not a plain folder.");
    const auto payload = QJsonDocument(QJsonObject{
        {"format", Format}, {"code", pending.code}, {"project", pending.project.left(120)},
        {"expiresUtcMs", pending.expiresUtcMs}, {"process", qint64(GetCurrentProcessId())}}).toJson(QJsonDocument::Compact);
    const auto temporary = QDir::toNativeSeparators(
        dir + QString("/.pending-%1-%2.tmp").arg(GetCurrentProcessId()).arg(GetTickCount64()));
    const HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()), GENERIC_WRITE, 0,
                                    &fileSecurity.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return fail("The connection offer could not be written.");
    DWORD written = 0;
    const bool ok = WriteFile(file, payload.constData(), DWORD(payload.size()), &written, nullptr) &&
                    written == DWORD(payload.size()) && FlushFileBuffers(file);
    CloseHandle(file);
    const auto target = QDir::toNativeSeparators(filePath());
    if (!ok || !MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()));
        return fail("The connection offer could not be written.");
    }
    return true;
}

std::optional<Pending> read(QString *error) {
    auto fail = [&](const QString &message) -> std::optional<Pending> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const auto path = filePath();
    QFile file(path);
    if (path.isEmpty() || !file.exists())
        return fail("no-offer");
    if (QFileInfo(path).isSymLink() || file.size() > FileLimit || !file.open(QIODevice::ReadOnly))
        return fail("unreadable");
    const auto object = QJsonDocument::fromJson(file.read(FileLimit)).object();
    Pending pending{object["code"].toString(), object["project"].toString(),
                    qint64(object["expiresUtcMs"].toDouble()), quint32(object["process"].toDouble())};
    if (object["format"].toString() != Format || !validCode(pending.code))
        return fail("unreadable");
    return pending;
}

void withdraw(const QString &code) {
    if (!validCode(code))
        return;
    QString error;
    const auto current = read(&error);
    if (current && current->code == code)
        QFile::remove(filePath());
}
} // namespace nova::mcp::handoff
