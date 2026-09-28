#pragma once

// Doc 15 IP5: the shell's hosts for a drop-in's UI. A drop-in's
// registerDropIn() hands the host a ShellExtension (dropins::DropInHost::
// addShellExtension()); the editor window calls it once, with itself as the
// ShellHost, after its own UI is built. The render tool has no shell and
// never calls it.
//
// With nothing registered the shell is exactly what it was: the inspector
// sidebar and its header toggle don't exist until a page is added, no
// preview overlay is stacked on the preview, and every list (actions,
// hints, timeline overlays, import handlers) is empty.
//
// Everything here is main thread only. Widgets and providers handed over
// belong to the window from then on (widgets) or must outlive it
// (providers, action targets): drop-ins keep them for the process.

#include "action_registry.h"
#include "preview_mapping.h"
#include "timeline/timeline_renderer.h"
#include "ui_hints.h"
#include "core/commands/command.h"
#include "core/model/model.h"
#include "core/model/signal.h"

#include <gtk/gtk.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::app {

// What the next edit applies to. Clips in time order; the track is the
// active one (where imports and single-key edits go). Transitions and
// adjustment blocks aren't selectable in the shell yet: the drop-in that
// adds their gestures (effects' FX lane, FX4) fills them.
struct ShellSelection
{
    std::vector<core::ClipId> clips;
    std::optional<core::TrackId> track;
    std::optional<core::TransitionId> transition;
    std::optional<core::AdjustmentBlockId> adjustmentBlock;

    bool operator==(const ShellSelection &) const = default;
};

struct InspectorPage
{
    const char *id;       // unique: "<drop-in>.<page>"
    const char *title;    // the switcher's label: "Effects"
    const char *iconName; // symbolic: "applications-graphics-symbolic"
    GtkWidget *widget;    // the page; the window takes it
};

// Files a drop-in opens itself instead of the media import (titles'
// .ustitle, doc 16). Runs on the main thread when the file is imported, so
// it must be quick: parse, then execute a command.
struct ImportHandler
{
    std::vector<std::string> extensions; // lowercase, no dot: {"ustitle"}
    std::string description;             // the Import dialog's filter name
    // `track` and `position` as for a media import: no track means the
    // project bin only; no position means after the track's last clip. The
    // result is where the next file starts on the track, or an error.
    std::function<std::expected<std::optional<core::FrameIndex>, std::string>(
        const std::string &path, std::optional<core::TrackId> track, std::optional<core::FrameIndex> position)>
        import;
};

class ShellHost
{
  public:
    virtual ~ShellHost() = default;

    // The project, as the shell sees it. Edits go through execute() so they
    // undo like any other (a core::Command); false if it didn't apply.
    virtual const core::Model &model() const = 0;
    virtual bool execute(std::unique_ptr<core::Command> command) = 0;
    // After every edit, undo, redo and project replacement.
    virtual core::Signal<> &projectChanged() = 0;
    // The frame the preview shows.
    virtual core::FrameIndex currentFrame() const = 0;
    virtual void showStatus(const std::string &text) = 0;

    // Selection.
    virtual ShellSelection currentSelection() const = 0;
    // Once per change, from an idle callback after the timeline redraws.
    virtual core::Signal<> &selectionChanged() = 0;

    // Inspector: a collapsible sidebar on the window's right, one switcher
    // page each. The first page creates it (and its header toggle).
    virtual void addInspectorPage(const InspectorPage &page) = 0;

    // Actions ("win.<name>") with default shortcuts, listed in Help under
    // their category (action_registry.h's contributeActions()). A shortcut
    // without Ctrl, Alt or Super (Shift+T) is off while a text field has
    // focus, like the editor's own single keys.
    virtual void addActions(const std::vector<ActionSpec> &specs, gpointer target) = 0;
    // Tooltips and Help's Controls tab (ui_hints.h's registerHints()), and
    // a widget's tooltip from them. Through the host, like everything
    // here, so a module calls the shell only through the vtable.
    virtual void addHints(const std::vector<HintSpec> &hints) = 0;
    virtual void setTooltip(GtkWidget *widget, const char *hintId) = 0;

    // Preview: `overlay` is stacked over the preview picture, the same
    // size; previewMapping() places the frame inside it. The first overlay
    // creates the stack.
    virtual void addPreviewOverlay(GtkWidget *overlay) = 0;
    virtual PreviewMapping previewMapping() const = 0;
    virtual void redrawPreviewOverlays() = 0;

    // Timeline: painting, lanes and clicks (timeline_renderer.h).
    virtual void addTimelineOverlay(timeline::TimelineOverlayProvider *provider) = 0;
    virtual void redrawTimeline() = 0;

    // Import: files by extension (ImportHandler).
    virtual void addImportHandler(ImportHandler handler) = 0;
    // A file an asset plays changed on disk (a drop-in watching its own
    // files: titles' .ustitle). The asset's fingerprint is updated and the
    // engine rebuilds what plays it, as when missing media is found again.
    // Not an edit: no undo step, and the project isn't marked changed.
    virtual void assetChangedOnDisk(core::AssetId asset) = 0;

    // Where files made for this project go (titles' New Title): the saved
    // project's folder, else Settings › Locations' default project folder
    // when it exists, else "".
    virtual std::string projectFolder() const = 0;
};

} // namespace ustudio::app
