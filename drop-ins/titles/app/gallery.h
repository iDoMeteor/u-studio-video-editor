#pragma once

// The template gallery (doc 16, T4.2): the built-in templates by category,
// then My Templates, each a live thumbnail of the template mid-hold,
// rendered on a worker thread. Click one to use it. My Templates can be
// renamed, duplicated, edited and deleted; a built-in is edited by making a
// copy in My Templates, so the built-in never changes.

#include "core/template_library.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace ustudio::titles::app {

// Where the templates are: the built-ins installed next to the program
// (share/u-studio/titles/templates), else this build's source; and the
// user's library under the data directory (ustudio/titles/templates).
std::string builtInTemplatesDir();
std::string userTemplatesDir();

struct GalleryCallbacks
{
    std::function<void(const TemplateInfo &)> use;  // a template was picked
    std::function<void(const TemplateInfo &)> edit; // open this template file to change it
    std::function<void(const std::string &)> toast; // something to tell the user
};

// Asks for a template's name (Save as Template, Rename): `done` gets it,
// unless the user cancels or leaves it empty.
void askTemplateName(GtkWidget *parent, const std::string &heading, const std::string &initial,
                     std::function<void(const std::string &)> done);

// Shows the gallery over `parent`.
void showGallery(GtkWidget *parent, GalleryCallbacks callbacks);

} // namespace ustudio::titles::app
