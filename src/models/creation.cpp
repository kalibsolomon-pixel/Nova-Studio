// SPDX-License-Identifier: GPL-3.0-or-later
#include "creation.h"
#include "display.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
namespace nova::mc {
namespace {
bool fail(QString *e, const QString &s) {
    if (e)
        *e = s;
    return false;
}
} // namespace
bool safeDestination(const QString &root, const QString &relative, QString *absolute,
                     QString *error) {
    static const QRegularExpression forbidden("[<>\"|?*\\x00-\\x1f]");
    static const QRegularExpression devices("^(con|prn|aux|nul|com[1-9]|lpt[1-9])($|\\.)",
                                            QRegularExpression::CaseInsensitiveOption);
    const auto base = QFileInfo(root).canonicalFilePath();
    if (base.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains('\\') ||
        relative.contains(':') || relative.size() > 2048)
        return fail(error, "Invalid project-relative destination");
    auto path = base;
    for (const auto &part : relative.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
            part.endsWith(' ') || part.contains(forbidden) || devices.match(part).hasMatch())
            return fail(error, "Unsafe destination component");
        path += '/' + part;
        const QFileInfo f(path);
        if (f.isSymLink() || f.isJunction() ||
            (f.exists() && !f.canonicalFilePath().startsWith(base + '/', Qt::CaseInsensitive)))
            return fail(error, "Linked or escaped destinations are read-only");
    }
    if (absolute)
        *absolute = path;
    return true;
}
QJsonObject modelTemplate(ModelTemplate type, const QString &texture) {
    if (type == ModelTemplate::FlatItem || type == ModelTemplate::LayeredItem) {
        QJsonObject t{{"layer0", texture}};
        if (type == ModelTemplate::LayeredItem)
            t["layer1"] = texture;
        return {{"parent", "minecraft:item/generated"}, {"textures", t}};
    }
    QJsonObject faces;
    for (const auto &d : {"down", "up", "north", "south", "west", "east"})
        faces[d] = QJsonObject{{"texture", "#all"}};
    QJsonObject e{{"from", QJsonArray{0, 0, 0}}, {"to", QJsonArray{16, 16, 16}}, {"faces", faces}};
    QJsonArray elements;
    if (type == ModelTemplate::Cuboid) {
        e["from"] = QJsonArray{4, 0, 4};
        e["to"] = QJsonArray{12, 12, 12};
    }
    if (type == ModelTemplate::Cross) {
        e["from"] = QJsonArray{0, 0, 8};
        e["to"] = QJsonArray{16, 16, 8};
        e["faces"] = QJsonObject{{"north", QJsonObject{{"texture", "#all"}}},
                                 {"south", QJsonObject{{"texture", "#all"}}}};
        e["rotation"] = QJsonObject{{"origin", QJsonArray{8, 8, 8}}, {"y", 45}};
        elements.append(e);
        e["rotation"] = QJsonObject{{"origin", QJsonArray{8, 8, 8}}, {"y", -45}};
    }
    if (type == ModelTemplate::MultiElement) {
        e["to"] = QJsonArray{16, 4, 16};
        elements.append(e);
        e["from"] = QJsonArray{4, 4, 4};
        e["to"] = QJsonArray{12, 16, 12};
    }
    elements.append(e);
    // Element templates have no parent supplying display transforms; without them
    // Minecraft draws the full-size block in hands, frames and on the ground (C7 finding).
    return {{"textures", QJsonObject{{"all", texture}, {"particle", texture}}},
            {"elements", elements},
            {"display", blockDisplayPreset()}};
}
CreationPlan planCreation(const QString &root, const CreationRequest &r) {
    CreationPlan p;
    p.root = QFileInfo(root).canonicalFilePath();
    auto id = Location::parse(r.id);
    if (p.root.isEmpty() || !Profile::forVersion(r.version).valid() || !r.id.contains(':') ||
        !id.valid() || r.textureSize < 1 || r.textureSize > 1024) {
        p.error = "Choose a valid namespace:path, supported target and 1–1024 texture size";
        return p;
    }
    QString kind = "models";
    if (r.type == AssetType::Blockstate)
        kind = "blockstates";
    if (r.type == AssetType::ItemDefinition)
        kind = "items";
    const bool pair = r.type == AssetType::ModelTexturePair;
    const bool image = r.type == AssetType::Texture || r.type == AssetType::AnimatedTexture || pair;
    const QString texture = r.texture.isEmpty() ? id.id() : r.texture;
    if (!Location::parse(texture).valid()) {
        p.error = "Invalid texture/model reference";
        return p;
    }
    if (r.type != AssetType::Texture && r.type != AssetType::AnimatedTexture) {
        auto model = modelTemplate(r.shape, texture);
        if (r.type == AssetType::Blockstate)
            model = {{"variants", QJsonObject{{"", QJsonObject{{"model", texture}}}}}};
        if (r.type == AssetType::ItemDefinition)
            model = {{"model", QJsonObject{{"type", "minecraft:model"}, {"model", texture}}}};
        p.files.append({id.asset(kind), QJsonDocument(model).toJson()});
    }
    if (image) {
        const auto target = pair ? Location::parse(texture) : id;
        const bool animated = r.type == AssetType::AnimatedTexture;
        QImage im(r.textureSize, r.textureSize * (animated ? 2 : 1), QImage::Format_RGBA8888);
        for (int y = 0; y < im.height(); ++y)
            for (int x = 0; x < im.width(); ++x)
                im.setPixelColor(
                    x, y, ((x / 4 + y / 4) % 2) ? QColor(95, 140, 155) : QColor(165, 195, 200));
        QByteArray bytes;
        QBuffer b(&bytes);
        b.open(QIODevice::WriteOnly);
        if (!im.save(&b, "PNG")) {
            p.error = "PNG encoding failed";
            return p;
        }
        p.files.append({target.asset("textures"), bytes});
        if (animated)
            p.files.append({target.asset("textures") + ".mcmeta",
                            QJsonDocument(QJsonObject{{"animation",
                                                       QJsonObject{{"frametime", 2},
                                                                   {"width", r.textureSize},
                                                                   {"height", r.textureSize},
                                                                   {"frames", QJsonArray{0, 1}}}}})
                                .toJson()});
    }
    QSet<QString> seen;
    for (const auto &f : p.files) {
        QString abs;
        if (!safeDestination(root, f.relative, &abs, &p.error))
            return p;
        if (QFileInfo::exists(abs) || seen.contains(abs.toCaseFolded())) {
            p.error = "Destination exists or conflicts: " + f.relative;
            return p;
        }
        seen.insert(abs.toCaseFolded());
    }
    return p;
}
CreationResult executeCreation(const CreationPlan &p, const std::function<void(int)> &beforeWrite) {
    CreationResult r;
    if (!p.valid()) {
        r.error = p.error.isEmpty() ? "Empty creation plan" : p.error;
        return r;
    }
    QSet<QString> seen;
    qint64 bytes = 0;
    for (const auto &f : p.files) {
        QString abs;
        if (!safeDestination(p.root, f.relative, &abs, &r.error))
            return r;
        if (QFileInfo::exists(abs) || seen.contains(abs.toCaseFolded()) ||
            (bytes += f.bytes.size()) > 16 * 1024 * 1024 || p.files.size() > 64) {
            r.error = "Stale, duplicate or over-budget creation plan";
            return r;
        }
        seen.insert(abs.toCaseFolded());
    }
    r.journal = ".nova/creation/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString folder;
    if (!safeDestination(p.root, r.journal, &folder, &r.error) || !QDir().mkpath(folder)) {
        r.error = "Cannot create recovery journal";
        return r;
    }
    QJsonArray entries;
    for (int i = 0; i < p.files.size(); ++i) {
        const auto &f = p.files[i];
        const auto payload = folder + '/' + QString::number(i) + ".payload";
        if (!atomicWrite(payload, f.bytes, Revision{}, &r.error))
            return r;
        entries.append(QJsonObject{
            {"path", f.relative},
            {"sha256",
             QString::fromLatin1(
                 QCryptographicHash::hash(f.bytes, QCryptographicHash::Sha256).toHex())}});
    }
    const auto manifest = folder + "/manifest.json";
    QJsonObject journal{
        {"schema", 1}, {"state", "prepared"}, {"files", entries}, {"written", QJsonArray{}}};
    Revision rev;
    auto record = [&] {
        if (!atomicWrite(manifest, QJsonDocument(journal).toJson(), rev, &r.error))
            return false;
        rev = Revision::read(manifest);
        return true;
    };
    if (!record())
        return r;
    for (int i = 0; i < p.files.size(); ++i) {
        if (beforeWrite)
            beforeWrite(i);
        QString dest;
        if (!safeDestination(p.root, p.files[i].relative, &dest, &r.error) ||
            !QDir().mkpath(QFileInfo(dest).absolutePath()) ||
            !safeDestination(p.root, p.files[i].relative, &dest, &r.error) ||
            !atomicWrite(dest, p.files[i].bytes, Revision{}, &r.error)) {
            journal["state"] = "partial";
            journal["error"] = r.error;
            QString original = r.error;
            record();
            r.error = original;
            return r;
        }
        r.written << p.files[i].relative;
        journal["written"] = QJsonArray::fromStringList(r.written);
        if (!record())
            return r;
    }
    journal["state"] = "complete";
    r.complete = record();
    return r;
}
bool rollbackCreation(const QString &root, const QString &journal, QString *error) {
    if (!journal.startsWith(".nova/creation/"))
        return fail(error, "Expected a creation journal");
    QString path;
    if (!safeDestination(root, journal + "/manifest.json", &path, error))
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
        return fail(error, "Journal unavailable");
    const auto j = QJsonDocument::fromJson(f.readAll()).object();
    if (j["schema"].toInt() != 1)
        return fail(error, "Unknown journal schema");
    const auto alias = j["alias"].toObject();
    QString aliasPath;
    Revision aliasRevision;
    QByteArray original;
    if (!alias.isEmpty()) {
        if (!safeDestination(root, alias["file"].toString(), &aliasPath, error))
            return false;
        aliasRevision = Revision::read(aliasPath);
        const auto hash = QString::fromLatin1(aliasRevision.hash.toHex());
        if (!aliasRevision.readable || !aliasRevision.exists ||
            (hash != alias["original_sha256"].toString() &&
             hash != alias["alias_sha256"].toString()))
            return fail(error, "Rename alias changed externally; recovery stopped");
        QFile backup(QFileInfo(path).absolutePath() + "/original.json");
        if (!backup.open(QIODevice::ReadOnly) || backup.size() > 2 * 1024 * 1024)
            return fail(error, "Rename backup unavailable");
        original = backup.readAll();
        if (QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex() !=
            alias["original_sha256"].toString().toLatin1())
            return fail(error, "Rename backup checksum mismatch");
    }
    // Include prepared entries: a crash may happen between replacement and journal update.
    for (const auto &v : j["files"].toArray()) {
        auto o = v.toObject();
        QString dest;
        if (!safeDestination(root, o["path"].toString(), &dest, error))
            return false;
        auto rev = Revision::read(dest);
        if (!rev.exists)
            continue;
        if (!rev.readable || rev.hash.toHex() != o["sha256"].toString().toLatin1())
            return fail(error, "Recovery conflict: " + o["path"].toString());
    }
    if (!alias.isEmpty() &&
        aliasRevision.hash.toHex() == alias["alias_sha256"].toString().toLatin1()) {
        if (!atomicWrite(aliasPath, original, aliasRevision, error))
            return false;
    }
    for (const auto &v : j["files"].toArray()) {
        auto o = v.toObject();
        QString dest;
        if (!safeDestination(root, o["path"].toString(), &dest, error))
            return false;
        auto rev = Revision::read(dest);
        if (!rev.exists)
            continue;
        if (rev.hash.toHex() != o["sha256"].toString().toLatin1() || !QFile::remove(dest))
            return fail(error, "Recovery changed or removal failed");
    }
    return true;
}
} // namespace nova::mc
