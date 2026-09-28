#pragma once

// The editor's effect scan (doc 15, "Health and cost probe"): in the
// background, once per plugin set, it fetches the registry from the render
// tool (--effects-registry) into the cache, then probes every offered effect
// in its own child process (--probe-effect), a few at a time, and records
// each result in effect-health.json. A child that crashes or hangs past its
// deadline is quarantined: never attached again (engine/effects_extension.h)
// and, from FX2, hidden unless "Show unstable effects" is on. The editor
// never runs a plugin it hasn't seen survive, beyond the playback of a
// project that already uses it.
//
// Construct, start and destroy on the main thread. The scan itself runs on
// its own thread (spawning a child from a process this size blocks for
// tens of milliseconds); progress arrives on the main loop.

#include "core/health.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ustudio::effects {

struct HealthScanOptions
{
    std::string renderTool;                // the u-studio-render to run
    std::filesystem::path registryCache;   // where the registry JSON goes
    std::filesystem::path healthFile;      // effect-health.json
    std::string fingerprint;               // results for another are dropped; empty: registryFingerprint()
    int parallel = 2;                      // children at once
    int deadlineSeconds = 20;              // then SIGKILL (a probe ignores the render tool's cancel)
    std::vector<std::string> onlyServices; // tests: probe exactly these, skip the registry
    // Extra environment for the children (tests: FREI0R_PATH).
    std::vector<std::pair<std::string, std::string>> environment;
};

class EffectRegistry;

class HealthScan
{
  public:
    // Called on the main loop after each result, and once with `finished`;
    // never after the scan is destroyed.
    using Progress = std::function<void(const std::string &service, const HealthRecord &record, bool finished)>;

    // The registry, once the scan has it (from the cache or the render
    // tool), on the main loop; never after the scan is destroyed.
    using RegistryReady = std::function<void(std::shared_ptr<const EffectRegistry> registry)>;

    HealthScan(HealthScanOptions options, Progress progress, RegistryReady registryReady = nullptr);
    ~HealthScan(); // stops the scan and kills any children still running
    HealthScan(const HealthScan &) = delete;
    HealthScan &operator=(const HealthScan &) = delete;

    void start();
    bool finished() const;
    HealthFile results() const; // as of the last result

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// The render tool: next to this program, else this build's, else on PATH.
std::string renderToolPath();

// The editor's scan, started once the window exists (register.cpp's shell
// extension). Quarantines take effect at the next graph build. The
// callbacks run on the main loop (the Rack's badges and effect list).
void startEditorHealthScan(HealthScan::Progress progress, HealthScan::RegistryReady registryReady);

} // namespace ustudio::effects
