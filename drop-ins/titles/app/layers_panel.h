#pragma once

// The titles app's layers list (doc 16, "Layers list on the left"):
// topmost first, each with an eye (visible) and a lock (can't be picked on
// the canvas). Drag a row to restack it; Raise and Lower do the same from
// the keyboard.

#include "core/title_document.h"

#include <gtk/gtk.h>

#include <functional>
#include <optional>
#include <string>

namespace ustudio::titles::app {

class LayersPanel
{
  public:
    struct Callbacks
    {
        std::function<void(std::optional<std::string>)> selected;
        std::function<void(const std::string &id, bool visible)> setVisible;
        std::function<void(const std::string &id, bool locked)> setLocked;
        // To `index` in the stacking order (0 is the bottom).
        std::function<void(const std::string &id, size_t index)> restack;
        std::function<void(const std::string &id)> remove;
    };

    explicit LayersPanel(Callbacks callbacks);
    ~LayersPanel();
    LayersPanel(const LayersPanel &) = delete;
    LayersPanel &operator=(const LayersPanel &) = delete;

    GtkWidget *widget() const
    {
        return m_root;
    }

    void show(const TitleDocument &doc, const std::optional<std::string> &selection);

  private:
    GtkWidget *makeRow(const Layer &layer, size_t index);
    void restackBy(int delta);

    Callbacks m_callbacks;
    GtkWidget *m_root = nullptr;
    GtkWidget *m_list = nullptr;
    TitleDocument m_doc;
    std::optional<std::string> m_selection;
    bool m_updating = false;

    // --- GTK trampolines -------------------------------------------------
    static void onRowSelected(GtkListBox *, GtkListBoxRow *row, gpointer self);
};

} // namespace ustudio::titles::app
