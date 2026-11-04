// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "validation.h"
#include <QComboBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QWidget>
#include <QProgressBar>
namespace nova {
struct ResourceContext {
    QString root, asset, folder;
    Index index;
    std::shared_ptr<mc::Library> library;
    QMap<QString,QJsonObject> documents;
};
class ResourceTools : public QWidget {
    QTreeWidget *problems;
    QComboBox *scope;
    QLabel *status, *detail;
    QPushButton *run, *cancelButton, *repair, *exportText, *exportJson;
    QProgressBar *progress;
    mc::ValidationEngine engine;
    mc::ValidationReport report;
    std::shared_ptr<std::atomic_bool> cancel;
    QFutureWatcher<QPair<mc::ValidationEngine, mc::ValidationReport>> *watcher = nullptr;
    QString reportRoot;
    quint64 population = 0;
    void populate(int offset = 0);
    void select();
    void fix();
    void exportReport(bool json);

  public:
    std::function<ResourceContext()> context;
    std::function<void(const QString &)> navigate;
    std::function<void(const QStringList &)> changed;
    explicit ResourceTools(QWidget *parent = nullptr);
    ~ResourceTools() override;
    void validate();
};
void createAssetDialog(const ResourceContext &, QWidget *parent,
                       const std::function<void(const QStringList &)> &created);
} // namespace nova
