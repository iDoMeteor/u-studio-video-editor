// The titles drop-in in the editor's shell (IP5): importing a .ustitle,
// and the file watch that reloads a title within a second of a save
// (doc 16, T1 acceptance). A fake ShellHost stands in for the window.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/undo_stack.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "editor/title_shell.h"
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
        return {};
    }
    core::Signal<> &selectionChanged() override
    {
        return m_selectionChanged;
    }
    void addInspectorPage(const app::InspectorPage &) override {}
    void addActions(const std::vector<app::ActionSpec> &, gpointer) override {}
    void addHints(const std::vector<app::HintSpec> &) override {}
    void setTooltip(GtkWidget *, const char *) override {}
    void addPreviewOverlay(GtkWidget *) override {}
    app::PreviewMapping previewMapping() const override
    {
        return {};
    }
    void redrawPreviewOverlays() override {}
    void addTimelineOverlay(app::timeline::TimelineOverlayProvider *) override {}
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
    std::vector<app::ImportHandler> importHandlers;
    std::vector<core::AssetId> changedOnDisk;
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
