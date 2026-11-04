// SPDX-License-Identifier: GPL-3.0-or-later
#include "project.h"
#include "animation_export.h"
#include "core.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <climits>
namespace nova {
QJsonValue PackFormat::json() const { return QJsonArray{major, minor}; }
QString PackFormat::text() const { return QString("%1.%2").arg(major).arg(minor); }
std::optional<PackFormat> PackFormat::parse(QJsonValue v, bool maximum) {
    auto integer = [](QJsonValue n) {
        return n.isDouble() && n.toDouble() >= 0 && n.toDouble() <= INT_MAX &&
               n.toDouble() == n.toInt(-1);
    };
    if (integer(v))
        return PackFormat{v.toInt(), maximum ? INT_MAX : 0};
    if (!v.isArray())
        return {};
    auto a = v.toArray();
    if (a.isEmpty() || a.size() > 2 || !integer(a[0]) || (a.size() == 2 && !integer(a[1])))
        return {};
    return PackFormat{a[0].toInt(), a.size() == 2 ? a[1].toInt() : (maximum ? INT_MAX : 0)};
}
const QList<GameVersion> &gameVersions() {
    static const QList<GameVersion> versions = {{"26.3", "release", {97, 1}, true},
                                                {"26.1", "release", {84, 0}, true},
                                                {"1.21.11", "release", {75, 0}, true},
                                                {"1.21.9", "release", {69, 0}, true},
                                                {"1.21.4", "release", {46, 0}, false}};
    return versions;
}
QString Project::validateMetadata(const QByteArray &bytes) {
    if (bytes.size() > 1024 * 1024)
        return "Metadata exceeds the 1 MiB limit";
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return "Invalid JSON object: " + error.errorString();
    auto p = doc.object().value("pack");
    if (!p.isObject())
        return "Missing pack object";
    auto pack = p.toObject();
    auto description = pack.value("description");
    if (!description.isString() && !description.isObject() && !description.isArray())
        return "Description must be text or a JSON text component";
    if (pack.contains("min_format") || pack.contains("max_format")) {
        auto lo = PackFormat::parse(pack.value("min_format")),
             hi = PackFormat::parse(pack.value("max_format"), true);
        if (!lo || !hi || *lo > *hi)
            return "Invalid minimum/maximum pack format range";
        if (lo->major >= 65 && pack.contains("supported_formats"))
            return "supported_formats is forbidden for resource-pack ranges starting at 65 or "
                   "later";
        if (lo->major < 65 && !pack.contains("pack_format"))
            return "Ranges before 65 require pack_format";
    } else {
        auto format = PackFormat::parse(pack.value("pack_format"));
        if (!format || !pack.value("pack_format").isDouble())
            return "Legacy pack_format must be a nonnegative integer";
        if (format->major >= 65)
            return "Modern resource packs require min_format and max_format";
    }
    return {};
}
Project Project::inspect(const QString &path) {
    Project p;
    QDir dir(path);
    p.root = dir.canonicalPath();
    p.name = dir.dirName();
    if (p.root.isEmpty() || !dir.exists() || !QFileInfo(path).isReadable()) {
        p.diagnostics << "Directory is missing or inaccessible";
        return p;
    }
    p.kind = Kind::Folder;
    p.namespaces =
        QDir(p.root + "/assets").entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    auto meta = p.root + "/pack.mcmeta";
    p.revision = Revision::read(meta, 1024 * 1024);
    if (p.revision.exists) {
        QFile file(meta);
        if (file.open(QIODevice::ReadOnly))
            p.metadata = file.read(1024 * 1024 + 1);
        auto error = validateMetadata(p.metadata);
        p.kind = error.isEmpty() ? Kind::ResourcePack : Kind::NeedsRepair;
        if (!error.isEmpty())
            p.diagnostics << error;
        p.json = QJsonDocument::fromJson(p.metadata).object();
        if (error.isEmpty()) {
            auto pack = p.json.value("pack").toObject();
            auto format = PackFormat::parse(
                pack.contains("min_format") ? pack.value("min_format") : pack.value("pack_format"));
            bool verified = false;
            if (format)
                for (const auto &v : gameVersions())
                    if (v.format == *format)
                        verified = true;
            p.diagnostics
                << (verified
                        ? "Pack format is in the verified version registry"
                        : "Pack format is outside the verified registry; compatibility is unknown");
        }
    } else if (dir.exists("assets")) {
        p.kind = Kind::NeedsRepair;
        p.diagnostics << "Missing pack.mcmeta; no repair was performed";
    } else
        p.diagnostics << "Ordinary folder; resource-pack metadata is absent";
    p.diagnostics << "Metadata checks do not validate all Minecraft assets";
    if (!QFileInfo(path).isWritable())
        p.diagnostics << "Directory is read-only; saving may be unavailable";
    QRegularExpression namespaceRule("^[a-z0-9_.-]+$");
    for (const auto &ns : p.namespaces)
        if (!namespaceRule.match(ns).hasMatch())
            p.diagnostics << "Invalid Minecraft namespace: " + ns;
    return p;
}
bool Project::saveMetadata(const QByteArray &bytes, QString *error) {
    auto issue = validateMetadata(bytes);
    if (!issue.isEmpty()) {
        if (error)
            *error = issue;
        return false;
    }
    if (!atomicWrite(root + "/pack.mcmeta", bytes, revision, error))
        return false;
    *this = inspect(root);
    return true;
}
bool Project::exportMinecraft(const QString &source, const QString &destination, QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto root = QFileInfo(source).canonicalFilePath();
    const auto parent = QFileInfo(QFileInfo(destination).absolutePath()).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir() || parent.isEmpty() ||
        QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
        return fail("Choose a new export folder inside an existing directory");
    const auto target = QDir(parent).filePath(QFileInfo(destination).fileName());
    if (pathIdentity(target) == pathIdentity(root) ||
        pathIdentity(target).startsWith(pathIdentity(root) + '/'))
        return fail("Export outside the source pack");
    // Closed layered documents must also agree with their Minecraft PNGs.
    const auto metadata = root + "/.nova/documents";
    if (QFileInfo(root + "/.nova").isSymLink() || QFileInfo(metadata).isSymLink())
        return fail("Linked editor metadata cannot be inspected safely");
    for (const auto &file : QDir(metadata).entryInfoList({"*.nvdoc"}, QDir::Files)) {
        QFile input(file.absoluteFilePath());
        if (file.isSymLink() || !input.open(QIODevice::ReadOnly))
            return fail("Cannot inspect editable source");
        const auto header = input.read(12);
        if (header.size() < 12 || (header.first(8) != "NVLD0001" && header.first(8) != "NVAN0002"))
            return fail("Corrupt editable source prevents pack export");
        const auto length =
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(header.constData() + 8));
        const auto limit = header.first(8) == "NVAN0002" ? 8 * 1024 * 1024 : 1024 * 1024;
        if (length > limit || length > input.size() - 12)
            return fail("Invalid editable manifest");
        const auto asset =
            QJsonDocument::fromJson(input.read(length)).object().value("asset").toString();
        const auto png = QDir(root).filePath(asset);
        if (pathIdentity(nativeDocumentPath(root, png)) != pathIdentity(file.absoluteFilePath()))
            return fail("Editable asset identity is invalid");
        Document document(png, QImage{});
        if (!document.loadNative(root, error))
            return false;
        if (document.exportModified() || document.externalChanged())
            return fail("Update or review the Minecraft texture and metadata before pack export: " + asset);
    }
    QTemporaryDir stage(parent + "/.nova-export-XXXXXX");
    if (!stage.isValid())
        return fail("Could not create export staging folder");
    QStringList todo{""};
    while (!todo.isEmpty()) {
        const auto relative = todo.takeLast();
        const auto entries = QDir(QDir(root).filePath(relative))
                                 .entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                    QDir::NoDotAndDotDot,
                                                QDir::Name);
        for (const auto &entry : entries) {
            if (entry.isSymLink() || entry.isJunction())
                return fail("Linked pack entries cannot be exported safely");
            if (entry.fileName().compare(".nova", Qt::CaseInsensitive) == 0) {
                const auto transactions = entry.absoluteFilePath() + "/animation-exports";
                if (QFileInfo(transactions).isSymLink() || QFileInfo(transactions).isJunction())
                    return fail("Linked animation transactions cannot be inspected safely");
                for (const auto &folder : QDir(transactions).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    const auto journal = folder.absoluteFilePath() + "/journal.json";
                    if (!QFileInfo::exists(journal)) continue; // staging never changed outputs before journaling
                    const auto status = inspectAnimationPair(journal);
                    if (!status.valid || !status.complete) return fail("Recover interrupted animation exports before pack export");
                }
                continue;
            }
            const auto child =
                relative.isEmpty() ? entry.fileName() : relative + '/' + entry.fileName();
            if (child.count('/') > 32)
                return fail("Pack nesting exceeds 32 directories");
            const auto output = stage.filePath(child);
            if (entry.isDir()) {
                if (!QDir().mkpath(output))
                    return fail("Could not create exported directory");
                todo.append(child);
            } else if (entry.isFile()) {
                const auto revision = Revision::read(entry.absoluteFilePath());
                if (!revision.readable || !QFile::copy(entry.absoluteFilePath(), output) ||
                    !revision.sameContent(Revision::read(entry.absoluteFilePath())) ||
                    !revision.sameContent(Revision::read(output)))
                    return fail("Pack file changed or could not be copied: " + child);
            } else
                return fail("Unsupported pack entry: " + child);
        }
    }
    if (QFileInfo::exists(target) || !QDir(parent).rename(stage.path(), target))
        return fail("Export destination changed or could not be finalized");
    stage.setAutoRemove(false);
    return true;
}
bool Project::create(const QString &destination, const QString &name, const GameVersion &version,
                     const QString &description, const QString &ns, const QString &icon,
                     bool folders, QString *error) {
    auto fail = [&](QString message) {
        if (error)
            *error = message;
        return false;
    };
    QRegularExpression namespaceRule("^[a-z0-9_.-]+$");
    if (name.trimmed().isEmpty() || !namespaceRule.match(ns).hasMatch() || ns == "." || ns == "..")
        return fail("Enter a project name and a lowercase Minecraft namespace (a-z, 0-9, _, -, .)");
    QDir parent(QFileInfo(destination).absolutePath());
    if (!parent.exists() || QFileInfo::exists(destination))
        return fail("Choose a new destination folder inside an existing directory; existing "
                    "folders are preserved");
    QImage packIcon;
    if (!icon.isEmpty()) {
        packIcon = loadPng(icon, error);
        if (packIcon.isNull())
            return false;
    }
    QTemporaryDir stage(parent.filePath(".nova-create-XXXXXX"));
    if (!stage.isValid())
        return fail("Could not create staging directory");
    QJsonObject pack{{"description", description}};
    if (version.rangeSchema) {
        pack["min_format"] = version.format.json();
        pack["max_format"] = version.format.json();
    } else
        pack["pack_format"] = version.format.major;
    auto bytes = QJsonDocument(QJsonObject{{"pack", pack}}).toJson();
    if (!validateMetadata(bytes).isEmpty() ||
        !atomicWrite(stage.filePath("pack.mcmeta"), bytes, {}, error))
        return false;
    QDir staging(stage.path());
    if (!staging.mkpath("assets/" + ns))
        return fail("Could not create namespace");
    if (folders)
        for (auto folder :
             {"textures/block", "textures/item", "models", "blockstates", "font", "sounds"})
            if (!staging.mkpath("assets/" + ns + '/' + folder))
                return fail("Could not create asset folders");
    if (!packIcon.isNull() && !savePng(stage.filePath("pack.png"), packIcon, error))
        return false;
    if (!parent.rename(stage.path(), QFileInfo(destination).absoluteFilePath()))
        return fail("Could not finalize project; destination may now exist");
    stage.setAutoRemove(false);
    return true;
}
} // namespace nova
