#include "autosave.h"

#include <glib.h>

#include <signal.h>

#include <cctype>
#include <cerrno>
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

// kill(pid, 0) sends no signal -- it only probes whether `pid` names a
// process this user could signal (POSIX kill(2)): ESRCH means no such
// process; EPERM means one exists but is owned by someone else, which
// still counts as "alive" here. pid <= 0 (0 = unknown/legacy meta,
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
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM)
        return false;
    if (recordedStartTime == 0)
        return true;
    // processStartTime() returning 0 here (process exited in the
    // window between the kill() probe above and this read) compares
    // unequal to any real recordedStartTime, so it falls through to
    // "not alive" -- the safe default, and consistent with the process
    // genuinely being gone.
    return processStartTime(pid) == recordedStartTime;
}
} // namespace

int64_t processStartTime(int64_t pid)
{
    if (pid <= 0)
        return 0;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in)
        return 0;
    std::string line;
    if (!std::getline(in, line))
        return 0;

    // comm (field 2) is "(...)"; and can itself contain spaces or even
    // parentheses, so the only robust split point is the LAST ')' on the
    // line, not the first space -- verified against /proc/self/stat's
    // own real field layout (2026-09-23).
    size_t closeParen = line.rfind(')');
    if (closeParen == std::string::npos || closeParen + 1 >= line.size())
        return 0;

    std::istringstream rest(line.substr(closeParen + 1));
    std::string token;
    int tokenIndex = 0; // field 3 (state) is rest's 1st token, so field 22 is its 20th
    while (rest >> token) {
        if (++tokenIndex == 20)
            return std::strtoll(token.c_str(), nullptr, 10);
    }
    return 0;
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

} // namespace ustudio::app::autosave
