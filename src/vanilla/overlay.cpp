// SPDX-License-Identifier: GPL-3.0-or-later
#include "overlay.h"
#include <QSet>
#include <algorithm>

namespace nova::vanilla {
namespace {
// Appends `path` to its parent folder and creates missing ancestors up to "". Stops at the
// first ancestor that already existed (its own chain is complete).
void addAncestors(QMap<QString, QStringList> &directories, const QString &path,
                  QSet<QString> *touched) {
    QString child = path;
    for (;;) {
        const auto slash = child.lastIndexOf('/');
        const QString parent = slash < 0 ? QString() : child.left(slash);
        auto it = directories.find(parent);
        const bool created = it == directories.end();
        if (created)
            it = directories.insert(parent, {});
        it->append(child);
        if (touched)
            touched->insert(parent);
        if (!created || parent.isEmpty())
            return;
        child = parent;
    }
}
} // namespace

Index baselineIndex(const BaselineEntry &entry) {
    Index index;
    const BaselineCache cache(entry.root);
    const auto modified = entry.created.toMSecsSinceEpoch();
    index.directories.insert("", {});
    index.files.reserve(entry.files.size());
    for (auto it = entry.files.cbegin(); it != entry.files.cend(); ++it) {
        const auto &relative = it.key();
        AssetIdentity a;
        a.origin = AssetIdentity::Origin::Baseline;
        a.state = AssetIdentity::FileState::Present;
        a.projectRoot = cache.root();
        a.relativePath = relative;
        a.filesystemPath = cache.objectPath(it->sha256, relative);
        a.nameSpace = "minecraft";
        a.category = assetCategory(relative);
        a.size = it->size;
        a.modified = modified;
        if (relative.endsWith(".png") && entry.files.contains(relative + ".mcmeta"))
            a.metadataReference = relative + ".mcmeta";
        index.records.insert(index.records.cend(), relative, std::move(a)); // sorted input
        index.files.append(relative);
        addAncestors(index.directories, relative, nullptr);
    }
    for (auto &children : index.directories)
        children.sort();
    return index;
}

Index mergeBaseline(const Index &project, const Index &baseline) {
    Index result = project;
    for (auto &record : result.records)
        record.shadowsBaseline = false; // set below for every path the baseline also has
    QStringList added;
    QSet<QString> touched;
    for (auto it = baseline.records.cbegin(); it != baseline.records.cend(); ++it) {
        const auto &path = it.key();
        auto own = result.records.find(path);
        if (own != result.records.end()) {
            own->shadowsBaseline = true; // the project file overrides vanilla
            continue;
        }
        if (result.directories.contains(path))
            continue; // a project folder has this name
        bool blocked = false;
        for (auto slash = path.lastIndexOf('/'); slash > 0; slash = path.lastIndexOf('/', slash - 1)) {
            const auto ancestor = path.left(slash);
            if (result.directories.contains(ancestor))
                break; // known folder: its ancestors are folders too
            if (result.records.contains(ancestor)) {
                blocked = true; // a project file occupies a folder name
                break;
            }
        }
        if (blocked)
            continue;
        addAncestors(result.directories, path, &touched);
        result.records.insert(path, it.value());
        added.append(path);
    }
    if (!added.isEmpty()) {
        QStringList files;
        files.reserve(result.files.size() + added.size());
        if (!std::is_sorted(result.files.cbegin(), result.files.cend()))
            result.files.sort();
        std::merge(result.files.cbegin(), result.files.cend(), added.cbegin(), added.cend(),
                   std::back_inserter(files));
        result.files = std::move(files);
    }
    for (const auto &folder : touched) {
        auto &children = result.directories[folder];
        children.sort();
        children.removeDuplicates();
    }
    return result;
}

} // namespace nova::vanilla
