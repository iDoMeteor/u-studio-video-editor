#include "platform/console.h"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace ustudio::platform {

// fd 1 is swapped for /dev/null and back.
ScopedStdoutSilence::ScopedStdoutSilence()
{
    std::fflush(stdout);
    m_saved = ::dup(STDOUT_FILENO);
    m_null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (m_saved >= 0 && m_null >= 0)
        ::dup2(m_null, STDOUT_FILENO);
}

ScopedStdoutSilence::~ScopedStdoutSilence()
{
    std::fflush(stdout);
    if (m_saved >= 0 && m_null >= 0)
        ::dup2(m_saved, STDOUT_FILENO);
    if (m_null >= 0)
        ::close(m_null);
    if (m_saved >= 0)
        ::close(m_saved);
}

} // namespace ustudio::platform
