#pragma once

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace ustudio::core {

// Minimal synchronous observer list. core/ has no GLib, so this is not
// GSignal -- callers are expected to be on the same thread that emits
// (the model only ever mutates on the main thread; see CLAUDE.md's
// threading rules).
template <class... Args> class Signal
{
  public:
    using Slot = std::function<void(Args...)>;

    Signal() = default;
    // Non-copyable, non-movable (audit C4): a Signal's subscriber list is
    // tied to the object identity of whatever owns it (EngineSync
    // registers against a specific Model instance), not to that owner's
    // data. Model embeds one directly, so without this, Model's
    // compiler-generated copy/move would silently carry (or, for a move,
    // silently drop) EngineSync's subscription along with the project
    // data whenever a Model was copied (a render thread's snapshot,
    // undo-stack property tests) or moved-into (Model::operator=, e.g.
    // "Open Project" replacing the live model's contents). Deleting these
    // forces Model to define its own copy/move that decides explicitly
    // what happens to `changed` instead of inheriting that member-wise
    // behaviour by accident.
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;
    Signal(Signal &&) = delete;
    Signal &operator=(Signal &&) = delete;

    int connect(Slot slot)
    {
        int id = m_nextId++;
        m_slots.emplace_back(id, std::move(slot));
        return id;
    }

    void disconnect(int id)
    {
        m_slots.erase(
            std::remove_if(m_slots.begin(), m_slots.end(), [id](const auto &entry) { return entry.first == id; }),
            m_slots.end());
    }

    void emit(Args... args) const
    {
        for (const auto &entry : m_slots)
            entry.second(args...);
    }

  private:
    int m_nextId = 1;
    std::vector<std::pair<int, Slot>> m_slots;
};

} // namespace ustudio::core
