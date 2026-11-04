// SPDX-License-Identifier: GPL-3.0-or-later
#include "revision.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSaveFile>
#include <algorithm>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <qt_windows.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#endif
namespace nova {
namespace {
thread_local WriteDiagnostics diagnostics;
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
#ifdef Q_OS_WIN
// Rename retry bounds. A thumbnail or preview read of a 4096 px PNG finishes well inside
// this window; an external program holding the file longer gets a clear failure instead.
constexpr int MaxAttempts = 16, MaxDelayMs = 64, BudgetMs = 750;

std::wstring nativePath(const QString &path) {
    const auto absolute =
        QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (absolute.startsWith(QStringLiteral("\\\\?\\")))
        return absolute.toStdWString();
    if (absolute.startsWith(QStringLiteral("\\\\")))
        return (QStringLiteral("\\\\?\\UNC\\") + absolute.mid(2)).toStdWString();
    return (QStringLiteral("\\\\?\\") + absolute).toStdWString();
}
QString systemMessage(DWORD code) {
    wchar_t *buffer = nullptr;
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    auto text = length ? QString::fromWCharArray(buffer, int(length)).trimmed()
                       : QStringLiteral("Windows error %1").arg(code);
    if (buffer)
        LocalFree(buffer);
    return text;
}
enum class Holder { None, SharingHandle, Denied, Missing };
// Asks for delete access while sharing everything. Windows refuses it with a sharing
// violation only when another open handle withholds FILE_SHARE_DELETE, which is exactly
// the case where a replacing rename is refused but will succeed once that handle closes.
Holder holder(const std::wstring &path) {
    const HANDLE handle = CreateFileW(path.c_str(), DELETE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return Holder::None;
    }
    switch (GetLastError()) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return Holder::SharingHandle;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return Holder::Missing;
    default:
        return Holder::Denied;
    }
}
// FILE_RENAME_INFO with the Windows 10 flags field, declared locally so the build does
// not depend on the SDK's _WIN32_WINNT selection. The layout matches the SDK union.
struct RenameInfoEx {
    DWORD Flags;
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
};
constexpr DWORD RenameReplaceIfExists = 0x1, RenamePosixSemantics = 0x2;
constexpr auto RenameInfoExClass = FILE_INFO_BY_HANDLE_CLASS(22); // FileRenameInfoEx
enum class Rename { Done, Failed, Unsupported };
// Renames the open temporary file over the destination with POSIX semantics: the name is
// replaced even while other handles that share delete still read the old file.
Rename posixRename(HANDLE handle, const std::wstring &target, bool replace, DWORD *code) {
    const DWORD nameBytes = DWORD(target.size() * sizeof(wchar_t));
    std::vector<char> storage(offsetof(RenameInfoEx, FileName) + nameBytes + sizeof(wchar_t), 0);
    auto *info = reinterpret_cast<RenameInfoEx *>(storage.data());
    info->Flags = RenamePosixSemantics | (replace ? RenameReplaceIfExists : 0);
    info->RootDirectory = nullptr;
    info->FileNameLength = nameBytes;
    memcpy(info->FileName, target.c_str(), nameBytes);
    if (SetFileInformationByHandle(handle, RenameInfoExClass, info, DWORD(storage.size())))
        return Rename::Done;
    *code = GetLastError();
    // FAT, exFAT, some network redirectors and older Windows do not implement the class.
    if (*code == ERROR_INVALID_PARAMETER || *code == ERROR_NOT_SUPPORTED ||
        *code == ERROR_INVALID_FUNCTION || *code == ERROR_INVALID_LEVEL)
        return Rename::Unsupported;
    return Rename::Failed;
}
// Waits about `ms` milliseconds, randomised by +/-50 %. Plain Sleep() wakes on the
// 15.6 ms system tick, so a retry can stay phase-locked with another program that also
// polls on that tick and keep hitting it mid-read. A high-resolution waitable timer
// (Windows 10 1803+, per timer, no process-wide timer change) plus jitter avoids that.
void retryWait(int ms) {
    const auto micros = qint64(ms) * 1000 / 2 + QRandomGenerator::global()->bounded(qint64(ms) * 1000);
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */,
                                                       TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -micros * 10; // relative, 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer, DWORD(ms) * 2 + 50) == WAIT_OBJECT_0)
        return;
    Sleep(DWORD(std::max<qint64>(1, micros / 1000)));
}
std::wstring randomSuffix() {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::wstring suffix = L".";
    for (int i = 0; i < 6; ++i)
        suffix += wchar_t(alphabet[QRandomGenerator::global()->bounded(36)]);
    return suffix;
}
#endif

bool writeAndPublish(const QString &path, const Revision *expected,
                     const std::function<bool(QIODevice &)> &writer, QString *error,
                     const std::function<void()> &beforeCommit) {
    diagnostics = {};
    const QFileInfo info(path);
    if (info.isSymLink())
        return fail(error, "Symbolic-link destinations are not supported");
    if (expected && !expected->sameContent(Revision::read(path)))
        return fail(error, "External file conflict: disk contents changed or became inaccessible");
    if (info.exists() && !info.isFile())
        return fail(error, "The destination is not a regular file");
    if (!info.absoluteDir().exists())
        return fail(error, "The destination folder does not exist");
#ifdef Q_OS_WIN
    const auto target = nativePath(path);
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
        return fail(error, QStringLiteral("Existing file %1 is not writable")
                               .arg(QDir::toNativeSeparators(info.absoluteFilePath())));
    // 1. Prepare: write every byte to an exclusive temporary file in the same folder.
    std::wstring temp;
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD createError = ERROR_FILE_EXISTS;
    for (int i = 0; i < 16 && handle == INVALID_HANDLE_VALUE && createError == ERROR_FILE_EXISTS; ++i) {
        temp = target + randomSuffix();
        handle = CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE | SYNCHRONIZE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            createError = GetLastError();
    }
    if (handle == INVALID_HANDLE_VALUE)
        return fail(error, systemMessage(createError));
    // `handle` stays open, unshared, until publication; QFile writes through a duplicate.
    auto closeHandle = [&] {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(std::exchange(handle, INVALID_HANDLE_VALUE));
    };
    auto removeTemp = [&] {
        closeHandle();
        if (!DeleteFileW(temp.c_str()))
            diagnostics.temporaryRemoved = false;
    };
    HANDLE writeHandle = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &writeHandle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        removeTemp();
        return fail(error, "Could not open a temporary file for writing");
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(writeHandle), _O_WRONLY | _O_BINARY);
    if (fd == -1) {
        CloseHandle(writeHandle);
        removeTemp();
        return fail(error, "Could not open a temporary file for writing");
    }
    {
        QFile file;
        if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
            _close(fd);
            removeTemp();
            return fail(error, file.errorString());
        }
        bool written = writer(file) && file.flush() && file.error() == QFileDevice::NoError;
        QString writeError = file.errorString();
        if (written && !FlushFileBuffers(writeHandle)) {
            written = false;
            writeError = systemMessage(GetLastError());
        }
        file.close();
        if (!written) {
            removeTemp();
            return fail(error, writeError);
        }
    }
    if (beforeCommit)
        beforeCommit();
    // 2. Publish: one rename. Only this step repeats, so no caller callback runs twice.
    // A destination expected to be absent is never replaced, even if a file appears.
    const DWORD flags =
        MOVEFILE_WRITE_THROUGH | (expected && !expected->exists ? 0 : MOVEFILE_REPLACE_EXISTING);
    int waited = 0, unexplained = 0;
    bool posix = true;
    diagnostics.posixRename = true;
    for (int attempt = 0;; ++attempt) {
        const int delay = std::min(1 << std::min(attempt, 6), MaxDelayMs);
        const bool canWait = attempt + 1 < MaxAttempts && waited + delay <= BudgetMs;
        if (QFileInfo(path).isSymLink()) {
            removeTemp();
            return fail(error, "External file conflict: destination changed while preparing the save");
        }
        if (expected) {
            const auto now = Revision::read(path);
            // An exclusive reader makes the destination briefly unreadable; that is
            // contention, not a change. Different bytes are always a conflict.
            if (now.exists && !now.readable && expected->exists && canWait) {
                ++diagnostics.sharingRetries;
                retryWait(delay);
                waited += delay;
                continue;
            }
            if (!expected->sameContent(now)) {
                removeTemp();
                return fail(error, "External file conflict: destination changed while preparing the save");
            }
        }
        diagnostics.publishAttempts += 1;
        DWORD code = 0;
        const bool replace = flags & MOVEFILE_REPLACE_EXISTING;
        auto result = posix ? posixRename(handle, target, replace, &code) : Rename::Unsupported;
        if (result == Rename::Unsupported) {
            // The legacy rename needs the temporary file closed. It cannot replace a file
            // that any handle has open, so it relies on the retry below.
            posix = false;
            diagnostics.posixRename = false;
            closeHandle();
            result = MoveFileExW(temp.c_str(), target.c_str(), flags) ? Rename::Done : Rename::Failed;
            if (result == Rename::Failed)
                code = GetLastError();
        }
        if (result == Rename::Done) {
            closeHandle();
            diagnostics.systemError = 0;
            return true;
        }
        diagnostics.systemError = code;
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            removeTemp();
            return fail(error, "External file conflict: a file appeared at the destination while saving");
        }
        bool retry = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
        if (code == ERROR_ACCESS_DENIED) {
            // Windows reports a replace onto an open file as access denied. Retry only
            // when a sharing handle is the cause; a permission refusal fails at once.
            // Our own temporary handle is unshared, so probe the source only once closed.
            const auto destination = holder(target);
            const auto source = handle == INVALID_HANDLE_VALUE ? holder(temp) : Holder::None;
            if (destination == Holder::SharingHandle || source == Holder::SharingHandle)
                retry = true;
            else if (destination != Holder::Denied && source == Holder::None && unexplained < 2) {
                // The holder closed between the rename and the probe.
                retry = true;
                ++unexplained;
            }
        }
        if (!retry || !canWait) {
            diagnostics.retriesExhausted = retry;
            removeTemp();
            auto message = systemMessage(code);
            if (retry)
                message = QStringLiteral("The file is in use by another program and was not replaced "
                                         "after %1 attempts: %2")
                              .arg(diagnostics.publishAttempts)
                              .arg(message);
            return fail(error, message);
        }
        ++diagnostics.sharingRetries;
        retryWait(delay);
        waited += delay;
    }
#else
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, file.errorString());
    if (!writer(file))
        return fail(error, file.errorString());
    if (beforeCommit)
        beforeCommit();
    if (QFileInfo(path).isSymLink() || (expected && !expected->sameContent(Revision::read(path)))) {
        file.cancelWriting();
        return fail(error, "External file conflict: destination changed while preparing the save");
    }
    diagnostics.publishAttempts = 1;
    if (!file.commit())
        return fail(error, file.errorString());
    return true;
#endif
}
} // namespace

QString pathIdentity(const QString &path) {
    QFileInfo info(path);
    auto result = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
    return QDir::cleanPath(result).toCaseFolded();
}
bool openSharedRead(const QString &path, QFile &file) {
#ifdef Q_OS_WIN
    const auto name = nativePath(path);
    const HANDLE h = CreateFileW(name.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
    if (fd == -1) {
        CloseHandle(h);
        return false;
    }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        _close(fd);
        return false;
    }
    return true;
#else
    file.setFileName(path);
    return file.open(QIODevice::ReadOnly);
#endif
}
Revision Revision::read(const QString &path, qint64 byteLimit) {
    QFileInfo info(path);
    Revision r;
    r.exists = info.exists();
    if (!r.exists)
        return r;
    r.size = info.size();
    r.modified = info.lastModified().toMSecsSinceEpoch();
    if (r.size > byteLimit) {
        r.readable = false;
        return r;
    }
    QFile file;
    if (!info.isFile() || !openSharedRead(path, file)) {
        r.readable = false;
        return r;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    r.readable = hash.addData(&file);
    r.hash = hash.result();
    return r;
}
bool Revision::sameContent(const Revision &r) const {
    return readable && r.readable && exists == r.exists &&
           (!exists || (size == r.size && hash == r.hash));
}
bool atomicWrite(const QString &path, const QByteArray &bytes, const Revision &expected,
                 QString *error, const std::function<void()> &beforeCommit) {
    return atomicWriteStream(
        path, expected, [&](QIODevice &file) { return file.write(bytes) == bytes.size(); }, error,
        beforeCommit);
}
bool atomicWriteStream(const QString &path, const Revision &expected,
                       const std::function<bool(QIODevice &)> &writer, QString *error,
                       const std::function<void()> &beforeCommit) {
    return writeAndPublish(path, &expected, writer, error, beforeCommit);
}
bool atomicReplace(const QString &path, const std::function<bool(QIODevice &)> &writer,
                   QString *error) {
    return writeAndPublish(path, nullptr, writer, error, {});
}
WriteDiagnostics lastWriteDiagnostics() { return diagnostics; }
QByteArray readFileBytes(const QString &path, qint64 byteLimit, QString *error) {
#ifdef Q_OS_WIN
    // One native read into a buffer sized from the open handle, then close.
    const auto name = nativePath(path);
    const HANDLE h = CreateFileW(name.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fail(error, QStringLiteral("Could not open %1 for reading: %2")
                        .arg(QDir::toNativeSeparators(path), systemMessage(GetLastError())));
        return {};
    }
    LARGE_INTEGER size{};
    QByteArray bytes;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart <= byteLimit;
    if (ok) {
        bytes.resize(qsizetype(size.QuadPart));
        qsizetype done = 0;
        while (ok && done < bytes.size()) {
            DWORD count = 0;
            const DWORD chunk = DWORD(std::min<qsizetype>(bytes.size() - done, 16 * 1024 * 1024));
            ok = ReadFile(h, bytes.data() + done, chunk, &count, nullptr) && count > 0;
            done += count;
        }
        bytes.resize(done);
    }
    CloseHandle(h);
    if (!ok) {
        fail(error, size.QuadPart > byteLimit ? QStringLiteral("File exceeds the read limit")
                                              : QStringLiteral("Could not read %1").arg(QDir::toNativeSeparators(path)));
        return {};
    }
    return bytes;
#else
    QFile file;
    if (!openSharedRead(path, file)) {
        fail(error, QStringLiteral("Could not open %1 for reading").arg(QDir::toNativeSeparators(path)));
        return {};
    }
    if (file.size() > byteLimit) {
        fail(error, "File exceeds the read limit");
        return {};
    }
    auto bytes = file.read(byteLimit + 1);
    if (bytes.size() > byteLimit) {
        fail(error, "File exceeds the read limit");
        return {};
    }
    return bytes;
#endif
}
} // namespace nova
