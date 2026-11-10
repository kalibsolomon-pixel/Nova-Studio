// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include "build_identity.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>
#include <cstdio>
#include <mutex>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nova::diagnostics {
namespace {
constexpr qint64 Limit = 1024 * 1024;
std::mutex guard;
QString currentLog;
#ifdef Q_OS_WIN
HANDLE crashFile = INVALID_HANDLE_VALUE;
LONG crashWriting = 0;
char crashIdentity[256]{};
DWORD crashIdentityLength = 0;
LONG WINAPI nativeException(EXCEPTION_POINTERS *info) {
    if (InterlockedExchange(&crashWriting, 1) || crashFile == INVALID_HANDLE_VALUE || !info ||
        !info->ExceptionRecord)
        return EXCEPTION_EXECUTE_HANDLER;
    // No Qt, heap allocation, project access or attempted save in a damaged process.
    char line[384]{};
    DWORD length = 0;
    for (DWORD i = 0; i < crashIdentityLength; ++i) line[length++] = crashIdentity[i];
    auto append = [&](const char *value) { while (*value && length < sizeof(line)-1) line[length++] = *value++; };
    auto hex = [&](quintptr value, int digits) {
        const char *alphabet = "0123456789ABCDEF";
        for (int i = digits-1; i >= 0; --i) line[length++] = alphabet[(value >> (i*4)) & 15];
    };
    append(" native exception=0x"); hex(info->ExceptionRecord->ExceptionCode, 8);
    append(" address=0x"); hex(reinterpret_cast<quintptr>(info->ExceptionRecord->ExceptionAddress), int(sizeof(void*)*2));
    append("\r\n");
    LARGE_INTEGER size{};
    DWORD written = 0;
    if (GetFileSizeEx(crashFile, &size) && size.QuadPart + length <= 64*1024) {
        WriteFile(crashFile, line, length, &written, nullptr);
        FlushFileBuffers(crashFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
void prepareNativeContext(const QString &directory) {
    if (crashFile != INVALID_HANDLE_VALUE) CloseHandle(crashFile);
    const auto path = QDir::toNativeSeparators(directory + "/native-crash-context.txt");
    crashFile = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto identity = QByteArray("Nova Studio ") + AppVersion + " source=" + BuildRevision;
    crashIdentityLength = DWORD(qMin(identity.size(), qsizetype(sizeof(crashIdentity)-1)));
    for (DWORD i = 0; i < crashIdentityLength; ++i) crashIdentity[i] = identity[int(i)];
    InterlockedExchange(&crashWriting, 0);
    SetUnhandledExceptionFilter(nativeException);
}
#endif
void message(QtMsgType type, const QMessageLogContext &, const QString &text) {
    const auto line = (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) +
                       " [" + QString::number(type) + "] " + text.left(8192) + '\n').toUtf8();
    {
        std::lock_guard lock(guard);
        QFile file(currentLog);
        if (!currentLog.isEmpty() && file.size() + line.size() <= Limit &&
            file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            file.write(line);
            file.flush();
        }
    }
    // Preserve stderr for developer/test harnesses and fatal diagnostics.
    std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
}
}
bool start(const QString &directory) {
    {
        std::lock_guard lock(guard);
        if (!QDir().mkpath(directory)) return false;
        currentLog = QDir(directory).absoluteFilePath("studio.log");
        QFile file(currentLog);
        if (file.size() >= Limit / 2) {
            const auto previous = QDir(directory).absoluteFilePath("studio-previous.log");
            if (QFile::exists(previous) && !QFile::remove(previous)) return false;
            if (!file.rename(previous)) return false;
        }
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
#ifdef Q_OS_WIN
        prepareNativeContext(directory);
#endif
    }
    qInstallMessageHandler(message);
    qInfo().noquote() << "Nova Studio" << AppVersion << BuildRevision << "Qt" << qVersion()
                      << QSysInfo::currentCpuArchitecture() << QSysInfo::prettyProductName();
    return true;
}
QString logPath() {
    std::lock_guard lock(guard);
    return currentLog;
}
QJsonObject report() {
    QFile file(logPath());
    QByteArray tail;
    if (!file.fileName().isEmpty() && file.open(QIODevice::ReadOnly)) {
        file.seek(qMax(qint64(0), file.size() - 64 * 1024));
        tail = file.read(64 * 1024);
    }
    QFile crash(QFileInfo(logPath()).absolutePath() + "/native-crash-context.txt");
    QByteArray context;
    if (!logPath().isEmpty() && crash.open(QIODevice::ReadOnly)) {
        crash.seek(qMax(qint64(0), crash.size() - 8192));
        context = crash.read(8192);
    }
    return {{"format", "nova-local-diagnostics"}, {"schema", 1},
            {"application_version", AppVersion}, {"source_revision", BuildRevision},
            {"qt_version", qVersion()}, {"architecture", QSysInfo::currentCpuArchitecture()},
            {"operating_system", QSysInfo::prettyProductName()},
            {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"log_tail", QString::fromUtf8(tail)}, {"native_crash_context", QString::fromUtf8(context)},
            {"automatic_upload", false}};
}
bool exportReport(const QString &destination, QString *error) {
    QSaveFile file(destination);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(report()).toJson();
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    if (!ok && error) *error = file.errorString();
    return ok;
}
}
