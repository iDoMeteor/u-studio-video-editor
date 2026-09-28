#pragma once

// The health and cost probe's results (doc 15, "Health and cost probe"). A
// probe runs one service in a child process (u-studio-render
// --probe-effect, engine/probe.cpp) and prints one JSON line; the editor's
// scan (app/health_scan.cpp) collects them into effect-health.json, keyed by
// the plugin set's fingerprint so a changed install is probed again.

#include "core/json.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace ustudio::effects {

enum class HealthStatus
{
    Ok,
    Crashed,     // the child died (a signal, or no result line)
    TimedOut,    // the child ran past its deadline and was stopped
    BadOutput,   // it ran, but the picture came out all one flat colour, or audio NaN
    Unavailable, // MLT couldn't create the service at all
};
const char *healthStatusName(HealthStatus status);
std::optional<HealthStatus> healthStatusFromName(const std::string &name);

struct HealthRecord
{
    HealthStatus status = HealthStatus::Ok;
    std::string reason;      // one line, for "Show unstable effects" and the log
    double msPerFrame = 0.0; // at 1080p, with defaults (the cost badge)

    bool usable() const
    {
        return status == HealthStatus::Ok;
    }
    bool operator==(const HealthRecord &) const = default;
};

// Cost badges (doc 15, "Effect Browser"): light under 8 ms a 1080p frame,
// heavy from 25 (a 30 fps frame is 33 ms).
enum class CostBadge
{
    Light,
    Medium,
    Heavy,
};
CostBadge costBadge(double msPerFrame);

// The probe's result line: {"service":..,"status":..,"reason":..,"ms_per_frame":..}.
std::string probeResultLine(const std::string &service, const HealthRecord &record);
// Nullopt when `line` isn't one (the child's other output is ignored).
std::optional<std::pair<std::string, HealthRecord>> parseProbeResultLine(const std::string &line);
// The line the probe prints as each stage starts ("defaults", "half size"):
// the last one seen says where a child that died was.
std::string probeStageLine(const std::string &service, const std::string &stage);
std::optional<std::string> parseProbeStageLine(const std::string &line);

// What one probe child's run means (the editor's scan, app/health_scan.cpp):
// its result line if it printed one; else it crashed (named by the last
// stage it started) or, when the scan stopped it at its deadline, timed out.
HealthRecord interpretProbe(const std::string &service, const std::string &output, bool timedOut);

// effect-health.json: every probed service under one plugin-set fingerprint.
struct HealthFile
{
    std::string fingerprint;
    std::map<std::string, HealthRecord> records;

    // Nullopt when unknown (not probed yet: offered, since the default is to
    // trust until the scan says otherwise).
    std::optional<HealthRecord> find(const std::string &service) const;
    bool quarantined(const std::string &service) const;
};
Json toJson(const HealthFile &file);
HealthFile healthFileFromJson(const Json &json);

// effect-health.json on disk: empty when missing or unreadable; written
// through a temp file and a rename, so a reader never sees half of one.
HealthFile loadHealthFile(const std::filesystem::path &path);
bool saveHealthFile(const std::filesystem::path &path, const HealthFile &file);

} // namespace ustudio::effects
