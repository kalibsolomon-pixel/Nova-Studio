// SPDX-License-Identifier: GPL-3.0-or-later
#include "workspace_layout.h"

namespace nova {
namespace {
constexpr auto layoutPrefix = "NOVALAYOUT1 ";
}
QByteArray packLayout(const QByteArray &state) { return QByteArray(layoutPrefix) + "left\n" + state; }
QByteArray unpackLayout(const QByteArray &blob) {
    if (!blob.startsWith(layoutPrefix))
        return blob;
    const auto end = blob.indexOf('\n');
    const auto prefix = qsizetype(qstrlen(layoutPrefix));
    if (end < 0 || end - prefix > 32)
        return {};
    return blob.mid(end + 1);
}
} // namespace nova
