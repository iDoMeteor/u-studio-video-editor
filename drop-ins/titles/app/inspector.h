#pragma once

// The titles app's inspector (doc 16, "Inspector on the right"): the
// selected layer's style and box, or, with nothing selected, the title's
// own settings (size, frame rate, timing, background) and Apply Brand. The
// brand kit's colours, gradients and fonts sit next to every colour and
// font. Rebuilt when the selection changes or the document changes from
// elsewhere (undo, the canvas); an edit made here doesn't rebuild it, so
// a field keeps focus while you type.

#include "core/brand_kit.h"
#include "core/title_document.h"

#include <gtk/gtk.h>

#include <functional>
#include <optional>
#include <string>

namespace ustudio::titles::app {

class Inspector
{
  public:
    struct Callbacks
    {
        // Change the layer `id`: one undo step per `mergeKey` run.
        std::function<void(const std::string &label, const std::string &id, const std::function<void(Layer &)> &,
                           const std::string &mergeKey)>
            editLayer;
        // Change the title itself.
        std::function<void(const std::string &label, const std::function<void(TitleDocument &)> &,
                           const std::string &mergeKey)>
            editDocument;
        std::function<void()> applyBrand;
        // After an edit that changes which rows the inspector shows (a fill's
        // kind, a shape, the shadow switch): show it again, later (the
        // widget whose signal is running mustn't be destroyed under it).
        std::function<void()> rebuild;
        // Pick another picture for the image layer `id`.
        std::function<void(const std::string &id)> replaceImage;
    };

    Inspector(Callbacks callbacks, BrandKit kit);
    ~Inspector();
    Inspector(const Inspector &) = delete;
    Inspector &operator=(const Inspector &) = delete;

    GtkWidget *widget() const
    {
        return m_scroller;
    }

    // Shows `selection` in `doc` (or the title, with none).
    void show(const TitleDocument &doc, const std::optional<std::string> &selection);

    const BrandKit &kit() const
    {
        return m_kit;
    }

  private:
    void buildLayer(const Layer &layer);
    void buildDocument(const TitleDocument &doc);
    GtkWidget *fillGroup(const char *title, const Fill &fill, bool allowNone,
                         std::function<void(const std::function<void(Fill &)> &, const std::string &)> edit);

    void rebuildLater();

    Callbacks m_callbacks;
    BrandKit m_kit;
    guint m_rebuildSource = 0;
    GtkWidget *m_scroller = nullptr;
    GtkWidget *m_box = nullptr;

    // --- GTK trampolines -------------------------------------------------
    static gboolean onRebuild(gpointer self);
};

} // namespace ustudio::titles::app
