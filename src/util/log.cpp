#include "log.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>

namespace {

std::mutex g_mutex;
std::ofstream g_file;
LogLevel g_level = LogLevel::Info;

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

LogLevel levelFromEnv()
{
    const char *env = std::getenv("USTUDIO_LOG_LEVEL");
    if (!env)
        return LogLevel::Info;
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
    return LogLevel::Info;
}

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

void writeLine(LogLevel level, const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (level == LogLevel::None || static_cast<int>(level) > static_cast<int>(g_level))
        return;

    std::string line = "[" + timestampForLine() + "] [" + levelName(level) + "] " + message;
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
    g_level = levelFromEnv();

    mkdir("logs", 0755); // ignore EEXIST; any other failure just means no file sink

    std::string path = "logs/" + appName + "-" + timestampForFilename() + ".log";
    g_file.open(path, std::ios::out | std::ios::trunc);
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
