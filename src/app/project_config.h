// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "revision.h"
#include <QJsonObject>
#include <optional>
namespace nova {
// The vanilla baseline a project is bound to (<project>/.nova/project.json, "baseline").
struct BaselineRef {
    QString edition = "java", version, identity, clientSha1, assetIndexId, assetIndexSha1;
    bool operator==(const BaselineRef &) const = default;
    QJsonObject json() const;
    // Fails for an unknown edition, a version outside gameVersions() or malformed hashes.
    static std::optional<BaselineRef> parse(const QJsonValue &value, QString *error = nullptr);
};
// The user's optional style brief for a pack (<project>/.nova/project.json, "styleNotes"). User content:
// shown to authorized agents as preferences, never as permissions. Edited only in Nova.
struct StyleNotes {
    static constexpr int ShortLimit = 200, LongLimit = 4000;
    QString theme, audience, palette, preserve, notes;
    bool empty() const;
    QJsonObject json() const;
    static StyleNotes fromJson(const QJsonValue &value); // trims and bounds every field
};
// <project>/.nova/project.json. Missing file: legacy project, no baseline and no diagnostic.
// Unknown schema or invalid content: no baseline, a diagnostic, and save() refuses to rewrite.
struct ProjectConfig {
    std::optional<BaselineRef> baseline;
    QJsonObject json;   // the complete file as read; unknown keys survive save()
    Revision revision;  // of the bytes parsed; Revision{} when the file is missing
    QString diagnostic;
    static QString path(const QString &root);
    static ProjectConfig load(const QString &root);
    StyleNotes styleNotes() const { return StyleNotes::fromJson(json.value("styleNotes")); }
    void setStyleNotes(const StyleNotes &notes) {
        if (notes.empty())
            json.remove("styleNotes");
        else
            json["styleNotes"] = notes.json();
    }
    // atomicWrite with the read revision; creates .nova only inside the project, never through links.
    bool save(const QString &root, QString *error = nullptr);
};
// Creates the missing parent folders of `relativeFile` (assets/<ns>/...) inside the project after
// validating every component; refuses traversal, links/junctions/reparse points and file ancestors.
bool ensureAssetDirectory(const QString &root, const QString &relativeFile, QString *error = nullptr);
// Same rules for any project-relative file (used for .nova/... journal and sidecar folders).
bool ensureProjectDirectory(const QString &root, const QString &relativeFile, QString *error = nullptr);
// Component rules shared by the helpers above, without touching the disk.
bool validProjectPath(const QString &relative);
// True for an existing path that is a symlink, junction or other reparse point.
bool reparsePoint(const QString &path);
} // namespace nova
