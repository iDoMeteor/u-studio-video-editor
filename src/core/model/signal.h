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
