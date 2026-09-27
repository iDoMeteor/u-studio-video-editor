// The titles drop-in in the editor's shell (IP5): importing a .ustitle,
// and the file watch that reloads a title within a second of a save
// (doc 16, T1 acceptance). A fake ShellHost stands in for the window.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "editor/title_launch.h"
#include "core/clip_fields.h"
#include "editor/title_bake.h"
#include "editor/title_shell.h"
#include "engine/factory_policy.h"
#include "engine/title_extension.h"
#include "platform/process.h"

#include <chrono>
#include <filesystem>

using namespace ustudio;
namespace fs = std::filesystem;

namespace {

class FakeShell : public app::ShellHost
{
  public:
    explicit FakeShell(core::Profile profile) : m_model(core::Model::createEmpty(profile))
    {
        video = m_model.addTrack(core::Track::Kind::Video, 0, "V1");
    }
    const core::Model &model() const override
    {
        return m_model;
    }
    bool execute(std::unique_ptr<core::Command> command) override
    {
        if (!m_undo.execute(std::move(command)))
            return false;
        m_projectChanged.emit();
        return true;
    }
    core::Signal<> &projectChanged() override
    {
        return m_projectChanged;
    }
    core::FrameIndex currentFrame() const override
    {
        return 0;
    }
    void showStatus(const std::string &text) override
    {
        status = text;
    }
    app::ShellSelection currentSelection() const override
    {
        return selection;
    }
    core::Signal<> &selectionChanged() override
    {
        return m_selectionChanged;
    }
    void addInspectorPage(const app::InspectorPage &page) override
    {
        pages.push_back(page.widget);
    }
    void addHints(const std::vector<app::HintSpec> &) override {}
    void setTooltip(GtkWidget *, const char *) override {}
    void addPreviewOverlay(GtkWidget *) override {}
    app::PreviewMapping previewMapping() const override
    {
        return {};
    }
    void redrawPreviewOverlays() override {}
    void addTimelineOverlay(app::timeline::TimelineOverlayProvider *provider) override
    {
        overlays.push_back(provider);
    }
    void addActions(const std::vector<app::ActionSpec> &specs, gpointer target) override
    {
        for (const app::ActionSpec &spec : specs)
            actions.push_back({spec, target});
    }
    void redrawTimeline() override {}
    void addImportHandler(app::ImportHandler handler) override
    {
        importHandlers.push_back(std::move(handler));
    }
    void assetChangedOnDisk(core::AssetId asset) override
    {
        changedOnDisk.push_back(asset);
    }

    core::TrackId video;
    std::string status;
    std::vector<app::timeline::TimelineOverlayProvider *> overlays;
    std::vector<std::pair<app::ActionSpec, gpointer>> actions;
    app::ShellSelection selection;
    std::vector<app::ImportHandler> importHandlers;
    std::vector<core::AssetId> changedOnDisk;
    std::vector<GtkWidget *> pages;
    void select(std::vector<core::ClipId> clips)
    {
        selection.clips = std::move(clips);
        m_selectionChanged.emit();
    }
    // As the window does: an undo is a project change.
    void undo()
    {
        m_undo.undo();
        m_projectChanged.emit();
    }

  private:
    core::Model m_model;
    core::UndoStack m_undo{m_model};
    core::Signal<> m_projectChanged, m_selectionChanged;
};

// extendShell() builds the Title page's widgets, which need a display.
bool haveGtk()
{
    if (gtk_init_check())
        return true;
    MESSAGE("no display: skipped");
    return false;
}

// Every GtkEntry under `widget`, in order.
void entriesIn(GtkWidget *widget, std::vector<GtkWidget *> &out)
{
    if (GTK_IS_ENTRY(widget))
        out.push_back(widget);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        entriesIn(child, out);
}

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-titles-shell-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

constexpr const char *kTitle = R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15"/>
  <layer kind="shape" x="0" y="0" w="10" h="10"><fill color="#ffffff"/></layer>
  <frobnicate/>
</ustitle>)";

std::string saveTitle(const std::string &name, const char *colour = "#ffffff")
{
    // Written as is (the reader's round trip would drop <frobnicate>), and
    // renamed into place as the titles app's saveTitle() does.
    std::string xml = kTitle;
    xml.replace(xml.find("#ffffff"), 7, colour);
    const fs::path path = scratch() / name;
    const fs::path part = scratch() / (name + ".part");
    std::FILE *file = std::fopen(part.string().c_str(), "wb");
    REQUIRE(file);
    std::fputs(xml.c_str(), file);
    std::fclose(file);
    fs::rename(part, path);
    return core::utf8String(path);
}

// Runs the main loop until `done` or `limit` passes; how long it took.
template <typename Done> std::chrono::milliseconds spinUntil(Done done, std::chrono::milliseconds limit)
{
    const auto start = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - start < limit)
        if (!g_main_context_iteration(nullptr, FALSE))
            g_usleep(2000);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

} // namespace

TEST_CASE("importing a title: an asset, and a clip of its designed length at the sequence's rate")
{
    core::Profile profile;
    profile.fps = {25, 1};
    FakeShell shell(profile);
    const std::string path = saveTitle("Lower Third.ustitle");
    auto next = titles::importTitle(shell, path, shell.video, std::nullopt);
    REQUIRE(next.has_value());
    const core::Model &model = shell.model();
    REQUIRE(model.project().bin.size() == 1);
    const core::Asset &asset = model.project().bin.front();
    CHECK(asset.displayName == "Lower Third.ustitle");
    CHECK(asset.info.hasVideo);
    CHECK_FALSE(asset.info.hasAudio);
    CHECK(asset.info.isBoundless());
    REQUIRE(model.track(shell.video).clips.size() == 1);
    const core::Clip &clip = model.clip(model.track(shell.video).clips.front());
    CHECK(clip.length() == 78); // 93 frames at 30 fps = 3.1 s = 77.5 at 25, rounded
    CHECK(**next == 78);
    // What T1 doesn't know about is reported, not fatal.
    CHECK(shell.status == "Lower Third.ustitle: <frobnicate> isn't supported yet");
    // One undo step removes both.
    shell.undo();
    CHECK(model.project().bin.empty());
    // Not a title: a clean error.
    const std::string bad = core::utf8String(scratch() / "bad.ustitle");
    {
        std::FILE *f = std::fopen(bad.c_str(), "w");
        std::fputs("hello", f);
        std::fclose(f);
    }
    CHECK_FALSE(titles::importTitle(shell, bad, shell.video, std::nullopt).has_value());
}

TEST_CASE("a saved title is reported changed within a second, once per save")
{
    if (!haveGtk())
        return;
    core::Profile profile;
    FakeShell shell(profile);
    titles::extendShell(shell);
    REQUIRE(shell.importHandlers.size() == 1);
    CHECK(shell.importHandlers[0].extensions == std::vector<std::string>{"ustitle"});
    const std::string path = saveTitle("watched.ustitle");
    REQUIRE(shell.importHandlers[0].import(path, shell.video, std::nullopt).has_value());
    const core::AssetId asset = shell.model().project().bin.front().id;
    spinUntil([] { return false; }, std::chrono::milliseconds(300)); // let the monitor settle

    // An atomic save (a temporary file renamed over it), as the titles app does.
    saveTitle("watched.ustitle", "#19e3ff");
    const auto took = spinUntil([&] { return !shell.changedOnDisk.empty(); }, std::chrono::milliseconds(2000));
    REQUIRE(shell.changedOnDisk.size() == 1);
    CHECK(shell.changedOnDisk[0] == asset);
    CHECK(took < std::chrono::milliseconds(1000));
    spinUntil([] { return false; }, std::chrono::milliseconds(400));
    CHECK(shell.changedOnDisk.size() == 1); // the save's several events settle into one

    // Undoing the import stops the watch.
    shell.undo();
    shell.changedOnDisk.clear();
    saveTitle("watched.ustitle", "#ff3cc7");
    spinUntil([] { return false; }, std::chrono::milliseconds(500));
    CHECK(shell.changedOnDisk.empty());
}

TEST_CASE("Edit Title: the action and a double-click open a title clip, and nothing else")
{
    if (!haveGtk())
        return;
    std::vector<std::string> launched;
    titles::setTitlesLauncherForTesting([&](const std::string &path, GdkTexture *) {
        launched.push_back(path);
        return std::string();
    });
    core::Profile profile;
    FakeShell shell(profile);
    titles::extendShell(shell);
    REQUIRE(shell.overlays.size() == 1);
    REQUIRE(shell.actions.size() == 2); // Edit Title, Bake Title
    CHECK(std::string(shell.actions[1].first.name) == "titles-bake");
    CHECK(std::string(shell.actions[0].first.name) == "titles-edit");
    const std::string path = saveTitle("edit-me.ustitle");
    REQUIRE(titles::importTitle(shell, path, shell.video, 100).has_value()); // frames 100..192
    const core::ClipId clip = shell.model().track(shell.video).clips.front();

    // The action: nothing selected, then the title clip.
    shell.actions[0].first.activated(nullptr, nullptr, shell.actions[0].second);
    CHECK(launched.empty());
    CHECK(shell.status == "Select a title clip to edit it.");
    shell.selection.clips = {clip};
    shell.actions[0].first.activated(nullptr, nullptr, shell.actions[0].second);
    REQUIRE(launched.size() == 1);
    CHECK(launched[0] == path);

    // The double-click: on the clip (under the name strip), not a single
    // click, not beside it.
    const app::timeline::Viewport viewport; // one pixel a frame from x = 0
    app::timeline::RowLayout layout;
    const double x = viewport.xForFrame(150.0), y = layout.rowHeight - 5.0;
    CHECK_FALSE(shell.overlays[0]->pressed(shell.model(), viewport, layout, x, y, 1));
    CHECK(shell.overlays[0]->pressed(shell.model(), viewport, layout, x, y, 2));
    CHECK(launched.size() == 2);
    CHECK_FALSE(shell.overlays[0]->pressed(shell.model(), viewport, layout, viewport.xForFrame(50.0), y, 2));
    CHECK_FALSE(shell.overlays[0]->pressed(shell.model(), viewport, layout, x, 2.0, 2)); // the name strip
    CHECK(launched.size() == 2);
    titles::setTitlesLauncherForTesting(nullptr);
}

TEST_CASE("the designer is found: next to the program, else this build's")
{
    CHECK(titles::titlesAppPath() == TITLES_APP_BUILD_PATH);
}

TEST_CASE("the Title page edits the selected clip's fields, one undo step per entry visit")
{
    if (!haveGtk())
        return;
    core::Profile profile;
    FakeShell shell(profile);
    titles::extendShell(shell);
    REQUIRE(shell.pages.size() == 1);
    GtkWidget *page = shell.pages[0];
    const fs::path file = scratch() / "guest.ustitle";
    {
        std::FILE *f = std::fopen(file.string().c_str(), "w");
        std::fputs(R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="0" hold="60" outro="0"/>
  <field name="name" default="Jay Doe"/><field name="role" default="Host"/>
  <layer kind="text" x="10" y="10" w="500"><text>{{name}}, {{role}}</text></layer>
</ustitle>)",
                   f);
        std::fclose(f);
    }
    REQUIRE(titles::importTitle(shell, core::utf8String(file), shell.video, 0).has_value());
    const core::ClipId clip = shell.model().track(shell.video).clips.front();

    std::vector<GtkWidget *> entries;
    entriesIn(page, entries);
    CHECK(entries.empty()); // nothing selected
    shell.select({clip});
    entriesIn(page, entries);
    REQUIRE(entries.size() == 2);
    CHECK(std::string(gtk_editable_get_text(GTK_EDITABLE(entries[0]))) == "Jay Doe");

    // Typing is one step; the default isn't stored.
    gtk_editable_set_text(GTK_EDITABLE(entries[0]), "A");
    gtk_editable_set_text(GTK_EDITABLE(entries[0]), "Ada");
    auto values = titles::clipFieldValues(shell.model().clip(clip));
    CHECK(values == std::map<std::string, std::string>{{"name", "Ada"}});
    shell.undo();
    CHECK(titles::clipFieldValues(shell.model().clip(clip)).empty());
    CHECK(std::string(gtk_editable_get_text(GTK_EDITABLE(entries[0]))) == "Jay Doe"); // the page follows undo

    // Nothing selected: no entries.
    shell.select({});
    CHECK_FALSE(gtk_widget_get_visible(gtk_widget_get_parent(gtk_widget_get_parent(entries[0]))));
}

TEST_CASE("Bake Title: the clip plays a ProRes file, keeps its place and transform, and undo brings the title back")
{
    if (!haveGtk())
        return;
    static const bool mlt = [] {
        engine::FactoryPaths paths;
        paths.mltModuleDirs.push_back(TITLES_MLT_BUILD_DIR);
        static engine::FactoryPolicy policy(paths);
        engine::registerEngineExtension([] { return titles::makeTitleExtension(); });
        return true;
    }();
    (void)mlt;
    core::Profile profile;
    profile.fps = {25, 1};
    FakeShell shell(profile);
    const fs::path dir = scratch() / "bake";
    fs::create_directories(dir);
    const std::string path = saveTitle("bake/Guest.ustitle");
    REQUIRE(titles::importTitle(shell, path, shell.video, 30).has_value());
    const core::ClipId clip = shell.model().track(shell.video).clips.front();
    REQUIRE(shell.execute(
        std::make_unique<titles::SetClipFields>(clip, std::map<std::string, std::string>{{"name", "Ada"}})));
    core::Transform flipped;
    flipped.flipH = true;
    REQUIRE(shell.execute(std::make_unique<core::SetClipTransform>(clip, flipped)));
    const core::Clip before = shell.model().clip(clip);

    CHECK(core::utf8String(core::pathFromUtf8(titles::bakePath(path)).filename()) == "Guest (baked).mov");
    titles::bakeTitleClip(shell, clip);
    CHECK(shell.status.starts_with("Baking"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (shell.status.starts_with("Baking") && std::chrono::steady_clock::now() < deadline)
        g_main_context_iteration(nullptr, TRUE);
    INFO(shell.status);
    REQUIRE(shell.status.starts_with("Baked"));

    const core::Clip &baked = shell.model().clip(clip);
    const core::Asset &asset = shell.model().asset(baked.asset);
    CHECK(asset.path.ends_with("Guest (baked).mov"));
    CHECK(asset.info.lengthInSequenceFrames >= baked.out + 1);
    CHECK(baked.position == before.position);
    CHECK(baked.in == before.in);
    CHECK(baked.out == before.out);
    CHECK(baked.transform.get().flipH);
    CHECK(baked.sourceParams.empty());
    CHECK(shell.model().check().empty());
    // The name is taken now.
    CHECK(core::utf8String(core::pathFromUtf8(titles::bakePath(path)).filename()) == "Guest (baked 2).mov");

    shell.undo();
    CHECK(shell.model().clip(clip).asset == before.asset);
    CHECK(titles::clipFieldValues(shell.model().clip(clip)).at("name") == "Ada");
}
