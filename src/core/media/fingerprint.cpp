#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"

#include <chrono>
#include <filesystem>

namespace ustudio::core {

std::string fileFingerprint(const std::string &path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = pathFromUtf8(path);
    const auto size = fs::file_size(file, ec);
    if (ec)
        return {};
    const fs::file_time_type modified = fs::last_write_time(file, ec);
    if (ec)
        return {};
    const auto sinceEpoch = std::chrono::file_clock::to_sys(modified).time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count();
    return std::to_string(size) + ":" + std::to_string(ns);
}

} // namespace ustudio::core
