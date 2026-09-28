#pragma once

// The Effect Browser (doc 15, "Effect Browser", FX2): an inspector page
// with a search, sections (Featured, Recent, All, then categories) and a
// grid of tiles, each the current frame rendered through that effect. Hover
// a tile, or move to it with the arrow keys, to audition it on the preview:
// the frame is rendered off the live graph (engine/frame_renderer.h) and
// shown over the preview, so nothing is rebuilt. Enter or a click applies
// it to the selected clip (else the sequence); Escape stops auditioning.
// E opens it.

namespace ustudio::app {
class ShellHost;
}

namespace ustudio::effects {

class Catalog;

// Adds the Browser to `host`'s inspector (main thread; once per window).
void addBrowser(app::ShellHost &host, Catalog &catalog);

} // namespace ustudio::effects
