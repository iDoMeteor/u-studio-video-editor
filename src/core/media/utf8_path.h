#pragma once

// The project keeps paths as UTF-8 std::strings (the model, project files,
// GTK). std::filesystem's narrow-string constructor and string() use the
// OS's narrow encoding, which is UTF-8 on Linux but not on Windows
// (ADR-017): convert through these instead, so a non-ASCII file name
// survives either way.

#include <filesystem>
#include <string>
#include <string_view>

namespace ustudio::core {

inline std::filesystem::path pathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string utf8String(const std::filesystem::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace ustudio::core
