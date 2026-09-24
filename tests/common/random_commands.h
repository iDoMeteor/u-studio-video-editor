#pragma once

// The random command stream behind doc 12's M1 property boxes, shared so
// both run the *same* sequence: tests/core ("random commands -> undo all ->
// equal", 10k iterations) and tests/engine ("EngineSync::verify() never
// fails across the property test"). Header-only, test-only.

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace ustudio::testing {

// Runs `iterations` random insert/remove/move/split commands on `track`
// through `undoStack`, calling `afterEach(i)` after every command that was
// actually executed. Returns how many were executed (a refused command is
// not pushed, so this is exactly how many undos unwind the whole run).
inline int runRandomCommands(core::Model &model, core::UndoStack &undoStack, core::TrackId track, core::AssetId asset,
                             uint32_t seed, int iterations, const std::function<void(int)> &afterEach = {})
{
    using namespace ustudio::core;
    std::mt19937 rng(seed);
    std::vector<ClipId> liveClips;
    FrameIndex nextFreePosition = 0;
    int appliedCount = 0;

    for (int i = 0; i < iterations; ++i) {
        std::uniform_int_distribution<int> pickAction(0, liveClips.empty() ? 0 : 3);
        int action = pickAction(rng);
        bool executed = false;

        if (action == 0 || liveClips.empty()) {
            FrameIndex length = 10 + static_cast<FrameIndex>(rng() % 90);
            auto cmd = std::make_unique<InsertClip>(track, asset, nextFreePosition, 0, length - 1);
            InsertClip *raw = cmd.get();
            executed = undoStack.execute(std::move(cmd));
            if (executed) {
                nextFreePosition += length;
                liveClips.push_back(raw->clipId());
            }
        } else if (action == 1) {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            size_t index = pickClip(rng);
            executed = undoStack.execute(std::make_unique<RemoveClip>(liveClips[index]));
            if (executed)
                liveClips.erase(liveClips.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (action == 2) {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            ClipId clip = liveClips[pickClip(rng)];
            FrameIndex length = model.clip(clip).length();
            executed = undoStack.execute(std::make_unique<MoveClip>(clip, track, nextFreePosition));
            if (executed)
                nextFreePosition += length;
        } else {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            ClipId clip = liveClips[pickClip(rng)];
            const Clip &current = model.clip(clip);
            if (current.length() > 2) {
                FrameIndex at = current.position + 1 + static_cast<FrameIndex>(rng() % (current.length() - 2));
                auto cmd = std::make_unique<SplitClip>(clip, at);
                SplitClip *raw = cmd.get();
                executed = undoStack.execute(std::move(cmd));
                if (executed)
                    liveClips.push_back(raw->rightId());
            }
        }

        if (executed) {
            ++appliedCount;
            if (afterEach)
                afterEach(i);
        }
    }
    return appliedCount;
}

} // namespace ustudio::testing
