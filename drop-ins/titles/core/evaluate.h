#pragma once

// A title at a moment: elastic timing (doc 16, "Elastic timing"), keyframe
// evaluation and field substitution. Pure functions of the document, so
// they're tested without Pango or a display.

#include "title_document.h"

#include <ctime>
#include <map>
#include <string>

namespace ustudio::titles {

// Where frame `clipFrame` of a clip `clipLength` frames long (both at
// `clipFps`) falls in the title's own timeline, in title frames (at the
// document's fps; fractional when the rates differ).
//
// The intro plays from the clip's start and the outro ends at its end, both
// at their designed speed; the hold stretches or shrinks to fill what's
// between (its keyframes stretch with it). A clip shorter than intro +
// outro plays both, squeezed in proportion, with no hold.
double titleFrame(const TitleDocument &doc, double clipLength, double clipFrame, double clipFps);

// A key's position in the title's own timeline (its zone's start + at).
double keyPosition(const Timing &timing, const TitleKey &key);

// A layer's animated properties at `titleFrame`: its own values and
// keyframes, then its behaviours' offsets and loops (core/animation.h).
struct LayerState
{
    double x = 0.0, y = 0.0, opacity = 1.0, scale = 1.0, rotation = 0.0;
    double blur = 0.0, tracking = 0.0, reveal = 1.0, shift = 0.0, shadowOpacity = 1.0;
    Rgba fill; // a solid fill's colour

    bool operator==(const LayerState &) const = default;
};
struct Expansion;
LayerState evaluateLayer(const Layer &layer, const Timing &timing, double titleFrame);
// With the layer's behaviours already expanded (expandBehaviors()).
LayerState evaluateLayer(const Layer &layer, const Expansion &expansion, const Timing &timing, double titleFrame);

// One property's value in a state (Property::FillR is fill.r, ...).
double &stateSlot(LayerState &state, Property property);
double stateValue(LayerState state, Property property);

// What the dynamic fields read (doc 16), for the frame being drawn.
struct FieldClock
{
    double clipFrame = 0.0;     // frames since the clip's first frame
    double timelineFrame = 0.0; // that frame's place in the sequence
    double fps = 30.0;
    std::tm localTime{}; // when it's drawn
};

// `text` with each {{name}} replaced by `values[name]`, or the field's
// default when the clip sets none. Unknown names stay as written, so a typo
// shows on screen instead of vanishing. The dynamic fields come first,
// from `clock`:
//   {{timecode}}          the sequence timecode, HH:MM:SS:FF
//   {{clip_time}}         time since the clip started, MM:SS (H:MM:SS)
//   {{countdown:mm:ss}}   counts down from mm:ss (or ss, or hh:mm:ss) to
//                         zero and stays there, in the same shape
//   {{date}}, {{date:%d %B %Y}}   today, strftime's format (%Y-%m-%d)
std::string substituteFields(const std::string &text, const std::vector<Field> &fields,
                             const std::map<std::string, std::string> &values, const FieldClock &clock = {});

// Whether `text` has a dynamic field, so what it says depends on the clock.
bool hasDynamicFields(const std::string &text);

} // namespace ustudio::titles
