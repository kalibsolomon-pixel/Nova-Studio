// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QMap>
#include <QStringList>
#include <atomic>
#include <memory>
namespace nova {
QString assetCategory(const QString &relativePath);
struct AssetIdentity {
    enum class FileState { Present, Missing };
    FileState state = FileState::Present;
    QString projectRoot, nameSpace, relativePath, category, filesystemPath, metadataReference;
    qint64 size = 0, modified = 0;
    static AssetIdentity fromPath(const QString &root, const QString &relative);
};
struct Index {
    QStringList files;
    QMap<QString, QStringList> directories;
    QMap<QString, AssetIdentity> records;
    QString error;
    static Index scan(const QString &root, const std::shared_ptr<std::atomic_bool> &cancel = {});
    static Index refresh(const QString &root, const Index &previous, const QStringList &paths,
                         const std::shared_ptr<std::atomic_bool> &cancel = {});
};
struct Navigation {
    QString directory, selected, query;
    bool grid = false, reverse = false;
    QStringList entries(const Index &index) const;
};
} // namespace nova
