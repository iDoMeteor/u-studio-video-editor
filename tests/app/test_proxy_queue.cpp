// M4 C: ProxyQueue with a fake launcher standing in for u-studio-render.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/proxy_queue.h"

#include <string>
#include <vector>

using namespace ustudio;
using app::ProxyQueue;

namespace {

struct FakeChild;

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
    bool failToStart = false;

    std::unique_ptr<ProxyQueue::Child> start(const std::vector<std::string> &argv,
                                             std::function<void(const std::string &)> onLine,
                                             std::function<void(int)> onExit) override;
};

struct FakeChild : ProxyQueue::Child
{
    FakeLauncher *launcher;
    size_t index;
    FakeChild(FakeLauncher *l, size_t i) : launcher(l), index(i) {}
    void cancel() override
    {
        launcher->started[index].cancelled = true;
    }
};

std::unique_ptr<ProxyQueue::Child> FakeLauncher::start(const std::vector<std::string> &argv,
                                                       std::function<void(const std::string &)> onLine,
                                                       std::function<void(int)> onExit)
{
    if (failToStart) {
        onExit(-1);
        return nullptr;
    }
    started.push_back({argv, std::move(onLine), std::move(onExit)});
    return std::make_unique<FakeChild>(this, started.size() - 1);
}

struct Harness
{
    FakeLauncher *launcher = new FakeLauncher;
    std::vector<std::pair<uint64_t, double>> progress;
    std::vector<std::pair<uint64_t, std::string>> done, failed;
    ProxyQueue queue{std::unique_ptr<ProxyQueue::Launcher>(launcher), "/opt/tool/u-studio-render",
                     ProxyQueue::Callbacks{
                         .progress = [this](core::AssetId a, double f) { progress.emplace_back(a.value, f); },
                         .done = [this](core::AssetId a, const std::string &o) { done.emplace_back(a.value, o); },
                         .failed = [this](core::AssetId a, const std::string &m) { failed.emplace_back(a.value, m); },
                     }};
};

ProxyQueue::Job job(uint64_t asset)
{
    return {core::AssetId{asset},
            "/media/clip " + std::to_string(asset) + ".mov",
            "/cache/proxy-" + std::to_string(asset) + ".mp4",
            540,
            {30000, 1001}};
}

} // namespace

TEST_CASE("ProxyQueue: runs the tool with the job's settings and reports progress and the result")
{
    Harness h;
    h.queue.add(job(1));
    REQUIRE(h.launcher->started.size() == 1);
    CHECK(h.launcher->started[0].argv == std::vector<std::string>{"/opt/tool/u-studio-render", "--proxy",
                                                                  "/media/clip 1.mov", "/cache/proxy-1.mp4", "--height",
                                                                  "540", "--fps", "30000/1001"});
    CHECK(h.queue.busy());
    h.launcher->started[0].onLine(R"({"progress":0.25})");
    CHECK(h.queue.progressOf(core::AssetId{1}) == doctest::Approx(0.25));
    h.launcher->started[0].onLine(R"({"status":"ok","output":"/cache/proxy-1.mp4"})");
    h.launcher->started[0].onExit(0);
    CHECK(h.done == std::vector<std::pair<uint64_t, std::string>>{{1, "/cache/proxy-1.mp4"}});
    CHECK(h.progress.size() == 1);
    CHECK_FALSE(h.queue.busy());
    CHECK_FALSE(h.queue.progressOf(core::AssetId{1}));
}

TEST_CASE("ProxyQueue: one at a time, in order, no duplicates; failures carry the tool's message")
{
    Harness h;
    h.queue.add(job(1));
    h.queue.add(job(2));
    h.queue.add(job(1)); // already running
    h.queue.add(job(2)); // already waiting
    REQUIRE(h.launcher->started.size() == 1);
    CHECK(h.queue.progressOf(core::AssetId{2}) == doctest::Approx(0.0)); // waiting
    h.launcher->started[0].onLine(R"({"status":"error","message":"can't open \"clip 1.mov\""})");
    h.launcher->started[0].onExit(1);
    CHECK(h.failed == std::vector<std::pair<uint64_t, std::string>>{{1, "can't open \"clip 1.mov\""}});
    REQUIRE(h.launcher->started.size() == 2); // the next one started
    CHECK(h.launcher->started[1].argv[2] == "/media/clip 2.mov");
    h.launcher->started[1].onExit(137); // killed, no status line
    REQUIRE(h.failed.size() == 2);
    CHECK(h.failed[1].second.find("137") != std::string::npos);
}

TEST_CASE("ProxyQueue: cancelling asks the child to stop and reports neither done nor failed")
{
    Harness h;
    h.queue.add(job(1));
    h.queue.add(job(2));
    h.queue.cancel(core::AssetId{2}); // waiting: just dropped
    h.queue.cancel(core::AssetId{1}); // running: asked to stop
    CHECK(h.launcher->started[0].cancelled);
    h.launcher->started[0].onLine(R"({"status":"error","message":"Render cancelled"})");
    h.launcher->started[0].onExit(1);
    CHECK(h.done.empty());
    CHECK(h.failed.empty());
    CHECK(h.launcher->started.size() == 1); // 2 never ran
    CHECK_FALSE(h.queue.busy());
}

TEST_CASE("ProxyQueue: a tool that can't start fails the job and moves on")
{
    Harness h;
    h.launcher->failToStart = true;
    h.queue.add(job(1));
    REQUIRE(h.failed.size() == 1);
    CHECK(h.failed[0].second.find("couldn't be started") != std::string::npos);
    CHECK_FALSE(h.queue.busy());
}

TEST_CASE("proxyPathFor: in the cache, named for the file's identity and the height")
{
    const std::string a = app::proxyPathFor("100:1", "/media/a.mov", 540);
    CHECK(a.find("/ustudio/proxies/") != std::string::npos);
    CHECK(a.ends_with("-540.mp4"));
    CHECK(app::proxyPathFor("100:1", "/media/a.mov", 0).ends_with("-src.mp4"));
    CHECK(app::proxyPathFor("100:2", "/media/a.mov", 540) != a); // the file changed
    CHECK(app::proxyPathFor("100:1", "/media/a.mov", 540) == a);
}
