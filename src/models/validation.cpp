// SPDX-License-Identifier: GPL-3.0-or-later
#include "validation.h"
#include "edit.h"
#include "minecraft_animation.h"
#include "project.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <cmath>
namespace nova::mc {
QString severityName(Severity s) {
    switch (s) {
    case Severity::Error:
        return "Error";
    case Severity::Warning:
        return "Warning";
    case Severity::Information:
        return "Information";
    case Severity::Unsupported:
        return "Unsupported";
    }
    return {};
}
QJsonObject Diagnostic::json() const {
    return {{"severity", severityName(severity)},
            {"code", code},
            {"file", file},
            {"field", field},
            {"line", line},
            {"explanation", message},
            {"suggested_action", suggestion},
            {"affected_dependencies", QJsonArray::fromStringList(affected)},
            {"repair_available", !replacement.isEmpty()}};
}
QJsonObject ValidationReport::json() const {
    QJsonArray rows;
    QJsonObject counts;
    for (const auto &d : diagnostics) {
        rows.append(d.json());
        auto key = severityName(d.severity);
        counts[key] = counts[key].toInt() + 1;
    }
    return {
        {"schema", 1},
        {"project", project},
        {"target_version", version},
        {"scope", scope},
        {"time_utc", time.toString(Qt::ISODateWithMs)},
        {"files_in_scope", files},
        {"files_inspected", inspected},
        {"cached_results", reused},
        {"milliseconds", milliseconds},
        {"canceled", canceled},
        {"truncated", truncated},
        {"counts", counts},
        {"diagnostics", rows},
        {"limitations",
         "Validation covers supported resource constructs and cannot certify in-game rendering."}};
}
QString ValidationReport::text() const {
    QString s = QString("Nova Studio resource validation\nProject: %1\nTarget: %2\nUTC: %3\nScope: "
                        "%4\nFiles: %5 (%6 inspected, %7 cached)\nElapsed: %8 ms\n%9%10\n")
                    .arg(project, version, time.toString(Qt::ISODateWithMs), scope)
                    .arg(files)
                    .arg(inspected)
                    .arg(reused)
                    .arg(milliseconds)
                    .arg(canceled ? "Canceled — partial results\n" : "",
                         truncated ? "Budget exceeded — partial results\n" : "");
    for (const auto &d : diagnostics)
        s += severityName(d.severity) + " | " + d.file + " " + d.field + " | " + d.message +
             "\nAction: " + d.suggestion + "\n";
    return s + "\nSupported-construct validation cannot certify identical Minecraft rendering.\n";
}
namespace {
bool selected(const QString &f, const QStringList &scope) {
    if (scope.isEmpty())
        return true;
    for (const auto &s : scope)
        if (f == s || f.startsWith(s + '/'))
            return true;
    return false;
}
QString sprite(const QJsonValue &v) {
    return v.isString() ? v.toString() : v.toObject()["sprite"].toString();
}
bool jsonKind(const QString &f) { return f.endsWith(".json") || f.endsWith(".mcmeta"); }
} // namespace
ValidationReport ValidationEngine::run(const QString &root, const Index &index, Library library,
                                       const QStringList &scope, const QStringList &changed,
                                       const std::shared_ptr<std::atomic_bool> &cancel,
                                       const std::function<void(int, int)> &progress,
                                       const QMap<QString, QJsonObject> &documents,
                                       const Library::AssetReader &reader) {
    QElapsedTimer timer;
    timer.start();
    ValidationReport report;
    report.project = root;
    report.version = library.version();
    report.time = QDateTime::currentDateTimeUtc();
    report.scope = scope.isEmpty() ? "Entire resource pack" : scope.join(", ");
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (project != root || version != library.version()) {
        cache.clear();
        cacheDiagnosticBytes = 0;
        project = root;
        version = library.version();
    }
    QSet<QString> dirty;
    for (auto it = documents.begin(); it != documents.end(); ++it) {
        dirty.insert(it.key());
        cache.remove(it.key());
        library.previewSource(it.key(), it.value());
        for (const auto &d : library.dependents(it.key())) {
            dirty.insert(d);
            cache.remove(d);
        }
    }
    for (const auto &f : changed) {
        dirty.insert(f);
        for (const auto &d : library.dependents(f))
            dirty.insert(d);
        if (f.isEmpty())
            cache.clear();
    }
    // Parent/texture additions, removals and edits invalidate cached dependent diagnostics.
    for (auto it = cache.cbegin(); it != cache.cend(); ++it) {
        const auto current = index.records.constFind(it.key());
        if (current == index.records.cend() || current->size != it->size ||
            current->modified != it->modified) {
            dirty.insert(it.key());
            for (const auto &d : library.dependents(it.key()))
                dirty.insert(d);
        }
    }
    for (const auto &file : index.files)
        if (!cache.contains(file))
            for (const auto &d : library.dependents(file))
                dirty.insert(d);
    for (auto it = cache.begin(); it != cache.end();) {
        if (!index.records.contains(it.key()))
            it = cache.erase(it);
        else
            ++it;
    }
    // Texture-name index supports an unambiguous deterministic missing-path proposal.
    QMap<QString, QStringList> textures;
    for (const auto &f : index.files)
        if (!Location::fromAsset(f, "textures").isEmpty())
            textures[QFileInfo(f).completeBaseName()] << f;
    QSet<QString> folded;
    int ordinal = 0;
    int repairCount = 0;
    for (const auto &file : index.files) {
        if (cancel && cancel->load()) {
            report.canceled = true;
            break;
        }
        if (!selected(file, scope) || file.startsWith(".nova/"))
            continue;
        ++report.files;
        if (progress && (ordinal++ % 128 == 0))
            progress(ordinal, index.files.size());
        const auto record = index.records.value(file);
        QVector<Diagnostic> rows;
        QStringList affected;
        auto add = [&](Severity s, const QString &code, const QString &field,
                       const QString &message, const QString &action) {
            if (rows.size() < 128) {
                Diagnostic d;
                d.severity = s;
                d.code = code;
                d.file = file;
                d.field = field;
                d.message = message;
                d.suggestion = action;
                if (rows.isEmpty())
                    affected = library.dependents(file).mid(0, 256);
                d.affected = affected;
                rows.append(d);
            }
        };
        auto previous = cache.constFind(file);
        if (previous != cache.cend() && !dirty.contains(file) && previous->size == record.size &&
            previous->modified == record.modified) {
            rows = previous->diagnostics;
            ++report.reused;
        } else {
            ++report.inspected;
            const QFileInfo info(reader ? QString() : root + '/' + file);
            const auto abs = reader ? QString() : info.canonicalFilePath();
            // Read-only validation resolves each final path once. Persistence retains
            // the stricter per-ancestor link/device checks in safeDestination.
            const bool needsBytes = jsonKind(file) || file.endsWith(".png");
            auto guarded = reader && needsBytes ? reader(file) : nullptr;
            if (reader ? (needsBytes && !guarded)
                       : (canonicalRoot.isEmpty() || abs.isEmpty() || info.isSymLink() ||
                          info.isJunction() || !abs.startsWith(canonicalRoot + '/', Qt::CaseInsensitive)))
                add(Severity::Error, "unsafe_path", "", "Missing, linked or escaped asset",
                    "Use an ordinary file inside the pack");
            else {
                const auto parts = file.split('/');
                if (parts.size() > 3 && parts[0] == "assets" &&
                    !Location::parse(parts[1] + ':' + parts.mid(3).join('/')).valid())
                    add(Severity::Error, "resource_path", "", "Invalid namespace or resource path",
                        "Use lowercase resource identifiers without traversal");
                if (jsonKind(file)) {
                    QFile f(abs);
                    QByteArray bytes;
                    QJsonParseError parse;
                    QJsonDocument doc;
                    Revision rev;
                    QIODevice *input = guarded.get();
                    if (!reader) {
                        rev = Revision::read(abs, 2 * 1024 * 1024);
                        if (rev.readable && f.open(QIODevice::ReadOnly))
                            input = &f;
                    }
                    if (!input || input->size() > 2 * 1024 * 1024)
                        add(Severity::Unsupported, "json_budget", "",
                            "JSON unreadable or exceeds 2 MiB",
                            "Inspect the source; reduce unsupported document size");
                    else {
                        bytes = input->read(2 * 1024 * 1024 + 1);
                        doc = QJsonDocument::fromJson(bytes, &parse);
                        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
                            add(Severity::Error, "malformed_json", "",
                                "Malformed JSON: " + parse.errorString(),
                                "Correct JSON syntax in the source file");
                            if (!rows.isEmpty())
                                rows.last().line = bytes.left(parse.offset).count('\n') + 1;
                        } else {
                            const bool inMemory = documents.contains(file);
                            const auto o = inMemory ? documents.value(file) : doc.object();
                            if (inMemory)
                                add(Severity::Information, "unsaved_model", "",
                                    "Validating in-memory model changes",
                                    "Save or cancel changes before applying disk repairs");
                            const auto modelId = Location::fromAsset(file, "models");
                            if (!modelId.isEmpty()) {
                                const auto m = library.model(modelId);
                                auto messages = mesh(m).diagnostics;
                                messages.removeDuplicates();
                                for (const auto &msg : messages) {
                                    // This exact mesh notice describes preview capability, not pack data.
                                    if (msg == "Generated item: flat layers; contour extrusion is not reproduced") {
                                        add(Severity::Information, "preview_limitation", "",
                                            "Preview limitation: Nova's 3D preview represents this generated-item "
                                            "model as flat layers without contour extrusion and cannot faithfully "
                                            "render it. The model may be valid; this finding does not indicate "
                                            "invalid pack data.",
                                            "Check the model's appearance in Minecraft; Nova's preview cannot "
                                            "establish rendering parity.");
                                        continue;
                                    }
                                    auto severity =
                                        msg.contains("Unsupported", Qt::CaseInsensitive) ||
                                                msg.contains("retained") || msg.contains("omits")
                                            ? Severity::Unsupported
                                            : Severity::Error;
                                    if (msg.contains("removed") ||
                                        msg.contains("unsupported shade"))
                                        severity = Severity::Warning;
                                    add(severity, "model", "", msg,
                                        "Inspect model fields, parent chain and configured "
                                        "resource stack");
                                }
                                auto bindings = o["textures"].toObject();
                                for (auto t = bindings.begin(); t != bindings.end(); ++t) {
                                    const auto ref = sprite(t.value());
                                    const auto id = Location::parse(ref);
                                    if (!id.valid() ||
                                        !library.assetPath(id.asset("textures")).isEmpty())
                                        continue;
                                    const auto candidates =
                                        textures.value(QFileInfo(id.path).fileName());
                                    if (!inMemory && candidates.size() == 1 && rows.size() < 128 &&
                                        repairCount < 8) {
                                        auto replacement = o, map = bindings;
                                        auto old = t.value();
                                        const auto corrected =
                                            Location::fromAsset(candidates.first(), "textures");
                                        if (old.isObject()) {
                                            auto entry = old.toObject();
                                            entry["sprite"] = corrected;
                                            map[t.key()] = entry;
                                        } else
                                            map[t.key()] = corrected;
                                        for (auto binding = bindings.begin();
                                             binding != bindings.end(); ++binding)
                                            if (sprite(binding.value()) == ref) {
                                                if (binding.value().isObject()) {
                                                    auto entry = binding.value().toObject();
                                                    entry["sprite"] = corrected;
                                                    map[binding.key()] = entry;
                                                } else
                                                    map[binding.key()] = corrected;
                                            }
                                        replacement["textures"] = map;
                                        add(Severity::Error, "texture_path", "/textures/" + t.key(),
                                            "Missing texture " + ref,
                                            "Replace identical references with sole basename "
                                            "match " +
                                                corrected);
                                        rows.last().replacement = replacement;
                                        rows.last().revision = rev;
                                        ++repairCount;
                                    }
                                }
                                auto elements = o["elements"].toArray();
                                bool literal = false;
                                for (const auto &v : elements) {
                                    const auto faces = v.toObject()["faces"].toObject();
                                    for (auto it = faces.begin(); it != faces.end(); ++it)
                                        if (!it.value().toObject()["texture"].toString().startsWith(
                                                '#'))
                                            literal = true;
                                }
                                if (literal && rows.size() < 128) {
                                    add(Severity::Error, "face_texture_reference", "/elements",
                                        "Face textures must use #aliases rather than literal "
                                        "resource paths",
                                        "Create equivalent texture bindings and reference them "
                                        "with #aliases");
                                    if (!inMemory && repairCount < 8) {
                                        auto replacement = o;
                                        QString error;
                                        if (normalizeFaceReferences(&replacement, &error)) {
                                            rows.last().replacement = replacement;
                                            rows.last().revision = rev;
                                            ++repairCount;
                                        }
                                    }
                                }
                                for (int i = 0; i < elements.size() && i < 4096; ++i) {
                                    auto e = elements[i].toObject();
                                    if (!inMemory && library.version() == "26.3" &&
                                        rows.size() < 128 && repairCount < 8 &&
                                        e.contains("shade") && e["shade"].isBool()) {
                                        auto replacement = o, changedElement = e;
                                        auto changedElements = elements;
                                        changedElement.remove("shade");
                                        if (!e["shade"].toBool())
                                            changedElement["shade_direction_override"] = "up";
                                        changedElements[i] = changedElement;
                                        replacement["elements"] = changedElements;
                                        add(Severity::Warning, "shade_migration",
                                            QString("/elements/%1/shade").arg(i),
                                            "shade was removed for target 26.3",
                                            "Remove shade:true; replace shade:false with "
                                            "shade_direction_override:up");
                                        rows.last().replacement = replacement;
                                        rows.last().revision = rev;
                                        ++repairCount;
                                    }
                                }
                            } else if (!Location::fromAsset(file, "blockstates").isEmpty() ||
                                       !Location::fromAsset(file, "items").isEmpty()) {
                                const bool item = !Location::fromAsset(file, "items").isEmpty();
                                int nodes = 0;
                                std::function<void(const QJsonValue &, const QString &, int)>
                                    inspect;
                                inspect = [&](const QJsonValue &v, const QString &field,
                                              int depth) {
                                    if (depth > 64 || ++nodes > 16384) {
                                        if (nodes == 16385)
                                            add(Severity::Unsupported, "definition_budget", field,
                                                "Definition traversal budget exceeded",
                                                "Simplify the definition");
                                        return;
                                    }
                                    if (v.isArray()) {
                                        int n = 0;
                                        for (const auto &value : v.toArray())
                                            inspect(value, field + '/' + QString::number(n++),
                                                    depth + 1);
                                    }
                                    if (!v.isObject())
                                        return;
                                    const auto object = v.toObject();
                                    if (object["model"].isString()) {
                                        if (!Location::parse(object["model"].toString()).valid())
                                            add(Severity::Error, "invalid_reference",
                                                field + "/model",
                                                "Invalid model resource identifier",
                                                "Choose a valid namespace:path");
                                        if (!item)
                                            for (const auto &axis : {"x", "y", "z"})
                                                if (object.contains(axis)) {
                                                    const auto a = object[axis];
                                                    if (!a.isDouble() ||
                                                        a.toDouble() != a.toInt() ||
                                                        a.toInt() < 0 || a.toInt() > 270 ||
                                                        a.toInt() % 90)
                                                        add(Severity::Error, "blockstate_rotation",
                                                            field + '/' + axis,
                                                            "Blockstate rotations must be "
                                                            "0/90/180/270",
                                                            "Correct this alternative's rotation");
                                                }
                                        if (!item && object.contains("uvlock") &&
                                            !object["uvlock"].isBool())
                                            add(Severity::Error, "blockstate_uvlock",
                                                field + "/uvlock", "uvlock must be boolean",
                                                "Set true or false");
                                    }
                                    for (auto it = object.begin(); it != object.end(); ++it)
                                        inspect(it.value(), field + '/' + it.key(), depth + 1);
                                };
                                inspect(o, "", 0);
                                if (item && !o["model"].isObject())
                                    add(Severity::Error, "item_definition", "/model",
                                        "Item definition requires a model object",
                                        "Add a supported item model dispatch object");
                                if (!item && !o["variants"].isObject() && !o["multipart"].isArray())
                                    add(Severity::Error, "blockstate", "",
                                        "Blockstate needs variants or multipart",
                                        "Create a supported blockstate definition");
                                for (const auto &dep : library.dependencies(file))
                                    if (library.assetPath(dep).isEmpty())
                                        add(Severity::Error, "missing_model", "",
                                            "Missing referenced model: " + dep,
                                            "Create the model or configure its fallback source");
                                for (const auto &msg : library.dispatch(file).diagnostics)
                                    if (!msg.startsWith("Manual "))
                                        add(msg.contains("Unsupported", Qt::CaseInsensitive)
                                                ? Severity::Unsupported
                                                : Severity::Error,
                                            "definition", "", msg,
                                            "Inspect the definition and selected target profile");
                            }
                            if (file == "pack.mcmeta") {
                                const auto message = Project::validateMetadata(bytes);
                                if (!message.isEmpty())
                                    add(Severity::Error, "pack_format", "/pack", message,
                                        "Correct the metadata through Project Properties");
                                else {
                                    const auto pack = o["pack"].toObject();
                                    const auto lo = PackFormat::parse(pack.contains("min_format")
                                                                          ? pack["min_format"]
                                                                          : pack["pack_format"]);
                                    const auto hi = PackFormat::parse(pack.contains("max_format")
                                                                          ? pack["max_format"]
                                                                          : pack["pack_format"],
                                                                      true);
                                    for (const auto &v : gameVersions())
                                        if (v.id == library.version() && lo && hi &&
                                            (v.format < *lo || v.format > *hi))
                                            add(Severity::Warning, "pack_target", "/pack",
                                                "Pack format range excludes selected Minecraft "
                                                "target",
                                                "Choose a compatible target or explicitly update "
                                                "metadata");
                                }
                            }
                            if (file.endsWith(".png.mcmeta") && o.contains("animation")) {
                                const auto sheet = abs.left(abs.size() - 7);
                                auto imageFile = reader ? reader(file.chopped(7)) : nullptr;
                                QImageReader imageReader;
                                imageReader.setFormat("PNG");
                                if (reader) {
                                    imageReader.setAutoDetectImageFormat(false);
                                    imageReader.setDevice(imageFile.get());
                                } else
                                    imageReader.setFileName(sheet);
                                const auto size = imageReader.size();
                                if (reader ? !imageFile : !QFileInfo::exists(sheet))
                                    add(Severity::Error, "missing_spritesheet", "/animation",
                                        "Animation spritesheet is missing",
                                        "Restore the PNG or remove obsolete metadata explicitly");
                                else if (!size.isValid() ||
                                         qint64(size.width()) * size.height() * 4 >
                                             64 * 1024 * 1024)
                                    add(Severity::Unsupported, "animation_budget", "/animation",
                                        "Spritesheet exceeds validation decode budget or has "
                                        "invalid dimensions",
                                        "Inspect dimensions; maximum decoded validation buffer is "
                                        "64 MiB");
                                else {
                                    AnimationState state;
                                    QString message;
                                    if (!parseAnimation(imageReader.read(), o, library.version(), &state,
                                                        &message))
                                        add(Severity::Error, "animation", "/animation", message,
                                            "Correct frame size, indices and timing");
                                }
                            }
                        }
                    }
                } else if (file.endsWith(".png")) {
                    QImageReader imageReader;
                    imageReader.setFormat("PNG");
                    if (reader) {
                        imageReader.setAutoDetectImageFormat(false);
                        imageReader.setDevice(guarded.get());
                    } else
                        imageReader.setFileName(abs);
                    const auto s = imageReader.size();
                    if (!s.isValid())
                        add(Severity::Error, "texture_header", "", "Invalid PNG header",
                            "Restore or re-export the texture");
                    else if (s.width() > 4096 || s.height() > 4096)
                        add(Severity::Warning, "texture_size", "", "Large texture dimensions",
                            "Check expected texture resolution and animation layout");
                }
            }
            bool repairable = false;
            qsizetype cost = 0;
            for (const auto &d : rows) {
                repairable |= !d.replacement.isEmpty();
                cost += 2 * (d.message.size() + d.suggestion.size() + d.field.size() +
                             d.affected.join(',').size()) +
                        256;
            }
            // Proposals are re-read next run so cached repairs cannot be presented as fresh.
            if (!repairable && !documents.contains(file) && cache.size() < 65536) {
                if (cacheDiagnosticBytes + cost > 32 * 1024 * 1024) {
                    cache.clear();
                    cacheDiagnosticBytes = 0;
                }
                cache[file] = {record.size, record.modified, rows};
                cacheDiagnosticBytes += cost;
            }
        }
        if (folded.contains(file.toCaseFolded()))
            add(Severity::Error, "duplicate_asset", "", "Case-conflicting asset path",
                "Rename through explicit resource references");
        folded.insert(file.toCaseFolded());
        for (const auto &d : rows) {
            if (report.diagnostics.size() >= 10000) {
                report.truncated = true;
                break;
            }
            report.diagnostics.append(d);
        }
    }
    if ((reader ? !index.records.contains("pack.mcmeta") : !QFileInfo::exists(root + "/pack.mcmeta")) &&
        scope.isEmpty()) {
        Diagnostic d;
        d.file = "pack.mcmeta";
        d.code = "missing_pack_metadata";
        d.message = "Resource pack metadata is missing";
        d.suggestion = "Create pack metadata using Project Properties";
        report.diagnostics.append(d);
    }
    for (const auto &s : library.diagnostics) {
        if (report.diagnostics.size() >= 10000) {
            report.truncated = true;
            break;
        }
        Diagnostic d;
        d.severity = Severity::Unsupported;
        d.code = "catalog";
        d.message = s;
        d.suggestion = "Review resource stack and catalog budgets";
        report.diagnostics.append(d);
    }
    report.milliseconds = timer.elapsed();
    return report;
}
RepairResult applyRepair(const QString &root, const Diagnostic &d) {
    RepairResult r;
    QString path;
    if (d.replacement.isEmpty() || !d.revision.exists ||
        !safeDestination(root, d.file, &path, &r.error)) {
        r.error = "No safe deterministic repair";
        return r;
    }
    if (!d.revision.sameContent(Revision::read(path, 2 * 1024 * 1024))) {
        r.error = "External revision conflict; revalidate before repair";
        return r;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = "Cannot read original";
        return r;
    }
    const auto original = f.readAll();
    f.close();
    if (QCryptographicHash::hash(original, QCryptographicHash::Sha256) != d.revision.hash) {
        r.error = "External revision conflict";
        return r;
    }
    r.backup = ".nova/repairs/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
    QString backup;
    if (!safeDestination(root, r.backup, &backup, &r.error) ||
        !QDir().mkpath(QFileInfo(backup).absolutePath()) ||
        !atomicWrite(backup, original, Revision{}, &r.error))
        return r;
    r.applied = atomicWrite(path, QJsonDocument(d.replacement).toJson(), d.revision, &r.error);
    return r;
}
QStringList modelTextureScope(Library library, const QString &asset, bool family) {
    QStringList todo{asset};
    if (family)
        todo += library.dependents(asset);
    QSet<QString> seen, textures;
    while (!todo.isEmpty() && seen.size() < 16384) {
        auto p = todo.takeLast();
        if (seen.contains(p))
            continue;
        seen.insert(p);
        const auto id = Location::fromAsset(p, "models");
        if (!id.isEmpty())
            for (const auto &t : library.model(id).textures)
                if (!t.isEmpty())
                    textures.insert(t);
        for (const auto &d : library.dependencies(p)) {
            if (!Location::fromAsset(d, "textures").isEmpty())
                textures.insert(d);
            else
                todo << d;
        }
    }
    auto result = textures.values();
    result.sort();
    return result;
}
} // namespace nova::mc
