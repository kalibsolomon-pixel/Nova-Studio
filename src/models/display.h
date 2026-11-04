// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Item display transforms ("display" in block/item model JSON).
//
// Semantics verified against the official 1.21.11, 26.1 and 26.3 client bytecode
// (see docs/phase-2c8/DISPLAY_TRANSFORMS.md):
//  * nine contexts, identical in all three supported versions;
//  * per model, a missing *_lefthand context reuses that model's *_righthand value;
//  * per context, the first model in the child -> parent chain that defines it wins
//    (contexts are not merged property-by-property);
//  * translation is in 1/16 block, clamped to +/-80; scale is clamped to +/-4;
//  * rotation is Euler XYZ degrees (rotationXYZ); left-hand contexts negate
//    translation X and rotation Y/Z when applied;
//  * the runtime pose is display * translate(-0.5) * item-definition transformation.
#include "models.h"
#include <array>

namespace nova::mc {
struct DisplayTransform {
    std::array<double, 3> rotation{0, 0, 0}, translation{0, 0, 0}, scale{1, 1, 1};
    bool operator==(const DisplayTransform &o) const {
        return rotation == o.rotation && translation == o.translation && scale == o.scale;
    }
    bool identity() const { return *this == DisplayTransform{}; }
};
enum class DisplaySource { Local, LocalFromRightHand, Inherited, InheritedFromRightHand, Default };
struct DisplayResolution {
    QString context;
    DisplayTransform value;   // as authored, before clamping and left-hand negation
    DisplaySource source = DisplaySource::Default;
    QString origin;           // model id that supplied the value
    QString originContext;    // JSON key that supplied it
    bool valid = true;        // false when the supplying JSON is not a supported transform
    bool translationClamped = false, scaleClamped = false;
    QString describe() const;
};
constexpr double DisplayTranslationLimit = 80, DisplayScaleLimit = 4, DisplayRotationLimit = 3600;

const QStringList &displayContexts();
// Contexts accepted by a version profile; empty for unsupported profiles.
QStringList displayContexts(const QString &version);
QString displayContextLabel(const QString &context);
bool leftHandContext(const QString &context);
bool handContext(const QString &context);
QString pairedHand(const QString &context); // right <-> left; empty for other contexts

bool parseDisplayTransform(const QJsonValue &value, DisplayTransform *out, QString *error = nullptr);
// Writes rotation/translation/scale into `preserved`, keeping its unknown keys. A property
// that was absent stays absent when it still has its default value.
QJsonObject displayTransformJson(const DisplayTransform &value, QJsonObject preserved = {});
// Authoring accepts only values Minecraft applies unchanged (no silent clamping).
bool validateAuthoredDisplay(const DisplayTransform &value, QString *error = nullptr);
DisplayTransform clampedDisplay(DisplayTransform value, bool *translation = nullptr,
                                bool *scale = nullptr);
// Matrix in Nova mesh units (one block = 2 units, centred at the origin).
QMatrix4x4 displayMatrix(const DisplayTransform &value, bool leftHand);
// Composes a display matrix (mesh units) with an item-definition transformation (block
// units, origin at the block corner) in Minecraft's order.
QMatrix4x4 itemPose(const QMatrix4x4 &display, const QMatrix4x4 &itemTransformation);
DisplayResolution resolveDisplay(const Model &model, const QString &context);
// Original preset for element (cuboid) models. See DISPLAY_TRANSFORMS.md for derivation.
QJsonObject blockDisplayPreset();
} // namespace nova::mc
