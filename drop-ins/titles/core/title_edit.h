#pragma once

// Editing a title in the titles app (doc 16, T2): the document operations
// and their undo history. A title is a few kilobytes, so undo keeps whole
// documents rather than inverse commands: simple, and exact by
// construction. Pure data; no GTK.

#include "title_document.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::titles {

// A layer id not yet in `doc`: `base`, or `base-2`, `base-3`, ...
std::string uniqueLayerId(const TitleDocument &doc, const std::string &base);
// The index of the layer with `id`, or nullopt.
std::optional<size_t> layerIndex(const TitleDocument &doc, const std::string &id);

// A new text or shape layer with sensible defaults, centred on the canvas.
Layer makeTextLayer(const TitleDocument &doc, const std::string &text);
Layer makeShapeLayer(const TitleDocument &doc, ShapeKind shape);

class TitleHistory
{
  public:
    explicit TitleHistory(TitleDocument doc = {}) : m_doc(std::move(doc)) {}

    const TitleDocument &document() const
    {
        return m_doc;
    }

    // Changes the document through `edit`, which returns false (and must
    // leave the copy it was given as it was) to refuse. One undo step
    // labelled `label`; with a `mergeKey`, an edit with the same key as the
    // previous one joins its step (a drag is one step, not hundreds).
    // Returns whether anything changed.
    bool apply(const std::string &label, const std::function<bool(TitleDocument &)> &edit,
               const std::string &mergeKey = {});
    // Ends merging: the next edit starts a new step even with the same key
    // (a new drag).
    void closeStep()
    {
        m_mergeKey.clear();
    }

    bool canUndo() const
    {
        return !m_undo.empty();
    }
    bool canRedo() const
    {
        return !m_redo.empty();
    }
    // The label of what undo / redo would do ("Move layer").
    std::string undoLabel() const;
    std::string redoLabel() const;
    bool undo();
    bool redo();

    // Since the last save (or load).
    bool isDirty() const
    {
        return m_doc != m_saved;
    }
    void markSaved()
    {
        m_saved = m_doc;
    }
    // A different document (Open, New): no history, not dirty.
    void reset(TitleDocument doc);

  private:
    struct Step
    {
        std::string label;
        TitleDocument before;
    };
    TitleDocument m_doc;
    TitleDocument m_saved = m_doc;
    std::vector<Step> m_undo;
    std::vector<Step> m_redo;
    std::string m_mergeKey;
};

// Common edits, for TitleHistory::apply().
bool addLayer(TitleDocument &doc, Layer layer, std::optional<size_t> index = std::nullopt);
bool removeLayer(TitleDocument &doc, const std::string &id);
// To `index` in the stacking order (0 is the bottom).
bool moveLayer(TitleDocument &doc, const std::string &id, size_t index);
bool updateLayer(TitleDocument &doc, const std::string &id, const std::function<void(Layer &)> &change);

} // namespace ustudio::titles
