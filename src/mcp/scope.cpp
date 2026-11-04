// SPDX-License-Identifier: GPL-3.0-or-later
#include "scope.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <map>
#include <fcntl.h>
#include <io.h>
#include <qt_windows.h>

namespace nova::mcp {
namespace {
bool ordinaryComponent(const QString &part) {
    static const QRegularExpression invalid(R"([<>:"\\|?*\x00-\x1f])");
    // Win32 device names, with or without an extension ("con.png"), and their
    // superscript-digit forms. Behind the \\?\ prefix Windows would create them as
    // ordinary files that Explorer, Java (Minecraft) and most tools cannot open.
    static const QRegularExpression device(R"(^(con|prn|aux|nul|conin\$|conout\$|(com|lpt)[0-9\x{00B9}\x{00B2}\x{00B3}])$)",
                                           QRegularExpression::CaseInsensitiveOption);
    const auto stem = part.section('.', 0, 0).trimmed();
    return !part.isEmpty() && !part.endsWith('.') && !part.endsWith(' ') &&
           !part.contains(invalid) && part.size() <= 255 && !device.match(stem).hasMatch();
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle &) = delete;
};
std::unique_ptr<Handle> checked(const QString &path, bool directory) {
    const auto native = QDir::toNativeSeparators(path);
    const auto extended = QStringLiteral("\\\\?\\") + native;
    auto handle = std::make_unique<Handle>(CreateFileW(
        reinterpret_cast<LPCWSTR>(extended.utf16()), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
        directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle->value == INVALID_HANDLE_VALUE || GetFileType(handle->value) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle->value, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1))
        return {};
    wchar_t finalPath[4096];
    const auto length = GetFinalPathNameByHandleW(handle->value, finalPath, DWORD(std::size(finalPath)),
                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= std::size(finalPath) ||
        QString::fromWCharArray(finalPath, length).compare(extended, Qt::CaseInsensitive) != 0)
        return {};
    return handle;
}
}

bool logicalAsset(const QString &path) {
    if (path == "pack.mcmeta" || path == "pack.png")
        return true;
    if (path.size() > 1024 || !path.startsWith("assets/"))
        return false;
    const auto parts = path.split('/');
    if (parts.size() < 4 || parts.size() > 33)
        return false;
    for (const auto &part : parts)
        if (!ordinaryComponent(part) || part.compare(".nova", Qt::CaseInsensitive) == 0)
            return false;
    return true;
}
struct Scope::Handles {
    QString root;
    QByteArray id;
    std::map<QString, std::unique_ptr<Handle>> directories;
    Handle *directory(const QString &path) {
        if (auto it = directories.find(path); it != directories.end())
            return it->second.get();
        if (directories.size() >= 8192)
            return nullptr;
        auto opened = checked(path, true);
        if (!opened)
            return nullptr;
        return directories.emplace(path, std::move(opened)).first->second.get();
    }
};
Scope::Scope(const QString &root, const QByteArray &expected) : handles(std::make_unique<Handles>()) {
    if (root.size() < 4 || root.size() > 2048 || !root[0].isLetter() || root.mid(1, 2) != ":/" ||
        QDir::cleanPath(root) != root)
        return;
    QString path = root.left(3);
    const auto drive = QDir::toNativeSeparators(path);
    if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16())) != DRIVE_FIXED)
        return;
    // Open from the drive down, keeping directory handles without delete sharing.
    // The drive root uses a trailing slash in Win32 and has no replaceable parent.
    Handle *last = nullptr;
    for (const auto &part : root.mid(3).split('/')) {
        if (!ordinaryComponent(part))
            return;
        if (!path.endsWith('/'))
            path += '/';
        path += part;
        last = handles->directory(path);
        if (!last)
            return;
    }
    FILE_ID_INFO id{};
    if (!last || !GetFileInformationByHandleEx(last->value, FileIdInfo, &id, sizeof(id)))
        return;
    QByteArray actual(reinterpret_cast<const char *>(&id), sizeof(id));
    if (!expected.isEmpty() && expected != actual)
        return;
    handles->root = root;
    handles->id = actual;
}
Scope::~Scope() = default;
QByteArray Scope::identity() const { return handles->id; }

bool Scope::vacant(const QString &relative) {
    if (handles->id.isEmpty() || !logicalAsset(relative))
        return false;
    auto parts = relative.split('/');
    const auto file = parts.takeLast();
    QString path = handles->root;
    for (const auto &part : parts) {
        path += '/' + part;
        if (!handles->directory(path))
            return false;
    }
    const auto native = QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(path + '/' + file);
    return GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16())) == INVALID_FILE_ATTRIBUTES &&
           GetLastError() == ERROR_FILE_NOT_FOUND;
}

std::unique_ptr<QIODevice> Scope::open(const QString &relative) {
    if (handles->id.isEmpty() || !logicalAsset(relative))
        return {};
    auto parts = relative.split('/');
    const auto file = parts.takeLast();
    QString path = handles->root;
    for (const auto &part : parts) {
        path += '/' + part;
        if (!handles->directory(path))
            return {};
    }
    auto handle = checked(path + '/' + file, false);
    if (!handle)
        return {};
    const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle->value), _O_RDONLY | _O_BINARY);
    if (fd == -1)
        return {};
    handle->value = INVALID_HANDLE_VALUE;
    auto device = std::make_unique<QFile>();
    if (!device->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        _close(fd);
        return {};
    }
    return device;
}
}
