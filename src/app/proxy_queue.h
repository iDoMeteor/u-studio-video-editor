#pragma once

// M4 C: proxies are rendered by `u-studio-render --proxy` in child
// processes (ADR-009), a few at a time, with progress read from their JSON
// lines (render/proxy_command.h). No GTK here: the window supplies a
// Launcher (GSubprocess in the app, a fake in tests/app/test_proxy_queue).
// Main thread only; the Launcher calls back on it.

#include "core/model/frame_time.h"
#include "core/model/ids.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::app {

class ProxyQueue
{
  public:
    struct Job
    {
        core::AssetId asset;
        std::string source, output;
        int height = 540; // 0: source size
        core::Rational fps{30, 1};
        int sequenceBegin = 0, sequenceCount = 0; // an image sequence: source is its pattern
    };

    // A running child: cancel() asks it to stop (it then exits, and
    // onExit still comes).
    class Child
    {
      public:
        virtual ~Child() = default;
        virtual void cancel() = 0;
    };
    class Launcher
    {
      public:
        virtual ~Launcher() = default;
        // Runs `argv`; each stdout line comes to onLine, then onExit with
        // the exit status (-1 if it couldn't start). Main thread.
        virtual std::unique_ptr<Child> start(const std::vector<std::string> &argv,
                                             std::function<void(const std::string &line)> onLine,
                                             std::function<void(int status)> onExit) = 0;
    };

    struct Callbacks
    {
        std::function<void(core::AssetId, double fraction)> progress;
        std::function<void(core::AssetId, const std::string &output)> done;
        std::function<void(core::AssetId, const std::string &message)> failed; // not for a cancel
    };

    ProxyQueue(std::unique_ptr<Launcher> launcher, std::string toolPath, Callbacks callbacks, size_t concurrent = 1);

    // Queued; ignored while the asset already has a job.
    void add(Job job);
    void cancel(core::AssetId asset);
    void cancelAll();
    // A job's progress (0 while queued), or nullopt when it has none.
    std::optional<double> progressOf(core::AssetId asset) const;
    bool busy() const
    {
        return !m_running.empty() || !m_waiting.empty();
    }

    // The tool's command line for a job (tests check it).
    std::vector<std::string> commandFor(const Job &job) const;

  private:
    struct Running
    {
        Job job;
        std::unique_ptr<Child> child;
        double progress = 0.0;
        std::string error;
        bool ok = false, cancelled = false;
    };
    void startNext();
    void onLine(core::AssetId asset, const std::string &line);
    void onExit(core::AssetId asset, int status);

    std::unique_ptr<Launcher> m_launcher;
    std::string m_toolPath;
    Callbacks m_callbacks;
    size_t m_concurrent;
    std::deque<Job> m_waiting;
    std::map<uint64_t, Running> m_running;
};

// Where u-studio-render is: USTUDIO_RENDER_BIN if set, else next to this
// program (installed: the same bindir), else the build tree's
// (builddir/src/app -> builddir/src/render; the test editor's too). "" if
// none of those exists.
std::string locateRenderTool();

// The value after `"key":` in one of the render tool's JSON lines: a number
// or a string (with its escapes undone). Enough for the tool's own output
// (render/proxy_command.h, gpu_probe_command.h), not a general JSON parser.
std::optional<std::string> jsonField(const std::string &line, const std::string &key);

// This asset's proxy file in the user's cache ($XDG_CACHE_HOME/ustudio/
// proxies), named for the source file's identity and the height, so it
// never lands next to the owner's footage and a changed file gets a new one.
std::string proxyPathFor(const std::string &fingerprint, const std::string &sourcePath, int height);

} // namespace ustudio::app
