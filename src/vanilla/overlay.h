// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Read-only baseline layer under a project's asset index (DESIGN.md section 4): records
// are keyed by logical resource path and the project always wins.
#include "cache.h"
#include "index.h"

namespace nova::vanilla {

// One Baseline record per entry file: filesystemPath = cache object, projectRoot = cache
// root, nameSpace "minecraft", category from assetCategory(), metadataReference for a .png
// with a .png.mcmeta. `directories` holds every ancestor folder (including ""). No disk access.
Index baselineIndex(const BaselineEntry &entry);
// Project records are kept as they are (shadowsBaseline = true when the baseline has the same
// path); baseline-only records are added with their ancestor folders. Files stay sorted and
// folder child lists sorted and de-duplicated. A baseline path under a project *file* of the
// same name as one of its folders is left out.
Index mergeBaseline(const Index &project, const Index &baseline);

} // namespace nova::vanilla
