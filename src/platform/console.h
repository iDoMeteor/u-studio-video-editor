#pragma once

// ADR-017: see process.h.

namespace ustudio::platform {

// While alive, the process's stdout goes nowhere; before and after, its
// buffer is flushed, so nothing written earlier is lost and nothing
// written during it leaks out later. For libraries that print to stdout
// what they also return (MLT's avformat encoder list). Not thread-safe
// with other writers to stdout; the app writes none (logs go to stderr).
class ScopedStdoutSilence
{
  public:
    ScopedStdoutSilence();
    ~ScopedStdoutSilence();
    ScopedStdoutSilence(const ScopedStdoutSilence &) = delete;
    ScopedStdoutSilence &operator=(const ScopedStdoutSilence &) = delete;

  private:
    int m_saved = -1;
    int m_null = -1;
};

} // namespace ustudio::platform
