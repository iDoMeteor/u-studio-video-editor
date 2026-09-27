#pragma once

// One u-studio-titles window: one title file (doc 16, "The titles app
// UX"). The canvas shows the document and asks for edits; the window
// applies them through the undo history, saves and opens.

#include "canvas.h"
#include "inspector.h"
#include "layers_panel.h"

#include "core/title_edit.h"

#include <adwaita.h>

#include <memory>
#include <string>

namespace ustudio::titles::app {

class TitlesWindow
{
  public:
    // `path` empty: a new, untitled title. `backdrop` (a PNG path, may be
    // empty) is shown behind the title: the editor's frame when launched
    // from the editor.
    TitlesWindow(GtkApplication *app, const std::string &path, const std::string &backdrop);
    ~TitlesWindow();
    TitlesWindow(const TitlesWindow &) = delete;
    TitlesWindow &operator=(const TitlesWindow &) = delete;

    GtkWindow *window() const
    {
        return GTK_WINDOW(m_window);
    }

  private:
    void buildUi();
    void refresh();
    void selectionChanged(const std::optional<std::string> &id);
    void toast(const std::string &text);
    bool edit(const std::string &label, const std::function<bool(TitleDocument &)> &change,
              const std::string &mergeKey = {});

    void open(const std::string &path);
    void save(const std::string &path);
    void chooseSavePath();
    void chooseOpenPath();
    void chooseBackdropColour();
    void chooseBackdropImage();
    void addLayerOf(Layer layer, const std::string &label);
    bool confirmClose();

    // Canvas requests.
    void moveLayerTo(const std::string &id, double x, double y, bool final);
    void resizeLayer(const std::string &id, const Rect &box, bool final);
    void deleteLayer(const std::string &id);

    AdwApplicationWindow *m_window = nullptr;
    AdwWindowTitle *m_title = nullptr;
    AdwToastOverlay *m_toasts = nullptr;
    std::unique_ptr<TitleCanvas> m_canvas;
    std::unique_ptr<LayersPanel> m_layers;
    std::unique_ptr<Inspector> m_inspector;
    // An edit from the inspector doesn't rebuild it (the field keeps focus).
    bool m_editFromInspector = false;
    TitleHistory m_history;
    std::string m_path;
    bool m_closing = false;

    // --- GTK trampolines -------------------------------------------------
    static gboolean onCloseRequest(GtkWindow *, gpointer self);
    static void onAction(GSimpleAction *action, GVariant *parameter, gpointer self);
    static void onDestroy(GtkWidget *, gpointer self);
};

} // namespace ustudio::titles::app
