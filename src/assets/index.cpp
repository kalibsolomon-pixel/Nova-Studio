// SPDX-License-Identifier: GPL-3.0-or-later
#include "index.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTimeZone>
#include <algorithm>
namespace nova {
QString assetCategory(const QString &relative) {
    auto p = relative.toLower();
    if (p.endsWith(".mcmeta") && p != "pack.mcmeta")
        return "Animations";
    for (auto pair : {std::pair{"/textures/block/", "Blocks"},
                      {"/textures/item/", "Items"},
                      {"/textures/entity/", "Entities"},
                      {"/textures/gui/", "GUI"},
                      {"/textures/particle/", "Particles"},
                      {"/textures/environment/", "Environment"},
                      {"/models/", "Models"},
                      {"/blockstates/", "Blockstates"},
                      {"/font/", "Fonts"},
                      {"/sounds/", "Sounds"}})
        if (p.contains(pair.first))
            return pair.second;
    if (p.endsWith(".ogg") || p.endsWith("/sounds.json"))
        return "Sounds";
    return "Other assets";
}
static AssetIdentity fromInfo(const QString &root, const QString &relative, const QFileInfo &info) {
    AssetIdentity a;
    a.projectRoot = root;
    a.relativePath = relative;
    a.filesystemPath = root + '/' + relative;
    a.state = info.exists() ? AssetIdentity::FileState::Present : AssetIdentity::FileState::Missing;
    auto parts = relative.split('/');
    if (parts.size() > 2 && parts[0] == "assets")
        a.nameSpace = parts[1];
    a.category = assetCategory(relative);
    a.size = info.size();
    a.modified = info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch();
    return a;
}
AssetIdentity AssetIdentity::fromPath(const QString &root, const QString &relative) {
    return fromInfo(root, relative, QFileInfo(QDir(root).filePath(relative)));
}
Index Index::scan(const QString &root, const std::shared_ptr<std::atomic_bool> &cancel) {
    Index index;
    if (!QFileInfo(root).isDir() || !QFileInfo(root).isReadable()) {
        index.error = "Project directory is missing or inaccessible";
        return index;
    }
    QDir base(root);
    index.directories.insert("", {});
    QStringList todo{""};
    while (!todo.isEmpty()) {
        if (cancel && cancel->load()) {
            index.error = "Cancelled";
            return index;
        }
        auto directory = todo.takeLast();
        auto entries =
            QDir(base.filePath(directory))
                .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                               QDir::Name);
        for (const auto &info : entries) {
            if (info.fileName().compare(".nova", Qt::CaseInsensitive) == 0)
                continue;
            if (cancel && cancel->load()) {
                index.error = "Cancelled";
                return index;
            }
            auto child = directory.isEmpty() ? info.fileName() : directory + '/' + info.fileName();
            if (child.count('/') > 32)
                continue;
            if (info.isDir()) {
                index.directories[directory].append(child);
                index.directories.insert(child, {});
                todo.append(child);
            } else if (info.isFile()) {
                index.directories[directory].append(child);
                index.files.append(child);
                index.records.insert(child, fromInfo(root, child, info));
            }
        }
    }
    for (auto it = index.records.begin(); it != index.records.end(); ++it)
        if (index.records.contains(it.key() + ".mcmeta"))
            it->metadataReference = it.key() + ".mcmeta";
    index.files.sort();
    return index;
}
Index Index::refresh(const QString &root, const Index &previous, const QStringList &paths,
                     const std::shared_ptr<std::atomic_bool> &cancel) {
    Index result = previous;
    if (!QFileInfo(root).isDir()) {
        result.error = "Project directory moved or disconnected";
        return result;
    }
    QStringList scopes = paths;
    scopes.removeDuplicates();
    scopes.sort();
    QStringList retained;
    for (auto path : scopes) {
        path = QDir::cleanPath(path);
        if (path == "." || path == ".." || path.startsWith("../") || QDir::isAbsolutePath(path))
            return scan(root, cancel);
        bool covered = false;
        for (auto parent : retained)
            if (path.startsWith(parent + '/')) {
                covered = true;
                break;
            }
        if (!covered)
            retained.append(path);
    }
    for (auto path : retained) {
        if (path.compare(".nova", Qt::CaseInsensitive) == 0 ||
            path.startsWith(".nova/", Qt::CaseInsensitive))
            continue;
        if (cancel && cancel->load()) {
            result.error = "Cancelled";
            return result;
        }
        QString prefix = path + '/';
        // Remove a subtree in one stable pass. removeAll() for every matching
        // file made a folder notification quadratic in the pack's file count.
        result.files.removeIf([&](const QString &file) {
            if (file == path || file.startsWith(prefix)) {
                result.records.remove(file);
                return true;
            }
            return false;
        });
        auto oldDirectories = result.directories.keys();
        for (const auto &dir : oldDirectories)
            if (dir == path || dir.startsWith(prefix))
                result.directories.remove(dir);
        auto parent = QFileInfo(path).path();
        if (parent == ".")
            parent.clear();
        result.directories[parent].removeAll(path);
        QFileInfo info(QDir(root).filePath(path));
        if (info.isSymLink() || !info.exists())
            continue;
        if (info.isDir()) {
            auto subtree = scan(info.absoluteFilePath(), cancel);
            if (!subtree.error.isEmpty()) {
                result.error = subtree.error;
                return result;
            }
            result.directories[parent].append(path);
            for (auto it = subtree.directories.cbegin(); it != subtree.directories.cend(); ++it) {
                QStringList children;
                for (auto child : it.value())
                    children.append(prefix + child);
                result.directories.insert(it.key().isEmpty() ? path : prefix + it.key(), children);
            }
            for (auto file : subtree.files) {
                auto key = prefix + file;
                result.files.append(key);
                // scan() already queried each file. Rebase its identity instead
                // of issuing another round of filesystem metadata queries.
                auto record = subtree.records.value(file);
                record.projectRoot = root;
                record.relativePath = key;
                record.filesystemPath = root + '/' + key;
                const auto parts = key.split('/');
                record.nameSpace = parts.size() > 2 && parts[0] == "assets" ? parts[1] : QString();
                record.category = assetCategory(key);
                if (!record.metadataReference.isEmpty()) record.metadataReference.prepend(prefix);
                result.records.insert(key, std::move(record));
            }
        } else if (info.isFile()) {
            result.files.append(path);
            result.directories[parent].append(path);
            result.records.insert(path, AssetIdentity::fromPath(root, path));
        }
    }
    result.files.sort();
    // A file-only refresh must maintain the same metadata link as a full scan,
    // including when the notification adds/removes just the .mcmeta sidecar.
    for (const auto &path : retained) {
        auto updateMetadata = [&](const QString &file) {
            auto record = result.records.find(file);
            if (record != result.records.end())
                record->metadataReference = result.records.contains(file + ".mcmeta") ? file + ".mcmeta" : QString();
        };
        updateMetadata(path);
        if (path.endsWith(".mcmeta")) updateMetadata(path.chopped(7));
    }
    for (auto &children : result.directories) {
        children.removeDuplicates();
        children.sort();
    }
    return result;
}
QStringList Navigation::entries(const Index &index) const {
    QStringList result;
    if (query.isEmpty())
        result = index.directories.value(directory);
    else
        for (auto p : index.files)
            if ((directory.isEmpty() || p.startsWith(directory + '/')) &&
                p.contains(query, Qt::CaseInsensitive))
                result.append(p);
    std::sort(result.begin(), result.end(), [&](const QString &a, const QString &b) {
        bool ad = index.directories.contains(a), bd = index.directories.contains(b);
        return ad != bd ? ad : a < b;
    });
    if (reverse)
        std::reverse(result.begin(), result.end());
    return result;
}
} // namespace nova
