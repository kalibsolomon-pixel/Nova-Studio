// SPDX-License-Identifier: GPL-3.0-or-later
#include "index.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
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
// The <namespace> of assets/<namespace>/<rest>; empty for anything else.
static QString namespaceOf(const QString &relative) {
    const QStringView view(relative);
    const auto prefix = QLatin1String("assets/");
    if (!view.startsWith(prefix))
        return {};
    const auto end = view.indexOf(u'/', prefix.size());
    return end < 0 ? QString() : view.mid(prefix.size(), end - prefix.size()).toString();
}
static AssetIdentity fromInfo(const QString &root, const QString &relative, const QFileInfo &info) {
    AssetIdentity a;
    a.projectRoot = root;
    a.relativePath = relative;
    a.filesystemPath = root + '/' + relative;
    a.state = info.exists() ? AssetIdentity::FileState::Present : AssetIdentity::FileState::Missing;
    a.nameSpace = namespaceOf(relative);
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
    // Files kept sorted by scan()/refresh(): a subtree is then one contiguous range found by
    // binary search. Anything else (a hand-built index) keeps the linear pass.
    const bool filesSorted = std::is_sorted(result.files.cbegin(), result.files.cend());
    QStringList added;
    QSet<QString> touched; // directory lists that need dedupe/sort again
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
        if (filesSorted) {
            auto first = std::lower_bound(result.files.begin(), result.files.end(), path);
            auto same = first;
            while (same != result.files.end() && *same == path) {
                result.records.remove(*same);
                ++same;
            }
            first = result.files.erase(first, same);
            // Everything starting with "path/" follows the position of "path/" itself.
            first = std::lower_bound(first, result.files.end(), prefix);
            auto last = first;
            while (last != result.files.end() && last->startsWith(prefix)) {
                result.records.remove(*last);
                ++last;
            }
            result.files.erase(first, last);
        } else
            result.files.removeIf([&](const QString &file) {
                if (file == path || file.startsWith(prefix)) {
                    result.records.remove(file);
                    return true;
                }
                return false;
            });
        result.directories.remove(path);
        for (auto dir = result.directories.lowerBound(prefix);
             dir != result.directories.end() && dir.key().startsWith(prefix);)
            dir = result.directories.erase(dir);
        auto parent = QFileInfo(path).path();
        if (parent == ".")
            parent.clear();
        result.directories[parent].removeAll(path);
        touched.insert(parent);
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
                const auto key = it.key().isEmpty() ? path : prefix + it.key();
                result.directories.insert(key, children);
                touched.insert(key);
            }
            for (const auto &file : subtree.files) {
                auto key = prefix + file;
                added.append(key);
                // scan() already queried each file. Rebase its identity instead
                // of issuing another round of filesystem metadata queries.
                auto record = subtree.records.value(file);
                record.projectRoot = root;
                record.relativePath = key;
                record.filesystemPath = root + '/' + key;
                record.nameSpace = namespaceOf(key);
                record.category = assetCategory(key);
                if (!record.metadataReference.isEmpty()) record.metadataReference.prepend(prefix);
                result.records.insert(key, std::move(record));
            }
        } else if (info.isFile()) {
            added.append(path);
            result.directories[parent].append(path);
            result.records.insert(path, AssetIdentity::fromPath(root, path));
        }
    }
    if (filesSorted) {
        // The surviving files are still in order: sort only what was added and merge.
        added.sort();
        const auto kept = result.files.size();
        result.files += added;
        std::inplace_merge(result.files.begin(), result.files.begin() + kept, result.files.end());
    } else {
        result.files += added;
        result.files.sort();
    }
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
    // Directory lists touched above are deduplicated and sorted again; every other list is
    // left alone when it is already strictly ascending (which makes both steps a no-op).
    for (auto it = result.directories.begin(); it != result.directories.end(); ++it) {
        auto &children = it.value();
        if (!touched.contains(it.key()) &&
            std::adjacent_find(children.cbegin(), children.cend(),
                               [](const QString &a, const QString &b) { return !(a < b); }) ==
                children.cend())
            continue;
        children.removeDuplicates();
        children.sort();
    }
    return result;
}
QStringList Navigation::entries(const Index &index) const {
    QStringList result;
    if (query.isEmpty())
        result = index.directories.value(directory);
    else {
        const QString prefix = directory + '/';
        for (const auto &p : index.files)
            if ((directory.isEmpty() || p.startsWith(prefix)) &&
                p.contains(query, Qt::CaseInsensitive))
                result.append(p);
    }
    // Look each entry up once, not on every comparison.
    struct Row {
        QString path;
        bool directory;
    };
    QVector<Row> rows;
    rows.reserve(result.size());
    for (auto &p : result) {
        const bool isDirectory = index.directories.contains(p);
        rows.append(Row{std::move(p), isDirectory});
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        return a.directory != b.directory ? a.directory : a.path < b.path;
    });
    result.clear();
    for (auto &row : rows)
        result.append(std::move(row.path));
    if (reverse)
        std::reverse(result.begin(), result.end());
    return result;
}
} // namespace nova
