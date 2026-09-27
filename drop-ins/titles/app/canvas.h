#pragma once

// The titles app's canvas (doc 16, "The titles app UX"): the title drawn
// by titlerender over a backdrop (a checkerboard, a colour, or the editor's
// frame as a PNG), the safe-area guides, and the selected layer's box and
// handles. Click selects, drag moves (snapping to guides, the centre and
// other layers; Alt drags freely), handles resize. Its keys (Delete, the
// arrows, Escape) belong to the canvas alone, so typing in any text field
// never reaches them (audit A1).
//
// The canvas owns no document: the window gives it one to show
// (setDocument) and applies the edits it asks for (the callbacks), through
// its undo history.

#include "render_worker.h"

#include "core/snapping.h"
#include "core/title_document.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::titles::app {

enum class Backdrop
{
    Checkerboard,
    Colour,
    Image,
};

class TitleCanvas
{
  public:
    struct Callbacks
    {
        // The selection changed (nullopt: nothing selected).
        std::function<void(std::optional<std::string>)> selected;
        // Move layer `id` so its box's top-left lands at (x, y) canvas
        // pixels; `final` on the gesture's last update.
        std::function<void(const std::string &id, double x, double y, bool final)> moved;
        // Set layer `id`'s box to `box` (a resize handle).
        std::function<void(const std::string &id, const Rect &box, bool final)> resized;
        std::function<void(const std::string &id)> deleteRequested;
        // Double-click on a text layer.
        std::function<void(const std::string &id)> editText;
    };

    explicit TitleCanvas(Callbacks callbacks);
    ~TitleCanvas();
    TitleCanvas(const TitleCanvas &) = delete;
    TitleCanvas &operator=(const TitleCanvas &) = delete;

    GtkWidget *widget() const
    {
        return m_widget;
    }

    void setDocument(const TitleDocument &doc);
    void setSelection(std::optional<std::string> id);
    const std::optional<std::string> &selection() const
    {
        return m_selection;
    }
    void setBackdrop(Backdrop backdrop);
    void setBackdropColour(const GdkRGBA &colour);
    // Takes a reference; null clears it (back to the checkerboard).
    void setBackdropImage(GdkTexture *texture);
    void setGuidesVisible(bool visible);

    // Where the canvas (title pixels) sits in the widget, and the reverse.
    struct Mapping
    {
        double x = 0.0, y = 0.0, scale = 1.0;
    };
    Mapping mapping() const;

    // Called by the widget class (canvas.cpp).
    void snapshot(GtkSnapshot *snapshot);

  private:
    enum class Handle
    {
        None,
        Move,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    void requestRender();
    void onRendered(RenderResult result, uint64_t generation);
    const LayerGeometry *geometryOf(const std::string &id) const;
    std::optional<std::string> layerAt(double canvasX, double canvasY) const;
    Handle handleAt(double widgetX, double widgetY) const;
    void drawOverlay(cairo_t *cr);

    // Gestures and keys.
    void pressed(int nPress, double x, double y);
    void dragBegin(double x, double y);
    void dragUpdate(double dx, double dy, bool final);
    bool keyPressed(guint keyval, GdkModifierType state);

    Callbacks m_callbacks;
    GtkWidget *m_widget = nullptr;
    TitleDocument m_doc;
    std::vector<LayerGeometry> m_geometry;
    std::optional<std::string> m_selection;
    Backdrop m_backdrop = Backdrop::Checkerboard;
    GdkRGBA m_backdropColour{0.0f, 0.0f, 0.0f, 1.0f};
    GdkTexture *m_backdropImage = nullptr;
    bool m_guides = true;

    RenderWorker m_worker;
    GdkTexture *m_frame = nullptr; // the newest rendered title
    uint64_t m_frameGeneration = 0, m_wantedGeneration = 0;
    int m_requestedWidth = 0, m_requestedHeight = 0;

    // The drag in progress.
    Handle m_dragHandle = Handle::None;
    Rect m_dragStartBox;
    double m_dragStartX = 0.0, m_dragStartY = 0.0;
    std::optional<double> m_snapGuideX, m_snapGuideY;
    bool m_freeDrag = false;
    bool m_dragActive = false; // past the threshold: a click is never an edit

    // --- GTK trampolines -------------------------------------------------
    static void onPressed(GtkGestureClick *, int nPress, double x, double y, gpointer self);
    static void onDragBegin(GtkGestureDrag *, double x, double y, gpointer self);
    static void onDragUpdate(GtkGestureDrag *, double dx, double dy, gpointer self);
    static void onDragEnd(GtkGestureDrag *, double dx, double dy, gpointer self);
    static gboolean onKeyPressed(GtkEventControllerKey *, guint keyval, guint keycode, GdkModifierType state,
                                 gpointer self);
};

} // namespace ustudio::titles::app
