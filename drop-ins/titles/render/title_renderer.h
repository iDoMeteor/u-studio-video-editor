#pragma once

// titlerender (ADR-012): draws a TitleDocument at a moment into pixels with
// Pango and Cairo. No GTK, no MLT, so the titles app's canvas, the editor's
// preview (through the ustudio_title MLT producer) and export all draw with
// this one function.
//
// Any thread. Pango's default font map isn't safe to share across threads,
// so each thread that renders gets its own (T0, docs/developer/notes/
// titles.md), made on its first render and freed when the thread ends.
//
// The picture depends only on the arguments: text is laid out in canvas
// pixels with hinting off and then scaled, so a half-size preview is the
// full-size frame scaled down, and two renders of the same frame are
// byte-identical.

#include "core/snapping.h"
#include "core/evaluate.h"
#include "core/title_document.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ustudio::titles {

struct RenderedFrame
{
    int width = 0, height = 0;
    // Cairo's ARGB32: premultiplied, one native-endian uint32 per pixel
    // (B, G, R, A in memory on little-endian machines), no row padding.
    std::vector<uint32_t> pixels;
};

struct RenderResult
{
    RenderedFrame frame;
    // Fonts that aren't installed, with what was used instead, each once:
    // "Space Grotesk isn't installed; using DejaVu Sans".
    std::vector<std::string> warnings;
};

// What the dynamic fields read in the titles app, where the title is its
// own clip at the start of its own timeline: `titleFrame`, and now.
FieldClock designerClock(const TitleDocument &doc, double titleFrame);

// `titleFrame` in the title's own timeline (titleFrame() in core/evaluate.h);
// `fields` are the clip's values for the title's {{fields}}; `clock` is
// what its dynamic fields read (designerClock() when null).
RenderResult renderTitle(const TitleDocument &doc, double titleFrame, const std::map<std::string, std::string> &fields,
                         int width, int height, const FieldClock *clock = nullptr);

// Where each layer is at `titleFrame`, in canvas pixels, in stacking order
// (bottom first): its own box (a text layer's is its text's, or its w/h
// box), and the rotation and scale drawn about the box's centre. For the
// titles app's selection handles, hit testing and snapping.
struct LayerGeometry
{
    std::string id;
    Rect box;
    double rotation = 0.0, scale = 1.0;
    bool visible = true;
    bool locked = false;
};
std::vector<LayerGeometry> measureLayers(const TitleDocument &doc, double titleFrame,
                                         const std::map<std::string, std::string> &fields,
                                         const FieldClock *clock = nullptr);

// MLT's rgba: straight (not premultiplied) R, G, B, A bytes. `out` must hold
// width * height * 4 bytes.
void toStraightRgba(const RenderedFrame &frame, uint8_t *out);

// Makes the fonts in `directory` (a title's or project's fonts/ folder)
// available to every later render in this process, on every thread.
// Returns false if there is no such directory or fontconfig refused it.
bool addFontDirectory(const std::string &directory);

} // namespace ustudio::titles
