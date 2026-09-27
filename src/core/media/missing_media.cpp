#include "core/media/missing_media.h"

#include "core/media/utf8_path.h"

#include <filesystem>

namespace ustudio::core {

bool isFileResource(const std::string &path)
{
    return !path.empty() && pathFromUtf8(path).is_absolute();
}

std::vector<AssetId> markMissingMedia(Model &model)
{
    std::vector<AssetId> missing;
    for (const Asset &asset : model.project().bin) {
        if (!isFileResource(asset.path))
            continue;
        std::error_code ec;
        const bool present = std::filesystem::is_regular_file(pathFromUtf8(asset.path), ec);
        if (!present)
            missing.push_back(asset.id);
        const Asset::Status status =
            present ? (asset.status == Asset::Status::Missing ? Asset::Status::Ready : asset.status)
                    : Asset::Status::Missing;
        model.setAssetStatus(asset.id, status);
    }
    return missing;
}

} // namespace ustudio::core
