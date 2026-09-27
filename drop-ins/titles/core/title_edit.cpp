#include "title_edit.h"

#include <algorithm>

namespace ustudio::titles {

namespace {
// Enough for any session; the oldest steps go first.
constexpr size_t kMaxUndo = 500;
} // namespace

std::string uniqueLayerId(const TitleDocument &doc, const std::string &base)
{
    const std::string stem = base.empty() ? "layer" : base;
    if (!layerIndex(doc, stem))
        return stem;
    for (int n = 2;; ++n) {
        const std::string candidate = stem + "-" + std::to_string(n);
        if (!layerIndex(doc, candidate))
            return candidate;
    }
}

std::optional<size_t> layerIndex(const TitleDocument &doc, const std::string &id)
{
    for (size_t i = 0; i < doc.layers.size(); ++i)
        if (doc.layers[i].id == id)
            return i;
    return std::nullopt;
}

Layer makeTextLayer(const TitleDocument &doc, const std::string &text)
{
    Layer layer;
    layer.id = uniqueLayerId(doc, "text");
    layer.kind = LayerKind::Text;
    layer.text = text;
    layer.font.size = std::max(16.0, doc.height / 15.0);
    layer.align = Align::Center;
    // No box: x is the centre the text is aligned on.
    layer.x = doc.width / 2.0;
    layer.y = doc.height / 2.0 - layer.font.size * 0.6;
    return layer;
}

Layer makeShapeLayer(const TitleDocument &doc, ShapeKind shape)
{
    Layer layer;
    layer.id = uniqueLayerId(doc, shape == ShapeKind::Ellipse ? "ellipse"
                                  : shape == ShapeKind::Line  ? "line"
                                                              : "shape");
    layer.kind = LayerKind::Shape;
    layer.shape = shape;
    layer.w = doc.width / 4.0;
    layer.h = shape == ShapeKind::Line ? 0.0 : doc.height / 6.0;
    layer.x = (doc.width - layer.w) / 2.0;
    layer.y = (doc.height - layer.h) / 2.0;
    if (shape == ShapeKind::RoundedRect)
        layer.radius = layer.h / 8.0;
    if (shape == ShapeKind::Line) {
        layer.stroke.width = 6.0;
        layer.stroke.color = {1.0, 1.0, 1.0, 1.0};
    }
    return layer;
}

bool TitleHistory::apply(const std::string &label, const std::function<bool(TitleDocument &)> &edit,
                         const std::string &mergeKey)
{
    TitleDocument next = m_doc;
    if (!edit(next) || next == m_doc)
        return false;
    const bool merge = !mergeKey.empty() && mergeKey == m_mergeKey && !m_undo.empty();
    if (!merge) {
        m_undo.push_back({label, m_doc});
        if (m_undo.size() > kMaxUndo)
            m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_mergeKey = mergeKey;
    m_doc = std::move(next);
    return true;
}

std::string TitleHistory::undoLabel() const
{
    return m_undo.empty() ? std::string() : m_undo.back().label;
}

std::string TitleHistory::redoLabel() const
{
    return m_redo.empty() ? std::string() : m_redo.back().label;
}

bool TitleHistory::undo()
{
    if (m_undo.empty())
        return false;
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back({step.label, m_doc});
    m_doc = std::move(step.before);
    m_mergeKey.clear();
    return true;
}

bool TitleHistory::redo()
{
    if (m_redo.empty())
        return false;
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back({step.label, m_doc});
    m_doc = std::move(step.before);
    m_mergeKey.clear();
    return true;
}

void TitleHistory::reset(TitleDocument doc)
{
    m_doc = std::move(doc);
    m_saved = m_doc;
    m_undo.clear();
    m_redo.clear();
    m_mergeKey.clear();
}

bool addLayer(TitleDocument &doc, Layer layer, std::optional<size_t> index)
{
    if (layer.id.empty() || layerIndex(doc, layer.id))
        return false;
    const size_t at = std::min(index.value_or(doc.layers.size()), doc.layers.size());
    doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(at), std::move(layer));
    return true;
}

bool removeLayer(TitleDocument &doc, const std::string &id)
{
    const std::optional<size_t> index = layerIndex(doc, id);
    if (!index)
        return false;
    doc.layers.erase(doc.layers.begin() + static_cast<std::ptrdiff_t>(*index));
    return true;
}

bool moveLayer(TitleDocument &doc, const std::string &id, size_t index)
{
    const std::optional<size_t> from = layerIndex(doc, id);
    if (!from || index >= doc.layers.size())
        return false;
    Layer layer = std::move(doc.layers[*from]);
    doc.layers.erase(doc.layers.begin() + static_cast<std::ptrdiff_t>(*from));
    doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    return true;
}

bool updateLayer(TitleDocument &doc, const std::string &id, const std::function<void(Layer &)> &change)
{
    const std::optional<size_t> index = layerIndex(doc, id);
    if (!index)
        return false;
    change(doc.layers[*index]);
    return true;
}

} // namespace ustudio::titles
