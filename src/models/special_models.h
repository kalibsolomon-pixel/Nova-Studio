// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "models.h"

namespace nova::mc {
Mesh specialMesh(const Model &base, const QJsonObject &special);
} // namespace nova::mc
