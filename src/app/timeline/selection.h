#pragma once

#include "core/model/ids.h"

#include <set>

namespace ustudio::core {
class Model;
}

namespace ustudio::app::timeline {

// doc 06's Selection: the clips the next edit applies to. Plain C++; the
// window asks it what's selected and the timeline draws from it.
class Selection
{
  public:
    const std::set<core::ClipId> &clips() const
    {
        return m_clips;
    }
    bool empty() const
    {
        return m_clips.empty();
    }
    bool contains(core::ClipId clip) const
    {
        return m_clips.contains(clip);
    }
    // The one selected clip, or an invalid id when none or several are.
    core::ClipId single() const
    {
        return m_clips.size() == 1 ? *m_clips.begin() : core::ClipId{};
    }

    void clear()
    {
        m_clips.clear();
    }
    void selectOnly(core::ClipId clip)
    {
        m_clips = {clip};
    }
    void add(core::ClipId clip)
    {
        m_clips.insert(clip);
    }
    void toggle(core::ClipId clip)
    {
        if (!m_clips.erase(clip))
            m_clips.insert(clip);
    }
    // Drops clips the model no longer has (after an undo, a delete).
    void prune(const core::Model &model);

  private:
    std::set<core::ClipId> m_clips;
};

} // namespace ustudio::app::timeline
