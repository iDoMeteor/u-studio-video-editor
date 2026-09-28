#pragma once

// ADR-019 point 6: whether the editor plays on the GPU pipeline, and what
// Settings › Performance says about it. No GTK: AppWindow wires it to the
// engine, the Settings rows and toasts, and tests drive it with a fake
// launcher.
//
// - At startup, with "GPU acceleration" on (Automatic): a sentinel left by
//   a session that died with the GPU pipeline live turns the setting off
//   and says so; otherwise a cached probe pass for this app and MLT version
//   turns the pipeline on at once. The probe (`u-studio-render --gpu-probe`,
//   a child process, so a driver crash can't take the editor down) runs
//   again every launch, in the background, and its result corrects both
//   the cache and the pipeline.
// - While the pipeline is live, the sentinel file exists; a clean exit or
//   a switch back to the CPU removes it.
// - Hardware decode follows the pipeline: VAAPI costs frames on the CPU
//   path (ADR-019, "Evidence").

#include "proxy_queue.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ustudio::app {

class Settings;

class GpuAcceleration
{
  public:
    struct Hooks
    {
        // Engine::setGpuPipeline() and Engine::setHardwareDecode().
        std::function<void(bool on, const std::string &hardwareDecodeApi)> setPipeline;
        std::function<void(const std::string &api)> setHardwareDecode;
        // Something to tell the user (a toast).
        std::function<void(const std::string &message)> notify;
        // status() changed (the Settings row's subtitle).
        std::function<void()> changed;
    };
    enum class Status
    {
        Off,         // the setting is off
        Checking,    // waiting for the probe or the engine
        On,          // detail(): the GL renderer
        Unavailable, // detail(): why the probe failed
        Stopped,     // detail(): why the engine fell back
    };

    // `cacheKey`: gpuProbeCacheKey(). `decodeApi`: platform::hardwareDecodeApi().
    GpuAcceleration(Settings &settings, std::unique_ptr<ProxyQueue::Launcher> launcher, std::string renderTool,
                    std::filesystem::path sentinel, std::string cacheKey, std::string decodeApi, Hooks hooks);
    ~GpuAcceleration();

    void start();
    // The Settings switches.
    void setEnabled(bool on);
    void setHardwareDecode(bool on);
    // Engine::gpuChanged.
    void engineChanged(bool on, const std::string &detail);
    // A clean exit, after the engine has shut down.
    void shutdown();

    Status status() const
    {
        return m_status;
    }
    const std::string &detail() const
    {
        return m_detail;
    }
    // The Settings row's subtitle.
    std::string statusText() const;

  private:
    void useCacheOrProbe();
    void probe();
    void probeFinished(const std::string &output, int exitStatus);
    void requestOn();
    std::string decodeApi() const;
    void setStatus(Status status, std::string detail);

    Settings &m_settings;
    std::unique_ptr<ProxyQueue::Launcher> m_launcher;
    std::unique_ptr<ProxyQueue::Child> m_probe;
    std::string m_renderTool;
    std::filesystem::path m_sentinel;
    std::string m_cacheKey;
    std::string m_decodeApi;
    Hooks m_hooks;
    Status m_status = Status::Off;
    std::string m_detail;
    bool m_requested = false; // setPipeline(true) sent, no "off" since
    bool m_live = false;      // the engine said it's on
};

// "<app version>|<MLT version>": a cached probe result is trusted at startup
// only for the versions it was made with (and re-checked in the background).
std::string gpuProbeCacheKey(const std::string &appVersion, const std::string &mltVersion);

} // namespace ustudio::app
