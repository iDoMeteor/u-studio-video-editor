#pragma once

// The titles drop-in's background work in the editor: a backdrop render,
// a bake. Each job gets its own thread, with its own MLT graph, never the
// main or the engine thread. When the app shuts down, jobsCancelled()
// turns true and every job is joined, before MLT closes.

#include <atomic>
#include <functional>

namespace ustudio::titles {

void startJob(std::function<void()> work);
// Set once the app is shutting down: a long job stops early.
const std::atomic<bool> &jobsCancelled();
// Runs `fn` on the main thread, from the main loop.
void postToMain(std::function<void()> fn);

} // namespace ustudio::titles
