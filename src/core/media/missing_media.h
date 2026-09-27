#pragma once

#include "core/model/model.h"

#include <vector>

namespace ustudio::core {

// Marks each file asset Missing or Ready as its path is found on disk
// (doc 07, on load; not a command). Generators ("color:red") and other
// non-file resources are left alone. Returns the assets now missing. IO: a
// stat per asset, so pool threads (the loader) or tests.
std::vector<AssetId> markMissingMedia(Model &model);

// Whether an asset's path names a file (absolute), not an MLT generator.
bool isFileResource(const std::string &path);

} // namespace ustudio::core
