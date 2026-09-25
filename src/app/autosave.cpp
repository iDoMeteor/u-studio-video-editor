#include "autosave.h"

#include "platform/process.h"

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

// platform::processExists() counts another user's process as alive too.
// pid <= 0 (0 = unknown/legacy meta,
// negative = never valid) is never treated as alive. recordedStartTime
// cross-checks that the CURRENT process at that pid (if any) is the
// SAME one that wrote the autosave, not a coincidental reuse of the pid
// by an unrelated process (audit A3) -- 0 (a meta file from before this
// field existed) is permissive, matching readMeta's own "pid 0 =
// unknown owner" convention, rather than refusing every pre-A3 autosave.
bool ownerAlive(int64_t pid, int64_t recordedStartTime)
{
    if (pid <= 0)
        return false;
    if (!platform::processExists(pid))
        return false;
    if (recordedStartTime == 0)
        return true;
    // processStartTime() returning 0 here (process exited in the
    // window between the existence probe above and this read) compares
    // unequal to any real recordedStartTime, so it falls through to
    // "not alive" -- the safe default, and consistent with the process
    // genuinely being gone.
    return processStartTime(pid) == recordedStartTime;
}
} // namespace

int64_t processStartTime(int64_t pid)
{
    return platform::processStartTime(pid);
}

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
    json << "{\"path\":\"" << jsonEscape(meta.originalPath) << "\",\"timestamp\":" << meta.timestampUnix
         << ",\"pid\":" << meta.ownerPid << ",\"pid_start\":" << meta.ownerStartTime << "}";

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

    // Optional, unlike path/timestamp above: a meta file written before
    // this field existed simply has no "pid" key, and that's fine --
    // ownerPid stays 0 ("unknown owner"), which findRecoverable() treats
    // the same permissive way this whole check behaved before A5.
    size_t pidPos = json.find("\"pid\"");
    if (pidPos != std::string::npos) {
        pidPos = json.find(':', pidPos);
        if (pidPos != std::string::npos) {
            ++pidPos;
            while (pidPos < json.size() && json[pidPos] == ' ')
                ++pidPos;
            size_t pidNumStart = pidPos;
            while (pidPos < json.size() &&
                   (std::isdigit(static_cast<unsigned char>(json[pidPos])) || json[pidPos] == '-'))
                ++pidPos;
            if (pidPos > pidNumStart)
                meta.ownerPid = std::strtoll(json.substr(pidNumStart, pidPos - pidNumStart).c_str(), nullptr, 10);
        }
    }

    // Optional, same reasoning as "pid" above -- a meta file written
    // before audit A3 has no "pid_start" key; ownerStartTime stays 0
    // ("unknown"), which ownerAlive() treats permissively.
    size_t startPos = json.find("\"pid_start\"");
    if (startPos != std::string::npos) {
        startPos = json.find(':', startPos);
        if (startPos != std::string::npos) {
            ++startPos;
            while (startPos < json.size() && json[startPos] == ' ')
                ++startPos;
            size_t startNumStart = startPos;
            while (startPos < json.size() &&
                   (std::isdigit(static_cast<unsigned char>(json[startPos])) || json[startPos] == '-'))
                ++startPos;
            if (startPos > startNumStart)
                meta.ownerStartTime =
                    std::strtoll(json.substr(startNumStart, startPos - startNumStart).c_str(), nullptr, 10);
        }
    }

    return meta;
}

std::optional<Recoverable> findRecoverable(const std::set<std::string> &excludeMetaPaths)
{
    std::string dir = directory();
    if (dir.empty())
        return std::nullopt;

    std::optional<Recoverable> best;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (ec || entry.path().extension() != ".meta")
            continue;
        if (excludeMetaPaths.contains(entry.path().string()))
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

        // Audit A5: a still-running instance's own autosave can otherwise
        // look identical to an orphaned one from a crashed session --
        // offering it here risks the owner choosing "discard" and deleting
        // work the other instance is still actively writing.
        if (recoverable && ownerAlive(meta->ownerPid, meta->ownerStartTime))
            continue;
        if (!recoverable)
            continue;

        // Most recently written wins when several qualify -- see this
        // function's own doc comment for why more than one legitimately
        // can, and why "first the directory happened to return" was the
        // actual bug, not just theoretically imprecise.
        if (!best || meta->timestampUnix > best->meta.timestampUnix)
            best = Recoverable{autosavePath.string(), entry.path().string(), *meta};
    }

    return best;
}

bool autosaveDue(int64_t nowUsec, int64_t lastEditUsec, int64_t unsavedSinceUsec, int64_t delayUsec,
                 int64_t heartbeatUsec)
{
    if (unsavedSinceUsec == 0)
        return false;
    return nowUsec - lastEditUsec >= delayUsec || nowUsec - unsavedSinceUsec >= delayUsec - heartbeatUsec;
}

void removeAutosavePair(const std::string &dir, const std::string &base)
{
    std::error_code ec;
    for (const char *suffix : {".ustudio", ".meta"})
        std::filesystem::remove(std::filesystem::path(dir) / (base + suffix), ec);
}

void OwnAutosaves::written(const std::string &originalPath)
{
    m_written.insert(baseNameFor(originalPath, m_sessionId));
}

std::vector<std::string> OwnAutosaves::take(const std::string &base)
{
    if (m_written.erase(base) == 0)
        return {};
    return {base};
}

std::vector<std::string> OwnAutosaves::saved(const std::string &oldPath, const std::string &newPath, bool clean)
{
    std::vector<std::string> stale;
    const std::string oldBase = baseNameFor(oldPath, m_sessionId);
    const std::string newBase = baseNameFor(newPath, m_sessionId);
    if (oldBase != newBase)
        for (std::string &base : take(oldBase))
            stale.push_back(std::move(base));
    if (clean)
        for (std::string &base : take(newBase))
            stale.push_back(std::move(base));
    return stale;
}

std::vector<std::string> OwnAutosaves::quitClean()
{
    std::vector<std::string> stale(m_written.begin(), m_written.end());
    m_written.clear();
    return stale;
}

} // namespace ustudio::app::autosave
