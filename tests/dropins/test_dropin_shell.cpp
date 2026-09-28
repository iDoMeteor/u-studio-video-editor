// IP5 (doc 15): the test drop-in's shell layer against a recording
// ShellHost, built in and as a module. Every host gets its contribution;
// the action, lane, selection and import handler work through them. The
// live window is exercised separately with u-studio-video-editor-testdropin.
// Needs a display for GTK widgets; skips itself without one.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/shell_host.h"
#include "app/test_shell.h"
#include "core/commands/undo_stack.h"
#include "dropins/registry.h"
#include "engine/engine_extension.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace ustudio;

extern "C" const UStudioDropInDescription *ustudio_dropin_testdropin_describe(void);

namespace {

namespace fs = std::filesystem;

class RecordingShell : public app::ShellHost
{
  public:
    RecordingShell()
    {
        video = m_model.addTrack(core::Track::Kind::Video, 0, "V1");
        audio = m_model.addTrack(core::Track::Kind::Audio, 1, "A1");
    }
    // The window keeps what it's given for the process; the fake gives it
    // back.
    ~RecordingShell() override
    {
        for (gpointer widget : pageWidgets)
            g_object_unref(widget);
        for (gpointer widget : previewOverlays)
            g_object_unref(widget);
    }
    RecordingShell(const RecordingShell &) = delete;
    RecordingShell &operator=(const RecordingShell &) = delete;
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
        pages.push_back(page.id);
        pageWidgets.push_back(g_object_ref_sink(page.widget));
    }
    void addActions(const std::vector<app::ActionSpec> &specs, gpointer target) override
    {
        for (const app::ActionSpec &spec : specs)
            actions.push_back({spec, target});
    }
    void addHints(const std::vector<app::HintSpec> &hints) override
    {
        for (const app::HintSpec &hint : hints)
            hintIds.push_back(hint.id);
    }
    void setTooltip(GtkWidget *, const char *hintId) override
    {
        tooltips.push_back(hintId);
    }
    void addPreviewOverlay(GtkWidget *overlay) override
    {
        previewOverlays.push_back(g_object_ref_sink(overlay));
    }
    app::PreviewMapping previewMapping() const override
    {
        return app::mapPreview(640, 360, 1920, 1080, 16.0 / 9.0);
    }
    void redrawPreviewOverlays() override {}
    void addTimelineOverlay(app::timeline::TimelineOverlayProvider *provider) override
    {
        timelineOverlays.push_back(provider);
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
    void addHeaderButton(GtkWidget *button) override
    {
        headerButtons.push_back(button);
    }
    std::vector<GtkWidget *> headerButtons;
    std::string projectFolder() const override
    {
        return folder;
    }
    std::string folder;
    std::vector<core::AssetId> changedOnDisk;

    core::TrackId video, audio;
    app::ShellSelection selection;
    std::string status;
    std::vector<std::string> pages, hintIds, tooltips;
    std::vector<gpointer> pageWidgets, previewOverlays;
    std::vector<app::ContributedAction> actions;
    std::vector<app::timeline::TimelineOverlayProvider *> timelineOverlays;
    std::vector<app::ImportHandler> importHandlers;
    core::UndoStack &undo()
    {
        return m_undo;
    }

  private:
    core::Model m_model = core::Model::createEmpty();
    core::UndoStack m_undo{m_model};
    core::Signal<> m_projectChanged, m_selectionChanged;
};

// The label under the page's title: the selection text.
std::string selectionLabel(gpointer page)
{
    GtkWidget *title = gtk_widget_get_first_child(GTK_WIDGET(page));
    return gtk_label_get_text(GTK_LABEL(gtk_widget_get_next_sibling(title)));
}

fs::path writeTestFile(const std::string &name, const std::string &text)
{
    fs::path path = fs::temp_directory_path() / ("ustudio-shell-" + std::to_string(::getpid()) + "-" + name);
    std::ofstream(path) << text;
    return path;
}

// What every copy of the drop-in contributes, named after it.
void checkContributions(RecordingShell &shell, const std::string &name)
{
    CHECK(shell.pages == std::vector<std::string>{name + ".page"});
    CHECK(shell.hintIds == std::vector<std::string>{name + ".inspector", name + ".hello"});
    CHECK(shell.tooltips == std::vector<std::string>{name + ".inspector"});
    REQUIRE(shell.actions.size() == 1);
    CHECK(std::string(shell.actions[0].spec.name) == name + "-hello");
    CHECK(shell.previewOverlays.size() == 1);
    REQUIRE(shell.timelineOverlays.size() == 1);
    REQUIRE(shell.importHandlers.size() == 1);
    CHECK(shell.importHandlers[0].extensions == std::vector<std::string>{"ustest"});

    // The action says hello through the host it was given.
    shell.actions[0].spec.activated(nullptr, nullptr, shell.actions[0].target);
    CHECK(shell.status == "Hello from " + name);

    // Lanes under video tracks only; a press in one is claimed.
    app::timeline::TimelineOverlayProvider &lanes = *shell.timelineOverlays[0];
    const core::Model &model = shell.model();
    CHECK(lanes.laneHeight(model, model.track(shell.video)) == doctest::Approx(12.0));
    CHECK(lanes.laneHeight(model, model.track(shell.audio)) == doctest::Approx(0.0));
    app::timeline::RowLayout layout{60.0, 14.0, {12.0, 0.0}};
    app::timeline::Viewport viewport;
    CHECK(lanes.pressed(model, viewport, layout, 100.0, 65.0, 1));
    CHECK(shell.status.starts_with(name + " lane: frame "));
    CHECK_FALSE(lanes.pressed(model, viewport, layout, 100.0, 30.0, 1)); // the clips: not the lane's

    // The selection reaches the page when it changes.
    CHECK(selectionLabel(shell.pageWidgets[0]).starts_with("0 clip(s) selected"));
    shell.selection.clips = {core::ClipId{42}};
    shell.selectionChanged().emit();
    CHECK(selectionLabel(shell.pageWidgets[0]).starts_with("1 clip(s) selected"));

    // .ustest imports a colour clip through the undo stack, one after another.
    fs::path green = writeTestFile(name + "-green.ustest", "#00aa55\n");
    auto first = shell.importHandlers[0].import(green.string(), shell.video, std::nullopt);
    REQUIRE(first.has_value());
    CHECK(*first == std::optional<core::FrameIndex>(75));
    auto second = shell.importHandlers[0].import(green.string(), shell.video, std::nullopt);
    REQUIRE(second.has_value());
    CHECK(*second == std::optional<core::FrameIndex>(150));
    REQUIRE(model.track(shell.video).clips.size() == 2);
    CHECK(model.asset(model.clip(model.track(shell.video).clips[0]).asset).path == "color:#00aa55");
    CHECK(shell.importHandlers[0].import(green.string(), std::nullopt, std::nullopt).has_value()); // bin only
    CHECK(model.track(shell.video).clips.size() == 2);
    CHECK(shell.undo().undo());
    CHECK(shell.undo().undo());
    CHECK(model.track(shell.video).clips.size() == 1);

    fs::path bad = writeTestFile(name + "-bad.ustest", "not a colour\n");
    CHECK_FALSE(shell.importHandlers[0].import(bad.string(), shell.video, std::nullopt).has_value());
    fs::remove(green);
    fs::remove(bad);
}

} // namespace

TEST_CASE("IP5: the built-in test drop-in adds to every shell host")
{
    if (!gtk_init_check()) {
        MESSAGE("no display: skipped");
        return;
    }
    dropins::DropInRegistry registry;
    registry.addBuiltin(ustudio_dropin_testdropin_describe());
    dropins::BasicDropInHost host("editor");
    registry.registerAll(host);
    engine::clearEngineExtensions();
    REQUIRE(host.shellExtensions().size() == 1);

    RecordingShell shell;
    host.shellExtensions()[0](shell);
    checkContributions(shell, "testdropin");
    CHECK(testdropin::shellLog().hellos == 1);
    CHECK(testdropin::shellLog().imports == 3);
}

TEST_CASE("IP5: the test drop-in built as a module does the same")
{
    if (!gtk_init_check()) {
        MESSAGE("no display: skipped");
        return;
    }
    dropins::DropInRegistry registry;
    registry.loadModules({TEST_MODULE_DIR});
    REQUIRE(registry.entries().size() == 1);
    dropins::BasicDropInHost host("editor");
    registry.registerAll(host);
    engine::clearEngineExtensions();
    REQUIRE(host.shellExtensions().size() == 1);

    RecordingShell shell;
    host.shellExtensions()[0](shell);
    checkContributions(shell, "moduledropin");
}
