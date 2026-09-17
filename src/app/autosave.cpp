#include "autosave.h"

#include <glib.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ustudio::app::autosave {

namespace {
namespace fs = std::filesystem;

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

// Extracts the content of a JSON string literal starting at `pos` (the
// opening quote); advances `pos` past the closing quote. Matches only the
// minimal escaping writeMeta() itself produces -- not a general parser.
std::string readJsonString(const std::string &json, size_t &pos)
{
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            out += json[pos + 1];
            pos += 2;
        } else {
            out += json[pos];
            ++pos;
        }
    }
    ++pos;
    return out;
}
} // namespace

std::string directory()
{
    const char *stateDir = g_get_user_state_dir();
    if (!stateDir || !*stateDir)
        return {};

    fs::path dir = fs::path(stateDir) / "ustudio" / "autosave";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return {};
    return dir.string();
}

std::string baseNameFor(const std::string &originalPath, const std::string &sessionId)
{
    std::string key = originalPath.empty() ? "untitled-" + sessionId : originalPath;
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key.c_str(), static_cast<gssize>(key.size()));
    std::string result = hash ? hash : "";
    if (hash)
        g_free(hash);
    return result;
}

bool writeMeta(const std::string &metaPath, const Meta &meta)
{
    std::ostringstream json;
    json << "{\"path\":\"" << jsonEscape(meta.originalPath) << "\",\"timestamp\":" << meta.timestampUnix << "}";

    std::string tmpPath = metaPath + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::trunc);
        if (!out)
            return false;
        out << json.str();
    }

    std::error_code ec;
    fs::rename(tmpPath, metaPath, ec);
    if (ec) {
        fs::remove(tmpPath, ec);
        return false;
    }
    return true;
}

std::optional<Meta> readMeta(const std::string &metaPath)
{
    std::ifstream in(metaPath);
    if (!in)
        return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string json = buf.str();

    Meta meta;
    size_t pos = json.find("\"path\"");
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find(':', pos);
    if (pos == std::string::npos)
        return std::nullopt;
    ++pos;
    while (pos < json.size() && json[pos] == ' ')
        ++pos;
    if (pos >= json.size() || json[pos] != '"')
        return std::nullopt;
    meta.originalPath = readJsonString(json, pos);

    pos = json.find("\"timestamp\"", pos);
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find(':', pos);
    if (pos == std::string::npos)
        return std::nullopt;
    ++pos;
    while (pos < json.size() && json[pos] == ' ')
        ++pos;
    size_t numStart = pos;
    while (pos < json.size() && (std::isdigit(static_cast<unsigned char>(json[pos])) || json[pos] == '-'))
        ++pos;
    if (pos == numStart)
        return std::nullopt;
    meta.timestampUnix = std::strtoll(json.substr(numStart, pos - numStart).c_str(), nullptr, 10);

    return meta;
}

std::optional<Recoverable> findRecoverable()
{
    std::string dir = directory();
    if (dir.empty())
        return std::nullopt;

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (ec || entry.path().extension() != ".meta")
            continue;

        auto meta = readMeta(entry.path().string());
        if (!meta)
            continue;

        fs::path autosavePath = entry.path();
        autosavePath.replace_extension(".ustudio");
        if (!fs::exists(autosavePath, ec))
            continue;

        bool recoverable;
        if (meta->originalPath.empty()) {
            recoverable = true; // untitled: any surviving autosave is unsaved work
        } else if (!fs::exists(meta->originalPath, ec)) {
            recoverable = true; // the file it would have saved over is gone
        } else {
            std::error_code mtimeEc;
            auto autosaveTime = fs::last_write_time(autosavePath, mtimeEc);
            auto originalTime = fs::last_write_time(meta->originalPath, mtimeEc);
            recoverable = !mtimeEc && autosaveTime > originalTime;
        }

        if (recoverable)
            return Recoverable{autosavePath.string(), entry.path().string(), *meta};
    }

    return std::nullopt;
}

} // namespace ustudio::app::autosave
