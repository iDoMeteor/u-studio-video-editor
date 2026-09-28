// ADR-019 point 6: GpuAcceleration's decisions, with a fake render tool.
// Runs on GSettings' memory backend (as test_settings), so nothing touches
// the real settings.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/gpu_acceleration.h"
#include "app/settings.h"
#include "platform/process.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ustudio::app;
namespace fs = std::filesystem;

namespace {

struct FakeLauncher : ProxyQueue::Launcher
{
    struct Started
    {
        std::vector<std::string> argv;
        std::function<void(const std::string &)> onLine;
        std::function<void(int)> onExit;
        bool cancelled = false;
    };
    std::vector<Started> started;

    struct Child : ProxyQueue::Child
    {
        FakeLauncher *launcher;
        size_t index;
        Child(FakeLauncher *l, size_t i) : launcher(l), index(i) {}
        void cancel() override
        {
            launcher->started[index].cancelled = true;
        }
    };

    std::unique_ptr<ProxyQueue::Child> start(const std::vector<std::string> &argv,
                                             std::function<void(const std::string &)> onLine,
                                             std::function<void(int)> onExit) override
    {
        started.push_back({argv, std::move(onLine), std::move(onExit)});
        return std::make_unique<Child>(this, started.size() - 1);
    }

    // The probe's last run answers.
    void finish(const std::string &line, int status)
    {
        REQUIRE(!started.empty());
        Started &run = started.back();
        if (!line.empty())
            run.onLine(line);
        run.onExit(status);
    }
};

const std::string kPass = R"({"status":"ok","renderer":"Test GPU","version":"4.6"})";
const std::string kFail = R"({"status":"error","message":"no EGL display","renderer":"","version":""})";

struct Harness
{
    Settings settings;
    FakeLauncher *launcher = new FakeLauncher;
    fs::path sentinel =
        fs::temp_directory_path() / ("ustudio-gpu-" + std::to_string(ustudio::platform::currentProcessId())) / "live";
    std::vector<std::pair<bool, std::string>> pipeline; // setPipeline() calls
    std::vector<std::string> decode;                    // setHardwareDecode() calls
    std::vector<std::string> notes;
    std::unique_ptr<GpuAcceleration> gpu;

    explicit Harness(bool enabled = true, std::string cache = "")
    {
        settings.setGpuAcceleration(enabled);
        settings.setHardwareDecode(true);
        settings.setGpuProbeCache(cache);
        fs::remove_all(sentinel.parent_path());
        gpu = std::make_unique<GpuAcceleration>(
            settings, std::unique_ptr<ProxyQueue::Launcher>(launcher), "/bin/u-studio-render", sentinel, "1.0|7.40",
            "vaapi",
            GpuAcceleration::Hooks{
                .setPipeline = [this](bool on, const std::string &api) { pipeline.emplace_back(on, api); },
                .setHardwareDecode = [this](const std::string &api) { decode.push_back(api); },
                .notify = [this](const std::string &message) { notes.push_back(message); },
                .changed = {},
            });
    }
    ~Harness()
    {
        gpu.reset();
        fs::remove_all(sentinel.parent_path());
    }
};

} // namespace

TEST_CASE("GpuAcceleration: off does nothing at all")
{
    Harness h(false);
    h.gpu->start();
    CHECK(h.gpu->status() == GpuAcceleration::Status::Off);
    CHECK(h.launcher->started.empty());
    CHECK(h.pipeline.empty());
}

TEST_CASE("GpuAcceleration: a first run probes, then turns the pipeline on with hardware decode")
{
    Harness h;
    h.gpu->start();
    CHECK(h.gpu->status() == GpuAcceleration::Status::Checking);
    REQUIRE(h.launcher->started.size() == 1);
    CHECK(h.launcher->started[0].argv == std::vector<std::string>{"/bin/u-studio-render", "--gpu-probe"});
    CHECK(h.pipeline.empty()); // nothing cached: wait for the probe

    h.launcher->finish(kPass, 0);
    REQUIRE(h.pipeline.size() == 1);
    CHECK(h.pipeline[0] == std::make_pair(true, std::string("vaapi")));
    CHECK(h.settings.gpuProbeCache() == "1.0|7.40\t1\tTest GPU");

    h.gpu->engineChanged(true, "Test GPU");
    CHECK(h.gpu->status() == GpuAcceleration::Status::On);
    CHECK(h.gpu->statusText() == "On: Test GPU");
    CHECK(fs::exists(h.sentinel)); // live: a crash now leaves it behind

    h.gpu->shutdown();
    CHECK_FALSE(fs::exists(h.sentinel));
    CHECK(h.notes.empty());
}

TEST_CASE("GpuAcceleration: a cached pass turns it on at once; a failing re-check turns it off again")
{
    Harness h(true, "1.0|7.40\t1\tTest GPU");
    h.gpu->start();
    REQUIRE(h.pipeline.size() == 1);
    CHECK(h.pipeline[0].first);
    REQUIRE(h.launcher->started.size() == 1); // re-checked in the background anyway
    h.gpu->engineChanged(true, "Test GPU");

    h.launcher->finish(kFail, 1);
    REQUIRE(h.pipeline.size() == 2);
    CHECK_FALSE(h.pipeline[1].first);
    CHECK(h.gpu->status() == GpuAcceleration::Status::Unavailable);
    CHECK(h.gpu->statusText() == "Not available here: no EGL display");
    CHECK(h.settings.gpuProbeCache() == "1.0|7.40\t0\tno EGL display");
    REQUIRE(h.notes.size() == 1);
    h.gpu->engineChanged(false, ""); // the engine's answer to the switch-off
    CHECK(h.gpu->status() == GpuAcceleration::Status::Unavailable);
    CHECK_FALSE(fs::exists(h.sentinel));
}

TEST_CASE("GpuAcceleration: a cache from other versions isn't trusted")
{
    Harness h(true, "0.9|7.39\t1\tOld GPU");
    h.gpu->start();
    CHECK(h.pipeline.empty());
    h.launcher->finish(kPass, 0);
    CHECK(h.pipeline.size() == 1);
}

TEST_CASE("GpuAcceleration: a probe that crashes counts as a failure")
{
    Harness h;
    h.gpu->start();
    h.launcher->finish("", 139);
    CHECK(h.pipeline.empty());
    CHECK(h.gpu->status() == GpuAcceleration::Status::Unavailable);
    CHECK(h.gpu->detail() == "the GPU check crashed or couldn't run");
    CHECK(h.notes.empty()); // it was never on
}

TEST_CASE("GpuAcceleration: a session that died with the pipeline live turns the setting off")
{
    Harness h(true, "1.0|7.40\t1\tTest GPU");
    fs::create_directories(h.sentinel.parent_path());
    std::ofstream(h.sentinel) << "Test GPU\n";
    h.gpu->start();
    CHECK_FALSE(h.settings.gpuAcceleration());
    CHECK(h.gpu->status() == GpuAcceleration::Status::Off);
    CHECK(h.pipeline.empty());
    CHECK(h.launcher->started.empty());
    CHECK_FALSE(fs::exists(h.sentinel));
    REQUIRE(h.notes.size() == 1);

    h.gpu->setEnabled(true); // the user turns it back on
    CHECK(h.settings.gpuAcceleration());
    CHECK(h.pipeline.size() == 1);
}

TEST_CASE("GpuAcceleration: the engine falling back is reported, and the switches reach the engine")
{
    Harness h(true, "1.0|7.40\t1\tTest GPU");
    h.gpu->start();
    h.launcher->finish(kPass, 0);
    h.gpu->engineChanged(true, "Test GPU");

    h.gpu->setHardwareDecode(false);
    CHECK(h.decode == std::vector<std::string>{""});
    h.gpu->setHardwareDecode(true);
    CHECK(h.decode.back() == "vaapi");

    h.gpu->engineChanged(false, "the render thread couldn't use the GL context");
    CHECK(h.gpu->status() == GpuAcceleration::Status::Stopped);
    CHECK_FALSE(fs::exists(h.sentinel));
    REQUIRE(h.notes.size() == 1);

    h.gpu->setEnabled(false);
    CHECK(h.gpu->status() == GpuAcceleration::Status::Off);
    CHECK_FALSE(h.settings.gpuAcceleration());
}

TEST_CASE("GpuAcceleration: switched off by the user, the pipeline goes and nothing is reported")
{
    Harness h(true, "1.0|7.40\t1\tTest GPU");
    h.gpu->start();
    h.gpu->engineChanged(true, "Test GPU");
    h.gpu->setEnabled(false);
    REQUIRE(h.pipeline.size() == 2);
    CHECK_FALSE(h.pipeline[1].first);
    CHECK(h.launcher->started[0].cancelled); // the background re-check too
    h.gpu->engineChanged(false, "");
    CHECK(h.gpu->status() == GpuAcceleration::Status::Off);
    CHECK(h.notes.empty());
}
