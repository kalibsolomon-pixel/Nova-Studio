// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor.h"
#include <QDir>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QWinEventNotifier>
#include <windows.h>
#endif
namespace nova {
struct DirectoryMonitor::Native {
#ifdef Q_OS_WIN
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    QByteArray buffer = QByteArray(64 * 1024, 0);
    QWinEventNotifier *notifier = nullptr;
    bool arm() {
        ResetEvent(overlapped.hEvent);
        return ReadDirectoryChangesW(handle, buffer.data(), DWORD(buffer.size()), TRUE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                     nullptr, &overlapped, nullptr) != 0;
    }
#endif
};
DirectoryMonitor::DirectoryMonitor(QObject *parent)
    : QObject(parent), native(std::make_unique<Native>()) {}
DirectoryMonitor::~DirectoryMonitor() { stop(); }
void DirectoryMonitor::stop() {
    watchedRoot.clear();
#ifdef Q_OS_WIN
    if (native->notifier) {
        native->notifier->setEnabled(false);
        delete native->notifier;
        native->notifier = nullptr;
    }
    if (native->handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(native->handle, &native->overlapped);
        DWORD bytes;
        GetOverlappedResult(native->handle, &native->overlapped, &bytes, TRUE);
        CloseHandle(native->handle);
        native->handle = INVALID_HANDLE_VALUE;
    }
    if (native->overlapped.hEvent) {
        CloseHandle(native->overlapped.hEvent);
        native->overlapped = {};
    }
#endif
}
bool DirectoryMonitor::active() const {
#ifdef Q_OS_WIN
    if (native->handle == INVALID_HANDLE_VALUE || !native->notifier ||
        !native->notifier->isEnabled() || watchedRoot.isEmpty())
        return false;
    const auto path = QDir::toNativeSeparators(watchedRoot);
    auto current = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (current == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION original{}, now{};
    const bool same = GetFileInformationByHandle(native->handle, &original) &&
                      GetFileInformationByHandle(current, &now) &&
                      (original.nFileIndexHigh || original.nFileIndexLow) &&
                      original.dwVolumeSerialNumber == now.dwVolumeSerialNumber &&
                      original.nFileIndexHigh == now.nFileIndexHigh &&
                      original.nFileIndexLow == now.nFileIndexLow;
    CloseHandle(current);
    return same;
#else
    return false;
#endif
}
bool DirectoryMonitor::watch(const QString &root) {
    stop();
#ifdef Q_OS_WIN
    auto path = QDir::toNativeSeparators(root);
    native->handle =
        CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (native->handle == INVALID_HANDLE_VALUE)
        return false;
    native->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!native->overlapped.hEvent || !native->arm()) {
        stop();
        return false;
    }
    native->notifier = new QWinEventNotifier(native->overlapped.hEvent, this);
    watchedRoot = root;
    connect(native->notifier, &QWinEventNotifier::activated, this, [this] {
        DWORD bytes = 0;
        bool ok = GetOverlappedResult(native->handle, &native->overlapped, &bytes, FALSE) != 0;
        QStringList paths;
        if (ok && bytes) {
            DWORD offset = 0;
            while (offset < bytes) {
                auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(
                    native->buffer.constData() + offset);
                paths.append(QDir::fromNativeSeparators(QString::fromWCharArray(
                    info->FileName, int(info->FileNameLength / sizeof(wchar_t)))));
                if (!info->NextEntryOffset)
                    break;
                offset += info->NextEntryOffset;
            }
        }
        bool armed = native->arm();
        if (!armed)
            native->notifier->setEnabled(false);
        if (!ok || !bytes || !armed)
            emit uncertain();
        else
            emit changed(paths);
    });
    return true;
#else
    Q_UNUSED(root);
    return false;
#endif
}
} // namespace nova
