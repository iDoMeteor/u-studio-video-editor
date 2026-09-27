#include "title_shell.h"

#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "engine/backdrop.h"
#include "jobs.h"
#include "title_bake.h"
#include "title_launch.h"
#include "title_page.h"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>


namespace ustudio::titles {

namespace {

// How long after the last change event a title is reloaded: an atomic save
// is several events (write, rename), and editors save more than once.
constexpr guint kSettleMs = 150;

// One GFileMonitor per title asset in the project, kept in step with the
// bin on every project change. Main thread only; lives for the process,
// like the window.
class TitleWatcher
{
  public:
    // The window it serves (the editor has one per process; a new host
    // replaces the old, and its watches go).
    void bind(app::ShellHost &host)
    {
        for (auto &[id, watch] : m_watches)
            stop(watch);
        m_watches.clear();
        m_pending.clear();
        if (m_timer)
            g_source_remove(m_timer);
        m_timer = 0;
        m_host = &host;
    }

    void sync()
    {
        if (!m_host)
            return;
        std::map<uint64_t, std::string> wanted;
        for (const core::Asset &asset : m_host->model().project().bin)
            if (isTitleFile(asset.path))
                wanted.emplace(asset.id.value, asset.path);
        for (auto it = m_watches.begin(); it != m_watches.end();) {
            auto want = wanted.find(it->first);
            if (want == wanted.end() || want->second != it->second.path) {
                stop(it->second);
                it = m_watches.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto &[id, path] : wanted) {
            if (m_watches.contains(id))
                continue;
            GFile *file = g_file_new_for_path(path.c_str());
            GFileMonitor *monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, nullptr, nullptr);
            g_object_unref(file);
            if (!monitor)
                continue;
            Watch &watch = m_watches[id];
            watch.path = path;
            watch.monitor = monitor;
            watch.handler = g_signal_connect(monitor, "changed", G_CALLBACK(onChanged), this);
            // Remembered by the monitor, so the callback knows which asset.
            g_object_set_data(G_OBJECT(monitor), "ustudio-asset", GSIZE_TO_POINTER(id));
        }
    }

  private:
    struct Watch
    {
        std::string path;
        GFileMonitor *monitor = nullptr;
        gulong handler = 0;
    };

    static void stop(Watch &watch)
    {
        g_signal_handler_disconnect(watch.monitor, watch.handler);
        g_file_monitor_cancel(watch.monitor);
        g_object_unref(watch.monitor);
    }

    void changed(uint64_t asset)
    {
        m_pending.insert(asset);
        if (m_timer)
            g_source_remove(m_timer);
        m_timer = g_timeout_add(kSettleMs, &onSettled, this);
    }

    void settled()
    {
        m_timer = 0;
        const std::set<uint64_t> pending = std::move(m_pending);
        m_pending.clear();
        for (uint64_t id : pending)
            if (m_host)
                m_host->assetChangedOnDisk(core::AssetId{id});
    }

    app::ShellHost *m_host = nullptr;
    std::map<uint64_t, Watch> m_watches;
    std::set<uint64_t> m_pending;
    guint m_timer = 0;

    // --- GLib trampolines --------------------------------------------------
    static void onChanged(GFileMonitor *monitor, GFile *, GFile *, GFileMonitorEvent event, gpointer self)
    {
        if (event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED || event == G_FILE_MONITOR_EVENT_PRE_UNMOUNT ||
            event == G_FILE_MONITOR_EVENT_UNMOUNTED)
            return;
        const auto asset =
            static_cast<uint64_t>(GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(monitor), "ustudio-asset")));
        static_cast<TitleWatcher *>(self)->changed(asset);
    }
    static gboolean onSettled(gpointer self)
    {
        static_cast<TitleWatcher *>(self)->settled();
        return G_SOURCE_REMOVE;
    }
};

void launch(app::ShellHost &host, const std::string &path, const std::string &name, GdkTexture *backdrop)
{
    const std::string error = launchTitlesApp(path, backdrop);
    host.showStatus(error.empty() ? "Editing " + name + " in U Stu Titles: save it there to update it here."
                                  : "Couldn't open U Stu Titles: " + error);
}

// Opens a title clip in U Stu Titles, over the editor's frame at the
// playhead (within the clip) without the title itself.
void editTitleClip(app::ShellHost &host, core::ClipId id)
{
    const core::Model &model = host.model();
    if (!model.hasClip(id))
        return;
    const core::Clip &clip = model.clip(id);
    if (!model.hasAsset(clip.asset) || !isTitleFile(model.asset(clip.asset).path))
        return;
    const core::Asset &asset = model.asset(clip.asset);
    const std::string path = asset.path, name = asset.displayName;
    if (titlesLauncherIsForTesting()) {
        launch(host, path, name, nullptr);
        return;
    }
    const core::FrameIndex frame = std::clamp(host.currentFrame(), clip.position, clip.end() - 1);
    std::shared_ptr<const core::Project> project = model.snapshot();
    host.showStatus("Opening " + name + " in U Stu Titles…");
    app::ShellHost *hostPtr = &host; // the window lives for the process
    startJob([project, id, frame, path, name, hostPtr] {
        auto backdrop = std::make_shared<Backdrop>(renderBackdrop(project, id, frame));
        postToMain([backdrop, path, name, hostPtr] {
            GdkTexture *texture = nullptr;
            if (!backdrop->rgba.empty()) {
                GBytes *bytes = g_bytes_new(backdrop->rgba.data(), backdrop->rgba.size());
                texture = gdk_memory_texture_new(backdrop->width, backdrop->height, GDK_MEMORY_R8G8B8A8, bytes,
                                                 static_cast<gsize>(backdrop->width) * 4);
                g_bytes_unref(bytes);
            }
            launch(*hostPtr, path, name, texture);
            if (texture)
                g_object_unref(texture);
        });
    });
}

// Edit Title (the action): the first selected title clip.
void onEditTitle(GSimpleAction *, GVariant *, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    const core::Model &model = host.model();
    for (core::ClipId id : host.currentSelection().clips) {
        if (!model.hasClip(id))
            continue;
        const core::Clip &clip = model.clip(id);
        if (model.hasAsset(clip.asset) && isTitleFile(model.asset(clip.asset).path)) {
            editTitleClip(host, id);
            return;
        }
    }
    host.showStatus("Select a title clip to edit it.");
}

// Bake Title (the action): the first selected title clip.
void onBakeTitle(GSimpleAction *, GVariant *, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    const core::Model &model = host.model();
    for (core::ClipId id : host.currentSelection().clips) {
        if (!model.hasClip(id))
            continue;
        const core::Clip &clip = model.clip(id);
        if (model.hasAsset(clip.asset) && isTitleFile(model.asset(clip.asset).path)) {
            bakeTitleClip(host, id);
            return;
        }
    }
    host.showStatus("Select a title clip to bake it.");
}

// A double-click on a title clip opens it (anywhere but the track's name
// strip, which still renames the track). Paints nothing.
class TitleClipClicks : public app::timeline::TimelineOverlayProvider
{
  public:
    void bind(app::ShellHost &host)
    {
        m_host = &host;
    }

    void paintOverlay(GtkSnapshot *, const core::Model &, const app::timeline::Viewport &,
                      const app::timeline::RowLayout &, double, double) const override
    {}

    bool pressed(const core::Model &model, const app::timeline::Viewport &viewport,
                 const app::timeline::RowLayout &layout, double x, double y, int nPress) override
    {
        if (nPress < 2 || layout.inNameStrip(y))
            return false;
        const int row = layout.rowAt(y);
        const auto &tracks = model.sequence().tracks;
        if (row < 0 || static_cast<size_t>(row) >= tracks.size())
            return false;
        for (core::ClipId id : model.track(tracks[static_cast<size_t>(row)].id).clips) {
            const core::Clip &clip = model.clip(id);
            // Against the clip's edges on screen (xForFrame is the timeline's
            // own mapping).
            if (x < viewport.xForFrame(static_cast<double>(clip.position)) ||
                x >= viewport.xForFrame(static_cast<double>(clip.end())))
                continue;
            if (!model.hasAsset(clip.asset) || !isTitleFile(model.asset(clip.asset).path))
                return false;
            if (m_host)
                editTitleClip(*m_host, id);
            return true;
        }
        return false;
    }

  private:
    app::ShellHost *m_host = nullptr;
};

} // namespace

std::expected<std::optional<core::FrameIndex>, std::string> importTitle(app::ShellHost &host, const std::string &path,
                                                                        std::optional<core::TrackId> track,
                                                                        std::optional<core::FrameIndex> position)
{
    auto read = readTitle(path);
    if (!read)
        return std::unexpected(read.error());
    const TitleDocument &doc = read->document;
    const core::Model &model = host.model();
    const core::Rational fps = model.sequence().profile.fps;

    core::Asset asset;
    asset.path = path;
    asset.displayName = core::utf8String(core::pathFromUtf8(path).filename());
    asset.fileFingerprint = core::fileFingerprint(path);
    asset.info.hasVideo = true;
    asset.info.width = doc.width;
    asset.info.height = doc.height;
    asset.info.fps = {doc.fpsNum, doc.fpsDen};
    asset.info.sar = {1, 1};
    // Boundless, like a still (doc 16): the elastic hold makes any length
    // valid. The still flag is what keeps it boundless once clips are cut
    // from it (InsertClip grows a boundless asset's length otherwise), and
    // keeps titles out of proxies and profile matching.
    asset.info.isStillImage = true;
    asset.info.container = "ustitle";

    // The designed length, in sequence frames.
    const double seconds = static_cast<double>(doc.timing.length()) * doc.fpsDen / doc.fpsNum;
    const auto length =
        std::max<core::FrameIndex>(1, static_cast<core::FrameIndex>(std::lround(seconds * fps.num / fps.den)));

    std::vector<std::unique_ptr<core::Command>> steps;
    const core::AssetId assetId{model.project().nextId}; // AddAsset takes the next id, as the media import does
    steps.push_back(std::make_unique<core::AddAsset>(asset));
    core::FrameIndex at = 0;
    if (track) {
        at = position.value_or(0);
        if (!position)
            for (core::ClipId id : model.track(*track).clips)
                at = std::max(at, model.clip(id).end());
        steps.push_back(std::make_unique<core::InsertClip>(*track, assetId, at, 0, length - 1));
    }
    if (!host.execute(std::make_unique<core::CompositeCommand>("Import " + asset.displayName, std::move(steps))))
        return std::unexpected("Couldn't import " + asset.displayName + " there");
    for (const std::string &warning : read->warnings)
        host.showStatus(asset.displayName + ": " + warning);
    return track ? std::optional<core::FrameIndex>(at + length) : std::nullopt;
}

void extendShell(app::ShellHost &host)
{
    host.addImportHandler({{"ustitle"}, "Titles", [&host](const std::string &path, auto track, auto position) {
                               return importTitle(host, path, track, position);
                           }});
    host.addActions({{"titles-edit", "Edit Title", "Titles", {"<Control><Shift>t"}, &onEditTitle},
                     {"titles-bake", "Bake Title", "Titles", {}, &onBakeTitle}},
                    &host);
    host.addHints({{"titles.edit", "Titles", "Edit title", "Open the selected title clip in U Stu Titles",
                    "titles-edit", "Double-click a title clip"},
                   {"titles.bake", "Titles", "Bake title",
                    "Render the clip to a video file with transparency, for tools without U Stu's titles; undo "
                    "brings the live title back",
                    "titles-bake", nullptr}});
    // For the process, as the host requires; bound to this window.
    static TitleClipClicks clicks;
    clicks.bind(host);
    host.addTimelineOverlay(&clicks);
    static TitleWatcher watcher;
    watcher.bind(host);
    watcher.sync();
    host.projectChanged().connect([] { watcher.sync(); });
    addTitlePage(host);
}

} // namespace ustudio::titles
