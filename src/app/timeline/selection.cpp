#include "selection.h"

#include "core/model/model.h"

namespace ustudio::app::timeline {

void Selection::prune(const core::Model &model)
{
    std::erase_if(m_clips, [&](core::ClipId id) { return !model.hasClip(id); });
}

} // namespace ustudio::app::timeline
