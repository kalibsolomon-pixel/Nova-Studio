// SPDX-License-Identifier: GPL-3.0-or-later
#include "models.h"
#include "tints.h"
#include <QDate>
#include <QJsonArray>
#include <cmath>
#include <functional>

namespace nova::mc {
Dispatch Library::dispatch(const QString &asset, const QJsonObject &inputs, int choice) const {
    Dispatch result;
    if (!profile.valid()) {
        result.diagnostics << "Unsupported version profile";
        return result;
    }
    if (!sources.contains(asset)) {
        result.diagnostics << failures.value(asset, "Missing definition: " + asset);
        return result;
    }
    int nodes = 0;
    auto append = [&](const QJsonObject &o, const QString &label) {
        const auto l = Location::parse(o["model"].toString());
        if (!l.valid()) {
            result.diagnostics << "Invalid model reference in " + label;
            return;
        }
        Instance i;
        i.model = l.id();
        i.label = label;
        i.uvlock = o["uvlock"].toBool();
        int *angles[] = {&i.x, &i.y, &i.z};
        const char *keys[] = {"x", "y", "z"};
        for (int a = 0; a < 3; ++a) {
            auto v = o[keys[a]];
            if (!v.isUndefined() && (!v.isDouble() || v.toDouble() != v.toInt() || v.toInt() < 0 ||
                                     v.toInt() > 270 || v.toInt() % 90)) {
                result.diagnostics << "Blockstate rotations must be 0/90/180/270";
                return;
            }
            *angles[a] = v.toInt();
        }
        if (result.instances.size() < 256)
            result.instances << i;
        else
            result.diagnostics << "Definition instance limit (256)";
    };
    auto weighted = [&](const QJsonValue &v, const QString &label) {
        if (v.isObject()) {
            append(v.toObject(), label);
            return;
        }
        const auto array = v.toArray();
        if (array.isEmpty() || array.size() > 1024) {
            result.diagnostics << "Empty/oversized weighted alternatives";
            return;
        }
        // The input selects an explicit alternative, never an unreported random choice.
        for (int n = 0; n < array.size(); ++n) {
            const auto o = array[n].toObject();
            const auto w = o["weight"];
            if (!array[n].isObject() ||
                (!w.isUndefined() &&
                 (!w.isDouble() || w.toDouble() != w.toInt() || w.toInt() < 1))) {
                result.diagnostics << "Invalid weighted model entry";
                return;
            }
        }
        const int selected = std::max(0, choice) % int(array.size());
        append(array[selected].toObject(),
               label + QString(" · alternative %1/%2 (weight %3)")
                           .arg(selected + 1)
                           .arg(array.size())
                           .arg(array[selected].toObject()["weight"].toInt(1)));
    };
    std::function<bool(const QJsonObject &, int)> condition;
    condition = [&](const QJsonObject &o, int depth) {
        if (depth > 32 || ++nodes > 8192) {
            result.diagnostics << "Condition nesting/node limit";
            return false;
        }
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == "OR" || it.key() == "AND") {
                if (!it.value().isArray()) {
                    result.diagnostics << "Invalid multipart condition";
                    return false;
                }
                bool value = it.key() == "AND";
                for (const auto &part : it.value().toArray()) {
                    const bool match = part.isObject() && condition(part.toObject(), depth + 1);
                    if (it.key() == "AND")
                        value &= match;
                    else
                        value |= match;
                }
                if (!value)
                    return false;
            } else {
                if (!it.value().isString()) {
                    result.diagnostics << "Blockstate property must be a string";
                    return false;
                }
                if (!inputs.contains(it.key())) {
                    result.diagnostics << "Manual blockstate input required: " + it.key();
                    return false;
                }
                auto wanted = it.value().toString();
                const bool negate = wanted.startsWith('!');
                if (negate)
                    wanted.remove(0, 1);
                const bool match = wanted.split('|').contains(
                    inputs[it.key()].isBool() ? (inputs[it.key()].toBool() ? "true" : "false")
                                              : inputs[it.key()].toVariant().toString());
                if (match == negate)
                    return false;
            }
        }
        return true;
    };
    const auto definition = sources[asset];
    const auto blockId = Location::fromAsset(asset, "blockstates");
    TintColors tintColors(*this);
    if (!blockId.isEmpty()) {
        if (definition.contains("variants") && definition.contains("multipart"))
            result.diagnostics << "Conflicting variants and multipart definitions";
        const auto variants = definition["variants"].toObject();
        for (auto it = variants.begin(); it != variants.end(); ++it) {
            result.variants << it.key();
            QJsonObject properties;
            for (const auto &pair : it.key().split(',', Qt::SkipEmptyParts)) {
                const auto p = pair.split('=');
                if (p.size() == 2)
                    properties[p[0]] = p[1];
                else
                    result.diagnostics << "Malformed variant property combination";
            }
            if (condition(properties, 0))
                weighted(it.value(), it.key().isEmpty() ? "default variant" : it.key());
        }
        int part = 0;
        for (const auto &v : definition["multipart"].toArray()) {
            if (++part > 1024) {
                result.diagnostics << "Multipart limit";
                break;
            }
            const auto o = v.toObject();
            if (!o.contains("when") || (o["when"].isObject() && condition(o["when"].toObject(), 0)))
                weighted(o["apply"], QString("multipart %1").arg(part));
        }
        const auto colors = tintColors.block(blockId, inputs);
        for (auto &instance : result.instances)
            instance.tints = colors;
    } else if (!Location::fromAsset(asset, "items").isEmpty()) {
        const auto manualTints = inputs["_tints"].toArray();
        const bool vanillaItem = Location::parse(Location::fromAsset(asset, "items")).nameSpace == "minecraft";
        QMatrix4x4 subtree;
        std::function<void(const QJsonObject &, int, QString)> visit;
        visit = [&](const QJsonObject &o, int depth, QString label) {
            if (++nodes > 8192 || depth > 32) {
                result.diagnostics << "Item dispatch node/depth limit";
                return;
            }
            const auto type = Location::parse(o["type"].toString()).id();
            const auto parent = subtree;
            struct Restore {
                QMatrix4x4 &target;
                QMatrix4x4 value;
                ~Restore() { target = value; }
            } restore{subtree, parent};
            if (o.contains("transformation")) {
                if (!profile.itemTransforms) {
                    result.diagnostics << "Item subtree transformation unsupported in 1.21.11";
                    return;
                }
                QMatrix4x4 local;
                QString error;
                if (!itemTransform(o["transformation"], local, &error)) {
                    result.diagnostics << error;
                    return;
                }
                subtree = parent * local;
            }
            if (type == "minecraft:model") {
                const auto previousCount = result.instances.size();
                append(o, label);
                if (result.instances.size() > previousCount) {
                    auto &instance = result.instances.last();
                    instance.transformation = subtree;
                    int tintIndex = 0;
                    for (const auto &t : o["tints"].toArray()) {
                        if (tintIndex >= 256) {
                            result.diagnostics << "Tint source limit";
                            break;
                        }
                        const auto source = t.toObject();
                        const auto type = Location::parse(source["type"].toString()).id();
                        const auto manual = tintRgb(manualTints.at(tintIndex));
                        QRgb color = qRgb(255, 255, 255);
                        if (type == "minecraft:map_color" && profile.version == "26.3")
                            result.diagnostics << "map_color tint was removed in 26.3";
                        else if (manual && type != "minecraft:constant") {
                            color = *manual;
                            result.diagnostics
                                << "Simulated runtime tint from explicit _tints preview input: " +
                                       type;
                        } else if (const auto base = tintColors.item(type, source)) {
                            color = *base;
                        } else {
                            result.diagnostics
                                << "Unknown or invalid item tint source: " + type;
                        }
                        instance.tints << color;
                        ++tintIndex;
                    }
                }
            } else if (type == "minecraft:special") {
                const auto renderer = o["model"].toObject();
                if (renderer.isEmpty()) {
                    result.diagnostics << "Invalid special renderer";
                    return;
                }
                const auto previousCount = result.instances.size();
                append({{"model", o["base"]}}, label + " / special");
                if (result.instances.size() > previousCount) {
                    auto &instance = result.instances.last();
                    instance.transformation = subtree;
                    instance.special = renderer;
                }
            } else if (type == "minecraft:composite") {
                for (const auto &child : o["models"].toArray())
                    visit(child.toObject(), depth + 1, label + " / composite");
            } else if (type == "minecraft:empty") { /* Intentionally invisible. */
            } else if (type == "minecraft:bundle/selected_item") {
                // The item picked inside a bundle; a default (empty) bundle shows none.
            } else if (type == "minecraft:condition" || type == "minecraft:select" ||
                       type == "minecraft:range_dispatch") {
                const auto property = o["property"].toString();
                auto value = inputs[property];
                if (value.isUndefined() && vanillaItem) {
                    // A default stack in a slot: no components, not held, used, cast or broken.
                    if (property == "minecraft:local_time" && o["pattern"].toString() == "MM-dd")
                        value = QDate::currentDate().toString("MM-dd");
                    else if (type == "minecraft:condition")
                        value = false;
                    else if (type == "minecraft:range_dispatch")
                        value = 0.0;
                }
                if (value.isUndefined() && type == "minecraft:select" && o.contains("fallback")) {
                    // A default stack has no value for the property, so the game uses the fallback
                    // (for example leather armour without a trim).
                    result.diagnostics << "Using the select fallback; set " + property + " to preview a case";
                    visit(o["fallback"].toObject(), depth + 1, label + " / select " + property + " (fallback)");
                    return;
                }
                if (value.isUndefined()) {
                    result.diagnostics << "Manual item input required: " + property;
                    return;
                }
                if (type == "minecraft:condition") {
                    if (!value.isBool()) {
                        result.diagnostics << "Boolean item input required: " + property;
                        return;
                    }
                    visit(o[value.toBool() ? "on_true" : "on_false"].toObject(), depth + 1,
                          label + " / " + property + '=' + (value.toBool() ? "true" : "false"));
                } else if (type == "minecraft:select") {
                    QJsonObject selected = o["fallback"].toObject();
                    for (const auto &c : o["cases"].toArray()) {
                        const auto entry = c.toObject();
                        const auto when = entry["when"];
                        if ((when.isArray() && when.toArray().contains(value)) || when == value) {
                            selected = entry["model"].toObject();
                            break;
                        }
                    }
                    if (selected.isEmpty())
                        result.diagnostics << "No matching select case/fallback";
                    else
                        visit(selected, depth + 1, label + " / select " + property);
                } else {
                    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
                        result.diagnostics << "Numeric item input required: " + property;
                        return;
                    }
                    const double n = value.toDouble() * o["scale"].toDouble(1);
                    QJsonObject selected = o["fallback"].toObject();
                    double threshold = -INFINITY;
                    for (const auto &e : o["entries"].toArray()) {
                        const auto entry = e.toObject();
                        const double t = entry["threshold"].toDouble(INFINITY);
                        if (t <= n && t >= threshold) {
                            threshold = t;
                            selected = entry["model"].toObject();
                        }
                    }
                    if (selected.isEmpty())
                        result.diagnostics << "No matching range/fallback";
                    else
                        visit(selected, depth + 1, label + " / range " + property);
                }
            } else
                result.diagnostics << "Unsupported item model type: " + o["type"].toString();
        };
        visit(definition["model"].toObject(), 0, "item definition");
    } else
        result.diagnostics << "Expected a blockstate or modern item definition";
    result.diagnostics.removeDuplicates();
    return result;
}
} // namespace nova::mc
