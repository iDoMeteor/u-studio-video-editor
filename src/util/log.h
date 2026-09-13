#pragma once

#include <string>

enum class LogLevel
{
    None = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
};

// Small, self-contained logger (no external dependency): writes timestamped
// lines to both stderr and logs/<appName>-<start-timestamp>.log, filtered by
// level. Safe to call from multiple threads (the engine's worker thread
// logs too).
namespace Log {

// Creates logs/ (relative to the current working directory) if needed and
// opens logs/<appName>-YYYYMMDD-HHMMSS.log. Initial level comes from the
// USTUDIO_LOG_LEVEL environment variable (debug/info/warn/error/none),
// defaulting to Info if unset or unrecognized.
void init(const std::string &appName);

void setLevel(LogLevel level);
LogLevel level();

void error(const std::string &message);
void warn(const std::string &message);
void info(const std::string &message);
void debug(const std::string &message);

} // namespace Log
