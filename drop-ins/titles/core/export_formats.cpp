#include "export_formats.h"

namespace ustudio::titles {

const std::vector<ExportFormat> &exportFormats()
{
    static const std::vector<ExportFormat> formats = {
        {"prores", "ProRes 4444 (alpha)", "mov", true},        {"webm", "WebM VP9 (alpha)", "webm", true},
        {"qtrle", "QuickTime Animation (alpha)", "mov", true}, {"png", "PNG sequence (alpha)", "", true},
        {"h264", "H.264 on its background", "mp4", false},
    };
    return formats;
}

const ExportFormat *exportFormat(std::string_view name)
{
    for (const ExportFormat &format : exportFormats())
        if (name == format.name)
            return &format;
    return nullptr;
}

} // namespace ustudio::titles
