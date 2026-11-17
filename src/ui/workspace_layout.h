// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Saved workspace arrangements. A blob is "NOVALAYOUT1 <tools>\n" followed by
// QMainWindow::saveState() bytes. The tool strip is a fixed rail on the left edge, so new
// blobs always say "left"; blobs written by builds with a movable strip may name another
// position, which is ignored. Blobs without the prefix are plain saveState() bytes.
#include <QByteArray>

namespace nova {
QByteArray packLayout(const QByteArray &state);
// The saveState() bytes of a blob, or empty when the prefix is malformed.
QByteArray unpackLayout(const QByteArray &blob);
} // namespace nova
