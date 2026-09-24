#include "project_loader.h"

#include "core/trace.h"

#include <memory>

namespace ustudio::app {

ProjectLoader::ProjectLoader(core::concurrency::ThreadPool &pool, Post post, Parse parse)
    : m_pool(pool), m_post(std::move(post)), m_parse(std::move(parse))
{}

void ProjectLoader::load(std::string path, Done done)
{
    cancel();
    uint64_t id = ++m_state->latest;
    m_state->busy = true;
    // The job captures copies, never `this`; its posted closure checks the
    // id on the main thread, so a superseded load's result is dropped even
    // if it finished before being cancelled.
    m_job = m_pool.submit(
        [state = m_state, id, path = std::move(path), parse = m_parse, post = m_post,
         done = std::move(done)](std::stop_token stop) mutable {
            auto result = std::make_shared<Result>([&] {
                core::trace::Scope trace("load: parse");
                return parse(path);
            }());
            if (stop.stop_requested())
                return;
            post([state, id, result, done = std::move(done)] {
                if (id != state->latest)
                    return;
                state->busy = false;
                done(std::move(*result));
            });
        },
        core::concurrency::Priority::Interactive);
}

void ProjectLoader::cancel()
{
    ++m_state->latest;
    m_job.cancel(); // a no-op on an empty handle
    m_state->busy = false;
}

bool ProjectLoader::busy() const
{
    return m_state->busy;
}

} // namespace ustudio::app
