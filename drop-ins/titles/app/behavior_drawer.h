#pragma once

// The behaviour drawer (doc 16, "Drag a behaviour onto a layer"): every
// behaviour for one slot, each thumbnail animating the selected layer
// itself (your text, your colours), rendered on a worker thread. Click one
// to add it.

#include "core/animation.h"
#include "core/title_document.h"

#include <gtk/gtk.h>

#include <functional>

namespace ustudio::titles::app {

// A popover for `slot` (In, Out or Loop) over `layer` of `doc`, to attach
// to `parent` (a menu button's popover). `pick` gets the chosen one.
GtkWidget *makeBehaviorDrawer(const TitleDocument &doc, const Layer &layer, BehaviorSlot slot,
                              std::function<void(const BehaviorInfo &)> pick);

// The layer alone on a canvas cropped to it, with `behavior` in place of
// the layer's own behaviours for its slot: what a thumbnail animates.
// (Exposed for tests.)
TitleDocument thumbnailDocument(const TitleDocument &doc, const Layer &layer, const Behavior &behavior);
// The moments a thumbnail cycles through: the behaviour's window.
std::vector<double> thumbnailFrames(const Timing &timing, const Behavior &behavior, int count);

} // namespace ustudio::titles::app
