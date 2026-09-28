#pragma once

// Template packs in the designer (doc 20, T4b): Open Package… shows what a
// .zip or .tar.gz holds before installing it into My Templates, and asks
// before replacing an installed version; Save as Package… makes one from
// templates in My Templates, their previews rendered here.

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace ustudio::titles::app {

// `done` runs after a pack was installed (to refresh a gallery).
void openPackage(GtkWidget *parent, std::function<void(const std::string &)> toast, std::function<void()> done);
void savePackage(GtkWidget *parent, std::function<void(const std::string &)> toast);

// Whether u-studio-share is installed where this can start it: the
// gallery offers Browse Shared and Publish only then.
bool shareAvailable();
// Browse Shared Templates: starts u-studio-share, the one program that
// reaches the network (ADR-020). Says so when it isn't installed.
void browseShared(std::function<void(const std::string &)> toast);
// Publish…: the installed pack at `folder` as an archive again, handed to
// u-studio-share --publish (which shows what it uploads before sending).
void publishPack(const std::string &folder, std::function<void(const std::string &)> toast);

// A preview for a pack: the template mid-hold, as a PNG (exposed for tests).
std::string previewPng(const std::string &templatePath, int width = 640, int height = 360);

} // namespace ustudio::titles::app
