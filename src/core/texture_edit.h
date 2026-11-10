// SPDX-License-Identifier: GPL-3.0-or-later
// Compact texture edits for automation (MCP Phase 2): palette-indexed pixel writes, palette
// remapping, gradient mapping and erasing. Each function decodes and validates its JSON arguments
// against the canvas before computing anything, then returns new pixels for one layer. Callers
// commit that result through Document::replaceLayerPixels, so undo, recovery and drafts are the
// editor's own. All colour arithmetic is integer, on straight-alpha 8-bit encoded sRGB channels.
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>

namespace nova::texture {
constexpr int MaxPalette = 62;      // one index character per pixel
constexpr int MaxMappings = 64;
constexpr int MaxStops = 16;
constexpr int MaxSide = 256;        // the MCP editing dimension limit
constexpr qint64 MaxPixels = qint64(MaxSide) * MaxSide;
// Index characters in order: '0'..'9' are 0..9, 'a'..'z' are 10..35, 'A'..'Z' are 36..61.
inline constexpr char Alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr char Keep = '.';          // leave this pixel unchanged

// An argument problem: the JSON pointer of the field and a message naming the rule.
struct Failure {
    QString pointer, message;
    bool ok() const { return message.isEmpty(); }
};
// "#RRGGBB" (alpha FF) or "#RRGGBBAA", case-insensitive. Returns false for anything else.
bool parseColor(const QString &text, QRgb *color, bool *withAlpha = nullptr);
bool compactCommand(const QString &command);
// The canvas rectangle an edit reads or writes (the whole canvas when the call names none).
QRect region(const QString &command, const QJsonObject &args, QSize canvas);
// Validates every argument of a command against the canvas without allocating pixels.
Failure validate(const QString &command, const QJsonObject &args, QSize canvas);
// Validates, then computes the layer's new pixels (Format_RGBA8888, layer size). selection is the
// document's selection mask (null: no restriction). Nothing outside region changes.
Failure apply(const QString &command, const QJsonObject &args, const QImage &layer, const QImage &selection,
              QImage *out);
} // namespace nova::texture
