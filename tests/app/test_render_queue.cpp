// app::RenderQueue with a fake backend and a hand-drained main thread.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/render_queue.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ustudio;
using namespace std::chrono_literals;

namespace {

// The "main thread": posted callbacks wait here until drain() runs them.
struct MainLoop
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::function<void()>> posted;

    void post(std::function<void()> fn)
    {
        std::lock_guard lock(mutex);
        posted.push_back(std::move(fn));
        cv.notify_all();
    }
    // Runs what's posted until `done` holds (or 5 s pass).
    bool runUntil(const std::function<bool()> &done)
    {
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!done()) {
            std::vector<std::function<void()>> batch;
            {
                std::unique_lock lock(mutex);
                if (!cv.wait_until(lock, deadline, [&] { return !posted.empty(); }))
                    return false;
                batch.swap(posted);
            }
            for (auto &fn : batch)
                fn();
        }
        return true;
    }
};

// A backend that renders until told to finish (or cancelled).
struct FakeBackend
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> started; // output paths, in order
    int concurrent = 0;
    int maxConcurrent = 0;
    int releases = 0; // how many renders may finish

    app::RenderQueue::Backend fn()
    {
        return [this](const app::RenderJob &job, std::string &error, std::function<void(int, int)> onProgress,
                      const std::atomic<bool> &cancel) {
            {
                std::lock_guard lock(mutex);
                started.push_back(job.outputPath);
                maxConcurrent = std::max(maxConcurrent, ++concurrent);
            }
            onProgress(1, 2);
            std::unique_lock lock(mutex);
            while (releases == 0 && !cancel)
                cv.wait_for(lock, 5ms);
            --concurrent;
            if (cancel) {
                error = "Render cancelled";
                return false;
            }
            --releases;
            return true;
        };
    }
    void release()
    {
        std::lock_guard lock(mutex);
        ++releases;
        cv.notify_all();
    }
};

struct Finished
{
    std::string path;
    bool ok;
    bool cancelled;
};

struct Fixture
{
    MainLoop loop;
    FakeBackend backend;
    std::vector<Finished> finished;
    std::vector<double> progress;
    app::RenderQueue queue{
        backend.fn(), [this](std::function<void()> fn) { loop.post(std::move(fn)); },
        app::RenderQueue::Callbacks{
            .started = {},
            .progress = [this](const app::RenderJob &, double f) { progress.push_back(f); },
            .finished = [this](const app::RenderJob &job, bool ok, bool cancelled,
                               const std::string &) { finished.push_back({job.outputPath, ok, cancelled}); },
        }};

    static app::RenderJob job(const std::string &path)
    {
        app::RenderJob j;
        j.outputPath = path;
        return j;
    }
};

} // namespace

TEST_CASE("RenderQueue: jobs run one at a time, in order")
{
    Fixture f;
    f.queue.enqueue(Fixture::job("a.mp4"));
    f.queue.enqueue(Fixture::job("b.mp4"));
    f.queue.enqueue(Fixture::job("c.mp4"));
    CHECK(f.queue.busy());
    CHECK(f.queue.queued() == 2);
    REQUIRE(f.queue.running() != nullptr);
    CHECK(f.queue.running()->outputPath == "a.mp4");

    for (int i = 0; i < 3; ++i)
        f.backend.release();
    REQUIRE(f.loop.runUntil([&] { return f.finished.size() == 3; }));
    CHECK(f.finished[0].path == "a.mp4");
    CHECK(f.finished[1].path == "b.mp4");
    CHECK(f.finished[2].path == "c.mp4");
    for (const Finished &done : f.finished)
        CHECK(done.ok);
    CHECK(f.backend.maxConcurrent == 1);
    CHECK_FALSE(f.queue.busy());
    CHECK_FALSE(f.progress.empty());
}

TEST_CASE("RenderQueue: cancelling the running job moves on to the next")
{
    Fixture f;
    f.queue.enqueue(Fixture::job("a.mp4"));
    f.queue.enqueue(Fixture::job("b.mp4"));
    f.queue.cancelCurrent();
    REQUIRE(f.loop.runUntil([&] { return f.finished.size() == 1; }));
    CHECK(f.finished[0].path == "a.mp4");
    CHECK(f.finished[0].cancelled);
    CHECK_FALSE(f.finished[0].ok);
    REQUIRE(f.queue.running() != nullptr);
    CHECK(f.queue.running()->outputPath == "b.mp4");
    f.backend.release();
    REQUIRE(f.loop.runUntil([&] { return f.finished.size() == 2; }));
    CHECK(f.finished[1].ok);
}

TEST_CASE("RenderQueue: shutdown stops the running job and hands back everything unfinished")
{
    Fixture f;
    f.queue.enqueue(Fixture::job("a.mp4"));
    f.queue.enqueue(Fixture::job("b.mp4"));
    f.queue.enqueue(Fixture::job("c.mp4"));
    std::vector<app::RenderJob> left = f.queue.shutdown();
    REQUIRE(left.size() == 3);
    CHECK(left[0].outputPath == "a.mp4");
    CHECK(left[2].outputPath == "c.mp4");
    CHECK_FALSE(f.queue.busy());
    // The worker's posted result is dropped; nothing more starts.
    f.loop.runUntil([&] { return true; });
    CHECK(f.finished.empty());
    CHECK(f.queue.enqueue(Fixture::job("d.mp4")) > 0);
    CHECK_FALSE(f.queue.busy());
}

TEST_CASE("RenderQueue: a job that finished before shutdown isn't handed back")
{
    Fixture f;
    f.queue.enqueue(Fixture::job("a.mp4"));
    f.backend.release();
    // Let it finish on its thread without delivering the result.
    for (int i = 0; i < 500; ++i) {
        std::lock_guard lock(f.backend.mutex);
        if (f.backend.releases == 0)
            break;
    }
    std::this_thread::sleep_for(50ms);
    std::vector<app::RenderJob> left = f.queue.shutdown();
    CHECK(left.empty());
}

#include "app/pending_renders.h"

#include <filesystem>
#include <unistd.h>

TEST_CASE("pending renders: saved jobs list back in order, with their profiles")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-pending-renders-" + std::to_string(::getpid()));
    struct Cleanup
    {
        fs::path dir;
        ~Cleanup()
        {
            fs::remove_all(dir);
        }
    } cleanup{dir};

    core::Model model = core::Model::createEmpty();
    model.addTrack(core::Track::Kind::Video, 0, "V1");
    std::vector<app::RenderJob> jobs(2);
    jobs[0].snapshot = model.snapshot();
    jobs[0].profile = core::builtInRenderProfiles()[0];
    jobs[0].outputPath = "/videos/show-high-quality.mp4";
    jobs[1].snapshot = model.snapshot();
    jobs[1].profile = core::RenderProfile{.name = "Mine", .height = 720, .quality = core::RenderProfile::Quality::Good};
    jobs[1].outputPath = "/videos/show-mine.mp4";
    REQUIRE(app::pending_renders::save(jobs, dir.string()).empty());

    std::vector<app::pending_renders::Pending> pending = app::pending_renders::list(dir.string());
    REQUIRE(pending.size() == 2);
    CHECK(pending[0].outputPath == "/videos/show-high-quality.mp4");
    CHECK(pending[0].profile == core::builtInRenderProfiles()[0]);
    CHECK(pending[1].profile == jobs[1].profile);
    CHECK(fs::exists(pending[1].projectPath));

    // Saving fewer replaces what was there; clear() empties it.
    REQUIRE(app::pending_renders::save({jobs[1]}, dir.string()).empty());
    CHECK(app::pending_renders::list(dir.string()).size() == 1);
    app::pending_renders::clear(dir.string());
    CHECK(app::pending_renders::list(dir.string()).empty());
}
