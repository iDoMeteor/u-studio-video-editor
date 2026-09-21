#pragma once

#include <chrono>
#include <string>

namespace ustudio::core {

enum class LogLevel
{
    None = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
};

// Small, self-contained logger (no external dependency): writes timestamped
// lines to both stderr and $XDG_STATE_HOME/ustudio/logs/<appName>-<start-
// timestamp>.log, filtered by level. Safe to call from multiple threads
// (the engine's worker thread logs too).
namespace Log {

// Creates the log directory if needed and opens
// <appName>-YYYYMMDD-HHMMSS.log inside it. Initial level comes from the
// USTUDIO_LOG_LEVEL environment variable (debug/info/warn/error/none),
// defaulting to Debug if unset or unrecognized (see log.cpp's
// levelFromEnv() for why -- raised from Info while actively hunting
// real bugs; set USTUDIO_LOG_LEVEL=info explicitly for quieter logs).
void init(const std::string &appName);

void setLevel(LogLevel level);
LogLevel level();

void error(const std::string &message);
void warn(const std::string &message);
void info(const std::string &message);
void debug(const std::string &message);

// RAII wall-clock timer for perf instrumentation: logs "<label> took Xms"
// at debug level when it goes out of scope. Meant for the handful of
// operations worth watching for regressions or a slow outlier on a real
// project (EngineSync::rebuildAll/reset, renderProject, a waveform job) --
// not for anything called per-frame, since even a debug-level call still
// costs a mutex lock and a timestamp format in writeLine() (see log.cpp)
// whether or not the line is actually kept.
//
//   void EngineSync::rebuildAll() {
//       Log::ScopedTimer timer("[engine] rebuildAll");
//       ...
//   }
class ScopedTimer
{
  public:
    explicit ScopedTimer(std::string label) : m_label(std::move(label)), m_start(std::chrono::steady_clock::now()) {}
    ~ScopedTimer()
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start);
        debug(m_label + " took " + std::to_string(static_cast<double>(elapsed.count()) / 1000.0) + "ms");
    }
    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;

  private:
    std::string m_label;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace Log

} // namespace ustudio::core
