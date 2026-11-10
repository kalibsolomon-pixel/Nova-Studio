// SPDX-License-Identifier: GPL-3.0-or-later
#include "relationships.h"
#include "inspection.h"
#include "minecraft_animation.h"
#include "protocol.h"
#include "scope.h"
#include "sources.h"
#include "vanilla/cache.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QtEndian>
#include <functional>

namespace nova::mcp {
namespace {
// The worn-equipment texture convention (equipment layer -> textures/entity/equipment/<layer>/<path>.png)
// and the item-icon naming below are checked against the official client only for this version
// (tests/fixtures/guides/minecraft-1.21.11-facts.json). Other versions get suggestions, not confirmations.
constexpr auto EquipmentVerified = "1.21.11";
const QStringList ArmorPieces{"helmet", "chestplate", "leggings", "boots"};

QString escape(QString key) { return key.replace('~', "~0").replace('/', "~1"); }
QString modelPath(const QString &id) { return mc::Location::parse(id).asset("models"); }

struct Selection {
    QString pointer, model, context;
    bool conditional = false;
};
// Every model a definition can select, with the JSON pointer of the reference. Runtime choices
// (select, condition, range_dispatch, weighted alternatives, multipart conditions) are marked
// conditional rather than evaluated: Nova cannot know the game state that selects them.
void walkItem(const QJsonObject &node, const QString &pointer, const QString &context, bool conditional, int depth,
              QList<Selection> &out, QStringList &unsupported) {
    if (depth > 32 || out.size() >= RelationshipLimit)
        return;
    const auto type = mc::Location::parse(node["type"].toString()).id();
    if (type == "minecraft:model") {
        out.append({pointer + "/model", node["model"].toString(), context, conditional});
    } else if (type == "minecraft:composite") {
        const auto models = node["models"].toArray();
        for (qsizetype i = 0; i < models.size(); ++i)
            walkItem(models[i].toObject(), pointer + "/models/" + QString::number(i), context + " / composite part " +
                     QString::number(i), conditional, depth + 1, out, unsupported);
    } else if (type == "minecraft:condition") {
        const auto property = node["property"].toString().left(64);
        walkItem(node["on_true"].toObject(), pointer + "/on_true", context + " / " + property + " = true", true,
                 depth + 1, out, unsupported);
        walkItem(node["on_false"].toObject(), pointer + "/on_false", context + " / " + property + " = false", true,
                 depth + 1, out, unsupported);
    } else if (type == "minecraft:select") {
        const auto property = node["property"].toString().left(64);
        const auto cases = node["cases"].toArray();
        for (qsizetype i = 0; i < cases.size(); ++i) {
            const auto when = cases[i].toObject()["when"];
            const auto label = QString::fromUtf8(QJsonDocument(QJsonArray{when}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
            walkItem(cases[i].toObject()["model"].toObject(), pointer + "/cases/" + QString::number(i) + "/model",
                     context + " / " + property + " = " + label.left(64), true, depth + 1, out, unsupported);
        }
        if (node["fallback"].isObject())
            walkItem(node["fallback"].toObject(), pointer + "/fallback", context + " / " + property + " fallback", true,
                     depth + 1, out, unsupported);
    } else if (type == "minecraft:range_dispatch") {
        const auto property = node["property"].toString().left(64);
        const auto entries = node["entries"].toArray();
        for (qsizetype i = 0; i < entries.size(); ++i)
            walkItem(entries[i].toObject()["model"].toObject(), pointer + "/entries/" + QString::number(i) + "/model",
                     context + QString(" / %1 >= %2").arg(property).arg(entries[i].toObject()["threshold"].toDouble()),
                     true, depth + 1, out, unsupported);
        if (node["fallback"].isObject())
            walkItem(node["fallback"].toObject(), pointer + "/fallback", context + " / " + property + " fallback", true,
                     depth + 1, out, unsupported);
    } else if (type == "minecraft:special") {
        unsupported << pointer + ": special renderer " + node["model"].toObject()["type"].toString().left(64);
        if (node["base"].isString())
            out.append({pointer + "/base", node["base"].toString(), context + " / special renderer base", conditional});
    } else if (type != "minecraft:empty") {
        unsupported << pointer + ": item model type " + (type.isEmpty() ? QString("(missing)") : type);
    }
}
QList<Selection> selections(const QString &path, const QJsonObject &definition, QStringList &unsupported) {
    QList<Selection> out;
    if (resourceKind(path) == "item_definition") {
        walkItem(definition["model"].toObject(), "/model", "item definition", false, 0, out, unsupported);
        return out;
    }
    auto apply = [&](const QJsonValue &value, const QString &pointer, const QString &context, bool conditional) {
        if (value.isObject()) {
            out.append({pointer + "/model", value.toObject()["model"].toString(), context, conditional});
            return;
        }
        const auto array = value.toArray();
        for (qsizetype i = 0; i < array.size() && out.size() < RelationshipLimit; ++i)
            out.append({pointer + '/' + QString::number(i) + "/model", array[i].toObject()["model"].toString(),
                        context + QString(" / weighted alternative %1 of %2").arg(i + 1).arg(array.size()), true});
    };
    const auto variants = definition["variants"].toObject();
    for (auto it = variants.begin(); it != variants.end() && out.size() < RelationshipLimit; ++it)
        apply(it.value(), "/variants/" + escape(it.key()), "variant " + (it.key().isEmpty() ? QString("(default)") : it.key()),
              variants.size() > 1);
    const auto parts = definition["multipart"].toArray();
    for (qsizetype i = 0; i < parts.size() && out.size() < RelationshipLimit; ++i)
        apply(parts[i].toObject()["apply"], "/multipart/" + QString::number(i) + "/apply",
              QString("multipart part %1").arg(i), parts[i].toObject().contains("when"));
    return out;
}

// One relationship request: the catalog over every allowed source, plus bounded output.
class Inspector {
  public:
    Inspector(const Snapshot &snapshot, SourceSet &set, const std::shared_ptr<std::atomic_bool> &stop)
        : s(snapshot), sources(set), cancel(stop) {}
    QJsonArray relations, limitations;
    bool truncated = false;
    qsizetype bytes = 0; // the result is sent twice (text and structured content) in one 512 KiB line

    void add(QJsonObject row) {
        const auto size = QJsonDocument(row).toJson(QJsonDocument::Compact).size();
        if (relations.size() >= RelationshipLimit || bytes + size > RelationshipBytes) {
            truncated = true;
            return;
        }
        bytes += size;
        relations.append(row);
    }
    QJsonObject source(const QString &path) {
        auto provider = sources.provider(path);
        if (provider.source == "none" && sources.vanillaTexture(path))
            return QJsonObject{{"source", "vanilla_texture_cache"}, {"verified", true},
                               {"identity", sources.textureCache().entry->identity}};
        return provider.json();
    }
    bool available(const QString &path) { return sources.usable(path) || sources.vanillaTexture(path); }
    QString missing(const QString &id, const QString &kind) {
        const auto location = mc::Location::parse(id);
        if (!sources.catalogComplete())
            return QString("The %1 %2 was not found, but Nova's model catalog budget (8192 JSON files / 32 MiB) was "
                           "exceeded, so the configured sources were read only in part; Nova cannot tell whether it "
                           "exists.").arg(kind, id);
        if (location.nameSpace == "minecraft" && !sources.vanillaModelSource())
            return QString("The %1 %2 is not in the project or the configured resource stack. It may be a vanilla %1, "
                           "but no verified vanilla resource source is configured for Java %3, and Nova's vanilla texture "
                           "cache contains textures only, so Nova cannot resolve it.").arg(kind, id, s.version);
        return QString("The %1 %2 is not in the project or the configured resource stack.").arg(kind, id);
    }
    // Parent chain from `id`, one step per model, with the source of each step.
    QJsonArray chain(const QString &id, QString *status, QString *reason) {
        QJsonArray steps;
        QSet<QString> seen;
        auto cursor = id;
        *status = "confirmed";
        for (int depth = 0;; ++depth) {
            if (depth >= ParentChainLimit) {
                *status = "unsupported";
                *reason = QString("The parent chain exceeds %1 models.").arg(ParentChainLimit);
                return steps;
            }
            if (cursor == "minecraft:builtin/generated") {
                steps.append(QJsonObject{{"id", cursor}, {"builtin", true}, {"status", "confirmed"}});
                return steps;
            }
            if (cursor.startsWith("minecraft:builtin/")) {
                steps.append(QJsonObject{{"id", cursor}, {"builtin", true}, {"status", "unsupported"}});
                *status = "unsupported";
                *reason = cursor + " is a built-in renderer that Nova does not resolve.";
                return steps;
            }
            if (seen.contains(cursor)) {
                steps.append(QJsonObject{{"id", cursor}, {"status", "cyclic"}});
                *status = "cyclic";
                *reason = "The parent chain returns to " + cursor + '.';
                return steps;
            }
            seen.insert(cursor);
            const auto path = modelPath(cursor);
            QJsonObject step{{"id", cursor}, {"path", path}};
            if (!sources.provided(path)) {
                step["status"] = "unresolved";
                steps.append(step);
                *status = "unresolved";
                *reason = missing(cursor, "model");
                return steps;
            }
            step["source"] = source(path);
            const auto model = sources.library().source(path);
            if (model.isEmpty() && sources.library().failure(path).isEmpty() && !sources.catalogComplete()) {
                step["status"] = "unresolved"; // provided but never read: the catalog budget stopped first
                steps.append(step);
                *status = "unresolved";
                *reason = missing(cursor, "model");
                return steps;
            }
            if (model.isEmpty()) {
                const auto failure = sources.library().failure(path);
                step["status"] = "invalid";
                steps.append(step);
                *status = "invalid";
                *reason = path + " cannot be read as model JSON" +
                          (failure.isEmpty() ? QString(".") : ": " + failure.left(200));
                return steps;
            }
            step["status"] = "confirmed";
            steps.append(step);
            if (!model.contains("parent"))
                return steps;
            const auto parent = mc::Location::parse(model["parent"].toString());
            if (!model["parent"].isString() || !parent.valid()) {
                *status = "invalid";
                *reason = path + " has an invalid parent reference.";
                return steps;
            }
            cursor = parent.id();
        }
    }
    // Which model of `chain` (child first) defines a texture variable or display context.
    static QString definer(const QStringList &chain, const QString &key, const QString &section, mc::Library &library) {
        for (const auto &id : chain)
            if (library.source(modelPath(id))[section].toObject().contains(key))
                return id;
        return {};
    }
    QStringList lineage(const mc::Model &model) {
        QStringList ids{model.id};
        for (auto it = model.parents.crbegin(); it != model.parents.crend(); ++it)
            ids << *it;
        return ids;
    }
    // Texture variables of a resolved model: raw value, resolved texture, the defining model and overrides.
    QJsonArray variables(const mc::Model &model) {
        QJsonArray rows;
        const auto ids = lineage(model);
        const auto raw = model.resolved["textures"].toObject();
        for (auto it = raw.begin(); it != raw.end() && rows.size() < RelationshipFaceLimit; ++it) {
            QJsonObject row{{"variable", it.key()}, {"value", it->isObject() ? it->toObject()["sprite"] : *it},
                            {"definedBy", definer(ids, it.key(), "textures", sources.library())}};
            QJsonArray overridden;
            bool first = true;
            for (const auto &id : ids)
                if (sources.library().source(modelPath(id))["textures"].toObject().contains(it.key())) {
                    if (!first)
                        overridden.append(id);
                    first = false;
                }
            if (!overridden.isEmpty())
                row["overrides"] = overridden; // parent definitions this model's value replaces
            const auto target = model.textures.value(it.key());
            if (target.isEmpty()) {
                row["status"] = "unresolved";
            } else {
                row["texture"] = target;
                row["textureSource"] = source(target);
                row["status"] = available(target) ? "confirmed" : "unresolved";
            }
            rows.append(row);
        }
        return rows;
    }
    // Faces of a resolved model that sample `texture` (all faces when empty).
    QJsonArray faces(const mc::Model &model, const QString &texture, bool *more, int limit = RelationshipFaceLimit) {
        QJsonArray rows;
        const auto ids = lineage(model);
        QString elementsBy;
        for (const auto &id : ids)
            if (sources.library().source(modelPath(id)).contains("elements")) {
                elementsBy = id;
                break;
            }
        const auto elements = model.resolved.value("elements").toArray();
        if (model.generated && elements.isEmpty()) {
            for (int layer = 0; layer < 16; ++layer) {
                const auto target = model.textures.value("layer" + QString::number(layer));
                if (!target.isEmpty() && (texture.isEmpty() || target == texture))
                    rows.append(QJsonObject{{"generatedLayer", layer}, {"variable", "layer" + QString::number(layer)},
                                            {"texture", target}, {"uv", "whole texture (item/generated)"}});
            }
            return rows;
        }
        for (qsizetype i = 0; i < elements.size() && i < 4096; ++i) {
            const auto faceMap = elements[i].toObject()["faces"].toObject();
            for (auto f = faceMap.begin(); f != faceMap.end(); ++f) {
                const auto face = f->toObject();
                const auto ref = face["texture"].toString();
                const auto target = ref.startsWith('#') ? model.textures.value(ref.mid(1))
                                                        : mc::Location::parse(ref).asset("textures");
                if (!texture.isEmpty() && target != texture)
                    continue;
                if (rows.size() >= limit) {
                    *more = true;
                    return rows;
                }
                QJsonObject row{{"element", qint64(i)}, {"face", f.key()}, {"reference", ref.left(128)},
                                {"elementsDefinedBy", elementsBy},
                                {"uv", face.contains("uv") ? face["uv"] : QJsonValue("default (from element bounds)")},
                                {"rotation", face["rotation"].toInt()}};
                if (target.isEmpty())
                    row["status"] = "unresolved";
                else
                    row["texture"] = target;
                if (face.contains("tintindex"))
                    row["tintindex"] = face["tintindex"];
                rows.append(row);
            }
        }
        return rows;
    }
    // Item definitions and blockstates that select the model at `modelPath`, with JSON pointers.
    void selectors(const QString &model, const QString &via) {
        const auto id = mc::Location::fromAsset(model, "models");
        for (const auto &user : sources.library().dependents(model, false)) {
            if (cancel->load())
                return;
            const auto kind = resourceKind(user);
            if (kind != "item_definition" && kind != "blockstate")
                continue;
            QStringList unsupported;
            for (const auto &selection : selections(user, sources.library().source(user), unsupported)) {
                if (mc::Location::parse(selection.model).id() != id)
                    continue;
                QJsonObject row{{"type", kind == "blockstate" ? "selected_by_blockstate" : "selected_by_item_definition"},
                    {"status", selection.conditional ? "conditional" : "confirmed"}, {"from", user}, {"to", model},
                    {"source", source(user)},
                    {"evidence", QJsonObject{{"pointer", selection.pointer}, {"context", selection.context}, {"model", id}}}};
                if (!via.isEmpty())
                    row["via"] = via;
                if (selection.conditional)
                    row["reason"] = "Selected only in some game states; Nova cannot evaluate which one applies.";
                add(row);
            }
        }
    }
    void modelUsesTexture(const QString &path) {
        QSet<QString> reported;
        for (const auto &user : sources.library().dependents(path)) {
            if (cancel->load())
                break;
            if (relations.size() >= RelationshipLimit) {
                truncated = true;
                break;
            }
            const auto id = mc::Location::fromAsset(user, "models");
            if (id.isEmpty())
                continue;
            const auto direct = sources.library().dependencies(user).contains(path);
            const auto model = sources.library().model(id);
            const auto bindings = sources.library().source(user)["textures"].toObject();
            QStringList variablesHere;
            for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                const auto value = it->isObject() ? it->toObject()["sprite"].toString() : it->toString();
                if (mc::Location::parse(value).asset("textures") == path)
                    variablesHere << it.key();
            }
            if (!model.valid) {
                if (!direct)
                    continue;
                QString status, reason;
                const auto steps = chain(id, &status, &reason);
                add({{"type", "model_texture_binding"}, {"status", "confirmed"}, {"from", user}, {"to", path},
                     {"source", source(user)},
                     {"evidence", QJsonObject{{"variables", QJsonArray::fromStringList(variablesHere)},
                                              {"pointer", variablesHere.isEmpty() ? QString("/elements")
                                                                                   : "/textures/" + escape(variablesHere.first())}}}});
                add({{"type", "model_resolution"}, {"status", status == "confirmed" ? QString("unresolved") : status},
                     {"from", user}, {"reason", reason.isEmpty() ? model.diagnostics.value(0).left(300) : reason},
                     {"evidence", QJsonObject{{"parentChain", steps}}}});
                continue;
            }
            QStringList used;
            for (auto it = model.textures.cbegin(); it != model.textures.cend(); ++it)
                if (it.value() == path)
                    used << it.key();
            bool more = false;
            const auto sampled = faces(model, path, &more, RowFaceLimit);
            truncated |= more;
            if (used.isEmpty() && sampled.isEmpty()) {
                // A parent binds the texture but this model replaces every variable that used it.
                QJsonArray replaced;
                for (const auto &parent : model.parents) {
                    const auto parentBindings = sources.library().source(modelPath(parent))["textures"].toObject();
                    for (auto it = parentBindings.begin(); it != parentBindings.end(); ++it) {
                        const auto value = it->isObject() ? it->toObject()["sprite"].toString() : it->toString();
                        if (mc::Location::parse(value).asset("textures") == path && model.textures.contains(it.key()))
                            replaced.append(QJsonObject{{"variable", it.key()}, {"inheritedFrom", parent},
                                                        {"replacedWith", model.textures.value(it.key())}});
                    }
                }
                if (!replaced.isEmpty())
                    add({{"type", "variable_override"}, {"status", "confirmed"}, {"from", user}, {"to", path},
                         {"source", source(user)}, {"evidence", QJsonObject{{"overrides", replaced}}},
                         {"reason", "This model inherits a parent that uses the texture but replaces the variable."}});
                continue;
            }
            QJsonArray variableRows;
            const auto ids = lineage(model);
            for (const auto &variable : used)
                variableRows.append(QJsonObject{{"variable", variable},
                    {"definedBy", definer(ids, variable, "textures", sources.library())}});
            add({{"type", "model_uses_texture"}, {"status", "confirmed"}, {"from", user}, {"to", path},
                 {"source", source(user)},
                 {"evidence", QJsonObject{{"model", id}, {"variables", variableRows}, {"faces", sampled},
                                          {"parentChain", QJsonArray::fromStringList(model.parents)},
                                          {"inherited", !direct}}}});
            if (!reported.contains(user)) {
                reported.insert(user);
                selectors(user, path);
            }
        }
    }
    // Armor icon <-> worn texture and equipment definition companions (1.21.11 convention).
    void companions(const QString &path) {
        const auto location = mc::Location::parse(mc::Location::fromAsset(path, "textures"));
        if (!location.valid())
            return;
        const bool verified = s.version == EquipmentVerified;
        auto equipment = [&](const QString &name) {
            return QString("assets/%1/equipment/%2.json").arg(location.nameSpace, name);
        };
        auto worn = [&](const QString &layer, const QString &name) {
            return QString("assets/%1/textures/entity/equipment/%2/%3.png").arg(location.nameSpace, layer, name);
        };
        auto suggest = [&](const QString &to, const QString &why) {
            add({{"type", "companion_asset"}, {"status", "possible"}, {"from", path}, {"to", to},
                 {"source", available(to) ? source(to) : QJsonObject{{"source", "none"}}},
                 {"reason", why + (verified ? QString() : QString(" The naming convention is verified by Nova for "
                                                                   "Java %1 only.").arg(EquipmentVerified))}});
        };
        if (location.path.startsWith("item/")) {
            const auto name = location.path.mid(5);
            for (const auto &piece : ArmorPieces) {
                if (!name.endsWith('_' + piece))
                    continue;
                const auto material = name.chopped(piece.size() + 1);
                const auto layer = piece == "leggings" ? QString("humanoid_leggings") : QString("humanoid");
                const QString why = "Armor icons and worn armor are separate textures; which equipment asset an item "
                                    "uses is set by game data outside the resource pack, so Nova cannot confirm the link.";
                suggest(equipment(material), why);
                suggest(worn(layer, material), why);
            }
            return;
        }
        if (location.path.startsWith("entity/equipment/")) {
            const auto parts = location.path.split('/');
            if (parts.size() < 4)
                return;
            const auto layer = parts[2];
            const auto textureId = location.nameSpace + ':' + parts.mid(3).join('/');
            // Confirmed only from an equipment definition that a source provides.
            QStringList definers;
            const auto candidateName = parts.mid(3).join('/');
            if (sources.provided(equipment(candidateName)))
                definers << equipment(candidateName);
            for (const auto &file : definers) {
                const auto json = QJsonDocument::fromJson(sources.read(file, 256 * 1024)).object();
                const auto entries = json["layers"].toObject()[layer].toArray();
                for (qsizetype i = 0; i < entries.size(); ++i)
                    if (mc::Location::parse(entries[i].toObject()["texture"].toString()).id() == textureId)
                        add({{"type", "equipment_layer_texture"}, {"status", verified ? "confirmed" : "possible"},
                             {"from", file}, {"to", path}, {"source", source(file)},
                             {"evidence", QJsonObject{{"pointer", QString("/layers/%1/%2/texture").arg(escape(layer)).arg(i)},
                                                      {"texture", textureId}, {"layer", layer},
                                                      {"convention", QString("textures/entity/equipment/<layer>/<texture>.png, "
                                                                             "verified for Java %1").arg(EquipmentVerified)}}}});
            }
            QStringList icons;
            for (const auto &piece : ArmorPieces)
                if ((layer == "humanoid_leggings") == (piece == "leggings"))
                    icons << QString("assets/%1/textures/item/%2_%3.png").arg(location.nameSpace, candidateName, piece);
            for (const auto &icon : icons)
                if (available(icon))
                    suggest(icon, "Worn armor and inventory icons are separate textures; restyle both together. Which "
                                  "item uses this equipment is game data outside the resource pack.");
            if (definers.isEmpty())
                add({{"type", "equipment_layer_texture"}, {"status", "unresolved"}, {"from", equipment(candidateName)},
                     {"to", path},
                     {"reason", available(equipment(candidateName)) ? QString("The equipment definition could not be read.")
                         : "No source provides " + equipment(candidateName) + (sources.vanillaModelSource() ? QString(".")
                             : QString(", and Nova's vanilla texture cache contains textures only."))}});
        }
    }

  private:
    const Snapshot &s;
    SourceSet &sources;
    std::shared_ptr<std::atomic_bool> cancel;
};

QSize pngSize(const QByteArray &bytes) {
    if (bytes.size() < 24 || !bytes.startsWith("\x89PNG\r\n\x1a\n") || bytes.mid(12, 4) != "IHDR")
        return {};
    return {int(qFromBigEndian<quint32>(bytes.constData() + 16)), int(qFromBigEndian<quint32>(bytes.constData() + 20))};
}
} // namespace

QString resourceKind(const QString &path) {
    if (path == "pack.mcmeta")
        return "pack_metadata";
    if (path.endsWith(".png.mcmeta"))
        return "animation_metadata";
    const auto parts = path.split('/');
    if (parts.size() < 4 || parts[0] != "assets")
        return "other";
    if (parts[2] == "textures" && path.endsWith(".png"))
        return "texture";
    if (!path.endsWith(".json"))
        return "other";
    if (parts[2] == "models")
        return "model";
    if (parts[2] == "items")
        return "item_definition";
    if (parts[2] == "blockstates")
        return "blockstate";
    if (parts[2] == "equipment")
        return "equipment";
    if (parts[2] == "atlases")
        return "atlas";
    return "other";
}

QJsonObject relationships(const Snapshot &s, Scope &scope, const QJsonObject &args,
                          const std::shared_ptr<std::atomic_bool> &cancel) {
    const auto path = args["path"].toString();
    for (const auto &segment : path.split('/'))
        if (segment.isEmpty() || segment == "." || segment == "..")
            return toolError("invalid_arguments", "Invalid argument at /path: expected a resource-pack path such as "
                             "assets/minecraft/textures/item/diamond_sword.png, without empty, . or .. segments.",
                             {{"pointer", "/path"}});
    if (args.contains("version") && args["version"].toString() != s.version)
        return toolError("version_unavailable",
            QString("Nova's model catalog and resource stack for this project are loaded for Java %1, not %2. Use "
                    "%1, or ask the user to switch the target version in Nova.").arg(s.version, args["version"].toString().left(16)),
            {{"pointer", "/version"}, {"projectVersion", s.version}});
    if (!supportedVersion(s.version))
        return toolError("unsupported_version", QString("The project's Minecraft version %1 is not supported.").arg(s.version.left(16)));
    SourceSet sources(s, scope, cancel);
    if (sources.canceled() || cancel->load())
        return toolError("operation_canceled", "The operation was canceled.");
    Inspector inspector(s, sources, cancel);
    const auto kind = resourceKind(path);
    QJsonObject detail;
    auto &library = sources.library();
    if (kind == "texture") {
        if (sources.providers(path + ".mcmeta").size() || (!sources.provided(path) && sources.vanillaTexture(path + ".mcmeta"))) {
            const auto texture = inspector.source(path), meta = inspector.source(path + ".mcmeta");
            const bool same = texture["source"] == meta["source"] && texture["stackIndex"] == meta["stackIndex"];
            QJsonObject row{{"type", "animation_metadata"}, {"status", same ? "confirmed" : "possible"}, {"from", path},
                            {"to", path + ".mcmeta"}, {"source", meta},
                            {"evidence", QJsonObject{{"rule", "<texture>.png.mcmeta beside the texture"}, {"textureSource", texture}}}};
            if (!same)
                row["reason"] = "The metadata comes from a different source than the texture; Nova has not verified "
                                "whether Minecraft applies it.";
            inspector.add(row);
        }
        inspector.modelUsesTexture(path);
        inspector.companions(path);
        if (inspector.relations.isEmpty() || std::none_of(inspector.relations.begin(), inspector.relations.end(),
                [](const QJsonValue &row) { return row.toObject()["type"].toString().startsWith("model_"); })) {
            inspector.add({{"type", "model_uses_texture"}, {"status", "unresolved"}, {"to", path},
                {"reason", sources.vanillaModelSource()
                    ? QString("No model in the project or the configured resource stack references this texture. It may "
                              "be used directly by game code (for example GUI or entity rendering), which Nova cannot see.")
                    : QString("No model in the project or the configured resource stack references this texture. No verified "
                              "vanilla resource source is configured, so vanilla models that may use it are unavailable; "
                              "Nova's vanilla texture cache contains textures only. Nova does not guess a model.")}});
        }
        const auto bytes = sources.read(path, 24, nullptr, true);
        if (const auto size = pngSize(bytes); size.isValid())
            detail = {{"width", size.width()}, {"height", size.height()}};
    } else if (kind == "animation_metadata") {
        const auto texture = path.chopped(7);
        inspector.add({{"type", "animation_texture"}, {"status", inspector.available(texture) ? "confirmed" : "unresolved"},
                       {"from", path}, {"to", texture}, {"source", inspector.source(texture)}});
        QString why;
        const auto meta = QJsonDocument::fromJson(sources.read(path, 2 * 1024 * 1024, &why)).object();
        const auto sheet = sources.read(texture, 16 * 1024 * 1024);
        QImage image;
        const auto size = pngSize(sheet);
        if (size.isValid() && size.width() <= 4096 && size.height() <= 4096)
            image.loadFromData(sheet, "PNG");
        AnimationState state;
        QString error;
        if (meta.isEmpty())
            detail = {{"status", "invalid"}, {"reason", why.isEmpty() ? QString("The metadata is not a JSON object.") : why}};
        else if (image.isNull())
            detail = {{"status", "unresolved"}, {"reason", "The spritesheet is missing, unreadable or larger than 4096×4096."}};
        else if (!parseAnimation(image, meta, s.version, &state, &error))
            detail = {{"status", "invalid"}, {"reason", error.left(300)}, {"textureWidth", image.width()},
                      {"textureHeight", image.height()}};
        else {
            const auto frame = state.frames.empty() || state.frames.front().layers.empty()
                ? QSize() : state.frames.front().layers.front().pixels.size();
            detail = {{"status", "confirmed"}, {"textureWidth", image.width()}, {"textureHeight", image.height()},
                      {"frameWidth", frame.width()}, {"frameHeight", frame.height()},
                      {"frames", qint64(state.frames.size())}, {"playbackEntries", qint64(state.entries.size())},
                      {"defaultTicks", state.defaultTicks}, {"interpolate", state.interpolate}};
        }
    } else if (kind == "model") {
        const auto id = mc::Location::fromAsset(path, "models");
        QString status, reason;
        const auto steps = inspector.chain(id, &status, &reason);
        inspector.add({{"type", "parent_chain"}, {"status", status}, {"from", path}, {"evidence", QJsonObject{{"steps", steps}}},
                       {"reason", reason}});
        const auto model = library.model(id);
        detail = {{"model", id}, {"valid", model.valid}, {"parents", QJsonArray::fromStringList(model.parents)},
                  {"diagnostics", QJsonArray::fromStringList(model.diagnostics.mid(0, 16))}};
        if (model.valid) {
            bool more = false;
            detail["textureVariables"] = inspector.variables(model);
            detail["faces"] = inspector.faces(model, {}, &more);
            inspector.truncated |= more;
            QJsonArray display;
            for (auto it = model.displayOrigin.cbegin(); it != model.displayOrigin.cend(); ++it)
                display.append(QJsonObject{{"context", it.key()}, {"definedBy", it->first}, {"key", it->second}});
            detail["display"] = display;
            for (const auto &target : model.textures)
                inspector.add({{"type", "texture_reference"}, {"status", inspector.available(target) ? "confirmed" : "unresolved"},
                               {"from", path}, {"to", target}, {"source", inspector.source(target)},
                               {"reason", inspector.available(target) ? QString() : QString("No source provides this texture.")}});
        }
        for (const auto &child : library.dependents(path, false))
            if (resourceKind(child) == "model")
                inspector.add({{"type", "child_model"}, {"status", "confirmed"}, {"from", child}, {"to", path},
                               {"source", inspector.source(child)}, {"evidence", QJsonObject{{"pointer", "/parent"}}}});
        inspector.selectors(path, {});
    } else if (kind == "item_definition" || kind == "blockstate") {
        const auto definition = library.source(path);
        if (definition.isEmpty() && sources.provided(path) && library.failure(path).isEmpty() && !sources.catalogComplete()) {
            inspector.add({{"type", "definition"}, {"status", "unresolved"}, {"from", path},
                           {"reason", "Nova's model catalog budget (8192 JSON files / 32 MiB) was exceeded before this "
                                      "definition was read."}});
        } else if (definition.isEmpty() && sources.provided(path)) {
            inspector.add({{"type", "definition"}, {"status", "invalid"}, {"from", path},
                           {"reason", "Cannot be read as JSON: " + library.failure(path).left(200)}});
        } else if (!sources.provided(path)) {
            inspector.add({{"type", "definition"}, {"status", "unresolved"}, {"from", path},
                           {"reason", inspector.missing(mc::Location::fromAsset(path, kind == "blockstate" ? "blockstates" : "items"),
                                                        kind == "blockstate" ? "blockstate" : "item definition")}});
        } else {
            QStringList unsupported;
            for (const auto &selection : selections(path, definition, unsupported)) {
                const auto location = mc::Location::parse(selection.model);
                if (!location.valid()) {
                    inspector.add({{"type", "selects_model"}, {"status", "invalid"}, {"from", path},
                                   {"evidence", QJsonObject{{"pointer", selection.pointer}}},
                                   {"reason", "Invalid model reference."}});
                    continue;
                }
                const auto target = location.asset("models");
                QString status, reason;
                const auto steps = inspector.chain(location.id(), &status, &reason);
                QJsonObject row{{"type", "selects_model"},
                    {"status", status != "confirmed" ? status : selection.conditional ? QString("conditional") : status},
                    {"from", path}, {"to", target}, {"source", inspector.source(target)},
                    {"evidence", QJsonObject{{"pointer", selection.pointer}, {"context", selection.context},
                                             {"model", location.id()}, {"parentChain", steps}}}};
                if (!reason.isEmpty())
                    row["reason"] = reason;
                else if (selection.conditional)
                    row["reason"] = "Selected only in some game states; Nova cannot evaluate which one applies.";
                const auto model = library.model(location.id());
                if (model.valid) {
                    QJsonArray textures;
                    for (auto it = model.textures.cbegin(); it != model.textures.cend(); ++it)
                        textures.append(QJsonObject{{"variable", it.key()}, {"texture", it.value()},
                                                    {"source", inspector.source(it.value())}});
                    auto evidence = row["evidence"].toObject();
                    evidence["textures"] = textures;
                    row["evidence"] = evidence;
                }
                inspector.add(row);
            }
            for (const auto &note : unsupported)
                inspector.add({{"type", "selects_model"}, {"status", "unsupported"}, {"from", path}, {"reason", note.left(200)}});
        }
    } else if (kind == "equipment") {
        const auto json = QJsonDocument::fromJson(sources.read(path, 256 * 1024)).object();
        const auto layers = json["layers"].toObject();
        if (!sources.provided(path))
            inspector.add({{"type", "equipment_layer_texture"}, {"status", "unresolved"}, {"from", path},
                           {"reason", "No source provides this equipment definition."}});
        for (auto it = layers.begin(); it != layers.end(); ++it) {
            const auto entries = it->toArray();
            for (qsizetype i = 0; i < entries.size(); ++i) {
                const auto entry = entries[i].toObject();
                const auto texture = mc::Location::parse(entry["texture"].toString());
                if (!texture.valid()) {
                    if (entry["use_player_texture"].toBool())
                        inspector.add({{"type", "equipment_layer_texture"}, {"status", "confirmed"}, {"from", path},
                                       {"evidence", QJsonObject{{"pointer", QString("/layers/%1/%2").arg(escape(it.key())).arg(i)},
                                                                {"usePlayerTexture", true}}},
                                       {"reason", "The layer uses the player's own texture, not a pack file."}});
                    continue;
                }
                const auto target = QString("assets/%1/textures/entity/equipment/%2/%3.png")
                                        .arg(texture.nameSpace, it.key(), texture.path);
                const bool verified = s.version == EquipmentVerified;
                inspector.add({{"type", "equipment_layer_texture"},
                    {"status", !inspector.available(target) ? QString("unresolved") : verified ? QString("confirmed") : QString("possible")},
                    {"from", path}, {"to", target}, {"source", inspector.source(target)},
                    {"evidence", QJsonObject{{"pointer", QString("/layers/%1/%2/texture").arg(escape(it.key())).arg(i)},
                                             {"texture", texture.id()}, {"layer", it.key()},
                                             {"convention", QString("textures/entity/equipment/<layer>/<texture>.png, verified "
                                                                    "for Java %1").arg(EquipmentVerified)}}},
                    {"reason", !inspector.available(target) ? QString("No source provides the worn texture.")
                               : verified ? QString() : QString("The path convention is verified by Nova for Java %1 only.").arg(EquipmentVerified)}});
            }
        }
    } else {
        inspector.limitations.append(QString("Nova does not analyse relationships of %1 resources.").arg(kind));
    }
    if (cancel->load())
        return toolError("operation_canceled", "The operation was canceled.");
    // Drop empty reason fields so a reader can rely on reason meaning "not established".
    QJsonArray rows;
    for (const auto &value : inspector.relations) {
        auto row = value.toObject();
        if (row["reason"].toString().isEmpty())
            row.remove("reason");
        rows.append(row);
    }
    if (!sources.vanillaModelSource())
        inspector.limitations.append(QString("No verified vanilla resource source is configured for Java %1, so vanilla "
            "models, item definitions, blockstates and equipment are unavailable. Nova's vanilla texture cache contains "
            "textures only and is never used as a model source.").arg(s.version));
    if (!sources.catalogComplete())
        inspector.limitations.append("Nova's model catalog budget (8192 JSON files / 32 MiB) was exceeded, so the "
                                     "configured sources were read only in part; absent models are reported as unknown.");
    for (const auto &entry : sources.stack())
        if (entry.toObject()["status"] != "loaded")
            inspector.limitations.append(QString("Resource stack source %1 is unavailable (%2); it was not used.")
                .arg(entry.toObject()["stackIndex"].toInt()).arg(entry.toObject()["reason"].toString().left(120)));
    inspector.limitations.append("Relationships follow Nova's model resolver; they are evidence, not a guarantee of "
                                 "in-game rendering.");
    const auto providers = sources.providers(path);
    QJsonArray providerRows;
    for (qsizetype i = 0; i < providers.size(); ++i) {
        auto row = providers[i].json();
        row["selected"] = i == 0;
        providerRows.append(row);
    }
    if (providers.isEmpty() && sources.vanillaTexture(path))
        providerRows.append(QJsonObject{{"source", "vanilla_texture_cache"}, {"verified", true}, {"selected", true}});
    return {{"generation", s.generation}, {"projectId", s.projectId}, {"path", path}, {"kind", kind},
            {"minecraftVersion", s.version}, {"versionSource", args.contains("version") ? "argument" : "project"},
            {"found", !providerRows.isEmpty()}, {"providers", providerRows}, {"relationships", rows},
            {"detail", detail}, {"sources", sources.summary()}, {"limitations", inspector.limitations},
            {"truncated", inspector.truncated}};
}

QJsonArray relationshipFindings(const Snapshot &s, SourceSet &sources, const QString &only,
                                const std::shared_ptr<std::atomic_bool> &cancel, QJsonArray &limitations) {
    QJsonArray findings;
    auto finding = [&](const QString &severity, const QString &rule, const QString &path, const QString &source,
                       const QString &message, const QString &next, const QJsonArray &evidence = {},
                       const QString &field = {}) {
        QJsonObject row{{"severity", severity}, {"code", rule}, {"rule", rule}, {"path", path.isEmpty() ? QJsonValue() : QJsonValue(path)},
                        {"source", source}, {"message", message}, {"nextStep", next}, {"evidence", evidence},
                        {"field", field}, {"line", 0}};
        if (source == "open_draft" && s.drafts.contains(path))
            row["revision"] = s.drafts.value(path).revision;
        findings.append(row);
    };
    auto scoped = [&](const QString &path) { return only.isEmpty() || path == only; };
    // 1. Configured resource-stack sources that could not be used.
    for (const auto &value : sources.stack()) {
        const auto entry = value.toObject();
        if (entry["status"] == "loaded" || !only.isEmpty())
            continue;
        finding("Warning", "resource_stack_unavailable", {}, "resource_stack",
                QString("Resource stack source %1 (%2) is configured but unavailable: %3. Its files were not validated or "
                        "used to resolve references.").arg(entry["stackIndex"].toInt()).arg(entry["label"].toString(),
                        entry["reason"].toString()),
                "Ask the user to check the folder in Models › Resource stack, then validate again.",
                {QJsonObject{{"stackIndex", entry["stackIndex"]}, {"label", entry["label"]}, {"reason", entry["reason"]}}});
    }
    // 2. Open drafts whose file changed on disk after they were opened or saved.
    for (auto it = s.drafts.cbegin(); it != s.drafts.cend(); ++it) {
        if (cancel->load() || !scoped(it.key()) || it->provisional)
            continue;
        // Only the project's own file, through the authorized scope; missing and unreadable differ.
        auto &scope = sources.projectScope();
        const auto file = scope.open(it.key());
        if (!file && !scope.vacant(it.key())) {
            limitations.append(QString("The saved file of the open draft %1 could not be read, so Nova could not compare "
                                       "it with the draft.").arg(it.key()));
            continue;
        }
        if (it->savedExists && it->savedHash.isEmpty()) {
            limitations.append(QString("The open draft %1 was opened from a file Nova could not hash, so a draft/disk "
                                       "comparison is not possible.").arg(it.key()));
            continue;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const bool exists = bool(file);
        if (exists && (file->size() > 128LL * 1024 * 1024 || !hash.addData(file.get()))) {
            limitations.append(QString("The saved file of the open draft %1 is too large to compare.").arg(it.key()));
            continue;
        }
        if (exists == it->savedExists && (!exists || hash.result() == it->savedHash))
            continue;
        finding("Warning", "draft_disk_conflict", it.key(), "open_draft",
                exists ? QString("The file on disk changed after this %1 was opened or last saved; the open draft and the "
                                 "saved file now differ independently.").arg(it->type)
                       : QString("The file this %1 was opened from is no longer on disk.").arg(it->type),
                "Ask the user to review both versions in Nova before saving; nova.drafts.save refuses to overwrite an "
                "external change.",
                {QJsonObject{{"draftRevision", it->revision}, {"diskChanged", exists}, {"diskMissing", !exists}}});
    }
    // 3. Index entries that no longer match the file (size, time or presence).
    int checked = 0;
    for (const auto &path : s.index.files) {
        if (cancel->load())
            break;
        if (!logicalAsset(path) || s.provisional.contains(path) || !scoped(path))
            continue;
        if (++checked > 20000) {
            limitations.append("Index freshness was checked for the first 20,000 assets only.");
            break;
        }
        const auto &record = s.index.records.value(path);
        // Presence and size through the authorized scope; the time from the same, already contained, file.
        const auto file = sources.projectScope().open(path);
        const bool missing = !file;
        const auto now = missing ? AssetIdentity{} : AssetIdentity::fromPath(s.root, path);
        if (!missing && file->size() == record.size && now.modified == record.modified)
            continue;
        finding("Warning", "stale_index_entry", path, "disk",
                missing ? QString("Nova's index lists this file but it is no longer on disk or cannot be opened safely.")
                        : QString("The file changed on disk after Nova indexed it; this report may use outdated data."),
                "Wait for Nova to finish re-indexing (or ask the user to refresh the project), then validate again.",
                {QJsonObject{{"indexedBytes", record.size}, {"diskBytes", missing ? QJsonValue() : QJsonValue(file->size())},
                             {"modifiedChanged", !missing && now.modified != record.modified}, {"missing", missing}}});
    }
    auto &library = sources.library();
    const bool modelData = sources.vanillaModelSource();
    if (!sources.catalogComplete())
        limitations.append("Nova's model catalog budget (8192 JSON files / 32 MiB) was exceeded, so model data is "
                           "incomplete: unreferenced textures were not reported and absent models are not errors.");
    // 4. Project block/item textures that no resolved model references.
    int unknown = 0;
    for (const auto &path : s.index.files) {
        if (cancel->load() || !sources.catalogComplete())
            break;
        if (resourceKind(path) != "texture" || s.provisional.contains(path) || !scoped(path))
            continue;
        const auto location = mc::Location::parse(mc::Location::fromAsset(path, "textures"));
        if (!location.valid() || !(location.path.startsWith("block/") || location.path.startsWith("item/")))
            continue;
        if (!library.dependents(path).isEmpty())
            continue;
        if (location.nameSpace == "minecraft" && !modelData) {
            ++unknown; // vanilla models that may use it are unavailable: no claim either way
            continue;
        }
        finding("Warning", "unreferenced_texture", path, "disk",
                "No model in the project, its open drafts or the configured resource stack references this texture, so "
                "Nova cannot show where it is used. It may still be used directly by game code.",
                "If it should appear on a block or item, reference it from a model's textures; otherwise ignore this warning.",
                {QJsonObject{{"searched", QJsonArray{"project", "open_draft", "resource_stack"}},
                             {"vanillaModelSource", modelData}}});
    }
    if (unknown)
        limitations.append(QString("%1 minecraft block/item texture(s) are not referenced by any project or stack model; "
                                   "without a verified vanilla resource source Nova cannot tell whether vanilla models use "
                                   "them, so they were not reported.").arg(unknown));
    // 5. UVs of the project's own model elements (drafts included).
    for (const auto &path : s.index.files) {
        if (cancel->load())
            break;
        if (resourceKind(path) != "model" || !scoped(path))
            continue;
        const auto source = library.source(path);
        const auto elements = source["elements"].toArray();
        const auto origin = s.drafts.contains(path) ? QString("open_draft") : QString("disk");
        int reported = 0;
        for (qsizetype i = 0; i < elements.size() && i < 4096 && reported < 16; ++i) {
            const auto faces = elements[i].toObject()["faces"].toObject();
            for (auto f = faces.begin(); f != faces.end() && reported < 16; ++f) {
                const auto uv = f->toObject()["uv"].toArray();
                if (uv.size() != 4)
                    continue;
                bool outside = false;
                for (const auto &v : uv)
                    outside |= v.toDouble() < 0 || v.toDouble() > 16;
                const bool flat = uv[0].toDouble() == uv[2].toDouble() || uv[1].toDouble() == uv[3].toDouble();
                if (!outside && !flat)
                    continue;
                ++reported;
                const auto pointer = QString("/elements/%1/faces/%2/uv").arg(i).arg(f.key());
                finding("Warning", outside ? "uv_outside_texture" : "uv_zero_area", path, origin,
                        outside ? QString("Face %1 of element %2 has UV coordinates outside 0–16, the texture's extent in "
                                          "model units. Nova does not verify how Minecraft treats them.").arg(f.key()).arg(i)
                                : QString("Face %1 of element %2 has a zero-width or zero-height UV region, so it samples a "
                                          "single row or column of the texture.").arg(f.key()).arg(i),
                        "Check the face in the UV editor (nova.asset.preview_image view \"uv\").",
                        {QJsonObject{{"pointer", pointer}, {"uv", uv}, {"texture", f->toObject()["texture"]}}}, pointer);
            }
        }
    }
    // 6. Armor companions: an icon changed without its worn texture (suggestions; 1.21.11 naming only).
    if (s.version == EquipmentVerified) {
        for (const auto &path : s.index.files) {
            if (cancel->load())
                break;
            const auto location = mc::Location::parse(mc::Location::fromAsset(path, "textures"));
            if (!location.valid() || !location.path.startsWith("item/") || !scoped(path))
                continue;
            for (const auto &piece : ArmorPieces) {
                const auto name = location.path.mid(5);
                if (!name.endsWith('_' + piece))
                    continue;
                const auto material = name.chopped(piece.size() + 1);
                const auto layer = piece == "leggings" ? QString("humanoid_leggings") : QString("humanoid");
                const auto worn = QString("assets/%1/textures/entity/equipment/%2/%3.png").arg(location.nameSpace, layer, material);
                if (s.index.records.contains(worn))
                    continue;
                finding("Information", "companion_suggestion", path, s.drafts.contains(path) ? "open_draft" : "disk",
                        QString("Suggestion, not a confirmed relationship: this pack changes the %1 icon but not %2, which is "
                                "usually its worn texture. Equipment links are game data Nova cannot read.").arg(name, worn),
                        "If the armor should also look different when worn, edit the worn texture too.",
                        {QJsonObject{{"confidence", "suggestion"}, {"companion", worn},
                                     {"convention", QString("verified naming for Java %1").arg(EquipmentVerified)}}});
            }
        }
    } else {
        limitations.append(QString("Armor companion suggestions use a naming convention Nova verified for Java %1 only; "
                                   "they were not produced for %2.").arg(EquipmentVerified, s.version));
    }
    return findings;
}
} // namespace nova::mcp
