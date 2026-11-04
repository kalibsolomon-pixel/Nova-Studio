// SPDX-License-Identifier: GPL-3.0-or-later
#include "process_tree.h"
#include <QHash>
#include <QSet>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>
#endif

namespace nova::testing {

#ifdef Q_OS_WIN
namespace {
quint64 ticks(const FILETIME &time) {
    return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
struct Entry {
    qint64 pid, parent;
};
QList<Entry> snapshot() {
    QList<Entry> entries;
    HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (handle == INVALID_HANDLE_VALUE)
        return entries;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(handle, &entry); ok; ok = Process32NextW(handle, &entry))
        entries.append({qint64(entry.th32ProcessID), qint64(entry.th32ParentProcessID)});
    CloseHandle(handle);
    return entries;
}
} // namespace

ProcessIdentity identifyProcess(qint64 pid) {
    ProcessIdentity identity{pid, 0};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process)
        return identity;
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user))
        identity.created = ticks(created);
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE)
        identity.created = 0;
    CloseHandle(process);
    return identity;
}

bool processAlive(const ProcessIdentity &process) {
    if (!process.valid())
        return false;
    const auto now = identifyProcess(process.pid);
    return now.created == process.created;
}

QList<ProcessIdentity> descendants(const ProcessIdentity &root) {
    QList<ProcessIdentity> found;
    if (!processAlive(root))
        return found;
    const auto entries = snapshot();
    QList<ProcessIdentity> frontier{root};
    QSet<qint64> seen{root.pid};
    while (!frontier.isEmpty()) {
        const auto parent = frontier.takeFirst();
        for (const auto &entry : entries) {
            if (entry.parent != parent.pid || seen.contains(entry.pid))
                continue;
            const auto child = identifyProcess(entry.pid);
            // A child is always created after its parent; an older process
            // with a matching parent id is an unrelated, recycled id.
            if (!child.valid() || child.created < parent.created)
                continue;
            seen.insert(entry.pid);
            found.append(child);
            frontier.append(child);
        }
    }
    return found;
}

int terminateTree(const ProcessIdentity &root) {
    auto tree = descendants(root);
    std::reverse(tree.begin(), tree.end()); // deepest first
    if (processAlive(root))
        tree.append(root);
    int ended = 0;
    for (const auto &process : tree) {
        HANDLE handle =
            OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                        DWORD(process.pid));
        if (!handle)
            continue;
        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(handle, &created, &exited, &kernel, &user) &&
            ticks(created) == process.created && TerminateProcess(handle, 1)) {
            WaitForSingleObject(handle, 5000);
            ++ended;
        }
        CloseHandle(handle);
    }
    return ended;
}

namespace {
struct WindowSearch {
    QSet<DWORD> pids;
    QList<HWND> windows;
};
BOOL CALLBACK collectWindow(HWND window, LPARAM data) {
    auto search = reinterpret_cast<WindowSearch *>(data);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (search->pids.contains(pid) && IsWindowVisible(window) && !GetWindow(window, GW_OWNER))
        search->windows.append(window);
    return TRUE;
}
QList<HWND> treeWindows(const ProcessIdentity &root) {
    WindowSearch search;
    if (processAlive(root))
        search.pids.insert(DWORD(root.pid));
    for (const auto &process : descendants(root))
        search.pids.insert(DWORD(process.pid));
    if (!search.pids.isEmpty())
        EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&search));
    return search.windows;
}
} // namespace

int requestTreeWindowsClose(const ProcessIdentity &root) {
    const auto windows = treeWindows(root);
    for (auto window : windows)
        PostMessageW(window, WM_CLOSE, 0, 0);
    return int(windows.size());
}

int treeVisibleWindows(const ProcessIdentity &root) { return int(treeWindows(root).size()); }

#else
ProcessIdentity identifyProcess(qint64 pid) { return {pid, 0}; }
bool processAlive(const ProcessIdentity &) { return false; }
QList<ProcessIdentity> descendants(const ProcessIdentity &) { return {}; }
int terminateTree(const ProcessIdentity &) { return 0; }
int requestTreeWindowsClose(const ProcessIdentity &) { return 0; }
int treeVisibleWindows(const ProcessIdentity &) { return 0; }
#endif

} // namespace nova::testing
