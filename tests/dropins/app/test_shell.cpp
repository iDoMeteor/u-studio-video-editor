#include "test_shell.h"

#include "app/shell_host.h"
#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "tokens.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace ustudio::testdropin {

namespace {

constexpr double kLaneHeight = 12.0;
constexpr core::FrameIndex kImportLength = 75;

GdkRGBA rgba(app::tokens::Rgb c, float alpha = 1.0f)
{
    return GdkRGBA{c.r, c.g, c.b, alpha};
}

void helloActivated(GSimpleAction *, GVariant *, gpointer target)
{
    ++shellLog().hellos;
    static_cast<app::ShellHost *>(target)->showStatus("Hello from " TEST_DROPIN_NAME);
}

// Lanes under the video tracks; a click in one is claimed.
class LaneOverlay : public app::timeline::TimelineOverlayProvider
{
  public:
    explicit LaneOverlay(app::ShellHost &host) : m_host(host) {}

    double laneHeight(const core::Model &, const core::Track &track) const override
    {
        return track.kind == core::Track::Kind::Video ? kLaneHeight : 0.0;
    }
    void paintOverlay(GtkSnapshot *snapshot, const core::Model &model, const app::timeline::Viewport &viewport,
                      const app::timeline::RowLayout &layout, double width, double) const override
    {
        const auto &tracks = model.sequence().tracks;
        for (size_t i = 0; i < tracks.size(); ++i) {
            const int row = static_cast<int>(i);
            if (layout.laneHeight(row) <= 0.0)
                continue;
            GdkRGBA lane = rgba(app::tokens::kBrandViolet, 0.18f);
            graphene_rect_t area =
                GRAPHENE_RECT_INIT(0.0f, static_cast<float>(layout.laneTop(row)), static_cast<float>(width),
                                   static_cast<float>(layout.laneHeight(row)));
            gtk_snapshot_append_color(snapshot, &lane, &area);
            if (m_marked) {
                GdkRGBA tick = rgba(app::tokens::kBrandCyan);
                graphene_rect_t mark = GRAPHENE_RECT_INIT(static_cast<float>(viewport.xForFrame(double(*m_marked))),
                                                          static_cast<float>(layout.laneTop(row)), 2.0f,
                                                          static_cast<float>(layout.laneHeight(row)));
                gtk_snapshot_append_color(snapshot, &tick, &mark);
            }
        }
    }
    bool pressed(const core::Model &, const app::timeline::Viewport &viewport, const app::timeline::RowLayout &layout,
                 double x, double y, int) override
    {
        if (!layout.inLane(y))
            return false;
        ++shellLog().lanePresses;
        m_marked = viewport.frameForX(x);
        m_host.showStatus(TEST_DROPIN_NAME " lane: frame " + std::to_string(*m_marked));
        return true;
    }

  private:
    app::ShellHost &m_host;
    std::optional<core::FrameIndex> m_marked;
};

// An outline around the middle quarter of the frame, wherever the preview
// letterboxes it.
void drawPreviewOutline(GtkDrawingArea *, cairo_t *cr, int, int, gpointer data)
{
    const app::PreviewMapping mapping = static_cast<app::ShellHost *>(data)->previewMapping();
    const double x0 = mapping.widgetX(mapping.frameWidth / 4), y0 = mapping.widgetY(mapping.frameHeight / 4);
    const double x1 = mapping.widgetX(mapping.frameWidth * 3 / 4), y1 = mapping.widgetY(mapping.frameHeight * 3 / 4);
    const app::tokens::Rgb c = app::tokens::kBrandCyan;
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.9);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_stroke(cr);
}

std::string selectionText(const app::ShellSelection &selection)
{
    std::string text = std::to_string(selection.clips.size()) + " clip(s) selected";
    if (selection.track)
        text += "\nactive track id " + std::to_string(selection.track->value);
    return text;
}

// ".ustest": the first line names a colour ("#00aa55"); a 75-frame clip of
// it, after the track's last clip unless a position is given.
std::expected<std::optional<core::FrameIndex>, std::string> importTestFile(app::ShellHost &host,
                                                                           const std::string &path,
                                                                           std::optional<core::TrackId> track,
                                                                           std::optional<core::FrameIndex> position)
{
    std::string colour;
    std::getline(std::ifstream(path) >> std::ws, colour);
    if (colour.empty() || colour.find_first_not_of("#0123456789abcdefABCDEF") != std::string::npos)
        return std::unexpected(path + " doesn't name a colour");
    const core::Model &model = host.model();
    core::Asset asset;
    asset.path = "color:" + colour;
    asset.displayName = std::filesystem::path(path).filename().string();
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = kImportLength;
    asset.info.fps = model.sequence().profile.fps;
    std::vector<std::unique_ptr<core::Command>> steps;
    const core::AssetId assetId{model.project().nextId}; // AddAsset takes the next id (as the media import does)
    steps.push_back(std::make_unique<core::AddAsset>(asset));
    core::FrameIndex at = 0;
    if (track) {
        at = position.value_or(0);
        if (!position)
            for (core::ClipId id : model.track(*track).clips)
                at = std::max(at, model.clip(id).position + model.clip(id).out - model.clip(id).in + 1);
        steps.push_back(std::make_unique<core::InsertClip>(*track, assetId, at, 0, kImportLength - 1));
    }
    if (!host.execute(std::make_unique<core::CompositeCommand>("Import " + asset.displayName, std::move(steps))))
        return std::unexpected("Couldn't import " + path + " there");
    ++shellLog().imports;
    return track ? std::optional<core::FrameIndex>(at + kImportLength) : std::nullopt;
}

} // namespace

ShellLog &shellLog()
{
    static ShellLog log;
    return log;
}

void extendShell(app::ShellHost &host)
{
    host.addHints({
        {TEST_DROPIN_NAME ".inspector", "Test drop-in", "Test inspector page", "Shows the selection (tests only)",
         nullptr, nullptr},
        {TEST_DROPIN_NAME ".hello", "Test drop-in", "Say hello", nullptr, TEST_DROPIN_NAME "-hello", nullptr},
    });
    host.addActions({{TEST_DROPIN_NAME "-hello", "Say Hello", "Test drop-in", {"<Control><Alt>h"}, &helloActivated}},
                    &host);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);
    GtkWidget *title = gtk_label_new(TEST_DROPIN_NAME);
    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *selection = gtk_label_new(selectionText(host.currentSelection()).c_str());
    gtk_widget_set_halign(selection, GTK_ALIGN_START);
    GtkWidget *changes = gtk_label_new("0 project change(s)");
    gtk_widget_set_halign(changes, GTK_ALIGN_START);
    gtk_widget_add_css_class(changes, "dim-label");
    host.setTooltip(title, TEST_DROPIN_NAME ".inspector");
    for (GtkWidget *child : {title, selection, changes})
        gtk_box_append(GTK_BOX(page), child);
    host.addInspectorPage({TEST_DROPIN_NAME ".page", "Test", "applications-science-symbolic", page});
    // The window lives for the process, so its labels outlive these slots.
    host.selectionChanged().connect([&host, selection] {
        ++shellLog().selectionChanges;
        gtk_label_set_text(GTK_LABEL(selection), selectionText(host.currentSelection()).c_str());
    });
    host.projectChanged().connect([changes] {
        const std::string text = std::to_string(++shellLog().projectChanges) + " project change(s)";
        gtk_label_set_text(GTK_LABEL(changes), text.c_str());
    });

    GtkWidget *outline = gtk_drawing_area_new();
    gtk_widget_set_can_target(outline, FALSE); // paints only; the preview keeps its input
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(outline), &drawPreviewOutline, &host, nullptr);
    host.addPreviewOverlay(outline);

    static LaneOverlay lanes(host); // one window per process
    host.addTimelineOverlay(&lanes);

    host.addImportHandler(
        {{"ustest"}, "Test drop-in files", [&host](const std::string &path, auto track, auto position) {
             return importTestFile(host, path, track, position);
         }});
}

} // namespace ustudio::testdropin
