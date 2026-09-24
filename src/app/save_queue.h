#pragma once

#include "core/concurrency/thread_pool.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ustudio::app {

// doc 19 MT1: project saves and autosaves write on the worker pool, one at
// a time -- two writes to one path would share its ".tmp" file -- and
// report back on the main thread. A write takes a model snapshot by value;
// nothing here touches the live Model or GTK, so
// tests/app/test_save_queue.cpp drives it directly.
//
// Latest wins: while one write runs, at most one Save and one Autosave
// wait behind it. A newer Save replaces the waiting one (both callers hear
// the newer write's result); a newer Autosave replaces the waiting one
// outright. A waiting Save goes before a waiting Autosave.
class SaveQueue
{
  public:
    // Pool thread: writes the file(s); returns an error message, or "" on
    // success.
    using Write = std::function<std::string()>;
    // Main thread, after the write: its error, or "" on success.
    using Done = std::function<void(const std::string &error)>;
    using Post = std::function<void(std::function<void()>)>;

    SaveQueue(core::concurrency::ThreadPool &pool, Post post);
    ~SaveQueue();

    void save(Write write, Done done);
    void autosave(Write write, Done done);

    // A write is running or waiting. Callbacks see it already false for
    // their own write when nothing else is queued.
    bool busy() const;

    // Quitting: waits for the running write, then runs the waiting ones on
    // this thread, calling every Done here instead of via Post. Main thread.
    void finish();

  private:
    struct Op;
    void submit(std::shared_ptr<Op> op);
    void complete(const std::shared_ptr<Op> &op);
    void startNext();

    core::concurrency::ThreadPool &m_pool;
    Post m_post;
    std::shared_ptr<Op> m_running;
    std::shared_ptr<Op> m_pendingSave;
    std::shared_ptr<Op> m_pendingAutosave;
};

} // namespace ustudio::app
