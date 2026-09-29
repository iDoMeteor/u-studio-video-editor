#include "engine.h"

#include "core/log.h"
#include "core/trace.h"
#include "engine/engine_sync.h"
#include "engine/gpu_session.h"
#include "engine/playback_controller.h"

#include <glib.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

// Everything here runs on the engine thread except enqueue() (any thread)
// and the constructor/stop() (main thread).
class Engine::Thread
{
  public:
    struct Entry
    {
        std::function<void(Thread &)> command; // empty for a snapshot
        std::shared_ptr<const core::Project> snapshot;
        bool reset = false;
        bool shutdown = false;
        uint64_t seq = 0;
    };

    Thread(Engine *owner, std::shared_ptr<const core::Project> project, PreviewScale scale)
        : m_owner(owner), m_ownerToken(owner->m_lifetime)
    {
        m_thread = std::jthread([this, project = std::move(project), scale] { run(project, scale); });
    }

    // Appends in order, except that a snapshot replaces the newest pending
    // one when nothing but internal frame drains sits between them (latest
    // wins), keeping a reset flag if either had one. While playing, a drain
    // lands between every two edits, so collapsing only adjacent snapshots
    // never collapsed anything then (review of 4c30666). A main-thread
    // command in between keeps both: a seek sent after an edit must run on
    // that edit's graph, or it clamps to the old length.
    void enqueue(Entry entry)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            bool merged = false;
            if (isSnapshot(entry)) {
                for (auto it = m_queue.rbegin(); it != m_queue.rend(); ++it) {
                    if (it->command && it->seq == kInternal)
                        continue; // a drain: safe to hop over
                    if (isSnapshot(*it)) {
                        it->reset = it->reset || entry.reset;
                        it->snapshot = std::move(entry.snapshot);
                        it->seq = entry.seq;
                        merged = true;
                    }
                    break;
                }
            }
            if (!merged)
                m_queue.push_back(std::move(entry));
        }
        m_cv.notify_one();
    }

    void stop(uint64_t seq)
    {
        Entry entry;
        entry.shutdown = true;
        entry.seq = seq;
        enqueue(std::move(entry));
        if (m_thread.joinable())
            m_thread.join();
    }

    std::unique_ptr<EngineSync> sync;
    std::unique_ptr<PlaybackController> controller;
    std::shared_ptr<GpuSession> gpu; // ADR-019; null on the CPU pipeline (an export may hold it too)

    // ADR-019. On: the session first (movit's normalisers from then on),
    // the render-thread hooks while no consumer has them, then the GPU graph,
    // whose rebuild restarts the consumer with them. Off: the consumer
    // stops first (its render thread releases the context), then the hooks
    // go, the session ends and the CPU graph is built.
    void enableGpu(const std::string &hardwareDecodeApi)
    {
        if (gpu)
            return;
        std::string error;
        gpu = GpuSession::acquire(error);
        if (!gpu) {
            Log::warn("[gpu] staying on the CPU pipeline: " + error);
            postGpu(false, error);
            return;
        }
        controller->setRenderThreadHooks(
            [this] {
                if (!gpu->renderThreadStarted())
                    fallBackToCpu("the render thread couldn't use the GL context");
            },
            [this] { gpu->renderThreadStopped(); });
        controller->setFrameShowHooks([this] { return gpu->frameShowEnter(); }, [this] { gpu->frameShowLeave(); });
        gpu->setHardwareDecodeApi(hardwareDecodeApi);
        sync->setPipeline(EngineSync::Pipeline::Gpu, hardwareDecodeApi);
        postGpu(true, gpu->renderer());
    }

    void disableGpu(const std::string &why)
    {
        if (!gpu)
            return;
        controller->shutdown();
        controller->setRenderThreadHooks({}, {});
        controller->setFrameShowHooks({}, {});
        gpu.reset();
        sync->setPipeline(EngineSync::Pipeline::Cpu, {}); // rebuilds: `rebuilt` restarts the consumer
        postGpu(false, why);
    }

  private:
    void run(std::shared_ptr<const core::Project> project, PreviewScale scale)
    {
        pthread_setname_np(pthread_self(), "ustudio-engine");
        {
            core::trace::Scope trace("engine: start");
            // Its frames are read only by the consumer, at the profile's size.
            sync = std::make_unique<EngineSync>(std::move(project), scale, EngineSync::FrameReads::ProfileSize);
            controller = std::make_unique<PlaybackController>();
            // The consumer thread's frame hand-off lands here, not on the
            // main thread: its loop wrap seeks the tractor, which only this
            // thread touches. The UI gets the frame from onControllerFrame().
            controller->setDrainPoster([this](std::function<void()> drain) {
                Entry entry;
                entry.command = [drain = std::move(drain)](Thread &) { drain(); };
                entry.seq = kInternal;
                enqueue(std::move(entry));
            });
            controller->setFrameCallback([this](std::vector<uint8_t> rgba, int width, int height, int position) {
                onControllerFrame(std::move(rgba), width, height, position);
            });
            sync->rebuilt.connect([this] {
                controller->setTractor(sync->tractorPtr());
                m_rebuilt = true;
            });
            // IP3: effect values changed in place, no new graph: a paused
            // consumer redraws its frame (seek purges and refreshes).
            sync->appliedInPlace.connect([this] {
                if (!controller->isPlaying())
                    controller->seek(controller->currentFrame());
            });
            sync->mediaUnavailable.connect([this](const std::string &path) {
                MainThreadDispatcher::post(m_ownerToken,
                                           [owner = m_owner, path] { owner->mediaUnavailable.emit(path); });
            });
            controller->setTractor(sync->tractorPtr());
            m_rebuilt = true;
            postState(0);
        }

        while (true) {
            Entry entry;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return !m_queue.empty(); });
                entry = std::move(m_queue.front());
                m_queue.pop_front();
            }
            if (entry.shutdown) {
                core::trace::Scope trace("engine: shutdown");
                // Stop the consumer first, then drop the graph (CLAUDE.md:
                // never destroy what a running consumer can reach). Anything
                // still queued behind this is dropped unrun.
                controller->shutdown();
                controller.reset();
                sync.reset();
                gpu.reset(); // after the graph: its producers were opened under it
                return;
            }
            // The stall monitor only watches the main thread; this is the
            // engine thread's own over-budget log (doc 19 MT2).
            auto started = std::chrono::steady_clock::now();
            const char *what = entry.command ? "command" : entry.reset ? "reset" : "new snapshot";
            if (entry.command) {
                entry.command(*this);
            } else if (entry.reset) {
                sync->reset(std::move(entry.snapshot));
            } else {
                sync->setProject(std::move(entry.snapshot));
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (ms > kSlowEntryMs)
                Log::debug("[engine] engine thread busy " + std::to_string(static_cast<int>(ms)) + " ms: " + what);
            if (entry.seq != kInternal)
                postState(entry.seq);
            else if (m_rebuilt)
                postState(m_lastSeq);
        }
    }

    // Render thread: queues the switch for the engine thread.
    void fallBackToCpu(std::string why)
    {
        Log::error("[gpu] falling back to the CPU pipeline: " + why);
        Entry entry;
        entry.command = [why = std::move(why)](Thread &t) { t.disableGpu(why); };
        entry.seq = kInternal;
        enqueue(std::move(entry));
    }

    void postGpu(bool on, std::string detail)
    {
        MainThreadDispatcher::post(m_ownerToken, [owner = m_owner, on, detail = std::move(detail)] {
            owner->m_gpuPipeline = on;
            owner->gpuChanged.emit(on, detail);
        });
    }

    void postState(uint64_t seq)
    {
        m_lastSeq = seq;
        State state;
        state.seq = seq;
        state.position = controller->currentFrame();
        state.playing = controller->isPlaying();
        state.speed = controller->speed();
        state.totalFrames = controller->totalFrames();
        state.fps = controller->fps();
        state.previewFactor = sync->previewFactor();
        state.backend = controller->backendName();
        bool rebuilt = std::exchange(m_rebuilt, false);
        MainThreadDispatcher::post(m_ownerToken,
                                   [owner = m_owner, state, rebuilt] { owner->applyState(state, rebuilt); });
    }

    // Engine thread (PlaybackController's drain, after its loop check): hands
    // the newest frame to the main thread, one post in flight at a time.
    void onControllerFrame(std::vector<uint8_t> rgba, int width, int height, int position)
    {
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_frame = Frame{std::move(rgba), width, height, position};
        }
        if (m_frameQueued.exchange(true))
            return;
        MainThreadDispatcher::post(m_ownerToken, [this, owner = m_owner] {
            // The owner's token being alive means shutdown() hasn't
            // returned, so this Thread still exists.
            m_frameQueued = false;
            std::optional<Frame> frame;
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                frame = std::exchange(m_frame, std::nullopt);
            }
            if (frame)
                owner->onFrame(std::move(frame->rgba), frame->width, frame->height, frame->position);
        });
    }

    struct Frame
    {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        int position = 0;
    };
    static bool isSnapshot(const Entry &entry)
    {
        return !entry.command && !entry.shutdown;
    }
    static constexpr double kSlowEntryMs = 50.0;
    static constexpr uint64_t kInternal = UINT64_MAX; // engine-originated work, not a main-thread command

    Engine *m_owner;
    std::weak_ptr<void> m_ownerToken;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Entry> m_queue;
    bool m_rebuilt = false; // engine thread only
    uint64_t m_lastSeq = 0; // engine thread only
    std::mutex m_frameMutex;
    std::optional<Frame> m_frame;
    std::atomic<bool> m_frameQueued{false};
    std::jthread m_thread;
};

Engine::Engine(std::shared_ptr<const core::Project> project, PreviewScale previewScale)
    : m_thread(std::make_unique<Thread>(this, std::move(project), previewScale))
{}

Engine::~Engine()
{
    shutdown();
}

void Engine::shutdown()
{
    if (!m_thread)
        return;
    Log::debug("[engine] shutting down the engine thread");
    m_thread->stop(++m_sent);
    m_thread.reset();
    // Frames and state still queued for the main thread would call into a
    // stopped engine's mirror only; they're harmless, but drop them anyway.
    m_lifetime = MainThreadDispatcher::makeToken();
}

uint64_t Engine::send(std::function<void(Thread &)> command)
{
    if (!m_thread)
        return m_sent;
    Thread::Entry entry;
    entry.command = std::move(command);
    entry.seq = ++m_sent;
    m_thread->enqueue(std::move(entry));
    return m_sent;
}

void Engine::publish(std::shared_ptr<const core::Project> project)
{
    if (!m_thread || !project)
        return;
    Thread::Entry entry;
    entry.snapshot = std::move(project);
    entry.seq = ++m_sent;
    m_thread->enqueue(std::move(entry));
}

void Engine::reset(std::shared_ptr<const core::Project> project)
{
    if (!m_thread || !project)
        return;
    Thread::Entry entry;
    entry.snapshot = std::move(project);
    entry.reset = true;
    entry.seq = ++m_sent;
    m_thread->enqueue(std::move(entry));
}

void Engine::setUseProxies(bool use)
{
    send([use](Thread &t) { t.sync->setUseProxies(use); });
}

void Engine::setHardwareDecode(std::string api)
{
    send([api = std::move(api)](Thread &t) {
        if (t.gpu)
            t.gpu->setHardwareDecodeApi(api); // exports follow the preview
        t.sync->setHardwareDecode(api);
    });
}

void Engine::setGpuPipeline(bool on, std::string hardwareDecodeApi)
{
    send([on, api = std::move(hardwareDecodeApi)](Thread &t) {
        if (on)
            t.enableGpu(api);
        else
            t.disableGpu({});
    });
}

void Engine::setPreviewScale(PreviewScale scale)
{
    send([scale](Thread &t) { t.sync->setPreviewScale(scale); });
}

void Engine::setFrameCallback(FrameCallback callback)
{
    m_frameCallback = std::move(callback);
}

int Engine::clampFrame(int frame) const
{
    return std::clamp(frame, 0, std::max(m_totalFrames - 1, 0));
}

void Engine::play(double speed)
{
    m_playing = speed != 0.0;
    m_speed = speed;
    send([speed](Thread &t) { t.controller->play(speed); });
}

void Engine::pause()
{
    m_playing = false;
    m_speed = 0.0;
    send([](Thread &t) { t.controller->pause(); });
}

void Engine::togglePlay()
{
    if (m_playing)
        pause();
    else
        play(1.0);
}

void Engine::seek(int frame)
{
    m_position = clampFrame(frame);
    send([frame](Thread &t) { t.controller->seek(frame); });
}

void Engine::stepFrame(int delta)
{
    m_playing = false;
    m_speed = 0.0;
    m_position = clampFrame(m_position + delta);
    send([delta](Thread &t) { t.controller->stepFrame(delta); });
}

void Engine::toHome()
{
    m_position = 0;
    send([](Thread &t) { t.controller->toHome(); });
}

void Engine::toEnd()
{
    m_position = std::max(m_totalFrames - 1, 0);
    send([](Thread &t) { t.controller->toEnd(); });
}

void Engine::setLoopRange(std::optional<std::pair<int, int>> range)
{
    if (range && range->first >= range->second)
        range.reset(); // as PlaybackController does
    m_loopRange = range;
    send([range](Thread &t) { t.controller->setLoopRange(range); });
}

void Engine::setVolume(double volume)
{
    m_volume = std::clamp(volume, 0.0, 1.0);
    send([volume = m_volume](Thread &t) { t.controller->setVolume(volume); });
}

void Engine::applyState(const State &state, bool graphRebuilt)
{
    m_totalFrames = state.totalFrames;
    m_fps = state.fps;
    m_previewFactor = state.previewFactor;
    m_backend = state.backend;
    if (state.seq >= m_sent) {
        // Nothing newer has been sent: this is where the engine really is.
        m_playing = state.playing;
        m_speed = state.speed;
        if (!m_playing)
            m_position = state.position;
    }
    m_applied = std::max(m_applied, state.seq);
    if (graphRebuilt)
        rebuilt.emit();
}

void Engine::onFrame(std::vector<uint8_t> rgba, int width, int height, int position)
{
    if (m_playing)
        m_position = position;
    if (m_frameCallback)
        m_frameCallback(std::move(rgba), width, height, position);
}

int Engine::consumerRestartsForTesting()
{
    auto count = std::make_shared<std::atomic<int>>(-1);
    send([count](Thread &t) { count->store(t.controller->consumerRestartCount()); });
    if (!syncForTesting())
        return -1;
    return count->load();
}

bool Engine::syncForTesting()
{
    uint64_t target = send([](Thread &) {});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (m_applied < target && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return m_applied >= target;
}

} // namespace ustudio::engine
