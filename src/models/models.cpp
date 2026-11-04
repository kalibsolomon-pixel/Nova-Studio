// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include "display.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace nova::mc {
namespace {
constexpr int MaxDepth = 64, MaxElements = 4096;
constexpr qint64 MaxJson = 2 * 1024 * 1024, MaxTotal = 32 * 1024 * 1024;
bool scoped(const QString &path, const QStringList &changes) {
    for (const auto &change : changes)
        if (change.isEmpty() || path == change || path.startsWith(change + '/'))
            return true;
    return false;
}
bool relevant(const QString &path) {
    return !Location::fromAsset(path, "models").isEmpty() ||
           !Location::fromAsset(path, "items").isEmpty() ||
           !Location::fromAsset(path, "blockstates").isEmpty();
}
QJsonObject readJson(QIODevice *file, QString &error) {
    if (!file || !file->isOpen() || file->size() > MaxJson) {
        error = "JSON missing, unreadable or exceeds 2 MiB";
        return {};
    }
    auto bytes = file->read(MaxJson + 1);
    // Bound nesting before Qt allocates a JSON tree. Respect quoted braces/escapes.
    int depth = 0;
    bool quoted = false, escape = false;
    for (char c : bytes) {
        if (escape) {
            escape = false;
            continue;
        }
        if (quoted && c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if ((c == '{' || c == '[') && ++depth > MaxDepth) {
            error = "Excessive JSON nesting";
            return {};
        }
        if (c == '}' || c == ']')
            --depth;
    }
    QJsonParseError parse;
    auto json = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) {
        error = "Malformed model JSON: " + parse.errorString();
        return {};
    }
    return json.object();
}
QString sprite(const QJsonValue &value) {
    return value.isString() ? value.toString() : value.toObject()["sprite"].toString();
}
QJsonObject merged(QJsonObject parent, const QJsonObject &child) {
    for (auto it = child.begin(); it != child.end(); ++it) {
        if (it.key() == "textures" || it.key() == "display") {
            auto properties = parent[it.key()].toObject();
            const auto additions = it.value().toObject();
            for (auto p = additions.begin(); p != additions.end(); ++p)
                properties[p.key()] = p.value();
            parent[it.key()] = properties;
        } else
            parent[it.key()] = it.value();
    }
    return parent;
}
void collectModels(const QJsonValue &v, QSet<QString> &out, int depth = 0) {
    if (depth > MaxDepth || out.size() > 8192)
        return;
    if (v.isArray())
        for (const auto &x : v.toArray())
            collectModels(x, out, depth + 1);
    if (v.isObject()) {
        const auto o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == "model" && it.value().isString()) {
                const auto id = Location::parse(it.value().toString());
                if (id.valid())
                    out.insert(id.asset("models"));
            } else
                collectModels(it.value(), out, depth + 1);
        }
    }
}
} // namespace
Profile Profile::forVersion(const QString &v) {
    if (v == "1.21.11")
        return {v, false, false, false};
    if (v == "26.1")
        return {v, true, false, true};
    if (v == "26.3")
        return {v, true, true, true};
    return {};
}
Location Location::parse(const QString &text) {
    if (text.isEmpty() || text.size() > 1024 || text.count(':') > 1)
        return {};
    auto parts = text.split(':');
    Location l{parts.size() == 1 ? "minecraft" : parts[0], parts.last()};
    static const QRegularExpression ns("^[a-z0-9_.-]+$"), path("^[a-z0-9_./-]+$");
    if (!ns.match(l.nameSpace).hasMatch() || !path.match(l.path).hasMatch())
        return {};
    for (const auto &part : l.path.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return {};
    return l;
}
QString Location::asset(const QString &kind) const {
    return valid() ? "assets/" + nameSpace + '/' + kind + '/' + path +
                         (kind == "textures" ? ".png" : ".json")
                   : QString();
}
QString Location::fromAsset(const QString &asset, const QString &kind) {
    auto p = asset.split('/');
    const QString suffix = kind == "textures" ? ".png" : ".json";
    if (p.size() < 4 || p[0] != "assets" || p[2] != kind || !asset.endsWith(suffix))
        return {};
    auto path = p.mid(3).join('/');
    path.chop(suffix.size());
    return parse(p[1] + ':' + path).id();
}
void Library::setVersion(const QString &version) {
    const auto next = Profile::forVersion(version);
    if (profile.version == next.version)
        return;
    profile = next;
    cache.clear();
    if (!root.isEmpty())
        update(root, editableSnapshot, {""});
}
void Library::setFallbacks(QVector<ResourceSource> next, const QString &version) {
    if (!version.isEmpty())
        profile = Profile::forVersion(version);
    fallbacks = std::move(next);
    records.clear();
    sources.clear();
    failures.clear();
    forward.clear();
    reverse.clear();
    cache.clear();
    if (!root.isEmpty())
        update(root, editableSnapshot, {""});
}
void Library::releaseModelSource(const QString &asset) {
    if (provisionalSources.remove(asset) && !records.contains(asset)) {
        for (const auto &path : dependents(asset))
            cache.remove(Location::fromAsset(path, "models"));
        sources.remove(asset);
        replaceEdges(asset);
        cache.remove(Location::fromAsset(asset, "models"));
    }
    refreshLocal({asset});
}
void Library::setProvisionalSource(const QString &asset, const QJsonObject &value) {
    if (Location::fromAsset(asset, "models").isEmpty())
        return;
    provisionalSources[asset] = value;
    previewSource(asset, value);
}
void Library::previewSource(const QString &asset, const QJsonObject &value) {
    if (!editable(asset))
        return;
    auto invalid = dependents(asset);
    invalid << asset;
    if (provisionalSources.contains(asset))
        provisionalSources[asset] = value;
    sources[asset] = value;
    replaceEdges(asset);
    for (const auto &path : invalid)
        cache.remove(Location::fromAsset(path, "models"));
}
void Library::replaceEdges(const QString &path) {
    for (const auto &target : forward.take(path)) {
        reverse[target].remove(path);
        if (reverse[target].isEmpty())
            reverse.remove(target);
    }
    const auto o = sources.value(path);
    QSet<QString> edges;
    if (!Location::fromAsset(path, "models").isEmpty()) {
        auto p = Location::parse(o["parent"].toString());
        if (p.valid() && !p.id().startsWith("minecraft:builtin/"))
            edges.insert(p.asset("models"));
        const auto textures = o["textures"].toObject();
        for (auto it = textures.begin(); it != textures.end(); ++it) {
            auto l = Location::parse(sprite(it.value()));
            if (l.valid())
                edges.insert(l.asset("textures"));
        }
        for (const auto &element : o["elements"].toArray()) {
            const auto faces = element.toObject()["faces"].toObject();
            for (auto f = faces.begin(); f != faces.end(); ++f) {
                auto l = Location::parse(f.value().toObject()["texture"].toString());
                if (l.valid())
                    edges.insert(l.asset("textures"));
            }
        }
    } else
        collectModels(o, edges);
    forward[path] = edges;
    for (const auto &target : edges)
        reverse[target].insert(path);
}
void Library::update(const QString &project, const Index &editableIndex, const QStringList &changed,
                     const std::shared_ptr<std::atomic_bool> &cancel, const AssetReader &reader) {
    if (root != project) {
        if (!root.isEmpty())
            fallbacks.clear();
        root = project;
        provisionalSources.clear();
        sources.clear();
        records.clear();
        failures.clear();
        forward.clear();
        reverse.clear();
        cache.clear();
    }
    editableSnapshot = editableIndex;
    stack.build(project, editableSnapshot, fallbacks, profile.version);
    const auto &index = stack.index();
    diagnostics = stack.diagnostics;
    QSet<QString> dirty;
    for (auto it = records.begin(); it != records.end(); ++it)
        if (!index.records.contains(it.key()))
            dirty.insert(it.key());
    int count = 0;
    qint64 total = 0;
    for (auto it = index.records.begin(); it != index.records.end(); ++it) {
        if (cancel && cancel->load())
            return;
        // Most pack entries are PNGs. Avoid splitting/resource-parsing each one
        // three times when consuming a large existing Index snapshot.
        if (!it.key().endsWith(".json"))
            continue;
        if (!relevant(it.key())) {
            const auto parts = it.key().split('/');
            if (parts.size() > 3 && parts[0] == "assets" &&
                QStringList{"models", "items", "blockstates"}.contains(parts[2]) &&
                it.key().endsWith(".json") && diagnostics.size() < 100)
                diagnostics << "Invalid namespace/resource path: " + it.key();
            continue;
        }
        if (++count > 8192 || (total += std::max(qint64(0), it->size)) > MaxTotal) {
            diagnostics << "Model catalog budget exceeded (8192 JSON files / 32 MiB); remaining "
                           "definitions unavailable";
            break;
        }
        auto old = records.constFind(it.key());
        if (old == records.cend() || old->projectRoot != it->projectRoot ||
            old->modified != it->modified || old->size != it->size || scoped(it.key(), changed))
            dirty.insert(it.key());
    }
    // Texture creation/removal also invalidates cached missing-reference diagnostics.
    for (const auto &path : changed) {
        for (auto c = cache.begin(); c != cache.end();) {
            bool hit = false;
            for (const auto &texture : c->textures)
                if (scoped(texture, {path})) {
                    hit = true;
                    break;
                }
            if (hit)
                c = cache.erase(c);
            else
                ++c;
        }
    }
    QSet<QString> invalid = dirty;
    for (const auto &path : dirty)
        for (const auto &child : dependents(path))
            invalid.insert(child);
    for (const auto &path : dirty) {
        if (cancel && cancel->load())
            return;
        sources.remove(path);
        failures.remove(path);
        records.remove(path);
        if (index.records.contains(path)) {
            QString error;
            QJsonObject o;
            if (reader) {
                auto file = reader(path);
                o = readJson(file.get(), error);
            } else {
                const auto canonical = assetPath(path);
                const auto base = index.records[path].projectRoot;
                if (canonical.isEmpty() || !canonical.startsWith(base + '/', Qt::CaseInsensitive)) {
                    error = "Asset escapes project or is missing";
                } else {
                    QFile file(canonical);
                    o = readJson(file.open(QIODevice::ReadOnly) ? &file : nullptr, error);
                }
            }
            if (!error.isEmpty())
                failures[path] = error;
            else
                sources[path] = o;
            records[path] = index.records[path];
            ++parsedFiles;
        }
        replaceEdges(path);
    }
    for (const auto &path : invalid)
        cache.remove(Location::fromAsset(path, "models"));
    for (auto it = provisionalSources.begin(); it != provisionalSources.end();) {
        if (stack.editable(it.key())) {
            const auto path = it.key();
            it = provisionalSources.erase(it);
            cache.remove(Location::fromAsset(path, "models"));
        } else {
            const auto path = it.key();
            const auto value = it.value();
            ++it;
            previewSource(path, value);
        }
    }
}
QStringList Library::dependents(const QString &asset, bool transitive) const {
    QSet<QString> seen;
    QStringList todo{asset};
    while (!todo.isEmpty() && seen.size() < 16384) {
        auto current = todo.takeLast();
        for (const auto &p : reverse.value(current))
            if (!seen.contains(p)) {
                seen.insert(p);
                if (transitive)
                    todo << p;
            }
    }
    auto out = seen.values();
    out.sort();
    return out;
}
QStringList Library::dependencies(const QString &asset) const {
    auto out = forward.value(asset).values();
    out.sort();
    return out;
}
QStringList Library::modelIds() const {
    QStringList out;
    for (auto it = records.begin(); it != records.end(); ++it) {
        auto id = Location::fromAsset(it.key(), "models");
        if (!id.isEmpty())
            out << id;
    }
    for (auto it = provisionalSources.cbegin(); it != provisionalSources.cend(); ++it)
        if (!records.contains(it.key()))
            out << Location::fromAsset(it.key(), "models");
    return out;
}
QStringList Library::definitionPaths() const {
    QStringList out;
    for (auto it = records.begin(); it != records.end(); ++it)
        if (Location::fromAsset(it.key(), "models").isEmpty())
            out << it.key();
    return out;
}
Model Library::model(const QString &input) {
    const auto id = Location::parse(input).id();
    if (cache.contains(id)) {
        ++cacheHits;
        return cache[id];
    }
    QSet<QString> visiting;
    auto result = resolve(input, visiting, 0);
    if (cache.size() >= 128)
        cache.clear();
    cache[id] = result;
    return result;
}
Model Library::resolve(const QString &input, QSet<QString> &visiting, int depth) {
    const auto l = Location::parse(input);
    Model m;
    m.id = l.id();
    m.version = profile.version;
    if (!profile.valid()) {
        m.diagnostics << "Unsupported Minecraft version profile";
        return m;
    }
    if (!l.valid()) {
        m.diagnostics << "Invalid namespace or resource path: " + input;
        return m;
    }
    if (depth >= MaxDepth || visiting.contains(m.id)) {
        m.diagnostics << "Cyclic or excessive parent chain: " + m.id;
        return m;
    }
    if (m.id == "minecraft:builtin/generated") {
        m.generated = true;
        m.valid = true;
        return m;
    }
    if (m.id.startsWith("minecraft:builtin/")) {
        m.diagnostics << "Unsupported built-in model renderer: " + m.id;
        return m;
    }
    const auto path = l.asset("models");
    if (!sources.contains(path)) {
        m.diagnostics << (failures.contains(path) ? failures[path]
                                                  : "Missing parent/model: " + m.id);
        return m;
    }
    visiting.insert(m.id);
    m.source = sources[path];
    if (m.source.size() > 4096 || m.source["textures"].toObject().size() > 4096 ||
        m.source["display"].toObject().size() > 64) {
        m.diagnostics << "Model property/texture/display allocation limit";
        visiting.remove(m.id);
        return m;
    }
    if (m.source.contains("parent")) {
        if (!m.source["parent"].isString()) {
            m.diagnostics << "Parent must be a resource-location string";
            visiting.remove(m.id);
            return m;
        }
        auto parent = resolve(m.source["parent"].toString(), visiting, depth + 1);
        for (const auto &message : parent.diagnostics) {
            // Texture variables are resolved again after child overrides merge;
            // a child may fix a missing/cyclic variable from the raw parent.
            if (message.startsWith("Missing texture:") ||
                message.startsWith("Missing texture variable:") ||
                message.startsWith("Cyclic texture variable:") ||
                message.startsWith("Invalid texture resource:") ||
                message.startsWith("Unsupported texture value") ||
                message.startsWith("force_translucent"))
                continue;
            m.diagnostics << message;
        }
        m.parents = parent.parents;
        m.parents << parent.id;
        m.generated = parent.generated;
        m.resolved = parent.resolved;
        m.display = parent.display;
        m.displayOrigin = parent.displayOrigin;
        if (!parent.valid) {
            visiting.remove(m.id);
            return m;
        }
    }
    m.resolved = merged(m.resolved, m.source);
    visiting.remove(m.id);
    // Minecraft resolves display per context: the nearest model defining the context wins,
    // and within one model a missing left-hand context reuses its right-hand value.
    if (m.source["display"].isObject()) {
        const auto local = m.source["display"].toObject();
        for (const auto &context : displayContexts()) {
            QString key = context;
            if (!local.contains(key) && leftHandContext(context) && local.contains(pairedHand(context)))
                key = pairedHand(context);
            if (!local.contains(key))
                continue;
            m.display[context] = local[key];
            m.displayOrigin[context] = {m.id, key};
        }
    }
    if ((m.resolved.contains("elements") && !m.resolved["elements"].isArray()) ||
        m.resolved["elements"].toArray().size() > MaxElements ||
        (m.resolved.contains("textures") && !m.resolved["textures"].isObject()) ||
        (m.resolved.contains("display") && !m.resolved["display"].isObject())) {
        m.diagnostics << "Invalid elements/textures/display or element allocation limit";
        return m;
    }
    static const QSet<QString> known{"parent",           "textures",  "elements", "display",
                                     "ambientocclusion", "gui_light", "credit"};
    for (auto it = m.source.begin(); it != m.source.end(); ++it)
        if (!known.contains(it.key()))
            m.diagnostics << "Unsupported model field retained: " + it.key();
    if (m.resolved.contains("ambientocclusion") && !m.resolved["ambientocclusion"].isBool()) {
        m.diagnostics << "ambientocclusion must be boolean";
        return m;
    }
    const auto display = m.resolved["display"].toObject();
    const QStringList contexts{"gui",
                               "firstperson_righthand",
                               "firstperson_lefthand",
                               "thirdperson_righthand",
                               "thirdperson_lefthand",
                               "ground",
                               "fixed",
                               "head",
                               "on_shelf"};
    for (auto it = display.begin(); it != display.end(); ++it) {
        if (!contexts.contains(it.key()))
            m.diagnostics << "Unsupported display context retained: " + it.key();
        if (!it->isObject()) {
            m.diagnostics << "Display transform must be an object: " + it.key();
            return m;
        }
        const auto o = it->toObject();
        for (auto p = o.begin(); p != o.end(); ++p) {
            if (!QStringList{"rotation", "translation", "scale"}.contains(p.key())) {
                m.diagnostics << "Unsupported display property retained: " + p.key();
                continue;
            }
            auto a = p->toArray();
            bool valid = a.size() == 3;
            for (auto n : a)
                valid &=
                    n.isDouble() && std::isfinite(n.toDouble()) && std::abs(n.toDouble()) <= 1e4;
            if (!valid) {
                m.diagnostics << "Invalid display vector: " + it.key() + '/' + p.key();
                return m;
            }
        }
    }
    if (m.resolved.contains("gui_light"))
        m.diagnostics << "gui_light retained; preview uses the selected lighting controls";
    const auto textures = m.resolved["textures"].toObject();
    if (textures.size() > 4096) {
        m.diagnostics << "Resolved texture-variable allocation limit";
        return m;
    }
    for (auto it = textures.begin(); it != textures.end(); ++it) {
        QJsonValue value = it.value();
        QSet<QString> aliases{it.key()};
        int steps = 0;
        while (value.isString() && value.toString().startsWith('#')) {
            const auto key = value.toString().mid(1);
            if (++steps >= MaxDepth || aliases.contains(key)) {
                m.diagnostics << "Cyclic texture variable: #" + key;
                value = {};
                break;
            }
            aliases.insert(key);
            if (!textures.contains(key)) {
                m.diagnostics << "Missing texture variable: #" + key;
                value = {};
                break;
            }
            value = textures[key];
        }
        if (value.isUndefined() || value.isNull())
            continue;
        if ((!value.isString() && !value.isObject()) ||
            (value.isObject() &&
             (!profile.spriteObjects || !value.toObject()["sprite"].isString()))) {
            m.diagnostics << "Unsupported texture value for " + profile.version + ": " + it.key();
            continue;
        }
        if (value.isObject()) {
            const auto o = value.toObject();
            if (o.contains("force_translucent") && !o["force_translucent"].isBool()) {
                m.diagnostics << "force_translucent must be boolean";
                continue;
            }
            for (auto p = o.begin(); p != o.end(); ++p)
                if (p.key() != "sprite" && p.key() != "force_translucent")
                    m.diagnostics << "Unsupported texture field retained: " + p.key();
        }
        auto texture = Location::parse(sprite(value));
        if (!texture.valid()) {
            m.diagnostics << "Invalid texture resource: " + sprite(value);
            continue;
        }
        m.textures[it.key()] = texture.asset("textures");
        if (assetPath(texture.asset("textures")).isEmpty())
            m.diagnostics << "Missing texture: " + texture.id();
    }
    m.valid = true;
    m.diagnostics.removeDuplicates();
    return m;
}
QStringList Library::modelsForTexture(const QString &asset) {
    QStringList out;
    for (const auto &path : dependents(asset)) {
        const auto id = Location::fromAsset(path, "models");
        if (id.isEmpty())
            continue;
        const auto m = model(id);
        if (m.textures.values().contains(asset))
            out << id;
        else {
            const auto elements = m.resolved["elements"].toArray();
            for (const auto &e : elements) {
                const auto faces = e.toObject()["faces"].toObject();
                bool hit = false;
                for (auto f = faces.begin(); f != faces.end(); ++f)
                    if (Location::parse(f.value().toObject()["texture"].toString())
                            .asset("textures") == asset) {
                        hit = true;
                        break;
                    }
                if (hit) {
                    out << id;
                    break;
                }
            }
        }
    }
    out.removeDuplicates();
    out.sort();
    return out;
}
} // namespace nova::mc
