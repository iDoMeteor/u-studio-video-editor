#include "app/health_scan.h"

#include "core/log.h"
#include "core/media/utf8_path.h"
#include "engine/dispatcher.h"
#include "engine/effects_extension.h"
#include "engine/plugins.h"
#include "engine/registry.h"
#include "platform/process.h"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>

namespace ustudio::effects {

namespace fs = std::filesystem;

namespace {

bool isFile(const fs::path &path)
{
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

std::optional<Json> readJson(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseJson(text);
}

void writeText(const fs::path &path, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out)
            return;
    }
    fs::rename(temp, path, ec);
}

// Probe order: frei0r first (the drop-in's own dependency, and the
// acceptance item), then MLT's own, then the rest.
int familyRank(const std::string &family)
{
    if (family == "frei0r")
        return 0;
    if (family == "mlt")
        return 1;
    return 2;
}

// Progress for the main thread, dropped if the scan is gone by the time
// the main loop gets to it.
struct Sink
{
    std::mutex mutex;
    HealthScan::Progress progress;           // cleared when the scan is destroyed
    HealthScan::RegistryReady registryReady; // likewise
};

} // namespace

// Everything below runs on the scan's own thread, around its own
// GMainContext: forking a child from a process the editor's size blocks
// for 17-27 ms, and parsing the registry's JSON for 200 (measured on the
// main thread, 2026-09-28), so neither may happen on the main loop.
struct HealthScan::Impl
{
    HealthScanOptions options;
    // The callbacks, reached from the scan's thread through
    // engine::MainThreadDispatcher (never g_main_context_invoke(), which runs
    // inline on the caller when the main context is unowned); posts made
    // after the scan is gone are dropped with the sink.
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();

    // The scan thread's own; created and freed here, on the main thread, so
    // the destructor's wakeup never reaches a context the thread has
    // already dropped.
    GMainContext *context = g_main_context_new();
    HealthFile health;
    std::deque<std::string> queue;
    int running = 0;
    GCancellable *cancellable = nullptr;

    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<bool> done{false};
    std::mutex resultsMutex;
    HealthFile results; // a copy for results(), under resultsMutex

    // One child: its process, its deadline, and whether that fired.
    struct Child
    {
        Impl *scan;
        std::string service;
        GSubprocess *process = nullptr;
        GSource *deadline = nullptr;
        bool timedOut = false;
    };
    std::set<Child *> live;

    ~Impl()
    {
        {
            std::lock_guard lock(sink->mutex);
            sink->progress = nullptr;
            sink->registryReady = nullptr;
        }
        stopping = true;
        g_main_context_wakeup(context);
        if (worker.joinable())
            worker.join();
        g_main_context_unref(context);
    }

    void run()
    {
        g_main_context_push_thread_default(context);
        cancellable = g_cancellable_new();
        begin();
        while (!done && !stopping)
            g_main_context_iteration(context, TRUE);
        // Stopped early: the children go with the scan (SIGKILL; a probe
        // ignores the render tool's cancel), and their callbacks run here.
        for (Child *child : live)
            g_subprocess_force_exit(child->process);
        g_cancellable_cancel(cancellable);
        while (!live.empty() || g_main_context_pending(context))
            g_main_context_iteration(context, TRUE);
        g_object_unref(cancellable);
        g_main_context_pop_thread_default(context);
    }

    void post(const std::string &service, const HealthRecord &record, bool finished)
    {
        {
            std::lock_guard lock(resultsMutex);
            results = health;
        }
        engine::MainThreadDispatcher::post(sink, [sink = std::weak_ptr<Sink>(sink), service, record, finished] {
            std::shared_ptr<Sink> alive = sink.lock();
            if (!alive)
                return;
            HealthScan::Progress progress;
            {
                std::lock_guard lock(alive->mutex);
                progress = alive->progress;
            }
            if (progress)
                progress(service, record, finished);
        });
    }

    GSubprocess *spawn(const std::vector<std::string> &args, GError **error)
    {
        GSubprocessLauncher *launcher = g_subprocess_launcher_new(
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
        for (const auto &[name, value] : options.environment)
            g_subprocess_launcher_setenv(launcher, name.c_str(), value.c_str(), TRUE);
        std::vector<const char *> argv;
        argv.push_back(options.renderTool.c_str());
        for (const std::string &arg : args)
            argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv.data(), error);
        g_object_unref(launcher);
        return process;
    }

    void begin()
    {
        if (options.fingerprint.empty())
            options.fingerprint = registryFingerprint(); // reads the plugin directories: here, not on the main loop
        health = loadHealthFile(options.healthFile);
        if (health.fingerprint != options.fingerprint)
            health = HealthFile{options.fingerprint, {}};
        if (!options.onlyServices.empty()) {
            enqueue(options.onlyServices);
            return;
        }
        // The registry: cached for this fingerprint, else from the tool.
        if (std::optional<Json> cached = readJson(options.registryCache))
            if (std::optional<EffectRegistry> registry = EffectRegistry::fromJson(*cached, options.fingerprint)) {
                enqueueFrom(*registry);
                return;
            }
        GError *error = nullptr;
        GSubprocess *process = spawn({"--effects-registry"}, &error);
        if (!process) {
            core::Log::warn(std::string("[effects] no registry from the render tool: ") +
                            (error ? error->message : "it didn't start"));
            g_clear_error(&error);
            finish();
            return;
        }
        g_subprocess_communicate_utf8_async(process, nullptr, cancellable, &Impl::onRegistry, this);
        g_object_unref(process);
    }

    static void onRegistry(GObject *source, GAsyncResult *result, gpointer data)
    {
        char *output = nullptr;
        GError *error = nullptr;
        const gboolean ok =
            g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, nullptr, &error);
        const bool cancelled = !ok && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        g_clear_error(&error);
        auto *self = static_cast<Impl *>(data);
        if (cancelled) {
            g_free(output);
            self->finish();
            return;
        }
        std::optional<Json> json = output ? parseJson(output) : std::nullopt;
        std::optional<EffectRegistry> registry =
            json ? EffectRegistry::fromJson(*json, self->options.fingerprint) : std::nullopt;
        if (registry) {
            writeText(self->options.registryCache, std::string(output));
            self->enqueueFrom(*registry);
        } else {
            core::Log::warn("[effects] the render tool's registry didn't parse (another plugin set, or it failed)");
            self->finish();
        }
        g_free(output);
    }

    void enqueueFrom(const EffectRegistry &registry)
    {
        engine::MainThreadDispatcher::post(
            sink, [sink = std::weak_ptr<Sink>(sink), shared = std::make_shared<const EffectRegistry>(registry)] {
                std::shared_ptr<Sink> alive = sink.lock();
                if (!alive)
                    return;
                HealthScan::RegistryReady ready;
                {
                    std::lock_guard lock(alive->mutex);
                    ready = alive->registryReady;
                }
                if (ready)
                    ready(shared);
            });
        std::vector<const EffectDescriptor *> offered;
        for (const EffectDescriptor &d : registry.descriptors())
            if (!d.hidden && d.unstable.empty())
                offered.push_back(&d);
        // Featured first (the Browser's first page), then by family.
        std::stable_sort(offered.begin(), offered.end(), [](const EffectDescriptor *a, const EffectDescriptor *b) {
            if (a->featured != b->featured)
                return a->featured;
            return familyRank(a->family) < familyRank(b->family);
        });
        std::vector<std::string> services;
        for (const EffectDescriptor *d : offered)
            services.push_back(d->service);
        enqueue(services);
    }

    void enqueue(const std::vector<std::string> &services)
    {
        for (const std::string &service : services)
            if (!health.find(service)) // already probed for this plugin set: resume
                queue.push_back(service);
        core::Log::info("[effects] health scan: " + std::to_string(queue.size()) + " effect(s) to probe, " +
                        std::to_string(health.records.size()) + " known");
        pump();
    }

    void pump()
    {
        while (!stopping && running < options.parallel && !queue.empty()) {
            const std::string service = queue.front();
            queue.pop_front();
            GError *error = nullptr;
            GSubprocess *process = spawn({"--probe-effect", service}, &error);
            if (!process) {
                core::Log::warn("[effects] probe for " + service +
                                " didn't start: " + (error ? error->message : std::string("unknown")));
                g_clear_error(&error);
                continue;
            }
            auto *child = new Child{this, service, process};
            live.insert(child);
            child->deadline = g_timeout_source_new_seconds(static_cast<guint>(options.deadlineSeconds));
            g_source_set_callback(child->deadline, &Impl::onDeadline, child, nullptr);
            g_source_attach(child->deadline, context);
            ++running;
            g_subprocess_communicate_utf8_async(process, nullptr, cancellable, &Impl::onProbe, child);
        }
        if (running == 0 && (queue.empty() || stopping))
            finish();
    }

    static gboolean onDeadline(gpointer data)
    {
        auto *child = static_cast<Child *>(data);
        child->timedOut = true;
        // SIGKILL, not the render tool's own cancel (SIGTERM): a probe
        // inside a plugin never checks for it.
        g_subprocess_force_exit(child->process);
        return G_SOURCE_REMOVE;
    }

    static void onProbe(GObject *source, GAsyncResult *result, gpointer data)
    {
        std::unique_ptr<Child> child(static_cast<Child *>(data));
        Impl *self = child->scan;
        self->live.erase(child.get());
        --self->running;
        char *output = nullptr;
        GError *error = nullptr;
        g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, nullptr, &error);
        g_source_destroy(child->deadline);
        g_source_unref(child->deadline);
        const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        g_clear_error(&error);
        g_object_unref(child->process);
        if (cancelled || self->stopping) {
            g_free(output); // stopped with the scan: no verdict
            return;
        }
        const HealthRecord record = interpretProbe(child->service, output ? output : "", child->timedOut);
        g_free(output);
        self->record(child->service, record);
        self->pump();
    }

    void record(const std::string &service, const HealthRecord &result)
    {
        health.records[service] = result;
        saveHealthFile(options.healthFile, health); // an interrupted scan resumes
        if (!result.usable()) {
            core::Log::warn("[effects] " + service + " quarantined: " + healthStatusName(result.status) + ", " +
                            result.reason);
            std::set<std::string> quarantined;
            for (const auto &[name, r] : health.records)
                if (!r.usable())
                    quarantined.insert(name);
            for (const auto &[name, why] : unstableServices(loadOverlays(effectsDataDir() / "overlays")))
                quarantined.insert(name);
            setQuarantinedServices(std::move(quarantined));
        }
        post(service, result, false);
    }

    void finish()
    {
        if (done)
            return;
        size_t bad = 0;
        for (const auto &[name, r] : health.records)
            bad += r.usable() ? 0 : 1;
        core::Log::info("[effects] health scan " + std::string(stopping ? "stopped" : "done") + ": " +
                        std::to_string(health.records.size()) + " probed, " + std::to_string(bad) + " quarantined");
        post({}, {}, true);
        done = true;
    }
};

HealthScan::HealthScan(HealthScanOptions options, Progress progress, RegistryReady registryReady)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->options = std::move(options);
    m_impl->sink->progress = std::move(progress);
    m_impl->sink->registryReady = std::move(registryReady);
}

HealthScan::~HealthScan() = default;

void HealthScan::start()
{
    if (m_impl->worker.joinable())
        return;
    m_impl->worker = std::thread([impl = m_impl.get()] { impl->run(); });
}

bool HealthScan::finished() const
{
    return m_impl->done;
}

HealthFile HealthScan::results() const
{
    std::lock_guard lock(m_impl->resultsMutex);
    return m_impl->results;
}

std::string renderToolPath()
{
    if (const char *overridden = std::getenv("USTUDIO_RENDER_BIN"); overridden && *overridden)
        return overridden;
    const std::string name = std::string("u-studio-render") + platform::executableSuffix();
    const fs::path self = platform::executablePath();
    if (!self.empty()) {
        // Installed: beside the editor. Build tree: src/app -> src/render.
        for (const fs::path &candidate : {self.parent_path() / name, self.parent_path() / ".." / "render" / name})
            if (isFile(candidate))
                return core::utf8String(candidate);
    }
    char *found = g_find_program_in_path(name.c_str());
    const std::string out = found ? found : "";
    g_free(found);
    return out;
}

void startEditorHealthScan(HealthScan::Progress progress, HealthScan::RegistryReady registryReady)
{
    const std::string tool = renderToolPath();
    if (tool.empty()) {
        core::Log::warn("[effects] no u-studio-render: effects aren't health-checked this session");
        return;
    }
    HealthScanOptions options;
    options.renderTool = tool;
    options.registryCache = effectsCacheDir() / "effects-registry.json";
    options.healthFile = healthFilePath();
    // Left empty: the scan's thread works it out (it reads the plugin
    // directories).
    options.fingerprint.clear();
    // For the process: stopped (its children killed) when the program exits.
    static std::unique_ptr<HealthScan> scan;
    scan = std::make_unique<HealthScan>(std::move(options), std::move(progress), std::move(registryReady));
    scan->start();
}

} // namespace ustudio::effects
