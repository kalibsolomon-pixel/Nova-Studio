// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QStringList>
#include <memory>
namespace nova {
// One recursive Windows directory handle, independent of directory count.
class DirectoryMonitor : public QObject {
    Q_OBJECT
    struct Native;
    std::unique_ptr<Native> native;
    QString watchedRoot;

  public:
    explicit DirectoryMonitor(QObject *parent = nullptr);
    ~DirectoryMonitor() override;
    bool watch(const QString &root);
    bool active() const;
    void stop();
    void simulateOverflow() { emit uncertain(); }
  signals:
    void changed(QStringList paths);
    void uncertain();
};
} // namespace nova
