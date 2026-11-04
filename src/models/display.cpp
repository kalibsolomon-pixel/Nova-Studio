// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.h"
#include "edit.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

namespace nova::mc {
namespace {
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
bool readVector(const QJsonObject &o, const char *key, std::array<double, 3> &out) {
    if (!o.contains(key))
        return true;
    const auto a = o[key].toArray();
    if (!o[key].isArray() || a.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!a[i].isDouble() || !std::isfinite(a[i].toDouble()) || std::abs(a[i].toDouble()) > 1e4)
            return false;
        out[size_t(i)] = a[i].toDouble();
    }
    return true;
}
QJsonArray array(const std::array<double, 3> &v) {
    // Round to 1e-6 so float noise from numeric controls never reaches source JSON.
    QJsonArray a;
    for (double x : v) {
        double r = std::round(x * 1e6) / 1e6;
        a.append(r == 0 ? 0.0 : r);
    }
    return a;
}
} // namespace

const QStringList &displayContexts() {
    static const QStringList contexts{"gui",
                                      "ground",
                                      "fixed",
                                      "head",
                                      "firstperson_righthand",
                                      "firstperson_lefthand",
                                      "thirdperson_righthand",
                                      "thirdperson_lefthand",
                                      "on_shelf"};
    return contexts;
}
QStringList displayContexts(const QString &version) {
    // The same nine ItemDisplayContext values exist in 1.21.11, 26.1 and 26.3.
    return Profile::forVersion(version).valid() ? displayContexts() : QStringList{};
}
QString displayContextLabel(const QString &context) {
    static const QMap<QString, QString> labels{{"gui", "GUI / inventory"},
                                               {"ground", "Ground (dropped)"},
                                               {"fixed", "Fixed (item frame)"},
                                               {"head", "Head"},
                                               {"firstperson_righthand", "First person · right hand"},
                                               {"firstperson_lefthand", "First person · left hand"},
                                               {"thirdperson_righthand", "Third person · right hand"},
                                               {"thirdperson_lefthand", "Third person · left hand"},
                                               {"on_shelf", "On shelf"}};
    return labels.value(context, context);
}
bool leftHandContext(const QString &context) { return context.endsWith("_lefthand"); }
bool handContext(const QString &context) {
    return context.endsWith("_lefthand") || context.endsWith("_righthand");
}
QString pairedHand(const QString &context) {
    if (context.endsWith("_lefthand"))
        return QString(context).replace("_lefthand", "_righthand");
    if (context.endsWith("_righthand"))
        return QString(context).replace("_righthand", "_lefthand");
    return {};
}
QString DisplayResolution::describe() const {
    const auto from = originContext == context ? QString() : " (" + originContext + ")";
    switch (source) {
    case DisplaySource::Local:
        return "Local value in " + origin;
    case DisplaySource::LocalFromRightHand:
        return "Mirrored from this model's right hand" + from;
    case DisplaySource::Inherited:
        return "Inherited from " + origin;
    case DisplaySource::InheritedFromRightHand:
        return "Inherited right hand from " + origin + from + ", mirrored";
    case DisplaySource::Default:
        break;
    }
    return "Default (no transform in the model chain)";
}
bool parseDisplayTransform(const QJsonValue &value, DisplayTransform *out, QString *error) {
    DisplayTransform t;
    if (value.isUndefined() || value.isNull()) {
        if (out)
            *out = t;
        return true;
    }
    if (!value.isObject())
        return fail(error, "Display transform must be an object");
    const auto o = value.toObject();
    if (!readVector(o, "rotation", t.rotation) || !readVector(o, "translation", t.translation) ||
        !readVector(o, "scale", t.scale))
        return fail(error, "Display rotation/translation/scale must be three finite numbers");
    if (out)
        *out = t;
    return true;
}
QJsonObject displayTransformJson(const DisplayTransform &value, QJsonObject o) {
    const DisplayTransform defaults;
    auto put = [&](const char *key, const std::array<double, 3> &v,
                   const std::array<double, 3> &fallback) {
        if (o.contains(key) || v != fallback)
            o[key] = array(v);
    };
    put("rotation", value.rotation, defaults.rotation);
    put("translation", value.translation, defaults.translation);
    put("scale", value.scale, defaults.scale);
    return o;
}
bool validateAuthoredDisplay(const DisplayTransform &v, QString *error) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v.rotation[i]) || !std::isfinite(v.translation[i]) ||
            !std::isfinite(v.scale[i]))
            return fail(error, "Display values must be finite numbers");
        if (std::abs(v.rotation[i]) > DisplayRotationLimit)
            return fail(error, "Display rotation must stay within ±3600°");
        if (std::abs(v.translation[i]) > DisplayTranslationLimit)
            return fail(error, "Translation beyond ±80 (5 blocks) is clamped by Minecraft");
        if (std::abs(v.scale[i]) > DisplayScaleLimit)
            return fail(error, "Scale beyond ±4 is clamped by Minecraft");
    }
    return true;
}
DisplayTransform clampedDisplay(DisplayTransform v, bool *translation, bool *scale) {
    bool t = false, s = false;
    for (int i = 0; i < 3; ++i) {
        const auto ct = std::clamp(v.translation[i], -DisplayTranslationLimit, DisplayTranslationLimit);
        const auto cs = std::clamp(v.scale[i], -DisplayScaleLimit, DisplayScaleLimit);
        t |= ct != v.translation[i];
        s |= cs != v.scale[i];
        v.translation[i] = ct;
        v.scale[i] = cs;
    }
    if (translation)
        *translation = t;
    if (scale)
        *scale = s;
    return v;
}
QMatrix4x4 displayMatrix(const DisplayTransform &input, bool leftHand) {
    auto v = clampedDisplay(input);
    auto t = v.translation;
    auto r = v.rotation;
    if (leftHand) {
        t[0] = -t[0];
        r[1] = -r[1];
        r[2] = -r[2];
    }
    QMatrix4x4 m;
    m.translate(float(t[0] / 8), float(t[1] / 8), float(t[2] / 8)); // 1/16 block = 1/8 mesh unit
    m.rotate(float(r[0]), 1, 0, 0);                                 // rotationXYZ: X, then Y, then Z
    m.rotate(float(r[1]), 0, 1, 0);
    m.rotate(float(r[2]), 0, 0, 1);
    m.scale(float(v.scale[0]), float(v.scale[1]), float(v.scale[2]));
    return m;
}
QMatrix4x4 itemPose(const QMatrix4x4 &display, const QMatrix4x4 &item) {
    // Minecraft: display * translate(-0.5) * item, applied to block-space vertices v.
    // Nova mesh points are p = 2 * (v - 0.5); convert to block space and back.
    QMatrix4x4 toBlock, fromBlock;
    toBlock.translate(.5f, .5f, .5f);
    toBlock.scale(.5f);
    fromBlock.scale(2.f);
    fromBlock.translate(-.5f, -.5f, -.5f);
    return display * fromBlock * item * toBlock;
}
DisplayResolution resolveDisplay(const Model &model, const QString &context) {
    DisplayResolution r;
    r.context = context;
    if (!model.display.contains(context))
        return r;
    const auto origin = model.displayOrigin.value(context);
    r.origin = origin.first;
    r.originContext = origin.second;
    const bool local = origin.first == model.id, fromRight = origin.second != context;
    r.source = local ? (fromRight ? DisplaySource::LocalFromRightHand : DisplaySource::Local)
                     : (fromRight ? DisplaySource::InheritedFromRightHand : DisplaySource::Inherited);
    r.valid = parseDisplayTransform(model.display[context], &r.value);
    clampedDisplay(r.value, &r.translationClamped, &r.scaleClamped);
    return r;
}
QJsonObject blockDisplayPreset() {
    auto t = [](DisplayTransform v) { return displayTransformJson(v); };
    DisplayTransform gui, ground, fixed, third, first, firstLeft;
    // GUI: isometric view (30° pitch, 45° yaw seen from the front-left). The projected
    // unit cube is about 1.57 blocks tall, so 10/16 keeps it inside one 16 px slot.
    gui.rotation = {30, 225, 0};
    gui.scale = {0.625, 0.625, 0.625};
    ground.translation = {0, 3, 0};
    ground.scale = {0.25, 0.25, 0.25};
    fixed.scale = {0.5, 0.5, 0.5};
    third.rotation = {75, 45, 0};
    third.translation = {0, 2.5, 0};
    third.scale = {0.375, 0.375, 0.375};
    first.rotation = {0, 45, 0};
    first.scale = {0.4, 0.4, 0.4};
    firstLeft = first;
    firstLeft.rotation = {0, 225, 0};
    return {{"gui", t(gui)},
            {"ground", t(ground)},
            {"fixed", t(fixed)},
            {"thirdperson_righthand", t(third)},
            {"firstperson_righthand", t(first)},
            {"firstperson_lefthand", t(firstLeft)}};
}

// ---- ModelEdit display operations: staged, then committed as one undo step ----
namespace {
bool knownContext(const QString &c, QString *error) {
    return displayContexts().contains(c) || fail(error, "Unsupported display context: " + c);
}
} // namespace
QJsonObject ModelEdit::localDisplay(const QString &context) const {
    return staged["display"].toObject()[context].toObject();
}
bool ModelEdit::previewDisplay(const QString &context, const DisplayTransform &value,
                               QString *error) {
    if (!knownContext(context, error) || !validateAuthoredDisplay(value, error))
        return false;
    if (staged.contains("display") && !staged["display"].isObject())
        return fail(error, "The model's display value is not an object; edit the source first");
    auto display = staged["display"].toObject();
    if (display.contains(context)) {
        DisplayTransform current;
        if (!parseDisplayTransform(display[context], &current))
            return fail(error, "This context holds an unsupported value; reset it before editing");
    }
    display[context] = displayTransformJson(value, display[context].toObject());
    staged["display"] = display;
    preview = true;
    return true;
}
bool ModelEdit::resetDisplay(const QString &context, QString *error) {
    if (!knownContext(context, error))
        return false;
    auto display = staged["display"].toObject();
    if (!display.contains(context))
        return true;
    display.remove(context);
    if (display.isEmpty())
        staged.remove("display");
    else
        staged["display"] = display;
    preview = true;
    return true;
}
bool ModelEdit::pasteDisplay(const QString &context, const QJsonObject &value, QString *error) {
    DisplayTransform t;
    if (!parseDisplayTransform(value, &t, error))
        return false;
    return previewDisplay(context, t, error);
}
bool ModelEdit::linkHands(const QString &context, bool linked, const DisplayTransform &right,
                          QString *error) {
    if (!handContext(context))
        return fail(error, "Hand linking applies to first- and third-person contexts");
    const auto left = leftHandContext(context) ? context : pairedHand(context);
    if (linked)
        return resetDisplay(left, error); // Minecraft mirrors the right hand automatically
    return previewDisplay(left, right, error);
}
bool ModelEdit::applyDisplayPreset(const QJsonObject &preset, QString *error) {
    const auto before = staged;
    const bool wasPreview = preview;
    for (auto it = preset.begin(); it != preset.end(); ++it) {
        DisplayTransform t;
        if (!parseDisplayTransform(it.value(), &t, error) || !previewDisplay(it.key(), t, error)) {
            staged = before;
            preview = wasPreview;
            return false;
        }
    }
    return true;
}
} // namespace nova::mc
