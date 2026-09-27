#include "log.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace ustudio::core {

namespace {

std::mutex g_mutex;
std::ofstream g_file;
LogLevel g_level = LogLevel::Info;
// The last lines written (at the current level), for Help's Copy
// Diagnostics: kept here rather than read back from the file.
constexpr size_t kRecentLines = 200;
std::deque<std::string> g_recent;
std::filesystem::path g_filePath; // this run's log file, once init() opened it

const char *levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::None:
        return "NONE";
    }
    return "?";
}

} // namespace

namespace Log {

LogLevel defaultLevel()
{
    // Debug in debug builds (raised from Info on 2026-09-20: a real crash
    // and a "playback stopped working" report were both hard to diagnose
    // from Info-level logs alone). Info in release builds, which the owner
    // runs day to day: Debug there wrote a 218 KB log per session.
    // USTUDIO_LOG_LEVEL overrides either way.
#if USTUDIO_DEBUG_BUILD
    return LogLevel::Debug;
#else
    return LogLevel::Info;
#endif
}

LogLevel levelFromValue(const char *env)
{
    if (!env)
        return defaultLevel();
    std::string v(env);
    for (auto &c : v)
        c = static_cast<char>(std::tolower(c));
    if (v == "debug")
        return LogLevel::Debug;
    if (v == "info")
        return LogLevel::Info;
    if (v == "warn" || v == "warning")
        return LogLevel::Warn;
    if (v == "error")
        return LogLevel::Error;
    if (v == "none" || v == "off")
        return LogLevel::None;
    return defaultLevel();
}

} // namespace Log

namespace {

std::string timestampForLine()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tmBuf;
    localtime_r(&t, &tmBuf);
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string timestampForFilename()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    localtime_r(&t, &tmBuf);
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y%m%d-%H%M%S");
    return oss.str();
}

// $XDG_STATE_HOME/ustudio/logs/, falling back to the XDG-spec default
// ~/.local/state/ustudio/logs/ if XDG_STATE_HOME is unset. Moved here from
// a plain ./logs/ (relative to cwd) in v2's M0 restructure (doc 14) — a
// deliberate, documented exception to M0's "no behaviour change" rule.
std::filesystem::path directoryFromEnvironment()
{
    if (const char *stateHome = std::getenv("XDG_STATE_HOME"); stateHome && *stateHome)
        return std::filesystem::path(stateHome) / "ustudio" / "logs";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "state" / "ustudio" / "logs";
    return std::filesystem::path("ustudio-logs"); // last-resort cwd fallback if even HOME is unset
}

void writeLine(LogLevel level, const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (level == LogLevel::None || static_cast<int>(level) > static_cast<int>(g_level))
        return;

    std::string line = "[" + timestampForLine() + "] [" + levelName(level) + "] " + message;
    g_recent.push_back(line);
    if (g_recent.size() > kRecentLines)
        g_recent.pop_front();
    std::cerr << line << std::endl;
    if (g_file.is_open()) {
        g_file << line << std::endl;
    }
}

} // namespace

namespace Log {

void init(const std::string &appName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = levelFromValue(std::getenv("USTUDIO_LOG_LEVEL"));

    std::filesystem::path dir = directoryFromEnvironment();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // ignore failure; any error just means no file sink

    std::filesystem::path path = dir / (appName + "-" + timestampForFilename() + ".log");
    g_file.open(path, std::ios::out | std::ios::trunc);
    if (g_file.is_open())
        g_filePath = path;
}

std::filesystem::path directory()
{
    return directoryFromEnvironment();
}

std::filesystem::path currentFile()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_filePath;
}

std::vector<std::string> recentLines(size_t count)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t take = std::min(count, g_recent.size());
    return {g_recent.end() - static_cast<std::ptrdiff_t>(take), g_recent.end()};
}

void setLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel level()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

void error(const std::string &message)
{
    writeLine(LogLevel::Error, message);
}

void warn(const std::string &message)
{
    writeLine(LogLevel::Warn, message);
}

void info(const std::string &message)
{
    writeLine(LogLevel::Info, message);
}

void debug(const std::string &message)
{
    writeLine(LogLevel::Debug, message);
}

} // namespace Log

} // namespace ustudio::core
