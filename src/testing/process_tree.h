// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Windows process-tree helpers for the Fabric runtime. Processes are
// identified by id plus creation time, so a recycled id is never mistaken for
// Nova's Gradle or Minecraft process. Command lines are never read.
#include <QList>
#include <QString>

namespace nova::testing {

struct ProcessIdentity {
    qint64 pid = 0;
    quint64 created = 0; // FILETIME as 100 ns ticks; 0 = unknown
    bool valid() const { return pid > 0 && created > 0; }
};

ProcessIdentity identifyProcess(qint64 pid);
bool processAlive(const ProcessIdentity &process);
QList<ProcessIdentity> descendants(const ProcessIdentity &root);
// Terminates the tree, deepest processes first. Returns how many were ended.
int terminateTree(const ProcessIdentity &root);
// Posts WM_CLOSE to the visible top-level windows of the tree (the same
// request as clicking the window's close button). Returns windows asked.
int requestTreeWindowsClose(const ProcessIdentity &root);
int treeVisibleWindows(const ProcessIdentity &root);

} // namespace nova::testing
