#include "platform/gpu.h"

#include <cstdlib>
#include <filesystem>

namespace ustudio::platform {

std::string hardwareDecodeApi()
{
    // The same device producer_avformat.c opens for "vaapi".
    const char *device = std::getenv("MLT_AVFORMAT_HWACCEL_DEVICE");
    std::error_code ec;
    return std::filesystem::exists(device && *device ? device : "/dev/dri/renderD128", ec) ? "vaapi" : "";
}

} // namespace ustudio::platform
